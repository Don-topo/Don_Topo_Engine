#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace DonTopo
{
    // Per-property override model of a prefab instance. Pure JSON in, JSON out:
    // nothing here knows about Scene or GameObject, so it can be tested (and
    // reused by the loader, the editor and the exporter) without a scene.
    //
    // An override says "on prefab-local node `node`, the value at JSON pointer
    // `path` is `value` instead of what the prefab says". Two special shapes:
    //   - `value` null  -> the key was REMOVED in the instance (a component).
    //   - path "/transform/position|rotation|scale" is virtual: it is not a key
    //     of the node json but a part of `localTransform`, so that moving an
    //     instance does not override its scale and vice versa.
    struct PrefabOverride
    {
        uint64_t node = 0;          // prefab-local id of the node, not the instance's runtime id
        std::string path;           // JSON pointer relative to that node's json
        nlohmann::json value;
    };

    struct PrefabLink
    {
        std::string path;                        // prefab asset, project-relative
        std::vector<PrefabOverride> overrides;
    };

    nlohmann::json overridesToJson(const std::vector<PrefabOverride>& ovs);

    // Tolerant: a malformed entry is skipped with a warning, never fatal.
    std::vector<PrefabOverride> overridesFromJson(const nlohmann::json& j,
                                                  std::vector<std::string>* warnings);

    // prefabNode / instanceNode: ONE node json each (children are ignored).
    // localId = the node's prefab-local id, stamped on every returned override.
    // For the instance root, position and rotation are not compared: where the
    // user places an instance in the scene is not an override of the prefab.
    std::vector<PrefabOverride> diffPrefabNode(uint64_t localId, const nlohmann::json& prefabNode,
                                               const nlohmann::json& instanceNode, bool isInstanceRoot);

    // Applies overrides to a whole prefab tree (node "id"s are prefab-local).
    // Orphans (node or component no longer in the prefab) become warnings and
    // are dropped. Returns the overrides that were actually applied.
    std::vector<PrefabOverride> applyPrefabOverrides(nlohmann::json& prefabRoot,
                                                     const std::vector<PrefabOverride>& ovs,
                                                     std::vector<std::string>* warnings);
}
