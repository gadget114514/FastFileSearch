# FAT Reader Internals

This document details the internal logic of the `FastFileSearch` FAT Reader (`FatReader`), which supports FAT16 and FAT32 filesystems.

## Overview

`FatReader` bypasses the Windows File System API by directly reading the **File Allocation Table (FAT)** and directory structures from the raw volume. It opens the volume handle (e.g., `\\.\D:`) and reads the BIOS Parameter Block (BPB) to understand the volume layout.

## Core Components

### 1. Initialization (`Initialize`)

- Opens a raw volume handle via `CreateFile` with `GENERIC_READ`.
- Reads the 512-byte boot sector at offset 0.
- Casts the boot sector to `FAT16_BPB` first to read common fields:
  - `BytesPerSector`, `SectorsPerCluster`, `ReservedSectors`, `Fats`
- **FAT12/16 vs FAT32 detection**: If `SectorsPerFat != 0`, it is FAT12/16; otherwise FAT32.
  - FAT16: computes `rootDirSectors` from `RootEntries * 32 / BytesPerSector`.
  - FAT32: re-casts to `FAT32_BPB` to read `SectorsPerFat32` and `RootCluster`.
- Computes `firstDataSector` as the start of the data area after reserved sectors, FATs, and the FAT16 root directory.
- Calls `LoadFat()` to cache the entire FAT in memory.

### 2. FAT Caching (`LoadFat`)

- The FAT is loaded entirely into `fatCache` (a `std::vector<uint8_t>`) for O(1) cluster chain lookups.
- FAT offset = `reservedSectors * bytesPerSector`.
- FAT size = `sectorsPerFat * bytesPerSector`.

### 3. Cluster Chain Traversal (`GetNextCluster`)

- **FAT16**: Each entry is 2 bytes. `next = *(uint16_t*)(&fatCache[cluster * 2])`.
- **FAT32**: Each entry is 4 bytes (28-bit value). `next = *(uint32_t*)(&fatCache[cluster * 4]) & 0x0FFFFFFF`.
- End-of-chain sentinel: `>= 0xFFF8` (FAT16) or `>= 0x0FFFFFF8` (FAT32).

### 4. Scanning (`Scan`)

- For FAT16: starts from the fixed root directory region (immediately after the FATs).
- For FAT32: starts from `rootCluster` and traverses the cluster chain.
- Calls `ProcessDirectory` recursively for each subdirectory found.

### 5. Directory Processing (`ProcessDirectory`)

- Reads directory sectors cluster by cluster.
- Parses 32-byte `FAT_DIR_ENTRY` records:
  - Skips deleted entries (first byte `0xE5`) and end-of-directory markers (`0x00`).
  - Skips volume label entries (`ATTR_VOLUME_ID = 0x08`).
- **Long File Names (LFN)**: Detects `ATTR_LONG_NAME = 0x0F` entries, accumulates Unicode name fragments in reverse order, and assembles the final wide-string name.
- Short name fallback: Converts OEM short names using the specified `codePage` via `MultiByteToWideChar`.
- Stores each entry in `fileMap` keyed by `FirstCluster` (unique for non-empty files and directories).
- Recurses into subdirectories.

### 6. Code Page Support

FAT volumes (especially removable drives) often store short filenames in OEM code pages. `FatReader` accepts a `codePage` parameter:
- `CP_OEMCP` — system default OEM code page
- `932` — Shift-JIS (Japanese)
- `936` — GBK (Simplified Chinese)
- `CP_UTF8` — UTF-8

LFN entries are always UTF-16 and do not require code page conversion.

### 7. Search & Filtering (`Search`)

- Iterates `fileMap` and applies `MatchPattern` for each entry.
- Reconstructs full paths by traversing `ParentFirstCluster` references via `BuildPath`.
- Applies `SearchOptions` filters: size range, date range, type (file/folder), extension, exclude pattern, invert match.
- Result count is capped by `maxResults` (default `-1` = no limit, set to 50,000 by the GUI).

## Internal Entry Structure

```cpp
struct Entry {
    uint32_t FirstCluster;        // Cluster number of the first cluster
    uint32_t ParentFirstCluster;  // Parent directory's first cluster
    std::wstring Name;            // Decoded filename (LFN preferred, short fallback)
    uint64_t Size;                // File size in bytes
    uint64_t LastWriteTime;       // Converted to FILETIME (100ns intervals since 1601)
    bool IsDirectory;
    bool IsValid;
};
```

## Cluster-to-Sector Conversion

```
sector = firstDataSector + (cluster - 2) * sectorsPerCluster
```

For absolute byte offset: `sector * bytesPerSector`

## Known Limitations

- **FAT12**: Not explicitly supported (detected as FAT16 path but cluster entries differ).
- **Empty files**: Multiple files with `FirstCluster == 0` share the same map key; the current implementation may overwrite earlier entries. This edge case is noted in the source.
- **Admin Rights**: Required to open the raw volume handle.
