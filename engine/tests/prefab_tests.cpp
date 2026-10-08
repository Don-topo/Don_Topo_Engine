// Tests of the prefab system (spec docs/superpowers/specs/2026-09-30-prefabs-design.md).
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/Core/MaterialAsset.h"
#include "DonTopo/Core/PrefabOverrides.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Renderer/Material.h"
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Core/Platform.h"
#include "DonTopo/Core/PrefabLibrary.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Renderer/Cube.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Scripting/ScriptBindings.h"
#include "DonTopo/Scripting/ScriptComponent.h"
#include "DonTopo/Scripting/ScriptManager.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <set>
#include <thread>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

namespace {
int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
    ++g_failures; } } while (0)

// A single PhysicsManager for the whole file: PxFoundation is unique per process.
DonTopo::PhysicsManager& physics()
{
    static DonTopo::PhysicsManager p;
    static bool inited = (p.init(), true);
    (void)inited;
    return p;
}

DonTopo::AudioManager& audio()
{
    static DonTopo::AudioManager a;
    static bool inited = (a.init(), true);
    (void)inited;
    return a;
}

nlohmann::json mat4Json(const glm::mat4& m)
{
    nlohmann::json a = nlohmann::json::array();
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) a.push_back(m[c][r]);
    return a;
}

nlohmann::json lightNode(uint64_t id, float intensity, const glm::mat4& t)
{
    return { {"id", id}, {"name", "Lamp"}, {"localTransform", mat4Json(t)},
             {"light", { {"type", "point"}, {"intensity", intensity}, {"range", 10.0} }},
             {"children", nlohmann::json::array()} };
}

void testDiffDetectsLeafChange()
{
    auto p = lightNode(2, 1.0f, glm::mat4(1.0f));
    auto i = lightNode(900, 3.5f, glm::mat4(1.0f));
    auto d = DonTopo::diffPrefabNode(2, p, i, false);
    CHECK(d.size() == 1, "exactly one override");
    CHECK(!d.empty() && d[0].node == 2 && d[0].path == "/light/intensity", "path of the override");
    CHECK(!d.empty() && d[0].value.get<double>() == 3.5, "value of the override");
}

void testDiffIgnoresIdsAndRootPose()
{
    auto p = lightNode(1, 1.0f, glm::mat4(1.0f));
    glm::mat4 moved = glm::translate(glm::mat4(1.0f), {5, 0, 0}) *
                      glm::rotate(glm::mat4(1.0f), 1.0f, {0, 1, 0});
    auto i = lightNode(812, 1.0f, moved);
    CHECK(DonTopo::diffPrefabNode(1, p, i, /*isInstanceRoot=*/true).empty(),
          "root position/rotation and id are never overrides");
    auto d = DonTopo::diffPrefabNode(1, p, i, /*isInstanceRoot=*/false);
    CHECK(d.size() == 2, "non-root: position and rotation are overrides, scale is not");
}

void testDiffSmallRotationAndQuatSign()
{
    auto p = lightNode(1, 1.0f, glm::mat4(1.0f));
    glm::mat4 tiny = glm::rotate(glm::mat4(1.0f), glm::radians(0.1f), {0, 1, 0});
    auto d = DonTopo::diffPrefabNode(1, p, lightNode(1, 1.0f, tiny), false);
    CHECK(d.size() == 1 && d[0].path == "/transform/rotation", "0.1 degree rotation is an override");

    // Same rotation through a different decomposition sign must not be an override.
    glm::mat4 big = glm::rotate(glm::mat4(1.0f), 3.0f, {0, 1, 0});
    CHECK(DonTopo::diffPrefabNode(1, lightNode(1, 1.0f, big), lightNode(1, 1.0f, big), false).empty(),
          "identical rotation gives no override");
}

void testDiffComponentAddedAndRemoved()
{
    auto p = lightNode(2, 1.0f, glm::mat4(1.0f));
    auto i = p;
    i.erase("light");
    i["rigidbody"] = { {"mass", 2.0} };
    auto d = DonTopo::diffPrefabNode(2, p, i, false);
    bool removed = false, added = false;
    for (auto& o : d) {
        if (o.path == "/light" && o.value.is_null()) removed = true;
        if (o.path == "/rigidbody" && o.value.is_object()) added = true;
    }
    CHECK(removed && added && d.size() == 2, "component removed = null, added = object");
}

void testApplyRoundTripAndOrphans()
{
    nlohmann::json root = lightNode(1, 1.0f, glm::mat4(1.0f));
    root["children"].push_back(lightNode(2, 1.0f, glm::mat4(1.0f)));
    std::vector<DonTopo::PrefabOverride> ovs = {
        {2, "/light/intensity", 3.5},
        {2, "/transform/scale", nlohmann::json::array({2.0, 2.0, 2.0})},
        {7, "/light/intensity", 9.0},     // orphan node
        {2, "/camera/fov", 50.0},         // orphan component
    };
    std::vector<std::string> warnings;
    auto applied = DonTopo::applyPrefabOverrides(root, ovs, &warnings);
    CHECK(applied.size() == 2, "orphans dropped");
    CHECK(warnings.size() == 2, "one warning per orphan");
    const auto& child = root["children"][0];
    CHECK(child["light"]["intensity"].get<double>() == 3.5, "leaf applied");
    auto again = DonTopo::diffPrefabNode(2, lightNode(2, 1.0f, glm::mat4(1.0f)), child, false);
    CHECK(again.size() == 2, "diff of the patched node gives back the 2 applied overrides");
}

void testOverridesJsonRoundTrip()
{
    std::vector<DonTopo::PrefabOverride> ovs = { {2, "/light/intensity", 3.5}, {1, "/boxCollider", nullptr} };
    auto back = DonTopo::overridesFromJson(DonTopo::overridesToJson(ovs), nullptr);
    CHECK(back.size() == 2 && back[1].value.is_null() && back[0].path == "/light/intensity",
          "overrides survive json");
}

void testLinkFieldsRoundTrip()
{
    DonTopo::Scene s;
    auto* a = s.addGameObject("Enemy");
    auto* b = s.addGameObject("Lamp", a);
    a->prefabNode = 1; b->prefabNode = 2;
    a->prefabLink = std::make_shared<DonTopo::PrefabLink>();
    a->prefabLink->path = "Prefabs/Enemy.prefab";
    a->prefabLink->overrides = { {2, "/light/intensity", 3.5} };
    auto j = s.toJson();
    DonTopo::Scene t;
    CHECK(t.fromJson(j, physics(), audio()), "loads");
    auto* a2 = t.getRoot().children[0].get();
    CHECK(a2->prefabNode == 1 && a2->prefabLink && a2->prefabLink->path == "Prefabs/Enemy.prefab",
          "root link survives");
    CHECK(a2->prefabLink && a2->prefabLink->overrides.size() == 1, "overrides survive");
    CHECK(a2->children[0]->prefabNode == 2 && !a2->children[0]->prefabLink, "child only has prefabNode");
    CHECK(t.toJson() == j, "second save identical");
}

void testScenesWithoutPrefabsByteIdentical()
{
    int checked = 0;
    // Every .scene / scene-shaped .json under assets/ that has "version" and "root".
    for (auto& e : std::filesystem::recursive_directory_iterator("assets")) {
        auto ext = e.path().extension().string();
        if (ext != ".scene" && ext != ".json") continue;
        std::ifstream in(e.path(), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), {});
        auto j = nlohmann::json::parse(bytes, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("root") || !j.contains("version")) continue;
        DonTopo::Scene a, b;
        if (!a.fromJson(j, physics(), audio())) continue;
        ++checked;
        auto first = a.toJson().dump();
        // A node NAME may legally contain "prefab": check the keys, not the word.
        CHECK(first.find("\"prefab\":") == std::string::npos &&
              first.find("\"prefabNode\":") == std::string::npos,
              "no prefab keys appear in old scenes");
        CHECK(b.fromJson(a.toJson(), physics(), audio()) && b.toJson().dump() == first,
              "old scene re-saves identical");
    }
    std::printf("scenes checked for prefab byte-identity: %d\n", checked);
}

void testMalformedPrefabLoadsUnlinked()
{
    DonTopo::Scene s;
    s.addGameObject("Enemy");
    auto j = s.toJson();
    auto& node = j["root"]["children"][0];
    node["prefab"] = 5;             // not an object
    node["prefabNode"] = "x";       // not an unsigned number
    DonTopo::Scene t;
    CHECK(t.fromJson(j, physics(), audio()), "malformed link still loads");
    auto* n = t.getRoot().children[0].get();
    CHECK(!n->prefabLink && n->prefabNode == 0, "malformed link loads unlinked");
    bool warned = false;
    for (auto& w : t.lastWarnings())
        if (w.find("prefab: malformed") != std::string::npos) warned = true;
    CHECK(warned, "malformed prefab produces a warning");
}

void testCloneKeepsLinkWithFreshIds()
{
    DonTopo::Scene s;
    auto* a = s.addGameObject("Enemy");
    a->prefabNode = 1;
    a->prefabLink = std::make_shared<DonTopo::PrefabLink>();
    a->prefabLink->path = "Prefabs/Enemy.prefab";
    a->prefabLink->overrides = { {1, "/light/intensity", 3.5} };
    auto* c = s.cloneGameObject(a, nullptr, physics(), audio());
    CHECK(c != nullptr, "clone created");
    if (!c) return;
    CHECK(c->prefabLink && c->prefabLink != a->prefabLink, "clone link is a deep copy");
    CHECK(c->prefabLink && c->prefabLink->path == a->prefabLink->path &&
          c->prefabLink->overrides.size() == 1, "clone link has the same content");
    CHECK(c->prefabNode == a->prefabNode, "prefabNode equal");
    CHECK(c->id != a->id, "ids differ");
}
nlohmann::json materialNode(uint64_t id)
{
    // A procedural mesh with empty geometry: nodeFromJson keeps "materials" for it
    // without any disk read (no sourcePath).
    nlohmann::json entry = { {"index", 0},
                             {"albedo", "Textures/a.png"}, {"normal", "Textures/n.png"},
                             {"orm", "Textures/o.png"}, {"matAsset", "Materials/m.mat"} };
    return { {"id", id}, {"name", "Crate"}, {"localTransform", mat4Json(glm::mat4(1.0f))},
             {"mesh", { {"sourcePath", ""}, {"name", "Cube"},
                        {"vertices", nlohmann::json::array()}, {"indices", nlohmann::json::array()},
                        {"materials", nlohmann::json::array({entry})} }},
             {"children", nlohmann::json::array()} };
}

void stripIds(nlohmann::json& j)
{
    if (j.is_object()) {
        j.erase("id");
        for (auto& [k, v] : j.items()) stripIds(v);
    } else if (j.is_array()) {
        for (auto& v : j) stripIds(v);
    }
}

void testResolveStoreInverse()
{
    const std::string root = (std::filesystem::temp_directory_path() / "dt_prefab_root").string();
    nlohmann::json j = materialNode(1);
    j["children"].push_back(materialNode(2));
    const nlohmann::json original = j;
    DonTopo::resolveStoredPaths(j, root);
    const auto abs0 = j["mesh"]["materials"][0]["albedo"].get<std::string>();
    CHECK(std::filesystem::path(abs0).is_absolute(), "resolve makes the path absolute");
    CHECK(j["children"][0]["mesh"]["materials"][0]["matAsset"].get<std::string>() !=
          "Materials/m.mat", "resolve walks the children");
    DonTopo::storeRelativePaths(j, root);
    CHECK(j == original, "resolve then store gives back the original");
}

// Drift guard. Limit: it only guards the keys present in the fixture, so a new
// stored-path field added to nodeToJson must be added to materialNode() too.
void testResolveMatchesNodeFromJson()
{
    const std::string root = (std::filesystem::temp_directory_path() / "dt_prefab_root2").string();
    const nlohmann::json disk = materialNode(5);

    // Path A: the disk route (Scene::fromJson -> nodeFromJson with the asset root).
    DonTopo::Scene a;
    a.setAssetRoot(root);
    nlohmann::json scene = a.toJson();
    scene["root"]["children"].push_back(disk);
    CHECK(a.fromJson(scene, physics(), audio()), "disk form loads");
    CHECK(!a.getRoot().children.empty(), "disk form produced the node");
    if (a.getRoot().children.empty()) return;
    nlohmann::json pathA = a.subtreeToMemoryJson(a.getRoot().children[0].get());

    // Path B: resolveStoredPaths + the memory route (insertFromJson, empty root).
    nlohmann::json copy = disk;
    DonTopo::resolveStoredPaths(copy, root);
    DonTopo::Scene b;
    auto* n = b.insertFromJson(copy, nullptr, 0, physics(), audio());
    CHECK(n != nullptr, "memory form loads");
    if (!n) return;
    nlohmann::json pathB = b.subtreeToMemoryJson(n);

    // Non-vacuous: every stored-path field must survive the disk load as a non-empty
    // absolute path with the expected relative suffix.
    CHECK(pathA["mesh"].contains("materials") && pathA["mesh"]["materials"].size() == 1,
          "materials survive the load");
    if (pathA["mesh"].contains("materials") && pathA["mesh"]["materials"].size() == 1)
    {
        const auto& m = pathA["mesh"]["materials"][0];
        const std::pair<const char*, const char*> expected[] = {
            {"albedo", "Textures/a.png"}, {"normal", "Textures/n.png"},
            {"orm", "Textures/o.png"}, {"matAsset", "Materials/m.mat"} };
        for (const auto& [key, suffix] : expected)
        {
            const std::string v = m.value(key, std::string());
            const std::string suf = suffix;
            CHECK(!v.empty() && std::filesystem::path(v).is_absolute() &&
                  v.size() >= suf.size() &&
                  std::filesystem::path(v).generic_string().ends_with(suf),
                  (std::string("disk load resolves ") + key).c_str());
        }
    }
    stripIds(pathA); stripIds(pathB);
    CHECK(pathA == pathB, "resolveStoredPaths + memory route == nodeFromJson disk route");
}

void testMakePrefabRenumbersAndLinks()
{
    DonTopo::Scene s;
    auto* enemy = s.addGameObject("Enemy");
    auto* lamp  = s.addGameObject("Lamp", enemy);
    auto* gun   = s.addGameObject("Gun", enemy);
    auto* muzzle = s.addGameObject("Muzzle", gun);
    const uint64_t ids[4] = { enemy->id, lamp->id, gun->id, muzzle->id };

    nlohmann::json f = s.makePrefab(enemy, "Prefabs/Enemy.prefab", true);
    CHECK(f.value("type", "") == "prefab" && f.value("version", 0) == 1 && f.contains("root"),
          "file header");
    if (!f.contains("root")) return;
    const auto& r = f["root"];
    CHECK(r["id"] == 1 && r["children"][0]["id"] == 2 && r["children"][1]["id"] == 3 &&
          r["children"][1]["children"][0]["id"] == 4, "prefab-local ids 1..4 in pre-order");
    CHECK(r["children"][1]["name"] == "Gun", "pre-order follows the tree");
    CHECK(f.dump().find("\"prefab\":") == std::string::npos &&
          f.dump().find("\"prefabNode\":") == std::string::npos, "no link keys in the file");

    CHECK(enemy->prefabNode == 1 && lamp->prefabNode == 2 && gun->prefabNode == 3 &&
          muzzle->prefabNode == 4, "scene nodes get prefabNode 1..4");
    CHECK(enemy->prefabLink && enemy->prefabLink->path == "Prefabs/Enemy.prefab" &&
          enemy->prefabLink->overrides.empty(), "root linked with no overrides");
    CHECK(!lamp->prefabLink && !gun->prefabLink && !muzzle->prefabLink, "only the root holds the link");
    CHECK(enemy->id == ids[0] && lamp->id == ids[1] && gun->id == ids[2] && muzzle->id == ids[3],
          "scene ids unchanged");

    // link == false leaves the scene alone.
    DonTopo::Scene t;
    auto* solo = t.addGameObject("Solo");
    nlohmann::json g = t.makePrefab(solo, "Prefabs/Solo.prefab", false);
    CHECK(g["root"]["id"] == 1 && solo->prefabNode == 0 && !solo->prefabLink,
          "link == false does not touch the scene");
}

// The cache is what supplies the .mat: the file is deleted before the nodes are
// built, so only the cached copy can give the albedo. The same call without the
// cache proves the difference is the cache and not some other source.
void testMatCacheAvoidsDisk()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "dt_prefab_matcache";
    fs::create_directories(dir);
    const fs::path matPath = dir / "c.mat";
    DonTopo::MaterialAsset asset;
    asset.albedo = (dir / "x.png").string();
    std::string err;
    CHECK(DonTopo::saveMaterialAsset(matPath, asset, &err), "test .mat written");

    const std::string key = matPath.string();
    const DonTopo::MaterialAssetCache cache{ { key, DonTopo::loadMaterialAsset(matPath) } };
    CHECK(fs::path(cache.at(key).albedo).filename() == "x.png", "cached .mat carries x.png");
    fs::remove(matPath);   // from here on the disk has nothing

    nlohmann::json entry = { {"index", 0}, {"matAsset", key} };
    nlohmann::json j = { {"id", 1}, {"name", "Crate"}, {"localTransform", mat4Json(glm::mat4(1.0f))},
                         {"mesh", { {"sourcePath", ""}, {"name", "Cube"},
                                    {"vertices", nlohmann::json::array()},
                                    {"indices", nlohmann::json::array()},
                                    {"materials", nlohmann::json::array({entry})} }},
                         {"children", nlohmann::json::array()} };

    auto albedoOf = [](DonTopo::GameObject* n) {
        const auto mats = DonTopo::materialsOfMesh(*n);
        return mats.empty() ? std::string("<no material>") : mats[0]->texturePath;
    };

    DonTopo::Scene withCache;
    auto* a = withCache.insertFromJsonWithCaches(j, nullptr, SIZE_MAX, physics(), audio(), nullptr, &cache);
    CHECK(a != nullptr, "insert with cache");
    if (a)
        CHECK(fs::path(albedoOf(a)).filename() == "x.png", "cache supplies the albedo of the deleted .mat");

    DonTopo::Scene without;
    auto* b = without.insertFromJsonWithCaches(j, nullptr, SIZE_MAX, physics(), audio(), nullptr, nullptr);
    CHECK(b != nullptr, "insert without cache");
    if (b)
        CHECK(fs::path(albedoOf(b)).filename() != "x.png", "without the cache the deleted .mat gives nothing");
    fs::remove_all(dir);
}

// ---- Propagation, recompute, unpack (Task 5) ----

// The prefab memory root the way the editor gets it: a real subtree serialized by
// makePrefab (complete node json, prefab-local ids 1..N) and resolved to memory form.
// Enemy (1) -> Lamp (2, point light intensity 1, range 10). A hand-written json
// would lack the default fields nodeToJson always writes, and every one of them
// would show up as a difference against a live instance.
nlohmann::json enemyPrefab()
{
    DonTopo::Scene s;
    auto* enemy = s.addGameObject("Enemy");
    auto* lamp  = s.addGameObject("Lamp", enemy);
    auto light = std::make_shared<DonTopo::LightComponent>();
    light->setIntensity(1.0f);
    light->setRange(10.0f);
    lamp->setLight(light);
    nlohmann::json root = s.makePrefab(enemy, "Prefabs/Enemy.prefab", /*link=*/false)["root"];
    DonTopo::resolveStoredPaths(root, s.assetRoot());
    return root;
}

// Instantiates prefabMemoryRoot at the end of the scene root: prefabNode = local id,
// "id" erased so every node gets a fresh scene id, link on the root.
DonTopo::GameObject* makeInstance(DonTopo::Scene& scene, const nlohmann::json& prefabMemoryRoot)
{
    nlohmann::json j = prefabMemoryRoot;
    std::function<void(nlohmann::json&)> link = [&](nlohmann::json& n) {
        n["prefabNode"] = n["id"];
        n.erase("id");
        for (auto& c : n["children"]) link(c);
    };
    link(j);
    j["prefab"] = { {"path", "Prefabs/Enemy.prefab"}, {"overrides", nlohmann::json::array()} };
    return scene.insertFromJson(j, nullptr, SIZE_MAX, physics(), audio());
}

DonTopo::GameObject* childNamed(DonTopo::GameObject* n, const std::string& name)
{
    if (!n) return nullptr;
    for (auto& c : n->children) if (c->name == name) return c.get();
    return nullptr;
}

size_t indexInParent(const DonTopo::GameObject* n)
{
    const auto& sib = n->parent->children;
    for (size_t i = 0; i < sib.size(); ++i) if (sib[i].get() == n) return i;
    return SIZE_MAX;
}

bool anyWarningContains(const DonTopo::Scene& s, const std::string& text)
{
    for (const auto& w : s.lastWarnings()) if (w.find(text) != std::string::npos) return true;
    return false;
}

void testPropagationRespectsOverrides()
{
    DonTopo::Scene s;
    nlohmann::json prefab = enemyPrefab();
    auto* a = makeInstance(s, prefab);
    auto* b = makeInstance(s, prefab);
    CHECK(a && b, "two instances");
    if (!a || !b) return;
    CHECK(!s.recomputePrefabOverrides(a, prefab) && a->prefabLink->overrides.empty(),
          "a fresh instance has no overrides");

    childNamed(a, "Lamp")->getLight()->setIntensity(5.0f);
    CHECK(s.recomputePrefabOverrides(a, prefab), "editing the instance changes its list");
    CHECK(a->prefabLink->overrides.size() == 1 && a->prefabLink->overrides[0].node == 2 &&
          a->prefabLink->overrides[0].path == "/light/intensity", "one override on the lamp intensity");

    prefab["children"][0]["light"]["range"]     = 20.0;
    prefab["children"][0]["light"]["intensity"] = 2.0;
    bool okA = false, okB = false;
    a = s.rebuildPrefabInstance(a, prefab, physics(), audio(), nullptr, nullptr, &okA);
    b = s.rebuildPrefabInstance(b, prefab, physics(), audio(), nullptr, nullptr, &okB);
    CHECK(a && b && okA && okB, "both rebuilt");
    if (!a || !b) return;
    auto* la = childNamed(a, "Lamp");
    auto* lb = childNamed(b, "Lamp");
    CHECK(la && la->getLight() && la->getLight()->getIntensity() == 5.0f &&
          la->getLight()->getRange() == 20.0f, "A keeps its intensity override and takes the new range");
    CHECK(lb && lb->getLight() && lb->getLight()->getIntensity() == 2.0f &&
          lb->getLight()->getRange() == 20.0f, "B takes both prefab values");
    CHECK(a->prefabLink && a->prefabLink->overrides.size() == 1, "A still lists its override");
    CHECK(!s.recomputePrefabOverrides(a, prefab) && !s.recomputePrefabOverrides(b, prefab),
          "a rebuilt instance diffs to exactly its stored overrides");
}

void testPropagationKeepsIdsAndAddsNew()
{
    DonTopo::Scene s;
    nlohmann::json prefab = enemyPrefab();
    s.addGameObject("Before");
    auto* a = makeInstance(s, prefab);
    s.addGameObject("After");
    if (!a) { CHECK(false, "instance"); return; }
    CHECK(indexInParent(a) == 1, "instance sits between two siblings");

    const uint64_t rootId = a->id;
    const uint64_t lampId = childNamed(a, "Lamp")->id;

    nlohmann::json gun = { {"id", 99}, {"name", "Gun"}, {"localTransform", mat4Json(glm::mat4(1.0f))},
                           {"children", nlohmann::json::array()} };
    prefab["children"].push_back(gun);
    bool ok = false;
    auto* n = s.rebuildPrefabInstance(a, prefab, physics(), audio(), nullptr, nullptr, &ok);
    CHECK(n != nullptr && ok, "rebuilt");
    if (!n) return;
    CHECK(n->id == rootId && s.findById(rootId) == n, "root keeps its id and findById finds the new root");
    CHECK(indexInParent(n) == 1, "same index in the parent");
    auto* lamp = childNamed(n, "Lamp");
    auto* g    = childNamed(n, "Gun");
    CHECK(lamp && lamp->id == lampId, "lamp keeps its id");
    CHECK(g && g->id != 99 && g->prefabNode == 99, "new node: fresh id, prefabNode = local id");
    std::unordered_set<uint64_t> seen;
    bool unique = true;
    s.traverse([&](DonTopo::GameObject* x) { if (!seen.insert(x->id).second) unique = false; });
    CHECK(unique, "every id in the scene is unique");
}

void testAddedChildSurvivesPropagation()
{
    DonTopo::Scene s;
    nlohmann::json prefab = enemyPrefab();
    auto* a = makeInstance(s, prefab);
    if (!a) { CHECK(false, "instance"); return; }
    auto* extra = s.addGameObject("Extra", childNamed(a, "Lamp"));
    const uint64_t extraId = extra->id;
    prefab["children"][0]["light"]["range"] = 20.0;
    bool ok = false;
    a = s.rebuildPrefabInstance(a, prefab, physics(), audio(), nullptr, nullptr, &ok);
    CHECK(a != nullptr && ok, "rebuilt");
    auto* e = childNamed(childNamed(a, "Lamp"), "Extra");
    CHECK(e && e->id == extraId && e->prefabNode == 0, "Extra still under Lamp, same id, still added");
}

void testOrphanOverrideDropped()
{
    DonTopo::Scene s;
    nlohmann::json prefab = enemyPrefab();
    auto* a = makeInstance(s, prefab);
    if (!a) { CHECK(false, "instance"); return; }
    a->prefabLink->overrides = { {1, "/camera/fov", 60.0} };
    a = s.rebuildPrefabInstance(a, prefab, physics(), audio());
    CHECK(a && a->prefabLink && a->prefabLink->overrides.empty(), "orphan dropped from the link");
    CHECK(anyWarningContains(s, "dropped"), "and reported");
}

void testRecomputeRemovesRevertedValue()
{
    DonTopo::Scene s;
    nlohmann::json prefab = enemyPrefab();
    auto* a = makeInstance(s, prefab);
    if (!a) { CHECK(false, "instance"); return; }
    auto light = childNamed(a, "Lamp")->getLight();
    light->setIntensity(5.0f);
    s.recomputePrefabOverrides(a, prefab);
    CHECK(a->prefabLink->overrides.size() == 1, "1 override");
    light->setIntensity(1.0f);
    CHECK(s.recomputePrefabOverrides(a, prefab), "reverting changes the list");
    CHECK(a->prefabLink->overrides.empty(), "0 overrides");
}

void testUnpack()
{
    DonTopo::Scene s;
    auto* a = makeInstance(s, enemyPrefab());
    if (!a) { CHECK(false, "instance"); return; }
    s.unpackPrefab(a);
    bool linked = false;
    a->traverse([&](DonTopo::GameObject* n) { if (n->prefabNode != 0 || n->prefabLink) linked = true; });
    CHECK(!linked, "no node of the subtree keeps prefabNode or prefabLink");
}

void testTemplateMissingDoesNotWipeOverrides()
{
    DonTopo::Scene s;
    auto* a = makeInstance(s, enemyPrefab());
    if (!a) { CHECK(false, "instance"); return; }
    a->prefabLink->overrides = { {2, "/light/intensity", 5.0} };
    CHECK(!s.recomputePrefabOverrides(a, nlohmann::json()), "null template: no change reported");
    CHECK(a->prefabLink->overrides.size() == 1, "null template: overrides intact");
}

void testRebuildFailureRestoresOldSubtree()
{
    DonTopo::Scene s;
    nlohmann::json prefab = enemyPrefab();
    s.addGameObject("Before");
    auto* a = makeInstance(s, prefab);
    s.addGameObject("After");
    if (!a) { CHECK(false, "instance"); return; }
    s.addGameObject("Extra", childNamed(a, "Lamp"));
    const size_t index = indexInParent(a);
    const uint64_t rootId = a->id;
    const nlohmann::json before = s.subtreeToMemoryJson(a);

    // A root field of the wrong type makes nodeFromJson throw on the ROOT (a child
    // would only be skipped with a warning), which is what fails the whole build.
    prefab["ssrEnabled"] = "yes";
    bool ok = true;
    auto* n = s.rebuildPrefabInstance(a, prefab, physics(), audio(), nullptr, nullptr, &ok);
    CHECK(!ok, "a failed rebuild reports rebuilt == false");
    CHECK(n != nullptr && n->id == rootId && s.findById(rootId) == n,
          "it returns the RESTORED root, same id, and findById agrees");
    auto* back = s.findById(rootId);
    CHECK(back && indexInParent(back) == index, "the old subtree is back at the same index");
    CHECK(back && s.subtreeToMemoryJson(back) == before, "exactly as before, ids included");
    CHECK(s.getRoot().children.size() == 3, "no leftover partial node");
    // The old slots were released on the way out: the restored subtree is NOT
    // registered, which is why the caller must register what comes back.
    bool unregistered = true;
    if (n) n->traverse([&](DonTopo::GameObject* x) { if (x->staticRenderIndex != -1) unregistered = false; });
    CHECK(unregistered, "restored nodes have staticRenderIndex -1 (caller registers them)");
    CHECK(anyWarningContains(s, "left as it was"), "the failure is reported");
}

void testClonedInstanceChildBecomesAddedChild()
{
    DonTopo::Scene s;
    nlohmann::json prefab = enemyPrefab();
    auto* a = makeInstance(s, prefab);
    if (!a) { CHECK(false, "instance"); return; }
    auto* lamp = childNamed(a, "Lamp");
    s.addGameObject("Bulb", lamp);   // an added child inside the cloned subtree
    auto* c = s.cloneGameObject(lamp, nullptr, physics(), audio());
    CHECK(c != nullptr, "clone created");
    if (!c) return;
    bool linked = false;
    c->traverse([&](DonTopo::GameObject* x) { if (x->prefabNode != 0 || x->prefabLink) linked = true; });
    CHECK(!linked, "a clone of an instance child carries no prefabNode anywhere");
    const uint64_t cloneId = c->id;

    // Cloning the instance ROOT keeps the link: the clone is a second instance.
    auto* c2 = s.cloneGameObject(a, nullptr, physics(), audio());
    CHECK(c2 && c2->prefabLink && c2->prefabNode == 1 && childNamed(c2, "Lamp") &&
          childNamed(c2, "Lamp")->prefabNode == 2, "a clone of the root stays linked");

    bool ok = false;
    a = s.rebuildPrefabInstance(a, prefab, physics(), audio(), nullptr, nullptr, &ok);
    CHECK(a && ok, "rebuilt");
    if (!a) return;
    auto* back = s.findById(cloneId);
    CHECK(back && back->parent == a && back->prefabNode == 0, "the clone survives as an added child");
    size_t lamps = 0;
    for (auto& ch : a->children) if (ch->prefabNode == 2) ++lamps;
    CHECK(lamps == 1, "exactly one linked lamp");
}

void testDuplicatedLinkedNodeKeptAsAddedChild()
{
    DonTopo::Scene s;
    nlohmann::json prefab = enemyPrefab();
    auto* a = makeInstance(s, prefab);
    if (!a) { CHECK(false, "instance"); return; }
    // Hand-built duplicate (what a clone made before the unlink, or a hand-edited
    // file, leaves behind): a second sibling claiming prefab node 2.
    auto* dup = s.addGameObject("LampCopy", a);
    dup->prefabNode = 2;
    auto* dupKid = s.addGameObject("Wick", dup);
    dupKid->prefabNode = 2;
    const uint64_t dupId = dup->id, dupKidId = dupKid->id;
    const uint64_t lampId = childNamed(a, "Lamp")->id;

    bool ok = false;
    a = s.rebuildPrefabInstance(a, prefab, physics(), audio(), nullptr, nullptr, &ok);
    CHECK(a && ok, "rebuilt");
    if (!a) return;
    auto* lamp = s.findById(lampId);
    auto* copy = s.findById(dupId);
    auto* wick = s.findById(dupKidId);
    CHECK(lamp && lamp->prefabNode == 2 && lamp->parent == a, "the first one stays linked");
    CHECK(copy && copy->parent == a && copy->prefabNode == 0, "the duplicate is kept as an added child");
    CHECK(wick && wick->parent == copy && wick->prefabNode == 0, "with its subtree, unlinked");
    size_t dupWarnings = 0;
    for (const auto& w : s.lastWarnings()) if (w.find("duplicates prefab node") != std::string::npos) ++dupWarnings;
    CHECK(dupWarnings == 1, "warned once");
}

void testRootWithoutPrefabNodeWarns()
{
    DonTopo::Scene s;
    nlohmann::json prefab = enemyPrefab();
    auto* a = makeInstance(s, prefab);
    if (!a) { CHECK(false, "instance"); return; }
    a->prefabNode = 0;
    a = s.rebuildPrefabInstance(a, prefab, physics(), audio());
    CHECK(a != nullptr, "still rebuilt");
    CHECK(anyWarningContains(s, "its root has no prefabNode"), "and the unmatched root is reported");
}

void testRebuildReleasesOldRootOnce()
{
    DonTopo::Scene s;
    nlohmann::json prefab = enemyPrefab();
    auto* a = makeInstance(s, prefab);
    if (!a) { CHECK(false, "instance"); return; }
    std::vector<DonTopo::GameObject*> removed;
    s.setOnNodeRemoved([&](DonTopo::GameObject* n) { removed.push_back(n); });
    const DonTopo::GameObject* old = a;
    auto* n = s.rebuildPrefabInstance(a, prefab, physics(), audio());
    CHECK(n != nullptr, "rebuilt");
    CHECK(removed.size() == 1 && removed[0] == old, "onNodeRemoved fires exactly once, for the old root");
    s.setOnNodeRemoved(nullptr);
}

void testRootPoseKeptScaleFollowsPrefab()
{
    DonTopo::Scene s;
    nlohmann::json prefab = enemyPrefab();
    auto* a = makeInstance(s, prefab);
    if (!a) { CHECK(false, "instance"); return; }
    a->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 4.0f, 5.0f));
    prefab["localTransform"] = mat4Json(glm::scale(glm::mat4(1.0f), glm::vec3(2.0f)));
    a = s.rebuildPrefabInstance(a, prefab, physics(), audio());
    if (!a) { CHECK(false, "rebuilt"); return; }
    const glm::mat4& m = a->localTransform;
    CHECK(glm::vec3(m[3]) == glm::vec3(3.0f, 4.0f, 5.0f), "root position is the instance's");
    CHECK(std::fabs(glm::length(glm::vec3(m[0])) - 2.0f) < 1e-5f, "root scale is the prefab's");
}

// ---------------------------------------------------------------- PrefabLibrary

// A temporary project root, removed on scope exit (also when a CHECK fails).
struct TempRoot
{
    std::filesystem::path p;
    explicit TempRoot(const char* name)
    {
        p = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
        std::filesystem::create_directories(p / "Prefabs");
        std::filesystem::create_directories(p / "Models");
    }
    ~TempRoot() { std::error_code ec; std::filesystem::remove_all(p, ec); }
    std::string str() const { return p.string(); }
};

void writeFile(const std::filesystem::path& file, const std::string& text)
{
    std::ofstream(file, std::ios::binary) << text;
}

void writePrefabFile(const std::filesystem::path& file, const nlohmann::json& root)
{
    writeFile(file, nlohmann::json{ {"version", 1}, {"type", "prefab"}, {"root", root} }.dump(2));
}

// One triangle: the only static model the tests can make without a tracked file
// (every FBX in the repo carries a rig).
std::string writeTriangleObj(const std::filesystem::path& file)
{
    writeFile(file, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    return file.string();
}

nlohmann::json plainNode(uint64_t id, const char* name)
{
    return { {"id", id}, {"name", name}, {"localTransform", mat4Json(glm::mat4(1.0f))},
             {"children", nlohmann::json::array()} };
}

nlohmann::json scriptRef(const char* var, const std::string& prefabPath)
{
    return nlohmann::json::array({ { {"name", "Spawner"},
                                     {"overrides", { {var, { {"prefab", prefabPath} }} }} } });
}

// Pumps until nothing is Loading (recursive requests included) or the timeout.
bool settle(DonTopo::PrefabLibrary& lib, int timeoutMs = 180000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        lib.pump(audio(), 1000.0f);
        if (lib.pending() == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// Pumps (with a zero mesh budget, so nothing is delivered) until the read result
// has been processed and exactly one mesh request sits in the loader.
bool pumpUntilMeshRequested(DonTopo::PrefabLibrary& lib, int timeoutMs = 10000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        lib.pump(audio(), 0.0f);
        if (lib.meshLoadsPending() == 1) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

void testLibraryLoadsTemplate()
{
    namespace fs = std::filesystem;
    TempRoot root("dt_prefab_lib_load");
    const fs::path maw  = root.p / "Models" / "Maw J Laygo.fbx";
    const fs::path idle = root.p / "Models" / "standing idle 01.fbx";
    fs::copy_file("assets/animatedCharacter/Maw J Laygo.fbx", maw);
    fs::copy_file("assets/animatedCharacter/standing idle 01.fbx", idle);
    const std::string tri = writeTriangleObj(root.p / "Models" / "tri.obj");
    fs::create_directories(root.p / "Materials");
    DonTopo::MaterialAsset matAsset;
    matAsset.roughness = 0.25f;
    CHECK(DonTopo::saveMaterialAsset(root.p / "Materials" / "m.mat", matAsset), ".mat written");

    nlohmann::json character = plainNode(1, "Character");
    character["mesh"] = { {"sourcePath", maw.string()}, {"name", "Maw"}, {"skinned", true},
                          {"animationSources", nlohmann::json::array({
                              { {"path", maw.string()},  {"builtin", true} },
                              { {"path", idle.string()}, {"builtin", false} } })} };
    nlohmann::json box = plainNode(2, "Box");
    box["mesh"] = { {"sourcePath", tri}, {"name", "tri"},
                    {"materials", nlohmann::json::array({ { {"index", 0}, {"matAsset", "Materials/m.mat"} } })} };
    character["children"].push_back(box);
    writePrefabFile(root.p / "Prefabs" / "Character.prefab", character);

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request("Prefabs/Character.prefab");
    CHECK(lib.find("Prefabs/Character.prefab") != nullptr, "requested key is findable at once");
    CHECK(settle(lib), "library settles");
    const DonTopo::PrefabTemplate* t = lib.find("Prefabs/Character.prefab");
    if (!t) { CHECK(false, "template exists"); return; }
    CHECK(t->state == DonTopo::PrefabTemplate::State::Ready, "template Ready");
    if (t->state != DonTopo::PrefabTemplate::State::Ready) std::fprintf(stderr, "  error: %s\n", t->error.c_str());
    CHECK(t->key == "Prefabs/Character.prefab", "key is project-relative");
    CHECK(t->generation != 0, "Ready template has a generation");
    const auto skinnedIt = t->meshes.find(DonTopo::meshCacheKey(maw.string(), 0));
    const auto triIt     = t->meshes.find(DonTopo::meshCacheKey(tri, 0));
    CHECK(skinnedIt != t->meshes.end() && triIt != t->meshes.end(), "both meshes cached");
    const auto* sk = skinnedIt != t->meshes.end()
                   ? dynamic_cast<const DonTopo::SkinnedMesh*>(skinnedIt->second.get()) : nullptr;
    CHECK(sk != nullptr, "the rigged FBX is cached as a SkinnedMesh");
    CHECK(sk && sk->animationSources.size() == 2 && sk->animationSources[1].path == idle.string(),
          "external animation source applied on the worker");
    const std::string matKey = t->memoryRoot["children"][0]["mesh"]["materials"][0]["matAsset"].get<std::string>();
    CHECK(fs::path(matKey).is_absolute(), "memoryRoot is in memory form (absolute .mat path)");
    CHECK(t->mats.count(matKey) == 1 && t->mats.at(matKey).roughness == 0.25f, ".mat preloaded");
    CHECK(lib.readCount() == 1, "one .prefab read");
}

void testLibraryFailedStates()
{
    TempRoot root("dt_prefab_lib_failed");
    writeFile(root.p / "Prefabs" / "Scene.prefab",
              nlohmann::json{ {"version", 1}, {"type", "scene"}, {"root", plainNode(1, "X")} }.dump());
    nlohmann::json broken = plainNode(1, "Broken");
    broken["mesh"] = { {"sourcePath", (root.p / "Models" / "missing.fbx").string()}, {"name", "m"} };
    writePrefabFile(root.p / "Prefabs" / "BadMesh.prefab", broken);

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    CHECK(lib.find("Prefabs/Missing.prefab") == nullptr, "never requested -> nullptr");
    lib.request("Prefabs/Missing.prefab");
    lib.request("Prefabs/Scene.prefab");
    lib.request("Prefabs/BadMesh.prefab");
    CHECK(settle(lib), "library settles");
    const auto* missing = lib.find("Prefabs/Missing.prefab");
    const auto* scene   = lib.find("Prefabs/Scene.prefab");
    const auto* badMesh = lib.find("Prefabs/BadMesh.prefab");
    CHECK(missing && missing->state == DonTopo::PrefabTemplate::State::Failed && !missing->error.empty(),
          "missing file -> Failed with an error");
    CHECK(scene && scene->state == DonTopo::PrefabTemplate::State::Failed && scene->error == "not a prefab file",
          "type scene -> Failed 'not a prefab file'");
    CHECK(badMesh && badMesh->state == DonTopo::PrefabTemplate::State::Failed && !badMesh->error.empty(),
          "mesh load error -> Failed with that error");
}

// A -> B -> A: the cycle ends because request() is idempotent.
void testLibraryRecursiveReferences()
{
    TempRoot root("dt_prefab_lib_recursive");
    nlohmann::json a = plainNode(1, "A");
    a["scripts"] = scriptRef("other", "Prefabs/B.prefab");
    nlohmann::json b = plainNode(1, "B");
    b["scripts"] = scriptRef("back", "Prefabs/A.prefab");
    writePrefabFile(root.p / "Prefabs" / "A.prefab", a);
    writePrefabFile(root.p / "Prefabs" / "B.prefab", b);

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    int readyCalls = 0;
    lib.setOnReady([&](const DonTopo::PrefabTemplate&) { ++readyCalls; });
    lib.request("Prefabs/A.prefab");
    CHECK(settle(lib), "library settles");
    const auto* ta = lib.find("Prefabs/A.prefab");
    const auto* tb = lib.find("Prefabs/B.prefab");
    CHECK(ta && ta->state == DonTopo::PrefabTemplate::State::Ready, "A Ready");
    CHECK(tb && tb->state == DonTopo::PrefabTemplate::State::Ready, "B Ready (requested by A)");
    CHECK(lib.readCount() == 2 && readyCalls == 2, "the A->B->A cycle reads each file once");
}

void testLibraryRequestIsIdempotent()
{
    TempRoot root("dt_prefab_lib_idempotent");
    writePrefabFile(root.p / "Prefabs" / "P.prefab", plainNode(1, "P"));
    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request("Prefabs/P.prefab");
    lib.request("Prefabs\\P.prefab");
    lib.request((root.p / "Prefabs" / "P.prefab").string());
    CHECK(settle(lib), "library settles");
    lib.request("Prefabs/P.prefab");
    CHECK(settle(lib), "library settles again");
    CHECK(lib.readCount() == 1, "three spellings + a later request = one read");
    CHECK(lib.normalizeKey((root.p / "Prefabs" / "P.prefab").string()) == "Prefabs/P.prefab",
          "absolute path inside the root -> project-relative");
}

void testCollectReferences()
{
    nlohmann::json scene = { {"version", 1}, {"root", plainNode(0, "")} };
    nlohmann::json n1 = plainNode(1, "S1");
    n1["scripts"] = scriptRef("enemy", "Prefabs/Enemy.prefab");
    nlohmann::json n2 = plainNode(2, "S2");
    n2["scripts"] = scriptRef("enemy", "Prefabs/Enemy.prefab");
    nlohmann::json n3 = plainNode(3, "S3");
    n3["scripts"] = scriptRef("bullet", "Prefabs/Bullet.prefab");
    // A prefab link is an object under "prefab", not a reference to collect.
    n3["prefab"] = { {"path", "Prefabs/Linked.prefab"}, {"overrides", nlohmann::json::array()} };
    scene["root"]["children"] = nlohmann::json::array({ n1, n2, n3 });
    const auto refs = DonTopo::PrefabLibrary::collectReferences(scene);
    CHECK(refs.size() == 2 && refs[0] == "Prefabs/Enemy.prefab" && refs[1] == "Prefabs/Bullet.prefab",
          "two unique script references, link path not included");
}

// A read that finished under the OLD root must not resurrect the key after
// setAssetRoot: the new root has no such file, so the key ends Failed.
void testLibraryStaleResultDropped()
{
    TempRoot oldRoot("dt_prefab_lib_stale_old");
    TempRoot newRoot("dt_prefab_lib_stale_new");
    writePrefabFile(oldRoot.p / "Prefabs" / "P.prefab", plainNode(1, "P"));
    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, oldRoot.str());
    lib.request("Prefabs/P.prefab");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (lib.readCount() < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));   // read done, NOT pumped
    std::this_thread::sleep_for(std::chrono::milliseconds(50));      // and its result posted
    lib.setAssetRoot(newRoot.str());
    CHECK(lib.find("Prefabs/P.prefab") == nullptr, "setAssetRoot clears");
    lib.request("Prefabs/P.prefab");
    CHECK(settle(lib), "library settles");
    const auto* t = lib.find("Prefabs/P.prefab");
    CHECK(t && t->state == DonTopo::PrefabTemplate::State::Failed, "old-root result did not resurrect the key");
}

// A key dropped by retainOnly while its mesh is loading can be requested again
// and loads cleanly (the cancel itself is checked by
// testLibraryRetainOnlyCancelsMeshLoads).
void testLibraryReRequestAfterRetainOnly()
{
    TempRoot root("dt_prefab_lib_retain");
    nlohmann::json p = plainNode(1, "P");
    const std::string tri = writeTriangleObj(root.p / "Models" / "tri.obj");
    p["mesh"] = { {"sourcePath", tri}, {"name", "tri"} };
    writePrefabFile(root.p / "Prefabs" / "P.prefab", p);
    writePrefabFile(root.p / "Prefabs" / "Q.prefab", plainNode(1, "Q"));

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request("Prefabs/P.prefab");
    lib.request("Prefabs/Q.prefab");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (lib.readCount() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    lib.pump(audio(), 0.0f);   // drains the reads -> P's mesh requested, Q Ready
    lib.retainOnly({ "Prefabs/Q.prefab" });
    CHECK(lib.find("Prefabs/P.prefab") == nullptr, "P dropped");
    CHECK(lib.find("Prefabs/Q.prefab") != nullptr, "Q kept");
    lib.request("Prefabs/P.prefab");
    CHECK(settle(lib), "library settles");
    const auto* t = lib.find("Prefabs/P.prefab");
    CHECK(t && t->state == DonTopo::PrefabTemplate::State::Ready && t->meshes.size() == 1,
          "re-requested P Ready with its mesh");
    CHECK(lib.readCount() == 3, "re-request after retainOnly reads again");
}

void testLibraryUpdateFromMemory()
{
    TempRoot root("dt_prefab_lib_update");
    writePrefabFile(root.p / "Prefabs" / "P.prefab", plainNode(1, "P"));
    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request("Prefabs/P.prefab");
    CHECK(settle(lib), "library settles");
    const auto* t = lib.find("Prefabs/P.prefab");
    const uint64_t gen = t ? t->generation : 0;
    int readyCalls = 0;
    lib.setOnReady([&](const DonTopo::PrefabTemplate&) { ++readyCalls; });
    nlohmann::json changed = plainNode(1, "Renamed");
    lib.updateFromMemory("Prefabs/P.prefab",
                         nlohmann::json{ {"version", 1}, {"type", "prefab"}, {"root", changed} }, audio());
    t = lib.find("Prefabs/P.prefab");
    CHECK(t && t->state == DonTopo::PrefabTemplate::State::Ready, "Ready at once (nothing to load)");
    CHECK(t && t->memoryRoot["name"] == "Renamed" && t->generation > gen, "content and generation updated");
    CHECK(readyCalls == 1 && lib.readCount() == 1, "onReady fired, file not read again");
}

// Destroying the library while its mesh job is RUNNING must wait for it: the job
// runs on the library's AsyncAssetLoader. One worker, nothing else queued, so
// "the pool is idle right after the destructor" means the destructor waited for
// the running ReadFile (without the wait the job is still mid-FBX, or crashes).
void testLibraryDestroyedWithJobsInFlight()
{
    namespace fs = std::filesystem;
    TempRoot root("dt_prefab_lib_destroy");
    const fs::path maw = root.p / "Models" / "Maw J Laygo.fbx";
    fs::copy_file("assets/animatedCharacter/Maw J Laygo.fbx", maw);
    nlohmann::json p = plainNode(1, "P");
    p["mesh"] = { {"sourcePath", maw.string()}, {"name", "Maw"} };
    writePrefabFile(root.p / "Prefabs" / "P.prefab", p);

    DonTopo::JobSystem js;
    js.start(1);
    {
        auto lib = std::make_unique<DonTopo::PrefabLibrary>(js, root.str());
        lib->request("Prefabs/P.prefab");
        CHECK(pumpUntilMeshRequested(*lib), "P's mesh job submitted");
        // Give the worker time to START it (the FBX takes far longer). Not a
        // correctness wait: if it is still queued, the destructor cancels it and
        // the assertions below hold just the same; this only makes the
        // wait-for-a-running-job path the one usually exercised.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(lib->meshLoadsPending() == 1 && !js.idle(), "the mesh job is in flight before destruction");
    }
    CHECK(js.idle(), "the destructor waited for the running mesh job");
    js.shutdown();
}

// retainOnly cancels the loader requests of the dropped templates. The single
// worker is held by a blocker, so the mesh job is still queued when retainOnly
// runs: only a real cancel brings the loader's pending count to 0 (the serial
// check alone would leave it at 1 until the result is delivered).
void testLibraryRetainOnlyCancelsMeshLoads()
{
    TempRoot root("dt_prefab_lib_retain_cancel");
    nlohmann::json p = plainNode(1, "P");
    p["mesh"] = { {"sourcePath", writeTriangleObj(root.p / "Models" / "tri.obj")}, {"name", "tri"} };
    writePrefabFile(root.p / "Prefabs" / "P.prefab", p);

    DonTopo::JobSystem js;
    js.start(1);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request("Prefabs/P.prefab");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (lib.readCount() < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // One worker: it finishes the read job (posting included) before it can take
    // the blocker, so once the blocker runs the read result is in the mailbox.
    std::atomic<bool> started{ false };
    std::atomic<bool> release{ false };
    js.submit([&started, &release] {
        started = true;
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    while (!started && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(started, "the blocker holds the worker");
    CHECK(pumpUntilMeshRequested(lib), "mesh request queued behind the blocker");
    lib.retainOnly({});
    CHECK(lib.meshLoadsPending() == 0, "retainOnly cancelled the mesh request");
    release = true;
    js.shutdown();
}

// An onReady callback that clears the library must stop the recursive requests
// of the template that just became Ready.
void testLibraryClearInOnReadyStopsRecursion()
{
    TempRoot root("dt_prefab_lib_clear_onready");
    nlohmann::json a = plainNode(1, "A");
    a["scripts"] = scriptRef("other", "Prefabs/B.prefab");
    writePrefabFile(root.p / "Prefabs" / "A.prefab", a);
    writePrefabFile(root.p / "Prefabs" / "B.prefab", plainNode(1, "B"));

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.setOnReady([&](const DonTopo::PrefabTemplate&) { lib.clear(); });
    lib.request("Prefabs/A.prefab");
    CHECK(settle(lib), "library settles");
    CHECK(lib.find("Prefabs/A.prefab") == nullptr && lib.find("Prefabs/B.prefab") == nullptr,
          "nothing requested after the callback cleared the library");
    CHECK(lib.readCount() == 1, "B never read");
}

void testLibraryNormalizeKeyRootCase()
{
    if (DonTopo::platform::currentOs() != DonTopo::platform::Os::Windows) return;   // case-sensitive paths
    TempRoot root("dt_prefab_lib_case");
    DonTopo::JobSystem js;
    DonTopo::PrefabLibrary lib(js, root.str());
    std::string lowerRoot = root.str();
    std::transform(lowerRoot.begin(), lowerRoot.end(), lowerRoot.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    CHECK(lowerRoot != root.str(), "the temp root has upper-case letters to lower");
    CHECK(lib.normalizeKey(lowerRoot + "\\Prefabs\\P.prefab") == "Prefabs/P.prefab",
          "the root prefix is compared case-insensitively on Windows");
}

// A mesh job the JobSystem refuses must fail the template, not leave it Loading.
void testLibraryRefusedMeshJobFails()
{
    TempRoot root("dt_prefab_lib_refused");
    DonTopo::JobSystem js;   // never started: every submit is refused
    DonTopo::PrefabLibrary lib(js, root.str());
    nlohmann::json p = plainNode(1, "P");
    p["mesh"] = { {"sourcePath", writeTriangleObj(root.p / "Models" / "tri.obj")}, {"name", "tri"} };
    lib.updateFromMemory("Prefabs/P.prefab",
                         nlohmann::json{ {"version", 1}, {"type", "prefab"}, {"root", p} }, audio());
    const auto* t = lib.find("Prefabs/P.prefab");
    CHECK(t && t->state == DonTopo::PrefabTemplate::State::Failed &&
          t->error == "the job system is not running", "refused mesh job -> Failed");
}

// updateFromMemory reuses what the old version already had (same mesh object,
// Ready on return) and loads on workers what it did not (a new .mat and mesh).
void testLibraryUpdateFromMemoryReuseAndAsync()
{
    namespace fs = std::filesystem;
    TempRoot root("dt_prefab_lib_update_meshes");
    const std::string tri  = writeTriangleObj(root.p / "Models" / "tri.obj");
    const std::string tri2 = writeTriangleObj(root.p / "Models" / "tri2.obj");
    fs::create_directories(root.p / "Materials");
    DonTopo::MaterialAsset mat;
    mat.metallic = 0.5f;
    CHECK(DonTopo::saveMaterialAsset(root.p / "Materials" / "n.mat", mat), ".mat written");
    nlohmann::json p = plainNode(1, "P");
    p["mesh"] = { {"sourcePath", tri}, {"name", "tri"} };
    writePrefabFile(root.p / "Prefabs" / "P.prefab", p);
    auto file = [](const nlohmann::json& r) { return nlohmann::json{ {"version", 1}, {"type", "prefab"}, {"root", r} }; };

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request("Prefabs/P.prefab");
    CHECK(settle(lib), "library settles");
    const auto* t = lib.find("Prefabs/P.prefab");
    if (!t || t->state != DonTopo::PrefabTemplate::State::Ready) { CHECK(false, "P Ready"); return; }
    const DonTopo::Mesh* original = t->meshes.at(tri).get();

    p["name"] = "Renamed";
    lib.updateFromMemory("Prefabs/P.prefab", file(p), audio());
    CHECK(t->state == DonTopo::PrefabTemplate::State::Ready && t->memoryRoot["name"] == "Renamed",
          "reuse path: Ready on return");
    CHECK(t->meshes.count(tri) && t->meshes.at(tri).get() == original && lib.meshLoadsPending() == 0,
          "reuse path: same mesh object, nothing requested");

    nlohmann::json child = plainNode(2, "Child");
    child["mesh"] = { {"sourcePath", tri2}, {"name", "tri2"},
                      {"materials", nlohmann::json::array({ { {"index", 0}, {"matAsset", "Materials/n.mat"} } })} };
    p["children"].push_back(child);
    lib.updateFromMemory("Prefabs/P.prefab", file(p), audio());
    CHECK(t->state == DonTopo::PrefabTemplate::State::Loading, "async path: Loading on return");
    CHECK(settle(lib), "library settles");
    CHECK(t->state == DonTopo::PrefabTemplate::State::Ready, "async path: Ready after pumping");
    CHECK(t->meshes.count(tri2) == 1 && t->meshes.count(tri) && t->meshes.at(tri).get() == original,
          "new mesh loaded, old one kept");
    const std::string matKey = t->memoryRoot["children"][0]["mesh"]["materials"][0]["matAsset"].get<std::string>();
    CHECK(t->mats.count(matKey) == 1 && t->mats.at(matKey).metallic == 0.5f, "new .mat loaded on a worker");
    CHECK(lib.readCount() == 1, "the .prefab itself never read again");
}

void testLibraryWarmsAudio()
{
    namespace fs = std::filesystem;
    if (!audio().available()) { std::printf("SKIP testLibraryWarmsAudio (FMOD not available)\n"); return; }
    TempRoot root("dt_prefab_lib_audio");
    const fs::path clip = root.p / "audio.mp3";
    fs::copy_file("assets/audio.mp3", clip);
    nlohmann::json p = plainNode(1, "P");
    p["audioClip"] = { {"path", clip.string()}, {"is3D", false}, {"loop", false} };
    writePrefabFile(root.p / "Prefabs" / "P.prefab", p);
    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request("Prefabs/P.prefab");
    CHECK(settle(lib), "library settles");
    const auto* t = lib.find("Prefabs/P.prefab");
    CHECK(t && t->state == DonTopo::PrefabTemplate::State::Ready && t->warmAudio.size() == 1,
          "Ready template keeps its audio clip warm");
}

nlohmann::json sourceJson(const DonTopo::AnimationSource& s)
{
    return { {"path", s.path}, {"builtin", s.builtin}, {"clips", s.clipNames} };
}

// updateFromMemory must configure the RAW loader mesh, not the one already
// configured for the previous version (applyAnimationSourceConfig only appends):
// otherwise swapped or removed sources leave stale clips, and nodeFromJson sees a
// mismatch at Instantiate and goes to disk.
// keepRaw = true (editor): the raw mesh is in RAM and the new config is built
// from it on a worker. keepRaw = false (runtime default): the raw one was dropped
// once configured, so changed sources re-request the mesh from the loader.
// Either way the template ends with exactly the requested clips.
void testLibraryUpdateReconfiguresAnimationSources(bool keepRaw)
{
    namespace fs = std::filesystem;
    TempRoot root(keepRaw ? "dt_prefab_lib_anim_update_raw" : "dt_prefab_lib_anim_update");
    const fs::path maw  = root.p / "Models" / "Maw J Laygo.fbx";
    const fs::path idle = root.p / "Models" / "standing idle 01.fbx";
    const fs::path run  = root.p / "Models" / "standing run forward.fbx";
    fs::copy_file("assets/animatedCharacter/Maw J Laygo.fbx", maw);
    fs::copy_file("assets/animatedCharacter/standing idle 01.fbx", idle);
    fs::copy_file("assets/animatedCharacter/standing run forward.fbx", run);
    auto node = [&](const nlohmann::json* sources) {
        nlohmann::json n = plainNode(1, "C");
        n["mesh"] = { {"sourcePath", maw.string()}, {"name", "Maw"}, {"skinned", true} };
        if (sources) n["mesh"]["animationSources"] = *sources;
        return n;
    };
    auto file = [](const nlohmann::json& r) { return nlohmann::json{ {"version", 1}, {"type", "prefab"}, {"root", r} }; };

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.setKeepRawMeshes(keepRaw);

    // The clip names each source gets, as an editor-saved file would carry them.
    const nlohmann::json all = nlohmann::json::array({ { {"path", maw.string()}, {"builtin", true} },
                                                       { {"path", idle.string()}, {"builtin", false} },
                                                       { {"path", run.string()}, {"builtin", false} } });
    writePrefabFile(root.p / "Prefabs" / "All.prefab", node(&all));
    lib.request("Prefabs/All.prefab");
    CHECK(settle(lib), "library settles");
    const auto* tAll = lib.find("Prefabs/All.prefab");
    const auto* skAll = tAll && tAll->meshes.count(maw.string())
                      ? dynamic_cast<const DonTopo::SkinnedMesh*>(tAll->meshes.at(maw.string()).get()) : nullptr;
    if (!skAll || skAll->animationSources.size() != 3) { CHECK(false, "three sources configured"); return; }
    const nlohmann::json s0 = sourceJson(skAll->animationSources[0]);
    const nlohmann::json s1 = sourceJson(skAll->animationSources[1]);
    const nlohmann::json s2 = sourceJson(skAll->animationSources[2]);

    auto meshOf = [&]() -> const DonTopo::SkinnedMesh* {
        const auto* t = lib.find("Prefabs/P.prefab");
        if (!t || t->state != DonTopo::PrefabTemplate::State::Ready || !t->meshes.count(maw.string())) return nullptr;
        return dynamic_cast<const DonTopo::SkinnedMesh*>(t->meshes.at(maw.string()).get());
    };
    // Exactly these sources (what nodeFromJson checks to SHARE the mesh) and no
    // clip that belongs to none of them.
    auto exactly = [&](const nlohmann::json& sources) {
        const auto* m = meshOf();
        if (!m || !DonTopo::meshMatchesAnimationConfig(*m, sources)) return false;
        size_t named = 0;
        for (const auto& s : m->animationSources) named += s.clipNames.size();
        return named == m->animationClips.size();
    };

    const nlohmann::json first = nlohmann::json::array({ s0, s1 });
    writePrefabFile(root.p / "Prefabs" / "P.prefab", node(&first));
    lib.request("Prefabs/P.prefab");
    CHECK(settle(lib), "library settles");
    CHECK(exactly(first), "initial [builtin, idle]");

    const nlohmann::json swapped = nlohmann::json::array({ s0, s2 });
    lib.updateFromMemory("Prefabs/P.prefab", file(node(&swapped)), audio());
    CHECK(lib.meshLoadsPending() == (keepRaw ? 0 : 1),
          keepRaw ? "raw kept: configured from RAM, no loader request"
                  : "raw dropped: changed sources re-request the mesh");
    CHECK(settle(lib), "library settles");
    CHECK(exactly(swapped), "[builtin, idle] -> [builtin, run]: no stale idle clips");

    // Same sources again (an Apply that did not touch them): reused as is.
    const DonTopo::SkinnedMesh* before = meshOf();
    lib.updateFromMemory("Prefabs/P.prefab", file(node(&swapped)), audio());
    CHECK(meshOf() != nullptr && meshOf() == before && lib.meshLoadsPending() == 0,
          "unchanged sources: same configured mesh, Ready at once, nothing requested");

    lib.updateFromMemory("Prefabs/P.prefab", file(node(nullptr)), audio());
    CHECK(settle(lib), "library settles");
    const auto* m = meshOf();
    CHECK(m && m->animationSources.size() == 1 && m->animationSources[0].builtin,
          "no animationSources key -> only the builtin source (what nodeFromJson shares)");

    const nlohmann::json onlyBuiltin = nlohmann::json::array({ s0 });
    lib.updateFromMemory("Prefabs/P.prefab", file(node(&first)), audio());
    CHECK(settle(lib), "library settles");
    lib.updateFromMemory("Prefabs/P.prefab", file(node(&onlyBuiltin)), audio());
    CHECK(settle(lib), "library settles");
    CHECK(exactly(onlyBuiltin), "[builtin, idle] -> [builtin]: removed source leaves no clips");
}

// ---- Scene::instantiatePrefab + Lua Instantiate (Task 8) ----

// Strips what an instance is allowed to differ in: scene ids and the link keys.
void stripInstanceKeys(nlohmann::json& j)
{
    if (j.is_object())
    {
        j.erase("id");
        j.erase("prefab");
        j.erase("prefabNode");
        for (auto& [k, v] : j.items()) stripInstanceKeys(v);
    }
    else if (j.is_array())
    {
        for (auto& v : j) stripInstanceKeys(v);
    }
}

std::vector<uint64_t> sceneIds(DonTopo::Scene& s)
{
    std::vector<uint64_t> ids;
    s.traverse([&](DonTopo::GameObject* n) { if (n->parent) ids.push_back(n->id); });
    return ids;
}

const DonTopo::PrefabTemplate* readyTemplate(DonTopo::PrefabLibrary& lib, const std::string& key)
{
    if (!settle(lib)) { CHECK(false, "library settles"); return nullptr; }
    const DonTopo::PrefabTemplate* t = lib.find(key);
    if (!t || t->state != DonTopo::PrefabTemplate::State::Ready)
    {
        std::fprintf(stderr, "  template %s not Ready: %s\n", key.c_str(), t ? t->error.c_str() : "never requested");
        CHECK(false, "template Ready");
        return nullptr;
    }
    return t;
}

// The Crate prefab of the round-trip and id tests: procedural cube + box collider +
// script override at the root, a point light child. Written to <root>/Prefabs/Crate.prefab.
// Returns the memory-form JSON of the source subtree (what an instance must equal).
nlohmann::json writeCratePrefab(const TempRoot& root, DonTopo::Scene& s)
{
    s.setAssetRoot(root.str());
    auto* crate = s.addGameObject("Crate");
    crate->localTransform = glm::translate(glm::mat4(1.0f), {2.0f, 3.0f, 4.0f}) *
                            glm::rotate(glm::mat4(1.0f), 0.5f, {0.0f, 1.0f, 0.0f});
    crate->updateWorldTransforms(glm::mat4(1.0f));
    crate->setMesh(std::make_shared<DonTopo::Mesh>(DonTopo::Cube::create(1.0f)));
    crate->setBoxCollider(physics().createBoxColliderComponent(glm::vec3(0.5f), glm::vec3(0.0f),
                                                               crate->worldTransform, /*dynamic=*/false));
    auto script = std::make_unique<DonTopo::ScriptComponent>("Rotator", crate);
    script->overrides["speed"] = 7.0;
    crate->addScript(std::move(script));
    auto* lamp = s.addGameObject("Lamp", crate);
    auto light = std::make_shared<DonTopo::LightComponent>();
    light->setIntensity(2.0f);
    lamp->setLight(light);

    const nlohmann::json original = s.subtreeToMemoryJson(crate);
    writeFile(root.p / "Prefabs" / "Crate.prefab", s.makePrefab(crate, "Prefabs/Crate.prefab", false).dump(2));
    s.removeGameObject(crate);
    return original;
}

// Spec test 1: subtree -> file -> template -> instance gives the same JSON, except
// ids and link fields.
void testInstantiateRoundTrip()
{
    TempRoot root("dt_prefab_inst_roundtrip");
    DonTopo::Scene s;
    nlohmann::json original = writeCratePrefab(root, s);

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request("Prefabs/Crate.prefab");
    const auto* t = readyTemplate(lib, "Prefabs/Crate.prefab");
    if (!t) return;

    DonTopo::GameObject* inst = s.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
    CHECK(inst != nullptr, "instantiatePrefab returns the root");
    if (!inst) return;
    CHECK(inst->parent == &s.getRoot(), "no parent -> child of the scene root");
    CHECK(inst->prefabLink && inst->prefabLink->path == "Prefabs/Crate.prefab" &&
          inst->prefabLink->overrides.empty(), "root linked to the template key, no overrides");
    CHECK(inst->prefabNode == 1 && inst->children.size() == 1 && inst->children[0]->prefabNode == 2 &&
          !inst->children[0]->prefabLink, "prefabNode = prefab-local ids, only the root holds the link");
    CHECK(inst->staticRenderIndex == -1 && inst->children[0]->staticRenderIndex == -1,
          "render indices left at -1 for the caller to register");

    nlohmann::json got = s.subtreeToMemoryJson(inst);
    stripInstanceKeys(got);
    stripInstanceKeys(original);
    CHECK(got == original, "instance equals the source subtree except ids and link fields");
    if (got != original)
        std::fprintf(stderr, "  original: %s\n  instance: %s\n", original.dump().c_str(), got.dump().c_str());

    // Pose: position + Euler degrees are WORLD space (Unity), the scale is the prefab's.
    DonTopo::GameObject* holder = s.addGameObject("Holder");
    holder->localTransform = glm::translate(glm::mat4(1.0f), {10.0f, 0.0f, 0.0f});
    holder->updateWorldTransforms(glm::mat4(1.0f));
    const glm::vec3 pos(1.0f, 2.0f, 3.0f), rot(0.0f, 90.0f, 0.0f);
    DonTopo::GameObject* posed = s.instantiatePrefab(*t, holder, &pos, &rot, physics(), audio());
    CHECK(posed && posed->parent == holder, "parented instance");
    if (posed)
    {
        const glm::vec3 world(posed->worldTransform[3]);
        CHECK(glm::length(world - pos) < 1e-4f, "position is world space");
        const glm::vec3 local(posed->localTransform[3]);
        CHECK(glm::length(local - glm::vec3(-9.0f, 2.0f, 3.0f)) < 1e-4f, "local = world relative to the parent");
        const glm::vec3 fwd = glm::normalize(glm::vec3(posed->worldTransform * glm::vec4(0, 0, -1, 0)));
        CHECK(glm::length(fwd - glm::vec3(-1.0f, 0.0f, 0.0f)) < 1e-4f, "90 deg yaw turns -Z into -X");
        CHECK(std::fabs(glm::length(glm::vec3(posed->worldTransform[0])) - 1.0f) < 1e-4f, "prefab scale kept");
    }

    // A tiny but perfectly invertible parent (uniform 1e-4, det 1e-12): the pose is
    // still world space, not mistaken for a singular parent.
    DonTopo::GameObject* tiny = s.addGameObject("Tiny");
    tiny->localTransform = glm::translate(glm::mat4(1.0f), {5.0f, 0.0f, 0.0f}) *
                           glm::scale(glm::mat4(1.0f), glm::vec3(1e-4f));
    tiny->updateWorldTransforms(glm::mat4(1.0f));
    DonTopo::GameObject* small = s.instantiatePrefab(*t, tiny, &pos, &rot, physics(), audio());
    CHECK(small && glm::length(glm::vec3(small->worldTransform[3]) - pos) < 1e-3f,
          "world position kept under a parent scaled by 1e-4");

    // Not Ready -> nullptr, scene untouched.
    DonTopo::PrefabTemplate loading;
    loading.key = "Prefabs/X.prefab";
    const size_t before = sceneIds(s).size();
    CHECK(s.instantiatePrefab(loading, nullptr, nullptr, nullptr, physics(), audio()) == nullptr &&
          sceneIds(s).size() == before, "a template that is not Ready instantiates nothing");
}

// Spec test 4: fresh unique ids on the four paths (Instantiate x100, clone, undo of
// Delete, propagation), and the old ids keep resolving.
void testInstantiateIdsUniqueAcrossFourPaths()
{
    TempRoot root("dt_prefab_inst_ids");
    DonTopo::Scene s;
    writeCratePrefab(root, s);
    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request("Prefabs/Crate.prefab");
    const auto* t = readyTemplate(lib, "Prefabs/Crate.prefab");
    if (!t) return;

    std::vector<DonTopo::GameObject*> insts;
    for (int i = 0; i < 100; ++i)
        insts.push_back(s.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio()));
    CHECK(std::all_of(insts.begin(), insts.end(), [](auto* p) { return p != nullptr; }), "100 instances");
    if (std::any_of(insts.begin(), insts.end(), [](auto* p) { return p == nullptr; })) return;

    // Clone of an instance.
    DonTopo::GameObject* clone = s.cloneGameObject(insts[0], nullptr, physics(), audio());
    CHECK(clone != nullptr, "clone");
    // Undo of Delete: snapshot, delete, re-insert at the same place.
    const uint64_t deletedId = insts[1]->id;
    const nlohmann::json snap = s.subtreeToJson(insts[1]);
    s.removeGameObject(insts[1]);
    DonTopo::GameObject* back = s.insertFromJson(snap, nullptr, 1, physics(), audio());
    CHECK(back && back->id == deletedId, "undo of Delete keeps the original id");
    // Propagation.
    const uint64_t propagatedId = insts[2]->id;
    DonTopo::GameObject* rebuilt = s.rebuildPrefabInstance(insts[2], t->memoryRoot, physics(), audio(),
                                                           &t->meshes, &t->mats);
    CHECK(rebuilt && rebuilt->id == propagatedId && s.findById(propagatedId) == rebuilt,
          "propagation keeps the root id and it resolves");

    const std::vector<uint64_t> ids = sceneIds(s);
    const std::set<uint64_t> unique(ids.begin(), ids.end());
    CHECK(ids.size() == 2u * 101u, "every node accounted for (100 instances + 1 clone, 2 nodes each)");
    CHECK(unique.size() == ids.size(), "no duplicate id across the four paths");
    CHECK(s.findById(insts[50]->id) == insts[50], "an older instance still resolves");
}

std::string albedoOf(DonTopo::GameObject* n)
{
    const auto mats = n ? DonTopo::materialsOfMesh(*n) : std::vector<const DonTopo::Material*>{};
    return mats.empty() ? std::string("<no material>") : mats[0]->texturePath;
}

// Spec test 5: once Ready, Instantiate touches no disk. The temp copies of the
// .prefab, the FBX, the .mat (and the audio sidecar) are deleted before the last
// Instantiate; the result must equal the one built while they existed.
// The skinned FBX is the benchmark subject too (spec test 10, Release only).
void testInstantiateNoIo()
{
    namespace fs = std::filesystem;
    TempRoot root("dt_prefab_inst_noio");
    const fs::path mawPath = root.p / "Models" / "Maw J Laygo.fbx";
    const fs::path idle    = root.p / "Models" / "standing idle 01.fbx";
    fs::copy_file("assets/animatedCharacter/Maw J Laygo.fbx", mawPath);
    fs::copy_file("assets/animatedCharacter/standing idle 01.fbx", idle);
    const std::string tri = writeTriangleObj(root.p / "Models" / "tri.obj");
    fs::create_directories(root.p / "Materials");
    DonTopo::MaterialAsset matAsset;
    matAsset.albedo = (root.p / "Textures" / "x.png").string();
    CHECK(DonTopo::saveMaterialAsset(root.p / "Materials" / "m.mat", matAsset), ".mat written");
    const fs::path clip = root.p / "audio.mp3";
    fs::copy_file("assets/audio.mp3", clip);
    DonTopo::AudioImportSettings sidecar;
    sidecar.gainDb = 3.0f;
    CHECK(DonTopo::saveAudioImportSettings(clip, sidecar), "audio sidecar written");

    DonTopo::MaterialAsset skinMat;
    skinMat.albedo = (root.p / "Textures" / "skin.png").string();
    CHECK(DonTopo::saveMaterialAsset(root.p / "Materials" / "skin.mat", skinMat), "skin .mat written");
    const std::string missing = (root.p / "Models" / "missing source.fbx").string();

    // Character (1): Maw + [builtin, idle] + a .mat on the SKINNED node.
    //   Box (2):     static triangle + a .mat.
    //   Speaker (3): a STREAMED clip with a non-default rolloff (both in the sound key).
    //   Twin (4):    Maw again with a different source list ([builtin] only).
    //   Broken (5):  Maw with [builtin, <missing file>]: the library warns once and
    //                the instance must NOT retry it.
    auto maw = [&](nlohmann::json sources) {
        return nlohmann::json{ {"sourcePath", mawPath.string()}, {"name", "Maw"}, {"skinned", true},
                               {"animationSources", std::move(sources)} };
    };
    const nlohmann::json builtin = { {"path", mawPath.string()}, {"builtin", true} };
    nlohmann::json character = plainNode(1, "Character");
    character["mesh"] = maw(nlohmann::json::array({ builtin, { {"path", idle.string()}, {"builtin", false} } }));
    character["mesh"]["materials"] = nlohmann::json::array({ { {"index", 0}, {"matAsset", "Materials/skin.mat"} } });
    nlohmann::json box = plainNode(2, "Box");
    box["mesh"] = { {"sourcePath", tri}, {"name", "tri"},
                    {"materials", nlohmann::json::array({ { {"index", 0}, {"matAsset", "Materials/m.mat"} } })} };
    nlohmann::json speaker = plainNode(3, "Speaker");
    speaker["audioClip"] = { {"path", clip.string()}, {"is3D", true}, {"loop", false},
                             {"loadMode", "stream"}, {"rolloff", "linear"} };
    nlohmann::json twin = plainNode(4, "Twin");
    twin["mesh"] = maw(nlohmann::json::array({ builtin }));
    nlohmann::json broken = plainNode(5, "Broken");
    broken["mesh"] = maw(nlohmann::json::array({ builtin, { {"path", missing}, {"builtin", false} } }));
    for (auto* c : { &box, &speaker, &twin, &broken }) character["children"].push_back(*c);
    writePrefabFile(root.p / "Prefabs" / "Character.prefab", character);

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    const int soundsCreatedBefore = audio().soundCreateCount();
    lib.request("Prefabs/Character.prefab");
    const auto* t = readyTemplate(lib, "Prefabs/Character.prefab");
    if (!t) return;
    if (audio().available())
    {
        // Fix 3: one sound, created once with its final key; nothing else pinned.
        CHECK(audio().soundCreateCount() - soundsCreatedBefore == 1, "the streamed clip creates exactly one sound");
        CHECK(t->warmAudio.size() == 1 && t->warmAudio[0]->getLoadMode() == DonTopo::AudioLoadMode::Stream &&
              t->warmAudio[0]->getRolloff() == DonTopo::AudioRolloff::Linear, "one warm clip, stream + linear");
    }
    // Fix 1: the missing source is reported by the TEMPLATE, once.
    const auto missingWarnings = std::count_if(t->warnings.begin(), t->warnings.end(), [&](const std::string& w) {
        return w.find("missing source.fbx") != std::string::npos; });
    CHECK(missingWarnings >= 1, "the template reports the missing animation source");
    for (const auto& w : t->warnings) std::fprintf(stderr, "  template warning: %s\n", w.c_str());

    auto nodeMesh = [&](uint64_t local) -> const DonTopo::Mesh* {
        const auto it = t->instanceMeshes.find(DonTopo::prefabNodeMeshRef(local));
        return it == t->instanceMeshes.end() ? nullptr : it->second.get();
    };
    CHECK(nodeMesh(1) && nodeMesh(2) && nodeMesh(4) && nodeMesh(5), "every file mesh node has a prepared mesh");
    CHECK(nodeMesh(4) != nodeMesh(5) && nodeMesh(4) != nodeMesh(1), "one mesh per configuration / override set");

    // Fix 5: with every file still on disk, building an instance reads no model file
    // (not even the hasBones probe, which on a deleted file would fail silently).
    DonTopo::Scene s;
    int modelReads = DonTopo::ModelLoader::readFileCount();
    DonTopo::GameObject* ref = s.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
    CHECK(DonTopo::ModelLoader::readFileCount() == modelReads, "no model file read with the files present");
    CHECK(ref != nullptr, "reference instance");
    if (!ref) return;
    const std::string refAlbedo = albedoOf(childNamed(ref, "Box"));
    CHECK(fs::path(refAlbedo).filename() == "x.png", "reference instance has the .mat albedo");
    CHECK(fs::path(albedoOf(ref)).filename() == "skin.png", "the skinned node has its .mat albedo");
    for (const auto& w : s.lastWarnings()) std::fprintf(stderr, "  warning: %s\n", w.c_str());
    CHECK(s.lastWarnings().empty(), "no per-instance warning (the missing source is the template's)");
    // Nothing but the template may keep the sound alive from here on.
    s.removeGameObject(ref);

    const int reads = lib.readCount();
    std::error_code ec;
    fs::remove(root.p / "Prefabs" / "Character.prefab", ec);
    fs::remove(mawPath, ec);
    fs::remove(idle, ec);
    fs::remove(root.p / "Models" / "tri.obj", ec);
    fs::remove(root.p / "Materials" / "m.mat", ec);
    fs::remove(root.p / "Materials" / "skin.mat", ec);
    fs::remove(fs::path(clip.string() + ".json"), ec);
    fs::remove(clip, ec);   // a streamed sound keeps it open; the sound counts are the audio proof
    CHECK(!fs::exists(mawPath) && !fs::exists(root.p / "Materials" / "m.mat") &&
          !fs::exists(root.p / "Prefabs" / "Character.prefab"), "temp copies deleted");

    const size_t sounds = audio().loadedSoundCount();
    const int created = audio().soundCreateCount();
    modelReads = DonTopo::ModelLoader::readFileCount();
    DonTopo::GameObject* inst  = s.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
    const size_t warningsFirst = s.lastWarnings().size();
    DonTopo::GameObject* inst2 = s.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
    CHECK(inst != nullptr && inst2 != nullptr, "instances built with the files gone");
    if (!inst || !inst2) return;
    CHECK(DonTopo::ModelLoader::readFileCount() == modelReads, "no model file read");
    CHECK(warningsFirst == 0 && s.lastWarnings().empty(), "no warnings per Instantiate (a disk miss would warn)");
    for (const auto& w : s.lastWarnings()) std::fprintf(stderr, "  warning: %s\n", w.c_str());
    for (const char* name : { "Box", "Twin", "Broken" })
    {
        DonTopo::GameObject* a = childNamed(inst, name);
        DonTopo::GameObject* b = childNamed(inst2, name);
        CHECK(a && b && a->getMesh() && a->getMesh() == b->getMesh(), "both instances share the node's mesh");    }
    CHECK(inst->getMesh() && inst->getMesh() == inst2->getMesh() && inst->getMesh().get() == nodeMesh(1),
          "the skinned node WITH a .mat shares the template's prepared mesh");
    CHECK(childNamed(inst, "Box")->getMesh().get() == nodeMesh(2), "static node with a .mat: template's mesh");
    CHECK(childNamed(inst, "Twin")->getMesh().get() == nodeMesh(4) &&
          childNamed(inst, "Broken")->getMesh().get() == nodeMesh(5), "per-configuration meshes shared");
    CHECK(fs::path(albedoOf(childNamed(inst, "Box"))).filename() == "x.png" &&
          albedoOf(childNamed(inst, "Box")) == refAlbedo, "same .mat albedo as the reference");
    CHECK(fs::path(albedoOf(inst)).filename() == "skin.png", "skinned .mat albedo");
    DonTopo::GameObject* speakerGo = childNamed(inst, "Speaker");
    if (audio().available())
    {
        CHECK(speakerGo && speakerGo->hasAudioClip(), "audio clip built");
        CHECK(audio().loadedSoundCount() == sounds && audio().soundCreateCount() == created,
              "no FMOD sound created, not even a throwaway one: the warm clip was reused");
    }
    else
    {
        std::printf("SKIP audio part of testInstantiateNoIo (FMOD not available)\n");
    }
    CHECK(lib.readCount() == reads, "no .prefab read");

    // An edit copies only THAT instance's mesh (COW); the baseline that travelled in
    // the template json is the model's own texture, so clearing the override restores
    // it rather than keeping the .mat's.
    DonTopo::GameObject* box2 = childNamed(inst2, "Box");
    const std::string fbxAlbedo = DonTopo::materialsOfMesh(*box2).empty() ? std::string()
        : t->meshes.at(DonTopo::meshCacheKey(tri, 0))->material.texturePath;
    box2->materialOverrides.at(0).matAsset.clear();
    DonTopo::applyMaterialOverrides(*box2, &t->mats);
    CHECK(box2->getMesh().get() != nodeMesh(2), "the edited instance got its own copy");
    CHECK(childNamed(inst, "Box")->getMesh().get() == nodeMesh(2), "the other instance still shares");
    CHECK(albedoOf(box2) == fbxAlbedo && fs::path(albedoOf(childNamed(inst, "Box"))).filename() == "x.png",
          "clearing the override restores the model's texture (correct baseline), the template is untouched");

#ifdef NDEBUG
    // Spec test 10: Core cost of Instantiate of a skinned prefab (GPU registration
    // is measured separately with the runtime harness).
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) s.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("prefab_tests: Instantiate skinned prefab: %.3f ms/call\n", ms / 100.0);
#endif
}

// A source saved without "clips" must not end the comparison: the sources after it
// still decide (they used to be ignored, so [rig, idle] "matched" [rig, other]).
void testMatchChecksEverySource()
{
    DonTopo::SkinnedMesh m;
    m.animationSources = { { "rig.fbx", true, {} }, { "idle.fbx", false, {} } };
    const nlohmann::json same  = nlohmann::json::array({ { {"path", "rig.fbx"}, {"builtin", true} },
                                                         { {"path", "idle.fbx"}, {"builtin", false} } });
    const nlohmann::json other = nlohmann::json::array({ { {"path", "rig.fbx"}, {"builtin", true} },
                                                         { {"path", "run.fbx"}, {"builtin", false} } });
    CHECK(DonTopo::meshMatchesAnimationConfig(m, same), "identical lists match");
    CHECK(!DonTopo::meshMatchesAnimationConfig(m, other), "a different second source does not match");
}

// A streamed clip in a scene is created ONCE, with its final key: no throwaway
// decoded Sample copy first (nodeFromJson used to create the default sound and
// then reload it for loadMode and rolloff).
void testSceneStreamClipCreatesOneSound()
{
    if (!audio().available()) { std::printf("SKIP testSceneStreamClipCreatesOneSound (FMOD not available)\n"); return; }
    namespace fs = std::filesystem;
    TempRoot root("dt_prefab_scene_stream");
    const fs::path clip = root.p / "music.mp3";
    fs::copy_file("assets/audio.mp3", clip);
    nlohmann::json node = plainNode(1, "Music");
    node["audioClip"] = { {"path", clip.string()}, {"is3D", false}, {"loop", true},
                          {"loadMode", "stream"}, {"rolloff", "linear"} };
    const nlohmann::json scene = { {"version", 1}, {"name", "S"},
                                   {"root", { {"name", "Root"}, {"localTransform", mat4Json(glm::mat4(1.0f))},
                                              {"children", nlohmann::json::array({ node })} }} };
    const int before = audio().soundCreateCount();
    {
        DonTopo::Scene s;
        CHECK(s.fromJson(scene, physics(), audio()), "scene with a streamed clip loads");
        DonTopo::GameObject* n = s.getRoot().children.empty() ? nullptr : s.getRoot().children[0].get();
        CHECK(n && n->hasAudioClip() && n->getAudioClip()->getLoadMode() == DonTopo::AudioLoadMode::Stream &&
              n->getAudioClip()->getRolloff() == DonTopo::AudioRolloff::Linear, "clip with its mode and rolloff");
        CHECK(audio().soundCreateCount() - before == 1, "exactly one FMOD sound created for it");
        s.shutdown();
    }
}

// A clip whose file is missing. FMOD opens sounds NONBLOCKING, so createSound
// succeeds and the failure arrives later, on the sound itself: the (failed) sound
// is cached under its key and held by the template, so no Instantiate asks FMOD
// or the disk for it again. (A SYNCHRONOUS refusal, which no file provokes here,
// fails the template instead -- PrefabLibrary::finishIfDone.)
void testLibraryMissingAudioIsNotRetried()
{
    if (!audio().available()) { std::printf("SKIP testLibraryMissingAudioIsNotRetried (FMOD not available)\n"); return; }
    TempRoot root("dt_prefab_lib_audio_missing");
    nlohmann::json p = plainNode(1, "P");
    p["audioClip"] = { {"path", (root.p / "nope.mp3").string()}, {"is3D", false}, {"loop", false} };
    writePrefabFile(root.p / "Prefabs" / "P.prefab", p);
    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request("Prefabs/P.prefab");
    const auto* t = readyTemplate(lib, "Prefabs/P.prefab");
    if (!t) return;
    CHECK(t->warmAudio.size() == 1, "the template holds the clip of the missing file");
    DonTopo::Scene s;
    const int created = audio().soundCreateCount();
    for (int i = 0; i < 3; ++i) s.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
    CHECK(audio().soundCreateCount() == created, "no Instantiate retries the sound");
}

// Lua side: a ScriptManager on the shared managers, with the library plugged in.
// The scene is a member declared AFTER the manager, so it dies first: its script
// instances are Lua references that must be released while the lua_State lives.
struct LuaFixture
{
    DonTopo::ScriptManager sm;
    DonTopo::Scene s;
    std::vector<std::string> log;
    explicit LuaFixture(DonTopo::PrefabLibrary* lib)
    {
        sm.init("__prefab_tests_no_scripts_folder__");
        sm.setScene(&s);
        sm.setPhysicsManager(&physics());
        sm.setAudioManager(&audio());
        sm.setPrefabLibrary(lib);
        sm.setLogCallback([this](const std::string& m) { log.push_back(m); });
    }
    bool load(const TempRoot& root, const char* cls, const std::string& code)
    {
        const auto file = root.p / (std::string(cls) + ".lua");
        writeFile(file, code);
        return sm.loadScript(file);
    }
    // Runs code; returns "" on success or the error message.
    std::string run(const std::string& code)
    {
        auto r = sm.lua().safe_script(code, sol::script_pass_on_error);
        if (r.valid()) return {};
        sol::error e = r;
        return e.what();
    }
};

// Review Focus 4: an Awake on an object created by Instantiate calls Instantiate
// itself, parenting the new object under its own entity (so the outer subtree grows
// while it is being woken). Collect-first means no iterator is invalidated and the
// nested object is woken exactly once, by its own Instantiate.
void testInstantiateAwakeReentrancy()
{
    TempRoot root("dt_prefab_inst_awake");
    nlohmann::json a = plainNode(1, "A");
    a["scripts"] = nlohmann::json::array({ { {"name", "Spawner"},
                                             {"overrides", { {"other", { {"prefab", "Prefabs/B.prefab"} }} }} } });
    nlohmann::json kid = plainNode(2, "Kid");
    kid["scripts"] = nlohmann::json::array({ { {"name", "Kid"} } });
    a["children"].push_back(kid);
    writePrefabFile(root.p / "Prefabs" / "A.prefab", a);
    nlohmann::json b = plainNode(1, "B");
    b["scripts"] = nlohmann::json::array({ { {"name", "Bee"} } });
    writePrefabFile(root.p / "Prefabs" / "B.prefab", b);

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request("Prefabs/A.prefab");   // B comes through A's reference (recursion)
    if (!readyTemplate(lib, "Prefabs/A.prefab") || !readyTemplate(lib, "Prefabs/B.prefab")) return;

    LuaFixture lua(&lib);
    DonTopo::Scene& s = lua.s;
    CHECK(lua.load(root, "Spawner",
                   "Spawner = { other = Prefab() }\n"
                   "function Spawner:Awake()\n"
                   "  SpawnerAwakes = (SpawnerAwakes or 0) + 1\n"
                   "  Nested = Instantiate(self.other, self.entity)\n"
                   "end\n"), "Spawner loads");
    CHECK(lua.load(root, "Kid", "Kid = {}\nfunction Kid:Awake() KidAwakes = (KidAwakes or 0) + 1 end\n"),
          "Kid loads");
    CHECK(lua.load(root, "Bee", "Bee = {}\nfunction Bee:Awake() BeeAwakes = (BeeAwakes or 0) + 1 end\n"),
          "Bee loads");
    lua.sm.lua()["pa"] = DonTopo::LuaPrefab{ "Prefabs/A.prefab" };
    lua.log.clear();   // the "registered" lines of the three loads

    const std::string err = lua.run("Outer = Instantiate(pa)\nLoaded = pa:IsLoaded()\n");
    CHECK(err.empty(), "Instantiate from Lua succeeds");
    if (!err.empty()) std::fprintf(stderr, "  error: %s\n", err.c_str());
    for (const auto& l : lua.log) std::fprintf(stderr, "  log: %s\n", l.c_str());
    CHECK(lua.log.empty(), "no Awake error logged");
    auto num = [&](const char* g) { sol::object o = lua.sm.lua()[g]; return o.is<int>() ? o.as<int>() : -1; };
    CHECK(num("SpawnerAwakes") == 1 && num("KidAwakes") == 1, "the outer instance woke once");
    CHECK(num("BeeAwakes") == 1, "the nested instance woke exactly once");
    CHECK(lua.sm.lua()["Loaded"].get<bool>(), "IsLoaded is true for a Ready template");

    DonTopo::GameObject* outer = nullptr;
    for (auto& c : s.getRoot().children) if (c->name == "A") outer = c.get();
    CHECK(outer && outer->children.size() == 2 && childNamed(outer, "B") && childNamed(outer, "Kid"),
          "B was created under A by A's Awake");
    CHECK(outer && outer->prefabLink && childNamed(outer, "B") && childNamed(outer, "B")->prefabLink &&
          childNamed(outer, "B")->prefabLink->path == "Prefabs/B.prefab", "both linked to their prefab");
    lua.sm.setPrefabLibrary(nullptr);
}

// The four Lua overloads and the argument errors.
void testInstantiateLuaOverloads()
{
    TempRoot root("dt_prefab_inst_lua");
    writePrefabFile(root.p / "Prefabs" / "P.prefab", plainNode(1, "P"));
    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request("Prefabs/P.prefab");
    if (!readyTemplate(lib, "Prefabs/P.prefab")) return;

    LuaFixture lua(&lib);
    DonTopo::Scene& s = lua.s;
    DonTopo::GameObject* holder = s.addGameObject("Holder");
    holder->localTransform = glm::translate(glm::mat4(1.0f), {10.0f, 0.0f, 0.0f});
    holder->updateWorldTransforms(glm::mat4(1.0f));
    lua.sm.rebuildAliveSet();
    lua.sm.lua()["p"] = DonTopo::LuaPrefab{ "Prefabs/P.prefab" };
    lua.sm.lua()["holder"] = DonTopo::LuaEntity{ holder, &lua.sm };

    CHECK(lua.run("e1 = Instantiate(p)\n"
                  "e2 = Instantiate(p, holder)\n"
                  "e3 = Instantiate(p, Vec3(1, 2, 3), Vec3(0, 90, 0))\n"
                  "e4 = Instantiate(p, Vec3(1, 2, 3), Vec3(0, 0, 0), holder)\n"
                  "e5 = Instantiate(p, nil)\n"
                  "e6 = Instantiate(p, Vec3(1, 2, 3), Vec3(0, 0, 0), nil)\n").empty(), "the overloads run");
    auto go = [&](const char* g) -> DonTopo::GameObject* {
        sol::object o = lua.sm.lua()[g];
        return o.is<DonTopo::LuaEntity>() ? o.as<DonTopo::LuaEntity>().go : nullptr;
    };
    CHECK(go("e1") && go("e1")->parent == &s.getRoot(), "(prefab) -> scene root");
    CHECK(go("e2") && go("e2")->parent == holder, "(prefab, parent)");
    CHECK(go("e3") && glm::length(glm::vec3(go("e3")->worldTransform[3]) - glm::vec3(1, 2, 3)) < 1e-4f,
          "(prefab, position, rotation)");
    CHECK(go("e4") && go("e4")->parent == holder &&
          glm::length(glm::vec3(go("e4")->worldTransform[3]) - glm::vec3(1, 2, 3)) < 1e-4f,
          "(prefab, position, rotation, parent): world position");
    CHECK(go("e5") && go("e5")->parent == &s.getRoot(), "a trailing nil parent is no parent");
    CHECK(go("e6") && go("e6")->parent == &s.getRoot(), "a trailing nil parent after a pose is no parent");

    // Wrong types: a clear Lua error, never a crash.
    auto errOf = [&](const char* call) { return lua.run(std::string("Instantiate(") + call + ")"); };
    const char* bad[] = { "p, 5", "p, Vec3(1, 2, 3)", "p, holder, Vec3(0, 0, 0)", "p, Vec3(), 'x'",
                          "p, Vec3(), Vec3(), 7", "p, Vec3(), Vec3(), holder, holder", "holder", "42",
                          "p, Vec3(0/0, 0, 0), Vec3()" };
    for (const char* b : bad)
    {
        const std::string e = errOf(b);
        CHECK(e.find("Instantiate: ") != std::string::npos, "bad arguments give an Instantiate error");
        if (e.find("Instantiate: ") == std::string::npos) std::fprintf(stderr, "  Instantiate(%s): '%s'\n", b, e.c_str());
    }
    CHECK(errOf("holder").find("Scene.Instantiate") != std::string::npos,
          "an Entity as first argument points at Scene.Instantiate");

    // Scene.Instantiate (shares the Awake block) still clones.
    CHECK(lua.run("c = Scene.Instantiate(e1)").empty() && go("c") && go("c") != go("e1"),
          "Scene.Instantiate still clones");
    lua.sm.setPrefabLibrary(nullptr);
}

// ---- Keyed arrays (Z1): scripts and materials recorded per element ----

// ov {} (null in an initializer list) = no overrides: an empty object, as nodeToJson writes it.
nlohmann::json script(const std::string& name, const nlohmann::json& ov)
{
    return { {"name", name}, {"overrides", ov.is_null() ? nlohmann::json::object() : ov} };
}

nlohmann::json nodeWith(const nlohmann::json& scripts, const nlohmann::json& materials = nullptr)
{
    nlohmann::json n = { {"id", 2}, {"name", "Lamp"}, {"children", nlohmann::json::array()} };
    if (!scripts.is_null()) n["scripts"] = scripts;
    if (!materials.is_null())
    {
        n["mesh"] = { {"sourcePath", "m.fbx"}, {"name", "m"} };
        if (!materials.empty()) n["mesh"]["materials"] = materials;
    }
    return n;
}

bool hasOv(const std::vector<DonTopo::PrefabOverride>& ovs, const std::string& path, const nlohmann::json& value)
{
    for (const auto& o : ovs) if (o.path == path && o.value == value) return true;
    return false;
}

void dumpOvs(const std::vector<DonTopo::PrefabOverride>& ovs)
{
    for (const auto& o : ovs) std::fprintf(stderr, "    %s = %s\n", o.path.c_str(), o.value.dump().c_str());
}

void testKeyedArrayKeysAndValueAt()
{
    using DonTopo::prefabArrayElementKey;
    const nlohmann::json s = nlohmann::json::array({ script("Mover", {}), script("Spin", {}), script("Mover", {}) });
    CHECK(DonTopo::isKeyedArrayPath("/scripts") && DonTopo::isKeyedArrayPath("/mesh/materials"), "keyed roots");
    CHECK(!DonTopo::isKeyedArrayPath("/mesh/animationSources") && !DonTopo::isKeyedArrayPath("/light/color"),
          "other arrays are whole");
    CHECK(prefabArrayElementKey("/scripts", s, 0) == "Mover#0" && prefabArrayElementKey("/scripts", s, 1) == "Spin#0" &&
          prefabArrayElementKey("/scripts", s, 2) == "Mover#1", "scripts: name + occurrence");
    const nlohmann::json m = nlohmann::json::array({ { {"index", 3}, {"metallic", 0.5} } });
    CHECK(prefabArrayElementKey("/mesh/materials", m, 0) == "3", "materials: the submesh index");

    const nlohmann::json n = nodeWith(nlohmann::json::array({ script("Mover", { {"speed", 3} }), script("Spin", {}) }),
                                      m);
    auto v = DonTopo::prefabValueAt(n, "/scripts/Mover#0/overrides/speed");
    CHECK(v && *v == 3, "a script property by key");
    CHECK(!DonTopo::prefabValueAt(n, "/scripts/Mover#1"), "a missing occurrence");
    v = DonTopo::prefabValueAt(n, "/scripts/#order");
    CHECK(v && *v == nlohmann::json::array({ "Mover#0", "Spin#0" }), "#order: the keys in order");
    v = DonTopo::prefabValueAt(n, "/mesh/materials/3/metallic");
    CHECK(v && *v == 0.5, "a material field by submesh index");
    CHECK(!DonTopo::prefabValueAt(n, "/mesh/materials/0/metallic"), "a slot with no entry has no field");
    v = DonTopo::prefabValueAt(nodeWith(nullptr), "/scripts");
    CHECK(v && v->is_array() && v->empty(), "an absent keyed array reads as []");
    v = DonTopo::prefabValueAt(n, "/name");
    CHECK(v && *v == "Lamp", "plain paths as before");
}

// Z1 at the model level: one element / one field per entry.
void testKeyedDiff()
{
    const nlohmann::json tmpl = nodeWith(nlohmann::json::array({
        script("Mover", { {"speed", 3}, {"label", "a"} }), script("Spin", { {"rate", 1} }) }));

    // One property changed: one entry, not the whole array.
    auto d = DonTopo::diffPrefabNode(2, tmpl, nodeWith(nlohmann::json::array({
        script("Mover", { {"speed", 3}, {"label", "b"} }), script("Spin", { {"rate", 1} }) })), false);
    CHECK(d.size() == 1 && hasOv(d, "/scripts/Mover#0/overrides/label", "b"), "a script property: one leaf entry");
    if (d.size() != 1) dumpOvs(d);

    // A property back to its default (absent): a removal of that property.
    d = DonTopo::diffPrefabNode(2, tmpl, nodeWith(nlohmann::json::array({
        script("Mover", { {"speed", 3} }), script("Spin", { {"rate", 1} }) })), false);
    CHECK(d.size() == 1 && hasOv(d, "/scripts/Mover#0/overrides/label", nullptr), "a property removed");

    // Added script in front, template script removed: no positional noise.
    d = DonTopo::diffPrefabNode(2, tmpl, nodeWith(nlohmann::json::array({
        script("Extra", { {"x", 1} }), script("Mover", { {"speed", 3}, {"label", "a"} }) })), false);
    CHECK(hasOv(d, "/scripts/Extra#0", script("Extra", { {"x", 1} })), "an added script: the whole element");
    CHECK(hasOv(d, "/scripts/Spin#0", nullptr), "a removed template script: null");
    CHECK(hasOv(d, "/scripts/#order", nlohmann::json::array({ "Extra#0", "Mover#0" })),
          "added in front: the order is recorded");
    CHECK(d.size() == 3, "and nothing else");
    if (d.size() != 3) dumpOvs(d);

    // Added at the end: the natural order, no #order.
    d = DonTopo::diffPrefabNode(2, tmpl, nodeWith(nlohmann::json::array({
        script("Mover", { {"speed", 3}, {"label", "a"} }), script("Spin", { {"rate", 1} }), script("Extra", {}) })), false);
    CHECK(d.size() == 1 && hasOv(d, "/scripts/Extra#0", script("Extra", {})), "appended: no order entry");

    // Reordered only.
    d = DonTopo::diffPrefabNode(2, tmpl, nodeWith(nlohmann::json::array({
        script("Spin", { {"rate", 1} }), script("Mover", { {"speed", 3}, {"label", "a"} }) })), false);
    CHECK(d.size() == 1 && hasOv(d, "/scripts/#order", nlohmann::json::array({ "Spin#0", "Mover#0" })),
          "a reorder is one #order entry");

    // No scripts on either side, or the same: nothing.
    CHECK(DonTopo::diffPrefabNode(2, tmpl, tmpl, false).empty(), "equal: nothing");
    CHECK(DonTopo::diffPrefabNode(2, nodeWith(nullptr), nodeWith(nullptr), false).empty(), "none: nothing");
    // All scripts removed (the key absent on the instance).
    d = DonTopo::diffPrefabNode(2, tmpl, nodeWith(nullptr), false);
    CHECK(d.size() == 2 && hasOv(d, "/scripts/Mover#0", nullptr) && hasOv(d, "/scripts/Spin#0", nullptr),
          "no scripts key: each template script removed");

    // Materials: sparse, keyed by submesh index, field-level.
    const nlohmann::json mt = nodeWith(nullptr, nlohmann::json::array({ { {"index", 0}, {"metallic", 0.5} } }));
    d = DonTopo::diffPrefabNode(2, mt, nodeWith(nullptr, nlohmann::json::array({
        { {"index", 0}, {"metallic", 0.5}, {"roughness", 0.25} }, { {"index", 1}, {"roughness", 0.75} } })), false);
    CHECK(d.size() == 2 && hasOv(d, "/mesh/materials/0/roughness", 0.25) && hasOv(d, "/mesh/materials/1/roughness", 0.75),
          "materials: one entry per field, keyed by slot (a new slot needs no whole entry)");
    if (d.size() != 2) dumpOvs(d);
    d = DonTopo::diffPrefabNode(2, mt, nodeWith(nullptr, nlohmann::json::array()), false);
    CHECK(d.size() == 1 && hasOv(d, "/mesh/materials/0/metallic", nullptr), "no materials key: the field removed");
    // Order of the sparse entries is irrelevant.
    const nlohmann::json two = nodeWith(nullptr, nlohmann::json::array({ { {"index", 0}, {"metallic", 0.5} },
                                                                          { {"index", 1}, {"roughness", 0.5} } }));
    const nlohmann::json swapped = nodeWith(nullptr, nlohmann::json::array({ { {"index", 1}, {"roughness", 0.5} },
                                                                              { {"index", 0}, {"metallic", 0.5} } }));
    CHECK(DonTopo::diffPrefabNode(2, two, swapped, false).empty(), "materials in another order: nothing");
}

// Applying keyed entries reproduces the instance; per-element entries leave
// every other element following the template.
void testKeyedApply()
{
    auto root = [](const nlohmann::json& node) {
        nlohmann::json r = { {"id", 1}, {"name", "Enemy"}, {"children", nlohmann::json::array({ node })} };
        return r;
    };
    const nlohmann::json tmplNode = nodeWith(nlohmann::json::array({
        script("Mover", { {"speed", 3}, {"label", "a"} }), script("Spin", { {"rate", 1} }) }),
        nlohmann::json::array({ { {"index", 0}, {"metallic", 0.5} } }));
    const std::vector<nlohmann::json> instances = {
        nodeWith(nlohmann::json::array({ script("Mover", { {"speed", 3}, {"label", "b"} }), script("Spin", { {"rate", 1} }) }),
                 nlohmann::json::array({ { {"index", 0}, {"metallic", 0.5} }, { {"index", 1}, {"roughness", 0.25} } })),
        nodeWith(nlohmann::json::array({ script("Extra", { {"x", 1} }), script("Mover", { {"speed", 3}, {"label", "a"} }) }),
                 nlohmann::json::array()),
        nodeWith(nlohmann::json::array({ script("Spin", { {"rate", 2} }), script("Mover", {}), script("Mover", { {"q", 1} }) }),
                 nlohmann::json::array({ { {"index", 0}, {"metallic", 0.75} } })),
        nodeWith(nullptr, nlohmann::json::array({ { {"index", 0}, {"metallic", 0.5} } })),
    };
    for (size_t k = 0; k < instances.size(); ++k)
    {
        const auto d = DonTopo::diffPrefabNode(2, tmplNode, instances[k], false);
        nlohmann::json tree = root(tmplNode);
        std::vector<std::string> warnings;
        const auto applied = DonTopo::applyPrefabOverrides(tree, d, &warnings);
        CHECK(applied.size() == d.size() && warnings.empty(), "every entry applies");
        CHECK(DonTopo::diffPrefabNode(2, tree["children"][0], instances[k], false).empty(),
              "the patched template equals the instance (round trip)");
        if (!DonTopo::diffPrefabNode(2, tree["children"][0], instances[k], false).empty())
            std::fprintf(stderr, "  instance %zu: %s\n", k, tree["children"][0].dump().c_str());
    }

    // Review Focus 2 for arrays: the template changes speed; an instance that
    // overrides only the label gets the new speed.
    nlohmann::json newTmpl = tmplNode;
    newTmpl["scripts"][0]["overrides"]["speed"] = 9;
    newTmpl["mesh"]["materials"] = nlohmann::json::array({ { {"index", 0}, {"metallic", 0.5} },
                                                           { {"index", 1}, {"roughness", 0.9} } });
    const auto labelOnly = DonTopo::diffPrefabNode(2, tmplNode, instances[0], false);
    nlohmann::json tree = root(newTmpl);
    DonTopo::applyPrefabOverrides(tree, labelOnly, nullptr);
    const nlohmann::json& mover = tree["children"][0]["scripts"][0];
    CHECK(mover["overrides"]["speed"] == 9 && mover["overrides"]["label"] == "b", "new speed, own label");

    // Keys are resolved against the template array BEFORE the entries: the
    // removal of Mover#0 does not turn Mover#1 into Mover#0.
    nlohmann::json twoMovers = root(nodeWith(nlohmann::json::array({ script("Mover", { {"n", 0} }),
                                                                     script("Mover", { {"n", 1} }) })));
    DonTopo::applyPrefabOverrides(twoMovers, { {2, "/scripts/Mover#0", nullptr}, {2, "/scripts/Mover#1/overrides/n", 7} },
                                  nullptr);
    CHECK(twoMovers["children"][0]["scripts"].size() == 1 &&
          twoMovers["children"][0]["scripts"][0]["overrides"]["n"] == 7, "keys do not shift mid-way");

    // Orphans: a property of a script the prefab no longer has.
    nlohmann::json t2 = root(tmplNode);
    std::vector<std::string> w;
    const auto a = DonTopo::applyPrefabOverrides(t2, { {2, "/scripts/Gone#0/overrides/x", 1} }, &w);
    CHECK(a.empty() && w.size() == 1, "an orphan script property is dropped with a warning");
    // A material field on a node without a mesh: orphan too.
    nlohmann::json t3 = root(nodeWith(nlohmann::json::array()));
    w.clear();
    CHECK(DonTopo::applyPrefabOverrides(t3, { {2, "/mesh/materials/0/metallic", 0.1} }, &w).empty() && w.size() == 1,
          "a material field without a mesh is an orphan");
    // Removing the last field of a slot removes the slot, and the empty array
    // is not written (as nodeToJson).
    nlohmann::json t4 = root(tmplNode);
    DonTopo::applyPrefabOverrides(t4, { {2, "/mesh/materials/0/metallic", nullptr} }, nullptr);
    CHECK(!t4["children"][0]["mesh"].contains("materials"), "no slot left: no materials key");
}

// Backward compatibility: a whole-array override of the old format applies
// as before, and comes back converted to per-element entries.
void testLegacyWholeArrayOverride()
{
    nlohmann::json tmplNode = nodeWith(nlohmann::json::array({
        script("Mover", { {"speed", 3}, {"label", "a"} }), script("Spin", { {"rate", 1} }) }),
        nlohmann::json::array({ { {"index", 0}, {"metallic", 0.5} } }));
    nlohmann::json tree = { {"id", 1}, {"name", "Enemy"}, {"children", nlohmann::json::array({ tmplNode })} };
    const nlohmann::json legacyScripts = nlohmann::json::array({ script("Mover", { {"speed", 9}, {"label", "a"} }),
                                                                 script("Spin", { {"rate", 1} }) });
    const nlohmann::json legacyMats = nlohmann::json::array({ { {"index", 0}, {"metallic", 0.5}, {"roughness", 0.2} } });
    std::vector<std::string> w;
    const auto applied = DonTopo::applyPrefabOverrides(tree, { {2, "/scripts", legacyScripts},
                                                                {2, "/mesh/materials", legacyMats} }, &w);
    const nlohmann::json& n = tree["children"][0];
    CHECK(n["scripts"] == legacyScripts && n["mesh"]["materials"] == legacyMats, "applied exactly as before");
    CHECK(applied.size() == 2 && hasOv(applied, "/scripts/Mover#0/overrides/speed", 9) &&
          hasOv(applied, "/mesh/materials/0/roughness", 0.2), "returned as per-element entries");
    if (applied.size() != 2) dumpOvs(applied);
    CHECK(w.empty(), "no warning");
    // Old null = the instance had no scripts at all.
    nlohmann::json tree2 = { {"id", 1}, {"name", "Enemy"}, {"children", nlohmann::json::array({ tmplNode })} };
    const auto removed = DonTopo::applyPrefabOverrides(tree2, { {2, "/scripts", nullptr} }, nullptr);
    CHECK(!tree2["children"][0].contains("scripts") && removed.size() == 2 &&
          hasOv(removed, "/scripts/Mover#0", nullptr), "an old whole removal becomes per-script removals");
}
// AA3: the KNOWN limitation of same-named scripts (spec 3.5), pinned so that
// changing it is a deliberate decision. Keys are the occurrence numbering of
// each side, not a stable per-script id.
void testDuplicateScriptNamesLimitation()
{
    auto root = [](const nlohmann::json& scripts) {
        return nlohmann::json{ {"id", 1}, {"name", "Enemy"}, {"children", nlohmann::json::array({ nodeWith(scripts) })} };
    };
    // 1. Removing the FIRST of two "Mover"s reads as "Mover#1 removed" plus
    //    Mover#0 taking the second one's fields.
    const nlohmann::json two = nodeWith(nlohmann::json::array({ script("Mover", { {"n", 0} }), script("Mover", { {"n", 1} }) }));
    auto d = DonTopo::diffPrefabNode(2, two, nodeWith(nlohmann::json::array({ script("Mover", { {"n", 1} }) })), false);
    CHECK(d.size() == 2 && hasOv(d, "/scripts/Mover#0/overrides/n", 1) && hasOv(d, "/scripts/Mover#1", nullptr),
          "removing the first of two: Mover#1 removed + Mover#0 = the second one's fields");

    // 2. The template inserts a same-named script in front: the instance's
    //    "Mover#0" entry now lands on the INSERTED script.
    nlohmann::json tree = root(nlohmann::json::array({ script("Mover", { {"n", 9} }), script("Mover", { {"n", 0} }) }));
    DonTopo::applyPrefabOverrides(tree, { {2, "/scripts/Mover#0/overrides/n", 5} }, nullptr);
    CHECK(tree["children"][0]["scripts"][0]["overrides"]["n"] == 5 && tree["children"][0]["scripts"][1]["overrides"]["n"] == 0,
          "keys shift with a template insert: the entry lands on the first Mover");

    // 3. An added "Extra#0" meets a later template "Extra": the instance's
    //    whole element replaces the template's (its fields are not merged).
    tree = root(nlohmann::json::array({ script("Extra", { {"y", 2} }) }));
    DonTopo::applyPrefabOverrides(tree, { {2, "/scripts/Extra#0", script("Extra", { {"x", 1} })} }, nullptr);
    CHECK(tree["children"][0]["scripts"].size() == 1 &&
          tree["children"][0]["scripts"][0]["overrides"] == nlohmann::json({ {"x", 1} }),
          "an added Extra#0 replaces a later template Extra whole");

    // 4. The template removes a same-named script: the entry for the second
    //    one becomes an orphan (dropped with a warning).
    tree = root(nlohmann::json::array({ script("Mover", { {"n", 0} }) }));
    std::vector<std::string> w;
    CHECK(DonTopo::applyPrefabOverrides(tree, { {2, "/scripts/Mover#1/overrides/n", 3} }, &w).empty() && w.size() == 1,
          "Mover#1 gone from the template: an orphan, warned");
}

// AA2 at the model level: an old whole-array entry and an explicit entry on
// a path it also covers (a fold while the template loaded) come back as ONE
// entry per path, the explicit value winning -- and a whole-element entry
// plus a field inside it come back as one canonical entry.
void testKeyedGroupReturnsCanonicalEntries()
{
    const nlohmann::json tmplNode = nodeWith(nlohmann::json::array({ script("Mover", { {"speed", 3}, {"label", "a"} }) }));
    auto fresh = [&] { return nlohmann::json{ {"id", 1}, {"name", "Enemy"}, {"children", nlohmann::json::array({ tmplNode })} }; };
    const nlohmann::json legacy = nlohmann::json::array({ script("Mover", { {"speed", 9}, {"label", "a"} }) });

    nlohmann::json tree = fresh();
    auto a = DonTopo::applyPrefabOverrides(tree, { {2, "/scripts", legacy}, {2, "/scripts/Mover#0/overrides/speed", 12} }, nullptr);
    CHECK(a.size() == 1 && hasOv(a, "/scripts/Mover#0/overrides/speed", 12), "one entry per path, the explicit value");
    if (a.size() != 1) dumpOvs(a);
    CHECK(tree["children"][0]["scripts"][0]["overrides"]["speed"] == 12, "and that is what is applied");

    tree = fresh();
    a = DonTopo::applyPrefabOverrides(tree, { {2, "/scripts/Extra#0", script("Extra", { {"x", 1} })},
                                              {2, "/scripts/Extra#0/overrides/x", 2} }, nullptr);
    CHECK(a.size() == 1 && hasOv(a, "/scripts/Extra#0", script("Extra", { {"x", 2} })),
          "a whole element and a field inside it: one entry, the merged element");
    if (a.size() != 1) dumpOvs(a);

    // An entry equal to the template is not an override: not returned.
    tree = fresh();
    a = DonTopo::applyPrefabOverrides(tree, { {2, "/scripts/Mover#0/overrides/speed", 3} }, nullptr);
    CHECK(a.empty(), "an entry equal to the template is dropped");
}
} // namespace

int main()
{
    testDuplicateScriptNamesLimitation();
    testKeyedGroupReturnsCanonicalEntries();
    testKeyedArrayKeysAndValueAt();
    testKeyedDiff();
    testKeyedApply();
    testLegacyWholeArrayOverride();
    testMatCacheAvoidsDisk();
    testResolveStoreInverse();
    testResolveMatchesNodeFromJson();
    testMakePrefabRenumbersAndLinks();
    testLinkFieldsRoundTrip();
    testScenesWithoutPrefabsByteIdentical();
    testMalformedPrefabLoadsUnlinked();
    testCloneKeepsLinkWithFreshIds();
    testDiffDetectsLeafChange();
    testDiffIgnoresIdsAndRootPose();
    testDiffSmallRotationAndQuatSign();
    testDiffComponentAddedAndRemoved();
    testApplyRoundTripAndOrphans();
    testOverridesJsonRoundTrip();
    testPropagationRespectsOverrides();
    testPropagationKeepsIdsAndAddsNew();
    testAddedChildSurvivesPropagation();
    testOrphanOverrideDropped();
    testRecomputeRemovesRevertedValue();
    testUnpack();
    testTemplateMissingDoesNotWipeOverrides();
    testRebuildFailureRestoresOldSubtree();
    testRebuildReleasesOldRootOnce();
    testRootPoseKeptScaleFollowsPrefab();
    testClonedInstanceChildBecomesAddedChild();
    testDuplicatedLinkedNodeKeptAsAddedChild();
    testRootWithoutPrefabNodeWarns();
    testCollectReferences();
    testLibraryRequestIsIdempotent();
    testLibraryFailedStates();
    testLibraryRecursiveReferences();
    testLibraryStaleResultDropped();
    testLibraryReRequestAfterRetainOnly();
    testLibraryUpdateFromMemory();
    testLibraryLoadsTemplate();
    testLibraryDestroyedWithJobsInFlight();
    testLibraryRetainOnlyCancelsMeshLoads();
    testLibraryClearInOnReadyStopsRecursion();
    testLibraryNormalizeKeyRootCase();
    testLibraryRefusedMeshJobFails();
    testLibraryUpdateFromMemoryReuseAndAsync();
    testLibraryWarmsAudio();
    testLibraryUpdateReconfiguresAnimationSources(/*keepRaw=*/true);
    testLibraryUpdateReconfiguresAnimationSources(/*keepRaw=*/false);
    testInstantiateRoundTrip();
    testInstantiateIdsUniqueAcrossFourPaths();
    testInstantiateNoIo();
    testMatchChecksEverySource();
    testSceneStreamClipCreatesOneSound();
    testLibraryMissingAudioIsNotRetried();
    testInstantiateAwakeReentrancy();
    testInstantiateLuaOverloads();
    if (g_failures) { std::fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    std::printf("prefab_tests OK\n");
    return 0;
}
