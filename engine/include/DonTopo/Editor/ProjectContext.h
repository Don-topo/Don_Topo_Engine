#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace DonTopo {

// Path sandbox of an editor project.
//
// The workspace is the `projects/` folder next to the executable; each project is
// a subfolder with a `project.json` and the subfolders `assets/`, `scenes/`
// and `scripts/`. Once the project is chosen, everything the editor reads or
// writes from the user (scenes, scripts, assets, export destination) goes through
// resolve()/contains(): a path from another project is rejected without touching disk.
//
// The ENGINE's assets (window logo, shaders, splash, demo
// skybox) are NOT the project's and are still resolved as always from the root
// of the executable/repo: they do not go through here.
class ProjectContext {
public:
    // View menu settings that are saved PER PROJECT in the "settings"
    // section of project.json.
    //
    // The ENABLES start as false on purpose: a new project, or one without a
    // "settings" section, opens with all effects off even if the
    // Renderer has other defaults.
    //
    // The PARAMETERS (intensities, radii, steps) do NOT carry their real default value
    // here: readSettings() receives a `base` that the editor fills in by
    // reading the Renderer itself, and each field missing from the JSON keeps
    // the value of that base. This way a parameter's default is still the Renderer's
    // without duplicating its numbers here (and without ProjectContext depending
    // on the Renderer).
    struct ViewSettings {
        // Panel visibility indices, in the same order as the View menu.
        //
        // The ORDER of this enum is not part of the on-disk format: each panel is
        // saved by its name (kPanelKeys, in ProjectContext.cpp), so
        // reordering or inserting in the middle does not invalidate any project.json. What
        // is mandatory is that kPanelKeys carries the same order, and a
        // static_assert watches that.
        //
        // Sprite Editor and Collision Layers are NOT here on purpose: they are opened
        // for a specific task and closed, they are not panels one wants to
        // find open at startup. It is the only deliberate absence.
        enum Panel {
            PanelScene = 0,
            PanelViewport,
            PanelProperties,
            PanelLog,
            PanelContentBrowser,
            PanelScriptEditor,
            PanelAnimator,
            PanelPerformance,
            PanelRendering,
            PanelInputActions,
            PanelCount
        };

        bool        ambient = false;
        // Wireframe mode. It was the ONLY RendererState field that was not
        // persisted: it was lost on reopening and the exported game never saw it.
        bool        wireframe = false;
        bool        bloom   = false;
        bool        ssao    = false;
        bool        ssr     = false;
        bool        fog     = false;
        bool        motionBlur = false;
        // Combos by NAME, never by index: reordering the options array
        // must not change anyone's saved setting.
        std::string aaMode = "None";
        std::string fpMode = "Off";
        // Render backend. It is not applied when read: the device is already created
        // when the project is opened, so it only takes effect on the next
        // startup (see readLastProject). Names in RenderBackend.h.
        std::string renderBackend = "Vulkan";
        // Sky folder. Inside it px/nx/py/ny/pz/nz.png are expected, which is
        // the convention the sandbox, the runtime, the DirectX 12 backend and the
        // exporter already assumed. By default the usual one, so a
        // project without the key looks the same.
        std::string skyboxFolder  = "assets/skybox";

        // The six paths that EditorRenderer::initSkybox expects, in its order:
        // +X, -X, +Y, -Y, +Z, -Z. Here and not in each caller so that the
        // naming convention lives in one place only.
        std::array<std::string, 6> skyboxFaces() const
        {
            const std::string base =
                skyboxFolder.empty() ? std::string("assets/skybox") : skyboxFolder;
            return {base + "/px.png", base + "/nx.png", base + "/py.png",
                    base + "/ny.png", base + "/pz.png", base + "/nz.png"};
        }

        float ambientIntensity = 0.0f;
        float bloomThreshold   = 0.0f;
        float bloomKnee        = 0.0f;
        float bloomIntensity   = 0.0f;
        float ssaoRadius       = 0.0f;
        float ssaoBias         = 0.0f;
        float ssaoIntensity    = 0.0f;
        float ssaoPower        = 0.0f;
        float ssrMaxDistance   = 0.0f;
        float ssrThickness     = 0.0f;
        int   ssrMaxSteps      = 0;
        float ssrEdgeFade      = 0.0f;
        float ssrIntensity     = 0.0f;
        float fogDensity       = 0.0f;
        float fogHeightFalloff = 0.0f;
        float fogBaseHeight    = 0.0f;
        float fogAnisotropy    = 0.0f;
        int   fogSteps         = 0;
        float fogScatter[3]    = {0.0f, 0.0f, 0.0f};
        float motionBlurIntensity = 0.0f;
        float motionBlurMaxRadius = 0.0f;
        int   motionBlurSamples   = 0;
        float fxaaSubpix       = 0.0f;
        float fxaaEdgeThreshold    = 0.0f;
        float fxaaEdgeThresholdMin = 0.0f;
        float ssaaFactor       = 0.0f;
        int   msaaSamples      = 0;
        float taaFeedback      = 0.0f;
        float taaJitterScale   = 0.0f;
        float fpLightRadius    = 0.0f;
        // Cascaded shadows. Defaults = the values it was drawn with
        // before they were adjustable, so a project.json without these
        // keys looks exactly the same as before.
        float shadowDistance   = 500.0f;
        float cascadeLambda    = 0.75f;
        int   shadowResolution = 2048;
        // Present mode: 0 vsync, 1 mailbox, 2 immediate. What was REQUESTED is
        // saved, not what the device could give: if the project is opened on another
        // machine that does support it, it takes effect again.
        int   presentMode      = 0;

        // --- Per-bus audio volumes --------------------------------------------
        //
        // Neutral (1.0) on purpose, and they are NOT covered by the "everything off" rule
        // that applies to the effects: a project without these fields has to
        // open sounding the same as before the feature, not silent.
        float masterVolume = 1.0f;
        float musicVolume  = 1.0f;
        float sfxVolume    = 1.0f;

        // Empty = no saved data: the panel stays as it is, which is NOT the
        // same as closed. Panels are not covered by the "everything
        // off" rule.
        //
        // `optional` and not an int with -1 as sentinel: this used to be a list of
        // nine hand-written -1s, and adding the tenth panel to the enum would have
        // given it a 0 ("closed") instead of "no data", closing the new panel
        // in all the projects that already exist. A new panel's slot
        // has to be born empty BY ITSELF.
        std::optional<bool> panelOpen[PanelCount] = {};

        // --- Physics collision layers -----------------------------------------
        //
        // Same index as PhysicsManager (0-31), but without including it: the
        // dependency goes Editor -> Physics in the editor's .cpp, not in this
        // header. The matrix travels compressed to one mask per layer (bit b of
        // layerMasks[a] = "a collides with b") and starts ALL ones, which is
        // the matrix that filters nothing: a project without these fields opens
        // exactly as before the feature.
        static constexpr int LayerCount = 32;

        // Layers actually created (the prefix [0, layerActive) of the arrays
        // below). Always >= 1: layer 0 ("Default") cannot be deleted.
        int layerActive = 1;

        std::array<std::string, LayerCount> layerNames = [] {
            std::array<std::string, LayerCount> n;
            n[0] = "Default";
            return n;
        }();

        std::array<uint32_t, LayerCount> layerMasks = [] {
            std::array<uint32_t, LayerCount> m{};
            m.fill(0xFFFFFFFFu);
            return m;
        }();

        // Diagnostic of the last read, for the editor Log. It is not
        // serialized.
        bool        loadFailed = false; // unreadable JSON or corrupt "settings"
        std::string unknownEnum;        // combo name that does not exist today
    };

    // Reads the "settings" section of project.json. NEVER throws: a missing file,
    // broken JSON, a "settings" that is not an object or fields with a changed type
    // fall back to the value of `base` (enables and combos, to the ViewSettings default).
    // It writes nothing: a corrupt file is left as it is.
    static ViewSettings readSettings(const std::filesystem::path& projectDir, const ViewSettings& base);

    // Replaces the "settings" section of project.json keeping the rest of the
    // file (name, version, whatever is there). It writes to a temporary in the same
    // folder and renames over it: a failure halfway does not leave project.json
    // truncated. Returns false without touching the original if something fails.
    static bool writeSettings(const std::filesystem::path& projectDir, const ViewSettings& settings);

    // Version of the "settings" section itself, independent of kProjectVersion.
    static constexpr const char* kSettingsVersion = "1.0";

    // --- Editor state (editor.json, next to the executable) ---------------
    //
    // The render backend is saved PER PROJECT, but the Renderer is created
    // before the user chooses a project: at startup, the editor does not yet
    // know which project.json to read it from. That is why it remembers here which was the last
    // project opened, and from that one it takes the backend to start with.
    //
    // It is editor state, not project state: it does not travel with it, it is not exported and
    // losing it only means starting on Vulkan again.

    // Path of the last project opened. Empty if there is no editor.json, if it is
    // corrupt, or if the folder it points to no longer exists (project deleted or
    // moved): in all those cases startup falls back to Vulkan without complaining.
    static std::filesystem::path readLastProject();

    // Remembers `projectDir` as the last project opened. Same atomic write
    // as writeSettings (temporary + rename). Returns false without touching
    // the previous file if something fails; nobody should abort because of that.
    static bool writeLastProject(const std::filesystem::path& projectDir);

    ProjectContext() = default;
    explicit ProjectContext(const std::filesystem::path& root);

    // Project root, already canonicalized if possible. Empty if the context has not
    // been initialized (headless tests, startup before the selector).
    const std::filesystem::path& root() const { return m_root; }
    bool valid() const { return !m_root.empty(); }

    // root() / relative, not normalized beyond what operator/ does. If
    // `relative` is already absolute it is returned as is (operator/ replaces it):
    // the filter is still contains(), not this function.
    std::filesystem::path resolve(const std::filesystem::path& relative) const;

    // Does `absolute` fall inside the project (or is it the root itself)?
    //
    // FAILS CLOSED: if the context is not valid, or if the root or the destination's
    // existing prefix cannot be canonicalized (permissions, path
    // deleted mid-operation), it returns false. When in doubt, outside.
    bool contains(const std::filesystem::path& absolute) const;

    // --- Workspace -------------------------------------------------------

    // `projects/` next to the executable. It does not create it.
    static std::filesystem::path workspaceDir();

    // Workspace subfolders that have a `project.json`, sorted by
    // folder name. Creates the workspace if it does not exist. Never throws.
    static std::vector<std::filesystem::path> discover();

    // Name declared in the project's `project.json`; if the file is missing or
    // cannot be parsed, the folder name.
    static std::string readProjectName(const std::filesystem::path& projectDir);

    // Validates the name to use it as a workspace folder: not empty or only
    // spaces, not `.`/`..`, no path separators or characters invalid on
    // Windows, no reserved device names, and unique against the
    // already existing folders comparing WITHOUT distinguishing case. Fills
    // `error` with the reason when it returns false.
    static bool validateName(const std::string& name, std::string& error);

    // Validates and, if it passes, creates `projects/<name>/` with its `project.json`, the
    // subfolders `assets/`, `scenes/`, `scripts/` and the startup scene
    // kStartupScene (empty: when the project opens only the skybox is seen). If
    // validation fails it creates nothing and returns false with the reason in `error`.
    static bool create(const std::string& name, std::filesystem::path& outDir, std::string& error);

    // Version written in new `project.json` files.
    static constexpr const char* kProjectVersion = "1.0";
    // Scene the editor opens when choosing a project, relative to root().
    static constexpr const char* kStartupScene = "scenes/main.json";

private:
    std::filesystem::path m_root;
};

} // namespace DonTopo
