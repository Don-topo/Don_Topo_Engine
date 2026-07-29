// Test headless del frustum culling (sin GUI, sin Vulkan). Renderer::
// frustumFromViewProj y Renderer::aabbVisible son estáticas justo para poder
// ejercitarlas sin un Renderer inicializado. Plain main + asserts, sin
// framework — coherente con camera_tests.cpp.
//
// Lo que de verdad se prueba aquí no es "¿culea?" sino "¿culea de MENOS?": un
// falso negativo es un objeto que desaparece de pantalla, así que cada caso
// dentro del frustum se afirma visible, y los casos de fuera se ponen bien
// lejos para que ninguna holgura conservadora los salve por accidente.
#include "DonTopo/Renderer/Renderer.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstdio>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Cámara en el origen mirando hacia -Z, que es el convenio de glm::lookAt y el
// que usa el motor. Devuelve proj*view listo para frustumFromViewProj.
static glm::mat4 makeViewProj(bool zeroToOne)
{
    const glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 0.0f),
                                       glm::vec3(0.0f, 0.0f, -1.0f),
                                       glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = zeroToOne
        ? glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 1.0f, 1000.0f)
        : glm::perspective(glm::radians(45.0f), 1.0f, 1.0f, 1000.0f);
    proj[1][1] *= -1.0f; // Y-flip de Vulkan, igual que currentFrameCamera()
    return proj * view;
}

// Cubo unidad centrado en el origen local: todo lo posiciona el model matrix,
// como los RenderObject reales.
static const glm::vec3 kMin(-10.0f, -10.0f, -10.0f);
static const glm::vec3 kMax( 10.0f,  10.0f,  10.0f);

static glm::mat4 at(float x, float y, float z)
{
    return glm::translate(glm::mat4(1.0f), glm::vec3(x, y, z));
}

// Delante de la cámara = visible; detrás, a los lados y más allá del far = no.
// Se prueba con los DOS rangos de profundidad porque el motor mezcla los dos
// (glm::perspective en el editor, *RH_ZO en CameraComponent y en la luz).
static void test_dentro_y_fuera(bool zeroToOne)
{
    const Renderer::Frustum f = Renderer::frustumFromViewProj(makeViewProj(zeroToOne));

    // Justo delante, a media distancia: el caso trivial que NO puede fallar.
    CHECK(Renderer::aabbVisible(f, kMin, kMax, at(0.0f, 0.0f, -100.0f)));
    // Cerca de la cámara pero delante. Es el caso que rompería si el plano
    // cercano se extrajera con el convenio equivocado sobre esta matriz.
    //
    // Tiene que ser una caja PEQUEÑA y muy cerca: con el cubo de semilado 10 de
    // los demás casos, una esquina asoma siempre lo bastante lejos como para
    // que el test conservador la salve, y el caso no discriminaría nada
    // (comprobado saboteando el plano a mano: pasaba igual). Con semilado 0.2 a
    // 1.5 de la cámara, extraer el cercano como "fila 2 a secas" sobre la
    // matriz [-1,1] sí lo descarta.
    const glm::vec3 chicoMin(-0.2f, -0.2f, -0.2f);
    const glm::vec3 chicoMax( 0.2f,  0.2f,  0.2f);
    CHECK(Renderer::aabbVisible(f, chicoMin, chicoMax, at(0.0f, 0.0f, -1.5f)));
    // Casi rozando el far, todavía dentro.
    CHECK(Renderer::aabbVisible(f, kMin, kMax, at(0.0f, 0.0f, -900.0f)));

    // Detrás de la cámara.
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(0.0f, 0.0f, 500.0f)));
    // Fuera por la derecha y por la izquierda (a 100 de profundidad el semiancho
    // del frustum de 45° es ~41, así que 400 está holgadamente fuera).
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(400.0f, 0.0f, -100.0f)));
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(-400.0f, 0.0f, -100.0f)));
    // Fuera por arriba y por abajo.
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(0.0f, 400.0f, -100.0f)));
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(0.0f, -400.0f, -100.0f)));
    // Más allá del plano lejano.
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(0.0f, 0.0f, -5000.0f)));
}

// Un objeto que asoma por el borde tiene que seguir dibujándose: el test es
// conservador, y el error que importa es el falso negativo.
static void test_borde_cuenta_como_visible()
{
    const Renderer::Frustum f = Renderer::frustumFromViewProj(makeViewProj(true));

    // A 100 de profundidad el semiancho es ~41.4. Un cubo de semilado 10
    // centrado en x=48 tiene su centro FUERA y su esquina DENTRO.
    CHECK(Renderer::aabbVisible(f, kMin, kMax, at(48.0f, 0.0f, -100.0f)));
    // Mismo caso por arriba.
    CHECK(Renderer::aabbVisible(f, kMin, kMax, at(0.0f, 48.0f, -100.0f)));
}

// La AABB va en espacio LOCAL: el test tiene que aplicar el model matrix
// entero, rotación y escala incluidas. Sin el valor absoluto de la 3x3 en
// aabbVisible, una caja rotada se acotaría de menos y desaparecería por el
// borde.
static void test_rotacion_y_escala()
{
    const Renderer::Frustum f = Renderer::frustumFromViewProj(makeViewProj(true));

    // Rotada sobre Y: la caja alineada a ejes que la envuelve crece un factor
    // ~1.41, así que asoma por el borde aunque sin rotar no llegara.
    //
    // 135° y no 45° a propósito: con 45° las tres contribuciones a extent.x son
    // positivas y sumar con o sin valor absoluto da lo mismo, así que el caso no
    // probaría nada (comprobado quitando el abs a mano: pasaba igual). A 135°
    // los términos son +0.707 y -0.707, y sin el abs se cancelan dejando la
    // caja con anchura cero.
    glm::mat4 rotada = glm::translate(glm::mat4(1.0f), glm::vec3(53.0f, 0.0f, -100.0f));
    rotada = glm::rotate(rotada, glm::radians(135.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(Renderer::aabbVisible(f, kMin, kMax, rotada));

    // Escalada x10 desde una posición donde el cubo sin escalar quedaría fuera:
    // el objeto grande sí entra en cámara.
    glm::mat4 grande = glm::translate(glm::mat4(1.0f), glm::vec3(120.0f, 0.0f, -100.0f));
    grande = glm::scale(grande, glm::vec3(10.0f));
    CHECK(Renderer::aabbVisible(f, kMin, kMax, grande));
    // Y el mismo sitio SIN escalar queda fuera — sin esta pareja, el CHECK de
    // arriba pasaría igual aunque aabbVisible ignorara la escala.
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(120.0f, 0.0f, -100.0f)));
}

// El pass de sombras culea contra la matriz de la luz, que es ortográfica y
// acotada a ±350 alrededor del origen (ver shadowLightSpaceMatrix). Lo que
// queda fuera de ese volumen no cabe en el shadow map.
static void test_frustum_ortografico_de_la_luz()
{
    glm::mat4 lightView = glm::lookAt(glm::vec3(0.0f, 500.0f, 300.0f),
                                      glm::vec3(0.0f),
                                      glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 lightProj = glm::orthoRH_ZO(-350.0f, 350.0f, -350.0f, 350.0f, 1.0f, 2000.0f);
    lightProj[1][1] *= -1.0f;
    const Renderer::Frustum f = Renderer::frustumFromViewProj(lightProj * lightView);

    // En el centro del volumen: proyecta sombra.
    CHECK(Renderer::aabbVisible(f, kMin, kMax, at(0.0f, 0.0f, 0.0f)));
    CHECK(Renderer::aabbVisible(f, kMin, kMax, at(300.0f, 0.0f, 0.0f)));
    // Muy lejos en X: fuera de los ±350 de la ortográfica.
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(2000.0f, 0.0f, 0.0f)));
    CHECK(!Renderer::aabbVisible(f, kMin, kMax, at(-2000.0f, 0.0f, 0.0f)));
}

// Los planos salen normalizados, así que dot(n,c)+d es una distancia real. Si
// no lo estuvieran, el radio proyectado de la AABB no sería comparable con esa
// distancia y el margen del test quedaría escalado por un factor arbitrario.
static void test_planos_normalizados()
{
    const Renderer::Frustum f = Renderer::frustumFromViewProj(makeViewProj(true));
    for (const glm::vec4& p : f.planes)
    {
        const float len = glm::length(glm::vec3(p));
        CHECK(std::fabs(len - 1.0f) < 0.001f);
    }
}

int main()
{
    test_dentro_y_fuera(/*zeroToOne=*/true);
    test_dentro_y_fuera(/*zeroToOne=*/false);
    test_borde_cuenta_como_visible();
    test_rotacion_y_escala();
    test_frustum_ortografico_de_la_luz();
    test_planos_normalizados();

    if (g_failures == 0) std::printf("frustum_tests: OK\n");
    else                 std::printf("frustum_tests: %d FALLOS\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
