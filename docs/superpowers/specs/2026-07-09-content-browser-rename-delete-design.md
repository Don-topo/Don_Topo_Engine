# Diseño: Rename y Delete de assets en Content Browser

## Objetivo

Permitir click derecho sobre un fichero o carpeta del Content Browser
(panel inferior izquierdo) y ejecutar dos acciones: **Rename** y **Delete**.
Ambas deben actualizar cualquier referencia que la escena tenga a ese
recurso (mesh, audio clip, texturas de material) — nunca dejar un
GameObject apuntando a un path que ya no existe o que cambió de nombre.

## Contexto actual

- `EditorUI::drawContentBrowser` (`engine/src/EditorUI.cpp:1271-1379`)
  dibuja dos paneles: izquierdo (`IGFD::FileDialog::Instance()`, navegación
  embebida, `ImGuiFileDialogFlags_NoDialog`) y derecho (grid de iconos
  propio, construido desde `m_assets`, que hoy solo incluye
  `e.is_regular_file()` — las subcarpetas no aparecen ahí).
- La librería `ImGuiFileDialog` (FetchContent, `build-ninja/_deps/`, no
  versionada en el repo) no expone ningún hook de right-click por fila —
  cualquier interacción custom debe vivir en el panel derecho, que es
  código propio.
- **Ningún path de mesh se persiste hoy.** `Mesh`
  (`engine/include/DonTopo/Mesh.h:9-17`) no tiene campo de path;
  `EditorUI::loadMeshForSelected` (`engine/src/EditorUI.cpp:325-349`)
  descarta el `path` recibido tras `ModelLoader::load(path)`. `SkinnedMesh`
  hereda de `Mesh` (`engine/include/DonTopo/SkinnedMesh.h:54`), así que un
  campo en la base cubre ambos casos.
- `AudioClipComponent` sí guarda el path (`m_path`,
  `engine/include/DonTopo/AudioClipComponent.h:36`), con getter
  `getPath()` (línea 30) pero sin setter.
- `Material` (`engine/include/DonTopo/Material.h:8-18`) guarda
  `texturePath` / `normalMapPath` / `metallicRoughnessPath`, resueltos
  relativos a la carpeta del FBX en `ModelLoader.cpp:97,345`. `Mesh` tiene
  un único `Material material`; `SkinnedMesh` añade
  `std::vector<Material> materials` (un submaterial por submesh).
- `GameObject::traverse(fn)` (`engine/include/DonTopo/GameObject.h:59-64`)
  hace DFS pre-order desde cualquier nodo — es el mecanismo para recorrer
  toda la escena buscando referencias a un path dado.
- `Renderer::removeStaticObject(int)` / `removeSkinnedObject(int)`
  (`engine/include/DonTopo/Renderer.h:207-208`) ya existen y desregistran
  el render object de `m_objects`/estructuras skinned sin tocar el
  `GameObject` dueño — patrón reutilizable para "quitar mesh sin borrar el
  GameObject".
- Los descriptor sets de textura (`RenderObject::descriptorSets[2]`,
  `engine/include/DonTopo/Renderer.h:92`, y `SkinnedMatGfx::descSets[2]`,
  línea 111) se llenan una vez con `vkUpdateDescriptorSets` en
  `allocateObjectDescriptorSet` (`Renderer.cpp:1117-1180`) y en el bucle de
  `addSkinnedMesh` (`Renderer.cpp:1971-1998`) — nada impide volver a
  llamar `vkUpdateDescriptorSets` más tarde sobre el mismo descriptor set
  ya asignado, mientras se sincronice con `vkDeviceWaitIdle` antes.
- `GpuResources::createTextureImage(path, embedded, img, mem)`
  (`engine/src/GpuResources.cpp:183-244`) ya genera un checkerboard gris de
  fallback (líneas 197-214) cuando `path` está vacío y no hay bytes
  embebidos — es el generador que reutilizamos para "textura missing".
- `EditorUI::beginRename` / popup `"Rename GameObject"`
  (`engine/src/EditorUI.cpp:267-313`) es el patrón de UX a replicar:
  `BeginPopupModal` + `InputText` con `ImGuiInputTextFlags_EnterReturnsTrue`
  + botones Accept/Cancel.

## Arquitectura

### 1. Datos nuevos

- `Mesh::sourcePath` (`std::string`, `engine/include/DonTopo/Mesh.h`):
  path del `.fbx` de origen, vacío para meshes procedurales (Cube/Sphere/
  Plane/Capsule vía `createBasicShape`). Se asigna en
  `EditorUI::loadMeshForSelected` justo después de `ModelLoader::load`:
  `mesh->sourcePath = path;`.
- `AudioClipComponent::setPath(const std::string&)`
  (`engine/include/DonTopo/AudioClipComponent.h`): setter nuevo junto al
  `getPath()` existente. Solo actualiza el string — el sonido FMOD ya
  cargado no cambia de contenido, solo de bookkeeping.

### 2. Grid derecho: incluir carpetas

En `drawContentBrowser` (`engine/src/EditorUI.cpp:1316-1324`), el scan que
alimenta `m_assets` deja de filtrar solo `is_regular_file()`: añade también
`is_directory()`. El render de cada celda distingue carpeta (icono/color
propio, label `"DIR"`, sin lógica de extensión) vs fichero (comportamiento
actual). Doble-click sobre una celda de carpeta hace que el panel IGFD
navegue a ella (`IGFD::FileDialog::Instance()`'s `SetCurrentPath` no es
público — en su lugar, cerrar+reabrir el diálogo con `cfg.path` apuntando a
esa carpeta, mismo mecanismo que el clamp de raíz ya implementado).

### 3. Menú contextual (right-click)

Tras dibujar cada botón de celda (fichero o carpeta), envolver con
`ImGui::BeginPopupContextItem()`:

```cpp
if (ImGui::BeginPopupContextItem())
{
    if (ImGui::MenuItem("Rename")) beginAssetRename(path, isDir);
    if (ImGui::MenuItem("Delete")) beginAssetDelete(path, isDir);
    ImGui::EndPopup();
}
```

Nuevo estado en `EditorUI.h` (junto a los campos de Content Browser
existentes):

```cpp
// Asset rename
std::filesystem::path m_assetRenameTarget;
bool                   m_assetRenameIsDir = false;
char                   m_assetRenameBuffer[128] = {};
bool                   m_openAssetRenamePopup = false;
// Asset delete
std::filesystem::path m_assetDeleteTarget;
bool                   m_assetDeleteIsDir = false;
bool                   m_openAssetDeletePopup = false;
```

### 4. Rename

`beginAssetRename(path, isDir)`: guarda `m_assetRenameTarget`/
`m_assetRenameIsDir`, precarga `m_assetRenameBuffer` con el *stem* (ficheros)
o nombre completo (carpetas), activa `m_openAssetRenamePopup`.

Popup modal `"Rename Asset"` (mismo patrón que `"Rename GameObject"`,
dibujado una vez por frame en `drawContentBrowser`):

1. `InputText` sobre `m_assetRenameBuffer`. La extensión (si es fichero) se
   muestra como sufijo fijo no editable al lado del input, no forma parte
   del buffer.
2. Validación al aceptar (`isValidFileName`, nueva función local — mismo
   estilo que `isValidGameObjectName`): no vacío tras trim, sin
   `\ / : * ? " < > |`. Si falla, error inline, popup no se cierra.
3. Colisión: `parentPath / (nuevoStem + extensión)` — si `exists()` y es
   distinto del path original, error inline ("ya existe"), no se cierra.
4. `std::filesystem::rename(oldPath, newPath, ec)` — si falla, muestra
   `ec.message()`, no se cierra.
5. Si éxito: `updateSceneReferencesForRename(sceneRoot, oldPath, newPath, isDir)`
   (nueva función, ver más abajo), `m_scanned = false`, cierra IGFD
   (`m_dlgOpen = false`) para forzar refresco, cierra popup.

**`updateSceneReferencesForRename(root, oldPath, newPath, isDir)`** —
`root->traverse([&](GameObject* go) { ... })`:

- Si `!isDir`: comparación exacta (`std::filesystem::equivalent` o
  comparación de paths canonicalizados) contra `oldPath`.
- Si `isDir`: comparación de prefijo — el path del recurso empieza por
  `oldPath` + separador; el nuevo path sustituye ese prefijo por
  `newPath`.
- Por cada `go`:
  - `go->getMesh() && go->getMesh()->sourcePath` matchea → reescribe
    `sourcePath` (y, si aplica, `material.texturePath` /
    `normalMapPath` / `metallicRoughnessPath` si alguno matchea el mismo
    criterio — un rename de carpeta puede mover el FBX y sus texturas a la
    vez).
  - `go->getAudioClip() && go->getAudioClip()->getPath()` matchea →
    `go->getAudioClip()->setPath(nuevoPath)`.
  - Si `go->isSkinned()`, iterar también `getSkinnedMesh()->materials` para
    los 3 campos de path de cada submaterial.

### 5. Delete

`beginAssetDelete(path, isDir)`: guarda `m_assetDeleteTarget`/
`m_assetDeleteIsDir`, activa `m_openAssetDeletePopup`.

Popup modal `"Delete Asset"`:

1. Antes de abrir, se cuenta cuántos `GameObject` referencian el recurso
   (mismo criterio de matching que en rename) para mostrar
   `"¿Borrar 'nombre'? N objeto(s) lo usan y perderán la referencia."` (o
   sin la segunda frase si `N == 0`).
2. Botones **Borrar** / **Cancelar**. Borrar ejecuta, en orden:
   - `detachSceneReferencesForDelete(sceneRoot, path, isDir)` (ver abajo).
   - `std::filesystem::remove(path)` (fichero) o `remove_all(path)`
     (carpeta).
   - `m_scanned = false`, `m_dlgOpen = false`.

**`detachSceneReferencesForDelete(root, path, isDir)`** — mismo criterio de
matching (exacto o prefijo) que rename, por cada `go` que matchea:

- **Mesh**: si `go->staticRenderIndex >= 0` →
  `m_renderer->removeStaticObject(go->staticRenderIndex)`; si
  `go->skinnedRenderIndex >= 0` → `m_renderer->removeSkinnedObject(...)`.
  Luego `go->setMesh(nullptr)`, ambos índices a `-1`.
- **Audio**: `go->setAudioClip(nullptr)`.
- **Textura** (el path borrado matchea `texturePath`/`normalMapPath`/
  `metallicRoughnessPath` de un mesh que **sigue teniendo su propio
  `sourcePath` intacto** — o sea, se borró solo la textura, no el modelo):
  llamar al método nuevo de Renderer (sección 6) para hacer hot-swap a
  checkerboard, luego limpiar el campo de `Material` a `""`.

Nota: si se borra el `.fbx` mismo (mesh completo), no hace falta tratar sus
texturas por separado — el mesh entero se desengancha.

### 6. Hot-swap de textura a "missing" en `Renderer`

Nuevos métodos públicos:

```cpp
enum class TextureSlot { Diffuse, Normal, MetallicRoughness };
void replaceStaticTextureWithMissing(int renderIndex, TextureSlot slot);
void replaceSkinnedTextureWithMissing(int renderIndex, int submeshIndex, TextureSlot slot);
```

Implementación (`Renderer.cpp`), para el caso estático:

1. `vkDeviceWaitIdle(m_device)` — evita tocar un descriptor set en vuelo.
2. Localizar `RenderObject& obj = m_objects[renderIndex]`.
3. Destruir la imagen/view/sampler del slot afectado
   (`vkDestroySampler`/`vkDestroyImageView`/`vkDestroyImage`/`vkFreeMemory`
   sobre los campos correspondientes según `slot`).
4. `m_res.createTextureImage("", {}, nuevaImg, nuevaMem)` (dispara el
   fallback checkerboard existente) → `createTextureImageView` →
   `createTextureSampler`.
5. `vkUpdateDescriptorSets` sobre `obj.descriptorSets[0]` y `[1]`
   (`MAX_FRAMES`), mismo binding que usa ese slot en
   `allocateObjectDescriptorSet` (1=diffuse, 2=normal, 4=ORM).

Caso skinned equivalente sobre la estructura `SkinnedMatGfx` del
submaterial `submeshIndex` dentro del modelo en `renderIndex` — la
implementación exacta de cómo `addSkinnedMesh` indexa sus
`SkinnedMatGfx` por submesh se revisa en el plan de implementación (no
hay API pública hoy para listarlos; es detalle interno de `Renderer.cpp`).

### 7. Refresco tras rename/delete

Ambos flujos terminan con `m_scanned = false` (repuebla `m_assets`) y
`m_dlgOpen = false` (fuerza que el panel IGFD izquierdo reabra y relea el
directorio actual — mismo mecanismo ya usado por el clamp de raíz de
proyecto).

## Fuera de alcance

- Menú contextual en el panel izquierdo (lista nativa de IGFD) — sin hook
  de extensión disponible sin vendorizar la librería; toda acción vive en
  el grid derecho (ver Contexto actual).
- Selección múltiple / rename-delete en batch — una acción, un recurso a
  la vez.
- Undo de rename/delete.
- Cascada hacia shaders (`shaders/*.spv`) o Skybox — sus paths están
  hardcodeados en `sandbox/src/main.cpp`, no son "assets" gestionados por
  el Content Browser.
- Detectar y limpiar paths huérfanos preexistentes (de sesiones anteriores
  a esta feature) — solo se actualizan referencias mientras la escena está
  cargada en memoria durante el rename/delete.
- Renombrar/borrar mientras el fichero está siendo reproducido (audio) o
  en medio de un drag&drop activo — no hay guard adicional más allá del
  que ya existe (el popup modal bloquea el resto de la UI mientras está
  abierto).

## Plan de verificación manual

1. Rename de un `.fbx` sin uso en la escena — el fichero cambia de nombre
   en disco, sin errores.
2. Rename de un `.fbx` que un GameObject tiene cargado como mesh — tras
   aceptar, el modelo sigue viéndose igual en el viewport (no se recarga
   geometría), y un segundo rename posterior sobre el mismo GameObject
   sigue encontrándolo (confirma que `sourcePath` se actualizó).
3. Rename de una carpeta que contiene el `.fbx`/audio en uso — mismo
   resultado, prefijo de path actualizado.
4. Intentar rename a un nombre ya existente en la misma carpeta — error
   inline, no se cierra el popup, disco sin cambios.
5. Delete de un audio en uso — el `AudioClipComponent` desaparece del
   GameObject (sección Properties dejar de mostrarlo como asignado),
   fichero borrado de disco.
6. Delete de un `.fbx` en uso — el modelo desaparece del viewport, el
   GameObject sigue existiendo (sigue en Scene panel), sección Mesh vuelve
   a mostrar "sin mesh".
7. Delete de una textura (`.png`) referenciada por un modelo cargado — el
   modelo pasa a mostrar el checkerboard gris en esa textura al instante,
   sin crash, sin validation errors de Vulkan.
8. Delete de una carpeta completa que contiene varios assets en uso —
   todas las referencias se limpian correctamente, carpeta desaparece de
   disco.
9. Cancelar ambos popups (Rename/Cancel, Delete/Cancelar) — disco y escena
   sin cambios.
10. Build limpio tras los cambios de `Mesh.h`/`AudioClipComponent.h`
    (recordar: borrar `.obj` propios si el rebuild incremental no detecta
    el cambio de header — ver memoria `ninja_stale_header_deps`).
