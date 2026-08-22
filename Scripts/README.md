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
| `entity:GetComponent(name)` | Devuelve el componente si existe, si no `nil`. `name`: `"BoxCollider"`, `"SphereCollider"`, `"CapsuleCollider"`, `"PlaneCollider"`, `"AudioClip"`, `"Rigidbody"`, `"Animator"`, `"Canvas"`, `"Button"`, `"Text"`, `"ProgressBar"`, `"Layout"`, `"Panel"`, `"Image"`, `"Slider"`, `"Checkbox"`, `"Toggle"`, `"Scrollbar"`, `"InputField"`, `"Dropdown"`, `"ScrollView"`, o `"Script:<NombreClase>"` pa acceder a la instancia de otro script en el mismo GameObject |
| `entity:AddComponent(name, arg?)` | Añade componente (mismos defaults que el botón Add del editor; colliders mutuamente excluyentes). `AudioClip` requiere `arg` = ruta del asset. Los de UI no se excluyen entre sí (caben todos en el mismo GameObject) y pedir uno que ya está devuelve el que hay. `"Script:<Nombre>"` añade el script (Awake/Start se disparan en el siguiente lifecycle update) |
| `entity:RemoveComponent(name)` | Quita el componente (scripts se remueven diferido, al final del frame) |
| `entity:GetCanvas()` / `GetButton()` / `GetText()` / `GetProgressBar()` / `GetLayout()` / `GetPanel()` / `GetImage()` / `GetSlider()` / `GetCheckbox()` / `GetToggle()` / `GetScrollbar()` / `GetInputField()` / `GetDropdown()` / `GetScrollView()` | El componente de UI, o `nil` si no lo tiene |
| `entity:AddCanvas()` / `AddButton()` / `AddText()` / `AddProgressBar()` / `AddLayout()` / `AddPanel()` / `AddImage()` / `AddSlider()` / `AddCheckbox()` / `AddToggle()` / `AddScrollbar()` / `AddInputField()` / `AddDropdown()` / `AddScrollView()` | Lo crea con los valores por defecto del componente y devuelve el wrapper; si ya existe devuelve el que hay sin pisarlo |
| `entity:RemoveCanvas()` / `RemoveButton()` / `RemoveText()` / `RemoveProgressBar()` / `RemoveLayout()` / `RemovePanel()` / `RemoveImage()` / `RemoveSlider()` / `RemoveCheckbox()` / `RemoveToggle()` / `RemoveScrollbar()` / `RemoveInputField()` / `RemoveDropdown()` / `RemoveScrollView()` | Lo quita del GameObject |

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

## Physics

Consultas de rayo contra la escena de física. Solo hay escena de física en Play:
fuera de Play `Raycast` devuelve `nil` y `RaycastHit` devuelve `false`.

| Método | Descripción |
| --- | --- |
| `Physics.Raycast(origin, direction, maxDistance, options)` | Tabla con el impacto, o `nil` si no choca nada |
| `Physics.RaycastHit(origin, direction, maxDistance, options)` | `true` / `false`; no construye la tabla del impacto |

`origin` y `direction` son `Vec3`. `direction` se normaliza dentro, así que no
hace falta pasarla unitaria; con longitud 0 la llamada devuelve `nil` sin
consultar la física. `maxDistance` es opcional: ausente o `<= 0` usa el default
de **1000**.

`options` es una tabla opcional, y todos sus campos lo son:

| Campo | Tipo | Default | Qué hace |
| --- | --- | --- | --- |
| `hitTriggers` | bool | `false` | Si los colliders con *Is Trigger* cuentan como impacto |
| `static` | bool | `true` | Consultar los colliders sin Rigidbody (actores estáticos) |
| `dynamic` | bool | `true` | Consultar los colliders con Rigidbody (actores dinámicos) |
| `ignore` | Entity | — | GameObject a ignorar, para que un script no se choque consigo mismo |

Con `static = false` y `dynamic = false` no queda nada que consultar: devuelve
`nil` (o `false`) sin tocar la física.

La tabla que devuelve `Raycast` trae exactamente estos campos:

| Campo | Tipo | Descripción |
| --- | --- | --- |
| `entity` | Entity | GameObject impactado. `nil` si el collider no cuelga de ninguno |
| `point` | Vec3 | Punto de impacto, en coordenadas de mundo |
| `normal` | Vec3 | Normal de la superficie en el punto de impacto |
| `distance` | number | Distancia desde `origin` hasta el impacto |

Un argumento del tipo equivocado no tumba el script: la llamada devuelve `nil`
(o `false`) y deja un aviso en el Log.

```lua
Disparo = {
    alcance = 50
}

function Disparo:Update()
    if not Input.IsKeyPressed(Key.Space) then return end

    local origen = self.entity:GetTransform():GetWorldPosition()
    -- Hacia delante en el mundo (+Z). Para disparar en la dirección en la que
    -- mira el objeto, rota este Vec3 con su rotación.
    local hit = Physics.Raycast(origen, Vec3(0, 0, 1), self.alcance,
                                { ignore = self.entity })

    if hit then
        local quien = hit.entity and hit.entity.name or "algo sin GameObject"
        Log.Info("Impacto en " .. quien .. " a " .. hit.distance .. " unidades")
    else
        Log.Info("Nada delante")
    end
end
```

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

## Animator

`entity:GetComponent("Animator")`. Los parámetros no son propiedades: se
declaran en el grafo (panel Animator) y se leen y escriben **por nombre**. Un
nombre no declarado, o de otro tipo, se ignora en el setter y devuelve el valor
neutro en el getter — nunca lanza.

| Método | Descripción |
| --- | --- |
| `a:SetBool(n, v)` / `a:GetBool(n)` | Parámetro `bool` |
| `a:SetTrigger(n)` | Arma un `trigger`; lo consume la transición que dispara |
| `a:SetInt(n, v)` / `a:GetInt(n)` | Parámetro `int` |
| `a:SetFloat(n, v)` / `a:GetFloat(n)` | Parámetro `float` (NaN/Inf se ignora con aviso) |
| `a:GetState()` | Nombre del estado activo, `""` si el grafo está vacío |
| `a:IsBlending()` | `true` mientras dura un cross-fade |
| `a:GetBlendWeight()` | 0 = solo el estado que se apaga, 1 = solo el nuevo. Vale 1 si no hay mezcla |
| `a:GetPreviousState()` | Nombre del estado que se apaga, `""` si no hay mezcla |
| `a:GetPoseWeight()` | El peso que va de verdad a la GPU: el del cross-fade si lo hay, si no el del blend por parámetro, y 1 si no hay mezcla |

### Cross-fade

Cada transición tiene su **duración de mezcla en segundos**, que se edita en el
panel Animator: clic derecho sobre el link → `cross-fade (s)`. Con 0 la
transición es un corte instantáneo, que es el comportamiento de siempre y el que
traen las escenas guardadas antes de que el campo existiera.

Durante la mezcla los dos estados siguen animándose, cada uno con su propio
`ticksPerSecond` y su propio loop, y la pose que llega a la GPU es la
interpolación de los dos. Si una segunda transición dispara con una mezcla aún en
vuelo, la anterior se corta: solo hay dos clips en juego a la vez.

Los cuatro accesores de arriba son de **lectura**: la duración es autoría del
grafo, igual que las condiciones de una transición.

### Blend de dos clips por parámetro

Un estado puede llevar **un segundo clip** y mezclarlo con el suyo según un
parámetro `float` — el típico walk/run conducido por la velocidad. Se configura
en el nodo del panel Animator: `blend` (el segundo clip), `by` (el parámetro
float) y `min` / `max`, el rango del parámetro que se remapea a peso 0..1. Fuera
de ese rango el peso se clampa, no extrapola.

Desde Lua **se conduce con `SetFloat`** sobre ese parámetro; el peso resultante
se lee con `GetPoseWeight()`.

Los dos clips se muestrean en la **misma fase normalizada**, no en el mismo
tiempo absoluto: un walk de 40 ticks y un run de 100 se quedarían desfasados y
las piernas patinarían.

Dos límites que conviene saber, porque en el push constant solo caben dos clips:

- **Un cross-fade en vuelo manda sobre el blend del estado.** Mientras dura la
  transición cada lado aporta su clip primario; el segundo clip vuelve a entrar
  al terminar la mezcla.
- Un `blend` cuyo clip no exista en el modelo, o un `by` no declarado, dejan el
  estado como uno normal (un solo clip) en vez de mezclar contra basura.

```lua
Locomotion = {}

function Locomotion:Update(dt)
    local a = self.entity:GetComponent("Animator")
    if not a then return end
    local rb = self.entity:GetComponent("Rigidbody")
    if not rb then return end
    -- Vec3 no tiene Length(): la velocidad horizontal, a mano
    local v = rb.velocity
    -- El estado "Locomotion" mezcla Walk y Run con blendMin 1.5 / blendMax 6.5
    a:SetFloat("speed", math.sqrt(v.x * v.x + v.z * v.z))
end
```

```lua
Fade = {}

function Fade:Update(dt)
    local a = self.entity:GetComponent("Animator")
    if not a then return end
    a:SetBool("running", Input.IsKeyDown(Key.W))
    -- Silencia los pasos mientras el personaje aún está entrando en "Run"
    if a:IsBlending() and a:GetBlendWeight() < 0.5 then return end
end
```

## UI — Canvas / Button / Text / ProgressBar / Layout / Panel / Image / Slider / Checkbox / Toggle / Scrollbar / InputField / Dropdown / ScrollView

Los siete se obtienen con `entity:GetCanvas()`, `entity:GetButton()`,
`entity:GetText()`, `entity:GetProgressBar()`, `entity:GetLayout()`,
`entity:GetPanel()` y `entity:GetImage()` (o `GetComponent("Button")`, etc.).
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
| `UiCanvasRenderMode` | `ScreenSpace`, `World` |
| `UiBillboard` | `None`, `YawOnly`, `Full` |
| `UiTextAlign` | `Left`, `Center`, `Right`, `Justify` |
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
| `c.renderMode` | `UiCanvasRenderMode.*`. En `World` el canvas se coloca EN LA ESCENA y se ignoran `scaleMode`, `screenMatch`, `matchWidthOrHeight`, los tres DPI, `safeArea` y `aspectRatio` |
| `c.worldScale` | Solo `World`. Unidades de mundo por PÍXEL de canvas |
| `c.billboard` | Solo `World`. `UiBillboard.*`: `YawOnly` gira solo en la vertical, `Full` encara del todo a la cámara |
| `c.depthTest` | Solo `World`. A `false` se dibuja siempre encima, atravesando paredes |

**Varios canvas de pantalla a la vez.** Una escena puede tener los que quiera
(un HUD y un menú de pausa encima, por ejemplo) y **todos** reciben el input.
Cuando dos se solapan hay que repartirlo, y el reparto es este:

- **El ratón, a UNO solo:** el de **más arriba** que tenga algo bajo el cursor.
  Arriba = el último que se dibuja, o sea el que va más abajo en la jerarquía de
  la escena. Es el mismo criterio que usa el clic del viewport del editor para
  seleccionar, así que se selecciona lo que se ve encima.
- **Los de debajo NO se quedan pegados:** reciben el ratón *fuera*, así que
  sueltan el hover, emiten su `MouseExit` y sus botones vuelven a `Normal`.
  Siguen animando y fundiendo colores con normalidad.
- **Un arrastre no se corta.** Mientras un botón del ratón siga bajado, el
  canvas donde empezó conserva el puntero aunque el cursor pase por encima de
  otro. Es lo que permite arrastrar un slider hasta el borde de la pantalla.
- **El teclado y el mando siguen al FOCO, no al cursor:** van al canvas que
  tiene el foco, así que escribir en un campo sigue llegando aunque el ratón se
  pasee por otro canvas. El foco se mueve al **clicar**, y solo lo tiene un
  canvas a la vez: en cuanto otro lo coge, el anterior lo suelta. El Tab y las
  flechas dan la vuelta *dentro* del canvas que lo tiene y **no saltan** al
  siguiente canvas.

**Dos limitaciones del modo `World`.** No son bugs: salen del sitio donde se
graba el canvas, y conviene tenerlas escritas antes de tropezar con ellas.

- **Un canvas de mundo NO se puede clicar**, ni en el juego ni en el viewport
  del editor. El hit test de la UI trabaja en píxeles de pantalla y un canvas de
  mundo está *proyectado*: puede salir rotado, en perspectiva o partido por el
  borde. Se selecciona desde el Hierarchy, igual que el contenedor de un
  `Layout`. Al seleccionarlo, el editor pinta el cuadrilátero de su plano sobre
  el viewport —con una marca en su esquina (0,0)—, que es lo que enseña dónde
  está y con qué inclinación; si alguna de sus cuatro esquinas queda detrás de
  la cámara, no se dibuja nada. Los widgets de dentro se seleccionan igual, desde
  el Hierarchy, y su gizmo también sale proyectado sobre el cartel, inclinado con
  él y con los ejes X/Y del pivot en la orientación que tienen ahí.
- **`clipChildren` NO recorta en un canvas de mundo.** El scissor va en píxeles
  de canvas y se mapea 1:1 al framebuffer; con el canvas proyectado no hay
  rectángulo alineado a los ejes que lo represente, así que los canvas de mundo
  se graban con el scissor a todo el framebuffer. Lo que sí se respeta es un
  clip que ya se quedó vacío: ese nodo no emite nada.

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
| `b.sprite`, `b.normalSprite`, … | **Nombre de un sub-rect del atlas**, no una ruta. Los define el Sprite Editor del editor y viven en `<atlas>.sprites.json`; un nombre que no esté en ese fichero dibuja la imagen entera |
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

El atlas de una fuente se hornea con ASCII, el suplemento Latin-1 completo
(`á é í ó ú ü ñ Ñ ¿ ¡ « » º ª`), `…` y `€`. Un carácter fuera de eso —o que la
fuente elegida no traiga— no se dibuja **ni deja hueco**: si falta una letra,
mira primero si el TTF la tiene.

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

### Layout

Coloca a los **hijos** del GameObject. No dibuja nada: sin otro componente de UI
en el objeto, monta un contenedor propio (un rect que agrupa y recorta); con un
`Button`, `Text` o `ProgressBar` al lado, escribe sobre el nodo de aquel y el
**rect lo manda aquel** (`position`, `size`, anclas y pivote de aquí no se leen).

Tampoco recibe clics: un grupo que no pinta no puede comerse el ratón de lo que
tenga detrás. En el editor se selecciona desde el Hierarchy.

| Propiedad / Método | Descripción |
| --- | --- |
| `l.mode` | `UiLayoutMode.None` / `Horizontal` / `Vertical` / `Grid`. `None` = solo agrupa y recorta |
| `l.crossAlign` | `UiCrossAlign.Start` / `Center` / `End`. Eje TRANSVERSAL; el `Grid` no la usa |
| `l.paddingLeft` / `paddingRight` / `paddingTop` / `paddingBottom` | Margen interior del contenedor |
| `l.columns` | Solo `Grid`. `0` = las que quepan en el ancho |
| `l.fitWidth` / `l.fitHeight` | Content size fitter: ese eje del `Size` pasa a ser la extensión de los hijos + padding |
| `l.ignoreLayout` | Este objeto se ancla por su cuenta y NO ocupa hueco en el layout de su padre |
| `l.clipChildren` | Recorta a los descendientes contra este rect (se **interseca** con el recorte del padre) |
| `l.visible` | Se resuelve o no (un contenedor invisible esconde su subárbol) |
| `l:GetSpacing/SetSpacing(x,y)` | Hueco entre celdas: `x` entre columnas, `y` entre filas |
| `l:GetCellSize/SetCellSize(x,y)` | Solo `Grid`: la celda, que se le impone a cada hijo |
| `l:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect del contenedor (solo si es suyo) |

```lua
local menu = self.entity:GetLayout() or self.entity:AddLayout()
menu.mode = UiLayoutMode.Vertical
menu.paddingLeft, menu.paddingTop = 12, 12
menu:SetSpacing(0, 8)
menu:SetSize(240, 300)
```

### Panel

El rectángulo de fondo: sin atlas es un quad de color plano, con atlas el sprite
estirado al rect. No tiene campos propios más allá del rect, el color y el
sprite — el `Panel` del núcleo tampoco los tiene.

| Propiedad / Método | Descripción |
| --- | --- |
| `p.visible` | Se dibuja o no |
| `p.raycastTarget` | A `false` deja pasar el ratón a lo que tenga detrás. Un fondo a pantalla completa con esto a `true` se come **todos** los clics, y no hay nada que lo delate a la vista |
| `p.atlasPath` | Imagen del panel. Vacía = color plano |
| `p.sprite` | Nombre del sub-rect dentro del atlas. Vacío = la imagen entera |
| `p:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `p:GetColor/SetColor(r,g,b,a)` | Color (tinte si hay sprite) |

### Image

El sprite con sus cuatro modos de reparto dentro del rect. Los cuatro se
resuelven en CPU dentro del batcher (N quads del mismo atlas y el mismo
scissor): ni un shader ni un pipeline de más.

| Propiedad / Método | Descripción |
| --- | --- |
| `i.visible` / `i.raycastTarget` | Igual que en el `Panel` |
| `i.atlasPath` / `i.sprite` | Imagen y nombre del sub-rect |
| `i.mode` | `UiImageMode.Normal` / `Tiled` / `Sliced` / `Filled` |
| `i.borderLeft` / `borderRight` / `borderTop` / `borderBottom` | Solo `Sliced`. Píxeles **del sprite**, no del rect: escalar el elemento no los mueve |
| `i.fillCenter` | Solo `Sliced`. A `false` salen 8 quads en vez de 9: es lo que quiere un marco que deja ver lo de detrás |
| `i.maxTiles` | Solo `Tiled`. Tope duro de quads; pasado el tope el elemento se dibuja como `Normal` en vez de reventar el buffer de vértices |
| `i.fillDirection` | Solo `Filled`. `UiFillDirection.Horizontal` / `Vertical` |
| `i.fillOrigin` | Solo `Filled`. `UiFillOrigin.Start` / `End`. `Start` es izquierda en `Horizontal` y arriba en `Vertical` |
| `i.fillAmount` | Solo `Filled`. `0..1`; a `0` no se emite ni un quad |
| `i:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `i:GetColor/SetColor(r,g,b,a)` | Tinte del sprite (se multiplica) |

```lua
-- Marco de 9-slice que no deforma las esquinas al estirarse
local marco = self.entity:GetImage() or self.entity:AddImage()
marco.atlasPath = "assets/ui/frames.png"
marco.sprite = "ventana"
marco.mode = UiImageMode.Sliced
marco.borderLeft, marco.borderRight = 12, 12
marco.borderTop, marco.borderBottom = 12, 12
marco.fillCenter = false
marco:SetSize(400, 260)
```

### Slider / Checkbox / Toggle / Scrollbar — los interactivos

Los cuatro tienen algo que los demás no: **el jugador los mueve**, y lo que mueve
se escribe **en el componente**, no en el nodo del canvas. O sea que leer
`s.value` o `c.isOn` da el valor de verdad sin sondear nada, se serializa con la
escena y se ve en el inspector mientras el juego corre.

Los cuatro llevan `interactable` (a `false` se dibujan igual pero no se dejan
tocar) y `OnValueChanged(fn)`, que llama a `fn` con el valor nuevo **solo cuando
cambia**. Pasar `nil` lo quita. Igual que `Button:OnClick`, el dueño del callback
es el componente, así que registrar una vez en `Start` basta: sobrevive a las
reconstrucciones del árbol de UI.

#### Slider

| Propiedad / Método | Descripción |
| --- | --- |
| `s.value` / `s.minValue` / `s.maxValue` | El rango. Sin clamp al escribir a mano; el arrastre sí acota |
| `s.wholeNumbers` | Redondea el valor que se **escribe**, no solo el que se dibuja |
| `s.direction` | `UiSliderDirection.*` |
| `s.handleSize` | Largo del asa **en el eje del recorrido**, en px. Se descuenta del recorrido para que no se salga por las puntas. A `0` el recorrido es el rect entero |
| `s.interactable` / `s.visible` | |
| `s.atlasPath` / `s.backgroundSprite` / `s.fillSprite` / `s.handleSprite` | Un atlas, tres nombres de sub-rect |
| `s:GetColor/SetColor`, `GetFillColor/SetFillColor`, `GetHandleColor/SetHandleColor` | Pista, relleno y asa |
| `s:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `s:GetNormalizedValue()` | El `0..1` ya acotado (rango degenerado = 0) |
| `s:OnValueChanged(fn)` | `fn(nuevoValor)` |

La **pista entera** es zona de clic, no solo el asa: un click salta el valor a
donde esté el cursor, como en Unity. El arrastre sigue al ratón aunque salga del
rect.

#### Checkbox

| Propiedad / Método | Descripción |
| --- | --- |
| `c.isOn` | El valor. Un click lo invierte |
| `c.checkPadding` | Px que la marca se mete hacia dentro de la caja por los cuatro lados. Uno que no cabe deja la marca a cero, nunca un rect del revés |
| `c.interactable` / `c.visible` | |
| `c.atlasPath` / `c.backgroundSprite` / `c.checkmarkSprite` | |
| `c:GetColor/SetColor` / `c:GetCheckColor/SetCheckColor` | Caja y marca |
| `c:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `c:OnValueChanged(fn)` | `fn(nuevoValor)` |

**No lleva etiqueta de texto**: el `Text` es su propio componente y cabe en el
mismo GameObject (son nodos hermanos), así que meter aquí una copia de los campos
del texto sería mantener dos.

#### Toggle

Guarda el mismo dato que el `Checkbox` (un bool) y aun así es otro componente:
lo que cambia no es el dato sino los **campos** — la casilla tiene padding y
color de marca, el interruptor tiene dos colores de pista y el tamaño del mando.

| Propiedad / Método | Descripción |
| --- | --- |
| `t.isOn` | El valor. Un click lo invierte |
| `t.knobSize` / `t.knobPadding` | El mando se acota a lo que quede entre paddings: uno más grande que la pista asomaría por el borde |
| `t.interactable` / `t.visible` | |
| `t.atlasPath` / `t.backgroundSprite` / `t.knobSprite` | |
| `t:GetOffColor/SetOffColor` / `t:GetOnColor/SetOnColor` | La pista **no tiene un color suelto**: se pinta con uno u otro según el estado |
| `t:GetKnobColor/SetKnobColor` | |
| `t:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `t:OnValueChanged(fn)` | `fn(nuevoValor)` |

#### Scrollbar

Se parece al `Slider` pero no es lo mismo: el asa tiene tamaño **variable** (la
fracción del contenido que se ve) y el valor va siempre en `0..1` — no hay rango
propio porque quien lo interpreta es lo que se desplaza, no la barra.

| Propiedad / Método | Descripción |
| --- | --- |
| `s.value` | `0..1` |
| `s.handleFraction` | Fracción del canal que ocupa el asa. `1` = el contenido cabe entero y no hay nada que desplazar |
| `s.direction` | `UiScrollbarDirection.*` |
| `s.numberOfSteps` | Paradas discretas. `0` y `1` = continuo: enganchar a una sola parada dejaría la barra muerta en un sitio |
| `s.scrollStep` | Cuánto mueve la rueda por muesca, en fracción del recorrido |
| `s.interactable` / `s.visible` | |
| `s.atlasPath` / `s.backgroundSprite` / `s.handleSprite` | |
| `s:GetColor/SetColor` / `s:GetHandleColor/SetHandleColor` | Canal y asa |
| `s:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `s:SnapValue(v)` | El mismo enganche a paradas que aplica el arrastre |
| `s:OnValueChanged(fn)` | `fn(nuevoValor)` |

La **rueda del ratón** encima de la barra también la mueve, y el evento se
consume ahí: si siguiera burbujeando, un contenedor que la envolviera se
desplazaría a la vez y el contenido saltaría el doble por muesca.

```lua
function Opciones:Start()
    local vol = self.entity:GetSlider()
    vol.minValue, vol.maxValue = 0, 100
    vol.wholeNumbers = true
    vol:OnValueChanged(function(v)
        Log.Info("Volumen: " .. v)
    end)
end
```

### InputField / Dropdown / ScrollView

#### InputField

El único widget en el que escribe el **jugador**. Para que existiera hubo que
darle al canvas algo que no tenía: un canal de **caracteres**. `UiKey` nombra
teclas físicas con significado propio (`Tab`, `Enter`, flechas) y una `a` no es
una de esas — sale del layout del teclado y de las muertas —, así que el core
ganó `UiInputState.chars` y `UiElement::onTextInput`. Es infraestructura del
canvas, no de este componente: cualquier cosa futura que reciba texto (una
consola, un chat, un buscador) usa la misma.

| Propiedad / Método | Descripción |
| --- | --- |
| `f.text` | El texto de verdad, en UTF-8. En `Password` se guarda **tal cual**: lo que cambia es lo que se enseña |
| `f.placeholder` | Lo que se ve con el campo vacío, con su propio color |
| `f.interactable` | A `false` ni siquiera toma el foco |
| `f.readOnly` | Toma el foco y deja mover el cursor, pero no cambiar el texto |
| `f.characterLimit` | Cuenta **caracteres**, no bytes. `0` = sin límite |
| `f.contentType` | `UiInputContentType.*`. Filtra lo que se puede **teclear**, no lo que se dibuja |
| `f.passwordChar` | Con qué se enmascara. Vacío cae al asterisco |
| `f.fontPath` / `f.fontSize` / `f.align` / `f.padding` | |
| `f.caretWidth` / `f.caretBlinkRate` | Segundos por medio ciclo; `0` = fijo |
| `f.atlasPath` / `f.backgroundSprite` | |
| `f:GetColor/SetColor`, `GetTextColor/SetTextColor`, `GetPlaceholderColor/SetPlaceholderColor`, `GetCaretColor/SetCaretColor` | |
| `f:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `f:GetDisplayText()` | Lo que se **dibuja**: el placeholder si está vacío, o la máscara si es `Password`. Nunca la contraseña |
| `f:GetCaretPos()` / `f:SetCaretPos(n)` | Posición del cursor en **caracteres**, `0` = antes del primero. Se acota al escribirla |
| `f:OnValueChanged(fn)` | `fn(textoNuevo)`, en cada tecla que cambia el texto |
| `f:OnEndEdit(fn)` | `fn(texto)` al pulsar Enter o al perder el foco. Es donde valida un formulario, no en cada tecla |

`Left` y `Right` mueven el cursor y **consumen** la tecla: si no, la navegación
direccional del canvas se llevaría el foco a otro widget en mitad de una
palabra. `Up` y `Down` no se consumen, así que se puede salir del campo con el
mando.

#### Dropdown

El único cuyo subárbol **cambia de forma** con los datos: una opción más es un
nodo más. Añadir o quitar opciones reconstruye el árbol de UI; abrir y cerrar
no (la lista existe siempre y solo se apaga).

| Propiedad / Método | Descripción |
| --- | --- |
| `d.value` | Índice **0-based** de la elegida, igual que en C++ y en el inspector |
| `d.isOpen` | Estado vivo. **No se serializa**: una escena no puede abrirse con la lista tapando el menú |
| `d.itemHeight` / `d.maxVisibleItems` | `0` = todas |
| `d.interactable` / `d.visible` | |
| `d.fontPath` / `d.fontSize` / `d.padding` | |
| `d.atlasPath` / `d.backgroundSprite` / `d.arrowSprite` / `d.itemSprite` | |
| `d:GetColor/SetColor`, `GetListColor/SetListColor`, `GetItemColor/SetItemColor`, `GetItemSelectedColor/SetItemSelectedColor`, `GetArrowColor/SetArrowColor`, `GetTextColor/SetTextColor` | |
| `d:GetPosition/SetPosition`, `GetSize/SetSize`, `GetAnchorMin/SetAnchorMin`, `GetAnchorMax/SetAnchorMax`, `GetPivot/SetPivot` | Rect |
| `d:GetOptionCount()` | Cuántas hay |
| `d:GetOption(i)` | La opción `i`, con índice **1-based** (lo natural en Lua). Fuera de rango devuelve cadena vacía |
| `d:GetSelectedLabel()` | El texto de la elegida, o vacío si el índice no apunta a ninguna |
| `d:SetOptions(tabla)` | Reemplaza la lista. Lo que no sea cadena se descarta, entrada a entrada |
| `d:AddOption(s)` / `d:ClearOptions()` | |
| `d:OnValueChanged(fn)` | `fn(indice)` (0-based), solo cuando cambia |

**Ojo con los dos índices**: `d.value` es 0-based (es el campo del componente) y
`d:GetOption(i)` es 1-based (es una tabla de Lua). Para leer la elegida sin
pensarlo, `d:GetSelectedLabel()`.

#### ScrollView

| Propiedad / Método | Descripción |
| --- | --- |
| `v.horizontal` / `v.vertical` | Un eje apagado no se mueve aunque el contenido sea más grande |
| `v.scrollSensitivity` | **Píxeles** que mueve la rueda por muesca (no fracción: una lista de 50 filas y otra de 5 quieren el mismo recorrido por muesca) |
| `v.visible` | |
| `v.atlasPath` / `v.backgroundSprite` | |
| `v:GetContentSize/SetContentSize(x,y)` | Tamaño del área desplazable. Es un **campo**, no algo medido de los hijos |
| `v:GetNormalizedPosition/SetNormalizedPosition(x,y)` | `0` = principio, `1` = final, por eje |
| `v:GetScrollRange()` | Cuánto se puede desplazar por eje, en píxeles. Un eje apagado da `0` |
| `v:GetContentOffset()` | Dónde está el contenido dentro del viewport. Siempre `<= 0` |
| `v:GetPosition/SetPosition`, `GetSize/SetSize`, ... | Rect del **viewport** |
| `v:OnValueChanged(fn)` | `fn(x, y)` con la posición normalizada de los dos ejes |

**Los hijos del GameObject cuelgan del contenido**, no del viewport: por eso
desplazarse los arrastra. El viewport recorta (`clipChildren`), así que lo que
se salga no se dibuja.

**No tiene referencia a un Scrollbar.** Enlazarlos es una línea de script; una
referencia entre componentes de la escena habría que serializarla y mantenerla
viva en el clone, el undo y el borrado.

```lua
function Opciones:Start()
    local barra = Scene.Find("BarraLateral"):GetScrollbar()
    local lista = self.entity:GetScrollView()
    barra:OnValueChanged(function(v)
        local x = select(1, lista:GetNormalizedPosition())
        lista:SetNormalizedPosition(x, v)
    end)

    local nombre = Scene.Find("CampoNombre"):GetInputField()
    nombre.contentType = UiInputContentType.Alphanumeric
    nombre.characterLimit = 16
    nombre:OnEndEdit(function(t) Log.Info("Jugador: " .. t) end)
end
```

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

### Autocompletado en el Script Editor

Toda esta API de UI está en la lista de identificadores del Script Editor: al
teclear `Canvas.`, `Button:`, `Text.`, `ProgressBar:`, `Entity:Get`, o cualquiera
de las siete tablas de enums (`UiScaleMode.`, `UiScreenMatch.`, `UiTextAlign.`,
`UiTextOverflow.`, `UiProgressFillDirection.`, `UiButtonTransition.`,
`UiButtonState.`) salen las sugerencias. Propiedades con `.`, métodos con `:`,
igual que el resto de la lista.

## Ejemplos existentes

`Scripts/Mover.lua` (Input + Transform), `Scripts/Rotator.lua` (rotación
continua), `Scripts/AudioTest.lua` (AudioClip Play/Stop/Loop),
`Scripts/AudioFade.lua` (AudioClip SetVolume/GetVolume por frame), `Scripts/Test.lua`
(plantilla vacía).
