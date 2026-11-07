#include "juceRmlComponent.h"
#include "rmlMouseInput.h"
#include "RmlUi/Core/Context.h"
#include "RmlUi/Core/Element.h"

namespace juceRmlUi
{
	bool RmlComponent::isTouchModifier(const juce::Point<float> position) const
	{
		if(!m_rmlContext) return false;
		const auto dimensions = m_rmlContext->GetDimensions();
		const Rml::Vector2f p{position.x * dimensions.x / std::max(1, getWidth()),
			position.y * dimensions.y / std::max(1, getHeight())};
		for(auto* e = m_rmlContext->GetElementAtPoint(p); e; e = e->GetParentNode())
			if(e->IsClassSet("touchModifier")) return true;
		return false;
	}

	void RmlComponent::processTouches(std::vector<TouchEvent> events)
	{
		RmlInterfaces::ScopedAccess access(*this);
		// NSTouch sets are unordered. Acquire a simultaneous FUNCTION before
		// its target; otherwise COPY can briefly be interpreted as plain RECORD.
		// Preserve the order between successive phases (including very short taps).
		juce::Component::SafePointer<RmlComponent> alive(this);
		for(auto first = events.begin(); first != events.end();)
		{
			const auto phase = first->phase;
			auto last = std::find_if(first, events.end(), [phase](const TouchEvent& e) { return e.phase != phase; });
			if(phase == TouchEvent::Phase::Begin)
				std::stable_partition(first, last, [this](const TouchEvent& e) { return isTouchModifier(e.position); });
			else if(phase == TouchEvent::Phase::Cancel)
				std::stable_partition(first, last, [this](const TouchEvent& e)
				{
					const auto it = m_touches.find(e.id);
					return it == m_touches.end() || !it->second.modifier;
				});
			while(first != last)
			{
				processTouch(*first++);
				if(!alive) return;
			}
		}
	}
	void RmlComponent::setTouchHandler(TouchHandler handler)
	{
		cancelTouches();
		m_nativeTouchInput.reset();
		m_touchHandler = std::move(handler);
#if JUCE_MAC
		if(m_touchHandler) m_nativeTouchInput = createNativeTouchInput(*this);
#endif
	}

	bool RmlComponent::processJuceTouch(const juce::MouseEvent& event, const TouchEvent::Phase phase)
	{
		if(!m_touchHandler || !event.source.isTouch()) return false;
		// macOS native contact IDs use the upper half of this space.
		processTouch({static_cast<uint64_t>(event.source.getIndex()), phase, event.position});
		return true;
	}

	void RmlComponent::processTouch(const TouchEvent& input)
	{
		if(!m_touchHandler || !m_rmlContext) return;
		RmlInterfaces::ScopedAccess access(*this);
		auto event = input;
		const auto dimensions = m_rmlContext->GetDimensions();
		event.position = {input.position.x * dimensions.x / std::max(1, getWidth()),
			input.position.y * dimensions.y / std::max(1, getHeight())};
		const auto existing = m_touches.find(event.id);
		if(event.phase == TouchEvent::Phase::Begin)
		{
			if(existing != m_touches.end() || !isShowing() || !isEnabled()
				|| isCurrentlyBlockedByAnotherModalComponent()) return;
			grabKeyboardFocus();
			TouchRoute route = TouchRoute::Ignored;
			if(m_touchHandler(event)) route = TouchRoute::Custom;
			else if(!m_mouseTouch && !m_realMouseDown)
			{
				route = TouchRoute::Mouse;
				m_mouseTouch = event.id;
			}
			m_touches.emplace(event.id, TouchCapture{route, isTouchModifier(input.position)});
			if(route == TouchRoute::Mouse) processMouseTouch(event);
		}
		else if(existing != m_touches.end())
		{
			const auto route = existing->second.route;
			if(event.phase == TouchEvent::Phase::End || event.phase == TouchEvent::Phase::Cancel)
			{
				m_touches.erase(existing);
				if(m_mouseTouch == event.id) m_mouseTouch.reset();
			}
			if(route == TouchRoute::Custom) m_touchHandler(event);
			else if(route == TouchRoute::Mouse) processMouseTouch(event);
		}
		enqueueUpdateOnce();
	}

	void RmlComponent::processMouseTouch(const TouchEvent& event)
	{
		const Rml::Vector2i pos{juce::roundToInt(event.position.x), juce::roundToInt(event.position.y)};
		switch(event.phase)
		{
		case TouchEvent::Phase::Begin: mouseInput::processButtonDown(*m_rmlContext, pos, 0, 0); break;
		case TouchEvent::Phase::Move: m_rmlContext->ProcessMouseMove(pos.x, pos.y, 0); break;
		case TouchEvent::Phase::End: mouseInput::processButtonUp(*m_rmlContext, pos, 0, 0); m_rmlContext->ProcessMouseLeave(); break;
		case TouchEvent::Phase::Cancel:
			// Move off before releasing: cancellation cannot click a dialog action.
			mouseInput::processButtonUp(*m_rmlContext, {-1, -1}, 0, 0);
			m_rmlContext->ProcessMouseLeave();
			break;
		}
	}

	void RmlComponent::cancelMouseTouch()
	{
		if(!m_mouseTouch) return;
		RmlInterfaces::ScopedAccess access(*this);
		const auto id = *m_mouseTouch;
		m_mouseTouch.reset();
		m_touches[id] = {TouchRoute::Ignored, false};
		if(m_rmlContext) processMouseTouch({id, TouchEvent::Phase::Cancel, {}});
	}

	void RmlComponent::cancelTouches()
	{
		RmlInterfaces::ScopedAccess access(*this);
		auto touches = std::move(m_touches);
		m_touches.clear();
		m_mouseTouch.reset();
		// Release targets before FUNCTION, regardless of native contact-ID order.
		for(const bool modifier : {false, true})
		for(auto it = touches.rbegin(); it != touches.rend(); ++it)
		{
			const auto [id, capture] = *it;
			if(capture.modifier != modifier) continue;
			const auto route = capture.route;
			const TouchEvent event{id, TouchEvent::Phase::Cancel, {}};
			if(route == TouchRoute::Custom && m_touchHandler) m_touchHandler(event);
			else if(route == TouchRoute::Mouse && m_rmlContext) processMouseTouch(event);
		}
		if(!touches.empty()) enqueueUpdateOnce();
	}
}
