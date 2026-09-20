# Curvas de clip (fila 15 / C9) — diseño

**Fecha:** 2026-09-20
**Backlog:** `docs/animation-audit.md`, fila 15, hallazgo C9 ("Curvas de clip: curvas float con nombre que escriban un parámetro").

## Qué falta hoy

Un clip de malla solo tiene canales de hueso (`SkinnedMesh.h:26-31`) y un clip de
propiedades (C14) solo escribe propiedades del `GameObject`. No hay forma de que
el tiempo de un estado alimente un **parámetro** del Animator: una curva
`velocidad` que suba durante la animación de arranque, una `ventanaDeGolpe` que
valga 1 entre dos instantes del ataque, un `pesoDePie` que condicione la IK.

## Qué se construye

Una pista de clip de propiedades puede tener dos destinos: la propiedad del
objeto (lo de hoy) o un **parámetro Float** del Animator, por nombre. La curva
es la misma estructura de keys que ya existe; lo único nuevo es a dónde va el
valor y en qué momento del frame se escribe.

No se toca la GPU: una curva no llega al backend.

## Modelo de datos

`PropertyTracks.h`:

```cpp
enum class TrackTarget { Property, Parameter };

struct PropertyTrack
{
    TrackTarget              target = TrackTarget::Property;
    PropertyId               property = PropertyId::PositionX;  // si target == Property
    std::string              parameterName;                     // si target == Parameter
    std::vector<PropertyKey> keys;
    bool                     resolved = false;
};
```

`target` por defecto `Property`: una pista existente sigue significando lo mismo
y un `.scene` viejo se lee igual.

**Solo Float.** Un parámetro de otro tipo o inexistente deja la pista sin
resolver, igual que hoy una propiedad de un componente que el objeto no tiene.
Int y Bool quedan fuera a propósito (YAGNI): redondeo y umbral son reglas que
habría que documentar y testear sin un caso que las pida.

## Resolución

`AnimatorComponent::bindProperties(go, warnings)` ya recorre todas las pistas de
todos los clips. Para `target == Parameter`:

- `resolved = true` si existe un parámetro declarado con ese nombre **y** de tipo
  `Float`.
- Si no, `resolved = false` y un aviso en `warnings`: `"curva 'X': no hay un
  parametro Float llamado 'Y'"`.

El nombre es la referencia (como `propertyClipName` en los estados), así que
renombrar un parámetro obliga a re-resolver: los sitios del panel que ya llaman a
`bindProperties` tras renombrar cubren esto.

## Evaluación: dónde y en qué orden

Dentro de `AnimatorComponent::updateLayer`, **después** de avanzar los relojes y
resolver el cross-fade y **antes** de `if (!evaluateTransitions) return;`
(hoy `AnimatorComponent.cpp:1193`).

Ese punto es lo que da la garantía que se pidió: el valor de la curva de este
frame ya condiciona las transiciones de este mismo frame, sin retardo. Una curva
puede así sacar a su propio estado, que es el comportamiento esperado.

Por capa:

1. Muestras de esa capa: estado actual y, en un fade, el que se apaga, con el
   peso del fade. Es el mismo reparto que `propertySamples()`, extraído a un
   helper por capa (`layerPropertySamples(li, out, max)`) que `propertySamples`
   pasa a reutilizar, para que no haya dos repartos que mantener.
2. Para cada nombre de parámetro que aparezca en las pistas resueltas de esas
   muestras: mezcla ponderada con `blendPropertyValues` en su rama no-rotación
   (media ponderada, sin camino corto: un parámetro no es un ángulo).
3. `setFloat(nombre, valor)`.

**El peso de la capa no escala el valor.** Una pose se mezcla; un parámetro se
escribe. Las capas se recorren en orden, así que si dos capas tienen una curva
para el mismo parámetro **gana la última**. Se documenta en el README y en el
comentario de la función.

Una capa con peso 0 sí escribe: su máquina de estados sigue corriendo y su curva
es información, no pose. (Distinto de `propertySamples()`, que salta las capas
con peso 0 porque allí el peso multiplica lo que se aplica.)

**En Edit también se escribe**, porque el punto elegido está antes del `return`
de `evaluateTransitions`: durante el preview del editor el parámetro se ve
moverse en el panel. El valor que el usuario hubiera fijado a mano queda pisado
mientras el preview avanza; es el mismo trato que reciben hoy las propiedades del
objeto, que el preview también mueve.

## Aplicación en el host

`applyAnimatorFrame` (`Renderer/SkinnedFrameSync.h`) recorre las pistas por
`PropertyId`. Se le añade la condición `tr.target == TrackTarget::Property`: una
pista de parámetro no toca el transform, ni el material, ni marca
`AnimatorFrameResult`. El componente ya la escribió en `update`.

## Serialización

`Scene.cpp`, dentro de `"propertyClips"`, por pista:

```json
{ "target": "parameter", "parameter": "velocidad", "keys": [...] }
```

- `"target"` y `"parameter"` se escriben **solo** cuando `target == Parameter`;
  una pista de propiedad se guarda exactamente como hoy.
- Al leer, sin `"target"` (o con `"property"`) la pista es de propiedad: los
  `.scene` existentes no cambian de significado.
- Una pista de parámetro sin nombre, o con `"parameter"` vacío, se descarta al
  cargar (como una transición fuera de rango).

## UI

Panel del Animator, sección **Property Clips**, en cada pista:

- Combo de destino: `Propiedad` / `Parametro`.
- Si es propiedad, el combo de `PropertyId` de hoy. Si es parámetro, un combo con
  los nombres de los parámetros **Float** declarados, más el nombre actual aunque
  ya no exista (para no perderlo en silencio al abrir una escena cuyo parámetro
  se borró).
- Una pista sin resolver se pinta como las de hoy (el aviso de `bindProperties`).
- IDs con `###` estables, como el resto del panel.

Cambiar el destino de una pista llama a `bindProperties` para re-resolver.

## Tests (`engine/tests/animator_tests.cpp`)

1. Una curva con dos keys escribe el parámetro con el valor interpolado en el
   instante del estado.
2. En un cross-fade entre dos estados con curvas del mismo parámetro, el valor es
   la media ponderada por el peso del fade.
3. Orden: una curva que cruza el umbral de una condición dispara la transición en
   **el mismo** `update`, no en el siguiente.
4. Una pista de parámetro no mueve el transform: `applyAnimatorFrame` deja la
   posición intacta y devuelve `transform == false`.
5. `resolved == false` (y un aviso) si el parámetro no existe, y también si
   existe con tipo `Int`/`Bool`/`Trigger`.
6. Dos capas con una curva para el mismo parámetro: gana la última.
7. Una capa con peso 0 escribe su curva igualmente.
8. Round-trip JSON: guardar y cargar conserva destino, nombre y keys; una pista
   de propiedad guardada sigue sin campo `"target"`.

Cada guarda nueva se sabotea de una en una (parchear → compilar → ejecutar →
revertir) para demostrar que cae **su** test.

## Verificación manual

En los dos backends (Vulkan y D3D12), con el proyecto abierto desde la GUI:
un estado con un clip de propiedades que tenga una curva sobre un parámetro Float
usado en la condición de una transición; se ve el parámetro moverse en el panel y
el estado cambiar al cruzar el umbral.

## Documentación

- `README.md`: en la sección del Animator, qué es una curva, que solo escribe
  parámetros Float, y las dos reglas no obvias (gana la última capa; en Edit el
  preview pisa el valor manual).
- `docs/animation-audit.md`: fila C9 a ✅ EXISTE con el rango de commits, y la
  fila 15 al día.

## Fuera de alcance

- Curvas en los clips del FBX (lo que se importa sigue siendo hueso puro).
- Interpolación distinta de la lineal (la comparte con C14; si un día se añaden
  tangentes, entran por `samplePropertyTrack` para las dos).
- Escribir parámetros Int/Bool.
