#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>

namespace DonTopo
{
    struct SkinnedMesh;

    // Máquina de estados de animación (equivalente al Animator de Unity). Cada
    // estado contiene un clip; los links son transiciones dirigidas.
    //
    // Data + lógica pura: sin Vulkan y sin conocer GameObject, misma regla que
    // CameraComponent y Rigidbody (la dependencia va Core -> resto, nunca al
    // revés). Eso es lo que deja probarlo entero sin GPU ni ventana.
    //
    // Es el ÚNICO dueño de animTime: el Renderer solo recibe (clip, tiempo) ya
    // calculados vía Renderer::setAnimationState. Partir el tiempo entre los dos
    // daría dos fuentes de verdad.
    //
    // Sin blending: una transición es un corte instantáneo, y solo se evalúa un
    // clip por frame.
    class AnimatorComponent
    {
        public:
            enum class ConditionType { Bool, Trigger, AnimationFinished };
            enum class ParamType     { Bool, Trigger };

            struct Condition
            {
                ConditionType type     = ConditionType::Bool;
                std::string   paramName;          // vacío si AnimationFinished
                bool          expected = true;    // solo Bool
            };

            struct Transition
            {
                int fromState = -1;
                int toState   = -1;
                // AND de todas: la transición dispara cuando se cumplen todas.
                std::vector<Condition> conditions;
            };

            struct State
            {
                std::string name;
                // El clip se referencia por NOMBRE, no por índice: el índice
                // depende del orden de mAnimations en el FBX, y reexportar el
                // modelo lo baraja. bindClips resuelve nombre -> clipIndex.
                std::string clipName;
                int         clipIndex      = -1;
                // Cacheados por bindClips pa que el componente sea auto-contenido
                // (y probable sin FBX ni Vulkan).
                float       duration       = 0.0f;    // ticks
                float       ticksPerSecond = 24.0f;
                // Autoría del usuario (checkbox del nodo), NO cacheado del clip:
                // el SkinnedMesh se reconstruye desde el FBX en cada carga y no
                // se serializa, así que un loop guardado ahí se perdería.
                bool        loop           = true;
                // Posición del nodo en el canvas del AnimatorPanel.
                glm::vec2   editorPos{0.0f};
            };

            struct Parameter
            {
                std::string name;
                ParamType   type = ParamType::Bool;
            };

            // --- Diseño (editor / carga de escena) ---
            int  addState(State s);                 // devuelve el índice del nuevo estado
            void addTransition(Transition t);
            void removeState(int idx);              // reindexa las transiciones
            void removeTransition(int idx);
            void setEntryState(int idx);
            void addParameter(std::string name, ParamType type);
            void removeParameter(const std::string& name);

            const std::vector<State>&      states()      const { return m_states; }
            const std::vector<Transition>& transitions() const { return m_transitions; }
            const std::vector<Parameter>&  parameters()  const { return m_parameters; }
            int                            entryState()  const { return m_entryState; }

            // Acceso mutable pa la UI (editar nombre/loop/editorPos in situ sin
            // reconstruir el estado entero).
            std::vector<State>&      statesMutable()      { return m_states; }
            std::vector<Transition>& transitionsMutable() { return m_transitions; }

            // Resuelve clipName -> clipIndex y cachea duration/ticksPerSecond de
            // cada estado. Un clipName que no exista en la malla deja clipIndex a
            // -1 y empuja un aviso (falla ruidoso, no silencioso). NO toca loop.
            void bindClips(const SkinnedMesh& mesh, std::vector<std::string>* warnings = nullptr);

            // --- Runtime ---
            void setBool(const std::string& n, bool v);
            bool getBool(const std::string& n) const;
            void setTrigger(const std::string& n);

            // evaluateTransitions == false (Edit Mode): avanza el tiempo del
            // estado actual pero no mueve el grafo.
            void update(float dt, bool evaluateTransitions);

            int   currentState()     const { return m_currentState; }
            int   currentClipIndex() const;
            float animTime()         const { return m_animTime; }   // ticks
            bool  finished()         const { return m_finished; }
            // Nombre del estado actual, "" si el grafo está vacío. Lo consume Lua.
            std::string currentStateName() const;

            // Vuelve al estado de entrada, tiempo a 0, parámetros y triggers a
            // false. El Stop de Play no necesita llamarlo (reconstruye la escena
            // desde JSON), pero el editor sí al reeditar el grafo.
            void reset();

        private:
            bool conditionsMet(const Transition& t) const;
            void consumeTriggers(const Transition& t);
            bool isTriggerSet(const std::string& n) const;
            bool hasParam(const std::string& n, ParamType type) const;

            std::vector<State>      m_states;
            std::vector<Transition> m_transitions;
            std::vector<Parameter>  m_parameters;
            int                     m_entryState   = -1;

            int                     m_currentState = -1;
            float                   m_animTime     = 0.0f;
            bool                    m_finished     = false;
            std::unordered_map<std::string, bool> m_bools;
            std::unordered_map<std::string, bool> m_triggers;
    };
}
