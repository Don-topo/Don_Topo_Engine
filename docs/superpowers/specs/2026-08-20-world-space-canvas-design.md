# Canvas en world-space

Fecha: 2026-08-20
Estado: aprobado, pendiente de plan de implementación

## Qué se construye

Que un `CanvasComponent` pueda dibujarse **en el mundo 3D** en vez de sobre la
pantalla, y que una escena pueda tener **varios canvas a la vez**.

Dos casos de uso, dados por el usuario:

- **Barras sobre enemigos**: muchos canvas, pequeños, en movimiento, mirando a
  cámara.
- **Pantallas diegéticas fijas**: un monitor en una pared, un cartel. Pocos y
  quietos, pero la geometría de la escena **tiene que taparlos**.

Fuera de alcance en esta tanda: **interacción**. Un canvas de mundo se dibuja y
sus valores los mueve un script; no hay hover, ni clic, ni foco. Se decidió
explícitamente, no se olvidó.

## Punto de partida

Tres hechos del código actual que mandan sobre el diseño:

1. El pipeline de UI **ya recibe una matriz 4×4 completa** por push constant
   (`UiSpriteBatch.cpp`, `record`). Dibujar en el mundo es, en lo esencial,
   pasar `proj · view · model` en vez de una ortográfica.
2. El pase de UI tiene **un solo attachment, de color, de 1 muestra, sin
   profundidad**, y corre **después del post-proceso**, sobre la imagen ya
   resuelta (`Renderer.cpp`, `m_uiRenderPass`). D3D12 replica la estructura
   exacta (`recordUiCanvas`, sobre el back buffer, tras el AA).
3. Hay **un solo canvas** por Renderer (`m_uiCanvas`, virtual `uiCanvas()` en
   `EditorRenderer`), y `Scene::findCanvas()` devuelve el primero en pre-orden.

El punto 2 descarta añadir un attachment de profundidad al pase de UI: ahí el
color es de 1 muestra y el depth de la escena es multimuestra con MSAA, así que
no casan; con SSAA tampoco casa la resolución. **Un canvas de mundo tiene que
grabarse dentro del pase de escena**, que es el que sí tiene profundidad. De
propina, así recibe niebla, bloom y AA como cualquier objeto del mundo — que es
lo que se quiere de una pantalla diegética.

## 1. Modelo de datos

`CanvasComponent` conserva sus 10 campos y gana cuatro:

```cpp
enum class UiCanvasRenderMode { ScreenSpace, World };
enum class UiBillboard        { None, YawOnly, Full };

class CanvasComponent {
    // ... los 10 de hoy, intactos ...

    UiCanvasRenderMode renderMode = UiCanvasRenderMode::ScreenSpace;

    // --- solo World ---
    float       worldScale = 0.001f;   // unidades de mundo por PIXEL de canvas
    UiBillboard billboard  = UiBillboard::None;
    bool        depthTest  = true;     // false = siempre encima, atraviesa paredes
};
```

En modo World el área útil es **exactamente** `referenceResolution`: no hay
pantalla a la que ajustarse, así que `scaleMode`, `screenMatch`,
`matchWidthOrHeight`, `screenDpi`, `fallbackDpi`, `referenceDpi`, `safeArea` y
`aspectRatio` **no se leen**. No se esconden — la regla del proyecto es
documentar el matiz, no capar el campo —, pero el inspector lo avisa y el
README también.

La posición sale del **`worldTransform` del GameObject**, que hoy el canvas
ignora a propósito. La matriz de modelo centra el canvas en el objeto y voltea
la Y (el canvas crece hacia abajo, el mundo hacia arriba):

```
model = worldTransform · T(-w/2·s, +h/2·s, 0) · S(s, -s, s)      s = worldScale
```

`billboard` sustituye la rotación de `worldTransform` por una que mire a la
cámara: `YawOnly` para barras de vida (no se tumban al mirar desde arriba),
`Full` para iconos.

`depthTest = false` es lo que deja ver una barra de vida a través de una pared.
Es estado de pipeline, así que cuesta una variante más (ver el recuento en la
sección 3).

## 2. La escena deja de tener un solo canvas

```cpp
struct UiCanvasBinding {
    uint64_t               ownerId;         // GameObject del Canvas
    const CanvasComponent* canvas;
    glm::mat4              worldTransform;  // del GameObject; solo lo lee el modo World
    UiWidgetLists          widgets;         // SOLO los que cuelgan de ESTE canvas
};

// Scene.h — sustituye a collectUiWidgets
void collectCanvases(std::vector<UiCanvasBinding>& out) const;
```

Es **un solo recorrido**: el walker que ya arrastra "cuál es el ancestro con
UI" arrastra además "cuál es el canvas ancestro". Gana el **más cercano hacia
arriba**, así que un canvas dentro de otro funciona sin reglas nuevas.

Dos cambios de comportamiento, deliberados:

- **Un widget sin ningún Canvas por encima deja de dibujarse.** Hoy acaba en el
  canvas único aunque no cuelgue de él. El editor ya lo impide
  (`uiComponentsAvailable` exige un Canvas ancestro), así que solo afecta a
  escenas hechas a mano — pero es un cambio real y lleva test.
- `collectUiWidgets` **desaparece**; sus 6 puntos de llamada (todos en tests)
  pasan a `collectCanvases`.

`findCanvas()` se queda: los tres bucles lo usan como puerta y el gizmo del área
útil también.

**Mejora colateral, y solo esta:** `UiWidgetLists`, `UiWidgetSyncCache` y
`syncUiWidgets` viven en `TextComponent.h`, que ya no tiene nada que ver con
ellos — y ahora sumarían `UiCanvasBinding`. Se mueven a un `UiWidgetSync.h`
nuevo, y `TextComponent.h` lo incluye, así **ningún punto de llamada cambia**.
Es mover texto, no reescribirlo.

## 3. Render

El Renderer pasa de un canvas a un vector de *slots*, emparejados por `ownerId`
entre frames **para que la caché de sync sobreviva**. Si se reasignaran por
índice, mover un enemigo en la jerarquía reconstruiría su barra de vida entera.

Un canvas cuyo GameObject desaparece pierde su slot en el frame siguiente, y con
él su árbol y su caché; uno nuevo estrena los suyos.

```cpp
struct UiCanvasSlot {
    uint64_t           ownerId = 0;
    UiCanvas           canvas;      // el árbol vivo
    UiWidgetSyncCache  cache;
    UiDrawData         drawData;
    UiCanvasRenderMode mode = UiCanvasRenderMode::ScreenSpace;
    glm::mat4          model{1.0f};
    bool               depthTest = true;
};
```

### Cambios de API pública del core

`EditorRenderer` gana **dos** virtuales:

```cpp
// Se traga el collect + syncUiWidgets que hoy repiten los TRES bucles
// (runtime y sandbox x2). Los bucles quedan en dos líneas.
virtual void syncUiCanvases(const std::vector<UiCanvasBinding>&) = 0;

// Busca un nodo por nombre en TODOS los slots. Sin esto, un widget dentro de un
// canvas de mundo se queda sin gizmo en el editor y nada lo dice.
virtual const UiElement* findUiNode(const std::string& name) const = 0;
```

`uiCanvas()` **no cambia de firma**: sigue devolviendo el canvas de **pantalla**
—el primero en modo ScreenSpace— con un canvas vacío persistente de repliegue,
así que gizmos, picking y `updateInput` siguen igual.

### Dónde se graba cada uno

`UiSpriteBatch::record` deja de calcular la ortográfica dentro y **recibe la
matriz**. Con eso:

- **Pantalla** → el mismo pase de UI de hoy, N llamadas a `record`, matriz
  ortográfica. Sin profundidad, como siempre.
- **Mundo** → grabado **dentro del pase de escena**, al final, con `depthTest`
  según el campo y **`depthWrite` siempre OFF**: la UI va con alpha, y escribir
  profundidad haría que los quads de un mismo canvas se recortaran entre sí.
  Matriz `proj · view · model`.

Eso obliga a variantes nuevas de pipeline en `UiSpriteBatch`, compiladas contra
el renderpass de la escena (otro sample count, con profundidad). El recuento
exacto, que conviene tener claro antes de empezar:

| Variante | Renderpass | depthTest | depthWrite |
| --- | --- | --- | --- |
| Pantalla (la de hoy) | pase de UI | off | off |
| Mundo, ocluido | pase de escena | **on** | off |
| Mundo, siempre encima | pase de escena | off | off |

**Tres en total, y por backend**, o sea seis objetos de pipeline. Es el trozo
más caro del trabajo.

Las dos de mundo dependen del sample count y del formato de profundidad del pase
de escena, así que **se recrean con él**: si el Renderer rehace ese pase al
cambiar de AA, tiene que rehacer también estas dos.

### Dos detalles que se olvidan y luego se ven

- **Orden entre canvas de mundo**: con alpha hay que pintar de lejos a cerca.
  Se ordenan por distancia a cámara antes de grabar.
- La matriz (incluido el billboard) va en una **función libre** en las cabeceras
  de UI, no dentro del Renderer, para poder probarla sin GPU:

```cpp
glm::mat4 uiWorldCanvasMatrix(const CanvasComponent&, glm::vec2 canvasSize,
                              const glm::mat4& worldTransform, const glm::mat4& view);
```

### El target de la escena es HDR LINEAL, no sRGB

Descubierto al escribir el plan, no estaba en el diseño original. El pase de UI
de hoy escribe sobre un `B8G8R8A8_SRGB`, donde el hardware hace la conversión al
escribir. El pase de escena (`m_offscreenRenderPass`) es **HDR lineal**
(`kHdrFormat`), así que el MISMO color escrito ahí sale lavado.

Las variantes de mundo del shader tienen que **convertir sRGB→lineal al
escribir**. Se resuelve con una constante de especialización (o un flag en el
push constant que ya existe), sin duplicar el shader.

Sin esto la UI de mundo se ve, pero con los colores mal — y es justo el tipo de
fallo que ninguna capa de validación reporta.

### SSAA

El canvas de pantalla se construye en píxeles de SALIDA, como hoy. Uno de mundo
se construye a su `referenceResolution` fija y se dibuja a resolución de RENDER
dentro del pase de escena — que con SSAA es mayor. Es lo correcto: el canvas de
mundo se supersamplea como cualquier otra cosa del mundo.

## 4. Editor y Lua

**Properties.** La sección Canvas gana el combo `Render Mode` y, debajo, los
tres campos de mundo. Los que World no lee siguen editables, con un aviso en la
sección diciendo cuáles se ignoran en ese modo. Serialización aditiva de los 4
campos nuevos, con su Command de undo.

**Gizmos de widget.** Los nueve (`drawButtonGizmo` y hermanos) buscan el nodo en
`ctx.renderer->uiCanvas()`, que ahora es solo el de pantalla. Pasan a
`ctx.renderer->findUiNode(nombre)`, así siguen funcionando sin tocarlos uno a
uno.

**Gizmo del propio Canvas.** Hoy dibuja el rect del área útil en 2D sobre la
imagen. En modo World eso no significa nada: proyecta las **cuatro esquinas**
por `proj · view · model` y dibuja el cuadrilátero, que además enseña la
perspectiva (o sea, si el cartel está torcido).

**Selección por clic: no.** Los canvas de mundo se seleccionan desde el
Hierarchy, como ya pasa con el contenedor de Layout. Clicarlos en el viewport
pide el rayo→plano que queda fuera de alcance. Documentado, no escondido.

**Lua.** `Canvas` gana `renderMode`, `worldScale`, `billboard` y `depthTest`,
más las tablas `UiCanvasRenderMode` y `UiBillboard`. Entrada en
`LuaApiReference` y en `Scripts/README.md`.

## 5. Pruebas

**Sin GPU** (`camera_tests` + `ui_batch_tests`):

1. **Agrupación.** Dos canvas hermanos con widgets debajo de cada uno → cada
   binding recibe solo los suyos. Canvas anidado → gana el más cercano. Widget
   huérfano → no aparece en ninguno.
2. **La matriz.** `uiWorldCanvasMatrix` con valores conocidos: un canvas de
   1920×1080 a `worldScale` 0,001 centrado en el objeto mide 1,92 × 1,08
   unidades, la esquina superior izquierda del canvas cae **arriba** en el mundo
   (la Y volteada) y el centro coincide con la posición del GameObject.
   Billboard `None` / `YawOnly` / `Full` contra una vista conocida: `YawOnly`
   conserva la vertical del mundo, `Full` no.
3. **Los slots sobreviven.** Dos frames sin tocar nada → `rebuiltNodes() == 0`
   en el segundo. Y reordenar los canvas en la jerarquía **no** debe resetear la
   caché del que no se movió: es justo lo que rompería un emparejamiento por
   índice.
4. **Orden lejos→cerca** de los canvas de mundo.
5. Round-trip de los 4 campos (valores no neutros y distintos entre sí), Command
   de add/undo, y bindings de Lua.

**Lo que NO se puede probar sin GPU:** que la profundidad ocluya de verdad y que
la segunda variante de pipeline compile en los dos backends. Eso es verificación
en GUI — un canvas detrás de una pared. Existe el camino de leer el back buffer
y comparar píxeles, que ya se usó para las sondas; queda como plan B.

**Sabotajes que se correrán antes de dar nada por bueno:**

- devolver la ortográfica en `uiWorldCanvasMatrix`;
- emparejar slots por índice en vez de por `ownerId`;
- meter todos los widgets en el primer canvas;
- quitar el orden por distancia.

## Riesgos

- **El trozo de pipeline es el que más puede sorprender.** Una segunda variante
  compilada contra el renderpass de la escena depende del sample count y del
  formato de profundidad, y esos cambian con los ajustes de calidad. Si el
  Renderer recrea el pase de escena al cambiar de AA, la variante hay que
  recrearla con él.
- **`collectUiWidgets` desaparece** y con ella cambia el significado de "un
  widget sin canvas". Es intencionado y va con test, pero puede sorprender a una
  escena vieja hecha a mano.
- **Dos backends.** Todo lo del render se paga dos veces. Vulkan es el que se
  prueba a diario; el de D3D12 sale del `project.json` del último proyecto
  abierto, así que hay que abrirlo a propósito para verificarlo.
