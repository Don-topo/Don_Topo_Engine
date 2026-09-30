#pragma once
#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace DonTopo
{
    // JSON file I/O wrapper, stateless and unaware of Scene/GameObject
    // — reusable for other engine files (config, presets) besides
    // scene serialization.
    class FileManager
    {
        public:
            // Writes j formatted (pretty-print, indent 2) to path. false if
            // the file could not be opened/written.
            static bool writeJson(const std::string& path, const nlohmann::json& j);

            // Reads and parses path. std::nullopt if the file does not exist or the
            // JSON is invalid (never throws an exception to the caller).
            static std::optional<nlohmann::json> readJson(const std::string& path);

            // Reads the full contents of path as plain text. std::nullopt
            // if the file does not exist or could not be opened.
            static std::optional<std::string> readText(const std::string& path);

            // Writes content to path, replacing any previous contents.
            // false if the file could not be opened/written.
            static bool writeText(const std::string& path, const std::string& content);
    };
}
