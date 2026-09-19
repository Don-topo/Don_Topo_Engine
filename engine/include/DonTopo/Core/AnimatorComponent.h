#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include "DonTopo/Core/AnimationPose.h"

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

            // fromState de una transición que sale de "Any State": vale desde
            // cualquier estado. Negativo a propósito: removeState solo
            // reindexa índices >= 0, así que el centinela sobrevive intacto.
            static constexpr int kAnyState = -2;

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
                // --- Exit time (como Unity) ---
                // Con hasExitTime la transición espera a que el estado de
                // origen llegue a exitTime, en tiempo NORMALIZADO (1 = fin del
                // clip). Por debajo de 1 se comprueba en cada vuelta; a partir
                // de 1 cuenta vueltas acumuladas (2.5 = dos vueltas y media).
                // Sin condiciones basta el tiempo; con condiciones hacen falta
                // las dos cosas. Al final del struct y apagado por defecto: es
                // lo que traen las escenas guardadas sin estos campos.
                bool  hasExitTime = false;
                float exitTime    = 1.0f;
                // Solo se lee en transiciones que salen de Any State: si puede
                // volver al estado en el que ya se está. Apagado por defecto:
                // encendido y con un bool, reiniciaría el estado cada frame.
                bool  canTransitionToSelf = false;
            };

            // Un clip extra de un blend 1D. El nombre es la autoría; índice y
            // duración los resuelve rebindClips desde la malla y no se guardan.
            struct BlendEntry
            {
                std::string clipName;
                int         clipIndex = -1;
                float       duration  = 0.0f;   // ticks
                float       threshold = 0.0f;
                float       thresholdY = 0.0f;   // solo en blend 2D
            };

            // Evento con nombre en un instante del ciclo del estado. time es
            // fase normalizada [0, 1] sobre la duración del clip principal.
            struct AnimationEvent
            {
                std::string name;
                float       time = 0.0f;
            };

            // Qué hace la traslación de la raíz del clip. Off: la pose la
            // mueve (lo de siempre). Lock: se clava a su bind y el clip se ve
            // en el sitio. Apply: la GPU clava solo X y Z (el vaivén vertical
            // se ve) y el desplazamiento horizontal mueve al GameObject.
            enum class RootMotion { Off, Lock, Apply };

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
                // --- Blend 1D por parámetro ---
                // El clip principal (clipName) es una entrada más, con
                // clipThreshold; blendEntries son los extra. Suenan los dos
                // vecinos del valor de blendParam (ver stateBlendPair). Sin
                // entradas, o con un parámetro no declarado, un solo clip.
                std::string             blendParam;
                float                   clipThreshold = 0.0f;
                std::vector<BlendEntry> blendEntries;
                // --- Blend 2D ---
                // Con blendParamY Float declarado, cada clip es un punto
                // (umbral, umbralY) y suenan los 3 del triángulo que contiene
                // (blendParam, blendParamY). Ver stateBlendSamples.
                std::string             blendParamY;
                float                   clipThresholdY = 0.0f;
                // Eventos del estado: disparan en Play (ver collectEvents) y
                // llegan a Lua como OnAnimationEvent(name).
                std::vector<AnimationEvent> events;
                // Posición del nodo en el canvas del AnimatorPanel.
                glm::vec2   editorPos{0.0f};
                // Id estable pa el nodo del canvas del editor (AnimatorPanel), NO
                // el índice en m_states: ese índice cambia cuando removeState
                // reindexa el vector, y si el id del canvas fuera el índice, un
                // superviviente heredaría el slot visual (posición/selección) del
                // nodo borrado en imgui-node-editor, que los cachea por id. NO se
                // serializa (ver Scene.cpp): se regenera en addState al cargar.
                int         editorId = -1;
                // Traslación de la raíz (el hueso de parentIndex < 0), ver
                // RootMotion. La rotación y la escala de la raíz NO se tocan en
                // ningún modo. Off es lo que traen todas las escenas guardadas
                // sin el campo.
                RootMotion  rootMotion = RootMotion::Off;
                // --- Velocidad (como el Speed + Multiplier de Unity) ---
                // Ritmo = ticksPerSecond x speed x valor de speedParam (si es un
                // float declarado; si no, x1). Negativo o NaN congela (0): ir
                // hacia atrás exigiría redefinir loop, finished y exit time. Al
                // final del struct y a x1 por defecto, que es lo que traen las
                // escenas guardadas sin estos campos.
                float       speed          = 1.0f;
                std::string speedParam;
            };

            struct Parameter
            {
                std::string name;
                ParamType   type = ParamType::Bool;
            };

            // Lo AUTORADO del grafo, sin nada de runtime: lo que guarda y
            // restaura el undo del editor (AnimatorGraphCommand). Los estados
            // van enteros —editorId y editorPos incluidos— porque applyGraph
            // necesita el editorId para casar los estados vivos con los del
            // snapshot, y la posición para colocar un nodo que vuelve de un
            // borrado.
            struct Graph
            {
                std::vector<State>      states;
                std::vector<Transition> transitions;
                std::vector<Parameter>  parameters;
                int                     entryState = -1;
            };

            // --- Diseño (editor / carga de escena) ---
            int  addState(State s);                 // devuelve el índice del nuevo estado
            void addTransition(Transition t);
            void removeState(int idx);              // reindexa las transiciones
            void removeTransition(int idx);
            void setEntryState(int idx);
            void addParameter(std::string name, ParamType type);
            void removeParameter(const std::string& name);
            // Posición del nodo Any State en el canvas del AnimatorPanel. Va
            // fuera de Graph a propósito: como la de los estados, mover un nodo
            // no entra en el undo.
            glm::vec2 anyStateEditorPos() const         { return m_anyStateEditorPos; }
            void      setAnyStateEditorPos(glm::vec2 p) { m_anyStateEditorPos = p; }

            Graph graph() const;
            // Sustituye estados, transiciones, parámetros y entrada por los de
            // g SIN pasar por reset(): corre en Play (undo a mitad de partida).
            //  - El playhead se casa por editorId, no por índice: si el estado
            //    actual sigue en g conserva su tiempo aunque cambie de índice;
            //    si no, cae a la entrada con tiempo 0. El que se apaga en un
            //    cross-fade se casa igual, y la mezcla solo se corta si falta
            //    alguno de los dos.
            //  - Un parámetro conserva su valor si ya existía con el mismo
            //    nombre Y el mismo tipo; si no, arranca a su valor por defecto.
            //  - Los estados vivos (mismo editorId) conservan su editorPos
            //    actual: mover nodos no entra en el undo.
            //  - m_nextEditorId nunca baja.
            // NO resuelve clips: el caché de clipIndex lo rehace el llamante
            // con rebindClips.
            // Precondición: g viene de graph() (editorId únicos, índices dentro
            // de rango); applyGraph no lo valida.
            void applyGraph(const Graph& g);

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

            // Desarma un trigger que nadie ha consumido todavía. Nombre no
            // declarado o de otro tipo: no hace nada, como setTrigger.
            void  resetTrigger(const std::string& n);

            // --- Control desde código (Lua) ---
            // Entran en el estado con ese NOMBRE sin esperar a ninguna
            // transición. false si no existe (no se mueve nada). Hacia el
            // estado actual lo reinician: una llamada explícita es intención,
            // no el rebote que canTransitionToSelf evita en el grafo.
            // play corta cualquier mezcla; crossFade mezcla durante seconds
            // (<= 0 = corte, igual que play).
            bool  play(const std::string& stateName);
            bool  crossFade(const std::string& stateName, float seconds);
            // Tiempo normalizado ACUMULADO del estado actual: 1 = una vuelta, y
            // en un loop sigue creciendo (como normalizedTime en Unity). 0 si
            // el clip no tiene duración.
            float normalizedTime() const;
            // Velocidad global del Animator (animator.speed de Unity): escala el
            // dt de todo update, cross-fade incluido. Runtime, no se guarda en
            // la escena. Negativo o NaN se acota a 0 (congela).
            void  setSpeed(float s);
            float speed() const { return m_speed; }

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
            // Fade en curso: con un estado previo vivo, o desde una pose
            // congelada (un fade interrumpido, ver pose()). blending() sigue
            // diciendo solo lo primero, que es lo que miran el root motion y
            // la pareja principal.
            bool  fading()            const { return m_prevState >= 0 || m_frozenFade; }
            // Lo que va a la GPU: hasta 4 muestras ponderadas (el estado que
            // sale y el que entra, cada uno con su pareja de blend) y la pose
            // congelada si un fade se interrumpió. Los pesos suman 1.
            AnimationPose pose() const;
            // La petición de congelar se manda UNA vez: la apaga quien acaba de
            // enviar la pose al backend (applySkinnedFrame).
            void clearFreezeRequest() { m_freezePending = false; }

            // --- La pareja PRINCIPAL de la pose ---
            // Dos clips, sus dos relojes y el peso (mix(A, B, w)):
            //   - cross-fade en vuelo: A = estado que se apaga, B = el nuevo,
            //     cada uno con su clip primario.
            //   - si no, estado con blend: los dos clips vecinos del valor del
            //     parámetro entre sus umbrales, con peso lineal.
            //   - ninguno de los dos: A == B y peso 1.
            // Es la vista de dos clips de siempre, para Lua y los tests. Lo que
            // va a la GPU es pose(), que en un fade lleva las parejas enteras de
            // los dos estados (y la pose congelada si se interrumpió).
            int   poseClipA() const;
            float poseTimeA() const;   // ticks
            int   poseClipB() const;
            float poseTimeB() const;   // ticks
            float poseWeight() const;
            // Nombres disparados en el ÚLTIMO update, en orden. Se vacía al
            // principio de cada update: quien los lea una vez por frame los ve
            // una sola vez.
            const std::vector<std::string>& firedEvents() const { return m_firedEvents; }
            // Lo que avanzó cada clip con root motion en el ÚLTIMO update (solo
            // Play y solo si el estado actual es Apply), con su peso en la pose.
            // Ticks acumulados, sin wrap. Lo convierte en delta rootMotionDelta
            // (Renderer/RootMotion.h), que es quien tiene los keyframes.
            struct RootMotionSample { int clip; double ticks0; double ticks1; float duration; bool loop; float weight; };
            const std::vector<RootMotionSample>& rootMotionSamples() const { return m_rootMotionSamples; }
            // Modo de raíz de la pose que sale a la GPU: 0 Off, 1 Lock, 2 Apply.
            // Durante un cross-fade manda el estado DESTINO —el mismo que aporta
            // poseClipB—: el push constant lleva UN solo modo para toda la
            // mezcla, y el destino es el estado al que se está entrando, así que
            // la pose acaba de acuerdo con él.
            uint32_t poseRootMotionMode() const;
            // Nombre del estado actual, "" si el grafo está vacío. Lo consume Lua.
            std::string currentStateName() const;
            // Nombre del estado que se está apagando en un cross-fade, "" si no
            // hay mezcla. Lo consume Lua, igual que currentStateName.
            std::string previousStateName() const;

            // Vuelve al estado de entrada, tiempo a 0, parámetros y triggers a
            // false. El Stop de Play no necesita llamarlo (reconstruye la escena
            // desde JSON), pero el editor sí al reeditar el grafo.
            //
            // OJO: borra los parámetros del usuario, así que NO vale para las
            // operaciones que pueden correr a mitad de Play — el AnimatorPanel
            // deja editar el grafo sin gate de isPlaying. Ésas usan
            // resetPlayback(), que mueve el playhead sin tocar los valores (la
            // misma distinción que hay entre bindClips y rebindClips).
            void reset();

            // Blend 2D: hay blend (stateBlends) y blendParamY es Float declarado.
            bool stateBlends2D(int stateIdx) const;
            // Las muestras de un estado con SU reloj: 1 o 2 en 1D (la pareja de
            // siempre, peso 0 incluido) y hasta 3 en 2D. Pesos que suman 1.
            struct BlendSample { int clip; float time; float weight; float duration; };
            int stateBlendSamples(int stateIdx, float animTime, BlendSample out[3]) const;

        private:
            // Deja el playhead en el estado de entrada y corta cualquier
            // cross-fade, SIN tocar bools/triggers/ints/floats. Es la mitad de
            // reset() que sí es segura a mitad de partida.
            void resetPlayback();

            bool conditionsMet(const Transition& t) const;
            // Único punto de entrada a un estado: fija el actual y pone a 0 su
            // reloj, su finished y el reloj normalizado acumulado. Todo camino
            // que reinicie el playhead pasa por aquí, para que el reloj del
            // exit time no quede colgado en el que se olvide.
            void enterState(int idx);
            // Arranca el paso al estado idx: con duration > 0 el actual pasa a
            // apagarse, si no se corta cualquier mezcla; después enterState.
            // Lo comparten una transición del grafo, play y crossFade.
            void startTransitionTo(int idx, float duration);
            // Índice del estado con ese nombre, -1 si no hay.
            int  stateIndexByName(const std::string& name) const;
            // Si la transición puede disparar este frame. n0/n1: tiempo
            // normalizado acumulado del estado actual antes y después de
            // avanzar el reloj. hasDuration false = clip de duración 0 o sin
            // resolver, donde el exit time cuenta como alcanzado.
            bool transitionReady(const Transition& t, double n0, double n1, bool hasDuration) const;
            // La regla del exit time, aislada para que se lea en un sitio.
            static bool exitTimeCrossed(double n0, double n1, float exitTime);
            // Avanza el reloj de un estado dt segundos, aplicando su loop. Lo
            // usan el estado actual y el que se apaga durante un cross-fade:
            // los dos tienen su propio ticksPerSecond y su propio loop, y
            // duplicar el bucle dejaría que se desincronizaran. finished solo
            // lo escribe el del estado actual (al previo ya no le importa).
            static void advanceClock(const State& st, float rate, float& time, bool* finished, float dt);
            // Empuja a m_firedEvents los eventos de st cuyo instante cae en
            // [ticks0, ticks1), una vez por ciclo cruzado (solo el primero
            // sin loop), con tope de kMaxEventCyclesPerUpdate por evento.
            void collectEvents(const State& st, double ticks0, double ticks1);
            // Rellena m_rootMotionSamples con lo avanzado en este update por el
            // estado actual (y el que se apaga, en un fade). ticks0 y
            // prevTicks0: los relojes acumulados ANTES de avanzar.
            void collectRootMotion(double ticks0, double prevTicks0);
            static constexpr int kMaxEventCyclesPerUpdate = 16;
            // Ticks por segundo efectivos del estado: ticksPerSecond x speed x
            // parámetro multiplicador, nunca negativo.
            float stateRate(const State& st) const;
            // true si hay al menos una entrada con clip RESUELTO y blendParam
            // es un Float declarado: solo entonces hay mezcla que hacer.
            bool stateBlends(int stateIdx) const;
            // Los dos clips vecinos del parámetro, sus tiempos y el peso.
            struct BlendPair { int clipA; float timeA; int clipB; float timeB; float weight;
                               float durA = 0.0f; float durB = 0.0f; };   // duración de cada clip, ticks
            BlendPair stateBlendPair(int stateIdx, float animTime) const;
            // La pareja del blend 1D (los dos vecinos del parámetro).
            BlendPair stateBlendPair1D(int stateIdx, float animTime) const;
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
            // Ticks avanzados desde que se entró en el estado actual, SIN fmod:
            // en un loop sigue creciendo, que es lo que permite contar vueltas
            // para el exit time. double y no float: en una sesión larga un
            // float pierde resolución para decidir un cruce. No se serializa.
            double                  m_stateTicks   = 0.0;
            std::vector<std::string> m_firedEvents;
            std::vector<RootMotionSample> m_rootMotionSamples;
            // Reloj acumulado (sin wrap) del estado que se apaga en un fade:
            // lo necesita el root motion para no saltar en su wrap.
            double                  m_prevStateTicks = 0.0;
            // Fade desde una pose congelada (se interrumpió otro fade) y la
            // petición de copiarla, pendiente hasta que el host la manda.
            bool                    m_frozenFade    = false;
            bool                    m_freezePending = false;
            float                   m_speed        = 1.0f;

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

            // A la izquierda del primer estado que crea el panel (40, 40).
            glm::vec2               m_anyStateEditorPos{ -220.0f, 40.0f };
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
