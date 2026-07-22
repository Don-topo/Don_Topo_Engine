#include "DonTopo/Editor/GameExporter.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Scripting/ScriptComponent.h"
#include "DonTopo/Files/FileManager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <system_error>

namespace fs = std::filesystem;

namespace {

// true si p está dentro de dir (ambos ya canonicalizados y en minúsculas, es
// decir, salidos de exportPathKey). Único predicado de contención del módulo:
// el editor tenía otro (isPathWithinOrEqual) con semántica distinta y sin
// tests, y dos implementaciones del mismo predicado divergen antes o después.
bool keyUnderDir(const std::string& p, const std::string& dir)
{
    if (dir.empty() || p.size() <= dir.size()) return false;
    if (p.compare(0, dir.size(), dir) != 0)    return false;
    return p[dir.size()] == '/';
}

// isspace de <cctype> con un char con signo (p.ej. una tilde en Latin-1) es
// UB; se pasa siempre por unsigned char primero.
bool isBlankChar(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

// Todos los materiales de un GameObject, sea la malla estática o skinned.
// loadSkinned nunca puebla el Material heredado de Mesh: reparte uno por
// submalla en SkinnedMesh::materials. Mismo criterio que materialsOf() en
// ContentBrowserPanel.cpp — mirar solo `material` dejaría fuera las texturas
// de cualquier personaje con rig.
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

} // namespace

namespace DonTopo {

bool isValidExportGameName(const std::string& name, std::string& reason)
{
    // find_first_not_of(' ') solo descartaba el espacio U+0020: un nombre de
    // puros tabuladores ("\t\t\t") lo pasaba y reventaba después al crear la
    // carpeta. std::all_of + isBlankChar cubre cualquier espacio en blanco
    // real (tab, CR, LF, form feed...).
    if (name.empty() || std::all_of(name.begin(), name.end(), isBlankChar))
    {
        reason = "El nombre no puede estar vacio";
        return false;
    }
    // Cubre "." y ".." a la vez que cualquier nombre con puntos/espacios
    // finales (p.ej. "...", "Juego. "): Win32 los descarta al crear la
    // carpeta, así que el destino real deja de ser el que se le mostró al
    // usuario en el popup.
    if (name.back() == '.' || isBlankChar(name.back()))
    {
        reason = "El nombre no puede terminar en '.' ni en espacio";
        return false;
    }
    // Mismo conjunto de caracteres reservados de Windows que
    // ContentBrowserPanel.cpp::isValidFileName (kReserved ahí): el comentario
    // que decía "mismo patrón" solo cubría ':' y los separadores, así que
    // "Mi*Juego", "a?b", "x|y" o "<z>" pasaban aquí y fallaban después con un
    // "no se pudo crear" genérico en vez de este motivo concreto.
    static const std::string kReserved = "\\/:*?\"<>|";
    for (char c : name)
    {
        if (kReserved.find(c) != std::string::npos)
        {
            reason = "El nombre no puede contener ninguno de estos caracteres: \\ / : * ? \" < > |";
            return false;
        }
    }
    // filename() distinto del nombre completo == contiene separadores de
    // ruta ('/' o '\') o es una ruta absoluta; en ambos casos destDir / name
    // deja de apuntar dentro de la carpeta que el usuario eligió en el
    // diálogo. Redundante con kReserved de arriba (ambos separadores ya están
    // en el set) pero se deja como red de seguridad extra sobre operator/.
    if (fs::path(name).filename().string() != name)
    {
        reason = "El nombre no puede contener separadores de ruta";
        return false;
    }
    // Nombres de dispositivo reservados por Windows (CON, NUL, COM1..9,
    // LPT1..9): "<destino>\NUL" no crea una carpeta, resuelve al dispositivo
    // NUL. exists() sobre eso da true, así que el popup de confirmación
    // afirmaría "la carpeta ya existe y se borrará su contenido" sobre algo
    // que no es una carpeta y no tiene contenido. La regla de Windows mira el
    // nombre SIN extensión (todo lo anterior al primer '.'), sin distinguir
    // mayúsculas/minúsculas, así que "NUL.txt" también está reservado.
    static const std::array<std::string, 22> kReservedDeviceNames = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    std::string baseUpper = name.substr(0, name.find('.'));
    std::transform(baseUpper.begin(), baseUpper.end(), baseUpper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    for (const std::string& reserved : kReservedDeviceNames)
    {
        if (baseUpper == reserved)
        {
            reason = "'" + name + "' es un nombre de dispositivo reservado por Windows";
            return false;
        }
    }
    return true;
}

ExportTargetState inspectExportTarget(const fs::path& pkg)
{
    // Un pkg vacío significa que destDir y gameName vinieron vacíos: no hay
    // ninguna carpeta que inspeccionar y desde luego ninguna que borrar.
    if (pkg.empty()) return ExportTargetState::Occupied;

    std::error_code ec;
    const fs::file_status st = fs::status(pkg, ec);
    // El type() se mira ANTES que ec a propósito: la STL de MSVC deja ec
    // puesto a ERROR_FILE_NOT_FOUND (value() == 2) para el caso "no existe",
    // pese a que el propio type() ya vale not_found — al reves de lo que
    // insinua cppreference ("no se trata como error"). Mirar ec primero
    // clasificaba TODO destino ausente como Occupied y el boton Export se
    // quedaba deshabilitado para cualquier carpeta nueva: se detecto porque
    // test_inspect_export_target_states (exporter_tests.cpp) fallaba incluso
    // en el caso Missing. Un error real —permisos, unidad desconectada, path
    // malformado— no da not_found; para esos sí importa ec, y por eso la
    // comprobación de abajo sigue ahí.
    if (st.type() == fs::file_type::not_found) return ExportTargetState::Missing;
    if (ec) return ExportTargetState::Occupied;
    // Un fichero, un enlace o un dispositivo con ese nombre: no es un paquete
    // nuestro y remove_all() se lo llevaría por delante igual.
    if (!fs::is_directory(st)) return ExportTargetState::Occupied;

    fs::directory_iterator it(pkg, ec), end;
    if (ec) return ExportTargetState::Occupied;
    if (it == end) return ExportTargetState::Empty;

    // game.scene solo lo escribe writeExportPackage, y siempre: su presencia
    // en la raíz es la firma de un paquete exportado. Es la única marca que
    // distingue "carpeta que yo mismo generé y puedo regenerar" de "carpeta
    // del usuario". Si no se puede ni consultar, se asume ocupada.
    std::error_code sceneEc;
    const bool hasSceneFile = fs::is_regular_file(pkg / "game.scene", sceneEc);
    if (sceneEc) return ExportTargetState::Occupied;
    return hasSceneFile ? ExportTargetState::PriorPackage : ExportTargetState::Occupied;
}

std::string exportPathKey(const std::string& path)
{
    if (path.empty()) return {};
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(fs::path(path), ec);
    std::string s = (ec ? fs::path(path) : canon).generic_string();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::vector<ExportAsset> collectSceneAssets(
    Scene& scene,
    const fs::path& projectRoot,
    const std::map<std::string, fs::path>& scriptPaths)
{
    std::vector<ExportAsset> out;
    std::map<std::string, size_t> seen;              // key -> índice en out
    std::map<std::string, int> externalDirIndex;     // key del directorio origen -> subcarpeta
    const std::string rootKey = exportPathKey(projectRoot.string());

    // Una subcarpeta por directorio externo de origen, numerada en orden de
    // primera aparición: assets/_external/0/prop.fbx, assets/_external/1/prop.fbx.
    //
    // El esquema anterior aplanaba todo a assets/_external/<nombre> con sufijo
    // numérico ante colisión, y eso rompía en silencio la relación de
    // hermandad que ModelLoader da por supuesta: deriva la textura de un FBX
    // como dirname(fbx)/basename (ModelLoader.cpp:156). Con dos carpetas
    // externas que tengan prop.fbx + prop.png, el segundo par pasaba a ser
    // prop_1.fbx + prop_1.png y ModelLoader buscaba dirname(prop_1.fbx)/prop.png
    // — la textura del PRIMER modelo, sin error ni aviso, solo un render
    // incorrecto. Con una subcarpeta por directorio la hermandad se preserva
    // por construcción y las colisiones desaparecen: dos ficheros con el mismo
    // nombre en el mismo directorio no existen.
    auto externalPackagePath = [&](const fs::path& abs) -> std::string
    {
        const std::string dirKey = exportPathKey(abs.parent_path().string());
        auto [it, inserted] = externalDirIndex.emplace(dirKey, (int)externalDirIndex.size());
        (void)inserted;
        return "assets/_external/" + std::to_string(it->second) + "/" + abs.filename().string();
    };

    auto add = [&](const std::string& raw)
    {
        if (raw.empty()) return;
        const std::string key = exportPathKey(raw);
        if (key.empty() || seen.count(key)) return;

        std::error_code ec;
        fs::path abs = fs::weakly_canonical(fs::path(raw), ec);
        if (ec) abs = fs::path(raw);

        std::string packagePath;
        if (keyUnderDir(key, rootKey))
        {
            // Dentro del proyecto: se conserva la jerarquía tal cual. Es lo
            // que hace que las texturas se reencuentren solas en el runtime:
            // ModelLoader las deriva como dirname(fbx)/filename.
            packagePath = fs::relative(abs, projectRoot, ec).generic_string();
            if (ec || packagePath.empty()) packagePath = externalPackagePath(abs);
        }
        else
        {
            packagePath = externalPackagePath(abs);
        }

        ExportAsset a;
        a.sourcePath   = abs.string();
        a.packagePath  = packagePath;
        a.existsOnDisk = fs::exists(abs, ec) && !ec;
        seen[key] = out.size();
        out.push_back(std::move(a));
    };

    scene.traverse([&](GameObject* go)
    {
        if (go->hasMesh())
        {
            // sourcePath vacío = mesh procedural: su geometría ya viaja
            // dentro del .scene, no hay fichero que copiar.
            add(go->getMesh()->sourcePath);

            if (const SkinnedMesh* sm = go->getSkinnedMesh())
                for (const AnimationSource& src : sm->animationSources)
                    add(src.path);   // la fuente builtin repite sourcePath; add() deduplica

            for (const Material* m : materialsOf(go))
            {
                // Los embedded* no aportan path: viajan dentro del FBX.
                add(m->texturePath);
                add(m->normalMapPath);
                add(m->metallicRoughnessPath);
            }
        }

        if (go->hasAudioClip())
            add(go->getAudioClip()->getPath());

        for (const auto& s : go->getScripts())
        {
            auto it = scriptPaths.find(s->scriptName);
            if (it != scriptPaths.end())
                add(it->second.string());
        }
    });

    std::sort(out.begin(), out.end(),
              [](const ExportAsset& a, const ExportAsset& b) { return a.packagePath < b.packagePath; });
    return out;
}

namespace {

// Reescribe un campo de path si el mapa lo conoce. Devuelve 1 si tocó algo.
int rewriteField(nlohmann::json& holder, const char* field,
                 const std::map<std::string, std::string>& sourceToPackage)
{
    if (!holder.contains(field) || !holder[field].is_string()) return 0;
    const std::string current = holder[field].get<std::string>();
    if (current.empty()) return 0;
    auto it = sourceToPackage.find(DonTopo::exportPathKey(current));
    if (it == sourceToPackage.end()) return 0;
    holder[field] = it->second;
    return 1;
}

int rewriteNode(nlohmann::json& node, const std::map<std::string, std::string>& sourceToPackage)
{
    int n = 0;
    if (node.contains("mesh") && node["mesh"].is_object())
    {
        nlohmann::json& mesh = node["mesh"];
        n += rewriteField(mesh, "sourcePath", sourceToPackage);
        if (mesh.contains("animationSources") && mesh["animationSources"].is_array())
            for (nlohmann::json& src : mesh["animationSources"])
                n += rewriteField(src, "path", sourceToPackage);
    }
    if (node.contains("audioClip") && node["audioClip"].is_object())
        n += rewriteField(node["audioClip"], "path", sourceToPackage);

    if (node.contains("children") && node["children"].is_array())
        for (nlohmann::json& child : node["children"])
            n += rewriteNode(child, sourceToPackage);
    return n;
}

} // namespace

int rewriteScenePaths(nlohmann::json& sceneJson,
                      const std::map<std::string, std::string>& sourceToPackage)
{
    // Acepta tanto el documento completo de Scene::toJson() ({version, root})
    // como un nodo suelto, para que los tests puedan armar el JSON a mano.
    if (sceneJson.contains("root") && sceneJson["root"].is_object())
        return rewriteNode(sceneJson["root"], sourceToPackage);
    return rewriteNode(sceneJson, sourceToPackage);
}

ExportResult writeExportPackage(const std::vector<ExportAsset>& assets,
                                const nlohmann::json& rewrittenScene,
                                const fs::path& destDir,
                                const std::string& gameName,
                                const fs::path& projectRoot,
                                const fs::path& scriptsDir,
                                const fs::path& runtimeExe)
{
    ExportResult r;
    std::error_code ec;

    if (!fs::exists(runtimeExe, ec) || ec)
    {
        r.messages.push_back("Export cancelado: no se encuentra " + runtimeExe.string() +
                             ". Compila el target DonTopoRuntime.");
        return r;
    }

    const fs::path pkg = destDir / gameName;

    // El estado del destino se consulta AQUÍ, no solo en la UI: esta función
    // es la que borra, así que es la que tiene que responder por el borrado.
    // Antes confiaba en que el editor hubiera validado, y la lista de sitios
    // prohibidos que el editor enumeraba dejó fuera <repo>/assets — remove_all
    // se llevó el árbol de assets fuente y el export reportó éxito, porque los
    // que copia los lee del directorio del ejecutable.
    const ExportTargetState targetState = inspectExportTarget(pkg);
    if (targetState == ExportTargetState::Occupied)
    {
        r.messages.push_back("Export cancelado: '" + pkg.string() +
                             "' ya existe y tiene contenido que no es de un export anterior "
                             "(no hay ningun game.scene dentro). No se ha borrado nada: "
                             "elige otro nombre u otra carpeta destino.");
        return r;
    }

    // Borrado + recreado: si se copiara encima, el paquete arrastraría assets
    // huérfanos de un export anterior y dejaría de cumplir "solo los
    // referenciados". Solo se llega aquí con el destino Missing, Empty o
    // PriorPackage, así que lo único que puede desaparecer es un paquete que
    // este mismo código generó. La confirmación al usuario, en el caso
    // PriorPackage, la pide la UI antes de llamar aquí.
    //
    // Dos error_code separados a propósito: create_directories() sobre una
    // carpeta que ya existe NO es un error, así que si compartiera el ec del
    // remove_all() anterior, un borrado fallido a medias (p.ej. un
    // MiJuego.exe del export previo todavía en ejecución y bloqueado)
    // quedaría enmascarado en cuanto create_directories "tuviera éxito" sobre
    // esa misma carpeta a medio borrar. La función seguiría creyendo que
    // tiene un paquete limpio y sobrevivirían ficheros huérfanos.
    std::error_code removeEc;
    fs::remove_all(pkg, removeEc);
    if (removeEc)
    {
        r.messages.push_back("Export fallido: no se pudo limpiar el paquete anterior en " +
                             pkg.string() + " (" + removeEc.message() +
                             "). ¿Hay algún proceso usando ficheros de esa carpeta?");
        return r;
    }

    std::error_code createEc;
    fs::create_directories(pkg, createEc);
    if (createEc)
    {
        r.messages.push_back("Export fallido: no se pudo crear " + pkg.string());
        return r;
    }

    auto copyOne = [&](const fs::path& from, const fs::path& to) -> bool
    {
        std::error_code cec;
        fs::create_directories(to.parent_path(), cec);
        if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, cec))
        {
            r.messages.push_back("No se pudo copiar " + from.string());
            return false;
        }
        std::error_code sec;
        std::uintmax_t size = fs::file_size(to, sec);
        if (!sec) r.totalBytes += size;
        ++r.fileCount;
        return true;
    };

    bool ok = copyOne(runtimeExe, pkg / (gameName + ".exe"));

    for (const ExportAsset& a : assets)
        ok = copyOne(fs::path(a.sourcePath), pkg / fs::path(a.packagePath)) && ok;

    // Skybox: el runtime lo tiene hardcoded (initSkybox con assets/skybox/*),
    // así que va siempre aunque la escena no lo "referencie".
    //
    // Se cuentan las caras copiadas y faltar una es un ERROR, no un salto
    // silencioso: Skybox.cpp:84 lanza std::runtime_error("Skybox: failed to
    // load face") y el juego muere al arrancar. Antes el if(exists) se comía
    // el caso, el Log decía "Export completado" y el usuario se enteraba
    // cuando le pasaba el .exe a otro.
    std::vector<std::string> missingFaces;
    for (const char* face : { "px", "nx", "py", "ny", "pz", "nz" })
    {
        const fs::path from = projectRoot / "assets" / "skybox" / (std::string(face) + ".png");
        std::error_code fec;
        if (fs::exists(from, fec) && !fec)
            ok = copyOne(from, pkg / "assets" / "skybox" / from.filename()) && ok;
        else
            missingFaces.push_back(from.string());
    }
    if (!missingFaces.empty())
    {
        std::string list;
        for (const std::string& f : missingFaces) list += (list.empty() ? "" : ", ") + f;
        r.messages.push_back("Export incompleto: faltan " + std::to_string(missingFaces.size()) +
                             " de las 6 caras del skybox (" + list +
                             "); el juego exportado abortaria al cargarlo.");
        ok = false;
    }

    // shaders/*.spv a la raíz del paquete: Renderer::createPipeline los abre
    // como "shaders/<nombre>.spv" relativo al CWD.
    //
    // Cero shaders copiados también es error: sin ningún .spv el runtime muere
    // en createPipeline. El error_code del iterador se dejaba caer al suelo, y
    // una carpeta shaders/ inaccesible o vacía producía un paquete que no
    // arranca con un Log que decía "completado".
    {
        int spvCopied = 0;
        std::error_code dec;
        for (fs::directory_iterator it(projectRoot / "shaders", dec), end; !dec && it != end; it.increment(dec))
        {
            if (it->path().extension() != ".spv") continue;
            if (copyOne(it->path(), pkg / "shaders" / it->path().filename())) ++spvCopied;
            else                                                             ok = false;
        }
        if (spvCopied == 0)
        {
            r.messages.push_back("Export incompleto: no se copio ningun shader .spv desde " +
                                 (projectRoot / "shaders").string() +
                                 (dec ? " (" + dec.message() + ")" : " (carpeta vacia o sin .spv)") +
                                 "; el juego exportado moriria al crear la pipeline.");
            ok = false;
        }
    }

    // Scripts/ entera: los .lua se referencian por nombre y pueden hacer
    // require entre ellos, así que filtrar por referencias los rompería.
    if (fs::exists(scriptsDir, ec))
    {
        // Clave del paquete para no recorrer la propia salida: si pkg cae
        // dentro de scriptsDir (destino Missing bajo Scripts/, que el criterio
        // de inspectExportTarget permite porque no borra nada), este walk
        // recursivo iría creando ficheros dentro del árbol que está
        // recorriendo y podría no terminar nunca. Excluirlos aquí resuelve el
        // problema donde está —la copia no debe consumir su propia salida— en
        // vez de prohibir carpetas destino desde fuera.
        const std::string pkgKey = exportPathKey(pkg.string());
        std::error_code rec;
        for (fs::recursive_directory_iterator it(scriptsDir, rec), end; !rec && it != end; it.increment(rec))
        {
            const std::string entryKey = exportPathKey(it->path().string());
            if (entryKey == pkgKey || keyUnderDir(entryKey, pkgKey))
            {
                if (it->is_directory()) it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file()) continue;
            std::error_code relEc;
            fs::path rel = fs::relative(it->path(), scriptsDir, relEc);
            if (relEc) continue;
            ok = copyOne(it->path(), pkg / "Scripts" / rel) && ok;
        }
    }

    // fmod.dll solo es una dependencia real si el motor se compiló con FMOD
    // (DT_FMOD_ENABLED, engine/CMakeLists.txt:71-73); sin él el runtime ni
    // siquiera la enlaza y su ausencia no significa nada. Con él, faltar la
    // DLL hace que el .exe muera con STATUS_ENTRYPOINT_NOT_FOUND antes de
    // main. Aun así es aviso y no error, a diferencia del skybox y los
    // shaders: el resto del paquete es correcto y se arregla copiando un
    // fichero al lado del .exe, sin re-exportar.
#ifdef DT_FMOD_ENABLED
    if (fs::exists(projectRoot / "fmod.dll", ec) && !ec)
        ok = copyOne(projectRoot / "fmod.dll", pkg / "fmod.dll") && ok;
    else
        r.messages.push_back("Aviso: no se encontro " + (projectRoot / "fmod.dll").string() +
                             "; el motor se compilo con FMOD, asi que el juego exportado no "
                             "arrancara hasta que copies esa DLL junto al .exe.");
#endif

    if (!FileManager::writeJson((pkg / "game.scene").string(), rewrittenScene))
    {
        r.messages.push_back("No se pudo escribir game.scene");
        ok = false;
    }
    else
    {
        ++r.fileCount;
        // Mismo patrón tolerante a error que copyOne: game.scene también
        // pesa y el resumen ("N ficheros, M KB") lo estaba dejando fuera.
        std::error_code sec;
        std::uintmax_t size = fs::file_size(pkg / "game.scene", sec);
        if (!sec) r.totalBytes += size;
    }

    r.ok = ok;
    if (ok)
        r.messages.push_back("Export completado en " + pkg.string() + ": " +
                             std::to_string(r.fileCount) + " ficheros, " +
                             std::to_string(r.totalBytes / 1024) + " KB");
    return r;
}

ExportResult exportGame(Scene& scene,
                        const std::map<std::string, fs::path>& scriptPaths,
                        const fs::path& destDir,
                        const std::string& gameName,
                        const fs::path& projectRoot,
                        const fs::path& scriptsDir,
                        const fs::path& runtimeExe)
{
    ExportResult r;

    // runExport (el nombre original de esto en EditorUI) es la última función
    // antes del remove_all() destructivo dentro de writeExportPackage, y no
    // debe confiar en que la UI ya validó: el guardián de un borrado
    // irreversible no puede depender de un flag ajeno. Se revalida aquí.
    std::string nameError;
    if (!isValidExportGameName(gameName, nameError))
    {
        r.messages.push_back("Export cancelado: nombre invalido (" + nameError + ")");
        return r;
    }

    // Sin camara el juego no podria renderizar: se falla aqui, donde el
    // usuario puede arreglarlo, y no en un .exe que abre una ventana negra.
    if (!scene.findCamera())
    {
        r.messages.push_back("Export cancelado: la escena no tiene camara (Add > Camera en Properties)");
        return r;
    }

    std::error_code ec;
    if (!fs::exists(runtimeExe, ec))
    {
        r.messages.push_back("Export cancelado: falta " + runtimeExe.string() +
                             ". Compila el target DonTopoRuntime.");
        return r;
    }

    std::vector<ExportAsset> assets = collectSceneAssets(scene, projectRoot, scriptPaths);

    std::vector<std::string> missing;
    for (const ExportAsset& a : assets)
        if (!a.existsOnDisk) missing.push_back(a.sourcePath);
    if (!missing.empty())
    {
        r.messages.push_back("Export cancelado: faltan en disco " +
                             std::to_string(missing.size()) + " assets referenciados:");
        for (const std::string& m : missing)
            r.messages.push_back("  " + m);
        return r;
    }

    std::map<std::string, std::string> sourceToPackage;
    for (const ExportAsset& a : assets)
        sourceToPackage[exportPathKey(a.sourcePath)] = a.packagePath;

    nlohmann::json sceneJson = scene.toJson();
    rewriteScenePaths(sceneJson, sourceToPackage);

    return writeExportPackage(assets, sceneJson, destDir, gameName, projectRoot, scriptsDir, runtimeExe);
}

} // namespace DonTopo
