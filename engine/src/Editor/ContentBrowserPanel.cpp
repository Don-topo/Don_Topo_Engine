#include "DonTopo/Editor/ContentBrowserPanel.h"
#include "DonTopo/Editor/EditorContext.h"
#include "DonTopo/Editor/AssetImport.h"
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Core/ImportSettings.h"
#include "DonTopo/Editor/ProjectContext.h"
#include "DonTopo/Editor/UndoManager.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <set>
#include "DonTopo/Renderer/EditorRenderer.h"

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
std::vector<const DonTopo::Material*> materialsOf(const DonTopo::GameObject* go)
{
    std::vector<const DonTopo::Material*> out;
    if (!go->hasMesh()) return out;
    if (const DonTopo::SkinnedMesh* sm = go->getSkinnedMesh())
        for (const DonTopo::Material& m : sm->materials)
            out.push_back(&m);
    else
        out.push_back(&go->getMesh()->material);
    return out;
}

// Mismo criterio, escribible. Pasa por editMesh: copia la malla si está
// compartida, así que solo se llama cuando algo del material VA a cambiar
// (ver tocaAlgunMaterial), nunca para recorrer por si acaso.
std::vector<DonTopo::Material*> editMaterialsOf(DonTopo::GameObject* go)
{
    std::vector<DonTopo::Material*> out;
    if (!go->hasMesh()) return out;
    if (DonTopo::SkinnedMesh* sm = go->editSkinnedMesh())
        for (DonTopo::Material& m : sm->materials)
            out.push_back(&m);
    else
        out.push_back(&go->editMesh()->material);
    return out;
}

// true si alguna ruta de algún material del objeto cumple `coincide`.
template <typename Pred>
bool tocaAlgunMaterial(const DonTopo::GameObject* go, Pred coincide)
{
    for (const DonTopo::Material* m : materialsOf(go))
        if (coincide(m->texturePath) || coincide(m->normalMapPath) || coincide(m->metallicRoughnessPath))
            return true;
    return false;
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

// Predicado único de "carpeta oculta para el Content Browser". Usado tanto por
// listVisibleSubdirs (árbol izquierdo) como por el escaneo del grid derecho
// (##AssetPane) para que ambos paneles vean el mismo conjunto de carpetas — si
// no, un doble-clic en el grid puede seleccionar una carpeta que el árbol
// nunca muestra.
//
// El criterio que manda es el CONTENIDO, no el nombre. Enumerar nombres de
// carpetas de build no escala: aquí es build-ninja, en CLion cmake-build-debug,
// en Visual Studio x64-Debug, y el siguiente que aparezca vuelve a colarse en el
// panel. Lo que de verdad distingue a un árbol de build es lo que tiene DENTRO:
// un CMakeCache.txt.
//
// Por eso NO están aquí los nombres genéricos ("build", "out"): el único caso
// que cubrirían es el de una carpeta de build recién creada y todavía sin
// configurar —vacía, o sea inofensiva en el panel— y a cambio esconderían una
// carpeta de assets del usuario que se llame así. Los dos que quedan son
// inequívocos: build-ninja es el de este repo y cmake-build-* el de CLion.
//
// El coste es un stat por subcarpeta y por frame (listVisibleSubdirs corre en
// el render loop), sobre las subcarpetas de UN directorio, no recursivo.
bool isHiddenDir(const std::filesystem::path& dir)
{
    const std::string name = dir.filename().string();
    if (name.empty() || name[0] == '.') return true;

    if (name == "build-ninja") return true;
    if (name.rfind("cmake-build-", 0) == 0) return true; // cmake-build-debug, -release...

    std::error_code ec;
    return std::filesystem::exists(dir / "CMakeCache.txt", ec) && !ec;
}

// Contencion de rect simple: borde superior/izquierdo inclusive, inferior/
// derecho exclusivo — estandar para hit-test de rects en pantalla.
bool pointInsideRect(float px, float py, float rectX, float rectY, float rectW, float rectH)
{
    return px >= rectX && px < rectX + rectW && py >= rectY && py < rectY + rectH;
}

} // namespace

namespace DonTopo {

std::string assetIconButtonLabel(const char* text, bool hasThumbnail)
{
    // "###" fija el id: ImGui hashea la etiqueta entera, y sin esto el boton
    // cambiaria de id (y perderia el click en curso) al llegar la miniatura.
    return std::string(hasThumbnail ? "" : text) + "###icon";
}

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
        if (isHiddenDir(entry.path())) continue;
        out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace {
std::string lowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

AssetKind classifyAsset(const std::string& ext, bool isDir)
{
    if (isDir) return AssetKind::Folder;
    const std::string e = lowerAscii(ext);
    if (e == ".fbx" || e == ".obj" || e == ".gltf" || e == ".glb")            return AssetKind::Model3D;
    if (e == ".mp3" || e == ".wav" || e == ".ogg" || e == ".flac")            return AssetKind::Audio;
    if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp") return AssetKind::Image;
    if (e == ".ttf" || e == ".otf" || e == ".ttc")                            return AssetKind::Font;
    if (e == ".json")                                                         return AssetKind::Scene;
    if (e == ".lua")                                                          return AssetKind::Script;
    if (e == ".spv")                                                          return AssetKind::Shader;
    if (e == ".mat")                                                          return AssetKind::Material;
    return AssetKind::Other;
}

bool assetMatchesFilter(const std::string& name, AssetKind kind,
                        const std::string& text, std::optional<AssetKind> kindFilter)
{
    if (kindFilter && *kindFilter != kind)
        return false;
    if (text.empty())
        return true;
    return lowerAscii(name).find(lowerAscii(text)) != std::string::npos;
}

std::vector<std::filesystem::path> listVisibleEntries(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> out;
    std::error_code existsEc;
    if (!std::filesystem::exists(dir, existsEc) || existsEc)
        return out;

    std::error_code iterEc;
    std::filesystem::directory_iterator it(dir, iterEc);
    // Avance manual con la sobrecarga que no lanza. exists() y la iteración son
    // dos llamadas a disco separadas (TOCTOU) — la carpeta puede desaparecer entre
    // medias (checkout, build, herramienta externa) — y esto corre desde el render
    // loop, así que un fallo debe dejar la lista vacía ese frame en vez de tirar el
    // editor.
    static const std::filesystem::directory_iterator kEnd;
    for (; !iterEc && it != kEnd; it.increment(iterEc))
    {
        const auto& entry = *it;
        std::error_code fileEc, dirEc;
        const bool isFile     = entry.is_regular_file(fileEc);
        const bool isDirEntry = entry.is_directory(dirEc);
        // Carpetas ocultas/ruido filtradas igual que el árbol izquierdo (mismo
        // predicado); los ficheros no se filtran, se listan todos.
        if (isDirEntry && isHiddenDir(entry.path()))
            continue;
        // Los .import.json son de sus assets, no assets: no se listan (y sin esto
        // el grid los clasificaria como escenas por su extension .json).
        if (isFile && isImportSidecar(entry.path()))
            continue;
        if (isFile || isDirEntry)
            out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::filesystem::path nearestExistingDir(const std::filesystem::path& dir,
                                         const std::filesystem::path& root)
{
    if (!pathUnderDir(dir, root))
        return root; // dir == root o fuera de la raíz
    std::filesystem::path cur = dir;
    while (pathUnderDir(cur, root))
    {
        std::error_code ec;
        if (std::filesystem::is_directory(cur, ec) && !ec)
            return cur;
        cur = cur.parent_path();
    }
    return root;
}

bool AssetSelection::contains(const std::filesystem::path& p) const
{
    return std::find(items.begin(), items.end(), p) != items.end();
}

namespace {
// Índice de p en visible, o -1.
int indexIn(const std::vector<std::filesystem::path>& visible, const std::filesystem::path& p)
{
    const auto it = std::find(visible.begin(), visible.end(), p);
    return it == visible.end() ? -1 : static_cast<int>(it - visible.begin());
}

// Reordena items según su posición en visible (los que ya no están, al final).
void sortByVisibleOrder(std::vector<std::filesystem::path>& items,
                        const std::vector<std::filesystem::path>& visible)
{
    std::stable_sort(items.begin(), items.end(),
        [&](const std::filesystem::path& a, const std::filesystem::path& b) {
            int ia = indexIn(visible, a), ib = indexIn(visible, b);
            if (ia < 0) ia = static_cast<int>(visible.size());
            if (ib < 0) ib = static_cast<int>(visible.size());
            return ia < ib;
        });
}
} // namespace

void applyAssetClick(AssetSelection& sel, const std::vector<std::filesystem::path>& visible,
                     const std::filesystem::path& clicked, bool ctrl, bool shift)
{
    if (shift)
    {
        const int a = sel.anchor ? indexIn(visible, *sel.anchor) : -1;
        const int b = indexIn(visible, clicked);
        if (a >= 0 && b >= 0)
        {
            sel.items.assign(visible.begin() + std::min(a, b), visible.begin() + std::max(a, b) + 1);
            return; // Shift no mueve el ancla
        }
        // Sin ancla visible: cae al clic normal de abajo.
    }
    else if (ctrl)
    {
        const auto it = std::find(sel.items.begin(), sel.items.end(), clicked);
        if (it != sel.items.end()) sel.items.erase(it);
        else                       sel.items.push_back(clicked);
        sortByVisibleOrder(sel.items, visible);
        sel.anchor = clicked;
        return;
    }
    sel.items  = { clicked };
    sel.anchor = clicked;
}

void pruneSelection(AssetSelection& sel, const std::vector<std::filesystem::path>& existing)
{
    sel.items.erase(std::remove_if(sel.items.begin(), sel.items.end(),
        [&](const std::filesystem::path& p) { return indexIn(existing, p) < 0; }),
        sel.items.end());
    if (sel.anchor && indexIn(existing, *sel.anchor) < 0)
        sel.anchor.reset();
}

MoveOutcome moveAsset(const std::filesystem::path& src, const std::filesystem::path& destDir)
{
    std::error_code ec;
    if (!std::filesystem::exists(src, ec) || ec)
        return { MoveResult::RejectedFailed, {}, "El origen no existe" };

    if (samePath(src.parent_path(), destDir))
        return { MoveResult::RejectedSameFolder, {}, "" };

    const bool isDir = std::filesystem::is_directory(src, ec);
    if (isDir && (samePath(src, destDir) || pathUnderDir(destDir, src)))
        return { MoveResult::RejectedIntoSelf, {}, "" };

    const std::filesystem::path dest = destDir / src.filename();
    if (std::filesystem::exists(dest, ec))
        return { MoveResult::RejectedNameConflict, {}, "" };
    // Un sidecar del destino (aunque sea huerfano) tambien es conflicto: mover
    // encima pisaria sus ajustes.
    if (!isDir && importSidecarConflict(src, dest))
        return { MoveResult::RejectedNameConflict, {}, "" };

    ec.clear();
    std::filesystem::rename(src, dest, ec);
    if (ec)
        return { MoveResult::RejectedFailed, {}, ec.message() };

    if (!isDir)
    {
        std::string sidecarError;
        if (!moveImportSidecar(src, dest, &sidecarError))
            return { MoveResult::Moved, dest,
                     "el asset se movio pero no su .import.json: " + sidecarError };
    }
    return { MoveResult::Moved, dest, "" };
}

RenameFileOutcome renameAssetFile(const std::filesystem::path& from, const std::filesystem::path& to,
                                  bool isDir)
{
    RenameFileOutcome out;
    // Si origen y destino son el MISMO sidecar (renombrar solo cambiando
    // mayusculas en un sistema que no las distingue) no hay conflicto: es el
    // mismo fichero.
    if (!isDir && !samePath(importSidecarPath(from), importSidecarPath(to)) &&
        importSidecarConflict(from, to))
    {
        out.error = "Ya existe un .import.json con ese nombre";
        return out;
    }
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec)
    {
        out.error = ec.message();
        return out;
    }
    out.ok = true;
    if (!isDir)
    {
        std::string sidecarError;
        if (!moveImportSidecar(from, to, &sidecarError))
            out.warning = "El asset se renombro pero no su .import.json: " + sidecarError;
    }
    return out;
}

std::error_code removeAssetPath(const std::filesystem::path& path, bool isDir)
{
    std::error_code ec;
    if (isDir) std::filesystem::remove_all(path, ec);
    else       std::filesystem::remove(path, ec);
    if (!ec && !isDir)
        removeImportSidecar(path);
    return ec;
}

std::vector<BreadcrumbSegment> breadcrumbSegments(const std::filesystem::path& root,
                                                  const std::filesystem::path& current)
{
    std::vector<BreadcrumbSegment> out;
    out.push_back({ root.filename().string(), root });
    if (!pathUnderDir(current, root))
        return out;

    // Los dos canonicalizados igual que pathUnderDir, para que el resto sea
    // una relativa limpia aunque vengan con distinto casing o separadores.
    std::error_code ecR, ecC;
    const std::filesystem::path canonRoot = std::filesystem::weakly_canonical(root, ecR);
    const std::filesystem::path canonCur  = std::filesystem::weakly_canonical(current, ecC);
    if (ecR || ecC)
        return out;

    std::filesystem::path accumulated = root;
    for (const auto& part : canonCur.lexically_relative(canonRoot))
    {
        if (part.empty() || part == ".") continue;
        accumulated /= part;
        out.push_back({ part.string(), accumulated });
    }
    return out;
}

std::string uniqueFolderName(const std::filesystem::path& dir)
{
    const std::string base = "Nueva carpeta";
    std::error_code ec;
    if (!std::filesystem::exists(dir / base, ec))
        return base;
    for (int n = 2; ; ++n)
    {
        const std::string candidate = base + " " + std::to_string(n);
        if (!std::filesystem::exists(dir / candidate, ec))
            return candidate;
    }
}

std::string uniqueMaterialName(const std::filesystem::path& dir)
{
    const std::string base = "Nuevo material";
    std::error_code ec;
    if (!std::filesystem::exists(dir / (base + ".mat"), ec))
        return base + ".mat";
    for (int n = 2; ; ++n)
    {
        const std::string candidate = base + " " + std::to_string(n) + ".mat";
        if (!std::filesystem::exists(dir / candidate, ec))
            return candidate;
    }
}

std::vector<AssetImportOutcome> importDroppedFilesInto(
    const std::vector<DroppedFile>& dropped,
    float rectX, float rectY, float rectW, float rectH,
    const std::filesystem::path& targetDir)
{
    std::vector<AssetImportOutcome> out;
    for (const DroppedFile& f : dropped)
    {
        if (!pointInsideRect(f.screenX, f.screenY, rectX, rectY, rectW, rectH))
            continue;
        if (!isImportableExtension(f.path.extension().string()))
        {
            out.push_back({ AssetImportResult::RejectedExtension, {}, "", f.path });
            continue;
        }
        out.push_back(importExternalAsset(f.path, targetDir));
    }
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

void updateSceneReferencesForRename(EditorContext& ctx, GameObject* sceneRoot,
                                     const std::filesystem::path& oldPath,
                                     const std::filesystem::path& newPath,
                                     bool isDir)
{
    (void)ctx;
    if (!sceneRoot) return;

    sceneRoot->traverse([&](GameObject* go)
    {
        auto coincide = [&](const std::string& field)
        {
            return !field.empty() && (isDir ? pathUnderDir(field, oldPath) : samePath(field, oldPath));
        };
        auto updateField = [&](std::string& field)
        {
            if (coincide(field))
                field = isDir ? replacePathPrefix(field, oldPath, newPath) : newPath.string();
        };

        if (go->hasMesh())
        {
            // Escribir en la malla la copia si está compartida: solo cuando
            // algo coincide, no para cada objeto que se recorre.
            if (coincide(go->getMesh()->sourcePath))
                updateField(go->editMesh()->sourcePath);
            // Mismo punto ciego que en count/detach: en skinned los materiales
            // viven en SkinnedMesh::materials, nunca en el Mesh::material
            // heredado. Sin esto, renombrar una textura dejaba a todos los
            // personajes con rig apuntando en memoria al nombre viejo durante
            // el resto de la sesión — en silencio, hasta el siguiente intento
            // de tocar esa ruta. Para un slot SIN override de usuario esto
            // basta también entre sesiones: el material se re-deriva del FBX
            // en cada carga. Para un slot CON override
            // (GameObject::materialOverrides, que desde la Task 6 sí se
            // serializa en mesh.materials) este fix solo corrige el Material
            // en memoria — el override guardado sigue apuntando al nombre
            // viejo hasta que el usuario vuelva a tocar ese slot desde
            // Properties.
            for (Material* mat : tocaAlgunMaterial(go, coincide) ? editMaterialsOf(go) : std::vector<Material*>{})
            {
                updateField(mat->texturePath);
                updateField(mat->normalMapPath);
                updateField(mat->metallicRoughnessPath);
            }
            // GameObject::materialOverrides es lo que de verdad sobrevive a un
            // guardado (mesh.materials, Task 6): el bucle de arriba solo
            // corrige el Material EN MEMORIA para lo que quede de sesión. Sin
            // esto, el override guardado sigue apuntando al nombre viejo, el
            // siguiente Save escribe esa ruta muerta tal cual, y al reabrir la
            // textura no carga — justo la pérdida de datos que este encargo
            // pide cerrar. Mismo updateField (mismo criterio de comparación:
            // pathUnderDir/samePath) que el resto de la función.
            //
            // baseAlbedo/baseNormal/baseOrm entran en el mismo bucle. No se
            // serializan fuera de los caminos de MEMORIA de clonar/undo (ver
            // el comentario grande junto a "baseAlbedo" en
            // Scene.cpp::nodeToJson), así que no son la pérdida de datos ENTRE
            // sesiones que cierra el bucle de arriba; lo que cierran es la
            // rendija de la MISMA sesión: si el fichero renombrado es el que
            // dio el baseline y no el override activo, un Clear() posterior
            // devolvía el nombre viejo, y el fichero con ese nombre ya no
            // existe. Renombrar es exactamente el caso en que el baseline SÍ
            // se puede corregir en vez de tirarse: el asset sigue ahí, solo ha
            // cambiado de nombre.
            for (MaterialOverride& ov : go->materialOverrides)
            {
                updateField(ov.albedo);
                updateField(ov.normal);
                updateField(ov.orm);
                updateField(ov.baseAlbedo);
                updateField(ov.baseNormal);
                updateField(ov.baseOrm);
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

TextureImportApplyResult applyTextureImportSettings(GameObject* sceneRoot,
                                                    const std::filesystem::path& asset,
                                                    const TextureImportSettings& settings,
                                                    const std::function<void(GameObject&)>& rebuild)
{
    TextureImportApplyResult r;
    if (!saveTextureImportSettings(asset, settings, &r.error))
        return r;                                    // nada reconstruido si no se pudo escribir
    r.ok = true;
    if (!sceneRoot || !rebuild) return r;

    sceneRoot->traverse([&](GameObject* go)
    {
        auto coincide = [&](const std::string& field)
        {
            return !field.empty() && samePath(field, asset);
        };
        if (!go->hasMesh() || !tocaAlgunMaterial(go, coincide)) return;
        rebuild(*go);
        ++r.refreshed;
    });
    return r;
}

MaterialAssetApplyResult applyMaterialAssetSettings(GameObject* sceneRoot, const std::filesystem::path& mat,
                                                    const MaterialAsset& asset,
                                                    const std::function<void(GameObject&)>& rebuild)
{
    MaterialAssetApplyResult r;
    if (!saveMaterialAsset(mat, asset, &r.error))
        return r;                                    // nada reconstruido si no se pudo escribir
    r.ok = true;
    if (!sceneRoot || !rebuild) return r;

    sceneRoot->traverse([&](GameObject* go)
    {
        if (!go->hasMesh()) return;
        bool usaEsteMat = false;
        for (const MaterialOverride& ov : go->materialOverrides)
            if (!ov.matAsset.empty() && samePath(ov.matAsset, mat)) { usaEsteMat = true; break; }
        if (!usaEsteMat) return;
        applyMaterialOverrides(*go);
        rebuild(*go);
        ++r.refreshed;
    });
    return r;
}

ImportSettingsKind importSettingsKindFor(const std::string& ext, bool isDir)
{
    if (isDir) return ImportSettingsKind::None;
    switch (classifyAsset(ext, false))
    {
        case AssetKind::Image: return ImportSettingsKind::Texture;
        case AssetKind::Audio: return ImportSettingsKind::Audio;
        default:               return ImportSettingsKind::None;
    }
}

AudioImportApplyResult applyAudioImportSettings(const std::filesystem::path& asset,
                                                const AudioImportSettings& settings,
                                                const std::function<void(const std::string&)>& refresh)
{
    AudioImportApplyResult r;
    if (!saveAudioImportSettings(asset, settings, &r.error))
        return r;                                    // nada refrescado si no se pudo escribir
    r.ok = true;
    if (refresh) refresh(asset.string());
    return r;
}

int countSceneReferences(GameObject* sceneRoot, const std::filesystem::path& path, bool isDir)
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

void detachSceneReferencesForDelete(EditorContext& ctx, GameObject* sceneRoot,
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
            const Mesh* mesh = go->getMesh().get();
            if (matches(mesh->sourcePath))
            {
                if (ctx.renderer)
                    ctx.renderer->removeMeshComponent(go);
            }
            else
            {
                // El path se limpia SIEMPRE que casa, aunque no haya hot-swap:
                // el fichero se va del disco, y dejar el path apuntando a él
                // haría que un registerGameObject/re-register posterior
                // intentase stbi_load sobre una ruta que ya no existe. Para un
                // slot SIN override de usuario esto basta también entre
                // sesiones: el material se re-deriva del FBX en cada carga y
                // ya no queda nada apuntando al fichero borrado. El hot-swap a
                // la textura "missing", en cambio, sólo existe para el
                // pipeline estático (replaceStaticTextureWithMissing indexa
                // m_objects), así que en skinned el path queda vacío pero la
                // GPU sigue mostrando la textura vieja hasta la siguiente
                // carga de la malla.
                const bool canSwap = ctx.renderer && go->staticRenderIndex >= 0;
                // Solo se pide la versión escribible (que copia una malla
                // compartida) si de verdad hay algo que limpiar.
                const bool hayQueLimpiar = tocaAlgunMaterial(go, matches);
                for (Material* mat : hayQueLimpiar ? editMaterialsOf(go) : std::vector<Material*>{})
                {
                    if (matches(mat->texturePath))
                    {
                        mat->texturePath.clear();
                        if (canSwap)
                            ctx.renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, EditorRenderer::TextureSlot::Diffuse);
                    }
                    if (matches(mat->normalMapPath))
                    {
                        mat->normalMapPath.clear();
                        if (canSwap)
                            ctx.renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, EditorRenderer::TextureSlot::Normal);
                    }
                    if (matches(mat->metallicRoughnessPath))
                    {
                        mat->metallicRoughnessPath.clear();
                        if (canSwap)
                            ctx.renderer->replaceStaticTextureWithMissing(go->staticRenderIndex, EditorRenderer::TextureSlot::MetallicRoughness);
                    }
                }
                // GameObject::materialOverrides es lo que se serializa
                // (mesh.materials, Task 6): sin limpiarlo aquí también, el
                // override guardado sigue apuntando al fichero que se acaba de
                // borrar, y el siguiente Save reescribe esa ruta muerta tal
                // cual — al reabrir, applyMaterialOverrides la reaplica sobre
                // el material recién derivado del FBX y la textura no carga.
                // Mismo criterio de comparación (matches) que el resto de la
                // función.
                //
                // Los base* también, y aquí NO por lo que se guarda en disco
                // —no se serializan fuera de los caminos de memoria de
                // clonar/undo, ver el comentario de "baseAlbedo" en
                // Scene.cpp::nodeToJson— sino por lo que pasa dentro de esta
                // misma sesión: un baseline apuntando al fichero recién
                // borrado se REESCRIBE en el Material en el siguiente
                // applyMaterialOverrides (la rama "sin override: vuelve al
                // baseline"), y ese siguiente puede ser el de editar OTRO slot
                // cualquiera. Es decir, deshacía el
                // replaceStaticTextureWithMissing que se acaba de hacer, sin
                // que nada lo dijera. El disparador no es "hacer Clear", que
                // es como estaba descrito, sino cualquier reaplicación.
                //
                // Se vacía en vez de corregirse (a diferencia del renombrado,
                // donde el asset sigue existiendo con otro nombre): el fichero
                // ya no está, y el flag *Taken se deja EN ALTO a propósito, que
                // es lo que hace que un Clear posterior devuelva el slot a
                // vacío en vez de resucitar la ruta muerta.
                for (MaterialOverride& ov : go->materialOverrides)
                {
                    if (matches(ov.albedo)) ov.albedo.clear();
                    if (matches(ov.normal)) ov.normal.clear();
                    if (matches(ov.orm))    ov.orm.clear();
                    if (matches(ov.baseAlbedo)) ov.baseAlbedo.clear();
                    if (matches(ov.baseNormal)) ov.baseNormal.clear();
                    if (matches(ov.baseOrm))    ov.baseOrm.clear();
                }
            }
        }
        if (go->hasAudioClip() && matches(go->getAudioClip()->getPath()))
        {
            go->setAudioClip(nullptr);
        }
    });
}

void ContentBrowserPanel::beginAssetDelete(GameObject* sceneRoot,
                                           std::vector<std::pair<std::filesystem::path, bool>> targets)
{
    m_assetDeleteTargets        = std::move(targets);
    // Con varios, es la suma por elemento: un objeto que use dos de ellos cuenta
    // dos veces. Basta para el aviso ("cuánto se va a romper"), no es un recuento exacto.
    m_assetDeleteAffectedCount  = 0;
    for (const auto& [path, isDir] : m_assetDeleteTargets)
        m_assetDeleteAffectedCount += countSceneReferences(sceneRoot, path, isDir);
    m_assetDeleteError.clear();
    m_openAssetDeletePopup      = true;
}

void ContentBrowserPanel::acceptAssetDropOnFolder(const std::filesystem::path& destDir)
{
    if (!ImGui::BeginDragDropTarget())
        return;
    // Los payloads que emite el grid: ficheros y carpetas sueltos van con tipos
    // distintos a propósito (ver el comentario de la fuente del arrastre), y una
    // selección de varios va como DT_ASSET_MULTI, un path por línea. Solo las
    // carpetas aceptan el múltiple: las zonas de Properties esperan UN asset.
    for (const char* type : { "DT_ASSET_PATH", "DT_ASSET_DIR", "DT_ASSET_MULTI" })
    {
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(type);
        if (!payload)
            continue;

        std::vector<std::filesystem::path> srcs;
        const std::string data(static_cast<const char*>(payload->Data));
        if (std::string_view(type) == "DT_ASSET_MULTI")
        {
            size_t start = 0;
            while (start < data.size())
            {
                size_t end = data.find('\n', start);
                if (end == std::string::npos) end = data.size();
                if (end > start) srcs.emplace_back(data.substr(start, end - start));
                start = end + 1;
            }
        }
        else
        {
            srcs.emplace_back(data);
        }
        // Soltar una carpeta sobre sí misma no es un error digno de log.
        srcs.erase(std::remove_if(srcs.begin(), srcs.end(),
                       [&](const std::filesystem::path& s) { return samePath(s, destDir); }),
                   srcs.end());
        if (!srcs.empty())
            m_pendingMove = PendingMove{ std::move(srcs), destDir };
    }
    ImGui::EndDragDropTarget();
}

void ContentBrowserPanel::applyPendingMove(EditorContext& ctx, GameObject* sceneRoot)
{
    if (!m_pendingMove)
        return;
    const PendingMove mv = *m_pendingMove;
    m_pendingMove.reset();

    // Mismo veto que los drops de asset de Properties: con Load Scene en vuelo
    // la escena se está reemplazando y reescribir sus referencias no tiene sentido.
    if (ctx.editingLocked)
    {
        ctx.pushLog("Carga de escena en curso: el movimiento del asset se descarta");
        return;
    }

    // El payload lo emite este mismo panel, pero se comprueba igual: origen y
    // destino tienen que estar dentro de la raíz del proyecto.
    const bool destInside = samePath(mv.destDir, m_projectRoot) || pathUnderDir(mv.destDir, m_projectRoot);

    int moved = 0;
    for (const std::filesystem::path& src : mv.srcs)
    {
        // Cada elemento del lote se resuelve por su cuenta: un rechazo no aborta
        // a los demás (mismo criterio que el import de varios ficheros).
        if (!pathUnderDir(src, m_projectRoot) || !destInside)
        {
            ctx.pushLog("Movimiento rechazado: origen o destino fuera del proyecto");
            continue;
        }

        std::error_code dirEc;
        const bool isDir = std::filesystem::is_directory(src, dirEc);
        const MoveOutcome outcome = moveAsset(src, mv.destDir);
        switch (outcome.result)
        {
        case MoveResult::Moved:
        {
            ++moved;
            if (!outcome.errorMessage.empty())
                ctx.pushLog("Aviso al mover '" + src.filename().string() + "': " + outcome.errorMessage);
            updateSceneReferencesForRename(ctx, sceneRoot, src, outcome.newPath, isDir);
            // Si la carpeta actual era la movida (o colgaba de ella) ya no existe
            // con esa ruta: seguirla a su sitio nuevo en vez de dejar el grid
            // apuntando a una carpeta que desapareció.
            const std::filesystem::path current(m_currentDir);
            if (isDir && samePath(current, src))
                m_currentDir = outcome.newPath.string();
            else if (isDir && pathUnderDir(current, src))
                m_currentDir = replacePathPrefix(m_currentDir, src, outcome.newPath);
            m_scanned = false;
            break;
        }
        case MoveResult::RejectedSameFolder:
            break; // ya estaba ahí: no hay nada que decir
        case MoveResult::RejectedIntoSelf:
            ctx.pushLog("No se puede mover una carpeta dentro de sí misma");
            break;
        case MoveResult::RejectedNameConflict:
            ctx.pushLog("Movimiento rechazado: ya existe '" + src.filename().string() +
                        "' en '" + mv.destDir.filename().string() + "'");
            break;
        case MoveResult::RejectedFailed:
            ctx.pushLog("No se pudo mover '" + src.filename().string() + "': " + outcome.errorMessage);
            break;
        }
    }

    if (moved == 1 && mv.srcs.size() == 1)
        ctx.pushLog("Asset movido: '" + mv.srcs[0].filename().string() + "' -> '" +
                    mv.destDir.filename().string() + "'");
    else if (moved == 1)
        ctx.pushLog("1 asset movido a '" + mv.destDir.filename().string() + "'");
    else if (moved > 1)
        ctx.pushLog(std::to_string(moved) + " assets movidos a '" + mv.destDir.filename().string() + "'");
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

    // Justo tras el nodo: BeginDragDropTarget opera sobre el último ítem.
    acceptAssetDropOnFolder(dir);

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
    if (!m_open)
    {
        // Drenar igualmente aunque la ventana esté cerrada: si no, la cola de
        // sandbox/main.cpp crece sin límite mientras el panel no está a la
        // vista, y al reabrirlo se importaría de golpe un lote entero contra
        // coordenadas de pantalla ya obsoletas. Sin panel visible no hay rect
        // contra el que comparar, así que estos drops se descartan en
        // silencio — el usuario no pudo soltar sobre algo que no veía.
        if (ctx.takeDroppedFiles)
            ctx.takeDroppedFiles();
        return;
    }

    // Decisión del modal (o clic directo) del frame anterior: la carga ocurre
    // aquí, fuera de cualquier popup y antes de abrir la ventana.
    if (m_scenePromptChoice != ScenePromptChoice::None)
    {
        const std::filesystem::path target = m_sceneLoadTarget;
        const ScenePromptChoice     choice = m_scenePromptChoice;
        m_sceneLoadTarget.clear();
        m_scenePromptChoice = ScenePromptChoice::None;
        if (choice == ScenePromptChoice::Save)
        {
            if (ctx.requestSaveScene) ctx.requestSaveScene(target);
        }
        else if (ctx.requestLoadScene)
        {
            ctx.requestLoadScene(target);
        }
    }

    // ImGui::Begin devuelve false para un tab dockeado sin foco (o ventana
    // colapsada): en ese caso el panel no está realmente a la vista, aunque
    // m_open siga en true, y soltar sobre lo que SÍ se ve (otro tab del mismo
    // dock, p.ej.) no debe importar aquí — visible gatea el bloque de drop.
    const bool visible = ImGui::Begin("Content Browser", &m_open);
    float totalWidth  = ImGui::GetContentRegionAvail().x;
    float totalHeight = ImGui::GetContentRegionAvail().y;
    float leftWidth   = totalWidth * 0.38f;

    // Raíz del browser: la carpeta del proyecto abierto. El árbol y la rejilla
    // solo listan hijos de m_projectRoot y no hay botón de subir, así que fijar
    // la raíz aquí es lo que impide ver —y arrastrar— assets de otro proyecto.
    // Sin proyecto (tests headless) se cae al cwd, como antes de que el concepto
    // existiera. Si la raíz cambia, el directorio actual vuelve a ella.
    if (ctx.project && ctx.project->valid() && m_projectRoot != ctx.project->root())
    {
        m_projectRoot = ctx.project->root();
        m_currentDir  = m_projectRoot.string();
    }

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

    // Drop OS-level (Explorer -> ventana): se consume una vez por frame, y
    // solo importa lo que cae dentro del rect de ESTA ventana — soltar sobre
    // otro panel dockeado no hace nada aquí (nadie más lo reclama). Si el
    // panel no está visible (tab de fondo, colapsado) igualmente se drena la
    // cola para no acumularla, pero se descarta sin más: sin rect real no hay
    // nada contra qué comparar la posición del drop.
    if (ctx.takeDroppedFiles)
    {
        if (!visible)
        {
            ctx.takeDroppedFiles();
        }
        else
        {
            const ImVec2 winPos  = ImGui::GetWindowPos();
            const ImVec2 winSize = ImGui::GetWindowSize();
            std::vector<AssetImportOutcome> outcomes = importDroppedFilesInto(
                ctx.takeDroppedFiles(), winPos.x, winPos.y, winSize.x, winSize.y, m_currentDir);
            for (const AssetImportOutcome& o : outcomes)
            {
                if (o.result == AssetImportResult::Copied)
                {
                    ctx.pushLog("Asset importado: " + o.destPath.filename().string());
                    m_scanned = false;
                }
                else
                {
                    ctx.pushLog("Import rechazado (" + o.sourcePath.filename().string() + "): " +
                                describeImportResult(o));
                }
            }
        }
    }

    // Left: árbol de carpetas
    ImGui::BeginChild("##FolderTreePane", ImVec2(leftWidth, totalHeight), false);
    drawFolderTree(m_projectRoot);
    ImGui::EndChild();
    // Reveal de un solo frame (ver comentario junto a la declaración en el
    // header): una vez pintado el árbol con la rama forzada abierta, se
    // limpia para que el usuario pueda volver a colapsarla a mano.
    m_revealCurrentDir = false;

    // Los destinos de soltar (árbol y carpetas del grid) solo anotan el
    // movimiento; se aplica aquí, con el árbol ya pintado y ANTES de escanear el
    // grid, para que este mismo frame ya vea el resultado.
    applyPendingMove(ctx, sceneRoot);

    ImGui::SameLine();

    // Right: Asset browser with type icons
    ImGui::BeginChild("##AssetPane", ImVec2(0, totalHeight), false);
    {
        // Miniaturas: el cache se crea la primera vez que el backend da atlas y hay
        // JobSystem; cambiar de carpeta abre una generacion nueva (lo pendiente de
        // la anterior ya no interesa) y cada frame empieza con beginFrame.
        if (!m_thumbs && ctx.renderer && ctx.jobs)
        {
            const uint64_t atlasId = ctx.renderer->uiThumbnailAtlasId();
            if (atlasId != 0)
            {
                m_thumbAtlasId = atlasId;
                EditorRenderer* renderer = ctx.renderer;
                JobSystem*      jobs     = ctx.jobs;
                m_thumbs = std::make_unique<ThumbnailCache>(
                    [jobs](std::function<void()> job) { return jobs->submit(std::move(job)) != 0; },
                    [renderer](const ThumbnailTile* tiles, size_t count) {
                        return renderer->uploadUiThumbnails(tiles, count);
                    });
                m_thumbDir = m_currentDir;
            }
        }
        if (m_thumbs)
        {
            if (m_thumbDir != m_currentDir)
            {
                m_thumbs->newGeneration();
                m_thumbDir = m_currentDir;
            }
            m_thumbs->beginFrame();
        }

        const double now = ImGui::GetTime();
        if (!m_scanned)
        {
            m_assets      = listVisibleEntries(m_currentDir);
            m_scanned     = true;
            m_lastPollTime = now;
        }
        else if (visible && now - m_lastPollTime >= kDirPollIntervalSeconds)
        {
            // Polling: alguien pudo crear, borrar o renombrar cosas por fuera del
            // editor. Se relee la carpeta actual y solo se sustituye la lista si
            // difiere, así que en reposo no hay cambio alguno. El árbol de la
            // izquierda no necesita esto: ya reescanea cada frame.
            m_lastPollTime = now;
            // Regenerar las miniaturas de lo que cambio de contenido con el mismo nombre.
            if (m_thumbs) m_thumbs->refreshStamps();
            const std::filesystem::path stillThere =
                nearestExistingDir(std::filesystem::path(m_currentDir), m_projectRoot);
            if (!samePath(stillThere, std::filesystem::path(m_currentDir)))
            {
                // La carpeta actual se borró por fuera: subir al ancestro que
                // quede (sin salir de la raíz) y que el árbol muestre esa rama.
                m_currentDir       = stillThere.string();
                m_scanned          = false;
                m_revealCurrentDir = true;
            }
            else
            {
                std::vector<std::filesystem::path> fresh = listVisibleEntries(m_currentDir);
                if (fresh != m_assets)
                    m_assets = std::move(fresh);
            }
        }

        constexpr float ICON_SIZE = 56.0f;
        constexpr float CELL_PAD  = 12.0f;
        float cellW = ICON_SIZE + CELL_PAD;
        float paneW = ImGui::GetContentRegionAvail().x;
        int   cols  = std::max(1, (int)(paneW / cellW));

        // Breadcrumb: raíz > ... > carpeta actual. Cada tramo salta a su carpeta;
        // el último (la actual) va deshabilitado porque pulsarlo no haría nada.
        // Se pide reveal del árbol por si el usuario había colapsado esa rama.
        {
            const std::vector<BreadcrumbSegment> segments =
                breadcrumbSegments(m_projectRoot, std::filesystem::path(m_currentDir));
            for (size_t i = 0; i < segments.size(); ++i)
            {
                if (i > 0)
                {
                    ImGui::SameLine(0.0f, 4.0f);
                    ImGui::TextDisabled(">");
                    ImGui::SameLine(0.0f, 4.0f);
                }
                const bool isLast = (i + 1 == segments.size());
                ImGui::PushID(static_cast<int>(i));
                ImGui::BeginDisabled(isLast);
                if (ImGui::SmallButton(segments[i].name.c_str()))
                {
                    m_currentDir       = segments[i].path.string();
                    m_scanned          = false;
                    m_revealCurrentDir = true;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }

        // Filtros del grid (solo la carpeta actual): texto por nombre y combo por
        // tipo. Se dibujan ANTES de Columns: dentro de una columna el campo de
        // texto se encogería al ancho de una celda.
        {
            struct KindOption { const char* label; std::optional<AssetKind> kind; };
            static const KindOption kOptions[] = {
                {"Todos", std::nullopt},           {"Carpetas", AssetKind::Folder},
                {"3D", AssetKind::Model3D},        {"Audio", AssetKind::Audio},
                {"Imagen", AssetKind::Image},      {"Fuente", AssetKind::Font},
                {"Escena", AssetKind::Scene},      {"Script", AssetKind::Script},
                {"Shader", AssetKind::Shader},     {"Material", AssetKind::Material},
                {"Otros", AssetKind::Other},
            };
            ImGui::SetNextItemWidth(std::max(80.0f, paneW - 140.0f));
            ImGui::InputTextWithHint("##AssetFilterText", "Buscar por nombre...",
                                     m_filterText, sizeof(m_filterText));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##AssetFilterKind", kOptions[m_filterKindIndex].label))
            {
                for (int i = 0; i < (int)(sizeof(kOptions) / sizeof(kOptions[0])); ++i)
                    if (ImGui::Selectable(kOptions[i].label, i == m_filterKindIndex))
                        m_filterKindIndex = i;
                ImGui::EndCombo();
            }
            m_filterKind = kOptions[m_filterKindIndex].kind;
        }

        // Lo que pasa el filtro, resuelto UNA vez: la selección por rango
        // (Shift+clic) necesita el orden visible completo antes de pintar, y así
        // el bucle de abajo no repite el stat por elemento.
        struct GridItem { std::filesystem::path path; bool isDir; std::string ext; AssetKind kind; };
        std::vector<GridItem>              items;
        std::vector<std::filesystem::path> visible;
        for (const auto& p : m_assets)
        {
            std::error_code isDirEc;
            const bool isDir = std::filesystem::is_directory(p, isDirEc);
            std::string ext = isDir ? "" : p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            const AssetKind kind = classifyAsset(ext, isDir);
            if (!assetMatchesFilter(p.filename().string(), kind, m_filterText, m_filterKind))
                continue;
            items.push_back({ p, isDir, std::move(ext), kind });
            visible.push_back(p);
        }
        // Cambiar de carpeta, filtrar o rescanear no deja selección fantasma.
        pruneSelection(m_selection, visible);

        ImGui::Columns(cols, "##AssetGrid", false);

        for (const GridItem& item : items) {
            const std::filesystem::path& path = item.path;
            const bool                   isDir = item.isDir;
            const std::string&           ext   = item.ext;
            const AssetKind              kind  = item.kind;

            ImVec4      btnColor;
            const char* label;
            switch (kind) {
            case AssetKind::Folder:  btnColor = ImVec4(0.55f, 0.55f, 0.60f, 1.0f); label = "DIR"; break;
            case AssetKind::Model3D: btnColor = ImVec4(0.15f, 0.55f, 0.85f, 1.0f); label = "3D";  break;
            case AssetKind::Audio:   btnColor = ImVec4(0.20f, 0.72f, 0.35f, 1.0f); label = "SFX"; break;
            case AssetKind::Image:   btnColor = ImVec4(0.85f, 0.72f, 0.10f, 1.0f); label = "IMG"; break;
            case AssetKind::Font:    btnColor = ImVec4(0.65f, 0.40f, 0.80f, 1.0f); label = "FNT"; break;
            case AssetKind::Scene:   btnColor = ImVec4(0.20f, 0.65f, 0.65f, 1.0f); label = "SCN"; break;
            case AssetKind::Script:  btnColor = ImVec4(0.30f, 0.40f, 0.85f, 1.0f); label = "LUA"; break;
            case AssetKind::Shader:  btnColor = ImVec4(0.80f, 0.35f, 0.10f, 1.0f); label = "SPV"; break;
            case AssetKind::Material: btnColor = ImVec4(0.75f, 0.55f, 0.20f, 1.0f); label = "MAT"; break;
            default:                 btnColor = ImVec4(0.40f, 0.40f, 0.40f, 1.0f); label = "..."; break;
            }

            ImGui::PushID(path.string().c_str());
            // Solo las imágenes que están a la vista: una carpeta de miles de
            // texturas no debe lanzar miles de decodificaciones.
            std::optional<UvRect> thumb;
            if (m_thumbs && kind == AssetKind::Image &&
                ImGui::IsRectVisible(ImVec2(ICON_SIZE, ICON_SIZE)))
                thumb = m_thumbs->request(path);
            // Seleccionado: borde claro y color más vivo. El borde se apila ANTES
            // que los colores del botón para poder sacarlo el último.
            const bool selected = m_selection.contains(path);
            if (selected)
            {
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.9f));
                btnColor = ImVec4(btnColor.x + 0.10f, btnColor.y + 0.10f, btnColor.z + 0.10f, 1.0f);
            }
            ImGui::PushStyleColor(ImGuiCol_Button, btnColor);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(btnColor.x + 0.15f, btnColor.y + 0.15f, btnColor.z + 0.15f, 1.0f));
            // Con miniatura el botón va sin etiqueta y la imagen se dibuja encima,
            // dentro del borde de selección.
            const bool clicked = ImGui::Button(assetIconButtonLabel(label, thumb.has_value()).c_str(),
                                               ImVec2(ICON_SIZE, ICON_SIZE));
            ImGui::PopStyleColor(2);
            if (selected)
            {
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
            }
            if (thumb)
            {
                const ImVec2 mn  = ImGui::GetItemRectMin();
                const ImVec2 mx  = ImGui::GetItemRectMax();
                const float  pad = 3.0f;
                ImGui::GetWindowDrawList()->AddImage(
                    (ImTextureID)m_thumbAtlasId,
                    ImVec2(mn.x + pad, mn.y + pad), ImVec2(mx.x - pad, mx.y - pad),
                    ImVec2(thumb->u0, thumb->v0), ImVec2(thumb->u1, thumb->v1));
            }
            if (clicked)
            {
                const ImGuiIO& io = ImGui::GetIO();
                applyAssetClick(m_selection, visible, path, io.KeyCtrl, io.KeyShift);
            }

            // Soltar un asset sobre una carpeta del grid lo mueve dentro. Tiene
            // que ir aquí, antes de cualquier otro widget: opera sobre el botón.
            if (isDir)
                acceptAssetDropOnFolder(path);

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

            if (!isDir && ext == ".json" && ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                if (ctx.isPlaying)
                {
                    // Misma razón que el Save/Load del toolbar: lo que hay en
                    // memoria durante Play es estado de simulación, y cargar
                    // otra escena dejaría al snapshot del Stop describiendo una
                    // escena que ya no existe.
                    if (ctx.pushLog)
                        ctx.pushLog("Para el Play Mode para cargar una escena");
                }
                else
                {
                    m_sceneLoadTarget = path;
                    // Sin cambios pendientes se carga directo (sin modal), pero
                    // igualmente en el frame siguiente: recargar la escena en
                    // mitad del bucle que recorre m_assets no.
                    if (ctx.undo && ctx.undo->isSceneDirty())
                        m_openScenePromptPopup = true;
                    else
                        m_scenePromptChoice = ScenePromptChoice::Discard;
                }
            }

            // Ficheros y CARPETAS se arrastran con payloads DISTINTOS a
            // proposito: las 14 zonas de drop que ya existen esperan un fichero
            // de una extension concreta, y con un tipo aparte ninguna acepta una
            // carpeta por accidente.
            // Arrastrar varios seleccionados (empezando por uno de ellos) sale con
            // un payload propio aunque alguno no sea "arrastrable" suelto: mover
            // no depende de qué zonas de drop sepan aceptar el tipo.
            const bool inMultiSelection = m_selection.items.size() > 1 && m_selection.contains(path);
            const bool arrastrable = isDir || isImportableExtension(ext) || inMultiSelection;
            if (arrastrable && ImGui::BeginDragDropSource())
            {
                // Arrastrar algo que no estaba seleccionado lo convierte en la
                // selección (como en el Explorador).
                if (!m_selection.contains(path))
                    applyAssetClick(m_selection, visible, path, false, false);

                if (m_selection.items.size() > 1)
                {
                    std::string joined;
                    for (const auto& p : m_selection.items)
                        joined += p.string() + "\n";
                    ImGui::SetDragDropPayload("DT_ASSET_MULTI", joined.c_str(), joined.size() + 1);
                    ImGui::Text("%zu elementos", m_selection.items.size());
                }
                else
                {
                    std::string fullPath = path.string();
                    ImGui::SetDragDropPayload(isDir ? "DT_ASSET_DIR" : "DT_ASSET_PATH",
                                              fullPath.c_str(), fullPath.size() + 1);
                    ImGui::Text("%s", fullPath.c_str());
                }
                ImGui::EndDragDropSource();
            }

            // Clic derecho sobre algo no seleccionado lo selecciona solo, como el
            // Explorador; sobre la selección, el menú actúa sobre toda ella.
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !m_selection.contains(path))
                applyAssetClick(m_selection, visible, path, false, false);
            if (ImGui::BeginPopupContextItem())
            {
                const size_t selCount = m_selection.items.size();
                // Renombrar es de uno en uno.
                if (ImGui::MenuItem("Rename", nullptr, false, selCount <= 1))
                    beginAssetRename(path, isDir);
                // Ajustes de importacion: solo de UN asset con ajustes (textura o audio).
                const ImportSettingsKind importKind = importSettingsKindFor(path.extension().string(), isDir);
                if (selCount <= 1 && importKind != ImportSettingsKind::None &&
                    ImGui::MenuItem("Import Settings..."))
                {
                    m_importTarget = path;
                    m_importKind   = importKind;
                    if (importKind == ImportSettingsKind::Audio)
                        m_importAudioEdit = loadAudioImportSettings(path);
                    else
                        m_importEdit = loadTextureImportSettings(path);
                    m_importError.clear();
                    m_openImportPopup = true;
                }
                const std::string deleteLabel =
                    selCount > 1 ? "Delete (" + std::to_string(selCount) + ")" : std::string("Delete");
                if (ImGui::MenuItem(deleteLabel.c_str()))
                {
                    std::vector<std::pair<std::filesystem::path, bool>> targets;
                    for (const auto& p : m_selection.items)
                    {
                        std::error_code tEc;
                        targets.emplace_back(p, std::filesystem::is_directory(p, tEc));
                    }
                    if (targets.empty())
                        targets.emplace_back(path, isDir);
                    beginAssetDelete(sceneRoot, std::move(targets));
                }
                ImGui::EndPopup();
            }

            std::string fname = path.filename().string();
            if (fname.size() > 11) fname = fname.substr(0, 10) + "..";
            ImGui::TextUnformatted(fname.c_str());

            ImGui::NextColumn();
            ImGui::PopID();
        }
        // Recoge lo decodificado y lo sube al atlas: se ve a partir del frame siguiente.
        if (m_thumbs) m_thumbs->pump();

        ImGui::Columns(1);

        // Clic izquierdo en el vacío del grid deselecciona. IsAnyItemHovered deja
        // fuera los widgets de arriba (breadcrumb, filtros) y los iconos.
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGui::IsAnyItemHovered())
            m_selection.clear();

        // Clic derecho en el vacío del grid (sobre un asset sale su propio menú
        // Rename/Delete): mismo patrón que el menú Create del ScenePanel. La
        // carpeta se crea ya en disco con un nombre libre y se abre el Rename,
        // como hace Unity, para que el usuario la nombre sin pasos extra.
        if (ImGui::BeginPopupContextWindow("##AssetPaneContext",
                ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
        {
            if (ImGui::BeginMenu("Create"))
            {
                if (ImGui::MenuItem("Folder"))
                {
                    const std::filesystem::path parent(m_currentDir);
                    const std::filesystem::path created = parent / uniqueFolderName(parent);
                    std::error_code mkEc;
                    std::filesystem::create_directory(created, mkEc);
                    if (mkEc)
                    {
                        ctx.pushLog("No se pudo crear la carpeta: " + mkEc.message());
                    }
                    else
                    {
                        ctx.pushLog("Carpeta creada: " + created.filename().string());
                        m_scanned = false;
                        beginAssetRename(created, /*isDir=*/true);
                    }
                }
                if (ImGui::MenuItem("Material"))
                {
                    const std::filesystem::path parent(m_currentDir);
                    const std::filesystem::path created = parent / uniqueMaterialName(parent);
                    std::string saveErr;
                    if (!saveMaterialAsset(created, MaterialAsset{}, &saveErr))
                    {
                        ctx.pushLog("No se pudo crear el material: " + saveErr);
                    }
                    else
                    {
                        ctx.pushLog("Material creado: " + created.filename().string());
                        m_scanned = false;
                        beginAssetRename(created, /*isDir=*/false);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }

        if (m_openScenePromptPopup)
        {
            ImGui::OpenPopup("Guardar cambios de escena");
            m_openScenePromptPopup = false;
        }
        if (ImGui::BeginPopupModal("Guardar cambios de escena", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("La escena actual tiene cambios sin guardar.");
            ImGui::TextUnformatted("¿Guardar los cambios de la escena actual?");
            ImGui::Text("Se cargará: %s", m_sceneLoadTarget.filename().string().c_str());
            ImGui::Separator();
            if (ImGui::Button("Guardar"))
            {
                m_scenePromptChoice = ScenePromptChoice::Save;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("No guardar"))
            {
                m_scenePromptChoice = ScenePromptChoice::Discard;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancelar"))
            {
                m_sceneLoadTarget.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

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
                        const RenameFileOutcome renamed =
                            renameAssetFile(m_assetRenameTarget, newPath, m_assetRenameIsDir);
                        if (!renamed.ok)
                        {
                            m_assetRenameError = renamed.error;
                        }
                        else
                        {
                            if (!renamed.warning.empty())
                                ctx.pushLog(renamed.warning);
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
            if (m_assetDeleteTargets.size() == 1)
                ImGui::Text("Borrar '%s'?", m_assetDeleteTargets[0].first.filename().string().c_str());
            else
                ImGui::Text("Borrar %zu elementos?", m_assetDeleteTargets.size());
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
                // Cada elemento por su cuenta: uno que falle no impide borrar los
                // demás. Los que fallan se quedan en el modal, con el primer error.
                std::vector<std::pair<std::filesystem::path, bool>> failed;
                std::string firstError;
                for (const auto& [target, isDir] : m_assetDeleteTargets)
                {
                    const std::error_code removeEc = removeAssetPath(target, isDir);

                    if (removeEc)
                    {
                        if (firstError.empty()) firstError = removeEc.message();
                        failed.emplace_back(target, isDir);
                    }
                    else
                    {
                        ctx.pushLog("Asset eliminado: " + target.string());
                        detachSceneReferencesForDelete(ctx, sceneRoot, target, isDir);
                    }
                }
                m_scanned = false;
                if (failed.empty())
                {
                    m_assetDeleteTargets.clear();
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    m_assetDeleteTargets = std::move(failed);
                    m_assetDeleteError = firstError;
                }
            }
            else if (cancel)
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Ajustes de importacion de UNA textura (menu contextual). Aplicar escribe
        // el sidecar y reconstruye los materiales que usan la textura; si no se
        // puede escribir, el modal se queda abierto con el error.
        if (m_openImportPopup)
        {
            ImGui::OpenPopup("Import Settings");
            m_openImportPopup = false;
        }
        if (ImGui::BeginPopupModal("Import Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("%s", m_importTarget.filename().string().c_str());
            ImGui::Separator();

            if (m_importKind == ImportSettingsKind::Audio)
            {
                // Edita una copia y no se aplica nada hasta "Aplicar": un
                // SliderFloat normal basta (no hay escritura por frame que diferir).
                ImGui::SliderFloat("Gain (dB)", &m_importAudioEdit.gainDb,
                                   kAudioGainMinDb, kAudioGainMaxDb, "%.1f dB");
                ImGui::Checkbox("Force mono", &m_importAudioEdit.forceMono);
                ImGui::TextDisabled("La ganancia se suma al volumen del componente.");
                ImGui::TextDisabled("Mono: solo clips 2D y desde la proxima reproduccion.");
            }
            else
            {
                int colorSpace = static_cast<int>(m_importEdit.colorSpace);
                if (ImGui::Combo("Color space", &colorSpace, "Auto (por slot)\0sRGB\0Linear\0"))
                    m_importEdit.colorSpace = static_cast<ColorSpaceOverride>(colorSpace);
                ImGui::Checkbox("Mipmaps", &m_importEdit.mipmaps);
                ImGui::TextDisabled("Auto: color base sRGB, normal y ORM lineal.");
            }

            if (!m_importError.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", m_importError.c_str());
            ImGui::Separator();

            const bool apply  = ImGui::Button("Aplicar");
            ImGui::SameLine();
            const bool cancel = ImGui::Button("Cancelar");

            if (apply && m_importKind == ImportSettingsKind::Audio)
            {
                const AudioImportApplyResult r = applyAudioImportSettings(
                    m_importTarget, m_importAudioEdit,
                    [&ctx](const std::string& p) { if (ctx.audio) ctx.audio->refreshImportSettings(p); });
                if (r.ok)
                {
                    ctx.pushLog("Import settings aplicados: " + m_importTarget.filename().string());
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    m_importError = r.error;   // el modal NO se cierra
                }
            }
            else if (apply)
            {
                // El mismo par de llamadas que MaterialTextureCommand::apply.
                const TextureImportApplyResult r = applyTextureImportSettings(
                    sceneRoot, m_importTarget, m_importEdit,
                    [&ctx](GameObject& go)
                    {
                        if (!ctx.renderer) return;
                        if (const SkinnedMesh* sm = go.getSkinnedMesh(); sm && go.skinnedRenderIndex >= 0)
                            ctx.renderer->rebuildSkinnedMesh(go.skinnedRenderIndex, *sm);
                        else if (go.staticRenderIndex >= 0)
                            ctx.renderer->rebuildStaticMesh(go.staticRenderIndex, *go.getMesh());
                    });
                if (r.ok)
                {
                    ctx.pushLog("Import settings aplicados: " + m_importTarget.filename().string() +
                                " (" + std::to_string(r.refreshed) + " objeto(s) actualizados)");
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    m_importError = r.error;   // el modal NO se cierra
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
