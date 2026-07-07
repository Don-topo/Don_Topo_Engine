# Diseño: sistema de Gizmos (debug draw)

## Objetivo

Clase para dibujar líneas de depuración sin iluminación (estilo Unity
`Gizmos`/`Debug.DrawLine`), activable/desactivable globalmente. Usos:
ejes de coordenadas de un objeto, bounding boxes de colliders, raycasts/rayos
de visión, normales de vértices/superficies, frustum de cámara, vectores
(velocidad, aceleración, dirección).

## Contexto actual

- No existe ningún sistema de debug draw en el engine (`Gizmo` en el código
  solo se refiere a ImGuizmo, el widget de manipulación interactiva del
  editor — no relacionado).
- `Skybox` (`engine/include/DonTopo/Skybox.h`, `engine/src/Skybox.cpp`) es el
  pipeline gráfico más simple existente y sirve de plantilla: `init()`/
  `shutdown()`/`draw()`, pipeline propio, shaders GLSL precompilados a
  `.spv`, push constant para la matriz de cámara, sin descriptor sets.
- `Renderer::recordCommandBuffer` (`engine/src/Renderer.cpp`) registra el
  Pass 1 (escena 3D → offscreen) terminando con `m_skybox.draw(...)` justo
  antes de `vkCmdEndRenderPass` (comentario explícito: skybox va al final
  por su depth `LEQUAL` sin escritura).
- Colliders (`BoxCollider`, `SphereCollider`, `CapsuleCollider`,
  `PlaneCollider`, en `engine/include/DonTopo/`) exponen centro/tamaño local
  (`getCenter()`, `getHalfExtents()`/`getRadius()`/`getHalfHeight()`), pero
  el transform mundial fiable en cualquier modo (kinematic o dinámico) es
  `GameObject::worldTransform`, no `collider->getWorldTransform()` (este
  último solo válido si `isDynamic()`).
- `Camera` no expone matriz de proyección; se construye ad-hoc en
  `Renderer.cpp` con `glm::perspective(fov, aspect, near, far)` +
  `proj[1][1] *= -1` (corrección Vulkan/GLM), con `near`/`far` heurísticos
  basados en distancia de cámara.

## Alcance v1: solo líneas

Los 6 casos de uso pedidos se cubren completamente con `LINE_LIST`. No se
construye pipeline de triángulos en esta versión — se añade después si
surge un caso concreto (ej. frustum relleno). La arquitectura (formato de
vértice `pos+color`, shader unlit, buffer host-mapped) no requiere cambios
para añadir un segundo pipeline `TRIANGLE_LIST` más adelante.

## Arquitectura

Clase `DonTopo::Gizmos`, singleton (`Gizmos::instance()`), sigue el patrón
`Skybox` de ciclo de vida. `Renderer` la posee y controla su init/draw/clear;
el resto del engine (sandbox, gameplay, editor) llama a la API estática sin
necesitar referencia al `Renderer`.

Decisión: **dibujo manual**, no automático. El motor no recorre el scene
graph dibujando colliders por su cuenta — quien quiera ver un bounding box
llama `Gizmos::drawWireBox(...)` explícitamente (ej. `EditorUI` cuando hay
un objeto seleccionado, o código de gameplay/debug).

## API pública

Namespace `DonTopo`, métodos estáticos vía singleton:

```cpp
void Gizmos::setEnabled(bool enabled);
bool Gizmos::isEnabled();

void Gizmos::drawLine(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color);

void Gizmos::drawRay(const glm::vec3& origin, const glm::vec3& dir,
                      float length, const glm::vec3& color);

void Gizmos::drawVector(const glm::vec3& origin, const glm::vec3& v,
                         const glm::vec3& color, float headSize = 0.1f);

void Gizmos::drawWireBox(const glm::mat4& transform, const glm::vec3& halfExtents,
                          const glm::vec3& color);

void Gizmos::drawWireSphere(const glm::mat4& transform, const glm::vec3& center,
                             float radius, const glm::vec3& color);

void Gizmos::drawWireCapsule(const glm::mat4& transform, const glm::vec3& center,
                              float radius, float halfHeight, const glm::vec3& color);

void Gizmos::drawAxes(const glm::mat4& transform, float scale = 1.0f);

void Gizmos::drawFrustum(const glm::mat4& viewProj, const glm::vec3& color);
```

Notas:

- `color` es libre (cualquier `vec3`); no hay significado especial salvo en
  `drawAxes`, que siempre usa rojo=X, verde=Y, azul=Z (convención estándar,
  sin parámetro de color).
- `drawVector` dibuja la línea `origin → origin+v` más una cabeza en forma
  de V (2 líneas cortas hacia atrás desde la punta), todo con `LINE_LIST` —
  no requiere triángulos.
- `drawWireSphere`/`drawWireCapsule` generan 3 y 2 círculos ortogonales
  respectivamente (24 segmentos por círculo) más, en la cápsula, 2 líneas
  laterales rectas uniendo los hemisferios.
- Si `setEnabled(false)`: todas las llamadas `drawX` son no-op inmediato (no
  acumulan), y `Gizmos::instance().draw()` no emite ningún comando (ni
  siquiera un `vkCmdDraw` vacío) — coste cero en runtime cuando está
  apagado.

## Formato de vértice y shaders

```cpp
struct GizmoVertex { glm::vec3 pos; glm::vec3 color; };
```

Nuevos shaders `shaders/gizmo.vert` / `shaders/gizmo.frag`:

- Vert: `gl_Position = push.viewProj * vec4(inPos, 1.0);`, pasa `inColor` a
  frag sin transformar (world space directo, sin modelo — cada `drawX` ya
  aplica su `transform` en CPU al generar los vértices).
- Frag: `outColor = vec4(inColor, 1.0);` — sin luz, sin textura, sin
  normales.
- Push constant: `mat4 viewProj` (64 bytes), mismo patrón que `Skybox`.

## Pipeline Vulkan

- Topología `VK_PRIMITIVE_TOPOLOGY_LINE_LIST`, `lineWidth = 1.0`.
- Viewport/scissor dinámicos (igual que el resto de pipelines del Renderer).
- Depth: `depthTestEnable = true`, `depthWriteEnable = false`,
  `compareOp = VK_COMPARE_OP_LESS` — el gizmo se oculta tras geometría
  opaca (respeta profundidad) pero no ensucia el depth buffer para el resto
  de la escena.
- Sin descriptor sets (no hay texturas ni UBOs, solo push constant).
- Render pass: el mismo `m_offscreenRenderPass` que usa `Skybox` (mismo
  color format y `VK_FORMAT_D32_SFLOAT` de depth).

## Buffer dinámico y ciclo de vida por frame

- Buffer de vértices `HOST_VISIBLE | HOST_COHERENT`, mapeado de forma
  persistente (`vkMapMemory` una vez en `init()`, nunca `unmap` hasta
  `shutdown()`).
- Capacidad fija: `kMaxGizmoVertices = 65536` (~32768 líneas). Si una
  llamada `drawX` excede la capacidad restante, se descarta el resto de esa
  llamada y se emite un warning una sola vez por sesión (no crash, no
  reallocation dinámica — YAGNI, 65536 vértices es margen amplio para debug
  draw).
- `Gizmos` acumula internamente en un `std::vector<GizmoVertex>` (CPU-side)
  durante el frame; el `memcpy` al buffer mapeado ocurre dentro de `draw()`,
  justo antes del `vkCmdDraw`.

Contrato de frame:

1. Código de gameplay/editor llama `Gizmos::drawX(...)` durante su update,
   **antes** de que `Renderer::drawFrame()` se invoque ese ciclo.
2. `Renderer::recordCommandBuffer`, tras `m_skybox.draw(...)`, llama
   `Gizmos::instance().draw(cmd, viewProj)`: si `count == 0` o
   `!isEnabled()`, no hace nada; si no, `memcpy` + `vkCmdDraw`.
3. Tras el `vkQueueSubmit`/present de ese frame, `Renderer::drawFrame` llama
   `Gizmos::instance().clear()` (vacía el vector CPU) para que el siguiente
   frame empiece limpio.

Esto replica el comportamiento de `Debug.DrawLine` de Unity sin duración: un
gizmo dibujado en un frame no persiste al siguiente salvo que se vuelva a
llamar.

## Integración en `Renderer`

- `Renderer::init()`: `Gizmos::instance().init(m_gpu, m_offscreenRenderPass, m_swapChainFormat);`
- `Renderer::recordCommandBuffer`: tras la línea de `m_skybox.draw(...)`
  (justo antes de `vkCmdEndRenderPass` del Pass 1),
  `Gizmos::instance().draw(cmd, m_projMatrix * m_viewMatrix);`
- `Renderer::drawFrame`: tras el submit/present, `Gizmos::instance().clear();`
- `Renderer::shutdown()`: `Gizmos::instance().shutdown(m_gpu);`

## Fuera de alcance

- Pipeline de triángulos / formas rellenas (frustum sólido, discos, etc.).
- Dibujo automático de colliders por el motor (toggle "mostrar colliders").
- Persistencia de gizmos por duración (`Debug.DrawLine(duration)` de Unity).
- Picking/selección de gizmos con el mouse.
- Grosor de línea variable.

## Plan de verificación manual

1. Desde `sandbox/src/main.cpp`, en el loop principal, llamar
   `Gizmos::drawAxes(someGameObject->worldTransform, 1.0f)` — confirmar 3
   líneas rojo/verde/azul en los ejes del objeto.
2. Llamar `Gizmos::drawWireBox(go->worldTransform, boxCollider->getHalfExtents(), {1,1,0})`
   sobre un GameObject con `BoxCollider` — confirmar wireframe amarillo
   alineado con el volumen de colisión real (mover/rotar el objeto y
   verificar que el gizmo sigue).
3. Llamar `Gizmos::drawRay(origin, dir, 10.0f, {1,0,1})` con un rayo fijo —
   confirmar línea recta de longitud correcta.
4. Construir `proj` con el mismo patrón ad-hoc que usa `Renderer.cpp` para
   el skybox (`glm::perspective(fov, aspect, near, far)` + `proj[1][1] *= -1`;
   `Camera` no expone `getProjMatrix()` hoy, no se añade en este plan) y
   llamar `Gizmos::drawFrustum(proj * camera.getViewMatrix(), {1,1,1})` desde
   una segunda cámara (o valores hardcodeados) — confirmar las 12 aristas
   del frustum en la posición esperada.
5. `Gizmos::setEnabled(false)` — confirmar que ninguna línea se dibuja ese
   frame aunque se sigan llamando `drawX`.
6. Verificar que un gizmo detrás de un objeto opaco queda oculto (test de
   profundidad correcto) y que uno delante es visible.
