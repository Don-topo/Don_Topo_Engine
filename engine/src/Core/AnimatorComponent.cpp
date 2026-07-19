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

        reset();
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
        reset();
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

    std::string AnimatorComponent::currentStateName() const
    {
        if (m_currentState < 0 || m_currentState >= (int)m_states.size()) return "";
        return m_states[m_currentState].name;
    }

    void AnimatorComponent::reset()
    {
        m_currentState = m_entryState;
        m_animTime     = 0.0f;
        m_finished     = false;
        for (auto& b : m_bools)    b.second = false;
        for (auto& t : m_triggers) t.second = false;
        for (auto& i : m_ints)     i.second = 0;
        for (auto& f : m_floats)   f.second = 0.0f;
    }

    void AnimatorComponent::rebindClips(const SkinnedMesh& mesh, std::vector<std::string>* warnings)
    {
        for (auto& st : m_states)
        {
            int found = -1;
            for (size_t i = 0; i < mesh.animationClips.size(); i++)
                if (mesh.animationClips[i].name == st.clipName) { found = (int)i; break; }

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
            if (st.clipName != oldName) continue;
            st.clipName = newName;
            changed++;
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

    void AnimatorComponent::update(float dt, bool evaluateTransitions)
    {
        if (m_currentState < 0 || m_currentState >= (int)m_states.size())
        {
            m_currentState = m_entryState;
            if (m_currentState < 0 || m_currentState >= (int)m_states.size()) return;
        }

        const State& st = m_states[m_currentState];
        if (st.duration > 0.0f && st.ticksPerSecond > 0.0f)
        {
            m_animTime += dt * st.ticksPerSecond;
            if (m_animTime >= st.duration)
            {
                if (st.loop)
                {
                    m_animTime = std::fmod(m_animTime, st.duration);
                }
                else
                {
                    // Clavado en el último frame, y así se queda en los updates
                    // siguientes.
                    m_animTime = st.duration;
                    m_finished = true;
                }
            }
        }
        else
        {
            // Un clip sin resolver (clipIndex == -1, duration a 0) o de
            // duración cero real nunca entraría en el bloque de arriba y
            // jamás pondría m_finished a true: una salida "animation finished"
            // se quedaría esperando para siempre. Semánticamente un estado de
            // duración 0 ya ha terminado en el instante en que entra, así que
            // se reafirma finished cada frame (igual que el clamp de arriba lo
            // reafirma en el último frame de un clip normal sin loop).
            m_finished = true;
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
