#import <AppKit/AppKit.h>
#include <CoreGraphics/CoreGraphics.h>

namespace mdJucePlugin
{
	int currentPanelNativeKeyCode()
	{
		NSEvent* event = [NSApp currentEvent];
		return event != nil && [event type] == NSEventTypeKeyDown
			? static_cast<int>([event keyCode]) : -1;
	}

	bool isPanelNativeKeyDown(const int _keyCode)
	{
		// Only query keys whose press was received by this editor. JUCE's macOS
		// key cache is cleared on modifier changes, even while a key stays held.
		return CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState,
			static_cast<CGKeyCode>(_keyCode));
	}
}
