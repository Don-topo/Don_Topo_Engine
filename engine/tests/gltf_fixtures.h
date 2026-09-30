#pragma once
// Hand-written .gltf files for the piece tests. In their own namespace so as not
// to clash with the helpers of each test file.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace dt_fixture {

inline std::string base64(const std::vector<uint8_t>& in)
{
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3)
    {
        uint32_t n = static_cast<uint32_t>(in[i]) << 16;
        if (i + 1 < in.size()) n |= static_cast<uint32_t>(in[i + 1]) << 8;
        if (i + 2 < in.size()) n |= in[i + 2];
        out += T[(n >> 18) & 63];
        out += T[(n >> 12) & 63];
        out += i + 1 < in.size() ? T[(n >> 6) & 63] : '=';
        out += i + 2 < in.size() ? T[n & 63] : '=';
    }
    return out;
}

// Two meshes and three nodes at the scene root:
//   A: mesh 0 (triangle in XY),  translation (5, 0, 0)
//   B: mesh 1 (triangle in YZ),  scale 2
//   C: mesh 0 again,             translation (0, 0, -3)
// Buffer: positions A (36 B) + UV (24 B) + positions B (36 B) = 96 bytes.
inline void writeThreePieceGltf(const std::filesystem::path& p)
{
    const float data[24] = { 0, 0, 0,  1, 0, 0,  0, 1, 0,     // positions A
                             0, 0,  1, 0,  0, 1,              // UV
                             0, 0, 0,  0, 1, 0,  0, 0, 1 };   // positions B
    std::vector<uint8_t> buf(sizeof(data));
    std::memcpy(buf.data(), data, sizeof(data));
    std::ofstream(p) << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1,2]}],)"
        R"("nodes":[{"name":"A","mesh":0,"translation":[5,0,0]},)"
                 R"({"name":"B","mesh":1,"scale":[2,2,2]},)"
                 R"({"name":"C","mesh":0,"translation":[0,0,-3]}],)"
        R"("meshes":[{"name":"triA","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1}}]},)"
                  R"({"name":"triB","primitives":[{"attributes":{"POSITION":2,"TEXCOORD_0":1}}]}],)"
        R"("buffers":[{"uri":"data:application/octet-stream;base64,)" << base64(buf) << R"(","byteLength":96}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24},)"
                       R"({"buffer":0,"byteOffset":60,"byteLength":36}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
                     R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},)"
                     R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0,1,1]}]})";
}

// Node A (translation (5,0,0)) with a child B (scale 2) that carries the mesh: the
// scene root comes in with identity, so ONE single level under the root does not
// tell the composition order apart (m * child vs child * m, see collectPieces).
// With two real levels, the translation of A*B gives (5,0,0); B*A would give (10,0,0).
// "Vacio" is a second node at the scene root, without it: Assimp does not create a
// synthetic root node when there is only ONE node in scene.nodes, and A would become
// scene->mRootNode itself. In glTF the root DOES contribute its transform
// (only FBX discards it), so the result would be the same, but then
// the test would not tell "child of the root" apart from "nested node": with "Vacio" the
// root is the synthetic one (identity) and A and B are two real levels.
inline void writeNestedNodeGltf(const std::filesystem::path& p)
{
    const float data[9] = { 0, 0, 0,  1, 0, 0,  0, 1, 0 };   // triangle in XY
    std::vector<uint8_t> buf(sizeof(data));
    std::memcpy(buf.data(), data, sizeof(data));
    std::ofstream(p) << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,2]}],)"
        R"("nodes":[{"name":"A","children":[1],"translation":[5,0,0]},)"
                 R"({"name":"B","mesh":0,"scale":[2,2,2]},)"
                 R"({"name":"Vacio"}],)"
        R"("meshes":[{"name":"triB","primitives":[{"attributes":{"POSITION":0}}]}],)"
        R"("buffers":[{"uri":"data:application/octet-stream;base64,)" << base64(buf) << R"(","byteLength":36}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}]})";
}

// Node "T" with a triangle mesh and node "L" with a mesh with NO
// triangle at all (LINES primitive, two vertices): collectPieces must only generate
// a piece for "T". The line carries its own normal in the file so that
// meshFromAssimp does not depend on whether GenNormals knows how (or not) to generate
// them for a primitive without 3-vertex faces.
inline void writeMixedTriangleAndLineGltf(const std::filesystem::path& p)
{
    const float data[15] = { 0, 0, 0,  1, 0, 0,  0, 1, 0,      // triangle (mesh 0)
                              0, 0, 0,  0, 0, 1 };              // line segment (mesh 1)
    const float normals[6] = { 0, 1, 0,  0, 1, 0 };             // explicit normal of the line
    std::vector<uint8_t> buf(sizeof(data) + sizeof(normals));
    std::memcpy(buf.data(), data, sizeof(data));
    std::memcpy(buf.data() + sizeof(data), normals, sizeof(normals));
    std::ofstream(p) << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],)"
        R"("nodes":[{"name":"T","mesh":0},{"name":"L","mesh":1}],)"
        R"("meshes":[{"name":"tri","primitives":[{"attributes":{"POSITION":0}}]},)"
                  R"({"name":"line","primitives":[{"attributes":{"POSITION":1,"NORMAL":2},"mode":1}]}],)"
        R"("buffers":[{"uri":"data:application/octet-stream;base64,)" << base64(buf) << R"(","byteLength":84}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
                       R"({"buffer":0,"byteOffset":36,"byteLength":24},)"
                       R"({"buffer":0,"byteOffset":60,"byteLength":24}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
                     R"({"bufferView":1,"componentType":5126,"count":2,"type":"VEC3","min":[0,0,0],"max":[0,0,1]},)"
                     R"({"bufferView":2,"componentType":5126,"count":2,"type":"VEC3"}]})";
}

// ONE single node in scene.nodes ("Raiz", scale 2) with two children that carry the
// mesh: P translated (1,0,0) and Q translated (0,0,1). With a single first-level
// node Assimp (glTF2) does NOT create a synthetic root: "Raiz" IS
// scene->mRootNode, and its scale is the author's axis/unit correction.
inline void writeSingleRootNodeGltf(const std::filesystem::path& p)
{
    const float data[9] = { 0, 0, 0,  1, 0, 0,  0, 1, 0 };   // triangle in XY
    std::vector<uint8_t> buf(sizeof(data));
    std::memcpy(buf.data(), data, sizeof(data));
    std::ofstream(p) << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
        R"("nodes":[{"name":"Raiz","children":[1,2],"scale":[2,2,2]},)"
                 R"({"name":"P","mesh":0,"translation":[1,0,0]},)"
                 R"({"name":"Q","mesh":0,"translation":[0,0,1]}],)"
        R"("meshes":[{"name":"tri","primitives":[{"attributes":{"POSITION":0}}]}],)"
        R"("buffers":[{"uri":"data:application/octet-stream;base64,)" << base64(buf) << R"(","byteLength":36}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}]})";
}

// Two nodes with the same mesh (triangle in XY with normals from the file
// (0.6, 0, 0.8)): "Aplastado" with scale 1e-20 in X and "Normal" untransformed.
// The normal matrix of "Aplastado" is diag(1e20, 1, 1): the transformed
// normal has finite components but its length overflows the float to
// inf, and normalizing by inf gives the zero vector.
inline void writeFlattenedPieceGltf(const std::filesystem::path& p)
{
    const float data[18] = { 0, 0, 0,  1, 0, 0,  0, 1, 0,              // positions
                             0.6f, 0, 0.8f,  0.6f, 0, 0.8f,  0.6f, 0, 0.8f };   // normales
    std::vector<uint8_t> buf(sizeof(data));
    std::memcpy(buf.data(), data, sizeof(data));
    std::ofstream(p) << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],)"
        R"("nodes":[{"name":"Aplastado","mesh":0,"scale":[1e-20,1,1]},)"
                 R"({"name":"Normal","mesh":0}],)"
        R"("meshes":[{"name":"tri","primitives":[{"attributes":{"POSITION":0,"NORMAL":1}}]}],)"
        R"("buffers":[{"uri":"data:application/octet-stream;base64,)" << base64(buf) << R"(","byteLength":72}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
                     R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"}]})";
}

} // namespace dt_fixture
