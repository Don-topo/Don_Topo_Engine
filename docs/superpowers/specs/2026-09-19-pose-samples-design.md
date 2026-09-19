# Pose por muestras ponderadas — diseño

Fila 13 de `docs/animation-audit.md` (A3, A4). Base de las capas (fila 14).

## Problema

`bone_eval.comp` recibe exactamente dos clips (A, B) y un peso. Consecuencias:

- **A3.** En un cross-fade cada lado aporta solo su clip **principal**: salir de un estado con blend (walk/run con peso 1 = run) hace saltar la pose a walk en el primer frame del fade; entrar en uno da el salto contrario al acabar.
- **A4.** Una transición con otro fade en vuelo descarta la mezcla anterior y la pose salta al estado origen puro.

## Decisión (enfoque B, elegido por el usuario)

`bone_eval` suma **hasta 4 muestras ponderadas** (clip, tiempo, peso), más opcionalmente una **pose congelada** (la que había en pantalla) con su propio peso. El estado que sale sigue animándose durante un fade, como ahora; la pose congelada solo se usa al **interrumpir** un fade.

## CPU: `AnimatorComponent`

```cpp
struct PoseSample { int clip; float time; float weight; };   // time en ticks del clip
struct AnimationPose
{
    PoseSample samples[4] = {};
    int        count = 0;
    float      frozenWeight = 0.0f;   // 0 = no se usa
    bool       freezeNow = false;     // este frame: congelar la pose actual ANTES de evaluar
    uint32_t   rootMotionMode = 0;
};
AnimationPose pose() const;
```

- `stateBlendPair(stateIdx)` pasa a `stateBlendPair(stateIdx, float animTime)`, para evaluar el estado que se apaga con su propio reloj (`m_prevAnimTime`). Las llamadas actuales pasan `m_animTime`.
- **Sin fade:** la pareja del estado actual: una muestra de peso 1 si `clipA == clipB`, o dos con `1−a` y `a`.
- **Fade normal** (`w = blendWeight()`): las del estado que sale (su pareja con su reloj) multiplicadas por `1−w`, más las del que entra multiplicadas por `w`. Como mucho 4.
- **Fade interrumpido:** si `startTransitionTo` arranca un fade con otro en vuelo, en lugar de descartar se pasa a modo congelado: `m_frozenFade = true`, `m_freezePending = true` y se descarta el estado previo (`m_prevState = -1`, como hoy). Mientras dure ese fade: `frozenWeight = 1−w` y las muestras del estado nuevo con `w`. `freezeNow` vale `m_freezePending`, que baja en el siguiente `update`: así se congela una sola vez y en el frame correcto. Un corte seco (`duration == 0`) no congela nada.
- Cuando el fade termina, `frozenWeight` vuelve a 0 y `m_frozenFade` se apaga.
- Muestras con peso ≤ 0 no se emiten; los pesos (muestras + congelada) suman 1.
- `rootMotionMode` = el del estado destino, como hoy.

`poseClipA/B`, `poseTimeA/B`, `poseWeight` y `poseRootMotionMode` **se conservan** para Lua (`GetPoseWeight`) y los tests existentes, con la semántica de siempre (la pareja principal). Su comentario pasa a decir que describen la pareja principal y que **lo que va a la GPU es `pose()`**.

## Backends

- `EditorRenderer::setAnimationBlend(...)` → `setAnimationPose(int index, const AnimationPose&)`, en los dos backends y en `applySkinnedFrame`. El camino sin Animator (`updateAnimation`/`setAnimationState`) queda como una muestra de peso 1.
- Cada índice de clip se acota con `clampClipIndex`, como hoy.
- **Push constant** compartido por los tres shaders de skinning, 17 palabras de 4 bytes (68 B, por debajo de 128 en Vulkan y de 64 palabras en la root signature de D3D12): `boneCount`, `vertexCount`, `sampleCount`, `rootMotionMode`, `frozenWeight`, y 4×(`clipBase`, `time`, `weight`). `bone_hierarchy.comp` y `skinning.comp` declaran el mismo bloque, con sus campos como relleno.
- **Dos buffers nuevos por personaje**, TRS por hueso (pos vec4, rot vec4, escala vec4 = 48 B): `poseTrs` (lo escribe `bone_eval` cada frame) y `frozenTrs`. Con `freezeNow`, antes de `bone_eval`, copia `poseTrs → frozenTrs` con sus barreras (Vulkan `vkCmdCopyBuffer`, D3D12 `CopyBufferRegion` con las transiciones de estado).
- El set de compute pasa de 8 a 10 bindings (Vulkan: layout, pool y writes; D3D12: root signature de `bone_eval`).

## GPU: `bone_eval.comp`

Por hueso:
1. Si ninguna muestra tiene canal y `frozenWeight == 0`: `bindLocal`, como hoy.
2. Acumular: `pos += w·p`, `scl += w·s`, y `rot += w·q`, con `q` negada si `dot(q, primera) < 0`; al final se normaliza. Una muestra sin canal en ese hueso aporta TRS identidad, como hoy en la mezcla.
3. Con `frozenWeight > 0`, la congelada entra en la misma suma, leída de `frozenTrs`.
4. Normalizar la rotación, `applyRootLock` sobre la posición, y escribir `localXforms` (mat4) **y** `poseTrs`.

Con dos muestras la rotación deja de ser `slerp` y pasa a suma normalizada (nlerp). Para ángulos pequeños entre clips la diferencia es despreciable, pero no es cero: la referencia en CPU de los tests (`evalLocalXformBlended`) se actualiza a la misma fórmula.

## Tests (`animator_tests.cpp`)

1. **Muestras sin fade:** estado sin blend = 1 muestra de peso 1; con blend = 2 con `1−a` y `a`.
2. **Fade entre dos estados con blend:** 4 muestras, pesos `(1−w)(1−a)`, `(1−w)a`, `w(1−b)` y `wb`; las dos primeras con el reloj del estado que sale.
3. **Interrupción:** `freezeNow` es true en el frame de la interrupción y false en el siguiente; `frozenWeight = 1−w` y el estado previo ya no aporta muestras.
4. **Criterio de la fila 13, A3:** con la referencia de evaluación en CPU sobre `pose()`, el salto máximo de una articulación entre el último frame antes de una transición desde un estado con blend y el primer frame del fade es menor que ε. Contra la lógica de hoy (pareja principal) este test falla.
5. **Criterio de la fila 13, A4:** lo mismo al interrumpir un fade, con la referencia guardando la pose del frame anterior como congelada.
6. **Pesos:** la suma de pesos (muestras + congelada) es 1 en todos los casos.
7. `applySkinnedFrame` pasa `pose()` completo al doble del backend.

Cada uno con su sabotaje. **Manual**, en los dos backends: un fade desde un estado con blend y la interrupción de un fade no saltan; el resto del Animator se ve igual.

## Fuera de alcance

- Capas con máscara (fila 14): encajan como muestras con máscara por hueso, pero no se hacen aquí.
- El root motion durante un fade desde un estado con blend sigue usando el clip principal del que sale (`rootMotionSamples`).
- La interrupción de una interrupción: vuelve a congelar la pose actual y funciona igual, pero no tiene test propio.
