#pragma once
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include "DonTopo/Core/AnimationPose.h"
#include "DonTopo/Core/PropertyTracks.h"

namespace DonTopo
{
    struct SkinnedMesh;

    // Animation state machine (equivalent to Unity's Animator). Each
    // state contains a clip; the links are directed transitions.
    //
    // Pure data + logic: no Vulkan and no knowledge of GameObject, same rule as
    // CameraComponent and Rigidbody (the dependency goes Core -> the rest, never the
    // other way). That is what allows testing it entirely without a GPU or a window.
    //
    // It is the ONLY owner of animTime: the Renderer only receives (clip, time) already
    // computed via Renderer::setAnimationState. Splitting the time between the two
    // would give two sources of truth.
    //
    // Cross-fade: a transition with duration > 0 keeps the state that is
    // being turned off alive during those seconds, so there are TWO clocks and two clips
    // in flight (currentClipIndex/animTime and previousClipIndex/previousAnimTime) plus
    // the blend weight. With duration == 0 there is no previous state and the
    // behavior is the usual instant cut.
    class AnimatorComponent
    {
        public:
            // New values go AT THE END: the tests use aggregate initialization of Condition
            // and serialization goes by string, so adding in the middle would break the
            // former without gaining anything.
            enum class ConditionType { Bool, Trigger, AnimationFinished, Int, Float };
            enum class ParamType     { Bool, Trigger, Int, Float };
            // Comparators of the numeric conditions: all four work for both
            // Int and Float. On a float, Equals requires exact binary equality
            // — a computed value almost never meets it, one assigned with setFloat does.
            enum class Compare       { Greater, Less, Equals, NotEquals };

            // fromState of a transition that leaves "Any State": valid from
            // any state. Negative on purpose: removeState only reindexes
            // indices >= 0, so the sentinel survives intact.
            static constexpr int kAnyState = -2;

            struct Condition
            {
                ConditionType type     = ConditionType::Bool;
                std::string   paramName;          // empty if AnimationFinished
                bool          expected = true;    // solo Bool
                // Int/Float only. A single float threshold serves both: the Int
                // UI uses DragInt, so an integral value always comes in,
                // and float represents exact integers up to 2^24.
                Compare       compare   = Compare::Greater;
                float         threshold = 0.0f;
            };

            struct Transition
            {
                int fromState = -1;
                int toState   = -1;
                // AND of all: the transition fires when all are met.
                std::vector<Condition> conditions;
                // Cross-fade, in real SECONDS (not ticks: the two states being
                // blended may have different ticksPerSecond, so a blend
                // time in ticks would mean nothing).
                //
                // 0 = instant cut, which is what the engine did before
                // this field existed and what every scene saved without it
                // brings. It goes AT THE END of the struct: the tests build
                // Transition by members and the conditions are serialized by
                // name.
                float duration = 0.0f;
                // --- Exit time (as in Unity) ---
                // With hasExitTime the transition waits for the source
                // state to reach exitTime, in NORMALIZED time (1 = end of the
                // clip). Below 1 it is checked on every loop; from 1
                // on it counts accumulated loops (2.5 = two and a half loops).
                // Without conditions the time is enough; with conditions both
                // things are needed. At the end of the struct and off by default: it is
                // what scenes saved without these fields bring.
                bool  hasExitTime = false;
                float exitTime    = 1.0f;
                // Only read on transitions that leave Any State: whether it can
                // return to the state it is already in. Off by default:
                // on, and with a bool, it would restart the state every frame.
                bool  canTransitionToSelf = false;
            };

            // An extra clip of a 1D blend. The name is the authoring; index and
            // duration are resolved by rebindClips from the mesh and are not saved.
            struct BlendEntry
            {
                std::string clipName;
                int         clipIndex = -1;
                float       duration  = 0.0f;   // ticks
                float       threshold = 0.0f;
                float       thresholdY = 0.0f;   // solo en blend 2D
            };

            // Named event at an instant of the state's cycle. time is a
            // normalized phase [0, 1] over the duration of the main clip.
            struct AnimationEvent
            {
                std::string name;
                float       time = 0.0f;
            };

            // What the translation of the clip root does. Off: the pose
            // moves it (as always). Lock: it is pinned to its bind and the clip is seen
            // in place. Apply: the GPU pins only X and Z (the vertical bobbing
            // is seen) and the horizontal displacement moves the GameObject.
            enum class RootMotion { Off, Lock, Apply };

            struct State
            {
                std::string name;
                // The clip is referenced by NAME, not by index: the index
                // depends on the order of mAnimations in the FBX, and re-exporting the
                // model shuffles it. bindClips resolves name -> clipIndex.
                std::string clipName;
                int         clipIndex      = -1;
                // Cached by bindClips so that the component is self-contained
                // (and testable without FBX or Vulkan).
                float       duration       = 0.0f;    // ticks
                float       ticksPerSecond = 24.0f;
                // User authoring (node checkbox), NOT cached from the clip:
                // the SkinnedMesh is rebuilt from the FBX on every load and is not
                // serialized, so a loop saved there would be lost.
                bool        loop           = true;
                // --- Sub-machines (C10) ---
                // The layer vector stays flat: this is containment, not a nested
                // graph. A box is NOT played and is never the current
                // state; it serves to group on the canvas and to write a
                // transition against the whole block.
                int         parent         = -1;      // who contains it; -1 = root of the layer
                bool        isSubMachine   = false;   // it is a box
                int         subEntry       = -1;      // child through which it is entered (only if isSubMachine)
                // --- 1D blend by parameter ---
                // The main clip (clipName) is one more entry, with
                // clipThreshold; blendEntries are the extra ones. The two
                // neighbors of the blendParam value play (see stateBlendPair). Without
                // entries, or with an undeclared parameter, a single clip.
                std::string             blendParam;
                float                   clipThreshold = 0.0f;
                std::vector<BlendEntry> blendEntries;
                // --- 2D blend ---
                // With blendParamY declared Float, each clip is a point
                // (threshold, thresholdY) and the 3 of the triangle that contains
                // (blendParam, blendParamY) play. See stateBlendSamples.
                std::string             blendParamY;
                float                   clipThresholdY = 0.0f;
                // Property clip of the state, by NAME (like the mesh one);
                // the index is resolved by bindProperties.
                std::string             propertyClipName;
                int                     propertyClipIndex = -1;
                // Events of the state: they fire in Play (see collectEvents) and
                // reach Lua as OnAnimationEvent(name).
                std::vector<AnimationEvent> events;
                // Position of the node in the AnimatorPanel canvas.
                glm::vec2   editorPos{0.0f};
                // Stable id for the editor canvas node (AnimatorPanel), NOT
                // the index in m_states: that index changes when removeState
                // reindexes the vector, and if the canvas id were the index, a
                // survivor would inherit the visual slot (position/selection) of the
                // deleted node in imgui-node-editor, which caches them by id. It is NOT
                // serialized (see Scene.cpp): it is regenerated in addState on load.
                int         editorId = -1;
                // Translation of the root (the bone with parentIndex < 0), see
                // RootMotion. The rotation and scale of the root are NOT touched in
                // any mode. Off is what every scene saved without the field
                // brings.
                RootMotion  rootMotion = RootMotion::Off;
                // --- Speed (like Unity's Speed + Multiplier) ---
                // Rate = ticksPerSecond x speed x value of speedParam (if it is a
                // declared float; otherwise, x1). Negative or NaN freezes (0): going
                // backwards would require redefining loop, finished and exit time. At the
                // end of the struct and at x1 by default, which is what scenes saved
                // without these fields bring.
                float       speed          = 1.0f;
                std::string speedParam;
            };

            struct Parameter
            {
                std::string name;
                ParamType   type = ParamType::Bool;
            };

            // --- Property clips ---
            // A state can play, besides its mesh clip, an authored clip
            // that writes properties of the GameObject (transform, light,
            // material). It is what allows animating an object WITHOUT a skeleton.
            static constexpr int kMaxPropertyClips = 16;
            const std::vector<PropertyClip>& propertyClips() const { return m_propertyClips; }
            std::vector<PropertyClip>&       propertyClipsMutable() { return m_propertyClips; }
            int  addPropertyClip(PropertyClip c);   // index, -1 if there are already kMaxPropertyClips
            void removePropertyClip(int i);
            // Resolves the propertyClipName of each state and the `resolved` of
            // each track against the object (which components it has). With a null go it
            // only does the first: the component does NOT store the GameObject.
            void bindProperties(const GameObject* go, std::vector<std::string>* warnings);
            // What plays this frame: clip, time IN SECONDS and weight (that of the
            // cross-fade times that of its layer). Returns how many.
            struct PropertySampleRef { int clip = -1; float time = 0.0f; float weight = 0.0f; };
            int  propertySamples(PropertySampleRef* out, int max) const;

            // --- IK ---
            // Constraints that correct the ALREADY evaluated pose, on the GPU, between
            // the hierarchy and the skinning (bone_ik.comp). They belong to the component, not
            // to a layer: they are applied on the final pose.
            enum class IkType { LookAt, TwoBone };
            static constexpr int kMaxIkConstraints = 4;
            struct IkConstraint
            {
                std::string name;                              // for Lua and the panel
                IkType      type     = IkType::LookAt;
                // LookAt: the bone that looks. TwoBone: the END of the chain
                // (hand, foot); the other two are its parent and its grandparent.
                std::string boneName;
                uint64_t    targetId = 0;                      // GameObject; 0 = no target
                uint64_t    poleId   = 0;                      // TwoBone: where the elbow goes
                float       weight   = 1.0f;                   // 0..1
                glm::vec3   aimAxis  = { 0.0f, 0.0f, 1.0f };   // LookAt: local axis that looks
                float       maxAngle = 80.0f;                  // LookAt: degrees
                // --- Resolved in bindClips, not serialized ---
                int boneIndex = -1, parentIndex = -1, grandParentIndex = -1;
            };

            const std::vector<IkConstraint>& ikConstraints() const { return m_ik; }
            std::vector<IkConstraint>&       ikConstraintsMutable() { return m_ik; }
            // Returns the index, -1 if there are already kMaxIkConstraints.
            int   addIkConstraint(IkConstraint c);
            void  removeIkConstraint(int i);
            // By NAME, like the parameters: one that does not exist does nothing
            // in the setters and returns 0 in the getter.
            void  setIkWeight(const std::string& nombre, float w);   // clamped to [0,1]
            float ikWeight(const std::string& nombre) const;
            void  setIkTarget(const std::string& nombre, uint64_t id);
            void  setIkPole(const std::string& nombre, uint64_t id);

            // --- Layers --- Layer 0 is the base; the
            // others are applied on top, in order, with their weight and their mask:
            // override replaces the pose, additive adds its difference from
            // the first frame of each clip.
            enum class LayerMode { Override, Additive };
            static constexpr int kMaxLayers = 8;
            struct Layer
            {
                std::string              name = "Base Layer";
                std::vector<State>       states;
                std::vector<Transition>  transitions;
                int                      entryState = -1;
                float                    weight     = 1.0f;
                LayerMode                mode       = LayerMode::Override;
                std::vector<std::string> maskBones;      // empty = whole body
                // To the left of the first state the panel creates (40, 40).
                glm::vec2                anyStatePos{ -220.0f, 40.0f };

                // --- Execution (not serialized) ---
                int    currentState   = -1;
                float  animTime       = 0.0f;
                bool   finished       = false;
                // Ticks advanced since the current state was entered, WITHOUT
                // fmod: in a loop it keeps growing, which is what allows counting
                // loops for the exit time. double and not float: in a long
                // session a float loses resolution to decide a crossing.
                double stateTicks     = 0.0;
                // Accumulated clock (without wrap) of the state being turned off in a fade:
                // root motion needs it so as not to jump at its wrap.
                double prevStateTicks = 0.0;
                // Cross-fade in progress. prevState at -1 means "no blend", and
                // it is the state everything is left in with transitions of
                // duration 0.
                int    prevState      = -1;
                float  prevAnimTime   = 0.0f;
                float  blendElapsed   = 0.0f;
                float  blendDuration  = 0.0f;
                // Fade from a frozen pose (another fade was interrupted) and the
                // request to copy it, pending until the host sends it.
                bool   frozenFade     = false;
                bool   freezePending  = false;
                // maskBones resolved against the skeleton (bindClips): one per
                // bone; empty = whole body.
                std::vector<uint8_t> maskResolved;
            };

            // The AUTHORED part of the graph, with nothing of runtime: what the editor
            // undo (AnimatorGraphCommand) saves and restores. The states
            // go whole —editorId and editorPos included— because applyGraph
            // needs the editorId to match the live states with those of the
            // snapshot, and the position to place a node that comes back from a
            // deletion.
            struct Graph
            {
                std::vector<State>      states;
                std::vector<Transition> transitions;
                std::vector<Parameter>  parameters;
                int                     entryState = -1;
                // Layers 1..N whole (the design; their execution is ignored).
                std::vector<Layer>      extraLayers;
                // The IK constraints are entirely design: they have no
                // execution to preserve. The property clips, the same.
                std::vector<IkConstraint> ik;
                std::vector<PropertyClip> propertyClips;
            };

            // --- Design (editor / scene load) ---
            int  addState(State s, int layer = 0);                 // returns the index of the new state
            void addTransition(Transition t, int layer = 0);
            void removeState(int idx, int layer = 0);              // reindexes the transitions
            void removeTransition(int idx, int layer = 0);
            void setEntryState(int idx, int layer = 0);
            void addParameter(std::string name, ParamType type);
            void removeParameter(const std::string& name);
            // Position of the Any State node in the AnimatorPanel canvas. It goes
            // outside Graph on purpose: like that of the states, moving a node
            // does not enter the undo.
            glm::vec2 anyStateEditorPos(int layer = 0) const         { return lay(layer).anyStatePos; }
            void      setAnyStateEditorPos(glm::vec2 p, int layer = 0) { lay(layer).anyStatePos = p; }

            Graph graph() const;
            // Replaces states, transitions, parameters and entry with those of
            // g WITHOUT going through reset(): it runs in Play (undo in the middle of a game).
            //  - The playhead is matched by editorId, not by index: if the current
            //    state is still in g it keeps its time even if its index changes;
            //    if not, it falls to the entry with time 0. The one being turned off in a
            //    cross-fade is matched the same way, and the blend is only cut if either
            //    of the two is missing.
            //  - A parameter keeps its value if it already existed with the same
            //    name AND the same type; otherwise, it starts at its default value.
            //  - The live states (same editorId) keep their current
            //    editorPos: moving nodes does not enter the undo.
            //  - m_nextEditorId never goes down.
            // It does NOT resolve clips: the clipIndex cache is rebuilt by the caller
            // with rebindClips.
            // Precondition: g comes from graph() (unique editorIds, indices within
            // range); applyGraph does not validate it.
            void applyGraph(const Graph& g);

            const std::vector<State>&      states(int layer = 0)      const { return lay(layer).states; }
            const std::vector<Transition>& transitions(int layer = 0) const { return lay(layer).transitions; }
            const std::vector<Parameter>&  parameters()  const { return m_parameters; }
            int                            entryState(int layer = 0)  const { return lay(layer).entryState; }

            // Mutable access for the UI (edit name/loop/editorPos in place without
            // rebuilding the whole state).
            std::vector<State>&      statesMutable(int layer = 0)      { return lay(layer).states; }
            std::vector<Transition>& transitionsMutable(int layer = 0) { return lay(layer).transitions; }

            // Resolves clipName -> clipIndex and caches duration/ticksPerSecond of
            // each state. A clipName that does not exist in the mesh leaves clipIndex at
            // -1 and pushes a warning (it fails loudly, not silently). It does NOT touch loop.
            // It ends in reset(): meant for scene load / entering Play,
            // where restarting m_currentState and the parameters is the right thing.
            void bindClips(const SkinnedMesh& mesh, std::vector<std::string>* warnings = nullptr);

            // The resolution loop of bindClips, WITHOUT the final reset(). Used by
            // the editor commands (AnimationSourceCommand) that mutate
            // animationClips live: after adding/removing an animation
            // source, m_states[].clipIndex points to indices of the OLD array
            // (or to an index that is now a different clip, see Finding 1 of the
            // review), so it has to be re-resolved by name. But it is live:
            // it can run in the middle of Play Mode, and bindClips's reset()
            // would erase m_currentState and all the user's bool/trigger/int/float,
            // which is exactly what is NOT wanted at that moment (unlike
            // a scene load, where reset() is correct).
            void rebindClips(const SkinnedMesh& mesh, std::vector<std::string>* warnings = nullptr);

            // Rewrites clipName in the states that used oldName. Returns
            // how many it changed. Called by the Animator Panel after renaming a clip
            // of the mesh: the graph references by name, so without this the
            // rename would leave the states orphaned.
            int renameClipReferences(const std::string& oldName, const std::string& newName);

            // --- Runtime ---
            void setBool(const std::string& n, bool v);
            bool getBool(const std::string& n) const;
            void setTrigger(const std::string& n);
            // They return 0 if the parameter does not exist; the setters do nothing
            // if the name is not declared or is of another type (same guard as
            // setBool).
            void  setInt(const std::string& n, int v);
            int   getInt(const std::string& n) const;
            void  setFloat(const std::string& n, float v);
            float getFloat(const std::string& n) const;
            // Is there a DECLARED parameter with that name and of type Float? It is what
            // makes a clip curve resolvable (a track whose destination is
            // a parameter).
            bool  hasFloatParameter(const std::string& n) const;
            // Thresholds of the Float conditions that look at that parameter, in
            // ALL the layers and without repeats. It is what the panel draws over the
            // curve: seeing where it crosses is the only question asked of it at
            // a glance. Returns how many it wrote.
            int   conditionThresholds(const std::string& n, float* out, int max) const;

            // Disarms a trigger that nobody has consumed yet. Undeclared name
            // or of another type: it does nothing, like setTrigger.
            void  resetTrigger(const std::string& n);

            // --- Control from code (Lua) ---
            // They enter the state with that NAME without waiting for any
            // transition. false if it does not exist (nothing is moved). Toward the
            // current state they restart it: an explicit call is intent,
            // not the bounce that canTransitionToSelf avoids in the graph.
            // play cuts any blend; crossFade blends during seconds
            // (<= 0 = cut, same as play).
            bool  play(const std::string& stateName, int layer = 0);
            bool  crossFade(const std::string& stateName, float seconds, int layer = 0);
            // ACCUMULATED normalized time of the current state: 1 = one loop, and
            // in a loop it keeps growing (like normalizedTime in Unity). 0 if
            // the clip has no duration.
            float normalizedTime(int layer = 0) const;
            // Global Animator speed (Unity's animator.speed): scales the
            // dt of every update, cross-fade included. Runtime, it is not saved in
            // the scene. Negative or NaN is clamped to 0 (freezes).
            void  setSpeed(float s);
            float speed() const { return m_speed; }

            // evaluateTransitions == false (Edit Mode): advances the time of the
            // current state but does not move the graph.
            void update(float dt, bool evaluateTransitions);

            int   currentState(int layer = 0)     const { return lay(layer).currentState; }
            int   currentClipIndex(int layer = 0) const;
            float animTime(int layer = 0)         const { return lay(layer).animTime; }   // ticks
            bool  finished(int layer = 0)         const { return lay(layer).finished; }

            // --- Cross-fade in progress ---
            // The state being turned off, -1 if there is no blend. Its clock
            // keeps running (with ITS ticksPerSecond and ITS loop) while it lasts.
            int   previousState(int layer = 0)     const { return lay(layer).prevState; }
            // Like currentClipIndex: falls to 0 if there is no previous state or its clip
            // is not resolved. 0 is a valid SSBO index, so the
            // compute never reads out of bounds even if the graph is half-built.
            int   previousClipIndex(int layer = 0) const;
            float previousAnimTime(int layer = 0)  const { return lay(layer).prevAnimTime; }   // ticks
            // 0 = only the previous state, 1 = only the current one. It is 1 when there is no
            // blend, which is exactly what makes the path without cross-fade
            // need no special case in any consumer.
            float blendWeight(int layer = 0)       const;
            bool  blending(int layer = 0)          const { return lay(layer).prevState >= 0; }
            // Fade in progress: with a live previous state, or from a frozen
            // pose (an interrupted fade, see pose()). blending() still
            // says only the first, which is what root motion and
            // the main pair look at.
            bool  fading(int layer = 0)            const { return lay(layer).prevState >= 0 || lay(layer).frozenFade; }
            // What goes to the GPU: up to 6 weighted samples (the outgoing state
            // and the incoming one, each with its blend samples, up to 3
            // in 2D) and the frozen
            // pose if a fade was interrupted. The weights sum to 1.
            AnimationPose pose() const;
            // The freeze request is sent ONCE: it is turned off by whoever just
            // sent the pose to the backend (applySkinnedFrame).
            void clearFreezeRequest() { for (auto& L : m_layers) L.freezePending = false; }

            // --- The MAIN pair of the pose ---
            // Two clips, their two clocks and the weight (mix(A, B, w)):
            //   - cross-fade in flight: A = the state being turned off, B = the new one,
            //     each with its primary clip.
            //   - otherwise, state with blend: the two neighboring clips of the
            //     parameter value between their thresholds, with linear weight.
            //   - neither: A == B and weight 1.
            // It is the usual two-clip view, for Lua and the tests. What goes
            // to the GPU is pose(), which in a fade carries the whole pairs of
            // the two states (and the frozen pose if it was interrupted).
            int   poseClipA() const;
            float poseTimeA() const;   // ticks
            int   poseClipB() const;
            float poseTimeB() const;   // ticks
            float poseWeight() const;
            // Names fired in the LAST update, in order. It is emptied at the
            // start of each update: whoever reads them once per frame sees them
            // only once.
            const std::vector<std::string>& firedEvents() const { return m_firedEvents; }
            // What each clip with root motion advanced in the LAST update (only
            // Play and only if the current state is Apply), with its weight in the pose.
            // Accumulated ticks, without wrap. It is converted into the rootMotionDelta delta
            // (Renderer/RootMotion.h), which is the one that has the keyframes.
            struct RootMotionSample { int clip; double ticks0; double ticks1; float duration; bool loop; float weight; };
            const std::vector<RootMotionSample>& rootMotionSamples() const { return m_rootMotionSamples; }
            // Root mode of the pose going out to the GPU: 0 Off, 1 Lock, 2 Apply.
            // During a cross-fade the TARGET state wins —the same one that contributes
            // poseClipB—: the push constant carries a SINGLE mode for the whole
            // blend, and the target is the state being entered, so
            // the pose ends up consistent with it.
            uint32_t poseRootMotionMode() const;
            // Name of the current state, "" if the graph is empty. Consumed by Lua.
            std::string currentStateName(int layer = 0) const;
            // Name of the state being turned off in a cross-fade, "" if there is
            // no blend. Consumed by Lua, like currentStateName.
            std::string previousStateName(int layer = 0) const;

            // Returns to the entry state, time to 0, parameters and triggers to
            // false. The Stop of Play does not need to call it (it rebuilds the scene
            // from JSON), but the editor does when re-editing the graph.
            //
            // NOTE: it erases the user's parameters, so it is NOT valid for
            // operations that can run in the middle of Play — the AnimatorPanel
            // allows editing the graph without an isPlaying gate. Those use
            // resetPlayback(), which moves the playhead without touching the values (the
            // same distinction as between bindClips and rebindClips).
            void reset();

            // --- Layers ---
            // Layer 0 always exists and is the base: weight 1, override, no mask
            // (the setters ignore it). The others are applied on top in order.
            int          layerCount() const { return (int)m_layers.size(); }
            const Layer& layer(int i) const { return lay(i); }
            Layer&       layerMutable(int i) { return lay(i); }
            // Returns the index of the new one, -1 if there are already kMaxLayers.
            int   addLayer(const std::string& name);
            // Index of the state with that editorId in the given layer, -1 if it is not there
            // (or the layer does not exist). The layer is mandatory on purpose: the
            // editor identifies nodes by editorId, and searching in the base by
            // default made the nodes of the other layers invisible.
            int   stateIndexByEditorId(int editorId, int layer) const;
            // Leaves the graph of a layer in a representable state: it discards
            // transitions with impossible indices and returns to the root whatever cannot
            // be where it says (nonexistent parent or one that is not a box,
            // entry that is not its child, containment cycles).
            //
            // It exists because statesMutable()/transitionsMutable() expose the whole
            // vectors (A10): the panel is their only legitimate user,
            // but nobody guarantees what it writes. Instead of 9 setters with
            // validation, one pass that is called where it IS known that the graph
            // has just changed: on loading it and on closing the editor's undo
            // session. A healthy graph changes nothing, so calling it too often is
            // free.
            void sanitizeGraph(int layer, std::vector<std::string>* warnings);
            // Is `state` inside `maybeAncestor`, at any depth?
            // One is NOT a descendant of oneself.
            bool  isDescendantOf(int state, int maybeAncestor, int layer) const;
            // Leaf to enter when going to `state`: itself if it is not a
            // box, or the end of the subEntry chain. -1 if the chain
            // breaks (empty box, bad index or cycle): entering halfway into a
            // state that does not exist is worse than not moving.
            int   resolveEntryLeaf(int state, int layer) const;
            void  removeLayer(int i);                 // no la 0
            void  moveLayer(int from, int to);        // neither from nor to 0
            void  setLayerWeight(int i, float w);     // clamped to [0, 1]
            float layerWeight(int i) const;           // 0 is always 1
            void  setLayerMode(int i, LayerMode m);

            // The constraint with that name, null if there is none.
            IkConstraint*       ikPorNombre(const std::string& n);
            const IkConstraint* ikPorNombre(const std::string& n) const;

            // 2D blend: there is a blend (stateBlends) and blendParamY is a declared Float.
            bool stateBlends2D(int stateIdx, int layer = 0) const;
            // The samples of a state with ITS clock: 1 or 2 in 1D (the usual
            // pair, weight 0 included) and up to 3 in 2D. Weights that sum to 1.
            struct BlendSample { int clip; float time; float weight; float duration; };
            int stateBlendSamples(int stateIdx, float animTime, BlendSample out[3], int layer = 0) const;

        private:
            // Leaves the playhead at the entry state and cuts any
            // cross-fade, WITHOUT touching bools/triggers/ints/floats. It is the half of
            // reset() that is safe in the middle of a game.
            void resetPlayback();

            bool conditionsMet(const Transition& t, int layer = 0) const;
            // Single entry point into a state: sets the current one and zeroes its
            // clock, its finished and the accumulated normalized clock. Every path that
            // restarts the playhead goes through here, so that the exit time clock
            // is not left dangling in the one that gets forgotten.
            void enterState(int idx, int layer = 0);
            // Starts the move to state idx: with duration > 0 the current one starts
            // being turned off, otherwise any blend is cut; then enterState.
            // Shared by a graph transition, play and crossFade.
            void startTransitionTo(int idx, float duration, int layer = 0);
            // Index of the state with that name, -1 if there is none.
            int  stateIndexByName(const std::string& name, int layer = 0) const;
            // Whether the transition can fire this frame. n0/n1: accumulated normalized
            // time of the current state before and after advancing
            // the clock. hasDuration false = clip of duration 0 or unresolved,
            // where the exit time counts as reached.
            bool transitionReady(const Transition& t, double n0, double n1, bool hasDuration, int layer = 0) const;
            // The exit time rule, isolated so that it is read in one place.
            static bool exitTimeCrossed(double n0, double n1, float exitTime);
            // Advances the clock of a state dt seconds, applying its loop. Used
            // by the current state and the one being turned off during a cross-fade:
            // both have their own ticksPerSecond and their own loop, and
            // duplicating the loop would let them get out of sync. finished is only
            // written by the current state's one (the previous one no longer cares).
            static void advanceClock(const State& st, float rate, float& time, bool* finished, float dt);
            // Pushes to m_firedEvents the events of st whose instant falls in
            // [ticks0, ticks1), once per crossed cycle (only the first
            // without loop), with a cap of kMaxEventCyclesPerUpdate per event.
            void collectEvents(const State& st, double ticks0, double ticks1);
            // Fills m_rootMotionSamples with what the current state advanced in this
            // update (and the one being turned off, in a fade). ticks0 and
            // prevTicks0: the accumulated clocks BEFORE advancing.
            void collectRootMotion(double ticks0, double prevTicks0);
            // The update of ONE layer. It does not consume triggers: it records the chosen
            // transition in `consumir`, and update consumes them after all the layers,
            // so the same trigger can move several layers in the same frame.
            // The part of applyGraph that belongs to ONE layer: it replaces its graph and repositions
            // its playhead by editorId (see applyGraph).
            void applyLayerGraph(int li, const std::vector<State>& states,
                                 const std::vector<Transition>& transitions, int entryState);
            void updateLayer(int li, float dt, bool evaluateTransitions,
                             std::vector<const Transition*>& consumir);
            // Samples of ONE layer (its current state and, in a fade, the one being
            // turned off), WITHOUT the layer weight. propertySamples is the sum of
            // all the layers and applyCurves needs that of a single one: the split
            // lives in a single place so that there are not two to maintain.
            int  layerPropertySamples(int li, PropertySampleRef* out, int max) const;
            // Writes the parameters of the tracks with Parameter destination of that
            // layer. Called from updateLayer BEFORE evaluating the
            // transitions: this frame's value conditions this frame.
            void applyCurves(int li);
            static constexpr int kMaxEventCyclesPerUpdate = 16;
            // Effective ticks per second of the state: ticksPerSecond x speed x
            // multiplier parameter, never negative.
            float stateRate(const State& st) const;
            // true if there is at least one entry with a RESOLVED clip and blendParam
            // is a declared Float: only then is there a blend to do.
            bool stateBlends(int stateIdx, int layer = 0) const;
            // The two neighboring clips of the parameter, their times and the weight.
            struct BlendPair { int clipA; float timeA; int clipB; float timeB; float weight;
                               float durA = 0.0f; float durB = 0.0f; };   // duration of each clip, ticks
            BlendPair stateBlendPair(int stateIdx, float animTime, int layer = 0) const;
            // The pair of the 1D blend (the two neighbors of the parameter).
            BlendPair stateBlendPair1D(int stateIdx, float animTime, int layer = 0) const;
            // Static because it touches no state: it isolates the four comparators in
            // one place and serves both Int and Float.
            template <typename T>
            static bool evalCompare(T value, Compare op, T threshold)
            {
                switch (op)
                {
                    case Compare::Greater:   return value >  threshold;
                    case Compare::Less:      return value <  threshold;
                    case Compare::Equals:    return value == threshold;
                    case Compare::NotEquals: return value != threshold;
                }
                return false;
            }
            void consumeTriggers(const Transition& t);
            bool isTriggerSet(const std::string& n) const;
            bool hasParam(const std::string& n, ParamType type) const;

            // Layers: 0 is the base (always override, weight 1, no mask).
            // Each one carries its graph and its execution state; parameters,
            // speed, events and root motion belong to the component.
            std::vector<Layer>      m_layers = std::vector<Layer>(1);
            std::vector<Parameter>  m_parameters;
            std::vector<IkConstraint> m_ik;
            std::vector<PropertyClip> m_propertyClips;
            std::vector<std::string> m_firedEvents;
            std::vector<RootMotionSample> m_rootMotionSamples;
            float                   m_speed        = 1.0f;
            std::unordered_map<std::string, bool> m_bools;
            std::unordered_map<std::string, bool> m_triggers;
            std::unordered_map<std::string, int>    m_ints;
            std::unordered_map<std::string, float>  m_floats;

            // Next editorId to hand out in addState, unique across ALL the
            // layers. It is never reset nor is an id freed by
            // removeState reused: while the panel is open in the same frame as
            // a deletion, a repeated id would bring back the visual identity mix-up
            // that this field exists to avoid.
            int                     m_nextEditorId = 0;

            // Layer i clamped to those that exist: an out-of-range index falls to the
            // nearest one instead of reading out of bounds (the public ones filter first).
            Layer&       lay(int i)       { return m_layers[(size_t)std::clamp(i, 0, (int)m_layers.size() - 1)]; }
            const Layer& lay(int i) const { return m_layers[(size_t)std::clamp(i, 0, (int)m_layers.size() - 1)]; }
    };

    // Readable label of a parameter type, shared by AnimatorPanel and
    // PropertiesPanel. It lives here and not in the editor because with four types the
    // "trigger : bool" ternary that both duplicated stops working, and two
    // copies of a switch get out of sync when the fifth type is added.
    //
    // It does NOT reuse (nor is it reused by) paramTypeToStr of Scene.cpp: that is the
    // .scene format and cannot change when tweaking a UI text.
    const char* paramTypeLabel(AnimatorComponent::ParamType t);
}
