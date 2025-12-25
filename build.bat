@echo off
setlocal
echo Building Fast File Search...

:: Check for cl
where cl >nul 2>nul
if %errorlevel% neq 0 (
    echo Error: cl.exe not found. Please run this from a Developer Command Prompt.
    exit /b 1
)

:: Compile Resources
rc /nologo FastFileSearch.rc
if %errorlevel% neq 0 (
    echo Resource compilation failed.
    exit /b 1
)

:: Compile and Link
cl /nologo /O2 /EHsc /DUNICODE /D_UNICODE ^
    main.cpp MFTReader.cpp FastFileSearch.res ^
    /FeFastFileSearch.exe ^
    /link user32.lib comctl32.lib comdlg32.lib kernel32.lib advapi32.lib shell32.lib ^
    /MANIFEST:EMBED /MANIFESTINPUT:app.manifest

if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)

echo Build Success! FastFileSearch.exe created.
endlocal
