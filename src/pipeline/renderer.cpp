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

#include "scene.h"
#include <cmath>


using namespace SCN;

//some globals
GFX::Mesh sphere;

Renderer::Renderer(const char* shader_atlas_filename)
{
	render_wireframe = false;
	render_boundaries = false;
	single_pass_mode = true;
	atlas_resolution = 4096;
	atlas_tile_size = 1024;
	shadow_bias = 0.0015f;
	shadow_cull_front_faces = true;
	shadowmap_fbo = nullptr;
	shadow_camera = nullptr;
	scene = nullptr;
	skybox_cubemap = nullptr;

	if (!GFX::Shader::LoadAtlas(shader_atlas_filename))
		exit(1);
	GFX::checkGLErrors();

	shadowmap_fbo = new GFX::FBO();
	if (!shadowmap_fbo->setDepthOnly(atlas_resolution, atlas_resolution))
	{
		delete shadowmap_fbo;
		shadowmap_fbo = nullptr;
	}
	else if (shadowmap_fbo->depth_texture) {
		shadowmap_fbo->depth_texture->setName("ShadowMap_Depth");
	}
	shadow_camera = new Camera();

	sphere.createSphere(1.0f);
	sphere.uploadToVRAM();
}

Renderer::~Renderer()
{
	if (shadowmap_fbo)
	{
		delete shadowmap_fbo;
		shadowmap_fbo = nullptr;
	}
	if (shadow_camera)
	{
		delete shadow_camera;
		shadow_camera = nullptr;
	}
}



void Renderer::renderShadowMap()
{
	if (!shadowmap_fbo || !shadow_camera)
		return;

	GFX::Shader* shadow_shader = GFX::Shader::Get("flat");
	if (!shadow_shader)
		return;

	GLint previous_viewport[4];
	glGetIntegerv(GL_VIEWPORT, previous_viewport);
	Camera* previous_camera = Camera::current;
	GLboolean was_cull_enabled = glIsEnabled(GL_CULL_FACE);
	GLint previous_cull_face_mode = GL_BACK;
	glGetIntegerv(GL_CULL_FACE_MODE, &previous_cull_face_mode);

	shadowmap_fbo->bind();
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glClear(GL_DEPTH_BUFFER_BIT);

	int tiles_per_row = atlas_resolution / atlas_tile_size;

	for (int i = 0; i < 16; ++i) {
		shadow_atlas_rects[i] = Vector4f(0, 0, 0, 0);
		shadow_viewprojections[i].setIdentity();
	}

	for (int i = 0; i < light_list.size() && i < 16; ++i)
	{
		LightEntity* shadow_light = light_list[i];
		if (!shadow_light || !shadow_light->cast_shadows || shadow_light->intensity == 0.0f)
			continue;

		Vector3f light_pos = shadow_light->root.model.getTranslation();
		Vector3f light_dir = shadow_light->root.model.frontVector().normalize();
		Vector3f target = light_pos + light_dir;
		Vector3f up = Vector3f(0.0f, 1.0f, 0.0f);
		float near_plane = (shadow_light->near_distance > 0.01f) ? shadow_light->near_distance : 0.01f;
		float min_far = near_plane + 0.01f;
		float far_plane = (shadow_light->max_distance > min_far) ? shadow_light->max_distance : min_far;

		if (shadow_light->light_type == SCN::eLightType::SPOT)
		{
			float full_fov_deg = shadow_light->cone_info.y * 2.0f;
			if (full_fov_deg < 1.0f) full_fov_deg = 1.0f;
			shadow_camera->lookAt(light_pos, target, up);
			shadow_camera->setPerspective(full_fov_deg, 1.0f, near_plane, far_plane);
		}
		else
		{
			// For directional lights we use the area property directly
			// without clamping - let the artist control it from the inspector
			float ortho_area = std::abs(shadow_light->area);
			if (ortho_area < 1.0f) ortho_area = 50.0f; // default fallback
			Vector3f focus = Camera::current ? Camera::current->center : Vector3f(0.0f, 0.0f, 0.0f);
			float light_distance = far_plane * 0.5f;
			light_pos = focus - light_dir * light_distance;
			shadow_camera->lookAt(light_pos, focus, up);
			shadow_camera->setOrthographic(-ortho_area, ortho_area, -ortho_area, ortho_area, near_plane, far_plane);
		}

		shadow_camera->enable();
		shadow_viewprojections[i] = shadow_camera->viewprojection_matrix;

		int tile_x = i % tiles_per_row;
		int tile_y = i / tiles_per_row;
		int vp_x = tile_x * atlas_tile_size;
		int vp_y = tile_y * atlas_tile_size;

		glViewport(vp_x, vp_y, atlas_tile_size, atlas_tile_size);
		glEnable(GL_SCISSOR_TEST);
		glScissor(vp_x, vp_y, atlas_tile_size, atlas_tile_size);

		shadow_atlas_rects[i] = Vector4f((float)vp_x / atlas_resolution, (float)vp_y / atlas_resolution, (float)atlas_tile_size / atlas_resolution, (float)atlas_tile_size / atlas_resolution);

		shadow_shader->enable();
		for (const sRenderable& renderable : render_list)
		{
			if (!renderable.mesh || !renderable.material)
				continue;
			if (renderable.material->alpha_mode == SCN::eAlphaMode::BLEND)
				continue;

			renderable.material->bind(shadow_shader);
			if (shadow_cull_front_faces) {
				glEnable(GL_CULL_FACE);
				glCullFace(GL_FRONT);
			} else {
				glDisable(GL_CULL_FACE);
			}

			shadow_shader->setUniform("u_model", renderable.matrix);
			shadow_shader->setUniform("u_viewprojection", shadow_camera->viewprojection_matrix);
			renderable.mesh->render(GL_TRIANGLES);
		}
	}
	shadow_shader->disable();
	shadowmap_fbo->unbind();

	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	if (was_cull_enabled) glEnable(GL_CULL_FACE);
	else glDisable(GL_CULL_FACE);
	glCullFace(previous_cull_face_mode);
	glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
	if (previous_camera) previous_camera->enable();
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
		/*// Tarea 3.5: Frustum culling (EXTRA)
		// 1. Calculamos la BoundingBox en coordenadas de munto (World Space) usando la matriz global del nodo
		BoundingBox global_aabb = transformBoundingBox(node->getGlobalMatrix(), node->mesh->box);

		// 2. Comprobamos si choca con el frustum de la cámara. 
		// Si devuelve CLIP_OUTSIDE (0), significa que no se ve y lo ignoramos.
		if (Camera::current->testBoxInFrustum(global_aabb.center, global_aabb.halfsize) != CLIP_OUTSIDE) {
			render_list.push_back({
				node->mesh,
				node->material,
				node->getGlobalMatrix(),
				node->getGlobalMatrix().getTranslation().distance(Camera::current->eye)
			});*/	
	if (node->mesh && node->material) {
		// For Assignment 2 we keep scene parsing robust and always register renderables.
		// Frustum culling can be re-enabled later if needed.
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
	
	//iteramos en todas las entidades de la escena
	for (int i = 0; i < scene->entities.size(); i++) {
		BaseEntity* entity = scene->entities[i];

		//si la entidad no es visible no la renderizamos
		if (!entity->visible) {
			continue;
		}
		//LAB1 ASSIG 1: si la entidad es un prefab, un objeti que ya exisitia, lo cogemos y metemos todos sus nodos en la lista de objetos a renderizar
		if(entity->getType() == SCN::eEntityType::PREFAB){
			//Convertimos el entity base a PrefabEntity
			PrefabEntity* prefabEntity = (PrefabEntity*)entity; 
			//Empezamos la magia pasándole la raíz y la cámara
			parseNode(&prefabEntity->root); 
		}
		//EJERCICIO 3.1 DE LAB 1 ASSIGNMENT 2: si la entudad es una luz la metemos en la lista de luces
		if (entity->getType() == SCN::eEntityType::LIGHT) {
			LightEntity* light = (LightEntity*)entity;
			if (light->light_type != SCN::eLightType::NO_LIGHT && light->intensity > 0.0f) {
				light_list.push_back(light);
			}
		}

		// Store Prefab Entitys
		// ...
		//		Store Children Prefab Entities

		// Store Lights
		// ...
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
	renderShadowMap();

	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	GFX::checkGLErrors();

	//render skybox
	if(skybox_cubemap)
		renderSkybox(skybox_cubemap);

	// HERE =====================
	// TODO: RENDER RENDERABLES
	// ==========================
	for (int i = 0; i < render_list.size(); i++) {
		renderMeshWithMaterial(render_list[i].matrix, render_list[i].mesh, render_list[i].material);
	}
}


void Renderer::renderSkybox(GFX::Texture* cubemap)
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
    assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	GFX::Shader* shader = NULL;
	Camera* camera = Camera::current;

	glEnable(GL_DEPTH_TEST);

	//chose a shader
	if (single_pass_mode) {
		shader = GFX::Shader::Get("phong_single");
	} else {
		shader = GFX::Shader::Get("texture");
	}

	//no shader? then nothing to render
	if (!shader)
		return;
	shader->enable();

	material->bind(shader);

	//upload uniforms
	shader->setUniform("u_model", model);

	// Upload camera uniforms
	shader->setUniform("u_viewprojection", camera->viewprojection_matrix);
	shader->setUniform("u_camera_position", camera->eye);

	//part of assigment 2 upload light information to the shader
	if (shader)
	{
		const int MAX_LIGHTS = 16;
		shader->setUniform("u_ambient_light", scene ? scene->ambient_light : Vector3f(0.1f, 0.1f, 0.1f));

		int light_count = (int)light_list.size();
		if (light_count > MAX_LIGHTS)
			light_count = MAX_LIGHTS;
		shader->setUniform("u_num_lights", light_count);

		if (light_count > 0)
		{
			std::vector<float> light_positions;
			std::vector<float> light_colors;
			std::vector<float> light_intensities;
			std::vector<float> light_directions;
			std::vector<int> light_types;
			std::vector<float> light_cones;
			light_positions.reserve(light_count * 3);
			light_colors.reserve(light_count * 3);
			light_intensities.reserve(light_count);
			light_directions.reserve(light_count * 3);
			light_types.reserve(light_count);
			light_cones.reserve(light_count * 2);

			for (int i = 0; i < light_count; ++i)
			{
				SCN::LightEntity* light = light_list[i];
				Vector3f light_pos = light->root.model.getTranslation();
				light_positions.push_back(light_pos.x);
				light_positions.push_back(light_pos.y);
				light_positions.push_back(light_pos.z);

				light_colors.push_back(light->color.x);
				light_colors.push_back(light->color.y);
				light_colors.push_back(light->color.z);

				light_intensities.push_back(light->intensity);

				Vector3f light_dir = light->root.model.frontVector();
				light_directions.push_back(light_dir.x);
				light_directions.push_back(light_dir.y);
				light_directions.push_back(light_dir.z);

				light_types.push_back((int)light->light_type);

				//cone angles 
				light_cones.push_back(light->cone_info.x * DEG2RAD); //inner angle
				light_cones.push_back(light->cone_info.y * DEG2RAD); //outer angle
			}

			shader->setUniform3Array("u_light_positions", light_positions.data(), light_count);
			shader->setUniform3Array("u_light_colors", light_colors.data(), light_count);
			shader->setUniform1Array("u_light_intensities", light_intensities.data(), light_count);
			shader->setUniform3Array("u_light_directions", light_directions.data(), light_count);
			shader->setUniform1Array("u_light_types", light_types.data(), light_count);
			shader->setUniform2Array("u_light_cones", light_cones.data(), light_count);
		}
	}

	//assignment 3.3 send shadow map and light camera 
	if (single_pass_mode)
	{
		bool shadow_enabled = shadowmap_fbo && shadowmap_fbo->depth_texture && shadow_camera;
		shader->setUniform("u_shadow_enabled", shadow_enabled ? 1 : 0);
		if (shadow_enabled)
		{
			shader->setUniform("u_shadowmap", shadowmap_fbo->depth_texture, 7);
			shader->setMatrix44Array("u_light_viewprojections", shadow_viewprojections, 16);
			shader->setUniform4Array("u_light_atlas_rects", (float*)shadow_atlas_rects, 16);
			shader->setUniform("u_shadow_bias", shadow_bias);
		}
	}


	//upload time for shader effects
	float t = getTime();
	shader->setUniform("u_time", t );

	if (render_wireframe)
		glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );

	//do the draw call that renders the mesh into the screen
	mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
}

#ifndef SKIP_IMGUI

void Renderer::showUI()
{
		
	ImGui::Checkbox("Wireframe", &render_wireframe);
	ImGui::Checkbox("Boundaries", &render_boundaries);
	ImGui::Checkbox("Single Pass Mode", &single_pass_mode);
	ImGui::DragFloat("Shadow Bias", &shadow_bias, 0.0001f, 0.0f, 0.02f, "%.5f");
	ImGui::Checkbox("Shadow Cull Front Faces", &shadow_cull_front_faces);

	//add here your stuff
	//...
}

#else
void Renderer::showUI() {}
#endif