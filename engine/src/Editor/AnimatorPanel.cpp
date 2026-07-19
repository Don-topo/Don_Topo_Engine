#include "DonTopo/Editor/AnimatorPanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
#include <imgui.h>
#include <imgui_node_editor.h>
#include <ImGuiFileDialog.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ed = ax::NodeEditor;

namespace DonTopo {

namespace {
    // IDs del node editor: tienen que ser != 0 y no colisionar entre nodos,
    // pines y links. Tres slots por estado + un rango aparte pa los links.
    //
    // El slot se indexa por State::editorId (estable, asignado una vez en
    // AnimatorComponent::addState), NUNCA por el índice en el vector de
    // estados: ese índice cambia cuando removeState reindexa tras borrar un
    // estado de en medio, y si el id del canvas fuera el índice, un
    // superviviente heredaría el slot visual (posición/selección) del nodo
    // borrado — imgui-node-editor cachea esas cosas por id, no por contenido.
    int nodeId(int eid)      { return eid * 3 + 1; }
    int inputPinId(int eid)  { return eid * 3 + 2; }
    int outputPinId(int eid) { return eid * 3 + 3; }
    int linkId(int transIdx) { return 100000 + transIdx; }

    // Decodifica el editorId codificado en un id de nodo o de pin (la fórmula
    // es la misma división entera pa las tres variantes de nodeId/inputPinId/
    // outputPinId, así que un solo decode sirve pa las tres).
    int editorIdFromRawId(int rawId) { return (rawId - 1) / 3; }
    bool isOutputPin(int pin) { return (pin - 1) % 3 == 2; }

    // Un pin (o un nodo) solo trae el editorId estable, y las transiciones
    // guardan índices del vector m_states (no editorIds) porque ese es el
    // contrato de AnimatorComponent::Transition. Este helper hace el puente:
    // decodifica el editorId y escanea states() buscando quién lo tiene hoy.
    // Devuelve -1 si ningún estado vivo tiene ese id (no debería pasar: los
    // ids que llegan aquí vienen de nodos/pines dibujados este mismo frame a
    // partir de states() actual).
    int stateIndexFromPin(const AnimatorComponent& anim, int rawId)
    {
        const int eid = editorIdFromRawId(rawId);
        const auto& states = anim.states();
        for (size_t i = 0; i < states.size(); i++)
            if (states[i].editorId == eid) return (int)i;
        return -1;
    }

    const char* condLabel(AnimatorComponent::ConditionType t)
    {
        switch (t)
        {
            case AnimatorComponent::ConditionType::Trigger:           return "trigger";
            case AnimatorComponent::ConditionType::AnimationFinished: return "animation finished";
            case AnimatorComponent::ConditionType::Int:               return "int";
            case AnimatorComponent::ConditionType::Float:             return "float";
            default:                                                  return "bool";
        }
    }

    // Etiquetas de Compare en el orden del enum. Los cuatro se ofrecen tanto
    // para Int como para Float: el == sobre float solo dispara con igualdad
    // binaria exacta (un valor calculado casi nunca la cumple, uno puesto con
    // SetFloat sí), pero recortar el combo escondía media API sin avisar.
    const char* kCompareLabels[] = { ">", "<", "==", "!=" };
}

AnimatorPanel::AnimatorPanel()
{
    ed::Config config;
    // Sin fichero de settings: las posiciones de los nodos viven en el JSON de
    // escena (AnimatorComponent::State::editorPos). Si lo dejáramos por defecto,
    // el node editor escribiría un NodeEditor.json paralelo y habría dos fuentes
    // de verdad peleándose.
    config.SettingsFile = nullptr;
    m_ctx = ed::CreateEditor(&config);
    m_animSrcDialog = std::make_unique<IGFD::FileDialog>();
}

AnimatorPanel::~AnimatorPanel()
{
    if (m_ctx) ed::DestroyEditor(m_ctx);
}

void AnimatorPanel::syncPositionsFromComponent(GameObject* go)
{
    const auto& states = go->getAnimator()->states();
    for (size_t i = 0; i < states.size(); i++)
        ed::SetNodePosition(nodeId(states[i].editorId), ImVec2(states[i].editorPos.x, states[i].editorPos.y));
}

void AnimatorPanel::syncPositionsToComponent(GameObject* go)
{
    auto& states = go->getAnimator()->statesMutable();
    for (size_t i = 0; i < states.size(); i++)
    {
        const ImVec2 p = ed::GetNodePosition(nodeId(states[i].editorId));
        states[i].editorPos = glm::vec2(p.x, p.y);
    }
}

void AnimatorPanel::drawParameterList(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();

    // 260 y no 200: con el widget de valor de int/float al lado del nombre, a
    // 200 el DragFloat se comía el botón de borrado.
    ImGui::BeginChild("params", ImVec2(260, 0), true);
    ImGui::TextUnformatted("Parameters");
    ImGui::Separator();

    std::string toRemove;
    for (const auto& p : anim->parameters())
    {
        ImGui::PushID(p.name.c_str());
        ImGui::TextUnformatted(p.name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", paramTypeLabel(p.type));
        ImGui::SameLine();

        // Valor editable in situ: en Play permite provocar una transición a mano
        // sin escribir Lua, que es como se depura un grafo.
        ImGui::SetNextItemWidth(70);
        switch (p.type)
        {
            case AnimatorComponent::ParamType::Bool:
            {
                bool v = anim->getBool(p.name);
                if (ImGui::Checkbox("##val", &v)) anim->setBool(p.name, v);
                break;
            }
            case AnimatorComponent::ParamType::Trigger:
                // Un trigger no tiene valor que mostrar: se arma y lo consume la
                // primera transición que lo mire (ver consumeTriggers).
                if (ImGui::SmallButton("Set")) anim->setTrigger(p.name);
                break;
            case AnimatorComponent::ParamType::Int:
            {
                int v = anim->getInt(p.name);
                if (ImGui::DragInt("##val", &v)) anim->setInt(p.name, v);
                break;
            }
            case AnimatorComponent::ParamType::Float:
            {
                float v = anim->getFloat(p.name);
                if (ImGui::DragFloat("##val", &v, 0.01f)) anim->setFloat(p.name, v);
                break;
            }
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("X")) toRemove = p.name;
        ImGui::PopID();
    }
    // Diferido: borrar dentro del for-range invalidaría el iterador.
    if (!toRemove.empty())
    {
        anim->removeParameter(toRemove);
        ctx.pushLog("Animator: parámetro '" + toRemove + "' eliminado");
    }

    ImGui::Separator();
    ImGui::SetNextItemWidth(110);
    ImGui::InputText("##newparam", m_newParamName, sizeof(m_newParamName));
    // El orden coincide con el del enum ParamType, así que el índice del combo
    // castea directo: si el enum crece, esta lista crece con él.
    const char* types[] = { "bool", "trigger", "int", "float" };
    ImGui::SetNextItemWidth(110);
    ImGui::Combo("##newparamtype", &m_newParamType, types, IM_ARRAYSIZE(types));
    if (ImGui::Button("Add Parameter") && m_newParamName[0] != '\0')
    {
        anim->addParameter(m_newParamName, (AnimatorComponent::ParamType)m_newParamType);
        ctx.pushLog(std::string("Animator: parámetro '") + m_newParamName + "' añadido");
        m_newParamName[0] = '\0';
    }

    ImGui::EndChild();
}

void AnimatorPanel::drawGraph(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();

    ed::SetCurrentEditor(m_ctx);
    ed::Begin("AnimatorCanvas");

    // --- Nodos ---
    const auto& states = anim->states();
    for (size_t i = 0; i < states.size(); i++)
    {
        const int eid = states[i].editorId;
        ed::BeginNode(nodeId(eid));

        // Cabecera (nombre/entry, clip, loop) agrupada para poder medir su ancho
        // y así saber dónde cae el borde derecho del nodo: la fila de pines de
        // abajo lo necesita para separar "-> in" (izquierda) de "out ->" (derecha).
        ImGui::BeginGroup();

        const bool isEntry = ((int)i == anim->entryState());
        if (isEntry)
        {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", states[i].name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(entry)");
        }
        else
        {
            ImGui::TextUnformatted(states[i].name.c_str());
        }

        // clipIndex < 0: el clip del grafo no existe en el modelo (bindClips ya
        // avisó al cargar). Se marca aquí también o el nodo mentiría.
        if (states[i].clipIndex < 0)
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "clip: %s (no existe)", states[i].clipName.c_str());
        else
            ImGui::TextDisabled("clip: %s", states[i].clipName.c_str());

        ImGui::PushID((int)i);
        bool loop = states[i].loop;
        if (ImGui::Checkbox("loop", &loop))
            anim->statesMutable()[i].loop = loop;
        ImGui::PopID();

        ImGui::EndGroup();
        const float headerW = ImGui::GetItemRectSize().x;

        // "-> in" a la izquierda, "out ->" empujado al borde derecho del nodo
        // (el ancho de la cabecera es el ancho real del nodo: el contenido más
        // ancho de las líneas de arriba). Antes ambos pines iban en la misma fila
        // sin separación y quedaban apelotonados en la esquina inferior
        // izquierda.
        ed::BeginPin(inputPinId(eid), ed::PinKind::Input);
        ImGui::TextUnformatted("-> in");
        ed::EndPin();
        ImGui::SameLine();
        const float inW  = ImGui::CalcTextSize("-> in").x;
        const float outW = ImGui::CalcTextSize("out ->").x;
        const float pad  = headerW - inW - outW;
        // Nodo muy estrecho (nombre/clip corto): un hueco fijo pequeño basta pa
        // que los pines no se solapen, aunque ya no queden pegados al borde.
        ImGui::Dummy(ImVec2(pad > 1.0f ? pad : 8.0f, 0.0f));
        ImGui::SameLine();
        ed::BeginPin(outputPinId(eid), ed::PinKind::Output);
        ImGui::TextUnformatted("out ->");
        ed::EndPin();

        ed::EndNode();
    }

    // --- Links ---
    const auto& transitions = anim->transitions();
    for (size_t t = 0; t < transitions.size(); t++)
    {
        const int from = transitions[t].fromState;
        const int to   = transitions[t].toState;
        // fromState/toState son índices del vector m_states (no editorIds: ese
        // es el contrato de Transition, ver comentario en el header). Hay que
        // convertirlos a editorId antes de construir los ids de pin del canvas.
        if (from < 0 || from >= (int)states.size() || to < 0 || to >= (int)states.size()) continue;
        ed::Link(linkId((int)t),
                 outputPinId(states[from].editorId),
                 inputPinId(states[to].editorId));
    }

    // --- Crear links arrastrando de pin a pin ---
    if (ed::BeginCreate())
    {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && a && b)
        {
            const int pa = (int)a.Get();
            const int pb = (int)b.Get();
            // El usuario puede arrastrar en cualquier dirección: se normaliza a
            // (salida -> entrada).
            const int outPin = isOutputPin(pa) ? pa : pb;
            const int inPin  = isOutputPin(pa) ? pb : pa;

            if (isOutputPin(outPin) && !isOutputPin(inPin) && ed::AcceptNewItem())
            {
                // stateFromPin (índice) en vez de directamente el editorId: las
                // transiciones guardan índices del vector, no editorIds.
                const int fromIdx = stateIndexFromPin(*anim, outPin);
                const int toIdx   = stateIndexFromPin(*anim, inPin);
                if (fromIdx >= 0 && toIdx >= 0)
                {
                    AnimatorComponent::Transition tr;
                    tr.fromState = fromIdx;
                    tr.toState   = toIdx;
                    // Sin condiciones no dispara nunca (por diseño): el usuario las
                    // añade con doble clic en el link.
                    anim->addTransition(tr);
                    ctx.pushLog("Animator: transición creada (sin condiciones todavía)");
                }
            }
        }
    }
    ed::EndCreate();

    // --- Borrar nodos y links ---
    if (ed::BeginDelete())
    {
        // Con box-select + Supr, una sola pasada de BeginDelete puede traer
        // varios ids (varios QueryDeletedNode/QueryDeletedLink). Si se borrara
        // cada uno según se acepta, el primer erase reindexa el vector y los
        // ids ya encolados (calculados antes de ese erase) pasan a apuntar a
        // otro elemento o se salen de rango — removeState/removeTransition
        // hacen bounds-check silencioso y ese elemento sobrevive sin más aviso.
        // Por eso se recogen todos los índices primero y se borran después, de
        // atrás hacia adelante: así cada erase solo desplaza índices ya
        // procesados, nunca los que quedan pendientes en esta misma pasada.
        std::vector<int> transitionsToRemove;
        ed::LinkId dl;
        while (ed::QueryDeletedLink(&dl))
            if (ed::AcceptDeletedItem())
                transitionsToRemove.push_back((int)dl.Get() - 100000);

        // El id de nodo que trae QueryDeletedNode es un nodeId(editorId): se
        // decodifica a editorId y se resuelve al índice actual del vector
        // escaneando por editorId (stateIndexFromPin sirve igual aquí pese al
        // nombre — decode + scan es exactamente lo mismo pa un id de nodo que
        // pa uno de pin, la fórmula es la misma división entera).
        std::vector<int> statesToRemove;
        ed::NodeId dn;
        while (ed::QueryDeletedNode(&dn))
            if (ed::AcceptDeletedItem())
            {
                const int idx = stateIndexFromPin(*anim, (int)dn.Get());
                if (idx >= 0) statesToRemove.push_back(idx);
            }

        std::sort(transitionsToRemove.rbegin(), transitionsToRemove.rend());
        for (int idx : transitionsToRemove)
            anim->removeTransition(idx);

        // Se borran los links explícitamente pedidos antes que los estados: un
        // estado borrado se lleva también sus transiciones (removeState las
        // reindexa/purga), así que procesar los links sueltos primero evita
        // pisarse con esa purga automática. El descending-erase sigue siendo
        // necesario con multi-delete: cada removeState desplaza los índices por
        // encima de idx, así que hay que ir de atrás hacia adelante pa que cada
        // erase no invalide los índices ya calculados y pendientes en este mismo
        // vector (statesToRemove son índices tomados ANTES de borrar nada).
        std::sort(statesToRemove.rbegin(), statesToRemove.rend());
        for (int idx : statesToRemove)
            // removeState reindexa las transiciones supervivientes.
            anim->removeState(idx);

        // A diferencia de antes (Task 10), YA NO hace falta desvincular
        // m_boundTo aquí: con ids estables por editorId, un superviviente no
        // cambia de id al reindexarse el vector, así que su nodo en el canvas
        // sigue siendo el mismo nodo (mismo id) con su misma posición — no hay
        // slot visual que heredar del borrado. syncPositionsToComponent de más
        // abajo puede leer las posiciones este mismo frame sin corromper nada.
    }
    ed::EndDelete();

    // --- Menús contextuales ---
    ed::Suspend();
    ed::NodeId ctxNode;
    ed::LinkId ctxLink;
    if (ed::ShowNodeContextMenu(&ctxNode))
    {
        ImGui::OpenPopup("node_ctx");
        m_conditionsFor = -1;
        // Miembro plano en vez de ImGui::GetStateStorage(): un solo popup de
        // nodo puede estar abierto a la vez, así que no hace falta la
        // indirección de la state storage de ImGui pa smuggle-ar el índice
        // hasta el popup diferido — un int en el panel llega igual de lejos.
        // ctxNode.Get() decodifica a un editorId, no al índice del vector que
        // "Set as Entry" necesita (setEntryState(idx)) — de ahí el paso por
        // stateIndexFromPin.
        m_nodeCtxTarget = stateIndexFromPin(*anim, (int)ctxNode.Get());
    }
    else if (ed::ShowLinkContextMenu(&ctxLink))
    {
        // Sin popup intermedio "Edit Conditions...": abrir "conditions" desde
        // dentro del BeginPopup/EndPopup de otro popup (link_ctx) anidaba un
        // OpenPopup dentro de otro, frágil y flaky. Un click derecho en el link
        // abre el editor de condiciones directamente.
        m_conditionsFor = (int)ctxLink.Get() - 100000;
        ImGui::OpenPopup("conditions");
    }

    if (ImGui::BeginPopup("node_ctx"))
    {
        const int idx = m_nodeCtxTarget;
        if (idx >= 0 && idx < (int)anim->states().size())
        {
            if (ImGui::MenuItem("Set as Entry"))
            {
                anim->setEntryState(idx);
                ctx.pushLog("Animator: '" + anim->states()[idx].name + "' es ahora el estado de entrada");
            }
        }
        ImGui::EndPopup();
    }
    drawConditionsPopup(ctx, go);
    ed::Resume();

    ed::End();
    ed::SetCurrentEditor(nullptr);
}

void AnimatorPanel::drawConditionsPopup(EditorContext& ctx, GameObject* go)
{
    auto anim = go->getAnimator();
    if (m_conditionsFor < 0 || m_conditionsFor >= (int)anim->transitions().size()) return;

    if (!ImGui::BeginPopup("conditions")) return;

    auto& tr = anim->transitionsMutable()[m_conditionsFor];
    ImGui::TextUnformatted("Conditions (AND)");
    ImGui::Separator();

    int toRemove = -1;
    for (size_t c = 0; c < tr.conditions.size(); c++)
    {
        ImGui::PushID((int)c);
        auto& cond = tr.conditions[c];
        ImGui::Text("%s", condLabel(cond.type));
        if (cond.type != AnimatorComponent::ConditionType::AnimationFinished)
        {
            ImGui::SameLine();
            ImGui::TextUnformatted(cond.paramName.c_str());
        }
        if (cond.type == AnimatorComponent::ConditionType::Bool)
        {
            ImGui::SameLine();
            ImGui::Checkbox("expected", &cond.expected);
        }
        if (cond.type == AnimatorComponent::ConditionType::Int ||
            cond.type == AnimatorComponent::ConditionType::Float)
        {
            const bool isFloat = cond.type == AnimatorComponent::ConditionType::Float;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50);
            int op = (int)cond.compare;
            // Los cuatro comparadores para ambos tipos: el evaluador siempre
            // los soportó (ver AnimatorComponent::conditionsMet) y recortar el
            // combo a 2 para Float solo servía para esconderlos.
            if (ImGui::Combo("##cmp", &op, kCompareLabels, IM_ARRAYSIZE(kCompareLabels)))
                cond.compare = (AnimatorComponent::Compare)op;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70);
            if (isFloat)
            {
                ImGui::DragFloat("##thr", &cond.threshold, 0.01f);
            }
            else
            {
                // El umbral vive en float pa no duplicar el campo; la UI de Int
                // pasa por un int temporal, así que por aquí nunca entra un
                // valor con parte fraccionaria y lo que se ve es exactamente lo
                // que evalúa conditionsMet (que redondea, no trunca, pa cubrir
                // los que llegan de un JSON editado a mano).
                int thr = (int)cond.threshold;
                if (ImGui::DragInt("##thr", &thr)) cond.threshold = (float)thr;
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) toRemove = (int)c;
        ImGui::PopID();
    }
    if (toRemove >= 0) tr.conditions.erase(tr.conditions.begin() + toRemove);

    ImGui::Separator();
    // Solo se ofrecen parámetros ya declarados: una condición sobre un parámetro
    // inexistente no dispararía nunca y no habría forma de saber por qué.
    for (const auto& p : anim->parameters())
    {
        ImGui::PushID(p.name.c_str());
        // Selectable con DontClosePopups en vez de MenuItem: un MenuItem cierra
        // el popup al primer click y el usuario solo podría añadir una condición
        // por apertura. Con esto puede encadenar varias "Add:" seguidas.
        if (ImGui::Selectable(("Add: " + p.name).c_str(), false, ImGuiSelectableFlags_DontClosePopups))
        {
            AnimatorComponent::Condition cond;
            // El tipo del parámetro decide el de la condición 1:1.
            switch (p.type)
            {
                case AnimatorComponent::ParamType::Trigger:
                    cond.type = AnimatorComponent::ConditionType::Trigger; break;
                case AnimatorComponent::ParamType::Int:
                    cond.type = AnimatorComponent::ConditionType::Int;     break;
                case AnimatorComponent::ParamType::Float:
                    cond.type = AnimatorComponent::ConditionType::Float;   break;
                default:
                    cond.type = AnimatorComponent::ConditionType::Bool;    break;
            }
            cond.paramName = p.name;
            cond.expected  = true;
            tr.conditions.push_back(cond);
        }
        ImGui::PopID();
    }
    // "animation finished" nunca dispara saliendo de un estado en loop (un
    // clip en loop nunca "termina", ver AnimatorComponent::update): se deja
    // el item pero deshabilitado, con tooltip, para que el usuario no arme un
    // link muerto sin saberlo.
    const bool fromLoops = tr.fromState >= 0 && tr.fromState < (int)anim->states().size()
                            && anim->states()[tr.fromState].loop;
    ImGui::BeginDisabled(fromLoops);
    if (ImGui::Selectable("Add: animation finished", false, ImGuiSelectableFlags_DontClosePopups))
    {
        AnimatorComponent::Condition cond;
        cond.type = AnimatorComponent::ConditionType::AnimationFinished;
        tr.conditions.push_back(cond);
    }
    ImGui::EndDisabled();
    if (fromLoops && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("El estado de origen está en loop: 'animation finished' nunca dispara aquí (un clip en loop nunca termina).");

    ImGui::EndPopup();
}

void AnimatorPanel::importAnimationSource(EditorContext& ctx, GameObject* go, const std::string& path)
{
    SkinnedMesh* mesh = go->getSkinnedMesh();
    if (!mesh) return;

    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".fbx")
    {
        m_animSrcError = "Formato no soportado: " + ext;
        return;
    }

    // Ensayo en seco sobre una copia: el comando no puede decir "no" a medias
    // (AnimationSourceCommand::applyAdd descarta los warnings de
    // addAnimationSource y simplemente no hace nada si falla), y así el error
    // del loader (rig equivocado, fichero sin animaciones) llega al usuario
    // antes de meter nada en el stack de undo.
    SkinnedMesh probe = *mesh;
    std::vector<std::string> warnings;
    const bool ok = addAnimationSource(probe, path, warnings);
    for (const auto& w : warnings) ctx.pushLog("Animator: " + w);

    if (!ok)
    {
        m_animSrcError = warnings.empty() ? ("No se pudieron importar animaciones de " + path)
                                           : warnings.back();
        return;
    }
    m_animSrcError.clear();

    auto cmd = std::make_unique<AnimationSourceCommand>(
        *ctx.scene, ctx.renderer, "Añadir animaciones", go->id,
        /*add=*/true, path, std::vector<std::string>{});
    cmd->execute();
    ctx.undo->push(std::move(cmd));
    ctx.pushLog("Animator: animaciones de '" + path + "' importadas");
}

void AnimatorPanel::drawAnimationSources(EditorContext& ctx, GameObject* go)
{
    SkinnedMesh* mesh = go->getSkinnedMesh();
    if (!mesh) return;

    if (!ImGui::CollapsingHeader("Animation Sources", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    int sourceToRemove = -1;
    for (size_t s = 0; s < mesh->animationSources.size(); s++)
    {
        const AnimationSource& src = mesh->animationSources[s];
        // El path como ID en vez del índice s: quitar una fuente reindexa el
        // vector, y si el ID fuera el índice, cada fila posterior heredaría
        // el estado abierto/cerrado (y el m_renamingClip en vuelo, si lo
        // hubiera) de la fila que ocupaba su índice antes del borrado. El
        // path es estable mientras la fuente exista.
        ImGui::PushID(src.path.c_str());

        const std::string file = std::filesystem::path(src.path).filename().string();
        const std::string label = file + "  (" + std::to_string(src.clipNames.size()) + " clips)"
                                + (src.builtin ? "  [modelo]" : "");

        const bool open = ImGui::TreeNodeEx("##src", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", label.c_str());

        // La fuente builtin es el FBX del modelo: quitarla dejaría la malla sin
        // el fichero que la creó, así que el botón existe pero deshabilitado
        // (mostrarlo y explicarlo enseña la regla; ocultarlo la esconde).
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 20.0f);
        ImGui::BeginDisabled(src.builtin);
        if (ImGui::SmallButton("X")) sourceToRemove = (int)s;
        ImGui::EndDisabled();
        if (src.builtin && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Es el FBX del modelo: sus animaciones no se pueden quitar por separado.");

        if (open)
        {
            for (const std::string& clipName : src.clipNames)
            {
                ImGui::PushID(clipName.c_str());
                if (m_renamingClip == clipName)
                {
                    ImGui::SetNextItemWidth(180.0f);
                    if (ImGui::InputText("##rename", m_renameBuf, sizeof(m_renameBuf),
                                          ImGuiInputTextFlags_EnterReturnsTrue))
                    {
                        const std::string nuevo = m_renameBuf;

                        // Validación ANTES de ejecutar nada, replicando exactamente
                        // las reglas de rechazo de renameClip (ver
                        // SkinnedMeshAnimations.cpp): nombre vacío, nombre ya usado
                        // por cualquier clip, o nombre idéntico al actual. Antes se
                        // ejecutaba el comando primero y se inferÍa el éxito
                        // escaneando el mesh por el nombre nuevo — eso no podía
                        // distinguir "el rename se aplicó" de "ya existía un clip
                        // con ese nombre", que es justo el motivo por el que
                        // renameClip lo rechaza: un duplicado (p.ej. renombrar
                        // "Walk" a "Idle" cuando "Idle" ya existe) se colaba en el
                        // undo stack como si hubiera funcionado, y el siguiente
                        // Ctrl+Z del usuario no deshacía nada porque el comando no
                        // había mutado nada.
                        bool rechazado = nuevo.empty() || nuevo == clipName;
                        if (!rechazado)
                            for (const auto& c : mesh->animationClips)
                                if (c.name == nuevo) { rechazado = true; break; }

                        if (rechazado)
                        {
                            m_animSrcError = "No se pudo renombrar a '" + nuevo
                                            + "': nombre vacío, duplicado o igual al actual";
                        }
                        else
                        {
                            // Copia del nombre viejo ANTES de execute(): clipName
                            // es una const std::string& que apunta directo al
                            // elemento de src.clipNames, y renameClip lo reescribe
                            // in-place — tras execute() clipName ya vale "nuevo",
                            // así que usarla en el log duplicaría el nombre nuevo
                            // en vez de mostrar qué cambió.
                            const std::string viejo = clipName;
                            auto cmd = std::make_unique<ClipRenameCommand>(
                                *ctx.scene, "Renombrar clip", go->id, viejo, nuevo);
                            cmd->execute();
                            ctx.undo->push(std::move(cmd));
                            m_animSrcError.clear();
                            ctx.pushLog("Animator: clip '" + viejo + "' renombrado a '" + nuevo + "'");
                        }
                        m_renamingClip.clear();
                    }
                    // Clic fuera sin pulsar Enter (InputTextFlags_EnterReturnsTrue
                    // no dispara ahí): se cierra el modo edición sin tocar nada,
                    // ni el mesh ni el undo stack — el usuario se arrepintió.
                    if (ImGui::IsItemDeactivated()) m_renamingClip.clear();
                }
                else
                {
                    ImGui::BulletText("%s", clipName.c_str());
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
                    {
                        m_renamingClip = clipName;
                        std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", clipName.c_str());
                    }
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    if (sourceToRemove >= 0)
    {
        // Diferido fuera del for: mutar animationSources dentro del propio
        // bucle que lo recorre invalidaría el iterador de este mismo frame.
        const AnimationSource& src = mesh->animationSources[(size_t)sourceToRemove];
        auto cmd = std::make_unique<AnimationSourceCommand>(
            *ctx.scene, ctx.renderer, "Quitar animaciones", go->id,
            /*add=*/false, src.path, src.clipNames);
        cmd->execute();
        ctx.undo->push(std::move(cmd));
        // Los estados que usaran esos clips quedan huérfanos a propósito: el
        // grafo es trabajo del usuario y borrarlo por él sería peor que dejarlo
        // avisado. bindClips los reporta en la siguiente vinculación.
        ctx.pushLog("Animator: fuente de animación quitada; los estados que la usaran quedan sin clip");
    }

    if (ImGui::Button("Add Animation FBX..."))
    {
        IGFD::FileDialogConfig cfg;
        // "assets", como el diálogo de malla de PropertiesPanel: los FBX de
        // este proyecto viven ahí, y abrir en la raíz del repo obligaría a
        // navegar cada vez.
        cfg.path = "assets";
        cfg.flags = ImGuiFileDialogFlags_HideColumnType |
                    ImGuiFileDialogFlags_HideColumnDate |
                    ImGuiFileDialogFlags_DisableThumbnailMode |
                    ImGuiFileDialogFlags_DisablePlaceMode;
        m_animSrcDialog->OpenDialog("AddAnimSrcDlg", "Choose Animation FBX", ".fbx", cfg);
        m_animSrcDlgOpen = true;
        // Se captura el id AHORA, al abrir, no ctx.selected al drenar: el
        // diálogo no es modal, así que el usuario puede cambiar de selección
        // mientras elige el fichero — el FBX debe ir a "go" (a quien estaba
        // seleccionado al pulsar el botón), no a quien sea que esté
        // seleccionado cuando el usuario por fin cierra el diálogo.
        m_animSrcDlgTarget = go->id;
    }

    // Drop target: el Content Browser emite "DT_ASSET_PATH" (ver
    // ContentBrowserPanel::BeginDragDropSource), no "CONTENT_BROWSER_ITEM" —
    // mismo id y mismo patrón (payload->Data es char* terminado en '\0',
    // tamaño = fullPath.size()+1) que usa PropertiesPanel::drawMeshSection
    // para su propio drop target de .fbx.
    ImGui::SameLine();
    ImGui::TextDisabled("(o arrastra un .fbx aquí)");
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("DT_ASSET_PATH"))
            importAnimationSource(ctx, go, std::string(static_cast<const char*>(payload->Data)));
        ImGui::EndDragDropTarget();
    }

    if (!m_animSrcError.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_animSrcError.c_str());

    ImGui::Separator();
}

void AnimatorPanel::drawAnimationSourceDialog(EditorContext& ctx)
{
    if (!m_animSrcDlgOpen) return;
    if (!m_animSrcDialog->Display("AddAnimSrcDlg")) return;

    if (m_animSrcDialog->IsOk())
    {
        // Resuelve por id (capturado al abrir el diálogo, ver OpenDialog más
        // arriba), no ctx.selected: para cuando el usuario cierra el diálogo
        // la selección puede haber cambiado, y el objeto original puede
        // incluso haberse borrado. Sin este resuelto explícito, un guard tipo
        // "ctx.selected && ctx.selected->hasAnimator()" habría importado el
        // FBX en el GameObject equivocado sin avisar, o lo habría descartado
        // en silencio si el actualmente seleccionado no tuviera Animator.
        GameObject* target = ctx.scene ? ctx.scene->findById(m_animSrcDlgTarget) : nullptr;
        if (!target)
        {
            m_animSrcError = "El GameObject de destino ya no existe en la escena";
        }
        else if (!target->getSkinnedMesh())
        {
            m_animSrcError = "'" + target->name + "' ya no tiene un mesh skinned";
        }
        else
        {
            importAnimationSource(ctx, target, m_animSrcDialog->GetFilePathName());
        }
    }

    m_animSrcDialog->Close();
    m_animSrcDlgOpen = false;
}

void AnimatorPanel::draw(EditorContext& ctx)
{
    // El cuerpo va en un bloque, no en early-returns: drawAnimationSourceDialog()
    // de más abajo tiene que ejecutarse SIEMPRE, tanto si el panel está cerrado
    // (m_open == false, p.ej. tras pulsar la X de la ventana, que Begin escribe
    // directo en m_open) como si Begin devuelve false (ventana colapsada). Con
    // los early-returns de antes, cerrar o colapsar el panel mientras el
    // diálogo de fichero estaba abierto dejaba m_animSrcDlgOpen (y el estado
    // interno de IGFD) atascados en true para siempre: el diálogo resucitaba
    // solo, sin pedirlo, al reabrir el panel. Begin/End se llaman siempre en
    // pareja pase lo que pase (regla de ImGui), de ahí el End() incondicional
    // dentro del if(m_open). Mismo patrón que PropertiesPanel::draw +
    // drawMeshDialog.
    if (m_open)
    {
        if (ImGui::Begin("Animator", &m_open))
        {
            GameObject* go = ctx.selected;
            if (!go || !go->hasAnimator())
            {
                ImGui::TextDisabled("Selecciona un GameObject con componente Animator.");
                ImGui::TextDisabled("Properties > Add > Animator");
                m_boundTo = nullptr;
            }
            else
            {
                // Cambio de objeto vinculado: se comprueba UNA vez aquí arriba
                // (antes de dibujar nada) para poder limpiar m_renamingClip antes
                // de que drawAnimationSources lo lea. Si no, un clip con el mismo
                // nombre en el nuevo GameObject heredaría el modo edición y el
                // buffer del clip del objeto anterior durante un frame.
                const bool selectionChanged = (m_boundTo != go);
                if (selectionChanged) m_renamingClip.clear();

                drawAnimationSources(ctx, go);

                // --- Añadir estado desde los clips del modelo ---
                SkinnedMesh* mesh = go->getSkinnedMesh();
                if (!mesh || mesh->animationClips.empty())
                {
                    ImGui::TextDisabled("El GameObject no tiene un mesh skinned con animaciones.");
                }
                else if (ImGui::BeginCombo("##addstate", "Add State from Clip"))
                {
                    for (size_t i = 0; i < mesh->animationClips.size(); i++)
                    {
                        if (!ImGui::Selectable(mesh->animationClips[i].name.c_str())) continue;
                        AnimatorComponent::State st;
                        st.name           = mesh->animationClips[i].name;
                        st.clipName       = mesh->animationClips[i].name;
                        st.clipIndex      = (int)i;
                        st.duration       = mesh->animationClips[i].duration;
                        st.ticksPerSecond = mesh->animationClips[i].ticksPerSecond;
                        st.editorPos      = glm::vec2(40.0f + 40.0f * (float)go->getAnimator()->states().size(),
                                                       40.0f + 30.0f * (float)go->getAnimator()->states().size());
                        const int idx = go->getAnimator()->addState(st);
                        const int eid = go->getAnimator()->states()[idx].editorId;
                        // El nodo es nuevo: hay que colocarlo en el canvas a mano, el
                        // sync general solo corre al cambiar de objeto.
                        ed::SetCurrentEditor(m_ctx);
                        ed::SetNodePosition(nodeId(eid), ImVec2(st.editorPos.x, st.editorPos.y));
                        ed::SetCurrentEditor(nullptr);
                        ctx.pushLog("Animator: estado '" + st.name + "' añadido");
                    }
                    ImGui::EndCombo();
                }

                drawParameterList(ctx, go);
                ImGui::SameLine();

                ImGui::BeginChild("canvas", ImVec2(0, 0), false);
                if (selectionChanged)
                {
                    // Cambio de selección: el canvas todavía tiene las posiciones del
                    // objeto anterior. Se vuelca una vez, no cada frame — si no, el
                    // usuario no podría arrastrar los nodos.
                    ed::SetCurrentEditor(m_ctx);
                    syncPositionsFromComponent(go);
                    ed::SetCurrentEditor(nullptr);
                    m_boundTo = go;
                }
                drawGraph(ctx, go);
                // Sync inverso cada frame, incondicional (ya no hace falta el guardia
                // de Task 10 que lo saltaba tras un borrado): con editorId estable, un
                // superviviente conserva su id de nodo pase lo que pase con el vector,
                // así que GetNodePosition(nodeId(editorId)) siempre lee la posición
                // del nodo correcto, incluso el mismo frame en que se borró otro nodo.
                ed::SetCurrentEditor(m_ctx);
                syncPositionsToComponent(go);
                ed::SetCurrentEditor(nullptr);
                ImGui::EndChild();
            }
        }
        ImGui::End();
    }

    // Incondicional y fuera de la ventana: tiene que drenarse aunque el panel
    // esté cerrado/colapsado o la selección haya cambiado mientras el diálogo
    // estaba abierto (ver comentario grande al principio de esta función).
    drawAnimationSourceDialog(ctx);
}

} // namespace DonTopo
