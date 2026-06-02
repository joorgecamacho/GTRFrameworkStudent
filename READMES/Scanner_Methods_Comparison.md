# Comparativa de Métodos: Escáner Sci-Fi y Documentación del Enfoque Actual

Este documento está diseñado para aclarar las diferencias entre los dos métodos principales para implementar un efecto de escáner de terreno, y documentar exhaustivamente el enfoque técnico que **hemos decidido implementar en nuestro framework**.

---

## 1. Los Dos Caminos: Post-Procesado vs. Expansión Geométrica

A la hora de crear un escáner tipo *Death Stranding*, existen dos escuelas de pensamiento principales. Es crucial entender que **no son lo mismo** ni visual ni técnicamente.

### Método A: Expansión de Malla Geométrica (El método alternativo)
Este método consiste en spawnear una **esfera 3D invisible** en el mundo (por ejemplo, usando un sistema de partículas) que crece rápidamente con el tiempo. El material de esta esfera utiliza un **Shader de Intersección de Profundidad**.
- **Cómo funciona:** El shader pinta una línea brillante *únicamente* en los lugares donde los polígonos de la esfera atraviesan (intersectan) la geometría del mundo (el suelo, las rocas).
- **Ventajas:** Es extremadamente barato en rendimiento (depende de los vértices de la esfera, no de la resolución de la pantalla) y permite detectar colisiones físicas en un motor si se le añade un *collider*.
- **Desventajas:** Está muy limitado visualmente. Solo puede dibujar un "anillo" de contacto. Dibujar líneas concéntricas múltiples (una cuadrícula), oscurecer el suelo dinámicamente por detrás de la onda, o hacer un desvanecimiento progresivo del rastro es prácticamente imposible sin trucos de rendering muy complejos.

### Método B: Post-Procesado Screen-Space (Nuestro Método Implementado)
Este método descarta el uso de geometría extra. En su lugar, es un **Filtro 2D a pantalla completa (Post-Process)** que se ejecuta en la tarjeta gráfica para cada píxel de la pantalla.
- **Cómo funciona:** Lee el *Depth Buffer* (la profundidad) de la escena y, mediante cálculos matriciales, reconstruye las coordenadas 3D exactas de cada píxel de la pantalla. Con eso, evalúa distancias y dibuja todo el efecto holográfico usando matemáticas puras.
- **Ventajas:** Permite una libertad creativa absoluta. Como operamos píxel a píxel, podemos dibujar infinitas líneas holográficas, aplicar modos de fusión complejos de Photoshop (como oscurecer el suelo) y crear rastros difuminados. Se adapta a la topografía a nivel de píxel, por lo que nunca hay errores de solapamiento.
- **Desventajas:** El coste de rendimiento depende de la resolución de la pantalla (Fill-rate). No sirve para detectar colisiones físicas (es puramente visual).

**Conclusión para tu compañero:** Nosotros hemos optado por el **Método B (Post-Procesado)** porque nuestro objetivo era lograr una calidad visual "Premium" multicapa, lo cual es inalcanzable con una simple esfera de intersección (Método A).

---

## 2. Explicación Súper Detallada de Nuestra Implementación

Nuestro efecto es un **Post-Proceso Integrado en un Pipeline Deferred**, ejecutado *después* del tonemapping (para que nuestras luces no se distorsionen) pero *antes* de los objetos transparentes (para no dibujar líneas sobre los cristales). Se compone de 6 capas o bloques matemáticos que se ejecutan en el *Fragment Shader* de manera simultánea.

### Capa 1: Reconstrucción de World Position (El ancla)
Si dibujáramos las líneas basándonos solo en la pantalla plana 2D, se deslizarían erráticamente por el suelo al girar la cámara (efecto pegatina). Para evitarlo, tomamos las coordenadas UV del píxel y su valor en el G-Buffer de profundidad. 
Multiplicamos este punto por la matriz inversa de *View-Projection* para obtener sus **coordenadas exactas en el mundo 3D real**. Todas las distancias del escáner se miden desde el origen (el jugador) hacia esta coordenada 3D, garantizando que el holograma se "clave" al suelo sin importar dónde mire la cámara.

### Capa 2: Darken Blend (El lienzo de alto contraste)
Si proyectas un holograma brillante sobre nieve, se vuelve invisible. Para solucionarlo, aplicamos una fusión matemática en el suelo. En lugar de multiplicar la luz (lo que destruiría las texturas volviéndolas negras), usamos la función matemática del modo **Darken** de Photoshop:
`blended = min(sceneColor, darkenColor)`
Esta función detecta las zonas muy iluminadas del terreno y las oscurece a un azul marino profundo, pero deja intactas las zonas que ya eran oscuras por sus propias sombras. Esto crea un lienzo de altísimo contraste preservando los detalles de las rocas.

### Capa 3: El Frente de Onda (Edge Gradient)
Para dar sensación de volumen y energía, creamos un anillo frontal brillante. Utilizamos la función de transición suave `smoothstep(R_{scan} - W_{edge}, R_{scan}, dist)` combinada con un límite binario estricto (`step`). Esto genera un perfil en "diente de sierra": la luz crece suavemente a lo largo de los últimos 0.8 metros del abanico y se corta de tajo exactamente en el límite exterior del radio, emulando un choque de energía.

### Capa 4: Grid Topográfico Holográfico (Las líneas azules)
No usamos texturas proyectadas; generamos las líneas mediante la función de repetición `fract(dist / intervalo)`.
- **Separación dinámica:** El intervalo entre las líneas crece un 3% por cada metro que se alejan del jugador. De cerca están juntas y detalladas; de lejos se separan, evitando convertirse en una mancha borrosa azul en el horizonte.
- **Suavizado (Glow):** Aplicamos otro `smoothstep` centrado en el interior de cada línea para que no sea un bloque sólido de color barato, sino que tenga un núcleo brillante y un borde degradado suave, como luz real.

### Capa 5: Visual Cue (La Línea Guía Blanca)
Para atraer la atención visual inmediatamente a la expansión de la onda, el shader calcula matemáticamente cuál es la primera línea del grid. Si la distancia al píxel cae en el primer intervalo detrás del borde, aplicamos una **máscara de emisión blanca**. Las líneas posteriores quedan en color cian holográfico estándar.

### Capa 6: Trailing Fade-Out (La Zona de Disipación / Clean up)
Para evitar que el mundo se sature de líneas que no desaparecen (ruido visual), implementamos una disipación trasera. 
`trailMask = smoothstep(R_{scan} - W_{trail}, R_{scan}, dist)`
Establecemos un "ancho de cola" (ej. 12 metros). A medida que la onda avanza, el shader calcula la distancia. Cualquier píxel que quede por detrás de esos 12 metros reduce su opacidad progresivamente a cero. Esto disuelve tanto las líneas azules como el oscurecimiento del suelo de manera armónica, devolviendo el terreno a su color original.
