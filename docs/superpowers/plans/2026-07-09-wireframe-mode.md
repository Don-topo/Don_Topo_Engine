# Wireframe Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Añadir un modo de visualización wireframe (solo aristas de todos los GameObjects, sin relleno/textura/skybox) con un botón toggle en una toolbar fija encima del viewport.

**Architecture:** Dos pipelines Vulkan nuevos (`polygonMode = VK_POLYGON_MODE_LINE`) clonados de los pipelines de render existentes (estático y skinned), con un fragment shader plano nuevo. Requiere habilitar la feature de device `fillModeNonSolid` (hoy no se habilita ninguna feature). Toggle de estado (`m_wireframeMode`) vive en `Renderer`, que ya es dueño de `recordCommandBuffer`; `EditorUI` solo lo consulta/muta a través del puntero `Renderer*` que ya tiene.

**Tech Stack:** C++20, Vulkan, GLSL (glslc), ImGui, CMake + Ninja (preset `debug`).

## Global Constraints

- No hay framework de tests en el repo (sin gtest/ctest). Verificación = `cmake --preset debug` (reconfigurar cuando se añada un `.frag` nuevo) + `cmake --build --preset debug` + ejecutar `build-ninja/sandbox/Sandbox.exe` + revisar visualmente, igual que el resto del proyecto.
- Solo aristas visibles en wireframe (sin puntos de vértice marcados aparte) — decisión de spec, patrón estándar de motores.
- Color de línea plano fijo `vec4(0.1, 1.0, 0.3, 1.0)`, sin configuración.
- Mismo `cullMode`/depth test que el pipeline normal — sin X-ray, sin cambios de oclusión entre modos.
- Skybox se omite en wireframe (fondo negro, ya es el `clearValue` por defecto). Gizmos (ejes de selección, wireframes de collider) se dibujan igual en ambos modos, sin cambios.
- Toggle no persiste entre sesiones — arranca siempre en modo normal.
- Spec completo: `docs/superpowers/specs/2026-07-09-wireframe-mode-design.md`.

---

### Task 1: Habilitar `fillModeNonSolid` en el device Vulkan

**Files:**
- Modify: `engine/src/GpuDevice.cpp:145-179` (`GpuDevice::createDevice`)

**Interfaces:**
- Produces: device lógico creado con `VkPhysicalDeviceFeatures.fillModeNonSolid = VK_TRUE` habilitado — requisito de Vulkan para usar `VK_POLYGON_MODE_LINE`/`VK_POLYGON_MODE_POINT` en cualquier pipeline (consumido por Task 3 y Task 4).

- [ ] **Step 1: Habilitar la feature en `createDevice`**

En `engine/src/GpuDevice.cpp`, dentro de `GpuDevice::createDevice()`, reemplazar:

```cpp
    const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount    = (uint32_t)queueInfos.size();
    createInfo.pQueueCreateInfos       = queueInfos.data();
    createInfo.enabledExtensionCount   = 1;
    createInfo.ppEnabledExtensionNames = extensions;

    if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS)
        throw std::runtime_error("failed to create logical device!");
```

por:

```cpp
    const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    // fillModeNonSolid: requerida para VK_POLYGON_MODE_LINE (modo wireframe).
    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.fillModeNonSolid = VK_TRUE;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount    = (uint32_t)queueInfos.size();
    createInfo.pQueueCreateInfos       = queueInfos.data();
    createInfo.enabledExtensionCount   = 1;
    createInfo.ppEnabledExtensionNames = extensions;
    createInfo.pEnabledFeatures        = &deviceFeatures;

    if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS)
        throw std::runtime_error("failed to create logical device!");
```

- [ ] **Step 2: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 3: Verificación manual — el motor sigue arrancando igual que antes**

Run: `build-ninja/sandbox/Sandbox.exe`

Confirmar que la escena renderiza normal (modo sólido, texturas, luces) igual que antes del cambio — este task solo habilita una feature de device, no cambia ningún pipeline todavía.

- [ ] **Step 4: Commit**

```bash
git add engine/src/GpuDevice.cpp
git commit -m "feat(gpu): habilita fillModeNonSolid para soportar polygon mode LINE"
```

---

### Task 2: Fragment shader wireframe

**Files:**
- Create: `shaders/wireframe.frag`

**Interfaces:**
- Produces: `shaders/wireframe.frag.spv` (compilado por el glob de `sandbox/CMakeLists.txt:17-21`) — consumido por Task 3 y Task 4 como fragment shader de los pipelines wireframe.

- [ ] **Step 1: Crear el shader**

Crear `shaders/wireframe.frag`:

```glsl
#version 450

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(0.1, 1.0, 0.3, 1.0); // verde wireframe
}
```

No declara ningún `in` — el vertex shader (`triangle.vert`, reusado sin cambios) sigue emitiendo sus outputs normales, el fragment shader simplemente no los lee.

- [ ] **Step 2: Reconfigurar CMake para que recoja el `.frag` nuevo**

El glob de shaders en `sandbox/CMakeLists.txt:17-21` no usa `CONFIGURE_DEPENDS`, así que un fichero nuevo requiere reconfigurar.

Run: `cmake --preset debug`
Expected: termina sin error, reconoce el target `Shaders` con el nuevo fichero.

- [ ] **Step 3: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error, `build-ninja/sandbox/shaders/wireframe.frag.spv` existe.

- [ ] **Step 4: Commit**

```bash
git add shaders/wireframe.frag
git commit -m "feat(shaders): fragment shader plano para modo wireframe"
```

---

### Task 3: Pipeline wireframe para objetos estáticos

**Files:**
- Modify: `engine/include/DonTopo/Renderer.h:237` (miembro `m_wireframePipeline`)
- Modify: `engine/src/Renderer.cpp:693-851` (`Renderer::createPipeline`, crear el pipeline wireframe adicional)
- Modify: `engine/src/Renderer.cpp:187` (`Renderer::shutdown`, destruir `m_wireframePipeline`)

**Interfaces:**
- Consumes: `shaders/wireframe.frag.spv` (Task 2), feature `fillModeNonSolid` habilitada (Task 1).
- Produces: miembro `VkPipeline m_wireframePipeline` en `Renderer`, creado con el mismo vertex input/layout/render pass que `m_pipeline` pero `polygonMode = VK_POLYGON_MODE_LINE` y `wireframe.frag` — consumido por Task 5 (`recordCommandBuffer`).

- [ ] **Step 1: Declarar el miembro**

En `engine/include/DonTopo/Renderer.h`, reemplazar (línea 237):

```cpp
            VkPipeline                      m_pipeline                          = VK_NULL_HANDLE;
```

por:

```cpp
            VkPipeline                      m_pipeline                          = VK_NULL_HANDLE;
            VkPipeline                      m_wireframePipeline                 = VK_NULL_HANDLE;
```

- [ ] **Step 2: Crear el pipeline wireframe en `createPipeline()`**

En `engine/src/Renderer.cpp`, dentro de `Renderer::createPipeline()`, justo antes del cierre de la función (antes de la línea `printf("pipeline OK\n"); fflush(stdout);` que sigue a la creación de `m_pipeline`), reemplazar:

```cpp
        if(vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create graphics pipeline!");
        }

        // los módulos se destruyen al final de esta función — solo los necesita el pipeline
        vkDestroyShaderModule(m_gpu.device(), vertModule, nullptr);
        vkDestroyShaderModule(m_gpu.device(), fragModule, nullptr);
        printf("pipeline OK\n"); fflush(stdout);
    }
```

por:

```cpp
        if(vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create graphics pipeline!");
        }

        // Pipeline wireframe: mismo vertex input/layout/render pass, solo
        // cambia polygonMode a LINE y el fragment shader a color plano.
        auto wireFragCode = loadShaderFile("shaders/wireframe.frag.spv");
        VkShaderModule wireFragModule = createShaderModule(wireFragCode);

        VkPipelineShaderStageCreateInfo wireFragStage{};
        wireFragStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        wireFragStage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        wireFragStage.module = wireFragModule;
        wireFragStage.pName  = "main";

        VkPipelineShaderStageCreateInfo wireStages[] = { vertStage, wireFragStage };

        VkPipelineRasterizationStateCreateInfo wireRasterizationInfo = rasterizationInfo;
        wireRasterizationInfo.polygonMode = VK_POLYGON_MODE_LINE;

        VkGraphicsPipelineCreateInfo wirePipelineInfo = pipelineInfo;
        wirePipelineInfo.pStages             = wireStages;
        wirePipelineInfo.pRasterizationState = &wireRasterizationInfo;

        if(vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &wirePipelineInfo, nullptr, &m_wireframePipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create wireframe graphics pipeline!");
        }

        // los módulos se destruyen al final de esta función — solo los necesita el pipeline
        vkDestroyShaderModule(m_gpu.device(), vertModule, nullptr);
        vkDestroyShaderModule(m_gpu.device(), fragModule, nullptr);
        vkDestroyShaderModule(m_gpu.device(), wireFragModule, nullptr);
        printf("pipeline OK\n"); fflush(stdout);
    }
```

- [ ] **Step 3: Destruir el pipeline en `shutdown()`**

En `engine/src/Renderer.cpp`, dentro de `Renderer::shutdown()`, reemplazar (línea 187):

```cpp
        vkDestroyPipeline(m_gpu.device(), m_pipeline, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_pipelineLayout, nullptr);
```

por:

```cpp
        vkDestroyPipeline(m_gpu.device(), m_pipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_wireframePipeline, nullptr);
        vkDestroyPipelineLayout(m_gpu.device(), m_pipelineLayout, nullptr);
```

- [ ] **Step 4: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 5: Verificación manual — el motor sigue arrancando y cerrando igual que antes**

Run: `build-ninja/sandbox/Sandbox.exe`

Confirmar arranque normal (modo sólido sin cambios — el pipeline nuevo se crea pero no se usa hasta Task 5) y cierre limpio de la app (sin validation errors de Vulkan en consola al salir, que indicarían un pipeline sin destruir).

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Renderer.h engine/src/Renderer.cpp
git commit -m "feat(renderer): pipeline wireframe para objetos estáticos"
```

---

### Task 4: Pipeline wireframe para objetos skinned

**Files:**
- Modify: `engine/include/DonTopo/Renderer.h:267` (miembro `m_skinnedWireframePipeline`)
- Modify: `engine/src/Renderer.cpp:1609-1697` (`Renderer::createComputePipelines`, bloque del skinned graphics pipeline)
- Modify: `engine/src/Renderer.cpp:214` (`Renderer::shutdown`, destruir `m_skinnedWireframePipeline`)

**Interfaces:**
- Consumes: `shaders/wireframe.frag.spv` (Task 2), feature `fillModeNonSolid` habilitada (Task 1).
- Produces: miembro `VkPipeline m_skinnedWireframePipeline` en `Renderer`, mismo vertex input que `m_skinnedGfxPipeline` con `polygonMode = VK_POLYGON_MODE_LINE` y `wireframe.frag` — consumido por Task 5.

- [ ] **Step 1: Declarar el miembro**

En `engine/include/DonTopo/Renderer.h`, reemplazar (línea 267):

```cpp
            VkPipeline            m_skinnedGfxPipeline    = VK_NULL_HANDLE;
```

por:

```cpp
            VkPipeline            m_skinnedGfxPipeline        = VK_NULL_HANDLE;
            VkPipeline            m_skinnedWireframePipeline  = VK_NULL_HANDLE;
```

- [ ] **Step 2: Crear el pipeline wireframe skinned en `createComputePipelines()`**

En `engine/src/Renderer.cpp`, dentro del bloque `// --- Skinned graphics pipeline (stride=80, mismos shaders) ---` de `createComputePipelines()`, reemplazar:

```cpp
            if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &pci, nullptr, &m_skinnedGfxPipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create skinned graphics pipeline!");

            vkDestroyShaderModule(m_gpu.device(), vertMod, nullptr);
            vkDestroyShaderModule(m_gpu.device(), fragMod, nullptr);
        }
    }
```

por:

```cpp
            if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &pci, nullptr, &m_skinnedGfxPipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create skinned graphics pipeline!");

            // Pipeline wireframe skinned: mismo vertex input/layout que el
            // gfx pipeline de arriba, solo cambia polygonMode a LINE y el
            // fragment shader a color plano.
            auto wireFragCode = loadShaderFile("shaders/wireframe.frag.spv");
            auto wireFragMod  = createShaderModule(wireFragCode);

            VkPipelineShaderStageCreateInfo wireFragStage{};
            wireFragStage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            wireFragStage.stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
            wireFragStage.module = wireFragMod;
            wireFragStage.pName  = "main";

            VkPipelineShaderStageCreateInfo wireStages[] = { stages[0], wireFragStage };

            VkPipelineRasterizationStateCreateInfo wireRs = rs;
            wireRs.polygonMode = VK_POLYGON_MODE_LINE;

            VkGraphicsPipelineCreateInfo wirePci = pci;
            wirePci.pStages             = wireStages;
            wirePci.pRasterizationState = &wireRs;

            if (vkCreateGraphicsPipelines(m_gpu.device(), VK_NULL_HANDLE, 1, &wirePci, nullptr, &m_skinnedWireframePipeline) != VK_SUCCESS)
                throw std::runtime_error("failed to create skinned wireframe pipeline!");

            vkDestroyShaderModule(m_gpu.device(), vertMod, nullptr);
            vkDestroyShaderModule(m_gpu.device(), fragMod, nullptr);
            vkDestroyShaderModule(m_gpu.device(), wireFragMod, nullptr);
        }
    }
```

- [ ] **Step 3: Destruir el pipeline en `shutdown()`**

En `engine/src/Renderer.cpp`, dentro de `Renderer::shutdown()`, reemplazar (línea 214):

```cpp
        vkDestroyPipeline(m_gpu.device(), m_skinnedGfxPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_shadowPipeline, nullptr);
```

por:

```cpp
        vkDestroyPipeline(m_gpu.device(), m_skinnedGfxPipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_skinnedWireframePipeline, nullptr);
        vkDestroyPipeline(m_gpu.device(), m_shadowPipeline, nullptr);
```

- [ ] **Step 4: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 5: Verificación manual — el motor sigue arrancando y cerrando igual que antes**

Run: `build-ninja/sandbox/Sandbox.exe`

Confirmar arranque normal (personajes skinned si hay alguno de prueba, sin cambios visuales todavía) y cierre limpio sin validation errors de Vulkan.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Renderer.h engine/src/Renderer.cpp
git commit -m "feat(renderer): pipeline wireframe para objetos skinned"
```

---

### Task 5: Toggle de estado + selección de pipeline en `recordCommandBuffer`

**Files:**
- Modify: `engine/include/DonTopo/Renderer.h:33-40` (métodos públicos `setWireframeMode`/`isWireframeMode`)
- Modify: `engine/include/DonTopo/Renderer.h:238` (miembro `m_wireframeMode`)
- Modify: `engine/src/Renderer.cpp:596` (bind de pipeline estático)
- Modify: `engine/src/Renderer.cpp:618-619` (bind de pipeline skinned)
- Modify: `engine/src/Renderer.cpp:656-661` (skip del skybox)

**Interfaces:**
- Consumes: `m_wireframePipeline` (Task 3), `m_skinnedWireframePipeline` (Task 4).
- Produces: `void Renderer::setWireframeMode(bool enabled)`, `bool Renderer::isWireframeMode() const` — consumidos por Task 6 (`EditorUI::drawToolbar`).

- [ ] **Step 1: Declarar el toggle público y el miembro de estado**

En `engine/include/DonTopo/Renderer.h`, reemplazar (línea 33-34):

```cpp
            void setCamera(const Camera& camera);
            void notifyResize() { m_framebufferResized = true; }
```

por:

```cpp
            void setCamera(const Camera& camera);
            void notifyResize() { m_framebufferResized = true; }
            void setWireframeMode(bool enabled) { m_wireframeMode = enabled; }
            bool isWireframeMode() const { return m_wireframeMode; }
```

Reemplazar (línea 238):

```cpp
            bool                            m_framebufferResized                = false;
```

por:

```cpp
            bool                            m_framebufferResized                = false;
            bool                            m_wireframeMode                     = false;
```

- [ ] **Step 2: Seleccionar pipeline estático en `recordCommandBuffer`**

En `engine/src/Renderer.cpp`, reemplazar (línea 596):

```cpp
            vkCmdBindPipeline(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
```

por:

```cpp
            vkCmdBindPipeline(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_wireframeMode ? m_wireframePipeline : m_pipeline);
```

- [ ] **Step 3: Seleccionar pipeline skinned en `recordCommandBuffer`**

En `engine/src/Renderer.cpp`, reemplazar (línea 618-619):

```cpp
                vkCmdBindPipeline(m_commandBuffers[m_currentFrame],
                    VK_PIPELINE_BIND_POINT_GRAPHICS, m_skinnedGfxPipeline);
```

por:

```cpp
                vkCmdBindPipeline(m_commandBuffers[m_currentFrame], VK_PIPELINE_BIND_POINT_GRAPHICS,
                    m_wireframeMode ? m_skinnedWireframePipeline : m_skinnedGfxPipeline);
```

- [ ] **Step 4: Omitir el skybox en wireframe**

En `engine/src/Renderer.cpp`, reemplazar:

```cpp
            // Skybox — fullscreen quad, depth LEQUAL sin escritura (al final del pass)
            if (m_skybox.isInitialized()) {
                glm::mat4 rotView    = glm::mat4(glm::mat3(m_viewMatrix)); // sin traslación
                glm::mat4 invViewProj = glm::inverse(proj * rotView);
                m_skybox.draw(m_commandBuffers[m_currentFrame], invViewProj);
            }
```

por:

```cpp
            // Skybox — fullscreen quad, depth LEQUAL sin escritura (al final del pass).
            // Omitido en wireframe: el fondo ya es negro sólido (clearValue por defecto).
            if (!m_wireframeMode && m_skybox.isInitialized()) {
                glm::mat4 rotView    = glm::mat4(glm::mat3(m_viewMatrix)); // sin traslación
                glm::mat4 invViewProj = glm::inverse(proj * rotView);
                m_skybox.draw(m_commandBuffers[m_currentFrame], invViewProj);
            }
```

- [ ] **Step 5: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 6: Verificación manual — sin forma de activarlo todavía**

Run: `build-ninja/sandbox/Sandbox.exe`

Confirmar que la escena renderiza exactamente igual que antes (modo sólido con skybox) — `m_wireframeMode` nunca se pone a `true` hasta Task 6, así que no hay cambio visible todavía.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Renderer.h engine/src/Renderer.cpp
git commit -m "feat(renderer): toggle de modo wireframe en recordCommandBuffer"
```

---

### Task 6: Toolbar con botón toggle en `EditorUI`

**Files:**
- Modify: `engine/include/DonTopo/EditorUI.h:60` (declaración `drawToolbar`)
- Modify: `engine/src/EditorUI.cpp:115-145` (`EditorUI::draw`, nueva `drawToolbar`, ajuste de `drawDockSpace`)

**Interfaces:**
- Consumes: `Renderer::setWireframeMode(bool)`, `Renderer::isWireframeMode() const` (Task 5), `EditorUI::m_renderer` (ya existente).
- Produces: franja de toolbar fija de 30px encima del dockspace, con botón "Wireframe" que alterna el modo — no expone nada consumido por otros tasks (es el punto final de la cadena).

- [ ] **Step 1: Declarar `drawToolbar` y la constante de altura**

En `engine/include/DonTopo/EditorUI.h`, reemplazar (línea 60):

```cpp
private:
    void drawDockSpace();
```

por:

```cpp
private:
    static constexpr float kToolbarHeight = 30.0f;
    void drawToolbar();
    void drawDockSpace();
```

- [ ] **Step 2: Implementar `drawToolbar` y ajustar `drawDockSpace`**

En `engine/src/EditorUI.cpp`, reemplazar:

```cpp
void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    drawDockSpace();
    drawScene(sceneRoot);
    drawSelectionGizmo();
    drawViewport(viewportTexture, cameraView);
    drawProperties();
    drawMeshDialog();
    drawAudioClipDialog();
    drawContentBrowser();
}

void EditorUI::drawDockSpace()
{
    ImGuiWindowFlags dockFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##DockSpace", nullptr, dockFlags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(ImGui::GetID("MainDockSpace"), ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();
}
```

por:

```cpp
void EditorUI::draw(VkDescriptorSet viewportTexture, GameObject* sceneRoot, const glm::mat4& cameraView)
{
    drawToolbar();
    drawDockSpace();
    drawScene(sceneRoot);
    drawSelectionGizmo();
    drawViewport(viewportTexture, cameraView);
    drawProperties();
    drawMeshDialog();
    drawAudioClipDialog();
    drawContentBrowser();
}

void EditorUI::drawToolbar()
{
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, kToolbarHeight));
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("##Toolbar", nullptr, flags);

    bool wireframe = m_renderer && m_renderer->isWireframeMode();
    if (wireframe)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("Wireframe") && m_renderer)
        m_renderer->setWireframeMode(!wireframe);
    if (wireframe)
        ImGui::PopStyleColor();

    ImGui::End();
}

void EditorUI::drawDockSpace()
{
    ImGuiWindowFlags dockFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + kToolbarHeight));
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - kToolbarHeight));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##DockSpace", nullptr, dockFlags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(ImGui::GetID("MainDockSpace"), ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();
}
```

- [ ] **Step 3: Compilar**

Run: `cmake --build --preset debug`
Expected: build termina sin error.

- [ ] **Step 4: Verificación manual**

Run: `build-ninja/sandbox/Sandbox.exe`

1. Confirmar que aparece una franja fija en la parte superior de toda la ventana, por encima del área de docking (Scene/Viewport/Properties), con un botón "Wireframe".
2. Con GameObjects en la escena (al menos un mesh estático; si hay uno skinned de prueba, incluirlo), pulsar "Wireframe" → solo aristas verdes visibles, sin relleno/textura/skybox, fondo negro. El botón queda resaltado (color activo).
3. Pulsar de nuevo → vuelve a modo normal (textura/iluminación/skybox como antes), botón deja de estar resaltado.
4. Alternar el botón varias veces seguidas → sin flicker, sin crash, sin validation errors de Vulkan en consola.
5. Orbitar la cámara y redimensionar la ventana del editor en ambos modos → sin crash (confirma que los pipelines wireframe sobreviven a `recreateSwapChain`, que reusa los mismos pipelines sin recrearlos).
6. Seleccionar un GameObject con collider en modo wireframe → el wireframe amarillo del collider y los ejes de selección se siguen viendo igual que en modo normal (Gizmos no depende de `m_wireframeMode`).
7. Con la escena vacía (sin GameObjects) en modo wireframe → solo fondo negro (+ gizmos si hay selección), sin crash.
8. Confirmar que la franja de toolbar no se puede mover, redimensionar ni acoplar (docking) — se queda fija arriba pase lo que pase con los paneles de abajo.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/EditorUI.h engine/src/EditorUI.cpp
git commit -m "feat(editor): toolbar con toggle de modo wireframe"
```
