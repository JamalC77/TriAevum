// Quest shell entry points exported from libTriAevum.so.
#include <jni.h>

#include <android/log.h>
#include <cstdio>
#include <unistd.h>

// Runtime globals are process-lifetime (ports/android/INTRO.md, "Remaining Android Issues"), so a
// destroyed game Activity ends the process instead of running static destructors or reusing state.
// Shared by the panel (TriAevumGameActivity) and the immersive (TriAevumXrActivity) activities
// through their base class, TriAevumSdlActivity.
extern "C" JNIEXPORT void JNICALL
Java_org_triaevum_quest_TriAevumSdlActivity_nativeExitProcess(JNIEnv*, jclass, jint code) {
    __android_log_print(ANDROID_LOG_INFO, "TriAevum", "exiting the process (%d)", static_cast<int>(code));
    std::fflush(nullptr);
    _exit(code);
}
