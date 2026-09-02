#pragma once
#include <cstdio>
#include <cstddef>

namespace DonTopo
{
    // Como se escribe un tiempo de GPU por pase, en UN solo sitio.
    //
    // La regla que importa es la del valor no medido. Un pase devuelve <= 0
    // cuando no ha corrido este frame: o su efecto esta apagado, o la captura
    // todavia no tiene los dos frames que necesita. El panel de Performance ya
    // lo pintaba como "--", pero las ocho lineas del menu View hacian "%.3f" a
    // secas y sacaban "0.000 ms" (H57).
    //
    // Y eso no es un detalle de estilo: "0.000 ms" se lee como «este efecto es
    // gratis», que es justo la conclusion contraria a «no hay medida». Peor aun,
    // se confunde con un pase que de verdad no cuesta nada. El numero esta ahi
    // para decidir si un efecto sale caro, asi que mentir en ese caso vacia de
    // sentido al resto.
    //
    // Devuelve `buf` para poder usarlo directamente en un ImGui::Text.
    inline const char* gpuMsText(float ms, char* buf, std::size_t n)
    {
        if (n == 0) return buf;
        if (ms > 0.0f) std::snprintf(buf, n, "%.3f", (double)ms);
        else           std::snprintf(buf, n, "--");
        return buf;
    }

    // Tamano de sobra para "%.3f" de cualquier float representable.
    constexpr std::size_t kGpuMsTextSize = 32;
}
