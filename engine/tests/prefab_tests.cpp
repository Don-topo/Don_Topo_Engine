// Tests of the prefab system (spec docs/superpowers/specs/2026-09-30-prefabs-design.md).
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Core/PrefabOverrides.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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
} // namespace

int main()
{
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
    if (g_failures) { std::fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    std::printf("prefab_tests OK\n");
    return 0;
}
