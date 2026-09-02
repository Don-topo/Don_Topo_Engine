#include "DonTopo/Editor/RenderingPanel.h"

#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/GpuTimeFormat.h"
#include "DonTopo/Renderer/EditorRenderer.h"

#include <imgui.h>

namespace DonTopo {

void RenderingPanel::draw(EditorContext& ctx, RenderBackend active, RenderBackend& selected)
{
    if (!m_open) return;

    // Sin backend no hay nada que ajustar. El panel se dibuja igual —vacio y
    // diciendolo— en vez de desaparecer: un panel que se esfuma solo hace
    // pensar que se ha roto el layout.
    if (!ctx.renderer)
    {
        if (ImGui::Begin("Rendering", &m_open))
            ImGui::TextDisabled("Sin renderer activo.");
        ImGui::End();
        return;
    }

    // Los wrappers necesitan el UndoManager y el guardado del proyecto, que
    // llegan por el contexto. Se reenganchan cada frame; su estado de arrastre
    // vive en el objeto y sobrevive a esto.
    m_ctl.bind(ctx.undo, ctx.saveSettings);

    EditorRenderer* rend = ctx.renderer;
    auto guardar = [&] { if (ctx.saveSettings) ctx.saveSettings(); };
    auto log     = [&](const std::string& s) { if (ctx.pushLog) ctx.pushLog(s); };

    if (!ImGui::Begin("Rendering", &m_open))
    {
        ImGui::End();
        return;
    }

    if (ImGui::CollapsingHeader("Ambiente (IBL)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // Scope de IDs propio para esta seccion, como el que daba su BeginMenu
        // antes de que esto fuera un panel: sin el, un widget que se llame como
        // la seccion choca con la cabecera.
        ImGui::PushID("Ambiente (IBL)");
            const bool ambientOn = m_ctl.checkbox("Ambient (IBL)",
                [rend] { return rend->ambientEnabled(); },
                [rend](bool v) { rend->setAmbientEnabled(v); });

            // Igual que en el bloom: el slider no se oculta con el ambiente
            // apagado, se deja desactivado.
            ImGui::BeginDisabled(!ambientOn);
            // Se guarda al SOLTAR —y en ese mismo momento entra en el
            // historial—: arrastrar de punta a punta escribe una vez, no
            // una por frame. Mismo criterio en todos los sliders.
            m_ctl.sliderFloat("Ambient intensity", 0.0f, 3.0f, "%.2f",
                [rend] { return rend->ambientIntensity(); },
                [rend](float v) { rend->setAmbientIntensity(v); });
            ImGui::EndDisabled();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Sondas de reflexion"))
    {
        // Scope de IDs propio para esta seccion, como el que daba su BeginMenu
        // antes de que esto fuera un panel: sin el, un widget que se llame como
        // la seccion choca con la cabecera.
        ImGui::PushID("Sondas de reflexion");
            // Reflection probes: control GLOBAL (rehornear la escena entera).
            // El radio y la intensidad de cada sonda van en su Properties,
            // que es donde se edita lo que es de un objeto. El bake solo se
            // encola: lo ejecuta el Renderer al principio del frame
            // siguiente, nunca como un pass del frame.
            ImGui::Separator();
            const int probes = rend->probeCount();
            ImGui::BeginDisabled(probes == 0);
            if (ImGui::MenuItem("Bake All Reflection Probes"))
                rend->requestProbeBakeAll();
            ImGui::EndDisabled();
            // La cifra la da el BACKEND ACTIVO: los dos guardan cosas
            // distintas por sonda y antes se enseñaba siempre la de Vulkan
            // (H51).
            ImGui::Text("Sondas: %d  (%.2f MB c/u)", probes,
                        (double)rend->probeMemoryBytes() / (1024.0 * 1024.0));
            // Sin sondas no hay bake que contar, y un "0.00 ms" se lee como
            // horneado instantáneo en vez de como "nunca" (H56). Es la
            // misma distinción que ya hacía la sección Reflection Probe del
            // panel Properties con su "sin bakear".
            if (probes == 0)
                ImGui::TextDisabled("Ultimo bake: sin sondas en la escena");
            else if (rend->lastProbeBakeMs() <= 0.0f)
                ImGui::TextDisabled("Ultimo bake: sin bakear");
            else
                ImGui::Text("Ultimo bake: %.2f ms de GPU", rend->lastProbeBakeMs());
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Skybox"))
    {
        // Scope de IDs propio para esta seccion, como el que daba su BeginMenu
        // antes de que esto fuera un panel: sin el, un widget que se llame como
        // la seccion choca con la cabecera.
        ImGui::PushID("Skybox");
            // El cielo NO se edita desde aqui: sobre un menu de ImGui no se
            // puede soltar un arrastre —el popup se cierra al soltar fuera—,
            // asi que vive en su propia ventana. Aqui solo la entrada que la
            // abre, junto al ambiente porque el IBL global sale de
            // convolucionar ese mismo cubemap.
            ImGui::Separator();
            if (ImGui::MenuItem("Environment (skybox)..."))
                if (ctx.openEnvironment) ctx.openEnvironment();
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Presentacion (vsync)"))
    {
        // Scope de IDs propio para esta seccion, como el que daba su BeginMenu
        // antes de que esto fuera un panel: sin el, un widget que se llame como
        // la seccion choca con la cabecera.
        ImGui::PushID("Presentacion (vsync)");
            // Modo de presentación. Los que el device no da salen
            // DESHABILITADOS con su motivo, no escondidos: si el core
            // soporta N opciones la UI ofrece N, y el matiz se documenta.
            {
                const PresentMode kModos[] = { PresentMode::Vsync,
                                               PresentMode::Mailbox,
                                               PresentMode::Immediate };
                const char* kNombres[] = { "Vsync", "Mailbox", "Immediate" };
                // Dos lineas por modo: que hace, y que se paga por ello.
                const char* kQueHace[] = {
                    "Espera al refresco.",
                    "Triple buffer: ni espera ni rompe la imagen.",
                    "No espera al refresco.",
                };
                const char* kQueCuesta[] = {
                    "Sin tearing, pero clava los FPS a los del monitor.",
                    "Dibuja frames que se descartan. Solo lo da Vulkan.",
                    "Aparece tearing, y es el UNICO modo con el que se puede medir"
                    " el coste real de un frame: con Vsync todo sale a 16 ms.",
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
                            // El modo CONCEDIDO puede no ser el pedido (un
                            // device sin Mailbox cae a Vsync), así que el
                            // "after" se relee en vez de darlo por hecho:
                            // un undo debe volver a lo que de verdad hubo.
                            m_ctl.pushUndo<PresentMode>("Present mode", antes,
                                rend->presentMode(),
                                [rend](const PresentMode& v) { rend->setPresentMode(v); });
                        }
                        ImGui::EndDisabled();
                        // El tooltip va FUERA del BeginDisabled: un item
                        // deshabilitado no recibe hover, y es justo el que
                        // mas necesita explicar por que no se puede elegir.
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
                                    "No disponible en este equipo con el backend activo.");
                            }
                            ImGui::EndTooltip();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Sombras"))
    {
        // Scope de IDs propio para esta seccion, como el que daba su BeginMenu
        // antes de que esto fuera un panel: sin el, un widget que se llame como
        // la seccion choca con la cabecera.
        ImGui::PushID("Sombras");
            // Sombras en cascada. Los dos eran constantes de compilacion
            // hasta ahora, y son de lo que mas se nota: las 4 cascadas se
            // reparten "Shadow distance", asi que bajarla concentra los
            // mismos texeles en menos mundo y afila la sombra de cerca.
            // Resolución del mapa. Es la que da el salto más bruto (4096
            // cuadruplica los texeles de 2048), y la única de las tres que
            // mueve recursos: por eso va por el backend y no por el estado.
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
                    // Deshacer esto recrea el texture array otra vez. Es
                    // caro y es lo correcto: el usuario pidió volver.
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
                ImGui::SetTooltip("0 = cortes uniformes, 1 = logaritmicos.\n"
                                  "Alto da resolucion cerca; bajo reparte mas parejo.");

            { char b[kGpuMsTextSize];
                ImGui::Text("Sombras GPU: %s ms", gpuMsText(rend->shadowGpuMs(), b, kGpuMsTextSize)); }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Bloom"))
    {
        // Scope de IDs propio para esta seccion, como el que daba su BeginMenu
        // antes de que esto fuera un panel: sin el, un widget que se llame como
        // la seccion choca con la cabecera.
        ImGui::PushID("Bloom");
            // Bloom. Mismo criterio que el ambiente: ajuste de sesion, no se
            // serializa. Intensity 0 deja la imagen como antes del bloom.
            ImGui::Separator();
            const bool bloom = m_ctl.checkbox("Bloom",
                [rend] { return rend->bloomEnabled(); },
                [rend](bool v) { rend->setBloomEnabled(v); });

            // Igual que en el SSAO y el SSR: los sliders no se ocultan con el
            // efecto apagado, se dejan desactivados.
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
        // Scope de IDs propio para esta seccion, como el que daba su BeginMenu
        // antes de que esto fuera un panel: sin el, un widget que se llame como
        // la seccion choca con la cabecera.
        ImGui::PushID("SSAO");
            // SSAO. Mismo criterio que el ambiente y el bloom: ajuste de
            // sesion, no se serializa. Apagado deja la imagen exactamente
            // como antes de la feature y el coste GPU a cero.
            ImGui::Separator();
            const bool ssao = m_ctl.checkbox("SSAO",
                [rend] { return rend->ssaoEnabled(); },
                [rend](bool v) { rend->setSsaoEnabled(v); });

            // Los sliders no se ocultan con el efecto apagado: se dejan
            // desactivados para que se vea que existen y con que valores
            // arrancarian.
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
        // Scope de IDs propio para esta seccion, como el que daba su BeginMenu
        // antes de que esto fuera un panel: sin el, un widget que se llame como
        // la seccion choca con la cabecera.
        ImGui::PushID("SSR");
            // SSR: interruptor global. La fuerza es POR GAMEOBJECT (panel
            // Properties), asi que con esto puesto pero ningun objeto marcado
            // tampoco se graba nada.
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

    if (ImGui::CollapsingHeader("Niebla"))
    {
        // Scope de IDs propio para esta seccion, como el que daba su BeginMenu
        // antes de que esto fuera un panel: sin el, un widget que se llame como
        // la seccion choca con la cabecera.
        ImGui::PushID("Niebla");
            // Niebla volumetrica: interruptor global, ajuste de sesion (no
            // se serializa) igual que el bloom, el SSAO y el SSR. Apagada
            // deja la imagen exactamente como antes de la feature y el coste
            // GPU a cero.
            ImGui::Separator();
            const bool fog = m_ctl.checkbox("Volumetric Fog",
                [rend] { return rend->fogEnabled(); },
                [rend](bool v) { rend->setFogEnabled(v); });

            // Como en el SSAO y el SSR: los sliders no se ocultan con el
            // efecto apagado, se dejan desactivados.
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
        // Scope de IDs propio para esta seccion, como el que daba su BeginMenu
        // antes de que esto fuera un panel: sin el, un widget que se llame como
        // la seccion choca con la cabecera.
        ImGui::PushID("Motion blur");
            // Motion blur de camara. Apagado por defecto: sin el la imagen
            // es exactamente la de antes de la feature y no se graba ni un
            // dispatch. La velocidad sale de reproyectar la profundidad al
            // frame anterior, asi que emborrona lo que mueve la CAMARA; un
            // objeto que se mueve solo con la camara quieta no deja estela.
            ImGui::Separator();
            const bool motionBlur = m_ctl.checkbox("Motion Blur",
                [rend] { return rend->motionBlurEnabled(); },
                [rend](bool v) { rend->setMotionBlurEnabled(v); });

            // Como en el SSAO, el SSR y la niebla: los sliders no se ocultan
            // con el efecto apagado, se dejan desactivados.
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
        // Scope de IDs propio para esta seccion, como el que daba su BeginMenu
        // antes de que esto fuera un panel: sin el, un widget que se llame como
        // la seccion choca con la cabecera.
        ImGui::PushID("Anti-aliasing");
            // Anti-aliasing. Modos EXCLUYENTES, cada uno con sus propios
            // parametros. Mismo criterio que el resto: ajuste de sesion, no
            // se serializa. En None no se graba ni un comando de mas y la
            // imagen es identica a la de antes de la feature.
            ImGui::Separator();
            using AaMode = EditorRenderer::AaMode;
            const char* aaNames[] = { "None", "FXAA", "SSAA", "MSAA", "TAA" };
            int aaCurrent = (int)rend->aaMode();
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::Combo("Anti-aliasing", &aaCurrent, aaNames, IM_ARRAYSIZE(aaNames)))
            {
                // El previo lo da el propio Combo: aaCurrent se leyo ANTES
                // de dibujarlo y el widget lo acaba de sobrescribir, asi que
                // el valor de antes hay que releerlo del renderer, que
                // todavia no ha cambiado.
                const AaMode antes = rend->aaMode();
                rend->setAaMode((AaMode)aaCurrent);
                // Se guarda el NOMBRE del modo, no este indice: ver
                // aaModeName() al principio del fichero.
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
                // Cambiar el factor recrea TODOS los targets internos, asi
                // que se aplica al soltar el slider y no a cada pixel
                // arrastrado: reconstruir el render entero 60 veces por
                // segundo mientras se arrastra congelaria el editor.
                // Miembro y no `static`: el static sobrevivia al cambio de
                // proyecto, y su refresco estaba guardado por IsAnyItemActive(),
                // que es GLOBAL — cualquier otro widget en uso congelaba el
                // valor mostrado. Ahora solo se congela mientras se arrastra
                // ESTE slider.
                if (!m_ssaaSliderActive) m_ssaaPendingFactor = rend->ssaaFactor();
                ImGui::SetNextItemWidth(140.0f);
                // Rango COMPLETO: el core no clampea y su default es 2.0, asi que
                // capar a [1.25, 2.0] dejaba 1.0 (SSAA efectivamente apagado) y
                // todo lo que pasa de 2 fuera del alcance del panel.
                ImGui::SliderFloat("SSAA factor", &m_ssaaPendingFactor, 1.0f, 4.0f, "%.2fx");
                m_ssaaSliderActive = ImGui::IsItemActive();
                if (ImGui::IsItemDeactivatedAfterEdit())
                {
                    // Este es el unico slider del menu que NO necesita
                    // recordar donde empezo el arrastre: el valor no se
                    // aplica hasta soltar, asi que el renderer todavia
                    // tiene el de antes justo aqui.
                    const float antes = rend->ssaaFactor();
                    rend->setSsaaFactor(m_ssaaPendingFactor);
                    m_ctl.pushUndo<float>("SSAA factor", antes, m_ssaaPendingFactor,
                        [rend](const float& v) { rend->setSsaaFactor(v); });
                }
                ImGui::TextDisabled("%.2fx pixeles por frame",
                                    m_ssaaPendingFactor * m_ssaaPendingFactor);
            }
            else if (aaMode == AaMode::Msaa)
            {
                const int maxSamples = rend->maxMsaaSamples();
                int samples = rend->msaaSamples();
                // Solo se ofrecen las cuentas que soporta el device para
                // color Y profundidad a la vez: el pass de escena usa las dos.
                // Desde 1x: es lo que el core acepta como "sin multimuestra"
                // y hasta ahora no habia forma de elegirlo desde aqui.
                for (int s = 1; s <= 8; s *= 2)
                {
                    if (s > maxSamples) break;
                    if (s > 1) ImGui::SameLine();
                    char label[8];
                    snprintf(label, sizeof(label), "%dx", s);
                    if (ImGui::RadioButton(label, samples == s))
                    {
                        // `samples` se leyo antes del bucle, o sea que es el
                        // valor de antes del click.
                        rend->setMsaaSamples(s);
                        m_ctl.pushUndo<int>("MSAA samples", samples, s,
                            [rend](const int& v) { rend->setMsaaSamples(v); });
                    }
                }
                // Con maxSamples == 1 el bucle no dibuja nada mas que el 1x, y
                // ademas conviene decir por que: el modo se puede elegir igual
                // pero el device no lo va a aplicar.
                ImGui::SameLine();
                ImGui::TextDisabled("(max %dx)", maxSamples);
                if (maxSamples <= 1)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                       "Esta GPU no soporta multimuestra: MSAA no hara nada.");
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

            // El pass propio solo existe en FXAA, SSAA y TAA. El coste del
            // MSAA y el del supersampling estan repartidos en el render, y
            // por eso se muestra tambien el total: comparandolo con el de
            // None sale el sobrecoste real del modo.
            { char b[kGpuMsTextSize];
                ImGui::Text("AA GPU: %s ms", gpuMsText(rend->aaGpuMs(), b, kGpuMsTextSize)); }
            { char b[kGpuMsTextSize];
                ImGui::Text("Render GPU: %s ms", gpuMsText(rend->renderGpuMs(), b, kGpuMsTextSize)); }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Forward+"))
    {
        // Scope de IDs propio para esta seccion, como el que daba su BeginMenu
        // antes de que esto fuera un panel: sin el, un widget que se llame como
        // la seccion choca con la cabecera.
        ImGui::PushID("Forward+");
            // Forward+. Modos EXCLUYENTES, igual que el AA: en Off no se
            // graba ni un dispatch y pbr.frag recorre las luces del UBO como
            // siempre. Ajuste de sesion, no se serializa: asi el runtime y el
            // editor arrancan en el mismo modo y renderizan igual.
            ImGui::Separator();
            using FpMode = EditorRenderer::FpMode;
            const char* fpNames[] = { "Off", "Tiled", "Clustered" };
            int fpCurrent = (int)rend->forwardPlusMode();
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::Combo("Forward+", &fpCurrent, fpNames, IM_ARRAYSIZE(fpNames)))
            {
                // Igual que en el Combo del AA: el previo se relee del
                // renderer, que aun no ha cambiado.
                const FpMode antes = rend->forwardPlusMode();
                rend->setForwardPlusMode((FpMode)fpCurrent);
                m_ctl.pushUndo<FpMode>("Forward+", antes, (FpMode)fpCurrent,
                    [rend](const FpMode& v) { rend->setForwardPlusMode(v); });
            }

            // El recorte de luces, dicho. La escena puede tener las que
            // quiera, pero solo las primeras MAX_LIGHTS llegan al shader y
            // el resto se descartaba EN SILENCIO: la escena se veía peor
            // iluminada sin que nada lo explicara.
            //
            // Va aquí, bajo Forward+, porque es justo la feature que
            // promete escalar el número de luces y que este tope capa.
            {
                const size_t total = rend->sceneLightTotal();
                if (total > (size_t)MAX_LIGHTS)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                       "%zu luces en escena, solo %d iluminan.",
                                       total, MAX_LIGHTS);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "El bloque UBO tiene sitio para MAX_LIGHTS luces y se queda "
                            "con\nlas primeras en orden de escena. Las demas ni iluminan "
                            "ni\nproyectan sombra.\n\nSubir ese tope obliga a recompilar "
                            "los shaders que declaran el\nbloque, asi que no es un ajuste "
                            "de la UI.");
                }
            }

            if (rend->forwardPlusMode() != FpMode::Off)
            {
                // El radio es lo que hace que el culling sirva de algo: con
                // uno enorme toda luz cae en toda celda y la lista se llena.
                m_ctl.sliderFloat("Light radius", 50.0f, 5000.0f, "%.0f",
                    [rend] { return rend->forwardPlusLightRadius(); },
                    [rend](float v) { rend->setForwardPlusLightRadius(v); });

                { char b[kGpuMsTextSize];
                ImGui::Text("Forward+ GPU: %s ms", gpuMsText(rend->forwardPlusGpuMs(), b, kGpuMsTextSize)); }
                ImGui::Text("Luces/celda: %.1f", rend->forwardPlusAvgPerCell());
                const uint32_t overflow = rend->forwardPlusOverflowCells();
                if (overflow > 0)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                       "%u celdas desbordadas (pierden luces)", overflow);
            }
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("Backend de render"))
    {
        // Scope de IDs propio para esta seccion, como el que daba su BeginMenu
        // antes de que esto fuera un panel: sin el, un widget que se llame como
        // la seccion choca con la cabecera.
        ImGui::PushID("Backend de render");


        // Backend de render. El único ajuste de este menú que NO se aplica
        // al tocarlo: device, swapchain y todos los recursos de GPU cuelgan
        // del backend, así que solo puede cambiar en el arranque. Se guarda
        // en el project.json y el editor arranca con el del último proyecto
        // abierto (ProjectContext::readLastProject).
        //
        // Se ofrecen SIEMPRE las dos opciones, aunque este build no traiga
        // DX12 o la máquina no lo soporte: el aviso explica el motivo y el
        // arranque se cae a Vulkan. Esconder la opción solo dejaría al
        // usuario sin saber por qué no está.
        ImGui::Separator();
        const char* backendNames[] = { "Vulkan", "DirectX 12" };
        int backendCurrent = (int)selected;
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::Combo("Render backend", &backendCurrent, backendNames,
                         IM_ARRAYSIZE(backendNames)))
        {
            // Se guarda el NOMBRE, no este índice: ver renderBackendName().
            selected = (RenderBackend)backendCurrent;
            guardar();
            if (selected != active)
                log(std::string("Backend de render cambiado a ") +
                                renderBackendName(selected) +
                                ": reinicia el editor para aplicarlo");
        }

        if (selected != active)
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "Requiere reiniciar (ahora: %s)",
                               renderBackendName(active));
        else
            ImGui::TextDisabled("En uso: %s", renderBackendName(active));
        ImGui::PopID();
    }


    ImGui::End();
}

} // namespace DonTopo
