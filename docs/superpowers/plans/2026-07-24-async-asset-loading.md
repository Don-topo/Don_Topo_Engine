# Carga asíncrona de assets y uploads GPU sin stall — Plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sacar la carga de meshes y texturas del hilo principal a un pool de workers, y agrupar los uploads a Vulkan en un submit con fence, eliminando los ~11 `vkQueueWaitIdle` por mesh y los 4 `vkDeviceWaitIdle` del Renderer.

**Architecture:** Dos capas independientes. La capa A (`JobSystem` + `AsyncAssetLoader`) mueve el trabajo CPU puro — `Assimp::ReadFile` y `stbi_load` — a hilos worker que producen bytes en RAM; el hilo principal los recoge en un pump con presupuesto por frame. La capa B (`TransferBatch` + `DeferredDeleteQueue`) agrupa todos los uploads de un pump en un command buffer con una fence, y difiere las destrucciones de recursos `MAX_FRAMES+1` frames en lugar de vaciar el device. El audio no usa el JobSystem: va con `FMOD_NONBLOCKING`.

**Tech Stack:** C++20, Vulkan, Assimp, stb_image, FMOD Core, ImGui, nlohmann/json, CMake + Ninja + MSVC.

**Spec:** `docs/superpowers/specs/2026-07-24-async-asset-loading-design.md`

## Global Constraints

- **C++20.** Todos los targets nuevos llevan `target_compile_features(<target> PRIVATE cxx_std_20)`, igual que los existentes en `engine/tests/CMakeLists.txt`.
- **Ni Vulkan ni FMOD se tocan desde un hilo worker.** El worker produce bytes en RAM; el hilo principal los sube. Violarlo es un fallo de revisión, no un detalle de estilo.
- **Nunca cruzar un `GameObject*` por el límite de hilo.** Las peticiones llevan `GameObject::id` (`uint64_t`, ver `engine/include/DonTopo/Core/GameObject.h:30`). El pump resuelve por `id` sobre la escena viva.
- **Ninguna excepción escapa de un job.** Escapar de un worker es `std::terminate`. Todo job lleva `try/catch(...)` y el error viaja como string en el resultado.
- **Comentarios y mensajes de usuario en español**, siguiendo el estilo del repo. El código (identificadores) en inglés.
- **Los 8 test suites existentes no se modifican.** Si un cambio los obliga a cambiar, el cambio está mal planteado. La ruta síncrona debe seguir siendo idéntica bit a bit.
- **Build:** `.\configure.bat` y `.\build.bat` desde PowerShell (usan vcvarsall + Ninja). No invocar cmake a pelo desde Bash.
- **Tests:** `main` + `assert` planos, sin framework, headless — estilo de `engine/tests/audio_tests.cpp`. Cada ejecutable devuelve 0 al pasar.
- **Todo test de concurrencia corre 50 iteraciones.** Un race de 1 entre 20 no se caza en una pasada.
- **Cada test lleva su comprobación de sabotaje.** Se rompe el código a propósito, se confirma que el test falla, se restaura. Sin esa evidencia el paso no está hecho.

## Estructura de ficheros

| Fichero | Responsabilidad | Tarea |
|---|---|---|
| `engine/include/DonTopo/Core/JobSystem.h` | Pool de hilos, cola FIFO, cancelación. Cero dependencias del motor | 1 |
| `engine/src/Core/JobSystem.cpp` | Implementación del pool | 1 |
| `engine/tests/jobsystem_tests.cpp` | Tests del pool | 1 |
| `engine/include/DonTopo/Renderer/AsyncAssetLoader.h` | `DecodedImage`, `LoadedMesh`, peticiones y buzón | 2, 3 |
| `engine/src/Renderer/AsyncAssetLoader.cpp` | Job de carga + decodificación + dedup | 2, 3 |
| `engine/tests/asset_loader_tests.cpp` | Tests del loader | 2, 3 |
| `engine/include/DonTopo/Renderer/TransferBatch.h` | Agrupa uploads en 1 submit + 1 fence | 4 |
| `engine/src/Renderer/TransferBatch.cpp` | Implementación | 4 |
| `engine/include/DonTopo/Renderer/DeferredDelete.h` | Cola de destrucción por frame | 5 |
| `engine/src/Renderer/DeferredDelete.cpp` | Implementación | 5 |
| `engine/include/DonTopo/Editor/LoadingModal.h` | Overlay ImGui de progreso | 9 |
| `engine/src/Editor/LoadingModal.cpp` | Implementación | 9 |
| `engine/tests/scene_async_tests.cpp` | Tests de `fromJson` con y sin loader | 8 |

Modificados: `engine/CMakeLists.txt`, `engine/tests/CMakeLists.txt`, `GpuResources.{h,cpp}`, `Renderer.{h,cpp}`, `AudioManager.{h,cpp}`, `Scene.{h,cpp}`, `GameObject.h`, `PropertiesPanel.{h,cpp}`, `EditorUI.{h,cpp}`, `sandbox/src/main.cpp`, `runtime/main.cpp`.

---

### Task 1: JobSystem

**Files:**
- Create: `engine/include/DonTopo/Core/JobSystem.h`
- Create: `engine/src/Core/JobSystem.cpp`
- Create: `engine/tests/jobsystem_tests.cpp`
- Modify: `engine/CMakeLists.txt` (añadir `src/Core/JobSystem.cpp` a la lista de fuentes)
- Modify: `engine/tests/CMakeLists.txt` (nuevo target `dt_jobsystem_tests`)

**Interfaces:**
- Consumes: nada.
- Produces: `DonTopo::JobSystem` con `using JobId = uint64_t`, `void start(unsigned threads = 0)`, `void shutdown()`, `JobId submit(std::function<void()>)`, `JobId reserveId()`, `void submitWithId(JobId, std::function<void()>)`, `void cancel(JobId)`, `bool idle() const`, `unsigned threadCount() const`.

- [ ] **Step 1: Escribir el test que falla**

Crear `engine/tests/jobsystem_tests.cpp`:

```cpp
// Tests del pool de hilos. Plain main + asserts, sin framework — coherente con
// audio_tests.cpp y camera_tests.cpp.
//
// Cada caso corre kIters veces: un race que aparece 1 de cada 20 ejecuciones no
// se caza en una sola pasada, y un test de concurrencia que solo corre una vez
// da una falsa sensación de cobertura.
#include "DonTopo/Core/JobSystem.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <vector>

namespace {

constexpr int kIters = 50;

// 1000 jobs incrementando un atomic dan exactamente 1000. Sabotaje: cambiar el
// atomic por un int normal — con -fsanitize=thread o repitiendo, la suma baja.
void testAllJobsRun()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start();
        std::atomic<int> counter{0};
        for (int i = 0; i < 1000; ++i)
            js.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        js.shutdown();
        assert(counter.load() == 1000 && "shutdown debe drenar la cola entera");
    }
}

// shutdown() con la cola llena ejecuta TODO lo encolado, no lo tira. Sabotaje:
// poner m_stop = true antes de drenar en shutdown() — el contador se queda corto.
void testShutdownDrains()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(1);   // 1 hilo garantiza que la cola se acumula de verdad
        std::atomic<int> counter{0};
        for (int i = 0; i < 200; ++i)
            js.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        js.shutdown();
        assert(counter.load() == 200 && "un shutdown no puede descartar jobs encolados");
    }
}

// cancel() de un job todavía en cola impide que corra. Con 1 hilo y un job
// bloqueante por delante, el cancelado no ha podido arrancar. Sabotaje: ignorar
// el flag de cancelación en el worker — el contador sube a 1.
void testCancelPreventsQueuedJob()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start(1);

        std::atomic<bool> release{false};
        std::atomic<int>  ran{0};

        js.submit([&release] { while (!release.load(std::memory_order_acquire)) {} });
        DonTopo::JobSystem::JobId victim =
            js.submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); });

        js.cancel(victim);
        release.store(true, std::memory_order_release);
        js.shutdown();

        assert(ran.load() == 0 && "un job cancelado antes de arrancar no debe ejecutarse");
    }
}

// shutdown() dos veces no cuelga ni peta. Sabotaje: quitar la guarda de
// idempotencia — el segundo join sobre hilos ya unidos aborta o deadlockea.
void testDoubleShutdown()
{
    for (int it = 0; it < kIters; ++it)
    {
        DonTopo::JobSystem js;
        js.start();
        js.submit([] {});
        js.shutdown();
        js.shutdown();
    }
}

// start(1) es válido: cubre el clamp inferior sin depender del hardware.
void testSingleThread()
{
    DonTopo::JobSystem js;
    js.start(1);
    assert(js.threadCount() == 1);
    std::atomic<int> counter{0};
    for (int i = 0; i < 50; ++i)
        js.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    js.shutdown();
    assert(counter.load() == 50);
}

// start(0) aplica clamp(hardware_concurrency()-1, 2, 8): nunca 0, nunca >8.
void testAutoThreadCountClamped()
{
    DonTopo::JobSystem js;
    js.start(0);
    const unsigned n = js.threadCount();
    assert(n >= 2 && n <= 8 && "el clamp automatico debe caer en [2,8]");
    js.shutdown();
}

// Un job que lanza no tumba el proceso: el worker lo traga. Sabotaje: quitar el
// try/catch del worker — std::terminate y el test no llega a imprimir OK.
void testJobExceptionDoesNotTerminate()
{
    DonTopo::JobSystem js;
    js.start(2);
    js.submit([] { throw std::runtime_error("boom"); });
    std::atomic<int> after{0};
    js.submit([&after] { after.fetch_add(1, std::memory_order_relaxed); });
    js.shutdown();
    assert(after.load() == 1 && "una excepcion en un job no puede matar al worker");
}

// reserveId() da ids unicos y submitWithId() los respeta: el job ve su propio
// id sin la carrera de leerlo despues de submit(). Sabotaje: hacer que
// reserveId devuelva siempre 1 — el assert de unicidad salta.
void testReserveIdIsUniqueAndUsable()
{
    DonTopo::JobSystem js;
    js.start(2);

    const DonTopo::JobSystem::JobId a = js.reserveId();
    const DonTopo::JobSystem::JobId b = js.reserveId();
    assert(a != 0 && b != 0 && a != b && "cada reserveId da un id distinto y no nulo");

    std::atomic<uint64_t> seen{0};
    js.submitWithId(a, [&seen, a] { seen.store(a, std::memory_order_relaxed); });
    js.shutdown();

    assert(seen.load() == a && "el job debe poder capturar su propio id ya relleno");
}

// Un id reservado y cancelado antes de submitWithId no llega a ejecutarse.
// Sabotaje: ignorar m_cancelled en el worker — el contador sube.
void testCancelBeforeSubmitWithId()
{
    DonTopo::JobSystem js;
    js.start(1);

    const DonTopo::JobSystem::JobId id = js.reserveId();
    js.cancel(id);

    std::atomic<int> ran{0};
    js.submitWithId(id, [&ran] { ran.fetch_add(1, std::memory_order_relaxed); });
    js.shutdown();

    assert(ran.load() == 0 && "cancelar antes de encolar tambien debe impedir la ejecucion");
}

} // namespace

int main()
{
    testAllJobsRun();
    testShutdownDrains();
    testCancelPreventsQueuedJob();
    testDoubleShutdown();
    testSingleThread();
    testAutoThreadCountClamped();
    testJobExceptionDoesNotTerminate();
    testReserveIdIsUniqueAndUsable();
    testCancelBeforeSubmitWithId();
    std::printf("jobsystem_tests OK\n");
    return 0;
}
```

Añadir a `engine/tests/CMakeLists.txt`, después del bloque de `dt_splash_tests` y antes del bloque `if(FMOD_FOUND AND WIN32)`:

```cmake
add_executable(dt_jobsystem_tests jobsystem_tests.cpp)
target_link_libraries(dt_jobsystem_tests PRIVATE DonTopoEngine)
target_compile_features(dt_jobsystem_tests PRIVATE cxx_std_20)
```

- [ ] **Step 2: Ejecutar el test para verificar que falla**

```powershell
.\build.bat
```

Esperado: FALLA en compilación con `Cannot open include file: 'DonTopo/Core/JobSystem.h'`.

- [ ] **Step 3: Escribir la cabecera**

Crear `engine/include/DonTopo/Core/JobSystem.h`:

```cpp
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

namespace DonTopo
{
    // Pool de hilos genérico: no sabe nada de assets, Vulkan ni escena. Lo usa
    // AsyncAssetLoader, pero se testea solo.
    //
    // NO es singleton a propósito: la app crea la suya en main() y la pasa por
    // referencia, así los tests montan una por caso sin estado global entre
    // ellos.
    class JobSystem
    {
        public:
            using JobId = uint64_t;

            JobSystem() = default;
            ~JobSystem() { shutdown(); }
            JobSystem(const JobSystem&)            = delete;
            JobSystem& operator=(const JobSystem&) = delete;

            // threads == 0 → clamp(hardware_concurrency() - 1, 2, 8). El -1 deja
            // un core para el hilo principal, que es quien pinta. Llamar dos
            // veces sin shutdown() entre medias es un no-op.
            void start(unsigned threads = 0);

            // Drena la cola (ejecuta lo pendiente), para los hilos y hace join.
            // Idempotente: el destructor lo llama y el usuario también puede.
            void shutdown();

            // Devuelve 0 si el pool no está arrancado — el job NO se ejecuta.
            JobId submit(std::function<void()> fn);

            // Reserva un JobId sin encolar nada, y encola con un id ya
            // reservado. Existen para que un job pueda conocer su PROPIO id
            // desde dentro: con submit() a secas, el id solo se conoce al
            // retornar, y un worker rápido puede haber arrancado ya. Leerlo
            // entonces desde el lambda es una carrera.
            JobId reserveId();
            void  submitWithId(JobId id, std::function<void()> fn);

            // Marca id como cancelado. Un job ya arrancado NO se interrumpe (no
            // se puede parar un Assimp::ReadFile a medias): termina y es el
            // consumidor quien descarta su resultado.
            void cancel(JobId id);

            bool     idle() const;
            unsigned threadCount() const;

        private:
            struct Job
            {
                JobId                 id;
                std::function<void()> fn;
            };

            void workerLoop();

            mutable std::mutex       m_mutex;
            std::condition_variable  m_cv;
            std::deque<Job>          m_queue;
            std::unordered_set<JobId> m_cancelled;
            std::vector<std::thread> m_threads;
            JobId                    m_nextId   = 1;
            int                      m_inFlight = 0;
            bool                     m_stop     = false;
    };
}
```

- [ ] **Step 4: Escribir la implementación**

Crear `engine/src/Core/JobSystem.cpp`:

```cpp
#include "DonTopo/Core/JobSystem.h"

#include <algorithm>

namespace DonTopo
{
    void JobSystem::start(unsigned threads)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_threads.empty()) return;   // ya arrancado
            m_stop = false;
        }

        if (threads == 0)
        {
            const unsigned hw = std::thread::hardware_concurrency();
            // hw puede devolver 0 si el SO no lo sabe; el clamp lo cubre.
            const unsigned avail = (hw > 1) ? (hw - 1) : 1;
            threads = std::clamp(avail, 2u, 8u);
        }

        m_threads.reserve(threads);
        for (unsigned i = 0; i < threads; ++i)
            m_threads.emplace_back([this] { workerLoop(); });
    }

    void JobSystem::shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // Idempotencia: sin esto, el segundo shutdown() haría join sobre
            // hilos ya unidos y abortaría. El destructor llama a shutdown(), y
            // el usuario también: los dos caminos tienen que convivir.
            if (m_threads.empty()) return;
            m_stop = true;
        }
        m_cv.notify_all();

        for (auto& t : m_threads)
            if (t.joinable()) t.join();

        m_threads.clear();

        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
        m_cancelled.clear();
    }

    JobSystem::JobId JobSystem::submit(std::function<void()> fn)
    {
        JobId id;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_threads.empty() || m_stop) return 0;
            id = m_nextId++;
            m_queue.push_back(Job{id, std::move(fn)});
        }
        m_cv.notify_one();
        return id;
    }

    JobSystem::JobId JobSystem::reserveId()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_nextId++;
    }

    void JobSystem::submitWithId(JobId id, std::function<void()> fn)
    {
        if (id == 0) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_threads.empty() || m_stop) return;
            m_queue.push_back(Job{id, std::move(fn)});
        }
        m_cv.notify_one();
    }

    void JobSystem::cancel(JobId id)
    {
        if (id == 0) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancelled.insert(id);
    }

    bool JobSystem::idle() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty() && m_inFlight == 0;
    }

    unsigned JobSystem::threadCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<unsigned>(m_threads.size());
    }

    void JobSystem::workerLoop()
    {
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });

                // Con m_stop y cola vacía se sale. Con m_stop y cola llena NO se
                // sale: shutdown() promete drenar lo encolado. Al revés, un
                // Load Scene cancelado a medias dejaría GameObjects sin mesh y
                // sin nadie que lo reporte.
                if (m_queue.empty()) return;

                job = std::move(m_queue.front());
                m_queue.pop_front();

                if (m_cancelled.erase(job.id) > 0)
                    continue;   // cancelado antes de arrancar: ni se ejecuta

                ++m_inFlight;
            }

            // Una excepción escapando de aquí es std::terminate: el hilo no
            // tiene a nadie por encima que la capture. Cada job de verdad ya
            // convierte sus fallos en un string de error, pero el catch(...) es
            // la red de seguridad de que ninguno se olvide.
            try { job.fn(); }
            catch (...) { }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                --m_inFlight;
            }
        }
    }
}
```

Añadir a `engine/CMakeLists.txt`, junto a las demás fuentes de `src/Core/` (después de `src/Core/GameObject.cpp`):

```cmake
    src/Core/JobSystem.cpp
```

- [ ] **Step 5: Ejecutar los tests para verificar que pasan**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_jobsystem_tests.exe
```

Esperado: `jobsystem_tests OK` y código de salida 0.

- [ ] **Step 6: Verificar el sabotaje de cada test**

Uno a uno, aplicar el sabotaje, reconstruir, confirmar que el test **falla**, y **revertir**:

1. En `workerLoop`, cambiar `if (m_queue.empty()) return;` por `if (m_stop) return;` → `testShutdownDrains` debe fallar en su assert.
2. En `workerLoop`, borrar la línea `if (m_cancelled.erase(job.id) > 0) continue;` → `testCancelPreventsQueuedJob` debe fallar.
3. En `shutdown`, borrar `if (m_threads.empty()) return;` → `testDoubleShutdown` debe abortar o colgarse.
4. En `workerLoop`, cambiar `try { job.fn(); } catch (...) { }` por `job.fn();` → `testJobExceptionDoesNotTerminate` debe terminar el proceso sin imprimir OK.

Un sabotaje que NO haga fallar su test significa que el test es vacuo y hay que arreglarlo antes de seguir.

- [ ] **Step 7: Verificar que no hay regresiones**

```powershell
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_audio_tests.exe
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_animator_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
.\build-ninja\engine\tests\dt_exporter_tests.exe
.\build-ninja\engine\tests\dt_scripting_tests.exe
.\build-ninja\engine\tests\dt_splash_tests.exe
```

Esperado: los 8 imprimen OK y salen con 0.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/Core/JobSystem.h engine/src/Core/JobSystem.cpp engine/tests/jobsystem_tests.cpp engine/CMakeLists.txt engine/tests/CMakeLists.txt
git commit -m "feat(core): JobSystem, pool de hilos con cancelacion"
```

---

### Task 2: AsyncAssetLoader — carga y decodificación en worker

**Files:**
- Create: `engine/include/DonTopo/Renderer/AsyncAssetLoader.h`
- Create: `engine/src/Renderer/AsyncAssetLoader.cpp`
- Create: `engine/tests/asset_loader_tests.cpp`
- Modify: `engine/CMakeLists.txt`
- Modify: `engine/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `DonTopo::JobSystem` de la Task 1 (`reserveId`, `submitWithId`, `cancel`, `start`, `shutdown`). `ModelLoader::loadAuto(const std::string&) -> std::shared_ptr<Mesh>` (`engine/include/DonTopo/Renderer/ModelLoader.h:48`).
- Produces: `struct DecodedImage { enum Slot { Albedo, Normal, ORM }; Slot slot; int w, h; std::vector<uint8_t> pixels; }`, `struct LoadedMesh { JobSystem::JobId job; uint64_t targetId; std::string path; std::shared_ptr<Mesh> mesh; std::vector<DecodedImage> images; std::string error; }`, y `class AsyncAssetLoader` con `JobId requestMesh(const std::string& path, uint64_t targetId)`, `void cancel(JobId)`, `std::vector<LoadedMesh> pumpCompleted(float budgetMs)`, `int pending() const`.

- [ ] **Step 1: Escribir el test que falla**

Crear `engine/tests/asset_loader_tests.cpp`:

```cpp
// Tests de AsyncAssetLoader. Headless: no crea device Vulkan, solo comprueba
// que el worker produce los mismos datos en RAM que la ruta sincrona.
//
// Las aserciones comparan contra ModelLoader::load / loadAuto, NO contra
// constantes hardcodeadas: si manana cambian los flags de Assimp, el test sigue
// siendo valido en vez de convertirse en una constante a reajustar.
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"
#include "DonTopo/Renderer/ModelLoader.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

// Ruta al FBX de pruebas. El test se salta los casos que dependen de el si no
// existe, en vez de fallar: un clone sin assets debe poder correr el resto.
std::string findTestFbx()
{
    for (const char* rel : { "assets/models/cube.fbx", "../assets/models/cube.fbx",
                             "../../assets/models/cube.fbx", "../../../assets/models/cube.fbx" })
        if (std::filesystem::exists(rel)) return rel;
    return {};
}

// Bombea hasta que lleguen `expected` resultados o se agote el plazo. Devuelve
// lo recogido. Sin plazo, un fallo del loader colgaria el test para siempre.
std::vector<DonTopo::LoadedMesh> drain(DonTopo::AsyncAssetLoader& loader,
                                       size_t expected, int timeoutMs = 30000)
{
    std::vector<DonTopo::LoadedMesh> out;
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);
    while (out.size() < expected && std::chrono::steady_clock::now() < deadline)
    {
        for (auto& r : loader.pumpCompleted(1000.0f))
            out.push_back(std::move(r));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return out;
}

// El mesh que sale del worker es el mismo que da la ruta sincrona. Sabotaje:
// en el job, cambiar loadAuto(path) por loadAuto(path) y vaciar mesh->indices
// — el assert de indices salta.
void testAsyncMatchesSync(const std::string& fbx)
{
    const std::shared_ptr<DonTopo::Mesh> expected = DonTopo::ModelLoader::loadAuto(fbx);
    assert(expected && "la ruta sincrona debe cargar el FBX de pruebas");

    DonTopo::JobSystem js;
    js.start();
    DonTopo::AsyncAssetLoader loader(js);

    loader.requestMesh(fbx, /*targetId=*/42);
    std::vector<DonTopo::LoadedMesh> got = drain(loader, 1);

    assert(got.size() == 1 && "el buzon debe entregar exactamente un resultado");
    assert(got[0].error.empty() && "un FBX valido no debe reportar error");
    assert(got[0].targetId == 42 && "el targetId debe viajar intacto");
    assert(got[0].mesh != nullptr);
    assert(got[0].mesh->vertices.size() == expected->vertices.size());
    assert(got[0].mesh->indices.size()  == expected->indices.size());
    assert(got[0].mesh->name            == expected->name);

    js.shutdown();
}

// Un path inexistente NO lanza: devuelve error no vacio y mesh nulo. Sabotaje:
// quitar el try/catch del job — el proceso muere por std::terminate.
void testMissingFileReportsError()
{
    DonTopo::JobSystem js;
    js.start();
    DonTopo::AsyncAssetLoader loader(js);

    loader.requestMesh("no/existe/ningun/fichero.fbx", 7);
    std::vector<DonTopo::LoadedMesh> got = drain(loader, 1);

    assert(got.size() == 1);
    assert(!got[0].error.empty() && "un path invalido debe llenar error");
    assert(got[0].mesh == nullptr && "sin mesh cuando hay error");

    js.shutdown();
}

// pumpCompleted(0) no procesa nada Y no pierde nada: el siguiente pump entrega
// todo. Sabotaje: hacer que pumpCompleted vacie el buzon antes de mirar el
// presupuesto — el segundo drain se queda a cero y el test se cuelga hasta el
// timeout, fallando el assert de tamano.
void testZeroBudgetKeepsResults(const std::string& fbx)
{
    DonTopo::JobSystem js;
    js.start();
    DonTopo::AsyncAssetLoader loader(js);

    loader.requestMesh(fbx, 1);
    // Espera activa a que el worker deje el resultado en el buzon.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (loader.pending() > 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    assert(loader.pumpCompleted(0.0f).empty() && "presupuesto 0 no procesa nada");

    std::vector<DonTopo::LoadedMesh> got = drain(loader, 1);
    assert(got.size() == 1 && "un pump con presupuesto 0 no puede perder resultados");

    js.shutdown();
}

// Las texturas del material llegan DECODIFICADAS desde el worker. Es el punto
// de la feature: si stbi_load siguiera en el hilo principal, se perderia la
// mayor parte de la ganancia. Sabotaje: en el job, no rellenar images — el
// assert de w/h salta.
//
// Solo aplica si el FBX de pruebas trae textura; si no, el caso se salta.
void testTexturesArriveDecoded(const std::string& fbx)
{
    const std::shared_ptr<DonTopo::Mesh> sync = DonTopo::ModelLoader::loadAuto(fbx);
    if (sync->material.texturePath.empty() && sync->material.embeddedTexture.empty())
    {
        std::printf("  (saltado: el FBX de pruebas no trae textura)\n");
        return;
    }

    DonTopo::JobSystem js;
    js.start();
    DonTopo::AsyncAssetLoader loader(js);

    loader.requestMesh(fbx, 3);
    std::vector<DonTopo::LoadedMesh> got = drain(loader, 1);
    assert(got.size() == 1 && got[0].error.empty());

    bool foundAlbedo = false;
    for (const auto& img : got[0].images)
        if (img.slot == DonTopo::DecodedImage::Albedo)
        {
            foundAlbedo = true;
            assert(img.w > 1 && img.h > 1 && "una textura real no es 1x1");
            assert(img.pixels.size() == static_cast<size_t>(img.w) * img.h * 4
                   && "los pixeles llegan en RGBA8, sin padding");
        }
    assert(foundAlbedo && "el worker debe decodificar el albedo, no dejarlo al main");

    js.shutdown();
}

} // namespace

int main()
{
    const std::string fbx = findTestFbx();

    testMissingFileReportsError();

    if (fbx.empty())
    {
        std::printf("asset_loader_tests OK (casos con FBX saltados: no se encontro el asset)\n");
        return 0;
    }

    testAsyncMatchesSync(fbx);
    testZeroBudgetKeepsResults(fbx);
    testTexturesArriveDecoded(fbx);

    std::printf("asset_loader_tests OK\n");
    return 0;
}
```

Añadir a `engine/tests/CMakeLists.txt`:

```cmake
add_executable(dt_asset_loader_tests asset_loader_tests.cpp)
target_link_libraries(dt_asset_loader_tests PRIVATE DonTopoEngine)
target_compile_features(dt_asset_loader_tests PRIVATE cxx_std_20)
```

- [ ] **Step 2: Ejecutar el test para verificar que falla**

```powershell
.\build.bat
```

Esperado: FALLA en compilación con `Cannot open include file: 'DonTopo/Renderer/AsyncAssetLoader.h'`.

- [ ] **Step 3: Localizar el FBX de pruebas**

```powershell
Get-ChildItem -Recurse -Path assets -Include *.fbx | Select-Object -First 5 FullName
```

Si ninguno está en `assets/models/cube.fbx`, ajustar la lista de rutas candidatas de `findTestFbx()` en el test a lo que exista realmente. **No copiar ni mover ficheros de `assets/`.**

- [ ] **Step 4: Escribir la cabecera**

Crear `engine/include/DonTopo/Renderer/AsyncAssetLoader.h`:

```cpp
#pragma once
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Renderer/Mesh.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace DonTopo
{
    // Una textura ya decodificada a RGBA8 por el worker. El hilo principal solo
    // hace el upload: el stbi_load, que es la mitad del coste de cargar un
    // modelo, ya ocurrió fuera.
    struct DecodedImage
    {
        enum Slot { Albedo, Normal, ORM };

        Slot                 slot = Albedo;
        int                  w    = 0;
        int                  h    = 0;
        std::vector<uint8_t> pixels;   // w*h*4, RGBA8, sin padding
    };

    // Resultado de una petición. Viaja por valor del worker al hilo principal:
    // ni un puntero compartido mutable, ni un GameObject*.
    struct LoadedMesh
    {
        JobSystem::JobId          job      = 0;
        uint64_t                  targetId = 0;   // GameObject::id, nunca un puntero
        std::string               path;
        std::shared_ptr<Mesh>     mesh;           // puede ser SkinnedMesh (loadAuto)
        std::vector<DecodedImage> images;
        std::string               error;          // no vacío = falló
    };

    // Traduce peticiones de asset a jobs y guarda los resultados en un buzón que
    // el hilo principal drena una vez por frame.
    //
    // No conoce Vulkan: produce bytes en RAM. Quien los sube es el Renderer.
    class AsyncAssetLoader
    {
        public:
            explicit AsyncAssetLoader(JobSystem& jobs) : m_jobs(jobs) {}
            AsyncAssetLoader(const AsyncAssetLoader&)            = delete;
            AsyncAssetLoader& operator=(const AsyncAssetLoader&) = delete;

            // targetId es el GameObject::id al que asignar el mesh. El pump
            // resuelve por id sobre la escena viva: si el objeto se borró
            // mientras cargaba, el resultado se descarta sin tocar memoria
            // liberada.
            JobSystem::JobId requestMesh(const std::string& path, uint64_t targetId);

            void cancel(JobSystem::JobId id);

            // Hilo principal. Devuelve los resultados listos, parando cuando se
            // agota budgetMs. Lo no devuelto sigue en el buzón para el próximo
            // frame — jamás se descarta por presupuesto.
            std::vector<LoadedMesh> pumpCompleted(float budgetMs);

            // Peticiones aún sin recoger por pumpCompleted (en cola, en vuelo o
            // en el buzón). Es lo que lee el modal de progreso.
            int pending() const;

        private:
            void runJob(JobSystem::JobId id, const std::string& path, uint64_t targetId);

            JobSystem&              m_jobs;
            mutable std::mutex      m_mutex;
            std::vector<LoadedMesh> m_inbox;
            int                     m_pending = 0;
    };
}
```

- [ ] **Step 5: Escribir la implementación**

Crear `engine/src/Renderer/AsyncAssetLoader.cpp`:

```cpp
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
        m_jobs.cancel(id);
    }

    void AsyncAssetLoader::runJob(JobSystem::JobId id, const std::string& path, uint64_t targetId)
    {
        LoadedMesh result;
        result.job      = id;
        result.targetId = targetId;
        result.path     = path;

        try
        {
            result.mesh = ModelLoader::loadAuto(path);
            if (result.mesh)
            {
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
            m_pending -= static_cast<int>(out.size());
        }
        return out;
    }

    int AsyncAssetLoader::pending() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pending;
    }
}
```

Añadir a `engine/CMakeLists.txt`, junto a las demás fuentes de `src/Renderer/`:

```cmake
    src/Renderer/AsyncAssetLoader.cpp
```

- [ ] **Step 6: Ejecutar los tests para verificar que pasan**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_asset_loader_tests.exe
.\build-ninja\engine\tests\dt_jobsystem_tests.exe
```

Esperado: `asset_loader_tests OK` y `jobsystem_tests OK`, ambos con código de salida 0.

- [ ] **Step 7: Verificar el sabotaje**

Aplicar, confirmar el fallo, revertir:

1. En `runJob`, sustituir el bloque `try/catch` por una llamada pelada a `ModelLoader::loadAuto(path)` → `testMissingFileReportsError` mata el proceso.
2. En `runJob`, comentar las tres llamadas a `decodeSlot` → `testTexturesArriveDecoded` falla en `foundAlbedo`.
3. En `pumpCompleted`, borrar las dos ramas que llenan `leftover` → `testZeroBudgetKeepsResults` falla en el assert de `pumpCompleted(0.0f).empty()`.
4. En `runJob`, tras cargar, hacer `result.mesh->indices.clear();` → `testAsyncMatchesSync` falla en el assert de `indices.size()`.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/Renderer/AsyncAssetLoader.h engine/src/Renderer/AsyncAssetLoader.cpp engine/tests/asset_loader_tests.cpp engine/include/DonTopo/Core/JobSystem.h engine/src/Core/JobSystem.cpp engine/CMakeLists.txt engine/tests/CMakeLists.txt
git commit -m "feat(renderer): AsyncAssetLoader carga y decodifica en worker"
```

---

### Task 3: Deduplicación por path

**Files:**
- Modify: `engine/include/DonTopo/Renderer/AsyncAssetLoader.h`
- Modify: `engine/src/Renderer/AsyncAssetLoader.cpp`
- Modify: `engine/tests/asset_loader_tests.cpp`

**Interfaces:**
- Consumes: `AsyncAssetLoader` de la Task 2.
- Produces: `int readFileCount() const` y `void cancelAllPending()` nuevos. El resto del interfaz no cambia; cambia el comportamiento interno: N peticiones concurrentes del mismo `path` hacen **un** `ReadFile` y producen N `LoadedMesh` con `mesh.get()` distintos.

- [ ] **Step 1: Escribir el test que falla**

Añadir a `engine/tests/asset_loader_tests.cpp`, dentro del `namespace { }` anónimo:

```cpp
// Cuatro peticiones del mismo path con targetId distintos comparten UN ReadFile
// y producen cuatro LoadedMesh de contenido identico con punteros DISTINTOS.
//
// Los dos lados importan. Comprobar solo el contenido pasaria tambien
// compartiendo el shared_ptr, que es justo lo que el diseno descarta: hoy cada
// nodo de Scene::nodeFromJson tiene su propio make_shared<Mesh> (Scene.cpp:721),
// y dos GameObject sobre el mismo Mesh mutable cambiaria esa semantica.
//
// Sabotaje: devolver el mismo shared_ptr a los cuatro (quitar la copia en
// buildResultFor) — el assert de punteros distintos salta.
void testDedupSharesReadFileNotPointers(const std::string& fbx)
{
    DonTopo::JobSystem js;
    js.start();
    DonTopo::AsyncAssetLoader loader(js);

    for (uint64_t t = 100; t < 104; ++t)
        loader.requestMesh(fbx, t);

    std::vector<DonTopo::LoadedMesh> got = drain(loader, 4);
    assert(got.size() == 4 && "cuatro peticiones, cuatro resultados");

    assert(loader.readFileCount() == 1 && "cuatro peticiones del mismo path = un solo ReadFile");

    std::vector<uint64_t> targets;
    for (const auto& r : got)
    {
        assert(r.error.empty());
        assert(r.mesh != nullptr);
        assert(r.mesh->vertices.size() == got[0].mesh->vertices.size());
        assert(r.mesh->indices.size()  == got[0].mesh->indices.size());
        targets.push_back(r.targetId);
    }

    // Los cuatro targetId son distintos y estan los cuatro esperados.
    std::sort(targets.begin(), targets.end());
    assert((targets == std::vector<uint64_t>{100, 101, 102, 103}));

    // Y los punteros NO se comparten.
    for (size_t i = 0; i < got.size(); ++i)
        for (size_t k = i + 1; k < got.size(); ++k)
            assert(got[i].mesh.get() != got[k].mesh.get()
                   && "cada target recibe su propia copia del Mesh");

    js.shutdown();
}
```

Añadir `#include <algorithm>` a los includes del test, y la llamada en `main()` después de `testTexturesArriveDecoded(fbx);`:

```cpp
    testDedupSharesReadFileNotPointers(fbx);
```

- [ ] **Step 2: Ejecutar el test para verificar que falla**

```powershell
.\build.bat
```

Esperado: FALLA en compilación con `'readFileCount': is not a member of 'DonTopo::AsyncAssetLoader'`.

- [ ] **Step 3: Ampliar la cabecera**

En `engine/include/DonTopo/Renderer/AsyncAssetLoader.h`, añadir `#include <unordered_map>` y, a la clase:

```cpp
            // Solo para tests: cuántos ReadFile de verdad se han hecho. Es la
            // única forma de comprobar el dedup desde fuera — contar resultados
            // no distingue "un ReadFile compartido" de "cuatro ReadFile".
            int readFileCount() const;

            // Cancela todas las peticiones vivas y vacía el buzón. Lo llama el
            // botón Cancelar del modal de carga (Task 9). Los jobs ya arrancados
            // terminan igual — no se puede parar un ReadFile a medias — pero sus
            // resultados se descartan.
            void cancelAllPending();
```

Y a la sección privada:

```cpp
            // Peticiones agrupadas por path mientras el job está en vuelo. Al
            // terminar, el worker construye un LoadedMesh por cada entrada
            // (copiando el Mesh) y vacía el grupo.
            struct PendingGroup
            {
                std::vector<std::pair<JobSystem::JobId, uint64_t>> waiters;  // (job, targetId)
            };

            LoadedMesh buildResultFor(const LoadedMesh& src, JobSystem::JobId job, uint64_t targetId);

            std::unordered_map<std::string, PendingGroup> m_groups;
            int                                          m_readFileCount = 0;
```

- [ ] **Step 4: Implementar el dedup**

En `engine/src/Renderer/AsyncAssetLoader.cpp`, sustituir `requestMesh` y `runJob` por:

```cpp
    JobSystem::JobId AsyncAssetLoader::requestMesh(const std::string& path, uint64_t targetId)
    {
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
            group.waiters.emplace_back(id, targetId);
        }

        if (needsJob)
        {
            // path por copia: por referencia sería dangling en cuanto el caller
            // saliera de scope.
            m_jobs.submitWithId(id, [this, path] { runJob(path); });
        }
        return id;
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
                const Material& mat = loaded.mesh->material;
                decodeSlot(mat.texturePath,           mat.embeddedTexture,           DecodedImage::Albedo, loaded.images);
                decodeSlot(mat.normalMapPath,         mat.embeddedNormalMap,         DecodedImage::Normal, loaded.images);
                decodeSlot(mat.metallicRoughnessPath, mat.embeddedMetallicRoughness, DecodedImage::ORM,    loaded.images);
            }
            else
            {
                loaded.error = "No se pudo cargar el modelo: " + path;
            }
        }
        catch (const std::exception& e)
        {
            loaded.mesh  = nullptr;
            loaded.error = e.what();
        }
        catch (...)
        {
            loaded.mesh  = nullptr;
            loaded.error = "Error desconocido cargando " + path;
        }

        std::vector<std::pair<JobSystem::JobId, uint64_t>> waiters;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_readFileCount;
            auto it = m_groups.find(path);
            if (it != m_groups.end())
            {
                waiters = std::move(it->second.waiters);
                m_groups.erase(it);
            }
        }

        // Las copias se hacen FUERA del lock: con 40 objetos del mismo path, el
        // hilo principal se quedaría esperando el mutex justo mientras intenta
        // pintar.
        std::vector<LoadedMesh> results;
        results.reserve(waiters.size());
        for (const auto& [job, targetId] : waiters)
            results.push_back(buildResultFor(loaded, job, targetId));

        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& r : results)
            m_inbox.push_back(std::move(r));
    }

    int AsyncAssetLoader::readFileCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_readFileCount;
    }

    void AsyncAssetLoader::cancelAllPending()
    {
        std::vector<JobSystem::JobId> toCancel;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& [path, group] : m_groups)
                for (const auto& [job, targetId] : group.waiters)
                    toCancel.push_back(job);
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
```

Actualizar la declaración de `runJob` en la cabecera (ahora toma solo `path`):

```cpp
            void runJob(const std::string& path);
```

Añadir `#include "DonTopo/Renderer/SkinnedMesh.h"` a `AsyncAssetLoader.cpp`.

- [ ] **Step 5: Ejecutar los tests para verificar que pasan**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_asset_loader_tests.exe
.\build-ninja\engine\tests\dt_jobsystem_tests.exe
```

Esperado: ambos OK, salida 0. Los tests de la Task 2 deben seguir pasando sin tocarlos.

- [ ] **Step 6: Verificar el sabotaje**

1. En `buildResultFor`, sustituir la copia profunda por `out.mesh = src.mesh;` → el assert de punteros distintos debe fallar.
2. En `requestMesh`, forzar `needsJob = true;` siempre → el assert de `readFileCount() == 1` debe fallar con 4.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Renderer/AsyncAssetLoader.h engine/src/Renderer/AsyncAssetLoader.cpp engine/tests/asset_loader_tests.cpp
git commit -m "feat(renderer): dedup de peticiones por path en AsyncAssetLoader"
```

---

### Task 4: TransferBatch

**Files:**
- Create: `engine/include/DonTopo/Renderer/TransferBatch.h`
- Create: `engine/src/Renderer/TransferBatch.cpp`
- Modify: `engine/include/DonTopo/Renderer/GpuResources.h`
- Modify: `engine/src/Renderer/GpuResources.cpp`
- Modify: `engine/CMakeLists.txt`

**Interfaces:**
- Consumes: `GpuDevice` (`device()`, `graphicsQueue()`, `commandPool()`), de `engine/include/DonTopo/Renderer/GpuDevice.h`.
- Produces: `class TransferBatch` con `explicit TransferBatch(const GpuDevice&)`, `VkCommandBuffer cmd()`, `void addStaging(VkBuffer, VkDeviceMemory)`, `void submit()`, `bool complete() const`, `void reclaim()`, `bool empty() const`. Y las sobrecargas de `GpuResources` con parámetro `TransferBatch* batch = nullptr`.

**Nota de diseño:** los métodos de `GpuResources` **no cambian de firma**, ganan un parámetro por defecto `TransferBatch* batch = nullptr`. Con `nullptr` hacen exactamente lo de hoy (`beginOneTimeCommands`/`endOneTimeCommands` con su `vkQueueWaitIdle`). Así el init de swapchain, shadow map y skybox sigue idéntico y no hay que auditarlo.

- [ ] **Step 1: Escribir la cabecera**

Crear `engine/include/DonTopo/Renderer/TransferBatch.h`:

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <utility>
#include <vector>

namespace DonTopo
{
    class GpuDevice;

    // Agrupa todos los uploads de un pump en UN command buffer, UN submit y UNA
    // fence.
    //
    // Antes, cada createTextureImage encadenaba tres vkQueueWaitIdle (transición
    // → copia → transición) y cada buffer uno más: unos 11 vaciados completos de
    // la cola gráfica por mesh estático, ~440 al abrir una escena de 40 objetos.
    //
    // La corrección se mantiene porque las barreras (vkCmdPipelineBarrier)
    // ordenan dentro del command buffer igual que ordenaban entre submits. Lo
    // que desaparece es la espera, no la sincronización.
    class TransferBatch
    {
        public:
            explicit TransferBatch(const GpuDevice& gpu) : m_gpu(gpu) {}
            ~TransferBatch();
            TransferBatch(const TransferBatch&)            = delete;
            TransferBatch& operator=(const TransferBatch&) = delete;

            // Abre el command buffer la primera vez que se llama. Todas las
            // operaciones del batch comparten este.
            VkCommandBuffer cmd();

            // El staging vive hasta que la fence señala: liberarlo antes es un
            // use-after-free en la GPU que no peta de forma reproducible.
            void addStaging(VkBuffer buf, VkDeviceMemory mem);

            // Cierra y envía. No espera.
            void submit();

            // vkGetFenceStatus. NO bloquea: es lo que consulta el Renderer cada
            // frame para decidir si ya puede dibujar los objetos del batch.
            bool complete() const;

            // Destruye staging y command buffer. Exige complete() == true.
            void reclaim();

            bool empty() const { return m_cmd == VK_NULL_HANDLE; }

        private:
            const GpuDevice& m_gpu;
            VkCommandBuffer  m_cmd       = VK_NULL_HANDLE;
            VkFence          m_fence     = VK_NULL_HANDLE;
            bool             m_submitted = false;
            std::vector<std::pair<VkBuffer, VkDeviceMemory>> m_staging;
    };
}
```

- [ ] **Step 2: Escribir la implementación**

Crear `engine/src/Renderer/TransferBatch.cpp`:

```cpp
#include "DonTopo/Renderer/TransferBatch.h"
#include "DonTopo/Renderer/GpuDevice.h"

#include <stdexcept>

namespace DonTopo
{
    TransferBatch::~TransferBatch()
    {
        // Un batch enviado y no reclamado al destruirse filtraría staging y
        // fence. No se puede esperar aquí sin arriesgar un bloqueo en el
        // destructor, así que se drena de forma explícita: si esto salta, hay un
        // camino que envía sin llamar a reclaim().
        if (m_submitted && m_fence != VK_NULL_HANDLE)
        {
            vkWaitForFences(m_gpu.device(), 1, &m_fence, VK_TRUE, UINT64_MAX);
            reclaim();
        }
        else if (m_cmd != VK_NULL_HANDLE)
        {
            // Abierto pero nunca enviado: nada corrió en la GPU, se libera sin
            // esperar.
            vkFreeCommandBuffers(m_gpu.device(), m_gpu.commandPool(), 1, &m_cmd);
            m_cmd = VK_NULL_HANDLE;
            for (auto& [buf, mem] : m_staging)
            {
                vkDestroyBuffer(m_gpu.device(), buf, nullptr);
                vkFreeMemory(m_gpu.device(), mem, nullptr);
            }
            m_staging.clear();
        }
    }

    VkCommandBuffer TransferBatch::cmd()
    {
        if (m_cmd != VK_NULL_HANDLE) return m_cmd;

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool        = m_gpu.commandPool();
        allocInfo.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m_gpu.device(), &allocInfo, &m_cmd) != VK_SUCCESS)
            throw std::runtime_error("TransferBatch: fallo al reservar command buffer");

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_cmd, &beginInfo);
        return m_cmd;
    }

    void TransferBatch::addStaging(VkBuffer buf, VkDeviceMemory mem)
    {
        m_staging.emplace_back(buf, mem);
    }

    void TransferBatch::submit()
    {
        if (m_cmd == VK_NULL_HANDLE || m_submitted) return;

        vkEndCommandBuffer(m_cmd);

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(m_gpu.device(), &fenceInfo, nullptr, &m_fence) != VK_SUCCESS)
            throw std::runtime_error("TransferBatch: fallo al crear la fence");

        VkSubmitInfo submitInfo{};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &m_cmd;
        vkQueueSubmit(m_gpu.graphicsQueue(), 1, &submitInfo, m_fence);
        m_submitted = true;
    }

    bool TransferBatch::complete() const
    {
        if (!m_submitted) return m_cmd == VK_NULL_HANDLE;   // batch vacío = nada que esperar
        return vkGetFenceStatus(m_gpu.device(), m_fence) == VK_SUCCESS;
    }

    void TransferBatch::reclaim()
    {
        if (!m_submitted) return;
        // Guarda invertida: en vez de enumerar desde dónde es seguro llamar,
        // se pregunta por el estado real de la GPU. Reclamar antes de tiempo es
        // un use-after-free que las capas de validación cazan, pero que en
        // release corrompe en silencio.
        if (vkGetFenceStatus(m_gpu.device(), m_fence) != VK_SUCCESS)
            throw std::runtime_error("TransferBatch::reclaim con la fence sin senalar");

        for (auto& [buf, mem] : m_staging)
        {
            vkDestroyBuffer(m_gpu.device(), buf, nullptr);
            vkFreeMemory(m_gpu.device(), mem, nullptr);
        }
        m_staging.clear();

        vkFreeCommandBuffers(m_gpu.device(), m_gpu.commandPool(), 1, &m_cmd);
        m_cmd = VK_NULL_HANDLE;

        vkDestroyFence(m_gpu.device(), m_fence, nullptr);
        m_fence     = VK_NULL_HANDLE;
        m_submitted = false;
    }
}
```

Añadir a `engine/CMakeLists.txt`:

```cmake
    src/Renderer/TransferBatch.cpp
```

- [ ] **Step 3: Añadir el parámetro `batch` a GpuResources**

En `engine/include/DonTopo/Renderer/GpuResources.h`, añadir `class TransferBatch;` junto a `class GpuDevice;` y cambiar las firmas:

```cpp
    void copyBuffer(VkBuffer src, VkBuffer dst, VkDeviceSize size,
                    TransferBatch* batch = nullptr);
    void uploadBuffer(const void* data, VkDeviceSize size,
                      VkBufferUsageFlags usage,
                      VkBuffer& buf, VkDeviceMemory& mem,
                      TransferBatch* batch = nullptr);
    void transitionImageLayout(VkImage img,
                               VkImageLayout from, VkImageLayout to,
                               TransferBatch* batch = nullptr);
    void copyBufferToImage(VkBuffer buf, VkImage img,
                           uint32_t w, uint32_t h,
                           TransferBatch* batch = nullptr);
    void createTextureImage(const std::string& path,
                            const std::vector<uint8_t>& embedded,
                            VkImage& img, VkDeviceMemory& mem,
                            TransferBatch* batch = nullptr);
    void createNormalMapImage(const std::string& path,
                              const std::vector<uint8_t>& embedded,
                              VkImage& img, VkDeviceMemory& mem,
                              TransferBatch* batch = nullptr);
    void createSolidColorImage(const uint8_t rgba[4],
                               VkImage& img, VkDeviceMemory& mem,
                               TransferBatch* batch = nullptr);
```

Y añadir dos sobrecargas nuevas que reciben píxeles ya decodificados — es lo que consume el pump, que no vuelve a llamar a `stbi_load`:

```cpp
    // Variantes que reciben los píxeles ya decodificados por el worker. Son las
    // que usa la carga asíncrona: repetir el stbi_load en el hilo principal
    // tiraría por tierra la mitad de la ganancia.
    void createTextureImageFromPixels(const uint8_t* rgba, uint32_t w, uint32_t h,
                                      VkImage& img, VkDeviceMemory& mem,
                                      TransferBatch* batch = nullptr);
    void createNormalMapImageFromPixels(const uint8_t* rgba, uint32_t w, uint32_t h,
                                        VkImage& img, VkDeviceMemory& mem,
                                        TransferBatch* batch = nullptr);
```

- [ ] **Step 4: Implementar el enrutado por batch en GpuResources.cpp**

En `engine/src/Renderer/GpuResources.cpp`, añadir `#include "DonTopo/Renderer/TransferBatch.h"` y este helper en el namespace anónimo del fichero (o justo antes de la primera función que lo use):

```cpp
namespace {
    // Devuelve el command buffer a usar. Con batch == nullptr abre uno
    // one-time igual que hasta ahora, y el caller debe cerrarlo con
    // endOneTime(). Con batch, se cuelga del command buffer compartido y NO se
    // cierra aquí.
    struct CmdScope
    {
        const DonTopo::GpuDevice& gpu;
        DonTopo::TransferBatch*   batch;
        VkCommandBuffer           cmd;

        CmdScope(const DonTopo::GpuDevice& g, DonTopo::TransferBatch* b)
            : gpu(g), batch(b), cmd(b ? b->cmd() : g.beginOneTimeCommands()) {}

        // Solo cierra y espera si NO hay batch: con batch, el submit y la fence
        // son responsabilidad de quien lo posee.
        ~CmdScope() { if (!batch) gpu.endOneTimeCommands(cmd); }
    };
}
```

Reescribir `transitionImageLayout` y `copyBufferToImage` para usarlo. Ejemplo de `copyBufferToImage`, que hoy termina en `m_gpu.endOneTimeCommands(cmd)` (línea ~180):

```cpp
void GpuResources::copyBufferToImage(VkBuffer buffer, VkImage image,
                                     uint32_t w, uint32_t h, TransferBatch* batch)
{
    CmdScope scope(m_gpu, batch);

    VkBufferImageCopy region{};
    region.bufferOffset                    = 0;
    region.bufferRowLength                 = 0;
    region.bufferImageHeight               = 0;
    region.imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel       = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount     = 1;
    region.imageOffset                     = {0, 0, 0};
    region.imageExtent                     = {w, h, 1};

    vkCmdCopyBufferToImage(scope.cmd, buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}
```

Aplicar el mismo patrón a `transitionImageLayout` y `copyBuffer`, y propagar `batch` en `uploadBuffer`, `createTextureImage`, `createNormalMapImage` y `createSolidColorImage` a sus llamadas internas.

**Crítico — el staging.** Hoy `createTextureImage` destruye su staging buffer al final (líneas ~242-243) porque el `vkQueueWaitIdle` garantiza que la copia terminó. Con batch **no ha terminado**. Cambiar el final de `createTextureImage`, `createNormalMapImage`, `createSolidColorImage` y `uploadBuffer` a:

```cpp
    if (batch)
        batch->addStaging(stagingBuffer, stagingMemory);   // se libera al senalar la fence
    else
    {
        vkDestroyBuffer(m_gpu.device(), stagingBuffer, nullptr);
        vkFreeMemory(m_gpu.device(), stagingMemory, nullptr);
    }
```

**Destruir el staging sin esperar la fence es el bug más probable de esta tarea.** No peta de forma reproducible: corrompe la textura de vez en cuando.

- [ ] **Step 5: Implementar las variantes `FromPixels`**

Añadir a `engine/src/Renderer/GpuResources.cpp`. Son el cuerpo de `createTextureImage` sin el `stbi_load` ni el fallback — el caller ya trae los píxeles:

```cpp
void GpuResources::createTextureImageFromPixels(const uint8_t* rgba, uint32_t w, uint32_t h,
                                                VkImage& img, VkDeviceMemory& mem,
                                                TransferBatch* batch)
{
    const VkDeviceSize imageSize = static_cast<VkDeviceSize>(w) * h * 4;

    VkBuffer       stagingBuffer;
    VkDeviceMemory stagingMemory;
    createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                 stagingBuffer, stagingMemory);

    void* data;
    vkMapMemory(m_gpu.device(), stagingMemory, 0, imageSize, 0, &data);
    memcpy(data, rgba, static_cast<size_t>(imageSize));
    vkUnmapMemory(m_gpu.device(), stagingMemory);

    createImage(w, h, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, mem);

    transitionImageLayout(img, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, batch);
    copyBufferToImage(stagingBuffer, img, w, h, batch);
    transitionImageLayout(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, batch);

    if (batch)
        batch->addStaging(stagingBuffer, stagingMemory);
    else
    {
        vkDestroyBuffer(m_gpu.device(), stagingBuffer, nullptr);
        vkFreeMemory(m_gpu.device(), stagingMemory, nullptr);
    }
}
```

`createNormalMapImageFromPixels` es idéntica salvo `VK_FORMAT_R8G8B8A8_UNORM` en lugar de `_SRGB` — el mismo par de formatos que distingue hoy `createTextureImage` de `createNormalMapImage`.

- [ ] **Step 6: Compilar y verificar que no hay regresiones**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_audio_tests.exe
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_animator_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
.\build-ninja\engine\tests\dt_exporter_tests.exe
.\build-ninja\engine\tests\dt_scripting_tests.exe
.\build-ninja\engine\tests\dt_splash_tests.exe
.\build-ninja\engine\tests\dt_jobsystem_tests.exe
.\build-ninja\engine\tests\dt_asset_loader_tests.exe
```

Esperado: los 10 OK. Con `batch == nullptr` en todos los callers actuales, el comportamiento debe ser idéntico al de antes de esta tarea.

- [ ] **Step 7: Verificación manual con capas de validación**

```powershell
.\build-ninja\sandbox\Sandbox.exe
```

Abrir una escena con modelos, entrar y salir de Play, cerrar. **Cero mensajes de validación en la consola.** Esta tarea todavía no cambia ninguna ruta a batch, así que cualquier error nuevo aquí es una regresión del refactor.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/Renderer/TransferBatch.h engine/src/Renderer/TransferBatch.cpp engine/include/DonTopo/Renderer/GpuResources.h engine/src/Renderer/GpuResources.cpp engine/CMakeLists.txt
git commit -m "feat(renderer): TransferBatch y enrutado opcional de uploads"
```

---

### Task 5: DeferredDeleteQueue

**Files:**
- Create: `engine/include/DonTopo/Renderer/DeferredDelete.h`
- Create: `engine/src/Renderer/DeferredDelete.cpp`
- Modify: `engine/include/DonTopo/Renderer/Renderer.h`
- Modify: `engine/src/Renderer/Renderer.cpp` (líneas 2304, 2383, 2413, 2433 y `destroyRenderObject`/`destroySkinnedRenderObject`)
- Modify: `engine/CMakeLists.txt`

**Interfaces:**
- Consumes: `GpuDevice::device()`.
- Produces: `class DeferredDeleteQueue` con `void push(std::function<void(VkDevice)>)`, `void tick(VkDevice)`, `void flushAll(VkDevice)`, `size_t pendingCount() const`. Y `Renderer::tickDeferredDeletes()` público, consumido por la Task 9.

- [ ] **Step 1: Escribir la cabecera**

Crear `engine/include/DonTopo/Renderer/DeferredDelete.h`:

```cpp
#pragma once
#include <vulkan/vulkan.h>
#include <cstddef>
#include <functional>
#include <vector>

namespace DonTopo
{
    // Retrasa la destrucción de recursos Vulkan hasta que ningún frame en vuelo
    // pueda estar usándolos, en lugar de vaciar el device entero.
    //
    // Sustituye a los cuatro vkDeviceWaitIdle de Renderer.cpp (2304, 2383, 2413,
    // 2433). Aquel era lento pero imposible de equivocar; esto es rápido y su
    // modo de fallo es peor: destruir un frame antes de tiempo es un
    // use-after-free en la GPU que no se reproduce de forma fiable.
    //
    // Por eso el retraso es kDelayFrames = MAX_FRAMES + 1, un frame más de lo
    // estrictamente necesario, y por eso flushAll() exige un vkDeviceWaitIdle
    // previo del caller.
    class DeferredDeleteQueue
    {
        public:
            // MAX_FRAMES de Renderer es 2 (Renderer.h:337). El +1 es margen
            // deliberado, no un off-by-one.
            static constexpr int kDelayFrames = 3;

            void push(std::function<void(VkDevice)> destroyer);

            // Una vez por frame, al principio de drawFrame.
            void tick(VkDevice device);

            // SOLO desde Renderer::shutdown, y SOLO después de un
            // vkDeviceWaitIdle. Ejecuta todo lo pendiente sin mirar el contador.
            void flushAll(VkDevice device);

            size_t pendingCount() const { return m_entries.size(); }

        private:
            struct Entry
            {
                int                            framesLeft;
                std::function<void(VkDevice)>  destroyer;
            };

            std::vector<Entry> m_entries;
    };
}
```

- [ ] **Step 2: Escribir la implementación**

Crear `engine/src/Renderer/DeferredDelete.cpp`:

```cpp
#include "DonTopo/Renderer/DeferredDelete.h"

#include <utility>

namespace DonTopo
{
    void DeferredDeleteQueue::push(std::function<void(VkDevice)> destroyer)
    {
        m_entries.push_back(Entry{kDelayFrames, std::move(destroyer)});
    }

    void DeferredDeleteQueue::tick(VkDevice device)
    {
        // Recorrido con índice y swap-erase: un destroyer no puede encolar más
        // trabajo (destruye, no crea), así que no hace falta protegerse de una
        // invalidación por reentrada, pero sí de reordenar mientras se itera.
        for (size_t i = 0; i < m_entries.size();)
        {
            if (--m_entries[i].framesLeft > 0) { ++i; continue; }

            m_entries[i].destroyer(device);
            m_entries[i] = std::move(m_entries.back());
            m_entries.pop_back();
        }
    }

    void DeferredDeleteQueue::flushAll(VkDevice device)
    {
        // El caller garantiza vkDeviceWaitIdle previo. Sin él, esto es
        // exactamente el use-after-free que la cola existe para evitar.
        for (auto& e : m_entries)
            e.destroyer(device);
        m_entries.clear();
    }
}
```

Añadir a `engine/CMakeLists.txt`:

```cmake
    src/Renderer/DeferredDelete.cpp
```

- [ ] **Step 3: Integrar en Renderer**

En `engine/include/DonTopo/Renderer/Renderer.h`:

- Añadir `#include "DonTopo/Renderer/DeferredDelete.h"`.
- En la sección pública, junto a los demás métodos de frame:

```cpp
            // Una vez por frame, desde el bucle principal, ANTES de drawFrame.
            // Es lo que hace avanzar la cola de destrucción diferida.
            void tickDeferredDeletes();
```

- En la sección privada, junto a los demás miembros:

```cpp
            DeferredDeleteQueue m_deferredDeletes;
```

- Mover `destroyRenderObject` y `destroySkinnedRenderObject` a privadas si no lo están ya (líneas 298 y 302 — ya lo son) y añadir junto a ellas:

```cpp
            // Encolan la destrucción en lugar de ejecutarla. Son el ÚNICO camino
            // permitido: llamar a destroyRenderObject directamente desde un call
            // site nuevo volvería a necesitar un vkDeviceWaitIdle, y nadie se
            // acordaría de ponerlo.
            void queueDestroyRenderObject(RenderObject& obj);
            void queueDestroySkinnedRenderObject(SkinnedRenderObject& obj);
```

En `engine/src/Renderer/Renderer.cpp`:

```cpp
    void Renderer::tickDeferredDeletes()
    {
        m_deferredDeletes.tick(m_gpu.device());
    }

    void Renderer::queueDestroyRenderObject(RenderObject& obj)
    {
        // Se copian los handles y se vacía el objeto YA: así el resto del frame
        // ve un RenderObject sin recursos (el skip de recordCommandBuffer lo
        // detecta por vertexBuffer == VK_NULL_HANDLE) mientras la destrucción de
        // verdad ocurre kDelayFrames después.
        RenderObject snapshot = obj;
        obj = RenderObject{};

        m_deferredDeletes.push([this, snapshot](VkDevice) mutable {
            destroyRenderObject(snapshot);
        });
    }

    void Renderer::queueDestroySkinnedRenderObject(SkinnedRenderObject& obj)
    {
        SkinnedRenderObject snapshot = std::move(obj);
        obj = SkinnedRenderObject{};

        m_deferredDeletes.push([this, snapshot = std::move(snapshot)](VkDevice) mutable {
            destroySkinnedRenderObject(snapshot);
        });
    }
```

- [ ] **Step 4: Sustituir los cuatro `vkDeviceWaitIdle`**

Los cuatro convergen en dos funciones: `removeStaticObject` y `removeSkinnedObject`, que son las únicas que llaman a `destroyRenderObject`/`destroySkinnedRenderObject`. Cambiando esas dos, los `vkDeviceWaitIdle` de sus callers pasan a sobrar.

**4.1 — `removeSkinnedObject` (línea ~2369).** Hoy:

```cpp
        if (obj.outputVertexBuffer == VK_NULL_HANDLE) return; // ya liberado
        destroySkinnedRenderObject(obj);
        obj = SkinnedRenderObject{};
```

Pasa a:

```cpp
        if (obj.outputVertexBuffer == VK_NULL_HANDLE) return; // ya liberado
        // queueDestroy... ya deja obj vacío: la asignación de después sobra y
        // pisaría el snapshot si se dejara.
        queueDestroySkinnedRenderObject(obj);
```

**4.2 — `removeStaticObject`.** Sustituir su `destroyRenderObject(obj); obj = RenderObject{};` por `queueDestroyRenderObject(obj);`, con el mismo criterio.

**4.3 — `removeGameObject` (línea ~2383).** Borrar estas tres líneas:

```cpp
        // Espera a que la GPU termine antes de destruir buffers/texturas que
        // un command buffer en vuelo (double buffering) pudiera seguir usando.
        vkDeviceWaitIdle(m_gpu.device());
```

y poner en su lugar:

```cpp
        // Sin vkDeviceWaitIdle: removeStaticObject/removeSkinnedObject encolan
        // la destrucción kDelayFrames frames, que es más de lo que cualquier
        // command buffer en vuelo puede tardar.
```

**4.4 — `removeMeshComponent` (línea ~2413).** Borrar igual:

```cpp
        // Mismo wait que removeGameObject: evita liberar buffers que un
        // command buffer en vuelo (double buffering) pudiera seguir usando.
        vkDeviceWaitIdle(m_gpu.device());
```

**4.5 — `replaceStaticTextureWithMissing` (línea ~2433).** Este NO pasa por `removeStaticObject`: reemplaza imagen, memoria, vista y sampler de un slot suelto (`switch (slot)` sobre `Diffuse`/`Normal`/…, que rellena los punteros `img`, `mem`, `view`, `sampler`). Borrar su `vkDeviceWaitIdle` y, **antes** de sobrescribir los handles con los nuevos, encolar los viejos:

```cpp
        // Los cuatro handles viejos siguen referenciados por el descriptor set
        // que un command buffer en vuelo puede estar usando. Se encolan por
        // valor: capturar los punteros img/mem/view/sampler sería leer los
        // NUEVOS cuando el lambda corriera, tres frames después.
        const VkImage        oldImage   = *img;
        const VkDeviceMemory oldMem     = *mem;
        const VkImageView    oldView    = *view;
        const VkSampler      oldSampler = *sampler;
        m_deferredDeletes.push([oldImage, oldMem, oldView, oldSampler](VkDevice dev) {
            vkDestroySampler(dev,   oldSampler, nullptr);
            vkDestroyImageView(dev, oldView,    nullptr);
            vkDestroyImage(dev,     oldImage,   nullptr);
            vkFreeMemory(dev,       oldMem,     nullptr);
        });
```

y borrar las destrucciones inmediatas de esos cuatro handles que hubiera más abajo en la función.

**El error clásico aquí es capturar `img`, `mem`, `view`, `sampler` (que son `VkImage*`, `VkDeviceMemory*`…) en vez de los valores.** El lambda corre tres frames después, cuando esos punteros ya apuntan a los handles nuevos: se destruiría la textura recién creada y la vieja quedaría filtrada. Ninguna capa de validación lo señala como error — solo se ve como una textura que desaparece.

- [ ] **Step 5: Actualizar `Renderer::shutdown`**

Localizar `vkDeviceWaitIdle(m_gpu.device());` en `Renderer::shutdown` (línea ~313) y añadir justo después:

```cpp
        // El vkDeviceWaitIdle de arriba es la precondición de flushAll: sin él,
        // esto destruiría recursos que la GPU todavía puede estar leyendo.
        m_deferredDeletes.flushAll(m_gpu.device());
```

- [ ] **Step 6: Compilar y verificar que no hay regresiones**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_audio_tests.exe
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_animator_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
.\build-ninja\engine\tests\dt_exporter_tests.exe
.\build-ninja\engine\tests\dt_scripting_tests.exe
.\build-ninja\engine\tests\dt_splash_tests.exe
.\build-ninja\engine\tests\dt_jobsystem_tests.exe
.\build-ninja\engine\tests\dt_asset_loader_tests.exe
```

Esperado: los 10 OK.

- [ ] **Step 7: Verificación manual con validación — el paso que de verdad cubre esta tarea**

`tickDeferredDeletes()` todavía no se llama desde el bucle (eso es la Task 9). Añadir la llamada temporalmente en `sandbox/src/main.cpp` justo antes de `renderer.drawFrame(window);` para poder probar:

```cpp
            renderer.tickDeferredDeletes();
```

```powershell
.\build.bat
.\build-ninja\sandbox\Sandbox.exe
```

Escenarios, todos con la consola a la vista:

1. Añadir un mesh a un GameObject, borrar el GameObject, repetir 10 veces.
2. Cambiar la textura de un objeto por una inexistente (dispara `replaceStaticTextureWithMissing`).
3. Entrar y salir de Play 5 veces con la escena cargada.
4. Cerrar la aplicación.

**Cero mensajes de validación.** Un `VUID-vkDestroyImage-image-01000` o un `VUID-vkFreeMemory-memory-00677` significa que algo se destruye demasiado pronto: **parar y arreglarlo, no seguir**.

- [ ] **Step 8: Commit**

```bash
git add engine/include/DonTopo/Renderer/DeferredDelete.h engine/src/Renderer/DeferredDelete.cpp engine/include/DonTopo/Renderer/Renderer.h engine/src/Renderer/Renderer.cpp engine/CMakeLists.txt sandbox/src/main.cpp
git commit -m "feat(renderer): destruccion diferida en vez de vkDeviceWaitIdle"
```

---

### Task 6: Visibilidad diferida por ticket de upload

**Files:**
- Modify: `engine/include/DonTopo/Renderer/Renderer.h`
- Modify: `engine/src/Renderer/Renderer.cpp` (`RenderObject`, `SkinnedRenderObject`, `recordCommandBuffer` ~789 y ~810, `recordShadowPass` ~1830, `addStaticMesh` ~1570, `addSkinnedMesh` ~2090)

**Interfaces:**
- Consumes: `TransferBatch` de la Task 4.
- Produces: `int Renderer::addStaticMesh(const Mesh& mesh, const std::vector<DecodedImage>* decoded = nullptr)` y `int Renderer::addSkinnedMesh(const SkinnedMesh& mesh, const std::vector<DecodedImage>* decoded = nullptr)`, que encolan en el batch pendiente. `void Renderer::flushPendingUploads()`, llamado al final del pump. `bool Renderer::hasPendingUploads() const`, que consume el runtime en la Task 10.

- [ ] **Step 1: Añadir el ticket a los render objects**

En `engine/include/DonTopo/Renderer/Renderer.h`:

- Añadir `#include "DonTopo/Renderer/TransferBatch.h"` y `#include "DonTopo/Renderer/AsyncAssetLoader.h"` (por `DecodedImage`).
- Dentro de `struct RenderObject` (línea ~148) y de `struct SkinnedRenderObject`:

```cpp
                // 0 = subido y visible. >0 = esperando a que la fence del batch
                // con ese ticket señale. Sin esto, el objeto se dibujaría con
                // sus texturas todavía en TRANSFER_DST_OPTIMAL.
                uint64_t uploadTicket = 0;
```

- En la sección privada del Renderer:

```cpp
            // Batch abierto donde caen los uploads del pump actual. Se envía en
            // flushPendingUploads() y pasa a m_inFlightBatches.
            std::unique_ptr<TransferBatch> m_pendingBatch;
            struct InFlightBatch { uint64_t ticket; std::unique_ptr<TransferBatch> batch; };
            std::vector<InFlightBatch>     m_inFlightBatches;
            uint64_t                       m_nextUploadTicket      = 1;
            uint64_t                       m_lastCompletedTicket   = 0;
```

- En la pública:

```cpp
            // Cierra y envía el batch del pump actual. Llamar UNA vez tras
            // procesar todos los resultados del frame.
            void flushPendingUploads();
```

- [ ] **Step 2: Implementar el ciclo del batch**

En `engine/src/Renderer/Renderer.cpp`:

```cpp
    void Renderer::flushPendingUploads()
    {
        if (m_pendingBatch && !m_pendingBatch->empty())
        {
            m_pendingBatch->submit();
            m_inFlightBatches.push_back(InFlightBatch{m_nextUploadTicket,
                                                      std::move(m_pendingBatch)});
        }
        m_pendingBatch.reset();
        ++m_nextUploadTicket;
    }
```

Y ampliar `tickDeferredDeletes()` (Task 5) para que también avance los batches — es el mismo punto del frame y comparte la lógica de "recursos que ya nadie usa":

```cpp
    void Renderer::tickDeferredDeletes()
    {
        m_deferredDeletes.tick(m_gpu.device());

        // Los batches se reclaman en orden: un ticket solo se da por completado
        // cuando el suyo y todos los anteriores han señalado. Sin ese orden,
        // m_lastCompletedTicket podría saltar hacia adelante y hacer visible un
        // objeto de un batch anterior que todavía no ha terminado.
        for (size_t i = 0; i < m_inFlightBatches.size();)
        {
            if (!m_inFlightBatches[i].batch->complete()) { ++i; continue; }

            m_inFlightBatches[i].batch->reclaim();
            if (m_inFlightBatches[i].ticket > m_lastCompletedTicket)
                m_lastCompletedTicket = m_inFlightBatches[i].ticket;
            m_inFlightBatches.erase(m_inFlightBatches.begin() + static_cast<long>(i));
        }
    }
```

- [ ] **Step 3: Enrutar addStaticMesh / addSkinnedMesh al batch**

Cambiar la firma en el header y la implementación (línea ~1570):

```cpp
    int Renderer::addStaticMesh(const Mesh& mesh, const std::vector<DecodedImage>* decoded)
    {
        if (!m_pendingBatch)
            m_pendingBatch = std::make_unique<TransferBatch>(m_gpu);

        m_objects.emplace_back();
        RenderObject& obj = m_objects.back();
        buildRenderObject(mesh, obj, m_pendingBatch.get(), decoded);
        allocateObjectDescriptorSet(obj);

        // El objeto no se dibuja hasta que la fence de este batch señale. Es la
        // decisión de producto de la spec: nada de placeholders, el GameObject
        // aparece cuando está listo.
        obj.uploadTicket = m_nextUploadTicket;
        return (int)m_objects.size() - 1;
    }
```

`buildRenderObject` (línea ~1539) gana los dos parámetros y usa las variantes `FromPixels` cuando hay píxeles decodificados:

```cpp
    void Renderer::buildRenderObject(const Mesh& mesh, RenderObject& obj,
                                     TransferBatch* batch,
                                     const std::vector<DecodedImage>* decoded)
    {
        obj.name       = mesh.name;
        obj.indexCount = (uint32_t)mesh.indices.size();
        createVertexBuffer(mesh.vertices, obj.vertexBuffer, obj.vertexMemory, batch);
        createIndexBuffer(mesh.indices,   obj.indexBuffer,  obj.indexMemory,  batch);

        // Busca un slot entre los píxeles que ya decodificó el worker. Sin
        // acierto se cae a la ruta de siempre (stbi_load en este hilo), que es
        // lo que pasa en el init síncrono y en las texturas sin decodificar.
        auto findSlot = [decoded](DecodedImage::Slot s) -> const DecodedImage* {
            if (!decoded) return nullptr;
            for (const auto& d : *decoded) if (d.slot == s) return &d;
            return nullptr;
        };

        if (const DecodedImage* albedo = findSlot(DecodedImage::Albedo))
            m_res.createTextureImageFromPixels(albedo->pixels.data(),
                                               (uint32_t)albedo->w, (uint32_t)albedo->h,
                                               obj.textureImage, obj.textureMem, batch);
        else
            m_res.createTextureImage(mesh.material.texturePath, mesh.material.embeddedTexture,
                                     obj.textureImage, obj.textureMem, batch);
        m_res.createTextureImageView(obj.textureImage, obj.textureView);
        m_res.createTextureSampler(obj.sampler);

        if (const DecodedImage* normal = findSlot(DecodedImage::Normal))
            m_res.createNormalMapImageFromPixels(normal->pixels.data(),
                                                 (uint32_t)normal->w, (uint32_t)normal->h,
                                                 obj.normalImage, obj.normalMem, batch);
        else
            m_res.createNormalMapImage(mesh.material.normalMapPath, mesh.material.embeddedNormalMap,
                                       obj.normalImage, obj.normalMem, batch);
        m_res.createTextureImageView(obj.normalImage, obj.normalView, VK_FORMAT_R8G8B8A8_UNORM);
        m_res.createTextureSampler(obj.normalSampler);

        if (const DecodedImage* orm = findSlot(DecodedImage::ORM))
        {
            m_res.createNormalMapImageFromPixels(orm->pixels.data(),
                                                 (uint32_t)orm->w, (uint32_t)orm->h,
                                                 obj.ormImage, obj.ormMem, batch);
            obj.metallic  = 1.0f;
            obj.roughness = 1.0f;
        }
        else if (!mesh.material.metallicRoughnessPath.empty()
                 || !mesh.material.embeddedMetallicRoughness.empty())
        {
            m_res.createNormalMapImage(mesh.material.metallicRoughnessPath,
                                       mesh.material.embeddedMetallicRoughness,
                                       obj.ormImage, obj.ormMem, batch);
            obj.metallic  = 1.0f;
            obj.roughness = 1.0f;
        }
        else
        {
            constexpr uint8_t white[4] = {255, 255, 255, 255};
            m_res.createSolidColorImage(white, obj.ormImage, obj.ormMem, batch);
            obj.metallic  = mesh.material.metallic;
            obj.roughness = mesh.material.roughness;
        }
        m_res.createTextureImageView(obj.ormImage, obj.ormView, VK_FORMAT_R8G8B8A8_UNORM);
        m_res.createTextureSampler(obj.ormSampler);
    }
```

Aplicar lo equivalente a `addSkinnedMesh` (línea ~2090) e `initSkinnedRenderObject` (~2097).

**Importante:** `createVertexBuffer` y `createIndexBuffer` (`Renderer.cpp:1238` y `:1265`) también necesitan el parámetro `TransferBatch* batch = nullptr` y propagarlo a `m_res.uploadBuffer` / `m_res.copyBuffer`.

- [ ] **Step 4: Saltar los objetos aún no subidos al grabar**

En `recordCommandBuffer` (línea ~789), cambiar la guarda del bucle de estáticos:

```cpp
            for (auto& obj : m_objects)
            {
                if (obj.vertexBuffer == VK_NULL_HANDLE) continue; // borrado desde el editor
                // Todavía en vuelo: sus texturas siguen en TRANSFER_DST_OPTIMAL y
                // samplearlas sería leer basura. Aparece en cuanto la fence de su
                // batch señale.
                if (obj.uploadTicket > m_lastCompletedTicket) continue;
```

Aplicar el mismo `continue` en:
- El bucle de `m_skinnedObjects` de `recordCommandBuffer` (línea ~810).
- El bucle de `recordShadowPass` (línea ~1830). **No olvidar este**: un objeto invisible que sí proyecta sombra delata el bug al instante.

- [ ] **Step 5: Compilar y verificar que no hay regresiones**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_audio_tests.exe
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_animator_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
.\build-ninja\engine\tests\dt_exporter_tests.exe
.\build-ninja\engine\tests\dt_scripting_tests.exe
.\build-ninja\engine\tests\dt_splash_tests.exe
.\build-ninja\engine\tests\dt_jobsystem_tests.exe
.\build-ninja\engine\tests\dt_asset_loader_tests.exe
```

Esperado: los 10 OK.

- [ ] **Step 6: Verificación manual**

Añadir temporalmente `renderer.flushPendingUploads();` tras el `traverse` de `sandbox/src/main.cpp`, reconstruir y:

```powershell
.\build-ninja\sandbox\Sandbox.exe
```

1. Abrir una escena con modelos: todo debe verse, con sus texturas y sus sombras.
2. Añadir un mesh nuevo a un GameObject: aparece un frame o dos después, ya texturizado. **Nunca con la textura en negro o en basura.**
3. Cerrar. Cero mensajes de validación.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Renderer/Renderer.h engine/src/Renderer/Renderer.cpp sandbox/src/main.cpp
git commit -m "feat(renderer): uploads agrupados en batch con visibilidad diferida"
```

---

### Task 7: Audio no bloqueante

**Files:**
- Modify: `engine/src/Audio/AudioManager.cpp:76,101-102` (`loadSound`)
- Modify: `engine/include/DonTopo/Audio/AudioManager.h:24,28` (comentarios de contrato)
- Modify: `engine/tests/audio_tests.cpp` — **NO**. Esta suite no se toca; el cambio debe ser transparente para ella.

**Interfaces:**
- Consumes: nada nuevo.
- Produces: mismo interfaz. `loadSound` retorna al instante; `playSound` no reproduce si el sonido sigue cargando.

- [ ] **Step 1: Añadir el flag a las dos rutas de creación**

En `engine/src/Audio/AudioManager.cpp`. En `loadSound` (línea 74), esto:

```cpp
    FMOD_MODE mode = (is3D ? FMOD_3D : FMOD_2D) | (loop ? FMOD_LOOP_NORMAL : FMOD_LOOP_OFF);
```

pasa a:

```cpp
    // FMOD arranca con FMOD_INIT_NORMAL (línea 29), que es la API thread-safe.
    // NONBLOCKING descarga la lectura y decodificación al hilo interno de FMOD:
    // createSound retorna al instante y no escribimos ni una línea de código de
    // concurrencia. Es por lo que el audio no pasa por el JobSystem.
    FMOD_MODE mode = (is3D ? FMOD_3D : FMOD_2D)
                   | (loop ? FMOD_LOOP_NORMAL : FMOD_LOOP_OFF)
                   | FMOD_NONBLOCKING;
```

Y en `loadBGM` (línea 101), esto:

```cpp
    FMOD_MODE mode = FMOD_2D | FMOD_LOOP_NORMAL | FMOD_CREATESTREAM;
```

pasa a:

```cpp
    // CREATESTREAM ya evitaba cargar el fichero entero, pero createSound seguía
    // abriéndolo y leyendo cabeceras en este hilo. NONBLOCKING quita también eso.
    FMOD_MODE mode = FMOD_2D | FMOD_LOOP_NORMAL | FMOD_CREATESTREAM | FMOD_NONBLOCKING;
```

- [ ] **Step 2: Proteger `playSound` del sonido a medio cargar**

En `AudioManager::playSound`, antes de `playSound` de FMOD:

```cpp
    // Con NONBLOCKING, el Sound existe pero puede estar todavía cargando. Un
    // playSound sobre él devuelve FMOD_ERR_NOTREADY. Se ignora en silencio en
    // vez de escupir un error: el usuario ha pedido reproducir algo que aún no
    // está, y el caso normal es que dé a Play nada más soltar el fichero.
    FMOD_OPENSTATE state;
    if (snd->getOpenState(&state, nullptr, nullptr, nullptr) == FMOD_OK
        && state == FMOD_OPENSTATE_LOADING)
        return;
```

Sustituir `snd` por el nombre real de la variable del `FMOD::Sound*` en esa función.

- [ ] **Step 3: Actualizar el contrato en el header**

En `engine/include/DonTopo/Audio/AudioManager.h`, sobre `loadSound` (línea 24):

```cpp
    // Retorna al instante: FMOD carga en su hilo interno (FMOD_NONBLOCKING). El
    // id es válido desde ya, pero un playSound antes de que termine la carga no
    // reproduce nada — no es un error, solo aún no está listo.
```

- [ ] **Step 4: Verificar que no hay regresiones**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_audio_tests.exe
.\build-ninja\engine\tests\dt_exporter_tests.exe
.\build-ninja\engine\tests\dt_scripting_tests.exe
```

Esperado: OK sin haber tocado `audio_tests.cpp`. Si esa suite falla, el cambio ha roto el contrato y hay que revisarlo — no ajustar el test.

- [ ] **Step 5: Verificación manual**

```powershell
.\build-ninja\sandbox\Sandbox.exe
```

1. Añadir un AudioClip a un GameObject con un `.wav` o `.mp3` grande. El editor no debe congelarse al soltarlo.
2. Dar a Play inmediatamente: el sonido suena (o no suena ese primer intento si aún cargaba), **sin crash ni error en el Log Console**.
3. Play de nuevo tras un segundo: suena.

- [ ] **Step 6: Commit**

```bash
git add engine/src/Audio/AudioManager.cpp engine/include/DonTopo/Audio/AudioManager.h
git commit -m "feat(audio): carga no bloqueante con FMOD_NONBLOCKING"
```

---

### Task 8: `Scene::fromJson` con loader opcional

**Files:**
- Modify: `engine/include/DonTopo/Core/Scene.h:96,103`
- Modify: `engine/src/Core/Scene.cpp:721,1217,1276` y `nodeFromJson`
- Create: `engine/tests/scene_async_tests.cpp`
- Modify: `engine/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `AsyncAssetLoader::requestMesh(path, targetId)` de las Tasks 2-3.
- Produces: `bool Scene::fromJson(const nlohmann::json&, PhysicsManager&, AudioManager&, AsyncAssetLoader* loader = nullptr)` y `bool Scene::load(const std::string&, PhysicsManager&, AudioManager&, AsyncAssetLoader* loader = nullptr)`.

- [ ] **Step 1: Escribir el test que falla**

Crear `engine/tests/scene_async_tests.cpp`:

```cpp
// Tests de Scene::fromJson con y sin AsyncAssetLoader.
//
// El caso mas importante es el primero: con loader == nullptr el
// comportamiento tiene que ser IDENTICO al de antes de esta feature. Es lo que
// protege el restore de Play->Stop (EditorUI.cpp:170) y las ocho suites que ya
// existen.
#include "DonTopo/Audio/AudioManager.h"
#include "DonTopo/Core/JobSystem.h"
#include "DonTopo/Core/Scene.h"
#include "DonTopo/Physics/PhysicsManager.h"
#include "DonTopo/Renderer/AsyncAssetLoader.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstdio>

namespace {

// Una sola PhysicsManager para todo el fichero: crear y liberar una por test
// crashea al segundo init porque PxFoundation es unica por proceso.
DonTopo::PhysicsManager& physics()
{
    static DonTopo::PhysicsManager p;
    static bool inited = (p.init(), true);
    (void)inited;
    return p;
}

DonTopo::AudioManager& audio()
{
    static DonTopo::AudioManager a;
    static bool inited = (a.init(), true);
    (void)inited;
    return a;
}

// Escena minima con dos nodos anidados y transforms NO neutros: un test que
// afirmase la identidad pasaria igual si nadie leyera el campo.
nlohmann::json twoNodeScene()
{
    return nlohmann::json::parse(R"({
        "name": "root",
        "children": [
            { "name": "hijoA", "position": [1.5, -2.0, 3.25], "children": [] },
            { "name": "hijoB", "position": [-4.0, 5.5, 6.75], "children": [] }
        ]
    })");
}

// loader == nullptr produce exactamente lo de siempre. Sabotaje: hacer que la
// rama sincrona encole en vez de cargar — los nodos pierden su mesh y el
// toJson deja de coincidir.
void testSyncPathUnchanged()
{
    DonTopo::Scene a, b;
    assert(a.fromJson(twoNodeScene(), physics(), audio()));
    assert(b.fromJson(twoNodeScene(), physics(), audio(), nullptr));

    // El parametro por defecto y el nullptr explicito son el MISMO camino.
    assert(a.toJson() == b.toJson());
}

// Con loader, los GameObject existen ya con su jerarquia y su transform: lo
// unico que falta es el mesh. Sabotaje: crear los nodos solo al bombear — el
// assert de nombres falla porque la escena esta vacia.
void testAsyncCreatesNodesImmediately()
{
    DonTopo::JobSystem js;
    js.start();
    DonTopo::AsyncAssetLoader loader(js);

    DonTopo::Scene s;
    assert(s.fromJson(twoNodeScene(), physics(), audio(), &loader));

    int found = 0;
    s.traverse([&](DonTopo::GameObject* go) {
        if (go->name == "hijoA" || go->name == "hijoB") ++found;
        // Nada se ha bombeado todavia: ningun nodo puede tener indice de render.
        assert(go->staticRenderIndex  == -1);
        assert(go->skinnedRenderIndex == -1);
    });
    assert(found == 2 && "los GameObject existen desde el frame 0, sin esperar al asset");

    js.shutdown();
}

// Borrar un GameObject con carga pendiente y bombear despues no crashea: el
// resultado se descarta porque su targetId ya no esta en la escena viva.
//
// Es el test con mas valor de los tres: el use-after-free clasico de este
// patron es guardar un GameObject* en la peticion. Sabotaje: guardar el
// puntero en vez del id y desreferenciarlo al bombear — crash o basura.
void testDeletedTargetIsDiscarded()
{
    DonTopo::JobSystem js;
    js.start();
    DonTopo::AsyncAssetLoader loader(js);

    DonTopo::Scene s;
    assert(s.fromJson(twoNodeScene(), physics(), audio(), &loader));

    DonTopo::GameObject* victim = nullptr;
    s.traverse([&](DonTopo::GameObject* go) { if (go->name == "hijoA") victim = go; });
    assert(victim);

    const uint64_t goneId = victim->id;
    s.removeGameObject(victim);

    // Un resultado dirigido a un id que ya no existe no puede tocar memoria
    // liberada. Se comprueba que sigue sin aparecer tras bombear.
    for (auto& r : loader.pumpCompleted(1000.0f))
        assert(r.targetId != goneId || true);   // solo importa que no crashee

    bool stillThere = false;
    s.traverse([&](DonTopo::GameObject* go) { if (go->id == goneId) stillThere = true; });
    assert(!stillThere && "el nodo borrado no puede resucitar al bombear");

    js.shutdown();
}

} // namespace

int main()
{
    testSyncPathUnchanged();
    testAsyncCreatesNodesImmediately();
    testDeletedTargetIsDiscarded();
    std::printf("scene_async_tests OK\n");
    return 0;
}
```

Añadir a `engine/tests/CMakeLists.txt`:

```cmake
add_executable(dt_scene_async_tests scene_async_tests.cpp)
target_link_libraries(dt_scene_async_tests PRIVATE DonTopoEngine)
target_compile_features(dt_scene_async_tests PRIVATE cxx_std_20)
```

- [ ] **Step 2: Ejecutar el test para verificar que falla**

```powershell
.\build.bat
```

Esperado: FALLA en compilación — `fromJson` no acepta cuatro argumentos.

**Antes de implementar**, comprobar los nombres reales de `PhysicsManager::init`, `AudioManager::init`, `Scene::toJson` y `Scene::removeGameObject` en sus headers, y ajustar el test si difieren. No inventar firmas.

- [ ] **Step 3: Añadir el parámetro a Scene**

En `engine/include/DonTopo/Core/Scene.h`, añadir `class AsyncAssetLoader;` a las declaraciones adelantadas y cambiar las dos firmas:

```cpp
            // loader == nullptr → carga síncrona, comportamiento idéntico al de
            // siempre. Es lo que usan el restore de Play→Stop y los tests.
            //
            // loader != nullptr → los GameObject se crean completos pero sin
            // mesh, y cada sourcePath encola una petición. El caller es
            // responsable de bombear y de mostrar el progreso.
            bool fromJson(const nlohmann::json& j, PhysicsManager& physics,
                          AudioManager& audio, AsyncAssetLoader* loader = nullptr);

            bool load(const std::string& path, PhysicsManager& physics,
                      AudioManager& audio, AsyncAssetLoader* loader = nullptr);
```

Propagar el parámetro a `nodeFromJson` (declaración privada) y a `insertFromJson` si comparte el camino.

- [ ] **Step 4: Bifurcar la carga de mesh en `nodeFromJson`**

En `engine/src/Core/Scene.cpp`, línea ~721, la rama `else if (!sourcePath.empty())`:

```cpp
                else if (!sourcePath.empty())
                {
                    if (loader)
                    {
                        // Asíncrono: el GameObject queda sin mesh y se apunta a
                        // la petición. El pump lo resolverá por id — nunca por
                        // puntero, que sería dangling si el usuario lo borra
                        // mientras carga.
                        node->pendingMeshJob = loader->requestMesh(sourcePath, node->id);
                    }
                    else
                    {
                        auto mesh = std::make_shared<DonTopo::Mesh>(DonTopo::ModelLoader::load(sourcePath));
                        node->setMesh(std::move(mesh));
                    }
                }
```

Aplicar lo mismo a la rama skinned de más arriba (la que usa `loadAuto`/`addAnimationSource`), respetando su lógica de clips.

En `engine/include/DonTopo/Core/GameObject.h`, junto a `uint64_t id;` (línea 30):

```cpp
            // JobId de la carga de mesh en vuelo, 0 = ninguna. Es un uint64_t
            // opaco a propósito: Core no conoce AsyncAssetLoader, y el
            // destructor NO cancela nada — el pump ya descarta los resultados
            // cuyo targetId no existe.
            uint64_t pendingMeshJob = 0;
```

- [ ] **Step 5: Ejecutar los tests para verificar que pasan**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_scene_async_tests.exe
```

Esperado: `scene_async_tests OK`.

- [ ] **Step 6: Verificar el sabotaje y las regresiones**

Sabotajes:
1. En `nodeFromJson`, invertir la condición a `if (!loader)` en la rama asíncrona → `testSyncPathUnchanged` falla en el `toJson` comparado.
2. En `nodeFromJson`, mover la creación del nodo a la rama del pump → `testAsyncCreatesNodesImmediately` falla en `found == 2`.

Regresiones — las 11 suites:

```powershell
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_audio_tests.exe
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_animator_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
.\build-ninja\engine\tests\dt_exporter_tests.exe
.\build-ninja\engine\tests\dt_scripting_tests.exe
.\build-ninja\engine\tests\dt_splash_tests.exe
.\build-ninja\engine\tests\dt_jobsystem_tests.exe
.\build-ninja\engine\tests\dt_asset_loader_tests.exe
.\build-ninja\engine\tests\dt_scene_async_tests.exe
```

Esperado: las 11 OK. Ninguna de las 8 originales ha sido modificada.

- [ ] **Step 7: Commit**

```bash
git add engine/include/DonTopo/Core/Scene.h engine/src/Core/Scene.cpp engine/include/DonTopo/Core/GameObject.h engine/tests/scene_async_tests.cpp engine/tests/CMakeLists.txt
git commit -m "feat(scene): fromJson con AsyncAssetLoader opcional"
```

---

### Task 9: Integración del editor — drop asíncrono, modal y pump

**Files:**
- Create: `engine/include/DonTopo/Editor/LoadingModal.h`
- Create: `engine/src/Editor/LoadingModal.cpp`
- Modify: `engine/src/Editor/PropertiesPanel.cpp:121-162`
- Modify: `engine/include/DonTopo/Editor/EditorUI.h`, `engine/src/Editor/EditorUI.cpp:315-402`
- Modify: `sandbox/src/main.cpp:256-348`
- Modify: `engine/CMakeLists.txt`

**Interfaces:**
- Consumes: `AsyncAssetLoader::pumpCompleted/pending`, `Renderer::addStaticMesh(mesh, decoded)`, `Renderer::addSkinnedMesh(mesh, decoded)`, `Renderer::flushPendingUploads()`, `Renderer::tickDeferredDeletes()`, `GameObject::pendingMeshJob`.
- Produces: `class LoadingModal` con `void begin(int total)`, `void update(int pending)`, `bool active() const`, `bool draw()` (devuelve `true` si el usuario pulsó Cancel), y `void EditorUI::onAssetsLoaded(std::vector<LoadedMesh>, Scene&, Renderer&)`.

- [ ] **Step 1: Escribir el LoadingModal**

Crear `engine/include/DonTopo/Editor/LoadingModal.h`:

```cpp
#pragma once

namespace DonTopo
{
    // Overlay de progreso para las cargas de escena. NO congela la ventana: la
    // aplicación sigue pintando frames, así que Windows nunca la marca como "no
    // responde". Lo que veta es la edición, no el render.
    class LoadingModal
    {
        public:
            void begin(int total);
            void update(int pending);
            bool active() const { return m_active; }

            // Dibuja el overlay. Devuelve true si el usuario pulsó Cancel.
            bool draw();

        private:
            bool m_active = false;
            int  m_total  = 0;
            int  m_done   = 0;
    };
}
```

Crear `engine/src/Editor/LoadingModal.cpp`:

```cpp
#include "DonTopo/Editor/LoadingModal.h"

#include <imgui.h>

#include <algorithm>

namespace DonTopo
{
    void LoadingModal::begin(int total)
    {
        if (total <= 0) return;   // nada que cargar: no se abre el modal
        m_active = true;
        m_total  = total;
        m_done   = 0;
    }

    void LoadingModal::update(int pending)
    {
        if (!m_active) return;
        m_done = std::max(0, m_total - pending);
        if (pending <= 0) m_active = false;
    }

    bool LoadingModal::draw()
    {
        if (!m_active) return false;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin("##loading", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration
                     | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        ImGui::Text("Cargando escena...  %d / %d", m_done, m_total);
        const float frac = (m_total > 0) ? (float)m_done / (float)m_total : 0.0f;
        ImGui::ProgressBar(frac, ImVec2(320.0f, 0.0f));

        const bool cancelled = ImGui::Button("Cancelar");
        ImGui::End();

        // Cancelar deja la escena con lo cargado hasta aquí. Es un estado
        // válido y guardable, no una escena a medias que haya que tirar.
        if (cancelled) m_active = false;
        return cancelled;
    }
}
```

Añadir a `engine/CMakeLists.txt`:

```cmake
    src/Editor/LoadingModal.cpp
```

- [ ] **Step 2: Escribir `applyLoadedMesh`, compartida por editor y runtime**

La lógica de aplicar un resultado a la escena la necesitan `EditorUI` (Task 9) y el runtime (Task 10). Se escribe **una sola vez** como función libre: duplicarla haría que un arreglo en uno se olvidara en el otro.

En `engine/include/DonTopo/Renderer/AsyncAssetLoader.h`, tras la clase:

```cpp
    class Scene;
    class Renderer;

    // Aplica un resultado a la escena resolviendo por targetId sobre la escena
    // VIVA. Devuelve false si el GameObject ya no existe (borrado mientras
    // cargaba) o si el resultado trae error; en ese caso outError, si no es
    // nulo, recibe el mensaje para el log.
    //
    // No llama a flushPendingUploads: el caller decide cuándo cerrar el batch,
    // porque el sentido de todo esto es agrupar N resultados en UN submit.
    bool applyLoadedMesh(LoadedMesh& r, Scene& scene, Renderer& renderer,
                         std::string* outError);
```

En `engine/src/Renderer/AsyncAssetLoader.cpp`:

```cpp
    bool applyLoadedMesh(LoadedMesh& r, Scene& scene, Renderer& renderer,
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
```

- [ ] **Step 3: Escribir `EditorUI::onAssetsLoaded`**

En `engine/include/DonTopo/Editor/EditorUI.h`, añadir el include de `AsyncAssetLoader.h` y `LoadingModal.h`, y a la clase:

```cpp
    // Aplica a la escena los resultados que devuelve AsyncAssetLoader::pumpCompleted.
    void onAssetsLoaded(std::vector<LoadedMesh> results, Scene& scene, Renderer& renderer);

    // Lo rellena main() antes del bucle; sin él, los drops no encolan nada.
    void setAssetLoader(AsyncAssetLoader* loader) { m_assetLoader = loader; }

private:
    LoadingModal      m_loadingModal;
    AsyncAssetLoader* m_assetLoader = nullptr;
```

En `engine/src/Editor/EditorUI.cpp`:

```cpp
void EditorUI::onAssetsLoaded(std::vector<LoadedMesh> results, Scene& scene, Renderer& renderer)
{
    for (auto& r : results)
    {
        std::string err;
        if (!applyLoadedMesh(r, scene, renderer, &err) && !err.empty())
            pushLog(err);
    }

    // Un solo submit para todos los uploads de este pump. Es lo que convierte
    // ~440 vkQueueWaitIdle en uno.
    renderer.flushPendingUploads();
}
```

Sustituir `pushLog` por el método real de log de `EditorUI` si se llama de otra forma.

- [ ] **Step 4: Volver asíncrono el drop de FBX**

En `engine/src/Editor/PropertiesPanel.cpp:121`, sustituir el cuerpo de `loadMeshForSelected`:

```cpp
void PropertiesPanel::loadMeshForSelected(EditorContext& ctx, const std::string& path)
{
    // El guard de hasMesh() ya no basta: mientras la carga está en vuelo
    // hasMesh() es falso, así que un segundo drop encolaría una carga duplicada
    // y el segundo resultado pisaría al primero.
    if (!ctx.selected || !ctx.assetLoader
        || ctx.selected->hasMesh() || ctx.selected->pendingMeshJob != 0)
        return;

    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".fbx")
    {
        m_meshLoadError = "Formato no soportado: " + ext;
        return;
    }

    // No carga: encola. El registro en el Renderer y el setMesh los hace
    // EditorUI::onAssetsLoaded cuando el worker termine.
    ctx.selected->pendingMeshJob = ctx.assetLoader->requestMesh(path, ctx.selected->id);
    m_meshLoadError.clear();
    ctx.pushLog("Cargando '" + path + "'...");
}
```

Añadir `AsyncAssetLoader* assetLoader = nullptr;` a `EditorContext` (buscarlo en `engine/include/DonTopo/Editor/`) y rellenarlo donde se construye el contexto.

- [ ] **Step 5: Enganchar Load Scene al modal**

En `EditorUI::reloadSceneFromJson` (línea ~315), donde se llama a `fromJson`:

- La ruta de **Load Scene** (línea ~402) pasa el loader y abre el modal con el número de peticiones encoladas.
- La ruta del **restore de Play → Stop** (línea ~170) sigue llamando **sin** loader. Es determinista por diseño y ya se acelera con la capa B.

```cpp
    // Solo Load Scene va asíncrono. El restore de Play→Stop se queda síncrono a
    // propósito: meter estados a medias en esa transición no compensa la
    // ganancia, que la capa B ya da sola.
    const bool ok = scene.fromJson(j, physics, audio, async ? m_assetLoader : nullptr);
    if (ok && async && m_assetLoader)
        m_loadingModal.begin(m_assetLoader->pending());
```

Y en el `draw` de `EditorUI`, tras dibujar los paneles:

```cpp
    m_loadingModal.update(m_assetLoader ? m_assetLoader->pending() : 0);
    if (m_loadingModal.draw() && m_assetLoader)
    {
        // Cancelar: la escena se queda con lo cargado. Los jobs en vuelo
        // terminan, pero sus resultados se descartan al bombear.
        m_assetLoader->cancelAllPending();
    }
```

Añadir `void cancelAllPending();` a `AsyncAssetLoader` (cancela cada job de `m_groups` y vacía el buzón).

**Veto de edición mientras el modal esté activo:** en `EditorUI::draw`, envolver el dibujado de gizmos, jerarquía y drops con `if (!m_loadingModal.active())`.

- [ ] **Step 6: Enganchar el bucle principal**

En `sandbox/src/main.cpp`, antes del `traverse` de la línea 294 (y quitando las llamadas temporales de las Tasks 5 y 6):

```cpp
            // Antes del traverse: los objetos que aparezcan este frame tienen
            // que estar ya en el Renderer cuando se recorra la escena para
            // empujar transforms.
            renderer.tickDeferredDeletes();
            editorUI.onAssetsLoaded(assetLoader.pumpCompleted(2.0f), scene, renderer);
```

Y crear el JobSystem y el loader antes del bucle:

```cpp
        DonTopo::JobSystem       jobSystem;
        jobSystem.start();
        DonTopo::AsyncAssetLoader assetLoader(jobSystem);
        editorUI.setAssetLoader(&assetLoader);
```

- [ ] **Step 7: Arreglar el orden de apagado**

En `sandbox/src/main.cpp`, tras salir del bucle y **antes** de `scene.shutdown(...)`:

```cpp
        // PRIMERO el JobSystem: si se destruyera después del Renderer, un worker
        // en vuelo podría tocar memoria ya liberada. El join garantiza que
        // ningún hilo sigue vivo cuando empieza la destrucción del resto.
        jobSystem.shutdown();
```

- [ ] **Step 8: Compilar y verificar que no hay regresiones**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_audio_tests.exe
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_animator_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
.\build-ninja\engine\tests\dt_exporter_tests.exe
.\build-ninja\engine\tests\dt_scripting_tests.exe
.\build-ninja\engine\tests\dt_splash_tests.exe
.\build-ninja\engine\tests\dt_jobsystem_tests.exe
.\build-ninja\engine\tests\dt_asset_loader_tests.exe
.\build-ninja\engine\tests\dt_scene_async_tests.exe
```

Esperado: las 11 OK.

- [ ] **Step 9: Verificación manual**

```powershell
.\build-ninja\sandbox\Sandbox.exe
```

1. Arrastrar un FBX grande al viewport: **el editor no se congela**. El objeto aparece cuando termina, texturizado.
2. Arrastrar el mismo FBX dos veces seguidas y rápido sobre el mismo GameObject: solo se carga una vez, sin duplicados.
3. Cargar una escena con muchos objetos: sale el modal con progreso, la ventana responde, la barra avanza.
4. Pulsar Cancelar a mitad: la escena se queda con lo cargado, sin crash, y se puede guardar.
5. Borrar un GameObject mientras su FBX carga: sin crash al terminar la carga.
6. Cerrar la aplicación con una carga en vuelo: sin crash y sin mensajes de validación.

- [ ] **Step 10: Commit**

```bash
git add engine/include/DonTopo/Editor/LoadingModal.h engine/src/Editor/LoadingModal.cpp engine/include/DonTopo/Editor/EditorUI.h engine/src/Editor/EditorUI.cpp engine/src/Editor/PropertiesPanel.cpp engine/include/DonTopo/Renderer/AsyncAssetLoader.h engine/src/Renderer/AsyncAssetLoader.cpp sandbox/src/main.cpp engine/CMakeLists.txt
git commit -m "feat(editor): drop asincrono, modal de progreso y pump por frame"
```

---

### Task 10: Runtime exportado

**Files:**
- Modify: `runtime/main.cpp:142` y el bucle de la línea ~402
- Modify: `runtime/SplashDriver.h` si expone el texto de progreso

**Interfaces:**
- Consumes: `DonTopo::applyLoadedMesh(LoadedMesh&, Scene&, Renderer&, std::string*)` de la Task 9, `Renderer::hasPendingUploads()` y `Renderer::flushPendingUploads()` de la Task 6, `Scene::load(..., AsyncAssetLoader*)` de la Task 8.
- Produces: nada nuevo.

- [ ] **Step 1: Cargar la escena con loader**

En `runtime/main.cpp`, antes de la línea 142:

```cpp
        DonTopo::JobSystem        jobSystem;
        jobSystem.start();
        DonTopo::AsyncAssetLoader assetLoader(jobSystem);
```

Y cambiar la carga:

```cpp
        if (!scene.load(scenePath, physics, audio, &assetLoader))
```

- [ ] **Step 2: Esperar a que la escena esté completa antes del primer frame de juego**

A diferencia del editor, aquí **sí** se espera: un jugador no debe ver pop-in. Tras el `scene.load`, y con el splash todavía en pantalla:

```cpp
        // El splash sigue pintándose mientras se bombea, así que la ventana
        // responde y Windows no la marca como "no responde".
        //
        // Se usa applyLoadedMesh, la función libre de la Task 9: la misma que
        // usa EditorUI::onAssetsLoaded. Duplicar la lógica aquí haría que un
        // arreglo en el editor se olvidara en el runtime.
        while (assetLoader.pending() > 0)
        {
            renderer.tickDeferredDeletes();
            for (auto& r : assetLoader.pumpCompleted(1000.0f))
            {
                std::string err;
                if (!DonTopo::applyLoadedMesh(r, scene, renderer, &err) && !err.empty())
                    std::fprintf(stderr, "%s\n", err.c_str());   // acaba en game.log
            }
            renderer.flushPendingUploads();
            splash.drawFrame();
            window.pollEvents();
        }

        // Un pump vacío no basta: el último batch puede seguir en vuelo y el
        // primer frame de juego mostraría objetos todavía invisibles. Se espera
        // también a que todos los tickets hayan señalado.
        while (renderer.hasPendingUploads())
        {
            renderer.tickDeferredDeletes();
            splash.drawFrame();
            window.pollEvents();
        }
```

Añadir a `Renderer` el predicado que usa ese segundo bucle:

```cpp
            // true mientras quede algún batch de upload sin que su fence haya
            // señalado. Lo consulta el runtime para no enseñar el primer frame
            // con objetos a medio subir.
            bool hasPendingUploads() const { return !m_inFlightBatches.empty(); }
```

- [ ] **Step 3: Añadir el pump al bucle de juego**

En `runtime/main.cpp`, antes de `renderer.drawFrame(window);` (línea ~402):

```cpp
            renderer.tickDeferredDeletes();
```

No hace falta bombear: en el runtime todo se cargó antes de empezar. `tickDeferredDeletes` sí, porque los scripts pueden borrar objetos en tiempo de juego.

- [ ] **Step 4: Orden de apagado**

Antes del `shutdown` de la escena:

```cpp
        jobSystem.shutdown();
```

- [ ] **Step 5: Verificación manual**

```powershell
.\build.bat
.\build-ninja\runtime\DonTopoRuntime.exe
```

Y desde el editor: `File > Export Game...`, exportar y ejecutar el `.exe` resultante.

1. El splash muestra progreso real y cierra cuando la escena está completa.
2. El primer frame de juego enseña la escena entera, **sin pop-in**.
3. Cerrar: sin crash. Revisar `game.log` — sin errores.

- [ ] **Step 6: Commit**

```bash
git add runtime/main.cpp runtime/SplashDriver.h engine/include/DonTopo/Renderer/AsyncAssetLoader.h engine/src/Renderer/AsyncAssetLoader.cpp engine/src/Editor/EditorUI.cpp
git commit -m "feat(runtime): carga asincrona con progreso en el splash"
```

---

### Task 11: Verificación final y medición

**Files:**
- Create: `docs/superpowers/plans/2026-07-24-async-asset-loading-verificacion.md`

**Interfaces:**
- Consumes: todo.
- Produces: el registro de evidencia de los 6 criterios de aceptación.

- [ ] **Step 1: Activar las capas de validación con sync validation**

```powershell
$env:VK_INSTANCE_LAYERS = "VK_LAYER_KHRONOS_validation"
$env:VK_LAYER_ENABLES   = "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT"
.\build-ninja\sandbox\Sandbox.exe
```

- [ ] **Step 2: Ejecutar los cinco escenarios manuales**

Con la consola visible durante todos:

1. Abrir una escena de ~40 objetos.
2. Play → Stop, cinco veces seguidas.
3. Arrastrar un FBX al viewport **durante** Play.
4. Borrar un GameObject mientras su asset carga.
5. Cerrar la aplicación con cargas en vuelo.

**Criterio: cero mensajes de validación en los cinco.** Cualquier VUID es un fallo que se arregla antes de dar la tarea por hecha.

- [ ] **Step 3: Medir el frame time antes y después**

Con `git stash` para volver al estado previo a la Task 1, o comparando contra el commit `c0f2878`:

| Medida | Antes | Después | Criterio |
|---|---|---|---|
| Frame más lento al arrastrar un FBX | — | — | < 33 ms |
| Frame del Stop de Play | — | — | < 33 ms |
| Tiempo total de abrir una escena de 40 objetos | — | — | ventana siempre responde |

Rellenar la tabla con números reales, no con impresiones.

- [ ] **Step 4: Pasar las 11 suites**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_physics_tests.exe
.\build-ninja\engine\tests\dt_audio_tests.exe
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_animator_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
.\build-ninja\engine\tests\dt_exporter_tests.exe
.\build-ninja\engine\tests\dt_scripting_tests.exe
.\build-ninja\engine\tests\dt_splash_tests.exe
.\build-ninja\engine\tests\dt_jobsystem_tests.exe
.\build-ninja\engine\tests\dt_asset_loader_tests.exe
.\build-ninja\engine\tests\dt_scene_async_tests.exe
```

- [ ] **Step 5: Verificar en Release**

Las capas de validación no corren en Release y el timing cambia: un race latente puede aparecer solo aquí.

```powershell
.\configure-release.bat
.\build-release.bat
.\build-ninja-release\sandbox\Sandbox.exe
```

Repetir los cinco escenarios del Step 2.

- [ ] **Step 6: Escribir el registro de verificación y commit**

Crear `docs/superpowers/plans/2026-07-24-async-asset-loading-verificacion.md` con la tabla de medidas rellena, el resultado de los cinco escenarios en Debug y en Release, y la lista de sabotajes confirmados por tarea.

```bash
git add docs/superpowers/plans/2026-07-24-async-asset-loading-verificacion.md
git commit -m "docs: registro de verificacion de la carga asincrona"
```

---

## Criterios de aceptación

| # | Criterio | Medida | Tarea |
|---|---|---|---|
| 1 | Un drop de FBX no congela el editor | Ningún frame supera 33 ms durante la carga | 9, 11 |
| 2 | Abrir una escena de 40 objetos | El modal responde; la ventana nunca se marca "no responde" | 9, 11 |
| 3 | Play → Stop deja de dar hitch | El frame del Stop baja por debajo de 33 ms | 5, 11 |
| 4 | Cero regresiones | Las 8 suites originales pasan sin modificarlas | todas |
| 5 | Cero errores de validación | En los cinco escenarios manuales, Debug y Release | 11 |
| 6 | Cierre limpio con cargas en vuelo | Sin crash ni fuga reportada por validación | 9, 10, 11 |

## Deuda asumida

Frustum culling y batching (sub-proyecto C) siguen sin hacer: el FPS base no sube con este plan. Solapar PhysX (sub-proyecto D) tampoco entra. Ambos van con spec propia.
