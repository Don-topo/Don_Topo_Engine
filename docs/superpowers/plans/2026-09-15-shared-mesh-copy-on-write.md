# Malla compartida con copia al modificar — plan de implementación

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Clonar un GameObject con malla (y deshacer su borrado) comparte la malla en vez de copiarla; solo se copia al modificarla.

**Architecture:** `GameObject::getMesh()` pasa a `shared_ptr<const Mesh>`; la única escritura es `editMesh()/editSkinnedMesh()`, que copia si la malla está compartida. `nodeFromJson` comparte una malla precargada cuando su configuración de animación coincide con la del JSON. El undo de Delete precarga las mallas vivas del subárbol.

**Tech Stack:** C++20, MSVC + Ninja. Tests `dt_*.exe` con `CHECK`.

**Spec:** `docs/superpowers/specs/2026-09-15-shared-mesh-copy-on-write-design.md` (`3301dd2`).

## Global Constraints

- Build con PowerShell desde la raíz: `& .\build.bat` (Release: `.\build-release.bat`). Tests con Bash desde la raíz: `./build-ninja/engine/tests/dt_animator_tests.exe`; suite `for exe in build-ninja/engine/tests/dt_*.exe; do "$exe" >/dev/null 2>&1 || echo "FALLA $exe"; done` (28/28 de partida).
- Edición solo con Edit/Write (CRLF y acentos). `git diff --stat` == `git diff --ignore-cr-at-eol --stat` antes de cada commit.
- Commits terminan en `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`.
- Sabotajes de uno en uno, tras commitear; restaurar con `git checkout --`. Scripts PowerShell sin `$ErrorActionPreference="Stop"`, build con `cmd /c ".\build.bat > nul 2>&1"`, pares como tablas hash.
- Formato del `.scene` sin cambios. Backends sin cambios.

## Mapa de ficheros

| Fichero | Tareas |
|---|---|
| `engine/include/DonTopo/Core/GameObject.h`, `engine/src/Core/GameObject.cpp` | 2 |
| `engine/include/DonTopo/Core/Scene.h`, `engine/src/Core/Scene.cpp` | 3, 4 |
| llamantes que escriben la malla (los señala el compilador) | 2 |
| `engine/src/Editor/Command.cpp`, `engine/include/DonTopo/Editor/Command.h` | 2, 4 |
| `engine/tests/animator_tests.cpp` | 1-4 |
| `docs/animation-audit.md`, `docs/core-audit.md` | 5 |

---

### Task 1: confirmar o descartar la sospecha

**Files:** Test: `engine/tests/animator_tests.cpp` (antes de `int main()`; llamada antes de `am.shutdown();`).

- [ ] **Step 1: test sobre el código actual**

```cpp
// ---- Malla compartida (Apéndice B) ----

// Clonar un objeto con un FBX de animación añadido NO duplica sus clips: el
// clon trae la configuración de fuentes del JSON, y re-aplicarla sobre una
// malla que ya la tiene la duplicaría (y releería el FBX).
static void test_clone_with_animation_source_keeps_clip_count(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    auto sm = std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx"));
    std::vector<std::string> w;
    CHECK(addAnimationSource(*sm, "assets/modelAnimation.fbx", w));
    const size_t clips = sm->animationClips.size();
    go->setMesh(sm);

    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr && clone->isSkinned());
    if (!clone || !clone->getSkinnedMesh()) return;
    CHECK(clone->getSkinnedMesh()->animationClips.size() == clips);
    CHECK(clone->getSkinnedMesh()->animationSources.size() == 2u);
}
```

Llamada: `test_clone_with_animation_source_keeps_clip_count(pm, am);`

- [ ] **Step 2: ejecutar y apuntar el resultado**

Build y test. Si FALLA: la sospecha era cierta; el test queda como regresión y lo arregla la tarea 3. Si PASA: la sospecha era falsa; se dice en el informe final y el test se conserva igualmente como red.

- [ ] **Step 3: commit** del test (`test(scene): clonar con fuente de animación no duplica clips`), incluso si falla — va marcado en el mensaje como "FALLA hasta la tarea 3" para no romper la suite: en ese caso, **no** añadir aún la llamada en `main` y añadirla en la tarea 3.

---

### Task 2: API `const` + `editMesh` + migración de escrituras

**Files:** `GameObject.h/.cpp`; los ficheros que el compilador señale; tests.

**Interfaces producidas:**
- `const std::shared_ptr<const Mesh>& GameObject::getMesh() const`
- `const SkinnedMesh* GameObject::getSkinnedMesh() const`
- `Mesh* GameObject::editMesh()`; `SkinnedMesh* GameObject::editSkinnedMesh()`
- `std::vector<const Material*> materialsOfMesh(const GameObject&)` y `std::vector<Material*> editMaterialsOfMesh(GameObject&)`

- [ ] **Step 1: tests**

```cpp
// editMesh sobre una malla única no copia; sobre una compartida, sí, y el otro
// dueño no ve el cambio.
static void test_edit_mesh_copies_only_when_shared()
{
    Scene scene("Test");
    GameObject* a = scene.addGameObject("A");
    GameObject* b = scene.addGameObject("B");
    auto m = std::make_shared<SkinnedMesh>(makeTwoClipFixture());
    a->setMesh(m);
    m.reset();                                   // A es el único dueño
    const Mesh* antes = a->getMesh().get();
    CHECK(a->editMesh() == antes);               // única: sin copia

    b->setMesh(std::const_pointer_cast<Mesh>(a->getMesh()));   // ahora compartida
    SkinnedMesh* editada = b->editSkinnedMesh();
    CHECK(editada != nullptr);
    CHECK(editada != a->getSkinnedMesh());       // B copió
    editada->material.texturePath = "solo_b.png";
    editada->animationClips.pop_back();
    CHECK(a->getMesh()->material.texturePath != "solo_b.png");
    CHECK(a->getSkinnedMesh()->animationClips.size() == 2u);
    CHECK(dynamic_cast<const SkinnedMesh*>(b->getMesh().get()) != nullptr);   // conserva el tipo
}

// Un override igual al valor que ya tiene la malla no fuerza la copia.
static void test_equal_material_override_does_not_copy()
{
    Scene scene("Test");
    GameObject* a = scene.addGameObject("A");
    GameObject* b = scene.addGameObject("B");
    auto m = std::make_shared<SkinnedMesh>(makeTwoClipFixture());
    m->material.texturePath = "igual.png";
    a->setMesh(m);
    b->setMesh(m);
    m.reset();

    MaterialOverride ov; ov.index = 0; ov.albedo = "igual.png";
    b->materialOverrides.push_back(ov);
    applyMaterialOverrides(*b);
    CHECK(b->getMesh().get() == a->getMesh().get());

    b->materialOverrides[0].albedo = "distinto.png";
    applyMaterialOverrides(*b);
    CHECK(b->getMesh().get() != a->getMesh().get());
    CHECK(b->getMesh()->material.texturePath == "distinto.png");
    CHECK(a->getMesh()->material.texturePath == "igual.png");
}
```

Llamadas en `main`.

- [ ] **Step 2: RED** — build: error `editMesh no es miembro`.

- [ ] **Step 3: `GameObject.h`**

```cpp
            // Solo lectura: la malla puede estar COMPARTIDA con otros objetos
            // (clon, undo de Delete). Para modificarla, editMesh().
            const std::shared_ptr<const Mesh>& getMesh() const { return m_mesh; }
            // Única vía de escritura. Si la malla está compartida
            // (use_count > 1) la copia primero —conservando su tipo: un skinned
            // se copia como SkinnedMesh— y el objeto pasa a apuntar a su copia.
            // Las referencias que no son de otro GameObject (caché de precarga,
            // un comando del undo) también cuentan: como mucho una copia de más.
            Mesh*        editMesh();
            SkinnedMesh* editSkinnedMesh();
```

`m_mesh` pasa a `std::shared_ptr<const Mesh>`; `setMesh(std::shared_ptr<Mesh> mesh)` guarda `std::move(mesh)` (conversión implícita a const). `getSkinnedMesh()` pasa a `const SkinnedMesh* getSkinnedMesh() const`.

- [ ] **Step 4: `GameObject.cpp`**

```cpp
    Mesh* GameObject::editMesh()
    {
        if (!m_mesh) return nullptr;
        if (m_mesh.use_count() > 1)
        {
            if (auto* sk = dynamic_cast<const SkinnedMesh*>(m_mesh.get()))
                m_mesh = std::make_shared<SkinnedMesh>(*sk);
            else
                m_mesh = std::make_shared<Mesh>(*m_mesh);
        }
        return std::const_pointer_cast<Mesh>(m_mesh).get();
    }

    SkinnedMesh* GameObject::editSkinnedMesh()
    {
        if (!dynamic_cast<const SkinnedMesh*>(m_mesh.get())) return nullptr;
        return static_cast<SkinnedMesh*>(editMesh());
    }
```

`materialsOfMesh` pasa a devolver `std::vector<const Material*>` desde `const GameObject&`, y se añade `editMaterialsOfMesh(GameObject&)` con el mismo cuerpo pero sobre `editMesh()`/`editSkinnedMesh()`.

`applyMaterialOverrides`: se calcula primero, sobre `materialsOfMesh` (const), si algún campo cambiaría (misma lógica de `aplica`/`aplicaFactor` pero comparando el valor final con el actual; los baselines se siguen capturando en `ov`, que es del GameObject). Si ninguno cambia, return. Si alguno cambia, se toma `editMaterialsOfMesh(go)` y se aplica como hoy.

- [ ] **Step 5: migración guiada por el compilador**

Build. Por cada error de escritura a través de `getMesh()`/`getSkinnedMesh()`:
- escritura de material → `editMaterialsOfMesh(go)` o `go->editMesh()->material`;
- escritura de clips/fuentes (`addAnimationSource`, `removeAnimationSource`, `renameClip`, `applyClipNamesPositionally`) → `*go->editSkinnedMesh()`;
- una lectura que solo faltaba de `const` → añadir `const` en la variable local.
Nunca `const_cast`. Repetir build hasta 0 errores. Revisar cada sitio por el riesgo de la spec (un `const Mesh*` guardado antes de un `edit*` que después se lee).

- [ ] **Step 6: GREEN + suite + commit** (`refactor(core): getMesh const y editMesh con copia al modificar`).

- [ ] **Step 7: sabotajes** — (a) `editMesh` sin el `if (use_count>1)` → cae `test_edit_mesh_copies_only_when_shared`; (b) copia como `Mesh` a secas → cae el `dynamic_cast` del mismo test; (c) `applyMaterialOverrides` sin la salida temprana → cae `test_equal_material_override_does_not_copy`.

---

### Task 3: compartir en el clon (`nodeFromJson`)

**Files:** `Scene.h/.cpp`; tests.

**Interfaces producidas:**
- `using PreloadedMeshCache = std::unordered_map<std::string, std::shared_ptr<const Mesh>>;`
- `bool meshMatchesAnimationConfig(const SkinnedMesh&, const nlohmann::json& animationSources);` (declarada en `Scene.h`)
- `PreloadedMeshCache Scene::collectMeshes(GameObject* root);`

- [ ] **Step 1: tests**

```cpp
// El clon comparte la malla en vez de copiarla.
static void test_clone_shares_the_mesh(PhysicsManager& pm, AudioManager& am)
{
    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    go->setMesh(std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned("assets/modelAnimation.fbx")));
    GameObject* clone = scene.cloneGameObject(go, nullptr, pm, am);
    CHECK(clone != nullptr);
    if (!clone) return;
    CHECK(clone->getMesh().get() == go->getMesh().get());
}

// Misma configuración de fuentes: coincide. Distinta (una fuente de más): no.
static void test_mesh_matches_animation_config()
{
    SkinnedMesh m = ModelLoader::loadSkinned("assets/modelAnimation.fbx");
    nlohmann::json fuentes = nlohmann::json::array();
    for (const auto& s : m.animationSources)
        fuentes.push_back({ {"path", s.path}, {"builtin", s.builtin}, {"clips", s.clipNames} });
    CHECK(meshMatchesAnimationConfig(m, fuentes));

    nlohmann::json mas = fuentes;
    mas.push_back({ {"path", "otro.fbx"}, {"builtin", false}, {"clips", nlohmann::json::array({"x"})} });
    CHECK(!meshMatchesAnimationConfig(m, mas));

    nlohmann::json renombrada = fuentes;
    if (!renombrada.empty() && !renombrada[0]["clips"].empty()) renombrada[0]["clips"][0] = "renombrado";
    CHECK(!meshMatchesAnimationConfig(m, renombrada));
}
```

Añadir aquí la llamada de `test_clone_with_animation_source_keeps_clip_count` si la tarea 1 lo dejó fuera.

- [ ] **Step 2: RED.**

- [ ] **Step 3: implementar**
  - `PreloadedMeshCache` a `shared_ptr<const Mesh>`.
  - `meshMatchesAnimationConfig`: mismo número de fuentes; por índice, `path`, `builtin` y `clips` (lista de strings, entera) iguales a `mesh.animationSources[i]`. JSON corrupto → `false`.
  - `Scene::collectMeshes(GameObject* root)`: el recorrido que hoy siembra `mallas` en `cloneGameObject` (solo mallas con `sourcePath`), sacado a función; `cloneGameObject` lo usa.
  - En `nodeFromJson`, rama skinned con `preloaded`: si la entrada es `SkinnedMesh` y `meshMatchesAnimationConfig(*sk, j["mesh"].value("animationSources", json::array()))` → `node->setMesh(std::const_pointer_cast<Mesh>(it->second))` y **saltar** el bloque de fuentes; si no, copia y configura como hoy. Rama estática con `preloaded`: compartir siempre.

- [ ] **Step 4: GREEN + suite + commit** (`perf(scene): el clon comparte la malla`).

- [ ] **Step 5: sabotajes** — (a) forzar `meshMatchesAnimationConfig` a `false` → cae `test_clone_shares_the_mesh`; (b) quitar la comparación de `clips` → cae el caso `renombrada`; (c) no saltar el bloque de fuentes al compartir → cae `test_clone_with_animation_source_keeps_clip_count` (o su equivalente si ya pasaba).

---

### Task 4: A8, undo de Delete sin disco

**Files:** `Scene.h/.cpp` (`insertFromJson`), `Command.h/.cpp` (`DeleteGameObjectCommand`); tests.

- [ ] **Step 1: test**

```cpp
// Undo de Delete con las mallas vivas: comparte y no lee disco.
static void test_insert_from_json_reuses_preloaded_mesh(PhysicsManager& pm, AudioManager& am)
{
    const std::filesystem::path temporal = std::filesystem::temp_directory_path() / "dt_undo_sin_disco.fbx";
    std::error_code ec;
    std::filesystem::remove(temporal, ec);
    std::filesystem::copy_file("assets/modelAnimation.fbx", temporal, ec);
    CHECK(!ec);
    if (ec) return;

    Scene scene("Test");
    GameObject* go = scene.addGameObject("Personaje");
    go->setMesh(std::make_shared<SkinnedMesh>(ModelLoader::loadSkinned(temporal.string())));
    nlohmann::json snap = scene.subtreeToJson(go);
    PreloadedMeshCache mallas = scene.collectMeshes(go);
    const Mesh* original = go->getMesh().get();
    scene.removeGameObject(go);
    std::filesystem::remove(temporal, ec);              // sin fichero: leer disco fallaría

    GameObject* r = scene.insertFromJson(snap, nullptr, 0, pm, am, &mallas);
    CHECK(r != nullptr);
    if (!r) return;
    CHECK(r->isSkinned());
    CHECK(r->getMesh().get() == original);
}
```

- [ ] **Step 2: RED** (firma de `insertFromJson` sin `preloaded`).

- [ ] **Step 3: implementar**
  - `insertFromJson(..., const PreloadedMeshCache* preloaded = nullptr)`: si llega, siembra `cache[ruta] = dynamic_cast<const SkinnedMesh*>(mesh) != nullptr` por cada entrada y pasa `preloaded` a `nodeFromJson`.
  - `DeleteGameObjectCommand`: miembro `PreloadedMeshCache m_meshes`; en `execute()`, antes de `removeGameObject(node)`, `m_meshes = m_scene.collectMeshes(node);`; en `undo()`, `insertFromJson(..., &m_meshes)`. Sin parámetro nuevo en el constructor (no hay obligación del llamante).

- [ ] **Step 4: GREEN + suite + commit** (`perf(editor): el undo de Delete reutiliza las mallas vivas`).

- [ ] **Step 5: sabotaje** — no pasar `preloaded` a `nodeFromJson` → cae `test_insert_from_json_reuses_preloaded_mesh`.

---

### Task 5: medición, Release, auditoría y verificación manual

- [ ] Arnés temporal de la fila 11 en Release (no se commitea): clon y undo de Delete. Objetivos: < 1 ms y < 10 ms.
- [ ] Release 28/28.
- [ ] `docs/animation-audit.md`: A7, A8, B6 y fila 11 con las cifras nuevas. `docs/core-audit.md`: Apéndice B cerrado con cifras.
- [ ] Manual (usuario), Vulkan y D3D12: textura del Mesh por objeto tras clonar (cambiar la del clon no toca el original); añadir una fuente de animación a un clon; `Instantiate` desde Lua; borrar un personaje y Ctrl+Z.
