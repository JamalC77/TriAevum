package org.triaevum.quest;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;

import org.junit.Test;

public class QuestLaunchModeTest {
    @Test public void parsesTheThreeModes() {
        assertEquals(QuestLaunchMode.VR, QuestLaunchMode.parse("vr"));
        assertEquals(QuestLaunchMode.PANEL, QuestLaunchMode.parse("panel"));
        assertEquals(QuestLaunchMode.PANEL, QuestLaunchMode.parse(" Panel "));
        assertEquals(QuestLaunchMode.ASK, QuestLaunchMode.parse("ask"));
    }

    @Test public void defaultsToVr() {
        assertEquals(QuestLaunchMode.VR, QuestLaunchMode.parse(null));
        assertEquals(QuestLaunchMode.VR, QuestLaunchMode.parse(""));
        assertEquals(QuestLaunchMode.VR, QuestLaunchMode.parse("something"));
    }

    @Test public void findsTheLastModeLine() {
        String text = "# comment\nTRIAEVUM_XR_REFRESH=90\nTRIAEVUM_QUEST_MODE=ask\r\n"
            + "#TRIAEVUM_QUEST_MODE=vr\nTRIAEVUM_QUEST_MODE = panel\n";
        assertEquals("panel", QuestLaunchMode.findValue(text));
        assertNull(QuestLaunchMode.findValue("TRIAEVUM_XR_REFRESH=90\n"));
        assertNull(QuestLaunchMode.findValue(null));
    }

    @Test public void readsAMissingFileAsVr() {
        assertEquals(QuestLaunchMode.VR, QuestLaunchMode.fromEnvFile(null));
        assertEquals(QuestLaunchMode.VR, QuestLaunchMode.fromEnvFile(new java.io.File("/nonexistent/triaevum-env.txt")));
    }
}
