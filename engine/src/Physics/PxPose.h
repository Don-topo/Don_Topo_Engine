#pragma once
// Cabecera INTERNA de física: incluye PhysX, así que no vive en
// engine/include —arrastrarlo a la API pública obligaría a compilar contra
// PhysX a todo el que use un collider—. La incluyen los .cpp de PhysicsManager
// y de los cuatro colliders.
#include <PxPhysicsAPI.h>

#include "DonTopo/Core/TransformDecompose.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace DonTopo
{
    // Pose y escala de PhysX a partir de un worldTransform, a prueba de matrices
    // que no se pueden descomponer.
    //
    // EL BUG QUE ESTO CIERRA. Poner Scale.Y = 0 en el inspector hace la matriz
    // singular. `glm::decompose` devuelve **bool**, y ante una matriz así
    // devuelve `false` SIN ESCRIBIR ninguna de sus salidas. Nadie miraba ese
    // valor de retorno —ni aquí ni en las doce copias de las que salió esta
    // función—, así que lo que se usaba eran las variables locales **sin
    // inicializar**: en Debug, el patrón 0xCDCDCDCD de la CRT, que como float
    // es -1.07374e+08.
    //
    // De ese único fallo salían tres síntomas que parecían bugs distintos:
    //
    //  1. La pose iba con ese valor en la rotación. PhysX la rechaza, devuelve
    //     null, el physxCheck de PhysicsManager lanza, nadie lo captura y el
    //     proceso MUERE: exit code 3 y CERO salida, ni siquiera lo ya impreso.
    //  2. La escala también, así que la caja pasaba a medir 1e8. Al darle
    //     Rigidbody —o sea, al entrar en Play— el tensor de inercia desbordaba y
    //     saltaba el PX_ASSERT de ExtInertiaTensor.h, que en Debug congela la
    //     aplicación con el diálogo modal de la CRT y luego la mata.
    //  3. Y la traslación, que el editor leía de vuelta con getWorldTransform()
    //     y escribía en el GameObject: posiciones de 1e8 y rotaciones perdidas.
    //
    // En Release no habría patrón reconocible: sería memoria de pila cualquiera,
    // o sea el mismo fallo sin un valor con el que atarlo.
    //
    // Lo que se recupera sin descomposición, que es casi todo:
    //  - La TRASLACIÓN es la cuarta columna, y lo sigue siendo aunque la matriz
    //    sea singular.
    //  - La ESCALA son las longitudes de las columnas. Con un eje a 0 da
    //    (2, 0, 2), que es la respuesta correcta; los colliders ya acotan ese 0
    //    a un mínimo positivo al hornearla en su geometría.
    //  - La ROTACIÓN es lo único que se pierde de verdad, y es que no existe: un
    //    eje aplastado no define ninguna orientación. Identidad, que deja al
    //    objeto sin girar en vez de matar el editor.
    //
    // `outScale` es opcional porque solo la mitad de los llamantes la usan.
    inline physx::PxTransform poseFromWorld(const glm::mat4& worldTransform,
                                            glm::vec3* outScale = nullptr)
    {
        // La descomposicion segura vive en Core/TransformDecompose.h: el mismo
        // fallo estaba en el inspector y en los bindings de Lua, asi que el
        // criterio tiene que ser UNO. Aqui solo se traduce a PhysX.
        glm::vec3 translation{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        decomposeTransform(worldTransform, &translation, &rotation, outScale);

        const physx::PxVec3 p(translation.x, translation.y, translation.z);
        const physx::PxQuat q(rotation.x, rotation.y, rotation.z, rotation.w);

        // El cinturón, por si entra un infinito por otro camino (una matriz con
        // un NaN dentro la envenena igual). `isSane` e `isFinite` son de PhysX A
        // PROPÓSITO: el criterio con el que se acepta la pose tiene que ser
        // EXACTAMENTE el que usa quien la va a rechazar, no una reimplementación
        // con std::isfinite —que además dejaría pasar un cuaternión finito pero
        // sin normalizar, que PhysX rechaza igual—.
        return physx::PxTransform(p.isFinite() ? p : physx::PxVec3(0.0f),
                                  q.isSane()   ? q : physx::PxQuat(physx::PxIdentity));
    }
}
