# Blend 1D de N clips — diseño

Fila 7 del backlog de `docs/animation-audit.md` (C2). Hoy un estado mezcla como mucho dos clips (`clipName` + `blendClipName`), con el peso sacado de un parámetro float remapeado de `[blendMin, blendMax]`.

## Decisión

Se elige en la **CPU** qué pareja de clips suena, y la GPU no se toca. Un blend 1D (el de Unity) nunca mezcla más de los dos clips vecinos del parámetro, y el Renderer ya consume exactamente eso: `poseClipA/B`, `poseTimeA/B` y `poseWeight`. Por eso no hay cambios de shader y la paridad Vulkan/D3D12 queda intacta.

Descartados:
- **Mezcla de N clips en la GPU**: un 1D no la necesita, y el coste sería L con riesgo en los dos backends.
- **Mantener `blendClip` y añadir la lista al lado**: dos representaciones del mismo concepto dan estados imposibles.

## Datos (`AnimatorComponent::State`)

Salen `blendClipName`, `blendClipIndex`, `blendDuration`, `blendMin` y `blendMax`. Entran:

```cpp
struct BlendEntry {
    std::string clipName;
    int         clipIndex = -1;    // lo resuelve bindClips, no se serializa
    float       duration  = 0.0f;  // ticks, cacheado del clip, no se serializa
    float       threshold = 0.0f;
};
std::string             blendParam;            // se conserva
float                   clipThreshold = 0.0f;  // umbral del clip principal
std::vector<BlendEntry> blendEntries;          // clips extra; vacío = estado de un clip
```

`clipName` sigue siendo el clip **principal**: marca el reloj (`animTime` sobre su `duration`), el exit time, `speed` y lo que aporta el estado en un cross-fade. En la búsqueda por umbral es una entrada más, con `clipThreshold`, así que puede quedar en medio de la lista.

## Evaluación

`stateBlends(i)` es true si hay al menos una entrada con `clipIndex >= 0` y `blendParam` es un parámetro Float declarado; si no, el estado se comporta como uno de un clip. Las entradas con `clipIndex < 0` (el clip no existe en el modelo) se ignoran en la búsqueda.

Con el estado mezclando, se forma la lista de candidatos: el principal primero y luego las entradas válidas en su orden. Se ordena **de forma estable** por umbral y, si un candidato repite el umbral de uno anterior, **se descarta** (con umbrales iguales solo cuenta el primero). Con eso los umbrales quedan estrictamente crecientes. Con `p = getFloat(blendParam)`:
- un solo candidato tras descartar: A = B = él, peso 1.
- `p <= t[0]`: A = B = candidato 0, peso 1.
- `p >= t[n-1]`: A = B = el último, peso 1.
- en otro caso, el `i` con `t[i] <= p < t[i+1]`: A = i, B = i+1, peso `(p - t[i]) / (t[i+1] - t[i])`.

Tiempos: `fase = animTime / duration(principal)`, y cada clip se muestrea en `fase * duration(clip)`, como ya hacen hoy los dos del par. Si la duración del principal es 0, el tiempo es 0.

`poseClipA/B`, `poseTimeA/B` y `poseWeight` usan esa pareja. El cross-fade sigue mandando sobre el blend del estado, igual que hoy: cada lado aporta su clip principal.

Cuando A == B, el resultado tiene que coincidir con el camino de un solo clip (peso 1). Esa condición es la que evita un caso especial en el Renderer.

## Serialización (`Scene.cpp`)

Formato nuevo; solo se escribe si `blendEntries` no está vacío:

```json
"blendParam": "Speed",
"clipThreshold": 0.0,
"blendEntries": [ { "clip": "Run", "threshold": 1.0 } ]
```

**Migración al cargar**: si no hay `blendEntries` pero sí `blendClip` no vacío, `clipThreshold = blendMin` y se crea una entrada `{blendClip, blendMax}`. La **pose** resultante es idéntica a la de antes:
- `blendMin < blendMax`: la misma pareja y el mismo peso.
- `blendMin > blendMax`: hoy un span negativo invierte el remapeo. Al ordenar por umbral, A y B salen **intercambiados** y con el peso complementario, así que cada clip aporta lo mismo.
- `blendMin == blendMax`: hoy el peso es siempre 0 (`stateBlendWeight`, `AnimatorComponent.cpp:358`) y solo suena el principal. Con umbrales iguales la entrada se descarta, así que también suena solo el principal. Los umbrales se leen con `readFloat`: un valor no finito avisa y usa el default. Al guardar nunca se vuelve a escribir `blendClip`, `blendMin` ni `blendMax`.

`animatorGraphKey` sigue saliendo del JSON, así que el undo del grafo cubre las ediciones de blend sin código adicional.

## Clips

- `bindClips` resuelve `clipIndex` y `duration` de cada entrada; si falta, avisa igual que hoy (`"blend clip '<n>', que no existe en el modelo"`).
- `renameClip` recorre las entradas.
- Quitar una fuente de animación: `rebindClips` deja a -1 las entradas cuyo clip ya no existe, sin borrarlas. Es la misma política que hoy con `blendClipName`.

## Editor (`AnimatorPanel.cpp`)

En el nodo del estado:
- botón del parámetro de blend (popup Float, como hoy);
- `DragFloat` "threshold" del clip principal, visible si hay entradas;
- una fila por entrada: botón del clip (popup de clips), `DragFloat` del umbral y botón "x" para quitarla;
- botón "+ blend clip", que añade una entrada sin clip con umbral `max(umbrales) + 1`.

Los popups se abren entre `ed::Suspend()`/`ed::Resume()`, igual que el `drawBlendPickPopup` actual, que pasa a llevar el índice de la entrada. Cada fila va con `PushID(índice)`. Una entrada sin clip resuelto se pinta en rojo, como hoy el par.

## Fuera de alcance

- API de Lua para el blend. Hoy no existe y no se añade.
- Blend 2D y umbrales automáticos (fila 14).
- Mezclar el blend dentro de un cross-fade (limitación ya existente).
- Duración ponderada del ciclo (Unity la usa): el reloj sigue siendo el del principal, como hoy.

## Tests (`animator_tests.cpp`, uno por regla y cada uno con su sabotaje)

1. Tres entradas: el parámetro entre los umbrales 2 y 3 da A/B = clips 2 y 3 con peso lineal.
2. Por debajo del primer umbral y por encima del último: A == B == el extremo, peso 1.
3. Principal con el umbral del medio: el orden lo decide el umbral, no la posición en la lista.
4. Umbrales iguales: gana el primero (el principal frente a una entrada, o la entrada anterior frente a la siguiente), también con `p` por encima.
5. Entrada con clip inexistente: se ignora; si no queda ninguna válida, el estado es de un clip.
6. Tiempos: la fase del principal se aplica a la duración de cada clip.
7. Migración: el JSON viejo con `blendClip`/`blendMin`/`blendMax` (span positivo, negativo y 0) da, para varios valores de `p`, el **mismo peso por clip** que la fórmula vieja. Se compara el peso de cada clip, no A/B, porque con span negativo A y B salen intercambiados.
8. Ida y vuelta por JSON del formato nuevo, y que al guardar no se escribe `blendClip`.
9. `renameClip` renombra una entrada; `rebindClips` sin el clip la deja a -1 sin borrarla.

Verificación manual en los dos backends: un estado idle/walk/run con un parámetro Speed y un slider en Play; transiciones suaves entre los tres. También hay que cargar una escena guardada con el formato viejo.
