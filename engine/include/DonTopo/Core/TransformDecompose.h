#pragma once
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace DonTopo
{
    // Descomponer una matriz de transformación sin quedarse con basura cuando no
    // se puede.
    //
    // `glm::decompose` devuelve **bool**, y ante una matriz singular —un eje a
    // escala 0, que es lo que sale de escribir un 0 en Scale.Y del inspector—
    // devuelve `false` **sin escribir ninguna de sus salidas**. Quien no mire
    // ese retorno se queda con sus variables locales SIN INICIALIZAR: en Debug,
    // el patrón 0xCDCDCDCD de la CRT, que como float es -1.07374e+08. En Release
    // no hay patrón: es memoria de pila cualquiera, el mismo fallo sin un valor
    // reconocible con el que atarlo.
    //
    // Eso ya costó un crash mudo del editor, una congelación al entrar en Play y
    // objetos saltando a posiciones de 1e8 — tres síntomas de este único fallo,
    // que fueron apareciendo de uno en uno según se arreglaba el anterior.
    //
    // Casi nada hace falta descomponer, y por eso esto puede responder bien
    // aunque `glm::decompose` se rinda:
    //
    //  - La TRASLACIÓN es la cuarta columna, y lo sigue siendo con una matriz
    //    singular.
    //  - La ESCALA son las longitudes de las columnas. Con un eje a 0 da
    //    (2, 0, 2), que es la respuesta correcta y no una aproximación.
    //  - La ROTACIÓN es lo único que se pierde de verdad, y es que no existe: un
    //    eje aplastado no define ninguna orientación. Identidad.
    //
    // Devuelve lo que devolvió `glm::decompose`, por si al llamante le importa
    // distinguir «rotación de verdad» de «no había ninguna que sacar». Las
    // salidas son válidas en los dos casos.
    inline bool decomposeTransform(const glm::mat4& m,
                                   glm::vec3* outPos   = nullptr,
                                   glm::quat* outRot   = nullptr,
                                   glm::vec3* outScale = nullptr)
    {
        // De `glm::decompose` solo se usa la ROTACIÓN, y solo cuando dice que
        // pudo. Las otras salidas se ignoran a propósito —la posición y la escala
        // se sacan de la matriz, abajo—, así que da igual lo que deje escrito
        // en ellas. Aun así van inicializadas: si algún día alguien las lee de
        // aquí, que encuentre valores neutros y no memoria virgen.
        glm::vec3 scale{1.0f}, translation{0.0f}, skew{0.0f};
        glm::vec4 perspective{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        const bool ok = glm::decompose(m, scale, rotation, translation, skew, perspective);

        // La rotación es la ÚNICA salida del decompose que sobrevive, así que es
        // la única que hay que descartar cuando no pudo descomponer. Comprobado
        // saboteando las dos mitades A LA VEZ —dejando basura en la variable y
        // quitando este if—: el test se pone rojo en los cuatro componentes del
        // cuaternión. Por separado no basta, y eso dice que las dos hacen falta.
        if (!ok)
            rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

        if (outPos)   *outPos = glm::vec3(m[3]);
        if (outRot)   *outRot = rotation;
        if (outScale) *outScale = glm::vec3(glm::length(glm::vec3(m[0])),
                                            glm::length(glm::vec3(m[1])),
                                            glm::length(glm::vec3(m[2])));
        return ok;
    }
}
