#include "DonTopo/Core/PrefabOverrides.h"
#include "DonTopo/Core/TransformDecompose.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <unordered_map>

namespace DonTopo
{
namespace
{
    using json = nlohmann::json;

    constexpr float kEps = 1e-5f;

    // Same column-major 16-float layout Scene.cpp writes (mat4ToJson).
    bool readMat4(const json& j, glm::mat4& out)
    {
        if (!j.is_array() || j.size() != 16) return false;
        float* p = glm::value_ptr(out);
        for (int i = 0; i < 16; ++i)
        {
            if (!j[i].is_number()) return false;
            p[i] = j[i].get<float>();
        }
        return true;
    }

    json writeMat4(const glm::mat4& m)
    {
        json a = json::array();
        const float* p = glm::value_ptr(m);
        for (int i = 0; i < 16; ++i) a.push_back(p[i]);
        return a;
    }

    struct Trs { glm::vec3 pos{0.0f}; glm::quat rot{1.0f, 0.0f, 0.0f, 0.0f}; glm::vec3 scale{1.0f}; };

    // false when the matrix cannot be decomposed (singular: one axis at scale 0).
    // decomposeTransform always leaves valid outputs, but a singular matrix has
    // no real rotation, so a diff/patch over its parts would lie: callers fall
    // back to treating the whole matrix as one leaf.
    bool decompose(const glm::mat4& m, Trs& out)
    {
        return decomposeTransform(m, &out.pos, &out.rot, &out.scale);
    }

    glm::mat4 compose(const Trs& t)
    {
        return glm::translate(glm::mat4(1.0f), t.pos) * glm::mat4_cast(t.rot) *
               glm::scale(glm::mat4(1.0f), t.scale);
    }

    json vec3Json(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
    json quatJson(const glm::quat& q) { return json::array({q.x, q.y, q.z, q.w}); }

    bool readVec(const json& j, size_t n, float* out)
    {
        if (!j.is_array() || j.size() != n) return false;
        for (size_t i = 0; i < n; ++i)
        {
            if (!j[i].is_number()) return false;
            out[i] = j[i].get<float>();
        }
        return true;
    }

    bool near(const glm::vec3& a, const glm::vec3& b)
    {
        return std::fabs(a.x - b.x) <= kEps && std::fabs(a.y - b.y) <= kEps &&
               std::fabs(a.z - b.z) <= kEps;
    }

    // q and -q are the same rotation; decompose may return either sign.
    // Signs are aligned first and then components are compared with the same
    // 1e-5 as position/scale. A |dot| >= 1 - eps test would be ~0.5 degrees
    // wide (1 - cos(t/2) ~ t^2/8) and silently drop small rotations.
    bool nearRot(const glm::quat& a, glm::quat b)
    {
        if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f)
            b = glm::quat(-b.w, -b.x, -b.y, -b.z);
        return std::fabs(a.x - b.x) <= kEps && std::fabs(a.y - b.y) <= kEps &&
               std::fabs(a.z - b.z) <= kEps && std::fabs(a.w - b.w) <= kEps;
    }

    bool isSkippedKey(const std::string& k)
    {
        return k == "id" || k == "children" || k == "prefab" || k == "prefabNode" ||
               k == "localTransform";
    }

    void diffObjects(uint64_t localId, const json::json_pointer& at, const json& p, const json& i,
                     bool topLevel, std::vector<PrefabOverride>& out)
    {
        for (auto it = p.begin(); it != p.end(); ++it)
        {
            if (topLevel && isSkippedKey(it.key())) continue;
            const json::json_pointer path = at / it.key();
            auto other = i.find(it.key());
            if (other == i.end())
            {
                out.push_back({localId, path.to_string(), json(nullptr)});
            }
            else if (it->is_object() && other->is_object())
            {
                diffObjects(localId, path, *it, *other, false, out);
            }
            else if (*it != *other)
            {
                out.push_back({localId, path.to_string(), *other});
            }
        }
        for (auto it = i.begin(); it != i.end(); ++it)
        {
            if (topLevel && isSkippedKey(it.key())) continue;
            if (p.find(it.key()) == p.end())
                out.push_back({localId, (at / it.key()).to_string(), *it});
        }
    }

    void diffTransform(uint64_t localId, const json& p, const json& i, bool isInstanceRoot,
                       std::vector<PrefabOverride>& out)
    {
        auto pt = p.find("localTransform");
        auto it = i.find("localTransform");
        // Legacy position/rotation/scale nodes carry no localTransform: nothing to compare.
        if (pt == p.end() || it == i.end()) return;

        glm::mat4 pm(1.0f), im(1.0f);
        if (!readMat4(*pt, pm) || !readMat4(*it, im)) return;

        Trs a, b;
        if (!decompose(pm, a) || !decompose(im, b))
        {
            // Singular on either side: no meaningful parts, compare the matrix whole.
            bool same = true;
            const float* x = glm::value_ptr(pm);
            const float* y = glm::value_ptr(im);
            for (int k = 0; k < 16; ++k)
                if (std::fabs(x[k] - y[k]) > kEps) { same = false; break; }
            if (!same) out.push_back({localId, "/localTransform", *it});
            return;
        }

        if (!isInstanceRoot)
        {
            if (!near(a.pos, b.pos))
                out.push_back({localId, "/transform/position", vec3Json(b.pos)});
            if (!nearRot(a.rot, b.rot))
                out.push_back({localId, "/transform/rotation", quatJson(b.rot)});
        }
        if (!near(a.scale, b.scale))
            out.push_back({localId, "/transform/scale", vec3Json(b.scale)});
    }

    void indexNodes(json& node, std::unordered_map<uint64_t, json*>& map)
    {
        if (!node.is_object()) return;
        auto id = node.find("id");
        if (id != node.end() && id->is_number_unsigned()) map.emplace(id->get<uint64_t>(), &node);
        auto ch = node.find("children");
        if (ch != node.end() && ch->is_array())
            for (auto& c : *ch) indexNodes(c, map);
    }

    void warn(std::vector<std::string>* w, const std::string& msg)
    {
        if (w) w->push_back(msg);
    }

    // Applies one /transform/<part> override. false = could not (caller warns).
    bool applyTransformPart(json& node, const std::string& part, const json& value)
    {
        auto lt = node.find("localTransform");
        glm::mat4 m(1.0f);
        if (lt == node.end() || !readMat4(*lt, m)) return false;
        Trs t;
        if (!decompose(m, t)) return false;

        float v[4];
        if (part == "position" && readVec(value, 3, v)) t.pos = {v[0], v[1], v[2]};
        else if (part == "scale" && readVec(value, 3, v)) t.scale = {v[0], v[1], v[2]};
        else if (part == "rotation" && readVec(value, 4, v))
            t.rot = glm::normalize(glm::quat(v[3], v[0], v[1], v[2]));   // glm ctor is (w,x,y,z)
        else return false;

        *lt = writeMat4(compose(t));
        return true;
    }
}

json overridesToJson(const std::vector<PrefabOverride>& ovs)
{
    json a = json::array();
    for (const auto& o : ovs)
        a.push_back({{"node", o.node}, {"path", o.path}, {"value", o.value}});
    return a;
}

std::vector<PrefabOverride> overridesFromJson(const json& j, std::vector<std::string>* warnings)
{
    std::vector<PrefabOverride> out;
    if (!j.is_array())
    {
        if (!j.is_null()) warn(warnings, "prefab overrides ignored: not an array");
        return out;
    }
    for (const auto& e : j)
    {
        if (!e.is_object() || !e.contains("node") || !e["node"].is_number_unsigned() ||
            !e.contains("path") || !e["path"].is_string())
        {
            warn(warnings, "prefab override ignored: malformed entry");
            continue;
        }
        PrefabOverride o;
        o.node = e["node"].get<uint64_t>();
        o.path = e["path"].get<std::string>();
        o.value = e.value("value", json(nullptr));
        out.push_back(std::move(o));
    }
    return out;
}

std::vector<PrefabOverride> diffPrefabNode(uint64_t localId, const json& prefabNode,
                                           const json& instanceNode, bool isInstanceRoot)
{
    std::vector<PrefabOverride> out;
    if (!prefabNode.is_object() || !instanceNode.is_object()) return out;
    diffTransform(localId, prefabNode, instanceNode, isInstanceRoot, out);
    diffObjects(localId, json::json_pointer(), prefabNode, instanceNode, true, out);
    return out;
}

std::vector<PrefabOverride> applyPrefabOverrides(json& prefabRoot,
                                                 const std::vector<PrefabOverride>& ovs,
                                                 std::vector<std::string>* warnings)
{
    std::vector<PrefabOverride> applied;
    std::unordered_map<uint64_t, json*> nodes;
    indexNodes(prefabRoot, nodes);

    for (const auto& o : ovs)
    {
        const std::string idStr = std::to_string(o.node);
        auto n = nodes.find(o.node);
        if (n == nodes.end())
        {
            warn(warnings, "prefab override on node " + idStr + " dropped: node no longer in the prefab");
            continue;
        }
        json& node = *n->second;
        const std::string orphan =
            "prefab override " + o.path + " on node " + idStr + " dropped: component no longer in the prefab";

        static const std::string kTransform = "/transform/";
        if (o.path.compare(0, kTransform.size(), kTransform) == 0)
        {
            if (applyTransformPart(node, o.path.substr(kTransform.size()), o.value))
                applied.push_back(o);
            else
                warn(warnings, orphan);
            continue;
        }

        json::json_pointer ptr;
        try { ptr = json::json_pointer(o.path); }
        catch (const json::exception&)
        {
            warn(warnings, "prefab override " + o.path + " on node " + idStr + " dropped: invalid path");
            continue;
        }
        if (ptr.empty())
        {
            warn(warnings, "prefab override on node " + idStr + " dropped: empty path");
            continue;
        }

        // A single-segment path is a whole component: adding it needs no parent.
        const bool wholeComponent = ptr.parent_pointer().empty();
        if (o.value.is_null())
        {
            if (!node.contains(ptr))
            {
                warn(warnings, orphan);
                continue;
            }
            json& parent = node[ptr.parent_pointer()];
            if (parent.is_object()) parent.erase(ptr.back());
            else { warn(warnings, orphan); continue; }
        }
        else
        {
            if (!wholeComponent)
            {
                // The component (or nested object) the leaf lives in must still exist.
                if (!node.contains(ptr.parent_pointer()) || !node[ptr.parent_pointer()].is_object())
                {
                    warn(warnings, orphan);
                    continue;
                }
            }
            node[ptr] = o.value;
        }
        applied.push_back(o);
    }
    return applied;
}
}
