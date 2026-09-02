// Test headless de la descomposicion de matrices (sin GUI, sin PhysX).
// decomposeTransform es glm puro, asi que la regla que gobierna los cinco
// sitios que descomponen un transform —fisica, inspector y los dos bindings de
// Lua— se puede afirmar aqui entera. Plain main + asserts, coherente con
// frustum_tests.cpp.
//
// Lo que se protege es lo que costo un crash mudo del editor, una congelacion
// al entrar en Play y objetos saltando a 1e8: glm::decompose devuelve bool y
// ante una matriz singular devuelve false SIN ESCRIBIR sus salidas.
#include "DonTopo/Core/TransformDecompose.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdio>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// El caso que rompia: un eje a escala 0, que es lo que sale de escribir un 0 en
// Scale.Y del inspector.
static void test_matriz_singular()
{
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 20.0f, -7.0f)) *
                        glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 5.0f));

    glm::vec3 pos{-1.0f}, scale{-1.0f};
    glm::quat rot{0.0f, 9.0f, 9.0f, 9.0f};
    const bool ok = decomposeTransform(m, &pos, &rot, &scale);

    // Dice que no pudo, que es la informacion que nadie miraba.
    CHECK(!ok);

    // Y aun asi las salidas son utiles. La POSICION entera: es la cuarta
    // columna y no depende de la descomposicion.
    CHECK(std::fabs(pos.x - 3.0f) < 1e-5f);
    CHECK(std::fabs(pos.y - 20.0f) < 1e-5f);
    CHECK(std::fabs(pos.z + 7.0f) < 1e-5f);

    // La ESCALA de verdad, incluido el cero: es la longitud de cada columna.
    CHECK(std::fabs(scale.x - 2.0f) < 1e-5f);
    CHECK(std::fabs(scale.y) < 1e-5f);
    CHECK(std::fabs(scale.z - 5.0f) < 1e-5f);

    // La ROTACION a identidad, que es lo unico honesto: un eje aplastado no
    // define ninguna orientacion. Sin esto salia el cuaternion sin inicializar,
    // y eulerAngles lo convertia en los 90 grados que se veian en el inspector.
    CHECK(std::fabs(rot.w - 1.0f) < 1e-5f);
    CHECK(std::fabs(rot.x) < 1e-5f);
    CHECK(std::fabs(rot.y) < 1e-5f);
    CHECK(std::fabs(rot.z) < 1e-5f);
}

// Con una matriz normal tiene que dar exactamente lo de siempre: esto sustituye
// a glm::decompose en cinco sitios, y cambiar el caso bueno seria peor que el
// bug que arregla.
static void test_matriz_normal()
{
    const glm::vec3 posEsperada(1.5f, -2.0f, 3.25f);
    const glm::vec3 escalaEsperada(2.0f, 3.0f, 4.0f);
    // 90 grados sobre Y: un angulo que no se confunde con la identidad ni es
    // simetrico en los tres ejes.
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), posEsperada) *
                        glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
                        glm::scale(glm::mat4(1.0f), escalaEsperada);

    glm::vec3 pos, scale;
    glm::quat rot;
    CHECK(decomposeTransform(m, &pos, &rot, &scale));

    CHECK(glm::length(pos - posEsperada) < 1e-4f);
    CHECK(glm::length(scale - escalaEsperada) < 1e-4f);

    // La rotacion se comprueba por lo que HACE, no por sus componentes: girar
    // 90 grados sobre Y lleva el eje X a -Z.
    const glm::vec3 giradoX = rot * glm::vec3(1.0f, 0.0f, 0.0f);
    CHECK(std::fabs(giradoX.x) < 1e-4f);
    CHECK(std::fabs(giradoX.z + 1.0f) < 1e-4f);
}

// Los tres punteros son opcionales, y quien solo quiera uno no debe pagar los
// otros ni petar.
static void test_salidas_opcionales()
{
    const glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, 5.0f, 6.0f));

    CHECK(decomposeTransform(m));   // sin ninguna salida

    glm::vec3 soloPos;
    CHECK(decomposeTransform(m, &soloPos));
    CHECK(glm::length(soloPos - glm::vec3(4.0f, 5.0f, 6.0f)) < 1e-5f);

    glm::vec3 soloEscala;
    CHECK(decomposeTransform(m, nullptr, nullptr, &soloEscala));
    CHECK(glm::length(soloEscala - glm::vec3(1.0f)) < 1e-5f);
}

// Una escala NEGATIVA es un espejo. La longitud de columna es siempre positiva,
// asi que el signo se pierde: se afirma para que conste, porque los colliders ya
// tomaban el valor absoluto (un espejo no adelgaza la caja) y el inspector lo
// mira con su propio cache.
static void test_escala_negativa_sale_en_magnitud()
{
    const glm::mat4 m = glm::scale(glm::mat4(1.0f), glm::vec3(-2.0f, 3.0f, 1.0f));

    glm::vec3 scale;
    decomposeTransform(m, nullptr, nullptr, &scale);
    CHECK(std::fabs(scale.x - 2.0f) < 1e-5f);
    CHECK(std::fabs(scale.y - 3.0f) < 1e-5f);
}

int main()
{
    test_matriz_singular();
    test_matriz_normal();
    test_salidas_opcionales();
    test_escala_negativa_sale_en_magnitud();

    if (g_failures == 0) std::printf("transform_decompose_tests: OK\n");
    else                 std::printf("transform_decompose_tests: %d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
