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
#include "DonTopo/UI/CanvasComponent.h"
#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/TextComponent.h"
#include "DonTopo/UI/LayoutComponent.h"
#include "DonTopo/UI/ProgressBarComponent.h"
#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiSpriteBatch.h"
#include <TextEditor.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
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

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL SCRIPTING TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
