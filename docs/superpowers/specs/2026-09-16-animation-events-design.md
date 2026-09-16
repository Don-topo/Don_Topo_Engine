# Eventos de animación — diseño

Fila 8 del backlog de `docs/animation-audit.md` (C8). Un estado del Animator dispara eventos con nombre en instantes de su ciclo, y los scripts Lua del mismo GameObject los reciben en `OnAnimationEvent(name)`.

## Decisión: eventos por estado, no por clip

En Unity los eventos viven en el clip, que es un asset propio y los lleva a todos sus usos. Aquí los clips salen del FBX y no se guardan en ningún sitio propio: el único lugar persistente es el Animator de cada objeto. Por eso, eventos "por clip" tampoco se compartirían entre personajes, y costarían una sección del panel y un undo propios. Por estado:
- se guardan con el grafo en el `.scene`;
- el undo del grafo los cubre sin código nuevo (`animatorGraphKey` sale del JSON);
- en un blend 1D disparan una sola vez, porque todos los clips comparten la fase del principal.

Si algún día hay assets de clip compartidos, se pueden migrar.

## Datos (`AnimatorComponent`)

```cpp
struct AnimationEvent
{
    std::string name;
    float       time = 0.0f;   // fase normalizada del estado, [0, 1]
};
// en State:
std::vector<AnimationEvent> events;
```

Nuevos miembros:
- `std::vector<std::string> m_firedEvents;`
- `const std::vector<std::string>& firedEvents() const { return m_firedEvents; }`

## Disparo (`AnimatorComponent::update`)

1. `m_firedEvents.clear()` al **principio** de `update`, antes de cualquier `return`. Así la lista contiene siempre lo del último update y nunca se acumula.
2. Tras avanzar el reloj del estado actual (`ticks0` → `m_stateTicks`) y **antes** de evaluar transiciones, solo si `evaluateTransitions` y `conDuracion`, se llama a `collectEvents(actual, ticks0, m_stateTicks)`.
3. `collectEvents`: para cada evento, con `c = time · duration` (en ticks), dispara una vez por cada entero `k >= 0` con `ticks0 <= k·duration + c < ticks1`. Sin loop solo cuenta `k = 0`. Cálculo en double:
   - `kMin = ceil((ticks0 - c) / duration)`, con mínimo 0;
   - `kMax`: el mayor `k` con `k·duration + c < ticks1`;
   - si `kMax >= kMin`, dispara `kMax - kMin + 1` veces (una sola sin loop, si `kMin == 0`), empujando `name` a `m_firedEvents` cada vez.
   - El orden de salida es el de la lista de eventos, repetido por ciclo. Con dt enormes se pone un tope de 16 ciclos por evento y update, para que un hitch no llene la lista.
4. **Estado que se apaga en un cross-fade**: no dispara. Solo el actual (el destino). Así las pisadas no salen dobles durante el fade.
5. Una transición que sale en este update ocurre **después** del disparo: los eventos del tramo final del estado viejo ya salieron. Al entrar en el nuevo estado sus ticks valen 0, así que un evento con `time = 0` dispara en su primer update con dt > 0.
6. `resetPlayback`, `play` y `crossFade` no disparan nada: los eventos salen solo de `update`.

Consecuencias del intervalo semiabierto `[ticks0, ticks1)`:
- un evento en 0 dispara al entrar y al empezar cada vuelta del loop;
- un evento en 1 en un loop coincide con el 0 de la vuelta siguiente, pero se cuenta con su propio `c = duration`, así que disparan los dos;
- sin loop, uno en 1 dispara cuando el reloj cruza el final, una vez.

`duration` es la del clip principal: la misma que usan el exit time y la fase.

## Entrega a Lua (`ScriptManager`)

En `ScriptManager::update`, después de `drainTriggerQueue()` y antes de `Update`, se llama a `deliverAnimationEvents()`. Recorre la escena (`m_scene->traverse`) y, para cada GameObject vivo con Animator, `firedEvents()` no vacío y scripts, llama a `OnAnimationEvent(name)` en cada script por cada nombre, en orden. Se sigue el patrón de `callOptionalCallback`: se sondea si el script define la función y, si falla, se marca `hasError` y se registra en el log. Hace falta una sobrecarga que pase un `std::string`.

El Animator avanza en el frame (`applySkinnedFrame`) y los scripts corren una vez por frame, así que cada tanda se lee una vez sin depender del orden entre los dos. Si en un frame avanza el Animator y no corren los scripts, esos eventos se pierden: queda documentado. El editor no tiene Pause (comprobado al planificar): hoy no hay frames en Play con Animator y sin scripts.

## Serialización (`Scene.cpp`)

- `"events": [ {"name": "...", "time": 0.5} ]` en el estado, **solo si** hay alguno.
- Lectura: `time` con `readFloat` (no finito: aviso y 0), y fuera de [0, 1] se clampa con aviso `animator.state.<n>.events: time fuera de [0,1]`. Elementos que no son objetos se ignoran.
- `name` vacío se conserva: el editor lo crea así y no es un error.

## Editor (`AnimatorPanel.cpp`)

En el nodo del estado, después del blend:
- una fila por evento: `InputText` del nombre (ancho 90), `DragFloat` de 0 a 1 con `ImGuiSliderFlags_AlwaysClamp` (ancho 50) y botón "x";
- botón "+ evento", que añade `{ "", 0.5 }`.

`PushID` con un prefijo distinto del de las filas de blend, para que los índices no choquen. El borrado se difiere fuera del bucle, igual que en el blend. El undo lo cubre el tracker del grafo.

## Documentación

- `LuaApiReference.cpp`: `OnAnimationEvent` en la lista de callbacks, con su firma y la descripción.
- README, `## Lua Scripting`: el callback y la regla de entrega (solo en Play, una vez por ciclo, el fade dispara solo el destino, y un frame sin scripts los pierde).
- README, sección del Animator: los eventos por estado.

## Fuera de alcance

- Parámetros tipados en el evento (float/int/objeto): solo nombre.
- Eventos por clip compartidos entre objetos.
- Disparo desde el estado que se apaga.
- Entrega a C++ fuera de Lua (`firedEvents()` ya es pública).

## Tests (uno por regla, cada uno con su sabotaje)

`animator_tests.cpp`, estado con `duration = 40`, `ticksPerSecond = 20` (2 s por ciclo), loop y evento "paso" en 0,5:
1. 250 updates de 0,016 s (4 s, dos ciclos): "paso" sale **exactamente 2 veces** en total, y nunca dos en el mismo update.
2. Un update que cruza el wrap (de 1,9 s a 2,1 s de reloj, con el evento en 0,02): dispara una vez.
3. Un update de 4,2 s desde 0: dispara **2** veces (0,5 y 1,5 ciclos).
4. Sin loop: tras 10 s, un solo disparo en total.
5. Evento en 0: dispara en el primer update con dt > 0 tras entrar, y no en un update de dt 0.
6. `evaluateTransitions = false`: ningún disparo.
7. Cross-fade: el estado que se apaga tiene un evento que cae dentro del fade y el destino no tiene ninguno; no sale nada.
8. `firedEvents()` se vacía en el siguiente update aunque no haya disparos.
9. Escena: ida y vuelta de `events`; `time = 3` carga como 1 con aviso.

`scripting_tests.cpp`:

10. Un script con `function OnAnimationEvent(self, name)` que acumula nombres recibe "paso" tras un `update` del Animator que lo dispara, seguido de un `ScriptManager::update`.
