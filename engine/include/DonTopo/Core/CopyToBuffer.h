#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace DonTopo
{
    // Copia a un buffer fijo (los de ImGui::InputText) truncando y terminando
    // siempre en '\0'. Sustituye a strncpy_s, que solo existe en MSVC.
    template<std::size_t N>
    void copyToBuffer(char (&buf)[N], std::string_view s)
    {
        const std::size_t n = std::min(s.size(), N - 1);
        std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
    }
}
