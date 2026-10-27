package org.triaevum.quest;

import android.content.Context;
import android.os.Bundle;
import android.util.Log;
import android.view.WindowManager;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import org.json.JSONObject;
import org.libsdl.app.SDLSurface;

/**
 * The flat panel: the game in a Horizon OS 2D window (the fallback of the immersive
 * {@link TriAevumXrActivity}). Started only by {@link TriAevumLauncherActivity}, which supplies the
 * installed title module path.
 */
public final class TriAevumGameActivity extends TriAevumSdlActivity {
    @Override protected void onCreate(Bundle state) {
        // Before SDLActivity.onCreate loads the native libraries, so every getenv() sees the values.
        applyEnvironmentFile(new File(getExternalFilesDir(null), ENV_FILE));
        // The panel never runs the immersive renderer path, whatever the environment file says.
        forceEnvironment("TRIAEVUM_XR", "0");
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override protected SDLSurface createSDLSurface(Context context) {
        int maximumShortEdge = 720;
        File config = new File(getExternalFilesDir(null), "TriAevum.quest.host.json");
        if (config.isFile()) {
            try {
                maximumShortEdge = new JSONObject(new String(Files.readAllBytes(config.toPath()), StandardCharsets.UTF_8))
                    .getInt("maximum_surface_short_edge");
                if (maximumShortEdge < 0) throw new IllegalArgumentException("Negative surface limit");
            } catch (Exception error) {
                Log.w(TAG, "Invalid Quest host config; using 720p surface limit", error);
                maximumShortEdge = 720;
            }
        }
        return new TriAevumSurface(context, maximumShortEdge);
    }
}
