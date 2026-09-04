#include "DonTopo/Editor/ScriptEditorPanel.h"
#include "DonTopo/Files/FileManager.h"
#include "DonTopo/Scripting/LuaSyntaxCheck.h"
#include "DonTopo/Scripting/LuaApiReference.h"
#include <imgui.h>
#include <optional>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <regex>
#include <system_error>

namespace DonTopo {

namespace {

// Frames de calma antes de recomprobar la sintaxis. A 60 fps son ~0,2 s: lo
// justo para que no salte en mitad de una palabra a medio escribir y lo
// bastante corto para que el error aparezca solo, sin tener que guardar.
constexpr int kSyntaxDelayFrames = 12;

// Margen izquierdo del canalón de números de línea del widget vendored
// (TextEditor.cpp, mLeftMargin del constructor). Es privado y no hay getter;
// se replica aquí, igual que se replica su conversión columna<->índice.
constexpr float kTextEditorLeftMargin = 10.0f;

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

// Línea donde pintar el marker de un error de sintaxis, dado el error que
// devuelve checkLuaSyntax y cuántas líneas tiene el editor.
//
// Hay dos trampas, las dos medidas (ver los tests de scripting_tests.cpp):
//
// 1. Lua reporta los errores de "algo sin cerrar" en <eof>, que cae UNA LÍNEA
//    MÁS ALLÁ del final del documento — y el editor solo dibuja markers de
//    líneas que existen, así que ese marker no se pintaba nunca. Es el caso
//    más frecuente: es lo que pasa al borrar un 'end'.
// 2. Acotarlo sin más a la última línea tampoco sirve de mucho: el editor
//    añade un salto final, así que esa última línea suele estar VACÍA y la
//    banda roja queda al final del fichero, donde no dice nada.
//
// Por eso, cuando Lua nombra la construcción que quedó abierta ("'end'
// expected (to close 'function' at line 12)"), se marca ESA línea: es donde
// está el problema de verdad. Si no la nombra, se cae al clamp.
int markerLine(const std::pair<int, std::string>& err, int totalLines)
{
    static const std::regex openedAt(R"(to close '[^']*' at line (\d+))");
    std::smatch match;
    if (std::regex_search(err.second, match, openedAt))
    {
        const int opened = std::stoi(match[1].str());
        if (opened >= 1 && opened <= totalLines) return opened;
    }
    const int line = (err.first > totalLines) ? totalLines : err.first;
    return line < 1 ? 1 : line;
}

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

} // namespace (anónimo)

void ScriptEditorPanel::openFile(const std::filesystem::path& path)
{
    m_open = true;
    // Abrir un fichero es una petición explícita de mirarlo: además de existir,
    // la ventana tiene que ponerse delante. Se pide aquí y se consume en draw()
    // porque SetNextWindowFocus solo vale justo antes del Begin de la ventana.
    m_focusWindowRequested = true;

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
    // Punto de partida para detectar cambios ajenos: si el mtime se mueve sin
    // que hayamos guardado nosotros, el fichero lo ha tocado otro.
    std::error_code timeEc;
    tab.diskTime = std::filesystem::last_write_time(canonicalPath, timeEc);
    // Diagnóstico de entrada: un fichero que ya viene roto de disco debe
    // enseñar el error al abrirlo, no esperar al primer Ctrl+S.
    refreshDiagnostics(tab);
    m_tabs.push_back(std::move(tab));
    m_focusIndex = static_cast<int>(m_tabs.size()) - 1;
}

void ScriptEditorPanel::applyMatch(Tab& tab, const LuaApiMatch& match)
{
    // DeleteRange/InsertTextAt son privados en el TextEditor vendored (ver
    // TextEditor.h línea 325) — se usa la API pública equivalente: seleccionar
    // el rango a sustituir y Delete(). Delete() no hace no-op si start == end
    // (a diferencia de DeleteRange), así que solo se borra cuando hay algo.
    //
    // El inicio de la sustitución NO es siempre el del fragmento: una
    // sugerencia encontrada por nombre de miembro ("t:Get" -> GetTransform)
    // conserva el "t:" que el usuario escribió. Los caracteres del fragmento
    // son alfanuméricos, '_', '.' y ':' —nunca tabuladores—, así que el
    // desplazamiento en caracteres y en columnas visuales coincide.
    const TextEditor::Coordinates start(
        tab.acFragmentStart.mLine,
        tab.acFragmentStart.mColumn + static_cast<int>(match.replaceOffset));

    TextEditor::Coordinates cursor = tab.editor.GetCursorPosition();
    if (cursor != start)
    {
        tab.editor.SetSelection(start, cursor);
        tab.editor.Delete();
    }
    tab.editor.SetCursorPosition(start);
    tab.editor.InsertText(match.insert);
    tab.dirty = true;
    tab.acVisible = false;
}

void ScriptEditorPanel::refreshDiagnostics(Tab& tab)
{
    // El chequeo de sintaxis se muestra vía marker visual y barra de estado,
    // nunca al Log Console — sería ruido redundante con el marker.
    TextEditor::ErrorMarkers markers;
    auto err = checkLuaSyntax(tab.editor.GetText());
    if (err)
    {
        const int line = markerLine(*err, tab.editor.GetTotalLines());
        markers[line] = err->second;
        tab.errorLine = line;
        tab.errorMessage = err->second;
    }
    else
    {
        tab.errorLine = 0;
        tab.errorMessage.clear();
    }
    tab.editor.SetErrorMarkers(markers);
    tab.syntaxDelay = -1;
}

void ScriptEditorPanel::reloadFromDisk(Tab& tab)
{
    std::optional<std::string> content = FileManager::readText(tab.path.string());
    if (!content)
    {
        log("Script Editor: no se pudo releer '" + tab.path.string() + "'");
        return;
    }
    tab.editor.SetText(*content);
    tab.dirty = false;
    tab.externalChange = false;
    std::error_code ec;
    tab.diskTime = std::filesystem::last_write_time(tab.path, ec);
    refreshDiagnostics(tab);
}

void ScriptEditorPanel::saveTab(Tab& tab)
{
    if (FileManager::writeText(tab.path.string(), tab.editor.GetText()))
    {
        tab.dirty = false;
        // El mtime nuevo lo hemos causado nosotros: se anota para no
        // confundirlo con una edición ajena en el siguiente frame.
        std::error_code ec;
        tab.diskTime = std::filesystem::last_write_time(tab.path, ec);
        tab.externalChange = false;
    }
    else
        log("Script Editor: no se pudo guardar '" + tab.path.string() + "'");

    refreshDiagnostics(tab);
}

// Busca hacia delante o hacia atrás desde el cursor, envolviendo por el
// extremo contrario. Trabaja sobre GetTextLines() en vez de sobre GetText()
// porque el resultado hay que expresarlo en (línea, columna) y partir de
// nuevo un texto plano por saltos de línea sería recorrerlo dos veces.
bool ScriptEditorPanel::findNext(Tab& tab, bool backwards)
{
    const std::string needle(tab.findBuffer);
    if (needle.empty()) return false;

    std::vector<std::string> lines = tab.editor.GetTextLines();
    if (lines.empty()) return false;

    auto normalize = [&tab](std::string s) {
        if (!tab.findCaseSensitive)
            std::transform(s.begin(), s.end(), s.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    const std::string target = normalize(needle);

    const TextEditor::Coordinates cursor = tab.editor.GetCursorPosition();
    const int total = static_cast<int>(lines.size());
    const int startLine = std::min(std::max(cursor.mLine, 0), total - 1);

    // Recorrido de 'total' líneas empezando por la del cursor: la primera
    // vuelta arranca desde la columna del cursor y las demás desde el borde.
    for (int step = 0; step <= total; ++step)
    {
        const int lineNo = backwards
            ? ((startLine - step) % total + total) % total
            : (startLine + step) % total;
        const std::string haystack = normalize(lines[lineNo]);

        std::size_t found = std::string::npos;
        if (step == 0)
        {
            // En la línea del cursor solo vale lo que queda por delante (o por
            // detrás): si no, cada F3 devolvería la misma coincidencia.
            const int col = std::min(
                characterIndexFromColumn(tab.editor, lines[lineNo], cursor.mColumn),
                static_cast<int>(haystack.size()));
            if (backwards)
            {
                if (col > 0) found = haystack.rfind(target, static_cast<std::size_t>(col) - 1);
            }
            else
                found = haystack.find(target, static_cast<std::size_t>(col));
        }
        else
            found = backwards ? haystack.rfind(target) : haystack.find(target);

        if (found == std::string::npos) continue;

        const int beginCol = characterColumnFromIndex(tab.editor, lines[lineNo],
            static_cast<int>(found));
        const int endCol = characterColumnFromIndex(tab.editor, lines[lineNo],
            static_cast<int>(found + target.size()));
        tab.editor.SetCursorPosition(TextEditor::Coordinates(lineNo, endCol));
        tab.editor.SetSelection(TextEditor::Coordinates(lineNo, beginCol),
                                TextEditor::Coordinates(lineNo, endCol));
        tab.findStatus.clear();
        return true;
    }

    tab.findStatus = "sin coincidencias";
    return false;
}

// Barra de buscar/reemplazar y salto a línea. Devuelve true si ha consumido
// teclado este frame: mientras el foco está en uno de sus campos, el editor no
// debe procesar la misma tecla.
bool ScriptEditorPanel::drawFindBar(Tab& tab)
{
    bool consumed = false;

    if (tab.gotoOpen)
    {
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputInt("Linea", &tab.gotoLine, 0, 0,
                            ImGuiInputTextFlags_EnterReturnsTrue))
        {
            const int total = tab.editor.GetTotalLines();
            const int target = std::min(std::max(tab.gotoLine, 1), total);
            // Coordinates es 0-based y lo que el usuario escribe es 1-based.
            tab.editor.SetCursorPosition(TextEditor::Coordinates(target - 1, 0));
            tab.gotoOpen = false;
        }
        if (ImGui::IsItemActive()) consumed = true;
        ImGui::SameLine();
        if (ImGui::Button("Cerrar##goto")) tab.gotoOpen = false;
    }

    if (!tab.findOpen) return consumed;

    if (tab.findFocusRequested)
    {
        ImGui::SetKeyboardFocusHere();
        tab.findFocusRequested = false;
    }
    ImGui::SetNextItemWidth(180.0f);
    const bool submitted = ImGui::InputText("##buscar", tab.findBuffer, sizeof(tab.findBuffer),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
    const bool findFieldActive = ImGui::IsItemActive();
    if (submitted) findNext(tab, false);
    ImGui::SameLine();
    ImGui::TextUnformatted("Buscar");

    ImGui::SameLine();
    ImGui::Checkbox("Aa", &tab.findCaseSensitive);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Distinguir mayusculas y minusculas");

    ImGui::SameLine();
    if (ImGui::Button("<")) findNext(tab, true);
    ImGui::SameLine();
    if (ImGui::Button(">")) findNext(tab, false);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("##reemplazar", tab.replaceBuffer, sizeof(tab.replaceBuffer));
    const bool replaceFieldActive = ImGui::IsItemActive();
    ImGui::SameLine();
    ImGui::TextUnformatted("Reemplazar por");

    ImGui::SameLine();
    if (ImGui::Button("Reemplazar"))
    {
        // Solo se sustituye si lo seleccionado ES la coincidencia: pulsar
        // Reemplazar sin haber buscado antes buscaría y sustituiría de golpe,
        // que no es lo que nadie espera del primer clic.
        const std::string selected = tab.editor.GetSelectedText();
        const std::string needle(tab.findBuffer);
        auto sameText = [&tab](std::string a, std::string b) {
            if (!tab.findCaseSensitive)
            {
                auto lower = [](std::string& s) {
                    std::transform(s.begin(), s.end(), s.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                };
                lower(a); lower(b);
            }
            return a == b;
        };
        if (!needle.empty() && !selected.empty() && sameText(selected, needle))
        {
            tab.editor.Delete();
            tab.editor.InsertText(tab.replaceBuffer);
            tab.dirty = true;
            tab.syntaxDelay = kSyntaxDelayFrames;
        }
        findNext(tab, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Todo"))
    {
        const std::string needle(tab.findBuffer);
        if (!needle.empty())
        {
            // Sobre el texto entero de una vez: ir coincidencia a coincidencia
            // con el cursor obliga a llevar la cuenta de cuánto se ha movido
            // todo lo de detrás cada vez que la sustitución cambia de longitud.
            std::string text = tab.editor.GetText();
            const std::string replacement(tab.replaceBuffer);
            std::string result;
            int count = 0;
            std::size_t pos = 0;
            auto foldCase = [&tab](const std::string& s) {
                if (tab.findCaseSensitive) return s;
                std::string out = s;
                std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return out;
            };
            const std::string hay = foldCase(text);
            const std::string pin = foldCase(needle);
            while (true)
            {
                const std::size_t hit = hay.find(pin, pos);
                if (hit == std::string::npos) break;
                result.append(text, pos, hit - pos);
                result += replacement;
                pos = hit + pin.size();
                ++count;
            }
            if (count > 0)
            {
                result.append(text, pos, std::string::npos);
                tab.editor.SetText(result);
                tab.dirty = true;
                tab.syntaxDelay = kSyntaxDelayFrames;
            }
            tab.findStatus = count > 0 ? (std::to_string(count) + " sustituciones")
                                       : std::string("sin coincidencias");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cerrar##buscar"))
    {
        tab.findOpen = false;
        tab.findStatus.clear();
    }
    if (!tab.findStatus.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tab.findStatus.c_str());
    }

    // Escape cierra la barra, pero solo si el foco está en ella: si no,
    // robaría el Escape que descarta el popup de autocompletado.
    if ((findFieldActive || replaceFieldActive) && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
    {
        tab.findOpen = false;
        tab.findStatus.clear();
    }
    return consumed || findFieldActive || replaceFieldActive;
}

void ScriptEditorPanel::drawStatusBar(Tab& tab)
{
    const TextEditor::Coordinates cursor = tab.editor.GetCursorPosition();
    ImGui::Separator();
    // Línea y columna en 1-based, como las cuenta el propio Lua al reportar un
    // error: en 0-based el número de la barra y el del error no cuadrarían.
    ImGui::Text("Ln %d, Col %d  |  %d lineas", cursor.mLine + 1, cursor.mColumn + 1,
                tab.editor.GetTotalLines());
    ImGui::SameLine();
    if (tab.errorLine > 0)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "|  Linea %d: %s",
                           tab.errorLine, tab.errorMessage.c_str());
        // Un clic lleva al error: el marcador está en el canalón y con un
        // fichero largo puede quedar fuera de la pantalla.
        if (ImGui::IsItemClicked())
            tab.editor.SetCursorPosition(TextEditor::Coordinates(tab.errorLine - 1, 0));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clic para ir a la linea del error");
    }
    else
        ImGui::TextDisabled("|  sin errores de sintaxis");
}

void ScriptEditorPanel::draw()
{
    if (!m_open) return;
    if (m_focusWindowRequested)
    {
        ImGui::SetNextWindowFocus();
        m_focusWindowRequested = false;
    }
    ImGui::Begin("Script Editor", &m_open);

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
                if (ImGui::Button("Guardar"))
                    saveTab(tab);
                ImGui::SameLine();
                if (ImGui::Button("Buscar"))
                {
                    tab.findOpen = true;
                    tab.findFocusRequested = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Ir a linea"))
                    tab.gotoOpen = true;
                ImGui::SameLine();
                if (ImGui::Button("Recargar"))
                    reloadFromDisk(tab);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Vuelve a leer el fichero de disco y descarta los cambios sin guardar");

                const bool panelFocused =
                    ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
                if (panelFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
                    saveTab(tab);
                if (panelFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F, false))
                {
                    tab.findOpen = true;
                    tab.findFocusRequested = true;
                }
                if (panelFocused && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false))
                    tab.gotoOpen = true;
                // F3 / Shift+F3 repiten la búsqueda sin volver a la barra.
                if (panelFocused && ImGui::IsKeyPressed(ImGuiKey_F3, false))
                    findNext(tab, ImGui::GetIO().KeyShift);

                // Cambio ajeno en disco: lo detecta el mtime. Si la pestaña no
                // tiene cambios propios se recarga sola (es lo que el usuario
                // querría, y así el texto no miente sobre lo que hay en disco);
                // si los tiene, se pregunta, porque cualquiera de las dos
                // opciones pierde trabajo de alguien.
                {
                    std::error_code ec;
                    const auto now = std::filesystem::last_write_time(tab.path, ec);
                    if (!ec && now != tab.diskTime)
                    {
                        tab.diskTime = now;
                        if (tab.dirty)
                            tab.externalChange = true;
                        else
                        {
                            reloadFromDisk(tab);
                            log("Script Editor: '" + tab.path.filename().string() +
                                "' cambio en disco y se ha recargado");
                        }
                    }
                }
                if (tab.externalChange)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                        "Este fichero ha cambiado en disco y tienes cambios sin guardar.");
                    ImGui::SameLine();
                    if (ImGui::Button("Recargar y perder los mios"))
                        reloadFromDisk(tab);
                    ImGui::SameLine();
                    if (ImGui::Button("Quedarme con los mios"))
                        tab.externalChange = false;
                }

                const bool findConsumed = drawFindBar(tab);

                bool acKeyConsumed = findConsumed;
                if (!acKeyConsumed && tab.acVisible &&
                    ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
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
                        applyMatch(tab, tab.acMatches[tab.acSelected]);
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

                // El editor ocupa lo que queda menos la barra de estado, que va
                // debajo y siempre visible: sin reservarla, Render() se come
                // todo el alto y la barra queda fuera del panel.
                const float statusHeight = ImGui::GetTextLineHeightWithSpacing() +
                    ImGui::GetStyle().ItemSpacing.y;
                ImVec2 editorSize = ImGui::GetContentRegionAvail();
                editorSize.y = std::max(editorSize.y - statusHeight, statusHeight);
                tab.editor.Render("##TextEditor", editorSize);
                if (tab.editor.IsTextChanged())
                {
                    tab.dirty = true;
                    // Se rearma la cuenta atrás en cada pulsación: el análisis
                    // corre cuando el usuario para, no mientras teclea.
                    tab.syntaxDelay = kSyntaxDelayFrames;
                }
                if (tab.syntaxDelay > 0)
                    --tab.syntaxDelay;
                else if (tab.syntaxDelay == 0)
                    refreshDiagnostics(tab);   // deja syntaxDelay en -1

                ImVec2 editorOrigin = ImGui::GetItemRectMin();
                ImVec2 editorEnd    = ImGui::GetItemRectMax();

                TextEditor::Coordinates currentCursor = tab.editor.GetCursorPosition();
                bool cursorMoved = !acKeyConsumed && (currentCursor != tab.acLastCursor);
                tab.acLastCursor = currentCursor;
                if (cursorMoved && tab.acVisible)
                    tab.acVisible = false;

                Fragment frag = extractFragment(tab.editor);
                tab.acLastFragment = frag.text;
                if (tab.acDismissed && !frag.text.starts_with(tab.acDismissedFragment))
                    tab.acDismissed = false;

                // Un '.' o un ':' recién escritos abren el popup aunque el
                // fragmento no llegue al mínimo de caracteres: escribir el
                // separador es justamente el momento en que se quiere ver qué
                // hay dentro del receptor.
                const bool afterSeparator = !frag.text.empty() &&
                    (frag.text.back() == '.' || frag.text.back() == ':');
                if (!acKeyConsumed &&
                    (forceOpen || (tab.editor.IsTextChanged() && !tab.acDismissed &&
                                   (frag.text.size() >= 2 || afterSeparator))))
                {
                    // El filtro vive en LuaApiReference (Core): además del
                    // prefijo del símbolo entero, casa por nombre de MIEMBRO,
                    // que es lo que hace falta cuando el receptor es una
                    // variable local ("t:Get") y no el nombre de un tipo.
                    tab.acMatches = DonTopo::luaApiMatches(frag.text);

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
                    // El widget mide sus columnas con el ancho de '#', no el de
                    // 'A' (TextEditor.cpp:856): con una fuente proporcional los
                    // dos no coinciden y el popup se separaba del caret cuanto
                    // más a la derecha estuviera.
                    const float charWidth = ImGui::GetFont()->CalcTextSizeA(
                        ImGui::GetFontSize(), FLT_MAX, -1.0f, "#").x;
                    const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
                    // Canalón de números de línea. mTextStart y mLeftMargin son
                    // privados en el widget, así que se recalculan igual que
                    // allí (TextEditor.cpp:889-890) — mismo apaño, y por el
                    // mismo motivo, que characterIndexFromColumn de arriba.
                    // Sin esto el popup salía ~35 px a la izquierda SIEMPRE.
                    char lineNoBuf[16];
                    snprintf(lineNoBuf, sizeof(lineNoBuf), " %d ", tab.editor.GetTotalLines());
                    const float gutter = ImGui::GetFont()->CalcTextSizeA(
                        ImGui::GetFontSize(), FLT_MAX, -1.0f, lineNoBuf).x + kTextEditorLeftMargin;

                    ImVec2 popupPos(
                        editorOrigin.x + gutter + tab.acFragmentStart.mColumn * charWidth,
                        editorOrigin.y + tab.acFragmentStart.mLine * lineHeight + lineHeight);

                    // El scroll interno del editor no se puede leer desde fuera
                    // (su child es suyo y ImGui no lo expone sin imgui_internal),
                    // así que con el fichero desplazado la posición calculada se
                    // va del panel. Acotarla al rectángulo visible del editor
                    // mantiene el popup siempre a la vista y pegado al borde más
                    // cercano al caret, en vez de dibujarlo donde nadie lo ve.
                    const float popupWidth = 420.0f;
                    const float popupMaxHeight = 9.0f * lineHeight;
                    popupPos.x = std::min(std::max(popupPos.x, editorOrigin.x),
                                          std::max(editorEnd.x - popupWidth, editorOrigin.x));
                    popupPos.y = std::min(std::max(popupPos.y, editorOrigin.y),
                                          std::max(editorEnd.y - popupMaxHeight, editorOrigin.y));

                    ImGui::SetNextWindowPos(popupPos);
                    ImGui::SetNextWindowSize(ImVec2(popupWidth, 0.0f));
                    ImGuiWindowFlags acFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing;

                    ImGui::Begin("##ScriptEditorAutocomplete", nullptr, acFlags);
                    int visibleCount = std::min(static_cast<int>(tab.acMatches.size()), 8);
                    ImGui::BeginChild("##ScriptEditorAutocompleteList",
                        ImVec2(0.0f, visibleCount * ImGui::GetTextLineHeightWithSpacing()), false);
                    for (int m = 0; m < static_cast<int>(tab.acMatches.size()); ++m)
                    {
                        const LuaApiMatch& match = tab.acMatches[m];
                        bool selected = (m == tab.acSelected);
                        // El ID va por índice: dos sugerencias pueden compartir
                        // texto visible (el mismo miembro en dos tipos) y
                        // colisionarían como un solo Selectable.
                        ImGui::PushID(m);
                        if (ImGui::Selectable("##fila", selected))
                        {
                            applyMatch(tab, match);
                            tab.editor.SetHandleKeyboardInputs(true);
                        }
                        // La firma va en la misma línea, en gris: el nombre
                        // solo no dice cuántos argumentos lleva ni qué devuelve.
                        ImGui::SameLine(0.0f, 0.0f);
                        ImGui::TextUnformatted(match.symbol.c_str());
                        if (!match.signature.empty())
                        {
                            ImGui::SameLine(0.0f, 0.0f);
                            ImGui::TextDisabled("%s", match.signature.c_str());
                        }
                        ImGui::PopID();
                        if (selected)
                        {
                            ImGui::SetItemDefaultFocus();
                            // Sin esto, bajar más allá de la octava fila movía
                            // la selección fuera de la parte visible y Enter
                            // insertaba algo que no se veía.
                            ImGui::SetScrollHereY(0.5f);
                        }
                    }
                    ImGui::EndChild();
                    // Documentación de la sugerencia seleccionada, debajo de la
                    // lista: una línea, y solo la de la seleccionada — ponerla
                    // en cada fila convertiría el popup en un muro de texto.
                    if (tab.acSelected >= 0 && tab.acSelected < static_cast<int>(tab.acMatches.size()))
                    {
                        const std::string& doc = tab.acMatches[tab.acSelected].doc;
                        if (!doc.empty())
                        {
                            ImGui::Separator();
                            ImGui::PushTextWrapPos(0.0f);
                            ImGui::TextDisabled("%s", doc.c_str());
                            ImGui::PopTextWrapPos();
                        }
                    }
                    ImGui::End();
                }

                drawStatusBar(tab);

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
