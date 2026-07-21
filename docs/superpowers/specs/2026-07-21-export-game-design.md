# Export del proyecto — build jugable de la escena actual

Fecha: 2026-07-21

## Objetivo

Exportar, desde el editor, un juego jugable de la escena actualmente abierta:
una carpeta autocontenida con un ejecutable de runtime, el `.scene` y solo los
assets que esa escena referencia.

Salida:

```
<destino>/<Nombre>/
  <Nombre>.exe          copia de DonTopoRuntime.exe
  game.scene
  assets/...            solo los referenciados + las 6 caras del skybox
  Scripts/...           carpeta completa
  shaders/*.spv
  fmod.dll
```

No hay `PhysX*.dll`: PhysX se compila estático (`PX_GENERATE_STATIC_LIBRARIES ON`
en `cmake/PhysX.cmake:32`).

## Decisiones cerradas

1. **Runtime pre-compilado.** Target CMake `DonTopoRuntime` construido junto al
   editor. Exportar es copiar ese `.exe` ya compilado más los datos. El export
   nunca invoca CMake ni un compilador.
2. **Solo assets referenciados.** Se recorre la escena, se recolectan los paths
   reales y se copian únicamente esos, reescribiendo los paths del `.scene`
   exportado a relativos respecto a la raíz del paquete.
3. **Carpeta plana.** Sin `.zip`, sin `.pak`, sin capa VFS.
4. **Luces y skybox hardcoded en el runtime.** No viven en `Scene` (se cablean
   en `sandbox/src/main.cpp:191-210`). `runtime/main.cpp` replica las mismas 2
   luces y llama a `initSkybox` con `assets/skybox/*.png`; el export copia esas
   6 caras siempre, aunque la escena no las "referencie".
5. **`Scripts/` se copia entera.** Los scripts se referencian por nombre
   (`ScriptComponent::scriptName`) y `ScriptManager::init(dir)` escanea la
   carpeta recursivamente; un `.lua` puede hacer `require` de otro. Filtrar por
   nombre rompería esas dependencias sin aviso.
6. **Sin cámara en la escena, el export aborta.** `Scene::findCamera()` es la
   única fuente de verdad; si devuelve `nullptr` no se copia nada y se informa
   por el Log Console.
7. **Assets fuera de la raíz del proyecto se aplanan** en
   `assets/_external/<nombre>`, con sufijo numérico ante colisión.
8. **Carpeta destino existente: confirmar, borrar y recrear.** Popup modal; al
   aceptar, `remove_all` + copia limpia. Así el paquete nunca arrastra assets
   huérfanos de un export anterior, que falsearían el criterio de
   "solo referenciados".
9. **Desacople Renderer↔EditorUI por flag `headless`**, no por split de
   librerías. El split (`DonTopoCore` + `DonTopoEditor`) queda como tarea
   posterior e independiente.

## Contexto del repo relevante

Hechos verificados que condicionan el diseño:

- `Renderer` embebe `EditorUI` como miembro (`Renderer.h:370`) y `Renderer.h`
  incluye `Editor/EditorUI.h` y `Editor/Gizmos.h`. Son los **únicos 2 includes**
  de core hacia editor en todo el motor.
- ImGui no vive solo en `EditorUI`: hay 36 referencias dentro de
  `Renderer.cpp` (`initImGui`/`shutdownImGui`, `m_offscreenDescSet` vía
  `ImGui_ImplVulkan_AddTexture`, el pass 2 de `recordCommandBuffer`, y el
  `NewFrame`/`Render` de `drawFrame`). `m_renderPass` y
  `m_swapChainFramebuffers` existen únicamente para ese pass de ImGui.
- La imagen offscreen se crea con **el mismo formato y extent que el
  swapchain** (`Renderer.cpp:2364-2368`) y su renderpass declara
  `initialLayout = VK_IMAGE_LAYOUT_UNDEFINED` (`Renderer.cpp:429`). Un blit
  offscreen→swapchain es por tanto 1:1 y no exige restaurar layouts.
- El `.scene` **no serializa texturas**. `nodeToJson` guarda `mesh.sourcePath`,
  `mesh.animationSources[].path` y `audioClip.path`; los paths de `Material`
  los deriva `ModelLoader` como `dirname(fbx)/filename`
  (`ModelLoader.cpp:156`). Conservar la jerarquía relativa en el paquete no es
  estética: es lo que hace que el runtime reencuentre las texturas al recargar
  el FBX.
- Los paths que guarda el editor son **absolutos** (vienen de
  `IGFD::FileDialog::GetFilePathName`), de ahí la necesidad de reescribirlos.
- `ScriptClass::path` (`ScriptManager.h:35`) da la resolución nombre → fichero
  `.lua`, accesible vía `ScriptManager::getRegistry()`.
- `Window::init` acepta `iconPath == nullptr` (`Window.cpp:32`): el runtime no
  necesita copiar el logo.
- `Mesh::sourcePath` vacío significa mesh procedural (Cube/Sphere/Plane/
  Capsule); su geometría ya viaja dentro del `.scene`, así que no aporta
  ningún asset.

## Arquitectura

### Ficheros nuevos

| Fichero | Contenido |
|---|---|
| `engine/include/DonTopo/Editor/GameExporter.h` | API del exportador |
| `engine/src/Editor/GameExporter.cpp` | recolección, reescritura, copia |
| `runtime/CMakeLists.txt` | target `DonTopoRuntime` |
| `runtime/main.cpp` | loop del juego, sin editor |
| `engine/tests/exporter_tests.cpp` | tests de la lógica pura |

### Ficheros modificados

- `engine/CMakeLists.txt` — añade `src/Editor/GameExporter.cpp`
- `engine/tests/CMakeLists.txt` — añade el target `dt_exporter_tests`
- `CMakeLists.txt` (raíz) — `add_subdirectory(runtime)` **después** de
  `add_subdirectory(sandbox)`
- `engine/include/DonTopo/Renderer/Renderer.h` y `engine/src/Renderer/Renderer.cpp`
  — modo headless
- `engine/include/DonTopo/Editor/EditorUI.h` y `engine/src/Editor/EditorUI.cpp`
  — menú `File > Export Game...`, diálogo y popups

`sandbox/src/main.cpp` no se toca. No se mueve ni se borra ningún fichero.

## GameExporter

Tres funciones libres, declaradas en el header para que los tests headless
puedan enlazarlas sin instanciar nada de ImGui/Vulkan — mismo patrón que
`countSceneReferences` y compañía en `ContentBrowserPanel.h:26-44`.

```cpp
struct ExportAsset {
    std::string sourcePath;    // absoluto, tal cual está en disco
    std::string packagePath;   // relativo a la raíz del paquete: "assets/model.fbx"
    bool        existsOnDisk = false;
};

struct ExportResult {
    bool                     ok = false;
    int                      fileCount = 0;
    std::uintmax_t           totalBytes = 0;
    std::vector<std::string> messages;   // volcados al Log Console
};

// SOLO lo que la escena referencia. Ordenado y sin duplicados.
std::vector<ExportAsset> collectSceneAssets(
    Scene& scene,
    const std::filesystem::path& projectRoot,
    const std::map<std::string, std::filesystem::path>& scriptPaths);

// Reescribe mesh.sourcePath, mesh.animationSources[].path y audioClip.path
// al packagePath correspondiente. Devuelve cuántos paths se reescribieron.
int rewriteScenePaths(nlohmann::json& sceneJson,
                      const std::map<std::string, std::string>& sourceToPackage);

// Crea la carpeta, copia exe/assets/skybox/shaders/Scripts/fmod.dll y
// escribe game.scene. Único punto que toca disco de forma destructiva.
ExportResult writeExportPackage(const std::vector<ExportAsset>& assets,
                                const nlohmann::json& rewrittenScene,
                                const std::filesystem::path& destDir,
                                const std::string& gameName,
                                const std::filesystem::path& projectRoot,
                                const std::filesystem::path& runtimeExe);
```

### Qué recolecta `collectSceneAssets`

Recorriendo `Scene::traverse`:

- `Mesh::sourcePath` (ignorado si está vacío: mesh procedural).
- `SkinnedMesh::animationSources[].path`. La fuente `builtin` comparte valor
  con `sourcePath`; la deduplicación la resuelve.
- Los 3 paths de cada `Material` (`texturePath`, `normalMapPath`,
  `metallicRoughnessPath`), tanto de `Mesh::material` como de
  `SkinnedMesh::materials`. Los campos `embeddedTexture*` no aportan path:
  viajan dentro del FBX.
- `AudioClipComponent::getPath()`.
- `ScriptComponent::scriptName` resuelto contra `scriptPaths`. Un nombre sin
  entrada en el mapa se ignora en silencio: el `.lua` llega igual al paquete
  por la copia completa de `Scripts/`.

La deduplicación y la comparación de rutas usan `weakly_canonical` +
minúsculas, igual que `samePath` en `ContentBrowserPanel.cpp:25`.

### Cálculo de `packagePath`

- Asset dentro de `projectRoot` → su ruta relativa, tal cual
  (`assets/chars/x.fbx` sigue siendo `assets/chars/x.fbx`).
- Asset fuera → `assets/_external/<filename>`. Si ese nombre ya está tomado
  por otro `sourcePath` distinto, se añade sufijo numérico:
  `assets/_external/x_1.fbx`.

### Qué añade `writeExportPackage` por su cuenta

Fuera de `collectSceneAssets`, y por tanto fuera del test de "exactamente los
assets referenciados":

- Las 6 caras de `assets/skybox/*.png` (el runtime las tiene hardcoded),
  tomadas de `projectRoot`.
- `shaders/*.spv`, desde `projectRoot/shaders`. `Renderer::createPipeline` ya
  los abre como `shaders/<nombre>.spv` relativo al CWD, así que si el editor
  arranca es porque están ahí; van a la raíz del paquete por el mismo motivo.
- `Scripts/`, completa, desde **`ScriptManager::scriptsDirPath()`** — no desde
  `projectRoot`. La carpeta se resuelve subiendo directorios desde el CWD
  (`sandbox/src/main.cpp:168-186`), así que vive en la raíz del repo mientras
  que `projectRoot` es el directorio del ejecutable.
- `fmod.dll`, desde `projectRoot`, si existe.
- `DonTopoRuntime.exe`, desde `projectRoot`, renombrado a `<Nombre>.exe`.

`projectRoot` es `std::filesystem::current_path()` canonicalizado, la misma
convención que usa `ContentBrowserPanel` (`ContentBrowserPanel.cpp:358-371`).
En una build normal eso es el directorio del ejecutable del editor, que es
donde el POST_BUILD de `sandbox/CMakeLists.txt` deja `assets/`, `shaders/` y
`fmod.dll`.

El mapa `sourceToPackage` que consume `rewriteScenePaths` se deriva de la
propia lista de `ExportAsset` (`sourcePath` → `packagePath`); no hay una
segunda fuente de verdad.

`Scripts/` se copia antes que los assets del plan; si algún `.lua` estuviera
también en la lista, la copia posterior cae sobre la misma ruta con
`overwrite_existing` y el resultado es idéntico.

### Contrato de errores

Todos abortan **antes** de copiar nada y se reportan al Log Console:

| Condición | Mensaje |
|---|---|
| `Scene::findCamera() == nullptr` | la escena no tiene cámara; el juego no podría renderizar |
| Algún `ExportAsset::existsOnDisk == false` | lista de los assets referenciados que faltan en disco |
| `DonTopoRuntime.exe` no está junto al editor | pide compilar el target `DonTopoRuntime` |
| Carpeta destino ya existe | popup de confirmación; al aceptar, `remove_all` y copia limpia |

En éxito, el Log recibe número de ficheros copiados y tamaño total.

## Modo headless del Renderer

`void setHeadless(bool)` más el miembro `bool m_headless = false`. Solo tiene
efecto si se llama antes de `init()`.

| Sitio | Cambio |
|---|---|
| `init()` (`Renderer.cpp:66`) | `initImGui` solo si `!m_headless` |
| `createSwapChain` (`:363`) | `imageUsage \|= VK_IMAGE_USAGE_TRANSFER_DST_BIT` |
| `createOffscreenImages` (`:2367`) | `usage \|= VK_IMAGE_USAGE_TRANSFER_SRC_BIT`; `ImGui_ImplVulkan_AddTexture` solo si `!m_headless` |
| `drawFrame` (`:116-122`) | salta `NewFrame` / `m_editorUI.draw` / `Render` |
| `recordCommandBuffer` (`:724`) | pass 2: si `m_headless`, blit; si no, ImGui como hoy |
| `shutdown` (`:182`) | `shutdownImGui` solo si `!m_headless` |
| `isPlaying()` | `return m_headless \|\| m_editorUI.isPlaying();` |

Los dos `|=` de usage van sin condicionar por el flag: son capacidades que
ninguna GPU con Vulkan niega, y así editor y runtime comparten exactamente el
mismo camino de creación de recursos — una rama menos donde puedan divergir.

`isPlaying()` devolviendo `true` en headless es lo que hace que
`currentFrameCamera()` elija el `CameraComponent` de la escena desde el frame 0
(`Renderer.cpp:284-288`), y lo que arranca física y scripts en el runtime.

Blit headless, sustituyendo el pass 2: barrier swapchain
`UNDEFINED → TRANSFER_DST`, `vkCmdBlitImage` offscreen→swapchain, barrier
swapchain `TRANSFER_DST → PRESENT_SRC`. El offscreen sale del renderpass en
`SHADER_READ_ONLY` y su siguiente uso lo redeclara `UNDEFINED`, así que basta
el barrier de lectura, sin restaurar.

El pass 1 (compute, shadow, escena, skybox, gizmos) no se toca. `m_renderPass`
y `m_swapChainFramebuffers` se siguen creando en headless aunque queden sin
uso: eliminarlos sería refactor del ciclo de vida del swapchain sin ganancia
funcional.

## DonTopoRuntime

`runtime/main.cpp`, calcado del wiring de `sandbox/src/main.cpp` menos todo lo
del editor:

1. Resuelve el directorio del `.exe` (`GetModuleFileNameW` en Windows,
   `/proc/self/exe` en el resto) y hace `current_path(exeDir)`. Sin esto, un
   paquete lanzado con otro CWD no encontraría sus assets relativos.
2. `scenePath = argc > 1 ? argv[1] : "game.scene"`.
3. `Window::init(1280, 720, <stem del exe>, nullptr)`.
4. Declara `physics`, `audio`, `scriptManager` y `scene` en **el mismo orden**
   que el editor. Los comentarios de `sandbox/src/main.cpp:38-55` documentan
   por qué: los `sol::table` de los `ScriptComponent` y los colliders de PhysX
   exigen ese orden de destrucción.
5. `scene.load(scenePath, physics, audio)`; si falla, mensaje a `stderr` y
   `EXIT_FAILURE`.
6. `renderer.setHeadless(true)` antes de `init`. Luego el mismo orden que el
   editor: recolectar meshes estáticos → `init(window, meshes)` →
   `setSceneRoot` / `setScene` / `setPhysicsManager` / `setAudioManager` →
   `initSkybox` → registrar los skinned con `addSkinnedMesh`.
7. `setLights` con las mismas 2 luces de `sandbox/src/main.cpp:207-210`.
8. `scriptManager.init("Scripts")` (relativo al paquete, ya en CWD), más
   `setScene` / `setPhysicsManager` / `setAudioManager` /
   `setOnInstantiated` / `setOnDestroying`. **Sin `pollChanges`**: un juego no
   tiene hot reload.
9. Loop: `Input::update` → dt → `audio.update` → `physics.stepSimulation` →
   `scene.update` → `scriptManager.update` → traverse fijando transforms y
   avanzando animadores (`anim->update(dt, /*playing=*/true)`) →
   `renderer.drawFrame`. Sin gizmos de debug. `ESC` cierra la ventana.
10. Shutdown en el mismo orden que el editor: `scene.shutdown(physics, audio)`,
    `audio.shutdown()`, `physics.shutdown()`, `renderer.shutdown()`,
    `window.shutdown()`.

`runtime/CMakeLists.txt` enlaza `DonTopoEngine` y añade un POST_BUILD que copia
`DonTopoRuntime.exe` junto a `Sandbox.exe`, para que el exportador lo encuentre
en su propio directorio. De ahí que `add_subdirectory(runtime)` deba ir después
de `add_subdirectory(sandbox)` en el CMakeLists raíz.

## UI del editor

`File > Export Game...` en `EditorUI::drawMenuBar` (hoy solo hay menú `View`,
`EditorUI.cpp:106`).

Flujo:

1. El item abre un `IGFD::FileDialog` en modo selección de carpeta. Instancia
   propia, no `Instance()`, por el mismo motivo documentado para
   `m_sceneFileDialog` (`EditorUI.h:122-127`): el singleton no soporta diálogos
   concurrentes.
2. Al aceptar, se abre un popup modal "Export Game" con un `InputText` para el
   nombre (por defecto `Game`) y botones Export / Cancel.
3. Si `<destino>/<Nombre>` ya existe, un segundo popup de confirmación avisa de
   que se borrará su contenido.
4. El resultado (`ExportResult::messages`) se vuelca a `m_logPanel`.

El mapa `scriptPaths` se construye en ese momento desde
`m_scriptManager->getRegistry()`.

## Tests

`engine/tests/exporter_tests.cpp`, target `dt_exporter_tests`. Todo sobre las
funciones puras: sin GPU, sin Lua y **sin PhysX** — los tests construyen los
`Mesh` a mano (con `sourcePath` y `material.texturePath` rellenos) en vez de
pasar por `ModelLoader`, y no crean colliders, así que no hay riesgo con la
regla de una sola `PxFoundation` por proceso.

1. Escena con 2 meshes + 1 textura + 1 script Lua + 1 audio clip →
   `collectSceneAssets` devuelve exactamente esos 5 paths y ninguno más.
2. Mesh procedural (`sourcePath` vacío) → no aporta ninguna entrada.
3. Dos GameObjects con el mismo FBX → una sola entrada.
4. `SkinnedMesh` con `animationSources` extra → cada `path` recolectado; la
   fuente `builtin` no duplica a `sourcePath`.
5. `packagePath` conserva la jerarquía (`assets/chars/x.fbx`); un asset fuera
   de la raíz cae en `assets/_external/x.fbx`; dos externos homónimos reciben
   sufijo distinto.
6. Tras `rewriteScenePaths`, ningún `mesh.sourcePath`,
   `mesh.animationSources[].path` ni `audioClip.path` del JSON es absoluto.
7. Un asset referenciado que no existe en disco queda marcado
   `existsOnDisk == false`.

## Criterios de aceptación

1. `build.bat` compila `Sandbox` y `DonTopoRuntime` sin errores ni warnings
   nuevos.
2. El runner de `engine/tests` pasa al 100%, incluidos los 7 tests nuevos.
3. Ninguna copia de asset no referenciado: si `assets/` tiene 8 ficheros y la
   escena usa 3, el paquete contiene esos 3 (más el skybox, que es una decisión
   explícita del runtime, no un descuido del filtro).
4. El `.exe` exportado, ejecutado desde una carpeta fuera del repo, abre
   ventana y renderiza la escena, sin ninguna ventana de ImGui.
5. `sandbox/src/main.cpp` sigue funcionando igual que antes.

El criterio 4 exige GUI: lo verifica el usuario a mano. Los demás se verifican
ejecutando build y tests.

## Fuera de alcance

Build settings en la UI, multi-escena, perfiles de release, menú de opciones
in-game, splash screen, compresión o empaquetado, y el split
`DonTopoCore` / `DonTopoEditor` (tarea posterior).
