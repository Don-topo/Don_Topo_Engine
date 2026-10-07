# Prefabs (Unity-style) — Design

Date: 2026-09-30
Status: draft, pending approval
Reference behavior: Unity. When in doubt, do what Unity does.

## 1. Goal

1. Create a prefab by dragging a GameObject (with its hierarchy) from the scene hierarchy onto the
   ContentBrowser → a `.prefab` file. The scene object becomes a linked instance.
2. Lua: a public script variable of type `Prefab`, assignable in Properties by dropping a `.prefab`,
   plus a global `Instantiate(prefab [, position, rotation, parent])` that returns an `Entity`.
3. Instance link: every instance stores the prefab path and a per-property override list (Unity's
   PropertyModifications). Editing the prefab propagates to every instance (open scene and on scene
   load) except overridden properties. Editing an instance only affects that instance (creates an
   override).
4. Properties on instances: overridden properties in bold, Apply / Revert, context menu
   "Unpack Prefab".
5. Hierarchy: instance names in blue; red when the prefab is missing.

### Out of scope

Nested prefabs, prefab variants, isolated prefab edit mode, `Resources.Load` / runtime loading by
path, replacing an existing `.prefab` by dropping an instance on it, prefab thumbnails.

### Assumptions (agreed)

- There is no GUID system in the repo; every asset is referenced by path. The prefab identity is
  its **path relative to the project root**. Renaming/moving the file outside the editor breaks the
  link (red), same as a `.mat` today.
- The instance root's position and rotation always belong to the instance: they are never
  overrides, never bold, never applied (Unity).
- The existing `Scene.Instantiate(entity, parent)` (clone) stays unchanged. The global
  `Instantiate(prefab, ...)` is new.
- All new identifiers, UI text and comments in English.

## 2. File formats

### 2.1 `.prefab`

Same schema as `Scene::toJson`, with a type tag:

```json
{ "version": 1, "type": "prefab",
  "root": { "id": 1, "name": "Enemy", "localTransform": [...], "mesh": {...},
            "scripts": [...], "children": [ { "id": 2, "...": "..." } ] } }
```

- Nodes are written by the existing `nodeToJson` with `assetRoot` = project root (paths relative to
  the project, exactly like `.scene`), `carryOverrideBaseline = false`.
- `id` inside a prefab is a **prefab-local id** (Unity's fileID). Renumbered 1..N in pre-order at
  creation. A node added to the prefab through Apply gets `max + 1`. They are never used as scene ids.
- Created in the ContentBrowser folder it was dropped on, named `<GameObject name>.prefab`; if it
  exists, `<name> 1.prefab`, `<name> 2.prefab`... (Unity).
- Loading rejects `version != 1` or `type != "prefab"` → template `Failed` with a message.

### 2.2 `.scene` — additive fields only

Only on instance nodes. A scene without prefabs is serialized **byte for byte** as today.

```json
{ "id": 812, "name": "Enemy", "...": "full subtree exactly as today",
  "prefab": { "path": "Prefabs/Enemy.prefab",
              "overrides": [
                { "node": 2, "path": "/light/intensity", "value": 3.5 },
                { "node": 1, "path": "/boxCollider",     "value": null },
                { "node": 2, "path": "/rigidbody",       "value": { "mass": 2.0 } } ] },
  "prefabNode": 1,
  "children": [ { "id": 813, "prefabNode": 2, "...": "..." },
                { "id": 900, "name": "Extra", "...": "no prefabNode = added child" } ] }
```

- The subtree keeps being stored **complete**, plus the link. If the `.prefab` is missing, the
  instance still loads with its last saved state (red in the hierarchy).
- `prefab {}` only on the instance root. `prefabNode` on every node that comes from the prefab.
  A node without `prefabNode` inside an instance is an **added child** (an override).
- `path` is a JSON Pointer inside the node JSON, excluding `id`, `children`, `prefab`, `prefabNode`.
- `localTransform` is compared through virtual paths `/transform/position`, `/transform/rotation`,
  `/transform/scale` (decomposed), so moving an object does not override its scale.
- `value: null` = component removed on the instance. An object value on a component key the prefab
  does not have = component added.
- `/scripts` and `/mesh/materials` are addressed per element by key (`/scripts/Mover#0/...`,
  `/mesh/materials/1/...`), never whole: see §3.5.

### 2.3 Script `Prefab` property

Lua declaration uses a sentinel:

```lua
Spawner = { enemy = Prefab(), speed = 3 }
```

- `ScriptManager::detectProps` recognizes the sentinel as a prop of type Prefab with an empty
  default.
- `ScriptValue` gets a fourth alternative: `std::variant<double, bool, std::string, PrefabRef>`,
  `struct PrefabRef { std::string path; }`.
- Serialized inside the existing script `overrides` (additive):
  `"overrides": { "enemy": { "prefab": "Prefabs/Enemy.prefab" } }`.
- The 5 existing `std::visit` / `std::get` sites are adapted (Scene.cpp serialization,
  ScriptManager::createInstance, PropertiesPanel draw + live update + Reset).
- In Lua the instance field is a `Prefab` userdata (`path` read-only, `IsLoaded()`), or nil when
  unassigned.

## 3. Overrides and propagation

### 3.1 Source of truth

The stored `overrides` list is authoritative. The diff against the template only **detects new
edits**; it never rebuilds the list against a prefab that changed (that would mistake "the prefab
changed" for "the user overrode").

### 3.2 Detection — `PrefabOverrideTracker` (editor, Edit Mode only)

Single entry point, not per widget (a per-widget obligation is the pattern that already failed
four times in this repo):

- When `UndoManager`'s revision changes (push / undo / redo) → recompute every instance in the scene.
- On the falling edge of `ImGui::IsAnyItemActive()` in the Properties panel → recompute the selected
  instance. Covers edits that do not go through a command today (e.g. script props).
- Recompute = `nodeToJson(instance node)` vs template node JSON matched by `prefabNode`, with the
  template paths resolved once when the template loads (no `filesystem::relative` per field).
  Differences are **merged** into the list; entries whose value is back to exactly the prefab value
  are **removed** (Unity). Floats inside the decomposed transform compare with 1e-5 tolerance;
  everything else exact.
- Always excluded: `id`, `prefabNode`, `prefab`, root position/rotation.
- Added children: any node without `prefabNode` under an instance. Removed component: component key
  present in the template and absent in the instance.

### 3.3 Propagation

Triggered by Apply (to every instance of that prefab in the open scene) and on scene load when the
template differs from the stored subtree (the stored subtree is re-derived; if equal, nothing
changes, so unchanged scenes stay byte-identical).

1. `json = template` → apply `overrides` patches (value / `null` removes / object adds) → re-attach
   added children under their `prefabNode` parent.
2. Replace the instance subtree. Nodes matched by `prefabNode` **keep their scene id** (selection
   and older Undo commands keep resolving). Nodes new in the prefab get a **fresh id** via
   `GameObject::allocateId`. The result goes through the existing live-id guard of
   `insertFromJson`.
3. GPU: replacement goes through `removeGameObject` (`onNodeRemoved` releases) and
   `registerGameObject`.

Timing: on scene load in the editor the stored subtree is shown immediately; propagation runs when
its template becomes `Ready` (asynchronous). Load-time propagation is not an Undo command and does
not mark the scene dirty (Unity). The exported runtime never propagates: `game.scene` is written
from the editor's in-memory scene, already propagated.

Edge cases (Unity):
- Override on a node or component that no longer exists in the prefab → dropped on propagation,
  warning in the Log.
- Prefab missing/failed → no propagation, red name, overrides untouched and saved as they are.
- Play Mode → no detection, no propagation. Instances created in Play keep the link (blue) and are
  discarded on Stop like any Play object.

### 3.4 Apply / Revert / Unpack

- **Apply** (per field, per component, or all): write the override into the `.prefab`
  (re-serialized with `assetRoot` = project root), remove it from the instance, propagate to every
  other instance. Applying an added child adds it to the prefab with a new prefab-local id; applying
  a removed component removes it from the prefab.
- **Revert** (per field, per component, or all): drop the overrides and re-propagate that instance.
  Reverting an added child deletes it; reverting a removed component restores it.
- **Unpack**: strip `prefab` / `prefabNode` from the subtree. The objects stay as they are.

### 3.5 Arrays: keyed per element (added after review Z1)

A JSON Pointer diff compares arrays whole, so one edited script property used to store the whole
`/scripts` array as a single override, which then shadowed every script field of the template (an
instance overriding `label` never received a new `speed` applied from another instance). Review
Focus 2 must hold for arrays too, so two arrays are recorded **per element, by a stable key**:

| Array | Key of an element | Paths |
|---|---|---|
| `/scripts` | `<name>#<k>`: the k-th script with that name (k from 0) | `/scripts/Mover#0/overrides/speed` (one property), `/scripts/Extra#0` with an object (a script the instance added) or `null` (a template script it removed), `/scripts/#order` (see below) |
| `/mesh/materials` | `<index>`: the entry's submesh index | `/mesh/materials/1/roughness`. The array is sparse (only slots with something to say), so a missing entry equals `{"index": k}`: per-field entries work whether or not either side has the slot |

- Keys, never positions: an added or removed element does not shift the others.
- **Order.** Material entries are unordered (keyed by slot). Script order matters (execution
  order), so the instance's order is recorded as `/scripts/#order` = its keys in order, **only**
  when it differs from "the template's order, then the instance's added scripts in its own order".
  Applying it sorts the listed keys first; anything not listed keeps its relative order after them.
  A pure reorder is therefore one entry, and adding a script at the end needs none.
- **Apply** of keyed entries happens per (node, array), after the node's other entries, against
  the keys the template array had *before* any of them (removing `Mover#0` never turns `Mover#1`
  into `Mover#0` mid-way). A removal that leaves a material slot empty removes the slot, and an
  empty array is not written (as `nodeToJson`).
- **Known limitation: scripts with the same name (accepted, pinned by
  `testDuplicateScriptNamesLimitation`).** A key is each side's own occurrence numbering, not a
  stable identity of the script, so with two or more scripts of one name:
  - removing the *first* of two `Mover`s on an instance records "`Mover#1` removed" plus
    "`Mover#0` = the second one's fields";
  - the template inserting or removing a same-named script shifts the keys: an instance's
    `Mover#0` entry then lands on another `Mover`, or (the key no longer exists) becomes an
    orphan, dropped with one warning;
  - a script the instance added as `Extra#0` meets a later template `Extra` on the same key: the
    instance's whole element replaces the template's (their fields are not merged).
  The real fix is a stable per-script id written in the script JSON (like `prefabNode` for
  nodes); it changes the scene format and is left for later. Scripts with distinct names (the
  common case) are not affected.
- **A stable stored list.** For a keyed array, `applyPrefabOverrides` returns the difference the
  group made in the diff's own form (one entry per path, a field merged into the whole added
  element it lies in, nothing for an entry equal to the template), not the entries it was given.
  `recomputePrefabOverrides` compares lists as multisets and keeps the stored order when only the
  order differs. Together: a rebuild stores what the next recompute finds, and consecutive saves
  of an unedited scene are byte-identical (also for a converted old scene).
- **Whole values.** Every other array is a single value and recorded whole: numeric vectors
  (positions, colors, anchors), `mesh/animationSources` (an ordered list that defines the mesh's
  clip configuration and identity), procedural `vertices`/`indices`, the animator graph (states,
  transitions and parameters refer to each other by name; a partial merge could leave dangling
  transitions) and dropdown `options` (the selected index points into it).
- **Backward compatibility.** A stored entry on `/scripts` or `/mesh/materials` with an array (or
  `null`) value is the format before keyed arrays. `applyPrefabOverrides` turns it into the
  per-element entries of its difference with the template array, applies those (same result as
  replacing the array) and returns them, so the rebuild stores the converted list. Every instance
  that carries one is rebuilt once when its template is first Ready (it does not match the template
  in the new format), and is saved in the new format afterwards. The conversion is made against the
  template as it is *now*: a field the prefab changed since the scene was saved, and the instance
  never touched, stays at the old value (exactly what the old whole array did) — from then on it
  follows the prefab. Scenes without prefabs, or without such entries, are not affected.

## 4. Preload and `Instantiate`

### 4.1 `PrefabLibrary` (Core, one per host)

```
PrefabTemplate {
  state: Loading | Ready | Failed(error)
  json                  // prefab-local ids; ids stripped once for instantiation
  PreloadedMeshCache    // shared_ptr<const Mesh>: existing COW (getMesh/editMesh) and B6 sharing
  NodeLoadCache seeds   // hasBones
  matAssets             // .mat files already read
  audioSettings         // audio sidecars already read
}
```

### 4.2 Pipeline — no IO on the main thread

1. On scene load (editor and runtime), walk the script `overrides` of the scene for
   `{"prefab": ...}` → `request(path)`. Recursive: a template that becomes `Ready` is scanned the
   same way (Enemy with a `Bullet` var). Also requested when a `.prefab` is dropped on a Prefab
   script var.
2. `JobSystem` job: read + parse the `.prefab`; read the `.mat` files and audio sidecars it
   references.
3. Meshes: a **dedicated** `AsyncAssetLoader` instance (own mailbox, never mixed with the editor's
   `onAssetsLoaded`), pumped by `PrefabLibrary::pump()` once per frame.
4. A skinned mesh with external animation sources → second job applies the animation config on the
   worker, so `meshMatchesAnimationConfig` is true at `Instantiate` time and the mesh is **shared**.
   The worker also converts the file JSON to **memory form** (absolute paths, via
   `resolveStoredPaths`, the counterpart of `storeRelativePaths`; a test ties both to
   `nodeFromJson`/`nodeToJson`), so neither `Instantiate` nor override detection calls
   `filesystem::relative` / `weakly_canonical`.
5. On `Ready` (main thread) the template warms its audio clips once (`createAudioClipComponent`,
   kept alive by the template), so the sidecar is read there and `Instantiate` hits the
   AudioManager sound cache.
6. `Ready` → host callback registers the template meshes with the Renderer once (textures and
   geometry into the shared caches / B6), so the GPU registration of every `Instantiate` also hits
   the caches.

- **Exported runtime**: preload runs during the splash; the first frame waits for every template
  to be `Ready` or `Failed`.
- **Editor**: asynchronous; Play does not wait.
- Loading another scene evicts templates no longer referenced; project change clears the library.

### 4.3 Zero IO in `Instantiate`

Today `cloneGameObject` still touches disk in three places even with the mesh cache:
`loadMaterialAsset` (`.mat`, via `applyMaterialOverrides`), external animation sources (Assimp
`ReadFile`) when the config does not match, and the audio sidecar + `filesystem::exists` of the
skinned FBX. `nodeFromJson` gets an optional cache bundle (mat assets, audio settings) and skips
`filesystem::exists` when the mesh came from the cache. Existing callers pass nothing and behave
exactly as before.

### 4.4 Lua API

```lua
local e = Instantiate(self.enemy)                          -- Unity overloads
Instantiate(self.enemy, parentEntity)
Instantiate(self.enemy, Vec3(0, 1, 0), Vec3(0, 90, 0))     -- rotation: euler degrees, like Transform
Instantiate(self.enemy, pos, rot, parentEntity)
```

- Returns an `Entity`.
- Errors via `luaL_error`, returning immediately:
  - not ready: `Instantiate: prefab 'Prefabs/Enemy.prefab' is not loaded yet (still loading)`
  - failed: `Instantiate: prefab 'Prefabs/Enemy.prefab' failed to load: <error>`
  - nil: `Instantiate: no prefab assigned`
- Implementation: `Scene::instantiatePrefab(template, parent, pos, rot, physics, audio)`. Fresh ids
  (stripped from the JSON, like `cloneGameObject`), drops a CameraComponent like clone, sets the
  link. Then `onInstantiated` + Awake. The Awake block is shared with `Scene.Instantiate`, which is
  otherwise unchanged.
- Registered in `ScriptBindings.cpp`, **and** in `LuaApiReference.cpp` (autocomplete), README
  `## Lua Scripting` and `Scripts/README.md`.

### 4.5 Measurement

Release benchmark: 100 × `Instantiate` of a prefab with a skinned FBX → ms per call (Core). GPU
registration measured separately with the runtime harness.

## 5. Editor

### 5.1 Create

Dragging a hierarchy node (existing `DT_GAMEOBJECT` payload) onto the ContentBrowser grid or folder
tree → `CreatePrefabCommand`:

- Writes the `.prefab`, links the subtree (`prefab {}` + `prefabNode`), registers the template in
  `PrefabLibrary`.
- **Undo** = unlink + delete the `.prefab`, **only if its bytes are exactly the ones the command
  wrote** (guard by content, not by path; if anything else touched it, it stays and the Log says
  so). Redo rewrites it. **This deletes a file — explicit approval requested.**
- A node that is already an instance, or inside one → Log "already part of a prefab instance"
  (no nesting).

### 5.2 Instantiate in Edit Mode

Dragging a `.prefab` from the ContentBrowser onto the hierarchy → `InstantiatePrefabCommand`
(reuses the `CreateGameObjectCommand` snapshot). In Edit Mode a synchronous load is allowed when the
template is not ready.

### 5.3 Hierarchy (ScenePanel)

- Nodes with `prefabNode`: blue (≈ `#7BB8FF`). Template `Failed` or file missing: red (`#FF6B6B`).
  Added children: normal color.
- Context menu on the instance root: **Unpack Prefab**, **Select Prefab Asset** (highlights it in
  the ContentBrowser).

### 5.4 Properties

- Top bar on an instance: `Prefab: Enemy.prefab` · [Select] · [Overrides ▾]. The dropdown lists
  every override (`Node › /light/intensity`) with per-row Apply / Revert, plus **Apply All** /
  **Revert All**.
- Bold (mixed granularity): per field for Name, Transform and script props; on the component
  header when the component has any override. The editor uses ImGui's default font, which has no
  bold face → **simulated bold** (text drawn twice, 1 px offset) in a `boldLabel()` helper; no new
  fonts or dependencies.
- Right click on a bold field or header → `Apply to Prefab 'Enemy'` / `Revert`.
- Deleting a child that comes from the prefab → blocked with "Unpack the prefab to delete this
  object" (Unity).
- Script Prefab prop: slot `[Enemy.prefab] (x)` accepting `DT_ASSET_PATH` with `.prefab`
  extension (stored relative to the project), triggers `PrefabLibrary::request`.

### 5.5 Commands (UndoManager, resolve by id, never raw pointers)

| Command | execute | undo |
|---|---|---|
| `CreatePrefabCommand` | write `.prefab` + link | unlink + delete (content guard) |
| `InstantiatePrefabCommand` | insert from template | delete |
| `ApplyPrefabOverridesCommand` | write new `.prefab` + propagate to all | write previous content + propagate |
| `RevertPrefabOverridesCommand` | drop overrides + propagate instance | restore overrides + subtree snapshot |
| `UnpackPrefabCommand` | strip link from subtree | restore it |

## 6. Export (GameExporter)

- `collectSceneAssets` also collects the `.prefab` files referenced by script props,
  **recursively** (prefab → its script props → other prefabs).
- Assets inside each prefab are collected by loading it into a temporary `Scene` and running the
  **same** `collectSceneAssets`, not a second walker that drifts from the 28 components.
- Each packaged `.prefab` is rewritten with `rewriteNode` (same rules as `game.scene`), and the
  `{"prefab": ...}` paths in `game.scene` and in the prefabs point into the package.
- Runtime: `PrefabLibrary` rooted at the package; preload during the splash.

## 7. Ids — the four paths

Every instance node gets a fresh unique scene id on: clone (`cloneGameObject`, existing strip),
undo of Delete (`insertFromJson` live-id guard, existing), `Instantiate` (strip from template), and
propagation (kept by `prefabNode` for existing nodes, fresh for new ones, then the live-id guard).
Prefab-local ids never reach the scene id space.

## 8. Tests

New `engine/tests/prefab_tests.cpp` + Lua cases in the scripting tests. Always run from the repo
root.

1. **Round-trip**: prefab from a subtree with mesh, collider, script and child → file → template →
   instance → JSON equal except ids and link fields.
2. **Propagation respects overrides**: 2 instances, override on A, Apply from B → A keeps its
   override and receives the rest. Scene saved with a newer prefab on disk → propagated on load.
   Orphan override → dropped with warning.
3. **Apply / Revert / Unpack / Create + undo/redo**; Create's undo does **not** delete a `.prefab`
   modified afterwards.
4. **Unique ids across the 4 paths**: Instantiate ×100 + clone + undo of Delete + propagation →
   zero duplicate ids in the scene; selection and older commands still resolve.
5. **No IO**: preload → delete the temp copies of `.prefab` / FBX / `.mat` / sidecars →
   `Instantiate` gives the same result. **Sabotage** (skip the preload, or empty one cache, one at a
   time) → the test **fails**; recorded.
6. **Not ready** → Lua error with the exact message, immediate return.
7. **Backward compatibility**: the repo's `.scene` files → load + save byte for byte identical.
8. `PrefabRef` in `ScriptValue`: `Prefab()` detection, serialization, Properties.
9. **Export**: the package contains the `.prefab` with rewritten paths, and a `Scene` +
   `PrefabLibrary` rooted at the package instantiates it.
10. **Release benchmark**: ms per `Instantiate` of a skinned prefab (100 calls), printed and
    reported.

## 9. Done when

Full suite green in Debug and Release, new tests green, Vulkan and D3D12 both compile, ms per
`Instantiate` measured, and a manual GUI checklist: create prefab, hierarchy colors, bold override,
Apply / Revert, `Instantiate` from Lua in Play, exported game.
