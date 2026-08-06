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
#include "DonTopo/UI/UiFont.h"
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

// ── Texto ───────────────────────────────────────────────────────────────────
// La fuente se rellena A MANO por la API pública de UiFont: sin TTF, sin
// FreeType y sin Vulkan, así que el test es determinista y no depende de que
// haya ningún fichero al lado del ejecutable.
//
// Todos los números son DISTINTOS entre sí a propósito: advance != alto,
// bearing != 0 y != entre ejes, y kerning negativo y distinto por par. Con
// valores neutros, ignorar el kerning o intercambiar bearing X e Y pasaría
// igual.
static constexpr float kBakeSize = 32.0f;

static void makeTestFont(UiFont& font)
{
    font.setBakeSize(kBakeSize);
    font.setPixelRange(4.0f);
    font.setMetrics(24.0f, 8.0f, 40.0f);   // ascent, descent, lineHeight
    font.atlas().setSize(128, 64);

    UiGlyph a{};
    a.rect     = {16.0f, 8.0f, 10.0f, 14.0f};
    a.bearingX = 3.0f;
    a.bearingY = 12.0f;
    a.advance  = 21.0f;
    font.addGlyph('A', a);

    UiGlyph b{};
    b.rect     = {40.0f, 24.0f, 9.0f, 18.0f};
    b.bearingX = -2.0f;
    b.bearingY = 17.0f;
    b.advance  = 13.0f;
    font.addGlyph('B', b);

    UiGlyph c{};
    c.rect     = {70.0f, 2.0f, 12.0f, 11.0f};
    c.bearingX = 5.0f;
    c.bearingY = 9.0f;
    c.advance  = 27.0f;
    font.addGlyph('C', c);

    font.setKerning('A', 'B', -4.0f);
    font.setKerning('B', 'C', -6.0f);
}

// Cursor: X del glyph n = X del n-1 + advance + kerning(n-1, n) + bearing.
static void test_texto_avance_y_kerning_colocan_las_x()
{
    UiFont font;
    makeTestFont(font);

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position = {100.0f, 50.0f};
    label.font     = &font;
    label.text     = "ABC";
    label.fontSize = kBakeSize;   // sin escala: los números son los del horneado

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 12);
    if (data.vertices.size() != 12) return;

    // El lote es el de la fuente, no el nulo del color plano.
    CHECK(data.batches[0].atlas == &font.atlas());

    // pluma 100 -> 'A' en 100+3
    CHECK(nearly(data.vertices[0].pos.x, 103.0f));
    // pluma 121, kerning A-B -4 -> 117 -> 'B' en 117-2
    CHECK(nearly(data.vertices[4].pos.x, 115.0f));
    // pluma 130, kerning B-C -6 -> 124 -> 'C' en 124+5
    CHECK(nearly(data.vertices[8].pos.x, 129.0f));

    // Y el ancho de cada quad sale de su rect, no del advance.
    CHECK(nearly(data.vertices[1].pos.x - data.vertices[0].pos.x, 10.0f));
    CHECK(nearly(data.vertices[5].pos.x - data.vertices[4].pos.x, 9.0f));
    CHECK(nearly(data.vertices[9].pos.x - data.vertices[8].pos.x, 12.0f));
}

// El bearing separa la pluma del quad, y los dos ejes NO valen lo mismo:
// intercambiarlos mueve los tres glyphs.
static void test_texto_bearing_separa_el_quad_del_cursor()
{
    UiFont font;
    makeTestFont(font);

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position = {100.0f, 50.0f};
    label.font     = &font;
    label.text     = "ABC";
    label.fontSize = kBakeSize;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 12);
    if (data.vertices.size() != 12) return;

    // Línea base a un ascent del borde superior: 50 + 24 = 74. El borde
    // superior de cada quad es baseline - bearingY (+Y va hacia ABAJO).
    CHECK(nearly(data.vertices[0].pos.y, 62.0f));    // 74 - 12
    CHECK(nearly(data.vertices[4].pos.y, 57.0f));    // 74 - 17
    CHECK(nearly(data.vertices[8].pos.y, 65.0f));    // 74 -  9

    // Y el alto sale del rect: 14, 18 y 11, ninguno igual a su advance.
    CHECK(nearly(data.vertices[3].pos.y - data.vertices[0].pos.y, 14.0f));
    CHECK(nearly(data.vertices[7].pos.y - data.vertices[4].pos.y, 18.0f));
    CHECK(nearly(data.vertices[11].pos.y - data.vertices[8].pos.y, 11.0f));
}

// De esto va el MSDF: cambiar de tamaño escala el quad y el screenPxRange, y NO
// toca ni una UV. Si hubiera que rehornear, las UVs cambiarían.
static void test_texto_fontsize_escala_el_quad_pero_no_las_uvs()
{
    UiFont font;
    makeTestFont(font);

    UiCanvas horneado;
    Text& base = horneado.root().add<Text>("Base");
    base.position = {100.0f, 50.0f};
    base.font     = &font;
    base.text     = "A";
    base.fontSize = kBakeSize;

    UiDrawData dataBase;
    horneado.buildDrawData(kW, kH, dataBase);

    UiCanvas ampliado;
    Text& grande = ampliado.root().add<Text>("Grande");
    grande.position = {100.0f, 50.0f};
    grande.font     = &font;
    grande.text     = "A";
    grande.fontSize = kBakeSize * 1.5f;   // 48 px

    UiDrawData dataGrande;
    ampliado.buildDrawData(kW, kH, dataGrande);

    CHECK(dataBase.vertices.size() == 4);
    CHECK(dataGrande.vertices.size() == 4);
    if (dataBase.vertices.size() != 4 || dataGrande.vertices.size() != 4) return;

    // Quad 1.5x: 10x14 -> 15x21, y la esquina se recoloca por el bearing ya
    // escalado (100 + 3*1.5, 50 + 24*1.5 - 12*1.5).
    CHECK(nearly(dataGrande.vertices[0].pos.x, 104.5f));
    CHECK(nearly(dataGrande.vertices[0].pos.y, 68.0f));
    CHECK(nearly(dataGrande.vertices[2].pos.x - dataGrande.vertices[0].pos.x, 15.0f));
    CHECK(nearly(dataGrande.vertices[2].pos.y - dataGrande.vertices[0].pos.y, 21.0f));

    // screenPxRange escalado igual: 4 -> 6.
    CHECK(nearly(dataBase.vertices[0].params.y, 4.0f));
    CHECK(nearly(dataGrande.vertices[0].params.y, 6.0f));

    // Modo MSDF en los dos, y MISMAS UVs: 16/128, 8/64, 26/128, 22/64.
    CHECK(nearly(dataBase.vertices[0].params.x, 1.0f));
    CHECK(nearly(dataGrande.vertices[0].params.x, 1.0f));
    for (size_t i = 0; i < 4; ++i)
    {
        CHECK(nearly(dataBase.vertices[i].uv.x, dataGrande.vertices[i].uv.x));
        CHECK(nearly(dataBase.vertices[i].uv.y, dataGrande.vertices[i].uv.y));
    }
    CHECK(nearly(dataBase.vertices[0].uv.x, 0.125f));
    CHECK(nearly(dataBase.vertices[0].uv.y, 0.125f));
    CHECK(nearly(dataBase.vertices[2].uv.x, 26.0f / 128.0f));
    CHECK(nearly(dataBase.vertices[2].uv.y, 22.0f / 64.0f));
}

// La sombra son quads EXTRA por delante, con el mismo atlas y el mismo scissor:
// el doble de geometría, pero UN solo lote.
static void test_texto_sombra_duplica_los_quads_en_un_solo_lote()
{
    UiFont font;
    makeTestFont(font);

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position     = {100.0f, 50.0f};
    label.font         = &font;
    label.text         = "AB";
    label.fontSize     = kBakeSize;
    label.shadowOffset = {3.0f, -5.0f};   // los dos ejes distintos y de signo distinto
    label.shadowColor  = {0.0f, 0.0f, 0.0f, 0.5f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    // 2 glyphs x 2 pases.
    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 16);
    CHECK(data.indices.size() == 24);
    if (data.vertices.size() != 16) return;

    // La sombra va PRIMERO: los cuatro primeros quads son los desplazados.
    CHECK(nearly(data.vertices[0].pos.x, 106.0f));   // 103 + 3
    CHECK(nearly(data.vertices[0].pos.y, 57.0f));    //  62 - 5
    CHECK(nearly(data.vertices[4].pos.x, 118.0f));   // 115 + 3
    CHECK(nearly(data.vertices[4].pos.y, 52.0f));    //  57 - 5

    // Y el texto detrás, sin desplazar.
    CHECK(nearly(data.vertices[8].pos.x, 103.0f));
    CHECK(nearly(data.vertices[8].pos.y, 62.0f));
    CHECK(nearly(data.vertices[12].pos.x, 115.0f));
    CHECK(nearly(data.vertices[12].pos.y, 57.0f));

    // Color de sombra en los primeros y de relleno (blanco) en los últimos.
    CHECK(nearly(data.vertices[0].color.a, 0.5f));
    CHECK(nearly(data.vertices[8].color.a, 1.0f));

    // La sombra usa las MISMAS UVs que su glyph: es el mismo atlas.
    CHECK(nearly(data.vertices[0].uv.x, data.vertices[8].uv.x));
    CHECK(nearly(data.vertices[0].uv.y, data.vertices[8].uv.y));

    // Sin offset no hay pase de sombra.
    label.shadowOffset = {0.0f, 0.0f};
    UiDrawData sinSombra;
    canvas.buildDrawData(kW, kH, sinSombra);
    CHECK(sinSombra.vertices.size() == 8);
    CHECK(sinSombra.batches.size() == 1);
}

// El outline viaja por vértice: ni parte el lote ni necesita otra textura.
static void test_texto_outline_viaja_al_vertice_sin_partir_el_lote()
{
    UiFont font;
    makeTestFont(font);

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position     = {100.0f, 50.0f};
    label.font         = &font;
    label.text         = "ABC";
    label.fontSize     = kBakeSize;
    label.outlineWidth = 2.5f;
    label.outlineColor = {0.25f, 0.5f, 0.75f, 1.0f};
    label.shadowOffset = {3.0f, -5.0f};
    label.shadowColor  = {0.0f, 0.0f, 0.0f, 0.5f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 24);
    if (data.vertices.size() != 24) return;

    // Los 12 primeros son la sombra: sin outline.
    for (size_t i = 0; i < 12; ++i)
        CHECK(nearly(data.vertices[i].params.z, 0.0f));

    // Los 12 siguientes lo llevan, con su color en effect.
    for (size_t i = 12; i < 24; ++i)
    {
        CHECK(nearly(data.vertices[i].params.z, 2.5f));
        CHECK(nearly(data.vertices[i].effect.x, 0.25f));
        CHECK(nearly(data.vertices[i].effect.y, 0.5f));
        CHECK(nearly(data.vertices[i].effect.z, 0.75f));
    }
}

// Neutralidad del texto: sin Text, o con un Text sin fuente, el batcher da lo
// mismo que antes de esta fase y params.x se queda a 0 (modo sprite).
static void test_neutralidad_del_texto()
{
    UiVertex fresh;
    CHECK(nearly(fresh.params.x, 0.0f) && nearly(fresh.params.y, 0.0f));
    CHECK(nearly(fresh.params.z, 0.0f) && nearly(fresh.effect.a, 0.0f));

    // El mismo árbol de test_transform_del_padre_se_acumula, vértice a vértice.
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
    if (data.vertices.size() != 4) return;
    CHECK(nearly(data.vertices[0].pos.x, 140.0f));
    CHECK(nearly(data.vertices[0].pos.y, 105.0f));
    CHECK(nearly(data.vertices[2].pos.x, 150.0f));
    CHECK(nearly(data.vertices[2].pos.y, 126.0f));
    for (const UiVertex& v : data.vertices)
    {
        CHECK(nearly(v.params.x, 0.0f));
        CHECK(nearly(v.params.y, 0.0f));
        CHECK(nearly(v.params.z, 0.0f));
        CHECK(nearly(v.effect.a, 0.0f));
    }

    // Un Text sin fuente vuelve a dibujarse como su base: un quad y nada más.
    UiCanvas conTexto;
    Text& label = conTexto.root().add<Text>("SinFuente");
    label.position = {31.0f, 43.0f};
    label.size     = {17.0f, 29.0f};
    label.text     = "ABC";

    UiDrawData plano;
    conTexto.buildDrawData(kW, kH, plano);
    CHECK(plano.batches.size() == 1);
    CHECK(plano.vertices.size() == 4);
    if (plano.vertices.size() != 4) return;
    CHECK(nearly(plano.vertices[0].pos.x, 31.0f));
    CHECK(nearly(plano.vertices[2].pos.y, 72.0f));
    CHECK(nearly(plano.vertices[0].params.x, 0.0f));
}

// Glyphs de relleno para los tests de tags literales: lo único que importa es
// que TODOS los caracteres de la cadena tengan uno, para poder contar un quad
// por carácter visible.
static void addAsciiGlyphs(UiFont& font, const char* chars)
{
    UiGlyph g{};
    g.rect     = {0.0f, 48.0f, 6.0f, 9.0f};
    g.bearingX = 1.0f;
    g.bearingY = 7.0f;
    g.advance  = 8.0f;

    for (const char* p = chars; *p != '\0'; ++p)
        font.addGlyph((uint32_t)(unsigned char)*p, g);
}

// El espacio no tiene contorno (rect a 0): solo avanza. Es el que reparte
// Justify y el que da los puntos de corte del wrap.
static void addSpaceGlyph(UiFont& font, float advance)
{
    UiGlyph sp{};
    sp.advance = advance;
    font.addGlyph(' ', sp);
}

// <color> pinta SOLO su tramo y el cierre restaura el de fuera, anidado
// incluido. Ni la posición ni el número de quads cambian por llevar tags.
static void test_texto_color_por_tramos()
{
    UiFont font;
    makeTestFont(font);

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position = {100.0f, 50.0f};
    label.font     = &font;
    label.fontSize = kBakeSize;
    label.color    = {0.2f, 0.4f, 0.6f, 1.0f};
    label.text     = "A<color=#FF0000>B</color>C";

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    // Tres glyphs: los tags no dejan ni un quad.
    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 12);
    if (data.vertices.size() != 12) return;

    // Y las X son EXACTAMENTE las del texto plano: 103, 115 y 129.
    CHECK(nearly(data.vertices[0].pos.x, 103.0f));
    CHECK(nearly(data.vertices[4].pos.x, 115.0f));
    CHECK(nearly(data.vertices[8].pos.x, 129.0f));

    CHECK(nearly(data.vertices[0].color.r, 0.2f) && nearly(data.vertices[0].color.b, 0.6f));
    CHECK(nearly(data.vertices[4].color.r, 1.0f) && nearly(data.vertices[4].color.g, 0.0f));
    CHECK(nearly(data.vertices[8].color.r, 0.2f) && nearly(data.vertices[8].color.b, 0.6f));

    // Anidado: el cierre de dentro devuelve al rojo, no al color de fuera.
    label.text = "A<color=#FF0000>B<color=#00FF80>C</color></color>";
    UiDrawData anidado;
    canvas.buildDrawData(kW, kH, anidado);
    CHECK(anidado.vertices.size() == 12);
    if (anidado.vertices.size() != 12) return;

    CHECK(nearly(anidado.vertices[0].color.r, 0.2f));
    CHECK(nearly(anidado.vertices[4].color.r, 1.0f) && nearly(anidado.vertices[4].color.g, 0.0f));
    CHECK(nearly(anidado.vertices[8].color.g, 1.0f) && nearly(anidado.vertices[8].color.b, 128.0f / 255.0f));

    // Y el alfa de 8 dígitos también llega.
    label.text = "<color=#10203040>A</color>";
    UiDrawData conAlfa;
    canvas.buildDrawData(kW, kH, conAlfa);
    CHECK(conAlfa.vertices.size() == 4);
    if (conAlfa.vertices.size() != 4) return;
    CHECK(nearly(conAlfa.vertices[0].color.a, 64.0f / 255.0f));
}

// <size> escala su tramo Y mueve el cursor de los siguientes: el avance y el
// kerning también se escalan, no solo el quad.
static void test_texto_size_escala_el_tramo_y_mueve_el_cursor()
{
    UiFont font;
    makeTestFont(font);

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position = {100.0f, 50.0f};
    label.font     = &font;
    label.fontSize = kBakeSize;
    label.text     = "A<size=48>B</size>C";

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 12);
    if (data.vertices.size() != 12) return;

    // A a escala 1: como siempre.
    CHECK(nearly(data.vertices[0].pos.x, 103.0f));
    CHECK(nearly(data.vertices[0].pos.y, 62.0f));

    // B a 48/32 = 1.5: pluma 121 + kerning -4*1.5 = 115, bearing -2*1.5.
    CHECK(nearly(data.vertices[4].pos.x, 112.0f));
    CHECK(nearly(data.vertices[4].pos.y, 74.0f - 17.0f * 1.5f));
    CHECK(nearly(data.vertices[6].pos.x, 112.0f + 9.0f * 1.5f));
    CHECK(nearly(data.vertices[6].pos.y, 74.0f - 17.0f * 1.5f + 18.0f * 1.5f));

    // Y C vuelve a escala 1 PERO desde una pluma que arrastra el avance grande:
    // 115 + 13*1.5 = 134.5, kerning -6, bearing +5.
    CHECK(nearly(data.vertices[8].pos.x, 133.5f));
    CHECK(nearly(data.vertices[8].pos.y, 65.0f));
    CHECK(nearly(data.vertices[10].pos.x, 145.5f));

    // El tamaño NO toca las UVs: es el mismo trozo de atlas.
    CHECK(nearly(data.vertices[4].uv.x, 40.0f / 128.0f));
    CHECK(nearly(data.vertices[4].uv.y, 24.0f / 64.0f));
    CHECK(nearly(data.vertices[6].uv.x, 49.0f / 128.0f));
    CHECK(nearly(data.vertices[6].uv.y, 42.0f / 64.0f));
}

// <b> engorda por el canal del outline y <i> cizalla el quad: ni una UV ni un
// quad de diferencia con el texto plano.
static void test_texto_negrita_y_cursiva_sin_tocar_uvs()
{
    UiFont font;
    makeTestFont(font);

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position = {100.0f, 50.0f};
    label.font     = &font;
    label.fontSize = kBakeSize;
    label.color    = {0.9f, 0.8f, 0.7f, 1.0f};
    label.text     = "ABC";

    UiDrawData plano;
    canvas.buildDrawData(kW, kH, plano);

    label.text = "<b>A</b><i>B</i>C";
    UiDrawData estilado;
    canvas.buildDrawData(kW, kH, estilado);

    CHECK(plano.vertices.size() == 12);
    CHECK(estilado.vertices.size() == plano.vertices.size());
    CHECK(estilado.batches.size() == 1);
    if (estilado.vertices.size() != 12 || plano.vertices.size() != 12) return;

    // Mismas UVs, vértice a vértice.
    for (size_t i = 0; i < 12; ++i)
    {
        CHECK(nearly(estilado.vertices[i].uv.x, plano.vertices[i].uv.x));
        CHECK(nearly(estilado.vertices[i].uv.y, plano.vertices[i].uv.y));
    }

    // A en negrita: grosor 0.08 * 32 y el "outline" del color del relleno.
    for (size_t i = 0; i < 4; ++i)
    {
        CHECK(nearly(estilado.vertices[i].params.z, 2.56f));
        CHECK(nearly(estilado.vertices[i].effect.x, 0.9f));
        CHECK(nearly(estilado.vertices[i].effect.z, 0.7f));
        CHECK(nearly(estilado.vertices[i].pos.x, plano.vertices[i].pos.x));
    }

    // B en cursiva: la línea base está en 74 y el quad va de 57 a 75, así que
    // arriba se va +4.25 y abajo -0.25. El avance NO cambia.
    CHECK(nearly(estilado.vertices[4].pos.x, 115.0f + 4.25f));
    CHECK(nearly(estilado.vertices[5].pos.x, 115.0f + 9.0f + 4.25f));
    CHECK(nearly(estilado.vertices[6].pos.x, 115.0f + 9.0f - 0.25f));
    CHECK(nearly(estilado.vertices[7].pos.x, 115.0f - 0.25f));
    CHECK(nearly(estilado.vertices[4].params.z, 0.0f));

    // Y C, fuera de los dos tramos, exactamente donde estaba.
    CHECK(nearly(estilado.vertices[8].pos.x, plano.vertices[8].pos.x));
    CHECK(nearly(estilado.vertices[8].params.z, 0.0f));
}

// Lo que no se entiende se DIBUJA: un quad por carácter visible, ni uno menos.
static void test_texto_tag_malformado_sale_literal()
{
    UiFont font;
    makeTestFont(font);
    addAsciiGlyphs(font, "<>/=#bcefilorsz");

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position = {100.0f, 50.0f};
    label.font     = &font;
    label.fontSize = kBakeSize;

    struct Caso { const char* texto; size_t visibles; };
    const Caso casos[] = {
        {"<colorr=#fff>", 13},   // tag desconocido
        {"<size=>",        7},   // sin número
        {"<b",             2},   // sin cerrar el '>'
        {"</color>",       8},   // cierre huérfano
        {"<color=#ff>",   11},   // hex de longitud imposible
    };

    for (const Caso& caso : casos)
    {
        label.text = caso.texto;
        UiDrawData data;
        canvas.buildDrawData(kW, kH, data);
        CHECK(data.vertices.size() == caso.visibles * 4);
        if (data.vertices.size() != caso.visibles * 4)
            std::printf("       (texto \"%s\": %zu quads, esperados %zu)\n",
                        caso.texto, data.vertices.size() / 4, caso.visibles);
    }

    // Y un '<' suelto en medio de texto normal no se come nada de lo de detrás.
    label.text = "A<B";
    UiDrawData suelto;
    canvas.buildDrawData(kW, kH, suelto);
    CHECK(suelto.vertices.size() == 12);
}

// La MISMA línea en tres X distintas, calculadas contra el ancho del rect.
static void test_texto_alineacion_izquierda_centro_derecha()
{
    UiFont font;
    makeTestFont(font);

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position = {100.0f, 50.0f};
    label.size     = {200.0f, 60.0f};   // ancho != alto a propósito
    label.font     = &font;
    label.fontSize = kBakeSize;
    label.text     = "ABC";

    // Ancho de la línea: 21 + (-4+13) + (-6+27) = 51.
    UiDrawData izquierda;
    label.align = UiTextAlign::Left;
    canvas.buildDrawData(kW, kH, izquierda);

    UiDrawData centro;
    label.align = UiTextAlign::Center;
    canvas.buildDrawData(kW, kH, centro);

    UiDrawData derecha;
    label.align = UiTextAlign::Right;
    canvas.buildDrawData(kW, kH, derecha);

    CHECK(izquierda.vertices.size() == 12);
    CHECK(centro.vertices.size() == 12);
    CHECK(derecha.vertices.size() == 12);
    if (izquierda.vertices.size() != 12 || centro.vertices.size() != 12 || derecha.vertices.size() != 12) return;

    CHECK(nearly(izquierda.vertices[0].pos.x, 103.0f));            // 100 + bearing 3
    CHECK(nearly(centro.vertices[0].pos.x,    177.5f));            // 100 + (200-51)/2 + 3
    CHECK(nearly(derecha.vertices[0].pos.x,   252.0f));            // 100 + 200-51 + 3

    // La Y es la misma en los tres: alinear es SOLO en X.
    CHECK(nearly(centro.vertices[0].pos.y, izquierda.vertices[0].pos.y));
    CHECK(nearly(derecha.vertices[0].pos.y, izquierda.vertices[0].pos.y));

    // Y el último glyph de la línea derecha acaba pegado al borde: 252 + ...
    CHECK(nearly(derecha.vertices[8].pos.x, 278.0f));              // 249 + 21+9-6 + 5
}

// Justify reparte el sobrante entre los espacios, y NUNCA en la última línea ni
// en una cortada por '\n'.
static void test_texto_justify_no_toca_la_ultima_linea()
{
    UiFont font;
    makeTestFont(font);
    addSpaceGlyph(font, 7.0f);

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position = {100.0f, 50.0f};
    label.size     = {45.0f, 120.0f};
    label.font     = &font;
    label.fontSize = kBakeSize;
    label.wordWrap = true;
    label.align    = UiTextAlign::Justify;
    label.text     = "A B C";

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    // Dos líneas: "A B" (21+7+13 = 41) y "C". El espacio no deja quad.
    CHECK(data.vertices.size() == 12);
    if (data.vertices.size() != 12) return;

    // Sobrante 45-41 = 4 para UN espacio: B se va 4 px a la derecha.
    CHECK(nearly(data.vertices[0].pos.x, 103.0f));
    CHECK(nearly(data.vertices[4].pos.x, 130.0f));   // 100 + 21 + (7+4) - 2

    // La última línea NO se justifica: C empieza pegada a la izquierda.
    CHECK(nearly(data.vertices[8].pos.x, 105.0f));   // 100 + bearing 5
    CHECK(nearly(data.vertices[8].pos.y, 105.0f));   // baseline 74 + 40 - bearingY 9

    // Con Left, el mismo espacio vale 7 y B se queda en 126.
    label.align = UiTextAlign::Left;
    UiDrawData izquierda;
    canvas.buildDrawData(kW, kH, izquierda);
    CHECK(izquierda.vertices.size() == 12);
    if (izquierda.vertices.size() != 12) return;
    CHECK(nearly(izquierda.vertices[4].pos.x, 126.0f));

    // Una línea cortada por '\n' tampoco se justifica, aunque le sobre sitio.
    label.align    = UiTextAlign::Justify;
    label.wordWrap = false;
    label.size     = {200.0f, 120.0f};
    label.text     = "A B\nC";
    UiDrawData conSalto;
    canvas.buildDrawData(kW, kH, conSalto);
    CHECK(conSalto.vertices.size() == 12);
    if (conSalto.vertices.size() != 12) return;
    CHECK(nearly(conSalto.vertices[4].pos.x, 126.0f));
    CHECK(nearly(conSalto.vertices[8].pos.y, 105.0f));   // y C en la segunda línea

    // Y con espacios en las DOS líneas: ni la del '\n' ni la última se estiran,
    // aunque a las dos les sobren 159 px. Sin esta pareja, quitar la guarda de
    // "última línea" no lo notaría nadie: la última suele no tener espacios.
    label.text = "A B\nA B";
    UiDrawData dosLineas;
    canvas.buildDrawData(kW, kH, dosLineas);
    CHECK(dosLineas.vertices.size() == 16);
    if (dosLineas.vertices.size() != 16) return;
    CHECK(nearly(dosLineas.vertices[4].pos.x, 126.0f));    // B de la línea del '\n'
    CHECK(nearly(dosLineas.vertices[12].pos.x, 126.0f));   // B de la ÚLTIMA línea
    CHECK(nearly(dosLineas.vertices[12].pos.y, 97.0f));    // 114 - 17
}

// Wrap por palabras, y por glyph cuando una palabra no cabe ni sola.
static void test_texto_word_wrap_por_palabras_y_por_glyph()
{
    UiFont font;
    makeTestFont(font);
    addSpaceGlyph(font, 7.0f);

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position = {100.0f, 50.0f};
    label.size     = {45.0f, 120.0f};
    label.font     = &font;
    label.fontSize = kBakeSize;
    label.wordWrap = true;
    label.text     = "A B C";

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 12);
    if (data.vertices.size() != 12) return;

    // "A B" arriba (misma línea base) y "C" abajo, un lineHeight más.
    CHECK(nearly(data.vertices[0].pos.y, 62.0f));    // 74 - 12
    CHECK(nearly(data.vertices[4].pos.y, 57.0f));    // 74 - 17
    CHECK(nearly(data.vertices[8].pos.y, 105.0f));   // 114 - 9
    CHECK(nearly(data.vertices[8].pos.x, 105.0f));

    // Sin wrap, las tres van seguidas en una línea.
    label.wordWrap = false;
    UiDrawData seguido;
    canvas.buildDrawData(kW, kH, seguido);
    CHECK(seguido.vertices.size() == 12);
    if (seguido.vertices.size() != 12) return;
    CHECK(nearly(seguido.vertices[8].pos.y, 65.0f));   // 74 - 9, la misma línea

    // Una palabra más ancha que el rect se parte por glyph: "ABC" son 51 contra
    // un rect de 30, así que caben A y B (30) y C baja.
    label.wordWrap = true;
    label.size     = {30.0f, 120.0f};
    label.text     = "ABC";
    UiDrawData partida;
    canvas.buildDrawData(kW, kH, partida);
    CHECK(partida.vertices.size() == 12);
    if (partida.vertices.size() != 12) return;
    CHECK(nearly(partida.vertices[0].pos.x, 103.0f));
    CHECK(nearly(partida.vertices[4].pos.x, 115.0f));
    CHECK(nearly(partida.vertices[4].pos.y, 57.0f));
    // C abre línea: sin kerning heredado y pegada a la izquierda del rect.
    CHECK(nearly(partida.vertices[8].pos.x, 105.0f));
    CHECK(nearly(partida.vertices[8].pos.y, 105.0f));
}

// Ellipsis recorta y remata; Clip recorta con el scissor; Overflow no hace nada.
static void test_texto_overflow_ellipsis_clip_y_overflow()
{
    UiFont font;
    makeTestFont(font);

    UiGlyph puntos{};
    puntos.rect     = {90.0f, 40.0f, 10.0f, 6.0f};
    puntos.bearingX = 1.0f;
    puntos.bearingY = 4.0f;
    puntos.advance  = 12.0f;
    font.addGlyph(0x2026, puntos);   // '…'

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position = {100.0f, 50.0f};
    label.size     = {40.0f, 60.0f};
    label.font     = &font;
    label.fontSize = kBakeSize;
    label.text     = "ABC";

    // Overflow: ni recorte ni puntos, y el scissor sigue siendo el de pantalla.
    label.overflow = UiTextOverflow::Overflow;
    UiDrawData libre;
    canvas.buildDrawData(kW, kH, libre);
    CHECK(libre.vertices.size() == 12);
    CHECK(libre.batches.size() == 1);
    if (libre.batches.empty()) return;
    CHECK(libre.batches[0].scissor.width == kW && libre.batches[0].scissor.height == kH);

    // Clip: los MISMOS quads, pero el lote sale recortado al rect.
    label.overflow = UiTextOverflow::Clip;
    UiDrawData recortado;
    canvas.buildDrawData(kW, kH, recortado);
    CHECK(recortado.vertices.size() == 12);
    CHECK(recortado.batches.size() == 1);
    if (recortado.batches.empty()) return;
    CHECK(recortado.batches[0].scissor.x == 100 && recortado.batches[0].scissor.y == 50);
    CHECK(recortado.batches[0].scissor.width == 40 && recortado.batches[0].scissor.height == 60);
    for (size_t i = 0; i < recortado.vertices.size(); ++i)
        CHECK(nearly(recortado.vertices[i].pos.x, libre.vertices[i].pos.x));

    // Ellipsis: 51 + 12 no cabe en 40, así que caen C y B y queda "A…".
    label.overflow = UiTextOverflow::Ellipsis;
    UiDrawData cortado;
    canvas.buildDrawData(kW, kH, cortado);
    CHECK(cortado.vertices.size() == 8);
    if (cortado.vertices.size() != 8) return;

    CHECK(nearly(cortado.vertices[0].pos.x, 103.0f));
    CHECK(nearly(cortado.vertices[4].pos.x, 122.0f));           // pluma 121 + bearing 1
    CHECK(nearly(cortado.vertices[4].uv.x, 90.0f / 128.0f));    // y son las UVs del '…'
    // Y el bloque no se sale del rect: 122 + 10 <= 140.
    CHECK(cortado.vertices[5].pos.x <= 140.0f);

    // Sin '…' en el atlas se cae a tres puntos.
    UiFont conPuntos;
    makeTestFont(conPuntos);
    UiGlyph punto{};
    punto.rect     = {100.0f, 50.0f, 3.0f, 3.0f};
    punto.bearingX = 1.0f;
    punto.bearingY = 3.0f;
    punto.advance  = 5.0f;
    conPuntos.addGlyph('.', punto);

    label.font = &conPuntos;
    UiDrawData fallback;
    canvas.buildDrawData(kW, kH, fallback);
    CHECK(fallback.vertices.size() == 16);   // A + tres puntos
    if (fallback.vertices.size() != 16) return;
    CHECK(nearly(fallback.vertices[4].pos.x, 122.0f));
    CHECK(nearly(fallback.vertices[8].pos.x, 127.0f));
    CHECK(nearly(fallback.vertices[12].pos.x, 132.0f));
}

// El bloque de texto medido alimenta el fitter, y desde ahí el layout del padre.
static void test_texto_alimenta_el_content_size_fitter()
{
    UiFont font;
    makeTestFont(font);

    UiCanvas canvas;
    UiElement& panel = canvas.root().add("Panel");
    panel.position   = {60.0f, 30.0f};
    panel.layoutMode = UiLayoutMode::Vertical;
    panel.fitWidth   = true;
    panel.fitHeight  = true;

    Text& label     = panel.add<Text>("Etiqueta");
    label.font      = &font;
    label.fontSize  = kBakeSize;
    label.text      = "ABC";
    label.fitWidth  = true;
    label.fitHeight = true;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    // El quad del panel primero, los glyphs detrás.
    CHECK(data.vertices.size() == 4 + 12);
    if (data.vertices.size() != 16) return;

    // Ancho = la línea (51) y alto = un lineHeight (40): ni uno ni otro salen
    // del size del panel, que es {0,0}.
    CHECK(nearly(data.vertices[0].pos.x, 60.0f));
    CHECK(nearly(data.vertices[0].pos.y, 30.0f));
    CHECK(nearly(data.vertices[2].pos.x, 111.0f));
    CHECK(nearly(data.vertices[2].pos.y, 70.0f));

    // Un texto más largo empuja el panel, que es de lo que va el fitter.
    label.text = "ABCABC";
    UiDrawData largo;
    canvas.buildDrawData(kW, kH, largo);
    CHECK(largo.vertices.size() == 4 + 24);
    if (largo.vertices.size() != 28) return;
    CHECK(largo.vertices[2].pos.x > data.vertices[2].pos.x);
    CHECK(nearly(largo.vertices[2].pos.y, 70.0f));   // sigue siendo UNA línea
}

// Neutralidad: con los valores por defecto (Left, sin wrap, Overflow) el texto
// plano da EXACTAMENTE los mismos vértices y lotes que la fase anterior.
static void test_neutralidad_del_rich_text()
{
    UiFont font;
    makeTestFont(font);

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position     = {100.0f, 50.0f};
    label.font         = &font;
    label.text         = "ABC";
    label.fontSize     = kBakeSize;
    label.outlineWidth = 2.5f;
    label.outlineColor = {0.25f, 0.5f, 0.75f, 1.0f};
    label.shadowOffset = {3.0f, -5.0f};

    UiDrawData porDefecto;
    canvas.buildDrawData(kW, kH, porDefecto);

    label.align    = UiTextAlign::Left;
    label.wordWrap = false;
    label.overflow = UiTextOverflow::Overflow;

    UiDrawData explicito;
    canvas.buildDrawData(kW, kH, explicito);

    CHECK(porDefecto.vertices.size() == 24);
    CHECK(explicito.vertices.size() == porDefecto.vertices.size());
    CHECK(explicito.batches.size() == porDefecto.batches.size());
    CHECK(explicito.batches.size() == 1);
    if (explicito.vertices.size() != porDefecto.vertices.size()) return;

    for (size_t i = 0; i < porDefecto.vertices.size(); ++i)
    {
        const UiVertex& a = porDefecto.vertices[i];
        const UiVertex& b = explicito.vertices[i];
        CHECK(nearly(a.pos.x, b.pos.x) && nearly(a.pos.y, b.pos.y));
        CHECK(nearly(a.uv.x, b.uv.x) && nearly(a.uv.y, b.uv.y));
        CHECK(nearly(a.color.a, b.color.a));
        CHECK(nearly(a.params.y, b.params.y) && nearly(a.params.z, b.params.z));
        CHECK(nearly(a.effect.x, b.effect.x));
    }

    // Los tres primeros quads siguen siendo la sombra, sin outline ni engorde.
    for (size_t i = 0; i < 12; ++i)
        CHECK(nearly(porDefecto.vertices[i].params.z, 0.0f));
    for (size_t i = 12; i < 24; ++i)
        CHECK(nearly(porDefecto.vertices[i].params.z, 2.5f));
}

// ── Imágenes: fuentes y modos de dibujo ─────────────────────────────────────
// El atlas de todos estos tests: 200x100 con un sub-rect que NO empieza en el
// origen y con proporciones distintas por eje.
//   u: 50/200 = 0.25 -> 75/200 = 0.375   (du = 0.125)
//   v: 10/100 = 0.10 -> 50/100 = 0.500   (dv = 0.400)
// El sprite mide 25x40 en píxeles del atlas: ese es su tamaño NATIVO y es
// distinto del rect de todos los elementos, así que un modo que se olvide de
// él y use el rect da otros números.
static UiTextureAtlas makeAtlas()
{
    UiTextureAtlas atlas;
    atlas.setSize(200, 100);
    atlas.addSprite("botella", {50.0f, 10.0f, 25.0f, 40.0f});
    return atlas;
}

// Comparación EXACTA, campo a campo incluidos params y effect: la neutralidad
// no es "parecido", es el mismo buffer.
static bool sameVertices(const UiDrawData& a, const UiDrawData& b)
{
    if (a.vertices.size() != b.vertices.size()) return false;
    if (a.indices.size()  != b.indices.size())  return false;
    if (a.batches.size()  != b.batches.size())  return false;
    if (a.vertices.empty()) return true;
    return std::memcmp(a.vertices.data(), b.vertices.data(),
                       a.vertices.size() * sizeof(UiVertex)) == 0;
}

// Una textura suelta es un atlas SIN entradas: el nombre no resuelve y las UVs
// salen 0..1. Un solo quad, como cualquier drawable.
static void test_imagen_textura_suelta_uv_0_1()
{
    UiTextureAtlas textura;
    textura.setSize(128, 64);   // ancho != alto, y sin un solo addSprite

    UiCanvas canvas;
    Image& img = canvas.root().add<Image>("Fondo");
    img.position = {17.0f, 23.0f};
    img.size     = {90.0f, 37.0f};   // rect distinto del tamaño de la textura
    img.atlas    = &textura;
    img.sprite   = "no_registrado";

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;

    CHECK(nearly(data.vertices[0].uv.x, 0.0f) && nearly(data.vertices[0].uv.y, 0.0f));
    CHECK(nearly(data.vertices[2].uv.x, 1.0f) && nearly(data.vertices[2].uv.y, 1.0f));
    // Y el rect sigue siendo el del elemento, no el de la textura.
    CHECK(nearly(data.vertices[2].pos.x, 107.0f));
    CHECK(nearly(data.vertices[2].pos.y, 60.0f));
}

// Un sprite con nombre dentro de un atlas usa las UVs de SU sub-rect, no las
// del atlas entero: con 0..1 saldrían los cuatro valores distintos.
static void test_imagen_sprite_con_nombre_usa_su_subrect()
{
    UiTextureAtlas atlas = makeAtlas();

    UiCanvas canvas;
    Image& img = canvas.root().add<Image>("Botella");
    img.position = {31.0f, 12.0f};
    img.size     = {70.0f, 44.0f};   // ni 25x40 ni cuadrado
    img.atlas    = &atlas;
    img.sprite   = "botella";

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;

    CHECK(nearly(data.vertices[0].uv.x, 0.25f)  && nearly(data.vertices[0].uv.y, 0.10f));
    CHECK(nearly(data.vertices[2].uv.x, 0.375f) && nearly(data.vertices[2].uv.y, 0.50f));
    // El rect es el del elemento: el sprite se estira, que es lo que hace Normal.
    CHECK(nearly(data.vertices[2].pos.x, 101.0f));
    CHECK(nearly(data.vertices[2].pos.y, 56.0f));
}

// Dos Image del mismo atlas van en UN lote AUNQUE estén en modos distintos y
// emitan un montón de quads: el modo se resuelve en CPU y no es estado del
// draw. Con otro atlas, dos lotes.
static void test_imagen_modos_no_parten_el_lote()
{
    UiTextureAtlas atlas = makeAtlas();

    UiCanvas canvas;

    Image& a = canvas.root().add<Image>("Tapiz");
    a.position = {10.0f, 10.0f};
    a.size     = {50.0f, 40.0f};        // 2 columnas x 1 fila de 25x40
    a.atlas    = &atlas;
    a.sprite   = "botella";
    a.mode     = UiImageMode::Tiled;

    Image& b = canvas.root().add<Image>("Marco");
    b.position     = {100.0f, 10.0f};
    b.size         = {60.0f, 50.0f};
    b.atlas        = &atlas;
    b.sprite       = "botella";
    b.mode         = UiImageMode::Sliced;
    b.borderLeft   = 4.0f;
    b.borderRight  = 6.0f;
    b.borderTop    = 3.0f;
    b.borderBottom = 9.0f;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    // 2 quads del tiled + 9 del sliced = 11.
    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 44);
    if (data.batches.size() != 1) return;
    CHECK(data.batches[0].indexCount == 66);

    // El mismo árbol pero con el segundo en otro atlas: dos lotes.
    UiTextureAtlas otro;
    otro.setSize(64, 32);
    otro.addSprite("llave", {8.0f, 4.0f, 16.0f, 8.0f});
    b.atlas  = &otro;
    b.sprite = "llave";

    UiDrawData split;
    canvas.buildDrawData(kW, kH, split);

    CHECK(split.batches.size() == 2);
    if (split.batches.size() != 2) return;
    CHECK(split.batches[0].atlas == &atlas);
    CHECK(split.batches[1].atlas == &otro);
    CHECK(split.batches[0].indexCount == 12);   // los 2 tiles
}

// Tiled: el sprite se repite a su tamaño NATIVO (25x40) y la última fila y la
// última columna se RECORTAN por UV, no se escalan.
// rect 60x90 -> ceil(60/25) = 3 columnas (25, 25, 10) y ceil(90/40) = 3 filas
// (40, 40, 10) = 9 quads.
static void test_imagen_tiled_cuenta_y_recorte_por_uv()
{
    UiTextureAtlas atlas = makeAtlas();

    UiCanvas canvas;
    Image& img = canvas.root().add<Image>("Tapiz");
    img.position = {17.0f, 23.0f};
    img.size     = {60.0f, 90.0f};
    img.atlas    = &atlas;
    img.sprite   = "botella";
    img.mode     = UiImageMode::Tiled;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 36);
    if (data.vertices.size() != 36) return;

    // Primer tile: tamaño nativo entero y UVs COMPLETAS del sprite.
    CHECK(nearly(data.vertices[0].pos.x, 17.0f) && nearly(data.vertices[0].pos.y, 23.0f));
    CHECK(nearly(data.vertices[2].pos.x, 42.0f) && nearly(data.vertices[2].pos.y, 63.0f));
    CHECK(nearly(data.vertices[0].uv.x, 0.25f)  && nearly(data.vertices[0].uv.y, 0.10f));
    CHECK(nearly(data.vertices[2].uv.x, 0.375f) && nearly(data.vertices[2].uv.y, 0.50f));

    // Tile del medio de la primera fila: completo también, y desplazado 25 px.
    CHECK(nearly(data.vertices[4].pos.x, 42.0f));
    CHECK(nearly(data.vertices[6].pos.x, 67.0f));
    CHECK(nearly(data.vertices[6].uv.x, 0.375f));

    // Última columna (índice 2): 10 px de ancho, y la U cortada a 10/25 = 0.4
    // del sub-rect -> 0.25 + 0.125*0.4 = 0.30. Escalar en vez de recortar
    // dejaría 0.375 aquí.
    CHECK(nearly(data.vertices[8].pos.x, 67.0f));
    CHECK(nearly(data.vertices[10].pos.x, 77.0f));
    CHECK(nearly(data.vertices[10].uv.x, 0.30f));
    CHECK(nearly(data.vertices[10].uv.y, 0.50f));   // la fila 0 no se corta en V

    // Última fila (índice 6): 10 px de alto, V cortada a 10/40 = 0.25 ->
    // 0.1 + 0.4*0.25 = 0.20, y la U entera porque es la primera columna.
    CHECK(nearly(data.vertices[24].pos.y, 103.0f));
    CHECK(nearly(data.vertices[26].pos.y, 113.0f));
    CHECK(nearly(data.vertices[26].uv.y, 0.20f));
    CHECK(nearly(data.vertices[26].uv.x, 0.375f));

    // La esquina (índice 8) se corta en los DOS ejes.
    CHECK(nearly(data.vertices[34].pos.x, 77.0f) && nearly(data.vertices[34].pos.y, 113.0f));
    CHECK(nearly(data.vertices[34].uv.x, 0.30f) && nearly(data.vertices[34].uv.y, 0.20f));
}

// Pasado el tope de quads el Image cae a Normal: un rect grande con un sprite
// diminuto no puede llevarse el buffer por delante.
static void test_imagen_tiled_tope_cae_a_normal()
{
    UiTextureAtlas atlas = makeAtlas();

    UiCanvas canvas;
    Image& img = canvas.root().add<Image>("Tapiz");
    img.position = {17.0f, 23.0f};
    img.size     = {60.0f, 90.0f};   // pediría 3x3 = 9 tiles
    img.atlas    = &atlas;
    img.sprite   = "botella";
    img.mode     = UiImageMode::Tiled;
    img.maxTiles = 4;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    // Un solo quad estirado al rect entero, con el sub-rect completo.
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;
    CHECK(nearly(data.vertices[2].pos.x, 77.0f) && nearly(data.vertices[2].pos.y, 113.0f));
    CHECK(nearly(data.vertices[2].uv.x, 0.375f) && nearly(data.vertices[2].uv.y, 0.50f));
}

// Sliced: 9 quads. Bordes DISTINTOS los cuatro (4/6/3/9 px del sprite) para que
// intercambiar dos cualesquiera falle.
// rect 100x70 en (17,23):
//   columnas x = 17 / 21 / 111  con anchos 4 / 90 / 6
//   filas    y = 23 / 26 / 84   con altos  3 / 58 / 9
//   u = 0.25 / 0.27 / 0.345 / 0.375   (4/25 y 6/25 del sub-rect)
//   v = 0.10 / 0.13 / 0.410 / 0.500   (3/40 y 9/40)
static void test_imagen_sliced_esquinas_bordes_y_centro()
{
    UiTextureAtlas atlas = makeAtlas();

    UiCanvas canvas;
    Image& img = canvas.root().add<Image>("Marco");
    img.position     = {17.0f, 23.0f};
    img.size         = {100.0f, 70.0f};
    img.atlas        = &atlas;
    img.sprite       = "botella";
    img.mode         = UiImageMode::Sliced;
    img.borderLeft   = 4.0f;
    img.borderRight  = 6.0f;
    img.borderTop    = 3.0f;
    img.borderBottom = 9.0f;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 36);
    if (data.vertices.size() != 36) return;

    // Esquina superior izquierda: 4x3, su tamaño NATIVO. Estirarla daría 100x70.
    CHECK(nearly(data.vertices[0].pos.x, 17.0f) && nearly(data.vertices[0].pos.y, 23.0f));
    CHECK(nearly(data.vertices[2].pos.x, 21.0f) && nearly(data.vertices[2].pos.y, 26.0f));
    CHECK(nearly(data.vertices[0].uv.x, 0.25f)  && nearly(data.vertices[0].uv.y, 0.10f));
    CHECK(nearly(data.vertices[2].uv.x, 0.27f)  && nearly(data.vertices[2].uv.y, 0.13f));

    // Esquina superior derecha: 6x3 pegada al borde derecho.
    CHECK(nearly(data.vertices[8].pos.x, 111.0f)  && nearly(data.vertices[8].pos.y, 23.0f));
    CHECK(nearly(data.vertices[10].pos.x, 117.0f) && nearly(data.vertices[10].pos.y, 26.0f));
    CHECK(nearly(data.vertices[8].uv.x, 0.345f)   && nearly(data.vertices[10].uv.x, 0.375f));

    // Esquina inferior izquierda: 4x9.
    CHECK(nearly(data.vertices[24].pos.x, 17.0f) && nearly(data.vertices[24].pos.y, 84.0f));
    CHECK(nearly(data.vertices[26].pos.x, 21.0f) && nearly(data.vertices[26].pos.y, 93.0f));
    CHECK(nearly(data.vertices[24].uv.y, 0.41f)  && nearly(data.vertices[26].uv.y, 0.50f));

    // Esquina inferior derecha: 6x9, la única que toca las dos esquinas del UV.
    CHECK(nearly(data.vertices[32].pos.x, 111.0f) && nearly(data.vertices[32].pos.y, 84.0f));
    CHECK(nearly(data.vertices[34].pos.x, 117.0f) && nearly(data.vertices[34].pos.y, 93.0f));
    CHECK(nearly(data.vertices[34].uv.x, 0.375f)  && nearly(data.vertices[34].uv.y, 0.50f));

    // Borde superior: estirado solo en X (90 px), con el alto nativo del borde.
    CHECK(nearly(data.vertices[4].pos.x, 21.0f)  && nearly(data.vertices[4].pos.y, 23.0f));
    CHECK(nearly(data.vertices[6].pos.x, 111.0f) && nearly(data.vertices[6].pos.y, 26.0f));
    CHECK(nearly(data.vertices[4].uv.x, 0.27f)   && nearly(data.vertices[6].uv.x, 0.345f));

    // Borde izquierdo: estirado solo en Y (58 px), con el ancho nativo.
    CHECK(nearly(data.vertices[12].pos.x, 17.0f) && nearly(data.vertices[12].pos.y, 26.0f));
    CHECK(nearly(data.vertices[14].pos.x, 21.0f) && nearly(data.vertices[14].pos.y, 84.0f));

    // Centro: cubre el hueco entero, 90x58, con el sub-rect interior.
    CHECK(nearly(data.vertices[16].pos.x, 21.0f)  && nearly(data.vertices[16].pos.y, 26.0f));
    CHECK(nearly(data.vertices[18].pos.x, 111.0f) && nearly(data.vertices[18].pos.y, 84.0f));
    CHECK(nearly(data.vertices[16].uv.x, 0.27f)   && nearly(data.vertices[16].uv.y, 0.13f));
    CHECK(nearly(data.vertices[18].uv.x, 0.345f)  && nearly(data.vertices[18].uv.y, 0.41f));

    // Sin centro: 8 quads, y el quinto pasa a ser el borde derecho.
    img.fillCenter = false;
    UiDrawData sinCentro;
    canvas.buildDrawData(kW, kH, sinCentro);

    CHECK(sinCentro.batches.size() == 1);
    CHECK(sinCentro.vertices.size() == 32);
    if (sinCentro.vertices.size() != 32) return;
    CHECK(nearly(sinCentro.vertices[16].pos.x, 111.0f));
    CHECK(nearly(sinCentro.vertices[16].pos.y, 26.0f));
}

// Un rect más estrecho que la suma de bordes: los dos del eje se escalan
// proporcionalmente (4 y 6 sobre 8 -> 3.2 y 4.8) en vez de solaparse, y la
// columna del centro desaparece porque mide 0.
static void test_imagen_sliced_bordes_mayores_que_el_rect()
{
    UiTextureAtlas atlas = makeAtlas();

    UiCanvas canvas;
    Image& img = canvas.root().add<Image>("Marco");
    img.position     = {17.0f, 23.0f};
    img.size         = {8.0f, 70.0f};   // 8 < 4+6, pero 70 > 3+9
    img.atlas        = &atlas;
    img.sprite       = "botella";
    img.mode         = UiImageMode::Sliced;
    img.borderLeft   = 4.0f;
    img.borderRight  = 6.0f;
    img.borderTop    = 3.0f;
    img.borderBottom = 9.0f;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    // 3 filas x 2 columnas: la del centro mide 0 y no se emite.
    CHECK(data.vertices.size() == 24);
    if (data.vertices.size() != 24) return;

    // Izquierda: 8 * 4/10 = 3.2 -> acaba en 20.2.
    CHECK(nearly(data.vertices[0].pos.x, 17.0f));
    CHECK(nearly(data.vertices[2].pos.x, 20.2f));
    // Derecha: 8 * 6/10 = 4.8 -> empieza EXACTAMENTE donde acaba la izquierda.
    CHECK(nearly(data.vertices[4].pos.x, 20.2f));
    CHECK(nearly(data.vertices[6].pos.x, 25.0f));
    CHECK(data.vertices[4].pos.x >= data.vertices[2].pos.x);   // sin solape
    // El eje Y sí cabe: los bordes ahí no se tocan.
    CHECK(nearly(data.vertices[2].pos.y, 26.0f));
    // Y las UVs NO se reescalan: siguen siendo las de los bordes del sprite.
    CHECK(nearly(data.vertices[2].uv.x, 0.27f));
    CHECK(nearly(data.vertices[4].uv.x, 0.345f));
}

// Filled: recorta la posición Y la UV a la vez. Con la UV entera el trozo
// visible enseñaría el sprite comprimido en vez de su cuarta parte.
// rect 80x44 en (17,23), fillAmount 0.25.
static void test_imagen_filled_recorta_pos_y_uv()
{
    UiTextureAtlas atlas = makeAtlas();

    UiCanvas canvas;
    Image& img = canvas.root().add<Image>("Barra");
    img.position   = {17.0f, 23.0f};
    img.size       = {80.0f, 44.0f};
    img.atlas      = &atlas;
    img.sprite     = "botella";
    img.mode       = UiImageMode::Filled;
    img.fillAmount = 0.25f;

    // Horizontal desde el principio: 80*0.25 = 20 px y u1 = 0.25 + 0.125*0.25.
    UiDrawData hStart;
    canvas.buildDrawData(kW, kH, hStart);
    CHECK(hStart.vertices.size() == 4);
    if (hStart.vertices.size() != 4) return;
    CHECK(nearly(hStart.vertices[0].pos.x, 17.0f));
    CHECK(nearly(hStart.vertices[2].pos.x, 37.0f));
    CHECK(nearly(hStart.vertices[2].pos.y, 67.0f));          // la Y no la toca
    CHECK(nearly(hStart.vertices[0].uv.x, 0.25f));
    CHECK(nearly(hStart.vertices[2].uv.x, 0.28125f));
    CHECK(nearly(hStart.vertices[2].uv.y, 0.50f));

    // Origen opuesto: recorta por el OTRO lado, en posición y en UV.
    img.fillOrigin = UiFillOrigin::End;
    UiDrawData hEnd;
    canvas.buildDrawData(kW, kH, hEnd);
    CHECK(hEnd.vertices.size() == 4);
    if (hEnd.vertices.size() != 4) return;
    CHECK(nearly(hEnd.vertices[0].pos.x, 77.0f));
    CHECK(nearly(hEnd.vertices[2].pos.x, 97.0f));
    CHECK(nearly(hEnd.vertices[0].uv.x, 0.34375f));
    CHECK(nearly(hEnd.vertices[2].uv.x, 0.375f));

    // Vertical desde arriba: 44*0.25 = 11 px y v1 = 0.1 + 0.4*0.25 = 0.2.
    img.fillDirection = UiFillDirection::Vertical;
    img.fillOrigin    = UiFillOrigin::Start;
    UiDrawData vStart;
    canvas.buildDrawData(kW, kH, vStart);
    CHECK(vStart.vertices.size() == 4);
    if (vStart.vertices.size() != 4) return;
    CHECK(nearly(vStart.vertices[0].pos.y, 23.0f));
    CHECK(nearly(vStart.vertices[2].pos.y, 34.0f));
    CHECK(nearly(vStart.vertices[2].pos.x, 97.0f));          // la X no la toca
    CHECK(nearly(vStart.vertices[2].uv.y, 0.20f));

    // Vertical desde abajo.
    img.fillOrigin = UiFillOrigin::End;
    UiDrawData vEnd;
    canvas.buildDrawData(kW, kH, vEnd);
    CHECK(vEnd.vertices.size() == 4);
    if (vEnd.vertices.size() != 4) return;
    CHECK(nearly(vEnd.vertices[0].pos.y, 56.0f));
    CHECK(nearly(vEnd.vertices[2].pos.y, 67.0f));
    CHECK(nearly(vEnd.vertices[0].uv.y, 0.40f));
    CHECK(nearly(vEnd.vertices[2].uv.y, 0.50f));

    // A 0 no se emite NADA: ni quad, ni lote.
    img.fillAmount = 0.0f;
    UiDrawData vacio;
    canvas.buildDrawData(kW, kH, vacio);
    CHECK(vacio.vertices.empty());
    CHECK(vacio.batches.empty());
}

// A 1 el Filled tiene que dar EXACTAMENTE lo mismo que Normal, byte a byte.
static void test_imagen_filled_completo_es_normal()
{
    UiTextureAtlas atlas = makeAtlas();

    UiCanvas lleno;
    Image& a = lleno.root().add<Image>("Barra");
    a.position   = {17.0f, 23.0f};
    a.size       = {80.0f, 44.0f};
    a.atlas      = &atlas;
    a.sprite     = "botella";
    a.color      = {0.3f, 0.6f, 0.9f, 0.7f};
    a.mode       = UiImageMode::Filled;
    a.fillAmount = 1.0f;

    UiCanvas normal;
    Image& b = normal.root().add<Image>("Barra");
    b.position = {17.0f, 23.0f};
    b.size     = {80.0f, 44.0f};
    b.atlas    = &atlas;
    b.sprite   = "botella";
    b.color    = {0.3f, 0.6f, 0.9f, 0.7f};

    UiDrawData da, db;
    lleno.buildDrawData(kW, kH, da);
    normal.buildDrawData(kW, kH, db);

    CHECK(da.vertices.size() == 4);
    CHECK(sameVertices(da, db));
}

// Neutralidad: un Image en Normal da los MISMOS vértices que el drawable de
// siempre. Si los campos nuevos tocaran algo, este memcmp lo caza.
static void test_neutralidad_de_los_modos_de_imagen()
{
    UiTextureAtlas atlas = makeAtlas();

    UiCanvas conImage;
    Image& img = conImage.root().add<Image>("Icono");
    img.position = {17.0f, 23.0f};
    img.size     = {70.0f, 44.0f};
    img.atlas    = &atlas;
    img.sprite   = "botella";
    img.color    = {0.3f, 0.6f, 0.9f, 0.7f};
    img.opacity  = 0.5f;

    UiCanvas base;
    UiElement& node = base.root().add("Icono");
    node.position = {17.0f, 23.0f};
    node.size     = {70.0f, 44.0f};
    node.atlas    = &atlas;
    node.sprite   = "botella";
    node.color    = {0.3f, 0.6f, 0.9f, 0.7f};
    node.opacity  = 0.5f;

    UiDrawData da, db;
    conImage.buildDrawData(kW, kH, da);
    base.buildDrawData(kW, kH, db);

    CHECK(da.vertices.size() == 4);
    CHECK(sameVertices(da, db));

    // Y tocar los campos de los OTROS modos sin cambiar el modo tampoco mueve
    // nada: un Normal no mira ni bordes ni fillAmount.
    img.borderLeft = 4.0f;
    img.borderTop  = 3.0f;
    img.fillAmount = 0.25f;
    img.maxTiles   = 2;

    UiDrawData dc;
    conImage.buildDrawData(kW, kH, dc);
    CHECK(sameVertices(dc, db));
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
    test_texto_avance_y_kerning_colocan_las_x();
    test_texto_bearing_separa_el_quad_del_cursor();
    test_texto_fontsize_escala_el_quad_pero_no_las_uvs();
    test_texto_sombra_duplica_los_quads_en_un_solo_lote();
    test_texto_outline_viaja_al_vertice_sin_partir_el_lote();
    test_neutralidad_del_texto();

    test_texto_color_por_tramos();
    test_texto_size_escala_el_tramo_y_mueve_el_cursor();
    test_texto_negrita_y_cursiva_sin_tocar_uvs();
    test_texto_tag_malformado_sale_literal();
    test_texto_alineacion_izquierda_centro_derecha();
    test_texto_justify_no_toca_la_ultima_linea();
    test_texto_word_wrap_por_palabras_y_por_glyph();
    test_texto_overflow_ellipsis_clip_y_overflow();
    test_texto_alimenta_el_content_size_fitter();
    test_neutralidad_del_rich_text();

    test_imagen_textura_suelta_uv_0_1();
    test_imagen_sprite_con_nombre_usa_su_subrect();
    test_imagen_modos_no_parten_el_lote();
    test_imagen_tiled_cuenta_y_recorte_por_uv();
    test_imagen_tiled_tope_cae_a_normal();
    test_imagen_sliced_esquinas_bordes_y_centro();
    test_imagen_sliced_bordes_mayores_que_el_rect();
    test_imagen_filled_recorta_pos_y_uv();
    test_imagen_filled_completo_es_normal();
    test_neutralidad_de_los_modos_de_imagen();

    if (g_failures == 0) std::printf("ui_batch_tests: OK\n");
    else                 std::printf("ui_batch_tests: %d fallos\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
