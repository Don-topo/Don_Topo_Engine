#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/Blend2D.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace DonTopo
{
    static_assert(AnimatorComponent::kMaxLayers == kMaxLayersPose, "AnimationPose y el Animator comparten el tope de capas");
    int AnimatorComponent::addState(State s, int layer)
    {
        Layer& L = lay(layer);
        // editorId estable pa el canvas del AnimatorPanel: nunca depende del
        // índice en L.states (ver comentario del campo en el header). Un estado
        // fresco (editor) llega con -1 y se le asigna aquí; uno copiado (redo de
        // AnimatorComponentCommand) o cargado (Scene::animatorFromJson, que
        // tampoco serializa editorId) también llega con -1 y cae en el mismo
        // camino, asignándose en orden de carga/copia — estable dentro de la
        // sesión, que es todo lo que el canvas necesita. Si ya trae un id (una
        // copia de un estado que SÍ tenía uno asignado) se conserva, y el
        // contador se adelanta pa que el siguiente addState nunca lo repita.
        if (s.editorId < 0) s.editorId = m_nextEditorId++;
        else                m_nextEditorId = std::max(m_nextEditorId, s.editorId + 1);

        L.states.push_back(std::move(s));
        // Primer estado añadido: entrada por defecto. Un grafo sin entrada no
        // arranca, y obligar a marcarla a mano sería un pie en el que tropezar.
        if (L.entryState < 0) L.entryState = 0;
        return (int)L.states.size() - 1;
    }

    void AnimatorComponent::addTransition(Transition t, int layer) { lay(layer).transitions.push_back(std::move(t)); }

    void AnimatorComponent::enterState(int idx, int layer)
    {
        Layer& L = lay(layer);
        L.currentState = idx;
        L.animTime     = 0.0f;
        L.finished     = false;
        L.stateTicks   = 0.0;
    }

    void AnimatorComponent::removeState(int idx, int layer)
    {
        Layer& L = lay(layer);
        if (idx < 0 || idx >= (int)L.states.size()) return;
        L.states.erase(L.states.begin() + idx);

        // Las transiciones guardan índices: borrar un estado invalida las que lo
        // tocan y desplaza las que apuntan por encima. Sin esto, borrar un nodo
        // dejaría links apuntando a un estado distinto del que el usuario ve.
        L.transitions.erase(
            std::remove_if(L.transitions.begin(), L.transitions.end(),
                [idx](const Transition& t) { return t.fromState == idx || t.toState == idx; }),
            L.transitions.end());
        for (auto& t : L.transitions)
        {
            if (t.fromState > idx) t.fromState--;
            if (t.toState   > idx) t.toState--;
        }

        if (L.entryState == idx)      L.entryState = L.states.empty() ? -1 : 0;
        else if (L.entryState > idx)  L.entryState--;

        // El playhead se reindexa igual que las transiciones: borrar OTRO estado
        // no tiene por qué mover al usuario de sitio. Sólo si se borra el actual
        // hay que caer a la entrada, porque el actual ya no existe.
        //
        // Antes esto era un reset() a secas, que además borraba todos los
        // parámetros — y el AnimatorPanel llama aquí sin mirar si se está en
        // Play, así que reordenar el grafo a mitad de partida se llevaba por
        // delante los bool/trigger/int/float que el script venía escribiendo.
        if (L.currentState == idx)
        {
            enterState(L.entryState, layer);
        }
        else if (L.currentState > idx)
        {
            L.currentState--;
        }

        // El cross-fade se corta siempre: el estado que se apagaba puede haberse
        // ido o haberse reindexado, y mezclar contra un índice movido daría la
        // pose de otro clip.
        L.prevState     = -1;
        L.prevAnimTime  = 0.0f;
        L.blendElapsed  = 0.0f;
        L.blendDuration = 0.0f;
        L.frozenFade    = false;
        L.freezePending = false;
    }

    void AnimatorComponent::removeTransition(int idx, int layer)
    {
        Layer& L = lay(layer);
        if (idx < 0 || idx >= (int)L.transitions.size()) return;
        L.transitions.erase(L.transitions.begin() + idx);
    }

    void AnimatorComponent::setEntryState(int idx, int layer)
    {
        Layer& L = lay(layer);
        if (idx < 0 || idx >= (int)L.states.size()) return;
        L.entryState = idx;
        // Mueve el playhead a la entrada nueva (el preview del editor tiene que
        // seguirla) pero sin borrar los parámetros: esto también corre en Play.
        resetPlayback();
    }

    void AnimatorComponent::addParameter(std::string name, ParamType type)
    {
        if (name.empty()) return;
        for (const auto& p : m_parameters)
            if (p.name == name) return;      // nombres únicos: se consultan por nombre
        m_parameters.push_back({ std::move(name), type });
        const std::string& n = m_parameters.back().name;
        switch (type)
        {
            case ParamType::Bool:    m_bools[n]    = false;  break;
            case ParamType::Trigger: m_triggers[n] = false;  break;
            case ParamType::Int:     m_ints[n]     = 0;      break;
            case ParamType::Float:   m_floats[n]   = 0.0f;   break;
        }
    }

    void AnimatorComponent::removeParameter(const std::string& name)
    {
        // Nombre vacío == el que usan las condiciones AnimationFinished (ver
        // Condition::paramName); si siguiéramos de largo, el bucle de abajo las
        // borraría todas del grafo sin que el usuario lo pidiera.
        if (name.empty()) return;

        m_parameters.erase(
            std::remove_if(m_parameters.begin(), m_parameters.end(),
                [&name](const Parameter& p) { return p.name == name; }),
            m_parameters.end());
        m_bools.erase(name);
        m_triggers.erase(name);
        m_ints.erase(name);
        m_floats.erase(name);

        for (auto& L : m_layers)
        {
            // Las condiciones que lo usaban quedarían colgadas y no dispararían
            // nunca: se van con él.
            for (auto& t : L.transitions)
                t.conditions.erase(
                    std::remove_if(t.conditions.begin(), t.conditions.end(),
                        [&name](const Condition& c) { return c.paramName == name; }),
                    t.conditions.end());

            // Una transición que se quedó sin condiciones (ésta era la única) no
            // puede disparar nunca (conditionsMet exige al menos una), así que
            // dejarla sería un link invisible y muerto en el canvas. Si conservó
            // otras condiciones, sobrevive tal cual.
            L.transitions.erase(
                std::remove_if(L.transitions.begin(), L.transitions.end(),
                    [](const Transition& t) { return t.conditions.empty(); }),
                L.transitions.end());
        }
    }

    AnimatorComponent::Graph AnimatorComponent::graph() const
    {
        const Layer& L = m_layers[0];
        Graph g{ L.states, L.transitions, m_parameters, L.entryState, {}, m_ik, m_propertyClips };
        g.extraLayers.assign(m_layers.begin() + 1, m_layers.end());
        return g;
    }

    void AnimatorComponent::applyGraph(const Graph& g)
    {
        // La IK y los clips de propiedades son diseño entero: entran tal cual
        // (sus índices los rehacen rebindClips y bindProperties, como los
        // clipIndex).
        m_ik            = g.ik;
        m_propertyClips = g.propertyClips;

        const std::vector<Parameter> oldParams = m_parameters;
        auto oldBools    = m_bools;
        auto oldTriggers = m_triggers;
        auto oldInts     = m_ints;
        auto oldFloats   = m_floats;
        m_parameters = g.parameters;

        m_bools.clear();
        m_triggers.clear();
        m_ints.clear();
        m_floats.clear();
        for (const auto& p : m_parameters)
        {
            bool mismoTipo = false;
            for (const auto& o : oldParams)
                if (o.name == p.name) { mismoTipo = (o.type == p.type); break; }
            switch (p.type)
            {
                case ParamType::Bool:    m_bools[p.name]    = mismoTipo ? oldBools[p.name]    : false; break;
                case ParamType::Trigger: m_triggers[p.name] = mismoTipo ? oldTriggers[p.name] : false; break;
                case ParamType::Int:     m_ints[p.name]     = mismoTipo ? oldInts[p.name]     : 0;     break;
                case ParamType::Float:   m_floats[p.name]   = mismoTipo ? oldFloats[p.name]   : 0.0f;  break;
            }
        }

        // Capas: la base y las del snapshot. Una capa que ya existía en ese
        // índice conserva su playhead (casado por editorId, único en todo el
        // componente) y su Any State en el canvas; una nueva arranca de cero.
        const int n = 1 + (int)g.extraLayers.size();
        if ((int)m_layers.size() > n) m_layers.resize((size_t)n);
        for (int li = 0; li < n; li++)
        {
            if (li >= (int)m_layers.size())
            {
                Layer nueva = g.extraLayers[(size_t)li - 1];
                nueva.states.clear();
                nueva.transitions.clear();
                nueva.currentState = -1;
                nueva.prevState    = -1;
                nueva.frozenFade   = nueva.freezePending = false;
                m_layers.push_back(std::move(nueva));
            }
            else if (li > 0)
            {
                const Layer& src = g.extraLayers[(size_t)li - 1];
                Layer& L = m_layers[li];
                L.name      = src.name;
                L.weight    = src.weight;
                L.mode      = src.mode;
                L.maskBones = src.maskBones;
            }
            if (li == 0) applyLayerGraph(0, g.states, g.transitions, g.entryState);
            else
            {
                const Layer& src = g.extraLayers[(size_t)li - 1];
                applyLayerGraph(li, src.states, src.transitions, src.entryState);
            }
        }
    }

    void AnimatorComponent::applyLayerGraph(int li, const std::vector<State>& states,
                                            const std::vector<Transition>& transitions, int entryState)
    {
        Layer& L = m_layers[li];
        // Identidades vivas ANTES de sustituir nada: tras copiar el grafo ya no
        // se sabría qué editorId era el actual, cuál el que se apagaba, ni
        // dónde estaba cada nodo en el canvas.
        auto editorIdAt = [&L](int idx) {
            return (idx >= 0 && idx < (int)L.states.size()) ? L.states[idx].editorId : -1;
        };
        const int curId  = editorIdAt(L.currentState);
        const int prevId = editorIdAt(L.prevState);
        std::unordered_map<int, glm::vec2> livePos;
        for (const auto& s : L.states) livePos[s.editorId] = s.editorPos;

        L.states      = states;
        L.transitions = transitions;
        L.entryState  = entryState;

        // Dos pasadas: primero adelantar el contador con los ids que ya
        // traen los estados, después repartir a los que llegan sin id (-1).
        // Al revés, un estado sin id podría recibir uno que otro trae ya.
        for (auto& s : L.states)
        {
            if (s.editorId < 0) continue;
            m_nextEditorId = std::max(m_nextEditorId, s.editorId + 1);
            auto it = livePos.find(s.editorId);
            if (it != livePos.end()) s.editorPos = it->second;
        }
        for (auto& s : L.states)
            if (s.editorId < 0) s.editorId = m_nextEditorId++;

        auto indexOf = [&L](int editorId) {
            if (editorId < 0) return -1;
            for (int i = 0; i < (int)L.states.size(); i++)
                if (L.states[i].editorId == editorId) return i;
            return -1;
        };
        const int cur  = indexOf(curId);
        const int prev = indexOf(prevId);

        if (curId < 0)
        {
            // No había playhead (grafo sin arrancar): update() lo pondrá en la
            // entrada, igual que antes de aplicar nada.
            L.currentState = -1;
        }
        else if (cur >= 0)
        {
            L.currentState = cur;
        }
        else
        {
            enterState(L.entryState, li);
        }

        if (L.frozenFade && cur >= 0)
        {
            // La congelada no depende de índices del grafo: el fade sigue.
        }
        else if (L.prevState >= 0 && cur >= 0 && prev >= 0)
        {
            L.prevState = prev;
        }
        else
        {
            L.prevState     = -1;
            L.prevAnimTime  = 0.0f;
            L.blendElapsed  = 0.0f;
            L.blendDuration = 0.0f;
            L.frozenFade    = false;
            L.freezePending = false;
        }
    }

    bool AnimatorComponent::hasParam(const std::string& n, ParamType type) const
    {
        for (const auto& p : m_parameters)
            if (p.name == n) return p.type == type;
        return false;
    }

    void AnimatorComponent::setBool(const std::string& n, bool v)
    {
        if (!hasParam(n, ParamType::Bool)) return;
        m_bools[n] = v;
    }

    bool AnimatorComponent::getBool(const std::string& n) const
    {
        auto it = m_bools.find(n);
        return it != m_bools.end() && it->second;
    }

    void AnimatorComponent::setTrigger(const std::string& n)
    {
        if (!hasParam(n, ParamType::Trigger)) return;
        m_triggers[n] = true;
    }

    void AnimatorComponent::resetTrigger(const std::string& n)
    {
        if (!hasParam(n, ParamType::Trigger)) return;
        m_triggers[n] = false;
    }

    bool AnimatorComponent::isTriggerSet(const std::string& n) const
    {
        auto it = m_triggers.find(n);
        return it != m_triggers.end() && it->second;
    }

    void AnimatorComponent::setInt(const std::string& n, int v)
    {
        if (!hasParam(n, ParamType::Int)) return;
        m_ints[n] = v;
    }

    int AnimatorComponent::getInt(const std::string& n) const
    {
        auto it = m_ints.find(n);
        return it != m_ints.end() ? it->second : 0;
    }

    void AnimatorComponent::setFloat(const std::string& n, float v)
    {
        if (!hasParam(n, ParamType::Float)) return;
        m_floats[n] = v;
    }

    float AnimatorComponent::getFloat(const std::string& n) const
    {
        auto it = m_floats.find(n);
        return it != m_floats.end() ? it->second : 0.0f;
    }

    bool AnimatorComponent::hasFloatParameter(const std::string& n) const
    {
        return hasParam(n, ParamType::Float);
    }

    int AnimatorComponent::currentClipIndex(int layer) const
    {
        const Layer& L = lay(layer);
        if (L.currentState < 0 || L.currentState >= (int)L.states.size()) return 0;
        const int ci = L.states[L.currentState].clipIndex;
        return ci >= 0 ? ci : 0;
    }

    int AnimatorComponent::previousClipIndex(int layer) const
    {
        const Layer& L = lay(layer);
        if (L.prevState < 0 || L.prevState >= (int)L.states.size()) return 0;
        const int ci = L.states[L.prevState].clipIndex;
        return ci >= 0 ? ci : 0;
    }

    float AnimatorComponent::blendWeight(int layer) const
    {
        const Layer& L = lay(layer);
        // Sin mezcla el destino pesa el 100%: así el consumidor no necesita
        // preguntar antes si hay cross-fade o no.
        if ((L.prevState < 0 && !L.frozenFade) || L.blendDuration <= 0.0f) return 1.0f;
        const float w = L.blendElapsed / L.blendDuration;
        return w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
    }

    std::string AnimatorComponent::currentStateName(int layer) const
    {
        const Layer& L = lay(layer);
        if (L.currentState < 0 || L.currentState >= (int)L.states.size()) return "";
        return L.states[L.currentState].name;
    }

    bool AnimatorComponent::stateBlends(int stateIdx, int layer) const
    {
        const Layer& L = lay(layer);
        if (stateIdx < 0 || stateIdx >= (int)L.states.size()) return false;
        const State& st = L.states[stateIdx];
        // Una entrada a -1 = el clip no existe en la malla (rebindClips ya
        // avisó): mezclar contra ella leería otro clip o fuera del SSBO.
        bool alguna = false;
        for (const auto& e : st.blendEntries)
            if (e.clipIndex >= 0) { alguna = true; break; }
        if (!alguna) return false;
        // Un parámetro no declarado devolvería 0.0f en getFloat y clavaría el
        // peso en un extremo sin decir por qué; mejor no mezclar.
        return hasParam(st.blendParam, ParamType::Float);
    }

    AnimatorComponent::BlendPair AnimatorComponent::stateBlendPair(int stateIdx, float animTime, int layer) const
    {
        if (!stateBlends2D(stateIdx, layer)) return stateBlendPair1D(stateIdx, animTime, layer);
        // 2D: la vista de dos clips son las dos muestras que más pesan.
        BlendSample bs[3];
        const int n = stateBlendSamples(stateIdx, animTime, bs, layer);
        int b = 0, a = -1;
        for (int i = 1; i < n; i++) if (bs[i].weight > bs[b].weight) b = i;
        for (int i = 0; i < n; i++) if (i != b && (a < 0 || bs[i].weight > bs[a].weight)) a = i;
        if (a < 0) a = b;
        const float wa = bs[a].weight, wb = bs[b].weight;
        return { bs[a].clip, bs[a].time, bs[b].clip, bs[b].time,
                 (a == b || wa + wb <= 0.0f) ? 1.0f : wb / (wa + wb), bs[a].duration, bs[b].duration };
    }

    bool AnimatorComponent::stateBlends2D(int stateIdx, int layer) const
    {
        const Layer& L = lay(layer);
        return stateBlends(stateIdx, layer) && hasParam(L.states[stateIdx].blendParamY, ParamType::Float);
    }

    int AnimatorComponent::stateBlendSamples(int stateIdx, float animTime, BlendSample out[3], int layer) const
    {
        const Layer& L = lay(layer);
        if (!stateBlends2D(stateIdx, layer))
        {
            const BlendPair bp = stateBlendPair1D(stateIdx, animTime, layer);
            if (bp.clipA == bp.clipB) { out[0] = { bp.clipB, bp.timeB, 1.0f, bp.durB }; return 1; }
            out[0] = { bp.clipA, bp.timeA, 1.0f - bp.weight, bp.durA };
            out[1] = { bp.clipB, bp.timeB, bp.weight, bp.durB };
            return 2;
        }
        const State& st    = L.states[stateIdx];
        const float  phase = st.duration > 0.0f ? animTime / st.duration : 0.0f;
        // Puntos: el principal primero (desempata), luego las entradas con clip.
        std::vector<glm::vec2> pts;
        std::vector<int>       quien;   // -1 principal, k entrada
        pts.push_back({ st.clipThreshold, st.clipThresholdY });
        quien.push_back(-1);
        for (int k = 0; k < (int)st.blendEntries.size(); k++)
            if (st.blendEntries[k].clipIndex >= 0)
            {
                pts.push_back({ st.blendEntries[k].threshold, st.blendEntries[k].thresholdY });
                quien.push_back(k);
            }
        Blend2DWeight w[3];
        const int n = blend2DWeights(pts, { getFloat(st.blendParam), getFloat(st.blendParamY) }, w);
        for (int i = 0; i < n; i++)
        {
            const int k = quien[w[i].point];
            if (k < 0) out[i] = { st.clipIndex, animTime, w[i].weight, st.duration };
            else       out[i] = { st.blendEntries[k].clipIndex, phase * st.blendEntries[k].duration,
                                  w[i].weight, st.blendEntries[k].duration };
        }
        return n;
    }

    AnimatorComponent::BlendPair AnimatorComponent::stateBlendPair1D(int stateIdx, float animTime, int layer) const
    {
        const Layer& L = lay(layer);
        const int clip = (stateIdx >= 0 && stateIdx < (int)L.states.size() && L.states[stateIdx].clipIndex >= 0)
                             ? L.states[stateIdx].clipIndex : 0;
        const float durActual = (stateIdx >= 0 && stateIdx < (int)L.states.size()) ? L.states[stateIdx].duration : 0.0f;
        BlendPair out{ clip, animTime, clip, animTime, 1.0f, durActual, durActual };
        if (!stateBlends(stateIdx, layer)) return out;

        const State& st    = L.states[stateIdx];
        const float  p     = getFloat(st.blendParam);
        const float  phase = st.duration > 0.0f ? animTime / st.duration : 0.0f;

        // Vecino de abajo (mayor umbral <= p) y de arriba (menor umbral > p),
        // sin ordenar ni asignar memoria. -1 es el principal, que se mira
        // primero: la comparación ESTRICTA hace que con umbrales iguales se
        // quede el primero visto.
        const int kNinguno = -2;
        int   lo  = kNinguno, hi  = kNinguno;
        float loT = 0.0f,     hiT = 0.0f;
        auto considerar = [&](int k, float t) {
            if (t <= p) { if (lo == kNinguno || t > loT) { lo = k; loT = t; } }
            else        { if (hi == kNinguno || t < hiT) { hi = k; hiT = t; } }
        };
        considerar(-1, st.clipThreshold);
        for (int k = 0; k < (int)st.blendEntries.size(); k++)
            if (st.blendEntries[k].clipIndex >= 0)
                considerar(k, st.blendEntries[k].threshold);

        // Por debajo del primer umbral o por encima del último: solo el extremo.
        if (lo == kNinguno) lo = hi;
        if (hi == kNinguno) hi = lo;

        // Todos en la MISMA fase normalizada del principal: un walk de 40
        // ticks y un run de 100 mezclados por tiempo absoluto se desincronizan
        // y las piernas patinan; por fase, el pie de apoyo de uno cae sobre el
        // del otro.
        auto clipDe   = [&](int k) { return k < 0 ? st.clipIndex : st.blendEntries[k].clipIndex; };
        auto tiempoDe = [&](int k) { return k < 0 ? animTime : phase * st.blendEntries[k].duration; };
        out.clipA  = clipDe(lo);
        out.timeA  = tiempoDe(lo);
        out.clipB  = clipDe(hi);
        out.timeB  = tiempoDe(hi);
        out.weight = (lo == hi) ? 1.0f : (p - loT) / (hiT - loT);
        auto durDe = [&](int k) { return k < 0 ? st.duration : st.blendEntries[k].duration; };
        out.durA   = durDe(lo);
        out.durB   = durDe(hi);
        return out;
    }

    AnimationPose AnimatorComponent::pose() const
    {
        AnimationPose out;
        out.rootMotionMode = poseRootMotionMode();
        out.layerCount     = (int)m_layers.size();
        for (int li = 0; li < (int)m_layers.size(); li++)
        {
            const Layer& L = m_layers[li];
            PoseLayer&   pl = out.layers[li];
            pl.weight    = layerWeight(li);
            pl.mode      = li == 0 ? 0u : (uint32_t)L.mode;
            pl.freezeNow = L.freezePending;
            pl.mask      = (li == 0 || L.maskResolved.empty()) ? nullptr : &L.maskResolved;
            // Una capa apagada no aporta muestras, pero sigue contando: el
            // índice de capa de las demás no cambia.
            if (li > 0 && pl.weight <= 0.0f) continue;

            int enCapa = 0;
            auto add = [&](int clip, float time, float w) {
                if (w <= 0.0f || enCapa >= kMaxPoseSamplesPerLayer) return;
                out.samples[out.count++] = { clip < 0 ? 0 : clip, time, w, li };
                enCapa++;
            };
            // Las muestras de blend de un estado (1D o 2D), con SU reloj,
            // escaladas por el peso que ese estado tiene en el fade.
            auto addMuestras = [&](int stateIdx, float time, float scale) {
                BlendSample bs[3];
                const int n = stateBlendSamples(stateIdx, time, bs, li);
                for (int i = 0; i < n; i++) add(bs[i].clip, bs[i].time, scale * bs[i].weight);
            };
            if (L.currentState < 0 || L.currentState >= (int)L.states.size())
            {
                // Sin estado: la base cae al clip 0 como siempre; una capa
                // superior sin grafo no dice nada.
                if (li == 0) add(0, L.animTime, 1.0f);
                continue;
            }
            const float w = blendWeight(li);
            if (L.frozenFade)
            {
                pl.frozenWeight = 1.0f - w;
                addMuestras(L.currentState, L.animTime, w);
            }
            else if (blending(li) && L.prevState < (int)L.states.size())
            {
                // El que sale aporta su pareja completa, no solo su principal (A3).
                addMuestras(L.prevState, L.prevAnimTime, 1.0f - w);
                addMuestras(L.currentState, L.animTime, w);
            }
            else
                addMuestras(L.currentState, L.animTime, 1.0f);
        }
        return out;
    }

    int AnimatorComponent::stateIndexByEditorId(int editorId, int layer) const
    {
        if (editorId < 0 || layer < 0 || layer >= (int)m_layers.size()) return -1;
        const auto& states = m_layers[(size_t)layer].states;
        for (size_t i = 0; i < states.size(); i++)
            if (states[i].editorId == editorId) return (int)i;
        return -1;
    }

    int AnimatorComponent::addPropertyClip(PropertyClip c)
    {
        if ((int)m_propertyClips.size() >= kMaxPropertyClips) return -1;
        m_propertyClips.push_back(std::move(c));
        return (int)m_propertyClips.size() - 1;
    }

    void AnimatorComponent::removePropertyClip(int i)
    {
        if (i < 0 || i >= (int)m_propertyClips.size()) return;
        m_propertyClips.erase(m_propertyClips.begin() + i);
    }

    void AnimatorComponent::bindProperties(const GameObject* go, std::vector<std::string>* warnings)
    {
        for (auto& L : m_layers)
            for (auto& st : L.states)
            {
                st.propertyClipIndex = -1;
                if (st.propertyClipName.empty()) continue;
                for (int i = 0; i < (int)m_propertyClips.size(); i++)
                    if (m_propertyClips[(size_t)i].name == st.propertyClipName) { st.propertyClipIndex = i; break; }
                if (st.propertyClipIndex < 0)
                {
                    if (warnings)
                        warnings->push_back("Animator: el estado '" + st.name + "' referencia el clip de "
                                            "propiedades '" + st.propertyClipName + "', que no existe");
                    continue;
                }
                // Sin clip de malla resuelto, el reloj del estado sale del clip
                // de propiedades: el Animator cuenta en TICKS, así que se le da
                // un ritmo fijo y la duración en ticks que le corresponde. Con
                // clip de malla manda ese, y el de propiedades se muestrea por
                // la fase del estado.
                if (st.clipIndex < 0)
                {
                    st.ticksPerSecond = 30.0f;
                    st.duration       = m_propertyClips[(size_t)st.propertyClipIndex].duration * st.ticksPerSecond;
                }
            }

        for (auto& clip : m_propertyClips)
            for (auto& tr : clip.tracks)
            {
                if (tr.target == TrackTarget::Parameter)
                {
                    // Una curva no depende del objeto: depende de que el
                    // parámetro exista y sea Float.
                    tr.resolved = !tr.parameterName.empty() && hasFloatParameter(tr.parameterName);
                    if (!tr.resolved && warnings)
                        warnings->push_back("Animator: la curva del clip '" + clip.name +
                                            "' escribe '" + tr.parameterName +
                                            "', que no es un parametro Float declarado");
                    continue;
                }
                tr.resolved = !go || propertyAvailable(*go, tr.property);
                if (!tr.resolved && warnings)
                    warnings->push_back("Animator: la pista '" + std::string(propertyName(tr.property)) +
                                        "' del clip '" + clip.name + "' necesita un componente que el objeto no tiene");
            }
    }

    int AnimatorComponent::propertySamples(PropertySampleRef* out, int max) const
    {
        int n = 0;
        // Mismo reparto que pose(): cada capa aporta su estado actual y, en un
        // fade, también el que se apaga, con el peso de los dos multiplicado
        // por el de la capa.
        auto add = [&](int li, int stateIdx, float animTime, float peso) {
            if (n >= max || peso <= 0.0f) return;
            const Layer& L = m_layers[(size_t)li];
            if (stateIdx < 0 || stateIdx >= (int)L.states.size()) return;
            const State& st = L.states[(size_t)stateIdx];
            if (st.propertyClipIndex < 0) return;
            out[n++] = { st.propertyClipIndex,
                         st.ticksPerSecond > 0.0f ? animTime / st.ticksPerSecond : 0.0f,
                         peso };
        };
        for (int li = 0; li < (int)m_layers.size(); li++)
        {
            const Layer& L    = m_layers[(size_t)li];
            const float  capa = layerWeight(li);
            if (capa <= 0.0f) continue;
            const float w = blendWeight(li);
            if (blending(li))
            {
                add(li, L.prevState, L.prevAnimTime, capa * (1.0f - w));
                add(li, L.currentState, L.animTime, capa * w);
            }
            else
                add(li, L.currentState, L.animTime, capa);
        }
        return n;
    }

    int AnimatorComponent::addIkConstraint(IkConstraint c)
    {
        if ((int)m_ik.size() >= kMaxIkConstraints) return -1;
        m_ik.push_back(std::move(c));
        return (int)m_ik.size() - 1;
    }

    void AnimatorComponent::removeIkConstraint(int i)
    {
        if (i < 0 || i >= (int)m_ik.size()) return;
        m_ik.erase(m_ik.begin() + i);
    }

    AnimatorComponent::IkConstraint* AnimatorComponent::ikPorNombre(const std::string& n)
    {
        for (auto& c : m_ik)
            if (c.name == n) return &c;
        return nullptr;
    }

    const AnimatorComponent::IkConstraint* AnimatorComponent::ikPorNombre(const std::string& n) const
    {
        for (const auto& c : m_ik)
            if (c.name == n) return &c;
        return nullptr;
    }

    void AnimatorComponent::setIkWeight(const std::string& nombre, float w)
    {
        if (IkConstraint* c = ikPorNombre(nombre))
            c->weight = std::isfinite(w) ? std::clamp(w, 0.0f, 1.0f) : 0.0f;
    }

    float AnimatorComponent::ikWeight(const std::string& nombre) const
    {
        const IkConstraint* c = ikPorNombre(nombre);
        return c ? c->weight : 0.0f;
    }

    void AnimatorComponent::setIkTarget(const std::string& nombre, uint64_t id)
    {
        if (IkConstraint* c = ikPorNombre(nombre)) c->targetId = id;
    }

    void AnimatorComponent::setIkPole(const std::string& nombre, uint64_t id)
    {
        if (IkConstraint* c = ikPorNombre(nombre)) c->poleId = id;
    }

    int AnimatorComponent::addLayer(const std::string& name)
    {
        if ((int)m_layers.size() >= kMaxLayers) return -1;
        Layer L;
        L.name = name;
        m_layers.push_back(std::move(L));
        return (int)m_layers.size() - 1;
    }

    void AnimatorComponent::removeLayer(int i)
    {
        if (i <= 0 || i >= (int)m_layers.size()) return;
        m_layers.erase(m_layers.begin() + i);
    }

    void AnimatorComponent::moveLayer(int from, int to)
    {
        const int n = (int)m_layers.size();
        if (from <= 0 || to <= 0 || from >= n || to >= n || from == to) return;
        Layer L = std::move(m_layers[from]);
        m_layers.erase(m_layers.begin() + from);
        m_layers.insert(m_layers.begin() + to, std::move(L));
    }

    void AnimatorComponent::setLayerWeight(int i, float w)
    {
        if (i <= 0 || i >= (int)m_layers.size()) return;
        m_layers[i].weight = std::isfinite(w) ? std::clamp(w, 0.0f, 1.0f) : 0.0f;
    }

    float AnimatorComponent::layerWeight(int i) const
    {
        if (i <= 0 || i >= (int)m_layers.size()) return 1.0f;
        return m_layers[i].weight;
    }

    void AnimatorComponent::setLayerMode(int i, LayerMode m)
    {
        if (i <= 0 || i >= (int)m_layers.size()) return;
        m_layers[i].mode = m;
    }

    int AnimatorComponent::poseClipA() const
    {
        const Layer& L = m_layers[0];
        return blending() ? previousClipIndex() : stateBlendPair(L.currentState, L.animTime).clipA;
    }

    float AnimatorComponent::poseTimeA() const
    {
        const Layer& L = m_layers[0];
        return blending() ? L.prevAnimTime : stateBlendPair(L.currentState, L.animTime).timeA;
    }

    int AnimatorComponent::poseClipB() const
    {
        const Layer& L = m_layers[0];
        // Cross-fade: el destino aporta su clip PRIMARIO (solo caben dos clips).
        return blending() ? currentClipIndex() : stateBlendPair(L.currentState, L.animTime).clipB;
    }

    float AnimatorComponent::poseTimeB() const
    {
        const Layer& L = m_layers[0];
        return blending() ? L.animTime : stateBlendPair(L.currentState, L.animTime).timeB;
    }

    float AnimatorComponent::poseWeight() const
    {
        const Layer& L = m_layers[0];
        return blending() ? blendWeight() : stateBlendPair(L.currentState, L.animTime).weight;
    }

    uint32_t AnimatorComponent::poseRootMotionMode() const
    {
        const Layer& L = m_layers[0];
        // Durante un cross-fade manda el estado DESTINO, que ES L.currentState
        // (el que aporta poseClipB): no hay caso especial que escribir.
        if (L.currentState < 0 || L.currentState >= (int)L.states.size()) return 0u;
        return (uint32_t)L.states[L.currentState].rootMotion;
    }

    std::string AnimatorComponent::previousStateName(int layer) const
    {
        const Layer& L = lay(layer);
        if (L.prevState < 0 || L.prevState >= (int)L.states.size()) return "";
        return L.states[L.prevState].name;
    }

    void AnimatorComponent::resetPlayback()
    {
        for (int li = 0; li < (int)m_layers.size(); li++)
        {
            Layer& L = m_layers[li];
            enterState(L.entryState, li);
            // Corta cualquier cross-fade en vuelo: tras esto el estado previo
            // puede ni existir (el editor acaba de reeditar el grafo), y mezclar
            // contra él dejaría una pose imposible o un índice fuera de rango.
            L.prevState     = -1;
            L.prevAnimTime  = 0.0f;
            L.blendElapsed  = 0.0f;
            L.blendDuration = 0.0f;
            L.frozenFade    = false;
            L.freezePending = false;
        }
    }

    void AnimatorComponent::reset()
    {
        resetPlayback();
        for (auto& b : m_bools)    b.second = false;
        for (auto& t : m_triggers) t.second = false;
        for (auto& i : m_ints)     i.second = 0;
        for (auto& f : m_floats)   f.second = 0.0f;
    }

    void AnimatorComponent::rebindClips(const SkinnedMesh& mesh, std::vector<std::string>* warnings)
    {
        auto findClip = [&mesh](const std::string& name) {
            for (size_t i = 0; i < mesh.animationClips.size(); i++)
                if (mesh.animationClips[i].name == name) return (int)i;
            return -1;
        };

        for (auto& L : m_layers)
        {
            for (auto& st : L.states)
            {
                // Las entradas del blend se resuelven SIEMPRE, aunque el primario
                // falle: los avisos son independientes y ver solo uno mandaría a
                // buscar al sitio equivocado. Una entrada recién añadida en el
                // editor todavía no tiene clip: no es un error, no avisa.
                for (auto& e : st.blendEntries)
                {
                    const int b = e.clipName.empty() ? -1 : findClip(e.clipName);
                    e.clipIndex = b;
                    e.duration  = (b >= 0) ? mesh.animationClips[b].duration : 0.0f;
                    if (b < 0 && !e.clipName.empty() && warnings)
                        warnings->push_back("Animator: el estado '" + st.name + "' mezcla con el clip '" +
                                            e.clipName + "', que no existe en el modelo");
                }

                const int found = findClip(st.clipName);
                if (found < 0)
                {
                    st.clipIndex = -1;
                    if (warnings)
                        warnings->push_back("Animator: el estado '" + st.name + "' referencia el clip '" +
                                            st.clipName + "', que no existe en el modelo");
                    continue;
                }
                st.clipIndex      = found;
                st.duration       = mesh.animationClips[found].duration;
                st.ticksPerSecond = mesh.animationClips[found].ticksPerSecond;
                // st.loop NO se toca: es autoría del usuario, no un dato del FBX.
            }
        }
        // Máscaras por nombre de hueso: un nombre que el esqueleto no tiene
        // avisa, como un clip que no existe, y no marca nada.
        for (auto& L : m_layers)
        {
            L.maskResolved.clear();
            if (L.maskBones.empty()) continue;
            L.maskResolved.assign(mesh.skeleton.names.size(), 0);
            for (const auto& nombre : L.maskBones)
            {
                auto it = std::find(mesh.skeleton.names.begin(), mesh.skeleton.names.end(), nombre);
                if (it == mesh.skeleton.names.end())
                {
                    if (warnings)
                        warnings->push_back("Animator: la máscara de la capa '" + L.name +
                                            "' nombra el hueso '" + nombre + "', que el modelo no tiene");
                    continue;
                }
                L.maskResolved[(size_t)(it - mesh.skeleton.names.begin())] = 1;
            }
        }

        // Huesos de las restricciones de IK, por nombre como los clips. En
        // TwoBone el hueso es el EXTREMO y la cadena son sus dos padres: sin
        // ellos no se puede resolver, así que la restricción se apaga y avisa.
        for (auto& c : m_ik)
        {
            c.boneIndex = c.parentIndex = c.grandParentIndex = -1;
            auto it = std::find(mesh.skeleton.names.begin(), mesh.skeleton.names.end(), c.boneName);
            if (it == mesh.skeleton.names.end())
            {
                if (warnings && !c.boneName.empty())
                    warnings->push_back("Animator: la IK '" + c.name + "' usa el hueso '" + c.boneName +
                                        "', que el modelo no tiene");
                continue;
            }
            const int   bi     = (int)(it - mesh.skeleton.names.begin());
            const auto& padres = mesh.skeleton.parentIndex;
            const int   p      = bi < (int)padres.size() ? padres[(size_t)bi] : -1;
            const int   gp     = (p >= 0 && p < (int)padres.size()) ? padres[(size_t)p] : -1;
            if (c.type == IkType::TwoBone && (p < 0 || gp < 0))
            {
                if (warnings)
                    warnings->push_back("Animator: la IK '" + c.name + "' necesita una cadena de tres "
                                        "huesos y '" + c.boneName + "' no tiene padre y abuelo");
                continue;
            }
            c.boneIndex = bi; c.parentIndex = p; c.grandParentIndex = gp;
        }
    }

    void AnimatorComponent::bindClips(const SkinnedMesh& mesh, std::vector<std::string>* warnings)
    {
        rebindClips(mesh, warnings);
        reset();
    }

    int AnimatorComponent::renameClipReferences(const std::string& oldName,
                                                 const std::string& newName)
    {
        int changed = 0;
        for (auto& L : m_layers)
        for (auto& st : L.states)
        {
            // Un estado cuenta UNA vez aunque el rename le toque los dos clips:
            // lo que se devuelve son estados afectados, no referencias.
            bool touched = false;
            if (st.clipName == oldName)      { st.clipName      = newName; touched = true; }
            for (auto& e : st.blendEntries)
                if (e.clipName == oldName) { e.clipName = newName; touched = true; }
            if (touched) changed++;
        }
        return changed;
    }

    bool AnimatorComponent::conditionsMet(const Transition& t, int layer) const
    {
        const Layer& L = lay(layer);
        // Una transición sin condiciones dispararía el frame en que se crea y
        // haría el grafo inusable. Unity cubre ese caso con exit time, que está
        // fuera de alcance.
        if (t.conditions.empty()) return false;

        for (const auto& c : t.conditions)
        {
            switch (c.type)
            {
                case ConditionType::Bool:
                    if (getBool(c.paramName) != c.expected) return false;
                    break;
                case ConditionType::Trigger:
                    if (!isTriggerSet(c.paramName)) return false;
                    break;
                case ConditionType::AnimationFinished:
                    if (!L.finished) return false;
                    break;
                case ConditionType::Int:
                    // El umbral vive en float (ver comentario en AnimatorPanel), así que
                    // redondeamos en vez de truncar: un JSON editado a mano con
                    // threshold: 2.9 debe evaluar como 3, no como 2 silenciosamente.
                    if (!evalCompare(getInt(c.paramName), c.compare, (int)std::lround(c.threshold))) return false;
                    break;
                case ConditionType::Float:
                    if (!evalCompare(getFloat(c.paramName), c.compare, c.threshold)) return false;
                    break;
            }
        }
        return true;
    }

    bool AnimatorComponent::exitTimeCrossed(double n0, double n1, float exitTime)
    {
        const double e = exitTime;
        // A partir de 1 cuenta vueltas acumuladas: listo en cualquier frame
        // que ya las haya dado.
        if (e >= 1.0) return n1 >= e;
        // Primer update tras entrar con exitTime 0: el inicio de la vuelta
        // cero es un cruce, o no dispararía hasta la segunda.
        if (e == 0.0 && n0 == 0.0) return true;
        // Por debajo de 1, en cada vuelta: ¿hay un entero k con
        // n0 < k + e <= n1? Cubre también el dt que da la vuelta cruzando e.
        return std::floor(n1 - e) > std::floor(n0 - e);
    }

    bool AnimatorComponent::transitionReady(const Transition& t, double n0, double n1,
                                            bool hasDuration, int layer) const
    {
        if (t.hasExitTime)
        {
            if (hasDuration && !exitTimeCrossed(n0, n1, t.exitTime)) return false;
            // Solo por tiempo: no hace falta ninguna condición.
            if (t.conditions.empty()) return true;
        }
        // Sin exit time y sin condiciones, conditionsMet devuelve false: una
        // transición así no dispara nunca, como siempre.
        return conditionsMet(t, layer);
    }

    void AnimatorComponent::consumeTriggers(const Transition& t)
    {
        // Solo los de la transición que gana: un trigger que nadie consume sigue
        // armado esperando (mismo comportamiento que Unity).
        for (const auto& c : t.conditions)
            if (c.type == ConditionType::Trigger)
                m_triggers[c.paramName] = false;
    }

    float AnimatorComponent::stateRate(const State& st) const
    {
        float mult = st.speed;
        if (!st.speedParam.empty() && hasParam(st.speedParam, ParamType::Float))
            mult *= getFloat(st.speedParam);
        // Negativo o NaN congela: !(x > 0) es true también para NaN.
        if (!(mult > 0.0f)) mult = 0.0f;
        return st.ticksPerSecond * mult;
    }

    void AnimatorComponent::setSpeed(float s)
    {
        m_speed = (s > 0.0f) ? s : 0.0f;   // negativo o NaN -> 0
    }

    void AnimatorComponent::advanceClock(const State& st, float rate, float& time, bool* finished, float dt)
    {
        if (st.duration > 0.0f && st.ticksPerSecond > 0.0f)
        {
            time += dt * rate;
            if (time >= st.duration)
            {
                if (st.loop)
                {
                    time = std::fmod(time, st.duration);
                }
                else
                {
                    // Clavado en el último frame, y así se queda en los updates
                    // siguientes.
                    time = st.duration;
                    if (finished) *finished = true;
                }
            }
        }
        else if (finished)
        {
            // Un clip sin resolver (clipIndex == -1, duration a 0) o de
            // duración cero real nunca entraría en el bloque de arriba y
            // jamás pondría L.finished a true: una salida "animation finished"
            // se quedaría esperando para siempre. Semánticamente un estado de
            // duración 0 ya ha terminado en el instante en que entra, así que
            // se reafirma finished cada frame (igual que el clamp de arriba lo
            // reafirma en el último frame de un clip normal sin loop).
            *finished = true;
        }
    }

    void AnimatorComponent::collectEvents(const State& st, double ticks0, double ticks1)
    {
        if (st.events.empty() || st.duration <= 0.0f || ticks1 <= ticks0) return;
        const double dur = st.duration;
        for (const auto& ev : st.events)
        {
            // Instantes k·dur + c, k >= 0, dentro de [ticks0, ticks1). El reloj
            // acumulado no tiene wrap, así que cruzar el loop o saltarse ciclos
            // enteros con un dt grande sale del mismo cálculo.
            const double c    = (double)ev.time * dur;
            double       kMin = std::ceil((ticks0 - c) / dur);
            if (kMin < 0.0) kMin = 0.0;
            double       kMax = std::ceil((ticks1 - c) / dur) - 1.0;
            if (!st.loop) kMax = std::min(kMax, 0.0);
            if (kMax < kMin) continue;
            // Tope: un hitch de varios segundos no debe llenar la lista.
            const double veces = std::min(kMax - kMin + 1.0, (double)kMaxEventCyclesPerUpdate);
            for (int i = 0; i < (int)veces; i++)
                m_firedEvents.push_back(ev.name);
        }
    }

    void AnimatorComponent::collectRootMotion(double ticks0, double prevTicks0)
    {
        Layer& L = m_layers[0];
        const State& st = L.states[L.currentState];
        // En un fade la pose usa el clip PRIMARIO de cada lado (ver poseClipB):
        // el movimiento sale de lo mismo que se ve.
        if (blending())
        {
            const float w = blendWeight();
            m_rootMotionSamples.push_back({ st.clipIndex, ticks0, L.stateTicks, st.duration, st.loop, w });
            if (L.prevState >= 0 && L.prevState < (int)L.states.size())
            {
                const State& prev = L.states[L.prevState];
                if (prev.rootMotion == RootMotion::Apply && prev.duration > 0.0f)
                    m_rootMotionSamples.push_back({ prev.clipIndex, prevTicks0, L.prevStateTicks,
                                                    prev.duration, prev.loop, 1.0f - w });
            }
            return;
        }
        BlendSample bs[3];
        const int n = stateBlendSamples(L.currentState, L.animTime, bs);
        // Cada clip va en la fase del principal: sus ticks acumulados son los
        // del principal escalados a su duración.
        for (int i = 0; i < n; i++)
        {
            const double esc = st.duration > 0.0f ? (double)bs[i].duration / st.duration : 0.0;
            m_rootMotionSamples.push_back({ bs[i].clip, ticks0 * esc, L.stateTicks * esc, bs[i].duration,
                                            st.loop, n == 1 ? 1.0f : bs[i].weight });
        }
    }

    void AnimatorComponent::update(float dt, bool evaluateTransitions)
    {
        // Solo lo de ESTE update: se vacía antes de cualquier return.
        m_firedEvents.clear();
        m_rootMotionSamples.clear();
        // Velocidad global: escala el dt entero, cross-fade incluido, en
        // todas las capas.
        dt *= m_speed;
        std::vector<const Transition*> consumir;
        for (int li = 0; li < (int)m_layers.size(); li++)
            updateLayer(li, dt, evaluateTransitions, consumir);
        for (const Transition* t : consumir) consumeTriggers(*t);
    }

    void AnimatorComponent::updateLayer(int li, float dt, bool evaluateTransitions,
                                        std::vector<const Transition*>& consumir)
    {
        Layer& L = m_layers[li];
        if (L.currentState < 0 || L.currentState >= (int)L.states.size())
        {
            L.currentState = L.entryState;
            if (L.currentState < 0 || L.currentState >= (int)L.states.size()) return;
        }

        const State& actual      = L.states[L.currentState];
        const bool   conDuracion = actual.duration > 0.0f && actual.ticksPerSecond > 0.0f;
        const float  ritmo       = stateRate(actual);
        const double ticks0      = L.stateTicks;
        const double prevTicks0  = L.prevStateTicks;
        if (conDuracion)
            L.stateTicks += (double)dt * ritmo;
        advanceClock(actual, ritmo, L.animTime, &L.finished, dt);

        // Cross-fade en curso: el estado que se apaga sigue animándose con SU
        // ritmo y SU loop mientras dura la mezcla. Congelarlo daría un salto
        // visible justo al empezar la transición, que es lo contrario de lo que
        // el cross-fade viene a resolver.
        if (L.prevState >= 0 || L.frozenFade)
        {
            if (L.prevState >= 0 && L.prevState < (int)L.states.size())
            {
                advanceClock(L.states[L.prevState], stateRate(L.states[L.prevState]),
                             L.prevAnimTime, nullptr, dt);
                L.prevStateTicks += (double)dt * stateRate(L.states[L.prevState]);
            }

            L.blendElapsed += dt;
            if (L.blendDuration <= 0.0f || L.blendElapsed >= L.blendDuration)
            {
                // Mezcla terminada: el destino se queda solo. A partir de aquí
                // blendWeight() vuelve a valer 1 por el camino de siempre.
                L.prevState     = -1;
                L.prevAnimTime  = 0.0f;
                L.blendElapsed  = 0.0f;
                L.blendDuration = 0.0f;
                L.frozenFade    = false;
                L.freezePending = false;
            }
        }

        if (!evaluateTransitions) return;

        // Eventos ANTES de las transiciones: si este update sale del estado,
        // su tramo final ya ha disparado. Solo el estado actual; el que se
        // apaga en un fade no, o las pisadas saldrían dobles.
        // Una capa apagada no dispara: no se ve.
        if (conDuracion && (li == 0 || L.weight > 0.0f))
            collectEvents(actual, ticks0, L.stateTicks);

        // Root motion con el mismo tramo: también antes de las transiciones.
        // Solo la base mueve el GameObject.
        if (li == 0 && conDuracion && actual.rootMotion == RootMotion::Apply)
            collectRootMotion(ticks0, prevTicks0);

        const double n0 = conDuracion ? ticks0 / actual.duration : 0.0;
        const double n1 = conDuracion ? L.stateTicks / actual.duration : 0.0;

        // Primero Any State y después las del estado actual, cada grupo por
        // orden de declaración: la primera lista, gana. Es la prioridad de
        // Unity, y lo que espera quien viene de allí.
        const Transition* elegida = nullptr;
        for (const auto& t : L.transitions)
        {
            if (t.fromState != kAnyState) continue;
            if (t.toState < 0 || t.toState >= (int)L.states.size()) continue;
            // Hacia el estado actual solo con el flag: con un bool, reentrar
            // reiniciaría el estado cada frame.
            if (t.toState == L.currentState && !t.canTransitionToSelf) continue;
            if (transitionReady(t, n0, n1, conDuracion, li)) { elegida = &t; break; }
        }
        if (!elegida)
        {
            for (const auto& t : L.transitions)
            {
                if (t.fromState != L.currentState) continue;
                if (t.toState < 0 || t.toState >= (int)L.states.size()) continue;
                if (transitionReady(t, n0, n1, conDuracion, li)) { elegida = &t; break; }
            }
        }
        if (!elegida) return;

        {
            consumir.push_back(elegida);
            startTransitionTo(elegida->toState, elegida->duration, li);   // una por update
        }
    }

    void AnimatorComponent::startTransitionTo(int idx, float duration, int layer)
    {
        Layer& L = lay(layer);
        if (duration > 0.0f)
        {
            if (fading(layer))
            {
                // Interrupción: la mezcla en vuelo se CONGELA en vez de
                // descartarse (A4). El backend copia la pose de pantalla a la
                // congelada antes de evaluar (freezeNow) y el fade sale de ella.
                L.frozenFade     = true;
                L.freezePending  = true;
                L.prevState      = -1;
                L.prevStateTicks = 0.0;
                L.prevAnimTime   = 0.0f;
            }
            else
            {
                // El estado que dejamos pasa a ser el que se apaga, con el
                // tiempo que llevara.
                L.prevState      = L.currentState;
                L.prevStateTicks = L.stateTicks;
                L.prevAnimTime   = L.animTime;
            }
            L.blendElapsed  = 0.0f;
            L.blendDuration = duration;
        }
        else
        {
            // Corte seco: ni estado previo ni mezcla, el camino de siempre.
            L.prevStateTicks = 0.0;
            L.prevState     = -1;
            L.prevAnimTime  = 0.0f;
            L.blendElapsed  = 0.0f;
            L.blendDuration = 0.0f;
            L.frozenFade    = false;
            L.freezePending = false;
        }
        enterState(idx, layer);
    }

    int AnimatorComponent::stateIndexByName(const std::string& name, int layer) const
    {
        const Layer& L = lay(layer);
        for (int i = 0; i < (int)L.states.size(); i++)
            if (L.states[i].name == name) return i;
        return -1;
    }

    bool AnimatorComponent::play(const std::string& stateName, int layer)
    {
        const int idx = stateIndexByName(stateName, layer);
        if (idx < 0) return false;
        startTransitionTo(idx, 0.0f, layer);
        return true;
    }

    bool AnimatorComponent::crossFade(const std::string& stateName, float seconds, int layer)
    {
        Layer& L = lay(layer);
        const int idx = stateIndexByName(stateName, layer);
        if (idx < 0) return false;
        // Sin estado actual no hay nada que apagar: se entra con corte.
        const bool hayActual = L.currentState >= 0 && L.currentState < (int)L.states.size();
        startTransitionTo(idx, hayActual ? seconds : 0.0f, layer);
        return true;
    }

    float AnimatorComponent::normalizedTime(int layer) const
    {
        const Layer& L = lay(layer);
        if (L.currentState < 0 || L.currentState >= (int)L.states.size()) return 0.0f;
        const State& st = L.states[L.currentState];
        if (st.duration <= 0.0f) return 0.0f;
        return (float)(L.stateTicks / st.duration);
    }

    const char* paramTypeLabel(AnimatorComponent::ParamType t)
    {
        switch (t)
        {
            case AnimatorComponent::ParamType::Trigger: return "trigger";
            case AnimatorComponent::ParamType::Int:     return "int";
            case AnimatorComponent::ParamType::Float:   return "float";
            default:                                    return "bool";
        }
    }
}
