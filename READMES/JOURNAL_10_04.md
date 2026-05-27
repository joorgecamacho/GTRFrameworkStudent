# Development Journal - Graphics Framework
**Fecha:** 10/04/2026

## Sesion:
Completar el **Assignment 1**, enfocado en asentar las bases del pipeline de renderizado ("The base rendering pipeline"). El objetivo principal ha sido separar la evaluación de la escena para recolectar información estructural, del proceso real de dibujar la imagen.

---

## Tareas Completadas y el porque de las cosas

### 3.1 & 3.2: Definir el "RenderCall" y Parsear la Escena

En `renderer.h` he creado un `struct sRenderable` y un `std::vector<sRenderable> render_list`.
En `renderer.cpp` he implementado una función recursiva `parseNode()` que se encarga de explorar todas las entidades tipo `PREFAB` extraídas de su estructura de árbol (los "nodos").

**¿Por qué?**
Iterar un árbol de nodos buscando la herencia y posiciones locales en el momento justo del *Render Phase* es ineficientemente lento.
Lo que hacemos es la fase de recolección (Parsing Phase) para "aplanar" todo el árbol: cogemos solo la malla, el material y computamos su matriz global final (dónde está en el mundo). Si tiene todo eso, lo guardamos en una lista plana (`render_list`) para que el renderizado se haga en un solo bucle continuado y contiguo en memoria.

### 3.4: Ordenar las Llamadas de Renderizado (Render Ordering)
**Lo que hice:** 
Tras recolectar la `render_list` (y antes de dibujarla), le aplicamos un `std::sort()` de C++ en la función `parseSceneEntities`.

**¿Por qué?**
Para dibujar bien tanto material sólido como material de tipo cristal, la GPU sufre sin un orden predefinido:
1. **Opacos (`MASK` o `NO_ALPHA`):** Se dibujan primero, ordenados desde **los más cercanos a la cámara hasta los más lejanos**. Esto nos regala rendimiento porque el Z-Buffer de la tarjeta gráfica detectará rápidamente si algo tapa a otra cosa, evitando colorear píxeles que terminarían ocultos (se evita el llamado *overdraw*).
2. **Transparentes (`BLEND`):** Se dibujan al final, ordenados exclusivamente de **los más lejanos a los más cercanos**. Esto es clave para el *"Z-fighting"*: como son objetos translúcidos, si pintamos el cristal delantero del coche antes que sus propios asientos, el rasterizador entenderá erróneamente que a través del cristal no debe dibujar lo posterior. Al hacerlo de la parte posterior a la delantera, todas las capas de cristal se acumulan (hacen *blending*) perfectamente sobre los opacos del mundo sin ocultar nada.

### 3.5: Frustum Culling (Extra)
**Lo que hice:** 
En la misma recolección `parseNode`, insertamos una condición: Extraemos la `BoundingBox` del modelo adaptándola a su Matriz Global, y le pedimos a la cámara que compruebe si está en su cono de visión o no con `Camera::current->testBoxInFrustum`. 

**¿Por qué?**
Si el resultado es `CLIP_OUTSIDE`, sabemos que el coche/casa/árbol está literalmente a espaldas de la cámara o muy desviado en sus laterales. Decidimos ignorar el registro y **no** hacer el `push_back()` a final de cuentas. Esto evita atibarrar la CPU y mandarle comandos (*Draw Calls*) a la GPU de datos que el usuario jamás iba a ver de todas formas en la pantalla, ahorrando miles de ciclos de procesamiento.

### 3.3: Renderizar la Escena
**Lo que hice:** 
En `renderScene` implementamos un único bucle secuencial *for* al final.

**¿Por qué?**
Gracias al *Parsing*, al *Frustum Culling* y al *Sorting* de los pasos anteriores... nuestra renderización termina resultando exageradamente simple y eficiente. Tan solo iteramos cíclicamente todo lo que ha logrado clasificarse en `render_list` invocando una a una la función `renderMeshWithMaterial`. El sistema fluye de manera autónoma en base a sus propiedades.
