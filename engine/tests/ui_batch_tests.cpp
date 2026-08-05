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
#include "DonTopo/UI/UiLayout.h"
#include "DonTopo/UI/UiSpriteBatch.h"
#include "DonTopo/UI/UiTextureAtlas.h"
#include "DonTopo/UI/UiWidgets.h"

#include <cmath>
#include <cstdio>
#include <cstring>

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
    UiElement& panel = canvas.root().add("Panel");
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
    UiElement& parent = canvas.root().add("Panel");
    parent.position = {120.0f, 45.0f};
    parent.scale    = {2.0f, 3.0f};    // escalas distintas por eje
    parent.drawable = false;           // solo agrupa: el único quad es el hijo

    UiElement& child = parent.add("Image");
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
        UiElement& node = canvas.root().add("Image");
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
    UiElement& a = canvas.root().add("Image");
    a.position = {17.0f, 23.0f};
    a.size     = {25.0f, 40.0f};
    a.atlas    = &hud;
    a.sprite   = "botella";

    UiElement& b = canvas.root().add("Image");
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

    UiElement& a = canvas.root().add("PanelIzquierdo");
    a.position     = {30.0f, 40.0f};
    a.size         = {120.0f, 70.0f};
    a.atlas        = &atlas;
    a.sprite       = "botella";
    a.clipChildren = true;

    UiElement& b = canvas.root().add("PanelDerecho");
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

    UiElement& parent = canvas.root().add("Panel");
    parent.position     = {100.0f, 50.0f};
    parent.size         = {200.0f, 80.0f};   // rect padre: x[100,300) y[50,130)
    parent.drawable     = false;
    parent.clipChildren = true;

    // Hijo más ancho que el padre y desplazado a la izquierda: si el scissor se
    // reemplazase, saldría (50,60,300,40) y se vería fuera del panel.
    UiElement& child = parent.add("Contenido");
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

    UiElement& parent = canvas.root().add("Panel");
    parent.position     = {100.0f, 50.0f};
    parent.size         = {200.0f, 80.0f};   // x[100,300)
    parent.drawable     = false;
    parent.clipChildren = true;

    UiElement& child = parent.add("Fuera");
    child.position     = {400.0f, 10.0f};    // mundo x[500,560): sin solape
    child.size         = {60.0f, 30.0f};
    child.clipChildren = true;

    // Un nieto visible: si el corte no propagase, este se colaría.
    UiElement& grandchild = child.add("Nieto");
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
    UiElement& node = canvas.root().add("Image");
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
    UiElement& panel = canvas.root().add("Panel");
    panel.position = {12.0f, 34.0f};
    panel.size     = {56.0f, 78.0f};
    panel.visible  = false;

    UiElement& child = panel.add("Hijo");
    child.position = {3.0f, 4.0f};
    child.size     = {11.0f, 13.0f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.empty());
    CHECK(data.vertices.empty());
}

// anchor cuenta sobre el rect DEL PADRE y pivot sobre el DEL PROPIO ELEMENTO.
// Los dos son vec2 normalizados, así que un campo leído por el otro no daría
// ningún error: los números están elegidos para que intercambiarlos falle.
static void test_anchor_y_pivot_colocan_el_hijo()
{
    UiCanvas canvas;

    UiElement& parent = canvas.root().add("Panel");
    parent.position = {100.0f, 40.0f};
    parent.size     = {200.0f, 120.0f};   // ancho != alto
    parent.drawable = false;              // el único quad es el hijo

    UiElement& child = parent.add("Image");
    // anchorMin == anchorMax: punto de ancla, sin estirar.
    child.anchorMin = {0.5f, 1.0f};       // centro-abajo del padre
    child.anchorMax = {0.5f, 1.0f};
    child.pivot     = {0.5f, 0.5f};       // por su propio centro
    child.position = {7.0f, -13.0f};      // desplazamiento desde el ancla
    child.size     = {40.0f, 24.0f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;

    // x: 100 + 0.5*200 + 7 - 0.5*40 = 187
    // y: 40  + 1.0*120 - 13 - 0.5*24 = 135
    // Sin anchor saldría (107,27); sin pivot, (207,147); con anchor y pivot
    // intercambiados, y = 63.
    CHECK(nearly(data.vertices[0].pos.x, 187.0f));
    CHECK(nearly(data.vertices[0].pos.y, 135.0f));
    // El tamaño no lo tocan ni el ancla ni el pivote.
    CHECK(nearly(data.vertices[2].pos.x, 227.0f));
    CHECK(nearly(data.vertices[2].pos.y, 159.0f));
}

// La opacidad se ACUMULA por el árbol y acaba multiplicando el alfa del color
// del vértice. Los tres factores son distintos: 0.5 * 0.25 * 0.8 = 0.1.
static void test_opacity_se_acumula_en_el_alfa()
{
    UiCanvas canvas;

    UiElement& parent = canvas.root().add("Panel");
    parent.position = {10.0f, 20.0f};
    parent.size     = {60.0f, 30.0f};
    parent.opacity  = 0.5f;
    parent.color    = {1.0f, 1.0f, 1.0f, 1.0f};

    UiElement& child = parent.add("Image");
    child.position = {5.0f, 6.0f};
    child.size     = {12.0f, 14.0f};
    child.opacity  = 0.25f;
    child.color    = {1.0f, 1.0f, 1.0f, 0.8f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;

    // El padre solo lleva la suya: 1.0 * 0.5.
    CHECK(nearly(data.vertices[0].color.a, 0.5f));
    // El hijo, la del padre por la suya por el alfa de su color.
    CHECK(nearly(data.vertices[4].color.a, 0.1f));
    // El RGB no lo toca: solo el alfa.
    CHECK(nearly(data.vertices[4].color.r, 1.0f));
    // Y la opacidad NO parte el lote: no es estado del command buffer.
    CHECK(data.batches.size() == 1);
}

// Un derivado se dibuja exactamente igual que la base — esta fase no le añade
// comportamiento — pero se identifica por typeName() sin RTTI.
static void test_widget_derivado_se_dibuja_como_la_base()
{
    UiCanvas canvas;

    Image& img = canvas.root().add<Image>("Icono");
    img.position = {31.0f, 43.0f};
    img.size     = {17.0f, 29.0f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;
    CHECK(nearly(data.vertices[0].pos.x, 31.0f));
    CHECK(nearly(data.vertices[0].pos.y, 43.0f));
    CHECK(nearly(data.vertices[2].pos.x, 48.0f));
    CHECK(nearly(data.vertices[2].pos.y, 72.0f));

    // add<T> devuelve el tipo concreto, no la base, y cada tipo dice el suyo.
    CHECK(std::strcmp(img.typeName(), "Image") == 0);
    CHECK(std::strcmp(canvas.root().add<Button>("Aceptar").typeName(), "Button") == 0);
    CHECK(std::strcmp(canvas.root().add<ScrollView>("Lista").typeName(), "ScrollView") == 0);
    CHECK(std::strcmp(canvas.root().add("Suelto").typeName(), "UiElement") == 0);

    // Y el árbol es dueño del derivado por la base: sin destructor virtual esto
    // sería UB al vaciarlo.
    canvas.clear();
    CHECK(canvas.root().children().empty());
}

// anchorMin != anchorMax en un eje = ESTIRADO: el rect sale de los márgenes y
// ni size ni pivot de ese eje se leen. Los cuatro márgenes son distintos entre
// sí, así que confundir left con right (o X con Y) cambia los números.
static void test_stretch_por_ejes_con_margenes()
{
    UiCanvas canvas;

    UiElement& parent = canvas.root().add("Panel");
    parent.position = {100.0f, 40.0f};
    parent.size     = {200.0f, 120.0f};   // ancho != alto
    parent.drawable = false;

    // Estirado en X, anclado a un punto en Y: marginTop/Bottom NO se leen.
    UiElement& wide = parent.add("Barra");
    wide.anchorMin    = {0.25f, 0.5f};
    wide.anchorMax    = {0.75f, 0.5f};
    wide.marginLeft   = 11.0f;
    wide.marginRight  = 7.0f;
    wide.marginTop    = 3.0f;
    wide.marginBottom = 5.0f;
    wide.position     = {7.0f, -13.0f};   // en X se ignora; en Y sí cuenta
    wide.pivot        = {0.5f, 0.5f};     // en X se ignora
    wide.size         = {40.0f, 24.0f};   // en X se ignora

    // Estirado en Y, anclado a un punto en X: marginLeft/Right NO se leen.
    UiElement& tall = parent.add("Columna");
    tall.anchorMin    = {0.5f, 0.2f};
    tall.anchorMax    = {0.5f, 0.9f};
    tall.marginLeft   = 11.0f;
    tall.marginRight  = 7.0f;
    tall.marginTop    = 3.0f;
    tall.marginBottom = 5.0f;
    tall.position     = {6.0f, 0.0f};
    tall.pivot        = {1.0f, 0.0f};
    tall.size         = {30.0f, 50.0f};   // en Y se ignora

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;

    // x0 = 100 + 0.25*200 + 11 = 161 ; x1 = 100 + 0.75*200 - 7 = 243
    CHECK(nearly(data.vertices[0].pos.x, 161.0f));
    CHECK(nearly(data.vertices[1].pos.x, 243.0f));
    // Y anclada al punto medio: 40 + 0.5*120 - 13 - 0.5*24 = 75, alto el suyo.
    CHECK(nearly(data.vertices[0].pos.y, 75.0f));
    CHECK(nearly(data.vertices[2].pos.y, 99.0f));

    // y0 = 40 + 0.2*120 + 3 = 67 ; y1 = 40 + 0.9*120 - 5 = 143
    CHECK(nearly(data.vertices[4].pos.y, 67.0f));
    CHECK(nearly(data.vertices[6].pos.y, 143.0f));
    // X anclada: 100 + 0.5*200 + 6 - 1.0*30 = 176, ancho el suyo.
    CHECK(nearly(data.vertices[4].pos.x, 176.0f));
    CHECK(nearly(data.vertices[6].pos.x, 206.0f));

    // Ni el estirado ni el anclado parten el lote: mismo atlas, mismo scissor.
    CHECK(data.batches.size() == 1);
}

// Un preset solo escribe anchorMin/anchorMax/pivot: el rect que sale es el que
// dicta la fórmula, sin ninguna rama especial en el batcher.
static void test_presets_de_ancla()
{
    UiCanvas canvas;

    UiElement& parent = canvas.root().add("Panel");
    parent.position = {100.0f, 40.0f};
    parent.size     = {200.0f, 120.0f};
    parent.drawable = false;

    UiElement& centered = parent.add("Centrado");
    applyAnchorPreset(centered, UiAnchorPreset::MiddleCenter);
    centered.size = {40.0f, 24.0f};

    UiElement& full = parent.add("Fondo");
    applyAnchorPreset(full, UiAnchorPreset::StretchAll);
    full.marginLeft   = 11.0f;
    full.marginRight  = 7.0f;
    full.marginTop    = 3.0f;
    full.marginBottom = 5.0f;
    full.size         = {1.0f, 1.0f};   // ignorado en los dos ejes

    // El preset NO toca position ni size.
    CHECK(nearly(centered.position.x, 0.0f) && nearly(centered.position.y, 0.0f));
    CHECK(nearly(full.size.x, 1.0f) && nearly(full.size.y, 1.0f));
    CHECK(nearly(centered.anchorMin.x, 0.5f) && nearly(centered.anchorMax.x, 0.5f));
    CHECK(nearly(full.anchorMin.x, 0.0f) && nearly(full.anchorMax.x, 1.0f));

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;

    // MiddleCenter: 100 + 0.5*200 - 0.5*40 = 180 ; 40 + 0.5*120 - 0.5*24 = 88.
    CHECK(nearly(data.vertices[0].pos.x, 180.0f));
    CHECK(nearly(data.vertices[0].pos.y, 88.0f));
    CHECK(nearly(data.vertices[2].pos.x, 220.0f));
    CHECK(nearly(data.vertices[2].pos.y, 112.0f));

    // StretchAll: el rect del padre menos los cuatro márgenes.
    CHECK(nearly(data.vertices[4].pos.x, 111.0f));
    CHECK(nearly(data.vertices[4].pos.y, 43.0f));
    CHECK(nearly(data.vertices[6].pos.x, 293.0f));
    CHECK(nearly(data.vertices[6].pos.y, 155.0f));
}

// Horizontal: los hijos van uno tras otro en X respetando SU ancho, con el
// padding del contenedor y el spacing entre ellos. spacing.x != spacing.y para
// que usar el eje equivocado falle. Y un hijo con ignoreLayout no ocupa hueco.
static void test_layout_horizontal_coloca_en_x()
{
    UiCanvas canvas;

    UiElement& row = canvas.root().add("Fila");
    row.position      = {50.0f, 30.0f};
    row.size          = {400.0f, 100.0f};
    row.drawable      = false;
    row.layoutMode    = UiLayoutMode::Horizontal;
    row.paddingLeft   = 9.0f;
    row.paddingTop    = 4.0f;
    row.paddingRight  = 6.0f;
    row.paddingBottom = 8.0f;
    row.spacing       = {13.0f, 21.0f};

    // Va PRIMERO: si consumiese slot, correría a los tres siguientes.
    UiElement& floating = row.add("Suelto");
    floating.ignoreLayout = true;
    floating.position     = {5.0f, 5.0f};
    floating.size         = {9.0f, 9.0f};

    const float widths[3]  = {20.0f, 35.0f, 12.0f};
    const float heights[3] = {10.0f, 18.0f, 6.0f};
    for (int i = 0; i < 3; ++i)
    {
        UiElement& item = row.add("Item");
        item.size = {widths[i], heights[i]};
    }

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 16);
    if (data.vertices.size() != 16) return;

    // El de ignoreLayout se ancla como siempre: 50+5, 30+5.
    CHECK(nearly(data.vertices[0].pos.x, 55.0f));
    CHECK(nearly(data.vertices[0].pos.y, 35.0f));

    // x: 50+9 = 59 ; 59+20+13 = 92 ; 92+35+13 = 140. Todos con y = 30+4 = 34.
    CHECK(nearly(data.vertices[4].pos.x, 59.0f));
    CHECK(nearly(data.vertices[8].pos.x, 92.0f));
    CHECK(nearly(data.vertices[12].pos.x, 140.0f));
    CHECK(nearly(data.vertices[4].pos.y, 34.0f));
    CHECK(nearly(data.vertices[8].pos.y, 34.0f));
    CHECK(nearly(data.vertices[12].pos.y, 34.0f));

    // El layout respeta el tamaño propio de cada hijo.
    CHECK(nearly(data.vertices[6].pos.x, 79.0f));
    CHECK(nearly(data.vertices[6].pos.y, 44.0f));
    CHECK(nearly(data.vertices[14].pos.x, 152.0f));
    CHECK(nearly(data.vertices[14].pos.y, 40.0f));
}

// Vertical: lo mismo en Y, y crossAlign Center centra en el eje transversal
// (la X), que es donde se nota si el layout confunde los ejes.
static void test_layout_vertical_coloca_en_y()
{
    UiCanvas canvas;

    UiElement& col = canvas.root().add("Columna");
    col.position      = {60.0f, 25.0f};
    col.size          = {150.0f, 300.0f};
    col.drawable      = false;
    col.layoutMode    = UiLayoutMode::Vertical;
    col.paddingLeft   = 7.0f;
    col.paddingTop    = 5.0f;
    col.paddingRight  = 3.0f;
    col.paddingBottom = 11.0f;
    col.spacing       = {17.0f, 9.0f};
    col.crossAlign    = UiCrossAlign::Center;

    const float widths[3]  = {30.0f, 50.0f, 20.0f};
    const float heights[3] = {14.0f, 22.0f, 8.0f};
    for (int i = 0; i < 3; ++i)
    {
        UiElement& item = col.add("Item");
        item.size = {widths[i], heights[i]};
    }

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 12);
    if (data.vertices.size() != 12) return;

    // y: 25+5 = 30 ; 30+14+9 = 53 ; 53+22+9 = 84.
    CHECK(nearly(data.vertices[0].pos.y, 30.0f));
    CHECK(nearly(data.vertices[4].pos.y, 53.0f));
    CHECK(nearly(data.vertices[8].pos.y, 84.0f));

    // Centrado en X sobre el ancho interior 150-7-3 = 140, desde x = 60+7 = 67.
    CHECK(nearly(data.vertices[0].pos.x, 122.0f));   // 67 + (140-30)/2
    CHECK(nearly(data.vertices[4].pos.x, 112.0f));   // 67 + (140-50)/2
    CHECK(nearly(data.vertices[8].pos.x, 127.0f));   // 67 + (140-20)/2
}

// Grid: la celda manda sobre el size del hijo, y con columns = 2 el tercero
// baja de fila. spacing.x y spacing.y separan columnas y filas por su cuenta.
static void test_layout_grid_llena_por_filas()
{
    UiCanvas canvas;

    UiElement& grid = canvas.root().add("Rejilla");
    grid.position    = {40.0f, 70.0f};
    grid.size        = {500.0f, 400.0f};
    grid.drawable    = false;
    grid.layoutMode  = UiLayoutMode::Grid;
    grid.columns     = 2;
    grid.cellSize    = {30.0f, 18.0f};
    grid.spacing     = {5.0f, 9.0f};
    grid.paddingLeft = 4.0f;
    grid.paddingTop  = 6.0f;

    for (int i = 0; i < 4; ++i)
    {
        UiElement& item = grid.add("Celda");
        item.size = {77.0f + (float)i, 88.0f};   // lo pisa cellSize
    }

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 16);
    if (data.vertices.size() != 16) return;

    // origen = (44, 76) ; paso = (30+5, 18+9) = (35, 27).
    CHECK(nearly(data.vertices[0].pos.x, 44.0f)  && nearly(data.vertices[0].pos.y, 76.0f));
    CHECK(nearly(data.vertices[4].pos.x, 79.0f)  && nearly(data.vertices[4].pos.y, 76.0f));
    CHECK(nearly(data.vertices[8].pos.x, 44.0f)  && nearly(data.vertices[8].pos.y, 103.0f));
    CHECK(nearly(data.vertices[12].pos.x, 79.0f) && nearly(data.vertices[12].pos.y, 103.0f));

    // Y todas miden la celda, no lo que decía su size.
    CHECK(nearly(data.vertices[2].pos.x, 74.0f) && nearly(data.vertices[2].pos.y, 94.0f));
    CHECK(nearly(data.vertices[14].pos.x, 109.0f) && nearly(data.vertices[14].pos.y, 121.0f));
}

// Content size fitter: el panel no declara tamaño y lo saca de sus hijos ya
// colocados más el padding. Los cuatro paddings son distintos entre sí.
static void test_content_size_fitter_crece_hasta_los_hijos()
{
    UiCanvas canvas;

    UiElement& panel = canvas.root().add("Panel");
    panel.position      = {200.0f, 90.0f};
    panel.size          = {0.0f, 0.0f};   // lo resuelve el fitter
    panel.layoutMode    = UiLayoutMode::Horizontal;
    panel.paddingLeft   = 11.0f;
    panel.paddingTop    = 3.0f;
    panel.paddingRight  = 7.0f;
    panel.paddingBottom = 5.0f;
    panel.spacing       = {13.0f, 21.0f};
    panel.fitWidth      = true;
    panel.fitHeight     = true;

    const float widths[3]  = {20.0f, 35.0f, 12.0f};
    const float heights[3] = {10.0f, 18.0f, 6.0f};
    for (int i = 0; i < 3; ++i)
    {
        UiElement& item = panel.add("Item");
        item.size = {widths[i], heights[i]};
    }

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 16);
    if (data.vertices.size() != 16) return;

    // ancho: 11 + (20+35+12) + 2*13 + 7 = 111 ; alto: 3 + max(10,18,6) + 5 = 26.
    CHECK(nearly(data.vertices[0].pos.x, 200.0f));
    CHECK(nearly(data.vertices[0].pos.y, 90.0f));
    CHECK(nearly(data.vertices[2].pos.x, 311.0f));
    CHECK(nearly(data.vertices[2].pos.y, 116.0f));

    // Y los hijos siguen donde el layout los pone: 200+11 = 211, 90+3 = 93.
    CHECK(nearly(data.vertices[4].pos.x, 211.0f));
    CHECK(nearly(data.vertices[4].pos.y, 93.0f));
    CHECK(nearly(data.vertices[8].pos.x, 244.0f));   // 211+20+13
    CHECK(nearly(data.vertices[12].pos.x, 292.0f));  // 244+35+13
}

// El pase de medida guarda un tamaño por nodo en pre-orden y el de colocación
// lo indexa. Un hijo INVISIBLE con su propio subárbol no se visita: si el
// recorrido no se saltase el subárbol ENTERO, el siguiente hijo leería la
// medida de un nieto y saldría con otro tamaño y en otro sitio.
static void test_hijo_invisible_no_desincroniza_la_medida()
{
    UiCanvas canvas;

    UiElement& panel = canvas.root().add("Panel");
    panel.position      = {100.0f, 50.0f};
    panel.layoutMode    = UiLayoutMode::Vertical;
    panel.paddingLeft   = 2.0f;
    panel.paddingTop    = 4.0f;
    panel.paddingRight  = 6.0f;
    panel.paddingBottom = 8.0f;
    panel.spacing       = {3.0f, 7.0f};
    panel.fitWidth      = true;
    panel.fitHeight     = true;

    UiElement& hidden = panel.add("Oculto");
    hidden.visible = false;
    hidden.size    = {123.0f, 456.0f};        // nada de esto puede colarse
    UiElement& deep = hidden.add("Nieto");
    deep.size = {77.0f, 88.0f};
    deep.add("Bisnieto").size = {99.0f, 111.0f};

    UiElement& first = panel.add("Primero");
    first.size = {40.0f, 20.0f};

    UiElement& second = panel.add("Segundo");
    second.size = {25.0f, 30.0f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    // Panel + los dos visibles. Ni el oculto ni sus descendientes.
    CHECK(data.vertices.size() == 12);
    if (data.vertices.size() != 12) return;

    // Panel: ancho 2 + max(40,25) + 6 = 48 ; alto 4 + (20+7+30) + 8 = 69.
    CHECK(nearly(data.vertices[0].pos.x, 100.0f) && nearly(data.vertices[0].pos.y, 50.0f));
    CHECK(nearly(data.vertices[2].pos.x, 148.0f));
    CHECK(nearly(data.vertices[2].pos.y, 119.0f));

    // Primero conserva SU tamaño (40x20) en (102, 54).
    CHECK(nearly(data.vertices[4].pos.x, 102.0f) && nearly(data.vertices[4].pos.y, 54.0f));
    CHECK(nearly(data.vertices[6].pos.x, 142.0f) && nearly(data.vertices[6].pos.y, 74.0f));

    // Segundo: 54 + 20 + 7 = 81, con su 25x30.
    CHECK(nearly(data.vertices[8].pos.x, 102.0f) && nearly(data.vertices[8].pos.y, 81.0f));
    CHECK(nearly(data.vertices[10].pos.x, 127.0f) && nearly(data.vertices[10].pos.y, 111.0f));
}

// Neutralidad: por defecto no hay ni estirado, ni layout, ni fitter, y el
// batcher tiene que dar EXACTAMENTE lo mismo que antes de esta fase.
static void test_neutralidad_de_los_campos_nuevos()
{
    UiElement fresh;
    CHECK(nearly(fresh.anchorMin.x, fresh.anchorMax.x));
    CHECK(nearly(fresh.anchorMin.y, fresh.anchorMax.y));
    CHECK(nearly(fresh.marginLeft, 0.0f) && nearly(fresh.marginRight, 0.0f));
    CHECK(nearly(fresh.marginTop, 0.0f) && nearly(fresh.marginBottom, 0.0f));
    CHECK(fresh.layoutMode == UiLayoutMode::None);
    CHECK(!fresh.fitWidth && !fresh.fitHeight && !fresh.ignoreLayout);

    // El mismo árbol de test_transform_del_padre_se_acumula, con los mismos
    // números: escala heredada, posición local y un solo lote.
    UiCanvas canvas;
    UiElement& parent = canvas.root().add("Panel");
    parent.position = {120.0f, 45.0f};
    parent.scale    = {2.0f, 3.0f};
    parent.drawable = false;

    UiElement& child = parent.add("Image");
    child.position = {10.0f, 20.0f};
    child.size     = {5.0f, 7.0f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 4);
    CHECK(data.indices.size() == 6);
    if (data.vertices.size() != 4) return;
    CHECK(nearly(data.vertices[0].pos.x, 140.0f));
    CHECK(nearly(data.vertices[0].pos.y, 105.0f));
    CHECK(nearly(data.vertices[2].pos.x, 150.0f));
    CHECK(nearly(data.vertices[2].pos.y, 126.0f));
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
    test_anchor_y_pivot_colocan_el_hijo();
    test_opacity_se_acumula_en_el_alfa();
    test_widget_derivado_se_dibuja_como_la_base();
    test_stretch_por_ejes_con_margenes();
    test_presets_de_ancla();
    test_layout_horizontal_coloca_en_x();
    test_layout_vertical_coloca_en_y();
    test_layout_grid_llena_por_filas();
    test_content_size_fitter_crece_hasta_los_hijos();
    test_hijo_invisible_no_desincroniza_la_medida();
    test_neutralidad_de_los_campos_nuevos();

    if (g_failures == 0) std::printf("ui_batch_tests: OK\n");
    else                 std::printf("ui_batch_tests: %d fallos\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
