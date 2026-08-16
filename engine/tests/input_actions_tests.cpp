// Test headless de la traducción de bindings del panel Input Actions (sin GUI
// ni mando conectado). Plain main + asserts, sin framework — mismo patrón que
// content_browser_tests.cpp.
//
// Cubre el camino que el panel NO podía recorrer: un botón de mando llega como
// código GLFW (Core lo lee con glfwGetGamepadState), no como ImGuiKey, así que
// hace falta la traducción inversa para poder guardarlo como binding.
#include "DonTopo/Editor/InputActionsPanel.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <set>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Cada botón digital del mando da un ImGuiKey distinto, y ese ImGuiKey vuelve
// al MISMO código GLFW: si la ida y la vuelta no cuadran, el binding se pinta
// con un nombre y el runtime dispara con otro botón.
//
// Excepción: GLFW_GAMEPAD_BUTTON_GUIDE (el botón central de Xbox/PS) no tiene
// ImGuiKey, así que no se puede bindear y su ida devuelve -1.
static void testPadRoundTrip()
{
    std::set<int> seen;
    for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b)
    {
        const int key = InputActionsPanel::padButtonToBinding(b);
        if (b == GLFW_GAMEPAD_BUTTON_GUIDE) { CHECK(key < 0); continue; }
        CHECK(key > 0);
        if (key <= 0) continue;

        // Dentro del rango que load() acepta: un binding fuera de él se
        // descartaría al releer el fichero.
        CHECK(key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_NamedKey_END);
        // Y dentro del bloque de mando: si cayera en el de teclado, GetKeyName
        // pintaría una tecla cualquiera.
        CHECK(key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown);
        CHECK(seen.insert(key).second);   // dos botones no pueden mapear al mismo

        const char* device = nullptr;
        int code = -1;
        CHECK(InputActionsPanel::bindingToGlfw(key, device, code));
        CHECK(device != nullptr && std::strcmp(device, "pad") == 0);
        CHECK(code == b);                 // ida y vuelta al mismo botón
    }
    CHECK(seen.size() == static_cast<size_t>(GLFW_GAMEPAD_BUTTON_LAST));   // todos menos GUIDE
}

// Un índice que no es un botón del mando no puede colarse como binding: sin
// esto, un GLFW_GAMEPAD_BUTTON_LAST+1 devolvería basura del enum de ImGui.
static void testPadOutOfRange()
{
    CHECK(InputActionsPanel::padButtonToBinding(-1) < 0);
    CHECK(InputActionsPanel::padButtonToBinding(GLFW_GAMEPAD_BUTTON_LAST + 1) < 0);
    CHECK(InputActionsPanel::padButtonToBinding(1000) < 0);
}

// Los tres dispositivos comparten el rango de ImGuiKey; la traducción tiene que
// separarlos bien, no solo el mando.
static void testKeyboardAndMouseStillTranslate()
{
    const char* device = nullptr;
    int code = -1;

    CHECK(InputActionsPanel::bindingToGlfw(ImGuiKey_A, device, code));
    CHECK(std::strcmp(device, "key") == 0 && code == GLFW_KEY_A);

    CHECK(InputActionsPanel::bindingToGlfw(ImGuiKey_Space, device, code));
    CHECK(std::strcmp(device, "key") == 0 && code == GLFW_KEY_SPACE);

    CHECK(InputActionsPanel::bindingToGlfw(ImGuiKey_MouseLeft, device, code));
    CHECK(std::strcmp(device, "mouse") == 0 && code == GLFW_MOUSE_BUTTON_LEFT);

    CHECK(InputActionsPanel::bindingToGlfw(ImGuiKey_MouseRight, device, code));
    CHECK(std::strcmp(device, "mouse") == 0 && code == GLFW_MOUSE_BUTTON_RIGHT);

    // Gatillos y sticks son ejes en GLFW: no tienen botón equivalente y el
    // panel no debe escribirlos en el mapa de runtime.
    CHECK(!InputActionsPanel::bindingToGlfw(ImGuiKey_GamepadL2, device, code));
    CHECK(!InputActionsPanel::bindingToGlfw(ImGuiKey_GamepadLStickUp, device, code));
}

int main()
{
    testPadRoundTrip();
    testPadOutOfRange();
    testKeyboardAndMouseStillTranslate();

    if (g_failures == 0) std::printf("input_actions_tests: OK\n");
    else                 std::printf("input_actions_tests: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
