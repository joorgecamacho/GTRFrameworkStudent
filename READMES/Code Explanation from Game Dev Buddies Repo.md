# Guía de Implementación Matemática: Escáner de Terreno (Post-Processing)

Para construir este shader en el framework, la función principal del **Fragment/Pixel Shader** debe procesar de manera secuencial la información espacial del píxel y aplicar operaciones de mezcla no lineales.

---

## 1. Reconstrucción Espacial de la Escena

Antes de aplicar cualquier máscara estética, el shader necesita saber dónde está el terreno tridimensionalmente con respecto al mundo, utilizando únicamente la textura de profundidad y las coordenadas UV de la pantalla.

High-level shader language

```
// Variables globales requeridas (pasadas por la CPU o la cámara)
float4x4 _InverseViewProjectionMatrix; // Matriz inversa para volver al mundo
Texture2D _SceneDepthTexture;          // Textura de profundidad de la escena
SamplerState sampler_SceneDepthTexture;

float3 GetWorldPosition(float2 screenUV)
{
    // 1. Muestrear la profundidad del buffer nativo de la escena
    float rawDepth = _SceneDepthTexture.Sample(sampler_SceneDepthTexture, screenUV).r;
    
    // 2. Normalizar a coordenadas de Clip Space (rango [-1, 1])
    float4 clipSpacePos = float4(screenUV * 2.0 - 1.0, rawDepth, 1.0);
    
    // 3. Multiplicar por la matriz inversa para obtener la posición en el mundo
    float4 worldSpacePos = mul(_InverseViewProjectionMatrix, clipSpacePos);
    
    // 4. Aplicar la división en perspectiva para la posición final (XYZ)
    return worldSpacePos.xyz / worldSpacePos.w;
}
```

---

## 2. Máscara Angular (Cono Direccional de 120°)

Para que el escáner sea proyectado en forma de abanico hacia adelante (siguiendo la orientación del jugador) y sus bordes laterales no terminen en un corte poligonal seco, aplicamos un decaimiento suavizado (`Smoothstep`).

High-level shader language

```
float3 _ScanOrigin;    // Posición XYZ del jugador al activar el escaneo
float3 _ScanForward;   // Vector forward de la orientación del jugador
float _ScanAngleCos;   // cos(60º) para limitar el abanico a 120º en total
float _EdgeFalloff;    // Ancho del suavizado lateral (ej. 0.05)

float CalculateAngleMask(float3 worldPos)
{
    // Vector unitario desde el jugador hacia el píxel del terreno
    float3 dirToPixel = normalize(worldPos - _ScanOrigin);
    
    // El producto escalar nos da el coseno del ángulo entre la dirección y el forward
    float dotResult = dot(dirToPixel, _ScanForward);
    
    // Determinamos un umbral interno donde empieza el desvanecimiento lateral
    float innerThreshold = _ScanAngleCos + _EdgeFalloff;
    
    // Retorna 1 en el centro, disminuye suavemente a 0 en los laterales del abanico
    return smoothstep(_ScanAngleCos, innerThreshold, dotResult);
}
```

---

## 3. Gradiente Delantero de Fricción (`Edge Gradient`)

Representa la onda expansiva de energía barriendo el mapa. Necesitamos limitar el efecto al radio de acción actual (`_CurrentRadius`) y generar un gradiente de caída hacia atrás para emular que choca contra el terreno.

High-level shader language

```
float _CurrentRadius;   // Radio actual animado por CPU
float _GradientWidth;   // Grosor de la banda de color trasera

float CalculateEdgeGradient(float3 worldPos)
{
    float dist = distance(worldPos, _ScanOrigin);
    
    // Generar rampa suave que va de 0 (atrás) a 1 (justo en el límite frontal)
    float gradient = smoothstep(_CurrentRadius - _GradientWidth, _CurrentRadius, dist);
    
    // Máscaras de corte binarias para que no pinte nada fuera del radio de acción
    float insideRadius = step(dist, _CurrentRadius);
    
    return gradient * insideRadius;
}
```

---

## 4. Generación de Líneas de Escaneo (Frecuencia e Intervalos)

En lugar de usar la profundidad nativa de la cámara (que deformaría las líneas al mover la vista), se usa la distancia real en el mundo calculada previamente. Usamos la función matemática `frac` para repetir las líneas de forma cíclica.

High-level shader language

```
float _LineInterval; // Distancia entre líneas (ej. cada 4 metros)
float _LineWidth;    // Grosor de cada línea en el mundo

float CalculateScanLines(float3 worldPos)
{
    float dist = distance(worldPos, _ScanOrigin);
    
    // frac() devuelve la parte decimal, simulando un bucle infinito de distancias
    float normalizedDist = frac(dist / _LineInterval) * _LineInterval;
    
    // Si la distancia fraccionada entra en el rango del grosor de la línea, se activa (1)
    return step(normalizedDist, _LineWidth);
}
```

---

## 5. Oscurecimiento del Fondo (`Darken Blend`)

Como se muestra en tus capturas de análisis de color, para que las rejillas e interfaces holográficas sean legibles sobre cualquier superficie (nieve, hierba brillante, rocas), el terreno se oscurece dinámicamente debajo del abanico usando la lógica matemática de un **Darken Blend** ($\min(\text{base}, \text{blend})$).

High-level shader language

```
float3 _DarkenColor; // Color oscuro base (ej. Gris muy oscuro o azul marino apagado)

float3 ApplyDarkenBlend(float3 sceneColor, float totalMask)
{
    // Evaluamos el valor mínimo componente a componente entre el suelo y el color de oscurecimiento
    float3 blendedColor = min(sceneColor, _DarkenColor);
    
    // Interpolamos el suelo original con el suelo oscurecido basándonos en la máscara del escáner
    return lerp(sceneColor, blendedColor, totalMask);
}
```

---

## 6. Integración del "Pipeline" en el Fragment Shader

Este es el orden lógico exacto que debes escribir dentro de tu función de sombreado para combinar todas las piezas anteriores en tu framework:

High-level shader language

```
// Entrada: UVs de pantalla y el Color original de la escena capturado previamente
float4 FragTerrainScan(float2 screenUV, float3 originalSceneColor) : SV_Target
{
    // Paso 1: Reconstrucción dimensional
    float3 worldPos = GetWorldPosition(screenUV);
    
    // Paso 2: Cálculo de máscaras
    float angleMask = CalculateAngleMask(worldPos);
    float edgeGradient = CalculateEdgeGradient(worldPos);
    float linesMask = CalculateScanLines(worldPos);
    
    // Combinar la onda expansiva (gradiente) con la forma del cono de 120º
    float finalScannerAreaMask = edgeGradient * angleMask;
    
    // Paso 3: Modificación del fondo (Oscurecimiento)
    // Pasamos un multiplicador del área para controlar la opacidad del fondo oscuro
    float3 workingColor = ApplyDarkenBlend(originalSceneColor, finalScannerAreaMask * 0.6);
    
    // Paso 4: Inyección de color holográfico (Aditivo)
    float3 holographicBlue = float3(0.0, 0.4, 1.0);
    float3 holographicWhite = float3(1.0, 1.0, 1.0);
    
    // Pintamos las líneas dentro del área activa
    float3 linesEmission = linesMask * holographicBlue * finalScannerAreaMask * 2.0;
    
    // Pintamos el gradiente frontal de fricción
    float3 edgeEmission = edgeGradient * holographicBlue * angleMask * 0.5;
    
    // Resultado Final: Fondo alterado + Emisión de los elementos del escáner
    float3 finalColor = workingColor + linesEmission + edgeEmission;
    
    return float4(finalColor, 1.0);
}
```

---
