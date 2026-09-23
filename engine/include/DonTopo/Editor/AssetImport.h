#pragma once
#include <filesystem>
#include <string>

namespace DonTopo {

// Resultado de intentar copiar un fichero externo (fuera del proyecto o del
// workspace compartido del motor) a una carpeta del proyecto. No hay
// sobreescritura: un nombre ya ocupado en destino es un rechazo, no un
// reemplazo — el usuario borra o renombra a mano primero.
enum class AssetImportResult {
    Copied,
    RejectedExtension,
    RejectedNameConflict,
    RejectedCopyFailed,
};

struct AssetImportOutcome {
    AssetImportResult result = AssetImportResult::RejectedCopyFailed;
    // Valido solo si result == Copied.
    std::filesystem::path destPath;
    // Vacio salvo RejectedCopyFailed (mensaje de std::error_code).
    std::string errorMessage;
    // El fichero de origen que se intento importar. Lo rellena el llamante
    // que sabe cual era (importExternalAsset e importDroppedFilesInto), para
    // que un log de rechazo en un lote de varios ficheros pueda decir CUAL
    // fallo y no solo por que.
    std::filesystem::path sourcePath;
};

// true si ext (con el punto, cualquier combinacion de mayusc/minusc) es uno
// de los tipos que el editor sabe importar hoy: mismo set que ya usa el
// drag&drop interno del grid del Content Browser. Unica fuente de verdad —
// ContentBrowserPanel y PropertiesPanel la comparten en vez de mantener cada
// uno su propia lista.
bool isImportableExtension(const std::string& ext);

// Carpeta destino por tipo de extension, bajo projectRoot/assets/Imported/:
// Meshes, Audio, Textures o Fonts. Vacio si la extension no es importable
// (isImportableExtension(ext) == false) — el llamante decide que hacer con
// eso, esta funcion no rechaza nada por su cuenta.
std::filesystem::path importedAssetDestDir(const std::filesystem::path& projectRoot,
                                            const std::string& ext);

// Copia source a destDir/source.filename(). No sobreescribe: si el destino ya
// existe, devuelve RejectedNameConflict sin tocar disco. Crea destDir si no
// existe. Nunca lanza — std::filesystem con la sobrecarga de std::error_code
// en todas las llamadas a disco.
AssetImportOutcome importExternalAsset(const std::filesystem::path& source,
                                        const std::filesystem::path& destDir);

// Mensaje de log legible para un resultado que NO es Copied (para Copied el
// llamante ya tiene destPath y compone su propio mensaje de exito).
std::string describeImportResult(const AssetImportOutcome& outcome);

} // namespace DonTopo
