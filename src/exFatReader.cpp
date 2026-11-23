#include "exFatReader.h"
#include <algorithm>
#include <cstring>

namespace {

constexpr uint64_t kMaxClusterBytes = 32ull * 1024 * 1024;

// exFAT EntrySetChecksum: 16-bit rotate-and-add over every byte of the set,
// skipping the SetChecksum field itself (bytes 2 and 3 of the primary entry).
bool EntrySetChecksumOk(const uint8_t *set, size_t bytes, uint16_t expected) {
  uint16_t sum = 0;
  for (size_t b = 0; b < bytes; ++b) {
    if (b == 2 || b == 3)
      continue;
    sum = (uint16_t)((uint16_t)(sum << 15) | (uint16_t)(sum >> 1));
    sum = (uint16_t)(sum + set[b]);
  }
  return sum == expected;
}

} // namespace

exFatReader::exFatReader()
    : hVolume(INVALID_HANDLE_VALUE), currentDrive(0), nextSyntheticKey(0),
      bytesPerSector(0), sectorsPerCluster(0), clusterHeapOffset(0),
      fatOffset(0), fatLength(0), rootDirectoryCluster(0), clusterCount(0),
      processedClusters(0), progressCb(nullptr), userPtr(nullptr) {}

exFatReader::~exFatReader() { Close(); }

void exFatReader::SetError(const std::wstring &msg) {
  DWORD err = GetLastError();
  LPWSTR lpMsgBuf = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                 (LPWSTR)&lpMsgBuf, 0, NULL);
  lastError = msg + L": " + (lpMsgBuf ? lpMsgBuf : L"Unknown Error");
  if (lpMsgBuf)
    LocalFree(lpMsgBuf);
}

void exFatReader::Close() {
  if (hVolume != INVALID_HANDLE_VALUE) {
    CloseHandle(hVolume);
    hVolume = INVALID_HANDLE_VALUE;
  }
  fileMap.clear();
  visitedClusters.clear();
}

std::wstring exFatReader::GetLastErrorMessage() const { return lastError; }

bool exFatReader::Initialize(TCHAR driveLetter) {
  Close();
  currentDrive = driveLetter;

  std::wstring path = L"\\\\.\\";
  path += driveLetter;
  path += L":";

  hVolume = CreateFileW(path.c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        0, NULL);
  if (hVolume == INVALID_HANDLE_VALUE) {
    SetError(L"Failed to open volume");
    return false;
  }

  // On failure, record the error and release the handle.
  auto fail = [this](const wchar_t *msg) {
    SetError(msg);
    Close();
    return false;
  };

  EXFAT_BOOT_SECTOR bs;
  if (!VolumeCommon::ReadVolumeExact(hVolume, 0, &bs, sizeof(bs)))
    return fail(L"Failed to read exFAT boot sector");

  if (std::memcmp(bs.FileSystemName, "EXFAT   ", 8) != 0)
    return fail(L"Not an exFAT volume");
  if (bs.BootSignature != 0xAA55)
    return fail(L"Invalid exFAT boot signature");

  // Bounds on the shift counts come before any shifting.
  if (bs.BytesPerSectorShift < 9 || bs.BytesPerSectorShift > 12)
    return fail(L"Invalid exFAT bytes-per-sector shift");
  if (bs.SectorsPerClusterShift > 25 - bs.BytesPerSectorShift)
    return fail(L"Invalid exFAT sectors-per-cluster shift");
  if (bs.FatOffset < 24 || bs.FatLength == 0 ||
      (uint64_t)bs.FatOffset + bs.FatLength > bs.ClusterHeapOffset)
    return fail(L"Invalid exFAT FAT geometry");
  if (bs.ClusterCount == 0 || bs.RootDirectoryCluster < 2 ||
      bs.RootDirectoryCluster - 2 >= bs.ClusterCount)
    return fail(L"Invalid exFAT root directory cluster");

  bytesPerSector = 1u << bs.BytesPerSectorShift;
  sectorsPerCluster = 1u << bs.SectorsPerClusterShift;
  clusterHeapOffset = bs.ClusterHeapOffset;
  fatOffset = bs.FatOffset;
  fatLength = bs.FatLength;
  rootDirectoryCluster = bs.RootDirectoryCluster;
  clusterCount = bs.ClusterCount;

  return true;
}

bool exFatReader::IsDataCluster(uint64_t cluster) const {
  return cluster >= 2 && cluster - 2 < clusterCount;
}

bool exFatReader::GetNextCluster(uint32_t cluster, uint32_t &next) {
  const uint64_t fatBytes = (uint64_t)fatLength * bytesPerSector;
  const uint64_t entryOff = (uint64_t)cluster * 4;
  if (entryOff + 4 > fatBytes) {
    next = 0xFFFFFFFF; // Outside the FAT: treat as end of chain
    return true;
  }

  uint64_t fatPos = (uint64_t)fatOffset * bytesPerSector + entryOff;
  uint32_t value = 0;
  if (!VolumeCommon::ReadVolumeExact(hVolume, fatPos, &value, 4)) {
    SetError(L"Failed to read exFAT FAT entry");
    return false;
  }
  next = value;
  return true;
}

uint64_t exFatReader::ClusterToSector(uint32_t cluster) {
  // Callers only pass clusters accepted by IsDataCluster, so cluster >= 2.
  return clusterHeapOffset + (uint64_t)(cluster - 2) * sectorsPerCluster;
}

bool exFatReader::Scan(void (*progressCallback)(int, int, void *),
                       void *userData,
                       std::function<void(const std::wstring &)> onFileFound) {
  if (hVolume == INVALID_HANDLE_VALUE)
    return false;

  progressCb = progressCallback;
  userPtr = userData;
  processedClusters = 0;
  fileMap.clear();
  visitedClusters.clear();
  nextSyntheticKey = 0;

  // Start from Root Directory (root entries have no parent key)
  if (!ProcessDirectory(rootDirectoryCluster, false, 0, 0, 0))
    return false;

  if (onFileFound) {
    for (const auto &kv : fileMap) {
      if (!kv.second.IsDirectory)
        onFileFound(VolumeCommon::BuildFullPath(fileMap, kv.second, currentDrive));
    }
  }

  if (progressCb)
    progressCb(100, 100, userPtr);

  return true;
}

bool exFatReader::ReadDirectoryBytes(uint32_t start, bool noFatChain,
                                     uint64_t dataLength,
                                     std::vector<uint8_t> &out) {
  const uint64_t clusterBytes = (uint64_t)bytesPerSector * sectorsPerCluster;
  std::vector<uint8_t> cluster((size_t)clusterBytes);

  // Reads one cluster and appends its first `takeBytes` bytes to `out`.
  auto readCluster = [&](uint32_t c, uint64_t takeBytes) -> bool {
    uint64_t byteOffset = ClusterToSector(c) * bytesPerSector;
    if (!VolumeCommon::ReadVolumeExact(hVolume, byteOffset, cluster.data(),
                                       (DWORD)clusterBytes)) {
      SetError(L"Failed to read exFAT directory cluster");
      return false;
    }
    out.insert(out.end(), cluster.begin(),
               cluster.begin() + (size_t)takeBytes);
    processedClusters++;
    return true;
  };

  if (noFatChain) {
    // Contiguous directory: clusters are start, start+1, ... with no FAT chain.
    const uint64_t count = (dataLength + clusterBytes - 1) / clusterBytes;
    for (uint64_t k = 0; k < count; ++k) {
      uint64_t c = (uint64_t)start + k;
      if (!IsDataCluster(c)) {
        SetError(L"exFAT directory extends beyond the cluster heap");
        return false;
      }
      if (!visitedClusters.insert((uint32_t)c).second)
        break;
      uint64_t remaining = dataLength - k * clusterBytes;
      if (!readCluster((uint32_t)c, (std::min)(clusterBytes, remaining)))
        return false;
    }
    return true;
  }

  uint32_t current = start;
  while (IsDataCluster(current)) {
    if (!visitedClusters.insert(current).second)
      break; // Cycle or cross-linked chain: end of chain
    if (!readCluster(current, clusterBytes))
      return false;
    uint32_t next = 0;
    if (!GetNextCluster(current, next))
      return false;
    current = next;
  }
  return true;
}

void exFatReader::ParseEntrySets(const std::vector<uint8_t> &data,
                                 uint64_t parentKey,
                                 std::vector<PendingDir> &subdirs) {
  // `data` is the whole directory as one byte stream, so an entry set that
  // spans a cluster boundary is parsed as a unit.
  const size_t n = data.size();
  for (size_t i = 0; i + VolumeCommon::kDirEntrySize <= n;
       i += VolumeCommon::kDirEntrySize) {
    const uint8_t entryType = data[i];
    if (entryType == EXFAT_ENTRY_TYPE_END)
      return;
    if (!(entryType & 0x80))
      continue; // Inactive entry
    if (entryType != EXFAT_ENTRY_TYPE_FILE)
      continue;

    const EXFAT_FILE_ENTRY *fe =
        reinterpret_cast<const EXFAT_FILE_ENTRY *>(&data[i]);
    const size_t secondaryCount = fe->SecondaryCount;
    // A file set needs a stream extension plus at least one name entry.
    if (secondaryCount < 2 ||
        i + VolumeCommon::kDirEntrySize * (1 + secondaryCount) > n)
      continue;

    const EXFAT_STREAM_EXTENSION_ENTRY *se =
        reinterpret_cast<const EXFAT_STREAM_EXTENSION_ENTRY *>(
            &data[i + VolumeCommon::kDirEntrySize]);
    if (se->EntryType != EXFAT_ENTRY_TYPE_STREAM_EXT)
      continue;

    const size_t setBytes = VolumeCommon::kDirEntrySize * (1 + secondaryCount);
    if (!EntrySetChecksumOk(&data[i], setBytes, fe->SetChecksum))
      continue;

    const size_t nameLen = se->NameLength;
    if (nameLen == 0 || nameLen > 255)
      continue;

    // Collect exactly NameLength characters, 15 per name entry.
    std::wstring fileName;
    size_t remaining = nameLen;
    bool nameOk = true;
    for (size_t k = 0; k + 1 < secondaryCount && remaining > 0; ++k) {
      const uint8_t *ne = &data[i + 2 * VolumeCommon::kDirEntrySize + k * VolumeCommon::kDirEntrySize];
      if (ne[0] != EXFAT_ENTRY_TYPE_FILENAME) {
        nameOk = false;
        break;
      }
      size_t take = std::min<size_t>(15, remaining);
      VolumeCommon::AppendUtf16Le(fileName, ne + 2, take); // FileName @ offset 2
      remaining -= take;
    }
    if (!nameOk || remaining != 0)
      continue;

    Entry entry;
    entry.FirstCluster = se->FirstCluster;
    entry.ParentKey = parentKey;
    entry.Name = fileName;
    entry.Size = se->DataLength;
    entry.IsDirectory = (fe->FileAttributes & 0x10) != 0;
    entry.LastWriteTime = VolumeCommon::ExFatTimestampToFileTime(
        fe->LastModifiedTimestamp, fe->LastModified10msIncrement);
    entry.IsValid = true;

    uint64_t key = entry.FirstCluster;
    if (entry.FirstCluster == 0 || fileMap.count(key))
      key = VolumeCommon::AllocateSyntheticKey(nextSyntheticKey);
    fileMap.emplace(key, entry);

    if (entry.IsDirectory && entry.FirstCluster >= 2) {
      bool noChain = (se->GeneralSecondaryFlags & EXFAT_FLAG_NO_FAT_CHAIN) != 0;
      subdirs.push_back({entry.FirstCluster, noChain, entry.Size, key});
    }

    // Skip the rest of this set (the loop adds one more entry).
    i += VolumeCommon::kDirEntrySize * secondaryCount;
  }
}

bool exFatReader::ProcessDirectory(uint32_t cluster, bool noFatChain,
                                   uint64_t dataLength, uint64_t parentKey,
                                   uint32_t depth) {
  if (depth > VolumeCommon::kMaxDirectoryDepth) {
    SetError(L"Directory nesting exceeds limit");
    return false;
  }

  std::vector<uint8_t> data;
  if (!ReadDirectoryBytes(cluster, noFatChain, dataLength, data))
    return false;

  std::vector<PendingDir> subdirs;
  ParseEntrySets(data, parentKey, subdirs);

  if (progressCb && clusterCount > 0) {
    progressCb((int)std::min<uint64_t>(99, (processedClusters * 100) / clusterCount),
               100, userPtr);
  }

  // Release this directory's bytes before recursing.
  data.clear();
  data.shrink_to_fit();

  for (const PendingDir &d : subdirs) {
    if (!ProcessDirectory(d.cluster, d.noFatChain, d.dataLength, d.key,
                          depth + 1))
      return false;
  }
  return true;
}

std::vector<FileResult> exFatReader::Search(const std::wstring &query,
                                            const std::wstring &targetFolder,
                                            const SearchOptions &options,
                                            int maxResults) {
  return VolumeCommon::SearchEntries(fileMap, currentDrive, query, targetFolder,
                                     options, maxResults);
}
