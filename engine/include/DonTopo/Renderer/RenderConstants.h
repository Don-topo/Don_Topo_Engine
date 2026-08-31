#pragma once
#include <cstdint>

namespace DonTopo
{
    // Numeros que los DOS backends tienen que ver iguales y que no dependen de
    // ninguna API grafica. Estaban declarados por duplicado —una copia en el
    // camino Vulkan y otra en D3D12Renderer.cpp, esta con un comentario
    // admitiendo que eran "los mismos valores que el camino Vulkan"—.
    //
    // Ese comentario era la unica defensa: nada obliga a que las dos copias
    // coincidan, y descuadrarlas no da error en ninguna capa de validacion. La
    // imagen sale distinta segun el backend y solo se ve comparando capturas.

    // ── IBL ─────────────────────────────────────────────────────────────────
    // Lado de los dos cubemaps precomputados del ambiente.
    constexpr uint32_t IBL_IRRADIANCE_SIZE = 32;
    constexpr uint32_t IBL_PREFILTER_SIZE  = 128;
    // Mips del prefiltrado especular: el prefiltrado reparte la rugosidad entre
    // ellos y pbr.frag lo da por hecho.
    //
    // OJO, este numero vive en TRES sitios y solo dos pueden compartirse: aqui,
    // y como `#define IBL_PREFILTER_MIPS` en shaders/pbr.frag. Un shader no
    // puede incluir un header de C++, y meterlo en el bloque UBO lo desplazaria
    // en silencio para los seis shaders que lo declaran (std140). Si cambia
    // aqui, hay que cambiarlo ALLI a mano.
    constexpr uint32_t IBL_PREFILTER_MIPS  = 5;

    // ── Bloom ───────────────────────────────────────────────────────────────
    // Niveles de la cadena de reduccion. Mas niveles = halo mas ancho y mas
    // barato de calcular, pero por debajo de unos pocos pixeles el mip deja de
    // aportar y solo cuesta dos dispatches.
    constexpr int BLOOM_MIPS = 5;
}
