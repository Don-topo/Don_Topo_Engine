# Lua Scripting API — Don Topo Engine

Complete reference of the methods available to Lua scripts. See the root `README.md`
for the general overview; this document details every class/table exposed by
`ScriptBindings.cpp`.

## How a script is defined

Each file `Scripts/<Name>.lua` defines a global table `<Name>`: its class name. It is
attached to a GameObject through **Properties → Add → Script**.

```lua
Rotator = {
    speed = 45   -- serializable prop (number/boolean/string), auto-UI in the editor
}

function Rotator:Awake() end
function Rotator:Start() end
function Rotator:Update(dt) end
function Rotator:FixedUpdate(dt) end
function Rotator:LateUpdate() end
function Rotator:OnDestroy() end
```

## Lifecycle

| Callback | When |
| --- | --- |
| `Awake()` | When the instance is created (Play start or `Scene.Instantiate`), before any `Start`/`Update` |
| `Start()` | Once, before the first `Update` |
| `Update(dt)` | Every frame, in Play Mode |
| `FixedUpdate(dt)` | Fixed step (`1/60`), accumulator capped against the spiral of death |
| `LateUpdate()` | Every frame, after all the `Update` calls |
| `OnDestroy()` | When the GameObject is destroyed or the component removed |
| `OnTriggerEnter(other)` | Another collider enters this trigger |
| `OnTriggerStay(other)` | Every physics frame while they keep overlapping |
| `OnTriggerExit(other)` | The other collider leaves |
| `OnCollisionEnter(other)` | A real hit (neither collider is a trigger): contact begins |
| `OnCollisionStay(other)` | They stay in contact |
| `OnCollisionExit(other)` | They separate |

All of them are optional: only the ones the script defines are called. An error in any
of them logs the message and **disables that component** (it stops receiving callbacks)
until a hot reload or `Stop`; it never crashes the engine.

### Triggers

The three `OnTrigger*` callbacks require the GameObject to have a collider with **Is
Trigger** checked in Properties. `other` is an `Entity`, just like `self.entity`.

Three rules you find out late if nobody tells you:

- **At least one of the two objects needs a Rigidbody**: the trigger or the one entering,
  either one. PhysX does not report overlaps between two static objects, and a collider
  without a Rigidbody is static. Without it nothing fires and there is no error: the
  editor warns about it under the checkbox. Same rule as Unity.
- **Only the trigger side receives the callbacks.** The object that enters is not told,
  unless it is also a trigger against a non-trigger.
- **Trigger against trigger fires nothing**, because of the same PhysX limitation.

`OnTriggerStay` is synthesized per frame (PhysX only gives Enter and Exit), so logging
there floods the console quickly.

Examples: `Scripts/TriggerProbe.lua` (counts entries and exits) and
`Scripts/TriggerTest.lua` (destroys its GameObject on Enter).

### Collisions

`OnCollisionEnter`, `OnCollisionStay` and `OnCollisionExit` are the twins of the
`OnTrigger*` callbacks for pairs that **really collide**, that is, pairs where
**neither** collider is a trigger. They receive the same single argument (the other
`Entity`) and run at the same point of the frame (physics first, then the `Update`
calls).

```lua
function Bullet:OnCollisionEnter(other)
    Log.Info("hit against " .. other.name)
    DestroyGameObject(self.entity)
end
```

The differences worth keeping clear:

| | `OnTrigger*` | `OnCollision*` |
| --- | --- | --- |
| When | one of the colliders has `isTrigger = true` | neither is a trigger |
| Who is called | the script of the **trigger** object | the scripts of **both** objects |
| Physical response | none, they pass through | PhysX resolves the impact |
| Where `Stay` comes from | synthesized, one per physics sub-step | PhysX's native `TOUCH_PERSISTS` |

The two families are **mutually exclusive per pair**: a trigger generates no contacts,
so setting `isTrigger = true` mid-game silently changes which family that collider
receives. The layer matrix filters both the same way: a pair turned off in
`Physics.SetLayerCollision` produces neither. And the "at least one Rigidbody" rule
still holds: two static colliders do not form a pair, so neither family fires.

Scripts only run in **Play Mode**. `self.entity` (type `Entity`, see below) is injected
into the instance automatically.

## Serializable props

Any `number`/`boolean`/`string` field in the class table is detected as a prop and
shows up in Properties (DragInt for Lua integers, DragFloat for floats). Only the values
edited in the editor (the ones that differ from the default) are serialized in the scene.

## Hot reload

Editing a loaded `.lua` while the engine runs reloads it (~1 s polling), keeping the
prop values already assigned.

## Script Editor

Double-clicking a `.lua` in the Content Browser, or the **Edit** button next to the
`ScriptComponent` in Properties, opens it in the **Script Editor** panel: a multi-tab
code editor (ImGuiColorTextEdit, Lua highlighting) docked with the rest of the panels.
`Ctrl+S` or the **Save** button write the file to disk; the hot reload polling picks up
the change like any external edit. Closing a tab with unsaved changes asks to
save/discard/cancel.

| Shortcut | What it does |
| --- | --- |
| `Ctrl+S` | Save |
| `Ctrl+F` | Open the **find / replace** bar |
| `F3` / `Shift+F3` | Next / previous match, without going back to the bar |
| `Ctrl+G` | **Go to line** |
| `Ctrl+Space` | Open autocomplete by hand |
| `Enter` / `Tab` | Accept the suggestion |
| `Escape` | Close the popup, or the find bar if it has focus |

Search **wraps around** to the opposite end when it reaches the end, and the `Aa` box
decides whether it matches case. **Replace** only substitutes if the selection *is* the
match: the first click without a previous search only searches and touches nothing.
**All** replaces in one pass over the whole text and says how many times.

The **syntax check** (it only compiles, it does not run) runs **while you type**, not
only on save: it waits a few quiet frames so it does not fire in the middle of a
half-typed word. The error shows as a mark on the offending line (with the message on
hover) and also in the **status bar** at the bottom, which also shows line, column and
line count; clicking the message moves the cursor to the error line. A file that is
already broken on disk shows the error as soon as it opens.

When Lua reports the error at `<eof>` (which happens when you delete an `end`, and lands
on a line that does not exist), the line where the unclosed block was **opened** is
marked, which is where the real problem is.

If the file **changes on disk** while it is open, the tab notices: if it has no changes
of its own it reloads by itself and says so in the Log Console; if it does, it asks,
because either option loses someone's work. The **Reload** button does the same by hand.

Opening a `.lua` (from the Content Browser or from the `ScriptComponent`'s **Edit**
button in Properties) not only opens the panel but also **brings it to the front**. If it
was docked behind another tab, that tab now comes forward; without that, the file opened
where it could not be seen. **Edit sprites...** does the same with the Sprite Editor
panel.

A script can also be created from scratch with **Properties → Add → Script → New
Script...**, which generates a `.lua` from a template.

---

## Vec3

Constructor `Vec3.new(x, y, z)` or `Vec3.new()` (zero). The table is also **callable**,
so `Vec3(x, y, z)` and `Vec3()` do exactly the same; the examples in this document use
both forms interchangeably. Fields `.x/.y/.z`.

Operators: `+`, `-` (binary and unary), `* scalar` **on both sides** (`v * 2` and
`2 * v`), `/ scalar`, `==` (component-wise) and `tostring`.

| Method | Description |
| --- | --- |
| `v:Length()` | Length of the vector |
| `v:Normalized()` | Copy with length 1. The zero vector is returned **as is**, not `NaN` |
| `a:Dot(b)` | Dot product |
| `a:Cross(b)` | Cross product |
| `a:Distance(b)` | Distance between the two points |
| `a:Lerp(b, t)` | Linear interpolation. `t` outside `[0,1]` **extrapolates** (like `glm::mix`, unlike Unity) |

None of them mutates the receiver: they all return a new value.

Dividing by zero gives `inf`/`NaN`. It does not blow up right there: the non-finite value
guard catches it as soon as the result tries to enter an engine setter (the value is
ignored and a warning goes to the Log Console).

## Time

The scripts' clock. The engine fills it **on every `update`**, before calling any
callback, so the `Awake` and `Start` of a new component already see the `Time` of their
own frame.

| Field | Description |
| --- | --- |
| `Time.deltaTime` | Seconds of the last frame. The same as the argument `Update` receives |
| `Time.fixedDeltaTime` | `FixedUpdate`'s fixed step, in seconds. Constant |
| `Time.time` | Seconds since the **current** Play started |
| `Time.frameCount` | Frames since the current Play started |

Entering Play resets `time` and `frameCount` to zero: a second Play after a Stop does not
continue where the previous run left off. A non-finite `dt` (a degenerate frame) is
ignored entirely instead of leaving `Time.time` as `NaN` forever.

The table is writable from Lua, but that is useless: the real accumulator lives in C++
and the next frame restores the right value.

```lua
function Counter:Update(dt)
    -- dt and Time.deltaTime are the same number.
    if Time.time > 5.0 then
        Log.Info("5 seconds and " .. Time.frameCount .. " frames have passed")
    end
end
```

## Log

| Method | Description |
| --- | --- |
| `Log.Info(msg)` | Normal log in the Log Console |
| `Log.Warn(msg)` | Log with a `[WARN]` prefix |
| `Log.Error(msg)` | Log with an `[ERROR]` prefix |

Lua's native `print(...)` is also redirected to the Log Console (same destination as
`Log.Info`).

## Input

| Method | Description |
| --- | --- |
| `Input.IsKeyDown(key)` | true while the key is held |
| `Input.IsKeyPressed(key)` | true only on the frame it was pressed |
| `Input.IsKeyReleased(key)` | true only on the frame it was released |
| `Input.IsMouseButtonDown(button)` | true while the button is held |
| `Input.IsActionDown(name)` | true while the **action** is active |
| `Input.IsActionPressed(name)` | true only on the frame it became active |
| `Input.IsActionReleased(name)` | true only on the frame it was released |

Constant tables: `Key.Space/Enter/Escape/Tab/LeftShift/LeftControl/
Up/Down/Left/Right/A..Z/Num0..Num9`, `MouseButton.Left/Right/Middle`.

### Gamepad

The normal thing is to use **named actions** (the Input Actions panel already knows about
gamepads, and that way the script does not depend on the device). These four are the raw
gamepad, for when the script wants a specific button:

| Method | Description |
| --- | --- |
| `Input.IsPadButtonDown(button)` | true while the button is held |
| `Input.IsPadButtonPressed(button)` | true only on the frame of the edge |
| `Input.IsPadAxisDown(code)` | true while the axis direction is active |
| `Input.IsPadAxisPressed(code)` | true only on the frame of the edge |

Without a connected gamepad they return `false`, never an error. They refer to the first
connected gamepad with a known mapping.

`PadButton.A/B/X/Y/LeftBumper/RightBumper/Back/Start/Guide/LeftThumb/
RightThumb/DpadUp/DpadRight/DpadDown/DpadLeft`.

**Axes** (sticks and triggers) are not queried by axis but by **direction**: an axis is
two separate bindings, because "left stick up" and "down" are two different things.
Ready-made constants: `PadAxis.LeftStickUp/Down/Left/Right`,
`PadAxis.RightStickUp/Down/Left/Right`, `PadAxis.LeftTrigger`, `PadAxis.RightTrigger`.
Any other is built with `PadAxis.Code(axis, negative)`.

The names say which way it is pushed, not the sign: in GLFW the sticks' Y axis grows
downwards, so `Up` is the negative axis. Triggers come in `[-1,1]` with rest at `-1` and
the engine renormalizes them to `[0,1]`, so they only have a positive direction.

### Named actions

The three `IsAction*` functions query the **actions** defined in the **Input Actions**
panel, not a specific key: that is how "jump" can be the space bar or the gamepad's A
button without the script knowing which. The name is the action's, exactly.

An unknown name returns `false` and warns **only once per name and session**: the typical
call lives in `Update()` and one warning per frame would drown the Log Console.

## Entity (`self.entity`)

| Method/prop | Description |
| --- | --- |
| `entity.name` | Read/write the GameObject's name |
| `entity.meshVisible` | Shows or hides the mesh. The object stays alive, colliding and running its scripts |
| `entity:IsValid()` | false if the entity was destroyed |
| `entity:GetTransform()` | Returns the `Transform` |
| `entity:GetParent()` | The parent's `Entity`, or `nil` if it is at the root |
| `entity:SetParent(parent?, keepWorldPose?)` | Changes the parent. Without an argument (or `nil`) it hangs it from the root. Returns `false` if the target is not valid |
| `entity:GetChildren()` | Table (1-based array) of child `Entity` values |
| `entity:GetComponent(name)` | Returns the component if it exists, otherwise `nil`. `name`: `"BoxCollider"`, `"SphereCollider"`, `"CapsuleCollider"`, `"PlaneCollider"`, `"AudioClip"`, `"ReverbZone"`, `"Rigidbody"`, `"Animator"`, `"Canvas"`, `"Button"`, `"Text"`, `"ProgressBar"`, `"Layout"`, `"Panel"`, `"Image"`, `"Slider"`, `"Checkbox"`, `"Toggle"`, `"Scrollbar"`, `"InputField"`, `"Dropdown"`, `"ScrollView"`, or `"Script:<ClassName>"` to reach the instance of another script on the same GameObject |
| `entity:AddComponent(name, arg?)` | Adds a component (same defaults as the editor's Add button; colliders are mutually exclusive). `AudioClip` requires `arg` = the asset path. UI components do not exclude each other (they all fit on the same GameObject) and asking for one that is already there returns the existing one. `"Script:<Name>"` adds the script (Awake/Start fire on the next lifecycle update) |
| `entity:RemoveComponent(name)` | Removes the component (scripts are removed deferred, at the end of the frame) |
| `entity:GetCanvas()` / `GetButton()` / `GetText()` / `GetProgressBar()` / `GetLayout()` / `GetPanel()` / `GetImage()` / `GetSlider()` / `GetCheckbox()` / `GetToggle()` / `GetScrollbar()` / `GetInputField()` / `GetDropdown()` / `GetScrollView()` | The UI component, or `nil` if it has none |
| `entity:AddCanvas()` / `AddButton()` / `AddText()` / `AddProgressBar()` / `AddLayout()` / `AddPanel()` / `AddImage()` / `AddSlider()` / `AddCheckbox()` / `AddToggle()` / `AddScrollbar()` / `AddInputField()` / `AddDropdown()` / `AddScrollView()` | Creates it with the component's default values and returns the wrapper; if it already exists, returns the existing one without overwriting it |
| `entity:RemoveCanvas()` / `RemoveButton()` / `RemoveText()` / `RemoveProgressBar()` / `RemoveLayout()` / `RemovePanel()` / `RemoveImage()` / `RemoveSlider()` / `RemoveCheckbox()` / `RemoveToggle()` / `RemoveScrollbar()` / `RemoveInputField()` / `RemoveDropdown()` / `RemoveScrollView()` | Removes it from the GameObject |
| `entity:GetLight()` / `AddLight()` / `RemoveLight()` | Light component, with the same contract as the UI ones (`Get` returns `nil` if missing, `Add` does not overwrite an existing one) |
| `entity:GetCamera()` / `AddCamera()` / `RemoveCamera()` | Game camera, same contract |

`"Light"` and `"Camera"` are also valid names for
`GetComponent`/`AddComponent`/`RemoveComponent`.

### Changing the parent

By default `SetParent` keeps the **world pose**, like Unity's `transform.parent`: the
object stays exactly where it is and its local transform is what gets recomputed.

```lua
local gun  = Scene.Find("Gun")
local hand = Scene.Find("RightHand")

gun:SetParent(hand)          -- stays where it is and now follows the hand
gun:SetParent(hand, false)   -- keeps the local: it JUMPS to the hand's origin
gun:SetParent()              -- lets go: it hangs from the root again
```

The `false` is what dragging in the editor's hierarchy does.

It returns `false` **without touching anything** (and warns in the Log Console) if the
target is inside the object's own subtree. That is not an odd scene: it would detach the
subtree from the tree and take down what keeps it alive.

The subtree's `worldTransform` values are up to date **immediately**, not on the next
frame: a `GetWorldPosition()` on the following line already reads the right value.

A parent with scale 0 has no inverse. In that case the local pose is kept and a warning
is issued, instead of baking a `NaN` into the matrix that would drag the children along.

If the object has a collider, reparenting with `keepWorldPose` set to `true` does not move
it, so there is no teleport. With `false` it jumps, and what
[Moving through Transform does NOT collide](#moving-through-transform-does-not-collide)
says applies.

## Light

Scene light (`GetComponent("Light")` or `entity:GetLight()`). **It stores no position or
direction**: both come from the GameObject's transform (the object's position, local
`-Z` direction), so to move or aim a light you move or rotate its object.

| Prop/method | Description |
| --- | --- |
| `light.type` | `LightType.Point/Spot/Directional/Area` |
| `light.intensity` | Color multiplier, clamped to `0..100` |
| `light.range` | Range of point and spot lights. Directional does not attenuate and ignores it; area uses its width/2 |
| `light.innerAngle` / `light.outerAngle` | Spot cone, in **half-angle degrees**. The inner never exceeds the outer |
| `light.areaWidth` / `light.areaHeight` | Sides of the area light's rectangle |
| `light:GetColor()` / `light:SetColor(Vec3)` | RGB color `0..1`, **without** the intensity premultiplied |

The color goes through a method and not a property on purpose: since it is a `Vec3`,
`light.color.x = 1` would write into a temporary copy and be lost without a warning.

The ranges are clamped by the core, not the UI, so an out-of-range value is clipped
(`intensity = 500` leaves 100). A `type` that is not in the `LightType` table **is not
applied**: a warning goes to the Log Console and the previous one is kept. `NaN`/`Inf`,
the same.

There is no uniqueness invariant: several lights fit in a scene, and the engine keeps the
first `MAX_LIGHTS` in scene order.

## Camera

Game camera (`GetComponent("Camera")` or `entity:GetCamera()`). It stores no position or
orientation either (they come from the transform), nor an aspect ratio, which the
viewport dictates.

| Prop | Description |
| --- | --- |
| `camera.mode` | `CameraProjection.Perspective` or `Orthographic` |
| `camera.fov` | Field of view in degrees. Perspective **only** |
| `camera.orthographicSize` | Visible half-height in world units. Orthographic **only** |
| `camera.near` / `camera.far` | Clipping planes |

Adding a camera from Lua does **not** check that there is no other one in the scene: the
"one per scene" invariant is enforced by the engine keeping the first one in pre-order,
as with the AudioListener.

```lua
function Switch:Update(dt)
    local l = self.entity:GetLight()
    if l then
        l.intensity = 2.0 + math.sin(Time.time * 3.0)
    end
end
```

## Transform

| Method | Description |
| --- | --- |
| `t:GetPosition()` / `t:SetPosition(Vec3)` | Local position |
| `t:GetRotation()` / `t:SetRotation(Vec3)` | Local rotation in Euler degrees |
| `t:GetScale()` / `t:SetScale(Vec3)` | Local scale |
| `t:GetWorldPosition()` | World position (translation of the world matrix) |
| `t:Translate(Vec3 delta)` | Adds delta to the local position |
| `t:Rotate(Vec3 deltaEulerDegrees)` | Incremental rotation composed as a quaternion (it does not get stuck in continuous multi-axis rotation) |
| `t:SetWorldPosition(Vec3)` | Places the object at that **world** position (undoes the parent's transform) |
| `t:GetForward()` | The object's `-Z` axis in world space, normalized |
| `t:GetRight()` | The object's `+X` axis in world space, normalized |
| `t:GetUp()` | The object's `+Y` axis in world space, normalized |
| `t:LookAt(target: Vec3, up: Vec3?)` | Rotates the object so its forward points at the point. `up` defaults to `(0,1,0)` |

The convention is the camera's and `glm::lookAt`'s: the object looks along local `-Z`.
The three axes come out already normalized; read raw, a scaled object would give vectors
longer than 1.

`LookAt` keeps position and scale and only touches the rotation. The two degenerate cases
(looking at itself, or an `up` parallel to the view direction) have no answer: a warning
goes to the Log Console and the rotation **stays as it was**, instead of installing a
matrix with `NaN` that would drag the children along.

With a parent, `GetWorldPosition` and `GetPosition` are not the same: the second is local.
`SetWorldPosition` on an object whose parent has scale 0 cannot be solved (singular
matrix) and is also ignored with a warning.

### Moving through Transform does NOT collide

An object moved with `SetPosition`/`Translate` **goes through walls**, with or without a
Rigidbody. It is not a bug: moving the transform is a teleport, and PhysX does not resolve
collisions on a teleport. With a kinematic Rigidbody it goes to `setKinematicTarget` (a
kinematic body pushes dynamic ones, but nothing stops it) and with a dynamic Rigidbody it
goes to `setGlobalPose`, which leaves it wherever you say even if it ends up overlapping.
It is the same rule as `transform.position` in Unity.

For an object to really **collide** it has to be moved by physics: let it fall with
gravity, or push it with the Rigidbody's `AddForce`/`AddImpulse`/`velocity`.

**Triggers do work** when moving through Transform: they detect overlap, which needs no
collision resolution. That is why an object can fire a zone's `OnTriggerEnter` and still
go through a solid wall.

## Scene

| Method | Description |
| --- | --- |
| `Scene.Find(name)` | First GameObject with that name (the root excluded), or `nil` |
| `Scene.CreateGameObject(name, parent?)` | Creates a new GameObject, optionally a child of `parent` |
| `Scene.Destroy(entity)` | Queues destruction (processed at the end of the frame). Internal alias of `DestroyGameObject` |
| `Scene.Instantiate(entity, parent?)` | Clones a GameObject (subtree, components and scripts included); `Awake` is called immediately, `Start` on the next lifecycle update |

## Physics

Queries against the physics scene, and the collision layer matrix. There is only a
physics scene in Play: outside Play `Raycast` returns `nil` and `RaycastHit` returns
`false`.

| Method | Description |
| --- | --- |
| `Physics.Raycast(origin, direction, maxDistance, options)` | Table with the hit, or `nil` if nothing is hit |
| `Physics.RaycastHit(origin, direction, maxDistance, options)` | `true` / `false`; it does not build the hit table |
| `Physics.RaycastAll(origin, direction, maxDistance, options)` | **All** the ray's hits, in a 1-based array sorted by ascending `distance` |
| `Physics.SphereCast(origin, direction, radius, maxDistance, options)` | One hit, with the same shape `Raycast` returns, or `nil` |
| `Physics.OverlapSphere(center, radius, options)` | 1-based array of `Entity` values inside the sphere |
| `Physics.OverlapBox(center, halfExtents, rotation, options)` | 1-based array of `Entity` values inside the box |
| `Physics.SetLayerCollision(a, b, enabled)` | Turns the layer pair on/off, **in both directions** |
| `Physics.GetLayerCollision(a, b)` | `true` if layers `a` and `b` collide |

`origin` and `direction` are `Vec3`. `direction` is normalized inside, so it does not
need to be a unit vector; with length 0 the call returns `nil` without querying physics.
`maxDistance` is optional: missing or `<= 0` uses the default of **1000**.

`options` is an optional table, and all of its fields are optional:

| Field | Type | Default | What it does |
| --- | --- | --- | --- |
| `hitTriggers` | bool | `false` | Whether colliders with *Is Trigger* count as a hit |
| `static` | bool | `true` | Query colliders without a Rigidbody (static actors) |
| `dynamic` | bool | `true` | Query colliders with a Rigidbody (dynamic actors) |
| `ignore` | Entity | — | GameObject to ignore, so a script does not hit itself |

With `static = false` and `dynamic = false` there is nothing left to query: it returns
`nil` (or `false`) without touching physics.

The table `Raycast` returns has exactly these fields:

| Field | Type | Description |
| --- | --- | --- |
| `entity` | Entity | GameObject hit. `nil` if the collider does not hang from any |
| `point` | Vec3 | Hit point, in world coordinates |
| `normal` | Vec3 | Surface normal at the hit point |
| `distance` | number | Distance from `origin` to the hit |

An argument of the wrong type does not bring down the script: the call returns `nil` (or
`false`) and leaves a warning in the Log.

```lua
Shot = {
    range = 50
}

function Shot:Update()
    if not Input.IsKeyPressed(Key.Space) then return end

    local origin = self.entity:GetTransform():GetWorldPosition()
    -- Forward in world space (+Z). To shoot in the direction the object faces,
    -- rotate this Vec3 by its rotation.
    local hit = Physics.Raycast(origin, Vec3(0, 0, 1), self.range,
                                { ignore = self.entity })

    if hit then
        local who = hit.entity and hit.entity.name or "something without a GameObject"
        Log.Info("Hit " .. who .. " at " .. hit.distance .. " units")
    else
        Log.Info("Nothing ahead")
    end
end
```

### RaycastAll — every hit

Instead of stopping at the first one, it collects **every** collider along the ray and
returns a 1-based array of hit tables, each with exactly the same fields as
`Physics.Raycast`, **sorted by ascending `distance`**. It accepts the same `options`.

It always returns a table: with no hits (or outside Play, or with bad arguments, which also
leave a warning in the Log) it returns an **empty** table, never `nil`, so `#hits` and
`ipairs` are always safe. The buffer holds **64** hits per call; if the ray crosses more,
the extra ones are lost (PhysX truncates arbitrarily, so what gets dropped is **not**
necessarily the farthest) and a `[Lua][WARN]` shows up in the Log Console.

```lua
for i, hit in ipairs(Physics.RaycastAll(Vec3(0,2,0), Vec3(0,0,1), 100)) do
    Log.Info(i .. ": " .. hit.entity.name .. " @ " .. hit.distance)  -- nearest first
end
```

### SphereCast and the Overlaps

All three take the **same `options` table** as the raycasts (`hitTriggers`, `static`,
`dynamic`, `ignore`) and, like them, do nothing outside Play.

`SphereCast` is the raycast "with thickness": a sphere of `radius` starts centered at
`origin` and sweeps along `direction`, so it catches what a zero-width ray misses; it is
the usual way to move a character without it slipping through corners. If the sphere
already overlaps something at `origin`, PhysX reports `distance = 0` and
`point`/`normal` mean nothing.

The two `Overlap*` functions answer "what is inside this volume **right now**", so they
return the list of `Entity` values directly, not hit tables: an overlap has no point, no
normal and no distance. Each entity comes out **only once** even if several of its shapes
overlap, actors that do not hang from any GameObject are skipped, and the order is
PhysX's, unsorted. In `OverlapBox`, `rotation` is an **optional** `Vec3` of Euler degrees
(same convention as `Transform:SetRotation`) and is told apart from `options` by its type,
so `Physics.OverlapBox(c, h, { hitTriggers = true })` works without a rotation. Both cap
at **64** overlaps per call and warn with a `[Lua][WARN]` when full, like `RaycastAll`.

```lua
-- is there ground ahead before jumping?
local ground = Physics.SphereCast(self.entity:GetTransform():GetWorldPosition(),
                                  Vec3(0,-1,0), 30, 200)
if ground then Log.Info("ground at " .. ground.distance) end

-- everything inside the explosion radius
for _, e in ipairs(Physics.OverlapSphere(Vec3(0,0,0), 250, { hitTriggers = true })) do
    Log.Info("hit: " .. e.name)
end
```

### Collision layers

`col.layer` is the collider's **collision layer**, an index into the project's layer list.
Layer 0 is `"Default"` and always exists; the rest are **created on demand** from
**View → Collision Layers** (`Add Layer`, rename inline, `x` to delete, with a
confirmation because deleting cannot be undone). Deleting a layer **compacts** the list:
the colliders that used it fall back to layer 0, the layers above shift down one index and
the matrix loses that row and column. So a script that hardcodes layer indices points at a
different layer after a deletion. The cap is **32** layers.

Which layers really collide is decided by a global, **symmetric** matrix:
`Physics.SetLayerCollision(a, b, enabled)` sets the pair in both directions (setting
`(a,b)` also sets `(b,a)`), and `Physics.GetLayerCollision(a, b)` queries it.

The matrix starts **all `true`**, so a project that does not touch it behaves exactly as
before layers existed. A filtered pair produces neither contacts nor `OnTrigger*` events
(the check comes before the filter shader's trigger branch), and both the collider's
`layer` and the matrix take effect **mid-game**: the PhysX filter data of every live shape
is rewritten on the spot.

An index outside `0..31` **raises a Lua error**; it is not clamped, because a silent clamp
would leave the script filtering by a layer it never asked for. Lua accepts the full
`0..31` range the core supports, even layers not yet created in the editor; the Properties
dropdown only offers the ones that exist.

Outside Play there is no PhysX scene, so `SetLayerCollision` does nothing and
`GetLayerCollision` answers `true`, which is the default matrix. The layer names and the
matrix are saved per project in the `settings` section of `project.json`; the collider's
`layer` is runtime/editor state and is **not** serialized with the scene.

## DonTopo — changing scenes at runtime

| Method | Description |
| --- | --- |
| `DonTopo.loadScene(path)` | Requests loading the scene at `path` (a Save Scene file: `version: 1` + `root`). Returns `true` if the request was queued, `false` if the path is empty, the file does not exist, the JSON does not parse or the structure is not a v1 scene (the reason goes to the Log) |

The load **does not happen in the call**: the binding only leaves the request in a mailbox
and the scene's owner runs it on the next frame, outside the script tick. Loading in the
middle of an `Update` would destroy the GameObject that is running that very script.
Practical consequences:

- After the call, the old scene **dies entirely**, your script included. Treat it as the
  last useful line: do not touch `self` or keep references afterwards.
- The `bool` is the result of the **validation**, not of the load. The outcome of the load
  arrives a frame later and shows in the Log (`Scene loaded: ...` /
  `Error loading scene: ...`).
- If a frame leaves several requests, **the last one wins** and the rest are discarded.
- Only in **Play Mode**. In Edit Mode it is ignored with a warning in the Log Console.
- The path is relative to the working directory (the project root in the editor; the
  executable's folder in the exported game, which sets its CWD there).

```lua
function test:Update(dt)
    if Input.IsKeyPressed(Key.R) then
        if not DonTopo.loadScene("Scenes/Empty.json") then
            Log.Error("Error loading scene")
        end
    end
end
```

Mind the capitalization: the table is `DonTopo`. Writing `Dontopo` gives
`attempt to index a nil value` and the component is left with `hasError`, that is, without
receiving more callbacks until a hot reload or Stop.

## Globals

| Function | Description |
| --- | --- |
| `DestroyGameObject(entity)` | Destroys the GameObject and its whole subtree during Play: calls `OnDestroy` on its scripts, frees the GPU meshes and releases colliders/audio (it leaves every manager). Deferred to the end of the frame, so calling it inside `Update` is safe. `entity` can be `self.entity` (self-destruction) or another entity. Lua error if the entity was already destroyed |

## Colliders — BoxCollider / SphereCollider / CapsuleCollider / PlaneCollider

Obtained through `entity:GetComponent("...Collider")`. All of them raise a Lua error if
the component no longer exists on the GameObject.

The **shape** goes through methods, because Lua has no vector type:

| Component | Shape methods |
| --- | --- |
| `BoxCollider` | `GetHalfExtents/SetHalfExtents(Vec3)`, `GetCenter/SetCenter(Vec3)` |
| `SphereCollider` | `GetRadius/SetRadius(float)`, `GetCenter/SetCenter(Vec3)` |
| `CapsuleCollider` | `GetRadius/SetRadius(float)`, `GetHalfHeight/SetHalfHeight(float)`, `GetCenter/SetCenter(Vec3)` |
| `PlaneCollider` | `GetCenter/SetCenter(Vec3)` |

All **four** also carry these five properties (scalars, so they are fields, not methods):

| Property | Description |
| --- | --- |
| `staticFriction` | Static friction. Each collider has its **own** PhysX material, so two objects in the same scene can slide differently |
| `dynamicFriction` | Dynamic friction |
| `bounciness` | Restitution |
| `isTrigger` | `true` = overlaps without colliding and fires the `OnTrigger*` callbacks. The write goes through the PhysicsManager, not the bare collider, so the `OnTriggerEnter`/`Stay`/`Exit` bookkeeping does not get out of step; outside Play there is no live PhysX scene and the write silently does nothing |
| `layer` | Collision layer, `0..31` (see [Collision layers](#collision-layers)) |

```lua
local col = self.entity:GetComponent("BoxCollider")
col.staticFriction  = 0.6
col.dynamicFriction = 0.4
col.bounciness      = 0.9
col.isTrigger       = false
col.layer           = 3
```

**Gravity and dynamics do not live here**: they belong to the `Rigidbody`. A collider
without a Rigidbody is a static actor.

## Rigidbody

`entity:GetComponent("Rigidbody")`. The same fields the Properties panel edits.

| Property | Description |
| --- | --- |
| `mass` | Mass |
| `useGravity` | Whether the scene's gravity affects it |
| `isKinematic` | Kinematic: you move it, physics does not push it |
| `drag` / `angularDrag` | Linear and angular damping |
| `constraints` | **Bitmask** of frozen axes (see `RigidbodyConstraints`) |
| `ccd` | Continuous collision detection |
| `interpolate` | Visual smoothing between fixed steps |
| `velocity` / `angularVelocity` | `Vec3`, read and write |

| Method | Description |
| --- | --- |
| `rb:AddForce(x, y, z [, mode])` | Three separate floats, **not** a `Vec3`. Optional `mode`, from `ForceMode` |
| `rb:AddTorque(x, y, z [, mode])` | Same |
| `rb:AddImpulse(x, y, z)` | Instantaneous, mass-dependent impulse (equivalent to `AddForce` with `ForceMode.Impulse`) |

`RigidbodyConstraints` is a table of integer constants with `None`, `FreezePositionX/Y/Z`
and `FreezeRotationX/Y/Z`, combined with Lua 5.4's bitwise OR. Bits outside those six are
**masked out** instead of raising, so an extra OR never brings down the script.

`ForceMode` is the optional fourth argument of `AddForce`/`AddTorque`:

| Mode | What it does |
| --- | --- |
| `ForceMode.Force` | Continuous, depends on mass and dt. **It is the default**, so three-argument calls behave exactly as always |
| `ForceMode.Acceleration` | Continuous, ignores mass |
| `ForceMode.Impulse` | Instantaneous, depends on mass; the same as `AddImpulse` |
| `ForceMode.VelocityChange` | Instantaneous, ignores mass |

A mode outside those four is reported in the Log Console and **the force is discarded**,
instead of raising: a miscomputed index should not bring down the game. Non-finite values
(a `0/0` in a script) are rejected the same way, with their warning.

### ccd and interpolate

Two booleans, **independent** of each other and `false` by default, so no existing scene
changes behavior.

`ccd` turns on continuous collision detection: PhysX sweeps the body's path within the
fixed step instead of testing only the start and end poses, which is what keeps a fast
projectile from slipping through thin geometry. It costs CPU, and **PhysX does not support
it on kinematic bodies**: setting it there keeps your intent, but the flag only reaches the
actor while the body is not kinematic.

`interpolate` smooths the **visible** pose between fixed steps (the render runs one physics
step behind). It does not change the simulation at all, so raycasts, overlaps and triggers
keep seeing the real pose.

```lua
function Bullet:Start()
    local rb = self.entity:GetComponent("Rigidbody")
    rb.ccd = true             -- fast projectile: keep it from going through the wall
    rb.interpolate = true     -- and make it look smooth between fixed steps
end
```

```lua
-- Scripts/Crate.lua
Crate = {}

function Crate:Start()
    local rb = self.entity:GetComponent("Rigidbody")
    rb.mass       = 3.0
    rb.useGravity = true
    rb.drag       = 0.1
    rb.constraints = RigidbodyConstraints.FreezePositionY | RigidbodyConstraints.FreezeRotationX

    -- layers 3 and 7 stop colliding, across the whole scene and in both directions
    Physics.SetLayerCollision(3, 7, false)
end

function Crate:Update(dt)
    local rb = self.entity:GetComponent("Rigidbody")
    rb:AddForce(0, 500 * dt, 0)
    rb:AddForce(0, 8, 0, ForceMode.VelocityChange)   -- instant jump, regardless of mass
end
```

## AudioClip

The track is assigned from the editor (Properties → Audio → Browse or drag-and-drop of an
asset) or through `entity:AddComponent("AudioClip", path)`.

| Method | Description |
| --- | --- |
| `clip:Play()` / `clip:Stop()` | `Play()` plays at the GameObject's current world position: it **restarts** the clip and cuts the previous voice of the same clip, like Unity's `AudioSource.Play()`. `Stop()` discards the playback position |
| `clip:PlayOneShot()` | **Overlaps** instead of cutting: two footsteps or two shots in a row no longer step on each other. The voice it fires is out of reach afterwards: `Stop()`, `SetVolume()` and `IsPlaying()` do not see it, and it does not follow the object. Short clips only, never looping |
| `clip:Pause()` / `clip:Resume()` | Keep the playback position, unlike `Stop()` |
| `clip:IsPlaying()` / `clip:IsPaused()` | A **paused voice still counts as playing**, as in FMOD and Unity: `IsPaused()` is what tells them apart |
| `clip:SetVolume(v)` / `GetVolume()` | Clip volume, clamped to `[0, 1]`. It MULTIPLIES with the bus and master volumes, it does not replace them. Safe to call in `Update`: it only writes to the channel |
| `clip:SetPitch(p)` / `GetPitch()` | Pitch, clamped to `[0.5, 2]`. `2.0` is one octave up and double speed. For FMOD 0 is not "silence", hence the floor. Safe in `Update` |
| `clip:SetLoop(b)` / `GetLoop()` | **Reloads the sound** (the loop is baked into the FMOD mode) and cuts whatever was playing. It is configuration, not a per-frame call |
| `clip:SetIs3D(b)` / `GetIs3D()` | Switches between 2D and 3D. Same warning as `SetLoop`: it reloads the sound |
| `clip:SetMinDistance(d)` / `GetMinDistance()` | 3D attenuation, clamped to `[0.1, 50]`. Closer than this plays at full volume |
| `clip:SetMaxDistance(d)` / `GetMaxDistance()` | Clamped to `[1, 1000]`, never below min. Cheap: no reload |
| `clip:SetPlayOnAwake(b)` / `GetPlayOnAwake()` | Whether entering Play starts the clip by itself |
| `clip:SetBus(name)` / `GetBus()` | Output bus: `"master"`, `"music"` or `"sfx"` (default). Only affects the **next** playback: the group is chosen when the voice starts. An unknown name warns and changes nothing |
| `clip:SetLoadMode(name)` / `GetLoadMode()` | `"sample"` (decompressed into RAM, several voices at once) or `"stream"` (read from disk, minimal memory, but **one voice at a time**). Stream for music, sample for effects. **Reloads the sound** and cuts whatever was playing |
| `clip:SetRolloff(name)` / `GetRolloff()` | Shape of the falloff between min and max: `"inverse"` (default, the most realistic), `"linear"` (exact silence at max) or `"linearSquare"`. **Reloads the sound** |
| `clip:SetSpread(deg)` / `GetSpread()` | Stereo spread of a 3D source, `[0, 360]`. At 0 it is still a point |
| `clip:SetStereoPan(p)` / `GetStereoPan()` | Manual pan `[-1, 1]`, **2D clips only**; in 3D the position decides it |
| `clip:SetDopplerLevel(l)` / `GetDopplerLevel()` | How much the relative velocity bends the pitch, `[0, 5]`. **0 by default**, and it only acts in Play: velocities are not computed in Edit Mode |
| `clip:SetMute(b)` / `GetMute()` | Mutes **without losing the volume**: unmuting brings back the previous one, without the script having to remember it. Unlike `Pause`, this **is** serialized: an object can start muted. A muted clip does not fire `PlayOneShot` either |
| `clip:GetTime()` / `clip:SetTime(sec)` | Playback position in seconds. `GetTime()` returns **-1** when nothing is playing; 0 would mean "at the start of the clip", which is a different answer. `SetTime` moves a playback in progress; it does not start one |
| `clip:GetPath()` | The path of the asset it was loaded from |

`SetVolume`, `SetPitch`, `SetMinDistance` and `SetMaxDistance` reject non-finite values
(a `0/0` in a script) and say so in the Log, instead of letting a `NaN` reach the scene
file.

A 3D clip **follows its GameObject**: the position is pushed to the live voice every
frame, so attenuation and panning follow a moving object.

A scene without an **Audio Listener** still plays its clips: they are heard from the
camera, and the log says so once per Play.

See `Scripts/AudioFade.lua` for a complete fade.

```lua
function Engine:Start()
    self.clip = self.entity:GetComponent("AudioClip")
    if self.clip then
        self.clip:SetIs3D(true)          -- configuration: outside Update
        self.clip:SetMinDistance(5)
        self.clip:SetMaxDistance(300)
        self.clip:Play()
    end
end

function Engine:Update(dt)
    if not self.clip then return end
    -- The pitch rises with the speed; volume and pitch can be changed every frame
    self.clip:SetPitch(1.0 + self.throttle * 0.8)

    if Input.IsKeyPressed(Key.P) then
        if self.clip:IsPaused() then self.clip:Resume() else self.clip:Pause() end
    end
end
```

```lua
-- Scripts/AudioTest.lua
AudioTest = {}

function AudioTest:Start()
    self.clip = self.entity:GetComponent("AudioClip")
end

function AudioTest:Update(dt)
    if not self.clip then return end
    if Input.IsKeyPressed(Key.Space) then self.clip:Play() end
    if Input.IsKeyPressed(Key.Enter) then self.clip:Stop() end
    if Input.IsKeyPressed(Key.L) then self.clip:SetLoop(not self.clip:GetLoop()) end
end
```

## Audio — global mix

The `Audio` table is what an options menu would drive. It does not hang from any
GameObject.

| Function | Description |
| --- | --- |
| `Audio.SetBusVolume(name, v)` | `name` is `"master"`, `"music"` or `"sfx"`; `v` is clamped to `[0, 1]`. Master scales the other two |
| `Audio.GetBusVolume(name)` | Returns 1.0 when there is no audio device, so a silent machine does not read as "volume at zero" |
| `Audio.PlayClipAtPoint(path, x, y, z [, volume, pitch, bus])` | A 3D one-shot at a world position, **with no GameObject involved**: for an impact or an explosion whose emitter dies that same frame. The sound stays cached after the first use |
| `Audio.Preload(path)` | Loads and keeps a clip without playing it. Worth calling in `Start()`: FMOD loads lazily, so the **first** `PlayClipAtPoint` of a new path is very likely not to be heard. Idempotent |
| `Audio.SetBusEffect(bus, effect, amount)` | Hangs a DSP on a whole bus: `"lowPass"`, `"highPass"`, `"echo"` or `"reverb"`. `amount` goes from `[0, 1]`; the engine maps it to each effect's real units, so scripts never touch Hz or ms. Idempotent: calling it every frame adjusts the same DSP instead of stacking copies |
| `Audio.ClearBusEffect(bus [, effect])` | Removes one effect, or **all** of that bus's effects if the second argument is omitted, which is what you want when leaving the water or closing the pause menu |
| `Audio.SetPaused(b)` / `Audio.IsPaused()` | Freezes **everything** that is playing, keeping the positions: what a pause menu wants. It acts on the master group, so it also catches the loose `PlayOneShot` voices, which cannot be reached any other way. Note: the engine has no simulation pause; this silences the audio, it does not stop the scene |

The three volumes are saved in `project.json` and restored when the project opens; the
editor exposes them in **View → Master / Music / SFX Volume**. The effects are
runtime-only and **deliberately not serialized**: they model a temporary game state, not a
property of the scene.

```lua
-- Everything sounds muffled underwater
function Player:OnEnterWater()
    Audio.SetBusEffect("master", "lowPass", 0.15)
    Audio.SetBusEffect("master", "reverb", 0.4)
end

function Player:OnExitWater()
    Audio.ClearBusEffect("master")   -- no second argument: all of them
end
```

## ReverbZone

Ambience spheres: inside one, everything is heard with that reverb. Several per scene are
no problem: FMOD blends the ones that overlap and fades between min and max by itself.
It is added from the inspector (**Add → Reverb Zone**, with its own wireframe gizmo) or
from a script with `entity:AddComponent("ReverbZone")`; it is obtained with
`entity:GetComponent("ReverbZone")`. One per GameObject.

| Method | Description |
| --- | --- |
| `z:SetPreset(name)` / `z:GetPreset()` | One of FMOD's presets: `"cave"`, `"bathroom"`, `"hangar"`, `"underwater"`, `"forest"`… An unknown name warns and keeps the previous one |
| `z:SetMinDistance(d)` / `z:GetMinDistance()` | Full reverb inside min. Clamped to `[0.1, 5000]` |
| `z:SetMaxDistance(d)` / `z:GetMaxDistance()` | It fades out up to max, and beyond that nothing. Clamped to `[1, 10000]` |
| `z:SetEnabled(b)` / `z:GetEnabled()` | Disabled, it stays reserved but silent, so turning it on and off costs nothing |

The position comes from the GameObject's Transform.

## Animator

`entity:GetComponent("Animator")`. Parameters are not properties: they are declared in the
graph (Animator panel) and read and written **by name**. An undeclared name, or one of
another type, is ignored by the setter and returns the neutral value in the getter; it
never throws because of a bad **name**. What does throw is the GameObject having lost its
Animator between the `GetComponent` and the call.

| Method | Description |
| --- | --- |
| `a:SetBool(n, v)` / `a:GetBool(n)` | `bool` parameter |
| `a:SetTrigger(n)` | Arms a `trigger`; the transition that fires consumes it |
| `a:ResetTrigger(n)` | Disarms a `trigger` that has not been consumed yet |
| `a:Play(state, [layer])` | Enters that state now (of layer `layer`, 0 = base by default), without blending and with time at 0 (also if it was already in it). `false` if it does not exist, with a warning in the log |
| `a:CrossFade(state, seconds, [layer])` | Blends into that state (of layer `layer`, 0 = base) over `seconds`; with 0, the same as `Play`. `false` if it does not exist, with a warning |
| `a:SetSpeed(v)` / `a:GetSpeed()` | The Animator's global speed (1 = normal, 0 = frozen; negative is clamped to 0, NaN/Inf is ignored with a warning). Not saved in the scene. The per-state speed is edited in the graph and driven with `SetFloat` on its multiplier parameter |
| `a:GetNormalizedTime([layer])` | Normalized time of the current state (of the layer, 0 = base): 1 = one loop, and it keeps growing when looping (2.5 = two and a half loops). 0 if the clip has no duration |
| `a:SetInt(n, v)` / `a:GetInt(n)` | `int` parameter |
| `a:SetFloat(n, v)` / `a:GetFloat(n)` | `float` parameter (NaN/Inf is ignored with a warning) |
| `a:GetState([layer])` | Name of the layer's active state (0 = base), `""` if the graph is empty or the layer does not exist |
| `a:IsBlending([layer])` | `true` while a cross-fade lasts on the layer (0 = base) |
| `a:SetLayerWeight(layer, weight)` | Weight 0..1 of an upper layer; the base (0) is always 1 and does not change. A layer that does not exist is ignored; NaN/Inf is ignored with a warning |
| `a:GetLayerWeight(layer)` | Weight of the layer; 1 for the base, 0 if it does not exist |
| `a:GetLayerCount()` | Number of layers, base included |
| `a:SetIkWeight(name, weight)` | Weight 0..1 of an IK constraint. A name that does not exist is ignored; NaN/Inf is ignored with a warning |
| `a:GetIkWeight(name)` | Weight of the constraint; 0 if it does not exist |
| `a:SetIkTarget(name, entity)` | Target of the IK. With `nil` it is removed, and the constraint stops applying |
| `a:SetIkPole(name, entity)` | Pole of a two-bone IK: where the elbow or knee points. `nil` removes it |
| `a:GetIkCount()` | Number of the Animator's IK constraints |
| `a:GetBlendWeight()` | 0 = only the state fading out, 1 = only the new one. 1 if there is no blend |
| `a:GetPreviousState()` | Name of the state fading out, `""` if there is no blend |
| `a:GetPoseWeight()` | The weight that really goes to the GPU: the cross-fade's if there is one, otherwise the parameter blend's, and 1 if there is no blend |

### Cross-fade

Each transition has its **blend duration in seconds**, edited in the Animator panel:
right-click the link → `cross-fade (s)`. With 0 the transition is an instant cut, which is
the long-standing behavior and the one scenes saved before the field existed carry.

During the blend both states keep animating, each with its own `ticksPerSecond` and its
own loop, and the pose that reaches the GPU is the interpolation of the two. If a second
transition fires with a blend still in flight, the previous one is cut: only two clips are
in play at a time.

The four accessors above are **read-only**: the duration is authored in the graph, like a
transition's conditions.

### Two-clip blend by parameter

A state can carry **a second clip** and blend it with its own according to a `float`
parameter: the typical walk/run driven by speed. It is set up on the node in the Animator
panel: `blend` (the second clip), `by` (the float parameter) and `min` / `max`, the range
of the parameter remapped to weight 0..1. Outside that range the weight is clamped, it does
not extrapolate.

From Lua **it is driven with `SetFloat`** on that parameter; the resulting weight is read
with `GetPoseWeight()`.

Both clips are sampled at the **same normalized phase**, not at the same absolute time: a
40-tick walk and a 100-tick run would drift out of phase and the legs would skate.

Two limits worth knowing, because only two clips fit in the push constant:

- **A cross-fade in flight wins over the state's blend.** While the transition lasts each
  side contributes its primary clip; the second clip comes back in when the blend ends.
- A `blend` whose clip does not exist in the model, or an undeclared `by`, leave the state
  as a normal one (a single clip) instead of blending against garbage.

```lua
Locomotion = {}

function Locomotion:Update(dt)
    local a = self.entity:GetComponent("Animator")
    if not a then return end
    local rb = self.entity:GetComponent("Rigidbody")
    if not rb then return end
    -- The horizontal speed, by hand: Length() would include the vertical component
    local v = rb.velocity
    -- The "Locomotion" state blends Walk and Run with blendMin 1.5 / blendMax 6.5
    a:SetFloat("speed", math.sqrt(v.x * v.x + v.z * v.z))
end
```

```lua
Fade = {}

function Fade:Update(dt)
    local a = self.entity:GetComponent("Animator")
    if not a then return end
    a:SetBool("running", Input.IsKeyDown(Key.W))
    -- Mute the footsteps while the character is still entering "Run"
    if a:IsBlending() and a:GetBlendWeight() < 0.5 then return end
end
```

## UI — Canvas / Button / Text / ProgressBar / Layout / Panel / Image / Slider / Checkbox / Toggle / Scrollbar / InputField / Dropdown / ScrollView

All fourteen are obtained with `entity:GetCanvas()`, `entity:GetPanel()`,
`entity:GetImage()`, `entity:GetText()`, `entity:GetButton()`, `entity:GetSlider()`,
`entity:GetCheckbox()`, `entity:GetToggle()`, `entity:GetScrollbar()`,
`entity:GetProgressBar()`, `entity:GetInputField()`, `entity:GetDropdown()`,
`entity:GetScrollView()` and `entity:GetLayout()` (or `GetComponent("Button")`, etc.),
and the matching `Add*`/`Remove*` create and remove them. They return `nil` if the
component is not there; the wrapper resolves the component **on every access**, so using it
after removing it gives a Lua error, not freed memory.

**Scalars, strings, booleans and enums are properties** (`b.text = "Play"`).
**Vectors are methods** (`b:SetSize(200, 48)`, `local w, h = b:GetSize()`), because Lua only
has `Vec3`: the getters return 2 or 4 values. A NaN/Inf value is ignored with a warning in
the Log, as in `Transform.SetPosition`.

Enums travel as tables of integer constants:

| Table | Values |
| --- | --- |
| `UiScaleMode` | `ConstantPixelSize`, `ScaleWithScreenSize`, `ConstantPhysicalSize` |
| `UiScreenMatch` | `MatchWidthOrHeight`, `Expand`, `Shrink` |
| `UiCanvasRenderMode` | `ScreenSpace`, `World` |
| `UiBillboard` | `None`, `YawOnly`, `Full` |
| `UiTextAlign` | `Left`, `Center`, `Right`, `Justify` |
| `UiTextVAlign` | `Top`, `Middle`, `Bottom` |
| `UiTextOverflow` | `Overflow`, `Clip`, `Ellipsis` |
| `UiProgressFillDirection` | `LeftToRight`, `RightToLeft`, `BottomToTop`, `TopToBottom` |
| `UiButtonTransition` | `ColorTint`, `SpriteSwap`, `Animation` |
| `UiButtonState` | `Normal`, `Hover`, `Pressed`, `Disabled`, `Selected` |
| `UiImageMode` | `Normal`, `Tiled`, `Sliced`, `Filled` |
| `UiFillDirection` | `Horizontal`, `Vertical` |
| `UiFillOrigin` | `Start`, `End` |
| `UiSliderDirection` | `LeftToRight`, `RightToLeft`, `BottomToTop`, `TopToBottom` |
| `UiScrollbarDirection` | `LeftToRight`, `RightToLeft`, `TopToBottom`, `BottomToTop` |
| `UiInputContentType` | `Standard`, `IntegerNumber`, `DecimalNumber`, `Alphanumeric`, `Password` |
| `UiLayoutMode` | `None`, `Horizontal`, `Vertical`, `Grid` |
| `UiCrossAlign` | `Start`, `Center`, `End` |

### Canvas

| Property / Method | Description |
| --- | --- |
| `c.scaleMode` | `UiScaleMode.*` |
| `c.scaleFactor` | Multiplies all three modes |
| `c.screenMatch` | `UiScreenMatch.*` |
| `c.matchWidthOrHeight` | 0 = width, 1 = height (`ScaleWithScreenSize` only) |
| `c.screenDpi` / `c.fallbackDpi` / `c.referenceDpi` | Real DPI (0 = unknown), the one used when it is unknown, and the reference one for `ConstantPhysicalSize` |
| `c.aspectRatio` | 0 = off |
| `c:GetReferenceResolution()` / `c:SetReferenceResolution(w, h)` | Reference resolution |
| `c:GetSafeArea()` / `c:SetSafeArea(l, t, r, b)` | Insets in real pixels |
| `c.renderMode` | `UiCanvasRenderMode.*`. In `World` the canvas is placed IN THE SCENE and `scaleMode`, `screenMatch`, `matchWidthOrHeight`, the three DPI fields, `safeArea` and `aspectRatio` are ignored |
| `c.worldScale` | `World` only. World units per canvas PIXEL |
| `c.billboard` | `World` only. `UiBillboard.*`: `YawOnly` only turns around the vertical, `Full` faces the camera completely |
| `c.depthTest` | `World` only. When `false` it is always drawn on top, through walls |

**Several screen canvases at once.** A scene can have as many as it wants (a HUD and a
pause menu on top, for example) and **all** of them receive input. When two overlap the
input has to be shared out, and this is how:

- **The mouse goes to ONE only:** the **topmost** one that has something under the cursor.
  Top = the last one drawn, that is, the one lowest in the scene hierarchy. It is the same
  criterion the editor viewport's click uses to select, so what is on top is what gets
  selected.
- **The ones below do NOT get stuck:** they receive the mouse *outside*, so they drop the
  hover, emit their `MouseExit` and their buttons go back to `Normal`. They keep animating
  and fading colors normally.
- **A drag is not cut.** While a mouse button stays down, the canvas where it started keeps
  the pointer even if the cursor passes over another one. That is what lets you drag a
  slider to the edge of the screen.
- **Keyboard and gamepad follow FOCUS, not the cursor:** they go to the canvas that has
  focus, so typing into a field keeps arriving even if the mouse wanders over another
  canvas. Focus moves on **click**, and only one canvas has it at a time: as soon as
  another takes it, the previous one lets go. Tab and the arrows wrap around *inside* the
  canvas that has it and **do not jump** to the next canvas.
- **Changing a canvas's `renderMode` releases its input.** Setting it to `World` takes it
  out of the sharing (a world canvas cannot be clicked), so if it happens mid-press its
  hover, press and focus are released (with their `MouseExit` and `Blur`), and it is not
  left with an orphan capture that, on coming back, would steal the mouse from the canvas
  on top.

**Two limitations of `World` mode.** They are not bugs: they come from where the canvas is
recorded, and it is worth having them written down before tripping over them.

- **A world canvas can NOT be clicked**, neither in the game nor in the editor viewport.
  The UI hit test works in screen pixels and a world canvas is *projected*: it can come out
  rotated, in perspective or cut by the edge. It is selected from the Hierarchy, like a
  `Layout`'s container. When selected, the editor draws its plane's quadrilateral over the
  viewport (with a mark on its (0,0) corner), which shows where it is and how it is tilted;
  if any of its four corners is behind the camera, nothing is drawn. The widgets inside are
  selected the same way, from the Hierarchy, and their gizmo is also projected onto the
  sign, tilted with it and with the pivot's X/Y axes in the orientation they have there.
- **`clipChildren` does NOT clip in a world canvas.** The scissor is in canvas pixels and
  maps 1:1 to the framebuffer; with the canvas projected there is no axis-aligned rectangle
  to represent it, so world canvases are recorded with the scissor covering the whole
  framebuffer. What is respected is a clip that has already become empty: that node emits
  nothing.

### Button

| Property / Method | Description |
| --- | --- |
| `b.visible` | Drawn or not |
| `b.atlasPath` / `b.sprite` | Atlas PNG and name of the base sprite (empty = flat color) |
| `b.interactable` | `false` forces the `Disabled` state |
| `b.selected` | Sustained `Selected` state |
| `b.transition` | `UiButtonTransition.*` |
| `b.normalSprite` / `hoverSprite` / `pressedSprite` / `disabledSprite` / `selectedSprite` | Names within the SAME atlas |
| `b.fadeDuration` | Seconds of the `Animation` fade |
| `b.text` / `b.fontPath` / `b.fontSize` / `b.textAlign` / `b.textVAlign` | Label (a `Text` child the sync builds); empty `fontPath` = default font. `textAlign` is `UiTextAlign.*` and `textVAlign` is `UiTextVAlign.*` |
| `b.sprite`, `b.normalSprite`, … | **Name of a sub-rect of the atlas**, not a path. They are defined by the editor's Sprite Editor and live in `<atlas>.sprites.json`; a name that is not in that file draws the whole image |
| `b:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect, in pixels and normalized anchors |
| `b:GetColor/SetColor(r,g,b,a)` | Base color |
| `b:GetNormalColor/SetNormalColor`, `GetHoverColor/SetHoverColor`, `GetPressedColor/SetPressedColor`, `GetDisabledColor/SetDisabledColor`, `GetSelectedColor/SetSelectedColor` | The 5 state colors |
| `b:GetTextColor/SetTextColor(r,g,b,a)` | Label color |
| `b:GetState()` | `UiButtonState.*` resolved by the last input (read-only) |
| `b:OnClick(fn)` / `b:OnDoubleClick(fn)` | Registers the callback; passing `nil` removes it |

### Text

| Property / Method | Description |
| --- | --- |
| `t.visible`, `t.text`, `t.fontPath`, `t.fontSize` | The basics; empty `fontPath` = default font |
| `t.outlineWidth` | 0 = no outline |
| `t.align` / `t.vAlign` / `t.overflow` / `t.wordWrap` | `UiTextAlign.*` (horizontal), `UiTextVAlign.*` (vertical), `UiTextOverflow.*`, word wrap |
| `t.boldStrength` / `t.italicSkew` | Simulated bold and italic |
| `t:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `t:GetColor/SetColor(r,g,b,a)` | Glyph fill |
| `t:GetOutlineColor/SetOutlineColor(r,g,b,a)` | Outline |
| `t:GetShadowOffset/SetShadowOffset(x,y)`, `t:GetShadowColor/SetShadowColor(r,g,b,a)` | Shadow (offset 0,0 = no shadow) |

A font's atlas is baked with ASCII, the complete Latin-1 supplement
(`á é í ó ú ü ñ Ñ ¿ ¡ « » º ª`), `…` and `€`. A character outside that (or one the chosen
font does not carry) is not drawn **and leaves no gap**: if a letter is missing, first
check whether the TTF has it.

### ProgressBar

| Property / Method | Description |
| --- | --- |
| `p.visible` | Drawn or not |
| `p.value` / `p.minValue` / `p.maxValue` | No clamp: drawing normalizes the range |
| `p.fillDirection` | `UiProgressFillDirection.*` |
| `p.atlasPath` / `p.backgroundPath` / `p.fillPath` | Shared image (fallback), background's and fill's |
| `p:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `p:GetColor/SetColor(r,g,b,a)` / `p:GetFillColor/SetFillColor(r,g,b,a)` | Background and fill color |
| `p:GetNormalizedValue()` | The already-clamped `0..1` that drawing uses (degenerate range = 0) |

### Layout

Places the GameObject's **children**. It draws nothing: without another UI component on the
object, it builds a container of its own (a rect that groups and clips); with a `Button`,
`Text` or `ProgressBar` next to it, it writes onto that one's node and **that one drives the
rect** (`position`, `size`, anchors and pivot from here are not read).

It does not receive clicks either: a group that paints nothing cannot swallow the mouse from
what is behind it. In the editor it is selected from the Hierarchy.

| Property / Method | Description |
| --- | --- |
| `l.mode` | `UiLayoutMode.None` / `Horizontal` / `Vertical` / `Grid`. `None` = only groups and clips |
| `l.crossAlign` | `UiCrossAlign.Start` / `Center` / `End`. CROSS axis; `Grid` does not use it |
| `l.paddingLeft` / `paddingRight` / `paddingTop` / `paddingBottom` | The container's inner margin |
| `l.columns` | `Grid` only. `0` = as many as fit in the width |
| `l.fitWidth` / `l.fitHeight` | Content size fitter: that `Size` axis becomes the extent of the children + padding |
| `l.ignoreLayout` | This object anchors on its own and does NOT take a slot in its parent's layout |
| `l.clipChildren` | Clips the descendants against this rect (**intersected** with the parent's clip) |
| `l.visible` | Resolved or not (an invisible container hides its subtree) |
| `l:GetSpacing/SetSpacing(x,y)` | Gap between cells: `x` between columns, `y` between rows |
| `l:GetCellSize/SetCellSize(x,y)` | `Grid` only: the cell, imposed on every child |
| `l:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Container rect (only if it is its own) |

```lua
local menu = self.entity:GetLayout() or self.entity:AddLayout()
menu.mode = UiLayoutMode.Vertical
menu.paddingLeft, menu.paddingTop = 12, 12
menu:SetSpacing(0, 8)
menu:SetSize(240, 300)
```

### Panel

The background rectangle: without an atlas it is a flat color quad, with an atlas the sprite
stretched to the rect. It has no fields of its own beyond the rect, the color and the sprite;
the core's `Panel` does not have them either.

| Property / Method | Description |
| --- | --- |
| `p.visible` | Drawn or not |
| `p.raycastTarget` | When `false` it lets the mouse through to whatever is behind it. A full-screen background with this `true` swallows **all** clicks, and nothing on screen gives it away |
| `p.atlasPath` | The panel's image. Empty = flat color |
| `p.sprite` | Name of the sub-rect within the atlas. Empty = the whole image |
| `p:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `p:GetColor/SetColor(r,g,b,a)` | Color (tint if there is a sprite) |

### Image

The sprite with its four ways of filling the rect. All four are resolved on the CPU inside
the batcher (N quads from the same atlas and the same scissor): not a single extra shader or
pipeline.

| Property / Method | Description |
| --- | --- |
| `i.visible` / `i.raycastTarget` | Same as in the `Panel` |
| `i.atlasPath` / `i.sprite` | Image and sub-rect name |
| `i.mode` | `UiImageMode.Normal` / `Tiled` / `Sliced` / `Filled` |
| `i.borderLeft` / `borderRight` / `borderTop` / `borderBottom` | `Sliced` only. Pixels **of the sprite**, not of the rect: scaling the element does not move them |
| `i.fillCenter` | `Sliced` only. When `false` you get 8 quads instead of 9: what a frame that shows what is behind it wants |
| `i.maxTiles` | `Tiled` only. Hard quad cap; past the cap the element is drawn as `Normal` instead of blowing up the vertex buffer |
| `i.fillDirection` | `Filled` only. `UiFillDirection.Horizontal` / `Vertical` |
| `i.fillOrigin` | `Filled` only. `UiFillOrigin.Start` / `End`. `Start` is left in `Horizontal` and top in `Vertical` |
| `i.fillAmount` | `Filled` only. `0..1`; at `0` not a single quad is emitted |
| `i:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `i:GetColor/SetColor(r,g,b,a)` | Sprite tint (multiplied) |

```lua
-- 9-slice frame that does not distort its corners when stretched
local frame = self.entity:GetImage() or self.entity:AddImage()
frame.atlasPath = "assets/ui/frames.png"
frame.sprite = "window"
frame.mode = UiImageMode.Sliced
frame.borderLeft, frame.borderRight = 12, 12
frame.borderTop, frame.borderBottom = 12, 12
frame.fillCenter = false
frame:SetSize(400, 260)
```

### Slider / Checkbox / Toggle / Scrollbar — the interactive ones

These four have something the rest do not: **the player moves them**, and what they move is
written **into the component**, not into the canvas node. So reading `s.value` or `c.isOn`
gives the real value without polling anything, it is serialized with the scene and it shows
in the inspector while the game runs.

All four carry `interactable` (when `false` they are drawn the same but cannot be touched)
and `OnValueChanged(fn)`, which calls `fn` with the new value **only when it changes**.
Passing `nil` removes it. As with `Button:OnClick`, the owner of the callback is the
component, so registering once in `Start` is enough: it survives rebuilds of the UI tree.

#### Slider

| Property / Method | Description |
| --- | --- |
| `s.value` / `s.minValue` / `s.maxValue` | The range. No clamp when written by hand; dragging does clamp |
| `s.wholeNumbers` | Rounds the value that is **written**, not just the one drawn |
| `s.direction` | `UiSliderDirection.*` |
| `s.handleSize` | Handle length **along the travel axis**, in px. It is subtracted from the travel so it does not stick out of the ends. At `0` the travel is the whole rect |
| `s.interactable` / `s.visible` | |
| `s.atlasPath` / `s.backgroundSprite` / `s.fillSprite` / `s.handleSprite` | One atlas, three sub-rect names |
| `s:GetColor/SetColor`, `GetFillColor/SetFillColor`, `GetHandleColor/SetHandleColor` | Track, fill and handle |
| `s:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `s:GetNormalizedValue()` | The already-clamped `0..1` (degenerate range = 0) |
| `s:OnValueChanged(fn)` | `fn(newValue)` |

The **whole track** is a click zone, not only the handle: a click jumps the value to where
the cursor is, as in Unity. The drag follows the mouse even when it leaves the rect.

#### Checkbox

| Property / Method | Description |
| --- | --- |
| `c.isOn` | The value. A click toggles it |
| `c.checkPadding` | Px the checkmark is inset into the box on all four sides. One that does not fit leaves the checkmark at zero, never an inverted rect |
| `c.interactable` / `c.visible` | |
| `c.atlasPath` / `c.backgroundSprite` / `c.checkmarkSprite` | |
| `c:GetColor/SetColor` / `c:GetCheckColor/SetCheckColor` | Box and checkmark |
| `c:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `c:OnValueChanged(fn)` | `fn(newValue)` |

**It has no text label**: `Text` is its own component and fits on the same GameObject (they
are sibling nodes), so putting a copy of the text fields here would mean maintaining two.

#### Toggle

It stores the same data as the `Checkbox` (a bool) and is still a different component: what
changes is not the data but the **fields**; the checkbox has padding and a checkmark color,
the switch has two track colors and the knob size.

| Property / Method | Description |
| --- | --- |
| `t.isOn` | The value. A click toggles it |
| `t.knobSize` / `t.knobPadding` | The knob is clamped to what is left between paddings: one bigger than the track would stick out of the edge |
| `t.interactable` / `t.visible` | |
| `t.atlasPath` / `t.backgroundSprite` / `t.knobSprite` | |
| `t:GetOffColor/SetOffColor` / `t:GetOnColor/SetOnColor` | The track **has no color of its own**: it is painted with one or the other depending on the state |
| `t:GetKnobColor/SetKnobColor` | |
| `t:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `t:OnValueChanged(fn)` | `fn(newValue)` |

#### Scrollbar

It looks like the `Slider` but it is not the same: the handle has a **variable** size (the
fraction of the content that is visible) and the value is always in `0..1`; there is no range
of its own because what scrolls interprets it, not the bar.

| Property / Method | Description |
| --- | --- |
| `s.value` | `0..1` |
| `s.handleFraction` | Fraction of the channel taken by the handle. `1` = the content fits entirely and there is nothing to scroll |
| `s.direction` | `UiScrollbarDirection.*` |
| `s.numberOfSteps` | Discrete stops. `0` and `1` = continuous: snapping to a single stop would leave the bar stuck in one place |
| `s.scrollStep` | How much the wheel moves per notch, as a fraction of the travel |
| `s.interactable` / `s.visible` | |
| `s.atlasPath` / `s.backgroundSprite` / `s.handleSprite` | |
| `s:GetColor/SetColor` / `s:GetHandleColor/SetHandleColor` | Channel and handle |
| `s:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `s:SnapValue(v)` | The same snapping to stops that dragging applies |
| `s:OnValueChanged(fn)` | `fn(newValue)` |

The **mouse wheel** over the bar also moves it, and the event is consumed there: if it kept
bubbling, a container wrapping it would scroll at the same time and the content would jump
twice per notch.

```lua
function Options:Start()
    local vol = self.entity:GetSlider()
    vol.minValue, vol.maxValue = 0, 100
    vol.wholeNumbers = true
    vol:OnValueChanged(function(v)
        Log.Info("Volume: " .. v)
    end)
end
```

### InputField / Dropdown / ScrollView

#### InputField

The only widget the **player** types into. For it to exist, the canvas needed something it
did not have: a **character** channel. `UiKey` names physical keys with a meaning of their own
(`Tab`, `Enter`, arrows) and an `a` is not one of them (it comes from the keyboard layout and
dead keys), so the core gained `UiInputState.chars` and `UiElement::onTextInput`. It is canvas
infrastructure, not this component's: anything in the future that receives text (a console, a
chat, a search box) uses the same one.

| Property / Method | Description |
| --- | --- |
| `f.text` | The real text, in UTF-8. With `Password` it is stored **as is**: what changes is what is shown |
| `f.placeholder` | What is shown when the field is empty, with its own color |
| `f.interactable` | When `false` it does not even take focus |
| `f.readOnly` | Takes focus and lets the cursor move, but not change the text |
| `f.characterLimit` | Counts **characters**, not bytes. `0` = no limit |
| `f.contentType` | `UiInputContentType.*`. Filters what can be **typed**, not what is drawn |
| `f.passwordChar` | What it is masked with. Empty falls back to the asterisk |
| `f.fontPath` / `f.fontSize` / `f.align` / `f.padding` | |
| `f.caretWidth` / `f.caretBlinkRate` | Seconds per half cycle; `0` = steady |
| `f.atlasPath` / `f.backgroundSprite` | |
| `f:GetColor/SetColor`, `GetTextColor/SetTextColor`, `GetPlaceholderColor/SetPlaceholderColor`, `GetCaretColor/SetCaretColor` | |
| `f:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `f:GetDisplayText()` | What is **drawn**: the placeholder if it is empty, or the mask for `Password`. Never the password |
| `f:GetCaretPos()` / `f:SetCaretPos(n)` | Cursor position in **characters**, `0` = before the first one. Clamped when written |
| `f:OnValueChanged(fn)` | `fn(newText)`, on every key that changes the text |
| `f:OnEndEdit(fn)` | `fn(text)` when Enter is pressed or focus is lost. That is where a form validates, not on every key |

`Left` and `Right` move the cursor and **consume** the key: otherwise the canvas's directional
navigation would take focus to another widget in the middle of a word. `Up` and `Down` are not
consumed, so you can leave the field with the gamepad.

#### Dropdown

The only one whose subtree **changes shape** with the data: one more option is one more node.
Adding or removing options rebuilds the UI tree; opening and closing does not (the list always
exists and is only switched off).

| Property / Method | Description |
| --- | --- |
| `d.value` | **0-based** index of the selected one, as in C++ and in the inspector |
| `d.isOpen` | Live state. **Not serialized**: a scene cannot open with the list covering the menu |
| `d.itemHeight` / `d.maxVisibleItems` | `0` = all |
| `d.interactable` / `d.visible` | |
| `d.fontPath` / `d.fontSize` / `d.padding` | |
| `d.atlasPath` / `d.backgroundSprite` / `d.arrowSprite` / `d.itemSprite` | |
| `d:GetColor/SetColor`, `GetListColor/SetListColor`, `GetItemColor/SetItemColor`, `GetItemSelectedColor/SetItemSelectedColor`, `GetArrowColor/SetArrowColor`, `GetTextColor/SetTextColor` | |
| `d:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `d:GetOptionCount()` | How many there are |
| `d:GetOption(i)` | Option `i`, with a **1-based** index (the natural one in Lua). Out of range returns an empty string |
| `d:GetSelectedLabel()` | The selected one's text, or empty if the index points to none |
| `d:SetOptions(table)` | Replaces the list. Anything that is not a string is discarded, entry by entry |
| `d:AddOption(s)` / `d:ClearOptions()` | |
| `d:OnValueChanged(fn)` | `fn(index)` (0-based), only when it changes |

**Mind the two indices**: `d.value` is 0-based (it is the component's field) and
`d:GetOption(i)` is 1-based (it is a Lua table). To read the selected one without thinking
about it, use `d:GetSelectedLabel()`.

#### ScrollView

| Property / Method | Description |
| --- | --- |
| `v.horizontal` / `v.vertical` | A disabled axis does not move even if the content is bigger |
| `v.scrollSensitivity` | **Pixels** the wheel moves per notch (not a fraction: a 50-row list and a 5-row one want the same travel per notch) |
| `v.visible` | |
| `v.atlasPath` / `v.backgroundSprite` | |
| `v:GetContentSize/SetContentSize(x,y)` | Size of the scrollable area. It is a **field**, not something measured from the children |
| `v:GetNormalizedPosition/SetNormalizedPosition(x,y)` | `0` = start, `1` = end, per axis |
| `v:GetScrollRange()` | How far it can scroll per axis, in pixels. A disabled axis gives `0` |
| `v:GetContentOffset()` | Where the content is inside the viewport. Always `<= 0` |
| `v:GetPosition/SetPosition`, `GetSize/SetSize`, ... | Rect of the **viewport** |
| `v:OnValueChanged(fn)` | `fn(x, y)` with the normalized position of both axes |

**The GameObject's children hang from the content**, not from the viewport: that is why
scrolling drags them. The viewport clips (`clipChildren`), so whatever sticks out is not drawn.

**It has no reference to a Scrollbar.** Linking them is one line of script; a reference between
scene components would have to be serialized and kept alive through clone, undo and delete.

```lua
function Options:Start()
    local bar  = Scene.Find("SideBar"):GetScrollbar()
    local list = self.entity:GetScrollView()
    bar:OnValueChanged(function(v)
        local x = select(1, list:GetNormalizedPosition())
        list:SetNormalizedPosition(x, v)
    end)

    local name = Scene.Find("NameField"):GetInputField()
    name.contentType = UiInputContentType.Alphanumeric
    name.characterLimit = 16
    name:OnEndEdit(function(t) Log.Info("Player: " .. t) end)
end
```

### Button callbacks: what kills them and what does not

The callback is kept by the **component**, not the canvas node, and the sync hooks it up again
every time it rebuilds the root, which happens when any widget in the scene is added or
removed. So registering once in `Start` is enough: it survives rebuilds.

They are invalidated (they stop firing, with no error or crash) when:

- the script is **hot reloaded**: the code that registered it no longer exists; the reloaded
  script's `Start` hooks it up again,
- **Play stops**: the instances are destroyed,
- the **component is removed** or the GameObject dies.

An error inside the callback is logged in the Log Console (`[Lua][ERROR]
Button.OnClick: ...`) and does not bring down the frame or the rest of the scripts.

### Two confusing things

1. **Setters write to the component, not to the live node.** The sync dumps the component onto
   the canvas tree every frame, so writing to the node directly would be useless. Writing an
   atlas or font path **loads nothing at that moment**: loading is the sync's job.
2. **In the editor the mouse only gets in during Play and with the cursor over the viewport
   image** (as in Unity, a button does not light up while editing). The coordinates are canvas
   pixels from the top-left corner of that image.

```lua
-- Scripts/ButtonDemo.lua
ButtonDemo = {}

function ButtonDemo:Start()
    local b = self.entity:GetButton()
    if b == nil then
        print("this GameObject has no Button")
        return
    end

    b.text = "Play"
    b.transition = UiButtonTransition.Animation
    b:SetSize(220, 48)
    b:SetNormalColor(0.1, 0.5, 0.9, 1)
    b:SetHoverColor(0.2, 0.7, 1.0, 1)

    self.lives = 3
    b:OnClick(function()
        self.lives = self.lives - 1
        local bar = self.entity:GetProgressBar() or self.entity:AddProgressBar()
        bar.minValue, bar.maxValue = 0, 3
        bar.value = self.lives
        print(self.lives .. " left")
    end)
end
```

### Autocomplete in the Script Editor

This whole API is in the Script Editor's identifier list: typing `Canvas.`, `Button:`,
`Text.`, `ProgressBar:`, `Entity:Get`, `Physics.`, `Audio.`, `ReverbZone:`, `Input.`,
`RigidbodyConstraints.`, `ForceMode.` or any of the UI enum tables (`UiScaleMode.`,
`UiScreenMatch.`, `UiCanvasRenderMode.`, `UiBillboard.`, `UiTextAlign.`, `UiTextOverflow.`,
`UiProgressFillDirection.`, `UiLayoutMode.`, `UiCrossAlign.`, `UiImageMode.`,
`UiFillDirection.`, `UiFillOrigin.`, `UiSliderDirection.`, `UiScrollbarDirection.`,
`UiInputContentType.`, `UiTextVAlign.`, `UiButtonTransition.`, `UiButtonState.`) brings up the
suggestions. Properties with `.`, methods with `:`, like the rest of the list.

**You do not need to type the type's name.** The filter also searches by **member** name,
which is what you need when calling through a variable, which is the normal case in real code:

```lua
local t = self.entity:GetTransform()
t:GetPos      -- suggests Transform:GetPosition
```

On accepting, the typed `t:` is kept and only the member is completed: you never end up with a
`t:Transform:GetPosition`. The separator is respected (with `.` only properties show and with
`:` only methods), so the suggestion always compiles.

The order is: first what starts with the whole typed text, then what was found by member; on a
tie, the shortest, and on equal length, alphabetical.

The popup shows each entry's **signature** next to its name (`(pos: Vec3)`, `() -> number`…)
and a one-line description of the selected one. A symbol without an annotated signature still
shows, just without it.

It opens by itself after two typed characters, and also right when typing a `.` or a `:`, which
is when you want to see what is inside the receiver. `Escape` dismisses it until the word
changes.

## Existing examples

`Scripts/Mover.lua` (Input + Transform), `Scripts/Rotator.lua` (continuous rotation),
`Scripts/AudioTest.lua` (AudioClip Play/Stop/Loop), `Scripts/AudioFade.lua` (AudioClip
SetVolume/GetVolume per frame), `Scripts/TriggerProbe.lua` (counts a trigger's entries and
exits), `Scripts/TriggerTest.lua` (destroys itself on Enter), `Scripts/PushMe.lua`
(AddForce/AddImpulse on the Rigidbody, which is the way that **does** collide),
`Scripts/DeleteGameObject.lua` (self-destruction in `Start`), `Scripts/Test.lua` (empty
template).
