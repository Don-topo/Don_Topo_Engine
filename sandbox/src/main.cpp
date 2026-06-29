#include "DonTopo/Engine.h"
#include "DonTopo/Window.h"
#include "DonTopo/Renderer.h"
#include "DonTopo/ModelLoader.h"
#include "DonTopo/Camera.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <iostream>

int main()
{
    try {
        DonTopo::Engine engine;
        DonTopo::Window window;
        window.init(1280, 720, "Don Topo Engine");
        DonTopo::Renderer renderer;
        DonTopo::Mesh mesh = DonTopo::ModelLoader::load("assets/model.fbx");
        DonTopo::Camera camera({0.0f, 90.0f, 300.0f});
        renderer.init(window, mesh);

        glfwSetInputMode(window.getNativeWindow(), GLFW_CURSOR, GLFW_CURSOR_DISABLED);

        struct AppCtx { DonTopo::Camera* cam; DonTopo::Renderer* rnd; };
        AppCtx ctx{ &camera, &renderer };
        glfwSetWindowUserPointer(window.getNativeWindow(), &ctx);

        glfwSetFramebufferSizeCallback(window.getNativeWindow(), [](GLFWwindow* w, int, int) {
            static_cast<AppCtx*>(glfwGetWindowUserPointer(w))->rnd->notifyResize();
        });
        glfwSetCursorPosCallback(window.getNativeWindow(), [](GLFWwindow* w, double x, double y) {
            static double lastX = x, lastY = y;
            static_cast<AppCtx*>(glfwGetWindowUserPointer(w))->cam->processMouse(
                (float)(x - lastX), (float)(y - lastY));
            lastX = x; lastY = y;
        });
        glfwSetKeyCallback(window.getNativeWindow(), [](GLFWwindow* w, int key, int, int action, int) {
            if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
                glfwSetWindowShouldClose(w, GLFW_TRUE);
        });

        while (!window.shouldClose())
        {
            auto now = std::chrono::high_resolution_clock::now();
            static auto last = now;
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            camera.update(window.getNativeWindow(), dt);
            renderer.setCamera(camera);

            renderer.drawFrame(window);
            window.pollEvents();
        }            
        renderer.shutdown();
        window.shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return 0;
}
