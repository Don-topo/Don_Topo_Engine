# Scene Serialization (File Manager) — Design Spec

## Problema

Hoy no existe forma de persistir la escena entre sesiones del editor. Toda la
jerarquía de `GameObject` (transforms, mesh refs, colliders, audio clips) se
construye en memoria vía `Scene::addGameObject` / EditorUI y se pierde al
cerrar la aplicación. No hay librería de serialización integrada en el
CMakeLists ni convención de fichero de escena.

## Objetivo

- Elegir e integrar una librería de serialización (JSON o YAML) vía
  `FetchContent`.
- Añadir capacidad de guardar/cargar la `Scene` completa (árbol de
  `GameObject` + componentes: mesh, los 4 tipos de collider, audio clip) a/de
  un fichero, de forma que reabrir el editor recupere el estado exacto.
- Exponer esto en el editor vía menú "File > Save Scene... / Load Scene..."
  con `ImGuiFileDialog`, mismo patrón que los diálogos existentes
  (`drawMeshDialog`, `drawAudioClipDialog`).

Fuera de scope esta iteración: serializar config de editor/cámara/ventanas
ImGui, sistema de asset registry con IDs/GUID (las referencias de asset se
guardan como path relativo, igual que hoy en Content Browser), materiales
custom no derivados del fichero de origen del mesh, multi-escena/escenas
anidadas (`.scene` embebido dentro de otro).

## Decisión de librería

**JSON + [nlohmann/json](https://github.com/nlohmann/json)** (`v3.11.3`),
sobre la alternativa YAML + yaml-cpp evaluada en brainstorming. Motivos:

- Header-only, `FetchContent_MakeAvailable` directo (sin el workaround
  `FetchContent_Populate` que usa el repo para ImGuiFileDialog/ImGuizmo).
- Menos boilerplate por componente: nuevo campo en un tipo serializable =
  añadirlo a la macro `NLOHMANN_DEFINE_TYPE_INTRUSIVE(Tipo, campo1, ...)`, sin
  tocar funciones `encode`/`decode` a mano (que sí exige yaml-cpp).
- Mejor rendimiento en ficheros grandes y camino de escape a formatos
  binarios compactos (BSON/CBOR/MessagePack) con la misma API, si en el
  futuro el tamaño de escena lo exige.
- Legibilidad/diff en git de YAML no aporta aquí: el fichero lo genera el
  editor, no se edita a mano.

## Diseño

### 1. CMake — integración de la librería

```cmake
# nlohmann/json — serialización JSON (Scene save/load)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)
```

Se enlaza `nlohmann_json::nlohmann_json` (target `INTERFACE` que expone el
include header-only) al target `engine` en `engine/CMakeLists.txt`.

### 2. `FileManager` — wrapper de I/O genérico

Nueva clase de funciones estáticas (`engine/include/DonTopo/FileManager.h` +
`.cpp`), sin estado, no acoplada a `Scene`/`GameObject` — reutilizable luego
para otros ficheros (config, presets):

```cpp
class FileManager {
public:
    // Escribe json formateado (pretty-print) en path. Devuelve false si no
    // se pudo abrir/escribir el fichero.
    static bool writeJson(const std::string& path, const nlohmann::json& j);

    // Lee y parsea path. Devuelve std::nullopt si el fichero no existe o el
    // JSON es inválido (no lanza excepción hacia el caller).
    static std::optional<nlohmann::json> readJson(const std::string& path);
};
```

### 3. Modelo de datos serializado

Raíz del fichero de escena:

```json
{
  "version": 1,
  "root": { ... nodo raíz recursivo ... }
}
```

`version` permite detectar formatos incompatibles en el futuro sin migrar
ahora. Cada nodo (recursivo vía `children`):

```json
{
  "name": "Player",
  "localTransform": [16 floats, column-major, igual layout que glm::mat4],
  "mesh": { "kind": "path", "sourcePath": "assets/models/player.fbx" },
  "boxCollider": { "halfExtents": [x,y,z], "center": [x,y,z], "useGravity": false },
  "audioClip": { "path": "assets/audio/step.wav", "loop": true, "is3D": true },
  "children": [ ... ]
}
```

- `mesh.kind == "path"` → `sourcePath` no vacío, se recarga con
  `ModelLoader::load(sourcePath)`.
- `mesh.kind == "procedural"` → `shapeType` uno de
  `"Cube"|"Sphere"|"Plane"|"Capsule"`, se recrea con los mismos parámetros
  fijos que usa hoy `EditorUI::createBasicShape` (`Cube::create(50.0f)`,
  `Sphere::create(50.0f)`, `Plane::create(50.0f, 0.0f)`,
  `Capsule::create(25.0f, 50.0f)`) — no hay UI para editar esos parámetros
  post-creación, así que no hace falta guardarlos.
- Como máximo uno de `boxCollider` / `sphereCollider` / `capsuleCollider` /
  `planeCollider` presente por nodo (mutuamente excluyentes, igual que
  `hasAnyCollider()` en `GameObject`). Cada uno guarda solo los campos "de
  datos" propios de su tipo (ver headers `BoxCollider.h`, `SphereCollider.h`,
  `CapsuleCollider.h`, `PlaneCollider.h`) — nunca punteros PhysX.
- `planeCollider` no guarda `useGravity` (siempre kinemático, sin toggle en
  esa clase).
- `Material` no se serializa aparte: se re-deriva al recargar el mesh desde
  `sourcePath` (incluye texturas embebidas en FBX/glTF, que `ModelLoader` ya
  extrae al cargar).
- Todos los bloques de componente (`mesh`, `*Collider`, `audioClip`) son
  opcionales — ausentes si el nodo no tiene ese componente.

### 4. `Scene::save` / `Scene::load`

```cpp
// Scene.h
bool save(const std::string& path) const;
bool load(const std::string& path, PhysicsManager& physics, AudioManager& audio);
```

**`save`**: recorre `m_root` recursivamente, construye el `nlohmann::json`
según el modelo de arriba, llama `FileManager::writeJson`. Devuelve `false`
si la escritura falla.

**`load`**:
1. `FileManager::readJson(path)` — si falla o `version` es incompatible,
   devuelve `false` sin tocar la escena actual (no se borra el estado en
   memoria a medias por una carga fallida).
2. Limpia `m_root` actual: `shutdown(physics, audio)` seguido de vaciar
   `children` — mismo camino de limpieza ordenada que ya existe.
3. Recorre el JSON recursivo; por cada nodo llama `addGameObject(name,
   parent)`, setea `localTransform`, y por cada bloque de componente
   presente invoca la misma factory que usa hoy `EditorUI`:
   - mesh → `ModelLoader::load(sourcePath)` o constructor procedural
     correspondiente.
   - collider → `PhysicsManager::createBox/Sphere/Capsule/Plane(...)`
     (mismas firmas que usa `EditorUI::add*Collider` hoy).
   - audioClip → `AudioManager::loadSound(path, is3D, loop)`.
4. Al terminar el recorrido, `m_root.updateWorldTransforms()` para propagar
   transforms mundiales a todo el árbol.

### 5. Manejo de errores

- **Asset roto** (mesh/audio con path que ya no existe en disco): no aborta
  la carga completa — loguea warning a stderr (patrón ya usado en el motor)
  y deja ese nodo concreto sin ese componente; el resto de la escena carga
  con normalidad.
- **JSON malformado o `version` futura desconocida**: `Scene::load` devuelve
  `false` sin modificar la escena actual; `EditorUI` muestra el error con el
  mismo patrón que ya usa para `m_audioLoadError`/errores de mesh (texto rojo
  bajo el diálogo).
- **Escritura fallida** (permisos, disco lleno): `Scene::save` devuelve
  `false`, `EditorUI` muestra el mismo patrón de error inline.

### 6. UI — File menu

Nueva entrada de menú (toolbar o barra superior, junto a donde vive hoy
"Basic Shapes") "File" con dos ítems:

- **Save Scene...** → abre `ImGuiFileDialog` propio (filtro `.json`,
  `cfg.path = "assets"`), al confirmar llama `scene.save(path)`.
- **Load Scene...** → mismo patrón, al confirmar llama
  `scene.load(path, physics, audio)`.

Ambos diálogos siguen el patrón ya establecido en `drawMeshDialog`/
`drawAudioClipDialog`: instancia de `ImGuiFileDialog` propia por diálogo,
flag `m_sceneDlgOpen` drenado cada frame independientemente de selección,
key plana sin prefijo `"##"` (evita la colisión de ID documentada en
`drawMeshSection`).

### 7. Testing

No hay framework de tests unitarios en el repo (motor verificado
manualmente vía sandbox/editor). Verificación de esta feature:

- Guardar una escena con los 4 tipos de collider + audio clip + mesh
  procedural + mesh importado (FBX), cerrar el editor, volver a abrir,
  cargar el fichero: jerarquía, transforms, física (dinámica y kinemática) y
  audio quedan idénticos al estado guardado.
- Cargar un fichero con un `sourcePath` de mesh/audio que ya no existe: la
  escena carga completa, ese nodo concreto queda sin el componente roto, sin
  crash.
- Cargar un fichero JSON malformado: `Scene::load` devuelve `false`, la
  escena en memoria no cambia, no crashea.
- Guardar y cargar en el mismo path repetidamente no acumula estado
  corrupto (idempotencia razonable).

## Riesgos / notas de implementación

- Las factories de `PhysicsManager`/`AudioManager` que usa `EditorUI` hoy
  para crear colliders/audio (`createBox`, `loadSound`, etc.) deben tener
  firmas reutilizables directamente desde `Scene::load` sin pasar por
  EditorUI — confirmar en implementación que no dependen de estado propio de
  EditorUI (selección, etc.).
- `updateWorldTransforms()` debe ejecutarse **después** de reconstruir los
  colliders kinemáticos, para que su primer `syncTransform` en el siguiente
  `Scene::update` parta de la pose correcta (evitar un frame de pop visual
  al cargar).
- Confirmar en implementación si `GameObject` necesita un método
  `clearChildren()` explícito o si `Scene::load` puede reusar
  `removeGameObject` en bucle sobre los hijos directos de `m_root` para el
  paso de limpieza previo a la carga.
