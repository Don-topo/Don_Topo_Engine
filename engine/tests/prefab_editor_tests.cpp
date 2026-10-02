// Editor-side prefab tests (spec docs/superpowers/specs/2026-09-30-prefabs-design.md):
// the override tracker (§3.2, Review Focus 1) and load-time propagation (§3.3,
// spec test 2), against a real PrefabLibrary.
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/Core/PrefabLibrary.h"
#include "DonTopo/Core/PrefabOverrides.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Editor/PrefabOverrideTracker.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMesh.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
    ++g_failures; } } while (0)

// One PhysicsManager for the whole file: PxFoundation is unique per process.
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

const char* kEnemyKey = "Prefabs/Enemy.prefab";

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

// Enemy (1) -> Lamp (2, point light). The .prefab FILE json (disk form), made by
// makePrefab from a real subtree so every default field nodeToJson writes is in it.
nlohmann::json enemyFile(const std::string& assetRoot, float range, float intensity = 1.0f)
{
    DonTopo::Scene s;
    s.setAssetRoot(assetRoot);
    auto* enemy = s.addGameObject("Enemy");
    auto* lamp  = s.addGameObject("Lamp", enemy);
    auto light = std::make_shared<DonTopo::LightComponent>();
    light->setIntensity(intensity);
    light->setRange(range);
    lamp->setLight(light);
    return s.makePrefab(enemy, kEnemyKey, /*link=*/false);
}

nlohmann::json memoryRootOf(const nlohmann::json& file, const std::string& assetRoot)
{
    nlohmann::json root = file["root"];
    DonTopo::resolveStoredPaths(root, assetRoot);
    return root;
}

// An instance built by hand from a template root, as a scene file stores it:
// prefabNode = local id, fresh scene ids, link on the root.
DonTopo::GameObject* makeInstance(DonTopo::Scene& scene, const nlohmann::json& memoryRoot)
{
    nlohmann::json j = memoryRoot;
    std::function<void(nlohmann::json&)> link = [&](nlohmann::json& n) {
        n["prefabNode"] = n["id"];
        n.erase("id");
        for (auto& c : n["children"]) link(c);
    };
    link(j);
    j["prefab"] = { {"path", kEnemyKey}, {"overrides", nlohmann::json::array()} };
    return scene.insertFromJson(j, nullptr, SIZE_MAX, physics(), audio());
}

DonTopo::GameObject* childNamed(DonTopo::GameObject* n, const std::string& name)
{
    if (!n) return nullptr;
    for (auto& c : n->children) if (c->name == name) return c.get();
    return nullptr;
}

DonTopo::LightComponent* lampLight(DonTopo::GameObject* instance)
{
    DonTopo::GameObject* lamp = childNamed(instance, "Lamp");
    return lamp ? lamp->getLight().get() : nullptr;
}

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

bool hasOverride(const DonTopo::GameObject* root, const std::string& path)
{
    if (!root || !root->prefabLink) return false;
    for (const auto& o : root->prefabLink->overrides) if (o.path == path) return true;
    return false;
}

const DonTopo::PrefabTemplate* readyTemplate(DonTopo::PrefabLibrary& lib, const std::string& key)
{
    const DonTopo::PrefabTemplate* t = lib.find(key);
    if (!t || t->state != DonTopo::PrefabTemplate::State::Ready)
    {
        if (t) std::fprintf(stderr, "  template '%s' not Ready: %s\n", key.c_str(), t->error.c_str());
        return nullptr;
    }
    return t;
}
}

// Review Focus 1, first half: a template that was never pumped (Loading, no
// content) or a Ready one the instance was never synced with -> no recompute,
// the stored list stays. Once synced, an edit IS detected.
void testTrackerSkipsUnsyncedInstances()
{
    TempRoot root("dt_prefab_editor_unsynced");
    const nlohmann::json file = enemyFile(root.str(), 10.0f);
    writeFile(root.p / kEnemyKey, file.dump(2));

    DonTopo::Scene scene;
    DonTopo::GameObject* inst = makeInstance(scene, memoryRootOf(file, root.str()));
    if (!inst || !lampLight(inst)) { CHECK(false, "instance built"); return; }
    lampLight(inst)->setIntensity(5.0f);
    inst->prefabLink->overrides = { {2, "/light/intensity", 5.0} };

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    DonTopo::PrefabOverrideTracker tracker;

    CHECK(tracker.onRevisionChanged(scene, lib) == 0, "never requested template: nothing recomputed");
    lib.request(kEnemyKey);   // Loading, never pumped
    lampLight(inst)->setRange(30.0f);
    CHECK(tracker.onRevisionChanged(scene, lib) == 0, "Loading template: nothing recomputed");
    CHECK(tracker.onItemDeactivated(scene, lib, childNamed(inst, "Lamp")) == 0,
          "Loading template: the item-deactivated edge recomputes nothing either");
    CHECK(inst->prefabLink->overrides.size() == 1 && hasOverride(inst, "/light/intensity"),
          "the stored override is untouched");
    CHECK(tracker.unsyncedInstances(scene, lib).empty(), "a Loading template has no instance to sync");

    CHECK(settle(lib), "library settles");
    const DonTopo::PrefabTemplate* t = readyTemplate(lib, kEnemyKey);
    if (!t) { CHECK(false, "template Ready"); return; }
    CHECK(tracker.onRevisionChanged(scene, lib) == 0, "Ready but never synced: nothing recomputed");
    CHECK(inst->prefabLink->overrides.size() == 1, "still the stored list");
    const auto unsynced = tracker.unsyncedInstances(scene, lib);
    CHECK(unsynced.size() == 1 && unsynced[0] == inst, "the instance is reported as unsynced");

    tracker.markSynced(inst->id, t->generation);
    CHECK(tracker.unsyncedInstances(scene, lib).empty(), "synced instances are not reported");
    CHECK(tracker.onRevisionChanged(scene, lib) == 1, "synced: the range edit is detected");
    CHECK(inst->prefabLink->overrides.size() == 2 && hasOverride(inst, "/light/range"),
          "the edit is merged into the list");

    lampLight(inst)->setIntensity(1.0f);   // back to the prefab value
    CHECK(tracker.onItemDeactivated(scene, lib, childNamed(inst, "Lamp")) == 1,
          "the item-deactivated edge recomputes the selected instance");
    CHECK(inst->prefabLink->overrides.size() == 1 && !hasOverride(inst, "/light/intensity"),
          "a value back to the prefab's drops its override");
    CHECK(tracker.onItemDeactivated(scene, lib, nullptr) == 0, "no selection: nothing to do");
}

// Review Focus 1, the case only the Ready check catches: updateFromMemory with a
// mesh the old version did not have leaves the template Loading with the NEW
// memoryRoot already in place and the OLD generation (the one the instance is
// synced with). Recomputing then would store "range 10" as a user override.
void testTrackerSkipsTemplateReloading()
{
    TempRoot root("dt_prefab_editor_reloading");
    const nlohmann::json file = enemyFile(root.str(), 10.0f);
    writeFile(root.p / kEnemyKey, file.dump(2));
    const std::filesystem::path tri = root.p / "Models" / "tri.obj";
    writeFile(tri, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request(kEnemyKey);
    CHECK(settle(lib), "library settles");
    const DonTopo::PrefabTemplate* t = readyTemplate(lib, kEnemyKey);
    if (!t) { CHECK(false, "template Ready"); return; }

    DonTopo::Scene scene;
    DonTopo::GameObject* inst = scene.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
    if (!inst) { CHECK(false, "instantiated"); return; }
    DonTopo::PrefabOverrideTracker tracker;
    tracker.markSynced(inst->id, t->generation);
    const uint64_t oldGeneration = t->generation;

    // New version: range 20 and a mesh on the root that must come from the loader.
    nlohmann::json newFile = enemyFile(root.str(), 20.0f);
    newFile["root"]["mesh"] = { {"sourcePath", "Models/tri.obj"}, {"name", "tri"} };
    lib.updateFromMemory(kEnemyKey, newFile, audio());   // not pumped: stays Loading
    t = lib.find(kEnemyKey);
    CHECK(t && t->state == DonTopo::PrefabTemplate::State::Loading, "the reload is in flight");
    CHECK(t && t->generation == oldGeneration, "the generation only moves when it is Ready");
    CHECK(t && t->memoryRoot.is_object() &&
          t->memoryRoot["children"][0]["light"]["range"].get<double>() == 20.0,
          "memoryRoot already holds the new content");

    CHECK(tracker.onRevisionChanged(scene, lib) == 0, "a reloading template is never diffed against");
    CHECK(inst->prefabLink->overrides.empty(), "no override invented from the prefab change");
    CHECK(tracker.onItemDeactivated(scene, lib, inst) == 0, "neither from the item-deactivated edge");
    CHECK(inst->prefabLink->overrides.empty(), "still no override");
    CHECK(!tracker.isSynced(inst, lib), "not synced while the template reloads");
    CHECK(settle(lib), "reload settles");
}

// A template that changed (updateFromMemory -> new generation) is not diffed
// until the instance was propagated to it; after the propagation, edits are
// detected again and the prefab change is NOT among the overrides.
void testTrackerIgnoresInstanceUntilPropagated()
{
    TempRoot root("dt_prefab_editor_generation");
    const nlohmann::json file = enemyFile(root.str(), 10.0f);
    writeFile(root.p / kEnemyKey, file.dump(2));

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request(kEnemyKey);
    CHECK(settle(lib), "library settles");
    const DonTopo::PrefabTemplate* t = readyTemplate(lib, kEnemyKey);
    if (!t) { CHECK(false, "template Ready"); return; }

    DonTopo::Scene scene;
    DonTopo::GameObject* inst = scene.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
    if (!inst) { CHECK(false, "instantiated"); return; }
    DonTopo::PrefabOverrideTracker tracker;
    tracker.markSynced(inst->id, t->generation);
    const uint64_t g1 = t->generation;

    lib.updateFromMemory(kEnemyKey, enemyFile(root.str(), 20.0f), audio());   // no meshes: Ready at once
    t = readyTemplate(lib, kEnemyKey);
    if (!t) { CHECK(false, "updated template Ready"); return; }
    CHECK(t->generation != g1, "the update bumped the generation");
    CHECK(tracker.onRevisionChanged(scene, lib) == 0, "not propagated to the new generation: ignored");
    CHECK(inst->prefabLink->overrides.empty(), "range 10 vs 20 is not recorded as an override");
    const auto unsynced = tracker.unsyncedInstances(scene, lib);
    CHECK(unsynced.size() == 1 && unsynced[0] == inst, "the instance waits for its propagation");

    int registered = 0;
    const uint64_t id = inst->id;
    const DonTopo::PropagationResult r = DonTopo::propagateReadyTemplate(
        scene, *t, tracker, physics(), audio(), [&](DonTopo::GameObject*) { ++registered; });
    inst = scene.findById(id);
    CHECK(r.rebuilt == 1 && registered == 1, "propagated once");
    CHECK(inst && lampLight(inst) && lampLight(inst)->getRange() == 20.0f, "the instance took range 20");
    CHECK(tracker.isSynced(inst, lib), "propagation marks it synced");

    lampLight(inst)->setIntensity(3.0f);
    CHECK(tracker.onRevisionChanged(scene, lib) == 1, "after propagating, edits are detected");
    CHECK(inst->prefabLink->overrides.size() == 1 && hasOverride(inst, "/light/intensity"),
          "only the user's edit is an override");

    tracker.forget(inst->id);
    CHECK(!tracker.isSynced(inst, lib), "forget drops the sync");
}

// Spec test 2, second half: a scene saved with an older prefab is propagated
// when the template becomes Ready; the stored overrides are kept, scene ids are
// kept, and an instance already up to date is left untouched.
void testLoadPropagatesNewerPrefab()
{
    TempRoot root("dt_prefab_editor_load");
    const nlohmann::json oldFile = enemyFile(root.str(), 10.0f);

    // The scene as it was saved: instance of the OLD prefab (range 10) with an
    // intensity override.
    nlohmann::json sceneJson;
    {
        DonTopo::Scene s0;
        s0.setAssetRoot(root.str());
        DonTopo::GameObject* inst = makeInstance(s0, memoryRootOf(oldFile, root.str()));
        if (!inst || !lampLight(inst)) { CHECK(false, "instance built"); return; }
        lampLight(inst)->setIntensity(5.0f);
        CHECK(s0.recomputePrefabOverrides(inst, memoryRootOf(oldFile, root.str())), "override recorded");
        sceneJson = s0.toJson();
    }
    // ...and the prefab on disk is newer: range 20.
    writeFile(root.p / kEnemyKey, enemyFile(root.str(), 20.0f).dump(2));

    DonTopo::Scene scene;
    scene.setAssetRoot(root.str());
    CHECK(scene.fromJson(sceneJson, physics(), audio()), "scene loads");
    DonTopo::GameObject* inst = scene.getRoot().children.empty() ? nullptr : scene.getRoot().children[0].get();
    if (!inst || !inst->prefabLink) { CHECK(false, "loaded instance"); return; }
    const uint64_t rootId = inst->id;
    const uint64_t lampId = childNamed(inst, "Lamp") ? childNamed(inst, "Lamp")->id : 0;
    CHECK(lampLight(inst) && lampLight(inst)->getRange() == 10.0f, "the stored subtree shows first");

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    for (const std::string& k : DonTopo::PrefabLibrary::collectReferences(sceneJson)) lib.request(k);
    lib.request(inst->prefabLink->path);
    CHECK(settle(lib), "library settles");
    const DonTopo::PrefabTemplate* t = readyTemplate(lib, kEnemyKey);
    if (!t) { CHECK(false, "template Ready"); return; }

    DonTopo::PrefabOverrideTracker tracker;
    std::vector<DonTopo::GameObject*> registered;
    DonTopo::PropagationResult r = DonTopo::propagateReadyTemplate(
        scene, *t, tracker, physics(), audio(), [&](DonTopo::GameObject* g) { registered.push_back(g); });
    inst = scene.findById(rootId);
    CHECK(r.rebuilt == 1 && r.upToDate == 0, "the older instance was rebuilt");
    CHECK(registered.size() == 1 && registered[0] == inst, "the new subtree goes to the GPU once");
    CHECK(inst && lampLight(inst) && lampLight(inst)->getRange() == 20.0f, "range 20 from the newer prefab");
    CHECK(inst && lampLight(inst) && lampLight(inst)->getIntensity() == 5.0f, "the override survives");
    CHECK(inst && inst->prefabLink && inst->prefabLink->overrides.size() == 1 &&
          hasOverride(inst, "/light/intensity"), "overrides unchanged");
    CHECK(inst && childNamed(inst, "Lamp") && childNamed(inst, "Lamp")->id == lampId, "scene ids kept");
    CHECK(inst && DonTopo::prefabInstanceMatchesTemplate(scene, inst, t->memoryRoot),
          "after propagating, the instance matches its template");
    CHECK(tracker.onRevisionChanged(scene, lib) == 0, "a propagated instance has nothing new to record");

    // Up to date: a second pass leaves the very same objects alone.
    DonTopo::GameObject* before = inst;
    registered.clear();
    r = DonTopo::propagateReadyTemplate(scene, *t, tracker, physics(), audio(),
                                        [&](DonTopo::GameObject* g) { registered.push_back(g); });
    CHECK(r.rebuilt == 0 && r.upToDate == 1 && registered.empty(), "an up-to-date instance is not rebuilt");
    CHECK(scene.findById(rootId) == before, "same GameObject, same GPU slots");

    // A node still waiting for its mesh from the async loader (Load Scene):
    // serialized now it would have no mesh, so the instance is left for later.
    DonTopo::PrefabOverrideTracker fresh;
    DonTopo::GameObject* lamp = childNamed(before, "Lamp");
    if (!lamp) { CHECK(false, "lamp"); return; }
    lamp->pendingMeshJob = 7;
    lampLight(before)->setRange(99.0f);   // stale against the template
    registered.clear();
    r = DonTopo::propagateReadyTemplate(scene, *t, fresh, physics(), audio(),
                                        [&](DonTopo::GameObject* g) { registered.push_back(g); });
    CHECK(r.deferred == 1 && r.rebuilt == 0 && registered.empty(), "an instance with a pending mesh is deferred");
    CHECK(scene.findById(rootId) == before && lampLight(before)->getRange() == 99.0f, "and left untouched");
    CHECK(!fresh.isSynced(before, lib), "a deferred instance is not synced");
    lamp->pendingMeshJob = 0;
    r = DonTopo::propagateReadyTemplate(scene, *t, fresh, physics(), audio(),
                                        [&](DonTopo::GameObject* g) { registered.push_back(g); });
    inst = scene.findById(rootId);
    CHECK(r.rebuilt == 1 && inst && lampLight(inst) && lampLight(inst)->getRange() == 20.0f,
          "once the mesh arrived it is propagated");
}

// Carried over from Task 8: propagation must be seeded with the template's
// per-node configured meshes. Two nodes on the same rig with DIFFERENT
// animation-source lists: only the first list is in t.meshes, so seeding with
// t.meshes alone makes the second node re-read its external source (Assimp) on
// the main thread at every propagation.
void testPropagationSeededWithInstanceMeshes()
{
    namespace fs = std::filesystem;
    TempRoot root("dt_prefab_editor_seed");
    const fs::path maw  = root.p / "Models" / "Maw J Laygo.fbx";
    const fs::path idle = root.p / "Models" / "standing idle 01.fbx";
    fs::copy_file("assets/animatedCharacter/Maw J Laygo.fbx", maw);
    fs::copy_file("assets/animatedCharacter/standing idle 01.fbx", idle);

    auto node = [](uint64_t id, const char* name) {
        nlohmann::json n = { {"id", id}, {"name", name}, {"children", nlohmann::json::array()} };
        nlohmann::json m = nlohmann::json::array();
        const glm::mat4 identity(1.0f);
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) m.push_back(identity[c][r]);
        n["localTransform"] = m;
        return n;
    };
    nlohmann::json group = node(1, "Group");
    nlohmann::json a = node(2, "A");
    a["mesh"] = { {"sourcePath", maw.string()}, {"name", "Maw"}, {"skinned", true},
                  {"animationSources", nlohmann::json::array({ { {"path", maw.string()}, {"builtin", true} } })} };
    nlohmann::json b = node(3, "B");
    b["mesh"] = { {"sourcePath", maw.string()}, {"name", "Maw"}, {"skinned", true},
                  {"animationSources", nlohmann::json::array({
                      { {"path", maw.string()},  {"builtin", true} },
                      { {"path", idle.string()}, {"builtin", false} } })} };
    group["children"].push_back(a);
    group["children"].push_back(b);
    writeFile(root.p / "Prefabs" / "Group.prefab",
              nlohmann::json{ {"version", 1}, {"type", "prefab"}, {"root", group} }.dump(2));

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.setKeepRawMeshes(true);   // as the editor does
    lib.request("Prefabs/Group.prefab");
    CHECK(settle(lib), "library settles");
    const DonTopo::PrefabTemplate* t = readyTemplate(lib, "Prefabs/Group.prefab");
    if (!t) { CHECK(false, "template Ready"); return; }

    DonTopo::Scene scene;
    DonTopo::GameObject* inst = scene.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
    if (!inst || inst->children.size() != 2) { CHECK(false, "instantiated"); return; }
    // An edit that is not in the stored list: the instance no longer matches its
    // template, so the propagation has to rebuild it.
    inst->children[0]->name = "Renamed";
    const uint64_t id = inst->id;

    DonTopo::PrefabOverrideTracker tracker;
    const int readsBefore = DonTopo::ModelLoader::readFileCount();
    const DonTopo::PropagationResult r = DonTopo::propagateReadyTemplate(
        scene, *t, tracker, physics(), audio(), [](DonTopo::GameObject*) {});
    const int reads = DonTopo::ModelLoader::readFileCount() - readsBefore;
    inst = scene.findById(id);
    CHECK(r.rebuilt == 1, "rebuilt");
    CHECK(reads == 0, "a propagation reads no FBX (configured meshes seeded)");
    if (reads != 0) std::fprintf(stderr, "  Assimp reads during propagation: %d\n", reads);
    DonTopo::GameObject* nb = inst ? childNamed(inst, "B") : nullptr;
    const DonTopo::SkinnedMesh* sk = nb ? nb->getSkinnedMesh() : nullptr;
    CHECK(sk && sk->animationSources.size() == 2, "B keeps its two animation sources");
    const auto cfg = t->configuredMeshes.find(DonTopo::prefabNodeMeshRef(3));
    CHECK(cfg != t->configuredMeshes.end() && nb && nb->getMesh() == cfg->second,
          "B shares the template's configured mesh");
    CHECK(inst && childNamed(inst, "A") != nullptr, "the rename is reverted (it was no override)");
}

int main()
{
    testTrackerSkipsUnsyncedInstances();
    testTrackerSkipsTemplateReloading();
    testTrackerIgnoresInstanceUntilPropagated();
    testLoadPropagatesNewerPrefab();
    testPropagationSeededWithInstanceMeshes();
    if (g_failures) { std::fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    std::printf("prefab_editor_tests OK\n");
    return 0;
}
