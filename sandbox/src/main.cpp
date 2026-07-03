#include "DonTopo/Engine.h"
#include "DonTopo/Window.h"
#include "DonTopo/Renderer.h"
#include "DonTopo/ModelLoader.h"
#include "DonTopo/Camera.h"
#include "DonTopo/SceneNode.h"
#include "DonTopo/AudioManager.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <chrono>
#include <iostream>
#include <limits>
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

        // Suelo
        {
            float floorY = std::numeric_limits<float>::max();
            for (auto& mesh : meshes)
                for (auto& v : mesh.vertices)
                    floorY = std::min(floorY, v.pos.y);

            DonTopo::Mesh floor;
            float s = 1000.0f;
            DonTopo::Vertex v0, v1, v2, v3;
            for (auto* v : {&v0,&v1,&v2,&v3}) {
                v->color   = {0.6f, 0.6f, 0.6f};
                v->normal  = {0.0f, 1.0f, 0.0f};
                v->tangent = {1.0f, 0.0f, 0.0f};
            }
            v0.pos={-s,floorY,-s}; v0.uv={0,0};
            v1.pos={ s,floorY,-s}; v1.uv={10,0};
            v2.pos={ s,floorY, s}; v2.uv={10,10};
            v3.pos={-s,floorY, s}; v3.uv={0,10};
            floor.name     = "floor";
            floor.vertices = {v0,v1,v2,v3};
            floor.indices  = {0,2,1, 0,3,2};
            meshes.push_back(floor);
        }

        // Cargar modelo animado antes de init
        auto skinnedMesh = DonTopo::ModelLoader::loadSkinned("assets/modelAnimation.fbx");

        DonTopo::Camera camera({0.0f, 90.0f, 300.0f});

        DonTopo::AudioManager audio;
        audio.init();
        int bgm = audio.loadBGM("assets/audio.mp3");
        if (bgm >= 0) audio.playBGM(bgm);

        renderer.init(window, meshes);

        // Añadir skinned mesh después de init
        int animIdx = renderer.addSkinnedMesh(skinnedMesh);
        renderer.setSkinnedTransform(animIdx,
            glm::translate(glm::mat4(1.0f), glm::vec3(-200.0f, 0.0f, 0.0f)));

        renderer.setLights({
            { glm::vec4(0.0f, 500.0f, 300.0f, 1.0f),    glm::vec4(1.0f, 0.95f, 0.8f, 1.0f) },
            { glm::vec4(-300.0f, 200.0f, -200.0f, 1.0f), glm::vec4(0.4f, 0.5f, 1.0f, 0.8f) },
        });

        DonTopo::SceneNode root;
        auto* soldier  = root.addChild("soldier", 0);
        auto* model    = root.addChild("model", 1);
        model->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(200.0f, 0.0f, 0.0f));
        auto* floor_node = root.addChild("floor", 2);

        struct AppCtx { DonTopo::Camera* cam; DonTopo::Renderer* rnd; };
        AppCtx ctx{ &camera, &renderer };
        glfwSetWindowUserPointer(window.getNativeWindow(), &ctx);

        glfwSetFramebufferSizeCallback(window.getNativeWindow(), [](GLFWwindow* w, int, int) {
            static_cast<AppCtx*>(glfwGetWindowUserPointer(w))->rnd->notifyResize();
        });

        // Cámara: solo rota con botón derecho y cuando ImGui no captura el ratón
        glfwSetCursorPosCallback(window.getNativeWindow(), [](GLFWwindow* w, double x, double y) {
            ImGui_ImplGlfw_CursorPosCallback(w, x, y);
            static double lastX = x, lastY = y;
            double dx = x - lastX, dy = y - lastY;
            lastX = x; lastY = y;
            auto* ctx = static_cast<AppCtx*>(glfwGetWindowUserPointer(w));
            if (ctx->rnd->isViewportHovered() &&
                glfwGetMouseButton(w, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
            {
                ctx->cam->processMouse((float)dx, (float)dy);
            }
        });

        // Reenviar mouse buttons y scroll a ImGui
        glfwSetMouseButtonCallback(window.getNativeWindow(), [](GLFWwindow* w, int btn, int action, int mods) {
            ImGui_ImplGlfw_MouseButtonCallback(w, btn, action, mods);
        });
        glfwSetScrollCallback(window.getNativeWindow(), [](GLFWwindow* w, double xoff, double yoff) {
            ImGui_ImplGlfw_ScrollCallback(w, xoff, yoff);
        });
        glfwSetCharCallback(window.getNativeWindow(), [](GLFWwindow* w, unsigned int c) {
            ImGui_ImplGlfw_CharCallback(w, c);
        });

        glfwSetKeyCallback(window.getNativeWindow(), [](GLFWwindow* w, int key, int scancode, int action, int mods) {
            ImGui_ImplGlfw_KeyCallback(w, key, scancode, action, mods);
            if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
                glfwSetWindowShouldClose(w, GLFW_TRUE);
        });

        while (!window.shouldClose())
        {
            auto now = std::chrono::high_resolution_clock::now();
            static auto last = now;
            float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            if (!ImGui::GetIO().WantCaptureKeyboard)
                camera.update(window.getNativeWindow(), dt);
            renderer.setCamera(camera);
            audio.update(camera.getPos(), camera.getFront(), camera.getUp());

            renderer.updateAnimation(animIdx, dt);

            root.updateWorldTransforms();
            root.traverse([&](DonTopo::SceneNode* node) {
                if (node->meshIndex >= 0)
                    renderer.setTransform(node->meshIndex, node->worldTransform);
            });

            renderer.drawFrame(window);
            window.pollEvents();
        }

        audio.shutdown();
        renderer.shutdown();
        window.shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    return 0;
}