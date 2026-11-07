#include "juceRmlComponent.h"
#include "rmlMouseInput.h"
#include "RmlUi/Core/Context.h"

namespace juceRmlUi
{
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
			m_touches.emplace(event.id, route);
			if(route == TouchRoute::Mouse) processMouseTouch(event);
		}
		else if(existing != m_touches.end())
		{
			const auto route = existing->second;
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
		m_touches[id] = TouchRoute::Ignored;
		if(m_rmlContext) processMouseTouch({id, TouchEvent::Phase::Cancel, {}});
	}

	void RmlComponent::cancelTouches()
	{
		RmlInterfaces::ScopedAccess access(*this);
		auto touches = std::move(m_touches);
		m_touches.clear();
		m_mouseTouch.reset();
		// Release later targets before their earlier modifier (FUNCTION).
		for(auto it = touches.rbegin(); it != touches.rend(); ++it)
		{
			const auto [id, route] = *it;
			const TouchEvent event{id, TouchEvent::Phase::Cancel, {}};
			if(route == TouchRoute::Custom && m_touchHandler) m_touchHandler(event);
			else if(route == TouchRoute::Mouse && m_rmlContext) processMouseTouch(event);
		}
		if(!touches.empty()) enqueueUpdateOnce();
	}
}
