# MD/MM keyboard and touch controls

[macOS build instructions](#build-on-macos-without-pgo) · [Touch and Sidecar](#touch-screens-and-sidecar)

This is a small fork of [joelanders/gearmulator-md-mm](https://github.com/joelanders/gearmulator-md-mm),
based on [PR #100](https://github.com/joelanders/gearmulator-md-mm/pull/100).
It adds computer-keyboard and multi-touch controls to the shared Machinedrum and Monomachine
editor for the standalone applications and VST3 plug-ins, including macOS.
The layout follows the useful parts of [Octemu's keyboard map](https://github.com/markandrus/octemu/blob/3adc0a8e59db4342ee105f937dc620a0039d5b58/src/main.c),
adapted to the buttons and pages these two machines actually have. Backspace is
NO, and Option/Alt selects tracks instead of Control, leaving Control+number
available for desktop switching.

| Computer key | Machinedrum | Monomachine |
| --- | --- | --- |
| Left / Right / Down / Up | Corresponding arrow button | Same |
| Enter / Return | YES | YES |
| Backspace (Mac Delete / ⌫) | NO | NO |
| Shift + a keyboard button | FUNCTION + that button, when no Shift-click gesture is active | Same |
| Shift-click a panel button | Hold the clicked button until Shift is released | Same |
| 1 2 3 4 5 6 7 8 | TRIG 1–8 | TRIG 1–8 |
| Q W E R T Y U I | TRIG 9–16 | TRIG 9–16 |
| Option/Alt + 1–6 | Select tracks 1–6 | Hold/select tracks 1–6 |
| Option/Alt + 7–8 | Select tracks 7–8 | Unassigned |
| Option/Alt + A S D F G H J K | Select tracks 9–16 | Unassigned |
| J | RECORD | RECORD |
| K or Space | PLAY | PLAY |
| L | STOP | STOP |
| Shift + J | FUNCTION + RECORD (COPY) | Same |
| Shift + K or Shift + Space | FUNCTION + PLAY (CLEAR) | Same |
| Shift + L | FUNCTION + STOP (PASTE) | Same |
| O | KIT | KIT/SONG SETUP |
| P | PATTERN/SONG | SONG MODE |
| N | SCALE / pattern page | SCALE / pattern page |
| Backslash (\) | TEMPO | TEMPO |
| Hold Shift and tap backslash | Tap tempo | Tap tempo |
| B | Bank group | Bank group |
| Z X C V | Bank buttons A/E, B/F, C/G, D/H | Same |
| A S D | Synthesis, Effects, Routing pages | Synthesis, Amplification, Filter pages |
| F G H ; | Unassigned | Effects, LFO 1, LFO 2, LFO 3 pages |
| ] | SYNTHESIS / EFFECTS / ROUTING page button | Next data page button |
| Shift + ] | FUNCTION + page button (LFO) | FUNCTION + next data page button |
| [ | Unassigned | Previous data page button |
| M | CLASSIC/EXTENDED | TRIG SELECT |
| F1–F8 | Hold encoder switches A–H | Same |
| Escape | Release held panel inputs; toggle settings when idle | Same |

Use a US English keyboard layout for the letter and punctuation bindings. The
upper Machinedrum tracks use the home row to avoid macOS accent dead keys. On a
Mac, Option is the Alt key, and Fn may be needed to send F1–F8 if the function
row is assigned to system controls. Encoder keys press switches; the existing
mouse controls still turn the encoders.

Held keys show the same press as a mouse hold on the panel. F1–F8 shade the
corresponding encoder, and direct page or MD track selection highlights its
label. RECORD turns pale coral with a dark border while held; the separate
recording LED continues to show the machine’s recording state.

The bindings work throughout the emulated machine's pages while its panel has
keyboard focus, including with mouse-held FUNCTION, COPY, CLEAR and PASTE.
Shift alone does not press FUNCTION: Shift-click holds only the clicked panel
button, so trigs can be held for parameter locks without selecting another sound.
Hold Shift and press a panel button on the keyboard to engage FUNCTION. It stays
held between button presses until you release Shift. For tap tempo, keep Shift
down and tap backslash once per beat; release Shift to finish.
After a Shift-click, Shift remains reserved for mouse holds until it is released;
keyboard arrows, trigs and encoder switches can then be used with those holds
without adding FUNCTION. To hold FUNCTION for a mouse action, Shift-click the
on-screen FUNCTION button. Secondary actions follow the firmware and current
screen. MD track selection and direct page selection use the same actions as
their clickable labels. Automatic page and mode changes wait for FUNCTION and
the required page/mode button to be free before sending a press.

Each key keeps its binding until released, including when Shift or Option is
released first. Mouse holds, keyboard holds and the K/Space aliases own their
button states independently. OS auto-repeat does not create additional presses.
Changing window focus or pressing Escape releases held inputs. Native dialogs,
settings and popup menus keep their normal keys. Other than the original six
navigation/YES/NO keys, Control/Command combinations are not panel shortcuts.

In a DAW, give the plug-in window keyboard focus and enable sending keys to the
plug-in if the host requires it. The emulation and audio are unchanged
from PR #100.

## Build on macOS without PGO

These commands build Release **standalone apps and VST3 plug-ins** for an
Apple-silicon Mac (M1 or newer), with PGO explicitly disabled. No profile
generation or training run is needed.

**1. Install the tools once.** Install [Xcode](https://developer.apple.com/xcode/)
(27 or newer for full touch support), open it and finish its initial setup.
Install [Homebrew](https://brew.sh/) if needed, then run in Terminal:

```sh
sudo xcode-select --switch /Applications/Xcode.app/Contents/Developer
brew install cmake ninja
```

**2. Get this branch, including its dependencies:**

```sh
git clone --branch dev/multi-touch-support --recurse-submodules https://github.com/nerazin/gearmulator-md-mm-test.git
cd gearmulator-md-mm-test
```

**3. Configure once** from the repository folder:

```sh
cmake -S . -B build-macos -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DGEARMULATOR_MDMM_APPLE_PGO_MODE=none \
  -DBUILD_TESTING=OFF -DDSP56300_DEBUGGER=OFF \
  -Dgearmulator_BUILD_JUCEPLUGIN=ON -Dgearmulator_BUILD_FX_PLUGIN=OFF \
  -Dgearmulator_BUILD_JUCEPLUGIN_Standalone=ON -Dgearmulator_BUILD_JUCEPLUGIN_VST3=ON \
  -Dgearmulator_BUILD_JUCEPLUGIN_VST2=OFF -Dgearmulator_BUILD_JUCEPLUGIN_AU=OFF \
  -Dgearmulator_BUILD_JUCEPLUGIN_CLAP=OFF -Dgearmulator_BUILD_JUCEPLUGIN_LV2=OFF \
  -Dgearmulator_SYNTH_ELEKTRON=ON -Dgearmulator_SYNTH_OSIRUS=OFF \
  -Dgearmulator_SYNTH_OSTIRUS=OFF -Dgearmulator_SYNTH_VAVRA=OFF \
  -Dgearmulator_SYNTH_XENIA=OFF -Dgearmulator_SYNTH_NODALRED2X=OFF \
  -Dgearmulator_SYNTH_JE8086=OFF
```

For an Intel Mac, replace `arm64` with `x86_64`; for a universal build, use
`"arm64;x86_64"`. The policy flag allows the bundled dependencies to configure
with CMake 4.

**4. Compile both machines:**

```sh
cmake --build build-macos --parallel 4 --target \
  mdJucePlugin_Standalone mdJucePlugin_VST3 \
  mmJucePlugin_Standalone mmJucePlugin_VST3
```

For **Machinedrum only**, keep just the two `mdJucePlugin_...` targets.
For **Monomachine only**, keep just the two `mmJucePlugin_...` targets.
For standalone only, omit the `_VST3` targets.

The finished files are inside the repository:

| Format | Folder | Files |
| --- | --- | --- |
| Standalone | `bin/plugins/Release/Standalone/` | `Gearmulator MD.app`, `Gearmulator MM.app` |
| VST3 | `bin/plugins/Release/VST3/` | `Gearmulator MD.vst3`, `Gearmulator MM.vst3` |

Open the apps directly. To use the plug-ins in a DAW, copy the `.vst3` bundles to
`~/Library/Audio/Plug-Ins/VST3/` and rescan plug-ins. Your own firmware is required
to run the emulators; it is not included in the build.

**Updating an existing checkout of this fork:** run the following inside its
folder, then repeat steps 3 and 4. You do not need to clone or reinstall the tools.

```sh
git fetch origin
git switch dev/multi-touch-support
git pull --ff-only origin dev/multi-touch-support
git submodule update --init --recursive
```

For the optional PGO workflow, see [Apple optimisation notes](doc/mdmm-apple-optimization.md).
For finger input through Sidecar, see the OS requirements below.

## Touch screens and Sidecar

On macOS, direct touch uses an AppKit gesture recognizer attached to the editor's
native view. Apple's expanded Sidecar finger input requires **macOS 27 and iPadOS
27 or later**. Build with Xcode 27 or newer to include the SDK's per-view gesture
exclusivity setting. Older Sidecar versions and remote-display software that
send only one mouse pointer cannot provide independent fingers to this code.
Trackpad gestures and Apple Pencil pointer input are not treated as multiple
screen contacts. See [Apple's Sidecar touch guidance](https://developer.apple.com/documentation/technotes/tn3212-adopting-gesture-recognizers-for-sidecar-touch-support).

- Hold physical panel buttons with separate fingers. Each stays down until its
  finger lifts, even if the finger drifts outside the button. Touching two places
  on one button keeps it down until both lift. Keyboard and mouse owners remain
  independent, and held buttons retain the same visible feedback.
- For COPY, CLEAR, PASTE and other secondary functions, hold the physical
  FUNCTION button and press the target button with another finger. The shortcut
  labels (including MM track MUTE labels) are inert for touch; their mouse
  shortcuts still work. For tap tempo, hold FUNCTION and repeatedly tap TEMPO.
- MM bank buttons are momentary with touch: hold a bank while selecting a trig,
  then lift. Their persistent mouse-click latch is not used for touch.
- Drag up/right to increase a knob and down/left to decrease it. Several knobs
  can be turned independently while other fingers hold trigs or FUNCTION. The
  first finger on each knob owns its drag until lift. Mouse and touch drags use
  the same reduced sensitivity for fine adjustments: at 100% in settings,
  drags need three times the movement of the original defaults. LCD dragging
  uses the same reduction. The firmware controls each parameter's value range;
  the reduction applies equally to continuous parameters and short choice lists.
- To push a data-entry knob **A–H**, tap it once, lift, then touch the same knob
  again within 350 ms and keep your finger down. The second contact immediately
  presses the encoder switch and shades the knob. Drag that finger to turn while
  pressed; lift to release. Several knobs can be pressed and turned independently.
  The first tap must be brief (at most 250 ms), with no substantial movement;
  an ordinary drag or long hold does not arm the gesture. LEVEL, MASTER VOLUME
  and the MD sound selector remain turn-only, matching the existing switch mapping.
- Focus loss, hiding, resizing, system cancellation and the keyboard Escape key
  release touch holds. No touch gesture opens the Escape/settings menu.
  Settings opened from the keyboard and popups use a single-touch mouse-compatible path; they do
  not expose the panel underneath. The touch bridge also accepts JUCE touch
  sources on platforms which supply them.

The `mdPanelTouchTest` and `mmPanelTouchTest` targets exercise independent scan
rows, FUNCTION chords, disabled touch shortcuts, eight concurrent encoder
drags, double-tap pushes and pressed turns, wrap direction, mixed-input ownership
and cancellation without firmware.
The `MD/MM touch input` workflow builds both products and runs the touch and
keyboard regression tests with the macOS 27 SDK.

Device validation still matters: on a real Sidecar connection, test FUNCTION +
RECORD, two held trigs while turning two knobs, tap tempo, ignored COPY/MUTE
labels, and lifting one of several fingers. Repeat in the standalone and the
DAW plug-in, then disconnect Sidecar during a hold and check that no input
remains stuck. Automated contact tests cannot validate the iPad transport or
every host's window behaviour.

The original project's README follows.

---

## Original project

My fork of TUS's Gearmulator project, where I add emulations of Elektron's
Machinedrum and Monomachine.

I'm not affiliated with TUS or Elektron. Don't bug them for support :)

There is a Discord channel [here](https://discord.gg/BnkTKpmp8) at #gearmulator-development.
**Do NOT discuss firmware or ROMs in Discord.**
**DO NOT ask us for the .bin files / firmware! They're under Elektron's copyright. This emulator is for people who own the original hardware.**

[Downloads](https://github.com/joelanders/gearmulator-md-mm/releases) ·
[Report a bug](https://github.com/joelanders/gearmulator-md-mm/issues)

Link to a short demo on Youtube:

<a href="https://www.youtube.com/watch?v=NmfE5xljYRU"><img width="800" alt="youtube" src="https://i3.ytimg.com/vi/NmfE5xljYRU/maxresdefault.jpg" /></a>


## Features

- **Key chording / p-locks:** shift-click one or more buttons to hold them
  down until you release the shift key.
- **Secondary functions:** rather than shift-click Function and another button,
  you can just click the secondary function text label.
- **Encoder clicking:** Alt/Option-click a DATA ENTRY encoder to press it, or
  Alt/Option-drag to press and turn. With a trig held, pressing its parameter's
  encoder toggles that parameter lock. This applies to encoders A–H, not LEVEL
  or SOUND SELECTION.
- **Send SysEx File** under the right click menu to send a `.syx` file to the
  machine. The menu shows transfer progress and lets you cancel. Follow the
  machine's normal receive procedure.
- **Panel look and feel:** adjust encoder-drag and mouse-wheel sensitivity in settings.
  An experimental crisp LCD/panel rendering option is also available.
- **Audio inputs and outputs:** route host audio to the machine's input effects or sampling
  functions. Additional output pairs are available in a multi-output VST3 host;
  the standalone apps use stereo output.

## Implementation references

- [TurboMIDI negotiation](doc/turbomidi.md): a worked exchange, firmware observations,
  and Gearmulator sender policy.

Thanks to the upstream Gearmulator contributors whose work makes this fork
possible. See [the upstream README](README.upstream.md) for the original project
overview.
