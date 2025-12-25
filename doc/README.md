# FastFileSearch

FastFileSearch is a high-performance file search utility for Windows that reads the NTFS Master File Table (MFT) directly to locate files instantly, bypassing the slow recursive directory traversal of standard search methods.

## Features
- **Ultra-fast Search**: Scans millions of files in seconds by reading the raw MFT.
- **Multilingual UI**: Supports English and Japanese.
- **Detailed Results**: Displays Name, Full Path, Size, and Modified Date.
- **Search Term Highlighting**: Matches in Name and Path are highlighted (Cyan).
- **Sortable Results**: Click column headers to sort by Name, Path, Date, or Size.
- **Persistence**: Application settings (Window size, Target folders, Language) are saved automatically to `Documents\FastFileSearch\FastFileSearch.ini`.
- **Context Menu**: Right-click to Copy Path to clipboard.
- **Search Capabilities**: Supports drive-wide search or specific folder filtering.
- **Save Results**: Export search results to a text file.

## Requirements
- Windows OS (NTFS Volume)
- **Administrator Privileges**: Required to open a handle to the volume (e.g., `\\.\C:`).

## Build Instructions (CMake)
Prerequisites: verify `cmake` and `msvc` (Visual Studio Compiler) are in your PATH.

1. Create a build directory:
   ```cmd
   mkdir build
   cd build
   ```
2. Generate project files:
   ```cmd
   cmake ..
   ```
3. Build the project (Release mode):
   ```cmd
   cmake --build . --config Release
   ```

The executable `FastFileSearch.exe` will be generated in `build/Release`.

## Usage
1. Run `FastFileSearch.exe` as Administrator.
2. Add target folders using "Add Folder" (e.g., `C:\Work`).
3. Enter a search query (substring match) and click **Search**.

## Debugging / Console Tool
The project includes a console-based test tool `test_console.exe` for verifying MFT reading logic without the GUI.

**Usage:**
```cmd
test_console.exe [Options] [Drive] [Query]
```

**Options:**
- `-v`: **Verbose Mode**. Prints every filename found in the MFT. Useful for checking if files are being seen at all.
- `-t`: **Trace Mode**. Prints detailed execution stages (Volume Open, MFT Header Read, Run Decoding). Use this to debug initialization or "RecordSize=0" errors.

**Examples:**
- `test_console.exe D: ws` (Search for "ws" on D:)
- `test_console.exe -t D:` (Trace MFT reading on D:)
- `test_console.exe -v C: notes` (Search "notes" on C: and print ALL files found)
