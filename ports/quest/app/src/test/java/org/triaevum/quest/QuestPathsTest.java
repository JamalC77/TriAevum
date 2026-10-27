package org.triaevum.quest;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.io.File;
import org.junit.Test;

public class QuestPathsTest {
    @Test public void acceptsBothAppDataSpellings() {
        File root = new File("/data/user/0/org.triaevum.quest/files/title");
        assertTrue(QuestPaths.isInside("/data/user/0/org.triaevum.quest/files/title/abc/libx.so", root));
        assertTrue(QuestPaths.isInside("/data/data/org.triaevum.quest/files/title/abc/libx.so", root));
        File legacyRoot = new File("/data/data/org.triaevum.quest/files/title");
        assertTrue(QuestPaths.isInside("/data/user/0/org.triaevum.quest/files/title/abc/libx.so", legacyRoot));
    }

    @Test public void refusesEscapesAndOtherDirectories() {
        File root = new File("/data/user/0/org.triaevum.quest/files/title");
        assertFalse(QuestPaths.isInside("/data/user/0/org.triaevum.quest/files/title/../evil.so", root));
        assertFalse(QuestPaths.isInside("/data/user/0/org.triaevum.quest/files/titlex/a.so", root));
        assertFalse(QuestPaths.isInside("/data/data/other.app/files/title/a.so", root));
        assertFalse(QuestPaths.isInside("/data/local/tmp/a.so", root));
        assertFalse(QuestPaths.isInside("", root));
        assertFalse(QuestPaths.isInside(null, root));
        assertFalse(QuestPaths.isInside("/data/user/0/org.triaevum.quest/files/title/a.so", null));
    }

    @Test public void spellsOnlyAppDataRootsTwice() {
        assertArrayEquals(new String[] { "/storage/emulated/0/Android/data/p/files" },
            QuestPaths.rootSpellings("/storage/emulated/0/Android/data/p/files"));
        assertArrayEquals(new String[] { "/data/user/0/p/files", "/data/data/p/files" },
            QuestPaths.rootSpellings("/data/user/0/p/files"));
    }
}
