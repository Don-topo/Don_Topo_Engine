# Runtime Splash Screen Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Mostrar el logo del motor mientras el juego exportado (`DonTopoRuntime.exe`) carga, tapando la ventana negra de ~2 s.

**Architecture:** Splash dibujado con Vulkan (fase 1, single-thread). Se parte `Renderer::init` en `initPresentation` (lo mínimo para presentar) + `initSceneResources` (lo diferible). Nueva clase `SplashScreen` con el molde de `Skybox`. El runtime reordena el arranque e intercala frames de splash con fade in/out entre las unidades de carga.

**Tech Stack:** C++20, Vulkan, stb_image (ya presente), glslc (shaders GLSL→SPIR-V, compilados por el GLOB de `sandbox/CMakeLists.txt`).

## Global Constraints

- Build: `configure.bat`/`build.bat` (Debug, `build-ninja/`) y `configure-release.bat`/`build-release.bat` (Release, `build-ninja-release/`) vía PowerShell, nunca cmake crudo en Bash. Medir tiempos siempre en Release.
- El editor (Sandbox) NO lleva splash. Solo el runtime exportado. `Renderer::init(window, meshes)` debe seguir existiendo con comportamiento idéntico (lo llama el editor).
- El logo ausente NUNCA bloquea el arranque: sin `splash.png` el runtime arranca directo sin splash.
- Shaders nuevos van a `shaders/` como `.vert`/`.frag`; el `file(GLOB *.vert *.frag *.comp)` de `sandbox/CMakeLists.txt` los compila. Reconfigurar (`configure*.bat`) tras añadirlos para que el GLOB los recoja.
- Tests headless: plain `main()` + `CHECK` macro, sin framework, mismo patrón que `engine/tests/camera_tests.cpp`. NO crear device Vulkan en tests (frágil sin GPU): la parte Vulkan de `SplashScreen` va a verificación manual.
- Comentarios y mensajes en español, sin tildes en literales de código que se logueen crudos (coherente con el repo).

---

## File Structure

- `shaders/splash.vert` (crear) — triángulo fullscreen, emite UV.
- `shaders/splash.frag` (crear) — muestrea el logo con letterbox, multiplica por alpha (push constant).
- `engine/include/DonTopo/Renderer/SplashScreen.h` (crear) — clase `SplashScreen` + función libre `loadSplashImage`.
- `engine/src/Renderer/SplashScreen.cpp` (crear) — implementación.
- `engine/CMakeLists.txt` (modificar) — añadir `src/Renderer/SplashScreen.cpp`.
- `engine/include/DonTopo/Renderer/Renderer.h` (modificar) — `initPresentation`/`initSceneResources`/`beginSplash`/`drawSplashFrame`, miembro `m_splash`.
- `engine/src/Renderer/Renderer.cpp` (modificar) — partir `init`, implementar splash, shutdown.
- `runtime/main.cpp` (modificar) — reorden + `SplashDriver`.
- `runtime/SplashDriver.h` (crear) — cálculo puro del alpha por fase (testeable).
- `engine/src/Editor/GameExporter.cpp` (modificar) — copiar `splash.png` al paquete.
- `engine/tests/splash_tests.cpp` (crear) — test de `loadSplashImage` y `SplashDriver`.
- `engine/tests/exporter_tests.cpp` (modificar) — test de `splash.png` en el paquete.
- `engine/tests/CMakeLists.txt` (modificar) — registrar `dt_splash_tests`.

---

## Task 1: Shaders del splash

**Files:**
- Create: `shaders/splash.vert`
- Create: `shaders/splash.frag`

**Interfaces:**
- Produces: SPIR-V `shaders/splash.vert.spv` y `shaders/splash.frag.spv` tras compilar. Push constant en el fragment: `layout(push_constant) uniform Push { float alpha; vec2 imgAspect; } push;` (el vertex no usa push constant).

- [ ] **Step 1: Escribir `shaders/splash.vert`**

```glsl
#version 450

// Fullscreen triangle — sin VBO, 3 vértices cubren la pantalla. Mismo patrón
// que skybox.vert. Emite UV en [0,1] para muestrear la textura del logo.
const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

layout(location = 0) out vec2 outUV;

void main() {
    vec2 pos    = positions[gl_VertexIndex];
    gl_Position = vec4(pos, 0.0, 1.0);
    // pos en [-1,3] -> UV en [0,2] (el triángulo desborda; el recorte a
    // pantalla deja UV en [0,1] visible). UV.y sin voltear: la textura se sube
    // tal cual y el letterbox se calcula en el fragment.
    outUV = pos * 0.5 + 0.5;
}
```

- [ ] **Step 2: Escribir `shaders/splash.frag`**

```glsl
#version 450

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D uLogo;

// Tres floats sueltos (no vec2) para evitar el padding std430 de vec2 a 8
// bytes: el C++ envia { float alpha; float imgAR; float screenAR; } = 12 bytes
// contiguos (ver SplashScreen::recordDraw, Task 3).
layout(push_constant) uniform Push {
    float alpha;     // fade [0,1]
    float imgAR;     // logoW/logoH
    float screenAR;  // screenW/screenH
} push;

// Color de fondo del splash (gris muy oscuro, no negro puro para que el fade
// se aprecie sobre la ventana).
const vec3 kBg = vec3(0.05, 0.05, 0.06);

void main() {
    // Letterbox: encajar el logo manteniendo su aspect ratio dentro de la
    // pantalla. scale = ratio pantalla / ratio logo por eje.
    float logoAR   = push.imgAR;
    float screenAR = push.screenAR;

    vec2 uv = inUV - 0.5;
    if (screenAR > logoAR) uv.x *= screenAR / logoAR; // pantalla mas ancha: barras laterales
    else                   uv.y *= logoAR / screenAR; // pantalla mas alta: barras arriba/abajo
    uv += 0.5;

    vec3 col = kBg;
    if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
        vec4 logo = texture(uLogo, uv);
        col = mix(kBg, logo.rgb, logo.a); // respeta el alpha del PNG del logo
    }
    outColor = vec4(col * push.alpha, 1.0); // fade a negro-fondo via alpha uniforme
}
```

- [ ] **Step 3: Reconfigurar y compilar (Release)**

Run (PowerShell): `.\configure-release.bat` luego `.\build-release.bat`
Expected: sin errores; aparecen `build-ninja-release/spv/splash.vert.spv` y `splash.frag.spv`, y se copian a `shaders/`.

- [ ] **Step 4: Verificar que los .spv existen**

Run (PowerShell): `Test-Path shaders\splash.vert.spv, shaders\splash.frag.spv`
Expected: `True` `True`

- [ ] **Step 5: Commit**

```bash
git add shaders/splash.vert shaders/splash.frag
git commit -m "feat(splash): shaders del splash (fullscreen quad + letterbox + fade)"
```

---

## Task 2: `loadSplashImage` — carga pura del PNG (testeable sin GPU)

**Files:**
- Create: `engine/include/DonTopo/Renderer/SplashScreen.h`
- Create: `engine/src/Renderer/SplashScreen.cpp`
- Modify: `engine/CMakeLists.txt` (añadir la fuente)
- Create: `engine/tests/splash_tests.cpp`
- Modify: `engine/tests/CMakeLists.txt`

**Interfaces:**
- Produces: `bool DonTopo::loadSplashImage(const std::string& path, std::vector<uint8_t>& outRGBA, int& outW, int& outH);` — carga un PNG a RGBA8. Devuelve `false` (sin tocar los out) si el fichero no existe o no decodifica. Es la garantía testeable de "logo ausente no bloquea".

- [ ] **Step 1: Escribir el test (falla porque no existe la función)**

En `engine/tests/splash_tests.cpp`:

```cpp
// Test headless del splash: la carga del PNG (sin GPU) y el calculo puro del
// alpha por fase. La parte Vulkan de SplashScreen NO se testea aqui (requiere
// device, fragil sin GPU): va a verificacion manual.
#include "DonTopo/Renderer/SplashScreen.h"

#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Un PNG real del repo carga a RGBA con dimensiones > 0.
static void test_load_valid_png()
{
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    CHECK(loadSplashImage("assets/MainEngineLogo.png", rgba, w, h));
    CHECK(w > 0);
    CHECK(h > 0);
    CHECK(rgba.size() == (size_t)w * h * 4);
}

// Un fichero inexistente devuelve false sin tocar los out (garantia de "logo
// ausente no bloquea el arranque").
static void test_missing_png_returns_false()
{
    std::vector<uint8_t> rgba;
    int w = -1, h = -1;
    CHECK(!loadSplashImage("assets/no_existe_splash.png", rgba, w, h));
    CHECK(rgba.empty());
}

int main()
{
    test_load_valid_png();
    test_missing_png_returns_false();
    if (g_failures == 0) std::printf("ALL SPLASH TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Escribir el header `SplashScreen.h` (solo lo necesario para Task 2)**

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <cstdint>

namespace DonTopo {

class GpuDevice;

// Carga un PNG a RGBA8 (4 canales). false si no existe o no decodifica, sin
// tocar los parametros de salida. Funcion libre (sin Vulkan) para poder
// testear la garantia de "logo ausente no bloquea" sin device.
bool loadSplashImage(const std::string& path, std::vector<uint8_t>& outRGBA,
                     int& outW, int& outH);

class SplashScreen {
public:
    SplashScreen()                               = default;
    SplashScreen(const SplashScreen&)            = delete;
    SplashScreen& operator=(const SplashScreen&) = delete;

    // Sube el logo y crea el pipeline sobre renderPass. false si el logo no
    // carga (el caller se salta el splash). No lanza por logo ausente.
    bool init(GpuDevice& gpu, VkRenderPass renderPass, VkFormat colorFormat,
              const std::string& logoPath);
    void shutdown(GpuDevice& gpu);

    // Graba el draw del splash. alpha [0,1] para el fade; screenAspect =
    // ancho/alto de la ventana (para el letterbox).
    void recordDraw(VkCommandBuffer cmd, float alpha, float screenAspect);

    bool isInitialized() const { return m_pipeline != VK_NULL_HANDLE; }

private:
    void createDescriptors(GpuDevice& gpu);
    void createPipeline(GpuDevice& gpu, VkRenderPass renderPass);

    int                   m_logoW      = 0;
    int                   m_logoH      = 0;
    VkImage               m_image      = VK_NULL_HANDLE;
    VkDeviceMemory        m_memory     = VK_NULL_HANDLE;
    VkImageView           m_view       = VK_NULL_HANDLE;
    VkSampler             m_sampler    = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      m_descPool   = VK_NULL_HANDLE;
    VkDescriptorSet       m_descSet    = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipeLayout = VK_NULL_HANDLE;
    VkPipeline            m_pipeline   = VK_NULL_HANDLE;
};

} // namespace DonTopo
```

- [ ] **Step 3: Implementar `loadSplashImage` en `SplashScreen.cpp` (mínimo para Task 2)**

```cpp
#include "DonTopo/Renderer/SplashScreen.h"
#include <stb_image.h>

namespace DonTopo {

bool loadSplashImage(const std::string& path, std::vector<uint8_t>& outRGBA,
                     int& outW, int& outH)
{
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!px) return false;
    outRGBA.assign(px, px + (size_t)w * h * 4);
    outW = w;
    outH = h;
    stbi_image_free(px);
    return true;
}

} // namespace DonTopo
```

- [ ] **Step 4: Añadir la fuente a `engine/CMakeLists.txt`**

Tras la línea `src/Renderer/Skybox.cpp` (o junto a las demás fuentes de Renderer), añadir:

```cmake
    src/Renderer/SplashScreen.cpp
```

- [ ] **Step 5: Registrar el test en `engine/tests/CMakeLists.txt`**

Siguiendo el patrón de las demás (`add_executable` + link). Añadir tras `dt_scripting_tests`:

```cmake
add_executable(dt_splash_tests splash_tests.cpp)
target_link_libraries(dt_splash_tests PRIVATE DonTopoEngine)
target_compile_features(dt_splash_tests PRIVATE cxx_std_20)
```

Además, `dt_splash_tests` enlaza `DonTopoEngine` (arrastra FMOD), así que hay que añadirlo al `foreach` que copia `fmod.dll` junto a cada test (líneas ~37): sin eso, el `.exe` muere con `STATUS_ENTRYPOINT_NOT_FOUND` al ejecutarlo. Cambiar la lista del `foreach(_dt_test_target ...)` para incluir `dt_splash_tests` al final.

- [ ] **Step 6: Configurar y compilar (Debug)**

Run (PowerShell): `.\configure.bat` luego `.\build.bat`
Expected: compila `dt_splash_tests.exe` sin errores.

- [ ] **Step 7: Ejecutar el test desde la raíz del repo (para que `assets/` resuelva)**

Run (PowerShell): `build-ninja\engine\tests\dt_splash_tests.exe`
Expected: `ALL SPLASH TESTS PASSED`, exit 0.
Nota: el test abre `assets/MainEngineLogo.png` relativo al CWD; ejecutar desde la raíz del repo.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/Renderer/SplashScreen.h engine/src/Renderer/SplashScreen.cpp engine/CMakeLists.txt engine/tests/splash_tests.cpp engine/tests/CMakeLists.txt
git commit -m "feat(splash): loadSplashImage con test headless (logo ausente no bloquea)"
```

---

## Task 3: `SplashScreen` — pipeline y draw (Vulkan)

**Files:**
- Modify: `engine/src/Renderer/SplashScreen.cpp`

**Interfaces:**
- Consumes: `loadSplashImage` (Task 2); `GpuDevice::device()`, `findMemoryType`, `beginOneTimeCommands`, `endOneTimeCommands`, `graphicsQueue` (existentes).
- Produces: `SplashScreen::init/shutdown/recordDraw` implementados. Push constant en el fragment: `struct { float alpha; float imgAR; float screenAR; }` (12 bytes, `VK_SHADER_STAGE_FRAGMENT_BIT`).

- [ ] **Step 1: Implementar la subida de textura + `init` + `shutdown` + `recordDraw`**

Añadir a `SplashScreen.cpp` (calcar el molde de `Skybox::loadCubemap`/`createDescriptors`/`createPipeline` en `engine/src/Renderer/Skybox.cpp`, adaptado a textura 2D de una capa). Puntos clave a adaptar respecto a Skybox:
- Imagen 2D normal: sin `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT`, `arrayLayers=1`, view `VK_IMAGE_VIEW_TYPE_2D`, subresource `layerCount=1`.
- `init` usa `loadSplashImage`; si devuelve `false`, `return false` ANTES de crear nada Vulkan (no deja recursos a medias).
- Descriptor: igual que Skybox (combined image sampler, binding 0, fragment stage).
- Pipeline: calcar `Skybox::createPipeline` con estos cambios:
  - shaders `shaders/splash.vert.spv` / `shaders/splash.frag.spv`.
  - Sin depth (`pDepthStencilState = nullptr`): el splash no usa depth. El render pass del splash (Task 5) no tendrá attachment de depth.
  - Push constant: `stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT`, `size = 3*sizeof(float)`.
  - Blend: opaco (el alpha se aplica en el shader multiplicando el color; el framebuffer es opaco).
- `recordDraw`:

```cpp
void SplashScreen::recordDraw(VkCommandBuffer cmd, float alpha, float screenAspect)
{
    if (!isInitialized()) return;
    struct Push { float alpha; float imgAR; float screenAR; } push{
        alpha,
        m_logoH > 0 ? (float)m_logoW / (float)m_logoH : 1.0f,
        screenAspect
    };
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipeLayout, 0, 1, &m_descSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_pipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(Push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
```
- `shutdown`: calcar `Skybox::shutdown` (destruir pipeline, layout, descPool, descLayout, sampler, view, image, memory; guard `if (m_pipeline == VK_NULL_HANDLE) return;`).

Nota de consistencia con el push constant: el `shaders/splash.frag` de Task 1 ya declara el push como tres floats sueltos (`{ float alpha; float imgAR; float screenAR; }`, 12 bytes) precisamente para casar con este `struct Push` del C++ sin el padding std430 de `vec2`. El `struct Push` de arriba debe seguir siendo tres floats en el mismo orden; NO cambiar el `.frag` (ya es correcto). Si al implementar el orden o el tipo no casan, arreglar el C++ para que case con el shader de Task 1, no al revés.

- [ ] **Step 2: Compilar (Debug) para verificar que `SplashScreen.cpp` enlaza**

Run (PowerShell): `.\build.bat`
Expected: `DonTopoEngine` y los tests compilan y enlazan sin errores. (No hay test automático de la parte Vulkan: se valida al usarla en el runtime, Task 7, y en verificación manual.)

- [ ] **Step 4: Commit**

```bash
git add engine/src/Renderer/SplashScreen.cpp shaders/splash.frag
git commit -m "feat(splash): pipeline y draw de SplashScreen (molde Skybox, textura 2D + fade)"
```

---

## Task 4: Partir `Renderer::init` en `initPresentation` + `initSceneResources`

**Files:**
- Modify: `engine/include/DonTopo/Renderer/Renderer.h`
- Modify: `engine/src/Renderer/Renderer.cpp:27-81` (el cuerpo de `init`)

**Interfaces:**
- Produces: `void Renderer::initPresentation(Window&)` y `void Renderer::initSceneResources(const std::vector<Mesh>&)`. `void Renderer::init(Window&, const std::vector<Mesh>&)` sigue existiendo y llama a las dos en orden, sin cambio de comportamiento.

- [ ] **Step 1: Declarar las dos fases en `Renderer.h`**

Junto a `void init(Window& window, const std::vector<Mesh>& meshes);` (línea 33) añadir:

```cpp
            // Fase 1 del arranque: lo minimo para presentar (device, swapchain,
            // render pass, framebuffers, command buffers, sync). No crea
            // pipelines de escena. La usa el runtime para poder dibujar el
            // splash antes de la carga pesada. init() la llama primero.
            void initPresentation(Window& window);
            // Fase 2: pipelines PBR/shadow/compute, offscreen, descriptor sets,
            // subida de mallas estaticas. init() la llama despues.
            void initSceneResources(const std::vector<Mesh>& meshes);
```

- [ ] **Step 2: Partir el cuerpo de `init` en `Renderer.cpp`**

Sustituir `Renderer::init` (líneas 27-81) por las tres funciones. `initPresentation` contiene: auto-fit camera, `m_gpu.init`, `createSwapChain`, `createImageViews`, `createDepthResources`, `createOffscreenRenderPass`, `Gizmos::init`, `createRenderPass`, `createFramebuffers`, `createCommandBuffers`, `createSyncObjects`. `initSceneResources` contiene: `createDescriptorSetLayout`, `createPipeline`, `createShadowResources`, `createComputePipelines`, `initImGui` (si no headless), `createOffscreenImages`, el bucle `buildRenderObject`, `createUniformBuffers`, `createDescriptorPool`, `createDescriptorSets`. `init` = las dos en orden.

IMPORTANTE — orden y dependencias: el orden original (líneas 53-81) debe preservarse dentro de la unión de las dos fases. Concretamente `createCommandBuffers`/`createSyncObjects` estaban al final; moverlos a `initPresentation` los adelanta. Verificar que no dependen de nada creado en `initSceneResources` (command buffers y sync objects solo dependen del device y del pool de comandos del device, no de pipelines). Si `createCommandBuffers` depende de `m_swapChainFramebuffers.size()`, ya está creado en `initPresentation`. Dejar un comentario explicando el reparto.

```cpp
    void Renderer::init(Window& window, const std::vector<Mesh>& meshes)
    {
        // Se mantiene como la suma de las dos fases, en el mismo orden que antes.
        // El editor (Sandbox) llama a init() y no cambia; el runtime llama a las
        // dos fases por separado para colar el splash entre medias.
        initPresentation(window);
        initSceneResources(meshes);
    }

    void Renderer::initPresentation(Window& window)
    {
        static_assert(Gizmos::kFramesInFlight == MAX_FRAMES,
            "Gizmos::kFramesInFlight debe coincidir con Renderer::MAX_FRAMES");
        // (auto-fit camera se mueve a initSceneResources: depende de meshes)
        m_gpu.init(window.getNativeWindow());
        createSwapChain(window);
        createImageViews();
        createDepthResources();
        createOffscreenRenderPass();
        Gizmos::init(m_gpu, m_offscreenRenderPass, m_swapChainFormat);
        createRenderPass();
        createFramebuffers();
        createCommandBuffers();
        createSyncObjects();
    }

    void Renderer::initSceneResources(const std::vector<Mesh>& meshes)
    {
        // Auto-fit camera to mesh bounding box (necesita meshes; por eso vive
        // aqui y no en initPresentation).
        glm::vec3 bMin( std::numeric_limits<float>::max());
        glm::vec3 bMax(-std::numeric_limits<float>::max());
        for (auto& mesh : meshes)
            for (auto& v : mesh.vertices) { bMin = glm::min(bMin, v.pos); bMax = glm::max(bMax, v.pos); }
        m_cameraTarget   = (bMin + bMax) * 0.5f;
        float maxDim     = glm::max(bMax.x - bMin.x, glm::max(bMax.y - bMin.y, bMax.z - bMin.z));
        m_cameraDistance = maxDim * 1.2f;

        createDescriptorSetLayout();
        createPipeline();
        createShadowResources();
        createComputePipelines();
        if (!m_headless) initImGui(nullptr); // ver nota abajo
        createOffscreenImages();

        m_objects.resize(meshes.size());
        for (size_t i = 0; i < meshes.size(); i++)
            buildRenderObject(meshes[i], m_objects[i]);

        createUniformBuffers();
        createDescriptorPool();
        createDescriptorSets();
    }
```

Nota sobre `initImGui(window.getNativeWindow())`: `initImGui` necesitaba el `GLFWwindow*`. `initSceneResources` no recibe `Window&`. Opciones: (a) guardar `m_window`/`GLFWwindow*` como miembro en `initPresentation` y usarlo en `initImGui`; (b) mover `initImGui` a `initPresentation` (necesita `m_renderPass` + swapchain size, ya creados ahí). Elegir (b): mover `if (!m_headless) initImGui(window.getNativeWindow());` al final de `initPresentation`, tras `createRenderPass`/`createFramebuffers`. Ajustar el pseudocódigo: quitar `initImGui` de `initSceneResources` y ponerlo en `initPresentation`. Verificar que `createOffscreenImages` (que en editor necesita ImGui inicializado, comentario en el código original línea 68) sigue después de `initImGui` — como `initImGui` pasa a `initPresentation` y `createOffscreenImages` queda en `initSceneResources`, el orden ImGui→offscreen se mantiene entre fases. Correcto.

- [ ] **Step 3: Compilar Debug y correr TODA la suite (el editor no debe cambiar)**

Run (PowerShell): `.\build.bat` luego `Get-ChildItem build-ninja\engine\tests\dt_*.exe | ForEach-Object { & $_.FullName; Write-Output "$($_.Name) exit=$LASTEXITCODE" }`
Expected: los 8 tests (7 previos + `dt_splash_tests`) exit 0.

- [ ] **Step 4: Verificación manual mínima del editor**

Run (PowerShell): `build-ninja\sandbox\Sandbox.exe`
Expected: el editor arranca y renderiza igual que antes (la escena se ve, paneles OK). Cerrar.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Renderer/Renderer.h engine/src/Renderer/Renderer.cpp
git commit -m "refactor(renderer): partir init en initPresentation + initSceneResources"
```

---

## Task 5: `Renderer::beginSplash` + `drawSplashFrame`

**Files:**
- Modify: `engine/include/DonTopo/Renderer/Renderer.h` (declaraciones + miembro `m_splash`)
- Modify: `engine/src/Renderer/Renderer.cpp` (implementación + shutdown)

**Interfaces:**
- Consumes: `SplashScreen` (Tasks 2-3); miembros del swapchain/sync del Renderer (`m_swapChain`, `m_renderPass`, `m_swapChainFramebuffers`, `m_commandBuffers`, `m_imageAvailable`, `m_renderFinished`, `m_inFlight`, `m_currentFrame`, `m_swapChainExtent`).
- Produces: `bool Renderer::beginSplash(const std::string& logoPath)` (true si el splash se inicializó); `void Renderer::drawSplashFrame(float alpha)`.

- [ ] **Step 1: Declarar en `Renderer.h`**

Junto a `initSceneResources`:

```cpp
            // Inicializa el splash sobre el render pass del swapchain (requiere
            // initPresentation ya llamado). false si el logo no carga: el caller
            // se salta el splash. Solo lo llama el runtime; el editor no.
            bool beginSplash(const std::string& logoPath);
            // Presenta un frame con solo el splash a alpha [0,1]. No-op si el
            // splash no se inicializo.
            void drawSplashFrame(float alpha);
```

Y como miembro (junto a `Skybox m_skybox;` — buscar su declaración):

```cpp
            SplashScreen m_splash;
```

Añadir `#include "DonTopo/Renderer/SplashScreen.h"` en `Renderer.h` (junto al include de `Skybox.h`).

- [ ] **Step 2: Implementar `beginSplash` y `drawSplashFrame` en `Renderer.cpp`**

```cpp
    bool Renderer::beginSplash(const std::string& logoPath)
    {
        // Sobre el render pass del swapchain ya creado por initPresentation.
        // colorFormat = m_swapChainFormat. No lanza si el logo falta.
        return m_splash.init(m_gpu, m_renderPass, m_swapChainFormat, logoPath);
    }

    void Renderer::drawSplashFrame(float alpha)
    {
        if (!m_splash.isInitialized()) return;

        vkWaitForFences(m_gpu.device(), 1, &m_inFlight[m_currentFrame], VK_TRUE, UINT64_MAX);

        uint32_t imageIndex;
        VkResult res = vkAcquireNextImageKHR(m_gpu.device(), m_swapChain, UINT64_MAX,
            m_imageAvailable[m_currentFrame], VK_NULL_HANDLE, &imageIndex);
        if (res == VK_ERROR_OUT_OF_DATE_KHR) return; // durante el splash no recreamos: el siguiente frame lo hara
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) return;

        vkResetFences(m_gpu.device(), 1, &m_inFlight[m_currentFrame]);
        vkResetCommandBuffer(m_commandBuffers[m_currentFrame], 0);

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(m_commandBuffers[m_currentFrame], &bi);

        VkClearValue clear{};
        clear.color = { { 0.05f, 0.05f, 0.06f, 1.0f } }; // mismo fondo que el shader

        VkRenderPassBeginInfo rp{};
        rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass        = m_renderPass;
        rp.framebuffer       = m_swapChainFramebuffers[imageIndex];
        rp.renderArea.extent = m_swapChainExtent;
        rp.clearValueCount   = 1;   // ver nota: el render pass del swapchain puede tener 2 attachments (color+depth)
        rp.pClearValues      = &clear;
        vkCmdBeginRenderPass(m_commandBuffers[m_currentFrame], &rp, VK_SUBPASS_CONTENTS_INLINE);

        // Viewport/scissor dinamicos (el pipeline los declara dinamicos).
        VkViewport vp{ 0, 0, (float)m_swapChainExtent.width, (float)m_swapChainExtent.height, 0.0f, 1.0f };
        VkRect2D sc{ { 0, 0 }, m_swapChainExtent };
        vkCmdSetViewport(m_commandBuffers[m_currentFrame], 0, 1, &vp);
        vkCmdSetScissor(m_commandBuffers[m_currentFrame], 0, 1, &sc);

        float aspect = m_swapChainExtent.height > 0
            ? (float)m_swapChainExtent.width / (float)m_swapChainExtent.height : 1.0f;
        m_splash.recordDraw(m_commandBuffers[m_currentFrame], alpha, aspect);

        vkCmdEndRenderPass(m_commandBuffers[m_currentFrame]);
        vkEndCommandBuffer(m_commandBuffers[m_currentFrame]);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{};
        si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount   = 1;
        si.pWaitSemaphores      = &m_imageAvailable[m_currentFrame];
        si.pWaitDstStageMask    = &waitStage;
        si.commandBufferCount   = 1;
        si.pCommandBuffers      = &m_commandBuffers[m_currentFrame];
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores    = &m_renderFinished[imageIndex];
        vkQueueSubmit(m_gpu.graphicsQueue(), 1, &si, m_inFlight[m_currentFrame]);

        VkPresentInfoKHR pi{};
        pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores    = &m_renderFinished[imageIndex];
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &m_swapChain;
        pi.pImageIndices      = &imageIndex;
        vkQueuePresentKHR(m_gpu.presentQueue(), &pi);

        m_currentFrame = (m_currentFrame + 1) % MAX_FRAMES;
    }
```

NOTA crítica sobre `clearValueCount`: mirar `createRenderPass` en `Renderer.cpp` (~línea 549) y `createFramebuffers` (~558). Si el render pass del swapchain tiene attachment de depth además del color, `clearValueCount` debe ser 2 y `pClearValues` un array de 2 (`{color, depthStencil={1.0f,0}}`). Ajustar según lo que declare `createRenderPass`. Si el render pass exige que el pipeline del splash tenga depth state compatible, o bien (a) dar al splash un `pDepthStencilState` con test/write off, o (b) confirmar que el pass no tiene depth. Resolver leyendo `createRenderPass` antes de implementar; el pipeline del splash (Task 3) debe ser compatible con ESTE render pass.

- [ ] **Step 3: Liberar el splash en `Renderer::shutdown`**

Junto a `m_skybox.shutdown(m_gpu);` (línea ~271) añadir:

```cpp
        m_splash.shutdown(m_gpu);
```

- [ ] **Step 4: Compilar Debug y Release**

Run (PowerShell): `.\build.bat` luego `.\build-release.bat`
Expected: sin errores.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Renderer/Renderer.h engine/src/Renderer/Renderer.cpp
git commit -m "feat(splash): beginSplash + drawSplashFrame en Renderer"
```

---

## Task 6: `SplashDriver` — cálculo puro del alpha por fase (testeable)

**Files:**
- Create: `runtime/SplashDriver.h`
- Modify: `engine/tests/splash_tests.cpp` (añadir tests del driver)

**Interfaces:**
- Produces: en `runtime/SplashDriver.h`, header-only:
  ```cpp
  struct SplashTimings { float fadeIn = 0.3f; float minTotal = 1.5f; float fadeOut = 0.3f; };
  struct SplashState { float alpha; bool crossfading; bool done; };
  SplashState splashStateAt(const SplashTimings& t, float elapsed,
                            bool loadingDone, float loadingDoneAt);
  ```
  `elapsed` y `loadingDoneAt` en segundos desde que el splash empezó a pintar. `crossfading`=estamos en fade-out (el runtime dibuja la escena debajo). `done`=el splash terminó (alpha llegó a 0).

- [ ] **Step 1: Escribir los tests del driver (fallan: no existe el header)**

Añadir a `engine/tests/splash_tests.cpp` (incluir `"../../runtime/SplashDriver.h"` — ajustar la ruta relativa al layout real de `engine/tests/`; usar la ruta que resuelva desde ese directorio, p.ej. `#include "SplashDriver.h"` con un `target_include_directories` al dir de runtime, o la ruta relativa correcta):

```cpp
static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

static void test_fade_in_rises()
{
    SplashTimings t; // fadeIn=0.3
    // A mitad del fade-in, alpha ~0.5, aun no crossfade ni done.
    SplashState s = splashStateAt(t, 0.15f, /*loadingDone=*/false, 0.0f);
    CHECK(near(s.alpha, 0.5f));
    CHECK(!s.crossfading);
    CHECK(!s.done);
}

static void test_hold_while_loading()
{
    SplashTimings t;
    // Tras el fade-in, cargando todavia: alpha 1, sin crossfade.
    SplashState s = splashStateAt(t, 1.0f, /*loadingDone=*/false, 0.0f);
    CHECK(near(s.alpha, 1.0f));
    CHECK(!s.crossfading);
    CHECK(!s.done);
}

static void test_min_total_respected()
{
    SplashTimings t; // minTotal=1.5
    // Carga termino pronto (a 0.5s) pero aun no se alcanzo minTotal: sigue en
    // hold a alpha 1, sin empezar el fade-out.
    SplashState s = splashStateAt(t, 1.0f, /*loadingDone=*/true, 0.5f);
    CHECK(near(s.alpha, 1.0f));
    CHECK(!s.crossfading);
    CHECK(!s.done);
}

static void test_crossfade_after_load_and_min()
{
    SplashTimings t; // minTotal=1.5, fadeOut=0.3
    // Carga termino a 1.0s; fade-out empieza en max(1.0,1.5)=1.5. A 1.65s
    // (mitad del fade-out) alpha ~0.5 y crossfading.
    SplashState s = splashStateAt(t, 1.65f, /*loadingDone=*/true, 1.0f);
    CHECK(near(s.alpha, 0.5f));
    CHECK(s.crossfading);
    CHECK(!s.done);
}

static void test_done_after_fade_out()
{
    SplashTimings t;
    // Fade-out completo (1.5 + 0.3 = 1.8): done, alpha 0.
    SplashState s = splashStateAt(t, 2.0f, /*loadingDone=*/true, 1.0f);
    CHECK(near(s.alpha, 0.0f));
    CHECK(s.done);
}
```
Y llamarlos desde `main()`.

- [ ] **Step 2: Implementar `runtime/SplashDriver.h`**

```cpp
#pragma once
#include <algorithm>
#include <cmath>

// Calculo puro del alpha del splash por fase. Sin GPU ni estado: dado el tiempo
// transcurrido y si la carga termino (y cuando), devuelve el alpha y en que
// fase esta. Testeable en headless (engine/tests/splash_tests.cpp).
struct SplashTimings { float fadeIn = 0.3f; float minTotal = 1.5f; float fadeOut = 0.3f; };
struct SplashState   { float alpha; bool crossfading; bool done; };

inline SplashState splashStateAt(const SplashTimings& t, float elapsed,
                                 bool loadingDone, float loadingDoneAt)
{
    // Fase 1: fade-in.
    if (elapsed < t.fadeIn)
        return { std::clamp(elapsed / t.fadeIn, 0.0f, 1.0f), false, false };

    // El fade-out no puede empezar hasta que la carga termino Y se cumplio el
    // minimo total. Mientras no, hold a alpha 1.
    if (!loadingDone)
        return { 1.0f, false, false };

    const float fadeOutStart = std::max(loadingDoneAt, t.minTotal);
    if (elapsed < fadeOutStart)
        return { 1.0f, false, false }; // hold hasta el minimo

    // Fase 3: fade-out (crossfade). alpha 1 -> 0.
    const float k = (elapsed - fadeOutStart) / t.fadeOut;
    if (k >= 1.0f) return { 0.0f, true, true };
    return { 1.0f - k, true, false };
}
```

- [ ] **Step 3: Dar visibilidad del header al test (CMake)**

En `engine/tests/CMakeLists.txt`, para `dt_splash_tests` añadir el include del dir de runtime:

```cmake
target_include_directories(dt_splash_tests PRIVATE ${CMAKE_SOURCE_DIR}/runtime)
```
y en el test `#include "SplashDriver.h"`.

- [ ] **Step 4: Compilar y correr el test**

Run (PowerShell): `.\build.bat` luego `build-ninja\engine\tests\dt_splash_tests.exe`
Expected: `ALL SPLASH TESTS PASSED`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add runtime/SplashDriver.h engine/tests/splash_tests.cpp engine/tests/CMakeLists.txt
git commit -m "feat(splash): SplashDriver (alpha por fase) con tests de las 5 fases"
```

---

## Task 7: Reordenar `runtime/main.cpp` con el splash

**Files:**
- Modify: `runtime/main.cpp`

**Interfaces:**
- Consumes: `Renderer::initPresentation/initSceneResources/beginSplash/drawSplashFrame` (Tasks 4-5); `splashStateAt`/`SplashTimings` (Task 6).

- [ ] **Step 1: Incluir el driver y `<chrono>`**

En `runtime/main.cpp` (ya incluye `<chrono>`): añadir `#include "SplashDriver.h"`. Dar visibilidad del header al target del runtime: en `runtime/CMakeLists.txt` el `.h` está en el propio dir del target, así que `#include "SplashDriver.h"` resuelve sin cambios (mismo directorio que `main.cpp`).

- [ ] **Step 2: Reemplazar `renderer.init(window, meshes)` por la secuencia con splash**

Sustituir la línea `renderer.init(window, meshes);` (línea ~115, tras construir `meshes`) por:

```cpp
        // Resuelve el logo: en un paquete exportado esta junto al .exe como
        // splash.png; en dev (sin exportar) se cae a assets/MainEngineLogo.png.
        std::string logoPath = "splash.png";
        {
            std::error_code lec;
            if (!std::filesystem::exists(logoPath, lec) || lec)
                logoPath = "assets/MainEngineLogo.png";
        }

        renderer.initPresentation(window);

        const auto splashStart = std::chrono::high_resolution_clock::now();
        const bool haveSplash = renderer.beginSplash(logoPath);
        const SplashTimings splashT;
        auto sinceSplash = [&]() {
            return std::chrono::duration<float>(
                std::chrono::high_resolution_clock::now() - splashStart).count();
        };
        auto pumpSplash = [&](bool loadingDone, float loadingDoneAt) {
            if (!haveSplash) return;
            window.pollEvents();
            SplashState s = splashStateAt(splashT, sinceSplash(), loadingDone, loadingDoneAt);
            renderer.drawSplashFrame(s.alpha);
        };

        // Un frame de splash antes de la carga pesada (alpha del fade-in inicial).
        pumpSplash(false, 0.0f);

        renderer.initSceneResources(meshes);
        pumpSplash(false, 0.0f);
```

- [ ] **Step 3: Intercalar splash en el resto de la carga**

Tras `initSkybox(...)` añadir `pumpSplash(false, 0.0f);`. En el bucle de `addSkinnedMesh` (pasada 2), añadir `pumpSplash(false, 0.0f);` dentro del bucle tras cada `addSkinnedMesh`. Tras `scriptManager.init("Scripts")` añadir `pumpSplash(false, 0.0f);`.

- [ ] **Step 4: Fase final del splash antes del game loop (hold hasta el mínimo + fade-out)**

Justo antes de `scriptManager.onPlayStart();` (o justo antes del `while (!window.shouldClose())`), añadir el bucle de cierre del splash:

```cpp
        // La carga termino: marcar el instante y drenar el resto del splash
        // (hold hasta minTotal + fade-out). Fallback simple (sin crossfade
        // con la escena): el logo funde a su color de fondo y se corta al
        // primer frame de juego. El crossfade real es una mejora posterior.
        if (haveSplash)
        {
            const float loadingDoneAt = sinceSplash();
            for (;;)
            {
                window.pollEvents();
                if (window.shouldClose()) break;
                SplashState s = splashStateAt(splashT, sinceSplash(), true, loadingDoneAt);
                renderer.drawSplashFrame(s.alpha);
                if (s.done) break;
            }
        }
```

- [ ] **Step 5: Compilar Release y copiar el runtime + fmod.dll a la raíz para probar**

Run (PowerShell): `.\build-release.bat` luego `Copy-Item build-ninja-release\runtime\DonTopoRuntime.exe .; Copy-Item build-ninja-release\sandbox\fmod.dll .`
Expected: sin errores.

- [ ] **Step 6: Crear escena de prueba y ejecutar**

Crear `assets/splash_probe.scene` (un personaje skinned + cámara) idéntica a la que se usó para medir tiempos (mesh `assets/modelAnimation.fbx`, cámara en `[0,150,400]`). Run (PowerShell):
```
$p = Start-Process .\DonTopoRuntime.exe -ArgumentList "assets/splash_probe.scene" -PassThru; Start-Sleep 6; if(!$p.HasExited){$p.Kill()}
```
Expected: al arrancar, tras ~0.6 s de negro aparece el logo con fade-in, se mantiene durante la carga y funde a la escena. Verificación **manual** (mirar la ventana).

- [ ] **Step 7: Limpiar artefactos de prueba**

Run (PowerShell): `Remove-Item DonTopoRuntime.exe, fmod.dll, assets\splash_probe.scene -ErrorAction SilentlyContinue`

- [ ] **Step 8: Commit**

```bash
git add runtime/main.cpp
git commit -m "feat(splash): runtime muestra el splash con fade in/out durante la carga"
```

---

## Task 8: El exportador copia `splash.png`

**Files:**
- Modify: `engine/src/Editor/GameExporter.cpp` (junto a la copia del skybox, ~línea 415)
- Modify: `engine/tests/exporter_tests.cpp`

**Interfaces:**
- Consumes: el patrón `copyOne` y `projectRoot` de `writeExportPackage`.
- Produces: el paquete contiene `splash.png` (copia de `assets/MainEngineLogo.png`). Faltar el logo es AVISO (no error): coherente con "logo ausente no bloquea".

- [ ] **Step 1: Escribir el test (falla: aún no se copia)**

En `engine/tests/exporter_tests.cpp`, extender `test_package_contents` (o añadir un test nuevo `test_package_includes_splash`): el fixture ya crea `assets/`; añadir `std::ofstream(root / "assets" / "MainEngineLogo.png") << "png";` en el fixture y, tras `writeExportPackage`, `CHECK(fs::exists(pkg / "splash.png"));`. Seguir el patrón exacto de los `CHECK(fs::exists(...))` existentes en ese test.

- [ ] **Step 2: Ejecutar el test y verlo fallar**

Run (PowerShell): `.\build.bat` luego `build-ninja\engine\tests\dt_exporter_tests.exe`
Expected: FAIL en el `CHECK(fs::exists(pkg / "splash.png"))`.

- [ ] **Step 3: Implementar la copia en `GameExporter.cpp`**

Tras el bloque del skybox (línea ~441, antes del bloque de shaders), añadir:

```cpp
    // Logo del splash: el runtime lo busca como splash.png junto al .exe. Va
    // siempre (la escena no lo referencia), pero faltar es AVISO, no error: sin
    // el, el runtime arranca directo sin splash (SplashScreen::init devuelve
    // false y el runtime lo respeta). Mismo criterio que fmod.dll.
    {
        const fs::path logo = projectRoot / "assets" / "MainEngineLogo.png";
        std::error_code lec;
        if (fs::exists(logo, lec) && !lec)
            ok = copyOne(logo, pkg / "splash.png") && ok;
        else
            r.messages.push_back("Aviso: no se encontro " + logo.string() +
                                 "; el juego exportado arrancara sin splash screen.");
    }
```

- [ ] **Step 4: Ejecutar el test y verlo pasar**

Run (PowerShell): `.\build.bat` luego `build-ninja\engine\tests\dt_exporter_tests.exe`
Expected: `OK`, exit 0.

- [ ] **Step 5: Verificar por sabotaje que el test tiene dientes**

Comentar temporalmente la línea `ok = copyOne(logo, pkg / "splash.png") && ok;`, recompilar, correr el test → debe FALLAR en el nuevo CHECK. Restaurar la línea, recompilar, correr → PASS.

- [ ] **Step 6: Commit**

```bash
git add engine/src/Editor/GameExporter.cpp engine/tests/exporter_tests.cpp
git commit -m "feat(export): copiar splash.png al paquete (aviso si falta, no error)"
```

---

## Task 9: Documentación

**Files:**
- Modify: `README.md`

**Interfaces:** ninguna.

- [ ] **Step 1: Documentar el splash en el README**

En la sección de export/runtime, añadir una nota: el juego exportado muestra un splash con el logo (`assets/MainEngineLogo.png` → `splash.png` en el paquete) durante la carga; si el PNG falta, arranca sin splash. Logo fijo por ahora (no configurable por proyecto).

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs(splash): documentar el splash del juego exportado"
```

---

## Verificación manual final (usuario)

No automatizable (GPU + temporal). Con un editor Release:

1. Exportar un juego con un personaje skinned. Ejecutar el `.exe` del paquete: tras ~0.6 s de negro, el logo aparece con fade-in, se mantiene durante la carga, y funde a la escena. Letterbox correcto (logo sin deformar).
2. Exportar una escena vacía (solo cámara): el splash respeta el mínimo de ~1.5 s aunque cargue rápido.
3. Borrar `splash.png` del paquete y ejecutar: arranca sin splash, sin crashear.
4. Ventana redimensionada a un aspect raro antes de arrancar: el letterbox sigue centrado, sin estirar.

---

## Fuera de alcance (recordatorio)

- Fase 2 multithread (worker cargando mientras el splash anima a 60 fps): requiere resolver `graphicsQueue`/transfer queue. Continuación natural, no aquí.
- Cross-fade real escena+logo: Task 7 entrega el fallback (fade del logo a fondo + corte). El cross-fade compuesto (dibujar la escena y el logo con alpha↓ encima en el mismo frame) es una mejora posterior.
- Reducir los ~600 ms de arranque de Vulkan (device+swapchain).
- Logo configurable por proyecto desde el editor.
