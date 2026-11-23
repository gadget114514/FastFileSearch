#include "FatReader.h"
#include <algorithm>
#include <cstring>
#include <new>

namespace {

constexpr uint64_t kMaxFatBytes = 512ull * 1024 * 1024;
constexpr uint64_t kMaxClusterBytes = 32ull * 1024 * 1024;

bool IsPowerOfTwo(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

// Checksum over the 11-byte 8.3 name, used to tie LFN fragments to their entry.
uint8_t ShortNameChecksum(const uint8_t *name) {
  uint8_t sum = 0;
  for (int i = 0; i < 11; ++i)
    sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + name[i]);
  return sum;
}

// Converts an 8.3 name using the given code page. Returns "" on failure.
std::wstring ShortNameToWide(const uint8_t *name, int codePage) {
  char sfn[13];
  int p = 0;
  for (int j = 0; j < 8; j++) {
    if (name[j] != ' ') {
      // 0x05 in the first byte stands for a literal 0xE5.
      sfn[p++] = (char)((j == 0 && name[j] == 0x05) ? 0xE5 : name[j]);
    }
  }
  if (name[8] != ' ') {
    sfn[p++] = '.';
    for (int j = 8; j < 11; j++)
      if (name[j] != ' ')
        sfn[p++] = (char)name[j];
  }
  sfn[p] = 0;

  int len = MultiByteToWideChar(codePage, 0, sfn, -1, NULL, 0);
  if (len <= 0)
    return L"";
  std::vector<wchar_t> wbuf(len);
  if (MultiByteToWideChar(codePage, 0, sfn, -1, wbuf.data(), len) <= 0)
    return L"";
  return std::wstring(wbuf.data());
}

} // namespace

FatReader::FatReader()
    : hVolume(INVALID_HANDLE_VALUE), currentDrive(0), nextSyntheticKey(0),
      bytesPerSector(0), sectorsPerCluster(0), reservedSectors(0), fatCount(0),
      sectorsPerFat(0), rootDirEntries(0), rootDirSectors(0),
      firstDataSector(0), totalSectors(0), clusterCount(0), isFat32(false),
      isFat12(false), rootCluster(0), totalUsedClusters(0),
      processedClusters(0), progressCb(nullptr), userPtr(nullptr) {}

FatReader::~FatReader() { Close(); }

void FatReader::SetError(const std::wstring &msg) {
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

void FatReader::Close() {
  if (hVolume != INVALID_HANDLE_VALUE) {
    CloseHandle(hVolume);
    hVolume = INVALID_HANDLE_VALUE;
  }
  fileMap.clear();
  visitedClusters.clear();
  fatCache.clear();
}

std::wstring FatReader::GetLastErrorMessage() const { return lastError; }

bool FatReader::Initialize(TCHAR driveLetter) {
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

  // On any failure, record the error and release the handle so a later Scan
  // cannot run against half-initialised geometry.
  auto fail = [this](const wchar_t *msg) {
    SetError(msg);
    Close();
    return false;
  };

  uint8_t bootSector[512];
  if (!VolumeCommon::ReadVolumeExact(hVolume, 0, bootSector, sizeof(bootSector)))
    return fail(L"Failed to read boot sector");
  if (bootSector[510] != 0x55 || bootSector[511] != 0xAA)
    return fail(L"Invalid boot sector signature");

  const FAT16_BPB *bpb16 = reinterpret_cast<const FAT16_BPB *>(bootSector);
  const uint32_t bps = bpb16->BytesPerSector;
  if (bps < 512 || bps > 4096 || !IsPowerOfTwo(bps))
    return fail(L"Invalid bytes-per-sector in BPB");

  const uint32_t spc = bpb16->SectorsPerCluster;
  if (!IsPowerOfTwo(spc) || (uint64_t)spc * bps > kMaxClusterBytes)
    return fail(L"Invalid sectors-per-cluster in BPB");

  const uint32_t reserved = bpb16->ReservedSectors;
  const uint32_t fats = bpb16->Fats;
  if (reserved == 0 || fats == 0)
    return fail(L"Invalid reserved-sector or FAT count in BPB");

  bytesPerSector = bps;
  sectorsPerCluster = spc;
  reservedSectors = reserved;
  fatCount = fats;

  uint64_t firstData = 0;
  uint64_t totalSecs = 0;
  if (bpb16->SectorsPerFat != 0) {
    // FAT12/16
    isFat32 = false;
    sectorsPerFat = bpb16->SectorsPerFat;
    rootDirEntries = bpb16->RootEntries;
    if (rootDirEntries == 0)
      return fail(L"Invalid root directory entry count");
    rootDirSectors = ((rootDirEntries * 32) + (bps - 1)) / bps;
    firstData = (uint64_t)reserved + (uint64_t)fats * sectorsPerFat +
                rootDirSectors;
    totalSecs = bpb16->SmallSectors ? bpb16->SmallSectors
                                    : bpb16->LargeSectors;
  } else {
    // FAT32
    FAT32_BPB *bpb32 = (FAT32_BPB *)bootSector;
    isFat32 = true;
    sectorsPerFat = bpb32->SectorsPerFat32;
    if (sectorsPerFat == 0)
      return fail(L"Invalid FAT32 sectors-per-FAT");
    rootDirEntries = 0;
    rootDirSectors = 0;
    rootCluster = bpb32->RootCluster;
    firstData = (uint64_t)reserved + (uint64_t)fats * sectorsPerFat;
    totalSecs = bpb16->LargeSectors;
  }

  if (totalSecs == 0 || firstData >= totalSecs)
    return fail(L"Invalid FAT volume geometry");
  firstDataSector = (uint32_t)firstData;
  totalSectors = (uint32_t)totalSecs;
  clusterCount = (totalSecs - firstData) / spc;
  if (clusterCount == 0)
    return fail(L"FAT volume has no data clusters");

  if (isFat32) {
    if (rootCluster < 2 || !IsDataCluster(rootCluster))
      return fail(L"Invalid FAT32 root cluster");
  } else {
    // Microsoft's cluster-count rule decides the FAT type; the FAT-size
    // field alone cannot tell FAT12 from FAT16.
    isFat12 = clusterCount < 4085;
  }

  if (!LoadFat())
    return fail(L"Failed to load FAT");

  return true;
}

bool FatReader::LoadFat() {
  if (hVolume == INVALID_HANDLE_VALUE)
    return false;

  uint64_t fatBytes = (uint64_t)sectorsPerFat * bytesPerSector;
  if (fatBytes == 0 || fatBytes > kMaxFatBytes) {
    SetError(L"Unsupported FAT size");
    return false;
  }

  uint64_t fatOffset = (uint64_t)reservedSectors * bytesPerSector;
  try {
    fatCache.assign((size_t)fatBytes, 0);
  } catch (const std::bad_alloc &) {
    SetError(L"Out of memory loading FAT");
    return false;
  }

  if (!VolumeCommon::ReadVolumeExact(hVolume, fatOffset, fatCache.data(),
                                     (DWORD)fatBytes)) {
    SetError(L"Failed to read FAT");
    return false;
  }

  // Count allocated clusters (progress denominator).
  totalUsedClusters = 0;
  for (uint64_t c = 2; c < clusterCount + 2; ++c) {
    uint32_t val = GetNextCluster((uint32_t)c);
    if (val != 0 && !(isFat32 ? val >= 0x0FFFFFF7
                              : isFat12 ? val >= 0xFF7 : val >= 0xFFF7))
      totalUsedClusters++;
  }
  return true;
}

bool FatReader::IsDataCluster(uint64_t cluster) const {
  return cluster >= 2 && cluster - 2 < clusterCount;
}

bool FatReader::IsEndOfChain(uint32_t value) const {
  if (isFat32)
    return value >= 0x0FFFFFF8;
  if (isFat12)
    return value >= 0xFF8;
  return value >= 0xFFF8;
}

uint32_t FatReader::GetNextCluster(uint32_t cluster) {
  // Out-of-range reads report end-of-chain so traversal stops.
  if (isFat32) {
    uint64_t off = (uint64_t)cluster * 4;
    if (off + 4 > fatCache.size())
      return 0x0FFFFFFF;
    uint32_t next;
    std::memcpy(&next, &fatCache[(size_t)off], 4);
    return next & 0x0FFFFFFF;
  }
  if (isFat12) {
    // Two 12-bit entries share three bytes: even clusters use the low 12 bits
    // of the pair, odd clusters the high 12 bits.
    uint64_t off = (uint64_t)cluster + cluster / 2;
    if (off + 2 > fatCache.size())
      return 0xFFF;
    uint16_t pair = (uint16_t)(fatCache[(size_t)off] |
                               ((uint16_t)fatCache[(size_t)off + 1] << 8));
    return (cluster & 1) ? (uint32_t)(pair >> 4) : (uint32_t)(pair & 0x0FFF);
  }
  uint64_t off = (uint64_t)cluster * 2;
  if (off + 2 > fatCache.size())
    return 0xFFFF;
  uint16_t next;
  std::memcpy(&next, &fatCache[(size_t)off], 2);
  return next;
}

uint64_t FatReader::ClusterToSector(uint32_t cluster) {
  // Callers only pass clusters accepted by IsDataCluster, so cluster >= 2.
  return firstDataSector + (uint64_t)(cluster - 2) * sectorsPerCluster;
}

bool FatReader::Scan(int codePage, void (*progressCallback)(int, int, void *),
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
  if (totalUsedClusters == 0)
    totalUsedClusters = 1; // Prevent div/0

  if (isFat32) {
    if (!ProcessDirectory(rootCluster, 0, codePage, 0))
      return false;
  } else {
    // FAT12/16 root is a fixed region, not a cluster chain.
    uint64_t rootOffset =
        ((uint64_t)reservedSectors + (uint64_t)fatCount * sectorsPerFat) *
        bytesPerSector;
    uint32_t rootSize = rootDirEntries * 32;
    std::vector<uint8_t> buffer(rootSize);
    if (!VolumeCommon::ReadVolumeExact(hVolume, rootOffset, buffer.data(),
                                       rootSize)) {
      SetError(L"Failed to read FAT root directory");
      return false;
    }

    LfnState lfn;
    std::vector<PendingDir> subdirs;
    bool sawEnd = false;
    ParseEntries(buffer.data(), buffer.size(), 0, codePage, lfn, subdirs,
                 sawEnd);
    for (const PendingDir &d : subdirs) {
      if (!ProcessDirectory(d.cluster, d.key, codePage, 1))
        return false;
    }
  }

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

bool FatReader::ProcessDirectory(uint32_t cluster, uint64_t parentKey,
                                 int codePage, uint32_t depth) {
  if (depth > VolumeCommon::kMaxDirectoryDepth) {
    SetError(L"Directory nesting exceeds limit");
    return false;
  }

  const uint64_t clusterBytes = (uint64_t)bytesPerSector * sectorsPerCluster;
  std::vector<uint8_t> buffer((size_t)clusterBytes);
  LfnState lfn;
  std::vector<PendingDir> subdirs;

  uint32_t current = cluster;
  while (IsDataCluster(current)) {
    // A revisited cluster means a cycle or cross-linked chain: end the chain.
    if (!visitedClusters.insert(current).second)
      break;

    processedClusters++;
    if (progressCb && (processedClusters % 100 == 0)) {
      progressCb((int)std::min<uint64_t>(
                     99, (processedClusters * 100) / totalUsedClusters),
                 100, userPtr);
    }

    uint64_t byteOffset = ClusterToSector(current) * bytesPerSector;
    if (!VolumeCommon::ReadVolumeExact(hVolume, byteOffset, buffer.data(),
                                       (DWORD)clusterBytes)) {
      SetError(L"Failed to read directory cluster");
      return false;
    }

    bool sawEnd = false;
    ParseEntries(buffer.data(), buffer.size(), parentKey, codePage, lfn,
                 subdirs, sawEnd);
    if (sawEnd)
      break;

    current = GetNextCluster(current);
  }

  // Release the cluster buffer before recursing.
  buffer.clear();
  buffer.shrink_to_fit();

  for (const PendingDir &d : subdirs) {
    if (!ProcessDirectory(d.cluster, d.key, codePage, depth + 1))
      return false;
  }
  return true;
}

void FatReader::AddLfnFragment(LfnState &lfn, const FAT_LFN_ENTRY *le) {
  const uint8_t raw = le->SequenceNumber;
  const uint8_t seq = raw & 0x1F;
  if (seq == 0 || seq > LfnState::kMaxFragments) {
    lfn.Reset();
    return;
  }

  if (raw & 0x40) {
    // The highest-numbered fragment comes first on disk and starts a new name.
    lfn.Reset();
    lfn.active = true;
    lfn.total = seq;
    lfn.expectedSeq = seq;
    lfn.checksum = le->Checksum;
  } else if (!lfn.active || seq != lfn.expectedSeq ||
             le->Checksum != lfn.checksum) {
    // Out-of-order fragment or checksum mismatch: discard the whole name.
    lfn.Reset();
    return;
  }

  uint8_t bytes[26]; // 13 UTF-16 code units
  std::memcpy(bytes, le->Name1, 10);
  std::memcpy(bytes + 10, le->Name2, 12);
  std::memcpy(bytes + 22, le->Name3, 4);

  std::wstring &part = lfn.parts[seq - 1];
  part.clear();
  VolumeCommon::AppendUtf16Le(part, bytes, 13);
  lfn.expectedSeq = seq - 1;
}

void FatReader::ParseEntries(const uint8_t *data, size_t size,
                             uint64_t parentKey, int codePage, LfnState &lfn,
                             std::vector<PendingDir> &subdirs, bool &sawEnd) {
  const uint64_t clusterBytes = (uint64_t)bytesPerSector * sectorsPerCluster;

  for (size_t off = 0; off + VolumeCommon::kDirEntrySize <= size;
       off += VolumeCommon::kDirEntrySize) {
    const FAT_DIRECTORY_ENTRY *de =
        reinterpret_cast<const FAT_DIRECTORY_ENTRY *>(data + off);

    if (de->Name[0] == 0x00) { // End of directory
      sawEnd = true;
      return;
    }
    if (de->Name[0] == 0xE5) { // Deleted entry
      lfn.Reset();
      continue;
    }
    if ((de->Attributes & 0x0F) == FAT_ATTR_LFN) {
      AddLfnFragment(lfn, reinterpret_cast<const FAT_LFN_ENTRY *>(de));
      continue;
    }
    if (de->Attributes & FAT_ATTR_VOLUME_ID) {
      lfn.Reset();
      continue;
    }
    if (de->Name[0] == '.') { // "." and ".."
      lfn.Reset();
      continue;
    }

    // Use the LFN only if it is complete and its checksum matches this 8.3 name.
    const bool useLfn = lfn.active && lfn.expectedSeq == 0 &&
                        lfn.checksum == ShortNameChecksum(de->Name);
    std::wstring name = useLfn ? lfn.Assemble() : ShortNameToWide(de->Name, codePage);
    lfn.Reset();
    if (name.empty())
      continue;

    Entry entry;
    entry.FirstCluster = (de->FirstClusterLow |
                          ((uint32_t)de->FirstClusterHigh << 16)) &
                         0x0FFFFFFF;
    entry.ParentKey = parentKey;
    entry.Name = name;
    entry.Size = de->FileSize;
    entry.IsDirectory = (de->Attributes & FAT_ATTR_DIRECTORY) != 0;
    entry.LastWriteTime = VolumeCommon::DosDateTimeToFileTime(de->WriteDate,
                                                             de->WriteTime);
    entry.IsValid = true;

    uint64_t key = entry.FirstCluster;
    if (entry.FirstCluster == 0 || fileMap.count(key))
      key = VolumeCommon::AllocateSyntheticKey(nextSyntheticKey);
    fileMap.emplace(key, entry);

    if (!entry.IsDirectory) {
      processedClusters += (entry.Size + clusterBytes - 1) / clusterBytes;
    } else if (entry.FirstCluster >= 2) {
      subdirs.push_back({entry.FirstCluster, key});
    }
  }
}

std::vector<FileResult> FatReader::Search(const std::wstring &query,
                                          const std::wstring &targetFolder,
                                          int codePage,
                                          const SearchOptions &options,
                                          int maxResults) {
  (void)codePage; // Names are decoded at scan time
  return VolumeCommon::SearchEntries(fileMap, currentDrive, query, targetFolder,
                                     options, maxResults);
}
