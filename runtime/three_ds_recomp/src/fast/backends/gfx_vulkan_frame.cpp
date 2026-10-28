// Vulkan backend: presentation settings and the frame (StartFrame, EndFrame, FinishRender).
#ifdef ENABLE_OOT3D_VULKAN
#include "gfx_vulkan_internal.h"
#include "fast/oot3d/grass_stage_profile.h"
#include "fast/renderer3ds/pica_surface_debug.h"
#include "fast/renderer3ds/pica_surface_emission.h"
#if defined(TRIAEVUM_QUEST_OPENXR)
#include <unistd.h>
#endif

namespace Fast {

bool GfxRenderingAPIVulkan::ApplyPresentationSettings(
    const Oot3d::TitlePresentationSettingsView& settings,
    std::string* error) {
    if (!mInitialized || mFrameActive || settings.WindowMode > 2U ||
        settings.Width < 320U || settings.Height < 240U) {
        if (error != nullptr) {
            *error =
                "invalid or unsafe OOT3D presentation-settings transaction";
        }
        mDiagnostics.RecordPresentationTransaction(
            settings.TransactionKind, false, true);
        return false;
    }
#if defined(TRIAEVUM_QUEST_OPENXR)
    if (mXrPresent) {
        // Immersive mode: the quad's target extent is fixed and the SDL window is hidden. The
        // request is recorded as applied without window operations or a target rebuild.
        mAppliedWindowMode = settings.WindowMode;
        mAppliedOutputWidth = settings.Width;
        mAppliedOutputHeight = settings.Height;
        mAppliedVsync = settings.VSync;
        mVsyncEnabled = settings.VSync;
        mPresentationSettingsApplied = true;
        mDiagnostics.SetPresentationState({
            true,
            mAppliedWindowMode,
            mAppliedOutputWidth,
            mAppliedOutputHeight,
            mSwapchainExtent.width,
            mSwapchainExtent.height,
            mAppliedVsync,
        });
        mDiagnostics.RecordPresentationTransaction(
            settings.TransactionKind, true, false);
        return true;
    }
#endif
    // The host may already have created the requested window. Adopt that state
    // instead of destroying a usable swapchain on the first gameplay frame.
    if (!mPresentationSettingsApplied && !mSwapchainDirty &&
        mSwapchainImages.size() >= 2U && mVsyncEnabled == settings.VSync) {
        uint32_t width = 0, height = 0;
        int32_t x = 0, y = 0;
        mWindowBackend->GetDimensions(&width, &height, &x, &y);
        const uint8_t mode = !mWindowBackend->IsFullscreen() ? 0U
            : mWindowBackend->IsWindowedFullscreen() ? 1U : 2U;
        if (mode == settings.WindowMode && width == settings.Width &&
            height == settings.Height) {
            mAppliedWindowMode = mode;
            mAppliedOutputWidth = width;
            mAppliedOutputHeight = height;
            mAppliedVsync = settings.VSync;
            mPresentationSettingsApplied = true;
        }
    }
    if (mPresentationSettingsApplied &&
        mAppliedWindowMode == settings.WindowMode &&
        mAppliedOutputWidth == settings.Width &&
        mAppliedOutputHeight == settings.Height &&
        mAppliedVsync == settings.VSync) {
        mDiagnostics.SetPresentationState({
            true,
            mAppliedWindowMode,
            mAppliedOutputWidth,
            mAppliedOutputHeight,
            mSwapchainExtent.width,
            mSwapchainExtent.height,
            mAppliedVsync,
        });
        mDiagnostics.RecordPresentationTransaction(
            settings.TransactionKind, true, false);
        return true;
    }

    Oot3d::TitlePresentationSettingsView previous;
    if (mPresentationSettingsApplied) {
        previous = {
            mAppliedWindowMode,
            mAppliedOutputWidth,
            mAppliedOutputHeight,
            mAppliedVsync,
        };
    } else {
        int32_t x = 0;
        int32_t y = 0;
        mWindowBackend->GetDimensions(
            &previous.Width, &previous.Height, &x, &y);
        const bool borderless =
            mWindowBackend->IsWindowedFullscreen();
        previous.WindowMode = !mWindowBackend->IsFullscreen()
            ? 0U
            : borderless ? 1U : 2U;
        previous.VSync = mVsyncEnabled;
    }

    const auto apply = [&](const Oot3d::TitlePresentationSettingsView& value,
                           bool allowInjectedFailure) {
        const bool fullscreen = value.WindowMode != 0U;
        const bool borderless = value.WindowMode == 1U;
        const bool currentlyBorderless =
            mWindowBackend->IsWindowedFullscreen();
        const uint8_t currentMode = !mWindowBackend->IsFullscreen()
            ? 0U
            : currentlyBorderless ? 1U : 2U;
        const bool fullscreenFlavorChanged =
            fullscreen && currentMode != 0U &&
            currentMode != value.WindowMode;
        const bool exclusiveModeChanged =
            currentMode == 2U && value.WindowMode == 2U;
        const bool leavingFullscreen =
            !fullscreen && currentMode != 0U;
        if (fullscreenFlavorChanged || exclusiveModeChanged ||
            leavingFullscreen) {
            mWindowBackend->SetFullscreen(false);
        }

        int32_t x = 0;
        int32_t y = 0;
        uint32_t oldWidth = 0;
        uint32_t oldHeight = 0;
        mWindowBackend->GetDimensions(
            &oldWidth, &oldHeight, &x, &y);
        mWindowBackend->SetWindowedFullscreen(borderless);
        if (!fullscreen || !borderless) {
            mWindowBackend->SetDimensions(
                value.Width, value.Height, x, y);
        }
        if (value.WindowMode == 2U &&
            !mWindowBackend->SetExclusiveFullscreenDisplayMode(
                value.Width, value.Height)) {
            throw std::runtime_error(
                "SDL rejected the requested exclusive display mode");
        }
        if (mWindowBackend->IsFullscreen() != fullscreen) {
            mWindowBackend->SetFullscreen(fullscreen);
        } else if (fullscreenFlavorChanged || exclusiveModeChanged) {
            mWindowBackend->SetFullscreen(true);
        }
        if (mWindowBackend->IsFullscreen() != fullscreen) {
            throw std::runtime_error(
                "SDL rejected the requested fullscreen mode");
        }
        if (fullscreen &&
            mWindowBackend->IsWindowedFullscreen() != borderless) {
            throw std::runtime_error(
                "SDL rejected the requested fullscreen flavor");
        }
        mVsyncEnabled = value.VSync;
        mSwapchainDirty = true;
        RecreateSwapchain();
        if (mSwapchainDirty || mSwapchainImages.size() < 2U) {
            throw std::runtime_error(
                "swapchain recreation did not produce usable images");
        }
        if (allowInjectedFailure) {
            const char* inject = std::getenv(
                "OOT3D_GRAPHICS_TEST_FAIL_PRESENTATION_APPLY");
            if (inject != nullptr &&
                std::string_view(inject) == "1") {
                throw std::runtime_error(
                    "injected presentation transaction failure");
            }
        }
    };

    WaitForAllPresents();
    CheckVk(vkDeviceWaitIdle(mDevice),
            "vkDeviceWaitIdle(OOT3D presentation settings)");
    try {
        apply(settings, mPresentationSettingsApplied);
    } catch (const std::exception& applyException) {
        try {
            apply(previous, false);
        } catch (const std::exception& rollbackException) {
            throw std::runtime_error(
                std::string("OOT3D presentation apply failed: ") +
                applyException.what() +
                "; rollback failed: " + rollbackException.what());
        }
        mAppliedWindowMode = previous.WindowMode;
        mAppliedOutputWidth = previous.Width;
        mAppliedOutputHeight = previous.Height;
        mAppliedVsync = previous.VSync;
        mPresentationSettingsApplied = true;
        mDiagnostics.SetPresentationState({
            true,
            mAppliedWindowMode,
            mAppliedOutputWidth,
            mAppliedOutputHeight,
            mSwapchainExtent.width,
            mSwapchainExtent.height,
            mAppliedVsync,
        });
        mDiagnostics.RecordPresentationTransaction(
            settings.TransactionKind, false, true);
        if (error != nullptr) {
            *error = std::string(applyException.what()) +
                "; previous presentation settings restored";
        }
        return false;
    }

    mAppliedWindowMode = settings.WindowMode;
    mAppliedOutputWidth = settings.Width;
    mAppliedOutputHeight = settings.Height;
    mAppliedVsync = settings.VSync;
    mPresentationSettingsApplied = true;
    mDiagnostics.SetPresentationState({
        true,
        mAppliedWindowMode,
        mAppliedOutputWidth,
        mAppliedOutputHeight,
        mSwapchainExtent.width,
        mSwapchainExtent.height,
        mAppliedVsync,
    });
    mDiagnostics.RecordPresentationTransaction(
        settings.TransactionKind, true, false);
    return true;
}

void GfxRenderingAPIVulkan::StartFrame() {
    Renderer3ds::SlowStepScope slowStep("start_frame");
    const char* traceEnvironment = std::getenv("TRIAEVUM_FRAME_START_TIMING");
    const bool traceStart = traceEnvironment != nullptr && std::string_view(traceEnvironment) == "1";
    auto startMark = traceStart ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::array<double, 7> startTimes{};
    const auto markStart = [&](size_t index) {
        if (!traceStart) return;
        const auto now = std::chrono::steady_clock::now();
        startTimes[index] = std::chrono::duration<double, std::milli>(now - startMark).count();
        startMark = now;
    };
    if (!mInitialized || mFrameActive || mFrameSubmitted) {
        return;
    }
#if defined(TRIAEVUM_QUEST_OPENXR)
    // In-world: the whole StartFrame for TRIAEVUM_XR_PERF (worker start=), early returns included.
    struct XrStartTimer {
        bool Enabled;
        std::chrono::steady_clock::time_point Start = std::chrono::steady_clock::now();
        ~XrStartTimer() {
            if (Enabled)
                TriAevumQuestXr::PerfNote(TriAevumQuestXr::PerfPhase::WorkerStart,
                                          std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - Start)
                                              .count());
        }
    } xrStartTimer{mXrWorld};
#endif
    // Guest completion survives presentations. A visual replay, however, must
    // capture this sample's source image again instead of reusing alpha zero.
    std::erase_if(mHandledNativePicaTextureCopies,
                  [](const auto& entry) { return !entry.second.second; });
    auto& settingsRuntime = Oot3d::GraphicsSettingsRuntime::Instance();
    Oot3d::TickAmbientOcclusionDiagnostics(settingsRuntime, mFrameCounter);
    Oot3d::TickDisplayDiagnostics(settingsRuntime, mFrameCounter);
    if (!mPresentationSettingsHeld) settingsRuntime.TickPresentation();
    const auto initialGraphicsSettings = settingsRuntime.Snapshot();
    const auto presentationRequest =
        Oot3d::ResolveRendererPresentationRequest(
            initialGraphicsSettings,
            settingsRuntime.PresentationStatus(),
            mPresentationSettingsApplied);
    if (presentationRequest.has_value() && !mPresentationSettingsHeld) {
        const auto& value = presentationRequest->Value;
        const Oot3d::TitlePresentationSettingsView view{
            static_cast<uint8_t>(value.Window),
            value.Width,
            value.Height,
            value.VSync,
            static_cast<uint8_t>(presentationRequest->Kind),
        };
        std::string presentationError;
        if (ApplyPresentationSettings(view, &presentationError)) {
            if (presentationRequest->RequiresAcknowledgement() &&
                !settingsRuntime.AcknowledgePresentationApplied(
                    initialGraphicsSettings)) {
                SPDLOG_ERROR(
                    "OOT3D renderer applied display settings but the "
                    "transaction could not be acknowledged");
            }
        } else {
            if (presentationRequest->Kind ==
                Oot3d::RendererPresentationRequestKind::Candidate) {
                settingsRuntime.RejectPresentationApply(
                    initialGraphicsSettings, presentationError);
            }
            SPDLOG_ERROR("OOT3D display settings were not applied: {}",
                         presentationError);
        }
    }
    mAmbientOcclusionExecutedThisFrame = false;
    mSceneDistanceFieldUpdatedThisFrame = false;
    mHiZExecutedThisFrame = false;
    mReflectionExecutedThisFrame = false;
    mReflectionProviderThisFrame =
        Oot3d::ReflectionProvider::Off;
    mReflectionOutputEncodingThisFrame =
        Oot3d::SceneColorEncoding::Unknown;
    mGrassExecutedThisFrame = false;
    mLinearColorExecutedThisFrame = false;
    Renderer3ds::PicaSurfaceEmissionRegistry::Instance().BeginFrame();
    mUpscalerExecutedThisFrame = false;
    mNativeUiOverlayExecutedThisFrame = false;
    mUpscalerUsedD3d12ThisFrame = false;
    mUpscalerOutputEncodingThisFrame =
        Oot3d::SceneColorEncoding::Unknown;
    mUpscalerEffectsCompositedThisFrame = false;
    mCompositeExecutedThisFrame = false;
    mCompositeOutputEncodingThisFrame =
        Oot3d::SceneColorEncoding::Unknown;
    mTaaExecutedThisFrame = false;
    mTaaOutputEncodingThisFrame =
        Oot3d::SceneColorEncoding::Unknown;
    mTaaEffectsCompositedThisFrame = false;
    mSmaaExecutedThisFrame = false;
    mSmaaEffectsCompositedThisFrame = false;
    mSmaaOutputEncodingThisFrame =
        Oot3d::SceneColorEncoding::Unknown;
    mSmaaRenderTargetNamespace = 0;
    mSmaaDisplayPhysicalAddress = 0;
    mSmaaWidth = 0;
    mSmaaHeight = 0;
    mGrassRenderedTargetsThisFrame.clear();
    mGrassInsertionAttemptedTargetsThisFrame.clear();
    mGrassWorldTargetsThisFrame.clear();
    mDirectionalShadowInsertionAttemptedTargetsThisFrame.clear();
    mPointShadowInsertionAttemptedTargetsThisFrame.clear();
    mGrassSurfaceOccurrencesThisFrame.clear();
    mNativePicaDepthWritingDrawsThisFrame = 0;
    mCustomTextureUploadBytesThisFrame = 0;
    if (mFrameCounter > 3U) {
        Oot3d::GrassSceneBridge::Instance().PruneBeforeFrame(mFrameCounter - 2U);
    }
    settingsRuntime.SetCapability(
        Oot3d::GraphicsCapability::ValidViewMetadata,
        Oot3d::SceneViewRuntime::Instance().LatestPerspective().has_value(),
        "No valid perspective frustum has been published by the game runtime");
    if (const char* grassSmoke = std::getenv("OOT3D_GRAPHICS_GRASS_AUTO");
        grassSmoke != nullptr && std::string_view(grassSmoke) == "1") {
        auto candidate = settingsRuntime.Snapshot();
        if (candidate.Grass.Rules.empty()) {
            const auto selectors =
                Oot3d::GrassSceneBridge::Instance().AvailableTextures();
            static bool grassSmokeSourceReported = false;
            if (!selectors.empty() && !grassSmokeSourceReported) {
                std::fprintf(stderr,
                    "OOT3D grass smoke: %zu source meshes, %zu texture selectors\n",
                    Oot3d::GrassSceneBridge::Instance().Size(), selectors.size());
                grassSmokeSourceReported = true;
            }
            uint64_t requestedHash = 0U;
            if (const char* hash = std::getenv("OOT3D_GRAPHICS_GRASS_HASH");
                hash != nullptr) {
                char* end = nullptr;
                requestedHash = std::strtoull(hash, &end, 16);
                if (end == hash || *end != '\0') requestedHash = 0U;
            }
            size_t addedRules = 0U;
            for (size_t index = 0;
                 index < selectors.size() && addedRules < 128U; ++index) {
                if (requestedHash != 0U &&
                    selectors[index].Rgba8Hash != requestedHash) continue;
                Oot3d::GrassPlacementRule rule;
                rule.RuleId = static_cast<uint32_t>(addedRules + 1U);
                rule.Target = selectors[index];
                rule.InputBlack = 0.0F;
                candidate.Grass.Rules.push_back(rule);
                ++addedRules;
            }
            if (!candidate.Grass.Rules.empty()) {
                candidate.Preset = Oot3d::GraphicsPreset::Custom;
                candidate.Grass.Quality = Oot3d::GrassQuality::Medium;
                candidate.Grass.Generation.InstancesPerSquareMeter =
                    3.0F;
                candidate.Grass.MaxInstancesPerRoom = 30000U;
                candidate.Grass.DrawDistance = 1200.0F;
                candidate.Grass.WindDirectionDegrees = 35.0F;
                candidate.Grass.WindStrength = 0.5F;
                candidate.Grass.WindSpeed = 1.2F;
                settingsRuntime.Apply(std::move(candidate));
            }
        }
    }
    auto graphicsSnapshot = settingsRuntime.SnapshotForRendering();
#if defined(TRIAEVUM_QUEST_OPENXR)
    if (mXrWorld) {
        // In-world (W1): the passes that build their camera from the guest perspective and the game's
        // WorldToView (interactive grass, Hi-Z/SSR and water reflections, AO, screen-space GI, temporal
        // motion/TAA) would be misregistered in the eye namespaces, whose rows are the eye's. Off for
        // this frame's rendering (the settings stay as they are); per-eye cameras are a later milestone.
        auto& value = graphicsSnapshot.Value;
        auto& effects = value.Effects;
        uint32_t forced = 0;
        const auto force = [&forced](bool on, uint32_t bit) { forced |= on ? bit : 0U; };
        force(value.Grass.Quality != Oot3d::GrassQuality::Off, 1U << 0);
        force(effects.Reflections != Oot3d::ReflectionMode::Off, 1U << 1);
        force(effects.WaterReflections && Oot3d::UsesFullSceneLighting(effects.SurfaceLighting), 1U << 2);
        force(effects.AmbientOcclusion != Oot3d::AmbientOcclusionMode::Off, 1U << 3);
        force(effects.GlobalIllumination.Mode != Oot3d::GlobalIlluminationMode::Off, 1U << 4);
        const bool temporalAa = value.AntiAliasing == Oot3d::AntiAliasingMode::Taa ||
            (value.AntiAliasing == Oot3d::AntiAliasingMode::Upscaler && Oot3d::IsTemporalUpscaler(value.Upscaler));
        force(temporalAa, 1U << 5);
        value.Grass.Quality = Oot3d::GrassQuality::Off;
        effects.Reflections = Oot3d::ReflectionMode::Off;
        effects.WaterReflections = false;
        effects.AmbientOcclusion = Oot3d::AmbientOcclusionMode::Off;
        effects.GlobalIllumination.Mode = Oot3d::GlobalIlluminationMode::Off;
        if (temporalAa) value.AntiAliasing = Oot3d::AntiAliasingMode::Off;
        if (forced != mXrForcedEffects) {
            mXrForcedEffects = forced;
            static constexpr const char* kNames[] = {"grass", "reflections", "water_reflections", "ao", "gi",
                                                     "temporal_aa"};
            std::string names;
            for (uint32_t bit = 0; bit < 6U; ++bit) {
                if ((forced & (1U << bit)) == 0U) continue;
                if (!names.empty()) names += ',';
                names += kNames[bit];
            }
            const std::string line = "TRIAEVUM_XR_VIEW effects_forced_off=" + (names.empty() ? std::string("none") : names);
            TriAevumQuestXr::LogLine(line.c_str());
        }
    }
#endif
    // Water reflections with Reflections Off (EffectsSettings::WaterReflections):
    // the frame traces Hi-Z, and only water writes a reflection material.
    {
        auto& effects = graphicsSnapshot.Value.Effects;
        const auto capabilities = settingsRuntime.Capabilities();
        mWaterOnlyReflections = effects.Reflections == Oot3d::ReflectionMode::Off && effects.WaterReflections &&
            Oot3d::UsesFullSceneLighting(effects.SurfaceLighting) &&
            capabilities.Has(Oot3d::GraphicsCapability::NriInterop) &&
            capabilities.Has(Oot3d::GraphicsCapability::ValidViewMetadata);
        if (mWaterOnlyReflections) {
            effects.Reflections = Oot3d::ReflectionMode::HiZ;
            effects.ReflectionMaterials.clear();
            // The water's reflectance weighs the trace; open water reaches far.
            effects.ReflectionStrength = 1.0F;
            effects.ReflectionMaxDistance = std::max(effects.ReflectionMaxDistance, 3000.0F);
        }
    }
#if defined(TRIAEVUM_RENDERER_DEVELOPER_TOOLS)
    Renderer3ds::PicaSurfaceDebug::Instance().BeginFrame();
#endif
    const auto& nextGraphicsSettings = graphicsSnapshot.Value;
    if (mFrameEnvironment != graphicsSnapshot.Environment) {
      if (!Renderer3ds::ContinuousPicaEnvironmentChange(mFrameEnvironment, graphicsSnapshot.Environment)) {
        mTemporalAaPass.ResetHistory();
        mTaaOutputValid = false;
        // Never use a previous scene/sun's depth map for a new lighting direction.
        if (mFrameEnvironment.ContinuityEpoch != graphicsSnapshot.Environment.ContinuityEpoch ||
            mFrameEnvironment.ScopeIdentity != graphicsSnapshot.Environment.ScopeIdentity ||
            mFrameEnvironment.Enabled != graphicsSnapshot.Environment.Enabled ||
            mFrameEnvironment.KeyDirection != graphicsSnapshot.Environment.KeyDirection ||
            mFrameEnvironment.KeyAzimuthOffset != graphicsSnapshot.Environment.KeyAzimuthOffset ||
            mFrameEnvironment.KeyElevationOffset != graphicsSnapshot.Environment.KeyElevationOffset ||
            mFrameEnvironment.CasterFaces != graphicsSnapshot.Environment.CasterFaces ||
            mFrameEnvironment.Lights[0].DirectionTowardSource !=
                graphicsSnapshot.Environment.Lights[0].DirectionTowardSource)
            mNriDirectionalShadowPass.InvalidateHistory();
      }
        // Continuous profile motion uses VirtualShadowKeyframes' existing
        // page budget/dissolve, just like the moving native sun. Do not reset
        // its construction (or TAA history) for every interpolated sample.
        mFrameEnvironment = graphicsSnapshot.Environment;
    }
    if (mFrameGraphicsSettings.Effects.DirectionalShadows.Mode != nextGraphicsSettings.Effects.DirectionalShadows.Mode ||
        mFrameGraphicsSettings.Effects.PointShadows.Mode != nextGraphicsSettings.Effects.PointShadows.Mode ||
        mFrameGraphicsSettings.Effects.SurfaceLighting != nextGraphicsSettings.Effects.SurfaceLighting)
        mNriDirectionalShadowPass.InvalidateHistory();
    if (mFrameGraphicsSettingsRevision != 0 &&
        (mFrameGraphicsSettings.Grass.Quality != nextGraphicsSettings.Grass.Quality ||
         mFrameGraphicsSettings.Effects.Toon != nextGraphicsSettings.Effects.Toon ||
         mFrameGraphicsSettings.Effects.SurfaceLighting != nextGraphicsSettings.Effects.SurfaceLighting ||
         mFrameGraphicsSettings.Effects.ToonStyle.OutlineEnabled != nextGraphicsSettings.Effects.ToonStyle.OutlineEnabled ||
         mFrameGraphicsSettings.Effects.AmbientOcclusion != nextGraphicsSettings.Effects.AmbientOcclusion ||
         mFrameGraphicsSettings.Effects.Reflections != nextGraphicsSettings.Effects.Reflections)) {
        // Never blend the old enhanced/native presentation into the newly selected one.
        mTemporalAaPass.ResetHistory();
        mTaaOutputValid = false;
    }
    mFrameGraphicsSettings = std::move(graphicsSnapshot.Value);
    mFrameGraphicsSettingsRevision = graphicsSnapshot.Revision;
    const auto& graphicsSettings = mFrameGraphicsSettings;
    mPicaShaderPipelineCache.BeginFrame(
        mFrameGraphicsSettingsRevision);
    auto& azaharTexturePacks =
        ::Oot3d::Renderer::AzaharTexturePackRuntime::Instance();
    if (mAzaharConfiguredSettingsRevision !=
        mFrameGraphicsSettingsRevision) {
        azaharTexturePacks.Configure({
            .DumpTextures =
                graphicsSettings.TexturePacks.Azahar.DumpTextures,
            .LoadCustomTextures =
                graphicsSettings.TexturePacks.Azahar
                    .LoadCustomTextures,
            .LoadDirectory =
                graphicsSettings.TexturePacks.Azahar.LoadDirectory,
            .DumpDirectory =
                graphicsSettings.TexturePacks.Azahar.DumpDirectory,
        });
        mAzaharConfiguredSettingsRevision =
            mFrameGraphicsSettingsRevision;
    }
    mFrameAzaharTextureGeneration =
        azaharTexturePacks.Generation();
    const auto materialRevision = azaharTexturePacks.MaterialRevision();
    if (mFrameMaterialRevision != materialRevision &&
        Oot3d::UsesPbrMaterials(graphicsSettings.Effects.SurfaceLighting)) {
        mTemporalAaPass.ResetHistory();
        mTaaOutputValid = false;
    }
    mFrameMaterialRevision = materialRevision;
    const char* taaEnvironment = std::getenv("OOT3D_GRAPHICS_TAA");
    const char* motionEnvironment = std::getenv("OOT3D_GRAPHICS_MOTION");
    const Oot3d::AntiAliasingFrameCapabilities aaCapabilities{
        mNativePicaDepthResolveMode != VK_RESOLVE_MODE_NONE,
        (mNativePicaSupportedSampleCounts & VK_SAMPLE_COUNT_2_BIT) != 0U,
        (mNativePicaSupportedSampleCounts & VK_SAMPLE_COUNT_4_BIT) != 0U,
        (mNativePicaSupportedSampleCounts & VK_SAMPLE_COUNT_8_BIT) != 0U};
    const auto aaPolicy = Oot3d::ResolveAntiAliasingFramePolicy(
        graphicsSettings, aaCapabilities,
        taaEnvironment != nullptr &&
            std::string_view(taaEnvironment) == "1",
        motionEnvironment != nullptr &&
            std::string_view(motionEnvironment) == "1");
    mNativeFidelityProfile =
        graphicsSettings.Preset == Oot3d::GraphicsPreset::Authentic;
    mTemporalJitterEnabled =
        !mNativeFidelityProfile && aaPolicy.TemporalJitter;
    mPresentationWorldSurface =
        !mNativeFidelityProfile && aaPolicy.PresentationWorldSurface;
    mSceneAntiAliasing = !mNativeFidelityProfile && aaPolicy.SceneAntiAliasing;
    mPresentationUpscaler = !mNativeFidelityProfile && aaPolicy.PresentationUpscaler &&
                            mNativeUiOverlayPass.Available();
    // The rendering snapshot already disables GI outside per-pixel lighting
    // and under native presentation.
    const bool globalIlluminationRequested =
        graphicsSettings.Effects.GlobalIllumination.Mode != Oot3d::GlobalIlluminationMode::Off &&
        !mNativeFidelityProfile;
    const bool screenSpaceGlobalIlluminationRequested =
        globalIlluminationRequested &&
        graphicsSettings.Effects.GlobalIllumination.Mode == Oot3d::GlobalIlluminationMode::ScreenSpace;
    // GI and temporal MXAO reproject their history with the scene's motion
    // vectors.
    const bool temporalAmbientOcclusionRequested =
        graphicsSettings.Effects.AmbientOcclusion == Oot3d::AmbientOcclusionMode::Mxao &&
        graphicsSettings.Effects.MxaoTemporal;
    mTemporalMotionEnabled =
        !mNativeFidelityProfile &&
        (aaPolicy.TemporalMotion || globalIlluminationRequested || temporalAmbientOcclusionRequested);
    const Oot3d::PicaAttachmentFeatureRequests extensionRequests{
        graphicsSettings.Effects.DirectionalShadows.Mode ==
            Oot3d::DirectionalShadowMode::VirtualShadowMaps,
        graphicsSettings.Effects.AmbientOcclusion != Oot3d::AmbientOcclusionMode::Off,
        graphicsSettings.Effects.Toon != Oot3d::ToonMode::Off &&
            graphicsSettings.Effects.ToonStyle.OutlineEnabled,
        graphicsSettings.Effects.Reflections !=
            Oot3d::ReflectionMode::Off,
        mTemporalMotionEnabled,
        graphicsSettings.Grass.Quality != Oot3d::GrassQuality::Off,
        globalIlluminationRequested,
        graphicsSettings.Effects.PointShadows.Mode == Oot3d::PointShadowMode::CubeMaps,
        Oot3d::EffectsReadFaceNormal(graphicsSettings.Effects) ||
            (screenSpaceGlobalIlluminationRequested &&
             Oot3d::GlobalIlluminationReadsFaceNormal(graphicsSettings.Effects.GlobalIllumination)),
        screenSpaceGlobalIlluminationRequested,
        graphicsSettings.Effects.DirectionalShadows.Mode == Oot3d::DirectionalShadowMode::VirtualShadowMaps &&
            graphicsSettings.Effects.DirectionalShadows.ContactShadows.Enabled,
    };
    if (!mPicaExtensionRequests.has_value() ||
        *mPicaExtensionRequests != extensionRequests) {
        auto graph = Oot3d::BuildPicaExtensionGraph(extensionRequests);
        if (!graph.Valid()) {
            throw std::runtime_error(
                "PICA extension graph is invalid: " + graph.Error);
        }
        auto directionalShadowMapBarriers =
            Oot3d::BuildEffectPassBarrierPlan(
                graph,
                Oot3d::PicaExtensionPassName(
                    Oot3d::PicaExtensionPass::DirectionalShadowMap));
        auto directionalShadowSchedule =
            Oot3d::BuildPicaEffectPassSchedulePlan(
                graph,
                Oot3d::PicaExtensionPassName(
                    Oot3d::PicaExtensionPass::DirectionalShadowMap),
                Oot3d::EffectContractKind::AuxiliaryOutput);
        auto pointShadowMapBarriers = Oot3d::BuildEffectPassBarrierPlan(
            graph, Oot3d::PicaExtensionPassName(Oot3d::PicaExtensionPass::PointShadowMap));
        auto pointShadowSchedule = Oot3d::BuildPicaEffectPassSchedulePlan(
            graph, Oot3d::PicaExtensionPassName(Oot3d::PicaExtensionPass::PointShadowMap),
            Oot3d::EffectContractKind::AuxiliaryOutput);
        if (extensionRequests.PointShadows &&
            (!pointShadowMapBarriers.Valid() || !pointShadowSchedule.Valid() ||
             !Oot3d::PicaExtensionPassEnabled(graph, Oot3d::PicaExtensionPass::PointShadowLighting))) {
            throw std::runtime_error("PICA extension graph did not compile point shadow contracts");
        }
        auto interactiveGrassProvider =
            Oot3d::BuildEffectGeometryProviderPlan(
                graph,
                Oot3d::PicaExtensionPassName(
                    Oot3d::PicaExtensionPass::InteractiveGrass));
        if (extensionRequests.DirectionalShadows &&
            (!directionalShadowMapBarriers.Valid() ||
             !directionalShadowSchedule.Valid() ||
             !Oot3d::PicaExtensionPassEnabled(
                 graph,
                 Oot3d::PicaExtensionPass::DirectionalShadowLighting))) {
            throw std::runtime_error(
                "PICA extension graph did not compile directional shadow contracts");
        }
        if (extensionRequests.InteractiveGrass &&
            !interactiveGrassProvider.Valid()) {
            throw std::runtime_error(
                "PICA extension graph did not compile the interactive grass geometry-provider contract");
        }
        // Disabling the field (GI off) releases its world: re-enabling it
        // later must not trace geometry of a world seen before.
        if (Oot3d::PicaExtensionPassEnabled(mPicaExtensionGraph, Oot3d::PicaExtensionPass::SceneDistanceField) &&
            !Oot3d::PicaExtensionPassEnabled(graph, Oot3d::PicaExtensionPass::SceneDistanceField)) {
            mBrixelizerGiPass.ResetHistory();
            mBrixelizerScene.Reset();
        }
        mPicaExtensionRequests = extensionRequests;
        mPicaExtensionGraph = std::move(graph);
        mInteractiveGrassProviderPlan =
            std::move(interactiveGrassProvider);
        mDirectionalShadowMapBarrierPlan =
            std::move(directionalShadowMapBarriers);
        mDirectionalShadowSchedulePlan =
            std::move(directionalShadowSchedule);
        mPointShadowMapBarrierPlan = std::move(pointShadowMapBarriers);
        mPointShadowSchedulePlan = std::move(pointShadowSchedule);
    }
    mInteractiveGrassProviderExecution.Reset(
        mInteractiveGrassProviderPlan);
    if (mFramePicaAttachmentRequirements != mPicaExtensionGraph.Attachments) {
        // Effect activation is not a native framebuffer invalidation. Retain
        // color/depth, transfer snapshots and cached framebuffer variants.
        for (auto& [key, target] : mNativePicaRenderTargets) {
            target.AuxiliaryNeedsClear = true;
        }
    }
    mFramePicaAttachmentRequirements = mPicaExtensionGraph.Attachments;
    if (mNativeFidelityProfile &&
        !mFramePicaAttachmentRequirements.NativeColorOnly()) {
        throw std::runtime_error(
            "Native Fidelity requested auxiliary PICA attachments");
    }
    mTemporalJitterPixels = mTemporalJitterEnabled
        ? Oot3d::TemporalJitterForFrame(
              mTemporalJitterSequence,
              aaPolicy.TemporalJitterPhases).PixelOffset
        : std::array<float, 2>{};
    if (!mTemporalMotionEnabled) {
        mPicaTemporalHistory.Reset();
    }
    if (!mTemporalJitterEnabled) {
        mTaaOutputValid = false;
        mTemporalAaPass.ResetHistory();
    }
    if (graphicsSettings.Effects.AmbientOcclusion == Oot3d::AmbientOcclusionMode::Off) {
        // The image may remain allocated for cheap re-enabling, but it must no
        // longer be eligible for scanout composition as soon as AO is Off.
        mAmbientOcclusionOutputValid = false;
    }
    const float requestedInternalResolutionScale =
        Oot3d::ResolveInternalResolutionScale(graphicsSettings);
    mSpatialAaMode = aaPolicy.SpatialAaMode;
    if (mVsyncEnabled != graphicsSettings.VSync) {
        mVsyncEnabled = graphicsSettings.VSync;
#if defined(TRIAEVUM_QUEST_OPENXR)
        // No swapchain in immersive mode: the compositor paces the quad.
        if (!mXrPresent)
#endif
        mSwapchainDirty = true;
    }
    markStart(0);
    WaitForFramePresent(mCurrentFrame);
    markStart(1);
    const VkSampleCountFlagBits requestedSampleCount =
        aaPolicy.MsaaSamples >= 8U ? VK_SAMPLE_COUNT_8_BIT
        : aaPolicy.MsaaSamples >= 4U ? VK_SAMPLE_COUNT_4_BIT
        : aaPolicy.MsaaSamples >= 2U ? VK_SAMPLE_COUNT_2_BIT
                                    : VK_SAMPLE_COUNT_1_BIT;
    if (requestedSampleCount != mNativePicaSampleCount) {
        ApplyNativePicaSampleCount(requestedSampleCount);
    }
#if defined(__ANDROID__)
    // A lost or dropped Android surface (the Activity in the background) is a pause: no active
    // frame, so the guest is held. Before the suboptimal check, which would query the surface.
    if (!EnsureAndroidSurface()) {
        return;
    }
#endif
    if (mPresentSwapchainDirty.exchange(false)) {
        mSwapchainDirty = true;
    }
    if (mSwapchainSuboptimal.exchange(false) && !mSwapchainDirty) {
        mSwapchainDirty = SwapchainSurfaceChanged();
    }
    const auto presentError = static_cast<VkResult>(mPresentError.exchange(VK_SUCCESS));
    if (presentError != VK_SUCCESS) {
        CheckVk(presentError, "asynchronous vkQueuePresentKHR");
    }
    if (std::abs(requestedInternalResolutionScale -
                 mInternalResolutionScale) > 0.0005F) {
        ApplyInternalResolutionScale(requestedInternalResolutionScale);
    }
    if (mSwapchainDirty) {
        RecreateSwapchain();
        if (mSwapchainDirty) {
            return;
        }
    }
    PrewarmNativePicaPipelines();
    markStart(2);

#if defined(TRIAEVUM_QUEST_OPENXR)
    const auto fenceStart = std::chrono::steady_clock::now();
#endif
    CheckVk(vkWaitForFences(mDevice, 1, &mInFlightFences[mCurrentFrame], VK_TRUE,
                            std::numeric_limits<uint64_t>::max()),
            "vkWaitForFences");
#if defined(TRIAEVUM_QUEST_OPENXR)
    if (mXrWorld)
        TriAevumQuestXr::PerfNote(
            TriAevumQuestXr::PerfPhase::WorkerFence,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fenceStart).count());
#endif
    markStart(3);
    // A fence handle is reused for another submission below. Retire all image
    // references to its completed submission before resetting it; otherwise a
    // later acquisition can wait for unrelated work using the same handle.
    std::replace(mImagesInFlight.begin(), mImagesInFlight.end(),
                 mInFlightFences[mCurrentFrame], VkFence{VK_NULL_HANDLE});
    mScanoutProbe.Consume(mCurrentFrame);
    ReleaseRetiredNativePicaGeometryBuffers(mCurrentFrame);
    ReleaseRetiredNativePicaDisplayImages(mCurrentFrame);
    RetireGpuTextureCopyProducers(mGpuCopySubmittedFrames[mCurrentFrame]);
    auto& frame = mFrameResources[mCurrentFrame];
    for (auto& image : frame.RetiredTextures) DestroyTexture(image);
    frame.RetiredTextures.clear();
    for (auto& buffer : frame.RetiredUniformBuffers) DestroyBuffer(buffer);
    frame.RetiredUniformBuffers.clear();
    for (auto material = mNativePicaMaterials.begin(); material != mNativePicaMaterials.end();) {
        if (mFrameCounter <= material->second.LastUsedFrame + 240U) {
            ++material;
            continue;
        }
        RetireNativePicaMaterial(material->second.Active);
        RetireNativePicaMaterial(material->second.Pending);
        material = mNativePicaMaterials.erase(material);
    }
    // Retired into this slot, after its completed work was released: the
    // images are destroyed only once this frame's own submission completed.
    if (mNativePicaTexturesGeneration != mFrameAzaharTextureGeneration)
        RetireNativePicaTexturesOfOtherGenerations(mFrameAzaharTextureGeneration);
    ReleaseIdleNativePicaTextures();
    mCompletedNativePicaIds.insert(mCompletedNativePicaIds.end(),
                                   frame.NativePicaCompletionIds.begin(),
                                   frame.NativePicaCompletionIds.end());
    frame.NativePicaCompletionIds.clear();
    markStart(4);
    bool nriAcquire = false;
    VkResult acquire = VK_NOT_READY;
    bool xrPresent = false;
#if defined(TRIAEVUM_QUEST_OPENXR)
    uint64_t xrCopyWaitValue = 0;
    if (mXrPresent) {
        xrPresent = true;
        // By thread, not once: the first frames can run on the game thread (the inline stream before the
        // render worker starts), which RegisterThread ignores, and the worker must still get its hint.
        if (const int tid = static_cast<int>(gettid()); tid != mXrRendererThreadTid) {
            TriAevumQuestXr::RegisterThread(TriAevumQuestXr::ThreadRole::RendererWorker, tid);
            mXrRendererThreadTid = tid;
        }
        if (!TriAevumQuestXr::RendererMayRun()) {
            return;  // session not focused (or stopping): no active frame, the guest is held
        }
        if (mXrWorld) {
            // Every image of the slot (panel, eyes): its last copy must be done; the GPU wait
            // below takes the latest copy value. (With the in-frame copy the pair's copies are in the
            // slot's own last submission, which the frame fence above covered.)
            const auto targetStart = std::chrono::steady_clock::now();
            for (uint32_t view = 0; view < kXrViewCount; ++view)
                xrCopyWaitValue = std::max(
                    xrCopyWaitValue, TriAevumQuestXr::WaitTargetFree(mCurrentFrame * kXrViewCount + view));
            TriAevumQuestXr::PerfNote(
                TriAevumQuestXr::PerfPhase::WorkerTargetFree,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - targetStart).count());
            BeginXrViewFrame();
        } else {
        // The slot's private target: its last copy into the quad must be done (normally long
        // since), and an unshown publication of it is dropped.
        xrCopyWaitValue = TriAevumQuestXr::WaitTargetFree(mCurrentFrame);
        mCurrentImage = mCurrentFrame;
        }
        acquire = VK_SUCCESS;
    }
#endif
    if (mBenchmarkNoPresent) {
        // Private targets, one per frame in flight, guarded by the frame fence.
        mCurrentImage = mCurrentFrame;
        acquire = VK_SUCCESS;
    }
    for (; !mBenchmarkNoPresent && !xrPresent;) {
        if (mNriSwapchain.Active()) {
            const auto result =
                mNriSwapchain.Acquire(mCurrentFrame, mCurrentImage);
            if (result ==
                Oot3d::NriSwapchainOperationResult::OutOfDate) {
#if defined(__ANDROID__)
                // NRI reports VK_ERROR_SURFACE_LOST_KHR as OUT_OF_DATE: a gone surface is a pause.
                if (AndroidSurfaceGone()) {
                    NoteAndroidSurfaceLost("acquire");
                    return;
                }
#endif
                mSwapchainDirty = true;
                // Let the host pump window events before retrying acquisition.
                return;
            }
            if (result !=
                Oot3d::NriSwapchainOperationResult::Success) {
                throw std::runtime_error(
                    "NRI AcquireNextTexture failed: " +
                    mNriSwapchain.UnavailableReason());
            }
            nriAcquire = true;
            acquire = VK_SUCCESS;
            break;
        }
        {
            std::lock_guard swapchainLock(mSwapchainCallMutex);
            acquire = vkAcquireNextImageKHR(
                mDevice, mSwapchain, std::numeric_limits<uint64_t>::max(),
                mImageAvailableSemaphores[mCurrentFrame], VK_NULL_HANDLE,
                &mCurrentImage);
        }
        if (acquire == VK_NOT_READY || acquire == VK_TIMEOUT) {
            return;
        }
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            mSwapchainDirty = true;
            return;
        }
#if defined(__ANDROID__)
        if (acquire == VK_ERROR_SURFACE_LOST_KHR) {
            NoteAndroidSurfaceLost("acquire");
            return;
        }
#endif
        break;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        CheckVk(acquire, "vkAcquireNextImageKHR");
    }
    if (acquire == VK_SUBOPTIMAL_KHR) {
        mSwapchainSuboptimal.store(true);
    }

    markStart(5);
    // The presentation picture: the chosen aspect ratio's letterbox inside the
    // successfully acquired swapchain (the whole swapchain for Free). Native
    // PICA targets follow the picture, never the swapchain, so a change of
    // ratio rebuilds them like a resize does.
    mPresentationPicture = Oot3d::ResolvePresentationPicture(
        mSwapchainExtent.width, mSwapchainExtent.height,
        graphicsSnapshot.Value.Aspect);
#if defined(TRIAEVUM_QUEST_OPENXR)
    mXrPanelPicture = mPresentationPicture;  // in-world: restored when the panel is selected again
#endif
    // Validate targets against the picture of the successfully acquired
    // swapchain, not the requested window size or the previous swapchain.
    // Mode/present-only changes retain targets; saved display pixels bridge
    // presentation-only frames.
    const auto pictureExtent = NativePicaOutputExtent();
    const bool staleTargetExtent = std::any_of(
        mNativePicaRenderTargets.begin(), mNativePicaRenderTargets.end(),
        [&](const auto& entry) {
            const auto& [key, target] = entry;
            const auto expected = Oot3d::ResolveNativePicaRenderExtent(
                {key.Width, key.Height},
                NativePicaOutputExtent(key.RenderTargetNamespace),
                mInternalResolutionScale);
            return target.Width != expected.Width || target.Height != expected.Height;
        });
    if (staleTargetExtent) {
        ResetNativePicaRenderTargets(true);
    }
    settingsRuntime.PublishDisplayMetrics({
        mSwapchainExtent.width, mSwapchainExtent.height, 0, 0,
        mInternalResolutionScale,
        !mWindowBackend->IsFullscreen() ? Oot3d::WindowMode::Windowed :
        mWindowBackend->IsWindowedFullscreen() ? Oot3d::WindowMode::Borderless :
                                               Oot3d::WindowMode::ExclusiveFullscreen,
        pictureExtent.Width, pictureExtent.Height});

    if (mImagesInFlight[mCurrentImage] != VK_NULL_HANDLE) {
        CheckVk(vkWaitForFences(mDevice, 1, &mImagesInFlight[mCurrentImage], VK_TRUE,
                                std::numeric_limits<uint64_t>::max()),
                "vkWaitForFences(image)");
    }
    mImagesInFlight[mCurrentImage] = mInFlightFences[mCurrentFrame];
    frame.VertexBytesUsed = 0;
    frame.UniformBytesUsed = 0;
    frame.SurfaceLightingUniformOffset.reset();
    CheckVk(vkResetDescriptorPool(mDevice, frame.DescriptorPool, 0), "vkResetDescriptorPool");
    frame.TextureDescriptorSets.clear();
    CheckVk(vkResetFences(mDevice, 1, &mInFlightFences[mCurrentFrame]), "vkResetFences");
    CheckVk(vkResetCommandBuffer(mCommandBuffers[mCurrentFrame], 0), "vkResetCommandBuffer");

    mFrameAcquirePending = !mBenchmarkNoPresent && !xrPresent;
    mFrameExternalComputeSplit = false;
    mFrameExternalWaitSemaphore = VK_NULL_HANDLE;
    mFrameExternalWaitValue = 0U;
#if defined(TRIAEVUM_QUEST_OPENXR)
    if (xrPresent && xrCopyWaitValue != 0U) {
        // The XR queue's last copy out of this target precedes this frame's writes on the GPU
        // too (the CPU wait above only ordered the host).
        mFrameExternalWaitSemaphore = TriAevumQuestXr::CopyTimeline();
        mFrameExternalWaitValue = xrCopyWaitValue;
    }
#endif

    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(vkBeginCommandBuffer(mCommandBuffers[mCurrentFrame], &beginInfo), "vkBeginCommandBuffer");
    if (mNriInterop.Available() &&
        !mNriInterop.WrapFrameCommandBuffer(mCurrentFrame, mCommandBuffers[mCurrentFrame])) {
        SPDLOG_WARN("OOT3D Vulkan NRI could not wrap frame command buffer {}", mCurrentFrame);
    }
    mFrameActive = true;
    PrepareRecordedDlssFeature();
    ++mFrameCounter;
    Oot3d::BeginGrassStageFrame(mFrameCounter);
    mSurfaceCalibrationTrace.BeginFrame(mFrameCounter);
    mDrawCallCountThisFrame = 0;
    mRenderPassActive = false;
    mOverlayRenderPassActive = false;
    mNativePicaRenderPassActive = false;
    mActiveNativePicaRenderTarget = nullptr;
    mNativePicaPresentedThisFrame = false;
    mPicaSceneFrame.BeginFrame(mCurrentFrame, mFrameCounter);
    mPreparedShadowCasterFrame.BeginFrame(mCurrentFrame, mFrameCounter);
    mPreparedShadowCasters.clear();
    mPicaScenePublications.Reset();
    mDiagnostics.BeginFrame(mFrameCounter);
    mDiagnostics.RecordPicaShaderProfile(mNativeFidelityProfile);
    mDiagnostics.RecordPicaAttachmentContract(
        mFramePicaAttachmentRequirements.ColorAttachmentCount(),
        mFramePicaAttachmentRequirements.Key());
    const auto diagnosticCount = [](uint64_t value) {
        return static_cast<uint32_t>(std::min<uint64_t>(
            value, std::numeric_limits<uint32_t>::max()));
    };
    mDiagnostics.RecordPicaPipelinePrewarm(
        diagnosticCount(mNativePicaPipelines.size()),
        diagnosticCount(mPicaPipelineManifest.Entries().size()),
        mPicaPipelinePrewarmEnabled,
        diagnosticCount(mPicaPipelinePrewarmCreated),
        diagnosticCount(mPicaPipelinePrewarmReused),
        diagnosticCount(mPicaPipelinePrewarmSkipped));
    mDiagnostics.RecordEffectGraph(
        static_cast<uint32_t>(mPicaExtensionGraph.Passes.size()),
        static_cast<uint32_t>(
            mPicaExtensionGraph.ResourceLifetimes.size()),
        static_cast<uint32_t>(std::min<size_t>(
            mPicaExtensionGraph.DeclaredBindingCount(),
            std::numeric_limits<uint32_t>::max())),
        static_cast<uint32_t>(mPicaExtensionGraph.Barriers.size()));
    mDiagnostics.RecordRendererValidation(
        mValidationTelemetry.Snapshot());
    mDiagnostics.RecordNriSwapchainAcquire(
        nriAcquire,
        static_cast<uint32_t>(mSwapchainImages.size()),
        nriAcquire,
        nriAcquire);
    if (const auto timings = mGpuProfiler.BeginFrame(mCurrentFrame, mFrameCounter, mCommandBuffers[mCurrentFrame]);
        mFrameCounter > kFramesInFlight) {
        if (timings)
            mDiagnostics.SetGpuTimings(mFrameCounter - kFramesInFlight, *timings);
#if defined(TRIAEVUM_QUEST_OPENXR)
        // In-world TRIAEVUM_XR_PERF: the slot's previous frame on the GPU (thread=gpu frame=).
        if (mXrWorld && timings && timings->FrameMilliseconds)
            TriAevumQuestXr::PerfNote(TriAevumQuestXr::PerfPhase::GpuFrame, *timings->FrameMilliseconds);
#endif
        if (Oot3d::GrassStageProfileEnabled()) {
            const auto measured = timings.value_or(Oot3dVulkanGpuTimings{});
            Oot3d::ReportGrassStageFrame(
                mFrameCounter - kFramesInFlight, measured.GrassMilliseconds.value_or(-1),
                measured.GrassCompactionMilliseconds.value_or(-1), measured.GrassBladesMilliseconds.value_or(-1),
                measured.GrassShadowsMilliseconds.value_or(-1), measured.GrassLightingMilliseconds.value_or(-1),
                measured.GrassCardsMilliseconds.value_or(-1));
        }
    }
    mViewport = { 0.0f, 0.0f, static_cast<float>(mSwapchainExtent.width),
                  static_cast<float>(mSwapchainExtent.height), 0.0f, 1.0f };
    mScissor = { { 0, 0 }, mSwapchainExtent };
    mViewportSet = true;
    mScissorSet = true;
    markStart(6);
    if (traceStart) {
        double total = 0;
        for (double ms : startTimes) total += ms;
        if (total >= 10.0) {
            std::fprintf(stderr, "TRIAEVUM_SLOW_FRAME_START frame=%llu settings_ms=%.3f present_wait_ms=%.3f transitions_ms=%.3f fence_ms=%.3f retire_ms=%.3f acquire_ms=%.3f finish_ms=%.3f\n",
                static_cast<unsigned long long>(mFrameCounter), startTimes[0], startTimes[1],
                startTimes[2], startTimes[3], startTimes[4], startTimes[5], startTimes[6]);
        }
    }
}

void GfxRenderingAPIVulkan::EndFrame() {
    Renderer3ds::SlowStepScope slowStep("end_frame");
    if (!mFrameActive) {
        return;
    }
#if defined(TRIAEVUM_QUEST_OPENXR)
    const auto xrEndStart = std::chrono::steady_clock::now();
    mXrFrameCopyToken = 0;
#endif
    StoreDlssFeatureRecord();
    if (mOot3dShadow2dPassActive) {
        EndOot3dShadow2dDepthEncodePass();
    }
    EndNativePicaRenderPass();
    bool rescanLastDisplay = !mNativePicaPresentedThisFrame &&
                             mLastPresentedNativePicaDisplayTransfer.has_value();
#if defined(TRIAEVUM_QUEST_OPENXR)
    // In-world: a world frame never re-scans an old display (it would put a stale picture into the
    // panel); the XR thread keeps showing the last eye pair (inworld-spec §2.4).
    if (mXrWorld && mXrWorldFrame) rescanLastDisplay = false;
#endif
    if (rescanLastDisplay) {
        std::string error;
        GfxNativePicaDisplayTransferView transfer =
            *mLastPresentedNativePicaDisplayTransfer;
        transfer.Present = true;
        if (!SubmitPicaDisplayTransfer(
                transfer, &error)) {
            throw std::runtime_error(
                "persistent native PICA scanout failed: " + error);
        }
    }
    if (!mNativePicaPresentedThisFrame) {
        BeginRenderPassIfNeeded();
    }
    if (mRenderPassActive) {
        vkCmdEndRenderPass(mCommandBuffers[mCurrentFrame]);
        mRenderPassActive = false;
        mOverlayRenderPassActive = false;
    }
#if defined(TRIAEVUM_QUEST_OPENXR)
    if (mXrWorld) mXrViewPresented[mXrView] = mXrViewPresented[mXrView] || mNativePicaPresentedThisFrame;
#endif
    const auto sceneFrameStats = mPicaSceneFrame.Stats();
    if (sceneFrameStats.DrawCount != 0U) ++mTemporalJitterSequence;
    mScanoutProbe.Record(mCommandBuffers[mCurrentFrame], mSwapchainImages[mCurrentImage],
                         mSwapchainFormat, mSwapchainExtent, mCurrentFrame, mCurrentImage,
                         mFrameCounter, mNativePicaPresentedThisFrame);
    mDiagnostics.RecordPicaSceneFrame(sceneFrameStats);
#if defined(TRIAEVUM_QUEST_OPENXR)
    // In-world: a frame that drew a pair copies it into the XR thread's held swapchain images inside this
    // very submission (FinishRender publishes it with the token); without a held image the XR queue copies
    // it after the frame, as before.
    if (mXrWorld && mXrViewPresented[kXrViewEyeLeft] && mXrViewPresented[kXrViewEyeRight] &&
        mXrStereoViews.Valid)
        mXrFrameCopyToken = TriAevumQuestXr::RecordFrameCopy(mCommandBuffers[mCurrentFrame], mCurrentFrame,
                                                             TriAevumQuestXr::HudEnabled());
#endif
    mGpuProfiler.EndFrame(mCurrentFrame, mCommandBuffers[mCurrentFrame]);
    CheckVk(vkEndCommandBuffer(mCommandBuffers[mCurrentFrame]), "vkEndCommandBuffer");

#if defined(TRIAEVUM_QUEST_OPENXR)
    if (mXrPresent) {
        // The XR thread copies the target once the render timeline reaches this frame's value.
        SubmitFrameCommandBuffer(mCommandBuffers[mCurrentFrame],
                                 mInFlightFences[mCurrentFrame],
                                 TriAevumQuestXr::RenderTimeline(), ++mXrRenderValue);
    } else
#endif
    SubmitFrameCommandBuffer(mCommandBuffers[mCurrentFrame],
                             mInFlightFences[mCurrentFrame],
                             mRenderFinishedSemaphores[mCurrentImage]);
    mGpuCopySubmittedFrames[mCurrentFrame] = mFrameCounter;
    mFrameActive = false;
    mFrameSubmitted = true;
#if defined(TRIAEVUM_QUEST_OPENXR)
    if (mXrWorld)
        TriAevumQuestXr::PerfNote(
            TriAevumQuestXr::PerfPhase::WorkerSubmit,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - xrEndStart).count());
#endif
}

void GfxRenderingAPIVulkan::FinishRender() {
    Renderer3ds::SlowStepScope slowStep("finish_render");
    if (!mFrameSubmitted) {
        return;
    }
    bool nriPresented = false;
    if (mBenchmarkNoPresent) {
        // Throughput benchmark: the frame is complete once submitted.
#if defined(TRIAEVUM_QUEST_OPENXR)
    } else if (mXrPresent) {
        if (mXrWorld) {
            // In-world: both eyes scanned out this frame -> the pair with its poses; a world frame
            // without a pair publishes nothing (the last pair stays, timewarped); otherwise the panel.
            // A pair carries its panel: the frame-start clear left it transparent and only the
            // overlays (TopScreen HUD, ImGui) drew on it, so it is the HUD quad's image (W4, minimal).
            const uint32_t slot = mCurrentImage / kXrViewCount;
            const uint64_t frameCopy = std::exchange(mXrFrameCopyToken, uint64_t{0});
            if (mXrViewPresented[kXrViewEyeLeft] && mXrViewPresented[kXrViewEyeRight] &&
                mXrStereoViews.Valid) {
                TriAevumQuestXr::PublishStereo(slot, mXrRenderValue, mXrStereoViews,
                                               TriAevumQuestXr::HudEnabled(), frameCopy);
            } else {
                TriAevumQuestXr::AbandonFrameCopy(frameCopy, mXrRenderValue);
                if (!mXrWorldFrame) TriAevumQuestXr::PublishPanel(slot, mXrRenderValue);
            }
        } else {
        // Immersive mode: hand the finished target to the XR thread (copied when GPU-complete).
        TriAevumQuestXr::Publish(mCurrentImage, mXrRenderValue);
        }
#endif
    } else if (mNriSwapchain.Active()) {
        const auto result = mNriSwapchain.Present(mCurrentImage);
        if (result == Oot3d::NriSwapchainOperationResult::OutOfDate) {
#if defined(__ANDROID__)
            // NRI reports VK_ERROR_SURFACE_LOST_KHR as OUT_OF_DATE: a gone surface is a pause.
            if (AndroidSurfaceGone())
                NoteAndroidSurfaceLost("present");
            else
#endif
            mSwapchainDirty = true;
        } else if (result !=
                   Oot3d::NriSwapchainOperationResult::Success) {
            throw std::runtime_error(
                "NRI QueuePresent failed: " +
                mNriSwapchain.UnavailableReason());
        } else {
            nriPresented = true;
        }
    } else if (mAsyncPresentSupported) {
        PresentRequest request;
        request.Swapchain = mSwapchain;
        request.WaitSemaphore = mRenderFinishedSemaphores[mCurrentImage];
        request.ImageIndex = mCurrentImage;
        request.FrameIndex = mCurrentFrame;
        {
            std::lock_guard lock(mPresentMutex);
            request.Serial = mNextPresentSerial++;
            mFramePresentSerials[mCurrentFrame] = request.Serial;
            mPresentRequests.push_back(request);
        }
        mPresentRequestCondition.notify_one();
    } else {
        VkPresentInfoKHR presentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &mRenderFinishedSemaphores[mCurrentImage];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &mSwapchain;
        presentInfo.pImageIndices = &mCurrentImage;
        VkResult present;
        {
            std::lock_guard swapchainLock(mSwapchainCallMutex);
            present = vkQueuePresentKHR(mPresentQueue, &presentInfo);
        }
        if (present == VK_ERROR_OUT_OF_DATE_KHR) {
            mSwapchainDirty = true;
        } else if (present == VK_SUBOPTIMAL_KHR) {
            mSwapchainSuboptimal.store(true);
#if defined(__ANDROID__)
        } else if (present == VK_ERROR_SURFACE_LOST_KHR) {
            NoteAndroidSurfaceLost("present");
#endif
        } else if (present != VK_SUCCESS) {
            CheckVk(present, "vkQueuePresentKHR");
        }
    }
    mDiagnostics.RecordNriSwapchainPresent(nriPresented);
    mDiagnostics.RecordEffectGeometryProvider(
        mInteractiveGrassProviderExecution.Summary());
    mDiagnostics.RecordRendererValidation(
        mValidationTelemetry.Snapshot());
    mDiagnostics.EndFrame();
    FinishSurfaceCalibrationTrace();
    mFrameSubmitted = false;
    mCurrentFrame = (mCurrentFrame + 1) % kFramesInFlight;
}


#if defined(TRIAEVUM_QUEST_OPENXR)
void GfxRenderingAPIVulkan::BeginXrViewFrame() {
    // Each view keeps its own last presented display (its re-scan fallback) across frames.
    mXrViewLastTransfer[mXrView] = mLastPresentedNativePicaDisplayTransfer;
    mXrView = kXrViewPanel;
    mLastPresentedNativePicaDisplayTransfer = mXrViewLastTransfer[kXrViewPanel];
    mXrViewPresented = {};
    mXrWorldFrame = false;
    mXrStereoViews = {};
    mCurrentImage = mCurrentFrame * kXrViewCount + kXrViewPanel;
    mSwapchainExtent = mXrViewExtents[kXrViewPanel];
}

bool GfxRenderingAPIVulkan::SelectXrView(uint32_t view) {
    if (!mXrPresent || !mXrWorld || view >= kXrViewCount || !mFrameActive || mSwapchainImages.empty())
        return false;
    if (view == mXrView) return true;
    // The open passes belong to the old image.
    if (mOot3dShadow2dPassActive) EndOot3dShadow2dDepthEncodePass();
    EndNativePicaRenderPass();
    if (mRenderPassActive) {
        vkCmdEndRenderPass(mCommandBuffers[mCurrentFrame]);
        mRenderPassActive = false;
        mOverlayRenderPassActive = false;
    }
    mXrViewPresented[mXrView] = mXrViewPresented[mXrView] || mNativePicaPresentedThisFrame;
    mXrViewLastTransfer[mXrView] = mLastPresentedNativePicaDisplayTransfer;
    mXrView = view;
    mNativePicaPresentedThisFrame = mXrViewPresented[view];
    mLastPresentedNativePicaDisplayTransfer = mXrViewLastTransfer[view];
    mCurrentImage = mCurrentFrame * kXrViewCount + view;
    mSwapchainExtent = mXrViewExtents[view];
    // The eyes fill their whole image (Aspect Free); the panel keeps the picture of its aspect.
    mPresentationPicture = view == kXrViewPanel
        ? mXrPanelPicture
        : Oot3d::PresentationPicture{0U, 0U, mSwapchainExtent.width, mSwapchainExtent.height};
    mViewport = {0.0F, 0.0F, static_cast<float>(mSwapchainExtent.width),
                 static_cast<float>(mSwapchainExtent.height), 0.0F, 1.0F};
    mScissor = {{0, 0}, mSwapchainExtent};
    mViewportSet = true;
    mScissorSet = true;
    return true;
}

void GfxRenderingAPIVulkan::SetXrStereoViews(const StereoViews& views) {
    if (!mXrPresent || !mXrWorld) return;
    mXrStereoViews = views;
    mXrWorldFrame = true;
}

void GfxRenderingAPIVulkan::SetNamespaceOutputExtent(uint64_t renderTargetNamespace, uint32_t width,
                                                     uint32_t height) {
    if (renderTargetNamespace >= mXrNamespaceExtentSet.size()) return;
    const bool set = width != 0U && height != 0U;
    mXrNamespaceExtentSet[renderTargetNamespace] = set;
    mXrNamespaceExtents[renderTargetNamespace] = set ? Oot3d::RenderExtent{width, height} : Oot3d::RenderExtent{};
}
#endif

} // namespace Fast

#endif
