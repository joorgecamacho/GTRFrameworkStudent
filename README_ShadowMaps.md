# Assignment 3 — Implementing Shadow Maps
## Documentación de cambios (Sesión de implementación)

---

## Resumen

Este documento explica en detalle todos los cambios realizados para implementar el sistema de **Shadow Maps** del Assignment 3 de Real-Time Graphics. El sistema final soporta múltiples luces proyectando sombras simultáneamente mediante un **Shadow Atlas**.

---

## Ficheros modificados

| Fichero | ¿Qué se cambió? |
|---------|-----------------|
| `src/gfx/fbo.cpp` | Fix crash macOS + swizzle para visualización |
| `src/pipeline/renderer.h` | Nueva estructura de datos para Shadow Atlas |
| `src/pipeline/renderer.cpp` | Lógica completa de generación del Shadow Atlas |
| `data/shader_atlas.glsl` | Shader de aplicación de sombras con Atlas |

---

## 1. `src/gfx/fbo.cpp` — Corrección de crashes en macOS

### Problema 1: Crash al iniciar la aplicación (`glPushAttrib` / `glPopAttrib`)

macOS usa un **OpenGL Core Profile** estricto, que no permite funciones de OpenGL "Legacy". Las funciones `glPushAttrib` y `glPopAttrib` son Legacy y producían un `GL_INVALID_OPERATION` que crasheaba la aplicación al intentar hacer bind del FBO.

```cpp
// ANTES (crasheaba en macOS):
glPushAttrib(GL_ALL_ATTRIB_BITS);  // ← NO existe en Core Profile
// ... código del FBO ...
glPopAttrib();                      // ← NO existe en Core Profile

// DESPUÉS (comentadas):
// glPushAttrib(GL_ALL_ATTRIB_BITS);
// glPopAttrib();
```

**Líneas afectadas:** ~230 y ~246 de `fbo.cpp`.

---

### Problema 2: La textura de profundidad se veía roja en macOS en lugar de gris

Cuando la UI del editor mostraba el Shadow Map, aparecía en rojo puro en lugar de escala de grises como en el PDF del profesor. Esto se debe a que macOS, al mostrar una textura de **1 solo canal** (depth), pone ese valor únicamente en el canal Rojo y deja Verde y Azul a 0.

La solución fue añadir un **Swizzle de textura** que copia el canal R en G y B:

```cpp
// Después de crear la depth texture en setDepthOnly():
glBindTexture(GL_TEXTURE_2D, depth_texture->texture_id);
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED); // G = R
glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED); // B = R
glBindTexture(GL_TEXTURE_2D, 0);
```

> ⚠️ **Esto es solo cosmético** para la visualización en la UI. Las matemáticas del shader siguen leyendo `.r` correctamente y son independientes del swizzle.

**Líneas afectadas:** ~217-221 de `fbo.cpp`.

---

## 2. `src/pipeline/renderer.h` — Nueva estructura de datos

### Eliminado (sistema de una sola luz):

```cpp
// ANTES — Solo admitía UNA luz con sombras:
int shadowmap_resolution;
int shadow_light_index;
int active_shadow_light_index;
```

### Añadido (sistema de Shadow Atlas para 16 luces):

```cpp
// DESPUÉS — El Atlas soporta hasta 16 luces simultáneas:
int atlas_resolution;   // Resolución total del FBO (ej: 4096x4096)
int atlas_tile_size;    // Tamaño de cada tile por luz (ej: 1024x1024)

// Un array de matrices: una por cada luz que proyecta sombras
Matrix44 shadow_viewprojections[16];

// Un array de rectángulos UV que indican DÓNDE está el tile de cada
// luz dentro del atlas (x=offset_x, y=offset_y, z=width, w=height)
// Todos en coordenadas normalizadas [0,1]
Vector4f shadow_atlas_rects[16];
```

**¿Por qué `Vector4f` para los rects?**  
Porque cada tile necesita 4 valores: `(x_inicio, y_inicio, ancho, alto)` dentro del atlas. Como el atlas mide 4096px y cada tile 1024px, los valores serán del tipo `(0.0, 0.0, 0.25, 0.25)` para el primer tile, `(0.25, 0.0, 0.25, 0.25)` para el segundo, etc.

---

## 3. `src/pipeline/renderer.cpp` — Lógica del Shadow Atlas

### 3.1. Constructor: FBO de alta resolución

```cpp
// ANTES (textura pequeña de 1024x1024 para UNA luz):
shadowmap_fbo->setDepthOnly(shadowmap_resolution, shadowmap_resolution);

// DESPUÉS (textura grande de 4096x4096 para el Atlas de múltiples luces):
atlas_resolution = 4096;
atlas_tile_size  = 1024;
shadowmap_fbo->setDepthOnly(atlas_resolution, atlas_resolution);
```

El FBO es ahora **una sola textura grande** dividida en cuadrículas. Con 4096/1024 = 4 tiles por fila, podemos albergar hasta 4×4 = 16 luces.

---

### 3.2. Función `renderShadowMap()`: bucle por cada luz

Esta es la función central. En lugar de renderizar la escena una vez desde una sola luz, ahora itera sobre **todas las luces activas** de la `light_list`:

```
Para cada luz i en light_list:
  1. Saltar si la luz no tiene cast_shadows activado
  2. Configurar la shadow_camera según el tipo de luz:
      - SPOT      → cámara perspectiva con el FOV del cono
      - DIRECTIONAL → cámara ortográfica con el área del inspector
  3. Calcular el tile en el atlas:
      tile_x = i % tiles_per_row   (columna)
      tile_y = i / tiles_per_row   (fila)
      vp_x   = tile_x * atlas_tile_size
      vp_y   = tile_y * atlas_tile_size
  4. Delimitar el área de dibujo con glViewport + glScissor
  5. Guardar la viewprojection matrix en shadow_viewprojections[i]
  6. Guardar el rect normalizado en shadow_atlas_rects[i]
  7. Renderizar todos los meshes opacos con el shader "flat"
```

**¿Por qué `glScissor`?**  
`glViewport` redirige dónde se dibujan los píxeles, pero el `glClear(GL_DEPTH_BUFFER_BIT)` afecta a TODA la textura. Usando `glScissor` junto con `glEnable(GL_SCISSOR_TEST)`, limitamos el clear y el draw estrictamente al tile de esa luz, para no borrar los tiles de otras luces.

```cpp
glViewport(vp_x, vp_y, atlas_tile_size, atlas_tile_size);
glEnable(GL_SCISSOR_TEST);
glScissor(vp_x, vp_y, atlas_tile_size, atlas_tile_size);
```

---

### 3.3. Cámara para luces Directionales (Bug crítico corregido)

```cpp
// ANTES (incorrecto — limitaba el área a 10 unidades, haciendo la escena negra):
if (ortho_area > 10.0f) ortho_area = 10.0f;  // ← ¡BUG!

// DESPUÉS (correcto — el área la controla el artista desde el Inspector):
float ortho_area = std::abs(shadow_light->area); // usa el valor del inspector
if (ortho_area < 1.0f) ortho_area = 50.0f;       // solo fallback si es 0
```

**¿Por qué hacía todo negro?**  
Con `area = -98` (como tiene la `moonlight`), el clamp lo limitaba a 10 unidades. El frustum de la luna cubría solo un cuadrado de 10×10 en el centro de la escena. Todo lo que estaba fuera de ese cuadrado **aún tenía asignado un tile en el atlas**, pero la textura en ese área tenía profundidad `1.0` (fondo del frustum). El shader comparaba la profundidad real del objeto con `1.0` → el objeto parecía estar "detrás" de la profundidad almacenada → `continue` → se ignoraba toda la contribución de luz → **negro**.

---

### 3.4. Envío de datos al shader

```cpp
// Enviar el atlas completo (en renderMeshWithMaterial):
shader->setUniform("u_shadowmap", shadowmap_fbo->depth_texture, 7);
shader->setMatrix44Array("u_light_viewprojections", shadow_viewprojections, 16);
shader->setUniform4Array("u_light_atlas_rects", (float*)shadow_atlas_rects, 16);
shader->setUniform("u_shadow_bias", shadow_bias);
```

En lugar de mandar una sola matrix y una sola textura, ahora se mandan **arrays completos** para que el shader pueda consultar los datos de cada luz independientemente.

---

## 4. `data/shader_atlas.glsl` (phong_single.fs) — Aplicación del Atlas

### 4.1. Nuevos uniforms

```glsl
// ANTES (solo UNA luz con sombras):
uniform sampler2D u_shadowmap;
uniform mat4 u_light_viewprojection;    // una sola matrix
uniform int  u_shadow_light_index;      // índice de la luz activa

// DESPUÉS (Shadow Atlas para TODAS las luces):
uniform sampler2D u_shadowmap;
uniform mat4  u_light_viewprojections[MAX_LIGHTS]; // una por luz
uniform vec4  u_light_atlas_rects[MAX_LIGHTS];     // rect del tile de cada luz
```

### 4.2. Lógica de shadow por luz (dentro del bucle de iluminación)

El bucle principal itera sobre cada luz `i`. Para cada una, la lógica de sombras es:

```glsl
if (u_shadow_enabled == 1)
{
    vec4 atlas_rect = u_light_atlas_rects[i];
    
    // Solo actuar si esta luz tiene un tile asignado (atlas_rect.z > 0)
    if (atlas_rect.z > 0.0)
    {
        // 1. Proyectar el fragmento al espacio de la cámara de luz i
        vec4 light_h = u_light_viewprojections[i] * vec4(v_world_position, 1.0);
        float safe_w = max(light_h.w, 0.00001);
        
        // 2. shadow_uv en [0,1] relativo al frustum de ESA luz
        vec2 shadow_uv = (light_h.xy / safe_w) * 0.5 + 0.5;
        
        // 3. Aplicar bias antes de dividir por W
        float biased_ndc_z = (light_h.z - u_shadow_bias) / safe_w;
        float current_depth = biased_ndc_z * 0.5 + 0.5;

        // 4. SOLO si el fragmento está DENTRO del frustum de esa luz...
        if (light_h.w > 0.0 &&
            shadow_uv.x >= 0.0 && shadow_uv.x <= 1.0 &&
            shadow_uv.y >= 0.0 && shadow_uv.y <= 1.0 &&
            current_depth >= 0.0 && current_depth <= 1.0)
        {
            // 5. ...remapear el UV al tile correspondiente del atlas
            vec2 atlas_uv = shadow_uv * atlas_rect.zw + atlas_rect.xy;
            
            // 6. Comparar profundidad
            float stored_depth = texture(u_shadowmap, atlas_uv).r;
            if (current_depth > stored_depth)
                continue; // En sombra: saltar contribución de esta luz
        }
        // Si el fragmento está FUERA del frustum: sin test, la luz contribuye normalmente
    }
}
```

### 4.3. Bug crítico del shader corregido (bounds check)

```glsl
// ANTES (incorrecto — remapeaba al atlas ANTES de comprobar bounds):
vec2 atlas_uv = shadow_uv * atlas_rect.zw + atlas_rect.xy;   // remap primero
if (atlas_uv.x >= atlas_rect.x && ...)                        // bounds check después
// ← PROBLEMA: atlas_uv SIEMPRE queda dentro del tile por construcción
//   matemática, el bounds check nunca descartaba nada.

// DESPUÉS (correcto — comprueba shadow_uv en [0,1] ANTES del remap):
if (shadow_uv.x >= 0.0 && shadow_uv.x <= 1.0 && ...)   // bounds check en frustum
{
    vec2 atlas_uv = shadow_uv * atlas_rect.zw + atlas_rect.xy; // remap después
    // ...
}
```

---

## 5. Shadow Bias (ejercicio 3.4)

El **Shadow Bias** se aplica para evitar el **Shadow Acne** (artefactos de auto-shadowing donde una superficie se sombrea a sí misma incorrectamente).

```glsl
// Se aplica ANTES de dividir por W (NDC), que es matemáticamente más correcto:
float biased_ndc_z = (light_h.z - u_shadow_bias) / safe_w;
float current_depth = biased_ndc_z * 0.5 + 0.5;
```

El bias se puede ajustar en tiempo real desde la UI del editor con el slider **"Shadow Bias"** en el panel Rendering. El valor por defecto es `0.0015`.

- **Bias demasiado bajo** → Shadow Acne (granulado en superficies)
- **Bias demasiado alto** → Peter Panning (las sombras "flotan" separadas del objeto)

---

## 6. Shadow Cull Front Faces (ejercicio 3.4)

Al renderizar el shadow map, se invierte el culling para eliminar otro tipo de artefacto:

```cpp
if (shadow_cull_front_faces) {
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);  // ← Renderizar caras traseras al FBO
}
```

Esto hace que en el depth buffer se registren las caras **traseras** de los objetos en lugar de las delanteras. Combinado con el bias, prácticamente elimina el shadow acne en superficies planas.

También controlable desde la UI con el checkbox **"Shadow Cull Front Faces"**.

---

## Cómo probar el sistema

1. **Compilar:**
   ```bash
   make -C build -j8 && ./build/GTR_Framework
   ```

2. **Activar sombras en la luna:**
   - Seleccionar `moonlight` en el panel Entities
   - En el Inspector, activar el checkbox `cast_shadows`
   - Deberías ver sombras proyectadas por los edificios y coches

3. **Activar sombras en un foco:**
   - Seleccionar `spot` o `spot2` 
   - Activar `cast_shadows` igualmente
   - Las dos luces proyectarán sombras simultáneamente

4. **Ver el Shadow Atlas en la UI:**
   - Menú **View → Textures**
   - Buscar la textura **`ShadowMap_Depth`** (4096×4096)
   - Cada tile activo (luz con `cast_shadows = true`) aparece como una región iluminada con la geometría proyectada desde esa luz

5. **Ajustar el bias:**
   - Panel **Rendering** → slider **"Shadow Bias"**
   - Valor recomendado: entre `0.001` y `0.005`

---

## Resumen de ejercicios completados

| Ejercicio | Descripción | Estado |
|-----------|-------------|--------|
| 3.1 | Crear FBO depth-only para el shadow map | ✅ |
| 3.2 | Renderizar la escena desde la cámara de luz (Spot + Directional) | ✅ |
| 3.3 | Aplicar el shadow map en el shader Phong single-pass | ✅ |
| 3.4 | Shadow Bias + Shadow Cull Front Faces para eliminar artefactos | ✅ |
| 3.5 | Shadow Atlas: soporte de múltiples luces en un solo FBO | ✅ |
| Fix | Crash macOS por `glPushAttrib`/`glPopAttrib` (Legacy OpenGL) | ✅ |
| Fix | Ortho area clamped a 10u → escena completamente negra | ✅ |
| Fix | Shader bounds check erróneo → sombras aplicadas fuera del frustum | ✅ |
