// Test headless del batcher de la UI 2D (sin GUI, sin Vulkan).
// UiSpriteBatch::build es estática y CPU pura justo para poder ejercitarla sin
// un Renderer inicializado, igual que buildInstanceBatches. Plain main +
// asserts, sin framework — coherente con instancing_tests.cpp.
//
// Lo que se prueba son las cuatro cosas que fallan EN SILENCIO en un renderer
// de UI: que el origen esté arriba a la izquierda con +Y hacia abajo (un signo
// mal puesto pinta la UI boca abajo sin un solo error de validación), que la
// transformada del padre se acumule en el hijo, que el lote rompa exactamente
// donde debe (romper de más solo cuesta draws; romper de menos pinta con la
// textura equivocada), y que el scissor del hijo se INTERSEQUE con el del
// padre en vez de reemplazarlo.
//
// Todos los valores son no neutros y distintos entre sí: con 0, 1 o valores
// repetidos, un campo que nadie lee pasaría igual.
#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiSpriteBatch.h"
#include "DonTopo/UI/UiTextureAtlas.h"

#include <cmath>
#include <cstdio>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearly(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// Tamaño de render deliberadamente asimétrico: con 800x600 un ancho y un alto
// intercambiados podrían colarse.
static constexpr uint32_t kW = 800;
static constexpr uint32_t kH = 480;

// Un canvas sin nodos visibles no puede generar ni un lote: es la condición que
// hace que la escena 3D salga exactamente igual que antes de esta feature.
static void test_canvas_vacio_no_emite_nada()
{
    UiCanvas canvas;
    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.empty());
    CHECK(data.vertices.empty());
    CHECK(data.indices.empty());
}

// (0,0) es la esquina SUPERIOR izquierda y +Y va hacia abajo: el vértice de
// abajo tiene la Y MAYOR, no menor.
static void test_origen_arriba_izquierda()
{
    UiCanvas canvas;
    UiNode& panel = canvas.root().addChild("Panel");
    panel.position = {0.0f, 0.0f};
    panel.size     = {37.0f, 53.0f};   // ancho != alto: distingue X de Y

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 4);
    CHECK(data.indices.size() == 6);
    if (data.vertices.size() != 4) return;

    // Esquina superior izquierda exactamente en el origen.
    CHECK(nearly(data.vertices[0].pos.x, 0.0f));
    CHECK(nearly(data.vertices[0].pos.y, 0.0f));
    // Superior derecha: solo se mueve en X.
    CHECK(nearly(data.vertices[1].pos.x, 37.0f));
    CHECK(nearly(data.vertices[1].pos.y, 0.0f));
    // Inferior derecha e inferior izquierda: Y = alto, y MAYOR que la de arriba.
    CHECK(nearly(data.vertices[2].pos.y, 53.0f));
    CHECK(nearly(data.vertices[3].pos.y, 53.0f));
    CHECK(data.vertices[2].pos.y > data.vertices[1].pos.y);
    CHECK(data.vertices[3].pos.y > data.vertices[0].pos.y);
    // Y ninguna coordenada negativa: un origen abajo-izquierda sacaría el quad
    // fuera de la pantalla por arriba.
    CHECK(data.vertices[2].pos.y > 0.0f);
}

// La posición del hijo es local y la escala del padre la multiplica: posición y
// tamaño del hijo salen del padre acumulado, no de sus propios campos a secas.
static void test_transform_del_padre_se_acumula()
{
    UiCanvas canvas;
    UiNode& parent = canvas.root().addChild("Panel");
    parent.position = {120.0f, 45.0f};
    parent.scale    = {2.0f, 3.0f};    // escalas distintas por eje
    parent.drawable = false;           // solo agrupa: el único quad es el hijo

    UiNode& child = parent.addChild("Image");
    child.position = {10.0f, 20.0f};
    child.size     = {5.0f, 7.0f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;

    // 120 + 10*2 = 140 ; 45 + 20*3 = 105. Sin acumular saldría (10,20), y sin
    // aplicar la escala del padre, (130,65).
    CHECK(nearly(data.vertices[0].pos.x, 140.0f));
    CHECK(nearly(data.vertices[0].pos.y, 105.0f));
    // Tamaño escalado por el padre: 5*2 = 10 ; 7*3 = 21.
    CHECK(nearly(data.vertices[2].pos.x, 150.0f));
    CHECK(nearly(data.vertices[2].pos.y, 126.0f));
}

// Mismo atlas y mismo scissor = UN draw. Es el caso que justifica el batcher.
static void test_mismo_atlas_y_scissor_un_solo_lote()
{
    UiTextureAtlas atlas;
    atlas.setSize(200, 100);
    atlas.addSprite("botella", {50.0f, 10.0f, 25.0f, 40.0f});

    UiCanvas canvas;
    for (int i = 0; i < 2; ++i)
    {
        UiNode& node = canvas.root().addChild("Image");
        node.position = {17.0f + 60.0f * (float)i, 23.0f};
        node.size     = {25.0f, 40.0f};
        node.atlas    = &atlas;
        node.sprite   = "botella";
    }

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 8);
    if (data.batches.size() != 1) return;
    // Los dos quads dentro del MISMO draw, no uno de dos.
    CHECK(data.batches[0].firstIndex == 0);
    CHECK(data.batches[0].indexCount == 12);
}

// Cambiar de atlas parte el lote: un draw no puede llevar dos texturas.
static void test_cambiar_de_atlas_parte_el_lote()
{
    UiTextureAtlas hud;
    hud.setSize(200, 100);
    hud.addSprite("botella", {50.0f, 10.0f, 25.0f, 40.0f});

    UiTextureAtlas iconos;
    iconos.setSize(64, 32);
    iconos.addSprite("llave", {8.0f, 4.0f, 16.0f, 8.0f});

    UiCanvas canvas;
    UiNode& a = canvas.root().addChild("Image");
    a.position = {17.0f, 23.0f};
    a.size     = {25.0f, 40.0f};
    a.atlas    = &hud;
    a.sprite   = "botella";

    UiNode& b = canvas.root().addChild("Image");
    b.position = {90.0f, 23.0f};
    b.size     = {16.0f, 8.0f};
    b.atlas    = &iconos;
    b.sprite   = "llave";

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 2);
    if (data.batches.size() != 2) return;
    // Cada lote apunta a SU atlas y a sus propios índices: un firstIndex mal
    // puesto pintaría el quad del otro.
    CHECK(data.batches[0].atlas == &hud);
    CHECK(data.batches[1].atlas == &iconos);
    CHECK(data.batches[0].firstIndex == 0);
    CHECK(data.batches[0].indexCount == 6);
    CHECK(data.batches[1].firstIndex == 6);
    CHECK(data.batches[1].indexCount == 6);
}

// Mismo atlas pero scissor distinto: también parte, porque el scissor es estado
// del command buffer y no viaja por vértice.
static void test_cambiar_de_scissor_parte_el_lote()
{
    UiTextureAtlas atlas;
    atlas.setSize(200, 100);
    atlas.addSprite("botella", {50.0f, 10.0f, 25.0f, 40.0f});

    UiCanvas canvas;

    UiNode& a = canvas.root().addChild("PanelIzquierdo");
    a.position     = {30.0f, 40.0f};
    a.size         = {120.0f, 70.0f};
    a.atlas        = &atlas;
    a.sprite       = "botella";
    a.clipChildren = true;

    UiNode& b = canvas.root().addChild("PanelDerecho");
    b.position     = {300.0f, 210.0f};   // rect claramente distinto
    b.size         = {90.0f, 55.0f};
    b.atlas        = &atlas;
    b.sprite       = "botella";
    b.clipChildren = true;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 2);
    if (data.batches.size() != 2) return;
    CHECK(data.batches[0].atlas == data.batches[1].atlas);   // el atlas NO cambió
    CHECK(data.batches[0].scissor != data.batches[1].scissor);
    CHECK(data.batches[0].scissor.x == 30 && data.batches[0].scissor.y == 40);
    CHECK(data.batches[0].scissor.width == 120 && data.batches[0].scissor.height == 70);
    CHECK(data.batches[1].scissor.x == 300 && data.batches[1].scissor.y == 210);
    CHECK(data.batches[1].scissor.width == 90 && data.batches[1].scissor.height == 55);
}

// El scissor del hijo se INTERSECA con el del padre. Con reemplazo el hijo
// pintaría fuera del panel que lo contiene, que es el bug clásico del scroll.
static void test_scissor_del_hijo_se_interseca_con_el_del_padre()
{
    UiCanvas canvas;

    UiNode& parent = canvas.root().addChild("Panel");
    parent.position     = {100.0f, 50.0f};
    parent.size         = {200.0f, 80.0f};   // rect padre: x[100,300) y[50,130)
    parent.drawable     = false;
    parent.clipChildren = true;

    // Hijo más ancho que el padre y desplazado a la izquierda: si el scissor se
    // reemplazase, saldría (50,60,300,40) y se vería fuera del panel.
    UiNode& child = parent.addChild("Contenido");
    child.position     = {-50.0f, 10.0f};    // mundo: x[50,350) y[60,100)
    child.size         = {300.0f, 40.0f};
    child.clipChildren = true;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    if (data.batches.size() != 1) return;

    const UiScissor& s = data.batches[0].scissor;
    CHECK(s.x == 100);          // recortado por la izquierda del padre
    CHECK(s.y == 60);           // el del hijo, que empieza más abajo
    CHECK(s.width == 200);      // hasta el borde derecho del padre (300)
    CHECK(s.height == 40);      // el alto del hijo, menor que el del padre
}

// Intersección vacía: ni un draw. Grabar un scissor de width/height 0 sería un
// comando inútil (y una trampa fácil de dejar pasar).
static void test_interseccion_vacia_no_emite_draw()
{
    UiCanvas canvas;

    UiNode& parent = canvas.root().addChild("Panel");
    parent.position     = {100.0f, 50.0f};
    parent.size         = {200.0f, 80.0f};   // x[100,300)
    parent.drawable     = false;
    parent.clipChildren = true;

    UiNode& child = parent.addChild("Fuera");
    child.position     = {400.0f, 10.0f};    // mundo x[500,560): sin solape
    child.size         = {60.0f, 30.0f};
    child.clipChildren = true;

    // Un nieto visible: si el corte no propagase, este se colaría.
    UiNode& grandchild = child.addChild("Nieto");
    grandchild.position = {5.0f, 5.0f};
    grandchild.size     = {20.0f, 10.0f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.empty());
    CHECK(data.vertices.empty());
    CHECK(data.indices.empty());
}

// Sub-rect que NO empieza en el origen del atlas y con proporciones distintas en
// cada eje: unas UVs invertidas, transpuestas o normalizadas por el eje
// equivocado darían números diferentes en las cuatro comprobaciones.
static void test_uvs_del_subrect_del_atlas()
{
    UiTextureAtlas atlas;
    atlas.setSize(200, 100);
    // u: 50/200 = 0.25 -> 75/200 = 0.375 ; v: 10/100 = 0.1 -> 50/100 = 0.5
    atlas.addSprite("botella", {50.0f, 10.0f, 25.0f, 40.0f});

    UiCanvas canvas;
    UiNode& node = canvas.root().addChild("Image");
    node.position = {17.0f, 23.0f};
    node.size     = {25.0f, 40.0f};
    node.atlas    = &atlas;
    node.sprite   = "botella";

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;

    // Cada esquina con SU par de UVs: el orden importa tanto como los valores.
    CHECK(nearly(data.vertices[0].uv.x, 0.25f));   // sup-izq
    CHECK(nearly(data.vertices[0].uv.y, 0.10f));
    CHECK(nearly(data.vertices[1].uv.x, 0.375f));  // sup-der
    CHECK(nearly(data.vertices[1].uv.y, 0.10f));
    CHECK(nearly(data.vertices[2].uv.x, 0.375f));  // inf-der
    CHECK(nearly(data.vertices[2].uv.y, 0.50f));
    CHECK(nearly(data.vertices[3].uv.x, 0.25f));   // inf-izq
    CHECK(nearly(data.vertices[3].uv.y, 0.50f));

    // V crece hacia abajo, igual que la pantalla.
    CHECK(data.vertices[2].uv.y > data.vertices[1].uv.y);

    // Y el API del atlas dice lo mismo por su cuenta.
    const UiUvRect uv = atlas.uvRect("botella");
    CHECK(nearly(uv.u0, 0.25f) && nearly(uv.v0, 0.10f));
    CHECK(nearly(uv.u1, 0.375f) && nearly(uv.v1, 0.50f));

    // Un sprite que no existe cae al rect completo, no a basura.
    const UiUvRect missing = atlas.uvRect("no_existe");
    CHECK(nearly(missing.u0, 0.0f) && nearly(missing.v1, 1.0f));
}

// Lo invisible no gasta ni vértices ni lote, ni arrastra a sus hijos.
static void test_nodo_invisible_no_emite()
{
    UiCanvas canvas;
    UiNode& panel = canvas.root().addChild("Panel");
    panel.position = {12.0f, 34.0f};
    panel.size     = {56.0f, 78.0f};
    panel.visible  = false;

    UiNode& child = panel.addChild("Hijo");
    child.position = {3.0f, 4.0f};
    child.size     = {11.0f, 13.0f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.empty());
    CHECK(data.vertices.empty());
}

int main()
{
    test_canvas_vacio_no_emite_nada();
    test_origen_arriba_izquierda();
    test_transform_del_padre_se_acumula();
    test_mismo_atlas_y_scissor_un_solo_lote();
    test_cambiar_de_atlas_parte_el_lote();
    test_cambiar_de_scissor_parte_el_lote();
    test_scissor_del_hijo_se_interseca_con_el_del_padre();
    test_interseccion_vacia_no_emite_draw();
    test_uvs_del_subrect_del_atlas();
    test_nodo_invisible_no_emite();

    if (g_failures == 0) std::printf("ui_batch_tests: OK\n");
    else                 std::printf("ui_batch_tests: %d fallos\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
