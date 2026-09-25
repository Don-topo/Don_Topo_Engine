// Test headless de como ModelLoader lee los ajustes de importacion de un modelo
// (escala, normales, tangentes, UVs, animaciones). Sin GPU. Desde la raiz del repo:
// usa assets/modelAnimation.fbx, que el test copia a una carpeta temporal para
// poner el sidecar sin ensuciar assets/.
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Core/ImportSettings.h"
#include "gltf_fixtures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glm/gtc/matrix_transform.hpp>
#include <memory>
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

// ── Preview para las miniaturas del Content Browser ──────────────────────────

using Rgba = std::array<uint8_t, 4>;

// TGA sin comprimir de 32 bits, origen arriba a la izquierda.
static void writeTga(const fs::path& p, int w, int h, const std::function<Rgba(int, int)>& pixel)
{
    std::ofstream f(p, std::ios::binary);
    uint8_t hdr[18] = {};
    hdr[2]  = 2;
    hdr[12] = static_cast<uint8_t>(w & 0xFF);
    hdr[13] = static_cast<uint8_t>((w >> 8) & 0xFF);
    hdr[14] = static_cast<uint8_t>(h & 0xFF);
    hdr[15] = static_cast<uint8_t>((h >> 8) & 0xFF);
    hdr[16] = 32;
    hdr[17] = 0x28;
    f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const Rgba c = pixel(x, y);
            const uint8_t bgra[4] = { c[2], c[1], c[0], c[3] };
            f.write(reinterpret_cast<const char*>(bgra), 4);
        }
}

static size_t previewTriangles(const ModelPreview& p)
{
    size_t n = 0;
    for (const PreviewPart& part : p.parts) n += part.indices.size() / 3;
    return n;
}

static bool hasDependency(const ModelPreview& p, const fs::path& dep)
{
    for (const FileStamp& d : p.dependencies)
        if (d.stamped && sameAssetPath(d.path, dep)) return true;   // selladas, no solo nombradas
    return false;
}

// El preview pinta lo mismo que el motor: mismo numero de triangulos que loadAuto,
// en un modelo estatico con textura y en un personaje con varias submallas.
static void test_preview_matches_the_engine_triangle_count()
{
    for (const char* file : { "assets/modelTexture.fbx", "assets/modelAnimation.fbx" })
    {
        const std::shared_ptr<Mesh> engine = ModelLoader::loadAuto(file);
        const ModelPreview preview = ModelLoader::loadPreview(file);
        CHECK(preview.status == PreviewStatus::Ok);
        CHECK(engine && !engine->indices.empty());
        if (!engine) continue;
        CHECK(previewTriangles(preview) == engine->indices.size() / 3);
    }
    // El personaje trae varias submallas: una parte por cada una.
    const ModelPreview character = ModelLoader::loadPreview("assets/modelAnimation.fbx");
    CHECK(character.parts.size() > 1);

    // Estatico con DOS mallas (dos grupos de OBJ, sin huesos): load solo pinta la
    // primera, y el preview tambien. Ningun asset del repo cubre este caso.
    const fs::path dir = makeDir("dt_model_preview_two_meshes");
    writeText(dir / "dos.obj",
              "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 5 0 0\nv 6 0 0\nv 5 1 0\n"
              "g a\nf 1 2 3\n"
              "g b\nf 4 5 6\n");
    const std::shared_ptr<Mesh> twoEngine = ModelLoader::loadAuto((dir / "dos.obj").string());
    const ModelPreview twoPreview = ModelLoader::loadPreview((dir / "dos.obj").string());
    CHECK(twoEngine && twoEngine->indices.size() == 3);      // precondicion: el motor pinta una
    if (twoEngine) CHECK(previewTriangles(twoPreview) == twoEngine->indices.size() / 3);
}

// La textura embebida se reduce: lado mayor <= 256.
static void test_preview_texture_is_downscaled()
{
    const ModelPreview p = ModelLoader::loadPreview("assets/modelTexture.fbx");
    bool textured = false;
    for (const PreviewPart& part : p.parts)
    {
        if (part.albedo.rgba.empty()) continue;
        textured = true;
        CHECK(std::max(part.albedo.w, part.albedo.h) <= kPreviewMaxTexture);
        CHECK(part.albedo.rgba.size() == static_cast<size_t>(part.albedo.w) * part.albedo.h * 4);
    }
    CHECK(textured);   // precondicion: el fixture trae textura
}

// Un FBX que solo trae animacion (Mixamo "without skin") no es un fallo.
static void test_preview_animation_only_file()
{
    const ModelPreview p = ModelLoader::loadPreview("assets/animatedCharacter/standing idle 01.fbx");
    CHECK(p.status == PreviewStatus::AnimationOnly);
    CHECK(p.parts.empty());
}

static void test_preview_garbage_and_missing_are_unreadable()
{
    const fs::path dir = makeDir("dt_model_preview_garbage");
    writeText(dir / "basura.fbx", "esto no es un fbx");
    CHECK(ModelLoader::loadPreview((dir / "basura.fbx").string()).status == PreviewStatus::Unreadable);
    CHECK(ModelLoader::loadPreview((dir / "no_existe.obj").string()).status == PreviewStatus::Unreadable);
}

// Textura externa: se lee, se reduce conservando la proporcion y es una dependencia.
// El sidecar tambien lo es aunque todavia no exista.
static void test_preview_external_texture_and_sidecar_are_dependencies()
{
    const fs::path dir = makeDir("dt_model_preview_external");
    writeTga(dir / "rojo.tga", 512, 128, [](int, int) { return Rgba{ 255, 0, 0, 255 }; });
    writeText(dir / "quad.mtl", "newmtl m\nmap_Kd rojo.tga\n");
    writeText(dir / "quad.obj",
              "mtllib quad.mtl\nusemtl m\n"
              "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
              "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
              "f 1/1 2/2 3/3\nf 1/1 3/3 4/4\n");
    const fs::path obj = dir / "quad.obj";
    const ModelPreview p = ModelLoader::loadPreview(obj.string());
    CHECK(p.status == PreviewStatus::Ok);
    CHECK(p.parts.size() == 1);
    if (p.parts.size() != 1) return;
    const PreviewImage& img = p.parts[0].albedo;
    CHECK(img.w == 256 && img.h == 64);
    if (!img.rgba.empty()) CHECK(img.rgba[0] == 255 && img.rgba[1] == 0 && img.rgba[2] == 0);
    CHECK(hasDependency(p, dir / "rojo.tga"));
    CHECK(hasDependency(p, importSidecarPath(obj)));
    // Revision final, Important 2: cambiar map_Kd en el .mtl no toca el .obj; sin
    // esto la miniatura no se regeneraria nunca.
    CHECK(hasDependency(p, dir / "quad.mtl"));
}

// El sidecar cambia el aspecto: normals = flat regenera las normales del fichero.
static void test_preview_respects_the_normals_setting()
{
    const fs::path obj = writeObj("dt_model_preview_normals", kFoldedObjWithNormals);
    auto hasTiltedNormal = [](const ModelPreview& p) {
        for (const PreviewPart& part : p.parts)
            for (const glm::vec3& n : part.normals)
                if (glm::length(n - kFaceNormal2) < 1e-3f) return true;
        return false;
    };
    CHECK(!hasTiltedNormal(ModelLoader::loadPreview(obj.string())));   // las del fichero: todas +Z
    ModelImportSettings s;
    s.normals = NormalsMode::Flat;
    writeSettings(obj, s);
    CHECK(hasTiltedNormal(ModelLoader::loadPreview(obj.string())));
}

// Una textura enorme se rechaza por su CABECERA: 65535 x 65535 sin cuerpo.
static void test_preview_image_rejects_huge_sources()
{
    const fs::path dir = makeDir("dt_model_preview_huge");
    {
        std::ofstream f(dir / "huge.tga", std::ios::binary);
        uint8_t hdr[18] = {};
        hdr[2] = 2; hdr[12] = 0xFF; hdr[13] = 0xFF; hdr[14] = 0xFF; hdr[15] = 0xFF; hdr[16] = 32; hdr[17] = 0x28;
        f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    }
    CHECK(ModelLoader::loadPreviewImage(dir / "huge.tga").rgba.empty());
    CHECK(ModelLoader::loadPreviewImage(dir / "no_existe.png").rgba.empty());
}

// ── Formatos, texturas en subcarpeta y ficheros asociados ────────────────────

static void test_supported_model_extensions()
{
    for (const char* e : { ".fbx", ".FBX", ".obj", ".gltf", ".GLB", ".glb" })
        CHECK(ModelLoader::isSupportedModelExtension(e));
    for (const char* e : { ".dae", ".blend", ".png", "", "fbx" })
        CHECK(!ModelLoader::isSupportedModelExtension(e));
    const std::string filter = ModelLoader::supportedModelFilter();
    for (const char* e : { ".fbx", ".obj", ".gltf", ".glb" })
        CHECK(filter.find(e) != std::string::npos);
}

static void test_resolve_texture_prefers_the_subfolder()
{
    const fs::path dir = makeDir("dt_resolve_sub");
    fs::create_directories(dir / "textures");
    writeText(dir / "textures" / "x.tga", "sub");
    writeText(dir / "x.tga", "root");                         // homonima junto al modelo
    CHECK(sameAssetPath(ModelLoader::resolveModelTexture(dir, "textures/x.tga"), dir / "textures" / "x.tga"));
}

// Review Focus 3: lo de siempre (nombre suelto) sigue funcionando.
static void test_resolve_texture_falls_back_to_the_bare_name()
{
    const fs::path dir = makeDir("dt_resolve_bare");
    writeText(dir / "x.tga", "root");
    CHECK(sameAssetPath(ModelLoader::resolveModelTexture(dir, "textures/x.tga"), dir / "x.tga"));
    CHECK(sameAssetPath(ModelLoader::resolveModelTexture(dir, "x.tga"), dir / "x.tga"));
    CHECK(sameAssetPath(ModelLoader::resolveModelTexture(dir, "../fuera/x.tga"), dir / "x.tga"));
    CHECK(sameAssetPath(ModelLoader::resolveModelTexture(dir, "C:/artista/x.tga"), dir / "x.tga"));
    CHECK(sameAssetPath(ModelLoader::resolveModelTexture(dir, "/home/artista/x.tga"), dir / "x.tga"));
}

static void test_companions_of_obj()
{
    const fs::path dir = makeDir("dt_companions_obj");
    writeText(dir / "m.obj", "mtllib a.mtl\n# comentario\nmtllib   sub/b.mtl  \r\nmtllib a.mtl\nv 0 0 0\n");
    const std::vector<std::string> c = ModelLoader::modelCompanionFiles((dir / "m.obj").string());
    CHECK(c.size() == 2);
    if (c.size() == 2) { CHECK(c[0] == "a.mtl"); CHECK(c[1] == "sub/b.mtl"); }
}

// Review Focus 5: %20 se decodifica; data:, absolutas y .. se ignoran.
static void test_companions_of_gltf()
{
    const fs::path dir = makeDir("dt_companions_gltf");
    writeText(dir / "m.gltf", R"({"asset":{"version":"2.0"},
        "buffers":[{"uri":"tri.bin","byteLength":4},{"uri":"data:application/octet-stream;base64,AAAA","byteLength":3}],
        "images":[{"uri":"textures/rojo%20x.tga"},{"uri":"../fuera.png"},{"uri":"/abs.png"},{"uri":"C:/abs.png"},{"uri":"tri.bin"}]})");
    const std::vector<std::string> c = ModelLoader::modelCompanionFiles((dir / "m.gltf").string());
    CHECK(c.size() == 2);
    if (c.size() == 2) { CHECK(c[0] == "tri.bin"); CHECK(c[1] == "textures/rojo x.tga"); }
}

static void test_companions_of_other_formats_are_empty()
{
    const fs::path dir = makeDir("dt_companions_other");
    writeText(dir / "m.glb", "glTF");
    writeText(dir / "roto.gltf", "{ esto no es json");
    CHECK(ModelLoader::modelCompanionFiles((dir / "m.glb").string()).empty());
    CHECK(ModelLoader::modelCompanionFiles("assets/modelTexture.fbx").empty());
    CHECK(ModelLoader::modelCompanionFiles((dir / "roto.gltf").string()).empty());
    CHECK(ModelLoader::modelCompanionFiles((dir / "no_existe.obj").string()).empty());
}

// ── glTF ─────────────────────────────────────────────────────────────────────

static std::string base64(const std::vector<uint8_t>& in)
{
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3)
    {
        uint32_t n = static_cast<uint32_t>(in[i]) << 16;
        if (i + 1 < in.size()) n |= static_cast<uint32_t>(in[i + 1]) << 8;
        if (i + 2 < in.size()) n |= in[i + 2];
        out += T[(n >> 18) & 63];
        out += T[(n >> 12) & 63];
        out += i + 1 < in.size() ? T[(n >> 6) & 63] : '=';
        out += i + 2 < in.size() ? T[n & 63] : '=';
    }
    return out;
}

// Un triangulo: 3 posiciones (36 bytes) + 3 UV (24 bytes) = 60 bytes.
static std::vector<uint8_t> triangleBuffer()
{
    const float data[15] = { 0, 0, 0,  1, 0, 0,  0, 1, 0,   0, 0,  1, 0,  0, 1 };
    std::vector<uint8_t> b(sizeof(data));
    std::memcpy(b.data(), data, sizeof(data));
    return b;
}

// bufferUri vacio = sin "uri" (el buffer va en el chunk BIN de un .glb).
static std::string triangleGltfJson(const std::string& bufferUri, const std::string& imageUri)
{
    std::string j = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
                    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1})";
    if (!imageUri.empty()) j += R"(,"material":0)";
    j += R"(}]}],)";
    if (!imageUri.empty())
        j += R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}}}],)"
             R"("textures":[{"source":0}],"images":[{"uri":")" + imageUri + R"("}],)";
    j += R"("buffers":[{)";
    if (!bufferUri.empty()) j += R"("uri":")" + bufferUri + R"(",)";
    j += R"("byteLength":60}],)"
         R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],)"
         R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
         R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}]})";
    return j;
}

static void writeGlb(const fs::path& p)
{
    std::string json = triangleGltfJson("", "");
    while (json.size() % 4) json += ' ';
    const std::vector<uint8_t> bin = triangleBuffer();          // 60: ya multiplo de 4
    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    u32(0x46546C67); u32(2); u32(static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
    u32(static_cast<uint32_t>(json.size())); u32(0x4E4F534A); f.write(json.data(), static_cast<std::streamsize>(json.size()));
    u32(static_cast<uint32_t>(bin.size()));  u32(0x004E4942); f.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
}

static bool hasUvX1(const Mesh& m)
{
    for (const Vertex& v : m.vertices) if (std::abs(v.uv.x - 1.0f) < 1e-4f) return true;
    return false;
}

static void test_gltf_with_embedded_buffer_loads()
{
    const fs::path dir = makeDir("dt_gltf_embedded");
    writeText(dir / "tri.gltf", triangleGltfJson("data:application/octet-stream;base64," + base64(triangleBuffer()), ""));
    try
    {
        const Mesh m = ModelLoader::load((dir / "tri.gltf").string());
        CHECK(m.indices.size() == 3);
        CHECK(hasUvX1(m));
    }
    catch (const std::exception& e) { std::printf("  %s\n", e.what()); CHECK(false); }
}

// Buffer externo y textura en subcarpeta: la textura se resuelve a textures/.
static void test_gltf_with_external_bin_and_subfolder_texture()
{
    const fs::path dir = makeDir("dt_gltf_external");
    fs::create_directories(dir / "textures");
    const std::vector<uint8_t> bin = triangleBuffer();
    std::ofstream(dir / "tri.bin", std::ios::binary).write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
    writeTga(dir / "textures" / "rojo.tga", 4, 4, [](int, int) { return Rgba{ 255, 0, 0, 255 }; });
    writeText(dir / "tri.gltf", triangleGltfJson("tri.bin", "textures/rojo.tga"));
    try
    {
        const Mesh m = ModelLoader::load((dir / "tri.gltf").string());
        CHECK(m.indices.size() == 3);
        CHECK(!m.material.texturePath.empty());
        if (!m.material.texturePath.empty())
            CHECK(sameAssetPath(m.material.texturePath, dir / "textures" / "rojo.tga"));
    }
    catch (const std::exception& e) { std::printf("  %s\n", e.what()); CHECK(false); }

    const ModelPreview p = ModelLoader::loadPreview((dir / "tri.gltf").string());
    CHECK(p.status == PreviewStatus::Ok);
    CHECK(hasDependency(p, dir / "tri.bin"));
    CHECK(hasDependency(p, dir / "textures" / "rojo.tga"));
    CHECK(p.parts.size() == 1 && !p.parts[0].albedo.rgba.empty());
}

static void test_glb_loads()
{
    const fs::path dir = makeDir("dt_glb");
    writeGlb(dir / "tri.glb");
    try
    {
        const std::shared_ptr<Mesh> m = ModelLoader::loadAuto((dir / "tri.glb").string());
        CHECK(m && m->indices.size() == 3);
    }
    catch (const std::exception& e) { std::printf("  %s\n", e.what()); CHECK(false); }
}

// Review Focus 1: sin su .bin, error limpio (no crash) y preview Unreadable.
static void test_gltf_missing_bin_fails_cleanly()
{
    const fs::path dir = makeDir("dt_gltf_missing_bin");
    writeText(dir / "tri.gltf", triangleGltfJson("no_esta.bin", ""));
    bool threw = false;
    try { (void)ModelLoader::load((dir / "tri.gltf").string()); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    const ModelPreview p = ModelLoader::loadPreview((dir / "tri.gltf").string());
    CHECK(p.status == PreviewStatus::Unreadable);
    CHECK(hasDependency(p, dir / "no_esta.bin"));      // vigilado: se regenera cuando aparezca
}

// Revision final, Critical 1: las texturas que nombra el .mtl tambien son
// asociados del .obj, con su ruta tal cual (el loader las resuelve respecto a la
// carpeta del modelo). La opcion antes del nombre (-o, -s...) no cuenta.
static void test_companions_of_obj_include_the_mtl_textures()
{
    const fs::path dir = makeDir("dt_companions_obj_tex");
    fs::create_directories(dir / "mats");
    writeText(dir / "m.obj", "mtllib mats/a.mtl\nv 0 0 0\n");
    writeText(dir / "mats" / "a.mtl", "newmtl x\nmap_Kd -o 0 0 0 tex/c.png\nmap_Bump n.png\nbump n.png\nKd 1 1 1\n");
    const std::vector<std::string> c = ModelLoader::modelCompanionFiles((dir / "m.obj").string());
    CHECK(c.size() == 3);
    if (c.size() == 3) { CHECK(c[0] == "mats/a.mtl"); CHECK(c[1] == "tex/c.png"); CHECK(c[2] == "n.png"); }
}

// Revision final, Important 2: Assimp NO decodifica %20 en la URI de la imagen;
// el loader tiene que encontrar "rojo x.tga" igual.
static void test_gltf_texture_uri_with_spaces_loads()
{
    const fs::path dir = makeDir("dt_gltf_spaces");
    fs::create_directories(dir / "textures");
    const std::vector<uint8_t> bin = triangleBuffer();
    std::ofstream(dir / "tri.bin", std::ios::binary).write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
    writeTga(dir / "textures" / "rojo x.tga", 4, 4, [](int, int) { return Rgba{ 255, 0, 0, 255 }; });
    writeText(dir / "tri.gltf", triangleGltfJson("tri.bin", "textures/rojo%20x.tga"));
    CHECK(sameAssetPath(ModelLoader::resolveModelTexture(dir, "textures/rojo%20x.tga"), dir / "textures" / "rojo x.tga"));
    try
    {
        const Mesh m = ModelLoader::load((dir / "tri.gltf").string());
        CHECK(sameAssetPath(m.material.texturePath, dir / "textures" / "rojo x.tga"));
    }
    catch (const std::exception& e) { std::printf("  %s\n", e.what()); CHECK(false); }
}

// ── Piezas de un modelo estatico ─────────────────────────────────────────────

static bool nearMat(const glm::mat4& a, const glm::mat4& b)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > 1e-4f) return false;
    return true;
}

static bool hasVertex(const Mesh& m, const glm::vec3& p)
{
    for (const Vertex& v : m.vertices) if (glm::length(v.pos - p) < 1e-4f) return true;
    return false;
}

static void test_load_static_lists_every_piece()
{
    const fs::path dir = makeDir("dt_pieces_three");
    dt_fixture::writeThreePieceGltf(dir / "casa.gltf");
    try
    {
        const StaticModel m = ModelLoader::loadStatic((dir / "casa.gltf").string());
        CHECK(m.meshes.size() == 2);
        CHECK(m.pieces.size() == 3);
        if (m.meshes.size() == 2) { CHECK(m.meshes[0].piece == 0); CHECK(m.meshes[1].piece == 1); }
        if (m.pieces.size() != 3) return;
        CHECK(m.pieces[0].piece == 0 && m.pieces[0].name == "A");
        CHECK(m.pieces[1].piece == 1 && m.pieces[1].name == "B");
        CHECK(m.pieces[2].piece == 0 && m.pieces[2].name == "C");   // malla reutilizada
        CHECK(nearMat(m.pieces[0].transform, glm::translate(glm::mat4(1.0f), glm::vec3(5, 0, 0))));
        CHECK(nearMat(m.pieces[1].transform, glm::scale(glm::mat4(1.0f), glm::vec3(2.0f))));
        CHECK(nearMat(m.pieces[2].transform, glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -3))));
    }
    catch (const std::exception& e) { std::printf("  %s\n", e.what()); CHECK(false); }
}

static void test_load_piece_gives_that_mesh()
{
    const fs::path dir = makeDir("dt_pieces_load");
    dt_fixture::writeThreePieceGltf(dir / "casa.gltf");
    try
    {
        const Mesh b = ModelLoader::load((dir / "casa.gltf").string(), 1);
        CHECK(b.piece == 1);
        CHECK(hasVertex(b, { 0, 0, 1 }) && !hasVertex(b, { 1, 0, 0 }));   // triangulo en YZ, sin transformar
        const Mesh a0 = ModelLoader::load((dir / "casa.gltf").string());
        const Mesh a1 = ModelLoader::load((dir / "casa.gltf").string(), 0);
        CHECK(a0.piece == 0 && a0.vertices.size() == a1.vertices.size() && hasVertex(a0, { 1, 0, 0 }));
    }
    catch (const std::exception& e) { std::printf("  %s\n", e.what()); CHECK(false); }

    bool threw = false;
    try { (void)ModelLoader::load((dir / "casa.gltf").string(), 7); }
    catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

// Review Focus 5: una sola malla = una sola pieza (y el editor no crea hijos).
static void test_single_mesh_file_is_one_piece()
{
    const fs::path obj = writeObj("dt_pieces_single", kFoldedObj);
    const StaticModel m = ModelLoader::loadStatic(obj.string());
    CHECK(m.meshes.size() == 1);
    CHECK(m.pieces.size() == 1);
}

// La escala del sidecar lleva las piezas juntas: tambien multiplica la traslacion.
static void test_sidecar_scale_moves_the_pieces_too()
{
    const fs::path dir = makeDir("dt_pieces_scale");
    const fs::path gltf = dir / "casa.gltf";
    dt_fixture::writeThreePieceGltf(gltf);
    ModelImportSettings s;
    s.scale = 2.0f;
    writeSettings(gltf, s);
    const StaticModel m = ModelLoader::loadStatic(gltf.string());
    CHECK(m.pieces.size() == 3);
    if (m.pieces.size() == 3) CHECK(std::abs(m.pieces[0].transform[3].x - 10.0f) < 1e-4f);
    if (!m.meshes.empty()) CHECK(hasVertex(m.meshes[0], { 2, 0, 0 }));
}

// Review de la Task 1: hasTriangles nunca se ejercitaba en su rama false. Una
// malla sin ningun triangulo (primitivo LINES) no debe generar pieza, aunque
// SI aparezca en meshes (loadStatic construye una Mesh por cada malla del
// fichero, con triangulos o sin ellos).
static void test_lineless_mesh_produces_no_piece()
{
    const fs::path dir = makeDir("dt_pieces_no_triangles");
    dt_fixture::writeMixedTriangleAndLineGltf(dir / "mix.gltf");
    try
    {
        const StaticModel m = ModelLoader::loadStatic((dir / "mix.gltf").string());
        CHECK(m.meshes.size() == 2);
        CHECK(m.pieces.size() == 1);
        if (m.pieces.size() == 1) CHECK(m.pieces[0].name == "T");
    }
    catch (const std::exception& e) { std::printf("  %s\n", e.what()); CHECK(false); }
}

// Review de la Task 1: con la raiz en identidad, un solo nivel de nodos no
// distingue "padre * hijo" de "hijo * padre". Con dos niveles reales (A
// trasladado, B escalado, colgando de A) la traslacion resultante SOLO
// coincide con A*B si la composicion es la correcta.
static void test_nested_node_transform_composes_parent_then_child()
{
    const fs::path dir = makeDir("dt_pieces_nested");
    dt_fixture::writeNestedNodeGltf(dir / "nested.gltf");
    try
    {
        const StaticModel m = ModelLoader::loadStatic((dir / "nested.gltf").string());
        CHECK(m.pieces.size() == 1);
        if (m.pieces.size() != 1) return;
        const glm::mat4 expected = glm::translate(glm::mat4(1.0f), glm::vec3(5, 0, 0)) *
                                    glm::scale(glm::mat4(1.0f), glm::vec3(2.0f));
        CHECK(nearMat(m.pieces[0].transform, expected));
    }
    catch (const std::exception& e) { std::printf("  %s\n", e.what()); CHECK(false); }
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
    test_preview_matches_the_engine_triangle_count();
    test_preview_texture_is_downscaled();
    test_preview_animation_only_file();
    test_preview_garbage_and_missing_are_unreadable();
    test_preview_external_texture_and_sidecar_are_dependencies();
    test_preview_respects_the_normals_setting();
    test_preview_image_rejects_huge_sources();
    test_supported_model_extensions();
    test_resolve_texture_prefers_the_subfolder();
    test_resolve_texture_falls_back_to_the_bare_name();
    test_companions_of_obj();
    test_companions_of_gltf();
    test_companions_of_other_formats_are_empty();
    test_gltf_with_embedded_buffer_loads();
    test_gltf_with_external_bin_and_subfolder_texture();
    test_glb_loads();
    test_gltf_missing_bin_fails_cleanly();
    test_companions_of_obj_include_the_mtl_textures();
    test_gltf_texture_uri_with_spaces_loads();
    test_load_static_lists_every_piece();
    test_load_piece_gives_that_mesh();
    test_single_mesh_file_is_one_piece();
    test_sidecar_scale_moves_the_pieces_too();
    test_lineless_mesh_produces_no_piece();
    test_nested_node_transform_composes_parent_then_child();

    if (g_failures == 0) std::printf("ALL MODEL IMPORT TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
