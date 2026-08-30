#pragma once
#include <glm/glm.hpp>

namespace DonTopo
{
    // Grosor del contorno de seleccion, PROPORCIONAL al tamano del objeto.
    //
    // Con un grosor fijo, un objeto grande apenas mostraria borde y uno pequeno
    // quedaria engullido por el. El minimo de una unidad evita que una malla
    // diminuta se quede sin contorno.
    //
    // Compartido por los dos backends: estaba escrito dos veces con el mismo
    // factor. Que se descuadren no rompe nada —el contorno solo se ve mas gordo
    // o mas fino en un backend—, y por eso mismo pasaria desapercibido.
    constexpr float OUTLINE_FACTOR = 0.009f;

    // localExtent = media dimension mayor de la malla en su espacio local, sin
    // escalar. transform = su matriz de mundo, de la que se saca la escala
    // efectiva: la mayor de las tres columnas, para que una escala no uniforme
    // no adelgace el contorno por el eje corto.
    inline float outlineThickness(float localExtent, const glm::mat4& transform)
    {
        const float escala = (glm::max)(
            glm::length(glm::vec3(transform[0])),
            (glm::max)(glm::length(glm::vec3(transform[1])),
                       glm::length(glm::vec3(transform[2]))));
        return (glm::max)(localExtent * escala, 1.0f) * OUTLINE_FACTOR;
    }
}
