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
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
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

int main()
{
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
