# Diseño: ejes de coordenadas al seleccionar un GameObject

## Objetivo

Al seleccionar un GameObject en el panel Scene del editor, mostrar sus ejes
de coordenadas (rojo=X, verde=Y, azul=Z) en su posición/orientación actual,
usando el sistema `Gizmos` ya existente. Al deseleccionar o cambiar de
selección, los ejes del objeto anterior dejan de dibujarse.

## Contexto actual

- `Gizmos::drawAxes(transform, scale)` (`engine/include/DonTopo/Gizmos.h`)
  ya existe y dibuja 3 líneas RGB desde el origen de `transform`. Es de
  dibujo manual: nadie lo llama automáticamente hoy.
- `EditorUI` (`engine/include/DonTopo/EditorUI.h`) ya posee el estado de
  selección (`GameObject* m_selected`, privado, sin getter), actualizado en
  `drawSceneNode()` (click en el árbol) y limpiado a `nullptr` en
  `drawScene()` (click en zona vacía, o borrado del nodo seleccionado).
- Ya existe ImGuizmo (flechas interactivas mover/rotar/escalar) sobre el
  objeto seleccionado — los ejes de `Gizmos` son un añadido visual simple
  (líneas, sin manejador interactivo), coexisten con ImGuizmo sin
  conflicto.
- `sandbox/src/main.cpp` tiene una llamada de demo
  `DonTopo::Gizmos::drawAxes(cube->worldTransform, 40.0f)` (del plan
  `2026-07-07-gizmos.md`, Task 4) que dibuja ejes fijos en el cubo sin
  relación con la selección — queda redundante y confusa una vez existe
  este feature, se elimina.

## Arquitectura

Todo el cambio vive dentro de `EditorUI` — no se toca `Renderer` ni
`sandbox/main.cpp` (salvo borrar la línea de demo redundante). Nuevo método
privado `EditorUI::drawSelectionGizmo()`, llamado desde `EditorUI::draw()`
justo después de `drawScene(sceneRoot)`:

```cpp
void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    drawDockSpace();
    drawScene(sceneRoot);
    drawSelectionGizmo();   // nuevo
    drawViewport(viewportTexture, cameraView);
    drawProperties();
    drawContentBrowser();
}
```

`drawScene()` ya resuelve ese mismo frame cualquier borrado/deselección
pendiente (`m_pendingDelete`, click en vacío) antes de retornar — así que en
el punto de la llamada, `m_selected` ya es el valor final y válido (o
`nullptr`) para este frame, sin riesgo de puntero colgante.

```cpp
void EditorUI::drawSelectionGizmo()
{
    if (!m_selected) return;
    Gizmos::drawAxes(m_selected->worldTransform, selectionAxisScale(m_selected));
}
```

Si no hay selección, no se llama nada — los ejes desaparecen solos (no hace
falta ningún "apagado" explícito, es la ausencia de la llamada lo que los
oculta, igual que el resto de `Gizmos`).

## Cálculo de escala

Método privado `float EditorUI::selectionAxisScale(GameObject* node) const`:

- **Si `node->hasMesh()` y el mesh tiene al menos 1 vértice**: recorre
  `node->getMesh()->vertices`, calcula el bounding box local (min/max por
  componente), toma la mitad del eje más largo
  (`maxHalf = max((max-min).x, (max-min).y, (max-min).z) / 2`).
- **En cualquier otro caso** (sin mesh — GameObject recién creado y vacío,
  o solo con collider —, o mesh con `vertices` vacío): valor fijo `50.0f`.

En ambos casos, el resultado final es `max(maxHalf, 1.0f) * 1.3f` — el
factor 1.3 hace que los ejes sobresalgan un poco del contorno visual del
objeto en vez de quedar exactamente al ras; el piso de `1.0f` evita ejes de
longitud cero si el mesh fuese degenerado (bbox nula).

No hay caché: se recalcula cada frame, solo para el objeto seleccionado (no
para todo el árbol) — coste trivial (un recorrido de vértices de un único
mesh, no de la escena completa), y evita bugs de invalidación si el mesh del
objeto cambiara en caliente.

## Fuera de alcance

- Cualquier cambio a `Renderer` o al contrato de `Gizmos` — se reutiliza tal
  cual.
- Multi-selección (el editor solo soporta un `m_selected` a la vez hoy).
- Persistencia de la preferencia (mostrar/ocultar ejes de selección) — no es
  configurable, siempre se muestra si hay selección.
- Caché de la escala calculada.

## Limpieza relacionada

Se elimina de `sandbox/src/main.cpp` la línea
`DonTopo::Gizmos::drawAxes(cube->worldTransform, 40.0f);` (demo de la Task 4
del plan de Gizmos) — queda redundante y engañosa ahora que la selección
dibuja ejes en cualquier objeto automáticamente. El resto de llamadas de
demo (`drawWireBox`, `drawRay`, `drawFrustum`) no cambian.

## Plan de verificación manual

1. Ejecutar el sandbox, seleccionar el `cube` en el panel Scene → aparecen
   3 líneas RGB desde su centro, con longitud proporcional a su tamaño
   visual (mesh de 50 unidades de lado).
2. Seleccionar el `floor` (mesh grande) → los ejes son visiblemente más
   largos que los del cubo.
3. Click en zona vacía del panel Scene → los ejes desaparecen.
4. Seleccionar otro objeto → los ejes del anterior desaparecen, aparecen
   los del nuevo, sin quedar ambos a la vez.
5. Crear un GameObject vacío (botón "Create GameObject", sin mesh ni
   collider) y seleccionarlo → aparecen ejes de longitud fija (usando el
   valor de repliegue 50), ni cero ni desproporcionados.
6. Borrar el objeto seleccionado (tecla Delete) → los ejes desaparecen sin
   crash (verifica que no queda un puntero colgante a `m_selected`).
