#pragma once
#include "FatStructs.h"
#include "MFTReader.h" // For FileResult and other shared structures
#include "VolumeReaderCommon.h"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>
#include <windows.h>

class FatReader {
public:
  FatReader();
  ~FatReader();

  bool Initialize(TCHAR driveLetter);
  void Close();
  bool Scan(int codePage, void (*progressCallback)(int, int, void *),
            void *userData,
            std::function<void(const std::wstring &)> onFileFound = nullptr);

  void SetTraceCallback(std::function<void(const std::wstring &)> callback) {
    traceCallback = callback;
  }

  std::vector<FileResult> Search(const std::wstring &query,
                                 const std::wstring &targetFolder,
                                 int codePage = CP_OEMCP,
                                 const SearchOptions &options = SearchOptions(),
                                 int maxResults = -1);

  std::wstring GetLastErrorMessage() const;

private:
  using Entry = VolumeCommon::VolumeEntry;

  // Accumulates LFN fragments until the 8.3 entry that terminates them.
  struct LfnState {
    static constexpr uint8_t kMaxFragments = 20; // 255 chars / 13 per fragment
    std::wstring parts[kMaxFragments];           // parts[seq - 1]
    uint8_t total = 0;                           // fragment count of this name
    uint8_t expectedSeq = 0;                     // next seq expected; 0 = complete
    uint8_t checksum = 0;                        // checksum of the 8.3 name
    bool active = false;

    void Reset() {
      for (auto &p : parts)
        p.clear();
      total = 0;
      expectedSeq = 0;
      checksum = 0;
      active = false;
    }
    std::wstring Assemble() const {
      std::wstring s;
      for (uint8_t i = 0; i < total; ++i)
        s += parts[i];
      return s;
    }
  };

  // A subdirectory found while parsing; processed after its parent's entries.
  struct PendingDir {
    uint32_t cluster;
    uint64_t key;
  };

  std::wstring lastError;
  void SetError(const std::wstring &msg);

  HANDLE hVolume;
  TCHAR currentDrive;
  VolumeCommon::EntryMap fileMap; // Key: see VolumeReaderCommon.h
  uint64_t nextSyntheticKey;
  std::unordered_set<uint32_t> visitedClusters; // Cycle / cross-link guard

  std::function<void(const std::wstring &)> traceCallback;

  uint32_t bytesPerSector;
  uint32_t sectorsPerCluster;
  uint32_t reservedSectors;
  uint32_t fatCount;
  uint32_t sectorsPerFat;
  uint32_t rootDirEntries; // Only for FAT12/16
  uint32_t rootDirSectors; // Only for FAT12/16
  uint32_t firstDataSector;
  uint32_t totalSectors;
  uint64_t clusterCount;   // Number of data clusters (valid: 2..clusterCount+1)

  bool isFat32;
  bool isFat12;
  uint32_t rootCluster; // For FAT32

  bool ProcessDirectory(uint32_t cluster, uint64_t parentKey, int codePage,
                        uint32_t depth);
  void ParseEntries(const uint8_t *data, size_t size, uint64_t parentKey,
                    int codePage, LfnState &lfn,
                    std::vector<PendingDir> &subdirs, bool &sawEnd);
  static void AddLfnFragment(LfnState &lfn, const FAT_LFN_ENTRY *le);

  bool IsDataCluster(uint64_t cluster) const;
  bool IsEndOfChain(uint32_t value) const;
  uint32_t GetNextCluster(uint32_t cluster);
  uint64_t ClusterToSector(uint32_t cluster);

  std::vector<uint8_t> fatCache;
  bool LoadFat();

  // Progress State
  uint64_t totalUsedClusters;
  uint64_t processedClusters;
  void (*progressCb)(int, int, void *);
  void *userPtr;
};
