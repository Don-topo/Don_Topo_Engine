# Sub-máquinas de estados (fila 15 / C10) — diseño

**Fecha:** 2026-09-20
**Backlog:** `docs/animation-audit.md`, fila 15, hallazgo C10 ("Sub-máquinas de estados: anidamiento y entradas/salidas; toca el canvas y la serialización").

## Qué falta hoy

Cada capa tiene un vector plano de estados (`Layer::states`). Un personaje con
doce estados de ataque, seis de locomoción y cuatro de daño es un grafo que no
se puede leer: todo vive en el mismo lienzo y las transiciones se cruzan. No hay
forma de decir "esto es un bloque" ni de reutilizar una entrada común.

## Qué se construye

Un estado puede **contener** a otros. El vector sigue siendo plano y el
anidamiento es una relación de padre: es lo que permite que la máquina de
estados —`update`, la pose, el cross-fade, el root motion, las curvas y la IK—
**no cambie nada**. El estado activo es siempre una hoja.

Lo que aporta: cajas en el canvas con navegación, una entrada propia por caja, y
transiciones que se escriben una vez contra la caja en vez de una por cada hoja.

**Lo que no aporta, dicho por delante:** no hay estado compuesto activo ni
transiciones jerárquicas con prioridad del padre al estilo de Unity. Eso obliga a
una pila de estados activos y a reescribir el runtime entero (y con él las cinco
filas anteriores); se descartó a propósito.

## Modelo de datos

`AnimatorComponent::State`:

```cpp
    // Sub-máquinas (C10). El vector de la capa sigue plano: esto es una
    // relación de contención, no un grafo anidado.
    int  parent       = -1;      // estado que lo contiene; -1 = raíz de la capa
    bool isSubMachine = false;   // es una caja: NO se reproduce, no puede ser el estado actual
    int  subEntry     = -1;      // hijo por el que se entra (solo si isSubMachine)
```

Reglas de integridad:

- `parent` apunta siempre a un estado con `isSubMachine == true`, o vale −1.
- Un estado no puede ser descendiente de sí mismo. Se impide al elegir el padre
  (la UI solo ofrece cajas que no sean descendientes suyas) y se comprueba al
  cargar: un ciclo deja a los implicados en la raíz con un aviso.
- `subEntry` apunta a un hijo directo. Si no lo es (fichero manipulado, hijo
  borrado), vale −1: la caja está vacía y no se puede entrar en ella.
- Una caja con `clipName`, blend o clip de propiedades **ignora** todo eso: no se
  reproduce. La UI no lo ofrece.

## Runtime

Todo lo nuevo vive en la resolución de transiciones; `updateLayer` no cambia de
forma.

**Entrar.** Una transición cuyo `toState` es una caja se resuelve bajando por
`subEntry` hasta una hoja. Si en el camino hay una caja vacía —o se agota el
límite de profundidad—, la transición **no dispara**: es preferible quedarse
donde se está a entrar a medias en un estado que no existe. Mismo criterio para
el `entryState` de la capa y para `Play`/`CrossFade` de Lua con el nombre de una
caja.

**Salir.** Una transición cuyo `fromState` es una caja vale cuando el estado
actual es descendiente suyo a cualquier profundidad. No hay nodos Exit que
mantener: la salida es "desde cualquier hoja de dentro".

**Orden.** Any State sigue yendo **primero**, como hoy (`AnimatorComponent.cpp`,
el bucle de `kAnyState`: es la prioridad de Unity y no se toca). Después, las
transiciones cuyo `fromState` es el estado actual, luego las de su padre, luego
las del abuelo, y así hasta la raíz. Dentro de cada nivel manda el orden del
vector. Hoy ese orden es el del vector a secas, que es arbitrario; con cajas
haría que una salida general pudiera ganarle a una salida concreta según dónde se
creó, que es justo lo que nadie espera.

**Garantía.** Un estado con `isSubMachine` nunca es `currentState`. La guarda
vive en `enterState` (una caja se resuelve a hoja antes de entrar; si no
resuelve, no se mueve nada), que es por donde pasan todos los caminos.

## Borrado y reindexado

`removeState` ya recoloca transiciones, `entryState` y el playhead cuando los
índices se desplazan. Se le añade:

- Borrar una caja **borra a sus descendientes**, a cualquier profundidad. La
  alternativa —soltarlos en la raíz— deja huérfanos sin que el usuario lo pida.
- `parent` y `subEntry` se reindexan como el resto; el que apuntaba al borrado
  queda en −1.
- El undo lo cubre entero sin tocarlo: es un snapshot del grafo.

## Editor

- El canvas dibuja **solo** los estados cuyo `parent` es el nivel actual, con un
  breadcrumb (`Base / Ataques / Combo`) para subir. Doble clic en una caja entra.
- Una transición con un extremo dentro de otra caja se dibuja **contra la caja
  ancestro visible** en este nivel, no se oculta. Si los dos extremos resuelven
  al mismo nodo visible, no se dibuja.
- Crear una caja: botón **Add Sub-State Machine**. Meter un estado dentro: combo
  **Padre** en las propiedades del estado, con las cajas que no son descendientes
  suyas. No hay arrastrar-y-soltar: es donde se va el tiempo y el combo resuelve
  lo mismo.
- Una caja se distingue en el lienzo (color y una marca en el título) y muestra
  su entrada actual; el combo de entrada lista sus hijos directos.
- Crear un link contra una caja crea la transición contra ella, que es lo que el
  runtime resuelve.

## Serialización

En `animatorToJson`, por estado, escritos **solo** cuando no son el default:
`"parent"`, `"subMachine": true`, `"subEntry"`. Un `.scene` de hoy no cambia ni
un byte.

Al leer: índices fuera de rango → −1 con aviso; `parent` que no apunta a una
caja → −1 con aviso; ciclo de contención → los implicados a la raíz con aviso.

## Lua

Sin API nueva. `Play` y `CrossFade` con el nombre de una caja entran por su
entrada resuelta; con una caja vacía devuelven false, como un nombre que no
existe.

## Tests (`engine/tests/animator_tests.cpp`)

1. Una transición hacia una caja entra en la hoja de su `subEntry`, y en la hoja
   correcta con dos niveles de anidamiento.
2. Una caja vacía (o con la cadena rota) no dispara la transición: el estado
   actual no se mueve.
3. Una transición desde una caja dispara estando en cualquiera de sus hojas, a
   dos niveles de profundidad, y **no** dispara desde una hoja de fuera.
4. Orden: con una salida en la hoja y otra en el padre que cumplen a la vez, gana
   la de la hoja.
5. `currentState` nunca es una caja: `enterState` sobre una caja entra en su hoja;
   sobre una caja vacía no mueve nada.
6. Borrar una caja borra a sus descendientes y deja las transiciones de los demás
   apuntando a donde apuntaban.
7. Reindexado: borrar un estado anterior a una caja deja `parent` y `subEntry`
   apuntando a los mismos estados.
8. Carga: `parent` fuera de rango, `parent` que no es caja y ciclo de contención
   → avisos y estado utilizable.
9. Round-trip JSON: los tres campos sobreviven, y un Animator sin cajas no
   escribe ninguna de las claves nuevas.
10. `Play("caja")` desde el core entra en la hoja; con la caja vacía devuelve
    false.

Cada guarda nueva se sabotea de una en una (parchear → compilar → ejecutar →
revertir), con la herramienta PowerShell y sin bucles.

## Verificación manual

En Vulkan **y** D3D12: crear una caja, meterle dos estados, marcar su entrada,
transicionar desde fuera hacia la caja y desde la caja hacia fuera, navegar con
el breadcrumb, borrar la caja y comprobar que el grafo queda coherente, guardar y
recargar la escena.

## Documentación

- `README.md`, sección del Animator: qué es una caja, cómo se entra y se sale,
  el orden de evaluación y lo que no da.
- `docs/animation-audit.md`: C10 a ✅ EXISTE y la fila 15 cerrada.

## Fuera de alcance

- Estado compuesto activo y prioridad jerárquica de transiciones (opción B).
- Arrastrar nodos dentro de una caja en el lienzo.
- Copiar o reutilizar una sub-máquina entre capas o entre Animators.
