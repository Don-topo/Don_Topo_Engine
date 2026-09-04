#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include <algorithm>
#include <cmath>

namespace DonTopo
{
    int AnimatorComponent::addState(State s)
    {
        // editorId estable pa el canvas del AnimatorPanel: nunca depende del
        // índice en m_states (ver comentario del campo en el header). Un estado
        // fresco (editor) llega con -1 y se le asigna aquí; uno copiado (redo de
        // AnimatorComponentCommand) o cargado (Scene::animatorFromJson, que
        // tampoco serializa editorId) también llega con -1 y cae en el mismo
        // camino, asignándose en orden de carga/copia — estable dentro de la
        // sesión, que es todo lo que el canvas necesita. Si ya trae un id (una
        // copia de un estado que SÍ tenía uno asignado) se conserva, y el
        // contador se adelanta pa que el siguiente addState nunca lo repita.
        if (s.editorId < 0) s.editorId = m_nextEditorId++;
        else                m_nextEditorId = std::max(m_nextEditorId, s.editorId + 1);

        m_states.push_back(std::move(s));
        // Primer estado añadido: entrada por defecto. Un grafo sin entrada no
        // arranca, y obligar a marcarla a mano sería un pie en el que tropezar.
        if (m_entryState < 0) m_entryState = 0;
        return (int)m_states.size() - 1;
    }

    void AnimatorComponent::addTransition(Transition t) { m_transitions.push_back(std::move(t)); }

    void AnimatorComponent::removeState(int idx)
    {
        if (idx < 0 || idx >= (int)m_states.size()) return;
        m_states.erase(m_states.begin() + idx);

        // Las transiciones guardan índices: borrar un estado invalida las que lo
        // tocan y desplaza las que apuntan por encima. Sin esto, borrar un nodo
        // dejaría links apuntando a un estado distinto del que el usuario ve.
        m_transitions.erase(
            std::remove_if(m_transitions.begin(), m_transitions.end(),
                [idx](const Transition& t) { return t.fromState == idx || t.toState == idx; }),
            m_transitions.end());
        for (auto& t : m_transitions)
        {
            if (t.fromState > idx) t.fromState--;
            if (t.toState   > idx) t.toState--;
        }

        if (m_entryState == idx)      m_entryState = m_states.empty() ? -1 : 0;
        else if (m_entryState > idx)  m_entryState--;

        // El playhead se reindexa igual que las transiciones: borrar OTRO estado
        // no tiene por qué mover al usuario de sitio. Sólo si se borra el actual
        // hay que caer a la entrada, porque el actual ya no existe.
        //
        // Antes esto era un reset() a secas, que además borraba todos los
        // parámetros — y el AnimatorPanel llama aquí sin mirar si se está en
        // Play, así que reordenar el grafo a mitad de partida se llevaba por
        // delante los bool/trigger/int/float que el script venía escribiendo.
        if (m_currentState == idx)
        {
            m_currentState = m_entryState;
            m_animTime     = 0.0f;
            m_finished     = false;
        }
        else if (m_currentState > idx)
        {
            m_currentState--;
        }

        // El cross-fade se corta siempre: el estado que se apagaba puede haberse
        // ido o haberse reindexado, y mezclar contra un índice movido daría la
        // pose de otro clip.
        m_prevState     = -1;
        m_prevAnimTime  = 0.0f;
        m_blendElapsed  = 0.0f;
        m_blendDuration = 0.0f;
    }

    void AnimatorComponent::removeTransition(int idx)
    {
        if (idx < 0 || idx >= (int)m_transitions.size()) return;
        m_transitions.erase(m_transitions.begin() + idx);
    }

    void AnimatorComponent::setEntryState(int idx)
    {
        if (idx < 0 || idx >= (int)m_states.size()) return;
        m_entryState = idx;
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

        // Las condiciones que lo usaban quedarían colgadas y no dispararían
        // nunca: se van con él.
        for (auto& t : m_transitions)
            t.conditions.erase(
                std::remove_if(t.conditions.begin(), t.conditions.end(),
                    [&name](const Condition& c) { return c.paramName == name; }),
                t.conditions.end());

        // Una transición que se quedó sin condiciones (ésta era la única) no
        // puede disparar nunca (conditionsMet exige al menos una), así que
        // dejarla sería un link invisible y muerto en el canvas. Si conservó
        // otras condiciones, sobrevive tal cual.
        m_transitions.erase(
            std::remove_if(m_transitions.begin(), m_transitions.end(),
                [](const Transition& t) { return t.conditions.empty(); }),
            m_transitions.end());
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

    int AnimatorComponent::currentClipIndex() const
    {
        if (m_currentState < 0 || m_currentState >= (int)m_states.size()) return 0;
        const int ci = m_states[m_currentState].clipIndex;
        return ci >= 0 ? ci : 0;
    }

    int AnimatorComponent::previousClipIndex() const
    {
        if (m_prevState < 0 || m_prevState >= (int)m_states.size()) return 0;
        const int ci = m_states[m_prevState].clipIndex;
        return ci >= 0 ? ci : 0;
    }

    float AnimatorComponent::blendWeight() const
    {
        // Sin mezcla el destino pesa el 100%: así el consumidor no necesita
        // preguntar antes si hay cross-fade o no.
        if (m_prevState < 0 || m_blendDuration <= 0.0f) return 1.0f;
        const float w = m_blendElapsed / m_blendDuration;
        return w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
    }

    std::string AnimatorComponent::currentStateName() const
    {
        if (m_currentState < 0 || m_currentState >= (int)m_states.size()) return "";
        return m_states[m_currentState].name;
    }

    bool AnimatorComponent::stateBlends(int stateIdx) const
    {
        if (stateIdx < 0 || stateIdx >= (int)m_states.size()) return false;
        const State& st = m_states[stateIdx];
        // blendClipIndex a -1 = el nombre no existe en la malla (bindClips ya
        // avisó). Mezclar contra un índice inválido leería otro clip, o fuera
        // del SSBO: el estado se comporta como uno normal y se ve el aviso.
        if (st.blendClipName.empty() || st.blendClipIndex < 0) return false;
        // Un parámetro no declarado devolvería 0.0f en getFloat y clavaría el
        // peso en un extremo sin decir por qué; mejor no mezclar.
        return hasParam(st.blendParam, ParamType::Float);
    }

    float AnimatorComponent::stateBlendWeight(int stateIdx) const
    {
        if (!stateBlends(stateIdx)) return 0.0f;
        const State& st = m_states[stateIdx];
        const float  span = st.blendMax - st.blendMin;
        // Rango degenerado (el usuario dejó min == max): sin él no hay mapeo
        // posible, así que el segundo clip no entra.
        if (std::fabs(span) < 1e-6f) return 0.0f;
        const float w = (getFloat(st.blendParam) - st.blendMin) / span;
        return w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
    }

    int AnimatorComponent::poseClipA() const
    {
        return blending() ? previousClipIndex() : currentClipIndex();
    }

    float AnimatorComponent::poseTimeA() const
    {
        return blending() ? m_prevAnimTime : m_animTime;
    }

    int AnimatorComponent::poseClipB() const
    {
        // Cross-fade: el destino aporta su clip PRIMARIO (solo caben dos clips).
        if (blending()) return currentClipIndex();
        if (!stateBlends(m_currentState)) return currentClipIndex();
        return m_states[m_currentState].blendClipIndex;
    }

    float AnimatorComponent::poseTimeB() const
    {
        if (blending()) return m_animTime;
        if (!stateBlends(m_currentState)) return m_animTime;

        // Los dos clips se muestrean en la MISMA fase normalizada. Un walk de
        // 40 ticks y un run de 100 mezclados por tiempo absoluto se
        // desincronizan y las piernas patinan; por fase, el pie de apoyo de uno
        // cae sobre el del otro.
        const State& st = m_states[m_currentState];
        if (st.duration <= 0.0f) return 0.0f;
        return (m_animTime / st.duration) * st.blendDuration;
    }

    float AnimatorComponent::poseWeight() const
    {
        if (blending()) return blendWeight();
        if (!stateBlends(m_currentState)) return 1.0f;
        return stateBlendWeight(m_currentState);
    }

    bool AnimatorComponent::poseLockRootMotion() const
    {
        // Durante un cross-fade manda el estado DESTINO, que ES m_currentState
        // (el que aporta poseClipB): no hay caso especial que escribir.
        if (m_currentState < 0 || m_currentState >= (int)m_states.size()) return false;
        return m_states[m_currentState].lockRootMotion;
    }

    std::string AnimatorComponent::previousStateName() const
    {
        if (m_prevState < 0 || m_prevState >= (int)m_states.size()) return "";
        return m_states[m_prevState].name;
    }

    void AnimatorComponent::resetPlayback()
    {
        m_currentState  = m_entryState;
        m_animTime      = 0.0f;
        m_finished      = false;
        // Corta cualquier cross-fade en vuelo: tras esto el estado previo puede
        // ni existir (el editor acaba de reeditar el grafo), y mezclar contra él
        // dejaría una pose imposible o un índice fuera de rango.
        m_prevState     = -1;
        m_prevAnimTime  = 0.0f;
        m_blendElapsed  = 0.0f;
        m_blendDuration = 0.0f;
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

        for (auto& st : m_states)
        {
            // El segundo clip del blend se resuelve SIEMPRE, aunque el primario
            // falle: los dos avisos son independientes y ver solo uno de ellos
            // mandaría a buscar al sitio equivocado.
            if (st.blendClipName.empty())
            {
                st.blendClipIndex = -1;
                st.blendDuration  = 0.0f;
            }
            else
            {
                const int b = findClip(st.blendClipName);
                st.blendClipIndex = b;
                st.blendDuration  = (b >= 0) ? mesh.animationClips[b].duration : 0.0f;
                if (b < 0 && warnings)
                    warnings->push_back("Animator: el estado '" + st.name + "' mezcla con el clip '" +
                                        st.blendClipName + "', que no existe en el modelo");
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

    void AnimatorComponent::bindClips(const SkinnedMesh& mesh, std::vector<std::string>* warnings)
    {
        rebindClips(mesh, warnings);
        reset();
    }

    int AnimatorComponent::renameClipReferences(const std::string& oldName,
                                                 const std::string& newName)
    {
        int changed = 0;
        for (auto& st : m_states)
        {
            // Un estado cuenta UNA vez aunque el rename le toque los dos clips:
            // lo que se devuelve son estados afectados, no referencias.
            bool touched = false;
            if (st.clipName == oldName)      { st.clipName      = newName; touched = true; }
            if (st.blendClipName == oldName) { st.blendClipName = newName; touched = true; }
            if (touched) changed++;
        }
        return changed;
    }

    bool AnimatorComponent::conditionsMet(const Transition& t) const
    {
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
                    if (!m_finished) return false;
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

    void AnimatorComponent::consumeTriggers(const Transition& t)
    {
        // Solo los de la transición que gana: un trigger que nadie consume sigue
        // armado esperando (mismo comportamiento que Unity).
        for (const auto& c : t.conditions)
            if (c.type == ConditionType::Trigger)
                m_triggers[c.paramName] = false;
    }

    void AnimatorComponent::advanceClock(const State& st, float& time, bool* finished, float dt)
    {
        if (st.duration > 0.0f && st.ticksPerSecond > 0.0f)
        {
            time += dt * st.ticksPerSecond;
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
            // jamás pondría m_finished a true: una salida "animation finished"
            // se quedaría esperando para siempre. Semánticamente un estado de
            // duración 0 ya ha terminado en el instante en que entra, así que
            // se reafirma finished cada frame (igual que el clamp de arriba lo
            // reafirma en el último frame de un clip normal sin loop).
            *finished = true;
        }
    }

    void AnimatorComponent::update(float dt, bool evaluateTransitions)
    {
        if (m_currentState < 0 || m_currentState >= (int)m_states.size())
        {
            m_currentState = m_entryState;
            if (m_currentState < 0 || m_currentState >= (int)m_states.size()) return;
        }

        advanceClock(m_states[m_currentState], m_animTime, &m_finished, dt);

        // Cross-fade en curso: el estado que se apaga sigue animándose con SU
        // ritmo y SU loop mientras dura la mezcla. Congelarlo daría un salto
        // visible justo al empezar la transición, que es lo contrario de lo que
        // el cross-fade viene a resolver.
        if (m_prevState >= 0)
        {
            if (m_prevState < (int)m_states.size())
                advanceClock(m_states[m_prevState], m_prevAnimTime, nullptr, dt);

            m_blendElapsed += dt;
            if (m_blendDuration <= 0.0f || m_blendElapsed >= m_blendDuration)
            {
                // Mezcla terminada: el destino se queda solo. A partir de aquí
                // blendWeight() vuelve a valer 1 por el camino de siempre.
                m_prevState     = -1;
                m_prevAnimTime  = 0.0f;
                m_blendElapsed  = 0.0f;
                m_blendDuration = 0.0f;
            }
        }

        if (!evaluateTransitions) return;

        // Orden de declaración: la primera cuyo AND se cumple, gana. Determinista
        // y sin prioridades explícitas que mantener.
        for (const auto& t : m_transitions)
        {
            if (t.fromState != m_currentState) continue;
            if (t.toState < 0 || t.toState >= (int)m_states.size()) continue;
            if (!conditionsMet(t)) continue;

            consumeTriggers(t);

            if (t.duration > 0.0f)
            {
                // El estado que dejamos pasa a ser el que se apaga, con el
                // tiempo que llevara. Si YA había una mezcla en vuelo se
                // descarta: mezclar tres clips necesitaría un tercer bloque en
                // el SSBO y en el push constant, así que la mezcla anterior se
                // corta aquí (mismo criterio que Unity con su capa base).
                m_prevState     = m_currentState;
                m_prevAnimTime  = m_animTime;
                m_blendElapsed  = 0.0f;
                m_blendDuration = t.duration;
            }
            else
            {
                // Corte seco: ni estado previo ni mezcla, el camino de siempre.
                m_prevState     = -1;
                m_prevAnimTime  = 0.0f;
                m_blendElapsed  = 0.0f;
                m_blendDuration = 0.0f;
            }

            m_currentState = t.toState;
            m_animTime     = 0.0f;
            m_finished     = false;
            return;                  // una transición por update
        }
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
