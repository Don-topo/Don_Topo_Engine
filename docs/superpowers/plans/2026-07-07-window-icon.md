# Icono de ventana/taskbar — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Que la ventana principal muestre `assets/MainEngineLogo.png` como icono en la barra de título y en la barra de tareas de Windows.

**Architecture:** `Window::init()` gana un parámetro opcional `iconPath`; si se pasa, carga el PNG con `stbi_load` (mismo patrón ya usado en `GpuResources.cpp`/`Skybox.cpp`) y llama `glfwSetWindowIcon`. `sandbox/main.cpp` pasa la ruta del asset.

**Tech Stack:** C++20, GLFW, stb_image (ya vendorizado en el proyecto).

## Global Constraints

- Spec de referencia: `docs/superpowers/specs/2026-07-07-window-icon-design.md`.
- Fallo al cargar el icono no debe crashear la app — solo warning por stderr, la ventana se crea igual sin icono personalizado.
- No definir `STB_IMAGE_IMPLEMENTATION` en `Window.cpp` — ya está definido una única vez en `engine/src/GpuResources.cpp:5`; un segundo `#define` causaría símbolos duplicados en el link.
- Sin framework de tests unitarios en este repo (proyecto de motor gráfico); verificación es build limpio (`.\build.bat`) + prueba manual visual descrita en el spec.

---

### Task 1: Icono de ventana desde `MainEngineLogo.png`

**Files:**
- Modify: `engine/include/DonTopo/Window.h`
- Modify: `engine/src/Window.cpp`
- Modify: `sandbox/src/main.cpp:28`

**Interfaces:**
- Consumes: nada de otras tasks.
- Produces: nada consumido por otras tasks (plan de una sola task).

- [ ] **Step 1: Añadir parámetro `iconPath` a la declaración en `Window.h`**

Reemplazar (línea 15):

```cpp
    void init(int width, int height, const char* title);
```

por:

```cpp
    // iconPath: ruta a un PNG (RGBA o no) para el icono de la ventana y de
    // la barra de tareas (glfwSetWindowIcon). nullptr = icono por defecto
    // del sistema.
    void init(int width, int height, const char* title, const char* iconPath = nullptr);
```

- [ ] **Step 2: Implementar la carga del icono en `Window.cpp`**

Añadir el include de `stb_image.h` tras la línea 2 (`#include <GLFW/glfw3.h>`).
**No** definir `STB_IMAGE_IMPLEMENTATION` aquí — ya existe una definición
única en `engine/src/GpuResources.cpp:5`, y el header se incluye en modo
"solo declaraciones" en cualquier otro `.cpp`:

```cpp
#include "DonTopo/Window.h"
#include <GLFW/glfw3.h>
#include <stb_image.h>
#include <cstdio>
#include <stdexcept>
```

Reemplazar la firma de `init` (línea 11) y el cuerpo completo de la función
(líneas 11-23):

```cpp
void Window::init(int width, int height, const char* title, const char* iconPath) {
    if (!glfwInit())
        throw std::runtime_error("GLFW: failed to initialize");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // Vulkan, sin contexto OpenGL
    glfwWindowHint(GLFW_RESIZABLE,  GLFW_TRUE);

    m_window = glfwCreateWindow(width, height, title, nullptr, nullptr);
    if (!m_window) {
        glfwTerminate();
        throw std::runtime_error("GLFW: failed to create window");
    }

    if (iconPath) {
        int w, h, channels;
        unsigned char* pixels = stbi_load(iconPath, &w, &h, &channels, STBI_rgb_alpha);
        if (pixels) {
            GLFWimage image{ w, h, pixels };
            // count=1: un solo tamaño: GLFW/Windows escala esa imagen para
            // ICON_SMALL (barra de título) e ICON_BIG (barra de tareas).
            glfwSetWindowIcon(m_window, 1, &image);
            stbi_image_free(pixels);
        } else {
            std::fprintf(stderr, "Window: no se pudo cargar el icono '%s'\n", iconPath);
        }
    }
}
```

- [ ] **Step 3: Pasar la ruta del asset desde `sandbox/main.cpp`**

Reemplazar (línea 28):

```cpp
        window.init(1280, 720, "Don Topo Engine");
```

por:

```cpp
        window.init(1280, 720, "Don Topo Engine", "assets/MainEngineLogo.png");
```

- [ ] **Step 4: Compilar**

Run: `.\build.bat`
Expected: build limpio, sin errores ni warnings de símbolo duplicado de
`stbi_load`/`STB_IMAGE_IMPLEMENTATION`.

- [ ] **Step 5: Prueba manual**

Ejecutar `build-ninja\sandbox\Sandbox.exe`. Confirmar visualmente:
1. El icono en la esquina superior izquierda de la barra de título es el
   logo (`MainEngineLogo.png`), no el icono genérico de GLFW.
2. El icono en la barra de tareas de Windows (mientras la app corre) es el
   mismo logo.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Window.h engine/src/Window.cpp sandbox/src/main.cpp
git commit -m "feat(window): icono de ventana/taskbar desde MainEngineLogo.png"
```
