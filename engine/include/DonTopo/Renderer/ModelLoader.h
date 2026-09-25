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
        std::vector<uint8_t> rgba;          // w*h*4, sRGB tal cual el fichero; vacía = sin textura
    };

    // Lo minimo para pintar una miniatura: geometria en bind pose y el albedo
    // reducido. Sin animaciones, esqueleto, normal map ni ORM (ver loadPreview).
    struct PreviewPart
    {
        std::vector<glm::vec3> positions, normals, colors;
        std::vector<glm::vec2> uvs;
        std::vector<uint32_t>  indices;
        PreviewImage           albedo;
        float                  metallic  = 0.0f;
        float                  roughness = 0.5f;
    };

    // Filtro de caja a kPreviewMaxTexture en el lado mayor, conservando la
    // proporcion. Lo que ya cabe no se toca. rgba: w*h*4. Vive en
    // PreviewImage.cpp junto al resto de la decodificacion del preview.
    PreviewImage downscalePreviewImage(const uint8_t* rgba, int w, int h);

    struct ModelPreview
    {
        PreviewStatus                      status = PreviewStatus::Unreadable;
        std::vector<PreviewPart>           parts;
        // Sidecar, .mtl de un .obj y texturas externas, cada uno sellado ANTES de leerlo.
        std::vector<FileStamp>             dependencies;
    };

    // Resultado de importar SOLO las animaciones de un fichero. warnings lleva
    // los mensajes ya formateados para el Log Console; mapped/totalChannels
    // dejan al caller decidir si eso es un fichero válido o un rig equivocado.
    struct LoadedClips
    {
        std::vector<AnimationClip> clips;
        std::vector<std::string>   warnings;
        int mappedChannels = 0;
        int totalChannels  = 0;
    };

    class ModelLoader
    {
        public:
            static Mesh load(const std::string& path);
            static SkinnedMesh loadSkinned(const std::string& path);

            // Importa las animaciones de path mapeando cada canal al esqueleto
            // skel POR NOMBRE de hueso. No construye geometría ni materiales:
            // un FBX de Mixamo trae la malla entera y aquí sobra.
            //
            // No lanza: un fichero ilegible devuelve clips vacío y un warning.
            static LoadedClips loadAnimationClips(const std::string& path, const Skeleton& skel);

            // true si algún aiMesh del fichero declara huesos. Es lo que separa
            // un personaje de un prop: sin huesos no hay pesos por vértice, y
            // sin pesos no hay nada que una animación pueda deformar.
            //
            // No lanza. Un fichero ilegible devuelve false y deja que load()
            // dé el error de verdad, con su mensaje.
            static bool hasBones(const std::string& path);

            // Decide estático vs skinned mirando el fichero, no al llamante. Un
            // FBX con huesos entra siempre como SkinnedMesh, aunque no traiga ni
            // una animación: es lo que habilita el Animator, y los clips pueden
            // venir después de otros ficheros (ver addAnimationSource).
            //
            // Propaga las excepciones de load()/loadSkinned(): los llamantes ya
            // tienen su try/catch y su mensaje de error para el usuario.
            static std::shared_ptr<Mesh> loadAuto(const std::string& path);

            // Lectura ligera para las miniaturas del Content Browser. Pinta lo
            // mismo que el motor (con huesos, todas las mallas como loadSkinned;
            // sin huesos, solo la primera como load) y respeta el sidecar en lo
            // que cambia el aspecto. Sin malla pero con clips -> AnimationOnly.
            // Nunca lanza.
            static ModelPreview loadPreview(const std::string& path);

            // Textura reducida a kPreviewMaxTexture en el lado mayor, conservando
            // la proporcion. Vacia si no se puede leer o si pasa de
            // kPreviewMaxSourcePixels (se mira la cabecera antes de decodificar).
            static PreviewImage loadPreviewImage(const std::filesystem::path& path);
            static PreviewImage decodePreviewImage(const uint8_t* bytes, size_t size);
    };
}