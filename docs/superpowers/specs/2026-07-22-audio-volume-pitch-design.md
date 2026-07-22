# Volumen y pitch por clip, y los bindings Lua que faltaban

## Qué se resuelve

`AudioClipComponent` expone hoy `loop`, `is3D` y `playOnAwake`. De esos, Lua
solo ve `SetLoop`/`GetLoop`: `SetIs3D`/`GetIs3D` existen en C++ y no están
bindeados. Y no hay ninguna forma, ni desde el editor ni desde script, de que
dos clips suenen a distinto volumen: `AudioManager` solo tiene volúmenes de
grupo (master, sfx, bgm), que afectan a todos por igual.

Esta spec añade `volume` y `pitch` como propiedades del componente y cierra el
hueco de los bindings.

## Decisiones tomadas

| Decisión | Elegido | Por qué |
|---|---|---|
| Naturaleza de volume/pitch | Propiedad del componente | Se serializan, salen en Properties y se aplican en cada `play()`, igual que `loop`/`is3D`/`playOnAwake`. Un control de runtime puro no sobreviviría al guardado de escena. |
| Rangos | volume `[0, 1]`, pitch `[0.5, 2]` | Lo mismo que hace Unity. `pitch = 2.0` es una octava arriba y el doble de velocidad: así funciona `FMOD::Channel::setPitch`. |
| Cambio en caliente | Sí, se aplica al canal que ya suena | Habilita fades y motores que aceleran desde Lua, y hace que mover el slider con el sonido puesto se oiga al momento. |
| Undo | Solo en los sliders nuevos | El resto de la sección Audio (Loop, Is 3D, Play On Awake) no tiene undo hoy; añadírselo queda fuera de esta feature. |
| Dónde vive la llamada a FMOD | En `AudioManager` | `AudioClipComponent` guarda un `soundId` y delega; no incluye FMOD ni lo va a incluir. |

## Arquitectura

### AudioClipComponent

Dos campos nuevos, `m_volume` y `m_pitch`, ambos a `1.0f` por defecto.

```cpp
void  setVolume(float v);   // clamp a [0, 1]
void  setPitch (float p);   // clamp a [0.5, 2]
float getVolume() const;
float getPitch()  const;
```

A diferencia de `setLoop`/`setIs3D`, estos **no llaman a `reload()`**. `loop` e
`is3D` van horneados en el `FMOD_MODE` del sonido y cambiarlos obliga a
descargar y volver a cargar; volumen y pitch son propiedades del **canal**, que
se pueden mutar mientras suena.

El clamp vive en el setter, no en la UI: el core es quien garantiza el rango,
y así Lua no puede colar un valor fuera de él. Un valor fuera de rango se
recorta en silencio, no es un error.

### AudioManager

```cpp
void setChannelVolume(int soundId, float v);
void setChannelPitch (int soundId, float p);
```

Simétricos a `stopSound`: misma guarda de índice y de canal nulo.

FMOD recicla los `Channel*`. Un puntero guardado en `m_sfxChannels` puede
apuntar a un canal que ya terminó y que FMOD reasignó a otro sonido — escribirle
el volumen le cambiaría el volumen a un sonido ajeno. Antes de tocar nada, los
dos setters comprueban `isPlaying()` y que `getCurrentSound()` sea el sonido de
ese `soundId`. Si no coincide, no se escribe: el valor ya está guardado en el
componente y se aplicará en el siguiente `play()`.

(`stopSound` vive hoy con ese mismo riesgo y esta spec no lo toca: parar un
canal ajeno es menos grave y arreglarlo es otro cambio.)

### playSound

Cambia de firma para recibir los valores:

```cpp
void playSound(int id, const glm::vec3& worldPos = {},
               float volume = 1.0f, float pitch = 1.0f);
```

Y de orden de operaciones: hoy arranca el canal sonando y **después** le pone
los atributos 3D. Pasa a arrancarlo con `paused = true`, aplicar volumen, pitch
y posición, y despausar. Sin eso, el primer instante de cada clip sonaría al
volumen anterior.

De paso arregla un defecto que ya existe: hoy un clip 3D suena un instante
desde el origen del mundo antes de que se le diga dónde está.

`AudioClipComponent::play` pasa sus propios `m_volume` y `m_pitch`.

### Serialización

Dos campos más en el objeto `audioClip`:

```json
{ "path": "...", "loop": false, "is3D": true,
  "playOnAwake": false, "volume": 1.0, "pitch": 1.0 }
```

En la carga, `c.value("volume", 1.0f)` y `c.value("pitch", 1.0f)`: mismo patrón
de back-compat que ya usa `playOnAwake` en `Scene.cpp`. Las escenas guardadas
antes de este cambio cargan con los valores neutros.

### Editor

Dos `SliderFloat` en la sección Audio Clip de `PropertiesPanel`, bajo los
checkboxes: `Volume` en `0..1` y `Pitch` en `0.5..2`.

Un drag continuo no puede empujar un comando por frame. Se sigue el patrón que
ya usa Transform: capturar el valor al empezar el drag y empujar un único
comando al soltar, con el valor previo y el nuevo.

### Bindings Lua

En el usertype `AudioClip`, con el mismo cuerpo que los cuatro que ya existen
(resolver el `GameObject`, lanzar si ya no tiene `AudioClip`):

```lua
clip:SetVolume(v)   clip:GetVolume()
clip:SetPitch(p)    clip:GetPitch()
clip:SetIs3D(b)     clip:GetIs3D()
```

`SetVolume` y `SetPitch` son seguros de llamar por frame: solo escriben un
valor en el canal.

`SetIs3D` **no** lo es, y hay que decirlo en el README de scripting: recarga el
sonido (descarga y vuelve a cargar) y corta lo que esté sonando. Es
configuración, no algo para meter en un `Update`.

## Tests

Ejecutable nuevo `dt_audio_tests`, con el patrón de los que ya hay (plain main,
`CHECK`, registrado en `engine/tests/CMakeLists.txt` y en la lista que copia
`fmod.dll`).

Todo se construye sin FMOD, con `AudioClipComponent(nullptr, path, -1, ...)` —
hay precedente en `exporter_tests.cpp`.

1. Defaults: componente recién construido → `volume == 1.0f`, `pitch == 1.0f`.
2. Clamp de volumen: `-1 → 0`, `5 → 1`, `0.5 → 0.5`.
3. Clamp de pitch: `0.1 → 0.5`, `10 → 2`, `1.5 → 1.5`.
4. `Scene::toJson` de un GameObject con clip emite `volume` y `pitch`.
5. Un JSON de `audioClip` sin esos campos carga con `1.0` y `1.0` — es la
   back-compat, y lo que se rompe si alguien cambia `.value()` por `.at()`.

### Lo que los tests NO cubren

Que el sonido salga de verdad más bajo o más agudo. `Scene::fromJson` necesita
un `AudioManager` con FMOD inicializado y ningún test de este repo lo tiene.
Queda para verificación manual (ver criterios 4 y 5).

## Criterios de aceptación

1. `build.bat` compila sin errores ni warnings nuevos.
2. Los 6 ejecutables de test pasan al 100%, incluidos los 5 tests nuevos.
3. Una escena guardada antes de este cambio carga con volume y pitch a 1.0.
4. (Manual) Mover el slider de Volume con el clip sonando cambia el volumen al
   momento; el de Pitch, el tono. Ctrl+Z deshace el drag entero, no frame a
   frame.
5. (Manual) Un script Lua que baja `SetVolume` progresivamente en `Update`
   produce un fade audible; `GetIs3D`/`SetIs3D` responden desde Lua.
6. Guardar y recargar la escena conserva los valores.

Los criterios 4 y 5 exigen GUI y audio: los verifica el usuario a mano.

## Fuera de alcance

Undo para los checkboxes que ya existen (Loop, Is 3D, Play On Awake), volumen o
pitch por grupo más allá de los tres que ya hay, curvas de atenuación 3D,
`SetPan`, prioridades de canal, y arreglar el reciclado de `Channel*` en
`stopSound`.
