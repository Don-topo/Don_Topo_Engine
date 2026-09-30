#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/Blend2D.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace DonTopo
{
    static_assert(AnimatorComponent::kMaxLayers == kMaxLayersPose, "AnimationPose and the Animator share the layer cap");
    int AnimatorComponent::addState(State s, int layer)
    {
        Layer& L = lay(layer);
        // Stable editorId for the AnimatorPanel canvas: it never depends on the
        // index in L.states (see the field comment in the header). A fresh state
        // (editor) arrives with -1 and is assigned one here; a copied one (redo of
        // AnimatorComponentCommand) or loaded one (Scene::animatorFromJson, which
        // does not serialize editorId either) also arrives with -1 and falls on the same
        // path, being assigned in load/copy order — stable within the
        // session, which is all the canvas needs. If it already brings an id (a
        // copy of a state that DID have one assigned) it is kept, and the
        // counter is advanced so that the next addState never repeats it.
        if (s.editorId < 0) s.editorId = m_nextEditorId++;
        else                m_nextEditorId = std::max(m_nextEditorId, s.editorId + 1);

        L.states.push_back(std::move(s));
        // First state added: default entry. A graph without an entry does not
        // start, and forcing it to be marked by hand would be a trap to trip on.
        if (L.entryState < 0) L.entryState = 0;
        return (int)L.states.size() - 1;
    }

    void AnimatorComponent::addTransition(Transition t, int layer) { lay(layer).transitions.push_back(std::move(t)); }

    void AnimatorComponent::enterState(int idx, int layer)
    {
        Layer& L = lay(layer);
        // A box is not played: its leaf is entered. If the chain is broken
        // nothing moves, which is the guarantee that currentState is never a
        // box wherever it comes from (transition, Lua Play or the editor).
        // idx < 0 still means "no state": removeState uses it when emptying
        // the layer.
        const int hoja = resolveEntryLeaf(idx, layer);
        if (idx >= 0 && hoja < 0) return;
        L.currentState = hoja;
        L.animTime     = 0.0f;
        L.finished     = false;
        L.stateTicks   = 0.0;
    }

    void AnimatorComponent::removeState(int idx, int layer)
    {
        Layer& L = lay(layer);
        if (idx < 0 || idx >= (int)L.states.size()) return;

        // Descendants go away with the box. Leaving them loose at the root is
        // creating orphans nobody asked for, and undo is a snapshot of the
        // whole graph, so undoing brings them all back.
        //
        // The box is relocated by editorId and NOT by index: deleting a
        // descendant that came before it shifts it, and continuing with the
        // old index would delete another state.
        if (L.states[(size_t)idx].isSubMachine)
        {
            const int idCaja = L.states[(size_t)idx].editorId;
            for (;;)
            {
                const int caja = stateIndexByEditorId(idCaja, layer);
                if (caja < 0) return;               // it is no longer there: nothing to delete
                int hijo = -1;
                for (int i = 0; i < (int)L.states.size(); i++)
                    if (isDescendantOf(i, caja, layer)) { hijo = i; break; }
                if (hijo < 0) { idx = caja; break; }   // no descendants left
                removeState(hijo, layer);              // recursive: a child box takes its own with it
            }
        }

        L.states.erase(L.states.begin() + idx);

        // The hierarchy is reindexed just like the transitions. `subEntry == idx`
        // happens when deleting the state that was the entry of its box: the box is
        // left empty and can no longer be entered, which is correct.
        // `parent == idx` is defensive and today UNREACHABLE —deleting a box
        // takes its descendants first—, so no test covers it; it is there
        // in case someday one is deleted without a cascade.
        for (auto& st : L.states)
        {
            if (st.parent   == idx) st.parent   = -1;
            else if (st.parent   > idx) st.parent--;
            if (st.subEntry == idx) st.subEntry = -1;
            else if (st.subEntry > idx) st.subEntry--;
        }

        // Transitions store indices: deleting a state invalidates those that touch
        // it and shifts those pointing above it. Without this, deleting a node
        // would leave links pointing to a state different from the one the user sees.
        L.transitions.erase(
            std::remove_if(L.transitions.begin(), L.transitions.end(),
                [idx](const Transition& t) { return t.fromState == idx || t.toState == idx; }),
            L.transitions.end());
        for (auto& t : L.transitions)
        {
            if (t.fromState > idx) t.fromState--;
            if (t.toState   > idx) t.toState--;
        }

        if (L.entryState == idx)      L.entryState = L.states.empty() ? -1 : 0;
        else if (L.entryState > idx)  L.entryState--;

        // The playhead is reindexed just like the transitions: deleting ANOTHER state
        // need not move the user from their place. Only if the current one is deleted
        // must it fall to the entry, because the current one no longer exists.
        //
        // Before this was a plain reset(), which also erased all the
        // parameters — and the AnimatorPanel calls here without checking whether it is in
        // Play, so reordering the graph in the middle of a game wiped out the
        // bool/trigger/int/float that the script was writing.
        if (L.currentState == idx)
        {
            enterState(L.entryState, layer);
        }
        else if (L.currentState > idx)
        {
            L.currentState--;
        }

        // The cross-fade is always cut: the state that was being turned off may have gone
        // or been reindexed, and blending against a moved index would give the
        // pose of another clip.
        L.prevState     = -1;
        L.prevAnimTime  = 0.0f;
        L.blendElapsed  = 0.0f;
        L.blendDuration = 0.0f;
        L.frozenFade    = false;
        L.freezePending = false;
    }

    void AnimatorComponent::removeTransition(int idx, int layer)
    {
        Layer& L = lay(layer);
        if (idx < 0 || idx >= (int)L.transitions.size()) return;
        L.transitions.erase(L.transitions.begin() + idx);
    }

    void AnimatorComponent::setEntryState(int idx, int layer)
    {
        Layer& L = lay(layer);
        if (idx < 0 || idx >= (int)L.states.size()) return;
        L.entryState = idx;
        // Moves the playhead to the new entry (the editor preview has to
        // follow it) but without erasing the parameters: this also runs in Play.
        resetPlayback();
    }

    void AnimatorComponent::addParameter(std::string name, ParamType type)
    {
        if (name.empty()) return;
        for (const auto& p : m_parameters)
            if (p.name == name) return;      // unique names: they are queried by name
        m_parameters.push_back({ std::move(name), type });
        const std::string& n = m_parameters.back().name;
        switch (type)
        {
            case ParamType::Bool:    m_bools[n]    = false;  break;
            case ParamType::Trigger: m_triggers[n] = false;  break;
            case ParamType::Int:     m_ints[n]     = 0;      break;
            case ParamType::Float:   m_floats[n]   = 0.0f;   break;
        }
    }

    void AnimatorComponent::removeParameter(const std::string& name)
    {
        // Empty name == the one used by the AnimationFinished conditions (see
        // Condition::paramName); if we went on, the loop below would
        // delete all of them from the graph without the user asking.
        if (name.empty()) return;

        m_parameters.erase(
            std::remove_if(m_parameters.begin(), m_parameters.end(),
                [&name](const Parameter& p) { return p.name == name; }),
            m_parameters.end());
        m_bools.erase(name);
        m_triggers.erase(name);
        m_ints.erase(name);
        m_floats.erase(name);

        for (auto& L : m_layers)
        {
            // The conditions that used it would be left dangling and would never
            // fire: they go with it.
            for (auto& t : L.transitions)
                t.conditions.erase(
                    std::remove_if(t.conditions.begin(), t.conditions.end(),
                        [&name](const Condition& c) { return c.paramName == name; }),
                    t.conditions.end());

            // A transition left without conditions (this was the only one) can
            // never fire (conditionsMet requires at least one), so
            // keeping it would be an invisible, dead link on the canvas. If it kept
            // other conditions, it survives as is.
            L.transitions.erase(
                std::remove_if(L.transitions.begin(), L.transitions.end(),
                    [](const Transition& t) { return t.conditions.empty(); }),
                L.transitions.end());
        }
    }

    AnimatorComponent::Graph AnimatorComponent::graph() const
    {
        const Layer& L = m_layers[0];
        Graph g{ L.states, L.transitions, m_parameters, L.entryState, {}, m_ik, m_propertyClips };
        g.extraLayers.assign(m_layers.begin() + 1, m_layers.end());
        return g;
    }

    void AnimatorComponent::applyGraph(const Graph& g)
    {
        // IK and property clips are entirely design: they come in as is
        // (their indices are rebuilt by rebindClips and bindProperties, like
        // clipIndex).
        m_ik            = g.ik;
        m_propertyClips = g.propertyClips;

        const std::vector<Parameter> oldParams = m_parameters;
        auto oldBools    = m_bools;
        auto oldTriggers = m_triggers;
        auto oldInts     = m_ints;
        auto oldFloats   = m_floats;
        m_parameters = g.parameters;

        m_bools.clear();
        m_triggers.clear();
        m_ints.clear();
        m_floats.clear();
        for (const auto& p : m_parameters)
        {
            bool mismoTipo = false;
            for (const auto& o : oldParams)
                if (o.name == p.name) { mismoTipo = (o.type == p.type); break; }
            switch (p.type)
            {
                case ParamType::Bool:    m_bools[p.name]    = mismoTipo ? oldBools[p.name]    : false; break;
                case ParamType::Trigger: m_triggers[p.name] = mismoTipo ? oldTriggers[p.name] : false; break;
                case ParamType::Int:     m_ints[p.name]     = mismoTipo ? oldInts[p.name]     : 0;     break;
                case ParamType::Float:   m_floats[p.name]   = mismoTipo ? oldFloats[p.name]   : 0.0f;  break;
            }
        }

        // Layers: the base and those of the snapshot. A layer that already existed at that
        // index keeps its playhead (matched by editorId, unique across the whole
        // component) and its Any State on the canvas; a new one starts from zero.
        const int n = 1 + (int)g.extraLayers.size();
        if ((int)m_layers.size() > n) m_layers.resize((size_t)n);
        for (int li = 0; li < n; li++)
        {
            if (li >= (int)m_layers.size())
            {
                Layer nueva = g.extraLayers[(size_t)li - 1];
                nueva.states.clear();
                nueva.transitions.clear();
                nueva.currentState = -1;
                nueva.prevState    = -1;
                nueva.frozenFade   = nueva.freezePending = false;
                m_layers.push_back(std::move(nueva));
            }
            else if (li > 0)
            {
                const Layer& src = g.extraLayers[(size_t)li - 1];
                Layer& L = m_layers[li];
                L.name      = src.name;
                L.weight    = src.weight;
                L.mode      = src.mode;
                L.maskBones = src.maskBones;
            }
            if (li == 0) applyLayerGraph(0, g.states, g.transitions, g.entryState);
            else
            {
                const Layer& src = g.extraLayers[(size_t)li - 1];
                applyLayerGraph(li, src.states, src.transitions, src.entryState);
            }
        }
    }

    void AnimatorComponent::applyLayerGraph(int li, const std::vector<State>& states,
                                            const std::vector<Transition>& transitions, int entryState)
    {
        Layer& L = m_layers[li];
        // Live identities BEFORE replacing anything: after copying the graph it would no longer
        // be known which editorId was the current one, which the one being turned off, nor
        // where each node was on the canvas.
        auto editorIdAt = [&L](int idx) {
            return (idx >= 0 && idx < (int)L.states.size()) ? L.states[idx].editorId : -1;
        };
        const int curId  = editorIdAt(L.currentState);
        const int prevId = editorIdAt(L.prevState);
        std::unordered_map<int, glm::vec2> livePos;
        for (const auto& s : L.states) livePos[s.editorId] = s.editorPos;

        L.states      = states;
        L.transitions = transitions;
        L.entryState  = entryState;

        // Two passes: first advance the counter with the ids the states already
        // bring, then hand out to those that arrive without an id (-1).
        // The other way round, a state without an id could receive one that another already brings.
        for (auto& s : L.states)
        {
            if (s.editorId < 0) continue;
            m_nextEditorId = std::max(m_nextEditorId, s.editorId + 1);
            auto it = livePos.find(s.editorId);
            if (it != livePos.end()) s.editorPos = it->second;
        }
        for (auto& s : L.states)
            if (s.editorId < 0) s.editorId = m_nextEditorId++;

        auto indexOf = [&L](int editorId) {
            if (editorId < 0) return -1;
            for (int i = 0; i < (int)L.states.size(); i++)
                if (L.states[i].editorId == editorId) return i;
            return -1;
        };
        const int cur  = indexOf(curId);
        const int prev = indexOf(prevId);

        if (curId < 0)
        {
            // There was no playhead (graph not started): update() will put it at the
            // entry, just as before applying anything.
            L.currentState = -1;
        }
        else if (cur >= 0)
        {
            L.currentState = cur;
        }
        else
        {
            enterState(L.entryState, li);
        }

        if (L.frozenFade && cur >= 0)
        {
            // The frozen one does not depend on graph indices: the fade continues.
        }
        else if (L.prevState >= 0 && cur >= 0 && prev >= 0)
        {
            L.prevState = prev;
        }
        else
        {
            L.prevState     = -1;
            L.prevAnimTime  = 0.0f;
            L.blendElapsed  = 0.0f;
            L.blendDuration = 0.0f;
            L.frozenFade    = false;
            L.freezePending = false;
        }
    }

    bool AnimatorComponent::hasParam(const std::string& n, ParamType type) const
    {
        for (const auto& p : m_parameters)
            if (p.name == n) return p.type == type;
        return false;
    }

    void AnimatorComponent::setBool(const std::string& n, bool v)
    {
        if (!hasParam(n, ParamType::Bool)) return;
        m_bools[n] = v;
    }

    bool AnimatorComponent::getBool(const std::string& n) const
    {
        auto it = m_bools.find(n);
        return it != m_bools.end() && it->second;
    }

    void AnimatorComponent::setTrigger(const std::string& n)
    {
        if (!hasParam(n, ParamType::Trigger)) return;
        m_triggers[n] = true;
    }

    void AnimatorComponent::resetTrigger(const std::string& n)
    {
        if (!hasParam(n, ParamType::Trigger)) return;
        m_triggers[n] = false;
    }

    bool AnimatorComponent::isTriggerSet(const std::string& n) const
    {
        auto it = m_triggers.find(n);
        return it != m_triggers.end() && it->second;
    }

    void AnimatorComponent::setInt(const std::string& n, int v)
    {
        if (!hasParam(n, ParamType::Int)) return;
        m_ints[n] = v;
    }

    int AnimatorComponent::getInt(const std::string& n) const
    {
        auto it = m_ints.find(n);
        return it != m_ints.end() ? it->second : 0;
    }

    void AnimatorComponent::setFloat(const std::string& n, float v)
    {
        if (!hasParam(n, ParamType::Float)) return;
        m_floats[n] = v;
    }

    float AnimatorComponent::getFloat(const std::string& n) const
    {
        auto it = m_floats.find(n);
        return it != m_floats.end() ? it->second : 0.0f;
    }

    bool AnimatorComponent::hasFloatParameter(const std::string& n) const
    {
        return hasParam(n, ParamType::Float);
    }

    int AnimatorComponent::conditionThresholds(const std::string& n, float* out, int max) const
    {
        if (n.empty() || !out || max <= 0) return 0;
        int cuantos = 0;
        for (const auto& L : m_layers)
            for (const auto& t : L.transitions)
                for (const auto& c : t.conditions)
                {
                    if (c.type != ConditionType::Float || c.paramName != n) continue;
                    // Without repeats: two transitions with the same threshold draw
                    // a single line.
                    bool visto = false;
                    for (int i = 0; i < cuantos; i++)
                        if (out[i] == c.threshold) { visto = true; break; }
                    if (visto) continue;
                    if (cuantos >= max) return cuantos;
                    out[cuantos++] = c.threshold;
                }
        return cuantos;
    }

    int AnimatorComponent::currentClipIndex(int layer) const
    {
        const Layer& L = lay(layer);
        if (L.currentState < 0 || L.currentState >= (int)L.states.size()) return 0;
        const int ci = L.states[L.currentState].clipIndex;
        return ci >= 0 ? ci : 0;
    }

    int AnimatorComponent::previousClipIndex(int layer) const
    {
        const Layer& L = lay(layer);
        if (L.prevState < 0 || L.prevState >= (int)L.states.size()) return 0;
        const int ci = L.states[L.prevState].clipIndex;
        return ci >= 0 ? ci : 0;
    }

    float AnimatorComponent::blendWeight(int layer) const
    {
        const Layer& L = lay(layer);
        // Without a blend the target weighs 100%: this way the consumer does not need to
        // ask beforehand whether there is a cross-fade or not.
        if ((L.prevState < 0 && !L.frozenFade) || L.blendDuration <= 0.0f) return 1.0f;
        const float w = L.blendElapsed / L.blendDuration;
        return w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
    }

    std::string AnimatorComponent::currentStateName(int layer) const
    {
        const Layer& L = lay(layer);
        if (L.currentState < 0 || L.currentState >= (int)L.states.size()) return "";
        return L.states[L.currentState].name;
    }

    bool AnimatorComponent::stateBlends(int stateIdx, int layer) const
    {
        const Layer& L = lay(layer);
        if (stateIdx < 0 || stateIdx >= (int)L.states.size()) return false;
        const State& st = L.states[stateIdx];
        // An entry at -1 = the clip does not exist in the mesh (rebindClips already
        // warned): blending against it would read another clip or outside the SSBO.
        bool alguna = false;
        for (const auto& e : st.blendEntries)
            if (e.clipIndex >= 0) { alguna = true; break; }
        if (!alguna) return false;
        // An undeclared parameter would return 0.0f in getFloat and pin the
        // weight at one extreme without saying why; better not to blend.
        return hasParam(st.blendParam, ParamType::Float);
    }

    AnimatorComponent::BlendPair AnimatorComponent::stateBlendPair(int stateIdx, float animTime, int layer) const
    {
        if (!stateBlends2D(stateIdx, layer)) return stateBlendPair1D(stateIdx, animTime, layer);
        // 2D: the two-clip view is the two samples that weigh the most.
        BlendSample bs[3];
        const int n = stateBlendSamples(stateIdx, animTime, bs, layer);
        int b = 0, a = -1;
        for (int i = 1; i < n; i++) if (bs[i].weight > bs[b].weight) b = i;
        for (int i = 0; i < n; i++) if (i != b && (a < 0 || bs[i].weight > bs[a].weight)) a = i;
        if (a < 0) a = b;
        const float wa = bs[a].weight, wb = bs[b].weight;
        return { bs[a].clip, bs[a].time, bs[b].clip, bs[b].time,
                 (a == b || wa + wb <= 0.0f) ? 1.0f : wb / (wa + wb), bs[a].duration, bs[b].duration };
    }

    bool AnimatorComponent::stateBlends2D(int stateIdx, int layer) const
    {
        const Layer& L = lay(layer);
        return stateBlends(stateIdx, layer) && hasParam(L.states[stateIdx].blendParamY, ParamType::Float);
    }

    int AnimatorComponent::stateBlendSamples(int stateIdx, float animTime, BlendSample out[3], int layer) const
    {
        const Layer& L = lay(layer);
        if (!stateBlends2D(stateIdx, layer))
        {
            const BlendPair bp = stateBlendPair1D(stateIdx, animTime, layer);
            if (bp.clipA == bp.clipB) { out[0] = { bp.clipB, bp.timeB, 1.0f, bp.durB }; return 1; }
            out[0] = { bp.clipA, bp.timeA, 1.0f - bp.weight, bp.durA };
            out[1] = { bp.clipB, bp.timeB, bp.weight, bp.durB };
            return 2;
        }
        const State& st    = L.states[stateIdx];
        const float  phase = st.duration > 0.0f ? animTime / st.duration : 0.0f;
        // Points: the main one first (breaks ties), then the entries with a clip.
        std::vector<glm::vec2> pts;
        std::vector<int>       quien;   // -1 main, k entry
        pts.push_back({ st.clipThreshold, st.clipThresholdY });
        quien.push_back(-1);
        for (int k = 0; k < (int)st.blendEntries.size(); k++)
            if (st.blendEntries[k].clipIndex >= 0)
            {
                pts.push_back({ st.blendEntries[k].threshold, st.blendEntries[k].thresholdY });
                quien.push_back(k);
            }
        Blend2DWeight w[3];
        const int n = blend2DWeights(pts, { getFloat(st.blendParam), getFloat(st.blendParamY) }, w);
        for (int i = 0; i < n; i++)
        {
            const int k = quien[w[i].point];
            if (k < 0) out[i] = { st.clipIndex, animTime, w[i].weight, st.duration };
            else       out[i] = { st.blendEntries[k].clipIndex, phase * st.blendEntries[k].duration,
                                  w[i].weight, st.blendEntries[k].duration };
        }
        return n;
    }

    AnimatorComponent::BlendPair AnimatorComponent::stateBlendPair1D(int stateIdx, float animTime, int layer) const
    {
        const Layer& L = lay(layer);
        const int clip = (stateIdx >= 0 && stateIdx < (int)L.states.size() && L.states[stateIdx].clipIndex >= 0)
                             ? L.states[stateIdx].clipIndex : 0;
        const float durActual = (stateIdx >= 0 && stateIdx < (int)L.states.size()) ? L.states[stateIdx].duration : 0.0f;
        BlendPair out{ clip, animTime, clip, animTime, 1.0f, durActual, durActual };
        if (!stateBlends(stateIdx, layer)) return out;

        const State& st    = L.states[stateIdx];
        const float  p     = getFloat(st.blendParam);
        const float  phase = st.duration > 0.0f ? animTime / st.duration : 0.0f;

        // Lower neighbor (greater threshold <= p) and upper one (smaller threshold > p),
        // without sorting or allocating memory. -1 is the main one, which is looked at
        // first: the STRICT comparison makes the first one seen stay when
        // thresholds are equal.
        const int kNinguno = -2;
        int   lo  = kNinguno, hi  = kNinguno;
        float loT = 0.0f,     hiT = 0.0f;
        auto considerar = [&](int k, float t) {
            if (t <= p) { if (lo == kNinguno || t > loT) { lo = k; loT = t; } }
            else        { if (hi == kNinguno || t < hiT) { hi = k; hiT = t; } }
        };
        considerar(-1, st.clipThreshold);
        for (int k = 0; k < (int)st.blendEntries.size(); k++)
            if (st.blendEntries[k].clipIndex >= 0)
                considerar(k, st.blendEntries[k].threshold);

        // Below the first threshold or above the last one: only the extreme.
        if (lo == kNinguno) lo = hi;
        if (hi == kNinguno) hi = lo;

        // All at the SAME normalized phase as the main one: a 40-tick walk
        // and a 100-tick run blended by absolute time get out of sync
        // and the legs skate; by phase, the planted foot of one lands on that
        // of the other.
        auto clipDe   = [&](int k) { return k < 0 ? st.clipIndex : st.blendEntries[k].clipIndex; };
        auto tiempoDe = [&](int k) { return k < 0 ? animTime : phase * st.blendEntries[k].duration; };
        out.clipA  = clipDe(lo);
        out.timeA  = tiempoDe(lo);
        out.clipB  = clipDe(hi);
        out.timeB  = tiempoDe(hi);
        out.weight = (lo == hi) ? 1.0f : (p - loT) / (hiT - loT);
        auto durDe = [&](int k) { return k < 0 ? st.duration : st.blendEntries[k].duration; };
        out.durA   = durDe(lo);
        out.durB   = durDe(hi);
        return out;
    }

    AnimationPose AnimatorComponent::pose() const
    {
        AnimationPose out;
        out.rootMotionMode = poseRootMotionMode();
        out.layerCount     = (int)m_layers.size();
        for (int li = 0; li < (int)m_layers.size(); li++)
        {
            const Layer& L = m_layers[li];
            PoseLayer&   pl = out.layers[li];
            pl.weight    = layerWeight(li);
            pl.mode      = li == 0 ? 0u : (uint32_t)L.mode;
            pl.freezeNow = L.freezePending;
            pl.mask      = (li == 0 || L.maskResolved.empty()) ? nullptr : &L.maskResolved;
            // A turned-off layer contributes no samples, but it still counts: the
            // layer index of the others does not change.
            if (li > 0 && pl.weight <= 0.0f) continue;

            int enCapa = 0;
            auto add = [&](int clip, float time, float w) {
                if (w <= 0.0f || enCapa >= kMaxPoseSamplesPerLayer) return;
                out.samples[out.count++] = { clip < 0 ? 0 : clip, time, w, li };
                enCapa++;
            };
            // The blend samples of a state (1D or 2D), with ITS clock,
            // scaled by the weight that state has in the fade.
            auto addMuestras = [&](int stateIdx, float time, float scale) {
                BlendSample bs[3];
                const int n = stateBlendSamples(stateIdx, time, bs, li);
                for (int i = 0; i < n; i++) add(bs[i].clip, bs[i].time, scale * bs[i].weight);
            };
            if (L.currentState < 0 || L.currentState >= (int)L.states.size())
            {
                // Without a state: the base falls to clip 0 as always; an upper
                // layer without a graph says nothing.
                if (li == 0) add(0, L.animTime, 1.0f);
                continue;
            }
            const float w = blendWeight(li);
            if (L.frozenFade)
            {
                pl.frozenWeight = 1.0f - w;
                addMuestras(L.currentState, L.animTime, w);
            }
            else if (blending(li) && L.prevState < (int)L.states.size())
            {
                // The outgoing one contributes its whole pair, not just its main one (A3).
                addMuestras(L.prevState, L.prevAnimTime, 1.0f - w);
                addMuestras(L.currentState, L.animTime, w);
            }
            else
                addMuestras(L.currentState, L.animTime, 1.0f);
        }
        return out;
    }

    int AnimatorComponent::stateIndexByEditorId(int editorId, int layer) const
    {
        if (editorId < 0 || layer < 0 || layer >= (int)m_layers.size()) return -1;
        const auto& states = m_layers[(size_t)layer].states;
        for (size_t i = 0; i < states.size(); i++)
            if (states[i].editorId == editorId) return (int)i;
        return -1;
    }

    int AnimatorComponent::addPropertyClip(PropertyClip c)
    {
        if ((int)m_propertyClips.size() >= kMaxPropertyClips) return -1;
        m_propertyClips.push_back(std::move(c));
        return (int)m_propertyClips.size() - 1;
    }

    void AnimatorComponent::removePropertyClip(int i)
    {
        if (i < 0 || i >= (int)m_propertyClips.size()) return;
        m_propertyClips.erase(m_propertyClips.begin() + i);
    }

    void AnimatorComponent::bindProperties(const GameObject* go, std::vector<std::string>* warnings)
    {
        for (auto& L : m_layers)
            for (auto& st : L.states)
            {
                st.propertyClipIndex = -1;
                if (st.propertyClipName.empty()) continue;
                for (int i = 0; i < (int)m_propertyClips.size(); i++)
                    if (m_propertyClips[(size_t)i].name == st.propertyClipName) { st.propertyClipIndex = i; break; }
                if (st.propertyClipIndex < 0)
                {
                    if (warnings)
                        warnings->push_back("Animator: state '" + st.name + "' references the "
                                            "property clip '" + st.propertyClipName + "', which does not exist");
                    continue;
                }
                // Without a resolved mesh clip, the state clock comes from the property
                // clip: the Animator counts in TICKS, so it is given
                // a fixed rate and the duration in ticks that corresponds. With a
                // mesh clip that one rules, and the property one is sampled by
                // the state phase.
                if (st.clipIndex < 0)
                {
                    st.ticksPerSecond = 30.0f;
                    st.duration       = m_propertyClips[(size_t)st.propertyClipIndex].duration * st.ticksPerSecond;
                }
            }

        for (auto& clip : m_propertyClips)
            for (auto& tr : clip.tracks)
            {
                if (tr.target == TrackTarget::Parameter)
                {
                    // A curve does not depend on the object: it depends on the
                    // parameter existing and being Float.
                    tr.resolved = !tr.parameterName.empty() && hasFloatParameter(tr.parameterName);
                    if (!tr.resolved && warnings)
                        warnings->push_back("Animator: the curve of clip '" + clip.name +
                                            "' writes '" + tr.parameterName +
                                            "', which is not a declared Float parameter");
                    continue;
                }
                tr.resolved = !go || propertyAvailable(*go, tr.property);
                if (!tr.resolved && warnings)
                    warnings->push_back("Animator: track '" + std::string(propertyName(tr.property)) +
                                        "' of clip '" + clip.name + "' needs a component the object does not have");
            }
    }

    int AnimatorComponent::layerPropertySamples(int li, PropertySampleRef* out, int max) const
    {
        int n = 0;
        auto add = [&](int stateIdx, float animTime, float peso) {
            if (n >= max || peso <= 0.0f) return;
            const Layer& L = m_layers[(size_t)li];
            if (stateIdx < 0 || stateIdx >= (int)L.states.size()) return;
            const State& st = L.states[(size_t)stateIdx];
            if (st.propertyClipIndex < 0) return;
            out[n++] = { st.propertyClipIndex,
                         st.ticksPerSecond > 0.0f ? animTime / st.ticksPerSecond : 0.0f,
                         peso };
        };
        const Layer& L = m_layers[(size_t)li];
        const float  w = blendWeight(li);
        if (blending(li))
        {
            add(L.prevState, L.prevAnimTime, 1.0f - w);
            add(L.currentState, L.animTime, w);
        }
        else
            add(L.currentState, L.animTime, 1.0f);
        return n;
    }

    int AnimatorComponent::propertySamples(PropertySampleRef* out, int max) const
    {
        int n = 0;
        // Same split as pose(): each layer contributes its current state and, in a
        // fade, also the one being turned off, with the weight of both multiplied
        // by that of the layer.
        for (int li = 0; li < (int)m_layers.size(); li++)
        {
            const float capa = layerWeight(li);
            if (capa <= 0.0f) continue;
            PropertySampleRef propias[kMaxPoseSamplesPerLayer];
            const int m = layerPropertySamples(li, propias, kMaxPoseSamplesPerLayer);
            for (int k = 0; k < m && n < max; k++)
            {
                // Here the layer weight does enter: this is what gets APPLIED to the
                // object. The curves (applyCurves) do not use it.
                propias[k].weight *= capa;
                out[n++] = propias[k];
            }
        }
        return n;
    }

    void AnimatorComponent::applyCurves(int li)
    {
        PropertySampleRef muestras[kMaxPoseSamplesPerLayer];
        const int n = layerPropertySamples(li, muestras, kMaxPoseSamplesPerLayer);
        if (n == 0) return;

        // The parameters these samples write, without repeats: in a fade the
        // two clips may touch the same one, and they have to be blended, not
        // written twice.
        const std::string* nombres[kMaxPoseSamplesPerLayer * 8];
        int numNombres = 0;
        for (int k = 0; k < n; k++)
            for (const auto& tr : m_propertyClips[(size_t)muestras[k].clip].tracks)
            {
                if (tr.target != TrackTarget::Parameter || !tr.resolved || tr.keys.empty()) continue;
                bool visto = false;
                for (int i = 0; i < numNombres; i++)
                    if (*nombres[i] == tr.parameterName) { visto = true; break; }
                if (!visto && numNombres < (int)(sizeof(nombres) / sizeof(nombres[0])))
                    nombres[numNombres++] = &tr.parameterName;
            }

        for (int i = 0; i < numNombres; i++)
        {
            const std::string& nombre = *nombres[i];
            PropertyContribution aporta[kMaxPoseSamplesPerLayer];
            int m = 0;
            for (int k = 0; k < n && m < kMaxPoseSamplesPerLayer; k++)
                for (const auto& tr : m_propertyClips[(size_t)muestras[k].clip].tracks)
                {
                    if (tr.target != TrackTarget::Parameter || !tr.resolved) continue;
                    if (tr.parameterName != nombre) continue;
                    aporta[m++] = { samplePropertyTrack(tr, muestras[k].time, getFloat(nombre)),
                                    muestras[k].weight };
                    break;   // one track per parameter and clip: the first one rules
                }
            // The LAYER weight does not enter: a pose is blended, a parameter is
            // written. If two layers have a curve for the same parameter the
            // last one wins, because update() traverses them in order.
            if (m > 0) setFloat(nombre, blendScalarValues(aporta, m));
        }
    }

    int AnimatorComponent::addIkConstraint(IkConstraint c)
    {
        if ((int)m_ik.size() >= kMaxIkConstraints) return -1;
        m_ik.push_back(std::move(c));
        return (int)m_ik.size() - 1;
    }

    void AnimatorComponent::removeIkConstraint(int i)
    {
        if (i < 0 || i >= (int)m_ik.size()) return;
        m_ik.erase(m_ik.begin() + i);
    }

    AnimatorComponent::IkConstraint* AnimatorComponent::ikPorNombre(const std::string& n)
    {
        for (auto& c : m_ik)
            if (c.name == n) return &c;
        return nullptr;
    }

    const AnimatorComponent::IkConstraint* AnimatorComponent::ikPorNombre(const std::string& n) const
    {
        for (const auto& c : m_ik)
            if (c.name == n) return &c;
        return nullptr;
    }

    void AnimatorComponent::setIkWeight(const std::string& nombre, float w)
    {
        if (IkConstraint* c = ikPorNombre(nombre))
            c->weight = std::isfinite(w) ? std::clamp(w, 0.0f, 1.0f) : 0.0f;
    }

    float AnimatorComponent::ikWeight(const std::string& nombre) const
    {
        const IkConstraint* c = ikPorNombre(nombre);
        return c ? c->weight : 0.0f;
    }

    void AnimatorComponent::setIkTarget(const std::string& nombre, uint64_t id)
    {
        if (IkConstraint* c = ikPorNombre(nombre)) c->targetId = id;
    }

    void AnimatorComponent::setIkPole(const std::string& nombre, uint64_t id)
    {
        if (IkConstraint* c = ikPorNombre(nombre)) c->poleId = id;
    }

    int AnimatorComponent::addLayer(const std::string& name)
    {
        if ((int)m_layers.size() >= kMaxLayers) return -1;
        Layer L;
        L.name = name;
        m_layers.push_back(std::move(L));
        return (int)m_layers.size() - 1;
    }

    void AnimatorComponent::removeLayer(int i)
    {
        if (i <= 0 || i >= (int)m_layers.size()) return;
        m_layers.erase(m_layers.begin() + i);
    }

    void AnimatorComponent::moveLayer(int from, int to)
    {
        const int n = (int)m_layers.size();
        if (from <= 0 || to <= 0 || from >= n || to >= n || from == to) return;
        Layer L = std::move(m_layers[from]);
        m_layers.erase(m_layers.begin() + from);
        m_layers.insert(m_layers.begin() + to, std::move(L));
    }

    void AnimatorComponent::setLayerWeight(int i, float w)
    {
        if (i <= 0 || i >= (int)m_layers.size()) return;
        m_layers[i].weight = std::isfinite(w) ? std::clamp(w, 0.0f, 1.0f) : 0.0f;
    }

    float AnimatorComponent::layerWeight(int i) const
    {
        if (i <= 0 || i >= (int)m_layers.size()) return 1.0f;
        return m_layers[i].weight;
    }

    void AnimatorComponent::setLayerMode(int i, LayerMode m)
    {
        if (i <= 0 || i >= (int)m_layers.size()) return;
        m_layers[i].mode = m;
    }

    int AnimatorComponent::poseClipA() const
    {
        const Layer& L = m_layers[0];
        return blending() ? previousClipIndex() : stateBlendPair(L.currentState, L.animTime).clipA;
    }

    float AnimatorComponent::poseTimeA() const
    {
        const Layer& L = m_layers[0];
        return blending() ? L.prevAnimTime : stateBlendPair(L.currentState, L.animTime).timeA;
    }

    int AnimatorComponent::poseClipB() const
    {
        const Layer& L = m_layers[0];
        // Cross-fade: the target contributes its PRIMARY clip (only two clips fit).
        return blending() ? currentClipIndex() : stateBlendPair(L.currentState, L.animTime).clipB;
    }

    float AnimatorComponent::poseTimeB() const
    {
        const Layer& L = m_layers[0];
        return blending() ? L.animTime : stateBlendPair(L.currentState, L.animTime).timeB;
    }

    float AnimatorComponent::poseWeight() const
    {
        const Layer& L = m_layers[0];
        return blending() ? blendWeight() : stateBlendPair(L.currentState, L.animTime).weight;
    }

    uint32_t AnimatorComponent::poseRootMotionMode() const
    {
        const Layer& L = m_layers[0];
        // During a cross-fade the TARGET state rules, which IS L.currentState
        // (the one that contributes poseClipB): there is no special case to write.
        if (L.currentState < 0 || L.currentState >= (int)L.states.size()) return 0u;
        return (uint32_t)L.states[L.currentState].rootMotion;
    }

    std::string AnimatorComponent::previousStateName(int layer) const
    {
        const Layer& L = lay(layer);
        if (L.prevState < 0 || L.prevState >= (int)L.states.size()) return "";
        return L.states[L.prevState].name;
    }

    void AnimatorComponent::resetPlayback()
    {
        for (int li = 0; li < (int)m_layers.size(); li++)
        {
            Layer& L = m_layers[li];
            enterState(L.entryState, li);
            // Cuts any in-flight cross-fade: after this the previous state
            // may not even exist (the editor has just re-edited the graph), and blending
            // against it would leave an impossible pose or an out-of-range index.
            L.prevState     = -1;
            L.prevAnimTime  = 0.0f;
            L.blendElapsed  = 0.0f;
            L.blendDuration = 0.0f;
            L.frozenFade    = false;
            L.freezePending = false;
        }
    }

    void AnimatorComponent::reset()
    {
        resetPlayback();
        for (auto& b : m_bools)    b.second = false;
        for (auto& t : m_triggers) t.second = false;
        for (auto& i : m_ints)     i.second = 0;
        for (auto& f : m_floats)   f.second = 0.0f;
    }

    void AnimatorComponent::rebindClips(const SkinnedMesh& mesh, std::vector<std::string>* warnings)
    {
        auto findClip = [&mesh](const std::string& name) {
            for (size_t i = 0; i < mesh.animationClips.size(); i++)
                if (mesh.animationClips[i].name == name) return (int)i;
            return -1;
        };

        for (auto& L : m_layers)
        {
            for (auto& st : L.states)
            {
                // The blend entries are ALWAYS resolved, even if the primary one
                // fails: the warnings are independent and seeing only one would send you
                // looking in the wrong place. An entry just added in the
                // editor does not have a clip yet: it is not an error, it does not warn.
                for (auto& e : st.blendEntries)
                {
                    const int b = e.clipName.empty() ? -1 : findClip(e.clipName);
                    e.clipIndex = b;
                    e.duration  = (b >= 0) ? mesh.animationClips[b].duration : 0.0f;
                    if (b < 0 && !e.clipName.empty() && warnings)
                        warnings->push_back("Animator: state '" + st.name + "' blends with clip '" +
                                            e.clipName + "', which does not exist in the model");
                }

                const int found = findClip(st.clipName);
                if (found < 0)
                {
                    st.clipIndex = -1;
                    if (warnings)
                        warnings->push_back("Animator: state '" + st.name + "' references clip '" +
                                            st.clipName + "', which does not exist in the model");
                    continue;
                }
                st.clipIndex      = found;
                st.duration       = mesh.animationClips[found].duration;
                st.ticksPerSecond = mesh.animationClips[found].ticksPerSecond;
                // st.loop is NOT touched: it is user authoring, not FBX data.
            }
        }
        // Masks by bone name: a name the skeleton does not have
        // warns, like a clip that does not exist, and marks nothing.
        for (auto& L : m_layers)
        {
            L.maskResolved.clear();
            if (L.maskBones.empty()) continue;
            L.maskResolved.assign(mesh.skeleton.names.size(), 0);
            for (const auto& nombre : L.maskBones)
            {
                auto it = std::find(mesh.skeleton.names.begin(), mesh.skeleton.names.end(), nombre);
                if (it == mesh.skeleton.names.end())
                {
                    if (warnings)
                        warnings->push_back("Animator: the mask of layer '" + L.name +
                                            "' names bone '" + nombre + "', which the model does not have");
                    continue;
                }
                L.maskResolved[(size_t)(it - mesh.skeleton.names.begin())] = 1;
            }
        }

        // Bones of the IK constraints, by name like the clips. In
        // TwoBone the bone is the END and the chain is its two parents: without
        // them it cannot be resolved, so the constraint is turned off and warns.
        for (auto& c : m_ik)
        {
            c.boneIndex = c.parentIndex = c.grandParentIndex = -1;
            auto it = std::find(mesh.skeleton.names.begin(), mesh.skeleton.names.end(), c.boneName);
            if (it == mesh.skeleton.names.end())
            {
                if (warnings && !c.boneName.empty())
                    warnings->push_back("Animator: IK '" + c.name + "' uses bone '" + c.boneName +
                                        "', which the model does not have");
                continue;
            }
            const int   bi     = (int)(it - mesh.skeleton.names.begin());
            const auto& padres = mesh.skeleton.parentIndex;
            const int   p      = bi < (int)padres.size() ? padres[(size_t)bi] : -1;
            const int   gp     = (p >= 0 && p < (int)padres.size()) ? padres[(size_t)p] : -1;
            if (c.type == IkType::TwoBone && (p < 0 || gp < 0))
            {
                if (warnings)
                    warnings->push_back("Animator: IK '" + c.name + "' needs a chain of three "
                                        "bones and '" + c.boneName + "' has no parent and grandparent");
                continue;
            }
            c.boneIndex = bi; c.parentIndex = p; c.grandParentIndex = gp;
        }
    }

    void AnimatorComponent::bindClips(const SkinnedMesh& mesh, std::vector<std::string>* warnings)
    {
        rebindClips(mesh, warnings);
        reset();
    }

    int AnimatorComponent::renameClipReferences(const std::string& oldName,
                                                 const std::string& newName)
    {
        int changed = 0;
        for (auto& L : m_layers)
        for (auto& st : L.states)
        {
            // A state counts ONCE even if the rename touches both its clips:
            // what is returned is affected states, not references.
            bool touched = false;
            if (st.clipName == oldName)      { st.clipName      = newName; touched = true; }
            for (auto& e : st.blendEntries)
                if (e.clipName == oldName) { e.clipName = newName; touched = true; }
            if (touched) changed++;
        }
        return changed;
    }

    bool AnimatorComponent::conditionsMet(const Transition& t, int layer) const
    {
        const Layer& L = lay(layer);
        // A transition without conditions would fire on the frame it is created and
        // make the graph unusable. Unity covers that case with exit time, which is
        // out of scope.
        if (t.conditions.empty()) return false;

        for (const auto& c : t.conditions)
        {
            switch (c.type)
            {
                case ConditionType::Bool:
                    if (getBool(c.paramName) != c.expected) return false;
                    break;
                case ConditionType::Trigger:
                    if (!isTriggerSet(c.paramName)) return false;
                    break;
                case ConditionType::AnimationFinished:
                    if (!L.finished) return false;
                    break;
                case ConditionType::Int:
                    // The threshold lives in float (see comment in AnimatorPanel), so we
                    // round instead of truncating: a hand-edited JSON with
                    // threshold: 2.9 must evaluate as 3, not silently as 2.
                    if (!evalCompare(getInt(c.paramName), c.compare, (int)std::lround(c.threshold))) return false;
                    break;
                case ConditionType::Float:
                    if (!evalCompare(getFloat(c.paramName), c.compare, c.threshold)) return false;
                    break;
            }
        }
        return true;
    }

    bool AnimatorComponent::exitTimeCrossed(double n0, double n1, float exitTime)
    {
        const double e = exitTime;
        // From 1 on it counts accumulated loops: ready on any frame
        // that has already completed them.
        if (e >= 1.0) return n1 >= e;
        // First update after entering with exitTime 0: the start of loop
        // zero is a crossing, or it would not fire until the second.
        if (e == 0.0 && n0 == 0.0) return true;
        // Below 1, on every loop: is there an integer k with
        // n0 < k + e <= n1? It also covers the dt that wraps around crossing e.
        return std::floor(n1 - e) > std::floor(n0 - e);
    }

    bool AnimatorComponent::transitionReady(const Transition& t, double n0, double n1,
                                            bool hasDuration, int layer) const
    {
        if (t.hasExitTime)
        {
            if (hasDuration && !exitTimeCrossed(n0, n1, t.exitTime)) return false;
            // By time only: no condition is needed.
            if (t.conditions.empty()) return true;
        }
        // Without exit time and without conditions, conditionsMet returns false: such a
        // transition never fires, as always.
        return conditionsMet(t, layer);
    }

    void AnimatorComponent::consumeTriggers(const Transition& t)
    {
        // Only those of the winning transition: a trigger that nobody consumes stays
        // armed waiting (same behavior as Unity).
        for (const auto& c : t.conditions)
            if (c.type == ConditionType::Trigger)
                m_triggers[c.paramName] = false;
    }

    float AnimatorComponent::stateRate(const State& st) const
    {
        float mult = st.speed;
        if (!st.speedParam.empty() && hasParam(st.speedParam, ParamType::Float))
            mult *= getFloat(st.speedParam);
        // Negative or NaN freezes: !(x > 0) is true for NaN too.
        if (!(mult > 0.0f)) mult = 0.0f;
        return st.ticksPerSecond * mult;
    }

    void AnimatorComponent::setSpeed(float s)
    {
        m_speed = (s > 0.0f) ? s : 0.0f;   // negative or NaN -> 0
    }

    void AnimatorComponent::advanceClock(const State& st, float rate, float& time, bool* finished, float dt)
    {
        if (st.duration > 0.0f && st.ticksPerSecond > 0.0f)
        {
            time += dt * rate;
            if (time >= st.duration)
            {
                if (st.loop)
                {
                    time = std::fmod(time, st.duration);
                }
                else
                {
                    // Pinned on the last frame, and it stays that way in the following
                    // updates.
                    time = st.duration;
                    if (finished) *finished = true;
                }
            }
        }
        else if (finished)
        {
            // An unresolved clip (clipIndex == -1, duration at 0) or one of
            // real zero duration would never enter the block above and
            // would never set L.finished to true: an "animation finished" exit
            // would be left waiting forever. Semantically a state of
            // duration 0 has already finished the instant it is entered, so
            // finished is reasserted every frame (just as the clamp above
            // reasserts it on the last frame of a normal non-loop clip).
            *finished = true;
        }
    }

    void AnimatorComponent::collectEvents(const State& st, double ticks0, double ticks1)
    {
        if (st.events.empty() || st.duration <= 0.0f || ticks1 <= ticks0) return;
        const double dur = st.duration;
        for (const auto& ev : st.events)
        {
            // Instants k·dur + c, k >= 0, within [ticks0, ticks1). The accumulated
            // clock has no wrap, so crossing the loop or skipping whole cycles
            // with a large dt comes out of the same computation.
            const double c    = (double)ev.time * dur;
            double       kMin = std::ceil((ticks0 - c) / dur);
            if (kMin < 0.0) kMin = 0.0;
            double       kMax = std::ceil((ticks1 - c) / dur) - 1.0;
            if (!st.loop) kMax = std::min(kMax, 0.0);
            if (kMax < kMin) continue;
            // Cap: a hitch of several seconds must not fill the list.
            const double veces = std::min(kMax - kMin + 1.0, (double)kMaxEventCyclesPerUpdate);
            for (int i = 0; i < (int)veces; i++)
                m_firedEvents.push_back(ev.name);
        }
    }

    void AnimatorComponent::collectRootMotion(double ticks0, double prevTicks0)
    {
        Layer& L = m_layers[0];
        const State& st = L.states[L.currentState];
        // In a fade the pose uses the PRIMARY clip of each side (see poseClipB):
        // the movement comes from the same thing that is seen.
        if (blending())
        {
            const float w = blendWeight();
            m_rootMotionSamples.push_back({ st.clipIndex, ticks0, L.stateTicks, st.duration, st.loop, w });
            if (L.prevState >= 0 && L.prevState < (int)L.states.size())
            {
                const State& prev = L.states[L.prevState];
                if (prev.rootMotion == RootMotion::Apply && prev.duration > 0.0f)
                    m_rootMotionSamples.push_back({ prev.clipIndex, prevTicks0, L.prevStateTicks,
                                                    prev.duration, prev.loop, 1.0f - w });
            }
            return;
        }
        BlendSample bs[3];
        const int n = stateBlendSamples(L.currentState, L.animTime, bs);
        // Each clip goes at the main one's phase: its accumulated ticks are those
        // of the main one scaled to its duration.
        for (int i = 0; i < n; i++)
        {
            const double esc = st.duration > 0.0f ? (double)bs[i].duration / st.duration : 0.0;
            m_rootMotionSamples.push_back({ bs[i].clip, ticks0 * esc, L.stateTicks * esc, bs[i].duration,
                                            st.loop, n == 1 ? 1.0f : bs[i].weight });
        }
    }

    void AnimatorComponent::update(float dt, bool evaluateTransitions)
    {
        // Only what is from THIS update: it is emptied before any return.
        m_firedEvents.clear();
        m_rootMotionSamples.clear();
        // Global speed: it scales the whole dt, cross-fade included, in
        // all the layers.
        dt *= m_speed;
        std::vector<const Transition*> consumir;
        for (int li = 0; li < (int)m_layers.size(); li++)
            updateLayer(li, dt, evaluateTransitions, consumir);
        for (const Transition* t : consumir) consumeTriggers(*t);
    }

    void AnimatorComponent::updateLayer(int li, float dt, bool evaluateTransitions,
                                        std::vector<const Transition*>& consumir)
    {
        Layer& L = m_layers[li];
        if (L.currentState < 0 || L.currentState >= (int)L.states.size())
        {
            L.currentState = L.entryState;
            if (L.currentState < 0 || L.currentState >= (int)L.states.size()) return;
        }

        const State& actual      = L.states[L.currentState];
        const bool   conDuracion = actual.duration > 0.0f && actual.ticksPerSecond > 0.0f;
        const float  ritmo       = stateRate(actual);
        const double ticks0      = L.stateTicks;
        const double prevTicks0  = L.prevStateTicks;
        if (conDuracion)
            L.stateTicks += (double)dt * ritmo;
        advanceClock(actual, ritmo, L.animTime, &L.finished, dt);

        // Cross-fade in progress: the state being turned off keeps animating with ITS
        // rate and ITS loop while the blend lasts. Freezing it would give a
        // visible jump right at the start of the transition, which is the opposite of what
        // the cross-fade comes to solve.
        if (L.prevState >= 0 || L.frozenFade)
        {
            if (L.prevState >= 0 && L.prevState < (int)L.states.size())
            {
                advanceClock(L.states[L.prevState], stateRate(L.states[L.prevState]),
                             L.prevAnimTime, nullptr, dt);
                L.prevStateTicks += (double)dt * stateRate(L.states[L.prevState]);
            }

            L.blendElapsed += dt;
            if (L.blendDuration <= 0.0f || L.blendElapsed >= L.blendDuration)
            {
                // Blend finished: the target stays alone. From here on
                // blendWeight() is 1 again through the usual path.
                L.prevState     = -1;
                L.prevAnimTime  = 0.0f;
                L.blendElapsed  = 0.0f;
                L.blendDuration = 0.0f;
                L.frozenFade    = false;
                L.freezePending = false;
            }
        }

        // Curves BEFORE the transitions and before the Edit return: this frame's
        // value conditions this frame's transitions, and in the
        // editor preview the parameter is seen moving in the panel.
        applyCurves(li);

        if (!evaluateTransitions) return;

        // Events BEFORE the transitions: if this update leaves the state,
        // its final stretch has already fired. Only the current state; the one being
        // turned off in a fade does not, or the footsteps would come out doubled.
        // A turned-off layer does not fire: it is not seen.
        if (conDuracion && (li == 0 || L.weight > 0.0f))
            collectEvents(actual, ticks0, L.stateTicks);

        // Root motion with the same stretch: also before the transitions.
        // Only the base moves the GameObject.
        if (li == 0 && conDuracion && actual.rootMotion == RootMotion::Apply)
            collectRootMotion(ticks0, prevTicks0);

        const double n0 = conDuracion ? ticks0 / actual.duration : 0.0;
        const double n1 = conDuracion ? L.stateTicks / actual.duration : 0.0;

        // First Any State and then those of the current state, each group in
        // declaration order: the first one that is ready wins. It is Unity's
        // priority, and what someone coming from there expects.
        const Transition* elegida = nullptr;
        for (const auto& t : L.transitions)
        {
            if (t.fromState != kAnyState) continue;
            if (t.toState < 0 || t.toState >= (int)L.states.size()) continue;
            // Toward the current state only with the flag: with a bool, re-entering
            // would restart the state every frame.
            if (t.toState == L.currentState && !t.canTransitionToSelf) continue;
            // Same rule as below: a broken box does not fire.
            if (resolveEntryLeaf(t.toState, li) < 0) continue;
            if (transitionReady(t, n0, n1, conDuracion, li)) { elegida = &t; break; }
        }
        if (!elegida)
        {
            // By LEVELS: first those leaving the leaf, then those of its
            // box, then those of the box above. This way a general exit of a
            // block does not beat a specific exit of a state just because
            // it was declared earlier. Any State was already checked above and still
            // has priority over all of this.
            int nivel = L.currentState;
            for (int pasos = 0; nivel >= 0 && nivel < (int)L.states.size() && !elegida &&
                                pasos <= (int)L.states.size(); pasos++)
            {
                for (const auto& t : L.transitions)
                {
                    if (t.fromState != nivel) continue;
                    if (t.toState < 0 || t.toState >= (int)L.states.size()) continue;
                    // Entering a broken box does not fire: better to stay where
                    // one is than halfway into a state that does not exist.
                    if (resolveEntryLeaf(t.toState, li) < 0) continue;
                    if (transitionReady(t, n0, n1, conDuracion, li)) { elegida = &t; break; }
                }
                nivel = L.states[(size_t)nivel].parent;
            }
        }
        if (!elegida) return;

        {
            consumir.push_back(elegida);
            startTransitionTo(elegida->toState, elegida->duration, li);   // one per update
        }
    }

    void AnimatorComponent::startTransitionTo(int idx, float duration, int layer)
    {
        Layer& L = lay(layer);
        if (duration > 0.0f)
        {
            if (fading(layer))
            {
                // Interruption: the in-flight blend is FROZEN instead of
                // being discarded (A4). The backend copies the on-screen pose to the
                // frozen one before evaluating (freezeNow) and the fade starts from it.
                L.frozenFade     = true;
                L.freezePending  = true;
                L.prevState      = -1;
                L.prevStateTicks = 0.0;
                L.prevAnimTime   = 0.0f;
            }
            else
            {
                // The state we leave becomes the one being turned off, with the
                // time it had.
                L.prevState      = L.currentState;
                L.prevStateTicks = L.stateTicks;
                L.prevAnimTime   = L.animTime;
            }
            L.blendElapsed  = 0.0f;
            L.blendDuration = duration;
        }
        else
        {
            // Hard cut: neither previous state nor blend, the usual path.
            L.prevStateTicks = 0.0;
            L.prevState     = -1;
            L.prevAnimTime  = 0.0f;
            L.blendElapsed  = 0.0f;
            L.blendDuration = 0.0f;
            L.frozenFade    = false;
            L.freezePending = false;
        }
        enterState(idx, layer);
    }

    void AnimatorComponent::sanitizeGraph(int layer, std::vector<std::string>* warnings)
    {
        Layer& L = lay(layer);
        const int n = (int)L.states.size();
        auto avisa = [&](const std::string& msg) { if (warnings) warnings->push_back(msg); };

        // Transitions with impossible indices: they go away. Clamping them would be worse,
        // because it would leave a link pointing to a state the user did not choose.
        // kAnyState is a sentinel, not an out-of-range index.
        const size_t antes = L.transitions.size();
        L.transitions.erase(
            std::remove_if(L.transitions.begin(), L.transitions.end(),
                [&](const Transition& t) {
                    const bool origenOk = t.fromState == kAnyState || (t.fromState >= 0 && t.fromState < n);
                    const bool destinoOk = t.toState >= 0 && t.toState < n;
                    if (origenOk && destinoOk) return false;
                    avisa("animator: transition " + std::to_string(t.fromState) + " -> " +
                          std::to_string(t.toState) + " with indices out of range, discarded");
                    return true;
                }),
            L.transitions.end());
        (void)antes;

        for (auto& st : L.states)
        {
            if (st.parent < -1 || st.parent >= n)
            {
                avisa("animator.state." + st.name + ": parent out of range, left at the root");
                st.parent = -1;
            }
            // Its own range check, not an `else` of the one above: a
            // guard that leans on another stops protecting as soon as someone
            // touches the first, and here that means indexing outside the vector.
            if (st.parent >= 0 && st.parent < n && !L.states[(size_t)st.parent].isSubMachine)
            {
                avisa("animator.state." + st.name + ": the parent is not a sub-state machine, left at the root");
                st.parent = -1;
            }
            if (st.subEntry < -1 || st.subEntry >= n)
            {
                avisa("animator.state." + st.name + ": entry out of range, the sub-state machine is left empty");
                st.subEntry = -1;
            }
        }

        // The entry of a box has to be ITS child: pointing elsewhere
        // would make entering the box exit from it.
        for (int i = 0; i < n; i++)
        {
            State& st = L.states[(size_t)i];
            if (st.subEntry < 0) continue;
            if (!st.isSubMachine)
            {
                st.subEntry = -1;   // it is not a box: the field means nothing
                continue;
            }
            if (L.states[(size_t)st.subEntry].parent != i)
            {
                avisa("animator.state." + st.name + ": the entry is not one of its children, the sub-state machine is left empty");
                st.subEntry = -1;
            }
        }

        // Containment cycles: climb from each state with a cap. If it is exceeded,
        // that state goes to the root; without this, isDescendantOf and resolveEntryLeaf
        // would have to trust their own cap on every frame.
        for (int i = 0; i < n; i++)
        {
            int p = L.states[(size_t)i].parent, pasos = 0;
            while (p >= 0 && p < n && pasos <= n) { p = L.states[(size_t)p].parent; pasos++; }
            if (pasos > n)
            {
                avisa("animator.state." + L.states[(size_t)i].name +
                      ": sub-state machine cycle, left at the root");
                L.states[(size_t)i].parent = -1;
            }
        }

        // Entry of the layer and playhead: an impossible index here leaves the layer
        // unstarted.
        if (L.entryState < -1 || L.entryState >= n)
        {
            avisa("animator: layer entry out of range");
            L.entryState = n > 0 ? 0 : -1;
        }
        if (L.currentState < -1 || L.currentState >= n) L.currentState = -1;
        if (L.prevState    < -1 || L.prevState    >= n) L.prevState    = -1;
    }

    bool AnimatorComponent::isDescendantOf(int state, int maybeAncestor, int layer) const
    {
        const Layer& L = lay(layer);
        if (maybeAncestor < 0 || state < 0 || state >= (int)L.states.size()) return false;
        int actual = L.states[(size_t)state].parent;
        // The cap is the number of states: a file with a parent cycle cannot
        // hang the engine.
        for (int pasos = 0; actual >= 0 && actual < (int)L.states.size() && pasos <= (int)L.states.size(); pasos++)
        {
            if (actual == maybeAncestor) return true;
            actual = L.states[(size_t)actual].parent;
        }
        return false;
    }

    int AnimatorComponent::resolveEntryLeaf(int state, int layer) const
    {
        const Layer& L = lay(layer);
        int actual = state;
        for (int pasos = 0; pasos <= (int)L.states.size(); pasos++)
        {
            if (actual < 0 || actual >= (int)L.states.size()) return -1;
            const State& st = L.states[(size_t)actual];
            if (!st.isSubMachine) return actual;
            actual = st.subEntry;
        }
        return -1;   // subEntry cycle
    }

    int AnimatorComponent::stateIndexByName(const std::string& name, int layer) const
    {
        const Layer& L = lay(layer);
        for (int i = 0; i < (int)L.states.size(); i++)
            if (L.states[i].name == name) return i;
        return -1;
    }

    bool AnimatorComponent::play(const std::string& stateName, int layer)
    {
        const int idx = stateIndexByName(stateName, layer);
        // A box is entered through its leaf; if the chain is broken there is nowhere to go,
        // and that is a false, like a name that does not exist.
        if (idx < 0 || resolveEntryLeaf(idx, layer) < 0) return false;
        startTransitionTo(idx, 0.0f, layer);
        return true;
    }

    bool AnimatorComponent::crossFade(const std::string& stateName, float seconds, int layer)
    {
        Layer& L = lay(layer);
        const int idx = stateIndexByName(stateName, layer);
        // Same guard as play: a broken box has no leaf to enter.
        if (idx < 0 || resolveEntryLeaf(idx, layer) < 0) return false;
        // Without a current state there is nothing to turn off: it is entered with a cut.
        const bool hayActual = L.currentState >= 0 && L.currentState < (int)L.states.size();
        startTransitionTo(idx, hayActual ? seconds : 0.0f, layer);
        return true;
    }

    float AnimatorComponent::normalizedTime(int layer) const
    {
        const Layer& L = lay(layer);
        if (L.currentState < 0 || L.currentState >= (int)L.states.size()) return 0.0f;
        const State& st = L.states[L.currentState];
        if (st.duration <= 0.0f) return 0.0f;
        return (float)(L.stateTicks / st.duration);
    }

    const char* paramTypeLabel(AnimatorComponent::ParamType t)
    {
        switch (t)
        {
            case AnimatorComponent::ParamType::Trigger: return "trigger";
            case AnimatorComponent::ParamType::Int:     return "int";
            case AnimatorComponent::ParamType::Float:   return "float";
            default:                                    return "bool";
        }
    }
}
