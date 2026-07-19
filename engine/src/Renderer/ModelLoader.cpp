#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMeshAnimations.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
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

    Mesh ModelLoader::load(const std::string &path)
    {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GenNormals | aiProcess_CalcTangentSpace );

        if(!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
        {
            throw std::runtime_error("Assimp: " + std::string(importer.GetErrorString()));
        }

        Mesh mesh;
        mesh.name = std::filesystem::path(path).stem().string();
        mesh.sourcePath = path;
        aiMesh* ai = scene->mMeshes[0];

        mesh.vertices.reserve(ai->mNumVertices);
        for(uint32_t i = 0; i < ai->mNumVertices; i++)
        {
            Vertex v{};
            v.pos   = { ai->mVertices[i].x, ai->mVertices[i].y, ai->mVertices[i].z};
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
                    outPath = (modelDir / fs::path(raw).filename()).string();
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

    SkinnedMesh ModelLoader::loadSkinned(const std::string& path)
    {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate | aiProcess_FlipUVs |
        aiProcess_GenNormals  | aiProcess_CalcTangentSpace /*|
        aiProcess_LimitBoneWeights*/);

        if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
        {
            printf("meshes: %u, anims: %u, bones in mesh[0]: %u\n",
            scene->mNumMeshes,
            scene->mNumAnimations,
            scene->mNumMeshes > 0 ? scene->mMeshes[0]->mNumBones : 0);
            fflush(stdout);
            throw std::runtime_error("Assimp loadSkinned: " + std::string(importer.GetErrorString()));
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
                v.position = { aim->mVertices[i].x, aim->mVertices[i].y, aim->mVertices[i].z, 1.0f };
                v.normal   = { aim->mNormals[i].x,  aim->mNormals[i].y,  aim->mNormals[i].z,  0.0f };
                v.tangent  = aim->mTangents
                    ? glm::vec4{ aim->mTangents[i].x, aim->mTangents[i].y, aim->mTangents[i].z, 0.0f }
                    : glm::vec4{ 1.0f, 0.0f, 0.0f, 0.0f };
                v.uv_pad   = aim->mTextureCoords[0]
                    ? glm::vec4{ aim->mTextureCoords[0][i].x, aim->mTextureCoords[0][i].y, 0.0f, 0.0f }
                    : glm::vec4{ 0.0f };
                v.color    = { 1.0f, 1.0f, 1.0f, 1.0f };

                float totalW = 0.0f;
                for (int s = 0; s < 4; s++) totalW += tempW[i][s].second;
                for (int s = 0; s < 4; s++)
                {
                    v.boneIndices[s] = (tempW[i][s].first < 0) ? 0 : tempW[i][s].first;
                    v.boneWeights[s] = (totalW > 0.0f) ? tempW[i][s].second / totalW : 0.0f;
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
        for (uint32_t a = 0; a < scene->mNumAnimations; a++)
        {
            int mapped = 0, total = 0;
            AnimationClip clip = clipFromAssimp(scene->mAnimations[a], skel, mapped, total, nullptr);
            // Nombres únicos y no vacíos: Mixamo exporta cada take como
            // "mixamo.com", y los FBX de Blender a veces sin nombre. El
            // Animator resuelve los clips por nombre, así que dos clips
            // homónimos harían que el segundo fuera inalcanzable.
            clip.name = uniqueClipName(smesh.animationClips, clip.name);
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
                else outPath = (modelDir / fs::path(raw).filename()).string();
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
}