// Headless tests of AudioClipComponent: volume/pitch ranges and their
// serialization. Plain main + asserts, no framework, consistent with
// camera_tests.cpp and physics_tests.cpp.
//
// The component is built bare with m_audio = nullptr and soundId = -1:
// this way the setters exercise the clamp without needing FMOD or an audio
// device. Same trick that exporter_tests.cpp uses.
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/CapsuleCollider.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Core/ImportSettings.h"
#ifdef DT_FMOD_ENABLED
#include <fmod.hpp>
#endif
#include "DonTopo/Audio/AudioListenerComponent.h"
#include "DonTopo/Audio/ReverbZoneComponent.h"
#include "DonTopo/Editor/Command.h"
#include <nlohmann/json.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>

#include "DonTopo/Core/Platform.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

using namespace DonTopo;

// The wait loops of this file (the ones that wait for FMOD to finish
// loading or to report a failure) carry GENEROUS caps on purpose: they exit as
// soon as the condition is met, so a high cap does not slow down the good case,
// but a tight one turns machine load into a red that is not the code's
// fault. It already happened once, running the whole suite in one go.
static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); ++g_failures; } } while (0)

static bool nearlyEqual(float a, float b, float eps = 0.0001f) { return std::fabs(a - b) < eps; }

// createAudioClipComponent returns nullptr for TWO different reasons: FMOD
// not available on the machine (a legitimate SKIP) or the exe was launched from a
// directory where "assets/audio.mp3" does not exist (wrong cwd). Without
// telling them apart, the second case gives exit 0 with the test not run: a false
// green for a repo criterion that is "exit code 0".
//
// The SKIP is decided by AudioManager::available() and NOT by "the file exists":
// before, with FMOD compiled in and no output device, init() threw and the
// binary aborted without ever printing this SKIP; the message promised
// a path that did not exist. Now init() returns false and available() says so.
static bool checkAudioProbe(const AudioManager& am,
                             const std::shared_ptr<AudioClipComponent>& probe, const char* testName)
{
    if (probe) return true;
    if (!am.available())
    {
        std::printf("SKIP %s (FMOD no disponible)\n", testName);
        return false;
    }
    if (!std::filesystem::exists("assets/audio.mp3"))
        std::printf("FAIL: %s - assets/audio.mp3 no existe (ejecuta los tests desde la raiz del repo)\n", testName);
    else
        std::printf("FAIL: %s - createAudioClipComponent fallo con FMOD vivo y el asset presente\n", testName);
    ++g_failures;
    return false;
}

static std::shared_ptr<AudioClipComponent> makeClip()
{
    return std::make_shared<AudioClipComponent>(nullptr, "assets/audio.mp3", -1, false, false);
}

// A freshly created clip sounds exactly as recorded: not attenuated and without
// altering the pitch. If these defaults changed, every scene saved before
// this feature would sound different when reloaded.
// FMOD without output starts "fine" and nothing sounds: that case has to give a warning,
// and a real output must not.
static void test_output_warning_only_without_output()
{
#ifdef DT_FMOD_ENABLED
    CHECK(!AudioManager::outputWarningFor(FMOD_OUTPUTTYPE_NOSOUND).empty());
    CHECK(AudioManager::outputWarningFor(FMOD_OUTPUTTYPE_PULSEAUDIO).empty());
    CHECK(AudioManager::outputWarningFor(FMOD_OUTPUTTYPE_WASAPI).empty());
#endif
    CHECK(AudioManager::outputWarningFor(-12345).empty());
}

static void test_defaults_are_neutral()
{
    auto clip = makeClip();
    CHECK(nearlyEqual(clip->getVolume(), 1.0f));
    CHECK(nearlyEqual(clip->getPitch(), 1.0f));
}

static void test_volume_clamps_to_range()
{
    auto clip = makeClip();

    clip->setVolume(0.5f);
    CHECK(nearlyEqual(clip->getVolume(), 0.5f));

    clip->setVolume(-1.0f);
    CHECK(nearlyEqual(clip->getVolume(), 0.0f));

    clip->setVolume(5.0f);
    CHECK(nearlyEqual(clip->getVolume(), 1.0f));
}

// The minimum is NOT 0: a pitch of 0 would stop the sound dead instead of
// lowering it, and FMOD does not accept it as "silence".
static void test_pitch_clamps_to_range()
{
    auto clip = makeClip();

    clip->setPitch(1.5f);
    CHECK(nearlyEqual(clip->getPitch(), 1.5f));

    clip->setPitch(0.1f);
    CHECK(nearlyEqual(clip->getPitch(), 0.5f));

    clip->setPitch(10.0f);
    CHECK(nearlyEqual(clip->getPitch(), 2.0f));
}

// Without an AudioManager there is no channel to push the value to. The setter has to
// store it anyway and not touch a null pointer.
static void test_setters_survive_without_manager()
{
    auto clip = makeClip();
    clip->setVolume(0.25f);
    clip->setPitch(1.75f);
    CHECK(nearlyEqual(clip->getVolume(), 0.25f));
    CHECK(nearlyEqual(clip->getPitch(), 1.75f));
}

// The JSON has to carry both fields: without them, moving a slider and
// saving the scene would leave no trace.
static void test_tojson_emits_volume_and_pitch()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    auto clip = makeClip();
    clip->setVolume(0.25f);
    clip->setPitch(1.5f);
    go->setAudioClip(clip);

    nlohmann::json j = scene.toJson();
    const nlohmann::json& node = j["root"]["children"][0];
    // Explicit anchor: if the Scene constructor ever seeded a default
    // child, this CHECK points at the real cause instead of the asserts
    // below failing against the wrong node with no clue.
    CHECK(node["name"] == "altavoz");
    CHECK(node.contains("audioClip"));
    if (!node.contains("audioClip")) return;
    CHECK(nearlyEqual(node["audioClip"].value("volume", -1.0f), 0.25f));
    CHECK(nearlyEqual(node["audioClip"].value("pitch",  -1.0f), 1.5f));
}

// Full round-trip through toJson/fromJson with NON-neutral values and, on
// purpose, DIFFERENT from each other (same pattern as
// camera_tests.cpp:190-221, test_serialization_round_trip). 1.0/1.0 is at the
// same time the component's factory neutral and the default that
// Scene::fromJson loads with when the keys are missing: a round-trip with those values
// would "pass" all the same even if nobody wrote or read anything (see finding 1 of the
// review; it is exactly what test_scene_without_volume_loads_neutral could not
// distinguish on its own). That volume != pitch also uncovers a swap of
// setters (setVolume(c.value("pitch",...)) or vice versa): with equal values
// the swap would go unnoticed.
//
// It needs FMOD alive, same as the back-compat: Scene::fromJson creates the clip
// with AudioManager::createAudioClipComponent, which without a loaded sound
// returns nullptr. Same SKIP if FMOD is not available on the machine.
static void test_volume_pitch_round_trip(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!checkAudioProbe(am, probe, "test_volume_pitch_round_trip")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setVolume(0.25f);
    probe->setPitch(1.5f);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(nearlyEqual(found->getAudioClip()->getVolume(), 0.25f));
    CHECK(nearlyEqual(found->getAudioClip()->getPitch(),  1.5f));
}

// Back-compat: a scene saved before this feature does not carry the fields and
// has to load with the neutral values. It is what breaks if someone
// changes the load's .value() to an .at().
//
// The JSON is built from scene.toJson() and not by hand: a bare-written
// literal already went out of sync once with the real format of
// nodeFromJson/Scene::fromJson (it lacked "version" and used
// position/rotation/scale instead of localTransform). Starting from toJson() and
// deleting there the keys we want missing is immune to schema changes
// (same pattern as camera_tests.cpp:246-257). The values before
// deletion are NON-neutral on purpose: if erase() did not really remove the
// keys (or fromJson read them from elsewhere), the test would see 0.25/1.5 instead of
// the neutral 1.0/1.0 and would fail all the same.
//
// It needs FMOD alive: Scene::fromJson creates the clip with
// AudioManager::createAudioClipComponent, which without a loaded sound returns
// nullptr. On a machine without an audio device the test skips
// itself instead of giving a false red.
static void test_scene_without_volume_loads_neutral(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!checkAudioProbe(am, probe, "test_scene_without_volume_loads_neutral")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setVolume(0.25f);
    probe->setPitch(1.5f);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    nlohmann::json& audioClip = j["root"]["children"][0]["audioClip"];
    CHECK(audioClip.contains("volume"));
    CHECK(audioClip.contains("pitch"));
    audioClip.erase("volume");
    audioClip.erase("pitch");

    Scene loaded("Vacia");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* loadedGo = loaded.findById(go->id);
    CHECK(loadedGo != nullptr);
    if (!loadedGo || !loadedGo->hasAudioClip()) { CHECK(false); return; }
    CHECK(nearlyEqual(loadedGo->getAudioClip()->getVolume(), 1.0f));
    CHECK(nearlyEqual(loadedGo->getAudioClip()->getPitch(),  1.0f));
}

// setVolume(NaN)/setPitch(NaN) have to leave the previous value intact.
// The clamp alone did NOT do it: std::clamp(NaN, lo, hi) returns NaN
// (every comparison with NaN is false), so before this fix a NaN
// coming from a broken Lua script (a 0/0, for example) slipped past the
// clamp and was stored as is in m_volume/m_pitch, to end up
// serialized as "null" in the .scene and bring down the whole Scene::fromJson (see
// the rest of the tests in this file). This test exercises the guard added
// directly in AudioClipComponent::setVolume/setPitch, without going through
// Lua or Scene.
static void test_setVolume_setPitch_reject_nan()
{
    auto clip = makeClip();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    clip->setVolume(0.6f);
    clip->setVolume(nan);
    CHECK(nearlyEqual(clip->getVolume(), 0.6f));

    clip->setPitch(1.4f);
    clip->setPitch(nan);
    CHECK(nearlyEqual(clip->getPitch(), 1.4f));
}

// THE TEST THAT MATTERS: a scene whose JSON carries "volume": null (the same
// "null" that nlohmann writes when serializing a NaN, see
// AudioClipComponent::setVolume) has to load fine in full: not just the broken
// audioClip, but the rest of its fields (pitch) too, with the
// clip falling to the neutral default volume and a warning in
// Scene::lastWarnings() that names the field. Before this fix,
// Scene::fromJson returned false: json::exception (302, "type must be
// number, but is null") escaped from nodeFromJson and fromJson's catch
// dropped the load of the WHOLE scene because of this single field.
static void test_scene_with_null_volume_loads_with_warning(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!checkAudioProbe(am, probe, "test_scene_with_null_volume_loads_with_warning")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setVolume(0.4f);
    probe->setPitch(1.3f);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    nlohmann::json& audioClip = j["root"]["children"][0]["audioClip"];
    CHECK(audioClip.contains("volume"));
    // Put the null in by hand: that is exactly how a serialized NaN arrives.
    audioClip["volume"] = nullptr;

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(nearlyEqual(found->getAudioClip()->getVolume(), 1.0f)); // neutral default
    CHECK(nearlyEqual(found->getAudioClip()->getPitch(),  1.3f)); // the rest kept loading fine

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("volume") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// Compares component by component against the identity (avoids depending on
// glm::mat4 having operator== available in this TU).
static bool isIdentity(const glm::mat4& m)
{
    const glm::mat4 id(1.0f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!nearlyEqual(m[c][r], id[c][r])) return false;
    return true;
}

// A localTransform with a null among its 16 floats (same origin as the
// volume one: a serialized NaN) cannot bring down the whole scene. Chosen
// design (also documented in Scene.cpp next to jsonToMat4): ANY
// corrupt float among the 16 discards the whole matrix and falls to the full
// identity, not just that component; a "half" matrix could look
// valid and have the scale or rotation silently broken. It does not need
// FMOD: the node carries no audioClip.
static void test_localTransform_null_element_loads_identity(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("cosa");
    go->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 20.0f, 30.0f));

    nlohmann::json j = scene.toJson();
    nlohmann::json& lt = j["root"]["children"][0]["localTransform"];
    CHECK(lt.is_array());
    CHECK(lt.size() == 16);
    lt[5] = nullptr; // one of the 16 floats corrupt (arbitrary position)

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(isIdentity(found->localTransform));

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("localTransform") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// Review finding 1: boxCollider.halfExtents is ALWAYS written by nodeToJson
// (it is never optional, unlike volume/pitch/fov...); if the
// .scene loses it (badly resolved merge, truncated write, hand edit)
// it is NOT legitimate back-compat, it is corruption, and it has to warn naming the
// field and the object instead of falling to a plausible value without a single WARN.
// Before this fix: a 25-unit box centered at the origin, zero warnings,
// the user sees it, questions nothing, presses Save and the original measurements
// are lost forever. It does not need FMOD (the node carries no audioClip).
static void test_boxCollider_missing_halfExtents_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Caja");
    go->setBoxCollider(pm.createBoxColliderComponent(glm::vec3(3.0f, 4.0f, 5.0f), glm::vec3(0.0f),
                                                      go->worldTransform, /*dynamic=*/false));

    nlohmann::json j = scene.toJson();
    nlohmann::json& box = j["root"]["children"][0]["boxCollider"];
    CHECK(box.contains("halfExtents"));
    box.erase("halfExtents");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasBoxCollider()) { CHECK(false); return; }

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("halfExtents") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// Review finding 2: "center": null (the EXACT form a serialized NaN takes,
// see the rest of this file) cannot silently fall to
// (0,0,0). Before this fix, readArrayFloat treated "is not an array" the same as
// "index out of range" (both through the same silent branch): a
// capsule with a corrupt center moved to the origin leaving no trace in the
// Log: "the capsule moved by itself", exactly as the review describes it.
static void test_capsuleCollider_null_center_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Capsula");
    go->setCapsuleCollider(pm.createCapsuleColliderComponent(
        15.0f, 25.0f, glm::vec3(10.0f, 20.0f, 30.0f), go->worldTransform, /*dynamic=*/false));

    nlohmann::json j = scene.toJson();
    nlohmann::json& cap = j["root"]["children"][0]["capsuleCollider"];
    CHECK(cap.contains("center"));
    cap["center"] = nullptr; // that is exactly how a serialized NaN arrives

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasCapsuleCollider()) { CHECK(false); return; }

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("center") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// path/is3D/loop are ALWAYS written by nodeToJson. Until this fix they were read with
// .at(): for an audioClip missing ANY of the three, nlohmann
// threw a json::exception that went up to fromJson's catch and dropped the
// load of the WHOLE scene; an unrelated object, on another branch of the tree, was lost
// over an audio field. Now the scene loads in full, the clip falls to its default
// (2D) and the warning names the field.
//
// "loop" is NOT deleted on purpose: if the fix had been applied to only two of
// the three fields, this test would see it all the same (the missing one is enough to throw),
// so there is one test per field below instead of one that deletes them all at once.
static void test_scene_audioclip_missing_is3D_warns(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, false);
    if (!checkAudioProbe(am, probe, "test_scene_audioclip_missing_is3D_warns")) return;

    Scene scene("Test");
    GameObject* go    = scene.addGameObject("altavoz");
    // A second object, a sibling and WITHOUT audio: it is the one that shows the real damage
    // of the old bug. With .at(), this node did not get loaded either.
    GameObject* otro  = scene.addGameObject("sin_audio");
    probe->setPitch(1.3f);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    nlohmann::json& audioClip = j["root"]["children"][0]["audioClip"];
    CHECK(audioClip.contains("is3D"));
    audioClip.erase("is3D");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    CHECK(loaded.findById(otro->id) != nullptr);

    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(found->getAudioClip()->getIs3D() == false);          // default
    CHECK(nearlyEqual(found->getAudioClip()->getPitch(), 1.3f)); // the rest kept loading

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("is3D") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

static void test_scene_audioclip_missing_loop_warns(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, /*loop=*/true);
    if (!checkAudioProbe(am, probe, "test_scene_audioclip_missing_loop_warns")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    nlohmann::json& audioClip = j["root"]["children"][0]["audioClip"];
    CHECK(audioClip.contains("loop"));
    audioClip.erase("loop");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(found->getAudioClip()->getLoop() == false); // default

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("loop") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// Without "path" there is nothing to load: the node is left WITHOUT a clip (not with one
// pointing to the empty string) and the rest of the scene loads all the same. It does not need
// FMOD: createAudioClipComponent is never reached.
static void test_scene_audioclip_missing_path_warns(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    // The audioClip block is injected by hand: building it with a real clip
    // would require FMOD, and here the case that matters is exactly the one that does not use it.
    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["audioClip"] = { {"is3D", false}, {"loop", false},
                                              {"volume", 0.5f}, {"pitch", 1.0f} };

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(!found->hasAudioClip());

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("path") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// A wrong type (not just absence) cannot bring down the scene either:
// "is3D": "true" as a string is what a hand-edited .scene leaves.
static void test_scene_audioclip_wrong_type_warns(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!checkAudioProbe(am, probe, "test_scene_audioclip_wrong_type_warns")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["audioClip"]["is3D"] = "true"; // string, not bool

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(found->getAudioClip()->getIs3D() == false);

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("is3D") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// init() twice cannot leave the previous System or ChannelGroups orphaned.
// There is no way to count FMOD objects from here, so what is checked is the
// observable contract: the second call returns true, the manager is still
// available and keeps loading sounds with the SAME system (if the
// second init had created a new one, the earlier soundIds would point to
// sounds of a System that no longer has an owner).
static void test_init_is_reentrant(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_init_is_reentrant (FMOD no disponible)\n");
        return;
    }
    auto before = am.createAudioClipComponent("assets/audio.mp3", false, false);
    CHECK(before != nullptr);

    CHECK(am.init());       // second call: no-op
    CHECK(am.available());

    auto after = am.createAudioClipComponent("assets/audio.mp3", false, false);
    CHECK(after != nullptr);
    // The clip from before the second init is still usable (its soundId did not
    // end up pointing to an orphaned System).
    if (before) before->setVolume(0.5f);
    CHECK(before && nearlyEqual(before->getVolume(), 0.5f));
}

// The attenuation distances had a clamp and NO test: the ranges
// ([0.1, 50] and [1, 1000]) were only written in a header comment.
static void test_distances_clamp_to_range()
{
    auto clip = makeClip();

    clip->setMinDistance(12.5f);
    CHECK(nearlyEqual(clip->getMinDistance(), 12.5f));
    clip->setMinDistance(-3.0f);
    CHECK(nearlyEqual(clip->getMinDistance(), 0.1f));
    clip->setMinDistance(999.0f);
    CHECK(nearlyEqual(clip->getMinDistance(), 50.0f));

    // After clamping min to 50, max is still at its default of 100: the invariant
    // has not had to be touched yet.
    CHECK(nearlyEqual(clip->getMaxDistance(), 100.0f));

    clip->setMaxDistance(250.0f);
    CHECK(nearlyEqual(clip->getMaxDistance(), 250.0f));
    clip->setMaxDistance(-1.0f);
    CHECK(nearlyEqual(clip->getMaxDistance(), 1.0f));
    clip->setMaxDistance(99999.0f);
    CHECK(nearlyEqual(clip->getMaxDistance(), 1000.0f));
}

// NaN: same hole as volume/pitch (std::clamp(NaN,...) returns NaN, so
// the clamp alone does not stop it) and it ends up the same, as "null" in the .scene.
static void test_distances_reject_nan()
{
    auto clip = makeClip();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    clip->setMinDistance(7.0f);
    clip->setMaxDistance(80.0f);
    clip->setMinDistance(nan);
    clip->setMaxDistance(nan);
    CHECK(nearlyEqual(clip->getMinDistance(), 7.0f));
    CHECK(nearlyEqual(clip->getMaxDistance(), 80.0f));
}

// The min <= max invariant lives in the component, not in the UI: a hand-edited
// .scene cannot install an inverted attenuation either. It is attacked
// from BOTH setters: each one drags the other along, and testing only one left
// half uncovered.
static void test_min_max_invariant_from_both_setters()
{
    // max drops below min: min follows it.
    auto a = makeClip();
    a->setMinDistance(40.0f);
    a->setMaxDistance(10.0f);
    CHECK(nearlyEqual(a->getMaxDistance(), 10.0f));
    CHECK(nearlyEqual(a->getMinDistance(), 10.0f));
    CHECK(a->getMinDistance() <= a->getMaxDistance());

    // min rises above max: max follows it.
    auto b = makeClip();
    b->setMaxDistance(5.0f);
    b->setMinDistance(30.0f);
    CHECK(nearlyEqual(b->getMinDistance(), 30.0f));
    CHECK(nearlyEqual(b->getMaxDistance(), 30.0f));
    CHECK(b->getMinDistance() <= b->getMaxDistance());
}

// Round-trip of ALL the audioClip fields at once, each with a value
// different from its default AND different from the others. Until now only
// volume and pitch were tested: is3D, loop, playOnAwake, minDistance and maxDistance were
// written to the JSON and nobody checked that they were read back; deleting
// any of those five reads in Scene::nodeFromJson passed the whole suite. That the
// seven values are different from each other also uncovers a field swap
// (reading "minDistance" into the max setter, for example).
static void test_all_audioclip_fields_round_trip(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, /*loop=*/true);
    if (!checkAudioProbe(am, probe, "test_all_audioclip_fields_round_trip")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setPlayOnAwake(true);
    probe->setVolume(0.35f);
    probe->setPitch(1.7f);
    probe->setMaxDistance(250.0f);
    probe->setMinDistance(12.5f);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));

    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    const auto& c = found->getAudioClip();
    CHECK(c->getIs3D() == true);
    CHECK(c->getLoop() == true);
    CHECK(c->getPlayOnAwake() == true);
    CHECK(nearlyEqual(c->getVolume(), 0.35f));
    CHECK(nearlyEqual(c->getPitch(), 1.7f));
    CHECK(nearlyEqual(c->getMinDistance(), 12.5f));
    CHECK(nearlyEqual(c->getMaxDistance(), 250.0f));
    CHECK(c->getPath() == "assets/audio.mp3");
}

// THE H2 TEST: a file that does not exist is accepted without a murmur and is only
// detected by querying the state AFTERWARDS. With FMOD_NONBLOCKING, createSound
// returns FMOD_OK even if the path is garbage, so loadSound hands out a valid
// id and createAudioClipComponent a whole component; before this fix
// that was absolute silence: the UI error branch and that of Scene::fromJson
// never got executed, and playSound gave up without saying anything.
//
// The failure shows up on FMOD's internal thread a few frames later, hence the
// loop with update(): it is not an arbitrary wait, it is the same per-frame pump
// that the editor and the runtime do.
static void test_missing_file_reports_load_failure(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_missing_file_reports_load_failure (FMOD no disponible)\n");
        return;
    }
    const std::string bogus = "assets/__no_existe_este_audio__.mp3";
    CHECK(!std::filesystem::exists(bogus));

    auto clip = am.createAudioClipComponent(bogus, false, false);
    // It is created ANYWAY. If createSound ever started failing here, this
    // CHECK gives it away instead of the test going on to test something else.
    CHECK(clip != nullptr);
    if (!clip) return;

    bool failed = false;
    for (int i = 0; i < 2000 && !failed; ++i)
    {
        am.update(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        failed = clip->hasLoadError();
    }
    CHECK(failed);

    std::vector<std::string> out;
    am.pollLoadFailures(out);
    bool reported = false;
    for (const auto& p : out)
        if (p.find("__no_existe_este_audio__") != std::string::npos) reported = true;
    CHECK(reported);

    // And only once: pollLoadFailures is called on EVERY frame, so a broken
    // sound that was always reported would flood the Log Console.
    out.clear();
    am.pollLoadFailures(out);
    for (const auto& p : out)
        CHECK(p.find("__no_existe_este_audio__") == std::string::npos);
}

// The other load failure, and the most treacherous: the file EXISTS, has an
// audio extension, passes the UI whitelist... and is not audio. An asset
// truncated by a merge or a half-finished download gets in through here. It is tested
// apart from the nonexistent path because the TIME to detect it is different: the
// path that does not exist fails instantly (it is not even opened), and this one has to be
// read and discarded on FMOD's internal thread, which takes tens of real
// ms. A single test with the fast case accepted a wait that is not
// valid for the slow case.
//
// Both end up leaving through the SAME branch of getSoundState (the return
// value of getOpenState); it has not been possible to trigger the
// FMOD_OPENSTATE_ERROR branch, and that is noted there.
static void test_corrupt_file_reports_load_failure(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_corrupt_file_reports_load_failure (FMOD no disponible)\n");
        return;
    }
    // Name unique per process. The temporary lives in a shared directory and
    // with a fixed name two simultaneous instances of the test would step on each other;
    // this is hygiene, not the fix for a specific failure: I tried to reproduce
    // an intermittent red that I saw once in this binary and did NOT succeed,
    // neither with a fixed name nor with four instances in parallel. The cause of that
    // failure is still unidentified.
    const std::filesystem::path bogus =
        std::filesystem::temp_directory_path() /
        ("dt_audio_corrupto_" + std::to_string(
             DonTopo::platform::processId()
         ) + ".mp3");
    {
        std::FILE* f = std::fopen(bogus.string().c_str(), "wb");
        if (!f) { std::printf("SKIP test_corrupt_file_reports_load_failure (no se pudo escribir el temporal)\n"); return; }
        const char basura[] = "esto no es un mp3, solo bytes";
        std::fwrite(basura, 1, sizeof(basura), f);
        std::fclose(f);
    }

    auto clip = am.createAudioClipComponent(bogus.string(), false, false);
    CHECK(clip != nullptr);

    // With a real wait between turns, not just iterations: the nonexistent path
    // fails instantly (the open does not even get to open), but a file that DOES
    // exist has to be read and discarded, and FMOD's internal thread does that
    // at its own pace. 2000 turns without sleeping ran out in milliseconds and the
    // test gave a red that was not the code's.
    bool failed = false;
    if (clip)
        for (int i = 0; i < 600 && !failed; ++i)
        {
            am.update(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            failed = clip->hasLoadError();
            if (!failed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    CHECK(failed);

    std::vector<std::string> out;
    am.pollLoadFailures(out);
    bool reported = false;
    for (const auto& p : out)
        if (p.find("dt_audio_corrupto") != std::string::npos) reported = true;
    CHECK(reported);

    // The clip is released before deleting the file: its destructor frees the
    // FMOD::Sound, which until that moment may have the file open.
    clip.reset();
    std::error_code ec;
    std::filesystem::remove(bogus, ec);
}

// Per-frame 3D tracking (H1). SCOPE OF THIS TEST: it covers that the path
// exists and that it withstands the degenerate cases: clip without AudioManager, invalid
// soundId, 2D clip, node without a clip. What it CANNOT cover is the audible
// effect: checking that the voice really moved would require exposing a
// getter for the channel position that nobody else would use, and that is production API
// written only for a test. That attenuation and panning follow the
// object is verified by hand in the editor, and that is noted in the audit.
static void test_updateSpatial_survives_degenerate_cases(AudioManager& am)
{
    // Without a manager and 2D: the m_is3D gate cuts before touching anything.
    auto orphan = makeClip();
    orphan->updateSpatial(glm::vec3(1.0f, 2.0f, 3.0f));
    CHECK(!orphan->getIs3D());

    // With a manager and an invalid soundId: it is the state a clip is left in whose
    // reload() failed. The AudioManager range guards have to absorb it.
    AudioClipComponent bad(&am, "no_existe.mp3", -1, /*is3D=*/true, /*loop=*/false);
    bad.updateSpatial(glm::vec3(5.0f, 0.0f, 0.0f));
    CHECK(bad.getIs3D());
}

// Scene::update has to walk the whole tree without tripping on nodes
// that carry no clip, and without requiring that anything is playing. It is the call
// that makes the tracking happen: if someone deletes it from Scene::update, finding
// H1 comes back; this test would not detect it, but it leaves the path written and
// exercised.
static void test_scene_updateAudioSpatial_walks_tree(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* vacio = scene.addGameObject("sin_audio");
    CHECK(vacio != nullptr);

    auto clip3d = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, false);
    if (clip3d)
    {
        GameObject* go = scene.addGameObject("altavoz");
        go->localTransform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f));
        go->setAudioClip(clip3d);
    }
    scene.getRoot().updateWorldTransforms();

    // With nothing playing: clean no-op on all the nodes.
    scene.updateAudioSpatial();
    // And through the real path, the one that runs in Play.
    scene.update(0.016f);
    CHECK(scene.findById(vacio->id) != nullptr);
}

// H5: removing an Audio Clip lost volume, pitch and the two distances
// forever; Ctrl+Z gave nothing back because the removal did not go through the undo
// stack. The command has to return the component with the SEVEN values, not
// a fresh one with defaults. The snapshot values are all different
// from the default and different from each other: with defaults, a command that restored
// nothing would pass all the same.
static void test_audioclip_command_restores_full_state(AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, /*loop=*/true);
    if (!checkAudioProbe(am, probe, "test_audioclip_command_restores_full_state")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setPlayOnAwake(true);
    probe->setVolume(0.35f);
    probe->setPitch(1.7f);
    probe->setMaxDistance(250.0f);
    probe->setMinDistance(12.5f);
    go->setAudioClip(probe);

    const AudioClipState snapshot{ 0.35f, 1.7f, 12.5f, 250.0f,
                                    /*loop=*/true, /*is3D=*/true, /*playOnAwake=*/true };
    AudioClipComponentCommand cmd(scene, am, "quitar", go->id, /*add=*/false,
                                   "assets/audio.mp3", snapshot);

    cmd.execute();
    CHECK(!go->hasAudioClip());

    cmd.undo();
    CHECK(go->hasAudioClip());
    if (!go->hasAudioClip()) return;
    const auto& c = go->getAudioClip();
    CHECK(nearlyEqual(c->getVolume(), 0.35f));
    CHECK(nearlyEqual(c->getPitch(), 1.7f));
    CHECK(nearlyEqual(c->getMinDistance(), 12.5f));
    CHECK(nearlyEqual(c->getMaxDistance(), 250.0f));
    CHECK(c->getLoop() == true);
    CHECK(c->getIs3D() == true);
    CHECK(c->getPlayOnAwake() == true);
    CHECK(c->getPath() == "assets/audio.mp3");

    // And the redo removes it again: a command that only knew how to undo would leave
    // the stack inconsistent as soon as it was redone.
    cmd.execute();
    CHECK(!go->hasAudioClip());
}

// The listener state is a single bool, and it is exactly the one that can get lost:
// removing a DISABLED listener and undoing has to return it
// disabled. A command that always created a default one would pass
// any test that did not look at this (the default is enabled = true).
static void test_audiolistener_command_preserves_disabled_state()
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("oido");
    auto listener = std::make_shared<AudioListenerComponent>();
    listener->setEnabled(false);
    go->setAudioListener(listener);

    AudioListenerComponentCommand cmd(scene, "quitar", go->id, /*add=*/false, /*enabled=*/false);
    cmd.execute();
    CHECK(!go->hasAudioListener());

    cmd.undo();
    CHECK(go->hasAudioListener());
    if (!go->hasAudioListener()) return;
    CHECK(go->getAudioListener()->getEnabled() == false);
}

// PlayOneShot fires a LOOSE voice: it is not registered in m_sfxChannels, so
// it does not cut the previous one and several shots overlap. That cannot be heard from
// a test, but it does have an observable and exact consequence: after a
// PlayOneShot, isPlaying() still says false, because there is no stored channel
// to ask. If someone "simplified" playOneShot by delegating to
// playSound, that CHECK would turn true and the overlap would be lost.
//
// The previous play() is not decoration: it is the control that shows that the sound
// did get loaded and that isPlaying() knows how to say true. Without it, the final assert
// would also pass with a sound that never played.
static void test_playOneShot_does_not_register_a_channel(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_playOneShot_does_not_register_a_channel (FMOD no disponible)\n");
        return;
    }
    auto clip = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/false, /*loop=*/true);
    if (!checkAudioProbe(am, clip, "test_playOneShot_does_not_register_a_channel")) return;

    const glm::vec3 pos(0.0f);
    auto pump = [&am, &pos]() {
        am.update(pos, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    };
    // FMOD does not respond instantly in either direction: loading
    // happens on its internal thread and the effect of a stop does not have to
    // show up on the same turn either. The two "wait for it to happen" asserts go through
    // this helper with a cap; making them immediate gave an intermittent test, which
    // is worse than not having one.
    auto waitUntil = [&pump](const std::function<bool()>& cond) {
        for (int i = 0; i < 900; ++i)
        {
            pump();
            if (cond()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };

    // Control: the sound does get loaded and played, and isPlaying() knows how to say
    // true. Without this, the assert below would also pass with a clip that never
    // played, which is exactly the false green to avoid.
    const bool playing = waitUntil([&]() { clip->play(pos); return clip->isPlaying(); });
    CHECK(playing);
    if (!playing) return;

    clip->stop();
    CHECK(waitUntil([&]() { return !clip->isPlaying(); }));

    // THE ASSERT THAT MATTERS, and this one is immediate: it is not a condition that
    // FMOD must reach over time, it is that playOneShot does NOT store the channel.
    // isPlaying() asks about the stored channel, so it has to keep
    // saying false on the same turn, whether the voice is sounding or not.
    clip->playOneShot(pos);
    pump();
    CHECK(!clip->isPlaying());

    // And it has not clobbered the reference: a later play() still registers. This
    // rules out playOneShot leaving m_sfxChannels in a state that breaks
    // normal playback.
    CHECK(waitUntil([&]() { clip->play(pos); return clip->isPlaying(); }));
    clip->stop();
}

// The bus travels to the .scene by NAME and comes back. Music is chosen on purpose: it is not
// the default (Sfx), so a fromJson that did not read the field would give Sfx and the
// test would see it.
static void test_bus_round_trip(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!checkAudioProbe(am, probe, "test_bus_round_trip")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setBus(AudioBus::Music);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    // The NAME has to be in the JSON, not the enum index: if someone
    // changed it to an integer, reordering AudioBus would move the bus of all the
    // saved scenes without anybody noticing.
    CHECK(j["root"]["children"][0]["audioClip"]["bus"] == "music");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(found->getAudioClip()->getBus() == AudioBus::Music);
}

// Back-compat: a scene saved before the buses does not carry the field and has
// to load as Sfx, which is where EVERYTHING went out then, so it sounds the same.
// And without a warning: the absence here is legitimate, not corruption.
static void test_scene_without_bus_loads_sfx(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!checkAudioProbe(am, probe, "test_scene_without_bus_loads_sfx")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setBus(AudioBus::Master); // non-neutral: if erase() failed, it would show
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["audioClip"].erase("bus");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(found->getAudioClip()->getBus() == AudioBus::Sfx);

    for (const auto& w : loaded.lastWarnings())
        CHECK(w.find("bus") == std::string::npos);
}

// A name that does NOT exist does warn: it is corruption or a project from a newer
// version. Silently falling to sfx would leave a clip playing through the wrong bus
// with no clue as to why.
static void test_scene_with_unknown_bus_warns(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
    if (!checkAudioProbe(am, probe, "test_scene_with_unknown_bus_warns")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["audioClip"]["bus"] = "ambience_3d";

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(found->getAudioClip()->getBus() == AudioBus::Sfx);

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("bus") != std::string::npos) { warned = true; break; }
    CHECK(warned);
}

// The three volumes are independent: writing one cannot move the other
// two. With only one bus tested, a getBusVolume that always returned the
// master's would pass all the same.
static void test_bus_volumes_are_independent(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_bus_volumes_are_independent (FMOD no disponible)\n");
        return;
    }
    am.setBusVolume(AudioBus::Master, 0.9f);
    am.setBusVolume(AudioBus::Music,  0.4f);
    am.setBusVolume(AudioBus::Sfx,    0.7f);

    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Master), 0.9f));
    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Music),  0.4f));
    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Sfx),    0.7f));

    // Master scales the other two, but does NOT rewrite them: lowering the master cannot
    // change what the user has set in Music or in SFX.
    am.setBusVolume(AudioBus::Master, 0.5f);
    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Music), 0.4f));
    CHECK(nearlyEqual(am.getBusVolume(AudioBus::Sfx),   0.7f));

    // They are left neutral: the tests that follow play sounds and an inherited bus at
    // 0.2 would leave them practically mute with no explanation.
    am.setBusVolume(AudioBus::Music, 1.0f);
    am.setBusVolume(AudioBus::Sfx,   1.0f);
}

// H11: two clips of the SAME file and the SAME mode share a single
// FMOD::Sound. Before, twenty objects with the same shot loaded twenty
// uncompressed copies in RAM.
static void test_same_path_shares_one_sound(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_same_path_shares_one_sound (FMOD no disponible)\n");
        return;
    }
    const size_t before = am.loadedSoundCount();

    auto a = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/false, /*loop=*/false);
    if (!checkAudioProbe(am, a, "test_same_path_shares_one_sound")) return;
    CHECK(am.loadedSoundCount() == before + 1);

    auto b = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/false, /*loop=*/false);
    CHECK(b != nullptr);
    // The H11 assert: the second one does NOT load anything new.
    CHECK(am.loadedSoundCount() == before + 1);

    // But the mode is baked into the FMOD_MODE, so the same file in 3D
    // is ANOTHER sound and cannot be shared. If the cache key were only
    // the path, ticking "Is 3D?" on one clip would change it for the others.
    auto c = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, /*loop=*/false);
    CHECK(c != nullptr);
    CHECK(am.loadedSoundCount() == before + 2);

    // Refcount: releasing one of the two that share CANNOT free the sound,
    // or the one that remains is left with a dead pointer.
    a.reset();
    CHECK(am.loadedSoundCount() == before + 2);
    CHECK(!b->hasLoadError()); // the survivor is still usable

    // And when the last one is released it is freed.
    b.reset();
    CHECK(am.loadedSoundCount() == before + 1);
    c.reset();
    CHECK(am.loadedSoundCount() == before);
}

// H10: ids are recycled. Before, each Play->Stop cycle (which recreates the entire
// scene) added one entry per clip to vectors that only grew.
static void test_sound_slots_are_recycled(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_sound_slots_are_recycled (FMOD no disponible)\n");
        return;
    }
    const size_t before = am.loadedSoundCount();
    // The count of SLOTS, not of live sounds: the two are different and it is
    // exactly the difference that proves the recycling. Without recycling, the live
    // sounds go back to zero all the same (the slot is left at nullptr) but the vector grows
    // one entry per turn; with loadedSoundCount alone, this test passed
    // even if nothing was recycled.
    const size_t slotsBefore = am.soundSlotCount();

    // Ten cycles of create and release. Each Play->Stop cycle of the editor recreates the
    // entire scene, so this is what happened in a normal session.
    for (int i = 0; i < 10; ++i)
    {
        auto clip = am.createAudioClipComponent("assets/audio.mp3", false, false);
        CHECK(clip != nullptr);
        CHECK(am.loadedSoundCount() == before + 1);
        clip.reset();
        CHECK(am.loadedSoundCount() == before);
    }
    CHECK(am.soundSlotCount() == slotsBefore);

    // The recycled id has to be really usable, not just counted:
    // a badly cleaned slot would give a clip that says it is broken or that does not sound.
    auto reused = am.createAudioClipComponent("assets/audio.mp3", false, false);
    CHECK(reused != nullptr);
    if (reused)
    {
        CHECK(!reused->hasLoadError());
        CHECK(reused->getPath() == "assets/audio.mp3");
    }
}

// The load mode travels to the .scene and back, and does NOT share a sound with the same
// file loaded in the other mode: a stream and an uncompressed sample are two
// distinct FMOD::Sound, just like with is3D and loop.
static void test_load_mode_round_trip_and_cache(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false,
                                              AudioLoadMode::Stream);
    if (!checkAudioProbe(am, probe, "test_load_mode_round_trip_and_cache")) return;
    CHECK(probe->getLoadMode() == AudioLoadMode::Stream);

    // That the mode REACHES FMOD, not just that the component remembers it. Without this
    // pair of asserts, deleting the line that applies FMOD_CREATESTREAM left the
    // whole feature without effect and the suite green: the mode was stored,
    // serialized and read back all the same.
    const int streamId = am.loadSound("assets/audio.mp3", false, true, AudioLoadMode::Stream);
    const int sampleId = am.loadSound("assets/audio.mp3", false, true, AudioLoadMode::Sample);
    CHECK(streamId >= 0);
    CHECK(sampleId >= 0);
    // You have to WAIT for the load to finish: with FMOD_NONBLOCKING, getMode()
    // does not yet reflect FMOD_CREATESTREAM while the sound is in
    // OPENSTATE_LOADING. Without this wait the assert always failed, and in a
    // misleading way, because it also "failed" with the sabotage in place and it looked like it
    // was detecting it.
    for (int i = 0; i < 600 && am.getSoundState(streamId) == AudioManager::SoundLoadState::Loading; ++i)
    {
        am.update(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(am.getSoundState(streamId) == AudioManager::SoundLoadState::Ready);
    CHECK(am.isSoundStreaming(streamId));
    CHECK(!am.isSoundStreaming(sampleId));
    am.unloadSound(streamId);
    am.unloadSound(sampleId);

    // Same file and same flags, but loaded as Sample: a separate sound.
    const size_t before = am.loadedSoundCount();
    auto sample = am.createAudioClipComponent("assets/audio.mp3", false, false,
                                               AudioLoadMode::Sample);
    CHECK(sample != nullptr);
    CHECK(am.loadedSoundCount() == before + 1);
    sample.reset();

    Scene scene("Test");
    GameObject* go = scene.addGameObject("musica");
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    CHECK(j["root"]["children"][0]["audioClip"]["loadMode"] == "stream");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    CHECK(found->getAudioClip()->getLoadMode() == AudioLoadMode::Stream);
}

// Back-compat: a scene prior to this feature does not carry the field and loads as
// Sample, which is how EVERYTHING was loaded then. Without a warning: the absence is
// legitimate. An unknown name does warn.
static void test_load_mode_back_compat_and_unknown(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false,
                                              AudioLoadMode::Stream);
    if (!checkAudioProbe(am, probe, "test_load_mode_back_compat_and_unknown")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("musica");
    go->setAudioClip(probe);
    nlohmann::json base = scene.toJson();

    {
        nlohmann::json j = base;
        j["root"]["children"][0]["audioClip"].erase("loadMode");
        Scene loaded("Loaded");
        CHECK(loaded.fromJson(j, pm, am));
        GameObject* found = loaded.findById(go->id);
        if (!found || !found->hasAudioClip()) { CHECK(false); return; }
        CHECK(found->getAudioClip()->getLoadMode() == AudioLoadMode::Sample);
        for (const auto& w : loaded.lastWarnings())
            CHECK(w.find("loadMode") == std::string::npos);
    }
    {
        nlohmann::json j = base;
        j["root"]["children"][0]["audioClip"]["loadMode"] = "compressed_in_memory";
        Scene loaded("Loaded");
        CHECK(loaded.fromJson(j, pm, am));
        GameObject* found = loaded.findById(go->id);
        if (!found || !found->hasAudioClip()) { CHECK(false); return; }
        CHECK(found->getAudioClip()->getLoadMode() == AudioLoadMode::Sample);
        bool warned = false;
        for (const auto& w : loaded.lastWarnings())
            if (w.find("loadMode") != std::string::npos) { warned = true; break; }
        CHECK(warned);
    }
}

// H16: the extension whitelist lived ONLY in the UI path, so a
// .scene with any extension created the clip all the same and, because of FMOD's
// deferred load, the only symptom was silence. Now scene loading names it
// and discards the clip, letting the rest load.
static void test_scene_rejects_unsupported_extension(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go   = scene.addGameObject("altavoz");
    GameObject* otro = scene.addGameObject("sin_audio");

    nlohmann::json j = scene.toJson();
    j["root"]["children"][0]["audioClip"] = { {"path", "assets/musica.xyz"},
                                              {"is3D", false}, {"loop", false} };

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    // The rest of the scene loads: discarding the clip cannot take an
    // unrelated object with it.
    CHECK(loaded.findById(otro->id) != nullptr);

    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found) return;
    CHECK(!found->hasAudioClip());

    bool warned = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("unsupported") != std::string::npos) { warned = true; break; }
    CHECK(warned);

    // And the control case: the SAME path with a valid extension does create the
    // clip. Without this, a filter that always rejected would pass the test above.
    if (am.available())
    {
        nlohmann::json ok = scene.toJson();
        ok["root"]["children"][0]["audioClip"] = { {"path", "assets/audio.mp3"},
                                                    {"is3D", false}, {"loop", false} };
        Scene loadedOk("LoadedOk");
        CHECK(loadedOk.fromJson(ok, pm, am));
        GameObject* f2 = loadedOk.findById(go->id);
        CHECK(f2 != nullptr && f2->hasAudioClip());
    }
}

// PlayClipAtPoint retains the sound in the cache after the first use, and the
// following calls do NOT count again. It is the delicate part: without the
// retained set, each shot would raise the refcount once more and the sound
// would become impossible to free (it would not leak memory, but the counter would grow
// without a ceiling and the diagnostics would lie).
static void test_playClipAtPoint_pins_sound_once(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_playClipAtPoint_pins_sound_once (FMOD no disponible)\n");
        return;
    }
    const size_t before = am.loadedSoundCount();
    const glm::vec3 pos(10.0f, 0.0f, 0.0f);

    am.playClipAtPoint("assets/audio.mp3", pos);
    const size_t afterFirst = am.loadedSoundCount();
    // The first time it does load: it is a non-loop 3D sound, different from those
    // used by the other tests (which go in 2D).
    CHECK(afterFirst == before + 1);

    // And ten more shots load nothing new.
    for (int i = 0; i < 10; ++i)
        am.playClipAtPoint("assets/audio.mp3", pos);
    CHECK(am.loadedSoundCount() == afterFirst);

    // Preload of the same path neither: it is idempotent.
    am.preloadClip("assets/audio.mp3");
    CHECK(am.loadedSoundCount() == afterFirst);

    // The retained sound CANNOT be released from outside: an AudioClipComponent
    // that uses that same path and mode shares the slot, and on being destroyed it cannot
    // take away the sound that playClipAtPoint keeps alive.
    {
        auto clip = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, /*loop=*/false);
        CHECK(clip != nullptr);
        CHECK(am.loadedSoundCount() == afterFirst); // shares, does not load another
    }
    CHECK(am.loadedSoundCount() == afterFirst); // and stays alive after destroying it

    // A path with an unsupported extension loads nothing (the whitelist lives in
    // the Lua binding, but the engine must not create a sound out of nothing either).
    am.playClipAtPoint("assets/__no_existe__.mp3", pos);
    // It does go up: the path is valid as an extension even if the file does not exist;
    // that is reported by pollLoadFailures, not this path. It is checked that at least
    // it breaks nothing and that the failure can be observed through the usual channel.
    std::vector<std::string> failures;
    for (int i = 0; i < 600 && failures.empty(); ++i)
    {
        am.update(pos, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        am.pollLoadFailures(failures);
        if (failures.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bool reported = false;
    for (const auto& f : failures)
        if (f.find("__no_existe__") != std::string::npos) reported = true;
    CHECK(reported);
}

// P12: rolloff, spread, panning and doppler. Clamps and rejection of non-finite values, with the
// same criterion as volume/pitch: a NaN would end up in the .scene as "null".
static void test_p12_props_clamp_and_reject_nan()
{
    auto clip = makeClip();
    const float nan = std::numeric_limits<float>::quiet_NaN();

    CHECK(nearlyEqual(clip->getSpread(), 0.0f));        // neutral by default:
    CHECK(nearlyEqual(clip->getStereoPan(), 0.0f));     // an old scene sounds
    CHECK(nearlyEqual(clip->getDopplerLevel(), 0.0f));  // exactly the same
    CHECK(clip->getRolloff() == AudioRolloff::Inverse);

    clip->setSpread(90.0f);
    CHECK(nearlyEqual(clip->getSpread(), 90.0f));
    clip->setSpread(-10.0f);
    CHECK(nearlyEqual(clip->getSpread(), 0.0f));
    clip->setSpread(1000.0f);
    CHECK(nearlyEqual(clip->getSpread(), 360.0f));
    clip->setSpread(nan);
    CHECK(nearlyEqual(clip->getSpread(), 360.0f)); // keeps the previous one

    clip->setStereoPan(-0.5f);
    CHECK(nearlyEqual(clip->getStereoPan(), -0.5f));
    clip->setStereoPan(-9.0f);
    CHECK(nearlyEqual(clip->getStereoPan(), -1.0f));
    clip->setStereoPan(9.0f);
    CHECK(nearlyEqual(clip->getStereoPan(), 1.0f));
    clip->setStereoPan(nan);
    CHECK(nearlyEqual(clip->getStereoPan(), 1.0f));

    clip->setDopplerLevel(2.5f);
    CHECK(nearlyEqual(clip->getDopplerLevel(), 2.5f));
    clip->setDopplerLevel(-1.0f);
    CHECK(nearlyEqual(clip->getDopplerLevel(), 0.0f));
    clip->setDopplerLevel(50.0f);
    CHECK(nearlyEqual(clip->getDopplerLevel(), 5.0f));
    clip->setDopplerLevel(nan);
    CHECK(nearlyEqual(clip->getDopplerLevel(), 5.0f));
}

// Round-trip of the four, and the cache separates them by rolloff: the curve goes in the
// FMOD_MODE, so the same file with a linear curve is ANOTHER sound. If the
// rolloff did not enter the key, changing one clip's curve would change it for
// all those that share the file.
static void test_p12_round_trip_and_cache(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", /*is3D=*/true, false);
    if (!checkAudioProbe(am, probe, "test_p12_round_trip_and_cache")) return;

    probe->setRolloff(AudioRolloff::Linear);

    // The rolloff has to enter the cache key: it goes in the FMOD_MODE,
    // so the same file with two curves is two sounds. Without this,
    // changing a clip's curve would change it for all those that share the
    // file.
    //
    // It is measured with a direct loadSound and with a flag combination that no
    // other test uses (3D + loop): this binary shares AudioManager among
    // tests and several leave sounds retained (playClipAtPoint pins exactly
    // 3D/no-loop/Sample/Inverse), so relying on whatever happens to be loaded
    // is tying oneself to the execution order. It already happened to me when writing this test.
    {
        const size_t base = am.loadedSoundCount();
        const int inverse = am.loadSound("assets/audio.mp3", true, true,
                                          AudioLoadMode::Sample, AudioRolloff::Inverse);
        CHECK(inverse >= 0);
        CHECK(am.loadedSoundCount() == base + 1);

        const int linear = am.loadSound("assets/audio.mp3", true, true,
                                         AudioLoadMode::Sample, AudioRolloff::Linear);
        CHECK(linear >= 0);
        CHECK(linear != inverse);
        CHECK(am.loadedSoundCount() == base + 2);

        // And that the curve REACHES FMOD, not just that the component remembers it.
        // You have to wait for Ready: with FMOD_NONBLOCKING the mode is not
        // complete while loading (the same trap as with CREATESTREAM).
        for (int i = 0; i < 600 && am.getSoundState(linear) == AudioManager::SoundLoadState::Loading; ++i)
        {
            am.update(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(am.getSoundState(linear) == AudioManager::SoundLoadState::Ready);
        CHECK(am.getSoundRolloff(linear) == AudioRolloff::Linear);
        CHECK(am.getSoundRolloff(inverse) == AudioRolloff::Inverse);

        am.unloadSound(inverse);
        am.unloadSound(linear);
        CHECK(am.loadedSoundCount() == base);
    }

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setSpread(120.0f);
    probe->setStereoPan(-0.75f);
    probe->setDopplerLevel(3.0f);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    CHECK(j["root"]["children"][0]["audioClip"]["rolloff"] == "linear");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    const auto& c = found->getAudioClip();
    CHECK(c->getRolloff() == AudioRolloff::Linear);
    CHECK(nearlyEqual(c->getSpread(), 120.0f));
    CHECK(nearlyEqual(c->getStereoPan(), -0.75f));
    CHECK(nearlyEqual(c->getDopplerLevel(), 3.0f));
}

// Back-compat: a scene prior to P12 carries none of the four fields and
// has to load with the neutral ones, without warnings.
static void test_p12_back_compat(PhysicsManager& pm, AudioManager& am)
{
    auto probe = am.createAudioClipComponent("assets/audio.mp3", true, false);
    if (!checkAudioProbe(am, probe, "test_p12_back_compat")) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("altavoz");
    probe->setSpread(200.0f);
    probe->setDopplerLevel(4.0f);
    go->setAudioClip(probe);

    nlohmann::json j = scene.toJson();
    auto& ac = j["root"]["children"][0]["audioClip"];
    ac.erase("rolloff"); ac.erase("spread"); ac.erase("stereoPan"); ac.erase("dopplerLevel");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    if (!found || !found->hasAudioClip()) { CHECK(false); return; }
    const auto& c = found->getAudioClip();
    CHECK(c->getRolloff() == AudioRolloff::Inverse);
    CHECK(nearlyEqual(c->getSpread(), 0.0f));
    CHECK(nearlyEqual(c->getStereoPan(), 0.0f));
    CHECK(nearlyEqual(c->getDopplerLevel(), 0.0f));
    for (const auto& w : loaded.lastWarnings())
    {
        CHECK(w.find("rolloff") == std::string::npos);
        CHECK(w.find("spread") == std::string::npos);
        CHECK(w.find("doppler") == std::string::npos);
    }
}

// P13: DSP effects per bus. What matters most here is not that the filter sounds
// (that cannot be measured headless) but that the DSPs are created ONCE and
// released: they are native FMOD resources, and the classic leak of this kind of API
// is chaining one DSP per frame while a slider is moved.
static void test_bus_effects_are_idempotent_and_released(AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_bus_effects_are_idempotent_and_released (FMOD no disponible)\n");
        return;
    }
    CHECK(am.activeEffectCount() == 0);
    CHECK(!am.hasBusEffect(AudioBus::Music, AudioEffect::LowPass));

    am.setBusEffect(AudioBus::Music, AudioEffect::LowPass, 0.2f);
    CHECK(am.activeEffectCount() == 1);
    CHECK(am.hasBusEffect(AudioBus::Music, AudioEffect::LowPass));

    // THE ASSERT THAT MATTERS: a hundred consecutive readjustments (what a slider does
    // when dragged) cannot create a hundred DSPs.
    //
    // It is measured by asking FMOD how many DSPs the group has, NOT by counting the
    // map entries: if setBusEffect chained a new one every time, the new
    // entry would overwrite the old one and the map would keep saying 1 while the
    // group accumulates a hundred lost DSPs. With activeEffectCount, this test passed
    // with the idempotence sabotaged.
    const size_t dspsWithOneEffect = am.busDspCount(AudioBus::Music);
    for (int i = 0; i < 100; ++i)
        am.setBusEffect(AudioBus::Music, AudioEffect::LowPass, i / 100.0f);
    CHECK(am.activeEffectCount() == 1);
    CHECK(am.busDspCount(AudioBus::Music) == dspsWithOneEffect);

    // Different effects and different buses are different instances: a
    // lowPass on Music and another on SFX do not step on each other.
    am.setBusEffect(AudioBus::Music, AudioEffect::Reverb, 0.5f);
    am.setBusEffect(AudioBus::Sfx,   AudioEffect::LowPass, 0.5f);
    CHECK(am.activeEffectCount() == 3);
    CHECK(am.hasBusEffect(AudioBus::Sfx, AudioEffect::LowPass));
    CHECK(!am.hasBusEffect(AudioBus::Sfx, AudioEffect::Reverb));

    // Removing just one leaves the others.
    am.clearBusEffect(AudioBus::Music, AudioEffect::LowPass);
    CHECK(am.activeEffectCount() == 2);
    CHECK(!am.hasBusEffect(AudioBus::Music, AudioEffect::LowPass));
    CHECK(am.hasBusEffect(AudioBus::Music, AudioEffect::Reverb));

    // Removing something that is not set is a no-op, not a failure.
    am.clearBusEffect(AudioBus::Music, AudioEffect::LowPass);
    CHECK(am.activeEffectCount() == 2);

    // And the per-bus sweep takes its own and only its own.
    am.clearBusEffects(AudioBus::Music);
    CHECK(am.activeEffectCount() == 1);
    CHECK(am.hasBusEffect(AudioBus::Sfx, AudioEffect::LowPass));

    am.clearBusEffects(AudioBus::Sfx);
    CHECK(am.activeEffectCount() == 0);

    // All four types can be created: if any did not exist in this version
    // of FMOD, createDSPByType would fail and the counter would say so.
    am.setBusEffect(AudioBus::Master, AudioEffect::LowPass,  0.5f);
    am.setBusEffect(AudioBus::Master, AudioEffect::HighPass, 0.5f);
    am.setBusEffect(AudioBus::Master, AudioEffect::Echo,     0.5f);
    am.setBusEffect(AudioBus::Master, AudioEffect::Reverb,   0.5f);
    CHECK(am.activeEffectCount() == 4);
    am.clearBusEffects(AudioBus::Master);
    CHECK(am.activeEffectCount() == 0);
}

// Reverb zones: idempotent creation, round-trip through the .scene and (what
// matters most) that the zone of a deleted GameObject is released. Without that, its
// reverb would keep being applied to the whole scene forever and there would be no way
// to remove it.
static void test_reverb_zones(PhysicsManager& pm, AudioManager& am)
{
    if (!am.available())
    {
        std::printf("SKIP test_reverb_zones (FMOD no disponible)\n");
        return;
    }
    am.clearReverbZones();
    CHECK(am.reverbZoneCount() == 0);

    Scene scene("Test");
    GameObject* cueva = scene.addGameObject("cueva");
    GameObject* sala  = scene.addGameObject("sala");
    auto z1 = std::make_shared<ReverbZoneComponent>();
    z1->setPreset("cave");
    z1->setMaxDistance(500.0f);
    z1->setMinDistance(120.0f);
    cueva->setReverbZone(z1);
    auto z2 = std::make_shared<ReverbZoneComponent>();
    z2->setPreset("bathroom");
    sala->setReverbZone(z2);
    scene.getRoot().updateWorldTransforms();

    scene.syncReverbZones(am);
    CHECK(am.reverbZoneCount() == 2);

    // Idempotent: a hundred frames do not create a hundred zones.
    for (int i = 0; i < 100; ++i) scene.syncReverbZones(am);
    CHECK(am.reverbZoneCount() == 2);

    // THE ASSERT THAT MATTERS: removing the component releases its zone on the
    // next sync.
    sala->setReverbZone(nullptr);
    scene.syncReverbZones(am);
    CHECK(am.reverbZoneCount() == 1);

    // And deleting the whole GameObject, too.
    scene.removeGameObject(cueva);
    scene.syncReverbZones(am);
    CHECK(am.reverbZoneCount() == 0);

    // A made-up preset does not install a zone with some arbitrary ambience.
    CHECK(!am.syncReverbZone(999, glm::vec3(0.0f), 10.0f, 100.0f, "catedral_submarina", true));
    CHECK(am.reverbZoneCount() == 0);

    // Round-trip through the scene, with non-neutral values different from each other.
    Scene s2("RT");
    GameObject* go = s2.addGameObject("ambiente");
    auto z = std::make_shared<ReverbZoneComponent>();
    z->setPreset("hangar");
    z->setMaxDistance(777.0f);
    z->setMinDistance(88.0f);
    z->setEnabled(false);
    go->setReverbZone(z);

    nlohmann::json j = s2.toJson();
    CHECK(j["root"]["children"][0]["reverbZone"]["preset"] == "hangar");

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(go->id);
    CHECK(found != nullptr);
    if (!found || !found->hasReverbZone()) { CHECK(false); return; }
    const auto& lz = found->getReverbZone();
    CHECK(lz->getPreset() == "hangar");
    CHECK(nearlyEqual(lz->getMinDistance(), 88.0f));
    CHECK(nearlyEqual(lz->getMaxDistance(), 777.0f));
    CHECK(lz->getEnabled() == false);

    // Unknown preset in the .scene: it warns and falls to "room".
    nlohmann::json bad = j;
    bad["root"]["children"][0]["reverbZone"]["preset"] = "cripta";
    Scene loadedBad("Bad");
    CHECK(loadedBad.fromJson(bad, pm, am));
    GameObject* fb = loadedBad.findById(go->id);
    if (!fb || !fb->hasReverbZone()) { CHECK(false); return; }
    CHECK(fb->getReverbZone()->getPreset() == "room");
    bool warned = false;
    for (const auto& w : loadedBad.lastWarnings())
        if (w.find("preset") != std::string::npos) { warned = true; break; }
    CHECK(warned);

    // And the min <= max invariant, attacked from the two setters.
    ReverbZoneComponent inv;
    inv.setMinDistance(400.0f);
    inv.setMaxDistance(50.0f);
    CHECK(inv.getMinDistance() <= inv.getMaxDistance());
    ReverbZoneComponent inv2;
    inv2.setMaxDistance(30.0f);
    inv2.setMinDistance(900.0f);
    CHECK(inv2.getMinDistance() <= inv2.getMaxDistance());

    am.clearReverbZones();
}

// Mute, playback time and global pause: the three "Missing" rows of the
// audit that had not been discarded with a criterion.
//
// Mute is the only one of the three that is serialized: pause and position are
// voice state, and saving them would make a freshly loaded scene start
// halfway through a clip or in silence without anyone having asked for it.
static void test_mute_time_and_global_pause(PhysicsManager& pm, AudioManager& am)
{
    // Part without FMOD: the component state and its round-trip.
    auto solo = makeClip();
    CHECK(solo->getMute() == false);   // neutral: an old scene is not born mute
    solo->setMute(true);
    CHECK(solo->getMute() == true);
    // Without a manager there is no voice to ask the position of.
    CHECK(solo->getTime() < 0.0f);

    if (!am.available())
    {
        std::printf("SKIP test_mute_time_and_global_pause (FMOD no disponible)\n");
        return;
    }

    // Round-trip of the mute through the scene, with the NON-neutral value.
    {
        auto probe = am.createAudioClipComponent("assets/audio.mp3", false, false);
        if (!checkAudioProbe(am, probe, "test_mute_time_and_global_pause")) return;
        Scene scene("Test");
        GameObject* go = scene.addGameObject("altavoz");
        probe->setMute(true);
        go->setAudioClip(probe);

        nlohmann::json j = scene.toJson();
        CHECK(j["root"]["children"][0]["audioClip"]["mute"] == true);

        Scene loaded("Loaded");
        CHECK(loaded.fromJson(j, pm, am));
        GameObject* found = loaded.findById(go->id);
        if (!found || !found->hasAudioClip()) { CHECK(false); return; }
        CHECK(found->getAudioClip()->getMute() == true);

        // Back-compat: without the field, not muted and without a warning.
        nlohmann::json old = j;
        old["root"]["children"][0]["audioClip"].erase("mute");
        Scene l2("Old");
        CHECK(l2.fromJson(old, pm, am));
        GameObject* f2 = l2.findById(go->id);
        if (!f2 || !f2->hasAudioClip()) { CHECK(false); return; }
        CHECK(f2->getAudioClip()->getMute() == false);
        for (const auto& w : l2.lastWarnings())
            CHECK(w.find("mute") == std::string::npos);
    }

    // Playback position on a real voice.
    {
        auto clip = am.createAudioClipComponent("assets/audio.mp3", false, /*loop=*/true);
        if (!clip) { CHECK(false); return; }
        const glm::vec3 pos(0.0f);

        // With nothing playing: -1, which is different from "it is at second 0".
        CHECK(clip->getTime() < 0.0f);

        bool playing = false;
        for (int i = 0; i < 600 && !playing; ++i)
        {
            clip->play(pos);
            am.update(pos, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            playing = clip->isPlaying();
            if (!playing) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(playing);
        if (playing)
        {
            CHECK(clip->getTime() >= 0.0f);   // now there is a position
            clip->setTime(1.0f);
            am.update(pos, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            // A range is checked, not equality: the mixer keeps
            // advancing between the seek and the read.
            const float t = clip->getTime();
            CHECK(t >= 0.9f && t <= 2.0f);
        }
        clip->stop();
    }

    // Global pause: it is the master's, so it also freezes what comes out of
    // any bus.
    CHECK(!am.isAudioPaused());
    am.setAudioPaused(true);
    CHECK(am.isAudioPaused());
    am.setAudioPaused(false);
    CHECK(!am.isAudioPaused());
}

// ── Audio import settings (sidecar) ────────────────────────────────

// 16-bit PCM WAV generated in the test: `channels` channels, silence, `seconds`
// of duration. It serves to test gain and mono without depending on an asset.
static void writeWav(const std::filesystem::path& p, int channels, double seconds)
{
    const uint32_t rate = 44100;
    const uint32_t frames = static_cast<uint32_t>(rate * seconds);
    const uint16_t ch = static_cast<uint16_t>(channels);
    const uint32_t dataBytes = frames * ch * 2;
    auto w32 = [](std::ofstream& o, uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
    auto w16 = [](std::ofstream& o, uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
    std::ofstream o(p, std::ios::binary);
    o.write("RIFF", 4); w32(o, 36 + dataBytes); o.write("WAVE", 4);
    o.write("fmt ", 4); w32(o, 16); w16(o, 1); w16(o, ch); w32(o, rate);
    w32(o, rate * ch * 2); w16(o, static_cast<uint16_t>(ch * 2)); w16(o, 16);
    o.write("data", 4); w32(o, dataBytes);
    const std::string zeros(dataBytes, '\0');
    o.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
}

static bool waitReady(AudioManager& am, int id)
{
    for (int i = 0; i < 600 && am.getSoundState(id) == AudioManager::SoundLoadState::Loading; ++i)
    {
        am.update(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return am.getSoundState(id) == AudioManager::SoundLoadState::Ready;
}

static std::filesystem::path audioTestDir(const char* name)
{
    std::error_code ec;
    std::filesystem::path d = std::filesystem::temp_directory_path(ec) / name;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}

static void setGain(const std::filesystem::path& clip, float db, bool mono = false)
{
    AudioImportSettings s;
    s.gainDb    = db;
    s.forceMono = mono;
    std::string err;
    CHECK(saveAudioImportSettings(clip, s, &err));
}

// Without FMOD (or without a sound) the getters are neutral.
static void test_import_getters_are_neutral_without_a_sound(AudioManager& am)
{
    CHECK(isDefault(am.getSoundImportSettings(-1)));
    CHECK(isDefault(am.getSoundImportSettings(123456)));
    CHECK(am.getChannelVolume(-1) < 0.0f);
    CHECK(am.getChannelVolume(123456) < 0.0f);
    am.refreshImportSettings("no/existe.wav");           // no-op without sounds, without throwing
    am.refreshImportSettings("");
}

static void test_import_gain_applies_and_refreshes(AudioManager& am)
{
    if (!am.available()) { std::printf("SKIP test_import_gain_applies_and_refreshes (FMOD no disponible)\n"); return; }
    const auto d = audioTestDir("dt_audio_import_gain");
    const std::string a = (d / "a.wav").string();
    const std::string b = (d / "b.wav").string();
    writeWav(a, 2, 3.0);
    writeWav(b, 2, 3.0);
    setGain(a, -6.0f);                                   // factor 0.501187

    // a as 2D and as 3D (different flags = two sounds), and b without a sidecar.
    const int a2d = am.loadSound(a, false, true);
    const int a3d = am.loadSound(a, true, true);
    const int bId = am.loadSound(b, false, true);
    if (a2d < 0 || a3d < 0 || bId < 0) { CHECK(false); return; }
    CHECK(a2d != a3d);
    if (!waitReady(am, a2d) || !waitReady(am, a3d) || !waitReady(am, bId)) { CHECK(false); return; }

    CHECK(am.getSoundImportSettings(a2d).gainDb == -6.0f);
    CHECK(am.getSoundImportSettings(a3d).gainDb == -6.0f);
    CHECK(isDefault(am.getSoundImportSettings(bId)));

    am.playSound(a2d, {}, 0.8f);
    am.playSound(a3d, {}, 0.8f);
    am.playSound(bId, {}, 0.8f);
    CHECK(nearlyEqual(am.getChannelVolume(a2d), 0.8f * 0.501187f, 1e-3f));
    CHECK(nearlyEqual(am.getChannelVolume(a3d), 0.8f * 0.501187f, 1e-3f));
    CHECK(am.getChannelVolume(bId) == 0.8f);             // without a sidecar: EXACT, as before

    // Review Focus 3: the refresh changes both ids of a and NOT that of b.
    setGain(a, 6.0f);                                    // factor 1.995262
    am.refreshImportSettings(a);
    CHECK(am.getSoundImportSettings(a2d).gainDb == 6.0f);
    CHECK(nearlyEqual(am.getChannelVolume(a2d), 0.8f * 1.995262f, 1e-3f));
    CHECK(nearlyEqual(am.getChannelVolume(a3d), 0.8f * 1.995262f, 1e-3f));
    CHECK(am.getChannelVolume(bId) == 0.8f);

    // Review Focus 4: after refreshing, a setChannelVolume of the component does NOT
    // accumulate the gain (it is not channelVolume x factor).
    am.setChannelVolume(a2d, 0.5f);
    CHECK(nearlyEqual(am.getChannelVolume(a2d), 0.5f * 1.995262f, 1e-3f));
    am.refreshImportSettings(a);                         // refresh again with the same sidecar
    CHECK(nearlyEqual(am.getChannelVolume(a2d), 0.5f * 1.995262f, 1e-3f));

    // Review Focus 5: removing the sidecar and refreshing gives back the clean volume.
    setGain(a, 0.0f);                                    // the default deletes the file
    CHECK(!std::filesystem::exists(importSidecarPath(a)));
    am.refreshImportSettings(a);
    CHECK(isDefault(am.getSoundImportSettings(a2d)));
    CHECK(am.getChannelVolume(a2d) == 0.5f);

    // A different or nonexistent path touches nothing.
    am.refreshImportSettings((d / "otra.wav").string());
    CHECK(am.getChannelVolume(bId) == 0.8f);

    am.stopSound(a2d); am.stopSound(a3d); am.stopSound(bId);
    am.unloadSound(a2d); am.unloadSound(a3d); am.unloadSound(bId);
}

// Review Focus 2: a recycled slot does not inherit gain or saved volume.
static void test_import_recycled_slot_does_not_inherit(AudioManager& am)
{
    if (!am.available()) { std::printf("SKIP test_import_recycled_slot_does_not_inherit (FMOD no disponible)\n"); return; }
    const auto d = audioTestDir("dt_audio_import_recycle");
    const std::string a = (d / "a.wav").string();
    const std::string b = (d / "b.wav").string();
    writeWav(a, 2, 3.0);
    writeWav(b, 2, 3.0);
    setGain(a, 12.0f);

    const int idA = am.loadSound(a, false, true);
    if (idA < 0 || !waitReady(am, idA)) { CHECK(false); return; }
    am.playSound(idA, {}, 0.9f);
    CHECK(am.getSoundImportSettings(idA).gainDb == 12.0f);
    am.stopSound(idA);
    am.unloadSound(idA);                                  // frees the slot

    const int idB = am.loadSound(b, false, true);         // without a sidecar: reuses a's slot
    if (idB < 0 || !waitReady(am, idB)) { CHECK(false); return; }
    CHECK(idB == idA);                                    // the test really exercises the recycling
    CHECK(isDefault(am.getSoundImportSettings(idB)));
    am.playSound(idB, {}, 0.7f);
    CHECK(am.getChannelVolume(idB) == 0.7f);
    // refreshing b does not put a's gain into it.
    am.refreshImportSettings(b);
    CHECK(am.getChannelVolume(idB) == 0.7f);
    am.stopSound(idB);
    am.unloadSound(idB);
}

// Without FMOD the getter is neutral.
static void test_mono_getter_is_neutral_without_a_voice(AudioManager& am)
{
    CHECK(!am.isVoiceForcedMono(-1));
    CHECK(!am.isVoiceForcedMono(123456));
}

static void test_import_force_mono_on_a_2d_stereo_voice(AudioManager& am)
{
    if (!am.available()) { std::printf("SKIP test_import_force_mono_on_a_2d_stereo_voice (FMOD no disponible)\n"); return; }
    const auto d = audioTestDir("dt_audio_import_mono");
    const std::string stereoMono = (d / "m.wav").string();   // stereo WITH forceMono
    const std::string stereoPlain = (d / "p.wav").string();  // stereo without a sidecar
    const std::string oneCh = (d / "one.wav").string();      // one channel WITH forceMono
    const std::string stereo3d = (d / "s3d.wav").string();   // stereo WITH forceMono, loaded 3D
    writeWav(stereoMono, 2, 3.0);
    writeWav(stereoPlain, 2, 3.0);
    writeWav(oneCh, 1, 3.0);
    writeWav(stereo3d, 2, 3.0);
    setGain(stereoMono, 0.0f, /*mono=*/true);
    setGain(oneCh, 0.0f, true);
    setGain(stereo3d, 0.0f, true);

    const int m  = am.loadSound(stereoMono, false, true);
    const int p  = am.loadSound(stereoPlain, false, true);
    const int o  = am.loadSound(oneCh, false, true);
    const int s3 = am.loadSound(stereo3d, true, true);
    if (m < 0 || p < 0 || o < 0 || s3 < 0) { CHECK(false); return; }
    if (!waitReady(am, m) || !waitReady(am, p) || !waitReady(am, o) || !waitReady(am, s3)) { CHECK(false); return; }
    CHECK(am.getSoundImportSettings(m).forceMono);

    am.playSound(m, {}, 1.0f);
    am.playSound(p, {}, 1.0f);
    am.playSound(o, {}, 1.0f);
    am.playSound(s3, {}, 1.0f);
    CHECK(am.isVoiceForcedMono(m));            // 2D stereo with the setting: mixed down to mono
    CHECK(!am.isVoiceForcedMono(p));           // without the setting: factory matrix
    CHECK(!am.isVoiceForcedMono(o));           // Review Focus 6: one channel, no-op without a crash
    CHECK(!am.isVoiceForcedMono(s3));          // Review Focus 6: 3D, it is not touched
    CHECK(am.isSoundPlaying(o));               // and it keeps playing

    // Another playback of the same voice keeps the mono (it is applied on every start).
    am.stopSound(m);
    am.playSound(m, {}, 1.0f);
    CHECK(am.isVoiceForcedMono(m));

    // Final review fix: FMOD's setPan REPLACES the whole mix matrix, so
    // with Stereo Pan != 0 the mono was silently lost. With panning, the voice
    // is still mono (the matrix carries the panning inside).
    am.stopSound(m);
    am.playSound(m, {}, 1.0f, 1.0f, AudioBus::Sfx, 1.0f, 100.0f, 0.0f, /*stereoPan=*/-0.5f);
    CHECK(am.isVoiceForcedMono(m));
    am.stopSound(m);
    am.playSound(m, {}, 1.0f, 1.0f, AudioBus::Sfx, 1.0f, 100.0f, 0.0f, /*stereoPan=*/0.75f);
    CHECK(am.isVoiceForcedMono(m));

    am.stopSound(m); am.stopSound(p); am.stopSound(o); am.stopSound(s3);
    am.unloadSound(m); am.unloadSound(p); am.unloadSound(o); am.unloadSound(s3);
}

// shutdown() empties the per-sound vectors; the import settings ones have
// to be emptied along with them. Otherwise, after shutdown()+init() the new sound is again
// id 0 but m_soundImport keeps the old entries and their settings.
static void test_import_settings_do_not_survive_shutdown_and_init(AudioManager& am)
{
    if (!am.available()) { std::printf("SKIP test_import_settings_do_not_survive_shutdown_and_init (FMOD no disponible)\n"); return; }
    const auto d = audioTestDir("dt_audio_import_shutdown");
    const std::string a = (d / "a.wav").string();
    const std::string b = (d / "b.wav").string();
    writeWav(a, 2, 3.0);
    writeWav(b, 2, 3.0);
    setGain(a, 9.0f, true);

    am.shutdown();
    CHECK(am.init());
    if (!am.available()) return;

    const int idA = am.loadSound(a, false, true);
    if (idA < 0 || !waitReady(am, idA)) { CHECK(false); return; }
    CHECK(am.getSoundImportSettings(idA).gainDb == 9.0f);
    am.unloadSound(idA);

    am.shutdown();                                        // empties everything again
    CHECK(am.init());
    if (!am.available()) return;
    const int idB = am.loadSound(b, false, true);         // without a sidecar: id 0 again
    if (idB < 0 || !waitReady(am, idB)) { CHECK(false); return; }
    CHECK(idB == idA);
    CHECK(isDefault(am.getSoundImportSettings(idB)));
    am.playSound(idB, {}, 0.6f);
    CHECK(am.getChannelVolume(idB) == 0.6f);
    am.stopSound(idB);
    am.unloadSound(idB);
}

// Policy: the gain is multiplied in ONE place. If someone writes
// ch->setVolume(volume) again in a voice path, that path silently ignores the sidecar
// (one-shots cannot be observed from a test, hence the grep).
static void test_policy_gain_is_applied_only_in_voiceVolume()
{
    std::ifstream in("engine/src/Audio/AudioManager.cpp", std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    CHECK(!src.empty());
    auto countOf = [&](const std::string& needle) {
        size_t n = 0;
        for (size_t at = src.find(needle); at != std::string::npos; at = src.find(needle, at + needle.size())) ++n;
        return n;
    };
    CHECK(countOf("setVolume(volume)") == 0);                 // no channel receives the bare volume
    CHECK(countOf("voiceVolume(") >= 4);                      // definition + 3 uses (plus the refresh)
}

int main()
{
    PhysicsManager pm;
    pm.init();
    AudioManager am;
    // init() no longer throws: without an output device it returns false and the tests
    // that need FMOD skip themselves via checkAudioProbe/available(). Before,
    // the exception escaped from here and aborted the binary, so the SKIP
    // those tests promised never got printed.
    if (!am.init())
        std::printf("AVISO: FMOD no disponible; los tests que lo necesitan se saltaran\n");

test_output_warning_only_without_output();
        test_defaults_are_neutral();
    test_volume_clamps_to_range();
    test_pitch_clamps_to_range();
    test_setters_survive_without_manager();
    test_tojson_emits_volume_and_pitch();
    test_volume_pitch_round_trip(pm, am);
    test_scene_without_volume_loads_neutral(pm, am);
    test_setVolume_setPitch_reject_nan();
    test_scene_with_null_volume_loads_with_warning(pm, am);
    test_localTransform_null_element_loads_identity(pm, am);
    test_boxCollider_missing_halfExtents_warns(pm, am);
    test_capsuleCollider_null_center_warns(pm, am);
    test_scene_audioclip_missing_is3D_warns(pm, am);
    test_scene_audioclip_missing_loop_warns(pm, am);
    test_scene_audioclip_missing_path_warns(pm, am);
    test_scene_audioclip_wrong_type_warns(pm, am);
    test_init_is_reentrant(am);
    test_missing_file_reports_load_failure(am);
    test_corrupt_file_reports_load_failure(am);
    test_distances_clamp_to_range();
    test_distances_reject_nan();
    test_min_max_invariant_from_both_setters();
    test_all_audioclip_fields_round_trip(pm, am);
    test_updateSpatial_survives_degenerate_cases(am);
    test_scene_updateAudioSpatial_walks_tree(pm, am);
    test_audioclip_command_restores_full_state(am);
    test_audiolistener_command_preserves_disabled_state();
    test_playOneShot_does_not_register_a_channel(am);
    test_bus_round_trip(pm, am);
    test_scene_without_bus_loads_sfx(pm, am);
    test_scene_with_unknown_bus_warns(pm, am);
    test_bus_volumes_are_independent(am);
    test_same_path_shares_one_sound(am);
    test_sound_slots_are_recycled(am);
    test_load_mode_round_trip_and_cache(pm, am);
    test_load_mode_back_compat_and_unknown(pm, am);
    test_scene_rejects_unsupported_extension(pm, am);
    test_playClipAtPoint_pins_sound_once(am);
    test_p12_props_clamp_and_reject_nan();
    test_p12_round_trip_and_cache(pm, am);
    test_p12_back_compat(pm, am);
    test_bus_effects_are_idempotent_and_released(am);
    test_reverb_zones(pm, am);
    test_mute_time_and_global_pause(pm, am);
    test_import_getters_are_neutral_without_a_sound(am);
    test_import_gain_applies_and_refreshes(am);
    test_import_recycled_slot_does_not_inherit(am);
    test_mono_getter_is_neutral_without_a_voice(am);
    test_import_force_mono_on_a_2d_stereo_voice(am);
    test_import_settings_do_not_survive_shutdown_and_init(am);
    test_policy_gain_is_applied_only_in_voiceVolume();

    am.shutdown();
    pm.shutdown();
    if (g_failures == 0) std::printf("ALL AUDIO TESTS PASSED\n");
    std::fflush(stdout);
    return g_failures == 0 ? 0 : 1;
}
