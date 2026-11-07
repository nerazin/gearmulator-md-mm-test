#include "mdEditor.h"
#include "mdPluginEditorState.h"
#include "mdPluginProcessor.h"
#include "juceRmlUi/juceRmlComponent.h"
#include "juceRmlUi/rmlElemKnob.h"
#include "RmlUi/Core/ComputedValues.h"
#include "RmlUi/Core/Context.h"
#include "RmlUi/Core/ElementDocument.h"
#include "synthLib/realtimeInstrumentation.h"
#include "synthLib/plugin.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>

namespace mdJucePlugin
{
	struct EditorIdentityTestAccess
	{
		static void releaseKey(Editor& e, int key) { e.setPanelKey(key, false); }
		static void loseFocus(Editor& e) { e.globalFocusChanged(nullptr); }
		static void tick(Editor& e) { e.timerCallback(1); }
		static void queue(Editor& e, md::PanelControl c) { e.queuePanelPulse(c); }
		static void tickQueue(Editor& e) { e.servicePanelQueue(); }
	};
}
namespace
{
	void require(bool condition, const char* message) { if(!condition) throw std::runtime_error(message); }
	using Rows = std::array<uint8_t, 7>;
	using Phase = juceRmlUi::TouchEvent::Phase;
	using Input = mdJucePlugin::EditorIdentityTestAccess;
}

int main()
{
	try
	{
		juce::ScopedJuceInitialiser_GUI gui;
#if defined(MD_TOUCH_TEST_MM)
		constexpr auto model = md::MachineModel::Monomachine;
#else
		constexpr auto model = md::MachineModel::Machinedrum;
#endif
		mdJucePlugin::AudioPluginAudioProcessor processor(model,
			mdJucePlugin::AudioPluginAudioProcessor::EphemeralConfig{std::string{}}, false);
		processor.setForceSoftwareRendererForSession(true);
		auto& state = static_cast<mdJucePlugin::PluginEditorState&>(processor.getOrCreateEditorState());
		auto* editor = dynamic_cast<mdJucePlugin::Editor*>(state.getEditor());
		require(editor != nullptr, "editor missing");
		auto* panel = editor->getRmlComponent();
		panel->addToDesktop(0);
		panel->setVisible(true);
		panel->toFront(true);
		for(int i = 0; i < 20 && !panel->hasKeyboardFocus(true); ++i)
		{
			panel->grabKeyboardFocus();
			if(!panel->hasKeyboardFocus(true)) juce::Thread::sleep(10);
		}
		require(panel->hasKeyboardFocus(true), "panel has no native focus");
		juceRmlUi::RmlInterfaces::ScopedAccess access(*panel);
		auto& context = *panel->getContext();
		context.Update();
		editor->getDocument()->Focus();
		auto& instrumentation = processor.getPlugin().getRealtimeInstrumentation();
		instrumentation.setEnabled(true);
		instrumentation.reset();
		Rows rows{};
		const auto drain = [&]
		{
			std::vector<md::PanelPacket> packets;
			synthLib::RealtimeEvent event;
			while(instrumentation.popTimelineEvent(event))
				if(event.kind == synthLib::RealtimeEventKind::PanelInput)
				{
					packets.push_back({static_cast<uint8_t>(event.command), static_cast<uint8_t>(event.argument)});
					if(event.command >= 0x20 && event.command <= 0x26) rows[event.command - 0x20] = event.argument;
				}
			return packets;
		};
		const auto element = [&](const std::string& id)
		{
			auto* e = editor->getDocument()->GetElementById(id);
			if(!e) throw std::runtime_error("missing touch target " + id);
			return e;
		};
		const auto centre = [&](const std::string& id)
		{
			context.Update();
			auto* e = element(id);
			const auto pos = e->GetAbsoluteOffset(Rml::BoxArea::Border);
			const auto size = e->GetBox().GetSize(Rml::BoxArea::Border);
			const auto dimensions = context.GetDimensions();
			return juce::Point<float>{(pos.x + size.x * 0.5f) * panel->getWidth() / dimensions.x,
				(pos.y + size.y * 0.5f) * panel->getHeight() / dimensions.y};
		};
		const auto touch = [&](uint64_t id, Phase phase, const std::string& target)
		{ panel->processTouch({id, phase, centre(target)}); };
		const auto mouse = [&](const char* id, bool down, int modifiers = 0)
		{
			const auto pos = centre(id);
			const auto p = panel->toRmlPosition(juce::roundToInt(pos.x), juce::roundToInt(pos.y));
			context.ProcessMouseMove(p.x, p.y, modifiers);
			if(down) context.ProcessMouseButtonDown(0, modifiers); else context.ProcessMouseButtonUp(0, modifiers);
		};
		const auto key = [&](int k) { require(panel->getPeer()->handleKeyPress(juce::KeyPress(k)), "key not handled"); };
		const auto held = [&](const std::string& id, bool expected)
		{ require(element(id)->IsClassSet("touchHeld") == expected, "wrong touch highlight"); };

		// Independent scan-row bits and targets; no hover dependency or repeats.
		Rows expected{};
		for(int i = 0; i < 16; ++i)
		{
			const auto id = "trigKey" + std::to_string(i);
			touch(i + 1, Phase::Begin, id); drain();
			expected[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
			require(rows == expected, "simultaneous trigs lost a bit or added FUNCTION");
			held(id, true);
			touch(i + 1, Phase::Begin, id);
			require(drain().empty(), "duplicate touch-down repeated a press");
		}
		panel->processTouch({1, Phase::Move, {-100, -100}});
		require(drain().empty() && rows == expected, "moving off released or retargeted a button");
		for(int i = 15; i >= 0; --i)
		{
			touch(i + 1, Phase::End, "btRec"); drain(); // lift over a different control
			expected[i / 8] &= static_cast<uint8_t>(~(1u << (i % 8)));
			require(rows == expected, "touch-up released another trig");
			held("trigKey" + std::to_string(i), false);
		}
		const auto function = *md::panelPacket(model, md::PanelControl::Function);
		const auto record = *md::panelPacket(model, md::PanelControl::Record);
		const auto tempo = *md::panelPacket(model, md::PanelControl::Tempo);
		// A native frame contains an unordered set, not an ordered list of fingers.
		panel->processTouches({{301, Phase::Begin, centre("btRec")}, {302, Phase::Begin, centre("btFunction")}});
		const auto simultaneous = drain();
		require(simultaneous.size() == 2 && simultaneous.front() == function, "simultaneous FUNCTION arrived after its target");
		panel->cancelTouches();
		const auto cancelled = drain();
		require(cancelled.size() == 2 && cancelled.back() == md::PanelPacket{function.row, 0}, "cancellation released FUNCTION before its target");
		require(rows == Rows{}, "simultaneous chord cancellation stuck");
		const auto bank = *md::panelPacket(model, md::PanelControl::BankA);
		const auto trig = *md::panelPacket(model, md::PanelControl::Trigger1);
		panel->processTouches({{303, Phase::Begin, centre("trigKey0")}, {304, Phase::Begin, centre("btBankA")}});
		require(drain() == std::vector<md::PanelPacket>{bank, trig}, "simultaneous bank arrived after its trig");
		panel->processTouches({{304, Phase::End, centre("btBankA")}, {303, Phase::End, centre("trigKey0")}});
		require(drain() == std::vector<md::PanelPacket>{{trig.row, 0}, {bank.row, 0}}, "simultaneous lift released bank before trig");
		panel->processTouches({{305, Phase::Begin, centre("btBankA")}, {306, Phase::Begin, centre("btFunction")}});
		const auto functionBank = drain();
		require(functionBank.size() == 2 && functionBank.front() == function, "bank preceded simultaneous FUNCTION");
		panel->cancelTouches(); drain();
		require(rows == Rows{}, "bank chord cancellation stuck");
		touch(100, Phase::Begin, "btFunction");
		expected[function.row - 0x20] = function.mask;
		for(int tap = 0; tap < 4; ++tap)
		{
			touch(101, Phase::Begin, "btTempo"); drain();
			auto chord = expected; chord[tempo.row - 0x20] |= tempo.mask;
			require(rows == chord, "touch tap-tempo chord wrong");
			touch(101, Phase::End, "btTempo"); Input::tick(*editor); drain();
			require(rows == expected, "keyboard/mouse fail-safe released touch FUNCTION");
			held("btFunction", true);
		}
		touch(102, Phase::Begin, "btRec"); touch(103, Phase::Begin, "trigKey7"); drain();
		expected[record.row - 0x20] |= record.mask; expected[0] = 0x80;
		require(rows == expected, "three-finger FUNCTION chord wrong");
		touch(102, Phase::End, "btRec"); touch(103, Phase::Cancel, "trigKey7");
		touch(100, Phase::End, "btFunction"); drain();
		require(rows == Rows{}, "touch chord stuck");

		// Two fingers, keyboard and mouse share RECORD in every release order.
		std::array<int, 4> order{{0,1,2,3}};
		do
		{
			touch(110, Phase::Begin, "btRec"); mouse("btRec", true); key('J');
			touch(111, Phase::Begin, "btRec"); drain();
			for(size_t i = 0; i < order.size(); ++i)
			{
				switch(order[i])
				{
				case 0: touch(110, Phase::End, "btRec"); break;
				case 1: touch(111, Phase::Cancel, "btRec"); break;
				case 2: mouse("btRec", false); break;
				case 3: Input::releaseKey(*editor, 'J'); break;
				}
				drain();
				require(rows[record.row - 0x20] == (i == 3 ? 0 : record.mask), "shared RECORD released early or stuck");
				if(i != 3)
				{
					context.Update();
					require(element("btRec")->GetComputedValues().background_color() == Rml::Colourb(255,224,204), "shared RECORD highlight lost");
				}
			}
		} while(std::next_permutation(order.begin(), order.end()));

		// FUNCTION shortcut labels absorb touches, but still work with a mouse.
		Rml::ElementList shortcuts;
		editor->getDocument()->GetElementsByClassName(shortcuts, "panelFunctionShortcut");
		require(shortcuts.size() >= 11, "shortcut exclusions missing");
		for(auto* label : shortcuts)
		{
			touch(120, Phase::Begin, "btFunction"); drain(); const auto before = rows;
			touch(121, Phase::Begin, label->GetId()); touch(121, Phase::Move, "btRec"); touch(121, Phase::End, "btRec");
			require(drain().empty() && rows == before, "touch label invoked a shortcut");
			touch(120, Phase::End, "btFunction"); drain();
			mouse(label->GetId().c_str(), true); drain();
			require((rows[function.row - 0x20] & function.mask) != 0, "mouse shortcut was disabled too");
			mouse(label->GetId().c_str(), false); drain();
			require(rows == Rows{}, "mouse shortcut stuck");
		}
		touch(125, Phase::Begin, "btBankA"); drain(); touch(125, Phase::End, "btBankA"); drain();
		require(rows == Rows{}, "touch bank acquired a persistent mouse latch");

		// Eight independent encoder drags plus a held trig. Check actual UART
		// commands and direction, including repeated wraps and a very fast swipe.
		touch(130, Phase::Begin, "trigKey0");
		drain();
		std::array<juce::Point<float>, 8> points;
		for(int i = 0; i < 8; ++i)
		{
			const auto id = std::string("enc") + static_cast<char>('A' + i);
			points[i] = centre(id); touch(140 + i, Phase::Begin, id);
		}
		require(drain().empty(), "touching knobs pressed their encoder switches");
		for(int frame = 0; frame < 12; ++frame)
			for(int i = 0; i < 8; ++i)
			{
				points[i].y += i % 2 ? 20 : -20;
				panel->processTouch({static_cast<uint64_t>(140 + i), Phase::Move, points[i]});
				const auto packets = drain();
				require(!packets.empty(), "touch encoder produced no detents");
				for(const auto& p : packets) require(p.row == 0x30 + i && p.mask == (i % 2 ? 0xff : 1), "wrong encoder, switch press, or reversed wrap");
				require(rows[0] == 1, "knob drag disturbed held trig");
			}
		touch(155, Phase::Begin, "encA");
		panel->processTouch({155, Phase::Move, points[0] + juce::Point<float>(0,-200)});
		require(drain().empty(), "second finger stole a knob");
		points[0].y -= 1000; panel->processTouch({140, Phase::Move, points[0]});
		for(const auto& p : drain()) require(p.row == 0x30 && p.mask == 1, "fast swipe reversed direction");
		touch(140, Phase::End, "encA"); panel->processTouch({155, Phase::Move, points[0]});
		require(drain().empty(), "ignored finger resumed a finished gesture");
		touch(155, Phase::End, "encA");
		for(int i = 1; i < 8; ++i) touch(140 + i, Phase::End, "encA");
		touch(130, Phase::End, "trigKey0"); drain();
		require(rows == Rows{}, "touch turning pressed a switch or left a trig");
		for(const auto& target : std::array<std::pair<const char*, int>, 2>{{{"encLevel", 0x38}, {"encSound", 0x39}}})
		{
			if(target.second == 0x39 && model == md::MachineModel::Monomachine) continue;
			touch(157, Phase::Begin, target.first);
			panel->processTouch({157, Phase::Move, centre(target.first) + juce::Point<float>(0, -30)});
			const auto packets = drain();
			require(!packets.empty(), "additional encoder did not turn");
			for(const auto& p : packets) require(p.row == target.second && p.mask == 1, "wrong additional encoder");
			touch(157, Phase::End, target.first);
		}

		// A quick tap arms only its own data-entry encoder. The second contact
		// owns the switch until lift, even while moving or holding other controls.
		const auto timedTouch = [&](uint64_t id, Phase phase, const std::string& target,
			double time, juce::Point<float> offset = {})
		{ panel->processTouch({id, phase, centre(target) + offset, time}); };
		const auto doubleDown = [&](uint64_t id, const std::string& target, double time)
		{
			timedTouch(id, Phase::Begin, target, time);
			timedTouch(id, Phase::End, target, time + 40);
			require(drain().empty(), "first tap pressed an encoder switch");
			timedTouch(id + 1, Phase::Begin, target, time + 100);
		};
		const auto encoderA = *md::panelEncoderPressPacket(model, md::PanelEncoder::DataEntryA);
		for(int i = 0; i < 8; ++i)
		{
			const auto target = std::string("enc") + static_cast<char>('A' + i);
			doubleDown(400 + i * 2, target, 1000); drain(); held(target, true);
			context.Update();
			require(element(target)->GetComputedValues().opacity() == 0.75f, "pressed touch knob is not shaded");
			require(rows[encoderA.row - 0x20] == (1u << (i + 1)) - 1, "independent encoder switch missing");
		}
		touch(420, Phase::Begin, "trigKey0"); touch(421, Phase::Begin, "btFunction"); drain();
		Input::tick(*editor); drain();
		require(rows[encoderA.row - 0x20] == 0xff, "mouse/keyboard polling released touch encoder switches");
		for(int i = 0; i < 8; ++i)
		{
			const auto target = std::string("enc") + static_cast<char>('A' + i);
			timedTouch(401 + i * 2, Phase::Move, target, 2200, {0, -30});
			const auto packets = drain();
			require(!packets.empty(), "pressed touch encoder did not turn");
			for(const auto& p : packets) require(p.row == 0x30 + i && p.mask == 1, "pressed touch turned wrong encoder");
			require(rows[0] == 1 && (rows[function.row - 0x20] & function.mask), "pressed turn released another control");
			timedTouch(401 + i * 2, Phase::End, "btRec", 2250); drain(); held(target, false);
			require(rows[encoderA.row - 0x20] == static_cast<uint8_t>(0xffu << (i + 1)), "encoder lift released another knob");
		}
		panel->cancelTouches(); drain();

		// A completed push consumes the double-tap rather than arming a third tap.
		doubleDown(430, "encA", 3000); drain();
		timedTouch(431, Phase::End, "encA", 3140); drain();
		timedTouch(432, Phase::Begin, "encA", 3200);
		require(drain().empty() && rows == Rows{}, "third tap repeated a consumed push");
		panel->cancelTouches(); drain();
		// Reject long taps, expired taps, out-and-back drags, cancelled taps and
		// overlapping fingers. Invalidation also applies between completed taps.
		for(int reason = 0; reason < 8; ++reason)
		{
			timedTouch(440, Phase::Begin, "encA", 4000);
			if(reason == 2)
			{
				timedTouch(440, Phase::Move, "encA", 4020, {20, 0});
				timedTouch(440, Phase::Move, "encA", 4030);
			}
			if(reason == 4) timedTouch(441, Phase::Begin, "encA", 4020);
			timedTouch(440, reason == 3 ? Phase::Cancel : Phase::End, "encA", reason == 0 ? 4300 : 4040);
			if(reason == 4) timedTouch(441, Phase::End, "encA", 4050);
			if(reason == 5) panel->cancelTouches();
			if(reason == 6) Input::loseFocus(*editor);
			if(reason == 7)
			{
				const auto bounds = panel->getBounds();
				panel->setSize(bounds.getWidth() + 1, bounds.getHeight()); panel->setBounds(bounds);
			}
			drain();
			timedTouch(442, Phase::Begin, "encA", reason == 1 ? 4391 : 4350);
			require(drain().empty() && rows == Rows{}, "invalid first tap armed an encoder push");
			panel->cancelTouches(); drain();
		}
		timedTouch(450, Phase::Begin, "encA", 5000); timedTouch(450, Phase::End, "encA", 5040);
		timedTouch(451, Phase::Begin, "encB", 5100);
		require(drain().empty(), "tap on another knob pressed a switch"); panel->cancelTouches();
		for(const auto* target : {"encMaster", "encLevel", "encSound"})
		{
			if(std::string(target) == "encSound" && model == md::MachineModel::Monomachine) continue;
			doubleDown(460, target, 6000);
			require(drain().empty() && rows == Rows{}, "turn-only control acquired an encoder switch");
			panel->cancelTouches(); drain();
		}

		// Touch, F1 and Alt-click own the same switch independently, including
		// visual feedback, in every release order.
		std::array<int, 3> encoderOrder{{0, 1, 2}};
		do
		{
			doubleDown(470, "encA", 7000); drain();
			key(juce::KeyPress::F1Key); mouse("encA", true, Rml::Input::KM_ALT); drain();
			for(size_t i = 0; i < encoderOrder.size(); ++i)
			{
				switch(encoderOrder[i])
				{
				case 0: timedTouch(471, Phase::End, "encA", 7150); break;
				case 1: Input::releaseKey(*editor, juce::KeyPress::F1Key); break;
				case 2: mouse("encA", false, Rml::Input::KM_ALT); break;
				}
				drain(); context.Update();
				require(rows[encoderA.row - 0x20] == (i == 2 ? 0 : encoderA.mask), "shared encoder switch released early or stuck");
				require(element("encA")->GetComputedValues().opacity() == (i == 2 ? 1.0f : 0.75f), "shared encoder highlight lost or stuck");
			}
			panel->cancelTouches(); drain();
		} while(std::next_permutation(encoderOrder.begin(), encoderOrder.end()));
		for(int reason = 0; reason < 4; ++reason)
		{
			doubleDown(480, "encA", 8000); drain();
			if(reason == 0) timedTouch(481, Phase::Cancel, "encA", 8200);
			else if(reason == 1) Input::loseFocus(*editor);
			else if(reason == 2) key(juce::KeyPress::escapeKey);
			else panel->setVisible(false);
			drain(); held("encA", false);
			require(rows == Rows{}, "cancelled touch left encoder pressed");
			panel->setVisible(true); panel->grabKeyboardFocus();
			timedTouch(481, Phase::Move, "encA", 8250, {0, -50}); timedTouch(481, Phase::End, "encA", 8300);
			require(drain().empty(), "late event revived a cancelled encoder push");
		}

		const auto pageControl = model == md::MachineModel::Machinedrum
			? md::PanelControl::SynthesisEffectsRouting : md::PanelControl::DataPageForward;
		touch(160, Phase::Begin, "btFunction"); drain();
		Input::queue(*editor, pageControl); Input::tickQueue(*editor);
		require(drain().empty(), "queued page collided with touch FUNCTION");
		touch(160, Phase::End, "btFunction"); drain(); Input::tickQueue(*editor);
		require(drain() == std::vector<md::PanelPacket>{*md::panelPacket(model, pageControl)}, "queued page did not resume");
		Input::loseFocus(*editor); drain();

		// Cancellation, focus, Escape, and hiding cannot leave held inputs or
		// revive them on a late move/up; touch-only cancellation preserves keys.
		for(int reason = 0; reason < 4; ++reason)
		{
			touch(170, Phase::Begin, "btRec"); touch(171, Phase::Begin, "encA"); key('J'); drain();
			if(reason == 0) panel->cancelTouches();
			else if(reason == 1) Input::loseFocus(*editor);
			else if(reason == 2) key(juce::KeyPress::escapeKey);
			else panel->setVisible(false);
			drain(); held("btRec", false);
			if(reason == 0) require(rows[record.row - 0x20] == record.mask, "touch cancellation released a keyboard owner");
			Input::releaseKey(*editor, 'J'); drain();
			panel->processTouch({171, Phase::Move, {0,0}}); touch(170, Phase::End, "btRec");
			require(drain().empty() && rows == Rows{}, "late event revived cancelled touch");
			panel->setVisible(true); panel->grabKeyboardFocus();
		}
		// Escape must cancel a touch-only gesture rather than opening settings.
		touch(175, Phase::Begin, "trigKey0"); key(juce::KeyPress::escapeKey); drain();
		require(rows == Rows{} && !editor->settingsOpened(), "Escape missed a touch-only hold");
		// Reflow invalidates gesture coordinates; a new contact uses the new scale.
		const auto originalSize = panel->getBounds();
		touch(176, Phase::Begin, "trigKey0");
		panel->setSize(originalSize.getWidth() * 5 / 4, originalSize.getHeight() * 5 / 4); drain();
		require(rows == Rows{}, "resizing left a touch held");
		touch(177, Phase::Begin, "btUp"); drain();
		const auto up = *md::panelPacket(model, md::PanelControl::Up);
		require(rows[up.row - 0x20] == up.mask, "scaled touch selected the wrong target");
		touch(177, Phase::End, "btUp"); drain();
		panel->setSize(originalSize.getWidth(), originalSize.getHeight());
		touch(179, Phase::Begin, "trigKey0"); drain();
		auto* popup = context.CreateDocument();
		popup->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Document);
		Input::tick(*editor); drain();
		require(rows == Rows{}, "opening a popup left an earlier touch held");
		touch(180, Phase::Begin, "trigKey0"); touch(180, Phase::End, "trigKey0");
		require(drain().empty(), "touch passed through a popup");
		popup->Close(); editor->getDocument()->Focus();
		juce::Component modal; modal.enterModalState(false);
		touch(181, Phase::Begin, "trigKey0"); touch(181, Phase::End, "trigKey0");
		require(drain().empty(), "touch passed through a native modal");
		modal.exitModalState(0);
		require(!editor->settingsOpened(), "touch unexpectedly opened the Escape/settings menu");
		panel->removeFromDesktop();
		std::puts("Multi-touch buttons, labels, independent knobs, ownership and cancellation passed.");
		return 0;
	}
	catch(const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
