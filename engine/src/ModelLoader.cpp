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
        const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_GenNormals);

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
            v.normal = {ai->mNormals[i].x, ai->mNormals[i].y, ai->mNormals[i].z};
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
            aiString texPath;
            if(mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS)
            {
                const char* raw = texPath.C_Str();
                const aiTexture* embedded = scene->GetEmbeddedTexture(raw);
                if(embedded)
                {
                    if(embedded->mHeight == 0)
                    {
                        // Compressed (PNG/JPG): mWidth = byte count
                        const uint8_t* begin = reinterpret_cast<const uint8_t*>(embedded->pcData);
                        mesh.embeddedTexture.assign(begin, begin + embedded->mWidth);
                    }
                    else
                    {
                        // Uncompressed ARGB8888: convert to RGBA
                        mesh.embeddedTexture.resize(embedded->mWidth * embedded->mHeight * 4);
                        for(uint32_t i = 0; i < embedded->mWidth * embedded->mHeight; i++)
                        {
                            mesh.embeddedTexture[i*4+0] = embedded->pcData[i].r;
                            mesh.embeddedTexture[i*4+1] = embedded->pcData[i].g;
                            mesh.embeddedTexture[i*4+2] = embedded->pcData[i].b;
                            mesh.embeddedTexture[i*4+3] = embedded->pcData[i].a;
                        }
                    }
                }
                else
                {
                    namespace fs = std::filesystem;
                    fs::path modelDir = fs::path(path).parent_path();
                    fs::path texFilename = fs::path(raw).filename();
                    mesh.texturePath = (modelDir / texFilename).string();
                }
            }
        }

        return mesh;
    }
}