#pragma once
#include "DonTopo/Editor/Command.h"
#include "DonTopo/Editor/UndoManager.h"

#include <glm/glm.hpp>
#include <functional>

namespace DonTopo
{
    // The render setting widgets, each with its undo and its saving.
    //
    // A well-made render setting has to do FOUR things: draw the
    // widget, apply the value, register the undo command and persist to
    // project.json. Every control repeated the first three and none did the
    // fourth (H49), and these wrappers came out of that.
    //
    // They lived inside EditorUI, with their drag state among the members of the
    // class. They come out here with the Rendering panel (H58): they belong to it and not to the
    // whole editor, and this way the panel does not need to know EditorUI at all.
    //
    // They go through std::function and not a template: this way the body stays in the .cpp
    // and this header does not drag ImGui into everything that includes it. There are 41 calls
    // inside a panel that is only drawn while open, so the cost
    // of the indirection is not measurable.
    class RenderSettingControls
    {
        public:
            RenderSettingControls() = default;

            // The UndoManager and the save callback arrive through the
            // EditorContext, which is received on each draw(). They are refreshed here instead
            // of in the constructor because the DRAG STATE below
            // has to survive between frames: rebuilding the object every
            // frame would lose the drag's start value, and undo would
            // return to the second-to-last pixel instead of to where it started.
            //
            // `persist` saves the project settings: it is called when applying and
            // also from the undo command, so that undoing a setting
            // leaves project.json as it was.
            void bind(UndoManager* undo, std::function<void()> persist)
            {
                m_undo    = undo;
                m_persist = std::move(persist);
            }

            // The DRAG ones record the value from the START of the drag, not the
            // one from the frame it is released: otherwise, undoing a long drag
            // would return to the second-to-last pixel. And the previous value is read BEFORE drawing
            // because SliderFloat already jumps in the same frame as the click.
            void sliderFloat(const char* label, float lo, float hi, const char* fmt,
                             const std::function<float()>& get,
                             const std::function<void(float)>& set);
            void sliderInt(const char* label, int lo, int hi,
                           const std::function<int()>& get,
                           const std::function<void(int)>& set);
            // Returns the CURRENT value after the click, which is what callers
            // use right away for the BeginDisabled of their effect's
            // sliders.
            bool checkbox(const char* label,
                          const std::function<bool()>& get,
                          const std::function<void(bool)>& set);
            void colorEdit3(const char* label,
                            const std::function<glm::vec3()>& get,
                            const std::function<void(const glm::vec3&)>& set);

            // For controls with their own shape (the Combos, the MSAA
            // RadioButtons, the SSAA slider and the Wireframe button): the caller
            // draws and applies, this only registers. Does nothing if before ==
            // after.
            template <typename T>
            void pushUndo(const char* label, const T& before, const T& after,
                          std::function<void(const T&)> set)
            {
                if (before == after || !m_undo) return;
                // dirtiesScene = false: these settings live in project.json,
                // not in the scene. Marking it dirty would bring up the unsaved-changes
                // modal just for having moved a bloom slider.
                m_undo->push(makeRenderSettingCommand<T>(label, before, after, std::move(set),
                                                         m_persist),
                             /*dirtiesScene=*/false);
                if (m_persist) m_persist();
            }

        private:
            UndoManager*          m_undo = nullptr;
            std::function<void()> m_persist;

            // State of the drag in progress. The id distinguishes WHICH widget is being
            // dragged: without it, starting to drag a second slider without
            // releasing the first would mix the start values.
            //
            // ColorEdit3 does not propagate IsItemActivated to the group, so the
            // start is detected by the EDGE of IsItemActive and not by the event.
            unsigned int m_activeId    = 0;
            float        m_beginScalar = 0.0f;
            int          m_beginInt    = 0;
            glm::vec3    m_beginColor{0.0f};
    };
}
