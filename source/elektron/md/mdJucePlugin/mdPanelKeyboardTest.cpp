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
		panel->grabKeyboardFocus();
		auto* peer = panel->getPeer();
		require(peer != nullptr, "native peer missing");
		auto& instrumentation = processor.getPlugin().getRealtimeInstrumentation();
		instrumentation.setEnabled(true);
		instrumentation.reset();
		Capture capture{instrumentation};
		juceRmlUi::RmlInterfaces::ScopedAccess access(*panel);
		auto& context = *panel->getContext();
		context.Update();

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
			{'p', {0x22, 0x20}}, {'m', {0x24, 0x01}}};
		constexpr uint8_t encoderRow = 0x25;
		constexpr int trackCount = 16, pageCount = 3;
#endif
		phase = "performance bindings";
		for(const auto& binding : performance) checkKey(binding.key, binding.packet);
		for(int i = 0; i < 8; ++i)
			checkKey(juce::KeyPress::F1Key + i, {encoderRow, static_cast<uint8_t>(1u << i)});

		// Shift alone is a held FUNCTION key, including COPY/CLEAR/PASTE chords.
		phase = "FUNCTION chords";
		for(int key : {'j', 'k', 'l'})
		{
			panel->modifierKeysChanged(juce::ModifierKeys(juce::ModifierKeys::shiftModifier));
			capture.drain();
			require(capture.rows[function.row - 0x20] == function.mask, "Shift did not press FUNCTION");
			press(key, juce::ModifierKeys::shiftModifier);
			capture.drain();
			const auto packet = performance[key - 'j'].packet;
			Rows expectedChord{};
			expectedChord[function.row - 0x20] |= function.mask;
			expectedChord[packet.row - 0x20] |= packet.mask;
			require(capture.rows == expectedChord, "Shift transport chord incorrect");
			panel->modifierKeysChanged(juce::ModifierKeys());
			capture.drain();
			Rows targetOnly{};
			targetOnly[packet.row - 0x20] = packet.mask;
			require(capture.rows == targetOnly, "releasing Shift released the target key");
			release(key);
			capture.drain();
			require(capture.rows == Rows{}, "Shift chord stuck");
		}

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
			capture.drain();
			if(mouseFirst) mouse("btFunction", false);
			else panel->modifierKeysChanged(juce::ModifierKeys());
			capture.drain();
			require(capture.rows[function.row - 0x20] == function.mask, "shared FUNCTION released early");
			if(mouseFirst) panel->modifierKeysChanged(juce::ModifierKeys());
			else mouse("btFunction", false);
			capture.drain();
			require(capture.rows == Rows{}, "shared FUNCTION stuck");

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
		mouse("btBankA", true);
		mouse("btBankA", false);
		press('1');
		capture.drain();
		require(capture.rows[4] == 0x08 && capture.rows[0] == 1, "mouse bank + keyboard trig broken");
		release('1');
		capture.drain();
		require(capture.rows == Rows{}, "keyboard trig did not release mouse bank latch");
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
		peer->handleKeyPress(juce::KeyPress('9'));
		require(capture.drain().empty(), "unbound key reached machine");
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
