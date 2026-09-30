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
    // How many Windows have a live window. GLFW is initialized once per process
    // and glfwTerminate() shuts it down FOR THE WHOLE PROCESS —destroying along the way the
    // windows of the others—, so the shutdown of ONE instance cannot
    // call it while another remains: doing so turned Window into a class that
    // can only exist once, which neither the name nor the header say.
    //
    // Plain int and not atomic: GLFW requires all these calls to happen on the
    // main thread, so this counter lives where there is no concurrency.
    int g_ventanasVivas = 0;
}

void Window::init(int width, int height, const char* title, const char* iconPath,
                  bool showOnInit) {
    // init() on an instance that already has a window used to run over the previous one
    // without destroying it: the handle was lost when overwriting m_window.
    if (m_window)
        shutdown();

    // glfwInit() is idempotent by contract (a second call with GLFW already
    // initialized returns true without doing anything), so calling it per instance
    // is correct; what had to be fixed was the terminate, not this.
    if (!glfwInit())
        throw std::runtime_error("GLFW: failed to initialize");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // Vulkan, no OpenGL context
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_TRUE);
    // Hidden until the icon is set: Windows creates the taskbar entry
    // as soon as the window becomes visible and caches that initial icon —
    // if glfwSetWindowIcon is called after the window is already
    // visible, the title bar updates (it responds to WM_SETICON at
    // any time) but the taskbar does not always refresh.
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!m_window) {
        // GLFW is only shut down if this instance was the only one interested: with another
        // live window, terminating here would kill it over a failure that is not its own.
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
            // count=1: a single size: GLFW/Windows scales that image for
            // ICON_SMALL (title bar) and ICON_BIG (taskbar).
            glfwSetWindowIcon(m_window, 1, &image);
            stbi_image_free(pixels);
        } else {
            std::fprintf(stderr, "Window: could not load the icon '%s'\n", iconPath);
        }
    }

    // With showOnInit=false the window stays hidden: the caller shows it with
    // show() after presenting its first frame, so that the first thing seen is
    // that frame and not the default white background of the window.
    //
    // On Wayland it is shown NOW, whatever the caller asks: there is no white
    // flash to avoid there (the compositor does not paint the window until it receives its
    // first buffer), and showing a window to which Vulkan already presented a
    // frame is a protocol error ("xdg_surface must not have a buffer at
    // creation") that leaves the game without a window. The exported runtime caught it on
    // WSLg; desktop Ubuntu uses Wayland by default. The caller's later show()
    // does nothing: glfwShowWindow is idempotent.
    if (showOnInit || glfwGetPlatform() == GLFW_PLATFORM_WAYLAND)
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
        // The last one turns off the light. See g_ventanasVivas.
        if (--g_ventanasVivas == 0)
            glfwTerminate();
    }
}

bool Window::shouldClose() const {
    // Without a window, close. And it is not just politeness: both hosts spin on
    // `while (!window.shouldClose())`, so returning false here would spin
    // forever on a window that does not exist. Besides, glfwWindowShouldClose
    // with nullptr does not return a soft error: it ASSERTS (window.c: `window !=
    // NULL`), that is, it aborts the process in a debug build.
    return !m_window || glfwWindowShouldClose(m_window);
}

void Window::pollEvents() const {
    // Guard for SYMMETRY with the ones above, and with less right than they have:
    // glfwPollEvents without GLFW initialized does NOT assert like glfwWindowShouldClose
    // —checked by removing it: the tests keep passing—, it only emits a
    // GLFW_NOT_INITIALIZED per call. It stays because the loop of a host that
    // already closed its window would call it every frame, which is the same rain of
    // errors that was just removed from the button codes (H10); but it does not
    // count as a fix for a failure, because it is not one.
    if (m_window)
        glfwPollEvents();
}

} // namespace DonTopo
