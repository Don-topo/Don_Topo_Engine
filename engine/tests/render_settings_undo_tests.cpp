// Headless test of the undo/redo of the View menu render settings (P8/H49).
//
// There is no GUI here: the COMMAND and its lambda are tested, not the ImGui widget,
// same pattern as the undo tests of camera_tests.cpp. The test subject
// is `makeRenderSettingCommand`, which is the seam: EditorUI only reads the
// previous value, draws the widget and calls that helper.
//
// The target state is a real RendererState, not a double: the class does not
// touch the graphics API (it is exactly what its header documents), so it can be
// built in a test without a device or window.
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Renderer/RendererState.h"

#include <glm/glm.hpp>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.0001f) { return std::fabs(a - b) < eps; }

// Any scene command, for the tests that mix the two families in
// the same stack. It touches nothing: it only records where it has been.
static std::unique_ptr<ICommand> makeSceneCommand(int& target, int before, int after)
{
    return std::make_unique<PropertyCommand<int>>(
        "Scene edit", before, after, [&target](const int& v) { target = v; });
}

// CompositeCommand: a single step; execute in order, undo in reverse.
struct RecordingCommand : public ICommand
{
    std::vector<std::string>& log;
    std::string               name;
    RecordingCommand(std::vector<std::string>& l, std::string n) : log(l), name(std::move(n)) {}
    void execute() override { log.push_back("do " + name); }
    void undo() override { log.push_back("undo " + name); }
    std::string label() const override { return name; }
};

static void test_composite_command_order()
{
    std::vector<std::string> log;
    CompositeCommand group("grupo");
    group.add(std::make_unique<RecordingCommand>(log, "a"));
    group.add(std::make_unique<RecordingCommand>(log, "b"));
    group.execute();
    group.undo();
    CHECK(log == std::vector<std::string>({ "do a", "do b", "undo b", "undo a" }));
    CHECK(group.label() == "grupo");
}

// ── The flag that separates the two families ─────────────────────────────────

// A render setting is NOT a scene edit: moving the bloom cannot
// make the Content Browser ask to save a scene that nobody has touched.
static void test_push_de_render_no_ensucia_la_escena()
{
    UndoManager undo;
    RendererState state;
    int saves = 0;

    undo.push(makeRenderSettingCommand<float>(
                  "Bloom threshold", state.bloomThreshold(), 2.5f,
                  [&state](const float& v) { state.setBloomThreshold(v); },
                  [&saves]() { ++saves; }),
              /*dirtiesScene=*/false);

    CHECK(!undo.isSceneDirty());
    CHECK(undo.canUndo());
}

// And the default still marks dirty: the ~30 pushes that already existed do not change
// behavior for carrying the new parameter.
static void test_push_de_escena_si_ensucia()
{
    UndoManager undo;
    int target = 0;
    undo.push(makeSceneCommand(target, 0, 1));
    CHECK(undo.isSceneDirty());
}

// A render push cannot CLEAR the dirty flag of a previous scene edit: it
// would be losing work without warning.
static void test_un_push_de_render_no_limpia_el_dirty_previo()
{
    UndoManager undo;
    RendererState state;
    int target = 0;
    int saves = 0;

    undo.push(makeSceneCommand(target, 0, 1));
    CHECK(undo.isSceneDirty());

    undo.push(makeRenderSettingCommand<bool>(
                  "SSAO", state.ssaoEnabled(), false,
                  [&state](const bool& v) { state.setSsaoEnabledFlag(v); },
                  [&saves]() { ++saves; }),
              /*dirtiesScene=*/false);

    CHECK(undo.isSceneDirty());
}

// ── One type for each widget shape of the View menu ──────────────────────────

// SliderFloat (25 of the 39).
static void test_undo_redo_float()
{
    UndoManager undo;
    RendererState state;
    int saves = 0;
    const float antes = state.bloomThreshold();

    state.setBloomThreshold(3.25f);   // what the widget already did when dragged
    undo.push(makeRenderSettingCommand<float>(
                  "Bloom threshold", antes, 3.25f,
                  [&state](const float& v) { state.setBloomThreshold(v); },
                  [&saves]() { ++saves; }),
              /*dirtiesScene=*/false);

    undo.undo();
    CHECK(nearlyEqual(state.bloomThreshold(), antes));
    undo.redo();
    CHECK(nearlyEqual(state.bloomThreshold(), 3.25f));
}

// SliderInt (SSR steps, Fog steps, Motion blur samples).
static void test_undo_redo_int()
{
    UndoManager undo;
    RendererState state;
    int saves = 0;
    const int antes = state.ssrMaxSteps();

    state.setSsrMaxSteps(96);
    undo.push(makeRenderSettingCommand<int>(
                  "SSR steps", antes, 96,
                  [&state](const int& v) { state.setSsrMaxSteps(v); },
                  [&saves]() { ++saves; }),
              /*dirtiesScene=*/false);

    undo.undo();
    CHECK(state.ssrMaxSteps() == antes);
    undo.redo();
    CHECK(state.ssrMaxSteps() == 96);
}

// Checkbox (the six effect switches, plus Wireframe).
static void test_undo_redo_bool()
{
    UndoManager undo;
    RendererState state;
    int saves = 0;
    const bool antes = state.isWireframeMode();

    state.setWireframeMode(!antes);
    undo.push(makeRenderSettingCommand<bool>(
                  "Wireframe", antes, !antes,
                  [&state](const bool& v) { state.setWireframeMode(v); },
                  [&saves]() { ++saves; }),
              /*dirtiesScene=*/false);

    undo.undo();
    CHECK(state.isWireframeMode() == antes);
    undo.redo();
    CHECK(state.isWireframeMode() == !antes);
}

// ColorEdit3 (Fog scattering): the only vec3 of the menu, and the only widget whose
// value does not fit in a scalar.
static void test_undo_redo_vec3()
{
    UndoManager undo;
    RendererState state;
    int saves = 0;
    const glm::vec3 antes = state.fogScatter();
    const glm::vec3 nuevo{0.1f, 0.2f, 0.3f};

    state.setFogScatter(nuevo);
    undo.push(makeRenderSettingCommand<glm::vec3>(
                  "Fog scattering", antes, nuevo,
                  [&state](const glm::vec3& v) { state.setFogScatter(v); },
                  [&saves]() { ++saves; }),
              /*dirtiesScene=*/false);

    undo.undo();
    CHECK(nearlyEqual(state.fogScatter().x, antes.x));
    CHECK(nearlyEqual(state.fogScatter().y, antes.y));
    CHECK(nearlyEqual(state.fogScatter().z, antes.z));
    undo.redo();
    CHECK(nearlyEqual(state.fogScatter().z, 0.3f));
}

// Combo de enum (Anti-aliasing, Forward+, Present mode, Shadow resolution).
static void test_undo_redo_enum()
{
    UndoManager undo;
    RendererState state;
    int saves = 0;
    using AaMode = RendererState::AaMode;
    const AaMode antes = state.aaMode();

    state.setAaModeFlag(AaMode::Taa);
    undo.push(makeRenderSettingCommand<AaMode>(
                  "Anti-aliasing", antes, AaMode::Taa,
                  [&state](const AaMode& v) { state.setAaModeFlag(v); },
                  [&saves]() { ++saves; }),
              /*dirtiesScene=*/false);

    undo.undo();
    CHECK(state.aaMode() == antes);
    undo.redo();
    CHECK(state.aaMode() == AaMode::Taa);
}

// ── What separates this command from a plain PropertyCommand ─────────────────

// A render setting lives in project.json, not in the scene: if undo
// applies the value but does not write the file again, the image is corrected and
// on reopening the project what was undone comes back. The helper has to persist
// in BOTH directions.
static void test_undo_y_redo_persisten()
{
    UndoManager undo;
    RendererState state;
    int saves = 0;

    undo.push(makeRenderSettingCommand<float>(
                  "SSAO radius", state.ssaoRadius(), 1.5f,
                  [&state](const float& v) { state.setSsaoRadius(v); },
                  [&saves]() { ++saves; }),
              /*dirtiesScene=*/false);

    CHECK(saves == 0);   // push() does not execute: the widget already applied and already saved
    undo.undo();
    CHECK(saves == 1);
    undo.redo();
    CHECK(saves == 2);
}

// ── El stack es UNO SOLO ────────────────────────────────────────────────────

// Ctrl+Z undoes the user's last action, whatever family it belongs to. This
// is the reason for not having built a second UndoManager for render.
static void test_orden_unico_mezclando_escena_y_render()
{
    UndoManager undo;
    RendererState state;
    int target = 0;
    int saves = 0;

    target = 1;
    undo.push(makeSceneCommand(target, 0, 1));

    const float antes = state.ssaoIntensity();
    state.setSsaoIntensity(2.0f);
    undo.push(makeRenderSettingCommand<float>(
                  "SSAO intensity", antes, 2.0f,
                  [&state](const float& v) { state.setSsaoIntensity(v); },
                  [&saves]() { ++saves; }),
              /*dirtiesScene=*/false);

    undo.undo();                                   // the render one, which is the last
    CHECK(nearlyEqual(state.ssaoIntensity(), antes));
    CHECK(target == 1);                            // the scene one is still untouched

    undo.undo();                                   // now the scene one
    CHECK(target == 0);
}

// A new render setting invalidates the pending redo just like any other
// action: the flag only decides whether the SCENE ends up dirty, nothing else.
static void test_un_push_de_render_invalida_el_redo()
{
    UndoManager undo;
    RendererState state;
    int saves = 0;

    undo.push(makeRenderSettingCommand<float>(
                  "Bloom knee", state.bloomKnee(), 0.8f,
                  [&state](const float& v) { state.setBloomKnee(v); },
                  [&saves]() { ++saves; }),
              /*dirtiesScene=*/false);
    undo.undo();
    CHECK(undo.canRedo());

    undo.push(makeRenderSettingCommand<float>(
                  "Bloom intensity", state.bloomIntensity(), 0.5f,
                  [&state](const float& v) { state.setBloomIntensity(v); },
                  [&saves]() { ++saves; }),
              /*dirtiesScene=*/false);
    CHECK(!undo.canRedo());
}

// revision() is the signal with which AnimatorGraphUndoTracker knows that someone
// touched the history in the middle of a gesture. It has to change with EVERYTHING
// that moves the stacks and with nothing else: if it did not change on an undo, the tracker
// would put the undone thing inside the gesture's command; if it changed on an
// empty undo, it would discard user gestures for no reason.
static void test_revision_cambia_con_cada_movimiento_del_historial()
{
    UndoManager undo;
    int target = 0;

    const uint64_t r0 = undo.revision();
    undo.undo();   // empty stacks: they do nothing
    undo.redo();
    CHECK(undo.revision() == r0);

    undo.push(makeSceneCommand(target, 0, 1));
    const uint64_t r1 = undo.revision();
    CHECK(r1 != r0);

    undo.undo();
    const uint64_t r2 = undo.revision();
    CHECK(r2 != r1);

    undo.redo();
    const uint64_t r3 = undo.revision();
    CHECK(r3 != r2);

    undo.clear();
    CHECK(undo.revision() != r3);
}

int main()
{
    test_composite_command_order();

    test_push_de_render_no_ensucia_la_escena();
    test_push_de_escena_si_ensucia();
    test_un_push_de_render_no_limpia_el_dirty_previo();

    test_undo_redo_float();
    test_undo_redo_int();
    test_undo_redo_bool();
    test_undo_redo_vec3();
    test_undo_redo_enum();

    test_undo_y_redo_persisten();
    test_orden_unico_mezclando_escena_y_render();
    test_un_push_de_render_invalida_el_redo();

    test_revision_cambia_con_cada_movimiento_del_historial();

    if (g_failures == 0) std::printf("ALL RENDER SETTINGS UNDO TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
