// Native A32 window session: one frame of the loop and the renderer-side work it schedules.
#include "oot3d_native_a32_window_session.h"
#include "fast/renderer3ds/pica_surface_debug.h"
#include "oot3d/renderer/ui_presentation_layout.h"
#include "oot3d_native_room_shadow_shell.h"
#if defined(TRIAEVUM_QUEST_OPENXR)
#include "quest_xr.h"
#endif

namespace Oot3dA32Window {

namespace {

uint64_t HashDiagnosticBytes(std::span<const uint8_t> bytes) noexcept {
  uint64_t hash = 14695981039346656037ULL;
  for (const uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool IsNativeFrontendOpaqueTargetInitializationDraw(
    const Oot3dNativeGame::Oot3dPicaDrawSubmission &draw) {
  const auto &state = draw.State;
  const auto &blend = state.OutputMerger.Blend;
  const auto &depth = state.OutputMerger.Depth;
  const bool hasTexture =
      std::any_of(state.Textures.begin(), state.Textures.end(),
                  [](const auto &texture) { return texture.Enabled; });
  // Native UI targets include rotated/padded storage outside the logical
  // viewport, so framebuffer dimensions cannot establish whether this copy
  // quad initializes the frontend canvas. The replacement blend/depth state
  // identifies that operation; its association with CommonBackground00's
  // target AND viewport is checked by the caller, preserving the scene clear.
  return !hasTexture && state.VertexInput.Indexed &&
         state.VertexInput.VertexCount == 4U &&
         state.OutputMerger.ColorWriteMask == 0x0FU &&
         state.OutputMerger.LogicOperation ==
             Oot3dNativeGame::Oot3dPicaLogicOperation::Copy &&
         blend.Enabled &&
         blend.SourceColor == Oot3dNativeGame::Oot3dPicaBlendFactor::One &&
         blend.DestinationColor ==
             Oot3dNativeGame::Oot3dPicaBlendFactor::Zero &&
         blend.SourceAlpha == Oot3dNativeGame::Oot3dPicaBlendFactor::One &&
         blend.DestinationAlpha ==
             Oot3dNativeGame::Oot3dPicaBlendFactor::Zero &&
         depth.TestEnabled && depth.WriteEnabled &&
         depth.Compare == Oot3dNativeGame::Oot3dPicaCompareFunction::Always;
}

Oot3dNativeGame::NativeGuestClockDeadlineResolution AdvanceGuestClockTo(
    Oot3dNativeGame::NativeA32Process &process,
    Oot3dNativeGame::NativeA32CtrHostServices &hostServices,
    Oot3dNativeGame::NativeA32ProcessRunResult &processResult,
    uint64_t deadline, uint32_t wholeAotBlockBudget) {
  auto resolution = Oot3dNativeGame::ResolveNativeGuestClockDeadline(
      hostServices.SystemTicks(), deadline);
  if (resolution.OvershootTicks != 0U) {
    return resolution;
  }
  while (processResult.Kind !=
         Oot3dNativeGame::NativeA32ProcessRunKind::Terminated) {
    const auto wakeTick = hostServices.NextSleepWakeTick();
    if (!wakeTick.has_value() || *wakeTick > deadline) {
      break;
    }
    const uint64_t currentTick = hostServices.SystemTicks();
    hostServices.AdvanceSystemTicks(
        *wakeTick > currentTick ? *wakeTick - currentTick : 0U);
    processResult = RunUntilGuestWait(process, wholeAotBlockBudget);
    RequireRunnableGuest(processResult);
    resolution = Oot3dNativeGame::ResolveNativeGuestClockDeadline(
        hostServices.SystemTicks(), deadline);
    if (resolution.OvershootTicks != 0U) {
      return resolution;
    }
  }
  resolution = Oot3dNativeGame::ResolveNativeGuestClockDeadline(
      hostServices.SystemTicks(), deadline);
  if (resolution.AdvanceTicks != 0U) {
    hostServices.AdvanceSystemTicks(resolution.AdvanceTicks);
  }
  return resolution;
}

// The hold rings (0x005CCA90): the D-pad picker's X clear and the shortcut
// row's A open. Black sectors, then the yellow fill, as free quads of the
// OcarinaPage texture under their layer-38 glyphs.
void AppendTopScreenDpadPickerHoldRings(
    const Oot3dNativeGame::Oot3dNativeUiLifecycleBridge &bridge,
    std::vector<oot3d::ui::UiPrimitive> &primitives) {
  const auto &clear = bridge.TopScreenDpadPickerCursor();
  const auto &open = bridge.TopScreenItemsShortcutCursor();
  if (clear.Count == 0U && open.Count == 0U) {
    return;
  }
  const auto texture = bridge.NativePauseSharedTextureIdentity(
      oot3d::ui::UiPauseSharedTextureSlot::OcarinaPage);
  if (!texture.has_value()) {
    return;
  }
  unsigned sourceQuad = 0U;
  for (const auto *ring : {&clear, &open}) {
    const unsigned count = std::min<unsigned>(ring->Count, ring->Quads.size());
    for (unsigned index = 0; index < count; ++index) {
      const auto &quad = ring->Quads[index];
      oot3d::ui::UiPrimitive primitive;
      primitive.subsystem = oot3d::ui::UiSubsystem::Items;
      primitive.role = oot3d::ui::UiPrimitiveRole::PauseCursor;
      primitive.owner_address = 0x005CCA90U;
      primitive.source_quad = sourceQuad++;
      primitive.texture = *texture;
      // 0x005CBD64's material: RGB is the vertex colour; alpha is the
      // filtered texel alpha (the opaque 425..426 line) times the vertex
      // alpha, scaled by 2 and saturated.
      primitive.texture_alpha_only = true;
      primitive.texture_alpha_scale = ring->TextureAlphaScale;
      primitive.color = quad.Color;
      primitive.layer = 37U;
      std::array<oot3d::ui::UiVertex, 4> corners{};
      float left = quad.Vertices[0].X, right = left;
      float top = quad.Vertices[0].Y, bottom = top;
      for (std::size_t corner = 0; corner < corners.size(); ++corner) {
        const auto &vertex = quad.Vertices[corner];
        corners[corner] = {vertex.X, vertex.Y, vertex.U, vertex.V};
        left = std::min(left, vertex.X);
        right = std::max(right, vertex.X);
        top = std::min(top, vertex.Y);
        bottom = std::max(bottom, vertex.Y);
      }
      primitive.destination = {left, top, right - left, bottom - top};
      primitive.corners = corners;
      primitives.push_back(std::move(primitive));
    }
  }
}

} // namespace

bool NativeA32WindowSession::RunFrame() {
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &window = *windowPointer;
  auto &api = *apiPointer;
  auto &preparationScreen = *preparationScreenStorage;
  auto &ocarinaText = *ocarinaTextStorage;
  auto &n64UiRenderer = *n64UiRendererStorage;
  auto &inGameMenu = *inGameMenuStorage;
  auto &picaSemanticTrace = *picaSemanticTraceStorage;
  auto &presentationScheduler = *presentationSchedulerStorage;
  auto &realtimePacer = *realtimePacerStorage;
  const auto phaseBeforeFrame = phaseTiming.Values();
  const double nestedGuestBeforeFrame = phaseTiming.GuestInsidePicaSubmitSeconds;
  const double guestClockBeforeFrame = phaseTiming.GuestClockSeconds;
  widescreenProjection.CurrentHostFrame =
      static_cast<uint32_t>(presentationFrameCount);
  WindowDemoFrameTiming frameTiming;
  // Input sampling waits come first. With parallel presentation the next
  // frame starts only once the previous one has ended (presented); then it
  // begins its renderer frame (frame slot fence and swapchain image
  // acquisition, where VSync waits) and only then pumps events and polls
  // input. Sampled before those waits, the input aged behind them: at 60 Hz
  // with VSync, 33 ms median parallel and 16 serial, now 8-10 ms at the same
  // frame rate (docs/TRIAEVUM_PARALLEL_PRESENTATION.md).
  // TRIAEVUM_INPUT_ADMISSION=1 keeps only the wait for the previous frame and
  // =0 restores the early sampling, for comparisons.
  static const char *inputAdmissionMode = std::getenv("TRIAEVUM_INPUT_ADMISSION");
  static const bool inputAdmission =
      inputAdmissionMode == nullptr || std::string_view(inputAdmissionMode) != "0";
  static const bool lateInputSample =
      inputAdmission && (inputAdmissionMode == nullptr ||
                         std::string_view(inputAdmissionMode) != "1");
  if (inputAdmission) renderStream.WaitForFramesInFlight(0U);
  if (!PrepareNextWindowDemoFrame(hostArgs, window, timing, frameTiming)) {
    return true;
  }
  // The SDL events just pumped are the physical input this frame polls.
  auto inputPumpTime = std::chrono::steady_clock::now();
  const auto requestedPresentationPolicy =
      Fast::Oot3d::ResolvePresentationPacingPolicy(
          Fast::Oot3d::GraphicsSettingsRuntime::Instance()
              .Snapshot()
              .FrameRate);
#if defined(TRIAEVUM_QUEST_OPENXR)
  // Immersive mode has no swapchain: the realtime pacer is the guest's only clock.
  if (TriAevumQuestXr::Requested())
    TriAevumQuestXr::NotePacing(requestedPresentationPolicy.TargetRateHz,
                                requestedPresentationPolicy.Enabled ? "paced" : "uncapped");
#endif
  if (Fast::Oot3d::PresentationPacingPolicyChanged(
          presentationPacingPolicy, requestedPresentationPolicy)) {
    const auto requestedCompositionPolicy =
        ResolveFrameCompositionPolicy(requestedPresentationPolicy);
    const bool previousInterpolationEnabled = guestInterpolationEnabled;
    const bool requestedInterpolationEnabled =
        Oot3dNativeGame::ShouldUseNativeVisualInterpolation(
            frameRateContract,
            requestedCompositionPolicy.InterpolationEnabled());
    const bool compositionModeChanged =
        guestCompositionPolicy.Interpolation !=
            requestedCompositionPolicy.Interpolation ||
        guestCompositionPolicy.FixedSampleMultiplier !=
            requestedCompositionPolicy.FixedSampleMultiplier;
    presentationPacingPolicy = requestedPresentationPolicy;
    guestCompositionPolicy = requestedCompositionPolicy;
    guestInterpolationEnabled = requestedInterpolationEnabled;
    renderStream.Submit([&, requestedPresentationPolicy,
                         requestedCompositionPolicy,
                         requestedInterpolationEnabled,
                         previousInterpolationEnabled,
                         compositionModeChanged] {
    frameCompositionPolicy = requestedCompositionPolicy;
    realtimePacer.Configure(requestedPresentationPolicy.Enabled &&
                                !hostArgs.ThroughputBenchmark,
                            requestedPresentationPolicy.TargetRateHz);
    visualSampleCadence.Configure(
        frameCompositionPolicy.FixedMultiplier()
            ? frameCompositionPolicy.FixedSampleMultiplier
            : 0U);
    visualInterpolationEnabled = requestedInterpolationEnabled;
    if (compositionModeChanged) {
      ++visualContinuityEpoch;
    }
    if (previousInterpolationEnabled != visualInterpolationEnabled) {
      preparedVisualTransition.Reset();
      visualContinuityTracker = {};
      previousVisualFrame.reset();
      latestVisualFrame.reset();
      latestVisualTransitionContinuous = false;
      visualSampleCadence.Reset();
    }
    });
  }
  const auto presentationTime = std::chrono::steady_clock::now();
  renderStream.Submit([&, presentationTime] {
    realtimePacer.ObservePresentation(presentationTime);
  });
  double presentationElapsedSeconds =
      hostArgs.FixedDeltaSeconds > 0.0
          ? hostArgs.FixedDeltaSeconds
          : std::chrono::duration<double>(presentationTime -
                                          lastPresentationTime)
                .count();
  lastPresentationTime = presentationTime;
#if defined(TRIAEVUM_QUEST_OPENXR)
  if (xrWorld != nullptr && TriAevumQuestXr::PerfEnabled()) {
    // TRIAEVUM_XR_PERF (guest): one host frame, its CPU, its waits for the render worker.
    namespace Xr = TriAevumQuestXr;
    const double cpu = Xr::ThreadCpuMs();
    if (xrPerfGuestCpu >= 0.0) {
      Xr::PerfNote(Xr::PerfPhase::GuestFrame,
                   std::chrono::duration<double, std::milli>(presentationTime - xrPerfGuestFrame).count());
      Xr::PerfNote(Xr::PerfPhase::GuestCpu, cpu - xrPerfGuestCpu);
      Xr::PerfNote(Xr::PerfPhase::GuestStreamWait,
                   static_cast<double>(renderStream.TakeSubmitterWaitNs()) * 1e-6);
    } else {
      (void)renderStream.TakeSubmitterWaitNs();
    }
    xrPerfGuestFrame = presentationTime;
    xrPerfGuestCpu = cpu;
  }
#endif
  if (xrWorld != nullptr && hostArgs.FixedDeltaSeconds <= 0.0) {
    // In-world: eyes locked 3:1 to a 90 Hz display; this frame's presentation time comes from the
    // display ticks (p_N - p_{N-1}), so the scheduler gives exactly one guest refresh per 3 ticks
    // (inworld-spec §3.4; FixedDeltaSeconds is the precedent).
    double tickElapsed = 0.0;
    if (xrWorld->BeginPresentation(&tickElapsed)) presentationElapsedSeconds = tickElapsed;
    double audioBufferedMs = -1.0;
    if (audioPlayer != nullptr && audioPlayer->IsInitialized() && audioPlayer->GetSampleRate() > 0)
      audioBufferedMs = 1000.0 * audioPlayer->Buffered() / audioPlayer->GetSampleRate();
    xrWorld->LogPeriodic(audioBufferedMs);
  }
  const auto presentationStateBeforeFrame = presentationScheduler.CaptureState();
  const auto presentationStep =
      presentationScheduler.Advance(presentationElapsedSeconds);
  const uint32_t guestRefreshesDue = presentationStep.GuestRefreshesDue;
  if (xrWorld != nullptr) xrWorld->NoteRefreshesDue(guestRefreshesDue);
  const float visualInterpolationAlpha = static_cast<float>(
      std::clamp(presentationStep.InterpolationAlpha, 0.0, 1.0));
  ApplyTopScreenConfigSnapshot();
  ApplyPhotoModeHud();
  uiLifecycleBridge.BeginHostFrame(
      static_cast<uint32_t>(presentationFrameCount));
  if (topScreenTextureOverrideRuntime.has_value()) {
    topScreenTextureOverrideRuntime->Observe(uiLifecycleBridge);
  }

  const uint32_t width = std::max<uint32_t>(1, window.GetWidth());
  const uint32_t height = std::max<uint32_t>(1, window.GetHeight());
  hdFonts.SetOutputHeight(height);
  UpdateScenePresentation();
  // The presentation picture every UI canvas is fitted into this frame.
  const oot3d::ui::N64UiOutputRegion uiRegion{
      presentationPicture.X, presentationPicture.Y, presentationPicture.Width,
      presentationPicture.Height};
  phaseStart = std::chrono::steady_clock::now();
  Fast::Renderer3ds::SlowStepTrace hostStartSteps("host_frame_start");
  window.GetMouseStateManager()->StartFrame();
  hostStartSteps.Mark("mouse");
  gui->StartDraw();
  hostStartSteps.Mark("gui_start_draw");
  renderStream.Submit([&] { window.StartFrame(); });
  hostStartSteps.Mark("render_start_frame");
  phaseTiming.HostFrameStartSeconds += SecondsSince(phaseStart);
  // Developer qualification: scheduled settings take the F1 commit path
  // (applied, not persisted) before this frame is rendered.
  for (const auto *step : graphicsSettingsSchedule.TakeDue(runFrameCount)) {
    auto &graphicsSettings = Fast::Oot3d::GraphicsSettingsRuntime::Instance();
    const auto current = graphicsSettings.Snapshot();
    const auto decoded = Fast::Oot3d::DeserializeGraphicsSettings(
        Oot3dNativeGame::MergeScheduledGraphics(
            Fast::Oot3d::SerializeGraphicsSettings(current), step->Graphics),
        current);
    const auto applied = graphicsSettings.Apply(decoded.Value, false);
    std::fprintf(stderr,
                 "TRIAEVUM_GRAPHICS_SETTINGS_SCHEDULE frame=%u accepted=%d "
                 "decode_issues=%zu apply_issues=%zu\n",
                 step->Frame, applied.Accepted() ? 1 : 0,
                 decoded.Issues.size(), applied.Issues.size());
    if (decoded.UnsupportedFutureVersion || !applied.Accepted())
      throw std::runtime_error(
          "graphics settings schedule step at frame " +
          std::to_string(step->Frame) + " was rejected");
  }
  // The renderer frame begin; false when the frame ended early (no image
  // was acquired, or the guest waits for the preparation screen).
  Fast::Renderer3ds::PicaPreparationStatus preparationStatus{};
  const auto beginGuestFrame = [&](bool forceSynchronous) -> bool {
    const auto rendererFrameStart = std::chrono::steady_clock::now();
    CpuProbe::Scope cpuRendererStart(CpuProbe::Phase::RendererStart);
    const auto beginRendererFrame = [&, width, height,
                                     presentationSerial =
                                         presentationFrameCount] {
      api.UpdateFramebufferParameters(0, width, height, 1, false, true, true,
                                      true);
      if (titleRenderBackend != nullptr) {
        // Display settings are window operations: applied only by a frame
        // begun on the window's thread.
        titleRenderBackend->SetPresentationSettingsHeld(
            !renderStream.InSynchronousCommand());
      }
      const auto beginStart = std::chrono::steady_clock::now();
      api.StartFrame();
      inputLatencyTrace.ObserveBegin(presentationSerial, beginStart,
                                     std::chrono::steady_clock::now());
      rendererFrameSkipped = !api.HasActiveFrame();
      if (rendererFrameSkipped) {
        rendererFrameWasInactive.store(true);
        return;
      }
      picaPresentationScheduler.BeginPresentation(presentationSerial);
      api.StartDrawToFramebuffer(0, 1.0F);
      // In-world: the panel (the image StartFrame selects) is cleared transparent, so a world frame's
      // panel is the HUD over alpha 0, the HUD quad's image (W4, minimal). The eyes and the cinema
      // quad are opaque layers: their alpha is ignored.
      api.SetClearColor(0.0F, 0.0F, 0.0F, xrWorld != nullptr ? 0.0F : 1.0F);
      api.ClearFramebuffer(true, true);
      api.SetViewport(0, 0, static_cast<int>(width), static_cast<int>(height));
      api.SetScissor(0, 0, static_cast<int>(width), static_cast<int>(height));
    };
    // A frame begin may recreate targets (and so read back GPU copies into
    // guest memory) after a resize or a graphics settings change, may apply
    // display settings (window operations), and an inactive frame must not
    // advance the guest: those begin synchronously, on this thread.
    // Otherwise the guest goes on while the renderer finishes the previous
    // frame.
    const auto renderSettings = Fast::Oot3d::GraphicsSettingsRuntime::Instance()
                                    .SnapshotWithRevision()
                                    .Revision;
    const bool renderNativePresentation =
        Fast::Oot3d::GraphicsSettingsRuntime::Instance()
            .NativePresentationOverrideActive();
    const bool presentationTransactionActive =
        Fast::Oot3d::GraphicsSettingsRuntime::Instance()
            .PresentationStatus()
            .Phase != Fast::Oot3d::PresentationTransactionPhase::Idle;
    const bool previousRendererFrameInactive =
        rendererFrameWasInactive.exchange(false);
    const bool synchronousFrameBegin =
        forceSynchronous ||
        renderStream.CurrentMode() ==
            Oot3dNativeGame::NativeRenderCommandStream::Mode::Inline ||
        presentationTransactionActive || previousRendererFrameInactive ||
        width != guestFrameBeginWidth ||
        height != guestFrameBeginHeight ||
        renderSettings != guestFrameBeginSettings ||
        renderNativePresentation != guestFrameBeginNativePresentation;
    guestFrameBeginWidth = width;
    guestFrameBeginHeight = height;
    guestFrameBeginSettings = renderSettings;
    guestFrameBeginNativePresentation = renderNativePresentation;
    bool rendererFrameActive = true;
    if (synchronousFrameBegin) {
      renderStream.Run(beginRendererFrame);
      // The renderer is idle after Run: its frame state is readable.
      rendererFrameActive = !rendererFrameSkipped;
    } else {
      renderStream.Submit(beginRendererFrame);
    }
    if (!rendererFrameActive) {
#if defined(TRIAEVUM_QUEST_OPENXR)
      // Immersive mode: the OpenXR session is not focused (system menu, headset off). The guest
      // is held below; on the pause edge the queued audio goes too, so the game falls silent.
      if (TriAevumQuestXr::Requested() && TriAevumQuestXr::NoteGuestPaused(true) &&
          audioPlayer != nullptr && audioPlayer->IsInitialized()) {
        audioPlayer->Flush();
      }
#endif
      // A minimized or changing surface is not a guest/PICA failure. Keep events
      // and UI balanced, but do not consume guest time without an acquired frame.
      if (!presentationScheduler.Restore(presentationStateBeforeFrame, &error)) {
        throw std::runtime_error("cannot restore suspended presentation clock: " + error);
      }
      gui->EndDraw();
      inputLatencyTrace.DiscardPending(presentationFrameCount);
      renderStream.Submit([&] { window.EndFrame(); }, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      renderStream.Submit([&] { realtimePacer.ResetDeadline(); });
      lastPresentationTime = std::chrono::steady_clock::now();
      // In-world: this frame's pace tick was never waited for; the pace restarts from the clock.
      if (xrWorld != nullptr) xrWorld->NoteHeldFrame();
      return false;
    }
#if defined(TRIAEVUM_QUEST_OPENXR)
    if (TriAevumQuestXr::Requested()) TriAevumQuestXr::NoteGuestPaused(false);
#endif
    preparationStatus = programPreparation != nullptr
        ? programPreparation->PreparationStatus()
        : Fast::Renderer3ds::PicaPreparationStatus{};
    if (preparationScreen.HoldsGuest(preparationStatus,
                                     std::chrono::steady_clock::now())) {
      // The guest waits for the initial preparation: its clock does not run
      // and the frame shows the progress instead of a game image.
      if (!presentationScheduler.Restore(presentationStateBeforeFrame, &error)) {
        throw std::runtime_error("cannot restore the presentation clock during preparation: " + error);
      }
      preparationScreen.Draw(preparationStatus);
      gui->EndDraw();
      inputLatencyTrace.DiscardPending(presentationFrameCount);
      renderStream.Submit([&] { window.EndFrame(); }, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      renderStream.Submit([&] { realtimePacer.ResetDeadline(); });
      lastPresentationTime = std::chrono::steady_clock::now();
      lastCompletedFrameTime = lastPresentationTime;
      if (xrWorld != nullptr) xrWorld->NoteHeldFrame();
      return false;
    }
    phaseTiming.RendererFrameStartSeconds += SecondsSince(rendererFrameStart);
    cpuRendererStart.Stop();
    return true;
  };
  if (lateInputSample) {
    // Synchronous, so the image is acquired when the input is sampled; the
    // frame's events are pumped again for it.
    if (!beginGuestFrame(true)) return true;
    window.HandleEvents();
    inputPumpTime = std::chrono::steady_clock::now();
  }
  const auto inputPhaseStart = std::chrono::steady_clock::now();
  // F5 saves the game moment and F8 loads the last state; with Shift, the
  // settings are saved beside it / applied from it.
  const bool quickSaveKeyDown = window.IsKeyDown(Ship::KbScancode::LUS_KB_F5);
  const bool quickLoadKeyDown = window.IsKeyDown(Ship::KbScancode::LUS_KB_F8);
  const bool shiftDown = window.IsKeyDown(Ship::KbScancode::LUS_KB_SHIFT) ||
                         window.IsKeyDown(Ship::KbScancode::LUS_KB_RSHIFT);
  if (quickLoadKeyDown && !quickLoadKeyWasDown) {
    savestateLibrary->Request(
        {Oot3dNativeGame::SavestateRequestKind::Load, shiftDown, {}});
  } else if (quickSaveKeyDown && !quickSaveKeyWasDown) {
    savestateLibrary->Request(
        {Oot3dNativeGame::SavestateRequestKind::Save, shiftDown, {}});
  }
  quickSaveKeyWasDown = quickSaveKeyDown;
  quickLoadKeyWasDown = quickLoadKeyDown;
  bool nativeFrontendTouchEnabled = ResolveNativeA32TouchInputEnabled(
      process.Memory(), uiLifecycleBridge,
      nativeCandidateDispatch.TopScreenUiProfile);
  uint32_t topScreenNativeGameMode = UINT32_MAX;
  const bool topScreenGameplayInputMode =
      process.Memory().Read32(kNativeGameMode, &topScreenNativeGameMode) &&
      Oot3dNativeGame::IsTopScreenGameplayInputMode(topScreenNativeGameMode);
  picaCompositionDomain =
      topScreenGameplayInputMode
          ? Oot3dNativeGame::Oot3dPicaCompositionDomain::Scene
          : Oot3dNativeGame::Oot3dPicaCompositionDomain::Unknown;
  bool topScreenStartRoutingEligible =
      uiLifecycleBridge.NativeGameplayPresentationActive() &&
      topScreenGameplayInputMode &&
      Oot3dNativeGame::IsTopScreenStartRoutingMode(topScreenNativeGameMode);
  Oot3dNativeGame::NativeAimProfileTransform aimProfileTransform;
  if (nativeCandidateDispatch.TopScreenUiProfile) {
    const auto topScreenAim =
        Oot3dNativeGame::ResolveTopScreenCStickAimPolicy(
            activeTopScreenConfig);
    aimProfileTransform.RightStickScale = topScreenAim.SpeedMultiplier;
    aimProfileTransform.RightStickSmoothingCoefficient =
        Oot3dNativeGame::ResolveTopScreenCStickSmoothingCoefficient(
            activeTopScreenConfig.FreeCameraSmoothing);
    aimProfileTransform.RightStickInvertX = topScreenAim.InvertX;
    aimProfileTransform.RightStickInvertY = topScreenAim.InvertY;
  }
  // The in-game menu withholds the game's input like an ImGui window; F1
  // (or the menu bar) owns the input over it and closes it.
  const auto f1Window = gui->GetGuiWindow("OOT3D Graphics");
  const bool imGuiOwnsInput =
      gui->GetMenuOrMenubarVisible() || (f1Window != nullptr && f1Window->IsVisible());
  const bool hostGuiVisible =
      gui->GetMenuOrMenubarVisible() || gui->GetAnyGuiWindowVisible() || inGameMenu.IsOpen();
  auto physicalInputFrame =
      PollNativeA32Input(window, presentationPicture, nativeFrontendTouchEnabled,
                         hostGuiVisible,
                         nativeCandidateDispatch.TopScreenUiProfile,
                         activeTopScreenConfig.FreeCameraEnabled,
                         *controlConfigRuntime, aimProfileTransform,
                         presentationElapsedSeconds,
                         guestRefreshesDue != 0U,
                         nativeControlPollingState);
#if defined(TRIAEVUM_DEVELOPER_TOOLS)
  // A dev lab "menu" command replaces the host controls with its presses.
  inGameMenu.Update(devLabMenuDriving ? devLabMenuInput : nativeControlPollingState.Menu, imGuiOwnsInput,
                    presentationElapsedSeconds);
  // The lab's wheel is a delta for one menu update; the lab command advances
  // per guest refresh, this update per presented frame.
  devLabMenuInput.PointerWheel = 0.0F;
#else
  inGameMenu.Update(nativeControlPollingState.Menu, imGuiOwnsInput, presentationElapsedSeconds);
#endif
  if (physicalInputFrame.Exit || devLabQuit) {
    window.Close();
    return false;
  }
  inputLatencyTrace.ObserveInput(presentationFrameCount, inputPumpTime,
                                 std::chrono::steady_clock::now(),
                                 visualInterpolationAlpha);
  if (!inputTimeline.Enabled()) {
    const uint32_t start = Oot3dNativeGame::NativeA32HidButtonMask(
        Oot3dNativeGame::NativeA32HidButton::Start);
    const bool physicalStartHeld =
        (physicalInputFrame.Hid.Buttons & start) != 0U;
    Oot3dNativeGame::ObserveNativeA32PolledButton(
        physicalStartHeld, nativeCandidateDispatch.StartButtonLatch);
    Oot3dNativeGame::ReleaseTopScreenStartRoutingLatch(
        physicalStartHeld, nativeCandidateDispatch.TopScreenStartRouting);
  }
  Oot3dNativeGame::NativeA32InputFrame inputFrame;
  phaseTiming.InputPollSeconds += SecondsSince(inputPhaseStart);
  if (!lateInputSample && !beginGuestFrame(false)) return true;
  phaseTiming.FrameStartSeconds += SecondsSince(phaseStart);
  const uint32_t guestRefreshIterations =
      std::max<uint32_t>(1U, guestRefreshesDue);
  for (uint32_t guestRefreshIndex = 0;
       guestRefreshIndex < guestRefreshIterations; ++guestRefreshIndex) {
    const bool advanceGuest = guestRefreshIndex < guestRefreshesDue;
    const bool presentHostFrame =
        guestRefreshIndex + 1U == guestRefreshIterations;
    if (advanceGuest) {
      if (!SampleGuestRefreshInput(
              guestRefreshIndex,
              nativeFrontendTouchEnabled,
              topScreenNativeGameMode,
              topScreenStartRoutingEligible,
              physicalInputFrame,
              inputFrame)) {
        break;
      }
      if (!UpdateGuestServicesForRefresh(topScreenStartRoutingEligible, inputFrame)) {
        break;
      }
#if defined(TRIAEVUM_QUEST_OPENXR)
      if (TriAevumQuestXr::Requested()) TriAevumQuestXr::NoteGuestRefresh();  // must stay 0 while paused
      const auto guestRefreshStart = std::chrono::steady_clock::now();
#endif
      RunGuestRefresh();
#if defined(TRIAEVUM_QUEST_OPENXR)
      if (xrWorld != nullptr)
        TriAevumQuestXr::PerfNote(TriAevumQuestXr::PerfPhase::GuestUpdate,
                                  std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - guestRefreshStart)
                                      .count());
#endif
    }
    if (presentHostFrame) {
      PresentVisualFrame(presentationElapsedSeconds, visualInterpolationAlpha);
    }
    frameCount += advanceGuest ? 1U : 0U;
  }

  if (launch.ExtendedDiagnostics) {
    inputConsumerDiagnostics.Observe(process.Memory(), frameCount);
  }

  for (std::size_t subsystemIndex = 0;
       subsystemIndex < oot3d::ui::kUiSubsystemCount; ++subsystemIndex) {
    const auto subsystem =
        static_cast<oot3d::ui::UiSubsystem>(subsystemIndex);
    const bool topScreenProfile =
        launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen;
    if (topScreenProfile &&
        subsystem != oot3d::ui::UiSubsystem::GameplayHud &&
        subsystem != oot3d::ui::UiSubsystem::TouchControls) {
      continue;
    }
    const bool topScreenPresentation = topScreenProfile;
    if (!topScreenPresentation && !uiLifecycleBridge.Runtime()
                                       .PlanFrame(subsystem)
                                       .run_host_presentation) {
      continue;
    }
    auto primitives =
        topScreenPresentation
            ? uiLifecycleBridge.BuildTopScreenPresentation(subsystem)
            : uiLifecycleBridge.BuildShadowPresentation(subsystem);
    if (topScreenPresentation &&
        subsystem == oot3d::ui::UiSubsystem::GameplayHud) {
      Oot3dNativeGame::AppendTopScreenFreeCameraOptionPresentation(
          nativeCandidateDispatch.TopScreenCamera.Camera, primitives);
      AppendTopScreenDpadPickerHoldRings(uiLifecycleBridge, primitives);
    }
    if (topScreenPresentation && subsystem == oot3d::ui::UiSubsystem::TouchControls) {
      ocarinaText.Append(primitives);
    }
    if (topScreenPresentation) {
      lastTopScreenPrimitivesBySubsystem[subsystemIndex] = primitives;
    }
    const auto canvasMode =
        topScreenPresentation
            ? oot3d::ui::N64UiCanvasMode::NativeTopScreen400x240
            : oot3d::ui::N64UiCanvasMode::Widescreen16x9;
    // Texture payloads are read from guest memory here; the renderer
    // only uploads them.
    auto preparedTextures = n64UiRenderer.PrepareTextures(primitives);
    renderStream.Submit([&, primitives = std::move(primitives), uiRegion,
                         canvasMode,
                         prepared = std::move(preparedTextures)]() mutable {
      if (rendererFrameSkipped) {
        n64UiRenderer.Stage(std::move(prepared));
        return;
      }
      const auto uiStart = std::chrono::steady_clock::now();
      if (!n64UiRenderer.Render(primitives, uiRegion, canvasMode,
                                std::move(prepared), &renderError)) {
        throw std::runtime_error("integrated N64 UI presentation failed: " +
                                 renderError);
      }
      if (xrWorld != nullptr)
        xrPerfUiMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uiStart).count();
    });
  }
  // The in-game menu draws last in the UI domain, over the HUD and the
  // native text, on the widescreen canvas so its dim covers the window.
  if (inGameMenu.IsOpen()) {
    auto menuPrimitives = inGameMenu.BuildPresentation(1280.0F / 3.0F, 240.0F);
    auto menuTextures = n64UiRenderer.PrepareTextures(menuPrimitives);
    renderStream.Submit([&, primitives = std::move(menuPrimitives), uiRegion,
                         prepared = std::move(menuTextures)]() mutable {
      if (rendererFrameSkipped) {
        n64UiRenderer.Stage(std::move(prepared));
        return;
      }
      const auto uiStart = std::chrono::steady_clock::now();
      if (!n64UiRenderer.Render(primitives, uiRegion, oot3d::ui::N64UiCanvasMode::Widescreen16x9,
                                std::move(prepared), &renderError)) {
        throw std::runtime_error("in-game menu presentation failed: " + renderError);
      }
      if (xrWorld != nullptr)
        xrPerfUiMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - uiStart).count();
    });
  }

  phaseStart = std::chrono::steady_clock::now();
  CpuProbe::Scope cpuPresent(CpuProbe::Phase::Present);
  Fast::Renderer3ds::SlowStepTrace presentSteps("present");
  // The first frames render under the preparation screen.
  preparationScreen.Draw(preparationStatus);
  gui->EndDraw();
  presentSteps.Mark("gui_end_draw");
  renderStream.Submit([&, width, height,
                       presentationSerial = presentationFrameCount,
                       forcedScreenshot = std::exchange(pendingForcedScreenshot, {}),
                       forcedView = std::exchange(pendingForcedScreenshotView, {}),
                       xrPacing = xrWorld != nullptr && xrWorld->FrameState().TickPacing,
                       xrPaceTick = xrWorld != nullptr ? xrWorld->FrameState().PaceTick : 0U] {
    // In-world: whether this host frame executed both eyes (taken here, once per frame).
    const bool xrEyes = std::exchange(xrEyesDrawn, false);
    (void)xrEyes;
    if (!forcedScreenshot.empty()) screenshotState.ForcedPath = forcedScreenshot;
    if (rendererFrameSkipped) {
      capturedTemporalSample.reset();
      window.EndFrame();
      realtimePacer.ResetDeadline();
      inputLatencyTrace.DiscardPending(presentationSerial);
      return;
    }
    uint32_t captureWidth = width, captureHeight = height;
    std::filesystem::path xrAlphaPath;  // in-world HUD capture: its alpha beside the colour BMP
#if defined(TRIAEVUM_QUEST_OPENXR)
    if (xrWorld != nullptr &&
        (!screenshotState.ForcedPath.empty() ||
         ShouldWriteFramebufferScreenshot(hostArgs, static_cast<uint32_t>(presentationSerial),
                                          screenshotState))) {
      // In-world captures (G14): the dev lab's "view" (left|right|panel); --screenshot takes the
      // left eye of a frame that drew a pair. An eye of a frame that drew none (Cinema/Black, no
      // prediction, no presented sample) falls back to the panel: its eye images are older frames'.
      const std::string requested = forcedView.empty() ? std::string(xrEyes ? "left" : "panel") : forcedView;
      uint32_t view = requested == "left"    ? Fast::kXrViewEyeLeft
                      : requested == "right" ? Fast::kXrViewEyeRight
                                             : Fast::kXrViewPanel;
      if (view != Fast::kXrViewPanel && !xrEyes) view = Fast::kXrViewPanel;
      const bool selected = api.SelectXrView(view);
      if (view != Fast::kXrViewPanel) {
        const auto eye = TriAevumQuestXr::EyeExtent();
        captureWidth = eye.width;
        captureHeight = eye.height;
      }
      // The HUD panel of a world frame is premultiplied colour over alpha 0: the colour BMP shows it
      // composited over black, the alpha goes to <path>.alpha.bmp (grey = alpha) for a halo check.
      if (view == Fast::kXrViewPanel && xrRenderState.World && !screenshotState.ForcedPath.empty()) {
        xrAlphaPath = screenshotState.ForcedPath;
        xrAlphaPath += ".alpha.bmp";
      }
      Oot3dNativeGame::XrWorldLog(
          "TRIAEVUM_XR_CAPTURE view=%s requested=%s selected=%d world_frame=%d eyes_drawn=%d w=%u h=%u path=%s",
          view == Fast::kXrViewEyeLeft ? "left" : view == Fast::kXrViewEyeRight ? "right" : "panel",
          requested.c_str(), selected ? 1 : 0, xrRenderState.World ? 1 : 0, xrEyes ? 1 : 0, captureWidth,
          captureHeight,
          screenshotState.ForcedPath.empty() ? "(--screenshot)" : screenshotState.ForcedPath.string().c_str());
    }
#endif
    MaybeWriteFramebufferScreenshot(
        hostArgs, api, captureWidth, captureHeight,
        static_cast<uint32_t>(presentationSerial), screenshotState,
        capturedTemporalSample ? &*capturedTemporalSample : nullptr);
#if defined(TRIAEVUM_QUEST_OPENXR)
    if (!xrAlphaPath.empty()) {
      std::vector<uint8_t> alpha(static_cast<size_t>(captureWidth) * captureHeight);
      if (api.ReadFramebufferAlpha8ToCPU(captureWidth, captureHeight, alpha.data())) {
        std::vector<uint8_t> grey(alpha.size() * 3U);
        size_t covered = 0, opaque = 0;
        uint64_t sum = 0;
        for (size_t i = 0; i < alpha.size(); ++i) {
          grey[i * 3U] = grey[i * 3U + 1U] = grey[i * 3U + 2U] = alpha[i];
          covered += alpha[i] != 0U ? 1U : 0U;
          opaque += alpha[i] == 255U ? 1U : 0U;
          sum += alpha[i];
        }
        WriteBmpFromRgb8Framebuffer(xrAlphaPath, captureWidth, captureHeight, grey);
        Oot3dNativeGame::XrWorldLog(
            "TRIAEVUM_XR_CAPTURE alpha path=%s pixels=%zu covered=%zu opaque=%zu mean_alpha=%.2f",
            xrAlphaPath.string().c_str(), alpha.size(), covered, opaque,
            alpha.empty() ? 0.0 : static_cast<double>(sum) / static_cast<double>(alpha.size()));
      } else {
        Oot3dNativeGame::XrWorldLog("TRIAEVUM_XR_CAPTURE alpha path=%s unavailable", xrAlphaPath.string().c_str());
      }
    }
#else
    (void)xrAlphaPath;
#endif
    capturedTemporalSample.reset();
    const auto waitStart = std::chrono::steady_clock::now();
    CpuProbe::Scope cpuPacing(CpuProbe::Phase::Pacing);
#if defined(TRIAEVUM_QUEST_OPENXR)
    if (xrWorld != nullptr && TriAevumQuestXr::PerfEnabled()) {
      // TRIAEVUM_XR_PERF (render worker): this frame's sums, then the frame, its CPU and idle time.
      namespace Xr = TriAevumQuestXr;
      Xr::PerfNote(Xr::PerfPhase::WorkerNs1, std::exchange(xrPerfNs1Ms, 0.0));
      Xr::PerfNote(Xr::PerfPhase::WorkerUi, std::exchange(xrPerfUiMs, 0.0));
      const double cpu = Xr::ThreadCpuMs();
      if (xrPerfWorkerCpu >= 0.0) {
        Xr::PerfNote(Xr::PerfPhase::WorkerFrame,
                     std::chrono::duration<double, std::milli>(waitStart - xrPerfWorkerFrame).count());
        Xr::PerfNote(Xr::PerfPhase::WorkerCpu, cpu - xrPerfWorkerCpu);
        Xr::PerfNote(Xr::PerfPhase::WorkerIdle,
                     static_cast<double>(renderStream.TakeWorkerIdleNs()) * 1e-6);
      }
      xrPerfWorkerFrame = waitStart;
      xrPerfWorkerCpu = cpu;
    }
    if (xrPacing) {
      // In-world: the display clock paces the eyes (§3.4 item 3): this frame's pair is submitted
      // at its tick p_N; the realtime pacer is the fallback when the tick does not come.
      const bool reached = TriAevumQuestXr::WaitDisplayTick(xrPaceTick, 50U);
      xrWorld->NoteTickWait(reached);
      if (!reached) realtimePacer.WaitForNextRefresh();
      TriAevumQuestXr::PerfNote(
          TriAevumQuestXr::PerfPhase::WorkerTickWait,
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count());
    } else
#else
    (void)xrPacing;
    (void)xrPaceTick;
    (void)forcedView;
#endif
    realtimePacer.WaitForNextRefresh();
    cpuPacing.Stop();
    const auto waitEnd = std::chrono::steady_clock::now();
    rendererTiming->PacingWaitSeconds +=
        std::chrono::duration<double>(waitEnd - waitStart).count();
    window.EndFrame();
    inputLatencyTrace.ObserveFrameEnd(presentationSerial, waitStart, waitEnd,
                                      std::chrono::steady_clock::now());
  }, true);
  presentSteps.Mark("render_end_frame");
  picaSemanticTrace.RecordFrameBoundary(
      presentationFrameCount, frameCount, guestRefreshesDue != 0U);
  phaseTiming.PresentSeconds += SecondsSince(phaseStart);
  cpuPresent.Stop();
  ++presentationFrameCount;
  ++runFrameCount;
#if defined(__SWITCH__)
  gSwitchRuntimeHostFrames = runFrameCount;
  gSwitchRuntimeGuestFrames = frameCount;
  // Write once at the first frame and at most once more when a persistent
  // geometry is first observed. Periodic receipt writes would put
  // synchronous SD filesystem work directly in the presentation loop.
  if (runFrameCount == 1U || !gSwitchPicaGeometryCacheObserved) {
    const auto wholeAotStats =
        Oot3dNativeGame::GetOot3dCompiledFunctionStats();
    gSwitchWholeAotBlockLimitExits =
        wholeAotStats.WholeAotBlockLimitExits;
    const auto picaBackend = api.GetNativePicaBackendStats();
    gSwitchPicaGeometryCacheEnabled =
        picaBackend.GeometryCacheEnabled;
    gSwitchPicaGeometryPersistentDraws =
        picaBackend.GeometryPersistentDraws;
    gSwitchPicaGeometryPersistentUploads =
        picaBackend.GeometryPersistentUploads;
    gSwitchPicaGeometryRegistryEntries =
        picaBackend.GeometryRegistryEntries;
    gSwitchPicaGeometryCacheObserved =
        gSwitchPicaGeometryPersistentDraws != 0U &&
        gSwitchPicaGeometryPersistentUploads != 0U &&
        gSwitchPicaGeometryRegistryEntries != 0U;
    if (runFrameCount == 1U || gSwitchPicaGeometryCacheObserved) {
      WriteSwitchBootStage("runtime_running");
    }
  }
#endif
  const auto completedFrameTime = std::chrono::steady_clock::now();
  if (!preparationScreen.Done()) {
    preparationScreen.ObserveFrame(
        std::chrono::duration<double, std::milli>(completedFrameTime - lastCompletedFrameTime).count(),
        completedFrameTime);
    if (preparationScreen.Done()) {
      std::fprintf(stderr,
                   "TRIAEVUM_INITIAL_PREPARATION parts=%zu startup_seconds=%.3f wait_seconds=%.3f "
                   "first_frames=%u first_frames_seconds=%.3f\n",
                   preparationScreen.Parts(), preparationScreen.StartupSeconds(),
                   preparationScreen.WaitSeconds(),
                   preparationScreen.FirstFrames(), preparationScreen.FirstFramesSeconds());
    }
  }
  lastCompletedFrameTime = completedFrameTime;
  if (!benchmarkMeasurementStarted && preparationScreen.Done() &&
      runFrameCount >= hostArgs.BenchmarkWarmupFrames) {
    benchmark.MeasurementStart = completedFrameTime;
    benchmark.MeasurementEnd = completedFrameTime;
    benchmarkMeasurementStarted = true;
    if (cpuProbeRequested) CpuProbe::Thread.StartAt(CpuProbe::Read());
  } else if (benchmarkMeasurementStarted &&
             runFrameCount > hostArgs.BenchmarkWarmupFrames) {
    const auto currentPhases = phaseTiming.Values();
    std::array<double, 15> delta{};
    for (size_t i=0; i<delta.size(); ++i) {
      delta[i]=currentPhases[i]-phaseBeforeFrame[i];
      benchmark.PhaseSeconds[i]+=delta[i];
    }
    const double nestedGuest=phaseTiming.GuestInsidePicaSubmitSeconds-nestedGuestBeforeFrame;
    benchmark.NestedGuestSeconds+=nestedGuest;
    benchmark.GuestClockSeconds+=phaseTiming.GuestClockSeconds-guestClockBeforeFrame;
    const Oot3dNativeGame::PreBackendBudget budget{
      delta[0],delta[7],nestedGuest,delta[8],delta[2],delta[3],delta[5],delta[6]};
    if(budget.Valid()) benchmark.PreBackendTimes.RecordMilliseconds(budget.Total()*1000.0);
    else ++benchmark.InvalidPreBackendIntervals;
    // Present includes pacing. Visual presentation includes backend/replay.
    // Remove each inclusive scope once; keep unclassified host/UI work visible.
    const double envelope=std::chrono::duration<double>(completedFrameTime-benchmark.MeasurementEnd).count()
        -delta[4]-delta[11]-delta[13];
    benchmark.PreBackendEnvelopeTimes.RecordMilliseconds(envelope*1000.0);
    benchmark.UnattributedSeconds+=envelope-budget.Total();
    if (benchmarkFrameTimes) {
      auto phases = phaseTiming.Values();
      for (size_t i = 0; i < phases.size(); ++i)
        phases[i] = (phases[i] - phaseBeforeFrame[i]) * 1000.0;
      benchmark.SlowFrames.Record(runFrameCount, std::chrono::duration<double, std::milli>(
          completedFrameTime - benchmark.MeasurementEnd).count(), phases);
      benchmarkFrameTimes->RecordMilliseconds(
          std::chrono::duration<double, std::milli>(
              completedFrameTime - benchmark.MeasurementEnd).count());
    }
    benchmark.MeasurementEnd = completedFrameTime;
    ++benchmark.MeasuredFrames;
    if (cpuProbeRequested) benchmarkCpuPhases = CpuProbe::Thread.Snapshot();
  }
  for (const auto &action : launch.SavestateActions) {
    if (runFrameCount == static_cast<uint64_t>(action.RunFrame) + 1U) {
      savestateLibrary->Request(
          {action.Load ? Oot3dNativeGame::SavestateRequestKind::Load
                       : Oot3dNativeGame::SavestateRequestKind::Save,
           action.WithSettings, action.Path});
    }
  }
  ServiceSavestateRequests();
  {
    if (Oot3dNativeGame::ShouldCaptureNativeA32AutomatedState(
            launch.SaveStateFrameAvailable,
            automatedStateSaved || automatedStateSaveFailed, runFrameCount,
            launch.SaveStateFrame)) {
      const auto outcome =
          SaveState(launch.SaveStatePath, launch.SaveStateWithSettings);
      if (outcome == SaveStateOutcome::Saved) {
        automatedStateSaved = true;
        automatedStateSavedAtRunFrame = runFrameCount - 1U;
        automatedStateSavedAtGuestFrame = frameCount;
      } else if (outcome == SaveStateOutcome::Failed) {
        automatedStateSaveFailed = true;
      }
    }
  }
  ApplyWindowDemoFrameLimit(hostArgs, window, runFrameCount);
  return true;
}

// The guest input of one refresh: touch and TopScreen routing state, the input timeline or the polled
// frame. False when the input asked to close the window.
bool NativeA32WindowSession::SampleGuestRefreshInput(
    const uint32_t &guestRefreshIndex,
    bool &nativeFrontendTouchEnabled,
    uint32_t &topScreenNativeGameMode,
    bool &topScreenStartRoutingEligible,
    Oot3dNativeGame::NativeA32InputFrame &physicalInputFrame,
    Oot3dNativeGame::NativeA32InputFrame &inputFrame) {
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &window = *windowPointer;
  if (guestRefreshIndex != 0U) {
    nativeFrontendTouchEnabled = ResolveNativeA32TouchInputEnabled(
        process.Memory(), uiLifecycleBridge,
        nativeCandidateDispatch.TopScreenUiProfile);
    topScreenNativeGameMode = UINT32_MAX;
    const bool refreshedTopScreenGameplayInputMode =
        process.Memory().Read32(kNativeGameMode,
                                &topScreenNativeGameMode) &&
        Oot3dNativeGame::IsTopScreenGameplayInputMode(
            topScreenNativeGameMode);
    topScreenStartRoutingEligible =
        uiLifecycleBridge.NativeGameplayPresentationActive() &&
        refreshedTopScreenGameplayInputMode &&
        Oot3dNativeGame::IsTopScreenStartRoutingMode(topScreenNativeGameMode);
    picaCompositionDomain =
        refreshedTopScreenGameplayInputMode
            ? Oot3dNativeGame::Oot3dPicaCompositionDomain::Scene
            : Oot3dNativeGame::Oot3dPicaCompositionDomain::Unknown;
  }
  // Re-arm after any state restore: RestoreState replaces the memory
  // image, and the watch is diagnostic state that must not disappear with
  // it. Cheap when the trapped page set is unchanged.
  if (!rawCopyWatchRanges.empty() &&
      !process.Memory().RangeWatchActive()) {
    process.Memory().SetRangeWatch(rawCopyWatchRanges);
  }
  inputFrame = ResolveNativeA32GuestInput(
      physicalInputFrame, inputTimeline, frameCount, runFrameCount,
      nativeFrontendTouchEnabled, topScreenStartRoutingEligible,
      nativeCandidateDispatch.TopScreenUiProfile,
      nativeCandidateDispatch.StartButtonLatch,
      nativeCandidateDispatch.TopScreenStartRouting);
  if (nativeCandidateDispatch.TopScreenUiProfile &&
      Oot3dNativeGame::ApplyTopScreenVersionChoiceReveal(
          inputFrame, uiLifecycleBridge.NativeVersionChoiceAwaitingReveal(),
          uiLifecycleBridge.NativeVersionChoicePrompting(),
          nativeCandidateDispatch.TopScreenVersionChoiceReveal)) {
    uiLifecycleBridge.RevealNativeVersionChoice();
  }
  if (xrWorld != nullptr && !inputTimeline.Enabled()) {
    // In-world (inworld-spec §7): the Circle Pad of this refresh pushes Link relative to where Jamal
    // looks, from the camera yaw Player reads (play+0x364+0x17E) to psi_ref, in World and Held alike.
    const auto &probe = playerTimingProbe.Stats();
    const uint32_t playState = probe.LastPlayStateAddress != 0U
                                   ? probe.LastPlayStateAddress
                                   : sceneViewProbe.Stats().LastPlayStateAddress;
    Oot3dNativeGame::TopScreenOcarinaState ocarina;
    const bool ocarinaActive = nativeCandidateDispatch.TopScreenUiProfile &&
                               Oot3dNativeGame::ReadTopScreenOcarinaState(process.Memory(), &ocarina) &&
                               ocarina.Active;
    xrWorld->RotateStick(
        Oot3dNativeGame::ReadXrGuestSnapshot(process.Memory(), probe.LastPlayerAddress, playState),
        ocarinaActive, inputFrame.Hid.CirclePadX, inputFrame.Hid.CirclePadY);
  }
  inputDiagnostics.Observe(inputFrame);
  if (inputFrame.Exit) {
    window.Close();
    return false;
  }
  return true;
}

// Before the guest runs a refresh: TopScreen input routing, the structural scenario, the guest clock to the
// next vblank, HID and DSP audio. False when the scenario closed the window.
bool NativeA32WindowSession::UpdateGuestServicesForRefresh(
    bool &topScreenStartRoutingEligible,
    Oot3dNativeGame::NativeA32InputFrame &inputFrame) {
  auto &hostServices = *hostServicesStorage;
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &window = *windowPointer;
  auto &dspHle = *dspHleStorage;
  UpdateDevFreeCamera(inputFrame);
  const uint32_t topScreenButtons = inputFrame.Hid.Buttons;
  const uint32_t topScreenPressed =
      topScreenButtons &
      ~nativeCandidateDispatch.PreviousTopScreenButtons;
  const auto topScreenButton =
      [](Oot3dNativeGame::NativeA32HidButton value) {
        return Oot3dNativeGame::NativeA32HidButtonMask(value);
      };
  Oot3dNativeGame::TopScreenDpadActionState topScreenDpadActions;
  Oot3dNativeGame::TopScreenOcarinaState ocarinaOwner;
  const bool ocarinaOwnsDpad = nativeCandidateDispatch.TopScreenUiProfile &&
      Oot3dNativeGame::ReadTopScreenOcarinaState(process.Memory(), &ocarinaOwner) &&
      ocarinaOwner.Active;
  // Gameplay D-pad mappings apply only while the native controller is in
  // gameplay (2); in the pause pages the D-pad navigates the native cursor.
  std::uint32_t topScreenController = 0U;
  const bool topScreenGameplayDpad =
      process.Memory().Read32(kTopScreenPauseState, &topScreenController) &&
      topScreenController == 2U;
  if (nativeCandidateDispatch.TopScreenUiProfile &&
      topScreenStartRoutingEligible && topScreenGameplayDpad && !ocarinaOwnsDpad) {
    bool childLink = false;
    std::string childLinkError;
    if (!Oot3dNativeGame::ReadTopScreenChildLink(
            process.Memory(), &childLink, &childLinkError)) {
      throw std::runtime_error("TopScreen Link age decode failed: " +
                               childLinkError);
    }
    Oot3dNativeGame::TopScreenDpadPhysicalState dpadPhysical;
    dpadPhysical.ChildLink = childLink;
    constexpr std::array<Oot3dNativeGame::NativeA32HidButton, 4>
        kDpadButtons{Oot3dNativeGame::NativeA32HidButton::DpadUp,
                     Oot3dNativeGame::NativeA32HidButton::DpadDown,
                     Oot3dNativeGame::NativeA32HidButton::DpadLeft,
                     Oot3dNativeGame::NativeA32HidButton::DpadRight};
    for (std::size_t direction = 0U;
         direction < kDpadButtons.size(); ++direction) {
      const auto mask = topScreenButton(kDpadButtons[direction]);
      dpadPhysical.Pressed[direction] =
          (topScreenPressed & mask) != 0U;
      dpadPhysical.Held[direction] =
          (topScreenButtons & mask) != 0U;
    }
    topScreenDpadActions = Oot3dNativeGame::ResolveTopScreenDpadActions(
        activeTopScreenConfig, dpadPhysical);
  }
  const bool zrHeld = ThreeDsRecomp::Input::IsButtonHeld(
      inputFrame, ThreeDsRecomp::Input::Button::Zr);
  const bool zlHeld = ThreeDsRecomp::Input::IsButtonHeld(
      inputFrame, ThreeDsRecomp::Input::Button::Zl);
  Oot3dNativeGame::TopScreenExtendedInputFrame sampledTopScreenInput = {
      .ZrHeld =
          zrHeld ||
          topScreenDpadActions.IsHeld(
              Oot3dNativeGame::TopScreenDpadAction::ItemZr),
      .ZlHeld =
          zlHeld ||
          topScreenDpadActions.IsHeld(
              Oot3dNativeGame::TopScreenDpadAction::ItemZl),
      .DpadLeftHeld =
          (inputFrame.Hid.Buttons &
           Oot3dNativeGame::NativeA32HidButtonMask(
               Oot3dNativeGame::NativeA32HidButton::DpadLeft)) != 0U,
      .DpadRightHeld =
          (inputFrame.Hid.Buttons &
           Oot3dNativeGame::NativeA32HidButtonMask(
               Oot3dNativeGame::NativeA32HidButton::DpadRight)) != 0U};
  sampledTopScreenInput.DpadUpHeld =
      (inputFrame.Hid.Buttons & topScreenButton(
          Oot3dNativeGame::NativeA32HidButton::DpadUp)) != 0U;
  if (ocarinaOwnsDpad) {
    inputFrame.Hid.Buttons &= ~(topScreenButton(Oot3dNativeGame::NativeA32HidButton::DpadLeft) |
                               (ocarinaOwner.Page == 12 ? topScreenButton(
                                   Oot3dNativeGame::NativeA32HidButton::DpadUp) : 0U) |
                               topScreenButton(Oot3dNativeGame::NativeA32HidButton::DpadRight));
  }
  sampledTopScreenInput.XHeld =
      (topScreenButtons &
       topScreenButton(Oot3dNativeGame::NativeA32HidButton::X)) != 0U;
  sampledTopScreenInput.YHeld =
      (topScreenButtons &
       topScreenButton(Oot3dNativeGame::NativeA32HidButton::Y)) != 0U;
  sampledTopScreenInput.GameplayDpadActionsOwned = true;
  sampledTopScreenInput.RestorationLayout =
      activeTopScreenConfig.HudLayout ==
      Oot3dNativeGame::TopScreenHudLayout::Restoration;
  nativeCandidateDispatch.TopScreenInputClock.ObserveGuest(sampledTopScreenInput);
  if (nativeCandidateDispatch.TopScreenUiProfile &&
      activeTopScreenConfig.FreeCameraEnabled &&
      nativeControlPollingState.GameplayMouseOwned) {
    nativeCandidateDispatch.TopScreenCameraInput.Observe(
        inputFrame.CStick);
  } else if (inputFrame.CStick.Kind ==
             Oot3dNativeGame::NativeFreeCameraInputKind::Absolute) {
    nativeCandidateDispatch.TopScreenCameraInput.Observe(
        inputFrame.CStick);
  } else {
    nativeCandidateDispatch.TopScreenCameraInput.Reset();
  }
  if (nativeCandidateDispatch.TopScreenUiProfile) {
    const uint32_t buttons = inputFrame.Hid.Buttons;
    const uint32_t pressed =
        buttons & ~nativeCandidateDispatch.PreviousTopScreenButtons;
    const auto button = [](Oot3dNativeGame::NativeA32HidButton value) {
      return Oot3dNativeGame::NativeA32HidButtonMask(value);
    };
    const bool gameplayViewPressed = topScreenDpadActions.WasPressed(
        Oot3dNativeGame::TopScreenDpadAction::View);
    const bool gameplayOcarinaPressed =
        topScreenDpadActions.WasPressed(
            Oot3dNativeGame::TopScreenDpadAction::Ocarina);
    const bool leftShoulderHeld =
        (buttons &
         button(Oot3dNativeGame::NativeA32HidButton::L)) != 0U;
    const bool rightShoulderHeld =
        (buttons &
         button(Oot3dNativeGame::NativeA32HidButton::R)) != 0U;
    bool selectPressed =
        (pressed &
         button(Oot3dNativeGame::NativeA32HidButton::Select)) != 0U;
    // R6 (2.1.1 0x005D5730..5BC0): on an open Items/Gear page SELECT toggles
    // the item description; it reaches the gameplay routes (minimap, Save)
    // only while the pause controller is in its gameplay state.
    if (Oot3dNativeGame::TopScreenItemDescriptionInputs description;
        Oot3dNativeGame::ReadTopScreenItemDescriptionInputs(process.Memory(), &description)) {
      auto &descriptionRuntime = *nativeCandidateDispatch.ItemDescription;
      if (Oot3dNativeGame::UpdateTopScreenItemDescription(
              description, selectPressed, descriptionRuntime.VisibleState())) {
        descriptionRuntime.CountToggle();
        std::string soundError;
        if (!Oot3dNativeGame::PlayTopScreenUiSound(process, 0x01000498U,
                                                   kTopScreenDirectCallReturn, &soundError))
          throw std::runtime_error("TopScreen description sound failed: " + soundError);
      }
      if (selectPressed && Oot3dNativeGame::TopScreenItemDescriptionOwnsSelect(description)) {
        selectPressed = false;
        inputFrame.Hid.Buttons &= ~button(Oot3dNativeGame::NativeA32HidButton::Select);
      }
    }
    if (topScreenDpadActions.WasPressed(
            Oot3dNativeGame::TopScreenDpadAction::MinimapToggle) ||
        (selectPressed &&
         activeTopScreenConfig.SelectAction ==
             Oot3dNativeGame::TopScreenSelectAction::MinimapToggle)) {
      SetTopScreenSessionMinimapVisible(
          !activeTopScreenConfig.MinimapVisible);
    }
    if (selectPressed &&
        activeTopScreenConfig.SelectAction ==
            Oot3dNativeGame::TopScreenSelectAction::MinimapToggle) {
      inputFrame.Hid.Buttons &=
          ~button(Oot3dNativeGame::NativeA32HidButton::Select);
    }
    // 2.1.1 0x005D5BC0..0x005D5C70 (issue 80): the source port disables the
    // native Save route at 0x0042EC2C, so SELECT opens Save/Quit through the
    // pause transaction, delivered at the native UI input boundary.
    if (selectPressed &&
        activeTopScreenConfig.SelectAction ==
            Oot3dNativeGame::TopScreenSelectAction::SaveScreen) {
      nativeCandidateDispatch.PauseTransaction->RequestSelectSave();
      nativeCandidateDispatch.ItemDescription->Hide();
      inputFrame.Hid.Buttons &=
          ~button(Oot3dNativeGame::NativeA32HidButton::Select);
    }

    if (gameplayViewPressed || gameplayOcarinaPressed) {
      ++nativeCandidateDispatch.TopScreenGameplayDpadAttempts;
      Oot3dNativeGame::TopScreenGameplayDpadResult dpadResult;
      std::string dpadError;
      if (!Oot3dNativeGame::PrepareTopScreenGameplayDpadGuest(
              process.Memory(),
              {.ViewPressed = gameplayViewPressed,
               .OcarinaPressed = gameplayOcarinaPressed,
               .LeftShoulderHeld = leftShoulderHeld,
               .RightShoulderHeld = rightShoulderHeld},
              &dpadResult, &dpadError)) {
        throw std::runtime_error(
            "TopScreen gameplay D-pad prepare failed: " + dpadError);
      }
      nativeCandidateDispatch.TopScreenNaviViewActivations +=
          dpadResult.NaviViewActivated ? 1U : 0U;
      if (dpadResult.OcarinaQueryRequired) {
        ++nativeCandidateDispatch.TopScreenOcarinaQueries;
        uint32_t nativeEligibility = 0U;
        constexpr std::array<std::uint32_t, 0> kNoArguments{};
        if (!process.InvokeFunctionWithResult(
                kTopScreenOcarinaEligibility, kNoArguments,
                kTopScreenDirectCallReturn, &nativeEligibility,
                &dpadError) ||
            !Oot3dNativeGame::CommitTopScreenGameplayOcarinaGuest(
                process.Memory(), nativeEligibility, &dpadResult,
                &dpadError)) {
          throw std::runtime_error(
              "TopScreen ocarina D-pad route failed: " + dpadError);
        }
        nativeCandidateDispatch.TopScreenOcarinaActivations +=
            dpadResult.OcarinaActivated ? 1U : 0U;
      }
    }
    std::string gameplayActionError;
    if (const auto sound = uiLifecycleBridge.TakeOcarinaGuideSound(); sound != 0U) {
      if (!Oot3dNativeGame::PlayTopScreenUiSound(
              process, sound, kTopScreenDirectCallReturn, &gameplayActionError)) {
        throw std::runtime_error("TopScreen ocarina guide sound failed: " + gameplayActionError);
      }
    }
    if (!Oot3dNativeGame::ConsumeTopScreenGameplayActions(
            process, topScreenDpadActions,
            nativeCandidateDispatch.TopScreenInput.ZrPressed ||
                nativeCandidateDispatch.TopScreenInput.ZlPressed,
            kTopScreenDirectCallReturn,
            &nativeCandidateDispatch.TopScreenGameplayActionRuntime,
            &nativeCandidateDispatch.TopScreenGameplayActions,
            &gameplayActionError)) {
      throw std::runtime_error(
          "TopScreen gameplay action failed: " +
          gameplayActionError);
    }
    nativeCandidateDispatch.PreviousTopScreenButtons = buttons;
    nativeCandidateDispatch.TopScreenItems.DirectItemId =
        nativeCandidateDispatch.TopScreenGameplayActionRuntime.DirectItem.ActiveItemId;
    nativeCandidateDispatch.PauseTransaction->AgeUndelivered();
    Oot3dNativeGame::TopScreenPauseDrawInputs pauseInputs;
    if (!Oot3dNativeGame::ReadTopScreenPauseDrawInputs(process.Memory(),
                                                       &pauseInputs)) {
      nativeCandidateDispatch.TopScreenPauseAotBarrier = true;
    } else {
      // ChildStates: [0] Gear 0x00504484, [1] Items 0x0050672C,
      // [2] Dungeon/World Map 0x00506CE8.
      Oot3dNativeGame::ObserveTopScreenStartRoutingPage(
          pauseInputs.ChildStates[1] != 0U, pauseInputs.ChildStates[0] != 0U,
          pauseInputs.ChildStates[2] != 0U,
          nativeCandidateDispatch.TopScreenStartRouting);
      const auto closePlan =
          Oot3dNativeGame::ResolveTopScreenPauseStartClose(
              pauseInputs,
              (pressed &
               button(Oot3dNativeGame::NativeA32HidButton::Start)) != 0U);
      // The transactions run inside this refresh's native UI update
      // (oot3d_top_screen_pause_transaction.h), not here: the guest may be
      // suspended inside a routine whose tail would undo them.
      if (closePlan.Page !=
          Oot3dNativeGame::TopScreenPauseClosePage::None) {
        nativeCandidateDispatch.PauseTransaction->RequestStartClose();
        Oot3dNativeGame::MarkTopScreenStartCloseGestureConsumed(
            nativeCandidateDispatch.TopScreenStartRouting);
        inputFrame.Hid.Buttons &=
            ~button(Oot3dNativeGame::NativeA32HidButton::Start);
        // START wins: a pending Save/Quit of the same page is dropped.
        nativeCandidateDispatch.TopScreenPauseSystemOpen = {};
      } else if (nativeCandidateDispatch.TopScreenStartRouting.OpenRequested) {
        nativeCandidateDispatch.PauseTransaction->RequestOpenLastPage(
            nativeCandidateDispatch.TopScreenStartRouting.LastPage);
      } else if (activeTopScreenConfig.ExitItemsToSaveScreen) {
        const auto systemOpenPlan =
            Oot3dNativeGame::ResolveTopScreenPauseBSystemOpen(
                pauseInputs,
                (pressed &
                 button(Oot3dNativeGame::NativeA32HidButton::B)) != 0U,
                nativeCandidateDispatch.TopScreenPauseSystemOpen);
        if (systemOpenPlan.Page !=
            Oot3dNativeGame::TopScreenPauseClosePage::None) {
          nativeCandidateDispatch.PauseTransaction->RequestSystemOpen(
              systemOpenPlan);
          // 0x005D5478..5558: B-to-Save clears the description.
          nativeCandidateDispatch.ItemDescription->Hide();
        }
      }
      const bool sceneRuntimeRoute = pauseInputs.HasScene &&
                                     pauseInputs.SceneMode == 3U &&
                                     (pauseInputs.RuntimeMode == 1U ||
                                      pauseInputs.RuntimeMode == 2U);
      nativeCandidateDispatch.TopScreenPauseAotBarrier =
          sceneRuntimeRoute ||
          Oot3dNativeGame::ResolveTopScreenPauseRouteActive(
              pauseInputs,
              &nativeCandidateDispatch.TopScreenAotPauseRoute);
    }
  }
  nativeCandidateDispatch.TopScreenStartRouting.OpenRequested = false;
  if (scenarioBootstrap.has_value() && !scenarioBootstrap->Finished()) {
    std::string scenarioError;
    if (!scenarioBootstrap->Advance(
            process, sceneViewProbe.Stats().LastPlayStateAddress,
            frameCount, &scenarioError)) {
      std::cerr << "oot3d_native_game: structural scenario failed: "
                << scenarioError << '\n';
      if (launch.ScenarioStrict) {
        window.Close();
        return false;
      }
    }
    if (launch.ScenarioAutoExit && scenarioBootstrap->Complete()) {
      window.Close();
      return false;
    }
  }
#if defined(TRIAEVUM_DEVELOPER_TOOLS)
  // F1 > Dev Tools warps run at the same guest-frame boundary as the
  // structural scenarios, through the same native transition request.
  if (devWarp != nullptr) {
    if (devWarpGuest == nullptr) devWarpGuest = Oot3dNativeGame::MakeDevWarpProcessGuest(process);
    devWarp->Advance(*devWarpGuest, sceneViewProbe.Stats().LastPlayStateAddress, frameCount);
    if (devLoadout != nullptr)
      devLoadout->Advance(*devWarpGuest, sceneViewProbe.Stats().LastPlayStateAddress, devWarp->Status(), frameCount);
    UpdateDevFreeCameraTour(frameCount);
    if (devLab) UpdateDevLab(frameCount);
  }
#endif
  refreshTickRemainder += kCtrArm11TicksPerSecond;
  nextVblankTick += refreshTickRemainder / kGuestDisplayRefreshRate;
  refreshTickRemainder %= kGuestDisplayRefreshRate;
  const auto clockStart = std::chrono::steady_clock::now();
  const auto clockResolution = AdvanceGuestClockTo(
      process, hostServices, processResult, nextVblankTick,
      launch.WholeAotBlockBudget);
  const double clockSeconds = SecondsSince(clockStart);
  phaseTiming.GuestClockSeconds += clockSeconds;
  phaseTiming.GuestSeconds += clockSeconds;
  if (clockResolution.OvershootTicks != 0U) {
    ++lateVblankDeadlineCount;
    maximumVblankDeadlineOvershootTicks =
        std::max(maximumVblankDeadlineOvershootTicks,
                 clockResolution.OvershootTicks);
  }
  const auto hidUpdate = hostServices.AdvanceHidToCurrentTick(
      process.Memory(), inputFrame.Hid);
  if (hidUpdate.Status ==
      Oot3dNativeGame::NativeA32CtrHidUpdateStatus::Failed) {
    throw std::runtime_error(
        "native HID sample could not be written to shared memory");
  }
  if (hidUpdate.EventsSignaled &&
      processResult.Kind !=
          Oot3dNativeGame::NativeA32ProcessRunKind::Terminated) {
    phaseStart = std::chrono::steady_clock::now();
    processResult =
        RunUntilGuestWait(process, launch.WholeAotBlockBudget);
    phaseTiming.GuestSeconds += SecondsSince(phaseStart);
    RequireRunnableGuest(processResult);
  }
  const uint32_t dspAudioFrames =
      hostServices.TakePendingDspAudioFrames();
  std::vector<int16_t> audioSamples;
  audioSamples.reserve(static_cast<size_t>(dspAudioFrames) * 320U);
  for (uint32_t audioFrame = 0; audioFrame < dspAudioFrames;
       ++audioFrame) {
    const size_t frameSampleOffset = audioSamples.size();
    const auto dspMixStart = std::chrono::steady_clock::now();
    CpuProbe::Scope cpuDsp(CpuProbe::Phase::Dsp);
    if (!dspHle.ProcessFrame(process.Memory(), audioSamples, &error)) {
      throw std::runtime_error("native DSP HLE mix failed: " + error);
    }
    phaseTiming.DspMixSeconds += SecondsSince(dspMixStart);
    cpuDsp.Stop();
    pcmContinuityDiagnostics.ObserveFrame(
        dspStats.FramesMixed, std::span<const int16_t>(audioSamples)
                            .subspan(frameSampleOffset));
    ++dspStats.FramesMixed;
    dspStats.FramesSuppressed += launch.DisableAudio ? 1U : 0U;
    if (!hostServices.SignalDspAudioFrame()) {
      break;
    }
    phaseStart = std::chrono::steady_clock::now();
    processResult =
        RunUntilGuestWait(process, launch.WholeAotBlockBudget);
    phaseTiming.GuestSeconds += SecondsSince(phaseStart);
    RequireRunnableGuest(processResult);
  }
  for (const int16_t sample : audioSamples) {
    const int32_t magnitude = std::abs(static_cast<int32_t>(sample));
    dspStats.PeakMagnitude = std::max(dspStats.PeakMagnitude, magnitude);
    dspStats.NonzeroSamples += sample != 0 ? 1U : 0U;
    const uint16_t bits = static_cast<uint16_t>(sample);
    dspStats.PcmFnv1a64 ^= static_cast<uint8_t>(bits);
    dspStats.PcmFnv1a64 *= 1099511628211ULL;
    dspStats.PcmFnv1a64 ^= static_cast<uint8_t>(bits >> 8U);
    dspStats.PcmFnv1a64 *= 1099511628211ULL;
  }
  dspStats.SamplesMixed += audioSamples.size() / 2U;
  if (!launch.AudioPcmDumpPath.empty()) {
    pcmDumpSamples.insert(pcmDumpSamples.end(), audioSamples.begin(),
                          audioSamples.end());
  }
  if (!launch.DisableAudio && audioPlayer != nullptr &&
      audioPlayer->IsInitialized() && !audioSamples.empty()) {
    const auto audioOutputStart = std::chrono::steady_clock::now();
    CpuProbe::Scope cpuAudio(CpuProbe::Phase::AudioOutput);
    const int32_t bufferedBefore = audioPlayer->Buffered();
    audioPlayer->Play(
        reinterpret_cast<const uint8_t *>(audioSamples.data()),
        audioSamples.size() * sizeof(int16_t));
    audioOutputDiagnostics.Observe(
        frameCount, bufferedBefore, audioPlayer->Buffered(),
        audioSamples.size() / 2U, audioPlayer->GetDesiredBuffered());
    phaseTiming.AudioOutputSeconds += SecondsSince(audioOutputStart);
  }
  if (!hostServices.SignalVBlank(process.Memory())) {
    throw std::runtime_error(
        "native VBlank could not enter the GSP relay queue");
  }
  ++vblankCount;
  return true;
}

// Runs the guest until it waits and drains the PICA work it submitted during the refresh.
void NativeA32WindowSession::RunGuestRefresh() {
  auto &hostServices = *hostServicesStorage;
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  const auto &picaMemoryView = *picaMemoryViewStorage;
  auto &submissionQueue = *submissionQueueStorage;
  auto &picaSemanticTrace = *picaSemanticTraceStorage;
  // PPF unblocks the current guest refresh, not the next presentation.
  // The dependency capture keeps its plans for the complete visual frame.
  (void)FlushNativeDependencies();
  sceneViewProbe.BeginGuestRefresh();
  phaseStart = std::chrono::steady_clock::now();
  processResult =
      RunUntilGuestWait(process, launch.WholeAotBlockBudget);
  phaseTiming.GuestSeconds += SecondsSince(phaseStart);
  RequireRunnableGuest(processResult);

  phaseStart = std::chrono::steady_clock::now();
  const auto picaSubmitStart = phaseStart;
  CpuProbe::Scope cpuQueue(CpuProbe::Phase::PicaQueue);
  size_t drainPassesThisRefresh = 0;
  bool refreshHadNativeDraws = false;
  while (drainPassesThisRefresh < kMaximumPicaDrainPassesPerRefresh) {
    PublishNativeSceneView();
    submissionQueue.TakePendingDraws(drainedDraws);
    auto &draws = drainedDraws;
    auto completions = submissionQueue.TakePendingCompletions();
    auto displayTransfers = submissionQueue.TakePendingDisplayTransfers();
    auto memoryFills = submissionQueue.TakePendingMemoryFills();
    if (draws.empty() && completions.empty() &&
        displayTransfers.empty() && memoryFills.empty()) {
      break;
    }
    ++drainPassesThisRefresh;
    ++picaStats.PicaDrainPassCount;
    refreshHadNativeDraws = refreshHadNativeDraws || !draws.empty();
    const bool suppressTopScreenFrontendBackdrop =
        Oot3dNativeGame::ShouldSuppressTopScreenFrontendBackdrop(
            launch.UiProfile,
            uiLifecycleBridge.NativeFrontendPresentationActive());
    // The native menu backdrops TopScreen removes: the shared
    // CommonBackground00 of File Select and Name Entry, and the version
    // choice's own background.
    std::array<std::optional<uint32_t>, 2> frontendBackdropPhysicalAddresses{};
    if (suppressTopScreenFrontendBackdrop) {
      const std::array<std::optional<uint32_t>, 2> frontendBackdropGuestAddresses{
          uiLifecycleBridge.NativePauseSharedTextureGuestAddress(
              oot3d::ui::UiPauseSharedTextureSlot::CommonBackground00),
          uiLifecycleBridge.NativeVersionChoiceBackdropGuestAddress()};
      for (size_t index = 0; index < frontendBackdropGuestAddresses.size();
           ++index) {
        if (frontendBackdropGuestAddresses[index].has_value()) {
          frontendBackdropPhysicalAddresses[index] =
              picaMemoryView.TranslateGuest(
                  *frontendBackdropGuestAddresses[index], 1U);
        }
      }
    }
    const auto usesFrontendBackdrop =
        [&](const Oot3dNativeGame::Oot3dPicaDecodedDrawState& state) {
          return std::any_of(
              state.Textures.begin(), state.Textures.end(),
              [&](const auto &texture) {
                return texture.Enabled &&
                       std::any_of(frontendBackdropPhysicalAddresses.begin(),
                                   frontendBackdropPhysicalAddresses.end(),
                                   [&](const auto &address) {
                                     return address.has_value() &&
                                            texture.PhysicalAddress == *address;
                                   });
              });
        };
    std::vector<Oot3dNativeGame::TopScreenFrontendCanvas>
        frontendBackdropCanvases;
    const auto rememberFrontendBackdropCanvas =
        [&](const Oot3dNativeGame::Oot3dPicaDecodedDrawState& state) {
          if (state.Framebuffer.ColorPhysicalAddress != 0U &&
              std::none_of(frontendBackdropCanvases.begin(),
                           frontendBackdropCanvases.end(),
                           [&](const auto& canvas) {
                             return canvas.Matches(state);
                           })) {
            frontendBackdropCanvases.push_back(
                {state.Framebuffer.ColorPhysicalAddress, state.Viewport});
          }
        };
    if (suppressTopScreenFrontendBackdrop) {
      for (const auto &draw : draws) {
        if (usesFrontendBackdrop(draw.State)) {
          rememberFrontendBackdropCanvas(draw.State);
        }
      }
    }
    bool signaledPicaCompletion = false;
    size_t completionIndex = 0;
    size_t memoryFillIndex = 0;
    size_t displayTransferIndex = 0;
    const auto submitMemoryFill =
        [&](const Oot3dNativeGame::Oot3dPicaMemoryFillSubmission &fill) {
          picaSemanticTrace.RecordMemoryFill(
              presentationFrameCount, frameCount, fill);
          picaPresentationScheduler.Capture(fill);
          if (fill.CompletionId != 0U) {
            if (!fill.Interrupt.has_value() ||
                !hostServices.SignalPicaInterrupt(process.Memory(),
                                                  *fill.Interrupt)) {
              throw std::runtime_error(
                  "native PICA memory fill completion "
                  "could not enter the GSP relay queue");
            }
            if (*fill.Interrupt ==
                Oot3dNativeGame::Oot3dPicaInterruptId::P3d) {
              ++picaStats.CompletedP3dCount;
            } else if (*fill.Interrupt ==
                       Oot3dNativeGame::Oot3dPicaInterruptId::Ppf) {
              ++picaStats.CompletedPpfCount;
            }
            ++picaStats.RecordedCompletionCount;
            signaledPicaCompletion = true;
          }
        };
    const auto submitDisplayTransfer =
        [&](const Oot3dNativeGame::Oot3dPicaDisplayTransferSubmission
                &transfer) {
          const auto topFramebuffer = hostServices.TopFramebuffer();
          const bool selectedForTop = topFramebuffer.has_value() &&
                                      (transfer.Transfer.OutputAddress ==
                                           topFramebuffer->AddressLeft ||
                                       transfer.Transfer.OutputAddress ==
                                           topFramebuffer->AddressRight);
          picaSemanticTrace.RecordDisplayTransfer(
              presentationFrameCount, frameCount, transfer,
              selectedForTop, suppressTopScreenFrontendBackdrop);
          if (launch.ExtendedDiagnostics) {
            if (displayTransferEvents.size() == 64U) {
              displayTransferEvents.erase(displayTransferEvents.begin());
            }
            displayTransferEvents.push_back({
                {"input_address", transfer.Transfer.InputAddress},
                {"output_address", transfer.Transfer.OutputAddress},
                {"input_physical_address", transfer.InputPhysicalAddress},
                {"output_physical_address",
                 transfer.OutputPhysicalAddress},
                {"input_size", transfer.Transfer.InputSize},
                {"output_size", transfer.Transfer.OutputSize},
                {"flags", transfer.Transfer.Flags},
                {"texture_copy_bytes", transfer.Transfer.TextureCopyBytes},
                {"after_draw_submission_id",
                 transfer.AfterDrawSubmissionId},
                {"signals_guest_interrupt", transfer.SignalInterrupt},
                {"selected_for_top_at_submission", selectedForTop},
                {"topscreen_frontend_backdrop_suppression_active",
                 suppressTopScreenFrontendBackdrop},
            });
            if (transfer.Transfer.TextureCopyBytes != 0U) {
              // Same span the backend writes: byte count plus the per-row
              // gap, so every byte the copy materializes is inside the
              // watch. For a raw copy OutputSize carries the native
              // width/height pair in 16-byte units (see the bridge).
              const uint32_t copySize =
                  transfer.Transfer.TextureCopyBytes & ~15U;
              const uint32_t copyGap =
                  (transfer.Transfer.OutputSize >> 16U) * 16U;
              const uint32_t copyWidth =
                  copyGap == 0U
                      ? copySize
                      : (transfer.Transfer.OutputSize & 0xFFFFU) * 16U;
              if (copySize != 0U && copyWidth != 0U) {
                const uint64_t copyEnd =
                    uint64_t{transfer.OutputPhysicalAddress} + copySize +
                    uint64_t{(copySize - 1U) / copyWidth} * copyGap;
                const auto copySpan = static_cast<size_t>(
                    std::min<uint64_t>(copyEnd, uint64_t{1} << 32) -
                    transfer.OutputPhysicalAddress);
                // The memory object is indexed by guest addresses: the
                // transfer names a physical destination, so it has to be
                // translated before anything can be trapped. A span that
                // does not translate is skipped rather than watched at the
                // wrong address.
                const auto watchGuest = picaMemoryView.Translate(
                    transfer.OutputPhysicalAddress, copySpan);
                if (watchGuest.has_value()) {
                  const std::pair<uint32_t, uint32_t> watched{
                      *watchGuest,
                      *watchGuest + static_cast<uint32_t>(copySpan)};
                  if (std::find(rawCopyWatchRanges.begin(),
                                rawCopyWatchRanges.end(),
                                watched) == rawCopyWatchRanges.end()) {
                    rawCopyWatchRanges.push_back(watched);
                    process.Memory().SetRangeWatch(rawCopyWatchRanges);
                  } else if (!process.Memory().RangeWatchActive()) {
                    process.Memory().SetRangeWatch(rawCopyWatchRanges);
                  }
                }
              }
            }
          }
          picaPresentationScheduler.Capture(transfer);
          if (transfer.SignalInterrupt && transfer.Transfer.TextureCopyBytes == 0U) {
            if (!hostServices.SignalPicaInterrupt(
                    process.Memory(),
                    Oot3dNativeGame::Oot3dPicaInterruptId::Ppf)) {
              throw std::runtime_error(
                  "native PICA display transfer completion could not "
                  "enter the GSP relay queue");
            }
            ++picaStats.CompletedPpfCount;
            ++picaStats.RecordedCompletionCount;
            signaledPicaCompletion = true;
          }
          displayTransfersByOutput[transfer.Transfer.OutputAddress] =
              transfer;
          ++picaStats.DisplayTransferCount;
        };
    while (displayTransferIndex < displayTransfers.size() &&
           displayTransfers[displayTransferIndex].AfterDrawSubmissionId <=
               lastSubmittedDrawId) {
      submitDisplayTransfer(displayTransfers[displayTransferIndex++]);
    }
    for (auto &draw : draws) {
      while (memoryFillIndex < memoryFills.size() &&
             memoryFills[memoryFillIndex].BeforeDrawSubmissionId <=
                 draw.Id) {
        submitMemoryFill(memoryFills[memoryFillIndex++]);
      }
      const bool usesCommonBackground =
          suppressTopScreenFrontendBackdrop && usesFrontendBackdrop(draw.State);
      const bool targetsFrontendBackdrop =
          std::any_of(frontendBackdropCanvases.begin(),
                      frontendBackdropCanvases.end(),
                      [&](const auto& canvas) {
                        return canvas.Matches(draw.State);
                      });
      const bool isOpaqueTargetInitialization =
          IsNativeFrontendOpaqueTargetInitializationDraw(draw);
      const bool suppressNativeFrontendBackdropDraw =
          suppressTopScreenFrontendBackdrop &&
          targetsFrontendBackdrop &&
          (usesCommonBackground || isOpaqueTargetInitialization);
      Oot3dNativeGame::Oot3dPicaVulkanDrawPlan plan;
      auto drawPhaseStart = std::chrono::steady_clock::now();
      if (!Oot3dNativeGame::
              BuildOot3dPicaVulkanDrawPlanAndConsumeResources(
                  draw, plan, &error, &shaderSourceCache)) {
        throw std::runtime_error("native PICA Vulkan plan failed: " +
                                 error);
      }
      phaseTiming.PicaPlanSeconds += SecondsSince(drawPhaseStart);
      if (picaRawDrawRecorder.Enabled() &&
          !picaRawDrawRecorder.Seen(plan.CanonicalIdentity.PipelineId)) {
        const auto &vertex = plan.VertexShader.SourceIdentity;
        const auto &fragment = plan.FragmentShader.SourceIdentity;
        picaRawDrawRecorder.Observe(
            plan.CanonicalIdentity.PipelineId, draw.Packet,
            {plan.VertexShader.StateKey, plan.FragmentShader.StateKey, vertex.Id,
             vertex.SecondaryHash, vertex.Size, fragment.Id, fragment.SecondaryHash,
             fragment.Size});
      }
      picaSemanticTrace.RecordDraw(
          presentationFrameCount, frameCount, draw, plan,
          suppressNativeFrontendBackdropDraw, usesCommonBackground,
          isOpaqueTargetInitialization);
      if (suppressNativeFrontendBackdropDraw) {
        picaStats.NativeMenuBackdropDrawOmissionCount +=
            usesCommonBackground ? 1U : 0U;
        picaStats.NativeMenuClearDrawOmissionCount +=
            isOpaqueTargetInitialization ? 1U : 0U;
      }
      if (launch.ExtendedDiagnostics) {
        drawPhaseStart = std::chrono::steady_clock::now();
        if (drawEvents.size() == 128U) {
          drawEvents.erase(drawEvents.begin());
        }
        nlohmann::json textures = nlohmann::json::array();
        for (size_t slot = 0; slot < draw.State.Textures.size();
             ++slot) {
          const auto &texture = draw.State.Textures[slot];
          if (!texture.Enabled) {
            continue;
          }
          const auto planTexture = std::find_if(
              plan.Textures.begin(), plan.Textures.end(),
              [slot](const auto &candidate) {
                return candidate.Slot == slot;
              });
          const std::span<const uint8_t> nativeBytes =
              planTexture != plan.Textures.end()
                  ? std::span<const uint8_t>(planTexture->NativeBytes)
                  : std::span<const uint8_t>();
          textures.push_back({
              {"slot", slot},
              {"width", texture.Width},
              {"height", texture.Height},
              {"physical_address", texture.PhysicalAddress},
              {"format", texture.Format},
              {"type", texture.Type},
              {"native_byte_count", nativeBytes.size()},
              {"native_byte_hash", HashDiagnosticBytes(nativeBytes)},
              {"native_nonzero_bytes",
               std::count_if(nativeBytes.begin(), nativeBytes.end(),
                             [](const uint8_t byte) {
                               return byte != 0U;
                             })},
          });
        }
        nlohmann::json vertexBindings = nlohmann::json::array();
        for (const auto &binding : plan.VertexBindings) {
          constexpr size_t kDiagnosticByteLimit = 256U;
          const auto bindingBytes = binding.ResolvedBytes();
          const size_t byteCount =
              std::min(bindingBytes.size(), kDiagnosticByteLimit);
          vertexBindings.push_back({
              {"binding", binding.Binding},
              {"byte_stride", binding.ByteStride},
              {"per_instance",
               binding.InputRate ==
                   Oot3dNativeGame::Oot3dPicaVertexInputRate::
                       PerInstance},
              {"byte_count", bindingBytes.size()},
              {"prefix_bytes",
               std::vector<uint8_t>(bindingBytes.begin(),
                                    bindingBytes.begin() + byteCount)},
          });
        }
        nlohmann::json defaultAttributes = nlohmann::json::array();
        for (size_t attributeIndex = 0;
             attributeIndex < draw.State.VertexInput.AttributeCount;
             ++attributeIndex) {
          if (!draw.State.VertexInput.Attributes[attributeIndex]
                   .Default) {
            continue;
          }
          defaultAttributes.push_back({
              {"attribute", attributeIndex},
              {"input_register",
               draw.State.ShaderInterface
                   .InputRegisterByAttribute[attributeIndex]},
              {"value", draw.Packet.DefaultAttributes[attributeIndex]},
          });
        }
        constexpr std::array<uint16_t, 6> kTevStageRegisters{
            0x0C0U, 0x0C8U, 0x0D0U, 0x0D8U, 0x0F0U, 0x0F8U};
        nlohmann::json tevStages = nlohmann::json::array();
        for (const uint16_t base : kTevStageRegisters) {
          tevStages.push_back({
              {"base_register", base},
              {"sources", draw.Packet.Registers[base]},
              {"modifiers", draw.Packet.Registers[base + 1U]},
              {"operations", draw.Packet.Registers[base + 2U]},
              {"constant", draw.Packet.Registers[base + 3U]},
              {"scales", draw.Packet.Registers[base + 4U]},
          });
        }
        drawEvents.push_back({
            {"frame", frameCount},
            {"submission_id", draw.Id},
            {"command_list_address", draw.Packet.CommandListAddress},
            {"command_list_offset_words",
             draw.Packet.CommandListOffsetWords},
            {"topscreen_frontend_backdrop_suppressed",
             suppressNativeFrontendBackdropDraw},
            {"native_frontend_background_texture", usesCommonBackground},
            {"native_frontend_target_initialization",
             isOpaqueTargetInitialization},
            {"vertex_count", draw.State.VertexInput.VertexCount},
            {"vertex_offset", draw.State.VertexInput.VertexOffset},
            {"minimum_vertex_index", draw.MinimumVertexIndex},
            {"maximum_vertex_index", draw.MaximumVertexIndex},
            {"indexed", draw.State.VertexInput.Indexed},
            {"indices_are_16_bit",
             draw.State.VertexInput.IndicesAre16Bit},
            {"attribute_count", draw.State.VertexInput.AttributeCount},
            {"vulkan_base_vertex", plan.BaseVertex},
            {"vulkan_vertex_bindings", plan.VertexBindings.size()},
            {"vertex_shader_key", plan.VertexShader.StateKey},
            {"fragment_shader_key", plan.FragmentShader.StateKey},
            {"vertex_shader_source",
             std::string(plan.ResolvedVertexShaderSource())},
            {"fragment_shader_source",
             std::string(plan.ResolvedFragmentShaderSource())},
            {"vertex_uniform_floats", plan.VertexShader.Uniforms.Floats},
            {"shader_output_mask", draw.State.ShaderInterface.OutputMask},
            {"shader_output_total",
             draw.Packet.Registers[0x04FU] & 7U},
            {"geometry_shader_enabled",
             draw.State.ShaderInterface.GeometryShaderEnabled},
            {"geometry_shader_mode",
             draw.Packet.Registers[0x229U] & 3U},
            {"shader_output_maps",
             {draw.Packet.Registers[0x050U],
              draw.Packet.Registers[0x051U],
              draw.Packet.Registers[0x052U],
              draw.Packet.Registers[0x053U],
              draw.Packet.Registers[0x054U],
              draw.Packet.Registers[0x055U],
              draw.Packet.Registers[0x056U]}},
            {"framebuffer",
             {{"color_address",
               draw.State.Framebuffer.ColorPhysicalAddress},
              {"depth_address",
               draw.State.Framebuffer.DepthPhysicalAddress},
              {"width", draw.State.Framebuffer.Width},
              {"height", draw.State.Framebuffer.Height},
              {"color_format", draw.State.Framebuffer.ColorFormat},
              {"depth_format", draw.State.Framebuffer.DepthFormat},
              {"flipped", draw.State.Framebuffer.Flipped}}},
            {"viewport",
             {{"half_width", draw.State.Viewport.HalfWidth},
              {"half_height", draw.State.Viewport.HalfHeight},
              {"depth_range", draw.State.Viewport.DepthRange},
              {"near_plane", draw.State.Viewport.NearPlane},
              {"z_buffering", draw.State.Viewport.ZBuffering},
              {"corner_x", draw.State.Viewport.CornerX},
              {"corner_y", draw.State.Viewport.CornerY}}},
            {"scissor",
             {{"mode", static_cast<uint32_t>(draw.State.Scissor.Mode)},
              {"x1", draw.State.Scissor.X1},
              {"y1", draw.State.Scissor.Y1},
              {"x2", draw.State.Scissor.X2},
              {"y2", draw.State.Scissor.Y2}}},
            {"topology", static_cast<uint32_t>(draw.State.Topology)},
            {"cull_mode", static_cast<uint32_t>(draw.State.CullMode)},
            {"output_merger",
             {{"fragment_operation_mode",
               plan.State.OutputMerger.FragmentOperationMode},
              {"color_write_mask",
               plan.State.OutputMerger.ColorWriteMask},
              {"logic_operation",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.LogicOperation)},
              {"blend_enabled", plan.State.OutputMerger.Blend.Enabled},
              {"blend_color_equation",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.Blend.ColorEquation)},
              {"blend_source_color",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.Blend.SourceColor)},
              {"blend_destination_color",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.Blend.DestinationColor)},
              {"depth_test_enabled",
               plan.State.OutputMerger.Depth.TestEnabled},
              {"depth_write_enabled",
               plan.State.OutputMerger.Depth.WriteEnabled},
              {"depth_compare",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.Depth.Compare)}}},
            {"textures", std::move(textures)},
            {"vertex_bindings", std::move(vertexBindings)},
            {"default_attributes", std::move(defaultAttributes)},
            {"tev_stages", std::move(tevStages)},
            {"combiner_buffer_color", draw.Packet.Registers[0x0FDU]},
        });
        phaseTiming.PicaDiagnosticsSeconds +=
            SecondsSince(drawPhaseStart);
      }
      if (!suppressNativeFrontendBackdropDraw) {
        picaPresentationScheduler.Capture(std::move(plan));
      }
      while (completionIndex < completions.size() &&
             completions[completionIndex].AfterDrawSubmissionId ==
                 draw.Id) {
        const auto &completion = completions[completionIndex++];
        if (!hostServices.SignalPicaInterrupt(process.Memory(),
                                              completion.Interrupt)) {
          throw std::runtime_error(
              "native PICA draw completion could not "
              "enter the GSP relay queue");
        }
        if (completion.Interrupt ==
            Oot3dNativeGame::Oot3dPicaInterruptId::P3d) {
          ++picaStats.CompletedP3dCount;
        } else if (completion.Interrupt ==
                   Oot3dNativeGame::Oot3dPicaInterruptId::Ppf) {
          ++picaStats.CompletedPpfCount;
        }
        ++picaStats.RecordedCompletionCount;
        signaledPicaCompletion = true;
      }
      lastSubmittedDrawId = draw.Id;
      while (
          displayTransferIndex < displayTransfers.size() &&
          displayTransfers[displayTransferIndex].AfterDrawSubmissionId ==
              draw.Id) {
        submitDisplayTransfer(displayTransfers[displayTransferIndex++]);
      }
      ++picaStats.DrawCount;
    }
    if (completionIndex != completions.size()) {
      throw std::runtime_error(
          "native PICA completion did not match a submitted draw");
    }
    while (memoryFillIndex < memoryFills.size()) {
      submitMemoryFill(memoryFills[memoryFillIndex++]);
    }
    if (displayTransferIndex != displayTransfers.size()) {
      throw std::runtime_error("native PICA display transfer did not "
                               "match a submitted draw");
    }
    signaledPicaCompletion =
        FlushNativeDependencies() || signaledPicaCompletion;
    if (!signaledPicaCompletion) {
      break;
    }
    phaseStart = std::chrono::steady_clock::now();
    processResult =
        RunUntilGuestWait(process, launch.WholeAotBlockBudget);
    const double resumedGuestSeconds = SecondsSince(phaseStart);
    phaseTiming.GuestSeconds += resumedGuestSeconds;
    phaseTiming.GuestInsidePicaSubmitSeconds += resumedGuestSeconds;
    RequireRunnableGuest(processResult);
  }
  if (drainPassesThisRefresh == kMaximumPicaDrainPassesPerRefresh &&
      (!submissionQueue.PendingDraws().empty() ||
       !submissionQueue.PendingCompletions().empty() ||
       !submissionQueue.PendingDisplayTransfers().empty() ||
       !submissionQueue.PendingMemoryFills().empty())) {
    throw std::runtime_error("native PICA completion chain exceeded "
                             "the per-refresh drain limit");
  }
  picaStats.MaximumPicaDrainPassesPerRefresh = std::max<uint64_t>(
      picaStats.MaximumPicaDrainPassesPerRefresh, drainPassesThisRefresh);
  picaStats.RefreshesWithNativeDraws += refreshHadNativeDraws ? 1U : 0U;
  phaseTiming.PicaSubmitSeconds += SecondsSince(picaSubmitStart);
}

// Presents the host frame: selects the top-screen transfer and replays the visual frame or its samples.
void NativeA32WindowSession::PresentVisualFrame(
    const double &presentationElapsedSeconds,
    const float &visualInterpolationAlpha) {
  auto &hostServices = *hostServicesStorage;
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &api = *apiPointer;
  auto &picaSemanticTrace = *picaSemanticTraceStorage;
  CpuProbe::Scope cpuVisual(CpuProbe::Phase::VisualPresentation);
  const auto visualPresentationStart = std::chrono::steady_clock::now();
  constexpr auto presentationRenderTargetNamespace =
      kVisualInterpolationRenderTargetNamespace;
  const bool lcdForceBlack = hostServices.LcdForceBlack();
  const auto topFramebuffer = hostServices.TopFramebuffer();
  const Oot3dNativeGame::Oot3dPicaDisplayTransferSubmission
      *traceSelectedTopTransfer = nullptr;
  if (topFramebuffer.has_value()) {
    auto traceSelected =
        displayTransfersByOutput.find(topFramebuffer->AddressLeft);
    if (traceSelected == displayTransfersByOutput.end() &&
        topFramebuffer->AddressRight != topFramebuffer->AddressLeft) {
      traceSelected =
          displayTransfersByOutput.find(topFramebuffer->AddressRight);
    }
    if (traceSelected != displayTransfersByOutput.end()) {
      traceSelectedTopTransfer = &traceSelected->second;
    }
  }
  picaSemanticTrace.RecordPresentationSelection(
      presentationFrameCount, frameCount, lcdForceBlack,
      topFramebuffer.has_value()
          ? std::optional<uint32_t>(topFramebuffer->AddressLeft)
          : std::nullopt,
      topFramebuffer.has_value()
          ? std::optional<uint32_t>(topFramebuffer->AddressRight)
          : std::nullopt,
      traceSelectedTopTransfer);
  // In-world: every host frame's render side learns its frame state (a world frame publishes a pair or
  // nothing; it never re-scans or publishes the panel), also without a selected top transfer (LCD
  // forced black, no top framebuffer or no transfer for it).
  bool xrFrameCommandSubmitted = false;
  const auto submitXrFrameCommand = [&] {
    if (xrWorld == nullptr || xrFrameCommandSubmitted) return;
    xrFrameCommandSubmitted = true;
    renderStream.Submit([&, xrFrame = xrWorld->FrameState()] {
      if (!rendererFrameSkipped) BeginXrRenderCommand(xrFrame);
    });
  };
  if (!lcdForceBlack) {
    if (topFramebuffer.has_value()) {
      auto selected =
          displayTransfersByOutput.find(topFramebuffer->AddressLeft);
      if (selected == displayTransfersByOutput.end() &&
          topFramebuffer->AddressRight != topFramebuffer->AddressLeft) {
        selected =
            displayTransfersByOutput.find(topFramebuffer->AddressRight);
      }
      if (selected != displayTransfersByOutput.end()) {
        // Guest side: transfer selection, frame completion and the
        // guest-memory reads the renderer needs. The rest runs as one
        // render command, in renderer order.
        const auto topSelection = selected->second;
        std::shared_ptr<Oot3dNativeGame::Oot3dPicaVisualFrame>
            completedFrame;
        // Guest frames completed before the selected one while the host
        // fell behind: canonical work and interpolation history only.
        std::shared_ptr<std::vector<Oot3dNativeGame::Oot3dPicaVisualFrame>>
            supersededFrames;
        std::vector<Fast::Oot3d::GrassInteractor> frameActors;
        bool suppressTopScreenBackdrop = false;
        if (topSelection.CompletionId !=
            lastSelectedTopTransferCompletionId) {
          lastSelectedTopTransferCompletionId =
              topSelection.CompletionId;
          ++picaStats.SelectedTopTransferCount;
          // TopScreen draws the relocated lower menu onto the upper target
          // after the upper snapshot: that work completes the same frame.
          // Reframed's file selection draws inside the upper pass instead.
          const bool relocatedFrontend =
              Oot3dNativeGame::ShouldSuppressTopScreenFrontendBackdrop(
                  launch.UiProfile,
                  uiLifecycleBridge.NativeFrontendPresentationActive()) &&
              !Oot3dNativeGame::TopScreenReframedFileSelect::Active(process.Memory());
          const auto frontendMenuCanvases =
              Oot3dNativeGame::TopScreenFrontendMenuCanvases();
          auto finishedFrames = picaPresentationScheduler.FinishFrames(
              topSelection, topDisplayOutputs,
              relocatedFrontend
                  ? std::span<const Oot3dNativeGame::Oot3dPicaViewportState>(
                        frontendMenuCanvases)
                  : std::span<const Oot3dNativeGame::Oot3dPicaViewportState>());
          if (std::find(topDisplayOutputs.begin(), topDisplayOutputs.end(),
                        topSelection.OutputPhysicalAddress) == topDisplayOutputs.end()) {
            if (topDisplayOutputs.size() == 4U)
              topDisplayOutputs.erase(topDisplayOutputs.begin());
            topDisplayOutputs.push_back(topSelection.OutputPhysicalAddress);
          }
          std::optional<Oot3dNativeGame::Oot3dPicaVisualFrame> finished;
          if (!finishedFrames.empty()) {
            finished = std::move(finishedFrames.back());
            finishedFrames.pop_back();
            if (!finishedFrames.empty())
              supersededFrames = std::make_shared<
                  std::vector<Oot3dNativeGame::Oot3dPicaVisualFrame>>(
                  std::move(finishedFrames));
          }
          if (finished.has_value()) {
            frameActors = Oot3dNativeGame::CollectNativeActorInteractions(
                process.Memory(),
                sceneViewProbe.Stats().LastPlayStateAddress,
                finished->Sequence);
            if (xrWorld != nullptr) {
              // In-world (inworld-spec §4.1, §4.5): the guest snapshot belongs to the frame it is
              // drawn with; the W1 state and the rig are decided here and travel with the command.
              const auto &probe = playerTimingProbe.Stats();
              const uint32_t playState = probe.LastPlayStateAddress != 0U
                                             ? probe.LastPlayStateAddress
                                             : sceneViewProbe.Stats().LastPlayStateAddress;
              uint32_t nativeGameMode = UINT32_MAX;
              Oot3dNativeGame::XrHostSignals host;
              host.Gameplay = process.Memory().Read32(kNativeGameMode, &nativeGameMode) &&
                              Oot3dNativeGame::IsTopScreenGameplayInputMode(nativeGameMode);
              host.HostGui = gui->GetMenuOrMenubarVisible() || gui->GetAnyGuiWindowVisible() ||
                             inGameMenuStorage->IsOpen();
              xrWorld->StampFrame(
                  Oot3dNativeGame::ReadXrGuestSnapshot(process.Memory(), probe.LastPlayerAddress, playState),
                  Oot3dNativeGame::CollectXrFrameFacts(*finished), host, finished->Sequence);
            }
            if (xrViewRenderer != nullptr && xrViewRenderer->TraceEnabled()) {
              // The W0 trace groups its table by scene (play+0x104).
              uint16_t scene = 0xFFFFU;
              const uint32_t playState = sceneViewProbe.Stats().LastPlayStateAddress;
              if (playState != 0U && process.Memory().Read16(playState + 0x104U, &scene))
                xrTraceScene.store(static_cast<int16_t>(scene));
            }
            ++visualStats.SnapshotCount;
            suppressTopScreenBackdrop = relocatedFrontend;
            completedFrame =
                std::make_shared<Oot3dNativeGame::Oot3dPicaVisualFrame>(
                    std::move(*finished));
          }
        }
        xrFrameCommandSubmitted = true;
        renderStream.Submit([&, topSelection, completedFrame, supersededFrames,
                             frameActors, suppressTopScreenBackdrop,
                             presentationElapsedSeconds,
                             visualInterpolationAlpha, frameCount = frameCount,
                             guestReferences =
                                 CollectGuestCopyReferences(),
                             xrFrame = xrWorld != nullptr
                                           ? xrWorld->FrameState()
                                           : Oot3dNativeGame::XrFrameState{}] {
        if (rendererFrameSkipped) return;
        if (xrWorld != nullptr) BeginXrRenderCommand(xrFrame);
        guestCopyReferences = guestReferences;
        bool presentedVisualSample = false;
        bool selectedNewVisualFrame = false;
        {
          if (completedFrame != nullptr && supersededFrames != nullptr) {
            for (auto &superseded : *supersededFrames) {
              auto &sample = visualFrameSampleScratch;
              (void)Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                  superseded, sample, true);
              const auto nativeTemporalSample =
                  Fast::Oot3d::BuildNativeFrameTemporalSample(
                      frameCompositionPolicy,
                      Fast::Oot3d::NativeFrameTemporalSampleKind::
                          Authoritative,
                      superseded.Sequence, superseded.Sequence,
                      visualContinuityEpoch, 1.0F,
                      static_cast<float>(presentationElapsedSeconds));
              ExecuteVisualSample(
                  sample,
                  Oot3dNativeGame::Oot3dPicaPresentationExecutionKind::
                      NativeFrame,
                  false, nativeTemporalSample,
                  kNativeDependencyRenderTargetNamespace);
              if (visualInterpolationEnabled) {
                Oot3dNativeGame::ComposeTopScreenFrontendFrame(
                    superseded, suppressTopScreenBackdrop);
                preparedVisualTransition.Reset();
                if (latestVisualFrame.has_value())
                  previousVisualFrame = std::move(latestVisualFrame);
                latestVisualFrame = std::move(superseded);
              }
            }
          }
          if (completedFrame != nullptr) {
            std::optional<Oot3dNativeGame::Oot3dPicaVisualFrame>
                currentVisualFrame(std::move(*completedFrame));
            Fast::Oot3d::GrassInteractionBridge::Instance().PublishFrame(
                currentVisualFrame->Sequence, frameActors);
            selectedNewVisualFrame = true;
            auto &sample = visualFrameSampleScratch;
            // Keep the canonical target complete independently of the
            // delayed visual target. Raw-copy prefixes have already
            // executed; only the native suffix remains here.
            (void)Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                *currentVisualFrame, sample, true);
            const auto nativeTemporalSample =
                Fast::Oot3d::BuildNativeFrameTemporalSample(
                    frameCompositionPolicy,
                    Fast::Oot3d::NativeFrameTemporalSampleKind::
                        Authoritative,
                    currentVisualFrame->Sequence,
                    currentVisualFrame->Sequence, visualContinuityEpoch,
                    1.0F, static_cast<float>(presentationElapsedSeconds));
            ExecuteVisualSample(
                sample,
                Oot3dNativeGame::Oot3dPicaPresentationExecutionKind::
                    NativeFrame,
                false, nativeTemporalSample,
                kNativeDependencyRenderTargetNamespace);
            Oot3dNativeGame::ComposeTopScreenFrontendFrame(
                *currentVisualFrame, suppressTopScreenBackdrop);
            if (!visualInterpolationEnabled) {
              if (Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                      *currentVisualFrame, sample)) {
                const auto temporalSample =
                    Fast::Oot3d::BuildNativeFrameTemporalSample(
                        frameCompositionPolicy,
                        Fast::Oot3d::NativeFrameTemporalSampleKind::
                            Authoritative,
                        currentVisualFrame->Sequence,
                        currentVisualFrame->Sequence,
                        visualContinuityEpoch, 1.0F,
                        static_cast<float>(
                            presentationElapsedSeconds));
                ExecuteVisualSample(
                    sample,
                    Oot3dNativeGame::
                        Oot3dPicaPresentationExecutionKind::CurrentFrame,
                    true, temporalSample);
                ++visualStats.DirectFramesPresented;
                ++visualStats.SamplesPresented;
                visualStats.SampleDrawCount += sample.Draws.size();
                presentedVisualSample = true;
              }
              // Direct gameplay modes retain no cross-frame matching
              // history. The scheduler keeps only the executed scanout
              // snapshot needed if presentation temporarily outruns
              // simulation.
              preparedVisualTransition.Reset();
              previousVisualFrame.reset();
              latestVisualFrame.reset();
              latestVisualTransitionContinuous = false;
            } else {
              bool resynchronizeVisualNamespace =
                  !latestVisualFrame.has_value();
              if (latestVisualFrame.has_value()) {
                const bool transitionPrepared =
                    preparedVisualTransition.Prepare(
                        *latestVisualFrame, *currentVisualFrame,
                        &visualInterpolationTiming);
                const auto &transition =
                    preparedVisualTransition.Stats();
                const float sampleAlpha =
                    visualSampleCadence.Resolve(
                        true, visualInterpolationAlpha);
                const bool visualSampleReady =
                    transitionPrepared &&
                    preparedVisualTransition.Sample(
                        *latestVisualFrame, *currentVisualFrame,
                        sampleAlpha, sample,
                        &visualInterpolationTiming);
                ++visualStats.TransitionCount;
                visualStats.MatchedDrawCount += transition.MatchedDraws;
                visualStats.StrictUniqueMatchedDrawCount +=
                    transition.StrictUniqueMatchedDraws;
                visualStats.StrictOrdinalMatchedDrawCount +=
                    transition.StrictOrdinalMatchedDraws;
                visualStats.StructuralOrdinalMatchedDrawCount +=
                    transition.StructuralOrdinalMatchedDraws;
                visualStats.PipelineOrdinalMatchedDrawCount +=
                    transition.PipelineOrdinalMatchedDraws;
                visualStats.ChangedContinuousDrawCount +=
                    transition.ChangedContinuousDraws;
                visualStats.ChangedPerInstanceVertexDrawCount +=
                    transition.ChangedPerInstanceVertexDraws;
                visualStats.ChangedPerVertexDrawCount +=
                    transition.ChangedPerVertexDraws;
                visualStats.AmbiguousPreviousDrawCount +=
                    transition.AmbiguousPreviousDraws;
                visualStats.AmbiguousCurrentDrawCount +=
                    transition.AmbiguousCurrentDraws;
                visualStats.UnmatchedPreviousDrawCount +=
                    transition.UnmatchedPreviousDraws;
                visualStats.UnmatchedCurrentDrawCount +=
                    transition.UnmatchedCurrentDraws;
                const double continuityThreshold =
                    visualContinuityTracker.CurrentThreshold();
                const bool continuousTransition =
                    transitionPrepared &&
                    visualContinuityTracker.Accept(transition);
                latestVisualTransitionContinuous = continuousTransition;
                visualStats.DiscontinuityCount +=
                    continuousTransition ? 0U : 1U;
                if (launch.ExtendedDiagnostics) {
                  if (visualTransitionEvents.size() == 1024U) {
                    visualTransitionEvents.erase(
                        visualTransitionEvents.begin());
                  }
                  visualTransitionEvents.push_back({
                      {"host_frame", frameCount},
                      {"previous_sequence",
                       latestVisualFrame->Sequence},
                      {"current_sequence",
                       currentVisualFrame->Sequence},
                      {"matched_draws", transition.MatchedDraws},
                      {"strict_unique_matched_draws",
                       transition.StrictUniqueMatchedDraws},
                      {"strict_ordinal_matched_draws",
                       transition.StrictOrdinalMatchedDraws},
                      {"structural_ordinal_matched_draws",
                       transition.StructuralOrdinalMatchedDraws},
                      {"pipeline_ordinal_matched_draws",
                       transition.PipelineOrdinalMatchedDraws},
                      {"changed_per_instance_vertex_draws",
                       transition.ChangedPerInstanceVertexDraws},
                      {"changed_per_vertex_draws",
                       transition.ChangedPerVertexDraws},
                      {"unmatched_previous_draws",
                       transition.UnmatchedPreviousDraws},
                      {"unmatched_current_draws",
                       transition.UnmatchedCurrentDraws},
                      {"top_target_matched_draws",
                       transition.TopTargetMatchedDraws},
                      {"matched_draw_coverage",
                       transition.MatchedDrawCoverage},
                      {"median_normalized_continuous_delta",
                       transition.MedianNormalizedContinuousDelta},
                      {"p90_normalized_continuous_delta",
                       transition.P90NormalizedContinuousDelta},
                      {"continuity_threshold", continuityThreshold},
                      {"continuous", continuousTransition},
                  });
                }
                if (continuousTransition && visualSampleReady) {
                  const auto temporalSample =
                      Fast::Oot3d::BuildNativeFrameTemporalSample(
                          frameCompositionPolicy,
                          Fast::Oot3d::NativeFrameTemporalSampleKind::
                              Transition,
                          latestVisualFrame->Sequence,
                          currentVisualFrame->Sequence,
                          visualContinuityEpoch, sampleAlpha,
                          static_cast<float>(
                              presentationElapsedSeconds));
                  ExecuteVisualSample(
                      sample,
                      Oot3dNativeGame::
                          Oot3dPicaPresentationExecutionKind::
                              InterpolatedFrame,
                      true, temporalSample);
                  ++visualStats.SamplesPresented;
                  visualStats.SampleDrawCount += sample.Draws.size();
                  visualStats.InterpolatedDrawCount +=
                      sample.InterpolatedDraws;
                  visualStats.InterpolatedPerInstanceVertexDrawCount +=
                      sample.InterpolatedPerInstanceVertexDraws;
                  visualStats.InterpolatedPerVertexDrawCount +=
                      sample.InterpolatedPerVertexDraws;
                  presentedVisualSample = true;
                } else {
                  resynchronizeVisualNamespace = true;
                }
              }
              if (resynchronizeVisualNamespace &&
                  Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                      *currentVisualFrame, sample)) {
                preparedVisualTransition.Reset();
                ++visualContinuityEpoch;
                const auto temporalSample =
                    Fast::Oot3d::BuildNativeFrameTemporalSample(
                        frameCompositionPolicy,
                        Fast::Oot3d::NativeFrameTemporalSampleKind::
                            Resynchronized,
                        currentVisualFrame->Sequence,
                        currentVisualFrame->Sequence,
                        visualContinuityEpoch, 1.0F,
                        static_cast<float>(
                            presentationElapsedSeconds));
                ExecuteVisualSample(
                    sample,
                    Oot3dNativeGame::
                        Oot3dPicaPresentationExecutionKind::CurrentFrame,
                    true, temporalSample);
                ++visualStats.NamespaceResyncCount;
                ++visualStats.SamplesPresented;
                visualStats.SampleDrawCount += sample.Draws.size();
                presentedVisualSample = true;
              }
              if (latestVisualFrame.has_value()) {
                previousVisualFrame = std::move(latestVisualFrame);
              } else {
                latestVisualTransitionContinuous = false;
              }
              latestVisualFrame = std::move(currentVisualFrame);
            }
          }
        }
        if (!selectedNewVisualFrame && !presentedVisualSample &&
            visualInterpolationEnabled &&
            latestVisualTransitionContinuous &&
            previousVisualFrame.has_value() &&
            latestVisualFrame.has_value() &&
            preparedVisualTransition.Ready()) {
          auto &sample = visualFrameSampleScratch;
          const float sampleAlpha =
              visualSampleCadence.Resolve(
                  false, visualInterpolationAlpha);
          if (preparedVisualTransition.Sample(
                  *previousVisualFrame, *latestVisualFrame, sampleAlpha,
                  sample, &visualInterpolationTiming)) {
            const auto temporalSample =
                Fast::Oot3d::BuildNativeFrameTemporalSample(
                    frameCompositionPolicy,
                    Fast::Oot3d::NativeFrameTemporalSampleKind::
                        Transition,
                    previousVisualFrame->Sequence,
                    latestVisualFrame->Sequence,
                    visualContinuityEpoch, sampleAlpha,
                    static_cast<float>(presentationElapsedSeconds));
            ExecuteVisualSample(
                sample,
                Oot3dNativeGame::
                    Oot3dPicaPresentationExecutionKind::RepeatedFrame,
                true, temporalSample);
            ++visualStats.SamplesPresented;
            visualStats.SampleDrawCount += sample.Draws.size();
            visualStats.InterpolatedDrawCount += sample.InterpolatedDraws;
            visualStats.InterpolatedPerInstanceVertexDrawCount +=
                sample.InterpolatedPerInstanceVertexDraws;
            visualStats.InterpolatedPerVertexDrawCount +=
                sample.InterpolatedPerVertexDraws;
            presentedVisualSample = true;
          }
        }
        if (!presentedVisualSample && !(xrWorld != nullptr && xrRenderState.World) &&
            picaPresentationScheduler.HasSnapshot(
                topSelection,
                presentationRenderTargetNamespace)) {
          const auto started = std::chrono::steady_clock::now();
          if (!picaPresentationScheduler.PresentExisting(
                  api, topSelection,
                  presentationRenderTargetNamespace, &renderError)) {
            throw std::runtime_error(
                "native top-screen scheduled scanout failed: " +
                renderError);
          }
          rendererTiming->VisualReplaySeconds += SecondsSince(started);
          ++visualStats.ReusedSnapshotPresentations;
        }
        });
      }
    }
    submitXrFrameCommand();  // before the bottom frontend command, which reads the frame state

    const bool presentNativeBottomFrontend =
        Oot3dNativeGame::ShouldPresentNativeBottomFrontend(
            launch.UiProfile,
            uiLifecycleBridge.NativeFrontendPresentationActive());
    const auto bottomFramebuffer = hostServices.BottomFramebuffer();
    if (presentNativeBottomFrontend &&
        bottomFramebuffer.has_value()) {
      ++picaStats.NativeFrontendPresentationActiveCount;
      auto selectedBottom =
          displayTransfersByOutput.find(bottomFramebuffer->AddressLeft);
      if (selectedBottom == displayTransfersByOutput.end() &&
          bottomFramebuffer->AddressRight != 0U &&
          bottomFramebuffer->AddressRight !=
              bottomFramebuffer->AddressLeft) {
        selectedBottom = displayTransfersByOutput.find(
            bottomFramebuffer->AddressRight);
      }
      picaStats.NativeFrontendBottomTransferHitCount +=
          selectedBottom != displayTransfersByOutput.end() ? 1U : 0U;
      if (selectedBottom != displayTransfersByOutput.end()) {
        renderStream.Submit(
            [&, bottomSelection = selectedBottom->second] {
              if (rendererFrameSkipped || (xrWorld != nullptr && xrRenderState.World) ||
                  !picaPresentationScheduler.HasSnapshot(
                      bottomSelection,
                      presentationRenderTargetNamespace)) {
                return;
              }
              if (!picaPresentationScheduler.PresentExisting(
                      api, bottomSelection,
                      presentationRenderTargetNamespace, &renderError)) {
                throw std::runtime_error(
                    "native bottom-screen frontend presentation "
                    "failed: " + renderError);
              }
              ++picaStats.NativeBottomFrontendPresentationCount;
            });
      }
    }
  }
  submitXrFrameCommand();  // LCD forced black
  phaseTiming.VisualPresentationSeconds +=
      SecondsSince(visualPresentationStart);
}

#if defined(TRIAEVUM_DEVELOPER_TOOLS)
void NativeA32WindowSession::UpdateDevFreeCameraTour(uint64_t guestFrame) {
  if (devTourPoses.empty() || devTourIndex >= static_cast<int>(devTourPoses.size()) || devWarp == nullptr) return;
  constexpr uint32_t kStartFrames = 90U;  // the scene's title card fades
  // Held for 45 frames in which the game took the free view (an intro or
  // other cutscene camera does not count), at most 900 frames.
  constexpr uint32_t kHoldFrames = 45U;
  constexpr uint32_t kGiveUpFrames = 900U;
  constexpr float kRadians = 3.14159265F / 180.0F;
  const auto status = devWarp->Status();
  const auto pose = [&](int index) {
    const auto &p = devTourPoses[static_cast<size_t>(index)];
    const float yaw = devTourAnchorYaw * kRadians;
    const std::array<float, 3> forward{std::sin(yaw), 0.0F, std::cos(yaw)};
    const std::array<float, 3> right{std::cos(yaw), 0.0F, -std::sin(yaw)};
    std::array<float, 3> eye{p.Forward, p.Right, p.Up};
    if (!p.Absolute)
      for (size_t axis = 0; axis < 3; ++axis)
        eye[axis] = devTourAnchor[axis] + forward[axis] * p.Forward + right[axis] * p.Right + (axis == 1 ? p.Up : 0.0F);
    const float viewYaw = p.Absolute ? p.Yaw : devTourAnchorYaw + p.Yaw;
    nativeCandidateDispatch.DevFreeCamera->SetPose(eye, viewYaw, p.Pitch);
    devTourAppliedAtPose = nativeCandidateDispatch.DevFreeCamera->AppliedCount();
    std::fprintf(stderr, "TRIAEVUM_DEV_FREECAM_TOUR pose=%d eye=%.1f,%.1f,%.1f yaw=%.1f pitch=%.1f frame=%llu\n", index,
                 eye[0], eye[1], eye[2], viewYaw, p.Pitch, static_cast<unsigned long long>(guestFrame));
  };
  if (devTourIndex < 0) {
    // A boot warp point is requested once gameplay starts: wait for it too.
    if (status.Phase != Oot3dNativeGame::DevWarpPhase::Idle) devTourAfterWarp = true;
    const bool ready = devTourAfterWarp ? status.Phase == Oot3dNativeGame::DevWarpPhase::Arrived
                                        : status.Observation.Gameplay && status.Observation.Player;
    if (!ready) {
      devTourFrames = 0;
      return;
    }
    if (++devTourFrames < kStartFrames) return;
    devTourAnchor = {status.Observation.Position[0], status.Observation.Position[1], status.Observation.Position[2]};
    devTourAnchorYaw = static_cast<float>(status.Observation.RotY) * (180.0F / 32768.0F);
    devTourIndex = 0;
    devTourFrames = 0;
    pose(0);
    return;
  }
  ++devTourFrames;
  // Only frames drawn from the main camera count: PlayState +0xA64 is the
  // active camera's index (cameras at +0xA54), 0 the main camera.
  int16_t activeCamera = -1;
  const uint32_t playState = sceneViewProbe.Stats().LastPlayStateAddress;
  if (playState != 0U) (void)processStorage->Memory().Read16(playState + 0xA64U, reinterpret_cast<uint16_t *>(&activeCamera));
  const uint64_t appliedNow = nativeCandidateDispatch.DevFreeCamera->AppliedCount();
  if (activeCamera != 0) devTourAppliedAtPose = appliedNow;
  const uint64_t applied = appliedNow - devTourAppliedAtPose;
  if (applied < kHoldFrames && devTourFrames < kGiveUpFrames) return;
  char name[32];
  std::snprintf(name, sizeof(name), "tour-%02d.bmp", devTourIndex);
  pendingForcedScreenshot = devTourDirectory / name;
  std::fprintf(stderr, "TRIAEVUM_DEV_FREECAM_TOUR capture=%s applied=%llu%s\n",
               pendingForcedScreenshot.string().c_str(), static_cast<unsigned long long>(applied),
               applied < kHoldFrames ? " not-applied" : "");
  devTourFrames = 0;
  if (++devTourIndex < static_cast<int>(devTourPoses.size()))
    pose(devTourIndex);
  else
    std::fprintf(stderr, "TRIAEVUM_DEV_FREECAM_TOUR done\n");
}

void NativeA32WindowSession::UpdateDevLab(uint64_t guestFrame) {
  using Json = nlohmann::json;
  auto &camera = *nativeCandidateDispatch.DevFreeCamera;
  const auto status = devWarp->Status();
  const auto &seen = status.Observation;
  const auto place = [&] {
    return Json{{"frame", guestFrame},
                {"scene", seen.Scene},
                {"room", seen.Room},
                {"position", {seen.Position[0], seen.Position[1], seen.Position[2]}},
                {"yaw", static_cast<float>(seen.RotY) * (180.0F / 32768.0F)},
                {"gameplay", seen.Gameplay}};
  };
  const auto finish = [&](std::string_view result, Json detail) {
    devLab->Answer(*devLabCommand, result, detail);
    devLabCommand.reset();
  };
  const auto vector3 = [](const Json &value) {
    return std::array<float, 3>{value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
  };
  // The in-game menu as the agent sees it: open, tab, focused row and value,
  // pane, status line and modal title.
  const auto menuState = [&] {
    const auto &model = inGameMenuStorage->Model();
    Json state{{"frame", guestFrame},
               {"open", model.IsOpen()},
               {"tabIndex", model.TabIndex()},
               {"rowIndex", model.RowIndex()},
               {"statusLine", model.Status()},
               {"statusError", model.StatusIsError()}};
    if (model.TabIndex() < model.Doc().Tabs.size()) state["tab"] = model.Doc().Tabs[model.TabIndex()].Title;
    if (const auto *row = model.FocusedRow(); row != nullptr) {
      state["row"] = row->Key;
      state["value"] = row->ValueText();
      state["enabled"] = row->Enabled;
      state["modified"] = row->IsModified;
    }
    switch (model.FocusPane()) {
      case Oot3dNativeGame::Menu::Pane::Rows: state["pane"] = "rows"; break;
      case Oot3dNativeGame::Menu::Pane::Choices: state["pane"] = "choices"; break;
      case Oot3dNativeGame::Menu::Pane::Modal: state["pane"] = "modal"; break;
    }
    if (model.CurrentModal()) state["modal"] = model.CurrentModal()->Title;
    // The pointer's targets as last drawn, in canvas units, when asked for.
    if (devLabCommand && devLabCommand->value("regions", false)) {
      Json regions = Json::array();
      for (const auto &region : model.CurrentLayout().Regions) {
        regions.push_back({{"kind", Oot3dNativeGame::Menu::MenuHitKindName(region.Kind)},
                           {"index", region.Index},
                           {"x", region.Area.X},
                           {"y", region.Area.Y},
                           {"width", region.Area.Width},
                           {"height", region.Area.Height}});
      }
      state["regions"] = std::move(regions);
    }
    return state;
  };
  if (!devLabCommand) {
    devLabCommand = devLab->Next();
    if (!devLabCommand) return;
    devLabFrames = 0;
    devLabSawArrival = false;
    const auto &command = *devLabCommand;
    const std::string op = command["op"].get<std::string>();
    try {
      if (op == "warp" && command.contains("better")) {
        // An entrance of the Debug Warp Screen's Better list ([scene, entrance]
        // indices), with age ("child"/"adult") and time ("day"/"night").
        const auto scenes = Oot3dNativeGame::DevWarpBetterScenes();
        const auto scene = command["better"].at(0).get<size_t>();
        const auto entrance = command["better"].at(1).get<size_t>();
        if (scene >= scenes.size() || entrance >= scenes[scene].Count)
          throw std::out_of_range("no such Better scene/entrance");
        devWarp->WarpTo(Oot3dNativeGame::DevWarpBetterEntrances()[scenes[scene].First + entrance],
                        command.value("age", std::string("child")) == "adult" ? Oot3dNativeGame::DevWarpAge::Adult
                                                                               : Oot3dNativeGame::DevWarpAge::Child,
                        command.value("time", std::string("day")) == "night");
      } else if (op == "warp") {
        // Link respawns at the position in the room (SoH's warp point), facing yaw.
        Oot3dNativeGame::DevWarpPoint point;
        point.EntranceId = command.value("entrance", 0);
        point.RoomNum = static_cast<int8_t>(command.value("room", 0));
        const auto position = vector3(command.at("position"));
        std::copy(position.begin(), position.end(), point.Position);
        point.RotY = static_cast<int16_t>(std::lround(command.value("yaw", 0.0) * 32768.0 / 180.0));
        devWarp->WarpToPlace("lab " + command.value("id", Json()).dump(), point);
      } else if (op == "camera") {
        camera.SetPose(vector3(command.at("eye")), command.value("yaw", 0.0F), command.value("pitch", 0.0F));
        devLabAppliedAt = camera.AppliedCount();
      } else if (op == "camera_off") {
        camera.SetActive(false);
        finish("ok", place());
      } else if (op == "capture") {
        // Written by the render stream with this frame; the file appears shortly after.
        pendingForcedScreenshot = command.at("path").get<std::string>();
        // In-world mode: "view" picks the left eye, the right eye or the panel/HUD image (default:
        // the left eye of a world frame, else the panel); ignored by the flat renderers.
        pendingForcedScreenshotView = command.value("view", std::string());
        finish("ok", {{"frame", guestFrame},
                      {"path", pendingForcedScreenshot.string()},
                      {"view", pendingForcedScreenshotView.empty() ? std::string("default")
                                                                   : pendingForcedScreenshotView}});
      } else if (op == "status") {
        const auto environment = sceneEnvironment->Snapshot();
        auto detail = place();
        detail["playState"] = sceneViewProbe.Stats().LastPlayStateAddress;
        detail["warp"] = status.Message;
        detail["profile"] = {{"file", environment.FileExists ? environment.Path.string() : std::string()},
                             {"enabled", environment.Profile.Enabled},
                             {"revision", environment.Revision},
                             {"error", environment.Error}};
        finish("ok", detail);
      } else if (op == "graphics") {
        // A partial "Graphics" block, applied like an F1 commit (not saved),
        // as the qualification schedule does.
        auto &graphicsSettings = Fast::Oot3d::GraphicsSettingsRuntime::Instance();
        const auto current = graphicsSettings.Snapshot();
        const auto decoded = Fast::Oot3d::DeserializeGraphicsSettings(
            Oot3dNativeGame::MergeScheduledGraphics(Fast::Oot3d::SerializeGraphicsSettings(current),
                                                    command.at("graphics")),
            current);
        const auto applied = graphicsSettings.Apply(decoded.Value, false);
        const auto issueDetails = [](const auto& issues, const char* phase) {
          auto details = Json::array();
          for (const auto& issue : issues) {
            details.push_back({{"field", issue.Field}, {"message", issue.Message}, {"corrected", issue.Corrected}});
            std::fprintf(stderr, "TRIAEVUM_DEV_LAB_GRAPHICS_ISSUE phase=%s field=\"%s\" corrected=%d message=\"%s\"\n",
                         phase, issue.Field.c_str(), issue.Corrected, issue.Message.c_str());
          }
          return details;
        };
        const auto effective = graphicsSettings.SnapshotForRendering();
        const auto effectiveJson = Fast::Oot3d::SerializeGraphicsSettings(effective.Value);
        finish(applied.Accepted() && !decoded.UnsupportedFutureVersion ? "ok" : "failed",
               {{"frame", guestFrame}, {"decodeIssues", decoded.Issues.size()},
                {"applyIssues", applied.Issues.size()},
                {"decodeIssueDetails", issueDetails(decoded.Issues, "decode")},
                {"applyIssueDetails", issueDetails(applied.Issues, "apply")},
                {"effectiveSurfaceLighting", effectiveJson["Effects"]["SurfaceLighting"]},
                {"effectiveDirectionalShadows", effectiveJson["Effects"]["DirectionalShadows"]["Mode"]},
                {"effectiveCasterFaces", static_cast<unsigned>(effective.Environment.CasterFaces)}});
      } else if (op == "surface_trace") {
        // The surface-lighting candidates of the next frames, one JSON line each.
        Fast::Renderer3ds::PicaSurfaceDebug::Instance().RequestTrace(command.at("path").get<std::string>(),
                                                                     command.value("frames", 1U),
                                                                     command.value("census", false));
        finish("ok", {{"frame", guestFrame}});
      } else if (op == "surface_exempt") {
        // Candidates first..last keep their native appearance ({} clears).
        Fast::Renderer3ds::PicaSurfaceDebug::Instance().SetExemptRange(command.value("first", 1U),
                                                                       command.value("last", 0U));
        finish("ok", {{"frame", guestFrame}});
      } else if (op == "peek") {
        // Guest memory as hex (at most 4 KiB): finding actor fields.
        const uint32_t address = command.at("address").get<uint32_t>();
        const uint32_t size = std::min(command.value("size", 256U), 4096U);
        std::string hex;
        hex.reserve(size * 2U);
        static constexpr char kDigits[] = "0123456789abcdef";
        bool readable = true;
        for (uint32_t i = 0; i < size && readable; ++i) {
          uint8_t byte = 0;
          readable = processStorage->Memory().Read8(address + i, &byte);
          hex += kDigits[byte >> 4U];
          hex += kDigits[byte & 15U];
        }
        finish(readable ? "ok" : "failed", {{"frame", guestFrame}, {"address", address}, {"hex", hex}});
      } else if (op == "poke16") {
        // One halfword written into guest memory (developer experiments only).
        const uint32_t address = command.at("address").get<uint32_t>();
        const uint16_t value = static_cast<uint16_t>(command.at("value").get<int>());
        const bool written = processStorage->Memory().Write16(address, value);
        finish(written ? "ok" : "failed", {{"frame", guestFrame}, {"address", address}});
      } else if (op == "water") {
        // The scene's water boxes as the renderer receives them.
        const auto water = Oot3dNativeGame::CaptureNativeWaterBodies(sceneViewProbe.Stats().LastPlayStateAddress, processStorage->Memory());
        Json bodies = Json::array();
        for (uint32_t i = 0; i < water.Count; ++i) {
          const auto &body = water.Bodies[i];
          bodies.push_back({{"min", {body.MinimumXZ[0], body.MinimumXZ[1]}},
                            {"max", {body.MaximumXZ[0], body.MaximumXZ[1]}},
                            {"y", body.SurfaceY},
                            {"properties", body.Properties}});
        }
        auto detail = place();
        detail["water"] = std::move(bodies);
        finish("ok", detail);
      } else if (op == "actors") {
        // The live actors (optionally only some ids): id, params, instance,
        // room, world position and scale.
        Json actors = Json::array();
        std::vector<uint16_t> only;
        if (command.contains("ids"))
          for (const auto &id : command["ids"]) only.push_back(id.get<uint16_t>());
        for (const auto &a : Oot3dNativeGame::CaptureNativeActors(sceneViewProbe.Stats().LastPlayStateAddress,
                                                                  processStorage->Memory())) {
          if (!only.empty() && std::find(only.begin(), only.end(), a.Id) == only.end()) continue;
          actors.push_back({{"id", a.Id},
                            {"params", a.Params},
                            {"category", a.Category},
                            {"room", a.Room},
                            {"address", a.Address},
                            {"position", {a.Position[0], a.Position[1], a.Position[2]}},
                            {"scale", {a.Scale[0], a.Scale[1], a.Scale[2]}}});
        }
        auto detail = place();
        detail["actors"] = std::move(actors);
        finish("ok", detail);
      } else if (op == "emitters") {
        // The native lights linked now (LightContext), with their owners.
        Json lights = Json::array();
        const uint32_t playState = sceneViewProbe.Stats().LastPlayStateAddress;
        for (const auto &e : Oot3dNativeGame::CaptureNativeEmitters(playState, processStorage->Memory())) {
          Json light = {{"type", e.Type},
                        {"position", {e.Position[0], e.Position[1], e.Position[2]}},
                        {"direction", {e.Direction[0], e.Direction[1], e.Direction[2]}},
                        {"color", {e.Color[0], e.Color[1], e.Color[2]}},
                        {"radius", e.Radius}};
          if (e.Owner && e.Owner->Kind == "actor")
            light["owner"] = {{"kind", "actor"},
                              {"actorId", e.Owner->ActorId},
                              {"actor", e.Owner->ActorAddress},
                              {"params", e.Owner->Params},
                              {"offset", e.Owner->Offset}};
          else if (e.Owner)
            light["owner"] = {{"kind", e.Owner->Kind}, {"offset", e.Owner->Offset}};
          lights.push_back(std::move(light));
        }
        auto detail = place();
        detail["lights"] = std::move(lights);
        finish("ok", detail);
      } else if (op == "gizmos") {
        sceneEnvironment->ShowLightGizmos(command.value("shown", true));
        finish("ok", {{"frame", guestFrame}});
      } else if (op == "menu") {
        // The in-game menu driven by synthetic presses ("steps": toggle, up,
        // down, left, right, confirm, back, tab_prev, tab_next), each held
        // one guest frame at the start of a "stepFrames" period (default 4);
        // answered with the menu's state once the last press was released.
        // Pointer steps: "move:<kind>[:<index>]" places the mouse on that
        // region's centre, "click:..." presses it one frame, "press:..."
        // holds it for the period, "wheel:<notches>", "pointer_out".
        static constexpr std::array<std::string_view, 9> kMenuSteps{
            "toggle", "up", "down", "left", "right", "confirm", "back", "tab_prev", "tab_next"};
        static constexpr std::array<std::string_view, 5> kMenuPointerSteps{"move", "click", "press", "wheel",
                                                                           "pointer_out"};
        devLabMenuSteps.clear();
        for (const auto &step : command.value("steps", Json::array())) {
          const auto name = step.get<std::string>();
          const std::string verb = name.substr(0, name.find(':'));
          if (std::find(kMenuSteps.begin(), kMenuSteps.end(), name) == kMenuSteps.end() &&
              std::find(kMenuPointerSteps.begin(), kMenuPointerSteps.end(), verb) == kMenuPointerSteps.end())
            throw std::invalid_argument("unknown menu step " + name);
          devLabMenuSteps.push_back(name);
        }
        // The pointer keeps its place from the previous command; only the
        // presses start clear.
        Oot3dNativeGame::InGameMenuHostInput pointerOnly;
        pointerOnly.PointerPresent = devLabMenuInput.PointerPresent;
        pointerOnly.PointerX = devLabMenuInput.PointerX;
        pointerOnly.PointerY = devLabMenuInput.PointerY;
        pointerOnly.PictureX = devLabMenuInput.PictureX;
        pointerOnly.PictureY = devLabMenuInput.PictureY;
        pointerOnly.PictureWidth = devLabMenuInput.PictureWidth;
        pointerOnly.PictureHeight = devLabMenuInput.PictureHeight;
        devLabMenuInput = pointerOnly;
        devLabMenuMissingTarget.clear();
        devLabMenuDriving = true;
      } else if (op == "quit") {
        finish("ok", place());
        devLabQuit = true;
      } else if (op != "wait") {
        finish("rejected", {{"reason", "unknown op"}});
      }
    } catch (const std::exception &error) {
      finish("rejected", {{"reason", error.what()}});
    }
    return;
  }
  ++devLabFrames;
  const auto &command = *devLabCommand;
  const std::string op = command["op"].get<std::string>();
  if (op == "warp") {
    // Settled: the given frames after arriving (title cards and fades).
    if (status.Phase == Oot3dNativeGame::DevWarpPhase::Failed) {
      finish("failed", {{"reason", status.Message}});
    } else if (status.Phase == Oot3dNativeGame::DevWarpPhase::Arrived) {
      if (!devLabSawArrival) {
        devLabSawArrival = true;
        devLabFrames = 0;
      } else if (devLabFrames >= command.value("settleFrames", 60U)) {
        finish("ok", place());
      }
    } else if (devLabFrames >= 3600U) {
      finish("timeout", {{"reason", status.Message}});
    }
  } else if (op == "camera") {
    // Held until the game took the free view for the given frames from the
    // main camera (PlayState +0xA64, 0): a cutscene camera does not count.
    int16_t activeCamera = -1;
    const uint32_t playState = sceneViewProbe.Stats().LastPlayStateAddress;
    if (playState != 0U)
      (void)processStorage->Memory().Read16(playState + 0xA64U, reinterpret_cast<uint16_t *>(&activeCamera));
    if (activeCamera != 0) devLabAppliedAt = camera.AppliedCount();
    const uint64_t applied = camera.AppliedCount() - devLabAppliedAt;
    if (applied >= command.value("holdFrames", 45U))
      finish("ok", {{"frame", guestFrame}, {"applied", applied}});
    else if (devLabFrames >= command.value("timeoutFrames", 900U))
      finish("timeout", {{"frame", guestFrame}, {"applied", applied}, {"activeCamera", activeCamera}});
  } else if (op == "wait") {
    if (devLabFrames >= command.value("frames", 30U)) finish("ok", place());
  } else if (op == "menu") {
    const uint32_t period = std::max(command.value("stepFrames", 4U), 2U);
    const uint32_t elapsed = devLabFrames - 1U;
    const size_t step = elapsed / period;
    Oot3dNativeGame::InGameMenuHostInput input;
    // The pointer keeps its place between steps (a "move" or "click" sets it
    // on a region's centre, as the menu last laid it out); the other steps
    // are one-frame presses.
    input.PointerPresent = devLabMenuInput.PointerPresent;
    input.PointerX = devLabMenuInput.PointerX;
    input.PointerY = devLabMenuInput.PointerY;
    input.PictureX = devLabMenuInput.PictureX;
    input.PictureY = devLabMenuInput.PictureY;
    input.PictureWidth = devLabMenuInput.PictureWidth;
    input.PictureHeight = devLabMenuInput.PictureHeight;
    if (step < devLabMenuSteps.size()) {
      if (elapsed % period == 0U) {
        const std::string &name = devLabMenuSteps[step];
        input.Toggle = name == "toggle";
        input.Up = name == "up";
        input.Down = name == "down";
        input.Left = name == "left";
        input.Right = name == "right";
        input.Confirm = name == "confirm";
        input.Back = name == "back";
        input.TabPrevious = name == "tab_prev";
        input.TabNext = name == "tab_next";
        // "move:<kind>[:<index>]", "click:<kind>[:<index>]", "press:<kind>[:<index>]"
        // (held for the whole period), "wheel:<notches>", "pointer_out".
        const auto colon = name.find(':');
        const std::string verb = name.substr(0, colon);
        const std::string target = colon == std::string::npos ? "" : name.substr(colon + 1);
        if (verb == "move" || verb == "click" || verb == "press") {
          const auto second = target.find(':');
          const std::string kind = target.substr(0, second);
          const size_t index = second == std::string::npos ? 0U : std::stoul(target.substr(second + 1));
          bool found = false;
          for (const auto &region : inGameMenuStorage->Model().CurrentLayout().Regions) {
            if (Oot3dNativeGame::Menu::MenuHitKindName(region.Kind) != kind || region.Index != index) continue;
            // Canvas units to window pixels through the same fit the menu
            // uses, in the presentation picture the host poll last reported.
            const auto &hostMenu = nativeControlPollingState.Menu;
            const auto fit = Oot3d::Renderer::FitUiPresentation(hostMenu.PictureWidth, hostMenu.PictureHeight,
                                                                1280.0F / 3.0F, 240.0F);
            input.PointerPresent = true;
            input.PictureX = hostMenu.PictureX;
            input.PictureY = hostMenu.PictureY;
            input.PictureWidth = hostMenu.PictureWidth;
            input.PictureHeight = hostMenu.PictureHeight;
            input.PointerX = static_cast<float>(hostMenu.PictureX) + fit.X +
                             (region.Area.X + region.Area.Width * 0.5F) * fit.Width / (1280.0F / 3.0F);
            input.PointerY = static_cast<float>(hostMenu.PictureY) + fit.Y +
                             (region.Area.Y + region.Area.Height * 0.5F) * fit.Height / 240.0F;
            found = true;
            break;
          }
          if (!found) devLabMenuMissingTarget = name;
          input.PointerPressed = found && verb != "move";
        } else if (verb == "wheel") {
          input.PointerWheel = std::stof(target);
        } else if (verb == "pointer_out") {
          input.PointerPresent = false;
        }
      } else if (devLabMenuSteps[step].rfind("press:", 0) == 0) {
        input.PointerPressed = devLabMenuInput.PointerPressed;
      }
      devLabMenuInput = input;
    } else {
      devLabMenuInput = input;
      // One more period after the last release: the menu's refresh and the
      // presentation of the final state are then behind the answer.
      if (elapsed >= (devLabMenuSteps.size() + 1U) * period) {
        // The lab keeps driving the menu between commands (its pointer stays
        // where the last step put it, no press held), so the host's real
        // mouse over the window cannot refocus a row behind the lab's back.
        auto state = menuState();
        if (!devLabMenuMissingTarget.empty()) state["missingTarget"] = devLabMenuMissingTarget;
        finish(devLabMenuMissingTarget.empty() ? "ok" : "missing_target", std::move(state));
      }
    }
  }
}
#endif

void NativeA32WindowSession::UpdateDevFreeCamera(Oot3dNativeGame::NativeA32InputFrame &inputFrame) {
  auto &freeCamera = *nativeCandidateDispatch.DevFreeCamera;
  const Uint8 *keys = SDL_GetKeyboardState(nullptr);
  const auto &io = ImGui::GetIO();
  const bool keyboard = keys != nullptr && !io.WantCaptureKeyboard;
  const bool toggle = keyboard && keys[SDL_SCANCODE_F6] != 0U;
  if (toggle && !devFreeCameraToggleHeld) freeCamera.SetActive(!freeCamera.Active());
  devFreeCameraToggleHeld = toggle;
  const auto now = std::chrono::steady_clock::now();
  const float seconds = devFreeCameraLastRefresh.time_since_epoch().count() == 0
      ? 0.0F
      : std::min(std::chrono::duration<float>(now - devFreeCameraLastRefresh).count(), 0.1F);
  devFreeCameraLastRefresh = now;
  // Physical sources, not the game's bindings (its R is the right mouse
  // button): the mouse motion since the last refresh, the pad as it is.
  auto &sources = nativeControlPollingState;
  const float mouseX = static_cast<float>(sources.DevMouseDeltaX);
  const float mouseY = static_cast<float>(sources.DevMouseDeltaY);
  sources.DevMouseDeltaX = sources.DevMouseDeltaY = 0;
  if (!freeCamera.Active()) return;
  // Pad: left stick moves, right stick looks (120 degrees per second), L/R
  // shoulders down/up.
  float forward = sources.DevPadLeftStick[1];
  float right = sources.DevPadLeftStick[0];
  float up = (sources.DevPadRightShoulder ? 1.0F : 0.0F) - (sources.DevPadLeftShoulder ? 1.0F : 0.0F);
  float yaw = sources.DevPadRightStick[0] * 120.0F * seconds;
  float pitch = sources.DevPadRightStick[1] * 120.0F * seconds;
  float multiplier = 1.0F;
  if (keyboard) {
    // WASD moves, Q/E down/up, Shift faster, Ctrl slower.
    forward += (keys[SDL_SCANCODE_W] ? 1.0F : 0.0F) - (keys[SDL_SCANCODE_S] ? 1.0F : 0.0F);
    right += (keys[SDL_SCANCODE_D] ? 1.0F : 0.0F) - (keys[SDL_SCANCODE_A] ? 1.0F : 0.0F);
    up += (keys[SDL_SCANCODE_E] ? 1.0F : 0.0F) - (keys[SDL_SCANCODE_Q] ? 1.0F : 0.0F);
    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) multiplier = 4.0F;
    if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]) multiplier = 0.25F;
  }
  // Left or right mouse button held: drag to look (0.15 degrees per pixel).
  // Not io.WantCaptureMouse: the full-window "Main Game" ImGui surface always
  // sets it, so looking never worked; only an open host menu or window
  // (DevMouseLookHeld is false then) owns the mouse.
  if (sources.DevMouseLookHeld) {
    yaw += mouseX * 0.15F;
    pitch -= mouseY * 0.15F;
  }
  const auto clampAxis = [](float value) { return std::clamp(value, -1.0F, 1.0F); };
  freeCamera.Move(clampAxis(forward), clampAxis(right), clampAxis(up), yaw, pitch, seconds, multiplier);
  // Link stays still: the game sees no pad, sticks or touch.
  inputFrame.Hid.Buttons = 0U;
  inputFrame.Hid.CirclePadX = inputFrame.Hid.CirclePadY = 0;
  inputFrame.Hid.TouchPressed = false;
  inputFrame.CStick = {};
}

void NativeA32WindowSession::PublishNativeSceneView() {
  auto &process = *processStorage;
  // Guest memory is read here; the scene environment, the duplicate check
  // and the publish belong to the renderer and run in its order.
  auto snapshot = sceneViewProbe.Capture(process.Memory());
  auto environmentTime = snapshot && !snapshot->FileSelect
      ? Oot3dNativeGame::CaptureSceneEnvironmentTime(snapshot->PlayStateAddress, process.Memory())
      : std::nullopt;
  if (environmentTime)
    environmentTime->Group = Oot3dNativeGame::CaptureNativeEnvironmentGroup(
        snapshot->PlayStateAddress, process.Memory(), &environmentTime->AuthoredScene);
  // The file select's title scene is not read for time or headers (the title
  // demo's PlayState may still be updating behind the cards, or the block may
  // read scene 0): fixed time, no group, one profile (scene-00FF).
  if (snapshot && snapshot->FileSelect)
    environmentTime = Oot3dNativeGame::SceneEnvironmentTime{
        .TimeDriven = false, .Clock = 0x8000, .Night = false, .Owner = snapshot->PlayStateAddress};
  {
    // TRIAEVUM_SCENE_VIEW_TRACE=1: per presentation frame, what the probes
    // hold when the scene view is captured (the PlayState and player the
    // Player_Update entry supplied, the snapshot's scene, camera and time).
    static const bool sceneViewTrace = std::getenv("TRIAEVUM_SCENE_VIEW_TRACE") != nullptr;
    static unsigned long sceneViewTraceFrames = 0;
    if (sceneViewTrace) {
      std::fprintf(stderr,
                   "TRIAEVUM_SCENE_VIEW frame=%lu playState=%08x timingPlayState=%08x player=%08x snapshot=%d sceneId=%d camera=%d eye=%.1f,%.1f,%.1f time=%d night=%d clock=%u driven=%d\n",
                   ++sceneViewTraceFrames, sceneViewProbe.Stats().LastPlayStateAddress,
                   playerTimingProbe.Stats().LastPlayStateAddress, playerTimingProbe.Stats().LastPlayerAddress,
                   snapshot ? 1 : 0, snapshot && snapshot->SceneId ? static_cast<int>(*snapshot->SceneId) : -1,
                   snapshot && snapshot->CameraAvailable ? 1 : 0, snapshot ? snapshot->Eye[0] : 0.0F,
                   snapshot ? snapshot->Eye[1] : 0.0F, snapshot ? snapshot->Eye[2] : 0.0F,
                   environmentTime ? 1 : 0, environmentTime && environmentTime->Night ? 1 : 0,
                   environmentTime ? static_cast<unsigned>(environmentTime->Clock) : 0U,
                   environmentTime && environmentTime->TimeDriven ? 1 : 0);
    }
  }
  // Where Link is (world position +0x28, shape yaw +0xBE: 0 faces +Z,
  // 0x4000 +X), for lights placed from F1.
  std::optional<Oot3dNativeGame::ScenePlayerPlacement> player;
  if (const uint32_t actor = playerTimingProbe.Stats().LastPlayerAddress; actor != 0U) {
    Oot3dNativeGame::ScenePlayerPlacement placement;
    uint32_t bits[3]{};
    uint16_t yaw = 0;
    if (process.Memory().Read32(actor + 0x28U, &bits[0]) &&
        process.Memory().Read32(actor + 0x2CU, &bits[1]) &&
        process.Memory().Read32(actor + 0x30U, &bits[2]) &&
        process.Memory().Read16(actor + 0xBEU, &yaw)) {
      for (size_t axis = 0; axis < 3; ++axis)
        placement.Position[axis] = std::bit_cast<float>(bits[axis]);
      placement.Yaw = static_cast<float>(static_cast<int16_t>(yaw)) * (180.0F / 32768.0F);
      if (std::isfinite(placement.Position[0]) && std::isfinite(placement.Position[1]) &&
          std::isfinite(placement.Position[2]))
        player = placement;
    }
    // With TRIAEVUM_SCENE_VIEW_TRACE, also the raw placement words (oracle
    // fixtures put Link at the same spot).
    static const bool placementTrace = std::getenv("TRIAEVUM_SCENE_VIEW_TRACE") != nullptr;
    if (placementTrace) {
      uint32_t rotation[2]{};
      process.Memory().Read32(actor + 0x34U, &rotation[0]);
      process.Memory().Read32(actor + 0x38U, &rotation[1]);
      std::fprintf(stderr, "TRIAEVUM_SCENE_VIEW_PLAYER actor=%08x pos=%08x,%08x,%08x rot34=%08x,%08x yaw=%04x\n",
                   actor, bits[0], bits[1], bits[2], rotation[0], rotation[1], yaw);
    }
  }
  // Light gizmos also show while the developer free camera looks around.
  bool freeCamera = false;
#if defined(TRIAEVUM_DEVELOPER_TOOLS)
  freeCamera = nativeCandidateDispatch.DevFreeCamera->Active();
  if (devEmitterLog && snapshot)
    devEmitterLog->Observe(++devEmitterFrames, snapshot->PlayStateAddress, snapshot->SceneId, process.Memory());
#endif
  renderStream.Submit([&, snapshot, environmentTime, player, freeCamera] {
  sceneEnvironment->ObserveScene(snapshot ? snapshot->SceneId : std::nullopt,
      environmentTime.value_or(Oot3dNativeGame::SceneEnvironmentTime{}), player);
  sceneEnvironment->PollProfileFiles();
  const auto shadowSettings = Fast::Oot3d::GraphicsSettingsRuntime::Instance().SnapshotForRendering();
  picaCompositionTracker.SetRoomShadowShellsEnabled(
      shadowSettings.Environment.CasterFaces == Fast::Renderer3ds::ShadowCasterFaces::InteriorShells &&
      shadowSettings.Value.Effects.DirectionalShadows.Mode == Fast::Oot3d::DirectionalShadowMode::VirtualShadowMaps);
  sceneEnvironment->PublishLightGizmo(
      snapshot && snapshot->CameraAvailable ? std::optional(snapshot->Eye) : std::nullopt, freeCamera);
  if (!snapshot.has_value() ||
      snapshot->Serial == lastPublishedSceneViewSerial) {
    return;
  }
  const Fast::Oot3d::TitleSceneViewSubmission view{
      snapshot->GuestFunction,
      snapshot->GuestReturnAddress,
      snapshot->Left,
      snapshot->Right,
      snapshot->Bottom,
      snapshot->Top,
      snapshot->NearPlane,
      snapshot->FarPlane,
      snapshot->Eye,
      snapshot->At,
      snapshot->CameraAvailable,
      // Rooms share their scene's coordinates; another scene is another world.
      snapshot->SceneId.has_value() ? uint64_t{*snapshot->SceneId} + 1U : 0U,
  };
  if (titleRenderBackend != nullptr &&
      titleRenderBackend->PublishSceneView(view)) {
    lastPublishedSceneViewSerial = snapshot->Serial;
    ++publishedSceneViewCount;
  }
  });
}

// Guest memory and guest interrupts belong to the guest. The renderer
// reaches them only from a command the guest waits for (inline or Run);
// anything else would race the running guest, so it fails instead.
void NativeA32WindowSession::RequireStoppedGuest(const char *access) {
  if (renderStream.InCommand() && !renderStream.InSynchronousCommand()) {
    throw std::runtime_error(
        std::string("renderer reached guest state while the guest was running (") + access + ")");
  }
}

std::vector<Fast::Renderer3ds::PicaTextureCopyReference> NativeA32WindowSession::CollectGuestCopyReferences() {
  auto &submissionQueue = *submissionQueueStorage;
  std::vector<Fast::Renderer3ds::PicaTextureCopyReference> references;
  submissionQueue.CollectLiveCopyReferences(references);
  for (const auto& draw : submissionQueue.PendingDraws()) {
    for (const auto& resource : draw.Resources) {
      references.insert(references.end(), resource.CopyReferences.begin(),
                        resource.CopyReferences.end());
    }
  }
  for (const auto& plan : picaPresentationScheduler.PendingDraws()) {
    Oot3dNativeGame::CollectOot3dPicaPlanCopyReferences(plan, references);
  }
  return references;
}

void NativeA32WindowSession::PinGpuCopyResources(const Oot3dNativeGame::Oot3dPicaVisualFrameViewSample& sample) {
  auto &api = *apiPointer;
  pinnedCopyReferences = guestCopyReferences;
  if (previousVisualFrame) {
    Oot3dNativeGame::CollectOot3dPicaFrameCopyReferences(
        *previousVisualFrame, pinnedCopyReferences);
  }
  if (latestVisualFrame) {
    Oot3dNativeGame::CollectOot3dPicaFrameCopyReferences(
        *latestVisualFrame, pinnedCopyReferences);
  }
  // The active sample may already have left the accumulator, or may be
  // the new frame before it replaces latestVisualFrame.
  for (const auto& draw : sample.Draws) {
    if (draw.BasePlan != nullptr) {
      Oot3dNativeGame::CollectOot3dPicaPlanCopyReferences(
          *draw.BasePlan, pinnedCopyReferences);
    }
  }
  if (!api.SetPinnedPicaTextureCopies(pinnedCopyReferences, &renderError)) {
    throw std::runtime_error("native PICA copy retention failed: " + renderError);
  }
}

bool NativeA32WindowSession::FlushNativeDependencies() {
  auto &api = *apiPointer;
  // The accumulator is guest-owned, so whether there is dependency work is
  // known here; only real work waits for the renderer.
  if (!picaPresentationScheduler.ViewDependencyWork(dependencySampleScratch))
    return false;
  // Synchronous: raw copies read and write guest memory and signal PPF
  // while the guest waits.
  bool flushed = false;
  renderStream.Run([&] {
    // Without an acquired image the flush waits; PPF arrives later, as it
    // may on hardware.
    if (rendererFrameSkipped) return;
    guestCopyReferences = CollectGuestCopyReferences();
    PinGpuCopyResources(dependencySampleScratch);
    if (!picaPresentationScheduler.Execute(
            api, dependencySampleScratch, kNativeDependencyRenderTargetNamespace,
            Oot3dNativeGame::Oot3dPicaPresentationExecutionKind::DependencyFlush,
            false, &renderError)) {
      throw std::runtime_error("native PICA dependency flush failed: " + renderError);
    }
    flushed = true;
  });
  return flushed;
}

void NativeA32WindowSession::ExecuteVisualSample(const Oot3dNativeGame::Oot3dPicaVisualFrameViewSample &sample, Oot3dNativeGame::Oot3dPicaPresentationExecutionKind kind, bool present, const Fast::Oot3d::NativeFrameTemporalSample& temporalSample, uint64_t renderTargetNamespace) {
  auto &api = *apiPointer;
  // In-world mode: every presented sample passes here (current, interpolated, repeated, resync), the
  // single choke point of inworld-spec §2.1. The canonical namespace-1 replay is untouched.
  if (xrViewRenderer != nullptr && present &&
      renderTargetNamespace == kVisualInterpolationRenderTargetNamespace) {
    if (xrViewRenderer->TraceEnabled()) xrViewRenderer->Trace(sample, xrTraceScene.load());
    if (ExecuteXrEyes(sample, kind, temporalSample)) return;
  }
  const auto started = std::chrono::steady_clock::now();
  (void)api.PublishPicaFrameTemporalSample(temporalSample);
  // Only displayed samples advance the grass colliders: the authoritative
  // replay of a new frame is not shown, and taking it as the previous position
  // gave the next displayed sample a backward sweep.
  if (present) Fast::Oot3d::GrassInteractionBridge::Instance().SelectSample(temporalSample);
  PinGpuCopyResources(sample);
  if (!picaPresentationScheduler.Execute(
          api, sample, renderTargetNamespace, kind,
          present, &renderError)) {
    throw std::runtime_error(
        "native PICA scheduled presentation failed: " + renderError);
  }
  const double elapsed = SecondsSince(started);
  if (present) {
    capturedTemporalSample = temporalSample;
  }
  if (xrWorld != nullptr && !present && renderTargetNamespace == kNativeDependencyRenderTargetNamespace)
    xrPerfNs1Ms += elapsed * 1000.0;
  rendererTiming->PicaBackendSeconds += elapsed;
  rendererTiming->VisualReplaySeconds += elapsed;
}

void NativeA32WindowSession::BeginXrRenderCommand(const Oot3dNativeGame::XrFrameState &state) {
  auto &api = *apiPointer;
  xrRenderState = state;
#if defined(TRIAEVUM_QUEST_OPENXR)
  if (!xrExtentsApplied) {
    // Fixed per-namespace extents (§2.4): ns1 at native density (TRIAEVUM_XR_NS1_NATIVE=0: follow the
    // eye), the eyes at the eye extent; ns2 keeps following the panel picture. No mode switch resizes.
    xrExtentsApplied = true;
    const auto eye = TriAevumQuestXr::EyeExtent();
    const char *native = std::getenv("TRIAEVUM_XR_NS1_NATIVE");
    const bool ns1Native = native == nullptr || std::string_view(native) != "0";
    api.SetNamespaceOutputExtent(kNativeDependencyRenderTargetNamespace, ns1Native ? 400U : eye.width,
                                 ns1Native ? 240U : eye.height);
    api.SetNamespaceOutputExtent(kXrEyeLeftNamespace, eye.width, eye.height);
    api.SetNamespaceOutputExtent(kXrEyeRightNamespace, eye.width, eye.height);
    api.SetNamespaceOutputExtent(kXrHudNamespace, 1280U, 720U);
    Oot3dNativeGame::XrWorldLog("TRIAEVUM_XR_NAMESPACES ns1=%ux%u ns2=picture ns3=%ux%u ns4=%ux%u ns5=1280x720",
                                ns1Native ? 400U : eye.width, ns1Native ? 240U : eye.height, eye.width,
                                eye.height, eye.width, eye.height);
  }
  // A world frame publishes only a pair (or nothing): no re-scan of an old display into the panel.
  if (state.World) api.SetXrStereoViews(Fast::StereoViews{});
#else
  (void)api;
#endif
}

bool NativeA32WindowSession::ExecuteXrEyes(const Oot3dNativeGame::Oot3dPicaVisualFrameViewSample &sample, Oot3dNativeGame::Oot3dPicaPresentationExecutionKind kind, const Fast::Oot3d::NativeFrameTemporalSample& temporalSample) {
  auto &api = *apiPointer;
  auto &xr = *xrViewRenderer;
#if defined(TRIAEVUM_QUEST_OPENXR)
  if (xr.World() && xrWorld != nullptr) {
    const auto &state = xrRenderState;
    // The render safety counts consecutive eye frames: a held trip ended (or any non-World frame)
    // restarts the count, so one bad second never ends the world mode for the session.
    if (xrWorld->TakeRenderSafetyReset()) xr.ResetRenderSafety();
    if (!state.World) {
      // Cinema / Black: the Q3 picture into the panel (namespace 2), as the screen mode.
      xr.ResetRenderSafety();
      api.SelectXrView(Fast::kXrViewPanel);
      return false;
    }
    // The eyes at the stamped target display time (§3.4 item 2), with the stamped rig (§4.5).
    Fast::StereoViews views;
    if (!TriAevumQuestXr::PredictEyes(state.TargetDisplayTimeNs, &views)) {
      xrWorld->NotePredictFailure();
      xr.EndPresentation(false);
      return true;  // nothing new this frame: the XR thread keeps the last pair, timewarped
    }
    views.PaceTick = state.PaceTick;
    views.TargetTick = state.TargetTick;
    views.RigEpoch = state.RigEpoch;
    const auto latest = TriAevumQuestXr::LatestTick();
    if (latest.Valid)
      xrWorld->NoteHorizon(static_cast<double>(state.TargetDisplayTimeNs - latest.DisplayTimeNs) * 1e-6);
    const auto camera = xr.FindFrameCamera(sample);
    const bool mirrored = camera.Valid ? camera.Mirrored : state.Mirrored;
    const auto started = std::chrono::steady_clock::now();
    (void)api.PublishPicaFrameTemporalSample(temporalSample);
    Fast::Oot3d::GrassInteractionBridge::Instance().SelectSample(temporalSample);
    PinGpuCopyResources(sample);
    for (int eye = 0; eye < 2; ++eye) {
      const auto eyeStart = std::chrono::steady_clock::now();
      const auto input = Oot3dNativeGame::XrViewRenderer::TrackedEye(state.Rig, views.Eye[eye]);
      const auto &eyeSample = xr.BuildEye(sample, camera, input, mirrored, eye);
      api.SelectXrView(eye == 0 ? Fast::kXrViewEyeLeft : Fast::kXrViewEyeRight);
      if (!picaPresentationScheduler.Execute(
              api, eyeSample, eye == 0 ? kXrEyeLeftNamespace : kXrEyeRightNamespace, kind, true,
              &renderError)) {
        throw std::runtime_error("native PICA in-world eye pass failed: " + renderError);
      }
      TriAevumQuestXr::PerfNote(eye == 0 ? TriAevumQuestXr::PerfPhase::WorkerEyeLeft
                                         : TriAevumQuestXr::PerfPhase::WorkerEyeRight,
                                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                          eyeStart)
                                    .count());
    }
    // Overlays (TopScreen HUD, ImGui) draw on the transparent panel; the pair carries it as the HUD.
    api.SelectXrView(Fast::kXrViewPanel);
    views.Valid = true;
    api.SetXrStereoViews(views);
    xrEyesDrawn = true;
    xrWorld->NoteRenderSafety(xr.EndPresentation(true));
    capturedTemporalSample = temporalSample;
    const double elapsed = SecondsSince(started);
    rendererTiming->PicaBackendSeconds += elapsed;
    rendererTiming->VisualReplaySeconds += elapsed;
    return true;
  }
#endif
  if (!xr.Preview()) return false;
  // PC preview (inworld-spec §2.7): the chosen eye executes into its own namespace and is presented on
  // the desktop swapchain (SelectXrView is a no-op there).
  const auto camera = xr.FindFrameCamera(sample);
  if (!camera.Valid) {
    xr.EndPresentation(false);
    return false;
  }
  const auto started = std::chrono::steady_clock::now();
  (void)api.PublishPicaFrameTemporalSample(temporalSample);
  Fast::Oot3d::GrassInteractionBridge::Instance().SelectSample(temporalSample);
  PinGpuCopyResources(sample);
  const int eyeIndex = xr.PreviewConfig().Eye;
  const auto& game = camera.Projection.Tangents;
  const double aspect = (game.TanRight - game.TanLeft) / std::max(1e-6, game.TanUp - game.TanDown);
  const double seconds = std::chrono::duration<double>(started - xrPreviewStart).count();
  const auto eye = xr.PreviewEye(camera, seconds, aspect);
  const auto &eyeSample = xr.BuildEye(sample, camera, eye, camera.Mirrored, eyeIndex);
  if (!picaPresentationScheduler.Execute(
          api, eyeSample, eyeIndex == 0 ? kXrEyeLeftNamespace : kXrEyeRightNamespace, kind, true,
          &renderError)) {
    throw std::runtime_error("native PICA in-world preview failed: " + renderError);
  }
  xr.EndPresentation(true);
  capturedTemporalSample = temporalSample;
  const double elapsed = SecondsSince(started);
  rendererTiming->PicaBackendSeconds += elapsed;
  rendererTiming->VisualReplaySeconds += elapsed;
  return true;
}

} // namespace Oot3dA32Window
