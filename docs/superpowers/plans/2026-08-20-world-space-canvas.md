# Canvas en world-space — plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Que un `CanvasComponent` pueda dibujarse en el mundo 3D con proyección y oclusión reales, y que una escena pueda tener varios canvas a la vez.

**Architecture:** El pipeline de UI ya recibe una matriz 4×4 por push constant, así que dibujar en el mundo es pasar `proj·view·model` en vez de una ortográfica. La oclusión obliga a grabar los canvas de mundo **dentro del pase de escena** (`m_offscreenRenderPass`), que es el único con profundidad. El Renderer pasa de un `m_uiCanvas` a un vector de *slots* emparejados por `ownerId`.

**Tech Stack:** C++20, Vulkan + D3D12, glm, nlohmann/json, sol2 (Lua), ImGui, Ninja + MSVC.

**Spec:** `docs/superpowers/specs/2026-08-20-world-space-canvas-design.md`

## Global Constraints

- **Build:** `.\build.bat` vía PowerShell (nunca cmake crudo). Tests en `build-ninja\engine\tests\*.exe`.
- **Ficheros:** el repo va en **CRLF**. NO usar `sed -i` (convierte todo el fichero a LF sin avisar). NO reescribir ficheros con PowerShell `Get-Content`/`Set-Content` (rompe el UTF-8 de los comentarios acentuados). Usar la herramienta Edit, o Python con `io.open(..., newline='')`.
- **Idioma:** comentarios y mensajes de commit en español, como el resto del repo.
- **TDD obligatorio:** test primero, confirmar RED, luego implementar. Los tests de carga usan valores **no neutros y distintos entre sí** (un default no prueba que se haya leído).
- **Crash raro o LNK2019 tras tocar un header** = build stale: borrar los `.obj` propios y recompilar, no tocar el código nuevo.
- **Los 16 ejecutables de test tienen que seguir en verde** al final de cada tarea.
- `printf` a un pipe se pierde si el test crashea: para ver la salida parcial, `dt_camera_tests.exe` ya usa `setvbuf(stdout, nullptr, _IONBF, 0)`; `dt_ui_batch_tests.exe` también.

---

## Estructura de ficheros

**Crear:**

| Fichero | Responsabilidad |
| --- | --- |
| `engine/include/DonTopo/UI/UiWidgetSync.h` | `UiWidgetLists`, `UiWidgetSyncCache`, `UiCanvasBinding`, `syncUiWidgets`. Hoy vive en `TextComponent.h`, que no tiene nada que ver |

**Modificar:**

| Fichero | Qué cambia |
| --- | --- |
| `engine/include/DonTopo/UI/CanvasComponent.h` | 4 campos, 2 enums, `uiWorldCanvasMatrix` |
| `engine/include/DonTopo/UI/TextComponent.h` | pierde la maquinaria de sync; incluye `UiWidgetSync.h` |
| `engine/include/DonTopo/Core/Scene.h`, `engine/src/Core/Scene.cpp` | `collectCanvases` sustituye a `collectUiWidgets`; serialización |
| `engine/include/DonTopo/Renderer/EditorRenderer.h` | 2 virtuales nuevas |
| `engine/include/DonTopo/Renderer/Renderer.h`, `engine/src/Renderer/Renderer.cpp` | slots, `syncUiCanvases`, `findUiNode`, pipelines de mundo, grabado |
| `engine/include/DonTopo/UI/UiSpriteBatch.h`, `engine/src/UI/UiSpriteBatch.cpp` | `record` recibe la matriz; variantes de pipeline |
| `engine/include/DonTopo/Renderer/D3D12/D3D12Renderer.h`, `engine/src/Renderer/D3D12/D3D12Renderer.cpp` | lo mismo, en D3D12 |
| `shaders/ui.frag` (o el que use `UiSpriteBatch`) | conversión sRGB→lineal en la variante de mundo |
| `engine/src/Editor/PropertiesPanel.cpp/.h` | campos nuevos en la sección Canvas |
| `engine/src/Editor/ViewportPanel.cpp` | `findUiNode`; gizmo del canvas en modo World |
| `engine/src/Scripting/ScriptBindings.cpp`, `LuaApiReference.cpp`, `Scripts/README.md` | bindings y docs |
| `runtime/main.cpp`, `sandbox/src/main.cpp` | los 3 bucles |
| `engine/tests/camera_tests.cpp`, `engine/tests/ui_batch_tests.cpp` | los tests |

**Sin cambios (comprobado):** `Command.h`/`Command.cpp` — `CanvasComponentCommand` ya guarda una **copia del componente entero** por valor, así que los 4 campos nuevos entran solos en el undo.

---

### Task 1: Mover la maquinaria de sync a su propio header

Puro movimiento de texto. Sin cambio de comportamiento: es la red de seguridad de todo lo que viene después.

**Files:**
- Create: `engine/include/DonTopo/UI/UiWidgetSync.h`
- Modify: `engine/include/DonTopo/UI/TextComponent.h`

**Interfaces:**
- Consumes: nada.
- Produces: `DonTopo::UiWidgetLists`, `DonTopo::UiWidgetSyncCache`, `DonTopo::syncUiWidgets` — **mismos nombres, mismo namespace, mismas firmas** que hoy, solo que en otro fichero.

- [ ] **Step 1: Localizar el bloque a mover**

En `TextComponent.h`, el bloque va desde el comentario `// Los widgets de UI de una escena, una lista por tipo y la jerarquía` (justo antes de `struct UiWidgetLists`) hasta el final de `syncUiWidgets`, que es la última llave antes del `}` del namespace.

- [ ] **Step 2: Crear `UiWidgetSync.h` con el bloque movido**

Cabecera del fichero nuevo:

```cpp
#pragma once
// La maquinaria que convierte los componentes de UI de la escena en el árbol
// vivo del canvas. Vivía en TextComponent.h por accidente histórico: el primer
// widget que la necesitó fue el Text, y se quedó ahí. No tiene nada que ver con
// el componente de texto.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/CheckboxComponent.h"
#include "DonTopo/UI/DropdownComponent.h"
#include "DonTopo/UI/ImageComponent.h"
#include "DonTopo/UI/InputFieldComponent.h"
#include "DonTopo/UI/LayoutComponent.h"
#include "DonTopo/UI/PanelComponent.h"
#include "DonTopo/UI/ProgressBarComponent.h"
#include "DonTopo/UI/ScrollViewComponent.h"
#include "DonTopo/UI/ScrollbarComponent.h"
#include "DonTopo/UI/SliderComponent.h"
#include "DonTopo/UI/TextComponent.h"
#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiFont.h"
#include "DonTopo/UI/UiTextureAtlas.h"
#include "DonTopo/UI/UiWidgets.h"

namespace DonTopo
{
    // ... aquí el bloque movido, TAL CUAL ...
}
```

- [ ] **Step 3: En `TextComponent.h`, borrar el bloque y añadir el include al final**

Al final de `TextComponent.h`, **fuera** del `namespace DonTopo`:

```cpp
// Compatibilidad: la maquinaria de sync se mudó a UiWidgetSync.h y hay ~80
// puntos de llamada que incluyen este fichero esperando encontrarla. Se incluye
// al FINAL a propósito: UiWidgetSync.h necesita TextComponent completo.
#include "DonTopo/UI/UiWidgetSync.h"
```

- [ ] **Step 4: Compilar**

Run: `.\build.bat`
Expected: sin errores. Si sale un LNK2019 o un C2065 raro, es build stale — borrar los `.obj` y recompilar.

- [ ] **Step 5: Los 16 tests en verde**

Run:
```powershell
$fail=0; Get-ChildItem build-ninja\engine\tests\*.exe | ForEach-Object { & $_.FullName | Out-Null; if ($LASTEXITCODE -ne 0) { $fail++; Write-Output $_.Name } }; Write-Output "FALLOS: $fail"
```
Expected: `FALLOS: 0`

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/UI/UiWidgetSync.h engine/include/DonTopo/UI/TextComponent.h
git commit -m "refactor(ui): la maquinaria de sync sale de TextComponent.h"
```

---

### Task 2: `CanvasComponent` gana los cuatro campos

Datos, serialización, inspector, Lua y docs. Sin tocar el render: al acabar, un canvas en modo World se guarda y se edita, pero todavía se dibuja como uno de pantalla.

**Files:**
- Modify: `engine/include/DonTopo/UI/CanvasComponent.h`
- Modify: `engine/src/Core/Scene.cpp` (bloque `canvas` de `nodeToJson` y de `nodeFromJson`)
- Modify: `engine/src/Editor/PropertiesPanel.cpp` (`drawCanvasSection`)
- Modify: `engine/src/Scripting/ScriptBindings.cpp`, `engine/src/Scripting/LuaApiReference.cpp`, `Scripts/README.md`
- Test: `engine/tests/camera_tests.cpp`, `engine/tests/scripting_tests.cpp`

**Interfaces:**
- Consumes: nada.
- Produces:
  - `enum class UiCanvasRenderMode { ScreenSpace, World };`
  - `enum class UiBillboard { None, YawOnly, Full };`
  - Campos `CanvasComponent::renderMode`, `::worldScale`, `::billboard`, `::depthTest`.
  - Claves JSON `"renderMode"` (`"screenSpace"` | `"world"`), `"worldScale"`, `"billboard"` (`"none"` | `"yawOnly"` | `"full"`), `"depthTest"`.

- [ ] **Step 1: Escribir el test de round-trip (RED)**

En `camera_tests.cpp`, junto a los demás tests de canvas:

```cpp
// Los cuatro campos nuevos del Canvas, con valores NO neutros y distintos entre
// sí: con los defaults, un fromJson que se saltara el campo pasaría igual.
static void test_canvas_world_fields_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cartel");
    auto c = std::make_shared<CanvasComponent>();
    c->renderMode = UiCanvasRenderMode::World;
    c->worldScale = 0.0234375f;
    c->billboard  = UiBillboard::YawOnly;
    c->depthTest  = false;
    go->setCanvas(c);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(scene.toJson(), pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n) { if (!found && n->hasCanvas()) found = n; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->getCanvas()->renderMode == UiCanvasRenderMode::World);
    CHECK(nearlyEqual(found->getCanvas()->worldScale, 0.0234375f));
    CHECK(found->getCanvas()->billboard == UiBillboard::YawOnly);
    CHECK(found->getCanvas()->depthTest == false);
    CHECK(loaded.lastWarnings().empty());
}

// Una escena guardada antes de estos campos carga con los defaults y sin avisos:
// bloque aditivo, misma regla que todos los componentes de UI.
static void test_canvas_without_world_fields_loads_with_defaults(PhysicsManager& pm,
                                                                  AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Viejo");
    go->setCanvas(std::make_shared<CanvasComponent>());
    nlohmann::json j = scene.toJson();
    // Se borran las claves nuevas para simular una escena antigua.
    for (auto& n : j["root"]["children"])
        if (n.contains("canvas"))
            for (const char* k : { "renderMode", "worldScale", "billboard", "depthTest" })
                n["canvas"].erase(k);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = nullptr;
    loaded.traverse([&](GameObject* n2) { if (!found && n2->hasCanvas()) found = n2; });
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->getCanvas()->renderMode == UiCanvasRenderMode::ScreenSpace);
    CHECK(nearlyEqual(found->getCanvas()->worldScale, 0.001f));
    CHECK(found->getCanvas()->billboard == UiBillboard::None);
    CHECK(found->getCanvas()->depthTest == true);
    CHECK(loaded.lastWarnings().empty());
}
```

Registrar las dos en `main()`.

La ruta `j["root"]["children"]` está comprobada: `Scene::toJson` hace
`root["root"] = nodeToJson(m_root)` y cada nodo lleva su `["children"]`.

- [ ] **Step 2: Confirmar RED**

Run: `.\build.bat`
Expected: FAIL — `"UiCanvasRenderMode": no es un miembro de "DonTopo"` (el enum aún no existe).

- [ ] **Step 3: Añadir enums y campos a `CanvasComponent.h`**

```cpp
// Dónde se dibuja el canvas. ScreenSpace es lo de siempre: una ortográfica en
// píxeles de salida, encima de todo. World lo coloca EN LA ESCENA, con la
// perspectiva de la cámara y tapado por la geometría que tenga delante.
enum class UiCanvasRenderMode { ScreenSpace, World };

// Cómo se orienta un canvas de mundo respecto a la cámara. YawOnly gira solo
// alrededor de la vertical del mundo: es lo que quiere una barra de vida, que no
// debe tumbarse al mirar desde arriba. Full lo encara del todo, que es lo que
// quiere un icono.
enum class UiBillboard { None, YawOnly, Full };
```

Y dentro de la clase, tras los 10 campos de hoy:

```cpp
            // --- Modo de dibujado ---------------------------------------------
            UiCanvasRenderMode renderMode = UiCanvasRenderMode::ScreenSpace;

            // --- Solo World ----------------------------------------------------
            // En modo World el área útil es EXACTAMENTE referenceResolution: no
            // hay pantalla a la que ajustarse, así que scaleMode, screenMatch,
            // matchWidthOrHeight, los tres DPI, safeArea y aspectRatio NO SE
            // LEEN. No se esconden en el editor: se documenta el matiz.
            float       worldScale = 0.001f;   // unidades de mundo por PÍXEL de canvas
            UiBillboard billboard  = UiBillboard::None;
            // A false el canvas se dibuja siempre encima, atravesando paredes: es
            // lo que quiere una barra de vida que no debe perderse de vista.
            bool        depthTest  = true;
```

- [ ] **Step 4: `applyTo` respeta el modo World**

```cpp
            void applyTo(UiCanvas& canvas) const
            {
                if (renderMode == UiCanvasRenderMode::World)
                {
                    // Un canvas de mundo no se ajusta a ninguna pantalla: su área
                    // útil es su resolución de referencia y punto. Volcar aquí el
                    // scaleMode o el safe area haría que el cartel cambiara de
                    // tamaño al redimensionar la ventana, que es justo lo que un
                    // objeto del mundo NO debe hacer.
                    canvas.scaleMode           = UiScaleMode::ConstantPixelSize;
                    canvas.scaleFactor         = 1.0f;
                    canvas.referenceResolution = referenceResolution;
                    canvas.screenMatch         = UiScreenMatch::MatchWidthOrHeight;
                    canvas.matchWidthOrHeight  = 0.5f;
                    canvas.screenDpi           = 0.0f;
                    canvas.fallbackDpi         = 96.0f;
                    canvas.referenceDpi        = 96.0f;
                    canvas.safeArea            = UiSafeArea{};
                    canvas.aspectRatio         = 0.0f;
                    return;
                }

                canvas.scaleMode           = scaleMode;
                canvas.scaleFactor         = scaleFactor;
                canvas.referenceResolution = referenceResolution;
                canvas.screenMatch         = screenMatch;
                canvas.matchWidthOrHeight  = matchWidthOrHeight;
                canvas.screenDpi           = screenDpi;
                canvas.fallbackDpi         = fallbackDpi;
                canvas.referenceDpi        = referenceDpi;
                canvas.safeArea            = safeArea;
                canvas.aspectRatio         = aspectRatio;
            }
```

- [ ] **Step 5: Serialización en `Scene.cpp`**

Añadir los conversores junto a los que ya hay (`uiScaleModeToStr` y compañía):

```cpp
    const char* uiCanvasRenderModeToStr(UiCanvasRenderMode m)
    {
        return m == UiCanvasRenderMode::World ? "world" : "screenSpace";
    }

    UiCanvasRenderMode uiCanvasRenderModeFromStr(const std::string& s)
    {
        return s == "world" ? UiCanvasRenderMode::World : UiCanvasRenderMode::ScreenSpace;
    }

    const char* uiBillboardToStr(UiBillboard b)
    {
        switch (b)
        {
            case UiBillboard::YawOnly: return "yawOnly";
            case UiBillboard::Full:    return "full";
            default:                   return "none";
        }
    }

    UiBillboard uiBillboardFromStr(const std::string& s)
    {
        if (s == "yawOnly") return UiBillboard::YawOnly;
        if (s == "full")    return UiBillboard::Full;
        return UiBillboard::None;   // valor desconocido -> el default
    }
```

En el bloque `j["canvas"] = {...}` añadir las 4 claves. En el `fromJson` del canvas añadir las 4 lecturas, con `readFloat` para `worldScale` y los `readStr`/`readBool` locales del bloque para el resto.

- [ ] **Step 6: Verde**

Run: `.\build.bat` y `.\build-ninja\engine\tests\dt_camera_tests.exe`
Expected: `ALL CAMERA TESTS PASSED`

- [ ] **Step 7: Inspector**

En `drawCanvasSection` (`PropertiesPanel.cpp`), con los helpers que ya tiene esa sección:

```cpp
        ImGui::TextDisabled("Modo");
        static const char* kRenderModes[] = { "Screen Space", "World" };
        comboEnum("Render Mode", (int)c->renderMode, kRenderModes, IM_ARRAYSIZE(kRenderModes),
                  +[](CanvasComponent& x, int v) { x.renderMode = (UiCanvasRenderMode)v; });

        if (c->renderMode == UiCanvasRenderMode::World)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                               "En World se ignoran: Scale Mode, Screen Match, Match,\n"
                               "los tres DPI, Safe Area y Aspect Ratio.");

            dragFloat("World Scale", +[](CanvasComponent& x) -> float& { return x.worldScale; },
                      0.0001f, 0.0f, 10.0f, "%.4f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Unidades de mundo por PIXEL de canvas.\n"
                                  "Un canvas de 1920x1080 a 0.001 mide 1.92 x 1.08 unidades.");

            static const char* kBillboards[] = { "None", "Yaw Only", "Full" };
            comboEnum("Billboard", (int)c->billboard, kBillboards, IM_ARRAYSIZE(kBillboards),
                      +[](CanvasComponent& x, int v) { x.billboard = (UiBillboard)v; });

            checkBox("Depth Test", +[](CanvasComponent& x) -> bool& { return x.depthTest; });
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A false se dibuja siempre encima, atravesando paredes");
        }
```

**Comprobado:** `drawCanvasSection` ya tiene `comboEnum`, `dragFloat` y
`dragVec2`, pero **NO tiene `checkBox`**. Hay que añadirlo junto a los otros,
antes del bloque de arriba:

```cpp
        using BoolRef = bool& (*)(CanvasComponent&);
        auto checkBox = [&](const char* label, BoolRef acc)
        {
            const bool before = acc(*c);
            bool       val    = before;
            if (ImGui::Checkbox(label, &val) && val != before)
            {
                acc(*c) = val;
                const std::string lbl = std::string(label) + " del canvas de '" + owner + "'";
                ctx.pushLog(lbl + (val ? " activado" : " desactivado"));
                if (scene && ctx.undo)
                    ctx.undo->push(std::make_unique<PropertyCommand<bool>>(
                        lbl, before, val,
                        [scene, id, acc](const bool& v) {
                            if (GameObject* go = scene->findById(id))
                                if (go->hasCanvas()) acc(*go->getCanvas()) = v;
                        }));
            }
        };
```

Verificar que `scene`, `id` y `owner` existen ya en esa función con esos
nombres; si no, copiarlos del patrón de `drawPanelSection`.

- [ ] **Step 8: Lua (test primero)**

En `scripting_tests.cpp`, dentro del test de Canvas que ya existe (o uno nuevo si no lo hay), añadir:

```cpp
    auto r = sm.lua().safe_script(R"(
        local c = e:AddCanvas()
        c.renderMode = UiCanvasRenderMode.World
        c.worldScale = 0.0234375
        c.billboard = UiBillboard.YawOnly
        c.depthTest = false
        modoLeido = c.renderMode
    )", sol::script_pass_on_error);
    CHECK(r.valid());
```
y las comprobaciones en C++ contra `go->getCanvas()`.

Confirmar RED (el script falla: `UiCanvasRenderMode` no existe), luego registrar en `ScriptBindings.cpp`:

```cpp
            lua["UiCanvasRenderMode"] = lua.create_table_with("ScreenSpace", 0, "World", 1);
            lua["UiBillboard"] = lua.create_table_with("None", 0, "YawOnly", 1, "Full", 2);
```

y en el usertype `Canvas`:

```cpp
                "renderMode", uiEnumProp(canvasOf, &CanvasComponent::renderMode, 1),
                "worldScale", uiFloatProp(canvasOf, &CanvasComponent::worldScale, &mgr, "Canvas.worldScale"),
                "billboard",  uiEnumProp(canvasOf, &CanvasComponent::billboard, 2),
                "depthTest",  uiProp(canvasOf, &CanvasComponent::depthTest),
```

Añadir los símbolos a `LuaApiReference.cpp` y la fila a `Scripts/README.md`.

- [ ] **Step 9: Verde y commit**

Run: los 16 ejecutables.
Expected: `FALLOS: 0`

```bash
git add -A
git commit -m "feat(ui): el Canvas declara su modo de dibujado y sus parametros de mundo"
```

---

### Task 3: `uiWorldCanvasMatrix`

Matemática pura, probable sin GPU. Es la pieza que más fácil se equivoca en silencio: un signo mal puesto pinta el cartel del revés y ninguna capa de validación lo dice.

**Files:**
- Modify: `engine/include/DonTopo/UI/CanvasComponent.h`
- Test: `engine/tests/ui_batch_tests.cpp`

**Interfaces:**
- Consumes: `CanvasComponent`, `UiBillboard` (Task 2).
- Produces:
```cpp
glm::mat4 uiWorldCanvasMatrix(const CanvasComponent& c, glm::vec2 canvasSize,
                              const glm::mat4& worldTransform, const glm::mat4& view);
```

- [ ] **Step 1: Escribir los tests (RED)**

En `ui_batch_tests.cpp`:

```cpp
// La matriz de un canvas de mundo. Tres cosas que fallan EN SILENCIO: que el
// canvas quede centrado en el objeto (si no, aparece desplazado media pantalla),
// que la Y esté VOLTEADA (el canvas crece hacia abajo y el mundo hacia arriba: un
// signo de más pinta el cartel boca abajo) y que la escala sea unidades por
// pixel y no al revés.
static void test_world_canvas_matrix_centra_y_voltea_la_y()
{
    CanvasComponent c;
    c.renderMode = UiCanvasRenderMode::World;
    c.worldScale = 0.001f;
    c.billboard  = UiBillboard::None;

    const glm::vec2 tam(1920.0f, 1080.0f);
    const glm::mat4 mundo = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 5.0f, -3.0f));
    const glm::mat4 vista(1.0f);

    const glm::mat4 m = uiWorldCanvasMatrix(c, tam, mundo, vista);

    // El CENTRO del canvas (960, 540) cae en la posicion del GameObject.
    const glm::vec4 centro = m * glm::vec4(960.0f, 540.0f, 0.0f, 1.0f);
    CHECK(nearlyEqual(centro.x, 10.0f));
    CHECK(nearlyEqual(centro.y, 5.0f));
    CHECK(nearlyEqual(centro.z, -3.0f));

    // La esquina (0,0) del canvas es la de ARRIBA a la izquierda, asi que en el
    // mundo cae a la izquierda y ARRIBA del centro.
    const glm::vec4 sup = m * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    CHECK(nearlyEqual(sup.x, 10.0f - 0.96f));
    CHECK(nearlyEqual(sup.y, 5.0f + 0.54f));

    // Y la (w,h) abajo a la derecha.
    const glm::vec4 inf = m * glm::vec4(1920.0f, 1080.0f, 0.0f, 1.0f);
    CHECK(nearlyEqual(inf.x, 10.0f + 0.96f));
    CHECK(nearlyEqual(inf.y, 5.0f - 0.54f));

    // El tamano total: 1.92 x 1.08 unidades.
    CHECK(nearlyEqual(inf.x - sup.x, 1.92f));
    CHECK(nearlyEqual(sup.y - inf.y, 1.08f));
}

// worldScale es unidades por PIXEL: doblarlo dobla el cartel.
static void test_world_canvas_matrix_escala()
{
    CanvasComponent c;
    c.renderMode = UiCanvasRenderMode::World;
    c.worldScale = 0.002f;

    const glm::mat4 m = uiWorldCanvasMatrix(c, glm::vec2(100.0f, 50.0f),
                                            glm::mat4(1.0f), glm::mat4(1.0f));
    const glm::vec4 a = m * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const glm::vec4 b = m * glm::vec4(100.0f, 0.0f, 0.0f, 1.0f);
    CHECK(nearlyEqual(b.x - a.x, 0.2f));
}

// Billboard. La camara mira desde +Z hacia el origen; el canvas esta en el
// origen con una rotacion cualquiera que el billboard tiene que PISAR.
static void test_world_canvas_matrix_billboard()
{
    CanvasComponent c;
    c.renderMode = UiCanvasRenderMode::World;
    c.worldScale = 0.001f;

    // Rotacion absurda del objeto: si el billboard no la pisa, se nota.
    glm::mat4 mundo = glm::rotate(glm::mat4(1.0f), 1.1f, glm::vec3(0.3f, 0.5f, 0.8f));

    // Camara en (0, 4, 6) mirando al origen: mira hacia abajo y hacia -Z.
    const glm::mat4 vista = glm::lookAt(glm::vec3(0.0f, 4.0f, 6.0f),
                                        glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    // Sin billboard la rotacion del objeto manda: el eje X del canvas NO es el
    // (1,0,0) del mundo.
    c.billboard = UiBillboard::None;
    {
        const glm::mat4 m = uiWorldCanvasMatrix(c, glm::vec2(100.0f, 100.0f), mundo, vista);
        const glm::vec3 ejeX = glm::normalize(glm::vec3(m[0]));
        CHECK(!nearlyEqual(ejeX.x, 1.0f));
    }

    // YawOnly: el eje VERTICAL del canvas sigue siendo el del mundo (no se
    // tumba al mirar desde arriba), que es lo que quiere una barra de vida.
    c.billboard = UiBillboard::YawOnly;
    {
        const glm::mat4 m = uiWorldCanvasMatrix(c, glm::vec2(100.0f, 100.0f), mundo, vista);
        const glm::vec3 ejeY = glm::normalize(glm::vec3(m[1]));
        CHECK(nearlyEqual(ejeY.x, 0.0f));
        CHECK(nearlyEqual(std::fabs(ejeY.y), 1.0f));
        CHECK(nearlyEqual(ejeY.z, 0.0f));
    }

    // Full: encara la camara del todo, asi que el eje Y del canvas SE INCLINA.
    c.billboard = UiBillboard::Full;
    {
        const glm::mat4 m = uiWorldCanvasMatrix(c, glm::vec2(100.0f, 100.0f), mundo, vista);
        const glm::vec3 ejeY = glm::normalize(glm::vec3(m[1]));
        CHECK(!nearlyEqual(std::fabs(ejeY.y), 1.0f));
    }
}
```

Registrar las tres en `main()` de `ui_batch_tests.cpp`. Añadir los includes que falten (`CanvasComponent.h`, `<glm/gtc/matrix_transform.hpp>`).

- [ ] **Step 2: Confirmar RED**

Run: `.\build.bat`
Expected: FAIL — `uiWorldCanvasMatrix: identificador no declarado`.

- [ ] **Step 3: Implementar en `CanvasComponent.h`**

```cpp
    // Matriz de MODELO de un canvas de mundo: de píxeles del canvas a unidades
    // del mundo. Función libre y no método a propósito — necesita la vista de la
    // cámara para el billboard, y el componente no tiene por qué saber de
    // cámaras. Aquí y no en el Renderer para poder probarla sin GPU.
    //
    // El canvas crece hacia ABAJO y el mundo hacia ARRIBA, así que la Y va
    // NEGADA. Y el canvas se centra en el objeto: su píxel (w/2, h/2) cae
    // exactamente en la posición del GameObject.
    inline glm::mat4 uiWorldCanvasMatrix(const CanvasComponent& c, glm::vec2 canvasSize,
                                         const glm::mat4& worldTransform, const glm::mat4& view)
    {
        const float s = c.worldScale;

        // Base del objeto: su transform, o una que mire a la cámara si hay
        // billboard. La POSICIÓN siempre sale del transform; lo que el billboard
        // sustituye es la rotación (y con ella la escala del objeto, que en un
        // canvas encarado no significa nada).
        glm::mat4 base = worldTransform;
        if (c.billboard != UiBillboard::None)
        {
            const glm::vec3 pos = glm::vec3(worldTransform[3]);

            // Los ejes de la CÁMARA salen de la inversa de la vista: las filas de
            // la parte rotacional de `view` son sus ejes en el mundo.
            const glm::vec3 camDerecha = glm::vec3(view[0][0], view[1][0], view[2][0]);
            const glm::vec3 camArriba  = glm::vec3(view[0][1], view[1][1], view[2][1]);
            const glm::vec3 camAtras   = glm::vec3(view[0][2], view[1][2], view[2][2]);

            glm::vec3 derecha, arriba, adelante;
            if (c.billboard == UiBillboard::Full)
            {
                derecha  = camDerecha;
                arriba   = camArriba;
                adelante = camAtras;
            }
            else   // YawOnly: gira solo alrededor de la vertical del MUNDO
            {
                arriba = glm::vec3(0.0f, 1.0f, 0.0f);
                // Proyectar el "hacia atrás" de la cámara sobre el plano
                // horizontal. Mirando en vertical justa el vector se anula: en ese
                // caso vale cualquier orientación, y se coge una fija en vez de
                // normalizar un cero (que daría NaN y borraría el canvas entero).
                glm::vec3 plano(camAtras.x, 0.0f, camAtras.z);
                const float largo2 = glm::dot(plano, plano);
                adelante = (largo2 > 1e-8f) ? plano * glm::inversesqrt(largo2)
                                            : glm::vec3(0.0f, 0.0f, 1.0f);
                derecha  = glm::normalize(glm::cross(arriba, adelante));
            }

            base = glm::mat4(1.0f);
            base[0] = glm::vec4(derecha,  0.0f);
            base[1] = glm::vec4(arriba,   0.0f);
            base[2] = glm::vec4(adelante, 0.0f);
            base[3] = glm::vec4(pos,      1.0f);
        }

        // Píxeles -> unidades, con la Y negada, y centrado.
        glm::mat4 local(1.0f);
        local[0][0] =  s;
        local[1][1] = -s;
        local[2][2] =  s;
        local[3]    = glm::vec4(-canvasSize.x * 0.5f * s, canvasSize.y * 0.5f * s, 0.0f, 1.0f);

        return base * local;
    }
```

**Nota:** `CanvasComponent.h` necesitará `#include <glm/gtc/matrix_transform.hpp>` y `<glm/geometric.hpp>` si no los arrastra ya por `UiCanvas.h`.

- [ ] **Step 4: Verde**

Run: `.\build.bat` y `.\build-ninja\engine\tests\dt_ui_batch_tests.exe`
Expected: `ui_batch_tests: OK`

- [ ] **Step 5: Sabotaje — devolver la ortográfica**

Sustituir temporalmente el `return base * local;` por `return glm::mat4(1.0f);`, recompilar y correr `dt_ui_batch_tests.exe`.
Expected: fallan los tres tests nuevos. Revertir.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/UI/CanvasComponent.h engine/tests/ui_batch_tests.cpp
git commit -m "feat(ui): matriz de modelo de un canvas de mundo, con billboard"
```

---

### Task 4: `Scene::collectCanvases` sustituye a `collectUiWidgets`

**Files:**
- Modify: `engine/include/DonTopo/UI/UiWidgetSync.h` (añadir `UiCanvasBinding`)
- Modify: `engine/include/DonTopo/Core/Scene.h`, `engine/src/Core/Scene.cpp`
- Modify: `engine/tests/camera_tests.cpp` (los 6 puntos de llamada + tests nuevos)

**Interfaces:**
- Consumes: `UiWidgetLists` (Task 1), `CanvasComponent` (Task 2).
- Produces:
```cpp
struct UiCanvasBinding {
    uint64_t               ownerId = 0;
    const CanvasComponent* canvas  = nullptr;
    glm::mat4              worldTransform{1.0f};
    UiWidgetLists          widgets;
};
void Scene::collectCanvases(std::vector<UiCanvasBinding>& out) const;
```
`Scene::collectUiWidgets` **deja de existir**.

- [ ] **Step 1: Escribir los tests (RED)**

En `camera_tests.cpp`:

```cpp
// Cada widget va al canvas del que cuelga, no a un saco comun. Con un solo
// canvas esto daba igual; con dos, meterlos todos en el primero pinta la UI del
// menu de pausa encima del HUD y nada lo dice.
static void test_collect_canvases_agrupa_por_canvas()
{
    Scene scene;
    GameObject* hud = scene.addGameObject("HUD");
    hud->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* vida = scene.addGameObject("Vida", hud);
    vida->setProgressBar(std::make_shared<ProgressBarComponent>());

    GameObject* menu = scene.addGameObject("Menu");
    menu->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* jugar = scene.addGameObject("Jugar", menu);
    jugar->setButton(std::make_shared<ButtonComponent>());
    GameObject* salir = scene.addGameObject("Salir", menu);
    salir->setButton(std::make_shared<ButtonComponent>());

    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);

    CHECK(bindings.size() == 2);
    if (bindings.size() != 2) return;
    CHECK(bindings[0].ownerId == hud->id);
    CHECK(bindings[0].widgets.bars.size() == 1);
    CHECK(bindings[0].widgets.buttons.empty());
    CHECK(bindings[1].ownerId == menu->id);
    CHECK(bindings[1].widgets.buttons.size() == 2);
    CHECK(bindings[1].widgets.bars.empty());
}

// Canvas dentro de canvas: gana el MAS CERCANO hacia arriba, sin reglas nuevas.
static void test_collect_canvases_anidado_gana_el_mas_cercano()
{
    Scene scene;
    GameObject* fuera = scene.addGameObject("Fuera");
    fuera->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* deFuera = scene.addGameObject("DeFuera", fuera);
    deFuera->setPanel(std::make_shared<PanelComponent>());

    GameObject* dentro = scene.addGameObject("Dentro", fuera);
    dentro->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* deDentro = scene.addGameObject("DeDentro", dentro);
    deDentro->setPanel(std::make_shared<PanelComponent>());

    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);

    CHECK(bindings.size() == 2);
    if (bindings.size() != 2) return;
    CHECK(bindings[0].ownerId == fuera->id);
    CHECK(bindings[0].widgets.panels.size() == 1);
    if (bindings[0].widgets.panels.size() == 1)
        CHECK(bindings[0].widgets.panels[0].first == deFuera->id);
    CHECK(bindings[1].ownerId == dentro->id);
    CHECK(bindings[1].widgets.panels.size() == 1);
    if (bindings[1].widgets.panels.size() == 1)
        CHECK(bindings[1].widgets.panels[0].first == deDentro->id);
}

// Un widget sin NINGUN canvas por encima no va a ninguna parte. Antes acababa
// en el canvas unico aunque no colgara de el; ahora no se dibuja. Es un cambio
// de comportamiento DELIBERADO: el editor ya lo impide (uiComponentsAvailable
// exige un Canvas ancestro) y solo afecta a escenas hechas a mano.
static void test_collect_canvases_ignora_los_huerfanos()
{
    Scene scene;
    GameObject* suelto = scene.addGameObject("Suelto");
    suelto->setButton(std::make_shared<ButtonComponent>());

    GameObject* hud = scene.addGameObject("HUD");
    hud->setCanvas(std::make_shared<CanvasComponent>());

    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);

    CHECK(bindings.size() == 1);
    if (bindings.empty()) return;
    CHECK(bindings[0].ownerId == hud->id);
    CHECK(bindings[0].widgets.buttons.empty());
}

// La jerarquia de cada binding es la SUYA: los ids de padre apuntan dentro del
// mismo canvas, y el primer nivel cuelga de 0 (la raiz de ESE canvas).
static void test_collect_canvases_jerarquia_por_canvas()
{
    Scene scene;
    GameObject* hud = scene.addGameObject("HUD");
    hud->setCanvas(std::make_shared<CanvasComponent>());
    GameObject* marco = scene.addGameObject("Marco", hud);
    marco->setPanel(std::make_shared<PanelComponent>());
    GameObject* icono = scene.addGameObject("Icono", marco);
    icono->setImage(std::make_shared<ImageComponent>());

    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);
    CHECK(bindings.size() == 1);
    if (bindings.empty()) return;

    const auto& p = bindings[0].widgets.parents;
    CHECK(p.size() == 2);
    if (p.size() != 2) return;
    CHECK(p[0].first == marco->id);
    CHECK(p[0].second == 0ull);          // primer nivel del canvas
    CHECK(p[1].first == icono->id);
    CHECK(p[1].second == marco->id);
}
```

Registrar las cuatro en `main()`.

- [ ] **Step 2: Confirmar RED**

Run: `.\build.bat`
Expected: FAIL — `collectCanvases: no es miembro de "DonTopo::Scene"`.

- [ ] **Step 3: `UiCanvasBinding` en `UiWidgetSync.h`**

```cpp
    // Un canvas de la escena con TODO lo que le cuelga. Es lo que
    // Scene::collectCanvases produce y lo que el Renderer consume.
    //
    // Los widgets van agrupados POR CANVAS y no en un saco común: con un solo
    // canvas daba igual, pero con dos, meterlos todos en el primero pinta el menú
    // de pausa encima del HUD sin que nada lo diga.
    struct UiCanvasBinding
    {
        uint64_t               ownerId = 0;         // GameObject del Canvas
        const CanvasComponent* canvas  = nullptr;
        // Del GameObject del canvas. Solo lo lee el modo World; en pantalla no
        // significa nada (la UI de pantalla no está en el mundo).
        glm::mat4              worldTransform{1.0f};
        UiWidgetLists          widgets;
    };
```

`UiWidgetSync.h` necesita `#include "DonTopo/UI/CanvasComponent.h"`.

- [ ] **Step 4: `collectCanvases` en `Scene.cpp`**

Reemplaza a `collectUiWidgets`. El walker actual arrastra `uiAncestor`; ahora arrastra además el **índice del binding** del canvas ancestro (`-1` = ninguno):

```cpp
    void Scene::collectCanvases(std::vector<UiCanvasBinding>& out) const
    {
        out.clear();

        struct Walker
        {
            std::vector<UiCanvasBinding>& out;

            // canvasIdx: en qué binding caen los widgets de este subárbol (-1 =
            // ninguno todavía). uiAncestor: el ancestro con UI DENTRO de ese
            // mismo canvas, que es contra quien se anclan los hijos.
            void visit(const GameObject* node, int canvasIdx, uint64_t uiAncestor)
            {
                if (node->hasCanvas())
                {
                    // Un canvas ANIDADO abre binding propio y CORTA la cadena de
                    // anclaje: lo que cuelgue de él se ancla a su raíz, no al
                    // widget que hubiera por encima en el canvas de fuera.
                    UiCanvasBinding b;
                    b.ownerId        = node->id;
                    b.canvas         = node->getCanvas().get();
                    b.worldTransform = node->worldTransform;
                    out.push_back(std::move(b));
                    canvasIdx  = (int)out.size() - 1;
                    uiAncestor = 0;
                }

                const bool tieneUi = node->hasButton() || node->hasText() ||
                                     node->hasProgressBar() || node->hasLayout() ||
                                     node->hasPanel() || node->hasImage() ||
                                     node->hasSlider() || node->hasCheckbox() ||
                                     node->hasToggle() || node->hasScrollbar() ||
                                     node->hasInputField() || node->hasDropdown() ||
                                     node->hasScrollView();

                // Sin canvas por encima, un widget no va a ninguna parte. El
                // editor ya lo impide (uiComponentsAvailable), así que esto solo
                // pasa en escenas hechas a mano.
                if (tieneUi && canvasIdx >= 0)
                {
                    UiWidgetLists& w = out[(size_t)canvasIdx].widgets;
                    if (node->hasScrollView())  w.scrollViews.emplace_back(node->id, node->getScrollView().get());
                    if (node->hasPanel())       w.panels.emplace_back(node->id, node->getPanel().get());
                    if (node->hasImage())       w.images.emplace_back(node->id, node->getImage().get());
                    if (node->hasSlider())      w.sliders.emplace_back(node->id, node->getSlider().get());
                    if (node->hasScrollbar())   w.scrollbars.emplace_back(node->id, node->getScrollbar().get());
                    if (node->hasToggle())      w.toggles.emplace_back(node->id, node->getToggle().get());
                    if (node->hasCheckbox())    w.checkboxes.emplace_back(node->id, node->getCheckbox().get());
                    if (node->hasInputField())  w.inputFields.emplace_back(node->id, node->getInputField().get());
                    if (node->hasDropdown())    w.dropdowns.emplace_back(node->id, node->getDropdown().get());
                    if (node->hasButton())      w.buttons.emplace_back(node->id, node->getButton().get());
                    if (node->hasProgressBar()) w.bars.emplace_back(node->id, node->getProgressBar().get());
                    if (node->hasText())        w.texts.emplace_back(node->id, node->getText().get());
                    if (node->hasLayout())      w.layouts.emplace_back(node->id, node->getLayout().get());
                    w.parents.emplace_back(node->id, uiAncestor);
                }

                const uint64_t paraLosHijos = (tieneUi && canvasIdx >= 0) ? node->id : uiAncestor;
                for (const auto& child : node->children)
                    visit(child.get(), canvasIdx, paraLosHijos);
            }
        };

        Walker walker{out};
        for (const auto& child : m_root.children) walker.visit(child.get(), -1, 0ull);
    }
```

**Cuidado con la invalidación:** `out.push_back` puede reasignar el vector, así que **no** guardar referencias a `out[i]` a través de llamadas recursivas. El código de arriba re-indexa (`out[(size_t)canvasIdx]`) en cada uso, que es justo lo que lo evita. **No refactorizar a una referencia local que sobreviva al `visit` de los hijos.**

**Cuidado con `worldTransform`:** lo rellena `Scene::updateWorldTransforms`. `collectCanvases` lo LEE, no lo calcula: quien llame tiene que haber actualizado los transforms ese frame (los tres bucles ya lo hacen).

- [ ] **Step 5: Borrar `collectUiWidgets` y arreglar sus 6 llamantes**

Quitar la declaración de `Scene.h` y la definición de `Scene.cpp`. Los 6 puntos de llamada están todos en `camera_tests.cpp` con la forma:

```cpp
    UiWidgetLists lists;
    scene.collectUiWidgets(lists);
    botones = lists.buttons; textos = lists.texts; barras = lists.bars;
    layouts = lists.layouts; jerarquia = lists.parents;
```

que pasa a:

```cpp
    std::vector<UiCanvasBinding> bindings;
    scene.collectCanvases(bindings);
    const UiWidgetLists& lists = bindings.empty() ? UiWidgetLists{} : bindings[0].widgets;
    botones = lists.buttons; textos = lists.texts; barras = lists.bars;
    layouts = lists.layouts; jerarquia = lists.parents;
```

**Ojo:** el ternario con un temporal no se puede ligar a una referencia const de forma segura aquí. Usar en su lugar:

```cpp
    static const UiWidgetLists kVacio;
    const UiWidgetLists& lists = bindings.empty() ? kVacio : bindings[0].widgets;
```

Algunos de esos tests tienen widgets **sin canvas** (p. ej. `test_collect_ui_widgets_salta_los_intermedios_sin_ui` cuelga del Canvas, pero otros no). Revisar uno a uno: si un test no tenía Canvas, **añadírselo**, porque ahora es requisito. Es el cambio de comportamiento del Step 1.

- [ ] **Step 6: Verde**

Run: `.\build.bat` y `.\build-ninja\engine\tests\dt_camera_tests.exe`
Expected: `ALL CAMERA TESTS PASSED`

- [ ] **Step 7: Sabotaje — meter todo en el primer canvas**

Cambiar `UiWidgetLists& w = out[(size_t)canvasIdx].widgets;` por `out[0].widgets`. Recompilar, correr.
Expected: falla `test_collect_canvases_agrupa_por_canvas`. Revertir.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "feat(ui): la escena agrupa los widgets por su canvas"
```

---

### Task 5: El Renderer pasa a N canvas (todos de pantalla)

Al acabar esta tarea todo se ve **exactamente igual que hoy**, pero por dentro son N canvas. Es el punto de control antes de tocar el render de verdad.

**Files:**
- Modify: `engine/include/DonTopo/Renderer/EditorRenderer.h`
- Modify: `engine/include/DonTopo/Renderer/Renderer.h`, `engine/src/Renderer/Renderer.cpp`
- Modify: `engine/include/DonTopo/Renderer/D3D12/D3D12Renderer.h`, `engine/src/Renderer/D3D12/D3D12Renderer.cpp`
- Modify: `runtime/main.cpp`, `sandbox/src/main.cpp` (los 3 bucles)
- Test: `engine/tests/camera_tests.cpp`

**Interfaces:**
- Consumes: `UiCanvasBinding` (Task 4).
- Produces:
```cpp
// EditorRenderer.h
virtual void syncUiCanvases(const std::vector<UiCanvasBinding>& bindings) = 0;
virtual const UiElement* findUiNode(const std::string& name) const = 0;
// uiCanvas() NO cambia de firma: devuelve el canvas de PANTALLA.
```

- [ ] **Step 1: Escribir el test de supervivencia de slots (RED)**

Este test no necesita Renderer: prueba la función libre que hace el emparejamiento, que se extrae a propósito para poder probarla sin GPU.

En `camera_tests.cpp`:

Los slots van en un `std::vector<std::unique_ptr<UiCanvasSlot>>` y **no** en un
vector por valor: el test compara PUNTEROS al árbol para probar que el slot
sobrevive, y con un vector por valor cualquier reasignación los movería de
dirección y el test mediría otra cosa.

```cpp
// Los slots se emparejan por ownerId, NO por indice. Reordenar los canvas en la
// jerarquia (o borrar uno de en medio) no puede resetear la cache del que no se
// ha movido: si lo hiciera, mover un enemigo reconstruiria su barra de vida
// entera y se veria como un parpadeo.
static void test_ui_slots_se_emparejan_por_owner_id()
{
    std::vector<std::unique_ptr<UiCanvasSlot>> slots;
    CanvasComponent c;

    std::vector<UiCanvasBinding> b(2);
    b[0].ownerId = 7ull;  b[0].canvas = &c;
    b[1].ownerId = 9ull;  b[1].canvas = &c;
    matchUiCanvasSlots(b, slots);
    CHECK(slots.size() == 2);
    if (slots.size() != 2) return;
    const UiCanvas* arbol7 = &slots[0]->canvas;
    const UiCanvas* arbol9 = &slots[1]->canvas;
    CHECK(slots[0]->ownerId == 7ull);
    CHECK(slots[1]->ownerId == 9ull);

    // Se INVIERTE el orden: cada slot tiene que seguir a SU dueno, con su arbol.
    std::vector<UiCanvasBinding> b2(2);
    b2[0].ownerId = 9ull; b2[0].canvas = &c;
    b2[1].ownerId = 7ull; b2[1].canvas = &c;
    matchUiCanvasSlots(b2, slots);
    CHECK(slots.size() == 2);
    if (slots.size() != 2) return;
    CHECK(slots[0]->ownerId == 9ull);
    CHECK(slots[1]->ownerId == 7ull);
    CHECK(&slots[0]->canvas == arbol9);
    CHECK(&slots[1]->canvas == arbol7);

    // Y uno que desaparece se lleva su slot; uno nuevo estrena el suyo.
    std::vector<UiCanvasBinding> b3(1);
    b3[0].ownerId = 42ull; b3[0].canvas = &c;
    matchUiCanvasSlots(b3, slots);
    CHECK(slots.size() == 1);
    if (slots.empty()) return;
    CHECK(slots[0]->ownerId == 42ull);
}

// Y el orden de pintado de los canvas de MUNDO: de lejos a cerca. Van con alpha,
// asi que pintarlos al reves mezcla mal y se ve como un halo. Contra la
// geometria manda el depth; entre ellos, manda esto.
static void test_world_canvases_se_ordenan_de_lejos_a_cerca()
{
    std::vector<std::unique_ptr<UiCanvasSlot>> slots;
    for (float z : { -2.0f, -10.0f, -6.0f })
    {
        auto s = std::make_unique<UiCanvasSlot>();
        s->mode  = UiCanvasRenderMode::World;
        s->model = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, z));
        slots.push_back(std::move(s));
    }

    // Camara en el origen mirando a -Z: el de z = -10 es el mas lejano.
    const glm::mat4 vista(1.0f);
    std::vector<UiCanvasSlot*> orden;
    sortWorldCanvasesBackToFront(slots, vista, orden);

    CHECK(orden.size() == 3);
    if (orden.size() != 3) return;
    CHECK(nearlyEqual(orden[0]->model[3].z, -10.0f));
    CHECK(nearlyEqual(orden[1]->model[3].z, -6.0f));
    CHECK(nearlyEqual(orden[2]->model[3].z, -2.0f));
}
```

Registrar las dos en `main()`.

- [ ] **Step 2: Confirmar RED**

Run: `.\build.bat`
Expected: FAIL — `UiCanvasSlot: identificador no declarado`.

- [ ] **Step 3: `UiCanvasSlot` y `matchUiCanvasSlots` en `UiWidgetSync.h`**

```cpp
    // Un canvas VIVO del Renderer: su árbol, su caché de sync y lo que hay que
    // saber para dibujarlo. Uno por CanvasComponent de la escena.
    struct UiCanvasSlot
    {
        uint64_t           ownerId = 0;
        UiCanvas           canvas;
        UiWidgetSyncCache  cache;
        UiDrawData         drawData;
        UiCanvasRenderMode mode = UiCanvasRenderMode::ScreenSpace;
        glm::mat4          model{1.0f};
        bool               depthTest = true;
        // Distancia al ojo, para ordenar los de mundo de lejos a cerca.
        float              viewDepth = 0.0f;
    };

    // Reordena `slots` para que casen uno a uno con `bindings`, emparejando por
    // ownerId. Los slots que sobreviven CONSERVAN su árbol y su caché: sin esto,
    // reordenar los canvas en la jerarquía reconstruiría árboles que no han
    // cambiado, y eso se ve como un parpadeo.
    inline void matchUiCanvasSlots(const std::vector<UiCanvasBinding>& bindings,
                                   std::vector<std::unique_ptr<UiCanvasSlot>>& slots)
    {
        std::vector<std::unique_ptr<UiCanvasSlot>> nuevos;
        nuevos.reserve(bindings.size());

        for (const UiCanvasBinding& b : bindings)
        {
            auto it = std::find_if(slots.begin(), slots.end(),
                [&](const std::unique_ptr<UiCanvasSlot>& s) {
                    return s && s->ownerId == b.ownerId;
                });

            if (it != slots.end())
            {
                nuevos.push_back(std::move(*it));   // se lleva árbol y caché
            }
            else
            {
                auto s = std::make_unique<UiCanvasSlot>();
                s->ownerId = b.ownerId;
                nuevos.push_back(std::move(s));
            }
        }
        // Lo que quede en `slots` es de canvas que ya no están: se destruye al
        // salir del scope, y con ello su árbol y su caché.
        slots = std::move(nuevos);
    }
```

Y el orden de pintado, extraído a función libre **para poder probarlo sin GPU**
(dentro del Renderer no habría forma):

```cpp
    // Los canvas de MUNDO en orden de pintado: de lejos a cerca. Van con alpha,
    // así que pintarlos al revés mezcla mal. Contra la geometría manda el depth
    // buffer; entre ellos, manda esto.
    //
    // Los de PANTALLA no entran: esos van en su propio pase, sin profundidad y en
    // el orden del árbol.
    inline void sortWorldCanvasesBackToFront(
        const std::vector<std::unique_ptr<UiCanvasSlot>>& slots,
        const glm::mat4& view, std::vector<UiCanvasSlot*>& out)
    {
        out.clear();
        for (const auto& s : slots)
        {
            if (!s || s->mode != UiCanvasRenderMode::World) continue;
            const glm::vec3 pos = glm::vec3(s->model[3]);
            // +z hacia delante: la vista deja el ojo mirando a -Z, así que se
            // niega para que "más grande" signifique "más lejos".
            s->viewDepth = -(view * glm::vec4(pos, 1.0f)).z;
            out.push_back(s.get());
        }
        std::sort(out.begin(), out.end(),
                  [](const UiCanvasSlot* a, const UiCanvasSlot* b) {
                      return a->viewDepth > b->viewDepth;   // lejos primero
                  });
    }
```

Necesita `#include <algorithm>` y `<memory>`.

- [ ] **Step 4: Verde del test**

Run: `.\build.bat` y `dt_camera_tests.exe`
Expected: `ALL CAMERA TESTS PASSED`

- [ ] **Step 5: Las dos virtuales en `EditorRenderer.h`**

```cpp
            // Monta el árbol vivo de CADA canvas de la escena. Sustituye al
            // collect + syncUiWidgets que antes repetían los tres bucles (runtime
            // y sandbox x2) — tres copias de lo mismo es como se desincronizan.
            virtual void syncUiCanvases(const std::vector<UiCanvasBinding>& bindings) = 0;

            // Busca un nodo por nombre en TODOS los canvas, no solo en el de
            // pantalla. Lo usan los nueve gizmos de widget del editor: sin esto,
            // un botón dentro de un canvas de mundo se quedaría sin gizmo y nada
            // lo diría.
            virtual const UiElement* findUiNode(const std::string& name) const = 0;
```

- [ ] **Step 6: Implementarlas en `Renderer`**

En `Renderer.h`, sustituir `UiCanvas m_uiCanvas;` por:

```cpp
            std::vector<std::unique_ptr<UiCanvasSlot>> m_uiSlots;
            // Repliegue de uiCanvas() cuando la escena no tiene ningún canvas de
            // pantalla. Persistente y vacío: devolver una referencia a un temporal
            // dejaría a los gizmos leyendo memoria muerta.
            UiCanvas m_uiCanvasFallback;
```

En `Renderer.cpp`:

```cpp
    void Renderer::syncUiCanvases(const std::vector<UiCanvasBinding>& bindings)
    {
        matchUiCanvasSlots(bindings, m_uiSlots);

        for (size_t i = 0; i < bindings.size(); i++)
        {
            UiCanvasSlot& s = *m_uiSlots[i];
            const UiCanvasBinding& b = bindings[i];
            if (b.canvas) b.canvas->applyTo(s.canvas);
            s.mode      = b.canvas ? b.canvas->renderMode : UiCanvasRenderMode::ScreenSpace;
            s.depthTest = b.canvas ? b.canvas->depthTest : true;
            syncUiWidgets(b.widgets, s.canvas, s.cache, *this);
        }
    }

    UiCanvas& Renderer::uiCanvas()
    {
        for (auto& s : m_uiSlots)
            if (s && s->mode == UiCanvasRenderMode::ScreenSpace) return s->canvas;
        return m_uiCanvasFallback;
    }

    const UiElement* Renderer::findUiNode(const std::string& name) const
    {
        for (const auto& s : m_uiSlots)
        {
            if (!s) continue;
            if (const UiElement* n = findUiNodeIn(s->canvas.root(), name)) return n;
        }
        return nullptr;
    }
```

`findUiNodeIn` es el recorrido recursivo por nombre; hoy vive como `findUiNodeNamed` en `ViewportPanel.cpp`. **Moverlo** a `UiCanvas.h` como función libre `findUiNodeIn(const UiElement&, const std::string&)` para que lo usen los dos.

- [ ] **Step 7: Dibujar los N canvas de pantalla**

Donde hoy hay:

```cpp
            const VkExtent2D uiExtent = effectiveViewport();
            m_uiCanvas.buildDrawData(uiExtent.width, uiExtent.height, m_uiDrawData);
            if (!m_uiDrawData.empty() && ...)
```

pasa a construir el draw data de **cada slot de pantalla** y abrir el pase si **alguno** tiene algo, grabando uno por uno dentro del mismo `vkCmdBeginRenderPass`.

- [ ] **Step 8: Lo mismo en `D3D12Renderer`**

Mismo cambio: `Impl::uiCanvas` pasa a `std::vector<std::unique_ptr<UiCanvasSlot>>`, `recordUiCanvas` itera los de pantalla, y se implementan las dos virtuales.

- [ ] **Step 9: Los tres bucles**

En `runtime/main.cpp` y en los **dos** de `sandbox/src/main.cpp`, sustituir el bloque `collectUiWidgets` + `syncUiWidgets` por:

```cpp
            uiBindings.clear();
            scene.collectCanvases(uiBindings);
            renderer.syncUiCanvases(uiBindings);
```

con `std::vector<DonTopo::UiCanvasBinding> uiBindings;` declarado fuera del bucle (para no reasignar cada frame). `renderer.uiCanvas().updateInput(uiInput)` se queda igual.

**Ojo:** el gate `if (scene.findCanvas())` deja de hacer falta — `collectCanvases` devuelve una lista vacía si no hay ninguno. Quitarlo.

- [ ] **Step 10: Verde y verificación en GUI**

Run: los 16 ejecutables → `FALLOS: 0`.
Verificar en el editor que la UI de la escena se ve **exactamente igual que antes**. Es el objetivo de esta tarea: cero cambio visible.

- [ ] **Step 11: Commit**

```bash
git add -A
git commit -m "feat(ui): el Renderer pasa de un canvas a N, emparejados por owner"
```

---

### Task 6: `UiSpriteBatch::record` recibe la matriz

Refactor sin cambio de comportamiento: los de pantalla siguen pasando la misma ortográfica que se calculaba dentro.

**Files:**
- Modify: `engine/include/DonTopo/UI/UiSpriteBatch.h`, `engine/src/UI/UiSpriteBatch.cpp`
- Modify: `engine/src/Renderer/Renderer.cpp`

**Interfaces:**
- Produces:
```cpp
void UiSpriteBatch::record(GpuDevice& gpu, VkCommandBuffer cmd, const UiDrawData& data,
                           const glm::mat4& transform,
                           VkExtent2D canvasExtent, VkExtent2D fbExtent, int frame);
```

- [ ] **Step 1: Cambiar la firma**

Quitar de `record` el cálculo de `proj` (hoy en `UiSpriteBatch.cpp:2084`) y recibirla. El `vkCmdPushConstants` pasa a empujar el parámetro.

- [ ] **Step 2: El llamante de pantalla calcula la ortográfica**

En `Renderer.cpp`, antes de llamar:

```cpp
                // La misma de siempre: píxeles con el origen ARRIBA a la
                // izquierda. RH_ZO y no ortho a secas: Vulkan clipea z fuera de
                // [0,1] y glm::ortho da [-1,1], que se comería la mitad cercana.
                const glm::mat4 proj = glm::orthoRH_ZO(0.0f, (float)uiExtent.width,
                                                       (float)uiExtent.height, 0.0f,
                                                       -1.0f, 1.0f);
```

- [ ] **Step 3: Verde**

Run: `.\build.bat`, los 16 ejecutables.
Expected: `FALLOS: 0`. Y en GUI, la UI **idéntica**.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "refactor(ui): record recibe la matriz en vez de calcularla"
```

---

### Task 7: Vulkan — pipelines de mundo y grabado en el pase de escena

Es el trozo con más riesgo. Al acabar, un canvas en modo World se ve en la escena y una pared lo tapa.

**Files:**
- Modify: `engine/include/DonTopo/UI/UiSpriteBatch.h`, `engine/src/UI/UiSpriteBatch.cpp`
- Modify: `engine/src/Renderer/Renderer.cpp`
- Modify: el fragment shader de UI

**Interfaces:**
- Produces:
```cpp
// Compila las dos variantes de mundo contra el renderpass de la ESCENA.
void UiSpriteBatch::initWorldPipelines(GpuDevice& gpu, VkRenderPass scenePass,
                                       VkSampleCountFlagBits samples);
// Igual que record, pero con la variante de mundo (depth on/off).
void UiSpriteBatch::recordWorld(GpuDevice& gpu, VkCommandBuffer cmd, const UiDrawData& data,
                                const glm::mat4& transform, bool depthTest,
                                VkExtent2D canvasExtent, VkExtent2D fbExtent, int frame);
```

- [ ] **Step 1: sRGB → lineal en el shader**

El pase de escena es **HDR lineal** (`kHdrFormat`), no `B8G8R8A8_SRGB`. El mismo color escrito ahí sale **lavado**, y ninguna capa de validación lo dice.

Añadir al push constant un flag (hay sitio: hoy solo va la mat4; ampliar el rango) y en el fragment shader:

```glsl
// El pase de UI escribe en un target sRGB y el hardware convierte solo. El de
// escena es HDR LINEAL, asi que aqui hay que convertir a mano o el color sale
// lavado.
if (pc.linearOutput != 0)
    color.rgb = pow(color.rgb, vec3(2.2));
```

Regenerar los `.spv` (los genera el build desde los `.comp`/`.frag`; **no están trackeados**). Un desajuste de layout del push constant entre CPU y GPU **no da error en ningún sitio**: verificar con `spirv-dis` que el bloque de push constants tiene el tamaño esperado.

- [ ] **Step 2: `initWorldPipelines`**

Clonar la creación del pipeline de pantalla cambiando: `renderPass` = el de la escena, `samples` = `m_aaSampleCount`, y el `VkPipelineDepthStencilStateCreateInfo`:

```cpp
        // depthWrite SIEMPRE off: la UI va con alpha, y escribir profundidad haria
        // que los quads de un mismo canvas se recortaran entre si segun el orden
        // en que salieran del batcher.
        ds.depthTestEnable  = conDepth ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = VK_FALSE;
        ds.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
```

Dos variantes: `m_worldPipelineDepth` y `m_worldPipelineNoDepth`.

- [ ] **Step 3: Recrearlas cuando se recrea el pase de escena**

Buscar dónde `Renderer` llama a `createOffscreenRenderPass()` en la recreación (cambio de AA / resize) y añadir ahí la llamada a `initWorldPipelines`. **Si se olvida, el pipeline queda compilado contra un renderpass destruido**: se manifiesta como device lost, no como error de validación.

- [ ] **Step 4: Grabar los canvas de mundo**

Al final del pase de escena —**después** de la geometría y del skybox, **antes** de `vkCmdEndRenderPass`—:

```cpp
        // El orden lo pone la funcion libre de UiWidgetSync.h, que es la que
        // esta probada sin GPU (test_world_canvases_se_ordenan_de_lejos_a_cerca).
        std::vector<UiCanvasSlot*> mundo;
        sortWorldCanvasesBackToFront(m_uiSlots, view, mundo);

        for (UiCanvasSlot* s : mundo)
        {
            const glm::vec2 tam = s->canvas.referenceResolution;
            s->canvas.buildDrawData((uint32_t)tam.x, (uint32_t)tam.y, s->drawData);
            if (s->drawData.empty()) continue;
            const glm::mat4 mvp = proj * view * s->model;
            m_uiBatch.recordWorld(m_gpu, cmd, s->drawData, mvp, s->depthTest,
                                  VkExtent2D{(uint32_t)tam.x, (uint32_t)tam.y},
                                  sceneExtent, m_currentFrame);
        }
```

`s->model` lo calcula `syncUiCanvases` con `uiWorldCanvasMatrix(*b.canvas, tam, b.worldTransform, view)` — **necesita la vista**, así que hay que pasarle la matriz de vista del frame a `syncUiCanvases`, o recalcular el modelo aquí. **Decisión: recalcularlo aquí**, en el grabado, que es donde la vista está disponible sin cambiar la firma de la virtual.

- [ ] **Step 5: Cuidado con el scissor**

El scissor del batcher está en píxeles de canvas y hoy se mapea 1:1 al framebuffer. En modo mundo **eso no vale**: el canvas está proyectado. Los canvas de mundo tienen que grabarse con el scissor a **todo el framebuffer**, y `clipChildren` deja de recortar. Documentarlo como limitación conocida en el spec y en el README: **el recorte de UI no funciona en canvas de mundo**.

- [ ] **Step 6: Verificación en GUI**

Crear una escena: un cubo grande, y detrás un GameObject con Canvas en modo World con un Panel de color y un Text. Comprobar:
1. Se ve en el mundo, con perspectiva (se hace pequeño al alejarse).
2. El cubo lo tapa al ponerse delante.
3. Con `depthTest = false` se ve a través del cubo.
4. Con `billboard = YawOnly` gira al mover la cámara y **no se tumba** al mirar desde arriba.
5. Los colores **no están lavados** (la conversión del Step 1).

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat(ui): los canvas de mundo se dibujan en el pase de escena, con profundidad"
```

---

### Task 8: D3D12 — paridad

**Files:**
- Modify: `engine/include/DonTopo/Renderer/D3D12/D3D12Renderer.h`, `engine/src/Renderer/D3D12/D3D12Renderer.cpp`

- [ ] **Step 1: Las dos variantes de PSO**

Clonar `uiPipeline` con `DSVFormat` = el de la escena, `SampleDesc` = el del pase de escena, y `DepthStencilState` con `DepthEnable` según la variante y `DepthWriteMask = ZERO`.

**Trampa conocida del backend:** una vista con más `MipLevels` que el recurso no falla al crearse y el device se cae después. Y una textura sin escribir vale 0 y anula lo que multiplique. Si algo sale negro, subir un factor global y ver si NO cambia nada delata la multiplicación por cero.

- [ ] **Step 2: Grabar en el pase de escena**

En `D3D12Renderer::Impl`, localizar dónde acaba la geometría de la escena (antes del post) y grabar ahí los canvas de mundo, con el mismo orden lejos→cerca.

**Trampa conocida:** la GPU lee el buffer al **ejecutar**, no al grabar. Si los N canvas comparten un buffer de constantes por frame y se reescribe entre pases, **todos** acaban con el último valor. Cada canvas necesita su propia región.

- [ ] **Step 3: Verificación en GUI con el backend de D3D12**

El backend sale del `project.json` del **último proyecto abierto**: hay que abrir a propósito un proyecto configurado en DirectX 12. La primera línea de la traza dice cuál se levantó — sin comprobarla, lanzar el Sandbox no verifica NADA de D3D12.

Repetir los 5 puntos de la verificación de la Task 7.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "feat(ui): canvas de mundo en el backend de DirectX 12"
```

---

### Task 9: Editor — gizmos

**Files:**
- Modify: `engine/src/Editor/ViewportPanel.cpp`

- [ ] **Step 1: Los nueve gizmos de widget pasan por `findUiNode`**

Sustituir en los nueve:

```cpp
    drawUiNodeGizmo(ctx, findUiNodeNamed(ctx.renderer->uiCanvas(), uiButtonNodeName(ctx.selected->id)),
                    imagePos, imageSize);
```
por
```cpp
    drawUiNodeGizmo(ctx, ctx.renderer->findUiNode(uiButtonNodeName(ctx.selected->id)),
                    imagePos, imageSize);
```

Sin esto, un widget dentro de un canvas de mundo se queda sin gizmo **en silencio**.

- [ ] **Step 2: El gizmo del Canvas en modo World**

`drawCanvasGizmo` hoy dibuja el rect del área útil en 2D. En modo World eso no significa nada: proyectar las **cuatro esquinas** del canvas por `proj · view · model`, dividir por w, pasar a píxeles de la imagen y dibujar el cuadrilátero. Un vértice con `w <= 0` está detrás de la cámara: en ese caso **no dibujar nada** en vez de pintar una figura del revés.

- [ ] **Step 3: Verificación en GUI**

Seleccionar un canvas de mundo y comprobar que el cuadrilátero **sigue al cartel** y enseña su inclinación. Seleccionar un botón dentro de él y comprobar que su gizmo aparece.

- [ ] **Step 4: Documentar las dos limitaciones conocidas**

En `Scripts/README.md`, en la sección del Canvas:
- Un canvas de mundo **no se puede clicar** (ni en el juego ni en el viewport): se selecciona desde el Hierarchy, como el contenedor de Layout.
- `clipChildren` **no recorta** en un canvas de mundo (Task 7, Step 5).

- [ ] **Step 5: Commit final**

```bash
git add -A
git commit -m "feat(ui): gizmos de los canvas de mundo y de sus widgets"
```

---

## Verificación final

- [ ] Los 16 ejecutables en verde.
- [ ] Los cuatro sabotajes del spec corridos y cazados: ortográfica en `uiWorldCanvasMatrix` (Task 3), slots por índice (Task 5), todo al primer canvas (Task 4), sin orden por distancia (Task 7).
- [ ] GUI con Vulkan: los 5 puntos de la Task 7.
- [ ] GUI con DirectX 12: los mismos 5, con un proyecto configurado en D3D12 y la primera línea de la traza comprobada.
- [ ] Actualizar la memoria: `pending_ui_audit_items` punto 3 pasa a cerrado.
