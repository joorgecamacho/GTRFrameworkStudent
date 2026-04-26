//example of some shaders compiled
flat basic.vs flat.fs
texture basic.vs texture.fs
phong basic.vs phong.fs
skybox basic.vs skybox.fs
depth quad.vs depth.fs
multi basic.vs multi.fs
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

\phong.fs

#version 330 core

#include "perturbNormal"

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

// Uniforms de iluminación (nuevos)
uniform vec3 u_ambient_light;     // luz ambiental de la escena
uniform vec3 u_camera_position;   // posición de la cámara (para specular)
uniform float u_shininess;        // alpha/shininess del material

const int MAX_LIGHTS = 8;
uniform vec3 u_light_position[MAX_LIGHTS];
uniform vec3 u_light_colors[MAX_LIGHTS];
uniform int u_light_types[MAX_LIGHTS];       // 1=POINT, 2=SPOT, 3=DIRECTIONAL
uniform vec3 u_light_directions[MAX_LIGHTS]; // dirección frontal de la luz
uniform vec2 u_light_cone_info[MAX_LIGHTS];  // x=alpha_min, y=alpha_max (en radianes)
uniform int u_num_lights;
uniform sampler2D u_shadow_map; // La textura de profundidad que creamos
uniform mat4 u_shadow_vp;       // La matriz View-Projection de nuestra cámara de luz


uniform sampler2D u_normal_texture;
uniform int u_has_normal_texture;
uniform int u_show_normals;

out vec4 FragColor;

// --- FUNCIÓN DE CÁLCULO DE LUZ INDIVIDUAL ---
vec3 computeLight(int index, vec3 world_pos, vec3 N, vec3 V, vec3 base_color) {
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

    vec3 light_intensity = u_light_colors[index] * attenuation;
    vec3 result_color = vec3(0.0);

    // Diffuse
    float NdotL = clamp(dot(N, L), 0.0, 1.0);
    result_color += base_color * NdotL * light_intensity;

    // Specular
    vec3 R = reflect(-L, N);
    float RdotV = clamp(dot(R, V), 0.0, 1.0);
    result_color += base_color * pow(RdotV, u_shininess) * light_intensity;

    return result_color;
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
	// Si u_show_normals == 1 aplicamos el Normal Map, si es 0 usamos la normal geométrica plana
	if (u_has_normal_texture == 1 && u_show_normals == 1) {
		// Leer textura de normales [0, 1]
		vec3 normal_pixel = texture(u_normal_texture, uv).xyz;
		// Mapear de [0, 1] a [-1, 1]
		normal_pixel = normal_pixel * 2.0 - 1.0;
		
		N = perturbNormal(N, v_world_position, uv, normal_pixel);
	}

	vec3 V = normalize(u_camera_position - v_world_position);

	vec3 out_color = vec3(0.0);

	// Ambient: una sola vez
	out_color += u_ambient_light * base_color;

	// Iterar luces
	for (int i = 0; i < MAX_LIGHTS; i++) {
		if (i < u_num_lights) {
			out_color += computeLight(i, v_world_position, N, V, base_color);
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
out vec4 FragColor;

void main()
{
	vec3 E = v_world_position - u_camera_position;
	vec4 color = texture( u_texture, E );
	FragColor = color;
}


\multi.fs

#version 330 core

in vec3 v_position;
in vec3 v_world_position;
in vec3 v_normal;
in vec2 v_uv;

uniform vec4 u_color;
uniform sampler2D u_texture;
uniform float u_time;
uniform float u_alpha_cutoff;

layout(location = 0) out vec4 FragColor;
layout(location = 1) out vec4 NormalColor;

void main()
{
	vec2 uv = v_uv;
	vec4 color = u_color;
	color *= texture( u_texture, uv );

	if(color.a < u_alpha_cutoff)
		discard;

	vec3 N = normalize(v_normal);

	FragColor = color;
	NormalColor = vec4(N,1.0);
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
