#include "DonTopo/ModelLoader.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <stdexcept>
#include <filesystem>

namespace DonTopo 
{
    Mesh ModelLoader::load(const std::string &path)
    {
        Assimp::Importer importer;
        const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GenNormals | aiProcess_CalcTangentSpace );

        if(!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
        {
            throw std::runtime_error("Assimp: " + std::string(importer.GetErrorString()));
        }

        Mesh mesh;
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

            loadTex(aiTextureType_DIFFUSE, mesh.embeddedTexture, mesh.texturePath);
            loadTex(aiTextureType_NORMALS, mesh.embeddedNormalMap, mesh.normalMapPath);
            // Assimp suele guardar el normal map como HEIGHT en FBX
            if(mesh.embeddedNormalMap.empty() && mesh.normalMapPath.empty())
                loadTex(aiTextureType_HEIGHT, mesh.embeddedNormalMap, mesh.normalMapPath);
        }

        return mesh;
    }
}