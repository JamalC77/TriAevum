// TriAevum Quest XR logging: logcat tag "TriAevumXR" and the same line on stderr, which the Android
// host redirects to <ext>/logs/native-stderr.log (logcat would not see stderr).
#pragma once

#include <android/log.h>

namespace TriAevumQuestXr::Log {
void Write(int priority, const char* format, ...) __attribute__((format(printf, 2, 3)));
}  // namespace TriAevumQuestXr::Log

#define XRLOG(...) ::TriAevumQuestXr::Log::Write(ANDROID_LOG_INFO, __VA_ARGS__)
#define XRWARN(...) ::TriAevumQuestXr::Log::Write(ANDROID_LOG_WARN, __VA_ARGS__)
#define XRERR(...) ::TriAevumQuestXr::Log::Write(ANDROID_LOG_ERROR, __VA_ARGS__)
