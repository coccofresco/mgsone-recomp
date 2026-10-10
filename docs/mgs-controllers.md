# Controller compatibility and Steam Input

Steam Input and common controllers are a required alpha acceptance criterion.
The current implementation passes automated SDL input tests. It is **not yet
qualified on physical SteamOS or Windows controller hardware**. Physical Vulkan
GPU tests are separate from controller transport tests. A virtual-device test cannot establish USB,
Bluetooth, Steam overlay, driver or suspend/resume behavior on a real Deck.

## Player setup

Add the AppImage or Flatpak launch command to Steam as described in
`mgs-steamos.md`, enable Steam Input for the shortcut, select the **Gamepad**
template and launch with `--big-picture`. Steam's controller remapping remains
available, including mapping rear buttons to existing controls. Guide/Home is
reserved for Steam. A desktop launch without Steam uses SDL's direct gamepad
support. The Steam client and Steamworks SDK are not required for desktop play.
On Windows, add `MGSRecomp.exe` to Steam and use the same `--big-picture` option.
Both Linux and Windows controller-database rows are exercised by the automated
virtual-controller tests, including DualShock 4, DualSense, Switch Pro and 8BitDo.

**Controller / input test**, present in the launcher and in-game host menu,
shows the active device, PS1 buttons, stick and trigger values. It offers:

- Automatic selection, preferring the Steam virtual gamepad when present.
- Explicit device selection, remembered by GUID and shared with the game.
- Adjustable stick deadzone and Xbox/PlayStation/Nintendo button labels.
- Live checks of buttons, stick movement and both triggers before playing.

The physical bottom face button maps to PS1 Cross; right maps to Circle,
left to Square and top to Triangle. Host-menu confirmation follows the same
positions. On Nintendo layouts this means **B confirms, A goes back**.
The original USA game's menu semantics are unchanged. Steam remapping and
custom SDL mappings can deliberately change these assignments.

SDL can expose the virtual Xbox layout when Steam is translating a different
physical controller. Label override changes prompts only and can match the
player's chosen Steam layout; it does not undo Steam's remapping. 8BitDo models
also change their reported identity depending on XInput/Switch/DInput mode.

Settings are stored in `controllers.toml` beside the launcher's `settings.json`.
They are outside both the application installation and guest savestates.
If the preferred device is absent, another available pad is used. When several
identical devices share one GUID, the active instance stays stable during the
session; cross-session ordering of identical devices is not guaranteed.

## Implementation and regression rationale

`ControllerHub` owns the SDL handles and selects one instance. The game and
Dear ImGui both read that same handle; ImGui uses its manual gamepad mode.
The former independent "first controller" selection in the UI could diverge
from gameplay after a disconnect or when Steam supplied another device.
Inputs from separate physical/virtual gamepads are never merged.

Before launching the game, the frontend closes its controller handles and
acknowledges release to the launcher bridge. The bridge waits for that explicit
acknowledgement before starting the runtime. On return, the frontend reopens
devices and reloads preferences changed in game. This also avoids keeping two
processes reading physical controllers during play.

Automatic selection recognizes Valve's virtual VID/PID from the raw joystick
GUID. SDL's vendor/product getters can report the physical device behind Steam,
so those getters alone are insufficient. A Valve vendor or device name alone
does not classify a physical Steam Deck/Controller as a virtual gamepad.
Explicit user device selection takes precedence over automatic selection.

Buttons and axes are sampled from the selected handle after each event batch.
Device add/remove/remap notifications use SDL instance IDs, not mutable device
indices. Gameplay waits for neutral controls after selection, reconnection,
remapping or a host-menu/focus pause. This prevents a held menu-confirm button
from becoming an unintended game action. Keyboard and controller state remain
separate until publication to the original selected PS1 port. Cards and guest
timing remain untouched.

The application fixes SDL's face-button policy to physical positions before
initialization. It preserves Steam's device ignore/allow lists, HID hints,
controller mappings, process environment and application identity. There is
no parallel raw-HID input path or dependency on an invented Steam AppID.

An unmodified SDL_GameControllerDB snapshot supplies 743 Linux mappings in
addition to SDL's built-ins. Its license and SHA256 provenance are included.
It is an existing local distribution snapshot, not claimed to be the latest
upstream commit. Environment-provided SDL mappings are applied after this
database. Packaged binaries and the launcher carry the same database beside
their executables. SDL 2.26 or later is required; AppImage bundles SDL and
Flatpak uses its declared runtime with `--device=input`.

## Evidence and acceptance matrix

Run `build/ps1Test/mgsController_tests` without game assets. The suite creates
SDL virtual controllers and exercises actual SDL mapping, event delivery,
sampling, ImGui navigation input and PS1 port publication. It covers:

- Steam virtual preference, exclusion of the other pad and explicit selection.
- Persistent selection, removal/index renumbering and reconnect while held.
- Pause/focus suppression, neutral rearming and remapping while held.
- Shared ImGui/game device, every standard digital control, stick/deadzone,
  trigger conversion, original port 2 and reserved Guide/R3 behavior.
- Ten real database mappings: Xbox 360/One/Series USB, DS4 and DualSense USB/BT,
  Switch Pro USB/BT and 8BitDo Pro 2 BT. Only the GUID is replaced for attachment
  to the virtual test device; this does not emulate a Bluetooth transport.

All 19 controller tests pass, alongside 1,331 runtime tests (31 skipped),
175 Python tests and a native game pause/save/load/save/resume probe.
The Python launch test verifies preservation of Steam and SDL environment
variables across the child-process boundary. See `mgs-controllers-evidence.json`
in the source archive for hashes, logs and the scope of each result.

The following physical matrix remains pending; no row is marked passed by
virtual tests. For each available transport, test desktop direct SDL and Steam
Input in Big Picture/Gaming Mode, in both distribution formats.

| Hardware | Transport | Physical qualification |
| --- | --- | --- |
| Steam Deck LCD/OLED controls | integrated / Steam Input | pending |
| Xbox 360 | USB / receiver | pending |
| Xbox One / Series | USB / Bluetooth | pending |
| DualShock 4 / DualSense | USB / Bluetooth | pending |
| Switch Pro | USB / Bluetooth | pending |
| 8BitDo Pro 2 or equivalent | XInput / Switch modes | pending |
| Steam Controller | receiver / Bluetooth / Steam Input | pending |

For each row: import/launch with controller only; verify four face buttons,
D-pad, stick diagonals, bumpers, triggers, Start/Select and R3; open Steam's
overlay; switch original PS1 ports; pause/save/load/resume; disconnect with a
button and trigger held; reconnect; suspend/resume; return to the launcher.
With two devices attached, verify that UI and game use the chosen device and
that one physical press generates one game action. Verify the user's Steam
remap after every handoff. Attach build hash, SteamOS/SDL versions, transport
and observations to the result rather than describing a device as tested from
its name alone.

Rumble, gyro actions and Steam Input API action-set/glyph integration are not
implemented by this change. Basic Steam Input compatibility uses its gamepad
emulation path. These features must not be implied by the controller matrix.

## References consulted

- [Dusklight action bindings](https://github.com/TwilitRealm/dusklight/blob/main/src/dusk/action_bindings.cpp)
  route actions through Aurora's controller selection and suppress game actions
  while UI is open.
- [Aurora input ownership and preferences](https://github.com/encounter/aurora/blob/main/lib/input.cpp)
  centralize handles, hotplug and persistent port/device preferences. Our SDL2
  implementation is independent; no Dusklight/Aurora source was copied.
- [Valve's gamepad emulation guidance](https://partner.steamgames.com/doc/features/steam_controller/steam_input_gamepad_emulation_bestpractices)
  describes the supported Steam Input integration path and its glyph limits.
- [SDL's physical-position policy](https://wiki.libsdl.org/SDL2/SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS)
  and [Steam virtual identity handling](https://github.com/libsdl-org/SDL/blob/SDL2/src/joystick/SDL_joystick.c)
  inform normalization and recognition.
- [SDL_GameControllerDB](https://github.com/mdqinc/SDL_GameControllerDB)
  supplies the additional controller mapping data under the zlib license.
