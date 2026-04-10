# macOS Build and Execution Guide

Since you are running macOS (Apple Silicon), you don't need Visual Studio at all! Visual Studio `.sln` files are for Windows only. The framework uses `CMake` instead, which handles the build process transparently.

## 1. How to Compile the Framework

Whenever you pull new code or change the `CMakeLists.txt` (or if you are building the project for the first time), you compile the framework entirely via your terminal:

```bash
# 1. Open your terminal and navigate to the framework's root folder
cd /Users/jorgecamacho/Documents/University/real-time-graphics/GTRFrameworkStudent

# 2. Make sure the 'build' directory exists and enter it
mkdir -p build && cd build

# 3. Generate the Makefiles using CMake
cmake ..

# 4. Compile the project! (The -j flag uses all your CPU cores to make it much faster)
make -j$(sysctl -n hw.logicalcpu)
```

**Note on fixes made for macOS compatibility:**
- The framework originally tried to link to `AGL.framework` (Apple's old OpenGL framework), which was removed in macOS 10.15 Catalina. We fixed this in `libraries/glew-cmake/CMakeLists.txt` around line 120. We changed `find_library(AGL_LIBRARY AGL REQUIRED)` to no longer be `REQUIRED`. The native `OpenGL.framework` is sufficient:
  ```cmake
  if(APPLE)
      find_library(AGL_LIBRARY AGL)
      if(AGL_LIBRARY)
          list(APPEND LIBRARIES ${AGL_LIBRARY})
      endif()
  ...
  ```
- An ambiguous call to `random()` in `src/core/math.cpp` was clashing with macOS standard libraries. We changed it to explicitly use `(float)rand() / (float)RAND_MAX`.

## 2. How to Run the Application

Once compiled, the executable file will be generated inside the `build/` directory under the name `GTR_Framework`. 

**🚨 CRITICAL STEP:** You *must* run the executable from the **root folder** of the project, NOT from inside the `build/` folder. If you run it from inside `build/`, the app will crash or show a black screen because it won't be able to find the assets (shaders, textures, scenes).

Here is the exact workflow to execute it:

```bash
# 1. Abre tu terminal y asegúrate de estar en la carpeta raíz del proyecto
cd /Users/jorgecamacho/Documents/University/real-time-graphics/GTRFrameworkStudent

# 2. Ejecuta el archivo compilado indicando la ruta hacia la carpeta build
./build/GTR_Framework
```

*(Cuando el programa se abra correctamente, verás la ventana del framework con la interfaz de ImGui).*

## 3. Developing

If you make modifications to `.cpp` or `.h` files, you only need to run:

```bash
cd /Users/jorgecamacho/Documents/University/real-time-graphics/GTRFrameworkStudent/build
make -j$(sysctl -n hw.logicalcpu)
```
You only need to re-run `cmake ..` if you add new files or modify the `CMakeLists.txt`.
