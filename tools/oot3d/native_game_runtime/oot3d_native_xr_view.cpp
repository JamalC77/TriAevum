// The picture per eye of the in-world mode (see oot3d_native_xr_view.h).
#include "oot3d_native_xr_view.h"

#include "oot3d_native_pica_composition.h"

#if defined(TRIAEVUM_QUEST_OPENXR)
#include "quest_xr.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace Oot3dNativeGame {

namespace {

namespace W = XrWorld;
namespace M = XrMath;

const char* Env(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : nullptr;
}

bool EnvTrue(const char* name) {
    const char* value = Env(name);
    return value != nullptr && (std::string_view(value) == "1" || std::string_view(value) == "true");
}

const Oot3dPicaVertexUniformState& Uniforms(const Oot3dPicaVisualDrawView& view) {
    return view.VertexUniforms.has_value() ? *view.VertexUniforms : view.BasePlan->VertexShader.Uniforms;
}

M::Rows ProjectionRows(const Oot3dPicaVertexUniformState& uniforms) {
    M::Rows rows{};
    for (size_t k = 0; k < 4; ++k) rows[k] = uniforms.Floats[k];
    return rows;
}

const std::optional<std::array<float, 16>>& WorldToView(const Oot3dPicaVisualDrawView& view) {
    return view.LightBinding.has_value() ? view.LightBinding->WorldToView : view.BasePlan->LightBinding.WorldToView;
}

const Oot3dPicaViewportState& Viewport(const Oot3dPicaVisualDrawView& view) {
    return view.Viewport.has_value() ? *view.Viewport : view.BasePlan->State.Viewport;
}

bool CoversTarget(const Oot3dPicaViewportState& viewport, const Oot3dPicaFramebufferState& framebuffer) {
    return viewport.CornerX <= 0 && viewport.CornerY <= 0 &&
           static_cast<float>(viewport.CornerX) + viewport.HalfWidth * 2.0F >= static_cast<float>(framebuffer.Width) &&
           static_cast<float>(viewport.CornerY) + viewport.HalfHeight * 2.0F >= static_cast<float>(framebuffer.Height);
}

}  // namespace

void XrWorldLog(const char* format, ...) {
    char buffer[1024];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(buffer, sizeof buffer, format, arguments);
    va_end(arguments);
#if defined(TRIAEVUM_QUEST_OPENXR)
    TriAevumQuestXr::LogLine(buffer);
#else
    std::fprintf(stderr, "%s\n", buffer);
#endif
}

XrFrameFacts CollectXrFrameFacts(const Oot3dPicaVisualFrame& frame) {
    XrFrameFacts facts;
    const uint32_t top = frame.TopTransfer.InputPhysicalAddress;
    for (const auto& plan : frame.Draws) {
        if (plan.State.Framebuffer.ColorPhysicalAddress != top) continue;
        if (plan.CompositionDomain == Oot3dPicaCompositionDomain::Ui) {
            if (!facts.NonHudUi && plan.Composition.SourcePc != kOot3dGameplayHudDrawEntry) {
                facts.NonHudUi = true;
                facts.UiSourcePc = plan.Composition.SourcePc;
            }
            continue;
        }
        if (facts.HasCamera || plan.Composition.Layer != Oot3dPicaCompositionLayer::OpaqueWorld ||
            !plan.LightBinding.WorldToView.has_value())
            continue;
        const auto projection = M::DecomposeGameProjection(ProjectionRows(plan.VertexShader.Uniforms));
        if (!projection.Ok()) continue;
        facts.HasCamera = true;
        facts.Mirrored = M::MirroredFrame(projection, M::Mat4FromColumnMajor(*plan.LightBinding.WorldToView));
    }
    return facts;
}

std::unique_ptr<XrViewRenderer> XrViewRenderer::Create(bool world) {
    auto renderer = std::unique_ptr<XrViewRenderer>(new XrViewRenderer());
    renderer->mWorld = world;
    renderer->mTrace = EnvTrue("TRIAEVUM_XR_VIEW_TRACE");
#if !defined(TRIAEVUM_QUEST_OPENXR)
    // The PC preview (§2.7): one eye on the desktop swapchain. Never on the Quest (the headset shows both).
    if (const char* preview = Env("TRIAEVUM_XR_PREVIEW")) {
        const std::string_view eye(preview);
        if (eye == "left" || eye == "right") {
            renderer->mPreview.Enabled = true;
            renderer->mPreview.Eye = eye == "right" ? 1 : 0;
            const char* head = Env("TRIAEVUM_XR_PREVIEW_HEAD");
            if (!renderer->mPreview.Head.Parse(head != nullptr ? head : "identity")) {
                XrWorldLog("TRIAEVUM_XR_PREVIEW head=\"%s\" invalid: identity", head);
                renderer->mPreview.Head = {};
            }
        } else {
            XrWorldLog("TRIAEVUM_XR_PREVIEW value=\"%s\" invalid (left|right): off", preview);
        }
    }
#endif
    // The sky scopes (§2.3 SkyScopePcs): the W0 trace on the device picks them; until then every
    // atmosphere scope but precipitation and the kankyo effect primitives may be sky when camera-centred.
    auto& scopes = renderer->mSkyScopes;
    scopes.Excluded[scopes.ExcludedCount++] = kOot3dGameplayDrawPrecipitationEffectsEntry;
    scopes.Excluded[scopes.ExcludedCount++] = kOot3dKankyoEffectPrimitiveDrawEntry;
    if (const char* list = Env("TRIAEVUM_XR_SKY_PCS")) {
        if (!scopes.ParseAllowed(list)) {
            XrWorldLog("TRIAEVUM_XR_SKY_PCS \"%s\" invalid: default scopes", list);
            scopes.AllowedCount = 0;
        }
    }
    if (!renderer->mWorld && !renderer->mPreview.Enabled && !renderer->mTrace) return nullptr;
    XrWorldLog("TRIAEVUM_XR_VIEW_MODE world=%d preview=%s head=%s trace=%d sky_pcs=%zu", renderer->mWorld ? 1 : 0,
               renderer->mPreview.Enabled ? (renderer->mPreview.Eye == 0 ? "left" : "right") : "off",
               Env("TRIAEVUM_XR_PREVIEW_HEAD") != nullptr ? Env("TRIAEVUM_XR_PREVIEW_HEAD") : "identity",
               renderer->mTrace ? 1 : 0, scopes.AllowedCount);
    return renderer;
}

XrFrameCamera XrViewRenderer::FindFrameCamera(const Oot3dPicaVisualFrameViewSample& sample) const {
    XrFrameCamera camera;
    for (const auto& view : sample.Draws) {
        if (view.BasePlan == nullptr ||
            view.BasePlan->Composition.Layer != Oot3dPicaCompositionLayer::OpaqueWorld ||
            view.BasePlan->State.Framebuffer.ColorPhysicalAddress != sample.TopTransfer.InputPhysicalAddress)
            continue;
        const auto& worldToView = WorldToView(view);
        if (!worldToView.has_value()) continue;
        const auto projection = M::DecomposeGameProjection(ProjectionRows(Uniforms(view)));
        if (!projection.Ok()) continue;
        camera.Valid = true;
        camera.WorldToView = *worldToView;
        camera.View = M::Mat4FromColumnMajor(*worldToView);
        camera.Projection = projection;
        camera.Mirrored = M::MirroredFrame(projection, camera.View);
        break;
    }
    return camera;
}

XrViewRenderer::Facts XrViewRenderer::ExtractFacts(const Oot3dPicaVisualDrawView& view,
                                                   const Oot3dPicaVisualFrameViewSample& sample,
                                                   const XrFrameCamera& camera) const {
    Facts facts;
    const auto& plan = *view.BasePlan;
    auto& f = facts.Draw;
    f.OnTopTarget = plan.State.Framebuffer.ColorPhysicalAddress == sample.TopTransfer.InputPhysicalAddress;
    f.UiDomain = plan.CompositionDomain == Oot3dPicaCompositionDomain::Ui;
    f.ViewportCoversTarget = CoversTarget(Viewport(view), plan.State.Framebuffer);
    if (f.UiDomain && f.ViewportCoversTarget) {
        // The backend's Screen evidence (pica_composition.h) without its geometry test: untextured, or
        // sampling only GPU copies of rendered output (washes and full-frame redraws).
        f.ScreenContent = plan.Textures.empty() ||
                          std::all_of(plan.Textures.begin(), plan.Textures.end(),
                                      [](const auto& texture) { return !texture.CopyReferences.empty(); });
    }
    f.Atmosphere = plan.Composition.Layer == Oot3dPicaCompositionLayer::Atmosphere;
    facts.Rows = ProjectionRows(Uniforms(view));
    f.Projection = M::DecomposeGameProjection(facts.Rows).Status;
    const auto& worldToView = WorldToView(view);
    f.HasWorldToView = worldToView.has_value();
    if (f.HasWorldToView) facts.WorldToView = &*worldToView;
    f.HasFrameCamera = camera.Valid;
    f.SkyScope = mSkyScopes.Contains(plan.Composition.SourcePc);
    if (f.Atmosphere && plan.VertexShader.TemporalProgram != nullptr) {
        const auto& group = plan.VertexShader.TemporalProgram->AffineLayout.ModelView;
        if (group.Valid()) {
            const auto& uniforms = Uniforms(view);
            const size_t first = group.FirstUniform;
            f.CameraCentred = W::CameraCentred(uniforms.Floats[first], uniforms.Floats[first + 1],
                                               uniforms.Floats[first + 2], mScale, &facts.OriginDistance);
        }
    }
    return facts;
}

const Oot3dPicaVisualFrameViewSample& XrViewRenderer::BuildEye(const Oot3dPicaVisualFrameViewSample& sample,
                                                               const XrFrameCamera& camera,
                                                               const W::XrEyeInput& eye, bool mirrored,
                                                               int eyeIndex) {
    auto& out = mEyeScratch[static_cast<size_t>(eyeIndex & 1)];
    auto& chains = mChains[static_cast<size_t>(eyeIndex & 1)];
    chains.Reset(eye, mirrored);
    out.TopTransfer = sample.TopTransfer;
    out.MemoryFills = sample.MemoryFills;
    out.DisplayTransfers = sample.DisplayTransfers;
    out.MatchedDraws = sample.MatchedDraws;
    out.StrictUniqueMatchedDraws = sample.StrictUniqueMatchedDraws;
    out.StrictOrdinalMatchedDraws = sample.StrictOrdinalMatchedDraws;
    out.StructuralOrdinalMatchedDraws = sample.StructuralOrdinalMatchedDraws;
    out.PipelineOrdinalMatchedDraws = sample.PipelineOrdinalMatchedDraws;
    out.InterpolatedDraws = sample.InterpolatedDraws;
    out.InterpolatedPerInstanceVertexDraws = sample.InterpolatedPerInstanceVertexDraws;
    out.InterpolatedPerVertexDraws = sample.InterpolatedPerVertexDraws;
    // Element-wise assignment keeps each draw view's allocations (vertex binding vectors) across frames.
    out.Draws.resize(sample.Draws.size());
    if (eyeIndex == 0) {
        ++mStats.EyeSamples;
        if (mirrored) ++mStats.MirroredSamples;
        if (!camera.Valid) ++mStats.NoFrameCamera;
        if (camera.Valid) {
            mStats.C = camera.Projection.C;
            mStats.DetR = camera.Projection.DetR;
            mStats.DetV = M::Determinant3(camera.View) < 0.0 ? -1 : 1;
        }
    }
    for (size_t index = 0; index < sample.Draws.size(); ++index) {
        const auto& view = sample.Draws[index];
        auto& target = out.Draws[index];
        target = view;
        target.Omitted = false;
        if (view.BasePlan == nullptr) continue;
        const Facts facts = ExtractFacts(view, sample, camera);
        const W::XrDrawClass drawClass = W::ClassifyXrDraw(facts.Draw);
        W::XrDrawClass counted = drawClass;
        if (eyeIndex == 0 && facts.Draw.OnTopTarget && !facts.Draw.UiDomain &&
            facts.Draw.Projection == M::ProjectionStatus::NonNegativeC)
            ++mStats.NonNegativeC;
        const auto treatment = W::TreatmentOf(drawClass);
        if (treatment == W::XrDrawTreatment::Omit) {
            target.Omitted = true;
        } else if (treatment != W::XrDrawTreatment::GameUniforms) {
            const std::array<float, 16>& worldToView =
                facts.WorldToView != nullptr ? *facts.WorldToView : camera.WorldToView;
            M::Rows rows{};
            if (!W::XrDrawRows(facts.Rows, worldToView, treatment == W::XrDrawTreatment::SkyRows, chains, &rows)) {
                target.Omitted = true;
                counted = W::XrDrawClass::Fallback;
                if (eyeIndex == 0) ++mStats.RowFailures;
            } else {
                if (!target.VertexUniforms.has_value()) target.VertexUniforms = view.BasePlan->VertexShader.Uniforms;
                for (size_t k = 0; k < 4; ++k) target.VertexUniforms->Floats[k] = rows[k];
                if (eyeIndex == 0 && facts.WorldToView != nullptr && camera.Valid) {
                    const auto own = M::DecomposeGameProjection(facts.Rows);
                    if (M::MirroredFrame(own, M::Mat4FromColumnMajor(*facts.WorldToView)) != camera.Mirrored)
                        ++mStats.MirrorDisagreements;
                }
            }
        }
        if (eyeIndex == 0) {
            ++mStats.Classes[static_cast<size_t>(counted)];
            if (counted == W::XrDrawClass::Fallback) mFallbackThisFrame = true;
        }
    }
    return out;
}

W::XrEyeInput XrViewRenderer::PreviewEye(const XrFrameCamera& camera, double seconds, double aspect) const {
    W::XrEyeInput eye;
    if (!camera.Valid) return eye;
    // The equivalent eye of the frame (§2.2): V_g itself, or S * V_g * M with mirrored x tangents for a
    // mirrored frame, so the identity head reproduces the game's rows exactly.
    M::Mat4 base = camera.View;
    M::EyeFov gameFov = camera.Projection.Tangents;
    if (camera.Mirrored) {
        base = M::MirrorMatrix(true) * camera.View * M::MirrorMatrix(true);
        gameFov.TanLeft = -camera.Projection.Tangents.TanRight;
        gameFov.TanRight = -camera.Projection.Tangents.TanLeft;
    }
    const auto& head = mPreview.Head;
    eye.Valid = true;
    if (head.Kind == W::XrPreviewHeadKind::Identity) {
        eye.View = base;
        eye.Fov = gameFov;
        return eye;
    }
    const double s = mScale;
    const double ipd = (mPreview.Eye == 0 ? -0.5 : 0.5) * mPreview.IpdM * s;
    eye.View = W::PreviewEyeView(base, head.YawAt(seconds), {head.X * s, head.Y * s, head.Z * s}, ipd);
    const double tanY = std::tan(M::DegreesToRadians(mPreview.HalfFovDegrees));
    const double tanX = tanY * (aspect > 0.0 ? aspect : 1.0);
    eye.Fov = {-tanX, tanX, tanY, -tanY};
    return eye;
}

W::XrEyeInput XrViewRenderer::TrackedEye(const M::Rig& rig, const Fast::StereoEyeView& view) {
    W::XrEyeInput eye;
    const M::Pose pose = M::PoseFromXr(view.Orientation, view.Position);
    eye.View = M::EyeView(rig, pose);
    eye.Fov = {view.TanLeft, view.TanRight, view.TanUp, view.TanDown};
    eye.Valid = M::IsFinite(eye.View) && M::ValidFov(eye.Fov);
    return eye;
}

bool XrViewRenderer::EndPresentation(bool eyesBuilt) {
    if (eyesBuilt) {
        mConsecutiveFallbackFrames = mFallbackThisFrame ? mConsecutiveFallbackFrames + 1 : 0;
        mStats.FallbackFrames = mConsecutiveFallbackFrames;
    }
    mFallbackThisFrame = false;
    const auto now = std::chrono::steady_clock::now();
    if (now >= mNextLog) {
        mNextLog = now + std::chrono::seconds(5);
        if (mStats.EyeSamples != 0U) {
            const auto& c = mStats.Classes;
            const auto count = [&](W::XrDrawClass value) {
                return static_cast<unsigned long long>(c[static_cast<size_t>(value)]);
            };
            XrWorldLog("TRIAEVUM_XR_VIEW samples=%llu full=%llu sky=%llu offtarget=%llu ui=%llu screen=%llu "
                       "ortho_full=%llu ortho_omitted=%llu framecam=%llu fallback=%llu row_failures=%llu "
                       "c_nonneg=%llu mirrored=%llu mirror_disagree=%llu no_frame_camera=%llu C=%.4f detR=%d "
                       "detV=%d fallback_frames=%llu",
                       static_cast<unsigned long long>(mStats.EyeSamples), count(W::XrDrawClass::World),
                       count(W::XrDrawClass::Sky), count(W::XrDrawClass::OffTarget), count(W::XrDrawClass::Ui),
                       count(W::XrDrawClass::Screen), count(W::XrDrawClass::OrthoFull),
                       count(W::XrDrawClass::OrthoOmitted), count(W::XrDrawClass::FrameCam),
                       count(W::XrDrawClass::Fallback), static_cast<unsigned long long>(mStats.RowFailures),
                       static_cast<unsigned long long>(mStats.NonNegativeC),
                       static_cast<unsigned long long>(mStats.MirroredSamples),
                       static_cast<unsigned long long>(mStats.MirrorDisagreements),
                       static_cast<unsigned long long>(mStats.NoFrameCamera), mStats.C, mStats.DetR, mStats.DetV,
                       static_cast<unsigned long long>(mConsecutiveFallbackFrames));
        }
        const uint64_t keep = mStats.FallbackFrames;
        mStats = {};
        mStats.FallbackFrames = keep;
    }
    return mConsecutiveFallbackFrames >= 30U;
}

void XrViewRenderer::Trace(const Oot3dPicaVisualFrameViewSample& sample, int scene) {
    if (!mTrace) return;
    if (scene != mTraceScene && mTraceSamples != 0U) FlushTrace("scene");
    mTraceScene = scene;
    const XrFrameCamera camera = FindFrameCamera(sample);
    if (camera.Valid) mTraceCamera = camera;
    ++mTraceSamples;
    for (const auto& view : sample.Draws) {
        if (view.BasePlan == nullptr) continue;
        const Facts facts = ExtractFacts(view, sample, camera);
        ++mTraceClasses[static_cast<size_t>(W::ClassifyXrDraw(facts.Draw))];
        if (!facts.Draw.Atmosphere || !facts.Draw.OnTopTarget) continue;
        auto& scope = mTraceScopes[view.BasePlan->Composition.SourcePc];
        ++scope.Draws;
        if (facts.Draw.Projection == M::ProjectionStatus::Ok) ++scope.Perspective;
        if (facts.Draw.ViewportCoversTarget) ++scope.FullViewport;
        if (facts.OriginDistance >= 0.0) {
            ++scope.OriginSamples;
            scope.OriginMin = std::min(scope.OriginMin, facts.OriginDistance);
            scope.OriginMax = std::max(scope.OriginMax, facts.OriginDistance);
        }
    }
    if (std::chrono::steady_clock::now() >= mTraceNextLog) FlushTrace("period");
}

void XrViewRenderer::FlushTrace(const char* reason) {
    mTraceNextLog = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    if (mTraceSamples == 0U) return;
    const auto& camera = mTraceCamera;
    const auto& g = camera.Projection;
    // f2 = (0, 0, A, B), f3 = (0, 0, C, 0): the eye depth where z/w is 0 and -1 (near/far, sign by C).
    const double a = g.Row2[2], b = g.Row2[3], c = g.C;
    const double depth0 = std::fabs(a) > 1e-12 ? -b / a : 0.0;
    const double depthMinus1 = std::fabs(c + a) > 1e-12 ? b / (-c - a) : 0.0;
    const auto& k = mTraceClasses;
    const auto count = [&](W::XrDrawClass value) { return static_cast<unsigned long long>(k[static_cast<size_t>(value)]); };
    XrWorldLog("TRIAEVUM_XR_VIEW_TRACE reason=%s scene=0x%X samples=%llu camera=%d C=%.5f A=%.5f B=%.5f "
               "depth_z0_u=%.2f depth_zm1_u=%.2f R=[[%d,%d],[%d,%d]] detR=%d detV=%d mirrored=%d "
               "tan=%.4f,%.4f,%.4f,%.4f full=%llu sky=%llu offtarget=%llu ui=%llu screen=%llu ortho_full=%llu "
               "ortho_omitted=%llu framecam=%llu fallback=%llu",
               reason, static_cast<unsigned>(mTraceScene), static_cast<unsigned long long>(mTraceSamples),
               camera.Valid ? 1 : 0, c, a, b, std::fabs(depth0), std::fabs(depthMinus1), g.Rot3ds[0][0],
               g.Rot3ds[0][1], g.Rot3ds[1][0], g.Rot3ds[1][1], g.DetR,
               camera.Valid ? (M::Determinant3(camera.View) < 0.0 ? -1 : 1) : 0, camera.Mirrored ? 1 : 0,
               g.Tangents.TanLeft, g.Tangents.TanRight, g.Tangents.TanDown, g.Tangents.TanUp,
               count(W::XrDrawClass::World), count(W::XrDrawClass::Sky), count(W::XrDrawClass::OffTarget),
               count(W::XrDrawClass::Ui), count(W::XrDrawClass::Screen), count(W::XrDrawClass::OrthoFull),
               count(W::XrDrawClass::OrthoOmitted), count(W::XrDrawClass::FrameCam),
               count(W::XrDrawClass::Fallback));
    for (const auto& [pc, scope] : mTraceScopes) {
        XrWorldLog("TRIAEVUM_XR_VIEW_TRACE atmosphere scene=0x%X pc=0x%08X draws=%llu perspective=%llu "
                   "full_viewport=%llu origin_samples=%llu origin_min_u=%.1f origin_max_u=%.1f sky_scope=%d",
                   static_cast<unsigned>(mTraceScene), pc, static_cast<unsigned long long>(scope.Draws),
                   static_cast<unsigned long long>(scope.Perspective),
                   static_cast<unsigned long long>(scope.FullViewport),
                   static_cast<unsigned long long>(scope.OriginSamples),
                   scope.OriginSamples != 0U ? scope.OriginMin : -1.0, scope.OriginSamples != 0U ? scope.OriginMax : -1.0,
                   mSkyScopes.Contains(pc) ? 1 : 0);
    }
    mTraceScopes.clear();
    mTraceClasses = {};
    mTraceSamples = 0;
}

}  // namespace Oot3dNativeGame
