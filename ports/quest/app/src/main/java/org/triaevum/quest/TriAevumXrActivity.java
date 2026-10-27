package org.triaevum.quest;

import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.system.Os;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import java.io.File;
import java.util.Locale;
import org.libsdl.app.SDLControllerManager;

/**
 * The immersive game (Quest VR category, own process ":xr"): the same runtime, with TRIAEVUM_XR=1
 * so the renderer creates its Vulkan device through OpenXR and the frame appears on a world-locked
 * virtual screen (ports/quest/runtime/xr). SDL still provides its (hidden) Surface, events,
 * gamepads and audio; pause and resume follow the OpenXR session state instead of SDL's pump.
 *
 * Pad diagnostics (logcat tag TriAevumPad, beside the native TRIAEVUM_PAD lines): the input devices
 * Android reports and whether SDL takes each for a joystick, at start and on every add, remove or
 * change; with TRIAEVUM_XR_INPUT_LOG=1 (or TRIAEVUM_PAD_LOG=2) every gamepad key event the window
 * receives (and whether SDL handled it) and one joystick motion line per second. TRIAEVUM_PAD_LOG=0
 * turns them off.
 *
 * Direct start for tests:
 *   am start -n org.triaevum.quest/.TriAevumXrActivity
 *     --es org.triaevum.quest.TITLE_PLUGIN &lt;files&gt;/title/&lt;sha256&gt;/libtriaevum_title_aot.so
 *     --es org.triaevum.quest.LAUNCH_PROFILE &lt;ext&gt;/TriAevum.quest.launch.json
 */
public final class TriAevumXrActivity extends TriAevumSdlActivity {
    static final String PAD_TAG = "TriAevumPad";

    private boolean padInputLog;
    private InputManager.InputDeviceListener padListener;
    private long lastMotionLogMs;

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
            return;
        }
        if (!"0".equals(Os.getenv("TRIAEVUM_PAD_LOG"))) startPadDiagnostics();
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
        if (padListener != null) {
            View focus = getCurrentFocus();
            Log.i(PAD_TAG, "TRIAEVUM_PAD java window_focus=" + (hasFocus ? 1 : 0) + " focus_view="
                + (focus != null ? focus.getClass().getSimpleName() : "none"));
        }
        super.onWindowFocusChanged(hasFocus);
    }

    @Override protected void onDestroy() {
        if (padListener != null) {
            InputManager manager = (InputManager) getSystemService(INPUT_SERVICE);
            if (manager != null) manager.unregisterInputDeviceListener(padListener);
            padListener = null;
        }
        super.onDestroy();
    }

    // ---- pad diagnostics

    private void startPadDiagnostics() {
        padInputLog = "1".equals(Os.getenv("TRIAEVUM_XR_INPUT_LOG")) || "2".equals(Os.getenv("TRIAEVUM_PAD_LOG"));
        int[] ids = InputDevice.getDeviceIds();
        Log.i(PAD_TAG, "TRIAEVUM_PAD java devices reason=create count=" + ids.length + " input_log="
            + (padInputLog ? 1 : 0));
        for (int id : ids) logInputDevice("create", id);
        InputManager manager = (InputManager) getSystemService(INPUT_SERVICE);
        if (manager == null) {
            Log.w(PAD_TAG, "TRIAEVUM_PAD java no InputManager: hot-plug events are not logged");
            return;
        }
        padListener = new InputManager.InputDeviceListener() {
            @Override public void onInputDeviceAdded(int deviceId) { logInputDevice("added", deviceId); }
            @Override public void onInputDeviceRemoved(int deviceId) {
                Log.i(PAD_TAG, "TRIAEVUM_PAD java device reason=removed id=" + deviceId);
            }
            @Override public void onInputDeviceChanged(int deviceId) { logInputDevice("changed", deviceId); }
        };
        manager.registerInputDeviceListener(padListener, new Handler(Looper.getMainLooper()));
    }

    static boolean isPadSource(int source) {
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
            || (source & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD;
    }

    static void logInputDevice(String reason, int id) {
        InputDevice device = InputDevice.getDevice(id);
        if (device == null) {
            Log.i(PAD_TAG, "TRIAEVUM_PAD java device reason=" + reason + " id=" + id + " gone");
            return;
        }
        int sources = device.getSources();
        boolean external = Build.VERSION.SDK_INT >= 29 && device.isExternal();
        boolean[] keys = device.hasKeys(KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_X, KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_DPAD_CENTER);
        int axes = 0;
        for (InputDevice.MotionRange range : device.getMotionRanges())
            if ((range.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) != 0) axes++;
        Log.i(PAD_TAG, String.format(Locale.ROOT,
            "TRIAEVUM_PAD java device reason=%s id=%d name=\"%s\" vendor=0x%04X product=0x%04X sources=0x%08X "
                + "gamepad=%d joystick=%d dpad=%d virtual=%d external=%d enabled=%d sdl_joystick=%d keys_abxy=%d%d%d%d "
                + "dpad_center=%d joystick_axes=%d",
            reason, id, device.getName(), device.getVendorId(), device.getProductId(), sources,
            (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ? 1 : 0,
            (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK ? 1 : 0,
            (sources & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD ? 1 : 0,
            device.isVirtual() ? 1 : 0, external ? 1 : 0, device.isEnabled() ? 1 : 0,
            SDLControllerManager.isDeviceSDLJoystick(id) ? 1 : 0,
            keys[0] ? 1 : 0, keys[1] ? 1 : 0, keys[2] ? 1 : 0, keys[3] ? 1 : 0, keys[4] ? 1 : 0, axes));
    }

    @Override public boolean dispatchKeyEvent(KeyEvent event) {
        boolean handled = super.dispatchKeyEvent(event);
        if (padInputLog && isPadSource(event.getSource()) && event.getRepeatCount() == 0) {
            View focus = getCurrentFocus();
            Log.i(PAD_TAG, String.format(Locale.ROOT,
                "TRIAEVUM_PAD java key action=%s code=%s device=%d source=0x%08X handled=%d focus_view=%s",
                event.getAction() == KeyEvent.ACTION_DOWN ? "down" : event.getAction() == KeyEvent.ACTION_UP ? "up" : "other",
                KeyEvent.keyCodeToString(event.getKeyCode()), event.getDeviceId(), event.getSource(), handled ? 1 : 0,
                focus != null ? focus.getClass().getSimpleName() : "none"));
        }
        return handled;
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent event) {
        boolean handled = super.dispatchGenericMotionEvent(event);
        if (padInputLog && (event.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            long now = SystemClock.uptimeMillis();
            if (now - lastMotionLogMs >= 1000) {
                lastMotionLogMs = now;
                Log.i(PAD_TAG, String.format(Locale.ROOT,
                    "TRIAEVUM_PAD java motion device=%d x=%.2f y=%.2f z=%.2f rz=%.2f hat=%.0f,%.0f lt=%.2f rt=%.2f "
                        + "handled=%d",
                    event.getDeviceId(), event.getAxisValue(MotionEvent.AXIS_X), event.getAxisValue(MotionEvent.AXIS_Y),
                    event.getAxisValue(MotionEvent.AXIS_Z), event.getAxisValue(MotionEvent.AXIS_RZ),
                    event.getAxisValue(MotionEvent.AXIS_HAT_X), event.getAxisValue(MotionEvent.AXIS_HAT_Y),
                    event.getAxisValue(MotionEvent.AXIS_LTRIGGER), event.getAxisValue(MotionEvent.AXIS_RTRIGGER),
                    handled ? 1 : 0));
            }
        }
        return handled;
    }
}
