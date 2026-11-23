#pragma once
#include "MFTReader.h"
#include "VolumeReaderCommon.h"
#include "exFatStructs.h"
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>
#include <windows.h>

class exFatReader {
public:
  exFatReader();
  ~exFatReader();

  bool Initialize(TCHAR driveLetter);
  void Close();
  bool Scan(void (*progressCallback)(int, int, void *), void *userData,
            std::function<void(const std::wstring &)> onFileFound = nullptr);

  void SetTraceCallback(std::function<void(const std::wstring &)> callback) {
    traceCallback = callback;
  }

  std::vector<FileResult> Search(const std::wstring &query,
                                 const std::wstring &targetFolder,
                                 const SearchOptions &options = SearchOptions(),
                                 int maxResults = -1);

  std::wstring GetLastErrorMessage() const;

private:
  using Entry = VolumeCommon::VolumeEntry;

  // A subdirectory found while parsing; processed after its parent's entries.
  struct PendingDir {
    uint32_t cluster;
    bool noFatChain;
    uint64_t dataLength;
    uint64_t key;
  };

  std::wstring lastError;
  void SetError(const std::wstring &msg);

  HANDLE hVolume;
  TCHAR currentDrive;
  VolumeCommon::EntryMap fileMap;
  uint64_t nextSyntheticKey;
  std::unordered_set<uint32_t> visitedClusters; // Cycle / cross-link guard

  std::function<void(const std::wstring &)> traceCallback;

  uint32_t bytesPerSector;
  uint32_t sectorsPerCluster;
  uint32_t clusterHeapOffset;
  uint32_t fatOffset;
  uint32_t fatLength;
  uint32_t rootDirectoryCluster;
  uint64_t clusterCount;

  // Progress State
  uint64_t processedClusters;
  void (*progressCb)(int, int, void *);
  void *userPtr;

  bool ProcessDirectory(uint32_t cluster, bool noFatChain, uint64_t dataLength,
                        uint64_t parentKey, uint32_t depth);
  bool ReadDirectoryBytes(uint32_t start, bool noFatChain, uint64_t dataLength,
                          std::vector<uint8_t> &out);
  void ParseEntrySets(const std::vector<uint8_t> &data, uint64_t parentKey,
                      std::vector<PendingDir> &subdirs);
  bool GetNextCluster(uint32_t cluster, uint32_t &next);
  bool IsDataCluster(uint64_t cluster) const;
  uint64_t ClusterToSector(uint32_t cluster);
};
