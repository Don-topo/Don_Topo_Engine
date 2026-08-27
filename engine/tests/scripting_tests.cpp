// Tests headless de ScriptBindings: el guard ensureFinite (NaN/Inf desde Lua,
// ver ScriptBindings.cpp) y su contrato de "silencio + aviso". Plain main +
// asserts, sin framework — mismo patrón que camera_tests.cpp/audio_tests.cpp/
// physics_tests.cpp.
//
// Antes de este fichero, ensureFinite (131 líneas cambiadas en
// ScriptBindings.cpp) no lo ejercitaba ningún test: podía romperse en
// silencio sin que ningún exe rojo lo delatara.
//
// PhysX solo admite UNA PxFoundation por proceso: se comparte un único
// PhysicsManager (y AudioManager) entre todos los tests, creados en main() y
// pasados por referencia — nunca uno por test (ver physics_tests.cpp).
//
// ScriptManager se ejercita headless de verdad: init() con una carpeta que no
// existe registra igualmente los bindings de Lua (solo loguea "carpeta no
// encontrada" y sigue, ver ScriptManager::init) — no hace falta ningún .lua
// en disco para llamar a Transform/Collider/Rigidbody directamente. Cada test
// empuja una LuaEntity ya resuelta a un global Lua ("e") y ejecuta Lua REAL
// contra ella (mismo mecanismo que instantiateComponentWith usa para inyectar
// self.entity, solo que aquí sin ScriptComponent de por medio) y captura el
// Log en un std::vector<std::string> vía setLogCallback.
#include "DonTopo/Scripting/ScriptManager.h"
#include "DonTopo/Scripting/ScriptBindings.h"
#include "DonTopo/Scripting/LuaSyntaxCheck.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Editor/ProjectContext.h"
#include "DonTopo/UI/CanvasComponent.h"
#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/TextComponent.h"
#include "DonTopo/UI/ImageComponent.h"
#include "DonTopo/UI/LayoutComponent.h"
#include "DonTopo/UI/PanelComponent.h"
#include "DonTopo/UI/SliderComponent.h"
#include "DonTopo/UI/CheckboxComponent.h"
#include "DonTopo/UI/ToggleComponent.h"
#include "DonTopo/UI/ScrollbarComponent.h"
#include "DonTopo/UI/InputFieldComponent.h"
#include "DonTopo/UI/DropdownComponent.h"
#include "DonTopo/UI/ScrollViewComponent.h"
#include "DonTopo/UI/ProgressBarComponent.h"
#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiSpriteBatch.h"
#include <TextEditor.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }

// true si alguna línea logueada contiene needle (p.ej. el nombre del método o
// "WARN").
static bool logContains(const std::vector<std::string>& log, const std::string& needle)
{
    for (const auto& l : log)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

// SetPosition con un Vec3 que trae un componente NaN (0/0 en Lua): la
// posición NO cambia (se conserva la de antes) y el Log recibe un aviso que
// nombra el método. Ejercita el guard tal cual lo ve un script real, que es
// justo lo que el review señaló sin cobertura.
static void test_set_position_rejects_nan(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Objetivo");
    go->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 6.0f, 7.0f));
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script("e:GetTransform():SetPosition(Vec3.new(0/0, 1, 2))");

    glm::vec3 pos(go->localTransform[3]);
    CHECK(nearlyEqual(pos.x, 5.0f));
    CHECK(nearlyEqual(pos.y, 6.0f));
    CHECK(nearlyEqual(pos.z, 7.0f));
    CHECK(logContains(log, "SetPosition"));
    CHECK(logContains(log, "WARN"));
}

// CASO DE CONTROL: el MISMO setter con un Vec3 finito de verdad SÍ cambia la
// posición y NO deja ningún aviso en el Log. Sin este test, un ensureFinite
// que devolviera siempre false pasaría igual el test de arriba (posición sin
// cambiar "porque nunca aplica nada" en vez de "porque el valor era NaN").
static void test_set_position_applies_finite_value(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Objetivo");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script("e:GetTransform():SetPosition(Vec3.new(11, 22, 33))");

    glm::vec3 pos(go->localTransform[3]);
    CHECK(nearlyEqual(pos.x, 11.0f));
    CHECK(nearlyEqual(pos.y, 22.0f));
    CHECK(nearlyEqual(pos.z, 33.0f));
    CHECK(log.empty());
}

// Caso de un FLOAT SUELTO (no un Vec3): SphereCollider.SetRadius con NaN.
// Mismo contrato que SetPosition: el radio no cambia y el Log avisa.
static void test_set_radius_rejects_nan(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Bola");
    auto col = pm.createSphereColliderComponent(10.0f, glm::vec3(0.0f), go->worldTransform, /*dynamic=*/false);
    go->setSphereCollider(col);
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script("e:GetComponent('SphereCollider'):SetRadius(0/0)");

    CHECK(nearlyEqual(go->getSphereCollider()->getRadius(), 10.0f));
    CHECK(logContains(log, "SetRadius"));
    CHECK(logContains(log, "WARN"));
}

// Control: el mismo SetRadius con un valor finito SÍ cambia el radio y NO
// loguea nada.
static void test_set_radius_applies_finite_value(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Bola");
    auto col = pm.createSphereColliderComponent(10.0f, glm::vec3(0.0f), go->worldTransform, /*dynamic=*/false);
    go->setSphereCollider(col);
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script("e:GetComponent('SphereCollider'):SetRadius(42)");

    CHECK(nearlyEqual(go->getSphereCollider()->getRadius(), 42.0f));
    CHECK(log.empty());
}

// AddForce recibe x,y,z SUELTOS (no un Vec3 ya construido): con un NaN entre
// ellos, la fuerza NO se aplica (la velocidad se queda a 0 tras avanzar la
// física) y el Log recibe un aviso.
static void test_add_force_rejects_nan(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Cuerpo");
    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);
    // El actor PhysX vive en el collider (Rigidbody no lo posee, ver
    // Rigidbody.h) — col tiene que seguir vivo mientras se usa rb, igual que
    // en physics_tests.cpp.
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    go->setRigidbody(rb);
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script("e:GetComponent('Rigidbody'):AddForce(0/0, 100, 0)");
    for (int i = 0; i < 10; ++i) pm.stepSimulation(1.0f / 60.0f);

    CHECK(nearlyEqual(rb->getVelocity().x, 0.0f));
    CHECK(nearlyEqual(rb->getVelocity().y, 0.0f));
    CHECK(logContains(log, "AddForce"));
    CHECK(logContains(log, "WARN"));
}

// Control: el mismo AddForce con x,y,z finitos SÍ mueve el cuerpo (velocidad
// no nula tras avanzar la física) y NO loguea nada. Sin este test, un
// ensureFinite que devolviera siempre false pasaría igual el test de arriba.
static void test_add_force_applies_finite_value(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Cuerpo");
    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    go->setRigidbody(rb);
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script("e:GetComponent('Rigidbody'):AddForce(1000, 0, 0)");
    for (int i = 0; i < 10; ++i) pm.stepSimulation(1.0f / 60.0f);

    CHECK(rb->getVelocity().x > 0.1f);
    CHECK(log.empty());
}

// El ORDEN importa, y este test es lo único que lo protege: ensureFinite
// tiene que correr DESPUÉS del deref, no antes.
//
// Con el guard delante, un script que toca una entity ya destruida pasándole
// además un NaN recibía un aviso de "valor no finito" y un return silencioso
// — enmascarando el use-after-destroy, que es el fallo grave de los dos. Con
// el orden correcto, deref lanza error de Lua y del NaN no se llega a hablar.
//
// Sin este test, revertir ese reordenamiento entero deja los 7 ejecutables en
// verde: el caso feliz no lo distingue, porque cuando la entity está viva los
// dos órdenes se comportan igual.
static void test_dead_entity_wins_over_nan(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Condenado");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    // El Transform se obtiene MIENTRAS la entity vive y se guarda. Si se
    // pidiera después del borrado, el deref que hace el propio GetTransform
    // lanzaría antes de llegar a SetPosition y este test pasaría sin ejercitar
    // nada — comprobado: así no distinguía el orden correcto del invertido.
    sm.lua().script("t = e:GetTransform()");

    // Ahora sí: la entity de Lua sostiene un GameObject que ya no está en la
    // escena, y el Transform guardado apunta a ella.
    scene.removeGameObject(go);
    sm.rebuildAliveSet();

    // pcall: se espera que LANCE. Con script() a secas el error se propagaría
    // y abortaría el test en vez de comprobarlo.
    sm.lua().script(
        "ok, err = pcall(function() t:SetPosition(Vec3.new(0/0, 1, 2)) end)");

    const bool ok = sm.lua()["ok"];
    CHECK(!ok);                              // tiene que fallar, no avisar

    // Y el motivo debe ser la entity muerta, no el NaN. El early-return es
    // necesario: si la llamada NO lanzó, "err" es nil y leerlo como string
    // hace panic a sol2, que abortaría el proceso en vez de dejar un FAIL
    // legible.
    if (!ok)
    {
        const std::string err = sm.lua()["err"];
        CHECK(err.find("destruida") != std::string::npos);
        CHECK(err.find("SetPosition") == std::string::npos);
    }
    CHECK(!logContains(log, "SetPosition"));
}

// checkLuaSyntax alimenta los markers de error del Script Editor y no tenía
// ninguna cobertura. Lo que importa es que devuelva la LÍNEA correcta: el
// marker se pinta por número de línea, así que un off-by-one o un fallo del
// regex que parsea el mensaje de Lua deja el aviso en el sitio equivocado —
// o, si no detecta nada, sin marker ninguno.
static void test_lua_syntax_check_detects_error()
{
    // Script válido: no hay error.
    CHECK(!checkLuaSyntax("local x = 1\nprint(x)\n").has_value());

    // 'end' que falta: el caso MÁS COMÚN, y el que destapó que los markers no
    // se veían. Lua lo reporta en <eof>, o sea UNA LÍNEA MÁS ALLÁ del final —
    // con 2 líneas de texto, dice línea 3. El editor solo pinta markers de
    // líneas que existen, así que ScriptEditorPanel::saveTab tiene que acotar
    // la línea al documento antes de pasarla; si alguien quita ese clamp, el
    // marker vuelve a guardarse sin dibujarse nunca.
    auto err = checkLuaSyntax("function f()\n  local y = 2\n");
    CHECK(err.has_value());
    if (!err) return;
    CHECK(err->first == 3);          // fuera del texto: 2 líneas, error en la 3
    CHECK(!err->second.empty());

    // Error en una línea concreta del medio: la línea reportada tiene que ser
    // ESA, no la primera ni la última.
    auto mid = checkLuaSyntax("local a = 1\nlocal b = = 2\nlocal c = 3\n");
    CHECK(mid.has_value());
    if (!mid) return;
    CHECK(mid->first == 2);
}

// El editor real: SetText -> GetText -> checkLuaSyntax, que es exactamente lo
// que hace saveTab. Fija las dos trampas que impedían ver el marker, medidas
// con el TextEditor de verdad y no supuestas:
//
//  a) GetText() devuelve UN CARÁCTER MÁS del que se metió (el editor añade un
//     salto final), así que Lua ve una línea de más y sitúa el <eof> fuera del
//     documento. El editor solo pinta markers de líneas existentes.
//  b) Esa última línea, además, está VACÍA — acotar el marker ahí lo deja al
//     final del fichero, sin señalar nada útil.
static void test_syntax_error_line_is_out_of_document()
{
    // Script SIN el 'end' final, con salto final como cualquier fichero real.
    const std::string roto = "Rotator = {\n  speed = 45\n}\n\nfunction Rotator:Update(dt)\n  local t = 1\n";

    TextEditor ed;
    ed.SetText(roto);
    const std::string ida = ed.GetText();

    // (a) el round-trip por el editor añade el salto final
    CHECK(ida.size() == roto.size() + 1);

    auto err = checkLuaSyntax(ida);
    CHECK(err.has_value());
    if (!err) return;

    // El error cae MÁS ALLÁ de la última línea del editor: ese marker no se
    // pintaría nunca. Es el corazón del bug.
    CHECK(err->first > ed.GetTotalLines());

    // Y el mensaje nombra la línea donde se abrió lo que quedó sin cerrar
    // (aquí el 'function' de la línea 5), que es lo que markerLine usa para
    // poner la banda en un sitio con sentido.
    CHECK(err->second.find("to close") != std::string::npos);
    CHECK(err->second.find("at line 5") != std::string::npos);
}

// ── UI desde Lua ────────────────────────────────────────────────────────────
// Sin GPU: el loader falso devuelve nullptr, que es lo que devuelve el Renderer
// con una ruta vacía. Lo que se prueba es el DATO que llega al nodo, no la
// textura (mismo loader que camera_tests).
struct FakeUiLoader
{
    UiTextureAtlas* loadUiAtlas(const std::string&) { return nullptr; }
    UiFont*         loadUiFont(const std::string&)  { return nullptr; }
};

// Adaptador SOLO DE TESTS a la firma que syncUiWidgets tenía antes de que las
// listas se agruparan en UiWidgetLists. Mismo apaño que en camera_tests.cpp: la
// API de producción es UNA (la de la struct) y esto solo evita reescribir las
// llamadas que prueban otra cosa. Las aridades no se solapan, así que ADL no
// tiene nada que desempatar.
template <class Loader>
static void syncUiWidgets(
    const std::vector<std::pair<uint64_t, const ButtonComponent*>>& buttons,
    const std::vector<std::pair<uint64_t, const TextComponent*>>& texts,
    const std::vector<std::pair<uint64_t, const ProgressBarComponent*>>& bars,
    UiCanvas& canvas, UiWidgetSyncCache& cache, Loader& loader,
    const std::vector<std::pair<uint64_t, uint64_t>>* parents = nullptr,
    const std::vector<std::pair<uint64_t, const LayoutComponent*>>* layouts = nullptr)
{
    UiWidgetLists w;
    w.buttons = buttons;
    w.texts   = texts;
    w.bars    = bars;
    if (layouts) w.layouts = *layouts;
    if (parents) w.parents = *parents;
    DonTopo::syncUiWidgets(w, canvas, cache, loader);
}

// Un click completo sobre p: un frame de hover, uno con el botón abajo y otro
// con el botón arriba. El hit test necesita rects, o sea un buildDrawData
// previo. Los tiempos se separan entre clicks para no cruzar el umbral del
// doble click sin querer.
static void clickEn(UiCanvas& canvas, glm::vec2 p, float t0)
{
    UiInputState in;
    in.mousePos    = p;
    in.timeSeconds = t0;
    canvas.updateInput(in);

    in.mouseDown[0] = true;
    in.timeSeconds  = t0 + 0.05f;
    canvas.updateInput(in);

    in.mouseDown[0] = false;
    in.timeSeconds  = t0 + 0.10f;
    canvas.updateInput(in);
}

// Un script escribe TODOS los campos de los cuatro componentes con valores no
// neutros y distintos entre sí, y C++ los lee del componente de la escena. Los
// valores son todos distintos a propósito: un binding que escribiera siempre en
// el mismo campo, o que se dejara un campo sin conectar, pasaría un test hecho
// con ceros y unos repetidos.
static void test_ui_lua_escribe_todos_los_campos(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Hud");
    go->setCanvas(std::make_shared<CanvasComponent>());
    go->setButton(std::make_shared<ButtonComponent>());
    go->setText(std::make_shared<TextComponent>());
    go->setProgressBar(std::make_shared<ProgressBarComponent>());
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        local c = e:GetCanvas()
        c.scaleMode = UiScaleMode.ScaleWithScreenSize
        c.scaleFactor = 2.5
        c.screenMatch = UiScreenMatch.Shrink
        c.matchWidthOrHeight = 0.75
        c.screenDpi = 141
        c.fallbackDpi = 110
        c.referenceDpi = 72
        c.aspectRatio = 1.75
        c:SetReferenceResolution(1280, 720)
        c:SetSafeArea(11, 12, 13, 14)
        c.renderMode = UiCanvasRenderMode.World
        c.worldScale = 0.0234375
        c.billboard = UiBillboard.YawOnly
        c.depthTest = false

        local b = e:GetButton()
        b.visible = false
        b.atlasPath = "assets/ui/botones.png"
        b.sprite = "base"
        b.interactable = false
        b.selected = true
        b.transition = UiButtonTransition.Animation
        b.normalSprite = "n"
        b.hoverSprite = "h"
        b.pressedSprite = "p"
        b.disabledSprite = "d"
        b.selectedSprite = "s"
        b.fadeDuration = 0.42
        b.text = "Jugar"
        b.fontPath = "assets/titulo.ttf"
        b.fontSize = 23
        b.textAlign = UiTextAlign.Right
        b:SetAnchorMin(0.1, 0.2)
        b:SetAnchorMax(0.3, 0.4)
        b:SetPivot(0.5, 0.6)
        b:SetPosition(31, 32)
        b:SetSize(210, 55)
        b:SetColor(0.11, 0.12, 0.13, 0.14)
        b:SetNormalColor(0.21, 0.22, 0.23, 0.24)
        b:SetHoverColor(0.31, 0.32, 0.33, 0.34)
        b:SetPressedColor(0.41, 0.42, 0.43, 0.44)
        b:SetDisabledColor(0.51, 0.52, 0.53, 0.54)
        b:SetSelectedColor(0.61, 0.62, 0.63, 0.64)
        b:SetTextColor(0.71, 0.72, 0.73, 0.74)

        local t = e:GetText()
        t.visible = false
        t.text = "Vidas: 3"
        t.fontPath = "assets/hud.ttf"
        t.fontSize = 19
        t.outlineWidth = 1.5
        t.align = UiTextAlign.Justify
        t.overflow = UiTextOverflow.Ellipsis
        t.wordWrap = true
        t.boldStrength = 0.33
        t.italicSkew = 0.66
        t:SetAnchorMin(0.15, 0.25)
        t:SetAnchorMax(0.35, 0.45)
        t:SetPivot(0.55, 0.65)
        t:SetPosition(41, 42)
        t:SetSize(220, 65)
        t:SetShadowOffset(3, 4)
        t:SetColor(0.81, 0.82, 0.83, 0.84)
        t:SetOutlineColor(0.91, 0.92, 0.93, 0.94)
        t:SetShadowColor(0.16, 0.17, 0.18, 0.19)

        local p = e:GetProgressBar()
        p.visible = false
        p.value = 7
        p.minValue = 2
        p.maxValue = 12
        p.fillDirection = UiProgressFillDirection.BottomToTop
        p.atlasPath = "assets/ui/barra.png"
        p.backgroundPath = "assets/ui/fondo.png"
        p.fillPath = "assets/ui/relleno.png"
        p:SetAnchorMin(0.18, 0.28)
        p:SetAnchorMax(0.38, 0.48)
        p:SetPivot(0.58, 0.68)
        p:SetPosition(51, 52)
        p:SetSize(320, 24)
        p:SetColor(0.26, 0.27, 0.28, 0.29)
        p:SetFillColor(0.36, 0.37, 0.38, 0.39)

        leidos = {
            escala = c.scaleFactor,
            texto = b.text,
            align = t.align,
            valor = p.value,
            normalizado = p:GetNormalizedValue(),
            modo = c.renderMode,
        }
        anchoRef, altoRef = c:GetReferenceResolution()
        bw, bh = b:GetSize()
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    CHECK(log.empty());

    const CanvasComponent& c = *go->getCanvas();
    CHECK(c.scaleMode == UiScaleMode::ScaleWithScreenSize);
    CHECK(nearlyEqual(c.scaleFactor, 2.5f));
    CHECK(c.screenMatch == UiScreenMatch::Shrink);
    CHECK(nearlyEqual(c.matchWidthOrHeight, 0.75f));
    CHECK(nearlyEqual(c.screenDpi, 141.0f));
    CHECK(nearlyEqual(c.fallbackDpi, 110.0f));
    CHECK(nearlyEqual(c.referenceDpi, 72.0f));
    CHECK(nearlyEqual(c.aspectRatio, 1.75f));
    CHECK(nearlyEqual(c.referenceResolution.x, 1280.0f));
    CHECK(nearlyEqual(c.referenceResolution.y, 720.0f));
    CHECK(nearlyEqual(c.safeArea.left, 11.0f));
    CHECK(nearlyEqual(c.safeArea.top, 12.0f));
    CHECK(nearlyEqual(c.safeArea.right, 13.0f));
    CHECK(nearlyEqual(c.safeArea.bottom, 14.0f));
    CHECK(c.renderMode == UiCanvasRenderMode::World);
    CHECK(nearlyEqual(c.worldScale, 0.0234375f));
    CHECK(c.billboard == UiBillboard::YawOnly);
    CHECK(c.depthTest == false);

    const ButtonComponent& b = *go->getButton();
    CHECK(b.visible == false);
    CHECK(b.atlasPath == "assets/ui/botones.png");
    CHECK(b.sprite == "base");
    CHECK(b.interactable == false);
    CHECK(b.selected == true);
    CHECK(b.transition == UiButtonTransition::Animation);
    CHECK(b.normalSprite == "n");
    CHECK(b.hoverSprite == "h");
    CHECK(b.pressedSprite == "p");
    CHECK(b.disabledSprite == "d");
    CHECK(b.selectedSprite == "s");
    CHECK(nearlyEqual(b.fadeDuration, 0.42f));
    CHECK(b.text == "Jugar");
    CHECK(b.fontPath == "assets/titulo.ttf");
    CHECK(nearlyEqual(b.fontSize, 23.0f));
    CHECK(b.textAlign == UiTextAlign::Right);
    CHECK(nearlyEqual(b.anchorMin.x, 0.1f) && nearlyEqual(b.anchorMin.y, 0.2f));
    CHECK(nearlyEqual(b.anchorMax.x, 0.3f) && nearlyEqual(b.anchorMax.y, 0.4f));
    CHECK(nearlyEqual(b.pivot.x, 0.5f) && nearlyEqual(b.pivot.y, 0.6f));
    CHECK(nearlyEqual(b.position.x, 31.0f) && nearlyEqual(b.position.y, 32.0f));
    CHECK(nearlyEqual(b.size.x, 210.0f) && nearlyEqual(b.size.y, 55.0f));
    CHECK(nearlyEqual(b.color.r, 0.11f) && nearlyEqual(b.color.a, 0.14f));
    CHECK(nearlyEqual(b.normalColor.r, 0.21f) && nearlyEqual(b.normalColor.a, 0.24f));
    CHECK(nearlyEqual(b.hoverColor.r, 0.31f) && nearlyEqual(b.hoverColor.a, 0.34f));
    CHECK(nearlyEqual(b.pressedColor.r, 0.41f) && nearlyEqual(b.pressedColor.a, 0.44f));
    CHECK(nearlyEqual(b.disabledColor.r, 0.51f) && nearlyEqual(b.disabledColor.a, 0.54f));
    CHECK(nearlyEqual(b.selectedColor.r, 0.61f) && nearlyEqual(b.selectedColor.a, 0.64f));
    CHECK(nearlyEqual(b.textColor.r, 0.71f) && nearlyEqual(b.textColor.a, 0.74f));

    const TextComponent& t = *go->getText();
    CHECK(t.visible == false);
    CHECK(t.text == "Vidas: 3");
    CHECK(t.fontPath == "assets/hud.ttf");
    CHECK(nearlyEqual(t.fontSize, 19.0f));
    CHECK(nearlyEqual(t.outlineWidth, 1.5f));
    CHECK(t.align == UiTextAlign::Justify);
    CHECK(t.overflow == UiTextOverflow::Ellipsis);
    CHECK(t.wordWrap == true);
    CHECK(nearlyEqual(t.boldStrength, 0.33f));
    CHECK(nearlyEqual(t.italicSkew, 0.66f));
    CHECK(nearlyEqual(t.anchorMin.x, 0.15f) && nearlyEqual(t.anchorMin.y, 0.25f));
    CHECK(nearlyEqual(t.anchorMax.x, 0.35f) && nearlyEqual(t.anchorMax.y, 0.45f));
    CHECK(nearlyEqual(t.pivot.x, 0.55f) && nearlyEqual(t.pivot.y, 0.65f));
    CHECK(nearlyEqual(t.position.x, 41.0f) && nearlyEqual(t.position.y, 42.0f));
    CHECK(nearlyEqual(t.size.x, 220.0f) && nearlyEqual(t.size.y, 65.0f));
    CHECK(nearlyEqual(t.shadowOffset.x, 3.0f) && nearlyEqual(t.shadowOffset.y, 4.0f));
    CHECK(nearlyEqual(t.color.r, 0.81f) && nearlyEqual(t.color.a, 0.84f));
    CHECK(nearlyEqual(t.outlineColor.r, 0.91f) && nearlyEqual(t.outlineColor.a, 0.94f));
    CHECK(nearlyEqual(t.shadowColor.r, 0.16f) && nearlyEqual(t.shadowColor.a, 0.19f));

    const ProgressBarComponent& p = *go->getProgressBar();
    CHECK(p.visible == false);
    CHECK(nearlyEqual(p.value, 7.0f));
    CHECK(nearlyEqual(p.minValue, 2.0f));
    CHECK(nearlyEqual(p.maxValue, 12.0f));
    CHECK(p.fillDirection == UiProgressFillDirection::BottomToTop);
    CHECK(p.atlasPath == "assets/ui/barra.png");
    CHECK(p.backgroundPath == "assets/ui/fondo.png");
    CHECK(p.fillPath == "assets/ui/relleno.png");
    CHECK(nearlyEqual(p.anchorMin.x, 0.18f) && nearlyEqual(p.anchorMin.y, 0.28f));
    CHECK(nearlyEqual(p.anchorMax.x, 0.38f) && nearlyEqual(p.anchorMax.y, 0.48f));
    CHECK(nearlyEqual(p.pivot.x, 0.58f) && nearlyEqual(p.pivot.y, 0.68f));
    CHECK(nearlyEqual(p.position.x, 51.0f) && nearlyEqual(p.position.y, 52.0f));
    CHECK(nearlyEqual(p.size.x, 320.0f) && nearlyEqual(p.size.y, 24.0f));
    CHECK(nearlyEqual(p.color.r, 0.26f) && nearlyEqual(p.color.a, 0.29f));
    CHECK(nearlyEqual(p.fillColor.r, 0.36f) && nearlyEqual(p.fillColor.a, 0.39f));

    // Y la LECTURA desde Lua devuelve lo mismo que se escribió: sin esto un
    // getter cableado a un campo distinto del setter pasaría desapercibido.
    CHECK(nearlyEqual(sm.lua()["leidos"]["escala"].get<float>(), 2.5f));
    CHECK(sm.lua()["leidos"]["texto"].get<std::string>() == "Jugar");
    CHECK(sm.lua()["leidos"]["align"].get<int>() == (int)UiTextAlign::Justify);
    CHECK(nearlyEqual(sm.lua()["leidos"]["valor"].get<float>(), 7.0f));
    CHECK(nearlyEqual(sm.lua()["leidos"]["normalizado"].get<float>(), 0.5f));
    CHECK(sm.lua()["leidos"]["modo"].get<int>() == (int)UiCanvasRenderMode::World);
    CHECK(nearlyEqual(sm.lua()["anchoRef"].get<float>(), 1280.0f));
    CHECK(nearlyEqual(sm.lua()["altoRef"].get<float>(), 720.0f));
    CHECK(nearlyEqual(sm.lua()["bw"].get<float>(), 210.0f));
    CHECK(nearlyEqual(sm.lua()["bh"].get<float>(), 55.0f));
}

// El getter de un componente que no está devuelve nil, Add lo crea, Remove lo
// quita, y usar el wrapper DESPUÉS del remove da un error de Lua capturable en
// vez de tumbar el proceso (el wrapper resuelve por id en cada acceso).
static void test_ui_getter_nil_add_y_remove(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Widget");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        sinNada = (e:GetButton() == nil) and (e:GetCanvas() == nil) and
                  (e:GetText() == nil) and (e:GetProgressBar() == nil)

        local b = e:AddButton()
        b.text = "Pausa"
        trasAdd = (e:GetButton() ~= nil)

        e:RemoveButton()
        trasRemove = (e:GetButton() == nil)

        -- El wrapper viejo sigue vivo en Lua pero su componente ya no está:
        -- tiene que dar error, no leer memoria liberada.
        okUsoTrasRemove, mensajeTrasRemove = pcall(function() b.text = "otra" end)
    )", sol::script_pass_on_error);
    CHECK(r.valid());

    CHECK(sm.lua()["sinNada"].get<bool>());
    CHECK(sm.lua()["trasAdd"].get<bool>());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasButton());
    CHECK(sm.lua()["okUsoTrasRemove"].get<bool>() == false);
    CHECK(sm.lua()["mensajeTrasRemove"].get<std::string>().find("Button") != std::string::npos);
}

// El Layout desde Lua: el mismo contrato que los otros tres componentes de UI
// (getter que da nil sin componente, Add, campos, Remove). Los valores son no
// neutros y distintos entre sí para que un campo que el binding no escriba no
// pueda pasar por el default.
static void test_ui_layout_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Menu");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        sinLayout = (e:GetLayout() == nil)

        local l = e:AddLayout()
        l.mode = UiLayoutMode.Grid
        l.crossAlign = UiCrossAlign.End
        l.paddingLeft = 3
        l.paddingRight = 5
        l.paddingTop = 7
        l.paddingBottom = 9
        l.columns = 4
        l.fitWidth = true
        l.fitHeight = true
        l.ignoreLayout = true
        l.clipChildren = true
        l.visible = false
        l:SetPosition(11, 13)
        l:SetSize(320, 240)
        l:SetSpacing(17, 19)
        l:SetCellSize(64, 48)

        trasAdd = (e:GetLayout() ~= nil)
        modoLeido = l.mode
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) return;

    CHECK(sm.lua()["sinLayout"].get<bool>());
    CHECK(sm.lua()["trasAdd"].get<bool>());
    CHECK(go->hasLayout());
    if (!go->hasLayout()) return;

    const LayoutComponent& l = *go->getLayout();
    CHECK(l.mode == UiLayoutMode::Grid);
    CHECK(l.crossAlign == UiCrossAlign::End);
    CHECK(nearlyEqual(l.paddingLeft, 3.0f));
    CHECK(nearlyEqual(l.paddingRight, 5.0f));
    CHECK(nearlyEqual(l.paddingTop, 7.0f));
    CHECK(nearlyEqual(l.paddingBottom, 9.0f));
    CHECK(l.columns == 4u);
    CHECK(l.fitWidth == true);
    CHECK(l.fitHeight == true);
    CHECK(l.ignoreLayout == true);
    CHECK(l.clipChildren == true);
    CHECK(l.visible == false);
    CHECK(nearlyEqual(l.position.x, 11.0f));
    CHECK(nearlyEqual(l.position.y, 13.0f));
    CHECK(nearlyEqual(l.size.x, 320.0f));
    CHECK(nearlyEqual(l.size.y, 240.0f));
    CHECK(nearlyEqual(l.spacing.x, 17.0f));
    CHECK(nearlyEqual(l.spacing.y, 19.0f));
    CHECK(nearlyEqual(l.cellSize.x, 64.0f));
    CHECK(nearlyEqual(l.cellSize.y, 48.0f));

    // Y el camino de vuelta: lo que Lua LEE es lo que hay en el componente.
    CHECK(sm.lua()["modoLeido"].get<int>() == (int)UiLayoutMode::Grid);

    auto r2 = sm.lua().safe_script(R"(
        e:RemoveLayout()
        trasRemove = (e:GetLayout() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasLayout());
}

// El Panel desde Lua: mismo contrato que los otros componentes de UI (getter que
// da nil sin componente, Add, campos, Remove). Valores no neutros y distintos
// entre sí: un campo que el binding no escriba no puede pasar por el default.
static void test_ui_panel_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Marco");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        sinPanel = (e:GetPanel() == nil)

        local p = e:AddPanel()
        p.visible = false
        p.raycastTarget = false
        p.atlasPath = "assets/ui/frames.png"
        p.sprite = "marco_dorado"
        p:SetPosition(21, 23)
        p:SetSize(410, 260)
        p:SetAnchorMin(0.125, 0.25)
        p:SetAnchorMax(0.75, 0.875)
        p:SetPivot(0.3125, 0.40625)
        p:SetColor(0.11, 0.12, 0.13, 0.14)

        trasAdd = (e:GetPanel() ~= nil)
        spriteLeido = p.sprite
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) return;

    CHECK(sm.lua()["sinPanel"].get<bool>());
    CHECK(sm.lua()["trasAdd"].get<bool>());
    CHECK(go->hasPanel());
    if (!go->hasPanel()) return;

    const PanelComponent& p = *go->getPanel();
    CHECK(p.visible == false);
    CHECK(p.raycastTarget == false);
    CHECK(p.atlasPath == "assets/ui/frames.png");
    CHECK(p.sprite == "marco_dorado");
    CHECK(nearlyEqual(p.position.x, 21.0f));
    CHECK(nearlyEqual(p.position.y, 23.0f));
    CHECK(nearlyEqual(p.size.x, 410.0f));
    CHECK(nearlyEqual(p.size.y, 260.0f));
    CHECK(nearlyEqual(p.anchorMin.x, 0.125f));
    CHECK(nearlyEqual(p.anchorMin.y, 0.25f));
    CHECK(nearlyEqual(p.anchorMax.x, 0.75f));
    CHECK(nearlyEqual(p.anchorMax.y, 0.875f));
    CHECK(nearlyEqual(p.pivot.x, 0.3125f));
    CHECK(nearlyEqual(p.pivot.y, 0.40625f));
    CHECK(nearlyEqual(p.color.r, 0.11f));
    CHECK(nearlyEqual(p.color.a, 0.14f));

    // Y el camino de vuelta: lo que Lua LEE es lo que hay en el componente.
    CHECK(sm.lua()["spriteLeido"].get<std::string>() == "marco_dorado");

    auto r2 = sm.lua().safe_script(R"(
        e:RemovePanel()
        trasRemove = (e:GetPanel() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasPanel());
}

// El Image desde Lua. Además del rect, los campos PROPIOS del widget del núcleo:
// modo, bordes del 9-slice, tope de tiles y el bloque de Filled.
static void test_ui_image_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Icono");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        sinImage = (e:GetImage() == nil)

        local i = e:AddImage()
        i.visible = false
        i.raycastTarget = false
        i.atlasPath = "assets/ui/iconos.png"
        i.sprite = "corazon"
        i.mode = UiImageMode.Sliced
        i.borderLeft = 3
        i.borderRight = 5
        i.borderTop = 7
        i.borderBottom = 9
        i.fillCenter = false
        i.maxTiles = 777
        i.fillDirection = UiFillDirection.Vertical
        i.fillOrigin = UiFillOrigin.End
        i.fillAmount = 0.375
        i:SetPosition(31, 37)
        i:SetSize(64, 48)

        trasAdd = (e:GetImage() ~= nil)
        modoLeido = i.mode
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) return;

    CHECK(sm.lua()["sinImage"].get<bool>());
    CHECK(sm.lua()["trasAdd"].get<bool>());
    CHECK(go->hasImage());
    if (!go->hasImage()) return;

    const ImageComponent& im = *go->getImage();
    CHECK(im.visible == false);
    CHECK(im.raycastTarget == false);
    CHECK(im.atlasPath == "assets/ui/iconos.png");
    CHECK(im.sprite == "corazon");
    CHECK(im.mode == UiImageMode::Sliced);
    CHECK(nearlyEqual(im.borderLeft, 3.0f));
    CHECK(nearlyEqual(im.borderRight, 5.0f));
    CHECK(nearlyEqual(im.borderTop, 7.0f));
    CHECK(nearlyEqual(im.borderBottom, 9.0f));
    CHECK(im.fillCenter == false);
    CHECK(im.maxTiles == 777u);
    CHECK(im.fillDirection == UiFillDirection::Vertical);
    CHECK(im.fillOrigin == UiFillOrigin::End);
    CHECK(nearlyEqual(im.fillAmount, 0.375f));
    CHECK(nearlyEqual(im.position.x, 31.0f));
    CHECK(nearlyEqual(im.position.y, 37.0f));
    CHECK(nearlyEqual(im.size.x, 64.0f));
    CHECK(nearlyEqual(im.size.y, 48.0f));

    CHECK(sm.lua()["modoLeido"].get<int>() == (int)UiImageMode::Sliced);

    auto r2 = sm.lua().safe_script(R"(
        e:RemoveImage()
        trasRemove = (e:GetImage() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasImage());
}


// Los cuatro widgets interactivos del segundo lote desde Lua: mismo contrato que
// los demás (getter que da nil sin componente, Add, campos, Remove) más el
// camino que solo tienen ellos: OnValueChanged, que es como un script se entera
// de que el jugador ha movido algo.
//
// Valores no neutros y distintos entre sí, por lo de siempre.
static void test_ui_slider_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Volumen");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        sinSlider = (e:GetSlider() == nil)

        local s = e:AddSlider()
        s.visible = false
        s.interactable = false
        s.value = 30
        s.minValue = -10
        s.maxValue = 90
        s.wholeNumbers = true
        s.direction = UiSliderDirection.BottomToTop
        s.handleSize = 33
        s.atlasPath = "assets/ui/hud.png"
        s.backgroundSprite = "pista"
        s.fillSprite = "relleno"
        s.handleSprite = "asa"
        s:SetPosition(19, 23)
        s:SetSize(273, 27)
        s:SetColor(0.11, 0.12, 0.13, 0.14)
        s:SetFillColor(0.21, 0.22, 0.23, 0.24)
        s:SetHandleColor(0.31, 0.32, 0.33, 0.34)

        trasAdd = (e:GetSlider() ~= nil)
        normalizado = s:GetNormalizedValue()
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) return;

    CHECK(sm.lua()["sinSlider"].get<bool>());
    CHECK(sm.lua()["trasAdd"].get<bool>());
    CHECK(go->hasSlider());
    if (!go->hasSlider()) return;

    const SliderComponent& s = *go->getSlider();
    CHECK(s.visible == false);
    CHECK(s.interactable == false);
    CHECK(nearlyEqual(s.value, 30.0f));
    CHECK(nearlyEqual(s.minValue, -10.0f));
    CHECK(nearlyEqual(s.maxValue, 90.0f));
    CHECK(s.wholeNumbers == true);
    CHECK(s.direction == UiSliderDirection::BottomToTop);
    CHECK(nearlyEqual(s.handleSize, 33.0f));
    CHECK(s.atlasPath == "assets/ui/hud.png");
    CHECK(s.backgroundSprite == "pista");
    CHECK(s.fillSprite == "relleno");
    CHECK(s.handleSprite == "asa");
    CHECK(nearlyEqual(s.position.x, 19.0f));
    CHECK(nearlyEqual(s.size.y, 27.0f));
    CHECK(nearlyEqual(s.color.r, 0.11f));
    CHECK(nearlyEqual(s.fillColor.g, 0.22f));
    CHECK(nearlyEqual(s.handleColor.b, 0.33f));

    // El camino de vuelta: (30 - -10) / (90 - -10) = 0.4
    CHECK(nearlyEqual(sm.lua()["normalizado"].get<float>(), 0.4f));

    auto r2 = sm.lua().safe_script(R"(
        e:RemoveSlider()
        trasRemove = (e:GetSlider() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasSlider());
}

// OnValueChanged es lo que distingue a estos widgets de los que solo se pintan:
// el script se entera de que el jugador ha movido algo SIN sondear el valor cada
// frame. El callback lo dispara el nodo vivo, así que hace falta un sync y un
// buildDrawData antes de tocar nada.
static void test_ui_slider_callback_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Volumen");
    go->setSlider(std::make_shared<SliderComponent>());
    go->getSlider()->position   = glm::vec2(0.0f, 0.0f);
    go->getSlider()->size       = glm::vec2(200.0f, 20.0f);
    go->getSlider()->handleSize = 0.0f;
    go->getSlider()->minValue   = 0.0f;
    go->getSlider()->maxValue   = 100.0f;
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        avisos = 0
        ultimo = -1
        e:GetSlider():OnValueChanged(function(v) avisos = avisos + 1; ultimo = v end)
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) return;

    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    UiWidgetLists w;
    w.sliders.emplace_back(go->id, go->getSlider().get());
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    UiInputState in;
    in.mousePos    = glm::vec2(150.0f, 10.0f);
    in.timeSeconds = 0.0f;
    canvas.updateInput(in);
    in.mouseDown[0] = true;
    in.timeSeconds  = 0.016f;
    canvas.updateInput(in);

    CHECK(nearlyEqual(go->getSlider()->value, 75.0f));
    CHECK(sm.lua()["avisos"].get<int>() == 1);
    CHECK(nearlyEqual(sm.lua()["ultimo"].get<float>(), 75.0f));

    // Pasar nil lo quita: el siguiente movimiento no avisa a nadie.
    auto r2 = sm.lua().safe_script("e:GetSlider():OnValueChanged(nil)",
                                   sol::script_pass_on_error);
    CHECK(r2.valid());
    in.mousePos    = glm::vec2(50.0f, 10.0f);
    in.timeSeconds = 0.032f;
    canvas.updateInput(in);
    CHECK(nearlyEqual(go->getSlider()->value, 25.0f));
    CHECK(sm.lua()["avisos"].get<int>() == 1);
}

static void test_ui_checkbox_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Subtitulos");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        sinCheckbox = (e:GetCheckbox() == nil)

        local c = e:AddCheckbox()
        c.visible = false
        c.interactable = false
        c.isOn = true
        c.checkPadding = 5
        c.atlasPath = "assets/ui/widgets.png"
        c.backgroundSprite = "casilla"
        c.checkmarkSprite = "tick"
        c:SetPosition(23, 29)
        c:SetSize(37, 39)
        c:SetColor(0.15, 0.16, 0.17, 0.18)
        c:SetCheckColor(0.25, 0.26, 0.27, 0.28)

        trasAdd = (e:GetCheckbox() ~= nil)
        onLeido = c.isOn
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) return;

    CHECK(sm.lua()["sinCheckbox"].get<bool>());
    CHECK(sm.lua()["trasAdd"].get<bool>());
    CHECK(go->hasCheckbox());
    if (!go->hasCheckbox()) return;

    const CheckboxComponent& c = *go->getCheckbox();
    CHECK(c.visible == false);
    CHECK(c.interactable == false);
    CHECK(c.isOn == true);
    CHECK(nearlyEqual(c.checkPadding, 5.0f));
    CHECK(c.atlasPath == "assets/ui/widgets.png");
    CHECK(c.backgroundSprite == "casilla");
    CHECK(c.checkmarkSprite == "tick");
    CHECK(nearlyEqual(c.position.x, 23.0f));
    CHECK(nearlyEqual(c.size.y, 39.0f));
    CHECK(nearlyEqual(c.color.r, 0.15f));
    CHECK(nearlyEqual(c.checkColor.g, 0.26f));
    CHECK(sm.lua()["onLeido"].get<bool>() == true);

    auto r2 = sm.lua().safe_script(R"(
        e:RemoveCheckbox()
        trasRemove = (e:GetCheckbox() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasCheckbox());
}

static void test_ui_toggle_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Vsync");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        sinToggle = (e:GetToggle() == nil)

        local t = e:AddToggle()
        t.visible = false
        t.interactable = false
        t.isOn = true
        t.knobSize = 25
        t.knobPadding = 3
        t.atlasPath = "assets/ui/widgets.png"
        t.backgroundSprite = "riel"
        t.knobSprite = "mando"
        t:SetPosition(27, 31)
        t:SetSize(71, 33)
        t:SetOffColor(0.35, 0.36, 0.37, 0.38)
        t:SetOnColor(0.45, 0.46, 0.47, 0.48)
        t:SetKnobColor(0.55, 0.56, 0.57, 0.58)

        trasAdd = (e:GetToggle() ~= nil)
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) return;

    CHECK(sm.lua()["sinToggle"].get<bool>());
    CHECK(sm.lua()["trasAdd"].get<bool>());
    CHECK(go->hasToggle());
    if (!go->hasToggle()) return;

    const ToggleComponent& t = *go->getToggle();
    CHECK(t.visible == false);
    CHECK(t.interactable == false);
    CHECK(t.isOn == true);
    CHECK(nearlyEqual(t.knobSize, 25.0f));
    CHECK(nearlyEqual(t.knobPadding, 3.0f));
    CHECK(t.atlasPath == "assets/ui/widgets.png");
    CHECK(t.backgroundSprite == "riel");
    CHECK(t.knobSprite == "mando");
    CHECK(nearlyEqual(t.position.x, 27.0f));
    CHECK(nearlyEqual(t.size.y, 33.0f));
    CHECK(nearlyEqual(t.offColor.r, 0.35f));
    CHECK(nearlyEqual(t.onColor.g, 0.46f));
    CHECK(nearlyEqual(t.knobColor.b, 0.57f));

    auto r2 = sm.lua().safe_script(R"(
        e:RemoveToggle()
        trasRemove = (e:GetToggle() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasToggle());
}

static void test_ui_scrollbar_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("BarraLateral");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        sinScrollbar = (e:GetScrollbar() == nil)

        local s = e:AddScrollbar()
        s.visible = false
        s.interactable = false
        s.value = 0.625
        s.handleFraction = 0.375
        s.direction = UiScrollbarDirection.BottomToTop
        s.numberOfSteps = 7
        s.scrollStep = 0.0625
        s.atlasPath = "assets/ui/widgets.png"
        s.backgroundSprite = "canal"
        s.handleSprite = "pulgar"
        s:SetPosition(29, 33)
        s:SetSize(17, 213)
        s:SetColor(0.19, 0.29, 0.39, 0.49)
        s:SetHandleColor(0.59, 0.69, 0.79, 0.89)

        trasAdd = (e:GetScrollbar() ~= nil)
        pegado = s:SnapValue(0.3)
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) return;

    CHECK(sm.lua()["sinScrollbar"].get<bool>());
    CHECK(sm.lua()["trasAdd"].get<bool>());
    CHECK(go->hasScrollbar());
    if (!go->hasScrollbar()) return;

    const ScrollbarComponent& s = *go->getScrollbar();
    CHECK(s.visible == false);
    CHECK(s.interactable == false);
    CHECK(nearlyEqual(s.value, 0.625f));
    CHECK(nearlyEqual(s.handleFraction, 0.375f));
    CHECK(s.direction == UiScrollbarDirection::BottomToTop);
    CHECK(s.numberOfSteps == 7u);
    CHECK(nearlyEqual(s.scrollStep, 0.0625f));
    CHECK(s.atlasPath == "assets/ui/widgets.png");
    CHECK(s.backgroundSprite == "canal");
    CHECK(s.handleSprite == "pulgar");
    CHECK(nearlyEqual(s.position.x, 29.0f));
    CHECK(nearlyEqual(s.size.y, 213.0f));
    CHECK(nearlyEqual(s.color.r, 0.19f));
    CHECK(nearlyEqual(s.handleColor.g, 0.69f));

    // 7 pasos = 6 tramos: la parada más cercana a 0.3 es 2/6 = 0.3333...
    CHECK(nearlyEqual(sm.lua()["pegado"].get<float>(), 1.0f / 3.0f));

    auto r2 = sm.lua().safe_script(R"(
        e:RemoveScrollbar()
        trasRemove = (e:GetScrollbar() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasScrollbar());
}


// Los tres widgets del tercer lote desde Lua. El InputField es el que trae algo
// que ninguno de los anteriores tenía: se puede escribir en él, así que aquí se
// prueba también el camino completo — tecla del canvas -> componente -> script.
static void test_ui_input_field_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Nombre");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        sinCampo = (e:GetInputField() == nil)

        local f = e:AddInputField()
        f.visible = false
        f.interactable = false
        f.readOnly = true
        f.text = "Jugador1"
        f.placeholder = "Tu nombre..."
        f.fontPath = "assets/fonts/mono.ttf"
        f.fontSize = 21
        f.align = UiTextAlign.Right
        f.padding = 7
        f.characterLimit = 12
        f.contentType = UiInputContentType.Password
        f.passwordChar = "#"
        f.caretWidth = 3
        f.caretBlinkRate = 0.625
        f.atlasPath = "assets/ui/widgets.png"
        f.backgroundSprite = "campo"
        f:SetPosition(33, 37)
        f:SetSize(287, 41)
        f:SetColor(0.12, 0.13, 0.14, 0.15)
        f:SetTextColor(0.22, 0.23, 0.24, 0.25)
        f:SetPlaceholderColor(0.32, 0.33, 0.34, 0.35)
        f:SetCaretColor(0.42, 0.43, 0.44, 0.45)

        trasAdd = (e:GetInputField() ~= nil)
        ensenado = f:GetDisplayText()
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) return;

    CHECK(sm.lua()["sinCampo"].get<bool>());
    CHECK(sm.lua()["trasAdd"].get<bool>());
    CHECK(go->hasInputField());
    if (!go->hasInputField()) return;

    const InputFieldComponent& f = *go->getInputField();
    CHECK(f.visible == false);
    CHECK(f.interactable == false);
    CHECK(f.readOnly == true);
    CHECK(f.text == "Jugador1");
    CHECK(f.placeholder == "Tu nombre...");
    CHECK(f.fontPath == "assets/fonts/mono.ttf");
    CHECK(nearlyEqual(f.fontSize, 21.0f));
    CHECK(f.align == UiTextAlign::Right);
    CHECK(nearlyEqual(f.padding, 7.0f));
    CHECK(f.characterLimit == 12u);
    CHECK(f.contentType == UiInputContentType::Password);
    CHECK(f.passwordChar == "#");
    CHECK(nearlyEqual(f.caretWidth, 3.0f));
    CHECK(nearlyEqual(f.caretBlinkRate, 0.625f));
    CHECK(f.atlasPath == "assets/ui/widgets.png");
    CHECK(f.backgroundSprite == "campo");
    CHECK(nearlyEqual(f.position.x, 33.0f));
    CHECK(nearlyEqual(f.size.y, 41.0f));
    CHECK(nearlyEqual(f.color.r, 0.12f));
    CHECK(nearlyEqual(f.textColor.g, 0.23f));
    CHECK(nearlyEqual(f.placeholderColor.b, 0.34f));
    CHECK(nearlyEqual(f.caretColor.a, 0.45f));

    // Password: lo que se enseña son ocho almohadillas, y el texto sigue entero.
    CHECK(sm.lua()["ensenado"].get<std::string>() == "########");

    auto r2 = sm.lua().safe_script(R"(
        e:RemoveInputField()
        trasRemove = (e:GetInputField() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasInputField());
}

// El camino entero: una tecla entra por el canvas, el handler del nodo la mete
// en el componente y el script se entera por OnValueChanged. Es lo que el canal
// de caracteres del core existe para permitir.
static void test_ui_input_field_callback_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Nombre");
    go->setInputField(std::make_shared<InputFieldComponent>());
    go->getInputField()->position = glm::vec2(0.0f, 0.0f);
    go->getInputField()->size     = glm::vec2(200.0f, 30.0f);
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        avisos = 0
        ultimo = ""
        finales = 0
        local f = e:GetInputField()
        f:OnValueChanged(function(t) avisos = avisos + 1; ultimo = t end)
        f:OnEndEdit(function(t) finales = finales + 1 end)
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) return;

    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    UiWidgetLists w;
    w.inputFields.emplace_back(go->id, go->getInputField().get());
    UiDrawData data;

    syncUiWidgets(w, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);
    canvas.setFocus(canvas.root().children()[0].get());

    UiInputState in;
    in.mousePos    = glm::vec2(100.0f, 15.0f);
    in.timeSeconds = 1.0f;
    in.chars       = { 'H', 'i' };
    canvas.updateInput(in);

    CHECK(go->getInputField()->text == "Hi");
    CHECK(sm.lua()["avisos"].get<int>() == 2);
    CHECK(sm.lua()["ultimo"].get<std::string>() == "Hi");

    in.chars.clear();
    in.keys        = { UiKey::Enter };
    in.timeSeconds = 1.016f;
    canvas.updateInput(in);
    CHECK(sm.lua()["finales"].get<int>() == 1);
}

static void test_ui_dropdown_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Calidad");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        sinCombo = (e:GetDropdown() == nil)

        local d = e:AddDropdown()
        d.visible = false
        d.interactable = false
        d:SetOptions({ "Bajo", "Medio", "Alto" })
        d.value = 2
        d.itemHeight = 27
        d.maxVisibleItems = 5
        d.fontPath = "assets/fonts/ui.ttf"
        d.fontSize = 19
        d.padding = 5
        d.atlasPath = "assets/ui/widgets.png"
        d.backgroundSprite = "combo"
        d.arrowSprite = "flecha"
        d.itemSprite = "fila"
        d:SetPosition(37, 39)
        d:SetSize(197, 35)
        d:SetColor(0.16, 0.17, 0.18, 0.19)
        d:SetListColor(0.26, 0.27, 0.28, 0.29)
        d:SetItemColor(0.36, 0.37, 0.38, 0.39)
        d:SetItemSelectedColor(0.46, 0.47, 0.48, 0.49)
        d:SetArrowColor(0.56, 0.57, 0.58, 0.59)
        d:SetTextColor(0.66, 0.67, 0.68, 0.69)

        trasAdd = (e:GetDropdown() ~= nil)
        cuantas = d:GetOptionCount()
        segunda = d:GetOption(2)
        elegida = d:GetSelectedLabel()
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) return;

    CHECK(sm.lua()["sinCombo"].get<bool>());
    CHECK(sm.lua()["trasAdd"].get<bool>());
    CHECK(go->hasDropdown());
    if (!go->hasDropdown()) return;

    const DropdownComponent& d = *go->getDropdown();
    CHECK(d.visible == false);
    CHECK(d.interactable == false);
    CHECK(d.options.size() == 3);
    CHECK(d.value == 2);
    CHECK(nearlyEqual(d.itemHeight, 27.0f));
    CHECK(d.maxVisibleItems == 5u);
    CHECK(d.fontPath == "assets/fonts/ui.ttf");
    CHECK(nearlyEqual(d.fontSize, 19.0f));
    CHECK(nearlyEqual(d.padding, 5.0f));
    CHECK(d.atlasPath == "assets/ui/widgets.png");
    CHECK(d.backgroundSprite == "combo");
    CHECK(d.arrowSprite == "flecha");
    CHECK(d.itemSprite == "fila");
    CHECK(nearlyEqual(d.position.x, 37.0f));
    CHECK(nearlyEqual(d.size.y, 35.0f));
    CHECK(nearlyEqual(d.color.r, 0.16f));
    CHECK(nearlyEqual(d.listColor.g, 0.27f));
    CHECK(nearlyEqual(d.itemColor.b, 0.38f));
    CHECK(nearlyEqual(d.itemSelectedColor.a, 0.49f));
    CHECK(nearlyEqual(d.arrowColor.r, 0.56f));
    CHECK(nearlyEqual(d.textColor.g, 0.67f));

    // Las opciones se leen desde Lua con índice 1-based, que es lo natural allí.
    CHECK(sm.lua()["cuantas"].get<int>() == 3);
    CHECK(sm.lua()["segunda"].get<std::string>() == "Medio");
    CHECK(sm.lua()["elegida"].get<std::string>() == "Alto");

    auto r2 = sm.lua().safe_script(R"(
        e:RemoveDropdown()
        trasRemove = (e:GetDropdown() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasDropdown());
}

static void test_ui_scroll_view_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Lista");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        sinVista = (e:GetScrollView() == nil)

        local v = e:AddScrollView()
        v.visible = false
        v.horizontal = true
        v.vertical = false
        v.scrollSensitivity = 43
        v.atlasPath = "assets/ui/widgets.png"
        v.backgroundSprite = "marco"
        v:SetPosition(41, 43)
        v:SetSize(311, 217)
        v:SetColor(0.17, 0.18, 0.19, 0.21)
        v:SetContentSize(613, 941)
        v:SetNormalizedPosition(0.3125, 0.6875)

        trasAdd = (e:GetScrollView() ~= nil)
        rx, ry = v:GetScrollRange()
        ox, oy = v:GetContentOffset()
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) return;

    CHECK(sm.lua()["sinVista"].get<bool>());
    CHECK(sm.lua()["trasAdd"].get<bool>());
    CHECK(go->hasScrollView());
    if (!go->hasScrollView()) return;

    const ScrollViewComponent& v = *go->getScrollView();
    CHECK(v.visible == false);
    CHECK(v.horizontal == true);
    CHECK(v.vertical == false);
    CHECK(nearlyEqual(v.scrollSensitivity, 43.0f));
    CHECK(v.atlasPath == "assets/ui/widgets.png");
    CHECK(v.backgroundSprite == "marco");
    CHECK(nearlyEqual(v.position.x, 41.0f));
    CHECK(nearlyEqual(v.size.y, 217.0f));
    CHECK(nearlyEqual(v.color.a, 0.21f));
    CHECK(nearlyEqual(v.contentSize.x, 613.0f));
    CHECK(nearlyEqual(v.contentSize.y, 941.0f));
    CHECK(nearlyEqual(v.normalizedPosition.x, 0.3125f));
    CHECK(nearlyEqual(v.normalizedPosition.y, 0.6875f));

    // El eje vertical está APAGADO: su recorrido es 0 aunque el contenido sea
    // más alto que la vista. El horizontal sí: 613 - 311 = 302.
    CHECK(nearlyEqual(sm.lua()["rx"].get<float>(), 302.0f));
    CHECK(nearlyEqual(sm.lua()["ry"].get<float>(), 0.0f));
    CHECK(nearlyEqual(sm.lua()["ox"].get<float>(), -302.0f * 0.3125f));
    CHECK(nearlyEqual(sm.lua()["oy"].get<float>(), 0.0f));

    auto r2 = sm.lua().safe_script(R"(
        e:RemoveScrollView()
        trasRemove = (e:GetScrollView() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasScrollView());
}

// Lo que un script escribe en el COMPONENTE llega al nodo vivo en el siguiente
// syncUiWidgets. Es la razón de que los setters no toquen el nodo: el sync lo
// vuelca solo.
static void test_ui_valor_de_lua_llega_al_nodo(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Barra");
    go->setProgressBar(std::make_shared<ProgressBarComponent>());
    sm.rebuildAliveSet();
    sm.lua()["e"] = LuaEntity{ go, &sm };

    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    std::vector<std::pair<uint64_t, const ProgressBarComponent*>> barras{
        { go->id, go->getProgressBar().get() } };

    syncUiWidgets({}, {}, barras, canvas, cache, loader);

    auto r = sm.lua().safe_script(R"(
        local p = e:GetProgressBar()
        p:SetSize(300, 30)
        p.value = 0.25
        p.minValue = 0
        p.maxValue = 1
        p:SetColor(0.9, 0.1, 0.2, 1)
    )", sol::script_pass_on_error);
    CHECK(r.valid());

    syncUiWidgets({}, {}, barras, canvas, cache, loader);

    CHECK(cache.barNodes.size() == 1);
    if (cache.barNodes.empty()) return;
    CHECK(nearlyEqual(cache.barNodes[0]->size.x, 300.0f));
    CHECK(nearlyEqual(cache.barNodes[0]->size.y, 30.0f));
    CHECK(nearlyEqual(cache.barNodes[0]->color.r, 0.9f));
    // El relleno es el cuarto del ancho: el valor de Lua ha llegado hasta el
    // rect que se dibuja, no solo hasta el campo.
    CHECK(nearlyEqual(cache.barFills[0]->size.x, 75.0f));
}

// El callback de Lua se dispara UNA vez por click y SIGUE disparándose después
// de que el sync reconstruya la raíz del canvas (clearChildren destruye el nodo
// que tenía el handler). Es el test de la trampa: el dueño del callback es el
// componente y el sync lo reinstala.
static void test_ui_click_sobrevive_a_la_reconstruccion(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Boton");
    go->setButton(std::make_shared<ButtonComponent>());
    GameObject* otro = scene.addGameObject("Otro");
    otro->setButton(std::make_shared<ButtonComponent>());
    otro->getButton()->position = glm::vec2(400.0f, 300.0f);
    sm.rebuildAliveSet();
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        clicks = 0
        dobles = 0
        e:GetButton():OnClick(function() clicks = clicks + 1 end)
        e:GetButton():OnDoubleClick(function() dobles = dobles + 1 end)
    )", sol::script_pass_on_error);
    CHECK(r.valid());

    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    UiDrawData data;

    std::vector<std::pair<uint64_t, const ButtonComponent*>> uno{
        { go->id, go->getButton().get() } };
    std::vector<std::pair<uint64_t, const ButtonComponent*>> dos{
        { go->id, go->getButton().get() },
        { otro->id, otro->getButton().get() } };

    syncUiWidgets(uno, {}, {}, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);
    clickEn(canvas, glm::vec2(20.0f, 20.0f), 1.0f);
    CHECK(sm.lua()["clicks"].get<int>() == 1);
    CHECK(sm.lua()["dobles"].get<int>() == 0);

    // Añadir un botón cambia el conjunto -> el sync llama a clearChildren() y
    // monta nodos NUEVOS. Un handler enganchado a pelo al nodo moriría aquí.
    syncUiWidgets(dos, {}, {}, canvas, cache, loader);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    clickEn(canvas, glm::vec2(20.0f, 20.0f), 10.0f);
    CHECK(sm.lua()["clicks"].get<int>() == 2);

    // Dos clicks seguidos en el mismo sitio: uno solo debe contar como doble.
    clickEn(canvas, glm::vec2(20.0f, 20.0f), 10.2f);
    CHECK(sm.lua()["clicks"].get<int>() == 3);
    CHECK(sm.lua()["dobles"].get<int>() == 1);

    // Y el estado del nodo vuelve al componente: el ratón se quedó encima.
    syncUiWidgets(dos, {}, {}, canvas, cache, loader);
    auto r2 = sm.lua().safe_script("estado = e:GetButton():GetState()",
                                   sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["estado"].get<int>() == (int)UiButtonState::Hover);
}

// Un callback registrado por un script que se recarga en caliente NO se vuelve
// a llamar (apunta a la clase vieja), y uno que sobrevive a su ScriptManager
// tampoco toca el lua_State muerto.
static void test_ui_callback_no_invoca_estado_viejo(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Boton");
    go->setButton(std::make_shared<ButtonComponent>());
    sm.rebuildAliveSet();
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        recargaClicks = 0
        e:GetButton():OnClick(function() recargaClicks = recargaClicks + 1 end)
    )", sol::script_pass_on_error);
    CHECK(r.valid());

    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    UiDrawData data;
    std::vector<std::pair<uint64_t, const ButtonComponent*>> lista{
        { go->id, go->getButton().get() } };

    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);
    clickEn(canvas, glm::vec2(20.0f, 20.0f), 1.0f);
    CHECK(sm.lua()["recargaClicks"].get<int>() == 1);

    // Lo que hace la recarga en caliente al terminar de cargar el .lua.
    sm.invalidateScriptCallbacks();
    clickEn(canvas, glm::vec2(20.0f, 20.0f), 10.0f);
    CHECK(sm.lua()["recargaClicks"].get<int>() == 1);   // mudo, no re-disparado

    // Y ahora el caso duro: el componente sobrevive al ScriptManager que
    // registró el callback. Si el handler guardara un sol::protected_function,
    // aquí ya habría petado al destruirlo.
    Scene otraEscena("Test2");
    GameObject* go2 = otraEscena.addGameObject("Boton2");
    go2->setButton(std::make_shared<ButtonComponent>());

    UiCanvas canvas2;
    UiWidgetSyncCache cache2;
    UiDrawData data2;
    std::vector<std::pair<uint64_t, const ButtonComponent*>> lista2{
        { go2->id, go2->getButton().get() } };
    {
        ScriptManager efimero;
        efimero.init("__scripting_tests_sin_carpeta_de_scripts__");
        efimero.setScene(&otraEscena);
        efimero.rebuildAliveSet();
        efimero.lua()["e2"] = LuaEntity{ go2, &efimero };
        auto r2 = efimero.lua().safe_script(
            "e2:GetButton():OnClick(function() error('no deberia llamarse') end)",
            sol::script_pass_on_error);
        CHECK(r2.valid());

        syncUiWidgets(lista2, {}, {}, canvas2, cache2, loader);
        canvas2.buildDrawData(800, 480, data2);
    }
    // ScriptManager destruido: el click no puede llamar a nada.
    clickEn(canvas2, glm::vec2(20.0f, 20.0f), 1.0f);
    CHECK(go2->getButton()->callbacks.ptr->onClick != nullptr);   // sigue ahí, pero mudo
}

// Un error dentro de un callback se registra en el Log y NO impide que el
// callback del otro botón se ejecute.
static void test_ui_error_en_callback_no_tumba_el_tick(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* roto = scene.addGameObject("Roto");
    roto->setButton(std::make_shared<ButtonComponent>());
    GameObject* sano = scene.addGameObject("Sano");
    sano->setButton(std::make_shared<ButtonComponent>());
    sano->getButton()->position = glm::vec2(400.0f, 300.0f);
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["roto"] = LuaEntity{ roto, &sm };
    sm.lua()["sano"] = LuaEntity{ sano, &sm };

    auto r = sm.lua().safe_script(R"(
        sanoClicks = 0
        roto:GetButton():OnClick(function() error("callback roto a proposito") end)
        sano:GetButton():OnClick(function() sanoClicks = sanoClicks + 1 end)
    )", sol::script_pass_on_error);
    CHECK(r.valid());

    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    UiDrawData data;
    std::vector<std::pair<uint64_t, const ButtonComponent*>> lista{
        { roto->id, roto->getButton().get() },
        { sano->id, sano->getButton().get() } };

    syncUiWidgets(lista, {}, {}, canvas, cache, loader);
    canvas.buildDrawData(800, 480, data);

    clickEn(canvas, glm::vec2(20.0f, 20.0f), 1.0f);      // el roto
    clickEn(canvas, glm::vec2(420.0f, 320.0f), 10.0f);   // el sano

    CHECK(logContains(log, "callback roto a proposito"));
    CHECK(logContains(log, "Button.OnClick"));
    CHECK(sm.lua()["sanoClicks"].get<int>() == 1);
}

// Los cuatro componentes en el MISMO GameObject: cada wrapper escribe en el
// suyo y el sync monta los tres nodos sin mezclarlos (Button, Text y
// ProgressBar comparten id de dueño).
static void test_ui_cuatro_componentes_en_el_mismo_objeto(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Todo");
    go->setCanvas(std::make_shared<CanvasComponent>());
    go->setButton(std::make_shared<ButtonComponent>());
    go->setText(std::make_shared<TextComponent>());
    go->setProgressBar(std::make_shared<ProgressBarComponent>());
    sm.rebuildAliveSet();
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        e:GetButton().text = "Boton"
        e:GetText().text = "Etiqueta"
        e:GetButton():SetSize(111, 22)
        e:GetText():SetSize(333, 44)
        e:GetProgressBar():SetSize(555, 66)
        e:GetCanvas().scaleFactor = 3
        e:GetProgressBar().value = 0.75
    )", sol::script_pass_on_error);
    CHECK(r.valid());

    CHECK(go->getButton()->text == "Boton");
    CHECK(go->getText()->text == "Etiqueta");
    CHECK(nearlyEqual(go->getButton()->size.x, 111.0f));
    CHECK(nearlyEqual(go->getText()->size.x, 333.0f));
    CHECK(nearlyEqual(go->getProgressBar()->size.x, 555.0f));
    CHECK(nearlyEqual(go->getCanvas()->scaleFactor, 3.0f));

    UiCanvas canvas;
    UiWidgetSyncCache cache;
    FakeUiLoader loader;
    std::vector<std::pair<uint64_t, const ButtonComponent*>> botones{
        { go->id, go->getButton().get() } };
    std::vector<std::pair<uint64_t, const TextComponent*>> textos{
        { go->id, go->getText().get() } };
    std::vector<std::pair<uint64_t, const ProgressBarComponent*>> barras{
        { go->id, go->getProgressBar().get() } };
    syncUiWidgets(botones, textos, barras, canvas, cache, loader);

    CHECK(cache.buttonNodes.size() == 1);
    CHECK(cache.textNodes.size() == 1);
    CHECK(cache.barNodes.size() == 1);
    if (cache.buttonNodes.size() != 1 || cache.textNodes.size() != 1 ||
        cache.barNodes.size() != 1) return;
    CHECK(nearlyEqual(cache.buttonNodes[0]->size.x, 111.0f));
    CHECK(nearlyEqual(cache.textNodes[0]->size.x, 333.0f));
    CHECK(nearlyEqual(cache.barNodes[0]->size.x, 555.0f));
    CHECK(cache.textNodes[0]->text == "Etiqueta");
    CHECK(cache.buttonLabels[0] != nullptr && cache.buttonLabels[0]->text == "Boton");
}

// ---------------------------------------------------------------------------
// Physics.Raycast — la PxScene la comparte todo el fichero (una sola
// PxFoundation por proceso, ver la cabecera): cada test monta sus colliders en
// un Scene local y los suelta al salir.
// ---------------------------------------------------------------------------

// Esfera de radio 1 centrada en (0,2,10). El rayo (0,2,0)->+Z la toca a 9
// unidades, en el punto (0,2,9) y con normal (0,0,-1): los tres campos con
// valores distintos entre sí y ninguno neutro, así un hit relleno a ceros no
// pasaría. El owner se pone a mano igual que hace Scene.cpp al deserializar.
static GameObject* addDiana(Scene& scene, PhysicsManager& pm, const char* name,
                            const glm::vec3& center, bool withOwner = true)
{
    GameObject* go = scene.addGameObject(name);
    auto col = pm.createSphereColliderComponent(1.0f, center, go->worldTransform, /*dynamic=*/false);
    if (withOwner) col->setOwner(go);
    go->setSphereCollider(col);
    return go;
}

static bool luaIsNil(ScriptManager& sm, const char* name)
{
    sol::object o = sm.lua()[name];
    return !o.valid() || o.get_type() == sol::type::lua_nil;
}

static void test_raycast_campos_del_impacto(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    sm.lua().script("hit = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 100)");

    sol::optional<sol::table> hit = sm.lua()["hit"];
    CHECK(hit.has_value());
    if (!hit) return;
    CHECK(nearlyEqual((*hit)["distance"].get<float>(), 9.0f));
    glm::vec3 p = (*hit)["point"].get<glm::vec3>();
    glm::vec3 n = (*hit)["normal"].get<glm::vec3>();
    CHECK(nearlyEqual(p.x, 0.0f) && nearlyEqual(p.y, 2.0f) && nearlyEqual(p.z, 9.0f));
    CHECK(nearlyEqual(n.x, 0.0f) && nearlyEqual(n.y, 0.0f) && nearlyEqual(n.z, -1.0f));
    LuaEntity e = (*hit)["entity"].get<LuaEntity>();
    CHECK(e.go == go);
}

// La dirección se normaliza dentro: con (0,0,5) la distancia sigue siendo 9
// (PhysX exige dir unitaria; sin normalizar sale escalada o directamente mal).
static void test_raycast_normaliza_la_direccion(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    sm.lua().script("hit = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,5), 100)");
    sol::optional<sol::table> hit = sm.lua()["hit"];
    CHECK(hit.has_value());
    if (!hit) return;
    CHECK(nearlyEqual((*hit)["distance"].get<float>(), 9.0f));
}

// Dirección de longitud 0 -> nil sin tocar PhysX.
static void test_raycast_direccion_cero(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    sm.lua().script("hit = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,0), 100)");
    CHECK(luaIsNil(sm, "hit"));
}

// maxDistance ausente o <= 0 -> default 1000 (el impacto a 9 entra); un
// maxDistance corto de verdad recorta.
static void test_raycast_max_distance(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    sm.lua().script(
        "sinArg   = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1))\n"
        "negativo = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), -5)\n"
        "corto    = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 5)\n");
    CHECK(!luaIsNil(sm, "sinArg"));
    CHECK(!luaIsNil(sm, "negativo"));
    CHECK(luaIsNil(sm, "corto"));
}

// hitTriggers: por defecto un collider Is Trigger NO cuenta como impacto (PhysX
// sí lo deja en las consultas de escena, lo descarta nuestro prefiltro).
static void test_raycast_hit_triggers(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    pm.setTrigger(go->getSphereCollider(), true);
    sm.rebuildAliveSet();

    sm.lua().script(
        "porDefecto  = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 100)\n"
        "conTriggers = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 100, { hitTriggers = true })\n");
    CHECK(luaIsNil(sm, "porDefecto"));
    CHECK(!luaIsNil(sm, "conTriggers"));

    pm.setTrigger(go->getSphereCollider(), false);
}

// static / dynamic: la diana es un PxRigidStatic, así que apagando 'static'
// desaparece; apagando solo 'dynamic' sigue ahí; con los dos apagados, nil.
static void test_raycast_filtro_static_dynamic(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    sm.lua().script(
        "sinStatic  = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 100, { static = false })\n"
        "sinDynamic = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 100, { dynamic = false })\n"
        "ninguno    = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 100, { static = false, dynamic = false })\n");
    CHECK(luaIsNil(sm, "sinStatic"));
    CHECK(!luaIsNil(sm, "sinDynamic"));
    CHECK(luaIsNil(sm, "ninguno"));
}

// ignore: la entidad que dispara no se choca consigo misma, pero ignorar a otra
// no le quita el impacto.
static void test_raycast_ignore(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* diana = addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    GameObject* otro  = scene.addGameObject("Otro");
    sm.rebuildAliveSet();
    sm.lua()["diana"] = LuaEntity{ diana, &sm };
    sm.lua()["otro"]  = LuaEntity{ otro,  &sm };

    sm.lua().script(
        "ignorada   = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 100, { ignore = diana })\n"
        "ignoraOtro = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 100, { ignore = otro })\n");
    CHECK(luaIsNil(sm, "ignorada"));
    CHECK(!luaIsNil(sm, "ignoraOtro"));
}

// Un argumento del tipo equivocado devuelve nil y avisa, pero NO tumba el
// script: la línea siguiente se ejecuta.
static void test_raycast_tipos_invalidos_no_tumban_el_script(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });

    sm.lua().script(
        "r1 = Physics.Raycast('hola', 3)\n"
        "r2 = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 'lejos')\n"
        "r3 = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 100, 'no soy tabla')\n"
        "r4 = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 100, { ignore = 7 })\n"
        "r5 = Physics.RaycastHit('hola')\n"
        "siguio = true\n");
    CHECK(luaIsNil(sm, "r1"));
    CHECK(luaIsNil(sm, "r2"));
    CHECK(luaIsNil(sm, "r3"));
    CHECK(luaIsNil(sm, "r4"));
    CHECK(sm.lua()["r5"].get<bool>() == false);
    CHECK(sm.lua()["siguio"].get<bool>() == true);
    CHECK(logContains(log, "WARN"));
    CHECK(logContains(log, "Raycast"));
    sm.setLogCallback(nullptr);
}

// Sin PhysicsManager (fuera de Play) -> nil, no excepción.
static void test_raycast_sin_fisica(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    sm.setPhysicsManager(nullptr);
    sm.lua().script(
        "hit  = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 100)\n"
        "toco = Physics.RaycastHit(Vec3(0,2,0), Vec3(0,0,1), 100)\n");
    sm.setPhysicsManager(&pm);

    CHECK(luaIsNil(sm, "hit"));
    CHECK(sm.lua()["toco"].get<bool>() == false);
}

// Collider sin GameObject asociado: entity nil, el resto de campos llenos.
static void test_raycast_actor_sin_gameobject(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f), /*withOwner=*/false);
    sm.rebuildAliveSet();

    sm.lua().script(
        "hit = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 100)\n"
        "sinEntity = (hit ~= nil) and (hit.entity == nil)\n");
    sol::optional<sol::table> hit = sm.lua()["hit"];
    CHECK(hit.has_value());
    if (!hit) return;
    CHECK(sm.lua()["sinEntity"].get<bool>() == true);
    CHECK(nearlyEqual((*hit)["distance"].get<float>(), 9.0f));
    CHECK(nearlyEqual((*hit)["point"].get<glm::vec3>().z, 9.0f));
}

// RaycastHit: solo el booleano, mismos filtros.
static void test_raycast_hit_booleano(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    sm.lua().script(
        "toca   = Physics.RaycastHit(Vec3(0,2,0), Vec3(0,0,1), 100)\n"
        "noToca = Physics.RaycastHit(Vec3(0,2,0), Vec3(0,0,-1), 100)\n"
        "corto  = Physics.RaycastHit(Vec3(0,2,0), Vec3(0,0,1), 5)\n");
    CHECK(sm.lua()["toca"].get<bool>() == true);
    CHECK(sm.lua()["noToca"].get<bool>() == false);
    CHECK(sm.lua()["corto"].get<bool>() == false);
}

// ---------------------------------------------------------------------------
// Physics.RaycastAll — mismas dianas, pero TRES en fila sobre el mismo rayo.
// ---------------------------------------------------------------------------

// Tres esferas a z=10/20/30: el rayo (0,2,0)->+Z las toca a 9, 19 y 29. Se
// crean en orden INVERSO (la más lejana primero) a propósito: PhysX entrega los
// touches en el orden del barrido espacial, no por distancia, así que un
// RaycastAll sin ordenar tiene todas las papeletas de sacarlas al revés.
static void test_raycast_all_ordenado_por_distancia(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* lejos  = addDiana(scene, pm, "Lejos",  glm::vec3(0.0f, 2.0f, 30.0f));
    GameObject* medio  = addDiana(scene, pm, "Medio",  glm::vec3(0.0f, 2.0f, 20.0f));
    GameObject* cerca  = addDiana(scene, pm, "Cerca",  glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    sm.lua().script(
        "hits = Physics.RaycastAll(Vec3(0,2,0), Vec3(0,0,1), 100)\n"
        "n = #hits\n");

    CHECK(sm.lua()["n"].get<int>() == 3);
    sol::optional<sol::table> hits = sm.lua()["hits"];
    CHECK(hits.has_value());
    if (!hits || sm.lua()["n"].get<int>() != 3) return;

    // Orden estricto por distancia ascendente + la entidad que toca a cada una.
    const float d1 = (*hits)[1]["distance"].get<float>();
    const float d2 = (*hits)[2]["distance"].get<float>();
    const float d3 = (*hits)[3]["distance"].get<float>();
    CHECK(nearlyEqual(d1, 9.0f));
    CHECK(nearlyEqual(d2, 19.0f));
    CHECK(nearlyEqual(d3, 29.0f));
    CHECK(d1 < d2 && d2 < d3);
    CHECK((*hits)[1]["entity"].get<LuaEntity>().go == cerca);
    CHECK((*hits)[2]["entity"].get<LuaEntity>().go == medio);
    CHECK((*hits)[3]["entity"].get<LuaEntity>().go == lejos);
}

// Cada elemento lleva EXACTAMENTE los mismos campos (y valores) que devolvería
// Physics.Raycast para ese impacto: se comparan uno contra otro, no contra
// constantes, así el día que Raycast cambie de forma este test lo canta.
static void test_raycast_all_misma_forma_que_raycast(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    sm.lua().script(
        "uno   = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 100)\n"
        "todos = Physics.RaycastAll(Vec3(0,2,0), Vec3(0,0,1), 100)\n"
        "n = #todos\n");
    CHECK(sm.lua()["n"].get<int>() == 1);
    sol::optional<sol::table> uno = sm.lua()["uno"];
    sol::optional<sol::table> todos = sm.lua()["todos"];
    CHECK(uno.has_value() && todos.has_value());
    if (!uno || !todos || sm.lua()["n"].get<int>() != 1) return;

    sol::table primero = (*todos)[1];
    CHECK(nearlyEqual(primero["distance"].get<float>(), (*uno)["distance"].get<float>()));
    const glm::vec3 pA = primero["point"].get<glm::vec3>();
    const glm::vec3 pB = (*uno)["point"].get<glm::vec3>();
    const glm::vec3 nA = primero["normal"].get<glm::vec3>();
    const glm::vec3 nB = (*uno)["normal"].get<glm::vec3>();
    CHECK(nearlyEqual(pA.x, pB.x) && nearlyEqual(pA.y, pB.y) && nearlyEqual(pA.z, pB.z));
    CHECK(nearlyEqual(nA.x, nB.x) && nearlyEqual(nA.y, nB.y) && nearlyEqual(nA.z, nB.z));
    CHECK(primero["entity"].get<LuaEntity>().go == (*uno)["entity"].get<LuaEntity>().go);

    // Y ningún campo de más: la tabla del hit tiene exactamente 4 claves.
    sm.lua().script(
        "claves = 0\n"
        "for k, v in pairs(todos[1]) do claves = claves + 1 end\n");
    CHECK(sm.lua()["claves"].get<int>() == 4);
}

// Sin impactos, sin física y con argumentos inválidos: SIEMPRE tabla (vacía),
// nunca nil. Un nil aquí rompería el ipairs del caller.
static void test_raycast_all_sin_impactos_devuelve_tabla_vacia(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });

    sm.lua().script(
        "alReves = Physics.RaycastAll(Vec3(0,2,0), Vec3(0,0,-1), 100)\n"
        "corto   = Physics.RaycastAll(Vec3(0,2,0), Vec3(0,0,1), 5)\n"
        "malos   = Physics.RaycastAll('hola', 3)\n"
        "esTabla = (type(alReves) == 'table') and (type(corto) == 'table') and (type(malos) == 'table')\n"
        "vacias  = (#alReves == 0) and (#corto == 0) and (#malos == 0)\n"
        "siguio  = true\n");

    CHECK(sm.lua()["esTabla"].get<bool>() == true);
    CHECK(sm.lua()["vacias"].get<bool>() == true);
    CHECK(sm.lua()["siguio"].get<bool>() == true);
    CHECK(logContains(log, "WARN"));
    CHECK(logContains(log, "RaycastAll"));
    sm.setLogCallback(nullptr);

    // Fuera de Play (sin PhysicsManager) tampoco sale nil.
    sm.setPhysicsManager(nullptr);
    sm.lua().script(
        "fuera = Physics.RaycastAll(Vec3(0,2,0), Vec3(0,0,1), 100)\n"
        "fueraOk = (type(fuera) == 'table') and (#fuera == 0)\n");
    sm.setPhysicsManager(&pm);
    CHECK(sm.lua()["fueraOk"].get<bool>() == true);
}

// Los filtros de options valen igual que en Raycast: el trigger del medio solo
// aparece con hitTriggers, y el ignore quita justo esa entidad de la lista.
static void test_raycast_all_filtros(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* cerca = addDiana(scene, pm, "Cerca", glm::vec3(0.0f, 2.0f, 10.0f));
    GameObject* medio = addDiana(scene, pm, "Medio", glm::vec3(0.0f, 2.0f, 20.0f));
    addDiana(scene, pm, "Lejos", glm::vec3(0.0f, 2.0f, 30.0f));
    pm.setTrigger(medio->getSphereCollider(), true);
    sm.rebuildAliveSet();
    sm.lua()["cerca"] = LuaEntity{ cerca, &sm };

    sm.lua().script(
        "porDefecto  = #Physics.RaycastAll(Vec3(0,2,0), Vec3(0,0,1), 100)\n"
        "conTriggers = #Physics.RaycastAll(Vec3(0,2,0), Vec3(0,0,1), 100, { hitTriggers = true })\n"
        "sinStatic   = #Physics.RaycastAll(Vec3(0,2,0), Vec3(0,0,1), 100, { static = false })\n"
        "ignorando   = Physics.RaycastAll(Vec3(0,2,0), Vec3(0,0,1), 100, { ignore = cerca })\n"
        "nIgnorando  = #ignorando\n");

    CHECK(sm.lua()["porDefecto"].get<int>() == 2);   // el trigger no bloquea ni cuenta
    CHECK(sm.lua()["conTriggers"].get<int>() == 3);
    CHECK(sm.lua()["sinStatic"].get<int>() == 0);
    CHECK(sm.lua()["nIgnorando"].get<int>() == 1);   // queda solo Lejos
    if (sm.lua()["nIgnorando"].get<int>() == 1)
    {
        sol::table ign = sm.lua()["ignorando"];
        CHECK(nearlyEqual(ign[1]["distance"].get<float>(), 29.0f));
    }

    pm.setTrigger(medio->getSphereCollider(), false);
}

// Physics.Raycast sigue parando en el PRIMER impacto: RaycastAll no puede
// haberle contagiado el eNO_BLOCK (con él, 'block' se queda sin escribir y la
// distancia saldría basura o el hit directamente nil).
static void test_raycast_all_no_altera_raycast(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* cerca = addDiana(scene, pm, "Cerca", glm::vec3(0.0f, 2.0f, 10.0f));
    addDiana(scene, pm, "Lejos", glm::vec3(0.0f, 2.0f, 30.0f));
    sm.rebuildAliveSet();

    sm.lua().script(
        "todos = Physics.RaycastAll(Vec3(0,2,0), Vec3(0,0,1), 100)\n"
        "uno   = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,1), 100)\n"
        "toca  = Physics.RaycastHit(Vec3(0,2,0), Vec3(0,0,1), 100)\n");
    sol::optional<sol::table> uno = sm.lua()["uno"];
    CHECK(uno.has_value());
    if (!uno) return;
    CHECK(nearlyEqual((*uno)["distance"].get<float>(), 9.0f));
    CHECK((*uno)["entity"].get<LuaEntity>().go == cerca);
    CHECK(sm.lua()["toca"].get<bool>() == true);
}

// ---------------------------------------------------------------------------
// Physics.SphereCast / OverlapSphere / OverlapBox — mismas dianas (esferas de
// radio 1) y los mismos filtros de 'options' que el rayo.
// ---------------------------------------------------------------------------

// Devuelve la Entity que hay en out[i] (1-indexado) del array de un overlap, o
// nullptr si no es una Entity viva.
static GameObject* overlapAt(ScriptManager& sm, const char* name, int i)
{
    sol::optional<sol::table> t = sm.lua()[name];
    if (!t) return nullptr;
    sol::object o = (*t)[i];
    if (!o.valid() || !o.is<LuaEntity>()) return nullptr;
    return o.as<LuaEntity>().go;
}

// El sweep es el rayo CON GROSOR: desde (0,3.5,0) el rayo pasa 0.5 por encima
// de la diana (centro y=2, radio 1) y no la toca, pero barriendo una esfera de
// radio 0.8 sí. Con el radio ignorado (o a 0) el CHECK de 'gordo' se pone rojo.
// De paso fija la forma de la tabla: los mismos cuatro campos que Raycast.
static void test_sphere_cast_usa_el_radio(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    sm.lua().script(
        "rayo  = Physics.Raycast(Vec3(0,3.5,0), Vec3(0,0,1), 100)\n"
        "gordo = Physics.SphereCast(Vec3(0,3.5,0), Vec3(0,0,1), 0.8, 100)\n"
        "fino  = Physics.SphereCast(Vec3(0,3.5,0), Vec3(0,0,1), 0.1, 100)\n"
        "recto = Physics.SphereCast(Vec3(0,2,0), Vec3(0,0,1), 0.5, 100)\n");

    CHECK(luaIsNil(sm, "rayo"));   // el rayo de grosor cero pasa de largo
    CHECK(luaIsNil(sm, "fino"));   // y una esfera demasiado fina, también
    CHECK(!luaIsNil(sm, "gordo")); // pero la gorda alcanza

    sol::optional<sol::table> recto = sm.lua()["recto"];
    CHECK(recto.has_value());
    if (!recto) return;
    // Centro a 10, radio de la diana 1, radio barrido 0.5 -> contacto a 8.5.
    CHECK(nearlyEqual((*recto)["distance"].get<float>(), 8.5f));
    glm::vec3 p = (*recto)["point"].get<glm::vec3>();
    glm::vec3 n = (*recto)["normal"].get<glm::vec3>();
    CHECK(nearlyEqual(p.z, 9.0f));                        // punto sobre la diana
    CHECK(nearlyEqual(n.x, 0.0f) && nearlyEqual(n.z, -1.0f));
    CHECK((*recto)["entity"].get<LuaEntity>().go == go);
}

// Barridos que no tocan: al revés, corto por maxDistance, y radio inválido
// (0 o negativo -> nil con aviso, nunca geometría inválida a PhysX).
static void test_sphere_cast_pasa_de_largo(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });

    sm.lua().script(
        "alReves  = Physics.SphereCast(Vec3(0,2,0), Vec3(0,0,-1), 0.5, 100)\n"
        "corto    = Physics.SphereCast(Vec3(0,2,0), Vec3(0,0,1), 0.5, 5)\n"
        "cero     = Physics.SphereCast(Vec3(0,2,0), Vec3(0,0,1), 0, 100)\n"
        "negativo = Physics.SphereCast(Vec3(0,2,0), Vec3(0,0,1), -3, 100)\n"
        "sinRadio = Physics.SphereCast(Vec3(0,2,0), Vec3(0,0,1))\n"
        "malos    = Physics.SphereCast('hola', 3, 1, 100)\n"
        "siguio   = true\n");

    CHECK(luaIsNil(sm, "alReves"));
    CHECK(luaIsNil(sm, "corto"));
    CHECK(luaIsNil(sm, "cero"));
    CHECK(luaIsNil(sm, "negativo"));
    CHECK(luaIsNil(sm, "sinRadio"));
    CHECK(luaIsNil(sm, "malos"));
    CHECK(sm.lua()["siguio"].get<bool>() == true);
    CHECK(logContains(log, "WARN"));
    CHECK(logContains(log, "SphereCast"));
    sm.setLogCallback(nullptr);
}

// 0, 1 y N solapes con la misma escena, cambiando sólo el radio. El caso N cae
// si falta el eNO_BLOCK (la consulta cerraría en el primero) y el caso 0 fija
// que sale tabla vacía, no nil.
static void test_overlap_sphere_cero_uno_n(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* cerca = addDiana(scene, pm, "Cerca", glm::vec3(0.0f, 2.0f, 10.0f));
    addDiana(scene, pm, "Medio", glm::vec3(0.0f, 2.0f, 20.0f));
    addDiana(scene, pm, "Lejos", glm::vec3(0.0f, 2.0f, 30.0f));
    // Cuarta diana DENTRO del radio de 'tres' pero sin GameObject detrás: hay
    // cuatro solapes y sólo tres entidades que devolver, así que 'nTres' == 3
    // fija que los actores huérfanos se omiten en vez de colar un nil (o de
    // reventar) en el array.
    addDiana(scene, pm, "Anonima", glm::vec3(0.0f, 2.0f, 21.0f), /*withOwner=*/false);
    // Y 'Cerca' se lleva un SEGUNDO collider en el mismo sitio: dos shapes que
    // solapan, un solo GameObject. 'nUno' == 1 fija que la entidad no sale
    // duplicada en el array.
    auto extra = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f, 2.0f, 10.0f),
                                                glm::mat4(1.0f), /*dynamic=*/false);
    extra->setOwner(cerca);
    cerca->setBoxCollider(extra);
    sm.rebuildAliveSet();

    sm.lua().script(
        "vacio  = Physics.OverlapSphere(Vec3(0,500,0), 2)\n"
        "uno    = Physics.OverlapSphere(Vec3(0,2,10), 2)\n"
        "tres   = Physics.OverlapSphere(Vec3(0,2,20), 15)\n"
        // Sin radio: argumento ausente, tabla vacía y aviso (y NO un crash al
        // mirarle el tipo a un sol::object sin lua_State).
        "sinRadio = Physics.OverlapSphere(Vec3(0,2,10))\n"
        "nVacio  = #vacio\n"
        "nUno    = #uno\n"
        "nTres   = #tres\n"
        "nSinRadio = #sinRadio\n"
        "esTabla = (type(vacio) == 'table')\n");

    CHECK(sm.lua()["esTabla"].get<bool>() == true);
    CHECK(sm.lua()["nSinRadio"].get<int>() == 0);
    CHECK(sm.lua()["nVacio"].get<int>() == 0);
    CHECK(sm.lua()["nUno"].get<int>() == 1);
    CHECK(sm.lua()["nTres"].get<int>() == 3);
    CHECK(overlapAt(sm, "uno", 1) == cerca);
}

// Los mismos filtros que el rayo: el trigger sólo aparece con hitTriggers, y
// 'ignore' quita justo su GameObject de la lista.
static void test_overlap_sphere_filtros(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* cerca = addDiana(scene, pm, "Cerca", glm::vec3(0.0f, 2.0f, 10.0f));
    GameObject* medio = addDiana(scene, pm, "Medio", glm::vec3(0.0f, 2.0f, 20.0f));
    GameObject* lejos = addDiana(scene, pm, "Lejos", glm::vec3(0.0f, 2.0f, 30.0f));
    pm.setTrigger(medio->getSphereCollider(), true);
    sm.rebuildAliveSet();
    sm.lua()["cerca"] = LuaEntity{ cerca, &sm };

    sm.lua().script(
        "porDefecto  = Physics.OverlapSphere(Vec3(0,2,20), 15)\n"
        "conTriggers = Physics.OverlapSphere(Vec3(0,2,20), 15, { hitTriggers = true })\n"
        "sinStatic   = Physics.OverlapSphere(Vec3(0,2,20), 15, { static = false })\n"
        "ignorando   = Physics.OverlapSphere(Vec3(0,2,20), 15, { ignore = cerca })\n"
        "nPorDefecto = #porDefecto\n"
        "nConTriggers= #conTriggers\n"
        "nSinStatic  = #sinStatic\n"
        "nIgnorando  = #ignorando\n");

    CHECK(sm.lua()["nPorDefecto"].get<int>() == 2);   // el trigger no cuenta
    CHECK(sm.lua()["nConTriggers"].get<int>() == 3);
    CHECK(sm.lua()["nSinStatic"].get<int>() == 0);
    CHECK(sm.lua()["nIgnorando"].get<int>() == 1);    // sin trigger y sin Cerca
    CHECK(overlapAt(sm, "ignorando", 1) == lejos);

    pm.setTrigger(medio->getSphereCollider(), false);
}

// La caja está ORIENTADA: larga en Z ve las tres dianas, girada 90° sobre Y no
// ve ninguna. Y el tercer argumento se desambigua por tipo: una tabla es
// 'options', no una rotación.
static void test_overlap_box_rotacion_y_options(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    addDiana(scene, pm, "Cerca", glm::vec3(0.0f, 2.0f, 10.0f));
    GameObject* medio = addDiana(scene, pm, "Medio", glm::vec3(0.0f, 2.0f, 20.0f));
    addDiana(scene, pm, "Lejos", glm::vec3(0.0f, 2.0f, 30.0f));
    pm.setTrigger(medio->getSphereCollider(), true);
    sm.rebuildAliveSet();

    sm.lua().script(
        "larga    = #Physics.OverlapBox(Vec3(0,2,20), Vec3(1,1,15))\n"
        "girada   = #Physics.OverlapBox(Vec3(0,2,20), Vec3(1,1,15), Vec3(0,90,0))\n"
        "conOpts  = #Physics.OverlapBox(Vec3(0,2,20), Vec3(1,1,15), { hitTriggers = true })\n"
        "rotYOpts = #Physics.OverlapBox(Vec3(0,2,20), Vec3(1,1,15), Vec3(0,0,0), { hitTriggers = true })\n"
        "malos    = Physics.OverlapBox(Vec3(0,2,20), 7)\n"
        "nMalos   = #malos\n");

    CHECK(sm.lua()["larga"].get<int>() == 2);      // Cerca y Lejos (Medio es trigger)
    CHECK(sm.lua()["girada"].get<int>() == 0);     // ahora la caja es larga en X
    CHECK(sm.lua()["conOpts"].get<int>() == 3);    // tabla en 3ª posición = options
    CHECK(sm.lua()["rotYOpts"].get<int>() == 3);
    CHECK(sm.lua()["nMalos"].get<int>() == 0);     // argumentos inválidos -> tabla vacía

    pm.setTrigger(medio->getSphereCollider(), false);
}

// Fuera de Play (sin PhysicsManager) las tres consultas devuelven lo mismo que
// dentro pero vacío: nil el sweep, tabla vacía los overlaps. Nunca excepción.
static void test_sweep_y_overlaps_sin_fisica(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    sm.setPhysicsManager(nullptr);
    sm.lua().script(
        "sweep  = Physics.SphereCast(Vec3(0,2,0), Vec3(0,0,1), 1, 100)\n"
        "nEsf   = #Physics.OverlapSphere(Vec3(0,2,10), 5)\n"
        "nCaja  = #Physics.OverlapBox(Vec3(0,2,10), Vec3(5,5,5))\n");
    sm.setPhysicsManager(&pm);

    CHECK(luaIsNil(sm, "sweep"));
    CHECK(sm.lua()["nEsf"].get<int>() == 0);
    CHECK(sm.lua()["nCaja"].get<int>() == 0);
}

// Rigidbody.constraints es un BITMASK, no un float: se compone desde Lua con
// el OR bit a bit de 5.4 sobre la tabla RigidbodyConstraints. El test escribe
// un valor DISTINTO del inicial (RB_None), lo lee de vuelta por la misma
// propiedad y remata midiendo la simulación: con Freeze-Y puesto, el cuerpo no
// cae aunque tenga gravedad. Sin el setter, el bitmask no llega al actor y la
// última comprobación se pone roja.
static void test_constraints_desde_lua(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Cuerpo");
    auto rb = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    go->setRigidbody(rb);
    sm.rebuildAliveSet();

    CHECK(rb->getConstraints() == RB_None); // punto de partida, no lo que se prueba

    sm.lua()["e"] = LuaEntity{ go, &sm };
    sm.lua().script(
        "rb = e:GetComponent('Rigidbody')\n"
        "rb.constraints = RigidbodyConstraints.FreezePositionY | RigidbodyConstraints.FreezeRotationX\n"
        "leido = rb.constraints\n");

    const uint32_t esperado = RB_FreezePositionY | RB_FreezeRotationX;
    CHECK(sm.lua()["leido"].get<uint32_t>() == esperado);
    CHECK(rb->getConstraints() == esperado);

    // Con gravedad, el eje congelado se queda quieto.
    const float y0 = col->getWorldTransform()[3].y;
    for (int i = 0; i < 30; ++i) pm.stepSimulation(1.0f / 60.0f);
    CHECK(std::fabs(col->getWorldTransform()[3].y - y0) < 0.001f);

    // Bits que no existen en Rigidbody.h: se recortan contra la máscara en vez
    // de lanzar (un OR de más no debe tumbar el script).
    sm.lua().script("rb.constraints = 0xFFFFFFFF\n");
    const uint32_t todos = RB_FreezePositionX | RB_FreezePositionY | RB_FreezePositionZ |
                           RB_FreezeRotationX | RB_FreezeRotationY | RB_FreezeRotationZ;
    CHECK(rb->getConstraints() == todos);
}

// Rigidbody.ccd y Rigidbody.interpolate desde Lua. No basta con leer de vuelta
// lo escrito (eso lo daría un campo suelto que no llega a ningún sitio): el
// test mira el flag que ve PhysX en el actor para ccd, y la pose que devuelve
// getWorldTransform para interpolate. Independientes: encender una no toca la
// otra.
static void test_ccd_e_interpolate_desde_lua(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Cuerpo");
    auto rb = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    go->setRigidbody(rb);
    sm.rebuildAliveSet();

    auto actorTieneCcd = [&col]() {
        auto* actor = static_cast<physx::PxRigidActor*>(col->actorHandle());
        auto* dyn   = actor ? actor->is<physx::PxRigidDynamic>() : nullptr;
        return dyn && (dyn->getRigidBodyFlags() & physx::PxRigidBodyFlag::eENABLE_CCD);
    };

    // Punto de partida: los dos apagados, en C++ y en el actor.
    CHECK(!rb->getCcd());
    CHECK(!rb->getInterpolate());
    CHECK(!actorTieneCcd());

    sm.lua()["e"] = LuaEntity{ go, &sm };
    sm.lua().script(
        "rb = e:GetComponent('Rigidbody')\n"
        "ccd0 = rb.ccd\n"
        "interp0 = rb.interpolate\n"
        "rb.ccd = true\n");

    CHECK(sm.lua()["ccd0"].get<bool>() == false);      // el getter lee el estado real
    CHECK(sm.lua()["interp0"].get<bool>() == false);
    CHECK(rb->getCcd());
    CHECK(actorTieneCcd());                            // y el setter llega hasta PhysX
    CHECK(!rb->getInterpolate());                      // independientes: ccd no encendió la otra

    // Ahora interpolate: se comprueba MIRANDO LA POSE, no el getter. Con el
    // acumulador a 0 tras un paso exacto, alpha vale 0 y lo visible es la pose
    // PREVIA al sub-step, no la del actor.
    sm.lua().script("rb.interpolate = true\nleido = rb.interpolate\n");
    CHECK(sm.lua()["leido"].get<bool>() == true);
    CHECK(rb->getInterpolate());

    auto* actor = static_cast<physx::PxRigidActor*>(col->actorHandle());
    pm.stepSimulation(1000.0f);                        // vacía el acumulador
    // La referencia se lee del ACTOR, no de getWorldTransform: con la
    // interpolación ya encendida, el getter devuelve la pose previa y compararse
    // contra sí mismo no probaría nada.
    const float yPartida = actor->getGlobalPose().p.y;
    pm.stepSimulation(pm.getFixedDeltaTime());
    const float yCrudo   = actor->getGlobalPose().p.y;
    const float yVisible = col->getWorldTransform()[3].y;
    CHECK(yCrudo < yPartida - 0.01f);                  // el actor cayó
    CHECK(std::fabs(yVisible - yPartida) < 1e-3f);     // lo visible sigue en la pose previa
    CHECK(std::fabs(yVisible - yCrudo) > 0.01f);       // o sea, NO es la pose cruda

    // Apagar desde Lua devuelve el camino de siempre.
    sm.lua().script("rb.ccd = false\nrb.interpolate = false\n");
    CHECK(!actorTieneCcd());
    CHECK(std::fabs(col->getWorldTransform()[3].y - actor->getGlobalPose().p.y) < 1e-4f);
}

// --- ForceMode desde Lua -----------------------------------------------------
//
// Monta un cuerpo sin gravedad con el Rigidbody expuesto en 'rb' y devuelve la
// velocidad en X tras un paso de la simulación del script dado. El collider se
// queda vivo dentro de la función (el actor PhysX lo posee él, no el
// Rigidbody), así que se mide antes de salir.
static float velocidadTrasScript(ScriptManager& sm, PhysicsManager& pm, const char* script)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Cuerpo");
    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    go->setRigidbody(rb);
    sm.rebuildAliveSet();

    sm.lua()["e"] = LuaEntity{ go, &sm };
    sm.lua().script(script);
    pm.stepSimulation(1.0f / 60.0f);
    return rb->getVelocity().x;
}

// Retrocompatibilidad: la llamada de TRES argumentos sigue funcionando y sigue
// significando ForceMode.Force. Y con el 4º argumento el resultado es otro:
// VelocityChange escribe la velocidad de golpe (v = F) en vez de integrarla
// durante el paso (v = F*dt/m), o sea 60× más con dt = 1/60.
static void test_force_mode_desde_lua(ScriptManager& sm, PhysicsManager& pm)
{
    const float dt = 1.0f / 60.0f;
    // Callback propio (el de otro test ya no es válido) y de paso comprueba que
    // el camino feliz no avisa de nada.
    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });

    float vTresArgs = velocidadTrasScript(sm, pm, "e:GetComponent('Rigidbody'):AddForce(100, 0, 0)");
    float vExplicito = velocidadTrasScript(sm, pm,
        "e:GetComponent('Rigidbody'):AddForce(100, 0, 0, ForceMode.Force)");
    float vVelCh = velocidadTrasScript(sm, pm,
        "e:GetComponent('Rigidbody'):AddForce(100, 0, 0, ForceMode.VelocityChange)");
    std::printf("  ForceMode Lua: 3 args -> %.4f | Force -> %.4f | VelocityChange -> %.4f\n",
                vTresArgs, vExplicito, vVelCh);

    CHECK(std::fabs(vTresArgs - 100.0f * dt) < 0.01f);   // igual que siempre
    CHECK(std::fabs(vTresArgs - vExplicito) < 1e-5f);    // 3 args == ForceMode.Force
    CHECK(std::fabs(vVelCh - 100.0f) < 0.01f);           // v de golpe
    CHECK(vVelCh > vTresArgs * 10.0f);                   // inequívocamente distintos
    CHECK(log.empty());
}

// Un modo fuera del rango [0,3] no lanza error de Lua: avisa por el Log y NO
// aplica la fuerza (mismo contrato que un NaN en x,y,z).
static void test_force_mode_fuera_de_rango(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Cuerpo");
    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    go->setRigidbody(rb);
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script("e:GetComponent('Rigidbody'):AddForce(1000, 0, 0, 99)");
    for (int i = 0; i < 10; ++i) pm.stepSimulation(1.0f / 60.0f);

    CHECK(nearlyEqual(rb->getVelocity().x, 0.0f));
    CHECK(logContains(log, "AddForce"));
    CHECK(logContains(log, "WARN"));
}

// Collider.isTrigger desde Lua. La lectura tiene que reflejar el cambio, pero
// eso solo no prueba nada: lo que demuestra que el setter pasó de verdad por
// PhysicsManager::setTrigger (flip de flags en PhysX) es que a partir de ahí
// un cuerpo dinámico ATRAVIESA el suelo en vez de posarse encima.
static void test_is_trigger_desde_lua(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* suelo = scene.addGameObject("Suelo");
    auto piso = pm.createBoxColliderComponent(glm::vec3(50.0f, 0.5f, 50.0f), glm::vec3(0.0f),
                                               glm::mat4(1.0f), /*dynamic=*/false);
    suelo->setBoxCollider(piso);
    sm.rebuildAliveSet();

    CHECK(piso->isTrigger() == false);

    sm.lua()["e"] = LuaEntity{ suelo, &sm };
    sm.lua().script(
        "bc = e:GetComponent('BoxCollider')\n"
        "antes = bc.isTrigger\n"
        "bc.isTrigger = true\n"
        "despues = bc.isTrigger\n");

    CHECK(sm.lua()["antes"].get<bool>() == false);
    CHECK(sm.lua()["despues"].get<bool>() == true);
    CHECK(piso->isTrigger() == true);

    // Caída desde y=5 sobre un suelo que ya es trigger: solape sin colisión.
    // Con el suelo sólido acabaría reposando en ~1,5.
    auto rb  = std::make_shared<Rigidbody>();
    auto caja = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                               glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 5.0f, 0.0f)),
                                               /*dynamic=*/true);
    pm.attachRigidbody(caja, rb);
    for (int i = 0; i < 120; ++i) pm.stepSimulation(1.0f / 60.0f);
    CHECK(caja->getWorldTransform()[3].y < -3.0f);
}

// --- Capas de colisión desde Lua --------------------------------------------

// collider.layer va y vuelve, y el número llega al Collider de verdad (no se
// queda en una copia del binding).
static void test_layer_desde_lua(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Caja");
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                             glm::mat4(1.0f), /*dynamic=*/false);
    go->setBoxCollider(col);
    sm.rebuildAliveSet();
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script(
        "bc = e:GetComponent('BoxCollider')\n"
        "antes = bc.layer\n"
        "bc.layer = 5\n"
        "despues = bc.layer\n");

    CHECK(sm.lua()["antes"].get<int>() == 0);
    CHECK(sm.lua()["despues"].get<int>() == 5);
    CHECK(col->getLayer() == 5);
}

// Índice fuera de [0,31]: error de Lua (no un clamp silencioso), y la capa se
// queda como estaba. Se prueban los dos extremos.
static void test_layer_fuera_de_rango_es_error(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Caja");
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                             glm::mat4(1.0f), /*dynamic=*/false);
    go->setBoxCollider(col);
    sm.rebuildAliveSet();
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script(
        "bc = e:GetComponent('BoxCollider')\n"
        "bc.layer = 3\n"
        // tostring: si el pcall NO falla el segundo valor es nil, y leerlo como
        // string desde C++ entra en panic de Lua y se lleva el proceso — el
        // fallo hay que verlo como un CHECK, no como un aborto.
        "okAlto, errAlto = pcall(function() bc.layer = 32 end)\n"
        "okBajo, errBajo = pcall(function() bc.layer = -1 end)\n"
        "errAlto = tostring(errAlto)\n");

    CHECK(sm.lua()["okAlto"].get<bool>() == false);
    CHECK(sm.lua()["okBajo"].get<bool>() == false);
    CHECK(sm.lua()["errAlto"].get<std::string>().find("fuera de rango") != std::string::npos);
    CHECK(col->getLayer() == 3); // ni el 32 ni el -1 han entrado
}

// La matriz desde Lua: apagar (6,7) se ve al leerla y llega al PhysicsManager;
// los índices inválidos también son error aquí.
static void test_matriz_de_capas_desde_lua(ScriptManager& sm, PhysicsManager& pm)
{
    sm.lua().script(
        "antes = Physics.GetLayerCollision(6, 7)\n"
        "Physics.SetLayerCollision(6, 7, false)\n"
        "despues = Physics.GetLayerCollision(6, 7)\n"
        "simetrico = Physics.GetLayerCollision(7, 6)\n"
        "okSet = pcall(Physics.SetLayerCollision, 6, 99, false)\n"
        "okGet = pcall(Physics.GetLayerCollision, -5, 0)\n");

    CHECK(sm.lua()["antes"].get<bool>() == true);
    CHECK(sm.lua()["despues"].get<bool>() == false);
    CHECK(sm.lua()["simetrico"].get<bool>() == false); // la matriz es simétrica
    CHECK(sm.lua()["okSet"].get<bool>() == false);
    CHECK(sm.lua()["okGet"].get<bool>() == false);
    CHECK(pm.getLayerCollision(6, 7) == false);        // ha llegado al manager

    sm.lua().script("Physics.SetLayerCollision(6, 7, true)\n");
    CHECK(pm.getLayerCollision(6, 7) == true);
}

// --- Persistencia de las capas en el project.json ----------------------------

// Escribe un project.json de mentira en 'dir' con el contenido dado.
static void escribirProjectJson(const std::filesystem::path& dir, const std::string& contenido)
{
    std::ofstream out(dir / "project.json", std::ios::binary | std::ios::trunc);
    out << contenido;
}

// Guardar -> recargar devuelve nombres y matriz IDÉNTICOS. Es el round-trip
// entero, pasando por disco.
static void test_capas_round_trip_en_project_json()
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "dt_capas_round_trip";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    ProjectContext::ViewSettings s;
    s.layerActive    = 8;            // 8 capas creadas de las 32 posibles
    s.layerNames[3]  = "Enemigos";
    s.layerNames[31] = "UI";
    s.layerMasks[3] &= ~(1u << 7);   // (3,7) apagada, en las dos mitades
    s.layerMasks[7] &= ~(1u << 3);
    s.layerMasks[0] &= ~(1u << 0);   // una capa que ni consigo colisiona

    CHECK(ProjectContext::writeSettings(dir, s));

    const ProjectContext::ViewSettings leido =
        ProjectContext::readSettings(dir, ProjectContext::ViewSettings{});

    CHECK(!leido.loadFailed);
    CHECK(leido.layerNames == s.layerNames);
    CHECK(leido.layerMasks == s.layerMasks);
    CHECK(leido.layerActive == 8);
    CHECK(leido.layerNames[3] == "Enemigos");
    CHECK((leido.layerMasks[3] & (1u << 7)) == 0u);

    std::filesystem::remove_all(dir, ec);
}

// JSON ausente, ilegible o con las capas de otro tipo: DEFAULTS, nunca una
// excepción ni una matriz a medias.
static void test_capas_json_ausente_o_corrupto_da_defaults()
{
    const ProjectContext::ViewSettings def;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "dt_capas_corrupto";
    std::error_code ec;

    // 1) Sin project.json.
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    ProjectContext::ViewSettings s = ProjectContext::readSettings(dir, ProjectContext::ViewSettings{});
    CHECK(s.layerMasks == def.layerMasks);
    CHECK(s.layerNames == def.layerNames);

    // 2) JSON truncado.
    escribirProjectJson(dir, "{ \"settings\": { \"layerNames\": ");
    s = ProjectContext::readSettings(dir, ProjectContext::ViewSettings{});
    CHECK(s.loadFailed);
    CHECK(s.layerMasks == def.layerMasks);
    CHECK(s.layerNames == def.layerNames);

    // 3) JSON válido pero con las capas de otro tipo, o con basura dentro del
    //    array: cada hueco malo se cae a su default, el bueno sí entra.
    escribirProjectJson(dir,
        "{ \"name\": \"x\", \"settings\": {"
        " \"layerNames\": \"no soy un array\","
        " \"layerCollision\": [ -7, \"tampoco\", 8, 4294967296 ],"
        " \"layerActive\": 0 } }");
    s = ProjectContext::readSettings(dir, ProjectContext::ViewSettings{});
    CHECK(!s.loadFailed);
    CHECK(s.layerActive == 1);                   // el 0 se clampea: la Default existe siempre
    CHECK(s.layerNames == def.layerNames);
    CHECK(s.layerMasks[0] == def.layerMasks[0]); // negativo: ignorado
    CHECK(s.layerMasks[1] == def.layerMasks[1]); // string: ignorado
    CHECK(s.layerMasks[2] == 8u);                // el único válido
    CHECK(s.layerMasks[3] == def.layerMasks[3]); // > 32 bits: ignorado
    CHECK(s.layerMasks[4] == def.layerMasks[4]); // fuera del array: default

    std::filesystem::remove_all(dir, ec);
}

// Los bindings de audio que faltaban: distancias, playOnAwake, path y el estado
// de la voz. Antes, un script tenía diez métodos y ninguno de estos, así que
// min/maxDistance solo se podían tocar desde el Inspector aunque el componente
// los soportara desde el principio.
//
// Se ejercitan CON valores no neutros y leyéndolos de vuelta por Lua: un
// binding que existiera pero llamara al setter equivocado (un clásico entre
// min y max) pasaría cualquier test que solo comprobara que no lanza.
static void test_audio_bindings_nuevos(ScriptManager& sm, AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_audio_bindings_nuevos (FMOD no disponible)\n");
        return;
    }
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Altavoz");
    auto clip = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, /*loop=*/false);
    if (!clip)
    {
        std::printf("SKIP test_audio_bindings_nuevos (no se pudo crear el clip)\n");
        return;
    }
    go->setAudioClip(clip);
    sm.rebuildAliveSet();
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script(R"(
        local c = e:GetComponent("AudioClip")
        c:SetMaxDistance(300)
        c:SetMinDistance(7.5)
        c:SetPlayOnAwake(true)
        leidoMin  = c:GetMinDistance()
        leidoMax  = c:GetMaxDistance()
        leidoAwake = c:GetPlayOnAwake()
        leidoPath = c:GetPath()
        sonando   = c:IsPlaying()
        pausado   = c:IsPaused()
    )");

    // Leídos por Lua Y comprobados en el componente: si el binding escribiera
    // en el sitio equivocado, uno de los dos lados lo delataría.
    CHECK(nearlyEqual(sm.lua()["leidoMin"].get<float>(), 7.5f));
    CHECK(nearlyEqual(sm.lua()["leidoMax"].get<float>(), 300.0f));
    CHECK(nearlyEqual(go->getAudioClip()->getMinDistance(), 7.5f));
    CHECK(nearlyEqual(go->getAudioClip()->getMaxDistance(), 300.0f));
    CHECK(sm.lua()["leidoAwake"].get<bool>() == true);
    CHECK(go->getAudioClip()->getPlayOnAwake() == true);
    CHECK(sm.lua()["leidoPath"].get<std::string>() == "assets/audio.mp3");
    // Sin haber llamado a Play: nada suena y nada está pausado.
    CHECK(sm.lua()["sonando"].get<bool>() == false);
    CHECK(sm.lua()["pausado"].get<bool>() == false);

    // NaN por los setters nuevos: mismo trato que SetVolume/SetPitch — se
    // rechaza, se avisa, y el valor anterior queda intacto.
    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua().script("e:GetComponent(\"AudioClip\"):SetMinDistance(0/0)");
    CHECK(nearlyEqual(go->getAudioClip()->getMinDistance(), 7.5f));
    CHECK(logContains(log, "SetMinDistance"));
    CHECK(logContains(log, "WARN"));
    sm.setLogCallback(nullptr);
}

// Buses desde Lua: por nombre en las dos direcciones, y un nombre desconocido
// avisa sin cambiar nada (en vez de caer a un bus arbitrario, que es lo que
// haría un cast desde entero).
static void test_audio_bus_desde_lua(ScriptManager& sm, AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_audio_bus_desde_lua (FMOD no disponible)\n");
        return;
    }
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Altavoz");
    auto clip = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!clip)
    {
        std::printf("SKIP test_audio_bus_desde_lua (no se pudo crear el clip)\n");
        return;
    }
    go->setAudioClip(clip);
    sm.rebuildAliveSet();
    sm.lua()["e"] = LuaEntity{ go, &sm };

    sm.lua().script(R"(
        local c = e:GetComponent("AudioClip")
        busInicial = c:GetBus()
        c:SetBus("music")
        busTrasSet = c:GetBus()
    )");
    CHECK(sm.lua()["busInicial"].get<std::string>() == "sfx");
    CHECK(sm.lua()["busTrasSet"].get<std::string>() == "music");
    CHECK(go->getAudioClip()->getBus() == AudioBus::Music);

    // Nombre inventado: avisa y CONSERVA el anterior.
    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua().script("e:GetComponent(\"AudioClip\"):SetBus(\"reverb\")");
    CHECK(go->getAudioClip()->getBus() == AudioBus::Music);
    CHECK(logContains(log, "SetBus"));
    CHECK(logContains(log, "WARN"));

    // Volúmenes globales por la tabla Audio. Se leen de vuelta por Lua Y del
    // manager: si el binding escribiera en el bus equivocado, uno de los dos
    // lados lo delataría.
    log.clear();
    sm.lua().script(R"(
        Audio.SetBusVolume("music", 0.25)
        Audio.SetBusVolume("sfx", 0.75)
        volMusic = Audio.GetBusVolume("music")
        volSfx   = Audio.GetBusVolume("sfx")
    )");
    CHECK(nearlyEqual(sm.lua()["volMusic"].get<float>(), 0.25f));
    CHECK(nearlyEqual(sm.lua()["volSfx"].get<float>(), 0.75f));
    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Music), 0.25f));
    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Sfx), 0.75f));

    // Fuera de rango se clampa, y un NaN se rechaza avisando: sin esto, el bus
    // se quedaría inutilizable el resto de la partida y no hay ningún fichero
    // donde se vea para depurarlo.
    sm.lua().script("Audio.SetBusVolume(\"music\", 5.0)");
    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Music), 1.0f));
    sm.lua().script("Audio.SetBusVolume(\"sfx\", 0/0)");
    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Sfx), 0.75f));
    CHECK(logContains(log, "WARN"));
    sm.setLogCallback(nullptr);

    // Neutros otra vez: este manager lo comparten todos los tests del binario.
    am.setBusVolume(AudioBus::Master, 1.0f);
    am.setBusVolume(AudioBus::Music,  1.0f);
    am.setBusVolume(AudioBus::Sfx,    1.0f);
}

int main()
{
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    am.init();

    // Carpeta inexistente a propósito: init() registra los bindings de Lua
    // igual (solo loguea el aviso de "carpeta no encontrada" y sigue, ver
    // ScriptManager::init) — estos tests no cargan ningún .lua de disco.
    ScriptManager sm;
    sm.init("__scripting_tests_sin_carpeta_de_scripts__");
    sm.setPhysicsManager(&pm);
    sm.setAudioManager(&am);

    test_set_position_rejects_nan(sm);
    test_set_position_applies_finite_value(sm);
    test_set_radius_rejects_nan(sm, pm);
    test_set_radius_applies_finite_value(sm, pm);
    test_add_force_rejects_nan(sm, pm);
    test_add_force_applies_finite_value(sm, pm);
    test_dead_entity_wins_over_nan(sm);
    test_lua_syntax_check_detects_error();
    test_syntax_error_line_is_out_of_document();
    test_ui_lua_escribe_todos_los_campos(sm);
    test_ui_getter_nil_add_y_remove(sm);
    test_ui_layout_desde_lua(sm);
    test_ui_panel_desde_lua(sm);
    test_ui_image_desde_lua(sm);
    test_ui_slider_desde_lua(sm);
    test_ui_slider_callback_desde_lua(sm);
    test_ui_checkbox_desde_lua(sm);
    test_ui_toggle_desde_lua(sm);
    test_ui_scrollbar_desde_lua(sm);
    test_audio_bindings_nuevos(sm, am);
    test_audio_bus_desde_lua(sm, am);
    test_ui_input_field_desde_lua(sm);
    test_ui_input_field_callback_desde_lua(sm);
    test_ui_dropdown_desde_lua(sm);
    test_ui_scroll_view_desde_lua(sm);
    test_ui_valor_de_lua_llega_al_nodo(sm);
    test_ui_click_sobrevive_a_la_reconstruccion(sm);
    test_ui_callback_no_invoca_estado_viejo(sm);
    test_ui_error_en_callback_no_tumba_el_tick(sm);
    test_ui_cuatro_componentes_en_el_mismo_objeto(sm);

    test_raycast_campos_del_impacto(sm, pm);
    test_raycast_normaliza_la_direccion(sm, pm);
    test_raycast_direccion_cero(sm, pm);
    test_raycast_max_distance(sm, pm);
    test_raycast_hit_triggers(sm, pm);
    test_raycast_filtro_static_dynamic(sm, pm);
    test_raycast_ignore(sm, pm);
    test_raycast_tipos_invalidos_no_tumban_el_script(sm, pm);
    test_raycast_sin_fisica(sm, pm);
    test_raycast_actor_sin_gameobject(sm, pm);
    test_raycast_hit_booleano(sm, pm);
    test_raycast_all_ordenado_por_distancia(sm, pm);
    test_raycast_all_misma_forma_que_raycast(sm, pm);
    test_raycast_all_sin_impactos_devuelve_tabla_vacia(sm, pm);
    test_raycast_all_filtros(sm, pm);
    test_raycast_all_no_altera_raycast(sm, pm);
    test_sphere_cast_usa_el_radio(sm, pm);
    test_sphere_cast_pasa_de_largo(sm, pm);
    test_overlap_sphere_cero_uno_n(sm, pm);
    test_overlap_sphere_filtros(sm, pm);
    test_overlap_box_rotacion_y_options(sm, pm);
    test_sweep_y_overlaps_sin_fisica(sm, pm);

    test_constraints_desde_lua(sm, pm);
    test_ccd_e_interpolate_desde_lua(sm, pm);
    test_force_mode_desde_lua(sm, pm);
    test_force_mode_fuera_de_rango(sm, pm);
    test_is_trigger_desde_lua(sm, pm);
    test_layer_desde_lua(sm, pm);
    test_layer_fuera_de_rango_es_error(sm, pm);
    test_matriz_de_capas_desde_lua(sm, pm);
    test_capas_round_trip_en_project_json();
    test_capas_json_ausente_o_corrupto_da_defaults();

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL SCRIPTING TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
