// Headless test of the shadow slot distribution among the secondary lights
// (H74 v2). No GPU: `repartirSombrasExtra` is a pure template over the list
// of lights, and it decides TWO things at once (in which layer the renderer records and with which
// matrix the shader samples), so a failure here is not "a wrong shadow": it is
// one light sampling another light's map.
//
// That is why the distribution lives in a single place shared by the two backends. This
// file is what guarantees that that place does what it says.
#include "DonTopo/Renderer/UniformBufferObject.h"

#include <cstdio>
#include <set>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

// Minimal light with the only things the distribution looks at: its type and its cone.
// CAREFUL with the default cone: the engine's `Light` one is cos 0.7, which is
// about 46 degrees of half-angle; doubled and with the 15% margin of the shadow
// map it goes to 105 degrees, that is, it ALREADY needs a cubemap. Here 0.9 is used (59
// degrees of FOV) to have a spot that really fits in one face.
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

// A secondary spot fits in ONE layer: it is the only type that needs no more.
static void test_un_foco_ocupa_una_ranura()
{
    std::vector<Luz> luces(2);
    luces[0].tipo = LightType::Directional;   // the key, which never comes in here
    int usadas = 0;
    const std::vector<int> r = repartir(luces, &usadas);
    CHECK(r[0] == -1);                      // the key is distributed by computeCascades
    CHECK(r[1] == SHADOW_KEY_MATRICES);     // first free slot
    CHECK(usadas == 1);
}

// NEW in v2: a secondary POINT light casts, and takes the six faces of its
// cubemap in CONSECUTIVE slots. Before, it was discarded entirely.
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

// Mix: each type consumes its own and nobody steps on anybody. This is the property
// that really matters: two lights with the same slot sample each other's map,
// and no validation layer reports that.
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
            CHECK(ocupadas.insert(r[i] + c).second);   // false = already occupied
    }
    for (int ranura : ocupadas)
        CHECK(ranura >= SHADOW_KEY_MATRICES && ranura < SHADOW_MATRICES);
    CHECK((int)ocupadas.size() == usadas);
}

// A point light that does NOT fit whole cannot reserve halfway: it would leave
// three faces of six recorded and the shader would sample layers of another light when choosing one of
// the missing ones. It either fits complete or it does not fit.
static void test_una_punto_que_no_cabe_no_reserva_nada()
{
    // Spots until fewer than 6 are free, and then a point light.
    const int libres = SHADOW_MATRICES - SHADOW_KEY_MATRICES;
    std::vector<Luz> luces(1);
    luces[0].tipo = LightType::Directional;
    for (int i = 0; i < libres - 2; ++i) luces.push_back(Luz{});   // spots
    Luz punto; punto.tipo = LightType::Point;
    luces.push_back(punto);

    int usadas = 0;
    const std::vector<int> r = repartir(luces, &usadas);
    CHECK(r.back() == -1);              // the point light is left without a shadow
    CHECK(usadas == libres - 2);        // and has not consumed slots halfway
}

// Past the cap they are no longer distributed, but the lights keep lighting: not
// casting is a degradation, not an error.
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

// A spot so wide that it needs a cubemap: with v2 it is no longer discarded, it goes through
// the six-face path just like a point light. It is the same criterion the
// key light uses, and having it in a single place is what prevented it from diverging (H65).
static void test_un_foco_muy_abierto_usa_cubemap()
{
    std::vector<Luz> luces(2);
    luces[0].tipo = LightType::Directional;
    luces[1].tipo = LightType::Spot;
    luces[1].cosExterior = -0.5f;   // ~120 degrees of cone
    CHECK(spotNecesitaCubemap(paramsDe(luces[1])));

    int usadas = 0;
    const std::vector<int> r = repartir(luces, &usadas);
    CHECK(r[1] == SHADOW_KEY_MATRICES);
    CHECK(usadas == 6);
}

// The key light never enters this distribution: its matrices are set by computeCascades
// in the first SHADOW_KEY_MATRICES slots.
static void test_la_key_nunca_recibe_ranura()
{
    std::vector<Luz> luces(3);
    luces[0].tipo = LightType::Point;   // point key, the one that uses the most slots
    const std::vector<int> r = repartir(luces);
    CHECK(r[0] == -1);
}

// A secondary directional does not cast: it would need its own cascades so as
// not to look worse than with no shadow. It still lights.
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
