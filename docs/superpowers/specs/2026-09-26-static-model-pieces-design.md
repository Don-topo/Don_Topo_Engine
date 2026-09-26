# Modelos estáticos de varias piezas — Diseño

Fecha: 2026-09-26. Sale de la limitación anotada en
`2026-09-25-gltf-obj-models-design.md` («un modelo estático solo carga su primera
malla, sin la transformación de su nodo»).

## El fallo

`ModelLoader::load` toma `scene->mMeshes[0]` y descarta la jerarquía de nodos.
Un FBX de un solo objeto no lo notaba. Pero Assimp convierte cada primitive de glTF
en una malla, y los exportadores sacan un objeto por pieza. Por eso una casa `.glb`
con paredes, tejado y ventanas, cada una con su material, se ve solo con su
primera pieza y sin colocar. Un personaje con huesos no tiene el problema:
`loadSkinned` recorre todas las mallas.

## Objetivo

Un modelo **estático** de varias piezas aparece como en Unity: el objeto al que
se añade pasa a ser el padre y tiene **un hijo por pieza**, con el nombre de su
nodo, la transformación de ese nodo y su propio material. Cada pieza se
selecciona, se mueve y se le cambia el material con la UI de hoy. **No se toca
el render de ningún backend.**

Fuera de alcance:
- Reproducir el árbol de nodos del fichero: la jerarquía es **plana**, un hijo
  por aparición de malla.
- Añadir solas las piezas nuevas de un fichero reexportado (ver Reimport).
- Modelos con huesos: siguen entrando enteros por `loadSkinned`.
- Soltar un modelo sobre el viewport o la jerarquía (hoy no existe).

## Decisiones

- **Jerarquía por pieza**, no submallas en un objeto ni malla fusionada. Las
  submallas obligaban a enseñar submallas al render estático de Vulkan y D3D12
  (con dedup, instancing y culling encima). Fusionar perdía los materiales.
- **Las piezas van como hijos del objeto seleccionado**, que se queda sin malla.
  Sin nodo intermedio.
- **Un fichero de una sola pieza se comporta exactamente como hoy**: la malla va
  en el objeto, sin aplicarle la transformación del nodo. Así no cambia el
  aspecto de ninguna escena existente.

## Arquitectura

### Identidad de una pieza

- `Mesh` gana `int piece = 0`: el índice de la malla en el fichero
  (`scene->mMeshes[piece]`).
- La escena lo guarda como `"piece"` dentro de `"mesh"` y lo omite cuando es 0.
  Una escena sin el campo carga la pieza 0, que es lo que carga hoy.
- La transformación de la pieza **no** se hornea en los vértices: vive en el
  `GameObject`. Una malla que aparece en dos nodos son dos hijos con la misma
  `piece` y distinta transformación.

### `ModelLoader`

```cpp
struct ModelPiece
{
    int         piece = 0;        // indice en scene->mMeshes
    std::string name;             // nombre del nodo (o de la malla si el nodo no tiene)
    glm::mat4   transform{1.0f};  // relativa al nodo RAIZ, con la escala del sidecar en la traslacion
};

struct StaticModel
{
    std::vector<Mesh>       meshes;   // una por scene->mMeshes, en orden (meshes[i].piece == i)
    std::vector<ModelPiece> pieces;   // apariciones en los nodos, recorrido en profundidad
};

// Un solo ReadFile: todas las mallas del fichero y donde aparece cada una. Solo
// para modelos SIN huesos (con huesos: loadSkinned). Lanza como load.
static StaticModel loadStatic(const std::string& path);

// La pieza `piece` sola, sin transformacion. load(path) == load(path, 0).
static Mesh load(const std::string& path, int piece);
```

- `load(path)` pasa a ser `load(path, 0)`, sin cambiar su resultado.
- `piece` fuera de rango lanza `std::runtime_error` con la ruta y el índice.
- **Transformación relativa a la raíz.** Es el producto de las transformaciones
  de los nodos desde los hijos de la raíz hasta el nodo de la pieza, **sin** la
  de la raíz. La raíz de un FBX lleva la conversión de unidades (×0,01);
  aplicarla descuadraría un modelo de varias piezas frente a uno de una sola,
  que hoy no la lleva.
- **La escala del sidecar** (`ModelImportSettings::scale`) ya multiplica los
  vértices. Aquí multiplica también la traslación de cada `transform`, para que
  las piezas sigan juntas al escalar.
- Las mallas sin caras (solo líneas o puntos) no generan pieza.

### `AsyncAssetLoader`

- `requestMesh(path, targetId, piece = 0)`. Los waiters siguen agrupados **por
  fichero**: N piezas del mismo fichero son un solo `ReadFile` de Assimp.
- El job de un modelo sin huesos llama a `loadStatic` y da a cada waiter su
  `meshes[piece]`. Con huesos, `loadAuto` como hoy.
- `LoadedMesh` gana `std::vector<ModelPiece> pieces`, relleno cuando el modelo es
  estático y tiene más de una pieza. También gana
  `std::vector<std::shared_ptr<const Mesh>> pieceMeshes` (todas las mallas del
  fichero, `pieceMeshes[i].piece == i`), para crear los hijos sin volver a leer.

### Caché de precarga por pieza (bug latente que destapa esta feature)

`PreloadedMeshCache` indexa por `sourcePath`. La rellenan el undo de Delete
(guarda las mallas vivas del subárbol antes de borrar) y la precarga del runtime,
y la consulta `nodeFromJson`. Con varias piezas del mismo fichero, deshacer el
borrado de «Casa» daría **la misma malla a todos sus hijos**. Por eso:

```cpp
// Clave de PreloadedMeshCache: el sourcePath para la pieza 0 (las caches de hoy
// siguen valiendo) y "<sourcePath>#piece=<n>" para las demas.
std::string meshCacheKey(const std::string& sourcePath, int piece);
```

- `Scene::collect…` (las mallas del subárbol por fichero), `nodeFromJson` y la
  precarga del runtime pasan a usar `meshCacheKey`.
- La precarga del runtime de un modelo estático lee con `loadStatic` y rellena
  una entrada por pieza usada en la escena.

### Editor: Add Mesh

`PropertiesPanel::loadMeshForSelected` no cambia: pide la pieza 0. Cuando el
resultado de una petición **del usuario** llega a `EditorUI::onAssetsLoaded`:
- **`pieces.size() <= 1`**: como hoy (`setMesh` en el seleccionado +
  `MeshComponentCommand`).
- **`pieces.size() > 1`**: no se asigna malla al seleccionado. Se apila **un solo
  paso de undo**, un `CompositeCommand` con un `CreateGameObjectCommand` por
  pieza. Cada uno crea un hijo del seleccionado al final de sus hijos, desde un
  snapshot JSON con:
  - el nombre de la pieza;
  - la transformación local descompuesta en posición, rotación y escala;
  - `mesh {sourcePath, piece}`.

  El snapshot se construye con el mismo serializador de nodos que usa la escena.
  `CreateGameObjectCommand` gana un parámetro opcional `PreloadedMeshCache`
  (copia propia, con `shared_ptr` a las mallas) que pasa a `insertFromJson`. Add
  Mesh le da la malla de la pieza que ya entregó el worker (`pieceMeshes`), así
  que crear, deshacer y rehacer **nunca vuelve a leer el fichero** ni carga nada
  en el hilo principal. Hoy `CreateGameObjectCommand::execute` llama a
  `insertFromJson` sin loader, y un snapshot con malla se cargaría de forma
  síncrona, una vez por hijo.
- Una transformación que `glm::decompose` no puede descomponer (singular, NaN)
  deja esa pieza en identidad, con un aviso en el Log. Es obligatorio comprobar
  el retorno: con una matriz singular `glm::decompose` no escribe sus salidas
  (nota de memoria del repo).

`CompositeCommand` (nuevo, en `Command.h`): una lista de `ICommand`; `execute`
los ejecuta en orden y `undo` los deshace en orden inverso; la etiqueta es la
que se le da («Añadir modelo 'casa' a 'Casa'»).

### Reimport

`reimportModelUsers` agrupa por `sourcePath` y recarga con `loadAuto`. Pasa a
recargar cada objeto con **su** pieza, `load(path, piece)` (una lectura por
fichero vía `loadStatic`).
- Si la pieza ya no existe en el fichero reexportado, el objeto conserva su
  malla anterior y queda un aviso: es el contrato actual cuando una recarga falla.
- Las piezas nuevas no se añaden solas.

### Miniaturas

`loadPreview` de un modelo estático pinta **todas** las apariciones con su
transformación: las posiciones y las normales transformadas (la normal con la
inversa traspuesta). Así el preview enseña el modelo entero. `kThumbDiskVersion`
pasa a 4, porque las entradas de la 3 pintaban solo la primera pieza.

### Lo que no cambia

El exportador, el renombrado/movimiento/borrado del Content Browser y el dedup
de GPU funcionan por `sourcePath` y por geometría. Los hijos son objetos
normales con malla propia.

## Errores y casos límite

| Caso | Comportamiento |
|---|---|
| `piece` fuera de rango (el fichero cambió) | `load` lanza. Al cargar escena, el objeto queda sin malla con el aviso de siempre; al reimportar, conserva la anterior |
| Nodo con matriz singular o NaN | Identidad y aviso en el Log |
| Malla sin caras | No es pieza |
| Una sola pieza en un nodo transformado | Como hoy: sin transformación |
| Add Mesh sobre un objeto con hijos | Las piezas van al final; los hijos que tenía no se tocan |
| Deshacer mientras las piezas cargan | Se borran los hijos; los resultados tardíos se descartan por id (ya ocurre) |
| Add Mesh de varias piezas sobre un objeto con malla | Imposible: Add Mesh solo se ofrece sin malla (`hasMesh()` o carga en vuelo lo cortan) |

## Testing

**Headless:**
- `model_import_tests`:
  - un `.gltf` escrito a mano con tres nodos: A trasladado usa la malla 0, B
    escalado usa la malla 1, y C **reutiliza** la malla 0. `loadStatic` da 2
    mallas y 3 piezas con los índices, nombres y matrices esperados;
  - `load(path, 1)` es la geometría de la segunda malla;
  - pieza fuera de rango → excepción;
  - un `.obj` de una malla → 1 pieza;
  - con la escala del sidecar a 2, las traslaciones se duplican;
  - `load(path)` == `load(path, 0)`.
- Escena: `"piece"` va y vuelve por JSON, se omite cuando es 0, y un JSON sin el
  campo carga la pieza 0.
- `asset_loader_tests`: dos peticiones del mismo fichero con piezas distintas →
  `readFileCount() == 1`; cada objeto recibe su pieza; `pieces` llega con las 3
  apariciones.
- `CompositeCommand`: `execute` en orden, `undo` en orden inverso (con comandos
  de prueba que registran el orden).
- Caché de precarga: `meshCacheKey` (pieza 0 = `sourcePath` a secas); un
  subárbol con dos hijos del mismo fichero y piezas distintas, serializado y
  reinsertado con la caché de sus mallas vivas (lo que hace el undo de Delete),
  devuelve a cada hijo **su** malla.
- `thumbnail_tests`: el preview de un modelo con dos piezas separadas cubre más
  píxeles que el de su primera pieza sola.

**Manual** (Release, Vulkan y D3D12):
- Un `.glb` real de varias piezas y materiales con Add Mesh:
  - se ve completo y cada hijo lleva su textura;
  - deshacer y rehacer;
  - guardar y recargar la escena;
  - reimportar con otra escala;
  - exportar.
- **Un FBX de varias piezas**: las unidades de la raíz no descuadran nada
  (riesgo 1).

## Riesgos

1. **Unidades y ejes de la raíz en FBX.** Excluir la transformación de la raíz
   es la hipótesis de que Assimp pone ahí la conversión de unidades y ejes. Si
   un FBX la pone en otro nodo, sus piezas saldrán ×100 o giradas respecto a su
   versión de una pieza. La verificación manual con un FBX de varias piezas lo
   decide.
2. **La clave de la caché de precarga toca caminos que hoy funcionan** (undo de
   Delete, precarga del runtime). Por eso la pieza 0 conserva el `sourcePath` a
   secas como clave, y hay tests de ida y vuelta del undo de Delete con un
   subárbol de dos piezas.
3. **Las texturas de los hijos se decodifican en el hilo principal al añadir el
   modelo.** El registro de cada hijo en GPU (`registerGameObject`) decodifica
   sus texturas externas de forma síncrona, como ya hacen hoy el undo de Delete
   y el duplicado; el worker solo entrega decodificada la pieza pedida. Se
   acepta como deuda (arreglarlo exige tocar el registro en GPU de los dos
   backends): un modelo grande de muchas piezas con texturas externas da un
   tirón visible al añadirse. No se relee el fichero del modelo.
