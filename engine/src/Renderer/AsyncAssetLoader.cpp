#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/ModelLoader.h"

#include <stb_image.h>

#include <chrono>
#include <exception>
#include <utility>

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
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_pending;
        }

        // El id se reserva ANTES de encolar: el job necesita conocer el suyo
        // para etiquetar el resultado, y leerlo del retorno de submit() sería
        // una carrera con un worker que ya hubiera arrancado.
        //
        // path y targetId van POR COPIA. Por referencia serían dangling en
        // cuanto el caller saliera de scope, y no se vería hasta que el worker
        // arrancase — el peor tipo de bug de este diseño.
        const JobSystem::JobId id = m_jobs.reserveId();
        m_jobs.submitWithId(id, [this, id, path, targetId] { runJob(id, path, targetId); });
        return id;
    }

    void AsyncAssetLoader::cancel(JobSystem::JobId id)
    {
        // Best-effort: si JobSystem todavía no ha sacado este job de la
        // cola, esto evita que llegue a correr (workerLoop lo salta al
        // hacer pop). Un job ya arrancado NO se interrumpe — eso es cosa de
        // JobSystem, no nuestra (ver JobSystem::cancel()).
        m_jobs.cancel(id);

        // pending() cuenta "en cola, en vuelo o en el buzón" (ver header).
        // Un id cancelado ANTES de arrancar nunca va a pasar por runJob(), y
        // por tanto nunca va a pasar por el post al buzón ni por la entrega
        // de pumpCompleted — que son los DOS únicos sitios que hoy
        // decrementan m_pending. Sin este bloque, ese id se queda contado
        // para siempre: el bug que arregla este fix.
        //
        // El problema es que cancel() no puede saber, de forma síncrona, si
        // el job ya había arrancado cuando llega la cancelación — es una
        // carrera real contra el worker. La resolvemos así: runJob() marca
        // su propio id en m_started como PRIMERA operación, bajo este mismo
        // m_mutex. Como cancel() también lee m_started bajo el mismo mutex,
        // "¿ya había arrancado?" es una pregunta atómica: o bien cancel()
        // adquiere el lock primero (m_started todavía no tiene el id) o
        // runJob() lo adquiere primero (ya lo tiene) — no hay tercer caso.
        //
        //  - No había arrancado: decrementamos aquí y marcamos el id en
        //    m_cancelledBeforeStart. Si pese a todo el job termina
        //    corriendo igualmente (JobSystem perdió la carrera interna por
        //    microsegundos) y postea su resultado al buzón, pumpCompleted
        //    verá el id en m_cancelledBeforeStart y NO volverá a
        //    decrementar — evita el doble decremento.
        //  - Ya había arrancado (o ya se entregó): no tocamos el contador.
        //    Lo resuelve runJob() + pumpCompleted() exactamente igual que
        //    si no se hubiera cancelado — el resultado sigue llegando al
        //    buzón y contando una única vez, ahí.
        //
        // insert().second sólo es true la primera vez: una cancelación
        // repetida del mismo id (o una cancelación tras la entrega) es un
        // no-op, nunca decrementa dos veces.
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started.count(id) && m_cancelledBeforeStart.insert(id).second)
            --m_pending;
    }

    void AsyncAssetLoader::runJob(JobSystem::JobId id, const std::string& path, uint64_t targetId)
    {
        {
            // Marca de "ya arrancado" para la contabilidad de cancel() (ver
            // el comentario largo ahí). Tiene que ser lo PRIMERO que hace
            // runJob(), antes de tocar disco o Assimp, para que la ventana
            // de carrera con cancel() sea lo más pequeña posible.
            std::lock_guard<std::mutex> lock(m_mutex);
            m_started.insert(id);
        }

        LoadedMesh result;
        result.job      = id;
        result.targetId = targetId;
        result.path     = path;

        try
        {
            result.mesh = ModelLoader::loadAuto(path);
            if (result.mesh)
            {
                // Solo se decodifica Mesh::material (singular). Un
                // SkinnedMesh (loadAuto de un FBX con rig) guarda sus
                // texturas por submesh en SkinnedMesh::materials, que aquí
                // NO se toca a propósito — decisión diferida. Para esos
                // modelos, result.images queda vacío y la textura se sigue
                // resolviendo en el hilo principal por la vía síncrona
                // existente (el fallback de buildRenderObject, Task 6).
                const Material& mat = result.mesh->material;
                decodeSlot(mat.texturePath,             mat.embeddedTexture,            DecodedImage::Albedo, result.images);
                decodeSlot(mat.normalMapPath,           mat.embeddedNormalMap,          DecodedImage::Normal, result.images);
                decodeSlot(mat.metallicRoughnessPath,   mat.embeddedMetallicRoughness,  DecodedImage::ORM,    result.images);
            }
            else
            {
                result.error = "No se pudo cargar el modelo: " + path;
            }
        }
        catch (const std::exception& e)
        {
            // Una excepción no puede cruzar el límite de hilo: escapar de un
            // worker es std::terminate. Viaja como string.
            result.mesh  = nullptr;
            result.error = e.what();
        }
        catch (...)
        {
            result.mesh  = nullptr;
            result.error = "Error desconocido cargando " + path;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_inbox.push_back(std::move(result));
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
            // Decremento por elemento, no en bloque: un id que cancel() ya
            // resolvió como "cancelado antes de arrancar" (ver cancel()) ya
            // decrementó m_pending ahí. Si ese mismo id, pese a todo, llegó a
            // correr y aparece aquí en 'out', hay que saltarlo — sumarlo
            // otra vez sería un doble decremento y dejaría m_pending
            // negativo o por debajo de la cuenta real.
            for (auto& r : out)
            {
                if (m_cancelledBeforeStart.erase(r.job) == 0)
                    --m_pending;
            }
        }
        return out;
    }

    int AsyncAssetLoader::pending() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pending;
    }
}
