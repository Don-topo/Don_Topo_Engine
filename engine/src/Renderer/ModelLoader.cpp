#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include "DonTopo/Core/ImportSettings.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <stdexcept>
#include <filesystem>
#include <string>
#include <functional>
#include <array>
#include <memory>

namespace DonTopo 
{
    static glm::mat4 aiToGlm(const aiMatrix4x4& m)
    {
        // aiMatrix4x4 es row-major; glm::mat4 es column-major
        return glm::mat4(
            m.a1, m.b1, m.c1, m.d1,
            m.a2, m.b2, m.c2, m.d2,
            m.a3, m.b3, m.c3, m.d3,
            m.a4, m.b4, m.c4, m.d4
        );
    }

    // Ajustes de importacion del fichero (<fbx>.import.json). Nunca lanza: un
    // sidecar roto da el defecto y un aviso por stderr (el Log Console no llega a
    // este nivel; mismo canal que [AudioImport]).
    static ModelImportSettings readModelSettings(const std::string& path)
    {
        std::string warning;
        const ModelImportSettings s = loadModelImportSettings(path, &warning);
        if (!warning.empty())
            std::fprintf(stderr, "[ModelImport] %s: %s\n",
                         std::filesystem::path(path).filename().string().c_str(), warning.c_str());
        return s;
    }

    // Sin sidecar: exactamente Triangulate | FlipUVs | GenNormals | CalcTangentSpace,
    // que era lo fijo antes de que existieran los ajustes.
    static unsigned int assimpFlags(const ModelImportSettings& s)
    {
        unsigned int f = aiProcess_Triangulate;
        if (s.flipUVs)      f |= aiProcess_FlipUVs;
        if (s.calcTangents) f |= aiProcess_CalcTangentSpace;
        switch (s.normals)
        {
            case NormalsMode::File:   f |= aiProcess_GenNormals; break;          // solo si faltan
            case NormalsMode::Smooth: f |= aiProcess_RemoveComponent | aiProcess_GenSmoothNormals; break;
            case NormalsMode::Flat:   f |= aiProcess_RemoveComponent | aiProcess_GenNormals; break;
        }
        // La escala NO va por aiProcess_GlobalScale: el importador FBX suma su
        // propio factor de unidades (UnitScaleFactor * 0.01, cm -> m) y "escala 2"
        // daba x0.02 en un FBX. Se aplica a mano tras la carga, en las unidades
        // del propio fichero (ver scaleClipTranslations y los tres sitios de abajo).
        return f;
    }

    static void configureImporter(Assimp::Importer& importer, const ModelImportSettings& s)
    {
        // Smooth y Flat descartan las normales del fichero ANTES de generarlas.
        if (s.normals != NormalsMode::File)
            importer.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS, aiComponent_NORMALS);
    }

    // Claves de traslacion de un clip x scale. Solo las de posicion: rotaciones y
    // escalas de hueso no dependen de las unidades. x1.0f es exacto, asi que sin
    // ajuste el clip queda bit a bit igual.
    static void scaleClipTranslations(AnimationClip& clip, float scale)
    {
        if (scale == 1.0f) return;
        for (BoneChannel& ch : clip.channels)
            for (BoneKeyframe& k : ch.posKeys)
                k.value *= scale;
    }

    // Convierte una aiAnimation a AnimationClip resolviendo cada canal contra
    // skel POR NOMBRE. Compartido por loadSkinned y loadAnimationClips: son el
    // mismo trabajo, y duplicarlo garantizaba que las dos rutas divergieran.
    //
    // clip.name queda con el nombre CRUDO del FBX: la unicidad la aplica quien
    // llama, que es quien sabe qué clips hay ya en el mesh destino.
    static AnimationClip clipFromAssimp(const aiAnimation* anim, const Skeleton& skel,
                                        int& mappedChannels, int& totalChannels,
                                        std::vector<std::string>* unknownBones)
    {
        AnimationClip clip;
        clip.name           = anim->mName.C_Str();
        clip.duration       = (float)anim->mDuration;
        clip.ticksPerSecond = (anim->mTicksPerSecond > 0.0) ? (float)anim->mTicksPerSecond : 24.0f;

        for (uint32_t c = 0; c < anim->mNumChannels; c++)
        {
            aiNodeAnim* ch = anim->mChannels[c];
            std::string boneName = ch->mNodeName.C_Str();
            totalChannels++;

            auto it = skel.boneMap.find(boneName);
            if (it == skel.boneMap.end())
            {
                if (unknownBones) unknownBones->push_back(boneName);
                continue;
            }
            mappedChannels++;

            BoneChannel bc;
            bc.boneIndex = it->second;

            for (uint32_t k = 0; k < ch->mNumPositionKeys; k++)
            {
                auto& key = ch->mPositionKeys[k];
                bc.posKeys.push_back({ (float)key.mTime,
                    { key.mValue.x, key.mValue.y, key.mValue.z } });
            }
            for (uint32_t k = 0; k < ch->mNumRotationKeys; k++)
            {
                auto& key = ch->mRotationKeys[k];
                // glm::quat constructor: (w, x, y, z)
                bc.rotKeys.push_back({ (float)key.mTime,
                    glm::quat(key.mValue.w, key.mValue.x, key.mValue.y, key.mValue.z) });
            }
            for (uint32_t k = 0; k < ch->mNumScalingKeys; k++)
            {
                auto& key = ch->mScalingKeys[k];
                bc.scaleKeys.push_back({ (float)key.mTime,
                    { key.mValue.x, key.mValue.y, key.mValue.z } });
            }
            clip.channels.push_back(std::move(bc));
        }
        return clip;
    }

    // Una aiMesh a Mesh con su material. Es el cuerpo que tenia load(path),
    // movido tal cual; `ai` sustituye a scene->mMeshes[0].
    static Mesh meshFromAssimp(const aiScene* scene, const aiMesh* ai,
                               const ModelImportSettings& settings, const std::string& path)
    {
        Mesh mesh;
        mesh.name = std::filesystem::path(path).stem().string();
        mesh.sourcePath = path;

        mesh.vertices.reserve(ai->mNumVertices);
        for(uint32_t i = 0; i < ai->mNumVertices; i++)
        {
            Vertex v{};
            v.pos   = { ai->mVertices[i].x * settings.scale,
                        ai->mVertices[i].y * settings.scale,
                        ai->mVertices[i].z * settings.scale };
            v.color = { 1.0f, 1.0f, 1.0f };
            if(ai->mTextureCoords[0])
            {
                v.uv = { ai->mTextureCoords[0][i].x, ai->mTextureCoords[0][i].y };
            }
            v.normal  = { ai->mNormals[i].x,  ai->mNormals[i].y,  ai->mNormals[i].z };
            v.tangent = ai->mTangents
                ? glm::vec3{ ai->mTangents[i].x, ai->mTangents[i].y, ai->mTangents[i].z }
                : glm::vec3{ 1.0f, 0.0f, 0.0f };
            mesh.vertices.push_back(v);
        }

        mesh.indices.reserve(ai->mNumFaces * 3);
        for(uint32_t i = 0; i < ai->mNumFaces; i++)
        {
            for(uint32_t j = 0; j < ai->mFaces[i].mNumIndices; j++)
            {
                mesh.indices.push_back(ai->mFaces[i].mIndices[j]);
            }
        }

        if(scene->mNumMaterials > 0)
        {
            aiMaterial* mat = scene->mMaterials[ai->mMaterialIndex];
            namespace fs = std::filesystem;
            fs::path modelDir = fs::path(path).parent_path();

            auto loadTex = [&](aiTextureType type, std::vector<uint8_t>& outEmbedded, std::string& outPath)
            {
                aiString texPath;
                if(mat->GetTexture(type, 0, &texPath) != AI_SUCCESS) return;
                const char* raw = texPath.C_Str();
                const aiTexture* emb = scene->GetEmbeddedTexture(raw);
                if(emb)
                {
                    if(emb->mHeight == 0)
                    {
                        const uint8_t* begin = reinterpret_cast<const uint8_t*>(emb->pcData);
                        outEmbedded.assign(begin, begin + emb->mWidth);
                    }
                    else
                    {
                        outEmbedded.resize(emb->mWidth * emb->mHeight * 4);
                        for(uint32_t k = 0; k < emb->mWidth * emb->mHeight; k++)
                        {
                            outEmbedded[k*4+0] = emb->pcData[k].r;
                            outEmbedded[k*4+1] = emb->pcData[k].g;
                            outEmbedded[k*4+2] = emb->pcData[k].b;
                            outEmbedded[k*4+3] = emb->pcData[k].a;
                        }
                    }
                }
                else
                {
                    outPath = ModelLoader::resolveModelTexture(modelDir, raw).string();
                }
            };

            loadTex(aiTextureType_DIFFUSE, mesh.material.embeddedTexture, mesh.material.texturePath);
            loadTex(aiTextureType_NORMALS, mesh.material.embeddedNormalMap, mesh.material.normalMapPath);
            // Assimp suele guardar el normal map como HEIGHT en FBX
            if(mesh.material.embeddedNormalMap.empty() && mesh.material.normalMapPath.empty())
                loadTex(aiTextureType_HEIGHT, mesh.material.embeddedNormalMap, mesh.material.normalMapPath);
            // ORM (glTF metallic-roughness packed: R=AO, G=roughness, B=metallic)
            loadTex(aiTextureType_UNKNOWN, mesh.material.embeddedMetallicRoughness, mesh.material.metallicRoughnessPath);
        }

        return mesh;
    }

    static bool hasTriangles(const aiMesh* m)
    {
        for (uint32_t f = 0; f < m->mNumFaces; ++f)
            if (m->mFaces[f].mNumIndices == 3) return true;
        return false;
    }

    // glTF/GLB por extension (sin distinguir mayusculas).
    static bool isGltfPath(const std::string& path)
    {
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext == ".gltf" || ext == ".glb";
    }

    // Apariciones de cada malla en los nodos, en profundidad. La raiz aporta su
    // transformacion SOLO en glTF/GLB (`applyRoot`): con un unico nodo de primer
    // nivel Assimp no crea raiz sintetica y ese nodo ES mRootNode, con la
    // correccion de ejes/unidades del autor; con varios, la raiz es sintetica e
    // identidad. En FBX se descarta (lleva la conversion de unidades del
    // importador) y sus mallas, si tiene, van con identidad. En OBJ es identidad.
    static std::vector<ModelPiece> collectPieces(const aiScene* scene, float scale, bool applyRoot)
    {
        std::vector<ModelPiece> out;
        std::function<void(const aiNode*, const glm::mat4&)> walk = [&](const aiNode* node, const glm::mat4& m)
        {
            for (uint32_t k = 0; k < node->mNumMeshes; ++k)
            {
                const uint32_t idx = node->mMeshes[k];
                if (idx >= scene->mNumMeshes || !hasTriangles(scene->mMeshes[idx])) continue;
                ModelPiece p;
                p.piece = static_cast<int>(idx);
                p.name  = node->mName.length > 0 ? node->mName.C_Str() : scene->mMeshes[idx]->mName.C_Str();
                p.transform = m;
                p.transform[3].x *= scale;
                p.transform[3].y *= scale;
                p.transform[3].z *= scale;
                out.push_back(std::move(p));
            }
            for (uint32_t c = 0; c < node->mNumChildren; ++c)
                walk(node->mChildren[c], m * aiToGlm(node->mChildren[c]->mTransformation));
        };
        if (!scene->mRootNode) return out;
        // Cada hijo entra con SU transformacion; la raiz, solo si applyRoot.
        walk(scene->mRootNode, applyRoot ? aiToGlm(scene->mRootNode->mTransformation) : glm::mat4(1.0f));
        return out;
    }

    Mesh ModelLoader::load(const std::string &path)
    {
        return load(path, 0);
    }

    Mesh ModelLoader::load(const std::string& path, int piece)
    {
        const ModelImportSettings settings = readModelSettings(path);
        Assimp::Importer importer;
        configureImporter(importer, settings);
        const aiScene* scene = importer.ReadFile(path, assimpFlags(settings));
        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
            throw std::runtime_error("Assimp: " + std::string(importer.GetErrorString()));
        if (piece < 0 || static_cast<uint32_t>(piece) >= scene->mNumMeshes)
            throw std::runtime_error("'" + path + "' no tiene la pieza " + std::to_string(piece) +
                                     " (tiene " + std::to_string(scene->mNumMeshes) + ")");
        Mesh mesh = meshFromAssimp(scene, scene->mMeshes[piece], settings, path);
        mesh.piece = piece;
        return mesh;
    }

    StaticModel ModelLoader::loadStatic(const std::string& path)
    {
        const ModelImportSettings settings = readModelSettings(path);
        Assimp::Importer importer;
        configureImporter(importer, settings);
        const aiScene* scene = importer.ReadFile(path, assimpFlags(settings));
        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
            throw std::runtime_error("Assimp: " + std::string(importer.GetErrorString()));
        StaticModel out;
        out.meshes.reserve(scene->mNumMeshes);
        for (uint32_t i = 0; i < scene->mNumMeshes; ++i)
        {
            out.meshes.push_back(meshFromAssimp(scene, scene->mMeshes[i], settings, path));
            out.meshes.back().piece = static_cast<int>(i);
        }
        out.pieces = collectPieces(scene, settings.scale, isGltfPath(path));
        return out;
    }

    SkinnedMesh ModelLoader::loadSkinned(const std::string& path)
    {
        const ModelImportSettings settings = readModelSettings(path);
        Assimp::Importer importer;
        configureImporter(importer, settings);
        const aiScene* scene = importer.ReadFile(path, assimpFlags(settings));

        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
        {
            // Aquí había un printf de depuración que leía scene->mNumMeshes —
            // dentro del if cuya PRIMERA condición es !scene—. Un fichero que no
            // existe mataba el proceso con un segfault mudo en vez de lanzar, y
            // eso convertía en mentira el catch de nodeFromJson
            // (Scene.cpp:1683), que promete que un asset movido o borrado deja
            // el nodo sin mesh y el resto de la escena carga igual: el editor se
            // caía antes de llegar al catch. La ruta lleva ya al mensaje —
            // GetErrorString no la incluye— porque el Log Console lo enseña sin
            // más contexto que este.
            throw std::runtime_error("Assimp loadSkinned '" + path + "': " +
                                     std::string(importer.GetErrorString()));
        }
        
        // --- Registro de huesos desde TODOS los meshes ---
        std::unordered_map<std::string, int> boneMapOld;
        std::vector<std::string>             boneNamesOld;
        std::vector<glm::mat4>               invBindOld;

        for (uint32_t m = 0; m < scene->mNumMeshes; m++)
        {
            aiMesh* aim = scene->mMeshes[m];
            for (uint32_t b = 0; b < aim->mNumBones; b++)
            {
                std::string name = aim->mBones[b]->mName.C_Str();
                if (!boneMapOld.count(name))
                {
                    boneMapOld[name] = (int)boneNamesOld.size();
                    boneNamesOld.push_back(name);
                    invBindOld.push_back(aiToGlm(aim->mBones[b]->mOffsetMatrix));
                }
            }
        }
        int numBones = (int)boneNamesOld.size();

        // --- Vértices e índices de TODOS los meshes ---
        SkinnedMesh smesh;
        uint32_t vertexOffset = 0;

        using Slot = std::pair<int,float>;
        for (uint32_t m = 0; m < scene->mNumMeshes; m++)
        {
            aiMesh* aim = scene->mMeshes[m];
            uint32_t numVerts = aim->mNumVertices;
            uint32_t idxStart = (uint32_t)smesh.indices.size();

            std::vector<std::array<Slot,4>> tempW(numVerts, {{{-1,0.f},{-1,0.f},{-1,0.f},{-1,0.f}}});
            std::vector<int> slotCount(numVerts, 0);

            for (uint32_t b = 0; b < aim->mNumBones; b++)
            {
                int bIdx = boneMapOld[aim->mBones[b]->mName.C_Str()];
                for (uint32_t w = 0; w < aim->mBones[b]->mNumWeights; w++)
                {
                    uint32_t vIdx = aim->mBones[b]->mWeights[w].mVertexId;
                    float    wt   = aim->mBones[b]->mWeights[w].mWeight;
                    int      slot = slotCount[vIdx];
                    if (slot < 4) { tempW[vIdx][slot] = {bIdx, wt}; slotCount[vIdx]++; }
                }
            }

            for (uint32_t i = 0; i < numVerts; i++)
            {
                SkinnedVertex v{};
                v.position = { aim->mVertices[i].x * settings.scale,
                               aim->mVertices[i].y * settings.scale,
                               aim->mVertices[i].z * settings.scale, 1.0f };
                v.normal   = { aim->mNormals[i].x,  aim->mNormals[i].y,  aim->mNormals[i].z,  0.0f };
                v.tangent  = aim->mTangents
                    ? glm::vec4{ aim->mTangents[i].x, aim->mTangents[i].y, aim->mTangents[i].z, 0.0f }
                    : glm::vec4{ 1.0f, 0.0f, 0.0f, 0.0f };
                v.uv_pad   = aim->mTextureCoords[0]
                    ? glm::vec4{ aim->mTextureCoords[0][i].x, aim->mTextureCoords[0][i].y, 0.0f, 0.0f }
                    : glm::vec4{ 0.0f };
                v.color    = { 1.0f, 1.0f, 1.0f, 1.0f };

                float crudos[4], normalizados[4];
                for (int s = 0; s < 4; s++) crudos[s] = tempW[i][s].second;
                // Sin pesos el vértice no lo mueve ningún hueso. Se cuentan para
                // poder decirlo: el shader lo resuelve con identidad (se queda en
                // su sitio en espacio de modelo), pero verlo quieto mientras el
                // resto anima parece un fallo del motor y es del FBX.
                if (!normalizeBoneWeights(crudos, normalizados)) smesh.verticesWithoutWeights++;
                for (int s = 0; s < 4; s++)
                {
                    v.boneIndices[s] = (tempW[i][s].first < 0) ? 0 : tempW[i][s].first;
                    v.boneWeights[s] = normalizados[s];
                }
                smesh.skinnedVertices.push_back(v);
            }

            for (uint32_t i = 0; i < aim->mNumFaces; i++)
                for (uint32_t j = 0; j < aim->mFaces[i].mNumIndices; j++)
                    smesh.indices.push_back(aim->mFaces[i].mIndices[j] + vertexOffset);

            SubMeshRange range{};
            range.indexStart   = idxStart;
            range.indexCount   = (uint32_t)smesh.indices.size() - idxStart;
            range.materialIndex = aim->mMaterialIndex;
            smesh.subMeshRanges.push_back(range);

            vertexOffset += numVerts;
        }

        // --- Topological sort: DFS sobre aiNode, solo huesos conocidos ---
        std::vector<int> topoOrder;
        topoOrder.reserve(numBones);

        std::function<void(aiNode*)> collectOrder = [&](aiNode* node)
        {
            std::string name = node->mName.C_Str();
            if (boneMapOld.count(name))
                topoOrder.push_back(boneMapOld[name]);
            for (uint32_t c = 0; c < node->mNumChildren; c++)
                collectOrder(node->mChildren[c]);
        };
        collectOrder(scene->mRootNode);

        // --- Parent map: para cada hueso, el ancestro más cercano que también es hueso ---
        std::unordered_map<std::string,std::string> boneParentName;

        std::function<void(aiNode*, const std::string&)> buildParent =
            [&](aiNode* node, const std::string& nearestBone)
        {
            std::string name = node->mName.C_Str();
            bool isBone = boneMapOld.count(name) > 0;
            if (isBone) boneParentName[name] = nearestBone;
            std::string next = isBone ? name : nearestBone;
            for (uint32_t c = 0; c < node->mNumChildren; c++)
                buildParent(node->mChildren[c], next);
        };
        buildParent(scene->mRootNode, "");

        // --- Remap: oldIdx → newIdx ---
        std::vector<int> remap(numBones, -1);
        for (int newIdx = 0; newIdx < (int)topoOrder.size(); newIdx++)
            remap[topoOrder[newIdx]] = newIdx;

        // --- Construir Skeleton en nuevo orden ---
        Skeleton& skel = smesh.skeleton;
        skel.names.resize(numBones);
        skel.parentIndex.resize(numBones);
        skel.inverseBindPose.resize(numBones);

        for (int newIdx = 0; newIdx < numBones; newIdx++)
        {
            int oldIdx = topoOrder[newIdx];
            const std::string& name = boneNamesOld[oldIdx];
            skel.names[newIdx]          = name;
            skel.inverseBindPose[newIdx] = invBindOld[oldIdx];
            // Escalar el modelo x s escala la TRASLACION del offset del hueso x s
            // (la rotacion no cambia); el bindLocal de GPU se deriva de esto.
            skel.inverseBindPose[newIdx][3].x *= settings.scale;
            skel.inverseBindPose[newIdx][3].y *= settings.scale;
            skel.inverseBindPose[newIdx][3].z *= settings.scale;
            skel.boneMap[name]           = newIdx;

            const std::string& pName = boneParentName.count(name) ? boneParentName[name] : "";
            skel.parentIndex[newIdx] = (pName.empty() || !boneMapOld.count(pName))
                ? -1 : remap[boneMapOld[pName]];
        }

        // --- Remap de bone indices en vértices ---
        for (auto& v : smesh.skinnedVertices)
            for (int s = 0; s < 4; s++)
                if (v.boneWeights[s] > 0.0f)
                    v.boneIndices[s] = remap[v.boneIndices[s]];

        // --- Animaciones: todas las del fichero ---
        // importAnimations = false: sin clips, pero la fuente builtin de abajo se
        // registra igual (la UI necesita una fila que represente al modelo).
        for (uint32_t a = 0; settings.importAnimations && a < scene->mNumAnimations; a++)
        {
            int mapped = 0, total = 0;
            AnimationClip clip = clipFromAssimp(scene->mAnimations[a], skel, mapped, total, nullptr);
            // Take estático: no anima nada y se llevaría el clip 0, que es
            // donde caen todos los caminos degradados del motor. Ver
            // clipHasMotion. El mesh se queda sin clips y eso ya está
            // soportado: la fuente builtin de abajo se registra igual.
            if (!clipHasMotion(clip)) continue;
            // Nombres únicos y no vacíos: Mixamo exporta cada take como
            // "mixamo.com", y los FBX de Blender a veces sin nombre. El
            // Animator resuelve los clips por nombre, así que dos clips
            // homónimos harían que el segundo fuera inalcanzable.
            clip.name = uniqueClipName(smesh.animationClips, clip.name);
            scaleClipTranslations(clip, settings.scale);
            smesh.animationClips.push_back(std::move(clip));
        }

        // Fuente builtin: el propio FBX. Se registra siempre, incluso sin
        // animaciones — la UI necesita una fila que represente al modelo.
        {
            AnimationSource builtin;
            builtin.path    = path;
            builtin.builtin = true;
            for (const auto& c : smesh.animationClips)
                builtin.clipNames.push_back(c.name);
            smesh.animationSources.push_back(std::move(builtin));
        }

        // --- Materiales: uno por cada materialIndex único entre los submeshes ---
        {
            namespace fs = std::filesystem;
            fs::path modelDir = fs::path(path).parent_path();

            auto loadTexFromMat = [&](aiMaterial* mat, aiTextureType type,
                                      std::vector<uint8_t>& outEmb, std::string& outPath)
            {
                aiString texPath;
                if (mat->GetTexture(type, 0, &texPath) != AI_SUCCESS) return;
                const char* raw = texPath.C_Str();
                const aiTexture* emb = scene->GetEmbeddedTexture(raw);
                if (emb)
                {
                    if (emb->mHeight == 0)
                    {
                        const uint8_t* begin = reinterpret_cast<const uint8_t*>(emb->pcData);
                        outEmb.assign(begin, begin + emb->mWidth);
                    }
                    else
                    {
                        outEmb.resize(emb->mWidth * emb->mHeight * 4);
                        for (uint32_t k = 0; k < emb->mWidth * emb->mHeight; k++)
                        {
                            outEmb[k*4+0] = emb->pcData[k].r;
                            outEmb[k*4+1] = emb->pcData[k].g;
                            outEmb[k*4+2] = emb->pcData[k].b;
                            outEmb[k*4+3] = emb->pcData[k].a;
                        }
                    }
                }
                else outPath = resolveModelTexture(modelDir, raw).string();
            };

            std::unordered_map<uint32_t, uint32_t> matRemap;
            for (uint32_t m = 0; m < scene->mNumMeshes; m++)
            {
                uint32_t assimpIdx = scene->mMeshes[m]->mMaterialIndex;
                if (matRemap.count(assimpIdx)) continue;

                uint32_t newIdx = (uint32_t)smesh.materials.size();
                matRemap[assimpIdx] = newIdx;
                smesh.materials.emplace_back();
                Material& smat = smesh.materials.back();

                if (assimpIdx < scene->mNumMaterials)
                {
                    aiMaterial* mat = scene->mMaterials[assimpIdx];
                    loadTexFromMat(mat, aiTextureType_DIFFUSE, smat.embeddedTexture,          smat.texturePath);
                    loadTexFromMat(mat, aiTextureType_NORMALS, smat.embeddedNormalMap,         smat.normalMapPath);
                    if (smat.embeddedNormalMap.empty() && smat.normalMapPath.empty())
                        loadTexFromMat(mat, aiTextureType_HEIGHT, smat.embeddedNormalMap,     smat.normalMapPath);
                    loadTexFromMat(mat, aiTextureType_UNKNOWN, smat.embeddedMetallicRoughness, smat.metallicRoughnessPath);
                }
            }

            for (auto& range : smesh.subMeshRanges)
                range.materialIndex = matRemap.at(range.materialIndex);
        }
        smesh.name = std::filesystem::path(path).stem().string();
        smesh.sourcePath = path;
        return smesh;
    }

    LoadedClips ModelLoader::loadAnimationClips(const std::string& path, const Skeleton& skel)
    {
        LoadedClips out;

        // Cada FBX usa SU sidecar: las claves de traslacion de este fichero tienen
        // que estar en las unidades del esqueleto al que se mapean, asi que la
        // escala se aplica aqui tambien. El resto de ajustes no le afectan.
        const ModelImportSettings settings = readModelSettings(path);
        Assimp::Importer importer;
        // Flags mínimos: aquí no se construye geometría, así que triangulate,
        // normales y tangentes serían trabajo tirado. Assimp lee las
        // animaciones igual.
        const aiScene* scene = importer.ReadFile(path, 0);

        const std::string file = std::filesystem::path(path).filename().string();

        if (!scene || !scene->mRootNode)
        {
            out.warnings.push_back(file + ": " + std::string(importer.GetErrorString()));
            return out;
        }
        if (scene->mNumAnimations == 0)
        {
            out.warnings.push_back(file + ": no contiene animaciones");
            return out;
        }

        std::vector<std::string> unknownBones;
        for (uint32_t a = 0; a < scene->mNumAnimations; a++)
        {
            AnimationClip clip = clipFromAssimp(scene->mAnimations[a], skel,
                                                out.mappedChannels, out.totalChannels,
                                                &unknownBones);
            // Un clip sin un solo canal válido no aporta nada: se descarta
            // individualmente en vez de tumbar el fichero entero.
            if (clip.channels.empty()) continue;
            // Ídem un take estático (ver clipHasMotion). Aquí sí se avisa: el
            // usuario ha elegido este fichero a mano esperando animación, y sin
            // el warning vería una fuente que no aporta clips y ninguna razón.
            if (!clipHasMotion(clip))
            {
                out.warnings.push_back(file + ": el clip '" + clip.name +
                                       "' no anima nada (una sola key por canal), descartado");
                continue;
            }
            // Despues de decidir si anima: esa decision no depende de la escala.
            scaleClipTranslations(clip, settings.scale);
            out.clips.push_back(std::move(clip));
        }

        if (out.mappedChannels == 0)
        {
            out.warnings.push_back(file + ": ningún hueso coincide con el esqueleto (0/"
                                    + std::to_string(out.totalChannels) + " canales)");
            out.clips.clear();
            return out;
        }

        if (!unknownBones.empty())
        {
            std::string msg = file + ": " + std::to_string(out.mappedChannels) + "/"
                            + std::to_string(out.totalChannels) + " canales mapeados, "
                            + std::to_string(unknownBones.size()) + " huesos desconocidos ignorados (";
            // Solo los 5 primeros: la lista completa de un rig ajeno llenaría
            // el Log Console sin decir nada más de lo que dicen 5 ejemplos.
            const size_t shown = unknownBones.size() < 5 ? unknownBones.size() : 5;
            for (size_t i = 0; i < shown; i++)
                msg += (i ? ", " : "") + unknownBones[i];
            if (unknownBones.size() > shown) msg += ", ...";
            msg += ")";
            out.warnings.push_back(std::move(msg));
        }

        return out;
    }

    bool ModelLoader::hasBones(const std::string& path)
    {
        Assimp::Importer importer;
        // Flags a cero, igual que loadAnimationClips: aquí no se construye
        // geometría, así que triangulate, normales y tangentes serían trabajo
        // tirado. mNumBones se lee igual.
        const aiScene* scene = importer.ReadFile(path, 0);
        if (!scene || !scene->mRootNode) return false;

        for (uint32_t i = 0; i < scene->mNumMeshes; i++)
            if (scene->mMeshes[i]->mNumBones > 0) return true;

        return false;
    }

    std::shared_ptr<Mesh> ModelLoader::loadAuto(const std::string& path)
    {
        if (hasBones(path))
            return std::make_shared<SkinnedMesh>(loadSkinned(path));  // convierte solo a shared_ptr<Mesh>
        return std::make_shared<Mesh>(load(path));
    }

    namespace
    {
        std::string lowerExtOf(const std::filesystem::path& p)
        {
            std::string e = p.extension().string();
            for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return e;
        }

        // Relativa, sin raiz y sin componentes "..": se queda dentro de la carpeta.
        bool staysInside(const std::filesystem::path& rel)
        {
            if (rel.empty() || rel.has_root_path()) return false;
            for (const auto& part : rel)
                if (part == "..") return false;
            return true;
        }

        std::string percentDecode(const std::string& s)
        {
            std::string out;
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '%' && i + 2 < s.size() &&
                    std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(s[i + 2])))
                {
                    out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                }
                else out += s[i];
            }
            return out;
        }

        void addCompanion(std::vector<std::string>& out, const std::string& raw)
        {
            if (raw.empty() || raw.rfind("data:", 0) == 0) return;
            // "C:/x" no tiene raiz en Linux ni "/x" nombre de unidad en Windows:
            // se rechazan las dos formas en cualquier plataforma.
            if (raw.size() > 1 && raw[1] == ':') return;
            const std::filesystem::path rel = std::filesystem::path(raw).lexically_normal();
            if (!staysInside(rel)) return;
            const std::string s = rel.generic_string();
            if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
        }
    }

    bool ModelLoader::isSupportedModelExtension(const std::string& ext)
    {
        std::string e = ext;
        for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return e == ".fbx" || e == ".obj" || e == ".gltf" || e == ".glb";
    }

    const char* ModelLoader::supportedModelFilter()
    {
        return "Models{.fbx,.obj,.gltf,.glb}";
    }

    std::filesystem::path ModelLoader::resolveModelTexture(const std::filesystem::path& modelDir,
                                                           const std::string& raw)
    {
        namespace fs = std::filesystem;
        auto exists = [](const fs::path& p) { std::error_code ec; return fs::exists(p, ec) && !ec; };
        // Assimp pasa la URI de una imagen glTF tal cual, con %20 y compania: se
        // prueba tambien decodificada.
        const std::string decoded = percentDecode(raw);
        for (const std::string& r : { raw, decoded })
        {
            const bool     looksAbsolute = r.size() > 1 && r[1] == ':';
            const fs::path rel           = fs::path(r).lexically_normal();
            if (!looksAbsolute && staysInside(rel) && rel != rel.filename() && exists(modelDir / rel))
                return modelDir / rel;
        }
        const fs::path byName = modelDir / fs::path(raw).filename();
        if (decoded != raw && !exists(byName) && exists(modelDir / fs::path(decoded).filename()))
            return modelDir / fs::path(decoded).filename();
        return byName;
    }

    std::vector<std::string> ModelLoader::modelCompanionFiles(const std::string& path)
    {
        namespace fs = std::filesystem;
        std::vector<std::string> out;
        try
        {
            const std::string ext = lowerExtOf(path);
            if (ext == ".obj")
            {
                std::ifstream in{ fs::path(path) };
                std::string line;
                while (std::getline(in, line))
                {
                    if (line.rfind("mtllib", 0) != 0 || line.size() < 7 ||
                        !std::isspace(static_cast<unsigned char>(line[6])))
                        continue;
                    size_t b = 7, e = line.size();
                    while (b < e && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
                    while (e > b && std::isspace(static_cast<unsigned char>(line[e - 1]))) --e;
                    addCompanion(out, line.substr(b, e - b));
                }
                // Las texturas que nombra cada .mtl, con su ruta TAL CUAL: el loader
                // las resuelve respecto a la carpeta del modelo. Las opciones
                // (-o, -s, -bm...) van delante, el nombre es el ultimo token.
                const std::vector<std::string> libraries = out;
                for (const std::string& lib : libraries)
                {
                    std::ifstream mtl{ fs::path(path).parent_path() / fs::path(lib) };
                    while (std::getline(mtl, line))
                    {
                        std::istringstream words(line);
                        std::string key, token, last;
                        words >> key;
                        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (key.rfind("map_", 0) != 0 && key != "bump" && key != "norm" &&
                            key != "disp" && key != "refl")
                            continue;
                        while (words >> token) last = token;
                        addCompanion(out, last);
                    }
                }
            }
            else if (ext == ".gltf")
            {
                std::ifstream in{ fs::path(path) };
                const nlohmann::json j = nlohmann::json::parse(in, nullptr, /*allow_exceptions*/ false);
                if (!j.is_object()) return {};
                for (const char* key : { "buffers", "images" })
                {
                    const auto it = j.find(key);
                    if (it == j.end() || !it->is_array()) continue;
                    for (const auto& item : *it)
                        if (item.is_object() && item.contains("uri") && item["uri"].is_string())
                            addCompanion(out, percentDecode(item["uri"].get<std::string>()));
                }
            }
        }
        catch (...) { return {}; }
        return out;
    }

    ModelPreview ModelLoader::loadPreview(const std::string& path)
    {
        namespace fs = std::filesystem;
        ModelPreview out;
        try
        {
            // El sidecar es dependencia exista o no: crearlo tambien cambia el
            // aspecto. Todo se sella ANTES de leerlo (ver FileStamp).
            out.dependencies.push_back(stampFile(importSidecarPath(path)));
            // Lo que el modelo lee ademas de si mismo (.mtl, .bin, texturas de un
            // .gltf), sellado ANTES de que Assimp lo lea.
            for (const std::string& rel : modelCompanionFiles(path))
                out.dependencies.push_back(stampFile(fs::path(path).parent_path() / fs::path(rel)));

            const ModelImportSettings settings = readModelSettings(path);
            Assimp::Importer importer;
            configureImporter(importer, settings);
            // Sin tangentes: la miniatura no usa normal map.
            const aiScene* scene = importer.ReadFile(path, assimpFlags(settings) & ~aiProcess_CalcTangentSpace);
            if (!scene || !scene->mRootNode) return out;
            if (scene->mNumMeshes == 0)
            {
                // Assimp marca INCOMPLETE un fichero sin mallas: con clips es un
                // FBX de solo animacion, no uno roto.
                if (scene->mNumAnimations > 0) out.status = PreviewStatus::AnimationOnly;
                return out;
            }
            if (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) return out;

            bool skinned = false;
            for (uint32_t m = 0; m < scene->mNumMeshes; ++m)
                skinned = skinned || scene->mMeshes[m]->mNumBones > 0;

            // Con huesos, todas las submallas en bind pose (como loadSkinned). Sin
            // huesos y con MAS de una aparicion en los nodos, una parte POR
            // aparicion con su transform (igual que StaticModel::pieces via
            // collectPieces): la miniatura deja de fingir que el modelo es una
            // unica malla cuando no lo es. Con una aparicion o ninguna, lo de
            // siempre -- malla 0 sin transformar -- que es lo que de verdad pinta
            // un objeto sin repartir en hijos (loadAuto).
            struct Appearance { uint32_t meshIndex; glm::mat4 transform; };
            std::vector<Appearance> appearances;
            if (skinned)
            {
                for (uint32_t m = 0; m < scene->mNumMeshes; ++m)
                    appearances.push_back({ m, glm::mat4(1.0f) });
            }
            else
            {
                const std::vector<ModelPiece> pieces = collectPieces(scene, settings.scale, isGltfPath(path));
                if (pieces.size() > 1)
                {
                    for (const ModelPiece& piece : pieces)
                        if (piece.piece >= 0 && static_cast<uint32_t>(piece.piece) < scene->mNumMeshes)
                            appearances.push_back({ static_cast<uint32_t>(piece.piece), piece.transform });
                }
                else if (scene->mNumMeshes > 0)
                {
                    appearances.push_back({ 0, glm::mat4(1.0f) });
                }
            }

            const fs::path modelDir = fs::path(path).parent_path();
            std::unordered_map<uint32_t, PreviewImage> albedoByMaterial;
            auto albedoOf = [&](uint32_t matIndex) -> const PreviewImage& {
                auto it = albedoByMaterial.find(matIndex);
                if (it != albedoByMaterial.end()) return it->second;
                PreviewImage img;
                aiString texPath;
                if (matIndex < scene->mNumMaterials &&
                    scene->mMaterials[matIndex]->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS)
                {
                    const aiTexture* emb = scene->GetEmbeddedTexture(texPath.C_Str());
                    if (emb && emb->mHeight == 0)
                    {
                        img = decodePreviewImage(reinterpret_cast<const uint8_t*>(emb->pcData), emb->mWidth);
                    }
                    else if (emb)
                    {
                        if (static_cast<uint64_t>(emb->mWidth) * emb->mHeight <= kPreviewMaxSourcePixels)
                        {
                            std::vector<uint8_t> raw(static_cast<size_t>(emb->mWidth) * emb->mHeight * 4);
                            for (size_t k = 0; k < static_cast<size_t>(emb->mWidth) * emb->mHeight; ++k)
                            {
                                raw[k * 4 + 0] = emb->pcData[k].r;
                                raw[k * 4 + 1] = emb->pcData[k].g;
                                raw[k * 4 + 2] = emb->pcData[k].b;
                                raw[k * 4 + 3] = emb->pcData[k].a;
                            }
                            img = downscalePreviewImage(raw.data(), static_cast<int>(emb->mWidth), static_cast<int>(emb->mHeight));
                        }
                    }
                    else
                    {
                        // Misma resolucion que load/loadSkinned: el nombre, junto al modelo.
                        const fs::path ext = resolveModelTexture(modelDir, texPath.C_Str());
                        out.dependencies.push_back(stampFile(ext));    // antes de leerla
                        img = loadPreviewImage(ext);
                    }
                }
                return albedoByMaterial.emplace(matIndex, std::move(img)).first->second;
            };

            for (const Appearance& app : appearances)
            {
                const aiMesh* ai = scene->mMeshes[app.meshIndex];
                // Identidad para el caso de siempre: mat3(1) invertida y traspuesta
                // sigue siendo la identidad, así que la normal no cambia.
                const glm::mat3 normalMat = glm::transpose(glm::inverse(glm::mat3(app.transform)));
                PreviewPart part;
                part.positions.reserve(ai->mNumVertices);
                for (uint32_t i = 0; i < ai->mNumVertices; ++i)
                {
                    const glm::vec3 pos(ai->mVertices[i].x * settings.scale,
                                        ai->mVertices[i].y * settings.scale,
                                        ai->mVertices[i].z * settings.scale);
                    part.positions.push_back(glm::vec3(app.transform * glm::vec4(pos, 1.0f)));
                    const glm::vec3 n = ai->mNormals
                        ? glm::vec3(ai->mNormals[i].x, ai->mNormals[i].y, ai->mNormals[i].z)
                        : glm::vec3(0.0f, 1.0f, 0.0f);
                    const glm::vec3 tn = normalMat * n;
                    const float len = glm::length(tn);
                    // Un eje (casi) aplastado da inf en la matriz normal, o una
                    // longitud que desborda a inf con componentes finitas:
                    // `len > 1e-8f` deja pasar el inf y tn / inf es NaN o el
                    // vector cero. Entonces, la normal del fichero sin tocar.
                    part.normals.push_back(std::isfinite(len) && len > 1e-8f ? tn / len : n);
                    part.uvs.push_back(ai->mTextureCoords[0]
                        ? glm::vec2(ai->mTextureCoords[0][i].x, ai->mTextureCoords[0][i].y)
                        : glm::vec2(0.0f));
                    part.colors.emplace_back(1.0f);
                }
                for (uint32_t f = 0; f < ai->mNumFaces; ++f)
                    if (ai->mFaces[f].mNumIndices == 3)
                        for (uint32_t j = 0; j < 3; ++j) part.indices.push_back(ai->mFaces[f].mIndices[j]);
                part.albedo = albedoOf(ai->mMaterialIndex);
                out.parts.push_back(std::move(part));
            }
            out.status = PreviewStatus::Ok;
        }
        catch (...)
        {
            out.status = PreviewStatus::Unreadable;
            out.parts.clear();
        }
        return out;
    }
}