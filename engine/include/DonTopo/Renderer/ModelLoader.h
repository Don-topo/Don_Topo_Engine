#pragma once
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include <string>

namespace DonTopo
{
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
    };
}