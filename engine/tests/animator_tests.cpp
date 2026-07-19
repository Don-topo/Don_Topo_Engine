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
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
#include "DonTopo/Editor/Command.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

// El FBX del propio modelo se registra como fuente "builtin": es la entrada de
// la lista que la UI muestra sin botón de borrar, y la que la escena reconstruye
// vía mesh.sourcePath en vez de vía addAnimationSource.
static void test_loader_registers_builtin_source()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");

    CHECK(m.animationSources.size() == 1u);
    if (m.animationSources.empty()) return;

    const AnimationSource& src = m.animationSources[0];
    CHECK(src.builtin == true);
    CHECK(src.path == "assets/modelAnimation.fbx");
    // clipNames refleja exactamente los clips cargados, en el mismo orden
    CHECK(src.clipNames.size() == m.animationClips.size());
    for (size_t i = 0; i < src.clipNames.size() && i < m.animationClips.size(); i++)
        CHECK(src.clipNames[i] == m.animationClips[i].name);
}

// El gate del Animator pregunta por isSkinned(), y hasta ahora el import del
// editor descartaba huesos y pesos llamando siempre a load(). hasBones es quien
// decide: mira el fichero, no al llamante.
static void test_has_bones_detects_rigged_fbx()
{
    CHECK(ModelLoader::hasBones("assets/modelAnimation.fbx") == true);
}

// Escribe un OBJ mínimo (triángulo, sin huesos: el formato OBJ no tiene
// concepto de esqueleto) en el directorio temporal del sistema. Helper
// compartido por los dos tests de "modelo sin rig" de abajo: cada uno pide un
// nombre de fichero distinto para no pisarse si llegaran a correr en paralelo.
static std::string writeUnriggedObjFixture(const std::string& filename)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / filename;
    std::ofstream f(path);
    f << "v 0 0 0\n"
         "v 1 0 0\n"
         "v 0 1 0\n"
         "f 1 2 3\n";
    return path.string();
}

// El repo no tiene ningún modelo sin rig: los dos únicos FBX trackeados,
// model.fbx y modelTexture.fbx, parecen props estáticos por el nombre pero
// son personajes Mixamo completos (65 y 67 huesos respectivamente,
// verificado con un probe de Assimp antes de escribir este test). Por eso el
// caso negativo se genera aquí en vez de apuntar a un asset del repo: un OBJ
// no puede declarar huesos ni queriendo, así que es un negativo real y no un
// accidente de qué .fbx haya en assets/. Si alguien "arregla" esto apuntando
// otra vez a model.fbx o modelTexture.fbx, el test vuelve a fallar.
//
// Un FBX/OBJ sin rig tiene que seguir entrando como Mesh plano: cargarlo
// skinned pagaría vértices de 112 B y una SSBO de huesos vacía para nada.
static void test_has_bones_rejects_unrigged_model()
{
    const std::string path = writeUnriggedObjFixture("dt_test_has_bones_unrigged.obj");
    CHECK(ModelLoader::hasBones(path) == false);
    std::filesystem::remove(path);
}

// No lanza: el fichero ilegible lo reporta el loader real, con su mensaje. Si
// hasBones lanzara, el import moriría antes de llegar a ese mensaje.
static void test_has_bones_survives_missing_file()
{
    bool threw = false;
    bool result = true;
    try { result = ModelLoader::hasBones("assets/no_existe_este_fichero.fbx"); }
    catch (...) { threw = true; }
    CHECK(!threw);
    CHECK(result == false);
}

// loadAuto devuelve el tipo dinámico correcto: es lo único que mira
// GameObject::isSkinned(), y por tanto lo único que habilita el Animator.
static void test_load_auto_returns_skinned_for_rigged()
{
    std::shared_ptr<Mesh> m = ModelLoader::loadAuto("assets/modelAnimation.fbx");
    CHECK(m != nullptr);
    if (!m) return;
    SkinnedMesh* sm = dynamic_cast<SkinnedMesh*>(m.get());
    CHECK(sm != nullptr);
    if (!sm) return;
    CHECK(!sm->skeleton.names.empty());
    // La fuente builtin la crea loadSkinned; loadAuto no debe alterarla.
    CHECK(sm->animationSources.size() == 1u);
}

// Mismo motivo que test_has_bones_rejects_unrigged_model: no hay ningún FBX
// sin rig en el repo (model.fbx y modelTexture.fbx son personajes Mixamo
// rigged pese al nombre), así que el negativo de loadAuto también se genera
// como OBJ en vez de apuntar a un asset de assets/.
static void test_load_auto_returns_static_for_unrigged()
{
    const std::string path = writeUnriggedObjFixture("dt_test_load_auto_unrigged.obj");
    std::shared_ptr<Mesh> m = ModelLoader::loadAuto(path);
    std::filesystem::remove(path);
    CHECK(m != nullptr);
    if (!m) return;
    CHECK(dynamic_cast<SkinnedMesh*>(m.get()) == nullptr);
    CHECK(!m->vertices.empty());
}

// La regla de nombres únicos es la misma que ya aplicaba el loader (Mixamo
// exporta todo como "mixamo.com"), pero ahora vive en un sitio compartido: la
// usan tanto loadSkinned como la importación de ficheros extra.
static void test_unique_clip_name()
{
    std::vector<AnimationClip> existing;
    AnimationClip a; a.name = "walk";       existing.push_back(a);
    AnimationClip b; b.name = "walk (1)";   existing.push_back(b);

    CHECK(uniqueClipName(existing, "run")  == "run");
    CHECK(uniqueClipName(existing, "walk") == "walk (2)");
    // Nombre vacío: no puede quedarse vacío, el Animator resuelve por nombre
    CHECK(uniqueClipName(existing, "")     == "Animation");
}

// Importar animaciones contra el esqueleto del propio fichero tiene que dar
// exactamente lo mismo que loadSkinned: mismos clips, mismos boneIndex, sin
// avisos. Es el caso "el FBX de animación es del mismo rig que el modelo".
static void test_load_animation_clips_matches_full_load()
{
    SkinnedMesh base = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    LoadedClips lc = ModelLoader::loadAnimationClips("assets/modelAnimation.fbx", base.skeleton);

    CHECK(lc.warnings.empty());
    CHECK(lc.clips.size() == base.animationClips.size());
    CHECK(lc.mappedChannels == lc.totalChannels);
    CHECK(lc.totalChannels > 0);

    for (size_t i = 0; i < lc.clips.size() && i < base.animationClips.size(); i++)
    {
        const AnimationClip& a = lc.clips[i];
        const AnimationClip& b = base.animationClips[i];
        CHECK(nearlyEqual(a.duration, b.duration));
        CHECK(nearlyEqual(a.ticksPerSecond, b.ticksPerSecond));
        CHECK(a.channels.size() == b.channels.size());
        for (size_t c = 0; c < a.channels.size() && c < b.channels.size(); c++)
        {
            CHECK(a.channels[c].boneIndex == b.channels[c].boneIndex);
            CHECK(a.channels[c].posKeys.size()   == b.channels[c].posKeys.size());
            CHECK(a.channels[c].rotKeys.size()   == b.channels[c].rotKeys.size());
            CHECK(a.channels[c].scaleKeys.size() == b.channels[c].scaleKeys.size());
        }
    }
}

// Esqueleto ajeno: ningún hueso casa, así que no hay nada que importar. Se avisa
// y se devuelven 0 clips — el caller lo convierte en rechazo.
static void test_load_animation_clips_against_foreign_skeleton()
{
    Skeleton foreign;
    foreign.names           = { "hueso_que_no_existe" };
    foreign.parentIndex     = { -1 };
    foreign.inverseBindPose = { glm::mat4(1.0f) };
    foreign.boneMap         = { { "hueso_que_no_existe", 0 } };

    LoadedClips lc = ModelLoader::loadAnimationClips("assets/modelAnimation.fbx", foreign);

    CHECK(lc.clips.empty());
    CHECK(!lc.warnings.empty());
    CHECK(lc.mappedChannels == 0);
    CHECK(lc.totalChannels > 0);
}

// Esqueleto parcial: se importa lo que casa y se avisa de lo que no. Es el caso
// real de un rig al que le faltan huesos de dedos respecto al FBX de animación.
static void test_load_animation_clips_partial_skeleton()
{
    SkinnedMesh base = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    CHECK(base.skeleton.names.size() >= 2u);
    if (base.skeleton.names.size() < 2u) return;

    // Esqueleto recortado: solo el primer hueso del original
    Skeleton partial;
    partial.names           = { base.skeleton.names[0] };
    partial.parentIndex     = { -1 };
    partial.inverseBindPose = { base.skeleton.inverseBindPose[0] };
    partial.boneMap         = { { base.skeleton.names[0], 0 } };

    LoadedClips lc = ModelLoader::loadAnimationClips("assets/modelAnimation.fbx", partial);

    CHECK(!lc.warnings.empty());                 // avisa de los huesos ignorados
    CHECK(lc.mappedChannels < lc.totalChannels);
    // Los canales que sobreviven apuntan al único hueso del esqueleto recortado
    for (const auto& c : lc.clips)
        for (const auto& ch : c.channels)
            CHECK(ch.boneIndex == 0);
}

// Fichero inexistente: no lanza, avisa. La UI lo convierte en un mensaje rojo,
// y Scene::fromJson en un log — ninguno de los dos quiere una excepción.
static void test_load_animation_clips_missing_file()
{
    Skeleton skel;
    skel.names = { "root" }; skel.parentIndex = { -1 };
    skel.inverseBindPose = { glm::mat4(1.0f) };
    skel.boneMap = { { "root", 0 } };

    LoadedClips lc = ModelLoader::loadAnimationClips("assets/no_existe_este_fichero.fbx", skel);

    CHECK(lc.clips.empty());
    CHECK(!lc.warnings.empty());
}

// El caso central de la feature: los clips de un segundo fichero se suman a los
// del modelo. Se importa el MISMO fbx dos veces a propósito — ejercita la
// deduplicación de nombres sin necesitar un asset extra en el repo.
static void test_add_animation_source_appends_clips()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t before = m.animationClips.size();

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(m, "assets/modelAnimation.fbx", warnings));

    CHECK(m.animationClips.size() == before * 2);
    CHECK(m.animationSources.size() == 2u);
    CHECK(m.animationSources[1].builtin == false);
    CHECK(m.animationSources[1].path == "assets/modelAnimation.fbx");
    CHECK(m.animationSources[1].clipNames.size() == before);

    // Nombres únicos entre TODOS los clips: el Animator resuelve por nombre
    for (size_t i = 0; i < m.animationClips.size(); i++)
        for (size_t j = i + 1; j < m.animationClips.size(); j++)
            CHECK(m.animationClips[i].name != m.animationClips[j].name);

    // Los clips nuevos se nombran por el fichero, no por el nombre interno
    CHECK(m.animationSources[1].clipNames[0].rfind("modelAnimation", 0) == 0);
}

// Rechazo: un rig que no casa deja el mesh EXACTAMENTE como estaba. Nada de
// clips a medias ni de una fuente registrada que no aportó nada.
static void test_add_animation_source_rejects_foreign_rig()
{
    SkinnedMesh m;
    m.skeleton.names           = { "hueso_que_no_existe" };
    m.skeleton.parentIndex     = { -1 };
    m.skeleton.inverseBindPose = { glm::mat4(1.0f) };
    m.skeleton.boneMap         = { { "hueso_que_no_existe", 0 } };

    std::vector<std::string> warnings;
    CHECK(!addAnimationSource(m, "assets/modelAnimation.fbx", warnings));

    CHECK(m.animationClips.empty());
    CHECK(m.animationSources.empty());
    CHECK(!warnings.empty());
}

// Quitar una fuente se lleva sus clips y solo los suyos, y el packing GPU queda
// coherente con la lista nueva (clipCount y offsets).
static void test_remove_animation_source()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t builtinCount = m.animationClips.size();
    std::vector<std::string> builtinNames;
    for (const auto& c : m.animationClips) builtinNames.push_back(c.name);

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(m, "assets/modelAnimation.fbx", warnings));
    CHECK(removeAnimationSource(m, 1));

    CHECK(m.animationSources.size() == 1u);
    CHECK(m.animationClips.size() == builtinCount);
    for (size_t i = 0; i < builtinNames.size() && i < m.animationClips.size(); i++)
        CHECK(m.animationClips[i].name == builtinNames[i]);   // orden intacto

    // El packing refleja la lista nueva: un boneInfos de más significaría que la
    // GPU seguiría teniendo el bloque del clip borrado.
    const PackedClips packed = packSkinnedClips(m);
    CHECK(packed.boneInfos.size() == m.animationClips.size() * m.skeleton.names.size());
}

// La fuente builtin es el modelo: quitarla dejaría un mesh sin el FBX que lo
// creó. Se rechaza, sin tocar nada.
static void test_remove_builtin_source_is_rejected()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t before = m.animationClips.size();

    CHECK(!removeAnimationSource(m, 0));
    CHECK(m.animationSources.size() == 1u);
    CHECK(m.animationClips.size() == before);

    // Índice fuera de rango: mismo trato, false y sin efectos
    CHECK(!removeAnimationSource(m, 99));
}

// Rename: cambia el clip y el clipNames de su fuente. Rechaza vacío y duplicado
// — los dos dejarían clips inalcanzables por nombre.
static void test_rename_clip()
{
    SkinnedMesh m;
    m.skeleton.names = { "root" };
    AnimationClip a; a.name = "walk"; m.animationClips.push_back(a);
    AnimationClip b; b.name = "run";  m.animationClips.push_back(b);
    AnimationSource src; src.path = "x.fbx"; src.builtin = true;
    src.clipNames = { "walk", "run" };
    m.animationSources.push_back(src);

    CHECK(renameClip(m, "walk", "andar"));
    CHECK(m.animationClips[0].name == "andar");
    CHECK(m.animationSources[0].clipNames[0] == "andar");

    CHECK(!renameClip(m, "andar", ""));       // vacío
    CHECK(!renameClip(m, "andar", "run"));    // ya existe
    CHECK(!renameClip(m, "no_existe", "x"));  // origen inexistente
    CHECK(m.animationClips[0].name == "andar");
}

// forcedNames es lo que hace que un rename sobreviva a guardar/cargar y a un
// undo: los clips se importan con los nombres que ya tenían, no con los del
// fichero.
static void test_add_animation_source_with_forced_names()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t builtinCount = m.animationClips.size();

    std::vector<std::string> forced;
    for (size_t i = 0; i < builtinCount; i++)
        forced.push_back("MiClip" + std::to_string(i));

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(m, "assets/modelAnimation.fbx", warnings, &forced));

    CHECK(m.animationSources.size() == 2u);
    CHECK(m.animationSources[1].clipNames == forced);
    for (size_t i = 0; i < forced.size(); i++)
        CHECK(m.animationClips[builtinCount + i].name == forced[i]);
}

// Renombrar un clip tiene que arrastrar a los estados que lo usan: si no, el
// rename dejaría el grafo apuntando a un nombre que ya no existe y bindClips
// los marcaría como huérfanos.
static void test_rename_clip_references_in_animator()
{
    AnimatorComponent a;
    AnimatorComponent::State s0; s0.name = "A"; s0.clipName = "walk";
    AnimatorComponent::State s1; s1.name = "B"; s1.clipName = "run";
    AnimatorComponent::State s2; s2.name = "C"; s2.clipName = "walk";
    a.addState(s0); a.addState(s1); a.addState(s2);

    CHECK(a.renameClipReferences("walk", "andar") == 2);
    CHECK(a.states()[0].clipName == "andar");
    CHECK(a.states()[1].clipName == "run");
    CHECK(a.states()[2].clipName == "andar");

    // Nombre no usado por ningún estado: 0 y sin efectos
    CHECK(a.renameClipReferences("no_existe", "x") == 0);
}

// Fix de review (finding 1, task-6): un swap de dos nombres builtin
// encadenando renameClip clip a clip colisiona consigo mismo y no aplica
// nada — el segundo rename choca con el nombre que el primero acaba de dejar
// libre, en el hueco equivocado. applyClipNamesPositionally lo resuelve de
// una vez. Se construye a mano el estado "recién cargado del FBX" (igual que
// test_rename_clip) porque el único asset del repo (modelAnimation.fbx)
// trae un solo clip y no puede producir un swap real de dos clips builtin
// vía Scene::fromJson.
static void test_apply_clip_names_positionally_swap()
{
    SkinnedMesh m;
    m.skeleton.names = { "root" };
    AnimationClip a; a.name = "Idle"; m.animationClips.push_back(a);
    AnimationClip b; b.name = "Walk"; m.animationClips.push_back(b);
    AnimationSource src; src.path = "x.fbx"; src.builtin = true;
    src.clipNames = { "Idle", "Walk" };
    m.animationSources.push_back(src);

    // Nombres guardados: exactamente el swap de los actuales.
    std::vector<std::string> saved = { "Walk", "Idle" };
    std::vector<std::string> warnings;
    applyClipNamesPositionally(m, m.animationSources[0], saved, warnings);

    CHECK(warnings.empty());
    CHECK(m.animationClips[0].name == "Walk");
    CHECK(m.animationClips[1].name == "Idle");
    CHECK(m.animationSources[0].clipNames[0] == "Walk");
    CHECK(m.animationSources[0].clipNames[1] == "Idle");

    // Un AnimatorComponent que referencia el clip por su nombre NUEVO tiene
    // que bindear al índice correcto: es la consecuencia real del bug. Con
    // renameClip secuencial los nombres se habrían quedado como estaban
    // (Idle/Walk) y este binding habría resuelto al clip EQUIVOCADO en vez
    // de fallar de forma visible.
    AnimatorComponent anim;
    AnimatorComponent::State st;
    st.name = "S"; st.clipName = "Walk";   // el nombre que AHORA tiene el clip 0
    anim.addState(st);

    std::vector<std::string> bindWarnings;
    anim.bindClips(m, &bindWarnings);
    CHECK(bindWarnings.empty());
    CHECK(anim.states()[0].clipIndex == 0);   // clip 0 es el que ahora se llama "Walk"
}

// Guard del finding 1: nombres guardados duplicados entre sí dejarían dos
// clips homónimos (el Animator resuelve por nombre) — se descarta ESE
// subconjunto y se avisa, sin tocar ningún clip.
static void test_apply_clip_names_positionally_rejects_duplicate_saved_names()
{
    SkinnedMesh m;
    m.skeleton.names = { "root" };
    AnimationClip a; a.name = "Idle"; m.animationClips.push_back(a);
    AnimationClip b; b.name = "Walk"; m.animationClips.push_back(b);
    AnimationSource src; src.path = "x.fbx"; src.builtin = true;
    src.clipNames = { "Idle", "Walk" };
    m.animationSources.push_back(src);

    std::vector<std::string> saved = { "Mismo", "Mismo" };
    std::vector<std::string> warnings;
    applyClipNamesPositionally(m, m.animationSources[0], saved, warnings);

    CHECK(!warnings.empty());
    // Nada se aplicó: los clips siguen con su nombre original
    CHECK(m.animationClips[0].name == "Idle");
    CHECK(m.animationClips[1].name == "Walk");
    CHECK(m.animationSources[0].clipNames[0] == "Idle");
    CHECK(m.animationSources[0].clipNames[1] == "Walk");
}

// Guard del finding 1: un nombre guardado que colisiona con un clip AJENO al
// lote (de otra fuente) no se aplica — dejaría dos clips con el mismo
// nombre. El segundo nombre del lote es un no-op (Walk == Walk) y sí debe
// sobrevivir intacto.
static void test_apply_clip_names_positionally_rejects_external_collision()
{
    SkinnedMesh m;
    m.skeleton.names = { "root" };
    AnimationClip a; a.name = "Idle"; m.animationClips.push_back(a);
    AnimationClip b; b.name = "Walk"; m.animationClips.push_back(b);
    AnimationClip c; c.name = "Jump"; m.animationClips.push_back(c);   // de otra fuente

    AnimationSource builtin; builtin.path = "x.fbx"; builtin.builtin = true;
    builtin.clipNames = { "Idle", "Walk" };
    m.animationSources.push_back(builtin);
    AnimationSource other; other.path = "y.fbx"; other.builtin = false;
    other.clipNames = { "Jump" };
    m.animationSources.push_back(other);

    std::vector<std::string> saved = { "Jump", "Walk" };   // "Jump" ya en uso fuera del lote
    std::vector<std::string> warnings;
    applyClipNamesPositionally(m, m.animationSources[0], saved, warnings);

    CHECK(!warnings.empty());
    CHECK(m.animationClips[0].name == "Idle");             // no se aplicó por la colisión
    CHECK(m.animationSources[0].clipNames[0] == "Idle");
    CHECK(m.animationClips[1].name == "Walk");             // no-op, permanece igual
    CHECK(m.animationSources[0].clipNames[1] == "Walk");
}

// Las fuentes extra y los renames viven en la escena: el SkinnedMesh se
// reconstruye desde los FBX en cada carga, así que sin esto un proyecto
// guardado perdería todas las animaciones importadas.
static void test_animation_sources_survive_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    std::vector<std::string> warnings;
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    const std::string importedName = mesh->animationSources[1].clipNames[0];
    CHECK(renameClip(*mesh, importedName, "SaltoRenombrado"));
    const std::string builtinName = mesh->animationSources[0].clipNames[0];
    go->setMesh(mesh);

    // Un estado que usa el clip importado y renombrado: tras cargar tiene que
    // seguir resolviendo
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State st;
    st.name = "Salto"; st.clipName = "SaltoRenombrado";
    a->addState(st);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;

    SkinnedMesh* lm = found->getSkinnedMesh();
    CHECK(lm != nullptr);
    if (!lm) return;

    CHECK(lm->animationSources.size() == 2u);
    CHECK(lm->animationSources[0].builtin == true);
    CHECK(lm->animationSources[0].clipNames[0] == builtinName);
    CHECK(lm->animationSources[1].builtin == false);
    CHECK(lm->animationSources[1].path == "assets/modelAnimation.fbx");
    CHECK(lm->animationSources[1].clipNames[0] == "SaltoRenombrado");

    // El clip renombrado existe con ese nombre en la lista plana
    bool encontrado = false;
    for (const auto& c : lm->animationClips)
        if (c.name == "SaltoRenombrado") encontrado = true;
    CHECK(encontrado);

    // Y el estado lo resuelve sin avisos
    std::vector<std::string> bindWarnings;
    found->getAnimator()->bindClips(*lm, &bindWarnings);
    CHECK(bindWarnings.empty());
    CHECK(found->getAnimator()->states()[0].clipIndex >= 0);
}

// Una fuente cuyo fichero ya no está no puede tumbar la carga de la escena: se
// avisa y se sigue, dejando los estados que la usaran huérfanos (bindClips ya
// los marca).
static void test_missing_animation_source_does_not_break_load(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh);

    nlohmann::json j = scene.toJson();
    // Se inyecta a mano una fuente que apunta a un fichero inexistente: simula
    // un proyecto cuyo .fbx se borró o se movió después de guardar.
    j["root"]["children"][0]["mesh"]["animationSources"].push_back(
        { {"path", "assets/no_existe.fbx"}, {"builtin", false}, {"clips", {"Fantasma"}} });

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    SkinnedMesh* lm = found->getSkinnedMesh();
    CHECK(lm != nullptr);
    if (!lm) return;
    // La fuente fantasma no se registra; el modelo sigue entero
    CHECK(lm->animationSources.size() == 1u);
    CHECK(!lm->animationClips.empty());
}

// Fix de review (finding 2, task-6): el aviso de una fuente que no carga
// tenía que llegar a Scene::lastWarnings() —lo que lee el Log Console del
// editor— y no solo a stdout vía std::printf, invisible en un build sin
// consola. Mismo escenario que el test anterior, pero comprobando el
// warning en vez de solo la resiliencia de la carga.
static void test_missing_animation_source_warns_through_scene(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh);

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["mesh"]["animationSources"].push_back(
        { {"path", "assets/no_existe.fbx"}, {"builtin", false}, {"clips", {"Fantasma"}} });

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;

    // El aviso del loader (fichero inexistente) tiene que llegar a
    // m_warnings, visible vía lastWarnings() — el mismo camino que usa el
    // editor, no stdout.
    bool sawMissingSourceWarning = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("no_existe.fbx") != std::string::npos) sawMissingSourceWarning = true;
    CHECK(sawMissingSourceWarning);
}

// Escenas guardadas antes de esta feature no tienen "animationSources": cargan
// con la fuente builtin sintetizada desde sourcePath, sin avisos.
static void test_scene_without_animation_sources_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    go->setMesh(std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx")));

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["mesh"].erase("animationSources");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    SkinnedMesh* lm = found->getSkinnedMesh();
    CHECK(lm != nullptr);
    if (!lm) return;
    CHECK(lm->animationSources.size() == 1u);
    CHECK(lm->animationSources[0].builtin == true);
}

// Las escenas guardadas antes de la auto-detección tienen "skinned": false para
// TODOS sus meshes — el editor nunca creaba skinned. Si la carga siguiera
// leyendo ese flag, esos proyectos nunca podrían tener Animator sin reimportar
// la malla a mano. Manda el fichero, no el flag.
static void test_scene_load_ignores_stale_skinned_false(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh);

    nlohmann::json j = scene.toJson();
    // Simula el fichero viejo: flag a false sobre un FBX que sí tiene huesos.
    j["root"]["children"][0]["mesh"]["skinned"] = false;

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;

    SkinnedMesh* lm = found->getSkinnedMesh();
    CHECK(lm != nullptr);
    if (!lm) return;
    CHECK(!lm->skeleton.names.empty());
    CHECK(found->isSkinned());
}

// El caso simétrico: escena con "skinned": true cuyo FBX se reexportó luego sin
// huesos. Carga estática y sus fuentes de animación se descartan — pero con un
// aviso en Scene::lastWarnings() (lo que lee el Log Console), no en silencio.
// No hay ningún FBX del repo que sirva de "sin rig" (model.fbx y
// modelTexture.fbx son personajes Mixamo rigged pese al nombre, ver el
// comentario de writeUnriggedObjFixture más arriba), así que se reusa ese
// mismo helper con un nombre de fichero propio para no colisionar con los
// tests de Task 1.
static void test_scene_load_warns_when_rig_disappeared(PhysicsManager& pm, AudioManager& am)
{
    const std::string unriggedPath = writeUnriggedObjFixture("dt_test_scene_rig_disappeared.obj");

    Scene scene("Test");
    GameObject* go = scene.addGameObject("Prop");
    const uint64_t id = go->id;
    go->setMesh(std::make_shared<Mesh>(ModelLoader::load(unriggedPath)));

    nlohmann::json j = scene.toJson();
    // Simula el reexport: la escena creía que era skinned y guardó fuentes.
    j["root"]["children"][0]["mesh"]["skinned"] = true;
    j["root"]["children"][0]["mesh"]["animationSources"] = nlohmann::json::array({
        { {"path", "assets/modelAnimation.fbx"}, {"builtin", false}, {"clips", {"Salto"}} }
    });

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    std::filesystem::remove(unriggedPath);
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;

    // Carga, pero estático: sin huesos no hay nada que animar.
    CHECK(found->hasMesh());
    CHECK(found->getSkinnedMesh() == nullptr);

    bool avisado = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("dt_test_scene_rig_disappeared.obj") != std::string::npos) avisado = true;
    CHECK(avisado);
}

// Fix de review: un forcedName que ya está en uso (por un clip existente, o
// por un forcedName anterior de esta misma importación) NO puede colarse tal
// cual — dejaría dos clips homónimos y el Animator resuelve por nombre, así
// que uno de los dos quedaría inalcanzable. Debe caer en uniqueClipName y
// avisar. Se fuerza la colisión reutilizando el nombre del clip builtin como
// forcedName de la fuente añadida.
static void test_add_animation_source_with_forced_names_collision()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t builtinCount = m.animationClips.size();
    CHECK(builtinCount >= 1u);
    if (builtinCount < 1u) return;

    const std::string collidingName = m.animationClips[0].name;

    std::vector<std::string> forced;
    forced.push_back(collidingName);            // colisiona con el clip builtin
    for (size_t i = 1; i < builtinCount; i++)
        forced.push_back("Extra" + std::to_string(i));

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(m, "assets/modelAnimation.fbx", warnings, &forced));

    // (a) Ningún nombre repetido entre TODOS los clips del mesh
    for (size_t i = 0; i < m.animationClips.size(); i++)
        for (size_t j = i + 1; j < m.animationClips.size(); j++)
            CHECK(m.animationClips[i].name != m.animationClips[j].name);

    // (b) clipNames de la fuente refleja exactamente lo que quedó en
    // animationClips, no los forcedNames pedidos a ciegas
    CHECK(m.animationSources.size() == 2u);
    CHECK(m.animationSources[1].clipNames.size() == builtinCount);
    for (size_t i = 0; i < m.animationSources[1].clipNames.size(); i++)
        CHECK(m.animationClips[builtinCount + i].name == m.animationSources[1].clipNames[i]);

    // El forcedName colisionado no se coló pelado: lleva el sufijo " (N)"
    // que pone uniqueClipName en vez del nombre exacto pedido.
    CHECK(m.animationSources[1].clipNames[0] == collidingName + " (1)");

    // (c) Se avisó del renombrado forzado
    CHECK(!warnings.empty());
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

// Añadir una fuente pasa por el stack: sin esto, un Ctrl+Z tras importar por
// error un FBX de 60 clips no tendría vuelta atrás.
static void test_animation_source_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    const size_t before = mesh->animationClips.size();
    go->setMesh(mesh);

    AnimationSourceCommand cmd(scene, /*renderer=*/nullptr, "Añadir animaciones",
                                id, /*add=*/true, "assets/modelAnimation.fbx",
                                /*clipNames=*/{});

    cmd.execute();
    CHECK(mesh->animationSources.size() == 2u);
    CHECK(mesh->animationClips.size() == before * 2);

    cmd.undo();
    CHECK(mesh->animationSources.size() == 1u);
    CHECK(mesh->animationClips.size() == before);

    // Redo: vuelven los mismos clips con los mismos nombres que la primera vez
    cmd.execute();
    CHECK(mesh->animationSources.size() == 2u);
    CHECK(mesh->animationClips.size() == before * 2);
}

// El Remove es el mismo comando con add=false, y su undo tiene que devolver los
// clips con los nombres EXACTOS que tenían (por eso el comando guarda
// clipNames): si volvieran con el nombre del fichero, los estados del grafo
// quedarían huérfanos tras un Ctrl+Z.
static void test_animation_source_command_remove_restores_names()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh);

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    const std::string imported = mesh->animationSources[1].clipNames[0];
    CHECK(renameClip(*mesh, imported, "MiSalto"));
    const std::vector<std::string> names = mesh->animationSources[1].clipNames;

    AnimationSourceCommand cmd(scene, nullptr, "Quitar animaciones",
                                id, /*add=*/false, "assets/modelAnimation.fbx", names);

    cmd.execute();
    CHECK(mesh->animationSources.size() == 1u);

    cmd.undo();
    CHECK(mesh->animationSources.size() == 2u);
    CHECK(mesh->animationSources[1].clipNames == names);
    bool encontrado = false;
    for (const auto& c : mesh->animationClips)
        if (c.name == "MiSalto") encontrado = true;
    CHECK(encontrado);
}

// Finding 1 de la revisión final: bindClips es lo único que resuelve
// clipName -> clipIndex, y su único llamador en el motor es
// Scene::nodeFromJson. Sin que AnimationSourceCommand re-resuelva tras
// mutar animationClips, un estado sobrevive con el índice VIEJO, que tras
// quitar una fuente del medio puede pasar a apuntar a un clip DISTINTO del
// que su nombre dice — sin aviso, y el Renderer lo reproduce igualmente
// (currentClipIndex no distingue "resuelto" de "por casualidad en rango").
// Este test arma tres estados (uno por clip: builtin + dos importados),
// quita la fuente importada del MEDIO, y comprueba la invariante real: cada
// estado que sigue resuelto apunta a un clip cuyo nombre coincide con el
// suyo, o está en -1 (huérfano). Ningún índice "se cuela" apuntando a otro
// clip. Se repite tras el undo (que reinserta al final, ver applyAdd) para
// cubrir también el camino de Add.
static void test_animation_source_command_remove_rebinds_clip_indices()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh);

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    CHECK(mesh->animationSources.size() == 3u);   // builtin + 2 importadas

    // Un estado por clip existente, mismo nombre que el clip para poder
    // comprobar la invariante por igualdad de nombre.
    auto anim = std::make_shared<AnimatorComponent>();
    for (const auto& c : mesh->animationClips)
    {
        AnimatorComponent::State st;
        st.name = c.name;
        st.clipName = c.name;
        anim->addState(st);
    }
    go->setAnimator(anim);
    anim->bindClips(*mesh);   // resuelve como lo haría Scene::nodeFromJson

    auto checkInvariant = [&]()
    {
        for (const auto& st : anim->states())
        {
            if (st.clipIndex < 0) continue;   // huérfano: aceptable
            CHECK((size_t)st.clipIndex < mesh->animationClips.size());
            if ((size_t)st.clipIndex < mesh->animationClips.size())
                CHECK(mesh->animationClips[(size_t)st.clipIndex].name == st.clipName);
        }
    };
    checkInvariant();   // premisa: cierto antes de tocar nada

    // Quita la fuente importada del medio (índice 1): desplaza los clips de
    // la fuente 2 un hueco hacia atrás en la lista plana. pathOccurrence=1
    // porque hay una fuente con el mismo path por delante (la fila 2, ver
    // el comentario de applyRemove en Command.cpp sobre contar desde el final).
    const AnimationSource middle = mesh->animationSources[1];
    const std::string middleClipName = middle.clipNames[0];
    AnimationSourceCommand cmd(scene, /*renderer=*/nullptr, "Quitar animaciones",
                                id, /*add=*/false, middle.path, middle.clipNames,
                                /*pathOccurrence=*/1);
    cmd.execute();

    CHECK(mesh->animationSources.size() == 2u);
    checkInvariant();   // invariante de Finding 1 tras el Remove

    // Y en concreto: el estado que nombraba el clip quitado quedó huérfano,
    // no reapuntado a otro clip por casualidad de índice.
    for (const auto& st : anim->states())
        if (st.clipName == middleClipName) CHECK(st.clipIndex == -1);

    // Mirror del camino de Add: undo() de un Remove es un applyAdd (reinserta
    // al final), y también tiene que re-resolver.
    cmd.undo();
    CHECK(mesh->animationSources.size() == 3u);
    checkInvariant();
    for (const auto& st : anim->states())
        if (st.clipName == middleClipName) CHECK(st.clipIndex != -1);   // ya no huérfano
}

// Finding 2 de la revisión final: importar el mismo FBX dos veces es legal
// (AnimatorPanel las distingue con pathOccurrence en el panel), así que dos
// filas pueden compartir path. El comando original localizaba por path solo,
// escaneando hacia atrás — quitar la PRIMERA de las dos filas borraba
// siempre la ÚLTIMA. Este test pincha exactamente ese caso: pide quitar la
// fila 1 (pathOccurrence=1, la fila 2 queda por delante) y comprueba que
// desaparece el clip de la fila 1, no el de la fila 2.
static void test_animation_source_command_remove_targets_clicked_occurrence()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh);

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    CHECK(mesh->animationSources.size() == 3u);
    CHECK(mesh->animationSources[1].path == mesh->animationSources[2].path);   // mismo path, filas distintas

    const std::string firstImportedName  = mesh->animationSources[1].clipNames[0];
    const std::string secondImportedName = mesh->animationSources[2].clipNames[0];
    CHECK(firstImportedName != secondImportedName);   // uniqueClipName ya lo garantiza

    AnimationSourceCommand cmd(scene, /*renderer=*/nullptr, "Quitar animaciones",
                                id, /*add=*/false,
                                mesh->animationSources[1].path,
                                mesh->animationSources[1].clipNames,
                                /*pathOccurrence=*/1);
    cmd.execute();

    CHECK(mesh->animationSources.size() == 2u);
    bool firstGone = true;
    bool secondSurvives = false;
    for (const auto& c : mesh->animationClips)
    {
        if (c.name == firstImportedName)  firstGone = false;
        if (c.name == secondImportedName) secondSurvives = true;
    }
    CHECK(firstGone);
    CHECK(secondSurvives);
}

// Finding 3 de la revisión final: si el escaneo no encuentra nada que quitar
// (occurrence que ya no existe: redo tras recarga, o mesh reemplazado entre
// execute() y undo()), applyRemove no debe mutar el mesh. No hay renderer
// real en un test headless pa comprobar que se ahorra el rebuild, pero SÍ se
// puede comprobar que el mesh queda intacto — antes el early-return no
// existía y el código de más abajo (el rebuild) corría igual sobre una malla
// sin cambios.
static void test_animation_source_command_remove_noop_when_occurrence_missing()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh);

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    const size_t sourcesBefore = mesh->animationSources.size();
    const size_t clipsBefore   = mesh->animationClips.size();

    // pathOccurrence=5: no hay ni de lejos cinco fuentes con ese path.
    AnimationSourceCommand cmd(scene, /*renderer=*/nullptr, "Quitar animaciones",
                                id, /*add=*/false,
                                "assets/modelAnimation.fbx",
                                std::vector<std::string>{"loQueSea"},
                                /*pathOccurrence=*/5);
    cmd.execute();

    CHECK(mesh->animationSources.size() == sourcesBefore);
    CHECK(mesh->animationClips.size() == clipsBefore);
}

// Finding de la revisión final (bloqueante): m_pathOccurrence localiza "la
// fuente N-ésima con este path, contando desde el final", y eso es estable
// para UN comando aislado, pero no para comandos INTERCALADOS: applyAdd
// siempre reinserta al final, así que cada undo/redo de un Add ajeno
// desplaza el "final" que el pathOccurrence de otro comando pendiente da
// por supuesto. Repro exacto de la revisión:
//   1. Mesh ya tiene [builtin B, S1(mismo FBX)].
//   2. Se reimporta el mismo FBX -> cmdA (Add), sources = [B, S1, S2].
//   3. Se quita la fila 1 (S1) -> cmdB (Remove, pathOccurrence=1 porque S2
//      queda por delante). sources = [B, S2].
//   4. Ctrl+Z de cmdB -> applyAdd reinserta S1 AL FINAL: [B, S2, S1'].
//   5. Ctrl+Z de cmdA -> applyRemove con pathOccurrence=0 (el que se
//      recalculó en su propio execute) encuentra la PRIMERA fuente no-builtin
//      escaneando desde el final: eso es ahora S1', no S2 -- borra lo que el
//      usuario acababa de recuperar y deja vivo lo que cmdA había metido.
// El fix resuelve por identidad (m_clipNames, únicos en la malla por
// uniqueClipName) en vez de por posición, así que este test comprueba que
// tras deshacer cmdB y luego cmdA, la fuente que sobrevive es S1 (la que
// cmdA NO tocó), no S2.
static void test_animation_source_command_undo_add_after_interleaved_remove_undo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh);

    // Estado de partida del repro: [B, S1]. S1 se importa fuera del undo
    // stack (representa "lo que ya había antes de que arrancara este
    // escenario").
    std::vector<std::string> warnings;
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    CHECK(mesh->animationSources.size() == 2u);
    const std::string s1Path = mesh->animationSources[1].path;
    const std::vector<std::string> s1Names = mesh->animationSources[1].clipNames;

    // cmdA: reimportar el mismo FBX, tal como AnimatorPanel::onAnimationFbxSelected
    // construye el comando (clipNames vacío, pathOccurrence por defecto 0).
    auto cmdA = std::make_unique<AnimationSourceCommand>(
        scene, /*renderer=*/nullptr, "Añadir animaciones", id,
        /*add=*/true, s1Path, std::vector<std::string>{});
    cmdA->execute();
    CHECK(mesh->animationSources.size() == 3u);   // [B, S1, S2]
    const std::vector<std::string> s2Names = mesh->animationSources[2].clipNames;
    CHECK(s1Names != s2Names);   // uniqueClipName las distingue

    // cmdB: clic en la X de la fila 1 (S1), con pathOccurrence calculado
    // exactamente como AnimatorPanel::drawAnimationSources (cuenta las
    // fuentes no-builtin con el mismo path por DELANTE de la fila pulsada:
    // S2 queda por delante, así que vale 1).
    auto cmdB = std::make_unique<AnimationSourceCommand>(
        scene, /*renderer=*/nullptr, "Quitar animaciones", id,
        /*add=*/false, s1Path, s1Names, /*pathOccurrence=*/1);
    cmdB->execute();
    CHECK(mesh->animationSources.size() == 2u);   // [B, S2]

    // LIFO del UndoManager real: se deshace primero lo último apilado
    // (cmdB), luego cmdA.
    cmdB->undo();   // applyAdd: reinserta S1' AL FINAL -> [B, S2, S1']
    CHECK(mesh->animationSources.size() == 3u);

    cmdA->undo();   // applyRemove: debe quitar S2 (lo que cmdA insertó), no S1'

    CHECK(mesh->animationSources.size() == 2u);
    CHECK(!mesh->animationSources[1].builtin);
    // La aserción clave: la fuente que sobrevive es S1, no S2. Con el bug
    // (localización por posición) esto falla porque applyRemove borra S1'
    // (lo que el usuario acababa de recuperar) y deja vivo S2.
    CHECK(mesh->animationSources[1].clipNames == s1Names);
}

// El rename es undoable y arrastra a los estados del grafo en ambos sentidos.
static void test_clip_rename_command()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto mesh = std::make_shared<SkinnedMesh>();
    AnimationClip c; c.name = "walk"; mesh->animationClips.push_back(c);
    AnimationSource src; src.path = "x.fbx"; src.builtin = true; src.clipNames = { "walk" };
    mesh->animationSources.push_back(src);
    go->setMesh(mesh);

    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State st; st.name = "Andar"; st.clipName = "walk";
    a->addState(st);
    go->setAnimator(a);

    ClipRenameCommand cmd(scene, "Renombrar clip", id, "walk", "andar");

    cmd.execute();
    CHECK(mesh->animationClips[0].name == "andar");
    CHECK(a->states()[0].clipName == "andar");

    cmd.undo();
    CHECK(mesh->animationClips[0].name == "walk");
    CHECK(a->states()[0].clipName == "walk");
}

// Igual que el resto de comandos: el objeto puede haber desaparecido entre
// execute y undo. Se resuelve por id cada vez, nunca por puntero crudo.
static void test_animation_source_command_survives_missing_target()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    AnimationSourceCommand cmd(scene, nullptr, "Añadir animaciones",
                                id, true, "assets/modelAnimation.fbx", {});
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

// Equals/NotEquals sobre un Float: se ofrecen en la UI igual que en Int, y el
// evaluador los aplica con == pelado, sin epsilon. Se usa 2.5f, exactamente
// representable en binario, pa que el test sea sobre el comparador y no sobre
// precisión de punto flotante.
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
    test_loader_registers_builtin_source();
    test_unique_clip_name();
    test_load_animation_clips_matches_full_load();
    test_load_animation_clips_against_foreign_skeleton();
    test_load_animation_clips_partial_skeleton();
    test_load_animation_clips_missing_file();
    test_add_animation_source_appends_clips();
    test_add_animation_source_rejects_foreign_rig();
    test_remove_animation_source();
    test_remove_builtin_source_is_rejected();
    test_rename_clip();
    test_add_animation_source_with_forced_names();
    test_rename_clip_references_in_animator();
    test_add_animation_source_with_forced_names_collision();
    test_apply_clip_names_positionally_swap();
    test_apply_clip_names_positionally_rejects_duplicate_saved_names();
    test_apply_clip_names_positionally_rejects_external_collision();
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

    test_has_bones_detects_rigged_fbx();
    test_has_bones_rejects_unrigged_model();
    test_has_bones_survives_missing_file();
    test_load_auto_returns_skinned_for_rigged();
    test_load_auto_returns_static_for_unrigged();

    test_bind_clips_resolves_by_name();
    test_gameobject_animator_slot();

    test_graph_survives_scene_round_trip(pm, am);
    test_scene_without_animator_block_loads(pm, am);
    test_numeric_graph_survives_scene_round_trip(pm, am);
    test_condition_without_compare_fields_loads(pm, am);
    test_animation_sources_survive_scene_round_trip(pm, am);
    test_missing_animation_source_does_not_break_load(pm, am);
    test_missing_animation_source_warns_through_scene(pm, am);
    test_scene_without_animation_sources_loads(pm, am);
    test_scene_load_ignores_stale_skinned_false(pm, am);
    test_scene_load_warns_when_rig_disappeared(pm, am);

    test_animator_command_add_undo_redo();
    test_animator_command_remove();
    test_animator_command_survives_missing_target();

    test_animation_source_command_add_undo_redo();
    test_animation_source_command_remove_restores_names();
    test_animation_source_command_remove_rebinds_clip_indices();
    test_animation_source_command_remove_targets_clicked_occurrence();
    test_animation_source_command_remove_noop_when_occurrence_missing();
    test_animation_source_command_undo_add_after_interleaved_remove_undo();
    test_clip_rename_command();
    test_animation_source_command_survives_missing_target();

    am.shutdown();
    pm.shutdown();
    if (g_failures) { std::printf("dt_animator_tests: %d FAILURES\n", g_failures); std::fflush(stdout); return 1; }
    std::printf("dt_animator_tests: OK\n");
    std::fflush(stdout);
    return 0;
}
