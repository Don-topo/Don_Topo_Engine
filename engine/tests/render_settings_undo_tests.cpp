// Test headless del undo/redo de los ajustes de render del menu View (P8/H49).
//
// No hay GUI aqui: se prueba el COMANDO y su lambda, no el widget de ImGui —
// mismo patron que los tests de undo de camera_tests.cpp. El sujeto de prueba
// es `makeRenderSettingCommand`, que es el seam: EditorUI solo lee el valor
// previo, dibuja el widget y llama a ese helper.
//
// El estado de destino es un RendererState de verdad, no un doble: la clase no
// toca la API grafica (es justo lo que documenta su cabecera), asi que se puede
// construir en un test sin device ni ventana.
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Renderer/RendererState.h"

#include <glm/glm.hpp>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.0001f) { return std::fabs(a - b) < eps; }

// Comando de escena cualquiera, para los tests que mezclan las dos familias en
// el mismo stack. No toca nada: solo deja constancia de por donde ha pasado.
static std::unique_ptr<ICommand> makeSceneCommand(int& target, int before, int after)
{
    return std::make_unique<PropertyCommand<int>>(
        "Scene edit", before, after, [&target](const int& v) { target = v; });
}

// ── El flag que separa las dos familias ─────────────────────────────────────

// Un ajuste de render NO es una edicion de la escena: mover el bloom no puede
// hacer que el Content Browser pida guardar una escena que nadie ha tocado.
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

// Y el default sigue ensuciando: los ~30 push que ya existian no cambian de
// comportamiento por llevar el parametro nuevo.
static void test_push_de_escena_si_ensucia()
{
    UndoManager undo;
    int target = 0;
    undo.push(makeSceneCommand(target, 0, 1));
    CHECK(undo.isSceneDirty());
}

// Un push de render no puede LIMPIAR el dirty de una edicion de escena
// anterior: seria perder trabajo sin avisar.
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

// ── Un tipo por cada forma de widget del menu View ──────────────────────────

// SliderFloat (25 de los 39).
static void test_undo_redo_float()
{
    UndoManager undo;
    RendererState state;
    int saves = 0;
    const float antes = state.bloomThreshold();

    state.setBloomThreshold(3.25f);   // lo que ya hizo el widget al arrastrarse
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

// Checkbox (los seis interruptores de efecto, mas Wireframe).
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

// ColorEdit3 (Fog scattering) — el unico vec3 del menu, y el unico widget cuyo
// valor no cabe en un escalar.
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

// ── Lo que separa este comando de un PropertyCommand a secas ────────────────

// Un ajuste de render vive en el project.json, no en la escena: si el undo
// aplica el valor pero no vuelve a escribir el fichero, la imagen se corrige y
// al reabrir el proyecto reaparece lo deshecho. El helper tiene que persistir
// en LOS DOS sentidos.
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

    CHECK(saves == 0);   // push() no ejecuta: el widget ya aplico y ya guardo
    undo.undo();
    CHECK(saves == 1);
    undo.redo();
    CHECK(saves == 2);
}

// ── El stack es UNO SOLO ────────────────────────────────────────────────────

// Ctrl+Z deshace la ultima accion del usuario, sea de la familia que sea. Este
// es el motivo de no haber montado un segundo UndoManager para el render.
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

    undo.undo();                                   // el de render, que es el ultimo
    CHECK(nearlyEqual(state.ssaoIntensity(), antes));
    CHECK(target == 1);                            // el de escena sigue sin tocarse

    undo.undo();                                   // ahora si, el de escena
    CHECK(target == 0);
}

// Un ajuste de render nuevo invalida el redo pendiente igual que cualquier otra
// accion: el flag solo decide si la ESCENA queda sucia, nada mas.
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

int main()
{
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

    if (g_failures == 0) std::printf("ALL RENDER SETTINGS UNDO TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
