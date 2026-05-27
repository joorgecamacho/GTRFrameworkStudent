# GTR Framework
OpenGL C++ Framework used for teach the Real-time Grapchics course at Universitat Pompeu Fabra.

Framework by Javier Agenjo.

## Building

Clone and initialize the repository:
```
git clone --recurse-submodules -j8 https://github.com/upf-gti/GTRFrameworkStudent.git
```
## Windows
Building requirements are

* [MS Visual Studio Community 2022](https://visualstudio.microsoft.com/es/free-developer-offers/). While installing make sure you select **"Desktop Development with C++"**.
* [CMake](https://cmake.org/download/). Remember to set **"Add CMake to PATH"** when asked while installing, otherwhise you won't be able to call CMake from the terminal.

Once you have all required open a Windows Terminal, go to the project folder and do this steps:
```console
cd GTRFrameworkStudent
mkdir build
cd build
cmake ..
```

This should generate you a Visual Studio Solution for the project.

## Mac OS (Xcode)
Building requirements are

* [Xcode].
* [CMake]. Install using Homebrew with the command ``brew install cmake``.

Once you have all required open a Terminal, go to the project folder and do this steps:
```console
cd GTRFrameworkStudent
mkdir build
cd build
cmake .. -G "Xcode"
```

This should generate you a Xcode project.
