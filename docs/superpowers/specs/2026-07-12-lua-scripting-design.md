# Módulo de Scripting Lua — Diseño

**Fecha:** 2026-07-12
**Estado:** Aprobado

## Objetivo

Añadir scripting de gameplay en Lua al motor. El motor nunca llama a Lua
directamente: toda la interacción pasa por `ScriptComponent` (varios por
GameObject) y una binding layer que expone una API limitada y bien definida.
Ciclo de vida estilo Unity, hot reload y propiedades serializables con UI
auto-generada desde el principio.

## Decisiones cerradas

| Tema | Decisión |
|---|---|
| Lenguaje / VM | Lua 5.4 vanilla (no LuaJIT) |
| Binding lib | sol2 (header-only) |
| Dependencias | FetchContent automático en CMake, como el resto (sin pasos manuales) |
| Storage en GameObject | `std::vector<std::unique_ptr<ScriptComponent>>` (patrón de slots existente, vector para permitir múltiples) |
| Hot reload trigger | Polling mtime cada ~60 frames |
| Estado tras reload | Se pierde el estado runtime; instancia nueva con props serializables reasignadas + Awake/Start de nuevo |
| Componentes expuestos a Lua | Transform + 4 colliders + AudioClip + Scripts. RigidBody/Light/Renderer materiales fuera (no existen como componentes aún) |
| Prefab en `Scene.Instantiate` | Clonar GameObject existente en la escena (deep copy). Sin assets de prefab nuevos |
| FixedUpdate | Acumulador propio a 1/60 s en `Scene::update`, independiente del step variable de PhysX |
| Ejecución de scripts | Solo en Play Mode (gate `isPlaying()` existente). Edit Mode solo edita props |

## Dependencias (CMake)

- **Lua 5.4**: el repo oficial no trae CMakeLists (solo Makefile). Nuevo
  `cmake/Lua.cmake` siguiendo el patrón de `cmake/PhysX.cmake`:
  `FetchContent_Populate` de las fuentes + `add_library(lua_lib STATIC ...)`
  propio con los `.c` del core (excluyendo `lua.c` y `luac.c`, mains del
  intérprete standalone). Expuesto como target `Lua::Lua`.
- **sol2**: trae CMakeLists propio (target INTERFACE header-only) →
  `FetchContent_MakeAvailable(sol2)` directo, como glm/nlohmann_json.
- `engine` linkea `sol2` y `Lua::Lua`. Todo se resuelve en configure, sin
  intervención del usuario.

## Arquitectura

Cuatro piezas nuevas:

1. **ScriptManager** — dueña del `sol::state` único. Carga, compila, registra,
   instancia y recarga scripts.
2. **ScriptComponent** — vive en GameObject. Guarda la instancia Lua
   (`sol::table`) + overrides de props serializables.
3. **Binding layer** (`ScriptBindings.h/.cpp`) — todo el registro sol2 en un
   solo sitio. Fachadas delgadas (Entity, Transform, Scene, Input, Audio vía
   componente, Log). Nunca se exponen los managers internos.
4. **EditorUI** — entrada "Script" en el popup Add existente + panel de props
   auto-generado.

Archivos nuevos: `ScriptManager.h/.cpp`, `ScriptComponent.h/.cpp`,
`ScriptBindings.h/.cpp`, `Input.h/.cpp`, `cmake/Lua.cmake`. Carpeta
`Scripts/` en la raíz del sandbox; ScriptManager recibe la ruta en `init`.

## ScriptManager

- `init(scriptsDir)`: escanea `Scripts/` recursivo (`std::filesystem`), llama
  `loadScript(path)` por cada `.lua`.
- **Convención de registro**: `Enemy.lua` define/devuelve tabla global `Enemy`
  (nombre = filename sin extensión). La tabla es la clase (defaults +
  funciones). Registry: `map<string, ScriptClass>` con tabla clase, path,
  mtime y lista de props serializables.
- **Detección de props**: al registrar, itera la tabla clase; entradas con
  valor `number`, `boolean` o `string` son props serializables (defaults).
  Funciones y tablas anidadas se ignoran. Sin sintaxis extra.
- **Errores**: compilación/ejecución fallida → no registra, `pushLog` con el
  mensaje de Lua. Un script roto nunca tira el motor.
- **Factory** `createInstance("Enemy")`: tabla instancia nueva con metatable
  `__index` → tabla clase. Las props serializables se copian a la instancia
  (cada instancia tiene las suyas); las funciones se heredan vía metatable.
- **Hot reload** `pollChanges()` (frame loop, cada ~60 frames):
  - Compara mtime de cada script registrado; también detecta `.lua` nuevos.
  - Recompila. Si falla: log de error, las instancias vivas siguen con el
    código viejo.
  - Si compila: por cada ScriptComponent vivo de esa clase — guarda valores
    actuales de props serializables → instancia nueva → re-aplica valores →
    `Awake()` + `Start()` de nuevo.

## ScriptComponent

- Campos: `scriptName`, `sol::table instance`, `bool started`, puntero al
  GameObject dueño, `map<string, valor> overrides` (props editadas que
  difieren del default). No copiable.
- GameObject gana `m_scripts` (vector de unique_ptr) +
  `addScript/removeScript/getScripts`. Se permiten múltiples scripts por
  GameObject, incluso del mismo tipo (como Unity).

## Ciclo de vida

Solo en Play Mode. En Edit Mode ningún callback ejecuta.

- **Play**: tras el snapshot, ScriptManager recorre la escena, crea instancia
  por cada ScriptComponent (defaults + overrides) y llama `Awake()` en todos;
  después `Start()` en todos (two-pass, como Unity).
- **`Update(dt)`**: cada frame, desde `Scene::update`.
- **`FixedUpdate(dt)`**: acumulador a 1/60 s en `Scene::update` —
  `while (acc >= step) { FixedUpdate(step); acc -= step; }` con clamp del
  acumulador (~0.25 s máx) para evitar spiral of death.
- **`LateUpdate()`**: tras todos los Update del frame.
- **`OnDestroy()`**: al destruir la entity (`Scene.Destroy` o Stop), y al
  quitar el componente en Play.
- **Instantiate en Play**: los scripts del clon reciben Awake inmediato y
  Start antes de su primer Update.
- **Stop**: OnDestroy en todo, instancias destruidas, escena restaurada del
  snapshot (flujo actual).
- Callbacks opcionales: se comprueba `instance["X"].valid()` una vez al
  instanciar, no cada frame.
- **Error runtime en callback**: `pushLog` con traceback, el componente se
  marca con flag de error y deja de recibir callbacks hasta hot reload o Stop
  (evita spam y crash loop).
- `self` = tabla instancia; `self.entity` = Entity del GameObject dueño.

## Binding layer (API Lua)

Todo el registro sol2 en `ScriptBindings.cpp`.

- **Vec3** = `glm::vec3` usertype: `x/y/z`, `+`, `-`, `*escalar`,
  `Vec3.new(x,y,z)`.
- **Entity** (wrapper de `GameObject*`):
  - `entity.name` (get/set), `entity:GetTransform()`, `entity:GetParent()`,
    `entity:GetChildren()`, `entity:IsValid()`.
  - `entity:GetComponent(name)` con `"BoxCollider" | "SphereCollider" |
    "CapsuleCollider" | "PlaneCollider" | "AudioClip" | "Script:Enemy"` →
    wrapper o nil.
  - `entity:AddComponent(name)` / `entity:RemoveComponent(name)` — mismas
    factories que EditorUI (colliders vía PhysicsManager, etc.).
    `"Script:X"` añade otro ScriptComponent.
  - **Lifetime**: ScriptManager mantiene `set<GameObject*>` vivos. Cada método
    valida antes de tocar; entity muerta → error Lua capturado, no crash C++.
- **Transform**: `GetPosition/SetPosition`, `GetRotation/SetRotation` (euler
  grados), `GetScale/SetScale` sobre `localTransform` (descomposición glm);
  `GetWorldPosition()` read-only; `Translate(v)`, `Rotate(euler)`.
- **Scene** (tabla global):
  - `Scene.Instantiate(entity, [parent])`: deep clone (transform, mesh,
    colliders, audio, scripts con overrides; registro en Renderer/PhysX
    incluido).
  - `Scene.Destroy(entity)`: **diferido** — cola procesada al final del frame
    (tras LateUpdate), para no invalidar la iteración de Update. OnDestroy al
    procesar.
  - `Scene.Find(name)` → primera entity con ese nombre o nil.
  - `Scene.CreateGameObject(name, [parent])`.
- **Input** (tabla global + clase C++ `Input` nueva, fachada sobre GLFW):
  - `Input.IsKeyDown(k)` (mantenida), `Input.IsKeyPressed(k)` (edge down),
    `Input.IsKeyReleased(k)`, `Input.IsMouseButtonDown(b)`.
  - Estado prev/curr capturado una vez por frame en el loop.
  - Tabla `Key` con constantes (A–Z, 0–9, Space, Enter, Escape, Shift,
    flechas) y `MouseButton`.
  - No se migra ningún código existente (Camera sigue con glfwGetKey).
- **Audio**: vía componente — `clip:Play()` (usa posición mundial de la
  entity), `clip:Stop()`, `clip:SetLoop(b)`, `clip:GetLoop()`.
- **Colliders**: wrappers finos sobre lo que ya exponen las clases
  (`IsDynamic/SetDynamic`, gravedad, dimensiones según tipo).
- **Log**: `Log.Info/Warn/Error(msg)` → Log Console. `print` de Lua
  redirigido ahí también.

## Props serializables + UI editor

- Popup **Add** gana entrada "Script" → submenú con los scripts registrados.
- Por cada ScriptComponent: sección colapsable "Nombre (Script)" con botón de
  quitar (patrón remove existente, loguea al Log Console). La sección solo
  aparece tras Add (regla del proyecto).
- Campos auto-generados: `number` → DragFloat (DragInt si el default es
  entero), `boolean` → Checkbox, `string` → InputText. Label capitalizado
  ("attackRange" → "Attack Range").
- Editar escribe en `overrides`. En Play Mode escribe además en la instancia
  Lua viva (tweaking en caliente).
- Botón "Reset" por sección → borra overrides, vuelve a defaults del `.lua`.
- Script con error de compilación: sección en rojo con el mensaje, props no
  editables.

## Serialización de escena

- Por GameObject, array `"scripts"`:
  `[{ "name": "Enemy", "overrides": { "speed": 12 } }]`. Solo overrides — los
  defaults viven en el `.lua` (cambiar un default actualiza todas las escenas
  sin override, como prefabs de Unity).
- `fromJson`: script inexistente en `Scripts/` → warning al log, el
  componente se conserva como "missing script" con overrides intactos (no se
  pierden datos al re-guardar).
- El snapshot de Play Mode usa toJson → scripts y overrides sobreviven
  Play/Stop sin trabajo extra.
- **Hot reload × props**: prop desaparecida del `.lua` → override huérfano se
  conserva en memoria/JSON pero la UI no lo muestra. Props nuevas aparecen
  con su default.

## Testing / verificación

Sin framework de tests en el motor. Verificación:

- Build limpio (configure.bat/build.bat).
- Scripts de ejemplo en `Scripts/`: `Rotator.lua` (rota la entity en Update),
  `Mover.lua` (mueve con Input) — sirven de smoke test y documentación viva.
- Verificación manual GUI (se añade a la lista pendiente conocida).
