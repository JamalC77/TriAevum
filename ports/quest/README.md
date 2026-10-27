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
| APK `assets/licenses/openxr_loader.txt`, tools `LICENSES/openxr_loader.txt` | The OpenXR loader's licence (Apache-2.0 OR MIT), `META-INF/LICENSE` of the same pinned AAR |
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
  is written as on any clean exit, then the process exits. A 15 s watchdog guards the XR part of
  every exit; the renderer's own shutdown (shader preparation drain, pipeline cache store) then runs
  under a 120 s one (`TRIAEVUM_XR_SHUTDOWN_WATCHDOG_S`). A watchdog exit logs
  `TRIAEVUM_XR_EXIT stage=watchdog` and exits with status 3.
- The immersive start writes `<ext>/logs/xr-start-failed.txt` ("pending ...") before it creates the
  OpenXR instance, replaces it with the reason on a known failure, and removes it only when the first
  game image reaches the headset (`TRIAEVUM_XR_FIRST_IMAGE`). Any failure before that (an error, a
  crash, a hang, or quitting before the first image) makes the next launch ask whether to play in VR
  or on the flat panel.

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
triggers are L2/R2. A Bluetooth gamepad is read through SDL alongside the Touch controllers (both
are ORed; with a pad connected, use the pad for the F1 window). If it does nothing, see
"Bluetooth pad diagnostics" below. The system's own recenter (hold the Oculus button) also re-places
the screen.

## Launch mode and options

`<ext>/triaevum-env.txt` holds `KEY=VALUE` lines applied with `setenv` before the runtime loads
(existing values win). The launcher reads `TRIAEVUM_QUEST_MODE` directly from it on every launch:

| Key | Values |
|---|---|
| `TRIAEVUM_QUEST_MODE` | `vr` (default), `panel` (flat panel, the Q2 behaviour), `ask` (two buttons) |
| `TRIAEVUM_XR_REFRESH` | display refresh request, default 72 (90 in the in-world mode; 72/80/90/120 on Quest 3) |
| `TRIAEVUM_XR_SCREEN_WIDTH_M`, `TRIAEVUM_XR_SCREEN_DISTANCE_M` | screen width (default 3.0) and distance (default 2.5) in metres |
| `TRIAEVUM_XR_PANEL_WIDTH`, `TRIAEVUM_XR_PANEL_HEIGHT` | render-target extent, default 1280x720 |
| `TRIAEVUM_XR_SWAPCHAIN_FORMAT` | force a `VkFormat` number for the quad (default: first of RGBA8/BGRA8 sRGB) |
| `TRIAEVUM_XR_SPLIT_FEATURES=0` | try the `VkPhysicalDeviceVulkan12Features` chain first (default: per-feature structs) |
| `TRIAEVUM_XR_RUN_UNFOCUSED=1` | keep the game and renderer running whatever the session state (system menu, headset off); for automated runs, see below |
| `TRIAEVUM_XR_RELEASE_WAIT=1` | wait for each copy before releasing the swapchain image (diagnostics) |
| `TRIAEVUM_XR_INPUT_LOG=1` | log Touch button edges, and the Bluetooth pad's raw SDL events, poll edges and Java key events (`TRIAEVUM_PAD`, below) |
| `TRIAEVUM_PAD_LOG` | Bluetooth pad diagnostics: on by default on Android (devices, hot-plug, selection); `2` adds the raw input lines like `TRIAEVUM_XR_INPUT_LOG=1`; `0` turns them off |

In immersive mode the game only advances while the session is FOCUSED (headset on and awake). With
the headset off or on a desk, `TRIAEVUM_DEV_LAB` commands (including its quit), `--frames` limits and
`--input-timeline` progress stop; only `--max-seconds` or a quit ends the run. Automated device runs
therefore either set `TRIAEVUM_XR_RUN_UNFOCUSED=1` or keep the headset awake with the proximity
override (`adb shell am broadcast -a com.oculus.vrpowermanager.prox_close`; undo with
`... -a com.oculus.vrpowermanager.automation_disable`).

`TriAevum.quest.host.json` (`{"maximum_surface_short_edge": 720}`) still sizes the flat panel's
surface. `QUEST_OPENXR=0 bash tools/quest/build_quest.sh` builds a panel-only APK for bisecting.

## In-world mode (first light, opt-in)

The life-size stereo world of `quest-research/inworld-spec.md` (W1 "first light"). It is **opt-in**
until it is verified on the headset; without it the immersive mode is the screen above, unchanged.
Add to `<ext>/triaevum-env.txt`:

```
TRIAEVUM_XR_VIEW=world
```

- Gameplay shows the world at life size (33 game units per metre) around you, in stereo, with your
  real head motion (6DoF) and a level horizon. When the world first appears (and after every
  recenter, door, scene change or return from the screen), Link stands 1.8 m ahead of you
  (`TRIAEVUM_XR_PLACE_M`) facing away from you: along your line to the centre of the play area
  (STAGE), or along your gaze when you stand within 0.6 m of it. With a small (stationary) boundary he
  stands outside it, which is intended. The game camera itself is still the game's (mode R is W2), so
  things behind that unseen camera can be missing and billboards face it.
- The left stick moves Link relative to that game camera; the right stick does nothing while the world
  shows (it becomes snap turn in W2). If Link runs past the leash (default the placement distance +
  2.5 m, 4.3 m; never inside the placement distance), a short blink puts him the placement distance
  ahead of you along his run (about one blink per 0.7 s of running, as in W1). The play-area rule
  (Link more than 2 m past the bounds) never fires before Link could be a leash away from the centre,
  so a 1 x 1 m boundary does not blink him early. Standing still far from the centre re-centres him
  with a blink only when the centre is at least the placement distance from you (never with a
  stationary boundary). Holding both stick clicks for ~1 s re-places the world around Link (60 ms
  buzz on both hands); in world mode it no longer moves the screen.
- The HUD (hearts, magic, rupees, the B/X/Y/ZL/ZR rings, the D-pad items) floats on a transparent
  panel 2.2 m wide, 2.5 m away and 8 degrees below eye level, so it sits in the periphery around
  Link. It moves with you and stays put while you look around; once you turn more than 35 degrees
  away it swings back in front of you smoothly (it also snaps in front of you whenever the world
  appears and after a recenter). It fades with the world and is not drawn over the screen (the screen
  shows the game's own HUD). `TRIAEVUM_XR_HUD=0` turns it off.
- Menus, pause, text boxes, F1, cutscenes, first person, crawl spaces, fixed-camera rooms and Epona
  fade (150 ms through black) to the screen of the Q3 mode, placed again in front of you each time;
  doors and scene changes go black, then place the world behind Link again.
- The display runs at 90 Hz (unless `TRIAEVUM_XR_REFRESH` is set) and each 30 Hz eye pair is shown
  for exactly 3 display frames. The game's frame-rate setting should stay Original (30).
- A system recenter (or a redrawn boundary) takes effect at the runtime's change time: the world goes
  black at once and comes back placed behind Link again; a new STAGE boundary is read again
  (`TRIAEVUM_XR_WORLD bounds_changed=WxD`).
- If the eye passes keep producing unusable draws (fallback draws in 30 consecutive eye frames), the
  screen is shown for 5 s (doubled on each repeat, at most 60 s; a scene change ends it), then the
  world is tried again (`TRIAEVUM_XR_VIEW render_safety=tripped|cleared`).
- While the mode is on, the effects that build their camera from the game camera are off for rendering
  (interactive grass, reflections and water reflections, AO, GI, TAA and temporal upscalers; MSAA,
  SMAA and FXAA stay): they would slide against the eye images. The settings themselves are kept
  (`TRIAEVUM_XR_VIEW effects_forced_off=...`). The FOV multiplier is 1 (`TRIAEVUM_XR_FOV`).

| Key (in-world) | Values |
|---|---|
| `TRIAEVUM_XR_VIEW` | `world` turns the mode on (`TRIAEVUM_XR_WORLD=1` too); anything else: the screen |
| `TRIAEVUM_XR_EYE_WIDTH`, `TRIAEVUM_XR_EYE_HEIGHT` | eye image extent, default 1024x1104 (the log shows the runtime's recommended size) |
| `TRIAEVUM_XR_WORLD_SCALE` | game units per metre, 25-45, default 33 |
| `TRIAEVUM_XR_PLACE_M` | head-to-Link distance of every placement, recenter, door, return from the screen, blink landing (and the minimum for an idle re-centre), 0.8-4.0 m, default 1.8 m (W1: 1.0 m) |
| `TRIAEVUM_XR_LEASH_M` | leash before a blink, 1-10 m, default `TRIAEVUM_XR_PLACE_M` + 2.5 m (4.3 m); raised to the placement distance + 0.5 m if set below it |
| `TRIAEVUM_XR_HUD=0` | no HUD quad (default on) |
| `TRIAEVUM_XR_HUD_WIDTH_M` | HUD quad width, 0.3-8 m, default 2.2 m (height from the 1280x720 panel: 1.24 m) |
| `TRIAEVUM_XR_HUD_DISTANCE_M` | HUD distance from the head, 0.5-10 m, default 2.5 m |
| `TRIAEVUM_XR_HUD_PITCH_DEG`, `TRIAEVUM_XR_HUD_FOLLOW_DEG` | HUD centre below the horizon (default -8) and the head turn before it follows (default 35) |
| `TRIAEVUM_XR_NS1_NATIVE=0` | the canonical replay follows the eye size instead of the native 400x240 |
| `TRIAEVUM_XR_WORLD_SPACE=local_floor` | skip STAGE (default: STAGE with bounds >= 1x1 m, else LOCAL_FLOOR, else LOCAL) |
| `TRIAEVUM_XR_SKY_PCS` | sky scope PCs (comma list); default: every atmosphere scope but precipitation and kankyo effects, when camera-centred |
| `TRIAEVUM_XR_UI_CINEMA=0` | do not switch to the screen for non-HUD PICA UI (bring-up) |
| `TRIAEVUM_XR_FORCE_WORLD=1` | ignore every screen trigger except the session and the player (bring-up only) |
| `TRIAEVUM_XR_VIEW_TRACE=1` | log the draw classes per scene (any mode, no rendering change; the W0 trace) |

Developer builds (`TRIAEVUM_DEV_LAB`): the `capture` op takes an optional `"view"`:

```json
{"id": 1, "op": "capture", "path": "/sdcard/Android/data/org.triaevum.quest/files/logs/left.bmp", "view": "left"}
{"id": 2, "op": "capture", "path": "/sdcard/Android/data/org.triaevum.quest/files/logs/right.bmp", "view": "right"}
{"id": 3, "op": "capture", "path": "/sdcard/Android/data/org.triaevum.quest/files/logs/panel.bmp", "view": "panel"}
```

`left`/`right` write the eye image (eye extent) of the frame's eye pair, `panel` the panel/HUD image
(the cinema picture, or in a world frame the HUD quad's image); without `view` a frame that drew a
pair gives the left eye, otherwise the panel. In a world frame the panel is the TopScreen HUD and
ImGui over transparent with premultiplied alpha: the BMP (24-bit, no alpha) shows its colour, which is
the HUD composited over black, and a second file `<path>.alpha.bmp` holds the alpha as grey (white =
opaque, black = transparent), with `TRIAEVUM_XR_CAPTURE alpha path= pixels= covered= opaque=
mean_alpha=` in the log (most of the panel must be `alpha 0`; `covered` is the HUD's footprint). An eye asked for in a frame that drew no pair (Cinema/Black, a failed
pose prediction, no presented sample) falls back to the panel, since its eye images belong to an
older frame (`TRIAEVUM_XR_CAPTURE view=panel requested=left eyes_drawn=0`). `--screenshot` captures
the left eye of frames that drew a pair.

In-world log lines (tag `TriAevumXR` and `native-stderr.log`):

| Line | Meaning |
|---|---|
| `TRIAEVUM_XR_VIEW_SELECT view=world` | the mode is on |
| `TRIAEVUM_XR_WORLD space=STAGE bounds=WxD ... recommended=WxH` | world space, play area, the runtime's eye size |
| `TRIAEVUM_XR_EVENT reference_space_change space= change_time=`, `TRIAEVUM_XR_RECENTER world=1 reason=space_change\|local_change`, `TRIAEVUM_XR_WORLD bounds_changed=WxD` | a system recenter or boundary change, applied at its change time |
| `TRIAEVUM_XR_SWAPCHAIN eye=WxH array=2`, `TRIAEVUM_XR_SWAPCHAIN fade=4x4`, `TRIAEVUM_XR_FADE ready`, `TRIAEVUM_XR_TARGETS slots=2 views=3` | eye and fade swapchains, renderer targets |
| `TRIAEVUM_XR_REFRESH ... requested=90` | the 90 Hz default |
| `TRIAEVUM_XR_NAMESPACES ns1=400x240 ... ns3=WxH ns4=WxH` | per-namespace extents (set once) |
| `TRIAEVUM_XR_CAMERA state=world\|cinema\|black owner=native blockers=0x.. (names)` | every state change and its reason |
| `TRIAEVUM_XR_LAYER request= / shown=` | the compositor's layer mode (fades) |
| `TRIAEVUM_XR_RIG event=place\|recenter\|door\|blink\|idle\|floor ... dist_m= epoch=` | rig changes; `event=status` every 5 s while the world shows |
| `TRIAEVUM_XR_STATS ... stereo_publications= stereo_copies= layer=world skipped_ticks=` | ~150 pairs and ~450 display frames per 5 s at 90 Hz; `skipped_ticks` = display periods the XR thread missed (the tick counts periods, not waits) |
| `TRIAEVUM_XR_TICK pairs= late_pairs= overruns= beats= lat_ticks= horizon_ms_mean= pacer_fallbacks= ... resyncs= held_frames= render_safety=` | the 3:1 lock (`beats=0`, `late_pairs` < 1%); `resyncs` = the pace pulled back to the display clock after it ran ahead (`TRIAEVUM_XR_TICK resync pace_was= latest= pace=`); `held_frames` = frames the guest was held (system menu, preparation screen) |
| `TRIAEVUM_XR_VIEW full= sky= ui= ... fallback= c_nonneg= mirrored= C= detR= detV= fallback_frames=` | draw classes of the eye passes (every 5 s); `c_nonneg` = perspective rows with C >= 0, left out as fallback |
| `TRIAEVUM_XR_VIEW render_safety=tripped hold_s= / cleared reason=hold\|scene` | the render safety's screen hold |
| `TRIAEVUM_XR_VIEW effects_forced_off=` / `TRIAEVUM_XR_FOV multiplier=1 setting=` | camera-dependent effects and the FOV multiplier the mode overrides |
| `TRIAEVUM_XR_FLAGS reason=edge flags1= flags2= setting=` | player flag and camera setting edges (decomp numbering check) |
| `TRIAEVUM_XR_CAPTURE view= requested= world_frame= eyes_drawn= w= h= path=` | a capture |
| `TRIAEVUM_XR_CAPTURE alpha path=<path>.alpha.bmp pixels= covered= opaque= mean_alpha=` | the HUD panel's alpha of a world-frame `panel` capture |
| `TRIAEVUM_XR_WORLD_SESSION scale= place_m= leash_m=` | the rig's knobs in use |
| `TRIAEVUM_XR_HUD select enabled=`, `TRIAEVUM_XR_SWAPCHAIN hud=1280x720 ... alpha=premultiplied` (or `hud=off`), `TRIAEVUM_XR_HUD config width_m= height_m= distance_m= pitch_deg= follow_deg=` | the HUD quad's setup |
| `TRIAEVUM_XR_HUD first_copy`, `TRIAEVUM_XR_HUD event=place reason=enter\|recenter\|follow yaw_deg= pos=` | the first HUD image; the quad snapped in front of the head (entering the world, a recenter) or began to follow a head turn (follows logged at most once a second) |
| `TRIAEVUM_XR_HUD enabled= placed= published= copies= skips= shown_frames= released= yaw_deg= following=` | every 5 s: `published` ~150 (one HUD per pair), `copies` ~= the pairs shown, `skips` ~0 (a HUD image not ready: the last HUD stays), `shown_frames` ~450 at 90 Hz while the world shows, `placed` = snaps and follows in the window |

How the HUD gets its alpha: the frame-start clear leaves the world frame's panel at (0, 0, 0, 0);
nothing scans out to it; the TopScreen HUD draws with colour `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` and (in
world mode) alpha `ONE, ONE_MINUS_SRC_ALPHA`, as ImGui's pipeline does, so the panel holds
premultiplied colour with the coverage in alpha. The pair carries it (`PublishStereo(..., panelValid)`),
the XR thread copies it with the eyes in one submit into the HUD swapchain (sRGB like the others, the
encoded bytes unchanged), and the quad uses `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`
without the unpremultiplied bit. Premultiplication happens in the sRGB-encoded space: exact for
opaque and transparent pixels and for dark translucent fills; antialiased edges of bright elements
come out slightly thinner than a linear-space blend would make them.

On a PC (no headset), `TRIAEVUM_XR_PREVIEW=left|right` shows one eye in the desktop window
(`TRIAEVUM_XR_PREVIEW_HEAD=identity` (default, must equal the mono picture), `sweep90`, `sweep180`, or
`yaw,x,y,z` relative to the game camera); `oot3d_xr_world_logic_tests` and `oot3d_xr_math_tests` run
the pure parts anywhere.

## Bluetooth pad diagnostics

The pad path: Android delivers the pad's key and motion events to the immersive activity's window
(the 2D panel never gets them: the Horizon shell takes them); SDL's Java side turns them into SDL
joystick events for a joystick its native side added (`SDLControllerManager.pollInputDevices`, polled
every 3 s and on the first press of an unknown device); the device manager
(`Ship::ConnectedPhysicalDeviceManager`) opens every SDL game controller at start and on each
`SDL_CONTROLLERDEVICEADDED`; `PollNativeA32Input` reads the port-0 pad it selects, if Controls
enables the controller, ORed with the Touch controllers. On Android the selection ranks pads with both
sticks first (SDL's automatic Android mapping also makes TV remotes and D-pad-only devices game
controllers, and the lowest instance used to win); a preferred controller set in Controls still wins.

Every step logs `TRIAEVUM_PAD` lines (logcat tag `TriAevumPad`, native lines also in
`native-stderr.log`), on by default on Android. For a test, add `TRIAEVUM_XR_INPUT_LOG=1` to
`triaevum-env.txt` (raw events and edges), start VR with the pad on, press A and B and move the left
stick, then read:

```powershell
& $ADB logcat -d -s TriAevumPad
& $ADB shell cat /sdcard/Android/data/org.triaevum.quest/files/logs/native-stderr.log | Select-String TRIAEVUM_PAD
```

| Line | What it decides |
|---|---|
| `TRIAEVUM_PAD java device reason=create\|added\|changed id= name="Xbox Wireless Controller" vendor=0x045E product=0x0B13 sources= gamepad= joystick= virtual= external= enabled= sdl_joystick=` | Android lists the pad for the app and SDL's Java side takes it for a joystick (`sdl_joystick=1`); absent or `enabled=0`: the system does not offer it to the app |
| `TRIAEVUM_PAD java key action=down code=KEYCODE_BUTTON_A device= source= handled= focus_view=` (input log) | the window receives the press and SDL's handler took it (`handled=1`); no such line while pressing: the events never reach the activity (routing, focus: `TRIAEVUM_PAD java window_focus=`) |
| `TRIAEVUM_PAD startup sdl=2.32.10 init_joystick= init_gamecontroller= hint_hidapi= env_hidapi= ...` | SDL's subsystems and the HIDAPI hint/env actually in effect |
| `TRIAEVUM_PAD event=joy_added\|controller_added\|joy_removed\|controller_removed\|controller_remapped` | SDL's hot-plug events as posted |
| `TRIAEVUM_PAD devices reason=startup\|device_event\|selection\|status joysticks= port0_pads=` then `TRIAEVUM_PAD joystick index= instance= name= guid= vendor= product= ... is_game_controller= has_mapping= virtual= opened_port0= full_gamepad=` | what SDL's native side knows: `joysticks=0` while Java says `sdl_joystick=1` = SDL never added it; `is_game_controller=0` = no mapping; `opened_port0=0` = the device manager did not open it |
| `TRIAEVUM_PAD opened instance= name= attached= axes= buttons= hats= full_gamepad=` | the pads the device manager holds |
| `TRIAEVUM_PAD port0 result= selected_instance= selected_name= controller_enabled= preferred_guid= preferred_serial= host_gui= touch_active=` | `result=selected` with the pad's name is the healthy case; `no_sdl_joysticks`, `none_opened`, `preferred_absent` (Controls names a controller that is not connected: choose Automatic), `none_selected`, `disabled_in_controls` (enable the controller in Controls) name the broken step; `selected` with another device's name = the wrong device won |
| `TRIAEVUM_PAD raw controller_button instance= button=a down=1`, `raw joy_button`, `raw joy_hat`, `raw joy_axis`, `raw controller_axis` (input log) | SDL turned the press into joystick events (any thread) |
| `TRIAEVUM_PAD raw key scancode=40 name="Return" down=1` (input log) | the press reached SDL but it had no opened joystick for the device, so it became a key (A = Return, B = Escape, D-pad = arrows) |
| `TRIAEVUM_PAD poll buttons=0x... pressed= released= instance= enabled= host_gui=` (input log) | the selected pad's buttons as the game's input poll reads them (bit i = SDL button i, 24-27 left stick, 30/31 triggers); edges here but no reaction in game = the Controls bindings |

The device table repeats every 30 s and after every hot-plug or selection change.

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

Stage game data as the shell user. `adb push` cannot create nested directories under
`Android/data/<pkg>` (`secure_mkdirs` EPERM) but `adb shell mkdir -p` can, and `run-as` cannot reach
`/sdcard/Android` at all; so create each directory first, then push the files (the method used on
the headset):

```powershell
$EXT = "/sdcard/Android/data/org.triaevum.quest/files"
$STAGE = (Resolve-Path "<staged game data>").Path
Get-ChildItem -Recurse -Directory $STAGE | ForEach-Object {
  & $ADB shell mkdir -p "$EXT/$($_.FullName.Substring($STAGE.Length).TrimStart('').Replace('', '/'))"
}
Get-ChildItem -Recurse -File $STAGE | ForEach-Object {
  & $ADB push $_.FullName "$EXT/$($_.FullName.Substring($STAGE.Length).TrimStart('').Replace('', '/'))"
}
```

The staged directory holds `TriAevum.quest.launch.json` and the inputs it names (`${profile_dir}`-relative).
On the next launch the launcher copies the module into `files/title/<sha256>/`, checks it and starts
the game (VR by default). `Try again` re-runs the checks without restarting the app.

Start the immersive activity directly (tests; the module path is the installed one):

```powershell
$EXT = "/sdcard/Android/data/org.triaevum.quest/files"
# One quoted device command: adb joins its arguments unescaped, so the glob must stay inside run-as.
$MOD = (& $ADB shell "run-as org.triaevum.quest sh -c 'ls files/title/*/libtriaevum_title_aot.so' | head -1").Trim()
& $ADB shell am start -n org.triaevum.quest/.TriAevumXrActivity --es org.triaevum.quest.TITLE_PLUGIN "/data/user/0/org.triaevum.quest/$MOD" --es org.triaevum.quest.LAUNCH_PROFILE "$EXT/TriAevum.quest.launch.json"
```

`$MOD` must look like `files/title/<sha256>/libtriaevum_title_aot.so`. The activity accepts a module
only under `files/title` (as `/data/user/0/...` or `/data/data/...`) and a profile only under `<ext>`;
otherwise logcat shows `Refusing runtime arguments outside the app's directories` with both
canonical paths and the allowed roots.

Logs:

```powershell
& $ADB logcat -s TriAevumXR TriAevum TriAevumPad SDL SDL/APP DEBUG AndroidRuntime
& $ADB shell cat /sdcard/Android/data/org.triaevum.quest/files/logs/native-stderr.log
& $ADB shell cat /sdcard/Android/data/org.triaevum.quest/files/data/runtime-state.json
& $ADB shell cat /sdcard/Android/data/org.triaevum.quest/files/logs/xr-start-failed.txt
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
| `TRIAEVUM_XR_EXIT stage=quit_event` ... `stage=xr_shutdown` ... `session_destroyed`, `TRIAEVUM_XR_WATCHDOG disarmed`, then `TriAevum: exiting the process (0)` | a clean quit |
| `TRIAEVUM_XR_EXIT stage=watchdog armed_at=... exit_code=3` | a shutdown wait hung and the watchdog ended the process |
| `TRIAEVUM_XR_START_MARKER removed` | the first image arrived; the next launch starts VR again without asking |
| `TRIAEVUM_XR_ERROR`, `TRIAEVUM_XR_WAIT_TIMEOUT`, `TRIAEVUM_XR_QUEUE_LOCK` | faults and stalls |

The flat panel logs `TRIAEVUM_SURFACE_LOST where=` and `TRIAEVUM_SURFACE_RECREATED w= h=` (tag
`TriAevum`) when it goes to the background and back: the game pauses instead of ending.

Probes (title-free, from `quest-tools-arm64.zip`; includes `quest_xr_logic_tests`):

```powershell
& $ADB push quest-tools-arm64 /data/local/tmp/
& $ADB shell "cd /data/local/tmp/quest-tools-arm64 && sh run_all.sh"
```
