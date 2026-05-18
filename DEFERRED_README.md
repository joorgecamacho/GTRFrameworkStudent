# Deferred Rendering - Implementación y Conceptos

Este documento explica de forma detallada la implementación de un **Deferred Renderer** (Renderizador Diferido) en este proyecto, explicando el concepto básico y mostrando explícitamente el código clave que hemos añadido a la arquitectura.

---

## 1. ¿Qué es el Deferred Rendering?

El **Deferred Rendering** (o Deferred Shading) es una técnica avanzada de renderizado en tiempo real diseñada para manejar escenas con una cantidad masiva de luces de forma eficiente. 

En un motor tradicional (**Forward Rendering**), por cada objeto que dibujamos, calculamos el color final teniendo en cuenta todas las luces que lo afectan de golpe. Si tenemos 100 objetos y 50 luces, la GPU podría llegar a ejecutar el complejo cálculo matemático de iluminación $100 \times 50$ veces por frame. Peor aún, muchos de esos objetos renderizados pueden acabar ocultos detrás de otros más cercanos (Overdraw), desperdiciando muchísimo tiempo de procesamiento.

El **Deferred Rendering** soluciona esto dividiendo el proceso de renderizado en dos fases (pasadas) muy claras:

### Fase 1: Geometry Pass (Llenar el G-Buffer)
En esta primera pasada no se calcula **nada de iluminación**. Solamente renderizamos la geometría de la escena (todos los modelos 3D opacos) y guardamos sus propiedades puras y crudas en múltiples texturas de forma simultánea. Este conjunto de texturas se llama **G-Buffer** (Geometry Buffer).
Normalmente en el G-Buffer guardamos:
- **Albedo**: El color base del objeto sin luz.
- **Normales**: Hacia dónde apunta la superficie pixel a pixel (modificado por los normal maps).
- **Depth**: La profundidad o distancia de ese píxel concreto hasta la cámara.

### Fase 2: Lighting Pass (Pasada de Iluminación)
Con el G-Buffer ya lleno, tenemos algo así como una "fotografía técnica" con las propiedades geométricas de la pantalla pixel a pixel. 
Ahora, para calcular la luz, nos dan igual los modelos 3D reales. En su lugar, dibujamos la influencia matemática de cada luz directamente sobre la pantalla leyendo los datos del G-Buffer. 
* **Si es una luz direccional:** Dibujamos un cuadrado que ocupa toda la pantalla (Full-screen Quad) y le aplicamos la luz uniformemente.
* **Si es una luz puntual o foco (Spot):** Dibujamos una simple geometría 3D, como una **esfera**, exactamente en la posición de la luz y con el tamaño de su rango máximo. Solamente los píxeles en pantalla que estén dentro de esa esfera recibirán luz de ese foco.

**La gran ventaja:** La iluminación se calcula de forma estrictamente 2D por cada píxel visible iluminado en pantalla, solucionando el problema del overdraw de forma radical y permitiendo poner cientos de luces sin que los FPS sufran una caída catastrófica.

---

## 2. Código Implementado

A continuación, se detalla explícitamente el código que hemos añadido en el Framework para lograr implementar esta técnica.

### A. Preparación de los Framebuffers (FBOs) en `renderer.cpp`
Creamos dos FBOs fundamentales: el `gbuffer_fbo` para almacenar la información geométrica, y el `illumination_fbo` para acumular iterativamente la luz final.

```cpp
// En la inicialización y validación del tamaño de pantalla de Renderer::renderScene
if (gbuffer_fbo == nullptr || gbuffer_fbo->color_textures[0]->width != width || gbuffer_fbo->color_textures[0]->height != height) {
    if (gbuffer_fbo) delete gbuffer_fbo;
    gbuffer_fbo = new GFX::FBO();

    // Pedimos: 2 texturas (Color Albedo y Normales empacadas), 
    // formato RGBA (4 canales) y activamos el buffer de profundidad (true)
    gbuffer_fbo->create(width, height, 2, GL_RGBA, GL_UNSIGNED_BYTE, true);
}

if (illumination_fbo == nullptr || illumination_fbo->color_textures[0]->width != width || illumination_fbo->color_textures[0]->height != height) {
    if (illumination_fbo) delete illumination_fbo;
    illumination_fbo = new GFX::FBO();

    // 1 textura final donde acumularemos todas las luces sumadas
    illumination_fbo->create(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, true);
}
```

### B. Shaders Añadidos en el `shader_atlas.glsl`

Para implementar el Deferred Shading, necesitamos separar la matemática de iluminación que antes estaba en `phong.fs` en múltiples shaders distintos.

#### 1. El Shader de llenado del G-Buffer (`gbuffer.fs`)
Extrae las propiedades del modelo (descartando píxeles tipo máscara) y las escribe en múltiples "attachments" (MRT: Multiple Render Targets) de forma simultánea.

```glsl
// TAREA 2.2: Declarar las salidas a múltiples texturas
layout(location = 0) out vec4 gbuffer_albedo;
layout(location = 1) out vec4 gbuffer_normal;

void main() {
    vec4 color = u_color * texture(u_texture, v_uv);

    // Alpha masking para materiales transparentes/recortados tipo hojas o rejas
    if(color.a < u_alpha_cutoff) {
        discard; // Si es muy transparente lo tiramos y no entra al GBuffer
    }

    // Calcular las Normales aplicando la influencia del Normal Map
    vec3 N = normalize(v_normal);
    if (u_has_normal_texture == 1 && u_show_normals == 1) {
        vec3 normal_pixel = texture(u_normal_texture, v_uv).xyz;
        normal_pixel = normal_pixel * 2.0 - 1.0;
        N = perturbNormal(N, v_world_position, v_uv, normal_pixel);
    }

    // Escribir en el G-Buffer
    gbuffer_albedo = vec4(color.rgb, 1.0);
    
    // IMPORTANTE: Las normales van de -1 a 1, pero la textura guarda valores de 0 a 1.
    // Hay que empaquetarlas multiplicando por 0.5 y sumando 0.5:
    gbuffer_normal = vec4(N * 0.5 + 0.5, 1.0); 
}
```

#### 2. Shader Global Deferred (`deferred_global.fs`)
Calcula la luz Ambiental y todas las luces Direccionales usando un simple **Quad** (Un polígono de un cuadrado que abarca por completo tu monitor/pantalla).

```glsl
// 1. Leemos del G-Buffer en base a nuestras UVs de pantalla
vec3 albedo = texture(u_albedo_texture, v_uv).rgb;
// Desempaquetamos normales de [0,1] a [-1,1]
vec3 normal = texture(u_normal_texture, v_uv).xyz * 2.0 - 1.0; 
float depth = texture(u_depth_texture, v_uv).x;

// 2. Reconstruimos la posición 3D real del píxel original usando la matriz inversa de la cámara
vec2 screen_pos = v_uv * 2.0 - 1.0;
vec4 proj_pos = vec4(screen_pos, depth * 2.0 - 1.0, 1.0);
vec4 world_pos_homo = u_inverse_viewprojection * proj_pos;
vec3 world_pos = world_pos_homo.xyz / world_pos_homo.w;

// 3. Calculamos y sumamos luz ambiental pura al color
vec3 out_color = u_ambient_light * albedo;

// 4. Bucle para añadir matemáticamente las luces direccionales...
for(int i = 0; i < u_num_dir_lights; ++i) {
    out_color += calculateDirectionalLight(...);
}

// 5. Devolvemos el color ya iluminado a la pantalla
FragColor = vec4(out_color, 1.0);
```

#### 3. Shader de Volúmenes de Luz (`deferred_light.fs`)
Renderiza esferas matemáticas. Si una esfera coincide con geometría que vemos en la pantalla, calcula e inyecta la luz usando matemáticas de Phong (iluminando así ese objeto).

```glsl
// Exactamente el mismo desempaquetado de G-Buffer que en la versión global
vec3 albedo = texture(u_albedo_texture, screen_uv).rgb;
vec3 normal = texture(u_normal_texture, screen_uv).xyz * 2.0 - 1.0;
float depth = texture(u_depth_texture, screen_uv).x;
// ... reconstrucción de posición usando inverse_viewprojection ...

// Aquí ya no aplicamos luz ambiental, SÓLO sumamos las luces locales Point o Spot
vec3 result = vec3(0.0);
if(u_light_type == 2) { 
    result = calculatePointLight(...);
} else if(u_light_type == 3) {
    result = calculateSpotLight(...);
}

// Devolvemos sólo el destello de la luz local para que se sume (blend)
FragColor = vec4(result, 1.0);
```

### C. La Lógica Principal: `renderDeferred` (C++)

La función `renderDeferred` coordina toda la danza entre estos FBOs y Shaders. Es la pieza arquitectónica central que hemos añadido:

```cpp
void Renderer::renderDeferred(Camera* camera) {
	
    // =========================================================================
    // 1. TRANSFERENCIA DEL DEPTH
    // Copiamos la profundidad del GBuffer al Illumination FBO.
    // Esto es vital para poder usar Light Volumes correctamente y, 
    // más adelante, para el dibujado de transparencias (Forward).
    // =========================================================================
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer_fbo->fbo_id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, illumination_fbo->fbo_id);
    glBlitFramebuffer(
        0, 0, gbuffer_fbo->width, gbuffer_fbo->height,
        0, 0, illumination_fbo->width, illumination_fbo->height,
        GL_DEPTH_BUFFER_BIT, GL_NEAREST
    );
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    illumination_fbo->bind();
    glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
    // Limpiamos SOLO el color, porque queremos mantener el Depth que acabamos de copiar
    glClear(GL_COLOR_BUFFER_BIT); 

    // =========================================================================
    // 2. PASADA GLOBAL (Ambiental + Direccionales)
    // Renderizamos un QUAD de pantalla completa usando las variables
    // del G-Buffer sin test de profundidad.
    // =========================================================================
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    GFX::Shader* global_shader = GFX::Shader::Get("deferred_global");
    global_shader->enable();
    
    // Pasar todas las texturas extraídas del G-Buffer
    global_shader->setUniform("u_albedo_texture", gbuffer_fbo->color_textures[0], 0);
    global_shader->setUniform("u_normal_texture", gbuffer_fbo->color_textures[1], 1);
    global_shader->setUniform("u_depth_texture", gbuffer_fbo->depth_texture, 2);
    // ... envío de matrices de cámara y arrays de luces direccionales ...
    
    // Dibujamos el cuadrado
    GFX::Mesh::getQuad()->render(GL_TRIANGLES);
    global_shader->disable();

    // =========================================================================
    // 3. PASADA DE VOLÚMENES DE LUZ (Puntuales y Spot)
    // =========================================================================
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE); // BLENDING ADITIVO (Para sumar luces unas con otras)
    
    // LÓGICA DE DEPTH PARA VOLÚMENES 3D:
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER); // Algoritmo: Ejecuta la luz en el objeto real de la pantalla
                             // que esté DELANTE de las "paredes traseras" de la esfera virtual.
    glDepthMask(GL_FALSE); // Las esferas no escriben su profundidad física al Depth Buffer
    glEnable(GL_CULL_FACE);
    glFrontFace(GL_CW); // Renderizar solo las caras traseras (Back faces) de las esferas

    GFX::Shader* light_shader = GFX::Shader::Get("deferred_light");
    light_shader->enable();

    // Pasar texturas del G-Buffer otra vez al shader de volúmenes...

    for (int i = 0; i < light_list.size(); ++i) {
        if (light_list[i]->light_type == eLightType::DIRECTIONAL) continue;

        LightEntity* light = light_list[i];
        
        // Escalar la esfera abstracta para que su tamaño físico coincida
        // con el "rango/radio máximo" exacto del foco de luz en el mundo.
        Matrix44 light_model;
        Vector3f p = light->root.model.getTranslation();
        light_model.setTranslation(p.x, p.y, p.z);
        light_model.scale(light->max_distance, light->max_distance, light->max_distance);

        light_shader->setUniform("u_model", light_model);
        // ... uniforms the luz (posicion, color, intensidad, falloff)...
        
        // Dibujamos la geometría de esfera 3D
        sphere.render(GL_TRIANGLES);
    }
    
    // Restaurar el estado GL estándar
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glFrontFace(GL_CCW);
    glDisable(GL_BLEND);
    
    illumination_fbo->unbind();

    // =========================================================================
    // 4. COPIADO A PANTALLA Y RENDER DE TRANSPARENCIAS (Forward Pass de Cristales)
    // El Deferred no puede renderizar objetos traslúcidos en su G-Buffer (solo un objeto a la vez por pixel),
    // así que los cristales se renderizan AHORA con Forward Clásico, dibujándose POR ENCIMA.
    // =========================================================================
    
    // Dibujamos la textura completa del Illumination en el Viewport
    glClearColor(0.0, 0.0, 0.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    illumination_fbo->color_textures[0]->toViewport();

    // Copiamos mágicamente de nuevo el Depth buffer para que los objetos transparentes que
    // pintemos ahora puedan tapar a cosas de la escena si fuera necesario
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer_fbo->fbo_id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0,0, gbuffer_fbo->width, gbuffer_fbo->height,
                      0,0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Iteramos renderizando SOLAMENTE los materiales Transparentes (BLEND)
    for (int i = 0; i < render_list.size(); i++) {
        if (render_list[i].material->alpha_mode == SCN::eAlphaMode::BLEND) {
            renderMeshWithMaterialForward(render_list[i].matrix, render_list[i].mesh, render_list[i].material);
        }
    }
}
```

---

## 3. Relación con el Assignment (¿Qué tenemos implementado y por qué?)

Hemos implementado **todos los requisitos obligatorios** del Assignment 4. A continuación, se detalla nuestra elección para cada apartado:

### ✔️ 2.1 Generate the G-Buffer
**Implementado**: Creamos el `gbuffer_fbo` con 2 texturas RGBA y un buffer de profundidad. Mantenemos 4 canales por textura por simplicidad y alineación de memoria en la GPU.

### ✔️ 2.2 Filling the G-Buffer with Opaque Geometry
**Implementado**: Hemos modificado el código para que, cuando se usa Deferred, la función `renderScene` omita los objetos con Alpha Blending y mande toda la geometría opaca a `renderMeshWithMaterial`, la cual usa el nuevo `gbuffer.fs` con múltiples salidas (`layout(location = 0)` y `1`).

### ✔️ 2.3 & 2.4.2 Deferred first pass (Global / Single Pass)
**Implementado**: En lugar de hacer toda la luz de golpe o usar un pase simple para todo, nos hemos adherido directamente a la arquitectura final propuesta en el **2.4.2**. Primero dibujamos un Quad a pantalla completa (`deferred_global.fs`) que computa exclusivamente la luz Ambiental y las luces Direccionales, y que sirve de lienzo base opaco para el frame de iluminación.

### ✔️ 2.4 & 2.4.1 Light volumes & Preparing the Light FBO
**Implementado**: Creamos el `illumination_fbo` y utilizamos `glBlitFramebuffer` para copiarle fielmente el búfer de profundidad del G-Buffer original. Esto es crítico para que las esferas de luz puedan hacer intersección correctamente con los modelos 3D.

### ✔️ 2.4.3 Rendering the light volumes
**Implementado**: Para los farolas (Spot) y luces puntuales (Point) dibujamos esferas abstractas limitadas a su `max_distance`. Configuramos OpenGL meticulosamente: `glDepthFunc(GL_GREATER)` para iluminar lo que hay por delante del fondo de la esfera, `glDepthMask(GL_FALSE)` para no corromper la profundidad, `glFrontFace(GL_CW)` para culling trasero y `glBlendFunc(GL_ONE, GL_ONE)` para luz puramente aditiva.

### ✔️ 2.4 Handling transparencies (La decisión de diseño)
**Implementado vía Forward Rendering**: El assignment nos daba dos opciones: *Checkerboarding* (Transparencias falsas descartando píxeles) o *Forward Rendering* clásico (Pintando las transparencias encima del deferred). 
**¿Por qué elegimos Forward Rendering?** Porque el checkerboarding da una calidad visual deficiente (se ve pixelado/granulado). Usando un pase Forward final, los cristales y vidrios se ven estéticamente perfectos. Para lograr esto, tras terminar el deferred, volcamos el color y restauramos el Depth original a la pantalla y luego ejecutamos la versión "Forward" antigua (`renderMeshWithMaterialForward`) solo para los cristales.

### ✔️ 2.5 ImGUI controls
**Implementado**: Hemos añadido el botón `Use Deferred` en la interfaz gráfica (`showUI()`). Esto permite cambiar en tiempo real de Forward a Deferred y comprobar que ambos producen visualmente el mismo resultado idéntico, demostrando el éxito de la implementación matemática.

### ❌ Extras (2.5 Extra, 2.6 Extra, 2.7 Extra)
**No Implementados**: El objetivo fundamental era asegurar una implementación perfecta, limpia y 100% estable (sin bugs visuales ni glitches de transparencias) de la base (Core Pipeline). Los extras como comprimir las normales u optimizar los spots usando conos en vez de esferas se han descartado por ahora a favor de la pureza didáctica y estabilidad del código.

### Resumen del Flujo General Añadido a tu Motor
1. En `renderScene`, evaluamos si el usuario seleccionó en la UI "Use Deferred".
2. Si es así, iteramos la geometría de la escena dibujándola con el shader extremadamente simplificado `gbuffer` que no aplica luz, al interior de `gbuffer_fbo`. Los cristales u objetos con `alpha_mode == BLEND` **se ignoran en este paso de forma programática** usando un condicional.
3. Llamamos a nuestra gran función `renderDeferred(camera)`.
4. Transferimos el Depth Buffer.
5. Calculamos la luz global unificada leyendo del G-Buffer.
6. Imprimimos esferas de luz aditivas sobre la pantalla para procesar de forma masiva los focos y bombillas (`Light Volumes`).
7. Copiamos al monitor final la imagen iluminada de forma sintética.
8. Dibujamos por fin la geometría transparente restante usando la lógica del Pipeline Forward, que evalúa el fondo que hay detrás de los cristales usando Alpha Blending y completa la imagen de tu videojuego.
