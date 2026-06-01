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
		static const int MAX_LIGHTS = 8;

		bool render_wireframe;
		bool render_boundaries;
		bool show_normals = true; // Nueva variable para el modo debug de normales
		bool single_pass = true; // Toggle para Single/Multi pass
		bool use_deferred = true; // Toggle para Forward/Deferred pipeline
		std::vector <sRenderable> render_list;
		std::vector<LightEntity*> light_list;
		
		//GFX::FBO* shadow_fbo = nullptr;
		//Matrix44 shadow_viewprojection;
		GFX::FBO* shadow_fbos[MAX_LIGHTS];
		Matrix44 shadow_viewprojections[MAX_LIGHTS];
		GFX::Texture* skybox_cubemap;

		GFX::FBO* gbuffer_fbo = nullptr;
		GFX::FBO* illumination_fbo = nullptr; // HDR RGBA16F (3.2)

		float tonemap_exposure = 2.0f; // Exposición previa al tonemap Uncharted 2 (3.3)

		// === SSAO ===
		GFX::FBO* ssao_fbo = nullptr;
		bool enable_ssao = true;
		bool ssao_hemisphere = true; // true = SSAO+ (hemisferio), false = SSAO básico (esfera)
		int ssao_num_samples = 32;   // Nº de muestras (recomendado 15-64)
		float ssao_radius = 0.05f;   // Radio de búsqueda de oclusores
		std::vector<Vector3f> ssao_sample_points; // Puntos de muestreo precalculados (radio 1.0)

		// === Sci-Fi Scanner (Method B — T5.1) ===
		bool enable_scanner = true;
		bool scanner_active = false;
		Vector3f scanner_origin = Vector3f(0, 0, 0);
		float scanner_radius = 0.0f;
		float scanner_max_radius = 45.0f;
		float scanner_pulse_width = 0.45f;
		Vector3f scanner_color = Vector3f(0.1f, 0.8f, 1.0f);
		float scanner_intensity = 3.2f;
		float scanner_sharpness = 3.0f;
		float scanner_elapsed = 0.0f;
		float scanner_duration = 7.0f;
		float scanner_edge_width = 0.8f;
		float scanner_grid_spacing = 3.5f;
		float scanner_grid_scale = 0.04f;
		float scanner_grid_line_width = 0.1f;
		float scanner_grid_intensity = 0.55f;
		float scanner_trail_width = 16.0f;
		Vector3f scanner_darken_color = Vector3f(0.01f, 0.04f, 0.12f);
		float scanner_darken_amount = 0.92f;
		bool scanner_show_sphere = true;   // T5.1: render the expanding sphere mesh
		float scanner_sphere_rim = 2.5f;   // fresnel falloff (faint shell)
		float scanner_sphere_alpha = 1.0f; // ring opacity
		float scanner_contact_thickness = 0.35f; // Method A: contact ring width (meters)
		float scanner_rim_strength = 0.35f;       // volumetric shell on leading ring
		int scanner_ring_count = 6;               // number of concentric sub-lines
		float scanner_ring_spacing = 1.8f;        // distance between sub-lines (meters)
		float scanner_ground_height = 0.0f;       // y of the floor (sphere origin height)
		bool scanner_screenspace_layers = false;  // kept off — Method A only

		void triggerScanner(const Vector3f& origin);
		void renderScannerSphere(Camera* camera);
		void updateScanner(float dt);
		float evaluateScannerRadius() const;
		void bindScannerUniforms(GFX::Shader* shader);
		void resetScannerDefaults();

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
		void renderSkybox(GFX::Texture* cubemap, bool apply_gamma = false);

		//to render one mesh given its material and transformation matrix
		void renderMeshWithMaterial(const Matrix44 model, GFX::Mesh* mesh, SCN::Material* material);

		// Forward render for transparent objects (reuses phong shader with all lights)
		void renderMeshWithMaterialForward(const Matrix44 model, GFX::Mesh* mesh, SCN::Material* material);

		void renderPlain(const Matrix44 model, GFX::Mesh* mesh, SCN::Material* material, Camera* light_cam);
		
		void generateShadowMap();

		// Deferred rendering pipeline
		void renderDeferred(Camera* camera);

		// Forward rendering pipeline (original)
		void renderForward(Camera* camera);

		void showUI();
	};

};