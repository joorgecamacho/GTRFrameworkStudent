#include "renderer.h"

#include <algorithm> //sort

#include "camera.h"
#include "../gfx/gfx.h"
#include "../gfx/shader.h"
#include "../gfx/mesh.h"
#include "../gfx/texture.h"
#include "../gfx/fbo.h"
#include "../pipeline/prefab.h"
#include "../pipeline/material.h"
#include "../pipeline/animation.h"
#include "../utils/utils.h"
#include "../extra/hdre.h"
#include "../core/ui.h"

#include "light.h"
#include "scene.h"


using namespace SCN;

//some globals
GFX::Mesh sphere;

Renderer::Renderer(const char* shader_atlas_filename)
{
	render_wireframe = false;
	render_boundaries = false;
	scene = nullptr;
	skybox_cubemap = nullptr;

	for (int i = 0; i < MAX_LIGHTS; ++i) {
		shadow_fbos[i] = new GFX::FBO();
		shadow_fbos[i]->setDepthOnly(1024, 1024);
	}
 

	if (!GFX::Shader::LoadAtlas(shader_atlas_filename))
		exit(1);
	GFX::checkGLErrors();

	sphere.createSphere(1.0f);
	sphere.uploadToVRAM();

	// SSAO: generar puntos de muestreo una sola vez (radio 1.0, se escala en el shader)
	ssao_sample_points = generateSpherePoints(ssao_num_samples, 1.0f, ssao_hemisphere);
}

void Renderer::triggerScanner(const Vector3f& origin)
{
	if (scanner_method == SCANNER_METHOD_A) {
		scan_origin        = origin;
		scan_radius        = 0.0f;
		scan_opacity       = 0.0f;
		scan_charge_radius = 2.5f;
		scan_start_time    = getTime();
		scan_time          = 0.0f;
		last_scan_frame_time = getTime();
		scan_paused        = false;
		scan_active        = true;
	}
	else if (scanner_method == SCANNER_METHOD_B) {
		// Drop the origin to ground level so the sphere's equator cuts the floor
		// plane in a full expanding circle (radar rings). A center up in the air
		// would only intersect the ground once the radius exceeds its height.
		scanner_origin = Vector3f(origin.x, scanner_ground_height, origin.z);
		scanner_radius = 0.0f;
		scanner_elapsed = 0.0f;
		scanner_active = true;
	}
}

float Renderer::evaluateScannerRadius() const
{
	if (scanner_duration <= 0.0f)
		return scanner_max_radius;

	float t = scanner_elapsed / scanner_duration;
	if (t > 1.0f) t = 1.0f;

	// Smoothstep: gentle start with "weight" but still reaches a large radius
	// well before the end, so the rings are visible across the scene.
	float eased = t * t * (3.0f - 2.0f * t);
	return scanner_max_radius * eased;
}

void Renderer::updateScanner(float dt)
{
	if (scanner_method == SCANNER_METHOD_A) {
		if (!scan_active)
			return;

		if (!scan_manual_mode && !scan_paused) {
			scan_time += dt;
		}

		float t = scan_time;

		// ============================================================
		// FASE 1 (0.0s — 0.5s): CARGA — Círculo oscuro que se contrae
		// El radio crece lentamente, scan_opacity sube de 0 a 1.
		// Un círculo de carga se contrae hacia el jugador.
		// ============================================================
		if (t < 0.5f) {
			float phase_t      = t / 0.5f;
			scan_radius        = phase_t * phase_t * 2.0f;
			scan_charge_radius = (1.0f - phase_t) * 2.5f;
			scan_opacity       = phase_t;
		}
		// ============================================================
		// FASE 2 (0.5s — 0.5 + scan_pause_duration): PAUSA — Tensión antes de la onda
		// ============================================================
		else if (t < 0.5f + scan_pause_duration) {
			scan_radius        = 2.0f;
			scan_charge_radius = 0.0f;
			scan_opacity       = 1.0f;
		}
		// ============================================================
		// FASE 3 (0.5s + scan_pause_duration +): EXPANSIÓN — Onda rápida
		// ============================================================
		else {
			float et           = t - (0.5f + scan_pause_duration);
			// (1 - e^-kt): curva que sube rápido y se aplana
			scan_radius        = 2.0f + (1.0f - expf(-et * scan_expansion_speed)) * (scan_max_radius - 2.0f);
			scan_charge_radius = 0.0f;
			// Fade-out cuando el radio supera el 75% del máximo
			float progress     = scan_radius / scan_max_radius;
			scan_opacity       = (progress > 0.75f)
							  ? clamp((1.0f - progress) / 0.25f, 0.0f, 1.0f)
							  : 1.0f;
			// Apagar automáticamente al terminar si no estamos en modo manual
			if (scan_opacity <= 0.01f && !scan_manual_mode) {
				scan_active = false;
				scan_radius = 0.0f;
			}
		}
	}
	else if (scanner_method == SCANNER_METHOD_B) {
		if (!scanner_active)
			return;

		scanner_elapsed += dt;
		if (scanner_elapsed >= scanner_duration) {
			scanner_active = false;
			scanner_elapsed = 0.0f;
			scanner_radius = 0.0f;
			return;
		}

		scanner_radius = evaluateScannerRadius();
	}
}

void Renderer::renderScannerSphere(Camera* camera)
{
	if (!enable_scanner || !scanner_active || !scanner_show_sphere)
		return;
	if (scanner_radius <= 0.01f)
		return;

	GFX::Shader* shader = GFX::Shader::Get("scanner_sphere");
	if (!shader)
		return;

	// REQUIRED render state for the intersection to ALWAYS be visible:
	//  - Additive blend (energy look, never hidden by background)
	//  - Depth TEST DISABLED: we need the sphere fragments that fall behind
	//    geometry, otherwise they'd be culled before we can detect contact.
	//  - Depth mask OFF: the bubble never writes depth (it is not solid).
	//  - Cull back faces: one clean front-facing contact line per shell.
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_CULL_FACE); // both hemispheres → full intersection circle

	shader->enable();
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform("u_depth_texture", gbuffer_fbo->depth_texture, 0);
	shader->setUniform("u_inverse_viewprojection", camera->inverse_viewprojection_matrix);
	shader->setUniform("u_iRes", vec2(1.0f / (float)illumination_fbo->width, 1.0f / (float)illumination_fbo->height));
	shader->setUniform("u_scanner_color", scanner_color);
	shader->setUniform("u_contact_thickness", scanner_contact_thickness);
	shader->setUniform("u_rim_power", scanner_sphere_rim);

	// Multiple concentric expanding shells = a leading ring + trailing sub-lines.
	// Each shell is the same sphere mesh scaled to a slightly smaller radius.
	for (int i = 0; i < scanner_ring_count; ++i) {
		float r = scanner_radius - i * scanner_ring_spacing;
		if (r <= 0.05f)
			continue;

		float fade = 1.0f - (float)i / (float)scanner_ring_count; // trailing rings dimmer

		Matrix44 model;
		model.setTranslation(scanner_origin.x, scanner_origin.y, scanner_origin.z);
		model.scale(r, r, r);

		shader->setUniform("u_model", model);
		shader->setUniform("u_scanner_intensity", scanner_intensity * fade);
		shader->setUniform("u_sphere_alpha", scanner_sphere_alpha * fade);
		// Only the leading shell shows the faint volumetric bubble shell
		shader->setUniform("u_rim_strength", (i == 0) ? scanner_rim_strength : 0.0f);

		sphere.render(GL_TRIANGLES);
	}

	shader->disable();

	// Restore state
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	glCullFace(GL_BACK);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
}

void Renderer::resetScannerDefaults()
{
	scanner_max_radius = 45.0f;
	scanner_color = Vector3f(0.1f, 0.85f, 1.0f);
	scanner_intensity = 3.5f;
	scanner_duration = 7.0f;
	scanner_show_sphere = true;
	scanner_sphere_rim = 2.5f;
	scanner_sphere_alpha = 1.0f;
	scanner_contact_thickness = 0.35f;
	scanner_rim_strength = 0.35f;
	scanner_ring_count = 6;
	scanner_ring_spacing = 1.8f;
}

void Renderer::setupScene()
{
	if (scene->skybox_filename.size())
		skybox_cubemap = GFX::Texture::Get(std::string(scene->base_folder + "/" + scene->skybox_filename).c_str());
	else
		skybox_cubemap = nullptr;
}

void Renderer::parseNode(SCN::Node* node) {
	if(!node) return;

	if(node->mesh && node->material) {
		// Frustum culling desactivado para que los objetos fuera de cámara
		// sigan apareciendo en el shadow map y proyecten sombras correctamente.
		render_list.push_back({
			node->mesh,
			node->material,
			node->getGlobalMatrix(),
			node->getGlobalMatrix().getTranslation().distance(Camera::current->eye)
		});
	}

	for (int i = 0; i < node->children.size(); i++) {
		parseNode(node->children[i]);
	}
}

void Renderer::parseSceneEntities(SCN::Scene* scene, Camera* cam) {
	// HERE =====================
	// TODO: GENERATE RENDERABLES
	// ==========================
	render_list.clear();
	light_list.clear();
	for (int i = 0; i < scene->entities.size(); i++) {
		BaseEntity* entity = scene->entities[i];

		if (!entity->visible) {
			continue;
		}
		if(entity->getType() == SCN::eEntityType::PREFAB){
			//Convertimos el entity base a PrefabEntity
			PrefabEntity* prefabEntity = (PrefabEntity*)entity; 
			//Empezamos la magia pasándole la raíz y la cámara
			parseNode(&prefabEntity->root); 
		}
		if(entity->getType() == SCN::eEntityType::LIGHT){
			// Cast a LightEntity para acceder a light_type, intensity, color, etc.
			LightEntity* light = (LightEntity*)entity;
			light_list.push_back(light);
		}
	}

	// Tarea 3.4: Ordenar Render Calls
	std::sort(render_list.begin(), render_list.end(), [](const sRenderable& a, const sRenderable& b) {
		bool a_transparent = (a.material->alpha_mode == SCN::eAlphaMode::BLEND);
		bool b_transparent = (b.material->alpha_mode == SCN::eAlphaMode::BLEND);

		// Si uno es opaco y el otro transparente, el opaco siempre va primero
		if (!a_transparent && b_transparent) return true;
		if (a_transparent && !b_transparent) return false;

		// Si ambos son transparentes: de LEJOS a CERCA (mayor distancia primero)
		if (a_transparent && b_transparent) {
			return a.distance_to_camera > b.distance_to_camera;
		}

		// Si ambos son opacos (o mask): de CERCA a LEJOS (menor distancia primero para ahorrar overdraw)
		return a.distance_to_camera < b.distance_to_camera;
	});
}

void Renderer::renderScene(SCN::Scene* scene, Camera* camera)
{
	this->scene = scene;
	setupScene();
	parseSceneEntities(scene, camera);

	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	int width = viewport[2];
	int height = viewport[3];

	if (gbuffer_fbo == nullptr || gbuffer_fbo->color_textures[0]->width != width || gbuffer_fbo->color_textures[0]->height != height) {
		if (gbuffer_fbo) delete gbuffer_fbo;
		gbuffer_fbo = new GFX::FBO();
		gbuffer_fbo->create(width, height, 2, GL_RGBA, GL_UNSIGNED_BYTE, true);
	}

	if (illumination_fbo == nullptr || illumination_fbo->color_textures[0]->width != width || illumination_fbo->color_textures[0]->height != height) {
		if (illumination_fbo) delete illumination_fbo;
		illumination_fbo = new GFX::FBO();
		illumination_fbo->create(width, height, 1, GL_RGBA, GL_HALF_FLOAT, true);//3.2 HDR: half-float
	}

	// SSAO FBO: 1 textura de color (escala de grises), sin depth buffer (es post-proceso 2D)
	if (ssao_fbo == nullptr || ssao_fbo->color_textures[0]->width != width || ssao_fbo->color_textures[0]->height != height) {
		if (ssao_fbo) delete ssao_fbo;
		ssao_fbo = new GFX::FBO();
		ssao_fbo->create(width, height, 1, GL_RGB, GL_UNSIGNED_BYTE, false);
	}

	generateShadowMap();

	if (use_deferred) {
		gbuffer_fbo->bind();
		glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		GFX::checkGLErrors();
		if (skybox_cubemap)
			renderSkybox(skybox_cubemap, false);
		for (int i = 0; i < render_list.size(); i++) {
			if (render_list[i].material->alpha_mode == SCN::eAlphaMode::BLEND)
				continue;
			renderMeshWithMaterial(render_list[i].matrix, render_list[i].mesh, render_list[i].material);
		}
		gbuffer_fbo->unbind();
		renderDeferred(camera);
	}
	else {
		renderForward(camera);
	}
}




void Renderer::renderSkybox(GFX::Texture* cubemap, bool apply_gamma)
{
	Camera* camera = Camera::current;

	// Apply skybox necesarry config:
	// No blending, no dpeth test, we are always rendering the skybox
	// Set the culling aproppiately, since we just want the back faces
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	if (render_wireframe)
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

	GFX::Shader* shader = GFX::Shader::Get("skybox");
	if (!shader)
		return;
	shader->enable();

	// Center the skybox at the camera, with a big sphere
	Matrix44 m;
	m.setTranslation(camera->eye.x, camera->eye.y, camera->eye.z);
	m.scale(10, 10, 10);
	shader->setUniform("u_model", m);

	// Upload camera uniforms
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);

	shader->setUniform("u_texture", cubemap, 0);
	shader->setUniform("u_apply_gamma", apply_gamma ? 1 : 0);

	sphere.render(GL_TRIANGLES);

	shader->disable();

	// Return opengl state to default
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	glEnable(GL_DEPTH_TEST);
}

// Renders a mesh given its transform and material
void Renderer::renderMeshWithMaterial(const Matrix44 model, GFX::Mesh* mesh, SCN::Material* material)
{
	//in case there is nothing to do
	if (!mesh || !mesh->getNumVertices() || !material )
		return;

	//define locals to simplify coding
	GFX::Shader* shader = GFX::Shader::Get("gbuffer");
	if (!shader)
		return;
		
	Camera* camera = Camera::current;

	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glDisable(GL_BLEND);

	shader->enable();

	material->bind(shader);

	//upload uniforms
	shader->setUniform("u_model", model);
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);
	shader->setUniform1("u_show_normals", show_normals ? 1 : 0);

	if (render_wireframe)
		glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );

	mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
}

void Renderer::renderPlain(const Matrix44 model, GFX::Mesh* mesh, SCN::Material* material, Camera* light_cam)
{
	if (!mesh || !mesh->getNumVertices() || !material) return;

	// Ignoramos objetos semi-transparentes (cristales, etc.)
	if (material->alpha_mode == SCN::eAlphaMode::BLEND) return;

	GFX::Shader* shader = nullptr;

	// Si el material tiene recortes (hojas de árbol, vallas...), necesitamos evaluar la textura
	if (material->alpha_mode == SCN::eAlphaMode::MASK) {
		shader = GFX::Shader::Get("texture"); // Usamos shader 'texture' que ya tiene el discard
		if (!shader) return;

		shader->enable();
		shader->setUniform("u_model", model);
		shader->setUniform("u_viewprojection", light_cam->viewprojection_matrix);
		shader->setUniform("u_color", Vector4f(1, 1, 1, 1));
		shader->setUniform("u_alpha_cutoff", material->alpha_cutoff);

		// Le pasamos la textura de color para que pueda leer la transparencia
		if (material->textures[SCN::eTextureChannel::ALBEDO].texture) {
			shader->setUniform("u_texture", material->textures[SCN::eTextureChannel::ALBEDO].texture, 0);
		}
	}
	// Para el 90% de los objetos (opacos), usamos la vía ultrarrápida
	else {
		shader = GFX::Shader::Get("flat");
		if (!shader) return;

		shader->enable();
		shader->setUniform("u_model", model);
		shader->setUniform("u_viewprojection", light_cam->viewprojection_matrix);
	}

	mesh->render(GL_TRIANGLES);
	shader->disable();
}
void Renderer::generateShadowMap() {
	if (light_list.empty()) return;

	GLint default_viewport[4];
	glGetIntegerv(GL_VIEWPORT, default_viewport);

	int num_lights = (int)light_list.size() < MAX_LIGHTS ? (int)light_list.size() : MAX_LIGHTS;

	// Hacemos una foto POR CADA LUZ
	for (int i = 0; i < num_lights; ++i) {
		LightEntity* light_ent = light_list[i];

		// 1. Matemáticas de la cámara
		mat4 light_model = light_ent->root.getGlobalMatrix();
		vec3 light_pos = light_model.getTranslation();
		vec3 light_dir = light_model.frontVector();
		vec3 light_target = light_pos + light_dir;

		Camera light_cam;
		light_cam.lookAt(light_pos, light_target, vec3(0.0f, 1.0f, 0.0f));

		if (light_ent->light_type == eLightType::SPOT) {
			light_cam.setPerspective(light_ent->cone_info.y * RAD2DEG * 2.0f, 1.0f, light_ent->near_distance, light_ent->max_distance);
		}
		else if (light_ent->light_type == eLightType::DIRECTIONAL) {
			float half_size = light_ent->area / 2.0f;
			light_cam.setOrthographic(-half_size, half_size, -half_size, half_size, light_ent->near_distance, light_ent->max_distance);
		}

		light_cam.updateViewMatrix();
		light_cam.updateProjectionMatrix();

		// Guardamos la matriz de ESTA luz en la celda 'i'
		this->shadow_viewprojections[i] = light_cam.viewprojection_matrix;

		// 2. Renderizar al FBO de esta luz en concreto
		shadow_fbos[i]->bind();
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glEnable(GL_CULL_FACE);
		glFrontFace(GL_CW);
		glColorMask(false, false, false, false);

		glClear(GL_DEPTH_BUFFER_BIT);

		for (int j = 0; j < render_list.size(); ++j) {
			sRenderable& render_call = render_list[j];
			if (render_call.material->alpha_mode != SCN::eAlphaMode::BLEND) {
				renderPlain(render_call.matrix, render_call.mesh, render_call.material, &light_cam);
			}
		}

		// 3. Restaurar estado antes de pasar a la siguiente luz
		glColorMask(true, true, true, true);
		glFrontFace(GL_CCW);
		glDisable(GL_CULL_FACE);
		shadow_fbos[i]->unbind();
	}

	glViewport(default_viewport[0], default_viewport[1], default_viewport[2], default_viewport[3]);
}

void Renderer::renderForward(Camera* camera) {
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	if (skybox_cubemap)
		renderSkybox(skybox_cubemap, true);

	for (int i = 0; i < render_list.size(); i++) {
		renderMeshWithMaterialForward(render_list[i].matrix, render_list[i].mesh, render_list[i].material);
	}
}

void Renderer::renderDeferred(Camera* camera) {

	// ==========================================
	// ASSIGNMENT 6: PASE DE SSAO
	// ==========================================
	if (enable_ssao && !ssao_sample_points.empty()) {
		ssao_fbo->bind();

		// Limpiar a blanco (1.0 = sin oclusión). Si un píxel no se procesa, no tendrá sombra.
		glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_BLEND);

		GFX::Shader* ao_shader = GFX::Shader::Get("ssao");
		if (ao_shader) {
			ao_shader->enable();

			// --- Matrices de cámara ---
			// Projection: para proyectar muestras 3D → coordenadas de pantalla 2D
			Matrix44 proj = camera->projection_matrix;
			ao_shader->setUniform("u_p_mat", proj);

			// Inverse Projection: para reconstruir posición 3D desde la profundidad 2D
			Matrix44 proj_inv = proj;
			proj_inv.inverse();
			ao_shader->setUniform("u_inv_p_mat", proj_inv);

			// View Matrix: para transformar normales de World Space → View Space
			ao_shader->setUniform("u_view_mat", camera->view_matrix);

			// --- Parámetros del SSAO ---
			ao_shader->setUniform1("u_sample_count", ssao_num_samples);
			ao_shader->setUniform("u_sample_radius", ssao_radius);

			// Resolución inversa: para centrar las UVs al centro exacto del píxel
			float inv_w = 1.0f / (float)ssao_fbo->color_textures[0]->width;
			float inv_h = 1.0f / (float)ssao_fbo->color_textures[0]->height;
			ao_shader->setUniform("u_res_inv", vec2(inv_w, inv_h));

			// Array de puntos de muestreo (precalculados en la CPU)
			ao_shader->setUniform3Array("u_sample_pos",
				(float*)&ssao_sample_points[0], ssao_num_samples);

			// --- Texturas del G-Buffer ---
			ao_shader->setUniform("u_depth_texture", gbuffer_fbo->depth_texture, 7);
			ao_shader->setUniform("u_normal_texture", gbuffer_fbo->color_textures[1], 8);

			// Dibujar quad a pantalla completa → ejecuta el fragment shader 1 vez por píxel
			GFX::Mesh* quad = GFX::Mesh::getQuad();
			quad->render(GL_TRIANGLES);

			ao_shader->disable();
		}

		ssao_fbo->unbind();
	}

	// ==========================================
	// ASSIGNMENT 4: TAREA 2.4.1 Y 2.4.2: PRIMERA PASADA
	// ==========================================

	// Copiar el buffer de profundidad del G-Buffer al de Iluminación
	glBindFramebuffer(GL_READ_FRAMEBUFFER, gbuffer_fbo->fbo_id);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, illumination_fbo->fbo_id);
	glBlitFramebuffer(
		0, 0, gbuffer_fbo->width, gbuffer_fbo->height,
		0, 0, illumination_fbo->width, illumination_fbo->height,
		GL_DEPTH_BUFFER_BIT, GL_NEAREST
	);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	illumination_fbo->bind();
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
	glClear(GL_COLOR_BUFFER_BIT); // Depth ya está copiado, no limpiarlo

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	GFX::Shader* global_shader = GFX::Shader::Get("deferred_global");
	global_shader->enable();

	// Texturas del G-Buffer
	global_shader->setUniform("u_albedo_texture", gbuffer_fbo->color_textures[0], 0);
	global_shader->setUniform("u_normal_texture", gbuffer_fbo->color_textures[1], 1);
	global_shader->setUniform("u_depth_texture", gbuffer_fbo->depth_texture, 2);

	global_shader->setUniform("u_ambient_light", scene->ambient_light);
	global_shader->setUniform("u_camera_position", camera->eye);
	global_shader->setUniform("u_inverse_viewprojection", camera->inverse_viewprojection_matrix);
	global_shader->setUniform("u_iRes", vec2(1.0f / (float)illumination_fbo->width, 1.0f / (float)illumination_fbo->height));
	global_shader->setUniform("u_shininess", 32.0f);

	// SSAO: enviar la textura de oclusión al shader de iluminación
	global_shader->setUniform("u_ssao_texture", ssao_fbo->color_textures[0], 4);
	global_shader->setUniform1("u_enable_ssao", enable_ssao ? 1 : 0);

	// Filtrar y enviar luces direccionales
	vec3 dir_light_colors[MAX_LIGHTS];
	vec3 dir_light_dirs[MAX_LIGHTS];
	int dir_light_indices[MAX_LIGHTS];
	int num_dir = 0;
	
	for (int i = 0; i < light_list.size() && num_dir < MAX_LIGHTS; ++i) {
		if (light_list[i]->light_type == eLightType::DIRECTIONAL) {
			dir_light_colors[num_dir] = light_list[i]->color * light_list[i]->intensity;
			dir_light_dirs[num_dir] = light_list[i]->root.getGlobalMatrix().frontVector();
			dir_light_indices[num_dir] = i;
			num_dir++;
		}
	}
	
	global_shader->setUniform1("u_num_dir_lights", num_dir);
	if (num_dir > 0) {
		global_shader->setUniform3Array("u_dir_light_color", &dir_light_colors[0].x, num_dir);
		global_shader->setUniform3Array("u_dir_light_direction", &dir_light_dirs[0].x, num_dir);
		global_shader->setUniform1Array("u_dir_light_indices", &dir_light_indices[0], num_dir);
		
		// Sombras de direccionales (usamos la primera para simplificar)
		int first_dir_idx = dir_light_indices[0];
		if (shadow_fbos[first_dir_idx] && shadow_fbos[first_dir_idx]->depth_texture) {
			global_shader->setUniform1("u_has_shadow_map", 1);
			global_shader->setUniform("u_shadow_bias", 0.01f);
			global_shader->setUniform("u_shadow_vp", shadow_viewprojections[first_dir_idx]);
			global_shader->setUniform("u_shadow_map", shadow_fbos[first_dir_idx]->depth_texture, 3);
		} else {
			global_shader->setUniform1("u_has_shadow_map", 0);
		}
	} else {
		global_shader->setUniform1("u_has_shadow_map", 0);
	}

	GFX::Mesh* quad = GFX::Mesh::getQuad();
	quad->render(GL_TRIANGLES);
	global_shader->disable();

	// ==========================================
	// ASSIGNMENT 4: TAREA 2.4.3: VOLUMENES DE LUZ
	// ==========================================
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE); // Aditivo
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_GREATER); // Renderiza si está detrás del depth o la geometría está delante
	glDepthMask(GL_FALSE); // No escribe depth
	glEnable(GL_CULL_FACE);
	glFrontFace(GL_CW); // Solo caras traseras (Back faces)
	glEnable(GL_DEPTH_CLAMP); // Evita que el near/far plane corte los light volumes

	GFX::Shader* light_shader = GFX::Shader::Get("deferred_light");
	light_shader->enable();
	
	light_shader->setUniform("u_albedo_texture", gbuffer_fbo->color_textures[0], 0);
	light_shader->setUniform("u_normal_texture", gbuffer_fbo->color_textures[1], 1);
	light_shader->setUniform("u_depth_texture", gbuffer_fbo->depth_texture, 2);
	light_shader->setUniform("u_camera_position", camera->eye);
	light_shader->setUniform("u_inverse_viewprojection", camera->inverse_viewprojection_matrix);
	light_shader->setUniform("u_iRes", vec2(1.0f / (float)illumination_fbo->width, 1.0f / (float)illumination_fbo->height));
	light_shader->setUniform("u_shininess", 32.0f);

	for (int i = 0; i < light_list.size(); ++i) {
		if (light_list[i]->light_type == eLightType::DIRECTIONAL) continue;

		LightEntity* light = light_list[i];
		Matrix44 light_model;
		Vector3f p = light->root.getGlobalMatrix().getTranslation();
		light_model.setTranslation(p.x, p.y, p.z);
		light_model.scale(light->max_distance, light->max_distance, light->max_distance);

		light_shader->setUniform("u_model", light_model);
		light_shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
		
		light_shader->setUniform("u_light_position", light->root.getGlobalMatrix().getTranslation());
		light_shader->setUniform("u_light_color", light->color * light->intensity);
		light_shader->setUniform("u_light_type", (int)light->light_type);
		light_shader->setUniform("u_light_direction", light->root.getGlobalMatrix().frontVector());
		light_shader->setUniform("u_light_cone_info", vec2(light->cone_info.x * DEG2RAD, light->cone_info.y * DEG2RAD));
		light_shader->setUniform("u_max_distance", light->max_distance);

		if (shadow_fbos[i] && shadow_fbos[i]->depth_texture) {
			light_shader->setUniform1("u_has_shadow_map", 1);
			light_shader->setUniform("u_shadow_bias", 0.005f);
			light_shader->setUniform("u_shadow_vp", shadow_viewprojections[i]);
			light_shader->setUniform("u_shadow_map", shadow_fbos[i]->depth_texture, 3);
		} else {
			light_shader->setUniform1("u_has_shadow_map", 0);
		}

		sphere.render(GL_TRIANGLES);
	}

	light_shader->disable();

	// Restaurar estado OpenGL
	glDisable(GL_DEPTH_CLAMP);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	glFrontFace(GL_CCW);
	glDisable(GL_BLEND);

	// T5.1: visible expanding bubble (rendered into HDR buffer, depth-tested)
	renderScannerSphere(camera);

	illumination_fbo->unbind();

	// ==========================================
	//RENDER FORWARD DE TRANSPARENCIAS
	// ==========================================

	// 1. Tonemap HDR + gamma a pantalla (3.3)
	glClearColor(0.0, 0.0, 0.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	GFX::Shader* tonemap_shader = GFX::Shader::Get("tonemap");
	if (tonemap_shader) {
		tonemap_shader->enable();
		tonemap_shader->setUniform("u_exposure", tonemap_exposure);
		illumination_fbo->color_textures[0]->toViewport(tonemap_shader);
	}

	// Pase de post-procesado: efecto Sci-Fi Scan (se aplica sobre el color ya tonemapeado)
	if (scanner_method == SCANNER_METHOD_A && scan_active) {
		renderScifiScan(camera);
	}
	
	// 2. Copiamos la profundidad usando un shader en lugar de glBlitFramebuffer
	// ya que el glBlitFramebuffer al FBO por defecto (0) falla por incompatibilidad
	// de formato. Esto permite que las transparencias se ocluyan por los opacos.
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_ALWAYS);
	glColorMask(false, false, false, false);
	GFX::Shader* depth_write = GFX::Shader::Get("depth_write");
	if (depth_write) {
		depth_write->enable();
		depth_write->setUniform("u_depth_texture", gbuffer_fbo->depth_texture, 0);
		GFX::Mesh::getQuad()->render(GL_TRIANGLES);
		depth_write->disable();
	}
	glColorMask(true, true, true, true);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE); // Asegurar que podemos escribir profundidad

	// 3. Dibujar transparencias DIRECTAMENTE en el viewport
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	for (int i = 0; i < render_list.size(); i++) {
		if (render_list[i].material->alpha_mode == SCN::eAlphaMode::BLEND) {
			renderMeshWithMaterialForward(render_list[i].matrix, render_list[i].mesh, render_list[i].material);
		}
	}
	glDisable(GL_BLEND);
}

// =============================================================================
// EFECTO SCI-FI SCAN — PASE DE POST-PROCESADO
// Este método se ejecuta DESPUÉS del tonemap, aplicando el efecto encima
// del color final de la escena usando el depth buffer del G-Buffer.
//
// CAPA 1: Reconstrucción de World Position (base invisible del efecto)
// CAPA 2: Oscurecimiento del terreno (Darken Blend)
//   → Aplica min(sceneColor, darkenColor) en el disco del scanner.
//   → Prepara el lienzo para que las líneas holográficas sean legibles.
// =============================================================================
void Renderer::renderScifiScan(Camera* camera)
{
    if (!scan_active) return;

    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    int width  = viewport[2];
    int height = viewport[3];

    // Crear (o recrear si cambia la resolución) el FBO que captura el color de pantalla
    if (scan_fbo == nullptr ||
        scan_fbo->color_textures[0]->width  != width ||
        scan_fbo->color_textures[0]->height != height)
    {
        if (scan_fbo) delete scan_fbo;
        scan_fbo = new GFX::FBO();
        scan_fbo->create(width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, false);
    }

    // --- CAPTURA: copiar el framebuffer actual (color de la escena) al FBO ---
    // Usamos glBlitFramebuffer para copiar el color del framebuffer por defecto
    // al FBO de captura. Así el shader puede leerlo como textura.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scan_fbo->fbo_id);
    glBlitFramebuffer(
        0, 0, width, height,
        0, 0, width, height,
        GL_COLOR_BUFFER_BIT, GL_NEAREST
    );
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // --- RENDER: ejecutar el shader scifi_scan sobre un quad de pantalla completa ---
    GFX::Shader* scan_shader = GFX::Shader::Get("scifi_scan");
    if (!scan_shader) return;

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    scan_shader->enable();

    // CAPA 1: Uniforms para la reconstrucción de world position
    scan_shader->setUniform("u_depth_texture",          gbuffer_fbo->depth_texture,       0);
    scan_shader->setUniform("u_scene_color_texture",    scan_fbo->color_textures[0],       1);
    scan_shader->setUniform("u_inverse_viewprojection", camera->inverse_viewprojection_matrix);
    scan_shader->setUniform("u_iRes",  vec2(1.0f / (float)width, 1.0f / (float)height));

    // Uniforms del estado del scanner
    scan_shader->setUniform("u_scan_origin", scan_origin);
    scan_shader->setUniform("u_scan_radius", scan_radius);
    scan_shader->setUniform("u_scan_active", scan_active ? 1.0f : 0.0f);
    // PASO 6: Animación — opacidad global, círculo de carga y ancho del rastro
    scan_shader->setUniform("u_scan_opacity",       scan_opacity);
    scan_shader->setUniform("u_charge_radius",      scan_charge_radius);
    scan_shader->setUniform("u_trail_width",        scan_trail_width);

    // CAPA 2: Oscurecimiento del terreno
    // Azul marino muy oscuro — hace que las líneas azules y blancas resalten
    scan_shader->setUniform("u_darken_color",    vec3(0.02f, 0.05f, 0.12f));
    scan_shader->setUniform("u_darken_strength", 0.85f); // 85% de oscurecimiento

    // CAPA 3: Frente de onda (Edge Gradient)
    // Escala menor: borde de 0.8m
    scan_shader->setUniform("u_edge_width",     0.8f);
    scan_shader->setUniform("u_edge_color",     vec3(0.1f, 0.6f, 1.0f));
    scan_shader->setUniform("u_edge_intensity", 1.5f);

    // CAPA 4: Líneas holográficas (frac sobre distancia World Space)
    // Escala menor: líneas cada 1.2 metros, grosor 0.04m
    scan_shader->setUniform("u_line_interval",  1.2f);
    scan_shader->setUniform("u_line_width",     0.04f);
    scan_shader->setUniform("u_line_color",     vec3(0.0f, 0.5f, 1.0f));
    scan_shader->setUniform("u_line_intensity", 2.0f);

    // CAPA 5: Primera línea blanca (Visual Cue)
    // Más brillante que las interiores para que destaque como el "frente de avance"
    scan_shader->setUniform("u_leading_intensity", 3.5f);

    // Renderizar el quad de pantalla completa → el shader se ejecuta una vez por píxel
    GFX::Mesh::getQuad()->render(GL_TRIANGLES);

    scan_shader->disable();

    glEnable(GL_DEPTH_TEST);
}

void Renderer::renderMeshWithMaterialForward(const Matrix44 model, GFX::Mesh* mesh, SCN::Material* material) {
	if (!mesh || !mesh->getNumVertices() || !material) return;
	GFX::Shader* shader = GFX::Shader::Get("phong");
	if (!shader) return;
	
	shader->enable();
	material->bind(shader);
	shader->setUniform("u_model", model);
	shader->setUniform("u_viewprojection", Camera::current->viewprojection_matrix);
	shader->setUniform("u_camera_position", Camera::current->eye);
	shader->setUniform("u_time", (float)getTime());
	shader->setUniform("u_ambient_light", scene->ambient_light);
	shader->setUniform1("u_show_normals", show_normals ? 1 : 0);

	int num_lights = (int)light_list.size() < MAX_LIGHTS ? (int)light_list.size() : MAX_LIGHTS;
	Vector3f light_positions[MAX_LIGHTS];
	Vector3f light_colors[MAX_LIGHTS];
	Vector3f light_directions[MAX_LIGHTS];
	int light_types[MAX_LIGHTS];
	Vector2f light_cone_info[MAX_LIGHTS];

	for (int i = 0; i < num_lights; i++) {
		light_positions[i] = light_list[i]->root.getGlobalMatrix().getTranslation();
		light_colors[i] = light_list[i]->color * light_list[i]->intensity;
		light_types[i] = light_list[i]->light_type;
		light_directions[i] = light_list[i]->root.getGlobalMatrix().frontVector();
		light_cone_info[i] = Vector2f(light_list[i]->cone_info.x * DEG2RAD, light_list[i]->cone_info.y * DEG2RAD);
	}

	shader->setUniform1("u_has_shadow_map", 1);
	shader->setMatrix44Array("u_shadow_vps", &shadow_viewprojections[0], MAX_LIGHTS);
	shader->setUniform("u_shadow_bias", 0.01f);
	for (int i = 0; i < num_lights; i++) {
		char var_name[64];
		sprintf(var_name, "u_shadow_maps[%d]", i);
		if (shadow_fbos[i] && shadow_fbos[i]->depth_texture)
			shader->setUniform(var_name, shadow_fbos[i]->depth_texture, 3 + i);
	}

	if (material->alpha_mode == SCN::eAlphaMode::BLEND) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	} else {
		glDisable(GL_BLEND);
	}

	shader->setUniform3Array("u_light_position", &light_positions[0].x, num_lights);
	shader->setUniform3Array("u_light_colors", &light_colors[0].x, num_lights);
	shader->setUniform1("u_num_lights", num_lights);
	shader->setUniform1Array("u_light_types", &light_types[0], num_lights);
	shader->setUniform3Array("u_light_directions", &light_directions[0].x, num_lights);
	shader->setUniform2Array("u_light_cone_info", &light_cone_info[0].x, num_lights);

	if (render_wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	mesh->render(GL_TRIANGLES);
	if (render_wireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	
	shader->disable();
	glDisable(GL_BLEND);
}

#ifndef SKIP_IMGUI

void Renderer::showUI()
{
		
	ImGui::Checkbox("Wireframe", &render_wireframe);
	ImGui::Checkbox("Boundaries", &render_boundaries);
	ImGui::Checkbox("Enable Normal Maps", &show_normals);
	ImGui::Checkbox("Single Pass", &single_pass);
	ImGui::Checkbox("Use Deferred", &use_deferred);

	// === SSAO Controls ===
	if (ImGui::TreeNode("SSAO")) {
		ImGui::Checkbox("Enable SSAO", &enable_ssao);

		// Si cambian las muestras o el modo hemisferio, regeneramos los puntos
		bool changed = false;
		changed |= ImGui::SliderInt("Samples", &ssao_num_samples, 1, 64);
		changed |= ImGui::Checkbox("Hemisphere (SSAO+)", &ssao_hemisphere);
		if (changed) {
			ssao_sample_points = generateSpherePoints(ssao_num_samples, 1.0f, ssao_hemisphere);
		}

		ImGui::SliderFloat("Radius", &ssao_radius, 0.001f, 0.2f);
		ImGui::TreePop();
	}

	if (ImGui::TreeNode("HDR / Tonemap")) {
		ImGui::SliderFloat("Exposure", &tonemap_exposure, 0.1f, 8.0f);
		ImGui::TreePop();
	}

	// === SCI-FI SCANNER (Unified) ===
	if (ImGui::TreeNode("Sci-Fi Scanner (Unified)")) {
		const char* methods[] = { "Method A: Screen-Space Post-Process", "Method B: Geometric Mesh Expansion" };
		int current_method = (int)scanner_method;
		if (ImGui::Combo("Scanner Method", &current_method, methods, 2)) {
			scanner_method = (eScannerMethod)current_method;
		}

		ImGui::Separator();

		if (scanner_method == SCANNER_METHOD_A) {
			ImGui::Text("Method A Status: %s", scan_active ? "ACTIVE" : "INACTIVE");
			
			if (ImGui::Button("Trigger Scan (Method A)")) {
				triggerScanner(Camera::current->eye);
			}
			ImGui::SameLine();
			if (ImGui::Button("Deactivate")) {
				scan_active = false;
				scan_radius = 0.0f;
				scan_time   = 0.0f;
			}

			if (scan_active) {
				ImGui::Separator();
				ImGui::Text("--- Simulation Controls ---");
				
				ImGui::Checkbox("Manual Mode (Timeline scrubbing)", &scan_manual_mode);
				
				if (scan_manual_mode) {
					ImGui::SliderFloat("Timeline (s)", &scan_time, 0.0f, 3.0f);
					ImGui::Text("Manual scrubbing active.");
				} else {
					if (scan_paused) {
						if (ImGui::Button("Resume (Play)")) {
							scan_paused = false;
							last_scan_frame_time = getTime();
						}
					} else {
						if (ImGui::Button("Pause")) {
							scan_paused = true;
						}
					}
					
					float progress_fraction = scan_time / 3.0f;
					ImGui::Text("Auto Progress:");
					ImGui::ProgressBar(progress_fraction > 1.0f ? 1.0f : progress_fraction, ImVec2(-1, 0));
				}
				
				ImGui::Separator();
				ImGui::Text("--- Physical / Visual Parameters ---");
				ImGui::SliderFloat("Trail Width (m)", &scan_trail_width, 1.0f, 30.0f);
				
				if (scan_manual_mode) {
					ImGui::SliderFloat("Wave Radius (m)", &scan_radius, 0.0f, scan_max_radius);
					ImGui::SliderFloat("Effect Opacity", &scan_opacity, 0.0f, 1.0f);
					ImGui::SliderFloat("Charge Radius (m)", &scan_charge_radius, 0.0f, 5.0f);
				} else {
					ImGui::Text("Calculated Radius: %.2f m", scan_radius);
					ImGui::Text("Calculated Opacity: %.2f", scan_opacity);
					ImGui::Text("Calculated Charge: %.2f m", scan_charge_radius);
				}
			}
		}
		else if (scanner_method == SCANNER_METHOD_B) {
			ImGui::Checkbox("Enable Scanner (Method B)", &enable_scanner);
			ImGui::Text("SPACE = trigger pulse");
			ImGui::Text("Pulse: %s", scanner_active ? "running" : "idle");
			if (ImGui::Button("Reset Defaults"))
				resetScannerDefaults();
			if (ImGui::Button("Trigger Scan (Method B)")) {
				triggerScanner(Camera::current->eye);
			}
			ImGui::SliderFloat("Max Radius", &scanner_max_radius, 5.0f, 100.0f);
			ImGui::SliderFloat("Duration", &scanner_duration, 1.0f, 15.0f);
			ImGui::ColorEdit3("Color", &scanner_color.x);
			ImGui::SliderFloat("Intensity", &scanner_intensity, 0.5f, 8.0f);
			ImGui::Text("Radius: %.2f  Elapsed: %.2fs", scanner_radius, scanner_elapsed);

			ImGui::SeparatorText("Sphere Intersection (Mesh Expansion)");
			ImGui::Checkbox("Show Sphere Mesh", &scanner_show_sphere);
			ImGui::SliderFloat("Ground Height (Y)", &scanner_ground_height, -10.0f, 20.0f);
			ImGui::SliderInt("Sub-lines (rings)", &scanner_ring_count, 1, 12);
			ImGui::SliderFloat("Ring Spacing", &scanner_ring_spacing, 0.5f, 5.0f);
			ImGui::SliderFloat("Contact Thickness", &scanner_contact_thickness, 0.05f, 2.0f);
			ImGui::SliderFloat("Shell Strength", &scanner_rim_strength, 0.0f, 0.5f);
			ImGui::SliderFloat("Shell Falloff", &scanner_sphere_rim, 0.5f, 6.0f);
			ImGui::SliderFloat("Opacity", &scanner_sphere_alpha, 0.0f, 1.0f);
		}

		ImGui::TreePop();
	}
}

#else
void Renderer::showUI() {}
#endif