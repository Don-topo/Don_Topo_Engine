#pragma once
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace DonTopo
{
    // Decompose a transformation matrix without ending up with garbage when it
    // cannot be done.
    //
    // `glm::decompose` returns a **bool**, and for a singular matrix —one axis at
    // scale 0, which is what comes from typing a 0 in Scale.Y of the inspector—
    // it returns `false` **without writing any of its outputs**. Whoever does not look at
    // that return is left with their UNINITIALIZED local variables: in Debug,
    // the CRT 0xCDCDCDCD pattern, which as a float is -1.07374e+08. In Release
    // there is no pattern: it is arbitrary stack memory, the same failure without a recognizable
    // value to pin it on.
    //
    // That already cost a silent editor crash, a freeze on entering Play and
    // objects jumping to positions of 1e8 — three symptoms of this single failure,
    // which showed up one at a time as the previous one was fixed.
    //
    // Almost nothing needs decomposing, and that is why this can answer correctly
    // even if `glm::decompose` gives up:
    //
    //  - The TRANSLATION is the fourth column, and it stays so with a singular
    //    matrix.
    //  - The SCALE is the lengths of the columns. With one axis at 0 it gives
    //    (2, 0, 2), which is the correct answer and not an approximation.
    //  - The ROTATION is the only thing really lost, and it is that it does not exist: a
    //    flattened axis defines no orientation. Identity.
    //
    // Returns what `glm::decompose` returned, in case the caller cares to
    // tell apart a "real rotation" from "there was none to extract". The
    // outputs are valid in both cases.
    inline bool decomposeTransform(const glm::mat4& m,
                                   glm::vec3* outPos   = nullptr,
                                   glm::quat* outRot   = nullptr,
                                   glm::vec3* outScale = nullptr)
    {
        // From `glm::decompose` only the ROTATION is used, and only when it says
        // it could. The other outputs are ignored on purpose —position and scale
        // are taken from the matrix, below—, so it does not matter what it leaves written
        // in them. Even so they are initialized: if someday someone reads them from
        // here, let them find neutral values and not virgin memory.
        glm::vec3 scale{1.0f}, translation{0.0f}, skew{0.0f};
        glm::vec4 perspective{0.0f};
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        const bool ok = glm::decompose(m, scale, rotation, translation, skew, perspective);

        // The rotation is the ONLY output of decompose that survives, so it is
        // the only one that has to be discarded when it could not decompose. Checked
        // by sabotaging both halves AT THE SAME TIME —leaving garbage in the variable and
        // removing this if—: the test goes red on the four components of the
        // quaternion. Separately it is not enough, and that says both are needed.
        if (!ok)
            rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

        if (outPos)   *outPos = glm::vec3(m[3]);
        if (outRot)   *outRot = rotation;
        if (outScale) *outScale = glm::vec3(glm::length(glm::vec3(m[0])),
                                            glm::length(glm::vec3(m[1])),
                                            glm::length(glm::vec3(m[2])));
        return ok;
    }
}
