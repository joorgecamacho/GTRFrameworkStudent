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

		// === SCI-FI SCAN ===
		GFX::FBO* scan_fbo = nullptr;        // FBO que captura el color de la escena
		bool  scan_active = false;           // Si el scan está activo
		float scan_radius = 0.0f;            // Radio actual de la onda (animado en CPU)
		Vector3f scan_origin;               // Posición del jugador al activar el scan
		// --- Animación (Paso 6) ---
		long  scan_start_time = 0;           // Timestamp (ms) cuando se activó el scan
		float scan_opacity = 1.0f;           // Opacidad global del efecto [0..1]
		float scan_charge_radius = 0.0f;     // Círculo oscuro que se contrae (fase 1)
		float scan_max_radius = 40.0f;       // Radio máximo antes del fade-out
		
		// --- Refinamientos (Fase 2) ---
		bool  scan_manual_mode = false;      // Control manual de la animación por slider
		bool  scan_paused = false;           // Pausa de la animación automática
		float scan_time = 0.0f;              // Acumulador de tiempo (segundos)
		long  last_scan_frame_time = 0;      // Timestamp del frame anterior
		float scan_trail_width = 12.0f;      // Largo del rastro en metros (desvanecimiento posterior)
		bool ssao_hemisphere = true; // true = SSAO+ (hemisferio), false = SSAO básico (esfera)
		int ssao_num_samples = 32;   // Nº de muestras (recomendado 15-64)
		float ssao_radius = 0.05f;   // Radio de búsqueda de oclusores
		std::vector<Vector3f> ssao_sample_points; // Puntos de muestreo precalculados (radio 1.0)

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

		// Renderiza el efecto sci-fi scan como pase de post-procesado final
		void renderScifiScan(Camera* camera);
	};

};