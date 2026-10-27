package org.triaevum.quest.hello;

import android.os.Bundle;
import android.system.Os;
import android.util.Log;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.InputStream;

/**
 * Immersive SDL2 activity. SDL owns the lifecycle and calls SDL_main in
 * libtriaevum_hello.so, which runs OpenXR on its own thread. Before that,
 * this class applies the env file and stages the side-load probe:
 * {@code <ext>/incoming/libprobe.so} is copied to {@code filesDir/probe/},
 * made read-only and handed to native code by path (it dlopens both copies).
 */
public final class HelloXrActivity extends SDLActivity {
    private static final String TAG = "TriAevumXR";

    @Override
    protected void onCreate(Bundle state) {
        try {
            bootstrap();
        } catch (Exception e) {
            Log.e(TAG, "bootstrap failed", e);
        }
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL2", "triaevum_hello"};
    }

    private static void setenv(String key, String value) {
        try {
            Os.setenv(key, value, true);
        } catch (Exception e) {
            Log.w(TAG, "setenv " + key + " failed", e);
        }
    }

    private void bootstrap() throws Exception {
        // Creates /sdcard/Android/data/<pkg>/files, owned by this app.
        File ext = getExternalFilesDir(null);
        File incoming = new File(ext, "incoming");
        if (!incoming.isDirectory() && !incoming.mkdirs())
            Log.w(TAG, "could not create " + incoming);
        setenv("TRIAEVUM_EXT_DIR", ext.getAbsolutePath());
        setenv("TRIAEVUM_FILES_DIR", getFilesDir().getAbsolutePath());

        // <ext>/triaevum-env.txt: KEY=VALUE lines (# comments), e.g.
        // TRIAEVUM_XR_REFRESH=90.
        File envFile = new File(ext, "triaevum-env.txt");
        if (envFile.isFile()) {
            try (BufferedReader r = new BufferedReader(new FileReader(envFile))) {
                String line;
                while ((line = r.readLine()) != null) {
                    line = line.trim();
                    int eq = line.indexOf('=');
                    if (line.isEmpty() || line.startsWith("#") || eq <= 0)
                        continue;
                    String key = line.substring(0, eq).trim(), value = line.substring(eq + 1).trim();
                    setenv(key, value);
                    Log.i(TAG, "env " + key + "=" + value);
                }
            }
        }

        File source = new File(incoming, "libprobe.so");
        setenv("TRIAEVUM_PROBE_SDCARD", source.getAbsolutePath());
        if (!source.isFile()) {
            setenv("TRIAEVUM_PROBE_COPY", "NO " + source.getAbsolutePath());
            Log.i(TAG, "probe: no " + source);
            return;
        }
        File dir = new File(getFilesDir(), "probe");
        if (!dir.isDirectory() && !dir.mkdirs())
            throw new IllegalStateException("could not create " + dir);
        File target = new File(dir, "libprobe.so");
        if (target.exists()) {
            target.setWritable(true, true);
            if (!target.delete())
                Log.w(TAG, "could not delete old " + target);
        }
        long bytes = 0;
        try (InputStream in = new FileInputStream(source); FileOutputStream out = new FileOutputStream(target)) {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                bytes += n;
            }
            out.getFD().sync();
        }
        boolean readOnly = target.setReadOnly();
        String status = "COPIED " + bytes + " BYTES TO " + target.getAbsolutePath() + ", READ-ONLY " + readOnly;
        setenv("TRIAEVUM_PROBE_LIB", target.getAbsolutePath());
        setenv("TRIAEVUM_PROBE_COPY", status);
        Log.i(TAG, "probe: " + status);
    }
}
