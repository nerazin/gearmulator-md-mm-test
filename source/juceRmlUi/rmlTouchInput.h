#pragma once
#include <cstdint>
#include <memory>
#include "juce_gui_basics/juce_gui_basics.h"

namespace juceRmlUi
{
	struct TouchEvent
	{
		enum class Phase { Begin, Move, End, Cancel };
		uint64_t id;
		Phase phase;
		juce::Point<float> position;
	};
	class RmlComponent;
	class NativeTouchInput
	{
	public:
		virtual ~NativeTouchInput() = default;
	};
#if JUCE_MAC
	std::unique_ptr<NativeTouchInput> createNativeTouchInput(RmlComponent& component);
#endif
}
