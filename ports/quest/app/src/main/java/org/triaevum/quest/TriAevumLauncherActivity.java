package org.triaevum.quest;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.Log;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;

/** Installs and checks the side-loaded title module, then starts the SDL game activity. */
public final class TriAevumLauncherActivity extends Activity {
    static final String LAUNCH_PROFILE = "TriAevum.quest.launch.json";

    private TextView status;
    private Button retry;

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
        retry = new Button(this);
        retry.setText("Try again");
        retry.setOnClickListener(view -> check());
        layout.addView(retry, new LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(layout);
        setContentView(scroll);
        check();
    }

    private void check() {
        retry.setEnabled(false);
        status.setText("Checking the title module...");
        new Thread(() -> {
            TitleInstaller installer = new TitleInstaller(this);
            TitleInstaller.Result result = installer.install();
            File profile = new File(installer.externalRoot(), LAUNCH_PROFILE);
            runOnUiThread(() -> {
                if (isFinishing()) return;
                if (result.module == null) {
                    show(result.message);
                } else if (!profile.isFile()) {
                    show(result.message + "\n\nNo launch profile.\n\nPush the staged game data so that\n"
                        + profile.getAbsolutePath() + "\nexists, then start TriAevum again.");
                } else {
                    Log.i(TitleInstaller.TAG, result.message + "; starting the game");
                    Intent intent = new Intent(this, TriAevumGameActivity.class);
                    intent.putExtra(TriAevumGameActivity.EXTRA_TITLE_PLUGIN, result.module.getAbsolutePath());
                    intent.putExtra(TriAevumGameActivity.EXTRA_LAUNCH_PROFILE, profile.getAbsolutePath());
                    startActivity(intent);
                    finish();
                }
            });
        }, "TitleInstaller").start();
    }

    private void show(String message) {
        Log.w(TitleInstaller.TAG, message);
        status.setText(message);
        retry.setEnabled(true);
    }
}
