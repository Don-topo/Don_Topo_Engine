// Tests of the prefab system (spec docs/superpowers/specs/2026-09-30-prefabs-design.md).
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/Core/MaterialAsset.h"
#include "DonTopo/Core/PrefabOverrides.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Renderer/Material.h"
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
    if (g_failures) { std::fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    std::printf("prefab_tests OK\n");
    return 0;
}
