// Headless test of the shared texture cache (no device). Fake handles with their
// own identity: two objects only share an image if the entry has NOT been created
// twice. What is tested are the two ways to break it: sharing too much (two
// different textures on the same image) and releasing too much (dropping one
// character and leaving its twins sampling freed memory).
#include "DonTopo/Renderer/SharedTextureCache.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

namespace
{
    int g_next = 1;
    int g_creadas = 0;
    int g_destruidas = 0;
    int crear() { ++g_creadas; return g_next++; }
    void reiniciar() { g_creadas = 0; g_destruidas = 0; }
}

static void test_same_key_creates_once()
{
    reiniciar();
    SharedTextureCache<int> c;
    const std::string k = makeTextureKey("a.png", {}, TextureKind::BaseColor);
    bool creada1 = false, creada2 = true;
    const int h1 = c.acquire(k, crear, &creada1);
    const int h2 = c.acquire(k, crear, &creada2);
    CHECK(h1 == h2);
    CHECK(g_creadas == 1);
    CHECK(creada1 && !creada2);
    CHECK(c.refCount(k) == 2);
}

static void test_same_path_other_kind_is_other_image()
{
    reiniciar();
    SharedTextureCache<int> c;
    const int a = c.acquire(makeTextureKey("a.png", {}, TextureKind::BaseColor), crear);
    const int b = c.acquire(makeTextureKey("a.png", {}, TextureKind::Normal), crear);
    CHECK(a != b);
    CHECK(g_creadas == 2);
    CHECK(c.size() == 2u);
}

static void test_release_destroys_at_zero()
{
    reiniciar();
    SharedTextureCache<int> c;
    const std::string k = makeTextureKey("a.png", {}, TextureKind::Orm);
    const int h = c.acquire(k, crear);
    c.acquire(k, crear);
    auto destruir = [](const int&) { ++g_destruidas; };
    c.release(h, destruir);
    CHECK(g_destruidas == 0);
    CHECK(c.contains(h));
    c.release(h, destruir);
    CHECK(g_destruidas == 1);
    CHECK(!c.contains(h));
    c.release(h, destruir);                 // no longer there: no-op
    CHECK(g_destruidas == 1);
}

static void test_empty_key_is_not_cached()
{
    reiniciar();
    SharedTextureCache<int> c;
    const std::string k = makeTextureKey("", {}, TextureKind::BaseColor);
    CHECK(k.empty());
    const int h1 = c.acquire(k, crear);
    const int h2 = c.acquire(k, crear);
    CHECK(g_creadas == 2);
    CHECK(h1 != h2);
    CHECK(c.size() == 0u);
    c.release(h1, [](const int&) { ++g_destruidas; });
    CHECK(g_destruidas == 0);               // it was not theirs: the caller goes on its way
}

// If creation fails (empty handle), no null entry is left to hand out to the
// next caller: it tries again.
static void test_failed_create_is_not_cached()
{
    reiniciar();
    SharedTextureCache<int> c;
    const std::string k = makeTextureKey("roto.png", {}, TextureKind::BaseColor);
    const int h = c.acquire(k, [] { ++g_creadas; return 0; });
    CHECK(h == 0);
    CHECK(c.size() == 0u);
    c.acquire(k, crear);
    CHECK(g_creadas == 2);
}

static void test_embedded_key_by_content()
{
    const std::vector<uint8_t> a = { 1, 2, 3, 4 };
    const std::vector<uint8_t> b = { 1, 2, 3, 4 };
    const std::vector<uint8_t> d = { 1, 2, 3, 5 };
    CHECK(makeTextureKey("x.fbx", a, TextureKind::BaseColor) == makeTextureKey("y.fbx", b, TextureKind::BaseColor));
    CHECK(makeTextureKey("x.fbx", a, TextureKind::BaseColor) != makeTextureKey("x.fbx", d, TextureKind::BaseColor));
    CHECK(makeTextureKey("x.fbx", a, TextureKind::BaseColor) != makeTextureKey("x.fbx", a, TextureKind::Normal));
}

static void test_suffix_separates_same_file_same_kind()
{
    reiniciar();
    SharedTextureCache<int> c;
    const int a = c.acquire(makeTextureKey("a.png", {}, TextureKind::BaseColor, "#am"), crear);
    const int b = c.acquire(makeTextureKey("a.png", {}, TextureKind::BaseColor, "#lm"), crear);
    CHECK(a != b);
    CHECK(g_creadas == 2);
}

int main()
{
    test_suffix_separates_same_file_same_kind();
    test_same_key_creates_once();
    test_same_path_other_kind_is_other_image();
    test_release_destroys_at_zero();
    test_empty_key_is_not_cached();
    test_failed_create_is_not_cached();
    test_embedded_key_by_content();
    if (g_failures == 0) std::printf("shared_texture_cache_tests OK\n");
    else                 std::printf("shared_texture_cache_tests FAILED: %d checks\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
