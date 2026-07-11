# Log Console Value/Checkbox Editing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** El panel Log (ya existente) gana 14 puntos de instrumentación nuevos: cada campo editable del panel Properties (Position/Rotation/Scale del Transform, Center/Size/Radius/Height/Use Gravity de los 4 tipos de collider) deja una línea al soltar el drag o marcar/desmarcar un checkbox — nunca en cada frame mientras se arrastra.

**Architecture:** Todo vive en `engine/src/EditorUI.cpp`, sin tocar `EditorUI.h` (no hace falta estado nuevo — `EditorUI::pushLog` ya existe desde la feature Log Console previa). El mecanismo de "commit" es `ImGui::IsItemDeactivatedAfterEdit()`, que ImGui pone en `true` exactamente un frame — el frame en que un widget pierde el foco *y* su valor cambió durante la interacción (soltar un drag o teclear+Enter). Cada grupo de campos que el código ya trata como una unidad (p.ej. Position = 3 `DragFloat` con un único `changed |=` que los engloba) gana un bool de commit adicional (`posCommitted`, `centerCommitted`, etc.) que agrega `IsItemDeactivatedAfterEdit()` de sus componentes; al cerrar la sección se llama `pushLog(...)` con el valor final del cache de edición (`m_editPosition`, `m_editColliderCenter`, ...), ya actualizado ese mismo frame por el propio widget. Dos funciones libres nuevas en el namespace anónimo de `EditorUI.cpp` (`formatVec3`, `formatFloat`, 2 decimales) formatean los valores para el mensaje.

**Tech Stack:** C++20, Dear ImGui (`IsItemDeactivatedAfterEdit`, ya parte de la librería integrada), `<cstdio>` (`std::snprintf`) para el formateo. Sin framework de tests unitarios — verificación por compilación + ejecución manual del editor (igual que el resto del motor).

## Global Constraints

- No hay gtest/ctest en el repo — cada tarea se verifica con `build.bat` vía **PowerShell** (no Bash).
- El log dispara **al soltar el drag** (`IsItemDeactivatedAfterEdit`), nunca en cada frame mientras se arrastra — el buffer de 200 líneas (ya existente, sin cambios en esta feature) se saturaría en segundos si se logueara cada frame.
- Una línea por campo vec3/escalar completo (Position, Rotation, Scale, Center, Size, Radius, Height cada uno independiente), no una línea por eje individual ni una línea que fusione toda la sección.
- Formato de mensaje: `"<Campo> de '<name>' cambiado a <valor>"` para DragFloat (vec3 con `formatVec3`, escalar con `formatFloat`); las secciones de collider añaden el tipo entre paréntesis (`"Center de '<name>' (Box Collider) cambiado a ..."`) porque "Center"/"Use Gravity" se repiten entre colliders y Transform no lleva ese sufijo (no ambiguo, un único Transform por objeto). Checkboxes: `"Use Gravity de '<name>' (<Tipo> Collider) activado"` o `"...desactivado"`.
- Solo valor nuevo en el mensaje (no valor anterior→nuevo) — decisión ya tomada en la spec del Log Console original y reafirmada en esta.
- `formatVec3`/`formatFloat` van en el namespace anónimo de `EditorUI.cpp`, mismo sitio que `isValidGameObjectName`/`trim` — no exponerlos en `EditorUI.h`.
- Plane Collider no tiene Use Gravity ni Size/Radius — solo Center. No inventar campos que no existen en esa sección.

---

### Task 1: Helpers de formato + instrumentación de Transform (Position/Rotation/Scale)

**Files:**
- Modify: `engine/src/EditorUI.cpp` (includes, namespace anónimo, `EditorUI::drawProperties`)

**Interfaces:**
- Produces: `formatVec3(const glm::vec3&) -> std::string`, `formatFloat(float) -> std::string` (namespace anónimo, usadas por las Tareas 2 y 3 también).

- [ ] **Step 1: Añadir `#include <cstdio>`**

En `engine/src/EditorUI.cpp:22-29`, reemplaza:

```cpp
#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
```

por:

```cpp
#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
```

- [ ] **Step 2: Añadir `formatVec3`/`formatFloat` al namespace anónimo**

En `engine/src/EditorUI.cpp`, localiza `trim` (justo antes de la función `moveGameObject`):

```cpp
std::string trim(const std::string& name)
{
    size_t begin = name.find_first_not_of(" \t");
    size_t end   = name.find_last_not_of(" \t");
    return name.substr(begin, end - begin + 1);
}

// Mueve dragged pa la posición de target dentro de la lista de hijos de
```

reemplaza por:

```cpp
std::string trim(const std::string& name)
{
    size_t begin = name.find_first_not_of(" \t");
    size_t end   = name.find_last_not_of(" \t");
    return name.substr(begin, end - begin + 1);
}

// 2 decimales — suficiente para leer el valor de un vistazo en el Log sin
// líneas kilométricas; el panel Properties ya muestra 3 decimales para
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

// Mueve dragged pa la posición de target dentro de la lista de hijos de
```

- [ ] **Step 3: Instrumentar Position/Rotation/Scale en `EditorUI::drawProperties`**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawProperties`, reemplaza el bloque completo (desde la declaración de `changed`/`posRotActive` hasta la línea `m_transformDragActive = posRotActive;`):

```cpp
    bool changed = false;
    bool posRotActive = false;

    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_OpenOnArrow))
    {
        ImGui::Text("Position");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("X##1", &m_editPosition.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Y##1", &m_editPosition.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Z##1", &m_editPosition.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();

        ImGui::Text("Rotation");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("X##2", &m_editRotationDeg.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Y##2", &m_editRotationDeg.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Z##2", &m_editRotationDeg.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();

        ImGui::Text("Scale   ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("X##3", &m_editScale.x, 0.005f, 0.001f, +FLT_MAX, "% .3f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Y##3", &m_editScale.y, 0.005f, 0.001f, +FLT_MAX, "% .3f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Z##3", &m_editScale.z, 0.005f, 0.001f, +FLT_MAX, "% .3f");

        ImGui::TreePop();
    }

    m_transformDragActive = posRotActive;
```

por:

```cpp
    bool changed = false;
    bool posRotActive = false;
    bool posCommitted = false;
    bool rotCommitted = false;
    bool scaleCommitted = false;

    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_OpenOnArrow))
    {
        ImGui::Text("Position");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("X##1", &m_editPosition.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        posCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Y##1", &m_editPosition.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        posCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Z##1", &m_editPosition.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        posCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Rotation");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("X##2", &m_editRotationDeg.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        rotCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Y##2", &m_editRotationDeg.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        rotCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Z##2", &m_editRotationDeg.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        posRotActive |= ImGui::IsItemActive();
        rotCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Scale   ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("X##3", &m_editScale.x, 0.005f, 0.001f, +FLT_MAX, "% .3f");
        scaleCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Y##3", &m_editScale.y, 0.005f, 0.001f, +FLT_MAX, "% .3f");
        scaleCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        changed |= ImGui::DragFloat("Z##3", &m_editScale.z, 0.005f, 0.001f, +FLT_MAX, "% .3f");
        scaleCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::TreePop();
    }

    m_transformDragActive = posRotActive;

    if (posCommitted)
        pushLog("Position de '" + m_selected->name + "' cambiado a " + formatVec3(m_editPosition));
    if (rotCommitted)
        pushLog("Rotation de '" + m_selected->name + "' cambiado a " + formatVec3(m_editRotationDeg));
    if (scaleCommitted)
        pushLog("Scale de '" + m_selected->name + "' cambiado a " + formatVec3(m_editScale));
```

- [ ] **Step 4: Compilar**

```powershell
& .\build.bat
```
Expected: build sin errores.

- [ ] **Step 5: Verificación manual**

```powershell
& .\build-ninja\sandbox\Sandbox.exe
```
- [ ] Seleccionar un GameObject, arrastrar Position.X y soltar → línea `Position de '<name>' cambiado a (x.xx, y.xx, z.xx)` en el Log.
- [ ] Arrastrar Rotation y soltar → línea `Rotation de '<name>' cambiado a (...)`.
- [ ] Arrastrar Scale y soltar → línea `Scale de '<name>' cambiado a (...)`.
- [ ] Mantener el drag a medio camino (sin soltar) → sin línea todavía.
- [ ] Doble-click en un DragFloat, teclear un valor, Enter → también loguea.

- [ ] **Step 6: Commit**

```bash
git add engine/src/EditorUI.cpp
git commit -m "feat(log): instrumenta Position/Rotation/Scale del Transform"
```

---

### Task 2: Instrumentar Box Collider y Sphere Collider

**Files:**
- Modify: `engine/src/EditorUI.cpp` (`drawBoxColliderSection`, `drawSphereColliderSection`)

**Interfaces:**
- Consumes: `formatVec3(const glm::vec3&) -> std::string`, `formatFloat(float) -> std::string` (Tarea 1); `EditorUI::pushLog(const std::string&) -> void` (feature Log Console previa).

- [ ] **Step 1: Instrumentar `EditorUI::drawBoxColliderSection`**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawBoxColliderSection`, reemplaza el bloque desde la declaración de `sectionOpen` hasta el `if (colliderChanged) { ... }`:

```cpp
    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Box Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##c1", &m_editColliderCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##c1", &m_editColliderCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##c1", &m_editColliderCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::Text("Size  ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##c2", &m_editColliderSize.x, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##c2", &m_editColliderSize.y, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##c2", &m_editColliderSize.z, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        if (ImGui::Checkbox("Use Gravity", &m_editUseGravity))
            colliderChanged = true;

        ImGui::TreePop();
    }

    m_colliderDragActive = dragActive;

    if (colliderChanged)
    {
        bc->setCenter(m_editColliderCenter);
        bc->setHalfExtents(m_editColliderSize * 0.5f);
        bc->setUseGravity(m_editUseGravity);
    }
```

por:

```cpp
    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Box Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;
    bool centerCommitted = false;
    bool sizeCommitted = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##c1", &m_editColliderCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##c1", &m_editColliderCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##c1", &m_editColliderCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Size  ");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##c2", &m_editColliderSize.x, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        sizeCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##c2", &m_editColliderSize.y, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        sizeCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##c2", &m_editColliderSize.z, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        sizeCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        if (ImGui::Checkbox("Use Gravity", &m_editUseGravity))
        {
            colliderChanged = true;
            pushLog(std::string("Use Gravity de '") + m_selected->name +
                     "' (Box Collider) " + (m_editUseGravity ? "activado" : "desactivado"));
        }

        ImGui::TreePop();
    }

    m_colliderDragActive = dragActive;

    if (centerCommitted)
        pushLog("Center de '" + m_selected->name + "' (Box Collider) cambiado a " + formatVec3(m_editColliderCenter));
    if (sizeCommitted)
        pushLog("Size de '" + m_selected->name + "' (Box Collider) cambiado a " + formatVec3(m_editColliderSize));

    if (colliderChanged)
    {
        bc->setCenter(m_editColliderCenter);
        bc->setHalfExtents(m_editColliderSize * 0.5f);
        bc->setUseGravity(m_editUseGravity);
    }
```

- [ ] **Step 2: Instrumentar `EditorUI::drawSphereColliderSection`**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawSphereColliderSection`, reemplaza el bloque desde la declaración de `sectionOpen` hasta el `if (colliderChanged) { ... }`:

```cpp
    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Sphere Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##s1", &m_editSphereCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##s1", &m_editSphereCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##s1", &m_editSphereCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::Text("Radius");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##s2", &m_editSphereRadius, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        if (ImGui::Checkbox("Use Gravity", &m_editSphereUseGravity))
            colliderChanged = true;

        ImGui::TreePop();
    }

    m_sphereColliderDragActive = dragActive;

    if (colliderChanged)
    {
        sc->setCenter(m_editSphereCenter);
        sc->setRadius(m_editSphereRadius);
        sc->setUseGravity(m_editSphereUseGravity);
    }
```

por:

```cpp
    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Sphere Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;
    bool centerCommitted = false;
    bool radiusCommitted = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##s1", &m_editSphereCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##s1", &m_editSphereCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##s1", &m_editSphereCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Radius");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##s2", &m_editSphereRadius, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        radiusCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        if (ImGui::Checkbox("Use Gravity", &m_editSphereUseGravity))
        {
            colliderChanged = true;
            pushLog(std::string("Use Gravity de '") + m_selected->name +
                     "' (Sphere Collider) " + (m_editSphereUseGravity ? "activado" : "desactivado"));
        }

        ImGui::TreePop();
    }

    m_sphereColliderDragActive = dragActive;

    if (centerCommitted)
        pushLog("Center de '" + m_selected->name + "' (Sphere Collider) cambiado a " + formatVec3(m_editSphereCenter));
    if (radiusCommitted)
        pushLog("Radius de '" + m_selected->name + "' (Sphere Collider) cambiado a " + formatFloat(m_editSphereRadius));

    if (colliderChanged)
    {
        sc->setCenter(m_editSphereCenter);
        sc->setRadius(m_editSphereRadius);
        sc->setUseGravity(m_editSphereUseGravity);
    }
```

- [ ] **Step 3: Compilar**

```powershell
& .\build.bat
```
Expected: build sin errores.

- [ ] **Step 4: Verificación manual**

```powershell
& .\build-ninja\sandbox\Sandbox.exe
```
- [ ] Añadir Box Collider a un GameObject, arrastrar Center y soltar → línea `Center de '<name>' (Box Collider) cambiado a (...)`.
- [ ] Arrastrar Size y soltar → línea `Size de '<name>' (Box Collider) cambiado a (...)`.
- [ ] Marcar/desmarcar Use Gravity del Box Collider → línea `Use Gravity de '<name>' (Box Collider) activado`/`desactivado`.
- [ ] Repetir los 3 pasos anteriores con Sphere Collider (Center, Radius, Use Gravity) → 3 líneas con "(Sphere Collider)" en el texto.

- [ ] **Step 5: Commit**

```bash
git add engine/src/EditorUI.cpp
git commit -m "feat(log): instrumenta Box Collider y Sphere Collider"
```

---

### Task 3: Instrumentar Capsule Collider y Plane Collider + verificación final completa

**Files:**
- Modify: `engine/src/EditorUI.cpp` (`drawCapsuleColliderSection`, `drawPlaneColliderSection`)

**Interfaces:**
- Consumes: `formatVec3(const glm::vec3&) -> std::string`, `formatFloat(float) -> std::string` (Tarea 1); `EditorUI::pushLog(const std::string&) -> void` (feature Log Console previa).

- [ ] **Step 1: Instrumentar `EditorUI::drawCapsuleColliderSection`**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawCapsuleColliderSection`, reemplaza el bloque desde la declaración de `sectionOpen` hasta el `if (colliderChanged) { ... }`:

```cpp
    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Capsule Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##k1", &m_editCapsuleCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##k1", &m_editCapsuleCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##k1", &m_editCapsuleCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::Text("Radius");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##k2", &m_editCapsuleRadius, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::Text("Height");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##k3", &m_editCapsuleHeight, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        if (ImGui::Checkbox("Use Gravity", &m_editCapsuleUseGravity))
            colliderChanged = true;

        ImGui::TreePop();
    }

    m_capsuleColliderDragActive = dragActive;

    if (colliderChanged)
    {
        cc->setCenter(m_editCapsuleCenter);
        cc->setRadius(m_editCapsuleRadius);
        cc->setHalfHeight(m_editCapsuleHeight * 0.5f);
        cc->setUseGravity(m_editCapsuleUseGravity);
    }
```

por:

```cpp
    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Capsule Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;
    bool centerCommitted = false;
    bool radiusCommitted = false;
    bool heightCommitted = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##k1", &m_editCapsuleCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##k1", &m_editCapsuleCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##k1", &m_editCapsuleCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Radius");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##k2", &m_editCapsuleRadius, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        radiusCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::Text("Height");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("##k3", &m_editCapsuleHeight, 0.5f, 0.01f, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        heightCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        if (ImGui::Checkbox("Use Gravity", &m_editCapsuleUseGravity))
        {
            colliderChanged = true;
            pushLog(std::string("Use Gravity de '") + m_selected->name +
                     "' (Capsule Collider) " + (m_editCapsuleUseGravity ? "activado" : "desactivado"));
        }

        ImGui::TreePop();
    }

    m_capsuleColliderDragActive = dragActive;

    if (centerCommitted)
        pushLog("Center de '" + m_selected->name + "' (Capsule Collider) cambiado a " + formatVec3(m_editCapsuleCenter));
    if (radiusCommitted)
        pushLog("Radius de '" + m_selected->name + "' (Capsule Collider) cambiado a " + formatFloat(m_editCapsuleRadius));
    if (heightCommitted)
        pushLog("Height de '" + m_selected->name + "' (Capsule Collider) cambiado a " + formatFloat(m_editCapsuleHeight));

    if (colliderChanged)
    {
        cc->setCenter(m_editCapsuleCenter);
        cc->setRadius(m_editCapsuleRadius);
        cc->setHalfHeight(m_editCapsuleHeight * 0.5f);
        cc->setUseGravity(m_editCapsuleUseGravity);
    }
```

- [ ] **Step 2: Instrumentar `EditorUI::drawPlaneColliderSection`**

En `engine/src/EditorUI.cpp`, dentro de `EditorUI::drawPlaneColliderSection`, reemplaza el bloque desde la declaración de `sectionOpen` hasta la línea `m_planeColliderDragActive = dragActive;` (esta sección no tiene `if (colliderChanged) { ... }` con llaves — es una línea suelta `if (colliderChanged) pc->setCenter(...)`, que se deja intacta):

```cpp
    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Plane Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##p1", &m_editPlaneCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##p1", &m_editPlaneCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##p1", &m_editPlaneCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();

        ImGui::TreePop();
    }

    m_planeColliderDragActive = dragActive;

    if (colliderChanged)
        pc->setCenter(m_editPlaneCenter);
```

por:

```cpp
    ImGui::Separator();
    bool sectionOpen = ImGui::TreeNodeEx("Plane Collider", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
    bool removeClicked = ImGui::SmallButton("x");

    bool colliderChanged = false;
    bool dragActive = false;
    bool centerCommitted = false;

    if (sectionOpen)
    {
        ImGui::Text("Center");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("X##p1", &m_editPlaneCenter.x, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Y##p1", &m_editPlaneCenter.y, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5);
        colliderChanged |= ImGui::DragFloat("Z##p1", &m_editPlaneCenter.z, 0.5f, -FLT_MAX, +FLT_MAX, "% .3f");
        dragActive |= ImGui::IsItemActive();
        centerCommitted |= ImGui::IsItemDeactivatedAfterEdit();

        ImGui::TreePop();
    }

    m_planeColliderDragActive = dragActive;

    if (centerCommitted)
        pushLog("Center de '" + m_selected->name + "' (Plane Collider) cambiado a " + formatVec3(m_editPlaneCenter));

    if (colliderChanged)
        pc->setCenter(m_editPlaneCenter);
```

- [ ] **Step 3: Compilar**

```powershell
& .\build.bat
```
Expected: build sin errores.

- [ ] **Step 4: Verificación manual final (checklist completo, sin framework de tests automatizado)**

```powershell
& .\build-ninja\sandbox\Sandbox.exe
```
- [ ] Añadir Capsule Collider, arrastrar Center/Radius/Height y soltar cada uno → 3 líneas independientes con "(Capsule Collider)".
- [ ] Marcar/desmarcar Use Gravity del Capsule Collider → línea `activado`/`desactivado`.
- [ ] Añadir Plane Collider, arrastrar Center y soltar → línea `Center de '<name>' (Plane Collider) cambiado a (...)`. Confirmar que Plane NO tiene línea de Use Gravity ni Size/Radius (no existen esos campos en esa sección).
- [ ] Repaso end-to-end de las 14 instrumentaciones (Transform x3 + 4 colliders con sus campos) en una sola sesión: cada commit de drag o cada toggle de checkbox deja exactamente 1 línea, nunca una por frame de arrastre.
- [ ] Arrastrar varios campos seguidos sin soltar entre ellos (p.ej. Position.X, luego sin soltar mover a Position.Y) → ImGui trata cada eje como un widget independiente, así que cada uno logea por separado al soltar — confirmar que no aparecen líneas duplicadas ni de más.
- [ ] Seleccionar un objeto con Box Collider dinámico (`useGravity=true`) y dejar que caiga por física sin tocar ningún campo → cero líneas nuevas (la sincronización automática de Center/Size no pasa por commit de usuario).
- [ ] Confirmar que el buffer de 200 líneas (ya existente) sigue funcionando con el volumen añadido de estas 14 instrumentaciones — generar más de 200 acciones combinando drags y toggles, las líneas más antiguas desaparecen.

- [ ] **Step 5: Commit**

```bash
git add engine/src/EditorUI.cpp
git commit -m "feat(log): instrumenta Capsule Collider y Plane Collider"
```

---

## Self-Review

**Cobertura del spec:** §1 (mecanismo `IsItemDeactivatedAfterEdit`) → aplicado en las 3 tareas, cada `DragFloat` gana su lectura pegada a la línea del widget (mismo patrón que `IsItemActive` ya existente). §2 (`formatVec3`/`formatFloat`) → Tarea 1 Step 2. §3 (tabla de 14 puntos) → Position/Rotation/Scale → Tarea 1 Step 3; Center/Size/Use Gravity de Box, Center/Radius/Use Gravity de Sphere → Tarea 2; Center/Radius/Height/Use Gravity de Capsule, Center de Plane → Tarea 3. §4 (testing) → checklist repartido por tarea + checklist final consolidado en Tarea 3 Step 4 (incluye el caso del collider dinámico sin commit de usuario y el buffer de 200 líneas, explícitos en el spec). Riesgo del spec sobre `<cstdio>` → Tarea 1 Step 1. Riesgo sobre Plane sin Use Gravity/Size → Tarea 3 Step 2 (sección no lleva esos campos, ni siquiera bools de commit para ellos).

**Placeholders:** ninguno — cada paso trae el código completo before/after, verificado contra el contenido real actual de `EditorUI.cpp` en el momento de escribir este plan (post-merge de la feature Log Console base).

**Consistencia de tipos:** `formatVec3(const glm::vec3&) -> std::string` y `formatFloat(float) -> std::string` declaradas una vez (Tarea 1 Step 2) y usadas con la misma firma en las 3 tareas. Los bools de commit (`posCommitted`, `centerCommitted`, `radiusCommitted`, etc.) son locales a cada función — sin colisión de nombres entre secciones porque cada uno vive en su propio scope de función. `pushLog(const std::string&) -> void` (de la feature Log Console previa) usada con la misma firma en los ~17 call sites nuevos de este plan.
