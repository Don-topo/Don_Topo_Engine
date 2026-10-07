#pragma once
#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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

    // ── Keyed arrays ────────────────────────────────────────────────────────
    // Two arrays of a node are recorded PER ELEMENT, not whole, so that an
    // instance overriding one element (or one field of it) does not shadow the
    // others (Review Focus 2 for scripts and materials). Their elements are
    // addressed by a stable KEY in the path, not by position:
    //  - "/scripts": key "<name>#<k>", the k-th script with that name (k from
    //    0). "/scripts/Mover#0/overrides/speed" is one script property;
    //    "/scripts/Extra#0" with an object value is a script the instance
    //    added, with null a template script it removed. The virtual
    //    "/scripts/#order" (value: the instance's keys in order) is recorded
    //    only when the instance's order differs from the template's order
    //    followed by its added scripts.
    //  - "/mesh/materials": key "<index>", the entry's submesh index. The array
    //    is sparse (only slots with something to say), so a missing entry is
    //    the same as {"index": k}: "/mesh/materials/1/roughness" works whether
    //    or not either side has an entry for slot 1. Order is irrelevant.
    // Every other array (vectors like positions and colors, mesh
    // animationSources, procedural vertices/indices, the animator graph,
    // dropdown options) is a single value and is recorded whole.
    // An override stored on a keyed array's root with an array (or null) value
    // is the format before keyed arrays; applyPrefabOverrides converts it to
    // per-element entries (see there).
    bool isKeyedArrayPath(std::string_view path);
    // The key of element i of a keyed array at arrayPath ("" for other paths).
    std::string prefabArrayElementKey(std::string_view arrayPath, const nlohmann::json& array, size_t i);
    // "<name>#<occurrence>": the key of a script element.
    std::string prefabScriptKey(const std::string& name, size_t occurrence);
    // The value at an override path inside ONE node json (memory form),
    // resolving keyed segments and "#order" (the keys in order). A keyed
    // array whose key is absent reads as []. nullopt when there is nothing.
    // "/transform/*" is virtual and is not resolved here.
    std::optional<nlohmann::json> prefabValueAt(const nlohmann::json& node, std::string_view path);

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
    // Keyed-array entries of a node are applied together after its other
    // entries, against the keys the template array had BEFORE any of them
    // (removing "Mover#0" never turns "Mover#1" into "Mover#0" mid-way);
    // "#order" last. A whole-array entry of the old format is replaced by the
    // per-element entries of its difference with the template array, and it
    // is THOSE that are returned (so a rebuild stores the converted list).
    std::vector<PrefabOverride> applyPrefabOverrides(nlohmann::json& prefabRoot,
                                                     const std::vector<PrefabOverride>& ovs,
                                                     std::vector<std::string>* warnings);
}
