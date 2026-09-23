# Miniaturas de texturas en el Content Browser — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Que el grid del Content Browser muestre una miniatura real de cada `.png/.jpg/.jpeg/.tga/.bmp` de la carpeta actual, en Vulkan y D3D12, en lugar del recuadro de color con `IMG`.

**Architecture:** Un atlas compartido de 2048×2048 (casillas de 64×64) con un único descriptor de ImGui por backend. La decodificación y la reducción a 64×64 corren en el `JobSystem`; el hilo principal sube las casillas ya listas al atlas en lote. Las tres piezas de lógica (`makeThumbnail`, `ThumbnailSlots`, `ThumbnailCache`) son CPU pura y se prueban headless; los dos backends solo aportan "crear el atlas" y "copiar N casillas".

**Tech Stack:** C++20, `stb_image` (ya está), Dear ImGui, Vulkan y D3D12 (D3D12MA), `JobSystem` del Core, CMake + Ninja + MSVC.

**Spec:** `docs/superpowers/specs/2026-09-23-content-browser-thumbnails-design.md`

## Desviaciones del spec (decididas al escribir el plan, tras leer los backends)

1. **La subida es por lote, no por casilla.** El spec define `uploadUiThumbnail(slot, rgba)`. Pero el subidor de Vulkan hace un submit **con espera** por llamada (`CmdScope` sin batch) y el de D3D12 hace `waitForGpu()` por llamada: 8 casillas por frame serían 8 esperas. El virtual pasa a ser `uploadUiThumbnails(const ThumbnailTile*, size_t)`: **una** espera por frame con subidas. Es lo que el propio spec ya pedía en su riesgo nº 2 ("las subidas de un frame se agrupan en un solo submit"). Todo o nada: si devuelve `false`, el cache marca todo el lote como fallido.
2. **Formato del atlas por backend.** El spec dice "el mismo formato que `UiTextureAtlas`". Vulkan pinta la UI sobre un swapchain `B8G8R8A8_SRGB`, así que el atlas es `R8G8B8A8_SRGB` (sampleo → lineal → el swapchain lo vuelve a codificar: identidad). En D3D12 el RTV de ImGui es `R8G8B8A8_UNORM` (`sandbox/src/main.cpp`: `uiInfo.d3dRtvFormat`), y sampleando un SRV sRGB hacia un RTV UNORM la imagen saldría más oscura; el atlas es `R8G8B8A8_UNORM`. Está en **una constante por backend** para poder cambiarla si la comprobación manual (Task 5/6) dice otra cosa.

## Global Constraints

- Sin dependencias nuevas de terceros; `stb_image` ya está en el proyecto.
- Casilla de miniatura: `kThumbCell = 64` px; atlas de `2048×2048` (`32×32 = 1024` casillas).
- Como máximo **4** decodificaciones en vuelo y **8** casillas subidas por frame.
- Se rechaza sin decodificar toda imagen de más de **100 megapíxeles** (`kThumbMaxSourcePixels = 100'000'000`).
- Solo texturas (`.png`, `.jpg`, `.jpeg`, `.tga`, `.bmp`) y solo las que están a la vista.
- Misma interfaz `EditorRenderer` para Vulkan y D3D12; sin soporte (`uiThumbnailAtlasId() == 0`) el grid se comporta exactamente como hoy.
- Build: `.\configure.bat` (si se añade un `.cpp` o un test) y `.\build.bat` desde la raíz del repo, en PowerShell; nunca `cmake` crudo desde Bash.
- Tests: `main()` + macro `CHECK`, sin framework, registrados con `dt_add_test(nombre fichero.cpp LIB)` en `engine/tests/CMakeLists.txt`. Se ejecutan **desde la raíz del repo**.
- Toda llamada a `std::filesystem` que toque disco usa la sobrecarga con `std::error_code`.
- Comentarios de código en español, una línea si es posible, solo cuando el porqué no es obvio.
- Paridad Vulkan/D3D12 en todo cableado de `sandbox/src/main.cpp`: las dos ramas de `main()` son independientes.
- Ficheros con finales de línea CRLF: tras editar uno grande, comprobar con `git diff --stat` que el diff no es una reescritura entera.
- Bajo D3D12 el editor deja un `d3d12_diag.log` en la raíz del repo: borrarlo antes de commitear.

## Review Focus

Entradas y fallos que el spec implica y que la lógica normal no ejercita, por orden de probabilidad:

1. **Imagen enorme** (p. ej. un PNG de 20000×20000): no debe reservar ~1,6 GB en un worker ni colgar el grid. Task 1: test con una cabecera TGA de 20000×20000 sin cuerpo → `TooLarge`.
2. **Imagen con transparencia**: al reducirla no debe aparecer un halo oscuro en los bordes (promediar sin ponderar por alfa). Task 1: test con alfa 128 que exige color intacto.
3. **Carpeta con cientos de imágenes**: no debe saturar los workers ni subir cientos de casillas en un frame. Task 3: tests del tope de 4 en vuelo y de 8 subidas por frame.
4. **Cambio de carpeta a mitad de carga**: los resultados que llegan después no deben acabar dibujados en la carpeta nueva. Task 3: test de `newGeneration`.
5. **Cierre del editor con trabajo en vuelo**: un resultado que llega tras destruir el cache no debe tocar memoria liberada; y en D3D12 el atlas debe soltarse antes del allocator (assert de D3D12MA al cerrar). Task 3: test del resultado tardío; Task 6: paso de cierre limpio en Debug.
6. **Atlas lleno / casilla desalojada**: pedir una miniatura cuya casilla otro ítem ha reutilizado no debe pintar la imagen equivocada. Task 2 y Task 3: tests de LRU y de re-petición tras desalojo.

---

### Task 1: `ThumbnailAtlas.h` y `makeThumbnail`

**Files:**
- Create: `engine/include/DonTopo/Renderer/ThumbnailAtlas.h`
- Create: `engine/include/DonTopo/Editor/Thumbnail.h`
- Create: `engine/src/Editor/Thumbnail.cpp`
- Create: `engine/tests/thumbnail_tests.cpp`
- Modify: `engine/CMakeLists.txt` (registrar `src/Editor/Thumbnail.cpp` junto a `src/Editor/AssetImport.cpp`)
- Modify: `engine/tests/CMakeLists.txt` (registrar `dt_thumbnail_tests`)

**Interfaces:**
- Consumes: nada (primera tarea).
- Produces: en `ThumbnailAtlas.h`: `kThumbCell`, `kThumbAtlasCells`, `kThumbAtlasSize`, `kThumbSlotCount`, `struct UvRect { float u0, v0, u1, v1; }`, `struct ThumbnailTile { uint32_t slot; const uint8_t* rgba; }`, `UvRect thumbnailUv(uint32_t slot)`. En `Thumbnail.h`: `enum class ThumbnailStatus { Ok, Unreadable, TooLarge }`, `struct ThumbnailResult { ThumbnailStatus status; std::vector<uint8_t> rgba; }` (`rgba` = `kThumbCell*kThumbCell*4` bytes cuando `Ok`), `constexpr uint64_t kThumbMaxSourcePixels`, `ThumbnailResult makeThumbnail(const std::filesystem::path&)`. Las Tasks 2-6 los usan.

- [ ] **Step 1: Escribir el test que falla**

Crear `engine/tests/thumbnail_tests.cpp`:

```cpp
// Tests headless de las miniaturas del Content Browser (sin GPU ni ImGui).
// Plain main + CHECK, mismo patron que content_browser_tests.cpp.
#include "DonTopo/Editor/Thumbnail.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <system_error>
#include <vector>

using namespace DonTopo;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

using Rgba = std::array<uint8_t, 4>;

// TGA sin comprimir de 32 bits, origen arriba a la izquierda: el formato mas
// simple que stb_image lee con alfa, y no hace falta stb_image_write.
static void writeTga(const fs::path& p, int w, int h, const std::function<Rgba(int, int)>& pixel)
{
    std::ofstream f(p, std::ios::binary);
    uint8_t hdr[18] = {};
    hdr[2]  = 2;                               // truecolor sin comprimir
    hdr[12] = static_cast<uint8_t>(w & 0xFF);
    hdr[13] = static_cast<uint8_t>((w >> 8) & 0xFF);
    hdr[14] = static_cast<uint8_t>(h & 0xFF);
    hdr[15] = static_cast<uint8_t>((h >> 8) & 0xFF);
    hdr[16] = 32;                              // bits por pixel
    hdr[17] = 0x28;                            // origen arriba-izquierda, 8 bits de alfa
    f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const Rgba c = pixel(x, y);
            const uint8_t bgra[4] = { c[2], c[1], c[0], c[3] };
            f.write(reinterpret_cast<const char*>(bgra), 4);
        }
}

static Rgba pixelAt(const ThumbnailResult& r, uint32_t x, uint32_t y)
{
    const size_t i = (static_cast<size_t>(y) * kThumbCell + x) * 4;
    return { r.rgba[i], r.rgba[i + 1], r.rgba[i + 2], r.rgba[i + 3] };
}

static bool isRed(const Rgba& c)   { return c[0] == 255 && c[1] == 0 && c[2] == 0 && c[3] == 255; }
static bool isBlue(const Rgba& c)  { return c[0] == 0 && c[1] == 0 && c[2] == 255 && c[3] == 255; }
static bool isClear(const Rgba& c) { return c[3] == 0; }

static const Rgba kRed  = { 255, 0, 0, 255 };
static const Rgba kBlue = { 0, 0, 255, 255 };

static fs::path makeDir()
{
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / "dt_thumbnail_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

// Apaisada 128x64: se reduce a 64x32 y se centra (16 filas transparentes arriba y abajo).
static void test_landscape_is_letterboxed(const fs::path& dir)
{
    writeTga(dir / "land.tga", 128, 64, [](int, int) { return kRed; });
    ThumbnailResult r = makeThumbnail(dir / "land.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    CHECK(r.rgba.size() == static_cast<size_t>(kThumbCell) * kThumbCell * 4);
    if (r.rgba.size() != static_cast<size_t>(kThumbCell) * kThumbCell * 4) return;
    CHECK(isRed(pixelAt(r, 32, 32)));
    CHECK(isRed(pixelAt(r, 0, 16)));
    CHECK(isRed(pixelAt(r, 63, 47)));
    CHECK(isClear(pixelAt(r, 32, 15)));
    CHECK(isClear(pixelAt(r, 32, 48)));
}

// Vertical 32x128: escala 0.5 -> 16x64, centrada en horizontal.
static void test_portrait_is_pillarboxed(const fs::path& dir)
{
    writeTga(dir / "port.tga", 32, 128, [](int, int) { return kRed; });
    ThumbnailResult r = makeThumbnail(dir / "port.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isRed(pixelAt(r, 24, 0)));
    CHECK(isRed(pixelAt(r, 39, 63)));
    CHECK(isClear(pixelAt(r, 23, 32)));
    CHECK(isClear(pixelAt(r, 40, 32)));
}

// Mas pequena que la casilla: NO se amplia, se centra tal cual.
static void test_small_image_is_not_upscaled(const fs::path& dir)
{
    writeTga(dir / "small.tga", 16, 16, [](int, int) { return kRed; });
    ThumbnailResult r = makeThumbnail(dir / "small.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isRed(pixelAt(r, 24, 24)));
    CHECK(isRed(pixelAt(r, 39, 39)));
    CHECK(isClear(pixelAt(r, 23, 24)));
    CHECK(isClear(pixelAt(r, 40, 40)));
}

// Exactamente del tamano de la casilla: pasa sin tocar.
static void test_exact_size_fills_the_tile(const fs::path& dir)
{
    writeTga(dir / "exact.tga", 64, 64, [](int, int) { return kBlue; });
    ThumbnailResult r = makeThumbnail(dir / "exact.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isBlue(pixelAt(r, 0, 0)));
    CHECK(isBlue(pixelAt(r, 63, 63)));
}

// Reduccion 2:1 con caja: mitad izquierda roja, derecha azul, sin mezcla en el centro.
static void test_downscale_keeps_the_halves(const fs::path& dir)
{
    writeTga(dir / "halves.tga", 128, 128, [](int x, int) { return x < 64 ? kRed : kBlue; });
    ThumbnailResult r = makeThumbnail(dir / "halves.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isRed(pixelAt(r, 10, 32)));
    CHECK(isBlue(pixelAt(r, 53, 32)));
}

// Alfa 128: el color NO debe oscurecerse (promedio ponderado por alfa); sin eso
// aparece un halo oscuro en los bordes de cualquier sprite recortado.
static void test_alpha_does_not_darken_color(const fs::path& dir)
{
    writeTga(dir / "alpha.tga", 128, 128, [](int, int) { return Rgba{ 0, 255, 0, 128 }; });
    ThumbnailResult r = makeThumbnail(dir / "alpha.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    const Rgba c = pixelAt(r, 32, 32);
    CHECK(c[0] == 0);
    CHECK(c[1] == 255);
    CHECK(c[2] == 0);
    CHECK(c[3] == 128);
}

// Alfa a cero en una zona: el color de esos pixeles es irrelevante pero no debe
// contaminar a los vecinos opacos al promediar.
static void test_transparent_neighbours_do_not_bleed(const fs::path& dir)
{
    writeTga(dir / "edge.tga", 128, 128, [](int x, int) {
        return x < 64 ? Rgba{ 255, 0, 0, 255 } : Rgba{ 0, 255, 0, 0 };
    });
    ThumbnailResult r = makeThumbnail(dir / "edge.tga");
    CHECK(r.status == ThumbnailStatus::Ok);
    if (r.rgba.empty()) return;
    CHECK(isRed(pixelAt(r, 31, 32)));
    CHECK(pixelAt(r, 32, 32)[3] == 0);
}

static void test_unreadable_files(const fs::path& dir)
{
    CHECK(makeThumbnail(dir / "no_existe.png").status == ThumbnailStatus::Unreadable);

    std::ofstream(dir / "corrupta.png", std::ios::binary) << "esto no es una imagen, solo texto";
    CHECK(makeThumbnail(dir / "corrupta.png").status == ThumbnailStatus::Unreadable);

    std::ofstream(dir / "vacia.png", std::ios::binary).flush();
    CHECK(makeThumbnail(dir / "vacia.png").status == ThumbnailStatus::Unreadable);
}

// Solo la cabecera de un TGA de 20000x20000 (400 MP): se rechaza por las
// dimensiones, SIN decodificar (no hay cuerpo que decodificar).
static void test_huge_image_is_rejected_without_decoding(const fs::path& dir)
{
    {
        std::ofstream f(dir / "enorme.tga", std::ios::binary);
        uint8_t hdr[18] = {};
        hdr[2] = 2; hdr[12] = 20000 & 0xFF; hdr[13] = 20000 >> 8;
        hdr[14] = 20000 & 0xFF; hdr[15] = 20000 >> 8; hdr[16] = 32; hdr[17] = 0x28;
        f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
    }
    ThumbnailResult r = makeThumbnail(dir / "enorme.tga");
    CHECK(r.status == ThumbnailStatus::TooLarge);
    CHECK(r.rgba.empty());
}

static bool nearF(float a, float b) { return std::fabs(a - b) < 1e-6f; }

// thumbnailUv: casilla -> rect UV con medio texel de margen por lado.
static void test_thumbnail_uv()
{
    const float size = static_cast<float>(kThumbAtlasSize);

    const UvRect first = thumbnailUv(0);
    CHECK(nearF(first.u0, 0.5f / size));
    CHECK(nearF(first.v0, 0.5f / size));
    CHECK(nearF(first.u1, 63.5f / size));
    CHECK(nearF(first.v1, 63.5f / size));

    const UvRect second = thumbnailUv(1);              // siguiente columna, misma fila
    CHECK(nearF(second.u0, 64.5f / size));
    CHECK(nearF(second.v0, 0.5f / size));

    const UvRect row1 = thumbnailUv(kThumbAtlasCells); // primera columna, segunda fila
    CHECK(nearF(row1.u0, 0.5f / size));
    CHECK(nearF(row1.v0, 64.5f / size));

    const UvRect last = thumbnailUv(kThumbSlotCount - 1);
    CHECK(nearF(last.u1, 2047.5f / size));
    CHECK(nearF(last.v1, 2047.5f / size));
}

int main()
{
    fs::path dir = makeDir();
    test_landscape_is_letterboxed(dir);
    test_portrait_is_pillarboxed(dir);
    test_small_image_is_not_upscaled(dir);
    test_exact_size_fills_the_tile(dir);
    test_downscale_keeps_the_halves(dir);
    test_alpha_does_not_darken_color(dir);
    test_transparent_neighbours_do_not_bleed(dir);
    test_unreadable_files(dir);
    test_huge_image_is_rejected_without_decoding(dir);
    test_thumbnail_uv();
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures == 0) std::printf("ALL THUMBNAIL TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Registrar el test**

En `engine/tests/CMakeLists.txt`, junto a `dt_asset_import_tests`:

```cmake
dt_add_test(dt_thumbnail_tests thumbnail_tests.cpp DonTopoEditor)
```

- [ ] **Step 3: Compilar para ver el RED**

Run: `.\configure.bat` y luego `.\build.bat`
Expected: FALLO de compilación en `thumbnail_tests.cpp` con `fatal error C1083: ... 'DonTopo/Editor/Thumbnail.h': No such file or directory`.

- [ ] **Step 4: Crear `ThumbnailAtlas.h`**

Crear `engine/include/DonTopo/Renderer/ThumbnailAtlas.h`:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>

// Lo que comparten el editor y los backends del atlas compartido de miniaturas
// del Content Browser: dimensiones, y nada mas. La logica vive en el editor.
namespace DonTopo {

constexpr uint32_t kThumbCell       = 64;                                    // lado de una casilla
constexpr uint32_t kThumbAtlasCells = 32;                                    // casillas por lado
constexpr uint32_t kThumbAtlasSize  = kThumbCell * kThumbAtlasCells;         // 2048 px
constexpr uint32_t kThumbSlotCount  = kThumbAtlasCells * kThumbAtlasCells;   // 1024

struct UvRect { float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f; };

// Una casilla lista para subir: `rgba` apunta a kThumbCell*kThumbCell*4 bytes.
struct ThumbnailTile
{
    uint32_t       slot = 0;
    const uint8_t* rgba = nullptr;
};

// UV de una casilla, con medio texel de margen por cada lado para que el
// filtrado lineal no sangre el color de la casilla vecina.
inline UvRect thumbnailUv(uint32_t slot)
{
    const uint32_t x = (slot % kThumbAtlasCells) * kThumbCell;
    const uint32_t y = (slot / kThumbAtlasCells) * kThumbCell;
    const float    s = static_cast<float>(kThumbAtlasSize);
    return { (x + 0.5f) / s, (y + 0.5f) / s,
             (x + kThumbCell - 0.5f) / s, (y + kThumbCell - 0.5f) / s };
}

} // namespace DonTopo
```

- [ ] **Step 5: Crear `Thumbnail.h`**

Crear `engine/include/DonTopo/Editor/Thumbnail.h`:

```cpp
#pragma once
#include "DonTopo/Renderer/ThumbnailAtlas.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace DonTopo {

// Mas de estos pixeles de origen y ni se intenta decodificar: un PNG de 16k x
// 16k son 1 GB transitorios en un worker.
constexpr uint64_t kThumbMaxSourcePixels = 100'000'000;

enum class ThumbnailStatus { Ok, Unreadable, TooLarge };

struct ThumbnailResult
{
    ThumbnailStatus      status = ThumbnailStatus::Unreadable;
    std::vector<uint8_t> rgba;   // kThumbCell*kThumbCell*4 si status == Ok; vacio si no
};

// Decodifica path y lo reduce a UNA casilla kThumbCell x kThumbCell RGBA8:
// conserva la proporcion (filtro de caja ponderado por alfa), no amplia lo que
// ya cabe, lo centra y deja el resto transparente. CPU pura: se llama desde un
// worker. Nunca lanza.
ThumbnailResult makeThumbnail(const std::filesystem::path& path);

} // namespace DonTopo
```

- [ ] **Step 6: Crear `Thumbnail.cpp`**

Crear `engine/src/Editor/Thumbnail.cpp`:

```cpp
#include "DonTopo/Editor/Thumbnail.h"

#include <stb_image.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>

namespace DonTopo {

ThumbnailResult makeThumbnail(const std::filesystem::path& path)
{
    ThumbnailResult out;

    // Se lee el fichero entero y se decodifica desde memoria: stbi_load recibe un
    // char* en la codepage local y falla con rutas Unicode en Windows; ifstream no.
    std::ifstream in(path, std::ios::binary);
    if (!in) return out;
    const std::vector<unsigned char> bytes{ std::istreambuf_iterator<char>(in),
                                            std::istreambuf_iterator<char>() };
    if (bytes.empty() || bytes.size() > static_cast<size_t>(INT_MAX)) return out;

    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp) ||
        w <= 0 || h <= 0)
        return out;
    if (static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > kThumbMaxSourcePixels)
    {
        out.status = ThumbnailStatus::TooLarge;
        return out;
    }

    std::unique_ptr<unsigned char, decltype(&stbi_image_free)> px(
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &comp, 4),
        &stbi_image_free);
    if (!px) return out;

    // Tamano destino: lo que ya cabe no se amplia; lo demas, a kThumbCell en el lado largo.
    uint32_t dw = static_cast<uint32_t>(w);
    uint32_t dh = static_cast<uint32_t>(h);
    if (dw > kThumbCell || dh > kThumbCell)
    {
        const double s = static_cast<double>(kThumbCell) / std::max(w, h);
        dw = std::clamp(static_cast<uint32_t>(std::lround(w * s)), 1u, kThumbCell);
        dh = std::clamp(static_cast<uint32_t>(std::lround(h * s)), 1u, kThumbCell);
    }

    out.rgba.assign(static_cast<size_t>(kThumbCell) * kThumbCell * 4, 0);
    const uint32_t offX = (kThumbCell - dw) / 2;
    const uint32_t offY = (kThumbCell - dh) / 2;

    for (uint32_t y = 0; y < dh; ++y)
    {
        const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * h / dh);
        uint32_t       y1 = static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * h / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (uint32_t x = 0; x < dw; ++x)
        {
            const uint32_t x0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * w / dw);
            uint32_t       x1 = static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * w / dw);
            if (x1 <= x0) x1 = x0 + 1;

            // Promedio ponderado por alfa: sin ponderar, un pixel transparente
            // (normalmente negro) oscurece el color de sus vecinos opacos.
            uint64_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
            for (uint32_t sy = y0; sy < y1; ++sy)
                for (uint32_t sx = x0; sx < x1; ++sx)
                {
                    const unsigned char* p = px.get() + (static_cast<size_t>(sy) * w + sx) * 4;
                    const uint64_t a = p[3];
                    sumR += p[0] * a;
                    sumG += p[1] * a;
                    sumB += p[2] * a;
                    sumA += a;
                }
            const uint64_t count = static_cast<uint64_t>(x1 - x0) * (y1 - y0);
            uint8_t* d = &out.rgba[(static_cast<size_t>(offY + y) * kThumbCell + offX + x) * 4];
            if (sumA > 0)
            {
                d[0] = static_cast<uint8_t>((sumR + sumA / 2) / sumA);
                d[1] = static_cast<uint8_t>((sumG + sumA / 2) / sumA);
                d[2] = static_cast<uint8_t>((sumB + sumA / 2) / sumA);
            }
            d[3] = static_cast<uint8_t>((sumA + count / 2) / count);
        }
    }

    out.status = ThumbnailStatus::Ok;
    return out;
}

} // namespace DonTopo
```

- [ ] **Step 7: Registrar el `.cpp`**

En `engine/CMakeLists.txt`, dentro de `add_library(DonTopoEditor STATIC ...)`, justo después de `src/Editor/AssetImport.cpp`:

```cmake
    src/Editor/Thumbnail.cpp
```

- [ ] **Step 8: Compilar y correr**

Run: `.\configure.bat` y luego `.\build.bat`
Expected: build OK.

Run: `.\build-ninja\engine\tests\dt_thumbnail_tests.exe`
Expected: `ALL THUMBNAIL TESTS PASSED`, exit code 0.

Si un `CHECK` falla en `test_huge_image_is_rejected_without_decoding` porque `stbi_info_from_memory` devuelve 0 con una cabecera TGA sin cuerpo, comprobar que la cabecera lleva `hdr[16] = 32` (bits por pixel) y `hdr[2] = 2`; `stbi__tga_info` solo lee la cabecera.

- [ ] **Step 9: Commit**

```bash
git add engine/include/DonTopo/Renderer/ThumbnailAtlas.h engine/include/DonTopo/Editor/Thumbnail.h \
        engine/src/Editor/Thumbnail.cpp engine/tests/thumbnail_tests.cpp \
        engine/CMakeLists.txt engine/tests/CMakeLists.txt
git commit -m "$(cat <<'EOF'
feat(editor): makeThumbnail y constantes del atlas de miniaturas

Decodifica con stb_image desde memoria, rechaza mas de 100 MP sin decodificar y
reduce a una casilla de 64x64 conservando la proporcion (caja ponderada por
alfa, sin ampliar). thumbnailUv da el rect UV de una casilla con medio texel de
margen. Tests headless con TGAs escritos a mano.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2: `ThumbnailSlots` (reparto de casillas con LRU)

**Files:**
- Modify: `engine/include/DonTopo/Editor/Thumbnail.h`
- Modify: `engine/src/Editor/Thumbnail.cpp`
- Modify: `engine/tests/thumbnail_tests.cpp`

**Interfaces:**
- Consumes: `kThumbSlotCount` (Task 1).
- Produces: `class ThumbnailSlots { static constexpr uint32_t kNone; explicit ThumbnailSlots(uint32_t capacity = kThumbSlotCount); void beginFrame(); uint32_t assign(uint64_t key, std::optional<uint64_t>* evicted = nullptr); uint32_t find(uint64_t key); void release(uint64_t key); uint32_t capacity() const; }`. La Task 3 lo usa.

- [ ] **Step 1: Escribir el test que falla**

En `engine/tests/thumbnail_tests.cpp`, antes de `int main()`:

```cpp
// ThumbnailSlots: reparto, reutilizacion de la misma clave, desalojo LRU y
// "no desalojar lo usado este frame".
static void test_slots_basic_assignment()
{
    ThumbnailSlots s(4);
    s.beginFrame();
    const uint32_t a = s.assign(101);
    const uint32_t b = s.assign(102);
    const uint32_t c = s.assign(103);
    const uint32_t d = s.assign(104);
    CHECK(a != ThumbnailSlots::kNone && b != ThumbnailSlots::kNone);
    CHECK(a != b && a != c && a != d && b != c && b != d && c != d);
    CHECK(a < 4 && b < 4 && c < 4 && d < 4);

    CHECK(s.assign(101) == a);        // misma clave: misma casilla
    CHECK(s.find(102) == b);
    CHECK(s.find(999) == ThumbnailSlots::kNone);
    CHECK(s.capacity() == 4);
}

// Todo el atlas usado ESTE frame: no hay a quien desalojar -> kNone, y nada se pierde.
static void test_slots_full_this_frame_returns_none()
{
    ThumbnailSlots s(2);
    s.beginFrame();
    const uint32_t a = s.assign(1);
    const uint32_t b = s.assign(2);
    CHECK(s.assign(3) == ThumbnailSlots::kNone);
    CHECK(s.find(1) == a);
    CHECK(s.find(2) == b);
}

// Frame nuevo: se desaloja la menos usada recientemente y se avisa de cual.
static void test_slots_evict_least_recently_used()
{
    ThumbnailSlots s(4);
    s.beginFrame();                                     // frame A
    const uint32_t slot1 = s.assign(1);
    s.assign(2); s.assign(3); s.assign(4);

    s.beginFrame();                                     // frame B: solo se toca la 2
    s.find(2);

    s.beginFrame();                                     // frame C
    std::optional<uint64_t> evicted;
    const uint32_t slot5 = s.assign(5, &evicted);
    CHECK(slot5 != ThumbnailSlots::kNone);
    CHECK(evicted.has_value());
    // Las candidatas (1, 3, 4) se usaron en el frame A; la 2 en el B. Empate en
    // la mas antigua: gana la de indice de casilla menor, o sea la 1.
    CHECK(evicted && *evicted == 1);
    CHECK(slot5 == slot1);
    CHECK(s.find(1) == ThumbnailSlots::kNone);
    CHECK(s.find(2) != ThumbnailSlots::kNone);          // la tocada en B sobrevive
}

// Una clave usada en el frame actual nunca se desaloja aunque sea la mas antigua en indice.
static void test_slots_never_evict_current_frame()
{
    ThumbnailSlots s(2);
    s.beginFrame();
    s.assign(1); s.assign(2);
    s.beginFrame();
    s.find(1);                                          // 1 usada ahora
    std::optional<uint64_t> evicted;
    s.assign(3, &evicted);
    CHECK(evicted && *evicted == 2);
    CHECK(s.find(1) != ThumbnailSlots::kNone);
}

// release() libera la casilla sin desalojar a nadie.
static void test_slots_release_frees_a_slot()
{
    ThumbnailSlots s(2);
    s.beginFrame();
    s.assign(1);
    const uint32_t b = s.assign(2);
    s.release(2);
    CHECK(s.find(2) == ThumbnailSlots::kNone);
    std::optional<uint64_t> evicted;
    CHECK(s.assign(3, &evicted) == b);                  // reutiliza el hueco liberado
    CHECK(!evicted.has_value());
    s.release(12345);                                   // clave inexistente: no pasa nada
}
```

Y en `main()`, antes de `std::error_code ec;`:

```cpp
    test_slots_basic_assignment();
    test_slots_full_this_frame_returns_none();
    test_slots_evict_least_recently_used();
    test_slots_never_evict_current_frame();
    test_slots_release_frees_a_slot();
```

- [ ] **Step 2: Compilar para ver el RED**

Run: `.\build.bat`
Expected: FALLO — `'ThumbnailSlots': identificador no declarado` en `thumbnail_tests.cpp`.

- [ ] **Step 3: Declarar `ThumbnailSlots`**

En `engine/include/DonTopo/Editor/Thumbnail.h`, añadir a los includes `#include <optional>` y `#include <unordered_map>`, y antes del `} // namespace DonTopo` final:

```cpp
// Reparto de las casillas del atlas entre claves (una por miniatura), con
// desalojo LRU. "Uso" = pedir la casilla en el frame actual (assign/find).
class ThumbnailSlots
{
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;

    explicit ThumbnailSlots(uint32_t capacity = kThumbSlotCount);

    // Una vez por frame, antes de cualquier assign/find de ese frame.
    void beginFrame() { ++m_frame; }

    // Casilla de key: la que ya tenia (marcada como usada este frame) o una
    // nueva. Sin hueco libre desaloja la menos usada recientemente que NO se usara
    // este frame (empate: la de indice menor) y, si `evicted` no es nulo, dice
    // cual. kNone si todas las casillas se usaron este frame.
    uint32_t assign(uint64_t key, std::optional<uint64_t>* evicted = nullptr);

    // Casilla de key sin reservar ninguna (y marcandola como usada); kNone si no esta.
    uint32_t find(uint64_t key);

    // Libera la casilla de key, si tenia. No desaloja a nadie.
    void release(uint64_t key);

    uint32_t capacity() const { return static_cast<uint32_t>(m_slots.size()); }

private:
    struct Slot
    {
        uint64_t key       = 0;
        bool     used      = false;
        uint64_t lastFrame = 0;
    };

    std::vector<Slot>                      m_slots;
    std::unordered_map<uint64_t, uint32_t> m_byKey;
    uint64_t                               m_frame = 1;
};
```

- [ ] **Step 4: Implementar**

En `engine/src/Editor/Thumbnail.cpp`, dentro del namespace, tras `makeThumbnail`:

```cpp
ThumbnailSlots::ThumbnailSlots(uint32_t capacity) : m_slots(capacity) {}

uint32_t ThumbnailSlots::find(uint64_t key)
{
    const auto it = m_byKey.find(key);
    if (it == m_byKey.end()) return kNone;
    m_slots[it->second].lastFrame = m_frame;
    return it->second;
}

uint32_t ThumbnailSlots::assign(uint64_t key, std::optional<uint64_t>* evicted)
{
    if (evicted) evicted->reset();
    if (const uint32_t existing = find(key); existing != kNone)
        return existing;

    uint32_t target = kNone;
    for (uint32_t i = 0; i < m_slots.size(); ++i)
        if (!m_slots[i].used) { target = i; break; }

    if (target == kNone)
    {
        // Sin hueco libre: la menos usada recientemente que NO se uso este frame.
        uint64_t oldest = UINT64_MAX;
        for (uint32_t i = 0; i < m_slots.size(); ++i)
            if (m_slots[i].lastFrame < m_frame && m_slots[i].lastFrame < oldest)
            {
                oldest = m_slots[i].lastFrame;
                target = i;
            }
        if (target == kNone) return kNone;
        m_byKey.erase(m_slots[target].key);
        if (evicted) *evicted = m_slots[target].key;
    }

    m_slots[target] = { key, true, m_frame };
    m_byKey[key]    = target;
    return target;
}

void ThumbnailSlots::release(uint64_t key)
{
    const auto it = m_byKey.find(key);
    if (it == m_byKey.end()) return;
    m_slots[it->second].used = false;
    m_byKey.erase(it);
}
```

- [ ] **Step 5: Compilar y correr**

Run: `.\build.bat`
Run: `.\build-ninja\engine\tests\dt_thumbnail_tests.exe`
Expected: `ALL THUMBNAIL TESTS PASSED`.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/Thumbnail.h engine/src/Editor/Thumbnail.cpp engine/tests/thumbnail_tests.cpp
git commit -m "$(cat <<'EOF'
feat(editor): ThumbnailSlots, reparto de casillas del atlas con LRU

assign/find/release sobre un numero fijo de casillas; sin hueco desaloja la menos
usada recientemente que no se pidio este frame (y avisa de cual), y devuelve
kNone si todo se uso este frame, para no pisar una miniatura que se esta pintando.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3: `ThumbnailCache` (pedidos, decodificación asíncrona, subida en lote)

**Files:**
- Modify: `engine/include/DonTopo/Editor/Thumbnail.h`
- Modify: `engine/src/Editor/Thumbnail.cpp`
- Modify: `engine/tests/thumbnail_tests.cpp`

**Interfaces:**
- Consumes: `makeThumbnail`, `ThumbnailResult`, `ThumbnailStatus` (Task 1); `ThumbnailSlots` (Task 2); `ThumbnailTile`, `UvRect`, `thumbnailUv`, `kThumbSlotCount` (Task 1).
- Produces: `class ThumbnailCache { using Runner = std::function<void(std::function<void()>)>; using Uploader = std::function<bool(const ThumbnailTile*, size_t)>; ThumbnailCache(Runner, Uploader, uint32_t maxInFlight = 4, uint32_t slotCapacity = kThumbSlotCount); void beginFrame(); std::optional<UvRect> request(const std::filesystem::path&); void pump(int maxUploads = 8); void refreshStamps(); void newGeneration(); uint32_t inFlight() const; }`. La Task 4 lo usa.

Reglas de comportamiento que los tests fijan:
- `request` de una ruta nueva devuelve `nullopt` y la encola; nunca bloquea.
- `pump` (1) recoge lo decodificado por los workers, (2) sube como mucho `maxUploads` casillas **en una sola llamada** al `Uploader`, (3) lanza decodificaciones hasta `maxInFlight`.
- Una decodificación fallida o una subida fallida deja la entrada en `Failed`: no se reintenta hasta que cambie el `mtime`.
- Una casilla desalojada por otra entrada hace que `request` devuelva `nullopt` y vuelva a encolar.
- `newGeneration` descarta lo pendiente (en cola, en vuelo, decodificado sin subir) pero **conserva** lo ya subido y lo fallido; un resultado tardío de la generación anterior se ignora.

- [ ] **Step 1: Escribir los tests que fallan**

En `engine/tests/thumbnail_tests.cpp`, añadir `#include <algorithm>`, `#include <chrono>` y `#include <string>` a los includes, y antes de `int main()`:

```cpp
// ── ThumbnailCache ───────────────────────────────────────────────────────────
// El "worker" es una cola manual: el test decide cuando termina cada job, asi
// que todo es determinista. El Uploader es un doble que registra cada lote.
struct CacheHarness
{
    std::vector<std::function<void()>>  pending;   // jobs encolados sin ejecutar
    std::vector<std::vector<uint32_t>>  uploads;   // slots de cada llamada al Uploader
    bool                                uploadOk = true;
    size_t                              maxPendingSeen = 0;
    ThumbnailCache                      cache;

    explicit CacheHarness(uint32_t maxInFlight = 4, uint32_t slotCapacity = kThumbSlotCount)
        : cache(
              [this](std::function<void()> job) {
                  pending.push_back(std::move(job));
                  maxPendingSeen = std::max(maxPendingSeen, pending.size());
              },
              [this](const ThumbnailTile* tiles, size_t count) {
                  std::vector<uint32_t> slots;
                  for (size_t i = 0; i < count; ++i) slots.push_back(tiles[i].slot);
                  uploads.push_back(std::move(slots));
                  return uploadOk;
              },
              maxInFlight, slotCapacity)
    {}

    // Termina todos los jobs encolados (como si los workers acabaran a la vez).
    void runAll()
    {
        std::vector<std::function<void()>> jobs = std::move(pending);
        pending.clear();
        for (auto& job : jobs) job();
    }

    size_t tilesUploaded() const
    {
        size_t n = 0;
        for (const auto& u : uploads) n += u.size();
        return n;
    }
};

static fs::path makeImage(const fs::path& dir, const char* name, Rgba color = kRed)
{
    const fs::path p = dir / name;
    writeTga(p, 8, 8, [color](int, int) { return color; });
    return p;
}

// Ciclo normal: pedir -> (nullopt) -> job -> subida -> pedir de nuevo -> UV.
static void test_cache_request_decode_upload_ready(const fs::path& dir)
{
    CacheHarness h;
    const fs::path f = makeImage(dir, "cache_a.tga");

    h.cache.beginFrame();
    CHECK(!h.cache.request(f));                 // primera vez: nada que enseñar
    h.cache.pump();                             // lanza la decodificacion
    CHECK(h.pending.size() == 1);
    CHECK(h.cache.inFlight() == 1);

    h.runAll();
    h.cache.pump();                             // recoge y sube
    CHECK(h.uploads.size() == 1);
    CHECK(h.tilesUploaded() == 1);
    CHECK(h.cache.inFlight() == 0);

    h.cache.beginFrame();
    const std::optional<UvRect> uv = h.cache.request(f);
    CHECK(uv.has_value());
    if (uv && !h.uploads.empty() && !h.uploads[0].empty())
        CHECK(nearF(uv->u0, thumbnailUv(h.uploads[0][0]).u0));
}

// Review Focus 3: 6 imagenes con tope de 4 en vuelo -> nunca mas de 4 jobs vivos.
static void test_cache_caps_jobs_in_flight(const fs::path& dir)
{
    CacheHarness h(4);
    std::vector<fs::path> files;
    for (int i = 0; i < 6; ++i)
        files.push_back(makeImage(dir, ("cap_" + std::to_string(i) + ".tga").c_str()));

    h.cache.beginFrame();
    for (const auto& f : files) h.cache.request(f);
    h.cache.pump();
    CHECK(h.pending.size() == 4);
    CHECK(h.cache.inFlight() == 4);

    h.runAll();
    h.cache.pump();                             // sube 4 y lanza los 2 que faltaban
    CHECK(h.tilesUploaded() == 4);
    CHECK(h.pending.size() == 2);

    h.runAll();
    h.cache.pump();
    CHECK(h.tilesUploaded() == 6);
    CHECK(h.maxPendingSeen <= 4);
}

// Review Focus 3: tope de subidas por frame y UNA sola llamada al Uploader por lote.
static void test_cache_caps_uploads_per_frame_in_one_batch(const fs::path& dir)
{
    CacheHarness h(8);
    for (int i = 0; i < 5; ++i)
        h.cache.request(makeImage(dir, ("up_" + std::to_string(i) + ".tga").c_str()));
    h.cache.beginFrame();
    h.cache.pump(2);                            // lanza los 5 jobs
    h.runAll();

    h.cache.pump(2);
    CHECK(h.uploads.size() == 1);
    CHECK(h.uploads[0].size() == 2);            // dos casillas en UNA llamada
    h.cache.pump(2);
    CHECK(h.uploads.size() == 2 && h.uploads[1].size() == 2);
    h.cache.pump(2);
    CHECK(h.tilesUploaded() == 5);
}

// Fallo de decodificacion (fichero corrupto o inexistente): se cachea, sin reintentar.
static void test_cache_failed_decode_is_cached(const fs::path& dir)
{
    CacheHarness h;
    std::ofstream(dir / "mala.png", std::ios::binary) << "basura";

    h.cache.beginFrame();
    CHECK(!h.cache.request(dir / "mala.png"));
    h.cache.pump();
    h.runAll();
    h.cache.pump();
    CHECK(h.uploads.empty());

    h.cache.beginFrame();
    CHECK(!h.cache.request(dir / "mala.png"));
    h.cache.pump();
    CHECK(h.pending.empty());                   // no se reencola

    // Un fichero que no existe ni siquiera llega a lanzar un job.
    CHECK(!h.cache.request(dir / "fantasma.png"));
    h.cache.pump();
    CHECK(h.pending.empty());
}

// Fallo de subida (el backend dice que no): Failed, sin reintento.
static void test_cache_failed_upload_is_cached(const fs::path& dir)
{
    CacheHarness h;
    h.uploadOk = false;
    const fs::path f = makeImage(dir, "upfail.tga");

    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump();
    h.runAll();
    h.cache.pump();
    CHECK(h.uploads.size() == 1);

    h.uploadOk = true;
    h.cache.beginFrame();
    CHECK(!h.cache.request(f));
    h.cache.pump();
    CHECK(h.pending.empty());                   // no se reintenta hasta que cambie el mtime
    CHECK(h.uploads.size() == 1);
}

// Cambiar el contenido (mtime nuevo) regenera la miniatura al pasar refreshStamps.
static void test_cache_regenerates_when_mtime_changes(const fs::path& dir)
{
    CacheHarness h;
    const fs::path f = makeImage(dir, "mtime.tga");

    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(h.tilesUploaded() == 1);

    // Mismo nombre, otro contenido, mtime posterior.
    writeTga(f, 8, 8, [](int, int) { return kBlue; });
    std::error_code ec;
    fs::last_write_time(f, fs::last_write_time(f, ec) + std::chrono::seconds(10), ec);

    h.cache.beginFrame();
    h.cache.refreshStamps();                    // detecta el cambio y descarta la entrada
    CHECK(!h.cache.request(f));                 // ahora es una peticion nueva
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(h.tilesUploaded() == 2);

    h.cache.beginFrame();
    CHECK(h.cache.request(f).has_value());
}

// refreshStamps sin cambios no regenera nada.
static void test_cache_refresh_without_changes_is_a_noop(const fs::path& dir)
{
    CacheHarness h;
    const fs::path f = makeImage(dir, "same.tga");
    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump(); h.runAll(); h.cache.pump();

    h.cache.beginFrame();
    h.cache.refreshStamps();
    CHECK(h.cache.request(f).has_value());
    h.cache.pump();
    CHECK(h.pending.empty());
    CHECK(h.tilesUploaded() == 1);
}

// Review Focus 4: cambio de carpeta a mitad de carga -> el resultado tardio se ignora.
static void test_cache_new_generation_discards_pending(const fs::path& dir)
{
    CacheHarness h;
    const fs::path f = makeImage(dir, "gen.tga");

    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump();                             // job en vuelo
    CHECK(h.cache.inFlight() == 1);

    h.cache.newGeneration();                    // el usuario cambia de carpeta
    h.runAll();                                 // el job termina despues
    h.cache.pump();
    CHECK(h.uploads.empty());                   // su resultado no se sube
    CHECK(h.cache.inFlight() == 0);             // pero el hueco en vuelo se libera

    // Volver a pedirlo funciona con normalidad.
    h.cache.beginFrame();
    CHECK(!h.cache.request(f));
    h.cache.pump(); h.runAll(); h.cache.pump();
    CHECK(h.tilesUploaded() == 1);
}

// newGeneration conserva lo ya subido: volver a la carpeta anterior no decodifica otra vez.
static void test_cache_new_generation_keeps_ready_entries(const fs::path& dir)
{
    CacheHarness h;
    const fs::path f = makeImage(dir, "keep.tga");
    h.cache.beginFrame();
    h.cache.request(f);
    h.cache.pump(); h.runAll(); h.cache.pump();

    h.cache.newGeneration();
    h.cache.beginFrame();
    CHECK(h.cache.request(f).has_value());
    h.cache.pump();
    CHECK(h.pending.empty());
}

// Review Focus 6: la casilla la reutiliza otra imagen -> se vuelve a decodificar, no se pinta la ajena.
static void test_cache_evicted_slot_is_requested_again(const fs::path& dir)
{
    CacheHarness h(4, /*slotCapacity=*/2);
    const fs::path a = makeImage(dir, "ev_a.tga");
    const fs::path b = makeImage(dir, "ev_b.tga");
    const fs::path c = makeImage(dir, "ev_c.tga");

    h.cache.beginFrame();
    h.cache.request(a); h.cache.request(b);
    h.cache.pump(); h.runAll(); h.cache.pump();          // a y b ocupan las 2 casillas
    CHECK(h.tilesUploaded() == 2);

    h.cache.beginFrame();
    h.cache.request(c);                                  // nueva: hace falta desalojar una
    h.cache.pump(); h.runAll();
    h.cache.beginFrame();                                // frame siguiente: a y b ya no se usan
    h.cache.pump();                                      // c entra y desaloja a una de las dos
    CHECK(h.tilesUploaded() == 3);

    h.cache.beginFrame();
    const bool aReady = h.cache.request(a).has_value();
    const bool bReady = h.cache.request(b).has_value();
    CHECK(aReady != bReady);                             // exactamente una perdio su casilla
    CHECK(h.cache.request(c).has_value());
}

// Review Focus 5: el resultado llega DESPUES de destruir el cache -> no toca memoria liberada.
static void test_cache_late_result_after_destruction_is_harmless(const fs::path& dir)
{
    const fs::path f = makeImage(dir, "late.tga");
    std::vector<std::function<void()>> orphan;
    {
        ThumbnailCache c(
            [&orphan](std::function<void()> job) { orphan.push_back(std::move(job)); },
            [](const ThumbnailTile*, size_t) { return true; });
        c.beginFrame();
        c.request(f);
        c.pump();
    }                                                     // el cache muere con el job sin ejecutar
    CHECK(orphan.size() == 1);
    for (auto& job : orphan) job();                       // no debe caerse
}
```

`nearF` ya está definido en el fichero (Task 1); si el orden del fichero lo deja después de estos tests, moverlo por encima.

Y en `main()`, antes de `std::error_code ec;`:

```cpp
    test_cache_request_decode_upload_ready(dir);
    test_cache_caps_jobs_in_flight(dir);
    test_cache_caps_uploads_per_frame_in_one_batch(dir);
    test_cache_failed_decode_is_cached(dir);
    test_cache_failed_upload_is_cached(dir);
    test_cache_regenerates_when_mtime_changes(dir);
    test_cache_refresh_without_changes_is_a_noop(dir);
    test_cache_new_generation_discards_pending(dir);
    test_cache_new_generation_keeps_ready_entries(dir);
    test_cache_evicted_slot_is_requested_again(dir);
    test_cache_late_result_after_destruction_is_harmless(dir);
```

- [ ] **Step 2: Compilar para ver el RED**

Run: `.\build.bat`
Expected: FALLO — `'ThumbnailCache': identificador no declarado`.

- [ ] **Step 3: Declarar `ThumbnailCache`**

En `engine/include/DonTopo/Editor/Thumbnail.h`, añadir a los includes `#include <deque>`, `#include <functional>`, `#include <memory>`, `#include <mutex>`, `#include <string>`, y antes del `} // namespace DonTopo` final:

```cpp
// Orquesta las miniaturas: pedidos desde el grid, decodificacion asincrona y
// subida al atlas. NO conoce GPU ni JobSystem: recibe un Runner (para lanzar
// trabajo fuera del hilo principal) y un Uploader (para copiar casillas al
// atlas), asi que se prueba entero sin ninguno de los dos. Todo el estado vive en
// el hilo principal; los workers solo ejecutan makeThumbnail y dejan el
// resultado en una cola con mutex.
class ThumbnailCache
{
public:
    using Runner   = std::function<void(std::function<void()>)>;
    // Copia el lote de casillas al atlas. false = no se pudo (todo el lote falla).
    using Uploader = std::function<bool(const ThumbnailTile* tiles, size_t count)>;

    ThumbnailCache(Runner run, Uploader upload, uint32_t maxInFlight = 4,
                   uint32_t slotCapacity = kThumbSlotCount);
    ThumbnailCache(const ThumbnailCache&)            = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // Una vez por frame, antes de los request() de ese frame.
    void beginFrame();

    // Miniatura de path si ya esta en el atlas; nullopt = todavia no (pendiente,
    // fallida o desalojada) y quien pide sigue con su icono. La primera vez que
    // se pide una ruta se ENCOLA su decodificacion; nunca bloquea.
    std::optional<UvRect> request(const std::filesystem::path& path);

    // Recoge lo decodificado, sube como mucho maxUploads casillas en UNA llamada
    // al Uploader y lanza decodificaciones hasta maxInFlight. Una vez por frame.
    void pump(int maxUploads = 8);

    // Vuelve a leer el mtime de lo pedido el frame anterior o este y descarta lo
    // que cambio, para que se regenere. Lo llama el polling del panel, no cada frame.
    void refreshStamps();

    // Cambio de carpeta: descarta lo pendiente (en cola, en vuelo, decodificado sin
    // subir). Lo ya subido y lo fallido se conserva; un resultado tardio de la
    // generacion anterior se ignora.
    void newGeneration();

    // Decodificaciones lanzadas cuyo resultado aun no se ha recogido.
    uint32_t inFlight() const { return m_inFlight; }

private:
    enum class State { Queued, Running, Decoded, Ready, Failed };

    struct Entry
    {
        std::filesystem::path           path;
        std::filesystem::file_time_type mtime{};
        uint64_t                        key = 0;
        State                           state = State::Queued;
        std::vector<uint8_t>            pixels;            // solo en Decoded
        uint64_t                        lastRequestFrame = 0;
    };

    struct Done
    {
        uint64_t        generation = 0;
        std::string     path;
        ThumbnailResult result;
    };

    // Lo unico que comparten los workers con el hilo principal. En un shared_ptr:
    // un resultado que llega tras destruir el cache cae aqui y no toca memoria muerta.
    struct Shared
    {
        std::mutex        mutex;
        std::vector<Done> done;
    };

    static uint64_t makeKey(const std::filesystem::path& path, std::filesystem::file_time_type mtime);
    void            startJobs();

    Runner                                 m_run;
    Uploader                               m_upload;
    uint32_t                               m_maxInFlight;
    uint32_t                               m_inFlight   = 0;
    uint64_t                               m_frame      = 1;
    uint64_t                               m_generation = 1;
    ThumbnailSlots                         m_slots;
    std::unordered_map<std::string, Entry> m_entries;
    std::deque<std::string>                m_queue;
    std::shared_ptr<Shared>                m_shared = std::make_shared<Shared>();
};
```

- [ ] **Step 4: Implementar**

En `engine/src/Editor/Thumbnail.cpp`, añadir `#include <functional>` a los includes y, dentro del namespace, tras `ThumbnailSlots::release`:

```cpp
ThumbnailCache::ThumbnailCache(Runner run, Uploader upload, uint32_t maxInFlight,
                               uint32_t slotCapacity)
    : m_run(std::move(run))
    , m_upload(std::move(upload))
    , m_maxInFlight(maxInFlight)
    , m_slots(slotCapacity)
{}

void ThumbnailCache::beginFrame()
{
    ++m_frame;
    m_slots.beginFrame();
}

uint64_t ThumbnailCache::makeKey(const std::filesystem::path& path,
                                 std::filesystem::file_time_type mtime)
{
    // Ruta + mtime: cambiar el contenido con el mismo nombre cambia la clave.
    const uint64_t h = std::hash<std::string>{}(path.string());
    const uint64_t t = static_cast<uint64_t>(mtime.time_since_epoch().count());
    return h ^ (t + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2));
}

std::optional<UvRect> ThumbnailCache::request(const std::filesystem::path& path)
{
    const std::string id = path.string();
    auto it = m_entries.find(id);
    if (it == m_entries.end())
    {
        Entry e;
        e.path = path;
        std::error_code ec;
        e.mtime            = std::filesystem::last_write_time(path, ec);
        e.key              = makeKey(path, e.mtime);
        e.state            = ec ? State::Failed : State::Queued;
        e.lastRequestFrame = m_frame;
        const bool queue   = (e.state == State::Queued);
        m_entries.emplace(id, std::move(e));
        if (queue) m_queue.push_back(id);
        return std::nullopt;
    }

    Entry& e = it->second;
    e.lastRequestFrame = m_frame;
    if (e.state == State::Ready)
    {
        const uint32_t slot = m_slots.find(e.key);
        if (slot != ThumbnailSlots::kNone)
            return thumbnailUv(slot);
        // Otra miniatura reutilizo su casilla: hay que decodificarla otra vez.
        e.state = State::Queued;
        m_queue.push_back(id);
    }
    return std::nullopt;
}

void ThumbnailCache::pump(int maxUploads)
{
    // 1. Resultados de los workers.
    std::vector<Done> done;
    {
        std::lock_guard<std::mutex> lock(m_shared->mutex);
        done.swap(m_shared->done);
    }
    for (Done& d : done)
    {
        if (m_inFlight > 0) --m_inFlight;                    // el hueco se libera siempre
        if (d.generation != m_generation) continue;          // carpeta anterior: se ignora
        const auto it = m_entries.find(d.path);
        if (it == m_entries.end() || it->second.state != State::Running) continue;
        Entry& e = it->second;
        if (d.result.status != ThumbnailStatus::Ok)
        {
            e.state = State::Failed;
            continue;
        }
        e.pixels = std::move(d.result.rgba);
        e.state  = State::Decoded;
    }

    // 2. Subida: un lote, una llamada.
    std::vector<ThumbnailTile> tiles;
    std::vector<Entry*>        batch;
    for (auto& kv : m_entries)
    {
        if (static_cast<int>(tiles.size()) >= maxUploads) break;
        Entry& e = kv.second;
        if (e.state != State::Decoded) continue;
        const uint32_t slot = m_slots.assign(e.key);
        if (slot == ThumbnailSlots::kNone) continue;         // atlas lleno este frame: mas tarde
        tiles.push_back({ slot, e.pixels.data() });
        batch.push_back(&e);
    }
    if (!tiles.empty())
    {
        const bool ok = m_upload && m_upload(tiles.data(), tiles.size());
        for (Entry* e : batch)
        {
            if (ok)
            {
                e->state = State::Ready;
            }
            else
            {
                e->state = State::Failed;
                m_slots.release(e->key);
            }
            e->pixels.clear();
            e->pixels.shrink_to_fit();
        }
    }

    // 3. Nuevas decodificaciones, hasta el tope.
    startJobs();
}

void ThumbnailCache::startJobs()
{
    if (!m_run) return;
    while (m_inFlight < m_maxInFlight && !m_queue.empty())
    {
        const std::string id = m_queue.front();
        m_queue.pop_front();
        const auto it = m_entries.find(id);
        if (it == m_entries.end() || it->second.state != State::Queued) continue;

        it->second.state = State::Running;
        ++m_inFlight;
        const std::shared_ptr<Shared>     shared = m_shared;
        const uint64_t                    gen    = m_generation;
        const std::filesystem::path       path   = it->second.path;
        m_run([shared, gen, id, path]() {
            Done d;
            d.generation = gen;
            d.path       = id;
            d.result     = makeThumbnail(path);
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->done.push_back(std::move(d));
        });
    }
}

void ThumbnailCache::refreshStamps()
{
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        Entry& e = it->second;
        // Solo lo que se ha estado viendo, y nunca lo que tiene un job en marcha.
        const bool recent  = e.lastRequestFrame + 1 >= m_frame;
        const bool pending = (e.state == State::Queued || e.state == State::Running);
        if (!recent || pending)
        {
            ++it;
            continue;
        }
        std::error_code ec;
        const auto now = std::filesystem::last_write_time(e.path, ec);
        if (ec || now == e.mtime)
        {
            ++it;
            continue;
        }
        m_slots.release(e.key);      // no-op si nunca tuvo casilla
        it = m_entries.erase(it);    // la siguiente peticion la trata como nueva
    }
}

void ThumbnailCache::newGeneration()
{
    ++m_generation;
    m_queue.clear();
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        const State s = it->second.state;
        if (s == State::Queued || s == State::Running || s == State::Decoded)
            it = m_entries.erase(it);
        else
            ++it;
    }
}
```

- [ ] **Step 5: Compilar y correr**

Run: `.\build.bat`
Run: `.\build-ninja\engine\tests\dt_thumbnail_tests.exe`
Expected: `ALL THUMBNAIL TESTS PASSED`.

Si `test_cache_evicted_slot_is_requested_again` falla por el reparto de casillas (`aReady != bReady` falso), imprimir qué casilla recibió cada una: en el frame de `c`, a y b se usaron en el frame anterior y `c` se asigna en el `pump` del frame siguiente, así que `assign(c)` solo puede desalojar a una de ellas y la otra sigue viva.

- [ ] **Step 6: Commit**

```bash
git add engine/include/DonTopo/Editor/Thumbnail.h engine/src/Editor/Thumbnail.cpp engine/tests/thumbnail_tests.cpp
git commit -m "$(cat <<'EOF'
feat(editor): ThumbnailCache, decodificacion asincrona y subida en lote

request no bloquea: encola makeThumbnail en un Runner (tope de 4 en vuelo) y pump
recoge los resultados y sube hasta 8 casillas en UNA llamada al Uploader. Fallos
de decodificacion o de subida se cachean sin reintento hasta que cambie el mtime;
newGeneration descarta lo pendiente al cambiar de carpeta; el estado compartido
con los workers va en un shared_ptr para que un resultado tardio sea inofensivo.
Runner y Uploader son inyectados: se prueba entero sin GPU ni JobSystem.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4: Interfaz `EditorRenderer`, plomería del `JobSystem` e integración en el panel

Tras esta tarea la funcionalidad está completa **pero apagada**: los dos virtuales nuevos devuelven "no soportado" por defecto y el cache nunca se crea, así que el grid se ve exactamente como hoy. Las Tasks 5 y 6 la encienden en cada backend.

**Files:**
- Modify: `engine/include/DonTopo/Renderer/EditorRenderer.h`
- Modify: `engine/include/DonTopo/Editor/EditorContext.h`
- Modify: `engine/include/DonTopo/Editor/EditorUI.h`
- Modify: `engine/src/Editor/EditorUI.cpp` (construcción de `ctx` en `EditorUI::draw`)
- Modify: `engine/include/DonTopo/Editor/ContentBrowserPanel.h`
- Modify: `engine/src/Editor/ContentBrowserPanel.cpp`
- Modify: `sandbox/src/main.cpp` (las dos ramas)

**Interfaces:**
- Consumes: `ThumbnailCache`, `ThumbnailTile`, `UvRect` (Tasks 1-3); `JobSystem::submit(std::function<void()>)` (`engine/include/DonTopo/Core/JobSystem.h`).
- Produces: `virtual uint64_t EditorRenderer::uiThumbnailAtlasId()` y `virtual bool EditorRenderer::uploadUiThumbnails(const ThumbnailTile*, size_t)` (las Tasks 5 y 6 los implementan); `EditorContext::jobs` (`JobSystem*`); `EditorUI::setJobSystem(JobSystem*)`.

- [ ] **Step 1: Los dos virtuales en `EditorRenderer`**

En `engine/include/DonTopo/Renderer/EditorRenderer.h`, añadir el include tras `#include "DonTopo/Renderer/RendererState.h"`:

```cpp
#include "DonTopo/Renderer/ThumbnailAtlas.h"
```

Y sustituir:

```cpp
            virtual uint64_t uiAtlasTextureId(const UiTextureAtlas* atlas)    = 0;
            virtual float    viewportAspect() const                           = 0;
```

por:

```cpp
            virtual uint64_t uiAtlasTextureId(const UiTextureAtlas* atlas)    = 0;

            // Atlas compartido de miniaturas del Content Browser: UNA textura de
            // kThumbAtlasSize² con un solo descriptor de ImGui (los pools de ImGui
            // son de 48 sets en Vulkan y 16 huecos en D3D12: una textura por
            // miniatura no cabe). No son puros: un backend sin soporte responde
            // "no", y el grid se queda con su icono de color.
            //
            // Id del atlas (lo que ImGui::AddImage entiende por textura), o 0 si el
            // backend no lo soporta o no pudo crearlo. Se crea en la primera
            // llamada y despues devuelve siempre el mismo valor.
            virtual uint64_t uiThumbnailAtlasId() { return 0; }
            // Copia las casillas al atlas, TODAS en una sola espera de GPU. false =
            // no se pudo y no se copió nada fiable (el lote entero falla).
            virtual bool uploadUiThumbnails(const ThumbnailTile* tiles, size_t count)
            {
                (void)tiles;
                (void)count;
                return false;
            }

            virtual float    viewportAspect() const                           = 0;
```

- [ ] **Step 2: `EditorContext::jobs`**

En `engine/include/DonTopo/Editor/EditorContext.h`, añadir tras `class ProjectContext;`:

```cpp
class JobSystem;
```

Y al final de `struct EditorContext`, tras `takeDroppedFiles`:

```cpp
    // Pool de workers del motor (vive en main.cpp, no-propietario). Lo usan las
    // miniaturas del Content Browser para decodificar imagenes fuera del hilo
    // principal. Sin el, no hay miniaturas y el grid se comporta como siempre.
    JobSystem* jobs = nullptr;
```

- [ ] **Step 3: `EditorUI::setJobSystem` y el `ctx`**

En `engine/include/DonTopo/Editor/EditorUI.h`, junto a `setDroppedFilesProvider`:

```cpp
    // Lo rellena main() antes del bucle, como setAssetLoader. Sin el no hay
    // miniaturas en el Content Browser.
    void setJobSystem(JobSystem* jobs) { m_jobSystem = jobs; }
```

y en la sección privada, junto a `m_droppedFilesProvider`:

```cpp
    JobSystem* m_jobSystem = nullptr;
```

Añadir la forward declaration `class JobSystem;` en el bloque de forward declarations de `EditorUI.h` (junto a `class AudioManager;`).

En `engine/src/Editor/EditorUI.cpp`, en la construcción de `EditorContext ctx{ ... }` de `EditorUI::draw`, sustituir la línea final:

```cpp
        m_droppedFilesProvider,
    };
```

por:

```cpp
        m_droppedFilesProvider,
        m_jobSystem,
    };
```

- [ ] **Step 4: Cablear `sandbox/src/main.cpp` (Vulkan y D3D12)**

Rama Vulkan, justo tras el bloque que ya hay de `editor.setDroppedFilesProvider(...)` (que sigue a `editor.setAssetLoader(&assetLoader);`):

```cpp
        editor.setJobSystem(&jobSystem);
```

Rama D3D12, tras `editor.setAssetLoader(&d3dAssets);`:

```cpp
            editor.setJobSystem(&d3dJobs);
```

- [ ] **Step 5: Compilar (nada cambia todavía en pantalla)**

Run: `.\build.bat`
Expected: build OK. Si algún test doble de `EditorContext` falla al compilar por el campo nuevo, no debería: los aggregate-init de los tests dan solo los dos primeros miembros.

- [ ] **Step 6: Miembros del panel**

En `engine/include/DonTopo/Editor/ContentBrowserPanel.h`, añadir el include tras `#include "DonTopo/Editor/AssetImport.h"`:

```cpp
#include "DonTopo/Editor/Thumbnail.h"
```

y `#include <memory>` a los includes de la biblioteca estándar. En la sección privada, junto a `m_selection`:

```cpp
    // Miniaturas de texturas. Se crea de forma perezosa cuando hay renderer con
    // atlas de miniaturas Y JobSystem; sin ellos queda en nullptr y el grid pinta
    // el icono de color de siempre.
    std::unique_ptr<ThumbnailCache> m_thumbs;
    uint64_t                        m_thumbAtlasId = 0;
    std::string                     m_thumbDir;    // carpeta de la generacion actual
```

- [ ] **Step 7: Preparación por frame, en `draw()`**

En `engine/src/Editor/ContentBrowserPanel.cpp`, en el bloque del grid, sustituir:

```cpp
        const double now = ImGui::GetTime();
        if (!m_scanned)
```

por:

```cpp
        // Miniaturas: el cache se crea la primera vez que el backend da atlas y hay
        // JobSystem; cambiar de carpeta abre una generacion nueva (lo pendiente de
        // la anterior ya no interesa) y cada frame empieza con beginFrame.
        if (!m_thumbs && ctx.renderer && ctx.jobs)
        {
            const uint64_t atlasId = ctx.renderer->uiThumbnailAtlasId();
            if (atlasId != 0)
            {
                m_thumbAtlasId = atlasId;
                EditorRenderer* renderer = ctx.renderer;
                JobSystem*      jobs     = ctx.jobs;
                m_thumbs = std::make_unique<ThumbnailCache>(
                    [jobs](std::function<void()> job) { jobs->submit(std::move(job)); },
                    [renderer](const ThumbnailTile* tiles, size_t count) {
                        return renderer->uploadUiThumbnails(tiles, count);
                    });
                m_thumbDir = m_currentDir;
            }
        }
        if (m_thumbs)
        {
            if (m_thumbDir != m_currentDir)
            {
                m_thumbs->newGeneration();
                m_thumbDir = m_currentDir;
            }
            m_thumbs->beginFrame();
        }

        const double now = ImGui::GetTime();
        if (!m_scanned)
```

Añadir a los includes del `.cpp`: `#include "DonTopo/Core/JobSystem.h"`.

- [ ] **Step 8: `refreshStamps` en el polling**

En el mismo fichero, en la rama `else if (visible && now - m_lastPollTime >= kDirPollIntervalSeconds)`, sustituir:

```cpp
            m_lastPollTime = now;
            const std::filesystem::path stillThere =
```

por:

```cpp
            m_lastPollTime = now;
            // Regenerar las miniaturas de lo que cambio de contenido con el mismo nombre.
            if (m_thumbs) m_thumbs->refreshStamps();
            const std::filesystem::path stillThere =
```

- [ ] **Step 9: Pedir y dibujar la miniatura de cada icono visible**

En el bucle de ítems del grid, sustituir:

```cpp
            ImGui::PushID(path.string().c_str());
            // Seleccionado: borde claro y color más vivo. El borde se apila ANTES
```

por:

```cpp
            ImGui::PushID(path.string().c_str());
            // Solo las imágenes que están a la vista: una carpeta de miles de
            // texturas no debe lanzar miles de decodificaciones.
            std::optional<UvRect> thumb;
            if (m_thumbs && kind == AssetKind::Image &&
                ImGui::IsRectVisible(ImVec2(ICON_SIZE, ICON_SIZE)))
                thumb = m_thumbs->request(path);
            // Seleccionado: borde claro y color más vivo. El borde se apila ANTES
```

y sustituir el botón y el cierre del estilo:

```cpp
            const bool clicked = ImGui::Button(label, ImVec2(ICON_SIZE, ICON_SIZE));
            ImGui::PopStyleColor(2);
            if (selected)
            {
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
            }
```

por:

```cpp
            // Con miniatura el botón va sin etiqueta y la imagen se dibuja encima,
            // dentro del borde de selección.
            const bool clicked = ImGui::Button(thumb ? "" : label, ImVec2(ICON_SIZE, ICON_SIZE));
            ImGui::PopStyleColor(2);
            if (selected)
            {
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
            }
            if (thumb)
            {
                const ImVec2 mn  = ImGui::GetItemRectMin();
                const ImVec2 mx  = ImGui::GetItemRectMax();
                const float  pad = 3.0f;
                ImGui::GetWindowDrawList()->AddImage(
                    (ImTextureID)m_thumbAtlasId,
                    ImVec2(mn.x + pad, mn.y + pad), ImVec2(mx.x - pad, mx.y - pad),
                    ImVec2(thumb->u0, thumb->v0), ImVec2(thumb->u1, thumb->v1));
            }
```

- [ ] **Step 10: `pump` una vez por frame**

Tras el bucle de ítems, sustituir:

```cpp
        ImGui::Columns(1);

        // Clic izquierdo en el vacío del grid deselecciona.
```

por:

```cpp
        // Recoge lo decodificado y lo sube al atlas: se ve a partir del frame siguiente.
        if (m_thumbs) m_thumbs->pump();

        ImGui::Columns(1);

        // Clic izquierdo en el vacío del grid deselecciona.
```

- [ ] **Step 11: Compilar, suite completa y humo**

Run: `.\build.bat`
Expected: build OK.

Run (PowerShell): recorrer todos los `.exe` de `build-ninja\engine\tests` y comprobar `Failed: 0`.
Expected: 31/31 (los 30 de antes más `dt_thumbnail_tests`).

Run: `.\build-ninja\sandbox\Sandbox.exe` durante unos segundos y cerrarlo.
Expected: arranca sin crashear y el grid se ve exactamente igual que antes (los virtuales devuelven "no soportado").

- [ ] **Step 12: Commit**

```bash
git add engine/include/DonTopo/Renderer/EditorRenderer.h engine/include/DonTopo/Editor/EditorContext.h \
        engine/include/DonTopo/Editor/EditorUI.h engine/src/Editor/EditorUI.cpp \
        engine/include/DonTopo/Editor/ContentBrowserPanel.h engine/src/Editor/ContentBrowserPanel.cpp \
        sandbox/src/main.cpp
git commit -m "$(cat <<'EOF'
feat(editor): el Content Browser pide y dibuja miniaturas de texturas

EditorRenderer gana uiThumbnailAtlasId y uploadUiThumbnails (por defecto "no
soportado"), EditorContext gana el JobSystem y el panel crea un ThumbnailCache
cuando ambos existen. Pide solo los iconos Image visibles, dibuja la casilla
sobre el boton (sin etiqueta IMG) y regenera lo que cambia de mtime en el polling.
Apagado hasta que los backends lo implementen: sin atlas el grid no cambia.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Backend Vulkan

**Files:**
- Modify: `engine/include/DonTopo/Renderer/GpuResources.h`
- Modify: `engine/src/Renderer/GpuResources.cpp`
- Modify: `engine/include/DonTopo/Renderer/Renderer.h`
- Modify: `engine/src/Renderer/Renderer.cpp`

**Interfaces:**
- Consumes: `EditorRenderer::uiThumbnailAtlasId` / `uploadUiThumbnails`, `ThumbnailTile`, `kThumbAtlasSize`, `kThumbCell`, `kThumbAtlasCells`, `kThumbSlotCount` (Task 4 / Task 1).
- Produces: `GpuResources::ImageTileUpload`, `GpuResources::createBlankImage`, `GpuResources::uploadPixelsToImageRegions`; `Renderer::uiThumbnailAtlasId()` y `Renderer::uploadUiThumbnails()` (overrides).

Sin test automático posible (GPU): se verifica con el build y con la comprobación manual del Step 8. Antes de tocar nada, **`git status` limpio**: si hay que sabotear para el control positivo de syncval (Step 8), se commitea primero.

- [ ] **Step 1: Declaraciones en `GpuResources.h`**

En `engine/include/DonTopo/Renderer/GpuResources.h`, añadir `#include <cstddef>` a los includes. Antes de `class GpuResources {`:

```cpp
// Una región rectangular de píxeles RGBA8 que sustituye a otra dentro de una
// imagen ya creada. Es lo que necesita el atlas de miniaturas: copiar UNA casilla
// sin volver a subir los 16 MB del atlas.
struct ImageTileUpload
{
    uint32_t       x = 0, y = 0;    // esquina superior izquierda dentro de la imagen
    uint32_t       w = 0, h = 0;
    const uint8_t* rgba = nullptr;  // w*h*4 bytes
};
```

Y dentro de la clase, tras `uploadPixelsToImage`:

```cpp
    // Imagen NUEVA, toda transparente y ya en SHADER_READ_ONLY_OPTIMAL. Para
    // texturas que se irán rellenando por regiones (uploadPixelsToImageRegions).
    void createBlankImage(uint32_t w, uint32_t h, VkFormat fmt,
                          VkImage& img, VkDeviceMemory& mem,
                          TransferBatch* batch = nullptr);

    // Copia N regiones a una imagen que ya está en SHADER_READ_ONLY_OPTIMAL y la
    // deja igual. Un solo staging, un solo command buffer y —sin batch— UNA sola
    // espera para las N regiones. La barrera de entrada cubre las lecturas de
    // shader de frames ya enviados a la misma cola.
    void uploadPixelsToImageRegions(VkImage img, const ImageTileUpload* tiles, size_t count,
                                    TransferBatch* batch = nullptr);
```

- [ ] **Step 2: Transición nueva, `createBlankImage` y `uploadPixelsToImageRegions`**

En `engine/src/Renderer/GpuResources.cpp`, en `grabarTransicion`, sustituir:

```cpp
    } else if(oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
```

por:

```cpp
    } else if(oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if(oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        // Reescribir una imagen que la GPU puede estar leyendo en un frame en vuelo:
        // la barrera espera a esas lecturas antes de dejar escribir.
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
```

Y tras el cuerpo de `GpuResources::uploadPixelsToImage` (antes del `namespace {` que abre `grabarTransicion`):

```cpp
void GpuResources::createBlankImage(uint32_t w, uint32_t h, VkFormat fmt,
                                    VkImage& img, VkDeviceMemory& mem, TransferBatch* batch)
{
    createImage(w, h, fmt, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem);

    CmdScope scope(m_gpu, batch);
    grabarTransicion(scope.cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    const VkClearColorValue clear{};   // ceros: transparente
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(scope.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    grabarTransicion(scope.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void GpuResources::uploadPixelsToImageRegions(VkImage img, const ImageTileUpload* tiles,
                                              size_t count, TransferBatch* batch)
{
    if (!tiles || count == 0) return;

    VkDeviceSize total = 0;
    for (size_t i = 0; i < count; ++i)
        total += static_cast<VkDeviceSize>(tiles[i].w) * tiles[i].h * 4;

    VkBuffer       staging    = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 staging, stagingMem);

    void* data = nullptr;
    vkMapMemory(m_gpu.device(), stagingMem, 0, total, 0, &data);
    std::vector<VkBufferImageCopy> regions(count);
    VkDeviceSize offset = 0;
    for (size_t i = 0; i < count; ++i)
    {
        const VkDeviceSize bytes = static_cast<VkDeviceSize>(tiles[i].w) * tiles[i].h * 4;
        memcpy(static_cast<uint8_t*>(data) + offset, tiles[i].rgba, static_cast<size_t>(bytes));

        VkBufferImageCopy& r = regions[i];
        r.bufferOffset                    = offset;
        r.bufferRowLength                 = 0;
        r.bufferImageHeight               = 0;
        r.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.mipLevel       = 0;
        r.imageSubresource.baseArrayLayer = 0;
        r.imageSubresource.layerCount     = 1;
        r.imageOffset                     = { static_cast<int32_t>(tiles[i].x),
                                              static_cast<int32_t>(tiles[i].y), 0 };
        r.imageExtent                     = { tiles[i].w, tiles[i].h, 1 };
        offset += bytes;
    }
    vkUnmapMemory(m_gpu.device(), stagingMem);

    {
        // Un solo scope: sin batch, un submit y una espera para las N regiones.
        CmdScope scope(m_gpu, batch);
        grabarTransicion(scope.cmd, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(scope.cmd, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(regions.size()), regions.data());
        grabarTransicion(scope.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (batch)
        batch->addStaging(staging, stagingMem);
    else
    {
        vkDestroyBuffer(m_gpu.device(), staging, nullptr);
        vkFreeMemory(m_gpu.device(), stagingMem, nullptr);
    }
}
```

- [ ] **Step 3: Declarar los overrides y el estado en `Renderer.h`**

En `engine/include/DonTopo/Renderer/Renderer.h`, tras `uint64_t uiAtlasTextureId(const UiTextureAtlas* atlas) override;`:

```cpp
            uint64_t uiThumbnailAtlasId() override;
            bool     uploadUiThumbnails(const ThumbnailTile* tiles, size_t count) override;
```

Y junto a `m_uiAtlasImGuiId` (sección privada):

```cpp
            // Atlas compartido de miniaturas del Content Browser (ver
            // EditorRenderer::uiThumbnailAtlasId). Se crea la primera vez que se
            // pide; si falla, no se reintenta cada frame.
            VkImage        m_thumbImage    = VK_NULL_HANDLE;
            VkDeviceMemory m_thumbMemory   = VK_NULL_HANDLE;
            VkImageView    m_thumbView     = VK_NULL_HANDLE;
            uint64_t       m_thumbImGuiId  = 0;
            bool           m_thumbFailed   = false;
            void           destroyThumbAtlas();
```

- [ ] **Step 4: Implementar en `Renderer.cpp`**

Tras `Renderer::uiAtlasTextureId(...)`:

```cpp
    namespace
    {
        // El swapchain del editor es B8G8R8A8_SRGB: sampleo sRGB -> lineal, y la
        // escritura vuelve a codificar. Identidad, así que las miniaturas salen con
        // los colores de la imagen. (D3D12 usa otra por su RTV UNORM.)
        constexpr VkFormat kThumbFormat = VK_FORMAT_R8G8B8A8_SRGB;
    }

    uint64_t Renderer::uiThumbnailAtlasId()
    {
        if (m_thumbImGuiId != 0) return m_thumbImGuiId;
        if (m_thumbFailed || !m_ui) return 0;

        try
        {
            m_res.createBlankImage(kThumbAtlasSize, kThumbAtlasSize, kThumbFormat,
                                   m_thumbImage, m_thumbMemory);
            m_res.createTextureImageView(m_thumbImage, m_thumbView, kThumbFormat);
        }
        catch (const std::exception&)
        {
            destroyThumbAtlas();
            m_thumbFailed = true;
            return 0;
        }
        m_thumbImGuiId = m_ui->registerUiTexture((uint64_t)m_uiBatch.sampler(),
                                                 (uint64_t)m_thumbView);
        return m_thumbImGuiId;
    }

    bool Renderer::uploadUiThumbnails(const ThumbnailTile* tiles, size_t count)
    {
        if (m_thumbImGuiId == 0 || !tiles || count == 0) return false;

        std::vector<ImageTileUpload> uploads;
        uploads.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            if (tiles[i].slot >= kThumbSlotCount || !tiles[i].rgba) return false;
            ImageTileUpload u;
            u.x    = (tiles[i].slot % kThumbAtlasCells) * kThumbCell;
            u.y    = (tiles[i].slot / kThumbAtlasCells) * kThumbCell;
            u.w    = kThumbCell;
            u.h    = kThumbCell;
            u.rgba = tiles[i].rgba;
            uploads.push_back(u);
        }
        try
        {
            m_res.uploadPixelsToImageRegions(m_thumbImage, uploads.data(), uploads.size());
        }
        catch (const std::exception&)
        {
            return false;
        }
        return true;
    }

    void Renderer::destroyThumbAtlas()
    {
        const VkDevice device = m_gpu.device();
        if (m_thumbView   != VK_NULL_HANDLE) vkDestroyImageView(device, m_thumbView, nullptr);
        if (m_thumbImage  != VK_NULL_HANDLE) vkDestroyImage(device, m_thumbImage, nullptr);
        if (m_thumbMemory != VK_NULL_HANDLE) vkFreeMemory(device, m_thumbMemory, nullptr);
        m_thumbView    = VK_NULL_HANDLE;
        m_thumbImage   = VK_NULL_HANDLE;
        m_thumbMemory  = VK_NULL_HANDLE;
        m_thumbImGuiId = 0;
    }
```

Y en el apagado, tras `m_uiAtlasImGuiId.clear();`:

```cpp
        // El atlas de miniaturas, con los demás atlas y antes de que muera el device.
        destroyThumbAtlas();
```

- [ ] **Step 5: Compilar y correr la suite**

Run: `.\build.bat`
Expected: build OK.
Run: los 31 tests. Expected: `Failed: 0`.

- [ ] **Step 6: Humo del arranque**

Comprobar que el backend del último proyecto es Vulkan (`build-ninja/sandbox/editor.json` → `lastProject` → `project.json` → `settings.renderBackend`), y si no, forzarlo con la receta de la nota `editor_backend_comes_from_last_project` (backup del `project.json` antes, restaurar después). Arrancar `Sandbox.exe`, ver en la traza `Backend Vulkan activo`, cerrar.
Expected: arranca y cierra sin crash ni mensajes de validación.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Renderer/GpuResources.h engine/src/Renderer/GpuResources.cpp \
        engine/include/DonTopo/Renderer/Renderer.h engine/src/Renderer/Renderer.cpp
git commit -m "$(cat <<'EOF'
feat(renderer): atlas de miniaturas del Content Browser en Vulkan

Imagen R8G8B8A8_SRGB de 2048x2048 creada de forma perezosa y transparente,
registrada UNA vez en ImGui, y subida por regiones: un solo staging, un solo
command buffer y una sola espera para todas las casillas de un frame. La barrera
SHADER_READ->TRANSFER_DST espera a las lecturas de frames en vuelo. Se destruye en
el apagado con los demas atlas.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 8: Verificación manual en Vulkan (la hace el usuario)**

La subida real y el dibujado no se pueden ejercitar sin GPU ni ratón. Con el backend en Vulkan y un proyecto abierto con una carpeta de imágenes de distintos tamaños y proporciones:
1. Las imágenes muestran su miniatura (proporción respetada, letterbox transparente) en lugar del recuadro `IMG`; el resto de tipos siguen igual.
2. Una carpeta de cientos de imágenes: sin tirones al entrar ni al hacer scroll; las miniaturas van apareciendo.
3. Re-exportar una textura con el mismo nombre: su miniatura se actualiza en menos de un segundo.
4. Cambiar de carpeta a mitad de carga: no aparecen miniaturas de la carpeta anterior.
5. Seleccionar y arrastrar una imagen sigue funcionando y el borde de selección se ve alrededor de la miniatura.
6. **Validación:** con `build-ninja/sandbox/vk_layer_settings.txt` presente (syncval activado, sin variables de entorno), entrar en una carpeta con imágenes y salir: sin avisos de validación por stderr. Antes de fiarse de un "0 avisos", control positivo del MISMO tipo de acceso: commitear (ya hecho), comentar la primera `grabarTransicion(... SHADER_READ_ONLY_OPTIMAL, TRANSFER_DST_OPTIMAL)` de `uploadPixelsToImageRegions`, recompilar, repetir y comprobar que syncval **sí** avisa; restaurar con `git checkout -- engine/src/Renderer/GpuResources.cpp` y recompilar.

---

### Task 6: Backend D3D12

**Files:**
- Modify: `engine/include/DonTopo/Renderer/D3D12/D3D12Renderer.h`
- Modify: `engine/src/Renderer/D3D12/D3D12Renderer.cpp`

**Interfaces:**
- Consumes: `EditorRenderer::uiThumbnailAtlasId` / `uploadUiThumbnails`, `ThumbnailTile`, `kThumbAtlasSize`, `kThumbCell`, `kThumbAtlasCells`, `kThumbSlotCount` (Task 4 / Task 1).
- Produces: `D3D12Renderer::uiThumbnailAtlasId()` y `D3D12Renderer::uploadUiThumbnails()` (overrides); `Impl::ensureThumbAtlas()` y `Impl::uploadThumbnailTiles()`.

Sin test automático posible. Ojo con el cierre: `D3D12MA` hace `assert` si al destruir el allocator queda una allocation viva (exit code 3 en Debug, sin dump), así que el atlas se suelta **junto a `uiAtlasTextures`**, antes de `allocator->Release()`.

- [ ] **Step 1: Hueco propio en el heap de SRV**

En `engine/src/Renderer/D3D12/D3D12Renderer.cpp`, sustituir:

```cpp
constexpr UINT kSrvUiAtlas    = kSrvViewport + 1;
constexpr UINT kMaxUiAtlases  = 16;
```

por:

```cpp
constexpr UINT kSrvUiAtlas    = kSrvViewport + 1;
constexpr UINT kMaxUiAtlases  = 16;

// Atlas compartido de miniaturas del Content Browser: UN hueco propio, para no
// gastar uno de los 16 de la UI 2D del juego.
constexpr UINT kSrvThumbAtlas = kSrvUiAtlas + kMaxUiAtlases;
```

y sustituir:

```cpp
constexpr UINT kSrvProbes   = kSrvUiAtlas + kMaxUiAtlases;
```

por:

```cpp
constexpr UINT kSrvProbes   = kSrvThumbAtlas + 1;
```

- [ ] **Step 2: Declaraciones**

En `D3D12Renderer.h`, tras `uint64_t uiAtlasTextureId(const UiTextureAtlas* atlas) override;`:

```cpp
    uint64_t uiThumbnailAtlasId() override;
    bool     uploadUiThumbnails(const ThumbnailTile* tiles, size_t count) override;
```

En `D3D12Renderer.cpp`, en `struct Impl`, junto a `uiNextAtlasSlot`:

```cpp
    // Atlas compartido de miniaturas (ver EditorRenderer::uiThumbnailAtlasId). Se
    // crea la primera vez que se pide; si falla, no se reintenta cada frame.
    D3D12MA::Allocation* thumbAtlas       = nullptr;
    bool                 thumbAtlasFailed = false;
    bool ensureThumbAtlas();
    bool uploadThumbnailTiles(const ThumbnailTile* tiles, size_t count);
```

- [ ] **Step 3: Implementar `ensureThumbAtlas` y `uploadThumbnailTiles`**

Tras `D3D12Renderer::Impl::registerUiAtlas(...)`:

```cpp
namespace
{
    // El RTV de ImGui en D3D12 es R8G8B8A8_UNORM (EditorUI::initUiD3D12 /
    // uiInfo.d3dRtvFormat): sampleando un SRV sRGB hacia un RTV UNORM la imagen
    // saldria mas oscura. El atlas es UNORM, asi que los bytes sRGB de la imagen
    // llegan tal cual a pantalla. Si la comprobacion manual dice otra cosa, esta es
    // la unica constante que cambiar.
    constexpr DXGI_FORMAT kThumbAtlasFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
}

bool D3D12Renderer::Impl::ensureThumbAtlas()
{
    if (thumbAtlas) return true;
    if (thumbAtlasFailed || !initialized || !srvHeap) return false;

    try
    {
        // Transparente. uploadTexture la deja en PIXEL_SHADER_RESOURCE y crea su SRV.
        const std::vector<uint8_t> blank(static_cast<size_t>(kThumbAtlasSize) * kThumbAtlasSize * 4, 0);
        thumbAtlas = uploadTexture(blank.data(), kThumbAtlasSize, kThumbAtlasSize, 1,
                                   kThumbAtlasFormat, 4, kSrvThumbAtlas);
    }
    catch (const std::exception&)
    {
        thumbAtlas = nullptr;
    }
    if (!thumbAtlas)
    {
        thumbAtlasFailed = true;
        return false;
    }
    thumbAtlas->SetName(L"ThumbnailAtlas");
    diagLog("ensureThumbAtlas: atlas de miniaturas " + std::to_string(kThumbAtlasSize) + "x" +
            std::to_string(kThumbAtlasSize) + " en el slot " + std::to_string(kSrvThumbAtlas));
    return true;
}

bool D3D12Renderer::Impl::uploadThumbnailTiles(const ThumbnailTile* tiles, size_t count)
{
    if (!thumbAtlas || !tiles || count == 0) return false;

    // Una casilla son 64 filas de 256 bytes: la fila ya cumple
    // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT y la casilla entera (16 KiB) cumple
    // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, asi que las N casillas van
    // contiguas en el staging sin relleno.
    constexpr UINT kRowBytes  = kThumbCell * 4;
    constexpr UINT kTileBytes = kRowBytes * kThumbCell;
    static_assert(kRowBytes % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0);
    static_assert(kTileBytes % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0);

    for (size_t i = 0; i < count; ++i)
        if (tiles[i].slot >= kThumbSlotCount || !tiles[i].rgba)
            return false;

    try
    {
        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width            = static_cast<UINT64>(kTileBytes) * count;
        bufferDesc.Height           = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels        = 1;
        bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12MA::ALLOCATION_DESC uploadDesc{};
        uploadDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

        D3D12MA::Allocation* staging = nullptr;
        throwIfFailed(allocator->CreateResource(&uploadDesc, &bufferDesc,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                &staging, IID_NULL, nullptr),
                      "D3D12MA::Allocator::CreateResource(staging de miniaturas)");

        uint8_t*          mapped = nullptr;
        const D3D12_RANGE noRead{0, 0};
        HRESULT hr = staging->GetResource()->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
        if (FAILED(hr)) {
            staging->Release();
            throwIfFailed(hr, "ID3D12Resource::Map(staging de miniaturas)");
        }
        for (size_t i = 0; i < count; ++i)
            std::memcpy(mapped + i * kTileBytes, tiles[i].rgba, kTileBytes);
        staging->GetResource()->Unmap(0, nullptr);

        // Mismo camino que uploadTexture: una lista, una espera. Las N casillas
        // comparten las dos barreras y la espera.
        throwIfFailed(allocators[frameIndex]->Reset(), "ID3D12CommandAllocator::Reset(miniaturas)");
        throwIfFailed(commandList->Reset(allocators[frameIndex].Get(), nullptr),
                      "ID3D12GraphicsCommandList::Reset(miniaturas)");

        D3D12_RESOURCE_BARRIER toCopy{};
        toCopy.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource   = thumbAtlas->GetResource();
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toCopy.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &toCopy);

        for (size_t i = 0; i < count; ++i)
        {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource        = thumbAtlas->GetResource();
            dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource                          = staging->GetResource();
            src.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset             = static_cast<UINT64>(i) * kTileBytes;
            src.PlacedFootprint.Footprint.Format   = kThumbAtlasFormat;
            src.PlacedFootprint.Footprint.Width    = kThumbCell;
            src.PlacedFootprint.Footprint.Height   = kThumbCell;
            src.PlacedFootprint.Footprint.Depth    = 1;
            src.PlacedFootprint.Footprint.RowPitch = kRowBytes;

            const UINT x = (tiles[i].slot % kThumbAtlasCells) * kThumbCell;
            const UINT y = (tiles[i].slot / kThumbAtlasCells) * kThumbCell;
            commandList->CopyTextureRegion(&dst, x, y, 0, &src, nullptr);
        }

        D3D12_RESOURCE_BARRIER toShader = toCopy;
        toShader.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toShader.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        commandList->ResourceBarrier(1, &toShader);

        throwIfFailed(commandList->Close(), "ID3D12GraphicsCommandList::Close(miniaturas)");
        ID3D12CommandList* lists[] = {commandList.Get()};
        queue->ExecuteCommandLists(1, lists);
        waitForGpu();
        staging->Release();
    }
    catch (const std::exception&)
    {
        return false;
    }
    return true;
}
```

- [ ] **Step 3b: Los dos métodos públicos**

Tras `D3D12Renderer::uiAtlasTextureId(...)`:

```cpp
uint64_t D3D12Renderer::uiThumbnailAtlasId()
{
    Impl& d = *m_impl;
    if (!d.ensureThumbAtlas())
        return 0;

    // Mismo criterio que uiAtlasTextureId: el handle de GPU del SRV ES lo que
    // ImGui entiende por textura.
    D3D12_GPU_DESCRIPTOR_HANDLE handle = d.srvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(kSrvThumbAtlas) * d.srvSize;
    return handle.ptr;
}

bool D3D12Renderer::uploadUiThumbnails(const ThumbnailTile* tiles, size_t count)
{
    return m_impl->uploadThumbnailTiles(tiles, count);
}
```

- [ ] **Step 4: Soltar el atlas antes del allocator**

En el apagado, justo tras `d.uiAtlasTextures.clear();`:

```cpp
    // El atlas de miniaturas, con los demas atlas de UI y por la misma razon: su
    // allocation tiene que estar suelta antes de allocator->Release() o D3D12MA
    // hace assert al destruirse (abort en Debug, exit code 3, sin dump).
    if (d.thumbAtlas) {
        d.thumbAtlas->Release();
        d.thumbAtlas = nullptr;
    }
    d.thumbAtlasFailed = false;
```

- [ ] **Step 5: Compilar y correr la suite**

Run: `.\build.bat`
Expected: build OK (revisar que ningún `static_assert` de tamaño del heap proteste por el `+1` de `kSrvProbes`).
Run: los 31 tests. Expected: `Failed: 0`.

- [ ] **Step 6: Humo del arranque y cierre en D3D12**

Forzar el backend D3D12 con la receta de `editor_backend_comes_from_last_project` (backup del `project.json` antes, restaurar después, comprobar `Backend DirectX 12 activo` en la primera línea de la traza). Arrancar y cerrar `Sandbox.exe`, y **borrar el `d3d12_diag.log` que deja en la raíz**.
Expected: arranca y cierra sin crash.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Renderer/D3D12/D3D12Renderer.h engine/src/Renderer/D3D12/D3D12Renderer.cpp
git commit -m "$(cat <<'EOF'
feat(renderer): atlas de miniaturas del Content Browser en D3D12

Textura R8G8B8A8_UNORM de 2048x2048 (el RTV de ImGui es UNORM) con un hueco propio
en el heap de SRV, creada de forma perezosa con uploadTexture y rellenada con
CopyTextureRegion: N casillas, dos barreras y una espera por frame. Se suelta
junto a uiAtlasTextures, antes del allocator, para no disparar el assert de
D3D12MA al cerrar.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 8: Verificación manual en D3D12 (la hace el usuario)**

Con el backend en D3D12 (`Backend DirectX 12 activo` en la traza):
1. Las mismas comprobaciones 1-5 del Step 8 de la Task 5.
2. **Colores:** abrir la misma imagen en el Sprite Editor y comparar con su miniatura, y comparar la miniatura de D3D12 con la de Vulkan: mismo brillo y mismos colores. Si la miniatura sale más oscura o más clara, cambiar `kThumbAtlasFormat` a `DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` (o viceversa), recompilar y repetir.
3. **Cierre limpio en Debug:** cerrar el editor con miniaturas cargadas. No debe salir la ventana de error de Windows ni terminar con exit code 3 (assert de D3D12MA por una allocation sin liberar). Lanzarlo con stderr capturado para ver el mensaje si ocurre.

---

### Task 7: Cerrar el audit y documentar

**Files:**
- Modify: `docs/assets-editor-audit.md`
- Modify: `README.md`

**Interfaces:**
- Consumes: todo lo anterior (esta tarea solo documenta lo ya hecho).
- Produces: nada que consuma otra tarea.

Esta tarea también corrige una deuda: `docs/assets-editor-audit.md` no se actualizó tras los quick wins, el mover assets, la multiselección ni el polling, y sus tablas siguen diciendo que faltan.

- [ ] **Step 1: Actualizar las filas del audit**

En `docs/assets-editor-audit.md`, dejar el estado de cada fila así (mantener el formato de las tablas existentes: añadir `· **CERRADO**` y el commit, como en `docs/animation-audit.md`):

| Fila | Estado |
|---|---|
| U1 (import real) | CERRADO — `AssetImport`, `acceptOrImportAsset`; merge `8e65a5f` |
| U2 (drop desde Explorer) | CERRADO — `glfwSetDropCallback` en las dos ramas de `main.cpp`; merge `8e65a5f` |
| U3 (miniaturas) | CERRADO solo para **texturas** — modelos y materiales siguen sin miniatura |
| U4 (breadcrumb / crear carpeta) | CERRADO — breadcrumb `ab3994c`, Create Folder `5561836` |
| U5 (menú Create) | CERRADO para **Create Folder**; no hay más tipos porque el Core no tiene asset de Material ni de otro tipo creable |
| U6 (búsqueda y filtro) | CERRADO — `5561836` |
| U7 (multiselección) | CERRADO — `338de1d`; mover con arrastre en `d4eb633` |
| U8 (import settings) | ABIERTO — depende de decidir dónde vive el metadato en el Core |
| U9 (refresco) | CERRADO por **polling** de la carpeta actual cada 0,5 s — `56dad33`; no se hizo un watcher nativo a propósito |

En la tabla de la §3 (comparación con Unity), cambiar los estados de las capacidades 1, 3, 4, 5, 7 y 8 a `EXISTE` (la 8 con la nota "por polling, latencia ≤ 0,5 s"), la 2 a `EXISTE (solo texturas)`, y dejar 6 (import settings) como `NO EXISTE`.

- [ ] **Step 2: Actualizar el README**

En `README.md`, en el párrafo de "Scene, Play Mode & Undo" (línea ~760), sustituir:

```
The **Content Browser** browses,
renames and deletes assets, and is the drag source for models, textures and skybox folders.
```

por:

```
The **Content Browser** browses the project as a folder
tree with a breadcrumb, filters by name and by asset type, creates folders, renames, moves (by
dragging onto a folder, with scene references rewritten) and deletes assets — one or several at
once with Ctrl/Shift selection — shows real thumbnails for textures, follows changes made on disk
outside the editor, and imports files dropped from the OS file explorer or picked in a Browse
dialog by copying them into `assets/Imported/`. It is also the drag source for models, textures
and skybox folders.
```

- [ ] **Step 3: Commit**

```bash
git add docs/assets-editor-audit.md README.md
git commit -m "$(cat <<'EOF'
docs: cerrar los hallazgos del audit del Content Browser y actualizar el README

El audit seguia diciendo que faltaban el import, el breadcrumb, los filtros, la
multiseleccion, el mover y el refresco; U3 pasa a cerrado solo para texturas y U8
sigue abierto. El README describia el Content Browser en una sola frase.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```
