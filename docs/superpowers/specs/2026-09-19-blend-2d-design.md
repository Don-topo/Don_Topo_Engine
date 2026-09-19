# Blend tree 2D por triangulación (fila 14a, C3)

Fila 14 del backlog de `docs/animation-audit.md`, partida en dos: 14a (este
documento, C3) y 14b (capas con máscara, C4), que va después y reutiliza lo
de aquí. Base: la pose por muestras ponderadas de la fila 13 (`AnimationPose`,
`bone_eval` suma N muestras).

## Qué se construye

Un estado del Animator puede ser un **blend 2D**: cada clip es un punto
(x, y) y dos parámetros Float dan la posición actual. Los puntos se
triangulan (Delaunay) y suenan los 3 clips del triángulo que contiene el
valor, con pesos baricéntricos. Es el esquema de los Blend Spaces libres de
Unreal: como mucho 3 clips por estado, peso continuo en todo el plano.

## 1. Datos

- `State` gana `std::string blendParamY` y `float clipThresholdY = 0`.
- `BlendEntry` gana `float thresholdY = 0`.
- Un estado es **2D** si tiene al menos una entrada de blend y `blendParamY`
  nombra un parámetro Float declarado; si no, se comporta exactamente como hoy
  (1D si `blendParam` es válido, un clip si no).
- Los puntos del estado son el clip principal (`clipThreshold`,
  `clipThresholdY`) y cada entrada con `clipIndex >= 0` (`threshold`,
  `thresholdY`), en ese orden. Ese orden es el que desempata.
- Serialización: `blendParamY`, `clipThresholdY` y `thresholdY` solo se
  escriben si el estado tiene `blendParamY` no vacío. Una escena sin esas
  claves carga igual que hoy, y la que no usa 2D se guarda idéntica, byte a
  byte.

## 2. Pesos: `stateBlendSamples`

`stateBlendPair(stateIdx, animTime)` pasa a ser
`stateBlendSamples(stateIdx, animTime)`, que devuelve hasta 3 muestras
`{clip, time, weight, duration}` con pesos que suman 1. En 1D devuelve la
pareja de siempre (1 o 2 muestras, mismos valores). El tiempo de cada clip
sale de la fase normalizada del estado, como en 1D:
`phase * duration` del clip.

Triangulación 2D, recalculada en cada llamada (sin caché que invalidar al
editar; con 9 clips son unos cientos de comprobaciones):

1. **Candidatos**: los tríos `i < j < k` no degenerados (área > ε) cuyo
   círculo circunscrito no tiene ningún otro punto **estrictamente** dentro.
2. **Aceptación**: en orden lexicográfico de `(i, j, k)`, un candidato se
   acepta si su interior no solapa el de uno ya aceptado. Con puntos
   concíclicos (una rejilla cuadrada) hay candidatos de las dos diagonales, y
   sin este paso el valor podría caer en triángulos de triangulaciones
   distintas y saltar.
3. **Localización**: si el valor cae dentro (o en el borde) de un triángulo
   aceptado, baricéntricas de ese triángulo.
4. **Fuera del contorno**: el punto más cercano del conjunto de triángulos
   aceptados; sus baricéntricas (2 muestras en una arista, 1 en un vértice).
5. **Degenerado** (ningún triángulo: 2 puntos, o todos alineados): los puntos
   se ordenan por su proyección sobre la recta de los dos más alejados entre
   sí, y el valor se proyecta al segmento consecutivo más cercano (2
   muestras), o al único punto.
6. **Puntos repetidos**: cuenta el primero (el principal antes que las
   entradas); los repetidos no entran en la triangulación.

Las muestras con peso 0 no se emiten. El clip principal sin resolver
(`clipIndex < 0`) es el clip 0, como ya pasa en 1D.

## 3. Pose y GPU

- `AnimationPose::samples` pasa de 4 a **6**: un fade entre dos estados 2D
  son 3 + 3; un fade interrumpido es la congelada + 3.
- `pose()` usa `stateBlendSamples` para el estado actual y el que se apaga,
  cada uno escalado por su peso en el fade (hoy `addPair`).
- El bloque push de los tres `.comp` pasa de 68 a **92 bytes** (23
  escalares: `boneCount`, `vertexCount`, `sampleCount`, `rootMotionMode`,
  `frozenWeight`, `clipBase0..5`, `time0..5`, `weight0..5`), espejado en
  `SkinningPass::Push` y `ComputePush` con `static_assert(... == 92)`. Cabe en
  los 128 bytes garantizados de Vulkan y en las root constants de D3D12.
- `bone_eval.comp`: el helper `muestra(k, ...)` cubre k = 0..5. Nada más cambia.

## 4. Root motion, eventos, accesores legados, Lua

- Root motion del estado actual fuera de fade: una `RootMotionSample` por
  muestra de `stateBlendSamples`, con su peso y sus ticks escalados a su
  duración (hoy lo hace con la pareja). Durante un fade no cambia: sigue
  con el clip principal de cada estado.
- Eventos: van por el tiempo normalizado del estado. Sin cambios.
- `poseClipA/B`, `poseTimeA/B`, `poseWeight` (vista de dos clips para Lua y
  tests): fuera de fade, las **dos muestras que más pesan**, B la más pesada,
  y `weight = wB / (wA + wB)`. En 1D da lo mismo que hoy.
- Lua: sin binding nuevo, los dos parámetros se mueven con `SetFloat`.

## 5. Editor (`AnimatorPanel`)

En la sección de blend del nodo, con al menos una entrada:

- Casilla **2D**. Al marcarla, `blendParamY` toma el primer parámetro Float
  declarado que no sea `blendParam` (o el propio `blendParam` si no hay otro);
  al desmarcarla, `blendParamY` se vacía.
- Selector del parámetro Y, igual que el de X.
- Columna **Y** junto al umbral de cada fila (principal y entradas).
- **Lienzo** (unos 160 px de alto, `ImDrawList`): los puntos con el nombre del
  clip, las aristas de la triangulación aceptada y un punto con el valor
  actual de (X, Y). Solo lectura.
- Todo pasa por el undo del grafo (`AnimatorGraphCommand`), un paso por gesto,
  como el resto de campos del blend. `PushID` por sección.

## 6. Pruebas

En `engine/tests/animator_tests.cpp`, TDD, sabotajes de uno en uno:

- Pesos: dentro de un triángulo (baricéntricas exactas), en un vértice (1
  muestra, peso 1), sobre una arista (2 muestras), fuera del contorno (arista
  más cercana), todos alineados, 2 puntos, puntos repetidos; los pesos suman 1
  siempre.
- Rejilla 2×2 concíclica: exactamente 2 triángulos aceptados y los pesos no
  saltan al recorrer la diagonal.
- Continuidad: 5 puntos (las esquinas del cuadrado [-1, 1]² y uno en
  (0.2, 0.1)), barrido de (X, Y) por [-1.5, 1.5]² en pasos de 0.01; entre dos
  pasos vecinos ningún clip cambia de peso más de 0.05.
- 1D sin cambios: `stateBlendSamples` en un estado 1D devuelve lo mismo que la
  pareja de antes (los tests 1D existentes siguen en verde).
- Pose: un fade entre dos estados 2D da 6 muestras que suman 1; la referencia
  en CPU (`evalPoseTrs`) cubre 6 muestras.
- Root motion 2D: 3 muestras con los pesos de la pose.
- Serialización: ida y vuelta de un estado 2D; una escena sin las claves
  nuevas carga como 1D.
- Push: `static_assert` de 92 bytes en los dos backends y el cbuffer de
  `build-ninja/hlsl/bone_eval.comp.hlsl` con los 23 escalares en orden.
- Runtime Debug con `rr10.scene` en los dos backends: Vulkan con syncval
  limpio, D3D12 solo el aviso `id=1328`.

Verificación manual del usuario en los dos backends: un estado 2D con 3-4
clips responde a X/Y sin saltos, el lienzo coincide con lo que se ve, un fade
entre dos estados 2D es continuo, y el blend 1D se ve igual que antes.

## Fuera de alcance

Capas y máscaras (14b), el buffer de muestras en SSBO (llega con 14b),
blend 2D direccional de Unity, y el root motion por muestra durante un fade.
