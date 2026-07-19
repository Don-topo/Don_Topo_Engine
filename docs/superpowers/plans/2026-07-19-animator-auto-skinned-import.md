# Import automático de mallas rigged — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Que un FBX con huesos entre como `SkinnedMesh` aunque no traiga animaciones, para poder crearle un Animator y añadirle clips desde otros ficheros.

**Architecture:** Un probe barato de Assimp (`ModelLoader::hasBones`) decide entre `load()` y `loadSkinned()` desde un único punto (`ModelLoader::loadAuto`). Los dos llamantes — el import del editor y la carga de escena — pasan a usarlo, y la carga de escena deja de leer el flag `"skinned"` del JSON. Ni los loaders existentes ni el Animator ni la ruta de GPU se tocan.

**Tech Stack:** C++20, Assimp, Vulkan, ImGui, nlohmann/json. Build Ninja + MSVC.

**Spec:** `docs/superpowers/specs/2026-07-19-animator-auto-skinned-import-design.md`

## Global Constraints

- Comentarios y mensajes de log/warning **en castellano**, con acentos correctos. Es la convención de todo el repo.
- Los comentarios explican **por qué**, no qué. Sigue el estilo denso de `ModelLoader.cpp` y `Scene.cpp`.
- Tests: plain `main()` + macro `CHECK`, sin framework. Se registran llamándolos desde `main()` en `engine/tests/animator_tests.cpp`.
- Los tests se ejecutan **desde la raíz del repo**: las rutas de assets son relativas (`assets/modelAnimation.fbx`).
- Build: `.\build.bat` desde PowerShell (envuelve `vcvarsall` + `cmake --build build-ninja`). **Nunca** `cmake` crudo desde Bash.
- Si un cambio en un header parece no surtir efecto en runtime, sospecha de deps stale de Ninja antes que del código nuevo.
- `hasBones` **no lanza**. Un fichero ilegible devuelve `false` y deja que `load()`/`loadSkinned()` den el error de verdad, con su mensaje.

## File Structure

| Fichero | Responsabilidad | Acción |
|---|---|---|
| `engine/include/DonTopo/Renderer/ModelLoader.h` | Declarar `hasBones` y `loadAuto` | Modificar |
| `engine/src/Renderer/ModelLoader.cpp` | Implementar el probe y el despacho | Modificar |
| `engine/src/Editor/PropertiesPanel.cpp` | Import del editor: usar `loadAuto`, registrar en la ruta correcta del renderer | Modificar |
| `engine/src/Core/Scene.cpp` | Carga: decidir por fichero en vez de por flag JSON | Modificar |
| `engine/tests/animator_tests.cpp` | Tests de `hasBones`, `loadAuto` y del round-trip de escena | Modificar |

No se crean ficheros nuevos: cada cambio es pequeño y pertenece a una unidad que ya existe.

---

### Task 1: `ModelLoader::hasBones` y `ModelLoader::loadAuto`

**Files:**
- Modify: `engine/include/DonTopo/Renderer/ModelLoader.h:22-30`
- Modify: `engine/src/Renderer/ModelLoader.cpp` (añadir al final del `namespace DonTopo`, tras `loadAnimationClips`)
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `ModelLoader::load(path) -> Mesh`, `ModelLoader::loadSkinned(path) -> SkinnedMesh` (ya existen).
- Produces:
  - `static bool ModelLoader::hasBones(const std::string& path)` — no lanza.
  - `static std::shared_ptr<Mesh> ModelLoader::loadAuto(const std::string& path)` — propaga las excepciones de `load`/`loadSkinned`.

**Suposición sin verificar:** el caso negativo de los tests descansa en que `assets/model.fbx` no declara huesos. Nadie lo ha comprobado — no hay forma de leerlo sin ejecutar Assimp. Lo verifica `test_has_bones_rejects_static_fbx` en el Step 4; si ese test falla, **para y repórtalo**: hará falta meter un FBX estático nuevo al repo como fixture, y trackear un asset binario es decisión del usuario, no tuya.

- [ ] **Step 1: Escribir los tests que fallan**

En `engine/tests/animator_tests.cpp`, justo después de `test_loader_registers_builtin_source()` (sobre la línea 70):

```cpp
// El gate del Animator pregunta por isSkinned(), y hasta ahora el import del
// editor descartaba huesos y pesos llamando siempre a load(). hasBones es quien
// decide: mira el fichero, no al llamante.
static void test_has_bones_detects_rigged_fbx()
{
    CHECK(ModelLoader::hasBones("assets/modelAnimation.fbx") == true);
}

// Un FBX sin rig tiene que seguir entrando como Mesh plano: cargarlo skinned
// pagaría vértices de 112 B y una SSBO de huesos vacía para nada.
static void test_has_bones_rejects_static_fbx()
{
    CHECK(ModelLoader::hasBones("assets/model.fbx") == false);
}

// No lanza: el fichero ilegible lo reporta el loader real, con su mensaje. Si
// hasBones lanzara, el import moriría antes de llegar a ese mensaje.
static void test_has_bones_survives_missing_file()
{
    bool threw = false;
    bool result = true;
    try { result = ModelLoader::hasBones("assets/no_existe_este_fichero.fbx"); }
    catch (...) { threw = true; }
    CHECK(!threw);
    CHECK(result == false);
}

// loadAuto devuelve el tipo dinámico correcto: es lo único que mira
// GameObject::isSkinned(), y por tanto lo único que habilita el Animator.
static void test_load_auto_returns_skinned_for_rigged()
{
    std::shared_ptr<Mesh> m = ModelLoader::loadAuto("assets/modelAnimation.fbx");
    CHECK(m != nullptr);
    if (!m) return;
    SkinnedMesh* sm = dynamic_cast<SkinnedMesh*>(m.get());
    CHECK(sm != nullptr);
    if (!sm) return;
    CHECK(!sm->skeleton.names.empty());
    // La fuente builtin la crea loadSkinned; loadAuto no debe alterarla.
    CHECK(sm->animationSources.size() == 1u);
}

static void test_load_auto_returns_static_for_unrigged()
{
    std::shared_ptr<Mesh> m = ModelLoader::loadAuto("assets/model.fbx");
    CHECK(m != nullptr);
    if (!m) return;
    CHECK(dynamic_cast<SkinnedMesh*>(m.get()) == nullptr);
    CHECK(!m->vertices.empty());
}
```

Y en `main()`, junto a las otras llamadas de loader (antes de `test_bind_clips_resolves_by_name();`):

```cpp
    test_has_bones_detects_rigged_fbx();
    test_has_bones_rejects_static_fbx();
    test_has_bones_survives_missing_file();
    test_load_auto_returns_skinned_for_rigged();
    test_load_auto_returns_static_for_unrigged();
```

- [ ] **Step 2: Ejecutar y verificar que falla la compilación**

```powershell
.\build.bat
```

Esperado: error de compilación, `'hasBones': is not a member of 'DonTopo::ModelLoader'`.

- [ ] **Step 3: Implementar**

En `engine/include/DonTopo/Renderer/ModelLoader.h`, añade `#include <memory>` a los includes y, dentro de `class ModelLoader`, tras la declaración de `loadAnimationClips`:

```cpp
            // true si algún aiMesh del fichero declara huesos. Es lo que separa
            // un personaje de un prop: sin huesos no hay pesos por vértice, y
            // sin pesos no hay nada que una animación pueda deformar.
            //
            // No lanza. Un fichero ilegible devuelve false y deja que load()
            // dé el error de verdad, con su mensaje.
            static bool hasBones(const std::string& path);

            // Decide estático vs skinned mirando el fichero, no al llamante. Un
            // FBX con huesos entra siempre como SkinnedMesh, aunque no traiga ni
            // una animación: es lo que habilita el Animator, y los clips pueden
            // venir después de otros ficheros (ver addAnimationSource).
            //
            // Propaga las excepciones de load()/loadSkinned(): los llamantes ya
            // tienen su try/catch y su mensaje de error para el usuario.
            static std::shared_ptr<Mesh> loadAuto(const std::string& path);
```

En `engine/src/Renderer/ModelLoader.cpp`, al final del `namespace DonTopo` (tras el cierre de `loadAnimationClips`):

```cpp
    bool ModelLoader::hasBones(const std::string& path)
    {
        Assimp::Importer importer;
        // Flags a cero, igual que loadAnimationClips: aquí no se construye
        // geometría, así que triangulate, normales y tangentes serían trabajo
        // tirado. mNumBones se lee igual.
        const aiScene* scene = importer.ReadFile(path, 0);
        if (!scene || !scene->mRootNode) return false;

        for (uint32_t i = 0; i < scene->mNumMeshes; i++)
            if (scene->mMeshes[i]->mNumBones > 0) return true;

        return false;
    }

    std::shared_ptr<Mesh> ModelLoader::loadAuto(const std::string& path)
    {
        if (hasBones(path))
            return std::make_shared<SkinnedMesh>(loadSkinned(path));  // convierte solo a shared_ptr<Mesh>
        return std::make_shared<Mesh>(load(path));
    }
```

Si `<memory>` no está ya incluido en `ModelLoader.cpp`, añádelo.

- [ ] **Step 4: Ejecutar los tests**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
```

Esperado: `dt_animator_tests: OK`, sin ninguna línea `FAIL:`.

Si falla `test_has_bones_rejects_static_fbx`, es la suposición sobre el fixture: `assets/model.fbx` está rigged y hace falta otro asset estático. Para y repórtalo.

- [ ] **Step 5: Commit**

```bash
git add engine/include/DonTopo/Renderer/ModelLoader.h engine/src/Renderer/ModelLoader.cpp engine/tests/animator_tests.cpp
git commit -m "feat(loader): add hasBones probe and loadAuto dispatch"
```

---

### Task 2: Import del editor por `loadAuto`

**Files:**
- Modify: `engine/src/Editor/PropertiesPanel.cpp:96-121`

**Interfaces:**
- Consumes: `ModelLoader::loadAuto` (Task 1), `GameObject::isSkinned()`, `GameObject::getSkinnedMesh()`, `Renderer::addSkinnedMesh(const SkinnedMesh&) -> int`, `Renderer::addStaticMesh(const Mesh&) -> int`.
- Produces: nada que consuman tareas posteriores.

**Nota sobre tests:** esta tarea no lleva test automático. `loadMeshForSelected` necesita un `Renderer` con dispositivo Vulkan vivo y un `EditorContext` de ImGui; los tests del repo son headless. La lógica que sí se puede probar aislada (`loadAuto`) ya quedó cubierta en Task 1. Aquí la verificación es manual, en el Step 3.

- [ ] **Step 1: Sustituir el cuerpo del `try`**

En `engine/src/Editor/PropertiesPanel.cpp`, dentro de `loadMeshForSelected`, reemplaza:

```cpp
    try
    {
        auto mesh = std::make_shared<Mesh>(ModelLoader::load(path));
        ctx.selected->staticRenderIndex = ctx.renderer->addStaticMesh(*mesh);
        ctx.selected->setMesh(std::move(mesh));
        m_meshLoadError.clear();
        ctx.pushLog("Componente Mesh añadido a '" + ctx.selected->name + "'");
    }
```

por:

```cpp
    try
    {
        // loadAuto decide skinned vs estático mirando el FBX. Un fichero rigged
        // sin animaciones tiene que entrar skinned igual: es lo que habilita el
        // botón Animator, y los clips vendrán luego de otros ficheros.
        auto mesh = ModelLoader::loadAuto(path);
        // setMesh antes del registro: así isSkinned() resuelve el tipo por
        // nosotros y no hay que repetir el dynamic_cast aquí.
        ctx.selected->setMesh(std::move(mesh));

        if (ctx.selected->isSkinned())
            ctx.selected->skinnedRenderIndex = ctx.renderer->addSkinnedMesh(*ctx.selected->getSkinnedMesh());
        else
            ctx.selected->staticRenderIndex  = ctx.renderer->addStaticMesh(*ctx.selected->getMesh());

        m_meshLoadError.clear();
        // Distinguir los dos casos en el log: sin esto, ante un FBX que el
        // usuario creía rigged y no lo está, el botón Animator se queda gris
        // sin ninguna pista de por qué.
        ctx.pushLog(std::string(ctx.selected->isSkinned() ? "Componente Skinned Mesh" : "Componente Mesh")
                    + " añadido a '" + ctx.selected->name + "'");
    }
```

El `catch (const std::exception& e)` de debajo se queda igual: `loadAuto` propaga las excepciones de los loaders tal cual.

- [ ] **Step 2: Compilar**

```powershell
.\build.bat
```

Esperado: build limpio, sin errores ni warnings nuevos.

- [ ] **Step 3: Verificación manual en el editor**

No hay test automático que cubra esto. Arranca el editor y comprueba:

1. Crear un GameObject → *Add Component → Mesh* → elegir `assets/animatedCharacter/Maw J Laygo.fbx`.
2. El Log Console dice **"Componente Skinned Mesh añadido a ..."**.
3. La malla se ve en el viewport (no invisible, no deformada).
4. *Add Component → Animator* está **habilitado** (antes estaba gris).
5. Repetir con `assets/model.fbx`: el log dice **"Componente Mesh añadido"** y *Animator* sigue gris con su tooltip.

Si el paso 3 falla (la malla no se ve), sospecha primero de deps stale de Ninja: borra los `.obj` afectados y reconstruye antes de tocar el código.

- [ ] **Step 4: Commit**

```bash
git add engine/src/Editor/PropertiesPanel.cpp
git commit -m "feat(editor): import rigged FBX as SkinnedMesh"
```

---

### Task 3: La carga de escena decide por fichero, no por flag

**Files:**
- Modify: `engine/src/Core/Scene.cpp:428-498`
- Test: `engine/tests/animator_tests.cpp`

**Interfaces:**
- Consumes: `ModelLoader::hasBones` (Task 1).
- Produces: nada que consuman tareas posteriores.

- [ ] **Step 1: Escribir los tests que fallan**

En `engine/tests/animator_tests.cpp`, tras `test_scene_without_animation_sources_loads`:

```cpp
// Las escenas guardadas antes de la auto-detección tienen "skinned": false para
// TODOS sus meshes — el editor nunca creaba skinned. Si la carga siguiera
// leyendo ese flag, esos proyectos nunca podrían tener Animator sin reimportar
// la malla a mano. Manda el fichero, no el flag.
static void test_scene_load_ignores_stale_skinned_false(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    const uint64_t id = go->id;
    auto mesh = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    go->setMesh(mesh);

    nlohmann::json j = scene.toJson();
    // Simula el fichero viejo: flag a false sobre un FBX que sí tiene huesos.
    j["root"]["children"][0]["mesh"]["skinned"] = false;

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;

    SkinnedMesh* lm = found->getSkinnedMesh();
    CHECK(lm != nullptr);
    if (!lm) return;
    CHECK(!lm->skeleton.names.empty());
    CHECK(found->isSkinned());
}

// El caso simétrico: escena con "skinned": true cuyo FBX se reexportó luego sin
// huesos. Carga estática y sus fuentes de animación se descartan — pero con un
// aviso en Scene::lastWarnings() (lo que lee el Log Console), no en silencio.
static void test_scene_load_warns_when_rig_disappeared(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Prop");
    const uint64_t id = go->id;
    go->setMesh(std::make_shared<Mesh>(ModelLoader::load("assets/model.fbx")));

    nlohmann::json j = scene.toJson();
    // Simula el reexport: la escena creía que era skinned y guardó fuentes.
    j["root"]["children"][0]["mesh"]["skinned"] = true;
    j["root"]["children"][0]["mesh"]["animationSources"] = nlohmann::json::array({
        { {"path", "assets/modelAnimation.fbx"}, {"builtin", false}, {"clips", {"Salto"}} }
    });

    Scene loaded("Loaded");
    CHECK(loaded.fromJson(j, pm, am));
    GameObject* found = loaded.findById(id);
    CHECK(found != nullptr);
    if (!found) return;

    // Carga, pero estático: sin huesos no hay nada que animar.
    CHECK(found->hasMesh());
    CHECK(found->getSkinnedMesh() == nullptr);

    bool avisado = false;
    for (const auto& w : loaded.lastWarnings())
        if (w.find("model.fbx") != std::string::npos) avisado = true;
    CHECK(avisado);
}
```

Y en `main()`, junto a los otros round-trips de escena (tras `test_scene_without_animation_sources_loads(pm, am);`):

```cpp
    test_scene_load_ignores_stale_skinned_false(pm, am);
    test_scene_load_warns_when_rig_disappeared(pm, am);
```

- [ ] **Step 2: Ejecutar y verificar que fallan**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
```

Esperado: exit code 1, con `FAIL:` en `lm != nullptr` (primer test) y en `avisado` (segundo). El resto de tests siguen en verde.

`lastWarnings()` está declarado en `engine/include/DonTopo/Core/Scene.h:48` y devuelve `const std::vector<std::string>&`.

- [ ] **Step 3: Implementar**

En `engine/src/Core/Scene.cpp`, reemplaza el bloque de decisión (líneas ~432-438):

```cpp
            // "skinned" no existe en ficheros guardados antes de este campo
            // — default false, se reconstruyen como mesh estático (mismo
            // comportamiento que tenían antes de soportar skinned).
            bool skinned = j["mesh"].value("skinned", false);
            try
            {
                if (skinned && !sourcePath.empty())
                {
```

por:

```cpp
            // El flag "skinned" se sigue GUARDANDO (dato informativo, y no
            // rompe ficheros viejos) pero ya no se lee: manda el fichero, en
            // carga igual que en import. Si no fuera así, las escenas guardadas
            // antes de la auto-detección — todas con el flag a false, porque el
            // editor nunca creaba skinned — jamás podrían tener Animator sin
            // reimportar la malla a mano.
            const bool skinnedFlag = j["mesh"].value("skinned", false);
            const bool skinned     = !sourcePath.empty() && ModelLoader::hasBones(sourcePath);
            try
            {
                if (skinned)
                {
```

Y justo antes de ese `try`, el aviso del caso divergente:

```cpp
            // El FBX se reexportó sin huesos después de guardar la escena: sus
            // fuentes de animación no tienen dónde aplicarse y se pierden. Se
            // avisa por Scene::lastWarnings() (lo que lee el Log Console) en vez
            // de dejarlo pasar en silencio: perder animaciones sin decir nada es
            // el tipo de bug que el usuario descubre semanas después.
            if (skinnedFlag && !skinned && !sourcePath.empty() && warnings)
            {
                const std::string file = std::filesystem::path(sourcePath).filename().string();
                warnings->push_back(file + ": la escena lo tenía guardado como animado, pero el fichero"
                                            " ya no declara huesos; se descartan sus fuentes de animación");
            }
```

Comprueba que `Scene.cpp` incluye `<filesystem>` y `DonTopo/Renderer/ModelLoader.h`; ya llama a `ModelLoader::loadSkinned` en esta misma función, así que el header debería estar.

La rama `else if (!sourcePath.empty())` de debajo (línea ~494) no se toca: sigue siendo el camino estático, y ahora recibe también los FBX rigged-que-ya-no-lo-son.

- [ ] **Step 4: Ejecutar los tests**

```powershell
.\build.bat
.\build-ninja\engine\tests\dt_animator_tests.exe
```

Esperado: `dt_animator_tests: OK`. Presta atención a que los round-trips que ya existían (`test_animation_sources_survive_scene_round_trip`, `test_missing_animation_source_does_not_break_load`) sigan pasando: usan `modelAnimation.fbx`, que tiene huesos, así que la nueva condición debe seguir metiéndolos por la rama skinned.

- [ ] **Step 5: Ejecutar el resto de la suite**

```powershell
.\build-ninja\engine\tests\dt_camera_tests.exe
.\build-ninja\engine\tests\dt_content_browser_tests.exe
.\build-ninja\engine\tests\dt_physics_tests.exe
```

Esperado: `OK` en los tres. `Scene::fromJson` es código compartido y conviene saber que no se ha roto nada de lo que cuelga de él.

- [ ] **Step 6: Verificación manual del round-trip**

1. Con el editor: importar `assets/animatedCharacter/Maw J Laygo.fbx`, añadirle un Animator, y desde el panel Animator añadir la fuente `assets/animatedCharacter/standing idle 01.fbx`.
2. Guardar la escena.
3. Reabrirla: la malla vuelve como skinned, la fuente de animación sigue en la lista, y el estado que use su clip resuelve sin avisos en el Log Console.

- [ ] **Step 7: Commit**

```bash
git add engine/src/Core/Scene.cpp engine/tests/animator_tests.cpp
git commit -m "feat(scene): detect rig from file instead of stored skinned flag"
```

---

## Verificación final

Antes de dar la feature por hecha:

- [ ] `.\build.bat` limpio.
- [ ] Los cuatro ejecutables de test en `OK`.
- [ ] El recorrido manual completo de Task 3 Step 6, que es lo único que cubre la integración editor + renderer + escena. Ningún subagente tiene GUI: esta verificación la hace el usuario.
