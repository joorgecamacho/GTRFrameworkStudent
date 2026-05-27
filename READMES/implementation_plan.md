# Plan de Refinamiento: Efecto Sci-Fi Scan (Gradientes, Rastro y Control Manual)

Este plan detalla los cambios necesarios para perfeccionar la apariencia y usabilidad del efecto de escáner en base a tu feedback:
1. **Desvanecimiento progresivo del rastro (Trailing Fade-out):** Haremos que el oscurecimiento y las líneas de escaneo azules se desvanezcan suavemente a medida que el frente de onda se aleja, evitando que se queden fijos en el suelo.
2. **Líneas con gradiente suave (Glowing Lines):** Sustituiremos el corte seco y grueso actual por un perfil de línea simétrico basado en `smoothstep` que simule un haz de luz holográfica brillante que se suaviza hacia los bordes.
3. **Controles manuales e interactivos (Timeline & Pause):** Añadiremos un modo manual en la interfaz ImGui con un deslizador de línea de tiempo ("Timeline") y un botón de pausa para poder congelar el efecto a cualquier distancia, rebobinar, avanzar a mano y tomar capturas de pantalla cómodamente para la presentación.

---

## Cambios Propuestos

### 1. Shader (`data/shader_atlas.glsl`)

#### [MODIFY] [shader_atlas.glsl](file:///c:/Users/Jorge/GTRFrameworkStudent/data/shader_atlas.glsl)
* **Líneas con Gradiente Suave:**
  Modificaremos la función `CalculateScanLines(float dist)` para que, en lugar de usar `step(fracDist, u_line_width)` (que devuelve un valor binario 0 o 1 seco), calcule una rampa simétrica:
  ```glsl
  float halfWidth = u_line_width * 0.5;
  float distToCenter = abs(fracDist - halfWidth);
  return smoothstep(halfWidth, 0.0, distToCenter);
  ```
  Esto creará líneas que son más brillantes en el centro y se desvanecen suavemente hacia los bordes.
* **Desvanecimiento del rastro (Trailing Fade-out):**
  - Declararemos un nuevo `uniform float u_trail_width;` que controlará la longitud del rastro en metros.
  - Calcularemos una máscara de rastro usando `smoothstep`:
    ```glsl
    float trailMask = smoothstep(u_scan_radius - u_trail_width, u_scan_radius, dist);
    ```
  - Aplicaremos esta máscara `trailMask` al oscurecimiento del terreno (`ApplyDarkenBlend`) y a las líneas de escaneo (`linesMask`), logrando que desaparezcan suavemente por detrás de la onda a medida que avanza.

---

### 2. C++ (`src/pipeline/renderer.h` y `renderer.cpp`)

#### [MODIFY] [renderer.h](file:///c:/Users/Jorge/GTRFrameworkStudent/src/pipeline/renderer.h)
* Añadiremos las siguientes variables miembro a la clase `Renderer`:
  - `bool scan_manual_mode = false;` (Activa el control manual de la animación)
  - `bool scan_paused = false;` (Pausa la reproducción automática de la animación)
  - `float scan_time = 0.0f;` (Acumulador del tiempo actual del escáner en segundos)
  - `long last_scan_frame_time = 0;` (Marca de tiempo del último frame para calcular el delta local)
  - `float scan_trail_width = 12.0f;` (Ancho del desvanecimiento del rastro en metros)

#### [MODIFY] [renderer.cpp](file:///c:/Users/Jorge/GTRFrameworkStudent/src/pipeline/renderer.cpp)
* **Actualización del tiempo delta:**
  - Cuando se active el escáner, inicializaremos `scan_time = 0.0f` y `last_scan_frame_time = getTime()`.
  - En cada frame que el escáner esté activo, si no está en modo manual y no está pausado, sumaremos el delta de tiempo real al acumulador `scan_time`.
* **Evaluación de Fases:**
  - Modificaremos la lógica del actualizador de fases para que dependa exclusivamente de la variable `scan_time` (en lugar de `t = (getTime() - scan_start_time) * 0.001f`).
  - Esto nos permitirá controlar el escáner tanto de forma automática (avanzando el tiempo cada frame) como de forma manual (sobrescribiendo `scan_time` desde un slider en ImGui).
* **Envío de Uniforms:**
  - Pasaremos el parámetro `scan_trail_width` al shader mediante `scan_shader->setUniform("u_trail_width", scan_trail_width);`.
* **UI en ImGui (`Renderer::showUI`):**
  - Añadiremos un checkbox para activar/desactivar el **Modo Manual**.
  - Si el Modo Manual está desactivado (Auto):
    - Mostraremos un botón para **Pausar/Reanudar** la animación.
    - Mostraremos una barra de progreso o slider deshabilitado de `scan_time` para ver el progreso de la animación.
  - Si el Modo Manual está activado:
    - Mostraremos un slider editable para controlar el **Tiempo de Animación (scrubbing)** manualmente de `0.0` a `3.0` segundos. Mover este slider actualizará instantáneamente el radio, la opacidad y la carga para ver la animación cuadro a cuadro en tiempo real.
    - Mostraremos sliders independientes para `scan_radius`, `scan_opacity` y `scan_charge_radius` por si se desea hacer un ajuste libre sin seguir la curva predefinida.
  - Expondremos deslizadores para:
    - **Largo del Rastro (`scan_trail_width`)** para ajustar qué tan rápido se desvanece el suelo y las líneas por detrás de la onda.
    - **Grosor de Línea (`u_line_width`)** para afinar a gusto la apariencia del gradiente de las líneas.

---

## Plan de Verificación

1. **Prueba de Desvanecimiento (Rastro):**
   - Iniciaremos el escáner y verificaremos visualmente que tanto la oscuridad azul marino como las líneas concéntricas de escaneo se desvanecen limpiamente por detrás de la onda frontal a medida que esta se propaga, dejando el suelo original intacto a distancias lejanas de la onda.
   - Ajustaremos el slider de rastro en tiempo real para ver cómo cambia la longitud de la cola.
2. **Prueba de Gradiente en Líneas:**
   - Observaremos de cerca las líneas concéntricas de color azul. Deberán verse como haces de luz suaves y con volumen en lugar de líneas gruesas de color plano.
3. **Prueba de Control Manual y Pausa:**
   - Activaremos el escáner y haremos clic en el botón "Pausar" a mitad de camino. La onda debe detenerse por completo. Podremos rotar la cámara y verificar que el efecto permanece congelado y perfectamente estable.
   - Activaremos el "Modo Manual" y arrastraremos el slider de tiempo. Deberíamos ser capaces de mover la onda hacia adelante y hacia atrás de forma ultra-fluida, lo cual es ideal para tomar capturas de pantalla de la fase de carga, del frente intermedio, o del fade-out final.
