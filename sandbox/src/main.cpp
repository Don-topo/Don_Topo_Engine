#include "DonTopo/Engine.h"
#include "DonTopo/Window.h"
#include "DonTopo/Renderer.h"
#include "DonTopo/ModelLoader.h"
#include "DonTopo/Camera.h"
#include "DonTopo/SceneNode.h"
#include <GLFW/glfw3.h>
#include <chrono>
#include <iostream>
#include <glm/gtc/matrix_transform.hpp>

int main()
{
    try {
        DonTopo::Engine engine;
        DonTopo::Window window;
        window.init(1280, 720, "Don Topo Engine");
        DonTopo::Renderer renderer;
        std::vector<DonTopo::Mesh> meshes;
        meshes.push_back(DonTopo::ModelLoader::load("assets/modelTexture.fbx"));
        meshes.push_back(DonTopo::ModelLoader::load("assets/model.fbx"));
        DonTopo::Camera camera({0.0f, 90.0f, 300.0f});
        renderer.init(window, meshes);

        // Scene node
        DonTopo::SceneNode root;
        auto* soldier = root.addChild("soldier", 0);
        auto* model = root.addChild("model", 1);
        model->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(200.0f, 0.0f, 0.0f));

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

            // SceneNode
            root.updateWorldTransforms();
            root.traverse([&](DonTopo::SceneNode* node) {
                if(node->meshIndex >= 0)
                {
                    renderer.setTransform(node->meshIndex, node->worldTransform);
                }
            });
            
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
