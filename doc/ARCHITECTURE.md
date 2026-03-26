# Source Architecture

This document describes the overall source code architecture of `FastFileSearch`.

## Directory Structure

```
src/
├── main.cpp              # Win32 GUI application entry point
├── MFTReader.cpp/h       # NTFS MFT filesystem reader
├── FatReader.cpp/h       # FAT16/FAT32 filesystem reader
├── exFatReader.cpp/h     # exFAT filesystem reader
├── Localization.cpp/h    # Multilingual UI string management
├── NtfsStructs.h         # Raw NTFS on-disk structure definitions
├── FatStructs.h          # Raw FAT16/FAT32 on-disk structure definitions
├── exFatStructs.h        # Raw exFAT on-disk structure definitions
├── test_console.cpp      # Command-line diagnostic tool
├── resource.h            # Win32 resource identifiers
├── FastFileSearch.rc     # Win32 resource script
└── app.manifest          # Windows UAC manifest (requires admin)
```

## Layers

```
┌─────────────────────────────────────────────────┐
│                  main.cpp (GUI)                 │
│  Win32 window, dialogs, list view, user input   │
├──────────────┬──────────────┬───────────────────┤
│  MFTReader   │  FatReader   │   exFatReader     │
│  (NTFS)      │  (FAT16/32)  │   (exFAT)         │
├──────────────┴──────────────┴───────────────────┤
│          Raw Volume I/O  (CreateFile / ReadFile) │
├─────────────────────────────────────────────────┤
│   NtfsStructs.h  │  FatStructs.h  │ exFatStructs.h │
│   (on-disk layout definitions)                  │
└─────────────────────────────────────────────────┘
```

## Shared Data Types (MFTReader.h)

All three readers share common types defined in `MFTReader.h`:

### `FileResult`
The output type returned by every reader's `Search()` method:
```cpp
struct FileResult {
    std::wstring Name;
    std::wstring FullPath;
    uint64_t Size;
    uint64_t LastWriteTime;  // FILETIME (100ns intervals since 1601-01-01)
    bool IsDirectory;
};
```

### `SearchOptions`
Controls how `Search()` filters results:
```cpp
struct SearchOptions {
    MatchMode mode;          // Substring | Exact | SpaceDivided | RegEx
    bool ignoreCase;
    uint64_t minSize, maxSize;
    uint64_t minDate, maxDate;
    bool includeFiles, includeFolders;
    std::wstring extensionFilter;  // e.g. "exe;dll"
    bool matchFullPath;
    std::wstring excludePattern;
    bool invertMatch;
};
```

### `MatchMode` enum
| Value | Behavior |
|-------|----------|
| `MatchMode_Substring` | Query is a substring of the filename (default) |
| `MatchMode_Exact` | Filename must equal the query exactly |
| `MatchMode_SpaceDivided` | All space-separated tokens must appear (AND logic) |
| `MatchMode_RegEx` | Query is a C++ `std::wregex` pattern |

## Filesystem Reader Interface

All three readers follow the same lifecycle pattern:

```
Initialize(drive) → Scan(progressCb) → Search(query, folder, options) → Close()
```

| Method | Description |
|--------|-------------|
| `Initialize(TCHAR drive)` | Opens the raw volume handle and reads filesystem metadata |
| `Scan(progressCb, userData)` | Walks all filesystem records, populates internal `fileMap` |
| `Search(query, folder, options)` | Filters `fileMap` and returns matching `FileResult` vector |
| `Close()` | Releases the volume handle and clears internal state |
| `SetTraceCallback(fn)` | Attaches a debug trace callback for diagnostic output |
| `GetLastErrorMessage()` | Returns the last error string |

### Internal `Entry` struct (per-reader)

Each reader maintains a private `fileMap` (`unordered_map`) of `Entry` structs:

```cpp
struct Entry {
    <ID field>              // uint64_t RefID (NTFS) or uint32_t FirstCluster (FAT/exFAT)
    <ParentID field>        // uint64_t ParentRefID (NTFS) or uint32_t ParentFirstCluster
    std::wstring Name;
    uint64_t Size;
    uint64_t LastWriteTime;
    bool IsDirectory;
    bool IsValid;
};
```

Full paths are reconstructed lazily by `BuildPath()` which walks the parent chain up to the root.

## GUI (`main.cpp`)

The GUI is a pure Win32 API application with no external UI framework.

Key responsibilities:
- **Drive detection**: Enumerates available drives and detects their filesystem type to choose the correct reader.
- **Search dispatch**: Creates the appropriate reader (`MFTReader`, `FatReader`, or `exFatReader`) based on drive type; runs `Scan` + `Search` on a background thread.
- **Progress reporting**: The progress callback posts window messages to the main thread to update the progress bar.
- **Result display**: Uses a `WC_LISTVIEW` control with virtual/owner-draw for sortable columns (Name, Path, Date, Size).
- **Persistent settings**: Reads/writes an `.ini` file in `%LOCALAPPDATA%` for window size, language, search options, and target folders.
- **Localization**: Calls `Localization` to retrieve UI strings in the selected language at render time.

## Localization (`Localization.cpp/h`)

- Holds a static table of ~70 string IDs × 8 languages.
- Supported languages: English, Japanese, Chinese Simplified, Chinese Traditional, Spanish, French, German, Portuguese.
- `Localization::Get(stringId)` returns the appropriate `std::wstring` for the active language.
- Language selection is persisted in the `.ini` file.

## Diagnostic Tool (`test_console.cpp`)

A command-line companion that exercises the readers without the GUI:

```
test_console.exe [Options] [Drive] [Query]
  -v   Verbose: print every file found during Scan
  -t   Trace: log initialization and run-list stages
```

Attaches callbacks to `SetTraceCallback` and the `onFileFound` parameter of `Scan` to produce diagnostic output directly to stdout.

## Build System (`CMakeLists.txt`)

- Requires CMake 3.10+ and MSVC (Visual Studio 2022 / v143 toolset).
- Target: `FastFileSearch.exe` (WIN32 subsystem) + `test_console.exe` (console subsystem).
- C++17 standard.
- Links: `comctl32` (list view), `shlwapi` (path utilities).
- The `app.manifest` requests `requireAdministrator` execution level for raw volume access.

## Further Reading

- [MFT_INTERNALS.md](MFT_INTERNALS.md) — Deep dive into NTFS MFT parsing
- [FAT_INTERNALS.md](FAT_INTERNALS.md) — Deep dive into FAT16/FAT32 parsing
- [EXFAT_INTERNALS.md](EXFAT_INTERNALS.md) — Deep dive into exFAT parsing
