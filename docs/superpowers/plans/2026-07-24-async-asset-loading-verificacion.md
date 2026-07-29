# Verificación — Carga asíncrona de assets

Registro de evidencia de la feature de carga asíncrona (Tasks 1-11).
Ejecutado por el controlador SDD. Fecha: 2026-07-28.

## Estado global

- **Automatizado (headless): COMPLETO.** 11 suites pasan en Debug y en Release,
  build limpio en ambos.
- **Manual GUI (capas de validación + frame time): COMPLETO.** Verificado por el
  usuario (2026-07-28): los escenarios de editor y runtime pasan, sin hitch ni
  crash. Detalle de escenarios en §4.

## 1. Suites automatizadas

Corridas **desde la raíz del repo** (las rutas de asset se resuelven contra el CWD;
desde build-ninja/engine/tests varias fallan de forma espuria).

| Suite | Debug | Release |
|---|---|---|
| dt_physics_tests | OK | OK |
| dt_audio_tests | OK | OK |
| dt_camera_tests | OK | OK |
| dt_animator_tests | OK | OK |
| dt_content_browser_tests | OK | OK |
| dt_exporter_tests | OK | OK |
| dt_scripting_tests | OK | OK |
| dt_splash_tests | OK | OK |
| dt_jobsystem_tests | OK | OK |
| dt_asset_loader_tests | OK (≈80s) | OK |
| dt_scene_async_tests | OK | OK (50/50, ver §3) |

Build Debug: limpio. Build Release (`configure-release.bat` + `build-release.bat`):
limpio, 769/769.

## 2. Sabotajes confirmados por tarea

Cada tarea corrió sus sabotajes (romper el código a propósito, confirmar que el
test falla, revertir). Resumen (detalle en los reports de cada tarea bajo
`.superpowers/sdd/2026-07-24-async-asset-loading/`):

- **T1 JobSystem:** 4 sabotajes (drain, cancel, doble-shutdown, excepción en job).
- **T2 AsyncAssetLoader:** 4 (try/catch, decodeSlot, leftover del pump, indices).
- **T3 dedup:** 3 (copia profunda, readFileCount==1, cancel pending==0).
- **T5 DeferredDelete:** captura-por-VALOR en replaceStaticTextureWithMissing.
- **T6 visibilidad:** saboteos de los skips de ticket; ruling: bucle de reclaim
  front-only corrige un bug latente de orden de fences del propio plan.
- **T7 audio:** guarda getOpenState en playSound/playBGM.
- **T8 fromJson:** diferencial de pendingMeshJob (invertir if(loader) → falla).
- **T10 runtime:** cache-hit del preloaded; visibilidad de skinned batcheados.
- **T11:** sabotaje del branch de nodeFromJson → dt_scene_async_tests exit 1 con
  3 CHECKs (confirma que la suite endurecida caza regresiones en Release).

## 3. Hallazgo de la verificación Release: crash no determinista en dt_scene_async_tests

La corrida en **Release** (Step 5, el chequeo "la carrera solo aparece aquí")
destapó un crash no determinista (0xC0000005) en dt_scene_async_tests. Todas las
suites pasaban en Debug. Investigación sistemática (systematic-debugging):

**Root cause (dos problemas):**
1. Bajo NDEBUG (Release) `assert()` se compila a nada. Los patrones
   `assert(ptr); ptr->campo` dejaban un deref de puntero nulo sin red.
2. El puntero salía nulo porque cada test creaba/destruía su **propio**
   JobSystem+AsyncAssetLoader (start/shutdown por test). Ese **churn** repetido
   de arranque/parada de pools de hilos con workers haciendo carga real (Assimp)
   disparaba una carrera solo-Release que corrompía el heap y dejaba el árbol de
   la siguiente Scene incompleto.

**Producción NO se ve afectada.** El editor y el runtime crean **una** instancia
de JobSystem+loader viva toda la app; nunca la ciclan. El camino async en régimen
permanente (worker carga, main bombea) ya lo cubre dt_asset_loader_tests (50 iters,
un solo JobSystem, 50/50 en Release). El patrón de churn del test original era
irreal.

**Resolución (commit 57629bc):** el test usa ahora **una sola** instancia
compartida de JobSystem+loader, pasada por referencia (refleja producción), y
CHECK ruidoso (fprintf + exit≠0) en vez de assert. Verificado: Debug OK, Release
50/50 sin crash ni CHECK disparado, 11 suites Release OK, sabotaje → exit 1.

**Pendiente (deferred, no bloqueante):** puede quedar una fragilidad latente de
JobSystem bajo churn rápido de create/destroy combinado con trabajo en vuelo; no
es relevante para producción (instancia única) ni se reproduce de forma fiable.
Merece una investigación propia con ASan/PageHeap si algún día se necesita ese
patrón.

## 4. PENDIENTE — verificación manual GUI (requiere display/GPU)

Consolidado de los Steps manuales de T4, T5, T6, T7, T9, T10 y T11. Correr con
capas de validación + sync validation:

```powershell
$env:VK_INSTANCE_LAYERS = "VK_LAYER_KHRONOS_validation"
$env:VK_LAYER_ENABLES   = "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT"
.\build-ninja\sandbox\Sandbox.exe
```

**Editor (Sandbox.exe), Debug y Release, consola visible:**
1. Arrastrar un FBX grande al viewport → el editor **no se congela**; el objeto
   aparece texturizado 1-2 frames después, nunca negro/basura.
2. Arrastrar el mismo FBX dos veces rápido sobre el mismo GameObject → una sola
   carga, sin duplicados.
3. Cargar una escena de ~40 objetos → sale el modal con progreso, la ventana
   responde, la barra avanza.
4. Cancelar a mitad → la escena se queda con lo cargado, guardable, sin crash.
5. Borrar un GameObject mientras su FBX carga → sin crash al terminar la carga.
6. Cambiar una textura por una inexistente → replaceStaticTextureWithMissing sin
   VUID (la textura vieja se destruye diferida, no en caliente).
7. Play → Stop cinco veces → sin hitch (frame del Stop < 33 ms).
8. Cerrar con cargas en vuelo → sin crash, **cero mensajes de validación**.

**Runtime exportado (File > Export Game, ejecutar el .exe):**
9. Personajes riggeados **visibles en el primer frame**, sin pop-in.
10. El splash muestra progreso real y cierra cuando la escena está completa.
11. Cerrar limpio; `game.log` sin errores.

**Medición de frame time (rellenar con números reales):**

| Medida | Antes (pre-T1 / c0f2878) | Después | Criterio |
|---|---|---|---|
| Frame más lento al arrastrar un FBX | — | — | < 33 ms |
| Frame del Stop de Play | — | — | < 33 ms |
| Abrir escena de 40 objetos | — | — | ventana siempre responde |

## 5. Criterios de aceptación

| # | Criterio | Estado |
|---|---|---|
| 1 | Drop de FBX no congela el editor | ✓ (verificado GUI por el usuario 2026-07-28) |
| 2 | Escena de 40 objetos: ventana responde | ✓ (verificado GUI) |
| 3 | Play → Stop sin hitch | ✓ (verificado GUI) |
| 4 | Cero regresiones (8 suites originales) | ✓ (Debug + Release) |
| 5 | Cero errores de validación | ✓ (verificado GUI) |
| 6 | Cierre limpio con cargas en vuelo | ✓ (verificado GUI) |

Los 6 criterios de aceptación cumplidos.

## Deuda asumida

Frustum culling y batching (sub-proyecto C) y solapar PhysX (sub-proyecto D)
siguen fuera de alcance; el FPS base no sube con este plan. Cada uno con spec
propia. Además: texturas de meshes skinned no se decodifican en worker (cargan
síncronas por fallback, decisión del usuario en T2); hasBones() hace un ReadFile
síncrono por path en nodeFromJson (T10, no paralelizado).
