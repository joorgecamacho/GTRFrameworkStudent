#pragma once
#include "scene.h"
#include "prefab.h"

#include "light.h"
#include <vector>

//forward declarations
class Camera;
class Skeleton;
namespace GFX {
	class Shader;
	class Mesh;
	class FBO;
}

namespace SCN {

	class Prefab;
	class Material;
	struct sRenderable {
		GFX::Mesh* mesh;
		SCN::Material* material;
		Matrix44 matrix;
		float distance_to_camera; //esto sirve para ordenar por distancia
	};
	// This class is in charge of rendering anything in our system.
	// Separating the render from anything else makes the code cleaner
	class Renderer
	{
	public:
		bool render_wireframe;
		bool render_boundaries;
		bool single_pass_mode;

		// Shadow mapping (assignment 3.2 / 3.3 / 3.4)
		int shadowmap_resolution;      // resolution of the depth FBO (e.g. 1024)
		int shadow_light_index;        // which index in light_list casts shadows (0=spot, 3=directional by default)
		float shadow_bias;             // 3.4.1: constant depth offset to avoid shadow acne
		bool shadow_front_face_cull;   // 3.4.2: render only back-faces to the shadow map (FFC)
		GFX::FBO* shadowmap_fbo;       // depth-only FBO
		Camera* shadow_camera;         // "light camera" used to render the shadow map
		SCN::LightEntity* shadow_light;// the light currently casting shadows (nullptr if disabled)
		std::vector <sRenderable> render_list; //donde guardamos todos los objetos a renderizar
		std::vector<SCN::LightEntity*> light_list; //Donde guardamos todas nuestras luces
		GFX::Texture* skybox_cubemap;

		SCN::Scene* scene;

		//updated every frame
		Renderer(const char* shaders_atlas_filename);
		~Renderer();

		//just to be sure we have everything ready for the rendering
		void setupScene();

		//add here your functions
		//...
		void updateShadowCamera();
		void renderShadowMap();
		void parseNode(SCN::Node* node);

		void parseSceneEntities(SCN::Scene* scene, Camera* camera);

		//renders several elements of the scene
		void renderScene(SCN::Scene* scene, Camera* camera);

		//render the skybox
		void renderSkybox(GFX::Texture* cubemap);

		//to render one mesh given its material and transformation matrix
		void renderMeshWithMaterial(const Matrix44 model, GFX::Mesh* mesh, SCN::Material* material);

		void showUI();
	};

};