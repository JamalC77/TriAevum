// oot3d_native_pad_diagnostics.h: the SDL pad path as device-log lines (TRIAEVUM_PAD ...), for the Quest,
// where spdlog's info lines (the device manager's own "SDL controllers initialized" and its mapping
// warnings) never reach the logs. Lines go to stderr (<ext>/logs/native-stderr.log on Android) and, on
// Android, to logcat with the tag TriAevumPad (the immersive activity's Java lines use the same tag).
//
//  - At the first input poll and after every SDL joystick/controller add, remove or remap (and every
//    30 s): SDL's view (version, subsystems, hints), every joystick SDL knows (name, GUID, vendor and
//    product, type, IsGameController, mapping, virtual, opened by the device manager on port 0, both
//    sticks), the opened pads, and the port-0 selection with Controls' ControllerEnabled and preference.
//  - The add/remove/remap events themselves, as SDL posts them (any thread).
//  - With TRIAEVUM_XR_INPUT_LOG=1 or TRIAEVUM_PAD_LOG=2: raw SDL joystick and controller button, hat and
//    axis events and key events (an Android pad press arrives as a key when SDL has no opened joystick
//    for it), and the selected pad's button edges as the game's input poll reads them.
//
// On by default on Android; elsewhere TRIAEVUM_PAD_LOG=1 (or 2). TRIAEVUM_PAD_LOG=0 turns it off.
#pragma once

#include <SDL2/SDL.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace Oot3dNativeGame {

struct PadPollFacts {
    // The device manager's pads for port 0 (instance id -> handle), as the selection saw them.
    const std::unordered_map<int32_t, SDL_GameController*>* Port0 = nullptr;
    int32_t SelectedInstance = -1;
    SDL_GameController* Selected = nullptr;
    bool ControllerEnabled = true;  // Controls: the pad is admitted to gameplay
    std::string_view PreferredGuid;
    std::string_view PreferredSerial;
    bool HostGuiVisible = false;    // F1, the menu bar or the in-game menu withholds the pad
    bool TouchActive = false;       // Quest immersive: the OpenXR Touch snapshot is live
};

// PollNativeA32Input, on the game thread, after the port-0 selection.
void ObservePadPoll(const PadPollFacts& facts);

}  // namespace Oot3dNativeGame
