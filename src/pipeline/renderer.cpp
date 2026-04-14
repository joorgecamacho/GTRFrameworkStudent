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


using namespace SCN;

//some globals
GFX::Mesh sphere;

Renderer::Renderer(const char* shader_atlas_filename)
{
	render_wireframe = false;
	render_boundaries = false;
	single_pass_mode = true;
	scene = nullptr;
	skybox_cubemap = nullptr;

	if (!GFX::Shader::LoadAtlas(shader_atlas_filename))
		exit(1);
	GFX::checkGLErrors();

	sphere.createSphere(1.0f);
	sphere.uploadToVRAM();
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

	// Upload scene/light uniforms for multi-light phong
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


	// Upload time, for cool shader effects
	float t = getTime();
	shader->setUniform("u_time", t );

	// Render just the verticies as a wireframe
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

	//add here your stuff
	//...
}

#else
void Renderer::showUI() {}
#endif