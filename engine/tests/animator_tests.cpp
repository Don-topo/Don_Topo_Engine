// Headless Animator tests: loading N clips, GPU packing, state machine and
// serialization. Plain main + asserts, no framework, consistent with
// camera_tests.cpp and physics_tests.cpp.
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/Blend2D.h"
#include "DonTopo/Core/PropertyTracks.h"
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/Renderer/PoseBlock.h"
#include "DonTopo/Renderer/IkBlock.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Renderer/SkinnedMeshPacking.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
#include "DonTopo/Renderer/SkinnedFrameSync.h"
#include "DonTopo/Renderer/MeshClock.h"
#include "DonTopo/Editor/AnimatorCanvasIds.h"
#include "DonTopo/Renderer/RootMotion.h"
#include "DonTopo/Core/RootMotionApply.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include <glm/gtc/matrix_transform.hpp>
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Editor/AnimatorGraphUndo.h"
#include "DonTopo/Core/AnimatorSerialization.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <tuple>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace DonTopo;

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.001f) { return std::fabs(a - b) < eps; }

// Alias of the mesh that does NOT count as an owner. A test that keeps the
// shared_ptr to inspect the mesh would make the GameObject see it as shared,
// and editMesh() would copy it: the test would look at the old copy. This way
// the GameObject stays the only owner; the alias does not keep it alive.
template <typename M>
static std::shared_ptr<M> soloObservador(const std::shared_ptr<M>& m)
{
    return std::shared_ptr<M>(m.get(), [](M*) {});
}

// Criterion 1: every clip loaded from the FBX has a non-empty, unique name, and
// a valid duration and ticksPerSecond. Exercises the loop over mAnimations even
// if the repo asset carries a single animation.
static void test_loader_reads_all_clips()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    CHECK(!m.skeleton.names.empty());
    CHECK(!m.animationClips.empty());

    for (size_t i = 0; i < m.animationClips.size(); i++)
    {
        const AnimationClip& c = m.animationClips[i];
        CHECK(!c.name.empty());
        CHECK(c.duration > 0.0f);
        CHECK(c.ticksPerSecond > 0.0f);
        CHECK(!c.channels.empty());
        // Names are unique among themselves: the Animator resolves by name.
        for (size_t j = i + 1; j < m.animationClips.size(); j++)
            CHECK(m.animationClips[i].name != m.animationClips[j].name);
    }
}

// The model's own FBX is registered as the "builtin" source: it is the entry of
// the list that the UI shows without a delete button, and the one the scene
// rebuilds via mesh.sourcePath instead of via addAnimationSource.
static void test_loader_registers_builtin_source()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");

    CHECK(m.animationSources.size() == 1u);
    if (m.animationSources.empty()) return;

    const AnimationSource& src = m.animationSources[0];
    CHECK(src.builtin == true);
    CHECK(src.path == "assets/modelAnimation.fbx");
    // clipNames reflects exactly the loaded clips, in the same order
    CHECK(src.clipNames.size() == m.animationClips.size());
    for (size_t i = 0; i < src.clipNames.size() && i < m.animationClips.size(); i++)
        CHECK(src.clipNames[i] == m.animationClips[i].name);
}

// The Animator gate asks isSkinned(), and until now the editor import dropped
// bones and weights by always calling load(). hasBones is what decides: it
// looks at the file, not at the caller.
static void test_has_bones_detects_rigged_fbx()
{
    CHECK(ModelLoader::hasBones("assets/modelAnimation.fbx") == true);
}

// loadSkinned with a file that does not exist KILLED the process: the guard at
// ModelLoader.cpp:180 detects the failure and then dereferences `scene` in a
// debug printf, precisely when `!scene` is one of the three conditions that
// trigger it. Silent segfault, exit 139, not a line in the log.
//
// It matters beyond the crash: nodeFromJson wraps the mesh load in a
// try/catch whose comment promises that a moved or deleted asset leaves the
// node without a mesh and "the rest of the scene keeps loading"
// (Scene.cpp:1683-1697). It could not keep that promise on the skinned path,
// because the process died before the catch, so opening a scene whose
// character is no longer on disk brought the editor down instead of warning.
// The sibling load() (ModelLoader.cpp:87-89) already threw correctly; it was
// only this site.
static void test_loadSkinned_missing_file_throws()
{
    bool lanzo = false;
    try
    {
        ModelLoader::loadSkinned("assets/este_fichero_no_existe.fbx");
    }
    catch (const std::exception&)
    {
        lanzo = true;
    }
    CHECK(lanzo);
}

// Writes a minimal OBJ (a triangle, no bones: the OBJ format has no concept of
// a skeleton) in the system temp directory. Helper shared by the two "model
// without rig" tests below: each asks for a different file name so they do not
// step on each other if they ever run in parallel.
static std::string writeUnriggedObjFixture(const std::string& filename)
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / filename;
    std::ofstream f(path);
    f << "v 0 0 0\n"
         "v 1 0 0\n"
         "v 0 1 0\n"
         "f 1 2 3\n";
    f.close();
    // Without this CHECK a non-writable temp dir would go unnoticed: the file
    // would not exist and hasBones() would return false anyway, so
    // test_has_bones_rejects_unrigged_model would PASS for the wrong reason, while
    // the other call sites would bring the binary down with an uncaught exception
    // from ModelLoader::load. A named failure here, in the helper.
    CHECK(!f.fail());
    return path.string();
}

// The repo has no model without a rig: the only two tracked FBX files,
// model.fbx and modelTexture.fbx, look like static props by name but are
// complete Mixamo characters (65 and 67 bones respectively, verified with an
// Assimp probe before writing this test). That is why the negative case is
// generated here instead of pointing at a repo asset: an OBJ cannot declare
// bones even if it wanted to, so it is a real negative and not an accident of
// which .fbx happens to be in assets/. If someone "fixes" this by pointing
// at model.fbx or modelTexture.fbx again, the test fails again.
//
// An FBX/OBJ without a rig must keep entering as a plain Mesh: loading it
// skinned would pay for 112 B vertices and an empty bone SSBO for nothing.
static void test_has_bones_rejects_unrigged_model()
{
    const std::string path = writeUnriggedObjFixture("dt_test_has_bones_unrigged.obj");
    CHECK(ModelLoader::hasBones(path) == false);
    std::filesystem::remove(path);
}

// It does not throw: the unreadable file is reported by the real loader, with
// its own message. If hasBones threw, the import would die before reaching that
// message.
static void test_has_bones_survives_missing_file()
{
    bool threw = false;
    bool result = true;
    try { result = ModelLoader::hasBones("assets/no_existe_este_fichero.fbx"); }
    catch (...) { threw = true; }
    CHECK(!threw);
    CHECK(result == false);
}

// clipHasMotion tells a real clip from a static take by comparing VALUES.
// Counting keys is not enough, and this test pins that: the Mixamo take has a
// channel with two keys per track whose values are identical, so a criterion
// based on "has more than one key" counted it as animated and filtered nothing.
static void test_clip_has_motion_criterion()
{
    AnimationClip vacio;                    // no channels: animates nothing
    CHECK(clipHasMotion(vacio) == false);

    // Exact shape of the Mixamo take: one channel with two keys per track with a
    // repeated value, and the rest with a single key.
    AnimationClip estatico;
    for (int b = 0; b < 3; b++)
    {
        BoneChannel ch;
        ch.boneIndex = b;
        ch.posKeys   = { { 0.0f, { 1.0f, 2.0f, 3.0f } } };
        ch.rotKeys   = { { 0.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) } };
        ch.scaleKeys = { { 0.0f, { 1.0f, 1.0f, 1.0f } } };
        if (b == 0)
        {
            ch.posKeys.push_back({ 1.0f, { 1.0f, 2.0f, 3.0f } });
            ch.rotKeys.push_back({ 1.0f, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
            ch.scaleKeys.push_back({ 1.0f, { 1.0f, 1.0f, 1.0f } });
        }
        estatico.channels.push_back(std::move(ch));
    }
    CHECK(clipHasMotion(estatico) == false);

    // Jitter below the tolerance: still not an animation.
    {
        AnimationClip c = estatico;
        c.channels[0].posKeys[1].value.y += 1e-6f;
        CHECK(clipHasMotion(c) == false);
    }

    // A value that really changes, in a single track of a single channel, already
    // counts as motion. The three are checked separately: if the criterion looked
    // only at posKeys, a purely rotational animation (the norm in a humanoid rig,
    // where only Hips translates) would be thrown away entirely.
    for (int pista = 0; pista < 3; pista++)
    {
        AnimationClip c = estatico;
        if (pista == 0) c.channels[1].posKeys.push_back({ 1.0f, { 9.0f, 2.0f, 3.0f } });
        if (pista == 1) c.channels[1].rotKeys.push_back({ 1.0f, glm::quat(0.0f, 1.0f, 0.0f, 0.0f) });
        if (pista == 2) c.channels[1].scaleKeys.push_back({ 1.0f, { 2.0f, 1.0f, 1.0f } });
        CHECK(clipHasMotion(c) == true);
    }
}

// The FBX of the bare Mixamo character carries a "mixamo.com" take of 1 tick
// whose channels have a single key: it animates nothing and leaves the mesh in
// its bind pose. Importing it as a normal clip left it at index 0, and ALL the
// engine's degraded paths fall back to that index (AnimatorComponent::
// currentClipIndex with no state or with clipIndex -1, Renderer::
// setAnimationState with an out-of-range index, Scene::fromJson with an orphan
// graph). The result was a character in T-pose that looked like a bone remap
// failure and was not: the rig and the skinning were fine, the default clip
// just did not animate.
static void test_loader_drops_static_take()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/animatedCharacter/Maw J Laygo.fbx");
    CHECK(!m.skeleton.names.empty());

    // That FBX carries exactly one take, and it is the static one: the model must
    // enter with no clips at all. Asserting only "the ones that exist animate"
    // would pass just the same with an empty list and would not prove the filter
    // acted.
    CHECK(m.animationClips.empty());
    for (const auto& c : m.animationClips)
        CHECK(clipHasMotion(c));

    // The builtin source is registered all the same: the UI needs the model's row
    // even if it contributes no clips.
    CHECK(m.animationSources.size() == 1);
    if (m.animationSources.empty()) return;
    CHECK(m.animationSources[0].builtin);
    CHECK(m.animationSources[0].clipNames.size() == m.animationClips.size());
}

// The filter must not take the good clips down with it: Mixamo animation FBX
// files enter through loadAnimationClips, not loadSkinned.
static void test_external_clips_survive_filter()
{
    SkinnedMesh base = ModelLoader::loadSkinned("assets/animatedCharacter/Maw J Laygo.fbx");
    LoadedClips lc = ModelLoader::loadAnimationClips(
        "assets/animatedCharacter/standing walk forward.fbx", base.skeleton);
    CHECK(lc.clips.size() == 1);
    CHECK(lc.mappedChannels == lc.totalChannels);
    for (const auto& c : lc.clips)
        CHECK(clipHasMotion(c));
}

// The reason for the filter, end to end: on the clean rig, the first animation
// FBX that is added gets index 0. Before, the static take occupied it and all
// the engine's fallbacks (currentClipIndex with no state, setAnimationState out
// of range, orphan graph) showed the bind pose.
static void test_first_added_clip_becomes_index_zero()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/animatedCharacter/Maw J Laygo.fbx");
    std::vector<std::string> warnings;
    CHECK(addAnimationSource(m, "assets/animatedCharacter/standing walk forward.fbx", warnings));
    CHECK(m.animationClips.size() == 1);
    if (m.animationClips.empty()) return;
    CHECK(m.animationClips[0].name == "standing walk forward");
    CHECK(clipHasMotion(m.animationClips[0]));
}

// loadAuto returns the correct dynamic type: it is the only thing
// GameObject::isSkinned() looks at, and therefore the only thing that enables
// the Animator.
static void test_load_auto_returns_skinned_for_rigged()
{
    std::shared_ptr<Mesh> m = ModelLoader::loadAuto("assets/modelAnimation.fbx");
    CHECK(m != nullptr);
    if (!m) return;
    SkinnedMesh* sm = dynamic_cast<SkinnedMesh*>(m.get());
    CHECK(sm != nullptr);
    if (!sm) return;
    CHECK(!sm->skeleton.names.empty());
    // The builtin source is created by loadSkinned; loadAuto must not alter it.
    CHECK(sm->animationSources.size() == 1u);
}

// Same reason as test_has_bones_rejects_unrigged_model: there is no FBX
// without a rig in the repo (model.fbx and modelTexture.fbx are rigged Mixamo
// characters despite the name), so the loadAuto negative is also generated as
// an OBJ instead of pointing at an asset in assets/.
static void test_load_auto_returns_static_for_unrigged()
{
    const std::string path = writeUnriggedObjFixture("dt_test_load_auto_unrigged.obj");
    std::shared_ptr<Mesh> m = ModelLoader::loadAuto(path);
    std::filesystem::remove(path);
    CHECK(m != nullptr);
    if (!m) return;
    CHECK(dynamic_cast<SkinnedMesh*>(m.get()) == nullptr);
    CHECK(!m->vertices.empty());
}

// The unique-name rule is the same one the loader already applied (Mixamo
// exports everything as "mixamo.com"), but now it lives in a shared place: it
// is used by both loadSkinned and the import of extra files.
static void test_unique_clip_name()
{
    std::vector<AnimationClip> existing;
    AnimationClip a; a.name = "walk";       existing.push_back(a);
    AnimationClip b; b.name = "walk (1)";   existing.push_back(b);

    CHECK(uniqueClipName(existing, "run")  == "run");
    CHECK(uniqueClipName(existing, "walk") == "walk (2)");
    // Empty name: it cannot stay empty, the Animator resolves by name
    CHECK(uniqueClipName(existing, "")     == "Animation");
}

// Importing animations against the file's own skeleton must give exactly the
// same as loadSkinned: same clips, same boneIndex, no warnings. It is the
// case "the animation FBX is from the same rig as the model".
static void test_load_animation_clips_matches_full_load()
{
    SkinnedMesh base = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    LoadedClips lc = ModelLoader::loadAnimationClips("assets/modelAnimation.fbx", base.skeleton);

    CHECK(lc.warnings.empty());
    CHECK(lc.clips.size() == base.animationClips.size());
    CHECK(lc.mappedChannels == lc.totalChannels);
    CHECK(lc.totalChannels > 0);

    for (size_t i = 0; i < lc.clips.size() && i < base.animationClips.size(); i++)
    {
        const AnimationClip& a = lc.clips[i];
        const AnimationClip& b = base.animationClips[i];
        CHECK(nearlyEqual(a.duration, b.duration));
        CHECK(nearlyEqual(a.ticksPerSecond, b.ticksPerSecond));
        CHECK(a.channels.size() == b.channels.size());
        for (size_t c = 0; c < a.channels.size() && c < b.channels.size(); c++)
        {
            CHECK(a.channels[c].boneIndex == b.channels[c].boneIndex);
            CHECK(a.channels[c].posKeys.size()   == b.channels[c].posKeys.size());
            CHECK(a.channels[c].rotKeys.size()   == b.channels[c].rotKeys.size());
            CHECK(a.channels[c].scaleKeys.size() == b.channels[c].scaleKeys.size());
        }
    }
}

// Foreign skeleton: no bone matches, so there is nothing to import. It warns
// and returns 0 clips; the caller turns that into a rejection.
static void test_load_animation_clips_against_foreign_skeleton()
{
    Skeleton foreign;
    foreign.names           = { "hueso_que_no_existe" };
    foreign.parentIndex     = { -1 };
    foreign.inverseBindPose = { glm::mat4(1.0f) };
    foreign.boneMap         = { { "hueso_que_no_existe", 0 } };

    LoadedClips lc = ModelLoader::loadAnimationClips("assets/modelAnimation.fbx", foreign);

    CHECK(lc.clips.empty());
    CHECK(!lc.warnings.empty());
    CHECK(lc.mappedChannels == 0);
    CHECK(lc.totalChannels > 0);
}

// Partial skeleton: what matches is imported and what does not is warned
// about. It is the real case of a rig that lacks finger bones compared to the
// animation FBX.
static void test_load_animation_clips_partial_skeleton()
{
    SkinnedMesh base = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    CHECK(base.skeleton.names.size() >= 2u);
    if (base.skeleton.names.size() < 2u) return;

    // Trimmed skeleton: only the first bone of the original
    Skeleton partial;
    partial.names           = { base.skeleton.names[0] };
    partial.parentIndex     = { -1 };
    partial.inverseBindPose = { base.skeleton.inverseBindPose[0] };
    partial.boneMap         = { { base.skeleton.names[0], 0 } };

    LoadedClips lc = ModelLoader::loadAnimationClips("assets/modelAnimation.fbx", partial);

    CHECK(!lc.warnings.empty());                 // warns about the ignored bones
    CHECK(lc.mappedChannels < lc.totalChannels);
    // The channels that survive point at the single bone of the trimmed skeleton
    for (const auto& c : lc.clips)
        for (const auto& ch : c.channels)
            CHECK(ch.boneIndex == 0);
}

// Nonexistent file: it does not throw, it warns. The UI turns it into a red
// message, and Scene::fromJson into a log line; neither wants an exception.
static void test_load_animation_clips_missing_file()
{
    Skeleton skel;
    skel.names = { "root" }; skel.parentIndex = { -1 };
    skel.inverseBindPose = { glm::mat4(1.0f) };
    skel.boneMap = { { "root", 0 } };

    LoadedClips lc = ModelLoader::loadAnimationClips("assets/no_existe_este_fichero.fbx", skel);

    CHECK(lc.clips.empty());
    CHECK(!lc.warnings.empty());
}

// The central case of the feature: the clips of a second file are added to
// those of the model. The SAME fbx is imported twice on purpose; it exercises
// name deduplication without needing an extra asset in the repo.
static void test_add_animation_source_appends_clips()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t before = m.animationClips.size();

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(m, "assets/modelAnimation.fbx", warnings));

    CHECK(m.animationClips.size() == before * 2);
    CHECK(m.animationSources.size() == 2u);
    CHECK(m.animationSources[1].builtin == false);
    CHECK(m.animationSources[1].path == "assets/modelAnimation.fbx");
    CHECK(m.animationSources[1].clipNames.size() == before);

    // Unique names across ALL clips: the Animator resolves by name
    for (size_t i = 0; i < m.animationClips.size(); i++)
        for (size_t j = i + 1; j < m.animationClips.size(); j++)
            CHECK(m.animationClips[i].name != m.animationClips[j].name);

    // The new clips are named after the file, not after the internal name
    CHECK(m.animationSources[1].clipNames[0].rfind("modelAnimation", 0) == 0);
}

// Rejection: a rig that does not match leaves the mesh EXACTLY as it was. No
// half-added clips and no registered source that contributed nothing.
static void test_add_animation_source_rejects_foreign_rig()
{
    SkinnedMesh m;
    m.skeleton.names           = { "hueso_que_no_existe" };
    m.skeleton.parentIndex     = { -1 };
    m.skeleton.inverseBindPose = { glm::mat4(1.0f) };
    m.skeleton.boneMap         = { { "hueso_que_no_existe", 0 } };

    std::vector<std::string> warnings;
    CHECK(!addAnimationSource(m, "assets/modelAnimation.fbx", warnings));

    CHECK(m.animationClips.empty());
    CHECK(m.animationSources.empty());
    CHECK(!warnings.empty());
}

// Removing a source takes its clips with it and only its own, and the GPU
// packing stays consistent with the new list (clipCount and offsets).
static void test_remove_animation_source()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t builtinCount = m.animationClips.size();
    std::vector<std::string> builtinNames;
    for (const auto& c : m.animationClips) builtinNames.push_back(c.name);

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(m, "assets/modelAnimation.fbx", warnings));
    CHECK(removeAnimationSource(m, 1));

    CHECK(m.animationSources.size() == 1u);
    CHECK(m.animationClips.size() == builtinCount);
    for (size_t i = 0; i < builtinNames.size() && i < m.animationClips.size(); i++)
        CHECK(m.animationClips[i].name == builtinNames[i]);   // order intact

    // The packing reflects the new list: an extra boneInfos would mean the GPU
    // still held the deleted clip's block.
    const PackedClips packed = packSkinnedClips(m);
    CHECK(packed.boneInfos.size() == m.animationClips.size() * m.skeleton.names.size());
}

// The builtin source is the model: removing it would leave a mesh without the
// FBX that created it. It is rejected, touching nothing.
static void test_remove_builtin_source_is_rejected()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t before = m.animationClips.size();

    CHECK(!removeAnimationSource(m, 0));
    CHECK(m.animationSources.size() == 1u);
    CHECK(m.animationClips.size() == before);

    // Index out of range: same treatment, false and no effects
    CHECK(!removeAnimationSource(m, 99));
}

// Rename: changes the clip and the clipNames of its source. Rejects empty and
// duplicate names, since both would leave clips unreachable by name.
static void test_rename_clip()
{
    SkinnedMesh m;
    m.skeleton.names = { "root" };
    AnimationClip a; a.name = "walk"; m.animationClips.push_back(a);
    AnimationClip b; b.name = "run";  m.animationClips.push_back(b);
    AnimationSource src; src.path = "x.fbx"; src.builtin = true;
    src.clipNames = { "walk", "run" };
    m.animationSources.push_back(src);

    CHECK(renameClip(m, "walk", "andar"));
    CHECK(m.animationClips[0].name == "andar");
    CHECK(m.animationSources[0].clipNames[0] == "andar");

    CHECK(!renameClip(m, "andar", ""));       // empty
    CHECK(!renameClip(m, "andar", "run"));    // already exists
    CHECK(!renameClip(m, "no_existe", "x"));  // nonexistent source name
    CHECK(m.animationClips[0].name == "andar");
}

// forcedNames is what makes a rename survive save/load and an undo: clips are
// imported with the names they already had, not with the file's.
static void test_add_animation_source_with_forced_names()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t builtinCount = m.animationClips.size();

    std::vector<std::string> forced;
    for (size_t i = 0; i < builtinCount; i++)
        forced.push_back("MiClip" + std::to_string(i));

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(m, "assets/modelAnimation.fbx", warnings, &forced));

    CHECK(m.animationSources.size() == 2u);
    CHECK(m.animationSources[1].clipNames == forced);
    for (size_t i = 0; i < forced.size(); i++)
        CHECK(m.animationClips[builtinCount + i].name == forced[i]);
}

// Renaming a clip must drag along the states that use it: otherwise the rename
// would leave the graph pointing at a name that no longer exists and bindClips
// would mark them as orphans.
static void test_rename_clip_references_in_animator()
{
    AnimatorComponent a;
    AnimatorComponent::State s0; s0.name = "A"; s0.clipName = "walk";
    AnimatorComponent::State s1; s1.name = "B"; s1.clipName = "run";
    AnimatorComponent::State s2; s2.name = "C"; s2.clipName = "walk";
    a.addState(s0); a.addState(s1); a.addState(s2);

    CHECK(a.renameClipReferences("walk", "andar") == 2);
    CHECK(a.states()[0].clipName == "andar");
    CHECK(a.states()[1].clipName == "run");
    CHECK(a.states()[2].clipName == "andar");

    // Name not used by any state: 0 and no effects
    CHECK(a.renameClipReferences("no_existe", "x") == 0);
}

// Review fix (finding 1, task-6): a swap of two builtin names by chaining
// renameClip clip by clip collides with itself and applies nothing: the second
// rename clashes with the name the first one just freed, in the wrong slot.
// applyClipNamesPositionally resolves it in one go. The "just loaded from the
// FBX" state is built by hand (like test_rename_clip) because the only asset
// in the repo (modelAnimation.fbx) carries a single clip and cannot produce a
// real swap of two builtin clips via Scene::fromJson.
static void test_apply_clip_names_positionally_swap()
{
    SkinnedMesh m;
    m.skeleton.names = { "root" };
    AnimationClip a; a.name = "Idle"; m.animationClips.push_back(a);
    AnimationClip b; b.name = "Walk"; m.animationClips.push_back(b);
    AnimationSource src; src.path = "x.fbx"; src.builtin = true;
    src.clipNames = { "Idle", "Walk" };
    m.animationSources.push_back(src);

    // Saved names: exactly the swap of the current ones.
    std::vector<std::string> saved = { "Walk", "Idle" };
    std::vector<std::string> warnings;
    applyClipNamesPositionally(m, m.animationSources[0], saved, warnings);

    CHECK(warnings.empty());
    CHECK(m.animationClips[0].name == "Walk");
    CHECK(m.animationClips[1].name == "Idle");
    CHECK(m.animationSources[0].clipNames[0] == "Walk");
    CHECK(m.animationSources[0].clipNames[1] == "Idle");

    // An AnimatorComponent that references the clip by its NEW name must bind to
    // the right index: it is the real consequence of the bug. With sequential
    // renameClip the names would have stayed as they were (Idle/Walk) and this
    // binding would have resolved to the WRONG clip instead of failing visibly.
    AnimatorComponent anim;
    AnimatorComponent::State st;
    st.name = "S"; st.clipName = "Walk";   // the name that clip 0 has NOW
    anim.addState(st);

    std::vector<std::string> bindWarnings;
    anim.bindClips(m, &bindWarnings);
    CHECK(bindWarnings.empty());
    CHECK(anim.states()[0].clipIndex == 0);   // clip 0 is the one now called "Walk"
}

// Guard for finding 1: saved names that are duplicated among themselves would
// leave two homonymous clips (the Animator resolves by name), so THAT subset is
// discarded and a warning is issued, without touching any clip.
static void test_apply_clip_names_positionally_rejects_duplicate_saved_names()
{
    SkinnedMesh m;
    m.skeleton.names = { "root" };
    AnimationClip a; a.name = "Idle"; m.animationClips.push_back(a);
    AnimationClip b; b.name = "Walk"; m.animationClips.push_back(b);
    AnimationSource src; src.path = "x.fbx"; src.builtin = true;
    src.clipNames = { "Idle", "Walk" };
    m.animationSources.push_back(src);

    std::vector<std::string> saved = { "Mismo", "Mismo" };
    std::vector<std::string> warnings;
    applyClipNamesPositionally(m, m.animationSources[0], saved, warnings);

    CHECK(!warnings.empty());
    // Nothing was applied: the clips keep their original name
    CHECK(m.animationClips[0].name == "Idle");
    CHECK(m.animationClips[1].name == "Walk");
    CHECK(m.animationSources[0].clipNames[0] == "Idle");
    CHECK(m.animationSources[0].clipNames[1] == "Walk");
}

// Guard for finding 1: a saved name that collides with a clip FOREIGN to the
// batch (from another source) is not applied, as it would leave two clips with
// the same name. The second name of the batch is a no-op (Walk == Walk) and
// must survive intact.
static void test_apply_clip_names_positionally_rejects_external_collision()
{
    SkinnedMesh m;
    m.skeleton.names = { "root" };
    AnimationClip a; a.name = "Idle"; m.animationClips.push_back(a);
    AnimationClip b; b.name = "Walk"; m.animationClips.push_back(b);
    AnimationClip c; c.name = "Jump"; m.animationClips.push_back(c);   // from another source

    AnimationSource builtin; builtin.path = "x.fbx"; builtin.builtin = true;
    builtin.clipNames = { "Idle", "Walk" };
    m.animationSources.push_back(builtin);
    AnimationSource other; other.path = "y.fbx"; other.builtin = false;
    other.clipNames = { "Jump" };
    m.animationSources.push_back(other);

    std::vector<std::string> saved = { "Jump", "Walk" };   // "Jump" already in use outside the batch
    std::vector<std::string> warnings;
    applyClipNamesPositionally(m, m.animationSources[0], saved, warnings);

    CHECK(!warnings.empty());
    CHECK(m.animationClips[0].name == "Idle");             // not applied because of the collision
    CHECK(m.animationSources[0].clipNames[0] == "Idle");
    CHECK(m.animationClips[1].name == "Walk");             // no-op, stays the same
    CHECK(m.animationSources[0].clipNames[1] == "Walk");
}

// Extra sources and renames live in the scene: the SkinnedMesh is rebuilt from
// the FBX files on every load, so without this a saved project would lose all
// imported animations.
static void test_animation_sources_survive_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    std::vector<std::string> warnings;
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    const std::string importedName = mesh->animationSources[1].clipNames[0];
    CHECK(renameClip(*mesh, importedName, "SaltoRenombrado"));
    const std::string builtinName = mesh->animationSources[0].clipNames[0];
    go->setMesh(mesh); mesh = soloObservador(mesh);

    // A state that uses the imported and renamed clip: after loading it must keep
    // resolving
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State st;
    st.name = "Salto"; st.clipName = "SaltoRenombrado";
    a->addState(st);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;

    const SkinnedMesh* lm = found->getSkinnedMesh();
    CHECK(lm != nullptr);
    if (!lm) return;

    CHECK(lm->animationSources.size() == 2u);
    CHECK(lm->animationSources[0].builtin == true);
    CHECK(lm->animationSources[0].clipNames[0] == builtinName);
    CHECK(lm->animationSources[1].builtin == false);
    CHECK(lm->animationSources[1].path == "assets/modelAnimation.fbx");
    CHECK(lm->animationSources[1].clipNames[0] == "SaltoRenombrado");

    // The renamed clip exists under that name in the flat list
    bool encontrado = false;
    for (const auto& c : lm->animationClips)
        if (c.name == "SaltoRenombrado") encontrado = true;
    CHECK(encontrado);

    // And the state resolves it without warnings
    std::vector<std::string> bindWarnings;
    found->getAnimator()->bindClips(*lm, &bindWarnings);
    CHECK(bindWarnings.empty());
    CHECK(found->getAnimator()->states()[0].clipIndex >= 0);
}

// A source whose file is gone must not bring down the scene load: it warns and
// carries on, leaving the states that used it orphaned (bindClips already marks
// them).
static void test_missing_animation_source_does_not_break_load(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh); mesh = soloObservador(mesh);

    nlohmann::json j = scene.toJson();
    // A source pointing at a nonexistent file is injected by hand: it simulates a
    // project whose .fbx was deleted or moved after saving.
    j["root"]["children"][0]["mesh"]["animationSources"].push_back(
        { {"path", "assets/no_existe.fbx"}, {"builtin", false}, {"clips", {"Fantasma"}} });

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    const SkinnedMesh* lm = found->getSkinnedMesh();
    CHECK(lm != nullptr);
    if (!lm) return;
    // The ghost source is not registered; the model stays whole
    CHECK(lm->animationSources.size() == 1u);
    CHECK(!lm->animationClips.empty());
}

// Task 3 of the model import settings plan: the source configuration of a mesh
// (renames of the builtin, external sources with their names) is captured and
// reapplied on a freshly loaded mesh, which is what a model reimport does.
static void test_animation_source_config_roundtrip()
{
    auto viejo = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    std::vector<std::string> w;
    CHECK(addAnimationSource(*viejo, "assets/modelAnimation.fbx", w));
    CHECK(viejo->animationSources.size() == 2u);
    if (viejo->animationSources.size() != 2u) return;
    const std::string builtinName  = viejo->animationSources[0].clipNames[0];
    const std::string importedName = viejo->animationSources[1].clipNames[0];
    CHECK(renameClip(*viejo, builtinName, "CaminarRenombrado"));
    CHECK(renameClip(*viejo, importedName, "SaltoRenombrado"));

    const std::vector<AnimationSourceConfig> cfg = animationSourceConfigOf(*viejo);
    CHECK(cfg.size() == 2u);
    if (cfg.size() != 2u) return;
    CHECK(cfg[0].builtin && cfg[0].clipNames[0] == "CaminarRenombrado");
    CHECK(!cfg[1].builtin && cfg[1].path == "assets/modelAnimation.fbx" &&
          cfg[1].clipNames[0] == "SaltoRenombrado");

    SkinnedMesh nuevo = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    std::vector<std::string> avisos;
    applyAnimationSourceConfig(nuevo, cfg, avisos);
    CHECK(avisos.empty());
    CHECK(nuevo.animationSources.size() == 2u);
    if (nuevo.animationSources.size() == 2u)
    {
        CHECK(nuevo.animationSources[0].builtin);
        CHECK(nuevo.animationSources[0].clipNames[0] == "CaminarRenombrado");
        CHECK(!nuevo.animationSources[1].builtin);
        CHECK(nuevo.animationSources[1].clipNames[0] == "SaltoRenombrado");
    }
    bool encontrado = false;
    for (const auto& c : nuevo.animationClips)
        if (c.name == "SaltoRenombrado") encontrado = true;
    CHECK(encontrado);
}

// An external source whose file is gone: it warns and carries on, without touching the mesh.
static void test_animation_source_config_missing_source_warns_and_continues()
{
    SkinnedMesh nuevo = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t clipsAntes = nuevo.animationClips.size();

    std::vector<AnimationSourceConfig> cfg(1);
    cfg[0].path      = "assets/no_existe.fbx";
    cfg[0].builtin   = false;
    cfg[0].clipNames = { "Fantasma" };

    std::vector<std::string> avisos;
    applyAnimationSourceConfig(nuevo, cfg, avisos);
    CHECK(!avisos.empty());
    CHECK(nuevo.animationSources.size() == 1u);
    CHECK(nuevo.animationClips.size() == clipsAntes);
}

// No builtin (mesh without sources): the builtin entry is ignored without throwing.
static void test_animation_source_config_builtin_on_empty_mesh_is_ignored()
{
    SkinnedMesh vacia;
    std::vector<AnimationSourceConfig> cfg(1);
    cfg[0].builtin   = true;
    cfg[0].clipNames = { "X" };
    std::vector<std::string> avisos;
    applyAnimationSourceConfig(vacia, cfg, avisos);
    CHECK(vacia.animationSources.empty());
}

// Review fix (finding 2, task-6): the warning of a source that fails to load
// H8 of docs/core-audit.md. A saved animator graph can carry indices
// that no longer exist (the FBX was re-exported with fewer clips and someone deleted
// states, or the .scene was edited by hand), and animatorFromJson accepted them
// as they were. Both symptoms are silent, which is the worst of all:
//
//   - entryState out of range: setEntryState validates and RETURNS DOING NOTHING,
//     so the character starts in state 0 and nobody says why.
//   - a transition with an invalid from/to: the panel skips it when drawing (it is
//     not visible) and update discards it when evaluating (it is not used), but it keeps
//     being serialized on every save. An invisible, permanent passenger.
//
// The rest of the file already warns about this kind of anomaly and animatorFromJson
// already receives the `warnings` channel (it uses it for the readFloat calls), so the
// fix is to use it.
static void test_animator_out_of_range_entry_state_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s0; s0.name = "Idle";
    AnimatorComponent::State s1; s1.name = "Run";
    a->addState(s0);
    a->addState(s1);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    // Two states (indices 0 and 1) and the file asks for 3.
    j["root"]["children"][0]["animator"]["entryState"] = 3;

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;

    // The behavior does not change (it still starts at 0, which is the only safe
    // option) but it stops being silent.
    CHECK(found->getAnimator()->entryState() == 0);
    bool aviso = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("entryState") != std::string::npos) aviso = true;
    CHECK(aviso);
}

// The transition with impossible indices is DISCARDED on load, not saved
// dead. Same criterion as pruneExtraCameras and the duplicate ids: the file
// came broken, it is repaired and the fact is reported.
static void test_animator_out_of_range_transition_is_dropped(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s0; s0.name = "Idle";
    AnimatorComponent::State s1; s1.name = "Run";
    a->addState(s0);
    a->addState(s1);
    AnimatorComponent::Transition buena; buena.fromState = 0; buena.toState = 1;
    a->addTransition(buena);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    auto& trs = j["root"]["children"][0]["animator"]["transitions"];
    trs.push_back({ {"from", 0}, {"to", 7}, {"duration", 0.0f} });   // destination that does not exist
    trs.push_back({ {"from", -1}, {"to", 1}, {"duration", 0.0f} });  // origin not set

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;

    // Only the good one survives.
    const auto& cargadas = found->getAnimator()->transitions();
    CHECK(cargadas.size() == 1u);
    if (cargadas.size() == 1u)
    {
        CHECK(cargadas[0].fromState == 0);
        CHECK(cargadas[0].toState   == 1);
    }

    int avisos = 0;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("transition") != std::string::npos) ++avisos;
    CHECK(avisos == 2);
}

// The counterweight, which keeps the guard from overreaching: a HEALTHY graph
// loads whole, without losing transitions and without a single warning. A guard
// that discarded too much would pass the two tests above and break the normal case.
static void test_animator_valid_graph_loads_untouched(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s0; s0.name = "Idle";
    AnimatorComponent::State s1; s1.name = "Run";
    a->addState(s0);
    a->addState(s1);
    a->setEntryState(1);
    AnimatorComponent::Transition t0; t0.fromState = 0; t0.toState = 1;
    AnimatorComponent::Transition t1; t1.fromState = 1; t1.toState = 0;
    a->addTransition(t0);
    a->addTransition(t1);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;

    CHECK(found->getAnimator()->transitions().size() == 2u);
    CHECK(found->getAnimator()->entryState() == 1);
    for (const auto& w : loaded.lastWarnings())
    {
        CHECK(w.find("entryState") == std::string::npos);
        CHECK(w.find("transition") == std::string::npos);
    }
}

// had to reach Scene::lastWarnings(), which is what the editor's Log Console
// reads, and not only stdout via std::printf, invisible in a build without a
// console. Same scenario as the previous test, but checking the warning
// instead of just the resilience of the load.
static void test_missing_animation_source_warns_through_scene(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh); mesh = soloObservador(mesh);

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["mesh"]["animationSources"].push_back(
        { {"path", "assets/no_existe.fbx"}, {"builtin", false}, {"clips", {"Fantasma"}} });

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;

    // The loader's warning (nonexistent file) must reach m_warnings, visible via
    // lastWarnings(), the same path the editor uses, not stdout.
    bool sawMissingSourceWarning = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("no_existe.fbx") != std::string::npos) sawMissingSourceWarning = true;
    CHECK(sawMissingSourceWarning);
}

// Scenes saved before this feature have no "animationSources": they load with
// the builtin source synthesized from sourcePath, without warnings.
static void test_scene_without_animation_sources_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    go->setMesh(std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx")));

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["mesh"].erase("animationSources");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    const SkinnedMesh* lm = found->getSkinnedMesh();
    CHECK(lm != nullptr);
    if (!lm) return;
    CHECK(lm->animationSources.size() == 1u);
    CHECK(lm->animationSources[0].builtin == true);
}

// Scenes saved before the auto-detection have "skinned": false for ALL their
// meshes, since the editor never created skinned ones. If the load kept
// reading that flag, those projects could never have an Animator without
// reimporting the mesh by hand. The file rules, not the flag.
static void test_scene_load_ignores_stale_skinned_false(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh); mesh = soloObservador(mesh);

    nlohmann::json j = scene.toJson();
    // Simulates the old file: flag set to false on an FBX that does have bones.
    j["root"]["children"][0]["mesh"]["skinned"] = false;

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;

    const SkinnedMesh* lm = found->getSkinnedMesh();
    CHECK(lm != nullptr);
    if (!lm) return;
    CHECK(!lm->skeleton.names.empty());
    CHECK(found->isSkinned());
}

// The symmetric case: a scene with "skinned": true whose FBX was later
// re-exported without bones. It loads static and its animation sources are
// discarded, but with a warning in Scene::lastWarnings() (what the Log Console
// reads), not silently. No FBX in the repo works as "without rig" (model.fbx
// and modelTexture.fbx are rigged Mixamo characters despite the name, see the
// comment of writeUnriggedObjFixture above), so that same helper is reused
// with its own file name so as not to collide with the Task 1 tests.
static void test_scene_load_warns_when_rig_disappeared(PhysicsManager& pm, AudioManager& am)
{
    const std::string unriggedPath = writeUnriggedObjFixture("dt_test_scene_rig_disappeared.obj");

    Scene scene("Test");
    GameObject* go = scene.addGameObject("Prop");
    const uint64_t id = go->id;
    go->setMesh(std::make_shared<Mesh>(ModelLoader::load(unriggedPath)));

    nlohmann::json j = scene.toJson();
    // Simulates the re-export: the scene believed it was skinned and saved sources.
    j["root"]["children"][0]["mesh"]["skinned"] = true;
    j["root"]["children"][0]["mesh"]["animationSources"] = nlohmann::json::array({
        { {"path", "assets/modelAnimation.fbx"}, {"builtin", false}, {"clips", {"Salto"}} }
    });

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    std::filesystem::remove(unriggedPath);
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;

    // It loads, but static: without bones there is nothing to animate.
    CHECK(found->hasMesh());
    CHECK(found->getSkinnedMesh() == nullptr);

    bool avisado = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("dt_test_scene_rig_disappeared.obj") != std::string::npos) avisado = true;
    CHECK(avisado);
}

// Review fix (task-3): hasBones() returns false both when the file has no
// bones and when it simply cannot be read (moved/deleted). Before this fix
// both cases triggered the same warning ("no longer declares bones"), which
// in the missing-file case points at the wrong place, and on top of that the
// attempt of ModelLoader::load() on a nonexistent path threw and the exception
// was swallowed silently, leaving the user without any clue.
// Simulates the moved/deleted file: the scene is saved with a real sourcePath,
// and then replaced with one that does not exist before reloading, so there is
// no need to touch the real disk to leave the path "dangling".
static void test_scene_load_warns_when_file_missing(PhysicsManager& pm, AudioManager& am)
{
    const std::string unriggedPath = writeUnriggedObjFixture("dt_test_scene_file_missing.obj");

    Scene scene("Test");
    GameObject* go = scene.addGameObject("Prop");
    const uint64_t id = go->id;
    go->setMesh(std::make_shared<Mesh>(ModelLoader::load(unriggedPath)));

    nlohmann::json j = scene.toJson();
    std::filesystem::remove(unriggedPath);
    // Scene saved believing it was animated; the real file (the OBJ, which
    // never had bones) is no longer on disk when reloading.
    j["root"]["children"][0]["mesh"]["skinned"] = true;

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;

    // The file does not exist: there is no mesh to load, but the whole scene
    // stays alive (it does not crash, it does not abort the load).
    CHECK(!found->hasMesh());

    bool warnsMissingFile  = false;
    bool wronglyClaimsBones = false;
    for (const auto& w : loaded.lastWarnings())
    {
        if (w.find("dt_test_scene_file_missing.obj") != std::string::npos &&
            w.find("cannot be found") != std::string::npos)
            warnsMissingFile = true;
        if (w.find("no longer declares bones") != std::string::npos)
            wronglyClaimsBones = true;
    }
    CHECK(warnsMissingFile);
    CHECK(!wronglyClaimsBones);
}

// Review fix (task-3): the catch that wraps the mesh load swallowed any
// std::exception silently. With this, the warning above reports the reason
// (missing file); this test checks that ALSO the load's own catch (the one
// for ModelLoader::load throwing) leaves its own trace with the exception's
// message, not just the hasBones warning. It reuses the same scenario as the
// previous test.
static void test_scene_load_warns_on_load_exception(PhysicsManager& pm, AudioManager& am)
{
    const std::string unriggedPath = writeUnriggedObjFixture("dt_test_scene_load_exception.obj");

    Scene scene("Test");
    GameObject* go = scene.addGameObject("Prop");
    const uint64_t id = go->id;
    go->setMesh(std::make_shared<Mesh>(ModelLoader::load(unriggedPath)));

    nlohmann::json j = scene.toJson();
    std::filesystem::remove(unriggedPath);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(!found->hasMesh());

    bool warnsLoadFailure = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("dt_test_scene_load_exception.obj") != std::string::npos &&
            w.find("could not load the mesh") != std::string::npos)
            warnsLoadFailure = true;
    CHECK(warnsLoadFailure);
}

// Review finding (task-3): hasBonesCache is a PER-LOAD cache inside
// Scene::fromJson (see nodeFromJson in Scene.cpp) that avoids repeating the
// full Assimp ReadFile when several nodes share a sourcePath. All the other
// tests in this file use a single GameObject per sourcePath, so the cache-HIT
// branch (sourcePath already present in the map) was never exercised in the
// suite, only the miss one. This test deliberately creates TWO GameObjects
// pointing at the SAME rigged FBX: the first node populates the cache (miss)
// and the second one queries it (hit). If the cache stored the wrong key, the
// wrong value, or treated the hit as "no bones" by mistake, the second node
// would load as a plain Mesh while the first would load as SkinnedMesh; that
// asymmetry is exactly what this test detects. Do not simplify to a single
// node: that would lose the coverage the finding asks for.
static void test_scene_load_shares_has_bones_cache_across_nodes(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go1 = scene.addGameObject("Personaje1");
    GameObject* go2 = scene.addGameObject("Personaje2");
    const uint64_t id1 = go1->id;
    const uint64_t id2 = go2->id;

    go1->setMesh(std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx")));
    go2->setMesh(std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx")));

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    GameObject* found1 = loaded.findById(id1);
    GameObject* found2 = loaded.findById(id2);
    CHECK(found1 != nullptr);
    CHECK(found2 != nullptr);
    if (!found1 || !found2) return;

    // First node: MISS branch of the cache (it populates it).
    const SkinnedMesh* sm1 = found1->getSkinnedMesh();
    CHECK(sm1 != nullptr);
    if (sm1) CHECK(!sm1->skeleton.names.empty());

    // Second node: HIT branch of the cache. It must give the SAME answer as the
    // first, not a failed read or a static Mesh.
    const SkinnedMesh* sm2 = found2->getSkinnedMesh();
    CHECK(sm2 != nullptr);
    if (sm2) CHECK(!sm2->skeleton.names.empty());
}

// Final review finding: nothing exercised cloneGameObject with a rigged mesh.
// Its only caller is Lua's Scene.Instantiate, which runs inside the Play loop,
// and until the fix it passed hasBonesCache == nullptr, so every spawn probed
// the FBX with Assimp and parsed it again in full. The authoritative answer
// is already in memory (src->isSkinned()): if the clone came back as a plain
// Mesh (because the FBX is being re-exported at that instant, or because
// someone broke the cache seeding) the clone would lose the Animator while the
// original keeps it. That is exactly what this test detects.
static void test_clone_of_rigged_mesh_stays_skinned(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    go->setMesh(std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx")));
    CHECK(go->isSkinned());

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;

    CHECK(clone != go);
    CHECK(clone->isSkinned());
    const SkinnedMesh* sm = clone->getSkinnedMesh();
    CHECK(sm != nullptr);
    if (!sm) return;
    CHECK(!sm->skeleton.names.empty());
    CHECK(sm->sourcePath == "assets/modelAnimation.fbx");
}

// H14 of docs/core-audit.md, and the half that the test above was missing.
// That fix seeded the hasBones cache so the clone would not PROBE the FBX, but
// the skinned path still called ModelLoader::loadSkinned, that is, it reparsed
// the ENTIRE file from disk. In the middle of Play, once per spawn: the only
// caller of cloneGameObject is Lua's Scene.Instantiate.
//
// This does not check "it did not read disk" (that is not observable from
// here) but its consequence, which is what really matters and is also the
// argument already written by the comment at Scene.cpp:2726-2731 for the
// hasBones cache: the authoritative answer is IN MEMORY (the source object's
// mesh), so the clone cannot depend on the file still being there or still
// being the same. An artist re-exporting the FBX mid-game cannot change what
// an already loaded object is cloned from.
//
// The file is copied to a temp because the test DELETES it: no repo asset is
// touched for this.
static void test_clone_of_rigged_mesh_does_not_reread_disk(PhysicsManager& pm, AudioManager& am)
{
    const std::filesystem::path temporal =
        std::filesystem::temp_directory_path() / "dt_clone_sin_disco.fbx";
    std::error_code ec;
    std::filesystem::remove(temporal, ec);
    std::filesystem::copy_file("assets/modelAnimation.fbx", temporal, ec);
    CHECK(!ec);
    if (ec) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    go->setMesh(std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned(temporal.string())));
    CHECK(go->isSkinned());
    const SkinnedMesh* origen = go->getSkinnedMesh();
    CHECK(origen != nullptr);
    if (!origen) return;
    const size_t vertsOrigen = origen->vertices.size();
    const size_t clipsOrigen = origen->animationClips.size();
    const size_t huesosOrigen = origen->skeleton.names.size();

    // From here on the file is not needed: the mesh is already in memory.
    std::filesystem::remove(temporal, ec);
    CHECK(!std::filesystem::exists(temporal));

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;

    CHECK(clone->isSkinned());
    const SkinnedMesh* sm = clone->getSkinnedMesh();
    CHECK(sm != nullptr);
    if (!sm) return;
    // A real deep copy, not an empty skeleton that passes isSkinned().
    CHECK(sm->vertices.size() == vertsOrigen);
    CHECK(sm->animationClips.size() == clipsOrigen);
    CHECK(sm->skeleton.names.size() == huesosOrigen);
    // Since Appendix B they SHARE it: the clone does not copy the mesh until
    // someone edits it (editMesh).
    CHECK(sm == origen);
}

// P2 of docs/core-audit.md, the seventh raw access: the saved clip names of
// an animation source were read with get<std::vector<std::string>>(), so a
// single element that was not a string threw.
//
// NOTE: the audit counted it wrong and this was noticed while writing the
// test: here the scene was NOT lost. This access falls inside the try/catch
// of the mesh load (Scene.cpp:1683), so the exception was caught long before
// reaching fromJson's catch. The damage was different and worse to diagnose:
// the character was loaded **without a rig** (no Animator, no skeleton) and
// the only warning said "the mesh could not be loaded", without naming the
// field or the clip. A silent failure disguised as another. Verified by
// sabotaging the fix: the test fails on isSkinned(), not on fromJson().
//
// The list is discarded ENTIRELY, not element by element: names are applied
// POSITIONALLY (applyClipNamesPositionally), so skipping the bad element
// would shift all the following ones by one place and rename the wrong clips.
// A scene that loads "fine" with the animations swapped around is worse than
// one that warns.
static void test_corrupt_clip_names_does_not_lose_scene(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Origen");
    GameObject* go = scene.addGameObject("Personaje");
    go->setMesh(std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx")));
    scene.addGameObject("Sano");
    nlohmann::json j = scene.toJson();

    nlohmann::json& fuentes = j["root"]["children"][0]["mesh"]["animationSources"];
    CHECK(fuentes.is_array() && !fuentes.empty());
    if (!fuentes.is_array() || fuentes.empty()) return;
    fuentes[0]["clips"] = nlohmann::json::array({ "Caminar", 42 });

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    // The healthy node and the character are still there, and the character keeps its rig.
    CHECK(loaded.getRoot().children.size() == 2);
    if (loaded.getRoot().children.size() != 2) return;
    CHECK(loaded.getRoot().children[0]->isSkinned());
    CHECK(loaded.getRoot().children[1]->name == "Sano");

    int avisos = 0;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("clips") != std::string::npos) ++avisos;
    CHECK(avisos == 1);
}

// Same finding, the other path that passed nullptr: insertFromJson, which is
// the undo of a Delete. The full cycle of the command is reproduced (snapshot
// with subtreeToJson, delete, reinsert) because that is where the mesh type is
// decided again: the JSON does not rule (the "skinned" flag is no longer read),
// rig detection rebuilds it. A deleted and recovered character must come back
// skinned, or the undo would have silently removed its Animator.
static void test_delete_undo_restores_rigged_mesh_as_skinned(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    go->setMesh(std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx")));

    nlohmann::json snapshot = scene.subtreeToJson(go);
    scene.removeGameObject(go);
    CHECK(scene.findById(id) == nullptr);

    GameObject* restored = scene.insertFromJson(snapshot, nullptr, 0, pm, am);
    CHECK(restored != nullptr);
    if (!restored) return;

    // The id is preserved: later commands in the undo stack keep resolving
    // against the same GameObject.
    CHECK(restored->id == id);
    CHECK(restored->isSkinned());
    const SkinnedMesh* sm = restored->getSkinnedMesh();
    CHECK(sm != nullptr);
    if (!sm) return;
    CHECK(!sm->skeleton.names.empty());
}

// Review fix: a forcedName that is already in use (by an existing clip, or
// by an earlier forcedName of this same import) must NOT slip through as it
// is: it would leave two homonymous clips and the Animator resolves by name,
// so one of the two would become unreachable. It must fall into uniqueClipName
// and warn. The collision is forced by reusing the builtin clip's name as the
// forcedName of the added source.
static void test_add_animation_source_with_forced_names_collision()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const size_t builtinCount = m.animationClips.size();
    CHECK(builtinCount >= 1u);
    if (builtinCount < 1u) return;

    const std::string collidingName = m.animationClips[0].name;

    std::vector<std::string> forced;
    forced.push_back(collidingName);            // collides with the builtin clip
    for (size_t i = 1; i < builtinCount; i++)
        forced.push_back("Extra" + std::to_string(i));

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(m, "assets/modelAnimation.fbx", warnings, &forced));

    // (a) No repeated name among ALL the mesh's clips
    for (size_t i = 0; i < m.animationClips.size(); i++)
        for (size_t j = i + 1; j < m.animationClips.size(); j++)
            CHECK(m.animationClips[i].name != m.animationClips[j].name);

    // (b) the source's clipNames reflects exactly what ended up in
    // animationClips, not the forcedNames requested blindly
    CHECK(m.animationSources.size() == 2u);
    CHECK(m.animationSources[1].clipNames.size() == builtinCount);
    for (size_t i = 0; i < m.animationSources[1].clipNames.size(); i++)
        CHECK(m.animationClips[builtinCount + i].name == m.animationSources[1].clipNames[i]);

    // The colliding forcedName did not slip through bare: it carries the " (N)"
    // suffix that uniqueClipName adds instead of the exact requested name.
    CHECK(m.animationSources[1].clipNames[0] == collidingName + " (1)");

    // (c) The forced rename was warned about
    CHECK(!warnings.empty());
}

// The keyframes of the N clips are concatenated in the same vectors and
// boneInfos ends up in a [clip][bone] layout. parentIndex/inverseBindPose belong
// to the skeleton, not the clip: identical in all blocks, which is what leaves
// bone_hierarchy.comp unchanged.
static void test_pack_concatenates_clips()
{
    SkinnedMesh m;
    m.skeleton.names           = { "root", "child" };
    m.skeleton.parentIndex     = { -1, 0 };
    m.skeleton.inverseBindPose = { glm::mat4(2.0f), glm::mat4(3.0f) };

    // 3 clips; only bone 0 has a channel, with 1, 2 and 3 keys respectively.
    // The position value carries the clip index as a marker.
    for (int c = 0; c < 3; c++)
    {
        AnimationClip clip;
        clip.name           = "clip" + std::to_string(c);
        clip.duration       = 10.0f * (float)(c + 1);
        clip.ticksPerSecond = 24.0f;

        BoneChannel ch;
        ch.boneIndex = 0;
        for (int k = 0; k <= c; k++)
        {
            ch.posKeys.push_back({ (float)k, glm::vec3((float)c) });
            ch.rotKeys.push_back({ (float)k, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
            ch.scaleKeys.push_back({ (float)k, glm::vec3(1.0f) });
        }
        clip.channels.push_back(ch);
        m.animationClips.push_back(clip);
    }

    PackedClips p = packSkinnedClips(m);

    // C*B entries
    CHECK(p.boneInfos.size() == 3u * 2u);
    // 1+2+3 keys, concatenated without overlapping
    CHECK(p.pos.size() == 6u);
    CHECK(p.rot.size() == 6u);
    CHECK(p.scale.size() == 6u);

    // [clip][bone] layout: bone 0 of clip c is at c*2 + 0
    CHECK(p.boneInfos[0 * 2 + 0].posCount == 1);
    CHECK(p.boneInfos[1 * 2 + 0].posCount == 2);
    CHECK(p.boneInfos[2 * 2 + 0].posCount == 3);
    // Bone 1 has no channel in any clip
    CHECK(p.boneInfos[0 * 2 + 1].posCount == 0);
    CHECK(p.boneInfos[2 * 2 + 1].posCount == 0);

    // Increasing offsets with no overlap
    CHECK(p.boneInfos[0 * 2 + 0].posOffset == 0);
    CHECK(p.boneInfos[1 * 2 + 0].posOffset == 1);
    CHECK(p.boneInfos[2 * 2 + 0].posOffset == 3);

    // Skeleton data replicated identically in each clip block
    for (int c = 0; c < 3; c++)
    {
        CHECK(p.boneInfos[c * 2 + 0].parentIndex == -1);
        CHECK(p.boneInfos[c * 2 + 1].parentIndex == 0);
        CHECK(p.boneInfos[c * 2 + 0].inverseBindPose == glm::mat4(2.0f));
        CHECK(p.boneInfos[c * 2 + 1].inverseBindPose == glm::mat4(3.0f));
    }

    // The block of clip 2 points at ITS keys, not at those of clip 0
    CHECK(nearlyEqual(p.pos[p.boneInfos[2 * 2 + 0].posOffset].value.x, 2.0f));
    CHECK(nearlyEqual(p.pos[p.boneInfos[0 * 2 + 0].posOffset].value.x, 0.0f));
}

// Mesh without animations: one clip block with all counts at 0, and the
// buffers never empty (Vulkan does not accept zero-size buffers).
static void test_pack_mesh_without_clips()
{
    SkinnedMesh m;
    m.skeleton.names           = { "root" };
    m.skeleton.parentIndex     = { -1 };
    m.skeleton.inverseBindPose = { glm::mat4(1.0f) };

    PackedClips p = packSkinnedClips(m);

    CHECK(p.boneInfos.size() == 1u);
    CHECK(p.boneInfos[0].posCount == 0);
    CHECK(p.boneInfos[0].parentIndex == -1);
    CHECK(p.pos.size() == 1u);     // dummy
    CHECK(p.rot.size() == 1u);
    CHECK(p.scale.size() == 1u);
}

// CPU replica of slerpQ from bone_eval.comp, including the negation for a
// negative dot product: glm::slerp does not necessarily take the same path, and
// here what matters is reproducing what the GPU computes, not what is equivalent.
static glm::vec4 slerpQ(glm::vec4 a, glm::vec4 b, float t)
{
    float c = glm::dot(a, b);
    if (c < 0.0f) { b = -b; c = -c; }
    if (c > 0.9995f) return glm::normalize(glm::mix(a, b, t));
    const float angle = std::acos(c);
    const float is    = 1.0f / std::sin(angle);
    return (std::sin((1.0f - t) * angle) * a + std::sin(t * angle) * b) * is;
}

// CPU replica of bone_eval.comp: the local transform of bone i inside the clip
// block that starts at clipBase, evaluated at instant T. Same linear search for
// the segment and same interpolation as the shader, so that what the tests
// assert is what the GPU actually ends up computing.
// lockRootMotion replicates the root bone override: with the flag active the
// translation of the parentless bone goes back to that of its bind pose (all
// three axes), without touching rotation or scale.
static glm::mat4 evalLocalXform(const PackedClips& p, size_t clipBase, size_t i, float T,
                                bool lockRootMotion = false)
{
    const GpuBoneInfo& info = p.boneInfos[clipBase + i];

    // A bone about which the clip says absolutely nothing: it keeps its bind pose
    // local. Putting the identity here would collapse it onto the origin and the
    // orientation of its parent; see test_bone_without_channel_keeps_bind_pose.
    if (info.posCount == 0 && info.rotCount == 0 && info.scaleCount == 0)
        return info.bindLocal;

    glm::vec3 pos(0.0f);
    if (info.posCount > 0)
    {
        int lo = info.posOffset, hi = info.posOffset + info.posCount - 1;
        for (int k = info.posOffset; k < info.posOffset + info.posCount - 1; k++)
            if (p.pos[(size_t)k + 1].timePad.x > T) { lo = k; hi = k + 1; break; }
        const float t0 = p.pos[(size_t)lo].timePad.x, t1 = p.pos[(size_t)hi].timePad.x;
        const float f  = (t1 > t0) ? glm::clamp((T - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
        pos = glm::mix(glm::vec3(p.pos[(size_t)lo].value), glm::vec3(p.pos[(size_t)hi].value), f);
    }

    glm::vec4 rot(0.0f, 0.0f, 0.0f, 1.0f);
    if (info.rotCount > 0)
    {
        int lo = info.rotOffset, hi = info.rotOffset + info.rotCount - 1;
        for (int k = info.rotOffset; k < info.rotOffset + info.rotCount - 1; k++)
            if (p.rot[(size_t)k + 1].timePad.x > T) { lo = k; hi = k + 1; break; }
        const float t0 = p.rot[(size_t)lo].timePad.x, t1 = p.rot[(size_t)hi].timePad.x;
        const float f  = (t1 > t0) ? glm::clamp((T - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
        rot = slerpQ(p.rot[(size_t)lo].value, p.rot[(size_t)hi].value, f);
    }

    glm::vec3 scl(1.0f);
    if (info.scaleCount > 0)
    {
        int lo = info.scaleOffset, hi = info.scaleOffset + info.scaleCount - 1;
        for (int k = info.scaleOffset; k < info.scaleOffset + info.scaleCount - 1; k++)
            if (p.scale[(size_t)k + 1].timePad.x > T) { lo = k; hi = k + 1; break; }
        const float t0 = p.scale[(size_t)lo].timePad.x, t1 = p.scale[(size_t)hi].timePad.x;
        const float f  = (t1 > t0) ? glm::clamp((T - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
        scl = glm::mix(glm::vec3(p.scale[(size_t)lo].value), glm::vec3(p.scale[(size_t)hi].value), f);
    }

    // trs(): rotation by quaternion, scaled columns, translation in the 4th.
    // The shader stores the quaternion as (x,y,z,w) and glm::quat is constructed
    // (w,x,y,z); swapping this would give a different rotation without failing anything.
    if (lockRootMotion && info.parentIndex < 0)
        pos = glm::vec3(info.bindLocal[3]);

    glm::mat4 out = glm::mat4_cast(glm::quat(rot.w, rot.x, rot.y, rot.z));
    out[0] *= scl.x;
    out[1] *= scl.y;
    out[2] *= scl.z;
    out[3] = glm::vec4(pos, 1.0f);
    return out;
}

// ── CPU replica of bone_eval by samples ─────────────────────────────────────
//
// TRS of a bone. bind = "nothing animates this bone: it equals its bindLocal"
// (the shader marks it with scale.w = 0 in poseTrs).
struct Trs { glm::vec3 p{0.0f}; glm::vec4 q{0.0f, 0.0f, 0.0f, 1.0f}; glm::vec3 s{1.0f}; bool bind = false; };

// Samples bone i of block clipBase at T. false if the clip has no channel for
// that bone (then `out` stays at identity, as in the shader).
static bool sampleTrs(const PackedClips& p, size_t clipBase, size_t i, float T, Trs& out)
{
    out = Trs{};
    const GpuBoneInfo& info = p.boneInfos[clipBase + i];
    if (info.posCount == 0 && info.rotCount == 0 && info.scaleCount == 0) return false;
    auto tramo = [&](int off, int count, auto clave) {
        int lo = off, hi = off + count - 1;
        for (int k = off; k < off + count - 1; k++)
            if (clave(k + 1) > T) { lo = k; hi = k + 1; break; }
        const float t0 = clave(lo), t1 = clave(hi);
        const float f  = (t1 > t0) ? glm::clamp((T - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
        return std::make_tuple(lo, hi, f);
    };
    if (info.posCount > 0)
    {
        auto [lo, hi, f] = tramo(info.posOffset, info.posCount, [&](int k) { return p.pos[(size_t)k].timePad.x; });
        out.p = glm::mix(glm::vec3(p.pos[(size_t)lo].value), glm::vec3(p.pos[(size_t)hi].value), f);
    }
    if (info.rotCount > 0)
    {
        auto [lo, hi, f] = tramo(info.rotOffset, info.rotCount, [&](int k) { return p.rot[(size_t)k].timePad.x; });
        out.q = slerpQ(p.rot[(size_t)lo].value, p.rot[(size_t)hi].value, f);
    }
    if (info.scaleCount > 0)
    {
        auto [lo, hi, f] = tramo(info.scaleOffset, info.scaleCount, [&](int k) { return p.scale[(size_t)k].timePad.x; });
        out.s = glm::mix(glm::vec3(p.scale[(size_t)lo].value), glm::vec3(p.scale[(size_t)hi].value), f);
    }
    return true;
}

// What bone_eval computes for bone i with an AnimationPose: weighted sum of the
// samples (and of the frozen one), rotation with its sign aligned to the first
// one and normalized, and the root lock at the end. With boneCount = 1 and
// clip = clipBase it also works for standalone blocks.
static Trs evalPoseTrs(const PackedClips& p, size_t boneCount, size_t i, const AnimationPose& pose,
                       const std::vector<Trs>* frozen)
{
    Trs acc; acc.p = glm::vec3(0.0f); acc.s = glm::vec3(0.0f);
    glm::vec4 q(0.0f), ref(0.0f, 0.0f, 0.0f, 1.0f);
    bool hayRef = false, alguna = false;
    float total = 0.0f;
    auto sumar = [&](const Trs& t, float w) {
        glm::vec4 qk = t.q;
        if (!hayRef) { ref = qk; hayRef = true; }
        if (glm::dot(qk, ref) < 0.0f) qk = -qk;
        acc.p += w * t.p; acc.s += w * t.s; q += w * qk; total += w;
    };
    for (int k = 0; k < pose.count; k++)
    {
        Trs t;
        if (sampleTrs(p, (size_t)pose.samples[k].clip * boneCount, i, pose.samples[k].time, t)) alguna = true;
        sumar(t, pose.samples[k].weight);
    }
    if (pose.layers[0].frozenWeight > 0.0f && frozen && i < frozen->size() && !(*frozen)[i].bind)
    {
        sumar((*frozen)[i], pose.layers[0].frozenWeight);
        alguna = true;
    }
    if (!alguna) { Trs b; b.bind = true; return b; }
    // If the frozen one does not enter (its bone was bind), the rest is renormalized.
    if (total > 0.0f && std::fabs(total - 1.0f) > 1e-4f) { acc.p /= total; acc.s /= total; q /= total; }
    const float len = glm::length(q);
    acc.q = len > 1e-6f ? q / len : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const size_t base = (size_t)(pose.count > 0 ? pose.samples[0].clip : 0) * boneCount;
    const GpuBoneInfo& info = p.boneInfos[base + i];
    if (info.parentIndex < 0)
    {
        if (pose.rootMotionMode == 1u) acc.p = glm::vec3(info.bindLocal[3]);
        if (pose.rootMotionMode == 2u) acc.p = glm::vec3(info.bindLocal[3].x, acc.p.y, info.bindLocal[3].z);
    }
    return acc;
}

static glm::mat4 trsToMat(const Trs& t, const GpuBoneInfo& info)
{
    if (t.bind) return info.bindLocal;
    glm::mat4 out = glm::mat4_cast(glm::quat(t.q.w, t.q.x, t.q.y, t.q.z));
    out[0] *= t.s.x;
    out[1] *= t.s.y;
    out[2] *= t.s.z;
    out[3] = glm::vec4(t.p, 1.0f);
    return out;
}

// CPU replica of bone_hierarchy.comp: the two passes, as they are. It is used to
// check the matrix that skinning.comp ends up seeing without needing a VkDevice.
static std::vector<glm::mat4> runBoneHierarchy(const PackedClips& p, size_t clipBase,
                                               size_t boneCount, float T,
                                               bool lockRootMotion = false)
{
    std::vector<glm::mat4> final(boneCount, glm::mat4(1.0f));

    // Pass 1: world transform (the topological order guarantees parent < child)
    for (size_t i = 0; i < boneCount; i++)
    {
        const glm::mat4 local  = evalLocalXform(p, clipBase, i, T, lockRootMotion);
        const int       parent = p.boneInfos[clipBase + i].parentIndex;
        final[i] = (parent < 0) ? local : final[(size_t)parent] * local;
    }

    // Pass 2: inverse bind pose
    for (size_t i = 0; i < boneCount; i++)
        final[i] = final[i] * p.boneInfos[clipBase + i].inverseBindPose;

    return final;
}

static bool nearlyEqualMat(const glm::mat4& a, const glm::mat4& b, float eps = 0.001f)
{
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            if (!nearlyEqual(a[c][r], b[c][r], eps)) return false;
    return true;
}

static bool nearlyIdentity(const glm::mat4& m, float eps = 0.001f)
{
    return nearlyEqualMat(m, glm::mat4(1.0f), eps);
}

// skinnedVertices stores positions in bind pose (which is what Assimp gives),
// so the skinning matrix maps bind pose -> current pose. With no animation at
// all the current pose IS the bind pose: the correct matrix is the identity and
// the mesh must come out exactly as its stored vertices. If finalBones ends up
// being the inverse bind pose, the character looks deformed.
//
// This case has no mechanism of its own: it is the degenerate extreme of the
// general rule pinned by test_bone_without_channel_keeps_bind_pose. With no
// clips, ALL the bones have no channel, so all of them receive their bind pose
// local, pass 1 rebuilds exactly the bind pose global and pass 2 multiplies it
// by its inverse: identity, by construction and not by a special case. That
// this test stays green with the REAL inverse bind pose in boneInfos is the
// proof that the general rule covers it.
static void test_pack_without_clips_yields_identity_final_bones()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    CHECK(!m.skeleton.names.empty());
    m.animationClips.clear(); // rigged FBX but without animations

    const size_t boneCount = m.skeleton.names.size();
    PackedClips  p         = packSkinnedClips(m);
    CHECK(p.boneInfos.size() == boneCount);

    // With no clips there is not a single key: it is the precondition of the case.
    for (size_t i = 0; i < boneCount; i++)
        CHECK(p.boneInfos[i].posCount == 0 && p.boneInfos[i].rotCount == 0 &&
              p.boneInfos[i].scaleCount == 0);

    std::vector<glm::mat4> final = runBoneHierarchy(p, 0, boneCount, 0.0f);

    int malas = 0;
    for (size_t i = 0; i < boneCount; i++)
    {
        if (nearlyIdentity(final[i])) continue;
        if (malas == 0)
        {
            std::printf("  hueso %zu (%s) no es identidad:\n", i, m.skeleton.names[i].c_str());
            for (int r = 0; r < 4; r++)
                std::printf("    [% .4f % .4f % .4f % .4f]\n",
                            final[i][0][r], final[i][1][r], final[i][2][r], final[i][3][r]);
        }
        ++malas;
    }
    CHECK(malas == 0);
}

// THE render bug: a clip does NOT have to carry a channel for every bone of the
// skeleton. For a bone without a channel packSkinnedClips leaves the three
// counts at 0, and bone_eval.comp translated those zeros into an IDENTITY local
// transform. An identity local is not "still": it erases the bone's offset
// relative to its parent, so the bone collapses onto the origin and the
// orientation of the parent and takes its whole descendant chain down with it.
// In the user's asset it was mixamorig:RightShoulder (1262 vertices, no channel)
// that left the entire right arm hanging at head height.
//
// The suite did not catch it because modelAnimation.fbx avoids it by chance:
// its 13 bones without a channel are tips (*_End, *4) that influence 0 vertices.
// That is why the case is built by hand here: the channel is removed from a
// bone that DOES have children and DOES deform geometry, and from its whole
// subtree, which is exactly the way the bug showed up.
//
// The property pinned is "the unanimated member stays where it was rigged": a
// bone without a channel travels RIGIDLY with its parent, keeping the bind pose
// offset. Formally final[b] == final[parent(b)], because
//   final[b] = global[parent] * localBind[b] * invBind[b]
// and localBind[b] * invBind[b] == invBind[parent] by definition of localBind.
// This formulation is chosen because it is EXACT with the real animated clip,
// without depending on the instant or on decomposing any matrix into TRS. Its
// visible consequence (and it is what the user saw broken) is that the
// descendant lands where it belongs relative to that parent, not half a screen away.
static void test_bone_without_channel_keeps_bind_pose()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    CHECK(!m.skeleton.names.empty());
    CHECK(!m.animationClips.empty());
    if (m.skeleton.names.empty() || m.animationClips.empty()) return;

    const size_t boneCount = m.skeleton.names.size();

    // Vertices influenced per bone: the bug is only visible in those that deform
    // geometry, and that is exactly what the case the suite already had was missing.
    std::vector<int> verts(boneCount, 0);
    for (const auto& v : m.skinnedVertices)
        for (int s = 0; s < 4; s++)
            if (v.boneWeights[s] > 0.0f && v.boneIndices[s] >= 0 &&
                (size_t)v.boneIndices[s] < (int)boneCount)
                verts[(size_t)v.boneIndices[s]]++;

    AnimationClip& clip = m.animationClips[0];
    std::vector<bool> tieneCanal(boneCount, false);
    for (const auto& ch : clip.channels)
        if (ch.boneIndex >= 0 && (size_t)ch.boneIndex < boneCount)
            tieneCanal[(size_t)ch.boneIndex] = true;

    // Subtree size. parentIndex is in topological order (parent < child), so a
    // backwards walk accumulates it in a single pass.
    std::vector<int> subtree(boneCount, 1);
    for (size_t i = boneCount; i-- > 1; )
    {
        const int par = m.skeleton.parentIndex[i];
        if (par >= 0) subtree[(size_t)par] += subtree[i];
    }

    // B: bone with a channel, with a parent, that deforms geometry and from which
    // the longest chain hangs; the analogue of the RightShoulder that broke the real asset.
    int B = -1;
    for (size_t i = 0; i < boneCount; i++)
        if (m.skeleton.parentIndex[i] >= 0 && tieneCanal[i] && verts[i] > 0 && subtree[i] > 1)
            if (B < 0 || subtree[i] > subtree[(size_t)B]) B = (int)i;
    CHECK(B >= 0);
    if (B < 0) return;

    const int P = m.skeleton.parentIndex[(size_t)B];
    // The parent must remain animated: otherwise there would be nothing to
    // "follow" and the test would pass for the wrong reason.
    CHECK(tieneCanal[(size_t)P]);

    std::vector<bool> enSubarbol(boneCount, false);
    enSubarbol[(size_t)B] = true;
    for (size_t i = (size_t)B + 1; i < boneCount; i++)
    {
        const int par = m.skeleton.parentIndex[i];
        if (par >= 0 && enSubarbol[(size_t)par]) enSubarbol[i] = true;
    }

    // D: the deepest descendant of B that deforms geometry, the "finger"
    // whose position gives away the collapse.
    std::vector<int> depth(boneCount, 0);
    int D = -1;
    for (size_t i = 0; i < boneCount; i++)
    {
        const int par = m.skeleton.parentIndex[i];
        depth[i] = (par >= 0) ? depth[(size_t)par] + 1 : 0;
        if (enSubarbol[i] && (int)i != B && verts[i] > 0)
            if (D < 0 || depth[i] > depth[(size_t)D]) D = (int)i;
    }
    CHECK(D >= 0);
    if (D < 0) return;

    // The channels of B and of its whole subtree are removed: the entire member
    // ends up with no animation of its own.
    std::vector<BoneChannel> quedan;
    for (const auto& ch : clip.channels)
        if (ch.boneIndex < 0 || (size_t)ch.boneIndex >= boneCount ||
            !enSubarbol[(size_t)ch.boneIndex])
            quedan.push_back(ch);
    CHECK(quedan.size() < clip.channels.size());
    clip.channels = quedan;

    PackedClips p = packSkinnedClips(m);
    CHECK(p.boneInfos.size() == boneCount);
    CHECK(p.boneInfos[(size_t)B].posCount == 0);
    CHECK(p.boneInfos[(size_t)P].posCount > 0);   // the parent is animated

    std::vector<glm::mat4> final = runBoneHierarchy(p, 0, boneCount, 0.0f);

    // Visible consequence: the origin of D in bind pose, deformed by its own final
    // matrix, must land where the matrix of the animated parent P takes it.
    const glm::vec3 bindD    = glm::vec3(glm::inverse(m.skeleton.inverseBindPose[(size_t)D])[3]);
    const glm::vec3 esperado = glm::vec3(final[(size_t)P] * glm::vec4(bindD, 1.0f));
    const glm::vec3 real     = glm::vec3(final[(size_t)D] * glm::vec4(bindD, 1.0f));
    const float     dist     = glm::length(real - esperado);

    if (dist > 0.2f)
        std::printf("  %s (sin canal, bajo %s): esperado (%.3f %.3f %.3f), real (%.3f %.3f %.3f)"
                    " -> desplazado %.3f unidades\n",
                    m.skeleton.names[(size_t)D].c_str(), m.skeleton.names[(size_t)P].c_str(),
                    esperado.x, esperado.y, esperado.z, real.x, real.y, real.z, dist);
    CHECK(dist < 0.2f);

    // Threshold deliberately loose: localBind is obtained by inverting the inverse
    // bind pose, and with translations of ~180 units the float error is around
    // 1e-3. The failure the test hunts is tens of units.
    CHECK(nearlyEqualMat(final[(size_t)B], final[(size_t)P], 0.05f));
    CHECK(nearlyEqualMat(final[(size_t)D], final[(size_t)P], 0.05f));
}

// Helper: graph Idle(loop) -> Run(loop) -> Jump(no loop) -> Idle.
// Idle->Run   by bool "running" == true
// Run->Jump   by trigger "jump"
// Jump->Idle  by animation finished
// Run->Idle   by bool "running" == false (closes the bool cycle, so the
//             expected == false case is also exercised)
static AnimatorComponent makeGraph()
{
    AnimatorComponent a;

    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "Idle";
    idle.clipIndex = 0; idle.duration = 30.0f; idle.ticksPerSecond = 30.0f; idle.loop = true;

    AnimatorComponent::State run;
    run.name = "Run"; run.clipName = "Run";
    run.clipIndex = 1; run.duration = 20.0f; run.ticksPerSecond = 20.0f; run.loop = true;

    AnimatorComponent::State jump;
    jump.name = "Jump"; jump.clipName = "Jump";
    jump.clipIndex = 2; jump.duration = 10.0f; jump.ticksPerSecond = 10.0f; jump.loop = false;

    a.addState(idle);
    a.addState(run);
    a.addState(jump);
    a.setEntryState(0);

    a.addParameter("running", AnimatorComponent::ParamType::Bool);
    a.addParameter("jump",    AnimatorComponent::ParamType::Trigger);

    AnimatorComponent::Transition t0;
    t0.fromState = 0; t0.toState = 1;
    t0.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "running", true });
    a.addTransition(t0);

    AnimatorComponent::Transition t1;
    t1.fromState = 1; t1.toState = 2;
    t1.conditions.push_back({ AnimatorComponent::ConditionType::Trigger, "jump", true });
    a.addTransition(t1);

    AnimatorComponent::Transition t2;
    t2.fromState = 2; t2.toState = 0;
    t2.conditions.push_back({ AnimatorComponent::ConditionType::AnimationFinished, "", true });
    a.addTransition(t2);

    AnimatorComponent::Transition t3;
    t3.fromState = 1; t3.toState = 0;
    t3.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "running", false });
    a.addTransition(t3);

    a.reset();
    return a;
}

// Criterion 2, part 1: a trigger changes the active state; without a trigger, it does not.
static void test_trigger_switches_state()
{
    AnimatorComponent a = makeGraph();
    CHECK(a.currentState() == 0);           // entry

    // With no parameters, nothing fires
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);

    // bool running -> Run
    a.setBool("running", true);
    a.update(0.016f, true);
    CHECK(a.currentState() == 1);
    CHECK(a.currentClipIndex() == 1);

    // trigger jump -> Jump
    a.setTrigger("jump");
    a.update(0.016f, true);
    CHECK(a.currentState() == 2);
    CHECK(a.currentClipIndex() == 2);
    // On entering a state, the time starts from zero
    CHECK(nearlyEqual(a.animTime(), 0.0f));
}

// Criterion 2, part 2: "animation finished" does NOT fire before duration and DOES
// after. Jump: duration 10 ticks at 10 tps = 1 real second.
static void test_animation_finished_timing()
{
    AnimatorComponent a = makeGraph();
    a.setBool("running", true);
    a.update(0.016f, true);          // -> Run
    a.setTrigger("jump");
    a.update(0.016f, true);          // -> Jump
    CHECK(a.currentState() == 2);

    // 0.9 s < 1.0 s of duration: not finished, no transition
    a.update(0.9f, true);
    CHECK(!a.finished());
    CHECK(a.currentState() == 2);

    // Past the end: finished and transition to Idle
    a.update(0.2f, true);
    CHECK(a.currentState() == 0);
}

// Criterion 2, part 3: loop=false sticks to the last frame; loop=true restarts.
static void test_loop_flag()
{
    // loop = false: animTime is clamped to duration and stays there
    AnimatorComponent noLoop;
    AnimatorComponent::State s;
    s.name = "Once"; s.clipName = "Once";
    s.clipIndex = 0; s.duration = 10.0f; s.ticksPerSecond = 10.0f; s.loop = false;
    noLoop.addState(s);
    noLoop.setEntryState(0);
    noLoop.reset();

    noLoop.update(2.0f, true);       // 20 ticks > 10 of duration
    CHECK(nearlyEqual(noLoop.animTime(), 10.0f));
    CHECK(noLoop.finished());
    noLoop.update(2.0f, true);       // still stuck, does not go back to the start
    CHECK(nearlyEqual(noLoop.animTime(), 10.0f));
    CHECK(noLoop.finished());

    // loop = true: restarts and is never marked finished
    AnimatorComponent looping;
    AnimatorComponent::State l;
    l.name = "Cycle"; l.clipName = "Cycle";
    l.clipIndex = 0; l.duration = 10.0f; l.ticksPerSecond = 10.0f; l.loop = true;
    looping.addState(l);
    looping.setEntryState(0);
    looping.reset();

    looping.update(1.2f, true);      // 12 ticks -> fmod -> 2
    CHECK(nearlyEqual(looping.animTime(), 2.0f));
    CHECK(!looping.finished());
}

// Bool condition: fires when expected is met, not with the opposite. There AND
// back: if expected were ignored (e.g. treating Bool as a plain "getBool(param)
// == true"), Idle->Run would keep working but Run->Idle would never fire with
// running == false, so this second leg is the one that really proves that
// expected == false is honored.
static void test_bool_condition_expected()
{
    AnimatorComponent a = makeGraph();
    a.setBool("running", false);
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);    // expected == true, running == false

    a.setBool("running", true);
    a.update(0.016f, true);
    CHECK(a.currentState() == 1);    // Idle -> Run: expected == true, running == true

    a.setBool("running", false);
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);    // Run -> Idle: expected == false, running == false
}

// The trigger consumed by the winning transition is turned off. One that is not
// consumed stays on, waiting (same behavior as Unity).
static void test_trigger_consumption()
{
    AnimatorComponent a = makeGraph();

    // "jump" set in Idle: no transition leaving Idle consumes it, so it is still
    // armed when we get to Run.
    a.setTrigger("jump");
    a.setBool("running", true);
    a.update(0.016f, true);          // Idle -> Run (by the bool)
    CHECK(a.currentState() == 1);

    a.update(0.016f, true);          // Run -> Jump: the trigger was still armed
    CHECK(a.currentState() == 2);

    // Already consumed: on going back to Idle and then to Run, it does not jump again
    a.update(2.0f, true);            // Jump ends -> Idle
    CHECK(a.currentState() == 0);
    a.update(0.016f, true);          // -> Run (running is still true)
    CHECK(a.currentState() == 1);
    a.update(0.016f, true);          // stays: the trigger is already spent
    CHECK(a.currentState() == 1);
}

// evaluateTransitions == false (Edit Mode): time advances but the graph does not
// move. Without this, the "animation finished" conditions would walk the graph
// around only in the editor.
static void test_edit_mode_does_not_transition()
{
    AnimatorComponent a = makeGraph();
    a.setBool("running", true);
    a.setTrigger("jump");

    a.update(0.5f, false);
    CHECK(a.currentState() == 0);            // has not moved
    CHECK(a.animTime() > 0.0f);              // but time runs

    a.update(0.016f, true);
    CHECK(a.currentState() == 1);            // in Play it does
}

// Fix #2 (task-16): editorId identifies a state independently of its position
// in the vector. Without this, the AnimatorPanel derives the canvas node id from
// the vector index, and deleting a state in the middle reindexes the vector: the
// survivor that inherits the deleted one's index also inherits its node id, and
// since imgui-node-editor caches position/selection PER id, that survivor
// "jumps" to the position/visual state of the deleted node. This test
// discriminates exactly that: if editorId were == index (the bug), after
// deleting the middle state the survivor that goes from index 2 to 1
// would change id (2 -> 1) and this CHECK would fail.
static void test_addstate_assigns_stable_unique_editor_ids()
{
    AnimatorComponent a;

    AnimatorComponent::State sa; sa.name = "A"; sa.clipName = "A";
    AnimatorComponent::State sb; sb.name = "B"; sb.clipName = "B";
    AnimatorComponent::State sc; sc.name = "C"; sc.clipName = "C";

    a.addState(sa);
    a.addState(sb);
    a.addState(sc);

    const int idA = a.states()[0].editorId;
    const int idB = a.states()[1].editorId;
    const int idC = a.states()[2].editorId;

    // Freshly assigned: unique among themselves.
    CHECK(idA != idB);
    CHECK(idB != idC);
    CHECK(idA != idC);

    a.removeState(1);    // deletes "B", the vector reindexes: A stays at
                          // 0 (it already was) and C goes from index 2 to 1.
    CHECK(a.states().size() == 2u);
    CHECK(a.states()[0].name == "A");
    CHECK(a.states()[1].name == "C");

    // Identity, not position: A keeps idA (index unchanged) and C keeps
    // idC EVEN THOUGH its index changed from 2 to 1. With ids derived from the
    // index, this second CHECK would fail (C would have the id that used to be B's).
    CHECK(a.states()[0].editorId == idA);
    CHECK(a.states()[1].editorId == idC);
    CHECK(a.states()[0].editorId != a.states()[1].editorId);

    // A state added after the deletion gets a fresh id, different from all those
    // still alive (the counter does not go back or reuse the id that was
    // freed when deleting B).
    AnimatorComponent::State sd; sd.name = "D"; sd.clipName = "D";
    a.addState(sd);
    const int idD = a.states()[2].editorId;
    CHECK(idD != idA);
    CHECK(idD != idC);
}

// Deleting a state reindexes the transitions that pointed above it and drops those
// that touched it. Without this, deleting a node would leave links pointing at another state.
static void test_remove_state_reindexes()
{
    AnimatorComponent a = makeGraph();
    a.removeState(1);                        // deletes "Run"

    CHECK(a.states().size() == 2u);
    CHECK(a.states()[0].name == "Idle");
    CHECK(a.states()[1].name == "Jump");
    // Idle->Run and Run->Jump go away; Jump->Idle survives reindexed (2 -> 1)
    CHECK(a.transitions().size() == 1u);
    CHECK(a.transitions()[0].fromState == 1);
    CHECK(a.transitions()[0].toState == 0);
    CHECK(a.entryState() == 0);
}

// Declaration order: if two transitions leaving the same state can be
// satisfied at once, the one declared first wins, not the last one the loop
// visits. Standalone graph (not makeGraph): A has two exits toward B and C,
// both with the same condition "flag" == true.
static void test_first_matching_transition_wins()
{
    AnimatorComponent a;

    AnimatorComponent::State sa;
    sa.name = "A"; sa.clipName = "A"; sa.clipIndex = 0;

    AnimatorComponent::State sb;
    sb.name = "B"; sb.clipName = "B"; sb.clipIndex = 1;

    AnimatorComponent::State sc;
    sc.name = "C"; sc.clipName = "C"; sc.clipIndex = 2;

    a.addState(sa);
    a.addState(sb);
    a.addState(sc);
    a.setEntryState(0);

    a.addParameter("flag", AnimatorComponent::ParamType::Bool);

    // Declared FIRST: A -> B
    AnimatorComponent::Transition toB;
    toB.fromState = 0; toB.toState = 1;
    toB.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "flag", true });
    a.addTransition(toB);

    // Declared AFTER, with the same condition: A -> C
    AnimatorComponent::Transition toC;
    toC.fromState = 0; toC.toState = 2;
    toC.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "flag", true });
    a.addTransition(toC);

    a.reset();
    a.setBool("flag", true);         // both conditions are met at once
    a.update(0.016f, true);

    CHECK(a.currentState() == 1);    // B wins: it is the first in declaration order
}

// A transition without conditions never fires (exit time is out of
// scope). Standalone graph of two states with a single link with no
// conditions: after several update() calls, the graph must still be in the
// entry state.
static void test_transition_without_conditions_never_fires()
{
    AnimatorComponent a;

    AnimatorComponent::State s0;
    s0.name = "X"; s0.clipName = "X"; s0.clipIndex = 0;

    AnimatorComponent::State s1;
    s1.name = "Y"; s1.clipName = "Y"; s1.clipIndex = 1;

    a.addState(s0);
    a.addState(s1);
    a.setEntryState(0);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;    // no conditions
    a.addTransition(t);

    a.reset();
    for (int i = 0; i < 5; i++)
        a.update(0.1f, true);

    CHECK(a.currentState() == 0);      // stays in X: the empty link never fires
}

// removeParameter("") must not touch the AnimationFinished conditions: their
// paramName is "" by design (they do not represent a real parameter), and
// deleting with an empty name would orphan them in one stroke.
static void test_remove_parameter_ignores_empty_name()
{
    AnimatorComponent a = makeGraph();       // Jump->Idle carries an AnimationFinished condition

    auto countFinishedConditions = [&a]() {
        size_t n = 0;
        for (const auto& t : a.transitions())
            for (const auto& c : t.conditions)
                if (c.type == AnimatorComponent::ConditionType::AnimationFinished) n++;
        return n;
    };

    CHECK(countFinishedConditions() == 1u);

    a.removeParameter("");

    CHECK(countFinishedConditions() == 1u);  // still alive: the empty name does not count as a real parameter
}

// Fix #3: if removeParameter leaves a transition without conditions (it was its
// only condition), that transition must disappear from the graph, not stay
// as a link drawn on the canvas that can never fire (conditionsMet
// requires at least one condition). Standalone graph with two exits from A:
// T1 only by bool "p" (it empties and must drop), T2 by bool "p" AND trigger "q"
// (it loses the "p" one but survives with "q").
static void test_remove_parameter_drops_emptied_transitions()
{
    AnimatorComponent a;

    AnimatorComponent::State sa;
    sa.name = "A"; sa.clipName = "A"; sa.clipIndex = 0;
    AnimatorComponent::State sb;
    sb.name = "B"; sb.clipName = "B"; sb.clipIndex = 1;
    AnimatorComponent::State sc;
    sc.name = "C"; sc.clipName = "C"; sc.clipIndex = 2;

    a.addState(sa);
    a.addState(sb);
    a.addState(sc);
    a.setEntryState(0);

    a.addParameter("p", AnimatorComponent::ParamType::Bool);
    a.addParameter("q", AnimatorComponent::ParamType::Trigger);

    AnimatorComponent::Transition t1;    // A -> B, only by "p"
    t1.fromState = 0; t1.toState = 1;
    t1.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "p", true });
    a.addTransition(t1);

    AnimatorComponent::Transition t2;    // A -> C, by "p" AND "q"
    t2.fromState = 0; t2.toState = 2;
    t2.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "p", true });
    t2.conditions.push_back({ AnimatorComponent::ConditionType::Trigger, "q", true });
    a.addTransition(t2);

    CHECK(a.transitions().size() == 2u);

    a.removeParameter("p");

    // T1 is left without conditions and disappears; T2 survives with only "q".
    CHECK(a.transitions().size() == 1u);
    CHECK(a.transitions()[0].toState == 2);
    CHECK(a.transitions()[0].conditions.size() == 1u);
    CHECK(a.transitions()[0].conditions[0].type == AnimatorComponent::ConditionType::Trigger);
    CHECK(a.transitions()[0].conditions[0].paramName == "q");
}

// Fix #2: a state with duration <= 0 (unresolved clip, or one of real zero
// duration) never enters the time advance block, so without the fix it would
// never mark m_finished and an "animation finished" exit would wait
// forever (graph parked at A). Standalone graph of two states A(duration=0)->B
// by a single AnimationFinished condition.
static void test_animation_finished_fires_on_zero_duration_state()
{
    AnimatorComponent a;

    AnimatorComponent::State sa;
    sa.name = "A"; sa.clipName = "A"; sa.clipIndex = -1;   // unresolved clip
    sa.duration = 0.0f; sa.ticksPerSecond = 30.0f; sa.loop = false;

    AnimatorComponent::State sb;
    sb.name = "B"; sb.clipName = "B"; sb.clipIndex = 0;
    sb.duration = 10.0f; sb.ticksPerSecond = 10.0f;

    a.addState(sa);
    a.addState(sb);
    a.setEntryState(0);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    t.conditions.push_back({ AnimatorComponent::ConditionType::AnimationFinished, "", true });
    a.addTransition(t);

    a.reset();
    CHECK(a.currentState() == 0);
    CHECK(!a.finished());       // just entered: reset() sets it to false

    a.update(0.016f, true);     // a single update: without the fix it would stay in A

    CHECK(a.currentState() == 1);
}

// ── Cross-fade ──────────────────────────────────────────────────────────────
//
// Minimal graph of two states with a transition by bool, and the cross-fade
// duration as a parameter. The two clips have DIFFERENT tps (20 and 50) on
// purpose: the clock of the state being turned off keeps running at ITS own pace,
// so sharing a tps or freezing one of the two clocks shows.
static AnimatorComponent makeFadeGraph(float fadeSeconds)
{
    AnimatorComponent a;

    AnimatorComponent::State from;
    from.name = "From"; from.clipName = "From";
    from.clipIndex = 0; from.duration = 40.0f; from.ticksPerSecond = 20.0f; from.loop = true;

    AnimatorComponent::State to;
    to.name = "To"; to.clipName = "To";
    to.clipIndex = 1; to.duration = 100.0f; to.ticksPerSecond = 50.0f; to.loop = true;

    a.addState(from);
    a.addState(to);
    a.setEntryState(0);
    a.addParameter("go", AnimatorComponent::ParamType::Bool);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    t.duration  = fadeSeconds;
    t.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "go", true });
    a.addTransition(t);

    a.reset();
    return a;
}

// The weight rises from 0 to 1 over the duration of the transition. 0.3 and 0.7
// are checked (not 0 and 1: both extremes would be returned just the same by an
// implementation that did not interpolate anything).
static void test_crossfade_weight_ramps_over_duration()
{
    AnimatorComponent a = makeFadeGraph(0.5f);
    a.setBool("go", true);
    a.update(0.0f, true);                    // fires the transition, without advancing

    CHECK(a.currentState() == 1);
    CHECK(a.blending());
    CHECK(a.previousState() == 0);
    CHECK(a.previousClipIndex() == 0);
    // Right on entering, the destination does not weigh anything yet
    CHECK(nearlyEqual(a.blendWeight(), 0.0f));

    a.update(0.15f, true);                   // 0.15 / 0.5
    CHECK(a.blending());
    CHECK(nearlyEqual(a.blendWeight(), 0.3f));

    a.update(0.20f, true);                   // 0.35 / 0.5
    CHECK(a.blending());
    CHECK(nearlyEqual(a.blendWeight(), 0.7f));

    // Past the end: the blend closes and the origin disappears
    a.update(0.20f, true);
    CHECK(!a.blending());
    CHECK(nearlyEqual(a.blendWeight(), 1.0f));
    CHECK(a.previousState() == -1);
    CHECK(a.previousClipIndex() == 0);       // safe degradation, like currentClipIndex
}

// During the blend BOTH clocks run, each with the tps of its state, and
// the origin's respects its own loop.
static void test_crossfade_advances_both_clocks()
{
    AnimatorComponent a = makeFadeGraph(1.0f);
    a.setBool("go", true);
    a.update(0.0f, true);
    CHECK(a.currentState() == 1);
    CHECK(nearlyEqual(a.animTime(), 0.0f));
    CHECK(nearlyEqual(a.previousAnimTime(), 0.0f));

    a.update(0.5f, true);
    // Destination: 0.5 s * 50 tps = 25 ticks. Origin: 0.5 s * 20 tps = 10 ticks.
    CHECK(nearlyEqual(a.animTime(), 25.0f));
    CHECK(nearlyEqual(a.previousAnimTime(), 10.0f));
    // Different from each other: a shared clock would pass the two CHECKs above
    // only if the tps coincided, and they do not.
    CHECK(!nearlyEqual(a.animTime(), a.previousAnimTime()));
}

// The origin respects ITS loop while it fades out: From lasts 40 ticks at 20 tps = 2 s.
static void test_crossfade_prev_clock_loops()
{
    AnimatorComponent a = makeFadeGraph(3.0f);
    a.setBool("go", true);
    a.update(0.0f, true);

    a.update(2.5f, true);                    // 2.5 s * 20 tps = 50 ticks > 40
    CHECK(a.blending());
    CHECK(nearlyEqual(a.previousAnimTime(), 10.0f));   // fmod(50, 40)
}

// duration 0 (the default, and what every scene prior to this feature carries):
// hard cut, no blend and no previous state. The behavior as always.
static void test_crossfade_zero_duration_is_instant_cut()
{
    AnimatorComponent a = makeFadeGraph(0.0f);
    a.setBool("go", true);
    a.update(0.016f, true);

    CHECK(a.currentState() == 1);
    CHECK(!a.blending());
    CHECK(nearlyEqual(a.blendWeight(), 1.0f));
    CHECK(a.previousState() == -1);
}

// reset() clears the blend in progress: otherwise, re-editing the graph in the
// editor would leave the character blending against a state that no longer exists.
static void test_crossfade_reset_clears_blend()
{
    AnimatorComponent a = makeFadeGraph(0.5f);
    a.setBool("go", true);
    a.update(0.1f, true);
    CHECK(a.blending());

    a.reset();
    CHECK(!a.blending());
    CHECK(a.previousState() == -1);
    CHECK(nearlyEqual(a.blendWeight(), 1.0f));
}

// CPU replica of the blend block of bone_eval.comp: it evaluates bone i in the
// TWO clip blocks and blends component by component (lerp in position and
// scale, slerp in rotation) BEFORE composing the matrix. Blending the
// already composed matrices would give something different as soon as there is rotation.
static glm::mat4 evalLocalXformBlended(const PackedClips& p, size_t clipBaseA, size_t clipBaseB,
                                       size_t i, float TA, float TB, float w,
                                       bool lockRootMotion = false)
{
    // With no blend in progress the shader skips the entire second evaluation.
    if (w >= 1.0f) return evalLocalXform(p, clipBaseB, i, TB, lockRootMotion);
    if (w <= 0.0f) return evalLocalXform(p, clipBaseA, i, TA, lockRootMotion);

    // From row 13 the GPU sums weighted samples (nlerp in the rotation,
    // not slerp): the reference is the same as the per-sample bone_eval.
    AnimationPose pose;
    pose.count = 2;
    pose.samples[0] = { (int)clipBaseA, TA, 1.0f - w };
    pose.samples[1] = { (int)clipBaseB, TB, w };
    pose.rootMotionMode = lockRootMotion ? 1u : 0u;
    return trsToMat(evalPoseTrs(p, 1, i, pose, nullptr), p.boneInfos[clipBaseA + i]);
}

// Mesh with two clips that place the same bone in VERY distant spots (x=0 and
// x=10) and with different scales (1 and 5), so that a badly weighted blend is not
// mistaken for numeric noise.
static SkinnedMesh makeTwoClipFixture()
{
    SkinnedMesh m;
    m.skeleton.names           = { "root" };
    m.skeleton.parentIndex     = { -1 };
    m.skeleton.inverseBindPose = { glm::mat4(1.0f) };

    const float posX[2]  = { 0.0f, 10.0f };
    const float scale[2] = { 1.0f, 5.0f };
    const char* names[2] = { "From", "To" };
    const float dur[2]   = { 40.0f, 100.0f };
    const float tps[2]   = { 20.0f, 50.0f };

    for (int c = 0; c < 2; c++)
    {
        AnimationClip clip;
        clip.name = names[c]; clip.duration = dur[c]; clip.ticksPerSecond = tps[c];

        BoneChannel ch;
        ch.boneIndex = 0;
        // Two keys with the SAME value: the clip is constant over time, so the
        // test isolates the blend between clips from the sampling within a clip.
        for (int k = 0; k < 2; k++)
        {
            ch.posKeys.push_back({ (float)k * dur[c], glm::vec3(posX[c], 0.0f, 0.0f) });
            ch.rotKeys.push_back({ (float)k * dur[c], glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
            ch.scaleKeys.push_back({ (float)k * dur[c], glm::vec3(scale[c]) });
        }
        clip.channels.push_back(ch);
        m.animationClips.push_back(clip);
    }
    return m;
}

// The test that really matters: the POSE that goes out to the GPU is between the two,
// in the proportion the weight says. A cross-fade that always returned the
// origin clip (or always the destination) passes all the weight tests above and
// dies here.
static void test_crossfade_pose_interpolates_between_clips()
{
    SkinnedMesh  mesh = makeTwoClipFixture();
    PackedClips  p    = packSkinnedClips(mesh);
    const size_t B    = mesh.skeleton.names.size();

    AnimatorComponent a = makeFadeGraph(0.5f);
    a.bindClips(mesh, nullptr);
    CHECK(a.states()[0].clipIndex == 0);
    CHECK(a.states()[1].clipIndex == 1);

    a.setBool("go", true);
    a.update(0.0f, true);
    a.update(0.15f, true);                   // weight 0.3
    CHECK(nearlyEqual(a.blendWeight(), 0.3f));

    const glm::mat4 blended = evalLocalXformBlended(
        p, (size_t)a.previousClipIndex() * B, (size_t)a.currentClipIndex() * B, 0,
        a.previousAnimTime(), a.animTime(), a.blendWeight());

    // mix(0, 10, 0.3) = 3 in position; mix(1, 5, 0.3) = 2.2 in scale
    CHECK(nearlyEqual(blended[3].x, 3.0f));
    CHECK(nearlyEqual(blended[0].x, 2.2f));

    // And it is NEITHER of the two extremes
    const glm::mat4 onlyFrom = evalLocalXform(p, 0, 0, a.previousAnimTime());
    const glm::mat4 onlyTo   = evalLocalXform(p, B, 0, a.animTime());
    CHECK(!nearlyEqualMat(blended, onlyFrom));
    CHECK(!nearlyEqualMat(blended, onlyTo));
    CHECK(nearlyEqual(onlyFrom[3].x, 0.0f));
    CHECK(nearlyEqual(onlyTo[3].x, 10.0f));

    // Weight 0.7: the pose has moved TOWARD the destination, it has not stayed stuck
    a.update(0.20f, true);
    CHECK(nearlyEqual(a.blendWeight(), 0.7f));
    const glm::mat4 later = evalLocalXformBlended(
        p, (size_t)a.previousClipIndex() * B, (size_t)a.currentClipIndex() * B, 0,
        a.previousAnimTime(), a.animTime(), a.blendWeight());
    CHECK(nearlyEqual(later[3].x, 7.0f));
    CHECK(later[3].x > blended[3].x);
}

// ── Blend by float parameter ────────────────────────────────────────────────
//
// A state with two clips: its own and blendClip, blended by the value of a
// float parameter between its two thresholds (clipThreshold and the entry's). The bounds are NOT
// 0 and 1 on purpose (1.5 and 6.5): a mapping that forgot about the range would go
// unnoticed with 0..1.
static AnimatorComponent::BlendEntry entrada(const char* clip, int index, float duration, float threshold)
{
    AnimatorComponent::BlendEntry e;
    e.clipName = clip; e.clipIndex = index; e.duration = duration; e.threshold = threshold;
    return e;
}

static AnimatorComponent makeBlendStateGraph()
{
    AnimatorComponent a;

    AnimatorComponent::State s;
    s.name = "Locomotion"; s.clipName = "From";
    s.clipIndex = 0; s.duration = 40.0f; s.ticksPerSecond = 20.0f; s.loop = true;
    s.blendParam    = "speed";
    s.clipThreshold = 1.5f;
    s.blendEntries  = { entrada("To", 1, 100.0f, 6.5f) };

    a.addState(s);
    a.setEntryState(0);
    a.addParameter("speed", AnimatorComponent::ParamType::Float);
    a.reset();
    return a;
}

// The weight comes from the parameter between the two thresholds, clamped.
static void test_blend_state_weight_from_float_param()
{
    AnimatorComponent a = makeBlendStateGraph();

    // 1.5 + 0.3 * (6.5 - 1.5) = 3.0
    a.setFloat("speed", 3.0f);
    a.update(0.016f, true);
    CHECK(nearlyEqual(a.poseWeight(), 0.3f));

    // 1.5 + 0.7 * 5 = 5.0
    a.setFloat("speed", 5.0f);
    a.update(0.016f, true);
    CHECK(nearlyEqual(a.poseWeight(), 0.7f));

    // Out of range on both sides: clamp, not extrapolation. Only the clip at the
    // extreme sounds (A == B, weight 1): the same pose as weight 0 or 1 of the pair.
    a.setFloat("speed", -100.0f);
    a.update(0.016f, true);
    CHECK(a.poseClipA() == 0 && a.poseClipB() == 0);
    CHECK(nearlyEqual(a.poseWeight(), 1.0f));
    a.setFloat("speed", 999.0f);
    a.update(0.016f, true);
    CHECK(a.poseClipA() == 1 && a.poseClipB() == 1);
    CHECK(nearlyEqual(a.poseWeight(), 1.0f));

    // And within the range, the two clips that come out are the state's, not the same
    // one twice
    a.setFloat("speed", 3.0f);
    a.update(0.016f, true);
    CHECK(a.poseClipA() == 0);
    CHECK(a.poseClipB() == 1);
}

// The two clips are sampled at the SAME normalized phase: otherwise, a walk of 40
// ticks and a run of 100 fall out of sync and the legs skate.
static void test_blend_state_syncs_normalized_phase()
{
    AnimatorComponent a = makeBlendStateGraph();
    a.setFloat("speed", 4.0f);
    a.update(0.5f, true);                    // 0.5 s * 20 tps = 10 ticks

    CHECK(nearlyEqual(a.poseTimeA(), 10.0f));
    // 10/40 = 25% of clip A -> 25% of the 100 ticks of clip B
    CHECK(nearlyEqual(a.poseTimeB(), 25.0f));
    CHECK(!nearlyEqual(a.poseTimeA(), a.poseTimeB()));
}

// Normal state (no blendClip): a single clip, weight 1 and the degenerate pair.
// It is what every scene prior to this feature does.
static void test_state_without_blend_clip_stays_single()
{
    AnimatorComponent a = makeFadeGraph(0.0f);
    a.update(0.016f, true);

    CHECK(nearlyEqual(a.poseWeight(), 1.0f));
    CHECK(a.poseClipA() == a.poseClipB());
    CHECK(nearlyEqual(a.poseTimeA(), a.poseTimeB()));
}

// A cross-fade in flight rules over the state's blend: there are only two clips in
// the push constant, so while the transition lasts the pair IS the transition.
static void test_crossfade_takes_priority_over_blend_state()
{
    AnimatorComponent a;

    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "From";
    idle.clipIndex = 0; idle.duration = 40.0f; idle.ticksPerSecond = 20.0f; idle.loop = true;

    AnimatorComponent::State loco;
    loco.name = "Locomotion"; loco.clipName = "From";
    loco.clipIndex = 0; loco.duration = 40.0f; loco.ticksPerSecond = 20.0f; loco.loop = true;
    loco.blendParam = "speed"; loco.clipThreshold = 0.0f;
    loco.blendEntries = { entrada("To", 1, 100.0f, 10.0f) };

    a.addState(idle);
    a.addState(loco);
    a.setEntryState(0);
    a.addParameter("speed", AnimatorComponent::ParamType::Float);
    a.addParameter("go",    AnimatorComponent::ParamType::Bool);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 0.5f;
    t.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "go", true });
    a.addTransition(t);
    a.reset();

    a.setFloat("speed", 8.0f);               // would ask for weight 0.8 on the destination
    a.setBool("go", true);
    a.update(0.0f, true);
    a.update(0.15f, true);                   // 0.3 de cross-fade

    // During the blend the pair is the transition, not the state's blend
    CHECK(a.blending());
    CHECK(nearlyEqual(a.poseWeight(), 0.3f));
    CHECK(a.poseClipA() == 0);               // clip of the state being turned off
    CHECK(a.poseClipB() == 0);               // PRIMARY clip of the destination

    // Once the transition is over, the state's blend rules again
    a.update(0.5f, true);
    CHECK(!a.blending());
    CHECK(nearlyEqual(a.poseWeight(), 0.8f));
    CHECK(a.poseClipB() == 1);
}

// bindClips ALSO resolves the second clip by name and caches its duration.
// A blendClip that does not exist leaves the index at -1, warns, and the state behaves
// like a normal one instead of blending against garbage.
static void test_bind_clips_resolves_blend_clip()
{
    SkinnedMesh mesh = makeTwoClipFixture();

    AnimatorComponent a;
    AnimatorComponent::State ok;
    ok.name = "Ok"; ok.clipName = "From"; ok.blendEntries = { entrada("To", -1, 0.0f, 1.0f) };
    AnimatorComponent::State bad;
    bad.name = "Bad"; bad.clipName = "From"; bad.blendEntries = { entrada("NoExiste", -1, 0.0f, 1.0f) };
    a.addState(ok);
    a.addState(bad);

    std::vector<std::string> warnings;
    a.bindClips(mesh, &warnings);

    CHECK(a.states()[0].blendEntries[0].clipIndex == 1);
    CHECK(nearlyEqual(a.states()[0].blendEntries[0].duration, 100.0f));
    CHECK(a.states()[1].blendEntries[0].clipIndex == -1);
    CHECK(warnings.size() == 1u);

    // The state with the broken blendClip does not blend: weight 1, a single clip
    a.setEntryState(1);
    a.update(0.016f, true);
    CHECK(nearlyEqual(a.poseWeight(), 1.0f));
    CHECK(a.poseClipA() == a.poseClipB());
}

// The real one: the POSE that comes out is between the two clips in the proportion of
// the parameter. A blend that always returned the primary clip passes the
// weight tests and dies here.
static void test_blend_state_pose_interpolates_between_clips()
{
    SkinnedMesh  mesh = makeTwoClipFixture();
    PackedClips  p    = packSkinnedClips(mesh);
    const size_t B    = mesh.skeleton.names.size();

    AnimatorComponent a = makeBlendStateGraph();
    a.bindClips(mesh, nullptr);
    CHECK(a.states()[0].clipIndex == 0);
    CHECK(a.states()[0].blendEntries[0].clipIndex == 1);

    a.setFloat("speed", 3.0f);               // weight 0.3
    a.update(0.016f, true);
    CHECK(nearlyEqual(a.poseWeight(), 0.3f));

    const glm::mat4 blended = evalLocalXformBlended(
        p, (size_t)a.poseClipA() * B, (size_t)a.poseClipB() * B, 0,
        a.poseTimeA(), a.poseTimeB(), a.poseWeight());

    // mix(0, 10, 0.3) = 3 in position; mix(1, 5, 0.3) = 2.2 in scale
    CHECK(nearlyEqual(blended[3].x, 3.0f));
    CHECK(nearlyEqual(blended[0].x, 2.2f));

    const glm::mat4 onlyPrimary = evalLocalXform(p, 0, 0, a.poseTimeA());
    const glm::mat4 onlyBlend   = evalLocalXform(p, B, 0, a.poseTimeB());
    CHECK(!nearlyEqualMat(blended, onlyPrimary));
    CHECK(!nearlyEqualMat(blended, onlyBlend));

    // Raising the parameter moves the pose TOWARD the second clip
    a.setFloat("speed", 5.0f);               // weight 0.7
    a.update(0.016f, true);
    const glm::mat4 later = evalLocalXformBlended(
        p, (size_t)a.poseClipA() * B, (size_t)a.poseClipB() * B, 0,
        a.poseTimeA(), a.poseTimeB(), a.poseWeight());
    CHECK(nearlyEqual(later[3].x, 7.0f));
    CHECK(later[3].x > blended[3].x);
}

// Repro of the reported bug: entry state with loop=false that on finishing
// moves to the next by "animation finished", with cross-fade. The destination
// must END UP playing: its clip at the output and its clock running.
static void test_finished_transition_reaches_destination()
{
    SkinnedMesh mesh = makeTwoClipFixture();   // clips "From" (40t@20) y "To" (100t@50)

    AnimatorComponent a;
    AnimatorComponent::State intro;
    intro.name = "Intro"; intro.clipName = "From"; intro.loop = false;
    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "To"; idle.loop = true;
    a.addState(intro);
    a.addState(idle);
    a.setEntryState(0);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 0.3f;
    t.conditions.push_back({ AnimatorComponent::ConditionType::AnimationFinished, "", true });
    a.addTransition(t);

    a.bindClips(mesh, nullptr);
    CHECK(a.states()[0].clipIndex == 0);
    CHECK(a.states()[1].clipIndex == 1);

    // Intro lasts 40 ticks at 20 tps = 2 s = 120 frames at 60 fps; the cross-fade
    // is 0.3 s = 18 more frames. 180 leaves plenty of margin for both.
    for (int f = 0; f < 180; f++) a.update(1.0f / 60.0f, true);

    CHECK(a.currentState() == 1);
    // With the cross-fade over (0.3 s) the destination rules completely
    CHECK(!a.blending());
    CHECK(a.poseClipA() == 1);
    CHECK(a.poseClipB() == 1);
    CHECK(nearlyEqual(a.poseWeight(), 1.0f));
    // And its clock RUNS: the destination animation plays, it does not stay stuck
    const float t0 = a.poseTimeB();
    a.update(0.1f, true);
    CHECK(a.poseTimeB() > t0);
}

// In Edit Mode the clock runs all the same (only transitions are skipped), so
// an entry state with loop=false reaches its end and leaves m_finished true
// forever. If nobody resets on entering Play, the "animation
// finished" transition fires on the FIRST frame and the entry animation is not seen.
//
// This test pins the contract needed above: entering Play must
// call reset() (EditorUI::drawToolbar does it in the Play button).
static void test_edit_mode_finished_leaks_into_play_without_reset()
{
    SkinnedMesh mesh = makeTwoClipFixture();

    AnimatorComponent a;
    AnimatorComponent::State intro;
    intro.name = "Intro"; intro.clipName = "From"; intro.loop = false;
    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "To"; idle.loop = true;
    a.addState(intro);
    a.addState(idle);
    a.setEntryState(0);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    t.conditions.push_back({ AnimatorComponent::ConditionType::AnimationFinished, "", true });
    a.addTransition(t);
    a.bindClips(mesh, nullptr);

    // 5 s in Edit Mode: Intro (2 s) finished a while ago
    for (int f = 0; f < 300; f++) a.update(1.0f / 60.0f, /*evaluateTransitions=*/false);
    CHECK(a.currentState() == 0);
    CHECK(a.finished());                       // the leak

    // Play WITHOUT reset: the transition fires on the first frame, Intro is not seen
    AnimatorComponent leaked = a;
    leaked.update(1.0f / 60.0f, true);
    CHECK(leaked.currentState() == 1);

    // Play WITH reset (what the editor does): Intro plays in full
    a.reset();
    CHECK(!a.finished());
    CHECK(nearlyEqual(a.animTime(), 0.0f));
    a.update(1.0f / 60.0f, true);
    CHECK(a.currentState() == 0);              // still in Intro
    for (int f = 0; f < 130; f++) a.update(1.0f / 60.0f, true);
    CHECK(a.currentState() == 1);              // and transitions when it is due
}

// bindClips resolves the clip by NAME and caches duration/ticksPerSecond. A
// name that does not exist leaves clipIndex at -1 and warns: it fails loudly, not silently.
// loop is NOT touched: it belongs to the user, not to the FBX.
static void test_bind_clips_resolves_by_name()
{
    SkinnedMesh mesh;
    mesh.skeleton.names           = { "root" };
    mesh.skeleton.parentIndex     = { -1 };
    mesh.skeleton.inverseBindPose = { glm::mat4(1.0f) };

    AnimationClip idle;  idle.name = "Idle"; idle.duration = 30.0f; idle.ticksPerSecond = 30.0f;
    AnimationClip run;   run.name  = "Run";  run.duration  = 20.0f; run.ticksPerSecond  = 20.0f;
    mesh.animationClips = { idle, run };

    AnimatorComponent a;
    AnimatorComponent::State s0; s0.name = "A"; s0.clipName = "Run";     s0.loop = false;
    AnimatorComponent::State s1; s1.name = "B"; s1.clipName = "NoExiste"; s1.loop = true;
    a.addState(s0);
    a.addState(s1);

    std::vector<std::string> warnings;
    a.bindClips(mesh, &warnings);

    // Resolved by name, not by declaration order in the graph
    CHECK(a.states()[0].clipIndex == 1);
    CHECK(nearlyEqual(a.states()[0].duration, 20.0f));
    CHECK(nearlyEqual(a.states()[0].ticksPerSecond, 20.0f));
    // loop was set by the user: bindClips does not overwrite it
    CHECK(a.states()[0].loop == false);

    // Nonexistent clip: -1 and a warning
    CHECK(a.states()[1].clipIndex == -1);
    CHECK(warnings.size() == 1u);

    // clipIndex -1 cannot blow up the Renderer: currentClipIndex falls back to 0
    a.setEntryState(1);
    CHECK(a.currentClipIndex() == 0);
}

// The slot follows the CameraComponent pattern, but without a uniqueness invariant:
// each skinned GameObject can have its own.
static void test_gameobject_animator_slot()
{
    GameObject go("personaje");
    CHECK(!go.hasAnimator());
    CHECK(go.getAnimator() == nullptr);

    go.setAnimator(std::make_shared<AnimatorComponent>());
    CHECK(go.hasAnimator());

    go.setAnimator(nullptr);
    CHECK(!go.hasAnimator());
}

// Criterion 3: the entire graph (nodes, positions, links, conditions,
// parameters, per-node loop and entry state) survives save -> load.
static void test_graph_survives_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    // nodeFromJson reuses the JSON "id" when it exists, so the same id
    // resolves the node in the loaded scene. Scene has no findByName.
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();

    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "ClipIdle";
    idle.loop = true;  idle.editorPos = glm::vec2(10.0f, 20.0f);
    AnimatorComponent::State jump;
    jump.name = "Jump"; jump.clipName = "ClipJump";
    jump.loop = false; jump.editorPos = glm::vec2(300.0f, 40.0f);
    a->addState(idle);
    a->addState(jump);
    a->setEntryState(1);                    // entry state NOT the default, on purpose

    a->addParameter("running", AnimatorComponent::ParamType::Bool);
    a->addParameter("jump",    AnimatorComponent::ParamType::Trigger);

    AnimatorComponent::Transition t0;
    t0.fromState = 0; t0.toState = 1;
    t0.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "running", false });
    t0.conditions.push_back({ AnimatorComponent::ConditionType::Trigger, "jump", true });
    a->addTransition(t0);

    AnimatorComponent::Transition t1;
    t1.fromState = 1; t1.toState = 0;
    t1.conditions.push_back({ AnimatorComponent::ConditionType::AnimationFinished, "", true });
    a->addTransition(t1);

    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->name == "Personaje");
    CHECK(found->hasAnimator());
    if (!found->hasAnimator()) return;
    const AnimatorComponent& r = *found->getAnimator();

    // States: name, clip, loop, node position
    CHECK(r.states().size() == 2u);
    CHECK(r.states()[0].name == "Idle");
    CHECK(r.states()[0].clipName == "ClipIdle");
    CHECK(r.states()[0].loop == true);
    CHECK(nearlyEqual(r.states()[0].editorPos.x, 10.0f));
    CHECK(nearlyEqual(r.states()[0].editorPos.y, 20.0f));
    CHECK(r.states()[1].name == "Jump");
    CHECK(r.states()[1].clipName == "ClipJump");
    CHECK(r.states()[1].loop == false);
    CHECK(nearlyEqual(r.states()[1].editorPos.x, 300.0f));

    // Entry state
    CHECK(r.entryState() == 1);

    // Parameters with their type
    CHECK(r.parameters().size() == 2u);
    CHECK(r.parameters()[0].name == "running");
    CHECK(r.parameters()[0].type == AnimatorComponent::ParamType::Bool);
    CHECK(r.parameters()[1].name == "jump");
    CHECK(r.parameters()[1].type == AnimatorComponent::ParamType::Trigger);

    // Links and conditions (including expected == false, which a default of true
    // would swallow without anything failing)
    CHECK(r.transitions().size() == 2u);
    CHECK(r.transitions()[0].fromState == 0);
    CHECK(r.transitions()[0].toState == 1);
    CHECK(r.transitions()[0].conditions.size() == 2u);
    CHECK(r.transitions()[0].conditions[0].type == AnimatorComponent::ConditionType::Bool);
    CHECK(r.transitions()[0].conditions[0].paramName == "running");
    CHECK(r.transitions()[0].conditions[0].expected == false);
    CHECK(r.transitions()[0].conditions[1].type == AnimatorComponent::ConditionType::Trigger);
    CHECK(r.transitions()[1].conditions[0].type == AnimatorComponent::ConditionType::AnimationFinished);
}

// Additive block: a scene saved before "animator" existed loads
// the same, with no animator and no warnings.
static void test_scene_without_animator_block_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    const uint64_t id = scene.addGameObject("Vacio")->id;
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(!found->hasAnimator());
}

// Add/Remove of the component go through the undo stack. The command keeps a
// COPY of the graph so that an Add-undo-redo does not return an empty component.
static void test_animator_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    AnimatorComponent st;
    AnimatorComponent::State s; s.name = "Idle"; s.clipName = "ClipIdle"; s.loop = false;
    st.addState(s);
    st.addParameter("running", AnimatorComponent::ParamType::Bool);

    AnimatorComponentCommand cmd(scene, "Añadir Animator", id, /*add=*/true, st);

    cmd.execute();
    CHECK(go->hasAnimator());
    CHECK(go->getAnimator()->states().size() == 1u);

    cmd.undo();
    CHECK(!go->hasAnimator());

    // Redo: the graph comes back whole, not an empty component
    cmd.execute();
    CHECK(go->hasAnimator());
    CHECK(go->getAnimator()->states().size() == 1u);
    CHECK(go->getAnimator()->states()[0].name == "Idle");
    CHECK(go->getAnimator()->states()[0].loop == false);
    CHECK(go->getAnimator()->parameters().size() == 1u);
}

// The Remove is the same command with add=false: execute removes, undo restores.
static void test_animator_command_remove()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    AnimatorComponent st;
    AnimatorComponent::State s; s.name = "Idle"; s.clipName = "ClipIdle";
    st.addState(s);
    go->setAnimator(std::make_shared<AnimatorComponent>(st));

    AnimatorComponentCommand cmd(scene, "Quitar Animator", id, /*add=*/false, st);
    cmd.execute();
    CHECK(!go->hasAnimator());
    cmd.undo();
    CHECK(go->hasAnimator());
    CHECK(go->getAnimator()->states().size() == 1u);
}

// The object may have disappeared between execute and undo: it is resolved by
// id in each one, never by raw pointer, so it must not crash.
static void test_animator_command_survives_missing_target()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    AnimatorComponentCommand cmd(scene, "Añadir Animator", id, /*add=*/true, AnimatorComponent{});
    scene.removeGameObject(go);
    cmd.execute();   // findById returns nullptr and it returns without touching anything
    cmd.undo();
}

// Adding a source goes through the stack: without this, a Ctrl+Z after
// mistakenly importing a 60-clip FBX would have no way back.
static void test_animation_source_command_add_undo_redo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    const size_t before = mesh->animationClips.size();
    go->setMesh(mesh); mesh = soloObservador(mesh);

    AnimationSourceCommand cmd(scene, /*renderer=*/nullptr, "Añadir animaciones",
                                id, /*add=*/true, "assets/modelAnimation.fbx",
                                /*clipNames=*/{});

    cmd.execute();
    CHECK(mesh->animationSources.size() == 2u);
    CHECK(mesh->animationClips.size() == before * 2);

    cmd.undo();
    CHECK(mesh->animationSources.size() == 1u);
    CHECK(mesh->animationClips.size() == before);

    // Redo: the same clips come back with the same names as the first time
    cmd.execute();
    CHECK(mesh->animationSources.size() == 2u);
    CHECK(mesh->animationClips.size() == before * 2);
}

// The Remove is the same command with add=false, and its undo has to return the
// clips with the EXACT names they had (that is why the command keeps
// clipNames): if they came back with the file's name, the graph's states
// would be left orphaned after a Ctrl+Z.
static void test_animation_source_command_remove_restores_names()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh); mesh = soloObservador(mesh);

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    const std::string imported = mesh->animationSources[1].clipNames[0];
    CHECK(renameClip(*mesh, imported, "MiSalto"));
    const std::vector<std::string> names = mesh->animationSources[1].clipNames;

    AnimationSourceCommand cmd(scene, nullptr, "Quitar animaciones",
                                id, /*add=*/false, "assets/modelAnimation.fbx", names);

    cmd.execute();
    CHECK(mesh->animationSources.size() == 1u);

    cmd.undo();
    CHECK(mesh->animationSources.size() == 2u);
    CHECK(mesh->animationSources[1].clipNames == names);
    bool encontrado = false;
    for (const auto& c : mesh->animationClips)
        if (c.name == "MiSalto") encontrado = true;
    CHECK(encontrado);
}

// Finding 1 of the final review: bindClips is the only thing that resolves
// clipName -> clipIndex, and its only caller in the engine is
// Scene::nodeFromJson. Unless AnimationSourceCommand re-resolves after
// mutating animationClips, a state survives with the OLD index, which after
// removing a source in the middle can end up pointing at a clip DIFFERENT from
// the one its name says, without a warning, and the Renderer plays it anyway
// (currentClipIndex does not distinguish "resolved" from "in range by chance").
// This test builds three states (one per clip: builtin + two imported),
// removes the imported source in the MIDDLE, and checks the real invariant: each
// state that stays resolved points at a clip whose name matches its
// own, or is at -1 (orphan). No index "slips through" pointing at another
// clip. It is repeated after the undo (which reinserts at the end, see applyAdd)
// to also cover the Add path.
static void test_animation_source_command_remove_rebinds_clip_indices()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh); mesh = soloObservador(mesh);

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    CHECK(mesh->animationSources.size() == 3u);   // builtin + 2 imported

    // One state per existing clip, same name as the clip so the invariant
    // can be checked by name equality.
    auto anim = std::make_shared<AnimatorComponent>();
    for (const auto& c : mesh->animationClips)
    {
        AnimatorComponent::State st;
        st.name = c.name;
        st.clipName = c.name;
        anim->addState(st);
    }
    go->setAnimator(anim);
    anim->bindClips(*mesh);   // resolves as Scene::nodeFromJson would

    auto checkInvariant = [&]()
    {
        for (const auto& st : anim->states())
        {
            if (st.clipIndex < 0) continue;   // orphan: acceptable
            CHECK((size_t)st.clipIndex < mesh->animationClips.size());
            if ((size_t)st.clipIndex < mesh->animationClips.size())
                CHECK(mesh->animationClips[(size_t)st.clipIndex].name == st.clipName);
        }
    };
    checkInvariant();   // premise: true before touching anything

    // Removes the imported source in the middle (index 1): it shifts the clips of
    // source 2 one slot back in the flat list. pathOccurrence=1
    // because there is a source with the same path ahead of it (row 2, see
    // the comment of applyRemove in Command.cpp about counting from the end).
    const AnimationSource middle = mesh->animationSources[1];
    const std::string middleClipName = middle.clipNames[0];
    AnimationSourceCommand cmd(scene, /*renderer=*/nullptr, "Quitar animaciones",
                                id, /*add=*/false, middle.path, middle.clipNames,
                                /*pathOccurrence=*/1);
    cmd.execute();

    CHECK(mesh->animationSources.size() == 2u);
    checkInvariant();   // Finding 1 invariant after the Remove

    // And in particular: the state that named the removed clip was left orphaned,
    // not repointed at another clip by index coincidence.
    for (const auto& st : anim->states())
        if (st.clipName == middleClipName) CHECK(st.clipIndex == -1);

    // Mirror of the Add path: undo() of a Remove is an applyAdd (it reinserts
    // at the end), and it also has to re-resolve.
    cmd.undo();
    CHECK(mesh->animationSources.size() == 3u);
    checkInvariant();
    for (const auto& st : anim->states())
        if (st.clipName == middleClipName) CHECK(st.clipIndex != -1);   // no longer orphaned
}

// Finding 2 of the final review: importing the same FBX twice is legal
// (AnimatorPanel tells them apart with pathOccurrence in the panel), so two
// rows can share a path. The original command located by path alone,
// scanning backwards, so removing the FIRST of the two rows always deleted
// the LAST. This test targets exactly that case: it asks to remove
// row 1 (pathOccurrence=1, row 2 stays ahead) and checks that
// the clip of row 1 disappears, not that of row 2.
static void test_animation_source_command_remove_targets_clicked_occurrence()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh); mesh = soloObservador(mesh);

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    CHECK(mesh->animationSources.size() == 3u);
    CHECK(mesh->animationSources[1].path == mesh->animationSources[2].path);   // same path, different rows

    const std::string firstImportedName  = mesh->animationSources[1].clipNames[0];
    const std::string secondImportedName = mesh->animationSources[2].clipNames[0];
    CHECK(firstImportedName != secondImportedName);   // uniqueClipName already guarantees it

    AnimationSourceCommand cmd(scene, /*renderer=*/nullptr, "Quitar animaciones",
                                id, /*add=*/false,
                                mesh->animationSources[1].path,
                                mesh->animationSources[1].clipNames,
                                /*pathOccurrence=*/1);
    cmd.execute();

    CHECK(mesh->animationSources.size() == 2u);
    bool firstGone = true;
    bool secondSurvives = false;
    for (const auto& c : mesh->animationClips)
    {
        if (c.name == firstImportedName)  firstGone = false;
        if (c.name == secondImportedName) secondSurvives = true;
    }
    CHECK(firstGone);
    CHECK(secondSurvives);
}

// Finding 3 of the final review: if the scan finds nothing to remove
// (an occurrence that no longer exists: redo after a reload, or mesh replaced between
// execute() and undo()), applyRemove must not mutate the mesh. There is no real
// renderer in a headless test to check that the rebuild is saved, but it IS
// possible to check that the mesh stays intact. Before, the early return did not
// exist and the code further down (the rebuild) ran anyway on a mesh
// with no changes.
static void test_animation_source_command_remove_noop_when_occurrence_missing()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh); mesh = soloObservador(mesh);

    std::vector<std::string> warnings;
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    const size_t sourcesBefore = mesh->animationSources.size();
    const size_t clipsBefore   = mesh->animationClips.size();

    // pathOccurrence=5: there are not remotely five sources with that path.
    AnimationSourceCommand cmd(scene, /*renderer=*/nullptr, "Quitar animaciones",
                                id, /*add=*/false,
                                "assets/modelAnimation.fbx",
                                std::vector<std::string>{"loQueSea"},
                                /*pathOccurrence=*/5);
    cmd.execute();

    CHECK(mesh->animationSources.size() == sourcesBefore);
    CHECK(mesh->animationClips.size() == clipsBefore);
}

// Finding of the final review (blocking): m_pathOccurrence locates "the Nth
// source with this path, counting from the end", and that is stable for
// ONE isolated command, but not for INTERLEAVED commands: applyAdd
// always reinserts at the end, so each undo/redo of an unrelated Add
// shifts the "end" that the pathOccurrence of another pending command
// takes for granted. Exact repro from the review:
//   1. Mesh already has [builtin B, S1 (same FBX)].
//   2. The same FBX is reimported -> cmdA (Add), sources = [B, S1, S2].
//   3. Row 1 (S1) is removed -> cmdB (Remove, pathOccurrence=1 because S2
//      stays ahead). sources = [B, S2].
//   4. Ctrl+Z of cmdB -> applyAdd reinserts S1 AT THE END: [B, S2, S1'].
//   5. Ctrl+Z of cmdA -> applyRemove with pathOccurrence=0 (the one
//      recomputed in its own execute) finds the FIRST non-builtin source
//      scanning from the end: that is now S1', not S2 -- it deletes what the
//      user had just recovered and leaves alive what cmdA had put in.
// The fix resolves by identity (m_clipNames, unique in the mesh thanks to
// uniqueClipName) instead of by position, so this test checks that
// after undoing cmdB and then cmdA, the source that survives is S1 (the one
// cmdA did NOT touch), not S2.
static void test_animation_source_command_undo_add_after_interleaved_remove_undo()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh); mesh = soloObservador(mesh);

    // Starting state of the repro: [B, S1]. S1 is imported outside the undo
    // stack (it represents "what was already there before this
    // scenario started").
    std::vector<std::string> warnings;
    CHECK(addAnimationSource(*mesh, "assets/modelAnimation.fbx", warnings));
    CHECK(mesh->animationSources.size() == 2u);
    const std::string s1Path = mesh->animationSources[1].path;
    const std::vector<std::string> s1Names = mesh->animationSources[1].clipNames;

    // cmdA: reimport the same FBX, just as AnimatorPanel::onAnimationFbxSelected
    // builds the command (empty clipNames, default pathOccurrence 0).
    auto cmdA = std::make_unique<AnimationSourceCommand>(
        scene, /*renderer=*/nullptr, "Añadir animaciones", id,
        /*add=*/true, s1Path, std::vector<std::string>{});
    cmdA->execute();
    CHECK(mesh->animationSources.size() == 3u);   // [B, S1, S2]
    const std::vector<std::string> s2Names = mesh->animationSources[2].clipNames;
    CHECK(s1Names != s2Names);   // uniqueClipName tells them apart

    // cmdB: click on the X of row 1 (S1), with pathOccurrence computed
    // exactly as AnimatorPanel::drawAnimationSources does (it counts the
    // non-builtin sources with the same path AHEAD of the clicked row:
    // S2 stays ahead, so it is 1).
    auto cmdB = std::make_unique<AnimationSourceCommand>(
        scene, /*renderer=*/nullptr, "Quitar animaciones", id,
        /*add=*/false, s1Path, s1Names, /*pathOccurrence=*/1);
    cmdB->execute();
    CHECK(mesh->animationSources.size() == 2u);   // [B, S2]

    // LIFO of the real UndoManager: the last one pushed is undone first
    // (cmdB), then cmdA.
    cmdB->undo();   // applyAdd: reinserts S1' AT THE END -> [B, S2, S1']
    CHECK(mesh->animationSources.size() == 3u);

    cmdA->undo();   // applyRemove: must remove S2 (what cmdA inserted), not S1'

    CHECK(mesh->animationSources.size() == 2u);
    CHECK(!mesh->animationSources[1].builtin);
    // The key assertion: the source that survives is S1, not S2. With the bug
    // (location by position) this fails because applyRemove deletes S1'
    // (what the user had just recovered) and leaves S2 alive.
    CHECK(mesh->animationSources[1].clipNames == s1Names);
}

// The rename is undoable and drags the graph's states along in both directions.
static void test_clip_rename_command()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto mesh = std::make_shared<SkinnedMesh>();
    AnimationClip c; c.name = "walk"; mesh->animationClips.push_back(c);
    AnimationSource src; src.path = "x.fbx"; src.builtin = true; src.clipNames = { "walk" };
    mesh->animationSources.push_back(src);
    go->setMesh(mesh); mesh = soloObservador(mesh);

    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State st; st.name = "Andar"; st.clipName = "walk";
    a->addState(st);
    go->setAnimator(a);

    ClipRenameCommand cmd(scene, "Renombrar clip", id, "walk", "andar");

    cmd.execute();
    CHECK(mesh->animationClips[0].name == "andar");
    CHECK(a->states()[0].clipName == "andar");

    cmd.undo();
    CHECK(mesh->animationClips[0].name == "walk");
    CHECK(a->states()[0].clipName == "walk");
}

// Like the rest of the commands: the object may have disappeared between
// execute and undo. It is resolved by id each time, never by raw pointer.
static void test_animation_source_command_survives_missing_target()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    AnimationSourceCommand cmd(scene, nullptr, "Añadir animaciones",
                                id, true, "assets/modelAnimation.fbx", {});
    scene.removeGameObject(go);
    cmd.execute();   // findById returns nullptr and it returns without touching anything
    cmd.undo();
}

// The API consumed by Lua: parameters are queried by name and only if
// they are declared in the graph. An undeclared name is ignored instead of
// creating a phantom parameter that no condition would look at.
static void test_parameter_api_ignores_undeclared()
{
    AnimatorComponent a = makeGraph();

    a.setBool("running", true);
    CHECK(a.getBool("running"));

    // Undeclared: it is neither stored nor blows up
    a.setBool("noExiste", true);
    CHECK(!a.getBool("noExiste"));

    // Wrong type: "jump" is a trigger, not a bool
    a.setBool("jump", true);
    CHECK(!a.getBool("jump"));

    CHECK(a.currentStateName() == "Idle");
    a.update(0.016f, true);
    CHECK(a.currentStateName() == "Run");
}

// Criterion: an Int parameter fires a transition according to its comparator. A
// minimal graph is built separately from makeGraph() so as not to touch the
// existing tests, which depend on its exact shape.
static AnimatorComponent makeNumericGraph()
{
    AnimatorComponent a;

    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "Idle";
    idle.clipIndex = 0; idle.duration = 30.0f; idle.ticksPerSecond = 30.0f; idle.loop = true;

    AnimatorComponent::State run;
    run.name = "Run"; run.clipName = "Run";
    run.clipIndex = 1; run.duration = 20.0f; run.ticksPerSecond = 20.0f; run.loop = true;

    a.addState(idle);
    a.addState(run);
    a.setEntryState(0);

    a.addParameter("combo", AnimatorComponent::ParamType::Int);
    a.addParameter("speed", AnimatorComponent::ParamType::Float);

    a.reset();
    return a;
}

static void test_int_condition_greater_and_equals()
{
    AnimatorComponent a = makeNumericGraph();

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition c;
    c.type      = AnimatorComponent::ConditionType::Int;
    c.paramName = "combo";
    c.compare   = AnimatorComponent::Compare::Greater;
    c.threshold = 2.0f;
    t.conditions.push_back(c);
    a.addTransition(t);

    // Initial value 0: does not fire
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);

    // Equal to the threshold: Greater is strict, it still does not fire
    a.setInt("combo", 2);
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);

    a.setInt("combo", 3);
    a.update(0.016f, true);
    CHECK(a.currentState() == 1);

    // Equals: same graph, another comparator
    AnimatorComponent b = makeNumericGraph();
    AnimatorComponent::Transition tb;
    tb.fromState = 0; tb.toState = 1;
    AnimatorComponent::Condition cb;
    cb.type      = AnimatorComponent::ConditionType::Int;
    cb.paramName = "combo";
    cb.compare   = AnimatorComponent::Compare::Equals;
    cb.threshold = 3.0f;
    tb.conditions.push_back(cb);
    b.addTransition(tb);

    b.setInt("combo", 4);
    b.update(0.016f, true);
    CHECK(b.currentState() == 0);

    b.setInt("combo", 3);
    b.update(0.016f, true);
    CHECK(b.currentState() == 1);
}

static void test_float_condition_less()
{
    AnimatorComponent a = makeNumericGraph();

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition c;
    c.type      = AnimatorComponent::ConditionType::Float;
    c.paramName = "speed";
    c.compare   = AnimatorComponent::Compare::Less;
    c.threshold = -1.0f;
    t.conditions.push_back(c);
    a.addTransition(t);

    // Initial value 0.0f: it is not less than -1.0f
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);

    a.setFloat("speed", -2.5f);
    CHECK(nearlyEqual(a.getFloat("speed"), -2.5f));
    a.update(0.016f, true);
    CHECK(a.currentState() == 1);
}

// Equals/NotEquals on a Float: they are offered in the UI just like on Int, and the
// evaluator applies them with a bare ==, without epsilon. 2.5f is used, exactly
// representable in binary, so that the test is about the comparator and not about
// floating point precision.
static void test_float_condition_equals_and_not_equals()
{
    AnimatorComponent a = makeNumericGraph();

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition c;
    c.type      = AnimatorComponent::ConditionType::Float;
    c.paramName = "speed";
    c.compare   = AnimatorComponent::Compare::Equals;
    c.threshold = 2.5f;
    t.conditions.push_back(c);
    a.addTransition(t);

    // Different from the threshold: Equals does not fire
    a.setFloat("speed", 1.0f);
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);

    // Exact match: Equals does fire
    a.setFloat("speed", 2.5f);
    a.update(0.016f, true);
    CHECK(a.currentState() == 1);

    // NotEquals: same graph, opposite comparator
    AnimatorComponent b = makeNumericGraph();
    AnimatorComponent::Transition tb;
    tb.fromState = 0; tb.toState = 1;
    AnimatorComponent::Condition cb;
    cb.type      = AnimatorComponent::ConditionType::Float;
    cb.paramName = "speed";
    cb.compare   = AnimatorComponent::Compare::NotEquals;
    cb.threshold = 2.5f;
    tb.conditions.push_back(cb);
    b.addTransition(tb);

    // Equal to the threshold: NotEquals does not fire
    b.setFloat("speed", 2.5f);
    b.update(0.016f, true);
    CHECK(b.currentState() == 0);

    // Different: NotEquals does fire
    b.setFloat("speed", 1.0f);
    b.update(0.016f, true);
    CHECK(b.currentState() == 1);
}

// Same guard as setBool: an undeclared name or one of another type is ignored
// instead of creating a phantom parameter that no condition would look at.
static void test_numeric_api_type_guards()
{
    AnimatorComponent a = makeNumericGraph();
    a.addParameter("running", AnimatorComponent::ParamType::Bool);

    a.setInt("combo", 7);
    CHECK(a.getInt("combo") == 7);
    a.setFloat("speed", 1.5f);
    CHECK(nearlyEqual(a.getFloat("speed"), 1.5f));

    // Undeclared
    a.setInt("noExiste", 5);
    CHECK(a.getInt("noExiste") == 0);
    a.setFloat("tampoco", 5.0f);
    CHECK(nearlyEqual(a.getFloat("tampoco"), 0.0f));

    // Wrong type in both directions
    a.setInt("speed", 9);            // speed es float
    CHECK(a.getInt("speed") == 0);
    a.setFloat("combo", 9.0f);       // combo es int
    CHECK(nearlyEqual(a.getFloat("combo"), 0.0f));
    a.setInt("running", 1);          // running es bool
    CHECK(a.getInt("running") == 0);

    // reset returns the numeric ones to zero, just like the bools
    a.reset();
    CHECK(a.getInt("combo") == 0);
    CHECK(nearlyEqual(a.getFloat("speed"), 0.0f));
}

// removeParameter already cleaned bools and triggers; the numeric ones go through the
// same path, including pruning transitions that are left without conditions.
static void test_remove_numeric_parameter_cleans_conditions()
{
    AnimatorComponent a = makeNumericGraph();

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition c;
    c.type      = AnimatorComponent::ConditionType::Int;
    c.paramName = "combo";
    c.compare   = AnimatorComponent::Compare::Greater;
    c.threshold = 0.0f;
    t.conditions.push_back(c);
    a.addTransition(t);
    CHECK(a.transitions().size() == 1);

    a.removeParameter("combo");
    CHECK(a.parameters().size() == 1);          // only "speed" is left
    CHECK(a.transitions().empty());             // it was left without conditions
    CHECK(a.getInt("combo") == 0);
}

// Full round-trip of a graph with numeric parameters and conditions: if
// compare or threshold did not survive, the loaded transition would fire when
// it must not (or never).
static void test_numeric_graph_survives_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();

    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "ClipIdle"; idle.loop = true;
    AnimatorComponent::State run;
    run.name = "Run"; run.clipName = "ClipRun"; run.loop = true;
    a->addState(idle);
    a->addState(run);

    a->addParameter("combo", AnimatorComponent::ParamType::Int);
    a->addParameter("speed", AnimatorComponent::ParamType::Float);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition ci;
    ci.type      = AnimatorComponent::ConditionType::Int;
    ci.paramName = "combo";
    ci.compare   = AnimatorComponent::Compare::NotEquals;
    ci.threshold = 4.0f;
    t.conditions.push_back(ci);
    AnimatorComponent::Condition cf;
    cf.type      = AnimatorComponent::ConditionType::Float;
    cf.paramName = "speed";
    cf.compare   = AnimatorComponent::Compare::Less;
    cf.threshold = 2.5f;
    t.conditions.push_back(cf);
    a->addTransition(t);

    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(found->hasAnimator());
    if (!found->hasAnimator()) return;

    auto la = found->getAnimator();
    CHECK(la->parameters().size() == 2);
    CHECK(la->parameters()[0].type == AnimatorComponent::ParamType::Int);
    CHECK(la->parameters()[1].type == AnimatorComponent::ParamType::Float);

    CHECK(la->transitions().size() == 1);
    if (la->transitions().empty()) return;
    const auto& lc = la->transitions()[0].conditions;
    CHECK(lc.size() == 2);
    if (lc.size() != 2) return;

    CHECK(lc[0].type    == AnimatorComponent::ConditionType::Int);
    CHECK(lc[0].compare == AnimatorComponent::Compare::NotEquals);
    CHECK(nearlyEqual(lc[0].threshold, 4.0f));
    CHECK(lc[1].type    == AnimatorComponent::ConditionType::Float);
    CHECK(lc[1].compare == AnimatorComponent::Compare::Less);
    CHECK(nearlyEqual(lc[1].threshold, 2.5f));
}

// Backward compatibility: a condition saved before this change carries no
// "compare" or "threshold" and must load with the struct's defaults, without
// warnings or nlohmann exceptions.
static void test_condition_without_compare_fields_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "ClipIdle";
    AnimatorComponent::State run;
    run.name = "Run"; run.clipName = "ClipRun";
    a->addState(idle);
    a->addState(run);
    a->addParameter("running", AnimatorComponent::ParamType::Bool);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    t.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "running", true });
    a->addTransition(t);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    // A Bool condition must not emit the numeric fields: they are noise in the
    // .scene and would confuse whoever reads it by hand. The tree is
    // root["root"]["children"], not a flat array (see Scene::toJson).
    bool checkedEmission = false;
    for (const auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        const auto& cond = node["animator"]["transitions"][0]["conditions"][0];
        CHECK(!cond.contains("compare"));
        CHECK(!cond.contains("threshold"));
        checkedEmission = true;
    }
    CHECK(checkedEmission);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found || !found->hasAnimator()) return;

    const auto& lc = found->getAnimator()->transitions()[0].conditions[0];
    CHECK(lc.type    == AnimatorComponent::ConditionType::Bool);
    CHECK(lc.expected);
    CHECK(lc.compare == AnimatorComponent::Compare::Greater);   // default
    CHECK(nearlyEqual(lc.threshold, 0.0f));                     // default
}

// The cross-fade duration survives save -> load. Two transitions with
// DIFFERENT durations and none at 0: a single shared constant, or a value
// that was not read and fell to the default, would pass with a single value.
static void test_crossfade_duration_survives_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State idle; idle.name = "Idle"; idle.clipName = "ClipIdle";
    AnimatorComponent::State run;  run.name  = "Run";  run.clipName  = "ClipRun";
    a->addState(idle);
    a->addState(run);
    a->addParameter("running", AnimatorComponent::ParamType::Bool);

    AnimatorComponent::Transition t0;
    t0.fromState = 0; t0.toState = 1; t0.duration = 0.35f;
    t0.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "running", true });
    a->addTransition(t0);

    AnimatorComponent::Transition t1;
    t1.fromState = 1; t1.toState = 0; t1.duration = 0.8f;
    t1.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "running", false });
    a->addTransition(t1);

    go->setAnimator(a);
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found || !found->hasAnimator()) return;

    const auto& tr = found->getAnimator()->transitions();
    CHECK(tr.size() == 2u);
    CHECK(nearlyEqual(tr[0].duration, 0.35f));
    CHECK(nearlyEqual(tr[1].duration, 0.8f));
    CHECK(!nearlyEqual(tr[0].duration, tr[1].duration));
}

// Backward compatibility: a scene saved BEFORE the cross-fade does not carry
// "duration" in its transitions. It must load with 0 (hard cut, the
// behavior as always), without warnings or exceptions.
static void test_transition_without_duration_field_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State idle; idle.name = "Idle"; idle.clipName = "ClipIdle";
    AnimatorComponent::State run;  run.name  = "Run";  run.clipName  = "ClipRun";
    a->addState(idle);
    a->addState(run);
    a->addParameter("running", AnimatorComponent::ParamType::Bool);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 0.5f;
    t.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "running", true });
    a->addTransition(t);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    // The field is deleted by hand: that IS an old scene. The tree is
    // root["root"]["children"] (see Scene::toJson).
    bool stripped = false;
    for (auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        node["animator"]["transitions"][0].erase("duration");
        stripped = true;
    }
    CHECK(stripped);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    CHECK(loaded.lastWarnings().empty());
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found || !found->hasAnimator()) return;

    CHECK(found->getAnimator()->transitions().size() == 1u);
    CHECK(nearlyEqual(found->getAnimator()->transitions()[0].duration, 0.0f));
}

// ── 1D Blend of N clips ─────────────────────────────────────────────────────
//
// Primary "Walk" (clip 0, threshold 1) IN THE MIDDLE, and the entries deliberately
// out of order: "Run" (clip 2, threshold 4) before "Idle" (clip 1, threshold 0). The
// thresholds are not equally spaced: a weight computed with the position in the
// list instead of the threshold would not pass.
static AnimatorComponent makeBlend1DGraph(float runThreshold = 4.0f, float idleThreshold = 0.0f)
{
    AnimatorComponent a;
    AnimatorComponent::State s;
    s.name = "Loco"; s.clipName = "Walk"; s.clipIndex = 0; s.duration = 40.0f;
    s.ticksPerSecond = 20.0f; s.loop = true;
    s.blendParam = "speed"; s.clipThreshold = 1.0f;
    s.blendEntries = { entrada("Run", 2, 100.0f, runThreshold), entrada("Idle", 1, 20.0f, idleThreshold) };
    a.addState(s);
    a.setEntryState(0);
    a.addParameter("speed", AnimatorComponent::ParamType::Float);
    a.reset();
    return a;
}

static void poner(AnimatorComponent& a, float speed)
{
    a.setFloat("speed", speed);
    a.update(0.0f, true);
}

// Between two thresholds: the neighboring pair with linear weight, wherever the
// primary is in the list.
static void test_blend1d_picks_neighbours_by_threshold()
{
    AnimatorComponent a = makeBlend1DGraph();
    poner(a, 2.5f);                              // between Walk(1) and Run(4)
    CHECK(a.poseClipA() == 0);
    CHECK(a.poseClipB() == 2);
    CHECK(nearlyEqual(a.poseWeight(), 0.5f));    // (2.5-1)/(4-1)
    poner(a, 0.25f);                             // between Idle(0) and Walk(1)
    CHECK(a.poseClipA() == 1);
    CHECK(a.poseClipB() == 0);
    CHECK(nearlyEqual(a.poseWeight(), 0.25f));
}

// Out of range: only the extreme, no extrapolation.
static void test_blend1d_clamps_to_extremes()
{
    AnimatorComponent a = makeBlend1DGraph();
    poner(a, -5.0f);
    CHECK(a.poseClipA() == 1 && a.poseClipB() == 1);
    CHECK(nearlyEqual(a.poseWeight(), 1.0f));
    poner(a, 9.0f);
    CHECK(a.poseClipA() == 2 && a.poseClipB() == 2);
    CHECK(nearlyEqual(a.poseWeight(), 1.0f));
    poner(a, 4.0f);                              // right at the last threshold
    CHECK(a.poseClipA() == 2 && a.poseClipB() == 2);
}

// Equal thresholds: only the first one counts (primary before entries, and
// among entries, the earlier one), from above and from below.
static void test_blend1d_equal_thresholds_first_wins()
{
    AnimatorComponent a = makeBlend1DGraph(/*run=*/1.0f, /*idle=*/0.0f);   // Run ties with Walk
    poner(a, 3.0f);
    CHECK(a.poseClipA() == 0 && a.poseClipB() == 0);
    poner(a, 0.5f);
    CHECK(a.poseClipA() == 1 && a.poseClipB() == 0);
    CHECK(nearlyEqual(a.poseWeight(), 0.5f));

    AnimatorComponent b = makeBlend1DGraph(/*run=*/4.0f, /*idle=*/4.0f);   // Idle ties with Run
    poner(b, 5.0f);
    CHECK(b.poseClipA() == 2 && b.poseClipB() == 2);
    poner(b, 2.5f);
    CHECK(b.poseClipB() == 2);
}

// An entry without a resolved clip does not take part; with none valid, the state is
// a single-clip one.
static void test_blend1d_ignores_unresolved_entries()
{
    AnimatorComponent a = makeBlend1DGraph();
    a.statesMutable()[0].blendEntries[1].clipIndex = -1;   // Idle unresolved
    poner(a, 0.25f);
    CHECK(a.poseClipA() == 0 && a.poseClipB() == 0);
    CHECK(nearlyEqual(a.poseWeight(), 1.0f));

    a.statesMutable()[0].blendEntries[0].clipIndex = -1;   // and Run too
    poner(a, 2.5f);
    CHECK(a.poseClipA() == 0 && a.poseClipB() == 0);
    CHECK(nearlyEqual(a.poseTimeB(), a.animTime()));
}

// Each clip is sampled at the primary's phase, scaled to ITS duration.
static void test_blend1d_samples_every_clip_at_principal_phase()
{
    AnimatorComponent a = makeBlend1DGraph();
    a.setFloat("speed", 2.5f);
    a.update(0.5f, true);                        // 10 ticks of 40 = phase 0.25
    CHECK(nearlyEqual(a.poseTimeA(), 10.0f));    // Walk
    CHECK(nearlyEqual(a.poseTimeB(), 25.0f));    // Run: 0.25 * 100
    poner(a, 0.25f);
    CHECK(nearlyEqual(a.poseTimeA(), 5.0f));     // Idle: 0.25 * 20
    CHECK(nearlyEqual(a.poseTimeB(), 10.0f));    // Walk
}

// Renaming touches the entries; rebindClips without the clip leaves the entry at -1 and
// does NOT delete it (the name is kept for when the FBX comes back).
static void test_blend1d_rename_and_rebind_entries()
{
    AnimatorComponent a = makeBlend1DGraph();
    CHECK(a.renameClipReferences("Run", "Sprint") == 1);
    CHECK(a.states()[0].blendEntries[0].clipName == "Sprint");

    SkinnedMesh mesh;
    AnimationClip walk; walk.name = "Walk"; walk.duration = 40.0f;
    AnimationClip idle; idle.name = "Idle"; idle.duration = 20.0f;
    mesh.animationClips = { walk, idle };
    std::vector<std::string> avisos;
    a.rebindClips(mesh, &avisos);
    CHECK(a.states()[0].blendEntries.size() == 2u);
    CHECK(a.states()[0].blendEntries[0].clipIndex == -1);
    CHECK(a.states()[0].blendEntries[1].clipIndex == 1);
    CHECK(nearlyEqual(a.states()[0].blendEntries[1].duration, 20.0f));
    CHECK(avisos.size() == 1u);
}

// Weight of each clip in the pose (A == B counts as weight 1 for that clip).
static float pesoDeClip(const AnimatorComponent& a, int clip)
{
    if (a.poseClipA() == a.poseClipB()) return a.poseClipA() == clip ? 1.0f : 0.0f;
    float w = 0.0f;
    if (a.poseClipA() == clip) w += 1.0f - a.poseWeight();
    if (a.poseClipB() == clip) w += a.poseWeight();
    return w;
}

// A scene saved with the old pair (blendClip/blendMin/blendMax) loads with the
// SAME pose: same weight per clip, with positive, negative and 0 span. A/B are not
// compared: with negative span they come out swapped.
static void test_blend1d_migrates_old_pair(PhysicsManager& pm, AudioManager& am)
{
    const float spans[][2] = { {1.5f, 6.5f}, {6.5f, 1.5f}, {2.0f, 2.0f} };
    for (const auto& mm : spans)
    {
        Scene scene("Test");
        GameObject* go = scene.addGameObject("Personaje");
        const uint64_t id = go->id;
        auto a = std::make_shared<AnimatorComponent>();
        AnimatorComponent::State s;
        s.name = "Loco"; s.clipName = "Walk"; s.blendParam = "speed";
        s.blendEntries = { entrada("Run", -1, 0.0f, 0.0f) };
        a->addState(s);
        a->addParameter("speed", AnimatorComponent::ParamType::Float);
        go->setAnimator(a);

        nlohmann::json j = scene.toJson();
        bool convertido = false;
        for (auto& node : j["root"]["children"])
        {
            if (!node.contains("animator")) continue;
            auto& js = node["animator"]["states"][0];
            js.erase("blendEntries");
            js.erase("clipThreshold");
            js["blendClip"] = "Run";
            js["blendMin"]  = mm[0];
            js["blendMax"]  = mm[1];
            convertido = true;
        }
        CHECK(convertido);

        Scene loaded("Loaded");
        CHECK(loaded.fromJson(j, pm, am));
        GameObject* found = loaded.findById(id);
        CHECK(found && found->hasAnimator());
        if (!found || !found->hasAnimator()) return;
        AnimatorComponent& b = *found->getAnimator();
        CHECK(b.states()[0].blendEntries.size() == 1u);
        if (b.states()[0].blendEntries.size() != 1u) return;
        // Without a mesh: the indices are resolved by hand.
        b.statesMutable()[0].clipIndex = 0;
        b.statesMutable()[0].duration  = 40.0f;
        b.statesMutable()[0].blendEntries[0].clipIndex = 1;
        b.statesMutable()[0].blendEntries[0].duration  = 40.0f;

        for (float p : { -1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 6.5f, 9.0f })
        {
            const float span = mm[1] - mm[0];
            float wViejo = std::fabs(span) < 1e-6f ? 0.0f : (p - mm[0]) / span;
            wViejo = wViejo < 0.0f ? 0.0f : (wViejo > 1.0f ? 1.0f : wViejo);
            b.setFloat("speed", p);
            b.update(0.0f, true);
            CHECK(nearlyEqual(pesoDeClip(b, 0), 1.0f - wViejo));
            CHECK(nearlyEqual(pesoDeClip(b, 1), wViejo));
        }
    }
}

// Round trip of the new format; on saving, the old format does not come out.
static void test_blend1d_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>(makeBlend1DGraph());
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    const std::string texto = j.dump();
    CHECK(texto.find("\"blendClip\"") == std::string::npos);
    CHECK(texto.find("\"blendMin\"") == std::string::npos);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& st = found->getAnimator()->states()[0];
    CHECK(st.blendParam == "speed");
    CHECK(nearlyEqual(st.clipThreshold, 1.0f));
    CHECK(st.blendEntries.size() == 2u);
    if (st.blendEntries.size() != 2u) return;
    CHECK(st.blendEntries[0].clipName == "Run" && nearlyEqual(st.blendEntries[0].threshold, 4.0f));
    CHECK(st.blendEntries[1].clipName == "Idle" && nearlyEqual(st.blendEntries[1].threshold, 0.0f));
}

// The parameter blend survives save -> load. Non-neutral values that are
// different from each other: min 1.5 and max 6.5 (not 0/1), and a second state WITHOUT blend
// so that an "applies to all equally" shows.
static void test_blend_state_survives_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State loco;
    loco.name = "Locomotion"; loco.clipName = "ClipWalk";
    loco.blendParam    = "speed";
    loco.clipThreshold = 1.5f;
    loco.blendEntries  = { entrada("ClipRun", -1, 0.0f, 6.5f) };
    AnimatorComponent::State idle;
    idle.name = "Idle"; idle.clipName = "ClipIdle";
    a->addState(loco);
    a->addState(idle);
    a->addParameter("speed", AnimatorComponent::ParamType::Float);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found || !found->hasAnimator()) return;

    const auto& st = found->getAnimator()->states();
    CHECK(st.size() == 2u);
    CHECK(st[0].blendEntries.size() == 1u);
    if (st[0].blendEntries.size() != 1u) return;
    CHECK(st[0].blendEntries[0].clipName == "ClipRun");
    CHECK(st[0].blendParam    == "speed");
    CHECK(nearlyEqual(st[0].clipThreshold, 1.5f));
    CHECK(nearlyEqual(st[0].blendEntries[0].threshold, 6.5f));
    CHECK(!nearlyEqual(st[0].clipThreshold, st[0].blendEntries[0].threshold));
    // The state without blend still has no blend
    CHECK(st[1].blendEntries.empty());
    CHECK(st[1].blendParam.empty());
}

// Backward compatibility: a state saved before the blend carries none of
// the four fields and must load as a single-clip state.
static void test_state_without_blend_fields_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s;
    s.name = "Locomotion"; s.clipName = "ClipWalk";
    s.blendParam = "speed"; s.clipThreshold = 1.5f;
    s.blendEntries = { entrada("ClipRun", -1, 0.0f, 6.5f) };
    a->addState(s);
    a->addParameter("speed", AnimatorComponent::ParamType::Float);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    // The blend fields are deleted by hand: that IS an old state.
    bool stripped = false;
    for (auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        auto& js = node["animator"]["states"][0];
        js.erase("blendEntries");
        js.erase("blendParam");
        js.erase("clipThreshold");
        stripped = true;
    }
    CHECK(stripped);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    CHECK(loaded.lastWarnings().empty());
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found || !found->hasAnimator()) return;

    const auto& st = found->getAnimator()->states()[0];
    CHECK(st.name == "Locomotion");
    CHECK(st.clipName == "ClipWalk");
    CHECK(st.blendEntries.empty());
    CHECK(st.blendParam.empty());
    // And with no entries the state does not blend
    found->getAnimator()->update(0.016f, true);
    CHECK(nearlyEqual(found->getAnimator()->poseWeight(), 1.0f));
}

// ── Root motion lock (lockRootMotion) ───────────────────────────────────────
//
// TWO-bone fixture: the root really MOVES (its bind pose is at
// (1.5, -0.5, 0.25) and the clips take it to x=4 and x=10) and it also rotates; the child
// animates too (its local goes off in z: 4 and 7 depending on the clip). Without the child, the
// test would not tell "root locked" from "animates nothing", and with the root's
// bind pose at the origin it would not tell "bind pose translation" from "zero".
static SkinnedMesh makeRootMotionFixture()
{
    SkinnedMesh m;
    m.skeleton.names       = { "root", "child" };
    m.skeleton.parentIndex = { -1, 0 };

    glm::mat4 bindRoot(1.0f);
    bindRoot[3] = glm::vec4(1.5f, -0.5f, 0.25f, 1.0f);
    glm::mat4 bindChildLocal(1.0f);
    bindChildLocal[3] = glm::vec4(0.0f, 2.0f, 0.0f, 1.0f);
    m.skeleton.inverseBindPose = { glm::inverse(bindRoot),
                                   glm::inverse(bindRoot * bindChildLocal) };

    const float rootX[2]   = { 4.0f, 10.0f };
    const float rootYaw[2] = { 90.0f, 30.0f };
    const float childZ[2]  = { 4.0f, 7.0f };
    const char* names[2]   = { "Walk", "Run" };
    const float dur[2]     = { 40.0f, 100.0f };
    const float tps[2]     = { 20.0f, 50.0f };

    for (int c = 0; c < 2; c++)
    {
        AnimationClip clip;
        clip.name = names[c]; clip.duration = dur[c]; clip.ticksPerSecond = tps[c];

        BoneChannel root;  root.boneIndex  = 0;
        BoneChannel child; child.boneIndex = 1;
        // Two keys with the SAME value: the clip is constant over time, so what
        // is asserted isolates the lock from the sampling within the clip.
        for (int k = 0; k < 2; k++)
        {
            const float t = (float)k * dur[c];
            root.posKeys.push_back({ t, glm::vec3(rootX[c], 0.0f, 0.0f) });
            root.rotKeys.push_back({ t, glm::angleAxis(glm::radians(rootYaw[c]),
                                                       glm::vec3(0.0f, 1.0f, 0.0f)) });
            root.scaleKeys.push_back({ t, glm::vec3(1.0f) });
            child.posKeys.push_back({ t, glm::vec3(0.0f, 2.0f, childZ[c]) });
            child.rotKeys.push_back({ t, glm::quat(1.0f, 0.0f, 0.0f, 0.0f) });
            child.scaleKeys.push_back({ t, glm::vec3(1.0f) });
        }
        clip.channels.push_back(root);
        clip.channels.push_back(child);
        m.animationClips.push_back(clip);
    }
    return m;
}

// With the flag: the root's local translation is that of its bind pose ON ALL THREE
// AXES, and its rotation comes out intact from the clip.
static void test_root_lock_pins_root_translation_to_bind_pose()
{
    SkinnedMesh  mesh = makeRootMotionFixture();
    PackedClips  p    = packSkinnedClips(mesh);
    const size_t B    = mesh.skeleton.names.size();
    const size_t run  = 1 * B;   // clip "Run": the root goes to x=10

    const glm::mat4 libre     = evalLocalXform(p, run, 0, 0.0f, false);
    const glm::mat4 bloqueada = evalLocalXform(p, run, 0, 0.0f, true);
    const glm::mat4 bind      = p.boneInfos[run].bindLocal;

    CHECK(nearlyEqual(libre[3].x, 10.0f));
    CHECK(nearlyEqual(bloqueada[3].x, bind[3].x));
    CHECK(nearlyEqual(bloqueada[3].y, bind[3].y));
    CHECK(nearlyEqual(bloqueada[3].z, bind[3].z));
    // And the bind pose is NOT the origin: an override that wrote vec3(0) dies here.
    CHECK(nearlyEqual(bloqueada[3].x, 1.5f));
    CHECK(nearlyEqual(bloqueada[3].y, -0.5f));
    CHECK(nearlyEqual(bloqueada[3].z, 0.25f));

    // Rotation and scale intact: the first three columns are the same as
    // without the flag, and they are not the identity (30° in Y).
    for (int c = 0; c < 3; c++)
        for (int r = 0; r < 4; r++)
            CHECK(nearlyEqual(bloqueada[c][r], libre[c][r]));
    CHECK(!nearlyIdentity(glm::mat4(glm::mat3(bloqueada))));
}

// With the flag: the child KEEPS moving relative to the root. This separates the
// lock from "the whole character froze".
static void test_root_lock_keeps_child_animating()
{
    SkinnedMesh  mesh = makeRootMotionFixture();
    PackedClips  p    = packSkinnedClips(mesh);
    const size_t B    = mesh.skeleton.names.size();
    const size_t walk = 0, run = 1 * B;

    const glm::mat4 hijoRun  = evalLocalXform(p, run,  1, 0.0f, true);
    const glm::mat4 hijoWalk = evalLocalXform(p, walk, 1, 0.0f, true);

    // The child comes from the clip, not from the lock: identical with and without the flag.
    CHECK(nearlyEqualMat(hijoRun, evalLocalXform(p, run, 1, 0.0f, false)));
    // And it has left its bind local (z=0), to a different place in each clip.
    CHECK(nearlyEqual(hijoRun[3].z,  7.0f));
    CHECK(nearlyEqual(hijoWalk[3].z, 4.0f));
    CHECK(!nearlyEqualMat(hijoRun, p.boneInfos[run + 1].bindLocal));

    // Meanwhile, the locked root is in the SAME place in both clips.
    CHECK(nearlyEqual(evalLocalXform(p, run,  0, 0.0f, true)[3].x,
                      evalLocalXform(p, walk, 0, 0.0f, true)[3].x));

    // And the child's skinning matrix is not the identity: it is not in bind pose.
    const std::vector<glm::mat4> finales = runBoneHierarchy(p, run, B, 0.0f, true);
    CHECK(!nearlyIdentity(finales[1]));
}

// Without the flag, the root moves: the behavior as always.
static void test_root_motion_unlocked_still_translates()
{
    SkinnedMesh  mesh = makeRootMotionFixture();
    PackedClips  p    = packSkinnedClips(mesh);
    const size_t B    = mesh.skeleton.names.size();

    CHECK(nearlyEqual(evalLocalXform(p, 0,     0, 0.0f)[3].x, 4.0f));
    CHECK(nearlyEqual(evalLocalXform(p, 1 * B, 0, 0.0f)[3].x, 10.0f));
    CHECK(!nearlyEqualMat(evalLocalXform(p, 1 * B, 0, 0.0f, false),
                          evalLocalXform(p, 1 * B, 0, 0.0f, true)));
}

// Graph of two states with cross-fade and a configurable flag on each one.
static AnimatorComponent makeRootLockFadeGraph(bool lockFrom, bool lockTo)
{
    AnimatorComponent a;

    AnimatorComponent::State from;
    from.name = "Walk"; from.clipName = "Walk";
    from.clipIndex = 0; from.duration = 40.0f; from.ticksPerSecond = 20.0f;
    from.rootMotion = lockFrom ? AnimatorComponent::RootMotion::Lock : AnimatorComponent::RootMotion::Off;

    AnimatorComponent::State to;
    to.name = "Run"; to.clipName = "Run";
    to.clipIndex = 1; to.duration = 100.0f; to.ticksPerSecond = 50.0f;
    to.rootMotion = lockTo ? AnimatorComponent::RootMotion::Lock : AnimatorComponent::RootMotion::Off;

    a.addState(from);
    a.addState(to);
    a.setEntryState(0);
    a.addParameter("go", AnimatorComponent::ParamType::Bool);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 0.5f;
    t.conditions.push_back({ AnimatorComponent::ConditionType::Bool, "go", true });
    a.addTransition(t);

    a.reset();
    return a;
}

// During a cross-fade the DESTINATION state rules, in both directions.
static void test_crossfade_root_lock_follows_destination_state()
{
    SkinnedMesh  mesh = makeRootMotionFixture();
    PackedClips  p    = packSkinnedClips(mesh);
    const size_t B    = mesh.skeleton.names.size();

    // Free origin -> locked destination: the flag takes effect as soon as the blend starts.
    AnimatorComponent a = makeRootLockFadeGraph(false, true);
    a.bindClips(mesh, nullptr);
    a.update(0.0f, true);
    CHECK(a.poseRootMotionMode() == 0u);
    a.setBool("go", true);
    a.update(0.0f, true);
    a.update(0.15f, true);
    CHECK(nearlyEqual(a.blendWeight(), 0.3f));
    CHECK(a.poseRootMotionMode() == 1u);

    const glm::mat4 mezclaLibre = evalLocalXformBlended(
        p, (size_t)a.poseClipA() * B, (size_t)a.poseClipB() * B, 0,
        a.poseTimeA(), a.poseTimeB(), a.poseWeight(), false);
    const glm::mat4 mezclaBloqueada = evalLocalXformBlended(
        p, (size_t)a.poseClipA() * B, (size_t)a.poseClipB() * B, 0,
        a.poseTimeA(), a.poseTimeB(), a.poseWeight(), a.poseRootMotionMode() == 1u);

    // mix(4, 10, 0.3) = 5.8 without lock; with lock, the bind pose translation.
    CHECK(nearlyEqual(mezclaLibre[3].x, 5.8f));
    CHECK(nearlyEqual(mezclaBloqueada[3].x, 1.5f));
    CHECK(nearlyEqual(mezclaBloqueada[3].y, -0.5f));
    CHECK(nearlyEqual(mezclaBloqueada[3].z, 0.25f));

    // Locked origin -> free destination: the destination rules the SAME, even though the
    // state being turned off is locked.
    AnimatorComponent b = makeRootLockFadeGraph(true, false);
    b.bindClips(mesh, nullptr);
    b.update(0.0f, true);
    CHECK(b.poseRootMotionMode() == 1u);
    b.setBool("go", true);
    b.update(0.0f, true);
    b.update(0.15f, true);
    CHECK(b.blending());
    CHECK(b.poseRootMotionMode() == 0u);
}

// Scene round trip with one locked state and another not in the SAME graph.
static void test_root_lock_survives_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State run;
    run.name = "Run"; run.clipName = "ClipRun"; run.rootMotion = AnimatorComponent::RootMotion::Lock;
    AnimatorComponent::State jump;
    jump.name = "Jump"; jump.clipName = "ClipJump"; jump.rootMotion = AnimatorComponent::RootMotion::Off;
    a->addState(run);
    a->addState(jump);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    // The key is only emitted when it is true: the free state does not carry it.
    bool checked = false;
    for (auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        CHECK(node["animator"]["states"][0].contains("rootMotion"));
        CHECK(!node["animator"]["states"][1].contains("rootMotion"));
        checked = true;
    }
    CHECK(checked);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found || !found->hasAnimator()) return;

    const auto& st = found->getAnimator()->states();
    CHECK(st.size() == 2u);
    CHECK(st[0].rootMotion == AnimatorComponent::RootMotion::Lock);
    CHECK(st[1].rootMotion == AnimatorComponent::RootMotion::Off);
}

// Backward compatibility: a state saved before this feature does not carry the key
// and has to load as false, without warnings.
static void test_state_without_lock_root_motion_field_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s;
    s.name = "Run"; s.clipName = "ClipRun"; s.rootMotion = AnimatorComponent::RootMotion::Lock;
    a->addState(s);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();

    bool stripped = false;
    for (auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        node["animator"]["states"][0].erase("rootMotion");
        stripped = true;
    }
    CHECK(stripped);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    CHECK(loaded.lastWarnings().empty());
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found || !found->hasAnimator()) return;

    const auto& st = found->getAnimator()->states()[0];
    CHECK(st.name == "Run");
    CHECK(st.rootMotion == AnimatorComponent::RootMotion::Off);
}

// H4 of docs/core-audit.md: removeState and setEntryState ended in reset(),
// which clears the current state AND ALL the user's parameters. The header already
// documents that danger word for word (and that is why rebindClips exists, which
// is bindClips without the reset()) but only for the animation sources path;
// the two neighbors were left with the reset() in place.
//
// And they are reachable in Play: AnimatorPanel.cpp calls both and that file
// does not check isPlaying even once. Touching the graph mid-game reset the
// state machine and the bool/trigger/int/float values that the Lua script had
// been writing.
static void test_remove_state_keeps_params_and_playhead()
{
    AnimatorComponent a;
    AnimatorComponent::State idle;    idle.name    = "Idle";
    AnimatorComponent::State andar;   andar.name   = "Andar";
    AnimatorComponent::State correr;  correr.name  = "Correr";
    a.addState(idle); a.addState(andar); a.addState(correr);

    // The playhead at "Correr" (index 2), and then the parameters: the other way round it
    // would not work, because setEntryState would clear them.
    a.setEntryState(2);
    CHECK(a.currentStateName() == "Correr");
    a.addParameter("velocidad", AnimatorComponent::ParamType::Float);
    a.addParameter("saltando", AnimatorComponent::ParamType::Bool);
    a.setFloat("velocidad", 0.75f);
    a.setBool("saltando", true);

    // A state that is NOT the current one is deleted: the user is reordering the
    // graph, not restarting the game.
    a.removeState(0);

    CHECK(a.getFloat("velocidad") == 0.75f);
    CHECK(a.getBool("saltando"));
    // The playhead stays on the SAME state, even though its index has shifted.
    CHECK(a.currentStateName() == "Correr");
    CHECK(a.currentState() == 1);
}

// If the one deleted IS the current one there is nowhere to continue, so the playhead falls
// to the entry state, but the parameters still have nothing to do with it.
static void test_remove_current_state_falls_back_but_keeps_params()
{
    AnimatorComponent a;
    AnimatorComponent::State uno; uno.name = "Uno";
    AnimatorComponent::State dos; dos.name = "Dos";
    a.addState(uno); a.addState(dos);
    a.setEntryState(1);
    a.addParameter("vida", AnimatorComponent::ParamType::Int);
    a.setInt("vida", 3);
    CHECK(a.currentStateName() == "Dos");

    a.removeState(1);   // el actual

    CHECK(a.getInt("vida") == 3);
    CHECK(a.currentStateName() == "Uno");
}

// setEntryState moves the playhead on purpose (the editor preview has to
// follow the new entry), but that is no reason to clear parameters.
static void test_set_entry_state_keeps_params()
{
    AnimatorComponent a;
    AnimatorComponent::State uno; uno.name = "Uno";
    AnimatorComponent::State dos; dos.name = "Dos";
    a.addState(uno); a.addState(dos);
    a.addParameter("mana", AnimatorComponent::ParamType::Float);
    a.setFloat("mana", 12.5f);

    a.setEntryState(1);

    CHECK(a.getFloat("mana") == 12.5f);
    CHECK(a.currentStateName() == "Dos");
}

// The playhead is REINDEXED, it does not jump to the entry. This test exists because the
// previous one did not tell them apart: there the entry WAS the current state, so
// "reindex" and "jump to the entry" gave the same result and a sabotage
// that swapped one for the other went unnoticed. Here "Correr" is reached
// through a TRANSITION, with the entry elsewhere: deleting a third
// state cannot send the user back to the start of the graph.
static void test_remove_state_reindexes_playhead_not_resets_it()
{
    AnimatorComponent a;
    AnimatorComponent::State idle;   idle.name   = "Idle";
    AnimatorComponent::State andar;  andar.name  = "Andar";
    AnimatorComponent::State correr; correr.name = "Correr";
    a.addState(idle); a.addState(andar); a.addState(correr);
    a.addParameter("ir", AnimatorComponent::ParamType::Bool);

    AnimatorComponent::Transition t;
    t.fromState = 0;
    t.toState   = 2;
    AnimatorComponent::Condition c;
    c.type      = AnimatorComponent::ConditionType::Bool;
    c.paramName = "ir";
    c.expected  = true;
    t.conditions.push_back(c);
    a.addTransition(t);

    a.setBool("ir", true);
    a.update(1.0f / 60.0f, /*evaluateTransitions=*/true);
    CHECK(a.currentStateName() == "Correr");
    CHECK(a.currentState() == 2);
    CHECK(a.entryState() == 0);

    a.removeState(1);   // neither the current one nor the entry

    CHECK(a.currentStateName() == "Correr");
    CHECK(a.currentState() == 1);     // reindexed 2 -> 1, not sent back to 0
    CHECK(a.entryState() == 0);
}

// ---- applyGraph: the graph undo replaces what was authored without touching the runtime ----

// Minimal state with a clip of 100 ticks at 10 ticks/s: 1 s of update = 10 ticks.
static AnimatorComponent::State makeTimedState(const char* name)
{
    AnimatorComponent::State s;
    s.name = name; s.clipName = name;
    s.duration = 100.0f; s.ticksPerSecond = 10.0f;
    return s;
}

// The values the script had been writing survive an undo if the
// parameter is still the same (name AND type). One that changed type
// goes back to its default value: the old value means nothing in the new
// type.
static void test_apply_graph_keeps_param_values_of_same_name_and_type()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addParameter("speed", AnimatorComponent::ParamType::Float);
    a.addParameter("jump",  AnimatorComponent::ParamType::Bool);
    a.setFloat("speed", 3.5f);
    a.setBool("jump", true);

    AnimatorComponent::Graph g = a.graph();
    for (auto& p : g.parameters)
        if (p.name == "jump") p.type = AnimatorComponent::ParamType::Int;
    a.applyGraph(g);

    CHECK(nearlyEqual(a.getFloat("speed"), 3.5f));
    CHECK(a.getInt("jump") == 0);
    CHECK(!a.getBool("jump"));
}

// An undo that reinserts a state IN FRONT of the current one changes the index of the
// current one. The playhead is matched by editorId: it stays on the same state, with its
// time.
static void test_apply_graph_playhead_follows_editor_id()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.setEntryState(1);
    a.update(1.0f, false);   // B, 10 ticks
    CHECK(a.currentStateName() == "B");

    AnimatorComponent::Graph g = a.graph();
    AnimatorComponent::State c = makeTimedState("C");
    c.editorId = 99;
    g.states.insert(g.states.begin(), c);
    g.entryState = 2;
    a.applyGraph(g);

    CHECK(a.currentState() == 2);
    CHECK(a.currentStateName() == "B");
    CHECK(nearlyEqual(a.animTime(), 10.0f));
}

// If the applied graph no longer has the current state, there is nothing to match:
// it falls to the entry with time 0.
static void test_apply_graph_missing_current_state_falls_to_entry()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.setEntryState(1);
    a.update(1.0f, false);

    AnimatorComponent::Graph g = a.graph();
    g.states.erase(g.states.begin() + 1);
    g.entryState = 0;
    a.applyGraph(g);

    CHECK(a.currentState() == 0);
    CHECK(nearlyEqual(a.animTime(), 0.0f));
}

// Undoing the creation of B leaves the graph with only A, but B's id cannot be
// handed out again: a redo will bring it back and two nodes with the same id
// share a visual slot in imgui-node-editor.
static void test_apply_graph_never_lowers_next_editor_id()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    const AnimatorComponent::Graph soloA = a.graph();
    const int idB = a.states()[a.addState(makeTimedState("B"))].editorId;

    a.applyGraph(soloA);
    const int idC = a.states()[a.addState(makeTimedState("C"))].editorId;

    CHECK(idC != idB);
    CHECK(idC != a.states()[0].editorId);
}

// Moving nodes does not enter the undo: when undoing something else, a live node stays
// where it is NOW. Only the one that comes back from a deletion takes the position from the
// snapshot.
static void test_apply_graph_live_states_keep_position()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    AnimatorComponent::State b = makeTimedState("B");
    b.editorPos = glm::vec2(7.0f, 8.0f);
    a.addState(b);
    const AnimatorComponent::Graph snapshot = a.graph();

    a.statesMutable()[0].editorPos = glm::vec2(50.0f, 60.0f);   // the user moves A
    a.removeState(1);                                            // and deletes B
    a.applyGraph(snapshot);                                      // undo of the deletion

    CHECK(a.states().size() == 2);
    CHECK(a.states()[0].editorPos == glm::vec2(50.0f, 60.0f));
    CHECK(a.states()[1].editorPos == glm::vec2(7.0f, 8.0f));
}

// A -> B by trigger with a 1 s cross-fade, already fired: leaves a blend in
// flight with A fading out.
static void makeCrossfadeInFlight(AnimatorComponent& a)
{
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addParameter("t", AnimatorComponent::ParamType::Trigger);
    a.addParameter("v", AnimatorComponent::ParamType::Float);
    AnimatorComponent::Transition tr;
    tr.fromState = 0; tr.toState = 1; tr.duration = 1.0f;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "t";
    tr.conditions.push_back(c);
    a.addTransition(tr);

    a.setFloat("v", 2.0f);
    a.update(0.1f, true);
    a.setTrigger("t");
    a.update(0.1f, true);
}

// Applying the graph itself cannot change anything visible: neither parameters, nor
// playhead, nor the blend in flight. It is the case of undoing, in Play, an
// edit that does not touch the states being blended.
static void test_apply_own_graph_changes_nothing()
{
    AnimatorComponent a;
    makeCrossfadeInFlight(a);
    CHECK(a.blending());

    const int   cur = a.currentState(),  prev = a.previousState();
    const float t   = a.animTime(),      pt   = a.previousAnimTime();
    const float w   = a.blendWeight();

    a.applyGraph(a.graph());

    CHECK(a.blending());
    CHECK(a.currentState() == cur);
    CHECK(a.previousState() == prev);
    CHECK(nearlyEqual(a.animTime(), t));
    CHECK(nearlyEqual(a.previousAnimTime(), pt));
    CHECK(nearlyEqual(a.blendWeight(), w));
    CHECK(nearlyEqual(a.getFloat("v"), 2.0f));
}

// Without the state being turned off there is nothing to blend against: the blend is cut.
static void test_apply_graph_without_fading_state_cuts_crossfade()
{
    AnimatorComponent a;
    makeCrossfadeInFlight(a);
    CHECK(a.blending());

    AnimatorComponent::Graph g = a.graph();
    g.states.erase(g.states.begin());   // A, the one that was fading out
    g.transitions.clear();              // pointed at A
    g.entryState = 0;
    a.applyGraph(g);

    CHECK(!a.blending());
    CHECK(a.currentStateName() == "B");
}

// The other half: if the missing one is the CURRENT state (B), the playhead falls to the
// entry, and blending from A toward a state that is not the transition's
// would be a pose nobody asked for. The blend is cut too.
static void test_apply_graph_without_current_state_cuts_crossfade()
{
    AnimatorComponent a;
    makeCrossfadeInFlight(a);
    CHECK(a.blending());

    AnimatorComponent::Graph g = a.graph();
    g.states.erase(g.states.begin() + 1);   // B, el actual
    g.transitions.clear();                  // pointed at B
    g.entryState = 0;
    a.applyGraph(g);

    CHECK(!a.blending());
    CHECK(a.currentStateName() == "A");
}

// ---- Graph undo key: what is saved, minus the position of the nodes ----

// Base graph of the undo tests: two states (A blends with another clip, so
// animatorToJson also emits the blend fields), a Bool parameter, a
// Float and an Int, and a transition A->B with a Bool condition and a Float one
// (so expected, compare and threshold are emitted).
static void makeBaseGraph(AnimatorComponent& a)
{
    AnimatorComponent::State sa;
    sa.name = "A"; sa.clipName = "walk";
    sa.blendParam = "speed"; sa.clipThreshold = 0.0f;
    sa.blendEntries = { entrada("run", -1, 0.0f, 1.0f) };
    sa.duration = 40.0f; sa.ticksPerSecond = 20.0f;
    AnimatorComponent::State sb;
    sb.name = "B"; sb.clipName = "run";
    sb.duration = 100.0f; sb.ticksPerSecond = 50.0f;
    a.addState(sa);
    a.addState(sb);
    a.addParameter("go",    AnimatorComponent::ParamType::Bool);
    a.addParameter("speed", AnimatorComponent::ParamType::Float);
    a.addParameter("hits",  AnimatorComponent::ParamType::Int);

    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 0.25f;
    AnimatorComponent::Condition cb;
    cb.type = AnimatorComponent::ConditionType::Bool;
    cb.paramName = "go"; cb.expected = true;
    AnimatorComponent::Condition cf;
    cf.type = AnimatorComponent::ConditionType::Float;
    cf.paramName = "speed";
    cf.compare = AnimatorComponent::Compare::Greater; cf.threshold = 0.5f;
    t.conditions = { cb, cf };
    t.hasExitTime = true;       // so the .scene emits exitTime and the mutations see it
    t.exitTime    = 0.75f;
    a.addTransition(t);

    // An Any State transition, so that canTransitionToSelf can be mutated.
    AnimatorComponent::Transition any;
    any.fromState = AnimatorComponent::kAnyState;
    any.toState   = 1;
    a.addTransition(any);
}

// One mutation for each thing the AnimatorPanel can change and the .scene
// saves, made through the same Core API the panel uses. On top of makeBaseGraph.
using GraphMutation = std::pair<const char*, std::function<void(AnimatorComponent&)>>;
static std::vector<GraphMutation> graphMutations()
{
    using A = AnimatorComponent;
    return {
        { "state.name",           [](A& a) { a.statesMutable()[0].name = "X"; } },
        { "state.clip",           [](A& a) { a.statesMutable()[0].clipName = "idle"; } },
        { "state.loop",           [](A& a) { a.statesMutable()[0].loop = !a.states()[0].loop; } },
        { "state.blendEntry.clip",      [](A& a) { a.statesMutable()[0].blendEntries[0].clipName = "idle"; } },
        { "state.blendParam",           [](A& a) { a.statesMutable()[0].blendParam = "hits"; } },
        { "state.clipThreshold",        [](A& a) { a.statesMutable()[0].clipThreshold = 0.25f; } },
        { "state.blendEntry.threshold", [](A& a) { a.statesMutable()[0].blendEntries[0].threshold = 2.0f; } },
        { "state.addBlendEntry",        [](A& a) { a.statesMutable()[0].blendEntries.push_back(entrada("idle", -1, 0.0f, 3.0f)); } },
        { "state.events",               [](A& a) { a.statesMutable()[0].events.push_back({ "paso", 0.5f }); } },
        { "state.rootMotion",     [](A& a) { a.statesMutable()[0].rootMotion = A::RootMotion::Apply; } },
        { "state.speed",          [](A& a) { a.statesMutable()[0].speed = 2.0f; } },
        { "state.speedParam",     [](A& a) { a.statesMutable()[0].speedParam = "speed"; } },
        { "addState",             [](A& a) { A::State s; s.name = "C"; s.clipName = "idle"; a.addState(s); } },
        { "removeState",          [](A& a) { a.removeState(1); } },
        { "entryState",           [](A& a) { a.setEntryState(1); } },
        { "addParameter",         [](A& a) { a.addParameter("nuevo", A::ParamType::Trigger); } },
        { "removeParameter",      [](A& a) { a.removeParameter("hits"); } },
        { "parameter.name",       [](A& a) { A::Graph g = a.graph(); g.parameters[2].name = "golpes"; a.applyGraph(g); } },
        { "parameter.type",       [](A& a) { A::Graph g = a.graph(); g.parameters[2].type = A::ParamType::Float; a.applyGraph(g); } },
        { "addTransition",        [](A& a) { A::Transition t; t.fromState = 1; t.toState = 0; a.addTransition(t); } },
        { "removeTransition",     [](A& a) { a.removeTransition(0); } },
        { "transition.from",      [](A& a) { a.transitionsMutable()[0].fromState = 1; } },
        { "transition.to",        [](A& a) { a.transitionsMutable()[0].toState = 0; } },
        { "transition.duration",  [](A& a) { a.transitionsMutable()[0].duration = 1.5f; } },
        { "condition.add",        [](A& a) { A::Condition c; c.type = A::ConditionType::AnimationFinished;
                                              a.transitionsMutable()[0].conditions.push_back(c); } },
        { "condition.remove",     [](A& a) { a.transitionsMutable()[0].conditions.pop_back(); } },
        { "condition.type",       [](A& a) { a.transitionsMutable()[0].conditions[0].type = A::ConditionType::Trigger; } },
        { "condition.param",      [](A& a) { a.transitionsMutable()[0].conditions[0].paramName = "otro"; } },
        { "condition.expected",   [](A& a) { a.transitionsMutable()[0].conditions[0].expected = false; } },
        { "condition.compare",    [](A& a) { a.transitionsMutable()[0].conditions[1].compare = A::Compare::Less; } },
        { "condition.threshold",  [](A& a) { a.transitionsMutable()[0].conditions[1].threshold = 9.0f; } },
        { "transition.hasExitTime",       [](A& a) { a.transitionsMutable()[0].hasExitTime = false; } },
        { "transition.exitTime",          [](A& a) { a.transitionsMutable()[0].exitTime = 0.25f; } },
        { "anyState.canTransitionToSelf", [](A& a) { a.transitionsMutable()[1].canTransitionToSelf = true; } },
        { "anyState.addTransition",       [](A& a) { A::Transition t; t.fromState = A::kAnyState; t.toState = 0;
                                                     a.addTransition(t); } },
    };
}

// Moving a node is not an edit for the undo (design decision). The
// .scene format DOES save the position: it is checked too, so that the
// test does not pass just because animatorToJson stopped emitting it.
static void test_graph_key_ignores_node_position()
{
    AnimatorComponent a;
    makeBaseGraph(a);
    const nlohmann::json k0 = animatorGraphKey(a);

    a.statesMutable()[0].editorPos = glm::vec2(123.0f, 456.0f);

    CHECK(animatorGraphKey(a) == k0);
    CHECK(animatorToJson(a)["states"][0].contains("pos"));

    // And moving the Any State node is not an edit either.
    const nlohmann::json k1 = animatorGraphKey(a);
    a.setAnyStateEditorPos(glm::vec2(9.0f, 9.0f));
    CHECK(animatorGraphKey(a) == k1);
    CHECK(animatorToJson(a).contains("anyStatePos"));
}

// Everything that is saved has to show in the key: a field that did not show
// would be an edit that leaves no undo entry, and without warning.
static void test_graph_key_sees_every_saved_field()
{
    AnimatorComponent base;
    makeBaseGraph(base);
    const nlohmann::json k0 = animatorGraphKey(base);

    for (const auto& [name, mutate] : graphMutations())
    {
        AnimatorComponent a = base;
        mutate(a);
        if (animatorGraphKey(a) == k0)
        {
            std::printf("FAIL: la clave del grafo no ve '%s'\n", name);
            ++g_failures;
        }
    }
}

// ---- AnimatorGraphCommand ----

// For each thing the panel can edit: undo leaves the graph as it was and
// redo leaves it edited again.
static void test_graph_command_round_trip_for_each_mutation()
{
    for (const auto& [name, mutate] : graphMutations())
    {
        Scene scene("Test");
        GameObject* go = scene.addGameObject("Personaje");
        auto a = std::make_shared<AnimatorComponent>();
        makeBaseGraph(*a);
        go->setAnimator(a);

        const AnimatorComponent::Graph before    = a->graph();
        const nlohmann::json           keyBefore = animatorGraphKey(*a);
        mutate(*a);
        const nlohmann::json           keyAfter  = animatorGraphKey(*a);

        AnimatorGraphCommand cmd(scene, name, go->id, before, a->graph());

        cmd.undo();
        if (animatorGraphKey(*a) != keyBefore)
        {
            std::printf("FAIL: el undo de '%s' no restaura el grafo\n", name);
            ++g_failures;
        }
        cmd.execute();
        if (animatorGraphKey(*a) != keyAfter)
        {
            std::printf("FAIL: el redo de '%s' no vuelve a aplicar el grafo\n", name);
            ++g_failures;
        }
    }
}

// The Animator may have disappeared between the gesture and the undo (a later
// AnimatorComponentCommand removed it), or the whole object.
static void test_graph_command_noop_without_animator()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);
    go->setAnimator(a);
    const uint64_t id = go->id;

    AnimatorGraphCommand cmd(scene, "Edit Animator", id, a->graph(), a->graph());

    go->setAnimator(nullptr);
    cmd.undo();
    cmd.execute();
    CHECK(!go->hasAnimator());

    scene.removeGameObject(go);
    cmd.undo();      // findById returns nullptr and it returns without touching anything
    cmd.execute();
}

// The snapshot carries the clipIndex from when it was taken; in the meantime an
// animation source may have changed the clip list. After applying, the indices
// come out resolved against the CURRENT mesh.
static void test_graph_command_rebinds_clips()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto mesh = std::make_shared<SkinnedMesh>();
    AnimationClip walk; walk.name = "walk"; walk.duration = 40.0f;  walk.ticksPerSecond = 20.0f;
    AnimationClip run;  run.name  = "run";  run.duration  = 100.0f; run.ticksPerSecond  = 50.0f;
    mesh->animationClips = { walk, run };
    go->setMesh(mesh); mesh = soloObservador(mesh);

    auto a = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);   // A usa "walk" (+ "run" de blend), B usa "run"
    go->setAnimator(a);

    AnimatorComponent::Graph stale = a->graph();
    for (auto& s : stale.states) { s.clipIndex = -1; for (auto& e : s.blendEntries) e.clipIndex = -1; }

    // Live runtime before the undo: parameter value and advanced clock. If
    // apply() used bindClips instead of rebindClips, its reset() would clear them
    // and the CHECKs below would give it away even if the clipIndex values come out right.
    a->setFloat("speed", 3.5f);
    a->update(0.5f, /*evaluateTransitions=*/false);
    const float tiempo = a->animTime();

    AnimatorGraphCommand cmd(scene, "Edit Animator", go->id, stale, stale);
    cmd.undo();

    CHECK(a->states()[0].clipIndex == 0);
    CHECK(a->states()[0].blendEntries[0].clipIndex == 1);
    CHECK(a->states()[1].clipIndex == 1);
    // rebindClips, not bindClips: the runtime (parameter and playhead) survives
    // the undo. bindClips would pass the three CHECKs above and still clear
    // this silently.
    CHECK(nearlyEqual(a->getFloat("speed"), 3.5f));
    CHECK(nearlyEqual(a->animTime(), tiempo));
}

// ---- AnimatorGraphUndoTracker: one command per gesture ----

// A drag is several frames with the widget active and a release frame. The whole
// drag has to be ONE command, and its 'before' is the graph from before the first
// frame.
static void test_tracker_drag_yields_one_command()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);
    go->setAnimator(a);
    const nlohmann::json keyBefore = animatorGraphKey(*a);

    AnimatorGraphUndoTracker tr;
    const uint64_t rev = 7;
    int durante = 0;
    for (int f = 0; f < 3; f++)
    {
        tr.beginFrame(go->id, a.get(), rev);
        a->transitionsMutable()[0].duration += 0.1f;
        if (tr.endFrame(scene, a.get(), /*anyItemActive=*/true, rev)) durante++;
    }
    tr.beginFrame(go->id, a.get(), rev);
    std::unique_ptr<ICommand> cmd = tr.endFrame(scene, a.get(), /*anyItemActive=*/false, rev);

    CHECK(durante == 0);
    CHECK(cmd != nullptr);
    if (cmd)
    {
        cmd->undo();
        CHECK(animatorGraphKey(*a) == keyBefore);
    }
}

// A drag that ends where it started is not an edit.
static void test_tracker_drag_back_to_start_yields_nothing()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);
    go->setAnimator(a);
    const float original = a->transitions()[0].duration;

    AnimatorGraphUndoTracker tr;
    tr.beginFrame(go->id, a.get(), 1);
    a->transitionsMutable()[0].duration = 3.0f;
    CHECK(tr.endFrame(scene, a.get(), true, 1) == nullptr);
    tr.beginFrame(go->id, a.get(), 1);
    a->transitionsMutable()[0].duration = original;
    CHECK(tr.endFrame(scene, a.get(), false, 1) == nullptr);
}

// If the history moved during the gesture (a command of the panel's own, a
// Ctrl+Z, the clear() of Play), the difference is no longer just the user's: nothing
// is emitted. And the new baseline is good: the next frame, without changes,
// emits nothing either.
static void test_tracker_revision_change_rebases()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);
    go->setAnimator(a);

    AnimatorGraphUndoTracker tr;
    tr.beginFrame(go->id, a.get(), 1);
    a->statesMutable()[0].clipName = "idle";   // p. ej. un ClipRenameCommand
    CHECK(tr.endFrame(scene, a.get(), false, /*undoRevision=*/2) == nullptr);

    tr.beginFrame(go->id, a.get(), 2);
    CHECK(tr.endFrame(scene, a.get(), false, 2) == nullptr);
}

// Changing GameObject closes the previous one's session: its 'before' cannot
// end up in a command against the new object.
static void test_tracker_selection_change_discards_session()
{
    Scene scene("Test");
    GameObject* goA = scene.addGameObject("A");
    GameObject* goB = scene.addGameObject("B");
    auto a = std::make_shared<AnimatorComponent>();
    auto b = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);
    makeBaseGraph(*b);
    // B different from A: if A's session survived the selection change,
    // its 'before' (the base graph) would not match B and a command would come out.
    b->statesMutable()[0].name = "OtroGrafo";
    goA->setAnimator(a);
    goB->setAnimator(b);

    AnimatorGraphUndoTracker tr;
    tr.beginFrame(goA->id, a.get(), 1);
    a->transitionsMutable()[0].duration = 3.0f;
    CHECK(tr.endFrame(scene, a.get(), true, 1) == nullptr);   // drag in progress on A

    tr.beginFrame(goB->id, b.get(), 1);                       // the selection moves to B
    CHECK(tr.endFrame(scene, b.get(), false, 1) == nullptr);  // B has not changed
}

// Moving nodes does not enter the undo.
static void test_tracker_node_move_yields_nothing()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);
    go->setAnimator(a);

    AnimatorGraphUndoTracker tr;
    tr.beginFrame(go->id, a.get(), 1);
    a->statesMutable()[0].editorPos = glm::vec2(99.0f, 99.0f);
    CHECK(tr.endFrame(scene, a.get(), false, 1) == nullptr);
}

// The label that a site in the panel sets is valid for THAT gesture; the next one
// goes back to the generic one.
static void test_tracker_label_is_per_gesture()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>();
    makeBaseGraph(*a);
    go->setAnimator(a);

    AnimatorGraphUndoTracker tr;
    tr.beginFrame(go->id, a.get(), 1);
    tr.setLabel("Borrar estado");
    a->removeState(1);
    std::unique_ptr<ICommand> c1 = tr.endFrame(scene, a.get(), false, 1);

    tr.beginFrame(go->id, a.get(), 1);
    a->statesMutable()[0].loop = !a->states()[0].loop;
    std::unique_ptr<ICommand> c2 = tr.endFrame(scene, a.get(), false, 1);

    CHECK(c1 && c1->label() == "Borrar estado");
    CHECK(c2 && c2->label() == "Edit Animator");
}

// ---- applySkinnedFrame: the block the three hosts repeated ----

// Double with the FIVE methods the helper touches and no more. That is why the
// helper is a template and does not take `EditorRenderer&`: that interface is 75 pure
// methods, and nobody writes a double of 75 stubs to test five calls.
// It records the ORDER, which is half of what has to be protected here.
static AnimatorComponent makeTwoBlendStates();   // further down, with the pose tests

struct SkinnedRendererDoble
{
    std::vector<std::string> orden;
    bool      visible       = false;
    AnimationPose pose;
    bool      poseRecibida  = false;
    AnimationIk ik;
    bool      ikRecibida    = false;
    bool      transformEstaticoRecibido = false;
    int       transformEstaticoIndice    = -1;
    glm::mat4 transformEstatico          = glm::mat4(0.0f);
    bool      factoresRecibidos = false;
    size_t    factoresIndice    = 0;
    float     factorMetallic    = -1.0f;
    float     factorRoughness   = -1.0f;
    float     dtSinAnimator = -1.0f;
    glm::mat4 transform     = glm::mat4(0.0f);
    float     ssr           = -1.0f;

    void setSkinnedMeshVisible(int, bool v) { orden.push_back("visible"); visible = v; }
    void updateAnimation(int, float dt)     { orden.push_back("updateAnimation"); dtSinAnimator = dt; }
    void setAnimationPose(int, const AnimationPose& p)
    {
        orden.push_back("blend");
        pose = p; poseRecibida = true;
    }
    void setAnimationIk(int, const AnimationIk& v) { orden.push_back("ik"); ik = v; ikRecibida = true; }
    void setTransform(int i, const glm::mat4& m)
    {
        orden.push_back("transformEstatico");
        transformEstaticoIndice = i; transformEstatico = m; transformEstaticoRecibido = true;
    }
    void setObjectMaterialFactors(size_t i, float m, float r)
    {
        orden.push_back("factores");
        factoresRecibidos = true; factoresIndice = i; factorMetallic = m; factorRoughness = r;
    }
    void setSkinnedTransform(int, const glm::mat4& m) { orden.push_back("transform"); transform = m; }
    void setSkinnedSsr(int, float s)                  { orden.push_back("ssr"); ssr = s; }

    int indiceDe(const char* nombre) const
    {
        for (size_t i = 0; i < orden.size(); i++)
            if (orden[i] == nombre) return (int)i;
        return -1;
    }
};

static GameObject* makeSkinnedGameObject(Scene& scene, std::shared_ptr<AnimatorComponent> anim)
{
    GameObject* go = scene.addGameObject("Personaje");
    go->skinnedRenderIndex = 0;
    go->meshVisible        = true;
    go->ssrEnabled         = true;
    go->ssrIntensity       = 0.4f;
    if (anim) go->setAnimator(std::move(anim));
    return go;
}

// A->B graph by trigger, with the trigger ALREADY armed and the playhead at the entry.
static std::shared_ptr<AnimatorComponent> makeArmedTriggerGraph()
{
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State sa;
    sa.name = "A"; sa.clipName = "A"; sa.duration = 100.0f; sa.ticksPerSecond = 10.0f;
    AnimatorComponent::State sb = sa;
    sb.name = "B"; sb.clipName = "B";
    a->addState(sa);
    a->addState(sb);
    a->addParameter("t", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition tr;
    tr.fromState = 0; tr.toState = 1;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "t";
    tr.conditions.push_back(c);
    a->addTransition(tr);
    a->update(0.0f, false);      // pins the playhead at the entry
    a->setTrigger("t");
    return a;
}

// The visible flag has to go BEFORE touching the animation: the backend
// freezes the clock of a hidden mesh, so setting it afterwards leaves it one frame
// behind. Until now that rule lived in a comment in runtime/main.cpp, which
// means it was lost as soon as someone copied the block elsewhere.
static void test_apply_skinned_frame_sets_visible_before_animation()
{
    Scene scene("Test");
    auto a = std::make_shared<AnimatorComponent>();
    makeCrossfadeInFlight(*a);
    GameObject* go = makeSkinnedGameObject(scene, a);

    SkinnedRendererDoble r;
    applySkinnedFrame(*go, r, 0.016f, /*evaluateTransitions=*/true);

    CHECK(r.indiceDe("visible") == 0);
    CHECK(r.indiceDe("blend") > r.indiceDe("visible"));
    CHECK(r.visible);
}

// The seven pose values, and above all WHICH goes where: the backend
// receives the current clip (B) first and then the one being turned off (A). With the two
// swapped the blend comes out backwards and no test saw it.
static void test_apply_skinned_frame_passes_pose_b_then_a()
{
    Scene scene("Test");
    auto a = std::make_shared<AnimatorComponent>();
    makeCrossfadeInFlight(*a);
    // Resolved and DISTINCT clips: without this the two would fall to the default 0 and
    // the test would pass with the arguments swapped.
    a->statesMutable()[0].clipIndex = 3;
    a->statesMutable()[1].clipIndex = 7;
    // Root mode other than 0 (the default of the double and of the backend): if the
    // host did not pass it, this test would not see it.
    for (auto& s : a->statesMutable()) s.rootMotion = AnimatorComponent::RootMotion::Lock;
    GameObject* go = makeSkinnedGameObject(scene, a);

    SkinnedRendererDoble r;
    applySkinnedFrame(*go, r, 0.016f, /*evaluateTransitions=*/true);

    // From row 13 the whole AnimationPose travels: the same one pose() gives.
    const AnimationPose esperada = a->pose();
    CHECK(r.poseRecibida);
    CHECK(esperada.count == 2);
    CHECK(r.pose.count == esperada.count);
    for (int k = 0; k < esperada.count; k++)
    {
        CHECK(r.pose.samples[k].clip == esperada.samples[k].clip);
        CHECK(nearlyEqual(r.pose.samples[k].time, esperada.samples[k].time));
        CHECK(nearlyEqual(r.pose.samples[k].weight, esperada.samples[k].weight));
    }
    CHECK(r.pose.samples[0].clip != r.pose.samples[1].clip);
    CHECK(!nearlyEqual(r.pose.samples[0].time, r.pose.samples[1].time));
    CHECK(r.pose.rootMotionMode == 1u);
    CHECK(nearlyEqual(r.pose.layers[0].frozenWeight, esperada.layers[0].frozenWeight));
}

// An interrupted fade asks to freeze the on-screen pose ONCE: the host
// delivers it with freezeNow and consumes it right away. If it did not consume it, the
// backend would freeze every frame and the output pose would stay stuck.
static void test_apply_skinned_frame_delivers_freeze_once()
{
    Scene scene("Test");
    auto a = std::make_shared<AnimatorComponent>(makeTwoBlendStates());
    a->setTrigger("go");
    a->update(0.016f, true);
    a->update(0.5f, true);                          // Loco -> Otro fade in flight
    a->setTrigger("back");
    GameObject* go = makeSkinnedGameObject(scene, a);

    SkinnedRendererDoble r;
    applySkinnedFrame(*go, r, 0.016f, /*evaluateTransitions=*/true);   // interrupts
    CHECK(r.pose.layers[0].freezeNow);
    CHECK(r.pose.layers[0].frozenWeight > 0.0f);
    CHECK(!a->pose().layers[0].freezeNow);

    applySkinnedFrame(*go, r, 0.016f, /*evaluateTransitions=*/true);
    CHECK(!r.pose.layers[0].freezeNow);
    CHECK(r.pose.layers[0].frozenWeight > 0.0f);
}

// Without an Animator the clock is run by the backend, as before the component
// existed. The two paths do not step on each other.
static void test_apply_skinned_frame_without_animator_advances_backend_clock()
{
    Scene scene("Test");
    GameObject* go = makeSkinnedGameObject(scene, nullptr);

    SkinnedRendererDoble r;
    applySkinnedFrame(*go, r, 0.033f, /*evaluateTransitions=*/true);

    CHECK(r.indiceDe("updateAnimation") >= 0);
    CHECK(r.indiceDe("blend") < 0);
    CHECK(nearlyEqual(r.dtSinAnimator, 0.033f));
    // Here is where the order DOES matter: Vulkan's updateAnimation freezes
    // the clock of a hidden mesh, so it has to see the visible flag already.
    // The order test with an Animator looks at a branch where the order has no
    // effect in any backend.
    CHECK(r.indiceDe("visible") < r.indiceDe("updateAnimation"));
}

// evaluateTransitions is the only thing that tells Edit from Play on this path:
// in Edit time advances but the graph does not move.
static void test_apply_skinned_frame_edit_mode_does_not_move_the_graph()
{
    Scene scene("Test");
    auto a = makeArmedTriggerGraph();
    GameObject* go = makeSkinnedGameObject(scene, a);

    SkinnedRendererDoble r;
    applySkinnedFrame(*go, r, 0.016f, /*evaluateTransitions=*/false);
    CHECK(a->currentStateName() == "A");

    applySkinnedFrame(*go, r, 0.016f, /*evaluateTransitions=*/true);
    CHECK(a->currentStateName() == "B");
}

// An object that is not registered in the backend has no index: NOTHING is
// sent to the backend. Its graph does run: since the property clips
// (C14) the Animator animates objects with no skeleton mesh, and before it left through
// this same door without doing anything.
static void test_apply_skinned_frame_ignores_unregistered_object()
{
    Scene scene("Test");
    auto a = makeArmedTriggerGraph();
    GameObject* go = makeSkinnedGameObject(scene, a);
    go->skinnedRenderIndex = -1;

    SkinnedRendererDoble r;
    applySkinnedFrame(*go, r, 0.016f, /*evaluateTransitions=*/true);

    // To the backend, nothing: no pose, no IK, no transform, no SSR.
    CHECK(r.orden.empty());
    // But the graph does advance: the armed trigger fires its transition.
    CHECK(a->currentStateName() == "B");
}

// Transform and SSR go to the backend with the rest of the block, and a disabled SSR
// sends a 0 instead of the intensity (the backend does not know the flag).
static void test_apply_skinned_frame_forwards_transform_and_ssr()
{
    Scene scene("Test");
    GameObject* go = makeSkinnedGameObject(scene, nullptr);
    glm::mat4 m(1.0f);
    m[3] = glm::vec4(1.0f, 2.0f, 3.0f, 1.0f);
    go->worldTransform = m;

    SkinnedRendererDoble r;
    applySkinnedFrame(*go, r, 0.016f, /*evaluateTransitions=*/true);
    CHECK(r.transform[3] == glm::vec4(1.0f, 2.0f, 3.0f, 1.0f));
    CHECK(nearlyEqual(r.ssr, 0.4f));

    go->ssrEnabled = false;
    SkinnedRendererDoble apagado;
    applySkinnedFrame(*go, apagado, 0.016f, /*evaluateTransitions=*/true);
    CHECK(nearlyEqual(apagado.ssr, 0.0f));
}

// ---- Exit time ----

// A -> B with exit time, and optionally a bool condition "ok". Playhead at
// the entry WITHOUT evaluating transitions: with exitTime 0 an update(0, true) would already
// fire. makeTimedState: 100 ticks at 10 ticks/s, so one second of
// update raises the normalized time by 0.1 and one lap is 10 s.
static AnimatorComponent makeExitTimeGraph(float exitTime, bool conCondicion)
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addParameter("ok", AnimatorComponent::ParamType::Bool);
    AnimatorComponent::Transition t;
    t.fromState   = 0;
    t.toState     = 1;
    t.hasExitTime = true;
    t.exitTime    = exitTime;
    if (conCondicion)
    {
        AnimatorComponent::Condition c;
        c.type = AnimatorComponent::ConditionType::Bool; c.paramName = "ok"; c.expected = true;
        t.conditions.push_back(c);
    }
    a.addTransition(t);
    a.update(0.0f, false);
    return a;
}

// Exit time < 1 without conditions: fires on the frame that CROSSES 0.9, not before.
static void test_exit_time_below_one_fires_when_crossed()
{
    AnimatorComponent a = makeExitTimeGraph(0.9f, false);
    a.update(8.5f, true);                 // N = 0.85
    CHECK(a.currentStateName() == "A");
    a.update(1.0f, true);                 // N = 0.95: crosses 0.9
    CHECK(a.currentStateName() == "B");
}

// With a condition, it is looked at ONLY on the crossing frame: if it is not met there,
// it waits for the next lap even if it is met right afterwards.
static void test_exit_time_below_one_checks_conditions_only_at_the_crossing()
{
    AnimatorComponent a = makeExitTimeGraph(0.9f, true);
    a.update(9.5f, true);                 // N = 0.95: crosses with ok == false
    CHECK(a.currentStateName() == "A");
    a.setBool("ok", true);
    a.update(0.1f, true);                 // N = 0.96: the 0.9 of this lap has already passed
    CHECK(a.currentStateName() == "A");
    a.update(8.5f, true);                 // N = 1.81: it does not reach 1.9 yet
    CHECK(a.currentStateName() == "A");
    a.update(1.0f, true);                 // N = 1.91: crosses the 0.9 of the second lap
    CHECK(a.currentStateName() == "B");
}

// A large dt that wraps around passing through 0.9 is also a crossing. A
// rule that only looked at the lap's phase (0.95 -> 0.92) would not see it.
static void test_exit_time_below_one_fires_when_a_big_dt_wraps_past_it()
{
    AnimatorComponent a = makeExitTimeGraph(0.9f, true);
    a.update(9.5f, true);                 // N = 0.95, crosses with ok == false
    CHECK(a.currentStateName() == "A");
    a.setBool("ok", true);
    a.update(9.7f, true);                 // N = 1.92 all at once: passes through 1.9
    CHECK(a.currentStateName() == "B");
}

// Exit time >= 1 counts laps: 2.5 does not fire with 2 laps.
static void test_exit_time_above_one_counts_loops()
{
    AnimatorComponent a = makeExitTimeGraph(2.5f, false);
    a.update(20.0f, true);                // N = 2.0
    CHECK(a.currentStateName() == "A");
    a.update(5.0f, true);                 // N = 2.5
    CHECK(a.currentStateName() == "B");
}

// Without conditions and without exit time it never fires (as always); with exit
// time, it does.
static void test_transition_without_conditions_needs_exit_time()
{
    AnimatorComponent sinExit;
    sinExit.addState(makeTimedState("A"));
    sinExit.addState(makeTimedState("B"));
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    sinExit.addTransition(t);
    sinExit.update(0.0f, false);
    sinExit.update(50.0f, true);
    CHECK(sinExit.currentStateName() == "A");

    AnimatorComponent conExit = makeExitTimeGraph(0.5f, false);
    conExit.update(6.0f, true);
    CHECK(conExit.currentStateName() == "B");
}

// A clip of duration 0 (or unresolved) has no normalized time: the exit
// time counts as reached on the first update.
static void test_exit_time_on_zero_duration_state_is_reached_at_once()
{
    AnimatorComponent a;
    AnimatorComponent::State cero = makeTimedState("A");
    cero.duration = 0.0f;
    a.addState(cero);
    a.addState(makeTimedState("B"));
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.hasExitTime = true; t.exitTime = 0.9f;
    a.addTransition(t);
    a.update(0.0f, false);
    a.update(0.016f, true);
    CHECK(a.currentStateName() == "B");
}

// The accumulated clock restarts on entering a state through a transition:
// 1.5 laps count from when B was entered, not from when the graph started.
static void test_exit_time_clock_restarts_when_entering_a_state()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addState(makeTimedState("C"));
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition ab;
    ab.fromState = 0; ab.toState = 1;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "go";
    ab.conditions.push_back(c);
    a.addTransition(ab);
    AnimatorComponent::Transition bc;
    bc.fromState = 1; bc.toState = 2; bc.hasExitTime = true; bc.exitTime = 1.5f;
    a.addTransition(bc);
    a.update(0.0f, false);

    a.update(12.0f, true);                // 1.2 laps in A
    a.setTrigger("go");
    a.update(0.0f, true);                 // enters B
    CHECK(a.currentStateName() == "B");
    a.update(14.0f, true);                // 1.4 in B (2.6 if the clock did not restart)
    CHECK(a.currentStateName() == "B");
    a.update(1.0f, true);                 // 1.5
    CHECK(a.currentStateName() == "C");
}

// The same through the other path that restarts the playhead: setEntryState goes
// through resetPlayback.
static void test_exit_time_clock_restarts_on_reset_playback()
{
    AnimatorComponent a = makeExitTimeGraph(1.5f, false);
    a.update(12.0f, true);                // 1.2 laps in A
    CHECK(a.currentStateName() == "A");
    a.setEntryState(0);                   // resetPlayback
    a.update(5.0f, true);                 // 0.5 (1.7 if it did not restart)
    CHECK(a.currentStateName() == "A");
}

// ---- Any State ----

static AnimatorComponent::Condition triggerCond(const char* nombre)
{
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger;
    c.paramName = nombre;
    return c;
}

// An Any State transition fires from any state.
static void test_any_state_fires_from_every_state()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addState(makeTimedState("C"));
    a.addParameter("hit", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = AnimatorComponent::kAnyState; t.toState = 2;
    t.conditions.push_back(triggerCond("hit"));
    a.addTransition(t);

    a.update(0.0f, false);                // en A
    a.setTrigger("hit");
    a.update(0.016f, true);
    CHECK(a.currentStateName() == "C");

    a.setEntryState(1);                   // now in B
    a.setTrigger("hit");
    a.update(0.016f, true);
    CHECK(a.currentStateName() == "C");
}

// Any State goes before the current state's transitions, even if the state's
// one is declared first.
static void test_any_state_wins_over_the_current_state_transition()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addState(makeTimedState("C"));
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition propia;
    propia.fromState = 0; propia.toState = 1;
    propia.conditions.push_back(triggerCond("go"));
    a.addTransition(propia);
    AnimatorComponent::Transition any;
    any.fromState = AnimatorComponent::kAnyState; any.toState = 2;
    any.conditions.push_back(triggerCond("go"));
    a.addTransition(any);

    a.update(0.0f, false);
    a.setTrigger("go");
    a.update(0.016f, true);
    CHECK(a.currentStateName() == "C");
}

// With canTransitionToSelf off, Any State does not re-enter the current state: if
// it did with a bool, the state would restart every frame.
static void test_any_state_does_not_reenter_the_current_state_by_default()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addParameter("b", AnimatorComponent::ParamType::Bool);
    AnimatorComponent::Transition t;
    t.fromState = AnimatorComponent::kAnyState; t.toState = 0;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Bool; c.paramName = "b"; c.expected = true;
    t.conditions.push_back(c);
    a.addTransition(t);

    a.update(0.0f, false);
    a.setBool("b", true);
    a.update(1.0f, true);                 // if it re-entered, animTime would go back to 0
    CHECK(a.currentStateName() == "A");
    CHECK(nearlyEqual(a.animTime(), 10.0f));
}

// With the flag on, a trigger re-enters the same state from the beginning.
static void test_any_state_reenters_with_can_transition_to_self()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addParameter("again", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = AnimatorComponent::kAnyState; t.toState = 0;
    t.canTransitionToSelf = true;
    t.conditions.push_back(triggerCond("again"));
    a.addTransition(t);

    a.update(0.0f, false);
    a.update(1.0f, true);
    CHECK(nearlyEqual(a.animTime(), 10.0f));
    a.setTrigger("again");
    a.update(0.5f, true);                 // re-enters: the clock goes back to 0
    CHECK(a.currentStateName() == "A");
    CHECK(nearlyEqual(a.animTime(), 0.0f));
}

// removeState with Any State: the one that pointed at the deleted state goes away, the other
// is reindexed. The sentinel is not touched.
static void test_remove_state_drops_and_reindexes_any_state_transitions()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addState(makeTimedState("C"));
    AnimatorComponent::Transition haciaB;
    haciaB.fromState = AnimatorComponent::kAnyState; haciaB.toState = 1;
    a.addTransition(haciaB);
    AnimatorComponent::Transition haciaC;
    haciaC.fromState = AnimatorComponent::kAnyState; haciaC.toState = 2;
    a.addTransition(haciaC);

    a.removeState(1);

    CHECK(a.transitions().size() == 1u);
    if (a.transitions().size() == 1u)
    {
        CHECK(a.transitions()[0].fromState == AnimatorComponent::kAnyState);
        CHECK(a.transitions()[0].toState == 1);
    }
}

// ---- Exit time and Any State in the .scene ----

static void test_exit_time_and_any_state_survive_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;

    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s0; s0.name = "Idle"; s0.clipName = "ClipIdle";
    AnimatorComponent::State s1; s1.name = "Hit";  s1.clipName = "ClipHit";
    a->addState(s0);
    a->addState(s1);
    a->addParameter("hit", AnimatorComponent::ParamType::Trigger);

    AnimatorComponent::Transition vuelta;
    vuelta.fromState = 1; vuelta.toState = 0;
    vuelta.hasExitTime = true; vuelta.exitTime = 0.75f;
    a->addTransition(vuelta);

    AnimatorComponent::Transition any;
    any.fromState = AnimatorComponent::kAnyState; any.toState = 1;
    any.canTransitionToSelf = true;
    any.conditions.push_back({ AnimatorComponent::ConditionType::Trigger, "hit" });
    a->addTransition(any);
    a->setAnyStateEditorPos(glm::vec2(-300.0f, 55.0f));

    go->setAnimator(a);
    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;

    const AnimatorComponent& b = *found->getAnimator();
    CHECK(b.transitions().size() == 2u);
    if (b.transitions().size() != 2u) return;
    CHECK(b.transitions()[0].hasExitTime);
    CHECK(nearlyEqual(b.transitions()[0].exitTime, 0.75f));
    CHECK(b.transitions()[1].fromState == AnimatorComponent::kAnyState);
    CHECK(b.transitions()[1].toState == 1);
    CHECK(b.transitions()[1].canTransitionToSelf);
    CHECK(b.anyStateEditorPos() == glm::vec2(-300.0f, 55.0f));
}

// Backward compatibility: a scene without the new fields loads with their
// defaults. It is saved with values DIFFERENT from the defaults and then the
// fields are removed, so that a broken reader shows.
static void test_scene_without_exit_time_fields_loads(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s0; s0.name = "Idle";
    AnimatorComponent::State s1; s1.name = "Run";
    a->addState(s0);
    a->addState(s1);
    AnimatorComponent::Transition t; t.fromState = 0; t.toState = 1;
    a->addTransition(t);
    a->setAnyStateEditorPos(glm::vec2(123.0f, 456.0f));
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    auto& anim = j["root"]["children"][0]["animator"];
    anim.erase("anyStatePos");
    for (auto& tr : anim["transitions"])
    {
        tr.erase("hasExitTime");
        tr.erase("exitTime");
        tr.erase("canTransitionToSelf");
    }

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const AnimatorComponent& b = *found->getAnimator();
    CHECK(b.transitions().size() == 1u);
    if (b.transitions().size() != 1u) return;
    CHECK(!b.transitions()[0].hasExitTime);
    CHECK(nearlyEqual(b.transitions()[0].exitTime, 1.0f));
    CHECK(!b.transitions()[0].canTransitionToSelf);
    CHECK(b.anyStateEditorPos() != glm::vec2(123.0f, 456.0f));
    CHECK(b.anyStateEditorPos() == AnimatorComponent().anyStateEditorPos());
}

// An Any State with an out-of-range destination is discarded with a warning, like a
// normal one; a good Any State survives; from = -1 is still invalid.
static void test_any_state_transition_with_bad_target_is_dropped(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s0; s0.name = "Idle";
    AnimatorComponent::State s1; s1.name = "Run";
    a->addState(s0);
    a->addState(s1);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    auto& trs = j["root"]["children"][0]["animator"]["transitions"];
    trs.push_back({ {"from", -2}, {"to", 1}, {"duration", 0.0f} });   // good
    trs.push_back({ {"from", -2}, {"to", 7}, {"duration", 0.0f} });   // destination that does not exist
    trs.push_back({ {"from", -1}, {"to", 1}, {"duration", 0.0f} });   // origin not set

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;

    const auto& cargadas = found->getAnimator()->transitions();
    CHECK(cargadas.size() == 1u);
    if (cargadas.size() == 1u)
    {
        CHECK(cargadas[0].fromState == AnimatorComponent::kAnyState);
        CHECK(cargadas[0].toState   == 1);
    }
    int avisos = 0;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("transition") != std::string::npos) ++avisos;
    CHECK(avisos == 2);
}

// A negative exitTime in the .scene is clamped to 0 and a warning is issued.
static void test_negative_exit_time_is_clamped_with_warning(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s0; s0.name = "Idle";
    AnimatorComponent::State s1; s1.name = "Run";
    a->addState(s0);
    a->addState(s1);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["animator"]["transitions"].push_back(
        { {"from", 0}, {"to", 1}, {"duration", 0.0f}, {"hasExitTime", true}, {"exitTime", -3.0f} });

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& cargadas = found->getAnimator()->transitions();
    CHECK(cargadas.size() == 1u);
    if (cargadas.size() == 1u) CHECK(nearlyEqual(cargadas[0].exitTime, 0.0f));

    bool avisado = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("exitTime") != std::string::npos) avisado = true;
    CHECK(avisado);
}

// ---- Bounded clip index (A12) ----

// How many clip blocks the SSBO carries: packSkinnedClips packs at least
// one even if the mesh brings no animations.
static void test_clip_count_is_at_least_one()
{
    SkinnedMesh sinClips;
    CHECK(skinnedClipCount(sinClips) == 1u);
    SkinnedMesh conDos = makeTwoClipFixture();
    CHECK(skinnedClipCount(conDos) == 2u);
}

// An out-of-range index falls to clip 0 instead of pointing outside the SSBO. The
// -1 that arrives from an unresolved int shows up as 0xFFFFFFFF after the cast.
static void test_clamp_clip_index_falls_back_to_zero()
{
    CHECK(clampClipIndex(1u, 2u) == 1u);
    CHECK(clampClipIndex(2u, 2u) == 0u);
    CHECK(clampClipIndex(7u, 2u) == 0u);
    CHECK(clampClipIndex((uint32_t)-1, 2u) == 0u);
}

// ---- Code API: play, crossFade, resetTrigger, normalizedTime ----

// A, B and C of 10 s per lap, playhead at A.
static AnimatorComponent makeThreeStates()
{
    AnimatorComponent a;
    a.addState(makeTimedState("A"));
    a.addState(makeTimedState("B"));
    a.addState(makeTimedState("C"));
    a.update(0.0f, false);
    return a;
}

// A name that does not exist moves nothing and says so by returning false.
static void test_play_and_crossfade_reject_unknown_state()
{
    AnimatorComponent a = makeThreeStates();
    a.update(1.0f, false);
    CHECK(!a.play("NoExiste"));
    CHECK(!a.crossFade("NoExiste", 0.5f));
    CHECK(a.currentStateName() == "A");
    CHECK(nearlyEqual(a.animTime(), 10.0f));
    CHECK(!a.blending());
}

// play enters right away, with time at 0, and cuts any blend there was.
static void test_play_enters_at_once_and_cuts_the_blend()
{
    AnimatorComponent a = makeThreeStates();
    CHECK(a.crossFade("B", 1.0f));
    CHECK(a.blending());
    a.update(0.2f, false);
    CHECK(a.play("C"));
    CHECK(a.currentStateName() == "C");
    CHECK(nearlyEqual(a.animTime(), 0.0f));
    CHECK(!a.blending());
}

// crossFade: the current one starts to fade out, with the requested duration.
static void test_crossfade_starts_a_blend_from_the_current_state()
{
    AnimatorComponent a = makeThreeStates();
    a.update(1.0f, false);
    CHECK(a.crossFade("B", 0.5f));
    CHECK(a.currentStateName() == "B");
    CHECK(a.previousStateName() == "A");
    CHECK(a.blending());
    a.update(0.25f, false);
    CHECK(nearlyEqual(a.blendWeight(), 0.5f));
}

// With 0 seconds it is a hard cut.
static void test_crossfade_with_zero_seconds_is_a_cut()
{
    AnimatorComponent a = makeThreeStates();
    CHECK(a.crossFade("B", 0.0f));
    CHECK(a.currentStateName() == "B");
    CHECK(!a.blending());
}

// Toward the current state it restarts: an explicit call is intent.
static void test_play_to_the_current_state_restarts_it()
{
    AnimatorComponent a = makeThreeStates();
    a.update(1.0f, false);
    CHECK(a.play("A"));
    CHECK(nearlyEqual(a.animTime(), 0.0f));
}

// resetTrigger disarms: the transition that was waiting for it no longer fires.
static void test_reset_trigger_disarms_it()
{
    AnimatorComponent a = makeThreeStates();
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "go";
    t.conditions.push_back(c);
    a.addTransition(t);

    a.setTrigger("go");
    a.resetTrigger("go");
    a.update(0.016f, true);
    CHECK(a.currentStateName() == "A");
}

// normalizedTime accumulates laps (it exceeds 1 in a loop) and goes back to 0 on entering.
static void test_normalized_time_accumulates_and_restarts()
{
    AnimatorComponent a = makeThreeStates();
    a.update(25.0f, false);                // 2.5 laps
    CHECK(nearlyEqual(a.normalizedTime(), 2.5f));
    a.play("B");
    CHECK(nearlyEqual(a.normalizedTime(), 0.0f));
}

// ---- Per-state and global speed ----

// speed 2 doubles the clip's time; a parameter at 0.5 leaves it at x1.
static void test_state_speed_and_param_multiply_the_clock()
{
    AnimatorComponent a;
    AnimatorComponent::State s = makeTimedState("A");
    s.speed = 2.0f;
    a.addState(s);
    a.update(0.0f, false);
    a.update(1.0f, false);
    CHECK(nearlyEqual(a.animTime(), 20.0f));

    a.addParameter("mult", AnimatorComponent::ParamType::Float);
    a.statesMutable()[0].speedParam = "mult";
    a.setFloat("mult", 0.5f);
    a.play("A");
    a.update(1.0f, false);
    CHECK(nearlyEqual(a.animTime(), 10.0f));
}

// Speed 0 freezes: it never finishes nor crosses an exit time.
static void test_zero_speed_never_finishes_nor_reaches_exit_time()
{
    AnimatorComponent a = makeExitTimeGraph(0.5f, false);
    a.statesMutable()[0].speed = 0.0f;
    a.statesMutable()[0].loop  = false;
    a.update(100.0f, true);
    CHECK(a.currentStateName() == "A");
    CHECK(!a.finished());
    CHECK(nearlyEqual(a.animTime(), 0.0f));
}

// A negative (in the state or in the parameter) freezes instead of going backwards.
static void test_negative_speed_freezes_instead_of_reversing()
{
    AnimatorComponent a;
    AnimatorComponent::State s = makeTimedState("A");
    s.speed = -1.0f;
    a.addState(s);
    a.update(0.0f, false);
    a.update(1.0f, false);
    CHECK(nearlyEqual(a.animTime(), 0.0f));
}

// During a cross-fade, the state being turned off advances with ITS speed.
static void test_fading_state_uses_its_own_speed()
{
    AnimatorComponent a = makeThreeStates();
    a.statesMutable()[0].speed = 3.0f;       // A
    a.crossFade("B", 10.0f);
    a.update(1.0f, false);
    CHECK(nearlyEqual(a.previousAnimTime(), 30.0f));
    CHECK(nearlyEqual(a.animTime(), 10.0f));
}

// The global speed scales the whole dt, blend included; negative -> 0.
static void test_global_speed_scales_clock_and_blend()
{
    AnimatorComponent a = makeThreeStates();
    a.setSpeed(2.0f);
    a.crossFade("B", 1.0f);
    a.update(0.25f, false);                  // 0.5 s effective
    CHECK(nearlyEqual(a.animTime(), 5.0f));
    CHECK(nearlyEqual(a.blendWeight(), 0.5f));

    a.setSpeed(-3.0f);
    CHECK(nearlyEqual(a.speed(), 0.0f));
}

static void test_state_speed_survives_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s0; s0.name = "Walk"; s0.speed = 1.75f; s0.speedParam = "mult";
    AnimatorComponent::State s1; s1.name = "Idle";
    a->addState(s0);
    a->addState(s1);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    CHECK(!j["root"]["children"][0]["animator"]["states"][1].contains("speed"));   // x1 is not written

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& st = found->getAnimator()->states();
    CHECK(st.size() == 2u);
    if (st.size() != 2u) return;
    CHECK(nearlyEqual(st[0].speed, 1.75f));
    CHECK(st[0].speedParam == "mult");
    CHECK(nearlyEqual(st[1].speed, 1.0f));
    CHECK(st[1].speedParam.empty());
}

// A negative speed in the .scene is clamped to 0 and a warning is issued.
static void test_negative_state_speed_is_clamped_on_load(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s0; s0.name = "Walk";
    a->addState(s0);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["animator"]["states"][0]["speed"] = -2.0f;

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    CHECK(nearlyEqual(found->getAnimator()->states()[0].speed, 0.0f));
    bool avisado = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("speed") != std::string::npos) avisado = true;
    CHECK(avisado);
}

// ---- Shared mesh (Appendix B) ----

// Cloning an object with an added animation FBX does NOT duplicate its clips: the
// clone brings the source configuration from the JSON, and re-applying it on a
// mesh that already has it would duplicate them (and reread the FBX).
static void test_clone_with_animation_source_keeps_clip_count(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto sm = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    std::vector<std::string> w;
    CHECK(addAnimationSource(*sm, "assets/modelAnimation.fbx", w));
    const size_t clips = sm->animationClips.size();
    go->setMesh(sm);

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr && clone->isSkinned());
    if (!clone || !clone->getSkinnedMesh()) return;
    CHECK(clone->getSkinnedMesh()->animationClips.size() == clips);
    CHECK(clone->getSkinnedMesh()->animationSources.size() == 2u);
}

// editMesh on a unique mesh does not copy; on a shared one, it does, and the other
// owner does not see the change.
static void test_edit_mesh_copies_only_when_shared()
{
    Scene scene("Test");
    GameObject* a = scene.addGameObject("A");
    GameObject* b = scene.addGameObject("B");
    auto m = std::make_shared<SkinnedMesh>(makeTwoClipFixture());
    a->setMesh(m);
    m.reset();                                   // A is the only owner
    const Mesh* antes = a->getMesh().get();
    CHECK(a->editMesh() == antes);               // unique: no copy

    b->setMesh(std::const_pointer_cast<Mesh>(a->getMesh()));   // now shared
    SkinnedMesh* editada = b->editSkinnedMesh();
    CHECK(editada != nullptr);
    if (!editada) return;
    CHECK(editada != a->getSkinnedMesh());       // B copied
    editada->material.texturePath = "solo_b.png";
    editada->animationClips.pop_back();
    CHECK(a->getMesh()->material.texturePath != "solo_b.png");
    CHECK(a->getSkinnedMesh()->animationClips.size() == 2u);
    CHECK(dynamic_cast<const SkinnedMesh*>(b->getMesh().get()) != nullptr);   // keeps the type
}

// An override equal to the value the mesh already has does not force the copy.
static void test_equal_material_override_does_not_copy()
{
    Scene scene("Test");
    GameObject* a = scene.addGameObject("A");
    GameObject* b = scene.addGameObject("B");
    auto m = std::make_shared<SkinnedMesh>(makeTwoClipFixture());
    m->material.texturePath = "igual.png";
    a->setMesh(m);
    b->setMesh(m);
    m.reset();

    MaterialOverride ov; ov.index = 0; ov.albedo = "igual.png";
    b->materialOverrides.push_back(ov);
    applyMaterialOverrides(*b);
    CHECK(b->getMesh().get() == a->getMesh().get());

    b->materialOverrides[0].albedo = "distinto.png";
    applyMaterialOverrides(*b);
    CHECK(b->getMesh().get() != a->getMesh().get());
    CHECK(b->getMesh()->material.texturePath == "distinto.png");
    CHECK(a->getMesh()->material.texturePath == "igual.png");
}

// The clone shares the mesh instead of copying it.
static void test_clone_shares_the_mesh(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    go->setMesh(std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx")));
    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    CHECK(clone->getMesh().get() == go->getMesh().get());
}

// Same source configuration: it matches. An extra source or a renamed
// clip: it does not.
static void test_mesh_matches_animation_config()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    nlohmann::json fuentes = nlohmann::json::array();
    for (const auto& s : m.animationSources)
        fuentes.push_back({ {"path", s.path}, {"builtin", s.builtin}, {"clips", s.clipNames} });
    CHECK(meshMatchesAnimationConfig(m, fuentes));

    nlohmann::json mas = fuentes;
    mas.push_back({ {"path", "otro.fbx"}, {"builtin", false}, {"clips", nlohmann::json::array({"x"})} });
    CHECK(!meshMatchesAnimationConfig(m, mas));

    nlohmann::json renombrada = fuentes;
    CHECK(!renombrada.empty() && !renombrada[0]["clips"].empty());
    if (!renombrada.empty() && !renombrada[0]["clips"].empty())
    {
        renombrada[0]["clips"][0] = "renombrado";
        CHECK(!meshMatchesAnimationConfig(m, renombrada));
    }
}

// Undo of Delete with the meshes alive: it shares and does not read disk.
static void test_insert_from_json_reuses_preloaded_mesh(PhysicsManager& pm, AudioManager& am)
{
    const std::filesystem::path temporal = std::filesystem::temp_directory_path() / "dt_undo_sin_disco.fbx";
    std::error_code ec;
    std::filesystem::remove(temporal, ec);
    std::filesystem::copy_file("assets/modelAnimation.fbx", temporal, ec);
    CHECK(!ec);
    if (ec) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    go->setMesh(std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned(temporal.string())));
    nlohmann::json snap = scene.subtreeToJson(go);
    const PreloadedMeshCache mallas = Scene::collectMeshes(go);
    const Mesh* original = go->getMesh().get();
    scene.removeGameObject(go);
    std::filesystem::remove(temporal, ec);              // no file: reading disk would fail

    GameObject* r = scene.insertFromJson(snap, nullptr, 0, pm, am, &mallas);
    CHECK(r != nullptr);
    if (!r) return;
    CHECK(r->isSkinned());
    CHECK(r->getMesh().get() == original);
}

// ── Animation events ────────────────────────────────────────────────────────
//
// Walk: 40 ticks at 20 tps = 2 s per cycle, looping, with "paso" in the middle.
static AnimatorComponent makeEventGraph(float eventTime = 0.5f, bool loop = true)
{
    AnimatorComponent a;
    AnimatorComponent::State s;
    s.name = "Walk"; s.clipName = "Walk"; s.clipIndex = 0;
    s.duration = 40.0f; s.ticksPerSecond = 20.0f; s.loop = loop;
    s.events.push_back({ "paso", eventTime });
    a.addState(s);
    a.setEntryState(0);
    a.reset();
    return a;
}

static int contar(const AnimatorComponent& a, const char* nombre)
{
    int n = 0;
    for (const auto& e : a.firedEvents()) if (e == nombre) n++;
    return n;
}

// Once per cycle with a frame dt, and never two in the same update.
static void test_event_fires_once_per_cycle()
{
    AnimatorComponent a = makeEventGraph();
    int total = 0;
    bool dobleEnUno = false;
    for (int i = 0; i < 250; i++)                  // 4 s = 2 cycles
    {
        a.update(0.016f, true);
        const int n = contar(a, "paso");
        total += n;
        if (n > 1) dobleEnUno = true;
    }
    CHECK(total == 2);
    CHECK(!dobleEnUno);
}

// A dt that crosses the loop wrap fires the event at the start of the cycle.
static void test_event_fires_across_loop_wrap()
{
    AnimatorComponent a = makeEventGraph(0.02f);   // 0,8 ticks
    a.update(1.9f, true);                          // 38 ticks
    a.update(0.2f, true);                          // 38 -> 42: crosses 40,8
    CHECK(contar(a, "paso") == 1);
}

// A dt that skips two cycles fires twice.
static void test_event_fires_per_skipped_cycle()
{
    AnimatorComponent a = makeEventGraph();
    a.update(4.2f, true);                          // 84 ticks: crosses 20 and 60
    CHECK(contar(a, "paso") == 2);
}

// Without loop, only once even if the clock keeps going.
static void test_event_non_loop_fires_once()
{
    AnimatorComponent a = makeEventGraph(0.5f, /*loop=*/false);
    int total = 0;
    for (int i = 0; i < 10; i++) { a.update(1.0f, true); total += contar(a, "paso"); }
    CHECK(total == 1);
}

// Event at 0: fires on the first update with dt > 0, not on one with dt 0.
static void test_event_at_zero_fires_on_entry()
{
    AnimatorComponent a = makeEventGraph(0.0f);
    a.update(0.0f, true);
    CHECK(contar(a, "paso") == 0);
    a.update(0.1f, true);
    CHECK(contar(a, "paso") == 1);
}

// In Edit (without evaluating transitions) nothing fires.
static void test_event_not_fired_in_edit_mode()
{
    AnimatorComponent a = makeEventGraph();
    a.update(4.2f, false);
    CHECK(a.firedEvents().empty());
}

// During a cross-fade only the destination fires; the one being turned off does not.
static void test_event_fading_out_state_is_silent()
{
    AnimatorComponent a = makeEventGraph(0.1f);    // 4 ticks
    AnimatorComponent::State b;
    b.name = "Idle"; b.clipName = "Idle"; b.clipIndex = 1;
    b.duration = 40.0f; b.ticksPerSecond = 20.0f; b.loop = true;
    a.addState(b);
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 2.0f;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "go";
    t.conditions.push_back(c);
    a.addTransition(t);

    a.setTrigger("go");
    a.update(0.016f, true);                        // exits to Idle with a 2 s fade
    CHECK(a.currentStateName() == "Idle");
    CHECK(a.blending());
    a.update(0.9f, true);                          // Walk continues: 0,32 -> 18,32, crosses 4
    CHECK(a.blending());
    CHECK(a.firedEvents().empty());
}

// firedEvents only keeps what the last update produced.
static void test_fired_events_cleared_each_update()
{
    AnimatorComponent a = makeEventGraph();
    a.update(1.1f, true);                          // crosses 20
    CHECK(contar(a, "paso") == 1);
    a.update(0.01f, true);
    CHECK(a.firedEvents().empty());
}

// Round trip of events; an out-of-range time is clamped with a warning.
static void test_events_scene_round_trip(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>(makeEventGraph(0.25f));
    a->statesMutable()[0].events.push_back({ "golpe", 0.75f });
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    bool tocado = false;
    for (auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        node["animator"]["states"][0]["events"][1]["time"] = 3.0f;
        tocado = true;
    }
    CHECK(tocado);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool aviso = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("time outside [0,1]") != std::string::npos) aviso = true;
    CHECK(aviso);
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& ev = found->getAnimator()->states()[0].events;
    CHECK(ev.size() == 2u);
    if (ev.size() != 2u) return;
    CHECK(ev[0].name == "paso" && nearlyEqual(ev[0].time, 0.25f));
    CHECK(ev[1].name == "golpe" && nearlyEqual(ev[1].time, 1.0f));
}

// The old bool lockRootMotion loads as Lock; "apply" goes there and back.
static void test_root_motion_mode_migration(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>();
    AnimatorComponent::State s;
    s.name = "Viejo"; s.clipName = "Run"; a->addState(s);
    s.name = "Nuevo"; s.clipName = "Run"; s.rootMotion = AnimatorComponent::RootMotion::Apply; a->addState(s);
    go->setAnimator(a);

    nlohmann::json j = scene.toJson();
    bool tocado = false;
    for (auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        CHECK(node["animator"]["states"][1]["rootMotion"] == "apply");
        node["animator"]["states"][0]["lockRootMotion"] = true;
        tocado = true;
    }
    CHECK(tocado);

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& st = found->getAnimator()->states();
    CHECK(st.size() == 2u);
    if (st.size() != 2u) return;
    CHECK(st[0].rootMotion == AnimatorComponent::RootMotion::Lock);
    CHECK(st[1].rootMotion == AnimatorComponent::RootMotion::Apply);
}

// ── Root motion ─────────────────────────────────────────────────────────────
//
// Synthetic mesh: skeleton of one root bone and clips whose root advances linearly
// in X `avance` units per cycle of `dur` ticks (and at a height of 1, with
// Y increasing in clip 2 to check that it is discarded).
static SkinnedMesh makeRootMotionMesh()
{
    SkinnedMesh m;
    m.skeleton.names = { "Hips" };
    m.skeleton.parentIndex = { -1 };
    m.skeleton.inverseBindPose = { glm::mat4(1.0f) };
    m.skeleton.boneMap["Hips"] = 0;
    auto clip = [](const char* n, float dur, glm::vec3 fin) {
        AnimationClip c; c.name = n; c.duration = dur; c.ticksPerSecond = 20.0f;
        BoneChannel ch; ch.boneIndex = 0;
        ch.posKeys = { { 0.0f, glm::vec3(0.0f, 1.0f, 0.0f) }, { dur, fin } };
        c.channels.push_back(ch);
        return c;
    };
    m.animationClips = { clip("Walk", 40.0f, glm::vec3(10.0f, 1.0f, 0.0f)),
                         clip("Run",  80.0f, glm::vec3(30.0f, 1.0f, 0.0f)),
                         clip("Sube", 40.0f, glm::vec3(10.0f, 5.0f, 0.0f)) };
    return m;
}

static void test_root_displacement_counts_cycles()
{
    const SkinnedMesh m = makeRootMotionMesh();
    CHECK(nearlyEqual(rootDisplacement(m, 0, 40.0, 40.0, true).x, 10.0f));
    CHECK(nearlyEqual(rootDisplacement(m, 0, 100.0, 40.0, true).x, 25.0f));
    CHECK(nearlyEqual(rootDisplacement(m, 0, 100.0, 40.0, false).x, 10.0f));
    CHECK(nearlyEqual(rootDisplacement(m, 0, 0.0, 40.0, true).x, 0.0f));
}

static AnimatorComponent makeRootMotionGraph(int clip = 0, float dur = 40.0f)
{
    AnimatorComponent a;
    AnimatorComponent::State s;
    s.name = "Walk"; s.clipName = "Walk"; s.clipIndex = clip; s.duration = dur;
    s.ticksPerSecond = 20.0f; s.loop = true; s.rootMotion = AnimatorComponent::RootMotion::Apply;
    a.addState(s);
    a.setEntryState(0);
    a.reset();
    return a;
}

// An update that crosses the wrap gives the real advance, without a negative jump.
static void test_root_motion_delta_across_wrap()
{
    const SkinnedMesh m = makeRootMotionMesh();
    AnimatorComponent a = makeRootMotionGraph();
    a.update(1.9f, true);                          // 38 ticks
    a.update(0.2f, true);                          // 38 -> 42
    CHECK(nearlyEqual(rootMotionDelta(m, a).x, 1.0f));
}

// Only X and Z: the root's Y stays in the pose.
static void test_root_motion_delta_drops_y()
{
    const SkinnedMesh m = makeRootMotionMesh();
    AnimatorComponent a = makeRootMotionGraph(2);
    a.update(1.0f, true);                          // 20 ticks: Y sube 2
    const glm::vec3 d = rootMotionDelta(m, a);
    CHECK(nearlyEqual(d.x, 5.0f));
    CHECK(nearlyEqual(d.y, 0.0f));
}

// 1D blend: each clip in its phase and the weighted delta.
static void test_root_motion_delta_blend_weighted()
{
    const SkinnedMesh m = makeRootMotionMesh();
    AnimatorComponent a = makeRootMotionGraph();
    auto& s = a.statesMutable()[0];
    s.blendParam = "speed"; s.clipThreshold = 0.0f;
    s.blendEntries = { entrada("Run", 1, 80.0f, 1.0f) };
    a.addParameter("speed", AnimatorComponent::ParamType::Float);
    a.setFloat("speed", 0.5f);
    a.update(1.0f, true);                          // phase 0,5: Walk 5, Run 15
    CHECK(nearlyEqual(rootMotionDelta(m, a).x, 10.0f));
}

// Cross-fade between two Apply: 1 − w of the outgoing one, w of the incoming one.
static void test_root_motion_delta_crossfade_weighted()
{
    const SkinnedMesh m = makeRootMotionMesh();
    AnimatorComponent a = makeRootMotionGraph();
    AnimatorComponent::State b;
    b.name = "Run"; b.clipName = "Run"; b.clipIndex = 1; b.duration = 40.0f;   // 30 for every 40 ticks
    b.ticksPerSecond = 20.0f; b.loop = true; b.rootMotion = AnimatorComponent::RootMotion::Apply;
    a.addState(b);
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 2.0f;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "go";
    t.conditions.push_back(c);
    a.addTransition(t);

    a.setTrigger("go");
    a.update(0.016f, true);                        // exits to Run with a 2 s fade
    a.update(0.5f, true);                          // w = 0,25; 10 ticks each
    // Walk: 10 ticks = 2,5. Run (clip of 80 ticks stretched to 40): 10 ticks of
    // the mesh = 3,75. Delta = 0,75·2,5 + 0,25·3,75.
    CHECK(a.blending());
    CHECK(nearlyEqual(rootMotionDelta(m, a).x, 0.75f * 2.5f + 0.25f * 3.75f));
}

// Without Apply, or in Edit, there are no samples.
static void test_root_motion_samples_only_in_play_with_apply()
{
    AnimatorComponent a = makeRootMotionGraph();
    a.update(0.5f, false);
    CHECK(a.rootMotionSamples().empty());
    a.statesMutable()[0].rootMotion = AnimatorComponent::RootMotion::Lock;
    a.update(0.5f, true);
    CHECK(a.rootMotionSamples().empty());
}

// Space: in the real FBX the root's Y is its largest component (hip
// height). If it fails, the root's keys are not in model space with Y
// up and the design has to be revisited, not the test.
static void test_root_motion_space_is_model_y_up()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    CHECK(!m.animationClips.empty());
    if (m.animationClips.empty()) return;
    const glm::vec3 p = sampleRootPosition(m, 0, 0.0);
    CHECK(std::fabs(p.y) > std::fabs(p.x));
    CHECK(std::fabs(p.y) > std::fabs(p.z));
}

// The state being turned off uses ITS accumulated clock, not one that starts at 0: with
// a non-linear clip and the fade crossing its wrap, starting at 0 gives another advance.
static void test_root_motion_crossfade_uses_prev_clock()
{
    SkinnedMesh m = makeRootMotionMesh();
    // Walk fast in the first half (8) and slow in the second (2).
    m.animationClips[0].channels[0].posKeys = { { 0.0f,  glm::vec3(0.0f, 1.0f, 0.0f) },
                                                { 20.0f, glm::vec3(8.0f, 1.0f, 0.0f) },
                                                { 40.0f, glm::vec3(10.0f, 1.0f, 0.0f) } };
    AnimatorComponent a = makeRootMotionGraph();
    AnimatorComponent::State b;
    b.name = "Run"; b.clipName = "Run"; b.clipIndex = 1; b.duration = 40.0f;
    b.ticksPerSecond = 20.0f; b.loop = true; b.rootMotion = AnimatorComponent::RootMotion::Apply;
    a.addState(b);
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 2.0f;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "go";
    t.conditions.push_back(c);
    a.addTransition(t);

    a.update(1.8f, true);                          // Walk en 36 ticks
    a.setTrigger("go");
    a.update(0.016f, true);                        // 36,32: sale a Run
    a.update(0.5f, true);                          // Walk 36,32 -> 46,32 crosses its wrap
    CHECK(a.blending());
    const float prev = rootDisplacement(m, 0, 46.32, 40.0, true).x - rootDisplacement(m, 0, 36.32, 40.0, true).x;
    // Starting at 0 it would be 10 ticks of the fast stretch = 4: the test tells them apart.
    CHECK(std::fabs(prev - 4.0f) > 0.5f);
    CHECK(nearlyEqual(rootMotionDelta(m, a).x, 0.75f * prev + 0.25f * 3.75f, 0.01f));
}

// Without Rigidbody: the transform advances the delta in world space (with the
// object's scale) and, with a rotated parent, the local position compensates for it.
static void test_apply_root_motion_moves_transform()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    go->localTransform = glm::scale(glm::mat4(1.0f), glm::vec3(0.5f));
    scene.getRoot().updateWorldTransforms();
    applyRootMotion(*go, glm::vec3(4.0f, 9.0f, 0.0f), 0.016f);
    CHECK(nearlyEqual(go->worldTransform[3].x, 2.0f));
    CHECK(nearlyEqual(go->worldTransform[3].y, 0.0f));

    GameObject* padre = scene.addGameObject("Padre");
    padre->localTransform = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 1, 0));
    GameObject* hijo = scene.addGameObject("Hijo", padre);
    scene.getRoot().updateWorldTransforms();
    applyRootMotion(*hijo, glm::vec3(1.0f, 0.0f, 0.0f), 0.016f);
    // The child's X in model = −Z in world (90° rotation in Y of the parent).
    CHECK(nearlyEqual(hijo->worldTransform[3].z, -1.0f));
    CHECK(nearlyEqual(hijo->worldTransform[3].x, 0.0f));
}

// With a dynamic Rigidbody: velocity X/Z = delta/dt and Y is kept; the
// transform is not touched.
static void test_apply_root_motion_sets_rigidbody_velocity(PhysicsManager& pm)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto rb  = std::make_shared<Rigidbody>();
    auto col = pm.createBoxColliderComponent(glm::vec3(1.0f), glm::vec3(0.0f), glm::mat4(1.0f), /*dynamic=*/true);
    pm.attachRigidbody(col, rb);
    go->setRigidbody(rb);
    rb->setVelocity(glm::vec3(0.0f, -3.0f, 0.0f));
    scene.getRoot().updateWorldTransforms();

    applyRootMotion(*go, glm::vec3(0.5f, 0.0f, 0.25f), 0.5f);
    const glm::vec3 v = rb->getVelocity();
    CHECK(nearlyEqual(v.x, 1.0f));
    CHECK(nearlyEqual(v.z, 0.5f));
    CHECK(nearlyEqual(v.y, -3.0f));
    CHECK(nearlyEqual(go->worldTransform[3].x, 0.0f));
}

// applySkinnedFrame chains the root motion BEFORE sending the transform: the
// backend already receives the advanced position in this same frame.
static void test_apply_skinned_frame_applies_root_motion()
{
    Scene scene("Test");
    GameObject* go = makeSkinnedGameObject(scene, std::make_shared<AnimatorComponent>(makeRootMotionGraph()));
    go->setMesh(std::make_shared<SkinnedMesh>(makeRootMotionMesh()));
    scene.getRoot().updateWorldTransforms();

    SkinnedRendererDoble r;
    applySkinnedFrame(*go, r, 1.0f, /*evaluateTransitions=*/true);   // 20 ticks = 5 en X
    CHECK(nearlyEqual(go->worldTransform[3].x, 5.0f));
    CHECK(nearlyEqual(r.transform[3].x, 5.0f));

    // In Edit it does not move.
    applySkinnedFrame(*go, r, 1.0f, /*evaluateTransitions=*/false);
    CHECK(nearlyEqual(go->worldTransform[3].x, 5.0f));
}

// The hierarchy is evaluated in parallel by depth levels: the shader
// is only right if each bone sits exactly one level below its parent.
// Two roots (a chain of 3 and another of 2), and two clips: the depth is
// repeated in each clip's block.
static void test_packing_fills_bone_depth()
{
    SkinnedMesh m;
    m.skeleton.names       = { "a", "b", "c", "r2", "d" };
    m.skeleton.parentIndex = { -1, 0, 1, -1, 3 };
    m.skeleton.inverseBindPose.assign(5, glm::mat4(1.0f));
    AnimationClip c1; c1.name = "uno";  c1.duration = 10.0f; c1.ticksPerSecond = 10.0f;
    AnimationClip c2; c2.name = "dos";  c2.duration = 10.0f; c2.ticksPerSecond = 10.0f;
    m.animationClips = { c1, c2 };

    const PackedClips p = packSkinnedClips(m);
    CHECK(p.boneInfos.size() == 10u);
    if (p.boneInfos.size() != 10u) return;
    const int esperada[5] = { 0, 1, 2, 0, 1 };
    for (size_t clip = 0; clip < 2; clip++)
        for (int b = 0; b < 5; b++)
        {
            const GpuBoneInfo& bi = p.boneInfos[clip * 5 + b];
            CHECK(bi.depth == esperada[b]);
            if (bi.parentIndex >= 0)
                CHECK(bi.depth == p.boneInfos[clip * 5 + bi.parentIndex].depth + 1);
        }
}

// With the real FBX, the invariant holds on all bones.
static void test_packing_depth_invariant_real_rig()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    const PackedClips p = packSkinnedClips(m);
    const size_t B = m.skeleton.names.size();
    CHECK(B > 0 && p.boneInfos.size() >= B);
    if (B == 0 || p.boneInfos.size() < B) return;
    int maxDepth = 0;
    for (size_t b = 0; b < B; b++)
    {
        const GpuBoneInfo& bi = p.boneInfos[b];
        CHECK(bi.parentIndex < (int)b);                        // topological order
        CHECK(bi.depth == (bi.parentIndex < 0 ? 0 : p.boneInfos[bi.parentIndex].depth + 1));
        maxDepth = std::max(maxDepth, bi.depth);
    }
    // A humanoid rig has far fewer levels than bones: that is what makes
    // going by levels beat the serial walk.
    CHECK(maxDepth > 0 && maxDepth < (int)B / 2);
}

// A scene saved before "animationSources" existed does not carry the
// key: it means "only the animations of the FBX itself". With the preloaded mesh
// exactly as it comes out of the file, it is shared instead of being copied per
// node (before, each node took ~80 MB). And if the preloaded one carries an extra
// source, it is NOT shared: it is not what that scene asked for.
static void test_legacy_scene_without_sources_shares_preloaded(PhysicsManager& pm, AudioManager& am)
{
    const std::string ruta = "assets/modelAnimation.fbx";
    auto pristina = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned(ruta));
    CHECK(pristina->animationSources.size() == 1u);

    Scene fuente("Fuente");
    GameObject* go = fuente.addGameObject("Personaje");
    go->setMesh(pristina);
    nlohmann::json snap = fuente.subtreeToJson(go);
    CHECK(snap["mesh"].contains("animationSources"));
    snap["mesh"].erase("animationSources");             // scene prior to the key

    PreloadedMeshCache cache;
    cache[ruta] = pristina;
    Scene scene("Test");
    GameObject* r = scene.insertFromJson(snap, nullptr, 0, pm, am, &cache);
    CHECK(r != nullptr);
    if (!r) return;
    CHECK(r->isSkinned());
    CHECK(r->getMesh().get() == pristina.get());

    // With an extra source in the preloaded one, the scene without the key does not ask for it:
    // it copies and configures it, without sharing.
    auto conExtra = std::make_shared<SkinnedMesh>(*pristina);
    AnimationSource extra; extra.path = "otra.fbx"; extra.builtin = false; extra.clipNames = { "x" };
    conExtra->animationSources.push_back(extra);
    PreloadedMeshCache cache2;
    cache2[ruta] = conExtra;
    Scene scene2("Test2");
    GameObject* r2 = scene2.insertFromJson(snap, nullptr, 0, pm, am, &cache2);
    CHECK(r2 != nullptr);
    if (!r2) return;
    CHECK(r2->getMesh().get() != conExtra.get());
}

// ── Pose by samples ─────────────────────────────────────────────────────────
//
// Two states with 1D blend: Loco (Walk clip 0 threshold 0, Run clip 2 threshold 1) and
// Otro (Idle clip 1 threshold 0, Jump clip 3 threshold 1), with speed = 0,5 (pair
// at halfway in both). go: Loco -> Otro, 2 s fade. back: Otro -> Loco, 1 s.
static AnimatorComponent makeTwoBlendStates()
{
    AnimatorComponent a;
    auto estado = [](const char* n, const char* principal, int ci, const char* extra, int ce) {
        AnimatorComponent::State s;
        s.name = n; s.clipName = principal; s.clipIndex = ci; s.duration = 40.0f;
        s.ticksPerSecond = 20.0f; s.loop = true;
        s.blendParam = "speed"; s.clipThreshold = 0.0f;
        s.blendEntries = { entrada(extra, ce, 40.0f, 1.0f) };
        return s;
    };
    a.addState(estado("Loco", "Walk", 0, "Run", 2));
    a.addState(estado("Otro", "Idle", 1, "Jump", 3));
    a.setEntryState(0);
    a.addParameter("speed", AnimatorComponent::ParamType::Float);
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    a.addParameter("back", AnimatorComponent::ParamType::Trigger);
    auto trans = [&](int from, int to, float dur, const char* trig) {
        AnimatorComponent::Transition t;
        t.fromState = from; t.toState = to; t.duration = dur;
        AnimatorComponent::Condition c;
        c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = trig;
        t.conditions.push_back(c);
        a.addTransition(t);
    };
    trans(0, 1, 2.0f, "go");
    trans(1, 0, 1.0f, "back");
    a.reset();
    a.setFloat("speed", 0.5f);
    return a;
}

static float pesoMuestra(const AnimationPose& p, int clip)
{
    float w = 0.0f;
    for (int i = 0; i < p.count; i++) if (p.samples[i].clip == clip) w += p.samples[i].weight;
    return w;
}

static float sumaPesos(const AnimationPose& p)
{
    float w = p.layers[0].frozenWeight;
    for (int i = 0; i < p.count; i++) w += p.samples[i].weight;
    return w;
}

// Without fade: the state's pair, with its weights.
static void test_pose_samples_without_fade()
{
    AnimatorComponent a = makeTwoBlendStates();
    a.update(0.5f, true);
    const AnimationPose p = a.pose();
    CHECK(p.count == 2);
    CHECK(nearlyEqual(pesoMuestra(p, 0), 0.5f));
    CHECK(nearlyEqual(pesoMuestra(p, 2), 0.5f));
    CHECK(nearlyEqual(p.layers[0].frozenWeight, 0.0f));
    CHECK(!p.layers[0].freezeNow);
    // Without blend: a single sample of weight 1.
    a.setFloat("speed", 0.0f);
    a.update(0.0f, true);
    CHECK(a.pose().count == 1);
    CHECK(nearlyEqual(a.pose().samples[0].weight, 1.0f));
}

// Fade between two states with blend: the 4 samples, the outgoing one with ITS clock.
static void test_pose_samples_fade_between_blends()
{
    AnimatorComponent a = makeTwoBlendStates();
    a.update(0.5f, true);                          // Loco at 10 ticks
    a.setTrigger("go");
    a.update(0.016f, true);                        // exits to Otro
    a.update(0.5f, true);                          // w = 0,258 approx.
    const float w = a.blendWeight();
    CHECK(w > 0.2f && w < 0.3f);
    const AnimationPose p = a.pose();
    CHECK(p.count == 4);
    CHECK(nearlyEqual(pesoMuestra(p, 0), (1.0f - w) * 0.5f));
    CHECK(nearlyEqual(pesoMuestra(p, 2), (1.0f - w) * 0.5f));
    CHECK(nearlyEqual(pesoMuestra(p, 1), w * 0.5f));
    CHECK(nearlyEqual(pesoMuestra(p, 3), w * 0.5f));
    for (int i = 0; i < p.count; i++)
        if (p.samples[i].clip == 0 || p.samples[i].clip == 2)
            CHECK(nearlyEqual(p.samples[i].time, a.previousAnimTime()));   // Run: phase of the previous x 40
}

// Interrupting a fade freezes the pose once and the previous state stops
// contributing samples.
static void test_pose_freeze_on_interrupted_fade()
{
    AnimatorComponent a = makeTwoBlendStates();
    a.setTrigger("go");
    a.update(0.016f, true);
    a.update(0.5f, true);                          // Loco -> Otro fade in flight
    a.setTrigger("back");
    a.update(0.016f, true);                        // interrupts: Otro -> Loco
    AnimationPose p = a.pose();
    CHECK(p.layers[0].freezeNow);
    CHECK(a.fading());
    CHECK(!a.blending());
    CHECK(nearlyEqual(p.layers[0].frozenWeight, 1.0f - a.blendWeight()));
    CHECK(nearlyEqual(pesoMuestra(p, 1) + pesoMuestra(p, 3), 0.0f));   // Otro no longer contributes
    a.clearFreezeRequest();
    CHECK(!a.pose().layers[0].freezeNow);
    a.update(0.25f, true);
    p = a.pose();
    CHECK(!p.layers[0].freezeNow);
    CHECK(nearlyEqual(p.layers[0].frozenWeight, 1.0f - a.blendWeight()));
    CHECK(p.layers[0].frozenWeight > 0.0f && p.layers[0].frozenWeight < 1.0f);
    a.update(2.0f, true);                          // the fade ends
    CHECK(!a.fading());
    CHECK(nearlyEqual(a.pose().layers[0].frozenWeight, 0.0f));
}

// The weights sum to 1 in all cases.
static void test_pose_weights_sum_to_one()
{
    AnimatorComponent a = makeTwoBlendStates();
    CHECK(nearlyEqual(sumaPesos(a.pose()), 1.0f));
    a.setTrigger("go");
    a.update(0.016f, true);
    for (int i = 0; i < 5; i++) { a.update(0.3f, true); CHECK(nearlyEqual(sumaPesos(a.pose()), 1.0f)); }
    a.setTrigger("back");
    a.update(0.016f, true);
    for (int i = 0; i < 5; i++) { a.update(0.3f, true); CHECK(nearlyEqual(sumaPesos(a.pose()), 1.0f)); }
}

static SkinnedMesh makeFourConstantClips()
{
    SkinnedMesh m;
    m.skeleton.names = { "Hips" };
    m.skeleton.parentIndex = { -1 };
    m.skeleton.inverseBindPose = { glm::mat4(1.0f) };
    m.skeleton.boneMap["Hips"] = 0;
    const glm::vec3 pos[4] = { {0,0,0}, {0,20,0}, {10,0,0}, {0,0,30} };
    const char* nombres[4] = { "Walk", "Idle", "Run", "Jump" };
    for (int c = 0; c < 4; c++)
    {
        AnimationClip clip; clip.name = nombres[c]; clip.duration = 40.0f; clip.ticksPerSecond = 20.0f;
        BoneChannel ch; ch.boneIndex = 0;
        ch.posKeys = { { 0.0f, pos[c] }, { 40.0f, pos[c] } };
        ch.rotKeys = { { 0.0f, glm::quat(1,0,0,0) }, { 40.0f, glm::quat(1,0,0,0) } };
        ch.scaleKeys = { { 0.0f, glm::vec3(1.0f) }, { 40.0f, glm::vec3(1.0f) } };
        clip.channels.push_back(ch);
        m.animationClips.push_back(clip);
    }
    return m;
}

// A3: a fade from a state with blend does not jump on its first frame.
static void test_pose_continuity_fade_from_blend()
{
    const SkinnedMesh mesh = makeFourConstantClips();
    const PackedClips p = packSkinnedClips(mesh);
    AnimatorComponent a = makeTwoBlendStates();
    a.update(0.5f, true);
    const glm::vec3 antes = evalPoseTrs(p, 1, 0, a.pose(), nullptr).p;   // (5,0,0)
    a.setTrigger("go");
    a.update(0.016f, true);                                              // first frame of the fade
    const glm::vec3 despues = evalPoseTrs(p, 1, 0, a.pose(), nullptr).p;
    CHECK(nearlyEqual(antes.x, 5.0f));
    CHECK(glm::length(despues - antes) < 0.5f);
}

// A4: interrupting a fade does not jump; the frozen one is the previous frame's output.
static void test_pose_continuity_interrupted_fade()
{
    const SkinnedMesh mesh = makeFourConstantClips();
    const PackedClips p = packSkinnedClips(mesh);
    AnimatorComponent a = makeTwoBlendStates();
    a.setTrigger("go");
    a.update(0.016f, true);
    a.update(0.5f, true);
    std::vector<Trs> salida = { evalPoseTrs(p, 1, 0, a.pose(), nullptr) };
    const glm::vec3 antes = salida[0].p;
    a.setTrigger("back");
    a.update(0.016f, true);
    const AnimationPose tras = a.pose();
    CHECK(tras.layers[0].freezeNow);
    const glm::vec3 despues = evalPoseTrs(p, 1, 0, tras, &salida).p;     // the GPU freezes 'salida'
    CHECK(glm::length(despues - antes) < 0.5f);
}

// ── 2D Blend: triangulation and weights ────────────────────────────────────
static float peso2D(const Blend2DWeight* w, int n, int point)
{
    float s = 0.0f;
    for (int i = 0; i < n; i++) if (w[i].point == point) s += w[i].weight;
    return s;
}
static float suma2D(const Blend2DWeight* w, int n)
{
    float s = 0.0f;
    for (int i = 0; i < n; i++) s += w[i].weight;
    return s;
}

static void test_blend2d_inside_triangle_barycentric()
{
    const std::vector<glm::vec2> pts = { {0, 0}, {2, 0}, {0, 2} };
    Blend2DWeight w[3];
    const int n = blend2DWeights(pts, {0.5f, 0.5f}, w);
    CHECK(n == 3);
    CHECK(nearlyEqual(peso2D(w, n, 0), 0.5f));
    CHECK(nearlyEqual(peso2D(w, n, 1), 0.25f));
    CHECK(nearlyEqual(peso2D(w, n, 2), 0.25f));
}

static void test_blend2d_vertex_edge_outside()
{
    const std::vector<glm::vec2> pts = { {0, 0}, {2, 0}, {0, 2} };
    Blend2DWeight w[3];
    int n = blend2DWeights(pts, {2, 0}, w);                 // vertex
    CHECK(n == 1 && w[0].point == 1 && nearlyEqual(w[0].weight, 1.0f));
    n = blend2DWeights(pts, {1, 0}, w);                     // edge
    CHECK(n == 2);
    CHECK(nearlyEqual(peso2D(w, n, 0), 0.5f) && nearlyEqual(peso2D(w, n, 1), 0.5f));
    n = blend2DWeights(pts, {1, -3}, w);                    // outside: lower edge
    CHECK(n == 2);
    CHECK(nearlyEqual(peso2D(w, n, 0), 0.5f) && nearlyEqual(peso2D(w, n, 1), 0.5f));
    n = blend2DWeights(pts, {5, 5}, w);                     // outside: hypotenuse, at its center
    CHECK(n == 2);
    CHECK(nearlyEqual(peso2D(w, n, 1), 0.5f) && nearlyEqual(peso2D(w, n, 2), 0.5f));
}

static void test_blend2d_degenerate_cases()
{
    Blend2DWeight w[3];
    // Collinear: the value falls between the 2nd and the 3rd in order along the
    // line, even if the middle index is the last one.
    const std::vector<glm::vec2> linea = { {0, 0}, {4, 4}, {1, 1} };
    CHECK(triangulate2D(linea).empty());
    int n = blend2DWeights(linea, {2.5f, 2.5f}, w);
    CHECK(n == 2);
    CHECK(nearlyEqual(peso2D(w, n, 2), 0.5f) && nearlyEqual(peso2D(w, n, 1), 0.5f));
    // Two points, and the value outside on one side.
    const std::vector<glm::vec2> dos = { {0, 0}, {1, 0} };
    n = blend2DWeights(dos, {-1, 3}, w);
    CHECK(n == 1 && w[0].point == 0);
    // Just one.
    n = blend2DWeights({ {3, 3} }, {0, 0}, w);
    CHECK(n == 1 && w[0].point == 0 && nearlyEqual(w[0].weight, 1.0f));
    // Repeated: the first counts, the other never gets weight.
    const std::vector<glm::vec2> rep = { {0, 0}, {2, 0}, {0, 2}, {2, 0} };
    CHECK(triangulate2D(rep).size() == 1u);
    n = blend2DWeights(rep, {2, 0}, w);
    CHECK(n == 1 && w[0].point == 1);
}

// Cocircular grid: both diagonals are Delaunay; only one can remain.
static void test_blend2d_cocircular_square()
{
    const std::vector<glm::vec2> pts = { {-1, -1}, {1, -1}, {-1, 1}, {1, 1} };
    CHECK(triangulate2D(pts).size() == 2u);
    Blend2DWeight w[3];
    for (int i = 0; i <= 100; i++)
    {
        const float t = -1.0f + 0.02f * (float)i;
        const int n = blend2DWeights(pts, {t, t}, w);
        CHECK(nearlyEqual(suma2D(w, n), 1.0f));
    }
}

// Quadrilateral with the long diagonal (A-B) first in lexicographic order:
// the circle of ABC contains D, so Delaunay keeps the short one (C-D).
// Without the circle test, the overlap filter would accept ABC and ABD alike.
static void test_blend2d_prefers_delaunay_diagonal()
{
    const std::vector<glm::vec2> pts = { {-2, 0}, {2, 0}, {0, 1}, {0, -1} };
    CHECK(triangulate2D(pts).size() == 2u);
    Blend2DWeight w[3];
    const int n = blend2DWeights(pts, {0.1f, 0.2f}, w);     // inside BCD
    CHECK(nearlyEqual(peso2D(w, n, 0), 0.0f));
    CHECK(peso2D(w, n, 1) > 0.0f);
}

static void test_blend2d_continuity_sweep()
{
    const std::vector<glm::vec2> pts = { {-1, -1}, {1, -1}, {-1, 1}, {1, 1}, {0.2f, 0.1f} };
    CHECK(triangulate2D(pts).size() == 4u);
    float prev[2][301][5] = {};
    float maxSalto = 0.0f, maxErrSuma = 0.0f;
    for (int iy = 0; iy <= 300; iy++)
        for (int ix = 0; ix <= 300; ix++)
        {
            const glm::vec2 p(-1.5f + 0.01f * ix, -1.5f + 0.01f * iy);
            Blend2DWeight w[3];
            const int n = blend2DWeights(pts, p, w);
            maxErrSuma = std::max(maxErrSuma, std::fabs(suma2D(w, n) - 1.0f));
            float* cur = prev[iy & 1][ix];
            for (int k = 0; k < 5; k++) cur[k] = peso2D(w, n, k);
            for (int k = 0; k < 5; k++)
            {
                if (ix > 0) maxSalto = std::max(maxSalto, std::fabs(cur[k] - prev[iy & 1][ix - 1][k]));
                if (iy > 0) maxSalto = std::max(maxSalto, std::fabs(cur[k] - prev[(iy - 1) & 1][ix][k]));
            }
        }
    CHECK(maxErrSuma < 1e-4f);
    CHECK(maxSalto < 0.05f);
}

// 2D state: clips 0..3 at the corners of [0,1]², parameters "x" and "y".
static AnimatorComponent makeBlend2DState()
{
    AnimatorComponent a;
    AnimatorComponent::State s;
    s.name = "Mov"; s.clipName = "C0"; s.clipIndex = 0; s.duration = 40.0f;
    s.ticksPerSecond = 20.0f; s.loop = true;
    s.blendParam = "x"; s.blendParamY = "y";
    s.clipThreshold = 0.0f; s.clipThresholdY = 0.0f;
    s.blendEntries = { entrada("C1", 1, 20.0f, 1.0f), entrada("C2", 2, 80.0f, 0.0f), entrada("C3", 3, 40.0f, 1.0f) };
    s.blendEntries[0].thresholdY = 0.0f;
    s.blendEntries[1].thresholdY = 1.0f;
    s.blendEntries[2].thresholdY = 1.0f;
    a.addState(s);
    a.setEntryState(0);
    a.addParameter("x", AnimatorComponent::ParamType::Float);
    a.addParameter("y", AnimatorComponent::ParamType::Float);
    a.reset();
    return a;
}

static void test_blend2d_state_samples()
{
    AnimatorComponent a = makeBlend2DState();
    CHECK(a.stateBlends2D(0));
    a.setFloat("x", 0.25f); a.setFloat("y", 0.25f);
    a.update(0.5f, true);                               // 10 ticks: phase 0.25
    AnimatorComponent::BlendSample bs[3];
    const int n = a.stateBlendSamples(0, a.animTime(), bs);
    CHECK(n == 3);
    float suma = 0.0f;
    for (int i = 0; i < n; i++)
    {
        suma += bs[i].weight;
        CHECK(nearlyEqual(bs[i].time, 0.25f * bs[i].duration));   // same phase
    }
    CHECK(nearlyEqual(suma, 1.0f));
    // Without a declared Y parameter: it goes back to being the usual 1D.
    AnimatorComponent b = makeBlend2DState();
    b.statesMutable()[0].blendParamY = "noExiste";
    CHECK(!b.stateBlends2D(0));
    CHECK(b.stateBlendSamples(0, 0.0f, bs) <= 2);
}

static void test_blend2d_pose_and_fade()
{
    AnimatorComponent a = makeBlend2DState();
    a.setFloat("x", 0.3f); a.setFloat("y", 0.6f);
    a.update(0.1f, true);
    AnimationPose p = a.pose();
    CHECK(p.count == 3);
    // Second 2D state (same clips) and a 1 s fade between the two.
    AnimatorComponent::State s2 = a.states()[0];
    s2.name = "Mov2";
    a.addState(s2);
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 1.0f;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "go";
    t.conditions.push_back(c);
    a.addTransition(t);
    a.setTrigger("go");
    a.update(0.016f, true);
    a.update(0.3f, true);
    p = a.pose();
    CHECK(p.count == 6);
    float suma = 0.0f;
    for (int i = 0; i < p.count; i++) suma += p.samples[i].weight;
    CHECK(nearlyEqual(suma, 1.0f));
}

static void test_blend2d_legacy_pair_is_two_heaviest()
{
    AnimatorComponent a = makeBlend2DState();
    a.setFloat("x", 0.9f); a.setFloat("y", 0.2f);      // triangle with C1 dominant
    a.update(0.1f, true);
    AnimatorComponent::BlendSample bs[3];
    const int n = a.stateBlendSamples(0, a.animTime(), bs);
    int best = 0, second = -1;
    for (int i = 1; i < n; i++) if (bs[i].weight > bs[best].weight) best = i;
    for (int i = 0; i < n; i++) if (i != best && (second < 0 || bs[i].weight > bs[second].weight)) second = i;
    CHECK(a.poseClipB() == bs[best].clip);
    CHECK(a.poseClipA() == bs[second].clip);
    CHECK(nearlyEqual(a.poseWeight(), bs[best].weight / (bs[best].weight + bs[second].weight)));
}

static void test_blend2d_root_motion_samples()
{
    AnimatorComponent a = makeBlend2DState();
    a.statesMutable()[0].rootMotion = AnimatorComponent::RootMotion::Apply;
    a.setFloat("x", 0.3f); a.setFloat("y", 0.6f);
    a.update(0.1f, true);
    a.update(0.1f, true);
    const auto& rm = a.rootMotionSamples();
    CHECK(rm.size() == 3u);
    const AnimationPose p = a.pose();
    for (const auto& s : rm) CHECK(nearlyEqual(s.weight, pesoMuestra(p, s.clip)));
}

static void test_blend2d_serialization(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>(makeBlend2DState());
    a->statesMutable()[0].clipThresholdY = 0.125f;
    go->setAnimator(a);
    nlohmann::json j = scene.toJson();
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& st = found->getAnimator()->states()[0];
    CHECK(st.blendParamY == "y");
    CHECK(nearlyEqual(st.clipThresholdY, 0.125f));
    CHECK(st.blendEntries.size() == 3u);
    if (st.blendEntries.size() == 3u) CHECK(nearlyEqual(st.blendEntries[2].thresholdY, 1.0f));
    // A 1D writes no new key.
    auto b = std::make_shared<AnimatorComponent>(makeTwoBlendStates());
    GameObject* go2 = scene.addGameObject("Otro");
    go2->setAnimator(b);
    const std::string texto = scene.toJson().dump();
    size_t apariciones = 0;
    // "hresholdY" matches clipThresholdY and thresholdY.
    for (size_t pos = texto.find("hresholdY"); pos != std::string::npos; pos = texto.find("hresholdY", pos + 1)) apariciones++;
    CHECK(apariciones == 4u);   // only the 2D state: clipThresholdY + 3 entries
}

// Two layers: base with "Idle"(clip 0) -> "Run"(clip 1) by trigger "go"; layer
// 1 "Brazos" with "Low"(clip 2) -> "Aim"(clip 3) by the SAME trigger.
static AnimatorComponent makeTwoLayers()
{
    AnimatorComponent a;
    auto st = [](const char* n, int clip) {
        AnimatorComponent::State s;
        s.name = n; s.clipName = n; s.clipIndex = clip; s.duration = 40.0f;
        s.ticksPerSecond = 20.0f; s.loop = true;
        return s;
    };
    a.addParameter("go", AnimatorComponent::ParamType::Trigger);
    auto trans = [&](int layer) {
        AnimatorComponent::Transition t;
        t.fromState = 0; t.toState = 1; t.duration = 0.5f;
        AnimatorComponent::Condition c;
        c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "go";
        t.conditions.push_back(c);
        a.addTransition(t, layer);
    };
    a.addState(st("Idle", 0)); a.addState(st("Run", 1)); a.setEntryState(0); trans(0);
    const int l1 = a.addLayer("Brazos");
    a.addState(st("Low", 2), l1); a.addState(st("Aim", 3), l1); a.setEntryState(0, l1); trans(l1);
    a.reset();
    return a;
}

static void test_layers_management()
{
    AnimatorComponent a = makeTwoLayers();
    CHECK(a.layerCount() == 2);
    CHECK(a.layer(1).name == "Brazos");
    a.setLayerWeight(0, 0.3f);  CHECK(nearlyEqual(a.layerWeight(0), 1.0f));   // the base does not change
    CHECK(nearlyEqual(a.layer(0).weight, 1.0f));                                // nor the saved field
    // And even if someone writes the field by hand, the pose sees the base at weight 1.
    a.layerMutable(0).weight = 0.2f;
    CHECK(nearlyEqual(a.layerWeight(0), 1.0f));
    CHECK(nearlyEqual(a.pose().layers[0].weight, 1.0f));
    a.layerMutable(0).weight = 1.0f;
    a.setLayerWeight(1, 1.7f);  CHECK(nearlyEqual(a.layerWeight(1), 1.0f));
    a.setLayerWeight(1, 0.25f); CHECK(nearlyEqual(a.layerWeight(1), 0.25f));
    for (int i = 2; i < AnimatorComponent::kMaxLayers; i++) CHECK(a.addLayer("x") == i);
    CHECK(a.addLayer("sobra") == -1);
    a.removeLayer(0);            CHECK(a.layerCount() == AnimatorComponent::kMaxLayers);
    a.moveLayer(1, 0);           CHECK(a.layer(1).name == "Brazos");
    a.moveLayer(1, 2);           CHECK(a.layer(2).name == "Brazos");
    a.removeLayer(2);            CHECK(a.layerCount() == AnimatorComponent::kMaxLayers - 1);
}

static void test_layers_trigger_fires_in_both_layers()
{
    AnimatorComponent a = makeTwoLayers();
    a.update(0.1f, true);
    a.setTrigger("go");
    a.update(0.1f, true);
    CHECK(a.currentState(0) == 1);
    CHECK(a.currentState(1) == 1);
    CHECK(a.fading(0) && a.fading(1));
}

static void test_layers_clocks_independent()
{
    AnimatorComponent a = makeTwoLayers();
    a.update(0.5f, true);
    a.play("Aim", 1);                     // only layer 1 restarts its clock
    a.update(0.25f, true);
    CHECK(nearlyEqual(a.animTime(0), 15.0f));
    CHECK(nearlyEqual(a.animTime(1), 5.0f));
}

static void test_layers_pose_tags_and_weights()
{
    AnimatorComponent a = makeTwoLayers();
    a.setLayerWeight(1, 0.4f);
    a.setLayerMode(1, AnimatorComponent::LayerMode::Additive);
    a.update(0.1f, true);
    AnimationPose p = a.pose();
    CHECK(p.layerCount == 2);
    CHECK(p.count == 2);
    CHECK(p.samples[0].layer == 0 && p.samples[0].clip == 0);
    CHECK(p.samples[1].layer == 1 && p.samples[1].clip == 2);
    CHECK(nearlyEqual(p.layers[1].weight, 0.4f));
    CHECK(p.layers[1].mode == 1u);
    a.setLayerWeight(1, 0.0f);
    p = a.pose();
    CHECK(p.layerCount == 2 && p.count == 1);            // the layer remains, without samples
    CHECK(nearlyEqual(p.layers[1].weight, 0.0f));
}

static void test_layers_events_and_root_motion()
{
    AnimatorComponent a = makeTwoLayers();
    a.statesMutable(1)[0].events = { { "golpe", 0.1f } };
    a.statesMutable(1)[0].rootMotion = AnimatorComponent::RootMotion::Apply;
    a.update(0.3f, true);                                // 6 ticks: phase 0.15
    CHECK(std::find(a.firedEvents().begin(), a.firedEvents().end(), "golpe") != a.firedEvents().end());
    CHECK(a.rootMotionSamples().empty());                // root motion only from the base
    a.reset();
    a.setLayerWeight(1, 0.0f);
    a.update(0.3f, true);
    CHECK(a.firedEvents().empty());                      // a layer at weight 0 does not fire
}

static void test_layers_mask_resolution()
{
    SkinnedMesh m;
    m.skeleton.names       = { "hips", "spine", "arm", "hand", "leg" };
    m.skeleton.parentIndex = { -1, 0, 1, 2, 0 };
    m.skeleton.inverseBindPose.assign(5, glm::mat4(1.0f));
    AnimatorComponent a = makeTwoLayers();
    a.layerMutable(1).maskBones = { "arm", "hand", "noExiste" };
    std::vector<std::string> avisos;
    a.bindClips(m, &avisos);
    const auto& r = a.layer(1).maskResolved;
    CHECK(r.size() == 5u);
    if (r.size() == 5u) CHECK(r[0] == 0 && r[1] == 0 && r[2] == 1 && r[3] == 1 && r[4] == 0);
    bool avisado = false;
    for (const auto& w : avisos) if (w.find("noExiste") != std::string::npos) avisado = true;
    CHECK(avisado);
    a.layerMutable(1).maskBones.clear();
    a.rebindClips(m, nullptr);
    CHECK(a.layer(1).maskResolved.empty());
    const AnimationPose p = a.pose();
    CHECK(p.layers[1].mask == nullptr);
}

static void test_layers_serialization(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>(makeTwoLayers());
    const int l2 = a->addLayer("Respirar");
    a->setLayerWeight(1, 0.6f);
    a->setLayerMode(l2, AnimatorComponent::LayerMode::Additive);
    a->layerMutable(1).maskBones = { "arm", "hand" };
    go->setAnimator(a);
    nlohmann::json j = scene.toJson();
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& b = *found->getAnimator();
    CHECK(b.layerCount() == 3);
    if (b.layerCount() != 3) return;
    CHECK(b.layer(1).name == "Brazos" && nearlyEqual(b.layer(1).weight, 0.6f));
    CHECK(b.layer(1).maskBones.size() == 2u);
    CHECK(b.layer(1).states.size() == 2u && b.layer(1).transitions.size() == 1u);
    CHECK(b.layer(2).mode == AnimatorComponent::LayerMode::Additive);
    CHECK(b.layer(2).maskBones.empty());
}

// One layer: no new key in the JSON.
static void test_layers_single_layer_json_unchanged()
{
    AnimatorComponent a = makeTwoBlendStates();
    const nlohmann::json j = animatorToJson(a);
    CHECK(!j.contains("layers"));
    AnimatorComponent b = makeTwoLayers();
    CHECK(animatorToJson(b).contains("layers"));
}

static void test_layers_too_many_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto a = std::make_shared<AnimatorComponent>(makeTwoLayers());
    go->setAnimator(a);
    nlohmann::json j = scene.toJson();
    for (auto& node : j["root"]["children"])
        if (node.contains("animator"))
            while (node["animator"]["layers"].size() < 9) node["animator"]["layers"].push_back(node["animator"]["layers"][0]);
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool avisado = false;
    for (const auto& w : loaded.lastWarnings()) if (w.find("layers") != std::string::npos) avisado = true;
    CHECK(avisado);
}

// The layer undo: the graph key changes when touching weight, mode or mask.
static void test_layers_graph_key_sees_layer_edits()
{
    AnimatorComponent a = makeTwoLayers();
    const auto k0 = animatorGraphKey(a);
    a.setLayerWeight(1, 0.5f);
    const auto k1 = animatorGraphKey(a);
    CHECK(k0 != k1);
    a.layerMutable(1).maskBones = { "arm" };
    CHECK(animatorGraphKey(a) != k1);
}

// The layers undo goes through graph()/applyGraph: the snapshot returns the
// layers (name, weight, mode, mask, graph) and removes those that were added.
static void test_layers_apply_graph_restores()
{
    AnimatorComponent a = makeTwoLayers();
    a.update(0.1f, true);
    const AnimatorComponent::Graph antes = a.graph();
    a.setLayerWeight(1, 0.3f);
    a.layerMutable(1).maskBones = { "arm" };
    a.addState(a.states(1)[0], 1);
    a.addLayer("Nueva");
    a.applyGraph(antes);
    CHECK(a.layerCount() == 2);
    CHECK(nearlyEqual(a.layer(1).weight, 1.0f));
    CHECK(a.layer(1).maskBones.empty());
    CHECK(a.states(1).size() == 2u);
    CHECK(a.currentState(1) == 0);          // keeps its playhead
    // And redo: the added layer comes back.
    AnimatorComponent b = makeTwoLayers();
    b.addLayer("Nueva");
    const AnimatorComponent::Graph despues = b.graph();
    a.applyGraph(despues);
    CHECK(a.layerCount() == 3);
    CHECK(a.layer(2).name == "Nueva");
}


// ── Layers on the GPU: block layout and CPU replica of bone_eval ────────────

static void test_pose_block_layout()
{
    AnimationPose p;
    p.layerCount = 2;
    p.count = 2;
    p.samples[0] = { 3, 12.5f, 1.0f, 0 };
    p.samples[1] = { 1, 4.0f, 1.0f, 1 };
    std::vector<uint8_t> mask = { 0, 1, 1 };
    p.layers[1] = { 0.75f, 1u, 0.25f, false, &mask };
    std::vector<uint32_t> b(poseBlockUints(3), 0xDEADBEEFu);
    writePoseBlock(p, 3, b.data());
    auto f = [&](uint32_t i) { float v; std::memcpy(&v, &b[i], 4); return v; };
    CHECK(b[0] == 2u && b[1] == 2u);
    CHECK(nearlyEqual(f(4), 1.0f) && b[5] == 0u && b[7] == 0u);          // layer 0: weight 1, no mask
    CHECK(nearlyEqual(f(8), 0.75f) && b[9] == 1u && nearlyEqual(f(10), 0.25f) && b[11] == 1u);
    CHECK(b[12] == 0u && b[15] == 0u);                                   // layer 2: empty
    CHECK(b[36] == 9u && nearlyEqual(f(37), 12.5f) && nearlyEqual(f(38), 1.0f) && b[39] == 0u);
    CHECK(b[40] == 3u && b[43] == 1u);
    CHECK(b[44] == 0u && b[47] == 0u);                                   // sample 2: empty
    CHECK(b[228 + 3] == 0u && b[228 + 4] == 1u && b[228 + 5] == 1u);
    CHECK(b[228] == 0xDEADBEEFu);                                        // layer 0 does not write a mask
}

// Replica of decomposeTrs (bone_eval.comp).
static Trs decomposeTrsRef(const glm::mat4& m)
{
    Trs r;
    r.p = glm::vec3(m[3]);
    r.s = glm::vec3(glm::length(glm::vec3(m[0])), glm::length(glm::vec3(m[1])), glm::length(glm::vec3(m[2])));
    glm::mat3 R(glm::vec3(m[0]) / std::max(r.s.x, 1e-8f), glm::vec3(m[1]) / std::max(r.s.y, 1e-8f),
                glm::vec3(m[2]) / std::max(r.s.z, 1e-8f));
    const float tr = R[0][0] + R[1][1] + R[2][2];
    glm::vec4 q;
    if (tr > 0.0f)
    {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q = { (R[1][2] - R[2][1]) / s, (R[2][0] - R[0][2]) / s, (R[0][1] - R[1][0]) / s, 0.25f * s };
    }
    else if (R[0][0] > R[1][1] && R[0][0] > R[2][2])
    {
        const float s = std::sqrt(1.0f + R[0][0] - R[1][1] - R[2][2]) * 2.0f;
        q = { 0.25f * s, (R[1][0] + R[0][1]) / s, (R[2][0] + R[0][2]) / s, (R[1][2] - R[2][1]) / s };
    }
    else if (R[1][1] > R[2][2])
    {
        const float s = std::sqrt(1.0f + R[1][1] - R[0][0] - R[2][2]) * 2.0f;
        q = { (R[1][0] + R[0][1]) / s, 0.25f * s, (R[2][1] + R[1][2]) / s, (R[2][0] - R[0][2]) / s };
    }
    else
    {
        const float s = std::sqrt(1.0f + R[2][2] - R[0][0] - R[1][1]) * 2.0f;
        q = { (R[2][0] + R[0][2]) / s, (R[2][1] + R[1][2]) / s, 0.25f * s, (R[0][1] - R[1][0]) / s };
    }
    r.q = glm::normalize(q);
    return r;
}

static glm::vec4 qmulRef(const glm::vec4& a, const glm::vec4& b)
{
    const glm::vec3 av(a), bv(b);
    return glm::vec4(a.w * bv + b.w * av + glm::cross(av, bv), a.w * b.w - glm::dot(av, bv));
}

// Replica of evalCapa (bone_eval.comp) without frozen. bind = override and
// no sample animates the bone.
static Trs capaTrs(const PackedClips& p, size_t boneCount, size_t i, const AnimationPose& pose, int L, bool additive)
{
    Trs acc; acc.p = glm::vec3(0.0f); acc.s = glm::vec3(0.0f);
    glm::vec4 q(0.0f), ref(0.0f, 0.0f, 0.0f, 1.0f);
    bool hayRef = additive, alguna = false;
    float total = 0.0f;
    for (int k = 0; k < pose.count; k++)
    {
        if (pose.samples[k].layer != L) continue;
        const size_t base = (size_t)pose.samples[k].clip * boneCount;
        Trs t;
        if (sampleTrs(p, base, i, pose.samples[k].time, t))
        {
            alguna = true;
            if (additive)
            {
                Trs t0;
                sampleTrs(p, base, i, 0.0f, t0);
                t.p = t.p - t0.p;
                t.q = qmulRef(t.q, glm::vec4(-t0.q.x, -t0.q.y, -t0.q.z, t0.q.w));
                t.s = glm::vec3(t0.s.x != 0.0f ? t.s.x / t0.s.x : 1.0f,
                                t0.s.y != 0.0f ? t.s.y / t0.s.y : 1.0f,
                                t0.s.z != 0.0f ? t.s.z / t0.s.z : 1.0f);
            }
        }
        glm::vec4 qk = t.q;
        if (!hayRef) { ref = qk; hayRef = true; }
        if (glm::dot(qk, ref) < 0.0f) qk = -qk;
        const float w = pose.samples[k].weight;
        acc.p += w * t.p; acc.s += w * t.s; q += w * qk; total += w;
    }
    if (!additive && !alguna) { Trs b; b.bind = true; return b; }
    if (total <= 0.0f) return Trs{};
    if (std::fabs(total - 1.0f) > 1e-4f) { acc.p /= total; acc.s /= total; q /= total; }
    const float len = glm::length(q);
    acc.q = len > 1e-6f ? q / len : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return acc;
}

// Replica of bone_eval's main (without the root lock): the base and each layer
// on top with m = weight x mask.
static Trs evalLayeredTrs(const PackedClips& p, size_t boneCount, size_t i, const AnimationPose& pose)
{
    const size_t cb0 = pose.count > 0 ? (size_t)pose.samples[0].clip * boneCount : 0;
    const glm::mat4& bindLocal = p.boneInfos[cb0 + i].bindLocal;
    Trs r = capaTrs(p, boneCount, i, pose, 0, false);
    for (int L = 1; L < pose.layerCount; L++)
    {
        const PoseLayer& pl = pose.layers[L];
        const bool add = pl.mode == 1u;
        Trs c = capaTrs(p, boneCount, i, pose, L, add);
        const float mk = pl.mask ? ((i < pl.mask->size() && (*pl.mask)[i]) ? 1.0f : 0.0f) : 1.0f;
        const float m = pl.weight * mk;
        if (m <= 0.0f || c.bind) continue;
        if (r.bind) r = decomposeTrsRef(bindLocal);
        if (!add)
        {
            glm::vec4 lq = c.q;
            if (glm::dot(lq, r.q) < 0.0f) lq = -lq;
            r.p = glm::mix(r.p, c.p, m);
            r.s = glm::mix(r.s, c.s, m);
            r.q = glm::normalize(glm::mix(r.q, lq, m));
        }
        else
        {
            glm::vec4 lq = c.q;
            if (lq.w < 0.0f) lq = -lq;
            const glm::vec4 d = glm::normalize(glm::mix(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), lq, m));
            r.q = glm::normalize(qmulRef(d, r.q));
            r.p += m * c.p;
            r.s *= glm::mix(glm::vec3(1.0f), c.s, m);
        }
    }
    return r;
}

// Hip -> spine -> arm. Clip 0 "Base": each bone at (i, 0, 0), not
// rotated. Clip 1 "Brazo": (0, 5 + i, 0) and 90 degrees in Z. Clip 2 "Suma": from
// (0,2,0) and 30 degrees in Y at t = 0 to (0,3,0) and also 90 degrees in X at
// t = 10: its delta relative to the first frame is +Y and a turn in X, and WITHOUT
// subtracting the first frame something else comes out.
static SkinnedMesh makeLayerFixture()
{
    SkinnedMesh m;
    m.skeleton.names       = { "hips", "spine", "arm" };
    m.skeleton.parentIndex = { -1, 0, 1 };
    m.skeleton.inverseBindPose.assign(3, glm::mat4(1.0f));
    for (int i = 0; i < 3; i++) m.skeleton.boneMap[m.skeleton.names[i]] = i;
    const glm::quat id(1, 0, 0, 0);
    const glm::quat rz = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 0, 1));
    const glm::quat rx = glm::angleAxis(glm::half_pi<float>(), glm::vec3(1, 0, 0));
    const glm::quat ry = glm::angleAxis(glm::radians(30.0f), glm::vec3(0, 1, 0));
    const char* nombres[3] = { "Base", "Brazo", "Suma" };
    for (int c = 0; c < 3; c++)
    {
        AnimationClip clip; clip.name = nombres[c]; clip.duration = 10.0f; clip.ticksPerSecond = 10.0f;
        for (int b = 0; b < 3; b++)
        {
            BoneChannel ch; ch.boneIndex = b;
            if (c == 0) { ch.posKeys = { { 0.0f, glm::vec3((float)b, 0, 0) }, { 10.0f, glm::vec3((float)b, 0, 0) } };
                          ch.rotKeys = { { 0.0f, id }, { 10.0f, id } }; }
            if (c == 1) { ch.posKeys = { { 0.0f, glm::vec3(0, 5.0f + b, 0) }, { 10.0f, glm::vec3(0, 5.0f + b, 0) } };
                          ch.rotKeys = { { 0.0f, rz }, { 10.0f, rz } }; }
            if (c == 2) { ch.posKeys = { { 0.0f, glm::vec3(0, 2, 0) }, { 10.0f, glm::vec3(0, 3, 0) } };
                          ch.rotKeys = { { 0.0f, ry }, { 10.0f, rx * ry } }; }
            ch.scaleKeys = { { 0.0f, glm::vec3(1.0f) }, { 10.0f, glm::vec3(1.0f) } };
            clip.channels.push_back(ch);
        }
        m.animationClips.push_back(clip);
    }
    return m;
}

static bool mismoTrs(const Trs& a, const Trs& b, float eps = 1e-5f)
{
    return glm::length(a.p - b.p) < eps && glm::length(a.s - b.s) < eps &&
           std::fabs(std::fabs(glm::dot(a.q, b.q)) - 1.0f) < eps;
}

// Criterion of row 14: layer 1 override with arm mask and weight 1 -> the
// bones outside are exactly layer 0 and those inside are layer 1; weight 0 ->
// exactly layer 0; weight 0.5 -> halfway only inside.
static void test_layers_override_mask_criterion()
{
    const SkinnedMesh m = makeLayerFixture();
    const PackedClips p = packSkinnedClips(m);
    const size_t B = 3;
    std::vector<uint8_t> brazo = { 0, 0, 1 };
    AnimationPose base;  base.count = 1;  base.samples[0]  = { 0, 2.0f, 1.0f, 0 };
    AnimationPose solo1; solo1.count = 1; solo1.samples[0] = { 1, 2.0f, 1.0f, 0 };
    AnimationPose pose = base;
    pose.layerCount = 2; pose.count = 2;
    pose.samples[1] = { 1, 2.0f, 1.0f, 1 };
    pose.layers[1] = { 1.0f, 0u, 0.0f, false, &brazo };
    for (size_t i = 0; i < B; i++)
        CHECK(mismoTrs(evalLayeredTrs(p, B, i, pose), evalLayeredTrs(p, B, i, brazo[i] ? solo1 : base)));
    CHECK(!mismoTrs(evalLayeredTrs(p, B, 2, base), evalLayeredTrs(p, B, 2, solo1)));   // el test ve algo
    pose.layers[1].weight = 0.0f;
    for (size_t i = 0; i < B; i++)
        CHECK(mismoTrs(evalLayeredTrs(p, B, i, pose), evalLayeredTrs(p, B, i, base)));
    pose.layers[1].weight = 0.5f;
    const Trs medio = evalLayeredTrs(p, B, 2, pose);
    CHECK(glm::length(medio.p - glm::vec3(1.0f, 3.5f, 0.0f)) < 1e-5f);                 // (2,0,0)..(0,7,0)
    CHECK(mismoTrs(evalLayeredTrs(p, B, 0, pose), evalLayeredTrs(p, B, 0, base)));
    // Without a mask: the whole body.
    pose.layers[1] = { 1.0f, 0u, 0.0f, false, nullptr };
    for (size_t i = 0; i < B; i++)
        CHECK(mismoTrs(evalLayeredTrs(p, B, i, pose), evalLayeredTrs(p, B, i, solo1)));
}

// Additive: at t = 0 nothing changes; at the midpoint it adds half the delta.
static void test_layers_additive_delta()
{
    const SkinnedMesh m = makeLayerFixture();
    const PackedClips p = packSkinnedClips(m);
    const size_t B = 3;
    AnimationPose base; base.count = 1; base.samples[0] = { 0, 2.0f, 1.0f, 0 };
    AnimationPose pose = base;
    pose.layerCount = 2; pose.count = 2;
    pose.samples[1] = { 2, 0.0f, 1.0f, 1 };
    pose.layers[1] = { 1.0f, 1u, 0.0f, false, nullptr };
    for (size_t i = 0; i < B; i++)
        CHECK(mismoTrs(evalLayeredTrs(p, B, i, pose), evalLayeredTrs(p, B, i, base)));
    pose.samples[1].time = 5.0f;
    const Trs r = evalLayeredTrs(p, B, 1, pose);
    CHECK(glm::length(r.p - glm::vec3(1.0f, 0.5f, 0.0f)) < 1e-5f);
    const float s = std::sin(glm::quarter_pi<float>() * 0.5f), c = std::cos(glm::quarter_pi<float>() * 0.5f);
    CHECK(std::fabs(std::fabs(glm::dot(r.q, glm::vec4(s, 0.0f, 0.0f, c))) - 1.0f) < 1e-5f);    // 45 degrees in X
    CHECK(glm::length(r.s - glm::vec3(1.0f)) < 1e-5f);
    // Weight 0.5: half of the half.
    pose.layers[1].weight = 0.5f;
    CHECK(glm::length(evalLayeredTrs(p, B, 1, pose).p - glm::vec3(1.0f, 0.25f, 0.0f)) < 1e-5f);
    // Additive layer without samples: nothing (and not scale 0).
    pose.count = 1;
    pose.layers[1].weight = 1.0f;
    CHECK(mismoTrs(evalLayeredTrs(p, B, 1, pose), evalLayeredTrs(p, B, 1, base)));
}

// One layer: the per-layer replica gives the same as the row 13 one.
static void test_layers_single_layer_matches_pose_samples()
{
    const SkinnedMesh m = makeLayerFixture();
    const PackedClips p = packSkinnedClips(m);
    AnimationPose pose; pose.count = 2;
    pose.samples[0] = { 0, 2.0f, 0.3f, 0 };
    pose.samples[1] = { 1, 7.0f, 0.7f, 0 };
    for (size_t i = 0; i < 3; i++)
        CHECK(mismoTrs(evalLayeredTrs(p, 3, i, pose), evalPoseTrs(p, 3, i, pose, nullptr)));
}

// The panel identifies nodes by editorId: the search has to be in the
// layer being edited. Always looking at the base, a node of another layer did not exist
// and creating or deleting in it was silently discarded.
static void test_layers_state_index_by_editor_id()
{
    AnimatorComponent a = makeTwoLayers();
    const int eidBase = a.states(0)[1].editorId;
    const int eidCapa = a.states(1)[1].editorId;
    CHECK(eidBase != eidCapa);
    CHECK(a.stateIndexByEditorId(eidBase, 0) == 1);
    CHECK(a.stateIndexByEditorId(eidCapa, 1) == 1);
    CHECK(a.stateIndexByEditorId(eidCapa, 0) == -1);
    CHECK(a.stateIndexByEditorId(eidBase, 1) == -1);
    CHECK(a.stateIndexByEditorId(eidCapa, 7) == -1);   // layer that does not exist
}

// Arm skeleton: hips -> spine -> shoulder -> elbow -> hand, and head.
static SkinnedMesh makeIkSkeleton()
{
    SkinnedMesh m;
    m.skeleton.names       = { "hips", "spine", "shoulder", "elbow", "hand", "head" };
    m.skeleton.parentIndex = { -1, 0, 1, 2, 3, 1 };
    m.skeleton.inverseBindPose.assign(6, glm::mat4(1.0f));
    for (int i = 0; i < 6; i++) m.skeleton.boneMap[m.skeleton.names[(size_t)i]] = i;
    AnimationClip clip; clip.name = "Idle"; clip.duration = 10.0f; clip.ticksPerSecond = 10.0f;
    m.animationClips.push_back(clip);
    return m;
}

static AnimatorComponent makeIkAnimator()
{
    AnimatorComponent a;
    AnimatorComponent::State s;
    s.name = "Idle"; s.clipName = "Idle"; s.duration = 10.0f; s.ticksPerSecond = 10.0f;
    a.addState(s);
    a.setEntryState(0);
    AnimatorComponent::IkConstraint mirar;
    mirar.name = "mirar"; mirar.type = AnimatorComponent::IkType::LookAt;
    mirar.boneName = "head"; mirar.targetId = 7;
    AnimatorComponent::IkConstraint mano;
    mano.name = "mano"; mano.type = AnimatorComponent::IkType::TwoBone;
    mano.boneName = "hand"; mano.targetId = 8; mano.poleId = 9;
    a.addIkConstraint(mirar);
    a.addIkConstraint(mano);
    return a;
}

static void test_ik_constraint_management()
{
    AnimatorComponent a = makeIkAnimator();
    CHECK(a.ikConstraints().size() == 2u);
    for (int i = 2; i < AnimatorComponent::kMaxIkConstraints; i++)
    {
        AnimatorComponent::IkConstraint c;
        c.name = "x" + std::to_string(i);
        CHECK(a.addIkConstraint(c) == i);
    }
    AnimatorComponent::IkConstraint sobra;
    sobra.name = "sobra";
    CHECK(a.addIkConstraint(sobra) == -1);
    a.setIkWeight("mano", 1.7f);   CHECK(nearlyEqual(a.ikWeight("mano"), 1.0f));
    a.setIkWeight("mano", -1.0f);  CHECK(nearlyEqual(a.ikWeight("mano"), 0.0f));
    a.setIkWeight("mano", 0.25f);  CHECK(nearlyEqual(a.ikWeight("mano"), 0.25f));
    a.setIkWeight("noExiste", 1.0f);
    CHECK(nearlyEqual(a.ikWeight("noExiste"), 0.0f));
    a.setIkTarget("mirar", 42);    CHECK(a.ikConstraints()[0].targetId == 42u);
    a.setIkPole("mano", 43);       CHECK(a.ikConstraints()[1].poleId == 43u);
    a.setIkTarget("noExiste", 99);   // does not blow up nor touch anything
    CHECK(a.ikConstraints()[0].targetId == 42u);
    a.removeIkConstraint(0);
    CHECK(a.ikConstraints().size() == (size_t)AnimatorComponent::kMaxIkConstraints - 1);
    CHECK(a.ikConstraints()[0].name == "mano");
}

static void test_ik_chain_resolution()
{
    const SkinnedMesh m = makeIkSkeleton();
    AnimatorComponent a = makeIkAnimator();
    std::vector<std::string> avisos;
    a.bindClips(m, &avisos);
    const auto& mirar = a.ikConstraints()[0];
    CHECK(mirar.boneIndex == 5 && mirar.parentIndex == 1);
    const auto& mano = a.ikConstraints()[1];
    CHECK(mano.boneIndex == 4 && mano.parentIndex == 3 && mano.grandParentIndex == 2);
    CHECK(avisos.empty());

    // Bone that does not exist: warning and inactive constraint.
    a.ikConstraintsMutable()[0].boneName = "noExiste";
    // Chain too short for TwoBone: hips has neither parent nor grandparent.
    a.ikConstraintsMutable()[1].boneName = "hips";
    avisos.clear();
    a.rebindClips(m, &avisos);
    CHECK(a.ikConstraints()[0].boneIndex == -1);
    CHECK(a.ikConstraints()[1].boneIndex == -1);
    CHECK(avisos.size() == 2u);
    bool nombre = false, cadena = false;
    for (const auto& w : avisos)
    {
        if (w.find("noExiste") != std::string::npos) nombre = true;
        if (w.find("chain")   != std::string::npos) cadena = true;
    }
    CHECK(nombre && cadena);
}

static void test_ik_serialization(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>(makeIkAnimator());
    a->ikConstraintsMutable()[0].aimAxis  = { 0.0f, 1.0f, 0.0f };
    a->ikConstraintsMutable()[0].maxAngle = 55.0f;
    a->setIkWeight("mano", 0.5f);
    go->setAnimator(a);
    nlohmann::json j = scene.toJson();
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& ik = found->getAnimator()->ikConstraints();
    CHECK(ik.size() == 2u);
    if (ik.size() != 2u) return;
    CHECK(ik[0].name == "mirar" && ik[0].type == AnimatorComponent::IkType::LookAt);
    CHECK(ik[0].boneName == "head" && ik[0].targetId == 7u);
    CHECK(nearlyEqual(ik[0].maxAngle, 55.0f) && nearlyEqual(ik[0].aimAxis.y, 1.0f));
    CHECK(ik[1].type == AnimatorComponent::IkType::TwoBone);
    CHECK(ik[1].poleId == 9u && nearlyEqual(ik[1].weight, 0.5f));
    // An animator without IK does not write the key.
    AnimatorComponent sinIk;
    CHECK(!animatorToJson(sinIk).contains("ik"));
    CHECK(animatorToJson(*a).contains("ik"));
}

static void test_ik_bad_file_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    go->setAnimator(std::make_shared<AnimatorComponent>(makeIkAnimator()));
    nlohmann::json j = scene.toJson();
    for (auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        node["animator"]["ik"][0]["type"] = "loQueSea";
        while (node["animator"]["ik"].size() < 6) node["animator"]["ik"].push_back(node["animator"]["ik"][1]);
    }
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool tipo = false, tope = false;
    for (const auto& w : loaded.lastWarnings())
    {
        if (w.find("loQueSea") != std::string::npos) tipo = true;
        if (w.find("IK constraints") != std::string::npos) tope = true;
    }
    CHECK(tipo && tope);
    GameObject* found = loaded.getRoot().children[0].get();
    CHECK(found->getAnimator()->ikConstraints().size() == (size_t)AnimatorComponent::kMaxIkConstraints);
    CHECK(found->getAnimator()->ikConstraints()[0].type == AnimatorComponent::IkType::LookAt);
}

static void test_ik_graph_key_and_apply_graph()
{
    AnimatorComponent a = makeIkAnimator();
    const auto k0 = animatorGraphKey(a);
    a.setIkWeight("mano", 0.25f);
    CHECK(animatorGraphKey(a) != k0);
    const AnimatorComponent::Graph snap = a.graph();
    a.removeIkConstraint(0);
    a.setIkWeight("mano", 1.0f);
    a.applyGraph(snap);
    CHECK(a.ikConstraints().size() == 2u);
    CHECK(nearlyEqual(a.ikWeight("mano"), 0.25f));
}

// ── IK: CPU replica of bone_ik.comp ────────────────────────────────────────
// Rotation of a world transform (uniform scale is assumed, as in the spec).
static glm::quat rotDe(const glm::mat4& m)
{
    glm::mat3 r(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2])));
    return glm::normalize(glm::quat_cast(r));
}

// Minimal turn from a to b (unit vectors). With opposite vectors, any perpendicular
// axis works: a stable one is chosen.
static glm::quat giroEntre(const glm::vec3& a, const glm::vec3& b)
{
    const float d = glm::clamp(glm::dot(a, b), -1.0f, 1.0f);
    if (d > 0.9999f) return glm::quat(1, 0, 0, 0);
    if (d < -0.9999f)
    {
        glm::vec3 eje = glm::cross(a, glm::vec3(1, 0, 0));
        if (glm::length(eje) < 1e-4f) eje = glm::cross(a, glm::vec3(0, 1, 0));
        return glm::angleAxis(glm::pi<float>(), glm::normalize(eje));
    }
    return glm::angleAxis(std::acos(d), glm::normalize(glm::cross(a, b)));
}

// Clamps the turn to maxAngle degrees and scales it by weight.
static glm::quat acotaYPesa(const glm::quat& q, float maxAngleDeg, float weight)
{
    const glm::quat u = glm::normalize(q);
    float ang = 2.0f * std::acos(glm::clamp(u.w, -1.0f, 1.0f));
    if (ang < 1e-6f) return glm::quat(1, 0, 0, 0);
    const glm::vec3 eje = glm::vec3(u.x, u.y, u.z) / std::sin(ang * 0.5f);
    ang = std::min(ang, glm::radians(glm::clamp(maxAngleDeg, 0.0f, 180.0f)));
    return glm::angleAxis(ang * glm::clamp(weight, 0.0f, 1.0f), glm::normalize(eje));
}

// Locals corrected by the IK. `mundo` are the world transforms of all
// the bones (what the "world only" hierarchy leaves), `padres` the skeleton and
// `locales` the input/output. Replica of bone_ik.comp.
static void resolverIk(const AnimationIk& ik, const std::vector<glm::mat4>& mundo,
                       const std::vector<int>& padres, std::vector<glm::mat4>& locales)
{
    auto mundoRot = [&](int i) { return i >= 0 ? rotDe(mundo[(size_t)i]) : glm::quat(1, 0, 0, 0); };
    auto pos      = [&](int i) { return glm::vec3(mundo[(size_t)i][3]); };
    auto escribeRot = [&](int i, const glm::quat& nuevaMundo) {
        const glm::quat padre = mundoRot(padres[(size_t)i]);
        const glm::quat local = glm::normalize(glm::inverse(padre) * nuevaMundo);
        // Only the rotation changes: position and scale of the local are kept.
        const glm::vec3 t(locales[(size_t)i][3]);
        const glm::vec3 s(glm::length(glm::vec3(locales[(size_t)i][0])),
                          glm::length(glm::vec3(locales[(size_t)i][1])),
                          glm::length(glm::vec3(locales[(size_t)i][2])));
        glm::mat4 m = glm::mat4_cast(local);
        m[0] *= s.x; m[1] *= s.y; m[2] *= s.z;
        m[3] = glm::vec4(t, 1.0f);
        locales[(size_t)i] = m;
    };

    for (int k = 0; k < ik.count; k++)
    {
        const IkSolve& s = ik.solves[k];
        if (s.bone < 0 || s.weight <= 0.0f) continue;
        if (s.type == 0u)
        {
            const glm::vec3 p = pos(s.bone);
            const glm::vec3 d = s.target - p;
            if (glm::length(d) < 1e-5f || glm::length(s.aimAxis) < 1e-5f) continue;
            const glm::quat R    = mundoRot(s.bone);
            const glm::vec3 actual = glm::normalize(R * glm::normalize(s.aimAxis));
            const glm::quat giro = acotaYPesa(giroEntre(actual, glm::normalize(d)), s.maxAngle, s.weight);
            escribeRot(s.bone, glm::normalize(giro * R));
            continue;
        }
        // TwoBone: A grandparent, B parent, C end effector.
        const int A = s.grandParent, B = s.parent, C = s.bone;
        if (A < 0 || B < 0) continue;
        const glm::vec3 pA = pos(A), pB = pos(B), pC = pos(C);
        const float lAB = glm::length(pB - pA), lBC = glm::length(pC - pB);
        if (lAB < 1e-5f || lBC < 1e-5f) continue;
        const glm::vec3 haciaT = s.target - pA;
        if (glm::length(haciaT) < 1e-5f) continue;
        const float lAT = glm::clamp(glm::length(haciaT), 1e-4f, lAB + lBC - 1e-4f);

        // 1) Elbow: current and desired angle by the law of cosines.
        const glm::vec3 BA = glm::normalize(pA - pB), BC = glm::normalize(pC - pB);
        const float ang0 = std::acos(glm::clamp(glm::dot(BA, BC), -1.0f, 1.0f));
        const float ang1 = std::acos(glm::clamp((lAB * lAB + lBC * lBC - lAT * lAT) / (2.0f * lAB * lBC),
                                                -1.0f, 1.0f));
        glm::vec3 eje = glm::cross(pC - pA, pB - pA);
        if (glm::length(eje) < 1e-5f) eje = glm::cross(pC - pA, glm::vec3(0, 0, 1));
        if (glm::length(eje) < 1e-5f) eje = glm::vec3(0, 1, 0);
        eje = glm::normalize(eje);
        const glm::quat flex = glm::angleAxis((ang1 - ang0) * glm::clamp(s.weight, 0.0f, 1.0f), eje);

        // 2) Shoulder: brings the end effector (already flexed) to the target.
        const glm::vec3 pCflex = pB + flex * (pC - pB);
        glm::quat giro = giroEntre(glm::normalize(pCflex - pA), glm::normalize(haciaT));
        giro = glm::slerp(glm::quat(1, 0, 0, 0), giro, glm::clamp(s.weight, 0.0f, 1.0f));

        // 3) Pole: rotates around the A->target axis until the elbow is in its plane.
        if (s.hasPole != 0u)
        {
            const glm::vec3 ejeT = glm::normalize(haciaT);
            // The elbow is only moved by the shoulder's turn: the flexion is the rotation
            // of the elbow ITSELF and does not change its position. Putting it in here, the pole
            // picked the opposite side.
            const glm::vec3 pBnuevo = pA + giro * (pB - pA);
            const glm::vec3 actual = pBnuevo - pA - ejeT * glm::dot(pBnuevo - pA, ejeT);
            const glm::vec3 deseado = s.pole - pA - ejeT * glm::dot(s.pole - pA, ejeT);
            if (glm::length(actual) > 1e-5f && glm::length(deseado) > 1e-5f)
            {
                const glm::quat q = giroEntre(glm::normalize(actual), glm::normalize(deseado));
                giro = glm::normalize(glm::slerp(glm::quat(1, 0, 0, 0), q,
                                                 glm::clamp(s.weight, 0.0f, 1.0f)) * giro);
            }
        }

        const glm::quat RA = mundoRot(A), RB = mundoRot(B);
        const glm::quat RAnuevo = glm::normalize(giro * RA);
        const glm::quat RBnuevo = glm::normalize(giro * flex * RB);
        escribeRot(A, RAnuevo);
        // B is written AFTER A: its local is measured against A's new world.
        const glm::quat local = glm::normalize(glm::inverse(RAnuevo) * RBnuevo);
        const glm::vec3 t(locales[(size_t)B][3]);
        glm::mat4 mB = glm::mat4_cast(local);
        mB[3] = glm::vec4(t, 1.0f);
        locales[(size_t)B] = mB;
    }
}

// Composes the hierarchy on CPU (what bone_hierarchy does in its pass 1).
static std::vector<glm::mat4> componer(const std::vector<glm::mat4>& locales, const std::vector<int>& padres)
{
    std::vector<glm::mat4> mundo(locales.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < locales.size(); i++)
        mundo[i] = padres[i] < 0 ? locales[i] : mundo[(size_t)padres[i]] * locales[i];
    return mundo;
}

// Chain along +Y: A at the origin, B at 2, C at 4. A's parent is the root.
// B is offset a little in Z so that the elbow plane is DEFINED: with a
// perfectly straight chain, cross(C-A, B-A) is null and the fallback
// axis kicks in, which is not what the pole test wants to measure.
static void cadenaDePrueba(std::vector<glm::mat4>& locales, std::vector<int>& padres)
{
    padres  = { -1, 0, 1, 2 };                       // root, A, B, C
    locales = { glm::mat4(1.0f),
                glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 0)),
                glm::translate(glm::mat4(1.0f), glm::vec3(0, 2, 0.05f)),
                glm::translate(glm::mat4(1.0f), glm::vec3(0, 2, -0.05f)) };
}

static void test_ik_lookat()
{
    std::vector<int> padres = { -1, 0 };
    std::vector<glm::mat4> locales = { glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0, 1, 0)) };
    auto mundo = componer(locales, padres);
    AnimationIk ik;
    ik.count = 1;
    ik.solves[0] = { 0u, 1, 0, -1, 1.0f, glm::vec3(5, 1, 0), glm::vec3(0), 0u, glm::vec3(0, 0, 1), 180.0f };
    auto conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    // The bone's +Z axis ends up pointing at the target.
    glm::vec3 eje = glm::vec3(componer(conIk, padres)[1] * glm::vec4(0, 0, 1, 0));
    CHECK(glm::length(glm::normalize(eje) - glm::vec3(1, 0, 0)) < 1e-4f);

    // With the limit at 30 degrees, it turns exactly 30.
    ik.solves[0].maxAngle = 30.0f;
    conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    eje = glm::normalize(glm::vec3(componer(conIk, padres)[1] * glm::vec4(0, 0, 1, 0)));
    CHECK(std::fabs(glm::degrees(std::acos(glm::clamp(glm::dot(eje, glm::vec3(0, 0, 1)), -1.0f, 1.0f))) - 30.0f) < 1e-3f);

    // Weight 0 and target on top of the bone: nothing changes.
    ik.solves[0].maxAngle = 180.0f;
    ik.solves[0].weight   = 0.0f;
    conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    CHECK(conIk[1] == locales[1]);
    ik.solves[0].weight = 1.0f;
    ik.solves[0].target = glm::vec3(0, 1, 0);
    conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    CHECK(conIk[1] == locales[1]);
}

static void test_ik_twobone_reaches_target()
{
    std::vector<int> padres; std::vector<glm::mat4> locales;
    cadenaDePrueba(locales, padres);
    const auto mundo = componer(locales, padres);
    AnimationIk ik;
    ik.count = 1;
    ik.solves[0] = { 1u, 3, 2, 1, 1.0f, glm::vec3(2, 2, 0), glm::vec3(0), 0u, glm::vec3(0, 0, 1), 80.0f };
    auto conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    const glm::vec3 extremo = glm::vec3(componer(conIk, padres)[3][3]);
    CHECK(glm::length(extremo - glm::vec3(2, 2, 0)) < 1e-3f);

    // Out of reach: the chain stretches toward the target, it does not break.
    ik.solves[0].target = glm::vec3(0, 40, 0);
    conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    const auto mundo2 = componer(conIk, padres);
    const glm::vec3 c = glm::vec3(mundo2[3][3]), a = glm::vec3(mundo2[1][3]);
    CHECK(std::fabs(glm::length(c - a) - 4.0f) < 1e-2f);   // the chain measures 4 except for the kink in Z
    CHECK(glm::length(glm::normalize(c - a) - glm::vec3(0, 1, 0)) < 1e-3f);
    CHECK(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z));

    // Weight 0: the chain stays as it was.
    ik.solves[0].target = glm::vec3(2, 2, 0);
    ik.solves[0].weight = 0.0f;
    conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    CHECK(conIk[1] == locales[1] && conIk[2] == locales[2]);

    // Target on A itself: it touches nothing and gives no NaN.
    ik.solves[0].weight = 1.0f;
    ik.solves[0].target = glm::vec3(0, 0, 0);
    conIk = locales;
    resolverIk(ik, mundo, padres, conIk);
    for (const auto& m : componer(conIk, padres))
        CHECK(std::isfinite(m[3].x) && std::isfinite(m[3].y) && std::isfinite(m[3].z));
}

// Intermediate weights: the chain approaches the target as the
// weight rises, without arriving until it is 1. Without this case, the weight was only measured at
// 0, which exits through the guard above and does not go through the flexion.
static void test_ik_twobone_partial_weight()
{
    std::vector<int> padres; std::vector<glm::mat4> locales;
    cadenaDePrueba(locales, padres);
    const auto mundo = componer(locales, padres);
    const glm::vec3 objetivo(2, 2, 0);
    const float sinIk = glm::length(glm::vec3(mundo[3][3]) - objetivo);
    float anterior = sinIk;
    for (float w : { 0.25f, 0.5f, 0.75f, 1.0f })
    {
        AnimationIk ik;
        ik.count = 1;
        ik.solves[0] = { 1u, 3, 2, 1, w, objetivo, glm::vec3(0), 0u, glm::vec3(0, 0, 1), 80.0f };
        auto conIk = locales;
        resolverIk(ik, mundo, padres, conIk);
        const float d = glm::length(glm::vec3(componer(conIk, padres)[3][3]) - objetivo);
        CHECK(d < anterior - 1e-3f);          // each step gets closer
        anterior = d;
    }
    CHECK(anterior < 1e-3f);                  // with weight 1 it arrives

    // And the ELBOW ANGLE interpolates between the starting one and the arrival one: it is
    // the only thing that measures the flexion weight (the distance to the target is
    // driven by the shoulder's turn, which carries its own weight).
    auto anguloCodo = [&](const std::vector<glm::mat4>& mundo2) {
        const glm::vec3 a(mundo2[1][3]), b(mundo2[2][3]), c(mundo2[3][3]);
        return std::acos(glm::clamp(glm::dot(glm::normalize(a - b), glm::normalize(c - b)), -1.0f, 1.0f));
    };
    const float ang0 = anguloCodo(mundo);
    const float lAB  = glm::length(glm::vec3(mundo[2][3]) - glm::vec3(mundo[1][3]));
    const float lBC  = glm::length(glm::vec3(mundo[3][3]) - glm::vec3(mundo[2][3]));
    const float lAT  = glm::length(objetivo - glm::vec3(mundo[1][3]));
    const float ang1 = std::acos(glm::clamp((lAB * lAB + lBC * lBC - lAT * lAT) / (2.0f * lAB * lBC), -1.0f, 1.0f));
    for (float w : { 0.25f, 0.5f, 0.75f })
    {
        AnimationIk ik;
        ik.count = 1;
        ik.solves[0] = { 1u, 3, 2, 1, w, objetivo, glm::vec3(0), 0u, glm::vec3(0, 0, 1), 80.0f };
        auto conIk = locales;
        resolverIk(ik, mundo, padres, conIk);
        CHECK(std::fabs(anguloCodo(componer(conIk, padres)) - (ang0 + w * (ang1 - ang0))) < 0.02f);
    }
}

static void test_ik_twobone_pole_decides_the_plane()
{
    std::vector<int> padres; std::vector<glm::mat4> locales;
    cadenaDePrueba(locales, padres);
    const auto mundo = componer(locales, padres);
    AnimationIk ik;
    ik.count = 1;
    ik.solves[0] = { 1u, 3, 2, 1, 1.0f, glm::vec3(2, 2, 0), glm::vec3(5, 2, 0), 1u, glm::vec3(0, 0, 1), 80.0f };
    auto conZ = locales;
    resolverIk(ik, mundo, padres, conZ);
    const glm::vec3 codoZ = glm::vec3(componer(conZ, padres)[2][3]);
    ik.solves[0].pole = glm::vec3(-5, 2, 0);
    auto conMenosZ = locales;
    resolverIk(ik, mundo, padres, conMenosZ);
    const glm::vec3 codoMenosZ = glm::vec3(componer(conMenosZ, padres)[2][3]);
    // The two elbows fall on opposite sides, and the end effector arrives all the same.
    // Each elbow falls on the side of ITS pole: the criterion is the distance to the pole,
    // not a specific axis. With this target the two valid solutions are
    // (2,0,0) and (0,2,0), and neither has a negative x.
    const glm::vec3 poleZ(5, 2, 0), poleMenosZ(-5, 2, 0);
    CHECK(glm::length(codoZ - poleZ) < glm::length(codoMenosZ - poleZ));
    CHECK(glm::length(codoMenosZ - poleMenosZ) < glm::length(codoZ - poleMenosZ));
    CHECK(glm::length(codoZ - codoMenosZ) > 1.0f);
    CHECK(glm::length(glm::vec3(componer(conZ, padres)[3][3]) - glm::vec3(2, 2, 0)) < 1e-3f);
    CHECK(glm::length(glm::vec3(componer(conMenosZ, padres)[3][3]) - glm::vec3(2, 2, 0)) < 1e-3f);
}

static void test_ik_block_layout()
{
    AnimationIk ik;
    ik.count = 2;
    ik.solves[0] = { 0u, 5, 1, -1, 0.5f, glm::vec3(1, 2, 3), glm::vec3(0), 0u, glm::vec3(0, 1, 0), 55.0f };
    ik.solves[1] = { 1u, 4, 3, 2, 1.0f, glm::vec3(7, 8, 9), glm::vec3(4, 5, 6), 1u, glm::vec3(0, 0, 1), 80.0f };
    std::vector<uint32_t> b(ikBlockUints(), 0xDEADBEEFu);
    writeIkBlock(ik, b.data());
    auto f = [&](uint32_t i) { float v; std::memcpy(&v, &b[i], 4); return v; };
    CHECK(b[0] == 2u);
    CHECK(b[4] == 0u && (int)b[5] == 5 && (int)b[6] == 1 && (int)b[7] == -1);
    CHECK(nearlyEqual(f(8), 0.5f) && nearlyEqual(f(9), 1.0f) && nearlyEqual(f(11), 3.0f));
    CHECK(b[15] == 0u && nearlyEqual(f(17), 1.0f) && nearlyEqual(f(19), 55.0f));
    CHECK(b[20] == 1u && (int)b[23] == 2 && b[31] == 1u);
    CHECK(nearlyEqual(f(28), 4.0f) && nearlyEqual(f(35), 80.0f));
    // Constraint 3 is left at zero, not with garbage.
    CHECK(b[36] == 0u && b[52] == 0u);
}

static void test_apply_skinned_frame_passes_ik_in_model_space()
{
    Scene scene("Test");
    auto a = std::make_shared<AnimatorComponent>(makeIkAnimator());
    GameObject* go = makeSkinnedGameObject(scene, a);
    go->setMesh(std::make_shared<SkinnedMesh>(makeIkSkeleton()));
    a->bindClips(*go->getSkinnedMesh(), nullptr);
    GameObject* objetivo = scene.addGameObject("Objetivo");
    objetivo->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0));
    a->setIkTarget("mirar", objetivo->id);
    a->ikConstraintsMutable()[1].weight = 0.0f;      // the other one does not travel
    // The displaced character: the target arrives in MODEL SPACE.
    go->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(4, 0, 0));
    scene.getRoot().updateWorldTransforms();

    SkinnedRendererDoble r;
    applySkinnedFrame(*go, r, 0.016f, /*evaluateTransitions=*/true);
    CHECK(r.ik.count == 1);
    if (r.ik.count != 1) return;
    CHECK(r.ik.solves[0].type == 0u && r.ik.solves[0].bone == 5);
    CHECK(glm::length(r.ik.solves[0].target - glm::vec3(6, 0, 0)) < 1e-4f);

    // Without a resolvable target nothing travels, but the call is made all the same.
    a->setIkTarget("mirar", 999999);
    applySkinnedFrame(*go, r, 0.016f, /*evaluateTransitions=*/true);
    CHECK(r.ikRecibida && r.ik.count == 0);
}

static void test_property_track_sampling()
{
    PropertyTrack t;
    t.property = PropertyId::PositionY;
    CHECK(nearlyEqual(samplePropertyTrack(t, 0.5f, 7.0f), 7.0f));       // no keys: the current one
    t.keys = { { 1.0f, 10.0f } };
    CHECK(nearlyEqual(samplePropertyTrack(t, 0.0f, 0.0f), 10.0f));      // a single key
    CHECK(nearlyEqual(samplePropertyTrack(t, 5.0f, 0.0f), 10.0f));
    t.keys = { { 1.0f, 10.0f }, { 3.0f, 30.0f } };
    CHECK(nearlyEqual(samplePropertyTrack(t, 0.0f, 0.0f), 10.0f));      // before the first one
    CHECK(nearlyEqual(samplePropertyTrack(t, 1.0f, 0.0f), 10.0f));      // right on a key
    CHECK(nearlyEqual(samplePropertyTrack(t, 2.0f, 0.0f), 20.0f));      // linear
    CHECK(nearlyEqual(samplePropertyTrack(t, 3.0f, 0.0f), 30.0f));
    CHECK(nearlyEqual(samplePropertyTrack(t, 9.0f, 0.0f), 30.0f));      // after the last one
    // Unordered keys: the sampling CANNOT depend on the file's order.
    t.keys = { { 3.0f, 30.0f }, { 1.0f, 10.0f } };
    CHECK(nearlyEqual(samplePropertyTrack(t, 2.0f, 0.0f), 20.0f));
}

static void test_property_blend_short_path()
{
    const PropertyContribution mitad[2] = { { 0.0f, 0.5f }, { 10.0f, 0.5f } };
    CHECK(nearlyEqual(blendPropertyValues(PropertyId::PositionX, mitad, 2), 5.0f));
    // Rotation: 350 and 10 are 20 degrees apart, not 340.
    const PropertyContribution giro[2] = { { 350.0f, 0.5f }, { 10.0f, 0.5f } };
    const float r = blendPropertyValues(PropertyId::RotationY, giro, 2);
    CHECK(std::fabs(std::remainder(r - 0.0f, 360.0f)) < 1e-3f);
    // Weights that do not sum to 1 (a layer at half power): they are renormalized.
    const PropertyContribution parcial[2] = { { 0.0f, 0.25f }, { 8.0f, 0.25f } };
    CHECK(nearlyEqual(blendPropertyValues(PropertyId::PositionX, parcial, 2), 4.0f));
    CHECK(nearlyEqual(blendPropertyValues(PropertyId::PositionX, mitad, 0), 0.0f));   // nothing
}

static void test_property_names_round_trip()
{
    for (int i = 0; i < (int)PropertyId::Count; i++)
    {
        const PropertyId id = (PropertyId)i;
        CHECK(propertyFromName(propertyName(id)) == id);
    }
    CHECK(propertyFromName("noExiste") == PropertyId::Count);
    CHECK(propertyIsRotation(PropertyId::RotationZ) && !propertyIsRotation(PropertyId::ScaleX));
}

static void test_property_get_set_transform()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Puerta");
    go->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(1, 2, 3));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::PositionY), 2.0f));
    CHECK(propertyAvailable(*go, PropertyId::PositionY));

    bool  escritas[(int)PropertyId::Count] = {};
    float valores [(int)PropertyId::Count] = {};
    escritas[(int)PropertyId::PositionY] = true; valores[(int)PropertyId::PositionY] = 9.0f;
    escritas[(int)PropertyId::RotationZ] = true; valores[(int)PropertyId::RotationZ] = 90.0f;
    escritas[(int)PropertyId::ScaleX]    = true; valores[(int)PropertyId::ScaleX]    = 2.0f;
    propertyApply(*go, escritas, valores);
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::PositionY), 9.0f));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::PositionX), 1.0f));   // what was not written does NOT change
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::PositionZ), 3.0f));
    CHECK(std::fabs(std::remainder(propertyGet(*go, PropertyId::RotationZ) - 90.0f, 360.0f)) < 1e-3f);
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::ScaleX), 2.0f));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::ScaleY), 1.0f));
    // The transform's X axis, already rotated 90 degrees in Z, points to +Y.
    CHECK(glm::length(glm::normalize(glm::vec3(go->localTransform[0])) - glm::vec3(0, 1, 0)) < 1e-4f);
}

static void test_property_light_and_material()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Farola");
    CHECK(!propertyAvailable(*go, PropertyId::LightIntensity));   // without LightComponent
    go->setLight(std::make_shared<LightComponent>());
    go->getLight()->setIntensity(2.0f);
    go->getLight()->setColor(glm::vec3(1, 0, 0));
    CHECK(propertyAvailable(*go, PropertyId::LightIntensity));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::LightIntensity), 2.0f));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::LightColorG), 0.0f));

    bool  escritas[(int)PropertyId::Count] = {};
    float valores [(int)PropertyId::Count] = {};
    escritas[(int)PropertyId::LightIntensity] = true; valores[(int)PropertyId::LightIntensity] = 5.0f;
    escritas[(int)PropertyId::LightColorG]    = true; valores[(int)PropertyId::LightColorG]    = 1.0f;
    escritas[(int)PropertyId::MaterialMetallic] = true; valores[(int)PropertyId::MaterialMetallic] = 0.75f;
    propertyApply(*go, escritas, valores);
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::LightIntensity), 5.0f));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::LightColorG), 1.0f));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::LightColorR), 1.0f));   // what was not written stays
    // The material goes through the OBJECT's override, not through the mesh: animating
    // cannot copy the mesh every frame (editMesh copies if it is shared).
    CHECK(!go->materialOverrides.empty());
    if (!go->materialOverrides.empty())
        CHECK(nearlyEqual(go->materialOverrides[0].metallic, 0.75f));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::MaterialMetallic), 0.75f));
}

// Door: Cerrada (clip "cerrar") -> Abierta (clip "abrir") by trigger.
static AnimatorComponent makePuerta()
{
    AnimatorComponent a;
    PropertyClip cerrar;
    cerrar.name = "cerrar"; cerrar.duration = 1.0f;
    PropertyTrack tc; tc.property = PropertyId::PositionY;
    tc.keys = { { 0.0f, 0.0f }, { 1.0f, 0.0f } };
    cerrar.tracks.push_back(tc);
    PropertyClip abrir;
    abrir.name = "abrir"; abrir.duration = 2.0f;
    PropertyTrack ta; ta.property = PropertyId::PositionY;
    ta.keys = { { 0.0f, 0.0f }, { 2.0f, 4.0f } };
    abrir.tracks.push_back(ta);
    a.addPropertyClip(cerrar);
    a.addPropertyClip(abrir);
    AnimatorComponent::State s;
    s.name = "Cerrada"; s.propertyClipName = "cerrar";
    a.addState(s);
    s.name = "Abierta"; s.propertyClipName = "abrir";
    a.addState(s);
    a.setEntryState(0);
    a.addParameter("abre", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 0.5f;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "abre";
    t.conditions.push_back(c);
    a.addTransition(t);
    return a;
}

static void test_property_clip_binding_and_duration()
{
    AnimatorComponent a = makePuerta();
    std::vector<std::string> avisos;
    a.bindProperties(nullptr, &avisos);
    CHECK(avisos.empty());
    CHECK(a.states()[1].propertyClipIndex == 1);
    // Without a mesh clip, the state's duration comes from the property clip.
    CHECK(a.states()[1].ticksPerSecond > 0.0f);
    CHECK(nearlyEqual(a.states()[1].duration / a.states()[1].ticksPerSecond, 2.0f));
    // A name that does not exist warns and leaves the state without a clip.
    a.statesMutable()[0].propertyClipName = "noExiste";
    avisos.clear();
    a.bindProperties(nullptr, &avisos);
    CHECK(a.states()[0].propertyClipIndex == -1);
    CHECK(avisos.size() == 1u);
    CHECK(a.addPropertyClip(PropertyClip{}) >= 0);
    while (a.propertyClips().size() < (size_t)AnimatorComponent::kMaxPropertyClips)
        CHECK(a.addPropertyClip(PropertyClip{}) >= 0);
    CHECK(a.addPropertyClip(PropertyClip{}) == -1);
}

static void test_property_clip_tracks_resolved_against_object()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Farola");
    AnimatorComponent a;
    PropertyClip parpadeo;
    parpadeo.name = "parpadeo"; parpadeo.duration = 1.0f;
    PropertyTrack luz; luz.property = PropertyId::LightIntensity;
    luz.keys = { { 0.0f, 0.0f }, { 1.0f, 5.0f } };
    PropertyTrack pos; pos.property = PropertyId::PositionY;
    pos.keys = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
    parpadeo.tracks = { luz, pos };
    a.addPropertyClip(parpadeo);
    std::vector<std::string> avisos;
    a.bindProperties(go, &avisos);
    CHECK(!a.propertyClips()[0].tracks[0].resolved);   // there is no LightComponent
    CHECK(a.propertyClips()[0].tracks[1].resolved);
    CHECK(avisos.size() == 1u);
    go->setLight(std::make_shared<LightComponent>());
    avisos.clear();
    a.bindProperties(go, &avisos);
    CHECK(a.propertyClips()[0].tracks[0].resolved);
    CHECK(avisos.empty());
}

// A curve is a track with a parameter target: it is resolved against the
// DECLARED parameters, not against the object's components.
static void test_curve_track_resolves_against_float_parameter()
{
    AnimatorComponent a;
    a.addParameter("velocidad", AnimatorComponent::ParamType::Float);
    a.addParameter("vivo",      AnimatorComponent::ParamType::Bool);
    PropertyClip c;
    c.name = "curvas"; c.duration = 1.0f;
    PropertyTrack ok;   ok.target   = TrackTarget::Parameter; ok.parameterName   = "velocidad";
    ok.keys = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
    PropertyTrack tipo; tipo.target = TrackTarget::Parameter; tipo.parameterName = "vivo";
    tipo.keys = { { 0.0f, 0.0f } };
    PropertyTrack no;   no.target   = TrackTarget::Parameter; no.parameterName   = "noExiste";
    no.keys = { { 0.0f, 0.0f } };
    PropertyTrack vacia; vacia.target = TrackTarget::Parameter;
    vacia.keys = { { 0.0f, 0.0f } };
    c.tracks = { ok, tipo, no, vacia };
    a.addPropertyClip(c);

    CHECK(a.hasFloatParameter("velocidad"));
    CHECK(!a.hasFloatParameter("vivo"));
    CHECK(!a.hasFloatParameter("noExiste"));

    std::vector<std::string> avisos;
    a.bindProperties(nullptr, &avisos);
    CHECK(a.propertyClips()[0].tracks[0].resolved);
    CHECK(!a.propertyClips()[0].tracks[1].resolved);   // it exists, but it is Bool
    CHECK(!a.propertyClips()[0].tracks[2].resolved);
    CHECK(!a.propertyClips()[0].tracks[3].resolved);   // no name
    CHECK(avisos.size() == 3u);
    // The default target is still the property: a track from before does not
    // change meaning.
    CHECK(PropertyTrack{}.target == TrackTarget::Property);
}

// Single state with a curve 0 -> 10 over 1 s on the parameter "velocidad".
static AnimatorComponent makeCurvaVelocidad()
{
    AnimatorComponent a;
    a.addParameter("velocidad", AnimatorComponent::ParamType::Float);
    PropertyClip c;
    c.name = "acelera"; c.duration = 1.0f;
    PropertyTrack cur; cur.target = TrackTarget::Parameter; cur.parameterName = "velocidad";
    cur.keys = { { 0.0f, 0.0f }, { 1.0f, 10.0f } };
    c.tracks.push_back(cur);
    a.addPropertyClip(c);
    AnimatorComponent::State s;
    s.name = "Arranca"; s.propertyClipName = "acelera";
    a.addState(s);
    a.setEntryState(0);
    a.bindProperties(nullptr, nullptr);
    return a;
}

static void test_curve_writes_the_parameter()
{
    AnimatorComponent a = makeCurvaVelocidad();
    a.update(0.25f, true);
    CHECK(nearlyEqual(a.getFloat("velocidad"), 2.5f));
    a.update(0.25f, true);
    CHECK(nearlyEqual(a.getFloat("velocidad"), 5.0f));
    // An unresolved curve does not write: the parameter stays as it was.
    AnimatorComponent b = makeCurvaVelocidad();
    b.propertyClipsMutable()[0].tracks[0].parameterName = "otro";
    b.bindProperties(nullptr, nullptr);
    b.setFloat("velocidad", -1.0f);
    b.update(0.5f, true);
    CHECK(nearlyEqual(b.getFloat("velocidad"), -1.0f));
}

static void test_curve_blends_during_cross_fade()
{
    AnimatorComponent a = makeCurvaVelocidad();
    // Second state with a constant curve at 0 and a transition by trigger.
    PropertyClip frena;
    frena.name = "frena"; frena.duration = 1.0f;
    PropertyTrack cur; cur.target = TrackTarget::Parameter; cur.parameterName = "velocidad";
    cur.keys = { { 0.0f, 0.0f }, { 1.0f, 0.0f } };
    frena.tracks.push_back(cur);
    a.addPropertyClip(frena);
    AnimatorComponent::State s;
    s.name = "Frena"; s.propertyClipName = "frena";
    a.addState(s);
    a.addParameter("para", AnimatorComponent::ParamType::Trigger);
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 1.0f;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "para";
    t.conditions.push_back(c);
    a.addTransition(t);
    a.bindProperties(nullptr, nullptr);

    // It starts with a CONSTANT curve at 10: that way what is measured is the blend and not
    // where each clock falls (the state loops and its ramp would give the wrap).
    a.propertyClipsMutable()[0].tracks[0].keys = { { 0.0f, 10.0f }, { 1.0f, 10.0f } };
    a.update(0.5f, true);
    CHECK(nearlyEqual(a.getFloat("velocidad"), 10.0f));
    a.setTrigger("para");
    a.update(0.0f, true);                 // the fade starts, the destination weighs 0
    a.update(0.5f, true);                 // halfway through the fade: 0,5*10 + 0,5*0
    CHECK(nearlyEqual(a.getFloat("velocidad"), 5.0f));
    a.update(0.5f, true);                 // fade finished: Frena rules
    CHECK(nearlyEqual(a.getFloat("velocidad"), 0.0f));
}

static void test_curve_fires_its_transition_in_the_same_frame()
{
    AnimatorComponent a = makeCurvaVelocidad();
    AnimatorComponent::State s;
    s.name = "Corre";
    a.addState(s);
    AnimatorComponent::Transition t;
    t.fromState = 0; t.toState = 1; t.duration = 0.0f;
    AnimatorComponent::Condition c;
    c.type = AnimatorComponent::ConditionType::Float;
    c.paramName = "velocidad"; c.compare = AnimatorComponent::Compare::Greater; c.threshold = 4.0f;
    t.conditions.push_back(c);
    a.addTransition(t);
    a.bindProperties(nullptr, nullptr);
    // At t=0,5 the curve is worth 5: the transition fires in THIS update, not in the
    // next one.
    a.update(0.5f, true);
    CHECK(a.currentState() == 1);
}

static void test_curve_of_a_zero_weight_layer_still_writes()
{
    AnimatorComponent a = makeCurvaVelocidad();
    // A layer with weight 0 is not seen, but its machine runs: its curve is
    // information, not pose.
    CHECK(a.addLayer("Info") == 1);
    a.setLayerWeight(1, 0.0f);
    a.addParameter("info", AnimatorComponent::ParamType::Float);
    PropertyClip c;
    c.name = "infoClip"; c.duration = 1.0f;
    PropertyTrack cur; cur.target = TrackTarget::Parameter; cur.parameterName = "info";
    cur.keys = { { 0.0f, 0.0f }, { 1.0f, 8.0f } };
    c.tracks.push_back(cur);
    a.addPropertyClip(c);
    AnimatorComponent::State s;
    s.name = "Info"; s.propertyClipName = "infoClip";
    a.addState(s, 1);
    a.setEntryState(0, 1);
    a.bindProperties(nullptr, nullptr);
    a.update(0.5f, true);
    CHECK(nearlyEqual(a.getFloat("info"), 4.0f));
}

static void test_curve_last_layer_wins()
{
    AnimatorComponent a = makeCurvaVelocidad();
    CHECK(a.addLayer("Encima") == 1);
    a.setLayerWeight(1, 1.0f);
    PropertyClip c;
    c.name = "encima"; c.duration = 1.0f;
    PropertyTrack cur; cur.target = TrackTarget::Parameter; cur.parameterName = "velocidad";
    cur.keys = { { 0.0f, 100.0f }, { 1.0f, 100.0f } };
    c.tracks.push_back(cur);
    a.addPropertyClip(c);
    AnimatorComponent::State s;
    s.name = "Encima"; s.propertyClipName = "encima";
    a.addState(s, 1);
    a.setEntryState(0, 1);
    a.bindProperties(nullptr, nullptr);
    a.update(0.5f, true);
    // Layers are walked in order: the last one writes.
    CHECK(nearlyEqual(a.getFloat("velocidad"), 100.0f));
}

// The layer's weight DOES scale what is applied to the object (unlike
// curves, which are written as they are).
// What the panel needs to draw a curve: against which thresholds it is read and
// with what vertical range.
// The clock of the path WITHOUT an Animator, which both backends share since
// it was discovered that they had diverged (A13).
// Base -> box "Ataques" { Golpe, box "Combo" { Uno } }
static AnimatorComponent makeCajas()
{
    AnimatorComponent a;
    AnimatorComponent::State s;
    s.name = "Base";      a.addState(s);                                             // 0
    s = {}; s.name = "Ataques"; s.isSubMachine = true;             a.addState(s);    // 1
    s = {}; s.name = "Golpe";   s.parent = 1;                      a.addState(s);    // 2
    s = {}; s.name = "Combo";   s.parent = 1; s.isSubMachine = true; a.addState(s);  // 3
    s = {}; s.name = "Uno";     s.parent = 3;                      a.addState(s);    // 4
    a.statesMutable()[1].subEntry = 3;   // Ataques enters through Combo
    a.statesMutable()[3].subEntry = 4;   // Combo enters through Uno
    a.setEntryState(0);
    return a;
}

// A9, the case that no repo asset produces: a vertex that the FBX did not weight
// against any bone.
static void test_normalize_bone_weights()
{
    float out[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
    const float dos[4] = { 2.0f, 2.0f, 0.0f, 0.0f };
    CHECK(normalizeBoneWeights(dos, out));
    CHECK(nearlyEqual(out[0], 0.5f) && nearlyEqual(out[1], 0.5f));
    CHECK(nearlyEqual(out[2], 0.0f) && nearlyEqual(out[3], 0.0f));

    // Already normalized: it does not touch them.
    const float uno[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    CHECK(normalizeBoneWeights(uno, out));
    CHECK(nearlyEqual(out[0], 1.0f));

    // Without weights: false and FOUR ZEROS. It is the data that makes the shader
    // have to use identity instead of the zeroed matrix.
    const float nada[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    CHECK(!normalizeBoneWeights(nada, out));
    for (int i = 0; i < 4; i++) CHECK(nearlyEqual(out[i], 0.0f));
}

// A9: the loader normalizes each vertex's weights to 1, and leaves FOUR ZEROS
// when the FBX did not weight that vertex against any bone. The latter is counted by
// verticesWithoutWeights, because nobody moves such a vertex: the shader
// leaves it in place (identity) instead of sending it to the origin.
static void test_loader_normalizes_bone_weights()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    CHECK(!m.skinnedVertices.empty());
    int cero = 0;
    float minSuma = 1e9f, maxSuma = 0.0f;
    for (const auto& v : m.skinnedVertices)
    {
        float s = 0.0f;
        for (int i = 0; i < 4; i++)
        {
            CHECK(v.boneWeights[i] >= 0.0f);          // a negative weight would give an impossible pose
            s += v.boneWeights[i];
        }
        if (s <= 0.0f) cero++;
        else { minSuma = std::min(minSuma, s); maxSuma = std::max(maxSuma, s); }
    }
    // The repo asset is well weighted: if someone swaps it for one that is not, this
    // says so instead of showing up as strange geometry on screen.
    CHECK(cero == 0);
    CHECK(m.verticesWithoutWeights == cero);
    CHECK(nearlyEqual(minSuma, 1.0f));
    CHECK(nearlyEqual(maxSuma, 1.0f));

    // The other half of A9: normals are transformed with `skin` and not with its
    // inverse transpose, which is only incorrect with NON-uniform scale in
    // some bone. Measured: this rig has none, so that inverse is not paid per
    // vertex. If a rig ever brings one, this test says so.
    int noUniformes = 0;
    for (const auto& clip : m.animationClips)
        for (const auto& ch : clip.channels)
            for (const auto& k : ch.scaleKeys)
                if (std::fabs(k.value.x - k.value.y) > 1e-3f ||
                    std::fabs(k.value.y - k.value.z) > 1e-3f) noUniformes++;
    CHECK(noUniformes == 0);
}

// A10: the graph can be written raw from the UI (statesMutable /
// transitionsMutable). sanitizeGraph is the pass that leaves usable whatever
// comes in, whether from the editor or from a tampered .scene.
// The panel remembers by editorId the sub-machine it is inside, NOT by
// index: deleting an earlier node reindexes the vector and with a stored index
// the editor left the box by itself. This is the invariant that holds it up.
// The canvas ids: encoding and decoding has to close the circle for
// EACH of the five slots. A failure here gives no compile error (it
// shows up as "I delete one node and another disappears"), so it is measured.
static void test_canvas_ids_round_trip()
{
    using namespace canvasIds;
    for (int eid = 0; eid < 64; eid++)
    {
        const int ids[5] = { node(eid), inputPin(eid), outputPin(eid), inputPin2(eid), outputPin2(eid) };
        for (int k = 0; k < 5; k++) CHECK(editorIdFrom(ids[k]) == eid);
        // No slot is confused with another, nor with that of another state.
        for (int k = 0; k < 5; k++)
            for (int j = k + 1; j < 5; j++) CHECK(ids[k] != ids[j]);
        if (eid > 0) CHECK(node(eid) != outputPin2(eid - 1));

        // Input and output, well classified in both pairs.
        CHECK(!isOutputPin(inputPin(eid)));
        CHECK(isOutputPin(outputPin(eid)));
        CHECK(!isOutputPin(inputPin2(eid)));
        CHECK(isOutputPin(outputPin2(eid)));
        // And the secondary pair is told apart from the normal one.
        CHECK(!isSecondaryPin(inputPin(eid)) && !isSecondaryPin(outputPin(eid)));
        CHECK(isSecondaryPin(inputPin2(eid)) && isSecondaryPin(outputPin2(eid)));
    }

    // Link ids live in their own range, far from the state ones: with
    // 100000 it would take 20,000 states to collide.
    CHECK(link(0) > outputPin2(19000));
    // Any State is outside the scheme and its decoded editorId is
    // unreachable, which is what allows checking it by equality before
    // decoding.
    CHECK(editorIdFrom(kAnyStateNode) > 100000);
    CHECK(kAnyStateNode != node(0) && kAnyStateOutPin != outputPin(0));
}

static void test_editor_id_survives_deleting_an_earlier_state()
{
    AnimatorComponent a = makeCajas();       // Base(0), Ataques(1, box), Golpe(2), Combo(3, box), Uno(4)
    const int idCaja = a.states()[1].editorId;
    CHECK(a.stateIndexByEditorId(idCaja, 0) == 1);
    a.removeState(0, 0);                     // deletes Base, which goes BEFORE the box
    // Same state, new index: the id is the only thing that does not move.
    CHECK(a.stateIndexByEditorId(idCaja, 0) == 0);
    CHECK(a.states()[0].isSubMachine);
    CHECK(a.states()[0].name == "Ataques");
    // And deleting the whole box does leave the id unresolved: that is when the panel
    // has to go back to the root.
    a.removeState(0, 0);
    CHECK(a.stateIndexByEditorId(idCaja, 0) == -1);
}

static void test_sanitize_graph_fixes_bad_indices()
{
    AnimatorComponent a = makeCajas();
    a.addParameter("t", AnimatorComponent::ParamType::Trigger);
    auto liga = [&](int from, int to) {
        AnimatorComponent::Transition tr;
        tr.fromState = from; tr.toState = to;
        AnimatorComponent::Condition c;
        c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "t";
        tr.conditions.push_back(c);
        a.addTransition(tr);
    };
    liga(0, 2);      // good
    liga(0, 99);     // destination out of range
    liga(42, 0);     // origin out of range
    liga(AnimatorComponent::kAnyState, 1);   // Any State: the sentinel is NOT garbage

    // Hierarchy broken by hand, as a widget with a bug would leave it.
    a.statesMutable()[2].parent   = 99;    // nonexistent parent
    a.statesMutable()[4].parent   = 0;     // Base is not a box
    a.statesMutable()[1].subEntry = 77;    // nonexistent entry

    std::vector<std::string> avisos;
    a.sanitizeGraph(0, &avisos);

    // The good one and the Any State one remain; the two with impossible indices go away.
    CHECK(a.transitions().size() == 2u);
    for (const auto& t : a.transitions())
    {
        CHECK(t.toState >= 0 && t.toState < (int)a.states().size());
        CHECK(t.fromState == AnimatorComponent::kAnyState ||
              (t.fromState >= 0 && t.fromState < (int)a.states().size()));
    }
    CHECK(a.states()[2].parent == -1);
    CHECK(a.states()[4].parent == -1);
    CHECK(a.states()[1].subEntry == -1);
    CHECK(avisos.size() >= 5u);

    // An entry that EXISTS but is not a child of the box: entering it would take you
    // out of the block, which is the opposite of what it means. The range
    // guard does not cover this case.
    AnimatorComponent b = makeCajas();
    b.statesMutable()[1].subEntry = 0;    // Base is a root, not a child of Ataques
    avisos.clear();
    b.sanitizeGraph(0, &avisos);
    CHECK(b.states()[1].subEntry == -1);
    CHECK(!avisos.empty());

    // Containment cycle: going up through parent has to terminate.
    AnimatorComponent c = makeCajas();
    c.statesMutable()[1].parent = 3;      // Ataques inside Combo, which is already inside Ataques
    avisos.clear();
    c.sanitizeGraph(0, &avisos);
    CHECK(!avisos.empty());
    CHECK(c.states()[1].parent == -1 || c.states()[3].parent == -1);
}

static void test_sanitize_graph_leaves_a_good_graph_alone()
{
    AnimatorComponent a = makeCajas();
    a.addParameter("t", AnimatorComponent::ParamType::Trigger);
    {
        AnimatorComponent::Transition tr;
        tr.fromState = 0; tr.toState = 1;
        AnimatorComponent::Condition c;
        c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = "t";
        tr.conditions.push_back(c);
        a.addTransition(tr);
        tr.fromState = AnimatorComponent::kAnyState; tr.toState = 2;
        a.addTransition(tr);
    }
    const std::string antes = animatorToJson(a).dump();
    std::vector<std::string> avisos;
    a.sanitizeGraph(0, &avisos);
    // A healthy graph does not change A SINGLE FIELD and does not generate a single warning: otherwise,
    // calling it after every editor edit would corrupt the graph little by
    // little.
    CHECK(avisos.empty());
    CHECK(animatorToJson(a).dump() == antes);
}

static void test_submachine_hierarchy_helpers()
{
    AnimatorComponent a = makeCajas();
    CHECK(a.isDescendantOf(4, 1, 0));    // Uno is inside Ataques (two levels)
    CHECK(a.isDescendantOf(2, 1, 0));
    CHECK(!a.isDescendantOf(0, 1, 0));   // Base no
    CHECK(!a.isDescendantOf(1, 1, 0));   // one is not its own descendant
    // Entering a box goes down to the leaf.
    CHECK(a.resolveEntryLeaf(1, 0) == 4);
    CHECK(a.resolveEntryLeaf(2, 0) == 2);   // a leaf resolves to itself
    // Empty box: there is no leaf, and that is NOT entering halfway.
    a.statesMutable()[3].subEntry = -1;
    CHECK(a.resolveEntryLeaf(1, 0) == -1);
    // A subEntry cycle cannot hang the engine.
    a.statesMutable()[3].subEntry = 1;
    CHECK(a.resolveEntryLeaf(1, 0) == -1);
    // The current state is never a box: entering through the layer's entry
    // (setEntryState -> resetPlayback -> enterState) resolves the leaf.
    AnimatorComponent b = makeCajas();
    b.setEntryState(1, 0);
    CHECK(b.currentState() == 4);
    b.setEntryState(0, 0);
    CHECK(b.currentState() == 0);
    b.statesMutable()[3].subEntry = -1;
    b.setEntryState(1, 0);
    CHECK(b.currentState() == 0);    // broken box: it does not move
}

// makeCajas + a Base -> Ataques transition and another Ataques -> Base, by trigger.
static AnimatorComponent makeCajasConTransiciones()
{
    AnimatorComponent a = makeCajas();
    a.addParameter("entra", AnimatorComponent::ParamType::Trigger);
    a.addParameter("sale",  AnimatorComponent::ParamType::Trigger);
    auto liga = [&](int from, int to, const char* trigger) {
        AnimatorComponent::Transition t;
        t.fromState = from; t.toState = to; t.duration = 0.0f;
        AnimatorComponent::Condition c;
        c.type = AnimatorComponent::ConditionType::Trigger; c.paramName = trigger;
        t.conditions.push_back(c);
        a.addTransition(t);
    };
    liga(0, 1, "entra");    // toward the box
    liga(1, 0, "sale");     // from the box
    return a;
}

static void test_submachine_transition_enters_the_leaf()
{
    AnimatorComponent a = makeCajasConTransiciones();
    a.reset();
    CHECK(a.currentState() == 0);
    a.setTrigger("entra");
    a.update(0.016f, true);
    // Ataques -> Combo -> Uno: it enters the leaf, not the box.
    CHECK(a.currentState() == 4);
}

static void test_submachine_broken_entry_does_not_fire()
{
    AnimatorComponent a = makeCajasConTransiciones();
    a.statesMutable()[3].subEntry = -1;    // Combo empty
    a.reset();
    a.setTrigger("entra");
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);          // it does not enter halfway

    // And the transition is not even CHOSEN: if it were chosen, the trigger would be
    // consumed (and the cross-fade would start against a state that does not change).
    // With the box already fixed, the same trigger (without arming it again) has
    // to enter.
    a.statesMutable()[3].subEntry = 4;
    a.update(0.016f, true);
    CHECK(a.currentState() == 4);
}

static void test_submachine_exit_fires_from_any_leaf()
{
    AnimatorComponent a = makeCajasConTransiciones();
    a.reset();
    a.setTrigger("entra");
    a.update(0.016f, true);
    CHECK(a.currentState() == 4);          // inside, two levels down
    a.setTrigger("sale");
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);          // the transition FROM the box applies
    // And it does not fire from outside: while in Base, "sale" does nothing.
    a.setTrigger("sale");
    a.update(0.016f, true);
    CHECK(a.currentState() == 0);
}

static void test_submachine_leaf_transition_wins_over_ancestor()
{
    AnimatorComponent a = makeCajasConTransiciones();
    AnimatorComponent::State s;
    s.name = "Otro";
    const int otro = a.addState(s);        // 5, at the root
    a.addParameter("ya", AnimatorComponent::ParamType::Bool);
    auto liga = [&](int from, int to) {
        AnimatorComponent::Transition t;
        t.fromState = from; t.toState = to; t.duration = 0.0f;
        AnimatorComponent::Condition c;
        c.type = AnimatorComponent::ConditionType::Bool; c.paramName = "ya"; c.expected = true;
        t.conditions.push_back(c);
        a.addTransition(t);
    };
    // The ANCESTOR's one is declared before the leaf's, on purpose: what
    // decides is the level, not the vector order.
    liga(1, 0);            // from the box -> Base
    liga(4, otro);         // from the leaf -> Otro
    a.reset();
    a.setTrigger("entra");
    a.update(0.016f, true);
    CHECK(a.currentState() == 4);
    a.setBool("ya", true);
    a.update(0.016f, true);
    CHECK(a.currentState() == otro);       // the leaf's one wins
}

static void test_play_resolves_a_submachine()
{
    AnimatorComponent a = makeCajasConTransiciones();
    a.reset();
    CHECK(a.play("Ataques", 0));
    CHECK(a.currentState() == 4);
    // Empty box: there is no leaf to go to.
    AnimatorComponent b = makeCajasConTransiciones();
    b.statesMutable()[3].subEntry = -1;
    b.reset();
    CHECK(!b.play("Ataques", 0));
    CHECK(b.currentState() == 0);
    CHECK(!b.crossFade("Ataques", 0.2f, 0));
}

static void test_removing_a_submachine_removes_its_children()
{
    AnimatorComponent a = makeCajasConTransiciones();
    AnimatorComponent::State s;
    s.name = "Otro";
    a.addState(s);                       // 5, at the root, to see that it survives
    a.removeState(1, 0);                 // deletes Ataques
    // Base and Otro remain: Golpe, Combo and Uno go away with the box.
    CHECK(a.states().size() == 2u);
    if (a.states().size() != 2u) return;
    CHECK(a.states()[0].name == "Base");
    CHECK(a.states()[1].name == "Otro");
    // And no transition is left pointing at what was deleted.
    for (const auto& t : a.transitions())
    {
        CHECK(t.fromState >= -2 && t.fromState < (int)a.states().size());
        CHECK(t.toState   >= 0  && t.toState   < (int)a.states().size());
    }
}

// A child can go BEFORE its box in the vector: the editor lets you put a
// state that already existed into a new box. Deleting the box then has to
// relocate it, because deleting that child shifts it.
static void test_removing_a_submachine_whose_child_comes_first()
{
    AnimatorComponent a;
    AnimatorComponent::State s;
    s.name = "Viejo";   a.addState(s);                                  // 0
    s = {}; s.name = "Testigo"; a.addState(s);                          // 1
    s = {}; s.name = "Caja"; s.isSubMachine = true; a.addState(s);      // 2
    a.statesMutable()[0].parent   = 2;    // Viejo goes into the Box
    a.statesMutable()[2].subEntry = 0;
    a.setEntryState(1);
    a.removeState(2, 0);                  // deletes the Box
    // Caja and Viejo go away; Testigo survives and no other one is deleted because of
    // the shift.
    CHECK(a.states().size() == 1u);
    if (a.states().size() != 1u) return;
    CHECK(a.states()[0].name == "Testigo");
}

static void test_removing_a_state_reindexes_parent_and_entry()
{
    AnimatorComponent a = makeCajas();
    a.removeState(0, 0);                 // deletes Base, which goes BEFORE the boxes
    // Ataques becomes 0, Golpe 1, Combo 2, Uno 3.
    CHECK(a.states().size() == 4u);
    if (a.states().size() != 4u) return;
    CHECK(a.states()[0].name == "Ataques" && a.states()[0].subEntry == 2);
    CHECK(a.states()[1].name == "Golpe"   && a.states()[1].parent   == 0);
    CHECK(a.states()[2].name == "Combo"   && a.states()[2].parent   == 0);
    CHECK(a.states()[2].subEntry == 3);
    CHECK(a.states()[3].name == "Uno"     && a.states()[3].parent   == 2);
    // And the hierarchy keeps meaning the same.
    CHECK(a.resolveEntryLeaf(0, 0) == 3);

    // Deleting the state that WAS its box's entry leaves it empty, not
    // pointing at another state because of the index shift.
    AnimatorComponent b = makeCajas();
    b.removeState(4, 0);                 // Uno, Combo's entry
    CHECK(b.states()[3].name == "Combo" && b.states()[3].subEntry == -1);
    CHECK(b.resolveEntryLeaf(1, 0) == -1);   // Ataques -> Combo -> nothing
}

static void test_mesh_clock_advances_in_ticks()
{
    // dt in SECONDS, clock in TICKS: without the rate, half a second of a clip at
    // 24 fps would advance 0,5 instead of 12, and the character would go 24 times slow.
    CHECK(nearlyEqual(advanceMeshClock(0.0f, 0.5f, 24.0f, 100.0f, true), 12.0f));
    // Wrap: 95 + 12 = 107 over a clip of 100.
    CHECK(nearlyEqual(advanceMeshClock(95.0f, 0.5f, 24.0f, 100.0f, true), 7.0f));
    // Hidden: the clock stands still and resumes where it was.
    CHECK(nearlyEqual(advanceMeshClock(30.0f, 0.5f, 24.0f, 100.0f, false), 30.0f));
    // Without a rate or without a duration there is no clip to sample.
    CHECK(nearlyEqual(advanceMeshClock(30.0f, 0.5f, 0.0f, 100.0f, true), 30.0f));
    CHECK(nearlyEqual(advanceMeshClock(30.0f, 0.5f, 24.0f, 0.0f, true), 30.0f));
}

static void test_curve_condition_thresholds()
{
    AnimatorComponent a = makeCurvaVelocidad();
    AnimatorComponent::State s;
    s.name = "Corre";
    a.addState(s);
    auto liga = [&](AnimatorComponent::ConditionType tipo, const char* nombre, float umbral, int capa) {
        AnimatorComponent::Transition t;
        t.fromState = 0; t.toState = capa == 0 ? 1 : 0;
        AnimatorComponent::Condition c;
        c.type = tipo; c.paramName = nombre; c.threshold = umbral;
        t.conditions.push_back(c);
        a.addTransition(t, capa);
    };
    liga(AnimatorComponent::ConditionType::Float, "velocidad", 4.0f, 0);
    liga(AnimatorComponent::ConditionType::Float, "velocidad", 4.0f, 0);   // repeated: only one
    liga(AnimatorComponent::ConditionType::Float, "velocidad", 9.0f, 0);
    liga(AnimatorComponent::ConditionType::Int,   "velocidad", 7.0f, 0);   // no es Float
    liga(AnimatorComponent::ConditionType::Float, "otro",      1.0f, 0);   // another parameter
    CHECK(a.addLayer("Encima") == 1);
    a.addState(s, 1);
    liga(AnimatorComponent::ConditionType::Float, "velocidad", 2.0f, 1);   // another layer: it counts

    float u[8];
    const int n = a.conditionThresholds("velocidad", u, 8);
    CHECK(n == 3);
    if (n != 3) return;
    bool tiene4 = false, tiene9 = false, tiene2 = false;
    for (int i = 0; i < n; i++)
    {
        tiene4 = tiene4 || nearlyEqual(u[i], 4.0f);
        tiene9 = tiene9 || nearlyEqual(u[i], 9.0f);
        tiene2 = tiene2 || nearlyEqual(u[i], 2.0f);
    }
    CHECK(tiene4 && tiene9 && tiene2);
    // The cap is respected: with room for one, one.
    CHECK(a.conditionThresholds("velocidad", u, 1) == 1);
    CHECK(a.conditionThresholds("noExiste", u, 8) == 0);
}

// C15: dragging a key is converting screen <-> data. The edges and the
// inversion of the Y axis are where the off-by-ones live.
static void test_curve_canvas_conversions()
{
    // Canvas of 200x50 at (100, 20). Clip of 2 s, values from -1 to 3.
    const float x0 = 100.0f, x1 = 300.0f, y0 = 20.0f, y1 = 70.0f;
    const float dur = 2.0f, lo = -1.0f, hi = 3.0f;

    // Corners: bottom-left is (t=0, v=lo); top-right is (t=dur, v=hi).
    CurvePoint a = canvasToCurve(x0, y1, x0, x1, y0, y1, dur, lo, hi);
    CHECK(nearlyEqual(a.time, 0.0f) && nearlyEqual(a.value, lo));
    CurvePoint b = canvasToCurve(x1, y0, x0, x1, y0, y1, dur, lo, hi);
    CHECK(nearlyEqual(b.time, dur) && nearlyEqual(b.value, hi));
    // The center falls at the midpoint of both scales.
    CurvePoint c = canvasToCurve((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, x0, x1, y0, y1, dur, lo, hi);
    CHECK(nearlyEqual(c.time, 1.0f) && nearlyEqual(c.value, 1.0f));

    // Dragging UP raises the value: the screen axis goes the other way.
    CurvePoint arriba = canvasToCurve(x0, y0 + 5.0f, x0, x1, y0, y1, dur, lo, hi);
    CurvePoint abajo  = canvasToCurve(x0, y1 - 5.0f, x0, x1, y0, y1, dur, lo, hi);
    CHECK(arriba.value > abajo.value);

    // Time is clamped to the clip even if the mouse leaves; the value is not.
    CurvePoint fuera = canvasToCurve(x1 + 500.0f, y0 - 500.0f, x0, x1, y0, y1, dur, lo, hi);
    CHECK(nearlyEqual(fuera.time, dur));
    CHECK(fuera.value > hi);
    CurvePoint izq = canvasToCurve(x0 - 500.0f, y1, x0, x1, y0, y1, dur, lo, hi);
    CHECK(nearlyEqual(izq.time, 0.0f));

    // Round trip: what is drawn and what is read have to match, or the
    // key would jump when grabbed.
    float x = 0.0f, y = 0.0f;
    curveToCanvas(1.5f, 2.0f, x0, x1, y0, y1, dur, lo, hi, x, y);
    CurvePoint v = canvasToCurve(x, y, x0, x1, y0, y1, dur, lo, hi);
    CHECK(nearlyEqual(v.time, 1.5f));
    CHECK(nearlyEqual(v.value, 2.0f));

    // Zero height range (lo == hi): it cannot give NaN nor divide by zero.
    float xd = 0.0f, yd = 0.0f;
    curveToCanvas(1.0f, 5.0f, x0, x1, y0, y1, dur, 2.0f, 2.0f, xd, yd);
    CHECK(std::isfinite(xd) && std::isfinite(yd));
    CurvePoint d = canvasToCurve(x0, y1, x0, x1, y0, y1, dur, 2.0f, 2.0f);
    CHECK(std::isfinite(d.value));
}

static void test_curve_draw_range()
{
    PropertyTrack t;
    t.keys = { { 0.0f, 1.0f }, { 1.0f, 3.0f } };
    float lo = 0.0f, hi = 0.0f;
    curveRange(t, nullptr, 0, lo, hi);
    CHECK(lo < 1.0f && hi > 3.0f);            // margin on both sides
    CHECK(hi - lo < 4.0f);                    // but not disproportionate

    // The threshold enters the range even if it falls outside the keys: otherwise the
    // condition line would go off the drawing.
    const float umbral = 10.0f;
    curveRange(t, &umbral, 1, lo, hi);
    CHECK(hi > 10.0f);

    // Flat track: a zero height range would be a line stuck to the edge.
    PropertyTrack plana;
    plana.keys = { { 0.0f, 2.0f }, { 1.0f, 2.0f } };
    curveRange(plana, nullptr, 0, lo, hi);
    CHECK(hi - lo >= 0.9f);
    CHECK(lo < 2.0f && hi > 2.0f);

    // With no keys a degenerate range cannot come out either.
    PropertyTrack vacia;
    curveRange(vacia, nullptr, 0, lo, hi);
    CHECK(hi > lo);
}

static void test_property_samples_scale_with_the_layer_weight()
{
    AnimatorComponent a = makePuerta();
    CHECK(a.addLayer("Encima") == 1);
    a.setLayerWeight(1, 0.25f);
    PropertyClip c;
    c.name = "sube"; c.duration = 1.0f;
    PropertyTrack pos; pos.property = PropertyId::PositionY;
    pos.keys = { { 0.0f, 0.0f }, { 1.0f, 4.0f } };
    c.tracks.push_back(pos);
    a.addPropertyClip(c);
    AnimatorComponent::State s;
    s.name = "Sube"; s.propertyClipName = "sube";
    a.addState(s, 1);
    a.setEntryState(0, 1);
    a.bindProperties(nullptr, nullptr);
    a.update(0.1f, true);

    AnimatorComponent::PropertySampleRef m[8];
    const int n = a.propertySamples(m, 8);
    CHECK(n == 2);
    if (n == 2)
    {
        CHECK(nearlyEqual(m[0].weight, 1.0f));     // base layer
        CHECK(nearlyEqual(m[1].weight, 0.25f));    // layer with weight
    }
}

static void test_property_samples_follow_the_graph()
{
    AnimatorComponent a = makePuerta();
    a.bindProperties(nullptr, nullptr);
    a.reset();
    a.update(0.1f, true);
    AnimatorComponent::PropertySampleRef m[8];
    int n = a.propertySamples(m, 8);
    CHECK(n == 1);
    CHECK(m[0].clip == 0 && nearlyEqual(m[0].weight, 1.0f));
    CHECK(nearlyEqual(m[0].time, 0.1f));          // time IN SECONDS
    // In the cross-fade both sound, with weights that sum to 1. TWO updates
    // are needed: in the one that fires the transition the fade is at 0, so the
    // new state enters with weight 0 and does not contribute a sample yet.
    a.setTrigger("abre");
    a.update(0.016f, true);
    a.update(0.25f, true);
    n = a.propertySamples(m, 8);
    CHECK(n == 2);
    CHECK(nearlyEqual(m[0].weight + m[1].weight, 1.0f));
    CHECK(m[0].weight > 0.0f && m[1].weight > 0.0f);
    CHECK(m[0].clip != m[1].clip);
    // A state without a property clip contributes no sample.
    a.statesMutable()[1].propertyClipName.clear();
    a.bindProperties(nullptr, nullptr);
    a.reset();
    a.update(0.016f, true);
    a.setTrigger("abre");
    a.update(0.016f, true);
    a.update(0.25f, true);      // halfway through the fade: the new state ALREADY weighs
    n = a.propertySamples(m, 8);
    CHECK(n == 1 && m[0].clip == 0);
}

static void test_property_clips_drive_a_non_skinned_object()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Puerta");
    auto a = std::make_shared<AnimatorComponent>(makePuerta());
    go->setAnimator(a);
    a->bindProperties(go, nullptr);
    a->reset();
    CHECK(go->skinnedRenderIndex < 0);           // it is not skinned: before it was not animated

    SkinnedRendererDoble r;
    applySkinnedFrame(*go, r, 0.5f, /*evaluateTransitions=*/true);
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::PositionY), 0.0f));   // "cerrar" is flat
    a->setTrigger("abre");
    applySkinnedFrame(*go, r, 0.016f, true);     // enters "Abierta"
    applySkinnedFrame(*go, r, 1.0f, true);       // 1 s of a 2 s clip: halfway through the run
    const float y = propertyGet(*go, PropertyId::PositionY);
    CHECK(y > 0.5f && y < 4.0f);
    // In Edit the graph does not transition, but the state's time runs.
    Scene scene2("Test");
    GameObject* go2 = scene2.addGameObject("Puerta");
    auto b = std::make_shared<AnimatorComponent>(makePuerta());
    go2->setAnimator(b);
    b->bindProperties(go2, nullptr);
    b->reset();
    b->setTrigger("abre");
    applySkinnedFrame(*go2, r, 0.5f, /*evaluateTransitions=*/false);
    CHECK(b->currentStateName() == "Cerrada");
}

static void test_property_clips_material_goes_to_the_backend()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    go->staticRenderIndex = 3;
    auto a = std::make_shared<AnimatorComponent>();
    PropertyClip brillo;
    brillo.name = "brillo"; brillo.duration = 1.0f;
    PropertyTrack tr; tr.property = PropertyId::MaterialMetallic;
    tr.keys = { { 0.0f, 0.0f }, { 1.0f, 1.0f } };
    brillo.tracks.push_back(tr);
    a->addPropertyClip(brillo);
    AnimatorComponent::State s;
    s.name = "Brilla"; s.propertyClipName = "brillo";
    a->addState(s);
    a->setEntryState(0);
    go->setAnimator(a);
    a->bindProperties(go, nullptr);
    a->reset();

    SkinnedRendererDoble r;
    applySkinnedFrame(*go, r, 0.5f, true);
    CHECK(r.factoresRecibidos);
    CHECK(r.factoresIndice == 3u);
    CHECK(r.factorMetallic > 0.1f);
    CHECK(nearlyEqual(r.factorMetallic, propertyGet(*go, PropertyId::MaterialMetallic)));
}

static void test_property_clips_serialization(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Puerta");
    const uint64_t id = go->id;
    go->setAnimator(std::make_shared<AnimatorComponent>(makePuerta()));
    nlohmann::json j = scene.toJson();
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& anim = *found->getAnimator();
    CHECK(anim.propertyClips().size() == 2u);
    if (anim.propertyClips().size() != 2u) return;
    CHECK(anim.propertyClips()[1].name == "abrir");
    CHECK(nearlyEqual(anim.propertyClips()[1].duration, 2.0f));
    CHECK(anim.propertyClips()[1].tracks.size() == 1u);
    CHECK(anim.propertyClips()[1].tracks[0].property == PropertyId::PositionY);
    // The size BEFORE indexing: without this guard, a failure to read the
    // keys did not give FAIL but a silent abort (exit 3) on going out of the vector.
    CHECK(anim.propertyClips()[1].tracks[0].keys.size() == 2u);
    if (anim.propertyClips()[1].tracks[0].keys.size() != 2u) return;
    CHECK(nearlyEqual(anim.propertyClips()[1].tracks[0].keys[1].value, 4.0f));
    CHECK(anim.states()[1].propertyClipName == "abrir");
    // An Animator without property clips writes no new key.
    AnimatorComponent vacio;
    const std::string texto = animatorToJson(vacio).dump();
    CHECK(texto.find("propertyClip") == std::string::npos);
}

static void test_submachine_serialization(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Bicho");
    const uint64_t id = go->id;
    go->setAnimator(std::make_shared<AnimatorComponent>(makeCajas()));

    const nlohmann::json ja = animatorToJson(*go->getAnimator());
    CHECK(ja["states"][1].contains("subMachine"));
    CHECK(ja["states"][2].contains("parent"));
    // A normal state writes no new key.
    CHECK(!ja["states"][0].contains("parent"));
    CHECK(!ja["states"][0].contains("subMachine"));
    CHECK(!ja["states"][0].contains("subEntry"));

    nlohmann::json j = scene.toJson();
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& anim = *found->getAnimator();
    CHECK(anim.states().size() == 5u);
    if (anim.states().size() != 5u) return;
    CHECK(anim.states()[1].isSubMachine);
    CHECK(anim.states()[2].parent == 1);
    CHECK(anim.states()[1].subEntry == 3);
    CHECK(anim.resolveEntryLeaf(1, 0) == 4);

    // An Animator without boxes writes no new key.
    AnimatorComponent plano;
    AnimatorComponent::State s;
    s.name = "Solo";
    plano.addState(s);
    const std::string texto = animatorToJson(plano).dump();
    CHECK(texto.find("subMachine") == std::string::npos);
    CHECK(texto.find("parent") == std::string::npos);
    CHECK(texto.find("subEntry") == std::string::npos);
}

static void test_submachine_bad_file_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Bicho");
    const uint64_t id = go->id;
    go->setAnimator(std::make_shared<AnimatorComponent>(makeCajas()));
    nlohmann::json j = scene.toJson();
    for (auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        node["animator"]["states"][2]["parent"] = 99;   // out of range
        node["animator"]["states"][4]["parent"] = 0;    // Base is not a box
    }
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    CHECK(found->getAnimator()->states()[2].parent == -1);
    CHECK(found->getAnimator()->states()[4].parent == -1);
    bool rango = false, noCaja = false;
    for (const auto& w : loaded.lastWarnings())
    {
        if (w.find("out of range") != std::string::npos)        rango = true;
        if (w.find("is not a sub-state machine") != std::string::npos) noCaja = true;
    }
    CHECK(rango);
    CHECK(noCaja);

    // Containment cycle: Ataques inside Combo, which is already inside
    // Ataques. Without the cycle pass, going up through parent would not terminate.
    nlohmann::json c = scene.toJson();
    for (auto& node : c["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        node["animator"]["states"][1]["parent"] = 3;
    }
    Scene conCiclo("Ciclo");
    CHECK(conCiclo.fromJson(c, pm, am));
    GameObject* f2 = conCiclo.findById(id);
    CHECK(f2 && f2->hasAnimator());
    if (!f2 || !f2->hasAnimator()) return;
    const auto& anim = *f2->getAnimator();
    CHECK(anim.states().size() == 5u);
    if (anim.states().size() != 5u) return;
    CHECK(anim.states()[1].parent == -1 || anim.states()[3].parent == -1);
    bool ciclo = false;
    for (const auto& w : conCiclo.lastWarnings())
        if (w.find("cycle") != std::string::npos) ciclo = true;
    CHECK(ciclo);
}

static void test_curve_serialization(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    const uint64_t id = go->id;
    auto a = std::make_shared<AnimatorComponent>();
    a->addParameter("velocidad", AnimatorComponent::ParamType::Float);
    PropertyClip c;
    c.name = "acelera"; c.duration = 2.0f;
    PropertyTrack cur; cur.target = TrackTarget::Parameter; cur.parameterName = "velocidad";
    cur.keys = { { 0.0f, 1.0f }, { 2.0f, 7.0f } };
    PropertyTrack pos; pos.property = PropertyId::PositionY;
    pos.keys = { { 0.0f, 0.0f }, { 2.0f, 3.0f } };
    c.tracks = { cur, pos };
    a->addPropertyClip(c);
    go->setAnimator(a);

    const nlohmann::json ja = animatorToJson(*a);
    // contains BEFORE indexing: if the key is not written, nlohmann aborts
    // (silent exit 3) instead of giving FAIL.
    const auto& pj = ja["propertyClips"][0]["tracks"];
    CHECK(pj[0].contains("target"));
    if (pj[0].contains("target"))
    {
        CHECK(pj[0]["target"] == "parameter");
        CHECK(pj[0].contains("parameter") && pj[0]["parameter"] == "velocidad");
    }
    // The property track is saved as always: without "target".
    CHECK(!pj[1].contains("target"));

    nlohmann::json j = scene.toJson();
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found && found->hasAnimator());
    if (!found || !found->hasAnimator()) return;
    const auto& leido = *found->getAnimator();
    CHECK(leido.propertyClips().size() == 1u);
    if (leido.propertyClips().empty()) return;
    const auto& pistas = leido.propertyClips()[0].tracks;
    CHECK(pistas.size() == 2u);
    if (pistas.size() != 2u) return;
    CHECK(pistas[0].target == TrackTarget::Parameter);
    CHECK(pistas[0].parameterName == "velocidad");
    CHECK(pistas[0].keys.size() == 2u);
    CHECK(pistas[1].target == TrackTarget::Property);
    CHECK(pistas[1].property == PropertyId::PositionY);

    // A curve without a parameter name is discarded on load.
    nlohmann::json roto = j;
    for (auto& nodo : roto["root"]["children"])
        if (nodo.contains("animator"))
            nodo["animator"]["propertyClips"][0]["tracks"][0]["parameter"] = "";
    Scene loaded2("Loaded2");
    CHECK(loaded2.fromJson(roto, pm, am));
    GameObject* f2 = loaded2.findById(id);
    CHECK(f2 && f2->hasAnimator());
    if (!f2 || !f2->hasAnimator()) return;
    CHECK(f2->getAnimator()->propertyClips()[0].tracks.size() == 1u);
}

static void test_property_clips_bad_file_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Puerta");
    go->setAnimator(std::make_shared<AnimatorComponent>(makePuerta()));
    nlohmann::json j = scene.toJson();
    for (auto& node : j["root"]["children"])
    {
        if (!node.contains("animator")) continue;
        node["animator"]["propertyClips"][0]["tracks"][0]["property"] = "noExiste";
        node["animator"]["propertyClips"][1]["duration"] = 0.0f;
        while (node["animator"]["propertyClips"].size() < 20)
            node["animator"]["propertyClips"].push_back(node["animator"]["propertyClips"][0]);
    }
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    bool prop = false, dur = false, tope = false;
    for (const auto& w : loaded.lastWarnings())
    {
        if (w.find("noExiste") != std::string::npos) prop = true;
        if (w.find("duration") != std::string::npos) dur = true;
        if (w.find("property clips") != std::string::npos) tope = true;
    }
    CHECK(prop && dur && tope);
    GameObject* found = loaded.getRoot().children[0].get();
    const auto& anim = *found->getAnimator();
    CHECK(anim.propertyClips().size() == (size_t)AnimatorComponent::kMaxPropertyClips);
    CHECK(anim.propertyClips()[0].tracks.empty());              // the bad track is discarded
    CHECK(anim.propertyClips()[1].duration > 0.0f);             // the duration is clamped
}

static void test_property_clips_apply_graph_restores()
{
    AnimatorComponent a = makePuerta();
    const AnimatorComponent::Graph snap = a.graph();
    a.propertyClipsMutable()[1].duration = 9.0f;
    a.removePropertyClip(0);
    a.applyGraph(snap);
    CHECK(a.propertyClips().size() == 2u);
    if (a.propertyClips().size() == 2u) CHECK(nearlyEqual(a.propertyClips()[1].duration, 2.0f));
}

// The host propagates the worldTransforms and pushes the transform BEFORE calling the
// helper: what is animated in this frame has to be resent, or the object would go
// one frame behind (and its children, two).
// A curve is not a property: it does not move the object nor mark the frame's
// result. The parameter is written (the component does it in update).
static void test_curve_does_not_touch_the_transform()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Cubo");
    go->staticRenderIndex = 3;
    auto a = std::make_shared<AnimatorComponent>(makeCurvaVelocidad());
    go->setAnimator(a);
    a->bindProperties(go, nullptr);
    a->reset();
    scene.getRoot().updateWorldTransforms();
    const float yAntes = propertyGet(*go, PropertyId::PositionY);
    const float xAntes = propertyGet(*go, PropertyId::PositionX);

    const AnimatorFrameResult r = applyAnimatorFrame(*go, 0.5f, true);
    CHECK(!r.transform);
    CHECK(!r.material);
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::PositionX), xAntes));
    CHECK(nearlyEqual(propertyGet(*go, PropertyId::PositionY), yAntes));
    CHECK(nearlyEqual(a->getFloat("velocidad"), 5.0f));
}

static void test_property_clips_push_transform_and_world()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Puerta");
    go->staticRenderIndex = 7;
    GameObject* hijo = scene.addGameObject("Pomo", go);
    hijo->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 1));
    auto a = std::make_shared<AnimatorComponent>(makePuerta());
    go->setAnimator(a);
    a->bindProperties(go, nullptr);
    a->reset();
    scene.getRoot().updateWorldTransforms();

    SkinnedRendererDoble r;
    a->setTrigger("abre");
    applySkinnedFrame(*go, r, 0.016f, true);
    applySkinnedFrame(*go, r, 1.0f, true);
    CHECK(r.transformEstaticoRecibido);
    CHECK(r.transformEstaticoIndice == 7);
    const float y = propertyGet(*go, PropertyId::PositionY);
    CHECK(y > 0.5f);
    // The transform the backend receives is THIS frame's, not the previous one's.
    CHECK(nearlyEqual(r.transformEstatico[3].y, go->worldTransform[3].y));
    CHECK(nearlyEqual(go->worldTransform[3].y, y));
    // And the child already hangs from the new position, without waiting for the next frame.
    CHECK(nearlyEqual(hijo->worldTransform[3].y, y));
}

int main()
{
    // A single PxFoundation per process: one PhysicsManager shared by
    // all the tests, never one per test. physics/audio are only needed because
    // Scene::fromJson requires them in its signature to recreate colliders and clips; these
    // tests simulate nothing.
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    am.init();

    test_remove_state_keeps_params_and_playhead();
    test_remove_state_reindexes_playhead_not_resets_it();
    test_remove_current_state_falls_back_but_keeps_params();
    test_set_entry_state_keeps_params();
    test_loader_reads_all_clips();
    test_loader_registers_builtin_source();
    test_unique_clip_name();
    test_load_animation_clips_matches_full_load();
    test_load_animation_clips_against_foreign_skeleton();
    test_load_animation_clips_partial_skeleton();
    test_load_animation_clips_missing_file();
    test_add_animation_source_appends_clips();
    test_add_animation_source_rejects_foreign_rig();
    test_remove_animation_source();
    test_remove_builtin_source_is_rejected();
    test_rename_clip();
    test_add_animation_source_with_forced_names();
    test_rename_clip_references_in_animator();
    test_add_animation_source_with_forced_names_collision();
    test_apply_clip_names_positionally_swap();
    test_apply_clip_names_positionally_rejects_duplicate_saved_names();
    test_apply_clip_names_positionally_rejects_external_collision();
    test_pack_concatenates_clips();
    test_pack_mesh_without_clips();
    test_pack_without_clips_yields_identity_final_bones();
    test_bone_without_channel_keeps_bind_pose();

    test_trigger_switches_state();
    test_animation_finished_timing();
    test_loop_flag();
    test_bool_condition_expected();
    test_trigger_consumption();
    test_edit_mode_does_not_transition();
    test_addstate_assigns_stable_unique_editor_ids();
    test_remove_state_reindexes();
    test_first_matching_transition_wins();
    test_transition_without_conditions_never_fires();
    test_remove_parameter_ignores_empty_name();
    test_remove_parameter_drops_emptied_transitions();
    test_parameter_api_ignores_undeclared();
    test_int_condition_greater_and_equals();
    test_float_condition_less();
    test_float_condition_equals_and_not_equals();
    test_numeric_api_type_guards();
    test_remove_numeric_parameter_cleans_conditions();
    test_animation_finished_fires_on_zero_duration_state();

    test_crossfade_weight_ramps_over_duration();
    test_crossfade_advances_both_clocks();
    test_crossfade_prev_clock_loops();
    test_crossfade_zero_duration_is_instant_cut();
    test_crossfade_reset_clears_blend();
    test_crossfade_pose_interpolates_between_clips();

    test_root_lock_pins_root_translation_to_bind_pose();
    test_root_lock_keeps_child_animating();
    test_root_motion_unlocked_still_translates();
    test_crossfade_root_lock_follows_destination_state();

    test_blend_state_weight_from_float_param();
    test_blend_state_syncs_normalized_phase();
    test_state_without_blend_clip_stays_single();
    test_crossfade_takes_priority_over_blend_state();
    test_bind_clips_resolves_blend_clip();
    test_blend_state_pose_interpolates_between_clips();
    test_finished_transition_reaches_destination();
    test_edit_mode_finished_leaks_into_play_without_reset();

    test_has_bones_detects_rigged_fbx();
    test_loadSkinned_missing_file_throws();
    test_has_bones_rejects_unrigged_model();
    test_has_bones_survives_missing_file();
    test_clip_has_motion_criterion();
    test_loader_drops_static_take();
    test_external_clips_survive_filter();
    test_first_added_clip_becomes_index_zero();
    test_load_auto_returns_skinned_for_rigged();
    test_load_auto_returns_static_for_unrigged();

    test_bind_clips_resolves_by_name();
    test_gameobject_animator_slot();

    test_graph_survives_scene_round_trip(pm, am);
    test_scene_without_animator_block_loads(pm, am);
    test_numeric_graph_survives_scene_round_trip(pm, am);
    test_condition_without_compare_fields_loads(pm, am);
    test_crossfade_duration_survives_scene_round_trip(pm, am);
    test_transition_without_duration_field_loads(pm, am);
    test_blend_state_survives_scene_round_trip(pm, am);
    test_blend1d_picks_neighbours_by_threshold();
    test_blend1d_clamps_to_extremes();
    test_blend1d_equal_thresholds_first_wins();
    test_blend1d_ignores_unresolved_entries();
    test_blend1d_samples_every_clip_at_principal_phase();
    test_blend1d_rename_and_rebind_entries();
    test_blend1d_migrates_old_pair(pm, am);
    test_blend1d_scene_round_trip(pm, am);
    test_event_fires_once_per_cycle();
    test_event_fires_across_loop_wrap();
    test_event_fires_per_skipped_cycle();
    test_event_non_loop_fires_once();
    test_event_at_zero_fires_on_entry();
    test_event_not_fired_in_edit_mode();
    test_event_fading_out_state_is_silent();
    test_fired_events_cleared_each_update();
    test_events_scene_round_trip(pm, am);
    test_root_motion_mode_migration(pm, am);
    test_root_displacement_counts_cycles();
    test_root_motion_delta_across_wrap();
    test_root_motion_delta_drops_y();
    test_root_motion_delta_blend_weighted();
    test_root_motion_delta_crossfade_weighted();
    test_root_motion_crossfade_uses_prev_clock();
    test_root_motion_samples_only_in_play_with_apply();
    test_root_motion_space_is_model_y_up();
    test_apply_root_motion_moves_transform();
    test_apply_root_motion_sets_rigidbody_velocity(pm);
    test_apply_skinned_frame_applies_root_motion();
    test_packing_fills_bone_depth();
    test_packing_depth_invariant_real_rig();
    test_pose_samples_without_fade();
    test_pose_samples_fade_between_blends();
    test_pose_freeze_on_interrupted_fade();
    test_pose_weights_sum_to_one();
    test_pose_continuity_fade_from_blend();
    test_pose_continuity_interrupted_fade();
    test_blend2d_inside_triangle_barycentric();
    test_blend2d_vertex_edge_outside();
    test_blend2d_degenerate_cases();
    test_blend2d_cocircular_square();
    test_blend2d_prefers_delaunay_diagonal();
    test_blend2d_continuity_sweep();
    test_blend2d_state_samples();
    test_blend2d_pose_and_fade();
    test_blend2d_legacy_pair_is_two_heaviest();
    test_blend2d_root_motion_samples();
    test_layers_management();
    test_layers_trigger_fires_in_both_layers();
    test_layers_clocks_independent();
    test_layers_pose_tags_and_weights();
    test_layers_events_and_root_motion();
    test_layers_mask_resolution();
    test_layers_single_layer_json_unchanged();
    test_layers_graph_key_sees_layer_edits();
    test_layers_apply_graph_restores();
    test_layers_state_index_by_editor_id();
    test_ik_constraint_management();
    test_ik_chain_resolution();
    test_property_track_sampling();
    test_property_blend_short_path();
    test_property_names_round_trip();
    test_property_get_set_transform();
    test_property_light_and_material();
    test_property_clip_binding_and_duration();
    test_property_clip_tracks_resolved_against_object();
    test_curve_track_resolves_against_float_parameter();
    test_curve_writes_the_parameter();
    test_curve_blends_during_cross_fade();
    test_curve_fires_its_transition_in_the_same_frame();
    test_curve_of_a_zero_weight_layer_still_writes();
    test_curve_last_layer_wins();
    test_normalize_bone_weights();
    test_loader_normalizes_bone_weights();
    test_canvas_ids_round_trip();
    test_editor_id_survives_deleting_an_earlier_state();
    test_sanitize_graph_fixes_bad_indices();
    test_sanitize_graph_leaves_a_good_graph_alone();
    test_submachine_hierarchy_helpers();
    test_submachine_transition_enters_the_leaf();
    test_submachine_broken_entry_does_not_fire();
    test_submachine_exit_fires_from_any_leaf();
    test_submachine_leaf_transition_wins_over_ancestor();
    test_play_resolves_a_submachine();
    test_removing_a_submachine_removes_its_children();
    test_removing_a_submachine_whose_child_comes_first();
    test_removing_a_state_reindexes_parent_and_entry();
    test_mesh_clock_advances_in_ticks();
    test_curve_condition_thresholds();
    test_curve_canvas_conversions();
    test_curve_draw_range();
    test_property_samples_scale_with_the_layer_weight();
    test_property_samples_follow_the_graph();
    test_property_clips_drive_a_non_skinned_object();
    test_property_clips_material_goes_to_the_backend();
    test_property_clips_push_transform_and_world();
    test_curve_does_not_touch_the_transform();
    test_property_clips_apply_graph_restores();
    test_ik_graph_key_and_apply_graph();
    test_ik_lookat();
    test_ik_twobone_reaches_target();
    test_ik_twobone_partial_weight();
    test_ik_twobone_pole_decides_the_plane();
    test_ik_block_layout();
    test_pose_block_layout();
    test_layers_override_mask_criterion();
    test_layers_additive_delta();
    test_layers_single_layer_matches_pose_samples();
    test_state_without_blend_fields_loads(pm, am);
    test_blend2d_serialization(pm, am);
    test_layers_serialization(pm, am);
    test_layers_too_many_warns(pm, am);
    test_ik_serialization(pm, am);
    test_ik_bad_file_warns(pm, am);
    test_property_clips_serialization(pm, am);
    test_curve_serialization(pm, am);
    test_submachine_serialization(pm, am);
    test_submachine_bad_file_warns(pm, am);
    test_property_clips_bad_file_warns(pm, am);
    test_root_lock_survives_scene_round_trip(pm, am);
    test_state_without_lock_root_motion_field_loads(pm, am);
    test_animation_sources_survive_scene_round_trip(pm, am);
    test_missing_animation_source_does_not_break_load(pm, am);
    test_animation_source_config_roundtrip();
    test_animation_source_config_missing_source_warns_and_continues();
    test_animation_source_config_builtin_on_empty_mesh_is_ignored();
    test_missing_animation_source_warns_through_scene(pm, am);
    test_animator_out_of_range_entry_state_warns(pm, am);
    test_animator_out_of_range_transition_is_dropped(pm, am);
    test_animator_valid_graph_loads_untouched(pm, am);
    test_scene_without_animation_sources_loads(pm, am);
    test_scene_load_ignores_stale_skinned_false(pm, am);
    test_scene_load_warns_when_rig_disappeared(pm, am);
    test_scene_load_warns_when_file_missing(pm, am);
    test_scene_load_warns_on_load_exception(pm, am);
    test_scene_load_shares_has_bones_cache_across_nodes(pm, am);
    test_clone_of_rigged_mesh_stays_skinned(pm, am);
    test_clone_of_rigged_mesh_does_not_reread_disk(pm, am);
    test_corrupt_clip_names_does_not_lose_scene(pm, am);
    test_delete_undo_restores_rigged_mesh_as_skinned(pm, am);

    test_animator_command_add_undo_redo();
    test_animator_command_remove();
    test_animator_command_survives_missing_target();

    test_animation_source_command_add_undo_redo();
    test_animation_source_command_remove_restores_names();
    test_animation_source_command_remove_rebinds_clip_indices();
    test_animation_source_command_remove_targets_clicked_occurrence();
    test_animation_source_command_remove_noop_when_occurrence_missing();
    test_animation_source_command_undo_add_after_interleaved_remove_undo();
    test_clip_rename_command();
    test_animation_source_command_survives_missing_target();

    test_apply_graph_keeps_param_values_of_same_name_and_type();
    test_apply_graph_playhead_follows_editor_id();
    test_apply_graph_missing_current_state_falls_to_entry();
    test_apply_graph_never_lowers_next_editor_id();
    test_apply_graph_live_states_keep_position();
    test_apply_own_graph_changes_nothing();
    test_apply_graph_without_fading_state_cuts_crossfade();
    test_apply_graph_without_current_state_cuts_crossfade();

    test_graph_key_ignores_node_position();
    test_graph_key_sees_every_saved_field();

    test_graph_command_round_trip_for_each_mutation();
    test_graph_command_noop_without_animator();
    test_graph_command_rebinds_clips();

    test_tracker_drag_yields_one_command();
    test_tracker_drag_back_to_start_yields_nothing();
    test_tracker_revision_change_rebases();
    test_tracker_selection_change_discards_session();
    test_tracker_node_move_yields_nothing();
    test_tracker_label_is_per_gesture();

    test_apply_skinned_frame_sets_visible_before_animation();
    test_apply_skinned_frame_passes_pose_b_then_a();
    test_apply_skinned_frame_delivers_freeze_once();
    test_apply_skinned_frame_passes_ik_in_model_space();
    test_apply_skinned_frame_without_animator_advances_backend_clock();
    test_apply_skinned_frame_edit_mode_does_not_move_the_graph();
    test_apply_skinned_frame_ignores_unregistered_object();
    test_apply_skinned_frame_forwards_transform_and_ssr();

    test_exit_time_below_one_fires_when_crossed();
    test_exit_time_below_one_checks_conditions_only_at_the_crossing();
    test_exit_time_below_one_fires_when_a_big_dt_wraps_past_it();
    test_exit_time_above_one_counts_loops();
    test_transition_without_conditions_needs_exit_time();
    test_exit_time_on_zero_duration_state_is_reached_at_once();
    test_exit_time_clock_restarts_when_entering_a_state();
    test_exit_time_clock_restarts_on_reset_playback();
    test_any_state_fires_from_every_state();
    test_any_state_wins_over_the_current_state_transition();
    test_any_state_does_not_reenter_the_current_state_by_default();
    test_any_state_reenters_with_can_transition_to_self();
    test_remove_state_drops_and_reindexes_any_state_transitions();
    test_exit_time_and_any_state_survive_scene_round_trip(pm, am);
    test_scene_without_exit_time_fields_loads(pm, am);
    test_any_state_transition_with_bad_target_is_dropped(pm, am);
    test_negative_exit_time_is_clamped_with_warning(pm, am);

    test_clip_count_is_at_least_one();
    test_clamp_clip_index_falls_back_to_zero();

    test_play_and_crossfade_reject_unknown_state();
    test_play_enters_at_once_and_cuts_the_blend();
    test_crossfade_starts_a_blend_from_the_current_state();
    test_crossfade_with_zero_seconds_is_a_cut();
    test_play_to_the_current_state_restarts_it();
    test_reset_trigger_disarms_it();
    test_normalized_time_accumulates_and_restarts();

    test_state_speed_and_param_multiply_the_clock();
    test_zero_speed_never_finishes_nor_reaches_exit_time();
    test_negative_speed_freezes_instead_of_reversing();
    test_fading_state_uses_its_own_speed();
    test_global_speed_scales_clock_and_blend();
    test_state_speed_survives_scene_round_trip(pm, am);
    test_negative_state_speed_is_clamped_on_load(pm, am);

    test_clone_with_animation_source_keeps_clip_count(pm, am);
    test_clone_shares_the_mesh(pm, am);
    test_mesh_matches_animation_config();
    test_insert_from_json_reuses_preloaded_mesh(pm, am);
    test_legacy_scene_without_sources_shares_preloaded(pm, am);
    test_edit_mesh_copies_only_when_shared();
    test_equal_material_override_does_not_copy();

    am.shutdown();
    pm.shutdown();
    if (g_failures) { std::printf("dt_animator_tests: %d FAILURES\n", g_failures); std::fflush(stdout); return 1; }
    std::printf("dt_animator_tests: OK\n");
    std::fflush(stdout);
    return 0;
}
