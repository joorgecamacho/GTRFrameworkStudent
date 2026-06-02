//example of some shaders compiled
flat basic.vs flat.fs
texture basic.vs texture.fs
phong basic.vs phong.fs
skybox basic.vs skybox.fs
depth quad.vs depth.fs
depth_write quad.vs depth_write.fs
gbuffer basic.vs gbuffer.fs
deferred_global quad.vs deferred_global.fs
deferred_light basic.vs deferred_light.fs
ssao quad.vs ssao.fs
tonemap quad.vs tonemap.fs
scifi_scan quad.vs scifi_scan.fs
scanner_sphere basic.vs scanner_sphere.fs
\perturbNormal

// From https://github.com/glslify/glsl-perturb-normal/blob/master/cotangent-frame.glsl
mat3 cotangent_frame(vec3 N, vec3 p, vec2 uv)
{
	// get edge vectors of the pixel triangle
	vec3 dp1 = dFdx(p);
	vec3 dp2 = dFdy(p);
	vec2 duv1 = dFdx(uv);
	vec2 duv2 = dFdy(uv);

	// solve the linear system
	vec3 dp2perp = cross(dp2, N);
	vec3 dp1perp = cross(N, dp1);
	vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;

	// construct a scale-invariant frame 

	float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
	return mat3(T * invmax, B * invmax, N);
}

// assume N, the interpolated vertex normal and 
// WP the world position
vec3 perturbNormal(vec3 N, vec3 WP, vec2 uv, vec3 normal_pixel)
{
	mat3 TBN = cotangent_frame(N, WP, uv);
	return normalize(TBN * normal_pixel);
}

\PBR_functions

const float GAMMA = 2.2;

vec3 degamma(vec3 c)
{
	return pow(max(c, vec3(0.0)), vec3(GAMMA));
}

vec3 gamma(vec3 c)
{
	return pow(max(c, vec3(0.0)), vec3(1.0 / GAMMA));
}

const float PI = 3.14159265359;
const float EPSILON = 0.0001;

float saturate(float x)
{
	return clamp(x, 0.0, 1.0);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
	float clampedCosTheta = saturate(cosTheta);
	float oneMinusCosTheta = 1.0 - clampedCosTheta;
	float oneMinusCosTheta2 = oneMinusCosTheta * oneMinusCosTheta;
	float oneMinusCosTheta5 = oneMinusCosTheta2 * oneMinusCosTheta2 * oneMinusCosTheta;
	return F0 + (1.0 - F0) * oneMinusCosTheta5;
}

float distributionGGX(vec3 N, vec3 H, float roughness)
{
	float alpha = roughness * roughness;
	float alpha2 = alpha * alpha;
	float NdotH = saturate(dot(N, H));
	float NdotH2 = NdotH * NdotH;
	float d = NdotH2 * (alpha2 - 1.0) + 1.0;
	float denom = PI * d * d + EPSILON;
	return alpha2 / denom;
}

float geometrySchlickGGX(float NdotX, float roughness)
{
	float n = saturate(NdotX);
	float alpha = roughness * roughness;
	float k = alpha * 0.5;
	float denom = n * (1.0 - k) + k + EPSILON;
	return n / denom;
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
	float NdotV = saturate(dot(N, V));
	float NdotL = saturate(dot(N, L));
	float ggxV = geometrySchlickGGX(NdotV, roughness);
	float ggxL = geometrySchlickGGX(NdotL, roughness);
	return ggxV * ggxL;
}

vec3 cookTorranceDiffuseBRDF(vec3 albedo, float metalness)
{
	vec3 kd = (1.0 - metalness) * albedo;
	return kd / PI;
}

vec3 cookTorranceSpecularBRDF(vec3 N, vec3 V, vec3 L, vec3 albedo, float metalness, float roughness)
{
	vec3 F0 = mix(vec3(0.04), albedo, metalness);
	vec3 H = normalize(V + L);
	float NdotL = saturate(dot(N, L));
	float NdotV = saturate(dot(N, V));
	float VdotH = saturate(dot(V, H));
	vec3 F = fresnelSchlick(VdotH, F0);
	float D = distributionGGX(N, H, roughness);
	float G = geometrySmith(N, V, L, roughness);
	float denom = 4.0 * NdotL * NdotV + EPSILON;
	return (F * D * G) / denom;
}

\phong.fs

#version 330 core

#include "perturbNormal"
#include "PBR_functions"

// Varyings: datos que llegan del vertex shader (basic.vs)
in vec3 v_position;
in vec3 v_world_position;
in vec3 v_normal;
in vec2 v_uv;
in vec4 v_color;

// Uniforms del material (los mismos que texture.fs)
uniform vec4 u_color;
uniform sampler2D u_texture;
uniform float u_time;
uniform float u_alpha_cutoff;

// Uniforms de iluminaciÃ³n (nuevos)
uniform vec3 u_ambient_light;     // luz ambiental de la escena
uniform vec3 u_camera_position;   // posiciÃ³n de la cÃ¡mara (para specular)
uniform float u_shininess;        // alpha/shininess del material

uniform float u_shadow_bias;
uniform int u_has_shadow_map;

const int MAX_LIGHTS = 8;
uniform vec3 u_light_position[MAX_LIGHTS];
uniform vec3 u_light_colors[MAX_LIGHTS];
uniform int u_light_types[MAX_LIGHTS];       // 1=POINT, 2=SPOT, 3=DIRECTIONAL
uniform vec3 u_light_directions[MAX_LIGHTS]; // direcciÃ³n frontal de la luz
uniform vec2 u_light_cone_info[MAX_LIGHTS];  // x=alpha_min, y=alpha_max (en radianes)
uniform int u_num_lights;


uniform sampler2D u_shadow_maps[MAX_LIGHTS]; 
uniform mat4 u_shadow_vps[MAX_LIGHTS];

uniform sampler2D u_normal_texture;
uniform int u_has_normal_texture;
uniform int u_show_normals;
uniform sampler2D u_metallic_roughness_texture;
uniform int u_has_metallic_roughness_texture;
uniform float u_roughness_factor;
uniform float u_metallic_factor;

out vec4 FragColor;

// --- FUNCIÃ“N DE CÃLCULO DE SOMBRAS ---
float testShadow(vec3 world_pos, int index) {
    vec4 proj_pos = u_shadow_vps[index] * vec4(world_pos, 1.0);
    if (proj_pos.w <= 0.0) return 1.0;

    vec2 uv = (proj_pos.xy / proj_pos.w) * 0.5 + 0.5;
    float current_depth = (proj_pos.z / proj_pos.w) * 0.5 + 0.5;
    current_depth -= u_shadow_bias; 

    if(uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || current_depth > 1.0) return 1.0;

    // Selector de textura segÃºn el Ã­ndice de la luz
    float shadow_depth = 1.0;
    if (index == 0) shadow_depth = texture(u_shadow_maps[0], uv).x;
    else if (index == 1) shadow_depth = texture(u_shadow_maps[1], uv).x;
    else if (index == 2) shadow_depth = texture(u_shadow_maps[2], uv).x;
    else if (index == 3) shadow_depth = texture(u_shadow_maps[3], uv).x;
    else if (index == 4) shadow_depth = texture(u_shadow_maps[4], uv).x;
    else if (index == 5) shadow_depth = texture(u_shadow_maps[5], uv).x;
    else if (index == 6) shadow_depth = texture(u_shadow_maps[6], uv).x;
    else if (index == 7) shadow_depth = texture(u_shadow_maps[7], uv).x;

    return (current_depth > shadow_depth) ? 0.0 : 1.0;
}


// --- FUNCIÃ“N DE CÃLCULO DE LUZ INDIVIDUAL ---
// --- FUNCIÃ“N DE CÃLCULO DE LUZ INDIVIDUAL ---
vec3 computeLight(int index, vec3 world_pos, vec3 N, vec3 V, vec3 base_color, float roughness, float metalness) {
    vec3 L;
    float attenuation;

    if (u_light_types[index] == 1) {
        // === POINT LIGHT ===
        L = normalize(u_light_position[index] - world_pos);
        float dist = length(u_light_position[index] - world_pos);
        attenuation = 1.0 / (dist * dist);
    }
    else if (u_light_types[index] == 3) {
        // === DIRECTIONAL LIGHT ===
        L = normalize(-u_light_directions[index]);
        attenuation = 1.0;
    }
    else if (u_light_types[index] == 2) {
        // === SPOT LIGHT ===
        L = normalize(u_light_position[index] - world_pos);
        float dist = length(u_light_position[index] - world_pos);
        attenuation = 1.0 / (dist * dist);

        vec3 D = normalize(u_light_directions[index]);
        float cos_angle = dot(-L, D);
        float cos_alpha_max = cos(u_light_cone_info[index].y);
        float cos_alpha_min = cos(u_light_cone_info[index].x);

        if (cos_angle < cos_alpha_max) {
            attenuation = 0.0;
        } else {
            float spot_factor = clamp(
                (cos_angle - cos_alpha_max) / (cos_alpha_min - cos_alpha_max),
                0.0, 1.0
            );
            attenuation *= spot_factor;
        }
    }

	float shadow_factor = 1.0;
	if (u_has_shadow_map == 1 && u_light_types[index] != 1) {
		shadow_factor = testShadow(world_pos, index);
	}

    vec3 light_intensity = degamma(u_light_colors[index]) * attenuation * shadow_factor;
    float NdotL = saturate(dot(N, L));
    vec3 diffuse_brdf = cookTorranceDiffuseBRDF(base_color, metalness);
    vec3 specular_brdf = cookTorranceSpecularBRDF(N, V, L, base_color, metalness, roughness);
    return (diffuse_brdf + specular_brdf) * light_intensity * NdotL;
}

void main()
{
	// 1. Color base = material color * textura
	vec2 uv = v_uv;
	vec4 color = u_color;
	color *= texture(u_texture, uv);

	if(color.a < u_alpha_cutoff)
		discard;

	vec3 base_color = degamma(color.rgb);

	// 2. Normal Mapping
	vec3 N = normalize(v_normal);
	// Si u_show_normals == 1 aplicamos el Normal Map, si es 0 usamos la normal geomÃ©trica plana
	if (u_has_normal_texture == 1 && u_show_normals == 1) {
		// Leer textura de normales [0, 1]
		vec3 normal_pixel = texture(u_normal_texture, uv).xyz;
		// Mapear de [0, 1] a [-1, 1]
		normal_pixel = normal_pixel * 2.0 - 1.0;
		
		N = perturbNormal(N, v_world_position, uv, normal_pixel);
	}

	vec3 V = normalize(u_camera_position - v_world_position);
	float roughness = u_roughness_factor;
	float metalness = u_metallic_factor;
	if (u_has_metallic_roughness_texture == 1) {
		vec3 metallic_roughness = texture(u_metallic_roughness_texture, uv).rgb;
		roughness *= metallic_roughness.g;
		metalness *= metallic_roughness.b;
	}
	roughness = clamp(roughness, 0.0, 1.0);
	metalness = clamp(metalness, 0.0, 1.0);

	vec3 out_color = vec3(0.0);

	// Ambient: una sola vez
	out_color += degamma(u_ambient_light) * cookTorranceDiffuseBRDF(base_color, metalness);

	// Iterar luces
	for (int i = 0; i < MAX_LIGHTS; i++) {
		if (i < u_num_lights) {
			out_color += computeLight(i, v_world_position, N, V, base_color, roughness, metalness);
		}
	}

	// Output final (linear -> gamma para pantalla)
	FragColor = vec4(gamma(out_color), color.a);
}



\basic.vs

#version 330 core

in vec3 a_vertex;
in vec3 a_normal;
in vec2 a_coord;
in vec4 a_color;

uniform vec3 u_camera_pos;

uniform mat4 u_model;
uniform mat4 u_viewprojection;

//this will store the color for the pixel shader
out vec3 v_position;
out vec3 v_world_position;
out vec3 v_normal;
out vec2 v_uv;
out vec4 v_color;

uniform float u_time;

void main()
{	
	//calcule the normal in camera space (the NormalMatrix is like ViewMatrix but without traslation)
	v_normal = (u_model * vec4( a_normal, 0.0) ).xyz;
	
	//calcule the vertex in object space
	v_position = a_vertex;
	v_world_position = (u_model * vec4( v_position, 1.0) ).xyz;
	
	//store the color in the varying var to use it from the pixel shader
	v_color = a_color;

	//store the texture coordinates
	v_uv = a_coord;

	//calcule the position of the vertex using the matrices
	gl_Position = u_viewprojection * vec4( v_world_position, 1.0 );
}

\quad.vs

#version 330 core

in vec3 a_vertex;
in vec2 a_coord;
out vec2 v_uv;

void main()
{	
	v_uv = a_coord;
	gl_Position = vec4( a_vertex, 1.0 );
}


\flat.fs

#version 330 core

uniform vec4 u_color;

out vec4 FragColor;

void main()
{
	FragColor = u_color;
}


\texture.fs

#version 330 core

in vec3 v_position;
in vec3 v_world_position;
in vec3 v_normal;
in vec2 v_uv;
in vec4 v_color;

uniform vec4 u_color;
uniform sampler2D u_texture;
uniform float u_time;
uniform float u_alpha_cutoff;

out vec4 FragColor;

void main()
{
	vec2 uv = v_uv;
	vec4 color = u_color;
	color *= texture( u_texture, v_uv );

	if(color.a < u_alpha_cutoff)
		discard;

	FragColor = color;
}


\skybox.fs

#version 330 core

in vec3 v_position;
in vec3 v_world_position;

#include "PBR_functions"

uniform samplerCube u_texture;
uniform vec3 u_camera_position;
uniform int u_apply_gamma;

// Salidas mÃºltiples
layout(location = 0) out vec4 gbuffer_albedo;
layout(location = 1) out vec4 gbuffer_normal;

void main()
{
	vec3 E = v_world_position - u_camera_position;
	vec4 color = texture( u_texture, E );
	vec3 rgb = color.rgb;
	if (u_apply_gamma == 1)
		rgb = gamma(rgb);

    gbuffer_albedo = vec4(rgb, color.a);
    // El skybox no tiene normales reales que afecten a la luz, 
    // pero debemos escribir algo en el canal de normales para que la textura no quede con basura.
    gbuffer_normal = vec4(0.5, 0.5, 0.5, 1.0); // Una normal "nula" en rango empaquetado 0-1
}


\depth.fs

#version 330 core

uniform vec2 u_camera_nearfar;
uniform sampler2D u_texture; //depth map
in vec2 v_uv;
out vec4 FragColor;

void main()
{
	float n = u_camera_nearfar.x;
	float f = u_camera_nearfar.y;
	float z = texture(u_texture,v_uv).x;
	if( n == 0.0 && f == 1.0 )
		FragColor = vec4(z);
	else
		FragColor = vec4( n * (z + 1.0) / (f + n - z * (f - n)) );
}


\instanced.vs

#version 330 core

in vec3 a_vertex;
in vec3 a_normal;
in vec2 a_coord;

in mat4 u_model;

uniform vec3 u_camera_pos;

uniform mat4 u_viewprojection;

//this will store the color for the pixel shader
out vec3 v_position;
out vec3 v_world_position;
out vec3 v_normal;
out vec2 v_uv;

void main()
{	
	//calcule the normal in camera space (the NormalMatrix is like ViewMatrix but without traslation)
	v_normal = (u_model * vec4( a_normal, 0.0) ).xyz;
	
	//calcule the vertex in object space
	v_position = a_vertex;
	v_world_position = (u_model * vec4( a_vertex, 1.0) ).xyz;
	
	//store the texture coordinates
	v_uv = a_coord;

	//calcule the position of the vertex using the matrices
	gl_Position = u_viewprojection * vec4( v_world_position, 1.0 );
}

\gbuffer.fs
#version 330 core

#include "perturbNormal"
#include "PBR_functions"

in vec3 v_position;
in vec3 v_world_position;
in vec3 v_normal;
in vec2 v_uv;
in vec4 v_color;

uniform vec4 u_color;
uniform sampler2D u_texture;
uniform float u_alpha_cutoff;

uniform sampler2D u_normal_texture;
uniform int u_has_normal_texture;
uniform int u_show_normals;
uniform sampler2D u_metallic_roughness_texture;
uniform int u_has_metallic_roughness_texture;
uniform float u_roughness_factor;
uniform float u_metallic_factor;

// TAREA 2.2: Declarar las salidas a mÃºltiples texturas[cite: 1]
layout(location = 0) out vec4 gbuffer_albedo;
layout(location = 1) out vec4 gbuffer_normal;

void main()
{
    // 1. Color base (perceptual -> linear para iluminación)
    vec4 color = u_color * texture(u_texture, v_uv);
    color.rgb = degamma(color.rgb);

    // Alpha masking para materiales tipo MASK (hojas, rejas, etc.)
    if(color.a < u_alpha_cutoff) {
        discard;
    }

    // 2. Normal Mapping (Igual que en tu phong.fs)
    vec3 N = normalize(v_normal);
    if (u_has_normal_texture == 1 && u_show_normals == 1) {
        vec3 normal_pixel = texture(u_normal_texture, v_uv).xyz;
        normal_pixel = normal_pixel * 2.0 - 1.0;
        N = perturbNormal(N, v_world_position, v_uv, normal_pixel);
    }

    float roughness = u_roughness_factor;
    float metalness = u_metallic_factor;
    if (u_has_metallic_roughness_texture == 1) {
        vec3 metallic_roughness = texture(u_metallic_roughness_texture, v_uv).rgb;
        roughness *= metallic_roughness.g;
        metalness *= metallic_roughness.b;
    }
    roughness = clamp(roughness, 0.0, 1.0);
    metalness = clamp(metalness, 0.0, 1.0);
    // 3. Escribir en el G-Buffer
    gbuffer_albedo = vec4(color.rgb, roughness);
    // IMPORTANTE: Las normales van de -1 a 1, pero la textura guarda valores de 0 a 1.[cite: 1]
    // Hay que empaquetarlas:
    gbuffer_normal = vec4(N * 0.5 + 0.5, metalness);
}

\scanner_sphere.fs

#version 330 core

// T5.1 METHOD A — Expanding sphere mesh + Depth-Intersection shader.
// The bright line is drawn ONLY where the sphere polygons cross the scene
// geometry (the contact ring), reading the scene depth from the G-Buffer.
in vec3 v_world_position;
in vec3 v_normal;

uniform sampler2D u_depth_texture;      // scene depth (G-Buffer)
uniform mat4 u_inverse_viewprojection;  // rebuild scene world pos
uniform vec2 u_iRes;                     // 1 / screen size
uniform vec3 u_camera_position;

uniform vec3 u_scanner_color;
uniform float u_scanner_intensity;
uniform float u_contact_thickness;       // ring thickness (world meters)
uniform float u_rim_power;               // faint shell so the bubble is visible
uniform float u_rim_strength;            // how much shell to add (0 on trailing rings)
uniform float u_sphere_alpha;

out vec4 FragColor;

void main()
{
    // 1. Where is this sphere fragment on screen?
    vec2 uv = gl_FragCoord.xy * u_iRes;

    // 2. Read the scene depth behind this fragment and rebuild its world pos
    float scene_depth = texture(u_depth_texture, uv).x;
    vec4 ndc = vec4(uv * 2.0 - 1.0, scene_depth * 2.0 - 1.0, 1.0);
    vec4 wp = u_inverse_viewprojection * ndc;
    vec3 scene_world = wp.xyz / wp.w;

    // 3. INTERSECTION: bright where the sphere surface meets real geometry
    float d = distance(scene_world, v_world_position);
    float contact = 1.0 - smoothstep(0.0, u_contact_thickness, d);
    contact *= step(scene_depth, 0.9999); // ignore the sky
    contact = pow(contact, 1.5);           // tighten into a crisp line

    // 4. Faint Fresnel shell so the growing bubble is visible in the air
    vec3 N = normalize(v_normal);
    vec3 V = normalize(u_camera_position - v_world_position);
    float rim = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), u_rim_power) * u_rim_strength;

    // Visible volumetric shell: rim glow + faint inner fill so the dome reads
    float shell = max(rim, u_rim_strength * 0.25);
    float mask = max(contact, shell);
    if (mask < 0.003)
        discard;

    // White-hot core fading to cyan, plus a glowing energy shell
    vec3 core_col = mix(u_scanner_color, vec3(1.0), contact * 0.6);
    vec3 color = core_col * contact * u_scanner_intensity + u_scanner_color * shell * 1.5;

    FragColor = vec4(color, mask * u_sphere_alpha);
}

\deferred_global.fs

#version 330 core

in vec2 v_uv;

#include "PBR_functions"

uniform sampler2D u_albedo_texture;
uniform sampler2D u_normal_texture;
uniform sampler2D u_depth_texture;

uniform vec3 u_ambient_light;
uniform vec3 u_camera_position;
uniform mat4 u_inverse_viewprojection;
uniform vec2 u_iRes; // 1.0 / screen size

// Directional lights
const int MAX_LIGHTS = 8;
uniform int u_num_dir_lights;
uniform vec3 u_dir_light_color[MAX_LIGHTS];
uniform vec3 u_dir_light_direction[MAX_LIGHTS];
uniform float u_shininess;

// Shadow map for first directional light
uniform int u_has_shadow_map;
uniform float u_shadow_bias;
uniform sampler2D u_shadow_map;
uniform mat4 u_shadow_vp;

// SSAO
uniform sampler2D u_ssao_texture;
uniform int u_enable_ssao;

out vec4 FragColor;

float testShadow(vec3 world_pos) {
    vec4 proj_pos = u_shadow_vp * vec4(world_pos, 1.0);
    if (proj_pos.w <= 0.0) return 1.0;
    vec2 uv = (proj_pos.xy / proj_pos.w) * 0.5 + 0.5;
    float current_depth = (proj_pos.z / proj_pos.w) * 0.5 + 0.5;
    current_depth -= u_shadow_bias;
    if(uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || current_depth > 1.0) return 1.0;
    float shadow_depth = texture(u_shadow_map, uv).x;
    return (current_depth > shadow_depth) ? 0.0 : 1.0;
}

void main()
{
    vec2 uv = gl_FragCoord.xy * u_iRes;
    
    // Read G-Buffer
    vec4 albedo_data = texture(u_albedo_texture, uv);
    vec4 normal_data = texture(u_normal_texture, uv);
    vec3 albedo = albedo_data.rgb;
    vec3 N = normal_data.rgb * 2.0 - 1.0;
    float roughness = clamp(albedo_data.a, 0.0, 1.0);
    float metalness = clamp(normal_data.a, 0.0, 1.0);
    N = normalize(N);
    float depth = texture(u_depth_texture, uv).x;
    
    // If depth==1.0, it is skybox, just output albedo (skybox color)
    if (depth >= 0.9999) {
        FragColor = vec4(albedo, 1.0);
        return;
    }
    
    // Reconstruct world position from depth
    vec2 screen_uv = uv * 2.0 - 1.0;
    vec4 screen_pos = vec4(screen_uv, depth * 2.0 - 1.0, 1.0);
    vec4 world_pos4 = u_inverse_viewprojection * screen_pos;
    vec3 world_pos = world_pos4.xyz / world_pos4.w;
    
    vec3 V = normalize(u_camera_position - world_pos);
    
    // Leer SSAO: si está activado, atenúa la luz ambiental. Si no, factor = 1.0 (sin efecto)
    float ao_factor = 1.0;
    if (u_enable_ssao == 1) {
        ao_factor = texture(u_ssao_texture, uv).r;
    }
    
    // REGLA CLAVE: Solo la luz ambiental se multiplica por el SSAO
    vec3 out_color = degamma(u_ambient_light) * cookTorranceDiffuseBRDF(albedo, metalness) * ao_factor;
    
    // Add directional lights
    for (int i = 0; i < MAX_LIGHTS; i++) {
        if (i >= u_num_dir_lights) break;
        
        vec3 L = normalize(-u_dir_light_direction[i]);
        vec3 light_color = degamma(u_dir_light_color[i]);
        
        // Shadow
        float shadow = 1.0;
        if (u_has_shadow_map == 1 && i == 0) {
            shadow = testShadow(world_pos);
        }
        
        float NdotL = saturate(dot(N, L));
        vec3 diffuse_brdf = cookTorranceDiffuseBRDF(albedo, metalness);
        vec3 specular_brdf = cookTorranceSpecularBRDF(N, V, L, albedo, metalness, roughness);
        out_color += (diffuse_brdf + specular_brdf) * light_color * shadow * NdotL;
    }

    FragColor = vec4(out_color, 1.0);
}


\deferred_light.fs

#version 330 core

in vec3 v_position;
in vec3 v_world_position;
in vec3 v_normal;
in vec2 v_uv;

#include "PBR_functions"

uniform sampler2D u_albedo_texture;
uniform sampler2D u_normal_texture;
uniform sampler2D u_depth_texture;

uniform vec3 u_camera_position;
uniform mat4 u_inverse_viewprojection;
uniform vec2 u_iRes;

// Single light data
uniform vec3 u_light_position;
uniform vec3 u_light_color;
uniform int u_light_type; // 1=POINT, 2=SPOT
uniform vec3 u_light_direction;
uniform vec2 u_light_cone_info;
uniform float u_max_distance;
uniform float u_shininess;

// Shadow
uniform int u_has_shadow_map;
uniform float u_shadow_bias;
uniform sampler2D u_shadow_map;
uniform mat4 u_shadow_vp;

out vec4 FragColor;

void main()
{
    // Calculate screen UV from fragment position
    vec2 uv = gl_FragCoord.xy * u_iRes;
    
    // Read G-Buffer
    vec4 albedo_data = texture(u_albedo_texture, uv);
    vec4 normal_data = texture(u_normal_texture, uv);
    vec3 albedo = albedo_data.rgb;
    vec3 N = normal_data.rgb * 2.0 - 1.0;
    float roughness = clamp(albedo_data.a, 0.0, 1.0);
    float metalness = clamp(normal_data.a, 0.0, 1.0);
    N = normalize(N);
    float depth = texture(u_depth_texture, uv).x;
    
    // If depth==1.0, skybox, no lighting
    if (depth >= 1.0) discard;
    
    // Reconstruct world position from depth
    vec2 screen_uv = uv * 2.0 - 1.0;
    vec4 screen_pos = vec4(screen_uv, depth * 2.0 - 1.0, 1.0);
    vec4 world_pos4 = u_inverse_viewprojection * screen_pos;
    vec3 world_pos = world_pos4.xyz / world_pos4.w;
    
    vec3 V = normalize(u_camera_position - world_pos);
    
    // Compute light
    vec3 L;
    float attenuation;
    
    if (u_light_type == 1) {
        // POINT
        L = normalize(u_light_position - world_pos);
        float dist = length(u_light_position - world_pos);
        if (dist > u_max_distance) discard;
        attenuation = 1.0 / (dist * dist);
    }
    else if (u_light_type == 2) {
        // SPOT
        L = normalize(u_light_position - world_pos);
        float dist = length(u_light_position - world_pos);
        if (dist > u_max_distance) discard;
        attenuation = 1.0 / (dist * dist);
        
        vec3 D = normalize(u_light_direction);
        float cos_angle = dot(-L, D);
        float cos_alpha_max = cos(u_light_cone_info.y);
        float cos_alpha_min = cos(u_light_cone_info.x);
        
        if (cos_angle < cos_alpha_max) {
            discard;
        } else {
            float spot_factor = clamp(
                (cos_angle - cos_alpha_max) / (cos_alpha_min - cos_alpha_max),
                0.0, 1.0
            );
            attenuation *= spot_factor;
        }
    }
    else {
        discard;
    }
    
    // Shadow
    float shadow = 1.0;
    if (u_has_shadow_map == 1 && u_light_type != 1) {
        vec4 proj_pos = u_shadow_vp * vec4(world_pos, 1.0);
        if (proj_pos.w > 0.0) {
            vec2 shadow_uv = (proj_pos.xy / proj_pos.w) * 0.5 + 0.5;
            float current_depth = (proj_pos.z / proj_pos.w) * 0.5 + 0.5;
            current_depth -= u_shadow_bias;
            if (shadow_uv.x >= 0.0 && shadow_uv.x <= 1.0 && shadow_uv.y >= 0.0 && shadow_uv.y <= 1.0 && current_depth <= 1.0) {
                float shadow_depth = texture(u_shadow_map, shadow_uv).x;
                shadow = (current_depth > shadow_depth) ? 0.0 : 1.0;
            }
        }
    }
    
    vec3 light_intensity = degamma(u_light_color) * attenuation * shadow;
    
    vec3 result = vec3(0.0);
    float NdotL = saturate(dot(N, L));
    vec3 diffuse_brdf = cookTorranceDiffuseBRDF(albedo, metalness);
    vec3 specular_brdf = cookTorranceSpecularBRDF(N, V, L, albedo, metalness, roughness);
    result += (diffuse_brdf + specular_brdf) * light_intensity * NdotL;
    
    FragColor = vec4(result, 1.0);
}

\tonemap.fs

#version 330 core

in vec2 v_uv;

#include "PBR_functions"

uniform sampler2D u_texture;
uniform float u_exposure;

out vec4 FragColor;

//tonemapper Uncharted 2 (Naughty Dog)
const float TONEMAP_A = 0.15;
const float TONEMAP_B = 0.50;
const float TONEMAP_C = 0.10;
const float TONEMAP_D = 0.20;
const float TONEMAP_E = 0.02;
const float TONEMAP_F = 0.30;
const float TONEMAP_WHITE = 11.2;

vec3 Uncharted2TonemapPartial(vec3 x)
{
	return ((x * (TONEMAP_A * x + TONEMAP_C * TONEMAP_B) + TONEMAP_D * TONEMAP_E)
		/ (x * (TONEMAP_A * x + TONEMAP_B) + TONEMAP_D * TONEMAP_F)) - TONEMAP_E / TONEMAP_F;
}

void main()
{
	vec3 hdr_color = texture(u_texture, v_uv).rgb;
	vec3 mapped = Uncharted2TonemapPartial(hdr_color * u_exposure);
	vec3 white_scale = vec3(1.0) / Uncharted2TonemapPartial(vec3(TONEMAP_WHITE));
	vec3 tonemapped = mapped * white_scale;
	FragColor = vec4(gamma(tonemapped), 1.0);
}

\depth_write.fs

#version 330 core

uniform sampler2D u_depth_texture;
in vec2 v_uv;
out vec4 FragColor;

void main() {
    gl_FragDepth = texture(u_depth_texture, v_uv).x;
    FragColor = vec4(0.0);
}

\ssao.fs

#version 330 core

in vec2 v_uv;

// --- Uniforms enviados desde la CPU (Fase 2) ---
uniform sampler2D u_depth_texture;   // Profundidad del G-Buffer
uniform sampler2D u_normal_texture;  // Normales del G-Buffer (empaquetadas 0-1)

uniform mat4 u_p_mat;       // Projection Matrix
uniform mat4 u_inv_p_mat;   // Inverse Projection Matrix
uniform mat4 u_view_mat;    // View Matrix (para normales World→View)

uniform int u_sample_count;          // Nº de muestras
uniform float u_sample_radius;       // Radio de búsqueda
uniform vec2 u_res_inv;              // 1.0 / resolución del FBO
uniform vec3 u_sample_pos[64];       // Puntos de muestreo precalculados (máx 64)

out vec4 FragColor;

void main()
{
    // =============================================
    // PASO 1: Reconstruir la posición 3D en View Space
    // =============================================

    // Centrar la UV en el medio del píxel (evitar artefactos de precisión)
    vec2 uv = v_uv + 0.5 * u_res_inv;

    // Leer profundidad del G-Buffer (rango 0-1)
    float depth = texture(u_depth_texture, uv).r;

    // Early exit: si depth == 1.0, es el cielo (no hay geometría)
    if (depth >= 1.0) {
        FragColor = vec4(1.0); // Blanco = sin oclusión
        return;
    }

    // Transformar UV + depth a Clip Space (NDC): de [0,1] a [-1,1]
    vec4 clip_coords = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);

    // Invertir la proyección: Clip Space → View Space
    vec4 view_pos = u_inv_p_mat * clip_coords;
    view_pos /= view_pos.w;  // División de perspectiva

    // =============================================
    // PASO 2: Preparar la Normal y la Matriz TBN
    // =============================================

    // Leer normal del G-Buffer (empaquetada en 0-1) y desempaquetar a -1..1
    vec3 N = texture(u_normal_texture, uv).rgb * 2.0 - 1.0;
    N = normalize(N);

    // Las normales del G-Buffer están en World Space.
    // Multiplicamos por View Matrix (w=0.0 para ignorar traslación) → View Space
    N = normalize((u_view_mat * vec4(N, 0.0)).xyz);

    // Construir la matriz TBN para orientar el hemisferio hacia la normal
    // Usamos un vector pseudo-aleatorio fijo (se puede mejorar con una textura de ruido)
    vec3 v_rand = vec3(0.0, 1.0, 0.0);

    // Gram-Schmidt: calcular Tangente ortogonal a N
    vec3 T = normalize(v_rand - N * dot(v_rand, N));
    vec3 B = cross(N, T);

    // La matriz que rota nuestro hemisferio "hacia arriba" para que apunte hacia N
    mat3 rotmat = mat3(T, B, N);

    // =============================================
    // PASO 3: Evaluar las muestras (bucle principal)
    // =============================================

    float ao_term = 0.0;

    for (int i = 0; i < u_sample_count; i++) {

        // a) Rotar la muestra con la TBN (alinear hemisferio a la superficie)
        vec3 view_sample = rotmat * u_sample_pos[i];

        // b) Escalar por el radio y mover al punto de origen
        view_sample = view_sample * u_sample_radius + view_pos.xyz;

        // c) Proyectar la muestra 3D de vuelta a 2D (View Space → Clip Space)
        vec4 proj_sample = u_p_mat * vec4(view_sample, 1.0);
        proj_sample /= proj_sample.w;  // División de perspectiva

        // d) Pasar de Clip Space [-1,1] a coordenadas UV [0,1]
        vec2 sample_uv = proj_sample.xy * 0.5 + 0.5;

        // e) Leer la profundidad real de la escena en esa posición
        float sample_depth = texture(u_depth_texture, sample_uv).r;

        // f) Reconstruir la Z real de la escena en View Space para comparar
        //    Convertimos sample_depth (0-1) a clip Z (-1..1), luego a View Space
        vec4 real_clip = vec4(0.0, 0.0, sample_depth * 2.0 - 1.0, 1.0);
        vec4 real_view = u_inv_p_mat * real_clip;
        real_view /= real_view.w;

        // g) Comparar: si la geometría real está MÁS CERCA que nuestra muestra,
        //    la muestra está ocluida (enterrada dentro de la geometría)
        //    En View Space de OpenGL, los objetos más cercanos tienen Z más grande (menos negativo)
        if (real_view.z > view_sample.z) {
            ao_term += 1.0;
        }
    }

    // Promediar: ao_term ahora contiene la fracción de muestras NO ocluidas
    ao_term = 1.0 - (ao_term / float(u_sample_count));

    FragColor = vec4(vec3(ao_term), 1.0);
}

// =============================================================================
// EFECTO SCI-FI SCAN — SHADER DE POST-PROCESADO
// Implementación capa por capa inspirada en el scanner de Death Stranding.
//
// CAPA 1: Reconstrucción de posición en World Space
//   → Lee el depth buffer para reconstruir la posición XYZ real de cada píxel.
//   → Sin esto, el efecto se deformaría al mover la cámara (efecto STATIC).
// CAPA 2: Oscurecimiento del terreno (Darken Blend)
//   → Dentro del radio del scanner, aplica min(sceneColor, darkenColor).
//   → Hace que cualquier línea holográfica destaque sobre CUALQUIER superficie.
// CAPA 3: Frente de onda (Edge Gradient)
//   → Banda brillante en el borde frontal del radio expansivo.
//   → Simula la "fricción" de la energía barriendo el terreno.
// CAPA 4: Líneas holográficas (frac sobre distancia 3D)
//   → frac() sobre la distancia real al mundo genera líneas ancladas al terreno.
//   → Se mantienen estáticas aunque la cámara se mueva (efecto CRISP).
// CAPA 5: Primera línea blanca (Visual Cue)
//   → El anillo exterior más cercano al frente se pinta en blanco puro.
//   → El ojo humano lo detecta instantáneamente como el "frente de avance".
// =============================================================================
\scifi_scan.fs

#version 330 core

in vec2 v_uv;

// ---- Textura de profundidad del G-Buffer (generada en el pase de gbuffer) ----
uniform sampler2D u_depth_texture;

// ---- Textura de color de la escena ya iluminada (resultado del tonemap) ----
uniform sampler2D u_scene_color_texture;

// ---- Matriz inversa de ViewProjection: transforma Clip Space → World Space ----
uniform mat4 u_inverse_viewprojection;

// ---- Resolución inversa de la pantalla (1/width, 1/height) ----
uniform vec2 u_iRes;

// ---- Parámetros del escáner ----
uniform vec3  u_scan_origin;      // Posición XYZ del jugador al activar el scan
uniform float u_scan_radius;      // Radio actual (animado desde la CPU)
uniform float u_scan_active;      // 1.0 = activo, 0.0 = inactivo

// ---- CAPA 2: Oscurecimiento del terreno ----
uniform vec3  u_darken_color;     // Color de mezcla oscuro (ej. azul marino muy oscuro)
uniform float u_darken_strength;  // Opacidad del oscurecimiento [0..1]

// ---- CAPA 3: Frente de onda ----
uniform float u_edge_width;       // Grosor de la banda de brillo frontal (en metros)
uniform vec3  u_edge_color;       // Color de la onda (azul holográfico brillante)
uniform float u_edge_intensity;   // Brillo de la onda

// ---- CAPA 4: Líneas holográficas ----
uniform float u_line_interval;    // Distancia entre líneas (metros)
uniform float u_line_width;       // Grosor de cada línea (metros)
uniform vec3  u_line_color;       // Color de las líneas interiores (azul holográfico)
uniform float u_line_intensity;   // Brillo de las líneas

// ---- CAPA 5: Primera línea blanca (Visual Cue) ----
uniform float u_leading_intensity; // Brillo extra de la línea blanca delantera

// ---- PASO 6: Animación (controlada desde la CPU) ----
uniform float u_scan_opacity;    // Opacidad global del efecto [0..1] (fade in/out)
uniform float u_charge_radius;   // Radio del círculo oscuro de carga que se contrae
uniform float u_trail_width;     // Ancho del rastro en metros (desvanecimiento posterior)

out vec4 FragColor;

// =============================================================================
// FUNCIÓN: Reconstrucción de World Position desde el depth buffer
// Convierte screen UV + raw depth → posición 3D en World Space
// =============================================================================
vec3 GetWorldPosition(vec2 screenUV, float rawDepth)
{
    // 1. Mapear de [0,1] a [-1,1] para obtener coordenadas NDC (Normalized Device Coords)
    vec2 ndc_xy = screenUV * 2.0 - 1.0;

    // 2. El depth raw del buffer también va de 0 a 1; mapeamos a [-1, 1] para Clip Space
    float ndc_z = rawDepth * 2.0 - 1.0;

    // 3. Construir el punto en Clip Space homogéneo (w = 1 antes de dividir)
    vec4 clipSpacePos = vec4(ndc_xy, ndc_z, 1.0);

    // 4. Multiplicar por la matriz inversa de ViewProjection → World Space
    vec4 worldSpacePos = u_inverse_viewprojection * clipSpacePos;

    // 5. División de perspectiva: obtener las coordenadas XYZ reales
    return worldSpacePos.xyz / worldSpacePos.w;
}

// =============================================================================
// FUNCIÓN: Darken Blend
// Oscurece el color base usando la fórmula del modo "Oscurecer" de Photoshop.
// Toma el mínimo de cada canal RGB → garantiza contraste sobre cualquier textura.
// =============================================================================
vec3 ApplyDarkenBlend(vec3 sceneColor, vec3 darkenColor, float mask)
{
    // min() componente a componente = el canal más oscuro siempre gana
    vec3 blended = min(sceneColor, darkenColor);
    // lerp: mezcla suave según la máscara del scanner y la fuerza del efecto
    return mix(sceneColor, blended, mask);
}

// =============================================================================
// FUNCIÓN: Frente de onda (Edge Gradient)
// Genera una banda brillante justo en el borde exterior del radio activo.
// Usa smoothstep para crear una rampa suave de 0 (atrás) a 1 (borde frontal).
// =============================================================================
float CalculateEdgeGradient(float dist)
{
    // smoothstep(edge0, edge1, x): rampa suave de 0 a 1 entre edge0 y edge1
    // Aquí: la rampa va de 0 (al comienzo de la banda) a 1 (en el borde frontal)
    float gradient = smoothstep(u_scan_radius - u_edge_width, u_scan_radius, dist);

    // step(): máscara binaria que elimina cualquier cosa fuera del radio
    // Esto convierte la rampa en un "diente de sierra": sube suave, corta seco
    float insideRadius = step(dist, u_scan_radius);

    return gradient * insideRadius;
}

// =============================================================================
// FUNCIÓN: Líneas holográficas (Scan Lines)
// Usa frac() para generar líneas concéntricas ancladas al World Space.
// Al usar la DISTANCIA REAL AL MUNDO (no la profundidad de cámara), las líneas
// se mantienen completamente ESTÁTICAS aunque la cámara se mueva o rote.
// El intervalo crece con la distancia para dar separación progresiva (efecto WEIGHT).
// =============================================================================
float CalculateScanLines(float dist)
{
    // El intervalo entre líneas crece un 3% por cada metro de distancia.
    // Con dist=0 el intervalo es u_line_interval, con dist=50m es 2.5x mayor.
    float dynamicInterval = u_line_interval * (1.0 + dist * 0.03);

    // frac(x) = parte decimal de x: repite el patrón [0..1) infinitamente.
    // Al dividir dist por el intervalo, cada "diente" es una línea potencial.
    // Multiplicamos de nuevo por el intervalo para que la comparación sea en metros.
    float fracDist = fract(dist / dynamicInterval) * dynamicInterval;

    // smoothstep simétrico para que las líneas tengan un gradiente suave
    // en lugar de ser un bloque binario plano.
    float halfWidth = u_line_width * 0.5;
    float distToCenter = abs(fracDist - halfWidth);
    return smoothstep(halfWidth, 0.0, distToCenter);
}

void main()
{
    // Calcular las UVs de este fragmento a partir de su posición en pantalla
    vec2 uv = gl_FragCoord.xy * u_iRes;

    // Leer el color original de la escena (sin modificar por ahora)
    vec3 sceneColor = texture(u_scene_color_texture, uv).rgb;

    // Leer la profundidad en bruto del G-Buffer para este píxel
    float rawDepth = texture(u_depth_texture, uv).r;

    // --- Early exit: si es el skybox (depth == 1.0), no aplicar el efecto ---
    if (rawDepth >= 0.9999) {
        FragColor = vec4(sceneColor, 1.0);
        return;
    }

    // --- CAPA 1: Reconstruir la posición 3D del píxel en el mundo ---
    vec3 worldPos = GetWorldPosition(uv, rawDepth);

    // Distancia horizontal (plano XZ) desde el origen del scan
    // Usar XZ hace que el efecto siga el suelo independientemente de la altura del terreno
    float dist = distance(worldPos.xz, u_scan_origin.xz);

    // Máscara general de área: 1.0 dentro del radio, 0.0 fuera
    float insideMask = step(dist, u_scan_radius);

    // Máscara de desvanecimiento progresivo del rastro (Trailing Fade-out)
    // A medida que la onda avanza, el rastro trasero se va difuminando suavemente.
    float trailMask = smoothstep(u_scan_radius - u_trail_width, u_scan_radius, dist);

    // --- CAPA 2: Oscurecimiento del terreno (Darken Blend) ---
    // El oscurecimiento también se desvanece suavemente detrás del frente de onda
    vec3 workingColor = ApplyDarkenBlend(sceneColor, u_darken_color, insideMask * trailMask * u_darken_strength);

    // --- ANIMACIÓN (Paso 6): Círculo de carga que se contrae ---
    // Durante la Fase 1, un círculo oscuro adicional se CONTRAE hacia el origen.
    // Cuando u_charge_radius > 0, todo lo que está DENTRO de ese radio se oscurece aún más,
    // creando la ilusión de "carga de energía" antes de la explosión de la onda.
    if (u_charge_radius > 0.0) {
        float chargeMask = step(dist, u_charge_radius);  // 1.0 dentro del círculo de carga
        workingColor = ApplyDarkenBlend(workingColor, vec3(0.0, 0.0, 0.05), chargeMask * 0.9);
    }

    // --- CAPA 3: Frente de onda (Edge Gradient) ---
    // Rampa que va de 0 (interior) a 1 (borde frontal) → emisión aditiva azul
    float edgeGrad   = CalculateEdgeGradient(dist);
    vec3 edgeEmission = edgeGrad * u_edge_color * u_edge_intensity;

    // --- CAPA 4 + 5: Líneas holográficas con primera línea blanca ---
    float dynamicInterval = u_line_interval * (1.0 + dist * 0.03);
    float linesMask = CalculateScanLines(dist) * insideMask * trailMask;

    // CAPA 5: Detectar el anillo más exterior (el inmediatamente detrás del frente).
    //
    // La idea: contamos hacia atrás desde u_scan_radius.
    // "distFromEdge" = cuánto se ha alejado el píxel del frente de la onda.
    // Si esa distancia cae dentro del PRIMER período del intervalo dinámico,
    // estamos en la línea más exterior → la pintamos en BLANCO.
    float distFromEdge = u_scan_radius - dist;
    float isFirstRing  = step(0.0, distFromEdge) * step(distFromEdge, dynamicInterval);

    // Máscara de la primera línea: solo donde hay línea Y estamos en el primer anillo
    float leadingLineMask  = linesMask * isFirstRing;
    // Máscara de las líneas interiores: todas menos la primera
    float interiorLinesMask = linesMask * (1.0 - isFirstRing);

    // Emisión azul para líneas interiores, blanco puro para la línea delantera
    vec3 linesEmission   = interiorLinesMask * u_line_color   * u_line_intensity;
    vec3 leadingEmission = leadingLineMask   * vec3(1.0, 1.0, 1.0) * u_leading_intensity;

    // --- Composición final: todas las capas sumadas (método aditivo) ---
    // workingColor  = escena oscurecida              (Capa 2)
    // edgeEmission  = brillo del frente de onda      (Capa 3) [aditivo]
    // linesEmission = líneas holográficas azules     (Capa 4) [aditivo]
    // leadingEmission = primera línea blanca         (Capa 5) [aditivo]
    //
    // u_scan_opacity modula TODAS las emisiones aditivas pero NO el oscurecimiento
    // (el fondo oscuro se aplica siempre para dar continuidad visual durante el fade)
    float opacity    = u_scan_opacity;
    vec3 allEmission = (edgeEmission + linesEmission + leadingEmission) * opacity;
    vec3 finalColor  = workingColor + allEmission;

    FragColor = vec4(finalColor, 1.0);
}
