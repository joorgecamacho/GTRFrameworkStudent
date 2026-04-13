//example of some shaders compiled
flat basic.vs flat.fs
texture basic.vs texture.fs
skybox basic.vs skybox.fs
depth quad.vs depth.fs
multi basic.vs multi.fs
phong_single basic.vs phong_single.fs

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

\phong_single.fs

#version 330 core

in vec3 v_position;
in vec3 v_world_position;
in vec3 v_normal;
in vec2 v_uv;
in vec4 v_color;

uniform vec4 u_color;
uniform sampler2D u_texture;
uniform float u_alpha_cutoff;
uniform float u_shininess;

uniform vec3 u_camera_position;
uniform vec3 u_ambient_light;

#define MAX_LIGHTS 16
uniform int u_num_lights;
uniform vec3 u_light_positions[MAX_LIGHTS];
uniform vec3 u_light_colors[MAX_LIGHTS];
uniform float u_light_intensities[MAX_LIGHTS];
uniform vec3 u_light_directions[MAX_LIGHTS];
uniform int u_light_types[MAX_LIGHTS];
uniform vec2 u_light_cones[MAX_LIGHTS];

out vec4 FragColor;

void main()
{
	vec4 base_color = u_color * texture(u_texture, v_uv);
	if (base_color.a < u_alpha_cutoff)
		discard;

	vec3 N = normalize(v_normal);
	vec3 V = normalize(u_camera_position - v_world_position);

	vec3 final_rgb = base_color.rgb * (u_ambient_light + vec3(0.03));
	int num_lights = min(u_num_lights, MAX_LIGHTS);
	for (int i = 0; i < num_lights; ++i)
	{
		int light_type = u_light_types[i];
		vec3 L_dir = u_light_positions[i] - v_world_position;
		float distance_to_light = length(L_dir);
		vec3 L = distance_to_light > 0.0001 ? normalize(L_dir) : vec3(0.0, 0.0, 0.0);

		float attenuation = u_light_intensities[i] / (1.0 + distance_to_light * distance_to_light);

		// Directional light
		if (light_type == 3)
		{
			vec3 D = normalize(u_light_directions[i]);
			L = normalize(-D);
			attenuation = u_light_intensities[i];
		}
		//point light with cone attenuation
		else if (light_type == 2)
		{
			vec3 D = normalize(u_light_directions[i]);
			float inner_angle = u_light_cones[i].x;
			float outer_angle = u_light_cones[i].y;
			float cos_inner = cos(inner_angle);
			float cos_outer = cos(outer_angle);

			float cos_theta = dot(D, normalize(-L));

			//cone
			float spot_factor = clamp((cos_theta - cos_outer) / max(cos_inner - cos_outer, 0.0001), 0.0, 1.0);
			attenuation *= spot_factor;
		}

		float NdotL = abs(dot(N, L));
		vec3 diffuse = base_color.rgb * u_light_colors[i] * NdotL * attenuation;

		vec3 R = reflect(-L, N);
		float spec_power = pow(abs(dot(R, V)), max(u_shininess, 1.0));
		vec3 specular = u_light_colors[i] * spec_power * attenuation;

		final_rgb += diffuse + specular;
	}

	FragColor = vec4(final_rgb, base_color.a);
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
