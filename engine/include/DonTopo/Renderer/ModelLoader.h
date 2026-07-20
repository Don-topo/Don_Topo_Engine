#pragma once
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include <memory>
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
    };
}