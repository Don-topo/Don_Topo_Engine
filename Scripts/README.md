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
| `OnTriggerEnter(other)` | Otro collider entra en este trigger |
| `OnTriggerStay(other)` | Cada frame de física mientras siguen solapando |
| `OnTriggerExit(other)` | El otro collider sale |

Todos son opcionales — solo se llaman los que el script define. Un error en
cualquiera loguea el mensaje y **desactiva ese componente** (deja de recibir
callbacks) hasta hot reload o `Stop`; nunca crashea el motor.

### Triggers

Los tres `OnTrigger*` exigen que el GameObject tenga un collider con **Is
Trigger** marcado en Properties. `other` es una `Entity`, igual que
`self.entity`.

Tres reglas que se descubren tarde si nadie las dice:

- **Al menos uno de los dos objetos necesita un Rigidbody** — el trigger o el
  que entra, da igual cuál. PhysX no reporta solapes entre dos objetos
  estáticos, y un collider sin Rigidbody lo es. Sin esto no salta nada y no hay
  ningún error: el editor lo avisa bajo el checkbox. Misma regla que Unity.
- **Solo el lado trigger recibe los callbacks.** El objeto que entra no se
  entera, salvo que él también sea trigger frente a un no-trigger.
- **Trigger contra trigger no dispara nada**, por la misma limitación de PhysX.

`OnTriggerStay` se sintetiza por frame (PhysX solo da Enter y Exit), así que
loguear ahí inunda la consola enseguida.

Ejemplos: `Scripts/TriggerProbe.lua` (cuenta entradas y salidas) y
`Scripts/TriggerTest.lua` (destruye su GameObject en el Enter).

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
| `entity:GetComponent(name)` | Devuelve el componente si existe, si no `nil`. `name`: `"BoxCollider"`, `"SphereCollider"`, `"CapsuleCollider"`, `"PlaneCollider"`, `"AudioClip"`, `"Rigidbody"`, `"Animator"`, `"Canvas"`, `"Button"`, `"Text"`, `"ProgressBar"`, o `"Script:<NombreClase>"` pa acceder a la instancia de otro script en el mismo GameObject |
| `entity:AddComponent(name, arg?)` | Añade componente (mismos defaults que el botón Add del editor; colliders mutuamente excluyentes). `AudioClip` requiere `arg` = ruta del asset. Los cuatro de UI no se excluyen entre sí y pedir uno que ya está devuelve el que hay. `"Script:<Nombre>"` añade el script (Awake/Start se disparan en el siguiente lifecycle update) |
| `entity:RemoveComponent(name)` | Quita el componente (scripts se remueven diferido, al final del frame) |
| `entity:GetCanvas()` / `GetButton()` / `GetText()` / `GetProgressBar()` | El componente de UI, o `nil` si no lo tiene |
| `entity:AddCanvas()` / `AddButton()` / `AddText()` / `AddProgressBar()` | Lo crea con los valores por defecto del componente y devuelve el wrapper; si ya existe devuelve el que hay sin pisarlo |
| `entity:RemoveCanvas()` / `RemoveButton()` / `RemoveText()` / `RemoveProgressBar()` | Lo quita del GameObject |

## Transform

| Método | Descripción |
| --- | --- |
| `t:GetPosition()` / `t:SetPosition(Vec3)` | Posición local |
| `t:GetRotation()` / `t:SetRotation(Vec3)` | Rotación local en euler-grados |
| `t:GetScale()` / `t:SetScale(Vec3)` | Escala local |
| `t:GetWorldPosition()` | Posición mundial (traducción de la world matrix) |
| `t:Translate(Vec3 delta)` | Suma delta a la posición local |
| `t:Rotate(Vec3 deltaEulerGrados)` | Rotación incremental compuesta como quaternion (no se atasca en rotación continua multi-eje) |

### Mover por Transform NO colisiona

Un objeto movido con `SetPosition`/`Translate` **atraviesa las paredes**, tenga
o no Rigidbody. No es un fallo: mover el transform es un teletransporte, y
PhysX no resuelve colisiones en un teleport — con Rigidbody kinematic va a
`setKinematicTarget` (un kinematic empuja a los dinámicos, pero nada lo detiene
a él) y con Rigidbody dinámico va a `setGlobalPose`, que lo deja donde le digas
aunque quede solapado. Es la misma regla que en Unity con `transform.position`.

Para que un objeto **choque** de verdad hay que moverlo por la física: dejarlo
caer con gravedad, o empujarlo con `AddForce`/`AddImpulse`/`velocity` del
Rigidbody.

Los **triggers sí funcionan** moviendo por Transform: detectan solape, que no
necesita resolución de colisión. Por eso un objeto puede disparar
`OnTriggerEnter` de una zona y aun así atravesar una pared sólida.

## Scene

| Método | Descripción |
| --- | --- |
| `Scene.Find(name)` | Primer GameObject con ese nombre (excluye la raíz), o `nil` |
| `Scene.CreateGameObject(name, parent?)` | Crea un GameObject nuevo, opcionalmente hijo de `parent` |
| `Scene.Destroy(entity)` | Encola destrucción (procesada al final del frame). Alias interno de `DestroyGameObject` |
| `Scene.Instantiate(entity, parent?)` | Clona un GameObject (incl. sub-árbol, componentes, scripts); `Awake` se llama de inmediato, `Start` en el siguiente lifecycle update |

## DonTopo — cambio de escena en runtime

| Método | Descripción |
| --- | --- |
| `DonTopo.loadScene(path)` | Pide cargar la escena de `path` (fichero de Save Scene: `version: 1` + `root`). Devuelve `true` si la petición se encoló, `false` si la ruta está vacía, el fichero no existe, el JSON no parsea o la estructura no es de escena v1 (el motivo sale en el Log) |

La carga **no ocurre en la llamada**: el binding solo deja la petición en un buzón
y la ejecuta el dueño de la escena al frame siguiente, fuera del tick de scripts.
Cargar en mitad de un `Update` destruiría el GameObject que está ejecutando ese
mismo script. Consecuencias prácticas:

- Tras llamar, la escena vieja **muere entera**, tu script incluido. Trátala como
  la última línea útil: no toques `self` ni guardes referencias después.
- El `bool` es el resultado de la **validación**, no de la carga. El desenlace de
  la carga llega un frame más tarde y sale en el Log (`Escena cargada: ...` /
  `Error al cargar escena: ...`).
- Si un frame deja varias peticiones, **gana la última** y las demás se descartan.
- Solo en **Play Mode**. En Edit Mode se ignora con un aviso en el Log Console.
- La ruta es relativa al directorio de trabajo (la raíz del proyecto en el editor;
  la carpeta del ejecutable en el juego exportado, que fija su CWD ahí).

```lua
function test:Update(dt)
    if Input.IsKeyPressed(Key.R) then
        if not DonTopo.loadScene("Scenes/Empty.json") then
            Log.Error("Error loading scene")
        end
    end
end
```

Ojo con las mayúsculas: la tabla es `DonTopo`. Escribir `Dontopo` da
`attempt to index a nil value` y el componente queda con `hasError`, o sea sin
recibir más callbacks hasta el hot reload o Stop.

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
| `clip:SetVolume(v)` | Volumen del clip, recortado a `[0, 1]`. Se MULTIPLICA con el del grupo SFX y el master, no los sustituye (esos dos sólo se ajustan desde C++, con `AudioManager::setSfxVolume`/`setMasterVolume`; el grupo BGM no interviene). Seguro de llamar en `Update`: sólo escribe en el canal. |
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

## UI — Canvas / Button / Text / ProgressBar

Los cuatro se obtienen con `entity:GetCanvas()`, `entity:GetButton()`,
`entity:GetText()` y `entity:GetProgressBar()` (o `GetComponent("Button")`, etc.).
Devuelven `nil` si el componente no está; el wrapper resuelve el componente **en
cada acceso**, así que usarlo después de quitarlo da error de Lua, no memoria
liberada.

**Escalares, strings, booleanos y enums son propiedades** (`b.text = "Jugar"`).
**Los vectores son métodos** (`b:SetSize(200, 48)`, `local w, h = b:GetSize()`),
porque en Lua solo hay `Vec3`: los getters devuelven 2 o 4 valores. Un valor
NaN/Inf se ignora con un aviso en el Log, igual que en `Transform.SetPosition`.

Los enums viajan como tablas de constantes enteras:

| Tabla | Valores |
| --- | --- |
| `UiScaleMode` | `ConstantPixelSize`, `ScaleWithScreenSize`, `ConstantPhysicalSize` |
| `UiScreenMatch` | `MatchWidthOrHeight`, `Expand`, `Shrink` |
| `UiTextAlign` | `Left`, `Center`, `Right`, `Justify` |
| `UiTextOverflow` | `Overflow`, `Clip`, `Ellipsis` |
| `UiProgressFillDirection` | `LeftToRight`, `RightToLeft`, `BottomToTop`, `TopToBottom` |
| `UiButtonTransition` | `ColorTint`, `SpriteSwap`, `Animation` |
| `UiButtonState` | `Normal`, `Hover`, `Pressed`, `Disabled`, `Selected` |

### Canvas

| Propiedad / Método | Descripción |
| --- | --- |
| `c.scaleMode` | `UiScaleMode.*` |
| `c.scaleFactor` | Multiplica a los tres modos |
| `c.screenMatch` | `UiScreenMatch.*` |
| `c.matchWidthOrHeight` | 0 = ancho, 1 = alto (solo `ScaleWithScreenSize`) |
| `c.screenDpi` / `c.fallbackDpi` / `c.referenceDpi` | DPI real (0 = desconocido), el que se usa si no se sabe, y el de referencia de `ConstantPhysicalSize` |
| `c.aspectRatio` | 0 = apagado |
| `c:GetReferenceResolution()` / `c:SetReferenceResolution(w, h)` | Resolución de referencia |
| `c:GetSafeArea()` / `c:SetSafeArea(l, t, r, b)` | Insets en píxeles reales |

### Button

| Propiedad / Método | Descripción |
| --- | --- |
| `b.visible` | Se dibuja o no |
| `b.atlasPath` / `b.sprite` | PNG del atlas y nombre del sprite base (vacíos = color plano) |
| `b.interactable` | `false` fuerza el estado `Disabled` |
| `b.selected` | Estado `Selected` sostenido |
| `b.transition` | `UiButtonTransition.*` |
| `b.normalSprite` / `hoverSprite` / `pressedSprite` / `disabledSprite` / `selectedSprite` | Nombres dentro del MISMO atlas |
| `b.fadeDuration` | Segundos del fundido de `Animation` |
| `b.text` / `b.fontPath` / `b.fontSize` / `b.textAlign` | Etiqueta (hijo `Text` que monta el sync); `fontPath` vacío = fuente por defecto |
| `b:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect, en píxeles y anclas normalizadas |
| `b:GetColor/SetColor(r,g,b,a)` | Color base |
| `b:GetNormalColor/SetNormalColor`, `GetHoverColor/SetHoverColor`, `GetPressedColor/SetPressedColor`, `GetDisabledColor/SetDisabledColor`, `GetSelectedColor/SetSelectedColor` | Los 5 colores de estado |
| `b:GetTextColor/SetTextColor(r,g,b,a)` | Color de la etiqueta |
| `b:GetState()` | `UiButtonState.*` resuelto por el último input (solo lectura) |
| `b:OnClick(fn)` / `b:OnDoubleClick(fn)` | Registra el callback; pasar `nil` lo quita |

### Text

| Propiedad / Método | Descripción |
| --- | --- |
| `t.visible`, `t.text`, `t.fontPath`, `t.fontSize` | Lo básico; `fontPath` vacío = fuente por defecto |
| `t.outlineWidth` | 0 = sin contorno |
| `t.align` / `t.overflow` / `t.wordWrap` | `UiTextAlign.*`, `UiTextOverflow.*`, salto de línea |
| `t.boldStrength` / `t.italicSkew` | Negrita y cursiva simuladas |
| `t:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `t:GetColor/SetColor(r,g,b,a)` | Relleno del glifo |
| `t:GetOutlineColor/SetOutlineColor(r,g,b,a)` | Contorno |
| `t:GetShadowOffset/SetShadowOffset(x,y)`, `t:GetShadowColor/SetShadowColor(r,g,b,a)` | Sombra (offset 0,0 = sin sombra) |

### ProgressBar

| Propiedad / Método | Descripción |
| --- | --- |
| `p.visible` | Se dibuja o no |
| `p.value` / `p.minValue` / `p.maxValue` | Sin clamp: el rango lo normaliza el dibujado |
| `p.fillDirection` | `UiProgressFillDirection.*` |
| `p.atlasPath` / `p.backgroundPath` / `p.fillPath` | Imagen compartida (fallback), del fondo y del relleno |
| `p:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `p:GetColor/SetColor(r,g,b,a)` / `p:GetFillColor/SetFillColor(r,g,b,a)` | Color del fondo y del relleno |
| `p:GetNormalizedValue()` | El `0..1` ya acotado que usa el dibujado (rango degenerado = 0) |

### Callbacks del Button: qué los mata y qué no

El callback lo guarda el **componente**, no el nodo del canvas, y el sync lo
vuelve a enganchar cada vez que reconstruye la raíz — cosa que pasa al añadir o
quitar cualquier widget de la escena. O sea: registrar una vez en `Start` basta,
sobrevive a las reconstrucciones.

Se invalidan (dejan de dispararse, sin error ni crash) cuando:

- se **recarga en caliente** el script — el código que lo registró ya no existe;
  vuelve a engancharlo el `Start` del script recargado,
- se **para el Play** — las instancias se destruyen,
- se **quita el componente** o muere el GameObject.

Un error dentro del callback se registra en el Log Console (`[Lua][ERROR]
Button.OnClick: ...`) y no tumba el frame ni al resto de scripts.

### Dos cosas que confunden

1. **Los setters escriben en el componente, no en el nodo vivo.** El sync vuelca
   el componente sobre el árbol del canvas cada frame, así que escribir al nodo
   directamente no serviría de nada. Escribir una ruta de atlas o de fuente **no
   carga nada en ese instante**: la carga es del sync.
2. **En el editor el ratón solo entra en Play y con el cursor sobre la imagen del
   viewport** (como en Unity, en edición un botón no se ilumina). Las coordenadas
   son píxeles del canvas desde la esquina superior izquierda de esa imagen.

```lua
-- Scripts/BotonDemo.lua
BotonDemo = {}

function BotonDemo:Start()
    local b = self.entity:GetButton()
    if b == nil then
        print("este GameObject no tiene Button")
        return
    end

    b.text = "Jugar"
    b.transition = UiButtonTransition.Animation
    b:SetSize(220, 48)
    b:SetNormalColor(0.1, 0.5, 0.9, 1)
    b:SetHoverColor(0.2, 0.7, 1.0, 1)

    self.vidas = 3
    b:OnClick(function()
        self.vidas = self.vidas - 1
        local barra = self.entity:GetProgressBar() or self.entity:AddProgressBar()
        barra.minValue, barra.maxValue = 0, 3
        barra.value = self.vidas
        print("quedan " .. self.vidas)
    end)
end
```

## Ejemplos existentes

`Scripts/Mover.lua` (Input + Transform), `Scripts/Rotator.lua` (rotación
continua), `Scripts/AudioTest.lua` (AudioClip Play/Stop/Loop),
`Scripts/AudioFade.lua` (AudioClip SetVolume/GetVolume por frame), `Scripts/Test.lua`
(plantilla vacía).
