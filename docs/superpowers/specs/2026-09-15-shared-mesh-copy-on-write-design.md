# Malla compartida con copia al modificar — diseño

Fecha: 2026-09-15. Origen: Apéndice B de `docs/core-audit.md`, reabierto por la
fila 11 de `docs/animation-audit.md`, más A8 de la misma auditoría.

## Problema (medido)

Release, `assets/modelAnimation.fbx` (101.252 vértices, 65 huesos), N=20:

| Qué | Coste |
|---|---|
| `Scene::cloneGameObject` de un skinned | ~19-22 ms por clon |
| solo copiar el `SkinnedMesh` (ablación) | ~22 ms |
| solo `subtreeToJson` (ablación) | 0,006 ms |
| bytes copiados por clon | 11,9 MB de geometría, 148 KB de clips |
| undo de un Delete (`insertFromJson`) | ~230 ms, relee el FBX |

`GameObject` ya guarda la malla como `std::shared_ptr<Mesh>`. Se copia porque
después se ESCRIBE dentro de ella por objeto:

- **Material**: `applyMaterialOverrides` escribe los overrides del objeto en
  `Mesh::material` / `SkinnedMesh::materials` (`GameObject.cpp:96`;
  también `ContentBrowserPanel.cpp:86`, `GameExporter.cpp:56`).
- **Animación**: `addAnimationSource`, `removeAnimationSource`, `renameClip` y
  `applyClipNamesPositionally` modifican `animationClips`/`animationSources`.

**Sospecha sin verificar**: `cloneGameObject` siembra la caché con la malla
VIVA (ya con sus fuentes de animación importadas), `nodeFromJson` la copia y
vuelve a aplicar las fuentes del JSON, así que un clon de un objeto con un FBX
de animación añadido duplicaría clips y releería ese FBX. El test 1 lo decide.

## Decisiones

| Decisión | Elegido | Descartado |
|---|---|---|
| Enfoque | Compartir la malla y copiar solo al modificar, con `const` impuesto por el compilador | Partir `Mesh` en geometría + material por objeto (L, toca los dos backends, serialización y cargador); sacar solo el `Material` (un FBX de animación importado seguiría copiando todo) |
| Cuándo copiar | `use_count() > 1` en el único punto de escritura | Contadores propios o flags de "compartida" |
| A8 | Incluido: el comando de Delete guarda las mallas vivas y el undo las comparte | — |
| Formato `.scene` | Sin cambios | — |

## Diseño

### 1. `GameObject`

- `const std::shared_ptr<const Mesh>& getMesh() const` (antes no `const Mesh`).
- `const SkinnedMesh* getSkinnedMesh() const` (antes puntero no const).
- `Mesh* editMesh()` y `SkinnedMesh* editSkinnedMesh()`: si `m_mesh.use_count() > 1`,
  sustituyen `m_mesh` por una copia (conservando el tipo dinámico: un skinned se
  copia como `SkinnedMesh`) y devuelven la copia; si no, devuelven la misma
  malla. `nullptr` sin malla (y `editSkinnedMesh` también si no es skinned).
- `setMesh` no cambia de contrato (sigue reseteando los baselines de los
  overrides); acepta `std::shared_ptr<Mesh>` y lo guarda como `const`.
- **Única** vía de escritura: el compilador rechaza escribir por `getMesh()`.
- **Coste aceptado**: referencias que no son de otro GameObject (la caché de
  precarga, un comando del undo) también cuentan; producen como mucho una copia
  de más, nunca un resultado incorrecto.

### 2. Material sin copias innecesarias

`applyMaterialOverrides` escribe un campo del material **solo si el valor es
distinto** del que ya tiene la malla; la escritura pasa por `editMesh()`. Clonar
un objeto con overrides no copia (la malla compartida ya los lleva); el primer
override que cambia algo copia solo ese objeto.

### 3. Clon y carga (`nodeFromJson`)

Función pura `bool meshMatchesAnimationConfig(const SkinnedMesh&, const nlohmann::json& animationSources)`:
mismas fuentes en el mismo orden (ruta, builtin) y los mismos nombres de clip
por fuente.

Con una malla de la caché de precarga (`preloaded`):
- **coincide** → se **comparte** el `shared_ptr` sin tocarla (clon, undo);
- **no coincide** → copia (`std::make_shared` de la precargada, como hoy) y
  aplica la configuración.
Sin caché, el camino de disco no cambia.

La malla estática (no skinned) de la caché se comparte siempre: no tiene
configuración de animación.

`PreloadedMeshCache` pasa a `std::unordered_map<std::string, std::shared_ptr<const Mesh>>`.

### 4. A8: undo de Delete sin disco

- `DeleteGameObjectCommand` guarda, al construirse, un `PreloadedMeshCache` con
  las mallas del subárbol (por `sourcePath`, solo las que tienen fichero).
- `Scene::insertFromJson` gana `const PreloadedMeshCache* preloaded = nullptr`
  y lo pasa a `nodeFromJson`. El undo le pasa la caché del comando.

### 5. Sitios que escriben hoy

Pasan a `editMesh()`/`editSkinnedMesh()`: los tres de material (`GameObject.cpp`,
`ContentBrowserPanel.cpp`, `GameExporter.cpp`), el cargador asíncrono,
`AnimationSourceCommand`, `ClipRenameCommand`, el panel del Animator donde
modifique la malla, y los que el compilador señale al pasar a `const`.

### Qué no cambia

El registro en los backends (lee `const Mesh&`, ya deduplica en GPU por
contenido), el formato del `.scene`, los bindings de Lua.

## Tests

En `engine/tests/animator_tests.cpp`, cada uno con su sabotaje de uno en uno:

1. **Sospecha**: clonar un objeto con un FBX de animación añadido deja el mismo
   número de clips que el original. Se ejecuta PRIMERO sobre el código actual;
   si pasa, la sospecha era falsa y se dice.
2. El clon comparte la malla (`clone->getMesh().get() == src->getMesh().get()`).
3. `editSkinnedMesh()` sobre el clon copia: el original no ve el cambio de
   material ni el clip añadido.
4. `editMesh()` sobre una malla única no copia (mismo puntero).
5. Un override igual al valor actual no copia; uno distinto, sí.
6. Malla precargada con configuración de animación distinta a la del JSON: se
   copia y se configura; con la misma: se comparte.
7. Undo de Delete: mismo puntero de malla y sin lectura de disco (FBX temporal
   borrado antes del undo, como `test_clone_of_rigged_mesh_does_not_reread_disk`).
8. Los tests existentes de ida y vuelta, undo y texturas del Mesh siguen verdes.

**Medición** antes/después con el arnés temporal de la fila 11: objetivo clon
< 1 ms y undo de Delete < 10 ms; las cifras reales van a la auditoría.

**Manual** (Vulkan y D3D12): textura por objeto tras clonar (cambiar la del clon
no cambia la del original); fuentes de animación en un clon; `Instantiate` desde
Lua.

## Riesgos

- Un `const Mesh*` guardado antes de un `edit*` sigue apuntando a la malla vieja:
  no puede escribir, pero puede leer datos que ya no son los del objeto. Se
  revisa cada llamante al pasarlo.
- El cargador asíncrono escribe la malla al terminar: el compilador lo señala.
