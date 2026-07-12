# Log Console — Component Remove Logging — Design Spec

## Problema

El Log ya registra cuándo se **añade** un componente a un GameObject (4
colliders, Mesh, Audio Clip — instrumentado en la feature Log Console
base). Pero **quitarlo** (botón "x" de cada sección en Properties) no deja
ningún rastro.

## Objetivo

Loguear, con el mismo formato y convención que "añadir", cada vez que se
pulsa el botón "x" de una sección de componente y el componente se quita
efectivamente: Box/Sphere/Capsule/Plane Collider, Mesh, Audio Clip — los
mismos 6 tipos que ya cubre "añadir".

Fuera de scope: cualquier otro control dentro de esas secciones (Loop/Is 3D
del Audio Clip, Play/Stop de previsualización) — el pedido es
específicamente sobre añadir/quitar el componente en sí, no sobre editar
sus propiedades internas (eso ya lo cubre, para los campos con DragFloat/
Checkbox aplicables, la feature de value/checkbox editing).

## Diseño

### Mensaje

Mismo verbo invertido que "añadido", mismo patrón `"Componente <Tipo>
<verbo> <preposición> '<name>'"`:

- Añadir (ya existente): `"Componente Box Collider añadido a '<name>'"`
- Quitar (nuevo): `"Componente Box Collider quitado de '<name>'"`

### Puntos de instrumentación (6)

Cada uno es una línea `pushLog(...)` añadida dentro del `if (removeClicked)`
ya existente en cada sección, después de la mutación que quita el
componente (mismo orden relativo que usa "añadir": log después de
confirmar el efecto). `m_selected` sigue siendo válido tras cada mutación
(solo se quita el componente del GameObject, el GameObject en sí no se
toca) — no hay riesgo de leer `m_selected->name` sobre un puntero inválido.

| Sección | Bloque | Mensaje |
|---|---|---|
| `drawBoxColliderSection` | `if (removeClicked) { m_selected->setBoxCollider(nullptr); ... }` | `Componente Box Collider quitado de '<name>'` |
| `drawSphereColliderSection` | ídem `setSphereCollider(nullptr)` | `Componente Sphere Collider quitado de '<name>'` |
| `drawCapsuleColliderSection` | ídem `setCapsuleCollider(nullptr)` | `Componente Capsule Collider quitado de '<name>'` |
| `drawPlaneColliderSection` | ídem `setPlaneCollider(nullptr)` | `Componente Plane Collider quitado de '<name>'` |
| `drawMeshSection` | `if (removeClicked && m_renderer) { m_renderer->removeMeshComponent(m_selected); ... }` | `Componente Mesh quitado de '<name>'` |
| `drawAudioClipSection` | `if (removeClicked) { m_selected->setAudioClip(nullptr); ... }` | `Componente Audio Clip quitado de '<name>'` |

Nótese que `drawMeshSection`'s bloque de borrado está condicionado también
a `m_renderer` (guard ya existente); el `pushLog` va dentro de esa misma
condición, no fuera — si no hay `m_renderer`, el componente no se quita de
verdad, así que tampoco debe loguearse.

### Testing

Manual, sin framework — igual que el resto del motor:
- Añadir cada uno de los 6 tipos de componente y quitarlo con el botón
  "x" → 1 línea `Componente <Tipo> quitado de '<name>'` por cada uno.
- Confirmar que "añadir" sigue funcionando igual (regresión) — este
  cambio no toca esa ruta.
- Quitar un Mesh sin `m_renderer` asignado (si aplica en el contexto de
  prueba) → sin línea de log (el componente tampoco se quita de verdad).

## Riesgos / notas de implementación

- Ninguno nuevo — reutiliza `pushLog` ya existente, mismo patrón de mensaje
  que "añadir", sin tocar `EditorUI.h`.
