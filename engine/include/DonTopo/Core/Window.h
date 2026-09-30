#pragma once

struct GLFWwindow;

namespace DonTopo {

class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    // iconPath: path to a PNG (RGBA or not) for the window icon and the
    // taskbar icon (glfwSetWindowIcon). nullptr = system default
    // icon.
    //
    // showOnInit=false leaves the window HIDDEN on return: the caller shows it
    // with show() when it has something to draw. It is the remedy GLFW recommends
    // for the startup "white flash": between the window becoming visible and the
    // first frame being presented, Windows paints the client area with the
    // default background (white). The runtime takes ~520ms to bring up Vulkan,
    // so that white is plainly visible. The editor uses the default (true) and
    // does not change.
    void init(int width, int height, const char* title, const char* iconPath = nullptr,
              bool showOnInit = true);
    void shutdown();

    // Makes the window visible. Idempotent (glfwShowWindow is). Only needed
    // if init was called with showOnInit=false.
    void show() const;

    bool shouldClose() const;
    void pollEvents() const;

    GLFWwindow* getNativeWindow() const { return m_window; }

private:
    GLFWwindow* m_window = nullptr;
};

} // namespace DonTopo
