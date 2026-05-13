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

    vec3 light_intensity = u_light_colors[index] * attenuation * shadow_factor;
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

	vec3 base_color = color.rgb;

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
	out_color += u_ambient_light * cookTorranceDiffuseBRDF(base_color, metalness);

	// Iterar luces
	for (int i = 0; i < MAX_LIGHTS; i++) {
		if (i < u_num_lights) {
			out_color += computeLight(i, v_world_position, N, V, base_color, roughness, metalness);
		}
	}

	// Output final
	FragColor = vec4(out_color, color.a);
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

uniform samplerCube u_texture;
uniform vec3 u_camera_position;

// Salidas mÃºltiples
layout(location = 0) out vec4 gbuffer_albedo;
layout(location = 1) out vec4 gbuffer_normal;

void main()
{
	vec3 E = v_world_position - u_camera_position;
	vec4 color = texture( u_texture, E );
	
    gbuffer_albedo = color;
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
    // 1. Color base
    vec4 color = u_color * texture(u_texture, v_uv);

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
    vec3 out_color = u_ambient_light * cookTorranceDiffuseBRDF(albedo, metalness) * ao_factor;
    
    // Add directional lights
    for (int i = 0; i < MAX_LIGHTS; i++) {
        if (i >= u_num_dir_lights) break;
        
        vec3 L = normalize(-u_dir_light_direction[i]);
        vec3 light_color = u_dir_light_color[i];
        
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
    
    vec3 light_intensity = u_light_color * attenuation * shadow;
    
    vec3 result = vec3(0.0);
    float NdotL = saturate(dot(N, L));
    vec3 diffuse_brdf = cookTorranceDiffuseBRDF(albedo, metalness);
    vec3 specular_brdf = cookTorranceSpecularBRDF(N, V, L, albedo, metalness, roughness);
    result += (diffuse_brdf + specular_brdf) * light_intensity * NdotL;
    
    FragColor = vec4(result, 1.0);
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
