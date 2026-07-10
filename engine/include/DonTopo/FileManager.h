#pragma once
#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace DonTopo
{
    // Wrapper de I/O de ficheros JSON, sin estado y sin conocer Scene/GameObject
    // — reutilizable para otros ficheros del motor (config, presets) además
    // de la serialización de escena.
    class FileManager
    {
        public:
            // Escribe j formateado (pretty-print, indent 2) en path. false si
            // el fichero no se pudo abrir/escribir.
            static bool writeJson(const std::string& path, const nlohmann::json& j);

            // Lee y parsea path. std::nullopt si el fichero no existe o el
            // JSON es inválido (nunca lanza excepción hacia el caller).
            static std::optional<nlohmann::json> readJson(const std::string& path);
    };
}
