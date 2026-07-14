#include "DonTopo/Files/FileManager.h"
#include <fstream>
#include <sstream>

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

    std::optional<std::string> FileManager::readText(const std::string& path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
            return std::nullopt;

        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    bool FileManager::writeText(const std::string& path, const std::string& content)
    {
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open())
            return false;

        out << content;
        return out.good();
    }
}
