#include "mdEditor.h"
#include "mdPluginEditorState.h"
#include "mdPluginProcessor.h"
#include "juceRmlUi/juceRmlComponent.h"
#include "juceRmlUi/rmlInterfaces.h"
#include "RmlUi/Core/Context.h"
#include "RmlUi/Core/ElementDocument.h"
#include "synthLib/realtimeInstrumentation.h"
#include "synthLib/device.h"
#include "synthLib/plugin.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace mdJucePlugin
{
	struct EditorIdentityTestAccess
	{
		static void release(Editor& _editor, int _key) { _editor.setPanelKey(_key, false); }
		static void loseFocus(Editor& _editor) { _editor.globalFocusChanged(nullptr); }
		static bool poll(Editor& e, int flags) { return e.releaseInactivePanelKeys(juce::ModifierKeys(flags)); }
		static void queue(Editor& e, md::PanelControl control) { e.queuePanelPulse(control); }
		static void tickQueue(Editor& e) { e.servicePanelQueue(); }
		static void acknowledgeQueuedDown(Editor& e)
		{
			if(!e.m_panelSteps.empty() && e.m_panelSteps.front().press) e.m_panelSteps.pop_front();
		}
		static size_t heldKeys(Editor& _editor) { return _editor.m_keyboardPanelKeys.size(); }
		static std::optional<int> page(Editor& _editor)
		{
			return _editor.getModel() == md::MachineModel::Machinedrum
				? _editor.m_machinedrumDataPageTarget.target() : _editor.m_monomachineDataPageTarget.target();
		}
	};
}

namespace
{
	void require(bool _ok, const char* _message)
	{
		if(!_ok) throw std::runtime_error(_message);
	}

	using Rows = std::array<uint8_t, 7>;
	class KeyboardTestProcessor final : public mdJucePlugin::AudioPluginAudioProcessor
	{
	public:
		using AudioPluginAudioProcessor::AudioPluginAudioProcessor;
		void setTestDevice(std::unique_ptr<synthLib::Device> device)
		{
			// Match Processor::setDeviceType: Plugin deletes the previous device,
			// but Processor retains ownership of the replacement's lifetime.
			getPlugin().setDevice(device.get());
			(void)m_device.release();
			m_device = std::move(device);
		}
	};
	class MidiCaptureDevice final : public synthLib::Device
	{
	public:
		MidiCaptureDevice() : Device({}) {}
		std::vector<std::vector<uint8_t>> messages;
		float getSamplerate() const override { return 44100.0f; }
		bool isValid() const override { return true; }
		uint32_t getChannelCountIn() override { return 0; }
		uint32_t getChannelCountOut() override { return 2; }
#if SYNTHLIB_DEMO_MODE == 0
		bool getState(std::vector<uint8_t>&, synthLib::StateType) override { return false; }
		bool setState(const std::vector<uint8_t>&, synthLib::StateType) override { return false; }
#endif
		bool setDspClockPercent(uint32_t) override { return false; }
		uint32_t getDspClockPercent() const override { return 100; }
		uint64_t getDspClockHz() const override { return 100000000; }
	private:
		void readMidiOut(std::vector<synthLib::SMidiEvent>&) override {}
		void processAudio(const synthLib::TAudioInputs&, const synthLib::TAudioOutputs& outputs, size_t count) override
		{
			for(int channel = 0; channel < 2; ++channel)
				std::fill_n(outputs[channel], count, 0.0f);
		}
		bool sendMidi(const synthLib::SMidiEvent& event, std::vector<synthLib::SMidiEvent>&) override
		{
			if(!event.sysex.empty()) messages.emplace_back(event.sysex.begin(), event.sysex.end());
			return true;
		}
	};
	struct Capture
	{
		synthLib::RealtimeInstrumentation& instrumentation;
		Rows rows{};
		std::vector<md::PanelPacket> drain()
		{
			std::vector<md::PanelPacket> packets;
			synthLib::RealtimeEvent event;
			while(instrumentation.popTimelineEvent(event))
				if(event.kind == synthLib::RealtimeEventKind::PanelInput)
				{
					require(event.command >= 0x20 && event.command <= 0x26,
						"keyboard unexpectedly emitted an encoder command");
					rows[event.command - 0x20] = event.argument;
					packets.push_back({static_cast<uint8_t>(event.command),
						static_cast<uint8_t>(event.argument)});
				}
			return packets;
		}
	};
}

int main()
{
	try
	{
		juce::ScopedJuceInitialiser_GUI gui;
#if defined(MD_KEYBOARD_TEST_MM)
		constexpr auto model = md::MachineModel::Monomachine;
		const std::array<md::PanelPacket, 6> expected{{
			{0x23, 0x80}, {0x24, 0x02}, {0x24, 0x01},
			{0x23, 0x40}, {0x23, 0x10}, {0x23, 0x20}}};
#else
		constexpr auto model = md::MachineModel::Machinedrum;
		const std::array<md::PanelPacket, 6> expected{{
			{0x23, 0x20}, {0x23, 0x80}, {0x23, 0x40},
			{0x24, 0x10}, {0x24, 0x08}, {0x23, 0x10}}};
#endif
		const std::array<int, 6> keys{{juce::KeyPress::leftKey, juce::KeyPress::rightKey,
			juce::KeyPress::downKey, juce::KeyPress::upKey,
			juce::KeyPress::returnKey, juce::KeyPress::backspaceKey}};
		KeyboardTestProcessor processor(model,
			mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig{std::string{}}, false);
		processor.setForceSoftwareRendererForSession(true);
		auto& state = static_cast<mdJucePlugin::PluginEditorState&>(processor.getOrCreateEditorState());
		auto* editor = dynamic_cast<mdJucePlugin::Editor*>(state.getEditor());
		require(editor != nullptr, "editor missing");
		auto* panel = editor->getRmlComponent();
		panel->addToDesktop(0);
		panel->setVisible(true);
		panel->toFront(true);
		// Window mapping/focus can be asynchronous under a desktop window manager.
		// Do not silently skip the modifier path when running without native focus.
		for(int attempt = 0; attempt < 20 && !panel->hasKeyboardFocus(true); ++attempt)
		{
			panel->grabKeyboardFocus();
			if(!panel->hasKeyboardFocus(true)) juce::Thread::sleep(10);
		}
		auto* peer = panel->getPeer();
		require(peer != nullptr, "native peer missing");
		require(panel->hasKeyboardFocus(true), "test panel did not acquire native keyboard focus");
		auto& instrumentation = processor.getPlugin().getRealtimeInstrumentation();
		instrumentation.setEnabled(true);
		instrumentation.reset();
		Capture capture{instrumentation};
		juceRmlUi::RmlInterfaces::ScopedAccess access(*panel);
		auto& context = *panel->getContext();
		context.Update();
		editor->getDocument()->Focus();

		const char* phase = "navigation and mouse chords";
		const auto press = [&](int _key, int _mods = 0)
		{
			if(!peer->handleKeyPress(juce::KeyPress(_key, juce::ModifierKeys(_mods), 0)))
				throw std::runtime_error("panel key " + std::to_string(_key) + " (modifiers "
					+ std::to_string(_mods) + ") was not consumed during " + phase);
		};
		const auto mouse = [&](const char* _id, bool _down, int _mods = 0)
		{
			auto* element = editor->getDocument()->GetElementById(_id);
			require(element != nullptr, "mouse target missing");
			const auto offset = element->GetAbsoluteOffset(Rml::BoxArea::Border);
			const auto size = element->GetBox().GetSize(Rml::BoxArea::Border);
			context.ProcessMouseMove(static_cast<int>(offset.x + size.x * 0.5f),
				static_cast<int>(offset.y + size.y * 0.5f), _mods);
			if(_down) context.ProcessMouseButtonDown(0, _mods);
			else context.ProcessMouseButtonUp(0, _mods);
		};

		// Exercise the native Shift transition before the Rml mouse event. Passing
		// KM_SHIFT only to the mouse helper missed the accidental FUNCTION press.
		phase = "Shift-click trig regression";
		for(int trig = 0; trig < 16; ++trig)
		{
			panel->modifierKeysChanged(juce::ModifierKeys(juce::ModifierKeys::shiftModifier));
			require(capture.drain().empty(), "bare Shift injected FUNCTION before the mouse click");
			const auto id = "trigKey" + std::to_string(trig);
			mouse(id.c_str(), true, Rml::Input::KM_SHIFT);
			mouse(id.c_str(), false, Rml::Input::KM_SHIFT);
			capture.drain();
			Rows held{};
			held[trig / 8] = static_cast<uint8_t>(1u << (trig % 8));
			require(capture.rows == held, "Shift-click trig injected FUNCTION instead of a plain trig hold");
			panel->modifierKeysChanged(juce::ModifierKeys());
			capture.drain();
			require(capture.rows == Rows{}, "releasing Shift left a mouse trig held");
		}
		// The same collision affected transport, navigation, page and MM track
		// buttons. Keyboard arrows and encoder switches must remain plain targets
		// while Shift belongs to a mouse hold (including a multi-trig p-lock).
		phase = "Shift mouse and keyboard combinations";
		struct MouseBinding { const char* id; md::PanelControl control; };
		const MouseBinding mouseBindings[] = {
			{"trigKey0", md::PanelControl::Trigger1},
			{"btRec", md::PanelControl::Record}, {"btPlay", md::PanelControl::Play},
			{"btStop", md::PanelControl::Stop}, {"btUp", md::PanelControl::Up},
#if defined(MD_KEYBOARD_TEST_MM)
			{"btDataNext", md::PanelControl::DataPageForward},
			{"btTrack1", md::PanelControl::Track1}, {"btTrack6", md::PanelControl::Track6},
#else
			{"btSynth", md::PanelControl::SynthesisEffectsRouting},
#endif
		};
		for(const auto& binding : mouseBindings)
		{
			panel->modifierKeysChanged(juce::ModifierKeys(juce::ModifierKeys::shiftModifier));
			mouse(binding.id, true, Rml::Input::KM_SHIFT);
			mouse(binding.id, false, Rml::Input::KM_SHIFT);
			capture.drain();
			Rows held{};
			const auto packet = *md::panelPacket(model, binding.control);
			held[packet.row - 0x20] = packet.mask;
			if(binding.control == md::PanelControl::Trigger1)
			{
				mouse("trigKey15", true, Rml::Input::KM_SHIFT);
				mouse("trigKey15", false, Rml::Input::KM_SHIFT);
				held[1] = 0x80;
			}
			capture.drain();
			require(capture.rows == held, "Shift-click added an unwanted secondary function");
			for(const int key : {juce::KeyPress::rightKey, juce::KeyPress::F1Key})
			{
				press(key, juce::ModifierKeys::shiftModifier);
				capture.drain();
				auto combined = held;
				const auto target = key == juce::KeyPress::rightKey ? expected[1]
					: *md::panelEncoderPressPacket(model, md::PanelEncoder::DataEntryA);
				combined[target.row - 0x20] |= target.mask;
				require(capture.rows == combined, "keyboard target injected FUNCTION into a mouse hold");
				mdJucePlugin::EditorIdentityTestAccess::release(*editor, key);
				capture.drain();
				require(capture.rows == held, "keyboard release disturbed the mouse hold");
			}
			mouse("encA", true, Rml::Input::KM_SHIFT | Rml::Input::KM_ALT);
			capture.drain();
			auto encoderChord = held;
			const auto encoder = *md::panelEncoderPressPacket(model, md::PanelEncoder::DataEntryA);
			encoderChord[encoder.row - 0x20] |= encoder.mask;
			require(capture.rows == encoderChord, "mouse encoder press added FUNCTION to the hold");
			mouse("encA", false, Rml::Input::KM_SHIFT | Rml::Input::KM_ALT);
			capture.drain();
			require(capture.rows == held, "mouse encoder release disturbed the hold");
			panel->modifierKeysChanged(juce::ModifierKeys());
			capture.drain();
			require(capture.rows == Rows{}, "Shift mouse combination left a stuck control");
		}

		// Six exact model-specific packets through the native listener, with and
		// without modifiers. Repeated keydown must not create repeated edges.
		for(int mods : std::array<int, 2>{0,
			juce::ModifierKeys::altModifier | juce::ModifierKeys::leftButtonModifier})
			for(size_t i = 0; i < keys.size(); ++i)
			{
				press(keys[i], mods);
				require(capture.drain() == std::vector<md::PanelPacket>{expected[i]}, "wrong key mapping");
				press(keys[i], mods);
				require(capture.drain().empty(), "OS auto-repeat duplicated a press");
				// Synthetic key events do not alter native key state, so JUCE's
				// key-up callback should observe this key as released.
				require(peer->handleKeyUpOrDown(false), "key-up was not handled");
				require(capture.drain() == std::vector<md::PanelPacket>{{expected[i].row, 0}},
					"key release missing");
			}

		// Mouse-held COPY/CLEAR/PASTE are real FUNCTION chords. Releasing either
		// input must preserve the other, even when it shares a scan row.
		for(const char* label : {"altCopy", "altClear", "altPaste"})
			for(bool mouseFirst : {false, true})
				for(size_t i = 0; i < keys.size(); ++i)
				{
					mouse(label, true);
					capture.drain();
					const auto chord = capture.rows;
					const auto function = *md::panelPacket(model, md::PanelControl::Function);
					require((chord[function.row - 0x20] & function.mask) != 0, "label did not hold FUNCTION");
					press(keys[i]);
					capture.drain();
					auto combined = chord;
					combined[expected[i].row - 0x20] |= expected[i].mask;
					require(capture.rows == combined, "keyboard broke a mouse chord");
					if(mouseFirst)
					{
						mouse(label, false);
						capture.drain();
						Rows held{};
						held[expected[i].row - 0x20] = expected[i].mask;
						require(capture.rows == held, "mouse-up released a held keyboard button");
						mdJucePlugin::EditorIdentityTestAccess::release(*editor, keys[i]);
					}
					else
					{
						mdJucePlugin::EditorIdentityTestAccess::release(*editor, keys[i]);
						capture.drain();
						require(capture.rows == chord, "key-up released a mouse chord");
						mouse(label, false);
					}
					capture.drain();
					require(capture.rows == Rows{}, "chord left a stuck button");
				}

		// Independent owners when mouse and keyboard hold the SAME button.
		for(bool mouseFirst : {false, true})
		{
			mouse("btUp", true);
			press(juce::KeyPress::upKey);
			capture.drain();
			if(mouseFirst) mouse("btUp", false);
			else mdJucePlugin::EditorIdentityTestAccess::release(*editor, juce::KeyPress::upKey);
			capture.drain();
			require(capture.rows[expected[3].row - 0x20] == expected[3].mask, "shared hold released early");
			if(mouseFirst) mdJucePlugin::EditorIdentityTestAccess::release(*editor, juce::KeyPress::upKey);
			else mouse("btUp", false);
			capture.drain();
			require(capture.rows == Rows{}, "shared hold stuck");
		}

		// Shift-latched FUNCTION and multiple simultaneous keyboard buttons.
		press(juce::KeyPress::upKey);
		capture.drain();
		const auto heldBeforeModifiers = capture.rows;
		panel->modifierKeysChanged(juce::ModifierKeys(juce::ModifierKeys::shiftModifier));
		panel->modifierKeysChanged(juce::ModifierKeys());
		capture.drain();
		require(capture.rows == heldBeforeModifiers, "modifier change released a keyboard button");
		mdJucePlugin::EditorIdentityTestAccess::release(*editor, juce::KeyPress::upKey);
		capture.drain();

		mouse("btFunction", true, Rml::Input::KM_SHIFT);
		mouse("btFunction", false, Rml::Input::KM_SHIFT);
		for(int key : keys) press(key, juce::ModifierKeys::shiftModifier);
		capture.drain();
		Rows chord{};
		const auto function = *md::panelPacket(model, md::PanelControl::Function);
		chord[function.row - 0x20] = function.mask;
		for(const auto packet : expected) chord[packet.row - 0x20] |= packet.mask;
		require(capture.rows == chord, "Shift-latched chord or simultaneous keys lost");
		mdJucePlugin::EditorIdentityTestAccess::loseFocus(*editor);
		capture.drain();
		require(capture.rows == Rows{}, "focus loss left buttons held");

		press(juce::KeyPress::upKey);
		capture.drain();
		peer->handleKeyPress(juce::KeyPress(juce::KeyPress::escapeKey));
		capture.drain();
		require(capture.rows == Rows{}, "Escape left a keyboard button held");
		require(!editor->settingsOpened(), "cancelling a hold opened settings");

		const auto release = [&](int key)
		{
			mdJucePlugin::EditorIdentityTestAccess::release(*editor, key);
		};
		const auto checkKey = [&](int key, md::PanelPacket packet, int mods = 0)
		{
			press(key, mods);
			require(capture.drain() == std::vector<md::PanelPacket>{packet}, "wrong performance binding");
			press(key, mods);
			require(capture.drain().empty(), "performance key repeated");
			release(key);
			require(capture.drain() == std::vector<md::PanelPacket>{{packet.row, 0}}, "wrong performance release");
		};
		constexpr char trigKeys[] = "12345678qwertyui";
		phase = "trig bindings";
		for(int i = 0; i < 16; ++i)
			checkKey(trigKeys[i], {static_cast<uint8_t>(0x20 + i / 8), static_cast<uint8_t>(1u << (i % 8))});
		struct ExpectedKey { int key; md::PanelPacket packet; };
#if defined(MD_KEYBOARD_TEST_MM)
		const ExpectedKey performance[] = {
			{'j', {0x24, 0x80}}, {'k', {0x25, 0x01}}, {'l', {0x25, 0x02}}, {' ', {0x25, 0x01}},
			{'o', {0x23, 0x04}}, {'n', {0x22, 0x01}}, {'\\', {0x23, 0x01}}, {'b', {0x24, 0x04}},
			{'z', {0x24, 0x08}}, {'x', {0x24, 0x10}}, {'c', {0x24, 0x20}}, {'v', {0x24, 0x40}},
			{'p', {0x23, 0x08}}, {'m', {0x22, 0x02}}, {'[', {0x25, 0x08}}, {']', {0x25, 0x04}}};
		constexpr uint8_t encoderRow = 0x26;
		constexpr int trackCount = 6, pageCount = 7;
#else
		const ExpectedKey performance[] = {
			{'j', {0x22, 0x02}}, {'k', {0x22, 0x04}}, {'l', {0x22, 0x08}}, {' ', {0x22, 0x04}},
			{'o', {0x22, 0x40}}, {'n', {0x22, 0x80}}, {'\\', {0x22, 0x01}}, {'b', {0x24, 0x04}},
			{'z', {0x23, 0x01}}, {'x', {0x23, 0x02}}, {'c', {0x23, 0x04}}, {'v', {0x23, 0x08}},
			{'p', {0x22, 0x20}}, {'m', {0x24, 0x01}}, {']', {0x22, 0x10}}};
		constexpr uint8_t encoderRow = 0x25;
		constexpr int trackCount = 16, pageCount = 3;
#endif
		phase = "performance bindings";
		for(const auto& binding : performance) checkKey(binding.key, binding.packet);
		for(int i = 0; i < 8; ++i)
			checkKey(juce::KeyPress::F1Key + i, {encoderRow, static_cast<uint8_t>(1u << i)});

		// A physical TEMPO tap has two edges. Only the first tap presses FUNCTION;
		// in particular MM sends both buttons on one row, MD on separate rows.
		phase = "tap tempo";
		const int shift = juce::ModifierKeys::shiftModifier;
		const auto modifiers = [&](int flags) { panel->modifierKeysChanged(juce::ModifierKeys(flags)); };
		Rows functionOnly{};
		functionOnly[function.row - 0x20] = function.mask;
		const auto tempo = *md::panelPacket(model, md::PanelControl::Tempo);
		auto tempoDown = tempo;
		tempoDown.mask |= functionOnly[tempo.row - 0x20];
		modifiers(shift);
		require(capture.drain().empty(), "bare Shift pressed FUNCTION");
		for(int tap = 0; tap < 4; ++tap)
		{
			press(tap % 2 ? '\\' : '|', shift);
			auto edges = capture.drain();
			if(tap == 0)
				require(edges == std::vector<md::PanelPacket>{function, tempoDown}, "first tap must press FUNCTION then TEMPO");
			else
				require(edges == std::vector<md::PanelPacket>{tempoDown}, "tap re-pressed FUNCTION between beats");
			press('|', shift);
			require(capture.drain().empty(), "OS repeat generated an extra tempo tap");
			// Synthetic key-up: native Shift remains down while the target key is up.
			require(mdJucePlugin::EditorIdentityTestAccess::poll(*editor, shift), "TEMPO key-up not recognised");
			require(capture.drain() == std::vector<md::PanelPacket>{{tempo.row, functionOnly[tempo.row - 0x20]}}
				&& capture.rows == functionOnly, "releasing TEMPO also dropped FUNCTION between beats");
		}
		modifiers(0);
		require(capture.drain() == std::vector<md::PanelPacket>{{function.row, 0}}, "releasing Shift did not end tap tempo");

		// The modifier lifetime is shared by every hardware button mapping.
		// Exercise each release order, including keys that share FUNCTION's row.
		phase = "FUNCTION chords";
		std::vector<ExpectedKey> buttons(std::begin(performance), std::end(performance));
		for(size_t i = 0; i < keys.size(); ++i) buttons.push_back({keys[i], expected[i]});
		for(int i = 0; i < 16; ++i)
			buttons.push_back({trigKeys[i], {static_cast<uint8_t>(0x20 + i / 8), static_cast<uint8_t>(1u << (i % 8))}});
		for(int i = 0; i < 8; ++i)
			buttons.push_back({juce::KeyPress::F1Key + i, {encoderRow, static_cast<uint8_t>(1u << i)}});
		for(bool shiftFirst : {false, true})
		for(const auto& binding : buttons)
		{
			modifiers(shift);
			require(capture.drain().empty(), "bare Shift pressed FUNCTION");
			press(binding.key, shift);
			const auto events = capture.drain();
			auto expectedChord = functionOnly;
			const auto packet = binding.packet;
			expectedChord[packet.row - 0x20] |= packet.mask;
			require(events.size() == 2 && events.front() == function && capture.rows == expectedChord,
				"FUNCTION must precede its keyboard target");
			press(binding.key == ']' ? '}' : binding.key, shift);
			require(capture.drain().empty(), "shifted chord auto-repeat duplicated a press");
			if(shiftFirst)
			{
				modifiers(0);
				capture.drain();
				Rows targetOnly{};
				targetOnly[packet.row - 0x20] = packet.mask;
				require(capture.rows == targetOnly, "releasing Shift released the target key");
				release(binding.key);
			}
			else
			{
				release(binding.key);
				capture.drain();
				require(capture.rows == functionOnly, "target release interrupted FUNCTION");
				modifiers(0);
			}
			capture.drain();
			require(capture.rows == Rows{}, "Shift chord stuck");
		}

		// Switching to a Shift-click must release keyboard FUNCTION before the
		// mouse target, and other modifier transitions must not bring it back.
		phase = "keyboard chord to mouse hold";
		panel->modifierKeysChanged(juce::ModifierKeys(juce::ModifierKeys::shiftModifier));
		press('k', juce::ModifierKeys::shiftModifier);
		capture.drain();
		mouse("trigKey0", true, Rml::Input::KM_SHIFT);
		const auto handoff = capture.drain();
		require(!handoff.empty() && handoff.front().row == function.row
			&& (handoff.front().mask & function.mask) == 0, "mouse target preceded FUNCTION release");
		mouse("trigKey0", false, Rml::Input::KM_SHIFT);
		release('k');
		panel->modifierKeysChanged(juce::ModifierKeys(juce::ModifierKeys::shiftModifier | juce::ModifierKeys::altModifier));
		panel->modifierKeysChanged(juce::ModifierKeys(juce::ModifierKeys::shiftModifier));
		press(juce::KeyPress::F1Key, juce::ModifierKeys::shiftModifier);
		capture.drain();
		Rows mouseAndEncoder{};
		mouseAndEncoder[0] = 1;
		mouseAndEncoder[encoderRow - 0x20] = 1;
		require(capture.rows == mouseAndEncoder, "modifier transition reintroduced FUNCTION into a trig hold");
		release(juce::KeyPress::F1Key);
		panel->modifierKeysChanged(juce::ModifierKeys());
		capture.drain();
		require(capture.rows == Rows{}, "keyboard-to-mouse handoff stuck");

		// From the gap between taps, a mouse gesture, host shortcut, Escape or
		// focus loss must not leave FUNCTION held. A missed Shift-up is recoverable.
		using Input = mdJucePlugin::EditorIdentityTestAccess;
		const auto betweenTaps = [&]
		{
			modifiers(shift);
			press('\\', shift);
			release('\\');
			capture.drain();
			require(capture.rows == functionOnly, "test did not reach the gap between taps");
		};
		phase = "ending a held FUNCTION";
		for(const int mouseMods : {0, static_cast<int>(Rml::Input::KM_SHIFT)})
		{
			betweenTaps();
			mouse("trigKey0", true, mouseMods);
			const auto edges = capture.drain();
			require(edges == std::vector<md::PanelPacket>{{function.row, 0}, {0x20, 1}},
				"click after a tap reached the trig before releasing FUNCTION");
			mouse("trigKey0", false, mouseMods);
			modifiers(0);
			capture.drain();
			require(capture.rows == Rows{}, "click after a tap stuck");
		}
		for(const int reserved : {juce::ModifierKeys::ctrlModifier, juce::ModifierKeys::commandModifier})
		{
			betweenTaps();
			peer->handleKeyPress(juce::KeyPress('9', juce::ModifierKeys(shift | reserved), 0));
			capture.drain();
			require(capture.rows == Rows{}, "host shortcut inherited FUNCTION after a missed modifier event");
			modifiers(0);
		}
		for(const int flags : {0, shift | juce::ModifierKeys::ctrlModifier, shift | juce::ModifierKeys::commandModifier})
		{
			betweenTaps();
			require(Input::poll(*editor, flags), "poll missed the end of the modifier hold");
			capture.drain();
			require(capture.rows == Rows{}, "reconciliation left FUNCTION held");
			modifiers(0);
		}
		for(bool escape : {false, true})
		{
			betweenTaps();
			if(escape) press(juce::KeyPress::escapeKey, shift);
			else Input::loseFocus(*editor);
			capture.drain();
			require(capture.rows == Rows{} && !editor->settingsOpened(), "cancelling between taps left a hold or opened settings");
			modifiers(0);
		}

		// Direct page shortcuts must never become FUNCTION+page (MD: LFO).
		// Cover a keyboard or mouse FUNCTION owner and a held page button.
		const auto pageControl = model == md::MachineModel::Machinedrum
			? md::PanelControl::SynthesisEffectsRouting : md::PanelControl::DataPageForward;
		const auto pagePacket = *md::panelPacket(model, pageControl);
		const char* pageButton = model == md::MachineModel::Machinedrum ? "btSynth" : "btDataNext";
		phase = "queued page shortcuts";
		for(int owner = 0; owner < 3; ++owner)
		{
			if(owner == 0) betweenTaps();
			else mouse(owner == 1 ? "btFunction" : pageButton, true);
			capture.drain();
			Input::queue(*editor, pageControl);
			Input::tickQueue(*editor);
			require(capture.drain().empty(), "page shortcut collided with a held panel control");
			if(owner == 0) modifiers(0);
			else mouse(owner == 1 ? "btFunction" : pageButton, false);
			capture.drain();
			Input::tickQueue(*editor);
			require(capture.drain() == std::vector<md::PanelPacket>{pagePacket}, "page shortcut did not resume as a plain press");
			Input::loseFocus(*editor);
			capture.drain();
			Input::tickQueue(*editor);
			require(capture.drain().empty(), "cancelled page pulse resumed after focus loss");
		}
		Input::queue(*editor, pageControl);
		Input::tickQueue(*editor);
		require(capture.drain() == std::vector<md::PanelPacket>{pagePacket}, "first page pulse was not sent");
		// Without firmware the send is recorded but there is no FIFO to acknowledge
		// it. Advance to the queued up if the observed down is still awaiting a retry.
		Input::acknowledgeQueuedDown(*editor);
		mouse(pageButton, true);
		capture.drain();
		Input::tickQueue(*editor);
		require(capture.drain() == std::vector<md::PanelPacket>{pagePacket}, "queued page release cut off a real mouse hold");
		mouse(pageButton, false);
		Input::loseFocus(*editor);
		capture.drain();
		require(capture.rows == Rows{}, "cancelling overlapping page controls left a hold");

		// Releasing either PLAY alias leaves the other owner intact.
		panel->modifierKeysChanged(juce::ModifierKeys(juce::ModifierKeys::shiftModifier));
		press('k', juce::ModifierKeys::shiftModifier);
		press(' ', juce::ModifierKeys::shiftModifier);
		capture.drain();
		release('k');
		capture.drain();
		require((capture.rows[function.row - 0x20] & function.mask) != 0,
			"releasing one PLAY alias released FUNCTION too early");
		release(' ');
		capture.drain();
		require(capture.rows == functionOnly, "last PLAY alias released Shift or left PLAY held");
		modifiers(0);
		capture.drain();
		require(capture.rows == Rows{}, "Shift release left FUNCTION held");

		// A shifted digit must still address the same trig, without repeat edges.
		panel->modifierKeysChanged(juce::ModifierKeys(juce::ModifierKeys::shiftModifier));
		capture.drain();
		press('!', juce::ModifierKeys::shiftModifier);
		capture.drain();
		require(capture.rows[0] == 1, "Shift+1 lost its trig mapping");
		press('1', juce::ModifierKeys::shiftModifier);
		require(capture.drain().empty(), "shifted repeat acquired a second owner");
		release('1');
		panel->modifierKeysChanged(juce::ModifierKeys());
		capture.drain();
		require(capture.rows == Rows{}, "shifted digit stuck");

		// Mouse and keyboard FUNCTION/encoder owners release independently.
		for(bool mouseFirst : {false, true})
		{
			mouse("btFunction", true);
			panel->modifierKeysChanged(juce::ModifierKeys(juce::ModifierKeys::shiftModifier));
			press('j', juce::ModifierKeys::shiftModifier);
			capture.drain();
			if(mouseFirst) mouse("btFunction", false);
			else modifiers(0);
			capture.drain();
			require(capture.rows[function.row - 0x20] == function.mask, "shared FUNCTION released early");
			if(mouseFirst) modifiers(0);
			else mouse("btFunction", false);
			release('j');
			capture.drain();
			require(capture.rows == Rows{}, "shared FUNCTION stuck");
			panel->modifierKeysChanged(juce::ModifierKeys());

			mouse("encA", true, Rml::Input::KM_ALT);
			press(juce::KeyPress::F1Key);
			capture.drain();
			if(mouseFirst) mouse("encA", false);
			else release(juce::KeyPress::F1Key);
			capture.drain();
			require(capture.rows[encoderRow - 0x20] == 1, "shared encoder released early");
			if(mouseFirst) release(juce::KeyPress::F1Key);
			else mouse("encA", false);
			capture.drain();
			require(capture.rows == Rows{}, "shared encoder stuck");
		}

		// Option selects tracks, and its release cannot turn the key into a trig.
		phase = "track selection";
		constexpr char trackKeys[] = "12345678asdfghjk";
		for(int i = 0; i < trackCount; ++i)
		{
			press(trackKeys[i], juce::ModifierKeys::altModifier);
			const auto events = capture.drain();
#if defined(MD_KEYBOARD_TEST_MM)
			require(events == std::vector<md::PanelPacket>{{0x22, static_cast<uint8_t>(0x80 >> i)}},
				"wrong Monomachine track");
#else
			require(events.empty(), "Machinedrum track selection fired a trig");
#endif
			press(trackKeys[i]);
			require(capture.drain().empty(), "releasing Option changed a held binding");
			require(mdJucePlugin::EditorIdentityTestAccess::heldKeys(*editor) == 1, "track repeat duplicated ownership");
			release(trackKeys[i]);
			capture.drain();
			require(capture.rows == Rows{}, "track selection stuck");
		}
		constexpr char pages[] = "asdfgh;";
		phase = "page selection";
		for(int i = 0; i < pageCount; ++i)
		{
			press(pages[i]);
			require(mdJucePlugin::EditorIdentityTestAccess::page(*editor) == i, "wrong direct data page");
			press(pages[i]);
			require(mdJucePlugin::EditorIdentityTestAccess::heldKeys(*editor) == 1, "page repeated");
			release(pages[i]);
		}
		capture.drain();

		// Separate keys sharing PLAY must release independently.
		phase = "PLAY aliases and mouse bank latch";
		press('k');
		press(' ');
		capture.drain();
		release('k');
		capture.drain();
		require(capture.rows[performance[1].packet.row - 0x20] == performance[1].packet.mask, "PLAY alias released early");
		release(' ');
		capture.drain();
		require(capture.rows == Rows{}, "PLAY alias stuck");

		// Mouse-latched MM banks still finish after a keyboard trig selection.
#if defined(MD_KEYBOARD_TEST_MM)
		panel->modifierKeysChanged(juce::ModifierKeys(juce::ModifierKeys::shiftModifier));
		mouse("btBankA", true, Rml::Input::KM_SHIFT);
		mouse("btBankA", false, Rml::Input::KM_SHIFT);
		press('1', juce::ModifierKeys::shiftModifier);
		capture.drain();
		Rows bankAndTrig{};
		bankAndTrig[4] = 0x08;
		bankAndTrig[0] = 1;
		require(capture.rows == bankAndTrig, "mouse bank + keyboard trig added FUNCTION");
		release('1');
		capture.drain();
		require(capture.rows == Rows{}, "keyboard trig did not release mouse bank latch");
		panel->modifierKeysChanged(juce::ModifierKeys());
#endif
		// Desktop/DAW shortcuts are not new panel bindings.
		for(int key : {'1', '2', '3', '4', '5', '6', '7', '8', 'Q', 'J'})
			for(int mods : {juce::ModifierKeys::ctrlModifier, juce::ModifierKeys::commandModifier})
			{
				peer->handleKeyPress(juce::KeyPress(key, juce::ModifierKeys(mods), 0));
				require(capture.drain().empty(), "host shortcut reached the machine");
			}
		// Rml popup focus and a native modal component must keep their keys.
		peer->handleKeyPress(juce::KeyPress(juce::KeyPress::escapeKey));
		require(editor->settingsOpened(), "idle Escape lost the existing settings shortcut");
		peer->handleKeyPress(juce::KeyPress('1'));
		require(capture.drain().empty(), "settings key reached the machine");
		peer->handleKeyPress(juce::KeyPress(juce::KeyPress::escapeKey));
		require(!editor->settingsOpened(), "Escape did not close settings");
		auto* popup = context.CreateDocument();
		popup->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Document);
		peer->handleKeyPress(juce::KeyPress('1'));
		require(capture.drain().empty(), "popup key reached the machine");
		popup->Close();
		editor->getDocument()->Focus();
		juce::Component modal;
		modal.enterModalState(false);
		peer->handleKeyPress(juce::KeyPress('J'));
		require(capture.drain().empty(), "modal key reached the machine");
		modal.exitModalState(0);
		// Focus loss also clears new held controls and encoders.
		phase = "focus loss after closing dialogs";
		for(int key : {'1', 'Q', 'Z', 'K'}) press(key);
		press(juce::KeyPress::F1Key);
		capture.drain();
		mdJucePlugin::EditorIdentityTestAccess::loseFocus(*editor);
		capture.drain();
		require(capture.rows == Rows{}, "focus loss left a performance key held");

		// An unbound key must not send a machine event.
		for(int mods : {0, static_cast<int>(juce::ModifierKeys::shiftModifier)})
		{
			panel->modifierKeysChanged(juce::ModifierKeys(mods));
			peer->handleKeyPress(juce::KeyPress('9', juce::ModifierKeys(mods), 0));
			require(capture.drain().empty(), "unbound key reached machine");
		}
		panel->modifierKeysChanged(juce::ModifierKeys());
#if !defined(MD_KEYBOARD_TEST_MM)
		// MD has no dedicated track-switch row. Verify the actual status SysEx
		// reaching a device, rather than just checking that no trig was sent.
		phase = "MD track MIDI delivery";
		auto device = std::make_unique<MidiCaptureDevice>();
		auto* midiDevice = device.get();
		auto& plugin = processor.getPlugin();
		processor.setTestDevice(std::move(device));
		plugin.setHostSamplerate(44100.0f, 44100.0f);
		plugin.setBlockSize(64);
		const auto process = [&]
		{
			for(int i = 0; i < 4; ++i) plugin.process({}, {}, 64, 120.0, 0.0, false, false);
		};
		process();
		midiDevice->messages.clear();
		for(int i = 0; i < 16; ++i)
		{
			press(trackKeys[i], juce::ModifierKeys::altModifier);
			process();
			const std::vector<uint8_t> expectedStatus{0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00, 0x71, 0x22,
				static_cast<uint8_t>(i), 0xf7};
			require(midiDevice->messages == std::vector<std::vector<uint8_t>>{expectedStatus}, "wrong MD track SysEx");
			midiDevice->messages.clear();
			press(trackKeys[i]);
			process();
			require(midiDevice->messages.empty(), "MD track auto-repeat sent another SysEx");
			release(trackKeys[i]);
		}
#endif
		panel->removeFromDesktop();
		std::puts("Panel keyboard bindings, mouse chords, repeat and focus release passed.");
		return 0;
	}
	catch(const std::exception& error)
	{
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
