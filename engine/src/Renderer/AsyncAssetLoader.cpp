#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/ModelLoader.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Core/GameObject.h"

#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>
#include "DonTopo/Renderer/EditorRenderer.h"

namespace DonTopo
{
    namespace
    {
        // Decodifica un slot a RGBA8. Devuelve false si no hay nada que
        // decodificar o si stb falla — el fallback (checkerboard, normal plana,
        // blanco) lo sigue poniendo GpuResources en el hilo principal, que es
        // donde vive esa política hoy.
        bool decodeSlot(const std::string& path, const std::vector<uint8_t>& embedded,
                        DecodedImage::Slot slot, std::vector<DecodedImage>& out)
        {
            int w = 0, h = 0, channels = 0;
            stbi_uc* px = nullptr;

            if (!embedded.empty())
                px = stbi_load_from_memory(embedded.data(), static_cast<int>(embedded.size()),
                                           &w, &h, &channels, STBI_rgb_alpha);
            else if (!path.empty())
                px = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);

            if (!px) return false;

            DecodedImage img;
            img.slot = slot;
            img.w    = w;
            img.h    = h;
            img.pixels.assign(px, px + static_cast<size_t>(w) * h * 4);
            stbi_image_free(px);
            out.push_back(std::move(img));
            return true;
        }
    }

    JobSystem::JobId AsyncAssetLoader::requestMesh(const std::string& path, uint64_t targetId)
    {
        // El id se reserva ANTES de tocar el grupo: el primer waiter de un path
        // encola el job con ESTE id, y el grupo lo guarda para poder
        // cancelarlo después aunque su waiter original desaparezca. Reservar
        // fuera del lock es seguro — reserveId() toma el lock del JobSystem, no
        // el nuestro.
        const JobSystem::JobId id = m_jobs.reserveId();

        bool needsJob = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_pending;

            PendingGroup& group = m_groups[path];
            // El primero que pide un path arranca el job; los que llegan
            // mientras sigue en vuelo se apuntan al mismo. El coste dominante es
            // el ReadFile de Assimp, así que deduplicarlo captura casi toda la
            // ganancia aunque luego se copie el Mesh por target.
            needsJob = group.waiters.empty();
            if (needsJob)
                group.jobId = id;
            group.waiters.emplace_back(id, targetId);
        }

        if (needsJob)
        {
            // path por copia: por referencia sería dangling en cuanto el caller
            // saliera de scope, y no se vería hasta que el worker arrancase.
            m_jobs.submitWithId(id, [this, path] { runJob(path); });
        }
        return id;
    }

    void AsyncAssetLoader::cancel(JobSystem::JobId id)
    {
        // cancel() sólo recibe un id, sin path: hay que localizar su waiter
        // recorriendo los grupos bajo el lock. Los grupos son pocos (el loader
        // se drena por frame) y el único caller de cancel(id) es el test —
        // producción cancela en bloque con cancelAllPending() — así que un
        // barrido lineal sobra.
        //
        // Carrera cancel() vs runJob(), resuelta por m_mutex: un waiter sale de
        // m_pending por EXACTAMENTE UNA de dos vías mutuamente excluyentes,
        // ambas bajo este mutex:
        //   1) runJob() saca los waiters del grupo (move + erase) → luego
        //      pumpCompleted() entrega su resultado y decrementa ALLÍ.
        //   2) cancel() encuentra el waiter todavía en su grupo → lo quita y
        //      decrementa AQUÍ; ese target no produce resultado.
        // Como el move-out de runJob() y el erase de cancel() ocurren ambos
        // bajo m_mutex, un waiter o sigue en el grupo (caso 2) o ya está en
        // resultados (caso 1), nunca las dos — sin tombstones ni doble
        // decremento. Por eso este diseño puede prescindir del m_started /
        // m_cancelledBeforeStart de la Task 2.
        JobSystem::JobId jobToCancel = 0;
        bool             cancelJob   = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = m_groups.begin(); it != m_groups.end(); ++it)
            {
                PendingGroup& group = it->second;
                auto w = std::find_if(group.waiters.begin(), group.waiters.end(),
                                      [id](const auto& pair) { return pair.first == id; });
                if (w == group.waiters.end())
                    continue;

                group.waiters.erase(w);
                --m_pending;

                if (group.waiters.empty())
                {
                    // Nadie más espera este ReadFile: se puede intentar parar el
                    // job (best-effort; si ya arrancó, JobSystem lo ignora y
                    // runJob() terminará encontrando el grupo vacío/ausente y no
                    // construirá nada). Si quedan waiters, el job DEBE seguir:
                    // los demás necesitan el ReadFile.
                    jobToCancel = group.jobId;
                    cancelJob   = true;
                    m_groups.erase(it);
                }
                break;
            }
            // id no encontrado en ningún grupo: el resultado ya se construyó y
            // se movió a resultados/buzón. pumpCompleted() es el dueño de ese
            // decremento — aquí no se toca m_pending.
        }

        // m_jobs.cancel() toma el lock del JobSystem, no el nuestro. Llamarlo
        // dentro de nuestro lock sería un orden de adquisición cruzado con el
        // worker (que toma primero el del JobSystem y luego el nuestro):
        // deadlock clásico. Por eso se hace FUERA del lock.
        if (cancelJob)
            m_jobs.cancel(jobToCancel);
    }

    LoadedMesh AsyncAssetLoader::buildResultFor(const LoadedMesh& src,
                                                JobSystem::JobId job, uint64_t targetId)
    {
        LoadedMesh out;
        out.job      = job;
        out.targetId = targetId;
        out.path     = src.path;
        out.error    = src.error;
        out.images   = src.images;   // copia: cada target sube su propia textura

        // Copia profunda del Mesh, no del shared_ptr. Compartirlo dejaría a dos
        // GameObject apuntando al mismo Mesh mutable, cambiando la semántica de
        // propiedad que hay hoy en Scene.cpp:721 (un make_shared por nodo).
        // Tampoco ahorraría VRAM: addStaticMesh sube cada Mesh a su propio par
        // de buffers.
        if (src.mesh)
        {
            if (const SkinnedMesh* sk = dynamic_cast<const SkinnedMesh*>(src.mesh.get()))
                out.mesh = std::make_shared<SkinnedMesh>(*sk);
            else
                out.mesh = std::make_shared<Mesh>(*src.mesh);
        }
        return out;
    }

    void AsyncAssetLoader::runJob(const std::string& path)
    {
        LoadedMesh loaded;
        loaded.path = path;

        try
        {
            loaded.mesh = ModelLoader::loadAuto(path);
            if (loaded.mesh)
            {
                // Solo se decodifica Mesh::material (singular). Un
                // SkinnedMesh (loadAuto de un FBX con rig) guarda sus
                // texturas por submesh en SkinnedMesh::materials, que aquí
                // NO se toca a propósito — decisión diferida. Para esos
                // modelos, loaded.images queda vacío y la textura se sigue
                // resolviendo en el hilo principal por la vía síncrona
                // existente (el fallback de buildRenderObject, Task 6).
                const Material& mat = loaded.mesh->material;
                decodeSlot(mat.texturePath,             mat.embeddedTexture,            DecodedImage::Albedo, loaded.images);
                decodeSlot(mat.normalMapPath,           mat.embeddedNormalMap,          DecodedImage::Normal, loaded.images);
                decodeSlot(mat.metallicRoughnessPath,   mat.embeddedMetallicRoughness,  DecodedImage::ORM,    loaded.images);
            }
            else
            {
                loaded.error = "No se pudo cargar el modelo: " + path;
            }
        }
        catch (const std::exception& e)
        {
            // Una excepción no puede cruzar el límite de hilo: escapar de un
            // worker es std::terminate. Viaja como string.
            loaded.mesh  = nullptr;
            loaded.error = e.what();
        }
        catch (...)
        {
            loaded.mesh  = nullptr;
            loaded.error = "Error desconocido cargando " + path;
        }

        std::vector<std::pair<JobSystem::JobId, uint64_t>> waiters;
        uint64_t myEpoch = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_readFileCount;
            myEpoch = m_epoch;   // generación vigente al sacar los waiters
            auto it = m_groups.find(path);
            if (it != m_groups.end())
            {
                // Sacar los waiters bajo el lock cierra la carrera con
                // cancel(): a partir de aquí ese grupo ya no existe, así que un
                // cancel() posterior no encontrará el id y no tocará m_pending
                // (pumpCompleted lo hará al entregar).
                waiters = std::move(it->second.waiters);
                m_groups.erase(it);
            }
            // Si el grupo ya no está, cancel() vació y borró el grupo antes de
            // que este job (ya arrancado, incancelable) llegara: no hay waiters
            // que servir.
        }

        // Las copias se hacen FUERA del lock: con decenas de objetos del mismo
        // path, el hilo principal se quedaría esperando el mutex justo mientras
        // intenta pintar.
        std::vector<LoadedMesh> results;
        results.reserve(waiters.size());
        for (const auto& [job, targetId] : waiters)
            results.push_back(buildResultFor(loaded, job, targetId));

        std::lock_guard<std::mutex> lock(m_mutex);
        // Si hubo un cancelAllPending() mientras copiábamos fuera del lock, la
        // generación cambió: estos waiters ya se cancelaron (su m_pending se
        // puso a 0 allí) y sus targets son basura. Descartar los resultados —
        // postarlos dejaría m_pending negativo para siempre en el siguiente
        // pumpCompleted (-= out.size()) y entregaría meshes de objetos muertos.
        if (myEpoch != m_epoch)
            return;
        for (auto& r : results)
            m_inbox.push_back(std::move(r));
    }

    std::vector<LoadedMesh> AsyncAssetLoader::pumpCompleted(float budgetMs)
    {
        std::vector<LoadedMesh> ready;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_inbox.empty()) return ready;
            // Swap-and-drain: se saca todo bajo el lock y se procesa fuera. Con
            // el presupuesto agotado, lo que sobra vuelve al buzón — nunca se
            // descarta.
            ready.swap(m_inbox);
        }

        const auto start = std::chrono::steady_clock::now();
        std::vector<LoadedMesh> out;
        std::vector<LoadedMesh> leftover;

        for (auto& r : ready)
        {
            const float elapsedMs = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - start).count();

            // El presupuesto se comprueba ANTES de aceptar cada elemento. Con
            // budgetMs == 0 no sale ninguno, que es justo lo que pide el test.
            if (elapsedMs >= budgetMs && !out.empty())
            {
                leftover.push_back(std::move(r));
                continue;
            }
            if (budgetMs <= 0.0f)
            {
                leftover.push_back(std::move(r));
                continue;
            }
            out.push_back(std::move(r));
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& r : leftover)
                m_inbox.push_back(std::move(r));
            // Un decremento por resultado ENTREGADO. Cada resultado nació de un
            // waiter que sumó +1 en requestMesh() y que runJob() sacó de su
            // grupo (nunca lo canceló cancel(), o no estaría aquí). Los waiters
            // cancelados antes de construirse ya decrementaron en cancel() y no
            // llegan a 'out'. Sin tombstones: la exclusión grupo-vs-resultado
            // bajo m_mutex garantiza que no hay doble conteo (ver cancel()).
            m_pending -= static_cast<int>(out.size());
        }
        return out;
    }

    int AsyncAssetLoader::pending() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pending;
    }

    int AsyncAssetLoader::readFileCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_readFileCount;
    }

    bool applyLoadedMesh(LoadedMesh& r, Scene& scene, EditorRenderer& renderer,
                         std::string* outError)
    {
        // Recorrido en vivo, no una lista cacheada: el editor permite borrar
        // GameObjects en cualquier frame, así que un puntero guardado en la
        // petición sería colgante. Mismo motivo que el liveCube de main.cpp:293.
        GameObject* target = nullptr;
        scene.traverse([&](GameObject* go) { if (go->id == r.targetId) target = go; });

        // Borrado mientras cargaba: el trabajo del worker se tira y ya está. Sin
        // tocar memoria liberada, que es justo lo que evita resolver por id.
        if (!target) return false;

        target->pendingMeshJob = 0;

        if (!r.error.empty())
        {
            if (outError) *outError = "Error cargando '" + r.path + "': " + r.error;
            return false;
        }
        if (!r.mesh) return false;

        // Registrar en el Renderer ANTES de setMesh, igual que la ruta síncrona
        // de PropertiesPanel:144-150: si el registro lanza, el GameObject queda
        // intacto y el reintento funciona en vez de ser un no-op silencioso.
        try
        {
            if (SkinnedMesh* sk = dynamic_cast<SkinnedMesh*>(r.mesh.get()))
                target->skinnedRenderIndex = renderer.addSkinnedMesh(*sk, &r.images);
            else
                target->staticRenderIndex  = renderer.addStaticMesh(*r.mesh, &r.images);

            target->setMesh(r.mesh);
        }
        catch (const std::exception& e)
        {
            if (outError) *outError = std::string("Error subiendo a GPU '") + r.path + "': " + e.what();
            return false;
        }
        return true;
    }

    void AsyncAssetLoader::cancelAllPending()
    {
        std::vector<JobSystem::JobId> toCancel;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Bump de generación: un job ya arrancado (incancelable) que sacó
            // sus waiters ANTES de este bump y todavía está copiando fuera del
            // lock verá el epoch cambiado al ir a postar y descartará sus
            // resultados (ver runJob()). Sin esto, esos posts dejarían
            // m_pending negativo para siempre y entregarían meshes cancelados.
            ++m_epoch;
            // Solo el id encolado por grupo: los ids reservados por waiters no
            // primeros nunca se enviaron al JobSystem, así que cancelarlos solo
            // ensuciaría su set m_cancelled (que no se limpia hasta que un job
            // con ese id se saca de la cola, cosa que nunca pasaría).
            for (const auto& [path, group] : m_groups)
                toCancel.push_back(group.jobId);
            m_groups.clear();
            m_inbox.clear();
            m_pending = 0;
        }

        // cancel() toma el lock del JobSystem, no el nuestro. Llamarlo dentro de
        // nuestro lock sería un orden de adquisición cruzado con el worker, que
        // toma primero el del JobSystem y luego el nuestro: deadlock clásico.
        for (JobSystem::JobId id : toCancel)
            m_jobs.cancel(id);
    }
}
