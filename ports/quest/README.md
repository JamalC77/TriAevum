# TriAevum on Meta Quest 3

Milestone Q3: `org.triaevum.quest` is a Meta Quest hybrid app. A 2D launcher panel installs the
side-loaded title, then starts the game **immersive** (Quest VR category): the game image appears on
a world-locked virtual screen, about 3 m wide and 2.5 m in front of you at head height, in a dark
room. The flat panel of Q2 stays available as a fallback.

The APK is title-free and built by `.github/workflows/quest.yml` (`tools/quest/build_quest.sh`).
The title module (`libtriaevum_title_aot.so`) and the game data are built and staged privately on
the player's PC from their own Forge installation and side-loaded with adb. Never commit or upload
them.

## Layout

| Path | What |
|---|---|
| `app/` | `TriAevumLauncherActivity` (title install, checks, VR/panel choice), `TriAevumSdlActivity` (shared SDL base), `TriAevumXrActivity` (immersive, process `:xr`), `TriAevumGameActivity` (flat panel), `TitleInstaller`, `QuestLaunchMode` |
| `runtime/` | CMake wrapper for `libTriAevum.so`: the shared `ports/android/runtime` host, `quest_host.cpp`, the OpenXR loader (Khronos AAR 1.1.63, hash-pinned) |
| `runtime/xr/` | The immersive mode: OpenXR instance/session/frame thread (`quest_xr_runtime.cpp`), the quad swapchain and copy (`quest_xr_vulkan.cpp`), Touch input (`quest_xr_input.cpp`), pure chord/placement logic and its tests |
| `../../tools/quest/title_abi.py` | Title-side source closure guard and `title-abi.json` writer |
| `../../tools/quest/stage_quest.py` | Stages APK libraries and `quest-tools-arm64.zip`; checks AArch64, 16 KiB alignment, NEEDED |
| `../../tools/quest/synthetic_title.py` | A one-function synthetic ABI-v2 title for the probes (no game code) |

`<ext>` below is `/sdcard/Android/data/org.triaevum.quest/files`.

## How the immersive mode works

- The renderer creates its Vulkan instance and device through `XR_KHR_vulkan_enable2` with two queues
  in the graphics family: queue 0 renders (and NRI), queue 1 is the OpenXR binding.
- The game renders its normal mono frame (TopScreen UI profile, 1280x720) into two private targets.
  An XR thread runs `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame` at the compositor rate (72 Hz),
  copies each GPU-complete game frame into a quad-layer swapchain on queue 1, and re-submits the
  last image every display frame. The game keeps its own 30 Hz pacer.
- The game pauses while the session is not focused (system menu, headset off): no guest time passes,
  the queued audio is flushed, the screen keeps the last image. Putting the headset back on resumes.
- Quitting from the system menu ends the game loop through `SDL_QUIT`; `--output runtime-state.json`
  is written as on any clean exit, then the process exits (a 15 s watchdog guards every exit).
- If the immersive start fails, the runtime writes `<ext>/logs/xr-start-failed.txt` and the next launch
  asks whether to play in VR or on the flat panel.

## Controls (Touch controllers)

| Touch | 3DS |
|---|---|
| Left stick | Circle Pad |
| Right stick | C-Stick (camera / aim, as on a Bluetooth pad) |
| A / B / X / Y | A / B / X / Y |
| Left grip / right grip | L / R |
| Left trigger / right trigger | ZL / ZR |
| Left menu button (≡) | Start |
| Left stick click (tap) | Select |
| Right stick click (tap) | In-game menu (as R3 on a pad) |
| Hold both stick clicks ~1 s | Recenter the screen in front of you (short buzz on both hands) |
| Hold left stick click + press ≡ | Open/close the F1 settings window (short buzz, left hand) |

In the F1 window: left stick or its directions move, A selects, B goes back, grips switch tabs,
triggers are L2/R2. Bluetooth gamepads keep working through SDL alongside the Touch controllers
(with a pad connected, use the pad for the F1 window). The system's own recenter (hold the Oculus
button) also re-places the screen.

## Launch mode and options

`<ext>/triaevum-env.txt` holds `KEY=VALUE` lines applied with `setenv` before the runtime loads
(existing values win). The launcher reads `TRIAEVUM_QUEST_MODE` directly from it on every launch:

| Key | Values |
|---|---|
| `TRIAEVUM_QUEST_MODE` | `vr` (default), `panel` (flat panel, the Q2 behaviour), `ask` (two buttons) |
| `TRIAEVUM_XR_REFRESH` | display refresh request, default 72 (72/80/90/120 on Quest 3) |
| `TRIAEVUM_XR_SCREEN_WIDTH_M`, `TRIAEVUM_XR_SCREEN_DISTANCE_M` | screen width (default 3.0) and distance (default 2.5) in metres |
| `TRIAEVUM_XR_PANEL_WIDTH`, `TRIAEVUM_XR_PANEL_HEIGHT` | render-target extent, default 1280x720 |
| `TRIAEVUM_XR_SWAPCHAIN_FORMAT` | force a `VkFormat` number for the quad (default: first of RGBA8/BGRA8 sRGB) |
| `TRIAEVUM_XR_SPLIT_FEATURES=0` | try the `VkPhysicalDeviceVulkan12Features` chain first (default: per-feature structs) |
| `TRIAEVUM_XR_RUN_UNFOCUSED=1` | keep the game running while the session is only visible (diagnostics) |
| `TRIAEVUM_XR_RELEASE_WAIT=1` | wait for each copy before releasing the swapchain image (diagnostics) |
| `TRIAEVUM_XR_INPUT_LOG=1` | log Touch button edges |

`TriAevum.quest.host.json` (`{"maximum_surface_short_edge": 720}`) still sizes the flat panel's
surface. `QUEST_OPENXR=0 bash tools/quest/build_quest.sh` builds a panel-only APK for bisecting.

## Title pairing

The title module runs its own copy of `NativeA32Memory` and the VFP support code against the host's
objects, so it must be built from the same title-side sources and the same NDK (29.0.14206865,
`c++_shared`). `title_abi.py check` fails the build if those 20 files differ from v0.8.0-beta.1.2.
The APK carries `assets/title-abi.json`; the title build writes the same file beside the `.so`
(`title_abi.py write`), and `TitleInstaller` refuses a module whose `title-abi.json` does not pair.

## Device steps

```powershell
$ADB = "$env:LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"
& $ADB install -r TriAevum-Quest-arm64.apk
# First launch from the headset's app library (Unknown Sources) creates <ext> and its writable
# directories (incoming, install, data/..., logs) and shows "No title module is installed".
& $ADB shell am start -n org.triaevum.quest/.TriAevumLauncherActivity
& $ADB push <title build>/libtriaevum_title_aot.so <title build>/title-abi.json /sdcard/Android/data/org.triaevum.quest/files/incoming/
```

Stage game data as the app user (directories adb creates are read-only to the app):

```powershell
& $ADB push quest-stage.tar /data/local/tmp/
& $ADB shell "cat /data/local/tmp/quest-stage.tar | run-as org.triaevum.quest tar -xf - -C /sdcard/Android/data/org.triaevum.quest/files"
& $ADB shell rm /data/local/tmp/quest-stage.tar
```

The tar holds `TriAevum.quest.launch.json` and the staged inputs it names (`${profile_dir}`-relative).
On the next launch the launcher copies the module into `files/title/<sha256>/`, checks it and starts
the game (VR by default). `Try again` re-runs the checks without restarting the app.

Start the immersive activity directly (tests; the module path is the installed one):

```powershell
$EXT = "/sdcard/Android/data/org.triaevum.quest/files"
$MOD = (& $ADB shell run-as org.triaevum.quest sh -c "ls files/title/*/libtriaevum_title_aot.so").Trim()
& $ADB shell am start -n org.triaevum.quest/.TriAevumXrActivity --es org.triaevum.quest.TITLE_PLUGIN "/data/data/org.triaevum.quest/$MOD" --es org.triaevum.quest.LAUNCH_PROFILE "$EXT/TriAevum.quest.launch.json"
```

Logs:

```powershell
& $ADB logcat -s TriAevumXR TriAevum SDL SDL/APP DEBUG AndroidRuntime
& $ADB shell run-as org.triaevum.quest cat /sdcard/Android/data/org.triaevum.quest/files/logs/native-stderr.log
& $ADB shell run-as org.triaevum.quest cat /sdcard/Android/data/org.triaevum.quest/files/data/runtime-state.json
```

What the immersive log shows (tag `TriAevumXR`, also in `native-stderr.log`):

| Line | Meaning |
|---|---|
| `TRIAEVUM_XR_MODE xr=1` | the XR activity selected the immersive renderer |
| `TRIAEVUM_XR_INSTANCE`, `TRIAEVUM_XR_DEVICE chain=split xr=0 vk=0`, `TRIAEVUM_XR_SESSION created`, `TRIAEVUM_XR_SWAPCHAIN`, `TRIAEVUM_XR_BINDINGS`, `TRIAEVUM_XR_TARGETS` | bring-up |
| `TRIAEVUM_XR_STATE from= to=FOCUSED`, `TRIAEVUM_XR_REFRESH`, `TRIAEVUM_XR_PERF`, `TRIAEVUM_XR_THREAD role= result=0` | session running |
| `TRIAEVUM_XR_SCREEN ... reason=first`, `TRIAEVUM_XR_FIRST_IMAGE` | the screen is placed and shows the game |
| `TRIAEVUM_XR_STATS` (every 5 s) | `new_frames` ~150 per 5 s at 30 Hz, `display_frames` ~360 at 72 Hz, `incomplete_skips`, `wait_frame_ms_max`, `guest_refreshes_while_paused=0` |
| `TRIAEVUM_XR_PAUSE reason=VISIBLE` / `TRIAEVUM_XR_RESUME paused_ms=` | system menu or headset off, and back |
| `TRIAEVUM_XR_RECENTER`, `TRIAEVUM_XR_F1 toggled`, `TRIAEVUM_XR_PROFILE` | chords and the controllers' interaction profile |
| `TRIAEVUM_XR_EXIT stage=quit_event` ... `stage=xr_shutdown` ... `session_destroyed`, then `TriAevum: exiting the process (0)` | a clean quit |
| `TRIAEVUM_XR_ERROR`, `TRIAEVUM_XR_WAIT_TIMEOUT`, `TRIAEVUM_XR_QUEUE_LOCK` | faults and stalls |

The flat panel logs `TRIAEVUM_SURFACE_LOST where=` and `TRIAEVUM_SURFACE_RECREATED w= h=` (tag
`TriAevum`) when it goes to the background and back: the game pauses instead of ending.

Probes (title-free, from `quest-tools-arm64.zip`; includes `quest_xr_logic_tests`):

```powershell
& $ADB push quest-tools-arm64 /data/local/tmp/
& $ADB shell "cd /data/local/tmp/quest-tools-arm64 && sh run_all.sh"
```
