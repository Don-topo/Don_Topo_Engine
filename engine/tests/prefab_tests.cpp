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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (lib->readCount() < 1 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));   // read result posted, worker free
        lib->pump(audio(), 0.0f);                                      // P's mesh job submitted
        std::this_thread::sleep_for(std::chrono::milliseconds(20));   // and started (the FBX takes far longer)
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (lib.readCount() < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::atomic<bool> release{ false };
    js.submit([&release] { while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));   // the blocker holds the worker
    lib.pump(audio(), 0.0f);                                       // mesh request queued behind it
    CHECK(lib.meshLoadsPending() == 1, "mesh request queued");
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
void testLibraryUpdateReconfiguresAnimationSources()
{
    namespace fs = std::filesystem;
    TempRoot root("dt_prefab_lib_anim_update");
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
    CHECK(settle(lib), "library settles");
    CHECK(exactly(swapped), "[builtin, idle] -> [builtin, run]: no stale idle clips");

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
} // namespace

int main()
{
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
    testLibraryUpdateReconfiguresAnimationSources();
    if (g_failures) { std::fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    std::printf("prefab_tests OK\n");
    return 0;
}
