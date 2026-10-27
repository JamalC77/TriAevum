// The SDL pad path as device-log lines (see oot3d_native_pad_diagnostics.h).
#include "oot3d_native_pad_diagnostics.h"

#include "three_ds_sdl_controller.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#if defined(__ANDROID__)
// Weak, like the renderer's surface log: no hard liblog dependency for this unit.
extern "C" int __android_log_write(int priority, const char* tag, const char* text) __attribute__((weak));
#endif

namespace Oot3dNativeGame {

namespace {

enum class PadLogLevel : uint8_t { Off, Devices, Input };

bool EnvEquals(const char* name, std::string_view value) {
    const char* set = std::getenv(name);
    return set != nullptr && std::string_view(set) == value;
}

// TRIAEVUM_PAD_LOG / TRIAEVUM_XR_INPUT_LOG set explicitly: the device table also repeats every 30 s.
// The Android default logs it only at startup, on hot-plug events and on selection changes.
bool Explicit() {
    static const bool set = [] {
        const char* pad = std::getenv("TRIAEVUM_PAD_LOG");
        const bool padSet = pad != nullptr && *pad != '\0' && std::string_view(pad) != "0";
        return padSet || EnvEquals("TRIAEVUM_XR_INPUT_LOG", "1");
    }();
    return set;
}

PadLogLevel Level() {
    static const PadLogLevel level = [] {
        if (EnvEquals("TRIAEVUM_PAD_LOG", "0")) return PadLogLevel::Off;
        if (EnvEquals("TRIAEVUM_PAD_LOG", "2") || EnvEquals("TRIAEVUM_XR_INPUT_LOG", "1")) return PadLogLevel::Input;
        if (EnvEquals("TRIAEVUM_PAD_LOG", "1")) return PadLogLevel::Devices;
#if defined(__ANDROID__)
        return PadLogLevel::Devices;
#else
        return PadLogLevel::Off;
#endif
    }();
    return level;
}

#if defined(__GNUC__)
void PadLog(const char* format, ...) __attribute__((format(printf, 1, 2)));
#endif
void PadLog(const char* format, ...) {
    char buffer[768];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof buffer, format, arguments);
    va_end(arguments);
#if defined(__ANDROID__)
    if (__android_log_write != nullptr) __android_log_write(4 /* ANDROID_LOG_INFO */, "TriAevumPad", buffer);
#endif
    std::fprintf(stderr, "%s\n", buffer);
}

#if defined(__GNUC__)
std::string Formatted(const char* format, ...) __attribute__((format(printf, 1, 2)));
#endif
std::string Formatted(const char* format, ...) {
    char buffer[768];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof buffer, format, arguments);
    va_end(arguments);
    return buffer;
}

std::atomic<uint32_t> gDeviceEvents{0};

// Raw axis logging (input level): an axis is logged when it crosses half deflection, per instance (mod
// 64) and axis (< 64); the event watcher runs on whichever thread posts the event.
std::mutex gAxisMutex;
std::array<uint64_t, 64> gJoyAxisHigh{};
std::array<uint64_t, 64> gControllerAxisHigh{};

bool AxisCrossed(std::array<uint64_t, 64>& table, int32_t instance, uint32_t axis, int32_t value) {
    if (axis >= 64U) return false;
    const bool high = value >= 16384 || value <= -16384;
    std::lock_guard lock(gAxisMutex);
    uint64_t& bits = table[static_cast<uint32_t>(instance) & 63U];
    const uint64_t mask = uint64_t{1} << axis;
    if (((bits & mask) != 0U) == high) return false;
    bits ^= mask;
    return true;
}

// SDL event watcher: only the event's own fields (no SDL calls that take the joystick lock).
int SDLCALL WatchPadEvents(void*, SDL_Event* event) {
    switch (event->type) {
    case SDL_JOYDEVICEADDED:
        PadLog("TRIAEVUM_PAD event=joy_added device_index=%d", static_cast<int>(event->jdevice.which));
        ++gDeviceEvents;
        break;
    case SDL_JOYDEVICEREMOVED:
        PadLog("TRIAEVUM_PAD event=joy_removed instance=%d", static_cast<int>(event->jdevice.which));
        ++gDeviceEvents;
        break;
    case SDL_CONTROLLERDEVICEADDED:
        PadLog("TRIAEVUM_PAD event=controller_added device_index=%d", static_cast<int>(event->cdevice.which));
        ++gDeviceEvents;
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        PadLog("TRIAEVUM_PAD event=controller_removed instance=%d", static_cast<int>(event->cdevice.which));
        ++gDeviceEvents;
        break;
    case SDL_CONTROLLERDEVICEREMAPPED:
        PadLog("TRIAEVUM_PAD event=controller_remapped instance=%d", static_cast<int>(event->cdevice.which));
        ++gDeviceEvents;
        break;
    default:
        break;
    }
    if (Level() != PadLogLevel::Input) return 1;
    switch (event->type) {
    case SDL_JOYBUTTONDOWN:
    case SDL_JOYBUTTONUP:
        PadLog("TRIAEVUM_PAD raw joy_button instance=%d button=%u down=%u", static_cast<int>(event->jbutton.which),
               static_cast<unsigned>(event->jbutton.button), static_cast<unsigned>(event->jbutton.state));
        break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        const char* name =
            SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(event->cbutton.button));
        PadLog("TRIAEVUM_PAD raw controller_button instance=%d button=%s down=%u",
               static_cast<int>(event->cbutton.which), name != nullptr ? name : "?",
               static_cast<unsigned>(event->cbutton.state));
        break;
    }
    case SDL_JOYHATMOTION:
        PadLog("TRIAEVUM_PAD raw joy_hat instance=%d hat=%u value=0x%X", static_cast<int>(event->jhat.which),
               static_cast<unsigned>(event->jhat.hat), static_cast<unsigned>(event->jhat.value));
        break;
    case SDL_JOYAXISMOTION:
        if (AxisCrossed(gJoyAxisHigh, event->jaxis.which, event->jaxis.axis, event->jaxis.value))
            PadLog("TRIAEVUM_PAD raw joy_axis instance=%d axis=%u value=%d", static_cast<int>(event->jaxis.which),
                   static_cast<unsigned>(event->jaxis.axis), static_cast<int>(event->jaxis.value));
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (AxisCrossed(gControllerAxisHigh, event->caxis.which, event->caxis.axis, event->caxis.value)) {
            const char* name =
                SDL_GameControllerGetStringForAxis(static_cast<SDL_GameControllerAxis>(event->caxis.axis));
            PadLog("TRIAEVUM_PAD raw controller_axis instance=%d axis=%s value=%d",
                   static_cast<int>(event->caxis.which), name != nullptr ? name : "?",
                   static_cast<int>(event->caxis.value));
        }
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (event->key.repeat == 0U) {
            const char* name = SDL_GetScancodeName(event->key.keysym.scancode);
            PadLog("TRIAEVUM_PAD raw key scancode=%d name=\"%s\" down=%u", static_cast<int>(event->key.keysym.scancode),
                   name != nullptr ? name : "", static_cast<unsigned>(event->key.state));
        }
        break;
    default:
        break;
    }
    return 1;
}

const char* HintValue(const char* name) {
    const char* value = SDL_GetHint(name);
    return value != nullptr ? value : "(unset)";
}

const char* EnvValue(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? value : "(unset)";
}

std::string Clipped(const char* text, size_t limit = 80) {
    std::string value = text != nullptr ? text : "";
    if (value.size() > limit) value.resize(limit);
    for (char& c : value)
        if (c == '"' || c == '\n' || c == '\r') c = '\'';
    return value;
}

const char* SelectionResult(const PadPollFacts& facts, int joysticks) {
    if (joysticks <= 0) return "no_sdl_joysticks";
    if (facts.Port0 == nullptr || facts.Port0->empty()) return "none_opened";
    if (facts.SelectedInstance < 0)
        return facts.PreferredGuid.empty() && facts.PreferredSerial.empty() ? "none_selected" : "preferred_absent";
    if (!facts.ControllerEnabled) return "disabled_in_controls";
    return "selected";
}

void LogDevices(const char* reason, const PadPollFacts& facts) {
    // SDL's joystick list can change on the Android UI thread (Android_OnPadDown -> JoystickByDeviceId ->
    // ANDROID_JoystickDetect), so the count and every index's fields are read under SDL's (recursive)
    // joystick lock into lines that are logged after it is released: one line never mixes two devices.
    std::vector<std::string> lines;
    SDL_LockJoysticks();
    const int joysticks = SDL_NumJoysticks();
    for (int index = 0; index < joysticks; ++index) {
        char guid[33] = "";
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof guid);
        const SDL_JoystickID instance = SDL_JoystickGetDeviceInstanceID(index);
        char* mapping = SDL_GameControllerMappingForDeviceIndex(index);
        const bool hasMapping = mapping != nullptr;
        if (mapping != nullptr) SDL_free(mapping);
        SDL_GameController* opened = nullptr;
        if (facts.Port0 != nullptr) {
            const auto found = facts.Port0->find(instance);
            if (found != facts.Port0->end()) opened = found->second;
        }
        lines.push_back(Formatted(
               "TRIAEVUM_PAD joystick index=%d instance=%d name=\"%s\" guid=%s vendor=0x%04X product=0x%04X "
               "version=0x%04X joystick_type=%d controller_type=%d is_game_controller=%d has_mapping=%d virtual=%d "
               "opened_port0=%d full_gamepad=%d",
               index, static_cast<int>(instance), Clipped(SDL_JoystickNameForIndex(index)).c_str(), guid,
               static_cast<unsigned>(SDL_JoystickGetDeviceVendor(index)),
               static_cast<unsigned>(SDL_JoystickGetDeviceProduct(index)),
               static_cast<unsigned>(SDL_JoystickGetDeviceProductVersion(index)),
               static_cast<int>(SDL_JoystickGetDeviceType(index)), static_cast<int>(SDL_GameControllerTypeForIndex(index)),
               SDL_IsGameController(index) == SDL_TRUE ? 1 : 0, hasMapping ? 1 : 0,
               SDL_JoystickIsVirtual(index) == SDL_TRUE ? 1 : 0, opened != nullptr ? 1 : 0,
               opened != nullptr && ThreeDsRecomp::Input::IsFullSdlGamepad(opened) ? 1 : 0));
    }
    SDL_UnlockJoysticks();
    PadLog("TRIAEVUM_PAD devices reason=%s joysticks=%d port0_pads=%zu", reason, joysticks,
           facts.Port0 != nullptr ? facts.Port0->size() : size_t{0});
    for (const std::string& line : lines) PadLog("%s", line.c_str());
    if (facts.Port0 != nullptr) {
        for (const auto& [instance, controller] : *facts.Port0) {
            SDL_Joystick* joystick = controller != nullptr ? SDL_GameControllerGetJoystick(controller) : nullptr;
            PadLog("TRIAEVUM_PAD opened instance=%d name=\"%s\" attached=%d axes=%d buttons=%d hats=%d full_gamepad=%d",
                   static_cast<int>(instance),
                   Clipped(controller != nullptr ? SDL_GameControllerName(controller) : nullptr).c_str(),
                   controller != nullptr && SDL_GameControllerGetAttached(controller) == SDL_TRUE ? 1 : 0,
                   joystick != nullptr ? SDL_JoystickNumAxes(joystick) : -1,
                   joystick != nullptr ? SDL_JoystickNumButtons(joystick) : -1,
                   joystick != nullptr ? SDL_JoystickNumHats(joystick) : -1,
                   ThreeDsRecomp::Input::IsFullSdlGamepad(controller) ? 1 : 0);
        }
    }
    const char* selectedName = facts.Selected != nullptr ? SDL_GameControllerName(facts.Selected) : nullptr;
    PadLog("TRIAEVUM_PAD port0 result=%s selected_instance=%d selected_name=\"%s\" controller_enabled=%d "
           "preferred_guid=\"%s\" preferred_serial=\"%s\" host_gui=%d touch_active=%d",
           SelectionResult(facts, joysticks), static_cast<int>(facts.SelectedInstance), Clipped(selectedName).c_str(),
           facts.ControllerEnabled ? 1 : 0, Clipped(std::string(facts.PreferredGuid).c_str()).c_str(),
           Clipped(std::string(facts.PreferredSerial).c_str()).c_str(), facts.HostGuiVisible ? 1 : 0,
           facts.TouchActive ? 1 : 0);
}

// The selected pad's buttons as the game's poll reads them: bit i = SDL_GameControllerButton i, bit 30/31
// = the left/right trigger past half, bits 24-27 = the left stick past half (left, right, up, down).
uint32_t PadState(SDL_GameController* pad) {
    if (pad == nullptr || SDL_GameControllerGetAttached(pad) != SDL_TRUE) return 0;
    uint32_t state = 0;
    for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX && button < 24; ++button)
        if (SDL_GameControllerGetButton(pad, static_cast<SDL_GameControllerButton>(button)) != 0) state |= 1U << button;
    const int lx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    const int ly = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (lx <= -16384) state |= 1U << 24;
    if (lx >= 16384) state |= 1U << 25;
    if (ly <= -16384) state |= 1U << 26;
    if (ly >= 16384) state |= 1U << 27;
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >= 16384) state |= 1U << 30;
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >= 16384) state |= 1U << 31;
    return state;
}

}  // namespace

void ObservePadPoll(const PadPollFacts& facts) {
    const PadLogLevel level = Level();
    if (level == PadLogLevel::Off) return;
    static bool started = false;
    static uint32_t seenEvents = 0;
    static int32_t lastSelected = -2;
    static bool lastEnabled = true;
    static size_t lastPort0 = 0;
    static uint32_t lastState = 0;
    static auto nextStatus = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const char* reason = nullptr;
    if (!started) {
        started = true;
        SDL_AddEventWatch(WatchPadEvents, nullptr);
        SDL_version compiled{}, linked{};
        SDL_VERSION(&compiled);
        SDL_GetVersion(&linked);
        PadLog("TRIAEVUM_PAD startup sdl=%u.%u.%u built=%u.%u.%u init_joystick=%d init_gamecontroller=%d "
               "hint_hidapi=%s env_hidapi=%s accelerometer_as_joystick=%s joy_events=%d controller_events=%d "
               "input_log=%d",
               linked.major, linked.minor, linked.patch, compiled.major, compiled.minor, compiled.patch,
               SDL_WasInit(SDL_INIT_JOYSTICK) != 0U ? 1 : 0, SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0U ? 1 : 0,
               HintValue(SDL_HINT_JOYSTICK_HIDAPI), EnvValue("SDL_JOYSTICK_HIDAPI"),
               HintValue(SDL_HINT_ACCELEROMETER_AS_JOYSTICK), SDL_JoystickEventState(SDL_QUERY),
               SDL_GameControllerEventState(SDL_QUERY), level == PadLogLevel::Input ? 1 : 0);
        reason = "startup";
    }
    const size_t port0 = facts.Port0 != nullptr ? facts.Port0->size() : 0U;
    const uint32_t events = gDeviceEvents.load();
    if (reason == nullptr) {
        if (events != seenEvents) reason = "device_event";
        else if (facts.SelectedInstance != lastSelected || facts.ControllerEnabled != lastEnabled || port0 != lastPort0)
            reason = "selection";
        else if (Explicit() && now >= nextStatus) reason = "status";
    }
    if (reason != nullptr) {
        LogDevices(reason, facts);
        seenEvents = events;
        lastSelected = facts.SelectedInstance;
        lastEnabled = facts.ControllerEnabled;
        lastPort0 = port0;
        nextStatus = now + std::chrono::seconds(30);
    }
    if (level == PadLogLevel::Input) {
        const uint32_t state = PadState(facts.Selected);
        if (state != lastState) {
            PadLog("TRIAEVUM_PAD poll buttons=0x%08X pressed=0x%08X released=0x%08X instance=%d enabled=%d host_gui=%d",
                   state, state & ~lastState, lastState & ~state, static_cast<int>(facts.SelectedInstance),
                   facts.ControllerEnabled ? 1 : 0, facts.HostGuiVisible ? 1 : 0);
            lastState = state;
        }
    }
}

}  // namespace Oot3dNativeGame
