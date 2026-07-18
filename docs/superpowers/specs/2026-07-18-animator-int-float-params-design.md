# Parámetros Int y Float en el Animator

Fecha: 2026-07-18

## Contexto

El `AnimatorComponent` (ver `2026-07-16-animator-graph-design.md`) soporta hoy dos
tipos de parámetro: `Bool` y `Trigger`. Un grafo real necesita además valores
numéricos: velocidad para pasar de idle a walk a run, contador de combo, vida
restante. Esta spec añade los tipos `Int` y `Float`, con condiciones de
comparación, siguiendo el modelo de Unity.

Estado actual del código:

- `engine/include/DonTopo/Core/AnimatorComponent.h`: `ParamType {Bool, Trigger}`,
  `ConditionType {Bool, Trigger, AnimationFinished}`, `Condition {type, paramName, expected}`.
- `engine/src/Core/AnimatorComponent.cpp`: mapas `m_bools` / `m_triggers`,
  `conditionsMet`, `reset`, `addParameter`, `removeParameter`, `hasParam`.
- `engine/src/Core/Scene.cpp`: serialización JSON con enums como string.
- `engine/src/Editor/AnimatorPanel.cpp`: lista de parámetros y popup de condiciones.
- `engine/src/Editor/PropertiesPanel.cpp:1169`: resumen de solo lectura.
- `engine/src/Scripting/ScriptBindings.cpp`: usertype `Animator` con
  `SetBool/GetBool/SetTrigger/GetState`.
- `engine/tests/animator_tests.cpp`: tests headless de la máquina de estados.

## Decisiones

| Decisión | Elección | Motivo |
|---|---|---|
| Comparadores | Int: Greater, Less, Equals, NotEquals. Float: solo Greater y Less en la UI | Modelo de Unity. `==` sobre float casi nunca dispara y es una trampa para el usuario |
| Umbral | Un solo campo `float threshold` para ambos tipos | Menos ramas en serialización y UI. Exacto para enteros hasta 2^24, y la UI de Int usa `DragInt`, así que el valor siempre entra íntegro |
| Valores en la UI | Widget de valor editable para los cuatro tipos | Permite probar transiciones a mano en Play sin escribir Lua |
| Valores por defecto | Siempre 0 / false, no autoríables | Coherente con el `reset()` actual. Evita migrar el JSON y distinguir edición de diseño frente a runtime |
| `ConditionType` fusionado con `ParamType` | No | Refactor de código ya mergeado; más riesgo que valor ahora |

## Diseño

### 1. Core — `AnimatorComponent.h` / `.cpp`

```cpp
enum class ConditionType { Bool, Trigger, AnimationFinished, Int, Float };
enum class ParamType     { Bool, Trigger, Int, Float };
enum class Compare       { Greater, Less, Equals, NotEquals };

struct Condition
{
    ConditionType type      = ConditionType::Bool;
    std::string   paramName;
    bool          expected  = true;                  // solo Bool
    Compare       compare   = Compare::Greater;      // solo Int/Float
    float         threshold = 0.0f;                  // solo Int/Float
};
```

Los valores nuevos van **al final** de los enums existentes y los campos nuevos
**al final** de `Condition`: los tests usan inicialización agregada
(`{ConditionType::Bool, "running", true}`) y la serialización va por string, así
que ni rompe la compilación ni las escenas guardadas.

Estado nuevo: `std::unordered_map<std::string, int> m_ints` y
`std::unordered_map<std::string, float> m_floats`, junto a los existentes.

API de runtime, simétrica con la actual y con la misma guarda `hasParam(n, tipo)`
que `setBool` (un `setInt` sobre un parámetro que no existe o que es de otro tipo
no hace nada):

```cpp
void  setInt(const std::string& n, int v);
int   getInt(const std::string& n) const;      // 0 si no existe
void  setFloat(const std::string& n, float v);
float getFloat(const std::string& n) const;    // 0.0f si no existe
```

Cambios en las funciones existentes:

- `addParameter`: inicializa la entrada del mapa correspondiente a 0.
- `removeParameter`: borra el nombre de los cuatro mapas.
- `reset()`: pone ints a 0 y floats a 0.0f, además de bools y triggers a false.
- `conditionsMet`: dos `case` nuevos que delegan en un helper compartido
  `evalCompare(valor, compare, umbral)`. Para `ConditionType::Int` el valor es
  `getInt(c.paramName)` comparado contra `(int)c.threshold`; para `Float`,
  `getFloat(c.paramName)` contra `c.threshold`.
- `evalCompare` implementa los cuatro comparadores. Aunque la UI solo ofrece
  Greater y Less para Float, un JSON editado a mano con `Equals` sobre un float
  se evalúa con `==` literal en vez de ignorarse: falla ruidoso, no silencioso.

### 2. Serialización — `Scene.cpp`

- `paramTypeToStr` / `paramTypeFromStr`: añaden `"int"` y `"float"`.
- `condTypeToStr` / `condTypeFromStr`: añaden `"int"` y `"float"`.
- `compareToStr` / `compareFromStr`: nuevos, con `"greater"`, `"less"`,
  `"equals"`, `"notEquals"`; el `from` desconocido cae en `Greater`.
- `animatorToJson` emite `compare` y `threshold` en la condición solo si el tipo
  es Int o Float, igual que hoy emite `expected` solo si es Bool.
- `animatorFromJson` lee ambos con `value(...)` y los defaults del struct.

Escenas antiguas no llevan los campos nuevos y cargan sin migración.

### 3. UI — `AnimatorPanel.cpp`

**Lista de Parameters** (`drawParameterList`): cada fila muestra nombre, widget de
valor y el botón `X` de borrado. El widget depende del tipo:

| Tipo | Widget |
|---|---|
| Bool | `Checkbox` enlazado a `getBool`/`setBool` |
| Trigger | Botón `Set` que llama a `setTrigger` |
| Int | `DragInt` enlazado a `getInt`/`setInt` |
| Float | `DragFloat` enlazado a `getFloat`/`setFloat` |

El ancho del `BeginChild("params", ...)` pasa de 200 a 260 para que quepan.
El texto `(bool)` / `(trigger)` / `(int)` / `(float)` sale de un helper
`paramTypeLabel(ParamType)` compartido, que sustituye al ternario
`? "trigger" : "bool"` duplicado hoy en tres sitios y que con cuatro tipos deja
de funcionar.

**Combo de creación**: las opciones pasan a `{bool, trigger, int, float}` y el
índice del combo mapea directamente al enum.

**Popup de condiciones**: `Add: <param>` mapea el tipo del parámetro al tipo de
condición (Bool→Bool, Trigger→Trigger, Int→Int, Float→Float). Las filas de una
condición Int o Float muestran, en lugar del checkbox `expected`, un `Combo` de
comparador y el widget del umbral: `DragInt` para Int (escribiendo en
`threshold` vía un `int` temporal) y `DragFloat` para Float. El combo de Float
ofrece dos entradas (Greater, Less); el de Int, las cuatro.

### 4. Lua — `ScriptBindings.cpp`

El usertype `Animator` gana `SetInt`, `GetInt`, `SetFloat` y `GetFloat`, con la
misma forma que `SetBool`/`GetBool`. Se documentan en el README junto al resto de
la API del Animator.

### 5. Resumen de solo lectura — `PropertiesPanel.cpp`

La línea 1169 sustituye el ternario por `paramTypeLabel(p.type)`.

## Tests

En `engine/tests/animator_tests.cpp`, casos nuevos (`makeGraph()` no se toca, así
que los tests existentes quedan intactos):

1. Transición por condición Int con `Greater` y con `Equals`: no dispara por
   debajo del umbral, dispara al alcanzarlo.
2. Transición por condición Float con `Less`.
3. Guarda de tipo: `setInt` sobre un parámetro declarado como bool no cambia nada
   y no crea la entrada.
4. `removeParameter` de un int borra las condiciones que lo usaban y las
   transiciones que se quedan sin condiciones.
5. Round-trip JSON de un grafo con condiciones Int y Float: `compare` y
   `threshold` sobreviven.
6. Retrocompatibilidad: un JSON de condición sin `compare` ni `threshold` carga
   con los defaults del struct.

## Fuera de alcance

- Valores por defecto autoríables y serializados.
- Blend trees y blending entre clips.
- Fusionar `ConditionType` con `ParamType`.
