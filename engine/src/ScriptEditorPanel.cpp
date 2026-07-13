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

// GetCurrentLineText()/GetCursorPosition().mColumn viven en espacios distintos:
// la primera devuelve los caracteres reales de la línea (un '\t' literal ocupa
// una sola posición), mientras que mColumn es una columna *visual* (un '\t'
// cuenta como hasta GetTabSize() celdas — ver TextEditor.h, doc de Coordinates).
// Indexar la línea con mColumn directamente es incorrecto en líneas con tabs
// precedentes. TextEditor::GetCharacterIndex/GetCharacterColumn hacen esta
// conversión pero son privados en el widget vendored (TextEditor.h línea
// 332-333), así que replicamos aquí el mismo algoritmo (TextEditor.cpp
// líneas 492-527) sobre el std::string público que ya tenemos.
int utf8CharLength(unsigned char c)
{
    if ((c & 0xFE) == 0xFC) return 6;
    if ((c & 0xFC) == 0xF8) return 5;
    if ((c & 0xF8) == 0xF0) return 4;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xE0) == 0xC0) return 2;
    return 1;
}

// Columna visual -> índice de carácter real (equivalente a GetCharacterIndex).
int characterIndexFromColumn(const TextEditor& editor, const std::string& line, int column)
{
    int tabSize = editor.GetTabSize();
    int c = 0;
    int i = 0;
    for (; i < static_cast<int>(line.size()) && c < column;)
    {
        if (line[i] == '\t')
            c = (c / tabSize) * tabSize + tabSize;
        else
            ++c;
        i += utf8CharLength(static_cast<unsigned char>(line[i]));
    }
    return i;
}

// Índice de carácter real -> columna visual (equivalente a GetCharacterColumn).
int characterColumnFromIndex(const TextEditor& editor, const std::string& line, int index)
{
    int tabSize = editor.GetTabSize();
    int col = 0;
    int i = 0;
    while (i < index && i < static_cast<int>(line.size()))
    {
        char c = line[i];
        i += utf8CharLength(static_cast<unsigned char>(c));
        if (c == '\t')
            col = (col / tabSize) * tabSize + tabSize;
        else
            ++col;
    }
    return col;
}

// Escanea GetCurrentLineText() hacia atrás desde la columna del cursor
// mientras los caracteres sean parte de un identificador/ruta con puntos
// (soporta "Entity:Get...", "Log.I..."). Devuelve el fragmento y su columna
// de inicio (índice de carácter real, no visual) en la misma línea que el
// cursor.
struct Fragment { std::string text; int startColumn; };

Fragment extractFragment(const TextEditor& editor)
{
    TextEditor::Coordinates cursor = editor.GetCursorPosition();
    std::string line = editor.GetCurrentLineText();
    int col = std::min(characterIndexFromColumn(editor, line, cursor.mColumn),
        static_cast<int>(line.size()));

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
            // El label del TabItem no debe cambiar de texto nunca: aunque el ID sea
            // estable (el "##" + path de abajo), ImGui pierde el foco del child de
            // dentro (el editor) en cuanto el TEXTO VISIBLE de un tab cambia entre
            // frames — confirmado bisectando (append " *" al título al pasar a dirty
            // causaba pérdida de foco del editor un frame después, con o sin ID
            // estable). Por eso el estado "sin guardar" se indica con el flag nativo
            // ImGuiTabItemFlags_UnsavedDocument (un punto junto al label) en vez de
            // tocar el texto.
            std::string title = tab.path.filename().string();
            // "##" + path: el ID del TabItem es independiente del texto visible,
            // así que reordenar tabs o rutas duplicadas en distintas carpetas no
            // colisionan.
            std::string tabLabel = title + "##" + tab.path.string();
            ImGuiTabItemFlags flags = (m_focusIndex == i) ? ImGuiTabItemFlags_SetSelected
                                                           : ImGuiTabItemFlags_None;
            if (tab.dirty)
                flags |= ImGuiTabItemFlags_UnsavedDocument;
            bool open = true;

            ImGui::PushID(i);
            if (ImGui::BeginTabItem(tabLabel.c_str(), &open, flags))
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
                        tab.acDismissedFragment = tab.acLastFragment;
                        acKeyConsumed = true;
                    }
                }
                // Ctrl+Space fuerza la apertura del popup incluso sin acVisible
                // previo. Hay que detectarlo aquí, antes del Render(), y sumarlo
                // a la desactivación del manejo de teclado del editor —
                // detectarlo después de Render() (como estaba) dejaba que
                // HandleKeyboardInputs() del editor ya hubiera procesado la
                // tecla ese mismo frame e insertado un espacio literal.
                bool forceOpen = !acKeyConsumed &&
                    ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                    ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false);

                // Solo desactivamos el manejo de teclado del editor el frame en
                // que de verdad consumimos una de las teclas del popup (o
                // forzamos su apertura) — el resto de frames con el popup
                // abierto, escribir/mover el caret con flechas sigue
                // funcionando con normalidad.
                // (SetHandleKeyboardInputs(false) afecta al *siguiente*
                // Render(), por eso este bloque corre antes del Render() de
                // abajo: así el frame en que se consume una tecla es el mismo
                // frame en que se desactiva el manejo antes de que el editor
                // la procese.)
                bool suppressEditorInput = acKeyConsumed || forceOpen;
                tab.editor.SetHandleKeyboardInputs(!suppressEditorInput);

                // TextEditor::Render() solo pone io.WantCaptureKeyboard = true
                // dentro de su propio HandleKeyboardInputs() — que nos saltamos
                // arriba a propósito. Sin esto, el Enter/Tab/flechas que acabamos
                // de consumir para el popup queda libre para el sistema de Nav de
                // ImGui, que lo usa para mover el foco de teclado a otro widget
                // (p.ej. cambia de pestaña del tab bar, o deja el editor sin foco
                // — la línea actual se pinta en gris). Reclamamos la captura
                // nosotros mismos para que Nav no toque esa misma tecla.
                if (suppressEditorInput)
                    ImGui::GetIO().WantCaptureKeyboard = true;

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
                tab.acLastFragment = frag.text;
                if (tab.acDismissed && !frag.text.starts_with(tab.acDismissedFragment))
                    tab.acDismissed = false;

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
                        // frag.startColumn es un índice de carácter real; acFragmentStart
                        // se usa como Coordinates (columna visual) en SetSelection/Delete/
                        // SetCursorPosition y en el posicionamiento del popup, así que hay
                        // que reconvertir aquí, no antes.
                        int line = tab.editor.GetCursorPosition().mLine;
                        int visualColumn = characterColumnFromIndex(
                            tab.editor, tab.editor.GetCurrentLineText(), frag.startColumn);
                        tab.acFragmentStart = TextEditor::Coordinates(line, visualColumn);
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
                    ImGui::BeginChild("##ScriptEditorAutocompleteList",
                        ImVec2(0.0f, visibleCount * ImGui::GetTextLineHeightWithSpacing()), false);
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
                    ImGui::EndChild();
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
