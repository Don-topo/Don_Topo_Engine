// Headless tests of ScriptBindings: the ensureFinite guard (NaN/Inf from Lua,
// see ScriptBindings.cpp) and its "silence + warning" contract. Plain main +
// asserts, no framework, same pattern as camera_tests.cpp/audio_tests.cpp/
// physics_tests.cpp.
//
// Before this file, no test exercised ensureFinite (131 lines changed in
// ScriptBindings.cpp): it could break silently without any red exe giving it away.
//
// PhysX only supports ONE PxFoundation per process: a single
// PhysicsManager (and AudioManager) is shared among all the tests, created in main() and
// passed by reference, never one per test (see physics_tests.cpp).
//
// ScriptManager is exercised headless for real: init() with a folder that does not
// exist registers the Lua bindings all the same (it only logs "folder not
// found" and carries on, see ScriptManager::init); no .lua is needed
// on disk to call Transform/Collider/Rigidbody directly. Each test
// pushes an already resolved LuaEntity into a Lua global ("e") and runs REAL Lua
// against it (same mechanism that instantiateComponentWith uses to inject
// self.entity, only here without a ScriptComponent in between) and captures the
// Log in a std::vector<std::string> via setLogCallback.
#include "DonTopo/Scripting/ScriptManager.h"
#include "DonTopo/Scripting/ScriptBindings.h"
#include "DonTopo/Scripting/LuaSyntaxCheck.h"
#include "DonTopo/Scripting/LuaApiReference.h"
#include "DonTopo/Scripting/ScriptComponent.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Editor/ProjectContext.h"
#include "DonTopo/Editor/Command.h"
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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>
#include "DonTopo/Audio/AudioClipComponent.h"

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }

// true if any logged line contains needle (e.g. the method name or
// "WARN").
static bool logContains(const std::vector<std::string>& log, const std::string& needle)
{
    for (const auto& l : log)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

// SetPosition with a Vec3 that carries a NaN component (0/0 in Lua): the
// position does NOT change (the earlier one is kept) and the Log receives a warning that
// names the method. It exercises the guard exactly as a real script sees it, which is
// exactly what the review pointed out as uncovered.
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

// CONTROL CASE: the SAME setter with a truly finite Vec3 DOES change the
// position and leaves NO warning in the Log. Without this test, an ensureFinite
// that always returned false would pass the test above all the same (position
// unchanged "because it never applies anything" instead of "because the value was NaN").
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

// Case of a LOOSE FLOAT (not a Vec3): SphereCollider.SetRadius with NaN.
// Same contract as SetPosition: the radius does not change and the Log warns.
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

// Control: the same SetRadius with a finite value DOES change the radius and logs
// NOTHING.
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

// AddForce receives x,y,z LOOSE (not an already built Vec3): with a NaN among
// them, the force is NOT applied (the velocity stays at 0 after advancing the
// physics) and the Log receives a warning.
static void test_add_force_rejects_nan(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Cuerpo");
    auto rb = std::make_shared<Rigidbody>();
    rb->setUseGravity(false);
    // The PhysX actor lives in the collider (Rigidbody does not own it, see
    // Rigidbody.h); col has to stay alive while rb is used, same as
    // in physics_tests.cpp.
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

// Control: the same AddForce with finite x,y,z DOES move the body (non-zero
// velocity after advancing the physics) and logs NOTHING. Without this test, an
// ensureFinite that always returned false would pass the test above all the same.
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

// The ORDER matters, and this test is the only thing that protects it: ensureFinite
// has to run AFTER the deref, not before.
//
// With the guard first, a script that touches an already destroyed entity while also
// passing a NaN received a "non-finite value" warning and a silent return,
// masking the use-after-destroy, which is the graver failure of the two. With
// the right order, deref throws a Lua error and the NaN is never even mentioned.
//
// Without this test, reverting that whole reordering leaves the 7 executables
// green: the happy path does not tell them apart, because when the entity is alive the
// two orders behave the same.
static void test_dead_entity_wins_over_nan(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Condenado");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    // The Transform is obtained WHILE the entity is alive and stored. If it were
    // requested after the deletion, the deref that GetTransform itself does
    // would throw before reaching SetPosition and this test would pass without exercising
    // anything; checked: that way it did not tell the correct order from the inverted one.
    sm.lua().script("t = e:GetTransform()");

    // Now yes: the Lua entity holds a GameObject that is no longer in the
    // scene, and the stored Transform points to it.
    scene.removeGameObject(go);
    sm.rebuildAliveSet();

    // pcall: it is expected to THROW. With a bare script() the error would propagate
    // and abort the test instead of checking it.
    sm.lua().script(
        "ok, err = pcall(function() t:SetPosition(Vec3.new(0/0, 1, 2)) end)");

    const bool ok = sm.lua()["ok"];
    CHECK(!ok);                              // it has to fail, not warn

    // And the reason must be the dead entity, not the NaN. The early return is
    // necessary: if the call did NOT throw, "err" is nil and reading it as a string
    // makes sol2 panic, which would abort the process instead of leaving a
    // readable FAIL.
    if (!ok)
    {
        const std::string err = sm.lua()["err"];
        CHECK(err.find("destroyed") != std::string::npos);
        CHECK(err.find("SetPosition") == std::string::npos);
    }
    CHECK(!logContains(log, "SetPosition"));
}

// checkLuaSyntax feeds the Script Editor error markers and had no
// coverage. What matters is that it returns the correct LINE: the
// marker is drawn by line number, so an off-by-one or a failure of the
// regex that parses the Lua message leaves the warning in the wrong place,
// or, if it detects nothing, without any marker at all.
static void test_lua_syntax_check_detects_error()
{
    // Valid script: no error.
    CHECK(!checkLuaSyntax("local x = 1\nprint(x)\n").has_value());

    // Missing 'end': the MOST COMMON case, and the one that uncovered that the markers were not
    // being seen. Lua reports it at <eof>, that is, ONE LINE PAST the end;
    // with 2 lines of text, it says line 3. The editor only draws markers for
    // lines that exist, so ScriptEditorPanel::saveTab has to clamp
    // the line to the document before passing it; if someone removes that clamp, the
    // marker goes back to being saved without ever being drawn.
    auto err = checkLuaSyntax("function f()\n  local y = 2\n");
    CHECK(err.has_value());
    if (!err) return;
    CHECK(err->first == 3);          // outside the text: 2 lines, error on the 3rd
    CHECK(!err->second.empty());

    // Error on a specific line in the middle: the reported line has to be
    // THAT one, not the first nor the last.
    auto mid = checkLuaSyntax("local a = 1\nlocal b = = 2\nlocal c = 3\n");
    CHECK(mid.has_value());
    if (!mid) return;
    CHECK(mid->first == 2);
}

// The real editor: SetText -> GetText -> checkLuaSyntax, which is exactly what
// saveTab does. It pins down the two traps that prevented seeing the marker, measured
// with the real TextEditor and not assumed:
//
//  a) GetText() returns ONE CHARACTER MORE than what was put in (the editor adds a
//     trailing newline), so Lua sees one extra line and places the <eof> outside the
//     document. The editor only draws markers for existing lines.
//  b) That last line is also EMPTY; clamping the marker there leaves it at the
//     end of the file, pointing at nothing useful.
static void test_syntax_error_line_is_out_of_document()
{
    // Script WITHOUT the final 'end', with a trailing newline like any real file.
    const std::string roto = "Rotator = {\n  speed = 45\n}\n\nfunction Rotator:Update(dt)\n  local t = 1\n";

    TextEditor ed;
    ed.SetText(roto);
    const std::string ida = ed.GetText();

    // (a) the round trip through the editor adds the trailing newline
    CHECK(ida.size() == roto.size() + 1);

    auto err = checkLuaSyntax(ida);
    CHECK(err.has_value());
    if (!err) return;

    // The error falls BEYOND the editor's last line: that marker would
    // never be drawn. It is the heart of the bug.
    CHECK(err->first > ed.GetTotalLines());

    // And the message names the line where what was left unclosed was opened
    // (here the 'function' on line 5), which is what markerLine uses to
    // put the band in a place that makes sense.
    CHECK(err->second.find("to close") != std::string::npos);
    CHECK(err->second.find("at line 5") != std::string::npos);
}

// ── UI from Lua ────────────────────────────────────────────────────────────
// Without a GPU: the fake loader returns nullptr, which is what the Renderer returns
// with an empty path. What is tested is the DATA that reaches the node, not the
// texture (same loader as camera_tests).
struct FakeUiLoader
{
    UiTextureAtlas* loadUiAtlas(const std::string&) { return nullptr; }
    UiFont*         loadUiFont(const std::string&)  { return nullptr; }
};

// TEST-ONLY adapter to the signature that syncUiWidgets had before the
// lists were grouped into UiWidgetLists. Same workaround as in camera_tests.cpp: the
// production API is ONE (the struct's) and this only avoids rewriting the
// calls that test something else. The arities do not overlap, so ADL has
// nothing to disambiguate.
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

// A complete click on p: one hover frame, one with the button down and another
// with the button up. The hit test needs rects, that is, a prior
// buildDrawData. The times are spaced out between clicks so as not to cross the
// double-click threshold by accident.
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

// A script writes ALL the fields of the four components with non-neutral values
// different from each other, and C++ reads them from the scene's component. The
// values are all different on purpose: a binding that always wrote to
// the same field, or that left a field unconnected, would pass a test made
// with zeros and repeated ones.
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

    // And the READ from Lua returns the same as what was written: without this a
    // getter wired to a different field than the setter would go unnoticed.
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

// The getter of a component that is not there returns nil, Add creates it, Remove
// removes it, and using the wrapper AFTER the remove gives a catchable Lua error instead
// of bringing down the process (the wrapper resolves by id on every access).
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

// The Layout from Lua: the same contract as the other three UI components
// (getter that gives nil without the component, Add, fields, Remove). The values are
// non-neutral and different from each other so that a field the binding does not write
// cannot pass for the default.
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

    // And the way back: what Lua READS is what is in the component.
    CHECK(sm.lua()["modoLeido"].get<int>() == (int)UiLayoutMode::Grid);

    auto r2 = sm.lua().safe_script(R"(
        e:RemoveLayout()
        trasRemove = (e:GetLayout() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasLayout());
}

// The Panel from Lua: same contract as the other UI components (getter that
// gives nil without the component, Add, fields, Remove). Non-neutral values different
// from each other: a field the binding does not write cannot pass for the default.
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

    // And the way back: what Lua READS is what is in the component.
    CHECK(sm.lua()["spriteLeido"].get<std::string>() == "marco_dorado");

    auto r2 = sm.lua().safe_script(R"(
        e:RemovePanel()
        trasRemove = (e:GetPanel() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasPanel());
}

// The Image from Lua. Besides the rect, the core widget's OWN fields:
// mode, 9-slice borders, tile cap and the Filled block.
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


// The four interactive widgets of the second batch from Lua: same contract as
// the others (getter that gives nil without the component, Add, fields, Remove) plus the
// path that only they have: OnValueChanged, which is how a script finds out
// that the player has moved something.
//
// Non-neutral values different from each other, for the usual reason.
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

    // The way back: (30 - -10) / (90 - -10) = 0.4
    CHECK(nearlyEqual(sm.lua()["normalizado"].get<float>(), 0.4f));

    auto r2 = sm.lua().safe_script(R"(
        e:RemoveSlider()
        trasRemove = (e:GetSlider() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasSlider());
}

// OnValueChanged is what distinguishes these widgets from those that are only drawn:
// the script finds out that the player has moved something WITHOUT polling the value every
// frame. The callback is fired by the live node, so a sync and a
// buildDrawData are needed before touching anything.
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

    // Passing nil removes it: the next movement notifies nobody.
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

    // 7 steps = 6 segments: the stop closest to 0.3 is 2/6 = 0.3333...
    CHECK(nearlyEqual(sm.lua()["pegado"].get<float>(), 1.0f / 3.0f));

    auto r2 = sm.lua().safe_script(R"(
        e:RemoveScrollbar()
        trasRemove = (e:GetScrollbar() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasScrollbar());
}


// The three widgets of the third batch from Lua. The InputField is the one that brings something
// that none of the earlier ones had: it can be typed into, so here the complete path
// is also tested: canvas key -> component -> script.
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

    // Password: what is shown is eight hash marks, and the text is still whole.
    CHECK(sm.lua()["ensenado"].get<std::string>() == "########");

    auto r2 = sm.lua().safe_script(R"(
        e:RemoveInputField()
        trasRemove = (e:GetInputField() == nil)
    )", sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["trasRemove"].get<bool>());
    CHECK(!go->hasInputField());
}

// The whole path: a key comes in through the canvas, the node's handler puts it
// into the component and the script finds out through OnValueChanged. It is what the core's
// character channel exists to allow.
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

    // The options are read from Lua with a 1-based index, which is the natural one there.
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

    // The vertical axis is OFF: its travel is 0 even if the content is
    // taller than the view. The horizontal one is on: 613 - 311 = 302.
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

// What a script writes into the COMPONENT reaches the live node on the next
// syncUiWidgets. It is the reason the setters do not touch the node: the sync
// dumps it on its own.
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
    // The fill is a quarter of the width: the Lua value has reached the
    // rect that is drawn, not just the field.
    CHECK(nearlyEqual(cache.barFills[0]->size.x, 75.0f));
}

// The Lua callback fires ONCE per click and KEEPS firing after
// the sync rebuilds the canvas root (clearChildren destroys the node
// that held the handler). It is the trap test: the owner of the callback is the
// component and the sync reinstalls it.
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

    // Adding a button changes the set -> the sync calls clearChildren() and
    // builds NEW nodes. A handler hooked bare onto the node would die here.
    syncUiWidgets(dos, {}, {}, canvas, cache, loader);
    data.clear();
    canvas.buildDrawData(800, 480, data);
    clickEn(canvas, glm::vec2(20.0f, 20.0f), 10.0f);
    CHECK(sm.lua()["clicks"].get<int>() == 2);

    // Two consecutive clicks in the same spot: only one must count as a double.
    clickEn(canvas, glm::vec2(20.0f, 20.0f), 10.2f);
    CHECK(sm.lua()["clicks"].get<int>() == 3);
    CHECK(sm.lua()["dobles"].get<int>() == 1);

    // And the node's state goes back to the component: the mouse stayed on top.
    syncUiWidgets(dos, {}, {}, canvas, cache, loader);
    auto r2 = sm.lua().safe_script("estado = e:GetButton():GetState()",
                                   sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(sm.lua()["estado"].get<int>() == (int)UiButtonState::Hover);
}

// A callback registered by a script that is hot-reloaded is NOT called
// again (it points to the old class), and one that outlives its ScriptManager
// does not touch the dead lua_State either.
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

    // What the hot reload does when it finishes loading the .lua.
    sm.invalidateScriptCallbacks();
    clickEn(canvas, glm::vec2(20.0f, 20.0f), 10.0f);
    CHECK(sm.lua()["recargaClicks"].get<int>() == 1);   // muted, not re-fired

    // And now the hard case: the component outlives the ScriptManager that
    // registered the callback. If the handler stored a sol::protected_function,
    // it would already have crashed here on destroying it.
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
    // ScriptManager destroyed: the click cannot call anything.
    clickEn(canvas2, glm::vec2(20.0f, 20.0f), 1.0f);
    CHECK(go2->getButton()->callbacks.ptr->onClick != nullptr);   // still there, but muted
}

// An error inside a callback is recorded in the Log and does NOT prevent the
// other button's callback from running.
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

    clickEn(canvas, glm::vec2(20.0f, 20.0f), 1.0f);      // the broken one
    clickEn(canvas, glm::vec2(420.0f, 320.0f), 10.0f);   // the healthy one

    CHECK(logContains(log, "callback roto a proposito"));
    CHECK(logContains(log, "Button.OnClick"));
    CHECK(sm.lua()["sanoClicks"].get<int>() == 1);
}

// The four components on the SAME GameObject: each wrapper writes into its own
// and the sync builds the three nodes without mixing them (Button, Text and
// ProgressBar share the owner id).
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
// Physics.Raycast: the PxScene is shared by the whole file (a single
// PxFoundation per process, see the header): each test sets up its colliders in
// a local Scene and releases them on exit.
// ---------------------------------------------------------------------------

// Sphere of radius 1 centered at (0,2,10). The ray (0,2,0)->+Z hits it at 9
// units, at the point (0,2,9) and with normal (0,0,-1): the three fields with
// values different from each other and none neutral, so a hit filled with zeros would not
// pass. The owner is set by hand just as Scene.cpp does when deserializing.
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

// The direction is normalized inside: with (0,0,5) the distance is still 9
// (PhysX requires a unit dir; unnormalized it comes out scaled or just plain wrong).
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

// Direction of length 0 -> nil without touching PhysX.
static void test_raycast_direccion_cero(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    addDiana(scene, pm, "Diana", glm::vec3(0.0f, 2.0f, 10.0f));
    sm.rebuildAliveSet();

    sm.lua().script("hit = Physics.Raycast(Vec3(0,2,0), Vec3(0,0,0), 100)");
    CHECK(luaIsNil(sm, "hit"));
}

// maxDistance absent or <= 0 -> default 1000 (the hit at 9 gets in); a
// truly short maxDistance cuts it off.
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

// hitTriggers: by default an Is Trigger collider does NOT count as a hit (PhysX
// does leave it in scene queries, our prefilter discards it).
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

// static / dynamic: the target is a PxRigidStatic, so turning off 'static'
// makes it disappear; turning off only 'dynamic' it is still there; with both off, nil.
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

// ignore: the entity that fires does not hit itself, but ignoring another one does
// not remove the hit.
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

// An argument of the wrong type returns nil and warns, but does NOT bring down the
// script: the next line runs.
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

// Without a PhysicsManager (outside Play) -> nil, not an exception.
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

// Collider without an associated GameObject: entity nil, the other fields filled.
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

// RaycastHit: only the boolean, same filters.
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
// Physics.RaycastAll: same targets, but THREE in a row on the same ray.
// ---------------------------------------------------------------------------

// Three spheres at z=10/20/30: the ray (0,2,0)->+Z hits them at 9, 19 and 29. They are
// created in REVERSE order (the farthest first) on purpose: PhysX delivers the
// touches in spatial sweep order, not by distance, so an unsorted
// RaycastAll has every chance of returning them backwards.
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

    // Strict ascending distance order + the entity that hits each one.
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

// Each element carries EXACTLY the same fields (and values) that
// Physics.Raycast would return for that hit: they are compared against each other, not against
// constants, so the day Raycast changes shape this test calls it out.
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

    // And no extra field: the hit table has exactly 4 keys.
    sm.lua().script(
        "claves = 0\n"
        "for k, v in pairs(todos[1]) do claves = claves + 1 end\n");
    CHECK(sm.lua()["claves"].get<int>() == 4);
}

// Without hits, without physics and with invalid arguments: ALWAYS a table (empty),
// never nil. A nil here would break the caller's ipairs.
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

    // Outside Play (without a PhysicsManager) it does not come out nil either.
    sm.setPhysicsManager(nullptr);
    sm.lua().script(
        "fuera = Physics.RaycastAll(Vec3(0,2,0), Vec3(0,0,1), 100)\n"
        "fueraOk = (type(fuera) == 'table') and (#fuera == 0)\n");
    sm.setPhysicsManager(&pm);
    CHECK(sm.lua()["fueraOk"].get<bool>() == true);
}

// The options filters apply the same as in Raycast: the trigger in the middle only
// shows up with hitTriggers, and ignore removes exactly that entity from the list.
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

    CHECK(sm.lua()["porDefecto"].get<int>() == 2);   // the trigger neither blocks nor counts
    CHECK(sm.lua()["conTriggers"].get<int>() == 3);
    CHECK(sm.lua()["sinStatic"].get<int>() == 0);
    CHECK(sm.lua()["nIgnorando"].get<int>() == 1);   // only Lejos remains
    if (sm.lua()["nIgnorando"].get<int>() == 1)
    {
        sol::table ign = sm.lua()["ignorando"];
        CHECK(nearlyEqual(ign[1]["distance"].get<float>(), 29.0f));
    }

    pm.setTrigger(medio->getSphereCollider(), false);
}

// Physics.Raycast still stops at the FIRST hit: RaycastAll cannot have
// passed on the eNO_BLOCK to it (with it, 'block' is left unwritten and the
// distance would come out garbage or the hit directly nil).
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
// Physics.SphereCast / OverlapSphere / OverlapBox: same targets (spheres of
// radius 1) and the same 'options' filters as the ray.
// ---------------------------------------------------------------------------

// Returns the Entity at out[i] (1-indexed) of an overlap's array, or
// nullptr if it is not a live Entity.
static GameObject* overlapAt(ScriptManager& sm, const char* name, int i)
{
    sol::optional<sol::table> t = sm.lua()[name];
    if (!t) return nullptr;
    sol::object o = (*t)[i];
    if (!o.valid() || !o.is<LuaEntity>()) return nullptr;
    return o.as<LuaEntity>().go;
}

// The sweep is the ray WITH THICKNESS: from (0,3.5,0) the ray passes 0.5 above
// the target (center y=2, radius 1) and does not hit it, but sweeping a sphere of
// radius 0.8 does. With the radius ignored (or at 0) the 'gordo' CHECK turns red.
// By the way it pins down the shape of the table: the same four fields as Raycast.
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

    CHECK(luaIsNil(sm, "rayo"));   // the zero-thickness ray passes by
    CHECK(luaIsNil(sm, "fino"));   // and a sphere that is too thin, too
    CHECK(!luaIsNil(sm, "gordo")); // but the fat one reaches

    sol::optional<sol::table> recto = sm.lua()["recto"];
    CHECK(recto.has_value());
    if (!recto) return;
    // Center at 10, target radius 1, sweep radius 0.5 -> contact at 8.5.
    CHECK(nearlyEqual((*recto)["distance"].get<float>(), 8.5f));
    glm::vec3 p = (*recto)["point"].get<glm::vec3>();
    glm::vec3 n = (*recto)["normal"].get<glm::vec3>();
    CHECK(nearlyEqual(p.z, 9.0f));                        // point on the target
    CHECK(nearlyEqual(n.x, 0.0f) && nearlyEqual(n.z, -1.0f));
    CHECK((*recto)["entity"].get<LuaEntity>().go == go);
}

// Sweeps that do not hit: the other way around, short because of maxDistance, and invalid radius
// (0 or negative -> nil with a warning, never invalid geometry to PhysX).
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

// 0, 1 and N overlaps with the same scene, changing only the radius. The N case fails
// if eNO_BLOCK is missing (the query would close on the first one) and the 0 case pins down
// that an empty table comes out, not nil.
static void test_overlap_sphere_cero_uno_n(ScriptManager& sm, PhysicsManager& pm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* cerca = addDiana(scene, pm, "Cerca", glm::vec3(0.0f, 2.0f, 10.0f));
    addDiana(scene, pm, "Medio", glm::vec3(0.0f, 2.0f, 20.0f));
    addDiana(scene, pm, "Lejos", glm::vec3(0.0f, 2.0f, 30.0f));
    // Fourth target INSIDE the radius of 'tres' but without a GameObject behind it: there are
    // four overlaps and only three entities to return, so 'nTres' == 3
    // pins down that orphan actors are omitted instead of sneaking a nil (or
    // blowing up) into the array.
    addDiana(scene, pm, "Anonima", glm::vec3(0.0f, 2.0f, 21.0f), /*withOwner=*/false);
    // And 'Cerca' gets a SECOND collider in the same spot: two shapes that
    // overlap, a single GameObject. 'nUno' == 1 pins down that the entity does not come out
    // duplicated in the array.
    auto extra = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f, 2.0f, 10.0f),
                                                glm::mat4(1.0f), /*dynamic=*/false);
    extra->setOwner(cerca);
    cerca->setBoxCollider(extra);
    sm.rebuildAliveSet();

    sm.lua().script(
        "vacio  = Physics.OverlapSphere(Vec3(0,500,0), 2)\n"
        "uno    = Physics.OverlapSphere(Vec3(0,2,10), 2)\n"
        "tres   = Physics.OverlapSphere(Vec3(0,2,20), 15)\n"
        // Without a radius: argument absent, empty table and warning (and NOT a crash when
        // looking at the type of a sol::object without a lua_State).
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

// The same filters as the ray: the trigger only shows up with hitTriggers, and
// 'ignore' removes exactly its GameObject from the list.
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

    CHECK(sm.lua()["nPorDefecto"].get<int>() == 2);   // the trigger does not count
    CHECK(sm.lua()["nConTriggers"].get<int>() == 3);
    CHECK(sm.lua()["nSinStatic"].get<int>() == 0);
    CHECK(sm.lua()["nIgnorando"].get<int>() == 1);    // without trigger and without Cerca
    CHECK(overlapAt(sm, "ignorando", 1) == lejos);

    pm.setTrigger(medio->getSphereCollider(), false);
}

// The box is ORIENTED: long in Z it sees the three targets, rotated 90° about Y it sees
// none. And the third argument is disambiguated by type: a table is
// 'options', not a rotation.
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

    CHECK(sm.lua()["larga"].get<int>() == 2);      // Cerca and Lejos (Medio is a trigger)
    CHECK(sm.lua()["girada"].get<int>() == 0);     // now the box is long in X
    CHECK(sm.lua()["conOpts"].get<int>() == 3);    // table in 3rd position = options
    CHECK(sm.lua()["rotYOpts"].get<int>() == 3);
    CHECK(sm.lua()["nMalos"].get<int>() == 0);     // invalid arguments -> empty table

    pm.setTrigger(medio->getSphereCollider(), false);
}

// Outside Play (without a PhysicsManager) the three queries return the same as
// inside but empty: nil the sweep, empty table the overlaps. Never an exception.
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

// Rigidbody.constraints is a BITMASK, not a float: it is composed from Lua with
// the 5.4 bitwise OR on the RigidbodyConstraints table. The test writes
// a value DIFFERENT from the initial one (RB_None), reads it back through the same
// property and finishes by measuring the simulation: with Freeze-Y set, the body does not
// fall even though it has gravity. Without the setter, the bitmask does not reach the actor and the
// last check turns red.
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

    CHECK(rb->getConstraints() == RB_None); // starting point, not what is being tested

    sm.lua()["e"] = LuaEntity{ go, &sm };
    sm.lua().script(
        "rb = e:GetComponent('Rigidbody')\n"
        "rb.constraints = RigidbodyConstraints.FreezePositionY | RigidbodyConstraints.FreezeRotationX\n"
        "leido = rb.constraints\n");

    const uint32_t esperado = RB_FreezePositionY | RB_FreezeRotationX;
    CHECK(sm.lua()["leido"].get<uint32_t>() == esperado);
    CHECK(rb->getConstraints() == esperado);

    // With gravity, the frozen axis stays still.
    const float y0 = col->getWorldTransform()[3].y;
    for (int i = 0; i < 30; ++i) pm.stepSimulation(1.0f / 60.0f);
    CHECK(std::fabs(col->getWorldTransform()[3].y - y0) < 0.001f);

    // Bits that do not exist in Rigidbody.h: they are trimmed against the mask instead
    // of throwing (an extra OR must not bring down the script).
    sm.lua().script("rb.constraints = 0xFFFFFFFF\n");
    const uint32_t todos = RB_FreezePositionX | RB_FreezePositionY | RB_FreezePositionZ |
                           RB_FreezeRotationX | RB_FreezeRotationY | RB_FreezeRotationZ;
    CHECK(rb->getConstraints() == todos);
}

// Rigidbody.ccd and Rigidbody.interpolate from Lua. Reading back what was
// written is not enough (a loose field that reaches nowhere would give that): the
// test looks at the flag PhysX sees on the actor for ccd, and at the pose that
// getWorldTransform returns for interpolate. Independent: turning one on does not touch the
// other.
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

    // Starting point: both off, in C++ and on the actor.
    CHECK(!rb->getCcd());
    CHECK(!rb->getInterpolate());
    CHECK(!actorTieneCcd());

    sm.lua()["e"] = LuaEntity{ go, &sm };
    sm.lua().script(
        "rb = e:GetComponent('Rigidbody')\n"
        "ccd0 = rb.ccd\n"
        "interp0 = rb.interpolate\n"
        "rb.ccd = true\n");

    CHECK(sm.lua()["ccd0"].get<bool>() == false);      // the getter reads the real state
    CHECK(sm.lua()["interp0"].get<bool>() == false);
    CHECK(rb->getCcd());
    CHECK(actorTieneCcd());                            // and the setter reaches PhysX
    CHECK(!rb->getInterpolate());                      // independent: ccd did not turn on the other one

    // Now interpolate: it is checked LOOKING AT THE POSE, not the getter. With the
    // accumulator at 0 after an exact step, alpha is 0 and what is visible is the pose
    // PRIOR to the sub-step, not the actor's.
    sm.lua().script("rb.interpolate = true\nleido = rb.interpolate\n");
    CHECK(sm.lua()["leido"].get<bool>() == true);
    CHECK(rb->getInterpolate());

    auto* actor = static_cast<physx::PxRigidActor*>(col->actorHandle());
    pm.stepSimulation(1000.0f);                        // empties the accumulator
    // The reference is read from the ACTOR, not from getWorldTransform: with
    // interpolation already on, the getter returns the previous pose and comparing against
    // itself would prove nothing.
    const float yPartida = actor->getGlobalPose().p.y;
    pm.stepSimulation(pm.getFixedDeltaTime());
    const float yCrudo   = actor->getGlobalPose().p.y;
    const float yVisible = col->getWorldTransform()[3].y;
    CHECK(yCrudo < yPartida - 0.01f);                  // the actor fell
    CHECK(std::fabs(yVisible - yPartida) < 1e-3f);     // what is visible is still at the previous pose
    CHECK(std::fabs(yVisible - yCrudo) > 0.01f);       // that is, it is NOT the raw pose

    // Turning it off from Lua brings back the usual path.
    sm.lua().script("rb.ccd = false\nrb.interpolate = false\n");
    CHECK(!actorTieneCcd());
    CHECK(std::fabs(col->getWorldTransform()[3].y - actor->getGlobalPose().p.y) < 1e-4f);
}

// --- ForceMode from Lua -----------------------------------------------------
//
// Sets up a body without gravity with the Rigidbody exposed as 'rb' and returns the
// X velocity after one simulation step of the given script. The collider
// stays alive inside the function (the PhysX actor is owned by it, not by the
// Rigidbody), so it is measured before leaving.
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

// Backward compatibility: the THREE-argument call still works and still
// means ForceMode.Force. And with the 4th argument the result is another one:
// VelocityChange writes the velocity at once (v = F) instead of integrating it
// over the step (v = F*dt/m), that is, 60x more with dt = 1/60.
static void test_force_mode_desde_lua(ScriptManager& sm, PhysicsManager& pm)
{
    const float dt = 1.0f / 60.0f;
    // Own callback (the one from another test is no longer valid) and by the way it checks that
    // the happy path warns about nothing.
    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });

    float vTresArgs = velocidadTrasScript(sm, pm, "e:GetComponent('Rigidbody'):AddForce(100, 0, 0)");
    float vExplicito = velocidadTrasScript(sm, pm,
        "e:GetComponent('Rigidbody'):AddForce(100, 0, 0, ForceMode.Force)");
    float vVelCh = velocidadTrasScript(sm, pm,
        "e:GetComponent('Rigidbody'):AddForce(100, 0, 0, ForceMode.VelocityChange)");
    std::printf("  ForceMode Lua: 3 args -> %.4f | Force -> %.4f | VelocityChange -> %.4f\n",
                vTresArgs, vExplicito, vVelCh);

    CHECK(std::fabs(vTresArgs - 100.0f * dt) < 0.01f);   // same as always
    CHECK(std::fabs(vTresArgs - vExplicito) < 1e-5f);    // 3 args == ForceMode.Force
    CHECK(std::fabs(vVelCh - 100.0f) < 0.01f);           // v at once
    CHECK(vVelCh > vTresArgs * 10.0f);                   // unmistakably different
    CHECK(log.empty());
}

// A mode outside the range [0,3] does not raise a Lua error: it warns through the Log and does NOT
// apply the force (same contract as a NaN in x,y,z).
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

// Collider.isTrigger from Lua. The read has to reflect the change, but
// that alone proves nothing: what shows that the setter really went through
// PhysicsManager::setTrigger (flag flip in PhysX) is that from then on
// a dynamic body GOES THROUGH the floor instead of settling on top of it.
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

    // Fall from y=5 onto a floor that is already a trigger: overlap without collision.
    // With a solid floor it would end up resting at ~1.5.
    auto rb  = std::make_shared<Rigidbody>();
    auto caja = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f),
                                               glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 5.0f, 0.0f)),
                                               /*dynamic=*/true);
    pm.attachRigidbody(caja, rb);
    for (int i = 0; i < 120; ++i) pm.stepSimulation(1.0f / 60.0f);
    CHECK(caja->getWorldTransform()[3].y < -3.0f);
}

// --- Collision layers from Lua --------------------------------------------

// collider.layer goes and comes back, and the number really reaches the Collider (it does not
// stay in a copy in the binding).
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

// Index outside [0,31]: a Lua error (not a silent clamp), and the layer is
// left as it was. Both extremes are tested.
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
        // tostring: if the pcall does NOT fail the second value is nil, and reading it as a
        // string from C++ causes a Lua panic and takes the process with it; the
        // failure has to be seen as a CHECK, not as an abort.
        "okAlto, errAlto = pcall(function() bc.layer = 32 end)\n"
        "okBajo, errBajo = pcall(function() bc.layer = -1 end)\n"
        "errAlto = tostring(errAlto)\n");

    CHECK(sm.lua()["okAlto"].get<bool>() == false);
    CHECK(sm.lua()["okBajo"].get<bool>() == false);
    CHECK(sm.lua()["errAlto"].get<std::string>().find("out of range") != std::string::npos);
    CHECK(col->getLayer() == 3); // neither 32 nor -1 got in
}

// The matrix from Lua: turning off (6,7) is seen when reading it and reaches the PhysicsManager;
// invalid indices are an error here too.
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
    CHECK(sm.lua()["simetrico"].get<bool>() == false); // the matrix is symmetric
    CHECK(sm.lua()["okSet"].get<bool>() == false);
    CHECK(sm.lua()["okGet"].get<bool>() == false);
    CHECK(pm.getLayerCollision(6, 7) == false);        // it has reached the manager

    sm.lua().script("Physics.SetLayerCollision(6, 7, true)\n");
    CHECK(pm.getLayerCollision(6, 7) == true);
}

// --- Persistence of the layers in project.json ----------------------------

// Writes a fake project.json in 'dir' with the given content.
static void escribirProjectJson(const std::filesystem::path& dir, const std::string& contenido)
{
    std::ofstream out(dir / "project.json", std::ios::binary | std::ios::trunc);
    out << contenido;
}

// Save -> reload returns IDENTICAL names and matrix. It is the whole round-trip,
// going through disk.
static void test_capas_round_trip_en_project_json()
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "dt_capas_round_trip";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    ProjectContext::ViewSettings s;
    s.layerActive    = 8;            // 8 layers created out of the 32 possible
    s.layerNames[3]  = "Enemigos";
    s.layerNames[31] = "UI";
    s.layerMasks[3] &= ~(1u << 7);   // (3,7) off, in both halves
    s.layerMasks[7] &= ~(1u << 3);
    s.layerMasks[0] &= ~(1u << 0);   // a layer that does not even collide with itself

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

// JSON absent, unreadable or with the layers of another type: DEFAULTS, never an
// exception or a half matrix.
static void test_capas_json_ausente_o_corrupto_da_defaults()
{
    const ProjectContext::ViewSettings def;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "dt_capas_corrupto";
    std::error_code ec;

    // 1) Without project.json.
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    ProjectContext::ViewSettings s = ProjectContext::readSettings(dir, ProjectContext::ViewSettings{});
    CHECK(s.layerMasks == def.layerMasks);
    CHECK(s.layerNames == def.layerNames);

    // 2) Truncated JSON.
    escribirProjectJson(dir, "{ \"settings\": { \"layerNames\": ");
    s = ProjectContext::readSettings(dir, ProjectContext::ViewSettings{});
    CHECK(s.loadFailed);
    CHECK(s.layerMasks == def.layerMasks);
    CHECK(s.layerNames == def.layerNames);

    // 3) Valid JSON but with the layers of another type, or with garbage inside the
    //    array: each bad slot falls to its default, the good one does get in.
    escribirProjectJson(dir,
        "{ \"name\": \"x\", \"settings\": {"
        " \"layerNames\": \"no soy un array\","
        " \"layerCollision\": [ -7, \"tampoco\", 8, 4294967296 ],"
        " \"layerActive\": 0 } }");
    s = ProjectContext::readSettings(dir, ProjectContext::ViewSettings{});
    CHECK(!s.loadFailed);
    CHECK(s.layerActive == 1);                   // 0 is clamped: Default always exists
    CHECK(s.layerNames == def.layerNames);
    CHECK(s.layerMasks[0] == def.layerMasks[0]); // negative: ignored
    CHECK(s.layerMasks[1] == def.layerMasks[1]); // string: ignored
    CHECK(s.layerMasks[2] == 8u);                // the only valid one
    CHECK(s.layerMasks[3] == def.layerMasks[3]); // > 32 bits: ignored
    CHECK(s.layerMasks[4] == def.layerMasks[4]); // outside the array: default

    std::filesystem::remove_all(dir, ec);
}

// --- Persistence of panel visibility ---------------------------------

// The bug: the Rendering panel docked as a tab and disappeared when reopening
// the editor. Its POSITION was saved (imgui.ini had it, with DockId and
// everything); what was not saved was that it was OPEN, because "rendering" did not
// exist either in the Panel enum or in kPanelKeys. The View menu already called
// saveProjectSettings() when ticking it, so the saving half looked done.
//
// The WHOLE round-trip through disk is asserted, which is where it was lost, and
// for ALL the panels of the enum: the failure was one of omission, so a test that
// only looked at Rendering would fall short again with the next panel.
static void test_visibilidad_de_panel_round_trip()
{
    using VS = ProjectContext::ViewSettings;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "dt_paneles_round_trip";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    // Its key on disk, panel by panel. The list is HERE on purpose, and
    // repeated by hand: it is a second opinion on the one in ProjectContext.cpp, and
    // without it the test would be worthless.
    //
    // A round-trip cannot see the enum crossed with kPanelKeys, because writing
    // and reading use the SAME wrong map and the trip comes out fine anyway;
    // checked by sabotaging it: moving "rendering" to another place in kPanelKeys left
    // the test green. What has to be asserted is the index-name pair, and for that
    // ONE single panel is written and it is checked which key appears.
    const struct { int panel; const char* clave; } kEsperado[] = {
        {VS::PanelScene,          "scene"},
        {VS::PanelViewport,       "viewport"},
        {VS::PanelProperties,     "properties"},
        {VS::PanelLog,            "log"},
        {VS::PanelContentBrowser, "contentBrowser"},
        {VS::PanelScriptEditor,   "scriptEditor"},
        {VS::PanelAnimator,       "animator"},
        {VS::PanelPerformance,    "performance"},
        {VS::PanelRendering,      "rendering"},
        {VS::PanelInputActions,   "inputActions"},
    };
    CHECK(std::size(kEsperado) == static_cast<size_t>(VS::PanelCount));

    for (const auto& e : kEsperado)
    {
        // Only this panel has data; the other nine are not written.
        ProjectContext::ViewSettings s;
        s.panelOpen[e.panel] = true;
        CHECK(ProjectContext::writeSettings(dir, s));

        std::ifstream in(dir / "project.json");
        const std::string json((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());

        // Its own appears...
        if (json.find(std::string("\"") + e.clave + "\"") == std::string::npos)
            std::printf("FAIL: falta la clave '%s' del panel %d\n", e.clave, e.panel);
        CHECK(json.find(std::string("\"") + e.clave + "\"") != std::string::npos);

        // ...and NO other. This is what catches the crossing: with the enum and the
        // keys misaligned, writing panel N produces another one's key.
        for (const auto& otro : kEsperado)
        {
            if (otro.panel == e.panel) continue;
            const bool sobra = json.find(std::string("\"") + otro.clave + "\"") != std::string::npos;
            if (sobra)
                std::printf("FAIL: al escribir solo el panel %d ('%s') sale ademas '%s'\n",
                            e.panel, e.clave, otro.clave);
            CHECK(!sobra);
        }

        // And it comes back to the SAME enum index: the complete trip, not just the outbound one.
        const ProjectContext::ViewSettings leido =
            ProjectContext::readSettings(dir, ProjectContext::ViewSettings{});
        CHECK(!leido.loadFailed);
        for (int i = 0; i < VS::PanelCount; ++i)
            CHECK(leido.panelOpen[i].has_value() == (i == e.panel));
        CHECK(leido.panelOpen[e.panel] == true);
    }

    std::filesystem::remove_all(dir, ec);
}

// A panel WITHOUT saved data stays like this, which is not the same as
// closed. It matters because of what it cost: the panelOpen initializer was
// a list of nine hand-written -1s, so adding the tenth panel to the enum would
// have given it a 0 ("closed") instead of "no data", and it would have CLOSED the new panel
// in every project that already exists. With optional the new slot is born
// empty on its own.
static void test_panel_sin_dato_no_es_cerrado()
{
    using VS = ProjectContext::ViewSettings;
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "dt_paneles_sin_dato";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    // A project.json that only knows about one panel: the rest have no data.
    escribirProjectJson(dir,
        "{ \"name\": \"x\", \"settings\": { \"panels\": { \"log\": false } } }");

    ProjectContext::ViewSettings s = ProjectContext::readSettings(dir, ProjectContext::ViewSettings{});
    CHECK(!s.loadFailed);

    CHECK(s.panelOpen[VS::PanelLog].has_value());
    CHECK(s.panelOpen[VS::PanelLog] == false);           // data: closed
    CHECK(!s.panelOpen[VS::PanelRendering].has_value()); // no data: it is NOT closed
    CHECK(!s.panelOpen[VS::PanelScene].has_value());

    // And a freshly constructed ViewSettings carries no data for any of them.
    const ProjectContext::ViewSettings limpio;
    for (int i = 0; i < VS::PanelCount; ++i)
        CHECK(!limpio.panelOpen[i].has_value());

    // What has no data is not WRITTEN: the file must not lie by saying
    // "closed" about what nobody has decided. What was read is rewritten and only
    // "log" has to remain inside panels.
    CHECK(ProjectContext::writeSettings(dir, s));
    std::ifstream in(dir / "project.json");
    const std::string json((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    CHECK(json.find("\"log\"") != std::string::npos);
    CHECK(json.find("\"rendering\"") == std::string::npos);

    std::filesystem::remove_all(dir, ec);
}

// The audio bindings that were missing: distances, playOnAwake, path and the voice
// state. Before, a script had ten methods and none of these, so
// min/maxDistance could only be touched from the Inspector even though the component
// supported them from the start.
//
// They are exercised WITH non-neutral values and read back through Lua: a
// binding that existed but called the wrong setter (a classic between
// min and max) would pass any test that only checked that it does not throw.
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

    // Read through Lua AND checked on the component: if the binding wrote
    // to the wrong place, one of the two sides would give it away.
    CHECK(nearlyEqual(sm.lua()["leidoMin"].get<float>(), 7.5f));
    CHECK(nearlyEqual(sm.lua()["leidoMax"].get<float>(), 300.0f));
    CHECK(nearlyEqual(go->getAudioClip()->getMinDistance(), 7.5f));
    CHECK(nearlyEqual(go->getAudioClip()->getMaxDistance(), 300.0f));
    CHECK(sm.lua()["leidoAwake"].get<bool>() == true);
    CHECK(go->getAudioClip()->getPlayOnAwake() == true);
    CHECK(sm.lua()["leidoPath"].get<std::string>() == "assets/audio.mp3");
    // Without having called Play: nothing sounds and nothing is paused.
    CHECK(sm.lua()["sonando"].get<bool>() == false);
    CHECK(sm.lua()["pausado"].get<bool>() == false);

    // NaN through the new setters: same treatment as SetVolume/SetPitch; it is
    // rejected, a warning is given, and the previous value is left intact.
    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua().script("e:GetComponent(\"AudioClip\"):SetMinDistance(0/0)");
    CHECK(nearlyEqual(go->getAudioClip()->getMinDistance(), 7.5f));
    CHECK(logContains(log, "SetMinDistance"));
    CHECK(logContains(log, "WARN"));
    sm.setLogCallback(nullptr);
}

// Buses from Lua: by name in both directions, and an unknown name
// warns without changing anything (instead of falling to an arbitrary bus, which is what
// a cast from an integer would do).
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

    // Made-up name: warns and KEEPS the previous one.
    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua().script("e:GetComponent(\"AudioClip\"):SetBus(\"reverb\")");
    CHECK(go->getAudioClip()->getBus() == AudioBus::Music);
    CHECK(logContains(log, "SetBus"));
    CHECK(logContains(log, "WARN"));

    // Global volumes through the Audio table. They are read back through Lua AND from the
    // manager: if the binding wrote to the wrong bus, one of the two
    // sides would give it away.
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

    // Out of range it is clamped, and a NaN is rejected with a warning: without this, the bus
    // would be left unusable for the rest of the game and there is no file
    // where it can be seen to debug it.
    sm.lua().script("Audio.SetBusVolume(\"music\", 5.0)");
    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Music), 1.0f));
    sm.lua().script("Audio.SetBusVolume(\"sfx\", 0/0)");
    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Sfx), 0.75f));
    CHECK(logContains(log, "WARN"));
    sm.setLogCallback(nullptr);

    // Neutral again: this manager is shared by all the tests of the binary.
    am.setBusVolume(AudioBus::Master, 1.0f);
    am.setBusVolume(AudioBus::Music,  1.0f);
    am.setBusVolume(AudioBus::Sfx,    1.0f);
}

// ---------------------------------------------------------------------------
// Autocomplete: the filter lives in LuaApiReference (Core), not in the panel, and
// that is why it can be tested here without ImGui or a window.
// ---------------------------------------------------------------------------

// true if any suggestion has that exact symbol.
static bool tieneSimbolo(const std::vector<LuaApiMatch>& ms, const std::string& s)
{
    for (const auto& m : ms) if (m.symbol == s) return true;
    return false;
}

static const LuaApiMatch* buscaSimbolo(const std::vector<LuaApiMatch>& ms, const std::string& s)
{
    for (const auto& m : ms) if (m.symbol == s) return &m;
    return nullptr;
}

// Typing the whole type name: the usual behavior, which cannot have
// been broken by adding the per-member filter. The substitution covers the
// whole fragment (offset 0) and what is typed is the complete symbol.
static void test_autocomplete_prefijo_de_tipo()
{
    auto ms = luaApiMatches("Transform:Set");
    CHECK(tieneSimbolo(ms, "Transform:SetPosition"));
    CHECK(tieneSimbolo(ms, "Transform:SetRotation"));
    const LuaApiMatch* m = buscaSimbolo(ms, "Transform:SetPosition");
    CHECK(m != nullptr);
    if (m)
    {
        CHECK(m->replaceOffset == 0);
        CHECK(m->insert == "Transform:SetPosition");
    }
    // Case-insensitive.
    CHECK(tieneSimbolo(luaApiMatches("transform:set"), "Transform:SetPosition"));
}

// The case that before gave NOTHING: the receiver is a local variable, so
// the search is by the member's name. What is typed is ONLY the member and
// the substitution starts after the ':', so as not to leave "t:Transform:...".
static void test_autocomplete_por_miembro_conserva_receptor()
{
    auto ms = luaApiMatches("t:GetPosi");
    const LuaApiMatch* m = buscaSimbolo(ms, "Transform:GetPosition");
    CHECK(m != nullptr);
    if (m)
    {
        CHECK(m->insert == "GetPosition");
        CHECK(m->replaceOffset == 2);   // after "t:"
    }
    // With a longer receiver the offset still falls after the separator.
    auto ms2 = luaApiMatches("miRigidbody:AddFor");
    const LuaApiMatch* m2 = buscaSimbolo(ms2, "Rigidbody:AddForce");
    CHECK(m2 != nullptr);
    if (m2)
    {
        CHECK(m2->insert == "AddForce");
        CHECK(m2->replaceOffset == std::string("miRigidbody:").size());
    }
}

// '.' is a property and ':' is a method: suggesting a method where a
// dot was typed would give code that does not compile.
static void test_autocomplete_respeta_el_separador()
{
    auto conPunto = luaApiMatches("t.GetPosi");
    CHECK(!tieneSimbolo(conPunto, "Transform:GetPosition"));

    auto conDosPuntos = luaApiMatches("rb.mas");
    CHECK(tieneSimbolo(conDosPuntos, "Rigidbody.mass"));
    CHECK(!tieneSimbolo(luaApiMatches("rb:mas"), "Rigidbody.mass"));
}

// Order: first what starts with the whole fragment, then what was found
// by member. And an empty fragment opens nothing.
static void test_autocomplete_orden_y_vacio()
{
    CHECK(luaApiMatches("").empty());

    auto ms = luaApiMatches("Text.v");
    // "Text.vAlign" starts with the fragment; "InputField...".vAlign does not exist,
    // but the general order can be pinned down with a case with both ranges.
    CHECK(!ms.empty());
    if (!ms.empty()) CHECK(ms.front().symbol == "Text.vAlign");

    auto mixtas = luaApiMatches("Button:GetSi");
    CHECK(!mixtas.empty());
    if (!mixtas.empty()) CHECK(mixtas.front().symbol == "Button:GetSize");

    // The limit is respected.
    CHECK(luaApiMatches("Entity:", 3).size() == 3);
}

// Every new binding has to be in the table or autocomplete never
// shows it (project rule). It includes the five gaps the audit
// found already registered in ScriptBindings but absent from the list.
static void test_autocomplete_simbolos_nuevos_en_la_tabla()
{
    const std::vector<std::string> esperados = {
        // Pre-existing gaps
        "Vec3.x", "Entity:GetLayout", "Text.vAlign", "Button.textVAlign",
        "UiTextVAlign.Top",
        // New bindings
        "Time.deltaTime", "Time.fixedDeltaTime", "Time.time", "Time.frameCount",
        "Vec3:Length", "Vec3:Dot", "Vec3:Lerp",
        "Transform:GetForward", "Transform:LookAt", "Transform:SetWorldPosition",
        "Input.IsPadButtonDown", "PadButton.A", "PadAxis.LeftStickUp",
        "Light.type", "Light:SetColor", "LightType.Spot",
        "Camera.fov", "CameraProjection.Orthographic",
        "Entity.meshVisible", "Entity:AddLight", "Entity:GetCamera",
    };
    const std::vector<std::string>& tabla = luaApiSymbols();
    for (const std::string& s : esperados)
    {
        const bool esta = std::find(tabla.begin(), tabla.end(), s) != tabla.end();
        if (!esta) std::printf("FAIL: falta '%s' en luaApiSymbols()\n", s.c_str());
        CHECK(esta);
    }
}

// Signature and documentation: the annotated ones come out, and an unannotated symbol does not
// disappear from the popup; it just goes without help text.
static void test_autocomplete_firma_y_doc()
{
    std::string firma, doc;
    luaApiDoc("Transform:SetPosition", firma, doc);
    CHECK(firma == "(pos: Vec3)");
    CHECK(!doc.empty());

    luaApiDoc("Button:GetSize", firma, doc);
    CHECK(firma == "() -> width, height");   // generated in a loop for the 14 widgets

    // A Lua keyword has no signature, but it is still in the table.
    luaApiDoc("while", firma, doc);
    CHECK(firma.empty());
    CHECK(doc.empty());
    CHECK(tieneSimbolo(luaApiMatches("whil"), "while"));

    // The signature travels inside the suggestion, which is what the popup draws.
    // The vector goes into a local on purpose: buscaSimbolo returns a pointer
    // INSIDE it, and passing it the temporary directly would leave it dangling.
    const std::vector<LuaApiMatch> deVec3 = luaApiMatches("Vec3:Len");
    const LuaApiMatch* m = buscaSimbolo(deVec3, "Vec3:Length");
    CHECK(m != nullptr);
    if (m) CHECK(m->signature == "() -> number");
}

// ---------------------------------------------------------------------------
// New bindings
// ---------------------------------------------------------------------------

static void test_vec3_algebra_desde_lua(ScriptManager& sm)
{
    sm.lua().script(R"(
        local a = Vec3.new(3, 4, 0)
        local b = Vec3.new(0, 0, 2)
        lon   = a:Length()
        norm  = a:Normalized():Length()
        punto = a:Dot(Vec3.new(1, 0, 0))
        cruz  = a:Cross(b):Length()
        dist  = a:Distance(Vec3.new(3, 0, 0))
        lerpY = Vec3.new(0, 0, 0):Lerp(Vec3.new(0, 10, 0), 0.25).y
        divX  = (Vec3.new(8, 0, 0) / 2).x
        negX  = (-Vec3.new(5, 0, 0)).x
        iguales  = (Vec3.new(1, 2, 3) == Vec3.new(1, 2, 3))
        distintos = (Vec3.new(1, 2, 3) == Vec3.new(9, 2, 3))
        izq   = (2 * Vec3.new(3, 0, 0)).x
        cero  = Vec3.new(0, 0, 0):Normalized():Length()
    )");
    CHECK(nearlyEqual(sm.lua()["lon"], 5.0f));
    CHECK(nearlyEqual(sm.lua()["norm"], 1.0f));
    CHECK(nearlyEqual(sm.lua()["punto"], 3.0f));
    CHECK(nearlyEqual(sm.lua()["cruz"], 10.0f));   // |(3,4,0)x(0,0,2)| = 10
    CHECK(nearlyEqual(sm.lua()["dist"], 4.0f));
    CHECK(nearlyEqual(sm.lua()["lerpY"], 2.5f));
    CHECK(nearlyEqual(sm.lua()["divX"], 4.0f));
    CHECK(nearlyEqual(sm.lua()["negX"], -5.0f));
    CHECK(sm.lua()["iguales"] == true);
    CHECK(sm.lua()["distintos"] == false);
    // The scalar on the left: without the second __mul overload this was
    // a type error that brought down the script.
    CHECK(nearlyEqual(sm.lua()["izq"], 6.0f));
    // Normalizing the zero vector gives zero, not NaN.
    CHECK(nearlyEqual(sm.lua()["cero"], 0.0f));
}

// Time is filled from ScriptManager::update, not from the binding: without the
// call to tickTime the table stays at the registration's zeros.
static void test_time_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    sm.rebuildAliveSet();

    // fixedDeltaTime is constant and is already there before playing anything.
    CHECK(nearlyEqual(sm.lua()["Time"]["fixedDeltaTime"], ScriptManager::kFixedStep, 0.0001f));

    sm.onPlayStart();
    CHECK(nearlyEqual(sm.lua()["Time"]["time"], 0.0f));

    sm.update(0.5f);
    CHECK(nearlyEqual(sm.lua()["Time"]["deltaTime"], 0.5f));
    CHECK(nearlyEqual(sm.lua()["Time"]["time"], 0.5f));
    CHECK(static_cast<int>(sm.lua()["Time"]["frameCount"]) == 1);

    sm.update(0.25f);
    CHECK(nearlyEqual(sm.lua()["Time"]["deltaTime"], 0.25f));
    CHECK(nearlyEqual(sm.lua()["Time"]["time"], 0.75f));
    CHECK(static_cast<int>(sm.lua()["Time"]["frameCount"]) == 2);

    // A non-finite dt does not poison the accumulated value.
    sm.update(std::nanf(""));
    CHECK(nearlyEqual(sm.lua()["Time"]["time"], 0.75f));

    // Playing again restarts the clock.
    sm.onPlayStop();
    sm.onPlayStart();
    CHECK(nearlyEqual(sm.lua()["Time"]["time"], 0.0f));
    CHECK(static_cast<int>(sm.lua()["Time"]["frameCount"]) == 0);
    sm.onPlayStop();
}

static void test_transform_ejes_y_lookat(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Mirador");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    // Unrotated: forward = -Z, right = +X, up = +Y.
    sm.lua().script(R"(
        local t = e:GetTransform()
        f = t:GetForward(); r = t:GetRight(); u = t:GetUp()
    )");
    CHECK(nearlyEqual(sm.lua()["f"]["z"], -1.0f));
    CHECK(nearlyEqual(sm.lua()["r"]["x"], 1.0f));
    CHECK(nearlyEqual(sm.lua()["u"]["y"], 1.0f));

    // Looking at +X leaves the forward pointing to +X.
    sm.lua().script(R"(
        local t = e:GetTransform()
        t:LookAt(Vec3.new(100, 0, 0))
        f2 = t:GetForward()
    )");
    CHECK(nearlyEqual(sm.lua()["f2"]["x"], 1.0f, 0.02f));
    CHECK(nearlyEqual(sm.lua()["f2"]["z"], 0.0f, 0.02f));

    // Looking at yourself has no answer: a warning is given and the rotation does not change.
    log.clear();
    sm.lua().script(R"(
        local t = e:GetTransform()
        t:LookAt(t:GetWorldPosition())
        f3 = t:GetForward()
    )");
    CHECK(logContains(log, "WARN"));
    CHECK(logContains(log, "LookAt"));
    CHECK(nearlyEqual(sm.lua()["f3"]["x"], 1.0f, 0.02f));   // still looking at +X

    // 'up' parallel to the direction: same treatment.
    log.clear();
    sm.lua().script("e:GetTransform():LookAt(Vec3.new(0, 100, 0), Vec3.new(0, 1, 0))");
    CHECK(logContains(log, "WARN"));

    sm.setLogCallback(nullptr);
}

// With a parent, the world position is NOT the local one: if SetWorldPosition did not
// undo the parent's transform, the object would end up twice as far away.
static void test_transform_set_world_position_con_padre(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* padre = scene.addGameObject("Padre");
    padre->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
    GameObject* hijo = scene.addGameObject("Hijo", padre);
    padre->updateWorldTransforms(glm::mat4(1.0f));
    sm.rebuildAliveSet();

    sm.lua()["e"] = LuaEntity{ hijo, &sm };
    sm.lua().script("e:GetTransform():SetWorldPosition(Vec3.new(0, 0, 0))");

    // Local = -10 in X to end up at the world origin.
    const glm::vec3 local(hijo->localTransform[3]);
    CHECK(nearlyEqual(local.x, -10.0f));
    const glm::vec3 mundo(hijo->worldTransform[3]);
    CHECK(nearlyEqual(mundo.x, 0.0f));
}

static void test_light_y_camera_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Farola");
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["e"] = LuaEntity{ go, &sm };

    // Before adding it, the getter returns nil (not an error).
    sm.lua().script("sinLuz = (e:GetLight() == nil)");
    CHECK(sm.lua()["sinLuz"] == true);

    sm.lua().script(R"(
        local l = e:AddLight()
        l.type = LightType.Spot
        l.intensity = 3.5
        l.range = 250
        l.innerAngle = 15
        l.outerAngle = 40
        l:SetColor(Vec3.new(1, 0.5, 0))
        tipo = l.type
        col = l:GetColor()
        -- El mismo componente por la vía genérica.
        porNombre = (e:GetComponent("Light") ~= nil)
    )");
    CHECK(go->hasLight());
    CHECK(go->getLight()->getType() == LightType::Spot);
    CHECK(nearlyEqual(go->getLight()->getIntensity(), 3.5f));
    CHECK(nearlyEqual(go->getLight()->getRange(), 250.0f));
    CHECK(nearlyEqual(go->getLight()->getOuterAngle(), 40.0f));
    CHECK(nearlyEqual(sm.lua()["col"]["y"], 0.5f));
    CHECK(sm.lua()["porNombre"] == true);

    // Core clamp: 500 is trimmed to 100, it is not written as is.
    sm.lua().script("e:GetLight().intensity = 500");
    CHECK(nearlyEqual(go->getLight()->getIntensity(), 100.0f));

    // Type outside the enum: warning and no change.
    log.clear();
    sm.lua().script("e:GetLight().type = 99");
    CHECK(logContains(log, "WARN"));
    CHECK(go->getLight()->getType() == LightType::Spot);

    // NaN: same contract as the rest of the setters.
    log.clear();
    sm.lua().script("e:GetLight().range = 0/0");
    CHECK(logContains(log, "WARN"));
    CHECK(nearlyEqual(go->getLight()->getRange(), 250.0f));

    sm.lua().script("e:RemoveLight()");
    CHECK(!go->hasLight());

    // Camera.
    sm.lua().script(R"(
        local c = e:AddCamera()
        c.mode = CameraProjection.Orthographic
        c.fov = 70
        c.orthographicSize = 42
        c.near = 2
        c.far = 900
        modo = c.mode
    )");
    CHECK(go->hasCameraComponent());
    CHECK(go->getCameraComponent()->getMode() == CameraComponent::ProjectionMode::Orthographic);
    CHECK(nearlyEqual(go->getCameraComponent()->getFov(), 70.0f));
    CHECK(nearlyEqual(go->getCameraComponent()->getOrthographicSize(), 42.0f));
    CHECK(nearlyEqual(go->getCameraComponent()->getFar(), 900.0f));

    log.clear();
    sm.lua().script("e:GetCamera().mode = 7");
    CHECK(logContains(log, "WARN"));
    CHECK(go->getCameraComponent()->getMode() == CameraComponent::ProjectionMode::Orthographic);

    sm.lua().script("e:RemoveComponent('Camera')");
    CHECK(!go->hasCameraComponent());

    sm.setLogCallback(nullptr);
}

static void test_entity_mesh_visible_desde_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Caja");
    sm.rebuildAliveSet();
    sm.lua()["e"] = LuaEntity{ go, &sm };

    CHECK(go->meshVisible);            // core default
    sm.lua().script("antes = e.meshVisible; e.meshVisible = false");
    CHECK(sm.lua()["antes"] == true);
    CHECK(!go->meshVisible);
    sm.lua().script("e.meshVisible = true");
    CHECK(go->meshVisible);
}

// ---------------------------------------------------------------------------
// Entity:SetParent: the movement lives in Scene::reparent (Core) and is shared by
// Lua and the editor hierarchy reparent.
// ---------------------------------------------------------------------------

// By default the WORLD pose is preserved (like Unity's transform.parent): the
// object stays where it was and what is recomputed is its local.
static void test_set_parent_mantiene_la_pose_de_mundo(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* padre = scene.addGameObject("Padre");
    padre->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, 0.0f, 0.0f));
    GameObject* suelto = scene.addGameObject("Suelto");
    suelto->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(30.0f, 0.0f, 0.0f));
    scene.getRoot().updateWorldTransforms();
    sm.rebuildAliveSet();

    sm.lua()["hijo"]  = LuaEntity{ suelto, &sm };
    sm.lua()["papa"]  = LuaEntity{ padre,  &sm };
    sm.lua().script("ok = hijo:SetParent(papa)");

    CHECK(sm.lua()["ok"] == true);
    CHECK(suelto->parent == padre);
    // Still at x=30 in WORLD...
    CHECK(nearlyEqual(glm::vec3(suelto->worldTransform[3]).x, 30.0f));
    // ...and that is why its local becomes -70 (30 - 100).
    CHECK(nearlyEqual(glm::vec3(suelto->localTransform[3]).x, -70.0f));

    // The world is up to date ALREADY, without waiting for the next frame.
    sm.lua().script("wx = hijo:GetTransform():GetWorldPosition().x");
    CHECK(nearlyEqual(sm.lua()["wx"], 30.0f));
}

// With false the LOCAL is preserved and the object jumps along with its new parent, which is
// what dragging in the editor hierarchy does.
static void test_set_parent_conservando_el_local(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* padre = scene.addGameObject("Padre");
    padre->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(100.0f, 0.0f, 0.0f));
    GameObject* suelto = scene.addGameObject("Suelto");
    suelto->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(30.0f, 0.0f, 0.0f));
    scene.getRoot().updateWorldTransforms();
    sm.rebuildAliveSet();

    sm.lua()["hijo"] = LuaEntity{ suelto, &sm };
    sm.lua()["papa"] = LuaEntity{ padre,  &sm };
    sm.lua().script("hijo:SetParent(papa, false)");

    CHECK(nearlyEqual(glm::vec3(suelto->localTransform[3]).x, 30.0f));   // local intact
    CHECK(nearlyEqual(glm::vec3(suelto->worldTransform[3]).x, 130.0f));  // world jumped
}

// Without an argument it goes back to the root, and GetParent reflects it.
static void test_set_parent_nil_vuelve_a_la_raiz(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* padre = scene.addGameObject("Padre");
    GameObject* hijo  = scene.addGameObject("Hijo", padre);
    scene.getRoot().updateWorldTransforms();
    sm.rebuildAliveSet();

    sm.lua()["h"] = LuaEntity{ hijo, &sm };
    sm.lua().script(R"(
        antes = h:GetParent().name
        h:SetParent()
        -- Colgando de la raíz, GetParent devuelve nil (contrato de siempre).
        enRaiz = (h:GetParent() == nil)
    )");
    CHECK(sm.lua()["antes"] == std::string("Padre"));
    CHECK(hijo->parent == &scene.getRoot());
    CHECK(sm.lua()["enRaiz"] == true);
    CHECK(padre->children.empty());
}

// Hanging an object from its own descendant would detach the subtree from the
// tree and take with it the unique_ptr that keeps it alive.
static void test_set_parent_rechaza_el_ciclo(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* abuelo = scene.addGameObject("Abuelo");
    GameObject* nieto  = scene.addGameObject("Nieto", abuelo);
    scene.getRoot().updateWorldTransforms();
    sm.rebuildAliveSet();

    std::vector<std::string> log;
    sm.setLogCallback([&](const std::string& m) { log.push_back(m); });
    sm.lua()["a"] = LuaEntity{ abuelo, &sm };
    sm.lua()["n"] = LuaEntity{ nieto,  &sm };
    sm.lua().script("ok = a:SetParent(n)");

    CHECK(sm.lua()["ok"] == false);
    CHECK(logContains(log, "WARN"));
    CHECK(logContains(log, "SetParent"));
    // The tree remains as it was.
    CHECK(nieto->parent == abuelo);
    CHECK(abuelo->parent == &scene.getRoot());
    CHECK(abuelo->children.size() == 1);
    sm.setLogCallback(nullptr);
}

// The editor hierarchy reparent now goes through Scene::reparent: this
// test protects that execute/undo keep leaving the node where it belongs. Without it, the
// move of the algorithm out of ReparentCommand was checked by nobody.
static void test_reparent_command_sigue_moviendo_y_deshaciendo()
{
    Scene scene("Test");
    GameObject* a = scene.addGameObject("A");
    GameObject* b = scene.addGameObject("B");
    GameObject* c = scene.addGameObject("C", a);

    ReparentCommand cmd(scene, "Mover 'C'", c->id,
                        a->id, /*oldIndex=*/0,
                        b->id, /*newIndex=*/0);
    cmd.execute();
    CHECK(c->parent == b);
    CHECK(b->children.size() == 1);
    CHECK(a->children.empty());

    cmd.undo();
    CHECK(c->parent == a);
    CHECK(a->children.size() == 1);
    CHECK(b->children.empty());

    // With ONE single child the index does not matter, so a case where it
    // matters is needed: reordering within the same parent. A reparent that ignored the
    // index and always appended at the end would pass the whole block above.
    GameObject* p  = scene.addGameObject("P");
    GameObject* h0 = scene.addGameObject("H0", p);
    GameObject* h1 = scene.addGameObject("H1", p);
    GameObject* h2 = scene.addGameObject("H2", p);

    ReparentCommand orden(scene, "Mover 'H2'", h2->id,
                          p->id, /*oldIndex=*/2,
                          p->id, /*newIndex=*/1);
    orden.execute();
    CHECK(p->children.size() == 3);
    CHECK(p->children[0].get() == h0);
    CHECK(p->children[1].get() == h2);   // got in the middle
    CHECK(p->children[2].get() == h1);

    orden.undo();
    CHECK(p->children[0].get() == h0);
    CHECK(p->children[1].get() == h1);
    CHECK(p->children[2].get() == h2);   // goes back to the end
}

// Animator from Lua: Play, CrossFade, ResetTrigger and GetNormalizedTime.
static void test_animator_lua_play_crossfade_reset_and_time(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s;
    s.duration = 100.0f; s.ticksPerSecond = 10.0f;     // 10 s per loop
    s.name = "Idle"; s.clipName = "Idle"; a->addState(s);
    s.name = "Run";  s.clipName = "Run";  a->addState(s);
    a->addParameter("go", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "go";
    t.conditions.push_back(c);
    a->addTransition(t);
    go->setAnimator(a);
    a->update(0.0f, false);
    sm.rebuildAliveSet();
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        local an = e:GetComponent("Animator")
        okPlay   = an:Play("Run")
        noExiste = an:Play("Nada")
        okFade   = an:CrossFade("Idle", 0.5)
        an:SetTrigger("go")
        an:ResetTrigger("go")
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    // Without this, a broken script leaves the globals at nil and get<bool> triggers
    // the sol2 panic, which brings down the whole executable instead of a FAIL.
    if (!r.valid()) return;
    CHECK(sm.lua()["okPlay"].get<bool>());
    CHECK(!sm.lua()["noExiste"].get<bool>());
    CHECK(sm.lua()["okFade"].get<bool>());
    CHECK(a->currentStateName() == "Idle");
    CHECK(a->previousStateName() == "Run");

    // The disarmed trigger does not fire Idle -> Run.
    a->update(0.016f, true);
    CHECK(a->currentStateName() == "Idle");

    a->update(2.484f, false);                          // 0.25 loops in Idle
    auto r2 = sm.lua().safe_script("tn = e:GetComponent('Animator'):GetNormalizedTime()",
                                   sol::script_pass_on_error);
    CHECK(r2.valid());
    if (!r2.valid()) return;
    CHECK(nearlyEqual(sm.lua()["tn"].get<float>(), 0.25f));

    // Global speed from Lua; a negative freezes (0).
    auto r3 = sm.lua().safe_script(R"(
        local an = e:GetComponent("Animator")
        an:SetSpeed(1.5)
        v1 = an:GetSpeed()
        an:SetSpeed(-4)
        v2 = an:GetSpeed()
    )", sol::script_pass_on_error);
    CHECK(r3.valid());
    if (!r3.valid()) return;
    CHECK(nearlyEqual(sm.lua()["v1"].get<float>(), 1.5f));
    CHECK(nearlyEqual(sm.lua()["v2"].get<float>(), 0.0f));
    CHECK(nearlyEqual(a->speed(), 0.0f));
}

// Layers from Lua: weight, layer count and the optional layer argument.
static void test_animator_lua_layers(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s;
    s.duration = 100.0f; s.ticksPerSecond = 10.0f;
    s.name = "Idle"; s.clipName = "Idle"; a->addState(s);
    s.name = "Run";  s.clipName = "Run";  a->addState(s);
    const int l1 = a->addLayer("Brazos");
    s.name = "Low"; s.clipName = "Low"; a->addState(s, l1);
    s.name = "Aim"; s.clipName = "Aim"; a->addState(s, l1);
    a->setEntryState(0, l1);
    go->setAnimator(a);
    a->update(0.0f, false);
    sm.rebuildAliveSet();
    sm.lua()["e"] = LuaEntity{ go, &sm };

    auto r = sm.lua().safe_script(R"(
        local an = e:GetComponent("Animator")
        an:SetLayerWeight(1, 0.5)
        peso    = an:GetLayerWeight(1)
        pesoB   = an:GetLayerWeight(0)
        pesoNo  = an:GetLayerWeight(9)
        capas   = an:GetLayerCount()
        okAim   = an:Play("Aim", 1)
        base    = an:GetState()
        brazos  = an:GetState(1)
        nadie   = an:GetState(9)
        okNo    = an:Play("Aim", 9)
        okFade  = an:CrossFade("Low", 0.5, 1)
        mezcla  = an:IsBlending(1)
        mezcla0 = an:IsBlending()
        an:SetLayerWeight(9, 1)
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) return;
    CHECK(nearlyEqual(sm.lua()["peso"].get<float>(), 0.5f));
    CHECK(nearlyEqual(sm.lua()["pesoB"].get<float>(), 1.0f));
    CHECK(nearlyEqual(sm.lua()["pesoNo"].get<float>(), 0.0f));
    CHECK(sm.lua()["capas"].get<int>() == 2);
    CHECK(sm.lua()["okAim"].get<bool>());
    CHECK(sm.lua()["base"].get<std::string>() == "Idle");
    CHECK(sm.lua()["brazos"].get<std::string>() == "Aim");
    CHECK(sm.lua()["nadie"].get<std::string>().empty());
    CHECK(!sm.lua()["okNo"].get<bool>());
    CHECK(sm.lua()["okFade"].get<bool>());
    CHECK(sm.lua()["mezcla"].get<bool>());
    CHECK(!sm.lua()["mezcla0"].get<bool>());
    CHECK(a->currentStateName(1) == "Low");
    CHECK(a->layerCount() == 2);
}

// IK from Lua: weight, target, pole and count.
static void test_animator_lua_ik(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Personaje");
    GameObject* objetivo = scene.addGameObject("Objetivo");
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::IkConstraint mirar;
    mirar.name = "mirar"; mirar.boneName = "head";
    AnimatorComponent::IkConstraint mano;
    mano.name = "mano"; mano.type = AnimatorComponent::IkType::TwoBone; mano.boneName = "hand";
    a->addIkConstraint(mirar);
    a->addIkConstraint(mano);
    go->setAnimator(a);
    sm.rebuildAliveSet();
    sm.lua()["e"] = LuaEntity{ go, &sm };
    sm.lua()["obj"] = LuaEntity{ objetivo, &sm };

    auto r = sm.lua().safe_script(R"(
        local an = e:GetComponent("Animator")
        an:SetIkWeight("mano", 0.5)
        peso   = an:GetIkWeight("mano")
        pesoNo = an:GetIkWeight("noExiste")
        cuenta = an:GetIkCount()
        an:SetIkTarget("mirar", obj)
        an:SetIkPole("mano", obj)
        an:SetIkWeight("noExiste", 1)
        an:SetIkTarget("noExiste", obj)
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) return;
    CHECK(nearlyEqual(sm.lua()["peso"].get<float>(), 0.5f));
    CHECK(nearlyEqual(sm.lua()["pesoNo"].get<float>(), 0.0f));
    CHECK(sm.lua()["cuenta"].get<int>() == 2);
    CHECK(a->ikConstraints()[0].targetId == objetivo->id);
    CHECK(a->ikConstraints()[1].poleId == objetivo->id);

    // Removing the target with nil.
    auto r2 = sm.lua().safe_script("e:GetComponent('Animator'):SetIkTarget('mirar', nil)",
                                    sol::script_pass_on_error);
    CHECK(r2.valid());
    CHECK(a->ikConstraints()[0].targetId == 0u);
}

// Repo rule: every new binding also goes into autocomplete.
static void test_animator_lua_new_methods_are_in_the_reference()
{
    const auto& simbolos = luaApiSymbols();
    for (const char* nombre : { "Animator:Play", "Animator:CrossFade",
                                "Animator:ResetTrigger", "Animator:GetNormalizedTime",
                                "Animator:SetSpeed", "Animator:GetSpeed",
                                "Animator:SetLayerWeight", "Animator:GetLayerWeight",
                                "Animator:GetLayerCount",
                                "Animator:SetIkWeight", "Animator:GetIkWeight",
                                "Animator:SetIkTarget", "Animator:SetIkPole",
                                "Animator:GetIkCount" })
    {
        std::string firma, doc;
        luaApiDoc(nombre, firma, doc);
        const bool esta = std::find(simbolos.begin(), simbolos.end(), nombre) != simbolos.end();
        if (!esta || firma.empty() || doc.empty())
        {
            std::printf("FAIL: %s no esta completo en LuaApiReference\n", nombre);
            ++g_failures;
        }
    }
}

// OnAnimationEvent: an event fired by the Animator reaches the scripts of the
// same GameObject on the next ScriptManager::update, only once.
static void test_animation_event_reaches_lua(ScriptManager& sm)
{
    Scene scene("Test");
    sm.setScene(&scene);
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s;
    s.name = "Walk"; s.clipName = "Walk"; s.duration = 40.0f; s.ticksPerSecond = 20.0f;
    s.events.push_back({ "paso", 0.5f });
    a->addState(s);
    go->setAnimator(a);
    sm.rebuildAliveSet();
    sm.onPlayStart();

    auto r = sm.lua().safe_script(R"(
        recibidos = ""
        OyenteEventos = {}
        function OyenteEventos:OnAnimationEvent(nombre) recibidos = recibidos .. nombre .. ";" end
    )", sol::script_pass_on_error);
    CHECK(r.valid());
    if (!r.valid()) { sm.onPlayStop(); return; }
    // Instance by hand and already started: there is no .lua on disk to load.
    auto comp = std::make_unique<ScriptComponent>("OyenteEventos", go);
    comp->instance = sm.lua()["OyenteEventos"];
    comp->started  = true;
    go->addScript(std::move(comp));

    a->update(1.1f, true);                 // crosses half of the 2 s cycle
    CHECK(a->firedEvents().size() == 1u);
    sm.update(0.016f);
    CHECK(sm.lua()["recibidos"].get<std::string>() == "paso;");

    a->update(0.1f, true);                 // crosses nothing
    sm.update(0.016f);
    CHECK(sm.lua()["recibidos"].get<std::string>() == "paso;");
    sm.onPlayStop();
}

int main()
{
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    am.init();

    // Nonexistent folder on purpose: init() registers the Lua bindings
    // all the same (it only logs the "folder not found" warning and carries on, see
    // ScriptManager::init); these tests load no .lua from disk.
    ScriptManager sm;
    sm.init("__scripting_tests_sin_carpeta_de_scripts__");
    sm.setPhysicsManager(&pm);
    sm.setAudioManager(&am);

    test_animator_lua_play_crossfade_reset_and_time(sm);
    test_animator_lua_layers(sm);
    test_animator_lua_ik(sm);
    test_animation_event_reaches_lua(sm);
    test_animator_lua_new_methods_are_in_the_reference();
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
    test_visibilidad_de_panel_round_trip();
    test_panel_sin_dato_no_es_cerrado();

    test_autocomplete_prefijo_de_tipo();
    test_autocomplete_por_miembro_conserva_receptor();
    test_autocomplete_respeta_el_separador();
    test_autocomplete_orden_y_vacio();
    test_autocomplete_simbolos_nuevos_en_la_tabla();
    test_autocomplete_firma_y_doc();
    test_vec3_algebra_desde_lua(sm);
    test_time_desde_lua(sm);
    test_transform_ejes_y_lookat(sm);
    test_transform_set_world_position_con_padre(sm);
    test_light_y_camera_desde_lua(sm);
    test_entity_mesh_visible_desde_lua(sm);
    test_set_parent_mantiene_la_pose_de_mundo(sm);
    test_set_parent_conservando_el_local(sm);
    test_set_parent_nil_vuelve_a_la_raiz(sm);
    test_set_parent_rechaza_el_ciclo(sm);
    test_reparent_command_sigue_moviendo_y_deshaciendo();

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL SCRIPTING TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
