# Diseño: modo de visualización wireframe

## Objetivo

Añadir un modo de visualización wireframe: al activarlo, solo se ven las
aristas (líneas) de todos los GameObjects de la escena, sin relleno de
caras, texturas ni iluminación. Un botón toggle en la parte superior de la
interfaz, por encima del viewport, alterna entre modo normal y wireframe.

## Contexto actual

- `Renderer::recordCommandBuffer` (`engine/src/Renderer.cpp:596-647`) dibuja
  objetos estáticos con `m_pipeline` y objetos skinned con
  `m_skinnedGfxPipeline`, ambos creados con `polygonMode = VK_POLYGON_MODE_FILL`
  y shader `pbr.frag`.
- `GpuDevice::createLogicalDevice` (`engine/src/GpuDevice.cpp:166-174`) no
  habilita ninguna `VkPhysicalDeviceFeatures` (`pEnabledFeatures` nunca se
  asigna) — `VK_POLYGON_MODE_LINE` requiere la feature `fillModeNonSolid`.
- `EditorUI::draw` (`engine/src/EditorUI.cpp:115-125`) llama primero a
  `drawDockSpace()`, que posiciona el dockspace ocupando todo
  `ImGui::GetMainViewport()` (`engine/src/EditorUI.cpp:127-145`). No existe
  ningún elemento de UI fuera del dockspace hoy.
- `EditorUI` ya tiene `Renderer* m_renderer` (seteado por
  `Renderer::setSceneRoot`, `engine/src/Renderer.cpp:2001-2005`), usado hoy
  para `addStaticMesh` al crear shapes — mismo canal se reutiliza para el
  toggle.
- Los shaders se compilan por glob en `sandbox/CMakeLists.txt:17-21`
  (`*.vert`, `*.frag`, `*.comp` en `shaders/`) — un `.frag` nuevo requiere
  reconfigurar CMake (el glob no usa `CONFIGURE_DEPENDS`).

## Arquitectura

### 1. `GpuDevice` — habilitar `fillModeNonSolid`

En `createLogicalDevice`, construir `VkPhysicalDeviceFeatures` con
`fillModeNonSolid = VK_TRUE` y asignarlo a `createInfo.pEnabledFeatures`.
Sin guard adicional de soporte: es una feature prácticamente universal en
GPUs desktop con Vulkan (misma asunción implícita que ya hace el motor con
otras capacidades del device).

### 2. Shader nuevo `shaders/wireframe.frag`

Fragment shader mínimo, sin `in` (ignora todos los outputs del vertex
shader), color plano fijo:

```glsl
#version 450

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(0.1, 1.0, 0.3, 1.0); // verde wireframe
}
```

Se empareja con el `triangle.vert` ya existente (mismo usado por
`m_pipeline` y `m_skinnedGfxPipeline`) — el vertex shader no cambia, la
attribute layout de vértice sigue siendo la misma en ambos pipelines
nuevos.

### 3. Dos pipelines wireframe nuevos en `Renderer`

Clones exactos de los pipelines existentes (mismo
`VkPipelineVertexInputStateCreateInfo`, mismo `m_pipelineLayout`, mismo
`cullMode`/`depthTest`/`renderPass`), cambiando únicamente
`polygonMode = VK_POLYGON_MODE_LINE` y el módulo de fragment shader por
`wireframe.frag.spv`:

- `m_wireframePipeline` — clon de `m_pipeline` (creado en `createPipeline()`,
  `engine/src/Renderer.cpp:693-842`).
- `m_skinnedWireframePipeline` — clon del skinned gfx pipeline (creado en
  `createComputePipelines()`, `engine/src/Renderer.cpp:1609-1689`).

Ambos se destruyen en `shutdown()` junto a sus contrapartes normales.

### 4. Toggle de estado en `Renderer`

```cpp
void setWireframeMode(bool enabled) { m_wireframeMode = enabled; }
bool isWireframeMode() const { return m_wireframeMode; }
```

`m_wireframeMode` (bool, default `false`) nuevo miembro privado.

### 5. `recordCommandBuffer` — selección de pipeline

En el bloque de dibujo de objetos estáticos
(`engine/src/Renderer.cpp:596-614`):

```cpp
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
    m_wireframeMode ? m_wireframePipeline : m_pipeline);
```

Mismo patrón para el bloque skinned
(`engine/src/Renderer.cpp:618-619`), alternando
`m_skinnedWireframePipeline` / `m_skinnedGfxPipeline`. El resto del loop
(descriptor sets, push constants, bind de vertex/index buffer, draw call)
no cambia — el pipeline wireframe comparte layout y push constants con el
normal.

El dibujo del skybox (`engine/src/Renderer.cpp:657-661`) se salta por
completo si `m_wireframeMode` es `true` — el `clearValues[0].color` ya es
negro sólido (`engine/src/Renderer.cpp:571`), así que omitir el skybox ya
deja el fondo negro sin cambios adicionales.

Gizmos (ejes de selección, wireframes de collider) se siguen dibujando
igual en ambos modos — no dependen de `m_wireframeMode`.

### 6. Toolbar en `EditorUI`

Nuevo método `drawToolbar()`, llamado antes de `drawDockSpace()` en
`EditorUI::draw`. Ventana fija de altura `kToolbarHeight = 30.0f`, ancho
completo del viewport principal, sin título/resize/move/scroll:

```cpp
void EditorUI::drawToolbar()
{
    constexpr float kToolbarHeight = 30.0f;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(ImVec2(vp->Size.x, kToolbarHeight));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
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
```

`drawDockSpace()` se ajusta para arrancar debajo de la toolbar en vez de
ocupar todo `vp->Size`:

```cpp
ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + kToolbarHeight));
ImGui::SetNextWindowSize(ImVec2(vp->Size.x, vp->Size.y - kToolbarHeight));
```

(`kToolbarHeight` se extrae a constante de clase o archivo para no
duplicar el literal entre los dos métodos.)

## Fuera de alcance

- Puntos/vértices dibujados como marcadores explícitos — el modo wireframe
  muestra solo aristas (vértices quedan implícitos en las intersecciones de
  líneas), decisión ya tomada por ser el patrón estándar de motores
  (Unity/Unreal/Blender object-mode wireframe).
- Persistencia del toggle entre sesiones — arranca siempre en modo normal.
- Wireframe con profundidad desactivada (X-ray) — se mantiene depth test +
  cull mode idénticos al pipeline normal, mismo comportamiento de oclusión
  que el modo sólido, solo cambia fill→line.
- Color configurable de las líneas — fijo en el shader.
- Cualquier cambio a Gizmos, colliders o al pipeline de sombras — no
  interactúan con este modo.

## Plan de verificación manual

1. Build limpio (reconfigurar CMake por el `.frag` nuevo), sin errores de
   compilación de shaders ni validation errors de Vulkan al arrancar.
2. Escena con varios GameObjects (mesh estático + al menos uno con
   AnimatedMesh/skinned si hay alguno de prueba disponible) — pulsar
   "Wireframe": solo aristas verdes visibles, sin relleno, fondo negro, sin
   skybox.
3. Pulsar de nuevo: vuelve a modo normal (textura/iluminación/skybox como
   antes).
4. Alternar el botón varias veces seguidas — sin flicker, sin crash, sin
   validation errors.
5. Orbitar la cámara y hacer resize de la ventana en ambos modos — sin
   crash, pipelines wireframe sobreviven a `recreateSwapChain`.
6. Seleccionar un GameObject con collider en modo wireframe — el wireframe
   amarillo del collider y los ejes de selección se siguen viendo igual que
   en modo normal.
7. Escena vacía (sin GameObjects) en modo wireframe — solo fondo negro +
   gizmos si hay selección, sin crash.
