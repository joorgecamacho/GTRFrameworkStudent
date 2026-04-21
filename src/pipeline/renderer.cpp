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

	// Shadow mapping defaults
	shadowmap_resolution = 1024;
	shadow_light_index = 3;        // 3 = directional (moonlight) per assignment 3.2.1
	shadow_bias = 0.0005f;         // 3.4.1
	shadow_front_face_cull = true; // 3.4.2: enabled by default (slide recommendation)
	shadowmap_fbo = nullptr;
	shadow_camera = nullptr;
	shadow_light = nullptr;

	scene = nullptr;
	skybox_cubemap = nullptr;

	if (!GFX::Shader::LoadAtlas(shader_atlas_filename))
		exit(1);
	GFX::checkGLErrors();

	shadowmap_fbo = new GFX::FBO();
	if (!shadowmap_fbo->setDepthOnly(shadowmap_resolution, shadowmap_resolution))
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

// Assignment 3.2.1: Configure the "light camera".
// Build view + projection matrices for the light currently at shadow_light_index.
// We follow the slides: lookAt with the light position+forward, then either
// setPerspective (spotlight, FOV = cone_info.y*2) or setOrthographic (directional,
// half_size = area/2) using the light's near_distance / max_distance.
void Renderer::updateShadowCamera()
{
	shadow_light = nullptr;
	if (!shadow_camera)
		return;
	if (shadow_light_index < 0 || shadow_light_index >= (int)light_list.size())
		return;

	LightEntity* light = light_list[shadow_light_index];
	if (!light)
		return;
	if (light->light_type != SCN::eLightType::SPOT &&
		light->light_type != SCN::eLightType::DIRECTIONAL)
		return; // pointlights are skipped per the slides

	// Check the cast_shadows property from the scene entity
	if (!light->cast_shadows)
		return;

	shadow_light = light;

	// Position & direction from the light entity's transform (like the slide).
	// Important: in this framework Matrix44::frontVector() returns the local +Z
	// axis of the transform, but the convention (same as cameras) is that the
	// light shines towards local -Z. The slide uses:
	//     lookAt(light_pos, light_mat * vec3(0,0,-1), up)
	// which is exactly "light_pos - frontVector()" in world space.
	Matrix44 light_model = light->root.model;
	Vector3f light_pos = light_model.getTranslation();
	Vector3f light_front = light_model.frontVector();
	light_front.normalize();
	Vector3f light_target = light_pos - light_front; // <-- the light shines towards -Z local
	Vector3f up(0.0f, 1.0f, 0.0f);

	shadow_camera->lookAt(light_pos, light_target, up);

	// Safe near/far from the light entity
	float near_p = light->near_distance > 0.01f ? light->near_distance : 0.01f;
	float far_p = light->max_distance;
	if (far_p < near_p + 0.01f)
		far_p = near_p + 10.0f;

	if (light->light_type == SCN::eLightType::SPOT)
	{
		// cone_info.y is the HALF cone angle in degrees -> full cone FOV
		float fov_deg = light->cone_info.y * 2.0f;
		if (fov_deg < 1.0f)
			fov_deg = 1.0f;
		float aspect = 1.0f; // shadow FBO is square
		shadow_camera->setPerspective(fov_deg, aspect, near_p, far_p);
	}
	else // DIRECTIONAL
	{
		// half_size = area / 2, using fabs to be robust against negative area
		// values stored in the scene JSON.
		float half_size = std::fabs(light->area) * 0.5f;
		if (half_size < 0.1f)
			half_size = 10.0f;
		shadow_camera->setOrthographic(-half_size, half_size,
			-half_size, half_size,
			near_p, far_p);
	}
}

// Assignment 3.2.2: Render the scene to the shadow map.
// Follow the slides exactly:
//   1) bind the FBO
//   2) disable color writes
//   3) clear depth buffer
//   4) enable light camera, iterate renderables with a cheap "flat" shader
//      (skipping BLEND materials, keeping opaque + MASK alpha-cutoff)
//   5) unbind FBO and restore the previous GL state
void Renderer::renderShadowMap()
{
	if (!shadowmap_fbo || !shadow_camera || !shadow_light)
		return;

	GFX::Shader* shader = GFX::Shader::Get("flat");
	if (!shader)
		return;

	// --- Save the state we are about to touch, to restore it at the end ---
	GLint prev_viewport[4];
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	Camera* prev_camera = Camera::current;
	GLboolean prev_cull_enabled = glIsEnabled(GL_CULL_FACE);
	GLint prev_front_face = GL_CCW;
	glGetIntegerv(GL_FRONT_FACE, &prev_front_face);

	// --- 1) Bind FBO + configure render target ---
	shadowmap_fbo->bind();
	glViewport(0, 0, shadowmap_resolution, shadowmap_resolution);

	// --- 2) disable color writes, only depth matters ---
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

	// --- 3) clean previous frame's depth ---
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glClear(GL_DEPTH_BUFFER_BIT);

	// --- 3.4.2: Front Face Culling ---
	// Enable culling and flip the front-face winding so that back faces are
	// the ones being rendered. This adds a self-adjusting bias and hides
	// the acne on the geometry that is actually lit by the shadow-casting
	// light. The state is restored below once the shadow map is finished.
	if (shadow_front_face_cull)
	{
		glEnable(GL_CULL_FACE);
		glFrontFace(GL_CW);
	}

	// --- 4) render the scene from the light camera with the flat shader ---
	shadow_camera->enable();
	shader->enable();

	for (const sRenderable& r : render_list)
	{
		if (!r.mesh || !r.material)
			continue;

		// Only opaque + alpha-cutoff (MASK). BLEND is skipped because the
		// depth buffer does not work with transparency.
		if (r.material->alpha_mode == SCN::eAlphaMode::BLEND)
			continue;

		// Bind material so flat.fs can optionally discard by alpha cutoff
		// (needed for tree leaves / signs).
		r.material->bind(shader);
		shader->setUniform("u_model", r.matrix);
		shader->setUniform("u_viewprojection", shadow_camera->viewprojection_matrix);

		r.mesh->render(GL_TRIANGLES);
	}

	shader->disable();

	// --- 5) unbind and restore everything (including 3.4.2 cull state) ---
	shadowmap_fbo->unbind();
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glFrontFace(prev_front_face);
	if (prev_cull_enabled)
		glEnable(GL_CULL_FACE);
	else
		glDisable(GL_CULL_FACE);
	glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
	if (prev_camera)
		prev_camera->enable();
}

void Renderer::setupScene()
{
	if (scene->skybox_filename.size())
		skybox_cubemap = GFX::Texture::Get(std::string(scene->base_folder + "/" + scene->skybox_filename).c_str());
	else
		skybox_cubemap = nullptr;
}

void Renderer::parseNode(SCN::Node* node) {
	if (!node) return;
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
		if (entity->getType() == SCN::eEntityType::PREFAB) {
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
	updateShadowCamera();
	renderShadowMap();

	//set the clear color (the background color)
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);

	// Clear the color and the depth buffer
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	GFX::checkGLErrors();

	//render skybox
	if (skybox_cubemap)
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
	if (!mesh || !mesh->getNumVertices() || !material)
		return;
	assert(glGetError() == GL_NO_ERROR);

	//define locals to simplify coding
	GFX::Shader* shader = NULL;
	Camera* camera = Camera::current;

	glEnable(GL_DEPTH_TEST);

	//chose a shader
	if (single_pass_mode) {
		shader = GFX::Shader::Get("phong_single");
	}
	else {
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

	//assignment 3.3 send shadow map and light camera (only used when 3.2 is working)
	if (single_pass_mode)
	{
		bool shadow_enabled = shadowmap_fbo && shadowmap_fbo->depth_texture && shadow_camera && shadow_light;
		shader->setUniform("u_shadow_enabled", shadow_enabled ? 1 : 0);
		shader->setUniform("u_shadow_light_index", shadow_enabled ? shadow_light_index : -1);
		if (shadow_enabled)
		{
			shader->setUniform("u_shadowmap", shadowmap_fbo->depth_texture, 7);
			shader->setUniform("u_light_viewprojection", shadow_camera->viewprojection_matrix);
			shader->setUniform("u_shadow_bias", shadow_bias);
		}
	}


	//upload time for shader effects
	float t = getTime();
	shader->setUniform("u_time", t);

	if (render_wireframe)
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

	//do the draw call that renders the mesh into the screen
	mesh->render(GL_TRIANGLES);

	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
}

#ifndef SKIP_IMGUI

void Renderer::showUI()
{

	ImGui::Checkbox("Wireframe", &render_wireframe);
	ImGui::Checkbox("Boundaries", &render_boundaries);
	ImGui::Checkbox("Single Pass Mode", &single_pass_mode);

	ImGui::Separator();
	ImGui::Text("Shadow mapping (3.2 / 3.3 / 3.4)");
	ImGui::DragInt("Shadow Light Index", &shadow_light_index, 1.0f, 0, 15);
	ImGui::Text("Casting light: %s", shadow_light ? "valid" : "none");
	ImGui::DragFloat("Shadow Bias (3.4.1)", &shadow_bias, 0.0001f, 0.0f, 0.05f, "%.5f");
	ImGui::Checkbox("Front Face Culling (3.4.2)", &shadow_front_face_cull);

	//add here your stuff
	//...
}

#else
void Renderer::showUI() {}
#endif