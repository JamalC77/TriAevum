package org.triaevum.quest;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;
import android.os.Process;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import org.libsdl.app.SDLActivity;

/**
 * What the flat panel ({@link TriAevumGameActivity}) and the immersive ({@link TriAevumXrActivity})
 * game activities share: the environment file, the native libraries, the validated runtime
 * arguments, the API 34 receiver fix and the process exit. SDL owns the Surface and lifecycle;
 * game, renderer and title loading keep their native owners.
 */
public abstract class TriAevumSdlActivity extends SDLActivity {
    static final String EXTRA_TITLE_PLUGIN = "org.triaevum.quest.TITLE_PLUGIN";
    static final String EXTRA_LAUNCH_PROFILE = "org.triaevum.quest.LAUNCH_PROFILE";
    static final String ENV_FILE = "triaevum-env.txt";
    static final String TAG = "TriAevum";

    /** fflush(NULL) then _exit(code): runtime globals are process-lifetime (libTriAevum.so, quest_host.cpp). */
    private static native void nativeExitProcess(int code);

    /**
     * KEY=VALUE lines, '#' comments, like Xelee's pc_env_file_bootstrap. Android cannot hand a
     * process an environment, so diagnostics knobs come from this file. Existing values win.
     */
    static void applyEnvironmentFile(File file) {
        if (!file.isFile()) {
            Log.i(TAG, "env: no " + file);
            return;
        }
        int applied = 0;
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                int eq = line.indexOf('=');
                if (eq <= 0) continue;
                String key = line.substring(0, eq).trim();
                String value = line.substring(eq + 1).trim();
                try {
                    Os.setenv(key, value, false);
                    applied++;
                    Log.i(TAG, "env: " + key + "=" + value);
                } catch (ErrnoException error) {
                    Log.w(TAG, "env: cannot set " + key, error);
                }
            }
            Log.i(TAG, "env: applied " + applied + " values from " + file);
        } catch (Exception error) {
            Log.w(TAG, "env: cannot read " + file, error);
        }
    }

    /** Overwrites: values this activity must own whatever the environment file says. */
    static void forceEnvironment(String key, String value) {
        try {
            Os.setenv(key, value, true);
            Log.i(TAG, "env: " + key + "=" + value + " (activity)");
        } catch (ErrnoException error) {
            Log.w(TAG, "env: cannot set " + key, error);
        }
    }

    File externalRoot() {
        return getExternalFilesDir(null);
    }

    boolean hasLaunchExtras() {
        Intent intent = getIntent();
        return intent != null && intent.getStringExtra(EXTRA_TITLE_PLUGIN) != null
            && intent.getStringExtra(EXTRA_LAUNCH_PROFILE) != null;
    }

    /** True when {@code path} names a file inside {@code root} (canonical paths, no escapes). */
    static boolean isInside(String path, File root) {
        if (path == null || path.isEmpty() || root == null) return false;
        try {
            String file = new File(path).getCanonicalPath();
            String directory = root.getCanonicalPath();
            return file.startsWith(directory + File.separator);
        } catch (IOException error) {
            return false;
        }
    }

    @Override protected String[] getLibraries() {
        return new String[] { "SDL2", "triaevum_title_bootstrap", "TriAevum" };
    }

    @Override protected String[] getArguments() {
        Intent intent = getIntent();
        String plugin = intent.getStringExtra(EXTRA_TITLE_PLUGIN);
        String profile = intent.getStringExtra(EXTRA_LAUNCH_PROFILE);
        if (plugin == null || profile == null) {
            // Re-created without the launcher's extras: fail visibly rather than guess a module.
            Log.e(TAG, "Game activity started without a title module; use the launcher");
            return new String[] { "--title-plugin", "", "--launch-profile", "" };
        }
        // Only the installer's directory may provide the module the runtime dlopens, and only the
        // app's external directory the launch profile (the immersive activity is exported).
        if (!isInside(plugin, new File(getFilesDir(), "title")) || !isInside(profile, externalRoot())) {
            Log.e(TAG, "Refusing runtime arguments outside the app's directories: " + plugin + ", " + profile);
            return new String[] { "--title-plugin", "", "--launch-profile", "" };
        }
        Log.i(TAG, "Runtime arguments: --launch-profile " + profile + " --title-plugin " + plugin);
        return new String[] { "--launch-profile", profile, "--title-plugin", plugin };
    }

    /**
     * SDL's HIDDeviceManager registers a receiver for a non-system broadcast (USB permission). From
     * target SDK 34 that throws unless the receiver states whether it is exported.
     */
    @Override public Intent registerReceiver(BroadcastReceiver receiver, IntentFilter filter) {
        if (Build.VERSION.SDK_INT >= 33) {
            return super.registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED);
        }
        return super.registerReceiver(receiver, filter);
    }

    @Override protected void onDestroy() {
        Log.i(TAG, "onDestroy: stopping the runtime and exiting the process");
        Thread sdl = SDLActivity.mSDLThread;
        if (!SDLActivity.mBrokenLibraries && sdl != null && sdl.isAlive()) {
            SDLActivity.nativeSendQuit();
            try {
                sdl.join(10000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            if (sdl.isAlive()) Log.w(TAG, "Runtime did not stop within 10 s; exiting anyway");
        }
        if (SDLActivity.mBrokenLibraries) {
            Process.killProcess(Process.myPid());
        }
        nativeExitProcess(0);
    }
}
