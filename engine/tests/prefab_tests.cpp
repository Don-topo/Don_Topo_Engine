// Tests of the prefab system (spec docs/superpowers/specs/2026-09-30-prefabs-design.md).
#include "DonTopo/Core/PrefabOverrides.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <cstdio>

namespace {
int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__); \
    ++g_failures; } } while (0)

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
} // namespace

int main()
{
    testDiffDetectsLeafChange();
    testDiffIgnoresIdsAndRootPose();
    testDiffComponentAddedAndRemoved();
    testApplyRoundTripAndOrphans();
    testOverridesJsonRoundTrip();
    if (g_failures) { std::fprintf(stderr, "%d failure(s)\n", g_failures); return 1; }
    std::printf("prefab_tests OK\n");
    return 0;
}
