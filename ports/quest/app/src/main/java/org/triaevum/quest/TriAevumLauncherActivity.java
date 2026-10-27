package org.triaevum.quest;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;

/**
 * Installs and checks the side-loaded title module, then starts the game: immersive
 * ({@link TriAevumXrActivity}, the default), on a flat panel ({@link TriAevumGameActivity}), or asks.
 * TRIAEVUM_QUEST_MODE=vr|panel|ask in {@code <ext>/triaevum-env.txt} chooses; a failed immersive
 * start ({@code <ext>/logs/xr-start-failed.txt}, written by the runtime) makes the next launch ask.
 */
public final class TriAevumLauncherActivity extends Activity {
    static final String LAUNCH_PROFILE = "TriAevum.quest.launch.json";
    static final String XR_FAILURE_MARKER = "logs/xr-start-failed.txt";

    private TextView status;
    private Button retry;
    private Button playVr;
    private Button playPanel;
    private File pendingModule;
    private File pendingProfile;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        int pad = (int) (32 * getResources().getDisplayMetrics().density);
        layout.setPadding(pad, pad, pad, pad);
        TextView title = new TextView(this);
        title.setText("TriAevum");
        title.setTextSize(28);
        title.setGravity(Gravity.CENTER);
        layout.addView(title);
        status = new TextView(this);
        status.setTextSize(16);
        status.setTypeface(Typeface.MONOSPACE);
        status.setTextIsSelectable(true);
        status.setPadding(0, pad / 2, 0, pad / 2);
        layout.addView(status);
        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);
        playVr = button("Play in VR", view -> choose(true));
        playPanel = button("Play on a flat panel", view -> choose(false));
        retry = button("Try again", view -> check());
        buttons.addView(playVr, wrap());
        buttons.addView(playPanel, wrap());
        buttons.addView(retry, wrap());
        layout.addView(buttons, wrap());
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(layout);
        setContentView(scroll);
        check();
    }

    private Button button(String text, View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(text);
        button.setOnClickListener(listener);
        return button;
    }

    private static LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private void showChoice(boolean visible) {
        int visibility = visible ? View.VISIBLE : View.GONE;
        playVr.setVisibility(visibility);
        playPanel.setVisibility(visibility);
    }

    private void check() {
        retry.setEnabled(false);
        showChoice(false);
        status.setText("Checking the title module...");
        new Thread(() -> {
            TitleInstaller installer = new TitleInstaller(this);
            TitleInstaller.Result result = installer.install();
            File external = installer.externalRoot();
            File profile = new File(external, LAUNCH_PROFILE);
            QuestLaunchMode mode = BuildConfig.QUEST_OPENXR
                ? QuestLaunchMode.fromEnvFile(new File(external, TriAevumSdlActivity.ENV_FILE))
                : QuestLaunchMode.PANEL;
            String failure = mode != QuestLaunchMode.PANEL ? firstLine(new File(external, XR_FAILURE_MARKER)) : null;
            runOnUiThread(() -> {
                if (isFinishing()) return;
                if (result.module == null) {
                    show(result.message);
                } else if (!profile.isFile()) {
                    show(result.message + "\n\nNo launch profile.\n\nPush the staged game data so that\n"
                        + profile.getAbsolutePath() + "\nexists, then start TriAevum again.");
                } else {
                    pendingModule = result.module;
                    pendingProfile = profile;
                    Log.i(TitleInstaller.TAG, result.message + "; launch mode " + mode
                        + (failure != null ? " (last VR start failed: " + failure + ")" : ""));
                    if (failure != null || mode == QuestLaunchMode.ASK) {
                        String why = failure != null
                            ? "The last VR start failed:\n" + failure + "\n\n"
                            : "";
                        show(result.message + "\n\n" + why + "How do you want to play?");
                        showChoice(true);
                    } else {
                        launch(mode == QuestLaunchMode.VR);
                    }
                }
            });
        }, "TitleInstaller").start();
    }

    private void choose(boolean vr) {
        if (pendingModule == null || pendingProfile == null) return;
        File marker = new File(getExternalFilesDir(null), XR_FAILURE_MARKER);
        if (marker.exists() && !marker.delete()) Log.w(TitleInstaller.TAG, "Cannot delete " + marker);
        launch(vr);
    }

    private void launch(boolean vr) {
        Intent intent = new Intent(this, vr ? TriAevumXrActivity.class : TriAevumGameActivity.class);
        intent.putExtra(TriAevumSdlActivity.EXTRA_TITLE_PLUGIN, pendingModule.getAbsolutePath());
        intent.putExtra(TriAevumSdlActivity.EXTRA_LAUNCH_PROFILE, pendingProfile.getAbsolutePath());
        if (vr) {
            // Hybrid app: the immersive activity runs in its own task and process; the panel closes.
            intent.setAction(Intent.ACTION_MAIN);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            Log.i(TitleInstaller.TAG, "launching VR activity");
            startActivity(intent);
            finishAndRemoveTask();
        } else {
            Log.i(TitleInstaller.TAG, "launching the flat panel activity");
            startActivity(intent);
            finish();
        }
    }

    private static String firstLine(File file) {
        if (!file.isFile()) return null;
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line = reader.readLine();
            return line == null || line.trim().isEmpty() ? "unknown reason" : line.trim();
        } catch (Exception error) {
            return "unreadable failure marker";
        }
    }

    private void show(String message) {
        Log.w(TitleInstaller.TAG, message);
        status.setText(message);
        retry.setEnabled(true);
    }
}
