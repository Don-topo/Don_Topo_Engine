#include "DonTopo/Core/PrefabOverrides.h"
#include "DonTopo/Core/TransformDecompose.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
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

    const char* const kScriptsPath   = "/scripts";
    const char* const kMaterialsPath = "/mesh/materials";
    const char* const kOrder         = "#order";

    // JSON-pointer segments of path, unescaped (~1 -> '/', ~0 -> '~'). false
    // when path is not a pointer ("" is the root: no segments).
    bool splitPointer(std::string_view path, std::vector<std::string>& out)
    {
        out.clear();
        if (path.empty()) return true;
        if (path[0] != '/') return false;
        size_t pos = 1;
        for (;;)
        {
            const size_t end = path.find('/', pos);
            const std::string_view raw = path.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
            std::string seg;
            for (size_t k = 0; k < raw.size(); ++k)
            {
                if (raw[k] == '~' && k + 1 < raw.size() && (raw[k + 1] == '0' || raw[k + 1] == '1'))
                {
                    seg.push_back(raw[k + 1] == '0' ? '~' : '/');
                    ++k;
                }
                else seg.push_back(raw[k]);
            }
            out.push_back(std::move(seg));
            if (end == std::string_view::npos) return true;
            pos = end + 1;
        }
    }

    std::string appendSegment(const std::string& path, const std::string& seg)
    {
        return (json::json_pointer(path) / seg).to_string();
    }

    // "/scripts" or "/mesh/materials" when path is one of them or lies inside
    // one; "" otherwise.
    std::string keyedRootOf(std::string_view path)
    {
        for (const char* root : { kScriptsPath, kMaterialsPath })
        {
            const std::string_view r(root);
            if (path.size() >= r.size() && path.compare(0, r.size(), r) == 0 &&
                (path.size() == r.size() || path[r.size()] == '/'))
                return std::string(root);
        }
        return {};
    }

    void diffObjects(uint64_t localId, const json::json_pointer& at, const json& p, const json& i,
                     bool topLevel, std::vector<PrefabOverride>& out);

    json emptyMaterialSlot(const std::string& key)
    {
        json slot = json::object();
        try { slot["index"] = std::stoi(key); } catch (const std::exception&) {}
        return slot;
    }

    // One keyed array: element by element, by key (see the header).
    void diffKeyed(uint64_t localId, const std::string& root, const json& p, const json& i,
                   std::vector<PrefabOverride>& out)
    {
        const bool scripts = root == kScriptsPath;
        auto keysOf = [&](const json& a) {
            std::vector<std::pair<std::string, const json*>> keys;
            if (a.is_array())
                for (size_t k = 0; k < a.size(); ++k) keys.emplace_back(prefabArrayElementKey(root, a, k), &a[k]);
            return keys;
        };
        const auto pk = keysOf(p);
        const auto ik = keysOf(i);
        auto find = [](const std::vector<std::pair<std::string, const json*>>& v, const std::string& key) -> const json* {
            for (const auto& [k, e] : v) if (k == key) return e;
            return nullptr;
        };
        for (const auto& [key, pe] : pk)
        {
            const std::string path = appendSegment(root, key);
            const json* ie = find(ik, key);
            if (!ie && scripts)
                out.push_back({localId, path, json(nullptr)});
            else
            {
                const json other = ie ? *ie : emptyMaterialSlot(key);
                if (pe->is_object() && other.is_object())
                    diffObjects(localId, json::json_pointer(path), *pe, other, false, out);
                else if (*pe != other)
                    out.push_back({localId, path, other});
            }
        }
        for (const auto& [key, ie] : ik)
        {
            if (find(pk, key)) continue;
            const std::string path = appendSegment(root, key);
            if (scripts)
                out.push_back({localId, path, *ie});
            else if (ie->is_object())
                diffObjects(localId, json::json_pointer(path), emptyMaterialSlot(key), *ie, false, out);
        }
        if (!scripts) return;
        // Order: recorded only when it is not "the template's order, then the
        // added scripts in the instance's order".
        json expected = json::array(), actual = json::array();
        for (const auto& [key, pe] : pk) if (find(ik, key)) expected.push_back(key);
        for (const auto& [key, ie] : ik) if (!find(pk, key)) expected.push_back(key);
        for (const auto& [key, ie] : ik) actual.push_back(key);
        if (expected != actual) out.push_back({localId, appendSegment(root, kOrder), actual});
    }

    void diffObjects(uint64_t localId, const json::json_pointer& at, const json& p, const json& i,
                     bool topLevel, std::vector<PrefabOverride>& out)
    {
        for (auto it = p.begin(); it != p.end(); ++it)
        {
            if (topLevel && isSkippedKey(it.key())) continue;
            const json::json_pointer path = at / it.key();
            auto other = i.find(it.key());
            if (const std::string ps = path.to_string(); isKeyedArrayPath(ps))
            {
                diffKeyed(localId, ps, *it, other == i.end() ? json(nullptr) : *other, out);
                continue;
            }
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
            if (p.find(it.key()) != p.end()) continue;
            const std::string ps = (at / it.key()).to_string();
            if (isKeyedArrayPath(ps)) diffKeyed(localId, ps, json(nullptr), *it, out);
            else                      out.push_back({localId, ps, *it});
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
        // A parsed file gives unsigned ids; a template built in code ({"id", 2})
        // a signed one. Both count when positive (as Scene's localIdOf).
        auto id = node.find("id");
        if (id != node.end() && id->is_number_unsigned()) map.emplace(id->get<uint64_t>(), &node);
        else if (id != node.end() && id->is_number_integer() && id->get<int64_t>() > 0)
            map.emplace(static_cast<uint64_t>(id->get<int64_t>()), &node);
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

bool isKeyedArrayPath(std::string_view path)
{
    return path == kScriptsPath || path == kMaterialsPath;
}

std::string prefabScriptKey(const std::string& name, size_t occurrence)
{
    return name + "#" + std::to_string(occurrence);
}

std::string prefabArrayElementKey(std::string_view arrayPath, const json& array, size_t i)
{
    if (!array.is_array() || i >= array.size()) return {};
    const json& e = array[i];
    if (arrayPath == kScriptsPath)
    {
        const std::string name = e.is_object() ? e.value("name", std::string()) : std::string();
        size_t occurrence = 0;
        for (size_t k = 0; k < i; ++k)
            if (array[k].is_object() && array[k].value("name", std::string()) == name) ++occurrence;
        return prefabScriptKey(name, occurrence);
    }
    if (arrayPath == kMaterialsPath)
    {
        if (e.is_object())
            if (const auto idx = e.find("index"); idx != e.end() && idx->is_number_integer())
                return std::to_string(idx->get<int64_t>());
        return "@" + std::to_string(i);   // malformed entry: by position, never matches a real slot
    }
    return {};
}

std::optional<json> prefabValueAt(const json& node, std::string_view path)
{
    std::vector<std::string> segs;
    if (!splitPointer(path, segs)) return std::nullopt;
    const json* cur = &node;
    std::string at;
    for (size_t s = 0; s < segs.size(); ++s)
    {
        const std::string& seg = segs[s];
        if (isKeyedArrayPath(at))
        {
            static const json kEmpty = json::array();
            const json& arr = (cur && cur->is_array()) ? *cur : kEmpty;
            if (seg == kOrder)
            {
                if (s + 1 != segs.size() || at != kScriptsPath) return std::nullopt;
                json keys = json::array();
                for (size_t k = 0; k < arr.size(); ++k) keys.push_back(prefabArrayElementKey(at, arr, k));
                return keys;
            }
            const json* found = nullptr;
            for (size_t k = 0; k < arr.size() && !found; ++k)
                if (prefabArrayElementKey(at, arr, k) == seg) found = &arr[k];
            if (!found) return std::nullopt;
            cur = found;
        }
        else if (!cur)
            return std::nullopt;
        else if (cur->is_object())
        {
            const auto it = cur->find(seg);
            if (it != cur->end())
                cur = &*it;
            else if (isKeyedArrayPath(appendSegment(at, seg)))
                cur = nullptr;   // an absent keyed array reads as []
            else
                return std::nullopt;
        }
        else if (cur->is_array())
        {
            size_t k = 0;
            if (seg.empty() || seg.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
            try { k = std::stoul(seg); } catch (const std::exception&) { return std::nullopt; }
            if (k >= cur->size()) return std::nullopt;
            cur = &(*cur)[k];
        }
        else
            return std::nullopt;
        at = appendSegment(at, seg);
    }
    if (!cur) return isKeyedArrayPath(at) ? std::optional<json>(json::array()) : std::nullopt;
    return *cur;
}

namespace
{
    // One entry on a plain path (no keyed array on the way), as always.
    bool applyPlain(json& node, const PrefabOverride& o, const std::string& idStr, std::vector<std::string>* warnings)
    {
        const std::string orphan =
            "prefab override " + o.path + " on node " + idStr + " dropped: component no longer in the prefab";
        static const std::string kTransform = "/transform/";
        if (o.path.compare(0, kTransform.size(), kTransform) == 0)
        {
            if (applyTransformPart(node, o.path.substr(kTransform.size()), o.value)) return true;
            warn(warnings, orphan);
            return false;
        }
        json::json_pointer ptr;
        try { ptr = json::json_pointer(o.path); }
        catch (const json::exception&)
        {
            warn(warnings, "prefab override " + o.path + " on node " + idStr + " dropped: invalid path");
            return false;
        }
        if (ptr.empty())
        {
            warn(warnings, "prefab override on node " + idStr + " dropped: empty path");
            return false;
        }
        // A single-segment path is a whole component: adding it needs no parent.
        const bool wholeComponent = ptr.parent_pointer().empty();
        if (o.value.is_null())
        {
            if (!node.contains(ptr)) { warn(warnings, orphan); return false; }
            json& parent = node[ptr.parent_pointer()];
            if (!parent.is_object()) { warn(warnings, orphan); return false; }
            parent.erase(ptr.back());
            return true;
        }
        // The component (or nested object) the leaf lives in must still exist.
        if (!wholeComponent && (!node.contains(ptr.parent_pointer()) || !node[ptr.parent_pointer()].is_object()))
        {
            warn(warnings, orphan);
            return false;
        }
        node[ptr] = o.value;
        return true;
    }

    // Every entry of ONE keyed array of ONE node, together (see the header).
    void applyKeyed(json& node, const std::string& root, const std::vector<PrefabOverride>& ops,
                    const std::string& idStr, std::vector<PrefabOverride>& applied, std::vector<std::string>* warnings)
    {
        const bool scripts = root == kScriptsPath;
        auto orphanOf = [&](const PrefabOverride& o) {
            return "prefab override " + o.path + " on node " + idStr + " dropped: " +
                   (scripts ? "script" : "component") + " no longer in the prefab";
        };
        // The object holding the array: the node, or its mesh.
        json* holder = &node;
        if (!scripts)
        {
            const auto mesh = node.find("mesh");
            if (mesh == node.end() || !mesh->is_object())
            {
                for (const PrefabOverride& o : ops) warn(warnings, orphanOf(o));
                return;
            }
            holder = &*mesh;
        }
        const char* field = scripts ? "scripts" : "materials";
        static const json kEmpty = json::array();
        const auto arrIt = holder->find(field);
        const json& arr = (arrIt != holder->end() && arrIt->is_array()) ? *arrIt : kEmpty;

        // The elements with the keys they had BEFORE any entry.
        struct Element { std::string key; json value; bool removed = false; };
        std::vector<Element> elements, added;
        for (size_t k = 0; k < arr.size(); ++k) elements.push_back({ prefabArrayElementKey(root, arr, k), arr[k] });
        auto find = [&](const std::string& key) -> Element* {
            for (Element& e : elements) if (!e.removed && e.key == key) return &e;
            for (Element& e : added) if (!e.removed && e.key == key) return &e;
            return nullptr;
        };

        // Old format (a whole array on the root): its difference with the
        // template array, as per-element entries.
        std::vector<PrefabOverride> expanded;
        for (const PrefabOverride& o : ops)
        {
            if (o.path != root) { expanded.push_back(o); continue; }
            if (!o.value.is_null() && !o.value.is_array()) { warn(warnings, orphanOf(o)); continue; }
            diffKeyed(o.node, root, arr, o.value, expanded);
        }

        std::optional<json> order;
        for (const PrefabOverride& o : expanded)
        {
            std::vector<std::string> segs;
            if (!splitPointer(std::string_view(o.path).substr(root.size()), segs) || segs.empty())
            {
                warn(warnings, "prefab override " + o.path + " on node " + idStr + " dropped: invalid path");
                continue;
            }
            const std::string& key = segs[0];
            if (key == kOrder)
            {
                if (scripts && o.value.is_array()) { order = o.value; applied.push_back(o); }
                else warn(warnings, orphanOf(o));
                continue;
            }
            Element* e = find(key);
            if (segs.size() == 1)
            {
                // The whole element: added / replaced, or removed.
                if (o.value.is_null())
                {
                    if (!e) { warn(warnings, orphanOf(o)); continue; }
                    e->removed = true;
                }
                else if (e) e->value = o.value;
                else        added.push_back({ key, o.value });
                applied.push_back(o);
                continue;
            }
            // A field inside an element. A material slot without an entry is
            // an empty slot; a script the prefab does not have is an orphan.
            if (!e)
            {
                if (scripts || o.value.is_null()) { warn(warnings, orphanOf(o)); continue; }
                added.push_back({ key, emptyMaterialSlot(key) });
                e = &added.back();
            }
            std::string sub;
            for (size_t s = 1; s < segs.size(); ++s) sub = appendSegment(sub, segs[s]);
            PrefabOverride inner{ o.node, sub, o.value };
            // Inside an element nothing is a "whole component": a leaf needs its parent.
            json wrapped = { {"e", e->value} };
            inner.path = "/e" + sub;
            if (!applyPlain(wrapped, inner, idStr, nullptr)) { warn(warnings, orphanOf(o)); continue; }
            e->value = std::move(wrapped["e"]);
            // A material slot with nothing left to say is not written (nodeToJson).
            if (!scripts && e->value.is_object() && e->value.size() <= 1) e->removed = true;
            applied.push_back(o);
        }

        json result = json::array();
        std::vector<std::string> keys;
        for (const auto* list : { &elements, &added })
            for (const Element& e : *list)
                if (!e.removed) { result.push_back(e.value); keys.push_back(e.key); }
        if (order)
        {
            json sorted = json::array();
            std::vector<bool> used(keys.size(), false);
            for (const json& k : *order)
                for (size_t i = 0; i < keys.size(); ++i)
                    if (!used[i] && k.is_string() && keys[i] == k.get<std::string>()) { sorted.push_back(result[i]); used[i] = true; break; }
            for (size_t i = 0; i < keys.size(); ++i) if (!used[i]) sorted.push_back(result[i]);
            result = std::move(sorted);
        }
        if (result.empty()) holder->erase(field);
        else                (*holder)[field] = std::move(result);
    }
}

std::vector<PrefabOverride> applyPrefabOverrides(json& prefabRoot,
                                                 const std::vector<PrefabOverride>& ovs,
                                                 std::vector<std::string>* warnings)
{
    std::vector<PrefabOverride> applied;
    std::unordered_map<uint64_t, json*> nodes;
    indexNodes(prefabRoot, nodes);

    // Keyed-array entries per (node, array), in first-seen order: applied after
    // the node's plain entries (one may have replaced the whole mesh).
    std::vector<std::pair<std::pair<uint64_t, std::string>, std::vector<PrefabOverride>>> keyed;
    for (const auto& o : ovs)
    {
        const std::string idStr = std::to_string(o.node);
        if (nodes.find(o.node) == nodes.end())
        {
            warn(warnings, "prefab override on node " + idStr + " dropped: node no longer in the prefab");
            continue;
        }
        if (const std::string root = keyedRootOf(o.path); !root.empty())
        {
            auto group = std::find_if(keyed.begin(), keyed.end(), [&](const auto& g) {
                return g.first.first == o.node && g.first.second == root;
            });
            if (group == keyed.end()) keyed.push_back({ { o.node, root }, { o } });
            else                      group->second.push_back(o);
            continue;
        }
        if (applyPlain(*nodes[o.node], o, idStr, warnings)) applied.push_back(o);
    }
    for (const auto& [target, ops] : keyed)
        applyKeyed(*nodes[target.first], target.second, ops, std::to_string(target.first), applied, warnings);
    return applied;
}
}
