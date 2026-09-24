# Ajustes de importación por clip de audio — Diseño

Fecha: 2026-09-24. Continúa `docs/assets-editor-audit.md` U8 (import settings por
asset). Las texturas ya lo tienen (`docs/superpowers/specs/2026-09-24-texture-import-settings-design.md`,
en `main`); esta spec añade el audio con el mismo sidecar.

## Objetivo

Que un clip de audio (`.wav/.mp3/.ogg/.flac`) pueda llevar dos ajustes **de fichero**,
persistentes, editables desde el Content Browser y respetados por el editor y por el
juego exportado:

- **Ganancia** en dB, [-30, +12], 0 por defecto: se multiplica al volumen de toda voz
  de ese fichero.
- **Forzar a mono**, apagado por defecto: la voz mezcla sus canales a uno solo.

Semántica decidida con el usuario: son propiedades del **fichero**, no defaults de
componente. No tocan ni sustituyen ningún campo de `AudioClipComponent` (loop, Is 3D,
Load Mode, rolloff, min/max, bus, volumen, pitch, spread, pan, doppler siguen siendo
del componente y se siguen editando en Properties).

Fuera de alcance: normalizar (exige decodificar el fichero entero), calidad o
compresión, sample rate, defaults para componentes nuevos, undo del cambio de ajustes,
y los ajustes de modelos (siguiente spec de U8).

## Restricciones que salen del código

- **El formato del sidecar ya es extensible por tipo**: `{ "version": 1, "type":
  "texture", ... }` (`engine/src/Core/ImportSettings.cpp`). Hoy `loadTextureImportSettings`
  rechaza cualquier `type` ≠ `texture` como defecto con aviso; el audio añade
  `type: "audio"` con sus propios campos.
- **Lo genérico ya funciona por ruta y vale para el audio sin tocarlo**:
  `importSidecarPath`, `isImportSidecar` (el grid no lista los `.import.json`),
  `moveImportSidecar` / `copyImportSidecar` / `removeImportSidecar` /
  `importSidecarConflict`, y su uso en `moveAsset`, `renameAssetFile`,
  `removeAssetPath` e `importExternalAsset` (`ContentBrowserPanel.cpp`,
  `AssetImport.cpp`).
- **El volumen de una voz se fija en TRES sitios de `AudioManager`**:
  `setChannelVolume` (`AudioManager.cpp:404`), `playSound` (`:451`, `ch->setVolume(volume)`
  en `:482`) y `playSoundOneShot` (`:646`, `:668`). `PlayClipAtPoint` (`:723`) y el
  componente (`:1091`) llegan a ellos. Si la ganancia la aplicaran los llamantes, un
  llamante nuevo la olvidaría en silencio.
- **Ganancia y mono son propiedades de la VOZ, no del `FMOD_MODE` del sonido**:
  `loadSound` (`:237`) hornea 3D/loop/stream/rolloff en el modo y los mete en
  `soundKey` (`:171`); estos dos no entran ahí, así que **cambiarlos no recarga el
  sonido** y no cambia la clave de caché de sonidos.
- **Los sonidos se comparten por clave** (`m_soundByKey`, `m_soundRefs`): el mismo
  fichero con los mismos flags es UN `FMOD::Sound` con varios `AudioClipComponent`.
  Los datos por sonido viven en vectores paralelos por `soundId` (`m_soundPaths`,
  `m_soundKeys`, ... `AudioManager.h:352-361`) que se reciclan con `m_freeSlots`.
- **FMOD carga en su hilo** (`FMOD_NONBLOCKING`): `loadSound` devuelve el id antes de
  que el fichero esté listo. Leer el sidecar en `loadSound` es lectura de un fichero
  de ~100 bytes, no de audio.
- **`EditorContext` ya expone `AudioManager* audio`** (`EditorContext.h:46`).
- **El exportador copia por lista** (`GameExporter.cpp`, `collectSceneAssets`): añade el
  clip del `AudioClipComponent` con `add(go->getAudioClip()->getPath())`; el sidecar no
  viajaría hoy.
- **Sin FMOD el audio no existe** (`DT_FMOD_ENABLED`): `loadSound` devuelve -1. Los
  tests de audio hacen SKIP si `AudioManager::available()` es falso.

## Diseño

### 1. Modelo y sidecar (Core)

Se amplía `Core/ImportSettings.h/.cpp` (mismo módulo, mismos helpers de ciclo de vida):

```cpp
struct AudioImportSettings { float gainDb = 0.0f; bool forceMono = false; };
inline constexpr float kAudioGainMinDb = -30.0f, kAudioGainMaxDb = 12.0f;
bool  operator==(const AudioImportSettings&, const AudioImportSettings&);
bool  isDefault(const AudioImportSettings&);
float audioGainLinear(float gainDb);            // 10^(dB/20), con el dB ya acotado
AudioImportSettings loadAudioImportSettings(const std::filesystem::path& asset, std::string* warning = nullptr);
bool saveAudioImportSettings(const std::filesystem::path& asset, const AudioImportSettings&, std::string* error = nullptr);
```

Fichero: `{ "version": 1, "type": "audio", "gainDb": 0.0, "forceMono": false }`.

- **Solo existe si difiere del defecto** (guardar el defecto lo borra), escritura por
  fichero temporal + `rename`, igual que las texturas.
- **Lectura tolerante, nunca lanza**: ausente (sin aviso), JSON roto, tope de 64 KiB,
  `version` ≠ 1, `type` ≠ `audio`, campo con tipo equivocado → defecto y aviso. Un
  campo suelto malo deja el otro leído. `gainDb` fuera de rango o no finito
  (`NaN`, `inf`) se **acota** a [-30, +12] (NaN → 0) con aviso: una ganancia infinita
  reventaría el oído del usuario.
- **Cruce de tipos**: un sidecar `texture` leído como audio (y al revés) da el defecto
  con aviso; nunca se interpreta con los campos del otro tipo. El tope de tamaño y el
  parseo tolerante se comparten con el lector de texturas (una única función interna).
- `saveTextureImportSettings` y `saveAudioImportSettings` no se pisan: un fichero de
  audio nunca recibe un sidecar `texture` porque la UI decide el tipo por extensión.

### 2. Consumo en `AudioManager`

- **Datos por sonido**: `std::vector<AudioImportSettings> m_soundImport` (paralelo a
  `m_sounds`, inicializado en el slot al crear o reciclar, como el resto de vectores
  paralelos). `loadSound` lee `loadAudioImportSettings(path)` **solo cuando crea un
  sonido nuevo** (no en un acierto de caché: el sonido compartido ya lo tiene), y
  vuelca el aviso, si lo hay, a stderr con el prefijo `[AudioImport]`.
- **Ganancia**: helper interno `voiceVolume(id, volume)` = `volume * audioGainLinear(gainDb)`.
  Los tres sitios (`setChannelVolume`, `playSound`, `playSoundOneShot`) llaman a ese
  helper: **es el único sitio donde se multiplica**. Un test de política (grep) exige
  que no quede ningún `->setVolume(volume)` de un canal de voz fuera del helper.
- **Mono**: al arrancar cada voz (en `playSound` y `playSoundOneShot`, antes de
  despausarla), si el sonido está marcado, `Channel::setMixMatrix` con una matriz que
  reparte cada canal de entrada a todas las salidas a igual nivel (mezcla a mono
  conservando la energía). Función interna `applyForceMono(channel, sound)`. Un clip
  ya mono no cambia. En 3D, la espacialización sigue aplicándose después.
- **Cambiar ajustes en caliente**: `void AudioManager::refreshImportSettings(const
  std::string& path)`. Recorre los soundId **vivos** cuya `m_soundPaths[id]` sea esa
  ruta (misma comparación de rutas que ya usa el proyecto, `samePath`), recarga su
  sidecar y, para cada uno, reaplica el volumen a su voz viva (`ch->setVolume(
  voiceVolume(id, volumeActual))`, con el volumen actual leído del canal ÷ ganancia
  vieja, o guardado por sonido). Los sonidos de **otras rutas** no se tocan. El mono
  aplica desde la siguiente reproducción (igual que spread y pan, con la misma
  limitación ya documentada en `AudioClipComponent.h`).
- **Sin sidecar todo queda igual que antes**: `gainDb = 0` → factor 1.0 exacto, sin
  `setMixMatrix`. Los tests de audio existentes no cambian.
- Sin FMOD, `refreshImportSettings` es un no-op.

### 3. UI (Content Browser)

El menú contextual "Import Settings…" ya existe para imágenes. Se amplía a los audios
(`classifyAsset` → `AssetKind::Audio`, con **una** sola selección). El modal muestra,
según el tipo del fichero, o bien color/mipmaps o bien: slider **Gain (dB)** de -30 a
+12 (con `DeferredSliderFloat` si aplica, para no escribir en cada frame del arrastre;
mirar el patrón existente) y casilla **Force mono**. Texto de ayuda: "La ganancia
se suma al volumen del componente. Mono aplica desde la próxima reproducción."

**Aplicar** llama a `applyAudioImportSettings(asset, settings, refresh)` (función pura
análoga a `applyTextureImportSettings`): escribe el sidecar (el defecto lo borra) y,
solo si se pudo, llama a `refresh(path)` (que en el panel es
`ctx.audio->refreshImportSettings`); un fallo de escritura se muestra en el modal, que
no se cierra, y no llama a `refresh`. Sin `ctx.audio` solo se escribe.

### 4. Ciclo de vida

Mover, renombrar, borrar, importar y listado: sin cambios (genérico por ruta).
**Exportador**: `collectSceneAssets` añade `<clip>.import.json` junto a cada clip de un
`AudioClipComponent` que lo tenga, con el helper de sidecar que ya usa para texturas
(se generaliza el nombre `addTexture` → `addWithSidecar`, sin cambio de
comportamiento para texturas). El runtime lee el sidecar por la misma ruta que el
clip, dentro del paquete.

## Verificación

Tests planos con `CHECK`, desde la raíz del repo:

- **`import_settings_tests`**: ida y vuelta de audio; defecto = sin fichero; JSON roto,
  versión, tipo, cruce texture↔audio; `NaN`, `inf`, `1e30`, `-1e30` en `gainDb`
  acotados sin lanzar; campo con tipo equivocado deja el otro; ruta Unicode; escribir
  en carpeta inexistente da error; sidecar hostil (5 MB, anidado a 200 000 niveles).
  `audioGainLinear(0) == 1`, `audioGainLinear(-6) ≈ 0.501`, `audioGainLinear(12) ≈ 3.98`.
- **`audio_tests`** (con FMOD; SKIP si no hay): ganancia por soundId leída del canal
  (`Channel::getVolume` = volumen × factor) en `playSound`, `playSoundOneShot` y
  `setChannelVolume`; sin sidecar el volumen es exacto al de antes;
  `refreshImportSettings` cambia el volumen de la voz viva de esa ruta y **no** el de
  otra ruta; dos componentes que comparten sonido ven ambos el cambio; un sonido
  liberado (slot reciclado) no arrastra los ajustes al siguiente clip que lo reutiliza.
- **Política (grep)**: ningún `->setVolume(volume)` de canal de voz fuera de
  `voiceVolume` en `AudioManager.cpp`.
- **`content_browser_tests`**: `applyAudioImportSettings` escribe y llama a `refresh`
  una vez con la ruta; con fallo de escritura no llama; sin `refresh` solo escribe.
- **`exporter_tests`**: el sidecar de un clip viaja con el clip; un clip sin sidecar no
  añade nada.

Verificación manual del usuario, **a oído**: un clip con ganancia -12 dB suena más bajo
y con +6 más alto que sin ajuste; Force mono en un clip estéreo con paneo marcado deja
de tener paneo entre altavoces; Aplicar con el clip sonando reajusta el volumen sin
cortarlo; mover y renombrar conservan los ajustes; el juego exportado los respeta;
cierre limpio. (No se puede comprobar el sonido desde un agente.)

## Riesgos

- **Saturación**: una ganancia positiva alta en un clip ya fuerte puede clipear; FMOD
  no lo limita. Por eso el tope es +12 dB y el valor por defecto 0.
- **`setMixMatrix` con clips de más de dos canales o ya mono**: la matriz se construye
  con los canales reales del sonido (`Sound::getFormat` para el número de canales); si
  no se puede leerlos (sonido aún cargando, `NONBLOCKING`) la voz no arranca de todos
  modos (`playSound` ya sale en `FMOD_OPENSTATE_LOADING`), así que no hay ventana sin
  datos.
- **Volumen "actual" al refrescar**: la voz viva puede haber recibido `setChannelVolume`
  del componente; el refresco debe reaplicar `voiceVolume(id, volumenDelComponente)`,
  no `volumenDelCanal × factorNuevo` (compondría la ganancia dos veces). El plan
  decide de dónde sale el volumen del componente (guardarlo por sonido o pedirlo al
  llamante) y lo cubre con test.
- **Ampliación**: los ajustes de modelos siguen abiertos (última parte de U8).
