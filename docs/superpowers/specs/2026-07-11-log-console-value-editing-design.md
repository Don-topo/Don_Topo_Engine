# Log Console — Value/Checkbox Editing — Design Spec

## Problema

El panel Log (spec previa: `2026-07-11-log-console-design.md`) ya registra
acciones discretas (crear/borrar objeto, añadir componente, guardar escena...)
pero no registra nada de lo que pasa dentro del panel Properties: mover un
objeto con los DragFloat de Position/Rotation/Scale, ajustar Center/Size/
Radius/Height de un collider, o marcar/desmarcar "Use Gravity" no deja
ningún rastro en el historial de la sesión.

## Objetivo

Instrumentar `EditorUI::drawProperties()` y las 4 secciones de collider
(`drawBoxColliderSection`, `drawSphereColliderSection`,
`drawCapsuleColliderSection`, `drawPlaneColliderSection`) para que cada
campo editable deje una línea en el Log **al soltar el drag** (no en cada
frame mientras se arrastra) o **al hacer click** (checkboxes).

Fuera de scope: loguear cada tecleo si el usuario edita el número a mano
(ImGui ya trata eso como "deactivated after edit" igual que soltar un
drag, así que cae dentro del mismo mecanismo sin trabajo extra). Loguear
valor anterior→nuevo (ya decidido en la spec original: solo valor nuevo).
Loguear la sincronización automática de Position/Rotation cuando un
collider dinámico cae por física (`m_transformDragActive`/
`m_colliderDragActive` ya evitan que esa sincronización interfiera con el
drag del usuario, y no pasa por `DragFloat`, así que nunca dispara
`IsItemDeactivatedAfterEdit`).

## Diseño

### 1. Mecanismo de "commit" — `ImGui::IsItemDeactivatedAfterEdit()`

ImGui expone, tras cada widget, `IsItemDeactivatedAfterEdit()`: `true`
exactamente un frame, el frame en que el widget pierde el foco *y* su
valor cambió durante la interacción (soltar un drag, o pulsar Enter/Tab
tras teclear un número). No dispara en cada frame de arrastre — evita
saturar el buffer de 200 líneas con el drag continuo.

Cada grupo de campos que ya se trata como una unidad en el código actual
(p.ej. Position = 3 `DragFloat` con un único `changed |=` que los engloba)
gana un bool de commit que agrega `IsItemDeactivatedAfterEdit()` de sus
componentes:

```cpp
bool posCommitted = false;
...
changed |= ImGui::DragFloat("X##1", &m_editPosition.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
posRotActive |= ImGui::IsItemActive();
posCommitted |= ImGui::IsItemDeactivatedAfterEdit();
ImGui::SameLine();
...
changed |= ImGui::DragFloat("Y##1", &m_editPosition.y, ...);
posCommitted |= ImGui::IsItemDeactivatedAfterEdit();
...
changed |= ImGui::DragFloat("Z##1", &m_editPosition.z, ...);
posCommitted |= ImGui::IsItemDeactivatedAfterEdit();
```

Al final de cada sección, tras cerrar el `if (sectionOpen)`:

```cpp
if (posCommitted)
    pushLog("Position de '" + m_selected->name + "' cambiado a " + formatVec3(m_editPosition));
```

El valor logueado es el cache de edición (`m_editPosition`, ya actualizado
ese mismo frame por el propio `DragFloat`) — nunca hace falta releer el
GameObject/collider real.

### 2. Helper de formato — `formatVec3`

Nueva función libre en el namespace anónimo de `EditorUI.cpp` (mismo sitio
que `isValidGameObjectName`, `trim`, etc.):

```cpp
// 2 decimales — suficiente para leer el valor de un vistazo sin líneas de
// log kilométricas; el panel Properties ya muestra 3 decimales para
// edición fina, el Log es solo un resumen legible.
std::string formatVec3(const glm::vec3& v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "(%.2f, %.2f, %.2f)", v.x, v.y, v.z);
    return buf;
}

std::string formatFloat(float f)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", f);
    return buf;
}
```

### 3. Puntos de instrumentación (14 en total)

| Sección | Campo | Tipo | Mensaje |
|---|---|---|---|
| Transform | Position | vec3 | `Position de '<name>' cambiado a (x, y, z)` |
| Transform | Rotation | vec3 | `Rotation de '<name>' cambiado a (x, y, z)` |
| Transform | Scale | vec3 | `Scale de '<name>' cambiado a (x, y, z)` |
| Box Collider | Center | vec3 | `Center de '<name>' (Box Collider) cambiado a (x, y, z)` |
| Box Collider | Size | vec3 | `Size de '<name>' (Box Collider) cambiado a (x, y, z)` |
| Box Collider | Use Gravity | bool | `Use Gravity de '<name>' (Box Collider) activado/desactivado` |
| Sphere Collider | Center | vec3 | `Center de '<name>' (Sphere Collider) cambiado a (x, y, z)` |
| Sphere Collider | Radius | float | `Radius de '<name>' (Sphere Collider) cambiado a r` |
| Sphere Collider | Use Gravity | bool | `Use Gravity de '<name>' (Sphere Collider) activado/desactivado` |
| Capsule Collider | Center | vec3 | `Center de '<name>' (Capsule Collider) cambiado a (x, y, z)` |
| Capsule Collider | Radius | float | `Radius de '<name>' (Capsule Collider) cambiado a r` |
| Capsule Collider | Height | float | `Height de '<name>' (Capsule Collider) cambiado a h` |
| Capsule Collider | Use Gravity | bool | `Use Gravity de '<name>' (Capsule Collider) activado/desactivado` |
| Plane Collider | Center | vec3 | `Center de '<name>' (Plane Collider) cambiado a (x, y, z)` |

Nótese que Transform NO lleva sufijo de tipo (no ambiguo, solo hay un
Transform por objeto); las secciones de collider sí lo llevan porque
"Center"/"Use Gravity" se repiten en varias — sin el sufijo, dos líneas
seguidas de "Center cambiado a..." no dirían a cuál collider pertenecen.

Los checkboxes (`Use Gravity`) ya son "commit inmediato" en el código
actual (`if (ImGui::Checkbox(...)) colliderChanged = true;`) — el `if`
que ya existe es el punto de log, no hace falta ningún mecanismo nuevo:

```cpp
if (ImGui::Checkbox("Use Gravity", &m_editUseGravity))
{
    colliderChanged = true;
    pushLog(std::string("Use Gravity de '") + m_selected->name +
             "' (Box Collider) " + (m_editUseGravity ? "activado" : "desactivado"));
}
```

### 4. Testing

Manual, sin framework — igual que el resto del motor:
- Arrastrar Position.X y soltar → 1 línea `Position de '<name>' cambiado
  a (...)`. Arrastrar Position.Y a continuación y soltar → 1 línea más
  (no se fusiona con la anterior).
- Teclear un valor directamente (doble-click en el DragFloat, escribir,
  Enter) → también loguea (mismo mecanismo, `IsItemDeactivatedAfterEdit`
  no distingue drag de tecleo).
- Arrastrar y NO soltar (mantener el frame a medio arrastre) → sin línea
  todavía; solo aparece al soltar.
- Marcar/desmarcar Use Gravity en cada uno de los 3 colliders que lo
  tienen → 1 línea cada vez, con el tipo de collider correcto en el texto.
- Seleccionar un objeto con collider dinámico y dejar que la física lo
  mueva (sin tocar ningún DragFloat) → NINGUNA línea nueva (la
  sincronización automática no pasa por commit de usuario).
- Cambiar de GameObject seleccionado (recarga el cache desde
  `localTransform`) → sin línea (no hay interacción de usuario sobre
  ningún widget).

## Riesgos / notas de implementación

- `IsItemDeactivatedAfterEdit()` debe leerse inmediatamente después de
  cada `DragFloat`/`Checkbox`, antes de que cualquier otro widget de
  ImGui "consuma" el estado del último item — seguir el mismo patrón ya
  usado para `IsItemActive()` en el código existente (llamada pegada a la
  línea del widget).
- `formatVec3`/`formatFloat` requieren `<cstdio>` (`std::snprintf`) —
  confirmar que ya está disponible transitivamente o añadirlo.
- Plane Collider no tiene Use Gravity ni Size/Radius — solo Center. No
  crear campos falsos para mantener "simetría" con los otros 3 colliders.
