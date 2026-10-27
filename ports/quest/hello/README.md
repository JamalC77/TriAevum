# TriAevum XR Test (Quest milestone Q1)

A title-free immersive app, `org.triaevum.quest.hello`, that proves the stack
the Quest port will use before any TriAevum code depends on it:

- SDL2 2.32.10 `SDLActivity` (the same activity base and SDL pin as
  `ports/android`, which the real port extends), immersive-only manifest with
  the Quest entries from Xelee.
- Khronos `openxr_loader_for_android` 1.1.63 (SHA-256 pinned in
  `native/CMakeLists.txt`).
- Vulkan 1.3 through `XR_KHR_vulkan_enable2`: instance, physical device and
  device created by the runtime, two queues in the graphics family; the XR
  binding and this app's rendering use queue 1, queue 0 is left for the
  renderer of the real port.
- A dedicated XR thread (`RENDERER_MAIN` via `XR_KHR_android_thread_settings`)
  renders a lit spinning cube on a grid floor with one multiview,
  dynamic-rendering pass into a 2-layer sRGB swapchain.
- 72 Hz through `XR_FB_display_refresh_rate` (`TRIAEVUM_XR_REFRESH=90` in
  `<ext>/triaevum-env.txt` asks for 90), CPU `SUSTAINED_HIGH` through
  `XR_EXT_performance_settings`.
- A HUD quad layer (5x7 bitmap font, CPU-drawn, copied into its swapchain)
  with fps, refresh rate and probe results.
- Touch: left stick moves the cube, A changes its colour with a haptic pulse,
  the left menu button exits. `XR_SESSION_STATE_EXITING` tears down and `_exit`s.

## Probes

1. **Side-load.** On start, `HelloXrActivity` copies
   `<ext>/incoming/libprobe.so` to `filesDir/probe/libprobe.so`, marks it
   read-only, and native code `dlopen`s it and calls `triaevum_probe()` (42).
   It also tries the `/sdcard` copy directly, which is expected to fail.
2. **CPU.** Affinity mask, usable cores, each allowed core's
   `cpuinfo_max_freq`, `scaling_max_freq` and `scaling_cur_freq`, and the
   `/proc/cpuinfo` part numbers, before the performance level is set and
   3 s after.

Everything is logged under the tag `TriAevumXR`.

## Build

CI: `.github/workflows/quest-hello.yml` (artifacts `TriAevum-XR-Test.apk`
and `libprobe.so`). Locally on Linux with JDK 17, Gradle 8.13, the Android
SDK and NDK 29.0.14206865: `bash ports/quest/hello/build.sh` (unsigned
builds fall back to the debug key).

## On the headset

```
adb install -r TriAevum-XR-Test.apk
# launch once (Library > Unknown Sources), quit, then:
adb push libprobe.so /sdcard/Android/data/org.triaevum.quest.hello/files/incoming/
adb shell am force-stop org.triaevum.quest.hello
# launch again
adb logcat -s TriAevumXR
```
