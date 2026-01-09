---
description: Build the FastFileSearch installer using InnoSetup
---

1. Build the project in Release mode
   
   ```powershell
   cmake --build build --config Release
   ```

2. Compile the InnoSetup script
   
   ```powershell
   & "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\setup.iss
   ```

   *Note: If Inno Setup is installed in a different location, please adjust the path to `ISCC.exe` accordingly.*
