// Test headless de los overrides de textura del Mesh (sin GPU). Plain main +
// asserts, sin framework — mismo patrón que content_browser_tests.cpp.
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"

#include <cstdio>
#include <memory>
#include <string>

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

// Un GameObject sin mesh no revienta.
static void test_no_mesh_is_noop()
{
    GameObject go("Vacio");
    go.materialOverrides.push_back(MaterialTextureOverride{});
    applyMaterialOverrides(go);
    CHECK(materialsOfMesh(go).empty());
}

int main()
{
    test_materials_of_static_mesh();
    test_materials_of_skinned_mesh();
    test_override_writes_material_and_captures_baseline();
    test_second_override_keeps_original_baseline();
    test_clear_restores_baseline();
    test_skinned_override_touches_only_its_index();
    test_out_of_range_index_is_ignored();
    test_slots_are_independent();
    test_no_mesh_is_noop();
    if (g_failures == 0) std::printf("ALL MATERIAL TEXTURE TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
