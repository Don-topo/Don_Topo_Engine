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
            // Los valores nuevos van AL FINAL: los tests usan inicialización
            // agregada de Condition y la serialización va por string, así que
            // añadir por el medio rompería lo primero sin ganar nada.
            enum class ConditionType { Bool, Trigger, AnimationFinished, Int, Float };
            enum class ParamType     { Bool, Trigger, Int, Float };
            // Comparadores de las condiciones numéricas: los cuatro valen tanto
            // para Int como para Float. Sobre un float, Equals exige igualdad
            // binaria exacta — un valor calculado casi nunca la cumple, uno
            // asignado con setFloat sí.
            enum class Compare       { Greater, Less, Equals, NotEquals };

            struct Condition
            {
                ConditionType type     = ConditionType::Bool;
                std::string   paramName;          // vacío si AnimationFinished
                bool          expected = true;    // solo Bool
                // Solo Int/Float. Un único umbral en float sirve a los dos: la
                // UI de Int usa DragInt, así que siempre entra un valor íntegro,
                // y float representa enteros exactos hasta 2^24.
                Compare       compare   = Compare::Greater;
                float         threshold = 0.0f;
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
                // Id estable pa el nodo del canvas del editor (AnimatorPanel), NO
                // el índice en m_states: ese índice cambia cuando removeState
                // reindexa el vector, y si el id del canvas fuera el índice, un
                // superviviente heredaría el slot visual (posición/selección) del
                // nodo borrado en imgui-node-editor, que los cachea por id. NO se
                // serializa (ver Scene.cpp): se regenera en addState al cargar.
                int         editorId = -1;
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
            // Devuelven 0 si el parámetro no existe; los setters no hacen nada
            // si el nombre no está declarado o es de otro tipo (misma guarda que
            // setBool).
            void  setInt(const std::string& n, int v);
            int   getInt(const std::string& n) const;
            void  setFloat(const std::string& n, float v);
            float getFloat(const std::string& n) const;

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
            // Estático porque no toca estado: aísla los cuatro comparadores en
            // un sitio y sirve tanto a Int como a Float.
            template <typename T>
            static bool evalCompare(T value, Compare op, T threshold)
            {
                switch (op)
                {
                    case Compare::Greater:   return value >  threshold;
                    case Compare::Less:      return value <  threshold;
                    case Compare::Equals:    return value == threshold;
                    case Compare::NotEquals: return value != threshold;
                }
                return false;
            }
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
            std::unordered_map<std::string, int>    m_ints;
            std::unordered_map<std::string, float>  m_floats;

            // Siguiente editorId a repartir en addState. Nunca se resetea ni se
            // reutiliza un id liberado por removeState: mientras el panel esté
            // abierto en el mismo frame de un borrado, un id repetido volvería a
            // liar la identidad visual que este campo existe para evitar.
            int                     m_nextEditorId = 0;
    };

    // Etiqueta legible de un tipo de parámetro, compartida por AnimatorPanel y
    // PropertiesPanel. Vive aquí y no en el editor porque con cuatro tipos el
    // ternario "trigger : bool" que ambos duplicaban deja de funcionar, y dos
    // copias de un switch se desincronizan al añadir el quinto tipo.
    //
    // NO reutiliza (ni la reutiliza) paramTypeToStr de Scene.cpp: aquello es el
    // formato del .scene y no puede cambiar al retocar un texto de la UI.
    const char* paramTypeLabel(AnimatorComponent::ParamType t);
}
