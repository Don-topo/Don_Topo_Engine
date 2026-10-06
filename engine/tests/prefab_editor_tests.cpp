// Editor-side prefab tests (spec docs/superpowers/specs/2026-09-30-prefabs-design.md):
// the override tracker (§3.2, Review Focus 1) and load-time propagation (§3.3,
// spec test 2), against a real PrefabLibrary.
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/Core/MaterialAsset.h"
#include "DonTopo/Core/PrefabLibrary.h"
#include "DonTopo/Core/PrefabOverrides.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Editor/EditorUI.h"
#include "DonTopo/Editor/PrefabOverrideTracker.h"
#include "DonTopo/Editor/PrefabSceneSync.h"
#include "DonTopo/Editor/ProjectContext.h"
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/Material.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMesh.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
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
        scene, lib, *t, tracker, physics(), audio(), [&](DonTopo::GameObject*) { ++registered; });
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
        scene, lib, *t, tracker, physics(), audio(), [&](DonTopo::GameObject* g) { registered.push_back(g); });
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
    r = DonTopo::propagateReadyTemplate(scene, lib, *t, tracker, physics(), audio(),
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
    r = DonTopo::propagateReadyTemplate(scene, lib, *t, fresh, physics(), audio(),
                                        [&](DonTopo::GameObject* g) { registered.push_back(g); });
    CHECK(r.deferred == 1 && r.rebuilt == 0 && registered.empty(), "an instance with a pending mesh is deferred");
    CHECK(scene.findById(rootId) == before && lampLight(before)->getRange() == 99.0f, "and left untouched");
    CHECK(!fresh.isSynced(before, lib), "a deferred instance is not synced");
    lamp->pendingMeshJob = 0;
    r = DonTopo::propagateReadyTemplate(scene, lib, *t, fresh, physics(), audio(),
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
    fs::create_directories(root.p / "Materials");
    DonTopo::MaterialAsset matAsset;
    matAsset.roughness = 0.25f;
    CHECK(DonTopo::saveMaterialAsset(root.p / "Materials" / "m.mat", matAsset), ".mat written");

    nlohmann::json group = node(1, "Group");
    nlohmann::json a = node(2, "A");
    // A: primary source list + a .mat material override (configured mesh, then
    // the rebuild's editMesh copy for the material).
    a["mesh"] = { {"sourcePath", maw.string()}, {"name", "Maw"}, {"skinned", true},
                  {"animationSources", nlohmann::json::array({ { {"path", maw.string()}, {"builtin", true} } })},
                  {"materials", nlohmann::json::array({ { {"index", 0}, {"matAsset", "Materials/m.mat"} } })} };
    nlohmann::json b = node(3, "B");
    b["mesh"] = { {"sourcePath", maw.string()}, {"name", "Maw"}, {"skinned", true},
                  {"animationSources", nlohmann::json::array({
                      { {"path", maw.string()},  {"builtin", true} },
                      { {"path", idle.string()}, {"builtin", false} } })} };
    group["children"].push_back(a);
    group["children"].push_back(b);
    const nlohmann::json file = { {"version", 1}, {"type", "prefab"}, {"root", group} };
    writeFile(root.p / "Prefabs" / "Group.prefab", file.dump(2));

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
    const uint64_t id = inst->id;
    DonTopo::PrefabOverrideTracker tracker;
    tracker.captureBaseline(scene, inst);   // as a scene load / sync does
    // An edit the tracker could not record yet (template about to change):
    inst->children[0]->name = "Renamed";

    // The prefab changes (a light added under the root), so the instance has to
    // be rebuilt. No new mesh: Ready at once, or after settling the reconfig.
    nlohmann::json newFile = file;
    nlohmann::json lamp = node(4, "Lamp");
    lamp["light"] = { {"type", "point"}, {"intensity", 1.0}, {"range", 10.0} };
    newFile["root"]["children"].push_back(lamp);
    lib.updateFromMemory("Prefabs/Group.prefab", newFile, audio());
    CHECK(settle(lib), "updated template settles");
    t = readyTemplate(lib, "Prefabs/Group.prefab");
    if (!t) { CHECK(false, "updated template Ready"); return; }
    // Nothing on disk is needed any more: the .mat goes too.
    std::error_code ec;
    fs::remove_all(root.p / "Materials", ec);

    const int readsBefore = DonTopo::ModelLoader::readFileCount();
    const DonTopo::PropagationResult r = DonTopo::propagateReadyTemplate(
        scene, lib, *t, tracker, physics(), audio(), [](DonTopo::GameObject*) {});
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
    CHECK(inst && childNamed(inst, "Lamp") != nullptr, "the new prefab child arrived");

    // The unrecorded rename is KEPT, as an override (spec §3.1: the user's edit).
    DonTopo::GameObject* na = inst ? childNamed(inst, "Renamed") : nullptr;
    CHECK(na != nullptr, "the rename made before the propagation survives it");
    CHECK(hasOverride(inst, "/name"), "and is now a stored override");
    // A: configured mesh + .mat from the template cache (the file is gone).
    const auto mats = na ? DonTopo::materialsOfMesh(*na) : std::vector<const DonTopo::Material*>{};
    CHECK(!mats.empty() && mats[0]->roughness == 0.25f, "A's .mat override applied from the template cache");
    const auto cfgA = t->configuredMeshes.find(DonTopo::prefabNodeMeshRef(2));
    CHECK(cfgA != t->configuredMeshes.end() && na && na->getMesh() != cfgA->second,
          "A's material override was applied on a copy, the configured mesh stays shared");
}

// Finding 6: integers compare exactly; only a float on either side gets the
// 1e-5 tolerance.
void testValuesEqualIntegersExact()
{
    using nlohmann::json;
    CHECK(!DonTopo::prefabValuesEqual(json(0xFFFFFFFFu), json(0xFFFFFFFEu)), "32-bit masks differ");
    CHECK(!DonTopo::prefabValuesEqual(json(uint64_t(1234567890123456789ull)), json(uint64_t(1234567890123456788ull))),
          "64-bit ids differ");
    CHECK(!DonTopo::prefabValuesEqual(json(-5), json(-6)), "signed ints differ");
    CHECK(DonTopo::prefabValuesEqual(json(7), json(7u)), "same value, signed vs unsigned");
    CHECK(!DonTopo::prefabValuesEqual(json(-1), json(uint64_t(0xFFFFFFFFFFFFFFFFull))), "-1 is not 2^64-1");
    CHECK(DonTopo::prefabValuesEqual(json(1.0), json(1.000001)), "floats within 1e-5");
    CHECK(DonTopo::prefabValuesEqual(json(3), json(3.000001)), "int vs float uses the tolerance");
    CHECK(!DonTopo::prefabValuesEqual(json(1.0), json(1.1)), "floats beyond the tolerance");
    CHECK(!DonTopo::prefabValuesEqual(json::array({4294967295u}), json::array({4294967294u})),
          "inside arrays too");
}

// Finding 7: link paths are matched with the library's normalization, the same
// one the template key went through.
void testPropagationMatchesNormalizedLinks()
{
    TempRoot root("dt_prefab_editor_normalize");
    const nlohmann::json oldFile = enemyFile(root.str(), 10.0f);
    writeFile(root.p / kEnemyKey, enemyFile(root.str(), 20.0f).dump(2));

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    lib.request(kEnemyKey);
    CHECK(settle(lib), "library settles");
    const DonTopo::PrefabTemplate* t = readyTemplate(lib, kEnemyKey);
    if (!t) { CHECK(false, "template Ready"); return; }

    DonTopo::Scene scene;
    DonTopo::GameObject* a = makeInstance(scene, memoryRootOf(oldFile, root.str()));
    DonTopo::GameObject* b = makeInstance(scene, memoryRootOf(oldFile, root.str()));
    if (!a || !b) { CHECK(false, "instances"); return; }
    a->prefabLink->path = "Prefabs\\Enemy.prefab";
    b->prefabLink->path = (root.p / "Prefabs" / "Enemy.prefab").string();
    const uint64_t ida = a->id, idb = b->id;
    DonTopo::PrefabOverrideTracker tracker;
    const DonTopo::PropagationResult r = DonTopo::propagateReadyTemplate(
        scene, lib, *t, tracker, physics(), audio(), [](DonTopo::GameObject*) {});
    CHECK(r.rebuilt == 2, "a backslash and an absolute link both match the template key");
    CHECK(lampLight(scene.findById(ida)) && lampLight(scene.findById(ida))->getRange() == 20.0f, "backslash one updated");
    CHECK(lampLight(scene.findById(idb)) && lampLight(scene.findById(idb))->getRange() == 20.0f, "absolute one updated");
}

// ---- PrefabSceneSync (the EditorUI wiring, headless) ----

struct SyncFixture
{
    std::vector<std::string> log, pins, unpins;
    bool playing      = false;
    bool pinThrows    = false;
    bool requestLive  = true;
    int  loaderPend   = 0;
    int  registered   = 0;

    DonTopo::PrefabSceneSync::Host host(DonTopo::Scene& scene)
    {
        DonTopo::PrefabSceneSync::Host h;
        h.scene = &scene;
        h.physics = &physics();
        h.audio = &audio();
        h.isPlaying = [this] { return playing; };
        h.registerGpu = [this](DonTopo::GameObject*) { ++registered; };
        h.pin = [this](const std::string& key, const std::vector<std::shared_ptr<const DonTopo::Mesh>>&) {
            if (pinThrows) throw std::runtime_error("out of device memory");
            pins.push_back(key);
        };
        h.unpin = [this](const std::string& key) { unpins.push_back(key); };
        h.log = [this](const std::string& m) { log.push_back(m); };
        h.meshRequestLive = [this](uint64_t) { return requestLive; };
        h.loaderPending = [this] { return loaderPend; };
        return h;
    }
    bool logged(const std::string& text) const
    {
        for (const auto& l : log) if (l.find(text) != std::string::npos) return true;
        return false;
    }
    int count(const std::vector<std::string>& v, const std::string& k) const
    {
        int n = 0;
        for (const auto& s : v) if (s == k) ++n;
        return n;
    }
};

bool settleSync(DonTopo::PrefabSceneSync& sync, DonTopo::PrefabLibrary& lib, int timeoutMs = 180000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        sync.pump(1000.0f);
        if (lib.pending() == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// A scene file with one Enemy instance of the given stored range / intensity.
nlohmann::json sceneWithInstance(const std::string& assetRoot, float range, float intensity)
{
    DonTopo::Scene s0;
    s0.setAssetRoot(assetRoot);
    const nlohmann::json file = enemyFile(assetRoot, range);
    DonTopo::GameObject* inst = makeInstance(s0, memoryRootOf(file, assetRoot));
    if (inst && lampLight(inst) && intensity != 1.0f)
    {
        lampLight(inst)->setIntensity(intensity);
        s0.recomputePrefabOverrides(inst, memoryRootOf(file, assetRoot));
    }
    return s0.toJson();
}

DonTopo::GameObject* firstInstance(DonTopo::Scene& s)
{
    for (auto& c : s.getRoot().children) if (c->prefabLink) return c.get();
    return nullptr;
}

// Finding 2: an edit made while the template loads is folded into the overrides
// at the first propagation instead of being reverted; undo/redo stay consistent.
void testSyncKeepsEditMadeWhileLoading()
{
    TempRoot root("dt_prefab_editor_fold_load");
    writeFile(root.p / kEnemyKey, enemyFile(root.str(), 10.0f).dump(2));
    const nlohmann::json sceneJson = sceneWithInstance(root.str(), 10.0f, 1.0f);

    DonTopo::Scene scene;
    scene.setAssetRoot(root.str());
    CHECK(scene.fromJson(sceneJson, physics(), audio()), "scene loads");
    DonTopo::GameObject* inst = firstInstance(scene);
    if (!inst || !childNamed(inst, "Lamp")) { CHECK(false, "instance"); return; }
    const uint64_t rootId = inst->id;
    const uint64_t lampId = childNamed(inst, "Lamp")->id;

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    SyncFixture fx;
    DonTopo::PrefabSceneSync sync(lib);
    sync.setHost(fx.host(scene));
    sync.sceneLoaded(sceneJson);   // template requested: Loading
    CHECK(lib.pending() == 1, "the instance's prefab is requested");

    // The user edits through the undo stack while the template loads.
    DonTopo::UndoManager undo;
    auto setIntensity = [&](const float& v) {
        if (DonTopo::GameObject* l = scene.findById(lampId)) l->getLight()->setIntensity(v);
    };
    setIntensity(5.0f);
    undo.push(std::make_unique<DonTopo::PropertyCommand<float>>("Intensity", 1.0f, 5.0f, setIntensity));
    sync.frame(/*revisionChanged=*/true, nullptr);
    CHECK(!hasOverride(scene.findById(rootId), "/light/intensity"), "nothing recorded while Loading");

    CHECK(settleSync(sync, lib), "library settles");
    inst = scene.findById(rootId);
    CHECK(inst && lampLight(inst) && lampLight(inst)->getIntensity() == 5.0f,
          "the edit made while loading survives the propagation");
    CHECK(hasOverride(inst, "/light/intensity"), "and is now a stored override");
    CHECK(sync.tracker().isSynced(inst, lib), "the instance is synced");

    // Undo after the fold: the command restores 1 (the prefab value) by id, and
    // the next revision recompute drops the override. Redo brings both back.
    undo.undo();
    sync.frame(true, nullptr);
    inst = scene.findById(rootId);
    CHECK(inst && lampLight(inst)->getIntensity() == 1.0f && !hasOverride(inst, "/light/intensity"),
          "undo after the fold: value and override back to the prefab");
    undo.redo();
    sync.frame(true, nullptr);
    inst = scene.findById(rootId);
    CHECK(inst && lampLight(inst)->getIntensity() == 5.0f && hasOverride(inst, "/light/intensity"),
          "redo: the override is back");
}

// Finding 2, Apply case: another instance's Apply puts the template in Loading
// (updateFromMemory with a new mesh). An edit on THIS instance during that window
// is kept, and it still receives the prefab change.
void testSyncKeepsEditDuringApplyReload()
{
    TempRoot root("dt_prefab_editor_fold_apply");
    writeFile(root.p / kEnemyKey, enemyFile(root.str(), 10.0f).dump(2));
    writeFile(root.p / "Models" / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    DonTopo::Scene scene;
    scene.setAssetRoot(root.str());
    SyncFixture fx;
    DonTopo::PrefabSceneSync sync(lib);
    sync.setHost(fx.host(scene));
    lib.request(kEnemyKey);
    CHECK(settleSync(sync, lib), "library settles");
    const DonTopo::PrefabTemplate* t = readyTemplate(lib, kEnemyKey);
    if (!t) { CHECK(false, "template Ready"); return; }
    DonTopo::GameObject* a = scene.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
    DonTopo::GameObject* b = scene.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
    if (!a || !b) { CHECK(false, "instances"); return; }
    const uint64_t ida = a->id, idb = b->id;
    sync.sceneLoaded(scene.toJson());   // baselines + sync of both
    CHECK(sync.tracker().isSynced(a, lib) && sync.tracker().isSynced(b, lib), "both synced");

    nlohmann::json applied = enemyFile(root.str(), 20.0f);
    applied["root"]["mesh"] = { {"sourcePath", (root.p / "Models" / "tri.obj").string()}, {"name", "tri"} };
    lib.updateFromMemory(kEnemyKey, applied, audio());   // Loading: new mesh to load
    CHECK(lib.find(kEnemyKey)->state == DonTopo::PrefabTemplate::State::Loading, "reloading");

    lampLight(b)->setIntensity(3.0f);   // edit on the OTHER instance meanwhile
    sync.frame(true, nullptr);
    CHECK(!hasOverride(scene.findById(idb), "/light/intensity"), "not recordable while reloading");

    CHECK(settleSync(sync, lib), "reload settles");
    CHECK(readyTemplate(lib, kEnemyKey) != nullptr, "the applied template is Ready");
    a = scene.findById(ida);
    b = scene.findById(idb);
    CHECK(a && lampLight(a) && lampLight(a)->getRange() == 20.0f && a->hasMesh(), "A took the applied prefab");
    CHECK(a && a->prefabLink->overrides.empty(), "A has no override");
    CHECK(b && lampLight(b) && lampLight(b)->getRange() == 20.0f && b->hasMesh(), "B took the prefab change too");
    CHECK(b && lampLight(b)->getIntensity() == 3.0f && hasOverride(b, "/light/intensity"),
          "B keeps the edit made during the reload, as an override");
}

// Finding 1: a propagation deferred by an async mesh request is retried at most
// every kRetryIntervalFrames, and a request the loader no longer knows
// (cancelled) does not keep the instance deferred.
void testSyncDeferredRetryBoundedAndCancel()
{
    TempRoot root("dt_prefab_editor_deferred");
    writeFile(root.p / kEnemyKey, enemyFile(root.str(), 20.0f).dump(2));
    const nlohmann::json sceneJson = sceneWithInstance(root.str(), 10.0f, 1.0f);

    DonTopo::Scene scene;
    scene.setAssetRoot(root.str());
    CHECK(scene.fromJson(sceneJson, physics(), audio()), "scene loads");
    DonTopo::GameObject* inst = firstInstance(scene);
    if (!inst || !childNamed(inst, "Lamp")) { CHECK(false, "instance"); return; }
    const uint64_t rootId = inst->id;
    childNamed(inst, "Lamp")->pendingMeshJob = 42;   // as Load Scene leaves it

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    SyncFixture fx;
    DonTopo::PrefabSceneSync sync(lib);
    sync.setHost(fx.host(scene));
    sync.sceneLoaded(sceneJson);
    CHECK(settleSync(sync, lib), "library settles");
    CHECK(sync.retryPending(), "the propagation was deferred");
    CHECK(lampLight(scene.findById(rootId))->getRange() == 10.0f, "and left untouched");

    const int passesBefore = sync.syncPasses();
    for (int i = 0; i < 60; ++i) sync.frame(false, nullptr);
    const int passes = sync.syncPasses() - passesBefore;
    CHECK(passes >= 1 && passes <= 2, "60 frames: at most 2 retries, not one per frame");
    if (passes > 2) std::fprintf(stderr, "  retries in 60 frames: %d\n", passes);
    CHECK(lampLight(scene.findById(rootId))->getRange() == 10.0f, "still deferred while the request lives");

    // The loading modal's Cancel: the loader forgets the request; the node's
    // pendingMeshJob is still set (cancelAllPending cannot reach the scene).
    fx.requestLive = false;
    for (int i = 0; i < DonTopo::PrefabSceneSync::kRetryIntervalFrames + 1 && sync.retryPending(); ++i)
        sync.frame(false, nullptr);
    inst = scene.findById(rootId);
    CHECK(!sync.retryPending(), "no retry left");
    CHECK(inst && lampLight(inst) && lampLight(inst)->getRange() == 20.0f, "propagated once the request is dead");
    CHECK(inst && sync.tracker().isSynced(inst, lib), "and synced: its edits are recorded again");
}

// Findings 5, 11 and the load decisions: pin per Ready template (a pin that
// throws is logged, not propagated), eviction + unpin on a load in Edit Mode only,
// Failed keys re-read on the next load, propagation only in Edit Mode.
void testSyncLoadPinEvictDecisions()
{
    TempRoot root("dt_prefab_editor_decisions");
    writeFile(root.p / kEnemyKey, enemyFile(root.str(), 20.0f).dump(2));
    writeFile(root.p / "Prefabs" / "Other.prefab", enemyFile(root.str(), 5.0f).dump(2));
    auto refs = [](std::vector<std::string> keys) {
        nlohmann::json ov = nlohmann::json::object();
        int i = 0;
        for (const auto& k : keys) ov["p" + std::to_string(i++)] = { {"prefab", k} };
        return nlohmann::json{ {"version", 1}, {"root", { {"name", "Root"},
            {"scripts", nlohmann::json::array({ { {"name", "S"}, {"overrides", ov} } })} }} };
    };

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    DonTopo::Scene scene;
    scene.setAssetRoot(root.str());
    SyncFixture fx;
    DonTopo::PrefabSceneSync sync(lib);
    sync.setHost(fx.host(scene));

    sync.sceneLoaded(refs({ kEnemyKey, "Prefabs/Other.prefab", "Prefabs/Missing.prefab" }));
    CHECK(settleSync(sync, lib), "settles");
    sync.pump(0.0f);
    CHECK(fx.count(fx.pins, kEnemyKey) == 1 && fx.count(fx.pins, "Prefabs/Other.prefab") == 1, "both Ready pinned once");
    CHECK(fx.logged("Could not load 'Prefabs/Missing.prefab'"), "the Failed one is logged");
    const size_t logSize = fx.log.size();
    sync.pump(0.0f);
    CHECK(fx.log.size() == logSize, "logged once, not every frame");

    // A Lua loadScene in Play: nothing is evicted, nothing unpinned.
    fx.playing = true;
    sync.sceneLoaded(refs({ kEnemyKey }));
    CHECK(lib.find("Prefabs/Other.prefab") != nullptr && fx.unpins.empty(), "Play: no eviction");
    fx.playing = false;

    // Edit Mode: Other is evicted and unpinned; Missing (Failed) is read again.
    writeFile(root.p / "Prefabs" / "Missing.prefab", enemyFile(root.str(), 1.0f).dump(2));
    sync.sceneLoaded(refs({ kEnemyKey, "Prefabs/Missing.prefab" }));
    CHECK(lib.find("Prefabs/Other.prefab") == nullptr, "Edit: unreferenced template evicted");
    CHECK(fx.count(fx.unpins, "Prefabs/Other.prefab") == 1 && sync.pinned().count("Prefabs/Other.prefab") == 0,
          "and unpinned");
    CHECK(settleSync(sync, lib), "settles again");
    CHECK(readyTemplate(lib, "Prefabs/Missing.prefab") != nullptr, "the fixed .prefab loads on the next scene load");

    // A pin that throws (VRAM exhausted) is caught and logged.
    fx.pinThrows = true;
    bool threw = false;
    try
    {
        lib.updateFromMemory(kEnemyKey, enemyFile(root.str(), 30.0f), audio());
    }
    catch (...) { threw = true; }
    CHECK(!threw, "a throwing pin does not escape through the library");
    CHECK(fx.logged("could not pin 'Prefabs/Enemy.prefab'"), "it is logged");
    fx.pinThrows = false;

    // Propagation on load: only in Edit Mode.
    const nlohmann::json stale = sceneWithInstance(root.str(), 10.0f, 1.0f);
    DonTopo::Scene playScene;
    playScene.setAssetRoot(root.str());
    CHECK(playScene.fromJson(stale, physics(), audio()), "scene loads");
    sync.setHost(fx.host(playScene));
    fx.playing = true;
    sync.sceneLoaded(stale);
    CHECK(lampLight(firstInstance(playScene))->getRange() == 10.0f, "Play: the stored subtree is not propagated");
    fx.playing = false;
    sync.sceneLoaded(stale);   // the Stop restore (EditorUI leaves Play first)
    CHECK(lampLight(firstInstance(playScene))->getRange() == 30.0f, "Edit: propagated on load");
}

// Falling edge of an active item: the selection's instance is recomputed (edits
// that bypass the undo stack, like script properties).
void testSyncFallingEdgeRecomputesSelection()
{
    TempRoot root("dt_prefab_editor_edge");
    writeFile(root.p / kEnemyKey, enemyFile(root.str(), 10.0f).dump(2));
    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    DonTopo::Scene scene;
    scene.setAssetRoot(root.str());
    SyncFixture fx;
    DonTopo::PrefabSceneSync sync(lib);
    sync.setHost(fx.host(scene));
    lib.request(kEnemyKey);
    CHECK(settleSync(sync, lib), "settles");
    DonTopo::GameObject* inst = scene.instantiatePrefab(*lib.find(kEnemyKey), nullptr, nullptr, nullptr,
                                                        physics(), audio());
    if (!inst) { CHECK(false, "instance"); return; }
    sync.sceneLoaded(scene.toJson());
    lampLight(inst)->setRange(12.0f);   // no command
    sync.frame(false, nullptr);
    CHECK(!hasOverride(inst, "/light/range"), "no edge, no revision: nothing recomputed");
    sync.frame(false, childNamed(inst, "Lamp"));
    CHECK(hasOverride(inst, "/light/range"), "the falling edge recomputes the selected instance");
}

// Finding 1, loader side: a request is live until delivered or cancelled.
void testLoaderRequestLiveness()
{
    TempRoot root("dt_prefab_editor_loader");
    const std::string tri = (root.p / "Models" / "tri.obj").string();
    writeFile(tri, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::AsyncAssetLoader loader(js);
    CHECK(!loader.isRequestLive(0), "0 is never live");
    const auto cancelled = loader.requestMesh(tri, 1);
    CHECK(cancelled != 0 && loader.isRequestLive(cancelled), "a fresh request is live");
    loader.cancelAllPending();
    CHECK(!loader.isRequestLive(cancelled), "cancelAllPending kills it (the node's field is stale now)");

    const auto delivered = loader.requestMesh(tri, 2);
    CHECK(loader.isRequestLive(delivered), "live while in flight");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool got = false;
    while (!got && std::chrono::steady_clock::now() < deadline)
    {
        for (const auto& r : loader.pumpCompleted(1000.0f)) if (r.job == delivered) got = true;
        if (!got) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(got, "delivered");
    CHECK(!loader.isRequestLive(delivered), "not live once its result was handed out");
}

// Finding 4: releasePrefabLibrary is final.
void testEditorReleaseIsFinal()
{
    TempRoot root("dt_prefab_editor_release");
    DonTopo::JobSystem js;
    js.start(1);
    DonTopo::ProjectContext project(root.p);
    DonTopo::EditorUI editor;
    editor.setJobSystem(&js);
    editor.setProject(&project);
    CHECK(editor.prefabLibrary() != nullptr, "a project + a JobSystem create the library");
    editor.releasePrefabLibrary();
    CHECK(editor.prefabLibrary() == nullptr, "released");
    editor.setProject(&project);
    editor.setJobSystem(&js);
    CHECK(editor.prefabLibrary() == nullptr, "nothing re-creates it after the release");
    editor.releasePrefabLibrary();   // idempotent
}

// What EditorUI::onAssetsLoaded does through applyLoadedMesh, minus the GPU
// registration (no renderer headless): the mesh lands on its target by id.
int applyLoaded(DonTopo::AsyncAssetLoader& loader, DonTopo::Scene& scene)
{
    int applied = 0;
    for (auto& r : loader.pumpCompleted(1000.0f))
    {
        DonTopo::GameObject* target = scene.findById(r.targetId);
        if (!target) continue;
        target->pendingMeshJob = 0;
        if (!r.error.empty() || !r.mesh) continue;
        target->setMesh(r.mesh);
        DonTopo::applyMaterialOverrides(*target);
        ++applied;
    }
    return applied;
}

// N1: a scene opened with Load Scene (async meshes). The baselines are captured
// while the file meshes are still in flight; when they land and the deferred
// propagation runs, NOTHING may be folded: no override, no rebuild, and saving
// the untouched scene gives back the very same bytes.
void testAsyncLoadAddsNoSpuriousOverride()
{
    namespace fs = std::filesystem;
    TempRoot root("dt_prefab_editor_async_load");
    const fs::path maw = root.p / "Models" / "Maw J Laygo.fbx";
    fs::copy_file("assets/animatedCharacter/Maw J Laygo.fbx", maw);
    const std::string tri = (root.p / "Models" / "box.obj").string();
    writeFile(tri, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    const nlohmann::json identity = nlohmann::json::array({ 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0 });

    // The prefab, made by makePrefab from real loaded nodes: Enemy -> Box (.obj,
    // static, async on Load Scene) + Body (skinned FBX).
    nlohmann::json prefabFile;
    {
        DonTopo::Scene s0;
        s0.setAssetRoot(root.str());
        nlohmann::json enemy = { {"name", "Enemy"}, {"localTransform", identity}, {"children", nlohmann::json::array()} };
        enemy["children"].push_back({ {"name", "Box"}, {"localTransform", identity},
                                      {"mesh", { {"sourcePath", tri}, {"name", "box"} }},
                                      {"children", nlohmann::json::array()} });
        enemy["children"].push_back({ {"name", "Body"}, {"localTransform", identity},
                                      {"mesh", { {"sourcePath", maw.string()}, {"name", "Maw"}, {"skinned", true} }},
                                      {"children", nlohmann::json::array()} });
        DonTopo::GameObject* e = s0.insertFromJson(enemy, nullptr, SIZE_MAX, physics(), audio());
        if (!e || !childNamed(e, "Box") || !childNamed(e, "Box")->hasMesh()) { CHECK(false, "prefab source built"); return; }
        prefabFile = s0.makePrefab(e, kEnemyKey, /*link=*/false);
    }
    writeFile(root.p / kEnemyKey, prefabFile.dump(2));

    // The scene as saved: one untouched instance (meshes loaded synchronously).
    nlohmann::json sceneJson;
    {
        DonTopo::Scene s1;
        s1.setAssetRoot(root.str());
        DonTopo::GameObject* inst = makeInstance(s1, memoryRootOf(prefabFile, root.str()));
        if (!inst) { CHECK(false, "instance built"); return; }
        sceneJson = s1.toJson();
    }

    // Load Scene: async meshes.
    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::AsyncAssetLoader loader(js);
    DonTopo::Scene scene;
    scene.setAssetRoot(root.str());
    CHECK(scene.fromJson(sceneJson, physics(), audio(), &loader), "scene loads async");
    DonTopo::GameObject* inst = firstInstance(scene);
    if (!inst || !childNamed(inst, "Box")) { CHECK(false, "instance"); return; }
    CHECK(childNamed(inst, "Box")->pendingMeshJob != 0 && !childNamed(inst, "Box")->hasMesh(),
          "the .obj is in flight when the scene is opened");
    const uint64_t rootId = inst->id;

    DonTopo::PrefabLibrary lib(js, root.str());
    SyncFixture fx;
    fx.requestLive = true;
    DonTopo::PrefabSceneSync sync(lib);
    DonTopo::PrefabSceneSync::Host host = fx.host(scene);
    host.meshRequestLive = [&](uint64_t job) { return loader.isRequestLive(job); };
    host.loaderPending   = [&] { return loader.pending(); };
    sync.setHost(host);
    sync.sceneLoaded(sceneJson);   // baselines captured NOW, mesh still in flight
    CHECK(settleSync(sync, lib), "library settles");

    // The meshes land (EditorUI::onAssetsLoaded), then the deferred retry runs.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (loader.pending() > 0 && std::chrono::steady_clock::now() < deadline)
    {
        applyLoaded(loader, scene);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(loader.pending() == 0, "the async meshes landed");
    for (int i = 0; i < DonTopo::PrefabSceneSync::kRetryIntervalFrames + 2; ++i) sync.frame(false, nullptr);

    inst = scene.findById(rootId);
    CHECK(inst && sync.tracker().isSynced(inst, lib), "the instance got synced once its meshes landed");
    CHECK(inst && inst->prefabLink->overrides.empty(), "no override the user never made");
    if (inst && !inst->prefabLink->overrides.empty())
        for (const auto& o : inst->prefabLink->overrides)
            std::fprintf(stderr, "  spurious override: node %llu %s\n", (unsigned long long)o.node, o.path.c_str());
    CHECK(fx.registered == 0, "nothing rebuilt (no GPU re-registration)");
    CHECK(scene.toJson().dump() == sceneJson.dump(), "saving the untouched scene gives the same bytes");
}

// N2: an instance with no baseline yet (a clone made while its template loads)
// gets one at the revision change of the command that created it, so an edit
// made afterwards survives the first propagation.
void testCloneDuringLoadingKeepsEdit()
{
    TempRoot root("dt_prefab_editor_clone_loading");
    writeFile(root.p / kEnemyKey, enemyFile(root.str(), 10.0f).dump(2));
    const nlohmann::json sceneJson = sceneWithInstance(root.str(), 10.0f, 1.0f);
    DonTopo::Scene scene;
    scene.setAssetRoot(root.str());
    CHECK(scene.fromJson(sceneJson, physics(), audio()), "scene loads");
    DonTopo::GameObject* inst = firstInstance(scene);
    if (!inst) { CHECK(false, "instance"); return; }

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    SyncFixture fx;
    DonTopo::PrefabSceneSync sync(lib);
    sync.setHost(fx.host(scene));
    sync.sceneLoaded(sceneJson);   // Loading

    DonTopo::GameObject* clone = scene.cloneGameObject(inst, nullptr, physics(), audio());
    if (!clone || !clone->prefabLink) { CHECK(false, "linked clone"); return; }
    const uint64_t cloneId = clone->id;
    sync.frame(/*revisionChanged=*/true, nullptr);   // the clone command's own revision
    lampLight(clone)->setIntensity(4.0f);            // then the user edits it
    sync.frame(true, nullptr);

    CHECK(settleSync(sync, lib), "library settles");
    clone = scene.findById(cloneId);
    CHECK(clone && lampLight(clone) && lampLight(clone)->getIntensity() == 4.0f,
          "the edit on a clone made while loading survives");
    CHECK(clone && hasOverride(clone, "/light/intensity"), "as an override");
}

// N3: an override undone while the template reloads returns the field to the
// OLD prefab value. That is not an edit: the instance must follow the NEW
// prefab, not keep the old value pinned as an override.
void testRevertDuringReloadFollowsNewPrefab()
{
    TempRoot root("dt_prefab_editor_revert_reload");
    writeFile(root.p / kEnemyKey, enemyFile(root.str(), 10.0f, 1.0f).dump(2));
    const std::string tri = (root.p / "Models" / "tri.obj").string();
    writeFile(tri, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::PrefabLibrary lib(js, root.str());
    DonTopo::Scene scene;
    scene.setAssetRoot(root.str());
    SyncFixture fx;
    DonTopo::PrefabSceneSync sync(lib);
    sync.setHost(fx.host(scene));
    lib.request(kEnemyKey);
    CHECK(settleSync(sync, lib), "settles");
    DonTopo::GameObject* inst = scene.instantiatePrefab(*lib.find(kEnemyKey), nullptr, nullptr, nullptr,
                                                        physics(), audio());
    if (!inst) { CHECK(false, "instance"); return; }
    const uint64_t id = inst->id;
    sync.sceneLoaded(scene.toJson());
    lampLight(inst)->setIntensity(5.0f);
    sync.frame(true, nullptr);
    CHECK(hasOverride(inst, "/light/intensity"), "override recorded while synced");

    // Apply elsewhere: new prefab intensity 2, plus a new mesh -> Loading.
    nlohmann::json applied = enemyFile(root.str(), 10.0f, 2.0f);
    applied["root"]["mesh"] = { {"sourcePath", tri}, {"name", "tri"} };
    lib.updateFromMemory(kEnemyKey, applied, audio());
    lampLight(inst)->setIntensity(1.0f);   // the user's undo: back to the OLD prefab value
    sync.frame(true, nullptr);

    CHECK(settleSync(sync, lib), "reload settles");
    inst = scene.findById(id);
    CHECK(inst && lampLight(inst) && lampLight(inst)->getIntensity() == 2.0f,
          "a reverted override follows the new prefab value");
    CHECK(inst && !hasOverride(inst, "/light/intensity"), "and is no longer an override");
}

// R1: revert detection must resolve the edited path INSIDE the template node,
// not look for a template diff entry at exactly the same depth.
// Shared setup: one synced Enemy instance; then another instance's Apply puts
// the template in Loading (new mesh) with intensity 2 on the Lamp.
struct ReloadWindow
{
    TempRoot root;
    DonTopo::JobSystem js;
    std::unique_ptr<DonTopo::PrefabLibrary> lib;
    DonTopo::Scene scene;
    SyncFixture fx;
    std::unique_ptr<DonTopo::PrefabSceneSync> sync;
    uint64_t id = 0;

    explicit ReloadWindow(const char* name) : root(name)
    {
        writeFile(root.p / kEnemyKey, enemyFile(root.str(), 10.0f, 1.0f).dump(2));
        writeFile(root.p / "Models" / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        js.start(2);
        lib = std::make_unique<DonTopo::PrefabLibrary>(js, root.str());
        scene.setAssetRoot(root.str());
        sync = std::make_unique<DonTopo::PrefabSceneSync>(*lib);
        sync->setHost(fx.host(scene));
        lib->request(kEnemyKey);
        settleSync(*sync, *lib);
        if (const DonTopo::PrefabTemplate* t = readyTemplate(*lib, kEnemyKey))
            if (DonTopo::GameObject* inst = scene.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio()))
                id = inst->id;
        sync->sceneLoaded(scene.toJson());
    }
    DonTopo::GameObject* inst() { return scene.findById(id); }
    // The Apply of another instance: Loading until settled.
    void startReload()
    {
        nlohmann::json applied = enemyFile(root.str(), 10.0f, 2.0f);
        applied["root"]["mesh"] = { {"sourcePath", (root.p / "Models" / "tri.obj").string()}, {"name", "tri"} };
        lib->updateFromMemory(kEnemyKey, applied, audio());
    }
    ~ReloadWindow() { sync.reset(); lib.reset(); js.shutdown(); }
};

// Case 1: a component the INSTANCE added (stored "/light" override on the
// root, which has no light in the prefab); its intensity edited during the
// reload. The template has no "/light" there at all: that is an edit, kept.
void testRevertDetectionAddedComponentEdit()
{
    ReloadWindow w("dt_prefab_editor_r1_added");
    DonTopo::GameObject* inst = w.inst();
    if (!inst) { CHECK(false, "instance"); return; }
    auto light = std::make_shared<DonTopo::LightComponent>();
    light->setIntensity(3.0f);
    inst->setLight(light);
    w.sync->frame(true, nullptr);
    CHECK(hasOverride(inst, "/light"), "the added light is a stored override");

    w.startReload();
    inst->getLight()->setIntensity(7.0f);   // edited while the template reloads
    w.sync->frame(true, nullptr);
    CHECK(settleSync(*w.sync, *w.lib), "reload settles");
    inst = w.inst();
    CHECK(inst && inst->getLight() && inst->getLight()->getIntensity() == 7.0f,
          "the edit of an instance-added component survives the reload");
}

// Case 2: a component the instance REMOVED (stored null override), re-added by
// the user with custom values during the reload: an edit, kept.
void testRevertDetectionReaddedComponent()
{
    ReloadWindow w("dt_prefab_editor_r1_readded");
    DonTopo::GameObject* inst = w.inst();
    DonTopo::GameObject* lamp = inst ? childNamed(inst, "Lamp") : nullptr;
    if (!lamp) { CHECK(false, "lamp"); return; }
    lamp->setLight(nullptr);
    w.sync->frame(true, nullptr);
    CHECK(hasOverride(inst, "/light"), "the removal is a stored override");

    w.startReload();
    auto light = std::make_shared<DonTopo::LightComponent>();
    light->setIntensity(1.0f);
    light->setRange(33.0f);
    lamp->setLight(light);   // re-added with a custom range while the template reloads
    w.sync->frame(true, nullptr);
    CHECK(settleSync(*w.sync, *w.lib), "reload settles");
    inst = w.inst();
    CHECK(inst && lampLight(inst) && lampLight(inst)->getRange() == 33.0f,
          "the re-added component keeps its custom values");
}

// R2: a mesh that lands after the scene load is part of the baseline from the
// next frame on, so a material edit made before the template is Ready is
// folded (and kept), not reverted by the rebuild.
void testMaterialEditAfterMeshLandsIsKept()
{
    namespace fs = std::filesystem;
    TempRoot root("dt_prefab_editor_r2");
    const std::string tri = (root.p / "Models" / "box.obj").string();
    writeFile(tri, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    const fs::path maw = root.p / "Models" / "Maw J Laygo.fbx";
    fs::copy_file("assets/animatedCharacter/Maw J Laygo.fbx", maw);
    const nlohmann::json identity = nlohmann::json::array({ 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0 });

    nlohmann::json prefabFile;
    {
        DonTopo::Scene s0;
        s0.setAssetRoot(root.str());
        // A skinned child keeps the template Loading while its mesh is not
        // delivered (pump budget 0 below); the .obj child is what lands first.
        nlohmann::json enemy = { {"name", "Enemy"}, {"localTransform", identity}, {"children", nlohmann::json::array()} };
        enemy["children"].push_back({ {"name", "Box"}, {"localTransform", identity},
                                      {"mesh", { {"sourcePath", tri}, {"name", "box"} }},
                                      {"children", nlohmann::json::array()} });
        enemy["children"].push_back({ {"name", "Body"}, {"localTransform", identity},
                                      {"mesh", { {"sourcePath", maw.string()}, {"name", "Maw"}, {"skinned", true} }},
                                      {"children", nlohmann::json::array()} });
        DonTopo::GameObject* e = s0.insertFromJson(enemy, nullptr, SIZE_MAX, physics(), audio());
        if (!e) { CHECK(false, "prefab source"); return; }
        prefabFile = s0.makePrefab(e, kEnemyKey, /*link=*/false);
    }
    writeFile(root.p / kEnemyKey, prefabFile.dump(2));
    nlohmann::json sceneJson;
    {
        DonTopo::Scene s1;
        s1.setAssetRoot(root.str());
        if (!makeInstance(s1, memoryRootOf(prefabFile, root.str()))) { CHECK(false, "instance"); return; }
        sceneJson = s1.toJson();
    }

    DonTopo::JobSystem js;
    js.start(2);
    DonTopo::AsyncAssetLoader loader(js);
    DonTopo::Scene scene;
    scene.setAssetRoot(root.str());
    CHECK(scene.fromJson(sceneJson, physics(), audio(), &loader), "scene loads async");
    DonTopo::GameObject* inst = firstInstance(scene);
    if (!inst || !childNamed(inst, "Box")) { CHECK(false, "instance"); return; }
    const uint64_t rootId = inst->id, boxId = childNamed(inst, "Box")->id;

    DonTopo::PrefabLibrary lib(js, root.str());
    SyncFixture fx;
    DonTopo::PrefabSceneSync sync(lib);
    DonTopo::PrefabSceneSync::Host host = fx.host(scene);
    host.meshRequestLive = [&](uint64_t job) { return loader.isRequestLive(job); };
    host.loaderPending   = [&] { return loader.pending(); };
    sync.setHost(host);
    sync.sceneLoaded(sceneJson);   // template requested; .obj still in flight

    // The scene's mesh lands (onAssetsLoaded), the template is still Loading.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (loader.pending() > 0 && std::chrono::steady_clock::now() < deadline)
    {
        applyLoaded(loader, scene);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    sync.pump(0.0f);   // next editor frame (mesh budget 0: the template stays Loading)
    CHECK(lib.find(kEnemyKey) && lib.find(kEnemyKey)->state == DonTopo::PrefabTemplate::State::Loading,
          "the template is still Loading");

    // The user edits the landed mesh's material.
    DonTopo::GameObject* box = scene.findById(boxId);
    if (!box || !box->hasMesh()) { CHECK(false, "box mesh landed"); return; }
    DonTopo::MaterialOverride ov;
    ov.index = 0;
    ov.roughness = 0.3f;
    box->materialOverrides.push_back(ov);
    DonTopo::applyMaterialOverrides(*box);
    sync.frame(true, nullptr);

    CHECK(settleSync(sync, lib), "library settles");
    for (int i = 0; i < DonTopo::PrefabSceneSync::kRetryIntervalFrames + 2; ++i) sync.frame(false, nullptr);
    inst = scene.findById(rootId);
    box = inst ? childNamed(inst, "Box") : nullptr;
    CHECK(inst && sync.tracker().isSynced(inst, lib), "synced");
    CHECK(box && !box->materialOverrides.empty() && box->materialOverrides[0].roughness == 0.3f,
          "the material edit made after the mesh landed survives");
    bool meshOverride = false;
    if (inst) for (const auto& o : inst->prefabLink->overrides) if (o.path.rfind("/mesh", 0) == 0) meshOverride = true;
    CHECK(meshOverride, "as a mesh override");
}

// S1: settling landed meshes is bounded: nothing in flight = no scene walk at
// all, and an instance deleted while its mesh was in flight stops being waited
// for (the undo of Delete restores it without mesh and without request).
void testSettleLandedMeshesBounded()
{
    TempRoot root("dt_prefab_editor_settle");
    const nlohmann::json file = enemyFile(root.str(), 10.0f);
    DonTopo::Scene scene;
    DonTopo::GameObject* inst = makeInstance(scene, memoryRootOf(file, root.str()));
    if (!inst || !childNamed(inst, "Lamp")) { CHECK(false, "instance"); return; }
    DonTopo::PrefabOverrideTracker tracker;
    tracker.setMeshRequestLive([](uint64_t) { return true; });

    CHECK(tracker.settleLandedMeshes(scene) == 0 && tracker.settleScans() == 0,
          "nothing in flight: no scene walk");
    childNamed(inst, "Lamp")->pendingMeshJob = 9;   // a live request
    tracker.captureBaseline(scene, inst);
    CHECK(tracker.meshInFlightCount() == 1, "one node in flight");
    tracker.settleLandedMeshes(scene);
    CHECK(tracker.meshInFlightCount() == 1 && tracker.settleScans() == 1, "still waiting: one walk per call");

    scene.removeGameObject(inst);   // deleted while in flight
    tracker.settleLandedMeshes(scene);
    CHECK(tracker.meshInFlightCount() == 0, "a deleted instance is no longer waited for");
    const int scans = tracker.settleScans();
    for (int i = 0; i < 10; ++i) tracker.settleLandedMeshes(scene);
    CHECK(tracker.settleScans() == scans, "and costs no further scene walks");
}

// S2: removing an instance-ADDED component during the reload window erases the
// stored add-override instead of folding a null over it (which the rebuild then
// reported as "component no longer in the prefab").
void testRemovingAddedComponentDuringReload()
{
    ReloadWindow w("dt_prefab_editor_s2");
    DonTopo::GameObject* inst = w.inst();
    if (!inst) { CHECK(false, "instance"); return; }
    auto light = std::make_shared<DonTopo::LightComponent>();
    light->setIntensity(3.0f);
    inst->setLight(light);
    w.sync->frame(true, nullptr);
    CHECK(hasOverride(inst, "/light"), "the added light is a stored override");

    w.startReload();
    inst->setLight(nullptr);   // removed again while the template reloads
    w.sync->frame(true, nullptr);
    w.fx.log.clear();
    CHECK(settleSync(*w.sync, *w.lib), "reload settles");
    inst = w.inst();
    CHECK(inst && !inst->getLight(), "no light");
    CHECK(inst && !hasOverride(inst, "/light"), "no override left for it");
    CHECK(!w.fx.logged("no longer in the prefab") && !w.fx.logged("dropped"), "no misleading warning");
}

// ---- Prefab undo commands (Task 10: spec test 3, Review Focus 2 and 5) ----
//
// Every step goes through a real UndoManager and is followed by one revision
// pass of the editor's sync (PrefabSceneSync::frame(true)): what the tracker
// makes of each command's own changes is part of what is tested.

std::string readBytes(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const DonTopo::PrefabOverride* overrideAt(const DonTopo::GameObject* root, const std::string& path)
{
    if (!root || !root->prefabLink) return nullptr;
    for (const auto& o : root->prefabLink->overrides) if (o.path == path) return &o;
    return nullptr;
}

size_t overrideCount(const DonTopo::GameObject* root)
{
    return root && root->prefabLink ? root->prefabLink->overrides.size() : 0;
}

bool overrideIs(const DonTopo::GameObject* root, const std::string& path, double v)
{
    const DonTopo::PrefabOverride* o = overrideAt(root, path);
    return o && o->value.is_number() && std::abs(o->value.get<double>() - v) < 1e-5;
}

// Every id in the scene once (the root included).
bool idsUnique(DonTopo::Scene& scene)
{
    std::set<uint64_t> seen;
    bool ok = true;
    scene.traverse([&](DonTopo::GameObject* n) { if (!seen.insert(n->id).second) ok = false; });
    return ok;
}

std::vector<uint64_t> subtreeIds(DonTopo::GameObject* root)
{
    std::vector<uint64_t> ids;
    if (root) root->traverse([&](DonTopo::GameObject* n) { ids.push_back(n->id); });
    return ids;
}

struct CommandWorld
{
    TempRoot root;
    DonTopo::JobSystem js;
    std::unique_ptr<DonTopo::PrefabLibrary> lib;
    DonTopo::Scene scene;
    SyncFixture fx;
    std::unique_ptr<DonTopo::PrefabSceneSync> sync;
    DonTopo::UndoManager undo;

    explicit CommandWorld(const char* name) : root(name)
    {
        js.start(2);
        lib = std::make_unique<DonTopo::PrefabLibrary>(js, root.str());
        lib->setKeepRawMeshes(true);   // as the editor's
        scene.setAssetRoot(root.str());
        sync = std::make_unique<DonTopo::PrefabSceneSync>(*lib);
        sync->setHost(fx.host(scene));
    }
    ~CommandWorld() { undo.clear(); sync.reset(); lib.reset(); js.shutdown(); }

    DonTopo::PrefabCommandContext ctx()
    {
        return { scene, *lib, sync->tracker(), physics(), audio(),
                 [this](DonTopo::GameObject*) { ++fx.registered; } };
    }
    // What the editor does at the end of the frame of a push / undo / redo.
    void pass() { sync->frame(/*revisionChanged=*/true, nullptr); }
    template <typename Cmd> Cmd* run(std::unique_ptr<Cmd> cmd)
    {
        Cmd* raw = cmd.get();
        raw->execute();
        undo.push(std::move(cmd));
        pass();
        return raw;
    }
    void doUndo() { undo.undo(); pass(); }
    void doRedo() { undo.redo(); pass(); }
    std::filesystem::path prefabFile() const { return root.p / kEnemyKey; }
    DonTopo::GameObject* get(uint64_t id) { return scene.findById(id); }
    bool synced(uint64_t id) { return sync->tracker().isSynced(get(id), *lib); }
};

// Enemy.prefab on disk (range 10, intensity 1), Ready, and three synced instances.
struct ThreeInstances : CommandWorld
{
    uint64_t a = 0, b = 0, c = 0;
    explicit ThreeInstances(const char* name) : CommandWorld(name)
    {
        writeFile(prefabFile(), enemyFile(root.str(), 10.0f).dump(2));
        lib->request(kEnemyKey);
        settleSync(*sync, *lib);
        const DonTopo::PrefabTemplate* t = readyTemplate(*lib, kEnemyKey);
        if (!t) return;
        DonTopo::GameObject* ia = scene.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
        DonTopo::GameObject* ib = scene.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
        DonTopo::GameObject* ic = scene.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
        if (!ia || !ib || !ic) return;
        a = ia->id; b = ib->id; c = ic->id;
        sync->sceneLoaded(scene.toJson());   // baselines + sync, as after a scene load
    }
    bool ok() { return a && b && c && synced(a) && synced(b) && synced(c); }
    float intensity(uint64_t id) { auto* l = lampLight(get(id)); return l ? l->getIntensity() : -1.0f; }
    float range(uint64_t id)     { auto* l = lampLight(get(id)); return l ? l->getRange() : -1.0f; }
};

// Spec test 3, Create: file + link; undo deletes the file it wrote and unlinks;
// redo writes the same bytes. Never overwrites an existing file, never nests.
void testCreatePrefabCommand()
{
    CommandWorld w("dt_prefab_cmd_create");
    DonTopo::GameObject* enemy = w.scene.addGameObject("Enemy");
    DonTopo::GameObject* lamp  = w.scene.addGameObject("Lamp", enemy);
    auto light = std::make_shared<DonTopo::LightComponent>();
    light->setRange(10.0f);
    lamp->setLight(light);
    const uint64_t enemyId = enemy->id, lampId = lamp->id;

    auto* create = w.run(std::make_unique<DonTopo::CreatePrefabCommand>(w.ctx(), enemyId, w.prefabFile()));
    CHECK(create->succeeded(), "create succeeded");
    CHECK(create->label() == "Create Prefab 'Enemy'", "create label");
    CHECK(std::filesystem::exists(w.prefabFile()), "the .prefab is written");
    const std::string bytes = readBytes(w.prefabFile());
    enemy = w.get(enemyId);
    CHECK(enemy && enemy->prefabLink && enemy->prefabLink->path == kEnemyKey, "the subtree is linked (project-relative)");
    CHECK(w.get(lampId) && w.get(lampId)->prefabNode == 2, "children get their prefab-local id");
    CHECK(settleSync(*w.sync, *w.lib), "settles");
    w.pass();
    CHECK(readyTemplate(*w.lib, kEnemyKey) != nullptr, "the template is registered in the library");
    CHECK(overrideCount(w.get(enemyId)) == 0, "a fresh prefab instance has no override");
    CHECK(w.synced(enemyId), "and is synced");
    CHECK(w.get(enemyId)->id == enemyId && w.get(lampId), "scene ids untouched");

    w.doUndo();
    CHECK(!std::filesystem::exists(w.prefabFile()), "undo deletes the file it wrote");
    CHECK(contains(create->lastMessage(), "deleted"), "and says so");
    CHECK(w.get(enemyId) && !w.get(enemyId)->prefabLink && w.get(lampId)->prefabNode == 0, "undo unlinks");

    w.doRedo();
    CHECK(std::filesystem::exists(w.prefabFile()) && readBytes(w.prefabFile()) == bytes, "redo: the same bytes");
    CHECK(w.get(enemyId)->prefabLink && w.get(lampId)->prefabNode == 2, "redo links again");
    CHECK(settleSync(*w.sync, *w.lib), "settles");
    w.pass();
    CHECK(overrideCount(w.get(enemyId)) == 0 && w.synced(enemyId), "no spurious override after redo");

    // An existing file is never overwritten.
    DonTopo::GameObject* other = w.scene.addGameObject("Other");
    DonTopo::CreatePrefabCommand clash(w.ctx(), other->id, w.prefabFile());
    clash.execute();
    CHECK(!clash.succeeded() && !other->prefabLink, "Create onto an existing file is refused");
    CHECK(readBytes(w.prefabFile()) == bytes, "and the file is untouched");
    clash.undo();
    CHECK(std::filesystem::exists(w.prefabFile()), "the undo of a refused Create deletes nothing");

    // No nesting: a node inside an instance cannot become a prefab.
    const std::filesystem::path lampFile = w.root.p / "Prefabs" / "Lamp.prefab";
    DonTopo::CreatePrefabCommand nested(w.ctx(), lampId, lampFile);
    nested.execute();
    CHECK(!nested.succeeded() && !std::filesystem::exists(lampFile), "a node inside an instance is refused");
    CHECK(contains(nested.lastMessage(), "already part of a prefab instance"), "with the spec's message");
}

// Review Focus 5: undo of Create deletes the file only while it holds exactly
// the bytes Create wrote.
void testCreateUndoKeepsModifiedFile()
{
    {
        // Create -> Apply (changes the file) -> undo Apply (restores it) -> undo
        // Create: the bytes are Create's again, so it goes.
        CommandWorld w("dt_prefab_cmd_rf5_apply");
        DonTopo::GameObject* enemy = w.scene.addGameObject("Enemy");
        DonTopo::GameObject* lamp  = w.scene.addGameObject("Lamp", enemy);
        lamp->setLight(std::make_shared<DonTopo::LightComponent>());
        const uint64_t enemyId = enemy->id;
        w.run(std::make_unique<DonTopo::CreatePrefabCommand>(w.ctx(), enemyId, w.prefabFile()));
        CHECK(settleSync(*w.sync, *w.lib), "settles");
        const std::string created = readBytes(w.prefabFile());
        lampLight(w.get(enemyId))->setIntensity(5.0f);
        w.pass();
        CHECK(hasOverride(w.get(enemyId), "/light/intensity"), "edit recorded");
        auto* apply = w.run(std::make_unique<DonTopo::ApplyPrefabOverridesCommand>(
            w.ctx(), enemyId, std::vector<DonTopo::PrefabOverride>{}, false));
        CHECK(apply->succeeded(), "apply succeeded");
        CHECK(readBytes(w.prefabFile()) != created, "Apply changed the file");
        w.doUndo();   // Apply
        CHECK(readBytes(w.prefabFile()) == created, "undo of Apply restores Create's bytes");
        w.doUndo();   // Create
        CHECK(!std::filesystem::exists(w.prefabFile()), "then undo of Create deletes it");
    }
    {
        // Create, then the file is changed outside the command: undo keeps it.
        CommandWorld w("dt_prefab_cmd_rf5_external");
        DonTopo::GameObject* enemy = w.scene.addGameObject("Enemy");
        const uint64_t enemyId = enemy->id;
        auto* create = w.run(std::make_unique<DonTopo::CreatePrefabCommand>(w.ctx(), enemyId, w.prefabFile()));
        const std::string external = "{\"edited\": \"by someone else\"}";
        writeFile(w.prefabFile(), external);
        w.doUndo();
        CHECK(std::filesystem::exists(w.prefabFile()), "a modified file is NOT deleted");
        CHECK(readBytes(w.prefabFile()) == external, "and keeps its content");
        CHECK(contains(create->lastMessage(), "kept"), "lastMessage says it was kept");
        CHECK(w.get(enemyId) && !w.get(enemyId)->prefabLink, "the subtree is unlinked anyway");
        // Redo must not overwrite it either.
        w.doRedo();
        CHECK(readBytes(w.prefabFile()) == external && !create->succeeded(), "redo does not overwrite it");
    }
    {
        // Gone already: nothing to delete, no crash, and it says so.
        CommandWorld w("dt_prefab_cmd_rf5_gone");
        DonTopo::GameObject* enemy = w.scene.addGameObject("Enemy");
        auto* create = w.run(std::make_unique<DonTopo::CreatePrefabCommand>(w.ctx(), enemy->id, w.prefabFile()));
        std::filesystem::remove(w.prefabFile());
        w.doUndo();
        CHECK(!create->lastMessage().empty() && !contains(create->lastMessage(), "deleted"),
              "a file already gone is reported, not deleted");
    }
}

// Review Focus 2 + spec test 3, Apply: one override applied from B while A has
// an override on the same path -> A keeps its value, C (no override) gets the
// new one. Undo restores the exact file bytes and every instance.
void testApplyKeepsOtherInstanceOverride()
{
    ThreeInstances w("dt_prefab_cmd_apply");
    if (!w.ok()) { CHECK(false, "three synced instances"); return; }
    lampLight(w.get(w.a))->setIntensity(7.0f);
    lampLight(w.get(w.b))->setIntensity(3.0f);
    lampLight(w.get(w.b))->setRange(40.0f);
    w.pass();
    CHECK(overrideIs(w.get(w.a), "/light/intensity", 7.0), "A overrides intensity");
    CHECK(overrideIs(w.get(w.b), "/light/intensity", 3.0) && overrideIs(w.get(w.b), "/light/range", 40.0),
          "B overrides intensity and range");
    const std::string before = readBytes(w.prefabFile());
    const uint64_t lampB = childNamed(w.get(w.b), "Lamp")->id;

    auto expectApplied = [&](const char* when) {
        std::string m = std::string(when) + ": ";
        CHECK(w.intensity(w.a) == 7.0f && overrideIs(w.get(w.a), "/light/intensity", 7.0) &&
              overrideCount(w.get(w.a)) == 1, (m + "A keeps its own override").c_str());
        CHECK(w.intensity(w.b) == 3.0f && !overrideAt(w.get(w.b), "/light/intensity") &&
              overrideIs(w.get(w.b), "/light/range", 40.0) && overrideCount(w.get(w.b)) == 1,
              (m + "B lost only the applied override").c_str());
        CHECK(w.intensity(w.c) == 3.0f && overrideCount(w.get(w.c)) == 0, (m + "C took the new value").c_str());
        CHECK(w.range(w.a) == 10.0f && w.range(w.c) == 10.0f, (m + "range was not applied").c_str());
        const DonTopo::PrefabTemplate* t = readyTemplate(*w.lib, kEnemyKey);
        CHECK(t && t->memoryRoot["children"][0]["light"]["intensity"].get<float>() == 3.0f,
              (m + "the template holds the applied value").c_str());
        CHECK(w.synced(w.a) && w.synced(w.b) && w.synced(w.c), (m + "every instance synced").c_str());
        CHECK(w.get(lampB) && w.get(lampB)->parent == w.get(w.b), (m + "B's lamp kept its id").c_str());
    };

    auto* apply = w.run(std::make_unique<DonTopo::ApplyPrefabOverridesCommand>(
        w.ctx(), w.b, std::vector<DonTopo::PrefabOverride>{ {2, "/light/intensity", 3.0} }, false));
    CHECK(apply->succeeded(), "apply succeeded");
    CHECK(apply->label() == "Apply to Prefab 'Enemy'", "apply label");
    CHECK(readBytes(w.prefabFile()) != before, "the file changed");
    nlohmann::json onDisk = nlohmann::json::parse(readBytes(w.prefabFile()));
    CHECK(onDisk["root"]["children"][0]["light"]["intensity"].get<float>() == 3.0f, "written to disk");
    expectApplied("after execute");
    w.pass();
    expectApplied("after a second pass");
    const std::string applied = readBytes(w.prefabFile());

    w.doUndo();
    CHECK(readBytes(w.prefabFile()) == before, "undo: the file bytes are exactly the previous ones");
    CHECK(w.intensity(w.c) == 1.0f && overrideCount(w.get(w.c)) == 0, "undo: C back to the old value");
    CHECK(w.intensity(w.b) == 3.0f && overrideIs(w.get(w.b), "/light/intensity", 3.0) &&
          overrideIs(w.get(w.b), "/light/range", 40.0) && overrideCount(w.get(w.b)) == 2,
          "undo: B's overrides are back");
    CHECK(w.intensity(w.a) == 7.0f && overrideIs(w.get(w.a), "/light/intensity", 7.0) &&
          overrideCount(w.get(w.a)) == 1, "undo: A untouched");
    CHECK(w.synced(w.a) && w.synced(w.b) && w.synced(w.c), "undo: every instance synced");

    w.doRedo();
    CHECK(readBytes(w.prefabFile()) == applied, "redo writes the applied file again");
    expectApplied("after redo");
    CHECK(idsUnique(w.scene), "ids unique");

    // Undo after the file was changed outside: the file is not overwritten.
    const std::string external = applied + "\n";
    writeFile(w.prefabFile(), external);
    w.doUndo();
    CHECK(readBytes(w.prefabFile()) == external, "an externally changed file is not overwritten by undo");
    // The command is gone (history discarded): only the manager can tell.
    CHECK(w.undo.lastDiscardedHistory() && contains(w.undo.lastMessage(), "changed outside the editor"),
          "and the manager says why the history went");
}

// Apply of B's override AND its added child with a new mesh: the template goes
// Loading. The overrides are exactly the intended ones through the reload, the
// other instances receive the child and the value (A keeping its own override
// on the same path), and undo unlinks the child again and takes it away from A
// and C. During the reload of the redo, B sets the field back to the OLD prefab
// value: B stands on the NEW template, so that is an edit (kept), not a revert.
void testApplyAddedChildWhileLoading()
{
    ThreeInstances w("dt_prefab_cmd_apply_loading");
    if (!w.ok()) { CHECK(false, "three synced instances"); return; }
    const std::string tri = (w.root.p / "Models" / "tri.obj").string();
    writeFile(tri, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    lampLight(w.get(w.a))->setIntensity(7.0f);
    lampLight(w.get(w.b))->setIntensity(3.0f);
    w.pass();
    DonTopo::GameObject* added = w.scene.insertFromJson(
        { {"name", "Tri"}, {"mesh", { {"sourcePath", tri}, {"name", "tri"} }} },
        w.get(w.b), SIZE_MAX, physics(), audio());
    if (!added || !added->hasMesh()) { CHECK(false, "added child with a mesh"); return; }
    const uint64_t triId = added->id;
    w.pass();
    CHECK(overrideCount(w.get(w.b)) == 1, "an added child is not an override");

    // bOverride: B's expected intensity override (0 = none).
    auto expectOverrides = [&](const char* when, double bOverride) {
        std::string m = std::string(when) + ": ";
        CHECK(overrideIs(w.get(w.a), "/light/intensity", 7.0) && overrideCount(w.get(w.a)) == 1,
              (m + "A: exactly its own override").c_str());
        if (bOverride == 0.0)
            CHECK(overrideCount(w.get(w.b)) == 0, (m + "B: none").c_str());
        else
            CHECK(overrideIs(w.get(w.b), "/light/intensity", bOverride) && overrideCount(w.get(w.b)) == 1,
                  (m + "B: exactly its intensity override").c_str());
        CHECK(overrideCount(w.get(w.c)) == 0, (m + "C: none").c_str());
    };

    auto* apply = w.run(std::make_unique<DonTopo::ApplyPrefabOverridesCommand>(
        w.ctx(), w.b, std::vector<DonTopo::PrefabOverride>{}, true));
    CHECK(apply->succeeded(), "apply succeeded");
    CHECK(w.lib->find(kEnemyKey) && w.lib->find(kEnemyKey)->state == DonTopo::PrefabTemplate::State::Loading,
          "the template reloads (new mesh)");
    CHECK(w.get(triId) && w.get(triId)->prefabNode == 3, "B's child is linked with the next local id");
    expectOverrides("Loading, after the pass", 0.0);
    w.pass();
    expectOverrides("Loading, second pass", 0.0);

    CHECK(settleSync(*w.sync, *w.lib), "reload settles");
    w.pass();
    expectOverrides("Ready", 0.0);
    CHECK(w.get(triId) && w.get(triId)->parent == w.get(w.b), "B's child kept its id and place");
    DonTopo::GameObject* triA = childNamed(w.get(w.a), "Tri");
    DonTopo::GameObject* triC = childNamed(w.get(w.c), "Tri");
    CHECK(triA && triA->prefabNode == 3 && triA->hasMesh(), "A received the child, with its mesh");
    CHECK(triC && triC->prefabNode == 3 && triC->hasMesh(), "C too");
    CHECK(w.intensity(w.a) == 7.0f && w.intensity(w.b) == 3.0f && w.intensity(w.c) == 3.0f,
          "A keeps its value, C took the applied one");
    CHECK(w.synced(w.a) && w.synced(w.b) && w.synced(w.c), "every instance synced");
    CHECK(idsUnique(w.scene), "ids unique");

    w.doUndo();
    CHECK(settleSync(*w.sync, *w.lib), "settles after undo");
    w.pass();
    expectOverrides("after undo", 3.0);
    CHECK(w.get(triId) && w.get(triId)->prefabNode == 0 && w.get(triId)->parent == w.get(w.b),
          "undo: B's child is an added child again, same id");
    CHECK(!childNamed(w.get(w.a), "Tri") && !childNamed(w.get(w.c), "Tri"), "undo: A and C lost it");
    CHECK(w.intensity(w.c) == 1.0f && w.intensity(w.b) == 3.0f, "undo: values back");
    CHECK(w.synced(w.a) && w.synced(w.b) && w.synced(w.c), "undo: every instance synced");

    w.doRedo();
    CHECK(w.lib->find(kEnemyKey)->state == DonTopo::PrefabTemplate::State::Loading, "redo reloads too");
    expectOverrides("after redo, Loading", 0.0);
    lampLight(w.get(w.b))->setIntensity(1.0f);   // the OLD prefab value, during the reload
    w.pass();
    CHECK(settleSync(*w.sync, *w.lib), "settles after redo");
    w.pass();
    expectOverrides("after redo, Ready", 1.0);
    CHECK(w.intensity(w.b) == 1.0f, "B's edit during the reload survives the propagation");
    CHECK(w.intensity(w.c) == 3.0f, "C has the applied value again");
    CHECK(w.get(triId) && w.get(triId)->prefabNode == 3, "redo links the same node again");
    CHECK(childNamed(w.get(w.a), "Tri") && childNamed(w.get(w.c), "Tri"), "redo: A and C receive it");
    CHECK(idsUnique(w.scene), "ids unique after redo");
}

// Spec test 3, Revert: one override, then all (with the added child); undo
// brings back exactly the previous overrides, values and the deleted child.
void testRevertPrefabOverridesCommand()
{
    ThreeInstances w("dt_prefab_cmd_revert");
    if (!w.ok()) { CHECK(false, "three synced instances"); return; }
    lampLight(w.get(w.b))->setIntensity(5.0f);
    lampLight(w.get(w.b))->setRange(40.0f);
    lampLight(w.get(w.a))->setIntensity(7.0f);
    const uint64_t extraId = w.scene.addGameObject("Extra", w.get(w.b))->id;
    const uint64_t lampB = childNamed(w.get(w.b), "Lamp")->id;
    w.pass();
    CHECK(overrideCount(w.get(w.b)) == 2, "B: two overrides");

    auto* one = w.run(std::make_unique<DonTopo::RevertPrefabOverridesCommand>(
        w.ctx(), w.b, std::vector<DonTopo::PrefabOverride>{ {2, "/light/intensity", nullptr} }, false));
    CHECK(one->succeeded(), "revert one succeeded");
    CHECK(w.intensity(w.b) == 1.0f && !overrideAt(w.get(w.b), "/light/intensity"), "intensity back to the prefab");
    CHECK(overrideIs(w.get(w.b), "/light/range", 40.0) && w.range(w.b) == 40.0f && overrideCount(w.get(w.b)) == 1,
          "range override untouched");
    CHECK(w.get(extraId) && w.get(lampB), "added child kept, lamp kept its id");
    CHECK(overrideIs(w.get(w.a), "/light/intensity", 7.0), "A untouched");

    w.doUndo();
    CHECK(w.intensity(w.b) == 5.0f && overrideIs(w.get(w.b), "/light/intensity", 5.0) &&
          overrideCount(w.get(w.b)) == 2, "undo: override and value back");
    CHECK(w.synced(w.b), "undo: synced");
    w.doRedo();
    CHECK(w.intensity(w.b) == 1.0f && overrideCount(w.get(w.b)) == 1, "redo reverts again");

    auto* all = w.run(std::make_unique<DonTopo::RevertPrefabOverridesCommand>(
        w.ctx(), w.b, std::vector<DonTopo::PrefabOverride>{}, true));
    CHECK(all->succeeded(), "revert all succeeded");
    CHECK(overrideCount(w.get(w.b)) == 0 && w.intensity(w.b) == 1.0f && w.range(w.b) == 10.0f,
          "revert all: no override, prefab values");
    CHECK(!w.get(extraId), "revert all deletes the added child");
    CHECK(w.synced(w.b), "synced");

    w.doUndo();
    CHECK(overrideIs(w.get(w.b), "/light/range", 40.0) && overrideCount(w.get(w.b)) == 1 && w.range(w.b) == 40.0f,
          "undo: exactly the previous override");
    CHECK(w.get(extraId) && w.get(extraId)->parent == w.get(w.b), "undo: the added child is back, same id");
    CHECK(w.get(lampB) && w.get(lampB)->parent == w.get(w.b), "undo: the lamp keeps its id");
    CHECK(w.synced(w.b), "undo: synced");
    w.pass();
    CHECK(overrideCount(w.get(w.b)) == 1, "no spurious override after another pass");
    CHECK(idsUnique(w.scene), "ids unique");
}

// Spec test 3, Unpack: no link; undo puts the link (with its overrides) and
// every prefabNode back on the same ids, synced.
void testUnpackPrefabCommand()
{
    ThreeInstances w("dt_prefab_cmd_unpack");
    if (!w.ok()) { CHECK(false, "three synced instances"); return; }
    lampLight(w.get(w.b))->setIntensity(5.0f);
    w.pass();
    const uint64_t lampB = childNamed(w.get(w.b), "Lamp")->id;
    // An edit the tracker has not seen yet (no revision pass in between, like a
    // script property): Unpack must record it, or its undo loses it.
    lampLight(w.get(w.b))->setRange(25.0f);

    auto* unpack = w.run(std::make_unique<DonTopo::UnpackPrefabCommand>(w.ctx(), w.b));
    CHECK(unpack->succeeded(), "unpack succeeded");
    CHECK(!w.get(w.b)->prefabLink && w.get(lampB)->prefabNode == 0 && w.get(w.b)->prefabNode == 0, "unlinked");
    CHECK(w.intensity(w.b) == 5.0f && w.range(w.b) == 25.0f, "objects stay as they are");

    w.doUndo();
    DonTopo::GameObject* b = w.get(w.b);
    CHECK(b && b->prefabLink && b->prefabLink->path == kEnemyKey, "undo: link back");
    CHECK(overrideIs(b, "/light/intensity", 5.0) && overrideIs(b, "/light/range", 25.0) && overrideCount(b) == 2,
          "undo: exactly the same overrides, the unrecorded edit included");
    CHECK(w.range(w.b) == 25.0f && w.intensity(w.b) == 5.0f, "undo: values intact");
    CHECK(b && b->prefabNode == 1 && w.get(lampB) && w.get(lampB)->prefabNode == 2, "undo: prefabNodes back, same ids");
    CHECK(w.synced(w.b), "undo: synced");
    w.pass();
    CHECK(overrideCount(w.get(w.b)) == 2, "no spurious or lost override after another pass");
    lampLight(w.get(w.b))->setRange(33.0f);
    w.pass();
    CHECK(overrideIs(w.get(w.b), "/light/range", 33.0), "and a new edit is recorded");

    w.doRedo();
    CHECK(!w.get(w.b)->prefabLink, "redo unpacks again");
}

// Spec test 3 + §7, Instantiate: fresh unique ids; redo reuses the ids of the
// first execute, so a later command in the stack still resolves.
void testInstantiatePrefabCommand()
{
    CommandWorld w("dt_prefab_cmd_instantiate");
    writeFile(w.prefabFile(), enemyFile(w.root.str(), 10.0f).dump(2));
    w.lib->request(kEnemyKey);
    CHECK(settleSync(*w.sync, *w.lib), "settles");
    const uint64_t holderId = w.scene.addGameObject("Holder")->id;

    auto* i1 = w.run(std::make_unique<DonTopo::InstantiatePrefabCommand>(w.ctx(), kEnemyKey, 0));
    auto* i2 = w.run(std::make_unique<DonTopo::InstantiatePrefabCommand>(w.ctx(), kEnemyKey, holderId));
    CHECK(i1->succeeded() && i2->succeeded(), "both instantiated");
    const std::vector<uint64_t> ids1 = subtreeIds(w.get(i1->instanceRootId()));
    const std::vector<uint64_t> ids2 = subtreeIds(w.get(i2->instanceRootId()));
    CHECK(ids1.size() == 2 && ids2.size() == 2, "Enemy + Lamp each");
    CHECK(w.get(i2->instanceRootId()) && w.get(i2->instanceRootId())->parent == w.get(holderId), "under its parent");
    CHECK(idsUnique(w.scene), "ids unique");
    CHECK(w.fx.registered >= 2, "registered on the GPU");
    CHECK(overrideCount(w.get(i1->instanceRootId())) == 0 && w.synced(i1->instanceRootId()) &&
          w.synced(i2->instanceRootId()), "synced, no override");
    // A later command that refers to the second instance by id.
    w.run(std::make_unique<DonTopo::UnpackPrefabCommand>(w.ctx(), i2->instanceRootId()));

    w.doUndo(); w.doUndo(); w.doUndo();
    CHECK(!w.get(ids1[0]) && !w.get(ids2[0]), "undo removes both");
    w.doRedo(); w.doRedo();
    CHECK(subtreeIds(w.get(i1->instanceRootId())) == ids1 && subtreeIds(w.get(i2->instanceRootId())) == ids2,
          "redo reuses the ids of the first execute");
    w.doRedo();
    CHECK(w.get(ids2[0]) && !w.get(ids2[0])->prefabLink, "the later Unpack resolves its target after the redo");
    CHECK(w.get(ids1[0]) && w.get(ids1[0])->prefabLink && overrideCount(w.get(ids1[0])) == 0 && w.synced(ids1[0]),
          "first instance synced with no override");
    CHECK(idsUnique(w.scene), "ids unique after redo");
}

// ---- Review of 2ada0e0: undo restores only what the command changed ----

int countChildren(DonTopo::GameObject* n, const std::string& name)
{
    int count = 0;
    if (n) for (auto& c : n->children) if (c->name == name) ++count;
    return count;
}

// The falling edge of an active item on the selection: an edit recorded by the
// tracker WITHOUT any command (script properties, spec §3.2).
void commandlessEdit(CommandWorld& w, DonTopo::GameObject* selected)
{
    w.sync->frame(/*revisionChanged=*/false, selected);
}

// T1: the .prefab changed on disk after Apply -> undo leaves the scene exactly
// as Apply left it (no unlinked copy beside a linked one, no override restored
// on top of a template that still has the applied value).
void testApplyUndoKeptLeavesSceneAsApplied()
{
    ThreeInstances w("dt_prefab_cmd_t1");
    if (!w.ok()) { CHECK(false, "three synced instances"); return; }
    lampLight(w.get(w.b))->setIntensity(3.0f);
    w.pass();
    const uint64_t extraId = w.scene.addGameObject("Extra", w.get(w.b))->id;
    w.pass();
    auto* apply = w.run(std::make_unique<DonTopo::ApplyPrefabOverridesCommand>(
        w.ctx(), w.b, std::vector<DonTopo::PrefabOverride>{}, true));
    CHECK(apply->succeeded(), "apply succeeded");
    CHECK(w.get(extraId) && w.get(extraId)->prefabNode == 3, "the added child is linked");
    const std::string external = readBytes(w.prefabFile()) + "\n";
    writeFile(w.prefabFile(), external);

    w.doUndo();
    auto expectAsApplied = [&](const char* when) {
        const std::string m = std::string(when) + ": ";
        CHECK(readBytes(w.prefabFile()) == external, (m + "the file is kept").c_str());
        CHECK(w.get(extraId) && w.get(extraId)->prefabNode == 3, (m + "the applied child stays linked").c_str());
        CHECK(countChildren(w.get(w.b), "Extra") == 1 && countChildren(w.get(w.c), "Extra") == 1,
              (m + "no duplicated child").c_str());
        CHECK(overrideCount(w.get(w.b)) == 0 && overrideCount(w.get(w.c)) == 0 && overrideCount(w.get(w.a)) == 0,
              (m + "overrides unchanged").c_str());
        CHECK(w.intensity(w.b) == 3.0f && w.intensity(w.c) == 3.0f, (m + "values unchanged").c_str());
    };
    // W1: a permanent refusal discards the history (the command is gone: no
    // pointer to it is used from here on).
    CHECK(w.undo.lastDiscardedHistory() && !w.undo.lastRefused() && !w.undo.canUndo() && !w.undo.canRedo(),
          "the undo history under the unrevertable Apply is discarded");
    expectAsApplied("after undo");
    w.pass();
    expectAsApplied("after another pass");
    CHECK(idsUnique(w.scene), "ids unique");
}

// T2: an edit recorded by the tracker after the command, without a command,
// survives the command's undo (Apply, Revert, Unpack).
void testUndoKeepsCommandlessEdits()
{
    {
        ThreeInstances w("dt_prefab_cmd_t2_apply");
        if (!w.ok()) { CHECK(false, "three synced instances"); return; }
        lampLight(w.get(w.b))->setIntensity(3.0f);
        w.pass();
        w.run(std::make_unique<DonTopo::ApplyPrefabOverridesCommand>(
            w.ctx(), w.b, std::vector<DonTopo::PrefabOverride>{ {2, "/light/intensity", 3.0} }, false));
        lampLight(w.get(w.c))->setRange(55.0f);
        commandlessEdit(w, childNamed(w.get(w.c), "Lamp"));
        CHECK(overrideIs(w.get(w.c), "/light/range", 55.0), "C's edit recorded");
        w.doUndo();
        CHECK(w.range(w.c) == 55.0f && overrideIs(w.get(w.c), "/light/range", 55.0) &&
              overrideCount(w.get(w.c)) == 1, "Apply undo: C's later edit survives");
        CHECK(w.intensity(w.c) == 1.0f, "Apply undo: C back to the old prefab value");
        CHECK(w.intensity(w.b) == 3.0f && overrideIs(w.get(w.b), "/light/intensity", 3.0), "B's override back");
    }
    {
        ThreeInstances w("dt_prefab_cmd_t2_revert");
        if (!w.ok()) { CHECK(false, "three synced instances"); return; }
        lampLight(w.get(w.b))->setIntensity(5.0f);
        const uint64_t extraId = w.scene.addGameObject("Extra", w.get(w.b))->id;
        const uint64_t lampB = childNamed(w.get(w.b), "Lamp")->id;
        w.pass();
        w.run(std::make_unique<DonTopo::RevertPrefabOverridesCommand>(
            w.ctx(), w.b, std::vector<DonTopo::PrefabOverride>{}, true));
        CHECK(!w.get(extraId) && w.intensity(w.b) == 1.0f, "reverted");
        lampLight(w.get(w.b))->setRange(44.0f);
        commandlessEdit(w, w.get(lampB));
        CHECK(overrideIs(w.get(w.b), "/light/range", 44.0), "B's edit recorded");
        w.doUndo();
        CHECK(w.intensity(w.b) == 5.0f && overrideIs(w.get(w.b), "/light/intensity", 5.0), "Revert undo: override back");
        CHECK(w.range(w.b) == 44.0f && overrideIs(w.get(w.b), "/light/range", 44.0) && overrideCount(w.get(w.b)) == 2,
              "Revert undo: the later edit survives");
        CHECK(w.get(extraId) && w.get(extraId)->parent == w.get(w.b), "Revert undo: deleted child back, same id");
        CHECK(w.get(lampB) && w.get(lampB)->parent == w.get(w.b), "Revert undo: lamp keeps its id");
        w.pass();
        CHECK(overrideCount(w.get(w.b)) == 2 && w.synced(w.b), "stable after another pass");
    }
    {
        ThreeInstances w("dt_prefab_cmd_t2_unpack");
        if (!w.ok()) { CHECK(false, "three synced instances"); return; }
        lampLight(w.get(w.b))->setIntensity(5.0f);
        w.pass();
        w.run(std::make_unique<DonTopo::UnpackPrefabCommand>(w.ctx(), w.b));
        lampLight(w.get(w.b))->setRange(33.0f);   // plain objects now: no tracker
        w.pass();
        w.doUndo();
        CHECK(overrideIs(w.get(w.b), "/light/intensity", 5.0) && overrideIs(w.get(w.b), "/light/range", 33.0) &&
              overrideCount(w.get(w.b)) == 2, "Unpack undo: the edit made while unpacked is merged");
        CHECK(w.range(w.b) == 33.0f && w.intensity(w.b) == 5.0f, "Unpack undo: values kept");
        w.pass();
        CHECK(overrideCount(w.get(w.b)) == 2 && w.synced(w.b), "stable after another pass");
    }
}

// T3: a later command addressing a node Apply's propagation created in ANOTHER
// instance still resolves after undo/redo (the redo reuses its id).
void testApplyRedoReusesPropagatedIds()
{
    ThreeInstances w("dt_prefab_cmd_t3");
    if (!w.ok()) { CHECK(false, "three synced instances"); return; }
    w.scene.addGameObject("Extra", w.get(w.b));
    w.pass();
    w.run(std::make_unique<DonTopo::ApplyPrefabOverridesCommand>(
        w.ctx(), w.b, std::vector<DonTopo::PrefabOverride>{}, true));
    DonTopo::GameObject* copy = childNamed(w.get(w.c), "Extra");
    if (!copy) { CHECK(false, "C received the child"); return; }
    const uint64_t copyId = copy->id;
    auto rename = [&](const std::string& v) { if (DonTopo::GameObject* n = w.get(copyId)) n->name = v; };
    rename("Renamed");
    w.undo.push(std::make_unique<DonTopo::PropertyCommand<std::string>>("Name", "Extra", "Renamed", rename));
    w.pass();
    CHECK(hasOverride(w.get(w.c), "/name"), "the rename is an override of C");

    w.doUndo(); w.doUndo();
    CHECK(!w.get(copyId), "undo of Apply removes C's copy");
    w.doRedo();
    CHECK(w.get(copyId) && w.get(copyId)->parent == w.get(w.c), "redo of Apply gives C's copy the same id");
    w.doRedo();
    CHECK(w.get(copyId) && w.get(copyId)->name == "Renamed", "the later rename lands on C's copy");
    CHECK(hasOverride(w.get(w.c), "/name"), "and is C's override again");
    CHECK(idsUnique(w.scene), "ids unique");
}

// T4: the .prefab changed on disk since it was loaded -> Apply refuses and
// touches nothing (building on the stale template would erase that change).
void testApplyRefusesPrefabChangedOnDisk()
{
    ThreeInstances w("dt_prefab_cmd_t4");
    if (!w.ok()) { CHECK(false, "three synced instances"); return; }
    lampLight(w.get(w.b))->setIntensity(3.0f);
    w.pass();
    const std::string external = enemyFile(w.root.str(), 99.0f).dump(2);
    writeFile(w.prefabFile(), external);
    DonTopo::ApplyPrefabOverridesCommand apply(w.ctx(), w.b, std::vector<DonTopo::PrefabOverride>{}, false);
    apply.execute();
    CHECK(!apply.succeeded() && contains(apply.lastMessage(), "changed on disk"), "refused, with the reason");
    CHECK(readBytes(w.prefabFile()) == external, "the file is untouched");
    CHECK(overrideIs(w.get(w.b), "/light/intensity", 3.0) && w.intensity(w.c) == 1.0f && w.range(w.c) == 10.0f,
          "the scene is untouched");
}

// T5: a path outside the project root is refused (paths are project-relative),
// and a failed write leaves no file behind.
void testCreateRefusesOutsideRootAndFailedWrite()
{
    CommandWorld w("dt_prefab_cmd_t5");
    TempRoot outside("dt_prefab_cmd_t5_outside");
    DonTopo::GameObject* enemy = w.scene.addGameObject("Enemy");
    const std::filesystem::path out = outside.p / "Prefabs" / "Enemy.prefab";
    DonTopo::CreatePrefabCommand c1(w.ctx(), enemy->id, out);
    c1.execute();
    CHECK(!c1.succeeded() && !std::filesystem::exists(out), "outside the project: refused, no file");
    CHECK(contains(c1.lastMessage(), "outside the project"), "with the reason");
    CHECK(!enemy->prefabLink && enemy->prefabNode == 0, "nothing linked");

    const std::filesystem::path missing = w.root.p / "NoSuchFolder" / "Enemy.prefab";
    DonTopo::CreatePrefabCommand c2(w.ctx(), enemy->id, missing);
    c2.execute();
    CHECK(!c2.succeeded() && !std::filesystem::exists(missing), "failed write: refused, no file");
    CHECK(!enemy->prefabLink && enemy->prefabNode == 0, "nothing linked after a failed write");
}

// T6: undo of Create drops the template from the library (and its GPU pin);
// redo registers it again.
void testCreateUndoEvictsTemplate()
{
    CommandWorld w("dt_prefab_cmd_t6");
    DonTopo::GameObject* enemy = w.scene.addGameObject("Enemy");
    w.scene.addGameObject("Lamp", enemy)->setLight(std::make_shared<DonTopo::LightComponent>());
    w.run(std::make_unique<DonTopo::CreatePrefabCommand>(w.ctx(), enemy->id, w.prefabFile()));
    CHECK(settleSync(*w.sync, *w.lib), "settles");
    CHECK(readyTemplate(*w.lib, kEnemyKey) && w.sync->pinned().count(kEnemyKey), "Ready and pinned");
    w.doUndo();
    CHECK(!std::filesystem::exists(w.prefabFile()), "file deleted");
    CHECK(w.lib->find(kEnemyKey) == nullptr, "the template is evicted");
    CHECK(!w.sync->pinned().count(kEnemyKey) && w.fx.count(w.fx.unpins, kEnemyKey) == 1, "and unpinned");
    w.doRedo();
    CHECK(settleSync(*w.sync, *w.lib), "settles after redo");
    CHECK(readyTemplate(*w.lib, kEnemyKey) != nullptr, "redo registers it again");
}

// T8: the template changed between a Revert/Unpack and its undo (an Apply of
// another instance): the instance is re-derived from the CURRENT template with
// its restored overrides, ids kept.
void testUndoAfterTemplateChanged()
{
    ThreeInstances w("dt_prefab_cmd_t8");
    if (!w.ok()) { CHECK(false, "three synced instances"); return; }
    lampLight(w.get(w.b))->setIntensity(5.0f);
    lampLight(w.get(w.a))->setRange(50.0f);
    w.pass();
    const uint64_t lampB = childNamed(w.get(w.b), "Lamp")->id;
    w.run(std::make_unique<DonTopo::RevertPrefabOverridesCommand>(
        w.ctx(), w.b, std::vector<DonTopo::PrefabOverride>{}, false));
    // Applied outside the stack: the template changes under the Revert.
    DonTopo::ApplyPrefabOverridesCommand applyRange(
        w.ctx(), w.a, std::vector<DonTopo::PrefabOverride>{ {2, "/light/range", 50.0} }, false);
    applyRange.execute();
    w.pass();
    CHECK(applyRange.succeeded() && w.range(w.b) == 50.0f, "B follows the new template");
    w.doUndo();   // the Revert
    CHECK(w.intensity(w.b) == 5.0f && overrideIs(w.get(w.b), "/light/intensity", 5.0) &&
          overrideCount(w.get(w.b)) == 1, "Revert undo: its override back");
    CHECK(w.range(w.b) == 50.0f, "Revert undo: the rest follows the current template");
    CHECK(w.get(lampB) && w.get(lampB)->parent == w.get(w.b) && w.synced(w.b), "ids kept, synced");
    w.pass();
    CHECK(overrideCount(w.get(w.b)) == 1, "stable after another pass");

    const uint64_t lampC = childNamed(w.get(w.c), "Lamp")->id;
    w.run(std::make_unique<DonTopo::UnpackPrefabCommand>(w.ctx(), w.c));
    lampLight(w.get(w.a))->setIntensity(8.0f);
    w.pass();
    DonTopo::ApplyPrefabOverridesCommand applyIntensity(
        w.ctx(), w.a, std::vector<DonTopo::PrefabOverride>{ {2, "/light/intensity", 8.0} }, false);
    applyIntensity.execute();
    w.pass();
    CHECK(applyIntensity.succeeded() && w.intensity(w.c) == 1.0f, "the unpacked C does not follow");
    w.doUndo();   // the Unpack
    CHECK(w.get(w.c) && w.get(w.c)->prefabLink && overrideCount(w.get(w.c)) == 0, "Unpack undo: linked, no override");
    CHECK(w.intensity(w.c) == 8.0f && w.range(w.c) == 50.0f, "Unpack undo: re-derived from the current template");
    CHECK(w.get(lampC) && w.get(lampC)->parent == w.get(w.c) && w.synced(w.c), "ids kept, synced");
    CHECK(idsUnique(w.scene), "ids unique");
}

// ---- Re-review of 2203a28 ----

const char* kGroupKey = "Prefabs/Group.prefab";

// Group (1) -> A (2), B (3): two skinned nodes on the same FBX with DIFFERENT
// animation-source lists (B adds an external clip file). Only the first list
// is in t.meshes; B's mesh is in t.configuredMeshes.
void writeGroupPrefab(const TempRoot& root)
{
    namespace fs = std::filesystem;
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
    writeFile(root.p / kGroupKey, nlohmann::json{ {"version", 1}, {"type", "prefab"}, {"root", group} }.dump(2));
}

// U1: Apply/undo/redo rebuild the other instances seeded with the template's
// configured meshes: the main thread reads no FBX (no Assimp reconfiguration
// of B's second source list per instance).
void testApplySeedsConfiguredMeshes()
{
    CommandWorld w("dt_prefab_cmd_u1");
    writeGroupPrefab(w.root);
    w.lib->request(kGroupKey);
    CHECK(settleSync(*w.sync, *w.lib), "settles");
    const DonTopo::PrefabTemplate* t = readyTemplate(*w.lib, kGroupKey);
    if (!t) { CHECK(false, "template Ready"); return; }
    {
        // The hand-written file is not in nodeToJson's canonical form (no
        // clip lists, no defaults), so every instance would differ from it
        // everywhere. Re-saved as the editor writes a prefab (makePrefab).
        DonTopo::GameObject* tmp = w.scene.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
        if (!tmp) { CHECK(false, "temporary instance"); return; }
        const nlohmann::json canonical = w.scene.makePrefab(tmp, kGroupKey, /*link=*/false);
        w.scene.removeGameObject(tmp);
        writeFile(w.root.p / kGroupKey, canonical.dump(2));
        w.lib->updateFromMemory(kGroupKey, canonical, audio());
        CHECK(settleSync(*w.sync, *w.lib), "canonical template settles");
        t = readyTemplate(*w.lib, kGroupKey);
        if (!t) { CHECK(false, "canonical template Ready"); return; }
    }
    DonTopo::GameObject* i1 = w.scene.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
    DonTopo::GameObject* i2 = w.scene.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
    DonTopo::GameObject* i3 = w.scene.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
    if (!i1 || !i2 || !i3) { CHECK(false, "instances"); return; }
    const uint64_t id1 = i1->id, id2 = i2->id, id3 = i3->id;
    w.sync->sceneLoaded(w.scene.toJson());
    childNamed(i1, "A")->name = "Renamed";
    w.pass();
    CHECK(hasOverride(w.get(id1), "/name"), "the rename is an override");

    auto mainReads = [] { return DonTopo::ModelLoader::readFileCountOnThisThread(); };
    auto bSources = [&](uint64_t id) {
        DonTopo::GameObject* b = childNamed(w.get(id), "B");
        const DonTopo::SkinnedMesh* sk = b ? b->getSkinnedMesh() : nullptr;
        return sk ? sk->animationSources.size() : size_t(0);
    };
    int before = mainReads();
    auto* apply = w.run(std::make_unique<DonTopo::ApplyPrefabOverridesCommand>(
        w.ctx(), id1, std::vector<DonTopo::PrefabOverride>{}, false));
    CHECK(apply->succeeded(), "apply succeeded");
    CHECK(mainReads() - before == 0, "execute: no FBX read on the main thread");
    CHECK(childNamed(w.get(id2), "Renamed") && childNamed(w.get(id3), "Renamed"), "the others took the rename");
    CHECK(bSources(id2) == 2 && bSources(id3) == 2, "B keeps its two animation sources");
    CHECK(settleSync(*w.sync, *w.lib), "settles");

    before = mainReads();
    w.doUndo();
    CHECK(mainReads() - before == 0, "undo: no FBX read on the main thread");
    CHECK(childNamed(w.get(id2), "A") && hasOverride(w.get(id1), "/name"), "undo restored");
    CHECK(settleSync(*w.sync, *w.lib), "settles");
    before = mainReads();
    w.doRedo();
    CHECK(mainReads() - before == 0, "redo: no FBX read on the main thread");
    CHECK(childNamed(w.get(id2), "Renamed") && bSources(id2) == 2, "redo applied");
    CHECK(settleSync(*w.sync, *w.lib), "settles");
}

// U2: Apply (execute and undo) refuses, touching nothing, while a mesh of an
// instance is still on its way from the async loader (its result would land on
// a rebuilt node and leak the rebuild's GPU slot).
void testApplyRefusedWhileMeshPending()
{
    ThreeInstances w("dt_prefab_cmd_u2");
    if (!w.ok()) { CHECK(false, "three synced instances"); return; }
    lampLight(w.get(w.b))->setIntensity(3.0f);
    w.pass();
    const uint64_t lampC = childNamed(w.get(w.c), "Lamp")->id;
    const std::string original = readBytes(w.prefabFile());
    w.get(lampC)->pendingMeshJob = 777;   // live (the fixture's loader says every request is)

    DonTopo::ApplyPrefabOverridesCommand apply(w.ctx(), w.b, std::vector<DonTopo::PrefabOverride>{}, false);
    apply.execute();
    CHECK(!apply.succeeded() && contains(apply.lastMessage(), "still loading"), "execute refused");
    CHECK(readBytes(w.prefabFile()) == original && overrideIs(w.get(w.b), "/light/intensity", 3.0) &&
          w.intensity(w.c) == 1.0f, "nothing touched");

    w.get(lampC)->pendingMeshJob = 0;
    apply.execute();
    CHECK(apply.succeeded() && w.intensity(w.c) == 3.0f, "executes once nothing is pending");
    const std::string applied = readBytes(w.prefabFile());
    w.get(lampC)->pendingMeshJob = 777;
    apply.undo();
    CHECK(contains(apply.lastMessage(), "still loading"), "undo refused");
    CHECK(readBytes(w.prefabFile()) == applied && overrideCount(w.get(w.b)) == 0 && w.intensity(w.c) == 3.0f,
          "undo touched nothing");
    w.get(lampC)->pendingMeshJob = 0;
    apply.undo();
    CHECK(readBytes(w.prefabFile()) == original && overrideIs(w.get(w.b), "/light/intensity", 3.0) &&
          w.intensity(w.c) == 1.0f, "undo works once nothing is pending");
}

// U3: redo of Apply while the template is still reloading from the undo: the
// redo rebuilds from the bytes it wrote (no Ready template needed), recreates
// the same ids, and reads no model file on the main thread.
void testApplyRedoWhileTemplateReloads()
{
    CommandWorld w("dt_prefab_cmd_u3");
    const std::string tri = (w.root.p / "Models" / "tri.obj").string();
    writeFile(tri, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    nlohmann::json file = enemyFile(w.root.str(), 10.0f);
    file["root"]["children"][0]["mesh"] = { {"sourcePath", tri}, {"name", "tri"} };
    writeFile(w.prefabFile(), file.dump(2));
    w.lib->request(kEnemyKey);
    CHECK(settleSync(*w.sync, *w.lib), "settles");
    const DonTopo::PrefabTemplate* t = readyTemplate(*w.lib, kEnemyKey);
    if (!t) { CHECK(false, "template Ready"); return; }
    uint64_t ids[3] = {};
    for (uint64_t& id : ids)
    {
        DonTopo::GameObject* inst = w.scene.instantiatePrefab(*t, nullptr, nullptr, nullptr, physics(), audio());
        if (!inst) { CHECK(false, "instance"); return; }
        id = inst->id;
    }
    const uint64_t a = ids[0], b = ids[1], c = ids[2];
    w.sync->sceneLoaded(w.scene.toJson());
    CHECK(childNamed(w.get(a), "Lamp")->hasMesh(), "the lamp has the prefab's mesh");

    // B drops the lamp's mesh and adds a child; Apply all -> the template no
    // longer has the mesh (the library drops it).
    childNamed(w.get(b), "Lamp")->setMesh(nullptr);
    w.scene.addGameObject("Extra", w.get(b));
    w.pass();
    CHECK(hasOverride(w.get(b), "/mesh"), "mesh removal is an override");
    auto* apply = w.run(std::make_unique<DonTopo::ApplyPrefabOverridesCommand>(
        w.ctx(), b, std::vector<DonTopo::PrefabOverride>{}, true));
    CHECK(apply->succeeded(), "apply succeeded");
    CHECK(settleSync(*w.sync, *w.lib), "settles");
    DonTopo::GameObject* copy = childNamed(w.get(c), "Extra");
    if (!copy) { CHECK(false, "C received the child"); return; }
    const uint64_t copyId = copy->id;
    auto rename = [&](const std::string& v) { if (DonTopo::GameObject* n = w.get(copyId)) n->name = v; };
    rename("Renamed");
    w.undo.push(std::make_unique<DonTopo::PropertyCommand<std::string>>("Name", "Extra", "Renamed", rename));
    w.pass();

    const int before = DonTopo::ModelLoader::readFileCountOnThisThread();
    w.doUndo(); w.doUndo();
    CHECK(w.lib->find(kEnemyKey) && w.lib->find(kEnemyKey)->state == DonTopo::PrefabTemplate::State::Loading,
          "undo reloads the template (its mesh is needed again)");
    CHECK(childNamed(w.get(a), "Lamp")->hasMesh() && childNamed(w.get(c), "Lamp")->hasMesh(),
          "undo gives A and C their mesh back");
    CHECK(!w.get(copyId), "undo removed C's copy");
    w.doRedo();
    CHECK(apply->succeeded(), "redo while the template reloads succeeds");
    CHECK(w.get(copyId) && w.get(copyId)->parent == w.get(c), "redo recreates C's copy with the same id");
    w.doRedo();
    CHECK(w.get(copyId) && w.get(copyId)->name == "Renamed", "the later rename lands");
    CHECK(DonTopo::ModelLoader::readFileCountOnThisThread() - before == 0, "no model file read on the main thread");
    CHECK(settleSync(*w.sync, *w.lib), "settles");
    w.pass();
    CHECK(!childNamed(w.get(a), "Lamp")->hasMesh() && overrideCount(w.get(b)) == 0 &&
          hasOverride(w.get(c), "/name") && overrideCount(w.get(c)) == 1, "consistent after the reload");
    CHECK(idsUnique(w.scene), "ids unique");
}

// U4: an edit made while unpacked that takes a field back to the prefab value
// is a revert: the stored override goes, no redundant entry is merged.
void testUnpackUndoRevertToTemplate()
{
    ThreeInstances w("dt_prefab_cmd_u4");
    if (!w.ok()) { CHECK(false, "three synced instances"); return; }
    lampLight(w.get(w.b))->setIntensity(5.0f);
    lampLight(w.get(w.b))->setRange(40.0f);
    w.pass();
    w.run(std::make_unique<DonTopo::UnpackPrefabCommand>(w.ctx(), w.b));
    lampLight(w.get(w.b))->setIntensity(1.0f);   // the prefab's value
    w.undo.undo();                                // no pass yet: what the undo itself leaves
    CHECK(!overrideAt(w.get(w.b), "/light/intensity"), "the revert removes the stored override");
    CHECK(overrideIs(w.get(w.b), "/light/range", 40.0) && overrideCount(w.get(w.b)) == 1, "the rest kept");
    CHECK(w.intensity(w.b) == 1.0f && w.range(w.b) == 40.0f, "values kept");
    w.pass();
    CHECK(overrideCount(w.get(w.b)) == 1 && w.synced(w.b), "stable after a pass");
}

// V1: a refused undo/redo leaves the command where it was (no move, no
// revision), and a command refused on its first execute is not pushed.
void testUndoManagerKeepsRefusedCommands()
{
    ThreeInstances w("dt_prefab_cmd_v1");
    if (!w.ok()) { CHECK(false, "three synced instances"); return; }
    lampLight(w.get(w.b))->setIntensity(3.0f);
    w.pass();
    const std::string original = readBytes(w.prefabFile());
    const uint64_t lampC = childNamed(w.get(w.c), "Lamp")->id;

    // Refused on its first execute: not pushed.
    w.get(lampC)->pendingMeshJob = 777;
    {
        auto refused = std::make_unique<DonTopo::ApplyPrefabOverridesCommand>(
            w.ctx(), w.b, std::vector<DonTopo::PrefabOverride>{}, false);
        refused->execute();
        const uint64_t rev = w.undo.revision();
        CHECK(!w.undo.push(std::move(refused)), "a refused command is not pushed");
        CHECK(!w.undo.canUndo() && w.undo.revision() == rev, "the history is untouched");
    }
    w.get(lampC)->pendingMeshJob = 0;

    auto* apply = w.run(std::make_unique<DonTopo::ApplyPrefabOverridesCommand>(
        w.ctx(), w.b, std::vector<DonTopo::PrefabOverride>{}, false));
    const std::string applied = readBytes(w.prefabFile());
    CHECK(applied != original && w.intensity(w.c) == 3.0f, "applied");
    // A desync calling execute() on an applied command: refused, nothing re-applied.
    apply->execute();
    CHECK(!apply->lastStepSucceeded() && apply->succeeded() && readBytes(w.prefabFile()) == applied,
          "execute on an applied command is refused");

    // Refused undo: stays on the undo stack, revision unchanged, nothing touched.
    w.get(lampC)->pendingMeshJob = 777;
    uint64_t rev = w.undo.revision();
    w.undo.undo();
    CHECK(w.undo.canUndo() && !w.undo.canRedo(), "a refused undo stays on the undo stack");
    CHECK(w.undo.revision() == rev && w.undo.lastRefused(), "no revision change, reported as refused");
    CHECK(!w.undo.lastDiscardedHistory(), "a transient refusal keeps the history");
    CHECK(readBytes(w.prefabFile()) == applied && w.intensity(w.c) == 3.0f && overrideCount(w.get(w.b)) == 0,
          "file and scene unchanged");
    w.get(lampC)->pendingMeshJob = 0;
    w.undo.undo();
    w.pass();
    CHECK(!w.undo.canUndo() && w.undo.canRedo() && !w.undo.lastRefused(), "the undo goes through once the mesh landed");
    CHECK(readBytes(w.prefabFile()) == original && w.intensity(w.c) == 1.0f &&
          overrideIs(w.get(w.b), "/light/intensity", 3.0), "file and scene restored");

    // Refused redo: stays on the redo stack.
    w.get(lampC)->pendingMeshJob = 777;
    rev = w.undo.revision();
    w.undo.redo();
    CHECK(w.undo.canRedo() && !w.undo.canUndo(), "a refused redo stays on the redo stack");
    CHECK(w.undo.revision() == rev && w.undo.lastRefused(), "no revision change");
    CHECK(readBytes(w.prefabFile()) == original && w.intensity(w.c) == 1.0f, "nothing re-applied");
    w.get(lampC)->pendingMeshJob = 0;
    w.undo.redo();
    w.pass();
    CHECK(w.undo.canUndo() && !w.undo.canRedo(), "the redo goes through once the mesh landed");
    CHECK(readBytes(w.prefabFile()) == applied && w.intensity(w.c) == 3.0f, "re-applied");
}

// V1, Create: a redo refused because someone else's file now sits at the path
// stays on the redo stack; the file is untouched.
void testCreateRedoRefusedThroughManager()
{
    CommandWorld w("dt_prefab_cmd_v1_create");
    const uint64_t enemyId = w.scene.addGameObject("Enemy")->id;
    w.run(std::make_unique<DonTopo::CreatePrefabCommand>(w.ctx(), enemyId, w.prefabFile()));
    w.doUndo();
    CHECK(!std::filesystem::exists(w.prefabFile()), "undo deleted the file");
    const std::string other = "{\"someone\": \"else\"}";
    writeFile(w.prefabFile(), other);
    const uint64_t rev = w.undo.revision();
    w.undo.redo();
    CHECK(w.undo.canRedo() && !w.undo.canUndo() && w.undo.revision() == rev && w.undo.lastRefused(),
          "a refused Create redo stays on the redo stack");
    CHECK(readBytes(w.prefabFile()) == other && !w.get(enemyId)->prefabLink, "file untouched, nothing linked");
    std::filesystem::remove(w.prefabFile());
    w.undo.redo();
    CHECK(w.undo.canUndo() && !w.undo.canRedo() && w.get(enemyId)->prefabLink, "it goes through once the path is free");
}

// W1: Apply's undo refused for good (the .prefab changed outside the editor)
// drops that command and everything below it; the redo stack, the revision,
// the scene and the file stay as they were.
void testUnrevertableApplyDiscardsHistory()
{
    ThreeInstances w("dt_prefab_cmd_w1");
    if (!w.ok()) { CHECK(false, "three synced instances"); return; }
    // Older history: an Unpack of C.
    w.run(std::make_unique<DonTopo::UnpackPrefabCommand>(w.ctx(), w.c));
    lampLight(w.get(w.b))->setIntensity(3.0f);
    w.pass();
    w.run(std::make_unique<DonTopo::ApplyPrefabOverridesCommand>(
        w.ctx(), w.b, std::vector<DonTopo::PrefabOverride>{}, false));
    // A later command, undone: it waits on the redo stack.
    const uint64_t aId = w.a;
    auto rename = [&](const std::string& v) { if (DonTopo::GameObject* n = w.get(aId)) n->name = v; };
    const std::string oldName = w.get(aId)->name;
    rename("Renamed");
    w.undo.push(std::make_unique<DonTopo::PropertyCommand<std::string>>("Name", oldName, "Renamed", rename));
    w.pass();
    w.doUndo();
    CHECK(w.undo.canRedo() && w.get(aId)->name == oldName, "the rename is on the redo stack");

    const std::string external = readBytes(w.prefabFile()) + "\n";
    writeFile(w.prefabFile(), external);
    const uint64_t rev = w.undo.revision();
    w.undo.undo();
    CHECK(w.undo.lastDiscardedHistory() && !w.undo.lastRefused(), "reported as history discarded");
    CHECK(w.undo.lastMessage() ==
              "Undo history before 'Apply to Prefab 'Enemy'' discarded: the .prefab changed outside the editor",
          "the exact Log message");
    CHECK(!w.undo.canUndo(), "the Apply and everything below it are gone");
    CHECK(w.undo.canRedo(), "the redo stack is kept");
    CHECK(w.undo.revision() == rev, "the revision does not move (the scene did not change)");
    CHECK(readBytes(w.prefabFile()) == external, "the file is untouched");
    CHECK(w.intensity(w.b) == 3.0f && overrideCount(w.get(w.b)) == 0 && w.intensity(w.a) == 3.0f,
          "the scene stays as Apply left it");
    CHECK(w.get(w.c) && !w.get(w.c)->prefabLink, "the older Unpack was not undone");
    w.undo.redo();
    CHECK(w.get(aId)->name == "Renamed" && w.undo.canUndo(), "the redo stack still works");
}

// ---- Task 11: the pure helpers behind the editor UI ----

// "<name>.prefab", then "<name> 1.prefab", "<name> 2.prefab"... never an
// existing file; a name that cannot be a file name is made into one.
void testUniquePrefabPath()
{
    TempRoot root("dt_prefab_ui_unique");
    const std::filesystem::path dir = root.p / "Prefabs";
    CHECK(DonTopo::uniquePrefabPath(dir, "Enemy") == dir / "Enemy.prefab", "a free name is used as is");
    writeFile(dir / "Enemy.prefab", "{}");
    CHECK(DonTopo::uniquePrefabPath(dir, "Enemy") == dir / "Enemy 1.prefab", "taken -> ' 1'");
    writeFile(dir / "Enemy 1.prefab", "{}");
    CHECK(DonTopo::uniquePrefabPath(dir, "Enemy") == dir / "Enemy 2.prefab", "both taken -> ' 2'");
    CHECK(DonTopo::uniquePrefabPath(dir, "Boss") == dir / "Boss.prefab", "another name is free");
    CHECK(DonTopo::uniquePrefabPath(dir, "a:b/c*d?") == dir / "a_b_c_d_.prefab",
          "characters a file name cannot hold become '_'");
    CHECK(DonTopo::uniquePrefabPath(dir, "  ") == dir / "Prefab.prefab", "an empty name falls back to 'Prefab'");
    CHECK(DonTopo::uniquePrefabPath(dir, "..") == dir / "Prefab.prefab", "'..' is not a file name either");
}

// A hand-built Enemy instance (Enemy 1 -> Lamp 2) plus an added child under the
// root, with the link path kEnemyKey.
struct UiInstance
{
    DonTopo::Scene scene;
    DonTopo::GameObject* root  = nullptr;
    DonTopo::GameObject* lamp  = nullptr;
    DonTopo::GameObject* added = nullptr;
    DonTopo::GameObject* addedKid = nullptr;
    DonTopo::GameObject* plain = nullptr;
    explicit UiInstance(const std::string& assetRoot)
    {
        root  = makeInstance(scene, memoryRootOf(enemyFile(assetRoot, 10.0f), assetRoot));
        lamp  = childNamed(root, "Lamp");
        added = scene.addGameObject("Added", root);
        addedKid = scene.addGameObject("AddedKid", added);
        plain = scene.addGameObject("Plain");
    }
    bool ok() const { return root && lamp && root->prefabLink && lamp->prefabNode == 2 && root->prefabNode == 1; }
};

// Hierarchy color: linked nodes blue (Instance), red when the template Failed
// or is unknown to the library (Missing); added children and plain nodes None.
void testPrefabNodeState()
{
    TempRoot root("dt_prefab_ui_state");
    UiInstance u(root.str());
    if (!u.ok()) { CHECK(false, "instance built"); return; }
    using S = DonTopo::PrefabNodeState;
    CHECK(DonTopo::prefabNodeState(*u.root, nullptr) == S::Instance, "no library: linked = Instance");
    CHECK(DonTopo::prefabNodeState(*u.lamp, nullptr) == S::Instance, "a linked child too");
    CHECK(DonTopo::prefabNodeState(*u.added, nullptr) == S::None, "an added child is not colored");
    CHECK(DonTopo::prefabNodeState(*u.addedKid, nullptr) == S::None, "nor anything under it");
    CHECK(DonTopo::prefabNodeState(*u.plain, nullptr) == S::None, "a plain node is not colored");
    CHECK(DonTopo::prefabNodeState(u.scene.getRoot(), nullptr) == S::None, "nor the scene root");

    DonTopo::JobSystem js;
    js.start(2);
    {
        DonTopo::PrefabLibrary lib(js, root.str());
        CHECK(DonTopo::prefabNodeState(*u.root, &lib) == S::Missing, "never requested: Missing");
        lib.request(kEnemyKey);   // no file on disk
        CHECK(DonTopo::prefabNodeState(*u.lamp, &lib) == S::Instance, "Loading is not Missing");
        CHECK(settle(lib), "settles");
        CHECK(lib.find(kEnemyKey) && lib.find(kEnemyKey)->state == DonTopo::PrefabTemplate::State::Failed, "Failed");
        CHECK(DonTopo::prefabNodeState(*u.root, &lib) == S::Missing, "Failed template: the root is Missing");
        CHECK(DonTopo::prefabNodeState(*u.lamp, &lib) == S::Missing, "and its linked children");
        CHECK(DonTopo::prefabNodeState(*u.added, &lib) == S::None, "added children keep the normal color");
    }
    writeFile(root.p / kEnemyKey, enemyFile(root.str(), 10.0f).dump(2));
    {
        DonTopo::PrefabLibrary lib(js, root.str());
        lib.request(kEnemyKey);
        CHECK(settle(lib), "settles");
        CHECK(DonTopo::prefabNodeState(*u.lamp, &lib) == S::Instance, "Ready: Instance");
    }
    js.shutdown();
}

void testPrefabInstanceRoot()
{
    TempRoot root("dt_prefab_ui_root");
    UiInstance u(root.str());
    if (!u.ok()) { CHECK(false, "instance built"); return; }
    CHECK(DonTopo::prefabInstanceRoot(u.root) == u.root, "the root is its own instance root");
    CHECK(DonTopo::prefabInstanceRoot(u.lamp) == u.root, "a linked child");
    CHECK(DonTopo::prefabInstanceRoot(u.addedKid) == u.root, "anything under an added child is inside the instance");
    CHECK(DonTopo::prefabInstanceRoot(u.plain) == nullptr, "a plain node is in no instance");
    CHECK(DonTopo::prefabInstanceRoot(&u.scene.getRoot()) == nullptr, "nor the scene root");
    CHECK(DonTopo::prefabInstanceRoot(static_cast<DonTopo::GameObject*>(nullptr)) == nullptr, "null in, null out");
}

// Bold labels: an override at the path or inside it, matched by JSON-pointer
// segments; an override on an ANCESTOR path (a script edit stores the whole
// "/scripts" array) counts only where its value differs from the template's.
void testIsOverridden()
{
    TempRoot root("dt_prefab_ui_overridden");
    UiInstance u(root.str());
    if (!u.ok()) { CHECK(false, "instance built"); return; }
    const nlohmann::json scripts = nlohmann::json::array({
        { {"name", "Mover"}, {"overrides", { {"speed", 9}, {"label", "a"} }} } });
    u.root->prefabLink->overrides = {
        {2, "/light/intensity", 5.0},
        {1, "/name", "Boss"},
        {1, "/transform/scale", nlohmann::json::array({2.0, 2.0, 2.0})},
        {2, "/scripts", scripts},
    };
    CHECK(DonTopo::isOverridden(*u.root, 2, "/light"), "an override inside the component");
    CHECK(DonTopo::isOverridden(*u.root, 2, "/light/intensity"), "exactly at the path");
    CHECK(!DonTopo::isOverridden(*u.root, 2, "/light/range"), "a sibling field is not");
    CHECK(!DonTopo::isOverridden(*u.root, 1, "/light"), "another node is not");
    CHECK(DonTopo::isOverridden(*u.root, 1, "/name"), "the name");
    CHECK(!DonTopo::isOverridden(*u.root, 1, "/nam"), "a prefix of a segment is not a match");
    CHECK(!DonTopo::isOverridden(*u.root, 2, "/lig"), "nor inside a segment");
    CHECK(DonTopo::isOverridden(*u.root, 1, "/transform/scale"), "transform part");
    CHECK(!DonTopo::isOverridden(*u.root, 1, "/transform/position"), "another transform part");

    // Ancestor override: without the template, conservative (overridden).
    CHECK(DonTopo::isOverridden(*u.root, 2, "/scripts/0/overrides/speed"), "covered by /scripts");
    // With the template node: only the props whose value differs.
    nlohmann::json tmplLamp = { {"scripts", nlohmann::json::array({
        { {"name", "Mover"}, {"overrides", { {"speed", 3}, {"label", "a"} }} } })} };
    CHECK(DonTopo::isOverridden(*u.root, 2, "/scripts/0/overrides/speed", &tmplLamp), "speed differs");
    CHECK(!DonTopo::isOverridden(*u.root, 2, "/scripts/0/overrides/label", &tmplLamp), "label equals the template's");
    CHECK(!DonTopo::isOverridden(*u.root, 2, "/scripts/0/overrides/missing", &tmplLamp), "absent on both sides");
    nlohmann::json tmplNoScripts = nlohmann::json::object();
    CHECK(DonTopo::isOverridden(*u.root, 2, "/scripts/0/overrides/label", &tmplNoScripts),
          "absent in the template, present in the instance");
    CHECK(DonTopo::isOverridden(*u.root, 2, "/scripts", &tmplLamp), "the header of the scripts");

    // What a right click on such a label applies or reverts.
    const auto under = DonTopo::overridesUnder(*u.root, 2, "/scripts/0/overrides/speed");
    CHECK(under.size() == 1 && under[0].path == "/scripts", "the covering override");
    const auto light = DonTopo::overridesUnder(*u.root, 2, "/light");
    CHECK(light.size() == 1 && light[0].path == "/light/intensity", "the overrides inside the component");
    CHECK(DonTopo::overridesUnder(*u.root, 2, "/mesh").empty(), "none");
    CHECK(!DonTopo::isOverridden(*u.plain, 1, "/name"), "a node with no link has nothing overridden");
}

// Deleting a prefab node from an instance is blocked (Unpack first); the
// instance root, added children and plain nodes can go.
void testCanDeleteNode()
{
    TempRoot root("dt_prefab_ui_delete");
    UiInstance u(root.str());
    if (!u.ok()) { CHECK(false, "instance built"); return; }
    CHECK(!DonTopo::canDeleteNode(*u.lamp), "a linked child cannot be deleted");
    CHECK(DonTopo::canDeleteNode(*u.root), "the instance root can");
    CHECK(DonTopo::canDeleteNode(*u.added), "an added child can");
    CHECK(DonTopo::canDeleteNode(*u.addedKid), "and what hangs from it");
    CHECK(DonTopo::canDeleteNode(*u.plain), "a plain node can");
    CHECK(!DonTopo::canDeleteNode(u.scene.getRoot()), "the scene root cannot");
    // A stale prefabNode outside any instance (e.g. a hand-edited file) does not
    // lock the object forever.
    u.plain->prefabNode = 7;
    CHECK(DonTopo::canDeleteNode(*u.plain), "an orphan prefabNode outside an instance can");
}

// Hierarchy drag: no nesting (an instance under another instance would be
// read as nodes of the outer template), and the structure of an instance is
// the prefab's (moving a linked node is like deleting it from the instance).
void testPrefabMoveRefusal()
{
    TempRoot root("dt_prefab_ui_move");
    UiInstance u(root.str());
    if (!u.ok()) { CHECK(false, "instance built"); return; }
    DonTopo::GameObject* other = makeInstance(u.scene, memoryRootOf(enemyFile(root.str(), 10.0f), root.str()));
    DonTopo::GameObject* holder = u.scene.addGameObject("Holder");
    DonTopo::GameObject* holderKid = u.scene.addGameObject("HolderKid", holder);
    if (!other) { CHECK(false, "second instance"); return; }
    DonTopo::GameObject* sceneRoot = &u.scene.getRoot();

    CHECK(!DonTopo::prefabMoveRefusal(*u.lamp, *u.plain).empty(), "a linked child cannot leave its instance");
    CHECK(!DonTopo::prefabMoveRefusal(*u.lamp, *u.root).empty(), "nor be reordered inside it");
    CHECK(!DonTopo::prefabMoveRefusal(*other, *u.lamp).empty(), "an instance under a linked node: nested");
    CHECK(!DonTopo::prefabMoveRefusal(*other, *u.root).empty(), "under an instance root: nested");
    CHECK(!DonTopo::prefabMoveRefusal(*other, *u.added).empty(), "under an added child: still inside");
    CHECK(DonTopo::prefabMoveRefusal(*other, *holder).empty(), "an instance under a plain node is fine");
    CHECK(DonTopo::prefabMoveRefusal(*other, *sceneRoot).empty(), "or at the scene root");
    CHECK(DonTopo::prefabMoveRefusal(*u.root, *sceneRoot).empty(), "the instance root itself moves freely");
    CHECK(DonTopo::prefabMoveRefusal(*u.added, *holder).empty(), "an added child can leave");
    CHECK(DonTopo::prefabMoveRefusal(*holderKid, *u.lamp).empty(), "a plain node can become an added child");
    u.scene.reparent(other, holder);   // holder now contains an instance
    CHECK(!DonTopo::prefabMoveRefusal(*holder, *u.lamp).empty(),
          "a plain node carrying an instance cannot go inside another instance");
    const std::string why = DonTopo::prefabMoveRefusal(*u.lamp, *u.plain);
    CHECK(contains(why, "Unpack"), "the message says how to do it");
}

// A dropped asset path -> the key of a script's Prefab slot / a hierarchy drop:
// project-relative, only .prefab files inside the project.
void testPrefabKeyForAsset()
{
    TempRoot root("dt_prefab_ui_key");
    DonTopo::JobSystem js;
    js.start(1);
    {
        DonTopo::PrefabLibrary lib(js, root.str());
        const auto k = DonTopo::prefabKeyForAsset((root.p / "Prefabs" / "Enemy.prefab").string(), lib);
        CHECK(k && *k == kEnemyKey, "absolute inside the project -> project-relative key");
        const auto upper = DonTopo::prefabKeyForAsset((root.p / "Prefabs" / "Big.PREFAB").string(), lib);
        CHECK(upper && *upper == "Prefabs/Big.PREFAB", "the extension in any case");
        CHECK(!DonTopo::prefabKeyForAsset((root.p / "Models" / "tri.obj").string(), lib), "not a prefab");
        const std::filesystem::path outside = std::filesystem::temp_directory_path() / "elsewhere" / "Out.prefab";
        CHECK(!DonTopo::prefabKeyForAsset(outside.string(), lib), "outside the project");
        CHECK(!DonTopo::prefabKeyForAsset("", lib), "empty");
    }
    js.shutdown();
}

// No nesting through the command either (any caller, not only the hierarchy).
void testInstantiateRefusesNestedParent()
{
    CommandWorld w("dt_prefab_ui_nested");
    writeFile(w.prefabFile(), enemyFile(w.root.str(), 10.0f).dump(2));
    w.lib->request(kEnemyKey);
    CHECK(settleSync(*w.sync, *w.lib), "settles");
    auto* first = w.run(std::make_unique<DonTopo::InstantiatePrefabCommand>(w.ctx(), kEnemyKey, 0));
    if (!first->succeeded()) { CHECK(false, "first instance"); return; }
    const uint64_t lampId = childNamed(w.get(first->instanceRootId()), "Lamp")->id;
    const uint64_t addedId = w.scene.addGameObject("Added", w.get(first->instanceRootId()))->id;

    for (uint64_t parent : { first->instanceRootId(), lampId, addedId })
    {
        DonTopo::InstantiatePrefabCommand nested(w.ctx(), kEnemyKey, parent);
        nested.execute();
        CHECK(!nested.succeeded() && contains(nested.lastMessage(), "nested"), "refused under an instance");
    }
    int instances = 0;
    w.scene.traverse([&](DonTopo::GameObject* n) { if (n->prefabLink) ++instances; });
    CHECK(instances == 1, "nothing was built");
}

// Prefab commands cannot ride inside a CompositeCommand: it would neither
// report their refusals nor discard the history under an unrevertable Apply.
void testCompositeRefusesPrefabCommands()
{
    CommandWorld w("dt_prefab_ui_composite");
    DonTopo::CompositeCommand group("Group");
    const uint64_t id = w.scene.addGameObject("Enemy")->id;
    CHECK(!group.add(std::make_unique<DonTopo::CreatePrefabCommand>(w.ctx(), id, w.prefabFile())),
          "a prefab command is refused");
    CHECK(group.empty(), "and not added");
    CHECK(group.add(std::make_unique<DonTopo::PropertyCommand<int>>("x", 0, 1, [](const int&) {})),
          "any other command is added");
    CHECK(!group.empty(), "added");
}

// The one way the editor runs a prefab command: execute, push only if it was
// applied, and log its message either way.
void testPushPrefabCommand()
{
    CommandWorld w("dt_prefab_ui_push");
    std::vector<std::string> log;
    auto logFn = [&](const std::string& m) { log.push_back(m); };

    const uint64_t id = w.scene.addGameObject("Enemy")->id;
    CHECK(DonTopo::pushPrefabCommand(w.undo,
              std::make_unique<DonTopo::CreatePrefabCommand>(w.ctx(), id, w.prefabFile()), logFn),
          "an applied command is pushed");
    CHECK(w.undo.canUndo() && std::filesystem::exists(w.prefabFile()), "executed once and on the stack");
    CHECK(log.size() == 1 && contains(log[0], "Created prefab"), "its message is logged");

    const uint64_t rev = w.undo.revision();
    const uint64_t other = w.scene.addGameObject("Other")->id;
    CHECK(!DonTopo::pushPrefabCommand(w.undo,
              std::make_unique<DonTopo::CreatePrefabCommand>(w.ctx(), other, w.prefabFile()), logFn),
          "a refused command is not pushed");
    CHECK(w.undo.revision() == rev, "the history did not move");
    CHECK(log.size() == 2 && contains(log[1], "already exists"), "the refusal is logged");
    CHECK(!DonTopo::pushPrefabCommand(w.undo, nullptr, logFn), "null: nothing");
}

// Per-row Apply / Revert of ONE added child in the Overrides popup: the
// instance's overrides stay where they are (prefabNoOverrides()).
void testApplyRevertSingleAddedChild()
{
    ThreeInstances w("dt_prefab_ui_added_rows");
    if (!w.ok()) { CHECK(false, "three synced instances"); return; }
    lampLight(w.get(w.b))->setIntensity(5.0f);
    const uint64_t x = w.scene.addGameObject("ExtraX", w.get(w.b))->id;
    const uint64_t y = w.scene.addGameObject("ExtraY", w.get(w.b))->id;
    w.pass();
    CHECK(overrideIs(w.get(w.b), "/light/intensity", 5.0), "B overrides the intensity");

    // Revert only X.
    auto* rx = w.run(std::make_unique<DonTopo::RevertPrefabOverridesCommand>(
        w.ctx(), w.b, DonTopo::prefabNoOverrides(), true, std::vector<uint64_t>{ x }));
    CHECK(rx->succeeded(), "revert of one added child");
    CHECK(!w.get(x) && w.get(y), "only X is deleted");
    CHECK(overrideIs(w.get(w.b), "/light/intensity", 5.0) && w.intensity(w.b) == 5.0f, "the override stays");
    w.doUndo();
    CHECK(w.get(x) && w.get(x)->parent == w.get(w.b), "undo: X is back with its id");

    // Apply only Y.
    auto* ay = w.run(std::make_unique<DonTopo::ApplyPrefabOverridesCommand>(
        w.ctx(), w.b, DonTopo::prefabNoOverrides(), true, std::vector<uint64_t>{ y }));
    CHECK(ay->succeeded(), "apply of one added child");
    CHECK(contains(readBytes(w.prefabFile()), "ExtraY") && !contains(readBytes(w.prefabFile()), "ExtraX"),
          "only Y is written into the prefab");
    CHECK(overrideIs(w.get(w.b), "/light/intensity", 5.0) && w.intensity(w.a) == 1.0f,
          "the override is not applied");
    CHECK(countChildren(w.get(w.a), "ExtraY") == 1 && countChildren(w.get(w.a), "ExtraX") == 0,
          "the other instances get Y only");

    // Nothing selected and no children: nothing to do, refused.
    DonTopo::RevertPrefabOverridesCommand none(w.ctx(), w.a, DonTopo::prefabNoOverrides(), false);
    none.execute();
    CHECK(!none.succeeded(), "an empty selection is refused");
}

// A template that Failed (file missing) loads again once its .prefab shows
// up on disk -- restored, recreated, moved back -- without a scene reload,
// and its instances turn from Missing to Instance.
void testFailedTemplateRetriedWhenFileReturns()
{
    CommandWorld w("dt_prefab_ui_retry");
    const nlohmann::json file = enemyFile(w.root.str(), 20.0f);
    DonTopo::GameObject* inst = makeInstance(w.scene, memoryRootOf(enemyFile(w.root.str(), 10.0f), w.root.str()));
    if (!inst) { CHECK(false, "instance"); return; }
    const uint64_t id = inst->id;
    w.sync->sceneLoaded(w.scene.toJson());
    CHECK(settleSync(*w.sync, *w.lib), "settles");
    const DonTopo::PrefabTemplate* t = w.lib->find(kEnemyKey);
    CHECK(t && t->state == DonTopo::PrefabTemplate::State::Failed, "no file: Failed");
    CHECK(DonTopo::prefabNodeState(*w.get(id), w.lib.get()) == DonTopo::PrefabNodeState::Missing, "Missing");

    // Unchanged file: no retry, however many frames.
    for (int i = 0; i < 3 * DonTopo::PrefabSceneSync::kFailedPollFrames; ++i) w.sync->pump(1.0f);
    CHECK(w.lib->find(kEnemyKey) && w.lib->find(kEnemyKey)->state == DonTopo::PrefabTemplate::State::Failed,
          "still Failed while nothing changed on disk");
    const int readsBefore = w.lib->readCount();

    writeFile(w.prefabFile(), file.dump(2));
    bool ready = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!ready && std::chrono::steady_clock::now() < deadline)
    {
        w.sync->pump(1000.0f);
        const DonTopo::PrefabTemplate* now = w.lib->find(kEnemyKey);
        ready = now && now->state == DonTopo::PrefabTemplate::State::Ready;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(ready, "the restored file is loaded");
    CHECK(w.lib->readCount() == readsBefore + 1, "read once");
    CHECK(w.fx.logged("reloading"), "the Log says it is reloading");
    CHECK(DonTopo::prefabNodeState(*w.get(id), w.lib.get()) == DonTopo::PrefabNodeState::Instance,
          "the instance is no longer Missing");
    CHECK(lampLight(w.get(id)) && lampLight(w.get(id))->getRange() == 20.0f,
          "and was propagated from the restored prefab");

    // Gone again and back with other content: the Ready template is not
    // touched by the poll (only Failed keys are watched).
    const int readsReady = w.lib->readCount();
    for (int i = 0; i < 3 * DonTopo::PrefabSceneSync::kFailedPollFrames; ++i) w.sync->pump(1.0f);
    CHECK(w.lib->readCount() == readsReady, "a Ready template is never re-read by the poll");
}

// Drop of a .prefab in Edit Mode: a template not loaded yet is waited for
// synchronously (spec §5.2); a missing one fails at once with its error.
void testWaitForTemplate()
{
    CommandWorld w("dt_prefab_ui_wait");
    writeFile(w.prefabFile(), enemyFile(w.root.str(), 10.0f).dump(2));
    std::string error;
    CHECK(w.sync->waitForTemplate(kEnemyKey, 60000, &error), "a loadable prefab is waited for");
    CHECK(error.empty(), "no error");
    CHECK(readyTemplate(*w.lib, kEnemyKey) != nullptr, "Ready on return");
    CHECK(!w.sync->waitForTemplate("Prefabs/Nope.prefab", 60000, &error), "a missing one is not");
    CHECK(!error.empty(), "and says why");
}

int main()
{
    testUniquePrefabPath();
    testPrefabNodeState();
    testPrefabInstanceRoot();
    testIsOverridden();
    testCanDeleteNode();
    testPrefabMoveRefusal();
    testPrefabKeyForAsset();
    testInstantiateRefusesNestedParent();
    testCompositeRefusesPrefabCommands();
    testPushPrefabCommand();
    testApplyRevertSingleAddedChild();
    testFailedTemplateRetriedWhenFileReturns();
    testWaitForTemplate();
    testUnrevertableApplyDiscardsHistory();
    testUndoManagerKeepsRefusedCommands();
    testCreateRedoRefusedThroughManager();
    testApplySeedsConfiguredMeshes();
    testApplyRefusedWhileMeshPending();
    testApplyRedoWhileTemplateReloads();
    testUnpackUndoRevertToTemplate();
    testApplyUndoKeptLeavesSceneAsApplied();
    testUndoKeepsCommandlessEdits();
    testApplyRedoReusesPropagatedIds();
    testApplyRefusesPrefabChangedOnDisk();
    testCreateRefusesOutsideRootAndFailedWrite();
    testCreateUndoEvictsTemplate();
    testUndoAfterTemplateChanged();
    testCreatePrefabCommand();
    testCreateUndoKeepsModifiedFile();
    testApplyKeepsOtherInstanceOverride();
    testApplyAddedChildWhileLoading();
    testRevertPrefabOverridesCommand();
    testUnpackPrefabCommand();
    testInstantiatePrefabCommand();
    testSettleLandedMeshesBounded();
    testRemovingAddedComponentDuringReload();
    testRevertDetectionAddedComponentEdit();
    testRevertDetectionReaddedComponent();
    testMaterialEditAfterMeshLandsIsKept();
    testAsyncLoadAddsNoSpuriousOverride();
    testCloneDuringLoadingKeepsEdit();
    testRevertDuringReloadFollowsNewPrefab();
    testValuesEqualIntegersExact();
    testTrackerSkipsUnsyncedInstances();
    testTrackerSkipsTemplateReloading();
    testTrackerIgnoresInstanceUntilPropagated();
    testLoadPropagatesNewerPrefab();
    testPropagationMatchesNormalizedLinks();
    testPropagationSeededWithInstanceMeshes();
    testSyncKeepsEditMadeWhileLoading();
    testSyncKeepsEditDuringApplyReload();
    testSyncDeferredRetryBoundedAndCancel();
    testSyncLoadPinEvictDecisions();
    testSyncFallingEdgeRecomputesSelection();
    testLoaderRequestLiveness();
    testEditorReleaseIsFinal();
    if (g_failures) { std::fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    std::printf("prefab_editor_tests OK\n");
    return 0;
}
