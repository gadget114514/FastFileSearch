#define WIN32_LEAN_AND_MEAN
#include "FatReader.h"
#include "MFTReader.h"
#include "exFatReader.h"
#include "resource.h"
#include <atomic>
#include <commctrl.h>
#include <commdlg.h>
#include <fstream>

#if 0
#include <iomanip>
#endif
#include <process.h>
#include <set>
#include <shlobj.h>
#include <string>
#include <vector>
#include <windows.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

// Globals
HINSTANCE hInstBuffer;
MFTReader mftReader;
FatReader fatReader;
exFatReader exFatReaderObj;
std::vector<FileResult> searchResults;
std::atomic<bool> isSearching(false);
HWND hList = NULL;
HWND hTargetList = NULL;
int currentLang = 0; // 0=English, 1=Japanese
int currentCodePage = CP_OEMCP;
std::vector<std::wstring> searchTargets;

// Sorting Globals
int sortColumn = -1;
bool sortAscending = true;

// Subclassing Globals for Startup Fix
WNDPROC oldEditProc = NULL;
WNDPROC oldListProc = NULL;
bool isStartupGuarded = true;

// Guarded Procedures to ignore disruptive messages during startup
LRESULT CALLBACK GuardedEditProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                 LPARAM lParam) {
  if (isStartupGuarded) {
    if (uMsg == WM_KILLFOCUS)
      return 0; // Ignore focus loss during guard
  }
  return CallWindowProc(oldEditProc, hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK GuardedListProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                 LPARAM lParam) {
  if (isStartupGuarded) {
    // Ignore all mouse messages and focus/capture during guard to prevent
    // "dragging mode"
    if (uMsg >= WM_MOUSEFIRST && uMsg <= WM_MOUSELAST)
      return 0;
    if (uMsg == WM_SETFOCUS || uMsg == WM_KILLFOCUS)
      return 0;
    if (uMsg == WM_CAPTURECHANGED || uMsg == WM_CANCELMODE)
      return 0;
    if (uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP || uMsg == WM_MOUSEMOVE)
      return 0;
  }
  return CallWindowProc(oldListProc, hWnd, uMsg, wParam, lParam);
}

// Prototypes
void SaveConfig(HWND hDlg);
void LoadConfig(HWND hDlg);
void ResizeLayout(HWND hDlg, int cx, int cy);
int CALLBACK CompareFunc(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort);
std::wstring FormatSize(uint64_t size);

// Strings
const wchar_t *STR_SEARCH[] = {L"Search", L"検索"};
const wchar_t *STR_FILENAME[] = {L"Search Filename:", L"検索ファイル名:"};
const wchar_t *STR_STATUS_READY[] = {L"Ready", L"準備完了"};
const wchar_t *STR_STATUS_BUSY[] = {L"Searching...", L"検索中..."};
const wchar_t *STR_STATUS_DONE[] = {L"Found %d items", L"%d 件見つかりました"};
const wchar_t *STR_COL_NAME[] = {L"Name", L"名前"};
const wchar_t *STR_COL_PATH[] = {L"Path", L"パス"};
const wchar_t *STR_COL_DATE[] = {L"Modified Date", L"更新日時"};
const wchar_t *STR_COL_SIZE[] = {L"Size", L"サイズ"};
const wchar_t *STR_SAVE[] = {L"Save Results", L"結果を保存"};
const wchar_t *STR_TARGETS[] = {L"Search Candidates:", L"検索対象:"};
const wchar_t *STR_ADD[] = {L"Add Folder", L"フォルダ追加"};
const wchar_t *STR_REMOVE[] = {L"Not candidate", L"検索対象外"};

std::wstring FormatDate(uint64_t fileTime) {
  FILETIME ft;
  ft.dwLowDateTime = (DWORD)fileTime;
  ft.dwHighDateTime = (DWORD)(fileTime >> 32);

  SYSTEMTIME stUTC, stLocal;
  FileTimeToSystemTime(&ft, &stUTC);
  SystemTimeToTzSpecificLocalTime(NULL, &stUTC, &stLocal);

  wchar_t buf[64];
  swprintf_s(buf, L"%04d/%02d/%02d %02d:%02d:%02d", stLocal.wYear,
             stLocal.wMonth, stLocal.wDay, stLocal.wHour, stLocal.wMinute,
             stLocal.wSecond);
  return buf;
}

std::wstring FormatSize(uint64_t size) {
  if (size < 1024)
    return std::to_wstring(size) + L" B";
  if (size < 1024 * 1024)
    return std::to_wstring(size / 1024) + L" KB";
  if (size < 1024 * 1024 * 1024)
    return std::to_wstring(size / (1024 * 1024)) + L" MB";
  return std::to_wstring(size / (1024 * 1024 * 1024)) + L" GB";
}

void UpdateLanguage(HWND hDlg) {
  SetDlgItemTextW(hDlg, IDC_BTN_SEARCH, STR_SEARCH[currentLang]);
  SetDlgItemTextW(hDlg, IDC_STATIC_FILENAME, STR_FILENAME[currentLang]);
  SetDlgItemTextW(hDlg, IDC_BTN_SAVE, STR_SAVE[currentLang]);
  SetDlgItemTextW(hDlg, IDC_STATIC_TARGETS, STR_TARGETS[currentLang]);
  SetDlgItemTextW(hDlg, IDC_BTN_ADD, STR_ADD[currentLang]);
  SetDlgItemTextW(hDlg, IDC_BTN_REMOVE, STR_REMOVE[currentLang]);

  // Update List Columns
  LVCOLUMN lvc;
  lvc.mask = LVCF_TEXT;
  lvc.pszText = (LPWSTR)STR_COL_NAME[currentLang];
  ListView_SetColumn(hList, 0, &lvc);
  lvc.pszText = (LPWSTR)STR_COL_PATH[currentLang];
  ListView_SetColumn(hList, 1, &lvc);
  lvc.pszText = (LPWSTR)STR_COL_DATE[currentLang];
  ListView_SetColumn(hList, 2, &lvc);
  lvc.pszText = (LPWSTR)STR_COL_SIZE[currentLang];
  ListView_SetColumn(hList, 3, &lvc);

  SetDlgItemTextW(hDlg, IDC_STATIC_CP,
                  currentLang == 0 ? L"Code Page:" : L"コードページ:");
}

void ScanThread(void *param) {
  HWND hDlg = (HWND)param;

  wchar_t queryBuf[256];
  GetDlgItemTextW(hDlg, IDC_EDIT_QUERY, queryBuf, 256);
  std::wstring query = queryBuf;

  searchResults.clear();

  std::set<wchar_t> drivesToScan;
  for (const auto &target : searchTargets) {
    if (target.length() >= 3 && target[1] == L':') {
      drivesToScan.insert(towupper(target[0]));
    }
  }

  auto callback = [](int percent, int max, void *userData) {
    // Post progress message (WM_USER + 3)
    // wParam = percent
    HWND hDlg = (HWND)userData;
    PostMessage(hDlg, WM_USER + 3, (WPARAM)percent, 0);
  };

  bool anySuccess = false;

  for (wchar_t drive : drivesToScan) {
    wchar_t driveRoot[] = {drive, L':', L'\\', L'\0'};
    wchar_t fsName[MAX_PATH];
    bool fsOk = GetVolumeInformationW(driveRoot, NULL, 0, NULL, NULL, NULL,
                                      fsName, MAX_PATH);

    if (fsOk) {
      bool scanSuccess = false;
      bool isNtfs = (wcscmp(fsName, L"NTFS") == 0);
      bool isFat =
          (wcscmp(fsName, L"FAT") == 0 || wcscmp(fsName, L"FAT32") == 0);
      bool isExFat = (wcscmp(fsName, L"exFAT") == 0);

      // Reset Progress for next drive
      callback(0, 100, hDlg);

      if (isNtfs) {
        if (mftReader.Initialize(drive) && mftReader.Scan(callback, hDlg))
          scanSuccess = true;
      } else if (isFat) {
        if (fatReader.Initialize(drive) &&
            fatReader.Scan(currentCodePage, callback, hDlg))
          scanSuccess = true;
      } else if (isExFat) {
        if (exFatReaderObj.Initialize(drive) &&
            exFatReaderObj.Scan(callback, hDlg))
          scanSuccess = true;
      }

      callback(100, 100, hDlg); // Ensure 100% at end

      if (scanSuccess) {
        std::vector<std::wstring> driveTargets;
        for (const auto &t : searchTargets) {
          if (t.length() >= 3 && towupper(t[0]) == drive) {
            driveTargets.push_back(t);
          }
        }

        for (const auto &targetFolder : driveTargets) {
          std::vector<FileResult> results;
          if (isNtfs)
            results = mftReader.Search(query, targetFolder, false, 50000);
          else if (isFat)
            results = fatReader.Search(query, targetFolder, currentCodePage,
                                       false, 50000);
          else if (isExFat)
            results = exFatReaderObj.Search(query, targetFolder, false, 50000);

          searchResults.insert(searchResults.end(), results.begin(),
                               results.end());
          if (searchResults.size() >= 50000)
            break;
        }
        anySuccess = true;
      }
    }
  }

  if (anySuccess) {
    PostMessage(hDlg, WM_USER + 1, 0, 0);
  } else {
    // If we scanned nothing or failed all
    PostMessage(hDlg, WM_USER + 2, 0, 0);
  }

  isSearching = false;
}

void PopulateList() {
  ListView_DeleteAllItems(hList);
  SendMessage(hList, WM_SETREDRAW, FALSE, 0);

  LVITEM lvI;
  lvI.mask = LVIF_TEXT | LVIF_STATE;
  lvI.stateMask = 0;
  lvI.state = 0;

  for (size_t i = 0; i < searchResults.size(); ++i) {
    memset(&lvI, 0, sizeof(lvI));
    lvI.mask = LVIF_TEXT | LVIF_PARAM | LVIF_STATE;
    lvI.state = 0;
    lvI.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    lvI.iItem = (int)i;
    lvI.iSubItem = 0;
    lvI.lParam = (LPARAM)i;
    lvI.pszText = (LPWSTR)searchResults[i].Name.c_str();
    int idx = ListView_InsertItem(hList, &lvI);

    ListView_SetItemText(hList, idx, 1,
                         (LPWSTR)searchResults[i].FullPath.c_str());

    std::wstring dateStr = FormatDate(searchResults[i].LastWriteTime);
    ListView_SetItemText(hList, idx, 2, (LPWSTR)dateStr.c_str());

    std::wstring sizeStr = FormatSize(searchResults[i].Size);
    ListView_SetItemText(hList, idx, 3, (LPWSTR)sizeStr.c_str());
  }

  SendMessage(hList, WM_SETREDRAW, TRUE, 0);

  // 1. Force focus away immediately
  SetFocus(GetDlgItem(GetParent(hList), IDC_EDIT_QUERY));

  // 2. Clear state strictly
  ListView_SetItemState(hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
  ListView_SetSelectionMark(hList, -1);

  // 3. Force Repaint
  InvalidateRect(hList, NULL, TRUE);
  UpdateWindow(hList);
}

void SaveResults(HWND hDlg) {
  OPENFILENAMEW ofn;
  wchar_t fileName[MAX_PATH] = L"scan_result.txt";

  ZeroMemory(&ofn, sizeof(ofn));
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hDlg;
  ofn.lpstrFilter = L"Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0";
  ofn.lpstrFile = fileName;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags =
      OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
  ofn.lpstrDefExt = L"txt";

  if (GetSaveFileNameW(&ofn)) {
    std::wofstream outfile(fileName);
    outfile << L"Name\tPath\tDate\tSize\n"; // Header
    for (const auto &res : searchResults) {
      outfile << res.Name << L"\t" << res.FullPath << L"\t"
              << FormatDate(res.LastWriteTime) << L"\t" << res.Size << L"\n";
    }
  }
}

void AddFolder(HWND hDlg) {
  BROWSEINFOW bi = {0};
  bi.hwndOwner = hDlg;
  bi.lpszTitle = L"Select Folder to Search";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

  LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
  if (pidl != 0) {
    wchar_t path[MAX_PATH];
    if (SHGetPathFromIDListW(pidl, path)) {
      // Check duplicates
      bool found = false;
      for (const auto &t : searchTargets) {
        if (t == path)
          found = true;
      }
      if (!found) {
        searchTargets.push_back(path);
        SendMessage(hTargetList, LB_ADDSTRING, 0, (LPARAM)path);
      }
    }
    CoTaskMemFree(pidl);
  }
}

void RemoveFolder(HWND hDlg) {
  int idx = (int)SendMessage(hTargetList, LB_GETCURSEL, 0, 0);
  if (idx != LB_ERR) {
    wchar_t buf[MAX_PATH];
    SendMessage(hTargetList, LB_GETTEXT, idx, (LPARAM)buf);
    SendMessage(hTargetList, LB_DELETESTRING, idx, 0);

    // Remove from vector
    for (auto it = searchTargets.begin(); it != searchTargets.end(); ++it) {
      if (*it == buf) {
        searchTargets.erase(it);
        break;
      }
    }
  }
}

std::wstring GetConfigPath(bool createDir) {
  wchar_t path[MAX_PATH];
  if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT,
                                 path))) {
    std::wstring dir = std::wstring(path) + L"\\FastFileSearch";
    if (createDir) {
      CreateDirectoryW(dir.c_str(), NULL);
    }
    return dir + L"\\FastFileSearch.ini";
  }
  // Fallback to local
  return L"FastFileSearch.ini";
}

void SaveConfig(HWND hDlg) {
  std::wstring iniPath = GetConfigPath(true);

  // Save Window Size
  RECT rc;
  if (GetWindowRect(hDlg, &rc)) {
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    WritePrivateProfileStringW(L"Settings", L"Width",
                               std::to_wstring(w).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(L"Settings", L"Height",
                               std::to_wstring(h).c_str(), iniPath.c_str());
  }

  // Save Language
  WritePrivateProfileStringW(L"Settings", L"Language",
                             std::to_wstring(currentLang).c_str(),
                             iniPath.c_str());

  // Save Code Page
  WritePrivateProfileStringW(L"Settings", L"CodePage",
                             std::to_wstring(currentCodePage).c_str(),
                             iniPath.c_str());

  // Clear old Targets
  WritePrivateProfileStringW(L"Targets", NULL, NULL, iniPath.c_str());

  // Save Count
  WritePrivateProfileStringW(L"Targets", L"Count",
                             std::to_wstring(searchTargets.size()).c_str(),
                             iniPath.c_str());

  // Save Items
  for (size_t i = 0; i < searchTargets.size(); ++i) {
    std::wstring key = L"Target" + std::to_wstring(i);
    WritePrivateProfileStringW(L"Targets", key.c_str(),
                               searchTargets[i].c_str(), iniPath.c_str());
  }
}

void LoadConfig(HWND hDlg) {
  std::wstring iniPath = GetConfigPath(false);

  // Load Window Size
  int w = GetPrivateProfileIntW(L"Settings", L"Width", 0, iniPath.c_str());
  int h = GetPrivateProfileIntW(L"Settings", L"Height", 0, iniPath.c_str());

  int screenW = GetSystemMetrics(SM_CXSCREEN);
  int screenH = GetSystemMetrics(SM_CYSCREEN);

  if (w > 300 && w <= screenW && h > 200 && h <= screenH) {
    SetWindowPos(hDlg, NULL, 0, 0, w, h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }

  // Load Language
  currentLang =
      GetPrivateProfileIntW(L"Settings", L"Language", 0, iniPath.c_str());
  if (currentLang < 0 || currentLang > 1)
    currentLang = 0;

  // Load Code Page
  currentCodePage = GetPrivateProfileIntW(L"Settings", L"CodePage", CP_OEMCP,
                                          iniPath.c_str());

  // Load Targets
  searchTargets.clear();
  SendMessage(hTargetList, LB_RESETCONTENT, 0, 0);

  int count = GetPrivateProfileIntW(L"Targets", L"Count", 0, iniPath.c_str());
  if (count > 0) {
    wchar_t buf[MAX_PATH];
    for (int i = 0; i < count; ++i) {
      std::wstring key = L"Target" + std::to_wstring(i);
      GetPrivateProfileStringW(L"Targets", key.c_str(), L"", buf, MAX_PATH,
                               iniPath.c_str());
      if (wcslen(buf) > 0) {
        searchTargets.push_back(buf);
        SendMessage(hTargetList, LB_ADDSTRING, 0, (LPARAM)buf);
      }
    }
  }

  SendDlgItemMessage(hDlg, IDC_COMBO_LANG, CB_SETCURSEL, currentLang, 0);
}

int CALLBACK CompareFunc(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort) {
  if (lParam1 >= (LPARAM)searchResults.size() ||
      lParam2 >= (LPARAM)searchResults.size())
    return 0;

  const FileResult &a = searchResults[lParam1];
  const FileResult &b = searchResults[lParam2];

  int cmp = 0;
  switch (sortColumn) {
  case 0: // Name
    cmp = lstrcmpiW(a.Name.c_str(), b.Name.c_str());
    break;
  case 1: // Path
    cmp = lstrcmpiW(a.FullPath.c_str(), b.FullPath.c_str());
    break;
  case 2: // Date
    if (a.LastWriteTime < b.LastWriteTime)
      cmp = -1;
    else if (a.LastWriteTime > b.LastWriteTime)
      cmp = 1;
    break;
  case 3: // Size
    if (a.Size < b.Size)
      cmp = -1;
    else if (a.Size > b.Size)
      cmp = 1;
    break;
  }

  return sortAscending ? cmp : -cmp;
}

void ResizeLayout(HWND hDlg, int cx, int cy) {
  if (cx == 0 || cy == 0)
    return;

  // Quick helper to move
  auto Move = [&](int id, int x, int y, int w, int h, bool size) {
    SetWindowPos(GetDlgItem(hDlg, id), NULL, x, y, w, h,
                 SWP_NOZORDER | (size ? 0 : SWP_NOSIZE));
  };

  // Margins (in pixels approx)
  int m = 10;

  // Get Button Size (Search)
  RECT rcBtn;
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_SEARCH), &rcBtn);
  int btnW = rcBtn.right - rcBtn.left;
  int btnH = rcBtn.bottom - rcBtn.top;

  // Top Row
  // Edit Query: x=m+220? No.
  // Filename Label: x=10.
  // Let's rely on original positions for "Left".
  // Buttons align to Right = cx - m - btnW.
  int btnX = cx - m - btnW;

  // Search Button
  // We need current Y.
  RECT rcS;
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_SEARCH), &rcS);
  POINT ptS = {rcS.left, rcS.top};
  ScreenToClient(hDlg, &ptS);
  Move(IDC_BTN_SEARCH, btnX, ptS.y, 0, 0, false);

  // Edit Query
  RECT rcE;
  GetWindowRect(GetDlgItem(hDlg, IDC_EDIT_QUERY), &rcE);
  POINT ptE = {rcE.left, rcE.top};
  ScreenToClient(hDlg, &ptE);
  // Width = btnX - m - ptE.x
  Move(IDC_EDIT_QUERY, ptE.x, ptE.y, btnX - m - ptE.x, rcE.bottom - rcE.top,
       true);

  // Target List Buttons
  RECT rcAdd;
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_ADD), &rcAdd);
  POINT ptAdd = {rcAdd.left, rcAdd.top};
  ScreenToClient(hDlg, &ptAdd);
  Move(IDC_BTN_ADD, btnX, ptAdd.y, 0, 0, false);

  RECT rcRem;
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_REMOVE), &rcRem);
  POINT ptRem = {rcRem.left, rcRem.top};
  ScreenToClient(hDlg, &ptRem);
  Move(IDC_BTN_REMOVE, btnX, ptRem.y, 0, 0, false);

  // Target List
  RECT rcT;
  GetWindowRect(GetDlgItem(hDlg, IDC_LIST_TARGETS), &rcT);
  POINT ptT = {rcT.left, rcT.top};
  ScreenToClient(hDlg, &ptT);
  Move(IDC_LIST_TARGETS, ptT.x, ptT.y, btnX - m - ptT.x, rcT.bottom - rcT.top,
       true);

  // Result List
  RECT rcL;
  GetWindowRect(GetDlgItem(hDlg, IDC_LIST_RESULTS), &rcL);
  POINT ptL = {rcL.left, rcL.top};
  ScreenToClient(hDlg, &ptL);
  // Height: cy - m - BottomRow - ptL.y
  int bottomRowH = 40;
  Move(IDC_LIST_RESULTS, ptL.x, ptL.y, cx - m - ptL.x, cy - bottomRowH - ptL.y,
       true);

  // Bottom Row
  int bottomY = cy - 35;
  Move(IDC_BTN_SAVE, m, bottomY, 0, 0, false);

  // Status and Progress
  RECT rcSave;
  GetWindowRect(GetDlgItem(hDlg, IDC_BTN_SAVE), &rcSave);
  int statusX = m + (rcSave.right - rcSave.left) + m;
  int availableWidth = cx - statusX - m;

  // Split width: Status gets 60%, Progress gets 40% (approx)
  int progressW = 150;
  if (availableWidth > 300)
    progressW = 200;

  int statusW = availableWidth - progressW - m;
  if (statusW < 50)
    statusW = 50; // Min width

  Move(IDC_STATUS, statusX, bottomY + 5, statusW, 20, true);
  Move(IDC_PROGRESS, statusX + statusW + m, bottomY + 2, progressW, 20, true);
}

LRESULT DrawItemWithHighlight(LPNMLVCUSTOMDRAW lplvcd, HWND hDlg) {
  int iItem = (int)lplvcd->nmcd.dwItemSpec;
  int iSubItem = lplvcd->iSubItem;
  HDC hdc = lplvcd->nmcd.hdc;

  // Get Text
  wchar_t textBuf[1024];
  ListView_GetItemText(lplvcd->nmcd.hdr.hwndFrom, iItem, iSubItem, textBuf,
                       1024);
  std::wstring text = textBuf;

  // Get Query
  wchar_t queryBuf[256];
  GetDlgItemTextW(hDlg, IDC_EDIT_QUERY, queryBuf, 256);
  std::wstring query = queryBuf;

  if (query.empty() || text.empty()) {
    return CDRF_DODEFAULT;
  }

  // Check if there is a match at all before custom drawing
  std::wstring textLower = text;
  std::wstring queryLower = query;
  for (auto &c : textLower)
    c = towlower(c);
  for (auto &c : queryLower)
    c = towlower(c);
  if (textLower.find(queryLower) == std::wstring::npos)
    return CDRF_DODEFAULT;

  // Prepare Colors
  // Check Selection State Explicitly
  bool isSelected =
      (ListView_GetItemState(lplvcd->nmcd.hdr.hwndFrom, iItem, LVIS_SELECTED) &
       LVIS_SELECTED) != 0;

  COLORREF clrText = isSelected ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                                : GetSysColor(COLOR_WINDOWTEXT);
  // COLORREF clrBk = isSelected ? GetSysColor(COLOR_HIGHLIGHT) :
  // GetSysColor(COLOR_WINDOW);
  COLORREF clrMatchBk = RGB(0, 255, 255); // Cyan
  COLORREF clrMatchText = RGB(0, 0, 0);   // Black

  // Get Rect
  RECT rc;
  ListView_GetSubItemRect(lplvcd->nmcd.hdr.hwndFrom, iItem, iSubItem,
                          LVIR_LABEL, &rc);
  if (iSubItem == 0) {
    // Fix for Item 0
    // LVIR_LABEL might be small if no icon.
    // Let's use LVIR_BOUNDS and adjust? Or trust LVIR_LABEL.
    // Usually safe.
  }

  // Fill Background
  HBRUSH hFillBr = isSelected ? GetSysColorBrush(COLOR_HIGHLIGHT)
                              : GetSysColorBrush(COLOR_WINDOW);
  FillRect(hdc, &rc, hFillBr);

  int oldBkMode = SetBkMode(hdc, TRANSPARENT);

  // Match and Draw
  size_t pos = 0;
  size_t found = textLower.find(queryLower, pos);

  int currentX = rc.left + 6; // Padding

  HFONT hFont = (HFONT)SendMessage(lplvcd->nmcd.hdr.hwndFrom, WM_GETFONT, 0, 0);
  HGDIOBJ oldFont = SelectObject(hdc, hFont);

  auto DrawSegment = [&](const std::wstring &seg, bool highlight) {
    if (seg.empty())
      return;

    SIZE sz;
    GetTextExtentPoint32W(hdc, seg.c_str(), (int)seg.length(), &sz);

    RECT segRect;
    segRect.left = currentX;
    segRect.top = rc.top;
    segRect.bottom = rc.bottom;
    segRect.right = currentX + sz.cx;

    // Clip to cell
    if (segRect.left < rc.right) {
      if (segRect.right > rc.right)
        segRect.right = rc.right;

      if (highlight) {
        HBRUSH hBr = CreateSolidBrush(clrMatchBk);
        FillRect(hdc, &segRect, hBr);
        DeleteObject(hBr);
        SetTextColor(hdc, clrMatchText);
      } else {
        SetTextColor(hdc, clrText);
      }

      RECT drawRc = segRect;
      DrawTextW(hdc, seg.c_str(), (int)seg.length(), &drawRc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    currentX += sz.cx;
  };

  while (found != std::wstring::npos) {
    if (found > pos)
      DrawSegment(text.substr(pos, found - pos), false);
    DrawSegment(text.substr(found, query.length()), true);
    pos = found + query.length();
    found = textLower.find(queryLower, pos);
  }
  if (pos < text.length())
    DrawSegment(text.substr(pos), false);

  SelectObject(hdc, oldFont);
  SetBkMode(hdc, oldBkMode);

  return CDRF_SKIPDEFAULT;
}

INT_PTR CALLBACK DialogProc(HWND hDlg, UINT uMsg, WPARAM wParam,
                            LPARAM lParam) {
  switch (uMsg) {
  case WM_SIZE:
    ResizeLayout(hDlg, LOWORD(lParam), HIWORD(lParam));
    break;
  case WM_INITDIALOG: {
    // Stage 1 Fix: Ensure no capture immediately and LOCKDOWN the list view
    ReleaseCapture();

    // Set Icon
    HICON hIconBig =
        (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON),
                         IMAGE_ICON, 32, 32, 0);
    HICON hIconSmall =
        (HICON)LoadImage(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON),
                         IMAGE_ICON, 16, 16, 0);
    SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
    SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);

    isStartupGuarded = true; // Start guarded

    hInstBuffer = GetModuleHandle(NULL);
    hList = GetDlgItem(hDlg, IDC_LIST_RESULTS);
    HWND hEdit = GetDlgItem(hDlg, IDC_EDIT_QUERY);

    // Subclass controls to ignore disruptive messages during startup
    oldEditProc = (WNDPROC)SetWindowLongPtr(hEdit, GWLP_WNDPROC,
                                            (LONG_PTR)GuardedEditProc);
    oldListProc = (WNDPROC)SetWindowLongPtr(hList, GWLP_WNDPROC,
                                            (LONG_PTR)GuardedListProc);

    // Set a 1000ms timer to settle and enforce focus
    SetTimer(hDlg, 1, 1000, NULL);

    // Temporarily disable the list to ignore double-click artifacts from launch
    EnableWindow(hList, FALSE);

    ListView_SetExtendedListViewStyle(hList,
                                      LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    hTargetList = GetDlgItem(hDlg, IDC_LIST_TARGETS);

    // Setup Columns
    LVCOLUMN lvc;
    lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
    lvc.iSubItem = 0;
    lvc.pszText = (LPWSTR)L"Name";
    lvc.cx = 200;
    lvc.fmt = LVCFMT_LEFT;
    ListView_InsertColumn(hList, 0, &lvc);

    lvc.iSubItem = 1;
    lvc.pszText = (LPWSTR)L"Path";
    lvc.cx = 300;
    ListView_InsertColumn(hList, 1, &lvc);

    lvc.iSubItem = 2;
    lvc.pszText = (LPWSTR)L"Modified Date";
    lvc.cx = 140;
    ListView_InsertColumn(hList, 2, &lvc);

    lvc.iSubItem = 3;
    lvc.pszText = (LPWSTR)L"Size";
    lvc.cx = 100;
    ListView_InsertColumn(hList, 3, &lvc);

    // Setup Languages
    SendDlgItemMessage(hDlg, IDC_COMBO_LANG, CB_ADDSTRING, 0,
                       (LPARAM)L"English");
    SendDlgItemMessage(hDlg, IDC_COMBO_LANG, CB_ADDSTRING, 0,
                       (LPARAM)L"Japanese");
    SendDlgItemMessage(hDlg, IDC_COMBO_LANG, CB_SETCURSEL, 0, 0);

    // Setup Code Pages
    HWND hCP = GetDlgItem(hDlg, IDC_COMBO_CP);
    SendMessage(hCP, CB_ADDSTRING, 0, (LPARAM)L"Auto (OEM)");
    SendMessage(hCP, CB_SETITEMDATA, 0, (LPARAM)CP_OEMCP);
    SendMessage(hCP, CB_ADDSTRING, 0, (LPARAM)L"Japanese (932)");
    SendMessage(hCP, CB_SETITEMDATA, 1, (LPARAM)932);
    SendMessage(hCP, CB_ADDSTRING, 0, (LPARAM)L"Western (1252)");
    SendMessage(hCP, CB_SETITEMDATA, 2, (LPARAM)1252);
    SendMessage(hCP, CB_ADDSTRING, 0, (LPARAM)L"Simplified Chinese (936)");
    SendMessage(hCP, CB_SETITEMDATA, 3, (LPARAM)936);
    SendMessage(hCP, CB_ADDSTRING, 0, (LPARAM)L"Traditional Chinese (950)");
    SendMessage(hCP, CB_SETITEMDATA, 4, (LPARAM)950);
    SendMessage(hCP, CB_ADDSTRING, 0, (LPARAM)L"UTF-8 (65001)");
    SendMessage(hCP, CB_SETITEMDATA, 5, (LPARAM)CP_UTF8);

    // Select current CP
    SendMessage(hCP, CB_SETCURSEL, 0, 0); // Default to Auto
    for (int i = 0; i < (int)SendMessage(hCP, CB_GETCOUNT, 0, 0); ++i) {
      if ((int)SendMessage(hCP, CB_GETITEMDATA, i, 0) == currentCodePage) {
        SendMessage(hCP, CB_SETCURSEL, i, 0);
        break;
      }
    }

    // Initial Default: Add C:\ drive?
    // Load Config
    LoadConfig(hDlg);

    if (searchTargets.empty()) {
      wchar_t defaultPath[] = L"C:\\";
      searchTargets.push_back(defaultPath);
      SendMessage(hTargetList, LB_ADDSTRING, 0, (LPARAM)defaultPath);
    }

    ListView_SetExtendedListViewStyle(hList,
                                      LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    UpdateLanguage(hDlg);

    // FIX: Set focus to the search field and return FALSE as requested.
    // This is the definitive way to set focus in a dialog during
    // initialization.
    SetFocus(GetDlgItem(hDlg, IDC_EDIT_QUERY));
    return FALSE;
  }
  case WM_USER + 3: {
    // Progress Update
    // wParam = percent
    SendDlgItemMessage(hDlg, IDC_PROGRESS, PBM_SETPOS, (WPARAM)wParam, 0);
    return TRUE;
  }
  case WM_TIMER: {
    if (wParam == 1) {
      KillTimer(hDlg, 1);

      // Release the interactive guard
      isStartupGuarded = false;

      // 1. Re-enable the list view first so it can process the cleanup messages
      EnableWindow(hList, TRUE);

      // 2. Clear any stray mouse messages that arrived during
      // startup/initialization
      MSG msg;
      while (PeekMessage(&msg, NULL, WM_MOUSEFIRST, WM_MOUSELAST, PM_REMOVE))
        ;

      // 3. Abort all internal modes (drags, marquees, mouse capture)
      ReleaseCapture();
      SendMessage(hDlg, WM_CANCELMODE, 0, 0);
      SendMessage(hList, WM_CANCELMODE, 0, 0);

      // 4. Force selection/focus reset on the list
      ListView_SetItemState(hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
      ListView_SetSelectionMark(hList, -1);

      // 5. Force focus to the query edit box as the absolute final action
      HWND hEdit = GetDlgItem(hDlg, IDC_EDIT_QUERY);
      SetFocus(hEdit);
      SendMessage(hEdit, EM_SETSEL, -1, -1);
    }
    break;
  }
  case WM_COMMAND: {
    int id = LOWORD(wParam);
    if (id == IDC_BTN_SEARCH) {
      if (!isSearching) {
        if (searchTargets.empty()) {
          MessageBoxW(hDlg, L"Please add at least one search candidate.",
                      L"Info", MB_OK);
          break;
        }
        isSearching = true;
        SetDlgItemTextW(hDlg, IDC_STATUS, STR_STATUS_BUSY[currentLang]);
        ListView_DeleteAllItems(hList);
        _beginthread(ScanThread, 0, (void *)hDlg);
      }
    } else if (id == IDC_BTN_SAVE) {
      SaveResults(hDlg);
    } else if (id == IDC_BTN_ADD) {
      AddFolder(hDlg);
    } else if (id == IDC_BTN_REMOVE) {
      RemoveFolder(hDlg);
    } else if (id == IDC_COMBO_LANG && HIWORD(wParam) == CBN_SELCHANGE) {
      currentLang =
          SendDlgItemMessage(hDlg, IDC_COMBO_LANG, CB_GETCURSEL, 0, 0);
      UpdateLanguage(hDlg);
    } else if (id == IDC_COMBO_CP && HIWORD(wParam) == CBN_SELCHANGE) {
      int idx = (int)SendDlgItemMessage(hDlg, IDC_COMBO_CP, CB_GETCURSEL, 0, 0);
      currentCodePage =
          (int)SendDlgItemMessage(hDlg, IDC_COMBO_CP, CB_GETITEMDATA, idx, 0);
    } else if (id == ID_POPUP_COPYPATH) {
    } else if (id == ID_HELP_ABOUT) {
      MessageBoxW(
          hDlg,
          L"Fast File Search v1.1\n\nHigh-performance File Search Tool.\n\n"
          L"Supports:\n"
          L"- NTFS (Direct MFT Access)\n"
          L"- FAT16/FAT32 (Direct Volume Access)\n"
          L"- exFAT (Direct Volume Access)\n\n"
          L"Scans raw disk structures for maximum speed.",
          L"About", MB_OK | MB_ICONINFORMATION);
    } else if (id == IDCANCEL) {
      SaveConfig(hDlg);
      EndDialog(hDlg, 0);
    }
    break;
  }
  case WM_ACTIVATE: {
    // Focus handled by Timer and Tab Order
    break;
  }
  case WM_NOTIFY: {
    LPNMHDR pnm = (LPNMHDR)lParam;
    if (pnm->idFrom == IDC_LIST_RESULTS) {
      if (pnm->code == NM_CUSTOMDRAW) {
        LPNMLVCUSTOMDRAW lplvcd = (LPNMLVCUSTOMDRAW)lParam;
        if (lplvcd->nmcd.dwDrawStage == CDDS_PREPAINT) {
          SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
          return TRUE;
        }
        if (lplvcd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
          SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_NOTIFYSUBITEMDRAW);
          return TRUE;
        }
        if (lplvcd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
          int col = lplvcd->iSubItem;
          if (col == 0 || col == 1) { // Name or Path
            LRESULT ret = DrawItemWithHighlight(lplvcd, hDlg);
            SetWindowLongPtr(hDlg, DWLP_MSGRESULT, ret);
            return TRUE;
          }
          SetWindowLongPtr(hDlg, DWLP_MSGRESULT, CDRF_DODEFAULT);
          return TRUE;
        }
      } else if (pnm->code == LVN_COLUMNCLICK) {
        LPNMLISTVIEW pnmv = (LPNMLISTVIEW)lParam;
        if (sortColumn == pnmv->iSubItem) {
          sortAscending = !sortAscending;
        } else {
          sortColumn = pnmv->iSubItem;
          sortAscending = true;
        }
        ListView_SortItems(hList, CompareFunc, 0);
      } else if (pnm->code == NM_RCLICK) {
        LPNMITEMACTIVATE pnmitem = (LPNMITEMACTIVATE)pnm;
        if (pnmitem->iItem != -1) {
          HMENU hMenu =
              LoadMenu(GetModuleHandle(NULL), MAKEINTRESOURCE(IDR_MENU1));
          HMENU hSubMenu = GetSubMenu(hMenu, 0);
          POINT pt;
          GetCursorPos(&pt);
          int cmd = TrackPopupMenu(hSubMenu, TPM_RETURNCMD, pt.x, pt.y, 0, hDlg,
                                   NULL);
          if (cmd == ID_POPUP_COPYPATH) {
            wchar_t buf[MAX_PATH];
            ListView_GetItemText(hList, pnmitem->iItem, 1, buf, MAX_PATH);

            if (OpenClipboard(hDlg)) {
              EmptyClipboard();
              size_t len = (wcslen(buf) + 1) * sizeof(wchar_t);
              HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len);
              memcpy(GlobalLock(hMem), buf, len);
              GlobalUnlock(hMem);
              SetClipboardData(CF_UNICODETEXT, hMem);
              CloseClipboard();
            }
          }
        }
      }
    }
    break;
  }
  case WM_USER + 1: // Search Done
    PopulateList();
    {
      wchar_t buf[100];
      wsprintfW(buf, STR_STATUS_DONE[currentLang], searchResults.size());
      SetDlgItemTextW(hDlg, IDC_STATUS, buf);
    }
    break;
  case WM_USER + 2: // Error
  {
    std::wstring err = mftReader.GetLastErrorMessage();
    if (err.empty())
      err = L"Unknown Error or Scan Failed.";

    // Detailed error dialog
    MessageBoxW(hDlg, err.c_str(), L"FastFileSearch Error",
                MB_ICONERROR | MB_OK);

    SetDlgItemTextW(hDlg, IDC_STATUS, L"Failed.");
  }
    isSearching = false;
    break;
  }
  return FALSE;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine,
                   int nShowCmd) {
  InitCommonControls();
  CoInitialize(NULL); // Init COM for SHBrowseForFolder
  int ret =
      DialogBox(hInstance, MAKEINTRESOURCE(IDD_MAINDIALOG), NULL, DialogProc);
  CoUninitialize();
  return ret;
}
