package org.triaevum.quest;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * How the launcher starts the game: TRIAEVUM_QUEST_MODE in {@code <ext>/triaevum-env.txt}.
 * Read straight from the file on every check (never through the process environment: the launcher
 * process outlives a VR launch, and the environment file's values never overwrite).
 */
enum QuestLaunchMode {
    VR, PANEL, ASK;

    static final String KEY = "TRIAEVUM_QUEST_MODE";

    /** {@code vr} (also the default for a missing or unknown value), {@code panel}, {@code ask}. */
    static QuestLaunchMode parse(String value) {
        if (value == null) return VR;
        switch (value.trim().toLowerCase(Locale.ROOT)) {
            case "panel":
            case "flat":
            case "2d":
                return PANEL;
            case "ask":
                return ASK;
            default:
                return VR;
        }
    }

    /** The value of the last TRIAEVUM_QUEST_MODE line of an env file's text, or null. */
    static String findValue(String text) {
        if (text == null) return null;
        String found = null;
        for (String line : text.split("\\r?\\n")) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            if (line.substring(0, eq).trim().equals(KEY)) found = line.substring(eq + 1).trim();
        }
        return found;
    }

    static QuestLaunchMode fromEnvFile(File file) {
        if (file == null || !file.isFile()) return VR;
        StringBuilder text = new StringBuilder();
        try (BufferedReader reader = new BufferedReader(
                new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) text.append(line).append('\n');
        } catch (Exception error) {
            return VR;
        }
        return parse(findValue(text.toString()));
    }
}
