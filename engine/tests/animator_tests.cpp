// Tests headless del Animator: carga de N clips, empaquetado GPU, máquina de
// estados y serialización. Plain main + asserts, sin framework — coherente con
// camera_tests.cpp y physics_tests.cpp.
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Renderer/SkinnedMeshPacking.h"
#include "DonTopo/Editor/Command.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.001f) { return std::fabs(a - b) < eps; }

// Criterio 1: todo clip cargado del FBX tiene nombre no vacío y único, duration
// y ticksPerSecond válidos. Ejercita el bucle sobre mAnimations aunque el asset
// del repo traiga una sola animación.
static void test_loader_reads_all_clips()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    CHECK(!m.skeleton.names.empty());
    CHECK(!m.animationClips.empty());

    for (size_t i = 0; i < m.animationClips.size(); i++)
    {
        const AnimationClip& c = m.animationClips[i];
        CHECK(!c.name.empty());
        CHECK(c.duration > 0.0f);
        CHECK(c.ticksPerSecond > 0.0f);
        CHECK(!c.channels.empty());
        // Nombres únicos entre sí: el Animator resuelve por nombre.
        for (size_t j = i + 1; j < m.animationClips.size(); j++)
            CHECK(m.animationClips[i].name != m.animationClips[j].name);
    }
}

// Los keyframes de los N clips van concatenados en los mismos vectores y
// boneInfos queda en layout [clip][hueso]. parentIndex/inverseBindPose son del
// esqueleto, no del clip: idénticos en todos los bloques — eso es lo que deja a
// bone_hierarchy.comp sin cambios.
static void test_pack_concatenates_clips()
{
    SkinnedMesh m;
    m.skeleton.names           = { "root", "child" };
    m.skeleton.parentIndex     = { -1, 0 };
    m.skeleton.inverseBindPose = { glm::mat4(2.0f), glm::mat4(3.0f) };

    // 3 clips; solo el hueso 0 tiene canal, con 1, 2 y 3 keys respectivamente.
    // El valor de posición lleva el índice del clip como marcador.
    for (int c = 0; c < 3; c++)
    {
        AnimationClip clip;
        clip.name           = "clip" + std::to_string(c);
        clip.duration       = 10.0f * (float)(c + 1);
        clip.ticksPerSecond = 24.0f;

        BoneChannel ch;
        ch.boneIndex = 0;
        for (int k = 0; k <= c; k++)
        {
            ch.posKeys.push_back({ (float)k, glm::vec3((float)c) });
            ch.rotKeys.push_back({ (float)k, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
            ch.scaleKeys.push_back({ (float)k, glm::vec3(1.0f) });
        }
        clip.channels.push_back(ch);
        m.animationClips.push_back(clip);
    }

    PackedClips p = packSkinnedClips(m);

    // C*B entradas
    CHECK(p.boneInfos.size() == 3u * 2u);
    // 1+2+3 keys, concatenadas sin solapar
    CHECK(p.pos.size() == 6u);
    CHECK(p.rot.size() == 6u);
    CHECK(p.scale.size() == 6u);

    // Layout [clip][hueso]: el hueso 0 del clip c está en c*2 + 0
    CHECK(p.boneInfos[0 * 2 + 0].posCount == 1);
    CHECK(p.boneInfos[1 * 2 + 0].posCount == 2);
    CHECK(p.boneInfos[2 * 2 + 0].posCount == 3);
    // El hueso 1 no tiene canal en ningún clip
    CHECK(p.boneInfos[0 * 2 + 1].posCount == 0);
    CHECK(p.boneInfos[2 * 2 + 1].posCount == 0);

    // Offsets crecientes y sin solape
    CHECK(p.boneInfos[0 * 2 + 0].posOffset == 0);
    CHECK(p.boneInfos[1 * 2 + 0].posOffset == 1);
    CHECK(p.boneInfos[2 * 2 + 0].posOffset == 3);

    // Datos de esqueleto replicados idénticos en cada bloque de clip
    for (int c = 0; c < 3; c++)
    {
        CHECK(p.boneInfos[c * 2 + 0].parentIndex == -1);
        CHECK(p.boneInfos[c * 2 + 1].parentIndex == 0);
        CHECK(p.boneInfos[c * 2 + 0].inverseBindPose == glm::mat4(2.0f));
        CHECK(p.boneInfos[c * 2 + 1].inverseBindPose == glm::mat4(3.0f));
    }

    // El bloque del clip 2 apunta a SUS keys, no a las del clip 0
    CHECK(nearlyEqual(p.pos[p.boneInfos[2 * 2 + 0].posOffset].value.x, 2.0f));
    CHECK(nearlyEqual(p.pos[p.boneInfos[0 * 2 + 0].posOffset].value.x, 0.0f));
}

// Malla sin animaciones: un bloque de clip con todos los counts a 0, y los
// buffers nunca vacíos (Vulkan no acepta buffers de tamaño 0).
static void test_pack_mesh_without_clips()
{
    SkinnedMesh m;
    m.skeleton.names           = { "root" };
    m.skeleton.parentIndex     = { -1 };
    m.skeleton.inverseBindPose = { glm::mat4(1.0f) };

    PackedClips p = packSkinnedClips(m);

    CHECK(p.boneInfos.size() == 1u);
    CHECK(p.boneInfos[0].posCount == 0);
    CHECK(p.boneInfos[0].parentIndex == -1);
    CHECK(p.pos.size() == 1u);     // dummy
    CHECK(p.rot.size() == 1u);
    CHECK(p.scale.size() == 1u);
}

// Helper: grafo Idle(loop) -> Run(loop) -> Jump(no loop) -> Idle.
// Idle->Run   por bool "running" == true
// Run->Jump   por trigger "jump"
// Jump->Idle  por animation finished
// Run->Idle   por bool "running" == false (cierra el ciclo del bool, así el
//             caso expected == false también queda ejercitado)
static AnimatorComponent makeGraph()
{
    AnimatorComponent a;

    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "Idle";
    idle.clipIndex = 0; idle.duration = 30.0f; idle.ticksPerSecond = 30.0f; idle.loop = true;

    AnimatorComponent::State run;
    run.name = "Run"; run.clipName = "Run";
    run.clipIndex = 1; run.duration = 20.0f; run.ticksPerSecond = 20.0f; run.loop = true;

    AnimatorComponent::State jump;
    jump.name = "Jump"; jump.clipName = "Jump";
    jump.clipIndex = 2; jump.duration = 10.0f; jump.ticksPerSecond = 10.0f; jump.loop = false;

    a.addState(idle);
    a.addState(run);
    a.addState(jump);
    a.setEntryState(0);

    a.addParameter("running", AnimatorComponent::ParamType::Bool);
    a.addParameter("jump",    AnimatorComponent::ParamType::Trigger);

    AnimatorComponent::Transition t0;
    t0.fromState = 0; t0.toState = 1;
    t0.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "running", true });
    a.addTransition(t0);

    AnimatorComponent::Transition t1;
    t1.fromState = 1; t1.toState = 2;
    t1.conditions.push_back({ AnimatorComponent::ConditionType::Trigger, "jump", true });
    a.addTransition(t1);

    AnimatorComponent::Transition t2;
    t2.fromState = 2; t2.toState = 0;
    t2.conditions.push_back({ AnimatorComponent::ConditionType::AnimationFinished, "", true });
    a.addTransition(t2);

    AnimatorComponent::Transition t3;
    t3.fromState = 1; t3.toState = 0;
    t3.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "running", false });
    a.addTransition(t3);

    a.reset();
    return a;
}

// Criterio 2, parte 1: un trigger cambia el estado activo; sin trigger, no.
static void test_trigger_switches_state()
{
    AnimatorComponent a = makeGraph();
    CHECK(a.currentState() == 0);           // entry

    // Sin parámetros, nada dispara
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);

    // bool running -> Run
    a.setBool("running", true);
    a.update(0.016f, true);
    CHECK(a.currentState() == 1);
    CHECK(a.currentClipIndex() == 1);

    // trigger jump -> Jump
    a.setTrigger("jump");
    a.update(0.016f, true);
    CHECK(a.currentState() == 2);
    CHECK(a.currentClipIndex() == 2);
    // Al entrar en un estado, el tiempo arranca de cero
    CHECK(nearlyEqual(a.animTime(), 0.0f));
}

// Criterio 2, parte 2: "animation finished" NO dispara antes de duration y SÍ
// después. Jump: duration 10 ticks a 10 tps = 1 segundo real.
static void test_animation_finished_timing()
{
    AnimatorComponent a = makeGraph();
    a.setBool("running", true);
    a.update(0.016f, true);          // -> Run
    a.setTrigger("jump");
    a.update(0.016f, true);          // -> Jump
    CHECK(a.currentState() == 2);

    // 0.9 s < 1.0 s de duración: no ha terminado, no transiciona
    a.update(0.9f, true);
    CHECK(!a.finished());
    CHECK(a.currentState() == 2);

    // Pasado el final: finished y transición a Idle
    a.update(0.2f, true);
    CHECK(a.currentState() == 0);
}

// Criterio 2, parte 3: loop=false se clava en el último frame; loop=true reinicia.
static void test_loop_flag()
{
    // loop = false: animTime se clampa a duration y se queda ahí
    AnimatorComponent noLoop;
    AnimatorComponent::State s;
    s.name = "Once"; s.clipName = "Once";
    s.clipIndex = 0; s.duration = 10.0f; s.ticksPerSecond = 10.0f; s.loop = false;
    noLoop.addState(s);
    noLoop.setEntryState(0);
    noLoop.reset();

    noLoop.update(2.0f, true);       // 20 ticks > 10 de duración
    CHECK(nearlyEqual(noLoop.animTime(), 10.0f));
    CHECK(noLoop.finished());
    noLoop.update(2.0f, true);       // sigue clavado, no vuelve al principio
    CHECK(nearlyEqual(noLoop.animTime(), 10.0f));
    CHECK(noLoop.finished());

    // loop = true: reinicia y nunca se marca finished
    AnimatorComponent looping;
    AnimatorComponent::State l;
    l.name = "Cycle"; l.clipName = "Cycle";
    l.clipIndex = 0; l.duration = 10.0f; l.ticksPerSecond = 10.0f; l.loop = true;
    looping.addState(l);
    looping.setEntryState(0);
    looping.reset();

    looping.update(1.2f, true);      // 12 ticks -> fmod -> 2
    CHECK(nearlyEqual(looping.animTime(), 2.0f));
    CHECK(!looping.finished());
}

// Condición bool: dispara con expected cumplido, no con el contrario. Ida Y
// vuelta: si expected se ignorase (p.ej. tratando Bool como "getBool(param)
// == true" a secas), Idle->Run seguiría funcionando pero Run->Idle jamás
// dispararía con running == false, así que este segundo tramo es el que
// realmente prueba que expected == false se respeta.
static void test_bool_condition_expected()
{
    AnimatorComponent a = makeGraph();
    a.setBool("running", false);
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);    // expected == true, running == false

    a.setBool("running", true);
    a.update(0.016f, true);
    CHECK(a.currentState() == 1);    // Idle -> Run: expected == true, running == true

    a.setBool("running", false);
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);    // Run -> Idle: expected == false, running == false
}

// El trigger que consume la transición ganadora se apaga. Uno no consumido sigue
// encendido esperando (mismo comportamiento que Unity).
static void test_trigger_consumption()
{
    AnimatorComponent a = makeGraph();

    // "jump" seteado en Idle: ninguna transición que salga de Idle lo consume,
    // así que sigue armado cuando lleguemos a Run.
    a.setTrigger("jump");
    a.setBool("running", true);
    a.update(0.016f, true);          // Idle -> Run (por el bool)
    CHECK(a.currentState() == 1);

    a.update(0.016f, true);          // Run -> Jump: el trigger seguía armado
    CHECK(a.currentState() == 2);

    // Ya consumido: al volver a Idle y luego a Run, no vuelve a saltar
    a.update(2.0f, true);            // Jump termina -> Idle
    CHECK(a.currentState() == 0);
    a.update(0.016f, true);          // -> Run (running sigue true)
    CHECK(a.currentState() == 1);
    a.update(0.016f, true);          // se queda: el trigger ya se gastó
    CHECK(a.currentState() == 1);
}

// evaluateTransitions == false (Edit Mode): el tiempo avanza pero el grafo no se
// mueve. Sin esto, las condiciones "animation finished" pasearían el grafo solo
// en el editor.
static void test_edit_mode_does_not_transition()
{
    AnimatorComponent a = makeGraph();
    a.setBool("running", true);
    a.setTrigger("jump");

    a.update(0.5f, false);
    CHECK(a.currentState() == 0);            // no se ha movido
    CHECK(a.animTime() > 0.0f);              // pero el tiempo corre

    a.update(0.016f, true);
    CHECK(a.currentState() == 1);            // en Play sí
}

// Fix #2 (task-16): editorId identifica un estado independientemente de su
// posición en el vector. Sin esto, el AnimatorPanel deriva el id del nodo del
// canvas del índice del vector, y borrar un estado de en medio reindexa el
// vector: el superviviente que hereda el índice del borrado hereda también su
// id de nodo, y como imgui-node-editor cachea posición/selección POR id, ese
// superviviente "salta" a la posición/estado visual del nodo borrado. Este
// test discrimina exactamente eso: si editorId fuera == índice (el bug),
// tras borrar el estado del medio el superviviente que pasa de índice 2 a 1
// cambiaría de id (2 -> 1) y este CHECK fallaría.
static void test_addstate_assigns_stable_unique_editor_ids()
{
    AnimatorComponent a;

    AnimatorComponent::State sa; sa.name = "A"; sa.clipName = "A";
    AnimatorComponent::State sb; sb.name = "B"; sb.clipName = "B";
    AnimatorComponent::State sc; sc.name = "C"; sc.clipName = "C";

    a.addState(sa);
    a.addState(sb);
    a.addState(sc);

    const int idA = a.states()[0].editorId;
    const int idB = a.states()[1].editorId;
    const int idC = a.states()[2].editorId;

    // Recién asignados: únicos entre sí.
    CHECK(idA != idB);
    CHECK(idB != idC);
    CHECK(idA != idC);

    a.removeState(1);    // borra "B", el vector reindexa: A pasa a quedarse en
                          // 0 (ya estaba) y C pasa del índice 2 al 1.
    CHECK(a.states().size() == 2u);
    CHECK(a.states()[0].name == "A");
    CHECK(a.states()[1].name == "C");

    // Identidad, no posición: A sigue con idA (índice sin cambios) y C sigue
    // con idC AUNQUE su índice haya cambiado de 2 a 1. Con ids derivados del
    // índice, este segundo CHECK fallaría (C tendría el id que antes era de B).
    CHECK(a.states()[0].editorId == idA);
    CHECK(a.states()[1].editorId == idC);
    CHECK(a.states()[0].editorId != a.states()[1].editorId);

    // Un estado añadido después del borrado saca un id fresco, distinto de
    // todos los que siguen vivos (el contador no retrocede ni reutiliza el id
    // que quedó libre al borrar B).
    AnimatorComponent::State sd; sd.name = "D"; sd.clipName = "D";
    a.addState(sd);
    const int idD = a.states()[2].editorId;
    CHECK(idD != idA);
    CHECK(idD != idC);
}

// Borrar un estado reindexa las transiciones que apuntaban por encima y tira las
// que lo tocaban. Sin esto, borrar un nodo dejaría links apuntando a otro estado.
static void test_remove_state_reindexes()
{
    AnimatorComponent a = makeGraph();
    a.removeState(1);                        // borra "Run"

    CHECK(a.states().size() == 2u);
    CHECK(a.states()[0].name == "Idle");
    CHECK(a.states()[1].name == "Jump");
    // Idle->Run y Run->Jump se van; Jump->Idle sobrevive reindexada (2 -> 1)
    CHECK(a.transitions().size() == 1u);
    CHECK(a.transitions()[0].fromState == 1);
    CHECK(a.transitions()[0].toState == 0);
    CHECK(a.entryState() == 0);
}

// Orden de declaración: si dos transiciones que salen del mismo estado pueden
// cumplirse a la vez, gana la declarada primero, no la última que el bucle
// visite. Grafo standalone (no makeGraph): A tiene dos salidas hacia B y C,
// ambas con la misma condición "flag" == true.
static void test_first_matching_transition_wins()
{
    AnimatorComponent a;

    AnimatorComponent::State sa;
    sa.name = "A"; sa.clipName = "A"; sa.clipIndex = 0;

    AnimatorComponent::State sb;
    sb.name = "B"; sb.clipName = "B"; sb.clipIndex = 1;

    AnimatorComponent::State sc;
    sc.name = "C"; sc.clipName = "C"; sc.clipIndex = 2;

    a.addState(sa);
    a.addState(sb);
    a.addState(sc);
    a.setEntryState(0);

    a.addParameter("flag", AnimatorComponent::ParamType::Bool);

    // Declarada PRIMERO: A -> B
    AnimatorComponent::Transition toB;
    toB.fromState = 0; toB.toState = 1;
    toB.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "flag", true });
    a.addTransition(toB);

    // Declarada DESPUÉS, con la misma condición: A -> C
    AnimatorComponent::Transition toC;
    toC.fromState = 0; toC.toState = 2;
    toC.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "flag", true });
    a.addTransition(toC);

    a.reset();
    a.setBool("flag", true);         // ambas condiciones se cumplen a la vez
    a.update(0.016f, true);

    CHECK(a.currentState() == 1);    // B gana: es la primera en orden de declaración
}

// Una transición sin condiciones nunca dispara (exit time está fuera de
// alcance). Grafo standalone de dos estados con un único link vacío de
// condiciones: tras varios update(), el grafo debe seguir en el estado de
// entrada.
static void test_transition_without_conditions_never_fires()
{
    AnimatorComponent a;

    AnimatorComponent::State s0;
    s0.name = "X"; s0.clipName = "X"; s0.clipIndex = 0;

    AnimatorComponent::State s1;
    s1.name = "Y"; s1.clipName = "Y"; s1.clipIndex = 1;

    a.addState(s0);
    a.addState(s1);
    a.setEntryState(0);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;    // sin condiciones
    a.addTransition(t);

    a.reset();
    for (int i = 0; i < 5; i++)
        a.update(0.1f, true);

    CHECK(a.currentState() == 0);      // se queda en X: el link vacío nunca dispara
}

// removeParameter("") no debe tocar las condiciones AnimationFinished: su
// paramName es "" por diseño (no representan un parámetro real), y borrar con
// nombre vacío las dejaría huérfanas de un plumazo.
static void test_remove_parameter_ignores_empty_name()
{
    AnimatorComponent a = makeGraph();       // Jump->Idle lleva una condición AnimationFinished

    auto countFinishedConditions = [&a]() {
        size_t n = 0;
        for (const auto& t : a.transitions())
            for (const auto& c : t.conditions)
                if (c.type == AnimatorComponent::ConditionType::AnimationFinished) n++;
        return n;
    };

    CHECK(countFinishedConditions() == 1u);

    a.removeParameter("");

    CHECK(countFinishedConditions() == 1u);  // sigue viva: el nombre vacío no cuenta como parámetro real
}

// Fix #3: si removeParameter deja una transición sin condiciones (era su
// única condición), esa transición debe desaparecer del grafo, no quedarse
// como un link dibujado en el canvas que jamás puede disparar (conditionsMet
// exige al menos una condición). Grafo standalone de dos salidas desde A:
// T1 solo por bool "p" (se vacía y debe caer), T2 por bool "p" Y trigger "q"
// (pierde la de "p" pero sobrevive con "q").
static void test_remove_parameter_drops_emptied_transitions()
{
    AnimatorComponent a;

    AnimatorComponent::State sa;
    sa.name = "A"; sa.clipName = "A"; sa.clipIndex = 0;
    AnimatorComponent::State sb;
    sb.name = "B"; sb.clipName = "B"; sb.clipIndex = 1;
    AnimatorComponent::State sc;
    sc.name = "C"; sc.clipName = "C"; sc.clipIndex = 2;

    a.addState(sa);
    a.addState(sb);
    a.addState(sc);
    a.setEntryState(0);

    a.addParameter("p", AnimatorComponent::ParamType::Bool);
    a.addParameter("q", AnimatorComponent::ParamType::Trigger);

    AnimatorComponent::Transition t1;    // A -> B, únicamente por "p"
    t1.fromState = 0; t1.toState = 1;
    t1.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "p", true });
    a.addTransition(t1);

    AnimatorComponent::Transition t2;    // A -> C, por "p" Y "q"
    t2.fromState = 0; t2.toState = 2;
    t2.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "p", true });
    t2.conditions.push_back({ AnimatorComponent::ConditionType::Trigger, "q", true });
    a.addTransition(t2);

    CHECK(a.transitions().size() == 2u);

    a.removeParameter("p");

    // T1 se queda sin condiciones y desaparece; T2 sobrevive con solo "q".
    CHECK(a.transitions().size() == 1u);
    CHECK(a.transitions()[0].toState == 2);
    CHECK(a.transitions()[0].conditions.size() == 1u);
    CHECK(a.transitions()[0].conditions[0].type == AnimatorComponent::ConditionType::Trigger);
    CHECK(a.transitions()[0].conditions[0].paramName == "q");
}

// Fix #2: un estado con duration <= 0 (clip sin resolver, o de duración
// real cero) nunca entra en el bloque de avance de tiempo, así que sin el
// fix jamás marcaría m_finished y una salida "animation finished" se
// quedaría esperando para siempre (grafo aparcado en A). Grafo standalone de
// dos estados A(duration=0)->B por una única condición AnimationFinished.
static void test_animation_finished_fires_on_zero_duration_state()
{
    AnimatorComponent a;

    AnimatorComponent::State sa;
    sa.name = "A"; sa.clipName = "A"; sa.clipIndex = -1;   // clip sin resolver
    sa.duration = 0.0f; sa.ticksPerSecond = 30.0f; sa.loop = false;

    AnimatorComponent::State sb;
    sb.name = "B"; sb.clipName = "B"; sb.clipIndex = 0;
    sb.duration = 10.0f; sb.ticksPerSecond = 10.0f;

    a.addState(sa);
    a.addState(sb);
    a.setEntryState(0);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    t.conditions.push_back({ AnimatorComponent::ConditionType::AnimationFinished, "", true });
    a.addTransition(t);

    a.reset();
    CHECK(a.currentState() == 0);
    CHECK(!a.finished());       // recién entrado: reset() lo pone a false

    a.update(0.016f, true);     // un solo update: sin el fix se quedaría en A

    CHECK(a.currentState() == 1);
}

// bindClips resuelve el clip por NOMBRE y cachea duration/ticksPerSecond. Un
// nombre que no exista deja clipIndex a -1 y avisa: falla ruidoso, no silencioso.
// loop NO se toca: es del usuario, no del FBX.
static void test_bind_clips_resolves_by_name()
{
    SkinnedMesh mesh;
    mesh.skeleton.names           = { "root" };
    mesh.skeleton.parentIndex     = { -1 };
    mesh.skeleton.inverseBindPose = { glm::mat4(1.0f) };

    AnimationClip idle;  idle.name = "Idle"; idle.duration = 30.0f; idle.ticksPerSecond = 30.0f;
    AnimationClip run;   run.name  = "Run";  run.duration  = 20.0f; run.ticksPerSecond  = 20.0f;
    mesh.animationClips = { idle, run };

    AnimatorComponent a;
    AnimatorComponent::State s0; s0.name = "A"; s0.clipName = "Run";     s0.loop = false;
    AnimatorComponent::State s1; s1.name = "B"; s1.clipName = "NoExiste"; s1.loop = true;
    a.addState(s0);
    a.addState(s1);

    std::vector<std::string> warnings;
    a.bindClips(mesh, &warnings);

    // Resuelto por nombre, no por orden de declaración en el grafo
    CHECK(a.states()[0].clipIndex == 1);
    CHECK(nearlyEqual(a.states()[0].duration, 20.0f));
    CHECK(nearlyEqual(a.states()[0].ticksPerSecond, 20.0f));
    // loop lo puso el usuario: bindClips no lo pisa
    CHECK(a.states()[0].loop == false);

    // Clip inexistente: -1 y aviso
    CHECK(a.states()[1].clipIndex == -1);
    CHECK(warnings.size() == 1u);

    // clipIndex -1 no puede reventar el Renderer: currentClipIndex cae a 0
    a.setEntryState(1);
    CHECK(a.currentClipIndex() == 0);
}

// El slot sigue el patrón de CameraComponent, pero sin invariante de unicidad:
// cada GameObject skinned puede tener el suyo.
static void test_gameobject_animator_slot()
{
    GameObject go("personaje");
    CHECK(!go.hasAnimator());
    CHECK(go.getAnimator() == nullptr);

    go.setAnimator(std::make_shared<AnimatorComponent>());
    CHECK(go.hasAnimator());

    go.setAnimator(nullptr);
    CHECK(!go.hasAnimator());
}

// Criterio 3: el grafo entero (nodos, posiciones, links, condiciones,
// parámetros, loop por nodo y estado de entrada) sobrevive guardar -> cargar.
static void test_graph_survives_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    // nodeFromJson reutiliza el "id" del JSON cuando existe, así que el mismo id
    // resuelve el nodo en la escena cargada. Scene no tiene findByName.
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();

    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "ClipIdle";
    idle.loop = true;  idle.editorPos = glm::vec2(10.0f, 20.0f);
    AnimatorComponent::State jump;
    jump.name = "Jump"; jump.clipName = "ClipJump";
    jump.loop = false; jump.editorPos = glm::vec2(300.0f, 40.0f);
    a->addState(idle);
    a->addState(jump);
    a->setEntryState(1);                    // entrada NO por defecto, a propósito

    a->addParameter("running", AnimatorComponent::ParamType::Bool);
    a->addParameter("jump",    AnimatorComponent::ParamType::Trigger);

    AnimatorComponent::Transition t0;
    t0.fromState = 0; t0.toState = 1;
    t0.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "running", false });
    t0.conditions.push_back({ AnimatorComponent::ConditionType::Trigger, "jump", true });
    a->addTransition(t0);

    AnimatorComponent::Transition t1;
    t1.fromState = 1; t1.toState = 0;
    t1.conditions.push_back({ AnimatorComponent::ConditionType::AnimationFinished, "", true });
    a->addTransition(t1);

    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Personaje");
    CHECK(found->hasAnimator());
    if (!found->hasAnimator()) return;
    const AnimatorComponent& r = *found->getAnimator();

    // Estados: nombre, clip, loop, posición del nodo
    CHECK(r.states().size() == 2u);
    CHECK(r.states()[0].name == "Idle");
    CHECK(r.states()[0].clipName == "ClipIdle");
    CHECK(r.states()[0].loop == true);
    CHECK(nearlyEqual(r.states()[0].editorPos.x, 10.0f));
    CHECK(nearlyEqual(r.states()[0].editorPos.y, 20.0f));
    CHECK(r.states()[1].name == "Jump");
    CHECK(r.states()[1].clipName == "ClipJump");
    CHECK(r.states()[1].loop == false);
    CHECK(nearlyEqual(r.states()[1].editorPos.x, 300.0f));

    // Estado de entrada
    CHECK(r.entryState() == 1);

    // Parámetros con su tipo
    CHECK(r.parameters().size() == 2u);
    CHECK(r.parameters()[0].name == "running");
    CHECK(r.parameters()[0].type == AnimatorComponent::ParamType::Bool);
    CHECK(r.parameters()[1].name == "jump");
    CHECK(r.parameters()[1].type == AnimatorComponent::ParamType::Trigger);

    // Links y condiciones (incluido expected == false, que un default a true
    // se comería sin que nada fallara)
    CHECK(r.transitions().size() == 2u);
    CHECK(r.transitions()[0].fromState == 0);
    CHECK(r.transitions()[0].toState == 1);
    CHECK(r.transitions()[0].conditions.size() == 2u);
    CHECK(r.transitions()[0].conditions[0].type == AnimatorComponent::ConditionType::Bool);
    CHECK(r.transitions()[0].conditions[0].paramName == "running");
    CHECK(r.transitions()[0].conditions[0].expected == false);
    CHECK(r.transitions()[0].conditions[1].type == AnimatorComponent::ConditionType::Trigger);
    CHECK(r.transitions()[1].conditions[0].type == AnimatorComponent::ConditionType::AnimationFinished);
}

// Bloque aditivo: una escena guardada antes de que existiera "animator" carga
// igual, sin animator y sin avisos.
static void test_scene_without_animator_block_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    const uint64_t id = scene.addGameObject("Vacio")->id;
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(!found->hasAnimator());
}

// Add/Remove del componente pasan por el stack de undo. El comando guarda una
// COPIA del grafo pa que un Add-undo-redo no lo devuelva a un componente vacío.
static void test_animator_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    AnimatorComponent st;
    AnimatorComponent::State s; s.name = "Idle"; s.clipName = "ClipIdle"; s.loop = false;
    st.addState(s);
    st.addParameter("running", AnimatorComponent::ParamType::Bool);

    AnimatorComponentCommand cmd(scene, "Añadir Animator", id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasAnimator());
    CHECK(go->getAnimator()->states().size() == 1u);

    cmd.undo();
    CHECK(!go->hasAnimator());

    // Redo: el grafo vuelve entero, no un componente vacío
    cmd.execute();
    CHECK(go->hasAnimator());
    CHECK(go->getAnimator()->states().size() == 1u);
    CHECK(go->getAnimator()->states()[0].name == "Idle");
    CHECK(go->getAnimator()->states()[0].loop == false);
    CHECK(go->getAnimator()->parameters().size() == 1u);
}

// El Remove es el mismo comando con add=false: execute quita, undo devuelve.
static void test_animator_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    AnimatorComponent st;
    AnimatorComponent::State s; s.name = "Idle"; s.clipName = "ClipIdle";
    st.addState(s);
    go->setAnimator(std::make_shared<AnimatorComponent>(st));

    AnimatorComponentCommand cmd(scene, "Quitar Animator", id, /*add=*/false, st);
    cmd.execute();
    CHECK(!go->hasAnimator());
    cmd.undo();
    CHECK(go->hasAnimator());
    CHECK(go->getAnimator()->states().size() == 1u);
}

// El objeto puede haber desaparecido entre el execute y el undo: se resuelve por
// id en cada uno, nunca por puntero crudo, así que no debe crashear.
static void test_animator_command_survives_missing_target()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    AnimatorComponentCommand cmd(scene, "Añadir Animator", id, /*add=*/true, AnimatorComponent{});
    scene.removeGameObject(go);
    cmd.execute();   // findById devuelve nullptr y sale sin tocar nada
    cmd.undo();
}

// La API que consume Lua: los parámetros se consultan por nombre y solo si
// están declarados en el grafo. Un nombre no declarado se ignora en vez de
// crear un parámetro fantasma que ninguna condición miraría.
static void test_parameter_api_ignores_undeclared()
{
    AnimatorComponent a = makeGraph();

    a.setBool("running", true);
    CHECK(a.getBool("running"));

    // No declarado: ni se guarda ni revienta
    a.setBool("noExiste", true);
    CHECK(!a.getBool("noExiste"));

    // Tipo equivocado: "jump" es trigger, no bool
    a.setBool("jump", true);
    CHECK(!a.getBool("jump"));

    CHECK(a.currentStateName() == "Idle");
    a.update(0.016f, true);
    CHECK(a.currentStateName() == "Run");
}

// Criterio: un parámetro Int dispara una transición según su comparador. Se
// construye un grafo mínimo aparte de makeGraph() pa no tocar los tests ya
// existentes, que dependen de su forma exacta.
static AnimatorComponent makeNumericGraph()
{
    AnimatorComponent a;

    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "Idle";
    idle.clipIndex = 0; idle.duration = 30.0f; idle.ticksPerSecond = 30.0f; idle.loop = true;

    AnimatorComponent::State run;
    run.name = "Run"; run.clipName = "Run";
    run.clipIndex = 1; run.duration = 20.0f; run.ticksPerSecond = 20.0f; run.loop = true;

    a.addState(idle);
    a.addState(run);
    a.setEntryState(0);

    a.addParameter("combo", AnimatorComponent::ParamType::Int);
    a.addParameter("speed", AnimatorComponent::ParamType::Float);

    a.reset();
    return a;
}

static void test_int_condition_greater_and_equals()
{
    AnimatorComponent a = makeNumericGraph();

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition c;
    c.type      = AnimatorComponent::ConditionType::Int;
    c.paramName = "combo";
    c.compare   = AnimatorComponent::Compare::Greater;
    c.threshold = 2.0f;
    t.conditions.push_back(c);
    a.addTransition(t);

    // Valor inicial 0: no dispara
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);

    // Igual al umbral: Greater es estricto, sigue sin disparar
    a.setInt("combo", 2);
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);

    a.setInt("combo", 3);
    a.update(0.016f, true);
    CHECK(a.currentState() == 1);

    // Equals: mismo grafo, otro comparador
    AnimatorComponent b = makeNumericGraph();
    AnimatorComponent::Transition tb;
    tb.fromState = 0; tb.toState = 1;
    AnimatorComponent::Condition cb;
    cb.type      = AnimatorComponent::ConditionType::Int;
    cb.paramName = "combo";
    cb.compare   = AnimatorComponent::Compare::Equals;
    cb.threshold = 3.0f;
    tb.conditions.push_back(cb);
    b.addTransition(tb);

    b.setInt("combo", 4);
    b.update(0.016f, true);
    CHECK(b.currentState() == 0);

    b.setInt("combo", 3);
    b.update(0.016f, true);
    CHECK(b.currentState() == 1);
}

static void test_float_condition_less()
{
    AnimatorComponent a = makeNumericGraph();

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition c;
    c.type      = AnimatorComponent::ConditionType::Float;
    c.paramName = "speed";
    c.compare   = AnimatorComponent::Compare::Less;
    c.threshold = -1.0f;
    t.conditions.push_back(c);
    a.addTransition(t);

    // Valor inicial 0.0f: no es menor que -1.0f
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);

    a.setFloat("speed", -2.5f);
    CHECK(nearlyEqual(a.getFloat("speed"), -2.5f));
    a.update(0.016f, true);
    CHECK(a.currentState() == 1);
}

// La UI solo ofrece Greater/Less para Float (un == sobre float casi nunca
// dispara), pero el evaluador implementa los cuatro comparadores: un JSON
// editado a mano con Equals/NotEquals sobre un float se evalúa, no se ignora
// en silencio (ver AnimatorPanel::drawConditionsPopup y conditionsMet). Se usa
// 2.5f, exactamente representable en binario, pa que el test sea sobre el
// comparador y no sobre precisión de punto flotante.
static void test_float_condition_equals_and_not_equals()
{
    AnimatorComponent a = makeNumericGraph();

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition c;
    c.type      = AnimatorComponent::ConditionType::Float;
    c.paramName = "speed";
    c.compare   = AnimatorComponent::Compare::Equals;
    c.threshold = 2.5f;
    t.conditions.push_back(c);
    a.addTransition(t);

    // Distinto del umbral: Equals no dispara
    a.setFloat("speed", 1.0f);
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);

    // Coincidencia exacta: Equals sí dispara
    a.setFloat("speed", 2.5f);
    a.update(0.016f, true);
    CHECK(a.currentState() == 1);

    // NotEquals: mismo grafo, comparador contrario
    AnimatorComponent b = makeNumericGraph();
    AnimatorComponent::Transition tb;
    tb.fromState = 0; tb.toState = 1;
    AnimatorComponent::Condition cb;
    cb.type      = AnimatorComponent::ConditionType::Float;
    cb.paramName = "speed";
    cb.compare   = AnimatorComponent::Compare::NotEquals;
    cb.threshold = 2.5f;
    tb.conditions.push_back(cb);
    b.addTransition(tb);

    // Igual al umbral: NotEquals no dispara
    b.setFloat("speed", 2.5f);
    b.update(0.016f, true);
    CHECK(b.currentState() == 0);

    // Distinto: NotEquals sí dispara
    b.setFloat("speed", 1.0f);
    b.update(0.016f, true);
    CHECK(b.currentState() == 1);
}

// Misma guarda que setBool: un nombre no declarado o de otro tipo se ignora en
// vez de crear un parámetro fantasma que ninguna condición miraría.
static void test_numeric_api_type_guards()
{
    AnimatorComponent a = makeNumericGraph();
    a.addParameter("running", AnimatorComponent::ParamType::Bool);

    a.setInt("combo", 7);
    CHECK(a.getInt("combo") == 7);
    a.setFloat("speed", 1.5f);
    CHECK(nearlyEqual(a.getFloat("speed"), 1.5f));

    // No declarado
    a.setInt("noExiste", 5);
    CHECK(a.getInt("noExiste") == 0);
    a.setFloat("tampoco", 5.0f);
    CHECK(nearlyEqual(a.getFloat("tampoco"), 0.0f));

    // Tipo equivocado en ambos sentidos
    a.setInt("speed", 9);            // speed es float
    CHECK(a.getInt("speed") == 0);
    a.setFloat("combo", 9.0f);       // combo es int
    CHECK(nearlyEqual(a.getFloat("combo"), 0.0f));
    a.setInt("running", 1);          // running es bool
    CHECK(a.getInt("running") == 0);

    // reset devuelve los numéricos a cero, igual que los bools
    a.reset();
    CHECK(a.getInt("combo") == 0);
    CHECK(nearlyEqual(a.getFloat("speed"), 0.0f));
}

// removeParameter ya limpiaba bools y triggers; los numéricos entran en el mismo
// camino, incluida la poda de transiciones que se quedan sin condiciones.
static void test_remove_numeric_parameter_cleans_conditions()
{
    AnimatorComponent a = makeNumericGraph();

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition c;
    c.type      = AnimatorComponent::ConditionType::Int;
    c.paramName = "combo";
    c.compare   = AnimatorComponent::Compare::Greater;
    c.threshold = 0.0f;
    t.conditions.push_back(c);
    a.addTransition(t);
    CHECK(a.transitions().size() == 1);

    a.removeParameter("combo");
    CHECK(a.parameters().size() == 1);          // solo queda "speed"
    CHECK(a.transitions().empty());             // se quedó sin condiciones
    CHECK(a.getInt("combo") == 0);
}

// Round-trip completo de un grafo con parámetros y condiciones numéricas: si
// compare o threshold no sobrevivieran, la transición cargada dispararía cuando
// no debe (o nunca).
static void test_numeric_graph_survives_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();

    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "ClipIdle"; idle.loop = true;
    AnimatorComponent::State run;
    run.name = "Run"; run.clipName = "ClipRun"; run.loop = true;
    a->addState(idle);
    a->addState(run);

    a->addParameter("combo", AnimatorComponent::ParamType::Int);
    a->addParameter("speed", AnimatorComponent::ParamType::Float);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition ci;
    ci.type      = AnimatorComponent::ConditionType::Int;
    ci.paramName = "combo";
    ci.compare   = AnimatorComponent::Compare::NotEquals;
    ci.threshold = 4.0f;
    t.conditions.push_back(ci);
    AnimatorComponent::Condition cf;
    cf.type      = AnimatorComponent::ConditionType::Float;
    cf.paramName = "speed";
    cf.compare   = AnimatorComponent::Compare::Less;
    cf.threshold = 2.5f;
    t.conditions.push_back(cf);
    a->addTransition(t);

    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->hasAnimator());
    if (!found->hasAnimator()) return;

    auto la = found->getAnimator();
    CHECK(la->parameters().size() == 2);
    CHECK(la->parameters()[0].type == AnimatorComponent::ParamType::Int);
    CHECK(la->parameters()[1].type == AnimatorComponent::ParamType::Float);

    CHECK(la->transitions().size() == 1);
    if (la->transitions().empty()) return;
    const auto& lc = la->transitions()[0].conditions;
    CHECK(lc.size() == 2);
    if (lc.size() != 2) return;

    CHECK(lc[0].type    == AnimatorComponent::ConditionType::Int);
    CHECK(lc[0].compare == AnimatorComponent::Compare::NotEquals);
    CHECK(nearlyEqual(lc[0].threshold, 4.0f));
    CHECK(lc[1].type    == AnimatorComponent::ConditionType::Float);
    CHECK(lc[1].compare == AnimatorComponent::Compare::Less);
    CHECK(nearlyEqual(lc[1].threshold, 2.5f));
}

// Retrocompatibilidad: una condición guardada antes de este cambio no lleva
// "compare" ni "threshold" y debe cargar con los defaults del struct, sin
// warnings ni excepciones de nlohmann.
static void test_condition_without_compare_fields_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "ClipIdle";
    AnimatorComponent::State run;
    run.name = "Run"; run.clipName = "ClipRun";
    a->addState(idle);
    a->addState(run);
    a->addParameter("running", AnimatorComponent::ParamType::Bool);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    t.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "running", true });
    a->addTransition(t);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    // Una condición Bool no debe emitir los campos numéricos: son ruido en el
    // .scene y confundirían a quien lo lea a mano. El árbol es
    // root["root"]["children"], no un array plano (ver Scene::toJson).
    bool checkedEmission = false;
    for (const auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        const auto& cond = node["animator"]["transitions"][0]["conditions"][0];
        CHECK(!cond.contains("compare"));
        CHECK(!cond.contains("threshold"));
        checkedEmission = true;
    }
    CHECK(checkedEmission);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found || !found->hasAnimator()) return;

    const auto& lc = found->getAnimator()->transitions()[0].conditions[0];
    CHECK(lc.type    == AnimatorComponent::ConditionType::Bool);
    CHECK(lc.expected);
    CHECK(lc.compare == AnimatorComponent::Compare::Greater);   // default
    CHECK(nearlyEqual(lc.threshold, 0.0f));                     // default
}

int main()
{
    // Una sola PxFoundation por proceso: un único PhysicsManager compartido por
    // todos los tests, nunca uno por test. physics/audio solo hacen falta porque
    // Scene::fromJson los exige en su firma pa recrear colliders y clips — estos
    // tests no simulan nada.
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    am.init();

    test_loader_reads_all_clips();
    test_pack_concatenates_clips();
    test_pack_mesh_without_clips();

    test_trigger_switches_state();
    test_animation_finished_timing();
    test_loop_flag();
    test_bool_condition_expected();
    test_trigger_consumption();
    test_edit_mode_does_not_transition();
    test_addstate_assigns_stable_unique_editor_ids();
    test_remove_state_reindexes();
    test_first_matching_transition_wins();
    test_transition_without_conditions_never_fires();
    test_remove_parameter_ignores_empty_name();
    test_remove_parameter_drops_emptied_transitions();
    test_parameter_api_ignores_undeclared();
    test_int_condition_greater_and_equals();
    test_float_condition_less();
    test_float_condition_equals_and_not_equals();
    test_numeric_api_type_guards();
    test_remove_numeric_parameter_cleans_conditions();
    test_animation_finished_fires_on_zero_duration_state();

    test_bind_clips_resolves_by_name();
    test_gameobject_animator_slot();

    test_graph_survives_scene_round_trip(pm, am);
    test_scene_without_animator_block_loads(pm, am);
    test_numeric_graph_survives_scene_round_trip(pm, am);
    test_condition_without_compare_fields_loads(pm, am);

    test_animator_command_add_undo_redo();
    test_animator_command_remove();
    test_animator_command_survives_missing_target();

    am.shutdown();
    pm.shutdown();
    if (g_failures) { std::printf("dt_animator_tests: %d FAILURES\n", g_failures); std::fflush(stdout); return 1; }
    std::printf("dt_animator_tests: OK\n");
    std::fflush(stdout);
    return 0;
}
