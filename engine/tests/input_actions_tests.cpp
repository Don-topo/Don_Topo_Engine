// Test headless de la traducción de bindings del panel Input Actions (sin GUI
// ni mando conectado). Plain main + asserts, sin framework — mismo patrón que
// content_browser_tests.cpp.
//
// Cubre el camino que el panel NO podía recorrer: un botón de mando llega como
// código GLFW (Core lo lee con glfwGetGamepadState), no como ImGuiKey, así que
// hace falta la traducción inversa para poder guardarlo como binding.
#include "DonTopo/Core/Input.h"
#include "DonTopo/Editor/InputActionsPanel.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

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

    // Las ruedas del ratón siguen sin equivalente: son ejes de ImGui que Core
    // no lee, y colarlas daría un binding que no dispara nunca.
    CHECK(!InputActionsPanel::bindingToGlfw(ImGuiKey_MouseWheelX, device, code));
    CHECK(!InputActionsPanel::bindingToGlfw(ImGuiKey_MouseWheelY, device, code));
}

// Gatillos y sticks son ejes, no botones: van por el dispositivo "padaxis" con
// el código eje*2+signo. Se comprueba cada dirección por separado porque el
// signo del eje Y de GLFW está invertido respecto a lo que dice el nombre
// (arriba es NEGATIVO), y un signo cambiado manda la acción al lado contrario
// sin que ninguna prueba de GUI lo note.
static void testPadAxisDirections()
{
    struct Case { int imguiKey; int axis; bool negative; };
    const Case cases[] = {
        { ImGuiKey_GamepadLStickRight, GLFW_GAMEPAD_AXIS_LEFT_X,       false },
        { ImGuiKey_GamepadLStickLeft,  GLFW_GAMEPAD_AXIS_LEFT_X,       true  },
        { ImGuiKey_GamepadLStickDown,  GLFW_GAMEPAD_AXIS_LEFT_Y,       false },
        { ImGuiKey_GamepadLStickUp,    GLFW_GAMEPAD_AXIS_LEFT_Y,       true  },
        { ImGuiKey_GamepadRStickRight, GLFW_GAMEPAD_AXIS_RIGHT_X,      false },
        { ImGuiKey_GamepadRStickLeft,  GLFW_GAMEPAD_AXIS_RIGHT_X,      true  },
        { ImGuiKey_GamepadRStickDown,  GLFW_GAMEPAD_AXIS_RIGHT_Y,      false },
        { ImGuiKey_GamepadRStickUp,    GLFW_GAMEPAD_AXIS_RIGHT_Y,      true  },
        { ImGuiKey_GamepadL2,          GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, false },
        { ImGuiKey_GamepadR2,          GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER,false },
    };

    for (const Case& c : cases)
    {
        const char* device = nullptr;
        int code = -1;
        CHECK(InputActionsPanel::bindingToGlfw(c.imguiKey, device, code));
        CHECK(device != nullptr && std::strcmp(device, "padaxis") == 0);
        CHECK(code == Input::padAxisCode(c.axis, c.negative));
        CHECK(Input::padAxisIndex(code) == c.axis);
        CHECK(Input::padAxisNegative(code) == c.negative);
        // Y la vuelta: el panel captura por código de eje y guarda ImGuiKey.
        CHECK(InputActionsPanel::padAxisToBinding(code) == c.imguiKey);
    }
}

// Todo código de eje válido va y vuelve al mismo sitio, y ningún ImGuiKey se
// repite. Los dos códigos sin equivalente (gatillo en negativo: un gatillo solo
// se pulsa hacia un lado) no se pueden bindear.
static void testPadAxisRoundTrip()
{
    std::set<int> seen;
    for (int c = 0; c < Input::kPadAxisBindingCount; ++c)
    {
        const int key = InputActionsPanel::padAxisToBinding(c);
        const bool isTriggerNegative =
            (Input::padAxisIndex(c) == GLFW_GAMEPAD_AXIS_LEFT_TRIGGER
             || Input::padAxisIndex(c) == GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER)
            && Input::padAxisNegative(c);
        if (isTriggerNegative) { CHECK(key < 0); continue; }

        CHECK(key > 0);
        if (key <= 0) continue;
        CHECK(key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_NamedKey_END);
        CHECK(key >= ImGuiKey_GamepadStart && key <= ImGuiKey_GamepadRStickDown);
        CHECK(seen.insert(key).second);

        const char* device = nullptr;
        int code = -1;
        CHECK(InputActionsPanel::bindingToGlfw(key, device, code));
        CHECK(device != nullptr && std::strcmp(device, "padaxis") == 0);
        CHECK(code == c);
    }
    CHECK(seen.size() == 10);   // 8 direcciones de stick + 2 gatillos
}

static void testPadAxisOutOfRange()
{
    CHECK(InputActionsPanel::padAxisToBinding(-1) < 0);
    CHECK(InputActionsPanel::padAxisToBinding(Input::kPadAxisBindingCount) < 0);
    CHECK(InputActionsPanel::padAxisToBinding(1000) < 0);
}

// La digitalización del eje: umbral con histéresis (activa a 0.5, suelta a 0.4)
// para que un stick parado justo en el borde no escupa un flanco por frame.
static void testPadAxisHysteresis()
{
    const int right = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_X, false);
    CHECK(!Input::padAxisActive(right, 0.30f, false));   // por debajo del umbral
    CHECK(Input::padAxisActive(right, 0.60f, false));    // lo cruza
    CHECK(Input::padAxisActive(right, 0.45f, true));     // zona muerta: sigue activo
    CHECK(!Input::padAxisActive(right, 0.35f, true));    // baja del de suelta
    // Empujar al lado contrario no activa esta dirección.
    CHECK(!Input::padAxisActive(right, -0.90f, false));

    const int left = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_X, true);
    CHECK(Input::padAxisActive(left, -0.60f, false));
    CHECK(!Input::padAxisActive(left, 0.60f, false));
}

// GLFW da los gatillos en [-1, 1] con el reposo en -1, no en 0: sin
// renormalizar a [0, 1] haría falta apretar el gatillo al 75% para llegar al
// umbral, y en reposo el valor quedaría a un pelo de activarse.
static void testPadTriggerRange()
{
    const int l2 = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, false);
    CHECK(!Input::padAxisActive(l2, -1.00f, false));   // reposo
    CHECK(!Input::padAxisActive(l2, -1.00f, true));    // y suelta si venía pulsado
    CHECK(!Input::padAxisActive(l2, -0.20f, false));   // 40% de recorrido
    CHECK(Input::padAxisActive(l2, 0.20f, false));     // 60%: cruza el umbral
    CHECK(Input::padAxisActive(l2, 1.00f, false));     // a fondo

    // El gatillo en negativo no existe como binding y nunca puede activarse.
    const int l2neg = Input::padAxisCode(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, true);
    CHECK(!Input::padAxisActive(l2neg, -1.00f, false));
    CHECK(!Input::padAxisActive(l2neg, 1.00f, false));
}

// Sin mando conectado, Input::update no toca los ejes y todo consulta a false:
// una acción bindeada a un stick no puede dispararse sola en un PC sin mando.
static void testPadAxisWithoutGamepad()
{
    for (int c = 0; c < Input::kPadAxisBindingCount; ++c)
    {
        CHECK(!Input::isPadAxisDown(c));
        CHECK(!Input::isPadAxisPressed(c));
    }
    CHECK(!Input::isPadAxisDown(-1));
    CHECK(!Input::isPadAxisDown(Input::kPadAxisBindingCount));
}

// H5 de docs/core-audit.md: takeActionDiagnostics() devolvia SIEMPRE una lista
// vacia. El canal existe —el header lo promete y ScriptBindings.cpp:501 lo
// vuelca al Log— pero nadie escribia nunca en el: los dos sitios que descartan
// un binding al cargar (codigo de eje fuera de rango y dispositivo desconocido)
// lo hacian con un `continue` mudo.
//
// Lo que costaba: una accion que no dispara nunca porque su binding se descarto
// al cargar es indistinguible de una accion mal configurada.
//
// El fichero vive en el directorio de trabajo y es EL MISMO que usa el editor,
// asi que el test respalda el del usuario y lo devuelve al terminar.
static void testActionDiagnosticsReportDiscardedBindings()
{
    const char* kFile = "input_actions.json";
    std::string previo;
    bool habia = false;
    {
        std::ifstream in(kFile, std::ios::binary);
        if (in)
        {
            habia = true;
            previo.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }

    {
        std::ofstream out(kFile);
        out << "{\"actions\":[{\"name\":\"Saltar\",\"glfw\":["
               "{\"device\":\"volante\",\"code\":3},"
               "{\"device\":\"padaxis\",\"code\":999},"
               "{\"device\":\"key\",\"code\":32}]}]}";
    }

    Input::reloadActions();
    std::vector<std::string> avisos = Input::takeActionDiagnostics();

    // Dos descartes: el dispositivo que no existe y el eje fuera de rango.
    CHECK(avisos.size() == 2);
    int nombran = 0;
    for (const std::string& a : avisos)
        if (a.find("Saltar") != std::string::npos) ++nombran;
    CHECK(nombran == 2);

    // Se vacia al leerla: el consumidor los vuelca al Log una sola vez.
    CHECK(Input::takeActionDiagnostics().empty());

    // Y la accion sigue existiendo con su binding bueno: descartar uno malo no
    // se lleva por delante los demas.
    CHECK(Input::hasAction("Saltar"));
    CHECK(Input::isActionDown("Saltar") == false);   // sin ventana, sin teclas

    if (habia)
    {
        std::ofstream out(kFile, std::ios::binary);
        out << previo;
    }
    else
    {
        std::remove(kFile);
    }
}

int main()
{
    testPadRoundTrip();
    testPadOutOfRange();
    testKeyboardAndMouseStillTranslate();
    testPadAxisDirections();
    testPadAxisRoundTrip();
    testPadAxisOutOfRange();
    testPadAxisHysteresis();
    testPadTriggerRange();
    testPadAxisWithoutGamepad();
    testActionDiagnosticsReportDiscardedBindings();

    if (g_failures == 0) std::printf("input_actions_tests: OK\n");
    else                 std::printf("input_actions_tests: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
