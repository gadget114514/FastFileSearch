# exFAT Reader Internals

This document details the internal logic of the `FastFileSearch` exFAT Reader (`exFatReader`).

## Overview

`exFatReader` reads the **exFAT** filesystem by directly accessing the raw volume handle. exFAT was designed for large flash storage and uses a different on-disk structure than FAT16/FAT32, with variable-length directory entry sets and a cluster heap layout described by power-of-two shift values.

## Core Components

### 1. Initialization (`Initialize`)

- Opens a raw volume handle via `CreateFile` with `GENERIC_READ`.
- Reads the `EXFAT_BOOT_SECTOR` from offset 0.
- Validates the filesystem signature: `FileSystemName` must equal `"EXFAT   "` (8 bytes).
- Parses layout fields using power-of-two shifts:
  - `bytesPerSector = 1 << BytesPerSectorShift`
  - `sectorsPerCluster = 1 << SectorsPerClusterShift`
- Stores key volume parameters:
  - `clusterHeapOffset` — sector offset of the Cluster Heap (data area)
  - `fatOffset` — sector offset of the FAT
  - `fatLength` — length of the FAT in sectors
  - `rootDirectoryCluster` — first cluster of the root directory

### 2. FAT Traversal (`GetNextCluster`)

Unlike `FatReader`, exFatReader does **not** cache the entire FAT. Instead, it seeks on demand:
- FAT entry position = `fatOffset * bytesPerSector + cluster * 4`
- Reads 4 bytes (uint32) directly from the volume.
- Returns `0xFFFFFFFF` on read failure (treated as end-of-chain).
- End-of-chain sentinel: `>= 0xFFFFFFF8`.

This avoids large memory allocations for big exFAT volumes, at the cost of extra I/O per cluster hop.

### 3. Cluster-to-Sector Conversion (`ClusterToSector`)

```
sector = clusterHeapOffset + (cluster - 2) * sectorsPerCluster
```

Byte offset: `sector * bytesPerSector`

### 4. Scanning (`Scan`)

- Begins at `rootDirectoryCluster`.
- Calls `ProcessDirectory` with `noFatChain = false` and `dataLength = 0` for the root.

### 5. Directory Processing (`ProcessDirectory`)

exFAT uses variable-length **directory entry sets** rather than fixed 32-byte entries:

- Reads each cluster's sectors into a buffer.
- Iterates over 32-byte `EXFAT_DIR_ENTRY` records identified by `EntryType`:

| EntryType | Meaning |
|-----------|---------|
| `0x85` | File Entry — starts a new entry set; contains file attributes and timestamp |
| `0xC0` | Stream Extension — contains data length, first cluster, `NoFatChain` flag |
| `0xC1` | File Name Extension — contains up to 15 UTF-16LE name characters |
| `0x00` | End of directory |

- A complete entry set is: one File Entry (`0x85`) + one Stream Extension (`0xC0`) + one or more File Name Extensions (`0xC1`).
- Name characters are assembled from all File Name Extension entries in order.

#### Timestamp Conversion (`FatTimestampToWin32`)

exFAT timestamps are in FAT format (packed 32-bit date/time fields) with a 10ms resolution field:
- Extracts year, month, day, hour, minute, second from the packed value.
- Converts to `FILETIME` (100-nanosecond intervals since January 1, 1601).
- Applies the `tenMs` field for sub-second precision.

#### NoFatChain Flag

exFAT supports a `NoFatChain` optimization: if the Stream Extension sets this flag, the file occupies contiguous clusters and no FAT chain traversal is needed. `ProcessDirectory` passes this flag down to recursive calls.

### 6. Entry Storage

Each valid file or directory is stored in `fileMap` keyed by `FirstCluster`:

```cpp
struct Entry {
    uint32_t FirstCluster;        // First cluster of the file/directory
    uint32_t ParentFirstCluster;  // First cluster of the parent directory
    std::wstring Name;            // UTF-16 name assembled from File Name Extensions
    uint64_t Size;                // Data length from Stream Extension
    uint64_t LastWriteTime;       // Converted FILETIME
    bool IsDirectory;
    bool IsValid;
};
```

### 7. Search & Filtering (`Search`)

- Iterates `fileMap` and applies `MatchPattern` for each entry.
- Reconstructs full paths by walking `ParentFirstCluster` references.
- Applies `SearchOptions` filters: size, date, type, extension, exclude pattern, invert match.
- Result count capped by `maxResults`.

## Key Differences from FatReader

| Aspect | FatReader (FAT16/32) | exFatReader |
|--------|----------------------|-------------|
| FAT caching | Full FAT loaded into memory | On-demand per-cluster reads |
| Directory entries | Fixed 32-byte, LFN chains | Variable entry sets (type-tagged) |
| Filenames | OEM short names + LFN Unicode | Always UTF-16 (File Name Extensions) |
| Code page | Configurable (OEM/Shift-JIS/etc.) | Not needed (native Unicode) |
| Timestamps | FAT date/time fields | FAT date/time + 10ms field |

## Known Limitations

- **No FAT caching**: On-demand FAT reads are slower for highly fragmented volumes.
- **Admin Rights**: Required to open the raw volume handle.
