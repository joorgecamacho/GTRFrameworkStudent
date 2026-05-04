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
		// Tarea 3.5: Frustum culling (EXTRA)
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
			});
		}
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
	
	
	// ==========================================
	// ASSIGNMENT 4: TAREA 2.1: INICIALIZAR EL G-BUFFER
	// ==========================================
	// Obtenemos el tamaño actual de la pantalla leyendo el viewport de OpenGL
	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	int width = viewport[2];
	int height = viewport[3];

	// Si el FBO no existe, o si la ventana ha cambiado de tamaño, lo (re)creamos
	if (gbuffer_fbo == nullptr || gbuffer_fbo->color_textures[0]->width != width || gbuffer_fbo->color_textures[0]->height != height) {
		if (gbuffer_fbo) delete gbuffer_fbo;
		gbuffer_fbo = new GFX::FBO();

		// create(width, height, num_color_textures, format, type, use_depth)
		// Pedimos: 2 texturas (Color y Normales), formato RGBA (4 canales), de 8 bits (UNSIGNED_BYTE) y CON buffer de profundidad (true)
		gbuffer_fbo->create(width, height, 2, GL_RGBA, GL_UNSIGNED_BYTE, true);
	}
	// ==========================================



	// TAREA 3.2: Generar el Shadow Map de la luz antes de dibujar la escena final
	generateShadowMap();

	// ==========================================
	// ASSIGNMMENT 4: TAREA 2.2 (CPU): LLENAR EL G-BUFFER
	// ==========================================

	// 1. Redirigir el renderizado hacia nuestro G-Buffer
	gbuffer_fbo->bind();

	// 2. Limpiamos el G-Buffer (Color y Depth)
	// El clear color determina el color "vacío" de nuestra textura de Albedo
	glClearColor(scene->background_color.x, scene->background_color.y, scene->background_color.z, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	GFX::checkGLErrors();

	// 3. Renderizamos el Skybox (que irá al G-Buffer)
	if (skybox_cubemap)
		renderSkybox(skybox_cubemap);

	// 4. Renderizamos la geometría opaca
	for (int i = 0; i < render_list.size(); i++) {
		// En deferred, los objetos con transparencia (BLEND) se saltan en esta fase
		// y se dibujan más tarde.
		if (render_list[i].material->alpha_mode == SCN::eAlphaMode::BLEND) {
			continue;
		}
		renderMeshWithMaterial(render_list[i].matrix, render_list[i].mesh, render_list[i].material);
	}

	// 5. Dejamos de dibujar en el G-Buffer y volvemos a la pantalla
	gbuffer_fbo->unbind();

	// --- DEBUG: Mostrar el G-Buffer en pantalla para comprobar que funciona ---[cite: 1]
	// Esto es temporal, lo quitaremos en el paso 2.3
	glClearColor(0.0, 0.0, 0.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Mostramos la textura 0 (Color) a pantalla completa
	gbuffer_fbo->color_textures[0]->toViewport();
	// Puedes cambiar el índice a 1 para ver las normales: gbuffer_fbo->color_textures[1]->toViewport();
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
	shader = GFX::Shader::Get("gbuffer");
    assert(glGetError() == GL_NO_ERROR);

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

	//// Pasamos la matriz de la luz y el bias para el Shadow Acne
	//shader->setUniform("u_shadow_vp", this->shadow_viewprojection);
	//shader->setUniform("u_shadow_bias", 0.005f); // Un valor pequeñito para empezar

	//// Pasamos la textura de profundidad al SLOT 3 (el 0 es albedo, el 1 normales)
	//if (shadow_fbo && shadow_fbo->depth_texture) {
	//	shader->setUniform("u_shadow_map", shadow_fbo->depth_texture, 3);
	//}

	// Upload time, for cool shader effects
	float t = getTime();
	shader->setUniform("u_time", t );
	shader->setUniform("u_ambient_light",this->scene->ambient_light);

	Vector3f light_positions[MAX_LIGHTS];
	Vector3f light_colors[MAX_LIGHTS];
	Vector3f light_directions[MAX_LIGHTS];
	int light_types[MAX_LIGHTS];
	Vector2f light_cone_info[MAX_LIGHTS];

	int num_lights = (int)light_list.size() < MAX_LIGHTS ? (int)light_list.size() : MAX_LIGHTS;
	for (int i = 0; i < num_lights; i++) {
		light_positions[i] = light_list[i]->root.model.getTranslation();
		light_colors[i] = light_list[i]->color * light_list[i]->intensity;
		light_types[i] = light_list[i]->light_type;
		light_directions[i] = light_list[i]->root.model.frontVector();
		// Convertir ángulos del cono de grados a radianes
		light_cone_info[i] = Vector2f(
			light_list[i]->cone_info.x * DEG2RAD,
			light_list[i]->cone_info.y * DEG2RAD
		);
	}

	shader->setUniform1("u_show_normals", show_normals ? 1 : 0);

	// Render just the verticies as a wireframe
	if (render_wireframe)
		glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );

	if (single_pass) {
		// ================= SINGLE PASS DEFINITIVO =================
		shader->setUniform3("u_ambient_light", this->scene->ambient_light);

		// 1. Enviamos el aviso de que hay sombras y el array de matrices
		shader->setUniform1("u_has_shadow_map", 1);
		shader->setMatrix44Array("u_shadow_vps", &this->shadow_viewprojections[0], num_lights);
		shader->setUniform("u_shadow_bias", 0.005f);

		// 2. IMPORTANTE: Enviamos cada mapa de sombras a un slot diferente
		for (int i = 0; i < num_lights; i++) {
			char var_name[64];
			sprintf(var_name, "u_shadow_maps[%d]", i);
			if (shadow_fbos[i] && shadow_fbos[i]->depth_texture) {
				// Slot 3 + i (Luz 0 -> Slot 3, Luz 1 -> Slot 4...)
				shader->setUniform(var_name, shadow_fbos[i]->depth_texture, 3 + i);
			}
		}

		// 3. Blending y resto de luces (como ya lo tenías)
		if (material->alpha_mode == SCN::eAlphaMode::BLEND) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}
		else {
			glDisable(GL_BLEND);
		}

		shader->setUniform3Array("u_light_position", &light_positions[0].x, num_lights);
		shader->setUniform3Array("u_light_colors", &light_colors[0].x, num_lights);
		shader->setUniform1("u_num_lights", num_lights);
		shader->setUniform1Array("u_light_types", &light_types[0], num_lights);
		shader->setUniform3Array("u_light_directions", &light_directions[0].x, num_lights);
		shader->setUniform2Array("u_light_cone_info", &light_cone_info[0].x, num_lights);

		mesh->render(GL_TRIANGLES);
	}
	else {
		// ================= MULTI PASS =================
		if (num_lights == 0) {
			// Render without lights (only ambient)
			shader->setUniform1("u_num_lights", 0);
			shader->setUniform3("u_ambient_light", this->scene->ambient_light);
			if (material->alpha_mode == SCN::eAlphaMode::BLEND) {
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			}
			else {
				glDisable(GL_BLEND);
			}
			mesh->render(GL_TRIANGLES);
		}
		else {
			for (int i = 0; i < num_lights; i++) {
				if (i == 0) {
					// Primera pasada: Color Base + Ambiental + Primera Luz + SOMBRA
					shader->setUniform3("u_ambient_light", this->scene->ambient_light);

					// Le pasamos el Shadow Map porque esta es la luz que genera sombras
					if (shadow_fbos[i] && shadow_fbos[i]->depth_texture) {
						shader->setUniform1("u_has_shadow_map", 1);
						shader->setUniform("u_shadow_bias", 0.005f);

						// En multipass enviamos la textura actual al slot u_shadow_maps[0] 
						// porque para el shader, en esta pasada, solo existe 1 luz (la luz 0)
						shader->setUniform("u_shadow_maps[0]", shadow_fbos[i]->depth_texture, 3);

						// Enviamos SOLO la matriz de la luz actual
						shader->setMatrix44Array("u_shadow_vps", &this->shadow_viewprojections[i], 1);
					}
					else {
						shader->setUniform1("u_has_shadow_map", 0);
					}

					if (material->alpha_mode == SCN::eAlphaMode::BLEND) {
						glEnable(GL_BLEND);
						glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
					}
					else {
						glDisable(GL_BLEND);
					}
					glDepthFunc(GL_LESS);
				}
				else {
					// Pasadas sucesivas: Aditivo + SIN ambiental + SIN SOMBRAS
					shader->setUniform3("u_ambient_light", vec3(0.0));
					shader->setUniform1("u_has_shadow_map", 0); // Apagamos la sombra para esta luz

					glEnable(GL_BLEND);
					if (material->alpha_mode == SCN::eAlphaMode::BLEND) {
						glBlendFunc(GL_SRC_ALPHA, GL_ONE);
					}
					else {
						glBlendFunc(GL_ONE, GL_ONE);
					}
					glDepthFunc(GL_LEQUAL);
				}

				// Solo enviamos los datos de la luz actual (num_lights = 1)
				// NOTA: Usamos la corrección de punteros &variable[i].x
				shader->setUniform3Array("u_light_position", &light_positions[i].x, 1);
				shader->setUniform3Array("u_light_colors", &light_colors[i].x, 1);
				shader->setUniform1("u_num_lights", 1);
				shader->setUniform1Array("u_light_types", &light_types[i], 1);
				shader->setUniform3Array("u_light_directions", &light_directions[i].x, 1);
				shader->setUniform2Array("u_light_cone_info", &light_cone_info[i].x, 1);

				mesh->render(GL_TRIANGLES);
			}

			// Reset OpenGL state after multi-pass
			glDisable(GL_BLEND);
			glDepthFunc(GL_LESS);
		}
	}

	//disable shader
	shader->disable();

	//set the render state as it was before to avoid problems with future renders
	glDisable(GL_BLEND);
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
		shader = GFX::Shader::Get("texture"); // Usamos tu shader 'texture' que ya tiene el discard
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
		vec3 light_dir = light_ent->root.model.frontVector();
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
#ifndef SKIP_IMGUI

void Renderer::showUI()
{
		
	ImGui::Checkbox("Wireframe", &render_wireframe);
	ImGui::Checkbox("Boundaries", &render_boundaries);
	ImGui::Checkbox("Enable Normal Maps", &show_normals);
	ImGui::Checkbox("Single Pass", &single_pass);


	//add here your stuff
	//...
}

#else
void Renderer::showUI() {}
#endif