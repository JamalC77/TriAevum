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
| Right stick click (tap) | In-game menu (as R3 on a pad); in-world while the world shows: hold ~0.7 s, stick centred |
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
| `TRIAEVUM_PAD_LOG` | Bluetooth pad diagnostics: on by default on Android (startup, hot-plug, selection changes; set `1` to repeat the table every 30 s); `2` adds the raw input lines like `TRIAEVUM_XR_INPUT_LOG=1`; `0` turns them off |

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
  stands outside it, which is intended. With `TRIAEVUM_XR_CAMERA=native` the game camera is the game's
  own, so things behind that unseen camera can be missing and billboards face it; mode R (below, the
  default from W2) makes it your head.
- With the native camera the left stick moves Link relative to that game camera (mode R: relative to
  where you look, below). While the world shows, the right stick moves him the same way whenever the
  left stick is idle or the left controller is off (one-handed play); with both pushed the larger
  deflection wins (`TRIAEVUM_XR_INPUT stick_source=left|right` on each change). It is never the game's
  C-stick there. In mode R (and `held`) the right stick is snap turn and D-pad flicks instead and moves
  Link only while the left controller is off (`RightStickMove::WhenLeftInactive`, inworld-spec §7).
  The in-game menu then opens with a deliberate hold: right stick click held ~0.7 s with the right
  stick centred (short buzz, right hand); a click while steering or a short click does nothing. On the screen (menus, cinema) a right stick click tap is
  the in-game menu as before. If Link runs past the leash (default the placement distance +
  2.5 m, 4.3 m; never inside the placement distance), a short blink puts him the placement distance
  ahead of you along his run (about one blink per 0.7 s of running, as in W1). The play-area rule
  (Link more than 2 m past the bounds) never fires before Link could be a leash away from the centre,
  so a 1 x 1 m boundary does not blink him early. Standing still far from the centre re-centres him
  with a blink only when the centre is at least the placement distance from you (never with a
  stationary boundary). Holding both stick clicks for ~1 s re-places the world around Link (60 ms
  buzz on both hands); in world mode it no longer moves the screen.
- The HUD (hearts, magic, rupees, the B/X/Y/ZL/ZR rings, the D-pad items) floats on a transparent
  panel 1.05 m wide, 1.5 m away (0.3 m nearer than Link, so it never looks behind him) and 8 degrees
  below eye level, about 39 degrees across, so the hearts and rings sit around Link like a TV's
  corners. It moves with you and stays put while you look around; once you turn about 21 degrees
  away (the point where the panel's far edge reaches 40 degrees from your gaze, inside the Quest 3's
  field of view) it swings back in front of you smoothly (it also snaps in front of you whenever the world
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
| `TRIAEVUM_XR_HUD_DISTANCE_M` | HUD distance from the head, 0.5-10 m; default the placement distance minus 0.3 m, clamped to 1.0-2.5 m (1.5 m at the default `TRIAEVUM_XR_PLACE_M=1.8`) |
| `TRIAEVUM_XR_HUD_WIDTH_M` | HUD quad width, 0.3-8 m; default 0.70 x the HUD distance (1.05 m at 1.5 m, ~39 degrees; height from the 1280x720 panel: 0.59 m) |
| `TRIAEVUM_XR_HUD_PITCH_DEG`, `TRIAEVUM_XR_HUD_FOLLOW_DEG` | HUD centre below the horizon (default -8) and the head turn before it follows; the follow default is 40 degrees minus the quad's half-angle (~20.7 at the default size), so its far edge never passes 40 degrees from the gaze |
| `TRIAEVUM_XR_NS1_NATIVE=0` | the canonical replay follows the eye size instead of the native 400x240 |
| `TRIAEVUM_XR_WORLD_SPACE=local_floor` | skip STAGE (default: STAGE with bounds >= 1x1 m, else LOCAL_FLOOR, else LOCAL) |
| `TRIAEVUM_XR_SKY_PCS` | sky scope PCs (comma list); default: every atmosphere scope but precipitation and kankyo effects, when camera-centred |
| `TRIAEVUM_XR_UI_CINEMA=0` | do not switch to the screen for non-HUD PICA UI (bring-up) |
| `TRIAEVUM_XR_FORCE_WORLD=1` | ignore every screen trigger except the session and the player (bring-up only) |
| `TRIAEVUM_XR_VIEW_TRACE=1` | log the draw classes per scene (any mode, no rendering change; the W0 trace) |

Developer builds: `TRIAEVUM_DEV_LOADOUT=sword,shield,slingshot,nuts,sticks,hearts:6` (or `full_child`)
starts Link with gear, applied at gameplay start, after a `TRIAEVUM_DEV_WARP` arrival and after a
continue (vocabulary and log lines: `docs/TRIAEVUM_DEVELOPER_WARP.md`, "Starting loadout"). With only
the right Touch controller awake, Start, Select, X and Y (the loadout's sticks and nuts) and the
recenter chord are out of reach (they are left-controller inputs): wake or pair the left controller
(a press on any of its buttons) for those; B (sword), A, the right grip (shield) and the right
trigger (slingshot) work one-handed.

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
| `TRIAEVUM_XR_HUD select enabled=`, `TRIAEVUM_XR_SWAPCHAIN hud=1280x720 ... alpha=premultiplied` (or `hud=off`), `TRIAEVUM_XR_HUD config width_m= height_m= distance_m= pitch_deg= follow_deg= edge_deg= place_m=` | the HUD quad's setup |
| `TRIAEVUM_XR_HUD first_copy`, `TRIAEVUM_XR_HUD event=place reason=enter\|recenter\|follow yaw_deg= pos=` | the first HUD image; the quad snapped in front of the head (entering the world, a recenter) or began to follow a head turn (follows logged at most once a second) |
| `TRIAEVUM_XR_HUD enabled= placed= published= copies= skips= shown_frames= released= yaw_deg= following=` | every 5 s: `published` ~150 (one HUD per pair), `copies` ~= the pairs shown, `skips` ~0 (a HUD image not ready: the last HUD stays), `shown_frames` ~450 at 90 Hz while the world shows, `placed` = snaps and follows in the window |

How the HUD gets its alpha: the frame-start clear leaves the world frame's panel at (0, 0, 0, 0);
nothing scans out to it; the TopScreen HUD draws with colour `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` and (in
world mode) alpha `ONE, ONE_MINUS_SRC_ALPHA`, as ImGui's pipeline does, so the panel holds
premultiplied colour with the coverage in alpha. The pair carries it (`PublishStereo(..., panelValid)`),
the XR thread copies it with the eyes in one submit into the HUD swapchain (sRGB like the others, the
encoded bytes unchanged), and the quad uses `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`
without the unpremultiplied bit. Premultiplication happens in the sRGB-encoded space while the
compositor decodes before it blends: exact for opaque and transparent pixels, but antialiased edges
and translucent pixels of bright elements come out darker than intended (white at alpha 0.5 shows as
0.21 linear instead of 0.5), a thin dark fringe around hearts, ring icons and ImGui text. The fix
(a small pass that converts to linear-space premultiplied alpha instead of the plain copy) belongs to
the full W4 HUD.

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
This rank applies to every Android mode (the Q3 screen mode, world mode and the 2D panel app), on
purpose: they share the selection, and the old lowest-instance rule fails the same way in each. It
only changes the outcome when a two-stick pad and a remote-like or D-pad-only device are attached
together; pick the other device in Controls to play with it.

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

The device table is logged at startup and after every hot-plug or selection change; with
`TRIAEVUM_PAD_LOG=1`/`2` or `TRIAEVUM_XR_INPUT_LOG=1` set it also repeats every 30 s (`reason=status`).
Its fields are read under SDL's joystick lock, so a line never mixes two devices during a hot-plug.

## In-world mode R: the game camera is your head (W2)

With `TRIAEVUM_XR_VIEW=world` and the TopScreen UI profile (Forge's default), the game camera follows
your head from W2 on (`quest-research/inworld-spec.md` §4-§7): every Camera_Update of the main camera
writes your head, predicted for the frame's display time, through TopScreen's phased Camera_Update
replacement. The game then culls, picks Z-targets, turns billboards and pans audio for where you stand
and look, so you can walk a full circle around Link and look behind you.

- The camera's FOV for culling is 130 deg (`TRIAEVUM_XR_CULL_FOV`), unclamped; the picture is still built
  per eye from your eye poses, so the camera write never judders. The written pitch stops at +-60 deg;
  looking further up or down (past about 70 deg) widens the FOV just enough to keep the edge of your
  view inside it, up to 150 deg. Actors within 4 m of your head are drawn even when the culling volume
  rejects them (through the actor light budget). When the game stops updating its camera (the pause
  screen) the picture on the screen keeps the game's own FOV.
- The native camera takes over (Held: the world still shows from your rig) for cutscenes, item gets,
  secondary cameras, the TopScreen-blocked camera settings (0x14, 0x15, 0x23, 0x40, 0x46), scene 0x45, a
  camera status other than 7 and player states 0x20/0x200. Cutscenes, item gets and secondary cameras
  also freeze the rig; such a scene goes to the screen only when its camera moves more than 2 m or
  45 deg within 1 s (it stays on the screen until the scene ends), or for a cutscene index >= 0xFFF0.
  The camera's first swing from where you stood to its own view is not counted: the 1 s window opens
  once the view has rested for 5 updates, or 0.75 s after the scene began.
- Talking and shop browsing stay head-owned with the rig frozen while the world shows. A text box,
  though, still sends the view to the screen after 0.3 s (the non-HUD UI rule, until W4's in-world
  text; `TRIAEVUM_XR_UI_CINEMA=0` keeps the world), so most conversations end with a return from the
  screen and a fresh placement behind Link. First person, crawl spaces, fixed cameras, Epona, doors
  and menus go to the screen or black as before.
- Z-targeting stays head-owned: the target choice follows your head.
- The left stick moves Link relative to where you look (in the head-owned and the Held states alike): the
  direction is latched from your head when you push the stick and follows your head slowly while you
  hold it (`TRIAEVUM_XR_MOVE_REF`); Z-targeting latches it at the L press. Talking, shops, first person,
  crawling and the ocarina keep the game's own stick.
- Right stick (with the left controller on): left/right turns smoothly around your head (120 deg/s,
  `TRIAEVUM_XR_TURN_SPEED`; `TRIAEVUM_XR_TURN=45` snaps instead, instantly, with a short buzz); flick up/down for
  D-pad up/down (TopScreen's first-person view is D-pad up). It never moves Link then. With the left
  controller off or asleep it moves Link instead and takes no flicks (one-handed play). The in-game
  menu is still the deliberate right stick click hold (~0.7 s, stick centred).
- Walking your head into a wall darkens the world in proportion to the depth (black at 0.3 m); open-air
  fences and railings do not, and your head below Link's floor goes black. The darkening stays while
  the native camera holds the view (talks, chests) and through a snap turn; it ends when your head is
  out again (tested from where it last was clear, and every other update from Link's head, so it never
  sticks once you are back in Link's open space). A blink (Link outran the leash, the idle re-centre,
  a floor change) that would put your head inside a wall re-places you behind Link with the probes
  instead (`TRIAEVUM_XR_GEOMETRY reseed=hard result=hit`, then `TRIAEVUM_XR_PROBE reason=blink_geometry`).
- Placements (first, recenter, doors, scene changes, the screen's end) stay black for up to 0.6 s while
  up to five collision probes from Link's head (behind, behind-left and -right 35 deg, then left and
  right) choose where you stand: the farthest clear one minus 0.3 m, capped at the placement distance
  (`TRIAEVUM_XR_PLACE_M`), at least 0.7 m. The probes run even while the game holds its camera after a
  load or a door (the walk-in, a camera status other than 7, an item get: `held_bypassed=` on the
  `TRIAEVUM_XR_PROBE` line); only first person and the other screen rows hold them off.
- Every rig change (turns, recenters, placements, blinks, following Link, the floor) happens at the
  game's camera update, before your head is written, so the frame culled from your head is the frame
  drawn with that rig.
- No comfort vignette by default. `TRIAEVUM_XR_VIGNETTE=1` darkens the edge of view while the anchor or
  the floor glides (running, landing, swimming, diving) or during a smooth turn.
- `TRIAEVUM_XR_CAMERA=native` runs W1 for A/B on the device: the native camera, no stick rotation (even
  with `TRIAEVUM_XR_MOVE_REF` set), no snap turns or D-pad flicks (the right stick moves Link while the
  left stick is idle, as in W1) and no vignette. `held` runs mode L (the native camera with the stick
  rotation, snap turns and flicks). Without the TopScreen profile the mode is `held`
  (`TRIAEVUM_XR_WORLD camera=held reason=ui_profile`).

| Key (mode R) | Values |
|---|---|
| `TRIAEVUM_XR_CAMERA` | `r` (default), `native` (W1), `held` (mode L) |
| `TRIAEVUM_XR_CULL_FOV` | the head camera's FOV for culling, 60-150, default 130 |
| `TRIAEVUM_XR_MOVE_REF` | `head-latched` (default), `head`, `line` (Link minus head beyond 0.6 m), `native` (no rotation) |
| `TRIAEVUM_XR_TURN` | `smooth` (default) or snap degrees (10-90, instant, no blink) |
| `TRIAEVUM_XR_TURN_SPEED` | smooth turn rate, 30-360 deg/s, default 120 |
| `TRIAEVUM_XR_TETHER` | `glide` (default: the world slides with running Link, keeping him within the placement distance of the play-area centre) or `leash` (W1's blink when he outruns the leash) |
| `TRIAEVUM_XR_VIGNETTE=1` | the comfort vignette during glides and smooth turns (off by default) |
| `TRIAEVUM_XR_SEATED=1` | seated: LOCAL_FLOOR, the floor offset from your seated eye height at each placement, leash 2.5 m |
| `TRIAEVUM_XR_INPUT_LOG=1` | also log `TRIAEVUM_XR_STICK theta= yaw_cam= yaw_dir= psi_ref=` (latches and every 0.5 s) |

| Line | Meaning |
|---|---|
| `TRIAEVUM_XR_WORLD camera=r reason= cull_fov= move_ref= turn= flicks= vignette=` | the camera mode at start (`camera=native`: `stick=0 flicks=0 vignette=0`) |
| `TRIAEVUM_XR_CAMERA owner=vr\|vr_probe\|native held=0x.. (names) probe_held=0x.. rig_frozen= talking= z_target= layer= pending= wall_fade=` | every owner change at Camera_Update (`owner=` is also on the `state=` lines) |
| `TRIAEVUM_XR_CAMWRITE first eye= at= yaw= pitch= fov= head_t=` | the first head write after each owner change |
| `TRIAEVUM_XR_CAMWRITE updates= owned= owned_pct= probes= held= eye= yaw= pitch= fov= fov_max= fov_widened= fov_hooks= fov_loaded= fov_restored= head_failures= seq_lag= wall_fade_updates= reseeds= rescues= reseed_hits= always_candidates= always_granted= turns=` | every 5 s: `owned_pct` > 95 in gameplay; `fov_hooks` > 0 and `fov=130` (the FovScalar returned the cull FOV; `fov_loaded` is play+0x198 as the native load read it; `fov_max` > 130 after looking steeply up or down); `seq_lag` = visual frames between the camera write and the frame drawn (0 expected) |
| `TRIAEVUM_XR_CAMWRITE fov_restored loaded=130.0 native=` | the game drew without a camera update (pause) and the native FOV replaced the stale cull FOV |
| `TRIAEVUM_XR_HELD entries=reason:count,...` | Held entries by reason (5 s) |
| `TRIAEVUM_XR_CAMERA cutscene_camera_settled settle_s= by=still\|time`, `TRIAEVUM_XR_CAMERA cutscene_camera_moved moved_m= turned_deg= settle_s= by=` | the hand-over ended (the motion window opens); a Held cutscene went to the screen |
| `TRIAEVUM_XR_PROBE start reason= length_m=`, `TRIAEVUM_XR_PROBE reason= choice=back\|back_left\|back_right\|left\|right\|fallback dist_m= clear_m=b,bl,br,l,r probes= timeout= wait_s= held_bypassed=0x.. (names) held_updates=` | placement probes; `TRIAEVUM_XR_RIG event=place ... probed=1`; `timeout=0` and `probes>=3` expected after doors and loads |
| `TRIAEVUM_XR_GEOMETRY fade= cause=q\|link\|below_floor\|reseed\|place\|layer q= head=` | the wall fade starts or ends; `TRIAEVUM_XR_OVERLAY wall_fade_frames= vignette_frames=` every 5 s |
| `TRIAEVUM_XR_GEOMETRY reseed=soft\|hard result=clear\|hit p_u= fade=` | the head test from Link's head after Held updates (soft) or a blink (hard; a hit re-places, `reason=blink_geometry`) |
| `TRIAEVUM_XR_FLICK axis=x\|y dir=`, `TRIAEVUM_XR_TURN dir= step_deg= psi= wall_fade=`, `TRIAEVUM_XR_RIG event=turn` | right stick flicks and snap turns |
| `TRIAEVUM_XR_INPUT right_stick=flicks\|move`, `TRIAEVUM_XR_INPUT flicks=0\|1 left_active=` | the right stick's job at start (by camera mode) and whenever the left controller comes or goes |
| `TRIAEVUM_XR_STICK rotations= bypassed= theta_abs_max= mode=` | stick rotation (5 s) |
| `TRIAEVUM_XR_VIGNETTE strength=`, `TRIAEVUM_XR_SWAPCHAIN vignette=64x64 array=8`, `TRIAEVUM_XR_VIGNETTE ready` | the vignette |

On a PC, `TRIAEVUM_XR_PREVIEW=left` with `TRIAEVUM_XR_PREVIEW_HEAD=circle:1.2,8` (and the TopScreen
profile) drives the game camera from a head walking a 1.2 m circle around Link in 8 s, looking at him;
`TRIAEVUM_XR_PREVIEW circle deg=0..315` marks each 45 deg for screenshots. `oot3d_xr_camera_logic_tests`
runs the pure parts (camera write, owner rows, the cutscene-camera test, head inside geometry, probes,
stick reference and rotation, R_always, the circle) anywhere.

Implementation notes (where W2 differs from the spec text):

- The frame state still travels by value with each host frame (W1 note 11). With the hook running,
  every rig change (turns, recenters, the probed placement, the leash and idle blinks, glides, the
  floor) is applied at Camera_Update before the head is computed, and the stamp publishes the rig with
  its epoch; `seq_lag` measures whether the drawn frame is the one the camera write belongs to. The
  sequence ring returns with W3's interpolation.
- The owner rows are evaluated on the guest's own flags at every Camera_Update, so first person, doors
  and the other screen rows hand the camera back in the same update, a frame before the layer changes.
- Camera status != 7 is a Held row (TopScreen's own blocker reads the same offset); it is still not a
  screen trigger by itself.
- Probe updates are head-owned updates that keep the camera where it is (no head write, native FOV):
  the native camera skips those three to five updates under black. They run over every Held row but
  first person and the other screen rows, a missing player, another camera and mode L.
- The head-in-geometry penetration is measured along the segment: BgCheckInfo extends the ray 8 u past
  its end, so a wall just ahead of the head's motion would otherwise fade the world 24 cm from it.
- q is not reset on turns (they pivot on the head). After a blink, after Held updates and every other
  update while q's test fails, the segment starts at Link's head instead (L + 50/38 u); that test only
  ever clears a fade (Link line of sight never darkens the world, §4.4), except after a blink, where a
  hit starts a probed placement.
- Above +-70 deg of head pitch the cull FOV is widened (`CullFovForPitch`, Quest 3 eye edge taken as
  55 deg), capped at 150 deg.
- The stick logic is in `oot3d_xr_camera_logic.h` (not `quest_xr_input_logic.h`): it runs in
  `SampleGuestRefreshInput`, which the PC build compiles too. The `controller` reference needs the aim
  pose (G11, W5). The ocarina bypass uses TopScreen's ocarina state and flags2 0x08000000. In a mirrored
  (Master Quest) frame theta is computed in the VR world [unverified until W5].
- `TRIAEVUM_XR_CAMERA=native` is the W1 baseline: no flicks (`SetWorldFlicks(false)`: the right stick
  keeps the one-handed movement of `RightStickMove::WhenLeftIdle`), no turns, no vignette, no stick
  rotation; `TRIAEVUM_XR_MOVE_REF` is ignored there.
- The R_always light budget walk runs while the head owns the camera even with Extended draw distance
  off (with the light arena's overflow relief). The pop-in counter of the W2 exit is approximated by
  `always_candidates`/`always_granted`.

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
