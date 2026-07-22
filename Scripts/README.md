# Lua Scripting API — Don Topo Engine

Referencia completa de los métodos disponibles pa scripts Lua. Ver `README.md`
(raíz) pa overview general; este documento detalla cada clase/tabla expuesta
por `ScriptBindings.cpp`.

## Cómo se define un script

Cada archivo `Scripts/<Name>.lua` define una tabla global `<Name>` — su nombre
de clase. Se adjunta a un GameObject vía **Properties → Add → Script**.

```lua
Rotator = {
    speed = 45   -- prop serializable (number/boolean/string), auto-UI en editor
}

function Rotator:Awake() end
function Rotator:Start() end
function Rotator:Update(dt) end
function Rotator:FixedUpdate(dt) end
function Rotator:LateUpdate() end
function Rotator:OnDestroy() end
```

## Lifecycle

| Callback | Cuándo |
| --- | --- |
| `Awake()` | Al crear la instancia (Play Start o `Scene.Instantiate`), antes de cualquier `Start`/`Update` |
| `Start()` | Una vez, antes del primer `Update` |
| `Update(dt)` | Cada frame, en Play Mode |
| `FixedUpdate(dt)` | Paso fijo (`1/60`), acumulador con tope anti spiral-of-death |
| `LateUpdate()` | Cada frame, después de todos los `Update` |
| `OnDestroy()` | Al destruirse el GameObject o quitarse el componente |

Todos son opcionales — solo se llaman los que el script define. Un error en
cualquiera loguea el mensaje y **desactiva ese componente** (deja de recibir
callbacks) hasta hot reload o `Stop`; nunca crashea el motor.

Scripts solo corren en **Play Mode**. `self.entity` (tipo `Entity`, ver abajo)
se inyecta automáticamente en la instancia.

## Props serializables

Cualquier campo `number`/`boolean`/`string` en la tabla de clase se detecta
como prop y aparece en Properties (DragInt pa integers Lua, DragFloat pa
floats). Solo los valores editados en el editor (que difieren del default)
se serializan en la escena.

## Hot reload

Editar un `.lua` cargado mientras el motor corre lo recarga (~1s de polling),
preservando los valores de props ya asignados.

---

## Vec3

Constructor `Vec3.new(x, y, z)` o `Vec3.new()` (cero). Campos `.x/.y/.z`.
Operadores `+`, `-`, `* escalar`, `tostring`.

## Log

| Método | Descripción |
| --- | --- |
| `Log.Info(msg)` | Log normal en Log Console |
| `Log.Warn(msg)` | Log con prefijo `[WARN]` |
| `Log.Error(msg)` | Log con prefijo `[ERROR]` |

`print(...)` nativo de Lua también se redirige al Log Console (mismo destino
que `Log.Info`).

## Input

| Método | Descripción |
| --- | --- |
| `Input.IsKeyDown(key)` | true mientras la tecla está apretada |
| `Input.IsKeyPressed(key)` | true solo en el frame que se apretó |
| `Input.IsKeyReleased(key)` | true solo en el frame que se soltó |
| `Input.IsMouseButtonDown(button)` | true mientras el botón está apretado |

Tablas de constantes: `Key.Space/Enter/Escape/Tab/LeftShift/LeftControl/
Up/Down/Left/Right/A..Z/Num0..Num9`, `MouseButton.Left/Right/Middle`.

## Entity (`self.entity`)

| Método/prop | Descripción |
| --- | --- |
| `entity.name` | Lectura/escritura del nombre del GameObject |
| `entity:IsValid()` | false si la entity fue destruida |
| `entity:GetTransform()` | Devuelve `Transform` |
| `entity:GetParent()` | `Entity` del padre, o `nil` si es raíz |
| `entity:GetChildren()` | Tabla (array 1-based) de `Entity` hijos |
| `entity:GetComponent(name)` | Devuelve el componente si existe, si no `nil`. `name`: `"BoxCollider"`, `"SphereCollider"`, `"CapsuleCollider"`, `"PlaneCollider"`, `"AudioClip"`, o `"Script:<NombreClase>"` pa acceder a la instancia de otro script en el mismo GameObject |
| `entity:AddComponent(name, arg?)` | Añade componente (mismos defaults que el botón Add del editor; colliders mutuamente excluyentes). `AudioClip` requiere `arg` = ruta del asset. `"Script:<Nombre>"` añade el script (Awake/Start se disparan en el siguiente lifecycle update) |
| `entity:RemoveComponent(name)` | Quita el componente (scripts se remueven diferido, al final del frame) |

## Transform

| Método | Descripción |
| --- | --- |
| `t:GetPosition()` / `t:SetPosition(Vec3)` | Posición local |
| `t:GetRotation()` / `t:SetRotation(Vec3)` | Rotación local en euler-grados |
| `t:GetScale()` / `t:SetScale(Vec3)` | Escala local |
| `t:GetWorldPosition()` | Posición mundial (traducción de la world matrix) |
| `t:Translate(Vec3 delta)` | Suma delta a la posición local |
| `t:Rotate(Vec3 deltaEulerGrados)` | Rotación incremental compuesta como quaternion (no se atasca en rotación continua multi-eje) |

## Scene

| Método | Descripción |
| --- | --- |
| `Scene.Find(name)` | Primer GameObject con ese nombre (excluye la raíz), o `nil` |
| `Scene.CreateGameObject(name, parent?)` | Crea un GameObject nuevo, opcionalmente hijo de `parent` |
| `Scene.Destroy(entity)` | Encola destrucción (procesada al final del frame). Alias interno de `DestroyGameObject` |
| `Scene.Instantiate(entity, parent?)` | Clona un GameObject (incl. sub-árbol, componentes, scripts); `Awake` se llama de inmediato, `Start` en el siguiente lifecycle update |

## Globales

| Función | Descripción |
| --- | --- |
| `DestroyGameObject(entity)` | Destruye el GameObject y todo su sub-árbol durante Play: llama `OnDestroy` en sus scripts, libera los meshes de GPU y suelta colliders/audio (sale de todos los managers). Diferido al final del frame — llamarlo dentro de `Update` es seguro. `entity` puede ser `self.entity` (auto-destrucción) u otra entity. Error Lua si la entity ya fue destruida |

## Colliders — BoxCollider / SphereCollider / CapsuleCollider / PlaneCollider

Obtenidos vía `entity:GetComponent("...Collider")`. Todos lanzan error Lua
si el componente ya no existe en el GameObject.

| Componente | Métodos |
| --- | --- |
| `BoxCollider` | `GetUseGravity/SetUseGravity(bool)`, `GetHalfExtents/SetHalfExtents(Vec3)`, `GetCenter/SetCenter(Vec3)`, `IsDynamic()` |
| `SphereCollider` | `GetUseGravity/SetUseGravity(bool)`, `GetRadius/SetRadius(float)`, `GetCenter/SetCenter(Vec3)`, `IsDynamic()` |
| `CapsuleCollider` | `GetUseGravity/SetUseGravity(bool)`, `GetRadius/SetRadius(float)`, `GetHalfHeight/SetHalfHeight(float)`, `GetCenter/SetCenter(Vec3)`, `IsDynamic()` |
| `PlaneCollider` | `GetCenter/SetCenter(Vec3)` (estático, sin gravedad/dinámica) |

## AudioClip

La pista se asigna desde el editor (Properties → Audio → Browse o drag-drop
de un asset) o vía `entity:AddComponent("AudioClip", path)`.

| Método | Descripción |
| --- | --- |
| `clip:Play()` | Reproduce en la posición mundial actual del GameObject |
| `clip:Stop()` | Detiene la reproducción |
| `clip:SetLoop(bool)` | Activa/desactiva loop (recarga el sonido si cambia) |
| `clip:GetLoop()` | Estado actual de loop |
| `clip:SetVolume(v)` | Volumen del clip, recortado a `[0, 1]`. Se MULTIPLICA con el volumen del grupo SFX y el master (`setMasterVolume`/`setSfxVolume`/`setBgmVolume`), no lo sustituye. Seguro de llamar en `Update`: sólo escribe en el canal. |
| `clip:GetVolume()` | Volumen actual. |
| `clip:SetPitch(p)` | Pitch del clip, recortado a `[0.5, 2]`. `2.0` es una octava arriba y el doble de velocidad. Seguro en `Update`. |
| `clip:GetPitch()` | Pitch actual. |
| `clip:SetIs3D(b)` | Cambia entre 2D y 3D. **Recarga el sonido y corta lo que esté sonando**: es configuración, no lo llames por frame. |
| `clip:GetIs3D()` | `true` si el clip es 3D. |

Ver `Scripts/AudioFade.lua` para un fade completo.

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

## Ejemplos existentes

`Scripts/Mover.lua` (Input + Transform), `Scripts/Rotator.lua` (rotación
continua), `Scripts/AudioTest.lua` (AudioClip Play/Stop/Loop),
`Scripts/AudioFade.lua` (AudioClip SetVolume/GetVolume por frame), `Scripts/Test.lua`
(plantilla vacía).
