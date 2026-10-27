package org.triaevum.quest;

import android.os.Bundle;
import android.os.Process;
import android.util.Log;
import android.view.WindowManager;
import java.io.File;

/**
 * The immersive game (Quest VR category, own process ":xr"): the same runtime, with TRIAEVUM_XR=1
 * so the renderer creates its Vulkan device through OpenXR and the frame appears on a world-locked
 * virtual screen (ports/quest/runtime/xr). SDL still provides its (hidden) Surface, events,
 * gamepads and audio; pause and resume follow the OpenXR session state instead of SDL's pump.
 *
 * Direct start for tests:
 *   am start -n org.triaevum.quest/.TriAevumXrActivity
 *     --es org.triaevum.quest.TITLE_PLUGIN &lt;files&gt;/title/&lt;sha256&gt;/libtriaevum_title_aot.so
 *     --es org.triaevum.quest.LAUNCH_PROFILE &lt;ext&gt;/TriAevum.quest.launch.json
 */
public final class TriAevumXrActivity extends TriAevumSdlActivity {
    @Override protected void onCreate(Bundle state) {
        // Before SDLActivity.onCreate loads the native libraries (and SDL reads its hints).
        applyEnvironmentFile(new File(getExternalFilesDir(null), ENV_FILE));
        forceEnvironment("TRIAEVUM_XR", "1");
        // The game thread must never park inside SDL_PumpEvents: the OpenXR session state pauses
        // the guest. And SDL must not pause AAudio from its pump either: SDL 2.32 holds the audio
        // device's mixer lock on the pumping thread until the resume, and closing the device in that
        // window (a quit with the headset off) would deadlock against SDL's audio thread.
        forceEnvironment("SDL_ANDROID_BLOCK_ON_PAUSE", "0");
        forceEnvironment("SDL_ANDROID_BLOCK_ON_PAUSE_PAUSEAUDIO", "0");
        Log.i(TAG, "XR activity onCreate pid=" + Process.myPid());
        boolean extras = hasLaunchExtras();
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (!extras) {
            // For example the system re-creating ":xr" without the launcher: never guess a module.
            Log.e(TAG, "XR activity started without a title module; use the launcher");
            finish();
        }
    }

    @Override protected void onResume() {
        Log.i(TAG, "XR activity onResume");
        super.onResume();
    }

    @Override protected void onPause() {
        Log.i(TAG, "XR activity onPause");
        super.onPause();
    }

    @Override public void onWindowFocusChanged(boolean hasFocus) {
        Log.i(TAG, "XR activity focus " + hasFocus);
        super.onWindowFocusChanged(hasFocus);
    }
}
