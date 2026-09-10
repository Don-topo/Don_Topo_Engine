#include "DonTopo/Core/Window.h"
#include <GLFW/glfw3.h>
#include <stb_image.h>
#include <cstdio>
#include <stdexcept>

namespace DonTopo {

Window::~Window() {
    shutdown();
}

namespace {
    // Cuántas Window tienen ventana viva. GLFW se inicializa una vez por proceso
    // y glfwTerminate() lo apaga PARA TODO EL PROCESO —destruyendo de paso las
    // ventanas de las demás—, así que el shutdown de UNA instancia no puede
    // llamarlo mientras quede otra: hacerlo convertía a Window en una clase que
    // solo puede existir una vez, cosa que ni el nombre ni el header dicen.
    //
    // int a secas y no atomic: GLFW exige que todas estas llamadas ocurran en el
    // hilo principal, así que este contador vive donde no hay concurrencia.
    int g_ventanasVivas = 0;
}

void Window::init(int width, int height, const char* title, const char* iconPath,
                  bool showOnInit) {
    // init() sobre una instancia que ya tiene ventana se llevaba la anterior por
    // delante sin destruirla: el handle se perdía al sobrescribir m_window.
    if (m_window)
        shutdown();

    // glfwInit() es idempotente por contrato (una segunda llamada con GLFW ya
    // inicializado devuelve true sin hacer nada), así que llamarlo por instancia
    // es correcto; lo que había que arreglar es el terminate, no esto.
    if (!glfwInit())
        throw std::runtime_error("GLFW: failed to initialize");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // Vulkan, sin contexto OpenGL
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_TRUE);
    // Oculta hasta setear el icono: Windows crea la entrada de la taskbar
    // en cuanto la ventana se hace visible y cachea ese icono inicial —
    // si glfwSetWindowIcon se llama después de que la ventana ya es
    // visible, la barra de título se actualiza (responde a WM_SETICON en
    // cualquier momento) pero la taskbar no siempre refresca.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!m_window) {
        // Solo se apaga GLFW si esta instancia era la única interesada: con otra
        // ventana viva, terminar aquí la mataría por un fallo que no es suyo.
        if (g_ventanasVivas == 0)
            glfwTerminate();
        throw std::runtime_error("GLFW: failed to create window");
    }
    ++g_ventanasVivas;

    if (iconPath) {
        int w, h, channels;
        unsigned char* pixels = stbi_load(iconPath, &w, &h, &channels, STBI_rgb_alpha);
        if (pixels) {
            GLFWimage image{ w, h, pixels };
            // count=1: un solo tamaño: GLFW/Windows escala esa imagen para
            // ICON_SMALL (barra de título) e ICON_BIG (barra de tareas).
            glfwSetWindowIcon(m_window, 1, &image);
            stbi_image_free(pixels);
        } else {
            std::fprintf(stderr, "Window: no se pudo cargar el icono '%s'\n", iconPath);
        }
    }

    // Con showOnInit=false la ventana se queda oculta: la enseña el caller con
    // show() tras presentar su primer frame, para que lo primero que se vea sea
    // ese frame y no el fondo blanco por defecto de la ventana.
    if (showOnInit)
        glfwShowWindow(m_window);
}

void Window::show() const {
    if (m_window)
        glfwShowWindow(m_window);
}

void Window::shutdown() {
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
        // La última apaga la luz. Ver g_ventanasVivas.
        if (--g_ventanasVivas == 0)
            glfwTerminate();
    }
}

bool Window::shouldClose() const {
    // Sin ventana, cerrar. Y no es solo por educación: los dos hosts giran en
    // `while (!window.shouldClose())`, así que devolver false aquí sería girar
    // para siempre sobre una ventana que no existe. Además glfwWindowShouldClose
    // con nullptr no devuelve un error blando: ASSERTA (window.c: `window !=
    // NULL`), o sea aborta el proceso en un build de depuración.
    return !m_window || glfwWindowShouldClose(m_window);
}

void Window::pollEvents() const {
    // Guarda por SIMETRÍA con las de arriba, y con menos derecho que ellas:
    // glfwPollEvents sin GLFW inicializado NO asserta como glfwWindowShouldClose
    // —comprobado quitándola: los tests siguen pasando—, solo emite un
    // GLFW_NOT_INITIALIZED por llamada. Se queda porque el bucle de un host que
    // ya cerró su ventana lo llamaría por frame, que es la misma lluvia de
    // errores que se acaba de quitar de los códigos de botón (H10); pero no se
    // cuenta como arreglo de un fallo, porque no lo es.
    if (m_window)
        glfwPollEvents();
}

} // namespace DonTopo
