#include "DonTopo/Renderer/ModelLoader.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <stdexcept>
#include <filesystem>
#include <string>
#include <functional>
#include <array>

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
            aiAnimation* anim = scene->mAnimations[a];
            AnimationClip clip;

            // Nombres únicos y no vacíos: Mixamo exporta cada take como
            // "mixamo.com", y los FBX de Blender a veces sin nombre. El
            // Animator resuelve los clips por nombre, así que dos clips
            // homónimos harían que el segundo fuera inalcanzable.
            std::string base = anim->mName.C_Str();
            if (base.empty()) base = "Animation " + std::to_string(a);
            std::string unique = base;
            int suffix = 1;
            auto taken = [&](const std::string& n) {
                for (const auto& c : smesh.animationClips)
                    if (c.name == n) return true;
                return false;
            };
            while (taken(unique)) unique = base + " (" + std::to_string(suffix++) + ")";

            clip.name            = unique;
            clip.duration        = (float)anim->mDuration;
            clip.ticksPerSecond  = (anim->mTicksPerSecond > 0.0) ? (float)anim->mTicksPerSecond : 24.0f;

            for (uint32_t c = 0; c < anim->mNumChannels; c++)
            {
                aiNodeAnim* ch = anim->mChannels[c];
                std::string boneName = ch->mNodeName.C_Str();
                if (!skel.boneMap.count(boneName)) continue;

                BoneChannel bc;
                bc.boneIndex = skel.boneMap[boneName];

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
            smesh.animationClips.push_back(std::move(clip));
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
}