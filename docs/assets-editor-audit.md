# Auditoría del Content Browser / sistema de assets del editor

Fecha: 2026-09-23. Solo lectura: no se ha tocado código ni ejecutado build,
test ni harness. Formato heredado de `docs/animation-audit.md` /
`docs/core-audit.md`. Rutas relativas a `engine/` salvo `docs/`.

Alcance leído, entero: `include/DonTopo/Editor/ContentBrowserPanel.h`,
`src/Editor/ContentBrowserPanel.cpp` (850 líneas), `include/DonTopo/Editor/EditorContext.h`.
Leído parcialmente para entender consumidores del drag&drop y de la
importación de assets: `src/Editor/PropertiesPanel.cpp` (los 18 diálogos IGFD
y `canAcceptAsset`), `src/Editor/ScenePanel.cpp`, `src/Editor/AnimatorPanel.cpp`.
Documentación previa revisada: `docs/superpowers/plans/2026-07-16-content-browser-folder-tree.md`
(implementó el árbol de carpetas propio, sustituyendo IGFD embebido — ya
CERRADO y en main) y `docs/superpowers/plans/2026-07-14-editor-panel-split.md`
(extrajo `ContentBrowserPanel` de `EditorUI`, ya CERRADO y en main).

## Estado vigente (2026-09-23)

Las tablas de las secciones 2 y 3 son el **diagnóstico original**: su evidencia
`file:line` describe el código tal y como estaba al medir, y no se ha reescrito. El
estado actual de cada hallazgo es este:

| ID | Estado | Cómo se cerró |
|---|---|---|
| U1 (import real de ficheros externos) | **CERRADO** | `AssetImport` (copia a `assets/Imported/<Tipo>/`, nunca sobreescribe) y `acceptOrImportAsset` en los 18 diálogos Browse; merge `8e65a5f`. La revisión final encontró que la primera versión dejaba pasar sin copiar justo el caso del Escritorio; corregido en `d7841cb`. |
| U2 (drop desde el Explorador) | **CERRADO** | `glfwSetDropCallback` en las dos ramas de `sandbox/src/main.cpp` (Vulkan y D3D12); `8e65a5f` |
| U3 (miniaturas) | **CERRADO solo para texturas** | Atlas compartido de 2048² con casillas de 64², decodificación en el `JobSystem`, subida en lote en Vulkan y D3D12. Modelos y materiales siguen sin miniatura. |
| U4 (breadcrumb / crear carpeta) | **CERRADO** | Breadcrumb `ab3994c`; Create Folder `5561836` |
| U5 (menú Create) | **CERRADO solo para carpeta** | No hay más tipos creables porque el Core no tiene asset de Material ni de otro tipo |
| U6 (búsqueda y filtro) | **CERRADO** | Filtro por nombre y por tipo, `5561836` |
| U7 (multiselección) | **CERRADO** | Ctrl/Shift+clic, arrastre y borrado de varios, `338de1d`; mover arrastrando a una carpeta, `d4eb633` |
| U8 (import settings por asset) | **CERRADO para texturas de material** | Sidecar `<asset>.import.json` con espacio de color (auto/sRGB/lineal) y mipmaps; modal "Import Settings..." en el menú contextual; Vulkan y D3D12; viaja con el asset y con el export. **Modelos y audio siguen abiertos**: son la siguiente spec (el formato del sidecar ya lleva `type`). Spec `docs/superpowers/specs/2026-09-24-texture-import-settings-design.md` |
| U9 (refresco ante cambios externos) | **CERRADO por polling** | Relectura de la carpeta actual cada 0,5 s, `56dad33`. No se hizo un watcher nativo a propósito: el repo ya usa polling de `last_write_time` para el hot reload de Lua y el árbol ya reescanea cada frame |

En la comparación con Unity (§3): las capacidades 1, 3, 4, 5, 7 y 8 pasan a
**EXISTE** (la 8 con latencia ≤ 0,5 s), la 2 a **EXISTE solo para texturas**, y la 6
(import settings) a **EXISTE solo para texturas de material** (modelos y audio, sin hacer).

## 0. Qué ya estaba decidido y no se re-litiga aquí

- **El árbol de carpetas propio ya existe** (`drawFolderTree`,
  `ContentBrowserPanel.cpp:430-469`). No es IGFD: es un `TreeNodeEx`
  recursivo sobre `listVisibleSubdirs`. El plan de 2026-07-16 ya cerró esto.
- **Rename/Delete de assets con detección de referencias en escena ya
  existen** (`beginAssetRename`/`beginAssetDelete` + `updateSceneReferencesForRename`/
  `detachSceneReferencesForDelete`, `ContentBrowserPanel.cpp:189-419`), con
  cobertura de mesh/audio/material/materialOverrides, incluidos los slots
  `base*` que no se serializan. Esto es infraestructura sólida y no es objeto
  de esta auditoría.
- **El filtrado de "carpeta de ruido"** (`isHiddenDir`, `:148-158`) ya se
  decidió por contenido (`CMakeCache.txt`) y no por nombre, documentado en el
  propio código como decisión deliberada. No se propone tocarlo.

## 1. Mapa del flujo actual

| Pieza | fichero:línea | Qué hace |
|---|---|---|
| Árbol de carpetas (panel izq.) | `ContentBrowserPanel.cpp:430-469` | `TreeNodeEx` recursivo, sin caché, re-escanea disco cada frame en los nodos abiertos |
| Grid de assets (panel der.) | `ContentBrowserPanel.cpp:538-697` | Icono de color + etiqueta de 3 letras por extensión, `ImGui::Button` de 56×56 |
| Selección de carpeta | `m_currentDir` (string), un solo directorio a la vez | `ContentBrowserPanel.h:70` |
| Drag&drop de asset **dentro** del editor | `ContentBrowserPanel.cpp:672-679` (`DT_ASSET_PATH`/`DT_ASSET_DIR`), consumido en `PropertiesPanel.cpp:480-484, 7954-7959, 8687-8692` | Solo origen-editor→destino-editor; el payload es el `path` en memoria |
| Importación desde disco externo | 18 diálogos `IGFD::FileDialog` en `PropertiesPanel.cpp` (Add Mesh, Add Audio, Texture, Font, 13 atlas de UI) | Abren `Browse...`, filtran por `canAcceptAsset` (`PropertiesPanel.cpp:75-99`) |
| Veto de origen del asset | `canAcceptAsset`, `PropertiesPanel.cpp:75-99` | Acepta si está dentro de `ctx.project` o del workspace compartido del motor; **si no, lo rechaza con un log y no copia nada** |
| Doble-clic en `.lua` / `.json` | `ContentBrowserPanel.cpp:636-665` | Abre en Script Editor / carga escena (con modal de guardado si hay cambios) |

## 2. Tabla de hallazgos (usabilidad)

Severidad: **Alta** (bloquea o distorsiona el flujo diario de trabajar con
assets), **Media** (fricción recurrente, hay rodeo manual), **Baja** (pulido).

| ID | Severidad | fichero:línea | Qué falta | Por qué importa |
|---|---|---|---|---|
| U1 | **Alta** | `PropertiesPanel.cpp:88-98` (`canAcceptAsset`) | **No hay importación real de un fichero externo al proyecto.** Un asset fuera de `ctx.project` o del workspace del motor se **rechaza** (log "Asset de otro proyecto, rechazado"), nunca se copia a `assets/`. El único "import" que existe es que el fichero ya viviera dentro del árbol del proyecto de antemano. | El flujo real de trabajo — traer un FBX o una textura nueva desde el Explorador de Windows — no tiene camino. Hoy el usuario tiene que copiar el fichero a mano dentro de la carpeta del proyecto con el Explorador *antes* de que el editor lo vea, y solo entonces aparece en el grid. |
| U2 | **Alta** | `ContentBrowserPanel.cpp:600-614` | **No hay drag&drop desde fuera del proceso (Explorer → editor).** No existe ningún `glfwSetDropCallback`/`WM_DROPFILES` en todo el repo (comprobado por grep en `engine/` y raíz). El único drag&drop es interno: grid del Content Browser → otro panel del propio editor. | Es la interacción más básica de un asset browser tipo Unity/Unreal (arrastrar un `.png` desde el escritorio a la ventana). Su ausencia total, combinada con U1, hace que "traer un asset nuevo" sea 100% manual fuera del editor. |
| U3 | **Media** | `ContentBrowserPanel.cpp:600-614` | Los iconos son un `ImGui::Button` de color sólido + 3 letras (`"3D"`, `"SFX"`, `"IMG"`, `"SPV"`, `"DIR"`, `"..."`). No hay preview real de ninguna textura, modelo o material. | Con una carpeta de 30 texturas es imposible distinguir cuál es cuál sin abrir cada una; en Unity la miniatura ES la información de trabajo diario al organizar assets. |
| U4 | **Media** | `ContentBrowserPanel.cpp:493-534` | No hay breadcrumb ni navegación "subir un nivel"; solo el árbol de la izquierda. Tampoco hay crear/renombrar carpeta desde el propio panel (rename/delete de carpeta sí existen vía right-click en el grid, pero **crear** una carpeta nueva no tiene ningún camino en la UI). | Crear una carpeta de organización ("Textures/Characters/") obliga a salir al Explorador de Windows; rompe el flujo cuando se está en medio de organizar assets recién importados. |
| U5 | **Media** | Todo el fichero — no hay `BeginPopupContextWindow` sobre el área vacía del grid, solo `BeginPopupContextItem` por asset (`:681-688`) | No existe menú "Create > ..." al hacer click derecho en vacío (ni para carpeta, ni para script Lua, ni para nada). Comparar con `ScenePanel.cpp:137` (`BeginPopupContextWindow("##SceneContext"...)`), que sí lo tiene para GameObjects — el patrón ya existe en el editor, solo no en este panel. | Es el punto de entrada estándar para crear cosas nuevas sin salir de la ventana; su ausencia es inconsistente con el resto del editor (Scene sí lo ofrece). |
| U6 | **Media** | Todo el fichero — no hay ningún `ImGuiTextFilter`/`InputTextWithHint` de filtro | No hay búsqueda ni filtro por nombre ni por tipo de asset en el grid. | Un proyecto con cientos de assets en una sola carpeta (o buscando "todas las texturas normal map") no tiene forma de acotar la vista salvo navegar carpeta por carpeta. |
| U7 | **Baja** | `ContentBrowserPanel.cpp:594-696` | Solo se puede tener un asset "activo" a la vez (click = selección + posible acción inmediata); no hay Ctrl/Shift-click ni selección múltiple, así que no se puede arrastrar ni borrar/mover varios assets de una vez. | Mover 20 texturas a una subcarpeta nueva exige repetir la operación una a una (y como no hay "mover" explícito, ni siquiera eso: solo rename dentro del mismo padre). |
| U8 | **Media** | No existe ningún ficheros de "sidecar" ni estructura equivalente en el repo (comprobado: no hay `.meta`, `.import` ni tabla de ajustes por asset) | No hay panel de "import settings" por asset (compresión de textura, escala de import de modelo, generar mipmaps, etc.). | Cada textura se sube tal cual a la GPU sin ningún control desde el editor; cualquier ajuste de import hoy exigiría re-exportar el fichero de origen fuera del editor. Nota: esto es más una limitación del pipeline de carga (`AsyncAssetLoader`/`ModelLoader`) que del panel — el panel no podría ofrecer esto sin que el core primero soporte guardar esos ajustes en alguna parte. |
| U9 | **Alta** | Todo el fichero — no hay ningún `FileWatcher` (comprobado por grep: no existe `ReadDirectoryChangesW`/`inotify`/watcher en `engine/`) | Ningún cambio hecho fuera del editor durante la sesión (copiar un fichero nuevo con el Explorador, o que otro proceso lo escriba) aparece automáticamente. El árbol y el grid **sí** re-escanean cada frame cuando se re-selecciona o se reabre la carpeta (`m_scanned=false` fuerza el rescan), así que **no hace falta reiniciar el editor** — pero tampoco hay refresco espontáneo mientras la carpeta ya está abierta y quieta: hay que forzar un evento (click en otra carpeta y volver, o similar) para que el rescan dispare. | Es el hueco que más golpea el flujo real: "copio un asset con el Explorador mientras tengo el editor abierto" hoy no se refleja solo — hace falta un gesto extra dentro del editor para forzarlo, y no es obvio cuál. |

## 3. Comparación con capacidades típicas de Unity

| # | Capacidad | Estado | Evidencia |
|---|---|---|---|
| 1 | Importación por drag&drop de ficheros externos con detección de tipo | **NO EXISTE** | Sin drop callback OS-level (grep negativo en todo el repo). El único "import" vía diálogo (`IGFD`) **rechaza** ficheros fuera del proyecto en vez de copiarlos (`PropertiesPanel.cpp:88-98`). Ver U1, U2. |
| 2 | Miniaturas/preview reales para texturas, modelos, materiales | **NO EXISTE** | Icono = botón de color sólido + 3 letras por extensión (`ContentBrowserPanel.cpp:600-614`). Ver U3. |
| 3 | Árbol de carpetas navegable + breadcrumb, crear/renombrar/mover in-place | **A MEDIAS** | Árbol: SÍ (`drawFolderTree`). Renombrar/borrar carpeta: SÍ (right-click en el grid, mismo camino que ficheros — `beginAssetRename`/`beginAssetDelete` no distinguen tipo salvo el flag `isDir`). Breadcrumb: NO. Crear carpeta desde la UI: NO. Mover un asset a otra carpeta arrastrándolo dentro del árbol: NO (el `DragDropSource` del grid solo lo consumen paneles externos — Properties, Scene —, no hay `DragDropTarget` en `drawFolderTree` que acepte soltar ahí). Ver U4. |
| 4 | Menú contextual "Create > ..." (material, carpeta, script) | **NO EXISTE** | Sin `BeginPopupContextWindow` en el fichero; patrón sí presente en `ScenePanel.cpp:137` para otro panel. Ver U5. Nota: "Create > Material" no aplicaría hoy tal cual — el Core no tiene un asset de Material independiente (no hay `.mat`, ni serialización de Material fuera de un Mesh/GameObject; confirmado por grep negativo de `saveMaterial`/`MaterialAsset` en el repo) — sería una feature de Core antes que de UI, fuera de alcance de este documento. |
| 5 | Búsqueda/filtro por nombre y por tipo | **NO EXISTE** | Sin `ImGuiTextFilter` ni `InputTextWithHint` en el fichero. Ver U6. |
| 6 | Panel de "import settings" por asset | **NO EXISTE** | Sin sidecar/metadata de asset en todo el repo (grep negativo `.meta`/`.import`). Ver U8 — depende del pipeline de carga, no solo de la UI. |
| 7 | Selección múltiple y arrastre a escena/Inspector | **A MEDIAS** | Arrastre de UN asset a Scene/Properties: SÍ, y funciona bien (payloads tipados `DT_ASSET_PATH`/`DT_ASSET_DIR`, veto centralizado en `canAcceptAsset`). Selección múltiple: NO existe ningún estado de selección plural. Ver U7. |
| 8 | Refresco automático al detectar cambios externos (FileWatcher) | **A MEDIAS** | No hay watcher, pero tampoco hace falta reiniciar el editor: cualquier evento que dispare `m_scanned=false` (cambiar de carpeta y volver, doble-clic) fuerza un rescan fresco de disco. Lo que falta es el refresco *espontáneo* mientras la carpeta actual permanece quieta y visible. Ver U9. |

## 4. Riesgos de alcance detectados al tirar del hilo

- **U1/U2 (importación real)** tocarían, como mínimo, `canAcceptAsset` y el
  punto de entrada de carga (`loadMeshForSelected`/`loadAudioClipForSelected`,
  `PropertiesPanel.cpp:315-370, 411-436`), que a su vez llaman a
  `AsyncAssetLoader` — el pipeline de carga asíncrona de mallas. Copiar un
  fichero de fuera del proyecto a `assets/` antes de encolarlo es una
  operación de disco nueva (no solo de UI) y additionally interactúa con
  `ProjectContext::contains`. Se documenta aquí como candidato a **plan
  propio**, no se toca en esta pasada.
- **U8 (import settings por asset)** no tiene dónde vivir hoy: no hay
  concepto de metadata de asset persistente en el Core (Scene.json guarda
  referencias por path, no ajustes de import). Añadir esto es una decisión de
  formato de datos del Core, más grande que un cambio de panel — **fuera de
  alcance de este documento**, se deja anotado como huella de una feature
  grande.
- **U9 (FileWatcher real)** es contenida: el propio comentario del código
  (`ContentBrowserPanel.cpp:64-65`) ya documenta que el árbol escanea disco
  "para los nodos abiertos: sin caché que invalidar". Un watcher real
  (`ReadDirectoryChangesW` en Windows) sería una pieza nueva sin tocar el
  pipeline de carga — candidato a quick win más grande que 1 fichero, pero
  contenido dentro del panel + una clase nueva de watcher.

## 5. Spec de mejoras priorizadas (Fase 3)

Priorizado por impacto en el flujo real de este proyecto (traer assets
nuevos, organizarlos, reutilizarlos), no por paridad con Unity per se.

### Quick wins (1 fichero, `ContentBrowserPanel.cpp`, bajo riesgo)

- [ ] **Menú "Create > Folder"** en `BeginPopupContextWindow` sobre el área
      vacía del grid — `std::filesystem::create_directory` + refresco
      (`m_scanned=false`). Mismo patrón que `ScenePanel.cpp:137`. (U5, U4)
- [ ] **Filtro de texto** (`ImGuiTextFilter`) sobre `m_assets` antes de
      pintar el grid, por nombre. (U6)
- [ ] **Filtro por tipo** (dropdown: Todos / 3D / Audio / Imagen / Script)
      reutilizando la misma clasificación de extensión que ya calcula el
      color/etiqueta del icono (`ContentBrowserPanel.cpp:600-614}`). (U6)
- [ ] **Breadcrumb** encima del grid, construido a partir de
      `std::filesystem::relative(m_currentDir, m_projectRoot)`, con cada
      segmento clicable para saltar `m_currentDir` sin pasar por el árbol.
      (U4)
- [ ] **Selección múltiple** (Ctrl/Shift-click) sobre `m_assets`, aunque solo
      sea para habilitar un "Delete" agrupado — no requiere cambiar el
      formato del payload de drag&drop de un solo asset. (U7)

### Features medianas (varios métodos del mismo fichero, o +1 fichero pequeño)

- [ ] **Drag&drop desde el árbol al grid** (mover un asset entre carpetas
      arrastrándolo) — el `DragDropSource` del grid ya existe
      (`ContentBrowserPanel.cpp:672-679`); falta un `DragDropTarget` en cada
      nodo de `drawFolderTree` que acepte `DT_ASSET_PATH`/`DT_ASSET_DIR` y
      haga `std::filesystem::rename` + reutilice
      `updateSceneReferencesForRename` (ya existe, la usa el rename por
      nombre). (U4)
- [ ] **Miniaturas reales para texturas** (`.png`/`.jpg`/`.tga`): decodificar
      con la misma ruta de carga que ya usa el material (`stb_image`, ya es
      dependencia existente — no añade una nueva), a una textura pequeña de
      preview, cacheada por path+mtime para no releer cada frame. Empezar
      solo por texturas (el caso de mayor impacto real: distinguir 30 PNG a
      simple vista); modelos/materiales quedan para una iteración posterior.
      (U3)
- [ ] **FileWatcher real** (Windows: `ReadDirectoryChangesW`, ya que el
      proyecto es Win32/D3D12 primero y Vulkan; en Linux —ya portado, ver
      memoria `linux_port_status`— sería `inotify`) sobre `m_projectRoot`,
      marcando `m_scanned=false` cuando dispare, sin librería de terceros.
      Reemplazaría el rescan-por-gesto actual por uno espontáneo. (U9)

### Features grandes (necesitan plan propio, spec/brainstorm previo)

- [ ] **Importación real de ficheros externos** (drag&drop desde Explorer +
      "copiar al proyecto" desde el diálogo Browse en vez de rechazar):
      toca `canAcceptAsset`, el punto de entrada del `AsyncAssetLoader`, y
      probablemente un callback OS-level nuevo (`glfwSetDropCallback` o
      equivalente) que hoy no existe en ningún sitio del repo. Ver riesgo de
      alcance en §4. (U1, U2)
- [ ] **Panel de "import settings" por asset**: requiere que el Core defina
      primero qué es un ajuste de import persistente y dónde vive (¿sidecar
      `.meta` junto al asset? ¿entrada en `project.json`?). Es una decisión
      de formato de datos, no de UI — necesita su propia spec de Core antes
      de tocar el panel. (U8)
- [ ] **Asset de Material independiente** (`.mat` reutilizable, con su propio
      "Create > Material"): hoy Material vive solo embebido en Mesh/GameObject
      (`materialsOf`/`editMaterialsOf`, `ContentBrowserPanel.cpp:78-103`),
      sin serialización propia fuera de una escena. Confirmar primero si el
      proyecto quiere este concepto en el Core — no es una carencia del
      panel, es una capacidad que el Core no tiene todavía, y por la regla de
      "no ocultar lo que el Core soporta" tampoco aplica al revés: no se le
      puede pedir a la UI que ofrezca lo que el Core no tiene.
