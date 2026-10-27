package org.triaevum.quest;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Log;
import org.json.JSONObject;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;

/**
 * Installs the side-loaded, privately built title module.
 *
 * adb pushes libtriaevum_title_aot.so and title-abi.json into {@code <ext>/incoming}. External storage
 * is noexec, so the module is copied into app-private storage ({@code <files>/title/<sha256>/}),
 * fsynced and made read-only before the runtime dlopens it. The app only reads {@code incoming}: it
 * cannot delete files created by the shell user, so an unchanged incoming module is not copied again.
 */
final class TitleInstaller {
    static final String TAG = "TriAevum";
    static final String MODULE = "libtriaevum_title_aot.so";
    static final String ABI = "title-abi.json";
    /** Directories the runtime writes. adb-created directories would be read-only to the app. */
    static final String[] WRITABLE_DIRS = {
        "incoming", "install", "data", "data/config", "data/savedata", "data/cache",
        "data/cache/renderer", "logs",
    };
    /** The fields that decide whether host and title were built from the same ABI closure. */
    static final String[] PAIRING_FIELDS = { "schema", "ndk_revision", "abi", "stl", "closure_sha256" };

    static final class Result {
        final File module;      // null when no usable module is installed
        final String message;   // shown to the user when module is null, logged otherwise
        Result(File module, String message) { this.module = module; this.message = message; }
    }

    private final Context context;
    private final File external;
    private final SharedPreferences prefs;

    TitleInstaller(Context context) {
        this.context = context;
        this.external = context.getExternalFilesDir(null);
        this.prefs = context.getSharedPreferences("title", Context.MODE_PRIVATE);
    }

    File externalRoot() { return external; }

    void prepareDirectories() {
        for (String dir : WRITABLE_DIRS) {
            File path = new File(external, dir);
            if (!path.isDirectory() && !path.mkdirs()) Log.w(TAG, "Cannot create " + path);
        }
    }

    Result install() {
        prepareDirectories();
        JSONObject expected;
        try {
            expected = readJson(context.getAssets().open(ABI));
        } catch (Exception error) {
            return new Result(null, "This APK has no assets/" + ABI + " (" + error.getMessage() + ").");
        }
        File incoming = new File(external, "incoming");
        File incomingModule = new File(incoming, MODULE);
        File incomingAbi = new File(incoming, ABI);
        if (incomingModule.isFile()) {
            String stamp = incomingModule.length() + ":" + incomingModule.lastModified() + ":"
                + incomingAbi.length() + ":" + incomingAbi.lastModified();
            if (!stamp.equals(prefs.getString("incoming_stamp", ""))) {
                Result result = installIncoming(incomingModule, incomingAbi, expected);
                if (result.module == null) return result;
                prefs.edit().putString("incoming_stamp", stamp).apply();
            }
        }
        String active = prefs.getString("active_sha256", "");
        if (active.isEmpty()) {
            return new Result(null, "No title module is installed.\n\nPush the title module:\n"
                + "adb push libtriaevum_title_aot.so title-abi.json " + incoming.getAbsolutePath() + "/\n"
                + "then start TriAevum again.");
        }
        File directory = new File(new File(context.getFilesDir(), "title"), active);
        File module = new File(directory, MODULE);
        if (!module.isFile()) {
            prefs.edit().remove("active_sha256").remove("incoming_stamp").apply();
            return new Result(null, "The installed title module is missing (" + module + ").\n\nPush the title module to "
                + incoming.getAbsolutePath() + "/ again.");
        }
        try {
            String mismatch = pairingMismatch(expected, readJson(new FileInputStream(new File(directory, ABI))));
            if (mismatch != null) {
                return new Result(null, "The installed title module was built for a different runtime: " + mismatch
                    + "\n\nRebuild the title from the same quest commit and NDK, then push it to "
                    + incoming.getAbsolutePath() + "/.");
            }
        } catch (Exception error) {
            return new Result(null, "Cannot read the installed " + ABI + ": " + error.getMessage());
        }
        return new Result(module, "Title module " + active.substring(0, 12) + " (" + module.length() + " bytes)");
    }

    private Result installIncoming(File incomingModule, File incomingAbi, JSONObject expected) {
        if (!incomingAbi.isFile()) {
            return new Result(null, "Found " + MODULE + " but no " + ABI + " beside it in "
                + incomingAbi.getParent() + ".\n\nPush both files from the title build directory.");
        }
        JSONObject provided;
        try {
            provided = readJson(new FileInputStream(incomingAbi));
        } catch (Exception error) {
            return new Result(null, "Cannot read incoming " + ABI + ": " + error.getMessage());
        }
        String mismatch = pairingMismatch(expected, provided);
        if (mismatch != null) {
            return new Result(null, "Refusing the pushed title module: it was built for a different runtime ("
                + mismatch + ").\n\nRebuild the title from the same quest commit and NDK.");
        }
        File titles = new File(context.getFilesDir(), "title");
        File staging = new File(titles, ".incoming");
        try {
            if (!titles.isDirectory() && !titles.mkdirs()) throw new IOException("cannot create " + titles);
            deleteTree(staging);
            if (!staging.mkdirs()) throw new IOException("cannot create " + staging);
            File stagedModule = new File(staging, MODULE);
            String sha = copyAndHash(incomingModule, stagedModule);
            copyAndHash(incomingAbi, new File(staging, ABI));
            if (!stagedModule.setReadOnly()) throw new IOException("cannot make the module read-only");
            File target = new File(titles, sha);
            if (new File(target, MODULE).isFile()) {
                deleteTree(staging);
            } else {
                deleteTree(target);
                if (!staging.renameTo(target)) throw new IOException("cannot move the module into " + target);
            }
            prefs.edit().putString("active_sha256", sha).commit();
            removeOtherTitles(titles, sha);
            Log.i(TAG, "Installed title module " + sha + " from " + incomingModule);
            return new Result(new File(target, MODULE), "Installed title module " + sha.substring(0, 12));
        } catch (Exception error) {
            deleteTree(staging);
            Log.e(TAG, "Title module install failed", error);
            return new Result(null, "Installing the title module failed: " + error.getMessage());
        }
    }

    /** Returns null when the two manifests pair, otherwise a short description of the difference. */
    static String pairingMismatch(JSONObject expected, JSONObject provided) {
        List<String> differences = new ArrayList<>();
        for (String field : PAIRING_FIELDS) {
            String want = expected.optString(field, "");
            String have = provided.optString(field, "");
            if (!want.equals(have)) differences.add(field + " " + shorten(have) + " (runtime " + shorten(want) + ")");
        }
        return differences.isEmpty() ? null : String.join(", ", differences);
    }

    private static String shorten(String value) {
        if (value.isEmpty()) return "missing";
        return value.length() > 16 ? value.substring(0, 12) : value;
    }

    private static JSONObject readJson(InputStream stream) throws Exception {
        try (InputStream input = stream) {
            byte[] bytes = input.readAllBytes();
            return new JSONObject(new String(bytes, StandardCharsets.UTF_8));
        }
    }

    private static String copyAndHash(File source, File destination) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        byte[] buffer = new byte[1 << 20];
        try (FileInputStream input = new FileInputStream(source);
             FileOutputStream output = new FileOutputStream(destination)) {
            int read;
            while ((read = input.read(buffer)) > 0) {
                digest.update(buffer, 0, read);
                output.write(buffer, 0, read);
            }
            output.flush();
            output.getFD().sync();
        }
        StringBuilder hex = new StringBuilder();
        for (byte b : digest.digest()) hex.append(String.format("%02x", b));
        return hex.toString();
    }

    private static void removeOtherTitles(File titles, String keep) {
        File[] entries = titles.listFiles();
        if (entries == null) return;
        for (File entry : entries) {
            if (!entry.getName().equals(keep)) deleteTree(entry);
        }
    }

    private static void deleteTree(File file) {
        if (!file.exists()) return;
        try {
            if (file.isDirectory()) {
                File[] children = file.listFiles();
                if (children != null) for (File child : children) deleteTree(child);
            }
            file.setWritable(true);
            Files.deleteIfExists(file.toPath());
        } catch (IOException error) {
            Log.w(TAG, "Cannot delete " + file, error);
        }
    }
}
