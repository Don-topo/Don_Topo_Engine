# Patches imgui_extra_math.inl inside imgui-node-editor's already-populated
# sources.
#
# The problem: imgui_extra_math.h defines IMGUI_DEFINE_MATH_OPERATORS before
# including <imgui.h>. With that macro on, our imgui.h (1.92.9 WIP) already
# implements "inline ImVec2 operator*(const float lhs, const ImVec2& rhs)".
# The neighboring operators in imgui_extra_math.inl are guarded with
# "#if IMGUI_VERSION_NUM < XXXXX" so they are not redefined if ImGui already has them,
# but this particular operator* is missing that guard (seen both in
# master and in imgui-node-editor's v0.9.3 tag: there is no better version
# to pin to). Result: double definition, imgui_node_editor.lib does not
# compile.
#
# The fix uses the sentinel imgui.h itself defines next to the real operator
# (IMGUI_DEFINE_MATH_OPERATORS_IMPLEMENTED, see imgui.h line ~3071):
# if it is defined, ImGui already implemented these operators, with no
# need to compare version numbers, and it corrects itself if ImGui changes.
#
# We do not use FetchContent PATCH_COMMAND because the sources are already populated
# from a previous attempt and PATCH_COMMAND is not re-run on an
# existing population. This script is invoked by hand after
# FetchContent_Populate and is idempotent: it can be called as many times as
# wanted (every re-configure) without duplicating the guard.

set(_dtNodeEditorMathInl "${imguinodeeditor_SOURCE_DIR}/imgui_extra_math.inl")

if(NOT EXISTS "${_dtNodeEditorMathInl}")
    message(FATAL_ERROR
        "PatchImGuiNodeEditor: cannot find ${_dtNodeEditorMathInl}. "
        "Did the imgui-node-editor repo layout change?"
    )
endif()

file(READ "${_dtNodeEditorMathInl}" _dtNodeEditorMathInlContents)

set(_dtGuardMarker "IMGUI_DEFINE_MATH_OPERATORS_IMPLEMENTED")
set(_dtUnguardedOperator "inline ImVec2 operator*(const float lhs, const ImVec2& rhs)\n{\n    return ImVec2(lhs * rhs.x, lhs * rhs.y);\n}")
set(_dtGuardedOperator "# ifndef IMGUI_DEFINE_MATH_OPERATORS_IMPLEMENTED\ninline ImVec2 operator*(const float lhs, const ImVec2& rhs)\n{\n    return ImVec2(lhs * rhs.x, lhs * rhs.y);\n}\n# endif")

string(FIND "${_dtNodeEditorMathInlContents}" "${_dtGuardMarker}" _dtGuardAlreadyPresent)

if(_dtGuardAlreadyPresent GREATER -1)
    # Already patched by a previous configure: no-op.
    return()
endif()

string(FIND "${_dtNodeEditorMathInlContents}" "${_dtUnguardedOperator}" _dtUnguardedOperatorPos)

if(_dtUnguardedOperatorPos EQUAL -1)
    # We did not find the exact text we expected to patch. Upstream may
    # have fixed the bug (there would be nothing left to patch: no
    # reason to break the build) or rewritten the file in a way
    # incompatible with our patch (that would be cause for alarm). We
    # cannot tell the two cases apart automatically, so we warn without
    # aborting the configure: if the second case is the real one, building
    # imgui_node_editor will fail right after with a clear duplicate-symbol
    # error, and that error will point straight here.
    message(WARNING
        "PatchImGuiNodeEditor: the unguarded operator* was not found in "
        "${_dtNodeEditorMathInl}. Upstream may have fixed it already "
        "(nothing to do) or the file changed unexpectedly "
        "(review this script). If building imgui_node_editor fails "
        "with 'operator*' redefined, this is the place to look."
    )
    return()
endif()

string(REPLACE "${_dtUnguardedOperator}" "${_dtGuardedOperator}" _dtNodeEditorMathInlContents "${_dtNodeEditorMathInlContents}")

file(WRITE "${_dtNodeEditorMathInl}" "${_dtNodeEditorMathInlContents}")

message(STATUS "PatchImGuiNodeEditor: operator*(float, ImVec2) guarded with IMGUI_DEFINE_MATH_OPERATORS_IMPLEMENTED in ${_dtNodeEditorMathInl}")
