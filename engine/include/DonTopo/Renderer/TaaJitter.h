#pragma once
#include <glm/glm.hpp>
#include <cstdint>

namespace DonTopo
{
    // Jitter subpixel del TAA. Compartido por los dos backends a proposito:
    // estaba escrito dos veces —AaPass.cpp y D3D12Renderer.cpp— con la misma
    // secuencia, el mismo ciclo y la misma forma de meterlo en la proyeccion.
    //
    // Que se descuadren no da ningun error: el TAA sigue convergiendo, solo que
    // a una imagen ligeramente distinta segun el backend, y eso solo se ve
    // poniendo las dos capturas una encima de la otra.

    // Secuencia de Halton en base b: la sucesion de baja discrepancia con la que
    // el TAA reparte las muestras dentro del pixel. Cubre el area mucho mas
    // uniformemente que un aleatorio, que es lo que hace que el promedio
    // temporal converja a un supersampling de verdad.
    inline float halton(uint32_t index, uint32_t base)
    {
        float result = 0.0f;
        float f      = 1.0f;
        while (index > 0)
        {
            f      /= static_cast<float>(base);
            result += f * static_cast<float>(index % base);
            index  /= base;
        }
        return result;
    }

    // Cuantas posiciones antes de repetir. Suficientes para que el promedio sea
    // estable, y pocas para que el ciclo no se note al parar la camara.
    constexpr uint32_t TAA_JITTER_CYCLE = 16;

    // Desplazamiento de este frame, en PIXELES, dentro de [-0.5, 0.5] * scale.
    // Avanza el indice, asi que se llama UNA vez por frame.
    inline glm::vec2 taaJitterPixels(uint32_t& index, float scale)
    {
        const glm::vec2 j((halton(index + 1, 2) - 0.5f) * scale,
                          (halton(index + 1, 3) - 0.5f) * scale);
        index = (index + 1) % TAA_JITTER_CYCLE;
        return j;
    }

    // Mete el jitter en la proyeccion. En clip space el ancho completo es 2, de
    // ahi el factor. Va sobre la columna de la Z para que el desplazamiento sea
    // constante en pantalla a cualquier profundidad; sobre la de traslacion
    // dependeria de la distancia y el TAA promediaria muestras que no cubren el
    // mismo area.
    //
    // width/height son los del render INTERNO, no los de la ventana: con SSAA
    // no son lo mismo y el jitter tiene que medirse en el pixel que se dibuja.
    inline void applyTaaJitter(glm::mat4& proj, const glm::vec2& jitterPx,
                               float width, float height)
    {
        if (width <= 0.0f || height <= 0.0f) return;
        proj[2][0] += 2.0f * jitterPx.x / width;
        proj[2][1] += 2.0f * jitterPx.y / height;
    }
}
