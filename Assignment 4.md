Got it! We are completely ditching the previous theory and shifting gears directly into implementation mode for **Assignment 3: Implementing a Deferred Renderer**.

Deferred rendering is all about solving the "overdraw" problem where you waste GPU cycles calculating complex lighting for pixels that end up blocked by other geometry anyway. Instead, we do it in two steps: save all the raw surface data to textures first, and then do one massive lighting calculation at the end using only the visible pixels.

Here is your step-by-step practical guide to implementing the code from the slides to hit the requirements for sections 2.1 through 2.3 of your assignment.

---

### Step 1: Setting up the G-Buffer (CPU Side)

Your first task is to create the Geometry Buffer (G-Buffer) FBO. This FBO won't render to the screen; it will render out data to textures.

**Where this goes:** In your initialization code (likely `renderer.cpp`).

C++

```
// Create the FBO object
GFX::FBO gbuffer_fbo;

// Configure it to match screen dimensions
gbuffer_fbo.create(SCREEN_WIDTH, 
                   SCREEN_HEIGHT, 
                   2,                 // Create 2 color textures (1 for Albedo, 1 for Normals)
                   GL_RGBA,           // 4 channels per texture (R, G, B, A)
                   GL_UNSIGNED_BYTE,  // 8 bits per channel is fine for now
                   true);             // VERY IMPORTANT: set to true so it stores a depth buffer
```

### Step 2: Filling the G-Buffer (CPU & GPU Side)

Now you need to render all your **opaque** geometry into this new FBO.

**CPU Code (`renderer.cpp`):**

You must wrap your standard opaque drawing loop inside the FBO bind/unbind methods.

C++

```
// 1. Bind your new FBO so OpenGL draws to it, not the screen
gbuffer_fbo.bind();

// 2. Clear the FBO data from the previous frame
glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

// 3. Render ONLY opaque objects (Transparencies are handled way later)
for (sDrawCall& call : opaque_draw_calls) {
    renderMeshWithMaterial(call...);
}

// 4. Unbind the FBO so you can use its textures later
gbuffer_fbo.unbind();
```

**Shader Code (`gbuffer_fill.fs`):**

Because your FBO has 2 color attachments, your fragment shader can no longer just output a single `FragColor`. You must explicitly tell the shader which data goes to which attachment using the `layout` qualifier.

OpenGL Shading Language

```
#version 330 core

// ... (your normal inputs and uniforms) ...

// REPLACE out vec4 FragColor with these:
layout(location = 0) out vec4 gbuffer_albedo;
layout(location = 1) out vec4 gbuffer_normal_mat;

void main() {
    // ... (calculate your base color and normals as usual) ...
    
    // Output Color
    gbuffer_albedo = vec4(base_color, 1.0);
    
    // Output Normal 
    // NOTE: Normals range from -1 to 1. Colors range from 0 to 1. 
    // You MUST pack the normal into the 0-1 range to store it in the texture!
    gbuffer_normal_mat = vec4(normal * 0.5 + 0.5, 1.0); 
}
```

### Step 3: The Lighting Pass (CPU Side)

Now that you have your G-Buffer filled with Albedo, Normal, and Depth textures, you render a single 2D quad that covers the whole screen and pass those textures to it.

**CPU Code (`renderer.cpp`):**

C++

```
// Get a 2D plane that perfectly aligns with the screen
GFX::Mesh *quad = GFX::Mesh::getQuad();

// Use your new deferred lighting shader (e.g., quad.vs and deferred_light.fs)
light_pass_shader->enable();

// Send your global light uniforms (positions, colors, etc.)
sendLightUniforms();

// Send an inverse screen resolution uniform (needed for the shader later!)
light_pass_shader->setUniform("u_res_inv", vec2(1.0f / SCREEN_WIDTH, 1.0f / SCREEN_HEIGHT));

// Bind the textures you generated in Step 2 into the shader
int texture_slots = 0;
light_pass_shader->setTexture("u_gbuffer_color", gbuffer_fbo.color_textures[0], texture_slots++);
light_pass_shader->setTexture("u_gbuffer_normal", gbuffer_fbo.color_textures[1], texture_slots++);
light_pass_shader->setTexture("u_gbuffer_depth", gbuffer_fbo.depth_texture, texture_slots++);

// Draw the quad!
quad->render(GL_TRIANGLES);

light_pass_shader->disable();
```

### Step 4: Shading & Reconstructing Position (GPU Side)

This is the hardest part. Since you are rendering a flat 2D quad, you don't actually know where any of the pixels are in 3D 🌍 world space. You **must** reconstruct their 3D position using the depth buffer data.

**Shader Code (`deferred_light.fs`):**

OpenGL Shading Language

```
#version 330 core
uniform sampler2D u_gbuffer_color;
uniform sampler2D u_gbuffer_normal;
uniform sampler2D u_gbuffer_depth;
uniform vec2 u_res_inv;       // Inverse screen size passed from CPU
uniform mat4 u_inv_vp_mat;    // Inverse View-Projection matrix of the camera!

out vec4 FragColor;

void main() {
    // 1. Calculate the UV coordinates of the current pixel on the screen
    vec2 uv = gl_FragCoord.xy * u_res_inv;
    
    // 2. Fetch the raw data from the G-Buffer textures
    vec3 albedo = texture(u_gbuffer_color, uv).rgb;
    
    // Unpack the normal back to the -1 to 1 range!
    vec3 normal = texture(u_gbuffer_normal, uv).rgb * 2.0 - 1.0; 
    
    // --- POSITION RECONSTRUCTION (CRITICAL) ---
    // Get the raw depth value (0.0 to 1.0)
    float depth = texture(u_gbuffer_depth, uv).r;
    
    // Convert depth and UVs into Clip Space (-1.0 to 1.0)
    float depth_clip = depth * 2.0 - 1.0;
    vec2 uv_clip = uv * 2.0 - 1.0;
    
    vec4 clip_coords = vec4(uv_clip.x, uv_clip.y, depth_clip, 1.0);
    
    // Multiply by inverse view-projection to get back to world coordinates
    vec4 not_norm_world_pos = u_inv_vp_mat * clip_coords;
    
    // Perspective division (divide by w) to get the final 3D world position
    vec3 world_pos = not_norm_world_pos.xyz / not_norm_world_pos.w;
    // ------------------------------------------
    
    // NOW you have `albedo`, `normal`, and `world_pos` for this specific pixel!
    // You can paste your standard Phong shading calculations right here.
    
    // Example: FragColor = vec4(CalculatePhong(albedo, normal, world_pos), 1.0);
}
```

### Checkpoint

If you implement steps 1 through 4 correctly, your scene should look exactly the same as your forward renderer, but without any translucent objects (windows, glass, etc.) rendering. Once this works, you can move on to Section 2.4 (Light Volumes)! Let me know when you are ready to tackle the Light FBOs and Additive Blending.

---

This is where deferred rendering gets really powerful—and a bit tricky with OpenGL states. You are moving from a brute-force "calculate every light for every pixel" approach to a highly optimized **Light Volume** approach.

Here is your practical, code-focused guide to implementing sections 2.4 (Light Volumes) and handling Transparencies based on these slides.

---

### Step 1: Preparing the Lighting FBO (CPU Side)

In single-pass deferred, you read from the G-Buffer and wrote directly to the screen. For Light Volumes, you need to draw 3D spheres (the lights) and use the G-Buffer's depth data to figure out exactly which pixels those spheres intersect.

Because OpenGL cannot read from and write to the same depth buffer simultaneously in a standard way, you must create a **second FBO** just for lighting.

**Where this goes:** In your initialization code, create a `lighting_fbo` identical in format to your G-Buffer's color textures (1 color attachment is enough).

**In your render loop:**

C++

```
// 1. You just finished filling the G-Buffer. Unbind it.
gbuffer_fbo.unbind();

// 2. Copy the depth texture from the G-Buffer FBO to the Lighting FBO
// This is critical! The light spheres need to know where the geometry is.
gbuffer_fbo.depth_texture->copyTo(lighting_fbo.depth_texture);

// 3. Bind the new Lighting FBO to start accumulating light
lighting_fbo.bind();
glClear(GL_COLOR_BUFFER_BIT); // Only clear color, KEEP the depth we just copied!
```

### Step 2: The Base Pass (Ambient & Directional)

Since light volumes use additive blending (adding colors on top of each other), you need a base layer first.

While `lighting_fbo` is bound, render a fullscreen quad exactly like you did in section 2.3, but **ONLY calculate Ambient Light and Directional Lights**. Do not calculate point or spot lights here. Render your Skybox in this step as well.

### Step 3: Configuring OpenGL for Light Volumes

Now we are going to draw actual 3D spheres representing point/spot lights. We need to tell OpenGL to add their colors together and only render the parts of the sphere that actually touch the geometry.

**CPU Code (Inside your render loop, after the base pass):**

C++

```
// --- SETUP OPENGL STATE FOR ADDITIVE LIGHT VOLUMES ---
glDepthFunc(GL_GREATER);       // Only render if the sphere is BEHIND or ON TOP of geometry
glDepthMask(GL_FALSE);         // DO NOT overwrite the depth buffer
glEnable(GL_BLEND);            // Turn on blending
glBlendFunc(GL_ONE, GL_ONE);   // Additive blending: Final = Light + Background
glFrontFace(GL_CW);            // Render BACK faces of the sphere only! (Prevents double lighting if camera is inside the sphere)

// Get the sphere mesh
GFX::Mesh* sphere = GFX::Mesh::Get("data/meshes/sphere.obj"); // Or however you get your sphere

// Enable your specific Light Volume shader
light_volume_shader->enable();
light_volume_shader->setUniform("u_res_inv", vec2(1.0f / SCREEN_WIDTH, 1.0f / SCREEN_HEIGHT));

// Bind G-buffer textures (Color, Normal, Depth) just like before
// ... (setTexture calls here) ...

// --- RENDER EACH LIGHT ---
for (int i = 0; i < scene_lights.size(); i++) {
    LightEntity* light = scene_lights[i];
    
    // Skip directional lights, we did those in the base pass
    if (light->type == LightType::DIRECTIONAL) continue; 

    // Send light properties to shader (color, intensity, position)
    sendLightUniformsToShader(light); 

    // Create the model matrix for the sphere
    Matrix44 model;
    model.setTranslation(light->position);
    // Scale the sphere so its radius equals the light's max_distance
    model.scale(light->max_distance, light->max_distance, light->max_distance); 
    
    light_volume_shader->setUniform("u_model", model);
    
    // Draw the sphere
    sphere->render(GL_TRIANGLES);
}

light_volume_shader->disable();

// --- RESTORE OPENGL STATE TO DEFAULTS ---
glDepthFunc(GL_LESS);
glDepthMask(GL_TRUE);
glDisable(GL_BLEND);
glFrontFace(GL_CCW);

// Unbind and display!
lighting_fbo.unbind();
lighting_fbo.color_textures[0]->toViewport();
```

### Step 4: The Light Volume Shader (GPU Side)

Your light volume fragment shader will be almost identical to the single-pass shader from section 2.3, with **one major difference**: you aren't drawing a fullscreen quad anymore, you are drawing a 3D sphere.

Because of this, you must use `basic.vs` as your vertex shader (so the sphere gets transformed correctly by the model/view/projection matrices).

In your fragment shader, you can't rely on standard vertex UVs anymore. You must calculate the screen UVs using `gl_FragCoord` to fetch the right pixel from the G-Buffer:

**Shader snippet (`light_volume.fs`):**

OpenGL Shading Language

```
// Calculate UV coordinates based on the pixel's position on the screen
vec2 uv = gl_FragCoord.xy * u_res_inv;

// Now fetch from G-buffers using this UV
vec3 albedo = texture(u_gbuffer_color, uv).rgb;
// ... (continue with normal unpacking, position reconstruction, and phong math) ...
```

---

### Step 5: Handling Transparencies

Deferred rendering fundamentally breaks standard transparency. A G-Buffer pixel can only store data for _one_ surface. If a glass window is in front of a wall, the G-Buffer only stores the glass, meaning the wall behind it gets zero lighting.

The slides offer two solutions:

#### Option A: The "Cheap" Deferred Way (Checkerboard Discard)

You fake transparency by throwing away half the pixels of the transparent object in a checkerboard pattern. The brain blends it together. This happens during the **G-Buffer fill pass**.

**In your `gbuffer_fill.fs`:**

OpenGL Shading Language

```
// If the material has alpha, discard pixels in a grid pattern
if (color.a < 0.9 && 
    floor(mod(gl_FragCoord.x, 2.0)) != floor(mod(gl_FragCoord.y, 2.0))) {
    discard; // Throw this pixel away, letting the object behind it render
}
```

#### Option B: The "Correct" Way (Forward Pass)

This is the recommended approach for games. You completely ignore transparent objects during the G-Buffer pass.

1. Fill G-Buffer (Opaque only).
    
2. Do Illumination Pass (Light Volumes).
    
3. **NEW STEP:** Turn on standard Forward Rendering (the pipeline from your previous lab) and render _only_ the transparent objects directly on top of the final, lit scene using standard alpha blending.
    

_(Note: To make Option B work perfectly, you will eventually need to copy the depth buffer from the G-Buffer into the main screen's depth buffer so your forward-rendered glass accurately intersects with the deferred-rendered opaque walls)._