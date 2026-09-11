// Test headless de los overrides de textura del Mesh (sin GPU). Plain main +
// asserts, sin framework — mismo patrón que content_browser_tests.cpp.
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/MaterialTextureSource.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Editor/DeferredSlider.h"
#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Estático con una textura de albedo venida del FBX.
static std::unique_ptr<GameObject> makeStaticFixture()
{
    auto go = std::make_unique<GameObject>("Estatico");
    auto mesh = std::make_shared<Mesh>();
    mesh->name = "cubo";
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));
    return go;
}

// Skinned con TRES materiales por submalla, cada uno con su albedo del FBX.
static std::unique_ptr<GameObject> makeSkinnedFixture()
{
    auto go = std::make_unique<GameObject>("Personaje");
    auto mesh = std::make_shared<SkinnedMesh>();
    mesh->name = "heroe";
    mesh->materials.resize(3);
    mesh->materials[0].texturePath = "assets/cuerpo.png";
    mesh->materials[1].texturePath = "assets/ropa.png";
    mesh->materials[2].texturePath = "assets/pelo.png";
    go->setMesh(std::move(mesh));
    return go;
}

// Un estático expone UN material: el heredado. El vector materials de un
// SkinnedMesh vacío no cuenta.
static void test_materials_of_static_mesh()
{
    auto go = makeStaticFixture();
    std::vector<Material*> mats = materialsOfMesh(*go);
    CHECK(mats.size() == 1);
    if (mats.size() == 1)
        CHECK(mats[0]->texturePath == "assets/fbx_albedo.png");
}

// Un skinned con submallas expone SUS materiales, no el heredado.
static void test_materials_of_skinned_mesh()
{
    auto go = makeSkinnedFixture();
    std::vector<Material*> mats = materialsOfMesh(*go);
    CHECK(mats.size() == 3);
    if (mats.size() == 3)
        CHECK(mats[2]->texturePath == "assets/pelo.png");
}

// Asignar pisa el material y guarda como baseline lo que traía el FBX.
static void test_override_writes_material_and_captures_baseline()
{
    auto go = makeStaticFixture();
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);

    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == "assets/mia.png");
    CHECK(go->materialOverrides[0].baseAlbedo == "assets/fbx_albedo.png");
}

// Cambiar de textura NO mueve el baseline: sigue siendo el del FBX, no el
// override intermedio. Sin esto, un Clear tras dos cambios devolvería la
// primera textura que puso el usuario en vez de la del modelo.
static void test_second_override_keeps_original_baseline()
{
    auto go = makeStaticFixture();
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].index  = 0;
    go->materialOverrides[0].albedo = "assets/primera.png";
    applyMaterialOverrides(*go);

    go->materialOverrides[0].albedo = "assets/segunda.png";
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == "assets/segunda.png");
    CHECK(go->materialOverrides[0].baseAlbedo == "assets/fbx_albedo.png");
}

// Clear = vaciar el override. El material vuelve al baseline capturado.
static void test_clear_restores_baseline()
{
    auto go = makeStaticFixture();
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].index  = 0;
    go->materialOverrides[0].albedo = "assets/mia.png";
    applyMaterialOverrides(*go);

    go->materialOverrides[0].albedo.clear();
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == "assets/fbx_albedo.png");
}

// El override de una submalla no toca a las vecinas.
static void test_skinned_override_touches_only_its_index()
{
    auto go = makeSkinnedFixture();
    MaterialOverride ov;
    ov.index  = 2;
    ov.albedo = "assets/pelo_rubio.png";
    go->materialOverrides.push_back(ov);

    applyMaterialOverrides(*go);

    SkinnedMesh* sm = go->getSkinnedMesh();
    CHECK(sm->materials[2].texturePath == "assets/pelo_rubio.png");
    CHECK(sm->materials[0].texturePath == "assets/cuerpo.png");
    CHECK(sm->materials[1].texturePath == "assets/ropa.png");
}

// Índice que ya no existe (FBX reexportado con menos submallas): se ignora sin
// tocar nada y sin crash.
static void test_out_of_range_index_is_ignored()
{
    auto go = makeSkinnedFixture();
    MaterialOverride ov;
    ov.index  = 7;
    ov.albedo = "assets/fantasma.png";
    go->materialOverrides.push_back(ov);

    applyMaterialOverrides(*go);

    SkinnedMesh* sm = go->getSkinnedMesh();
    CHECK(sm->materials[0].texturePath == "assets/cuerpo.png");
    CHECK(sm->materials[1].texturePath == "assets/ropa.png");
    CHECK(sm->materials[2].texturePath == "assets/pelo.png");
}

// Los tres slots son independientes: pisar el albedo no toca normal ni ORM.
static void test_slots_are_independent()
{
    auto go = makeStaticFixture();
    go->getMesh()->material.normalMapPath          = "assets/fbx_normal.png";
    go->getMesh()->material.metallicRoughnessPath  = "assets/fbx_orm.png";

    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.normalMapPath == "assets/fbx_normal.png");
    CHECK(go->getMesh()->material.metallicRoughnessPath == "assets/fbx_orm.png");
}

// El override de normal y el de ORM escriben cada uno su propio campo, no el
// de al lado. Sin este test, un cableado con copy-paste equivocado en
// applyMaterialOverrides (p.ej. pasar mat.texturePath como destino del
// normal) habría pasado los tests de arriba: ninguno de ellos toca ov.normal
// ni ov.orm.
static void test_normal_and_orm_overrides_write_their_own_field()
{
    auto go = makeStaticFixture();
    go->getMesh()->material.normalMapPath         = "assets/fbx_normal.png";
    go->getMesh()->material.metallicRoughnessPath = "assets/fbx_orm.png";

    MaterialOverride ov;
    ov.index  = 0;
    ov.normal = "assets/mi_normal.png";
    ov.orm    = "assets/mi_orm.png";
    go->materialOverrides.push_back(ov);

    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.normalMapPath == "assets/mi_normal.png");
    CHECK(go->getMesh()->material.metallicRoughnessPath == "assets/mi_orm.png");
    // El albedo, sin override en este slot, no se ha tocado.
    CHECK(go->getMesh()->material.texturePath == "assets/fbx_albedo.png");
    CHECK(go->materialOverrides[0].baseNormal == "assets/fbx_normal.png");
    CHECK(go->materialOverrides[0].baseOrm    == "assets/fbx_orm.png");
}

// Caso que justifica los flags baseAlbedoTaken/baseNormalTaken/baseOrmTaken:
// un mesh procedural (sin textura del FBX) tiene texturePath vacío DESDE EL
// PRINCIPIO, y ese vacío legítimo no se puede distinguir de "aún no se ha
// tomado el baseline" mirando solo si base* está vacío. Si applyMaterialOverrides
// usara esa heurística en vez de los flags explícitos, Clear no restauraría
// el vacío original: dejaría puesta la textura que puso el usuario.
static void test_clear_restores_empty_baseline_on_procedural_mesh()
{
    auto go = std::make_unique<GameObject>("Procedural");
    auto mesh = std::make_shared<Mesh>();
    mesh->name = "esfera";
    // texturePath se deja vacío a propósito: no viene de ningún FBX.
    go->setMesh(std::move(mesh));

    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == "assets/mia.png");

    go->materialOverrides[0].albedo.clear();
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath.empty());
}

// Task 7: el override se aplica sobre la malla que LLEGA, no después: si se
// aplicara tras registrar en el renderer (AsyncAssetLoader::applyLoadedMesh),
// la GPU subiría la textura del FBX y la del usuario no se vería hasta el
// siguiente rebuild. Este test es el mecanismo puro (sin EditorRenderer, que
// son 73 virtuales puras); que applyLoadedMesh lo llame en el orden correcto
// se verifica en GUI, igual que test_remove_notifies_listener en
// camera_tests.cpp.
static void test_overrides_applied_to_incoming_mesh()
{
    auto go = makeStaticFixture();
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);

    // La malla que "llega" trae lo del FBX.
    auto llegada = std::make_shared<Mesh>();
    llegada->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(llegada);

    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath == "assets/mia.png");
}

// Un GameObject sin mesh no revienta.
static void test_no_mesh_is_noop()
{
    GameObject go("Vacio");
    go.materialOverrides.push_back(MaterialOverride{});
    applyMaterialOverrides(go);
    CHECK(materialsOfMesh(go).empty());
}

// La ruta explícita GANA a los bytes embebidos. Es lo contrario de lo que hacía
// el motor antes de esta feature, y es lo que hace posible el Clear: si ganara
// la embebida, asignar una textura a mano exigiría destruir los bytes del FBX y
// no habría a que volver.
static void test_path_wins_over_embedded()
{
    const std::vector<uint8_t> bytes{1, 2, 3};
    CHECK(chooseTextureSource("assets/x.png", bytes) == TextureSource::Path);
}

// Sin ruta, la embebida.
static void test_embedded_when_no_path()
{
    const std::vector<uint8_t> bytes{1, 2, 3};
    CHECK(chooseTextureSource("", bytes) == TextureSource::Embedded);
}

// Sin nada, nada: el caller pone su relleno (blanca compartida en Vulkan,
// neutro global en D3D12).
static void test_none_when_empty()
{
    CHECK(chooseTextureSource("", {}) == TextureSource::None);
}

// Solo ruta.
static void test_path_when_no_embedded()
{
    CHECK(chooseTextureSource("assets/x.png", {}) == TextureSource::Path);
}

// Round-trip: los overrides de TODOS los índices sobreviven a guardar y cargar.
static void test_overrides_survive_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto mesh = std::make_shared<SkinnedMesh>();
    mesh->sourcePath = "assets/hero.fbx";
    mesh->materials.resize(3);
    go->setMesh(std::move(mesh));

    MaterialOverride a; a.index = 0; a.albedo = "assets/cuerpo.png";
    MaterialOverride b; b.index = 2; b.albedo = "assets/pelo.png";
                               b.normal = "assets/pelo_n.png";
    go->materialOverrides = {a, b};

    const nlohmann::json j = scene.toJson();

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Personaje") leido = n; });
    CHECK(leido != nullptr);
    if (!leido) return;
    CHECK(leido->materialOverrides.size() == 2);
    if (leido->materialOverrides.size() != 2) return;
    CHECK(leido->materialOverrides[0].index  == 0);
    CHECK(leido->materialOverrides[0].albedo == "assets/cuerpo.png");
    CHECK(leido->materialOverrides[1].index  == 2);
    CHECK(leido->materialOverrides[1].normal == "assets/pelo_n.png");
    // El baseline NO viaja: se recaptura al aplicar sobre el material recién
    // derivado del FBX.
    CHECK(leido->materialOverrides[0].baseAlbedo.empty());
}

// Un objeto sin overrides no escribe la clave: las escenas viejas y las nuevas
// sin texturas tocadas son byte a byte iguales.
static void test_no_overrides_writes_no_key()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));

    const nlohmann::json j = scene.toJson();
    // El nodo raíz cuelga de "root"; localizar el hijo por nombre en vez de
    // asumir el índice.
    CHECK(!j.dump().empty());
    CHECK(j.dump().find("\"materials\"") == std::string::npos);
}

// Escena SIN la clave materials: carga exactamente igual que hoy, sin overrides
// y sin avisos.
static void test_scene_without_materials_key_loads_clean(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    nlohmann::json j = scene.toJson();

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (leido) CHECK(leido->materialOverrides.empty());
}

// Con raíz de proyecto fijada, una ruta bajo ella se guarda RELATIVA con "/".
static void test_path_under_root_is_stored_relative(PhysicsManager& pm, AudioManager& am)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "dt_mat_root";

    Scene scene("Test");
    scene.setAssetRoot(root.string());
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = (root / "assets" / "x.png").string();
    go->materialOverrides.push_back(ov);

    const std::string texto = scene.toJson().dump();
    CHECK(texto.find("assets/x.png") != std::string::npos);
    CHECK(texto.find(root.string()) == std::string::npos);

    // Y al leerla con la misma raíz vuelve absoluta.
    Scene cargada("Vacia");
    cargada.setAssetRoot(root.string());
    CHECK(cargada.fromJson(scene.toJson(), pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (leido && leido->materialOverrides.size() == 1)
        CHECK(fs::path(leido->materialOverrides[0].albedo) == (root / "assets" / "x.png"));
}

// Una ruta FUERA de la raíz se guarda absoluta tal cual: relativizarla daría
// una ristra de ".." que no sobrevive a mover el proyecto.
static void test_path_outside_root_stays_absolute()
{
    namespace fs = std::filesystem;
    const fs::path root  = fs::temp_directory_path() / "dt_mat_root";
    const fs::path fuera = fs::temp_directory_path() / "dt_otro" / "y.png";

    Scene scene("Test");
    scene.setAssetRoot(root.string());
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = fuera.string();
    go->materialOverrides.push_back(ov);

    const std::string texto = scene.toJson().dump();
    CHECK(texto.find("y.png") != std::string::npos);
    CHECK(texto.find("..") == std::string::npos);
}

// Sin raíz fijada (tests, runtime headless), la ruta va y vuelve IDÉNTICA.
static void test_without_root_path_is_verbatim(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/tal/cual.png";
    go->materialOverrides.push_back(ov);

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(scene.toJson(), pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    if (leido && leido->materialOverrides.size() == 1)
        CHECK(leido->materialOverrides[0].albedo == "assets/tal/cual.png");
}

// Un bloque "materials" corrupto no tumba la carga: avisa y sigue.
static void test_corrupt_materials_block_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    // Mesh PROCEDURAL (sourcePath vacío), no "assets/cubo.fbx": ese fichero no
    // existe en assets/, así que ModelLoader::load lanzaría, el catch de
    // nodeFromJson empujaría SU PROPIO aviso de "no se pudo cargar la malla" y
    // lastWarnings() ya saldría no-vacío ANTES de mirar el bloque materials —
    // el CHECK de abajo pasaría aunque se borrara el aviso que dice comprobar.
    // Con un mesh procedural (vértices/índices vacíos, se reconstruye sin
    // tocar disco) el único aviso posible de esta carga es el del bloque
    // materials corrupto.
    auto mesh = std::make_shared<Mesh>();
    go->setMesh(std::move(mesh));
    nlohmann::json j = scene.toJson();

    // Inyectar basura donde iría el bloque: un objeto en vez de un array.
    // Localizar el nodo del cubo recorriendo el JSON por nombre: toJson()
    // cuelga los hijos de la raíz de root->children, cada uno con su propio
    // "mesh" (aquí sin "materials" porque el Cubo no tiene overrides todavía).
    nlohmann::json& hijos = j["root"]["children"];
    nlohmann::json* cuboJson = nullptr;
    for (auto& hijo : hijos)
        if (hijo.value("name", std::string()) == "Cubo") cuboJson = &hijo;
    CHECK(cuboJson != nullptr);
    if (!cuboJson) return;
    (*cuboJson)["mesh"]["materials"] = nlohmann::json::object();

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    // La SUBCADENA del aviso que le toca a ESTE bloque, no solo "hay algún
    // aviso": eso último pasaría igual aunque se borrara el push_back de la
    // rama "!mats.is_array()" y algún otro aviso ajeno colara por casualidad.
    bool warned = false;
    for (const auto& w : cargada.lastWarnings())
        if (w.find("no es una lista") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// Una entrada sin "index" válido se descarta con aviso, sin tirar las demás
// del mismo array: mismo fichero editado a mano que solo corrompe una entrada.
static void test_materials_entry_without_valid_index_is_discarded(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/valida.png";
    go->materialOverrides.push_back(ov);

    nlohmann::json j = scene.toJson();
    nlohmann::json& hijos = j["root"]["children"];
    nlohmann::json* cuboJson = nullptr;
    for (auto& hijo : hijos)
        if (hijo.value("name", std::string()) == "Cubo") cuboJson = &hijo;
    CHECK(cuboJson != nullptr);
    if (!cuboJson) return;
    CHECK((*cuboJson)["mesh"].contains("materials"));
    // Se añade una segunda entrada sin "index": tiene que descartarse SOLA,
    // dejando viva la primera.
    (*cuboJson)["mesh"]["materials"].push_back({ {"albedo", "assets/sin_indice.png"} });

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (!leido) return;
    CHECK(leido->materialOverrides.size() == 1);
    if (leido->materialOverrides.size() == 1)
        CHECK(leido->materialOverrides[0].albedo == "assets/valida.png");

    bool warned = false;
    for (const auto& w : cargada.lastWarnings())
        if (w.find("index") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// GUARDA DE LA DECISIÓN "en los caminos de memoria la ruta viaja verbatim"
// (ronda 2 de revisión): con la raíz FIJADA en la escena, clonar un objeto
// cuyo override es una ruta ABSOLUTA bajo esa raíz tiene que dejar al clon con
// la MISMA cadena, byte a byte — ni relativizada, ni con los separadores
// reescritos por el viaje relative()/weakly_canonical de toStoredPath. Sin
// este test, devolver m_assetRoot a cloneGameObject deja la suite en verde
// mientras el clon recibe una ruta distinta de la del original.
static void test_clone_keeps_override_path_verbatim_with_root_set(PhysicsManager& pm, AudioManager& am)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "dt_mat_root";

    Scene scene("Test");
    scene.setAssetRoot(root.string());
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = (root / "assets" / "x.png").string();
    go->materialOverrides.push_back(ov);

    const std::string original = go->materialOverrides[0].albedo;

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    CHECK(clone->materialOverrides.size() == 1);
    if (clone->materialOverrides.size() == 1)
        CHECK(clone->materialOverrides[0].albedo == original);
}

// Misma guarda para el par subtreeToJson/insertFromJson que usa Undo/Redo de
// Create/Delete: el ciclo completo (capturar snapshot, borrar el original,
// reinsertar desde el snapshot) tiene que devolver la ruta idéntica.
static void test_undo_redo_keeps_override_path_verbatim_with_root_set(PhysicsManager& pm, AudioManager& am)
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "dt_mat_root";

    Scene scene("Test");
    scene.setAssetRoot(root.string());
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = (root / "assets" / "y.png").string();
    go->materialOverrides.push_back(ov);

    const std::string original = go->materialOverrides[0].albedo;
    const nlohmann::json snapshot = scene.subtreeToJson(go);

    // El ciclo real de un Undo de Delete: el nodo se destruye y se reconstruye
    // desde el snapshot capturado ANTES de borrarlo.
    scene.removeGameObject(go);
    GameObject* restored = scene.insertFromJson(snapshot, nullptr, 0, pm, am);
    CHECK(restored != nullptr);
    if (!restored) return;
    CHECK(restored->materialOverrides.size() == 1);
    if (restored->materialOverrides.size() == 1)
        CHECK(restored->materialOverrides[0].albedo == original);
}

// Task 7: nodeFromJson tiene que llamar a applyMaterialOverrides con la malla
// YA PUESTA, para cada una de las tres ramas de carga (aquí, la procedural).
// Sin esa llamada, un objeto recién cargado desde disco se ve con la textura
// del FBX hasta el primer edit que dispare un applyMaterialOverrides externo.
static void test_scene_load_applies_override_to_material(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();   // procedural: sourcePath vacío
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);

    const nlohmann::json j = scene.toJson();
    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (!leido) return;
    CHECK(leido->hasMesh());
    if (leido->hasMesh())
        CHECK(leido->getMesh()->material.texturePath == "assets/mia.png");
}

// Task 7, punto 2 de la revisión: el comentario de GameObject::applyMaterialOverrides
// promete que "el aviso [de index fuera de rango] lo da el lector de escena,
// que es quien tiene canal para darlo". Este test es esa promesa cumplida: un
// index que ya no existe en el mesh recién cargado deja un aviso en
// lastWarnings(), no un fallo silencioso.
static void test_out_of_range_index_warns_on_scene_load(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();   // procedural: expone UN material (índice 0)
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 3;   // fuera de rango: el mesh procedural solo tiene el índice 0
    ov.albedo = "assets/fantasma.png";
    go->materialOverrides.push_back(ov);

    const nlohmann::json j = scene.toJson();
    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));

    // La SUBCADENA de ESTE aviso, no solo "hay algún aviso": mismo criterio
    // que test_corrupt_materials_block_warns.
    bool warned = false;
    for (const auto& w : cargada.lastWarnings())
        if (w.find("fuera de rango") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// El mismo aviso, pero por el camino ASÍNCRONO. El de arriba solo cubre
// Scene::fromJson; una escena grande carga sus mallas por el pump
// (AsyncAssetLoader::applyLoadedMesh), que llamaba a applyMaterialOverrides a
// secas y se comía el índice inválido sin una línea. El aviso vive ahora en
// collectMaterialOverrideWarnings, que es lo que se prueba aquí: los dos
// caminos dicen lo mismo porque llaman a la misma función.
static void test_override_warnings_helper()
{
    GameObject go("Cubo");
    auto mesh = std::make_shared<Mesh>();   // procedural: UN material (índice 0)
    go.setMesh(std::move(mesh));

    MaterialOverride dentro;
    dentro.index  = 0;
    dentro.albedo = "assets/mia.png";
    go.materialOverrides.push_back(dentro);

    std::vector<std::string> avisos;
    collectMaterialOverrideWarnings(go, avisos);
    // Un índice válido no dice nada: si avisara siempre, el aviso no
    // distinguiría nada y el camino async se llenaría de ruido por cada
    // objeto con overrides de una escena grande.
    CHECK(avisos.empty());

    MaterialOverride fuera;
    fuera.index  = 3;
    fuera.albedo = "assets/fantasma.png";
    go.materialOverrides.push_back(fuera);

    collectMaterialOverrideWarnings(go, avisos);
    CHECK(avisos.size() == 1);
    if (avisos.size() == 1)
    {
        // El texto, no solo el número: es lo que el usuario lee en el Log, y
        // tiene que decir de QUÉ objeto e índice habla.
        CHECK(avisos[0].find("Cubo") != std::string::npos);
        CHECK(avisos[0].find("index 3") != std::string::npos);
        CHECK(avisos[0].find("fuera de rango") != std::string::npos);
    }

    // Un negativo cuenta igual que un índice pasado: applyMaterialOverrides los
    // descarta por la misma condición.
    MaterialOverride negativo;
    negativo.index  = -1;
    negativo.albedo = "assets/otro.png";
    go.materialOverrides.push_back(negativo);
    avisos.clear();
    collectMaterialOverrideWarnings(go, avisos);
    CHECK(avisos.size() == 2);

    // Sin malla no hay materiales contra los que comparar: no es que todos los
    // índices estén fuera de rango, es que la pregunta no aplica todavía (el
    // pump llama a esto DESPUÉS del setMesh, pero el orden lo garantiza el
    // caller, no esta función).
    GameObject sinMalla("Vacio");
    sinMalla.materialOverrides.push_back(fuera);
    avisos.clear();
    collectMaterialOverrideWarnings(sinMalla, avisos);
    CHECK(avisos.empty());
}

// Task 7, punto 1 de la revisión: la trampa del clon. cloneGameObject siembra
// la malla del clon desde una PreloadedMeshCache con la malla VIVA del
// original, es decir con el material YA PISADO por el override. Sin el
// baseline real viajando en el JSON de clonado (carryOverrideBaseline), el
// clon capturaría como "original" la textura del override, y un Clear sobre
// el clon dejaría puesta esa textura en vez de devolver la del FBX.
static void test_clone_clear_restores_fbx_texture_not_override(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    // sourcePath no vacío: es lo que hace que cloneGameObject use la
    // PreloadedMeshCache (mallas) en vez de ir a disco (que fallaría, el
    // fichero no existe, y el clon se quedaría sin mesh).
    mesh->sourcePath = "assets/cubo.fbx";
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));

    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);
    // El material VIVO de go ya trae el override horneado, igual que un
    // objeto editado en el editor antes de duplicarlo.
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == "assets/mia.png");

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    CHECK(clone->materialOverrides.size() == 1);
    if (clone->materialOverrides.empty()) return;

    // Clear en el CLON, no en el original.
    clone->materialOverrides[0].albedo.clear();
    applyMaterialOverrides(*clone);

    CHECK(clone->getMesh()->material.texturePath == "assets/fbx_albedo.png");
}

// Ronda de revisión de Task 7, punto 1 (Critical): r.images trae los píxeles
// que el worker decodificó del FBX ANTES de que ningún override pisara nada
// (AsyncAssetLoader::runJob), y Renderer::createSharedGpuMesh (Vulkan)
// PREFIERE esos píxeles ya decodificados sobre la ruta del material — sin
// filtrarlos, un override sobre un FBX con textura propia subiría a GPU la
// del FBX. Este es el único seam de ese bug que se puede probar sin GPU: la
// función de filtrado en sí, aislada de applyLoadedMesh (que sí necesita un
// EditorRenderer real y se queda sin cubrir a nivel de test, igual que el
// resto de esa función — se verifica en GUI). Comprueba que SOLO se descarta
// el slot que el override pisa, dejando los demás intactos para que el
// trabajo del worker no se tire entero.
static void test_discard_overridden_decoded_images_removes_only_overridden_slot()
{
    DecodedImage albedo; albedo.slot = DecodedImage::Albedo; albedo.w = 1; albedo.h = 1; albedo.pixels = {1, 2, 3, 4};
    DecodedImage normal; normal.slot = DecodedImage::Normal; normal.w = 1; normal.h = 1; normal.pixels = {5, 6, 7, 8};
    DecodedImage orm;    orm.slot    = DecodedImage::ORM;    orm.w    = 1; orm.h    = 1; orm.pixels = {9, 10, 11, 12};
    std::vector<DecodedImage> images{albedo, normal, orm};

    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";   // solo el albedo está overrideado
    std::vector<MaterialOverride> overrides{ov};

    discardOverriddenDecodedImages(images, overrides);

    CHECK(images.size() == 2);
    bool hasAlbedo = false, hasNormal = false, hasOrm = false;
    for (const DecodedImage& img : images)
    {
        if (img.slot == DecodedImage::Albedo) hasAlbedo = true;
        if (img.slot == DecodedImage::Normal) hasNormal = true;
        if (img.slot == DecodedImage::ORM)    hasOrm    = true;
    }
    CHECK(!hasAlbedo);
    CHECK(hasNormal);
    CHECK(hasOrm);
}

// r.images solo decodifica Mesh::material (índice 0, ver el comentario de
// runJob): un override de OTRO índice (SkinnedMesh multi-material) no tiene
// nada que descartar en este vector.
static void test_discard_overridden_decoded_images_ignores_other_index()
{
    DecodedImage albedo; albedo.slot = DecodedImage::Albedo;
    std::vector<DecodedImage> images{albedo};
    MaterialOverride ov;
    ov.index  = 2;
    ov.albedo = "assets/mia.png";
    std::vector<MaterialOverride> overrides{ov};

    discardOverriddenDecodedImages(images, overrides);

    CHECK(images.size() == 1);
}

// Ronda de revisión de Task 7, punto 2 (Important): el baseline pertenece a
// LA MALLA de la que salió. setMesh es el único punto por el que cambia la
// malla, así que tiene que resetear ahí los base*/base*Taken — si
// sobrevivieran a un cambio de malla (a null, o a otra distinta), un Clear
// posterior devolvería la textura de un modelo que ya no es el que está
// cargado.
static void test_set_mesh_resets_stale_baseline()
{
    auto go = makeStaticFixture();
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);
    CHECK(go->materialOverrides[0].baseAlbedoTaken);
    CHECK(go->materialOverrides[0].baseAlbedo == "assets/fbx_albedo.png");

    // setMesh(nullptr): lo que hace el catch de AsyncAssetLoader::applyLoadedMesh
    // (antes de este fix, restauraba la malla pero no el baseline) y lo que
    // hace Renderer::removeMeshComponent al quitar el componente.
    go->setMesh(nullptr);

    CHECK(!go->materialOverrides[0].baseAlbedoTaken);
    CHECK(go->materialOverrides[0].baseAlbedo.empty());
    // El override en sí sigue vivo (Remove no lo borra): solo el baseline se
    // invalida.
    CHECK(go->materialOverrides[0].albedo == "assets/mia.png");
}

// Mismo mecanismo que el test de arriba, pero de punta a punta con el
// escenario exacto que describía la revisión: un load fallido captura un
// baseline de una malla que nunca llega a cuajar, y el siguiente load
// (distinto FBX) tiene que capturar el SUYO, no heredar el de antes.
static void test_reload_after_failed_load_recaptures_correct_baseline()
{
    auto go = makeStaticFixture();   // material.texturePath == "assets/fbx_albedo.png"
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);
    CHECK(go->materialOverrides[0].baseAlbedo == "assets/fbx_albedo.png");

    // Simula el catch de applyLoadedMesh: el registro en GPU lanzó, se
    // deshace el setMesh. Sin el reset de setMesh, el baseline de arriba
    // ("assets/fbx_albedo.png") sobreviviría aquí aunque esa malla nunca
    // llegó a quedarse cargada.
    go->setMesh(nullptr);

    // Simula el siguiente intento con OTRO FBX, esta vez con éxito: setMesh
    // seguido de applyMaterialOverrides, igual que hace applyLoadedMesh.
    auto otraMalla = std::make_shared<Mesh>();
    otraMalla->material.texturePath = "assets/otro_fbx.png";
    go->setMesh(otraMalla);
    applyMaterialOverrides(*go);

    CHECK(go->materialOverrides[0].baseAlbedo == "assets/otro_fbx.png");
    CHECK(go->getMesh()->material.texturePath == "assets/mia.png");

    // Clear: sin el fix, esto devolvía "assets/fbx_albedo.png" (la malla que
    // nunca llegó a cargar), no la del FBX realmente cargado.
    go->materialOverrides[0].albedo.clear();
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == "assets/otro_fbx.png");
}

// Undo/redo de una asignación, con el renderer a nullptr (sin GPU): lo que se
// prueba es el dato, que es lo único que sobrevive al ciclo.
static void test_command_undo_redo_assignment(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;

    MaterialTextureCommand cmd(scene, nullptr, "Textura de 'Cubo'", id, 0,
                                MaterialTextureSlot::Albedo, "", "assets/mia.png");
    cmd.execute();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/mia.png");

    cmd.undo();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/fbx_albedo.png");

    cmd.execute();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/mia.png");
    (void)pm; (void)am;
}

// Undo de un Clear: vuelve a poner la ruta que el usuario había asignado.
static void test_command_undo_of_clear(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;

    setMaterialTextureOverride(*go, 0, MaterialTextureSlot::Albedo, "assets/mia.png");

    MaterialTextureCommand clear(scene, nullptr, "Quitar textura", id, 0,
                                  MaterialTextureSlot::Albedo, "assets/mia.png", "");
    clear.execute();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/fbx_albedo.png");

    clear.undo();
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/mia.png");
    (void)pm; (void)am;
}

// El comando resuelve por id en CADA aplicación: un puntero guardado quedaría
// colgando tras un undo de Delete que reconstruya el objeto.
static void test_command_survives_object_rebuild(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->material.texturePath = "assets/fbx_albedo.png";
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;

    MaterialTextureCommand cmd(scene, nullptr, "Textura", id, 0,
                                MaterialTextureSlot::Albedo, "", "assets/mia.png");

    // El objeto desaparece: el comando no puede reventar ni escribir en memoria
    // liberada, solo no hacer nada.
    scene.removeGameObject(scene.findById(id));
    cmd.execute();
    CHECK(scene.findById(id) == nullptr);
    (void)pm; (void)am;
}

// Sin setMesh: el objeto no tiene material donde aplicar nada. Sin la guarda
// !go->hasMesh() en apply(), setMaterialTextureOverride le crearía igual una
// entrada huérfana en materialOverrides -- un override escrito sobre un
// objeto que no tiene malla, sin efecto visible y sin que nada lo delate.
static void test_command_on_object_without_mesh_is_noop(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("SinMalla");
    const uint64_t id = go->id;

    MaterialTextureCommand cmd(scene, nullptr, "Textura", id, 0,
                                MaterialTextureSlot::Albedo, "", "assets/mia.png");
    cmd.execute();
    CHECK(scene.findById(id)->materialOverrides.empty());
    (void)pm; (void)am;
}

// El índice era válido cuando se construyó el comando (skinned con 3
// materiales, índice 2 = "pelo"), pero antes de que undo/redo lo reproduzca
// la malla cambia a un Mesh plano de UN solo material -- el mismo escenario
// que describe el comentario de la guarda en Command.cpp. Sin
// "m_materialIndex >= mats.size()" en apply(), setMaterialTextureOverride
// crearía igual una entrada huérfana con index=2 en materialOverrides (el
// propio corte de applyMaterialOverrides evita la escritura fuera de rango
// en el Material, pero no evita la entrada huérfana): el objeto se queda con
// un override serializable que no describe ningún material real del mesh
// actual, silencioso hasta el siguiente guardado.
static void test_command_stale_index_after_mesh_shrinks(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto skinned = std::make_shared<SkinnedMesh>();
    skinned->materials.resize(3);
    skinned->materials[0].texturePath = "assets/cuerpo.png";
    skinned->materials[1].texturePath = "assets/ropa.png";
    skinned->materials[2].texturePath = "assets/pelo.png";
    go->setMesh(skinned);
    const uint64_t id = go->id;

    // Comando construido para el índice 2 ("pelo") mientras el mesh es
    // skinned con 3 materiales.
    MaterialTextureCommand cmd(scene, nullptr, "Textura de pelo", id, 2,
                                MaterialTextureSlot::Albedo, "", "assets/mia.png");

    // La malla se sustituye por un Mesh estático plano: UN solo material, sin
    // que el comando se entere -- exactamente el reordenamiento que el
    // comentario de la guarda advierte.
    auto plano = std::make_shared<Mesh>();
    plano->material.texturePath = "assets/plano.png";
    go->setMesh(plano);

    cmd.execute();
    CHECK(scene.findById(id)->materialOverrides.empty());
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/plano.png");

    cmd.undo();
    CHECK(scene.findById(id)->materialOverrides.empty());
    CHECK(scene.findById(id)->getMesh()->material.texturePath == "assets/plano.png");
    (void)pm; (void)am;
}

// ---------------------------------------------------------------------------
// Factores PBR (metallic/roughness) del material: mismo mecanismo que los
// tres slots de textura de arriba, pero con un centinela float (-1.0f, fuera
// del rango 0..1 del slider) en vez de una cadena vacía — ver la nota grande
// de MaterialOverride en GameObject.h.
// ---------------------------------------------------------------------------

// Asignar un factor pisa el material y guarda como baseline lo que traía el
// modelo (aquí, los defaults de Material: metallic=0.0f, roughness=0.5f). El
// factor que no se toca se queda en su centinela, sin baseline capturado.
static void test_factor_override_writes_material_and_captures_baseline()
{
    auto go = makeStaticFixture();
    MaterialOverride ov;
    ov.index    = 0;
    ov.metallic = 0.8f;
    go->materialOverrides.push_back(ov);

    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.metallic == 0.8f);
    CHECK(go->materialOverrides[0].baseMetallic == 0.0f);
    CHECK(go->materialOverrides[0].baseMetallicTaken);
    // roughness no se tocó: sigue en el centinela, y el material se queda con
    // el default del modelo, no con 0.0.
    CHECK(go->materialOverrides[0].roughness < 0.0f);
    CHECK(!go->materialOverrides[0].baseRoughnessTaken);
    CHECK(go->getMesh()->material.roughness == 0.5f);
}

// Cambiar de valor NO mueve el baseline: sigue siendo el del modelo, no el
// primer valor que puso el usuario. Mismo motivo que
// test_second_override_keeps_original_baseline con las texturas.
static void test_factor_second_change_keeps_original_baseline()
{
    auto go = makeStaticFixture();
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].index    = 0;
    go->materialOverrides[0].metallic = 0.3f;
    applyMaterialOverrides(*go);

    go->materialOverrides[0].metallic = 0.9f;
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.metallic == 0.9f);
    CHECK(go->materialOverrides[0].baseMetallic == 0.0f);
}

// Clear (el centinela -1.0f) = el material vuelve al baseline capturado, o
// sea al valor del modelo. No hay botón "Clear" propio para los factores en
// el panel (a diferencia de las texturas), pero el mecanismo de datos es el
// mismo y se prueba igual, directo sobre el override.
static void test_factor_clear_restores_baseline()
{
    auto go = makeStaticFixture();
    go->materialOverrides.push_back(MaterialOverride{});
    go->materialOverrides[0].index    = 0;
    go->materialOverrides[0].metallic = 0.75f;
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.metallic == 0.75f);

    go->materialOverrides[0].metallic = -1.0f;
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.metallic == 0.0f);
}

// El baseline de un factor pertenece a LA MALLA de la que salió, igual que el
// de una textura: setMesh es el único punto por el que cambia la malla, así
// que tiene que resetear ahí baseMetallicTaken/baseRoughnessTaken.
static void test_set_mesh_resets_stale_factor_baseline()
{
    auto go = makeStaticFixture();
    MaterialOverride ov;
    ov.index    = 0;
    ov.metallic = 0.5f;
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);
    CHECK(go->materialOverrides[0].baseMetallicTaken);

    go->setMesh(nullptr);

    CHECK(!go->materialOverrides[0].baseMetallicTaken);
    // El override en sí sigue vivo (Remove no lo borra): solo el baseline se
    // invalida.
    CHECK(go->materialOverrides[0].metallic == 0.5f);
}

// Round-trip: metallic Y roughness sobreviven a guardar y cargar.
static void test_factor_overrides_survive_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    // Procedural: "assets/cubo.fbx" no existe en disco (ver el comentario de
    // test_factor_absent_in_json_does_not_touch_material, más abajo) y este
    // test SÍ necesita que la recarga deje malla puesta para comprobar el
    // material.
    auto mesh = std::make_shared<Mesh>();
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index     = 0;
    ov.metallic  = 0.6f;
    ov.roughness = 0.2f;
    go->materialOverrides = {ov};

    const nlohmann::json j = scene.toJson();

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(j, pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (!leido) return;
    CHECK(leido->materialOverrides.size() == 1);
    if (leido->materialOverrides.empty()) return;
    CHECK(leido->materialOverrides[0].metallic  == 0.6f);
    CHECK(leido->materialOverrides[0].roughness == 0.2f);
    // El baseline NO viaja por disco: la aserción que de verdad lo prueba es
    // que la CADENA "baseMetallic"/"baseRoughness" no aparece en el propio
    // JSON (gateado por carryOverrideBaseline, que Scene::toJson() nunca
    // pone a true) -- mismo criterio que
    // test_factor_absent_in_json_does_not_touch_material con "metallic".
    // Ronda de revisión: comprobar baseMetallicTaken/baseMetallic del objeto
    // RECARGADO no discrimina nada, porque en este fixture el origen ya
    // tiene baseMetallicTaken=false y baseMetallic=0.0f -- si el bug
    // escribiera esas claves también en disco, LEERLAS daría los mismos dos
    // valores (recapturar desde un Material fresco con metallic=0.0 produce
    // exactamente "taken=true, base=0.0" igual que si esos valores
    // vinieran del JSON), y el test pasaría con el bug puesto.
    CHECK(j.dump().find("baseMetallic")  == std::string::npos);
    CHECK(j.dump().find("baseRoughness") == std::string::npos);
    // Estas dos siguen siendo ciertas y documentan el comportamiento real
    // (recaptura en la carga, no lectura del fichero), pero no son las que
    // defienden la regresión -- las de arriba sí.
    CHECK(leido->materialOverrides[0].baseMetallicTaken);
    CHECK(leido->materialOverrides[0].baseMetallic == 0.0f);
    // Y ya aplicados sobre el material (nodeFromJson llama a
    // applyMaterialOverrides con la malla puesta, Task 7).
    CHECK(leido->hasMesh());
    if (leido->hasMesh())
    {
        CHECK(leido->getMesh()->material.metallic  == 0.6f);
        CHECK(leido->getMesh()->material.roughness == 0.2f);
    }
}

// Un metallic/roughness ausente en el JSON no toca el material: se queda con
// el default del modelo, no se fuerza a 0. El override de albedo SÍ está
// presente (para que el bloque "materials" exista en el JSON) pero sin las
// claves "metallic"/"roughness".
static void test_factor_absent_in_json_does_not_touch_material(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    // Procedural (sourcePath vacío): "assets/cubo.fbx" no existe en disco, y
    // con un sourcePath que no resuelve, ModelLoader::load lanza y el catch de
    // nodeFromJson deja el nodo sin mesh -- justo lo que este test necesita
    // evitar para poder comprobar el material tras la recarga (mismo motivo
    // que documenta test_corrupt_materials_block_warns más arriba).
    auto mesh = std::make_shared<Mesh>();
    go->setMesh(std::move(mesh));
    MaterialOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);

    const std::string texto = scene.toJson().dump();
    CHECK(texto.find("\"metallic\"") == std::string::npos);
    CHECK(texto.find("\"roughness\"") == std::string::npos);

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(scene.toJson(), pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (!leido || !leido->hasMesh()) return;
    CHECK(leido->getMesh()->material.metallic  == 0.0f);
    CHECK(leido->getMesh()->material.roughness == 0.5f);
    CHECK(leido->materialOverrides[0].metallic  < 0.0f);
    CHECK(leido->materialOverrides[0].roughness < 0.0f);
}

// Undo/redo de un MaterialFactorCommand, calcado de
// test_command_undo_redo_assignment con las texturas: el "antes" es el
// centinela -1.0f (sin override), igual que "" lo es para una ruta.
static void test_factor_command_undo_redo(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;

    MaterialFactorCommand cmd(scene, nullptr, "Metallic de 'Cubo'", id, 0,
                               MaterialFactorSlot::Metallic, -1.0f, 0.8f);
    cmd.execute();
    CHECK(scene.findById(id)->getMesh()->material.metallic == 0.8f);

    cmd.undo();
    CHECK(scene.findById(id)->getMesh()->material.metallic == 0.0f);

    cmd.execute();
    CHECK(scene.findById(id)->getMesh()->material.metallic == 0.8f);
    (void)pm; (void)am;
}

// Ronda de revisión: el "before" de un MaterialFactorCommand construido por
// el panel tiene que ser el OVERRIDE CRUDO (el centinela, si el slot nunca
// se había tocado), NO el valor EFECTIVO ya aplicado (mat.metallic) --
// exactamente el bug que tenía PropertiesPanel::drawTexturesSection antes de
// este fix (antes de que currentFactorOverride() sustituyera a mat.metallic
// como "before" en la construcción del comando). Con el efectivo como
// "before", deshacer la PRIMERA edición sobre un factor nunca tocado
// escribiría el valor del modelo COMO OVERRIDE ACTIVO -- y nodeToJson SÍ
// serializa un override activo: el factor quedaría clavado en el .scene
// para siempre, resucitando en cada carga aunque el usuario nunca lo
// hubiera tocado (y pisando en silencio un metallic distinto si el FBX se
// reexporta más tarde).
static void test_factor_command_undo_of_first_edit_leaves_no_active_override(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    // Sin override todavía: el estado que currentFactorOverride() lee como
    // "-1.0f, sin override" y que mat.metallic lee como "0.0f, el default"
    // -- las dos lecturas que el bug confundía.
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;
    CHECK(go->getMesh()->material.metallic == 0.0f);   // el EFECTIVO
    CHECK(go->materialOverrides.empty());               // el override CRUDO: ninguno

    // before = -1.0f (el centinela; lo que currentFactorOverride() devuelve
    // para un slot sin entrada en materialOverrides), NO 0.0f (lo que
    // devolvería mat.metallic, el bug).
    MaterialFactorCommand cmd(scene, nullptr, "Metallic de 'Cubo'", id, 0,
                               MaterialFactorSlot::Metallic, -1.0f, 0.8f);
    cmd.execute();
    CHECK(scene.findById(id)->materialOverrides.size() == 1);
    if (scene.findById(id)->materialOverrides.size() == 1)
        CHECK(scene.findById(id)->materialOverrides[0].metallic == 0.8f);

    cmd.undo();

    // Con el bug (before=0.0f, el efectivo) esto habría dejado
    // materialOverrides[0].metallic == 0.0f -- un override ACTIVO con el
    // valor del modelo, no el centinela. Con el fix, vuelve a -1.0f: sin
    // override.
    GameObject* despues = scene.findById(id);
    CHECK(despues->materialOverrides.size() == 1);
    if (!despues->materialOverrides.empty())
        CHECK(despues->materialOverrides[0].metallic < 0.0f);
    CHECK(despues->getMesh()->material.metallic == 0.0f);

    // Y el round-trip por JSON no lo resucita: sin override activo (el
    // centinela), nodeToJson no escribe la clave "metallic" para este
    // objeto, así que una recarga no trae de vuelta nada.
    const std::string texto = scene.toJson().dump();
    CHECK(texto.find("\"metallic\"") == std::string::npos);

    Scene cargada("Vacia");
    CHECK(cargada.fromJson(scene.toJson(), pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (leido && leido->hasMesh())
        CHECK(leido->getMesh()->material.metallic == 0.0f);
}

// Sin setMesh: mismo criterio que test_command_on_object_without_mesh_is_noop.
static void test_factor_command_on_object_without_mesh_is_noop(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("SinMalla");
    const uint64_t id = go->id;

    MaterialFactorCommand cmd(scene, nullptr, "Metallic", id, 0,
                               MaterialFactorSlot::Metallic, -1.0f, 0.8f);
    cmd.execute();
    CHECK(scene.findById(id)->materialOverrides.empty());
    (void)pm; (void)am;
}

// Índice válido al construir el comando, mesh encogido antes del replay:
// mismo escenario que test_command_stale_index_after_mesh_shrinks.
static void test_factor_command_stale_index_after_mesh_shrinks(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto skinned = std::make_shared<SkinnedMesh>();
    skinned->materials.resize(3);
    go->setMesh(skinned);
    const uint64_t id = go->id;

    MaterialFactorCommand cmd(scene, nullptr, "Metallic de pelo", id, 2,
                               MaterialFactorSlot::Metallic, -1.0f, 0.8f);

    auto plano = std::make_shared<Mesh>();
    go->setMesh(plano);

    cmd.execute();
    CHECK(scene.findById(id)->materialOverrides.empty());

    cmd.undo();
    CHECK(scene.findById(id)->materialOverrides.empty());
    (void)pm; (void)am;
}

// El objeto desaparece entre construir el comando y ejecutarlo: mismo
// criterio que test_command_survives_object_rebuild.
static void test_factor_command_survives_object_rebuild(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    go->setMesh(std::move(mesh));
    const uint64_t id = go->id;

    MaterialFactorCommand cmd(scene, nullptr, "Metallic", id, 0,
                               MaterialFactorSlot::Metallic, -1.0f, 0.8f);

    scene.removeGameObject(scene.findById(id));
    cmd.execute();
    CHECK(scene.findById(id) == nullptr);
    (void)pm; (void)am;
}

// GUARDA para el mismo hallazgo que el baseline de textura en clonar (ver
// test_clone_clear_restores_fbx_texture_not_override): sin
// baseMetallic/baseMetallicTaken viajando en el JSON de MEMORIA
// (carryOverrideBaseline, el que usa cloneGameObject), el clon capturaría
// como "original" el valor YA HORNEADO del override, y un Clear sobre el
// clon devolvería ESE valor en vez del metallic del modelo. Sin este test
// pasaba desapercibido: ningún otro cubre el camino de memoria para los
// factores, solo el de disco (test_factor_overrides_survive_round_trip,
// que nunca lleva el baseline).
static void test_factor_clone_clear_restores_model_value_not_override(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    // sourcePath no vacío: mismo motivo que
    // test_clone_clear_restores_fbx_texture_not_override -- hace que
    // cloneGameObject reuse la malla YA CARGADA (PreloadedMeshCache) en vez
    // de ir a disco, donde "assets/cubo.fbx" no existe.
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));

    MaterialOverride ov;
    ov.index    = 0;
    ov.metallic = 0.9f;
    go->materialOverrides.push_back(ov);
    // El material VIVO de go ya trae el override horneado, igual que un
    // objeto editado en el editor antes de duplicarlo.
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.metallic == 0.9f);

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    CHECK(clone->materialOverrides.size() == 1);
    if (clone->materialOverrides.empty()) return;

    // Clear en el CLON, no en el original.
    clone->materialOverrides[0].metallic = -1.0f;
    applyMaterialOverrides(*clone);

    // Sin el fix, esto devolvía 0.9 (el override horneado que trajo el
    // clon), no el default del modelo.
    CHECK(clone->getMesh()->material.metallic == 0.0f);
}

// --- Sliders de Metallic/Roughness contra un ImGui de VERDAD, sin ventana ---
//
// El bug que tapan estos tests no estaba en ningun comando ni en el Material:
// estaba en QUE FRAME entrega ImGui el valor de un SliderFloat. Por eso se
// conduce el widget real con eventos de raton, en vez de llamar a draw() con
// valores inventados: un doble del slider habria heredado la misma suposicion
// falsa que causo el bug.
struct ImGuiSinVentana
{
    ImGuiContext* ctx = nullptr;

    ImGuiSinVentana()
    {
        ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename  = nullptr;
        io.LogFilename  = nullptr;
        io.DisplaySize  = ImVec2(800.0f, 600.0f);
        io.DeltaTime    = 1.0f / 60.0f;
        // Sin backend de render: con este flag el atlas de fuentes se construye
        // solo en NewFrame y nadie tiene que subir la textura a ningun lado.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }
    ~ImGuiSinVentana() { ImGui::DestroyContext(ctx); }

    template<typename Fn> void frame(Fn&& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(400.0f, 200.0f));
        ImGui::Begin("Test", nullptr, ImGuiWindowFlags_NoDecoration |
                                      ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }
};

// Clic al 10 % del slider, arrastre hasta pasado el tope derecho (clampa a 1.0)
// y soltar. Un evento por frame: ImGui los reparte asi de todos modos
// (ConfigInputTrickleEventQueue), y separados se sabe que frame hace que.
// `body` dibuja el slider y nada detras, para que GetItemRect* sea el suyo.
template<typename Fn>
static void arrastraHastaElTope(ImGuiSinVentana& ui, Fn&& drawSlider)
{
    ImVec2 min(0.0f, 0.0f), max(0.0f, 0.0f);
    auto body = [&] {
        drawSlider();
        min = ImGui::GetItemRectMin();
        max = ImGui::GetItemRectMax();
    };
    ImGuiIO& io = ImGui::GetIO();
    ui.frame(body);                                                  // layout
    const float y = (min.y + max.y) * 0.5f;
    io.AddMousePosEvent(min.x + (max.x - min.x) * 0.1f, y); ui.frame(body);  // hover
    io.AddMouseButtonEvent(0, true);                        ui.frame(body);  // clic
    io.AddMousePosEvent(max.x + 50.0f, y);                  ui.frame(body);  // arrastre
    io.AddMouseButtonEvent(0, false);                       ui.frame(body);  // soltar
    ui.frame(body);
}

// CARACTERIZACION de ImGui, no de codigo nuestro: es la premisa del arreglo, y
// si una version futura de ImGui la cambia este test lo dice antes que nadie.
// El patron que tenia el panel -local desde el dato, commit leyendo la local en
// IsItemDeactivatedAfterEdit- con un dato que NO se escribe en vivo: ImGui SI
// avisa de que hubo edicion, pero ese frame no escribe el valor, y la local
// vale lo de antes del arrastre.
static void test_imgui_slider_release_frame_does_not_deliver_value()
{
    ImGuiSinVentana ui;
    const float material = 0.0f;
    float maxVisto = -1.0f, commitIngenuo = -1.0f;
    bool  huboCommit = false;
    arrastraHastaElTope(ui, [&] {
        float v = material;
        ImGui::SliderFloat("Metallic", &v, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemActive()) maxVisto = std::max(maxVisto, v);
        if (ImGui::IsItemDeactivatedAfterEdit()) { huboCommit = true; commitIngenuo = v; }
    });
    CHECK(maxVisto > 0.99f);            // el arrastre llego al tope
    CHECK(huboCommit);                  // e ImGui avisa de que hubo edicion
    CHECK(commitIngenuo == material);   // pero la local no trae el valor
}

// El bug que vio el usuario: arrastrar Metallic a 1 y que al soltar el slider
// volviera a 0 aunque el objeto se viera metalico. El commit tiene que traer el
// valor ARRASTRADO, y `begin` el del dato antes del clic -0.25-, no el 0.1 al
// que SliderFloat salta en el frame del clic.
static void test_deferred_slider_commits_dragged_value()
{
    ImGuiSinVentana ui;
    DeferredSliderFloat slider;
    float material = 0.25f;
    int   commits = 0;
    bool  vivo = false;
    float begin = -1.0f, value = -1.0f;
    arrastraHastaElTope(ui, [&] {
        const auto r = slider.draw("Metallic", material, 0.0f, 1.0f, "%.2f");
        if (r.active) vivo = true;
        if (r.committed)
        {
            ++commits;
            begin    = r.begin;
            value    = r.value;
            material = r.value;   // lo que hace MaterialFactorCommand
        }
    });
    CHECK(vivo);
    CHECK(commits == 1);
    CHECK(begin == 0.25f);
    CHECK(value > 0.99f);
    CHECK(material > 0.99f);
}

// El widget desaparece a mitad de arrastre (la seleccion pasa a un objeto sin
// malla) y vuelve despues: tiene que enseñar el dato, no el pendiente de un
// arrastre que nunca se entrego. Sin la comprobacion de frame en draw(),
// m_activeId se queda puesto y el slider reaparece con el valor abandonado.
static void test_deferred_slider_forgets_drag_of_vanished_widget()
{
    ImGuiSinVentana ui;
    DeferredSliderFloat slider;
    const float material = 0.25f;
    ImVec2 min(0.0f, 0.0f), max(0.0f, 0.0f);
    DeferredSliderFloat::Result r;
    auto conSlider = [&] {
        r   = slider.draw("Metallic", material, 0.0f, 1.0f, "%.2f");
        min = ImGui::GetItemRectMin();
        max = ImGui::GetItemRectMax();
    };
    auto sinSlider = [] { ImGui::TextUnformatted("otro objeto"); };

    ImGuiIO& io = ImGui::GetIO();
    ui.frame(conSlider);
    const float y = (min.y + max.y) * 0.5f;
    io.AddMousePosEvent(min.x + (max.x - min.x) * 0.1f, y); ui.frame(conSlider);
    io.AddMouseButtonEvent(0, true);                        ui.frame(conSlider);
    io.AddMousePosEvent(max.x + 50.0f, y);                  ui.frame(conSlider);
    CHECK(r.active && r.value > 0.99f);   // el arrastre esta en marcha

    // Varios frames sin el widget, como al ir a otro objeto y volver con un clic
    // en el Hierarchy. Con UNO solo no vale: ImGui deja que un widget que
    // reaparece justo al frame siguiente de perder el ActiveId vea esa
    // desactivacion (IsItemDeactivatedAfterEdit), y eso ya es comportamiento de
    // ImGui, no del arrastre abandonado que se prueba aqui.
    ui.frame(sinSlider);
    io.AddMouseButtonEvent(0, false); ui.frame(sinSlider);
    io.AddMousePosEvent(0.0f, 0.0f);  ui.frame(sinSlider);
    ui.frame(sinSlider);
    ui.frame(conSlider);
    CHECK(!r.active);
    CHECK(!r.committed);
    CHECK(r.value == material);
}

int main()
{
    // PhysicsManager/AudioManager comparten instancia entre los tests que la
    // necesitan: crear y destruir un PhysicsManager por test crashea al
    // segundo init (una PxFoundation por proceso), mismo patrón que
    // audio_tests.cpp.
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    if (!am.init())
        std::printf("AVISO: FMOD no disponible; los tests que lo necesitan se saltarán\n");

    test_materials_of_static_mesh();
    test_materials_of_skinned_mesh();
    test_override_writes_material_and_captures_baseline();
    test_second_override_keeps_original_baseline();
    test_clear_restores_baseline();
    test_skinned_override_touches_only_its_index();
    test_out_of_range_index_is_ignored();
    test_slots_are_independent();
    test_normal_and_orm_overrides_write_their_own_field();
    test_clear_restores_empty_baseline_on_procedural_mesh();
    test_no_mesh_is_noop();
    test_path_wins_over_embedded();
    test_embedded_when_no_path();
    test_none_when_empty();
    test_path_when_no_embedded();
    test_overrides_survive_round_trip(pm, am);
    test_no_overrides_writes_no_key();
    test_scene_without_materials_key_loads_clean(pm, am);
    test_path_under_root_is_stored_relative(pm, am);
    test_path_outside_root_stays_absolute();
    test_without_root_path_is_verbatim(pm, am);
    test_corrupt_materials_block_warns(pm, am);
    test_materials_entry_without_valid_index_is_discarded(pm, am);
    test_clone_keeps_override_path_verbatim_with_root_set(pm, am);
    test_undo_redo_keeps_override_path_verbatim_with_root_set(pm, am);
    test_overrides_applied_to_incoming_mesh();
    test_scene_load_applies_override_to_material(pm, am);
    test_out_of_range_index_warns_on_scene_load(pm, am);
    test_override_warnings_helper();
    test_clone_clear_restores_fbx_texture_not_override(pm, am);
    test_discard_overridden_decoded_images_removes_only_overridden_slot();
    test_discard_overridden_decoded_images_ignores_other_index();
    test_set_mesh_resets_stale_baseline();
    test_reload_after_failed_load_recaptures_correct_baseline();
    test_command_undo_redo_assignment(pm, am);
    test_command_undo_of_clear(pm, am);
    test_command_survives_object_rebuild(pm, am);
    test_command_on_object_without_mesh_is_noop(pm, am);
    test_command_stale_index_after_mesh_shrinks(pm, am);

    test_factor_override_writes_material_and_captures_baseline();
    test_factor_second_change_keeps_original_baseline();
    test_factor_clear_restores_baseline();
    test_set_mesh_resets_stale_factor_baseline();
    test_factor_overrides_survive_round_trip(pm, am);
    test_factor_absent_in_json_does_not_touch_material(pm, am);
    test_factor_command_undo_redo(pm, am);
    test_factor_command_undo_of_first_edit_leaves_no_active_override(pm, am);
    test_factor_command_on_object_without_mesh_is_noop(pm, am);
    test_factor_command_stale_index_after_mesh_shrinks(pm, am);
    test_factor_command_survives_object_rebuild(pm, am);
    test_factor_clone_clear_restores_model_value_not_override(pm, am);

    test_imgui_slider_release_frame_does_not_deliver_value();
    test_deferred_slider_commits_dragged_value();
    test_deferred_slider_forgets_drag_of_vanished_widget();

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL MATERIAL TEXTURE TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
