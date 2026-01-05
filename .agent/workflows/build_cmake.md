---
description: Build the FastFileSearch project using CMake
---
1. Create a build directory:
   ```bash
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
   ```
2. Build the project (Release mode):
   ```bash
   cmake --build build --config Release
   ```
3. The executable will be located at `build/Release/FastFileSearch.exe`.
