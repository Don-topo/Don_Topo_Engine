#include "DonTopo/Editor/ContentBrowserPanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Renderer/Renderer.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <set>

namespace {

std::string trim(const std::string& name)
{
    size_t begin = name.find_first_not_of(" \t");
    size_t end   = name.find_last_not_of(" \t");
    return name.substr(begin, end - begin + 1);
}

// Compara dos paths de forma robusta a mayúsc/minúsc (Windows es
// case-insensitive pero std::filesystem::path::operator== no lo es) y a
// formato relativo/absoluto (weakly_canonical antes de comparar).
bool samePath(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ecA, ecB;
    std::filesystem::path ca = std::filesystem::weakly_canonical(a, ecA);
    std::filesystem::path cb = std::filesystem::weakly_canonical(b, ecB);
    std::string sa = (ecA ? a : ca).string();
    std::string sb = (ecB ? b : cb).string();
    std::transform(sa.begin(), sa.end(), sa.begin(), ::tolower);
    std::transform(sb.begin(), sb.end(), sb.begin(), ::tolower);
    return sa == sb;
}

// true si p está estrictamente dentro de dir (p == dir cuenta como false).
bool pathUnderDir(const std::filesystem::path& p, const std::filesystem::path& dir)
{
    std::error_code ecP, ecD;
    std::filesystem::path cp = std::filesystem::weakly_canonical(p, ecP);
    std::filesystem::path cd = std::filesystem::weakly_canonical(dir, ecD);
    std::string sp = (ecP ? p : cp).string();
    std::string sd = (ecD ? dir : cd).string();
    std::transform(sp.begin(), sp.end(), sp.begin(), ::tolower);
    std::transform(sd.begin(), sd.end(), sd.begin(), ::tolower);
    if (sp.size() <= sd.size() || sp.compare(0, sd.size(), sd) != 0)
        return false;
    char sep = sp[sd.size()];
    return sep == '\\' || sep == '/';
}

// Sustituye el prefijo oldDir por newDir en original. Asume que
// pathUnderDir(original, oldDir) ya dio true.
std::string replacePathPrefix(const std::string& original,
                               const std::filesystem::path& oldDir,
                               const std::filesystem::path& newDir)
{
    std::error_code ecO, ecD;
    std::filesystem::path canonOriginal = std::filesystem::weakly_canonical(std::filesystem::path(original), ecO);
    std::filesystem::path canonOldDir   = std::filesystem::weakly_canonical(oldDir, ecD);
    std::string canonOriginalStr = ecO ? original      : canonOriginal.string();
    std::string canonOldDirStr   = ecD ? oldDir.string() : canonOldDir.string();
    if (canonOriginalStr.size() <= canonOldDirStr.size())
        return newDir.string(); // defensive: pathUnderDir should already guarantee original is strictly under oldDir
    return newDir.string() + canonOriginalStr.substr(canonOldDirStr.size());
}

// Todos los materiales de un GameObject en una sola lista, sea la malla
// estática o skinned. ModelLoader::loadSkinned NUNCA puebla el Mesh::material
// heredado: reparte un Material por submalla en SkinnedMesh::materials, así
// que mirar sólo material dejaba a cualquier personaje con rig fuera del
// tracking de texturas — borrar una textura suya decía "0 objetos afectados"
// en un diálogo destructivo. Devuelve punteros al material real para que los
// callers puedan limpiar campos, no copias.
std::vector<DonTopo::Material*> materialsOf(DonTopo::GameObject* go)
{
    std::vector<DonTopo::Material*> out;
    if (!go->hasMesh()) return out;
    if (DonTopo::SkinnedMesh* sm = go->getSkinnedMesh())
        for (DonTopo::Material& m : sm->materials)
            out.push_back(&m);
    else
        out.push_back(&go->getMesh()->material);
    return out;
}

// Nombre de fichero/carpeta válido: no vacío tras trim, sin separadores de
// path ni caracteres reservados de Windows.
bool isValidFileName(const std::string& name)
{
    if (name.empty())
        return false;
    static const std::string kReserved = "\\/:*?\"<>|";
    for (char c : name)
        if (kReserved.find(c) != std::string::npos)
            return false;
    return true;
}

// Predicado único de "carpeta oculta para el Content Browser": nombre que
// empieza por '.' o está en la lista de directorios de build/ruido. Usado
// tanto por listVisibleSubdirs (árbol izquierdo) como por el escaneo del
// grid derecho (##AssetPane) para que ambos paneles vean el mismo conjunto
// de carpetas — si no, un doble-clic en el grid puede seleccionar una
// carpeta que el árbol nunca muestra.
bool isHiddenDirName(const std::string& name)
{
    static const std::set<std::string> kHiddenDirs = { "build-ninja" };
    return name.empty() || name[0] == '.' || kHiddenDirs.count(name) != 0;
}

} // namespace

namespace DonTopo {

std::vector<std::filesystem::path> listVisibleSubdirs(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) return out; // no existe, es un fichero o no hay permisos

    // Avance manual con la sobrecarga que no lanza excepciones: operator++
    // del range-based for lanza filesystem_error si la enumeración falla a
    // mitad de camino (carpeta borrada, permisos, etc.), y esta función se
    // llama cada frame desde el render loop del editor.
    static const std::filesystem::directory_iterator kEnd;
    for (; it != kEnd; it.increment(ec))
    {
        if (ec) break;
        const auto& entry = *it;
        std::error_code isDirEc;
        if (!entry.is_directory(isDirEc) || isDirEc) continue;
        std::string name = entry.path().filename().string();
        if (isHiddenDirName(name)) continue;
        out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

void ContentBrowserPanel::beginAssetRename(const std::filesystem::path& path, bool isDir)
{
    m_assetRenameTarget = path;
    m_assetRenameIsDir  = isDir;
    m_assetRenameError.clear();
    std::string prefill = isDir ? path.filename().string() : path.stem().string();
    std::strncpy(m_assetRenameBuffer, prefill.c_str(), sizeof(m_assetRenameBuffer) - 1);
    m_assetRenameBuffer[sizeof(m_assetRenameBuffer) - 1] = '\0';
    m_openAssetRenamePopup = true;
}

void ContentBrowserPanel::updateSceneReferencesForRename(EditorContext& ctx, GameObject* sceneRoot,
                                                           const std::filesystem::path& oldPath,
                                                           const std::filesystem::path& newPath,
                                                           bool isDir)
{
    (void)ctx;
    if (!sceneRoot) return;

    sceneRoot->traverse([&](GameObject* go)
    {
        auto updateField = [&](std::string& field)
        {
            if (field.empty()) return;
            bool matches = isDir ? pathUnderDir(field, oldPath) : samePath(field, oldPath);
            if (matches)
                field = isDir ? replacePathPrefix(field, oldPath, newPath) : newPath.string();
        };

        if (go->hasMesh())
        {
            updateField(go->getMesh()->sourcePath);
            // Mismo punto ciego que en count/detach: en skinned los materiales
            // viven en SkinnedMesh::materials, nunca en el Mesh::material
            // heredado. Sin esto, renombrar una textura dejaba a todos los
            // personajes con rig apuntando al nombre viejo — referencia rota
            // al recargar la escena, y en silencio.
            for (Material* mat : materialsOf(go))
            {
                updateField(mat->texturePath);
                updateField(mat->normalMapPath);
                updateField(mat->metallicRoughnessPath);
            }
        }
        if (go->hasAudioClip())
        {
            std::string audioPath = go->getAudioClip()->getPath();
            bool matches = isDir ? pathUnderDir(audioPath, oldPath) : samePath(audioPath, oldPath);
            if (matches)
            {
                std::string newAudioPath = isDir ? replacePathPrefix(audioPath, oldPath, newPath) : newPath.string();
                go->getAudioClip()->setPath(newAudioPath);
            }
        }
    });
}

int ContentBrowserPanel::countSceneReferences(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)
{
    if (!sceneRoot) return 0;

    int count = 0;
    sceneRoot->traverse([&](GameObject* go)
    {
        auto matches = [&](const std::string& field)
        {
            if (field.empty()) return false;
            return isDir ? pathUnderDir(field, path) : samePath(field, path);
        };

        bool textureMatches = false;
        for (const Material* mat : materialsOf(go))
            if (matches(mat->texturePath) || matches(mat->normalMapPath) ||
                matches(mat->metallicRoughnessPath))
                textureMatches = true;

        bool meshMatches = go->hasMesh() &&
            (matches(go->getMesh()->sourcePath) || textureMatches);
        bool audioMatches = go->hasAudioClip() && matches(go->getAudioClip()->getPath());
        if (meshMatches || audioMatches)
            ++count;
    });
    return count;
}

void ContentBrowserPanel::detachSceneReferencesForDelete(EditorContext& ctx, GameObject* sceneRoot,
                                                           const std::filesystem::path& path, bool isDir)
{
    if (!sceneRoot) return;

    sceneRoot->traverse([&](GameObject* go)
    {
        auto matches = [&](const std::string& field)
        {
            if (field.empty()) return false;
            return isDir ? pathUnderDir(field, path) : samePath(field, path);
        };

        if (go->hasMesh())
        {
            Mesh* mesh = go->getMesh().get();
            if (matches(mesh->sourcePath))
            {
                if (ctx.renderer)
                    ctx.renderer->removeMeshComponent(go);
            }
            else
            {
                // El path se limpia SIEMPRE que casa: el fichero se va del
                // disco, dejarlo apuntado en el material sólo garantiza una
                // referencia rota al volver a cargar la escena. El hot-swap a
                // la textura "missing" en cambio sólo existe para el pipeline
                // estático (replaceStaticTextureWithMissing indexa m_objects),
                // así que en skinned la GPU sigue mostrando la textura vieja
                // hasta la siguiente carga de la malla.
                const bool canSwap = ctx.renderer && go->staticRenderIndex >= 0;
                for (Material* mat : materialsOf(go))
                {
                    if (matches(mat->texturePath))
                    {
                        mat->texturePath.clear();
                        if (canSwap)
                            ctx.renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, Renderer::TextureSlot::Diffuse);
                    }
                    if (matches(mat->normalMapPath))
                    {
                        mat->normalMapPath.clear();
                        if (canSwap)
                            ctx.renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, Renderer::TextureSlot::Normal);
                    }
                    if (matches(mat->metallicRoughnessPath))
                    {
                        mat->metallicRoughnessPath.clear();
                        if (canSwap)
                            ctx.renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, Renderer::TextureSlot::MetallicRoughness);
                    }
                }
            }
        }
        if (go->hasAudioClip() && matches(go->getAudioClip()->getPath()))
        {
            go->setAudioClip(nullptr);
        }
    });
}

void ContentBrowserPanel::beginAssetDelete(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)
{
    m_assetDeleteTarget         = path;
    m_assetDeleteIsDir          = isDir;
    m_assetDeleteAffectedCount  = countSceneReferences(sceneRoot, path, isDir);
    m_assetDeleteError.clear();
    m_openAssetDeletePopup      = true;
}

void ContentBrowserPanel::drawFolderTree(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> subdirs = listVisibleSubdirs(dir);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (subdirs.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (samePath(dir, std::filesystem::path(m_currentDir)))
        flags |= ImGuiTreeNodeFlags_Selected;
    if (samePath(dir, m_projectRoot))
        flags |= ImGuiTreeNodeFlags_DefaultOpen;

    // Si la carpeta seleccionada cuelga de ésta, forzar la rama abierta para
    // que se vea (p.ej. tras doble-clic en una carpeta del grid derecho).
    // Sólo cuando m_revealCurrentDir está activo (un solo frame): si se
    // hiciera en todos los frames, el usuario nunca podría colapsar a mano
    // un ancestro de la carpeta seleccionada.
    if (m_revealCurrentDir && pathUnderDir(std::filesystem::path(m_currentDir), dir))
        ImGui::SetNextItemOpen(true);

    ImGui::PushID(dir.string().c_str());
    bool open = ImGui::TreeNodeEx("##node", flags, "%s", dir.filename().string().c_str());

    // IsItemToggledOpen: pulsar la flecha expande, pero no cambia selección.
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    {
        m_currentDir = dir.string();
        m_scanned    = false;
    }

    if (open)
    {
        for (const auto& sub : subdirs)
            drawFolderTree(sub);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void ContentBrowserPanel::draw(EditorContext& ctx, GameObject* sceneRoot)
{
    if (!m_open) return;
    ImGui::Begin("Content Browser", &m_open);
    float totalWidth  = ImGui::GetContentRegionAvail().x;
    float totalHeight = ImGui::GetContentRegionAvail().y;
    float leftWidth   = totalWidth * 0.38f;

    if (m_projectRoot.empty())
    {
        std::error_code cwdEc, canonEc;
        std::filesystem::path cwd = std::filesystem::current_path(cwdEc);
        // current_path devuelve un path vacío al fallar, y canonical("") falla
        // siempre: sin este fallback m_projectRoot acabaría vacío.
        if (cwdEc)
            cwd = ".";
        std::filesystem::path canon = std::filesystem::canonical(cwd, canonEc);
        // Si canonical o current_path fallan (permisos, cwd borrado bajo los
        // pies), no dejar m_projectRoot vacío: eso reintentaría este bloque
        // cada frame. Cae al mejor valor disponible.
        m_projectRoot = canonEc ? cwd : canon;
    }
    if (m_currentDir.empty())
        m_currentDir = m_projectRoot.string();

    // Left: árbol de carpetas
    ImGui::BeginChild("##FolderTreePane", ImVec2(leftWidth, totalHeight), false);
    drawFolderTree(m_projectRoot);
    ImGui::EndChild();
    // Reveal de un solo frame (ver comentario junto a la declaración en el
    // header): una vez pintado el árbol con la rama forzada abierta, se
    // limpia para que el usuario pueda volver a colapsarla a mano.
    m_revealCurrentDir = false;

    ImGui::SameLine();

    // Right: Asset browser with type icons
    ImGui::BeginChild("##AssetPane", ImVec2(0, totalHeight), false);
    {
        if (!m_scanned) {
            m_assets.clear();
            std::error_code existsEc;
            if (std::filesystem::exists(m_currentDir, existsEc) && !existsEc)
            {
                std::error_code iterEc;
                std::filesystem::directory_iterator it(m_currentDir, iterEc);
                // Mismo patrón que listVisibleSubdirs: avance manual con la
                // sobrecarga que no lanza. exists() y la iteración son dos
                // llamadas a disco separadas (TOCTOU) — la carpeta puede
                // desaparecer entre medias (checkout, build, herramienta
                // externa) — y este bloque corre cada frame desde el render
                // loop, así que un fallo debe dejar el grid vacío ese frame
                // en vez de tirar el editor.
                static const std::filesystem::directory_iterator kEnd;
                for (; it != kEnd; it.increment(iterEc))
                {
                    if (iterEc) break;
                    const auto& entry = *it;
                    std::error_code fileEc, dirEc;
                    bool isFile      = entry.is_regular_file(fileEc);
                    bool isDirEntry  = entry.is_directory(dirEc);
                    // Filtrar carpetas ocultas/ruido igual que el árbol
                    // izquierdo (mismo predicado); los ficheros no se
                    // filtran, se listan todos como siempre.
                    if (isDirEntry && isHiddenDirName(entry.path().filename().string()))
                        continue;
                    if (isFile || isDirEntry)
                        m_assets.push_back(entry.path());
                }
            }
            std::sort(m_assets.begin(), m_assets.end());
            m_scanned = true;
        }

        constexpr float ICON_SIZE = 56.0f;
        constexpr float CELL_PAD  = 12.0f;
        float cellW = ICON_SIZE + CELL_PAD;
        float paneW = ImGui::GetContentRegionAvail().x;
        int   cols  = std::max(1, (int)(paneW / cellW));
        ImGui::Columns(cols, "##AssetGrid", false);

        static const std::set<std::string> kDraggableExt = {".fbx", ".wav", ".mp3", ".ogg", ".flac"};

        for (auto& path : m_assets) {
            std::error_code isDirEc;
            bool isDir = std::filesystem::is_directory(path, isDirEc);
            std::string ext = isDir ? "" : path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            ImVec4      btnColor;
            const char* label;
            if (isDir) {
                btnColor = ImVec4(0.55f, 0.55f, 0.60f, 1.0f); label = "DIR";
            } else if (ext == ".fbx" || ext == ".obj" || ext == ".gltf" || ext == ".glb") {
                btnColor = ImVec4(0.15f, 0.55f, 0.85f, 1.0f); label = "3D";
            } else if (ext == ".mp3" || ext == ".wav" || ext == ".ogg" || ext == ".flac") {
                btnColor = ImVec4(0.20f, 0.72f, 0.35f, 1.0f); label = "SFX";
            } else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga") {
                btnColor = ImVec4(0.85f, 0.72f, 0.10f, 1.0f); label = "IMG";
            } else if (ext == ".spv") {
                btnColor = ImVec4(0.80f, 0.35f, 0.10f, 1.0f); label = "SPV";
            } else {
                btnColor = ImVec4(0.40f, 0.40f, 0.40f, 1.0f); label = "...";
            }

            ImGui::PushID(path.string().c_str());
            ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(btnColor.x + 0.15f, btnColor.y + 0.15f, btnColor.z + 0.15f, 1.0f));
            ImGui::Button(label, ImVec2(ICON_SIZE, ICON_SIZE));
            ImGui::PopStyleColor(2);

            if (isDir && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                m_currentDir        = path.string();
                m_scanned           = false;
                // Esta carpeta puede no estar visible aún en el árbol (fue
                // seleccionada desde el grid, no clicada en el árbol), así
                // que pedimos un reveal de un solo frame para forzar abierta
                // su rama de ancestros. Un click en el árbol no necesita
                // esto: la carpeta clicada ahí ya es visible por
                // construcción.
                m_revealCurrentDir  = true;
            }

            if (!isDir && ext == ".lua" && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                ctx.openScript(path);
            }

            if (!isDir && kDraggableExt.count(ext) && ImGui::BeginDragDropSource())
            {
                std::string fullPath = path.string();
                ImGui::SetDragDropPayload("DT_ASSET_PATH", fullPath.c_str(), fullPath.size() + 1);
                ImGui::Text("%s", fullPath.c_str());
                ImGui::EndDragDropSource();
            }

            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Rename"))
                    beginAssetRename(path, isDir);
                if (ImGui::MenuItem("Delete"))
                    beginAssetDelete(sceneRoot, path, isDir);
                ImGui::EndPopup();
            }

            std::string fname = path.filename().string();
            if (fname.size() > 11) fname = fname.substr(0, 10) + "..";
            ImGui::TextUnformatted(fname.c_str());

            ImGui::NextColumn();
            ImGui::PopID();
        }
        ImGui::Columns(1);

        if (m_openAssetRenamePopup)
        {
            ImGui::OpenPopup("Rename Asset");
            m_openAssetRenamePopup = false;
        }
        if (ImGui::BeginPopupModal("Rename Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere();

            bool enterPressed = ImGui::InputText("##assetRenameInput", m_assetRenameBuffer,
                                                  sizeof(m_assetRenameBuffer),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
            if (!m_assetRenameIsDir)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", m_assetRenameTarget.extension().string().c_str());
            }
            if (!m_assetRenameError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_assetRenameError.c_str());
            ImGui::Separator();
            bool accept = ImGui::Button("Accept") || enterPressed;
            ImGui::SameLine();
            bool cancel = ImGui::Button("Cancel");

            if (accept)
            {
                std::string newStem = trim(m_assetRenameBuffer);
                if (!isValidFileName(newStem))
                {
                    m_assetRenameError = "Nombre invalido";
                }
                else
                {
                    std::string newName = m_assetRenameIsDir
                        ? newStem
                        : (newStem + m_assetRenameTarget.extension().string());
                    std::filesystem::path newPath = m_assetRenameTarget.parent_path() / newName;
                    std::error_code existsEc;
                    if (!samePath(newPath, m_assetRenameTarget) && std::filesystem::exists(newPath, existsEc))
                    {
                        m_assetRenameError = "Ya existe un fichero/carpeta con ese nombre";
                    }
                    else
                    {
                        std::error_code renameEc;
                        std::filesystem::rename(m_assetRenameTarget, newPath, renameEc);
                        if (renameEc)
                        {
                            m_assetRenameError = renameEc.message();
                        }
                        else
                        {
                            ctx.pushLog("Asset renombrado: '" + m_assetRenameTarget.filename().string() +
                                    "' -> '" + newPath.filename().string() + "'");
                            updateSceneReferencesForRename(ctx, sceneRoot, m_assetRenameTarget, newPath, m_assetRenameIsDir);
                            m_scanned = false;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
            }
            else if (cancel)
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (m_openAssetDeletePopup)
        {
            ImGui::OpenPopup("Delete Asset");
            m_openAssetDeletePopup = false;
        }
        if (ImGui::BeginPopupModal("Delete Asset", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Borrar '%s'?", m_assetDeleteTarget.filename().string().c_str());
            if (m_assetDeleteAffectedCount > 0)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                    "%d objeto(s) lo usan y perderan la referencia.", m_assetDeleteAffectedCount);
            if (!m_assetDeleteError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_assetDeleteError.c_str());
            ImGui::Separator();
            bool confirm = ImGui::Button("Borrar");
            ImGui::SameLine();
            bool cancel = ImGui::Button("Cancelar");

            if (confirm)
            {
                std::error_code removeEc;
                if (m_assetDeleteIsDir)
                    std::filesystem::remove_all(m_assetDeleteTarget, removeEc);
                else
                    std::filesystem::remove(m_assetDeleteTarget, removeEc);

                if (removeEc)
                {
                    m_assetDeleteError = removeEc.message();
                }
                else
                {
                    ctx.pushLog("Asset eliminado: " + m_assetDeleteTarget.string());
                    detachSceneReferencesForDelete(ctx, sceneRoot, m_assetDeleteTarget, m_assetDeleteIsDir);
                    m_scanned = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            else if (cancel)
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace DonTopo
