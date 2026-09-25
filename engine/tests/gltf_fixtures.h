#pragma once
// .gltf escritos a mano para los tests de piezas. En un namespace propio para
// no chocar con los helpers de cada fichero de test.
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

// Dos mallas y tres nodos en la raiz de la escena:
//   A: malla 0 (triangulo en XY),  traslacion (5, 0, 0)
//   B: malla 1 (triangulo en YZ),  escala 2
//   C: malla 0 otra vez,           traslacion (0, 0, -3)
// Buffer: posiciones A (36 B) + UV (24 B) + posiciones B (36 B) = 96 bytes.
inline void writeThreePieceGltf(const std::filesystem::path& p)
{
    const float data[24] = { 0, 0, 0,  1, 0, 0,  0, 1, 0,     // posiciones A
                             0, 0,  1, 0,  0, 1,              // UV
                             0, 0, 0,  0, 1, 0,  0, 0, 1 };   // posiciones B
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

} // namespace dt_fixture
