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
    // Cross-fade: una transición con duration > 0 mantiene vivo el estado que
    // se apaga durante esos segundos, así que hay DOS relojes y dos clips en
    // vuelo (currentClipIndex/animTime y previousClipIndex/previousAnimTime) más
    // el peso de la mezcla. Con duration == 0 no hay estado previo y el
    // comportamiento es el corte instantáneo de siempre.
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
                // Cross-fade, en SEGUNDOS reales (no ticks: los dos estados que
                // se mezclan pueden tener ticksPerSecond distintos, así que un
                // tiempo de mezcla en ticks no querría decir nada).
                //
                // 0 = corte instantáneo, que es lo que hacía el motor antes de
                // que este campo existiera y lo que trae toda escena guardada
                // sin él. Va AL FINAL del struct: los tests construyen
                // Transition por miembros y las condiciones se serializan por
                // nombre.
                float duration = 0.0f;
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
                // --- Blend por parámetro (dos clips en el mismo estado) ---
                // Segundo clip de la mezcla, por NOMBRE igual que clipName.
                // Vacío = estado de un solo clip, que es lo que hacía el motor
                // antes de este campo y lo que traen las escenas viejas.
                std::string blendClipName;
                int         blendClipIndex = -1;   // lo resuelve rebindClips
                float       blendDuration  = 0.0f; // ticks, cacheado del clip
                // Parámetro float que manda en el peso, remapeado de
                // [blendMin, blendMax] a [0, 1] y clampado. Vacío, o un
                // parámetro no declarado, deja el peso a 0 (solo clipName).
                std::string blendParam;
                float       blendMin       = 0.0f;
                float       blendMax       = 1.0f;
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
            // Termina en reset(): pensado pa carga de escena / entrada a Play,
            // donde reiniciar m_currentState y los parámetros es lo correcto.
            void bindClips(const SkinnedMesh& mesh, std::vector<std::string>* warnings = nullptr);

            // El bucle de resolución de bindClips, SIN el reset() final. Lo usan
            // los comandos del editor (AnimationSourceCommand) que mutan
            // animationClips en caliente: tras añadir/quitar una fuente de
            // animación, m_states[].clipIndex apunta a índices del array VIEJO
            // (o a un índice que ahora es un clip distinto, ver Finding 1 de la
            // revisión), así que hay que re-resolver por nombre. Pero es en
            // caliente: puede correr a mitad de Play Mode, y bindClips's reset()
            // borraría m_currentState y todos los bool/trigger/int/float del
            // usuario, que es justo lo que NO se quiere en ese momento (a
            // diferencia de una carga de escena, donde reset() es correcto).
            void rebindClips(const SkinnedMesh& mesh, std::vector<std::string>* warnings = nullptr);

            // Reescribe clipName en los estados que usaban oldName. Devuelve
            // cuántos cambió. Lo llama el Animator Panel tras renombrar un clip
            // del mesh: el grafo referencia por nombre, así que sin esto el
            // rename dejaría los estados huérfanos.
            int renameClipReferences(const std::string& oldName, const std::string& newName);

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

            // --- Cross-fade en curso ---
            // El estado que se está apagando, -1 si no hay mezcla. Su reloj
            // sigue corriendo (con SU ticksPerSecond y SU loop) mientras dura.
            int   previousState()     const { return m_prevState; }
            // Como currentClipIndex: cae a 0 si no hay estado previo o su clip
            // no está resuelto. 0 es un índice válido del SSBO, así que el
            // compute nunca lee fuera aunque el grafo esté a medias.
            int   previousClipIndex() const;
            float previousAnimTime()  const { return m_prevAnimTime; }   // ticks
            // 0 = solo el estado previo, 1 = solo el actual. Vale 1 cuando no
            // hay mezcla, que es justo lo que hace que el camino sin cross-fade
            // no necesite un caso especial en ningún consumidor.
            float blendWeight()       const;
            bool  blending()          const { return m_prevState >= 0; }

            // --- La pose que sale a la GPU ---
            // Los cinco valores que consume el Renderer: dos clips, sus dos
            // relojes y el peso (pose = mix(A, B, w)). Resuelven los DOS
            // orígenes de mezcla que hay:
            //   - cross-fade en vuelo: A = estado que se apaga, B = el nuevo.
            //   - si no, estado con blendClip: A = su clip, B = su blendClip,
            //     peso del parámetro float.
            //   - ninguno de los dos: A == B y peso 1 (una sola evaluación).
            // El cross-fade MANDA sobre el blend del estado: en el push
            // constant solo caben dos clips, así que mientras dura la
            // transición cada lado aporta su clip primario.
            int   poseClipA() const;
            float poseTimeA() const;   // ticks
            int   poseClipB() const;
            float poseTimeB() const;   // ticks
            float poseWeight() const;
            // Nombre del estado actual, "" si el grafo está vacío. Lo consume Lua.
            std::string currentStateName() const;
            // Nombre del estado que se está apagando en un cross-fade, "" si no
            // hay mezcla. Lo consume Lua, igual que currentStateName.
            std::string previousStateName() const;

            // Vuelve al estado de entrada, tiempo a 0, parámetros y triggers a
            // false. El Stop de Play no necesita llamarlo (reconstruye la escena
            // desde JSON), pero el editor sí al reeditar el grafo.
            void reset();

        private:
            bool conditionsMet(const Transition& t) const;
            // Avanza el reloj de un estado dt segundos, aplicando su loop. Lo
            // usan el estado actual y el que se apaga durante un cross-fade:
            // los dos tienen su propio ticksPerSecond y su propio loop, y
            // duplicar el bucle dejaría que se desincronizaran. finished solo
            // lo escribe el del estado actual (al previo ya no le importa).
            static void advanceClock(const State& st, float& time, bool* finished, float dt);
            // true si el estado tiene un segundo clip RESUELTO y un parámetro
            // que existe: solo entonces hay mezcla que hacer.
            bool  stateBlends(int stateIdx) const;
            // Peso del blend del estado, ya remapeado y clampado.
            float stateBlendWeight(int stateIdx) const;
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

            // Cross-fade en curso. m_prevState a -1 significa "sin mezcla", y es
            // el estado en el que queda todo con transiciones de duración 0.
            int                     m_prevState     = -1;
            float                   m_prevAnimTime  = 0.0f;
            float                   m_blendElapsed  = 0.0f;
            float                   m_blendDuration = 0.0f;
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
