#pragma once
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Core/FileStamp.h"
#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace DonTopo
{
    inline constexpr int      kPreviewMaxTexture      = 256;
    inline constexpr uint64_t kPreviewMaxSourcePixels = 100'000'000;

    enum class PreviewStatus { Ok, AnimationOnly, Unreadable };

    struct PreviewImage
    {
        int                  w = 0, h = 0;
        std::vector<uint8_t> rgba;          // w*h*4, sRGB as in the file; empty = no texture
    };

    // The minimum to paint a thumbnail: geometry in bind pose and the reduced
    // albedo. Without animations, skeleton, normal map or ORM (see loadPreview).
    struct PreviewPart
    {
        std::vector<glm::vec3> positions, normals, colors;
        std::vector<glm::vec2> uvs;
        std::vector<uint32_t>  indices;
        PreviewImage           albedo;
        float                  metallic  = 0.0f;
        float                  roughness = 0.5f;
    };

    // Box filter down to kPreviewMaxTexture on the larger side, keeping the
    // aspect ratio. What already fits is not touched. rgba: w*h*4. Lives in
    // PreviewImage.cpp alongside the rest of the preview decoding.
    PreviewImage downscalePreviewImage(const uint8_t* rgba, int w, int h);

    struct ModelPreview
    {
        PreviewStatus                      status = PreviewStatus::Unreadable;
        std::vector<PreviewPart>           parts;
        // Sidecar, .mtl of an .obj and external textures, each one stamped BEFORE reading it.
        std::vector<FileStamp>             dependencies;
    };

    // Result of importing ONLY the animations of a file. warnings carries
    // the messages already formatted for the Log Console; mapped/totalChannels
    // let the caller decide whether that is a valid file or a wrong rig.
    struct LoadedClips
    {
        std::vector<AnimationClip> clips;
        std::vector<std::string>   warnings;
        int mappedChannels = 0;
        int totalChannels  = 0;
    };

    struct ModelPiece
    {
        int         piece = 0;        // indice en scene->mMeshes
        std::string name;             // name of the node (or of the mesh if the node has none)
        glm::mat4   transform{1.0f};  // relative to the root; translation x sidecar scale
    };

    struct StaticModel
    {
        std::vector<Mesh>       meshes;   // one per scene->mMeshes; meshes[i].piece == i
        std::vector<ModelPiece> pieces;   // occurrences, depth-first
    };

    class ModelLoader
    {
        public:
            static Mesh load(const std::string& path);
            static SkinnedMesh loadSkinned(const std::string& path);

            // A single ReadFile: all the meshes of the file and where each
            // one appears in the nodes. For models WITHOUT bones (with bones, loadSkinned).
            // The transformation of each piece is relative to the root (that of an FBX
            // carries the unit conversion) and its translation is scaled by the
            // sidecar's scale. Meshes without triangles are not a piece. Throws like load.
            static StaticModel loadStatic(const std::string& path);

            // The mesh `piece` of the file, without transformation. load(path) is
            // load(path, 0). Piece out of range: std::runtime_error.
            static Mesh load(const std::string& path, int piece);

            // Imports the animations of path mapping each channel to the skeleton
            // skel BY bone NAME. It does not build geometry or materials:
            // a Mixamo FBX brings the whole mesh and it is not needed here.
            //
            // Does not throw: an unreadable file returns empty clips and a warning.
            static LoadedClips loadAnimationClips(const std::string& path, const Skeleton& skel);

            // true if any aiMesh of the file declares bones. It is what separates
            // a character from a prop: without bones there are no per-vertex weights, and
            // without weights there is nothing an animation can deform.
            //
            // Does not throw. An unreadable file returns false and lets load()
            // give the real error, with its message.
            static bool hasBones(const std::string& path);

            // Decides static vs skinned by looking at the file, not the caller. An
            // FBX with bones always comes in as SkinnedMesh, even if it brings not
            // a single animation: it is what enables the Animator, and the clips may
            // come later from other files (see addAnimationSource).
            //
            // Propagates the exceptions of load()/loadSkinned(): the callers already
            // have their try/catch and their error message for the user.
            static std::shared_ptr<Mesh> loadAuto(const std::string& path);

            // Lightweight read for the Content Browser thumbnails. It draws the
            // same as the engine (with bones, all the meshes like loadSkinned;
            // without bones, only the first like load) and respects the sidecar in
            // what changes the look. Without a mesh but with clips -> AnimationOnly.
            // Never throws.
            static ModelPreview loadPreview(const std::string& path);

            // Texture reduced to kPreviewMaxTexture on the larger side, keeping
            // the aspect ratio. Empty if it cannot be read or if it exceeds
            // kPreviewMaxSourcePixels (the header is looked at before decoding).
            static PreviewImage loadPreviewImage(const std::filesystem::path& path);
            static PreviewImage decodePreviewImage(const uint8_t* bytes, size_t size);

            // Model formats that Assimp has compiled in. The ONLY list: the
            // whole editor asks here (classify, Add Mesh, import,
            // thumbnails, Animator). Case-insensitive; ext with the dot.
            static bool isSupportedModelExtension(const std::string& ext);
            // Filter for ImGuiFileDialog with the same formats.
            static const char* supportedModelFilter();

            // Path of an external texture referenced by the model: first the
            // relative path AS IS with respect to modelDir (textures/x.png); if it does not
            // exist, the bare name next to the model, which is the usual behavior. An
            // absolute path or one that leaves the folder (..) only tries the name.
            static std::filesystem::path resolveModelTexture(const std::filesystem::path& modelDir,
                                                             const std::string& raw);

            // Files the model reads besides itself, in paths RELATIVE to
            // its folder (separator /), without duplicates: the mtllib of an .obj and
            // the external buffers[].uri and images[].uri of a .gltf (decoded
            // from %XX; data:, absolute and with .. out). .glb and .fbx: none.
            // Never throws: an unreadable file returns empty.
            static std::vector<std::string> modelCompanionFiles(const std::string& path);
    };
}