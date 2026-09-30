#include "DonTopo/Editor/RenderingPanel.h"

#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/GpuTimeFormat.h"
#include "DonTopo/Renderer/EditorRenderer.h"

#include <imgui.h>

namespace DonTopo {

void RenderingPanel::draw(EditorContext& ctx, RenderBackend active, RenderBackend& selected)
{
    if (!m_open) return;

    // Without a backend there is nothing to adjust. The panel is still drawn (empty and
    // saying so) instead of disappearing: a panel that vanishes only makes
    // one think the layout is broken.
    if (!ctx.renderer)
    {
        if (ImGui::Begin("Rendering", &m_open))
            ImGui::TextDisabled("No active renderer.");
        ImGui::End();
        return;
    }

    // The wrappers need the UndoManager and the project save, which
    // arrive through the context. They are re-hooked every frame; their drag state
    // lives in the object and survives this.
    m_ctl.bind(ctx.undo, ctx.saveSettings);

    EditorRenderer* rend = ctx.renderer;
    auto guardar = [&] { if (ctx.saveSettings) ctx.saveSettings(); };
    auto log     = [&](const std::string& s) { if (ctx.pushLog) ctx.pushLog(s); };

    if (!ImGui::Begin("Rendering", &m_open))
    {
        ImGui::End();
        return;
    }

    if (ImGui::CollapsingHeader("Ambient (IBL)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // Own ID scope for this section, like the one its BeginMenu gave
        // before this was a panel: without it, a widget named like
        // the section collides with the header.
        ImGui::PushID("Ambient (IBL)");
            const bool ambientOn = m_ctl.checkbox("Ambient (IBL)",
                [rend] { return rend->ambientEnabled(); },
                [rend](bool v) { rend->setAmbientEnabled(v); });

            // Same as in bloom: the slider is not hidden with the ambient
            // off, it is left disabled.
            ImGui::BeginDisabled(!ambientOn);
            // It is saved on RELEASE (and at that very moment it enters the
            // history): dragging from end to end writes once, not
            // once per frame. Same criterion in all the sliders.
            m_ctl.sliderFloat("Ambient intensity", 0.0f, 3.0f, "%.2f",
                [rend] { return rend->ambientIntensity(); },
                [rend](float v) { rend->setAmbientIntensity(v); });
            ImGui::EndDisabled();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Reflection probes"))
    {
        // Own ID scope for this section, like the one its BeginMenu gave
        // before this was a panel: without it, a widget named like
        // the section collides with the header.
        ImGui::PushID("Reflection probes");
            // Reflection probes: GLOBAL control (re-bake the whole scene).
            // Each probe's radius and intensity go in its Properties,
            // which is where what belongs to one object is edited. The bake is only
            // enqueued: the Renderer runs it at the start of the next
            // frame, never as a pass of the frame.
            ImGui::Separator();
            const int probes = rend->probeCount();
            ImGui::BeginDisabled(probes == 0);
            if (ImGui::MenuItem("Bake All Reflection Probes"))
                rend->requestProbeBakeAll();
            ImGui::EndDisabled();
            // The figure is given by the ACTIVE BACKEND: the two store different
            // things per probe and before, the Vulkan one was always shown
            // (H51).
            ImGui::Text("Probes: %d  (%.2f MB each)", probes,
                        (double)rend->probeMemoryBytes() / (1024.0 * 1024.0));
            // Without probes there is no bake to count, and a "0.00 ms" reads as an
            // instant bake instead of as "never" (H56). It is the
            // same distinction the Reflection Probe section of the
            // Properties panel already made with its "not baked".
            if (probes == 0)
                ImGui::TextDisabled("Last bake: no probes in the scene");
            else if (rend->lastProbeBakeMs() <= 0.0f)
                ImGui::TextDisabled("Last bake: not baked");
            else
                ImGui::Text("Last bake: %.2f ms of GPU", rend->lastProbeBakeMs());
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Skybox"))
    {
        // Own ID scope for this section, like the one its BeginMenu gave
        // before this was a panel: without it, a widget named like
        // the section collides with the header.
        ImGui::PushID("Skybox");
            // The sky is NOT edited from here: on an ImGui menu a drag
            // cannot be released (the popup closes when released outside),
            // so it lives in its own window. Here only the entry that
            // opens it, next to the ambient because the global IBL comes from
            // convolving that same cubemap.
            ImGui::Separator();
            if (ImGui::MenuItem("Environment (skybox)..."))
                if (ctx.openEnvironment) ctx.openEnvironment();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Presentation (vsync)"))
    {
        // Own ID scope for this section, like the one its BeginMenu gave
        // before this was a panel: without it, a widget named like
        // the section collides with the header.
        ImGui::PushID("Presentation (vsync)");
            // Present mode. The ones the device does not provide come out
            // DISABLED with their reason, not hidden: if the core
            // supports N options the UI offers N, and the nuance is documented.
            {
                const PresentMode kModos[] = { PresentMode::Vsync,
                                               PresentMode::Mailbox,
                                               PresentMode::Immediate };
                const char* kNombres[] = { "Vsync", "Mailbox", "Immediate" };
                // Two lines per mode: what it does, and what is paid for it.
                const char* kQueHace[] = {
                    "Waits for the refresh.",
                    "Triple buffer: neither waits nor tears the image.",
                    "Does not wait for the refresh.",
                };
                const char* kQueCuesta[] = {
                    "No tearing, but it pins the FPS to the monitor's.",
                    "Draws frames that get discarded. Only Vulkan offers it.",
                    "Tearing appears, and it is the ONLY mode that measures"
                    " the real cost of a frame: with Vsync everything comes out at 16 ms.",
                };

                const int actual = static_cast<int>(rend->presentMode());
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::BeginCombo("Present mode", kNombres[actual]))
                {
                    for (int i = 0; i < IM_ARRAYSIZE(kModos); ++i)
                    {
                        const bool soportado = rend->presentModeSupported(kModos[i]);
                        ImGui::BeginDisabled(!soportado);
                        if (ImGui::Selectable(kNombres[i], i == actual))
                        {
                            const PresentMode antes = rend->presentMode();
                            rend->setPresentMode(kModos[i]);
                            // The GRANTED mode may not be the requested one (a
                            // device without Mailbox falls back to Vsync), so the
                            // "after" is re-read instead of taken for granted:
                            // an undo must go back to what really was there.
                            m_ctl.pushUndo<PresentMode>("Present mode", antes,
                                rend->presentMode(),
                                [rend](const PresentMode& v) { rend->setPresentMode(v); });
                        }
                        ImGui::EndDisabled();
                        // The tooltip goes OUTSIDE the BeginDisabled: a disabled
                        // item does not receive hover, and it is precisely the one that
                        // most needs to explain why it cannot be chosen.
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        {
                            ImGui::BeginTooltip();
                            ImGui::TextUnformatted(kQueHace[i]);
                            ImGui::TextUnformatted(kQueCuesta[i]);
                            if (!soportado)
                            {
                                ImGui::Separator();
                                ImGui::TextColored(
                                    ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                    "Not available on this machine with the active backend.");
                            }
                            ImGui::EndTooltip();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Shadows"))
    {
        // Own ID scope for this section, like the one its BeginMenu gave
        // before this was a panel: without it, a widget named like
        // the section collides with the header.
        ImGui::PushID("Shadows");
            // Cascaded shadows. Both were compile-time constants
            // until now, and they are what is most noticeable: the 4 cascades
            // split "Shadow distance", so lowering it concentrates the
            // same texels in less world and sharpens the nearby shadow.
            // Map resolution. It gives the roughest jump (4096
            // quadruples the texels of 2048), and the only one of the three that
            // moves resources: that is why it goes through the backend and not the state.
            {
                const int  kSizes[]  = {1024, 2048, 4096, 8192};
                const char* kLabels[] = {"1024", "2048", "4096", "8192"};
                int current = 1;
                for (int i = 0; i < IM_ARRAYSIZE(kSizes); ++i)
                    if (kSizes[i] == rend->shadowResolution()) current = i;
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::Combo("Shadow resolution", &current, kLabels, IM_ARRAYSIZE(kLabels)))
                {
                    const int antes = rend->shadowResolution();
                    rend->setShadowResolution(kSizes[current]);
                    // Undoing this recreates the texture array again. It is
                    // expensive and it is the right thing: the user asked to go back.
                    m_ctl.pushUndo<int>("Shadow resolution", antes, kSizes[current],
                        [rend](const int& v) { rend->setShadowResolution(v); });
                }
            }

            m_ctl.sliderFloat("Shadow distance", 20.0f, 2000.0f, "%.0f",
                [rend] { return rend->shadowDistance(); },
                [rend](float v) { rend->setShadowDistance(v); });

            m_ctl.sliderFloat("Cascade blend", 0.0f, 1.0f, "%.2f",
                [rend] { return rend->cascadeLambda(); },
                [rend](float v) { rend->setCascadeLambda(v); });
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("0 = uniform splits, 1 = logarithmic.\n"
                                  "High gives resolution up close; low spreads it more evenly.");

            { char b[kGpuMsTextSize];
                ImGui::Text("Shadows GPU: %s ms", gpuMsText(rend->shadowGpuMs(), b, kGpuMsTextSize)); }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Bloom"))
    {
        // Own ID scope for this section, like the one its BeginMenu gave
        // before this was a panel: without it, a widget named like
        // the section collides with the header.
        ImGui::PushID("Bloom");
            // Bloom. Same criterion as the ambient: session setting, it is not
            // serialized. Intensity 0 leaves the image as before the bloom.
            ImGui::Separator();
            const bool bloom = m_ctl.checkbox("Bloom",
                [rend] { return rend->bloomEnabled(); },
                [rend](bool v) { rend->setBloomEnabled(v); });

            // Same as in SSAO and SSR: the sliders are not hidden with the
            // effect off, they are left disabled.
            ImGui::BeginDisabled(!bloom);
            m_ctl.sliderFloat("Bloom threshold", 0.0f, 5.0f, "%.2f",
                [rend] { return rend->bloomThreshold(); },
                [rend](float v) { rend->setBloomThreshold(v); });

            m_ctl.sliderFloat("Bloom knee", 0.0f, 1.0f, "%.2f",
                [rend] { return rend->bloomKnee(); },
                [rend](float v) { rend->setBloomKnee(v); });

            m_ctl.sliderFloat("Bloom intensity", 0.0f, 1.0f, "%.3f",
                [rend] { return rend->bloomIntensity(); },
                [rend](float v) { rend->setBloomIntensity(v); });
            ImGui::EndDisabled();

            { char b[kGpuMsTextSize];
                ImGui::Text("Bloom GPU: %s ms", gpuMsText(rend->bloomGpuMs(), b, kGpuMsTextSize)); }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("SSAO"))
    {
        // Own ID scope for this section, like the one its BeginMenu gave
        // before this was a panel: without it, a widget named like
        // the section collides with the header.
        ImGui::PushID("SSAO");
            // SSAO. Same criterion as the ambient and the bloom: session
            // setting, it is not serialized. Off leaves the image exactly
            // as before the feature and the GPU cost at zero.
            ImGui::Separator();
            const bool ssao = m_ctl.checkbox("SSAO",
                [rend] { return rend->ssaoEnabled(); },
                [rend](bool v) { rend->setSsaoEnabled(v); });

            // The sliders are not hidden with the effect off: they are left
            // disabled so that it is visible that they exist and with which values
            // they would start.
            ImGui::BeginDisabled(!ssao);
            m_ctl.sliderFloat("SSAO radius", 0.05f, 2.0f, "%.2f",
                [rend] { return rend->ssaoRadius(); },
                [rend](float v) { rend->setSsaoRadius(v); });

            m_ctl.sliderFloat("SSAO bias", 0.0f, 0.2f, "%.3f",
                [rend] { return rend->ssaoBias(); },
                [rend](float v) { rend->setSsaoBias(v); });

            m_ctl.sliderFloat("SSAO intensity", 0.0f, 3.0f, "%.2f",
                [rend] { return rend->ssaoIntensity(); },
                [rend](float v) { rend->setSsaoIntensity(v); });

            m_ctl.sliderFloat("SSAO power", 0.25f, 4.0f, "%.2f",
                [rend] { return rend->ssaoPower(); },
                [rend](float v) { rend->setSsaoPower(v); });
            ImGui::EndDisabled();

            { char b[kGpuMsTextSize];
                ImGui::Text("SSAO GPU: %s ms", gpuMsText(rend->ssaoGpuMs(), b, kGpuMsTextSize)); }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("SSR"))
    {
        // Own ID scope for this section, like the one its BeginMenu gave
        // before this was a panel: without it, a widget named like
        // the section collides with the header.
        ImGui::PushID("SSR");
            // SSR: global switch. The strength is PER GAMEOBJECT (Properties
            // panel), so with this on but no object marked
            // nothing is recorded either.
            ImGui::Separator();
            const bool ssr = m_ctl.checkbox("SSR",
                [rend] { return rend->ssrEnabled(); },
                [rend](bool v) { rend->setSsrEnabled(v); });

            ImGui::BeginDisabled(!ssr);
            m_ctl.sliderFloat("SSR distance", 0.5f, 50.0f, "%.1f",
                [rend] { return rend->ssrMaxDistance(); },
                [rend](float v) { rend->setSsrMaxDistance(v); });

            m_ctl.sliderFloat("SSR thickness", 0.01f, 3.0f, "%.2f",
                [rend] { return rend->ssrThickness(); },
                [rend](float v) { rend->setSsrThickness(v); });

            m_ctl.sliderInt("SSR steps", 8, 128,
                [rend] { return rend->ssrMaxSteps(); },
                [rend](int v) { rend->setSsrMaxSteps(v); });

            m_ctl.sliderFloat("SSR edge fade", 0.0f, 0.5f, "%.3f",
                [rend] { return rend->ssrEdgeFade(); },
                [rend](float v) { rend->setSsrEdgeFade(v); });

            m_ctl.sliderFloat("SSR intensity", 0.0f, 2.0f, "%.2f",
                [rend] { return rend->ssrIntensity(); },
                [rend](float v) { rend->setSsrIntensity(v); });
            ImGui::EndDisabled();

            { char b[kGpuMsTextSize];
                ImGui::Text("SSR GPU: %s ms", gpuMsText(rend->ssrGpuMs(), b, kGpuMsTextSize)); }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Fog"))
    {
        // Own ID scope for this section, like the one its BeginMenu gave
        // before this was a panel: without it, a widget named like
        // the section collides with the header.
        ImGui::PushID("Fog");
            // Volumetric fog: global switch, session setting (not
            // serialized) just like the bloom, the SSAO and the SSR. Off
            // leaves the image exactly as before the feature and the
            // GPU cost at zero.
            ImGui::Separator();
            const bool fog = m_ctl.checkbox("Volumetric Fog",
                [rend] { return rend->fogEnabled(); },
                [rend](bool v) { rend->setFogEnabled(v); });

            // As in SSAO and SSR: the sliders are not hidden with the
            // effect off, they are left disabled.
            ImGui::BeginDisabled(!fog);
            m_ctl.sliderFloat("Fog density", 0.0f, 0.5f, "%.3f",
                [rend] { return rend->fogDensity(); },
                [rend](float v) { rend->setFogDensity(v); });

            m_ctl.sliderFloat("Fog height falloff", 0.0f, 0.5f, "%.3f",
                [rend] { return rend->fogHeightFalloff(); },
                [rend](float v) { rend->setFogHeightFalloff(v); });

            m_ctl.sliderFloat("Fog base height", -50.0f, 50.0f, "%.1f",
                [rend] { return rend->fogBaseHeight(); },
                [rend](float v) { rend->setFogBaseHeight(v); });

            m_ctl.sliderFloat("Fog anisotropy", -0.95f, 0.95f, "%.2f",
                [rend] { return rend->fogAnisotropy(); },
                [rend](float v) { rend->setFogAnisotropy(v); });

            m_ctl.sliderInt("Fog steps", 8, 128,
                [rend] { return rend->fogSteps(); },
                [rend](int v) { rend->setFogSteps(v); });

            m_ctl.colorEdit3("Fog scattering",
                [rend] { return rend->fogScatter(); },
                [rend](const glm::vec3& v) { rend->setFogScatter(v); });
            ImGui::EndDisabled();

            { char b[kGpuMsTextSize];
                ImGui::Text("Fog GPU: %s ms", gpuMsText(rend->fogGpuMs(), b, kGpuMsTextSize)); }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Motion blur"))
    {
        // Own ID scope for this section, like the one its BeginMenu gave
        // before this was a panel: without it, a widget named like
        // the section collides with the header.
        ImGui::PushID("Motion blur");
            // Camera motion blur. Off by default: without it the image
            // is exactly the one from before the feature and not a single
            // dispatch is recorded. The velocity comes from reprojecting the depth to the
            // previous frame, so it blurs what the CAMERA moves; an
            // object that moves alone with the camera still leaves no trail.
            ImGui::Separator();
            const bool motionBlur = m_ctl.checkbox("Motion Blur",
                [rend] { return rend->motionBlurEnabled(); },
                [rend](bool v) { rend->setMotionBlurEnabled(v); });

            // As in SSAO, SSR and fog: the sliders are not hidden
            // with the effect off, they are left disabled.
            ImGui::BeginDisabled(!motionBlur);
            m_ctl.sliderFloat("Motion blur intensity", 0.0f, 4.0f, "%.2f",
                [rend] { return rend->motionBlurIntensity(); },
                [rend](float v) { rend->setMotionBlurIntensity(v); });

            m_ctl.sliderFloat("Motion blur max radius", 1.0f, 128.0f, "%.0f px",
                [rend] { return rend->motionBlurMaxRadius(); },
                [rend](float v) { rend->setMotionBlurMaxRadius(v); });

            m_ctl.sliderInt("Motion blur samples", 2, 32,
                [rend] { return rend->motionBlurSamples(); },
                [rend](int v) { rend->setMotionBlurSamples(v); });
            ImGui::EndDisabled();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Anti-aliasing"))
    {
        // Own ID scope for this section, like the one its BeginMenu gave
        // before this was a panel: without it, a widget named like
        // the section collides with the header.
        ImGui::PushID("Anti-aliasing");
            // Anti-aliasing. EXCLUSIVE modes, each with its own
            // parameters. Same criterion as the rest: session setting, it is
            // not serialized. On None not a single extra command is recorded and the
            // image is identical to the one from before the feature.
            ImGui::Separator();
            using AaMode = EditorRenderer::AaMode;
            const char* aaNames[] = { "None", "FXAA", "SSAA", "MSAA", "TAA" };
            int aaCurrent = (int)rend->aaMode();
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::Combo("Anti-aliasing", &aaCurrent, aaNames, IM_ARRAYSIZE(aaNames)))
            {
                // The previous value is given by the Combo itself: aaCurrent was read BEFORE
                // drawing it and the widget has just overwritten it, so
                // the earlier value has to be re-read from the renderer, which
                // has not changed yet.
                const AaMode antes = rend->aaMode();
                rend->setAaMode((AaMode)aaCurrent);
                // The mode NAME is saved, not this index: see
                // aaModeName() at the top of the file.
                m_ctl.pushUndo<AaMode>("Anti-aliasing", antes, (AaMode)aaCurrent,
                    [rend](const AaMode& v) { rend->setAaMode(v); });
            }

            const AaMode aaMode = rend->aaMode();

            if (aaMode == AaMode::Fxaa)
            {
                m_ctl.sliderFloat("FXAA subpixel", 0.0f, 1.0f, "%.2f",
                    [rend] { return rend->fxaaSubpix(); },
                    [rend](float v) { rend->setFxaaSubpix(v); });

                m_ctl.sliderFloat("FXAA edge threshold", 0.063f, 0.333f, "%.3f",
                    [rend] { return rend->fxaaEdgeThreshold(); },
                    [rend](float v) { rend->setFxaaEdgeThreshold(v); });

                m_ctl.sliderFloat("FXAA edge min", 0.0312f, 0.0833f, "%.4f",
                    [rend] { return rend->fxaaEdgeThresholdMin(); },
                    [rend](float v) { rend->setFxaaEdgeThresholdMin(v); });
            }
            else if (aaMode == AaMode::Ssaa)
            {
                // Changing the factor recreates ALL the internal targets, so
                // it is applied on releasing the slider and not on every dragged
                // pixel: rebuilding the whole render 60 times per
                // second while dragging would freeze the editor.
                // A member and not `static`: the static survived the project
                // change, and its refresh was guarded by IsAnyItemActive(),
                // which is GLOBAL: any other widget in use froze the
                // displayed value. Now it only freezes while THIS slider
                // is being dragged.
                if (!m_ssaaSliderActive) m_ssaaPendingFactor = rend->ssaaFactor();
                ImGui::SetNextItemWidth(140.0f);
                // FULL range: the core does not clamp and its default is 2.0, so
                // capping to [1.25, 2.0] left 1.0 (SSAA effectively off) and
                // everything above 2 out of the panel's reach.
                ImGui::SliderFloat("SSAA factor", &m_ssaaPendingFactor, 1.0f, 4.0f, "%.2fx");
                m_ssaaSliderActive = ImGui::IsItemActive();
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    // This is the only slider of the menu that does NOT need to
                    // remember where the drag started: the value is not
                    // applied until release, so the renderer still
                    // has the previous one right here.
                    const float antes = rend->ssaaFactor();
                    rend->setSsaaFactor(m_ssaaPendingFactor);
                    m_ctl.pushUndo<float>("SSAA factor", antes, m_ssaaPendingFactor,
                        [rend](const float& v) { rend->setSsaaFactor(v); });
                }
                ImGui::TextDisabled("%.2fx pixels per frame",
                                    m_ssaaPendingFactor * m_ssaaPendingFactor);
            }
            else if (aaMode == AaMode::Msaa)
            {
                const int maxSamples = rend->maxMsaaSamples();
                int samples = rend->msaaSamples();
                // Only the counts the device supports for
                // color AND depth at once are offered: the scene pass uses both.
                // From 1x: it is what the core accepts as "no multisampling"
                // and until now there was no way to choose it from here.
                for (int s = 1; s <= 8; s *= 2)
                {
                    if (s > maxSamples) break;
                    if (s > 1) ImGui::SameLine();
                    char label[8];
                    snprintf(label, sizeof(label), "%dx", s);
                    if (ImGui::RadioButton(label, samples == s))
                    {
                        // `samples` was read before the loop, that is, it is the
                        // value from before the click.
                        rend->setMsaaSamples(s);
                        m_ctl.pushUndo<int>("MSAA samples", samples, s,
                            [rend](const int& v) { rend->setMsaaSamples(v); });
                    }
                }
                // With maxSamples == 1 the loop draws nothing but the 1x, and
                // it is also worth saying why: the mode can still be chosen
                // but the device is not going to apply it.
                ImGui::SameLine();
                ImGui::TextDisabled("(max %dx)", maxSamples);
                if (maxSamples <= 1)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                       "This GPU does not support multisampling: MSAA will do nothing.");
            }
            else if (aaMode == AaMode::Taa)
            {
                m_ctl.sliderFloat("TAA feedback", 0.0f, 0.98f, "%.2f",
                    [rend] { return rend->taaFeedback(); },
                    [rend](float v) { rend->setTaaFeedback(v); });

                m_ctl.sliderFloat("TAA jitter", 0.0f, 2.0f, "%.2f",
                    [rend] { return rend->taaJitterScale(); },
                    [rend](float v) { rend->setTaaJitterScale(v); });
            }

            // The dedicated pass only exists in FXAA, SSAA and TAA. The cost of
            // MSAA and of supersampling is spread over the render, and
            // that is why the total is also shown: comparing it with that of
            // None gives the real overhead of the mode.
            { char b[kGpuMsTextSize];
                ImGui::Text("AA GPU: %s ms", gpuMsText(rend->aaGpuMs(), b, kGpuMsTextSize)); }
            { char b[kGpuMsTextSize];
                ImGui::Text("Render GPU: %s ms", gpuMsText(rend->renderGpuMs(), b, kGpuMsTextSize)); }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Forward+"))
    {
        // Own ID scope for this section, like the one its BeginMenu gave
        // before this was a panel: without it, a widget named like
        // the section collides with the header.
        ImGui::PushID("Forward+");
            // Forward+. EXCLUSIVE modes, like AA: on Off not a single
            // dispatch is recorded and pbr.frag walks the UBO lights as
            // always. Session setting, it is not serialized: this way the runtime and the
            // editor start in the same mode and render the same.
            ImGui::Separator();
            using FpMode = EditorRenderer::FpMode;
            const char* fpNames[] = { "Off", "Tiled", "Clustered" };
            int fpCurrent = (int)rend->forwardPlusMode();
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::Combo("Forward+", &fpCurrent, fpNames, IM_ARRAYSIZE(fpNames)))
            {
                // Same as in the AA Combo: the previous value is re-read from the
                // renderer, which has not changed yet.
                const FpMode antes = rend->forwardPlusMode();
                rend->setForwardPlusMode((FpMode)fpCurrent);
                m_ctl.pushUndo<FpMode>("Forward+", antes, (FpMode)fpCurrent,
                    [rend](const FpMode& v) { rend->setForwardPlusMode(v); });
            }

            // The light cut, stated. The scene can have as many as it
            // wants, but only the first MAX_LIGHTS reach the shader and
            // the rest was discarded SILENTLY: the scene looked worse
            // lit with nothing explaining it.
            //
            // It goes here, under Forward+, because it is precisely the feature that
            // promises to scale the number of lights and that this cap limits.
            {
                const size_t total = rend->sceneLightTotal();
                if (total > (size_t)MAX_LIGHTS)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                       "%zu lights in the scene, only %d contribute.",
                                       total, MAX_LIGHTS);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "The UBO block has room for MAX_LIGHTS lights and keeps "
                            "\nthe first ones in scene order. The rest neither light "
                            "\nnor cast shadows.\n\nRaising that cap requires recompiling "
                            "the shaders that declare the\nblock, so it is not a UI "
                            "setting.");
                }
            }

            if (rend->forwardPlusMode() != FpMode::Off)
            {
                // The radius is what makes the culling worth anything: with
                // a huge one every light falls in every cell and the list fills up.
                m_ctl.sliderFloat("Light radius", 50.0f, 5000.0f, "%.0f",
                    [rend] { return rend->forwardPlusLightRadius(); },
                    [rend](float v) { rend->setForwardPlusLightRadius(v); });

                { char b[kGpuMsTextSize];
                ImGui::Text("Forward+ GPU: %s ms", gpuMsText(rend->forwardPlusGpuMs(), b, kGpuMsTextSize)); }
                ImGui::Text("Lights/cell: %.1f", rend->forwardPlusAvgPerCell());
                const uint32_t overflow = rend->forwardPlusOverflowCells();
                if (overflow > 0)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                       "%u overflowing cells (they lose lights)", overflow);
            }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Backend de render"))
    {
        // Own ID scope for this section, like the one its BeginMenu gave
        // before this was a panel: without it, a widget named like
        // the section collides with the header.
        ImGui::PushID("Backend de render");


        // Render backend. The only setting of this menu that is NOT applied
        // when touched: device, swapchain and all the GPU resources hang
        // from the backend, so it can only change at startup. It is saved
        // in project.json and the editor starts with that of the last project
        // opened (ProjectContext::readLastProject).
        //
        // BOTH options are ALWAYS offered, even if this build does not bring
        // DX12 or the machine does not support it: the warning explains the reason and
        // startup falls back to Vulkan. Hiding the option would only leave the
        // user not knowing why it is not there.
        ImGui::Separator();
        const char* backendNames[] = { "Vulkan", "DirectX 12" };
        int backendCurrent = (int)selected;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::Combo("Render backend", &backendCurrent, backendNames,
                         IM_ARRAYSIZE(backendNames)))
        {
            // The NAME is saved, not this index: see renderBackendName().
            selected = (RenderBackend)backendCurrent;
            guardar();
            if (selected != active)
                log(std::string("Render backend changed to ") +
                                renderBackendName(selected) +
                                ": restart the editor to apply it");
        }

        if (selected != active)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "Requires a restart (now: %s)",
                               renderBackendName(active));
        else
            ImGui::TextDisabled("In use: %s", renderBackendName(active));
        ImGui::PopID();
    }


    ImGui::End();
}

} // namespace DonTopo
