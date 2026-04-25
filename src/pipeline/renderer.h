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
    GFX::Mesh *mesh;
    SCN::Material *material;
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
		bool show_normals = true; // Nueva variable para el modo debug de normales
		bool single_pass = true; // Toggle para Single/Multi pass
    std::vector <sRenderable> render_list;
	std::vector<LightEntity*> light_list;
		GFX::Texture* skybox_cubemap;

		SCN::Scene* scene;

		//updated every frame
		Renderer(const char* shaders_atlas_filename );

		//just to be sure we have everything ready for the rendering
		void setupScene();

		//add here your functions
		//...
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