# Diseño: componente Mesh (estático) desde el editor

## Objetivo

Permitir añadir/eliminar un componente Mesh (modelo FBX estático, sin
skinning) a un GameObject desde el panel Properties del editor ImGui, con dos
vías de entrada (botón "Browse..." con file dialog, o drag&drop desde
Content Browser), sin crashear la aplicación en ningún caso (fichero
inválido, borrado, selección cambiante).

## Contexto actual

- `GameObject` ya tiene el slot de mesh completo: `m_mesh`
  (`GameObject.h:73`) con `setMesh/getMesh/hasMesh` (`GameObject.h:22-24`) y
  `staticRenderIndex` (`GameObject.h:69`) para el registro en el `Renderer`.
  No hace falta tocar `GameObject`.
- `ModelLoader::load(path)` (`ModelLoader.cpp:24`) ya carga FBX estático vía
  Assimp (triangulate/FlipUVs/GenNormals/CalcTangentSpace), con textura
  embebida o por ruta (diffuse/normal/ORM) y lanza `std::runtime_error` si
  Assimp falla (`ModelLoader.cpp:29-32`) — solo lee `scene->mMeshes[0]` (un
  submesh), límite conocido que se mantiene.
- Placeholder checkerboard cuando no hay textura o falla su carga ya existe
  en `GpuResources.cpp:197-212`. Sin cambios.
- `EditorUI::createBasicShape` (`EditorUI.cpp:278-286`) ya es el patrón
  exacto a reusar para registrar un mesh: `addChild` → `addStaticMesh` →
  `setMesh`. Para Mesh no creamos hijo nuevo, se asigna al `m_selected` ya
  existente.
- `Renderer::removeStaticObject(index)` (`Renderer.cpp:2008-2015`) es
  idempotente (`if (vertexBuffer == VK_NULL_HANDLE) return`).
  `Renderer::removeGameObject` (`Renderer.cpp:2026-2038`) hace
  `vkDeviceWaitIdle` antes de destruir buffers — necesario porque hay
  double-buffering y un command buffer en vuelo podría seguir referenciando
  el buffer. Borrar un componente Mesh suelto (sin borrar el GameObject)
  necesita el mismo wait, que hoy no existe fuera de `removeGameObject`.
- Patrón "Add" existente: `drawAddComponentButton` (`EditorUI.cpp:909-952`)
  — guard único de exclusión mutua (`hasAnyCollider()`), popup con
  `Selectable` deshabilitado si ya existe. Mesh no comparte este popup (ver
  sección UI más abajo) — no es mutuamente excluyente con colliders (un
  GameObject puede tener collider + mesh a la vez).
- `drawContentBrowser` (`EditorUI.cpp:954-1048`) ya lista assets con icono
  por tipo (`.fbx` → label "3D", `EditorUI.cpp:1018-1019`) pero no tiene
  ningún drag source hoy. El único drag&drop existente en el editor es
  reorder de jerarquía con payload `"DT_GAMEOBJECT"` (`EditorUI.cpp:302-317`).
- `ImGuiFileDialog` ya integrado y en uso embebido (modo `NoDialog`) dentro
  de Content Browser con key `"##ContentDlg"` (`EditorUI.cpp:964-979`). Para
  Add Mesh se abre una clave *distinta* (`"##AddMeshDlg"`, modo modal
  normal, sin `NoDialog`) — pero **`IGFD::FileDialog::Instance()` es un
  singleton real (solo un diálogo activo a la vez, confirmado en el header
  vendorizado)**, no soporta claves concurrentes como se asumió aquí
  originalmente. Requiere coordinación explícita: un flag (`m_meshDlgOpen`)
  que cede el singleton al abrir Add Mesh (`Close()` + `m_dlgOpen = false`)
  y bloquea el reopen de Content Browser mientras esté activo; el drenado
  del diálogo (`Display`/`IsOk`/`Close`) debe ejecutarse cada frame de forma
  incondicional (no solo dentro de la rama sin-mesh de la sección
  Properties), o queda atascado si la selección cambia con el diálogo
  abierto. Ver implementación final en `EditorUI::drawMeshDialog()`.

## Arquitectura

### Helper de carga: `EditorUI::loadMeshForSelected(const std::string& path)`

Único punto de entrada, usado tanto por Browse como por drag&drop:

```cpp
void EditorUI::loadMeshForSelected(const std::string& path)
{
    if (!m_selected || !m_renderer || m_selected->hasMesh()) return;

    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".fbx")
    {
        m_meshLoadError = "Formato no soportado: " + ext;
        return;
    }

    try
    {
        auto mesh = std::make_shared<Mesh>(ModelLoader::load(path));
        m_selected->staticRenderIndex = m_renderer->addStaticMesh(*mesh);
        m_selected->setMesh(std::move(mesh));
        m_meshLoadError.clear();
    }
    catch (const std::exception& e)
    {
        m_meshLoadError = e.what(); // se muestra como texto rojo transitorio en la sección Mesh
    }
}
```

`m_meshLoadError` (nuevo campo `std::string`) persiste hasta el próximo
intento de carga o cambio de selección — se limpia también al cambiar
`m_selected` (mismo punto donde se resincronizan las demás cachés).

### Borrado seguro: `Renderer::removeMeshComponent(GameObject* go)`

Nuevo método público en `Renderer`, análogo a `removeGameObject` pero solo
para el componente Mesh (no borra el GameObject ni sus otros componentes):

```cpp
void Renderer::removeMeshComponent(GameObject* go)
{
    if (!go || !go->hasMesh()) return;
    vkDeviceWaitIdle(m_gpu.device());
    if (go->staticRenderIndex >= 0)
        removeStaticObject(go->staticRenderIndex);
    go->staticRenderIndex = -1;
    go->setMesh(nullptr);
}
```

`EditorUI` lo invoca a través de `m_renderer` (puntero ya existente,
`EditorUI.h:149`) — no requiere wiring nuevo.

## UI — sección "Mesh" en Properties

Nueva función `EditorUI::drawMeshSection()`, llamada desde `drawProperties()`
junto a las secciones de collider (antes de `drawAddComponentButton()`).

**Si `m_selected->hasMesh()`:**
- Header `TreeNodeEx` "Mesh" con botón "x" (mismo patrón visual que
  colliders) → `m_renderer->removeMeshComponent(m_selected)`.
- Texto informativo: nombre del mesh (`m_selected->getMesh()->name`).
- Sin campos editables (no hay transform propio del mesh; usa el Transform
  del GameObject).

**Si NO tiene mesh:** zona dedicada (`ImGui::Button("Browse...")` +
`ImGui::BeginChild` con borde, usado como target de drop) siempre visible al
final de Properties, con texto tipo "Drop .fbx here or Browse...":
- **Browse...**: abre `IGFD::FileDialog` con key `"##AddMeshDlg"`,
  `cfg.path = "assets"`, filtro `".fbx"`. Al confirmar
  (`Display("##AddMeshDlg", ...)` retorna true e `IsOk()`):
  `loadMeshForSelected(GetFilePathName())`, luego `Close()`.
- **Drag&drop target**: `ImGui::BeginDragDropTarget()` sobre el `BeginChild`
  de la zona → `AcceptDragDropPayload("DT_ASSET_PATH")` → payload es
  `std::string` (ruta completa) → `loadMeshForSelected(path)`.
- Si `!m_meshLoadError.empty()`: texto en rojo debajo de la zona con el
  mensaje (p. ej. "Assimp: ..." si el FBX está corrupto).

Nota: no se usa `hasMesh()` como guard en `drawAddComponentButton` porque
Mesh no vive en ese popup — tiene su propia zona siempre visible (a
diferencia de colliders, donde 4 tipos comparten un único punto de entrada).
Esto es intencional: Mesh es unario y no exclusivo con colliders, así que no
pertenece al mismo guard de "Add".

### Drag source en Content Browser

En `drawContentBrowser` (`EditorUI.cpp:1030-1042`), tras el `ImGui::Button`
del icono de asset, si `ext == ".fbx"`:

```cpp
if (ImGui::BeginDragDropSource())
{
    ImGui::SetDragDropPayload("DT_ASSET_PATH", path.string().c_str(), path.string().size() + 1);
    ImGui::Text("%s", fname.c_str());
    ImGui::EndDragDropSource();
}
```

(Otros tipos de asset no son drag source todavía — fuera de alcance.)

## Manejo de errores / crash-safety

- Extensión no `.fbx` → rechazo silencioso con mensaje, sin llamar a Assimp.
- Assimp lanza en fichero corrupto/incompleto → capturado en
  `loadMeshForSelected`, no se modifica `m_selected` ni se registra nada en
  `Renderer` (mesh queda `nullptr`, `hasMesh()` sigue false, zona de drop
  sigue visible).
- `hasMesh()` ya true → `loadMeshForSelected` es no-op (guard al inicio);
  evita sobrescribir un mesh existente sin pasar por remove explícito
  primero (constraint "un Mesh por GameObject").
- Borrado de componente Mesh: `vkDeviceWaitIdle` antes de liberar buffers
  (mismo patrón que `removeGameObject`) evita usar memoria de un buffer en
  vuelo del double-buffering.
- Borrado del GameObject completo (no solo el componente): ya cubierto sin
  cambios por `removeGameObject`/`traverse` existente, que llama
  `removeStaticObject(go->staticRenderIndex)` idempotente.
- Cambio de selección mientras hay error pendiente: `m_meshLoadError` se
  limpia (mismo punto donde se resincronizan `m_propsCachedFor` y demás
  cachés al detectar `m_selected` distinto).

## Fuera de alcance

- Multi-submesh FBX (se mantiene el límite actual: solo `mMeshes[0]`).
- SkinnedMesh / animación — feature aparte, ya existe pipeline paralelo.
- Drag&drop de otros tipos de asset (texturas, audio) al Content Browser.
- Edición de material/textura desde la sección Mesh del Properties.
- Reemplazar un Mesh existente sin pasar por "x" (remove) primero.

## Plan de verificación manual

1. Seleccionar GameObject sin mesh → aparece zona "Drop .fbx here or
   Browse..." al final de Properties.
2. Click "Browse..." → seleccionar un `.fbx` válido en `assets/` → aparece
   en el viewport con su textura (o placeholder si no tiene), sección Mesh
   muestra su nombre.
3. Arrastrar un `.fbx` desde Content Browser hasta la zona Mesh de otro
   GameObject sin mesh → mismo resultado que Browse.
4. Con un GameObject que ya tiene mesh, intentar drop de otro `.fbx` sobre
   la sección Mesh (no la zona de drop, que ya no debería estar visible) →
   no debe reemplazar ni crashear.
5. Seleccionar un fichero `.fbx` corrupto/inválido → mensaje de error rojo
   visible, no crashea, zona de drop sigue disponible para reintentar.
6. Click "x" en sección Mesh → mesh desaparece del viewport, zona de drop
   reaparece, no crashea (frame siguiente y varios después).
7. Añadir Mesh a un GameObject y borrar el GameObject entero (Delete) → no
   crash al borrar ni al cerrar la app.
8. Añadir Mesh + Box Collider al mismo GameObject → ambos coexisten sin
   pelearse (mesh no es parte del guard de exclusión de colliders).
