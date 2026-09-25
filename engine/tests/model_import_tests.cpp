// Test headless de como ModelLoader lee los ajustes de importacion de un modelo
// (escala, normales, tangentes, UVs, animaciones). Sin GPU. Desde la raiz del repo:
// usa assets/modelAnimation.fbx, que el test copia a una carpeta temporal para
// poner el sidecar sin ensuciar assets/.
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Core/ImportSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static fs::path makeDir(const char* name)
{
    std::error_code ec;
    fs::path d = fs::temp_directory_path(ec) / name;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

static void writeText(const fs::path& p, const std::string& s)
{
    std::ofstream(p, std::ios::binary) << s;
}

static void writeSettings(const fs::path& asset, const ModelImportSettings& s)
{
    std::string err;
    CHECK(saveModelImportSettings(asset, s, &err));
}

// Dos triangulos plegados que comparten la arista p1-p3. T1 esta en el plano XY
// (normal +Z); T2 va inclinado (normal (1,-1,1)/sqrt3). En T1 la u crece a lo largo
// de +Y, asi que su tangente correcta es (0,1,0) y NO el fallback (1,0,0). El
// importador de OBJ desenrolla los vertices por cara: los indices 0-2 son T1
// (p1,p2,p3) y los 3-5 son T2 (p1,p3,p4).
static const char* kFoldedObj =
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 1\n"
    "vt 0 0\nvt 0 1\nvt 1 1\nvt 1 0\n"
    "f 1/1 2/2 3/3\n"
    "f 1/1 3/3 4/4\n";

// Lo mismo, pero el fichero TRAE normales (todas +Z, tambien las de T2, que son
// "mentira"): sirve para probar que smooth/flat las regeneran.
static const char* kFoldedObjWithNormals =
    "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 1\n"
    "vt 0 0\nvt 0 1\nvt 1 1\nvt 1 0\n"
    "vn 0 0 1\n"
    "f 1/1/1 2/2/1 3/3/1\n"
    "f 1/1/1 3/3/1 4/4/1\n";

static fs::path writeObj(const char* dirName, const char* text)
{
    const fs::path obj = makeDir(dirName) / "plegado.obj";
    writeText(obj, text);
    return obj;
}

static const Vertex* vertexAt(const Mesh& m, size_t tri, const glm::vec3& pos)
{
    for (size_t k = 0; k < 3; ++k)
    {
        const Vertex& v = m.vertices[m.indices[tri * 3 + k]];
        if (glm::length(v.pos - pos) < 1e-4f) return &v;
    }
    return nullptr;
}

static bool foldedShape(const Mesh& m)
{
    return m.vertices.size() == 6 && m.indices.size() == 6;
}

static glm::vec3 kFaceNormal2 = glm::normalize(glm::vec3(1.0f, -1.0f, 1.0f));

// Review Focus 4: sin sidecar, exactamente lo de antes.
static void test_defaults_match_the_old_flags()
{
    const fs::path obj = writeObj("dt_model_import_defaults", kFoldedObj);
    const Mesh m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m));
    if (!foldedShape(m)) return;

    const Vertex* p2 = vertexAt(m, 0, { 1, 0, 0 });
    CHECK(p2 != nullptr);
    if (!p2) return;
    CHECK(p2->uv.y == 0.0f);                                     // FlipUVs: v = 1 - 1
    CHECK(glm::length(p2->normal - glm::vec3(0, 0, 1)) < 1e-3f); // GenNormals: plana de T1
    CHECK(std::abs(p2->tangent.y) > 0.9f);                       // CalcTangentSpace corrio
    CHECK(vertexAt(m, 0, { 2, 0, 0 }) == nullptr);               // escala 1
}

static void test_scale_multiplies_positions()
{
    const fs::path obj = writeObj("dt_model_import_scale", kFoldedObj);
    ModelImportSettings s;
    s.scale = 2.0f;
    writeSettings(obj, s);
    const Mesh m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m));
    if (!foldedShape(m)) return;
    CHECK(vertexAt(m, 0, { 2, 0, 0 }) != nullptr);
    CHECK(vertexAt(m, 0, { 2, 2, 0 }) != nullptr);
    CHECK(vertexAt(m, 0, { 1, 0, 0 }) == nullptr);
}

static void test_flip_uvs_off_keeps_v()
{
    const fs::path obj = writeObj("dt_model_import_flip", kFoldedObj);
    ModelImportSettings s;
    s.flipUVs = false;
    writeSettings(obj, s);
    const Mesh m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m));
    if (!foldedShape(m)) return;
    const Vertex* p2 = vertexAt(m, 0, { 1, 0, 0 });
    CHECK(p2 != nullptr);
    if (p2) CHECK(p2->uv.y == 1.0f);
}

static void test_tangents_off_uses_the_fallback()
{
    const fs::path obj = writeObj("dt_model_import_tangents", kFoldedObj);
    ModelImportSettings s;
    s.calcTangents = false;
    writeSettings(obj, s);
    const Mesh m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m));
    for (const Vertex& v : m.vertices)
        CHECK(glm::length(v.tangent - glm::vec3(1, 0, 0)) < 1e-6f);
}

// Review Focus 5.
static void test_flat_normals_regenerate_even_when_the_file_has_them()
{
    const fs::path obj = writeObj("dt_model_import_flat", kFoldedObjWithNormals);

    // Por defecto (file) se respetan las del fichero: T2 sale con +Z.
    {
        const Mesh m = ModelLoader::load(obj.string());
        CHECK(foldedShape(m));
        if (!foldedShape(m)) return;
        const Vertex* v = vertexAt(m, 1, { 0, 1, 1 });
        CHECK(v != nullptr);
        if (v) CHECK(glm::length(v->normal - glm::vec3(0, 0, 1)) < 1e-3f);
    }

    ModelImportSettings s;
    s.normals = NormalsMode::Flat;
    writeSettings(obj, s);
    const Mesh m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m));
    if (!foldedShape(m)) return;
    const Vertex* t1 = vertexAt(m, 0, { 1, 0, 0 });
    const Vertex* t2 = vertexAt(m, 1, { 0, 1, 1 });
    CHECK(t1 != nullptr && t2 != nullptr);
    if (t1) CHECK(glm::length(t1->normal - glm::vec3(0, 0, 1)) < 1e-3f);
    if (t2) CHECK(glm::dot(t2->normal, kFaceNormal2) > 0.99f);   // regenerada, ya no +Z
}

static void test_smooth_normals_average_the_shared_vertices()
{
    const fs::path obj = writeObj("dt_model_import_smooth", kFoldedObj);

    ModelImportSettings s;
    s.normals = NormalsMode::Smooth;
    writeSettings(obj, s);
    const Mesh smooth = ModelLoader::load(obj.string());
    CHECK(foldedShape(smooth));
    if (!foldedShape(smooth)) return;

    // p1 esta en las dos caras: suave = misma normal en las dos copias, y ni
    // la de T1 (+Z) ni la de T2.
    const Vertex* a = vertexAt(smooth, 0, { 0, 0, 0 });
    const Vertex* b = vertexAt(smooth, 1, { 0, 0, 0 });
    CHECK(a != nullptr && b != nullptr);
    if (!a || !b) return;
    CHECK(glm::length(a->normal - b->normal) < 1e-3f);
    CHECK(glm::dot(a->normal, glm::vec3(0, 0, 1)) < 0.999f);
    CHECK(glm::dot(a->normal, kFaceNormal2) < 0.999f);

    // Planas: cada copia conserva la de SU cara.
    s.normals = NormalsMode::Flat;
    writeSettings(obj, s);
    const Mesh flat = ModelLoader::load(obj.string());
    CHECK(foldedShape(flat));
    if (!foldedShape(flat)) return;
    const Vertex* fa = vertexAt(flat, 0, { 0, 0, 0 });
    const Vertex* fb = vertexAt(flat, 1, { 0, 0, 0 });
    CHECK(fa != nullptr && fb != nullptr);
    if (fa && fb) CHECK(glm::length(fa->normal - fb->normal) > 0.1f);
}

// Review Focus 1: un sidecar hostil nunca tumba la carga.
static void test_hostile_sidecar_never_breaks_the_load()
{
    const fs::path obj = writeObj("dt_model_import_hostile", kFoldedObj);

    writeText(importSidecarPath(obj), "{ esto no es json");
    Mesh m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m) && vertexAt(m, 0, { 1, 0, 0 }) != nullptr);

    writeText(importSidecarPath(obj), R"({"version":1,"type":"model","scale":0})");
    m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m) && vertexAt(m, 0, { 1, 0, 0 }) != nullptr);   // 0 -> 1, no colapsa

    writeText(importSidecarPath(obj), R"({"version":1,"type":"model","scale":1e30})");
    m = ModelLoader::load(obj.string());
    CHECK(foldedShape(m) && vertexAt(m, 0, { 1000, 0, 0 }) != nullptr);   // acotado a 1000
}

static void test_missing_model_still_throws()
{
    bool threw = false;
    try { (void)ModelLoader::load("no_existe_dt_model.obj"); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

// ── Skinned: el personaje de prueba del repo, copiado a una carpeta temporal ──

static fs::path copyCharacter(const char* dirName)
{
    const fs::path dst = makeDir(dirName) / "char.fbx";
    std::error_code ec;
    fs::copy_file("assets/modelAnimation.fbx", dst, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    return dst;
}

static float maxAbsCoord(const SkinnedMesh& m)
{
    float r = 0.0f;
    for (const SkinnedVertex& v : m.skinnedVertices)
        r = std::max({ r, std::abs(v.position.x), std::abs(v.position.y), std::abs(v.position.z) });
    return r;
}

// |b - 2a| dentro de una tolerancia relativa: la razon, no un valor absoluto.
static bool doubled(float a, float b)
{
    return std::abs(b - 2.0f * a) <= 1e-3f * std::max(1.0f, std::abs(a));
}

static const BoneKeyframe* firstPosKey(const std::vector<AnimationClip>& clips)
{
    for (const AnimationClip& c : clips)
        for (const BoneChannel& ch : c.channels)
            if (!ch.posKeys.empty()) return &ch.posKeys[0];
    return nullptr;
}

// Riesgo del spec: GlobalScale frente a las unidades propias del importador FBX.
// Se comprueba la RAZON con y sin ajuste sobre el FBX real. Si esto falla no se
// debilita el test: es el hallazgo que el spec dejo anotado.
static void test_skinned_scale_scales_geometry_bones_and_clips()
{
    const fs::path fbx = copyCharacter("dt_model_import_skinned_scale");
    const SkinnedMesh base = ModelLoader::loadSkinned(fbx.string());
    CHECK(!base.skinnedVertices.empty());
    if (base.skinnedVertices.empty()) return;

    ModelImportSettings s;
    s.scale = 2.0f;
    writeSettings(fbx, s);
    const SkinnedMesh scaled = ModelLoader::loadSkinned(fbx.string());
    CHECK(scaled.skinnedVertices.size() == base.skinnedVertices.size());

    CHECK(doubled(maxAbsCoord(base), maxAbsCoord(scaled)));

    // Un hueso cuyo offset tenga traslacion apreciable: se duplica.
    bool checkedBone = false;
    for (size_t i = 0; i < base.skeleton.inverseBindPose.size() &&
                       i < scaled.skeleton.inverseBindPose.size(); ++i)
    {
        const glm::vec3 t0(base.skeleton.inverseBindPose[i][3]);
        if (glm::length(t0) < 1e-3f) continue;
        const glm::vec3 t1(scaled.skeleton.inverseBindPose[i][3]);
        CHECK(doubled(t0.x, t1.x) && doubled(t0.y, t1.y) && doubled(t0.z, t1.z));
        checkedBone = true;
        break;
    }
    CHECK(checkedBone);

    // La primera clave de traslacion de un clip: se duplica.
    const BoneKeyframe* k0 = firstPosKey(base.animationClips);
    const BoneKeyframe* k1 = firstPosKey(scaled.animationClips);
    CHECK(k0 != nullptr && k1 != nullptr);
    if (k0 && k1) CHECK(doubled(k0->value.x, k1->value.x) && doubled(k0->value.y, k1->value.y) &&
                        doubled(k0->value.z, k1->value.z));
}

static void test_import_animations_off_leaves_the_builtin_source_empty()
{
    const fs::path fbx = copyCharacter("dt_model_import_anim_off");
    const SkinnedMesh base = ModelLoader::loadSkinned(fbx.string());
    CHECK(!base.animationClips.empty());          // precondicion: el fixture trae clips

    ModelImportSettings s;
    s.importAnimations = false;
    writeSettings(fbx, s);
    const SkinnedMesh off = ModelLoader::loadSkinned(fbx.string());
    CHECK(off.animationClips.empty());
    CHECK(off.animationSources.size() == 1);
    if (off.animationSources.size() == 1)
    {
        CHECK(off.animationSources[0].builtin);
        CHECK(off.animationSources[0].clipNames.empty());
    }
    CHECK(off.skinnedVertices.size() == base.skinnedVertices.size());   // la malla sigue
}

// Cada FBX usa SU sidecar: los clips de una fuente externa se escalan con el del
// propio fichero de animacion.
static void test_animation_source_uses_its_own_scale()
{
    const fs::path fbx = copyCharacter("dt_model_import_clip_scale");
    const SkinnedMesh base = ModelLoader::loadSkinned(fbx.string());
    const LoadedClips c0 = ModelLoader::loadAnimationClips(fbx.string(), base.skeleton);
    const BoneKeyframe* k0 = firstPosKey(c0.clips);
    CHECK(k0 != nullptr);
    if (!k0) return;
    const BoneKeyframe copy0 = *k0;

    ModelImportSettings s;
    s.scale = 2.0f;
    writeSettings(fbx, s);
    const LoadedClips c1 = ModelLoader::loadAnimationClips(fbx.string(), base.skeleton);
    const BoneKeyframe* k1 = firstPosKey(c1.clips);
    CHECK(k1 != nullptr);
    if (k1) CHECK(doubled(copy0.value.x, k1->value.x) && doubled(copy0.value.y, k1->value.y) &&
                  doubled(copy0.value.z, k1->value.z));
}

int main()
{
    test_defaults_match_the_old_flags();
    test_scale_multiplies_positions();
    test_flip_uvs_off_keeps_v();
    test_tangents_off_uses_the_fallback();
    test_flat_normals_regenerate_even_when_the_file_has_them();
    test_smooth_normals_average_the_shared_vertices();
    test_hostile_sidecar_never_breaks_the_load();
    test_missing_model_still_throws();
    test_skinned_scale_scales_geometry_bones_and_clips();
    test_import_animations_off_leaves_the_builtin_source_empty();
    test_animation_source_uses_its_own_scale();

    if (g_failures == 0) std::printf("ALL MODEL IMPORT TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
