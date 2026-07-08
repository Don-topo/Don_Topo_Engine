# Camera Focus on Selected GameObject (tecla F)

## Objetivo

Al pulsar `F`, la cámara del editor se acerca y centra en el `GameObject`
actualmente seleccionado en el panel Scene (jerarquía izquierda de ImGui). No
debe activarse mientras el usuario está escribiendo en una caja de texto de
ImGui (rename, campos numéricos en modo edición, etc).

## Comportamiento

- Salto instantáneo (sin animación/lerp). Coherente con que `Camera` no tiene
  hoy ningún sistema de interpolación temporal.
- La cámara retrocede a lo largo del vector *cámara→objeto* actual (no del
  `front` de la cámara), y queda mirando directo al centro del objeto — patrón
  "Frame Selected" de Unity/Blender.
- Sin selección: no-op silencioso.

## Componentes

### `Camera::focusOn(glm::vec3 center, float boundingRadius)` (nuevo, público)

Método puro, sin dependencia de `GameObject`/ImGui.

```
dir = m_pos - center
if length(dir) < epsilon: dir = -m_front   // cámara ya está en center
dir = normalize(dir)
distance = max(boundingRadius, kMinRadius) * kFocusDistanceFactor
m_pos = center + dir * distance
lookAlongAxis(dir)   // reusa método existente; deja front = -dir, mirando a center
```

Constantes (en `Camera.cpp`):
- `kFocusDistanceFactor = 2.5f`
- `kMinRadius = 5.0f` (clamp mínimo, evita quedar pegado a objetos diminutos)

### `EditorUI::focusSelected(Camera&)` (nuevo, público)

- No-op si `m_selected == nullptr`.
- Si `m_selected->hasMesh()`: bbox local de `vertices` (mismo cálculo que
  `selectionAxisScale`, sin el factor 2.0 de esa función), escalado por el
  world-scale del nodo (longitud de las columnas 0/1/2 de `worldTransform`,
  aproximación ya asumida en el código existente — ignora shear/rotación no
  uniforme). Centro = traslación de `worldTransform` (columna 3).
- Si no tiene mesh: radio fallback fijo `kFallbackRadius = 50.0f` (igual que
  el fallback de `selectionAxisScale`), centro = traslación de
  `worldTransform`.
- Llama a `camera.focusOn(center, radius)`.

### `Renderer::focusSelected(Camera&)` (nuevo, público)

Passthrough a `m_editorUI.focusSelected(camera)`, mismo patrón que
`isViewportHovered()`.

### `main.cpp` — key callback existente (`glfwSetKeyCallback`)

```cpp
if (key == GLFW_KEY_F && action == GLFW_PRESS && !ImGui::GetIO().WantTextInput)
    ctx->rnd->focusSelected(*ctx->cam);
```

`WantTextInput` (no `WantCaptureKeyboard`): este último es `true` casi
siempre que el mouse está sobre cualquier panel ImGui, incluida la lista de
GameObjects donde se hace la selección — bloquearía F ahí. `WantTextInput`
solo es `true` con foco real en una caja de texto (rename, DragFloat en modo
edición), que es exactamente el caso a excluir.

No se añade gate de `isViewportHovered()`: F debe funcionar con la selección
activa sin importar sobre qué panel esté el mouse, siempre que no sea una
caja de texto.

## Casos borde

- Sin selección → no-op.
- Cámara exactamente en el centro del objeto (`dir` degenerado) → fallback a
  `-m_front`.
- Nodo sin mesh (ej. GameObject vacío usado como padre) → radio fallback.
- No modifica `moveSpeed`/WASD ni ningún otro estado de `Camera` más allá de
  `m_pos`/`m_yaw`/`m_pitch` (vía `lookAlongAxis`).

## Testing (manual)

1. Seleccionar un GameObject con mesh en el panel Scene, pulsar F → cámara se
   centra y mira al objeto.
2. Seleccionar un GameObject sin mesh (nodo vacío) → F usa radio fallback, no
   crashea.
3. Sin selección, pulsar F → no pasa nada.
4. Abrir rename (F2) o poner el cursor en modo edición de un DragFloat
   (Position/Scale), pulsar F → NO se dispara el foco.
5. Pulsar F con el mouse sobre el panel Scene (no viewport) y selección
   activa → sí se dispara (sin gate de hover).
