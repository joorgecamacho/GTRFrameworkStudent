# Death Stranding Scanner: Visual Breakdown

The terrain scanner isn't just a simple expanding circle; it is a highly sophisticated composition of several layered visual elements that give it a sense of physical presence, volume, and data progression.

## 1. Directional 120° Wave Expansion

- **The Look:** Unlike a standard 360° radar, the scan propagates as a directional wedge or cone. It projects forward from the player's position at an approximate **120-degree angle**, matching the direction the character was facing when the scan initiated.
    
- **Visual Purpose:** This aligns the effect with the player's field of view and focal point, making it feel like a targeted tool rather than a generic environmental pulse.
    
Maybe we may skip this part and just make a full 360º radar that expands from the pointing camera point.

## 2. High-Contrast Leading Edge Gradient

- **The Look:** At the very front of the expanding wave, there is a distinct color gradient. It looks like a glowing, compressed band of light that actively sweeps over the terrain.
    
- **Visual Purpose:** It gives the illusion of physical friction—as if the scanner's energy wave is physically cutting through or rolling over the topography of the world.
    

## 3. The "First-Line" Visual Cue

- **The Look:** The absolute foremost scan line (the leading edge) is rendered in a **pure white color**, while all the subsequent tracking lines that follow behind it are a distinct **holographic blue**.
    
- **Visual Purpose:** This creates a powerful subconscious cue for the player's eyes, immediately highlighting the progression and direction of the wave front.
    

## 4. Adaptive Terrain Contouring (Perfect Outlines)

- **The Look:** The scan lines are pixel-perfect and sharp. They bend seamlessly around complex geometry, hugging every rock, cliff, and even individual tufts of grass without blurring or stretching.
    
- **Visual Purpose:** It behaves like a digital wrap, making the environment look like it is being actively digitized and mapped in real-time.
    

## 5. Ambient Ground Darkening

- **The Look:** The terrain directly underneath the active scanning zone is heavily darkened. This darkening follows the same 120° expansion shape.
    
- **Visual Purpose:** This is crucial for **readability**. By darkening the ground textures (rocks, grass, snow), the bright blue and white holographic lines gain massive contrast and "pop" regardless of whether the player is in a bright snowfield or a dark volcanic area.
    

## 6. The Initial Contracting Dark Circle

- **The Look:** The moment the player activates the scanner, a subtle dark circle rapidly contracts _inward_ toward the player before the main wave bursts _outward_.
    
- **Visual Purpose:** It visualizes the "charging" or "gathering" phase of the device, creating a brief moment of visual tension right before the energy release.

## 8. Incrementing scan line distance while distances from origin increments

- **The Look:** The farther we are from the origin point the larger are the scan lines from each other. This is not explained in the video but we have observed that.
    
- **Visual Purpose:** This makes that the scan lines at the end of the scanning wave have a nicer look.
--- 
# The implementation techniques used:

## 1. Pixel-Perfect Scan Lines (The Sobel Operator)

An initial intuitive approach might be to divide the scene depth buffer into linear segments to draw the lines. However, this causes severe aliasing and looks completely broken around complex geometries like foliage or grass.

To achieve crisp, perfectly defined lines that hug the terrain smoothly, the game leverages an **edge-detection algorithm** using the **Sobel Operator** in a full-screen shader.

- **How it works:** The shader samples the depth of a pixel and its four immediate neighbors. It calculates the depth delta (difference) between them. A high sum indicates a sharp transition—an edge—allowing the shader to draw clean outlines only at specific moving distance thresholds.
    

---

## 2. World-Space Distance Calculation

If the effect relied solely on the camera's raw depth buffer, the scan lines would warp, stretch, and move whenever the player rotates or moves the camera.

To make the effect feel anchored to the environment, it uses **World-Space Reconstruction**.

- **How it works:** Using the pixel's screen-space position and its depth value, the shader mathematically reconstructs the exact $XYZ$ **World Position** of that pixel. The distance is then calculated from the **scan origin** (the player's position at the moment of activation) rather than the camera. This ensures the lines stay perfectly static relative to the ground.
    

---

## 3. Visual Contrast Enhancement

A simple colored line would look detached from the world. _Death Stranding_ injects a high sense of physical interaction through two blending techniques:

- **The Edge Gradient:** A progressive color mask applied just ahead of the scan lines, simulating a wave pushing through the terrain and fighting friction.
    
- **Ground Darkening (Darken Blend):** The area directly underneath the expanding wave is dynamically darkened using a mathematical "minimum" blend node ($\min(\text{base color}, \text{blend color})$). This drastically increases visual contrast, making the holographic lines pop regardless of the underlying texture brightness.
    

---

## 4. Non-Linear Pacing and Animation Curve (The Juicy Feel)

The expansion velocity is not linear; it is highly stylized to give a sense of physical weight and energy buildup. The animation cycle is divided into three distinct phases controlled via a CPU script updating the shader material properties:

| **Phase**               | **Behavior**                                                                                | **Purpose**                                                                       |
| ----------------------- | ------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------- |
| **1. Spawn State**      | Fast initial speed, but low opacity. A dark circle contracts around the player.             | Simulates an energy "charge" or loading phase.                                    |
| **2. Pre-Expansion**    | Sudden deceleration and brief pause as opacity hits maximum.                                | Creates anticipation and a feeling of heavy buildup.                              |
| **3. Expansion & Fade** | High-velocity burst outward, followed by an exponential decay in both speed and visibility. | Delivers a powerful explosion of data that naturally dissolves into the distance. |

---
## Alternative Method: Geometric Mesh Expansion

Instead of analyzing the whole screen via post-processing, this technique takes a clever shortcut: it spawns a physical, growing **3D Sphere mesh** and highlights only the precise lines where that sphere intersects with the ground and environmental objects.

### 1. The Growing Sphere Approach (The Vehicle)

- **The Visual Element:** The core of this effect is a simple, invisible **3D Sphere** generated via Unity’s Particle System (or VFX Graph).
    
- **The Animation:** When the player presses the button, a single spherical particle is triggered. It rapidly scales up from a size of zero to hundreds of meters based on a **Size over Lifetime curve**.
    
- **Visual Purpose:** It naturally anchors the scan origin to the player's world position. By using a non-linear curve (fast at the beginning, slowing down towards the end), it perfectly mimics the organic energy burst seen in _Death Stranding_.
    

### 2. Scene Depth Intersection (The Contact Line)

- **The Visual Element:** The shader applied to this sphere doesn't render its solid walls. Instead, it only renders a bright glowing line exactly where the sphere's polygon faces pass through solid geometry (like mountains, rocks, or trees).
    
- **The Technique:** It uses a **Scene Depth Intersection Shader** (an Unlit, Transparent material with Two-Sided rendering turned on).
    
- **Visual Purpose:** By comparing the screen depth with the sphere’s actual geometry depth, it creates a clean, moving ring of light that effortlessly maps the topography of the scene in real-time.
    

### 3. Alpha Dissolve over Lifetime (The Fade Out)

- **The Visual Element:** As the wave reaches its maximum distance radius, the entire scan line smoothly loses its intensity and opacity until it completely disappears.
    
- **The Technique:** It utilizes the **Vertex Color node** inside Unity's Shader Graph to allow the Particle System's "Color over Lifetime" module to talk directly to the material's Alpha channel.
    
- **Visual Purpose:** This prevents the scanner from ending abruptly, giving it a natural, dissipating energy trail that feels smooth and alive.
    

---

## Key Differences (Comparison Slide)

| **Feature**        | **Method 1: Screen-Space Post-Processing**                       | **Method 2: Geometric Mesh Expansion**                                                           |
| ------------------ | ---------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| **Implementation** | Full-screen Blit Render Feature + Sobel Operator.                | Particle System + 3D Sphere Mesh + Intersection Shader.                                          |
| **Complexity**     | Higher (Requires world-space reconstruction and edge filtering). | Lower (Easier and faster to set up directly in Unity).                                           |
| **Performance**    | Performance depends on screen resolution (Pixel-bound).          | Extremely lightweight (Vertex-bound, only renders where intersections occur).                    |
| **Gameplay Bonus** | Purely visual.                                                   | Can easily trigger physical gameplay **Collisions** to tag enemies or items as the mesh expands. |
