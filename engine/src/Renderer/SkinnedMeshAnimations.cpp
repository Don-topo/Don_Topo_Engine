#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include <cmath>
#include <filesystem>

namespace DonTopo
{
    bool clipHasMotion(const AnimationClip& clip)
    {
        constexpr float kEps = 1e-4f;

        // Basta con comparar contra la PRIMERA key de la pista: si todas son
        // iguales a la primera, todas son iguales entre sí, y si alguna difiere
        // ya hay movimiento. No hace falta comparar cada par.
        auto varia = [](const auto& keys, auto component) {
            for (size_t k = 1; k < keys.size(); k++)
                for (int c = 0; c < component(keys[0]).length(); c++)
                    if (std::fabs(component(keys[k])[c] - component(keys[0])[c]) > kEps)
                        return true;
            return false;
        };

        for (const auto& ch : clip.channels)
        {
            if (varia(ch.posKeys,   [](const BoneKeyframe& k)  { return k.value; })) return true;
            if (varia(ch.scaleKeys, [](const BoneKeyframe& k)  { return k.value; })) return true;
            if (varia(ch.rotKeys,   [](const BoneKeyframeQ& k) { return k.value; })) return true;
        }
        return false;
    }

    std::string uniqueClipName(const std::vector<AnimationClip>& existing,
                               const std::string& base)
    {
        const std::string root = base.empty() ? std::string("Animation") : base;

        auto taken = [&](const std::string& n) {
            for (const auto& c : existing)
                if (c.name == n) return true;
            return false;
        };

        if (!taken(root)) return root;

        int suffix = 1;
        std::string candidate = root + " (" + std::to_string(suffix) + ")";
        while (taken(candidate))
            candidate = root + " (" + std::to_string(++suffix) + ")";
        return candidate;
    }

    bool addAnimationSource(SkinnedMesh& mesh, const std::string& path,
                            std::vector<std::string>& warnings,
                            const std::vector<std::string>* forcedNames)
    {
        LoadedClips loaded = ModelLoader::loadAnimationClips(path, mesh.skeleton);
        for (auto& w : loaded.warnings) warnings.push_back(w);

        if (loaded.clips.empty()) return false;   // el warning ya lo puso el loader

        const std::string base = std::filesystem::path(path).stem().string();
        const std::string file = std::filesystem::path(path).filename().string();

        AnimationSource src;
        src.path    = path;
        src.builtin = false;

        for (size_t i = 0; i < loaded.clips.size(); i++)
        {
            AnimationClip clip = std::move(loaded.clips[i]);
            // forcedNames manda; si se agota (el FBX trae más clips que la
            // última vez), el resto cae en la regla normal de basename.
            const bool forced = forcedNames && i < forcedNames->size();
            if (forced)
            {
                // El nombre forzado manda SI está libre: uniqueClipName lo
                // devuelve tal cual y un rename sobrevive a un save/load
                // intacto. Si ya está en uso —por un clip previo, o por un
                // forcedName anterior de esta misma importación— devuelve una
                // variante con sufijo, así que comparar el resultado con lo
                // pedido detecta la colisión sin duplicar la lógica de "taken".
                const std::string& wanted = (*forcedNames)[i];
                clip.name = uniqueClipName(mesh.animationClips, wanted);
                if (clip.name != wanted)
                    warnings.push_back(file + ": el nombre de clip guardado '" + wanted
                                        + "' ya estaba en uso, renombrado a '" + clip.name + "'");
            }
            else
            {
                clip.name = uniqueClipName(mesh.animationClips, base);
            }
            src.clipNames.push_back(clip.name);
            mesh.animationClips.push_back(std::move(clip));
        }

        mesh.animationSources.push_back(std::move(src));
        return true;
    }

    bool removeAnimationSource(SkinnedMesh& mesh, size_t sourceIndex)
    {
        if (sourceIndex >= mesh.animationSources.size())  return false;
        if (mesh.animationSources[sourceIndex].builtin)   return false;

        const std::vector<std::string>& names = mesh.animationSources[sourceIndex].clipNames;

        for (const std::string& n : names)
        {
            for (size_t i = 0; i < mesh.animationClips.size(); i++)
            {
                if (mesh.animationClips[i].name != n) continue;
                mesh.animationClips.erase(mesh.animationClips.begin() + (long)i);
                break;   // los nombres son únicos: uno y solo uno por nombre
            }
        }

        mesh.animationSources.erase(mesh.animationSources.begin() + (long)sourceIndex);
        return true;
    }

    bool renameClip(SkinnedMesh& mesh, const std::string& oldName,
                    const std::string& newName)
    {
        if (newName.empty() || oldName == newName) return false;

        for (const auto& c : mesh.animationClips)
            if (c.name == newName) return false;      // duplicado

        AnimationClip* target = nullptr;
        for (auto& c : mesh.animationClips)
            if (c.name == oldName) { target = &c; break; }
        if (!target) return false;

        target->name = newName;

        for (auto& src : mesh.animationSources)
            for (auto& n : src.clipNames)
                if (n == oldName) n = newName;

        return true;
    }

    void applyClipNamesPositionally(SkinnedMesh& mesh, AnimationSource& source,
                                    const std::vector<std::string>& savedNames,
                                    std::vector<std::string>& warnings)
    {
        const size_t n = savedNames.size() < source.clipNames.size()
                        ? savedNames.size() : source.clipNames.size();
        if (n == 0) return;

        // Snapshot ANTES de tocar nada: source.clipNames[i] se sobreescribe
        // más abajo, y localizar cada clip en mesh.animationClips por su
        // nombre ANTIGUO tiene que hacerse contra el estado previo a
        // cualquier mutación — si no, un rename ya aplicado podría dejar dos
        // clips con el mismo nombre temporalmente y una búsqueda por nombre
        // más adelante en el bucle encontraría el equivocado.
        std::vector<std::string> oldNames(source.clipNames.begin(), source.clipNames.begin() + (long)n);

        // Índice en animationClips de cada clip del lote, resuelto una sola
        // vez contra oldNames (todavía intactos en este punto).
        std::vector<long> clipIdx(n, -1);
        for (size_t i = 0; i < n; i++)
            for (size_t k = 0; k < mesh.animationClips.size(); k++)
                if (mesh.animationClips[k].name == oldNames[i]) { clipIdx[i] = (long)k; break; }

        std::vector<bool> skip(n, false);

        // 1) Duplicados entre los propios savedNames: dos índices apuntando
        //    al mismo nombre destino dejarían un clip inalcanzable. Se
        //    descartan ambos índices, no se aplica ninguno de los dos.
        for (size_t i = 0; i < n; i++)
        {
            for (size_t j = i + 1; j < n; j++)
            {
                if (savedNames[i] != savedNames[j]) continue;
                if (!skip[i] && !skip[j])
                    warnings.push_back("Nombre de clip duplicado al restaurar la fuente builtin: '"
                                        + savedNames[i] + "' — se conservan los nombres originales");
                skip[i] = true;
                skip[j] = true;
            }
        }

        // batchSet: nombres ORIGINALES de los clips que forman este lote —
        // "los clips que se están renombrando en esta misma operación", tal
        // cual pide el finding. Un nombre destino que coincide con uno de
        // estos no es una colisión externa (es, por ejemplo, el otro lado de
        // un swap).
        auto inBatch = [&](const std::string& name) {
            for (size_t k = 0; k < n; k++)
                if (oldNames[k] == name) return true;
            return false;
        };

        // 2) Colisión con un clip AJENO al lote (otra fuente, u otro clip del
        //    propio mesh fuera de estos n). Un nombre igual al que el clip ya
        //    tiene es un no-op válido, nunca una colisión.
        for (size_t i = 0; i < n; i++)
        {
            if (skip[i] || clipIdx[i] < 0) continue;
            if (savedNames[i] == oldNames[i]) continue;   // no-op

            for (const auto& c : mesh.animationClips)
            {
                if (c.name != savedNames[i] || inBatch(c.name)) continue;
                warnings.push_back("Nombre de clip guardado '" + savedNames[i]
                                    + "' ya en uso por otro clip — no se aplica al restaurar la fuente builtin");
                skip[i] = true;
                break;
            }
        }

        // 3) Aplicar lo que sobrevivió: todo de una vez, así que un swap
        //    (A->B y B->A a la vez) converge en vez de colisionar consigo
        //    mismo como pasaría encadenando renameClip.
        for (size_t i = 0; i < n; i++)
        {
            if (skip[i] || clipIdx[i] < 0) continue;
            if (savedNames[i] == oldNames[i]) continue;   // no-op

            mesh.animationClips[(size_t)clipIdx[i]].name = savedNames[i];
            source.clipNames[i] = savedNames[i];
        }
    }
}
