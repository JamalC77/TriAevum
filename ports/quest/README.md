# TriAevum on Meta Quest 3

Milestone Q2 shell: `org.triaevum.quest` is a normal 2D Android app, so the game shows flat in a
Horizon OS panel window. The immersive OpenXR activity comes in Q3.

The APK is title-free and built by `.github/workflows/quest.yml` (`tools/quest/build_quest.sh`).
The title module (`libtriaevum_title_aot.so`) and the game data are built and staged privately on
the player's PC from their own Forge installation and side-loaded with adb. Never commit or upload
them.

## Layout

| Path | What |
|---|---|
| `app/` | `TriAevumLauncherActivity` (title install and checks), `TriAevumGameActivity` (SDL), `TitleInstaller` |
| `runtime/` | CMake wrapper for `libTriAevum.so`: the shared `ports/android/runtime` host plus `quest_host.cpp` |
| `../../tools/quest/title_abi.py` | Title-side source closure guard and `title-abi.json` writer |
| `../../tools/quest/stage_quest.py` | Stages APK libraries and `quest-tools-arm64.zip`; checks AArch64, 16 KiB alignment, NEEDED |
| `../../tools/quest/synthetic_title.py` | A one-function synthetic ABI-v2 title for the probes (no game code) |

`<ext>` below is `/sdcard/Android/data/org.triaevum.quest/files`.

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
the game. `Try again` re-runs the checks without restarting the app.

Optional files in `<ext>`: `triaevum-env.txt` (`KEY=VALUE` lines applied with `setenv` before the
runtime loads; existing values win) and `TriAevum.quest.host.json` (`{"maximum_surface_short_edge": 720}`).

Logs:

```powershell
& $ADB logcat -s TriAevum SDL SDL/APP DEBUG AndroidRuntime
& $ADB shell run-as org.triaevum.quest cat /sdcard/Android/data/org.triaevum.quest/files/logs/native-stderr.log
& $ADB shell run-as org.triaevum.quest cat /sdcard/Android/data/org.triaevum.quest/files/data/runtime-state.json
```

Probes (title-free, from `quest-tools-arm64.zip`):

```powershell
& $ADB push quest-tools-arm64 /data/local/tmp/
& $ADB shell "cd /data/local/tmp/quest-tools-arm64 && sh run_all.sh"
```
