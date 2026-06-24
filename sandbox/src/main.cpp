#include "DonTopo/Engine.h"
#include "DonTopo/Window.h"
#include "DonTopo/Renderer.h"
#include <iostream>

int main()
{
    try {
        DonTopo::Engine engine;
        DonTopo::Window window;
        window.init(1280, 720, "Don Topo Engine");
        DonTopo::Renderer renderer;

        renderer.init(window);

        while (!window.shouldClose())
        {
            renderer.drawFrame();
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
