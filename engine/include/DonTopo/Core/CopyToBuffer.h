#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace DonTopo
{
    // Copies to a fixed buffer (the ImGui::InputText ones) truncating and always
    // terminating in '\0'. Replaces strncpy_s, which only exists in MSVC.
    template<std::size_t N>
    void copyToBuffer(char (&buf)[N], std::string_view s)
    {
        const std::size_t n = std::min(s.size(), N - 1);
        std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
    }
}
