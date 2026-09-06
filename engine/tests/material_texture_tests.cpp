// Test headless de los overrides de textura del Mesh (sin GPU). Plain main +
// asserts, sin framework — mismo patrón que content_browser_tests.cpp.
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Renderer/MaterialTextureSource.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include <nlohmann/json.hpp>

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
    MaterialTextureOverride ov;
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
    go->materialOverrides.push_back(MaterialTextureOverride{});
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
    go->materialOverrides.push_back(MaterialTextureOverride{});
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
    MaterialTextureOverride ov;
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
    MaterialTextureOverride ov;
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

    MaterialTextureOverride ov;
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

    MaterialTextureOverride ov;
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

    MaterialTextureOverride ov;
    ov.index  = 0;
    ov.albedo = "assets/mia.png";
    go->materialOverrides.push_back(ov);
    applyMaterialOverrides(*go);
    CHECK(go->getMesh()->material.texturePath == "assets/mia.png");

    go->materialOverrides[0].albedo.clear();
    applyMaterialOverrides(*go);

    CHECK(go->getMesh()->material.texturePath.empty());
}

// Un GameObject sin mesh no revienta.
static void test_no_mesh_is_noop()
{
    GameObject go("Vacio");
    go.materialOverrides.push_back(MaterialTextureOverride{});
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

// Round-trip: los overrides de TODOS los indices sobreviven a guardar y cargar.
static void test_overrides_survive_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto mesh = std::make_shared<SkinnedMesh>();
    mesh->sourcePath = "assets/hero.fbx";
    mesh->materials.resize(3);
    go->setMesh(std::move(mesh));

    MaterialTextureOverride a; a.index = 0; a.albedo = "assets/cuerpo.png";
    MaterialTextureOverride b; b.index = 2; b.albedo = "assets/pelo.png";
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
    // El baseline NO viaja: se recaptura al aplicar sobre el material recien
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
    // El nodo raiz cuelga de "root"; localizar el hijo por nombre en vez de
    // asumir el indice.
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

// Con raiz de proyecto fijada, una ruta bajo ella se guarda RELATIVA con "/".
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
    MaterialTextureOverride ov;
    ov.index  = 0;
    ov.albedo = (root / "assets" / "x.png").string();
    go->materialOverrides.push_back(ov);

    const std::string texto = scene.toJson().dump();
    CHECK(texto.find("assets/x.png") != std::string::npos);
    CHECK(texto.find(root.string()) == std::string::npos);

    // Y al leerla con la misma raiz vuelve absoluta.
    Scene cargada("Vacia");
    cargada.setAssetRoot(root.string());
    CHECK(cargada.fromJson(scene.toJson(), pm, am));
    GameObject* leido = nullptr;
    cargada.traverse([&](GameObject* n) { if (n->name == "Cubo") leido = n; });
    CHECK(leido != nullptr);
    if (leido && leido->materialOverrides.size() == 1)
        CHECK(fs::path(leido->materialOverrides[0].albedo) == (root / "assets" / "x.png"));
}

// Una ruta FUERA de la raiz se guarda absoluta tal cual: relativizarla daria
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
    MaterialTextureOverride ov;
    ov.index  = 0;
    ov.albedo = fuera.string();
    go->materialOverrides.push_back(ov);

    const std::string texto = scene.toJson().dump();
    CHECK(texto.find("y.png") != std::string::npos);
    CHECK(texto.find("..") == std::string::npos);
}

// Sin raiz fijada (tests, runtime headless), la ruta va y vuelve IDENTICA.
static void test_without_root_path_is_verbatim(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialTextureOverride ov;
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

// Una entrada sin "index" valido se descarta con aviso, sin tirar las demas
// del mismo array: mismo fichero editado a mano que solo corrompe una entrada.
static void test_materials_entry_without_valid_index_is_discarded(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    auto mesh = std::make_shared<Mesh>();
    mesh->sourcePath = "assets/cubo.fbx";
    go->setMesh(std::move(mesh));
    MaterialTextureOverride ov;
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
    // Se anade una segunda entrada sin "index": tiene que descartarse SOLA,
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

int main()
{
    // PhysicsManager/AudioManager comparten instancia entre los tests que la
    // necesitan: crear y destruir un PhysicsManager por test crashea al
    // segundo init (una PxFoundation por proceso), mismo patron que
    // audio_tests.cpp.
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    if (!am.init())
        std::printf("AVISO: FMOD no disponible; los tests que lo necesitan se saltaran\n");

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

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL MATERIAL TEXTURE TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
