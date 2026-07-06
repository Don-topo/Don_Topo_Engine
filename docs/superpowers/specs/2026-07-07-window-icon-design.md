# Diseño: icono de ventana/taskbar desde MainEngineLogo.png

## Objetivo

Que la ventana principal (GLFW) muestre `assets/MainEngineLogo.png` como
icono en la barra de título y en la barra de tareas de Windows, en vez del
icono genérico por defecto.

## Diseño

`Window::init()` (`engine/include/DonTopo/Window.h`,
`engine/src/Window.cpp`) gana un parámetro opcional:

```cpp
void init(int width, int height, const char* title, const char* iconPath = nullptr);
```

Si `iconPath != nullptr`, tras `glfwCreateWindow`:

1. `stbi_load(iconPath, &w, &h, &channels, STBI_rgb_alpha)` — mismo patrón
   ya usado en `GpuResources.cpp`/`Skybox.cpp` para cargar PNG a RGBA8.
2. Si la carga falla (`pixels == nullptr`): `fprintf(stderr, ...)` con el
   path y seguir sin icono (cosmético, no debe crashear la app).
3. Si carga bien: construir `GLFWimage{w, h, pixels}` y llamar
   `glfwSetWindowIcon(m_window, 1, &image)`.
4. `stbi_image_free(pixels)` siempre que se haya cargado.

`glfwSetWindowIcon` en el backend Win32 de GLFW envía `WM_SETICON` para
`ICON_SMALL` e `ICON_BIG` — cubre tanto el icono de la barra de título como
el de la barra de tareas, sin necesitar un recurso `.ico` embebido en el
`.exe`.

`sandbox/src/main.cpp` pasa la ruta al llamar `window.init(...)`:

```cpp
window.init(1280, 720, "Don Topo Engine", "assets/MainEngineLogo.png");
```

(`assets/` ya se copia al lado del ejecutable vía CMake —
`sandbox/CMakeLists.txt:37-42`.)

## Fuera de alcance

- Icono embebido como recurso `.exe` (`.ico` + `.rc`) — no aplica aquí,
  `glfwSetWindowIcon` en runtime es suficiente y ya cubre taskbar+título.
- Icono para otras plataformas (macOS/Linux) — `glfwSetWindowIcon` es
  no-op en macOS (usa el bundle icon), pero no rompe nada al llamarlo.

## Verificación manual

Ejecutar `sandbox/Sandbox.exe` → confirmar visualmente que el icono de la
barra de título y el de la barra de tareas de Windows son el logo, no el
icono genérico de GLFW.
