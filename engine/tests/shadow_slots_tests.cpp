// Test headless del reparto de ranuras de sombra entre las luces secundarias
// (H74 v2). Sin GPU: `repartirSombrasExtra` es una plantilla pura sobre la lista
// de luces, y decide DOS cosas a la vez —en que capa graba el renderer y con que
// matriz muestrea el shader—, asi que un fallo aqui no es "una sombra mal": es
// una luz muestreando el mapa de otra.
//
// Por eso el reparto vive en un solo sitio compartido por los dos backends. Este
// fichero es lo que garantiza que ese sitio hace lo que dice.
#include "DonTopo/Renderer/UniformBufferObject.h"

#include <cstdio>
#include <set>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Luz minima con lo unico que mira el reparto: su tipo y su cono.
// OJO con el cono por defecto: el de `Light` en el motor es cos 0.7, que son
// unos 46 grados de semiangulo — con el doble y el margen del 15 % del shadow
// map se va a 105 grados, o sea que YA necesita cubemap. Aqui se usa 0.9 (59
// grados de FOV) para tener un foco que de verdad cabe en una cara.
struct Luz {
    LightType tipo  = LightType::Spot;
    float     cosInterior = 0.95f;
    float     cosExterior = 0.9f;
};

static int tipoDe(const Luz& l) { return static_cast<int>(l.tipo); }
static glm::vec4 paramsDe(const Luz& l) { return glm::vec4(10.0f, l.cosInterior, l.cosExterior, 1.0f); }

static std::vector<int> repartir(const std::vector<Luz>& luces, int* usadas = nullptr)
{
    std::vector<int> ranuras(luces.size(), -99);
    const int n = repartirSombrasExtra(luces.data(), (int)luces.size(), tipoDe, paramsDe,
                                        ranuras.data());
    if (usadas) *usadas = n;
    return ranuras;
}

// Un foco secundario cabe en UNA capa: es el unico tipo que no necesita mas.
static void test_un_foco_ocupa_una_ranura()
{
    std::vector<Luz> luces(2);
    luces[0].tipo = LightType::Directional;   // la key, que nunca entra aqui
    int usadas = 0;
    const std::vector<int> r = repartir(luces, &usadas);
    CHECK(r[0] == -1);                      // la key la reparte computeCascades
    CHECK(r[1] == SHADOW_KEY_MATRICES);     // primera ranura libre
    CHECK(usadas == 1);
}

// LO NUEVO de v2: una luz de PUNTO secundaria proyecta, y se lleva las seis
// caras de su cubemap en ranuras CONSECUTIVAS. Antes se descartaba entera.
static void test_una_punto_ocupa_seis_ranuras_seguidas()
{
    std::vector<Luz> luces(2);
    luces[0].tipo = LightType::Directional;
    luces[1].tipo = LightType::Point;
    int usadas = 0;
    const std::vector<int> r = repartir(luces, &usadas);
    CHECK(r[1] == SHADOW_KEY_MATRICES);
    CHECK(usadas == 6);
}

// Mezcla: cada tipo consume lo suyo y nadie pisa a nadie. Esta es la propiedad
// que de verdad importa — dos luces con la misma ranura muestrean el mapa de la
// otra, y eso no lo avisa ninguna capa de validacion.
static void test_mezcla_sin_solapamiento()
{
    std::vector<Luz> luces(5);
    luces[0].tipo = LightType::Directional;
    luces[1].tipo = LightType::Spot;
    luces[2].tipo = LightType::Point;
    luces[3].tipo = LightType::Spot;
    luces[4].tipo = LightType::Point;

    int usadas = 0;
    const std::vector<int> r = repartir(luces, &usadas);

    std::set<int> ocupadas;
    for (size_t i = 1; i < luces.size(); ++i) {
        if (r[i] < 0) continue;
        const int cuantas = (luces[i].tipo == LightType::Point) ? 6 : 1;
        for (int c = 0; c < cuantas; ++c)
            CHECK(ocupadas.insert(r[i] + c).second);   // false = ya ocupada
    }
    for (int ranura : ocupadas)
        CHECK(ranura >= SHADOW_KEY_MATRICES && ranura < SHADOW_MATRICES);
    CHECK((int)ocupadas.size() == usadas);
}

// Una punto que NO cabe entera no puede reservar a medias: dejaria grabadas
// tres caras de seis y el shader muestrearia capas de otra luz al elegir una de
// las que faltan. O entra completa, o no entra.
static void test_una_punto_que_no_cabe_no_reserva_nada()
{
    // Focos hasta dejar menos de 6 libres, y luego una punto.
    const int libres = SHADOW_MATRICES - SHADOW_KEY_MATRICES;
    std::vector<Luz> luces(1);
    luces[0].tipo = LightType::Directional;
    for (int i = 0; i < libres - 2; ++i) luces.push_back(Luz{});   // focos
    Luz punto; punto.tipo = LightType::Point;
    luces.push_back(punto);

    int usadas = 0;
    const std::vector<int> r = repartir(luces, &usadas);
    CHECK(r.back() == -1);              // la punto se queda sin sombra
    CHECK(usadas == libres - 2);        // y no ha consumido ranuras a medias
}

// Pasado el tope se dejan de repartir, pero las luces siguen iluminando: no
// proyectar es una degradacion, no un error.
static void test_pasado_el_tope_no_se_reparte_mas()
{
    const int libres = SHADOW_MATRICES - SHADOW_KEY_MATRICES;
    std::vector<Luz> luces(1);
    luces[0].tipo = LightType::Directional;
    for (int i = 0; i < libres + 5; ++i) luces.push_back(Luz{});

    int usadas = 0;
    const std::vector<int> r = repartir(luces, &usadas);
    CHECK(usadas == libres);
    CHECK(r.back() == -1);
}

// Un foco tan abierto que necesita cubemap: con v2 ya no se descarta, entra por
// el camino de las seis caras igual que una punto. Es el mismo criterio que usa
// la luz key, y tenerlo en un solo sitio es lo que evito que divergiera (H65).
static void test_un_foco_muy_abierto_usa_cubemap()
{
    std::vector<Luz> luces(2);
    luces[0].tipo = LightType::Directional;
    luces[1].tipo = LightType::Spot;
    luces[1].cosExterior = -0.5f;   // ~120 grados de cono
    CHECK(spotNecesitaCubemap(paramsDe(luces[1])));

    int usadas = 0;
    const std::vector<int> r = repartir(luces, &usadas);
    CHECK(r[1] == SHADOW_KEY_MATRICES);
    CHECK(usadas == 6);
}

// La luz key nunca entra en este reparto: sus matrices las pone computeCascades
// en los SHADOW_KEY_MATRICES primeros huecos.
static void test_la_key_nunca_recibe_ranura()
{
    std::vector<Luz> luces(3);
    luces[0].tipo = LightType::Point;   // key de punto, la que mas ranuras usa
    const std::vector<int> r = repartir(luces);
    CHECK(r[0] == -1);
}

// Una direccional secundaria no proyecta: necesitaria sus propias cascadas para
// no verse peor que sin sombra. Sigue iluminando.
static void test_una_direccional_secundaria_no_proyecta()
{
    std::vector<Luz> luces(2);
    luces[0].tipo = LightType::Point;
    luces[1].tipo = LightType::Directional;
    int usadas = 0;
    const std::vector<int> r = repartir(luces, &usadas);
    CHECK(r[1] == -1);
    CHECK(usadas == 0);
}

int main()
{
    test_un_foco_ocupa_una_ranura();
    test_una_punto_ocupa_seis_ranuras_seguidas();
    test_mezcla_sin_solapamiento();
    test_una_punto_que_no_cabe_no_reserva_nada();
    test_pasado_el_tope_no_se_reparte_mas();
    test_un_foco_muy_abierto_usa_cubemap();
    test_la_key_nunca_recibe_ranura();
    test_una_direccional_secundaria_no_proyecta();

    if (g_failures == 0) std::printf("ALL SHADOW SLOT TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
