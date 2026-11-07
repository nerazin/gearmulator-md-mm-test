#include "juceRmlComponent.h"
#import <AppKit/AppKit.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include "juce_core/native/juce_ObjCHelpers_mac.h"

// AppKit exposes subclass hooks in NSGestureRecognizer.h. Its state property
// must be redeclared writable by custom recognizers (there is no separate
// subclass header as there is in UIKit).
@interface NSGestureRecognizer (GearmulatorSubclassState)
@property(readwrite) NSGestureRecognizerState state;
@end

namespace juceRmlUi
{
	// macOS 27/Sidecar direct touches go to gesture recognizers, not NSView
	// mouse callbacks or event monitors. One recognizer tracks all fingers.
	class MacTouchInput final : public NativeTouchInput, private juce::ComponentMovementWatcher
	{
	public:
		explicit MacTouchInput(RmlComponent& component)
			: juce::ComponentMovementWatcher(&component), m_component(component)
		{
			m_identities = [[NSMutableDictionary alloc] init];
			attach();
		}
		~MacTouchInput() override { detach(); [m_identities release]; }

	private:
		static MacTouchInput* owner(id recognizer) { return juce::getIvar<MacTouchInput*>(recognizer, "touchOwner"); }
		struct RecognizerClass final : juce::ObjCClass<NSGestureRecognizer>
		{
			RecognizerClass() : ObjCClass("GearmulatorDirectTouch_")
			{
				// Random runtime names avoid collisions between MD/MM plug-in images.
				addIvar<MacTouchInput*>("touchOwner");
				addProtocol(@protocol(NSGestureRecognizerDelegate));
				addMethod(@selector(gestureRecognizer:shouldAttemptToRecognizeWithEvent:),
					[](id, SEL, NSGestureRecognizer*, NSEvent*) -> BOOL { return NO; }); // mouse stays in JUCE
				addMethod(@selector(gestureRecognizer:shouldReceiveTouch:),
					[](id self, SEL, NSGestureRecognizer*, NSTouch* touch) -> BOOL
					{ auto* p = owner(self); return p && p->accept(touch); });
				addMethod(@selector(touchesBeganWithEvent:), [](id self, SEL, NSEvent* event)
					{ if(auto* p = owner(self)) p->collect(event, NSTouchPhaseBegan, TouchEvent::Phase::Begin); });
				addMethod(@selector(touchesMovedWithEvent:), [](id self, SEL, NSEvent* event)
					{ if(auto* p = owner(self)) p->collect(event, NSTouchPhaseMoved, TouchEvent::Phase::Move); });
				addMethod(@selector(touchesEndedWithEvent:), [](id self, SEL, NSEvent* event)
					{ if(auto* p = owner(self)) p->collect(event, NSTouchPhaseEnded, TouchEvent::Phase::End); });
				addMethod(@selector(touchesCancelledWithEvent:), [](id self, SEL, NSEvent*)
					{ if(auto* p = owner(self)) p->cancel(); });
				addMethod(@selector(mouseCancelledWithEvent:), [](id self, SEL, NSEvent*)
					{ if(auto* p = owner(self)) p->cancel(); });
				addMethod(@selector(canBePreventedByGestureRecognizer:),
					[](id, SEL, NSGestureRecognizer*) -> BOOL { return NO; });
				// Recognise on contact, before AppKit's compatibility tap, scroll or
				// long-press recognizers. Never change a DAW's global touch settings.
				addMethod(@selector(canPreventGestureRecognizer:),
					[](id, SEL, NSGestureRecognizer*) -> BOOL { return YES; });
				addMethod(@selector(deliverPanelTouches:), [](id self, SEL, id)
					{ if(auto* p = owner(self)) p->deliver(); });
				// Required for custom recognizers built with the macOS 27 SDK.
				// Panel routing uses each contact; AppKit callers get the latest one.
				addMethod(@selector(locationInView:), [](id self, SEL, NSView* view) -> NSPoint
					{
						if(auto* p = owner(self)) return [p->m_view convertPoint:p->m_lastLocation toView:view];
						return NSZeroPoint;
					});
				addMethod(@selector(panelTouchWindowInactive:), [](id self, SEL, NSNotification* notification)
					{
						if(auto* p = owner(self))
							if([notification object] == [p->m_view window]
								|| [[notification name] isEqualToString:NSApplicationDidResignActiveNotification]) p->reset();
					});
				addMethod(@selector(reset), [](id self, SEL selector)
					{
						if(auto* p = owner(self)) p->clear();
						ObjCClass::sendSuperclassMessage<void>(self, selector);
					});
				registerClass();
			}
		};

		juce::Point<float> position(NSTouch* touch) const
		{
			const auto point = [touch locationInView:m_view];
			const auto bounds = [m_view bounds];
			auto* peer = m_component.getPeer();
			if(!peer || bounds.size.width <= 0 || bounds.size.height <= 0) return {-1, -1};
			auto& root = peer->getComponent();
			const auto x = (point.x - bounds.origin.x) * root.getWidth() / bounds.size.width;
			const auto y = ([m_view isFlipped] ? point.y - bounds.origin.y : NSMaxY(bounds) - point.y)
				* root.getHeight() / bounds.size.height;
			return m_component.getLocalPoint(&root, juce::Point<float>{static_cast<float>(x), static_cast<float>(y)});
		}
		bool accept(NSTouch* touch)
		{
			if([touch type] != NSTouchTypeDirect || !m_component.isShowing() || !m_component.isEnabled()
				|| m_component.isCurrentlyBlockedByAnotherModalComponent()
				|| !m_component.getLocalBounds().toFloat().contains(position(touch))) return false;
			if(![m_identities objectForKey:[touch identity]])
				[m_identities setObject:@(++m_nextId) forKey:[touch identity]];
			m_lastLocation = [touch locationInView:m_view];
			return true;
		}
		void collect(NSEvent* event, NSTouchPhase phase, TouchEvent::Phase outputPhase)
		{
			for(NSTouch* touch in [event touchesMatchingPhase:phase inView:m_view])
			{
				NSNumber* identity = [m_identities objectForKey:[touch identity]];
				if(!identity) continue; // another panel sharing this peer
				m_lastLocation = [touch locationInView:m_view];
				m_pending.push_back({[identity unsignedLongLongValue], outputPhase, position(touch)});
				if(outputPhase == TouchEvent::Phase::End) [m_identities removeObjectForKey:[touch identity]];
			}
			if(m_pending.empty()) return;
			if([m_identities count] == 0) [m_recognizer setState:NSGestureRecognizerStateEnded];
			else if([m_recognizer state] == NSGestureRecognizerStatePossible) [m_recognizer setState:NSGestureRecognizerStateBegan];
			else [m_recognizer setState:NSGestureRecognizerStateChanged];
		}
		void cancel()
		{
			for(NSNumber* identity in [m_identities allValues])
				m_pending.push_back({[identity unsignedLongLongValue], TouchEvent::Phase::Cancel, {}});
			[m_identities removeAllObjects];
			[m_recognizer setState:[m_recognizer state] == NSGestureRecognizerStatePossible
				? NSGestureRecognizerStateFailed : NSGestureRecognizerStateCancelled];
		}
		void deliver()
		{
			auto pending = std::move(m_pending);
			m_pending.clear();
			m_component.processTouches(std::move(pending));
		}
		void clear()
		{
			m_pending.clear();
			[m_identities removeAllObjects];
			m_component.cancelTouches();
		}
		void reset() { clear(); [m_recognizer setEnabled:NO]; [m_recognizer setEnabled:YES]; }
		void attach()
		{
			auto* peer = m_component.getPeer();
			NSView* view = peer ? static_cast<NSView*>(peer->getNativeHandle()) : nil;
			if(view == m_view) return;
			detach();
			if(!view) return;
			m_view = [view retain];
			static RecognizerClass cls;
			m_recognizer = [cls.createInstance() initWithTarget:nil action:@selector(deliverPanelTouches:)];
			object_setInstanceVariable(m_recognizer, "touchOwner", this);
			[m_recognizer setTarget:m_recognizer];
			[m_recognizer setDelegate:(id<NSGestureRecognizerDelegate>)m_recognizer];
			[m_recognizer setAllowedTouchTypes:NSTouchTypeMaskDirect];
			[m_recognizer setDelaysPrimaryMouseButtonEvents:NO];
			[m_recognizer setDelaysSecondaryMouseButtonEvents:NO];
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
			if(@available(macOS 27.0, *))
			{
				m_oldExclusiveBehavior = [m_view exclusiveGestureBehavior];
				[m_view setExclusiveGestureBehavior:NSViewExclusiveGestureBehaviorNotExclusive];
			}
#endif
			[m_view addGestureRecognizer:m_recognizer];
			for(NSNotificationName name in @[NSWindowDidResignKeyNotification, NSWindowWillCloseNotification,
				NSApplicationDidResignActiveNotification])
				[[NSNotificationCenter defaultCenter] addObserver:m_recognizer selector:@selector(panelTouchWindowInactive:) name:name object:nil];
		}
		void detach()
		{
			clear();
			if(!m_recognizer) return;
			object_setInstanceVariable(m_recognizer, "touchOwner", nullptr);
			[[NSNotificationCenter defaultCenter] removeObserver:m_recognizer];
			[m_view removeGestureRecognizer:m_recognizer];
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
			if(@available(macOS 27.0, *))
				[m_view setExclusiveGestureBehavior:static_cast<NSViewExclusiveGestureBehavior>(m_oldExclusiveBehavior)];
#endif
			[m_recognizer release];
			m_recognizer = nil;
			[m_view release];
			m_view = nil;
		}
		void componentPeerChanged() override { attach(); }
		void componentMovedOrResized(bool, bool) override { reset(); }
		void componentVisibilityChanged() override { if(!m_component.isShowing()) reset(); else attach(); }

		RmlComponent& m_component;
		NSView* m_view = nil;
		NSGestureRecognizer* m_recognizer = nil;
		NSMutableDictionary* m_identities = nil;
		NSPoint m_lastLocation = NSZeroPoint;
		uint64_t m_nextId = uint64_t{1} << 63;
		std::vector<TouchEvent> m_pending;
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 270000
		// Keep the SDK 27 enum inside availability-guarded calls; older systems
		// still load the plug-in and use their existing mouse/keyboard path.
		NSInteger m_oldExclusiveBehavior = 0;
#endif
	};

	std::unique_ptr<NativeTouchInput> createNativeTouchInput(RmlComponent& component)
	{
		return std::make_unique<MacTouchInput>(component);
	}
}
