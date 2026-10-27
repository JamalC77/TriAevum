// TriAevum Quest immersive mode: Touch controller actions and bindings, the pad snapshot the game
// polls, stick-click chords (recenter, F1, Select, in-game menu), haptics and the ImGui gamepad feed.
// q3-spec §7, vr-camera.md §7.
#include "quest_xr_internal.h"
#include "quest_xr_input_logic.h"

#include <SDL2/SDL.h>
#include <imgui.h>

#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace TriAevumQuestXr {

namespace {

// SDL_GameControllerButton values (PadSnapshot::Buttons bit indices).
constexpr uint32_t kA = 1U << 0, kB = 1U << 1, kX = 1U << 2, kY = 1U << 3, kBack = 1U << 4, kStart = 1U << 6,
                   kRightStick = 1U << 8, kLeftShoulder = 1U << 9, kRightShoulder = 1U << 10,
                   kDpadUp = 1U << 11, kDpadDown = 1U << 12;

struct Actions {
    XrActionSet Set = XR_NULL_HANDLE;
    XrAction LeftStick = XR_NULL_HANDLE, RightStick = XR_NULL_HANDLE;
    XrAction A = XR_NULL_HANDLE, B = XR_NULL_HANDLE, X = XR_NULL_HANDLE, Y = XR_NULL_HANDLE;
    XrAction GripL = XR_NULL_HANDLE, GripR = XR_NULL_HANDLE, TriggerL = XR_NULL_HANDLE, TriggerR = XR_NULL_HANDLE;
    XrAction Menu = XR_NULL_HANDLE, ClickL = XR_NULL_HANDLE, ClickR = XR_NULL_HANDLE;
    XrAction HapticL = XR_NULL_HANDLE, HapticR = XR_NULL_HANDLE;
};
Actions gActions;
bool gHapticsBound = false;

// Published to the game thread.
std::mutex gPadMutex;
PadSnapshot gPad;
Logic::PressLatch gLatch;

// XR thread only.
Logic::StickChords gChords;
Logic::Pulse gSelectPulse, gMenuPulse;
// In-world (W2): right stick flicks while the world shows (snap turns to the guest, D-pad pulses here).
Logic::StickFlicks gFlicks;
Logic::Pulse gDpadUpPulse, gDpadDownPulse;
std::atomic<bool> gWorldFlicks{false};  // set by the guest (SetWorldFlicks)
bool gFlicksTaken = false;              // XR thread: the last SyncInput took flicks (log edges)
std::atomic<int32_t> gSnapTurns{0};
std::atomic<float> gTurnAxis{0.0F};
Logic::Hysteresis gGripL, gGripR;
Logic::StickSourceTracker gStickSource;
uint32_t gLoggedInputs = 0;

XrPath Path(const char* text) {
    XrPath path = XR_NULL_PATH;
    xrStringToPath(G().Instance, text, &path);
    return path;
}

void CreateAction(XrAction& action, const char* name, const char* label, XrActionType type) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::strncpy(info.localizedActionName, label, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    info.actionType = type;
    const XrResult result = xrCreateAction(gActions.Set, &info, &action);
    if (XR_FAILED(result))
        throw std::runtime_error(std::string("xrCreateAction(") + name + ") failed: XrResult " +
                                 std::to_string(static_cast<int>(result)));
}

bool Suggest(const char* profile, const std::vector<std::pair<XrAction, const char*>>& bindings, bool haptics) {
    std::vector<XrActionSuggestedBinding> suggested;
    suggested.reserve(bindings.size());
    for (const auto& [action, path] : bindings) suggested.push_back({action, Path(path)});
    XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    info.interactionProfile = Path(profile);
    info.suggestedBindings = suggested.data();
    info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
    const XrResult result = xrSuggestInteractionProfileBindings(G().Instance, &info);
    XRLOG("TRIAEVUM_XR_BINDINGS profile=%s haptics=%d result=%d", profile, haptics ? 1 : 0, static_cast<int>(result));
    return XR_SUCCEEDED(result);
}

bool ReadBoolean(XrAction action) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(G().Session, &info, &state)) && state.isActive && state.currentState;
}

float ReadFloat(XrAction action) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(G().Session, &info, &state)) && state.isActive ? state.currentState
                                                                                             : 0.0F;
}

XrVector2f ReadVector(XrAction action, bool* active = nullptr) {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    const bool isActive = XR_SUCCEEDED(xrGetActionStateVector2f(G().Session, &info, &state)) && state.isActive;
    if (active != nullptr) *active = isActive;
    return isActive ? state.currentState : XrVector2f{0.0F, 0.0F};
}

void PushSettingsKey() {
    SDL_Event event{};
    event.type = SDL_KEYDOWN;
    event.key.timestamp = SDL_GetTicks();
    event.key.state = SDL_PRESSED;
    event.key.repeat = 0;
    event.key.keysym.scancode = SDL_SCANCODE_F1;
    event.key.keysym.sym = SDLK_F1;
    const int down = SDL_PushEvent(&event);
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    const int up = SDL_PushEvent(&event);
    XRLOG("TRIAEVUM_XR_F1 toggled push_down=%d push_up=%d", down, up);
}

void LogInputEdges(uint32_t inputs) {
    static const bool enabled = EnvFlag("TRIAEVUM_XR_INPUT_LOG");
    if (!enabled || inputs == gLoggedInputs) return;
    static constexpr std::array<const char*, 13> kNames{"a",         "b",         "x",      "y",
                                                        "grip_l",    "grip_r",    "trigger_l", "trigger_r",
                                                        "menu",      "click_l",   "click_r", "start", "select"};
    for (size_t bit = 0; bit < kNames.size(); ++bit) {
        const uint32_t mask = 1U << bit;
        if ((inputs ^ gLoggedInputs) & mask) XRLOG("TRIAEVUM_XR_INPUT %s=%d", kNames[bit], (inputs & mask) ? 1 : 0);
    }
    gLoggedInputs = inputs;
}

}  // namespace

void CreateActions() {
    auto& g = G();
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strncpy(setInfo.actionSetName, "ctr", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(setInfo.localizedActionSetName, "3DS controls", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    setInfo.priority = 0;
    const XrResult created = xrCreateActionSet(g.Instance, &setInfo, &gActions.Set);
    if (XR_FAILED(created))
        throw std::runtime_error("xrCreateActionSet failed: XrResult " + std::to_string(static_cast<int>(created)));
    CreateAction(gActions.LeftStick, "left_stick", "Circle Pad", XR_ACTION_TYPE_VECTOR2F_INPUT);
    CreateAction(gActions.RightStick, "right_stick", "C-Stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    CreateAction(gActions.A, "a", "A", XR_ACTION_TYPE_BOOLEAN_INPUT);
    CreateAction(gActions.B, "b", "B", XR_ACTION_TYPE_BOOLEAN_INPUT);
    CreateAction(gActions.X, "x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT);
    CreateAction(gActions.Y, "y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT);
    CreateAction(gActions.GripL, "grip_l", "L", XR_ACTION_TYPE_FLOAT_INPUT);
    CreateAction(gActions.GripR, "grip_r", "R", XR_ACTION_TYPE_FLOAT_INPUT);
    CreateAction(gActions.TriggerL, "trigger_l", "ZL", XR_ACTION_TYPE_FLOAT_INPUT);
    CreateAction(gActions.TriggerR, "trigger_r", "ZR", XR_ACTION_TYPE_FLOAT_INPUT);
    CreateAction(gActions.Menu, "menu", "Start", XR_ACTION_TYPE_BOOLEAN_INPUT);
    CreateAction(gActions.ClickL, "click_l", "Left stick click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    CreateAction(gActions.ClickR, "click_r", "Right stick click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    CreateAction(gActions.HapticL, "haptic_l", "Left haptics", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    CreateAction(gActions.HapticR, "haptic_r", "Right haptics", XR_ACTION_TYPE_VIBRATION_OUTPUT);

    const std::vector<std::pair<XrAction, const char*>> pad{
        {gActions.LeftStick, "/user/hand/left/input/thumbstick"},
        {gActions.RightStick, "/user/hand/right/input/thumbstick"},
        {gActions.A, "/user/hand/right/input/a/click"},
        {gActions.B, "/user/hand/right/input/b/click"},
        {gActions.X, "/user/hand/left/input/x/click"},
        {gActions.Y, "/user/hand/left/input/y/click"},
        {gActions.GripL, "/user/hand/left/input/squeeze/value"},
        {gActions.GripR, "/user/hand/right/input/squeeze/value"},
        {gActions.TriggerL, "/user/hand/left/input/trigger/value"},
        {gActions.TriggerR, "/user/hand/right/input/trigger/value"},
        {gActions.Menu, "/user/hand/left/input/menu/click"},
        {gActions.ClickL, "/user/hand/left/input/thumbstick/click"},
        {gActions.ClickR, "/user/hand/right/input/thumbstick/click"},
    };
    auto withHaptics = pad;
    withHaptics.emplace_back(gActions.HapticL, "/user/hand/left/output/haptic");
    withHaptics.emplace_back(gActions.HapticR, "/user/hand/right/output/haptic");
    std::vector<const char*> profiles{"/interaction_profiles/oculus/touch_controller"};
    if (g.HasTouchPlus) profiles.push_back("/interaction_profiles/meta/touch_controller_plus");
    if (g.HasTouchPro) profiles.push_back("/interaction_profiles/facebook/touch_controller_pro");
    bool anyBound = false;
    for (const char* profile : profiles) {
        // A later suggestion for a profile replaces an earlier one: retry the pad alone if the
        // runtime turns the haptic bindings down.
        if (Suggest(profile, withHaptics, true)) {
            gHapticsBound = true;
            anyBound = true;
        } else if (Suggest(profile, pad, false)) {
            anyBound = true;
        }
    }
    if (!anyBound) throw std::runtime_error("no Touch controller interaction profile accepted the bindings");
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &gActions.Set;
    const XrResult attached = xrAttachSessionActionSets(g.Session, &attach);
    if (XR_FAILED(attached))
        throw std::runtime_error("xrAttachSessionActionSets failed: XrResult " +
                                 std::to_string(static_cast<int>(attached)));
    XRLOG("TRIAEVUM_XR_ACTIONS ready profiles=%zu haptics=%d", profiles.size(), gHapticsBound ? 1 : 0);
}

void SyncInput(XrTime) {
    XrActiveActionSet active{gActions.Set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(G().Session, &sync))) return;
    const double now = NowSeconds();
    const bool a = ReadBoolean(gActions.A), b = ReadBoolean(gActions.B);
    const bool x = ReadBoolean(gActions.X), y = ReadBoolean(gActions.Y);
    const bool menu = ReadBoolean(gActions.Menu);
    const bool clickL = ReadBoolean(gActions.ClickL), clickR = ReadBoolean(gActions.ClickR);
    const bool gripL = gGripL.Update(ReadFloat(gActions.GripL));
    const bool gripR = gGripR.Update(ReadFloat(gActions.GripR));
    const float triggerL = ReadFloat(gActions.TriggerL), triggerR = ReadFloat(gActions.TriggerR);
    bool leftActive = false;
    const XrVector2f left = ReadVector(gActions.LeftStick, &leftActive), right = ReadVector(gActions.RightStick);
    // In-world while the world shows: the right stick also steers Link (single-controller play: the
    // left Touch can be off or asleep), so the in-game menu moves from an R3 tap to a deliberate R3
    // hold with the right stick centred (quest_xr_input_logic.h). Screens keep the Q3 mapping.
    const bool worldShown = WorldRequested() && RequestedLayerMode() == LayerMode::World;

    const auto chords = gChords.Update(now, clickL, clickR, menu, worldShown, Logic::StickIdle(right.x, right.y));
    if (chords.SelectTap) gSelectPulse.Fire(now);
    if (chords.MenuTap) {
        gMenuPulse.Fire(now);
        if (worldShown) Haptic(false, true, 40);  // the hold took: a short buzz on the right hand
    }
    if (chords.Recenter) {
        if (WorldRequested()) {
            // In-world: the chord re-places the world around Link (§1.1 G9), not the screen.
            NoteWorldRecenter("chord");
            Haptic(true, true, 60);
        } else {
            RequestRecenter();
        }
    }
    if (chords.ToggleSettings) {
        PushSettingsKey();
        Haptic(true, false, 40);
    }
    // In-world (inworld-spec §5.5, §7): while the world shows and flicks are armed (mode R, mode L), right
    // stick X flicks are snap turns (the guest applies them at Camera_Update) and Y flicks press the D-pad
    // (up: TopScreen's first-person view), but only while the left controller is active: without it the
    // right stick moves Link instead. The game's C-stick sees nothing (TakePad).
    const auto job = Logic::WorldRightStickJob(worldShown, gWorldFlicks.load(), leftActive);
    if (job.Flicks != gFlicksTaken && worldShown) {
        XRLOG("TRIAEVUM_XR_INPUT flicks=%d left_active=%d armed=%d", job.Flicks ? 1 : 0, leftActive ? 1 : 0,
              gWorldFlicks.load() ? 1 : 0);
    }
    gFlicksTaken = job.Flicks;
    if (job.Flicks) {
        const auto flicks = gFlicks.Update(right.x, right.y);
        if (flicks.Turn != 0) {
            gSnapTurns.fetch_add(flicks.Turn);
            XRLOG("TRIAEVUM_XR_FLICK axis=x dir=%s", flicks.Turn > 0 ? "right" : "left");
        }
        if (flicks.Vertical != 0) {
            (flicks.Vertical > 0 ? gDpadUpPulse : gDpadDownPulse).Fire(now);
            XRLOG("TRIAEVUM_XR_FLICK axis=y dir=%s dpad=%s", flicks.Vertical > 0 ? "up" : "down",
                  flicks.Vertical > 0 ? "up" : "down");
        }
        gTurnAxis.store(std::isfinite(right.x) ? right.x : 0.0F);
    } else {
        gFlicks.Reset();
        gTurnAxis.store(0.0F);
    }

    PadSnapshot pad;
    pad.Buttons = (a ? kA : 0U) | (b ? kB : 0U) | (x ? kX : 0U) | (y ? kY : 0U) |
                  (gSelectPulse.Held(now) ? kBack : 0U) | (chords.StartHeld ? kStart : 0U) |
                  (gMenuPulse.Held(now) ? kRightStick : 0U) | (gripL ? kLeftShoulder : 0U) |
                  (gripR ? kRightShoulder : 0U) | (gDpadUpPulse.Held(now) ? kDpadUp : 0U) |
                  (gDpadDownPulse.Held(now) ? kDpadDown : 0U);
    // OpenXR thumbsticks are already Y up-positive, the PhysicalInputState convention.
    pad.LeftX = Logic::AxisToSdl(left.x);
    pad.LeftY = Logic::AxisToSdl(left.y);
    if (worldShown) {
        // The Circle Pad: the left stick, or the right one while the left is idle or inactive (flicks armed:
        // only while the left controller is inactive); the larger deflection wins. TakePad still hides the
        // right stick from the game's C-stick.
        const auto move = Logic::PickMoveStick(left.x, left.y, leftActive, right.x, right.y, job.Move);
        pad.LeftX = Logic::AxisToSdl(move.X);
        pad.LeftY = Logic::AxisToSdl(move.Y);
        if (gStickSource.Update(move.Source))
            XRLOG("TRIAEVUM_XR_INPUT stick_source=%s left_active=%d", Logic::StickSourceName(move.Source),
                  leftActive ? 1 : 0);
    }
    pad.RightX = Logic::AxisToSdl(right.x);
    pad.RightY = Logic::AxisToSdl(right.y);
    pad.TriggerLeft = Logic::TriggerToSdl(triggerL);
    pad.TriggerRight = Logic::TriggerToSdl(triggerR);
    {
        std::lock_guard lock(gPadMutex);
        gPad = pad;
        gLatch.Observe(pad.Buttons);
    }
    LogInputEdges((a ? 1U : 0U) | (b ? 2U : 0U) | (x ? 4U : 0U) | (y ? 8U : 0U) | (gripL ? 16U : 0U) |
                  (gripR ? 32U : 0U) | (triggerL > 0.5F ? 64U : 0U) | (triggerR > 0.5F ? 128U : 0U) |
                  (menu ? 256U : 0U) | (clickL ? 512U : 0U) | (clickR ? 1024U : 0U) |
                  (chords.StartHeld ? 2048U : 0U) | (gSelectPulse.Held(now) ? 4096U : 0U));
}

void ReleaseInput() {
    gChords.Reset();
    gSelectPulse = {};
    gMenuPulse = {};
    gDpadUpPulse = {};
    gDpadDownPulse = {};
    gFlicks.Reset();
    gFlicksTaken = false;
    gTurnAxis.store(0.0F);
    gGripL.On = gGripR.On = false;
    gStickSource = {};
    std::lock_guard lock(gPadMutex);
    gPad = {};
    gLatch.Reset();
}

bool TakePad(PadSnapshot* pad) {
    if (!Requested()) return false;
    const auto& g = G();
    if (!g.Focused.load() || g.SessionStopped.load() || g.Exiting.load()) return false;
    std::lock_guard lock(gPadMutex);
    *pad = gPad;
    pad->Buttons = gLatch.Take(gPad.Buttons);
    // In-world, while the world shows: the right stick is not the game's C-stick (inworld-spec §7); it
    // reaches the game only as the Circle Pad while the left stick is idle (SyncInput). Cinema and
    // menus keep the raw Q3 mapping.
    if (WorldRequested() && RequestedLayerMode() == LayerMode::World) pad->RightX = pad->RightY = 0;
    return true;
}

int32_t TakeSnapTurns() { return gSnapTurns.exchange(0); }

void SetWorldFlicks(bool armed) {
    gWorldFlicks.store(armed);
    if (!armed) gSnapTurns.store(0);
    XRLOG("TRIAEVUM_XR_INPUT right_stick=%s", armed ? "flicks (moves Link only with the left controller off)"
                                                    : "move (W1: moves Link while the left stick is idle)");
}

float WorldTurnAxis() { return gTurnAxis.load(); }

void Haptic(bool left, bool right, int milliseconds) {
    if (!gHapticsBound || G().Session == XR_NULL_HANDLE) return;
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.amplitude = 0.6F;
    vibration.duration = static_cast<XrDuration>(milliseconds) * 1'000'000;
    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
    for (const auto& [enabled, action] : {std::pair{left, gActions.HapticL}, std::pair{right, gActions.HapticR}}) {
        if (!enabled) continue;
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = action;
        xrApplyHapticFeedback(G().Session, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
    }
}

void LogInteractionProfiles() {
    const auto& g = G();
    for (const char* hand : {"/user/hand/left", "/user/hand/right"}) {
        XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_FAILED(xrGetCurrentInteractionProfile(g.Session, Path(hand), &state))) continue;
        char text[XR_MAX_PATH_LENGTH] = "none";
        uint32_t length = 0;
        if (state.interactionProfile != XR_NULL_PATH)
            xrPathToString(g.Instance, state.interactionProfile, sizeof text, &length, text);
        XRLOG("TRIAEVUM_XR_PROFILE hand=%s profile=%s", hand, text);
    }
}

void DestroyActions() {
    if (gActions.Set != XR_NULL_HANDLE) xrDestroyActionSet(gActions.Set);  // destroys its actions
    gActions = {};
    gHapticsBound = false;
}

void FeedImGui(ImGuiIO& io) {
    if (!Requested()) return;
    // ImGui_ImplSDL2_NewFrame sized ImGui after the hidden immersive surface; the overlay draws into
    // the private panel target.
    const VkExtent2D panel = PanelExtent();
    io.DisplaySize = ImVec2(static_cast<float>(panel.width), static_cast<float>(panel.height));
    io.DisplayFramebufferScale = ImVec2(1.0F, 1.0F);

    const auto& g = G();
    const bool active = g.Focused.load() && !g.SessionStopped.load();
    PadSnapshot pad;
    if (active) {
        std::lock_guard lock(gPadMutex);
        pad = gPad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    }
    const auto axis = [](int16_t value) { return static_cast<float>(value) / 32767.0F; };
    const float lx = axis(pad.LeftX), ly = axis(pad.LeftY), rx = axis(pad.RightX), ry = axis(pad.RightY);
    struct Digital {
        ImGuiKey Key;
        bool Down;
    };
    const Digital digital[] = {
        {ImGuiKey_GamepadFaceDown, (pad.Buttons & kA) != 0U},  {ImGuiKey_GamepadFaceRight, (pad.Buttons & kB) != 0U},
        {ImGuiKey_GamepadFaceLeft, (pad.Buttons & kX) != 0U},  {ImGuiKey_GamepadFaceUp, (pad.Buttons & kY) != 0U},
        {ImGuiKey_GamepadL1, (pad.Buttons & kLeftShoulder) != 0U},
        {ImGuiKey_GamepadR1, (pad.Buttons & kRightShoulder) != 0U},
        {ImGuiKey_GamepadDpadLeft, lx < -0.5F},                {ImGuiKey_GamepadDpadRight, lx > 0.5F},
        {ImGuiKey_GamepadDpadUp, ly > 0.5F},                   {ImGuiKey_GamepadDpadDown, ly < -0.5F},
    };
    static bool sent[sizeof digital / sizeof digital[0]] = {};
    for (size_t index = 0; index < sizeof digital / sizeof digital[0]; ++index) {
        if (digital[index].Down == sent[index]) continue;
        io.AddKeyEvent(digital[index].Key, digital[index].Down);
        sent[index] = digital[index].Down;
    }
    struct Analog {
        ImGuiKey Key;
        float Value;
    };
    const auto positive = [](float value) { return value > 0.0F ? value : 0.0F; };
    const Analog analog[] = {
        {ImGuiKey_GamepadL2, axis(pad.TriggerLeft)},  {ImGuiKey_GamepadR2, axis(pad.TriggerRight)},
        {ImGuiKey_GamepadLStickLeft, positive(-lx)},  {ImGuiKey_GamepadLStickRight, positive(lx)},
        {ImGuiKey_GamepadLStickUp, positive(ly)},     {ImGuiKey_GamepadLStickDown, positive(-ly)},
        {ImGuiKey_GamepadRStickLeft, positive(-rx)},  {ImGuiKey_GamepadRStickRight, positive(rx)},
        {ImGuiKey_GamepadRStickUp, positive(ry)},     {ImGuiKey_GamepadRStickDown, positive(-ry)},
    };
    static float sentAnalog[sizeof analog / sizeof analog[0]] = {};
    for (size_t index = 0; index < sizeof analog / sizeof analog[0]; ++index) {
        // Quantized, so only changes become events (a Bluetooth pad's backend events still win).
        const float value = std::round(analog[index].Value * 32.0F) / 32.0F;
        if (value == sentAnalog[index]) continue;
        io.AddKeyAnalogEvent(analog[index].Key, value > 0.1F, value);
        sentAnalog[index] = value;
    }
}

}  // namespace TriAevumQuestXr
