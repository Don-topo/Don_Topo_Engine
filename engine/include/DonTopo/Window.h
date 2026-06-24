#pragma once

struct GLFWwindow;

namespace DonTopo {

class Window {
public:
    Window() = default;
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    void init(int width, int height, const char* title);
    void shutdown();

    bool shouldClose() const;
    void pollEvents() const;

    GLFWwindow* getNativeWindow() const { return m_window; }

private:
    GLFWwindow* m_window = nullptr;
};

} // namespace DonTopo
