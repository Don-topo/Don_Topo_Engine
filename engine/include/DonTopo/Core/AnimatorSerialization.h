#pragma once
#include <nlohmann/json_fwd.hpp>

namespace DonTopo
{
    class AnimatorComponent;

    // The "animator" block of the .scene. The definition lives in Scene.cpp, because the
    // format belongs to the scene. It is declared here so that the editor undo
    // compares graphs with the SAME yardstick used to save them.
    nlohmann::json animatorToJson(const AnimatorComponent& a);

    // animatorToJson without the node positions ("pos" of each state): what
    // the AnimatorPanel undo considers an edit. Moving a node is not
    // one (see docs/superpowers/specs/2026-09-11-animator-graph-undo-design.md).
    // Any field added to the .scene enters here without touching anything else.
    nlohmann::json animatorGraphKey(const AnimatorComponent& a);
}
