# Auditoría del módulo de audio — Don Topo Engine

**Fecha:** 2026-08-27
**Alcance:** `engine/{include,src}/DonTopo/Audio/*`, `engine/tests/audio_tests.cpp`, superficie en el editor (`PropertiesPanel`, `ViewportPanel`, `ContentBrowserPanel`, `EditorUI`, `GameExporter`), serialización (`Core/Scene.cpp`), scripting (`ScriptBindings.cpp`, `LuaApiReference.cpp`), y las dos rutas de host (`sandbox/src/main.cpp`, `runtime/main.cpp`).
**Referencia comparativa:** Unity 6 (AudioSource / AudioListener / AudioMixer).
**Backend:** FMOD **Core API** (`fmod.hpp`), no FMOD Studio. Todo el código FMOD vive tras `#ifdef DT_FMOD_ENABLED`.

> Convenciones de este documento: cada afirmación sobre el estado actual lleva `fichero:línea` de código leído en esta sesión. Donde no he podido verificar algo contra el código o contra la documentación de FMOD, aparece `[verificar contra la API de FMOD]` o `[sin verificar]`. Se distingue siempre **no existe** (hay que escribir motor) de **existe pero no está expuesto** (hay que escribir UI/binding).

---

## 1. Inventario

### 1.1 Clases y responsabilidades

| Clase | Fichero | LOC | Responsabilidad |
|---|---|---|---|
| `AudioManager` | [AudioManager.h](../engine/include/DonTopo/Audio/AudioManager.h):11-77 / [AudioManager.cpp](../engine/src/Audio/AudioManager.cpp) | 79 / 307 | Dueño del `FMOD::System`, de dos `ChannelGroup` (SFX/BGM) y de dos tablas de `FMOD::Sound*`. Toda la API pública es de tipos estándar + GLM; los punteros FMOD viajan como `void*` ([AudioManager.h:69-75](../engine/include/DonTopo/Audio/AudioManager.h#L69)). |
| `AudioClipComponent` | [AudioClipComponent.h](../engine/include/DonTopo/Audio/AudioClipComponent.h):13-77 / [.cpp](../engine/src/Audio/AudioClipComponent.cpp) | 79 / 115 | Componente **único por GameObject** que envuelve un `soundId`. Guarda path, is3D, loop, playOnAwake, volume, pitch, min/maxDistance. No es copiable ([AudioClipComponent.h:18-19](../engine/include/DonTopo/Audio/AudioClipComponent.h#L18)). |
| `AudioListenerComponent` | [AudioListenerComponent.h](../engine/include/DonTopo/Audio/AudioListenerComponent.h):21-31 | 32 (header-only) | Un solo `bool m_enabled`. **No guarda posición ni orientación**: salen del `worldTransform` del GameObject dueño ([AudioListenerComponent.h:11-13](../engine/include/DonTopo/Audio/AudioListenerComponent.h#L11)). |

No hay más clases de audio. No hay `AudioMixer`, ni bus/grupo expuesto, ni DSP, ni componente de reverb, ni asset de clip compartido.

### 1.2 Ciclo de vida

- **init** — [AudioManager.cpp:24-40](../engine/src/Audio/AudioManager.cpp#L24): `System_Create` + `init(512, FMOD_INIT_NORMAL | FMOD_INIT_3D_RIGHTHANDED)` + dos `createChannelGroup("SFX"/"BGM")`. Cualquier fallo **lanza** `std::runtime_error` vía `fmodCheck` ([AudioManager.cpp:14-17](../engine/src/Audio/AudioManager.cpp#L14)).
- **update** — [AudioManager.cpp:42-53](../engine/src/Audio/AudioManager.cpp#L42): `set3DListenerAttributes(0, pos, vel={0,0,0}, fwd, up)` + `System::update()`. La velocidad del listener es **siempre cero**.
- **shutdown** — [AudioManager.cpp:55-68](../engine/src/Audio/AudioManager.cpp#L55): libera todos los `Sound`, los dos `ChannelGroup`, `close()` + `release()` del sistema. Idempotente por el guard `if (!m_system) return`. También lo llama el destructor ([AudioManager.cpp:22](../engine/src/Audio/AudioManager.cpp#L22)).
- **Quién lo conduce**:
  - Editor/Vulkan: `audio.update(...)` **solo dentro de** `if (renderer.isPlaying())` ([sandbox/src/main.cpp:948-975](../sandbox/src/main.cpp#L948)); cierre explícito en [sandbox/src/main.cpp:1155-1161](../sandbox/src/main.cpp#L1155) (`scene.shutdown()` → `audio.shutdown()`).
  - Editor/D3D12: mismo patrón, [sandbox/src/main.cpp:428-448](../sandbox/src/main.cpp#L428). Aquí no hay `shutdown()` explícito: el orden de declaración (`d3dPhysics`, `d3dAudio` antes de `d3dScene`, [sandbox/src/main.cpp:233-242](../sandbox/src/main.cpp#L233)) garantiza que la escena muere primero.
  - Runtime exportado: `audio.update(...)` **cada frame, siempre** ([runtime/main.cpp:646](../runtime/main.cpp#L646)); `audio.shutdown()` en [runtime/main.cpp:765](../runtime/main.cpp#L765).
- **Resolución del listener** (idéntica en las tres rutas): fallback a la cámara → si `Scene::findAudioListener()` devuelve algo habilitado y con base no degenerada, manda él. Posición = `worldTransform[3]`, forward = `-worldTransform[2]` normalizado, up = `worldTransform[1]` normalizado ([runtime/main.cpp:598-645](../runtime/main.cpp#L598), [sandbox/src/main.cpp:956-970](../sandbox/src/main.cpp#L956)). El guard de escala 0 (`length >= 1e-6f`) evita meter un NaN irrecuperable en FMOD.

### 1.3 Carga y reproducción

- `loadSound(path, is3D, loop)` — [AudioManager.cpp:70-90](../engine/src/Audio/AudioManager.cpp#L70): `FMOD_MODE` = (3D|2D) | (LOOP_NORMAL|LOOP_OFF) | **`FMOD_NONBLOCKING`**. Devuelve el índice en `m_sounds`, o -1.
- `unloadSound(id)` — [AudioManager.cpp:92-100](../engine/src/Audio/AudioManager.cpp#L92): `release()` y deja el slot a `nullptr` (**no lo recicla**).
- `playSound(id, worldPos, volume, pitch)` — [AudioManager.cpp:180-222](../engine/src/Audio/AudioManager.cpp#L180): descarta si `getOpenState == LOADING`; arranca **pausado**, para la voz anterior del mismo id vía `liveChannel`, aplica volumen/pitch, aplica `set3DAttributes` una sola vez si el modo es 3D, y despausa.
- `liveChannel` — [AudioManager.cpp:121-134](../engine/src/Audio/AudioManager.cpp#L121): valida que el `Channel*` guardado siga sonando **y** que su `getCurrentSound()` sea el sonido esperado. Es el guard contra el reciclado de voces de FMOD, y lo usan `setChannelVolume`, `setChannelPitch`, `setSound3DMinMaxDistance` y `stopSound`.
- BGM — `loadBGM` ([AudioManager.cpp:102-116](../engine/src/Audio/AudioManager.cpp#L102), modo `2D|LOOP_NORMAL|CREATESTREAM|NONBLOCKING`), `playBGM`/`stopBGM`/`pauseBGM` ([AudioManager.cpp:238-274](../engine/src/Audio/AudioManager.cpp#L238)).  **RETIRADO 2026-08-28 (H12/P11):** esta API ya no existe; el streaming se pide ahora por clip con `Load Mode = Stream` y la musica sale por el bus `Music`. Se deja la linea porque el resto del documento la referencia.
- Volúmenes de grupo — `setMasterVolume`/`setSfxVolume`/`setBgmVolume` ([AudioManager.cpp:276-298](../engine/src/Audio/AudioManager.cpp#L276)).

### 1.4 Propiedades serializadas

Escritura — [Scene.cpp:1109-1119](../engine/src/Core/Scene.cpp#L1109): `path`, `loop`, `is3D`, `playOnAwake`, `volume`, `pitch`, `minDistance`, `maxDistance`.
Lectura — [Scene.cpp:2396-2424](../engine/src/Core/Scene.cpp#L2396): `path`/`is3D`/`loop` con **`.at()`** (obligatorios), el resto con `.value()`/`readFloat` (tolerantes, con warning nombrando el campo).
Listener — escritura [Scene.cpp:1121-1126](../engine/src/Core/Scene.cpp#L1121) (solo `enabled`), lectura [Scene.cpp:2429-2434](../engine/src/Core/Scene.cpp#L2429), invariante de uno por escena impuesto a posteriori en [`pruneExtraAudioListeners`, Scene.cpp:2729-2739](../engine/src/Core/Scene.cpp#L2729).
Liberación — [`Scene::shutdown`, Scene.cpp:2911-2921](../engine/src/Core/Scene.cpp#L2911) pone `setAudioClip(nullptr)` en todo el árbol.

**No se serializa nada de BGM ni de volúmenes globales**: no existe sección de audio ni en la escena ni en el `project.json` (verificado por grep sobre `masterVolume`/`setSfxVolume`: cero resultados fuera del módulo).

### 1.5 API expuesta a Lua

Usertype `AudioClip` — [ScriptBindings.cpp:752-809](../engine/src/Scripting/ScriptBindings.cpp#L752): `Play`, `Stop`, `SetLoop`, `GetLoop`, `SetVolume`, `GetVolume`, `SetPitch`, `GetPitch`, `SetIs3D`, `GetIs3D`. Diez métodos, ni uno más.
Obtención — `Entity:GetComponent("AudioClip")` ([ScriptBindings.cpp:2071](../engine/src/Scripting/ScriptBindings.cpp#L2071)); alta — `AddComponent("AudioClip", path)` con `is3D=false, loop=false` fijos ([ScriptBindings.cpp:2128-2132](../engine/src/Scripting/ScriptBindings.cpp#L2128)); baja — [ScriptBindings.cpp:2234](../engine/src/Scripting/ScriptBindings.cpp#L2234).
`SetVolume`/`SetPitch` filtran no-finitos con `ensureFinite` y **avisan** por el Log ([ScriptBindings.cpp:777, 788](../engine/src/Scripting/ScriptBindings.cpp#L777)); el clamp real vive en el componente ([AudioClipComponent.cpp:66-77](../engine/src/Audio/AudioClipComponent.cpp#L66)).
Autocompletado — [LuaApiReference.cpp:139-142](../engine/src/Scripting/LuaApiReference.cpp#L139), los mismos diez.
Uso real — [Scripts/AudioFade.lua](../Scripts/AudioFade.lua) (fade por frame con `GetVolume`/`SetVolume`), [Scripts/AudioTest.lua](../Scripts/AudioTest.lua) (Play/Stop/Loop por teclado).
**No hay `AudioListener` en Lua, ni volúmenes globales, ni BGM, ni min/maxDistance, ni playOnAwake, ni una consulta `IsPlaying()`.**

### 1.6 Superficie en el editor

| Sitio | Qué hace | Referencia |
|---|---|---|
| Properties → Audio Clip | Nombre del fichero, Play/Stop de preview, checkboxes Loop / Is 3D? / Play On Awake, sliders Volume, Pitch y (solo en 3D) Min/Max distance, botón `x` de borrado. Add-gate por `m_audioClipAddRequestedFor`. | [PropertiesPanel.cpp:7639-7843](../engine/src/Editor/PropertiesPanel.cpp#L7639) |
| Properties → Audio Listener | Texto explicativo + checkbox Enabled + `x`. Add-gate por tener el componente. | [PropertiesPanel.cpp:705-734](../engine/src/Editor/PropertiesPanel.cpp#L705) |
| Menú Add | "Audio Clip" (deshabilitado si ya hay uno) y "Audio Listener" (deshabilitado si ya existe uno **en la escena**, con tooltip que nombra al dueño). | [PropertiesPanel.cpp:8075-8094](../engine/src/Editor/PropertiesPanel.cpp#L8075) |
| Alta del clip | Diálogo IGFD `.wav,.mp3,.ogg,.flac` + drop-zone `DT_ASSET_PATH`; whitelist de extensión y mensaje de error en rojo. | [PropertiesPanel.cpp:251-274](../engine/src/Editor/PropertiesPanel.cpp#L251), [7812-7842](../engine/src/Editor/PropertiesPanel.cpp#L7812) |
| Undo | Un solo `PropertyCommand<AudioClipState>` que agrupa volume+pitch+min+max de un arrastre, con guarda de propietario por id. | [PropertiesPanel.cpp:7696-7795](../engine/src/Editor/PropertiesPanel.cpp#L7696) |
| Viewport | Dos esferas de alambre magenta (min y max) para el clip 3D seleccionado. | [ViewportPanel.cpp:256-285](../engine/src/Editor/ViewportPanel.cpp#L256) |
| Content Browser | Audio arrastrable (`.wav/.mp3/.ogg/.flac` en `kDraggableExt`), y el rename/delete de un asset repara o limpia el `path` del clip en toda la escena. | [ContentBrowserPanel.cpp:488-492](../engine/src/Editor/ContentBrowserPanel.cpp#L488), [213-221](../engine/src/Editor/ContentBrowserPanel.cpp#L213), [314-316](../engine/src/Editor/ContentBrowserPanel.cpp#L314) |
| Play Mode | Al pulsar Play: gate del listener + un único aviso al Log, y `play()` de todos los clips con `playOnAwake`. | [EditorUI.cpp:1480-1494](../engine/src/Editor/EditorUI.cpp#L1480) |
| Export | Los `.wav/.mp3/...` referenciados entran en el bundle y su `path` se reescribe al del paquete. | [GameExporter.cpp:264-265](../engine/src/Editor/GameExporter.cpp#L264), [364-365](../engine/src/Editor/GameExporter.cpp#L364) |

**Ninguna ventana ni panel expone volumen Master/SFX/BGM.** No hay panel de audio.

---

## 2. Comparativa con Unity

Leyenda de estado: **Completo** = equivalente funcional a Unity · **Parcial** = existe pero limitado o no expuesto · **Ausente** = no existe en el motor.

| Capacidad Unity | Estado en Don Topo | Evidencia (file:line) | Impacto para el usuario |
|---|---|---|---|
| **AudioSource** — varios por GameObject | **Parcial** — exactamente uno; el Add se deshabilita si ya hay | `GameObject.h:111-113`, `PropertiesPanel.cpp:8075-8079` | Un objeto no puede tener música + pasos + voz. Hay que crear GameObjects hijos vacíos como truco. |
| AudioSource → `playOnAwake` | **Completo** | `AudioClipComponent.h:52-53`, `EditorUI.cpp:1490-1493`, `runtime/main.cpp:530-533`, `Scene.cpp:1115` | Paridad con Unity, incluido el exportado. |
| AudioSource → `loop` | **Completo** (con matiz) | `AudioClipComponent.cpp:34-39`, `PropertiesPanel.cpp:7672-7674`, `ScriptBindings.cpp:764-773` | Cambiarlo **recarga el sonido y corta lo que suene** (va horneado en el `FMOD_MODE`); en Unity es un flag del source. |
| AudioSource → `priority` | **Ausente** | — (sin `setPriority` en `AudioManager.cpp`) | Con 512 voces ([AudioManager.cpp:29](../engine/src/Audio/AudioManager.cpp#L29)) rara vez importa; cuando se saturen, FMOD robará voces sin criterio de diseño. `Channel::setPriority` existe en Core API. |
| AudioSource → `spatialBlend` | **Parcial** — booleano 2D/3D, no un blend continuo | `AudioClipComponent.cpp:41-46`, `PropertiesPanel.cpp:7676-7678` | No se puede hacer un sonido "medio espacializado". Además alternarlo recarga el clip. |
| AudioSource -> `mute` | **Completo** (2026-08-28) | `AudioClipComponent::setMute`, checkbox en el inspector, `AudioClip:SetMute` en Lua | Silencia sin perder el volumen: al desmutear vuelve el que habia. Se serializa (un objeto puede nacer mudo), a diferencia de la pausa. Un clip muteado tampoco dispara `PlayOneShot`: esa voz no se puede alcanzar despues, asi que no habria forma de callarla. |
| AudioSource -> `bypassEffects` / `bypassListenerEffects` | **Ausente, y descartado** (ver seccion 5) | - | Con P13 dejo de ser teorico —ya hay efectos que puentear— pero hacerlo bien exige partir el grafo de buses en dos niveles, y no compensa. Razon completa en la seccion 5. |
| AudioSource → `minDistance` / `maxDistance` | **Completo**, con gizmo | `AudioClipComponent.cpp:79-103`, `AudioManager.cpp:161-178`, `ViewportPanel.cpp:256-285`, `Scene.cpp:1118-1119` | Mejor que el mínimo: el valor se conserva al alternar 2D/3D y se aplica en vivo al canal sonando. **No expuesto en Lua.** |
| AudioSource -> curva de rolloff | **Completo** para las curvas de FMOD Core (P12, 2026-08-28) - `inverse`/`linear`/`linearSquare`, en el FMOD_MODE y en la clave de cache. No hay curva a mano (`CUSTOMROLLOFF`) ni un modo "sin atenuacion": para eso se sube maxDistance | `AudioManager.cpp:169` (solo `set3DMinMaxDistance`) | Sin control de la forma de la atenuación, solo del rango. Core API ofrece `FMOD_3D_LINEARROLLOFF`/`INVERSE`/`CUSTOMROLLOFF` en el `FMOD_MODE` **[verificar contra la API de FMOD]** el nombre exacto de las constantes. |
| AudioSource -> `dopplerLevel` | **Completo** (P12, 2026-08-28) - velocidades derivadas por frame del listener y de cada fuente, con tope antiteleport. 0 por defecto: encenderlo cambiaria el tono de lo que ya suena en escenas existentes. Solo actua en Play | `AudioManager.cpp:47` (`vel = {0,0,0}` del listener), `AudioManager.cpp:214` (`v = {0,0,0}` de la fuente) | Un coche pasando de largo no cambia de tono. Requiere calcular velocidades por frame, que hoy no se calculan en ninguna parte. |
| AudioSource -> `spread` | **Completo** (P12, 2026-08-28) - [0, 360] grados, propiedad de la voz | — (sin `set3DSpread`) | Un sonido 3D colapsa a un punto; sin ensanchado estéreo de cerca. |
| AudioSource -> paneo estereo (`panStereo`) | **Completo** (P12, 2026-08-28) - [-1, 1], solo en clips 2D (en 3D lo decide la posicion), y la UI solo lo ofrece ahi | — (sin `setPan`) | No hay forma de colocar un sonido 2D a izquierda/derecha (típico de UI y diálogo). `Channel::setPan` existe en Core API. |
| **AudioListener** — componente y transform | **Completo** | `AudioListenerComponent.h:21-31`, `Scene.cpp:2585-2596`, `runtime/main.cpp:634-645` | Paridad, incluido el fallback a cámara y el guard de escala 0. |
| AudioListener -> pausa global (`AudioListener.pause`) | **Completo** (2026-08-28) | `AudioManager::setAudioPaused`, `Audio.SetPaused` en Lua | Congela todo conservando posiciones, actuando sobre el grupo master — asi tambien alcanza las voces sueltas de `PlayOneShot`, que no se pueden tocar de otra forma. NO es un timeScale: el motor sigue sin pausa de simulacion, esto solo calla el audio. |
| AudioListener → volumen global (`AudioListener.volume`) | **Completo** (2026-08-27, P4) - slider Master en View, persistido, y `Audio.SetBusVolume("master", v)` | `AudioManager.cpp:276-284` existe; sin llamadas (grep) | No hay slider de volumen ni en el editor ni en el juego exportado. Coste bajo: es UI + binding, no motor. |
| **AudioMixer**, grupos/buses con volúmenes independientes (Master/Music/SFX) | **Completo** para lo que da Core API (2026-08-27, P4) - tres buses, enrutado por clip, volumenes persistidos y expuestos en editor y Lua | `AudioManager.cpp:31-38` (SFX/BGM), `AudioManager.cpp:198` (todo SFX va al mismo grupo), `AudioManager.cpp:286-298` sin llamantes | El diseñador no puede bajar la música sin bajar los efectos. Un clip no puede elegir bus. La infraestructura de `ChannelGroup` ya está: falta enrutado por clip + persistencia + UI. |
| Snapshots del mixer y transiciones | **Ausente** | - | Siguen siendo de FMOD Studio. Ojo: el ejemplo con el que se justificaba esta fila —"modo bajo el agua"— YA se puede hacer desde P13 con `Audio.SetBusEffect` y un lowpass en el Master; lo que falta es el concepto de snapshot con nombre y transiciones interpoladas entre ellos, no el efecto. |
| Filtros y efectos (lowpass, highpass, reverb, echo) | **Completo** (P13, 2026-08-28) | `AudioManager::setBusEffect`, `Audio.SetBusEffect` en Lua | Los cuatro tipos, colgados del BUS y no de cada clip: el caso de uso real es de grupo, y un filtro por voz se paga por voz. Un solo mando [0,1] por efecto, mapeado a las unidades de FMOD en un unico sitio. NO se serializan a proposito: modelan un estado temporal de juego, no una propiedad de la escena. |
| Reverb Zones | **Completo** (P13, 2026-08-28) | `ReverbZoneComponent`, `Scene::syncReverbZones`, seccion y gizmo en el editor | Varias por escena; la mezcla entre zonas solapadas y el desvanecido entre min y max los hace FMOD via `Reverb3D` (comprobado en el header instalado, no supuesto). El recurso nativo lo lleva `AudioManager` emparejado por id del GameObject, y las zonas huerfanas se recogen en el sync por frame — sin eso, borrar el objeto dejaba su reverb aplicandose para siempre. |
| One-shots sin GameObject (`PlayOneShot`, `PlayClipAtPoint`) | **Completo** (P5, 2026-08-27/28) - `PlayOneShot` en el componente y `Audio.PlayClipAtPoint` global, con `Audio.Preload` para que el primer disparo no se pierda por la carga diferida | `AudioManager.cpp:205` para la voz anterior del mismo id; `AudioClipComponent::play` es el único camino | **El hueco más visible en la práctica:** dos disparos seguidos del mismo clip no se solapan (el segundo corta al primero), y no hay forma de soltar un sonido en una posición sin crear un GameObject con componente. |
| Control de reproduccion: `time` / `timeSamples` | **Completo** para `time`; `timeSamples` descartado | `AudioClipComponent::getTime/setTime`, `AudioClip:GetTime`/`SetTime` en Lua | Posicion en SEGUNDOS. `GetTime` devuelve -1 sin nada sonando: 0 seria "al principio del clip", que es otra respuesta. `timeSamples` (indice en muestras del PCM) sigue descartado en la seccion 5 por las razones de siempre. |
| Control de reproduccion: `Pause`/`UnPause` de una fuente | **Completo** (P7, 2026-08-27) | `AudioClipComponent::pause/resume`, `AudioClip:Pause`/`Resume` en Lua | Conservan la posicion de reproduccion, al reves que `Stop`. |
| Control de reproduccion: `isPlaying` consultable | **Completo** (P7, 2026-08-27) | `AudioClipComponent::isPlaying/isPaused`, expuestos en Lua | La logica ya existia en `liveChannel` pero era `static` del .cpp. Una voz PAUSADA cuenta como sonando, igual que en FMOD y en Unity; `IsPaused` es lo que las separa. |
| Control de reproducción: `PlayScheduled` | **Ausente** | — | Sin encadenado sin costuras (música por capas). Core API lo cubre con `Channel::setDelay` + `System::getDSPClock` **[verificar contra la API de FMOD]**. |
| Modos de carga: streaming vs decompress-on-load, `preloadAudioData` | **Completo** para lo que hace falta (2026-08-28, P8b) - `Load Mode` Sample/Stream por clip, serializado, con combo en el inspector y `SetLoadMode`/`GetLoadMode` en Lua. `preloadAudioData` sigue ausente (no hay pipeline de importacion donde encajarlo) | `AudioManager.cpp:109` (`CREATESTREAM` en BGM) vs `AudioManager.cpp:78-80` (SFX sin flag de carga) | Todo SFX se descomprime entero en RAM. Un `.mp3` largo puesto como clip de un objeto se come decenas de MB sin avisar. No hay ningún control en la UI. |
| Formatos soportados | **Parcial** — FMOD acepta más de lo que deja la UI | `PropertiesPanel.cpp:258` (`.wav .mp3 .ogg .flac`), `ContentBrowserPanel.cpp:488-492` (misma lista) | La lista cubre lo normal. Ojo: la whitelist **solo existe en la ruta de UI** — Lua y la carga de escena aceptan cualquier extensión (ver hallazgo H16). |
| Integración con `timeScale` / pausa del Play Mode | **N/A — no existe ninguna de las dos cosas en el motor** | grep `timeScale`/`pause` en `engine/`, `runtime/`, `sandbox/`: cero resultados fuera de `pauseBGM` | No es un hueco de audio sino de Core. Cuando se añada `timeScale`, el pitch de los clips debería seguirlo (hoy nadie lo haría). |
| Superficie de scripting equivalente (`AudioSource` en C#) | **Parcial** — 10 de ~30 miembros útiles | `ScriptBindings.cpp:752-809`, `LuaApiReference.cpp:139-142` | Faltan min/max distance, playOnAwake, mute, pan, time, IsPlaying, y todo lo de listener/mixer. Y **el README no documenta ni uno solo** (ver H15). |

---

## 3. Hallazgos

### H1 - Un clip 3D no sigue a su GameObject · **Alto** · RESUELTO 2026-08-27 (P1)

> Resuelto: `AudioManager::setSoundPosition()` (via `liveChannel`, no-op en 2D y sin voz viva) + `AudioClipComponent::updateSpatial()` + `Scene::updateAudioSpatial()`, que recorre el arbol y se llama al final de `Scene::update` -con los transforms ya al dia, no antes- cubriendo Play en las tres rutas de host. En Edit Mode se llama ademas desde las dos ramas `else` del editor, porque ahi no se pasa por `Scene::update` y la preview del inspector puede estar sonando mientras se arrastra el objeto con el gizmo.
> 
> **Limite del test.** Los dos tests nuevos cubren la ruta y los casos degenerados (sin manager, `soundId` invalido, clip 2D, nodo sin clip), no el efecto audible: comprobar que la voz se movio exigiria exponer un getter de la posicion del canal que nadie mas usaria. La atenuacion y el paneo siguiendo al objeto quedan como verificacion manual en el editor.
> 
> Sigue sin doppler (velocidad a cero tanto en la fuente como en el listener): es P12.

**Qué falla.** `set3DAttributes` se llama **una sola vez**, dentro de `playSound` ([AudioManager.cpp:211-216](../engine/src/Audio/AudioManager.cpp#L211)). No hay ninguna otra llamada en el repo (verificado por grep). El sonido se queda clavado en la posición que tenía el objeto en el instante del `play()`.
**Cómo reproducirlo.** Cubo con `AudioClip` 3D en loop y `Play On Awake`, más un `Rigidbody` o `Mover.lua`. Play: el objeto se aleja del listener y el volumen **no cambia**. Con el listener quieto y el objeto cruzando de izquierda a derecha, el paneo tampoco se mueve.
**Dónde.** [AudioManager.cpp:212-216](../engine/src/Audio/AudioManager.cpp#L212) (única escritura), [runtime/main.cpp:646-649](../runtime/main.cpp#L646) y [sandbox/src/main.cpp:971-974](../sandbox/src/main.cpp#L971) (el bucle de Play actualiza el listener pero no las fuentes).
**Severidad: Alto.** Es la razón de ser del audio 3D; el gizmo de min/max ([ViewportPanel.cpp:256-285](../engine/src/Editor/ViewportPanel.cpp#L256)) promete un comportamiento que solo se cumple para objetos estáticos.

### H2 — El fallo de carga de un audio es invisible de punta a punta · **Alto** · RESUELTO 2026-08-27 (P2)

> Resuelto: `AudioManager::getSoundState()` (enum `SoundLoadState`) y `pollLoadFailures()`, que drenan una vez por sonido los paths que han pasado a `Failed`. El pump corre cada frame en `EditorUI::draw` (Log Console) y en el bucle del runtime (`cerr`), fuera del gate de Play. `AudioClipComponent::hasLoadError()` pinta "No se pudo cargar" en rojo en el inspector, con tooltip del path. Dos tests: path inexistente y fichero que existe pero no es audio.
> 
> Confirmado ademas contra FMOD: `createSound` con `FMOD_NONBLOCKING` SI devuelve `FMOD_OK` para un path inexistente y para un fichero corrupto (el test lo exige explicitamente con `CHECK(clip != nullptr)`), lo que cierra el `[verificar contra la API de FMOD]` que llevaba este hallazgo. El fallo se detecta por el valor de retorno de `getOpenState`, no por `FMOD_OPENSTATE_ERROR`: esa rama quedo sin cobertura y esta anotada como tal en el codigo.

**Qué falla.** Con `FMOD_NONBLOCKING` ([AudioManager.cpp:80](../engine/src/Audio/AudioManager.cpp#L80)), `createSound` retorna al instante y el error real de apertura aparece después, vía `getOpenState` — así que `createSound(...) != FMOD_OK` ([AudioManager.cpp:82](../engine/src/Audio/AudioManager.cpp#L82)) no atrapa un fichero inexistente ni corrupto **[verificar contra la API de FMOD]**: el `soundId` se entrega como válido. Consecuencia en cadena:
- `createAudioClipComponent` devuelve un componente ([AudioManager.cpp:300-305](../engine/src/Audio/AudioManager.cpp#L300)), así que la rama de error de la UI, `m_audioLoadError = "No se pudo cargar el audio"` ([PropertiesPanel.cpp:266-269](../engine/src/Editor/PropertiesPanel.cpp#L266)), no llega a ejecutarse.
- La rama equivalente de la carga de escena tampoco ([Scene.cpp:2421-2423](../engine/src/Core/Scene.cpp#L2421), comentada como "asset roto: el nodo queda sin audio").
- `playSound` solo descarta el estado `LOADING` ([AudioManager.cpp:191-194](../engine/src/Audio/AudioManager.cpp#L191)); con `FMOD_OPENSTATE_ERROR` cae al `SYS->playSound(...) != FMOD_OK` y retorna **en silencio** ([AudioManager.cpp:198](../engine/src/Audio/AudioManager.cpp#L198)).
**Cómo reproducirlo.** Renombrar `assets/audio.mp3` a mano (fuera del Content Browser, para que no salte la reparación de [ContentBrowserPanel.cpp:213-221](../engine/src/Editor/ContentBrowserPanel.cpp#L213)) y cargar la escena. Ni un warning en `Scene::lastWarnings()`, ni una línea en el Log, y el clip aparece en Properties como si estuviera bien. Igual con un `.mp3` de 0 bytes soltado en la drop-zone.
**Severidad: Alto.** Es exactamente el patrón "ruta de error silenciosa": el usuario ve verde y no oye nada, sin ninguna pista.

### H3 — El gate "sin Audio Listener no suena nada" solo cubre `playOnAwake`; el Log miente · **Alto** · ✅ RESUELTO 2026-08-27 (P3, opción "retirar el gate")

> Resuelto: el gate se ha retirado — los clips con `playOnAwake` suenan también sin listener, oídos desde la cámara, y el aviso pasa a ser informativo y cierto ("el audio 3D se oye desde la camara"). Alineados `EditorUI.cpp`, `runtime/main.cpp`, `AudioListenerComponent.h` y `README.md:25`. Se conserva el análisis de abajo por el patrón: una regla que no puede vivir en una sola capa acaba aplicada en la mitad de sus callsites.

**Qué falla.** El gate existe en dos sitios y ambos envuelven **solo** el barrido de `playOnAwake`: [EditorUI.cpp:1485-1493](../engine/src/Editor/EditorUI.cpp#L1485) y [runtime/main.cpp:524-534](../runtime/main.cpp#L524). Las otras dos rutas de reproducción no lo consultan: `AudioClip:Play` de Lua ([ScriptBindings.cpp:754-758](../engine/src/Scripting/ScriptBindings.cpp#L754)) y el botón Play de Properties ([PropertiesPanel.cpp:7662-7666](../engine/src/Editor/PropertiesPanel.cpp#L7662)). Y como `audio.update()` se llama igualmente cada frame de Play con **fallback a la cámara** ([runtime/main.cpp:598-646](../runtime/main.cpp#L598)), el sonido se oye perfectamente.
**Cómo reproducirlo.** Escena sin `AudioListener` + un GameObject con `AudioFade.lua` (que hace `clip:Play()` en `Start`, [Scripts/AudioFade.lua:15-23](../Scripts/AudioFade.lua#L15)). Play: el Log dice *"Sin Audio Listener en la escena: los AudioClip no se reproduciran"* y el clip **suena**.
**Dónde.** [EditorUI.cpp:1488](../engine/src/Editor/EditorUI.cpp#L1488) (el mensaje), [ScriptBindings.cpp:757](../engine/src/Scripting/ScriptBindings.cpp#L757) (la ruta que lo ignora). El README repite la afirmación falsa en [README.md:25](../README.md#L25).
**Severidad: Alto.** No por el sonido de más, sino porque un invariante documentado en tres sitios (header, README, log) no se cumple: quien depure "por qué suena si no hay listener" no encontrará el gate.

### H4 — Un `audioClip` sin `path`/`is3D`/`loop` tumba la escena entera · **Medio** · ✅ RESUELTO 2026-08-27 (P9)

> Resuelto: nuevos helpers `readBool`/`readString` junto a `readFloat` en `Scene.cpp`; los tres campos pasan por ellos con `required=true` (avisan nombrando el campo y caen al default). Sin `path` el nodo se queda sin clip y la escena sigue. Cuatro tests nuevos, uno de ellos con un GameObject hermano **sin audio** para medir el daño real: con el `.at()` viejo, ese objeto ajeno también se perdía (verificado por sabotaje).

**Qué falla.** [Scene.cpp:2399-2400](../engine/src/Core/Scene.cpp#L2399) usa `c.at("path")`, `c.at("is3D")`, `c.at("loop")`. Si falta cualquiera de las tres, `nlohmann` lanza, la excepción sube por `nodeFromJson` y el `catch` de `fromJson` devuelve `false`: se pierde **toda** la escena por un campo. El resto del mismo bloque usa `readFloat` con warning ([Scene.cpp:2412-2419](../engine/src/Core/Scene.cpp#L2412)), y el fichero de tests exige justo ese criterio para otros componentes ([audio_tests.cpp:328-350](../engine/tests/audio_tests.cpp#L328), `test_boxCollider_missing_halfExtents_warns`).
**Cómo reproducirlo.** Abrir un `.json` de escena, borrar `"is3D"` de un `audioClip`, cargar: "No se pudo cargar la escena", sin decir cuál era el campo.
**Severidad: Medio.** No es back-compat (los tres campos existen desde el principio) sino corrupción/merge, pero el coste para el usuario es máximo y el patrón correcto ya está escrito al lado.

### H5 - Anadir y quitar AudioClip / AudioListener no pasan por el undo · **Medio** · RESUELTO 2026-08-27 (P6)

> Resuelto: `AudioClipComponentCommand` y `AudioListenerComponentCommand` en `Command.h/.cpp`, con el patron de `CanvasComponentCommand` (GameObject por id, nunca puntero crudo) y una diferencia obligada: `AudioClipComponent` NO es copiable -envuelve un `soundId` de FMOD-, asi que el snapshot son datos planos (path + `AudioClipState`) y el redo recrea el componente con `createAudioClipComponent`. Las cuatro mutaciones del panel pasan ya por el stack. Verificado por sabotaje (no restaurar `playOnAwake`; crear el listener siempre habilitado) -> fallan los dos tests nuevos.

**Qué falla.** Cuatro mutaciones directas sin comando: alta de clip ([PropertiesPanel.cpp:271](../engine/src/Editor/PropertiesPanel.cpp#L271)), baja de clip ([PropertiesPanel.cpp:7802](../engine/src/Editor/PropertiesPanel.cpp#L7802)), alta de listener ([PropertiesPanel.cpp:8088](../engine/src/Editor/PropertiesPanel.cpp#L8088)), baja de listener ([PropertiesPanel.cpp:731](../engine/src/Editor/PropertiesPanel.cpp#L731)). Compárese con Canvas, que sí usa `CanvasComponentCommand` ([PropertiesPanel.cpp:8102-8109](../engine/src/Editor/PropertiesPanel.cpp#L8102)).
**Cómo reproducirlo.** Ajustar volumen 0.3, pitch 1.4 y distancias de un clip; pulsar la `x`; Ctrl+Z. El clip no vuelve, y los cuatro valores se han perdido (hay que recargar el fichero y reajustarlos a mano).
**Severidad: Medio.** Pérdida de trabajo silenciosa, con el agravante de que el undo **sí** funciona para los sliders del mismo panel: la asimetría hace confiar en él.

### H6 - Loop / Is 3D / Play On Awake tampoco tienen undo, y dos de ellos cortan el sonido · **Medio** · RESUELTO 2026-08-27 (P6)

> Resuelto: `AudioClipState` pasa de 4 a 7 campos y los tres checkboxes empujan un `PropertyCommand<AudioClipState>` inmediato (un checkbox no tiene arrastre que esperar). El snapshot y la restauracion viven en dos helpers unicos (`audioClipStateOf` / `applyAudioClipState`) que comparten sliders, checkboxes y Add/Remove. El undo del arrastre de un slider toma los tres bools del estado actual en las dos puntas, para no revertir de rebote un checkbox tocado a mitad de camino.
> 
> Nota: `dt_audio_tests` pasa a enlazar `DonTopoEditor` en vez de `DonTopoCore` (los comandos viven en la libreria del editor), igual que ya hacen otros cinco tests.

**Qué falla.** Los tres checkboxes escriben directo ([PropertiesPanel.cpp:7672-7682](../engine/src/Editor/PropertiesPanel.cpp#L7672)) mientras Volume/Pitch/Min/Max sí generan comando ([PropertiesPanel.cpp:7746-7794](../engine/src/Editor/PropertiesPanel.cpp#L7746)). Además `setLoop`/`setIs3D` disparan `reload()` ([AudioClipComponent.cpp:105-113](../engine/src/Audio/AudioClipComponent.cpp#L105)): `unloadSound` + `loadSound`, lo que mata la voz en curso y —por H2— vuelve a pasar por la carga asíncrona sin reportar errores.
**Cómo reproducirlo.** Con un clip en loop sonando en Play, desmarcar "Is 3D?": el sonido se corta en seco y Ctrl+Z no lo devuelve.
**Severidad: Medio.**

### H7 — La preview en Edit Mode se reproduce contra un listener que nadie actualiza · **Medio** · ✅ RESUELTO 2026-08-27 (P10)

> Resuelto: `audio.update(...)` sale del `if (isPlaying())` en las dos rutas del editor. En Edit Mode el listener es **siempre** la cámara del editor (aunque la escena tenga `AudioListener`: la preview debe oírse desde donde mira el usuario); en Play se mantiene la resolución de antes. De paso se corrigió una divergencia entre backends encontrada al implementarlo: la rama D3D12 no comprobaba `getEnabled()` del listener ([sandbox/src/main.cpp:441](../sandbox/src/main.cpp#L441) antes del cambio), así que un listener deshabilitado se respetaba en D3D12 y se ignoraba en Vulkan con la misma escena.

**Qué falla.** `audio.update(...)` —el único sitio donde se llama a `set3DListenerAttributes` **y** a `System::update()`— vive dentro del `if (isPlaying())` en las dos rutas del editor: [sandbox/src/main.cpp:948-971](../sandbox/src/main.cpp#L948) (Vulkan) y [sandbox/src/main.cpp:428-448](../sandbox/src/main.cpp#L428) (D3D12). El botón Play del inspector ([PropertiesPanel.cpp:7662-7666](../engine/src/Editor/PropertiesPanel.cpp#L7662)) funciona en Edit Mode, donde el listener sigue donde lo dejó la última sesión de Play, o en el `(0,0,0)` mirando a `-Z` por defecto si nunca se entró en Play.
**Cómo reproducirlo.** Objeto con clip 3D a 300 unidades del origen (`maxDistance` 100). Editor recién abierto, seleccionar, Play en el inspector: silencio, sin ninguna explicación. Marcar "Is 3D?" a off y vuelve a oírse.
**Dónde.** [sandbox/src/main.cpp:948](../sandbox/src/main.cpp#L948) (el gate), [AudioManager.cpp:42-53](../engine/src/Audio/AudioManager.cpp#L42) (lo que no se ejecuta).
**Nota.** El efecto exacto de no llamar a `System::update()` sobre voces virtuales, callbacks y el avance de las cargas `NONBLOCKING` en Edit Mode queda **[verificar contra la API de FMOD]**; lo verificado aquí es que no se llama.
**Severidad: Medio.**

### H8 — `init()` no limpia si falla a medias y no es reentrante · **Medio** · ✅ RESUELTO 2026-08-27 (P9)

> Resuelto: guarda de reentrada al principio de `init()`, y `try/catch` que hace `release()` del `FMOD::System` si falla cualquier paso posterior a `System_Create`. Test `test_init_is_reentrant`.

**Qué falla.** `m_system` se asigna en [AudioManager.cpp:36](../engine/src/Audio/AudioManager.cpp#L36), **después** de los dos `createChannelGroup` ([AudioManager.cpp:33-34](../engine/src/Audio/AudioManager.cpp#L33)). Si uno de ellos falla, `fmodCheck` lanza con un `FMOD::System` ya creado y con `m_system` todavía `nullptr` — con lo que `shutdown()` sale por su guard ([AudioManager.cpp:58](../engine/src/Audio/AudioManager.cpp#L58)) y el sistema **nunca se cierra ni se libera**. Un segundo `init()` sobre un manager ya inicializado pisa los tres punteros sin liberar nada.
**Cómo reproducirlo.** No hay ruta de usuario directa: exige que `createChannelGroup` falle (memoria agotada) o llamar a `init()` dos veces. Auditoría de código, no bug observado.
**Severidad: Medio** (fuga de recurso en una ruta de error real, no alcanzable desde la UI hoy).

### H9 — Sin dispositivo de audio, `init()` lanza: el editor no arranca y los tests abortan en vez de saltarse · **Medio** · ✅ RESUELTO 2026-08-27 (P9)

> Resuelto: `init()` devuelve `bool` y ya no propaga la excepción — escribe una línea en `cerr` y el motor sigue mudo. Nuevo `AudioManager::available()`, que es lo que decide ahora el SKIP de los tests (antes lo decidía "existe el fichero", y con FMOD compilado sin dispositivo el binario abortaba antes de imprimirlo).

**Qué falla.** `fmodCheck` lanza `std::runtime_error` ([AudioManager.cpp:14-17](../engine/src/Audio/AudioManager.cpp#L14)) desde `init()` ([AudioManager.cpp:28-29](../engine/src/Audio/AudioManager.cpp#L28)). En el editor la excepción viaja hasta el `catch` de `main` ([sandbox/src/main.cpp:1164-1167](../sandbox/src/main.cpp#L1164)) → `EXIT_FAILURE`: **una máquina sin salida de audio no puede abrir el editor**. En los tests, `am.init()` está fuera de cualquier `try` ([audio_tests.cpp:386-387](../engine/tests/audio_tests.cpp#L386)) → excepción no capturada → abort. La rama `SKIP ... (FMOD no disponible)` de `checkAudioProbe` ([audio_tests.cpp:38-51](../engine/tests/audio_tests.cpp#L38)) solo es alcanzable compilando **sin** `DT_FMOD_ENABLED` (ahí `init()` es no-op y `createAudioClipComponent` devuelve `nullptr` por `m_system == nullptr`, [AudioManager.cpp:73](../engine/src/Audio/AudioManager.cpp#L73)); con FMOD compilado y sin dispositivo, el mensaje que promete el SKIP nunca se imprime.
**Cómo reproducirlo.** Deshabilitar el dispositivo de salida en Windows y lanzar `Sandbox.exe` (o `audio_tests.exe`).
**Severidad: Medio.**

### H10 - Los slots de sonido no se reciclan · **Bajo** · RESUELTO 2026-08-27 (P8a)

> Resuelto: free-list de slots (`m_freeSlots`); `unloadSound` devuelve el slot y `loadSound` lo reutiliza antes de crecer. Verificable desde fuera gracias a `soundSlotCount()`: el primer test que escribi para esto usaba `loadedSoundCount()` y **pasaba aunque no se reciclara nada** (sin reciclar, el slot queda a nullptr y los sonidos vivos vuelven a cero igual) — lo delato el sabotaje, y de ahi el segundo getter.

**Qué falla.** `unloadSound` deja `nullptr` ([AudioManager.cpp:97-98](../engine/src/Audio/AudioManager.cpp#L97)) y `loadSound` siempre hace `push_back` ([AudioManager.cpp:83-85](../engine/src/Audio/AudioManager.cpp#L83)). Cada ciclo Play→Stop reconstruye la escena entera desde el snapshot ([EditorUI.cpp:1429](../engine/src/Editor/EditorUI.cpp#L1429)), y cada `setLoop`/`setIs3D` fuerza un `reload()`: los dos vectores crecen de forma monótona durante toda la sesión.
**Severidad: Bajo** — son punteros, no objetos FMOD; no es fuga, es crecimiento sin techo.

### H11 - Sin cache por path: N objetos con el mismo `.wav` = N copias descomprimidas en RAM · **Bajo** · RESUELTO 2026-08-27 (P8a)

> Resuelto: cache con refcount. La clave NO es el path sino **path + is3D + loop**: los dos flags van horneados en el FMOD_MODE, asi que el mismo fichero en 3D y en 2D son sonidos distintos; con el path solo, marcar "Is 3D?" en un clip se lo habria cambiado a todos los demas (sabotaje verificado).
> 
> **Efecto colateral obligado**, ya previsto en la propuesta: al compartir el `FMOD::Sound`, `set3DMinMaxDistance` sobre el sonido le cambiaria el radio de atenuacion a todos los clips que lo usen. Las distancias pasan a viajar en `playSound`/`playSoundOneShot` y se escriben en la VOZ. El one-shot las necesitaba explicitamente: su voz no se guarda, asi que no habia forma de aplicarselas despues y se habria quedado con las de fabrica de FMOD (1 / 10000).
> 
> **P8b, hecho a continuacion (2026-08-28):** el modo de carga elegible (Sample vs Stream) ya esta — clave de cache ampliada al modo, `FMOD_CREATESTREAM`, campo `loadMode` en el .scene con back-compat a `sample`, combo en el inspector y los tres sitios de scripting.
> 
> Aviso que costo un rato entender: con `FMOD_NONBLOCKING`, `Sound::getMode()` **no refleja `FMOD_CREATESTREAM` hasta que la carga termina**. El primer test que escribi no esperaba, fallaba siempre, y como tambien fallaba con el sabotaje puesto **parecia que lo estaba detectando**. El test espera ahora a `Ready` antes de comprobar el modo.

**Qué falla.** `createSound` se llama por componente, sin consultar si ese path ya está cargado ([AudioManager.cpp:82](../engine/src/Audio/AudioManager.cpp#L82)), y sin `CREATESTREAM` ni `CREATECOMPRESSEDSAMPLE` para SFX ([AudioManager.cpp:78-80](../engine/src/Audio/AudioManager.cpp#L78)) — el modo por defecto de FMOD descomprime la muestra entera en memoria **[verificar contra la API de FMOD]**.
**Cómo reproducirlo.** Duplicar 20 veces un objeto con un `.mp3` de 3 minutos y mirar el consumo de RAM del proceso.
**Severidad: Bajo** hoy (escenas pequeñas), **Alto** en cuanto haya una escena real.

> **Nota P5 (2026-08-27).** `PlayOneShot` ya existe: `AudioManager::playSoundOneShot` dispara una voz que NO se guarda en `m_sfxChannels`, asi que se solapa con lo que suene. Expuesto en Lua como `AudioClip:PlayOneShot()` (los tres sitios de la convencion tocados). La voz suelta queda fuera de alcance a proposito: `Stop`, los setters de volumen/pitch, `IsPlaying` y el seguimiento 3D por frame no la ven. Es la mitad del hueco; la variante sin GameObject (`PlayClipAtPoint`) sigue pendiente y depende de la cache por path (P8).

### H12 - La API de BGM esta muerta · **Bajo** · RESUELTO 2026-08-28 (P11)

> Resuelto RETIRANDOLA, que era la recomendacion del propio analisis: `loadBGM`/`playBGM`/`stopBGM`/`pauseBGM` fuera, junto con `m_bgmSounds`, `m_bgmCh` y las dos lineas comentadas del sandbox. El grupo pasa a llamarse `m_musicGroup` ("Music" en FMOD), que es el bus del que ya tira P4. Se fue con ello el orden correcto: **primero** P4 (bus Music) y P8b (streaming por clip), que son el sustituto, y solo despues la retirada — quitarlo antes habria dejado el motor sin ninguna via de streaming.
> 
> De paso, los tres envoltorios `setMasterVolume`/`setSfxVolume`/`setBgmVolume`: tambien fuera. `setBusVolume` los cubre y ya lo usan la UI, Lua y los tests; conservarlos era la misma clase de API muerta que el hallazgo denuncia.

**Qué falla.** `loadBGM`/`playBGM`/`stopBGM`/`pauseBGM` ([AudioManager.cpp:102-274](../engine/src/Audio/AudioManager.cpp#L102)) no se llaman desde ninguna parte: la única referencia del repo está **comentada** ([sandbox/src/main.cpp:754-755](../sandbox/src/main.cpp#L754)). No hay `unloadBGM` (los streams solo se liberan en `shutdown`, [AudioManager.cpp:60](../engine/src/Audio/AudioManager.cpp#L60)), ni serialización, ni UI, ni binding.
**Severidad: Bajo** (código muerto, no roto). Relevante porque *música de fondo* es una de las primeras cosas que pide un usuario, y el motor ya la tiene a medio camino.

### H13 - Los volumenes globales existen y nadie los expone · **Bajo** · RESUELTO 2026-08-27 (P4)

> Resuelto junto con los buses: `AudioBus` en su propio header, `setBusVolume`/`getBusVolume`, enrutado por clip en `playSound`/`playSoundOneShot`, campo `bus` en el .scene (por NOMBRE, con back-compat a `sfx` y aviso si el nombre no existe), combo en el inspector con undo, tres sliders en **View** persistidos en `project.json`, y `Audio.SetBusVolume`/`GetBusVolume` en Lua. Los tres sitios de la convencion tocados.
> 
> **Sin cobertura de test, y dicho en el codigo:** que la voz salga por el grupo correcto no se puede observar desde un test headless; un sabotaje que enrutara todo a SFX pasa la suite. Queda como verificacion manual (clip en Music + Music Volume a 0). Lo que si esta cubierto: round-trip del bus, back-compat, nombre desconocido, independencia de los tres volumenes y el clamp/NaN desde Lua (los tres sabotajes correspondientes fallan).

**Qué falla.** `setMasterVolume`/`setSfxVolume`/`setBgmVolume` ([AudioManager.cpp:276-298](../engine/src/Audio/AudioManager.cpp#L276)) no tienen ni un llamante (grep en `engine/`, `runtime/`, `sandbox/`). No hay panel de audio, ni ajuste en el proyecto, ni binding Lua.
**Severidad: Bajo** por el coste de arreglarlo (es exposición pura, cero motor), **Alto** por lo que impide: un juego exportado sin control de volumen.

### H14 - Huecos de test · **Bajo** · RESUELTO EN SU MAYOR PARTE 2026-08-27 (P14)

> Cubierto: clamp de `minDistance`/`maxDistance`, rechazo de NaN en ambas, invariante `min <= max` atacado desde los DOS setters, y round-trip de los siete campos del `audioClip` a la vez con valores distintos entre si (`is3D`, `loop`, `playOnAwake`, `volume`, `pitch`, `min`, `max`, `path`). Verificado con tres sabotajes simultaneos: quitar el clamp de min, quitar el arrastre del invariante y no leer `playOnAwake` -> fallan tres tests distintos.
> 
> **Correccion a este hallazgo.** Decia que el orden `setMaxDistance` antes que `setMinDistance` (Scene.cpp, y el `apply` del undo en PropertiesPanel) era sutil y quedaba sin proteger. Al escribir el test se comprobo que **el orden no puede importar**: cada setter arrastra al otro para mantener el invariante, y con los rangos vigentes (min en [0.1, 50] con default 1, max en [1, 1000] con default 100) no existe ninguna pareja de valores en la que los dos ordenes den resultados distintos. No hay test que escribir ahi, y el comentario de `Scene.cpp` que justifica ese orden sobra.
> 
> **Sigue pendiente:** los dos tests de collider (`test_boxCollider_missing_halfExtents_warns`, `test_capsuleCollider_null_center_warns`) siguen viviendo en `audio_tests.cpp`. Moverlos a `physics_tests.cpp` obliga a montar alli un `AudioManager` (lo pide `Scene::fromJson`), y es coste sin beneficio funcional. Y `liveChannel` / el reciclado de voces de FMOD sigue sin test: exige reproducir de verdad y esperar a que una voz termine.

**Qué falla.** Los 407 LOC de [audio_tests.cpp](../engine/tests/audio_tests.cpp) cubren clamp de volume/pitch, rechazo de NaN y robustez del JSON — y de sus 12 tests, **dos son de colliders**, no de audio ([audio_tests.cpp:328-380](../engine/tests/audio_tests.cpp#L328)). Sin cobertura:
- `minDistance`/`maxDistance`: ni clamp ([AudioClipComponent.cpp:83, 93](../engine/src/Audio/AudioClipComponent.cpp#L83)), ni el invariante `min <= max` en ambos sentidos ([AudioClipComponent.cpp:86, 94](../engine/src/Audio/AudioClipComponent.cpp#L86)), ni round-trip.
- `playOnAwake`, `is3D`, `loop`: se serializan ([Scene.cpp:1113-1115](../engine/src/Core/Scene.cpp#L1113)) y ningún test los lee de vuelta.
- El orden `setMaxDistance` antes que `setMinDistance` en la carga ([Scene.cpp:2418-2419](../engine/src/Core/Scene.cpp#L2418)) y en el undo ([PropertiesPanel.cpp:7788-7789](../engine/src/Editor/PropertiesPanel.cpp#L7788)) es sutil y no está protegido: invertirlo pasaría todos los tests.
- `liveChannel` y el reciclado de voces ([AudioManager.cpp:121-134](../engine/src/Audio/AudioManager.cpp#L121)) — el guard más delicado del módulo, sin un solo test.
- `pruneExtraAudioListeners` ([Scene.cpp:2729-2739](../engine/src/Core/Scene.cpp#L2729)) y el gate del listener.
**Severidad: Bajo** individualmente; en conjunto es el motivo por el que H1 y H3 han sobrevivido.

### H15 - La API Lua de audio no esta en el README, contra la convencion del repo · **Bajo** · RESUELTO 2026-08-27 (P7)

> Resuelto junto con P7: nueva subseccion `### Audio from Lua` en la seccion `## Lua Scripting` del README, con la tabla completa de metodos, las trampas (SetLoop/SetIs3D recargan el sonido; una voz pausada cuenta como sonando) y un ejemplo. Los tres sitios de la convencion tocados a la vez: `ScriptBindings.cpp`, `LuaApiReference.cpp` y el README.

**Qué falla.** El autocompletado la declara ([LuaApiReference.cpp:139-142](../engine/src/Scripting/LuaApiReference.cpp#L139)) pero la sección `## Lua Scripting` del README ([README.md:626](../README.md#L626) y siguientes: *Scene switching*, *Physics from Lua*, *Collision callbacks*, *UI from Lua*) **no menciona `AudioClip` ni una vez**; solo hay una alusión genérica a "Audio API" en [README.md:44](../README.md#L44).
**Severidad: Bajo.** Es deuda de documentación, pero la convención del repo la marca como obligatoria.

### H16 - La whitelist de extensiones solo protege la ruta de UI · **Bajo** · RESUELTO 2026-08-28

> Resuelto: `isSupportedAudioExtension()` en `AudioBus.h`, usada por las tres rutas — inspector, `Scene::nodeFromJson` (avisa nombrando la extension y descarta el clip, dejando cargar el resto de la escena) y `AddComponent("AudioClip")` de Lua (avisa por el Log y devuelve nil). Test con caso de control: la misma ruta con extension valida SI crea el clip, para que un filtro que rechazara siempre no pasara por bueno. Sabotaje verificado.

**Qué falla.** `kValidExt` vive en [PropertiesPanel.cpp:258](../engine/src/Editor/PropertiesPanel.cpp#L258). Ni `AddComponent("AudioClip", path)` de Lua ([ScriptBindings.cpp:2128-2131](../engine/src/Scripting/ScriptBindings.cpp#L2128)) ni la carga de escena ([Scene.cpp:2399-2400](../engine/src/Core/Scene.cpp#L2399)) la consultan. Combinado con H2, un path absurdo por esas dos vías produce un clip mudo y silencioso.
**Severidad: Bajo.**

### Nota de riesgo (no es un bug)

`AudioClipComponent` guarda un `AudioManager*` crudo ([AudioClipComponent.h:65](../engine/include/DonTopo/Audio/AudioClipComponent.h#L65)) y lo usa en el destructor ([AudioClipComponent.cpp:19-22](../engine/src/Audio/AudioClipComponent.cpp#L19)). La corrección depende enteramente del orden de declaración de los hosts, que está documentado y **es correcto hoy** en las tres rutas ([sandbox/src/main.cpp:228-242](../sandbox/src/main.cpp#L228), [1155-1161](../sandbox/src/main.cpp#L1155); [runtime/main.cpp:233-238](../runtime/main.cpp#L233), [765](../runtime/main.cpp#L765)). Un cuarto host que lo ignore tendrá un use-after-free sin ninguna red debajo.

**Recuento:** Crítico 0 · Alto 3 (H1, H2, H3) · Medio 6 (H4–H9) · Bajo 7 (H10–H16).

---

## 4. Mejoras propuestas

Cada propuesta de API Lua nombra los **tres** sitios que exige la convención del repo: `engine/src/Scripting/ScriptBindings.cpp` (binding), `engine/src/Scripting/LuaApiReference.cpp` (autocompletado) y la sección `## Lua Scripting` de `README.md` (documentación).

### P1 · Seguimiento 3D por frame de las fuentes — **arregla H1**
- **Qué se gana.** Que el audio 3D funcione con objetos en movimiento, que es su único caso de uso interesante. Habilita después el doppler.
- **Ficheros.** `AudioManager.h/.cpp` (nuevo `setSoundPosition(int, const glm::vec3&, const glm::vec3& vel)` sobre `liveChannel`), `AudioClipComponent.h/.cpp` (un `updateSpatial(worldPos)`), `Core/Scene.cpp` (`Scene::update` recorre el árbol y empuja la posición de los clips 3D sonando), `engine/tests/audio_tests.cpp`.
- **Esfuerzo:** M · **Riesgo:** Bajo (todo pasa por `liveChannel`, que ya protege del reciclado de voces) · **Depende de:** nada.

### P2 · Hacer visible el fallo de carga — **arregla H2 y parte de H16**
- **Qué se gana.** Que un asset roto o inexistente deje de ser silencio inexplicable.
- **Cómo.** Un `getSoundState(id)` público (`Loading` / `Ready` / `Error`) sobre `getOpenState`; un pump por frame en el host que, al pasar a `Error`, escriba una línea en el Log/`std::cerr` nombrando el path. `playSound` distingue `LOADING` (silencio legítimo, ya está) de `ERROR` (avisa una vez).
- **Ficheros.** `AudioManager.h/.cpp`, `Editor/EditorUI.cpp` + `runtime/main.cpp` (el pump), `Editor/PropertiesPanel.cpp` (marcar el clip roto en rojo en la sección), `engine/tests/audio_tests.cpp`.
- **Esfuerzo:** M · **Riesgo:** Bajo · **Depende de:** nada. Conviene **antes** que P1 (si no, un clip roto seguirá siendo mudo con o sin seguimiento).

### P3 · Cumplir o retirar el gate del listener — **arregla H3**
- **Qué se gana.** Coherencia entre lo que el motor hace y lo que dicen header, README y Log. **Decisión de diseño previa** (no la tomo aquí): o el gate se aplica también a `AudioClip:Play` y a la preview del inspector, o se retira y se cambia el mensaje por "sin listener, el audio 3D se oye desde la cámara" — que es la verdad de hoy.
- **Ficheros.** `Editor/EditorUI.cpp`, `runtime/main.cpp`, `Scripting/ScriptBindings.cpp`, `Editor/PropertiesPanel.cpp`, `Audio/AudioListenerComponent.h` (comentario), `README.md`, `engine/tests/audio_tests.cpp`.
- **Esfuerzo:** S · **Riesgo:** Bajo · **Depende de:** nada.

### P4 · Buses Master/Music/SFX de verdad + volúmenes globales persistidos — **arregla H13, parte de H12 y la fila AudioMixer**
- **Qué se gana.** El control que hoy no existe: bajar la música sin tocar los efectos, y un volumen global en el juego exportado. La infraestructura de `ChannelGroup` ya está ([AudioManager.cpp:31-38](../engine/src/Audio/AudioManager.cpp#L31)); falta enrutado por clip, persistencia y superficie.
- **Cómo.** Un enum `AudioBus { Master, Music, Sfx }` en `AudioClipComponent`, usado en `playSound` en vez del `SFXG` fijo de [AudioManager.cpp:198](../engine/src/Audio/AudioManager.cpp#L198). Los tres volúmenes viven en el `project.json` y se aplican en el arranque del editor y del runtime.
- **Ficheros.** `AudioManager.h/.cpp`, `AudioClipComponent.h/.cpp`, `Core/Scene.cpp` (campo `bus` con `.value()` para back-compat), `Editor/PropertiesPanel.cpp` (combo Bus), un panel o sección de ajustes de audio en `Editor/EditorUI.cpp`, `Core/Project.*`, `runtime/main.cpp`, **`ScriptBindings.cpp` + `LuaApiReference.cpp` + `README.md`** para `Audio.SetMasterVolume/SetMusicVolume/SetSfxVolume`.
- **Esfuerzo:** L · **Riesgo:** Medio (toca serialización: exige el criterio de back-compat de [Scene.cpp:2403-2405](../engine/src/Core/Scene.cpp#L2403)) · **Depende de:** conviene después de P6 (undo/serialización sanas).

### P5 · `PlayOneShot` y one-shots posicionales — **arregla la fila de one-shots**
- **Qué se gana.** Que dos disparos seguidos se solapen, y poder soltar un sonido en una posición del mundo sin crear un GameObject. Hoy el segundo `play()` corta al primero por diseño ([AudioManager.cpp:205](../engine/src/Audio/AudioManager.cpp#L205)).
- **Cómo.** Un `playSoundOneShot(id, pos, vol, pitch)` que **no** guarde el canal en `m_sfxChannels` (voz suelta, la gestiona FMOD) + un `Audio.PlayClipAtPoint(path, x, y, z)` global que cachee el sonido por path (sinergia con P8).
- **Ficheros.** `AudioManager.h/.cpp`, `AudioClipComponent.h/.cpp` (`PlayOneShot`), **`ScriptBindings.cpp` + `LuaApiReference.cpp` + `README.md`**, `engine/tests/audio_tests.cpp`.
- **Esfuerzo:** M · **Riesgo:** Bajo · **Depende de:** P8 para la variante sin GameObject (si no, cada `PlayClipAtPoint` cargaría el fichero otra vez).

### P6 · Undo y comandos para alta/baja de componente y checkboxes — **arregla H5 y H6**
- **Qué se gana.** Que el panel de audio se comporte como el resto del editor, y que quitar un clip deje de ser una pérdida irreversible de cuatro valores ajustados.
- **Cómo.** Un `AudioClipComponentCommand` calcado de `CanvasComponentCommand` ([PropertiesPanel.cpp:8102-8109](../engine/src/Editor/PropertiesPanel.cpp#L8102)) que guarde el estado completo del clip, y ampliar `AudioClipState` con `loop`/`is3D`/`playOnAwake`.
- **Ficheros.** `Editor/Command.h/.cpp`, `Editor/PropertiesPanel.h/.cpp`.
- **Esfuerzo:** M · **Riesgo:** Bajo (patrón ya establecido dos veces en el repo) · **Depende de:** nada.

### P7 · Completar la superficie Lua del audio — **arregla parte de H15 y la fila de scripting**
- **Qué se gana.** Paridad razonable con `AudioSource`: `SetMinDistance`/`GetMinDistance`, `SetMaxDistance`/`GetMaxDistance`, `SetPlayOnAwake`/`GetPlayOnAwake`, `GetPath`, **`IsPlaying`** (la lógica ya existe en `liveChannel`, [AudioManager.cpp:121-134](../engine/src/Audio/AudioManager.cpp#L121); solo hay que hacerla pública), `SetMute`/`GetMute`, `Pause`/`Resume`.
- **Ficheros.** `AudioManager.h/.cpp` (`isSoundPlaying`, `setChannelMute`, `setChannelPaused`), `AudioClipComponent.h/.cpp`, **`ScriptBindings.cpp` + `LuaApiReference.cpp` + `README.md` (sección `## Lua Scripting`, hoy sin nada de audio)**.
- **Esfuerzo:** M · **Riesgo:** Bajo · **Depende de:** nada; los getters de distancia son gratis (ya están en el componente).

### P8 · Caché de sonidos por path + modo de carga elegible — **arregla H10 y H11, y la fila de modos de carga**
- **Qué se gana.** Que N objetos con el mismo asset compartan un `FMOD::Sound`, y que un clip largo pueda marcarse como *streaming* en vez de descomprimirse entero.
- **Cómo.** `std::unordered_map<std::string, int>` en `AudioManager` con refcount, reciclado de slots libres, y un `AudioLoadMode { Sample, Stream }` serializado y expuesto en la UI. Ojo: un `Sound` compartido implica que `set3DMinMaxDistance` sobre el sonido ([AudioManager.cpp:169](../engine/src/Audio/AudioManager.cpp#L169)) afectaría a todos los componentes que lo usen — habría que mover ese ajuste **solo** al canal.
- **Ficheros.** `AudioManager.h/.cpp`, `AudioClipComponent.h/.cpp`, `Core/Scene.cpp`, `Editor/PropertiesPanel.cpp`, `engine/tests/audio_tests.cpp`.
- **Esfuerzo:** L · **Riesgo:** Medio (el refcount y la interacción con `reload()`) · **Depende de:** conviene tras P1 (que fija el canal como sitio de los atributos espaciales).

### P9 · Robustecer la carga de escena y el `init` — **arregla H4, H8, H9**
- **Qué se gana.** Que un `audioClip` corrupto no tumbe la escena entera, y que una máquina sin dispositivo de audio abra el editor (mudo y con un aviso) en vez de no abrir.
- **Cómo.** `.at()` → `.value()` con warning nombrando el campo, siguiendo `readFloat`; `init()` con `try/catch` interno que libere el `System` si falla a media construcción, guard de reentrada, y un `bool AudioManager::available()` que los hosts consulten. Los tests envuelven `am.init()` para que el SKIP sea alcanzable de verdad.
- **Ficheros.** `Core/Scene.cpp`, `Audio/AudioManager.h/.cpp`, `sandbox/src/main.cpp`, `runtime/main.cpp`, `engine/tests/audio_tests.cpp`.
- **Esfuerzo:** M · **Riesgo:** Bajo · **Depende de:** nada.

### P10 · Actualizar el listener también en Edit Mode — **arregla H7**
- **Qué se gana.** Que la preview del inspector sea representativa. Basta con sacar `audio.update(...)` del `if (isPlaying())` y alimentarlo con la cámara del editor en Edit Mode.
- **Ficheros.** `sandbox/src/main.cpp` (las dos rutas, Vulkan y D3D12).
- **Esfuerzo:** S · **Riesgo:** Bajo · **Depende de:** nada.

### P11 · Cerrar el círculo del BGM o retirarlo — **arregla H12**
- **Qué se gana.** Quitar código muerto o convertirlo en la función que el usuario espera. Con P4 hecho, el bus Music + un clip en streaming **cubren el caso de uso entero**: mi recomendación es **retirar** `loadBGM`/`playBGM`/`stopBGM`/`pauseBGM` en favor de P4+P8, no seguir construyendo el camino paralelo.
- **Ficheros.** `AudioManager.h/.cpp`, `sandbox/src/main.cpp` (borrar las dos líneas comentadas).
- **Esfuerzo:** S · **Riesgo:** Bajo · **Depende de:** P4 y P8 (no retirar antes de que exista el sustituto).

### P12 · Rolloff, spread, pan y doppler — **arregla cuatro filas de la tabla**
- **Qué se gana.** El resto de la espacialización de Unity. `rolloff` y `spread` son un flag de `FMOD_MODE` / una llamada de canal; `pan` es `Channel::setPan`; el doppler necesita velocidades por frame (derivar de la posición del frame anterior, tanto de la fuente como del listener) **[verificar contra la API de FMOD]** los nombres exactos de constantes y setters en la versión instalada.
- **Ficheros.** `AudioManager.h/.cpp`, `AudioClipComponent.h/.cpp`, `Core/Scene.cpp`, `Editor/PropertiesPanel.cpp`, **`ScriptBindings.cpp` + `LuaApiReference.cpp` + `README.md`**.
- **Esfuerzo:** L · **Riesgo:** Medio (el doppler amplifica cualquier salto de posición: un teleport suena como un chirrido) · **Depende de:** P1 (sin seguimiento por frame no hay velocidad que calcular).

### P13 · Filtros DSP (lowpass/highpass/echo/reverb) y una zona de reverb — **arregla dos filas**
- **Qué se gana.** Ambientes y oclusión. `System::createDSPByType` es **Core API**, no requiere Studio.
- **Ficheros.** `AudioManager.h/.cpp` (gestión de DSP por bus y por canal), un `ReverbZoneComponent` nuevo (`engine/include/DonTopo/Audio/`, `engine/src/Audio/`, CMakeLists), `Core/Scene.cpp`, `Editor/PropertiesPanel.cpp` + `ViewportPanel.cpp` (gizmo de radio), **`ScriptBindings.cpp` + `LuaApiReference.cpp` + `README.md`**.
- **Esfuerzo:** L · **Riesgo:** Medio-Alto (coste de CPU por voz, y el ciclo de vida de los DSP es otro recurso nativo que liberar) · **Depende de:** P4 (los DSP se cuelgan de los buses).

### P14 · Cerrar los huecos de test — **arregla H14**
- **Qué se gana.** Que H1 y H3 no vuelvan a colarse. Fixtures: round-trip de `minDistance`/`maxDistance`/`playOnAwake`/`is3D`/`loop` con valores **distintos entre sí y no neutros** (el criterio ya escrito en [audio_tests.cpp:132-141](../engine/tests/audio_tests.cpp#L132)); invariante `min <= max` atacado desde los dos setters; y un test que **sabotee** el orden `max`-antes-que-`min` de [Scene.cpp:2418-2419](../engine/src/Core/Scene.cpp#L2418) para comprobar que falla.
- **Ficheros.** `engine/tests/audio_tests.cpp` (y mover los dos tests de collider a `physics_tests.cpp`).
- **Esfuerzo:** M · **Riesgo:** Bajo · **Depende de:** nada.

### Requieren FMOD Studio (fuera de alcance con Core API sola)

- **Snapshots del mixer y transiciones entre ellos** — son un concepto de la herramienta Studio (`FMOD::Studio::EventInstance` + `Bus`), no de Core. Se pueden *emular* con interpolaciones de volumen y parámetros de DSP a mano, pero no es la misma feature.
- **Eventos con parámetros, capas y aleatorización por diseño** (el modelo de trabajo real de un diseñador de sonido con FMOD) — enteramente de Studio.
- **Bancos (`.bank`) y su carga/descarga** — de Studio.
- Todo lo demás propuesto arriba (P1–P14) **es implementable con Core API**, con las verificaciones marcadas.

---

## 5. Fuera de alcance / no merece la pena copiar de Unity

| Capacidad de Unity | Por qué no aquí |
|---|---|
| **AudioMixer visual con grafo de efectos** (el editor de nodos) | Coste de UI desproporcionado para un motor de una persona. Tres buses con volumen + un par de DSP fijos por bus (P4 + P13) cubren el 95% del valor con el 5% del trabajo. |
| **Snapshots y transiciones** | Requieren Studio para hacerse bien. La alternativa casera (interpolar volúmenes desde Lua) ya es posible con P4 + P7 y no justifica una feature con nombre propio. |
| **`AudioClip` como asset importable con ajustes de importación** (compresión, calidad, load type por plataforma) | El motor no tiene pipeline de importación de assets: los ficheros se referencian por path desde el disco ([Scene.cpp:1112](../engine/src/Core/Scene.cpp#L1112)). Un `load type` por clip (P8) da el beneficio principal sin inventar un sistema de assets. |
| **Múltiples `AudioListener`** (Unity los permite pero avisa) | El invariante de uno por escena ya está impuesto en tres capas ([PropertiesPanel.cpp:8084-8094](../engine/src/Editor/PropertiesPanel.cpp#L8084), [Scene.cpp:2729-2739](../engine/src/Core/Scene.cpp#L2729), [AudioListenerComponent.h:6-8](../engine/include/DonTopo/Audio/AudioListenerComponent.h#L6)) y es la decisión correcta: FMOD soporta varios listeners, pero el caso de uso (pantalla partida) no existe en este motor. |
| **`AudioSource.timeSamples`** | Índice en muestras del PCM; para un motor sin herramienta de edición de audio, `time` en segundos (P7) es todo lo que se va a usar. |
| **`PlayScheduled` / música por capas sincronizada al reloj del DSP** | Feature de juego musical. Alto coste conceptual (relojes de DSP, latencia), cero demanda hoy. Reabrir si aparece un caso concreto. |
| **`bypassEffects` / `bypassListenerEffects`** | Suena barato y no lo es. Para que una voz esquive los efectos de su bus **sin** esquivar tambien su volumen, el grafo tiene que partirse en dos niveles: master -> masterFx -> buses, con el grupo de bypass colgando del master directamente. Eso reestructura el enrutado que P13 acaba de asentar (y que ya tiene tests) para servir a un caso de nicho —la voz del narrador que no debe amortiguarse bajo el agua— que nadie ha pedido todavia. Reabrir cuando aparezca ese caso: con el grafo en dos niveles, el resto es un bool por clip. |
| **`AudioSource.priority`** | Con 512 voces ([AudioManager.cpp:29](../engine/src/Audio/AudioManager.cpp#L29)) y escenas de esta escala, el robo de voces no ocurre. Reabrir si alguna vez se satura. |
| **Ambisonics, spatializer plugins, HRTF** | Territorio de VR. Fuera del perfil del motor. |

---

## 6. Plan sugerido

> **ESTADO A 2026-08-28: PLAN COMPLETADO.** Los 16 hallazgos cerrados y las 14 propuestas
> ejecutadas (P8 partida en a/b, P5 en dos entregas). Despues del plan se cerraron ademas
> tres filas de la comparativa que no eran hallazgos: `mute`, pausa global y `time`.
>
> Lo que queda Ausente esta **descartado con criterio** en la seccion 5: `priority`,
> `timeSamples`, `PlayScheduled`, `bypassEffects`, snapshots del mixer (Studio), el
> AudioMixer visual, multiples listeners y ambisonics.
>
> **Lo unico realmente pendiente es la verificacion manual en el editor**, que ningun test
> headless puede cubrir: seguimiento 3D con un objeto en movimiento, preview del inspector
> en Edit Mode, el enrutado por bus (clip en Music + Music Volume a 0 mientras otro en SFX
> se sigue oyendo), la persistencia de los tres volumenes en `project.json`, el doppler de
> un objeto cruzando delante, y las zonas de reverb al entrar y salir de una.
>
> Los dos huecos de cobertura que se decidieron NO tapar con API artificial siguen
> anotados en el codigo: el enrutado de la voz al bus correcto (`groupForBus`) y el efecto
> audible del seguimiento 3D.
>
> El plan original se conserva debajo como registro de lo que se hizo y en que orden.


### Fase 1 — Rápido (S/M, sin tocar serialización)

Objetivo: que lo que ya existe deje de mentir y de fallar en silencio.

1. **P3** (S) — decidir y aplicar el gate del listener; alinear README, header y mensaje del Log.
2. **P10** (S) — sacar `audio.update` del `if (isPlaying())` en las dos rutas del editor.
3. **P9** (M) — `.at()` → `.value()` con warning; `init()` reentrante y con limpieza; SKIP alcanzable en los tests.
4. **P2** (M) — estado de carga público + pump + aviso al Log y marca roja en el inspector.
5. **P14** (M) — fixtures de distancias, `playOnAwake`, `is3D`/`loop` y el sabotaje del orden max/min. *(Va aquí y no al final: es la red que sostiene todo lo demás.)*

Al terminar la fase 1 se cierran H2, H3, H4, H7, H8, H9 y H14.

### Fase 2 — Medio (M, empieza a añadir capacidad)

6. **P1** (M) — seguimiento 3D por frame. Cierra H1, el hallazgo de mayor impacto funcional.
7. **P6** (M) — comandos de undo para alta/baja y checkboxes. Cierra H5 y H6.
8. **P7** (M) — completar la API Lua (incluido `IsPlaying`, casi gratis) + los tres sitios de la convención. Cierra H15.
9. **P5** (M) — `PlayOneShot` sobre el componente (la variante sin GameObject espera a P8).

Al terminar la fase 2 se cierran H1, H5, H6, H15 y la fila de one-shots.

### Fase 3 — Grande (L, toca serialización y arquitectura)

10. **P4** (L) — buses Master/Music/SFX con enrutado por clip, persistencia en `project.json` y UI. Cierra H13.
11. **P8** (L) — caché por path con refcount, reciclado de slots y modo de carga elegible. Cierra H10 y H11.
12. **P11** (S) — retirar la API de BGM una vez P4+P8 la sustituyen. Cierra H12.
13. **P5 (2ª mitad)** — `Audio.PlayClipAtPoint` global, ahora que la caché lo hace barato.
14. **P12** (L) — rolloff, spread, pan y doppler (este último exige P1, ya hecho).
15. **P13** (L) — DSP por bus y `ReverbZoneComponent`.

**Orden de ejecución recomendado:** 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 9 → 10 → 11 → 12 → 13 → 14 → 15.
La única reordenación defendible es adelantar **P1** al final de la fase 1 si urge una demo con audio 3D en movimiento; el resto de dependencias (P12←P1, P8←P1, P13←P4, P11←P4+P8) son duras.
