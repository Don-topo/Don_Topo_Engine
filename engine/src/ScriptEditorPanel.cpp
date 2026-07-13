#include "DonTopo/ScriptEditorPanel.h"
#include "DonTopo/FileManager.h"
#include "DonTopo/LuaSyntaxCheck.h"
#include "DonTopo/LuaApiReference.h"
#include <imgui.h>
#include <optional>
#include <algorithm>
#include <cctype>

namespace DonTopo {

namespace {

bool isFragmentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':';
}

// Escanea GetCurrentLineText() hacia atrás desde la columna del cursor
// mientras los caracteres sean parte de un identificador/ruta con puntos
// (soporta "Entity:Get...", "Log.I..."). Devuelve el fragmento y su
// columna de inicio en la misma línea que el cursor.
struct Fragment { std::string text; int startColumn; };

Fragment extractFragment(const TextEditor& editor)
{
    TextEditor::Coordinates cursor = editor.GetCursorPosition();
    std::string line = editor.GetCurrentLineText();
    int col = std::min(cursor.mColumn, static_cast<int>(line.size()));

    int start = col;
    while (start > 0 && isFragmentChar(line[start - 1]))
        --start;

    return Fragment{ line.substr(start, col - start), start };
}

bool startsWithCaseInsensitive(const std::string& value, const std::string& prefix)
{
    if (value.size() < prefix.size())
        return false;
    return std::equal(prefix.begin(), prefix.end(), value.begin(),
        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b)); });
}

} // namespace (anónimo)

void ScriptEditorPanel::openFile(const std::filesystem::path& path)
{
    // Canonicalizamos el path antes de comparar/guardar: los distintos call sites
    // (Content Browser vs Properties/Nuevo-Script) construyen el mismo fichero real
    // desde raíces distintas, y una comparación lexical puede no coincidir (".." ,
    // separadores, mayúsculas de unidad, etc.), llevando a tabs duplicadas que
    // pisan silenciosamente los cambios de la otra al guardar.
    std::error_code ec;
    std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(path, ec);
    if (ec) canonicalPath = path;

    for (size_t i = 0; i < m_tabs.size(); ++i)
    {
        if (m_tabs[i].path == canonicalPath)
        {
            m_focusIndex = static_cast<int>(i);
            return;
        }
    }

    std::optional<std::string> content = FileManager::readText(path.string());
    if (!content)
    {
        log("Script Editor: no se pudo abrir '" + path.string() + "'");
        return;
    }

    Tab tab;
    tab.path = canonicalPath;
    tab.editor.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
    tab.editor.SetText(*content);
    m_tabs.push_back(std::move(tab));
    m_focusIndex = static_cast<int>(m_tabs.size()) - 1;
}

void ScriptEditorPanel::saveTab(Tab& tab)
{
    if (FileManager::writeText(tab.path.string(), tab.editor.GetText()))
        tab.dirty = false;
    else
        log("Script Editor: no se pudo guardar '" + tab.path.string() + "'");

    // El chequeo de sintaxis se muestra vía marker visual, nunca al Log
    // Console — sería ruido redundante con el marker.
    TextEditor::ErrorMarkers markers;
    auto err = checkLuaSyntax(tab.editor.GetText());
    if (err)
        markers[err->first] = err->second;
    tab.editor.SetErrorMarkers(markers);
}

void ScriptEditorPanel::draw()
{
    ImGui::Begin("Script Editor");

    int closeRequested = -1;

    if (ImGui::BeginTabBar("##ScriptEditorTabs", ImGuiTabBarFlags_Reorderable))
    {
        for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i)
        {
            Tab& tab = m_tabs[i];
            std::string title = tab.path.filename().string() + (tab.dirty ? " *" : "");
            ImGuiTabItemFlags flags = (m_focusIndex == i) ? ImGuiTabItemFlags_SetSelected
                                                           : ImGuiTabItemFlags_None;
            bool open = true;

            ImGui::PushID(i);
            if (ImGui::BeginTabItem(title.c_str(), &open, flags))
            {
                if (ImGui::Button("Save"))
                    saveTab(tab);

                if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
                    saveTab(tab);

                bool acKeyConsumed = false;
                if (tab.acVisible && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                {
                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))
                    {
                        tab.acSelected = (tab.acSelected + 1) % static_cast<int>(tab.acMatches.size());
                        acKeyConsumed = true;
                    }
                    else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))
                    {
                        tab.acSelected = (tab.acSelected - 1 + static_cast<int>(tab.acMatches.size())) %
                                         static_cast<int>(tab.acMatches.size());
                        acKeyConsumed = true;
                    }
                    else if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Tab, false))
                    {
                        // DeleteRange/InsertTextAt son privados en el TextEditor vendored
                        // (ver TextEditor.h línea 325) — usamos la API pública equivalente:
                        // seleccionar el rango del fragmento y Delete(). Delete() no hace
                        // no-op si start==end (a diferencia de DeleteRange), así que solo
                        // seleccionamos/borramos cuando hay algo real que borrar.
                        TextEditor::Coordinates cursor = tab.editor.GetCursorPosition();
                        if (cursor != tab.acFragmentStart)
                        {
                            tab.editor.SetSelection(tab.acFragmentStart, cursor);
                            tab.editor.Delete();
                        }
                        tab.editor.SetCursorPosition(tab.acFragmentStart);
                        tab.editor.InsertText(tab.acMatches[tab.acSelected]);
                        tab.dirty = true;
                        tab.acVisible = false;
                        acKeyConsumed = true;
                    }
                    else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                    {
                        tab.acVisible = false;
                        tab.acDismissed = true;
                        acKeyConsumed = true;
                    }
                }
                // Solo desactivamos el manejo de teclado del editor el frame en
                // que de verdad consumimos una de las teclas del popup — el
                // resto de frames con el popup abierto, escribir/mover el
                // caret con flechas sigue funcionando con normalidad.
                // (SetHandleKeyboardInputs(false) afecta al *siguiente*
                // Render(), por eso este bloque corre antes del Render() de
                // abajo: así el frame en que se consume una tecla es el mismo
                // frame en que se desactiva el manejo antes de que el editor
                // la procese.)
                tab.editor.SetHandleKeyboardInputs(!acKeyConsumed);

                tab.editor.Render("##TextEditor", ImGui::GetContentRegionAvail());
                if (tab.editor.IsTextChanged())
                    tab.dirty = true;

                ImVec2 editorOrigin = ImGui::GetItemRectMin();

                TextEditor::Coordinates currentCursor = tab.editor.GetCursorPosition();
                bool cursorMoved = !acKeyConsumed && (currentCursor != tab.acLastCursor);
                tab.acLastCursor = currentCursor;
                if (cursorMoved && tab.acVisible)
                    tab.acVisible = false;

                Fragment frag = extractFragment(tab.editor);
                bool fragmentChanged = frag.text != tab.acLastFragment;
                tab.acLastFragment = frag.text;
                if (fragmentChanged)
                    tab.acDismissed = false;

                bool forceOpen = !acKeyConsumed &&
                    ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false);

                if (!acKeyConsumed &&
                    (forceOpen || (tab.editor.IsTextChanged() && frag.text.size() >= 2 && !tab.acDismissed)))
                {
                    tab.acMatches.clear();
                    for (const auto& symbol : DonTopo::luaApiSymbols())
                        if (startsWithCaseInsensitive(symbol, frag.text))
                            tab.acMatches.push_back(symbol);

                    tab.acVisible = !tab.acMatches.empty();
                    if (tab.acVisible)
                    {
                        tab.acSelected = 0;
                        tab.acFragmentStart = TextEditor::Coordinates(
                            tab.editor.GetCursorPosition().mLine, frag.startColumn);
                    }
                }

                if (tab.acVisible)
                {
                    float charWidth = ImGui::CalcTextSize("A").x;
                    float lineHeight = ImGui::GetTextLineHeightWithSpacing();
                    ImVec2 popupPos(
                        editorOrigin.x + tab.acFragmentStart.mColumn * charWidth,
                        editorOrigin.y + tab.acFragmentStart.mLine * lineHeight + lineHeight);

                    ImGui::SetNextWindowPos(popupPos);
                    ImGui::SetNextWindowSize(ImVec2(280.0f, 0.0f));
                    ImGuiWindowFlags acFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing;

                    ImGui::Begin("##ScriptEditorAutocomplete", nullptr, acFlags);
                    int visibleCount = std::min(static_cast<int>(tab.acMatches.size()), 8);
                    for (int m = 0; m < static_cast<int>(tab.acMatches.size()); ++m)
                    {
                        bool selected = (m == tab.acSelected);
                        if (ImGui::Selectable(tab.acMatches[m].c_str(), selected))
                        {
                            TextEditor::Coordinates cursor = tab.editor.GetCursorPosition();
                            if (cursor != tab.acFragmentStart)
                            {
                                tab.editor.SetSelection(tab.acFragmentStart, cursor);
                                tab.editor.Delete();
                            }
                            tab.editor.SetCursorPosition(tab.acFragmentStart);
                            tab.editor.InsertText(tab.acMatches[m]);
                            tab.dirty = true;
                            tab.acVisible = false;
                            tab.editor.SetHandleKeyboardInputs(true);
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    (void)visibleCount;
                    ImGui::End();
                }

                ImGui::EndTabItem();
            }
            ImGui::PopID();

            if (!open)
                closeRequested = i;
        }
        ImGui::EndTabBar();
    }
    m_focusIndex = -1;

    if (closeRequested >= 0)
    {
        if (m_tabs[closeRequested].dirty)
        {
            m_closeConfirmIndex = closeRequested;
            m_openCloseConfirmPopup = true;
        }
        else
            m_tabs.erase(m_tabs.begin() + closeRequested);
    }

    if (m_openCloseConfirmPopup)
    {
        ImGui::OpenPopup("Cambios sin guardar##ScriptEditor");
        m_openCloseConfirmPopup = false;
    }

    if (ImGui::BeginPopupModal("Cambios sin guardar##ScriptEditor", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
    {
        Tab& tab = m_tabs[m_closeConfirmIndex];
        ImGui::Text("'%s' tiene cambios sin guardar.", tab.path.filename().string().c_str());

        if (ImGui::Button("Guardar"))
        {
            saveTab(tab);
            m_tabs.erase(m_tabs.begin() + m_closeConfirmIndex);
            m_closeConfirmIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Descartar"))
        {
            m_tabs.erase(m_tabs.begin() + m_closeConfirmIndex);
            m_closeConfirmIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar"))
        {
            m_closeConfirmIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

} // namespace DonTopo
