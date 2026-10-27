package org.triaevum.quest;

import java.io.File;
import java.io.IOException;

/** Runtime-argument path checks for the game activities (plain Java, unit tested). */
final class QuestPaths {
    private static final String LEGACY_DATA = "/data/data/";
    private static final String USER0_DATA = "/data/user/0/";

    private QuestPaths() {}

    /**
     * True when {@code path} names a file inside {@code root} (canonical paths, no escapes). With
     * app data isolation /data/data/<pkg> and /data/user/0/<pkg> are separate mounts of the same
     * directory rather than a symlink, so either spelling of the root is accepted.
     */
    static boolean isInside(String path, File root) {
        if (path == null || path.isEmpty() || root == null) return false;
        try {
            String file = new File(path).getCanonicalPath();
            for (String directory : rootSpellings(root.getCanonicalPath())) {
                if (file.startsWith(directory + File.separator)) return true;
            }
            return false;
        } catch (IOException error) {
            return false;
        }
    }

    /** The canonical root and, for app-private storage, its /data/data vs /data/user/0 twin. */
    static String[] rootSpellings(String canonicalRoot) {
        if (canonicalRoot.startsWith(USER0_DATA)) {
            return new String[] { canonicalRoot, LEGACY_DATA + canonicalRoot.substring(USER0_DATA.length()) };
        }
        if (canonicalRoot.startsWith(LEGACY_DATA)) {
            return new String[] { canonicalRoot, USER0_DATA + canonicalRoot.substring(LEGACY_DATA.length()) };
        }
        return new String[] { canonicalRoot };
    }

    /** The canonical form for log lines (the input plus the reason when it has none). */
    static String canonical(String path) {
        try {
            return new File(path).getCanonicalPath();
        } catch (IOException error) {
            return path + " (no canonical path: " + error.getMessage() + ")";
        }
    }
}
