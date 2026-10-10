# Windows alpha

Extract the entire ZIP and start **MGSRecomp.exe**. The native frontend supports
keyboard, mouse and SDL controllers. Import both original **USA v1.0** BIN/CUE
pairs. Python, SDL2, controller mappings and compiled Vulkan shaders are bundled;
game discs, executable data, asset caches and saves are not distributed.

Requires Windows 10 1903 or later / Windows 11, x64, and a Vulkan graphics driver.
The same game translation, original-asset renderer, 4K fixed 16:9 presentation,
per-pixel lighting, modern effects and distance-scaled ambient occlusion are used
as on Linux. The guest simulation retains its original timing and precision.

Add MGSRecomp.exe as a non-Steam game. `--big-picture` opens the controller
frontend and launches fullscreen. SDL2 supports XInput and HIDAPI; Steam Input
can supply a virtual Xbox controller. Hardware-specific Windows controller and
Steam-overlay acceptance is still required.

Data defaults to `%LOCALAPPDATA%\MGSRecomp`, settings to `%APPDATA%\MGSRecomp`.
An empty `portable.txt` beside the executable selects `userdata` within the
extracted folder. `--user-dir "D:\My MGS Profile"` selects an isolated profile.
Keep the extracted folder writable when using portable mode.

F5 saves state, F9 loads, F6/F7 select a slot, F11 toggles fullscreen. The host
menu, in-game memory cards, rotating backups and seamless disc change share the
Linux implementation. Prefer in-game Codec saves for long-term progression;
savestates may change compatibility in future builds.

For terminal commands (including import, verify, settings, backup, diagnostics):

```powershell
.\python\python.exe -X utf8 .\scripts\mgs_alpha.py --help
.\python\python.exe -X utf8 .\scripts\mgs_alpha.py paths
```

Launcher failures are logged at `%LOCALAPPDATA%\MGSRecomp\launcher.log`.
No administrator privileges or external Python installation are needed. The
optional old Tk settings editor is not included; use the native frontend.

## Build and provenance

On the Linux development checkout, with native translations already generated
from verified local discs and CMake/Ninja/glslc/Python 3.11+ available:

```sh
python3 scripts/build_windows.py --jobs 6
python3 scripts/package_windows.py data/releases/windows-new
```

`packaging/windows/dependencies.json` pins download URLs and SHA-256 hashes for
LLVM-MinGW, SDL2, embedded Python and Vulkan headers/import definitions. The
toolchain lives in `.local/windows-sdk`; Linux build products remain separate.
Windows uses Win32 fibers for guest thread continuations and
FlushFileBuffers/MoveFileExW for save publication. Shutdown uses a named event
and parent-process lifetime monitoring, including a shared profile lock until
the native game has joined and flushed saves.

`python3 scripts/build_windows.py --tests` also builds `mgsWindows_tests.exe`
and `mgsController_tests.exe`, with their DLLs beside them in
`build-windows/ps1Test`. The former runs the shared BIOS/continuation, config,
memory-card and savestate cases plus Windows atomic-publication tests; the
POSIX fork/fsync fault-injection suite remains a separate Linux check.

The Windows launcher adds a UTF-8 / per-monitor DPI / long-path manifest and a
16 MiB reserved host stack. Guest fibers keep the existing 4 MiB reserve.
LLVM-MinGW's huge generated dispatch initializer is built without function
inlining; the game shards, runtime and renderer keep their usual optimization.
PE timestamps are disabled for repeatable executable hashes.

Each package includes licenses, an exact file/hash inventory, dependency import
closure and build identity; the adjacent source archive excludes proprietary
inputs and generated game translations. Run `scripts/package_alpha.py --audit`
on the extracted directory to verify its inventory.

Validation is performed with the actual Windows PE executables through the
installed Proton Wine and physical Vulkan GPU. This is not native Windows
certification. Continuous campaign completion and full boss fights retain the
same unverified scope as the Linux alpha. Detailed evidence is maintained in
`docs/mgs-windows-evidence.json` in the development repository.

To repeat the private GPU comparison using the existing Linux fixtures:

```sh
python3 scripts/mgs_windows_check.py --package /path/to/extracted/package \
  --wine /path/to/wine --prefix "$PWD/.local/wine-windows" \
  --output data/validation/windows-new
```

The checks require a physical Vulkan device, verify asset upload/readback,
complete original meshes, AO, three true-4K captures per scene and eight
pause/save/restore cycles. `mgs_tas.py record/replay --wine /path/to/wine`
also exercises input routes and disc transitions with the actual Windows PE.
The installed Wine hides application Vulkan layers. Enabling Linux validation
underneath Wine (`--host-validation`) currently reports repeated physical-device
ID-properties extension VUIDs from the compatibility path; that failed attempt
is retained separately. Windows GPU results do not claim validation-layer
certification. The Linux renderer continues to pass hardware/synchronization
validation. Wine also reports zero for process handle counts: the soak report
marks handle-growth coverage unavailable rather than claiming zero leaks.
