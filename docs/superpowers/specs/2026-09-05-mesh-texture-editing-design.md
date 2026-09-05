# Edición de texturas del componente Mesh desde Properties

Fecha: 2026-09-05
Estado: aprobado para plan de implementación

## Objetivo

Asignar, cambiar y quitar las texturas de un GameObject con Mesh desde el panel
Properties, con la misma UX que el resto de asset slots del panel (caja de drop
+ botón "Browse..."). El cambio se ve en el viewport sin reiniciar el editor, en
los dos backends, y sobrevive a guardar y cargar la escena.

## Alcance

Tres slots por material, los tres que el motor soporta hoy: Albedo, Normal Map,
Metallic/Roughness. Los tres se muestran siempre y ninguno se oculta — si el
core soporta N opciones, la UI ofrece N.

Un mesh estático tiene un material (`Mesh::material`). Un `SkinnedMesh` tiene
además `materials`, uno por submalla, indexado por `SubMeshRange::materialIndex`;
se muestran los N, cada uno con sus tres slots.

Fuera de alcance: `metallic` y `roughness` (que hoy tampoco se serializan),
refactor de `Mesh`/`Material`, y unificar las dos copias de `materialsOf()`.

## Estado del código que condiciona el diseño

Verificado en `main` a fecha de hoy:

- `Material` (Material.h) tiene, por slot, una ruta y un vector de bytes
  embebidos: `texturePath`/`embeddedTexture`, `normalMapPath`/`embeddedNormalMap`,
  `metallicRoughnessPath`/`embeddedMetallicRoughness`.
- **La embebida gana a la ruta** en los tres sitios que suben una textura:
  `GpuResources::createTextureImage` (GpuResources.cpp:293), 
  `D3D12Renderer::Impl::uploadMaterialTexture` (D3D12Renderer.cpp:2555) y
  `decodeSlot` (AsyncAssetLoader.cpp:186-188). Es lo contrario de lo que pide
  este diseño.
- **Vulkan comparte la entrada de GPU por clave = geometría + material**
  (`makeSharedMeshKey`, SharedGpuMesh.cpp:26). Los descriptor sets viven en esa
  entrada compartida: mutarla in situ cambiaría la textura de todos los objetos
  que comparten la clave.
- **D3D12 da a cada objeto su propio bloque de descriptores** (`srvBase`) y sus
  propias `baseColorAllocation`/`normalMapAllocation`/`metalRoughAllocation`
  (D3D12Renderer.cpp:9686-9745). Ahí el cambio in situ es directo.
- `rebuildSkinnedMesh(index, mesh)` ya existe en los dos backends y reconstruye
  conservando índice de render, transform y estado de animación. Para estático
  no hay equivalente.
- `replaceStaticTextureWithMissing(renderIndex, slot)` ya hace, en los dos
  backends, la mitad del trabajo: sustituir la imagen de un slot y reescribir el
  descriptor. Lo que le falta es cargar una ruta real en vez del relleno.
- `applyLoadedMesh` (AsyncAssetLoader.cpp:312) registra la malla en el renderer
  y luego hace `setMesh`. `buildResultFor` hace **copia profunda por target**:
  ningún GameObject comparte `shared_ptr<Mesh>` con otro, así que un override no
  puede pisar a un objeto vecino.
- El payload `DT_ASSET_PATH` del Content Browser es `entry.path()` bajo
  `m_projectRoot = ctx.project->root()`, o sea **absoluto**. `Mesh::sourcePath`
  ya se guarda absoluto cuando entra por drag&drop y relativo cuando entra por
  código: el .scene ya es mixto hoy.
- `nodeToJson` (Scene.cpp:687) **no** serializa ninguna ruta de material. Está
  documentado como limitación en ContentBrowserPanel.cpp:202.
- `Scene` no conoce la raíz del proyecto: `toJson()` no recibe nada, y `Scene`
  vive en Core, que no puede depender de `ProjectContext` (Editor).
- `drawAssetDropBox(ctx, idSuffix, hint, onBrowse, onDrop)`
  (PropertiesPanel.cpp:347) implementa el patrón pedido y respeta
  `ctx.editingLocked`. Se reutiliza tal cual.

## Decisiones tomadas

1. **Re-subida a GPU: `rebuildStaticMesh`**, un pure virtual nuevo en
   `EditorRenderer`, simétrico al `rebuildSkinnedMesh` que ya existe.
   Descartadas: `remove+add` (mueve el `staticRenderIndex`, que es el dato que
   guardan los comandos de undo y los mapas del editor) y `setStaticTexture` por
   slot (filtra al interfaz una asimetría que es detalle de un backend, y
   obliga a Vulkan a rehacer la clave compartida tres veces por un cambio de
   tres texturas).
2. **Formato JSON: array `materials` con `index`** dentro del bloque `mesh`.
   Clave ausente = comportamiento actual exacto; las escenas viejas cargan sin
   tocar nada.
3. **Rutas relativas a la raíz del proyecto** cuando caen bajo ella, absolutas
   si no. `sourcePath` no se toca.
4. **Una sola fase**, ~19 ficheros. Partirlo deja mitades no verificables: la UI
   sin la API de render no enseña nada en el viewport, y la serialización sin la
   UI no tiene qué guardar.

## Modelo de datos

`GameObject` gana un vector de overrides. `Material` y `Mesh` no se tocan.

```cpp
struct MaterialTextureOverride {
    int         index = 0;   // índice en SkinnedMesh::materials; 0 = Mesh::material
    std::string albedo, normal, orm;              // "" = sin override. Se serializa.
    std::string baseAlbedo, baseNormal, baseOrm;  // lo que traía el FBX. NO se serializa.
};
```

El `Material` sigue siendo la única fuente que leen los uploaders: el override se
aplica **encima** de él.

Qué significa `index`, sin ambigüedad: si el mesh es un `SkinnedMesh` con
`materials` no vacío, los índices son los de ese vector y `Mesh::material` no se
toca — es el heredado que ese camino no usa. En cualquier otro caso (estático, o
skinned con `materials` vacío) el único índice válido es 0 y apunta a
`Mesh::material`.

`base*` se captura la primera vez que se pisa ese slot y es lo que restaura
**Clear** en caliente. No va al JSON porque al recargar la escena el `Material`
se re-deriva del FBX y el baseline se vuelve a capturar solo. Sin él, Clear no
tendría a qué volver: el valor del FBX ya se habría perdido al escribir el
override encima.

Semántica de Clear: quitar la capa, o sea volver a lo que trajera el FBX — una
ruta externa si la traía, o la textura embebida si lo que traía era bytes. Con la
precedencia invertida (abajo), quitar la ruta hace que la embebida vuelva a verse
sola.

### Aplicación de los overrides

Una función libre, un solo sitio del que salen las tres llamadas:

```cpp
void applyMaterialOverrides(GameObject& go);  // escribe en los Material del mesh
```

Se llama desde:

1. El panel, al asignar o limpiar (camino en caliente).
2. `applyLoadedMesh`, **antes** de `addStaticMesh`/`addSkinnedMesh`: la malla
   recién llegada trae el material del FBX y hay que pisarlo antes de subirlo,
   no después.
3. El lector síncrono de `Scene::fromJson`, en el mismo punto respecto a
   `setMesh`.

Un `index` que ya no existe (el FBX se reexportó con menos submallas) se ignora
con aviso por `Scene::lastWarnings()`, como el resto de incoherencias del lector.

## UI (PropertiesPanel)

Dentro de la sección Mesh ya existente (`drawMeshSection`), un
`CollapsingHeader("Textures")`. Sin Add-gate: no es un componente nuevo, es parte
del Mesh que ya está.

Por cada material, un bloque etiquetado `Material 0`, `Material 1`… envuelto en
`ImGui::PushID(index)`. El PushID no es opcional: `CollapsingHeader` no abre
scope de ID propio, y tres slots con el mismo nombre repetidos en N materiales
colisionarían entre sí.

Por slot:

- Nombre del slot y ruta actual, o `None` si está vacía.
- `drawAssetDropBox` reutilizado, con `idSuffix` que incluya índice de material y
  slot.
- Botón `Clear`, deshabilitado si ese slot no tiene override.

Extensiones aceptadas: las de imagen que ya lista `kDraggableExt`
(`.png .jpg .jpeg .bmp .tga`). Otra extensión deja un mensaje de error en el
panel, como hace el mesh.

Todo respeta `ctx.editingLocked`, que ya viene de `drawAssetDropBox`.

## Camino a GPU

### API nueva

```cpp
// EditorRenderer
virtual void rebuildStaticMesh(int index, const Mesh& mesh) = 0;
```

Contrato: el índice de render **no se mueve**; transform, visibilidad y SSR se
conservan. Mismo contrato que `rebuildSkinnedMesh`, que es de donde sale el
nombre.

### Vulkan

- `refs == 1` (caso normal, un objeto con su malla): mutación in situ de la
  imagen del slot + reescritura del descriptor + **re-clave** de la entrada en
  `SharedGpuMeshCache`. Cero bytes de geometría. Es el camino que ya recorre
  `replaceStaticTextureWithMissing`, con una ruta real en vez del damero:
  encolar los handles viejos en `m_deferredDeletes`, respetar
  `isSharedPlaceholder` para no destruir una imagen prestada, y `vkDeviceWaitIdle`
  antes de escribir el set (los sets no piden `UPDATE_AFTER_BIND`).
- `refs > 1`: release de la entrada compartida + acquire con la clave nueva. Se
  re-sube la geometría de esa malla una vez, y es lo correcto: el objeto se está
  separando del grupo.
- `SharedGpuMeshCache` gana un método de re-clave (borrar la entrada del mapa e
  insertarla con la clave nueva, sin tocar refs ni el slot).

### D3D12

Siempre in situ, nunca toca geometría: `waitForGpu`, soltar la allocation vieja
del slot, `uploadMaterialTexture` sobre `srvBase + n`, `++materialVariant` y
`drawGroupsDirty = true` (su bloque deja de decir lo mismo que el de los que
comparten malla, así que deja de poder compartir draw con ellos).

Cuidado con el caso `reusa` (objeto que comparte malla con un dueño): hay que
comprobar de quién es la allocation antes de soltarla. Aplica la regla del
proyecto sobre guardas de recursos prestados: preguntar qué hay en el destino y
fallar en cerrado, no enumerar sitios prohibidos.

### Skinned

`rebuildSkinnedMesh(index, mesh)`, que ya existe en los dos backends. Es caro
(espera a la GPU y re-sube el personaje entero), y da igual: corre una vez por
cambio hecho a mano.

### Inversión de precedencia

En los tres sitios (`GpuResources::createTextureImage`,
`Impl::uploadMaterialTexture`, `decodeSlot`) pasa a ganar la ruta sobre los bytes
embebidos. Hoy es al revés.

Sin esto el requisito "la ruta explícita pisa a la embebida" solo se podría
cumplir vaciando `embeddedTexture`, lo que destruiría la embebida y haría
imposible el Clear. El cambio solo altera el comportamiento cuando los dos
campos están llenos a la vez, que es exactamente el caso nuevo que introduce
esta feature.

## Serialización

Dentro del bloque `mesh`:

```json
"mesh": {
  "sourcePath": "assets/models/hero.fbx",
  "skinned": true,
  "materials": [
    { "index": 0, "albedo": "assets/tex/body.png" },
    { "index": 2, "albedo": "assets/tex/hair.png", "normal": "assets/tex/hair_n.png" }
  ]
}
```

Solo se escriben los materiales con al menos un override, y dentro de cada uno
solo las claves no vacías. `orm` y no `metallicRoughness` por brevedad; queda
documentado aquí.

Compatibilidad: la clave `materials` ausente significa "sin overrides", que es el
comportamiento de hoy. Un `materials` que no sea array, o una entrada sin `index`
numérico, se ignora con aviso — media configuración es peor que ninguna, mismo
criterio que `jsonToMat4` con la matriz.

### Rutas relativas

`Scene` gana `setAssetRoot(const std::string&)` y un miembro. Lo fija el editor al
abrir proyecto y el runtime a su directorio de trabajo.

- Al escribir: si la ruta cae bajo la raíz, se guarda relativa con `/`; si no,
  absoluta tal cual.
- Al leer: una ruta relativa se resuelve contra la raíz.
- Raíz vacía = las rutas van y vienen tal cual. Es lo que verán los tests y
  cualquier caller que no la fije, y funciona porque el directorio de trabajo ya
  es la raíz en esos casos.

`sourcePath` no se toca: arreglar el mixto que ya existe es otro trabajo.

## Undo

`MaterialTextureCommand` en `Command.h/.cpp`, con `(ownerId, materialIndex, slot,
before, after)`. Resuelve el GameObject por id en cada aplicación, nunca por
puntero: así sobrevive a un undo de Delete que lo haya reconstruido. `execute()` y
`undo()` escriben el override, llaman a `applyMaterialOverrides` y lanzan el
rebuild que toque (estático o skinned).

## Tests

Fichero nuevo `engine/tests/material_texture_tests.cpp`, enlazado contra
`DonTopoEditor` (los comandos de undo del panel viven ahí), sin GPU. Se ejecutan
desde la raíz del repo.

1. Asignar una textura a un mesh estático deja la ruta en `Material::texturePath`
   y el baseline guardado.
2. Cambiarla por otra conserva el baseline original, no el intermedio.
3. Clear restaura el baseline; si el baseline era vacío y había bytes embebidos,
   la ruta queda vacía y la embebida vuelve a ser lo único que hay.
4. Skinned multi-material: el override de `index` 2 toca `materials[2]` y no
   toca `materials[0]`.
5. Round-trip: guardar y cargar conserva los overrides de todos los índices.
6. Escena sin clave `materials` carga exactamente igual que hoy.
7. `index` fuera de rango: aviso en `lastWarnings()`, sin crash y sin tocar
   ningún material.
8. Relativización: con raíz fijada, una ruta bajo ella se guarda relativa y se
   lee resuelta; una fuera de ella se guarda absoluta.
9. Sin raíz fijada, la ruta va y vuelve idéntica.
10. Undo/redo de una asignación y de un Clear.

Cada guarda nueva se sabotea **de una en una** (parchear, compilar, ejecutar,
revertir) para demostrar que cada test cae por la suya.

## Verificación

1. Compila en Debug y en Release.
2. Suite completa en verde desde la raíz del repo, con la salida pegada.
3. Sabotaje uno a uno de cada guarda nueva.
4. Ruta de re-subida probada en los dos backends. El backend sale del
   `project.json` del último proyecto: para probar el otro hay que cambiarlo y
   abrir el proyecto por la GUI.
5. Verificación visual en el editor: se le pide al usuario, no se da por hecha.

## Ficheros

| Fichero | Qué cambia |
|---|---|
| `engine/include/DonTopo/Core/GameObject.h` | `MaterialTextureOverride`, vector, accesores |
| `engine/include/DonTopo/Core/Scene.h` | `setAssetRoot` + miembro |
| `engine/src/Core/Scene.cpp` | bloque `materials`, rutas relativas, aplicación en el lector síncrono |
| `engine/include/DonTopo/Renderer/EditorRenderer.h` | `rebuildStaticMesh` |
| `engine/include/DonTopo/Renderer/Renderer.h` | declaración |
| `engine/src/Renderer/Renderer.cpp` | implementación Vulkan (fast path + release/acquire) |
| `engine/include/DonTopo/Renderer/D3D12/D3D12Renderer.h` | declaración |
| `engine/src/Renderer/D3D12/D3D12Renderer.cpp` | implementación in situ + precedencia |
| `engine/src/Renderer/GpuResources.cpp` | precedencia ruta sobre embebida |
| `engine/src/Renderer/AsyncAssetLoader.cpp` | precedencia en `decodeSlot` + `applyMaterialOverrides` en `applyLoadedMesh` |
| `engine/include/DonTopo/Renderer/SharedGpuMesh.h` | re-clave |
| `engine/src/Renderer/SharedGpuMesh.cpp` | re-clave |
| `engine/include/DonTopo/Editor/PropertiesPanel.h` | sección Textures |
| `engine/src/Editor/PropertiesPanel.cpp` | UI de la sección |
| `engine/include/DonTopo/Editor/Command.h` | `MaterialTextureCommand` |
| `engine/src/Editor/Command.cpp` | implementación |
| `engine/tests/material_texture_tests.cpp` | tests nuevos |
| `engine/tests/CMakeLists.txt` | target del test |
| `engine/src/Editor/ContentBrowserPanel.cpp` | los comentarios que dicen que estas rutas no se serializan dejan de ser ciertos |

## Riesgos

- **Guardas de recursos prestados en D3D12**: soltar una allocation que en
  realidad es del dueño de una malla compartida. Es el fallo que ya se ha dado
  antes en este motor y no lo delata ninguna validación.
- **Descriptor sets en vuelo en Vulkan**: escribir un set que un command buffer
  todavía tiene bindeado es uso inválido aunque los recursos aguanten. El
  `vkDeviceWaitIdle` va antes de escribir el set, no antes de crear la imagen.
- **Re-clave de `SharedGpuMeshCache`**: dejar el mapa y el vector de entradas
  descolocados devolvería una malla ajena. El free-list son pocas líneas; el
  coste está en los consumidores del índice.
- **`game.log` no captura la validación de Vulkan** (va a stderr): un error de
  validación en este camino no aparecerá en el log.
