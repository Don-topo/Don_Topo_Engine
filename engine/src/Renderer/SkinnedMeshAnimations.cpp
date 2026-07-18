#include "DonTopo/Renderer/SkinnedMeshAnimations.h"

namespace DonTopo
{
    std::string uniqueClipName(const std::vector<AnimationClip>& existing,
                               const std::string& base)
    {
        const std::string root = base.empty() ? std::string("Animation") : base;

        auto taken = [&](const std::string& n) {
            for (const auto& c : existing)
                if (c.name == n) return true;
            return false;
        };

        if (!taken(root)) return root;

        int suffix = 1;
        std::string candidate = root + " (" + std::to_string(suffix) + ")";
        while (taken(candidate))
            candidate = root + " (" + std::to_string(++suffix) + ")";
        return candidate;
    }
}
