// Headless test of the 2D UI batcher (no GUI, no Vulkan).
// UiSpriteBatch::build is static and pure CPU just to exercise it without
// an initialized Renderer, same as buildInstanceBatches. Plain main +
// asserts, no framework — consistent with instancing_tests.cpp.
//
// What is tested are the four things that fail SILENTLY in a UI
// renderer: the origin at the top left with +Y downward (a wrong sign
// paints the UI upside down without a single validation error), that the
// parent's transform accumulates in the child, that the batch breaks exactly
// where it should (breaking too much only costs draws; breaking too little
// paints with the wrong texture), and that the child's scissor INTERSECTS
// with the parent's instead of replacing it.
//
// All values are non-neutral and distinct from each other: with 0, 1, or
// repeated values, a field that nobody reads would pass the same.
#include "DonTopo/UI/ButtonComponent.h"   // kDefaultUiFontPath
#include "DonTopo/UI/CanvasComponent.h"   // uiWorldCanvasMatrix
#include "DonTopo/UI/UiCanvas.h"
#include "DonTopo/UI/UiFont.h"
#include "DonTopo/UI/UiLayout.h"
#include "DonTopo/UI/UiSpriteBatch.h"
#include "DonTopo/UI/UiTextureAtlas.h"
#include "DonTopo/UI/UiWidgets.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearly(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// Deliberately asymmetric render size: with 800x600, width and height
// swapped could slip through.
static constexpr uint32_t kW = 800;
static constexpr uint32_t kH = 480;

// ── Frame buffer sub-allocation (N canvas, single VkBuffer) ───────
// With one canvas per frame, record() could always bind at offset 0: no
// one else to collide with. With N, each call has to write from where
// the previous one left off — if not, the GPU (which reads the buffer at
// EXECUTE, not at RECORD) draws all N canvas with the geometry of the LAST,
// without any validation layer catching it. bumpUiCursor is the exact
// arithmetic that beginFrame()/record() uses; it's tested here without GPU
// because with two canvas on screen the failure cannot be verified any other
// way until the bug is already in place.
static void test_ui_batch_offsets_se_acumulan_dentro_del_frame()
{
    // Three distinct sizes of vertices and indices: with equal values,
    // an offset calculated with the wrong field (vertices instead of
    // indices, or vice versa) would give the same number anyway.
    uint32_t vCursor = 0, iCursor = 0;

    const uint32_t v0 = bumpUiCursor(vCursor, 10);
    const uint32_t i0 = bumpUiCursor(iCursor, 15);
    CHECK(v0 == 0);
    CHECK(i0 == 0);

    const uint32_t v1 = bumpUiCursor(vCursor, 25);
    const uint32_t i1 = bumpUiCursor(iCursor, 33);
    // The second canvas starts exactly where the first one ended.
    CHECK(v1 == 10);
    CHECK(i1 == 15);

    const uint32_t v2 = bumpUiCursor(vCursor, 4);
    const uint32_t i2 = bumpUiCursor(iCursor, 6);
    CHECK(v2 == 35);   // 10 + 25
    CHECK(i2 == 48);   // 15 + 33

    // The final cursor is the exact ACCUMULATED value: it's the total against
    // which beginFrame() has to dimension the frame buffer.
    CHECK(vCursor == 39);   // 10 + 25 + 4
    CHECK(iCursor == 54);   // 15 + 33 + 6
}

// The capacity guard that record() checks before EVERY memcpy.
// beginFrame() dimensions the buffer against the total of the PASS, but that
// invariant — "called this frame, only once, with the exact total" — is not
// enforced by the type: today it's held up by Renderer being the sole caller.
// Once a second one exists (world canvas, arriving in the scene pass, in
// another loop), a record() without its beginFrame, one called twice,
// or a total that fell short, would write OUTSIDE the mapped memory
// — a HOST write that neither Vulkan nor D3D12 validation layers see:
// no device lost, no error, just silent corruption.
// uiCursorFits is the exact check that makes that guard possible without GPU.
static void test_ui_cursor_fits_guarda_la_capacidad_del_buffer()
{
    // The normal case: the gap [base, base+count) falls within the capacity
    // that beginFrame() reserved for the complete pass.
    CHECK(uiCursorFits(0, 10, 39));
    CHECK(uiCursorFits(10, 25, 39));
    CHECK(uiCursorFits(35, 4, 39));    // ends EXACTLY at the edge: fits

    // Right at the limit: base+count == capacity is the last valid gap.
    CHECK(uiCursorFits(30, 9, 39));
    // One element too many goes over by one: doesn't fit.
    CHECK(!uiCursorFits(30, 10, 39));

    // The case that triggers the finding: a short total. With the same three
    // canvas of sizes 10/25/4 from the test above (offsets 0, 10, and 35) but
    // a capacity reserved of only 30 — as if a second caller
    // (world canvas, in another loop) had calculated the accumulation of the
    // pass without adding the third canvas and beginFrame() had reserved
    // too little:
    const uint32_t capacidadCorta = 30;
    CHECK(uiCursorFits(0, 10, capacidadCorta));     // the first DOES fit
    // The SECOND doesn't: 10+25 = 35 > 30. The guard cuts as soon as the
    // real accumulation exceeds what beginFrame() reserved, not just on the
    // last canvas of the pass.
    CHECK(!uiCursorFits(10, 25, capacidadCorta));
    CHECK(!uiCursorFits(35, 4, capacidadCorta));    // and the third, not by a long shot

    // record() without prior beginFrame() — capacity at 0, the initial state of
    // m_vertexCapacity/m_indexCapacity before the first allocation: nothing fits,
    // not even a canvas of "0 elements" at an offset other than 0. A truly
    // empty canvas (0 elements at base 0) does fit: it writes nothing, there's
    // nothing to corrupt, and record() already discards it before via
    // data.empty().
    CHECK(uiCursorFits(0, 0, 0));
    CHECK(!uiCursorFits(0, 1, 0));
    CHECK(!uiCursorFits(5, 1, 0));
}

// A canvas with no visible nodes cannot generate a batch: it's the condition
// that makes the 3D scene come out exactly the same as before this feature.
static void test_canvas_vacio_no_emite_nada()
{
    UiCanvas canvas;
    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.empty());
    CHECK(data.vertices.empty());
    CHECK(data.indices.empty());
}

// (0,0) is the TOP LEFT corner and +Y goes downward: the vertex at the
// bottom has the LARGER Y, not smaller.
static void test_origen_arriba_izquierda()
{
    UiCanvas canvas;
    UiElement& panel = canvas.root().add("Panel");
    panel.position = {0.0f, 0.0f};
    panel.size     = {37.0f, 53.0f};   // width != height: distinguishes X from Y

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 4);
    CHECK(data.indices.size() == 6);
    if (data.vertices.size() != 4) return;

    // Top left corner exactly at the origin.
    CHECK(nearly(data.vertices[0].pos.x, 0.0f));
    CHECK(nearly(data.vertices[0].pos.y, 0.0f));
    // Top right: moves only in X.
    CHECK(nearly(data.vertices[1].pos.x, 37.0f));
    CHECK(nearly(data.vertices[1].pos.y, 0.0f));
    // Bottom right and bottom left: Y = height, and LARGER than the one above.
    CHECK(nearly(data.vertices[2].pos.y, 53.0f));
    CHECK(nearly(data.vertices[3].pos.y, 53.0f));
    CHECK(data.vertices[2].pos.y > data.vertices[1].pos.y);
    CHECK(data.vertices[3].pos.y > data.vertices[0].pos.y);
    // And no negative coordinates: a bottom-left origin would push the quad
    // off the screen at the top.
    CHECK(data.vertices[2].pos.y > 0.0f);
}

// The child's position is local and the parent's scale multiplies it: child
// position and size come from the accumulated parent, not just its own fields.
static void test_transform_del_padre_se_acumula()
{
    UiCanvas canvas;
    UiElement& parent = canvas.root().add("Panel");
    parent.position = {120.0f, 45.0f};
    parent.scale    = {2.0f, 3.0f};    // different scales per axis
    parent.drawable = false;           // only groups: the only quad is the child

    UiElement& child = parent.add("Image");
    child.position = {10.0f, 20.0f};
    child.size     = {5.0f, 7.0f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;

    // 120 + 10*2 = 140 ; 45 + 20*3 = 105. Without accumulating would be (10,20),
    // and without applying parent scale, (130,65).
    CHECK(nearly(data.vertices[0].pos.x, 140.0f));
    CHECK(nearly(data.vertices[0].pos.y, 105.0f));
    // Size scaled by parent: 5*2 = 10 ; 7*3 = 21.
    CHECK(nearly(data.vertices[2].pos.x, 150.0f));
    CHECK(nearly(data.vertices[2].pos.y, 126.0f));
}

// The rect is the element's: the sprite stretches, which is what Normal does.
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
    // The two quads inside the SAME draw, not one of two.
    CHECK(data.batches[0].firstIndex == 0);
    CHECK(data.batches[0].indexCount == 12);
}

// Changing atlas breaks the batch: one draw cannot carry two textures.
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
    // Each batch points to ITS atlas and its own indices: a wrong firstIndex
    // would paint the other's quad.
    CHECK(data.batches[0].atlas == &hud);
    CHECK(data.batches[1].atlas == &iconos);
    CHECK(data.batches[0].firstIndex == 0);
    CHECK(data.batches[0].indexCount == 6);
    CHECK(data.batches[1].firstIndex == 6);
    CHECK(data.batches[1].indexCount == 6);
}

// Same atlas but different scissor: also breaks, because scissor is state
// of the command buffer and doesn't travel per vertex.
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
    b.position     = {300.0f, 210.0f};   // clearly different rect
    b.size         = {90.0f, 55.0f};
    b.atlas        = &atlas;
    b.sprite       = "botella";
    b.clipChildren = true;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 2);
    if (data.batches.size() != 2) return;
    CHECK(data.batches[0].atlas == data.batches[1].atlas);   // the atlas did NOT change
    CHECK(data.batches[0].scissor != data.batches[1].scissor);
    CHECK(data.batches[0].scissor.x == 30 && data.batches[0].scissor.y == 40);
    CHECK(data.batches[0].scissor.width == 120 && data.batches[0].scissor.height == 70);
    CHECK(data.batches[1].scissor.x == 300 && data.batches[1].scissor.y == 210);
    CHECK(data.batches[1].scissor.width == 90 && data.batches[1].scissor.height == 55);
}

// The child's scissor INTERSECTS with the parent's. With replacement the child
// would paint outside the panel containing it, which is the classic scroll bug.
static void test_scissor_del_hijo_se_interseca_con_el_del_padre()
{
    UiCanvas canvas;

    UiElement& parent = canvas.root().add("Panel");
    parent.position     = {100.0f, 50.0f};
    parent.size         = {200.0f, 80.0f};   // parent rect: x[100,300) y[50,130)
    parent.drawable     = false;
    parent.clipChildren = true;

    // Child wider than parent and offset to the left: if the scissor were
    // replaced, it would be (50,60,300,40) and show outside the panel.
    UiElement& child = parent.add("Contenido");
    child.position     = {-50.0f, 10.0f};    // world: x[50,350) y[60,100)
    child.size         = {300.0f, 40.0f};
    child.clipChildren = true;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    if (data.batches.size() != 1) return;

    const UiScissor& s = data.batches[0].scissor;
    CHECK(s.x == 100);          // clipped by the parent's left
    CHECK(s.y == 60);           // the child's, which starts lower
    CHECK(s.width == 200);      // up to the parent's right edge (300)
    CHECK(s.height == 40);      // the child's height, smaller than the parent's
}

// Empty intersection: no draw. Recording a scissor of width/height 0 would be a
// pointless command (and an easy trap to miss).
static void test_interseccion_vacia_no_emite_draw()
{
    UiCanvas canvas;

    UiElement& parent = canvas.root().add("Panel");
    parent.position     = {100.0f, 50.0f};
    parent.size         = {200.0f, 80.0f};   // x[100,300)
    parent.drawable     = false;
    parent.clipChildren = true;

    UiElement& child = parent.add("Fuera");
    child.position     = {400.0f, 10.0f};    // world x[500,560): no overlap
    child.size         = {60.0f, 30.0f};
    child.clipChildren = true;

    // Two Image of the same atlas go in ONE batch EVEN THOUGH they're in different
    // modes and emit many quads: the mode is resolved in CPU and is not draw
    // state. With another atlas, two batches.
    UiElement& grandchild = child.add("Nieto");
    grandchild.position = {5.0f, 5.0f};
    grandchild.size     = {20.0f, 10.0f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.empty());
    CHECK(data.vertices.empty());
    CHECK(data.indices.empty());
}

// Sub-rect that does NOT start at the atlas origin and with different proportions
// on each axis: inverted UVs, transposed, or normalized by the wrong
// axis would give different numbers in all four checks.
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

    // Each corner with ITS pair of UVs: order matters as much as values.
    CHECK(nearly(data.vertices[0].uv.x, 0.25f));   // top-left
    CHECK(nearly(data.vertices[0].uv.y, 0.10f));
    CHECK(nearly(data.vertices[1].uv.x, 0.375f));  // sup-der
    CHECK(nearly(data.vertices[1].uv.y, 0.10f));
    CHECK(nearly(data.vertices[2].uv.x, 0.375f));  // inf-der
    CHECK(nearly(data.vertices[2].uv.y, 0.50f));
    CHECK(nearly(data.vertices[3].uv.x, 0.25f));   // bottom-left
    CHECK(nearly(data.vertices[3].uv.y, 0.50f));

    // V grows downward, like the screen.
    CHECK(data.vertices[2].uv.y > data.vertices[1].uv.y);

    // And the atlas API says the same thing on its own.
    const UiUvRect uv = atlas.uvRect("botella");
    CHECK(nearly(uv.u0, 0.25f) && nearly(uv.v0, 0.10f));
    CHECK(nearly(uv.u1, 0.375f) && nearly(uv.v1, 0.50f));

    // A sprite that doesn't exist falls to the full rect, not garbage.
    const UiUvRect missing = atlas.uvRect("no_existe");
    CHECK(nearly(missing.u0, 0.0f) && nearly(missing.v1, 1.0f));
}

// The invisible doesn't spend vertices or batch, nor drag its children.
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

// anchor counts on the PARENT's rect and pivot on the ELEMENT's OWN rect.
// Both are normalized vec2, so one field read by the other wouldn't give
// any error: the numbers are chosen so swapping them fails.
static void test_anchor_y_pivot_colocan_el_hijo()
{
    UiCanvas canvas;

    UiElement& parent = canvas.root().add("Panel");
    parent.position = {100.0f, 40.0f};
    parent.size     = {200.0f, 120.0f};   // width != height
    parent.drawable = false;              // the only quad is the child

    UiElement& child = parent.add("Image");
    // anchorMin == anchorMax: anchor point, no stretching.
    child.anchorMin = {0.5f, 1.0f};       // center-bottom of parent
    child.anchorMax = {0.5f, 1.0f};
    child.pivot     = {0.5f, 0.5f};       // by its own center
    child.position = {7.0f, -13.0f};      // offset from the anchor
    child.size     = {40.0f, 24.0f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;

    // x: 100 + 0.5*200 + 7 - 0.5*40 = 187
    // y: 40  + 1.0*120 - 13 - 0.5*24 = 135
    // Without anchor would be (107,27); without pivot, (207,147); with anchor
    // and pivot swapped, y = 63.
    CHECK(nearly(data.vertices[0].pos.x, 187.0f));
    CHECK(nearly(data.vertices[0].pos.y, 135.0f));
    // Size is untouched by either anchor or pivot.
    CHECK(nearly(data.vertices[2].pos.x, 227.0f));
    CHECK(nearly(data.vertices[2].pos.y, 159.0f));
}

// Opacity ACCUMULATES through the tree and ends up multiplying the alpha of
// the vertex color. The three factors are distinct: 0.5 * 0.25 * 0.8 = 0.1.
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

    // The parent only carries its own: 1.0 * 0.5.
    CHECK(nearly(data.vertices[0].color.a, 0.5f));
    // The child, the parent's times its own times the alpha of its color.
    CHECK(nearly(data.vertices[4].color.a, 0.1f));
    // RGB is untouched: only alpha.
    CHECK(nearly(data.vertices[4].color.r, 1.0f));
    // And opacity does NOT break the batch: it's not command buffer state.
    CHECK(data.batches.size() == 1);
}

// A derived class draws exactly like the base — this phase doesn't add
// behavior — but identifies itself via typeName() without RTTI.
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

    // add<T> returns the concrete type, not the base, and each type says its own.
    CHECK(std::strcmp(img.typeName(), "Image") == 0);
    CHECK(std::strcmp(canvas.root().add<Button>("Aceptar").typeName(), "Button") == 0);
    CHECK(std::strcmp(canvas.root().add<ScrollView>("Lista").typeName(), "ScrollView") == 0);
    CHECK(std::strcmp(canvas.root().add("Suelto").typeName(), "UiElement") == 0);

    // And the tree owns the derived by the base: without a virtual destructor
    // this would be UB when clearing it.
    canvas.clear();
    CHECK(canvas.root().children().empty());
}

// anchorMin != anchorMax on one axis = STRETCHED: the rect goes beyond the
// margins and neither size nor pivot of that axis are read. The four margins
// are all different, so confusing left with right (or X with Y) changes the numbers.
static void test_stretch_por_ejes_con_margenes()
{
    UiCanvas canvas;

    UiElement& parent = canvas.root().add("Panel");
    parent.position = {100.0f, 40.0f};
    parent.size     = {200.0f, 120.0f};   // width != height
    parent.drawable = false;

    // Stretched in X, anchored at a point in Y: marginTop/Bottom are NOT read.
    UiElement& wide = parent.add("Barra");
    wide.anchorMin    = {0.25f, 0.5f};
    wide.anchorMax    = {0.75f, 0.5f};
    wide.marginLeft   = 11.0f;
    wide.marginRight  = 7.0f;
    wide.marginTop    = 3.0f;
    wide.marginBottom = 5.0f;
    wide.position     = {7.0f, -13.0f};   // in X it's ignored; in Y it counts
    wide.pivot        = {0.5f, 0.5f};     // in X it's ignored
    wide.size         = {40.0f, 24.0f};   // in X it's ignored

    // Stretched in Y, anchored at a point in X: marginLeft/Right are NOT read.
    UiElement& tall = parent.add("Columna");
    tall.anchorMin    = {0.5f, 0.2f};
    tall.anchorMax    = {0.5f, 0.9f};
    tall.marginLeft   = 11.0f;
    tall.marginRight  = 7.0f;
    tall.marginTop    = 3.0f;
    tall.marginBottom = 5.0f;
    tall.position     = {6.0f, 0.0f};
    tall.pivot        = {1.0f, 0.0f};
    tall.size         = {30.0f, 50.0f};   // in Y it's ignored

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 8);
    if (data.vertices.size() != 8) return;

    // x0 = 100 + 0.25*200 + 11 = 161 ; x1 = 100 + 0.75*200 - 7 = 243
    CHECK(nearly(data.vertices[0].pos.x, 161.0f));
    CHECK(nearly(data.vertices[1].pos.x, 243.0f));
    // And anchored at the midpoint: 40 + 0.5*120 - 13 - 0.5*24 = 75, with its height.
    CHECK(nearly(data.vertices[0].pos.y, 75.0f));
    CHECK(nearly(data.vertices[2].pos.y, 99.0f));

    // y0 = 40 + 0.2*120 + 3 = 67 ; y1 = 40 + 0.9*120 - 5 = 143
    CHECK(nearly(data.vertices[4].pos.y, 67.0f));
    CHECK(nearly(data.vertices[6].pos.y, 143.0f));
    // X anchored: 100 + 0.5*200 + 6 - 1.0*30 = 176, with its width.
    CHECK(nearly(data.vertices[4].pos.x, 176.0f));
    CHECK(nearly(data.vertices[6].pos.x, 206.0f));

    // Neither stretched nor anchored breaks the batch: same atlas, same scissor.
    CHECK(data.batches.size() == 1);
}

// A preset only writes anchorMin/anchorMax/pivot: the rect that comes out is
// what the formula dictates, with no special branch in the batcher.
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
    full.size         = {1.0f, 1.0f};   // ignored on both axes

    // The preset does NOT touch position or size.
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

    // StretchAll: the parent's rect minus the four margins.
    CHECK(nearly(data.vertices[4].pos.x, 111.0f));
    CHECK(nearly(data.vertices[4].pos.y, 43.0f));
    CHECK(nearly(data.vertices[6].pos.x, 293.0f));
    CHECK(nearly(data.vertices[6].pos.y, 155.0f));
}

// Horizontal: children go one after another in X respecting THEIR width, with
// the container's padding and spacing between them. spacing.x != spacing.y so
// using the wrong axis fails. And a child with ignoreLayout takes no space.
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

    // Goes FIRST: if it consumed a slot, it would push the next three.
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

    // The ignoreLayout one anchors as always: 50+5, 30+5.
    CHECK(nearly(data.vertices[0].pos.x, 55.0f));
    CHECK(nearly(data.vertices[0].pos.y, 35.0f));

    // x: 50+9 = 59 ; 59+20+13 = 92 ; 92+35+13 = 140. All with y = 30+4 = 34.
    CHECK(nearly(data.vertices[4].pos.x, 59.0f));
    CHECK(nearly(data.vertices[8].pos.x, 92.0f));
    CHECK(nearly(data.vertices[12].pos.x, 140.0f));
    CHECK(nearly(data.vertices[4].pos.y, 34.0f));
    CHECK(nearly(data.vertices[8].pos.y, 34.0f));
    CHECK(nearly(data.vertices[12].pos.y, 34.0f));

    // The layout respects each child's own size.
    CHECK(nearly(data.vertices[6].pos.x, 79.0f));
    CHECK(nearly(data.vertices[6].pos.y, 44.0f));
    CHECK(nearly(data.vertices[14].pos.x, 152.0f));
    CHECK(nearly(data.vertices[14].pos.y, 40.0f));
}

// Vertical: same in Y, and crossAlign Center centers on the cross axis
// (the X), which is where you notice if the layout confuses the axes.
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

    // Centered in X over inner width 150-7-3 = 140, from x = 60+7 = 67.
    CHECK(nearly(data.vertices[0].pos.x, 122.0f));   // 67 + (140-30)/2
    CHECK(nearly(data.vertices[4].pos.x, 112.0f));   // 67 + (140-50)/2
    CHECK(nearly(data.vertices[8].pos.x, 127.0f));   // 67 + (140-20)/2
}

// Grid: the cell dominates over child size, and with columns = 2 the third
// goes to the next row. spacing.x and spacing.y separate columns and rows independently.
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
        item.size = {77.0f + (float)i, 88.0f};   // cellSize overrides it
    }

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 16);
    if (data.vertices.size() != 16) return;

    // origin = (44, 76) ; step = (30+5, 18+9) = (35, 27).
    CHECK(nearly(data.vertices[0].pos.x, 44.0f)  && nearly(data.vertices[0].pos.y, 76.0f));
    CHECK(nearly(data.vertices[4].pos.x, 79.0f)  && nearly(data.vertices[4].pos.y, 76.0f));
    CHECK(nearly(data.vertices[8].pos.x, 44.0f)  && nearly(data.vertices[8].pos.y, 103.0f));
    CHECK(nearly(data.vertices[12].pos.x, 79.0f) && nearly(data.vertices[12].pos.y, 103.0f));

    // And all measure the cell, not what their size said.
    CHECK(nearly(data.vertices[2].pos.x, 74.0f) && nearly(data.vertices[2].pos.y, 94.0f));
    CHECK(nearly(data.vertices[14].pos.x, 109.0f) && nearly(data.vertices[14].pos.y, 121.0f));
}

// Content size fitter: the panel declares no size and gets it from its
// already-placed children plus padding. The four paddings are all different.
static void test_content_size_fitter_crece_hasta_los_hijos()
{
    UiCanvas canvas;

    UiElement& panel = canvas.root().add("Panel");
    panel.position      = {200.0f, 90.0f};
    panel.size          = {0.0f, 0.0f};   // the fitter resolves it
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

    // width: 11 + (20+35+12) + 2*13 + 7 = 111 ; height: 3 + max(10,18,6) + 5 = 26.
    CHECK(nearly(data.vertices[0].pos.x, 200.0f));
    CHECK(nearly(data.vertices[0].pos.y, 90.0f));
    CHECK(nearly(data.vertices[2].pos.x, 311.0f));
    CHECK(nearly(data.vertices[2].pos.y, 116.0f));

    // And children stay where the layout puts them: 200+11 = 211, 90+3 = 93.
    CHECK(nearly(data.vertices[4].pos.x, 211.0f));
    CHECK(nearly(data.vertices[4].pos.y, 93.0f));
    CHECK(nearly(data.vertices[8].pos.x, 244.0f));   // 211+20+13
    CHECK(nearly(data.vertices[12].pos.x, 292.0f));  // 244+35+13
}

// The measure pass stores one size per node in pre-order and the placement
// pass indexes it. An INVISIBLE child with its own subtree is not visited: if
// the traversal didn't skip the ENTIRE subtree, the next child would read the
// measure of a grandchild and end up with a different size and position.
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
    hidden.size    = {123.0f, 456.0f};        // none of this can slip through
    UiElement& deep = hidden.add("Nieto");
    deep.size = {77.0f, 88.0f};
    deep.add("Bisnieto").size = {99.0f, 111.0f};

    UiElement& first = panel.add("Primero");
    first.size = {40.0f, 20.0f};

    UiElement& second = panel.add("Segundo");
    second.size = {25.0f, 30.0f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    // Panel + the two visible. Not the hidden one or its descendants.
    CHECK(data.vertices.size() == 12);
    if (data.vertices.size() != 12) return;

    // Panel: width 2 + max(40,25) + 6 = 48 ; height 4 + (20+7+30) + 8 = 69.
    CHECK(nearly(data.vertices[0].pos.x, 100.0f) && nearly(data.vertices[0].pos.y, 50.0f));
    CHECK(nearly(data.vertices[2].pos.x, 148.0f));
    CHECK(nearly(data.vertices[2].pos.y, 119.0f));

    // First keeps ITS size (40x20) at (102, 54).
    CHECK(nearly(data.vertices[4].pos.x, 102.0f) && nearly(data.vertices[4].pos.y, 54.0f));
    CHECK(nearly(data.vertices[6].pos.x, 142.0f) && nearly(data.vertices[6].pos.y, 74.0f));

    // Second: 54 + 20 + 7 = 81, with its 25x30.
    CHECK(nearly(data.vertices[8].pos.x, 102.0f) && nearly(data.vertices[8].pos.y, 81.0f));
    CHECK(nearly(data.vertices[10].pos.x, 127.0f) && nearly(data.vertices[10].pos.y, 111.0f));
}

// Neutrality: by default there's no stretching, layout, or fitter, and the
// batcher has to give EXACTLY the same as before this phase.
static void test_neutralidad_de_los_campos_nuevos()
{
    UiElement fresh;
    CHECK(nearly(fresh.anchorMin.x, fresh.anchorMax.x));
    CHECK(nearly(fresh.anchorMin.y, fresh.anchorMax.y));
    CHECK(nearly(fresh.marginLeft, 0.0f) && nearly(fresh.marginRight, 0.0f));
    CHECK(nearly(fresh.marginTop, 0.0f) && nearly(fresh.marginBottom, 0.0f));
    CHECK(fresh.layoutMode == UiLayoutMode::None);
    CHECK(!fresh.fitWidth && !fresh.fitHeight && !fresh.ignoreLayout);

    // The same tree as test_transform_del_padre_se_acumula, with the same
    // numbers: inherited scale, local position, and a single batch.
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

// ── Text ───────────────────────────────────────────────────────────────────
// The font is filled MANUALLY via UiFont's public API: no TTF, no
// FreeType, and no Vulkan, so the test is deterministic and doesn't depend
// on any file next to the executable.
//
// All numbers are DISTINCT on purpose: advance != height,
// bearing != 0 and != between axes, and kerning negative and distinct per pair. With
// neutral values, ignoring kerning or swapping bearing X and Y would
// pass the same.
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

// Cursor: X of glyph n = X of n-1 + advance + kerning(n-1, n) + bearing.
static void test_texto_avance_y_kerning_colocan_las_x()
{
    UiFont font;
    makeTestFont(font);

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position = {100.0f, 50.0f};
    label.font     = &font;
    label.text     = "ABC";
    label.fontSize = kBakeSize;   // no scaling: the numbers are those of baking

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 12);
    if (data.vertices.size() != 12) return;

    // The batch is the font's, not the null one of solid color.
    CHECK(data.batches[0].atlas == &font.atlas());

    // pen 100 -> 'A' at 100+3
    CHECK(nearly(data.vertices[0].pos.x, 103.0f));
    // pen 121, kerning A-B -4 -> 117 -> 'B' at 117-2
    CHECK(nearly(data.vertices[4].pos.x, 115.0f));
    // pen 130, kerning B-C -6 -> 124 -> 'C' at 124+5
    CHECK(nearly(data.vertices[8].pos.x, 129.0f));

    // And each quad's width comes from its rect, not from advance.
    CHECK(nearly(data.vertices[1].pos.x - data.vertices[0].pos.x, 10.0f));
    CHECK(nearly(data.vertices[5].pos.x - data.vertices[4].pos.x, 9.0f));
    CHECK(nearly(data.vertices[9].pos.x - data.vertices[8].pos.x, 12.0f));
}

// The bearing separates the pen from the quad, and the two axes are NOT the
// same: swapping them moves all three glyphs.
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

    // Baseline one ascent from top edge: 50 + 24 = 74. The top edge of each
    // quad is baseline - bearingY (+Y goes DOWNWARD).
    CHECK(nearly(data.vertices[0].pos.y, 62.0f));    // 74 - 12
    CHECK(nearly(data.vertices[4].pos.y, 57.0f));    // 74 - 17
    CHECK(nearly(data.vertices[8].pos.y, 65.0f));    // 74 -  9

    // And height comes from rect: 14, 18, and 11, none equal to its advance.
    CHECK(nearly(data.vertices[3].pos.y - data.vertices[0].pos.y, 14.0f));
    CHECK(nearly(data.vertices[7].pos.y - data.vertices[4].pos.y, 18.0f));
    CHECK(nearly(data.vertices[11].pos.y - data.vertices[8].pos.y, 11.0f));
}

// That's what MSDF is about: changing size scales the quad and screenPxRange,
// and does NOT touch a single UV. If rebaking were needed, UVs would change.
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

    // Quad 1.5x: 10x14 -> 15x21, and the corner is repositioned by already-scaled
    // bearing (100 + 3*1.5, 50 + 24*1.5 - 12*1.5).
    CHECK(nearly(dataGrande.vertices[0].pos.x, 104.5f));
    CHECK(nearly(dataGrande.vertices[0].pos.y, 68.0f));
    CHECK(nearly(dataGrande.vertices[2].pos.x - dataGrande.vertices[0].pos.x, 15.0f));
    CHECK(nearly(dataGrande.vertices[2].pos.y - dataGrande.vertices[0].pos.y, 21.0f));

    // screenPxRange scaled the same: 4 -> 6.
    CHECK(nearly(dataBase.vertices[0].params.y, 4.0f));
    CHECK(nearly(dataGrande.vertices[0].params.y, 6.0f));

    // MSDF mode in both, and SAME UVs: 16/128, 8/64, 26/128, 22/64.
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

// The shadow is EXTRA quads up front, with the same atlas and same scissor:
// twice the geometry, but ONE batch.
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
    label.shadowOffset = {3.0f, -5.0f};   // two axes different and opposite signs
    label.shadowColor  = {0.0f, 0.0f, 0.0f, 0.5f};

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    // 2 glyphs x 2 pases.
    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 16);
    CHECK(data.indices.size() == 24);
    if (data.vertices.size() != 16) return;

    // The shadow goes FIRST: the first four quads are the offset ones.
    CHECK(nearly(data.vertices[0].pos.x, 106.0f));   // 103 + 3
    CHECK(nearly(data.vertices[0].pos.y, 57.0f));    //  62 - 5
    CHECK(nearly(data.vertices[4].pos.x, 118.0f));   // 115 + 3
    CHECK(nearly(data.vertices[4].pos.y, 52.0f));    //  57 - 5

    // And the text behind, unshifted.
    CHECK(nearly(data.vertices[8].pos.x, 103.0f));
    CHECK(nearly(data.vertices[8].pos.y, 62.0f));
    CHECK(nearly(data.vertices[12].pos.x, 115.0f));
    CHECK(nearly(data.vertices[12].pos.y, 57.0f));

    // Shadow color in the first and fill (white) in the last.
    CHECK(nearly(data.vertices[0].color.a, 0.5f));
    CHECK(nearly(data.vertices[8].color.a, 1.0f));

    // The shadow uses the SAME UVs as its glyph: it's the same atlas.
    CHECK(nearly(data.vertices[0].uv.x, data.vertices[8].uv.x));
    CHECK(nearly(data.vertices[0].uv.y, data.vertices[8].uv.y));

    // No offset means no shadow pass.
    label.shadowOffset = {0.0f, 0.0f};
    label.markDirty(UiElement::DirtyAll);
    UiDrawData sinSombra;
    canvas.buildDrawData(kW, kH, sinSombra);
    CHECK(sinSombra.vertices.size() == 8);
    CHECK(sinSombra.batches.size() == 1);
}

// Outline travels per vertex: neither breaks the batch nor needs another texture.
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

    // The first 12 are the shadow: no outline.
    for (size_t i = 0; i < 12; ++i)
        CHECK(nearly(data.vertices[i].params.z, 0.0f));

    // The next 12 carry it, with its color in effect.
    for (size_t i = 12; i < 24; ++i)
    {
        CHECK(nearly(data.vertices[i].params.z, 2.5f));
        CHECK(nearly(data.vertices[i].effect.x, 0.25f));
        CHECK(nearly(data.vertices[i].effect.y, 0.5f));
        CHECK(nearly(data.vertices[i].effect.z, 0.75f));
    }
}

// Text neutrality: without Text, or with a Text with no font, the batcher
// gives the same as before this phase and params.x stays at 0 (sprite mode).
static void test_neutralidad_del_texto()
{
    UiVertex fresh;
    CHECK(nearly(fresh.params.x, 0.0f) && nearly(fresh.params.y, 0.0f));
    CHECK(nearly(fresh.params.z, 0.0f) && nearly(fresh.effect.a, 0.0f));

    // The same tree as test_transform_del_padre_se_acumula, vertex by vertex.
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

    // A Text without font draws again as its base: one quad and nothing more.
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

// Placeholder glyphs for literal tag tests: all that matters is that EVERY
// character in the string has one, to count one quad per visible character.
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

// Space has no outline (rect at 0): it only advances. It's the one that
// distributes Justify and gives the wrap cut points.
static void addSpaceGlyph(UiFont& font, float advance)
{
    UiGlyph sp{};
    sp.advance = advance;
    font.addGlyph(' ', sp);
}

// <color> paints ONLY its span and the close restores the outside one, nested
// included. Neither position nor quad count changes from carrying tags.
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

    // Three glyphs: tags leave not a single quad.
    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 12);
    if (data.vertices.size() != 12) return;

    // And the X's are EXACTLY those of plain text: 103, 115, and 129.
    CHECK(nearly(data.vertices[0].pos.x, 103.0f));
    CHECK(nearly(data.vertices[4].pos.x, 115.0f));
    CHECK(nearly(data.vertices[8].pos.x, 129.0f));

    CHECK(nearly(data.vertices[0].color.r, 0.2f) && nearly(data.vertices[0].color.b, 0.6f));
    CHECK(nearly(data.vertices[4].color.r, 1.0f) && nearly(data.vertices[4].color.g, 0.0f));
    CHECK(nearly(data.vertices[8].color.r, 0.2f) && nearly(data.vertices[8].color.b, 0.6f));

    // Nested: the inner close returns to red, not the outer color.
    label.text = "A<color=#FF0000>B<color=#00FF80>C</color></color>";
    label.markDirty(UiElement::DirtyAll);
    UiDrawData anidado;
    canvas.buildDrawData(kW, kH, anidado);
    CHECK(anidado.vertices.size() == 12);
    if (anidado.vertices.size() != 12) return;

    CHECK(nearly(anidado.vertices[0].color.r, 0.2f));
    CHECK(nearly(anidado.vertices[4].color.r, 1.0f) && nearly(anidado.vertices[4].color.g, 0.0f));
    CHECK(nearly(anidado.vertices[8].color.g, 1.0f) && nearly(anidado.vertices[8].color.b, 128.0f / 255.0f));

    // And 8-digit alpha also arrives.
    label.text = "<color=#10203040>A</color>";
    label.markDirty(UiElement::DirtyAll);
    UiDrawData conAlfa;
    canvas.buildDrawData(kW, kH, conAlfa);
    CHECK(conAlfa.vertices.size() == 4);
    if (conAlfa.vertices.size() != 4) return;
    CHECK(nearly(conAlfa.vertices[0].color.a, 64.0f / 255.0f));
}

// <size> scales its span and moves the cursor of the following: advance and
// kerning also scale, not just the quad.
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

    // A at scale 1: as always.
    CHECK(nearly(data.vertices[0].pos.x, 103.0f));
    CHECK(nearly(data.vertices[0].pos.y, 62.0f));

    // B at 48/32 = 1.5: pen 121 + kerning -4*1.5 = 115, bearing -2*1.5.
    CHECK(nearly(data.vertices[4].pos.x, 112.0f));
    CHECK(nearly(data.vertices[4].pos.y, 74.0f - 17.0f * 1.5f));
    CHECK(nearly(data.vertices[6].pos.x, 112.0f + 9.0f * 1.5f));
    CHECK(nearly(data.vertices[6].pos.y, 74.0f - 17.0f * 1.5f + 18.0f * 1.5f));

    // And C back to scale 1 BUT from a pen that drags the large advance:
    // 115 + 13*1.5 = 134.5, kerning -6, bearing +5.
    CHECK(nearly(data.vertices[8].pos.x, 133.5f));
    CHECK(nearly(data.vertices[8].pos.y, 65.0f));
    CHECK(nearly(data.vertices[10].pos.x, 145.5f));

    // Size does NOT touch UVs: it's the same piece of atlas.
    CHECK(nearly(data.vertices[4].uv.x, 40.0f / 128.0f));
    CHECK(nearly(data.vertices[4].uv.y, 24.0f / 64.0f));
    CHECK(nearly(data.vertices[6].uv.x, 49.0f / 128.0f));
    CHECK(nearly(data.vertices[6].uv.y, 42.0f / 64.0f));
}

// <b> fattens via the outline channel and <i> shears the quad: neither a UV
// nor a quad different from plain text.
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
    label.markDirty(UiElement::DirtyAll);
    UiDrawData estilado;
    canvas.buildDrawData(kW, kH, estilado);

    CHECK(plano.vertices.size() == 12);
    CHECK(estilado.vertices.size() == plano.vertices.size());
    CHECK(estilado.batches.size() == 1);
    if (estilado.vertices.size() != 12 || plano.vertices.size() != 12) return;

    // Same UVs, vertex by vertex.
    for (size_t i = 0; i < 12; ++i)
    {
        CHECK(nearly(estilado.vertices[i].uv.x, plano.vertices[i].uv.x));
        CHECK(nearly(estilado.vertices[i].uv.y, plano.vertices[i].uv.y));
    }

    // A in bold: thickness 0.08 * 32 and the "outline" of the fill color.
    for (size_t i = 0; i < 4; ++i)
    {
        CHECK(nearly(estilado.vertices[i].params.z, 2.56f));
        CHECK(nearly(estilado.vertices[i].effect.x, 0.9f));
        CHECK(nearly(estilado.vertices[i].effect.z, 0.7f));
        CHECK(nearly(estilado.vertices[i].pos.x, plano.vertices[i].pos.x));
    }

    // B in italic: baseline is at 74 and quad goes from 57 to 75, so at the top
    // it goes +4.25 and at the bottom -0.25. Advance does NOT change.
    CHECK(nearly(estilado.vertices[4].pos.x, 115.0f + 4.25f));
    CHECK(nearly(estilado.vertices[5].pos.x, 115.0f + 9.0f + 4.25f));
    CHECK(nearly(estilado.vertices[6].pos.x, 115.0f + 9.0f - 0.25f));
    CHECK(nearly(estilado.vertices[7].pos.x, 115.0f - 0.25f));
    CHECK(nearly(estilado.vertices[4].params.z, 0.0f));

    // And C, outside both spans, exactly where it was.
    CHECK(nearly(estilado.vertices[8].pos.x, plano.vertices[8].pos.x));
    CHECK(nearly(estilado.vertices[8].params.z, 0.0f));
}

// What is not understood is DRAWN: one quad per visible character, not less.
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
        {"<colorr=#fff>", 13},   // unknown tag
        {"<size=>",        7},   // no number
        {"<b",             2},   // no closing '>'
        {"</color>",       8},   // orphan close
        {"<color=#ff>",   11},   // hex of impossible length
    };

    for (const Caso& caso : casos)
    {
        label.text = caso.texto;
        label.markDirty(UiElement::DirtyAll);
        UiDrawData data;
        canvas.buildDrawData(kW, kH, data);
        CHECK(data.vertices.size() == caso.visibles * 4);
        if (data.vertices.size() != caso.visibles * 4)
            std::printf("       (texto \"%s\": %zu quads, esperados %zu)\n",
                        caso.texto, data.vertices.size() / 4, caso.visibles);
    }

    // And a lone '<' in the middle of normal text doesn't eat anything after it.
    label.text = "A<B";
    label.markDirty(UiElement::DirtyAll);
    UiDrawData suelto;
    canvas.buildDrawData(kW, kH, suelto);
    CHECK(suelto.vertices.size() == 12);
}

// The SAME line at three different X's, calculated against rect width.
static void test_texto_alineacion_izquierda_centro_derecha()
{
    UiFont font;
    makeTestFont(font);

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position = {100.0f, 50.0f};
    label.size     = {200.0f, 60.0f};   // width != height on purpose
    label.font     = &font;
    label.fontSize = kBakeSize;
    label.text     = "ABC";

    // Line width: 21 + (-4+13) + (-6+27) = 51.
    UiDrawData izquierda;
    label.align = UiTextAlign::Left;
    canvas.buildDrawData(kW, kH, izquierda);

    UiDrawData centro;
    label.align = UiTextAlign::Center;
    label.markDirty(UiElement::DirtyAll);
    canvas.buildDrawData(kW, kH, centro);

    UiDrawData derecha;
    label.align = UiTextAlign::Right;
    label.markDirty(UiElement::DirtyAll);
    canvas.buildDrawData(kW, kH, derecha);

    CHECK(izquierda.vertices.size() == 12);
    CHECK(centro.vertices.size() == 12);
    CHECK(derecha.vertices.size() == 12);
    if (izquierda.vertices.size() != 12 || centro.vertices.size() != 12 || derecha.vertices.size() != 12) return;

    CHECK(nearly(izquierda.vertices[0].pos.x, 103.0f));            // 100 + bearing 3
    CHECK(nearly(centro.vertices[0].pos.x,    177.5f));            // 100 + (200-51)/2 + 3
    CHECK(nearly(derecha.vertices[0].pos.x,   252.0f));            // 100 + 200-51 + 3

    // Y is the same in all three: alignment is ONLY in X.
    CHECK(nearly(centro.vertices[0].pos.y, izquierda.vertices[0].pos.y));
    CHECK(nearly(derecha.vertices[0].pos.y, izquierda.vertices[0].pos.y));

    // And the last glyph of the right line ends at the edge: 252 + ...
    CHECK(nearly(derecha.vertices[8].pos.x, 278.0f));              // 249 + 21+9-6 + 5
}

// The other half of alignment: where the TEXT BLOCK falls vertically on the
// rect. This is why a button with align=Center didn't look centered — the text
// came out at the top edge because this didn't exist.
//
// The Y of a specific glyph is checked, not "that it changes": a displacement
// of the block in the wrong direction, or of half what it should, would pass
// the same loose CHECK. And it's verified that X does NOT move, which is
// what separates this from horizontal alignment.
static void test_texto_alineacion_vertical()
{
    UiFont font;
    makeTestFont(font);

    UiCanvas canvas;
    Text& label = canvas.root().add<Text>("Etiqueta");
    label.position = {100.0f, 50.0f};
    label.size     = {200.0f, 60.0f};
    label.font     = &font;
    label.fontSize = kBakeSize;
    label.text     = "ABC";   // a single line

    UiDrawData arriba;
    label.vAlign = UiTextVAlign::Top;
    canvas.buildDrawData(kW, kH, arriba);

    UiDrawData medio;
    label.vAlign = UiTextVAlign::Middle;
    label.markDirty(UiElement::DirtyAll);
    canvas.buildDrawData(kW, kH, medio);

    UiDrawData abajo;
    label.vAlign = UiTextVAlign::Bottom;
    label.markDirty(UiElement::DirtyAll);
    canvas.buildDrawData(kW, kH, abajo);

    CHECK(arriba.vertices.size() == 12);
    CHECK(medio.vertices.size() == 12);
    CHECK(abajo.vertices.size() == 12);
    if (arriba.vertices.size() != 12 || medio.vertices.size() != 12 ||
        abajo.vertices.size() != 12)
        return;

    // The gap is the rect height minus ONE line of line height.
    const float sobra = 60.0f - font.lineHeight() * (kBakeSize / font.bakeSize());
    CHECK(sobra > 0.0f);

    CHECK(nearly(medio.vertices[0].pos.y, arriba.vertices[0].pos.y + sobra * 0.5f));
    CHECK(nearly(abajo.vertices[0].pos.y, arriba.vertices[0].pos.y + sobra));

    // Aligning at the top is ONLY in Y: X is untouched.
    CHECK(nearly(medio.vertices[0].pos.x, arriba.vertices[0].pos.x));
    CHECK(nearly(abajo.vertices[0].pos.x, arriba.vertices[0].pos.x));
}

// Justify distributes the leftover among spaces, and NEVER on the last line
// or one cut by '\n'.
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

    // Two lines: "A B" (21+7+13 = 41) and "C". Space leaves no quad.
    CHECK(data.vertices.size() == 12);
    if (data.vertices.size() != 12) return;

    // Leftover 45-41 = 4 for ONE space: B goes 4 px to the right.
    CHECK(nearly(data.vertices[0].pos.x, 103.0f));
    CHECK(nearly(data.vertices[4].pos.x, 130.0f));   // 100 + 21 + (7+4) - 2

    // The last line is NOT justified: C starts at the left edge.
    CHECK(nearly(data.vertices[8].pos.x, 105.0f));   // 100 + bearing 5
    CHECK(nearly(data.vertices[8].pos.y, 105.0f));   // baseline 74 + 40 - bearingY 9

    // With Left, the same space is worth 7 and B stays at 126.
    label.align = UiTextAlign::Left;
    label.markDirty(UiElement::DirtyAll);
    UiDrawData izquierda;
    canvas.buildDrawData(kW, kH, izquierda);
    CHECK(izquierda.vertices.size() == 12);
    if (izquierda.vertices.size() != 12) return;
    CHECK(nearly(izquierda.vertices[4].pos.x, 126.0f));

    // A line cut by '\n' is also NOT justified, even if there's room.
    label.align    = UiTextAlign::Justify;
    label.wordWrap = false;
    label.size     = {200.0f, 120.0f};
    label.text     = "A B\nC";
    label.markDirty(UiElement::DirtyAll);
    UiDrawData conSalto;
    canvas.buildDrawData(kW, kH, conSalto);
    CHECK(conSalto.vertices.size() == 12);
    if (conSalto.vertices.size() != 12) return;
    CHECK(nearly(conSalto.vertices[4].pos.x, 126.0f));
    CHECK(nearly(conSalto.vertices[8].pos.y, 105.0f));   // and C on the second line

    // And with spaces on BOTH lines: neither the '\n' one nor the last stretch,
    // even though both have 159 px left. Without this pair, removing the guard
    // of "last line" would go unnoticed: the last usually has no spaces.
    label.text = "A B\nA B";
    label.markDirty(UiElement::DirtyAll);
    UiDrawData dosLineas;
    canvas.buildDrawData(kW, kH, dosLineas);
    CHECK(dosLineas.vertices.size() == 16);
    if (dosLineas.vertices.size() != 16) return;
    CHECK(nearly(dosLineas.vertices[4].pos.x, 126.0f));    // B of the '\n' line
    CHECK(nearly(dosLineas.vertices[12].pos.x, 126.0f));   // B of the LAST line
    CHECK(nearly(dosLineas.vertices[12].pos.y, 97.0f));    // 114 - 17
}

// Wrap by words, and by glyph when a word doesn't fit alone.
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

    // "A B" above (same baseline) and "C" below, one lineHeight more.
    CHECK(nearly(data.vertices[0].pos.y, 62.0f));    // 74 - 12
    CHECK(nearly(data.vertices[4].pos.y, 57.0f));    // 74 - 17
    CHECK(nearly(data.vertices[8].pos.y, 105.0f));   // 114 - 9
    CHECK(nearly(data.vertices[8].pos.x, 105.0f));

    // Without wrap, all three go in a line.
    label.wordWrap = false;
    label.markDirty(UiElement::DirtyAll);
    UiDrawData seguido;
    canvas.buildDrawData(kW, kH, seguido);
    CHECK(seguido.vertices.size() == 12);
    if (seguido.vertices.size() != 12) return;
    CHECK(nearly(seguido.vertices[8].pos.y, 65.0f));   // 74 - 9, the same line

    // A word wider than the rect breaks by glyph: "ABC" is 51 against
    // a rect of 30, so A and B fit (30) and C goes down.
    label.wordWrap = true;
    label.size     = {30.0f, 120.0f};
    label.text     = "ABC";
    label.markDirty(UiElement::DirtyAll);
    UiDrawData partida;
    canvas.buildDrawData(kW, kH, partida);
    CHECK(partida.vertices.size() == 12);
    if (partida.vertices.size() != 12) return;
    CHECK(nearly(partida.vertices[0].pos.x, 103.0f));
    CHECK(nearly(partida.vertices[4].pos.x, 115.0f));
    CHECK(nearly(partida.vertices[4].pos.y, 57.0f));
    // C opens a line: no inherited kerning and at the left of the rect.
    CHECK(nearly(partida.vertices[8].pos.x, 105.0f));
    CHECK(nearly(partida.vertices[8].pos.y, 105.0f));
}

// Ellipsis clips and ends; Clip clips with scissor; Overflow does nothing.
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

    // Overflow: no clipping or dots, and scissor is still the screen's.
    label.overflow = UiTextOverflow::Overflow;
    UiDrawData libre;
    canvas.buildDrawData(kW, kH, libre);
    CHECK(libre.vertices.size() == 12);
    CHECK(libre.batches.size() == 1);
    if (libre.batches.empty()) return;
    CHECK(libre.batches[0].scissor.width == kW && libre.batches[0].scissor.height == kH);

    // Clip: the SAME quads, but the batch comes out clipped to the rect.
    label.overflow = UiTextOverflow::Clip;
    label.markDirty(UiElement::DirtyAll);
    UiDrawData recortado;
    canvas.buildDrawData(kW, kH, recortado);
    CHECK(recortado.vertices.size() == 12);
    CHECK(recortado.batches.size() == 1);
    if (recortado.batches.empty()) return;
    CHECK(recortado.batches[0].scissor.x == 100 && recortado.batches[0].scissor.y == 50);
    CHECK(recortado.batches[0].scissor.width == 40 && recortado.batches[0].scissor.height == 60);
    for (size_t i = 0; i < recortado.vertices.size(); ++i)
        CHECK(nearly(recortado.vertices[i].pos.x, libre.vertices[i].pos.x));

    // Ellipsis: 51 + 12 doesn't fit in 40, so C and B drop and "A…" remains.
    label.overflow = UiTextOverflow::Ellipsis;
    label.markDirty(UiElement::DirtyAll);
    UiDrawData cortado;
    canvas.buildDrawData(kW, kH, cortado);
    CHECK(cortado.vertices.size() == 8);
    if (cortado.vertices.size() != 8) return;

    CHECK(nearly(cortado.vertices[0].pos.x, 103.0f));
    CHECK(nearly(cortado.vertices[4].pos.x, 122.0f));           // pen 121 + bearing 1
    CHECK(nearly(cortado.vertices[4].uv.x, 90.0f / 128.0f));    // and they're the '…' UVs
    // And the block doesn't go outside rect: 122 + 10 <= 140.
    CHECK(cortado.vertices[5].pos.x <= 140.0f);

    // Without '…' in atlas it falls back to three dots.
    UiFont conPuntos;
    makeTestFont(conPuntos);
    UiGlyph punto{};
    punto.rect     = {100.0f, 50.0f, 3.0f, 3.0f};
    punto.bearingX = 1.0f;
    punto.bearingY = 3.0f;
    punto.advance  = 5.0f;
    conPuntos.addGlyph('.', punto);

    label.font = &conPuntos;
    label.markDirty(UiElement::DirtyAll);
    UiDrawData fallback;
    canvas.buildDrawData(kW, kH, fallback);
    CHECK(fallback.vertices.size() == 16);   // A + three dots
    if (fallback.vertices.size() != 16) return;
    CHECK(nearly(fallback.vertices[4].pos.x, 122.0f));
    CHECK(nearly(fallback.vertices[8].pos.x, 127.0f));
    CHECK(nearly(fallback.vertices[12].pos.x, 132.0f));
}

// The measured text block feeds the fitter, and from there the parent layout.
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

    // Panel quad first, glyphs behind.
    CHECK(data.vertices.size() == 4 + 12);
    if (data.vertices.size() != 16) return;

    // Width = the line (51) and height = one lineHeight (40): neither goes
    // outside panel size, which is {0,0}.
    CHECK(nearly(data.vertices[0].pos.x, 60.0f));
    CHECK(nearly(data.vertices[0].pos.y, 30.0f));
    CHECK(nearly(data.vertices[2].pos.x, 111.0f));
    CHECK(nearly(data.vertices[2].pos.y, 70.0f));

    // Longer text pushes the panel, which is what the fitter is for.
    label.text = "ABCABC";
    label.markDirty(UiElement::DirtyAll);
    UiDrawData largo;
    canvas.buildDrawData(kW, kH, largo);
    CHECK(largo.vertices.size() == 4 + 24);
    if (largo.vertices.size() != 28) return;
    CHECK(largo.vertices[2].pos.x > data.vertices[2].pos.x);
    CHECK(nearly(largo.vertices[2].pos.y, 70.0f));   // it's still ONE line
}

// Neutrality: with default values (Left, no wrap, Overflow) the text
// plain gives EXACTLY the same vertices and batches as the previous phase.
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

    // The first three quads are still the shadow, no outline or fattening.
    for (size_t i = 0; i < 12; ++i)
        CHECK(nearly(porDefecto.vertices[i].params.z, 0.0f));
    for (size_t i = 12; i < 24; ++i)
        CHECK(nearly(porDefecto.vertices[i].params.z, 2.5f));
}

// ── Images: sources and draw modes ─────────────────────────────────────
// The atlas for all these tests: 200x100 with a sub-rect that does NOT start
// at the origin and with different proportions per axis.
//   u: 50/200 = 0.25 -> 75/200 = 0.375   (du = 0.125)
//   v: 10/100 = 0.10 -> 50/100 = 0.500   (dv = 0.400)
// The sprite is 25x40 in atlas pixels: that's its NATIVE size and it's
// different from all elements' rect, so a mode that forgets it and uses the
// rect gives different numbers.
static UiTextureAtlas makeAtlas()
{
    UiTextureAtlas atlas;
    atlas.setSize(200, 100);
    atlas.addSprite("botella", {50.0f, 10.0f, 25.0f, 40.0f});
    return atlas;
}

// EXACT comparison, field by field including params and effect: neutrality
// is not "similar", it's the same buffer.
static bool sameVertices(const UiDrawData& a, const UiDrawData& b)
{
    if (a.vertices.size() != b.vertices.size()) return false;
    if (a.indices.size()  != b.indices.size())  return false;
    if (a.batches.size()  != b.batches.size())  return false;
    if (a.vertices.empty()) return true;
    return std::memcmp(a.vertices.data(), b.vertices.data(),
                       a.vertices.size() * sizeof(UiVertex)) == 0;
}

// A loose texture is an atlas with NO entries: the name doesn't resolve and
// UVs are 0..1. One quad, like any drawable.
static void test_imagen_textura_suelta_uv_0_1()
{
    UiTextureAtlas textura;
    textura.setSize(128, 64);   // width != height, and no addSprite at all

    UiCanvas canvas;
    Image& img = canvas.root().add<Image>("Fondo");
    img.position = {17.0f, 23.0f};
    img.size     = {90.0f, 37.0f};   // rect different from texture size
    img.atlas    = &textura;
    img.sprite   = "no_registrado";

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;

    CHECK(nearly(data.vertices[0].uv.x, 0.0f) && nearly(data.vertices[0].uv.y, 0.0f));
    CHECK(nearly(data.vertices[2].uv.x, 1.0f) && nearly(data.vertices[2].uv.y, 1.0f));
    // And the rect is still the element's, not the texture's.
    CHECK(nearly(data.vertices[2].pos.x, 107.0f));
    CHECK(nearly(data.vertices[2].pos.y, 60.0f));
}

// A sprite with a name in an atlas uses ITS sub-rect's UVs, not the
// whole atlas: with 0..1 all four values would be different.
static void test_imagen_sprite_con_nombre_usa_su_subrect()
{
    UiTextureAtlas atlas = makeAtlas();

    UiCanvas canvas;
    Image& img = canvas.root().add<Image>("Botella");
    img.position = {31.0f, 12.0f};
    img.size     = {70.0f, 44.0f};   // neither 25x40 nor square
    img.atlas    = &atlas;
    img.sprite   = "botella";

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;

    CHECK(nearly(data.vertices[0].uv.x, 0.25f)  && nearly(data.vertices[0].uv.y, 0.10f));
    CHECK(nearly(data.vertices[2].uv.x, 0.375f) && nearly(data.vertices[2].uv.y, 0.50f));
    // The rect is the element's: the sprite stretches, which is what Normal does.
    CHECK(nearly(data.vertices[2].pos.x, 101.0f));
    CHECK(nearly(data.vertices[2].pos.y, 56.0f));
}

// Two Image of the same atlas go in ONE batch EVEN THOUGH they're in different
// modes and emit many quads: the mode is resolved in CPU and is not draw
// state. With another atlas, two batches.
static void test_imagen_modos_no_parten_el_lote()
{
    UiTextureAtlas atlas = makeAtlas();

    UiCanvas canvas;

    Image& a = canvas.root().add<Image>("Tapiz");
    a.position = {10.0f, 10.0f};
    a.size     = {50.0f, 40.0f};        // 2 columns x 1 row of 25x40
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

    // 2 quads from tiled + 9 from sliced = 11.
    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 44);
    if (data.batches.size() != 1) return;
    CHECK(data.batches[0].indexCount == 66);

    // Same tree but the second in another atlas: two batches.
    UiTextureAtlas otro;
    otro.setSize(64, 32);
    otro.addSprite("llave", {8.0f, 4.0f, 16.0f, 8.0f});
    b.atlas  = &otro;
    b.sprite = "llave";
    b.markDirty(UiElement::DirtyAll);

    UiDrawData split;
    canvas.buildDrawData(kW, kH, split);

    CHECK(split.batches.size() == 2);
    if (split.batches.size() != 2) return;
    CHECK(split.batches[0].atlas == &atlas);
    CHECK(split.batches[1].atlas == &otro);
    CHECK(split.batches[0].indexCount == 12);   // the 2 tiles
}

// Tiled: the sprite repeats at its NATIVE size (25x40) and the last row and
// last column are CLIPPED by UV, not scaled.
// rect 60x90 -> ceil(60/25) = 3 columns (25, 25, 10) and ceil(90/40) = 3 rows
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

    // First tile: full native size and COMPLETE sprite UVs.
    CHECK(nearly(data.vertices[0].pos.x, 17.0f) && nearly(data.vertices[0].pos.y, 23.0f));
    CHECK(nearly(data.vertices[2].pos.x, 42.0f) && nearly(data.vertices[2].pos.y, 63.0f));
    CHECK(nearly(data.vertices[0].uv.x, 0.25f)  && nearly(data.vertices[0].uv.y, 0.10f));
    CHECK(nearly(data.vertices[2].uv.x, 0.375f) && nearly(data.vertices[2].uv.y, 0.50f));

    // Middle tile of first row: also complete, offset 25 px.
    CHECK(nearly(data.vertices[4].pos.x, 42.0f));
    CHECK(nearly(data.vertices[6].pos.x, 67.0f));
    CHECK(nearly(data.vertices[6].uv.x, 0.375f));

    // Last column (index 2): 10 px wide, and U clipped to 10/25 = 0.4
    // of the sub-rect -> 0.25 + 0.125*0.4 = 0.30. Scaling instead of clipping
    // would leave 0.375 here.
    CHECK(nearly(data.vertices[8].pos.x, 67.0f));
    CHECK(nearly(data.vertices[10].pos.x, 77.0f));
    CHECK(nearly(data.vertices[10].uv.x, 0.30f));
    CHECK(nearly(data.vertices[10].uv.y, 0.50f));   // row 0 is not clipped in V

    // Last row (index 6): 10 px high, V clipped to 10/40 = 0.25 ->
    // 0.1 + 0.4*0.25 = 0.20, and full U because it's the first column.
    CHECK(nearly(data.vertices[24].pos.y, 103.0f));
    CHECK(nearly(data.vertices[26].pos.y, 113.0f));
    CHECK(nearly(data.vertices[26].uv.y, 0.20f));
    CHECK(nearly(data.vertices[26].uv.x, 0.375f));

    // The corner (index 8) is clipped on BOTH axes.
    CHECK(nearly(data.vertices[34].pos.x, 77.0f) && nearly(data.vertices[34].pos.y, 113.0f));
    CHECK(nearly(data.vertices[34].uv.x, 0.30f) && nearly(data.vertices[34].uv.y, 0.20f));
}

// Past the quad limit Image falls to Normal: a large rect with a tiny sprite
// can't carry the buffer ahead.
static void test_imagen_tiled_tope_cae_a_normal()
{
    UiTextureAtlas atlas = makeAtlas();

    UiCanvas canvas;
    Image& img = canvas.root().add<Image>("Tapiz");
    img.position = {17.0f, 23.0f};
    img.size     = {60.0f, 90.0f};   // would ask for 3x3 = 9 tiles
    img.atlas    = &atlas;
    img.sprite   = "botella";
    img.mode     = UiImageMode::Tiled;
    img.maxTiles = 4;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    // A single quad stretched to the entire rect, with the full sub-rect.
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;
    CHECK(nearly(data.vertices[2].pos.x, 77.0f) && nearly(data.vertices[2].pos.y, 113.0f));
    CHECK(nearly(data.vertices[2].uv.x, 0.375f) && nearly(data.vertices[2].uv.y, 0.50f));
}

// Sliced: 9 quads. Four DIFFERENT borders (4/6/3/9 px of sprite) so
// swapping any two fails.
// rect 100x70 at (17,23):
//   columns x = 17 / 21 / 111  with widths 4 / 90 / 6
//   rows    y = 23 / 26 / 84   with heights  3 / 58 / 9
//   u = 0.25 / 0.27 / 0.345 / 0.375   (4/25 and 6/25 of sub-rect)
//   v = 0.10 / 0.13 / 0.410 / 0.500   (3/40 and 9/40)
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

    // Top left corner: 4x3, its NATIVE size. Stretching would give 100x70.
    CHECK(nearly(data.vertices[0].pos.x, 17.0f) && nearly(data.vertices[0].pos.y, 23.0f));
    CHECK(nearly(data.vertices[2].pos.x, 21.0f) && nearly(data.vertices[2].pos.y, 26.0f));
    CHECK(nearly(data.vertices[0].uv.x, 0.25f)  && nearly(data.vertices[0].uv.y, 0.10f));
    CHECK(nearly(data.vertices[2].uv.x, 0.27f)  && nearly(data.vertices[2].uv.y, 0.13f));

    // Top right corner: 6x3 at the right edge.
    CHECK(nearly(data.vertices[8].pos.x, 111.0f)  && nearly(data.vertices[8].pos.y, 23.0f));
    CHECK(nearly(data.vertices[10].pos.x, 117.0f) && nearly(data.vertices[10].pos.y, 26.0f));
    CHECK(nearly(data.vertices[8].uv.x, 0.345f)   && nearly(data.vertices[10].uv.x, 0.375f));

    // Bottom left corner: 4x9.
    CHECK(nearly(data.vertices[24].pos.x, 17.0f) && nearly(data.vertices[24].pos.y, 84.0f));
    CHECK(nearly(data.vertices[26].pos.x, 21.0f) && nearly(data.vertices[26].pos.y, 93.0f));
    CHECK(nearly(data.vertices[24].uv.y, 0.41f)  && nearly(data.vertices[26].uv.y, 0.50f));

    // Bottom right corner: 6x9, the only one touching both UV corners.
    CHECK(nearly(data.vertices[32].pos.x, 111.0f) && nearly(data.vertices[32].pos.y, 84.0f));
    CHECK(nearly(data.vertices[34].pos.x, 117.0f) && nearly(data.vertices[34].pos.y, 93.0f));
    CHECK(nearly(data.vertices[34].uv.x, 0.375f)  && nearly(data.vertices[34].uv.y, 0.50f));

    // Top edge: stretched only in X (90 px), with the edge's native height.
    CHECK(nearly(data.vertices[4].pos.x, 21.0f)  && nearly(data.vertices[4].pos.y, 23.0f));
    CHECK(nearly(data.vertices[6].pos.x, 111.0f) && nearly(data.vertices[6].pos.y, 26.0f));
    CHECK(nearly(data.vertices[4].uv.x, 0.27f)   && nearly(data.vertices[6].uv.x, 0.345f));

    // Left edge: stretched only in Y (58 px), with native width.
    CHECK(nearly(data.vertices[12].pos.x, 17.0f) && nearly(data.vertices[12].pos.y, 26.0f));
    CHECK(nearly(data.vertices[14].pos.x, 21.0f) && nearly(data.vertices[14].pos.y, 84.0f));

    // Center: covers the entire gap, 90x58, with the interior sub-rect.
    CHECK(nearly(data.vertices[16].pos.x, 21.0f)  && nearly(data.vertices[16].pos.y, 26.0f));
    CHECK(nearly(data.vertices[18].pos.x, 111.0f) && nearly(data.vertices[18].pos.y, 84.0f));
    CHECK(nearly(data.vertices[16].uv.x, 0.27f)   && nearly(data.vertices[16].uv.y, 0.13f));
    CHECK(nearly(data.vertices[18].uv.x, 0.345f)  && nearly(data.vertices[18].uv.y, 0.41f));

    // Without center: 8 quads, and the fifth becomes the right edge.
    img.fillCenter = false;
    img.markDirty(UiElement::DirtyAll);
    UiDrawData sinCentro;
    canvas.buildDrawData(kW, kH, sinCentro);

    CHECK(sinCentro.batches.size() == 1);
    CHECK(sinCentro.vertices.size() == 32);
    if (sinCentro.vertices.size() != 32) return;
    CHECK(nearly(sinCentro.vertices[16].pos.x, 111.0f));
    CHECK(nearly(sinCentro.vertices[16].pos.y, 26.0f));
}

// A rect narrower than the sum of borders: the two on the axis scale
// proportionally (4 and 6 over 8 -> 3.2 and 4.8) instead of overlapping, and
// the center column vanishes because it measures 0.
static void test_imagen_sliced_bordes_mayores_que_el_rect()
{
    UiTextureAtlas atlas = makeAtlas();

    UiCanvas canvas;
    Image& img = canvas.root().add<Image>("Marco");
    img.position     = {17.0f, 23.0f};
    img.size         = {8.0f, 70.0f};   // 8 < 4+6, but 70 > 3+9
    img.atlas        = &atlas;
    img.sprite       = "botella";
    img.mode         = UiImageMode::Sliced;
    img.borderLeft   = 4.0f;
    img.borderRight  = 6.0f;
    img.borderTop    = 3.0f;
    img.borderBottom = 9.0f;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    // 3 rows x 2 columns: the center one measures 0 and is not emitted.
    CHECK(data.vertices.size() == 24);
    if (data.vertices.size() != 24) return;

    // Left: 8 * 4/10 = 3.2 -> ends at 20.2.
    CHECK(nearly(data.vertices[0].pos.x, 17.0f));
    CHECK(nearly(data.vertices[2].pos.x, 20.2f));
    // Right: 8 * 6/10 = 4.8 -> starts EXACTLY where the left ends.
    CHECK(nearly(data.vertices[4].pos.x, 20.2f));
    CHECK(nearly(data.vertices[6].pos.x, 25.0f));
    CHECK(data.vertices[4].pos.x >= data.vertices[2].pos.x);   // no overlap
    // The Y axis does fit: the borders there don't touch.
    CHECK(nearly(data.vertices[2].pos.y, 26.0f));
    // And UVs are NOT rescaled: they're still the sprite's border ones.
    CHECK(nearly(data.vertices[2].uv.x, 0.27f));
    CHECK(nearly(data.vertices[4].uv.x, 0.345f));
}

// Filled: clips both position and UV. With full UV the visible piece
// would show the compressed sprite instead of its quarter.
// rect 80x44 at (17,23), fillAmount 0.25.
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

    // Horizontal from start: 80*0.25 = 20 px and u1 = 0.25 + 0.125*0.25.
    UiDrawData hStart;
    canvas.buildDrawData(kW, kH, hStart);
    CHECK(hStart.vertices.size() == 4);
    if (hStart.vertices.size() != 4) return;
    CHECK(nearly(hStart.vertices[0].pos.x, 17.0f));
    CHECK(nearly(hStart.vertices[2].pos.x, 37.0f));
    CHECK(nearly(hStart.vertices[2].pos.y, 67.0f));          // doesn't touch Y
    CHECK(nearly(hStart.vertices[0].uv.x, 0.25f));
    CHECK(nearly(hStart.vertices[2].uv.x, 0.28125f));
    CHECK(nearly(hStart.vertices[2].uv.y, 0.50f));

    // Opposite origin: clips from the OTHER side, in position and UV.
    img.fillOrigin = UiFillOrigin::End;
    img.markDirty(UiElement::DirtyAll);
    UiDrawData hEnd;
    canvas.buildDrawData(kW, kH, hEnd);
    CHECK(hEnd.vertices.size() == 4);
    if (hEnd.vertices.size() != 4) return;
    CHECK(nearly(hEnd.vertices[0].pos.x, 77.0f));
    CHECK(nearly(hEnd.vertices[2].pos.x, 97.0f));
    CHECK(nearly(hEnd.vertices[0].uv.x, 0.34375f));
    CHECK(nearly(hEnd.vertices[2].uv.x, 0.375f));

    // Vertical from top: 44*0.25 = 11 px and v1 = 0.1 + 0.4*0.25 = 0.2.
    img.fillDirection = UiFillDirection::Vertical;
    img.fillOrigin    = UiFillOrigin::Start;
    img.markDirty(UiElement::DirtyAll);
    UiDrawData vStart;
    canvas.buildDrawData(kW, kH, vStart);
    CHECK(vStart.vertices.size() == 4);
    if (vStart.vertices.size() != 4) return;
    CHECK(nearly(vStart.vertices[0].pos.y, 23.0f));
    CHECK(nearly(vStart.vertices[2].pos.y, 34.0f));
    CHECK(nearly(vStart.vertices[2].pos.x, 97.0f));          // doesn't touch X
    CHECK(nearly(vStart.vertices[2].uv.y, 0.20f));

    // Vertical from bottom.
    img.fillOrigin = UiFillOrigin::End;
    img.markDirty(UiElement::DirtyAll);
    UiDrawData vEnd;
    canvas.buildDrawData(kW, kH, vEnd);
    CHECK(vEnd.vertices.size() == 4);
    if (vEnd.vertices.size() != 4) return;
    CHECK(nearly(vEnd.vertices[0].pos.y, 56.0f));
    CHECK(nearly(vEnd.vertices[2].pos.y, 67.0f));
    CHECK(nearly(vEnd.vertices[0].uv.y, 0.40f));
    CHECK(nearly(vEnd.vertices[2].uv.y, 0.50f));

    // At 0 nothing is emitted: no quad, no batch.
    img.fillAmount = 0.0f;
    img.markDirty(UiElement::DirtyAll);
    UiDrawData vacio;
    canvas.buildDrawData(kW, kH, vacio);
    CHECK(vacio.vertices.empty());
    CHECK(vacio.batches.empty());
}

// At 1 Filled has to give EXACTLY the same as Normal, byte for byte.
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

// Neutrality: an Image in Normal gives the SAME vertices as the drawable
// of yore. If the new fields touched anything, this memcmp catches it.
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

    // And touching the fields of the OTHER modes without changing mode also
    // moves nothing: a Normal doesn't look at borders or fillAmount.
    img.borderLeft = 4.0f;
    img.borderTop  = 3.0f;
    img.fillAmount = 0.25f;
    img.maxTiles   = 2;

    UiDrawData dc;
    conImage.buildDrawData(kW, kH, dc);
    CHECK(sameVertices(dc, db));
}

// ── Events ─────────────────────────────────────────────────────────────────
// This is all pure CPU: no Vulkan, no window, no clock. Time comes through
// UiInputState, so the event sequence is the same on each run.

static const uint32_t kEvW = 800;
static const uint32_t kEvH = 600;

// Input REUSES the rects from the last buildDrawData: without placing the tree
// there's nothing to hit, so each event test starts here.
static void colocar(UiCanvas& canvas)
{
    UiDrawData basura;
    canvas.buildDrawData(kEvW, kEvH, basura);
}

// One frame of still mouse, no buttons or keys.
static UiInputState raton(float x, float y, float t)
{
    UiInputState in;
    in.mousePos    = glm::vec2(x, y);
    in.timeSeconds = t;
    return in;
}

// Two OVERLAPPING elements: the one drawn last wins, which is the bottom one
// in the child list and thus the top one on screen.
static void test_eventos_hit_test_gana_el_de_arriba()
{
    UiCanvas canvas;

    UiElement& abajo = canvas.root().add("abajo");
    abajo.position = glm::vec2(40.0f, 30.0f);
    abajo.size     = glm::vec2(200.0f, 80.0f);      // [40,240) x [30,110)

    UiElement& arriba = canvas.root().add("arriba");
    arriba.position = glm::vec2(100.0f, 50.0f);
    arriba.size     = glm::vec2(140.0f, 60.0f);     // [100,240) x [50,110)

    colocar(canvas);

    // Shared zone: the last drawn one wins.
    CHECK(canvas.hitTest(glm::vec2(150.0f, 70.0f)) == &arriba);
    // Only the bottom one.
    CHECK(canvas.hitTest(glm::vec2(50.0f, 40.0f)) == &abajo);
    // Outside both: the root does NOT intercept.
    CHECK(canvas.hitTest(glm::vec2(700.0f, 500.0f)) == nullptr);

    // raycastTarget turns off the element but NOT its children.
    arriba.raycastTarget = false;
    CHECK(canvas.hitTest(glm::vec2(150.0f, 70.0f)) == &abajo);

    // A rect of size 0 receives nothing.
    UiElement& vacio = canvas.root().add("vacio");
    vacio.position = glm::vec2(400.0f, 400.0f);
    vacio.size     = glm::vec2(0.0f, 0.0f);
    colocar(canvas);
    CHECK(canvas.hitTest(glm::vec2(400.0f, 400.0f)) == nullptr);
}

// The parent's scissor clips the child's hit test, not just its drawing.
static void test_eventos_clip_recorta_el_hit_test()
{
    UiCanvas canvas;

    UiElement& padre = canvas.root().add("padre");
    padre.position     = glm::vec2(300.0f, 100.0f);
    padre.size         = glm::vec2(100.0f, 60.0f);   // [300,400) x [100,160)
    padre.clipChildren = true;

    UiElement& hijo = padre.add("hijo");
    hijo.size = glm::vec2(300.0f, 40.0f);            // [300,600) x [100,140), se sale

    colocar(canvas);

    // Inside the clipping: reaches the child.
    CHECK(canvas.hitTest(glm::vec2(350.0f, 120.0f)) == &hijo);
    // Inside the child's RECT but outside the parent's clip: nobody.
    CHECK(canvas.hitTest(glm::vec2(450.0f, 120.0f)) == nullptr);
    // And the parent still receives on its part that the child doesn't cover.
    CHECK(canvas.hitTest(glm::vec2(350.0f, 150.0f)) == &padre);
}

// Enter and Exit are DERIVED from each frame's hit, and hovered is the state.
static void test_eventos_enter_exit_y_hovered()
{
    UiCanvas canvas;

    UiElement& caja = canvas.root().add("caja");
    caja.position = glm::vec2(120.0f, 90.0f);
    caja.size     = glm::vec2(160.0f, 70.0f);        // [120,280) x [90,160)

    colocar(canvas);

    int entradas = 0, salidas = 0, movimientos = 0;
    caja.onMouseEnter = [&](UiEvent&) { ++entradas; };
    caja.onMouseExit  = [&](UiEvent&) { ++salidas; };
    caja.onMouseMove  = [&](UiEvent&) { ++movimientos; };

    canvas.updateInput(raton(10.0f, 10.0f, 0.0f));          // outside
    CHECK(entradas == 0);
    CHECK(salidas == 0);
    CHECK(caja.hovered == false);
    CHECK(canvas.hovered() == nullptr);

    canvas.updateInput(raton(130.0f, 100.0f, 0.1f));        // enters
    CHECK(entradas == 1);
    CHECK(caja.hovered == true);
    CHECK(canvas.hovered() == &caja);
    CHECK(movimientos == 1);

    canvas.updateInput(raton(200.0f, 140.0f, 0.2f));        // moves INSIDE
    CHECK(entradas == 1);                                   // doesn't repeat
    CHECK(salidas == 0);
    CHECK(caja.hovered == true);
    CHECK(movimientos == 2);

    canvas.updateInput(raton(600.0f, 400.0f, 0.3f));        // sale
    CHECK(salidas == 1);
    CHECK(entradas == 1);
    CHECK(caja.hovered == false);
    CHECK(canvas.hovered() == nullptr);
}

// Click = Down and Up on the SAME element.
static void test_eventos_click_pide_el_mismo_elemento()
{
    UiCanvas canvas;
    canvas.dragThreshold = 500.0f;   // here we do not want anything to be a drag

    UiElement& a = canvas.root().add("a");
    a.position = glm::vec2(40.0f, 30.0f);
    a.size     = glm::vec2(120.0f, 50.0f);           // [40,160) x [30,80)

    UiElement& b = canvas.root().add("b");
    b.position = glm::vec2(300.0f, 200.0f);
    b.size     = glm::vec2(90.0f, 140.0f);           // [300,390) x [200,340)

    colocar(canvas);

    int clicksA = 0, clicksB = 0, abajoA = 0, arribaB = 0;
    a.onClick     = [&](UiEvent&) { ++clicksA; };
    b.onClick     = [&](UiEvent&) { ++clicksB; };
    a.onMouseDown = [&](UiEvent&) { ++abajoA; };
    b.onMouseUp   = [&](UiEvent&) { ++arribaB; };

    // Down and Up on A.
    UiInputState in = raton(60.0f, 50.0f, 0.0f);
    in.mouseDown[0] = true;
    canvas.updateInput(in);
    in.timeSeconds  = 0.02f;
    in.mouseDown[0] = false;
    canvas.updateInput(in);

    CHECK(abajoA == 1);
    CHECK(clicksA == 1);

    // Down on A, Up on B: neither one gets the click.
    in = raton(60.0f, 50.0f, 0.5f);
    in.mouseDown[0] = true;
    canvas.updateInput(in);
    in = raton(320.0f, 250.0f, 0.52f);
    in.mouseDown[0] = false;
    canvas.updateInput(in);

    CHECK(clicksA == 1);        // the earlier one stays
    CHECK(clicksB == 0);
    CHECK(arribaB == 1);        // the MouseUp does belong to the one under the cursor
}

// Canvas thresholds, and DIFFERENT from each other: 0.25 s and 10 px.
static void test_eventos_doble_click_por_tiempo_y_distancia()
{
    auto montar = [](UiCanvas& c, UiElement*& caja)
    {
        c.doubleClickTime     = 0.25f;
        c.doubleClickDistance = 10.0f;
        c.dragThreshold       = 40.0f;
        caja = &c.root().add("caja");
        caja->position = glm::vec2(100.0f, 100.0f);
        caja->size     = glm::vec2(200.0f, 120.0f);   // [100,300) x [100,220)
        colocar(c);
    };

    auto click = [](UiCanvas& c, float x, float y, float t)
    {
        UiInputState in = raton(x, y, t);
        in.mouseDown[0] = true;
        c.updateInput(in);
        in.timeSeconds  = t + 0.01f;
        in.mouseDown[0] = false;
        c.updateInput(in);
    };

    // Within both thresholds: the double comes out.
    {
        UiCanvas c;
        UiElement* caja = nullptr;
        montar(c, caja);
        int simples = 0, dobles = 0;
        caja->onClick       = [&](UiEvent&) { ++simples; };
        caja->onDoubleClick = [&](UiEvent&) { ++dobles; };

        click(c, 150.0f, 150.0f, 0.00f);
        click(c, 153.0f, 152.0f, 0.12f);   // 3.6 px, 0.12 s

        CHECK(simples == 2);               // the double does NOT replace the second click
        CHECK(dobles == 1);
    }

    // Once the time has passed: no.
    {
        UiCanvas c;
        UiElement* caja = nullptr;
        montar(c, caja);
        int dobles = 0;
        caja->onDoubleClick = [&](UiEvent&) { ++dobles; };

        click(c, 150.0f, 150.0f, 0.00f);
        click(c, 150.0f, 150.0f, 0.40f);   // 0.40 s > 0.25 s
        CHECK(dobles == 0);
    }

    // Moved beyond the distance: no either, even if it arrives in time.
    {
        UiCanvas c;
        UiElement* caja = nullptr;
        montar(c, caja);
        int dobles = 0;
        caja->onDoubleClick = [&](UiEvent&) { ++dobles; };

        click(c, 150.0f, 150.0f, 0.00f);
        click(c, 180.0f, 150.0f, 0.08f);   // 30 px > 10 px
        CHECK(dobles == 0);
    }
}

// Drag threshold different from the double click one: 12 px.
static void test_eventos_drag_umbral_y_destino_del_drop()
{
    UiCanvas canvas;
    canvas.dragThreshold       = 12.0f;
    canvas.doubleClickTime     = 0.25f;
    canvas.doubleClickDistance = 10.0f;

    UiElement& origen = canvas.root().add("origen");
    origen.position = glm::vec2(40.0f, 30.0f);
    origen.size     = glm::vec2(120.0f, 50.0f);      // [40,160) x [30,80)

    UiElement& destino = canvas.root().add("destino");
    destino.position = glm::vec2(300.0f, 200.0f);
    destino.size     = glm::vec2(90.0f, 140.0f);     // [300,390) x [200,340)

    colocar(canvas);

    int begins = 0, drags = 0, ends = 0, clicks = 0;
    int dropsOrigen = 0, dropsDestino = 0;
    UiElement* fuente = nullptr;

    origen.onDragBegin = [&](UiEvent&) { ++begins; };
    origen.onDrag      = [&](UiEvent&) { ++drags; };
    origen.onDragEnd   = [&](UiEvent&) { ++ends; };
    origen.onClick     = [&](UiEvent&) { ++clicks; };
    origen.onDrop      = [&](UiEvent&) { ++dropsOrigen; };
    destino.onDrop     = [&](UiEvent& e) { ++dropsDestino; fuente = e.dragSource; };

    // BELOW the threshold: there is no drag and there is a click.
    UiInputState in = raton(60.0f, 50.0f, 0.0f);
    in.mouseDown[0] = true;
    canvas.updateInput(in);
    in = raton(66.0f, 54.0f, 0.02f);     // 7.2 px < 12
    in.mouseDown[0] = true;
    canvas.updateInput(in);
    in.mouseDown[0] = false;
    in.timeSeconds  = 0.04f;
    canvas.updateInput(in);

    CHECK(begins == 0);
    CHECK(drags == 0);
    CHECK(clicks == 1);

    // ABOVE the threshold: a complete drag and NO click.
    in = raton(60.0f, 50.0f, 1.0f);
    in.mouseDown[0] = true;
    canvas.updateInput(in);
    in = raton(120.0f, 90.0f, 1.02f);    // well above 12 px
    in.mouseDown[0] = true;
    canvas.updateInput(in);
    in = raton(320.0f, 250.0f, 1.04f);   // already over the target
    in.mouseDown[0] = true;
    canvas.updateInput(in);
    in.mouseDown[0] = false;
    in.timeSeconds  = 1.06f;
    canvas.updateInput(in);

    CHECK(begins == 1);
    CHECK(drags == 2);          // one per frame moved with the button down
    CHECK(ends == 1);
    CHECK(clicks == 1);         // the earlier one: the drag does NOT add another
    // The Drop belongs to the element UNDER THE CURSOR on release, not to the one that started it.
    CHECK(dropsDestino == 1);
    CHECK(dropsOrigen == 0);
    CHECK(fuente == &origen);
}

// The wheel goes to the one under the cursor and goes up to the parent if the child does not consume it.
static void test_eventos_scroll_burbujea_hasta_el_padre()
{
    UiCanvas canvas;

    UiElement& lista = canvas.root().add("lista");
    lista.position = glm::vec2(200.0f, 60.0f);
    lista.size     = glm::vec2(240.0f, 180.0f);

    UiElement& fila = lista.add("fila");
    fila.size = glm::vec2(240.0f, 30.0f);

    colocar(canvas);

    int enFila = 0, enLista = 0;
    float recibido = 0.0f;
    fila.onScroll  = [&](UiEvent&)   { ++enFila; };
    lista.onScroll = [&](UiEvent& e) { ++enLista; recibido = e.scrollDelta; };

    UiInputState in = raton(250.0f, 70.0f, 0.0f);   // on top of the row
    in.scrollDelta = -3.5f;
    canvas.updateInput(in);

    CHECK(enFila == 1);
    CHECK(enLista == 1);            // it bubbled up
    CHECK(nearly(recibido, -3.5f));

    // Without a wheel nothing is emitted.
    canvas.updateInput(raton(250.0f, 70.0f, 0.1f));
    CHECK(enFila == 1);
    CHECK(enLista == 1);
}

// consumed cuts the bubbling: the parent does not find out.
static void test_eventos_consumed_corta_la_burbuja()
{
    UiCanvas canvas;
    canvas.dragThreshold = 500.0f;

    UiElement& padre = canvas.root().add("padre");
    padre.position = glm::vec2(150.0f, 120.0f);
    padre.size     = glm::vec2(260.0f, 90.0f);

    UiElement& hijo = padre.add("hijo");
    hijo.size = glm::vec2(80.0f, 40.0f);

    colocar(canvas);

    int enHijo = 0, enPadre = 0;
    bool consumir = true;
    hijo.onClick  = [&](UiEvent& e) { ++enHijo; if (consumir) e.consumed = true; };
    padre.onClick = [&](UiEvent&)   { ++enPadre; };

    auto click = [&](float t)
    {
        UiInputState in = raton(170.0f, 130.0f, t);
        in.mouseDown[0] = true;
        canvas.updateInput(in);
        in.mouseDown[0] = false;
        in.timeSeconds  = t + 0.01f;
        canvas.updateInput(in);
    };

    click(0.0f);
    CHECK(enHijo == 1);
    CHECK(enPadre == 0);        // consumed in the child

    consumir = false;
    click(1.0f);
    CHECK(enHijo == 2);
    CHECK(enPadre == 1);        // now it does go up
}

// Tab in pre-order skipping the non-focusable and the invisible; Escape releases.
static void test_eventos_foco_tab_y_escape()
{
    UiCanvas canvas;

    UiElement& a = canvas.root().add("a");
    a.position = glm::vec2(20.0f, 20.0f);
    a.size     = glm::vec2(100.0f, 40.0f);
    a.focusable = true;

    UiElement& b = canvas.root().add("b");          // NO focusable
    b.position = glm::vec2(20.0f, 80.0f);
    b.size     = glm::vec2(100.0f, 40.0f);

    UiElement& c = canvas.root().add("c");
    c.position = glm::vec2(20.0f, 140.0f);
    c.size     = glm::vec2(100.0f, 40.0f);
    c.focusable = true;

    UiElement& d = canvas.root().add("d");          // focusable but INVISIBLE
    d.position = glm::vec2(20.0f, 200.0f);
    d.size     = glm::vec2(100.0f, 40.0f);
    d.focusable = true;
    d.visible   = false;

    UiElement& e = canvas.root().add("e");
    e.position = glm::vec2(20.0f, 260.0f);
    e.size     = glm::vec2(100.0f, 40.0f);
    e.focusable = true;

    colocar(canvas);

    // Blur of the old one BEFORE Focus of the new one.
    std::vector<int> orden;
    a.onBlur  = [&](UiEvent&) { orden.push_back(1); };
    c.onFocus = [&](UiEvent&) { orden.push_back(2); };

    canvas.setFocus(&a);
    CHECK(canvas.focused() == &a);
    CHECK(a.focused == true);

    auto tab = [&](bool shift, float t)
    {
        UiInputState in = raton(700.0f, 500.0f, t);   // far from everything
        in.keys.push_back(UiKey::Tab);
        in.shift = shift;
        canvas.updateInput(in);
    };

    tab(false, 0.1f);
    CHECK(canvas.focused() == &c);          // it jumped to b, which is not focusable
    CHECK(a.focused == false);
    CHECK(orden.size() == 2);
    CHECK(orden[0] == 1);                   // Blur
    CHECK(orden[1] == 2);                   // Focus, afterwards

    tab(false, 0.2f);
    CHECK(canvas.focused() == &e);          // it jumped to d, invisible

    tab(false, 0.3f);
    CHECK(canvas.focused() == &a);          // wraps around

    tab(true, 0.4f);
    CHECK(canvas.focused() == &e);          // Shift+Tab goes the other way

    tab(true, 0.5f);
    CHECK(canvas.focused() == &c);

    // Escape releases the focus.
    UiInputState esc = raton(700.0f, 500.0f, 0.6f);
    esc.keys.push_back(UiKey::Escape);
    canvas.updateInput(esc);
    CHECK(canvas.focused() == nullptr);
    CHECK(c.focused == false);

    // An element that is not focusable cannot take it.
    canvas.setFocus(&b);
    CHECK(canvas.focused() == nullptr);
}

// The keyboard goes ONLY to the one that has the focus. Without focus not a single event is emitted.
static void test_eventos_teclado_solo_con_foco()
{
    UiCanvas canvas;

    UiElement& campo = canvas.root().add("campo");
    campo.position = glm::vec2(60.0f, 40.0f);
    campo.size     = glm::vec2(220.0f, 36.0f);
    campo.focusable = true;

    UiElement& otro = canvas.root().add("otro");
    otro.position = glm::vec2(60.0f, 120.0f);
    otro.size     = glm::vec2(220.0f, 36.0f);
    otro.focusable = true;

    colocar(canvas);

    // What is tested here is the DELIVERY of the key to the focus, not the navigation:
    // with it turned on the arrows would move the focus to the other focusable and the
    // final Escape would no longer arrive at the same place. Navigation has its
    // own test.
    canvas.keyboardNavigation = false;

    int enCampo = 0, enOtro = 0;
    std::vector<UiKey> vistas;
    campo.onKeyDown = [&](UiEvent& ev) { ++enCampo; vistas.push_back(ev.key); };
    otro.onKeyDown  = [&](UiEvent&)    { ++enOtro; };

    // Without focus: nothing.
    UiInputState in = raton(500.0f, 400.0f, 0.0f);
    in.keys.push_back(UiKey::Enter);
    in.keys.push_back(UiKey::Left);
    canvas.updateInput(in);
    CHECK(enCampo == 0);
    CHECK(enOtro == 0);

    canvas.setFocus(&campo);

    in = raton(500.0f, 400.0f, 0.1f);
    in.keys.push_back(UiKey::Enter);
    in.keys.push_back(UiKey::Left);
    in.keys.push_back(UiKey::Right);
    in.keys.push_back(UiKey::Up);
    in.keys.push_back(UiKey::Down);
    canvas.updateInput(in);

    CHECK(enCampo == 5);
    CHECK(enOtro == 0);                     // only the one with the focus
    CHECK(vistas.size() == 5);
    CHECK(vistas[0] == UiKey::Enter);
    CHECK(vistas[1] == UiKey::Left);
    CHECK(vistas[4] == UiKey::Down);

    // Escape is DELIVERED as a key and also releases the focus.
    in = raton(500.0f, 400.0f, 0.2f);
    in.keys.push_back(UiKey::Escape);
    canvas.updateInput(in);
    CHECK(enCampo == 6);
    CHECK(canvas.focused() == nullptr);

    // And now without focus nothing arrives again.
    in = raton(500.0f, 400.0f, 0.3f);
    in.keys.push_back(UiKey::Enter);
    canvas.updateInput(in);
    CHECK(enCampo == 6);
}

// Arrows and Enter: it is what makes a menu playable with a gamepad. Without this the focus
// could be moved through the API but no key moved it, and the focused element could not
// be activated in any way other than the mouse.
static void test_teclado_navega_y_activa_el_foco()
{
    UiCanvas canvas;
    UiDrawData data;

    // Two buttons in a row: the directional is resolved by geometry, so
    // real rects are needed.
    UiElement& fila = canvas.root().add<UiElement>("fila");
    fila.size = glm::vec2(600.0f, 100.0f);
    fila.drawable = false;

    Button& izq = fila.add<Button>("izq");
    izq.position = glm::vec2(0.0f, 0.0f);
    izq.size     = glm::vec2(120.0f, 40.0f);
    izq.focusable = true;

    Button& der = fila.add<Button>("der");
    der.position = glm::vec2(300.0f, 0.0f);
    der.size     = glm::vec2(120.0f, 40.0f);
    der.focusable = true;

    canvas.buildDrawData(kW, kH, data);

    int clicksIzq = 0, clicksDer = 0;
    izq.onClick = [&](UiEvent&) { ++clicksIzq; };
    der.onClick = [&](UiEvent&) { ++clicksDer; };

    canvas.setFocus(&izq);

    // Right moves the focus to the one on the right.
    UiInputState in = raton(-1.0f, -1.0f, 0.0f);
    in.keys.push_back(UiKey::Right);
    canvas.updateInput(in);
    CHECK(canvas.focused() == &der);

    // Enter ACTIVATES it: same effect as a mouse click on top.
    in = raton(-1.0f, -1.0f, 0.1f);
    in.keys.push_back(UiKey::Enter);
    canvas.updateInput(in);
    CHECK(clicksDer == 1);
    CHECK(clicksIzq == 0);

    // Left goes back.
    in = raton(-1.0f, -1.0f, 0.2f);
    in.keys.push_back(UiKey::Left);
    canvas.updateInput(in);
    CHECK(canvas.focused() == &izq);

    // A handler that CONSUMES the key keeps it: the canvas does not navigate
    // over whoever already used it.
    izq.onKeyDown = [&](UiEvent& ev) { ev.consumed = true; };
    in = raton(-1.0f, -1.0f, 0.3f);
    in.keys.push_back(UiKey::Right);
    canvas.updateInput(in);
    CHECK(canvas.focused() == &izq);
    izq.onKeyDown = nullptr;

    // A non-interactable button is not activated, same as with the mouse.
    izq.interactable = false;
    in = raton(-1.0f, -1.0f, 0.4f);
    in.keys.push_back(UiKey::Enter);
    canvas.updateInput(in);
    CHECK(clicksIzq == 0);
    izq.interactable = true;

    // And submitFocused without focus fires nothing.
    canvas.setFocus(nullptr);
    CHECK(canvas.submitFocused() == false);
    CHECK(clicksIzq == 0);

    // When turned off, the arrows stop moving the focus (they still arrive as KeyDown).
    canvas.keyboardNavigation = false;
    canvas.setFocus(&izq);
    in = raton(-1.0f, -1.0f, 0.5f);
    in.keys.push_back(UiKey::Right);
    canvas.updateInput(in);
    CHECK(canvas.focused() == &izq);
}

// Neutrality: a canvas with handlers and with processed input gives EXACTLY the
// same vertices as an identical one that never saw an event.
static void test_neutralidad_de_los_eventos()
{
    UiTextureAtlas atlas = makeAtlas();

    auto montar = [&](UiCanvas& c)
    {
        UiElement& panel = c.root().add("panel");
        panel.position     = glm::vec2(70.0f, 45.0f);
        panel.size         = glm::vec2(180.0f, 95.0f);
        panel.color        = glm::vec4(0.2f, 0.6f, 0.9f, 1.0f);
        panel.clipChildren = true;

        UiElement& hijo = panel.add("hijo");
        hijo.position = glm::vec2(10.0f, 12.0f);
        hijo.size     = glm::vec2(60.0f, 25.0f);
        hijo.atlas    = &atlas;
        hijo.sprite   = "boton";
    };

    UiCanvas conEventos;
    UiCanvas sinEventos;
    montar(conEventos);
    montar(sinEventos);

    // Handlers across the whole tree and a complete gesture: hover, click, drag,
    UiElement* panel = const_cast<UiElement*>(conEventos.root().children()[0].get());
    UiElement* hijo  = const_cast<UiElement*>(panel->children()[0].get());
    panel->focusable = true;
    int golpes = 0;
    panel->onClick     = [&](UiEvent&) { ++golpes; };
    panel->onScroll    = [&](UiEvent&) { ++golpes; };
    hijo->onMouseEnter = [&](UiEvent&) { ++golpes; };
    hijo->onDragBegin  = [&](UiEvent&) { ++golpes; };

    UiDrawData da;
    conEventos.buildDrawData(kEvW, kEvH, da);

    UiInputState in = raton(100.0f, 70.0f, 0.0f);
    in.scrollDelta = 2.0f;
    conEventos.updateInput(in);
    in.mouseDown[0] = true;
    in.scrollDelta  = 0.0f;
    in.timeSeconds  = 0.05f;
    conEventos.updateInput(in);
    in = raton(220.0f, 120.0f, 0.10f);
    in.mouseDown[0] = true;
    conEventos.updateInput(in);
    in.mouseDown[0] = false;
    in.timeSeconds  = 0.15f;
    conEventos.updateInput(in);
    in = raton(100.0f, 70.0f, 0.20f);
    in.keys.push_back(UiKey::Tab);
    conEventos.updateInput(in);

    CHECK(golpes > 0);          // the gesture did do something

    UiDrawData db;
    conEventos.buildDrawData(kEvW, kEvH, db);

    UiDrawData dc;
    sinEventos.buildDrawData(kEvW, kEvH, dc);

    CHECK(!da.vertices.empty());
    CHECK(sameVertices(da, db));    // processing input did not move a single vertex
    CHECK(sameVertices(db, dc));    // and they are the same as without events
}

// ── Buttons: 5 states and 3 transitions ─────────────────────────────────────
// Five colors that are NOT repeated: if the priority table picks the wrong row,
// the color gives away which one it chose.
static const glm::vec4 kNormal  {0.10f, 0.20f, 0.30f, 1.00f};
static const glm::vec4 kHover   {0.40f, 0.50f, 0.60f, 0.90f};
static const glm::vec4 kPressed {0.70f, 0.15f, 0.25f, 0.80f};
static const glm::vec4 kDisabled{0.05f, 0.85f, 0.45f, 0.70f};
static const glm::vec4 kSelected{0.90f, 0.35f, 0.55f, 0.60f};

// Rect with width != height: a square does not distinguish one swapped axis from the other.
static Button& montaBoton(UiCanvas& canvas, UiDrawData& data)
{
    Button& b = canvas.root().add<Button>("Aceptar");
    b.position = glm::vec2(40.0f, 30.0f);
    b.size     = glm::vec2(120.0f, 64.0f);

    b.normalColor   = kNormal;
    b.hoverColor    = kHover;
    b.pressedColor  = kPressed;
    b.disabledColor = kDisabled;
    b.selectedColor = kSelected;
    b.color         = kNormal;

    // The input reuses the rects that the emitter leaves: without this the hit test
    // finds nobody.
    canvas.buildDrawData(800, 600, data);
    return b;
}

static UiInputState ratonBoton(float x, float y, float t, bool abajo)
{
    UiInputState in = raton(x, y, t);
    in.mouseDown[0] = abajo;
    return in;
}

// Disabled > Pressed > Selected > Hover > Normal, and in THAT order.
static void test_boton_prioridad_de_estados()
{
    UiCanvas   canvas;
    UiDrawData data;
    Button&    b = montaBoton(canvas, data);

    const float dentroX = 100.0f, dentroY = 60.0f;   // inside the rect
    const float fueraX  = 700.0f, fueraY  = 500.0f;  // far from everything

    canvas.updateInput(ratonBoton(fueraX, fueraY, 0.0f, false));
    CHECK(b.state == UiButtonState::Normal);
    CHECK(b.hovered == false);

    canvas.updateInput(ratonBoton(dentroX, dentroY, 0.1f, false));
    CHECK(b.hovered == true);
    CHECK(b.state == UiButtonState::Hover);

    // Hover and pressed at the same time: Pressed wins.
    canvas.updateInput(ratonBoton(dentroX, dentroY, 0.2f, true));
    CHECK(b.state == UiButtonState::Pressed);

    // Selected beats Hover...
    canvas.updateInput(ratonBoton(dentroX, dentroY, 0.4f, false));
    b.selected = true;
    canvas.updateInput(ratonBoton(dentroX, dentroY, 0.5f, false));
    CHECK(b.state == UiButtonState::Selected);

    // ...and keeps winning with the mouse outside: it is game state, not mouse state.
    canvas.updateInput(ratonBoton(fueraX, fueraY, 0.6f, false));
    CHECK(b.state == UiButtonState::Selected);

    // ...but it loses to Pressed.
    canvas.updateInput(ratonBoton(dentroX, dentroY, 0.7f, true));
    CHECK(b.state == UiButtonState::Pressed);

    // Disabled takes out all four.
    b.interactable = false;
    canvas.updateInput(ratonBoton(dentroX, dentroY, 0.8f, true));
    CHECK(b.hovered == true);            // still receives hit testing
    CHECK(b.state == UiButtonState::Disabled);

    b.selected = false;
    canvas.updateInput(ratonBoton(dentroX, dentroY, 0.9f, false));
    CHECK(b.state == UiButtonState::Disabled);

    b.interactable = true;
    canvas.updateInput(ratonBoton(dentroX, dentroY, 1.0f, false));
    CHECK(b.state == UiButtonState::Hover);
}

// ColorTint writes to the SAME field that the batcher already reads.
static void test_boton_color_tint()
{
    UiCanvas   canvas;
    UiDrawData data;
    Button&    b = montaBoton(canvas, data);
    b.transition = UiButtonTransition::ColorTint;

    const glm::vec4 partida = b.color;

    canvas.updateInput(ratonBoton(700.0f, 500.0f, 0.0f, false));
    CHECK(b.color == kNormal);

    canvas.updateInput(ratonBoton(100.0f, 60.0f, 0.1f, false));
    CHECK(b.color == kHover);

    canvas.updateInput(ratonBoton(100.0f, 60.0f, 0.2f, true));
    CHECK(b.color == kPressed);

    b.interactable = false;
    canvas.updateInput(ratonBoton(100.0f, 60.0f, 0.3f, true));
    CHECK(b.color == kDisabled);

    b.interactable = true;
    b.selected     = true;
    canvas.updateInput(ratonBoton(100.0f, 60.0f, 0.4f, false));
    CHECK(b.color == kSelected);

    // Going back to Normal gives back EXACTLY the starting color.
    b.selected = false;
    canvas.updateInput(ratonBoton(700.0f, 500.0f, 0.5f, false));
    CHECK(b.state == UiButtonState::Normal);
    CHECK(b.color == partida);

    // And the color ends up in the vertex, which is what all this was about.
    canvas.buildDrawData(800, 600, data);
    CHECK(data.vertices.size() == 4);
    CHECK(nearly(data.vertices[0].color.r, kNormal.r));
    CHECK(nearly(data.vertices[0].color.g, kNormal.g));
    CHECK(nearly(data.vertices[0].color.b, kNormal.b));
}

// SpriteSwap changes the sprite name, not the atlas: the batch is not split.
// The base tint multiplies the state color. Before, a button's `color` field
// did NOTHING: the first updateInput overwrote it with the state color,
// so whoever touched it in the editor saw not a pixel of difference.
static void test_boton_base_color_tinta_los_estados()
{
    UiCanvas   canvas;
    UiDrawData data;
    Button&    b = montaBoton(canvas, data);
    b.transition = UiButtonTransition::ColorTint;

    // Non-neutral base with the three channels DIFFERENT: with a gray, a
    // swapped component would go unnoticed.
    b.baseColor = glm::vec4(0.5f, 0.25f, 0.75f, 1.0f);

    canvas.updateInput(ratonBoton(700.0f, 500.0f, 0.0f, false));
    CHECK(nearly(b.color.r, kNormal.r * 0.5f));
    CHECK(nearly(b.color.g, kNormal.g * 0.25f));
    CHECK(nearly(b.color.b, kNormal.b * 0.75f));

    canvas.updateInput(ratonBoton(100.0f, 60.0f, 0.1f, false));
    CHECK(nearly(b.color.r, kHover.r * 0.5f));
    CHECK(nearly(b.color.g, kHover.g * 0.25f));
    CHECK(nearly(b.color.b, kHover.b * 0.75f));

    // And it reaches the vertex, which is the only thing that can be seen.
    canvas.buildDrawData(800, 600, data);
    CHECK(data.vertices.size() == 4);
    CHECK(nearly(data.vertices[0].color.r, kHover.r * 0.5f));
    CHECK(nearly(data.vertices[0].color.g, kHover.g * 0.25f));

    // Neutrality: white base = the state color as is, which is what it
    // did before baseColor existed.
    b.baseColor = glm::vec4(1.0f);
    canvas.updateInput(ratonBoton(100.0f, 60.0f, 0.2f, false));
    CHECK(b.color == kHover);
}

static void test_boton_sprite_swap_no_parte_el_lote()
{
    UiTextureAtlas atlas;
    atlas.setSize(200, 100);
    atlas.addSprite("boton_normal",   {0.0f,  0.0f,  40.0f, 20.0f});
    atlas.addSprite("boton_hover",    {40.0f, 0.0f,  40.0f, 20.0f});
    atlas.addSprite("boton_pressed",  {80.0f, 0.0f,  40.0f, 20.0f});
    atlas.addSprite("boton_disabled", {0.0f,  20.0f, 40.0f, 20.0f});
    atlas.addSprite("boton_selected", {40.0f, 20.0f, 40.0f, 20.0f});

    UiCanvas   canvas;
    UiDrawData data;
    Button&    b = montaBoton(canvas, data);
    b.transition     = UiButtonTransition::SpriteSwap;
    b.atlas          = &atlas;
    b.sprite         = "boton_normal";
    b.normalSprite   = "boton_normal";
    b.hoverSprite    = "boton_hover";
    b.pressedSprite  = "boton_pressed";
    b.disabledSprite = "boton_disabled";
    b.selectedSprite = "boton_selected";

    // A neighbor of the SAME atlas: if the swap split the batch it would show here.
    UiElement& vecino = canvas.root().add("Fondo");
    vecino.position = glm::vec2(300.0f, 200.0f);
    vecino.size     = glm::vec2(90.0f, 45.0f);
    vecino.atlas    = &atlas;
    vecino.sprite   = "boton_normal";

    canvas.buildDrawData(800, 600, data);
    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 8);

    canvas.updateInput(ratonBoton(100.0f, 60.0f, 0.1f, false));
    CHECK(b.sprite == "boton_hover");
    CHECK(b.color == kNormal);              // SpriteSwap does NOT touch the color

    canvas.updateInput(ratonBoton(100.0f, 60.0f, 0.2f, true));
    CHECK(b.sprite == "boton_pressed");

    b.interactable = false;
    canvas.updateInput(ratonBoton(100.0f, 60.0f, 0.3f, true));
    CHECK(b.sprite == "boton_disabled");

    b.interactable = true;
    b.selected     = true;
    canvas.updateInput(ratonBoton(100.0f, 60.0f, 0.4f, false));
    CHECK(b.sprite == "boton_selected");

    b.selected = false;
    canvas.updateInput(ratonBoton(700.0f, 500.0f, 0.5f, false));
    CHECK(b.sprite == "boton_normal");

    // Same atlas = same batch, with whatever sprite.
    canvas.buildDrawData(800, 600, data);
    CHECK(data.batches.size() == 1);
    CHECK(data.vertices.size() == 8);
    CHECK(data.batches[0].atlas == &atlas);
}

// Animation: linear, with the time that comes in through UiInputState. No clock.
static void test_boton_animation_interpola_y_no_pasa_de_largo()
{
    UiCanvas   canvas;
    UiDrawData data;
    Button&    b = montaBoton(canvas, data);
    b.transition   = UiButtonTransition::Animation;
    // Neither 0.35 (double click) nor 5 (drag). And a power of two: the test's
    // instants fall EXACT in float, so the "at the end it is exact" measures the
    // clamp, not the rounding of a division.
    b.fadeDuration = 0.25f;

    // The first updateInput PLACES the color: it does not fade from the factory one.
    canvas.updateInput(ratonBoton(700.0f, 500.0f, 0.0f, false));
    CHECK(b.state == UiButtonState::Normal);
    CHECK(b.color == kNormal);

    // The mouse enters: the fade starts here, still without advancing.
    canvas.updateInput(ratonBoton(100.0f, 60.0f, 1.00f, false));
    CHECK(b.state == UiButtonState::Hover);
    CHECK(b.color == kNormal);

    // At half of fadeDuration, halfway along.
    canvas.updateInput(ratonBoton(100.0f, 60.0f, 1.125f, false));
    CHECK(nearly(b.color.r, 0.5f * (kNormal.r + kHover.r)));
    CHECK(nearly(b.color.g, 0.5f * (kNormal.g + kHover.g)));
    CHECK(nearly(b.color.b, 0.5f * (kNormal.b + kHover.b)));
    CHECK(nearly(b.color.a, 0.5f * (kNormal.a + kHover.a)));

    // One eighth more: three quarters of the way, linear.
    canvas.updateInput(ratonBoton(100.0f, 60.0f, 1.1875f, false));
    CHECK(nearly(b.color.r, kNormal.r + 0.75f * (kHover.r - kNormal.r)));
    CHECK(nearly(b.color.g, kNormal.g + 0.75f * (kHover.g - kNormal.g)));

    // At the end, EXACT.
    canvas.updateInput(ratonBoton(100.0f, 60.0f, 1.25f, false));
    CHECK(b.color == kHover);

    // And past the end it does not overshoot no matter how much time runs.
    canvas.updateInput(ratonBoton(100.0f, 60.0f, 9.00f, false));
    CHECK(b.color == kHover);

    // Cutting a fade halfway starts from the CURRENT color, without a jump.
    canvas.updateInput(ratonBoton(100.0f, 60.0f, 10.00f, true));   // → Pressed
    CHECK(b.state == UiButtonState::Pressed);
    CHECK(b.color == kHover);
    canvas.updateInput(ratonBoton(100.0f, 60.0f, 10.125f, true));
    CHECK(nearly(b.color.r, 0.5f * (kHover.r + kPressed.r)));
    canvas.updateInput(ratonBoton(100.0f, 60.0f, 10.25f, true));
    CHECK(b.color == kPressed);
}

// Non-interactable: it still changes state with the mouse over it, but neither Click
// nor DoubleClick.
static void test_boton_no_interactable_no_emite_click()
{
    UiCanvas   canvas;
    UiDrawData data;
    Button&    b = montaBoton(canvas, data);
    b.interactable = false;

    int clicks = 0, dobles = 0, abajo = 0;
    b.onClick       = [&](UiEvent&) { ++clicks; };
    b.onDoubleClick = [&](UiEvent&) { ++dobles; };
    b.onMouseDown   = [&](UiEvent&) { ++abajo; };

    auto clic = [&](float t)
    {
        canvas.updateInput(ratonBoton(100.0f, 60.0f, t, true));
        canvas.updateInput(ratonBoton(100.0f, 60.0f, t + 0.01f, false));
    };

    clic(0.0f);
    clic(0.10f);            // within doubleClickTime (0.35)
    CHECK(clicks == 0);
    CHECK(dobles == 0);
    CHECK(abajo == 2);      // the Down does come out: only Click and DoubleClick are swallowed
    CHECK(b.hovered == true);
    CHECK(b.state == UiButtonState::Disabled);

    // With the mouse outside it goes back to Disabled (not to Normal): interactable rules.
    canvas.updateInput(ratonBoton(700.0f, 500.0f, 0.5f, false));
    CHECK(b.state == UiButtonState::Disabled);

    // And with interactable the same gesture does give a click: the gate is the only thing that
    // was stopping them.
    b.interactable = true;
    clic(1.0f);
    CHECK(clicks == 1);
    CHECK(dobles == 0);
    clic(1.10f);
    CHECK(clicks == 2);
    CHECK(dobles == 1);
}

// Without updateInput a Button moves NOT A SINGLE vertex: same bytes as an equivalent canvas
// without buttons.
static void test_neutralidad_de_los_botones()
{
    UiTextureAtlas atlas;
    atlas.setSize(200, 100);
    atlas.addSprite("boton_normal", {0.0f, 0.0f, 40.0f, 20.0f});
    atlas.addSprite("boton_hover",  {40.0f, 0.0f, 40.0f, 20.0f});

    UiCanvas conBoton;
    {
        Button& b = conBoton.root().add<Button>("Aceptar");
        b.position       = glm::vec2(40.0f, 30.0f);
        b.size           = glm::vec2(120.0f, 64.0f);
        b.color          = kNormal;
        b.atlas          = &atlas;
        b.sprite         = "boton_normal";
        b.transition     = UiButtonTransition::Animation;
        b.fadeDuration   = 0.24f;
        b.interactable   = false;
        b.selected       = true;
        b.normalColor    = kNormal;
        b.hoverColor     = kHover;
        b.pressedColor   = kPressed;
        b.disabledColor  = kDisabled;
        b.selectedColor  = kSelected;
        b.normalSprite   = "boton_normal";
        b.hoverSprite    = "boton_hover";
    }

    UiCanvas sinBoton;
    {
        UiElement& e = sinBoton.root().add("Aceptar");
        e.position = glm::vec2(40.0f, 30.0f);
        e.size     = glm::vec2(120.0f, 64.0f);
        e.color    = kNormal;
        e.atlas    = &atlas;
        e.sprite   = "boton_normal";
    }

    UiDrawData a, c;
    conBoton.buildDrawData(800, 600, a);
    sinBoton.buildDrawData(800, 600, c);

    CHECK(a.vertices.size() == c.vertices.size());
    CHECK(a.indices.size() == c.indices.size());
    CHECK(a.batches.size() == c.batches.size());
    CHECK(!a.vertices.empty());
    CHECK(std::memcmp(a.vertices.data(), c.vertices.data(),
                      a.vertices.size() * sizeof(a.vertices[0])) == 0);
    CHECK(std::memcmp(a.indices.data(), c.indices.data(),
                      a.indices.size() * sizeof(a.indices[0])) == 0);
}

// ── Rectangular masks ──────────────────────────────────────────────────
// The mask is clipChildren + insets, and it is ALWAYS composed by intersection.
// Everything checked here fails silently: an inset on the wrong
// side clips too much on one side and too little on the other without a single
// validation error, and an empty mask that comes out negative is a VkRect2D
// that blows up the driver, not a red test.

// No scissor can come out negative (in uint32_t that is a giant value) nor
// larger than the render.
static void scissorSano(const UiDrawData& data)
{
    for (const auto& b : data.batches)
    {
        CHECK(b.scissor.width  <= kW);
        CHECK(b.scissor.height <= kH);
        CHECK(b.scissor.x >= 0 && b.scissor.y >= 0);
    }
}

// The four insets, each on ITS side: x, y, width and height are checked
// separately because an inset switched to another side leaves the total area the same.
static void test_mascara_insets_cada_uno_en_su_lado()
{
    UiCanvas canvas;

    UiElement& panel = canvas.root().add("panel");
    panel.position     = glm::vec2(100.0f, 60.0f);
    panel.size         = glm::vec2(240.0f, 150.0f);   // width != height
    panel.clipChildren = true;
    panel.maskInsetLeft   = 7.0f;                     // all four different
    panel.maskInsetRight  = 13.0f;
    panel.maskInsetTop    = 5.0f;
    panel.maskInsetBottom = 21.0f;

    // It goes out through ALL FOUR sides and by a different amount: 30 on the
    // left, 18 on top, 70 on the right and 92 at the bottom.
    UiElement& hijo = panel.add("hijo");
    hijo.position = glm::vec2(-30.0f, -18.0f);
    hijo.size     = glm::vec2(340.0f, 260.0f);

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    // 100+7 = 107 ; 60+5 = 65 ; 240-7-13 = 220 ; 150-5-21 = 124.
    const UiScissor& s = panel.screenScissor;
    CHECK(s.x == 107);
    CHECK(s.y == 65);
    CHECK(s.width  == 220);
    CHECK(s.height == 124);

    // The one that is drawn is exactly that one, not another computed separately.
    CHECK(data.batches.size() == 1);
    if (!data.batches.empty()) CHECK(data.batches[0].scissor == s);
    // And the clipped child inherits the same one (same atlas + same scissor = 1 batch).
    CHECK(hijo.screenScissor == s);
    CHECK(data.vertices.size() == 8);
    scissorSano(data);
}

// maskSelf = false: the window's frame is left OUTSIDE its own mask.
static void test_mascara_self_deja_fuera_al_propio_elemento()
{
    UiCanvas canvas;

    UiElement& panel = canvas.root().add("panel");
    panel.position     = glm::vec2(90.0f, 40.0f);
    panel.size         = glm::vec2(260.0f, 130.0f);
    panel.clipChildren = true;
    panel.maskInsetLeft   = 11.0f;
    panel.maskInsetRight  = 4.0f;
    panel.maskInsetTop    = 9.0f;
    panel.maskInsetBottom = 17.0f;
    panel.maskSelf        = false;

    UiElement& hijo = panel.add("hijo");
    hijo.position = glm::vec2(-25.0f, -12.0f);
    hijo.size     = glm::vec2(400.0f, 220.0f);

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    const UiScissor mascara{101, 49, 245, 104};       // 260-11-4 ; 130-9-17

    // The panel comes out with what it INHERITED (the whole screen), the child with the
    // mask: two different scissors, two batches.
    CHECK(panel.screenScissor.x == 0 && panel.screenScissor.y == 0);
    CHECK(panel.screenScissor.width == kW && panel.screenScissor.height == kH);
    CHECK(hijo.screenScissor == mascara);
    CHECK(data.batches.size() == 2);
    if (data.batches.size() == 2)
    {
        CHECK(data.batches[0].scissor == panel.screenScissor);
        CHECK(data.batches[1].scissor == mascara);
    }
    scissorSano(data);

    // With maskSelf true (the default) both go with the mask and there is again
    // a single batch.
    panel.maskSelf = true;
    panel.markDirty(UiElement::DirtyAll);
    UiDrawData recortado;
    canvas.buildDrawData(kW, kH, recortado);

    CHECK(panel.screenScissor == mascara);
    CHECK(hijo.screenScissor  == mascara);
    CHECK(recortado.batches.size() == 1);
    scissorSano(recortado);
}

// Mask inside mask = INTERSECTION. A daughter larger than the mother does not
// enlarge it: with replacement, the daughter would draw outside its parent.
static void test_mascara_anidada_es_interseccion()
{
    UiCanvas canvas;

    UiElement& madre = canvas.root().add("madre");
    madre.position     = glm::vec2(120.0f, 70.0f);
    madre.size         = glm::vec2(240.0f, 150.0f);   // [120,360) x [70,220)
    madre.clipChildren = true;
    madre.drawable     = false;

    // Larger than the mother on all four sides: the intersection has to
    // still be the mother's, not a pixel more.
    UiElement& grande = madre.add("grande");
    grande.position     = glm::vec2(-50.0f, -20.0f);
    grande.size         = glm::vec2(400.0f, 300.0f);
    grande.clipChildren = true;
    grande.drawable     = false;

    UiElement& nietoGrande = grande.add("nietoGrande");
    nietoGrande.size = glm::vec2(500.0f, 400.0f);

    // And a smaller mask inside: it rules.
    UiElement& pequena = madre.add("pequena");
    pequena.position     = glm::vec2(20.0f, 10.0f);   // [140,230) x [80,140)
    pequena.size         = glm::vec2(90.0f, 60.0f);
    pequena.clipChildren = true;
    pequena.drawable     = false;
    pequena.maskInsetLeft = 6.0f;                     // and with insets on top
    pequena.maskInsetTop  = 3.0f;

    UiElement& nietoPequeno = pequena.add("nietoPequeno");
    nietoPequeno.size = glm::vec2(300.0f, 200.0f);

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    const UiScissor deLaMadre{120, 70, 240, 150};
    CHECK(madre.screenScissor  == deLaMadre);
    CHECK(grande.screenScissor == deLaMadre);
    CHECK(nietoGrande.screenScissor == deLaMadre);

    // 140+6 = 146 ; 80+3 = 83 ; 90-6 = 84 ; 60-3 = 57.
    const UiScissor deLaPequena{146, 83, 84, 57};
    CHECK(pequena.screenScissor      == deLaPequena);
    CHECK(nietoPequeno.screenScissor == deLaPequena);
    scissorSano(data);
}

// Insets that cross: empty mask. Not a single vertex of the children, and NEVER a
// scissor with negative width or height (in Vulkan that is a crash, not an empty
// draw).
static void test_mascara_vacia_no_emite_ni_revienta()
{
    UiCanvas canvas;

    UiElement& panel = canvas.root().add("panel");
    panel.position     = glm::vec2(150.0f, 80.0f);
    panel.size         = glm::vec2(240.0f, 150.0f);
    panel.clipChildren = true;
    panel.maskInsetLeft  = 140.0f;                    // 140+130 > 240
    panel.maskInsetRight = 130.0f;
    panel.maskInsetTop   = 12.0f;
    panel.maskInsetBottom = 9.0f;
    panel.maskSelf       = false;                     // the frame is visible

    UiElement& hijo = panel.add("hijo");
    hijo.size = glm::vec2(200.0f, 120.0f);
    UiElement& nieto = hijo.add("nieto");
    nieto.size = glm::vec2(60.0f, 40.0f);

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    // Only the panel: the descendants emit not a single vertex.
    CHECK(data.vertices.size() == 4);
    CHECK(data.indices.size() == 6);
    CHECK(data.batches.size() == 1);
    scissorSano(data);

    // The element is still alive; the clipped ones are left without a resolved rect.
    CHECK(panel.rectValid);
    CHECK(!hijo.rectValid);
    CHECK(!nieto.rectValid);

    // With maskSelf true not even the panel is seen, and even so nothing blows up.
    panel.maskSelf = true;
    panel.markDirty(UiElement::DirtyAll);
    UiDrawData nada;
    canvas.buildDrawData(kW, kH, nada);
    CHECK(nada.vertices.empty());
    CHECK(nada.batches.empty());
    CHECK(!panel.rectValid);
    scissorSano(nada);
}

// Two children under the SAME mask and the same atlas = ONE batch; and maskEnabled at
// false gives back exactly the batches of not having a mask.
static void test_mascara_lotes_y_mask_enabled()
{
    UiCanvas canvas;

    UiElement& panel = canvas.root().add("panel");
    panel.position     = glm::vec2(60.0f, 35.0f);
    panel.size         = glm::vec2(300.0f, 180.0f);
    panel.drawable     = false;
    panel.clipChildren = true;
    panel.maskInsetLeft   = 8.0f;
    panel.maskInsetRight  = 3.0f;
    panel.maskInsetTop    = 14.0f;
    panel.maskInsetBottom = 6.0f;

    UiElement& a = panel.add("a");
    a.position = glm::vec2(10.0f, 20.0f);
    a.size     = glm::vec2(70.0f, 45.0f);
    UiElement& b = panel.add("b");
    b.position = glm::vec2(-40.0f, 130.0f);           // it goes out through two sides
    b.size     = glm::vec2(260.0f, 90.0f);

    UiDrawData conMascara;
    canvas.buildDrawData(kW, kH, conMascara);

    CHECK(conMascara.batches.size() == 1);
    CHECK(conMascara.vertices.size() == 8);
    CHECK(a.screenScissor == b.screenScissor);
    scissorSano(conMascara);

    // Turned off: the same batches as an equivalent tree without clipChildren.
    panel.maskEnabled = false;
    panel.markDirty(UiElement::DirtyAll);
    UiDrawData apagada;
    canvas.buildDrawData(kW, kH, apagada);

    UiCanvas limpio;
    UiElement& panel2 = limpio.root().add("panel");
    panel2.position = glm::vec2(60.0f, 35.0f);
    panel2.size     = glm::vec2(300.0f, 180.0f);
    panel2.drawable = false;
    UiElement& a2 = panel2.add("a");
    a2.position = glm::vec2(10.0f, 20.0f);
    a2.size     = glm::vec2(70.0f, 45.0f);
    UiElement& b2 = panel2.add("b");
    b2.position = glm::vec2(-40.0f, 130.0f);
    b2.size     = glm::vec2(260.0f, 90.0f);

    UiDrawData sinMascara;
    limpio.buildDrawData(kW, kH, sinMascara);

    CHECK(apagada.batches.size() == sinMascara.batches.size());
    CHECK(apagada.vertices.size() == sinMascara.vertices.size());
    if (!apagada.batches.empty() && !sinMascara.batches.empty())
        CHECK(apagada.batches[0].scissor == sinMascara.batches[0].scissor);
    CHECK(!apagada.vertices.empty());
    CHECK(std::memcmp(apagada.vertices.data(), sinMascara.vertices.data(),
                      apagada.vertices.size() * sizeof(apagada.vertices[0])) == 0);
}

// The mask clips the hit test just like the drawing, without code of its own: the
// input reuses the SAME screenScissor with which it was drawn.
static void test_mascara_recorta_el_hit_test()
{
    UiCanvas canvas;

    UiElement& panel = canvas.root().add("panel");
    panel.position     = glm::vec2(300.0f, 100.0f);   // [300,500) x [100,220)
    panel.size         = glm::vec2(200.0f, 120.0f);
    panel.clipChildren = true;
    panel.maskInsetLeft   = 40.0f;                    // mask [340,490) x [120,190)
    panel.maskInsetRight  = 10.0f;
    panel.maskInsetTop    = 20.0f;
    panel.maskInsetBottom = 30.0f;

    UiElement& hijo = panel.add("hijo");
    hijo.size = glm::vec2(200.0f, 120.0f);            // the panel's whole rect

    colocar(canvas);

    // Inside the mask: it reaches the child.
    CHECK(canvas.hitTest(glm::vec2(400.0f, 150.0f)) == &hijo);
    // Inside the child's RECT but outside the mask: nobody, on all four
    // sides.
    CHECK(canvas.hitTest(glm::vec2(310.0f, 150.0f)) == nullptr);
    CHECK(canvas.hitTest(glm::vec2(495.0f, 150.0f)) == nullptr);
    CHECK(canvas.hitTest(glm::vec2(400.0f, 110.0f)) == nullptr);
    CHECK(canvas.hitTest(glm::vec2(400.0f, 210.0f)) == nullptr);

    // With the mask turned off, that same point is reached.
    panel.maskEnabled = false;
    panel.markDirty(UiElement::DirtyAll);
    colocar(canvas);
    CHECK(canvas.hitTest(glm::vec2(310.0f, 150.0f)) == &hijo);
}

// Without clipChildren, the new fields do NOTHING: same bytes of vertices and
// indices as a tree built without touching them.
static void test_mascara_neutral_sin_clip_children()
{
    UiCanvas conCampos;
    UiElement& p = conCampos.root().add("p");
    p.position = glm::vec2(70.0f, 90.0f);
    p.size     = glm::vec2(210.0f, 130.0f);
    p.maskInsetLeft   = 19.0f;                        // without clipChildren: noise
    p.maskInsetRight  = 27.0f;
    p.maskInsetTop    = 33.0f;
    p.maskInsetBottom = 41.0f;
    p.maskSelf    = false;
    p.maskEnabled = false;
    UiElement& h = p.add("h");
    h.position = glm::vec2(-15.0f, 25.0f);
    h.size     = glm::vec2(180.0f, 60.0f);
    h.maskInsetTop = 12.0f;
    h.maskSelf     = false;

    UiCanvas limpio;
    UiElement& p2 = limpio.root().add("p");
    p2.position = glm::vec2(70.0f, 90.0f);
    p2.size     = glm::vec2(210.0f, 130.0f);
    UiElement& h2 = p2.add("h");
    h2.position = glm::vec2(-15.0f, 25.0f);
    h2.size     = glm::vec2(180.0f, 60.0f);

    UiDrawData conRuido, sinRuido;
    conCampos.buildDrawData(kW, kH, conRuido);
    limpio.buildDrawData(kW, kH, sinRuido);

    CHECK(conRuido.vertices.size() == sinRuido.vertices.size());
    CHECK(conRuido.indices.size()  == sinRuido.indices.size());
    CHECK(conRuido.batches.size()  == sinRuido.batches.size());
    CHECK(!conRuido.vertices.empty());
    CHECK(std::memcmp(conRuido.vertices.data(), sinRuido.vertices.data(),
                      conRuido.vertices.size() * sizeof(conRuido.vertices[0])) == 0);
    CHECK(std::memcmp(conRuido.indices.data(), sinRuido.indices.data(),
                      conRuido.indices.size() * sizeof(conRuido.indices[0])) == 0);
    if (!conRuido.batches.empty() && !sinRuido.batches.empty())
        CHECK(conRuido.batches[0].scissor == sinRuido.batches[0].scissor);
}

// ── Property animations ──────────────────────────────────────────────
// All CPU: the clock is advanced by hand and what came out in the vertices is looked at.
// The first updateInput NEVER advances (there is no previous frame), so all
// the tests start with a call at t=0 and count from there.

static void avanzaReloj(UiCanvas& cv, float t)
{
    UiInputState in{};
    in.mousePos    = glm::vec2(-1000.0f, -1000.0f);   // far away: no hover in the way
    in.timeSeconds = t;
    cv.updateInput(in);
}

// Value of the curve at t, measured THROUGH THE REAL PATH: a Fade from 0 to 1
// returns exactly f(t) in opacity. PingPong and not Once on purpose: that way at
// t=1 the curve rules and not the snap to animTo.
static float curvaEn(UiAnimCurve curva, float t01)
{
    const float dur = 2.5f;

    UiCanvas cv;
    UiElement& e = cv.root().add("c");
    e.size = glm::vec2(40.0f, 25.0f);

    e.anim         = UiAnim::Fade;
    e.animCurve    = curva;
    e.animFrom     = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    e.animTo       = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
    e.animDuration = dur;
    e.animLoop     = UiAnimLoop::PingPong;
    e.animPlaying  = true;

    avanzaReloj(cv, 0.0f);
    avanzaReloj(cv, t01 * dur);
    return e.opacity;
}

static void test_anim_curvas_extremos_medio_y_desbordes()
{
    const UiAnimCurve todas[5] = { UiAnimCurve::Linear, UiAnimCurve::EaseIn,
                                   UiAnimCurve::EaseOut, UiAnimCurve::Bounce,
                                   UiAnimCurve::Elastic };

    // The extremes are EXACT, not "almost": a curve that closes at 0.99999994
    // leaves the property a hair away from its destination forever.
    float medio[5];
    for (int i = 0; i < 5; ++i)
    {
        CHECK(curvaEn(todas[i], 0.0f) == 0.0f);
        CHECK(curvaEn(todas[i], 1.0f) == 1.0f);
        medio[i] = curvaEn(todas[i], 0.5f);
    }

    // The five DIFFERENT at t=0.5: a curve copied from another shows up here.
    for (int i = 0; i < 5; ++i)
        for (int j = i + 1; j < 5; ++j)
            CHECK(std::fabs(medio[i] - medio[j]) > 1e-3f);

    CHECK(medio[1] < medio[0]);   // EaseIn BELOW Linear
    CHECK(medio[2] > medio[0]);   // EaseOut ABOVE

    // Bounce bounces: it is not monotonic (it rises, overshoots and falls).
    bool baja = false;
    float previo = curvaEn(UiAnimCurve::Bounce, 0.0f);
    for (int i = 1; i <= 50; ++i)
    {
        const float v = curvaEn(UiAnimCurve::Bounce, (float)i / 50.0f);
        if (v < previo - 1e-4f) baja = true;
        previo = v;
    }
    CHECK(baja);

    // Elastic GOES OUT of [0,1] upward halfway through, and that is not
    // clamped: it is what makes the property overshoot and come back.
    float maximo = 0.0f;
    for (int i = 0; i <= 50; ++i)
        maximo = std::fmax(maximo, curvaEn(UiAnimCurve::Elastic, (float)i / 50.0f));
    CHECK(maximo > 1.0f);
}

static void test_anim_fade_mueve_el_alfa_y_no_la_posicion()
{
    UiCanvas cv;
    UiElement& e = cv.root().add("f");
    e.position = glm::vec2(70.0f, 45.0f);
    e.size     = glm::vec2(120.0f, 34.0f);

    // 0.04 -> 0.23 is not just any pair: the lerp with k=1 does NOT return exactly 0.23,
    // so the snap at the end has to really be there.
    e.anim         = UiAnim::Fade;
    e.animFrom     = glm::vec4(0.04f, 0.0f, 0.0f, 0.0f);
    e.animTo       = glm::vec4(0.23f, 0.0f, 0.0f, 0.0f);
    e.animDuration = 2.5f;
    e.animPlaying  = true;

    UiDrawData a, b;
    avanzaReloj(cv, 0.0f);
    cv.buildDrawData(kW, kH, a);
    CHECK(a.vertices.size() == 4);
    CHECK(nearly(a.vertices[0].color.a, 0.04f));

    avanzaReloj(cv, 1.25f);       // exact half of 2.5
    cv.buildDrawData(kW, kH, b);
    CHECK(nearly(b.vertices[0].color.a, 0.135f));   // 0.04 + 0.19*0.5

    // Not a vertex out of place: Fade only touches the alpha.
    for (size_t i = 0; i < 4; ++i)
    {
        CHECK(a.vertices[i].pos.x == b.vertices[i].pos.x);
        CHECK(a.vertices[i].pos.y == b.vertices[i].pos.y);
    }
}

static void test_anim_move_mueve_la_posicion_y_no_el_tamano()
{
    UiCanvas cv;
    UiElement& e = cv.root().add("m");
    e.size = glm::vec2(120.0f, 34.0f);

    // Both axes change and both change sign; and neither of the two
    // pairs lands exact by lerp, so the snap is noticeable if it is missing.
    e.anim         = UiAnim::Move;
    e.animFrom     = glm::vec4(-90.0f, 33.7f, 0.0f, 0.0f);
    e.animTo       = glm::vec4(8.3f, -25.9f, 0.0f, 0.0f);
    e.animDuration = 2.5f;
    e.animPlaying  = true;

    UiDrawData a, b;
    avanzaReloj(cv, 0.0f);
    cv.buildDrawData(kW, kH, a);
    CHECK(nearly(a.vertices[0].pos.x, -90.0f));
    CHECK(nearly(a.vertices[0].pos.y,  33.7f));

    avanzaReloj(cv, 2.5f);
    cv.buildDrawData(kW, kH, b);
    CHECK(e.position.x == 8.3f);      // animTo EXACTO
    CHECK(e.position.y == -25.9f);
    CHECK(nearly(b.vertices[0].pos.x,   8.3f));
    CHECK(nearly(b.vertices[0].pos.y, -25.9f));

    // Nobody touches the size: width and height are still the usual ones.
    CHECK(nearly(a.vertices[1].pos.x - a.vertices[0].pos.x, 120.0f));
    CHECK(nearly(b.vertices[1].pos.x - b.vertices[0].pos.x, 120.0f));
    CHECK(nearly(a.vertices[2].pos.y - a.vertices[1].pos.y, 34.0f));
    CHECK(nearly(b.vertices[2].pos.y - b.vertices[1].pos.y, 34.0f));
}

static void test_anim_scale_cambia_el_tamano_y_no_el_pivot()
{
    UiCanvas cv;
    UiElement& e = cv.root().add("s");
    e.position = glm::vec2(200.0f, 130.0f);
    e.size     = glm::vec2(80.0f, 40.0f);
    e.pivot    = glm::vec2(0.5f, 0.5f);

    e.anim         = UiAnim::Scale;
    e.animFrom     = glm::vec4(0.6f, 1.0f, 0.0f, 0.0f);
    e.animTo       = glm::vec4(2.7f, 0.1f, 0.0f, 0.0f);
    e.animDuration = 2.5f;
    e.animPlaying  = true;

    UiDrawData a, b;
    avanzaReloj(cv, 0.0f);
    cv.buildDrawData(kW, kH, a);
    avanzaReloj(cv, 2.5f);
    cv.buildDrawData(kW, kH, b);

    CHECK(e.scale.x == 2.7f);
    CHECK(e.scale.y == 0.1f);

    CHECK(nearly(a.vertices[1].pos.x - a.vertices[0].pos.x,  48.0f));   // 80 * 0.6
    CHECK(nearly(b.vertices[1].pos.x - b.vertices[0].pos.x, 216.0f));   // 80 * 2.7
    CHECK(nearly(a.vertices[2].pos.y - a.vertices[1].pos.y,  40.0f));
    CHECK(nearly(b.vertices[2].pos.y - b.vertices[1].pos.y,   4.0f));   // 40 * 0.1

    // The pivot does NOT move: it scales around it, not dragging it.
    CHECK(nearly((a.vertices[0].pos.x + a.vertices[2].pos.x) * 0.5f,
                 (b.vertices[0].pos.x + b.vertices[2].pos.x) * 0.5f));
    CHECK(nearly((a.vertices[0].pos.y + a.vertices[2].pos.y) * 0.5f,
                 (b.vertices[0].pos.y + b.vertices[2].pos.y) * 0.5f));
}

static void test_anim_color_mueve_los_cuatro_canales()
{
    UiCanvas cv;
    UiElement& e = cv.root().add("c");
    e.position = glm::vec2(15.0f, 90.0f);
    e.size     = glm::vec2(140.0f, 55.0f);

    // The four channels with values DIFFERENT from each other on both sides, and the
    // four pairs chosen so that the lerp does not close exact on its
    // own: if the snap disappears, the four equality CHECKs fall.
    e.anim         = UiAnim::Color;
    e.animFrom     = glm::vec4(0.04f, 0.02f, 0.01f, 0.05f);
    e.animTo       = glm::vec4(0.17f, 0.10f, 0.05f, 0.12f);
    e.animDuration = 2.5f;
    e.animPlaying  = true;

    UiDrawData a, b;
    avanzaReloj(cv, 0.0f);
    cv.buildDrawData(kW, kH, a);
    avanzaReloj(cv, 2.5f);
    cv.buildDrawData(kW, kH, b);

    CHECK(nearly(a.vertices[0].color.r, 0.04f));
    CHECK(nearly(a.vertices[0].color.g, 0.02f));
    CHECK(nearly(a.vertices[0].color.b, 0.01f));
    CHECK(nearly(a.vertices[0].color.a, 0.05f));

    CHECK(e.color.r == 0.17f);   // animTo EXACT, channel by channel
    CHECK(e.color.g == 0.10f);
    CHECK(e.color.b == 0.05f);
    CHECK(e.color.a == 0.12f);
    CHECK(nearly(b.vertices[0].color.r, 0.17f));
    CHECK(nearly(b.vertices[0].color.g, 0.10f));
    CHECK(nearly(b.vertices[0].color.b, 0.05f));
    CHECK(nearly(b.vertices[0].color.a, 0.12f));

    // And not a vertex out of place.
    CHECK(std::memcmp(&a.vertices[0].pos, &b.vertices[0].pos, sizeof(a.vertices[0].pos)) == 0);
    CHECK(std::memcmp(&a.vertices[2].pos, &b.vertices[2].pos, sizeof(a.vertices[2].pos)) == 0);
}

static void test_anim_rotation_gira_las_esquinas_conservando_la_distancia()
{
    UiCanvas cv;
    UiElement& e = cv.root().add("r");
    e.position = glm::vec2(200.0f, 130.0f);
    e.size     = glm::vec2(80.0f, 40.0f);
    e.pivot    = glm::vec2(0.5f, 0.5f);

    e.anim         = UiAnim::Rotation;
    e.animFrom     = glm::vec4(0.18f, 0.0f, 0.0f, 0.0f);
    e.animTo       = glm::vec4(1.32f, 0.0f, 0.0f, 0.0f);
    e.animDuration = 2.5f;
    e.animPlaying  = true;

    UiDrawData a, b;
    avanzaReloj(cv, 0.0f);
    cv.buildDrawData(kW, kH, a);
    avanzaReloj(cv, 2.5f);
    cv.buildDrawData(kW, kH, b);

    CHECK(e.rotation == 1.32f);

    // With rotation the quad is no longer axis-aligned: the two on
    // top no longer share the Y.
    CHECK(std::fabs(a.vertices[0].pos.y - a.vertices[1].pos.y) > 1e-3f);
    CHECK(std::fabs(b.vertices[0].pos.y - b.vertices[1].pos.y) > 1e-3f);

    // The pivot in world space is the position: that is where the center of rotation is.
    const glm::vec2 centro(200.0f, 130.0f);
    bool alguna_se_movio = false;
    for (size_t i = 0; i < 4; ++i)
    {
        const float d0 = std::sqrt((a.vertices[i].pos.x - centro.x) * (a.vertices[i].pos.x - centro.x) +
                                   (a.vertices[i].pos.y - centro.y) * (a.vertices[i].pos.y - centro.y));
        const float d1 = std::sqrt((b.vertices[i].pos.x - centro.x) * (b.vertices[i].pos.x - centro.x) +
                                   (b.vertices[i].pos.y - centro.y) * (b.vertices[i].pos.y - centro.y));
        CHECK(std::fabs(d0 - d1) < 1e-3f);
        if (std::fabs(a.vertices[i].pos.x - b.vertices[i].pos.x) > 1e-3f) alguna_se_movio = true;
    }
    CHECK(alguna_se_movio);

    // It rotates, it does not stretch: the diagonal still measures the same.
    const float diag0 = std::sqrt((a.vertices[2].pos.x - a.vertices[0].pos.x) * (a.vertices[2].pos.x - a.vertices[0].pos.x) +
                                  (a.vertices[2].pos.y - a.vertices[0].pos.y) * (a.vertices[2].pos.y - a.vertices[0].pos.y));
    const float diag1 = std::sqrt((b.vertices[2].pos.x - b.vertices[0].pos.x) * (b.vertices[2].pos.x - b.vertices[0].pos.x) +
                                  (b.vertices[2].pos.y - b.vertices[0].pos.y) * (b.vertices[2].pos.y - b.vertices[0].pos.y));
    CHECK(std::fabs(diag0 - diag1) < 1e-3f);
}

// The same animated tree, built twice, to compare bytes.
static void montaAnimada(UiCanvas& cv)
{
    UiElement& e = cv.root().add("d");
    e.position = glm::vec2(40.0f, 25.0f);
    e.size     = glm::vec2(150.0f, 60.0f);

    e.anim         = UiAnim::Move;
    e.animCurve    = UiAnimCurve::Elastic;
    e.animFrom     = glm::vec4(-30.0f, 45.0f, 0.0f, 0.0f);
    e.animTo       = glm::vec4(60.0f, -20.0f, 0.0f, 0.0f);
    e.animDuration = 2.5f;
    e.animPlaying  = true;
}

static void test_anim_determinismo_por_tiempo_y_por_pasos()
{
    // Same instant requested twice: the same bytes.
    UiCanvas uno, otro;
    montaAnimada(uno);
    montaAnimada(otro);

    avanzaReloj(uno, 0.0f);  avanzaReloj(uno, 1.5f);
    avanzaReloj(otro, 0.0f); avanzaReloj(otro, 1.5f);

    UiDrawData a, b;
    uno.buildDrawData(kW, kH, a);
    otro.buildDrawData(kW, kH, b);
    CHECK(!a.vertices.empty());
    CHECK(a.vertices.size() == b.vertices.size());
    CHECK(std::memcmp(a.vertices.data(), b.vertices.data(),
                      a.vertices.size() * sizeof(a.vertices[0])) == 0);

    // In one jump or in four steps: it does not matter, because what advances is the
    // DELTA and the sum of the four is the same.
    UiCanvas salto, pasos;
    montaAnimada(salto);
    montaAnimada(pasos);

    avanzaReloj(salto, 0.0f);
    avanzaReloj(salto, 2.0f);

    avanzaReloj(pasos, 0.0f);
    avanzaReloj(pasos, 0.5f);
    avanzaReloj(pasos, 1.0f);
    avanzaReloj(pasos, 1.5f);
    avanzaReloj(pasos, 2.0f);

    UiDrawData c, d;
    salto.buildDrawData(kW, kH, c);
    pasos.buildDrawData(kW, kH, d);
    CHECK(c.vertices.size() == d.vertices.size());
    CHECK(std::memcmp(c.vertices.data(), d.vertices.data(),
                      c.vertices.size() * sizeof(c.vertices[0])) == 0);
}

static void test_anim_once_loop_y_pingpong()
{
    // Once: it snaps to animTo EXACT and stops.
    UiCanvas cv;
    UiElement& e = cv.root().add("once");
    e.size         = glm::vec2(90.0f, 30.0f);
    e.anim         = UiAnim::Fade;
    e.animFrom     = glm::vec4(0.04f, 0.0f, 0.0f, 0.0f);
    e.animTo       = glm::vec4(0.23f, 0.0f, 0.0f, 0.0f);
    e.animDuration = 2.5f;
    e.animPlaying  = true;

    avanzaReloj(cv, 0.0f);
    avanzaReloj(cv, 9.0f);            // far past the end
    CHECK(e.opacity == 0.23f);
    CHECK(e.animPlaying == false);
    CHECK(e.animTime == 2.5f);        // it does not keep growing

    // Loop: at 3.0 with duration 2.5 it is where it is at 0.5, and it keeps playing.
    UiCanvas lc, ref;
    UiElement& l = lc.root().add("loop");
    l.size         = glm::vec2(90.0f, 30.0f);
    l.anim         = UiAnim::Fade;
    l.animFrom     = glm::vec4(0.2f, 0.0f, 0.0f, 0.0f);
    l.animTo       = glm::vec4(0.85f, 0.0f, 0.0f, 0.0f);
    l.animDuration = 2.5f;
    l.animLoop     = UiAnimLoop::Loop;
    l.animPlaying  = true;

    UiElement& r = ref.root().add("ref");
    r.size         = l.size;
    r.anim         = l.anim;
    r.animFrom     = l.animFrom;
    r.animTo       = l.animTo;
    r.animDuration = l.animDuration;
    r.animLoop     = l.animLoop;
    r.animPlaying  = true;

    avanzaReloj(lc, 0.0f);  avanzaReloj(lc, 3.0f);
    avanzaReloj(ref, 0.0f); avanzaReloj(ref, 0.5f);
    CHECK(l.opacity == r.opacity);
    CHECK(l.animPlaying == true);
    CHECK(l.opacity < 0.85f);          // it has restarted, it has not stayed at the end

    // PingPong: at 3.75 it comes back the way it went and matches the outbound at 1.25.
    UiCanvas pp, ida;
    UiElement& p = pp.root().add("pp");
    p.size         = glm::vec2(90.0f, 30.0f);
    p.anim         = UiAnim::Fade;
    p.animFrom     = glm::vec4(0.2f, 0.0f, 0.0f, 0.0f);
    p.animTo       = glm::vec4(0.85f, 0.0f, 0.0f, 0.0f);
    p.animDuration = 2.5f;
    p.animLoop     = UiAnimLoop::PingPong;
    p.animPlaying  = true;

    UiElement& i = ida.root().add("ida");
    i.size         = p.size;
    i.anim         = p.anim;
    i.animFrom     = p.animFrom;
    i.animTo       = p.animTo;
    i.animDuration = p.animDuration;
    i.animLoop     = p.animLoop;
    i.animPlaying  = true;

    avanzaReloj(pp, 0.0f);  avanzaReloj(pp, 3.75f);
    avanzaReloj(ida, 0.0f); avanzaReloj(ida, 1.25f);
    CHECK(p.opacity == i.opacity);
    CHECK(p.animPlaying == true);
}

static void test_anim_playing_false_congela()
{
    UiCanvas cv;
    UiElement& e = cv.root().add("frio");
    e.position = glm::vec2(40.0f, 25.0f);
    e.size     = glm::vec2(150.0f, 60.0f);

    e.anim         = UiAnim::Move;
    e.animFrom     = glm::vec4(-30.0f, 45.0f, 0.0f, 0.0f);
    e.animTo       = glm::vec4(60.0f, -20.0f, 0.0f, 0.0f);
    e.animDuration = 2.5f;
    e.animPlaying  = false;

    UiDrawData a, b;
    avanzaReloj(cv, 0.0f);
    cv.buildDrawData(kW, kH, a);
    avanzaReloj(cv, 4.0f);
    cv.buildDrawData(kW, kH, b);

    CHECK(e.animTime == 0.0f);
    CHECK(e.position == glm::vec2(40.0f, 25.0f));   // untouched
    CHECK(a.vertices.size() == b.vertices.size());
    CHECK(std::memcmp(a.vertices.data(), b.vertices.data(),
                      a.vertices.size() * sizeof(a.vertices[0])) == 0);
}

static void test_rotacion_cero_no_toca_ni_un_vertice()
{
    UiCanvas conCero, sinTocar;

    UiElement& a = conCero.root().add("p");
    a.position = glm::vec2(33.0f, 71.0f);
    a.size     = glm::vec2(170.0f, 45.0f);
    a.pivot    = glm::vec2(0.5f, 0.25f);
    a.rotation = 0.0f;                       // explicit
    UiElement& ah = a.add("h");
    ah.position = glm::vec2(-12.0f, 18.0f);
    ah.size     = glm::vec2(60.0f, 90.0f);
    ah.rotation = 0.0f;

    UiElement& b = sinTocar.root().add("p");
    b.position = glm::vec2(33.0f, 71.0f);
    b.size     = glm::vec2(170.0f, 45.0f);
    b.pivot    = glm::vec2(0.5f, 0.25f);
    UiElement& bh = b.add("h");
    bh.position = glm::vec2(-12.0f, 18.0f);
    bh.size     = glm::vec2(60.0f, 90.0f);

    UiDrawData x, y;
    conCero.buildDrawData(kW, kH, x);
    sinTocar.buildDrawData(kW, kH, y);

    CHECK(!x.vertices.empty());
    CHECK(x.vertices.size() == y.vertices.size());
    CHECK(x.batches.size()  == y.batches.size());
    CHECK(std::memcmp(x.vertices.data(), y.vertices.data(),
                      x.vertices.size() * sizeof(x.vertices[0])) == 0);
}

static void test_neutralidad_de_las_animaciones()
{
    // The one on the left has a clock (updateInput every frame) but no
    // animation; the one on the right does not even call it. Same bytes.
    UiCanvas conReloj, sinReloj;

    for (UiCanvas* cv : { &conReloj, &sinReloj })
    {
        UiElement& p = cv->root().add("p");
        p.position     = glm::vec2(70.0f, 90.0f);
        p.size         = glm::vec2(210.0f, 130.0f);
        p.clipChildren = true;

        UiElement& h = p.add("h");
        h.position = glm::vec2(-15.0f, 25.0f);
        h.size     = glm::vec2(180.0f, 60.0f);
        h.color    = glm::vec4(0.4f, 0.6f, 0.8f, 0.9f);
        h.opacity  = 0.7f;
    }

    avanzaReloj(conReloj, 0.0f);
    avanzaReloj(conReloj, 1.7f);
    avanzaReloj(conReloj, 4.2f);

    UiDrawData a, b;
    conReloj.buildDrawData(kW, kH, a);
    sinReloj.buildDrawData(kW, kH, b);

    CHECK(!a.vertices.empty());
    CHECK(a.vertices.size() == b.vertices.size());
    CHECK(a.indices.size()  == b.indices.size());
    CHECK(a.batches.size()  == b.batches.size());
    CHECK(std::memcmp(a.vertices.data(), b.vertices.data(),
                      a.vertices.size() * sizeof(a.vertices[0])) == 0);
    CHECK(std::memcmp(a.indices.data(), b.indices.data(),
                      a.indices.size() * sizeof(a.indices[0])) == 0);
}

// ── Focus navigation (gamepad) ──────────────────────────────────────────────
// Four neighbors around a central one, with widths != heights and not forming a
// grid: the centers are misaligned on purpose so that the transverse axis
// penalty has something to do.
struct EscenaNav
{
    UiCanvas   canvas;
    UiElement* medio  = nullptr;
    UiElement* arriba = nullptr;
    UiElement* abajo  = nullptr;
    UiElement* izq    = nullptr;
    UiElement* der    = nullptr;
};

static void montarNav(EscenaNav& e)
{
    auto nodo = [&](const char* nombre, glm::vec2 pos, glm::vec2 size) -> UiElement*
    {
        UiElement& n = e.canvas.root().add(nombre);
        n.position   = pos;
        n.size       = size;
        n.focusable  = true;
        return &n;
    };

    e.medio  = nodo("medio",  {300.0f, 300.0f}, {120.0f, 40.0f});   // center (360, 320)
    e.arriba = nodo("arriba", {300.0f, 150.0f}, {100.0f, 60.0f});   // center (350, 180)
    e.abajo  = nodo("abajo",  {320.0f, 420.0f}, { 90.0f, 50.0f});   // center (365, 445)
    e.izq    = nodo("izq",    {100.0f, 290.0f}, { 80.0f, 70.0f});   // center (140, 325)
    e.der    = nodo("der",    {520.0f, 310.0f}, {110.0f, 30.0f});   // center (575, 325)

    colocar(e.canvas);
}

// Next and Previous are the Tab traversal: pre-order and WITH wraparound.
static void test_nav_next_y_previous_dan_la_vuelta()
{
    UiCanvas canvas;

    auto nodo = [&](const char* nombre, glm::vec2 pos, glm::vec2 size, bool foco) -> UiElement&
    {
        UiElement& n = canvas.root().add(nombre);
        n.position   = pos;
        n.size       = size;
        n.focusable  = foco;
        return n;
    };

    UiElement& a     = nodo("a",     { 20.0f,  20.0f}, {120.0f, 40.0f}, true);
    /*      */        nodo("sinFoco",{170.0f,  25.0f}, { 60.0f, 90.0f}, false);
    UiElement& b     = nodo("b",     {200.0f,  90.0f}, { 80.0f, 60.0f}, true);
    UiElement& oculto= nodo("oculto",{330.0f, 120.0f}, { 70.0f, 35.0f}, true);
    UiElement& apaga = nodo("apagado",{40.0f, 200.0f}, {150.0f, 25.0f}, true);
    UiElement& c     = nodo("c",     { 60.0f, 300.0f}, {140.0f, 50.0f}, true);

    oculto.visible = false;
    apaga.enabled  = false;

    colocar(canvas);
    canvas.setFocus(&a);

    CHECK(canvas.navigate(UiNavDir::Next) == true);
    CHECK(canvas.focused() == &b);
    CHECK(canvas.navigate(UiNavDir::Next) == true);
    CHECK(canvas.focused() == &c);          // it skipped the invisible one and the disabled one
    CHECK(canvas.navigate(UiNavDir::Next) == true);
    CHECK(canvas.focused() == &a);          // and wrapped around

    // Previous is exactly the same in the opposite direction.
    CHECK(canvas.navigate(UiNavDir::Previous) == true);
    CHECK(canvas.focused() == &c);
    CHECK(canvas.navigate(UiNavDir::Previous) == true);
    CHECK(canvas.focused() == &b);
    CHECK(canvas.navigate(UiNavDir::Previous) == true);
    CHECK(canvas.focused() == &a);
}

// The four neighbors, each on its own side. And in Right and Down the aligned
// candidate wins over another that is CLOSER but diagonal.
static void test_nav_direccional_elige_al_vecino_de_ese_lado()
{
    EscenaNav e;
    montarNav(e);

    struct Caso { UiNavDir dir; UiElement** esperado; };
    UiElement* arriba = e.arriba;
    UiElement* abajo  = e.abajo;
    UiElement* izq    = e.izq;
    UiElement* der    = e.der;

    const Caso casos[] = {
        {UiNavDir::Up,    &arriba},
        {UiNavDir::Down,  &abajo},
        {UiNavDir::Left,  &izq},
        {UiNavDir::Right, &der},
    };

    for (const Caso& caso : casos)
    {
        e.canvas.setFocus(e.medio);
        CHECK(e.canvas.navigate(caso.dir) == true);
        CHECK(e.canvas.focused() == *caso.esperado);
    }
}

// Aligned at 160 px beats a diagonal at 106 px: the transverse distance weighs.
static void test_nav_alineado_gana_al_diagonal_mas_cercano()
{
    UiCanvas canvas;

    auto nodo = [&](const char* nombre, glm::vec2 pos, glm::vec2 size) -> UiElement&
    {
        UiElement& n = canvas.root().add(nombre);
        n.position   = pos;
        n.size       = size;
        n.focusable  = true;
        return n;
    };

    UiElement& medio    = nodo("medio",    {300.0f, 300.0f}, {120.0f, 40.0f});  // (360, 320)
    UiElement& diagonal = nodo("diagonal", {390.0f, 210.0f}, { 80.0f, 60.0f});  // (430, 240)
    UiElement& alineado = nodo("alineado", {460.0f, 290.0f}, {120.0f, 60.0f});  // (520, 320)

    colocar(canvas);
    canvas.setFocus(&medio);

    CHECK(canvas.navigate(UiNavDir::Right) == true);
    CHECK(canvas.focused() == &alineado);
    CHECK(diagonal.focused == false);
}

// The directional does NOT wrap around: with nobody on that side the focus stays.
static void test_nav_sin_candidato_no_mueve_el_foco()
{
    EscenaNav e;
    montarNav(e);

    e.canvas.setFocus(e.arriba);                 // the topmost of all
    CHECK(e.canvas.navigate(UiNavDir::Up) == false);
    CHECK(e.canvas.focused() == e.arriba);
    CHECK(e.arriba->focused == true);
}

// The overrides rule over the geometry, even pointing the opposite way.
static void test_nav_overrides_ganan_a_la_geometria()
{
    EscenaNav e;
    montarNav(e);

    e.medio->navUp   = e.abajo;                  // up leads DOWN
    e.medio->navLeft = e.der;                    // left leads to the RIGHT

    e.canvas.setFocus(e.medio);
    CHECK(e.canvas.navigate(UiNavDir::Up) == true);
    CHECK(e.canvas.focused() == e.abajo);

    e.canvas.setFocus(e.medio);
    CHECK(e.canvas.navigate(UiNavDir::Left) == true);
    CHECK(e.canvas.focused() == e.der);

    // And without an override it is still decided by geometry.
    e.canvas.setFocus(e.medio);
    CHECK(e.canvas.navigate(UiNavDir::Down) == true);
    CHECK(e.canvas.focused() == e.abajo);
}

// Without a previous focus you enter through the first focusable in pre-order, whichever
// navigation direction comes.
static void test_nav_sin_foco_toma_el_primero_en_preorden()
{
    UiCanvas canvas;

    UiElement& tapa = canvas.root().add("tapa");   // first of the tree, NOT focusable
    tapa.position   = {10.0f, 10.0f};
    tapa.size       = {200.0f, 30.0f};

    UiElement& primero = canvas.root().add("primero");
    primero.position  = {10.0f, 60.0f};
    primero.size      = {120.0f, 45.0f};
    primero.focusable = true;

    UiElement& otro = canvas.root().add("otro");
    otro.position  = {10.0f, 130.0f};
    otro.size      = {90.0f, 70.0f};
    otro.focusable = true;

    colocar(canvas);

    CHECK(canvas.focused() == nullptr);
    CHECK(canvas.navigate(UiNavDir::Down) == true);
    CHECK(canvas.focused() == &primero);
}

// Without buildDrawData there are no rects: the directional finds nobody (same as
// the hit test) but Next, which does not look at geometry, still works.
static void test_nav_sin_build_draw_data_solo_falla_la_direccional()
{
    UiCanvas canvas;

    UiElement& a = canvas.root().add("a");
    a.position   = {20.0f, 20.0f};
    a.size       = {120.0f, 40.0f};
    a.focusable  = true;

    UiElement& b = canvas.root().add("b");
    b.position   = {300.0f, 25.0f};
    b.size       = {80.0f, 60.0f};
    b.focusable  = true;

    canvas.setFocus(&a);                          // without laying out(canvas)

    CHECK(canvas.navigate(UiNavDir::Right) == false);
    CHECK(canvas.focused() == &a);

    CHECK(canvas.navigate(UiNavDir::Next) == true);
    CHECK(canvas.focused() == &b);
}

// ONCE each, and on the right one.
static void test_nav_dispara_blur_y_focus_una_sola_vez()
{
    EscenaNav e;
    montarNav(e);

    int blurMedio = 0;
    int focoMedio = 0;
    int blurDer   = 0;
    int focoDer   = 0;

    e.canvas.setFocus(e.medio);                   // before wiring: it does not count

    e.medio->onBlur  = [&](UiEvent&) { ++blurMedio; };
    e.medio->onFocus = [&](UiEvent&) { ++focoMedio; };
    e.der->onBlur    = [&](UiEvent&) { ++blurDer; };
    e.der->onFocus   = [&](UiEvent&) { ++focoDer; };

    CHECK(e.canvas.navigate(UiNavDir::Right) == true);
    CHECK(e.canvas.focused() == e.der);
    CHECK(blurMedio == 1);
    CHECK(focoDer   == 1);
    CHECK(focoMedio == 0);
    CHECK(blurDer   == 0);

    // An attempt that does not move the focus fires nothing.
    CHECK(e.canvas.navigate(UiNavDir::Right) == false);
    CHECK(blurMedio == 1);
    CHECK(focoDer   == 1);
    CHECK(blurDer   == 0);
}

// The same sequence twice ends up in the same place.
static void test_nav_determinismo_de_la_secuencia()
{
    EscenaNav e;
    montarNav(e);

    const UiNavDir secuencia[] = {
        UiNavDir::Right, UiNavDir::Up, UiNavDir::Down, UiNavDir::Left,
        UiNavDir::Next,  UiNavDir::Down, UiNavDir::Previous
    };

    auto correr = [&]() -> UiElement*
    {
        e.canvas.setFocus(e.medio);
        for (UiNavDir dir : secuencia) e.canvas.navigate(dir);
        return e.canvas.focused();
    };

    UiElement* primera = correr();
    UiElement* segunda = correr();
    CHECK(primera != nullptr);
    CHECK(primera == segunda);
}

// Navigating does NOT touch the drawing: same bytes of vertices and indices and same
// batches as an identical canvas that nobody navigated.
static void test_neutralidad_de_la_navegacion()
{
    EscenaNav navegado;
    montarNav(navegado);
    navegado.medio->navUp = navegado.abajo;
    navegado.canvas.setFocus(navegado.medio);
    navegado.canvas.navigate(UiNavDir::Right);
    navegado.canvas.navigate(UiNavDir::Up);
    navegado.canvas.navigate(UiNavDir::Next);
    navegado.canvas.navigate(UiNavDir::Left);

    EscenaNav quieto;
    montarNav(quieto);

    UiDrawData a;
    UiDrawData b;
    navegado.canvas.buildDrawData(kEvW, kEvH, a);
    quieto.canvas.buildDrawData(kEvW, kEvH, b);

    CHECK(a.vertices.size() == b.vertices.size());
    CHECK(a.indices.size()  == b.indices.size());
    CHECK(a.batches.size()  == b.batches.size());
    if (a.vertices.size() != b.vertices.size()) return;
    if (a.indices.size()  != b.indices.size())  return;

    CHECK(std::memcmp(a.vertices.data(), b.vertices.data(),
                      a.vertices.size() * sizeof(a.vertices[0])) == 0);
    CHECK(std::memcmp(a.indices.data(), b.indices.data(),
                      a.indices.size() * sizeof(a.indices[0])) == 0);
}

// ── Canvas resolution ───────────────────────────────────────────────────
// The reference (1920x1080) is NOT an exact multiple of the render (800x480):
// 800/1920 = 0.416666... and 480/1080 = 0.444444..., so no ratio comes out
// round, the two axes give different numbers and one axis swapped for the other
// shows. The panel is neither square nor at the origin.
static UiElement& montaResolucion(UiCanvas& canvas)
{
    UiElement& panel = canvas.root().add("Panel");
    panel.position = {30.0f, 40.0f};
    panel.size     = {200.0f, 100.0f};
    return panel;
}

static glm::vec2 quadPos(const UiDrawData& d)
{
    return {d.vertices[0].pos.x, d.vertices[0].pos.y};
}

static glm::vec2 quadSize(const UiDrawData& d)
{
    return {d.vertices[2].pos.x - d.vertices[0].pos.x,
            d.vertices[2].pos.y - d.vertices[0].pos.y};
}

// The scale multiplies the WHOLE rect (local position included) but does not move
// the origin: without safe area or aspect ratio the usable area still starts at 0.
static void test_resolucion_constant_pixel_size_escala_el_quad()
{
    UiCanvas canvas;
    montaResolucion(canvas);
    canvas.scaleMode   = UiScaleMode::ConstantPixelSize;
    canvas.scaleFactor = 2.0f;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;

    CHECK(nearly(canvas.uiScale(), 2.0f));
    CHECK(nearly(canvas.uiOrigin().x, 0.0f));
    CHECK(nearly(canvas.uiOrigin().y, 0.0f));
    // The tree is resolved in reference units: with scale 2 the usable area
    // measures half in those units.
    CHECK(nearly(canvas.referenceSize().x, 400.0f));
    CHECK(nearly(canvas.referenceSize().y, 240.0f));

    // 30*2 = 60 ; 40*2 = 80. Without scaling the position would come out (30,40).
    CHECK(nearly(quadPos(data).x, 60.0f));
    CHECK(nearly(quadPos(data).y, 80.0f));
    CHECK(nearly(quadSize(data).x, 400.0f));
    CHECK(nearly(quadSize(data).y, 200.0f));

    // And the root scissor is still the whole render.
    CHECK(data.batches.size() == 1);
    if (data.batches.empty()) return;
    CHECK(data.batches[0].scissor.x == 0 && data.batches[0].scissor.y == 0);
    CHECK(data.batches[0].scissor.width == kW && data.batches[0].scissor.height == kH);
}

// match 0 follows the width, match 1 the height, and 0.5 falls BETWEEN the two. With the
// arithmetic mean the 0.5 would come out different from the logarithmic lerp, so the
// exact number is also checked.
static void test_resolucion_scale_with_screen_size_match()
{
    const float rx = (float)kW / 1920.0f;   // 0,4166…
    const float ry = (float)kH / 1080.0f;   // 0,4444…
    CHECK(rx < ry);                         // if not, the test distinguishes nothing

    struct Caso { UiScreenMatch modo; float match; float esperado; };
    const Caso casos[] = {
        {UiScreenMatch::MatchWidthOrHeight, 0.0f, rx},
        {UiScreenMatch::MatchWidthOrHeight, 1.0f, ry},
        {UiScreenMatch::MatchWidthOrHeight, 0.5f, std::sqrt(rx * ry)},
        {UiScreenMatch::Expand,             0.5f, rx},   // the SMALLER
        {UiScreenMatch::Shrink,             0.5f, ry},   // the LARGER
    };

    for (const Caso& c : casos)
    {
        UiCanvas canvas;
        montaResolucion(canvas);
        canvas.scaleMode           = UiScaleMode::ScaleWithScreenSize;
        canvas.referenceResolution = {1920.0f, 1080.0f};
        canvas.screenMatch         = c.modo;
        canvas.matchWidthOrHeight  = c.match;

        UiDrawData data;
        canvas.buildDrawData(kW, kH, data);

        CHECK(nearly(canvas.uiScale(), c.esperado));
        CHECK(data.vertices.size() == 4);
        if (data.vertices.size() != 4) continue;
        CHECK(nearly(quadSize(data).x, 200.0f * c.esperado));
        CHECK(nearly(quadSize(data).y, 100.0f * c.esperado));
    }

    // The 0.5 falls STRICTLY between the two extremes, not on top of either.
    UiCanvas medio;
    montaResolucion(medio);
    medio.scaleMode          = UiScaleMode::ScaleWithScreenSize;
    medio.matchWidthOrHeight = 0.5f;
    UiDrawData data;
    medio.buildDrawData(kW, kH, data);
    CHECK(medio.uiScale() > rx && medio.uiScale() < ry);

    // And scaleFactor also multiplies this mode.
    medio.scaleFactor = 3.0f;
    medio.buildDrawData(kW, kH, data);
    CHECK(nearly(medio.uiScale(), std::sqrt(rx * ry) * 3.0f));
}

static void test_resolucion_constant_physical_size()
{
    // DPI double the reference: scale 2.
    UiCanvas doble;
    montaResolucion(doble);
    doble.scaleMode    = UiScaleMode::ConstantPhysicalSize;
    doble.screenDpi    = 192.0f;
    doble.referenceDpi = 96.0f;

    UiDrawData data;
    doble.buildDrawData(kW, kH, data);
    CHECK(nearly(doble.uiScale(), 2.0f));
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() == 4) CHECK(nearly(quadSize(data).x, 400.0f));

    // Unknown DPI: the fallback rules, and it is set to a value that is NOT the
    // reference one so that an ignored fallback does not pass all the same.
    UiCanvas desconocido;
    montaResolucion(desconocido);
    desconocido.scaleMode    = UiScaleMode::ConstantPhysicalSize;
    desconocido.screenDpi    = 0.0f;
    desconocido.fallbackDpi  = 120.0f;
    desconocido.referenceDpi = 96.0f;
    desconocido.buildDrawData(kW, kH, data);
    CHECK(nearly(desconocido.uiScale(), 1.25f));

    // Negative DPI: it is "unknown" just like 0, it falls to the default fallback
    // (96, the same as the reference) and gives scale 1 without NaN or an upside-down quad.
    UiCanvas negativo;
    montaResolucion(negativo);
    negativo.scaleMode = UiScaleMode::ConstantPhysicalSize;
    negativo.screenDpi = -50.0f;
    negativo.buildDrawData(kW, kH, data);
    CHECK(nearly(negativo.uiScale(), 1.0f));
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() == 4)
    {
        CHECK(nearly(quadPos(data).x, 30.0f));
        CHECK(quadSize(data).x > 0.0f);
    }
}

// The insets move the origin and shrink the usable area, and the root scissor leaves
// out what falls inside the inset.
static void test_resolucion_safe_area()
{
    UiCanvas canvas;
    montaResolucion(canvas);
    canvas.safeArea.left   = 40.0f;   // all four different from each other
    canvas.safeArea.top    = 24.0f;
    canvas.safeArea.right  = 16.0f;
    canvas.safeArea.bottom = 8.0f;

    UiDrawData data;
    canvas.buildDrawData(kW, kH, data);

    CHECK(nearly(canvas.uiScale(), 1.0f));
    CHECK(nearly(canvas.uiOrigin().x, 40.0f));
    CHECK(nearly(canvas.uiOrigin().y, 24.0f));
    CHECK(nearly(canvas.referenceSize().x, (float)kW - 56.0f));   // 800-40-16
    CHECK(nearly(canvas.referenceSize().y, (float)kH - 32.0f));   // 480-24-8

    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() != 4) return;
    // The panel moves with the origin, but does not change size.
    CHECK(nearly(quadPos(data).x, 70.0f));    // 40 + 30
    CHECK(nearly(quadPos(data).y, 64.0f));    // 24 + 40
    CHECK(nearly(quadSize(data).x, 200.0f));

    CHECK(!data.batches.empty());
    if (data.batches.empty()) return;
    const UiScissor& s = data.batches[0].scissor;
    CHECK(s.x == 40 && s.y == 24);
    CHECK(s.width == kW - 56 && s.height == kH - 32);

    // An element placed at a negative falls INSIDE the inset: it is emitted, but the
    // root scissor leaves it off the usable screen.
    UiCanvas fuera;
    UiElement& metido = fuera.root().add("Metido");
    metido.position = {-35.0f, -20.0f};
    metido.size     = {30.0f, 10.0f};
    fuera.safeArea.left = 40.0f;
    fuera.safeArea.top  = 24.0f;

    UiDrawData d2;
    fuera.buildDrawData(kW, kH, d2);
    CHECK(d2.vertices.size() == 4);
    CHECK(!d2.batches.empty());
    if (d2.vertices.size() != 4 || d2.batches.empty()) return;
    CHECK(nearly(quadPos(d2).x, 5.0f));    // 40 - 35
    CHECK(nearly(quadPos(d2).y, 4.0f));    // 24 - 20
    CHECK(d2.batches[0].scissor.x == 40 && d2.batches[0].scissor.y == 24);
    // Entirely above and to the left of the scissor: not a pixel is seen.
    CHECK(quadPos(d2).x + quadSize(d2).x <= (float)d2.batches[0].scissor.x);
    CHECK(quadPos(d2).y + quadSize(d2).y <= (float)d2.batches[0].scissor.y);
}

// 16/9 on a 4:3 render puts bars ABOVE and BELOW; 1:1 on a landscape render
// puts them on the SIDES. In both cases the usable area ends up centered.
static void test_resolucion_aspect_ratio()
{
    // 4:3 with 16/9 requested: there is height left over.
    UiCanvas letterbox;
    montaResolucion(letterbox);
    letterbox.aspectRatio = 16.0f / 9.0f;

    UiDrawData data;
    letterbox.buildDrawData(800, 600, data);

    const float utilAlto = 800.0f * 9.0f / 16.0f;    // 450
    CHECK(nearly(letterbox.uiOrigin().x, 0.0f));
    CHECK(nearly(letterbox.uiOrigin().y, (600.0f - utilAlto) * 0.5f));   // 75
    CHECK(nearly(letterbox.referenceSize().x, 800.0f));
    CHECK(nearly(letterbox.referenceSize().y, utilAlto));
    CHECK(!data.batches.empty());
    if (!data.batches.empty())
    {
        CHECK(data.batches[0].scissor.x == 0 && data.batches[0].scissor.y == 75);
        CHECK(data.batches[0].scissor.width == 800 && data.batches[0].scissor.height == 450);
    }
    CHECK(data.vertices.size() == 4);
    if (data.vertices.size() == 4) CHECK(nearly(quadPos(data).y, 115.0f));   // 75 + 40

    // Landscape with 1:1 requested: there is width left over.
    UiCanvas pillarbox;
    montaResolucion(pillarbox);
    pillarbox.aspectRatio = 1.0f;

    UiDrawData d2;
    pillarbox.buildDrawData(kW, kH, d2);

    CHECK(nearly(pillarbox.uiOrigin().x, ((float)kW - (float)kH) * 0.5f));   // 160
    CHECK(nearly(pillarbox.uiOrigin().y, 0.0f));
    CHECK(nearly(pillarbox.referenceSize().x, (float)kH));
    CHECK(nearly(pillarbox.referenceSize().y, (float)kH));
    CHECK(!d2.batches.empty());
    if (!d2.batches.empty())
    {
        CHECK(d2.batches[0].scissor.x == 160 && d2.batches[0].scissor.y == 0);
        CHECK(d2.batches[0].scissor.width == kH && d2.batches[0].scissor.height == kH);
    }
    CHECK(d2.vertices.size() == 4);
    if (d2.vertices.size() == 4) CHECK(nearly(quadPos(d2).x, 190.0f));   // 160 + 30

    // And the aspect ratio is applied AFTER the safe area: over the already
    // trimmed area, not over the whole render.
    UiCanvas encadenado;
    montaResolucion(encadenado);
    encadenado.safeArea.left = 200.0f;   // usable area 600x480
    encadenado.aspectRatio   = 1.0f;     // trims to 480x480, centered in it
    UiDrawData d3;
    encadenado.buildDrawData(kW, kH, d3);
    CHECK(nearly(encadenado.uiOrigin().x, 200.0f + (600.0f - 480.0f) * 0.5f));   // 260
    CHECK(nearly(encadenado.referenceSize().x, 480.0f));
}

// The SAME element in reference units ends up at different pixels depending on
// the scale, and the hit test hits with the point in PIXELS in both cases.
static void test_resolucion_hit_test_en_pixeles()
{
    UiCanvas uno;
    UiElement& pUno = montaResolucion(uno);
    pUno.raycastTarget = true;
    UiDrawData data;
    uno.buildDrawData(kW, kH, data);

    UiCanvas dos;
    UiElement& pDos = montaResolucion(dos);
    pDos.raycastTarget = true;
    dos.scaleFactor = 2.0f;
    UiDrawData d2;
    dos.buildDrawData(kW, kH, d2);

    // (100,60) is inside the rect at scale 1 (30..230, 40..140) and OUTSIDE at
    // scale 2 (60..460, 80..280).
    CHECK(uno.hitTest({100.0f, 60.0f}) == &pUno);
    CHECK(dos.hitTest({100.0f, 60.0f}) == nullptr);

    // (200,120) is inside at scale 2 and also at scale 1: the point that
    // really separates them is the one above.
    CHECK(dos.hitTest({200.0f, 120.0f}) == &pDos);

    // (400,200): inside at scale 2, outside at scale 1.
    CHECK(dos.hitTest({400.0f, 200.0f}) == &pDos);
    CHECK(uno.hitTest({400.0f, 200.0f}) == nullptr);

    // With safe area the hit test moves with the origin.
    UiCanvas movido;
    UiElement& pMovido = montaResolucion(movido);
    pMovido.raycastTarget = true;
    movido.safeArea.left = 40.0f;
    movido.safeArea.top  = 24.0f;
    UiDrawData d3;
    movido.buildDrawData(kW, kH, d3);
    CHECK(movido.hitTest({100.0f, 60.0f}) == nullptr);          // no longer reaches
    CHECK(movido.hitTest({140.0f, 84.0f}) == &pMovido);         // 100+40, 60+24
}

// Two consecutive builds with the same configuration: the same bytes. Without this,
// module state that stayed on from one build to the next would go
// unnoticed.
static void test_resolucion_determinismo()
{
    UiCanvas canvas;
    montaResolucion(canvas);
    canvas.scaleMode           = UiScaleMode::ScaleWithScreenSize;
    canvas.matchWidthOrHeight  = 0.25f;
    canvas.safeArea.left       = 17.0f;
    canvas.safeArea.bottom     = 9.0f;
    canvas.aspectRatio         = 4.0f / 3.0f;

    UiDrawData a;
    UiDrawData b;
    canvas.buildDrawData(kW, kH, a);
    const float escalaA = canvas.uiScale();
    const glm::vec2 origenA = canvas.uiOrigin();
    canvas.buildDrawData(kW, kH, b);

    CHECK(escalaA == canvas.uiScale());
    CHECK(origenA == canvas.uiOrigin());
    CHECK(a.vertices.size() == b.vertices.size());
    CHECK(a.indices.size()  == b.indices.size());
    CHECK(a.batches.size()  == b.batches.size());
    if (a.vertices.size() != b.vertices.size() || a.indices.size() != b.indices.size()) return;
    CHECK(std::memcmp(a.vertices.data(), b.vertices.data(),
                      a.vertices.size() * sizeof(a.vertices[0])) == 0);
    CHECK(std::memcmp(a.indices.data(), b.indices.data(),
                      a.indices.size() * sizeof(a.indices[0])) == 0);
}

// With the default values NOT A SINGLE BYTE changes: same vertices, same
// indices and same batches as before the feature. It is the condition that makes
// the tests above keep passing without touching a single expected value.
static void test_neutralidad_de_la_resolucion()
{
    UiCanvas base;
    UiElement& panel = base.root().add("Panel");
    panel.position = {31.0f, 47.0f};
    panel.size     = {123.0f, 57.0f};
    UiElement& hijo = panel.add("Hijo");
    hijo.position     = {5.0f, 9.0f};
    hijo.size         = {40.0f, 20.0f};
    panel.clipChildren = true;
    panel.maskEnabled  = true;
    panel.maskInsetLeft = 7.0f;
    panel.maskInsetTop  = 3.0f;

    UiDrawData antes;
    base.buildDrawData(kW, kH, antes);

    // The defaults, written by hand: if any of them were not neutral, this
    // would come out different from the canvas that has not touched them.
    UiCanvas igual;
    UiElement& panel2 = igual.root().add("Panel");
    panel2.position = {31.0f, 47.0f};
    panel2.size     = {123.0f, 57.0f};
    UiElement& hijo2 = panel2.add("Hijo");
    hijo2.position     = {5.0f, 9.0f};
    hijo2.size         = {40.0f, 20.0f};
    panel2.clipChildren = true;
    panel2.maskEnabled  = true;
    panel2.maskInsetLeft = 7.0f;
    panel2.maskInsetTop  = 3.0f;

    igual.scaleMode          = UiScaleMode::ConstantPixelSize;
    igual.scaleFactor        = 1.0f;
    igual.safeArea           = UiSafeArea{};
    igual.aspectRatio        = 0.0f;

    UiDrawData despues;
    igual.buildDrawData(kW, kH, despues);

    CHECK(antes.vertices.size() == despues.vertices.size());
    CHECK(antes.indices.size()  == despues.indices.size());
    CHECK(antes.batches.size()  == despues.batches.size());
    if (antes.vertices.size() != despues.vertices.size()) return;
    if (antes.indices.size()  != despues.indices.size())  return;
    CHECK(std::memcmp(antes.vertices.data(), despues.vertices.data(),
                      antes.vertices.size() * sizeof(antes.vertices[0])) == 0);
    CHECK(std::memcmp(antes.indices.data(), despues.indices.data(),
                      antes.indices.size() * sizeof(antes.indices[0])) == 0);

    // And the default transform is the exact identity, not "almost".
    CHECK(base.uiScale() == 1.0f);
    CHECK(base.uiOrigin() == glm::vec2(0.0f, 0.0f));
    CHECK(base.referenceSize() == glm::vec2((float)kW, (float)kH));
    // The mask still falls EXACTLY where it fell: the neutral scale does not
    // add a pixel more or less to it.
    CHECK(!antes.batches.empty());
    if (!antes.batches.empty())
    {
        CHECK(antes.batches[0].scissor.x == 38 && antes.batches[0].scissor.y == 50);
        CHECK(antes.batches[0].scissor.width == 116 && antes.batches[0].scissor.height == 54);
    }

    // And with no mask in between, the root scissor is still the whole render.
    UiCanvas raso;
    raso.root().add("Panel").size = {50.0f, 20.0f};
    UiDrawData rasoData;
    raso.buildDrawData(kW, kH, rasoData);
    CHECK(rasoData.batches.size() == 1);
    if (rasoData.batches.size() == 1)
    {
        CHECK(rasoData.batches[0].scissor.x == 0 && rasoData.batches[0].scissor.y == 0);
        CHECK(rasoData.batches[0].scissor.width == kW && rasoData.batches[0].scissor.height == kH);
    }
}

// ── Vertex cache and dirty flags ─────────────────────────────────────────
// Everything here is measured with the engine's own counters (the canvas's rebuiltNodes and
// the per-node rebuildCount), NEVER with the clock: the number comes out the same on
// any machine and in any configuration.

static size_t cuentaNodos(const UiElement& n)
{
    size_t total = 1;   // the root is also walked and also emits
    for (const auto& hijo : n.children()) total += cuentaNodos(*hijo);
    return total;
}

// Byte for byte: vertices, indices and batches. No tolerances.
static bool mismosBytes(const UiDrawData& a, const UiDrawData& b)
{
    if (a.vertices.size() != b.vertices.size()) return false;
    if (a.indices.size()  != b.indices.size())  return false;
    if (a.batches.size()  != b.batches.size())  return false;

    if (!a.vertices.empty() &&
        std::memcmp(a.vertices.data(), b.vertices.data(),
                    a.vertices.size() * sizeof(a.vertices[0])) != 0) return false;
    if (!a.indices.empty() &&
        std::memcmp(a.indices.data(), b.indices.data(),
                    a.indices.size() * sizeof(a.indices[0])) != 0) return false;

    for (size_t i = 0; i < a.batches.size(); ++i)
    {
        if (a.batches[i].atlas != b.batches[i].atlas)             return false;
        if (!(a.batches[i].scissor == b.batches[i].scissor))      return false;
        if (a.batches[i].firstIndex != b.batches[i].firstIndex)   return false;
        if (a.batches[i].indexCount != b.batches[i].indexCount)   return false;
    }
    return true;
}

// A second build without touching ANYTHING re-emits not a single node and gives the same bytes.
static void test_cache_segundo_build_no_reemite_nada()
{
    UiCanvas cv;
    UiElement& panel = cv.root().add("panel");
    panel.position = glm::vec2(40.0f, 25.0f);
    panel.size     = glm::vec2(200.0f, 120.0f);

    UiElement& a = panel.add("a");
    a.position = glm::vec2(10.0f, 10.0f);
    a.size     = glm::vec2(50.0f, 30.0f);
    UiElement& b = panel.add("b");
    b.position = glm::vec2(80.0f, 10.0f);
    b.size     = glm::vec2(50.0f, 30.0f);

    UiDrawData uno;
    cv.buildDrawData(kW, kH, uno);
    // Everything is born dirty: the first build emits the whole tree.
    CHECK(cv.rebuiltNodes() == cuentaNodos(cv.root()));

    UiDrawData dos;
    cv.buildDrawData(kW, kH, dos);
    CHECK(cv.rebuiltNodes() == 0);
    CHECK(mismosBytes(uno, dos));
    CHECK(!uno.vertices.empty());
}

// Moving a leaf re-emits THAT leaf and its chain of parents (Transform goes up), but
// not its siblings. If Transform also went down from the parent, the sibling
// would fall with it and this test would catch it.
static void test_cache_mover_una_hoja_no_reemite_hermanos()
{
    UiCanvas cv;
    UiElement& panel = cv.root().add("panel");
    panel.position = glm::vec2(40.0f, 25.0f);
    panel.size     = glm::vec2(200.0f, 120.0f);

    UiElement& a = panel.add("a");
    a.position = glm::vec2(10.0f, 10.0f);
    a.size     = glm::vec2(50.0f, 30.0f);
    UiElement& b = panel.add("b");
    b.position = glm::vec2(80.0f, 10.0f);
    b.size     = glm::vec2(50.0f, 30.0f);

    UiDrawData d;
    cv.buildDrawData(kW, kH, d);
    cv.buildDrawData(kW, kH, d);
    CHECK(cv.rebuiltNodes() == 0);

    const uint32_t aAntes     = a.rebuildCount;
    const uint32_t bAntes     = b.rebuildCount;
    const uint32_t panelAntes = panel.rebuildCount;

    a.position.x += 17.0f;
    a.markDirty(UiElement::DirtyTransform);

    UiDrawData movido;
    cv.buildDrawData(kW, kH, movido);

    CHECK(a.rebuildCount     == aAntes + 1);
    CHECK(b.rebuildCount     == bAntes);         // the sibling is NOT re-emitted
    CHECK(panel.rebuildCount == panelAntes + 1); // the parent is: Transform goes up
    CHECK(cv.rebuiltNodes()  == 3);              // root + panel + a

    // And the sibling is still exactly where it was, byte for byte.
    if (movido.vertices.size() == d.vertices.size() && movido.vertices.size() >= 12)
    {
        CHECK(std::memcmp(&movido.vertices[8], &d.vertices[8], 4 * sizeof(UiVertex)) == 0);
        CHECK(!nearly(movido.vertices[4].pos.x, d.vertices[4].pos.x));
    }
}

// Changing the color marks ONLY Material: it does not leave the node and relocates nothing. It is
// checked for real by moving the parent WITHOUT marking it: if the rect were recomputed,
// the child would go along with it.
static void test_cache_color_es_material_y_no_recoloca()
{
    UiCanvas cv;
    UiElement& panel = cv.root().add("panel");
    panel.position = glm::vec2(40.0f, 25.0f);
    panel.size     = glm::vec2(200.0f, 120.0f);

    UiElement& hijo = panel.add("hijo");
    hijo.position = glm::vec2(10.0f, 10.0f);
    hijo.size     = glm::vec2(50.0f, 30.0f);

    UiDrawData uno;
    cv.buildDrawData(kW, kH, uno);
    CHECK(uno.vertices.size() == 8);
    if (uno.vertices.size() != 8) return;

    const glm::vec2 rectAntes = hijo.screenPos;

    // Write WITHOUT marking: by contract it is not seen. It is here precisely so that
    // the child's rect would change if someone recomputed it.
    panel.position = glm::vec2(140.0f, 25.0f);

    hijo.color = glm::vec4(0.25f, 0.5f, 0.75f, 1.0f);
    hijo.markDirty(UiElement::DirtyMaterial);
    CHECK((hijo.dirty & (UiElement::DirtyTransform | UiElement::DirtyLayout)) == 0);

    UiDrawData dos;
    cv.buildDrawData(kW, kH, dos);

    CHECK(cv.rebuiltNodes() == 1);              // Material goes neither up nor down
    CHECK(hijo.screenPos.x == rectAntes.x);     // the rect was NOT recomputed
    CHECK(hijo.screenPos.y == rectAntes.y);
    CHECK(dos.vertices.size() == 8);
    if (dos.vertices.size() != 8) return;

    // The panel, intact; the child, same place and new color.
    CHECK(std::memcmp(dos.vertices.data(), uno.vertices.data(), 4 * sizeof(UiVertex)) == 0);
    CHECK(dos.vertices[4].pos.x == uno.vertices[4].pos.x);
    CHECK(dos.vertices[4].pos.y == uno.vertices[4].pos.y);
    CHECK(nearly(dos.vertices[4].color.r, 0.25f));
    CHECK(nearly(dos.vertices[4].color.b, 0.75f));
}

// Changing the size of a parent with layout re-emits ALL its descendants.
// An uncle from the other side of the tree does not find out.
static void test_cache_layout_del_padre_baja_pero_no_cruza()
{
    UiCanvas cv;

    UiElement& izq = cv.root().add("izq");
    izq.position   = glm::vec2(20.0f, 20.0f);
    izq.size       = glm::vec2(200.0f, 120.0f);
    izq.layoutMode = UiLayoutMode::Vertical;
    izq.spacing    = glm::vec2(0.0f, 5.0f);

    UiElement& h1 = izq.add("h1");
    h1.size = glm::vec2(60.0f, 20.0f);
    UiElement& nieto = h1.add("nieto");
    nieto.size = glm::vec2(10.0f, 10.0f);
    UiElement& h2 = izq.add("h2");
    h2.size = glm::vec2(60.0f, 20.0f);

    UiElement& tio = cv.root().add("tio");
    tio.position = glm::vec2(400.0f, 20.0f);
    tio.size     = glm::vec2(80.0f, 40.0f);
    UiElement& primo = tio.add("primo");
    primo.size = glm::vec2(20.0f, 20.0f);

    UiDrawData d;
    cv.buildDrawData(kW, kH, d);
    cv.buildDrawData(kW, kH, d);
    CHECK(cv.rebuiltNodes() == 0);

    const uint32_t h1Antes    = h1.rebuildCount;
    const uint32_t nietoAntes = nieto.rebuildCount;
    const uint32_t h2Antes    = h2.rebuildCount;
    const uint32_t tioAntes   = tio.rebuildCount;
    const uint32_t primoAntes = primo.rebuildCount;

    izq.size = glm::vec2(200.0f, 200.0f);
    izq.markDirty(UiElement::DirtyLayout);

    cv.buildDrawData(kW, kH, d);

    CHECK(h1.rebuildCount    == h1Antes + 1);
    CHECK(nieto.rebuildCount == nietoAntes + 1);   // Layout GOES DOWN to the grandchild
    CHECK(h2.rebuildCount    == h2Antes + 1);
    CHECK(tio.rebuildCount   == tioAntes);         // and does not cross to the other side
    CHECK(primo.rebuildCount == primoAntes);
    CHECK(cv.rebuiltNodes()  == 5);                // root + left + h1 + grandchild + h2
}

// Changing the render resolution or the canvas scale moves the placement of
// everybody: not a single cache is left standing.
static void test_cache_resolucion_y_escala_reemiten_todo()
{
    UiCanvas cv;
    UiElement& panel = cv.root().add("panel");
    panel.position = glm::vec2(40.0f, 25.0f);
    panel.size     = glm::vec2(200.0f, 120.0f);
    UiElement& hijo = panel.add("hijo");
    hijo.position = glm::vec2(10.0f, 10.0f);
    hijo.size     = glm::vec2(50.0f, 30.0f);

    const size_t nodos = cuentaNodos(cv.root());

    UiDrawData d;
    cv.buildDrawData(kW, kH, d);
    cv.buildDrawData(kW, kH, d);
    CHECK(cv.rebuiltNodes() == 0);

    cv.buildDrawData(kW + 40, kH, d);
    CHECK(cv.rebuiltNodes() == nodos);

    cv.buildDrawData(kW + 40, kH, d);
    CHECK(cv.rebuiltNodes() == 0);

    // The canvas scale is not a tree field: nobody marks it and even so
    // it has to invalidate everything.
    cv.scaleFactor = 2.0f;
    cv.buildDrawData(kW + 40, kH, d);
    CHECK(cv.rebuiltNodes() == nodos);

    cv.buildDrawData(kW + 40, kH, d);
    CHECK(cv.rebuiltNodes() == 0);
}

// An animation in progress dirties its node on every frame and NO other. Color goes
// through Material, which goes neither up nor down.
static void test_cache_animacion_ensucia_solo_su_nodo()
{
    UiCanvas cv;
    UiElement& a = cv.root().add("a");
    a.position = glm::vec2(30.0f, 30.0f);
    a.size     = glm::vec2(50.0f, 20.0f);
    UiElement& b = cv.root().add("b");
    b.position = glm::vec2(200.0f, 30.0f);
    b.size     = glm::vec2(50.0f, 20.0f);

    a.anim         = UiAnim::Color;
    a.animFrom     = glm::vec4(0.1f, 0.2f, 0.3f, 1.0f);
    a.animTo       = glm::vec4(0.9f, 0.8f, 0.7f, 1.0f);
    a.animDuration = 4.0f;
    a.animPlaying  = true;

    UiDrawData d;
    avanzaReloj(cv, 0.0f);
    cv.buildDrawData(kW, kH, d);

    const uint32_t bAntes = b.rebuildCount;

    for (int i = 1; i <= 3; ++i)
    {
        const uint32_t aAntes = a.rebuildCount;
        avanzaReloj(cv, (float)i * 0.5f);
        cv.buildDrawData(kW, kH, d);

        CHECK(cv.rebuiltNodes() == 1);
        CHECK(a.rebuildCount == aAntes + 1);
        CHECK(b.rebuildCount == bAntes);
    }

    // And once the animation stops, it stops dirtying altogether.
    a.animPlaying = false;
    avanzaReloj(cv, 3.0f);
    cv.buildDrawData(kW, kH, d);
    CHECK(cv.rebuiltNodes() == 0);
}

static void montaEscenaCompleta(UiCanvas& cv, UiFont& font, UiTextureAtlas& atlas)
{
    UiElement& panel = cv.root().add("panel");
    panel.position        = glm::vec2(40.0f, 25.0f);
    panel.size            = glm::vec2(320.0f, 200.0f);
    panel.clipChildren    = true;
    panel.maskInsetLeft   = 6.0f;
    panel.maskInsetTop    = 4.0f;
    panel.maskInsetRight  = 8.0f;
    panel.maskInsetBottom = 10.0f;

    Text& label = panel.add<Text>("txt");
    label.position = glm::vec2(10.0f, 10.0f);
    label.size     = glm::vec2(200.0f, 60.0f);
    label.font     = &font;
    label.fontSize = kBakeSize;
    label.text     = "ABC";

    Image& marco = panel.add<Image>("marco");
    marco.position     = glm::vec2(10.0f, 90.0f);
    marco.size         = glm::vec2(120.0f, 70.0f);
    marco.atlas        = &atlas;
    marco.sprite       = "botella";
    marco.mode         = UiImageMode::Sliced;
    marco.borderLeft   = 4.0f;
    marco.borderRight  = 6.0f;
    marco.borderTop    = 3.0f;
    marco.borderBottom = 9.0f;

    Button& btn = panel.add<Button>("btn");
    btn.position   = glm::vec2(150.0f, 90.0f);
    btn.size       = glm::vec2(90.0f, 40.0f);
    btn.transition = UiButtonTransition::ColorTint;
    btn.hoverColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
}

// Neutrality: scene with text, mask, button and sliced image. The canvas with
// the cache running gives the SAME bytes and the same batches as an identical one
// whose whole tree is dirtied by hand before every build.
static void test_cache_neutralidad_escena_completa()
{
    UiFont font;
    makeTestFont(font);
    UiTextureAtlas atlas = makeAtlas();

    UiCanvas conCache;
    UiCanvas sinCache;
    // The SAME font and the SAME atlas in both: the batches carry the pointer.
    montaEscenaCompleta(conCache, font, atlas);
    montaEscenaCompleta(sinCache, font, atlas);

    UiDrawData a, b;
    for (int frame = 0; frame < 4; ++frame)
    {
        const float t = (float)frame * 0.25f;
        conCache.updateInput(raton(200.0f, 150.0f, t));
        sinCache.updateInput(raton(200.0f, 150.0f, t));

        // Cache turned off by hand: DirtyAll from the root goes down the whole tree.
        sinCache.root().markDirty(UiElement::DirtyAll);

        conCache.buildDrawData(kW, kH, a);
        sinCache.buildDrawData(kW, kH, b);

        CHECK(mismosBytes(a, b));
        CHECK(sinCache.rebuiltNodes() == cuentaNodos(sinCache.root()));
    }

    CHECK(!a.vertices.empty());
    CHECK(a.batches.size() >= 2);   // the text goes in a different atlas than the image

    // And with the scene already still (the button's hover already settled) the one with the
    // cache re-emits nothing, which is what all this was about.
    conCache.updateInput(raton(200.0f, 150.0f, 1.0f));
    conCache.buildDrawData(kW, kH, a);
    CHECK(conCache.rebuiltNodes() == 0);
}

// ── Real baking of a font ─────────────────────────────────────────────
// The only test that touches FreeType and the project's TTF. It does not test the outline
// (that is msdfgen): it tests WHICH CODEPOINTS enter the default atlas, which
// is what decides whether a Spanish text is seen whole. The failure it catches is
// SILENT: shapeText skips the codepoint with no glyph and leaves neither a gap nor a box, so
// "Año" comes out "Ao" and there is not a log that says so.
static void test_fuente_por_defecto_hornea_acentos()
{
    UiFont font;
    // 32 px and not the default 48: the atlas is smaller and the test faster;
    // the baked range does not depend on the size.
    if (!font.bakeFromFile(kDefaultUiFontPath, 32.0f))
    {
        std::printf("FAIL: no se pudo hornear %s (cwd equivocado?)\n", kDefaultUiFontPath);
        ++g_failures;
        return;
    }

    // ASCII: what already worked has to stay there.
    CHECK(font.findGlyph('A') != nullptr);
    CHECK(font.findGlyph('z') != nullptr);
    CHECK(font.findGlyph(' ') != nullptr);

    // Latin-1: accented lowercase and uppercase letters, eñe and opening marks.
    CHECK(font.findGlyph(0x00F1) != nullptr);   // ñ
    CHECK(font.findGlyph(0x00D1) != nullptr);   // Ñ
    CHECK(font.findGlyph(0x00E1) != nullptr);   // á
    CHECK(font.findGlyph(0x00E9) != nullptr);   // é
    CHECK(font.findGlyph(0x00ED) != nullptr);   // í
    CHECK(font.findGlyph(0x00F3) != nullptr);   // ó
    CHECK(font.findGlyph(0x00FA) != nullptr);   // ú
    CHECK(font.findGlyph(0x00FC) != nullptr);   // ü
    CHECK(font.findGlyph(0x00C1) != nullptr);   // Á
    CHECK(font.findGlyph(0x00BF) != nullptr);   // ¿
    CHECK(font.findGlyph(0x00A1) != nullptr);   // ¡

    // The ellipsis of UiTextOverflow::Ellipsis: without it it falls back to
    // "..." (three quads instead of one) without warning.
    CHECK(font.findGlyph(0x2026) != nullptr);

    // A codepoint that the font does not carry is NOT invented: no .notdef boxes
    // taking up room in the atlas.
    CHECK(font.findGlyph(0x4E2D) == nullptr);

    // And the atlas does not blow up from extending the range.
    CHECK(font.atlas().width() <= 2048);
    CHECK(font.atlas().width() == font.atlas().height());
    CHECK(font.hasGlyphs());
}

// The baking distributes each glyph's MSDF among several threads. The result
// CANNOT depend on how many: the packing is decided BEFORE and each glyph writes
// into its rect. This test compares the parallel atlas with the sequential one BYTE BY BYTE,
// which is the only thing that catches both ways of breaking it at once: that msdfgen
// had shared state, and that two glyphs stepped on each other's rect.
//
// Two attempts with threads: a race can pass by luck once.
static void test_fuente_paralela_da_el_mismo_atlas()
{
    UiFont seq;
    // 24 px: what is compared is that the atlas comes out the same, not the quality, and at
    // this size the whole batch costs seconds instead of half a minute with the
    // debug heap.
    if (!seq.bakeFromFile(kDefaultUiFontPath, 24.0f, defaultUiCodepointRanges(), 1))
    {
        std::printf("FAIL: no se pudo hornear %s en secuencial\n", kDefaultUiFontPath);
        ++g_failures;
        return;
    }
    const std::vector<uint8_t> esperado = seq.atlas().sourcePixels();
    CHECK(!esperado.empty());

    for (int intento = 0; intento < 2; ++intento)
    {
        UiFont par;
        CHECK(par.bakeFromFile(kDefaultUiFontPath, 24.0f, defaultUiCodepointRanges(), 4));
        CHECK(par.atlas().width()  == seq.atlas().width());
        CHECK(par.atlas().height() == seq.atlas().height());
        CHECK(par.atlas().sourcePixels() == esperado);

        const UiGlyph* a = seq.findGlyph(0x00F1);   // ñ
        const UiGlyph* b = par.findGlyph(0x00F1);
        CHECK(a != nullptr && b != nullptr);
        if (a && b)
        {
            CHECK(a->rect.x == b->rect.x);
            CHECK(a->rect.y == b->rect.y);
            CHECK(a->advance == b->advance);
        }
    }
}

// The kerning of a MODERN font. FreeType only reads the classic 'kern' table and
// today's fonts carry the pairs in GPOS, so UiFont::kerning returned
// 0 for everything: the engine had kerning implemented, tested with fake
// fonts, and dead with the real ones.
static void test_fuente_por_defecto_trae_kerning()
{
    UiFont font;
    if (!font.bakeFromFile(kDefaultUiFontPath, 32.0f))
    {
        std::printf("FAIL: no se pudo hornear %s\n", kDefaultUiFontPath);
        ++g_failures;
        return;
    }

    // At least ONE pair with a correction: which one is not pinned down because that is up to the
    // font designer, but "none in 26x26" can only mean
    // that the kerning is not being read.
    int    pares = 0;
    float  algunValor = 0.0f;
    for (uint32_t a = 'A'; a <= 'Z'; ++a)
        for (uint32_t b = 'A'; b <= 'Z'; ++b)
            if (font.kerning(a, b) != 0.0f) { ++pares; algunValor = font.kerning(a, b); }

    CHECK(pares > 0);
    CHECK(algunValor != 0.0f);

    // And in bake pixels, not in design units: a value in the hundreds
    // would mean the conversion is missing and it would separate the letters by half a word.
    CHECK(std::fabs(algunValor) < 32.0f);

    // A pair with no entry is still worth exactly 0.
    CHECK(font.kerning('A', 0x4E2D) == 0.0f);
}

// ── Disk cache ──────────────────────────────────────────────────────────
// What is tested is that the font coming out of the cache is INDISTINGUISHABLE from
// the baked one: same pixels byte for byte, same metrics. If it were not, the
// text would change appearance between the first start and the second, which is the
// kind of failure nobody attributes to a cache.
//
// And that any problem with the file ends in a bake, never in garbage.
static const char* kCacheDirTest = ".dt-cache-test/fonts";

static void test_cache_de_fuente_devuelve_lo_mismo_que_hornear()
{
    std::filesystem::remove_all(".dt-cache-test");
    UiFont::setCacheDirectory(kCacheDirTest);

    // Short range on purpose: the cache does not know how many glyphs it holds, and this way the
    // test costs tenths of a second instead of seconds.
    const std::vector<UiCodepointRange> rangos = { {65, 90}, {0x00F1, 0x00F1} };

    UiFont horneada;
    CHECK(horneada.bakeFromFileCached(kDefaultUiFontPath, 24.0f, rangos));

    // First call: there was nothing, so it must have LEFT something behind.
    size_t ficheros = 0;
    for (const auto& e : std::filesystem::directory_iterator(kCacheDirTest))
        if (e.path().extension() == ".dtfont") ++ficheros;
    CHECK(ficheros == 1);

    UiFont cacheada;
    CHECK(cacheada.bakeFromFileCached(kDefaultUiFontPath, 24.0f, rangos));

    CHECK(cacheada.atlas().width()  == horneada.atlas().width());
    CHECK(cacheada.atlas().height() == horneada.atlas().height());
    CHECK(cacheada.atlas().sourcePixels() == horneada.atlas().sourcePixels());
    CHECK(cacheada.atlas().sourceIsSrgb() == false);   // un MSDF no es color

    CHECK(cacheada.bakeSize()   == horneada.bakeSize());
    CHECK(cacheada.ascent()     == horneada.ascent());
    CHECK(cacheada.descent()    == horneada.descent());
    CHECK(cacheada.lineHeight() == horneada.lineHeight());

    const UiGlyph* a = horneada.findGlyph(0x00F1);
    const UiGlyph* b = cacheada.findGlyph(0x00F1);
    CHECK(a != nullptr && b != nullptr);
    if (a && b)
    {
        CHECK(a->rect.x == b->rect.x);
        CHECK(a->rect.y == b->rect.y);
        CHECK(a->rect.width == b->rect.width);
        CHECK(a->bearingY == b->bearingY);
        CHECK(a->advance == b->advance);
    }

    // Kerning: comparing a pair blindly proves NOTHING if it is worth 0 in both (a
    // sabotage that emptied the map on saving checked this and nobody caught it).
    // A pair with a real correction is looked for and only that one is compared.
    uint32_t izq = 0, der = 0;
    for (uint32_t i = 65; i <= 90 && izq == 0; ++i)
        for (uint32_t j = 65; j <= 90; ++j)
            if (horneada.kerning(i, j) != 0.0f) { izq = i; der = j; break; }

    if (izq != 0)
    {
        CHECK(cacheada.kerning(izq, der) == horneada.kerning(izq, der));
        CHECK(cacheada.kerning(izq, der) != 0.0f);
    }
    else
    {
        // It is not an engine failure: FreeType only reads the classic 'kern' table and
        // modern fonts carry the kerning in GPOS. This is stated so that
        // nobody reads this test as "kerning works and is covered".
        std::printf("[aviso] %s no trae tabla kern: el round-trip del kerning "
                    "no queda cubierto por este test\n", kDefaultUiFontPath);
    }

    // What was NOT requested is still not there: the cache does not invent glyphs.
    CHECK(cacheada.findGlyph('a') == nullptr);

    // Another configuration = another entry, not the neighboring one reinterpreted.
    UiFont otroTamano;
    CHECK(otroTamano.bakeFromFileCached(kDefaultUiFontPath, 18.0f, rangos));
    CHECK(otroTamano.bakeSize() == 18.0f);

    // A corrupt entry is baked, not interpreted: its magic is smashed.
    std::filesystem::path victima;
    for (const auto& e : std::filesystem::directory_iterator(kCacheDirTest))
        if (e.path().extension() == ".dtfont" && std::filesystem::file_size(e.path()) > 100000)
            victima = e.path();
    if (!victima.empty())
    {
        {
            std::fstream f(victima, std::ios::binary | std::ios::in | std::ios::out);
            const uint32_t basura = 0xDEADBEEFu;
            f.seekp(0);
            f.write((const char*)&basura, sizeof(basura));
        }
        UiFont trasCorromper;
        CHECK(trasCorromper.bakeFromFileCached(kDefaultUiFontPath, 24.0f, rangos));
        CHECK(trasCorromper.atlas().sourcePixels() == horneada.atlas().sourcePixels());
    }

    // With the cache turned off it is baked all the same.
    UiFont::setCacheDirectory("");
    UiFont sinCache;
    CHECK(sinCache.bakeFromFileCached(kDefaultUiFontPath, 24.0f, rangos));
    CHECK(sinCache.atlas().sourcePixels() == horneada.atlas().sourcePixels());

    std::filesystem::remove_all(".dt-cache-test");
}

// ── Sprite sidecar ──────────────────────────────────────────────────────
// The sub-rects of an atlas live in a JSON next to the image. What is tested
// is the round trip and, above all, that a sidecar that cannot be read
// does NOT leave the atlas half-done: with half a list the widget draws the whole image
// instead of its sprite, and that looks like an art problem, not a loading one.
static const char* kSidecarDir = ".dt-sprites-test";

static void test_sidecar_de_sprites_ida_y_vuelta()
{
    std::filesystem::remove_all(kSidecarDir);
    std::filesystem::create_directories(kSidecarDir);
    const std::string ruta = std::string(kSidecarDir) + "/botones.sprites.json";

    UiTextureAtlas origen;
    origen.setSize(256, 128);
    // Non-neutral values different from each other: with zeros or repeated ones, an x
    // written into y's slot would go unnoticed.
    origen.addSprite("btn_normal",  UiSpriteRect{ 3.0f,  5.0f, 64.0f, 32.0f});
    origen.addSprite("btn_hover",   UiSpriteRect{70.0f, 11.0f, 48.0f, 24.0f});
    origen.addSprite("btn_pressed", UiSpriteRect{130.0f, 17.0f, 40.0f, 20.0f});
    CHECK(origen.saveSprites(ruta));
    CHECK(std::filesystem::exists(ruta));

    UiTextureAtlas destino;
    destino.setSize(256, 128);
    CHECK(destino.loadSprites(ruta));
    CHECK(destino.spriteCount() == 3);

    const UiSpriteRect* r = destino.findSprite("btn_hover");
    CHECK(r != nullptr);
    if (r)
    {
        CHECK(nearly(r->x, 70.0f));
        CHECK(nearly(r->y, 11.0f));
        CHECK(nearly(r->width, 48.0f));
        CHECK(nearly(r->height, 24.0f));
    }

    // And the UVs that come out of there are those of the sub-rect, not of the whole atlas:
    // it is the only thing the batcher sees.
    const UiUvRect uv = destino.uvRect("btn_hover");
    CHECK(nearly(uv.u0, 70.0f / 256.0f));
    CHECK(nearly(uv.v0, 11.0f / 128.0f));
    CHECK(nearly(uv.u1, (70.0f + 48.0f) / 256.0f));
    CHECK(nearly(uv.v1, (11.0f + 24.0f) / 128.0f));

    // Sorted and stable names: the editor's combo indexes them.
    const std::vector<std::string> nombres = destino.spriteNames();
    CHECK(nombres.size() == 3);
    if (nombres.size() == 3)
    {
        CHECK(nombres[0] == "btn_hover");
        CHECK(nombres[1] == "btn_normal");
        CHECK(nombres[2] == "btn_pressed");
    }

    // Loading REPLACES: a sprite that is no longer in the file does not survive.
    destino.addSprite("sobra", UiSpriteRect{1.0f, 2.0f, 3.0f, 4.0f});
    CHECK(destino.loadSprites(ruta));
    CHECK(destino.hasSprite("sobra") == false);
    CHECK(destino.spriteCount() == 3);

    std::filesystem::remove_all(kSidecarDir);
}

static void test_sidecar_ruta_derivada_de_la_imagen()
{
    CHECK(UiTextureAtlas::spriteSheetPathFor("assets/ui/botones.png") ==
          "assets/ui/botones.sprites.json");
    // Uppercase and other extensions: it cuts at the last dot, not at ".png".
    CHECK(UiTextureAtlas::spriteSheetPathFor("a/b/HOJA.PNG") == "a/b/HOJA.sprites.json");
    CHECK(UiTextureAtlas::spriteSheetPathFor("x.tga") == "x.sprites.json");
    // Without an extension there is nothing to remove.
    CHECK(UiTextureAtlas::spriteSheetPathFor("sinpunto") == "sinpunto.sprites.json");
    // A dot in a DIRECTORY is not the file's extension.
    CHECK(UiTextureAtlas::spriteSheetPathFor("v1.2/hoja.png") == "v1.2/hoja.sprites.json");
}

static void test_sidecar_roto_no_deja_el_atlas_a_medias()
{
    std::filesystem::remove_all(kSidecarDir);
    std::filesystem::create_directories(kSidecarDir);

    UiTextureAtlas atlas;
    atlas.setSize(64, 64);
    atlas.addSprite("bueno", UiSpriteRect{7.0f, 9.0f, 11.0f, 13.0f});

    // (a) It does not exist.
    CHECK(atlas.loadSprites(std::string(kSidecarDir) + "/no-existe.sprites.json") == false);
    CHECK(atlas.spriteCount() == 1);
    CHECK(atlas.hasSprite("bueno"));

    // (b) No es JSON.
    const std::string basura = std::string(kSidecarDir) + "/basura.sprites.json";
    { std::ofstream f(basura); f << "{{{ esto no es json"; }
    CHECK(atlas.loadSprites(basura) == false);
    CHECK(atlas.hasSprite("bueno"));

    // (c) It is JSON but does not have the expected root.
    const std::string ajeno = std::string(kSidecarDir) + "/ajeno.sprites.json";
    { std::ofstream f(ajeno); f << R"({"otracosa": 42})"; }
    CHECK(atlas.loadSprites(ajeno) == false);
    CHECK(atlas.hasSprite("bueno"));

    // (d) Invalid loose entries: they are skipped, the rest get in. A rect of area
    // 0 would give degenerate UVs and an invisible quad with no error.
    const std::string mixto = std::string(kSidecarDir) + "/mixto.sprites.json";
    {
        std::ofstream f(mixto);
        f << R"({"sprites": {
                   "vale":     {"x": 2, "y": 4, "w": 8,  "h": 6},
                   "ancho0":   {"x": 0, "y": 0, "w": 0,  "h": 6},
                   "alto0":    {"x": 0, "y": 0, "w": 8,  "h": 0},
                   "negativo": {"x": 0, "y": 0, "w": -8, "h": 6},
                   "tambien":  {"x": 20, "y": 24, "w": 12, "h": 10}
                 }})";
    }
    CHECK(atlas.loadSprites(mixto));
    CHECK(atlas.spriteCount() == 2);
    CHECK(atlas.hasSprite("vale"));
    CHECK(atlas.hasSprite("tambien"));
    CHECK(atlas.hasSprite("ancho0") == false);
    CHECK(atlas.hasSprite("alto0") == false);
    CHECK(atlas.hasSprite("negativo") == false);

    std::filesystem::remove_all(kSidecarDir);
}

// ── World canvas ─────────────────────────────────────────────────────
// The matrix of a world canvas. Three things that fail SILENTLY: that the
// canvas ends up centered on the object (otherwise it shows up shifted half a screen),
// that the Y is FLIPPED (the canvas grows downward and the world upward: an
// extra sign draws the sign upside down) and that the scale is units per
// pixel and not the other way around.
static void test_world_canvas_matrix_centra_y_voltea_la_y()
{
    CanvasComponent c;
    c.renderMode = UiCanvasRenderMode::World;
    c.worldScale = 0.001f;
    c.billboard  = UiBillboard::None;

    const glm::vec2 tam(1920.0f, 1080.0f);
    const glm::mat4 mundo = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 5.0f, -3.0f));
    const glm::mat4 vista(1.0f);

    const glm::mat4 m = uiWorldCanvasMatrix(c, tam, mundo, vista);

    // The canvas CENTER (960, 540) falls at the GameObject's position.
    const glm::vec4 centro = m * glm::vec4(960.0f, 540.0f, 0.0f, 1.0f);
    CHECK(nearly(centro.x, 10.0f));
    CHECK(nearly(centro.y, 5.0f));
    CHECK(nearly(centro.z, -3.0f));

    // The canvas (0,0) corner is the TOP left one, so in the
    // world it falls to the left and ABOVE the center.
    const glm::vec4 sup = m * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    CHECK(nearly(sup.x, 10.0f - 0.96f));
    CHECK(nearly(sup.y, 5.0f + 0.54f));

    // And the (w,h) at the bottom right.
    const glm::vec4 inf = m * glm::vec4(1920.0f, 1080.0f, 0.0f, 1.0f);
    CHECK(nearly(inf.x, 10.0f + 0.96f));
    CHECK(nearly(inf.y, 5.0f - 0.54f));

    // The total size: 1.92 x 1.08 units.
    CHECK(nearly(inf.x - sup.x, 1.92f));
    CHECK(nearly(sup.y - inf.y, 1.08f));
}

// worldScale is units per PIXEL: doubling it doubles the sign.
static void test_world_canvas_matrix_escala()
{
    CanvasComponent c;
    c.renderMode = UiCanvasRenderMode::World;
    c.worldScale = 0.002f;

    const glm::mat4 m = uiWorldCanvasMatrix(c, glm::vec2(100.0f, 50.0f),
                                            glm::mat4(1.0f), glm::mat4(1.0f));
    const glm::vec4 a = m * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const glm::vec4 b = m * glm::vec4(100.0f, 0.0f, 0.0f, 1.0f);
    CHECK(nearly(b.x - a.x, 0.2f));
}

// Billboard. The camera looks from +Z toward the origin; the canvas is at the
// origin with some rotation that the billboard has to OVERRIDE.
static void test_world_canvas_matrix_billboard()
{
    CanvasComponent c;
    c.renderMode = UiCanvasRenderMode::World;
    c.worldScale = 0.001f;

    // Absurd rotation of the object: if the billboard does not override it, it shows.
    glm::mat4 mundo = glm::rotate(glm::mat4(1.0f), 1.1f, glm::vec3(0.3f, 0.5f, 0.8f));

    // Camera at (0, 4, 6) looking at the origin: it looks down and toward -Z.
    const glm::mat4 vista = glm::lookAt(glm::vec3(0.0f, 4.0f, 6.0f),
                                        glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    // Without billboard the object's rotation rules: the canvas X axis is NOT the
    // world's (1,0,0).
    c.billboard = UiBillboard::None;
    {
        const glm::mat4 m = uiWorldCanvasMatrix(c, glm::vec2(100.0f, 100.0f), mundo, vista);
        const glm::vec3 ejeX = glm::normalize(glm::vec3(m[0]));
        CHECK(!nearly(ejeX.x, 1.0f));
    }

    // YawOnly: with the camera at (0,4,6) the azimuth falls EXACTLY on the world's Z
    // axis (x=0): "really turning the yaw" and "doing nothing" would give the same
    // basis by pure coincidence of that camera, and only looking at axisY does not
    // distinguish it (up is fixed (0,1,0) no matter what happens with right/forward).
    // A camera with an X component is used so that the azimuth does not coincide with
    // any world axis, and right (axisX) and forward
    // (axisZ) are checked by hand with their EXACT values, not just "is different from".
    const glm::mat4 vistaYaw = glm::lookAt(glm::vec3(5.0f, 4.0f, 6.0f),
                                           glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    c.billboard = UiBillboard::YawOnly;
    {
        const glm::mat4 m = uiWorldCanvasMatrix(c, glm::vec2(100.0f, 100.0f), mundo, vistaYaw);
        const glm::vec3 ejeX = glm::normalize(glm::vec3(m[0]));
        const glm::vec3 ejeY = glm::normalize(glm::vec3(m[1]));
        const glm::vec3 ejeZ = glm::normalize(glm::vec3(m[2]));

        // up (axisY) is still the world's: it does not tip over when looking from
        // above, which is what a health bar wants.
        CHECK(nearly(ejeY.x, 0.0f));
        CHECK(nearly(std::fabs(ejeY.y), 1.0f));
        CHECK(nearly(ejeY.z, 0.0f));

        // forward (axisZ) is the horizontal projection of the camera (5,4,6)
        // onto the XZ plane, normalized: (5,0,6)/sqrt(61). Y = 0 (the yaw does not
        // tilt) and X != 0: it DOES turn. With the old camera (x=0) this would have
        // come out (0,0,1) by coincidence, same as with no turning at all.
        CHECK(nearly(ejeZ.x, 0.6401844f));
        CHECK(nearly(ejeZ.y, 0.0f));
        CHECK(nearly(ejeZ.z, 0.7682212f));

        // right (axisX) = up x forward: perpendicular to axisZ and with its
        // own exact value, different from the world's (1,0,0).
        CHECK(nearly(ejeX.x, 0.7682212f));
        CHECK(nearly(ejeX.y, 0.0f));
        CHECK(nearly(ejeX.z, -0.6401844f));
        CHECK(nearly(glm::dot(ejeX, ejeZ), 0.0f));
    }

    // Full: it faces the camera fully, so the canvas Y axis TILTS.
    c.billboard = UiBillboard::Full;
    {
        const glm::mat4 m = uiWorldCanvasMatrix(c, glm::vec2(100.0f, 100.0f), mundo, vista);
        const glm::vec3 ejeY = glm::normalize(glm::vec3(m[1]));
        CHECK(!nearly(std::fabs(ejeY.y), 1.0f));
    }

    // Guard against looking STRAIGHT VERTICAL: with the camera above looking
    // down, the horizontal projection of "back" vanishes (length2 ~ 0) and
    // without the guard normalize(0,0,0) would give NaN, which would slip into the entire
    // matrix. Nobody exercised this branch before: if a refactor deletes it, this
    // is the only check that finds out.
    const glm::mat4 vistaVertical = glm::lookAt(glm::vec3(0.0f, 10.0f, 0.0f),
                                                glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    c.billboard = UiBillboard::YawOnly;
    {
        const glm::mat4 m = uiWorldCanvasMatrix(c, glm::vec2(100.0f, 100.0f), mundo, vistaVertical);
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                CHECK(!std::isnan(m[col][row]));
    }
}

int main()
{
    // Without a buffer: if a test blows up halfway through a batch, what was already printed is NOT lost
    // and you can see exactly where it was.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // The tests bake FOR REAL: without this, the first one would leave the cache set
    // and the rest would compare files instead of bakes.
    UiFont::setCacheDirectory("");

    test_fuente_por_defecto_hornea_acentos();
    test_fuente_paralela_da_el_mismo_atlas();
    test_fuente_por_defecto_trae_kerning();
    test_cache_de_fuente_devuelve_lo_mismo_que_hornear();

    test_sidecar_de_sprites_ida_y_vuelta();
    test_sidecar_ruta_derivada_de_la_imagen();
    test_sidecar_roto_no_deja_el_atlas_a_medias();

    test_ui_batch_offsets_se_acumulan_dentro_del_frame();
    test_ui_cursor_fits_guarda_la_capacidad_del_buffer();

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
    test_texto_alineacion_vertical();
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

    test_eventos_hit_test_gana_el_de_arriba();
    test_eventos_clip_recorta_el_hit_test();
    test_eventos_enter_exit_y_hovered();
    test_eventos_click_pide_el_mismo_elemento();
    test_eventos_doble_click_por_tiempo_y_distancia();
    test_eventos_drag_umbral_y_destino_del_drop();
    test_eventos_scroll_burbujea_hasta_el_padre();
    test_eventos_consumed_corta_la_burbuja();
    test_eventos_foco_tab_y_escape();
    test_eventos_teclado_solo_con_foco();
    test_teclado_navega_y_activa_el_foco();

    test_boton_prioridad_de_estados();
    test_boton_color_tint();
    test_boton_base_color_tinta_los_estados();
    test_boton_sprite_swap_no_parte_el_lote();
    test_boton_animation_interpola_y_no_pasa_de_largo();
    test_boton_no_interactable_no_emite_click();

    test_mascara_insets_cada_uno_en_su_lado();
    test_mascara_self_deja_fuera_al_propio_elemento();
    test_mascara_anidada_es_interseccion();
    test_mascara_vacia_no_emite_ni_revienta();
    test_mascara_lotes_y_mask_enabled();
    test_mascara_recorta_el_hit_test();
    test_mascara_neutral_sin_clip_children();
    test_neutralidad_de_los_botones();
    test_neutralidad_de_los_eventos();

    test_anim_curvas_extremos_medio_y_desbordes();
    test_anim_fade_mueve_el_alfa_y_no_la_posicion();
    test_anim_move_mueve_la_posicion_y_no_el_tamano();
    test_anim_scale_cambia_el_tamano_y_no_el_pivot();
    test_anim_color_mueve_los_cuatro_canales();
    test_anim_rotation_gira_las_esquinas_conservando_la_distancia();
    test_anim_determinismo_por_tiempo_y_por_pasos();
    test_anim_once_loop_y_pingpong();
    test_anim_playing_false_congela();
    test_rotacion_cero_no_toca_ni_un_vertice();
    test_neutralidad_de_las_animaciones();

    test_nav_next_y_previous_dan_la_vuelta();
    test_nav_direccional_elige_al_vecino_de_ese_lado();
    test_nav_alineado_gana_al_diagonal_mas_cercano();
    test_nav_sin_candidato_no_mueve_el_foco();
    test_nav_overrides_ganan_a_la_geometria();
    test_nav_sin_foco_toma_el_primero_en_preorden();
    test_nav_sin_build_draw_data_solo_falla_la_direccional();
    test_nav_dispara_blur_y_focus_una_sola_vez();
    test_nav_determinismo_de_la_secuencia();
    test_neutralidad_de_la_navegacion();

    test_resolucion_constant_pixel_size_escala_el_quad();
    test_resolucion_scale_with_screen_size_match();
    test_resolucion_constant_physical_size();
    test_resolucion_safe_area();
    test_resolucion_aspect_ratio();
    test_resolucion_hit_test_en_pixeles();
    test_resolucion_determinismo();
    test_neutralidad_de_la_resolucion();

    test_cache_segundo_build_no_reemite_nada();
    test_cache_mover_una_hoja_no_reemite_hermanos();
    test_cache_color_es_material_y_no_recoloca();
    test_cache_layout_del_padre_baja_pero_no_cruza();
    test_cache_resolucion_y_escala_reemiten_todo();
    test_cache_animacion_ensucia_solo_su_nodo();
    test_cache_neutralidad_escena_completa();

    test_world_canvas_matrix_centra_y_voltea_la_y();
    test_world_canvas_matrix_escala();
    test_world_canvas_matrix_billboard();

    if (g_failures == 0) std::printf("ui_batch_tests: OK\n");
    else                 std::printf("ui_batch_tests: %d fallos\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
