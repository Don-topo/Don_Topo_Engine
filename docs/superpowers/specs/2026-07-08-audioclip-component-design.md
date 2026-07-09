# Diseño: componente AudioClip desde el editor

## Objetivo

Permitir añadir/eliminar un componente AudioClip (un único clip de audio por
GameObject) desde el panel Properties del editor ImGui, con dos vías de
entrada (botón "Browse..." con file dialog, o drag&drop desde Content
Browser), formatos restringidos a `.wav/.mp3/.ogg/.flac`, checkbox "Loop" y
checkbox "Is 3D?" que controlan de verdad la reproducción FMOD, y
botones Play/Stop de previsualización dentro del editor.

## Contexto actual

- `AudioManager` (`AudioManager.h/.cpp`) es standalone, vive solo en
  `sandbox/src/main.cpp:114` — no está inyectado en `EditorUI`/`Renderer`
  como sí lo están `PhysicsManager` (`Renderer.h:37`,
  `EditorUI.h:44`). API actual: `loadSound(path, is3D=true)` (mode
  horneado `FMOD_3D|FMOD_LOOP_OFF` o `FMOD_2D|FMOD_LOOP_OFF`,
  `AudioManager.cpp:69-81`), `playSound(id, worldPos={})`
  (`AudioManager.cpp:97-111`) — no guarda el `FMOD::Channel*` devuelto,
  no hay forma de pararlo ni de saber si sigue sonando. No existe
  `unloadSound`.
- Patrón de componente físico a replicar: `BoxCollider` (`BoxCollider.h`)
  — construido con handles ya creados por `PhysicsManager`, encapsula toda
  la lógica de "cómo se usa ese handle" (setUseGravity, teleport, etc.) para
  que `EditorUI` no toque PhysX directamente. `PhysicsManager` expone
  factories `createXColliderComponent(...) -> shared_ptr<X>`
  (`PhysicsManager.h:23,29,35,42`). `AudioClipComponent` sigue el mismo
  contrato: factory en `AudioManager`, lógica de recarga/reproducción
  encapsulada en el componente.
- `GameObject` slots existentes: `m_mesh`/`m_boxCollider`/etc. son
  `std::shared_ptr<T>` privados con trío `set/get/has` público
  (`GameObject.h:22-24,73-77`). `AudioClip` sigue el mismo patrón, campo
  nuevo (no hay exclusión mutua con nada existente).
- Patrón "Add" con gate de sección oculta: `m_meshAddRequestedFor`
  (`EditorUI.h:172-177`) + `drawMeshSection()` (`EditorUI.cpp:946-951`) +
  entrada en `drawAddComponentButton()` (`EditorUI.cpp:1070-1074`,
  `BeginDisabled` si ya existe el componente). `AudioClip` replica esto
  exactamente: `m_audioClipAddRequestedFor`, `drawAudioClipSection()`,
  entrada "Audio Clip" en el popup Add deshabilitada si `hasAudioClip()`.
- `ImGuiFileDialog`: **nunca compartir el singleton** `IGFD::FileDialog::Instance()`
  (usado por Content Browser embebido). Bug ya sufrido con Mesh: compartir
  instancia rompía el estado interno al redimensionar. AudioClip usa su
  propia instancia `std::unique_ptr<IGFD::FileDialog> m_audioFileDialog`
  (mismo patrón que `m_meshFileDialog`, `EditorUI.h:102`), key plana sin
  `##` (el bug del `##` duplicado en el título rompía el ID de ImGui,
  `EditorUI.cpp:984-993`), drenado cada frame vía `drawAudioClipDialog()`
  incondicional (igual que `drawMeshDialog()`, `EditorUI.cpp:1011-1026`) —
  si no se drena cada frame, cambiar de selección con el diálogo abierto lo
  deja atascado.
- Drag&drop de asset → GameObject ya existe con payload `"DT_ASSET_PATH"`:
  fuente en Content Browser solo para `.fbx` hoy (`EditorUI.cpp:1163-1169`),
  target en la drop-zone de Mesh (`EditorUI.cpp:997-1005`). El Content
  Browser ya reconoce `.wav/.mp3/.ogg/.flac` para el icono "SFX"
  (`EditorUI.cpp:1146-1147`) pero no los habilita como *drag source* — hay
  que extender el `if (ext == ".fbx" ...)` para incluirlos.
- **No existe serialización de escena** (ningún componente se
  guarda/carga hoy) — `AudioClip` no necesita cubrir ese caso.
- **Bug de ordering de destrucción detectado en `sandbox/main.cpp`:**
  `audio` se declara en la línea 114, DESPUÉS de `root` (línea 39). En C++
  la destrucción es en orden inverso a la declaración: `audio` se destruiría
  ANTES que `root`. Al destruirse `root`, los `AudioClipComponent` que
  cuelguen de sus `GameObject` llamarían `m_audio->unloadSound(...)` sobre
  un `AudioManager` ya destruido (use-after-free). Mismo problema ya
  resuelto para `physics`, que se declara ANTES de `root` con un comentario
  explícito al respecto (`main.cpp:32-35`). Fix: mover la declaración +
  `audio.init()` junto a `physics.init()`, antes de `GameObject root`.

## Arquitectura

### `AudioManager` — extensiones

```cpp
// AudioManager.h
int  loadSound(const std::string& path, bool is3D = true, bool loop = false);
void unloadSound(int id);            // nuevo
void playSound(int id, const glm::vec3& worldPos = {});
void stopSound(int id);              // nuevo

std::shared_ptr<class AudioClipComponent>
     createAudioClipComponent(const std::string& path, bool is3D, bool loop); // nuevo, factory
```

- `loadSound`: `mode = (is3D ? FMOD_3D : FMOD_2D) | (loop ? FMOD_LOOP_NORMAL : FMOD_LOOP_OFF)`.
  Sigue empujando a `m_sounds` (no reutiliza huecos — mismo estilo simple
  que ya tenía).
- Nuevo `std::vector<void*> m_sfxChannels` (paralelo a `m_sounds`,
  `FMOD::Channel*` de la última reproducción de cada id). `playSound`
  guarda el canal devuelto por `SYS->playSound(...)` ahí.
- `unloadSound(id)`: si `m_sounds[id]` no es null, `release()` y pone a
  `nullptr` (tanto el sonido como el canal cacheado). Guard de rango
  idéntico al resto de métodos (`id < 0 || id >= size()`).
- `stopSound(id)`: si hay canal cacheado, `channel->stop()` (FMOD trata un
  handle de canal ya parado/reciclado como error silencioso, no crashea).
- `createAudioClipComponent`: `int id = loadSound(path, is3D, loop); if (id < 0) return nullptr; return std::make_shared<AudioClipComponent>(this, path, id, is3D, loop);`

### `AudioClipComponent` (nuevo, `AudioClipComponent.h/.cpp`)

```cpp
class AudioClipComponent {
public:
    AudioClipComponent(AudioManager* mgr, std::string path, int soundId, bool is3D, bool loop);
    ~AudioClipComponent(); // mgr->unloadSound(m_soundId) si mgr no es null

    AudioClipComponent(const AudioClipComponent&) = delete;
    AudioClipComponent& operator=(const AudioClipComponent&) = delete;

    void play(const glm::vec3& worldPos); // mgr->playSound(m_soundId, worldPos)
    void stop();                          // mgr->stopSound(m_soundId)

    void setLoop(bool loop);  // si cambia: unloadSound(old) + loadSound(path, m_is3D, loop) -> nuevo m_soundId
    void setIs3D(bool is3D);  // idem, recarga con el nuevo modo

    bool getLoop() const  { return m_loop; }
    bool getIs3D() const  { return m_is3D; }
    const std::string& getPath() const { return m_path; }

private:
    AudioManager* m_audio;
    std::string   m_path;
    int           m_soundId;
    bool          m_is3D;
    bool          m_loop;
};
```

`setLoop`/`setIs3D` son no-op si el valor no cambia (evita recargas
innecesarias en cada frame de UI). Al recargar, si `loadSound` devuelve -1
(fichero movido/borrado entre medias), el componente queda con
`m_soundId = -1` — `play()` se convierte en no-op silencioso (guard ya
existe en `AudioManager::playSound`).

### `GameObject` — nuevo slot

`GameObject.h`: `#include "DonTopo/AudioClipComponent.h"`, campo privado
`std::shared_ptr<AudioClipComponent> m_audioClip;`, trío público
`setAudioClip(std::shared_ptr<AudioClipComponent>)`,
`getAudioClip() const`, `hasAudioClip() const` — calco exacto de Mesh
(`GameObject.h:22-24`).

### Wiring `AudioManager` hacia `EditorUI`

- `Renderer::setAudioManager(AudioManager* audio) { m_editorUI.setAudioManager(audio); }`
  (mismo patrón que `Renderer::setPhysicsManager`, `Renderer.h:37`).
- `EditorUI::setAudioManager(AudioManager* audio) { m_audio = audio; }`,
  campo `AudioManager* m_audio = nullptr;` (mismo patrón que `m_physics`).
- `sandbox/main.cpp`: mover `DonTopo::AudioManager audio; audio.init();`
  a justo después de `physics.init()` (antes de `GameObject root`, ver fix
  de ordering arriba); añadir `renderer.setAudioManager(&audio);` junto a
  `renderer.setPhysicsManager(&physics);`.

## UI — sección "Audio Clip" en Properties

Nueva función `EditorUI::drawAudioClipSection()`, llamada desde
`drawProperties()` junto a las demás secciones de componente, antes de
`drawAddComponentButton()`.

**Gate** (oculta hasta pulsar Add, igual que Mesh): campo nuevo
`GameObject* m_audioClipAddRequestedFor = nullptr;` (`EditorUI.h`). Guard al
inicio de la función: `if (!m_selected->hasAudioClip() && m_audioClipAddRequestedFor != m_selected) return;`

**Si `hasAudioClip()`:**

- Header con nombre de fichero (basename de `getPath()`).
- Botones **Play** / **Stop**, deshabilitados (`BeginDisabled`) si
  `m_audio == nullptr`. Play llama `clip->play(worldPos)` con
  `worldPos = glm::vec3(m_selected->worldTransform[3])`.
- Checkbox **Loop** → si cambia, `clip->setLoop(nuevoValor)`.
- Checkbox **Is 3D?** → si cambia, `clip->setIs3D(nuevoValor)`.
- Botón "x" → `m_selected->setAudioClip(nullptr)` (dispara destructor →
  `unloadSound`) + `m_audioClipAddRequestedFor = nullptr` (re-oculta).

**Si NO tiene clip:** botón "Browse..." + drop-zone (`BeginChild` con
borde, texto "Drop audio here"):

- **Browse...**: abre `m_audioFileDialog->OpenDialog("AddAudioDlg", "Choose Audio", ".wav,.mp3,.ogg,.flac", cfg)`
  con `cfg.path = "assets"`. Confirmado vía `drawAudioClipDialog()` →
  `loadAudioClipForSelected(path)`.
- **Drop target**: `AcceptDragDropPayload("DT_ASSET_PATH")` →
  `loadAudioClipForSelected(path)`.

### `EditorUI::loadAudioClipForSelected(const std::string& path)`

Único punto de entrada (Browse + drag&drop), calco de `loadMeshForSelected`:

```cpp
void EditorUI::loadAudioClipForSelected(const std::string& path)
{
    if (!m_selected || !m_audio || m_selected->hasAudioClip()) return;

    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    static const std::set<std::string> kValid = {".wav", ".mp3", ".ogg", ".flac"};
    if (!kValid.count(ext))
    {
        m_audioLoadError = "Formato no soportado: " + ext;
        return;
    }

    auto clip = m_audio->createAudioClipComponent(path, /*is3D=*/false, /*loop=*/false);
    if (!clip) { m_audioLoadError = "No se pudo cargar el audio"; return; }
    m_selected->setAudioClip(std::move(clip));
    m_audioLoadError.clear();
}
```

`m_audioLoadError` (nuevo `std::string`, texto rojo transitorio bajo la
drop-zone) — se limpia también al cambiar `m_selected` (mismo punto donde
se resincronizan las demás cachés, ver `m_meshLoadError`).

Valores por defecto al crear: `is3D=false` (audio "normal" 2D, tal y como
pide el requisito — "normal" es el que viene por defecto), `loop=false`.

### Drag source en Content Browser

`EditorUI.cpp:1163` — extender la condición existente:

```cpp
static const std::set<std::string> kDraggableExt = {".fbx", ".wav", ".mp3", ".ogg", ".flac"};
if (kDraggableExt.count(ext) && ImGui::BeginDragDropSource())
{ ... } // resto sin cambios, mismo payload "DT_ASSET_PATH"
```

## Manejo de errores / crash-safety

- Extensión no soportada → rechazo silencioso con mensaje, sin llamar a FMOD.
- `m_audio == nullptr` (editor sin FMOD o sin wiring) → Browse/drop y
  Play/Stop quedan deshabilitados o son no-op; no crashea.
- `createAudioClipComponent` devuelve `nullptr` (FMOD no pudo abrir el
  fichero) → no se asigna nada a `m_selected`, `hasAudioClip()` sigue
  false, zona de drop sigue visible con mensaje de error.
- `hasAudioClip()` ya true → `loadAudioClipForSelected` es no-op (constraint
  "un AudioClip por GameObject").
- Togglear Loop/Is3D repetidamente → cada cambio libera el sonido anterior
  antes de crear el nuevo (`unloadSound` antes de `loadSound`), sin fuga de
  `FMOD::Sound` acumulada.
- Borrado del componente (botón "x") o del GameObject completo → el
  `shared_ptr<AudioClipComponent>` llega a refcount 0, destructor libera el
  sonido FMOD. Fix de ordering en `sandbox/main.cpp` garantiza que
  `AudioManager` sigue vivo en ese punto.
- `stopSound` sobre un canal ya terminado naturalmente → FMOD devuelve
  error internamente, sin crash (comportamiento documentado de la API).

## Fuera de alcance

- Serialización de escena (no existe para ningún componente todavía).
- Control de volumen por-clip (existe `setSfxVolume` global en
  `AudioManager`, suficiente por ahora).
- Reemplazar un AudioClip existente sin pasar por "x" (remove) primero.
- Reproducción automática en gameplay (`PlayOnStart`, triggers, etc.) —
  esta tarea cubre solo el componente + preview del editor.
- Streaming de audio largo (`FMOD_CREATESTREAM`, usado hoy solo por BGM) —
  AudioClip usa `createSound` normal como el resto de SFX.

## Plan de verificación manual

1. Seleccionar GameObject sin AudioClip → sección Audio Clip oculta hasta
   pulsar Add.
2. Add > Audio Clip → aparece zona "Browse.../Drop audio here".
3. Browse de un `.wav` válido en `assets/` → sección muestra nombre +
   Play/Stop/Loop/Is 3D?.
4. Arrastrar un `.mp3` desde Content Browser hasta la drop-zone de otro
   GameObject → mismo resultado que Browse.
5. Intentar cargar un `.fbx` u otro formato no soportado → mensaje de error
   rojo, no crashea, zona de drop sigue disponible.
6. Play → se oye el audio. Stop → se corta. Repetir varias veces sin crash.
7. Activar Loop, Play → el audio se repite indefinidamente hasta pulsar
   Stop.
8. Activar Is 3D?, mover el GameObject/cámara y Play → el volumen/paneo
   cambia con la distancia/posición relativa (a diferencia de con Is 3D?
   desactivado).
9. Togglear Loop/Is 3D? varias veces seguidas (con y sin reproducción activa)
   → no crashea, no fuga memoria visible en uso prolongado.
10. Click "x" en sección Audio Clip → sección se oculta, zona de drop
    reaparece, Add > Audio Clip vuelve a estar disponible.
11. Añadir AudioClip a un GameObject y borrarlo entero (Delete) → no crash
    al borrar ni al cerrar la app (verifica el fix de ordering).
12. Confirmar que solo se puede tener un AudioClip: con uno ya puesto,
    "Audio Clip" en el popup Add aparece deshabilitado.
