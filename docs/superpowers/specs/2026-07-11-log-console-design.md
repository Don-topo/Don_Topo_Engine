# Log Console — Design Spec

## Problema

Hoy no hay forma de ver un historial de qué acciones de edición se han ido
realizando durante la sesión (crear/borrar objetos, guardar escena, entrar en
Play Mode...). Cualquier feedback es efímero (un texto de error puntual en el
toolbar/Properties que desaparece al siguiente cambio de estado) o inexistente.

## Objetivo

Panel nuevo "Log" (dockable, igual que Content Browser/Scene/Properties) que
muestra una línea por acción de edición significativa, con timestamp, en orden
cronológico con autoscroll al fondo. Sin persistencia a disco. Buffer acotado:
al superar 200 líneas se descartan las más antiguas (FIFO).

Fuera de scope: filtrado/búsqueda, niveles de severidad con color, persistencia
entre sesiones, acciones de solo-vista (hover, selección, wireframe toggle,
focus de cámara, apertura/cierre de paneles).

## Diseño

### 1. Estado — extensión de `EditorUI`

Todas las acciones a loguear ya se originan dentro de `EditorUI` (es el único
punto de entrada de edición), así que no hace falta un logger global/singleton:

```cpp
// EditorUI.h — privado
// Ring buffer del Log: añade una línea con timestamp y descarta la más
// antigua si se supera kLogMaxEntries. Único punto de escritura del log.
void pushLog(const std::string& message);
void drawLogPanel();

static constexpr size_t kLogMaxEntries = 200;
std::deque<std::string> m_logEntries;
// true si el panel ya estaba scrolleado al fondo el frame anterior — evita
// pelear con el usuario si sube a leer historial mientras llegan más líneas.
bool m_logAutoScroll = true;
```

`pushLog` formatea `"[HH:MM:SS] " + message` usando `<chrono>` +
`localtime` (mismo patrón ya usado en el motor donde hace falta hora local),
hace `push_back`, y si `size() > kLogMaxEntries` hace `pop_front()`.

### 2. Panel — `drawLogPanel()`

```cpp
void EditorUI::drawLogPanel()
{
    ImGui::Begin("Log");
    for (const auto& line : m_logEntries)
        ImGui::TextUnformatted(line.c_str());

    // Autoscroll: solo si ya estaba al fondo (no pelea con el usuario si
    // sube a revisar historial mientras entran líneas nuevas).
    if (m_logAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        ImGui::SetScrollHereY(1.0f);
    m_logAutoScroll = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();

    ImGui::End();
}
```

Llamada añadida en `EditorUI::draw()`, junto al resto de paneles:

```cpp
void EditorUI::draw(...)
{
    drawToolbar();
    drawDockSpace();
    drawScene(sceneRoot);
    drawSelectionGizmo();
    drawViewport(viewportTexture, cameraView);
    drawProperties();
    drawLogPanel();      // <-- nuevo
    drawMeshDialog();
    drawAudioClipDialog();
    drawSceneDialog();
    drawContentBrowser(sceneRoot);
}
```

Ventana dockable normal (`ImGui::Begin("Log")`, sin flags forzados): la
primera vez el usuario la arrastra y la fija abajo del layout; `imgui.ini`
recuerda la posición en sesiones siguientes — mismo comportamiento que
Content Browser hoy (no hay `DockBuilder` explícito en el motor).

### 3. Puntos de instrumentación

Cada uno llama `pushLog(...)` justo tras confirmar el efecto (no antes, para
no loguear intentos fallidos salvo que se indique lo contrario):

| Acción | Sitio (EditorUI.cpp) | Mensaje |
|---|---|---|
| Crear GameObject (menú contextual) | `drawScene`, tras `sceneRoot->addChild(...)` / `node->addChild(...)` | `GameObject '<name>' creado` |
| Crear Basic Shape | `createBasicShape`, tras `parent->addChild(name)` | `GameObject '<name>' creado` |
| Borrar GameObject | `drawScene`, bloque `m_pendingDelete`, tras `m_scene->removeGameObject(target)` | `GameObject '<name>' eliminado` |
| Renombrar GameObject | popup Rename, rama `accept` tras aplicar `newName` | `GameObject renombrado: '<old>' -> '<new>'` |
| Add Box/Sphere/Capsule/Plane Collider | `drawAddComponentButton`, tras cada `set*Collider(...)` | `Componente Box Collider añadido a '<name>'` (etc.) |
| Add Mesh | `loadMeshForSelected`, tras `m_selected->setMesh(...)` (rama éxito) | `Componente Mesh añadido a '<name>'` |
| Add Audio Clip | `loadAudioClipForSelected`, tras `m_selected->setAudioClip(...)` (rama éxito) | `Componente Audio Clip añadido a '<name>'` |
| Save Scene | `drawSceneDialog`, rama save, ambos desenlaces | `Escena guardada: <path>` / `Error al guardar escena: <path>` |
| Load Scene | `drawSceneDialog`, rama load, ambos desenlaces | `Escena cargada: <path>` / `Error al cargar escena: <path>` |
| Play Mode iniciado | `drawToolbar`, botón Play | `Play Mode iniciado` |
| Play Mode detenido | `drawToolbar`, botón Stop, tras restore | `Play Mode detenido` |
| Rename asset | Content Browser, tras `std::filesystem::rename(...)` con éxito | `Asset renombrado: '<old>' -> '<new>'` |
| Delete asset | Content Browser, tras `std::filesystem::remove_all(...)` con éxito | `Asset eliminado: <path>` |

Save/Load Scene son el único par con log también en el caso de error (ya
existe `m_sceneIOError` para ese feedback puntual; el log añade el historial
persistente de la sesión). El resto de fallos (mesh/audio load error, rename/
delete de asset fallido) ya tienen su propio feedback puntual en la UI
existente y quedan fuera de esta iteración para no duplicar ruido.

### 4. Testing

Manual, sin framework — igual que el resto del motor:
- Crear/borrar/renombrar un GameObject → aparece línea correspondiente.
- Añadir cada tipo de componente (4 colliders, Mesh, Audio Clip) → 1 línea
  cada uno, con el nombre correcto.
- Guardar y cargar una escena (éxito) → 2 líneas con el path.
- Forzar un fallo de carga (fichero corrupto) → línea de error, no crashea.
- Play → Stop → 2 líneas.
- Renombrar/borrar un asset desde Content Browser → línea correspondiente.
- Superar 200 acciones en una sesión → las líneas más antiguas desaparecen,
  el panel no crece sin límite.
- Arrastrar el panel Log y dockearlo abajo del layout, cerrar y reabrir el
  editor → posición persistida vía `imgui.ini`.

## Riesgos / notas de implementación

- `pushLog` es el único punto de escritura — cualquier acción futura que se
  quiera loguear pasa por ahí, no se duplica lógica de ring-buffer.
- El autoscroll usa el patrón estándar de la consola de ejemplo de ImGui
  (`GetScrollY() >= GetScrollMaxY()` antes de añadir, `SetScrollHereY(1.0f)`
  si aplica) — confirmar que no pelea con el usuario al hacer scroll manual
  hacia arriba mientras llegan líneas nuevas.
- `<chrono>` + `localtime` para el timestamp: usar la variante thread-safe
  disponible en la plataforma (`localtime_s` en MSVC, este motor ya compila
  solo con MSVC/Ninja en Windows).
