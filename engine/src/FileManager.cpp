#include "DonTopo/FileManager.h"
#include <fstream>

namespace DonTopo
{
    bool FileManager::writeJson(const std::string& path, const nlohmann::json& j)
    {
        std::ofstream out(path);
        if (!out.is_open())
            return false;

        out << j.dump(2);
        return out.good();
    }

    std::optional<nlohmann::json> FileManager::readJson(const std::string& path)
    {
        std::ifstream in(path);
        if (!in.is_open())
            return std::nullopt;

        nlohmann::json j;
        try
        {
            in >> j;
        }
        catch (const nlohmann::json::parse_error&)
        {
            return std::nullopt;
        }
        return j;
    }
}
