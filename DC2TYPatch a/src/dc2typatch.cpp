#include <windows.h>
#include <ddraw.h>
/*
 * DC2TY Patch DLL (Injection Version)
 *
 * This DLL is injected by the launcher - no proxy exports needed!
 * Much simpler and more reliable.
 */

#include <commdlg.h>
#include <cstdarg>
#include <cwctype>
#include <cstdio>
#include <detours.h>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <ctype.h>
#include "choice_layout.h"

static std::string DiagWideToUtf8(LPCWSTR text) {
  if (!text || IS_INTRESOURCE(text))
    return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
  if (len <= 1)
    return {};
  std::string out(len - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], len, NULL, NULL);
  return out;
}

static std::string DiagSjisToUtf8(LPCSTR text) {
  if (!text || IS_INTRESOURCE(text))
    return {};
  int wlen = MultiByteToWideChar(932, 0, text, -1, NULL, 0);
  if (wlen <= 1)
    return {};
  std::wstring wide(wlen - 1, L'\0');
  MultiByteToWideChar(932, 0, text, -1, &wide[0], wlen);
  return DiagWideToUtf8(wide.c_str());
}

static const char *DiagDriveTypeName(UINT type) {
  switch (type) {
  case DRIVE_UNKNOWN:
    return "UNKNOWN";
  case DRIVE_NO_ROOT_DIR:
    return "NO_ROOT_DIR";
  case DRIVE_REMOVABLE:
    return "REMOVABLE";
  case DRIVE_FIXED:
    return "FIXED";
  case DRIVE_REMOTE:
    return "REMOTE";
  case DRIVE_CDROM:
    return "CDROM";
  case DRIVE_RAMDISK:
    return "RAMDISK";
  default:
    return "OTHER";
  }
}

// AdvSys32 DVD check scans drives for DRIVE_CDROM. On devices without an
// optical drive (e.g. GameHub), spoof the install drive as CD-ROM.
static char g_dvdSpoofDriveLetter = 0;

static void InitDvdBypass() {
  WCHAR exePath[MAX_PATH];
  if (GetModuleFileNameW(NULL, exePath, MAX_PATH) >= 2 && exePath[1] == L':')
    g_dvdSpoofDriveLetter = (char)towupper(exePath[0]);
}

static bool IsGameInstallDriveA(LPCSTR path) {
  if (!path || !g_dvdSpoofDriveLetter || path[0] == '\0')
    return false;
  return (char)towupper((unsigned char)path[0]) == g_dvdSpoofDriveLetter &&
         path[1] == ':';
}

static bool IsGameInstallDriveW(LPCWSTR path) {
  if (!path || IS_INTRESOURCE(path) || !g_dvdSpoofDriveLetter || path[0] == L'\0')
    return false;
  return (wchar_t)towupper(path[0]) == (wchar_t)g_dvdSpoofDriveLetter &&
         path[1] == L':';
}

static bool IsDvdInsertMessageA(LPCSTR text) {
  if (!text)
    return false;
  if (strstr(text, "DVD") != nullptr)
    return true;
  // Shift-JIS fullwidth ＤＶＤ (82 60 82 86 82 60)
  if (strstr(text, "\x82\x60\x82\x86\x82\x60") != nullptr)
    return true;
  return false;
}

struct Dc2tySerialReg {
  std::string instKey = "EJ3JI8EZRRS";
  std::string actMail = "always@smi.le";
  std::string actKey = "0000001267532272";
  std::string saveDir;
  std::wstring saveDirW;
  DWORD useDirectX = 0;
  DWORD noMovie = 0;
  DWORD useWMV = 0;
  bool loaded = false;
  bool usePatchSaveDir = false;
};

static Dc2tySerialReg g_dc2tySerial;
static std::vector<HKEY> g_dc2tyRegKeyHandles;
static const char *g_dc2tySerialSource = "built-in defaults";

static void DiagLog(const char *fmt, ...);

static std::wstring GetExeDirForPatch() {
  WCHAR exePath[MAX_PATH];
  GetModuleFileNameW(NULL, exePath, MAX_PATH);
  std::wstring exeDir = exePath;
  size_t pos = exeDir.find_last_of(L"\\/");
  if (pos != std::wstring::npos)
    exeDir = exeDir.substr(0, pos);
  return exeDir;
}

static std::string WideToAcpString(const std::wstring &text) {
  if (text.empty())
    return {};
  int len = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, NULL, 0, NULL,
                                NULL);
  if (len <= 1)
    return {};
  std::string out(len - 1, '\0');
  WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, &out[0], len, NULL, NULL);
  return out;
}

static void EnsureDirectoryExistsW(const std::wstring &path) {
  if (path.empty())
    return;
  std::wstring normalized = path;
  for (wchar_t &ch : normalized) {
    if (ch == L'/')
      ch = L'\\';
  }
  size_t start = 0;
  if (normalized.size() >= 2 && normalized[1] == L':')
    start = 3;
  for (size_t pos = normalized.find(L'\\', start); pos != std::wstring::npos;
       pos = normalized.find(L'\\', pos + 1)) {
    if (pos > 0)
      CreateDirectoryW(normalized.substr(0, pos).c_str(), NULL);
  }
  CreateDirectoryW(normalized.c_str(), NULL);
}

static bool DirectoryHasEntriesW(const std::wstring &path) {
  DWORD attr = GetFileAttributesW(path.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
    return false;

  std::wstring search = path;
  if (!search.empty() && search.back() != L'\\' && search.back() != L'/')
    search += L'\\';
  search += L"*";

  WIN32_FIND_DATAW data;
  HANDLE hFind = FindFirstFileW(search.c_str(), &data);
  if (hFind == INVALID_HANDLE_VALUE)
    return false;
  bool hasEntries = false;
  do {
    if (wcscmp(data.cFileName, L".") != 0 && wcscmp(data.cFileName, L"..") != 0) {
      hasEntries = true;
      break;
    }
  } while (FindNextFileW(hFind, &data));
  FindClose(hFind);
  return hasEntries;
}

static bool ReadRegStringW(HKEY hKey, LPCWSTR valueName, std::wstring &out) {
  DWORD type = 0;
  DWORD bytes = 0;
  LSTATUS ret = RegQueryValueExW(hKey, valueName, NULL, &type, NULL, &bytes);
  if (ret != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
      bytes < sizeof(wchar_t))
    return false;

  std::vector<wchar_t> buf((bytes / sizeof(wchar_t)) + 1, L'\0');
  ret = RegQueryValueExW(hKey, valueName, NULL, &type, (LPBYTE)buf.data(),
                         &bytes);
  if (ret != ERROR_SUCCESS)
    return false;
  buf.back() = L'\0';
  out.assign(buf.data());
  return !out.empty();
}

static bool IsDc2tyRegistryPathA(LPCSTR subKey) {
  if (!subKey)
    return false;
  return _stricmp(subKey, "Software\\CIRCUS\\DC2TY") == 0 ||
         _stricmp(subKey, "DC2TY") == 0;
}

static bool IsDc2tyRegistryPathW(LPCWSTR subKey) {
  if (!subKey || IS_INTRESOURCE(subKey))
    return false;
  return _wcsicmp(subKey, L"Software\\CIRCUS\\DC2TY") == 0 ||
         _wcsicmp(subKey, L"DC2TY") == 0;
}

static void TrackDc2tyRegKey(HKEY hKey) {
  if (!hKey)
    return;
  for (HKEY tracked : g_dc2tyRegKeyHandles) {
    if (tracked == hKey)
      return;
  }
  g_dc2tyRegKeyHandles.push_back(hKey);
}

static bool IsTrackedDc2tyRegKey(HKEY hKey) {
  for (HKEY tracked : g_dc2tyRegKeyHandles) {
    if (tracked == hKey)
      return true;
  }
  return false;
}

static LSTATUS InjectRegStringA(LPBYTE lpData, LPDWORD lpcbData, LPDWORD lpType,
                                const std::string &value) {
  const DWORD needed = (DWORD)value.size() + 1;
  if (lpType)
    *lpType = REG_SZ;
  if (!lpcbData)
    return ERROR_INVALID_PARAMETER;
  if (!lpData) {
    *lpcbData = needed;
    return ERROR_SUCCESS;
  }
  if (*lpcbData < needed)
    return ERROR_MORE_DATA;
  memcpy(lpData, value.c_str(), needed);
  *lpcbData = needed;
  return ERROR_SUCCESS;
}

static LSTATUS InjectRegStringW(LPBYTE lpData, LPDWORD lpcbData, LPDWORD lpType,
                                const std::wstring &value) {
  const DWORD needed = (DWORD)((value.size() + 1) * sizeof(wchar_t));
  if (lpType)
    *lpType = REG_SZ;
  if (!lpcbData)
    return ERROR_INVALID_PARAMETER;
  if (!lpData) {
    *lpcbData = needed;
    return ERROR_SUCCESS;
  }
  if (*lpcbData < needed)
    return ERROR_MORE_DATA;
  memcpy(lpData, value.c_str(), needed);
  *lpcbData = needed;
  return ERROR_SUCCESS;
}

static LSTATUS InjectRegDword(LPBYTE lpData, LPDWORD lpcbData, LPDWORD lpType,
                              DWORD value) {
  if (lpType)
    *lpType = REG_DWORD;
  if (!lpcbData)
    return ERROR_INVALID_PARAMETER;
  if (!lpData) {
    *lpcbData = sizeof(DWORD);
    return ERROR_SUCCESS;
  }
  if (*lpcbData < sizeof(DWORD))
    return ERROR_MORE_DATA;
  *(DWORD *)lpData = value;
  *lpcbData = sizeof(DWORD);
  return ERROR_SUCCESS;
}

static LSTATUS InjectDc2tyRegValueA(LPCSTR lpValueName, LPBYTE lpData,
                                    LPDWORD lpcbData, LPDWORD lpType) {
  if (!lpValueName)
    return ERROR_INVALID_PARAMETER;
  if (_stricmp(lpValueName, "InstKey") == 0)
    return InjectRegStringA(lpData, lpcbData, lpType, g_dc2tySerial.instKey);
  if (_stricmp(lpValueName, "ActMail") == 0)
    return InjectRegStringA(lpData, lpcbData, lpType, g_dc2tySerial.actMail);
  if (_stricmp(lpValueName, "ActKey") == 0)
    return InjectRegStringA(lpData, lpcbData, lpType, g_dc2tySerial.actKey);
  if (_stricmp(lpValueName, "SaveDir") == 0 && g_dc2tySerial.usePatchSaveDir)
    return InjectRegStringA(lpData, lpcbData, lpType, g_dc2tySerial.saveDir);
  if (_stricmp(lpValueName, "UseDirectX") == 0)
    return InjectRegDword(lpData, lpcbData, lpType, g_dc2tySerial.useDirectX);
  if (_stricmp(lpValueName, "NoMovie") == 0)
    return InjectRegDword(lpData, lpcbData, lpType, g_dc2tySerial.noMovie);
  if (_stricmp(lpValueName, "UseWMV") == 0)
    return InjectRegDword(lpData, lpcbData, lpType, g_dc2tySerial.useWMV);
  return ERROR_FILE_NOT_FOUND;
}

static LSTATUS InjectDc2tyRegValueW(LPCWSTR lpValueName, LPBYTE lpData,
                                    LPDWORD lpcbData, LPDWORD lpType) {
  if (!lpValueName || IS_INTRESOURCE(lpValueName))
    return ERROR_INVALID_PARAMETER;
  if (_wcsicmp(lpValueName, L"SaveDir") == 0 && g_dc2tySerial.usePatchSaveDir)
    return InjectRegStringW(lpData, lpcbData, lpType, g_dc2tySerial.saveDirW);
  char nameA[128];
  if (WideCharToMultiByte(CP_ACP, 0, lpValueName, -1, nameA, sizeof(nameA),
                          NULL, NULL) <= 0)
    return ERROR_INVALID_PARAMETER;
  return InjectDc2tyRegValueA(nameA, lpData, lpcbData, lpType);
}

static DWORD ParseRegDwordHex(const char *text) {
  if (!text)
    return 0;
  if (_strnicmp(text, "dword:", 6) == 0)
    text += 6;
  return (DWORD)strtoul(text, NULL, 16);
}

static void ApplyDc2tySerialStringValue(const std::string &key,
                                        const std::string &value) {
  if (key == "InstKey")
    g_dc2tySerial.instKey = value;
  else if (key == "ActMail")
    g_dc2tySerial.actMail = value;
  else if (key == "ActKey")
    g_dc2tySerial.actKey = value;
}

static void ApplyDc2tySerialDwordValue(const std::string &key, DWORD value) {
  if (key == "UseDirectX")
    g_dc2tySerial.useDirectX = value;
  else if (key == "NoMovie")
    g_dc2tySerial.noMovie = value;
  else if (key == "UseWMV")
    g_dc2tySerial.useWMV = value;
}

static bool LoadDc2tySerialFromRegFile(const std::wstring &path) {
  FILE *f = _wfopen(path.c_str(), L"rb");
  if (!f)
    return false;

  bool sawValue = false;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    const char *keyStart = strchr(line, '"');
    if (!keyStart)
      continue;
    const char *keyEnd = strchr(keyStart + 1, '"');
    if (!keyEnd)
      continue;
    std::string key(keyStart + 1, keyEnd);

    const char *eq = strchr(keyEnd, '=');
    if (!eq)
      continue;

    if (key == "InstKey" || key == "ActMail" || key == "ActKey") {
      const char *valStart = strchr(eq, '"');
      if (!valStart)
        continue;
      const char *valEnd = strchr(valStart + 1, '"');
      if (!valEnd)
        continue;
      ApplyDc2tySerialStringValue(key, std::string(valStart + 1, valEnd));
      sawValue = true;
      continue;
    }

    if (key == "UseDirectX" || key == "NoMovie" || key == "UseWMV") {
      const char *valStart = eq + 1;
      while (*valStart == ' ' || *valStart == '\t')
        ++valStart;
      char valBuf[64];
      size_t n = 0;
      while (valStart[n] && valStart[n] != '\r' && valStart[n] != '\n' &&
             n + 1 < sizeof(valBuf)) {
        valBuf[n] = valStart[n];
        ++n;
      }
      valBuf[n] = '\0';
      ApplyDc2tySerialDwordValue(key, ParseRegDwordHex(valBuf));
      sawValue = true;
    }
  }

  fclose(f);
  return sawValue;
}

static void InitDc2tySaveDirRegistry(HKEY hKey) {
  std::wstring patchSaveDir = GetExeDirForPatch() + L"\\AdvData\\Savegame";
  EnsureDirectoryExistsW(patchSaveDir);

  std::wstring existingSaveDir;
  if (ReadRegStringW(hKey, L"SaveDir", existingSaveDir) &&
      DirectoryHasEntriesW(existingSaveDir)) {
    g_dc2tySerial.usePatchSaveDir = false;
    DiagLog("SaveDir preserved: existing save data found at %s",
            WideToAcpString(existingSaveDir).c_str());
    return;
  }

  g_dc2tySerial.saveDirW = patchSaveDir;
  g_dc2tySerial.saveDir = WideToAcpString(patchSaveDir);
  g_dc2tySerial.usePatchSaveDir = true;
  RegSetValueExW(hKey, L"SaveDir", 0, REG_SZ,
                 (const BYTE *)g_dc2tySerial.saveDirW.c_str(),
                 (DWORD)((g_dc2tySerial.saveDirW.size() + 1) *
                         sizeof(wchar_t)));
  DiagLog("SaveDir fallback enabled: %s", g_dc2tySerial.saveDir.c_str());
}

static void WriteDc2tySerialToRegistry() {
  HKEY hKey = NULL;
  if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\CIRCUS\\DC2TY", 0, NULL, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &hKey,
                      NULL) != ERROR_SUCCESS)
    return;

  RegSetValueExA(hKey, "InstKey", 0, REG_SZ,
                 (const BYTE *)g_dc2tySerial.instKey.c_str(),
                 (DWORD)g_dc2tySerial.instKey.size() + 1);
  RegSetValueExA(hKey, "ActMail", 0, REG_SZ,
                 (const BYTE *)g_dc2tySerial.actMail.c_str(),
                 (DWORD)g_dc2tySerial.actMail.size() + 1);
  RegSetValueExA(hKey, "ActKey", 0, REG_SZ,
                 (const BYTE *)g_dc2tySerial.actKey.c_str(),
                 (DWORD)g_dc2tySerial.actKey.size() + 1);
  RegSetValueExA(hKey, "UseDirectX", 0, REG_DWORD,
                 (const BYTE *)&g_dc2tySerial.useDirectX, sizeof(DWORD));
  RegSetValueExA(hKey, "NoMovie", 0, REG_DWORD,
                 (const BYTE *)&g_dc2tySerial.noMovie, sizeof(DWORD));
  RegSetValueExA(hKey, "UseWMV", 0, REG_DWORD,
                 (const BYTE *)&g_dc2tySerial.useWMV, sizeof(DWORD));
  InitDc2tySaveDirRegistry(hKey);
  RegCloseKey(hKey);
}

static void InitSerialKeyBypass() {
  const std::wstring regPath =
      GetExeDirForPatch() + L"\\registry\\dc2ty registry.reg";
  if (LoadDc2tySerialFromRegFile(regPath)) {
    g_dc2tySerial.loaded = true;
    g_dc2tySerialSource = "registry file";
  } else {
    g_dc2tySerial.loaded = true;
    g_dc2tySerialSource = "built-in defaults";
  }

  WriteDc2tySerialToRegistry();
  DiagLog("Serial key bypass: source=%s InstKey=%s",
          g_dc2tySerialSource, g_dc2tySerial.instKey.c_str());
}

static bool IsSerialKeyDialogTemplate(LPCVOID lpTemplateName) {
  if (!lpTemplateName || !IS_INTRESOURCE(lpTemplateName))
    return false;
  const UINT id = (UINT)(UINT_PTR)lpTemplateName;
  return id == 101 || id == 100;
}

static bool DiagContainsNoCaseA(LPCSTR haystack, LPCSTR needle) {
  if (!haystack || !needle || !*needle)
    return false;
  std::string h(haystack);
  std::string n(needle);
  for (char &ch : h)
    ch = (char)tolower((unsigned char)ch);
  for (char &ch : n)
    ch = (char)tolower((unsigned char)ch);
  return h.find(n) != std::string::npos;
}

static bool DiagContainsNoCaseW(LPCWSTR haystack, LPCWSTR needle) {
  if (!haystack || IS_INTRESOURCE(haystack) || !needle || !*needle)
    return false;
  std::wstring h(haystack);
  std::wstring n(needle);
  for (wchar_t &ch : h)
    ch = (wchar_t)towlower(ch);
  for (wchar_t &ch : n)
    ch = (wchar_t)towlower(ch);
  return h.find(n) != std::wstring::npos;
}

static void DiagLog(const char *fmt, ...) {
  static thread_local bool s_busy = false;
  if (s_busy)
    return;
  s_busy = true;

  WCHAR exePath[MAX_PATH];
  GetModuleFileNameW(NULL, exePath, MAX_PATH);
  std::wstring exeDir = exePath;
  size_t pos = exeDir.find_last_of(L"\\/");
  if (pos != std::wstring::npos)
    exeDir = exeDir.substr(0, pos);
  std::wstring logPath = exeDir + L"\\dc2ty_diag.log";

  FILE *f = _wfopen(logPath.c_str(), L"a");
  if (f) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", st.wYear,
            st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
  }

  s_busy = false;
}

static void DiagLogProcessState(const char *phase) {
  char exePath[MAX_PATH] = {};
  char dllPath[MAX_PATH] = {};
  char cwd[MAX_PATH] = {};
  GetModuleFileNameA(NULL, exePath, MAX_PATH);
  HMODULE self = NULL;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCSTR)&DiagLogProcessState, &self);
  if (self)
    GetModuleFileNameA(self, dllPath, MAX_PATH);
  GetCurrentDirectoryA(MAX_PATH, cwd);
  DWORD drives = GetLogicalDrives();
  DiagLog("STATE phase=%s exe='%s' dll='%s' cwd='%s' logicalDrives=0x%08lX",
          phase, exePath, dllPath, cwd, drives);
}

// ============================================================================
// Font Manager
// ============================================================================

static void LogTextTelemetry(const char *funcName, LPCSTR str, int c,
                             const INT *dx, SIZE *sz) {
  WCHAR exePath[MAX_PATH];
  GetModuleFileNameW(NULL, exePath, MAX_PATH);
  std::wstring exeDir = exePath;
  size_t pos = exeDir.find_last_of(L"\\/");
  if (pos != std::wstring::npos) {
    exeDir = exeDir.substr(0, pos);
  }
  std::wstring logPath = exeDir + L"\\dc2ty_text.log";

  FILE *f = _wfopen(logPath.c_str(), L"a");
  if (!f)
    return;

  // Convert string to hex safely
  char hexStr[256] = {0};
  int toPrint = (c < 50) ? c : 49;
  for (int i = 0; i < toPrint; i++) {
    sprintf(hexStr + strlen(hexStr), "%02X ", (unsigned char)str[i]);
  }

  fprintf(f, "[%s] c=%d, str='%s' (%s)\n", funcName, c, str, hexStr);

  if (dx) {
    fprintf(f, "    lpDx: ");
    for (int i = 0; i < c && i < 10; i++) {
      fprintf(f, "%d ", dx[i]);
    }
    fprintf(f, "\n");
  }

  if (sz) {
    fprintf(f, "    SIZE: cx=%d cy=%d\n", sz->cx, sz->cy);
  }

  fclose(f);
}

static int CALLBACK EnumFontFaceProbeProc(const LOGFONTW *lf,
                                          const TEXTMETRICW *tm, DWORD type,
                                          LPARAM lParam) {
  (void)tm;
  (void)type;
  bool *found = reinterpret_cast<bool *>(lParam);
  if (found)
    *found = true;
  return 0;
}

static bool IsInstalledFontFace(const std::wstring &faceName) {
  if (faceName.empty())
    return false;
  HDC hdc = GetDC(NULL);
  if (!hdc)
    return false;

  LOGFONTW lf = {};
  lf.lfCharSet = DEFAULT_CHARSET;
  wcsncpy_s(lf.lfFaceName, faceName.c_str(), _TRUNCATE);

  bool found = false;
  EnumFontFamiliesExW(hdc, &lf, EnumFontFaceProbeProc, (LPARAM)&found, 0);
  ReleaseDC(NULL, hdc);
  return found;
}

static std::wstring GetDefaultGuiFaceName() {
  HFONT hGui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  if (hGui) {
    LOGFONTW lf = {};
    if (GetObjectW(hGui, sizeof(lf), &lf) == sizeof(lf) && lf.lfFaceName[0]) {
      return lf.lfFaceName;
    }
  }
  return L"Segoe UI";
}

static std::wstring ResolveSafeFontFace(std::wstring faceName) {
  while (!faceName.empty() &&
         (faceName.front() == L' ' || faceName.front() == L'\t')) {
    faceName.erase(faceName.begin());
  }
  while (!faceName.empty() &&
         (faceName.back() == L' ' || faceName.back() == L'\t')) {
    faceName.pop_back();
  }
  if (!faceName.empty() && faceName[0] == L'@') {
    faceName.erase(faceName.begin());
  }
  if (faceName.find(L'\uFFFD') != std::wstring::npos) {
    faceName.clear();
  }

  if (!faceName.empty() && IsInstalledFontFace(faceName)) {
    return faceName;
  }

  const wchar_t *fallbacks[] = {L"MS PGothic", L"MS Gothic", L"Yu Gothic UI",
                                L"Meiryo UI", L"Tahoma", L"Segoe UI"};
  for (const auto *fallback : fallbacks) {
    if (IsInstalledFontFace(fallback)) {
      return fallback;
    }
  }
  return GetDefaultGuiFaceName();
}

class FontManager {
private:
  std::map<int, HFONT> m_dialogueFonts;
  std::map<int, HFONT> m_dialogueLatinFonts;
  std::map<int, HFONT> m_backlogFonts;
  std::map<int, HFONT> m_backlogNameFonts;
  std::map<int, HFONT> m_engineFonts;
  std::wstring m_dialogueFontName = L"MS PGothic";
  std::wstring m_backlogFontName = L"MS PGothic";
  std::wstring m_backlogNameFontName = L"MS PGothic";
  int m_dialogueFontSizeOverride = -15;
  int m_backlogFontSizeOverride = -15;
  int m_backlogNameFontSizeOverride = -11;
  int m_backlogXOffset = 0;
  int m_backlogLineSpacing = 0;
  int m_backlogYOffset = 12;
  int m_backlogNameXOffset = 13;
  int m_backlogNameYOffset = 0;
  int m_backlogNameSpacing = 0;
  int m_backlogDialogSpacing = 0;
  int m_dialogueRenderScale = 74;
  int m_dialogueAdvanceScale = 62;
  int m_dialogueSpaceScale = 24;
  bool m_advancedSettings = true;
  bool m_enableBacklogAllIcon = true;
  bool m_enableFileRedirection = true;
  bool m_disableBacklogFont = false;
  bool m_disableBacklogSpacing = false;
  bool m_disableBacklogTranslation = false;
  std::wstring m_iniPath;
  int m_language =
      0; // 0 = Japanese, 1 = id_Data (Indonesian), 2 = eng_data (English)

public:
  void Init() {
    // Determine INI path - stored in main game folder (same dir as DC2TY.EXE)
    WCHAR modulePath[MAX_PATH];
    GetModuleFileNameW(NULL, modulePath, MAX_PATH);
    std::wstring exeDir = modulePath;
    size_t pos = exeDir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
      exeDir = exeDir.substr(0, pos);
    }
    m_iniPath = exeDir + L"\\DC2TYPatch.ini";

    // Create subfolder structure for file redirection
    // id_Data subfolders
    CreateDirectoryW((exeDir + L"\\id_Data").c_str(), NULL);
    CreateDirectoryW((exeDir + L"\\id_Data\\MES").c_str(), NULL);
    CreateDirectoryW((exeDir + L"\\id_Data\\GRP").c_str(), NULL);
    CreateDirectoryW((exeDir + L"\\id_Data\\MOVIE").c_str(), NULL);
    // eng_data subfolders
    CreateDirectoryW((exeDir + L"\\eng_data").c_str(), NULL);
    CreateDirectoryW((exeDir + L"\\eng_data\\MES").c_str(), NULL);
    CreateDirectoryW((exeDir + L"\\eng_data\\GRP").c_str(), NULL);
    CreateDirectoryW((exeDir + L"\\eng_data\\MOVIE").c_str(), NULL);

    // Check if INI exists, otherwise create it with defaults
    if (GetFileAttributesW(m_iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
      // Write the exact requested default settings in pure Shift-JIS (ANSI)
      // This prevents WritePrivateProfileStringW from destroying Japanese
      // characters on systems with English/non-JP locales.
      HANDLE hFile = CreateFileW(m_iniPath.c_str(), GENERIC_WRITE, 0, NULL,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
      if (hFile != INVALID_HANDLE_VALUE) {
        const char *defaultIni = "[Fonts]\r\n"
                                 "BacklogFont=MS PGothic\r\n"
                                 "BacklogSize=-15\r\n"
                                 "DialogueFont=MS PGothic\r\n"
                                 "DialogueSize=-15\r\n"
                                 "BacklogNameFont=MS PGothic\r\n"
                                 "BacklogNameSize=-11\r\n"
                                 "BacklogXOffset=0\r\n"
                                 "BacklogLineSpacing=0\r\n"
                                 "BacklogYOffset=12\r\n"
                                 "BacklogNameXOffset=13\r\n"
                                 "BacklogNameYOffset=0\r\n"
                                 "BacklogNameSpacing=0\r\n"
                                 "BacklogDialogSpacing=0\r\n"
                                 "DialogueRenderScale=74\r\n"
                                 "DialogueAdvanceScale=62\r\n"
                                 "DialogueSpaceScale=24\r\n"
                                 "AdvancedSettings=1\r\n"
                                 "\r\n"
                                 "[Settings]\r\n"
                                 "ShowBacklogIcon=1\r\n"
                                 "EnableFileRedirection=1\r\n"
                                 "Language=1\r\n"
                                 "DisableBacklogFont=0\r\n"
                                 "DisableBacklogSpacing=0\r\n"
                                 "DisableBacklogTranslation=0\r\n";

        DWORD bytesWritten;
        WriteFile(hFile, defaultIni, (DWORD)strlen(defaultIni), &bytesWritten,
                  NULL);
        CloseHandle(hFile);
      }
    }

    // Load custom settings if any
    WCHAR buf[128];
    if (GetPrivateProfileStringW(L"Fonts", L"DialogueFont", L"", buf, 128,
                                 m_iniPath.c_str()) > 0) {
      m_dialogueFontName = buf;
    }
    if (GetPrivateProfileStringW(L"Fonts", L"BacklogFont", L"", buf, 128,
                                 m_iniPath.c_str()) > 0) {
      m_backlogFontName = buf;
    }
    if (GetPrivateProfileStringW(L"Fonts", L"BacklogNameFont", L"", buf, 128,
                                 m_iniPath.c_str()) > 0) {
      m_backlogNameFontName = buf;
    }
    m_dialogueFontName = ResolveSafeFontFace(m_dialogueFontName);
    m_backlogFontName = ResolveSafeFontFace(m_backlogFontName);
    m_backlogNameFontName = ResolveSafeFontFace(m_backlogNameFontName);

    // GetPrivateProfileIntW returns 0 for negative numbers. We need a helper.
    auto ReadInt = [&](LPCWSTR key, int defVal) {
      WCHAR valBuf[64];
      if (GetPrivateProfileStringW(L"Fonts", key, L"", valBuf, 64,
                                   m_iniPath.c_str()) > 0) {
        return _wtoi(valBuf);
      }
      return defVal;
    };

    m_dialogueFontSizeOverride = ReadInt(L"DialogueSize", -19);
    m_backlogFontSizeOverride = ReadInt(L"BacklogSize", -19);
    m_backlogNameFontSizeOverride = ReadInt(L"BacklogNameSize", -11);
    m_backlogXOffset = ReadInt(L"BacklogXOffset", 0);
    m_backlogLineSpacing = ReadInt(L"BacklogLineSpacing", 0);
    m_backlogYOffset = ReadInt(L"BacklogYOffset", 12);
    m_backlogNameXOffset = ReadInt(L"BacklogNameXOffset", 13);
    m_backlogNameYOffset = ReadInt(L"BacklogNameYOffset", 0);
    m_backlogNameSpacing = ReadInt(L"BacklogNameSpacing", 0);
    m_backlogDialogSpacing = ReadInt(L"BacklogDialogSpacing", 0);
    m_dialogueRenderScale = ReadInt(L"DialogueRenderScale", 74);
    m_dialogueAdvanceScale = ReadInt(L"DialogueAdvanceScale", 62);
    m_dialogueSpaceScale = ReadInt(L"DialogueSpaceScale", 24);
    m_advancedSettings = ReadInt(L"AdvancedSettings", 1) != 0;

    // Clamp dialogue tuning to safe ranges but keep enough headroom so
    // DialogueSize in INI has a visible in-game effect.
    if (m_dialogueFontSizeOverride > 0)
      m_dialogueFontSizeOverride = -m_dialogueFontSizeOverride;
    if (m_dialogueFontSizeOverride == 0)
      m_dialogueFontSizeOverride = -19;
    if (m_dialogueFontSizeOverride > -8)
      m_dialogueFontSizeOverride = -8;
    if (m_dialogueFontSizeOverride < -48)
      m_dialogueFontSizeOverride = -48;

    if (m_dialogueRenderScale < 40)
      m_dialogueRenderScale = 40;
    if (m_dialogueRenderScale > 140)
      m_dialogueRenderScale = 140;
    if (m_dialogueAdvanceScale < 35)
      m_dialogueAdvanceScale = 35;
    if (m_dialogueAdvanceScale > 90)
      m_dialogueAdvanceScale = 90;
    if (m_dialogueSpaceScale < 15)
      m_dialogueSpaceScale = 15;
    if (m_dialogueSpaceScale > 70)
      m_dialogueSpaceScale = 70;

    auto ReadIntOther = [&](LPCWSTR section, LPCWSTR key, int defVal) {
      WCHAR valBuf[64];
      if (GetPrivateProfileStringW(section, key, L"", valBuf, 64,
                                   m_iniPath.c_str()) > 0) {
        return _wtoi(valBuf);
      }
      return defVal;
    };

    m_enableBacklogAllIcon =
        ReadIntOther(L"Settings", L"ShowBacklogIcon", 1) != 0;

    // Read legacy settings for backwards compatibility
    bool legacyIdEnabled =
        ReadIntOther(L"Settings", L"EnableFileRedirection", 1) != 0;
    bool legacyEngEnabled = ReadIntOther(L"Settings", L"EnableEngData", 0) != 0;

    // Default to Indonesian if legacy EnableFileRedirection was true, but
    // English overrides it.
    int defaultLang = 0;
    if (legacyIdEnabled)
      defaultLang = 1;
    if (legacyEngEnabled)
      defaultLang = 2;

    m_language = ReadIntOther(L"Settings", L"Language", defaultLang);
    m_disableBacklogFont =
        ReadIntOther(L"Settings", L"DisableBacklogFont", 0) != 0;
    m_disableBacklogSpacing =
        ReadIntOther(L"Settings", L"DisableBacklogSpacing", 0) != 0;
    m_disableBacklogTranslation =
        ReadIntOther(L"Settings", L"DisableBacklogTranslation", 0) != 0;

    // Ensure all font names stay usable across locale/encoding differences.
    m_dialogueFontName = ResolveSafeFontFace(m_dialogueFontName);
    m_backlogFontName = ResolveSafeFontFace(m_backlogFontName);
    m_backlogNameFontName = ResolveSafeFontFace(m_backlogNameFontName);
    WritePrivateProfileStringW(L"Fonts", L"DialogueFont",
                               m_dialogueFontName.c_str(), m_iniPath.c_str());
    WritePrivateProfileStringW(L"Fonts", L"BacklogFont",
                               m_backlogFontName.c_str(), m_iniPath.c_str());
    WritePrivateProfileStringW(L"Fonts", L"BacklogNameFont",
                               m_backlogNameFontName.c_str(),
                               m_iniPath.c_str());
  }

  HFONT GetDialogueFont(int baseSize) {
    int requestedSize =
        m_dialogueFontSizeOverride != 0 ? m_dialogueFontSizeOverride : baseSize;
    if (m_dialogueFonts.count(requestedSize))
      return m_dialogueFonts[requestedSize];

    HFONT f = CreateFontW(requestedSize, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                          SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH, m_dialogueFontName.c_str());
    if (f)
      m_dialogueFonts[requestedSize] = f;
    return f;
  }

  HFONT GetDialogueFontExact(int requestedSize) {
    if (requestedSize == 0)
      requestedSize = -GetDialogueGlyphHeightHint();
    if (requestedSize > 0)
      requestedSize = -requestedSize;
    if (m_dialogueFonts.count(requestedSize))
      return m_dialogueFonts[requestedSize];

    std::wstring fontFace = m_dialogueFontName;
    if (fontFace == L"MS Gothic" || fontFace == L"ＭＳ ゴシック")
      fontFace = L"MS PGothic";

    HFONT f = CreateFontW(requestedSize, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                          SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH, fontFace.c_str());
    if (f)
      m_dialogueFonts[requestedSize] = f;
    return f;
  }

  HFONT GetDialogueOutlineFontExact(int requestedSize) {
    if (requestedSize == 0)
      requestedSize = -GetDialogueGlyphHeightHint();
    if (requestedSize > 0)
      requestedSize = -requestedSize;
    if (m_dialogueLatinFonts.count(requestedSize))
      return m_dialogueLatinFonts[requestedSize];

    std::wstring fontFace = m_dialogueFontName;
    bool japaneseLatinFace =
        (fontFace == L"MS Gothic" || fontFace == L"\uFF2D\uFF33 \u30B4\u30B7\u30C3\u30AF" ||
         fontFace == L"MS PGothic" || fontFace == L"\uFF2D\uFF33 \uFF30\u30B4\u30B7\u30C3\u30AF" ||
         fontFace == L"Yu Gothic UI" || fontFace == L"Meiryo UI");
    if (japaneseLatinFace) {
      // These faces render Japanese well, but their Latin bitmap glyphs look
      // uneven in ADV32's per-glyph cache. Use a Latin UI face only for ASCII
      // textbox outlines; Japanese/SJIS rendering keeps the configured face.
      if (IsInstalledFontFace(L"Verdana"))
        fontFace = L"Verdana";
      else if (IsInstalledFontFace(L"Tahoma"))
        fontFace = L"Tahoma";
      else if (IsInstalledFontFace(L"Segoe UI"))
        fontFace = L"Segoe UI";
      else if (IsInstalledFontFace(L"Consolas"))
        fontFace = L"Consolas";
      else if (IsInstalledFontFace(L"Lucida Console"))
        fontFace = L"Lucida Console";
    }

    HFONT f = CreateFontW(requestedSize, 0, 0, 0, FW_EXTRABOLD, 0, 0, 0,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH, fontFace.c_str());
    if (f)
      m_dialogueLatinFonts[requestedSize] = f;
    return f;
  }

  // Returns a font guaranteed to have Unicode musical symbols (U+266A etc.)
  // rendered at a size proportional to the dialogue Latin text.
  // Uses Segoe UI Symbol / Segoe UI Emoji which natively contain these glyphs.
  HFONT GetMusicNoteFont(int requestedSize) {
    if (requestedSize > 0)
      requestedSize = -requestedSize;
    // Use a separate cache key offset so we don't collide with regular Latin.
    int cacheKey = requestedSize - 10000;
    if (m_dialogueLatinFonts.count(cacheKey))
      return m_dialogueLatinFonts[cacheKey];

    // Prefer fonts that natively contain musical symbol glyphs (Miscellaneous
    // Symbols block U+2600-U+26FF). Segoe UI Symbol is always available on
    // Windows 7+ and renders them at a compact Latin-compatible size.
    std::wstring fontFace = L"Segoe UI Symbol";
    if (!IsInstalledFontFace(L"Segoe UI Symbol")) {
      if (IsInstalledFontFace(L"Segoe UI Emoji"))
        fontFace = L"Segoe UI Emoji";
      else if (IsInstalledFontFace(L"Arial Unicode MS"))
        fontFace = L"Arial Unicode MS";
      else if (IsInstalledFontFace(L"Lucida Sans Unicode"))
        fontFace = L"Lucida Sans Unicode";
      else
        fontFace = L"Verdana"; // fallback – may do GDI substitution
    }

    HFONT f = CreateFontW(requestedSize, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH, fontFace.c_str());
    if (f)
      m_dialogueLatinFonts[cacheKey] = f;
    return f;
  }



  HFONT GetEngineScaledFont(int height) {
    int absHeight = 0;
    if (height > 0) {
      // For ADV32 textbox path, source tmHeight is often very large (e.g. 56).
      // Scale from runtime height to get stable Latin glyph size in textbox.
      absHeight = (height * 64 + 50) / 100;
      // Apply user dialogue size override proportionally so DialogueSize in INI
      // has a visible effect in-game even on runtime-sized text paths.
      int userAbs = m_dialogueFontSizeOverride;
      if (userAbs == 0)
        userAbs = -19;
      if (userAbs < 0)
        userAbs = -userAbs;
      if (userAbs < 8)
        userAbs = 8;
      if (userAbs > 48)
        userAbs = 48;
      absHeight = (absHeight * userAbs + 9) / 19;
      int renderScale = m_dialogueRenderScale;
      if (renderScale <= 0)
        renderScale = 100;
      absHeight = (absHeight * renderScale + 50) / 100;
      if (absHeight < 10)
        absHeight = 10;
      if (absHeight > 38)
        absHeight = 38;
    } else {
      int requestedHeight = m_dialogueFontSizeOverride;
      if (requestedHeight == 0)
        requestedHeight = -19;
      if (requestedHeight > 0)
        requestedHeight = -requestedHeight;
      absHeight = -requestedHeight;
      if (absHeight < 12)
        absHeight = 12;
      if (absHeight > 26)
        absHeight = 26;
    }
    int requestedHeight = -absHeight;
    if (m_engineFonts.count(requestedHeight))
      return m_engineFonts[requestedHeight];
    // Let GDI pick proportional glyph width from selected face.
    int requestedWidth = 0;

    // Try PGothic first for proportional widths if available, fallback to
    // Gothic
    std::wstring fontFace = m_dialogueFontName;
    if (fontFace == L"MS Gothic" || fontFace == L"ＭＳ ゴシック") {
      fontFace = L"MS PGothic";
    }

    HFONT f = CreateFontW(requestedHeight, requestedWidth, 0, 0, FW_NORMAL, 0,
                          0, 0,
                          SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH, fontFace.c_str());
    if (!f && fontFace != L"MS PGothic") {
      f = CreateFontW(requestedHeight, requestedWidth, 0, 0, FW_NORMAL, 0, 0,
                      0,
                      SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                      L"MS PGothic");
    }
    if (!f) {
      f = CreateFontW(requestedHeight, requestedWidth, 0, 0, FW_NORMAL, 0, 0,
                      0,
                      SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                      L"Yu Gothic UI");
    }
    if (!f) {
      f = CreateFontW(requestedHeight, requestedWidth, 0, 0, FW_NORMAL, 0, 0,
                      0,
                      SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH,
                      L"Tahoma");
    }
    if (f)
      m_engineFonts[requestedHeight] = f;
    return f;
  }

  HFONT GetBacklogFont(int baseSize) {
    int requestedSize =
        m_backlogFontSizeOverride != 0 ? m_backlogFontSizeOverride : baseSize;
    if (m_backlogFonts.count(requestedSize))
      return m_backlogFonts[requestedSize];

    HFONT f = CreateFontW(requestedSize, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                          SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH, m_backlogFontName.c_str());
    if (f)
      m_backlogFonts[requestedSize] = f;
    return f;
  }

  HFONT GetBacklogNameFont(int baseSize) {
    int requestedSize = m_backlogNameFontSizeOverride != 0
                            ? m_backlogNameFontSizeOverride
                            : baseSize;
    if (requestedSize == 0)
      requestedSize = baseSize; // Fallback
    if (m_backlogNameFonts.count(requestedSize))
      return m_backlogNameFonts[requestedSize];

    HFONT f = CreateFontW(requestedSize, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                          SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                          DEFAULT_PITCH, m_backlogNameFontName.c_str());
    if (f)
      m_backlogNameFonts[requestedSize] = f;
    return f;
  }

  void SetDialogueFont(const std::wstring &name, int size) {
    m_dialogueFontName = ResolveSafeFontFace(name);
    m_dialogueFontSizeOverride = size;
    for (auto &p : m_dialogueFonts)
      DeleteObject(p.second);
    m_dialogueFonts.clear();
    for (auto &p : m_dialogueLatinFonts)
      DeleteObject(p.second);
    m_dialogueLatinFonts.clear();
    for (auto &p : m_engineFonts)
      DeleteObject(p.second);
    m_engineFonts.clear();

    // Save
    WritePrivateProfileStringW(L"Fonts", L"DialogueFont",
                               m_dialogueFontName.c_str(),
                               m_iniPath.c_str());
    WritePrivateProfileStringW(L"Fonts", L"DialogueSize",
                               std::to_wstring(size).c_str(),
                               m_iniPath.c_str());
  }

  void SetBacklogFont(const std::wstring &name, int size) {
    m_backlogFontName = ResolveSafeFontFace(name);
    m_backlogFontSizeOverride = size;
    for (auto &p : m_backlogFonts)
      DeleteObject(p.second);
    m_backlogFonts.clear();

    // Save
    WritePrivateProfileStringW(L"Fonts", L"BacklogFont",
                               m_backlogFontName.c_str(),
                               m_iniPath.c_str());
    WritePrivateProfileStringW(L"Fonts", L"BacklogSize",
                               std::to_wstring(size).c_str(),
                               m_iniPath.c_str());
  }

  void SetBacklogNameFont(const std::wstring &name, int size) {
    m_backlogNameFontName = ResolveSafeFontFace(name);
    m_backlogNameFontSizeOverride = size;
    for (auto &p : m_backlogNameFonts)
      DeleteObject(p.second);
    m_backlogNameFonts.clear();

    // Save
    WritePrivateProfileStringW(L"Fonts", L"BacklogNameFont",
                               m_backlogNameFontName.c_str(),
                               m_iniPath.c_str());
    WritePrivateProfileStringW(L"Fonts", L"BacklogNameSize",
                               std::to_wstring(size).c_str(),
                               m_iniPath.c_str());
  }

  void SetBacklogOffsets(int xOffset, int yOffset, int spacing, int nameXOffset,
                         int nameYOffset, int nameSpacing, int dialogSpacing) {
    m_backlogXOffset = xOffset;
    m_backlogYOffset = yOffset;
    m_backlogLineSpacing = spacing;
    m_backlogNameXOffset = nameXOffset;
    m_backlogNameYOffset = nameYOffset;
    m_backlogNameSpacing = nameSpacing;
    m_backlogDialogSpacing = dialogSpacing;
    WritePrivateProfileStringW(L"Fonts", L"BacklogXOffset",
                               std::to_wstring(xOffset).c_str(),
                               m_iniPath.c_str());
    WritePrivateProfileStringW(L"Fonts", L"BacklogYOffset",
                               std::to_wstring(yOffset).c_str(),
                               m_iniPath.c_str());
    WritePrivateProfileStringW(L"Fonts", L"BacklogLineSpacing",
                               std::to_wstring(spacing).c_str(),
                               m_iniPath.c_str());
    WritePrivateProfileStringW(L"Fonts", L"BacklogNameXOffset",
                               std::to_wstring(nameXOffset).c_str(),
                               m_iniPath.c_str());
    WritePrivateProfileStringW(L"Fonts", L"BacklogNameYOffset",
                               std::to_wstring(nameYOffset).c_str(),
                               m_iniPath.c_str());
    WritePrivateProfileStringW(L"Fonts", L"BacklogNameSpacing",
                               std::to_wstring(nameSpacing).c_str(),
                               m_iniPath.c_str());
    WritePrivateProfileStringW(L"Fonts", L"BacklogDialogSpacing",
                               std::to_wstring(dialogSpacing).c_str(),
                               m_iniPath.c_str());
  }

  std::wstring GetDialogueFontName() const { return m_dialogueFontName; }
  std::wstring GetBacklogFontName() const { return m_backlogFontName; }
  std::wstring GetBacklogNameFontName() const { return m_backlogNameFontName; }
  int GetDialogueFontSize() const { return m_dialogueFontSizeOverride; }
  int GetBacklogFontSize() const { return m_backlogFontSizeOverride; }
  int GetBacklogNameFontSize() const { return m_backlogNameFontSizeOverride; }
  int GetBacklogXOffset() const {
    return m_advancedSettings ? m_backlogXOffset : 0;
  }
  int GetBacklogLineSpacing() const {
    return m_advancedSettings ? m_backlogLineSpacing : 0;
  }
  int GetBacklogYOffset() const {
    return m_advancedSettings ? m_backlogYOffset : 12;
  }
  int GetBacklogNameXOffset() const {
    return m_advancedSettings ? m_backlogNameXOffset : 13;
  }
  int GetBacklogNameYOffset() const {
    return m_advancedSettings ? m_backlogNameYOffset : 0;
  }
  int GetBacklogNameSpacing() const {
    return m_advancedSettings ? m_backlogNameSpacing : 0;
  }
  int GetBacklogDialogSpacing() const {
    return m_advancedSettings ? m_backlogDialogSpacing : 0;
  }
  int GetDialogueRenderScale() const { return m_dialogueRenderScale; }
  int GetDialogueAdvanceScale() const { return m_dialogueAdvanceScale; }
  int GetDialogueSpaceScale() const { return m_dialogueSpaceScale; }
  int GetDialogueGlyphHeightHint() const {
    int h = m_dialogueFontSizeOverride;
    if (h == 0)
      h = -19;
    if (h < 0)
      h = -h;
    int renderScale = m_dialogueRenderScale;
    if (renderScale <= 0)
      renderScale = 100;
    h = (h * renderScale + 50) / 100;
    // ADV32 consumes glyph outlines directly and then applies its own layout
    // scaling. Keep Latin dialogue at a readable floor even when the settings
    // dialog stores a small point size.
    if (h < 28)
      h = 28;
    if (h > 30)
      h = 30;
    return h;
  }

  void SetAdvancedSettings(bool advanced) {
    m_advancedSettings = advanced;
    WritePrivateProfileStringW(L"Fonts", L"AdvancedSettings",
                               std::to_wstring(advanced ? 1 : 0).c_str(),
                               m_iniPath.c_str());
  }

  bool GetAdvancedSettings() const { return m_advancedSettings; }

  void SetEnableBacklogAllIcon(bool enable) {
    m_enableBacklogAllIcon = enable;
    WritePrivateProfileStringW(L"Settings", L"ShowBacklogIcon",
                               std::to_wstring(enable ? 1 : 0).c_str(),
                               m_iniPath.c_str());
  }

  bool GetEnableBacklogAllIcon() const { return m_enableBacklogAllIcon; }

  void SetLanguage(int lang) {
    m_language = lang;
    WritePrivateProfileStringW(L"Settings", L"Language",
                               std::to_wstring(lang).c_str(),
                               m_iniPath.c_str());
  }

  int GetLanguage() const { return m_language; }

  bool GetDisableBacklogFont() const { return m_disableBacklogFont; }
  bool GetDisableBacklogSpacing() const { return m_disableBacklogSpacing; }
  bool GetDisableBacklogTranslation() const {
return m_disableBacklogTranslation;
  }
};

static FontManager g_fontManager;
static HWND g_mainWindow = nullptr;
static const wchar_t *kMainWindowTitle =
    L"D.C. II ~Da Capo II~ To You - Side Episodes";

// ============================================================================
// Backlog Font Handling
// ============================================================================
// The game's backlog font is large. This section handles font rendering in the
// backlog. Font scaling is currently disabled (100% original size).

static volatile bool g_inBacklogRender = false;
static thread_local int g_patchSettingsUiDepth = 0;
static thread_local int g_systemMenuLoopDepth = 0;
static thread_local int g_closeConfirmUiDepth = 0;
static thread_local int g_dialogueLatinRenderDepth = 0;
static thread_local int g_popupUiLatinRenderDepth = 0;
static thread_local int g_hsxf4LatinRenderDepth = 0;
static thread_local bool g_hsxf4ChoiceRender = false;
static thread_local bool g_hsxf4PopupRender = false;
static thread_local bool g_hsxf4ReplayRender = false;
static thread_local bool g_hsxf4NameplateRender = false;
static thread_local bool g_hsxf4NameplateLongRender = false;
static thread_local bool g_hsxf4ProblemNameplateRender = false;
static thread_local bool g_hsxf4EllipsisDotRender = false;
static thread_local UINT g_hsxf4GlyphOverride = 0;
static thread_local bool g_hsxf4DrawExtraPauseDot = false;
static thread_local int g_hsxf4ExtraPauseDotCount = 0;
static thread_local bool g_hsxf4TriplePauseGlyph = false;
static thread_local HDC g_choiceGlyphRunHdc = NULL;
static thread_local int g_choiceGlyphRunBudget = 0;
static thread_local DWORD g_choiceGlyphRunTick = 0;
static thread_local unsigned char g_choiceGlyphPendingLead = 0;
static thread_local HDC g_choiceGlyphPendingLeadHdc = NULL;
static thread_local bool g_choiceGlyphWordStart = true;
// Safety switch: bypass custom dialogue text shaping when output gets corrupted.
// Keep backlog/system-menu patches active.
static bool g_dialogueSafePassthrough = true;
static volatile UINT g_versionInfoMenuCommandId = 0;
static thread_local bool g_expectVersionInfoDialog = false;
static thread_local bool g_allowCloseWithoutPrompt = false;
static thread_local bool g_closeRequestedFromSysCommand = false;
static HFONT g_popupUiFont = NULL;
static HFONT g_choiceUiFont = NULL;
static HFONT g_versionInfoUiFont = NULL;
static const wchar_t *kVerInfoProp = L"DC2TY_VERINFO_DIALOG";

static inline bool InPatchSettingsUi() {
  return g_patchSettingsUiDepth > 0 || g_closeConfirmUiDepth > 0;
}
static inline bool InSystemMenuLoop() { return g_systemMenuLoopDepth > 0; }
static inline bool InDialogueLatinRender() {
  return g_dialogueLatinRenderDepth > 0;
}
static inline bool InPopupUiLatinRender() {
  return g_popupUiLatinRenderDepth > 0;
}
static inline bool InHsxf4LatinRender() { return g_hsxf4LatinRenderDepth > 0; }
static inline bool InHsxf4UiRender() {
  return g_hsxf4ChoiceRender || g_hsxf4PopupRender || g_hsxf4ReplayRender;
}
static inline int AdjustHsxf4DrawX(int x) {
  return x;
}

static inline bool IsMenuWindowDrawContext(HDC hdc) {
  if (!hdc)
    return false;
  HWND hwnd = WindowFromDC(hdc);
  if (!hwnd)
    return false;
  WCHAR cls[16] = {};
  if (GetClassNameW(hwnd, cls, 16) <= 0)
    return false;
  return wcscmp(cls, L"#32768") == 0;
}

static inline bool IsVersionInfoDialogDrawContext(HDC hdc) {
  if (!hdc)
    return false;
  HWND hwnd = WindowFromDC(hdc);
  if (!hwnd)
    return false;
  HWND root = GetAncestor(hwnd, GA_ROOT);
  if (!root)
    root = hwnd;
  return GetPropW(root, kVerInfoProp) != NULL;
}

struct ScopedPatchSettingsUi {
  ScopedPatchSettingsUi() { ++g_patchSettingsUiDepth; }
  ~ScopedPatchSettingsUi() { --g_patchSettingsUiDepth; }
};

struct ScopedCloseConfirmUi {
  ScopedCloseConfirmUi() { ++g_closeConfirmUiDepth; }
  ~ScopedCloseConfirmUi() { --g_closeConfirmUiDepth; }
};

struct ScopedDialogueLatinRender {
  explicit ScopedDialogueLatinRender(bool enable) : m_enable(enable) {
    if (m_enable)
      ++g_dialogueLatinRenderDepth;
  }
  ~ScopedDialogueLatinRender() {
    if (m_enable)
      --g_dialogueLatinRenderDepth;
  }
  bool m_enable;
};

struct ScopedPopupUiLatinRender {
  explicit ScopedPopupUiLatinRender(bool enable) : m_enable(enable) {
    if (m_enable)
      ++g_popupUiLatinRenderDepth;
  }
  ~ScopedPopupUiLatinRender() {
    if (m_enable)
      --g_popupUiLatinRenderDepth;
  }
  bool m_enable;
};

// Backlog font scaling function - returns original size (100%)
static int ScaleBacklogFontSize(int originalSize) {
  if (!g_inBacklogRender)
    return originalSize;
  return originalSize;
}

// ============================================================================
// File Redirection - Load from id_Data / eng_data subfolders by file type
//
// Directory layout:
//   id_Data\MES\     - translated .mes script files (Indonesian)
//   id_Data\GRP\     - translated .grp graphics files
//   id_Data\MOVIE\   - translated movie files (.mpg, .avi, .movie)
//   eng_data\MES\    - English .mes script files
//   eng_data\GRP\    - English .grp graphics files
//   eng_data\MOVIE\  - English movie files
//
// Priority: eng_data (if enabled) > id_Data > original
// ============================================================================

// Get the subfolder name for a given file extension
static const char *GetSubfolderForExt(const char *ext) {
  if (!ext)
    return nullptr;
  if (_stricmp(ext, ".mes") == 0)
    return "MES";
  if (_stricmp(ext, ".grp") == 0 || _stricmp(ext, ".crx") == 0 ||
      _stricmp(ext, ".crm") == 0)
    return "GRP";
  if (_stricmp(ext, ".mpg") == 0 || _stricmp(ext, ".avi") == 0 ||
      _stricmp(ext, ".movie") == 0)
    return "MOVIE";
  return nullptr;
}

static std::string ReplacePathA(const char *path) {
  if (!path)
    return {};

  std::string_view sv(path);

  // Don't redirect paths that are already inside our data folders
  std::string lowerPath(sv);
  for (auto &c : lowerPath)
    c = tolower(c);

  if (lowerPath.find("id_data") != std::string_view::npos ||
      lowerPath.find("eng_data") != std::string_view::npos ||
      lowerPath.find("sys_data") != std::string_view::npos) {
    return {};
  }

  // Extract filename (with extension) to determine the routing folder
  size_t sep = sv.find_last_of("\\/");
  std::string_view filename =
      (sep != std::string_view::npos) ? sv.substr(sep + 1) : sv;

  // Extract extension
  size_t dot = filename.find_last_of('.');
  const char *subfolder = nullptr;
  if (dot != std::string_view::npos) {
    std::string ext(filename.substr(dot));

    // Ignore graphic/animation files for dynamic redirection.
    // They are hardlink-synced to AdvData at startup, so redirecting them now
    // causes handle conflicts and engine access violations during animations.
    if (_stricmp(ext.c_str(), ".crx") == 0 ||
        _stricmp(ext.c_str(), ".grp") == 0 ||
        _stricmp(ext.c_str(), ".crm") == 0 ||
        _stricmp(ext.c_str(), ".pck") == 0) {
      return {};
    }

    subfolder = GetSubfolderForExt(ext.c_str());
  }

  // 1. Strip 'AdvData\' if it exists to get the clean relative path
  std::string relPath(sv);
  size_t advPos = lowerPath.find("advdata");
  if (advPos != std::string_view::npos) {
    size_t startPos = advPos + 7;
    while (startPos < sv.length() &&
           (sv[startPos] == '\\' || sv[startPos] == '/')) {
      startPos++;
    }
    relPath = std::string(sv.substr(startPos));
  } else if (sv.length() > 2 && sv[1] == ':') {
    // If it's an absolute path entirely outside AdvData, just use the filename
    // to be safe
    relPath = std::string(filename);
  } else {
    // Strip leading .\ or / from flat paths
    size_t startPos = 0;
    while (
        startPos < sv.length() &&
        (sv[startPos] == '.' || sv[startPos] == '\\' || sv[startPos] == '/')) {
      startPos++;
    }
    relPath = std::string(sv.substr(startPos));
  }

  // 2. Ensure relPath starts with the correct routing subfolder (e.g. "GRP")
  if (subfolder && !relPath.empty()) {
    std::string lowerRel(relPath);
    for (auto &c : lowerRel)
      c = tolower(c);

    std::string lowerSub(subfolder);
    for (auto &c : lowerSub)
      c = tolower(c);

    bool hasPrefix = false;
    if (lowerRel.length() >= lowerSub.length()) {
      if (lowerRel.substr(0, lowerSub.length()) == lowerSub) {
        if (lowerRel.length() == lowerSub.length() ||
            lowerRel[lowerSub.length()] == '\\' ||
            lowerRel[lowerSub.length()] == '/') {
          hasPrefix = true;
        }
      }
    }

    if (!hasPrefix) {
      relPath = std::string(subfolder) + "\\" + relPath;
    }
  }

  // Replace all forward slashes with backslashes
  for (auto &c : relPath) {
    if (c == '/')
      c = '\\';
  }

  // 3. Build candidate paths to try, in priority order:
  std::string candidates[6];
  int numCandidates = 0;

  int language = g_fontManager.GetLanguage();
  bool engEnabled = (language == 2);
  bool idEnabled = (language == 1);

  if (engEnabled) {
    if (!relPath.empty()) {
      candidates[numCandidates++] = std::string(".\\eng_data\\") + relPath;
    }
    if (subfolder && sep != std::string_view::npos) {
      candidates[numCandidates++] = std::string(".\\eng_data\\") +
                                    std::string(subfolder) + "\\" +
                                    std::string(filename);
    }
  }

  if (idEnabled) {
    if (!relPath.empty()) {
      candidates[numCandidates++] = std::string(".\\id_Data\\") + relPath;
    }
    if (subfolder && sep != std::string_view::npos) {
      candidates[numCandidates++] = std::string(".\\id_Data\\") +
                                    std::string(subfolder) + "\\" +
                                    std::string(filename);
    }
  }

  if (idEnabled) {
    if (sep != std::string_view::npos) {
      candidates[numCandidates++] =
          std::string(".\\id_Data\\") + std::string(filename);
    } else {
      candidates[numCandidates++] =
          std::string(".\\id_Data\\") + std::string(sv);
    }
  }

  for (int i = 0; i < numCandidates; i++) {
    char dbgCheck[512];
    DWORD attr = GetFileAttributesA(candidates[i].c_str());
    bool exists = (attr != INVALID_FILE_ATTRIBUTES);

    if (exists) {
      sprintf_s(dbgCheck,
                "DC2TYPatch Route Info: File=%s -> Candidate Resolved: %s\n",
                path, candidates[i].c_str());
      OutputDebugStringA(dbgCheck);
      return candidates[i];
    }
  }
  return {};
}

// JmpWrite - Write a JMP instruction at a target address
static bool JmpWrite(DWORD orgAddr, void *targetFunc) {
  BYTE jmp[5];
  jmp[0] = 0xE9; // JMP opcode
  *(DWORD *)(jmp + 1) = (DWORD)targetFunc - orgAddr - 5;

  DWORD oldProtect;
  if (!VirtualProtect((LPVOID)orgAddr, 5, PAGE_EXECUTE_READWRITE,
                      &oldProtect)) {
    return false;
  }

  SIZE_T written;
  WriteProcessMemory(INVALID_HANDLE_VALUE, (LPVOID)orgAddr, jmp, 5, &written);
  VirtualProtect((LPVOID)orgAddr, 5, oldProtect, &oldProtect);
  FlushInstructionCache(GetCurrentProcess(), (LPCVOID)orgAddr, 5);

  return (written == 5);
}

// MemWrite - Write arbitrary bytes at a target address
static bool MemWrite(DWORD addr, const void *data, size_t len) {
  DWORD oldProtect;
  if (!VirtualProtect((LPVOID)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect))
    return false;
  SIZE_T written;
  WriteProcessMemory(INVALID_HANDLE_VALUE, (LPVOID)addr, data, len, &written);
  VirtualProtect((LPVOID)addr, len, oldProtect, &oldProtect);
  FlushInstructionCache(GetCurrentProcess(), (LPCVOID)addr, len);
  return (written == len);
}

// Convert a UTF-8 string to Shift-JIS (codepage 932)
// Returns the number of bytes written (excluding null terminator), or 0 on
// failure
static int Utf8ToShiftJIS(const char *utf8, char *sjisOut, int sjisMaxLen) {
  // Step 1: UTF-8 → wide char (UTF-16)
  int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
  if (wideLen <= 0)
    return 0;

  wchar_t wideBuf[64];
  if (wideLen > 64)
    return 0;
  MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wideBuf, 64);

  // Step 2: wide char → Shift-JIS (codepage 932)
  int sjisLen = WideCharToMultiByte(932, 0, wideBuf, -1, sjisOut, sjisMaxLen,
                                    nullptr, nullptr);
  if (sjisLen <= 0)
    return 0;

  return sjisLen - 1; // exclude null terminator
}

// ============================================================================
// Hooks
// ============================================================================

static decltype(&GetGlyphOutlineA) Real_GetGlyphOutlineA = GetGlyphOutlineA;
static decltype(&GetGlyphOutlineW) Real_GetGlyphOutlineW = GetGlyphOutlineW;
static decltype(&CreateFileA) Real_CreateFileA = CreateFileA;
static decltype(&CreateFileW) Real_CreateFileW = CreateFileW;

static HANDLE WINAPI Hook_CreateFileW(
    LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {

  static thread_local bool s_inCreateFileW = false;
  if (s_inCreateFileW) {
    return Real_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
                            lpSecurityAttributes, dwCreationDisposition,
                            dwFlagsAndAttributes, hTemplateFile);
  }

  s_inCreateFileW = true;

  int lang = g_fontManager.GetLanguage();
  if (lang == 1 || lang == 2) {
    int len = WideCharToMultiByte(932, 0, lpFileName, -1, NULL, 0, NULL, NULL);
    if (len > 0) {
      std::string pathA(len, '\0');
      WideCharToMultiByte(932, 0, lpFileName, -1, &pathA[0], len, NULL, NULL);
      pathA.resize(len - 1);

      std::string newPathA = ReplacePathA(pathA.c_str());
      if (!newPathA.empty()) {
        // Resolve to absolute path
        char absPath[MAX_PATH];
        DWORD absLen =
            GetFullPathNameA(newPathA.c_str(), MAX_PATH, absPath, NULL);
        const char *finalPathA =
            (absLen > 0 && absLen < MAX_PATH) ? absPath : newPathA.c_str();

        int wlen = MultiByteToWideChar(932, 0, finalPathA, -1, NULL, 0);
        if (wlen > 0) {
          std::wstring newPathW(wlen, L'\0');
          MultiByteToWideChar(932, 0, finalPathA, -1, &newPathW[0], wlen);
          newPathW.resize(wlen - 1);

          char debugStr[512];
          sprintf_s(debugStr, "DC2TYPatch Redirecting W: %s -> %s\n",
                    pathA.c_str(), finalPathA);
          OutputDebugStringA(debugStr);

          HANDLE h =
              Real_CreateFileW(newPathW.c_str(), dwDesiredAccess, dwShareMode,
                               lpSecurityAttributes, dwCreationDisposition,
                               dwFlagsAndAttributes, hTemplateFile);
          s_inCreateFileW = false;
          return h;
        }
      }
    }
  }

  HANDLE h = Real_CreateFileW(lpFileName, dwDesiredAccess, dwShareMode,
                              lpSecurityAttributes, dwCreationDisposition,
                              dwFlagsAndAttributes, hTemplateFile);
  s_inCreateFileW = false;
  return h;
}
static decltype(&CreateWindowExA) Real_CreateWindowExA = CreateWindowExA;
static decltype(&CreateWindowExW) Real_CreateWindowExW = CreateWindowExW;
static decltype(&LoadLibraryA) Real_LoadLibraryA = LoadLibraryA;
static decltype(&LoadLibraryW) Real_LoadLibraryW = LoadLibraryW;
static decltype(&GetProcAddress) Real_GetProcAddress = GetProcAddress;
static decltype(&GetTextExtentPoint32A) Real_GetTextExtentPoint32A =
    GetTextExtentPoint32A;
static decltype(&GetTextExtentPoint32W) Real_GetTextExtentPoint32W =
    GetTextExtentPoint32W;
static decltype(&MessageBoxA) Real_MessageBoxA = MessageBoxA;
static decltype(&MessageBoxW) Real_MessageBoxW = MessageBoxW;
static decltype(&DialogBoxParamA) Real_DialogBoxParamA = DialogBoxParamA;
static decltype(&DialogBoxParamW) Real_DialogBoxParamW = DialogBoxParamW;
static decltype(&RegOpenKeyExA) Real_RegOpenKeyExA = RegOpenKeyExA;
static decltype(&RegOpenKeyExW) Real_RegOpenKeyExW = RegOpenKeyExW;
static decltype(&RegQueryValueExA) Real_RegQueryValueExA = RegQueryValueExA;
static decltype(&RegQueryValueExW) Real_RegQueryValueExW = RegQueryValueExW;
static decltype(&GetDriveTypeA) Real_GetDriveTypeA = GetDriveTypeA;
static decltype(&GetDriveTypeW) Real_GetDriveTypeW = GetDriveTypeW;
static decltype(&GetLogicalDrives) Real_GetLogicalDrives = GetLogicalDrives;
static decltype(&AppendMenuA) Real_AppendMenuA = AppendMenuA;
static decltype(&InsertMenuA) Real_InsertMenuA = InsertMenuA;
static decltype(&ModifyMenuA) Real_ModifyMenuA = ModifyMenuA;
static decltype(&GetTextMetricsA) Real_GetTextMetricsA = GetTextMetricsA;
static decltype(&GetTextMetricsW) Real_GetTextMetricsW = GetTextMetricsW;
static decltype(&ExtTextOutA) Real_ExtTextOutA = ExtTextOutA;
static decltype(&ExtTextOutW) Real_ExtTextOutW = ExtTextOutW;
static decltype(&TextOutA) Real_TextOutA = TextOutA;
static decltype(&TextOutW) Real_TextOutW = TextOutW;
static decltype(&DrawTextW) Real_DrawTextW = DrawTextW;
static decltype(&DrawTextExW) Real_DrawTextExW = DrawTextExW;
static decltype(&BitBlt) Real_BitBlt = BitBlt;
static decltype(&StretchBlt) Real_StretchBlt = StretchBlt;
typedef HRESULT(WINAPI *DirectDrawCreateProc)(GUID FAR *,
                                              LPDIRECTDRAW FAR *,
                                              IUnknown FAR *);
typedef HRESULT(WINAPI *DDCreateSurfaceProc)(LPDIRECTDRAW, LPDDSURFACEDESC,
                                             LPDIRECTDRAWSURFACE FAR *,
                                             IUnknown FAR *);
typedef HRESULT(WINAPI *DDSBltProc)(LPDIRECTDRAWSURFACE, LPRECT,
                                    LPDIRECTDRAWSURFACE, LPRECT, DWORD,
                                    LPDDBLTFX);
typedef HRESULT(WINAPI *DDSFlipProc)(LPDIRECTDRAWSURFACE,
                                     LPDIRECTDRAWSURFACE, DWORD);
static DirectDrawCreateProc Real_DirectDrawCreate = NULL;
static DDCreateSurfaceProc Real_DD_CreateSurface = NULL;
static DDSBltProc Real_DDS_Blt = NULL;
static DDSFlipProc Real_DDS_Flip = NULL;
static volatile LONG g_directDrawHooked = 0;
static volatile LONG g_directDrawSurfaceHooked = 0;
static decltype(&SetTextCharacterExtra) Real_SetTextCharacterExtra =
    SetTextCharacterExtra;
static decltype(&SetTextJustification) Real_SetTextJustification =
    SetTextJustification;
static int GetDialogueAdvanceStep();
static int GetDialogueCharExtraTight();

// ============================================================================
// Japanese Codepage Hooks (Wine/Proton fix)
// ============================================================================
// On Wine/Proton (Android), GetACP() may return the host Linux codepage
// instead of 932 (Shift-JIS), breaking Japanese string handling in the game.
// These hooks are harmless on Windows (LE already set the codepage).
// NOTE: LCID/LangID hooks are intentionally omitted - they break backlog.

typedef UINT(WINAPI *pfnGetACP)(void);
typedef UINT(WINAPI *pfnGetOEMCP)(void);

static pfnGetACP Real_GetACP = GetACP;
static pfnGetOEMCP Real_GetOEMCP = GetOEMCP;

static UINT WINAPI Hook_GetACP() { return 932; }
static UINT WINAPI Hook_GetOEMCP() { return 932; }

static const char *DiagRootKeyName(HKEY hKey) {
  if (hKey == HKEY_CURRENT_USER)
    return "HKCU";
  if (hKey == HKEY_LOCAL_MACHINE)
    return "HKLM";
  if (hKey == HKEY_CLASSES_ROOT)
    return "HKCR";
  if (hKey == HKEY_USERS)
    return "HKU";
  if (hKey == HKEY_CURRENT_CONFIG)
    return "HKCC";
  return "HKEY";
}

static bool DiagShouldLogRegSubKeyA(LPCSTR subKey) {
  if (!subKey)
    return false;
  return DiagContainsNoCaseA(subKey, "CIRCUS") ||
         DiagContainsNoCaseA(subKey, "DC2TY") ||
         DiagContainsNoCaseA(subKey, "D.C") ||
         DiagContainsNoCaseA(subKey, "Da Capo");
}

static bool DiagShouldLogRegSubKeyW(LPCWSTR subKey) {
  if (!subKey || IS_INTRESOURCE(subKey))
    return false;
  return DiagContainsNoCaseW(subKey, L"CIRCUS") ||
         DiagContainsNoCaseW(subKey, L"DC2TY") ||
         DiagContainsNoCaseW(subKey, L"D.C") ||
         DiagContainsNoCaseW(subKey, L"Da Capo");
}

static bool DiagShouldLogRegValueA(LPCSTR valueName) {
  if (!valueName)
    return true;
  return _stricmp(valueName, "InstKey") == 0 ||
         _stricmp(valueName, "ActMail") == 0 ||
         _stricmp(valueName, "ActKey") == 0 ||
         _stricmp(valueName, "SaveDir") == 0 ||
         _stricmp(valueName, "UseDirectX") == 0 ||
         _stricmp(valueName, "UseWMV") == 0 ||
         _stricmp(valueName, "NoMovie") == 0;
}

static bool DiagShouldLogRegValueW(LPCWSTR valueName) {
  if (!valueName || IS_INTRESOURCE(valueName))
    return true;
  return _wcsicmp(valueName, L"InstKey") == 0 ||
         _wcsicmp(valueName, L"ActMail") == 0 ||
         _wcsicmp(valueName, L"ActKey") == 0 ||
         _wcsicmp(valueName, L"SaveDir") == 0 ||
         _wcsicmp(valueName, L"UseDirectX") == 0 ||
         _wcsicmp(valueName, L"UseWMV") == 0 ||
         _wcsicmp(valueName, L"NoMovie") == 0;
}

static LSTATUS WINAPI Hook_RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey,
                                         DWORD ulOptions, REGSAM samDesired,
                                         PHKEY phkResult) {
  LSTATUS ret =
      Real_RegOpenKeyExA(hKey, lpSubKey, ulOptions, samDesired, phkResult);
  if (ret == ERROR_SUCCESS && phkResult && IsDc2tyRegistryPathA(lpSubKey))
    TrackDc2tyRegKey(*phkResult);
  if (DiagShouldLogRegSubKeyA(lpSubKey)) {
    DiagLog("RegOpenKeyExA root=%s sub='%s' sam=0x%08lX -> %ld",
            DiagRootKeyName(hKey), lpSubKey ? lpSubKey : "(null)",
            (DWORD)samDesired, (long)ret);
  }
  return ret;
}

static LSTATUS WINAPI Hook_RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey,
                                         DWORD ulOptions, REGSAM samDesired,
                                         PHKEY phkResult) {
  LSTATUS ret =
      Real_RegOpenKeyExW(hKey, lpSubKey, ulOptions, samDesired, phkResult);
  if (ret == ERROR_SUCCESS && phkResult && IsDc2tyRegistryPathW(lpSubKey))
    TrackDc2tyRegKey(*phkResult);
  if (DiagShouldLogRegSubKeyW(lpSubKey)) {
    std::string sub = DiagWideToUtf8(lpSubKey);
    DiagLog("RegOpenKeyExW root=%s sub='%s' sam=0x%08lX -> %ld",
            DiagRootKeyName(hKey), sub.c_str(), (DWORD)samDesired, (long)ret);
  }
  return ret;
}

static LSTATUS WINAPI Hook_RegQueryValueExA(HKEY hKey, LPCSTR lpValueName,
                                            LPDWORD lpReserved, LPDWORD lpType,
                                            LPBYTE lpData, LPDWORD lpcbData) {
  if (g_dc2tySerial.loaded && IsTrackedDc2tyRegKey(hKey) && lpValueName) {
    LSTATUS injected =
        InjectDc2tyRegValueA(lpValueName, lpData, lpcbData, lpType);
    if (injected == ERROR_SUCCESS || injected == ERROR_MORE_DATA) {
      if (DiagShouldLogRegValueA(lpValueName)) {
        DiagLog("RegQueryValueExA value='%s' -> injected(%s)", lpValueName,
                g_dc2tySerialSource);
      }
      return injected;
    }
  }

  LSTATUS ret = Real_RegQueryValueExA(hKey, lpValueName, lpReserved, lpType,
                                      lpData, lpcbData);
  if (ret != ERROR_SUCCESS && g_dc2tySerial.loaded && lpValueName) {
    LSTATUS injected =
        InjectDc2tyRegValueA(lpValueName, lpData, lpcbData, lpType);
    if (injected == ERROR_SUCCESS || injected == ERROR_MORE_DATA) {
      if (DiagShouldLogRegValueA(lpValueName)) {
        DiagLog("RegQueryValueExA value='%s' -> injected(fallback)", lpValueName);
      }
      return injected;
    }
  }
  if (DiagShouldLogRegValueA(lpValueName)) {
    DWORD type = lpType ? *lpType : 0xFFFFFFFF;
    DWORD size = lpcbData ? *lpcbData : 0;
    DiagLog("RegQueryValueExA value='%s' -> %ld type=0x%08lX size=%lu data=%s",
            lpValueName ? lpValueName : "(default)", (long)ret, type, size,
            ret == ERROR_SUCCESS ? "present(masked)" : "missing");
  }
  return ret;
}

static LSTATUS WINAPI Hook_RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName,
                                            LPDWORD lpReserved, LPDWORD lpType,
                                            LPBYTE lpData, LPDWORD lpcbData) {
  if (g_dc2tySerial.loaded && IsTrackedDc2tyRegKey(hKey) && lpValueName &&
      !IS_INTRESOURCE(lpValueName)) {
    LSTATUS injected =
        InjectDc2tyRegValueW(lpValueName, lpData, lpcbData, lpType);
    if (injected == ERROR_SUCCESS || injected == ERROR_MORE_DATA) {
      std::string name = DiagWideToUtf8(lpValueName);
      if (DiagShouldLogRegValueW(lpValueName)) {
        DiagLog("RegQueryValueExW value='%s' -> injected(%s)", name.c_str(),
                g_dc2tySerialSource);
      }
      return injected;
    }
  }

  LSTATUS ret = Real_RegQueryValueExW(hKey, lpValueName, lpReserved, lpType,
                                      lpData, lpcbData);
  if (ret != ERROR_SUCCESS && g_dc2tySerial.loaded && lpValueName &&
      !IS_INTRESOURCE(lpValueName)) {
    LSTATUS injected =
        InjectDc2tyRegValueW(lpValueName, lpData, lpcbData, lpType);
    if (injected == ERROR_SUCCESS || injected == ERROR_MORE_DATA) {
      std::string name = DiagWideToUtf8(lpValueName);
      if (DiagShouldLogRegValueW(lpValueName)) {
        DiagLog("RegQueryValueExW value='%s' -> injected(fallback)",
                name.c_str());
      }
      return injected;
    }
  }
  if (DiagShouldLogRegValueW(lpValueName)) {
    std::string name = DiagWideToUtf8(lpValueName);
    DWORD type = lpType ? *lpType : 0xFFFFFFFF;
    DWORD size = lpcbData ? *lpcbData : 0;
    DiagLog("RegQueryValueExW value='%s' -> %ld type=0x%08lX size=%lu data=%s",
            name.empty() ? "(default)" : name.c_str(), (long)ret, type, size,
            ret == ERROR_SUCCESS ? "present(masked)" : "missing");
  }
  return ret;
}

static UINT WINAPI Hook_GetDriveTypeA(LPCSTR lpRootPathName) {
  if (IsGameInstallDriveA(lpRootPathName)) {
    DiagLog("GetDriveTypeA path='%s' -> 5(CDROM) [dvd bypass]",
            lpRootPathName ? lpRootPathName : "(null)");
    return DRIVE_CDROM;
  }
  UINT ret = Real_GetDriveTypeA(lpRootPathName);
  DiagLog("GetDriveTypeA path='%s' -> %u(%s)",
          lpRootPathName ? lpRootPathName : "(null)", ret,
          DiagDriveTypeName(ret));
  return ret;
}

static UINT WINAPI Hook_GetDriveTypeW(LPCWSTR lpRootPathName) {
  if (IsGameInstallDriveW(lpRootPathName)) {
    std::string path = DiagWideToUtf8(lpRootPathName);
    DiagLog("GetDriveTypeW path='%s' -> 5(CDROM) [dvd bypass]",
            path.empty() ? "(null)" : path.c_str());
    return DRIVE_CDROM;
  }
  UINT ret = Real_GetDriveTypeW(lpRootPathName);
  std::string path = DiagWideToUtf8(lpRootPathName);
  DiagLog("GetDriveTypeW path='%s' -> %u(%s)",
          path.empty() ? "(null)" : path.c_str(), ret,
          DiagDriveTypeName(ret));
  return ret;
}

static DWORD WINAPI Hook_GetLogicalDrives() {
  DWORD ret = Real_GetLogicalDrives();
  DiagLog("GetLogicalDrives -> 0x%08lX", ret);
  return ret;
}

// ============================================================================
// Indonesian UI Translation System
// ============================================================================
// Two-pronged approach:
// 1. Hook-based: intercepts MessageBoxA, AppendMenuA etc. for runtime
// translation
// 2. Memory-patch: scans loaded EXE's .rdata/.data and replaces strings
// in-place
//    at startup, catching chapter titles and game-engine-rendered text.
// Indonesian text is pure ASCII which is valid in Shift-JIS codepage.

struct UITranslation {
  const char *japanese;   // Original SJIS bytes
  const char *indonesian; // Indonesian replacement (ASCII)
};

// Master translation table - used by both hooks and memory patcher
static const UITranslation g_uiTranslations[] = {
    {"\x83\x6F\x81\x5B\x83\x57\x83\x87\x83\x93\x8F\xEE\x95\xF1",
     "Informasi Versi"},
    {"\x96\xDF\x82\xC1\x82\xC4\x82\xE0\x82\xE6\x82\xEB\x82\xB5\x82\xA2\x82\xC5\x82\xB7\x82\xA9\x81\x48",
     "Apakah Anda yakin ingin kembali?"},
    {"\x8F\x49\x97\xB9\x82\xB5\x82\xC4\x82\xE0\x82\xE6\x82\xEB\x82\xB5\x82\xA2\x82\xC5\x82\xB7\x82\xA9\x81\x48",
     "Apakah Anda ingin keluar dari permainan?"},
    {"\x83\x51\x81\x5B\x83\x80\x8F\x49\x97\xB9", "Akhiri Permainan"},
    {nullptr, nullptr} // End marker
};
// Translate a SJIS string if it matches a known entry (exact match)
static const char *TranslateUI(const char *sjisText) {
  if (!sjisText)
    return nullptr;
  if (g_fontManager.GetLanguage() != 1)
    return nullptr;
  for (int i = 0; g_uiTranslations[i].japanese != nullptr; i++) {
    if (strcmp(sjisText, g_uiTranslations[i].japanese) == 0) {
      return g_uiTranslations[i].indonesian;
    }
  }
  return nullptr;
}

// Check if a string contains a SJIS substring and return the translation
static const char *TranslateUIPartial(const char *sjisText) {
  if (!sjisText)
    return nullptr;
  if (g_fontManager.GetLanguage() != 1)
    return nullptr;
  for (int i = 0; g_uiTranslations[i].japanese != nullptr; i++) {
    if (strstr(sjisText, g_uiTranslations[i].japanese) != nullptr) {
      return g_uiTranslations[i].indonesian;
    }
  }
  return nullptr;
}

// ============================================================================
// Scene Replay Runtime Translation
// ============================================================================
// sce_*.mes contains replay-list data with internal offsets. Rebuilding it like
// normal dialogue can corrupt those offsets and crash the replay menu. Keep the
// MES raw JP and replace the visible replay strings at GDI render time instead.

static bool g_replayMapLoaded = false;
static std::map<std::string, std::string> g_replayMapA;
static std::map<std::wstring, std::wstring> g_replayMapW;
static std::map<std::wstring, std::wstring> g_replayDisplayMapW;
static void NormalizeReplayTextW(std::wstring &text);

static std::wstring GetExeDirW() {
  WCHAR exePath[MAX_PATH];
  GetModuleFileNameW(NULL, exePath, MAX_PATH);
  std::wstring exeDir = exePath;
  size_t pos = exeDir.find_last_of(L"\\/");
  if (pos != std::wstring::npos)
    exeDir = exeDir.substr(0, pos);
  return exeDir;
}

static void LogReplayMarqueeDebug(const char *tag, int a = 0, int b = 0,
                                  int c = 0, int d = 0,
                                  const wchar_t *text = NULL) {
  static volatile LONG s_logCount = 0;
  LONG idx = InterlockedIncrement(&s_logCount);
  if (idx > 800)
    return;

  std::wstring path = GetExeDirW() + L"\\replay_marquee_debug.log";
  FILE *f = _wfopen(path.c_str(), L"a, ccs=UTF-8");
  if (!f)
    return;
  SYSTEMTIME st = {};
  GetLocalTime(&st);
  fwprintf(f, L"[%02d:%02d:%02d.%03d] %S a=%d b=%d c=%d d=%d",
           st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
           tag ? tag : "(null)", a, b, c, d);
  if (text)
    fwprintf(f, L" text=%ls", text);
  fwprintf(f, L"\n");
  fclose(f);
}

static bool Utf8ToWideString(const std::string &src, std::wstring &out) {
  out.clear();
  if (src.empty())
    return true;
  int len = MultiByteToWideChar(CP_UTF8, 0, src.c_str(), (int)src.size(),
                                NULL, 0);
  if (len <= 0)
    return false;
  out.resize(len);
  return MultiByteToWideChar(CP_UTF8, 0, src.c_str(), (int)src.size(),
                             &out[0], len) == len;
}

static bool WideToSjisString(const std::wstring &src, std::string &out) {
  out.clear();
  if (src.empty())
    return true;
  int len = WideCharToMultiByte(932, 0, src.c_str(), (int)src.size(), NULL, 0,
                                NULL, NULL);
  if (len <= 0)
    return false;
  out.resize(len);
  return WideCharToMultiByte(932, 0, src.c_str(), (int)src.size(), &out[0],
                             len, NULL, NULL) == len;
}

static bool SjisToWideString(const std::string &src, std::wstring &out) {
  out.clear();
  if (src.empty())
    return true;
  int len = MultiByteToWideChar(932, 0, src.c_str(), (int)src.size(), NULL, 0);
  if (len <= 0)
    return false;
  out.resize(len);
  return MultiByteToWideChar(932, 0, src.c_str(), (int)src.size(), &out[0],
                             len) == len;
}

static std::string TrimUtf8Field(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                       s.back() == ' ' || s.back() == '\t'))
    s.pop_back();
  size_t start = 0;
  while (start < s.size() &&
         (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' ||
          s[start] == '\n'))
    ++start;
  return start ? s.substr(start) : s;
}

static void LoadReplayTranslationMap() {
  if (g_replayMapLoaded)
    return;
  g_replayMapLoaded = true;

  std::wstring path = GetExeDirW() + L"\\id_Data\\replay_titles.tsv";
  FILE *f = _wfopen(path.c_str(), L"rb");
  if (!f)
    return;

  char line[8192];
  while (fgets(line, sizeof(line), f)) {
    std::string row(line);
    if (row.empty() || row[0] == '#')
      continue;
    size_t t1 = row.find('\t');
    if (t1 == std::string::npos)
      continue;
    size_t t2 = row.find('\t', t1 + 1);
    std::string jpUtf8 = TrimUtf8Field(row.substr(0, t1));
    std::string idUtf8 =
        TrimUtf8Field(row.substr(t1 + 1, t2 == std::string::npos
                                             ? std::string::npos
                                             : t2 - t1 - 1));
    if (jpUtf8.empty() || idUtf8.empty() || jpUtf8 == idUtf8)
      continue;

    std::wstring jpW, idW;
    std::string jpA, idA;
    if (!Utf8ToWideString(jpUtf8, jpW) || !Utf8ToWideString(idUtf8, idW))
      continue;
    if (!WideToSjisString(jpW, jpA) || !WideToSjisString(idW, idA))
      continue;
    g_replayMapA[jpA] = idA;
    g_replayMapW[jpW] = idW;
  }
  fclose(f);

  std::wstring displayPath =
      GetExeDirW() + L"\\id_Data\\replay_display_titles.tsv";
  FILE *df = _wfopen(displayPath.c_str(), L"rb");
  if (!df)
    return;

  while (fgets(line, sizeof(line), df)) {
    std::string row(line);
    if (row.empty() || row[0] == '#')
      continue;
    size_t t1 = row.find('\t');
    if (t1 == std::string::npos)
      continue;
    size_t t2 = row.find('\t', t1 + 1);
    std::string displayUtf8 = TrimUtf8Field(row.substr(0, t1));
    std::string fullUtf8 =
        TrimUtf8Field(row.substr(t1 + 1, t2 == std::string::npos
                                             ? std::string::npos
                                             : t2 - t1 - 1));
    if (displayUtf8.empty() || fullUtf8.empty())
      continue;

    std::wstring displayW, fullW;
    if (!Utf8ToWideString(displayUtf8, displayW) ||
        !Utf8ToWideString(fullUtf8, fullW))
      continue;
    NormalizeReplayTextW(displayW);
    NormalizeReplayTextW(fullW);
    if (displayW.empty() || fullW.empty())
      continue;
    g_replayDisplayMapW[displayW] = fullW;
  }
  fclose(df);
}

static bool IsLikelyReplayTextArea(int x, int y) {
  // Scene replay list lives outside the ADV textbox band. Keep this broad
  // enough for both left category labels and right title rows.
  return x >= 0 && x <= 800 && y >= 20 && y <= 590;
}

static const char *TranslateReplayTextA(LPCSTR text, UINT len, UINT *outLen) {
  if (!text || len == 0)
    return nullptr;
  LoadReplayTranslationMap();
  if (g_replayMapA.empty())
    return nullptr;
  std::string key(text, text + len);
  while (!key.empty() && key.back() == '\0')
    key.pop_back();
  auto it = g_replayMapA.find(key);
  if (it == g_replayMapA.end())
    return nullptr;
  if (outLen)
    *outLen = (UINT)it->second.size();
  return it->second.c_str();
}

static std::string NormalizeReplayLatinTextA(const BYTE *text, UINT len) {
  std::string out;
  if (!text || len == 0)
    return out;

  for (UINT i = 0; i < len && i < 256;) {
    BYTE b1 = text[i];
    if (b1 == 0)
      break;
    if (b1 == '\t') {
      out.push_back(' ');
      ++i;
      continue;
    }
    if (b1 == 0x81 && i + 1 < len && text[i + 1] == 0x40) {
      out.push_back(' ');
      i += 2;
      continue;
    }
    if (b1 == 0x82 && i + 1 < len) {
      BYTE b2 = text[i + 1];
      if (b2 >= 0x4F && b2 <= 0x58) {
        out.push_back((char)('0' + (b2 - 0x4F)));
        i += 2;
        continue;
      }
      if (b2 >= 0x60 && b2 <= 0x79) {
        out.push_back((char)('A' + (b2 - 0x60)));
        i += 2;
        continue;
      }
      if (b2 >= 0x81 && b2 <= 0x9A) {
        out.push_back((char)('a' + (b2 - 0x81)));
        i += 2;
        continue;
      }
    }
    if (b1 == 0x81 && i + 1 < len) {
      char ch = 0;
      switch (text[i + 1]) {
      case 0x43:
        ch = ',';
        break;
      case 0x44:
        ch = '.';
        break;
      case 0x46:
        ch = ':';
        break;
      case 0x47:
        ch = ';';
        break;
      case 0x48:
        ch = '?';
        break;
      case 0x49:
        ch = '!';
        break;
      case 0x60:
        ch = '~';
        break;
      case 0x65:
      case 0x66:
        ch = '\'';
        break;
      case 0x69:
        ch = '(';
        break;
      case 0x6A:
        ch = ')';
        break;
      case 0x75:
      case 0x76:
      case 0x77:
      case 0x78:
        ch = '"';
        break;
      case 0x5B:
      case 0x5D:
      case 0x7C:
        ch = '-';
        break;
      default:
        break;
      }
      if (ch != 0) {
        out.push_back(ch);
        i += 2;
        continue;
      }
    }
    out.push_back((char)b1);
    ++i;
  }

  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

static bool IsReplayTranslatedValueA(const BYTE *text, UINT len) {
  LoadReplayTranslationMap();
  if (g_replayMapA.empty())
    return false;

  std::string key = NormalizeReplayLatinTextA(text, len);
  if (key.empty())
    return false;

  for (const auto &entry : g_replayMapA) {
    if (entry.second == key)
      return true;
  }
  return false;
}

static const wchar_t *TranslateReplayTextW(LPCWSTR text, UINT len,
                                           UINT *outLen) {
  if (!text || len == 0)
    return nullptr;
  LoadReplayTranslationMap();
  if (g_replayMapW.empty())
    return nullptr;
  std::wstring key(text, text + len);
  while (!key.empty() && key.back() == L'\0')
    key.pop_back();
  auto it = g_replayMapW.find(key);
  if (it == g_replayMapW.end())
    return nullptr;
  if (outLen)
    *outLen = (UINT)it->second.size();
  return it->second.c_str();
}

static bool StartsWithReplayKeyW(const std::wstring &value,
                                 const std::wstring &prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

static const std::wstring *
FindReplayDisplayTitleW(const std::wstring &rawKey) {
  LoadReplayTranslationMap();
  if (g_replayDisplayMapW.empty() || rawKey.empty())
    return nullptr;

  std::wstring key = rawKey;
  while (!key.empty() && key.back() == L'\0')
    key.pop_back();
  NormalizeReplayTextW(key);
  if (key.empty())
    return nullptr;

  auto exact = g_replayDisplayMapW.find(key);
  if (exact != g_replayDisplayMapW.end())
    return &exact->second;

  // Some GDI calls provide the cell-clipped prefix (for example "Pergi Memba")
  // while the fixed-offset MES stores one more character ("Pergi Memban").
  // Use a conservative prefix match only for title-length strings.
  if (key.size() < 5)
    return nullptr;

  const std::wstring *best = nullptr;
  size_t bestDisplayLen = 0;
  for (const auto &entry : g_replayDisplayMapW) {
    const std::wstring &display = entry.first;
    if (display.size() < 5)
      continue;
    bool matched = StartsWithReplayKeyW(display, key) ||
                   StartsWithReplayKeyW(key, display);
    if (!matched)
      continue;
    if (!best || display.size() > bestDisplayLen) {
      best = &entry.second;
      bestDisplayLen = display.size();
    }
  }
  return best;
}

static const wchar_t *TranslateReplayDisplayTextW(LPCWSTR text, UINT len,
                                                  UINT *outLen) {
  if (!text || len == 0)
    return nullptr;

  static thread_local std::wstring s_render;
  std::wstring key(text, text + len);
  const std::wstring *full = FindReplayDisplayTitleW(key);
  if (!full)
    return nullptr;

  s_render = *full;
  NormalizeReplayTextW(s_render);
  if (s_render.empty())
    return nullptr;
  if (outLen)
    *outLen = (UINT)s_render.size();
  LogReplayMarqueeDebug("display.mapW", 0, 0, (int)key.size(),
                        (int)s_render.size(), s_render.c_str());
  return s_render.c_str();
}

static const char *TranslateReplayDisplayTextA(LPCSTR text, UINT len,
                                               UINT *outLen) {
  if (!text || len == 0)
    return nullptr;

  std::string raw(text, text + len);
  while (!raw.empty() && raw.back() == '\0')
    raw.pop_back();

  std::wstring wide;
  if (!SjisToWideString(raw, wide))
    return nullptr;

  UINT wideLen = (UINT)wide.size();
  const wchar_t *wideFull =
      TranslateReplayDisplayTextW(wide.c_str(), wideLen, &wideLen);
  if (!wideFull)
    return nullptr;

  static thread_local std::string s_render;
  std::wstring fullW(wideFull, wideFull + wideLen);
  if (!WideToSjisString(fullW, s_render) || s_render.empty())
    return nullptr;
  if (outLen)
    *outLen = (UINT)s_render.size();
  LogReplayMarqueeDebug("display.mapA", 0, 0, (int)raw.size(),
                        (int)s_render.size(), fullW.c_str());
  return s_render.c_str();
}

static HFONT SelectReplayTextFont(HDC hdc, int height = -18) {
  if (!hdc)
    return NULL;
  HFONT font = g_fontManager.GetDialogueOutlineFontExact(height);
  if (!font)
    font = g_fontManager.GetDialogueFontExact(height);
  return font ? (HFONT)SelectObject(hdc, font) : NULL;
}

static int GetReplayTextWidthHint(int x) {
  if (x >= 500)
    return 165;
  if (x >= 300)
    return 160;
  if (x >= 220)
    return 175;
  return 180;
}

static bool IsReplayTitleTextPosition(int x, int y) {
  return x >= 220 && x <= 760 && y >= 115 && y <= 530;
}

static bool GetReplayHoverRect(HDC hdc, int x, int y, int widthHint,
                               int lineHeight, const RECT *drawRect,
                               RECT *hoverRect) {
  if (!hdc || !hoverRect)
    return false;

  // Use text origin instead of the incoming GDI rect. In scene replay the rect
  // is often local/clipping data from an offscreen buffer, not the actual row.
  hoverRect->left = x - 70;
  hoverRect->right = x + widthHint + 12;
  hoverRect->top = y - 18;
  hoverRect->bottom = y + lineHeight + 18;
  return true;
}

static HWND GetReplayDrawWindow(HDC hdc) {
  HWND hwnd = WindowFromDC(hdc);
  if (!hwnd)
    hwnd = GetForegroundWindow();
  return hwnd;
}

static bool IsMouseOverReplayTitle(HDC hdc, const RECT &hoverRect) {
  HWND hwnd = GetReplayDrawWindow(hdc);
  if (!hwnd)
    return false;

  POINT pt = {};
  if (!GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt))
    return false;

  return PtInRect(&hoverRect, pt) != FALSE;
}

static volatile LONG g_replayMarqueeActive = 0;
static volatile LONG g_replayMarqueeLastTick = 0;
static volatile LONG g_replayMarqueePumpRunning = 0;
static HANDLE g_replayMarqueePumpThread = NULL;

struct ReplayMarqueeItem {
  RECT hoverRect;
  RECT textRect;
  int x;
  int y;
  int widthHint;
  int textWidth;
  DWORD lastSeen;
  COLORREF textColor;
  COLORREF bgColor;
  std::wstring text;
};

static CRITICAL_SECTION g_replayMarqueeLock;
static volatile LONG g_replayMarqueeLockState = 0;
static std::vector<ReplayMarqueeItem> g_replayMarqueeItems;

struct ReplayRuntimeOverlayItem {
  int x;
  int y;
  int widthHint;
  DWORD lastSeen;
  std::wstring text;
};

static std::vector<ReplayRuntimeOverlayItem> g_replayRuntimeOverlayItems;

static void EnsureReplayMarqueeLock() {
  for (;;) {
    LONG state = InterlockedCompareExchange(&g_replayMarqueeLockState, 2, 2);
    if (state == 2)
      return;
    if (state == 0 &&
        InterlockedCompareExchange(&g_replayMarqueeLockState, 1, 0) == 0) {
      InitializeCriticalSection(&g_replayMarqueeLock);
      InterlockedExchange(&g_replayMarqueeLockState, 2);
      return;
    }
    Sleep(0);
  }
}

static COLORREF SafeGetPixelColor(HDC hdc, int x, int y, COLORREF fallback) {
  COLORREF color = GetPixel(hdc, x, y);
  return color == CLR_INVALID ? fallback : color;
}

static void RecordReplayRuntimeOverlay(int x, int y, int widthHint,
                                       const std::wstring &text) {
  if (text.empty() || x < 220 || y < 100 || y > 540)
    return;

  EnsureReplayMarqueeLock();
  EnterCriticalSection(&g_replayMarqueeLock);
  ReplayRuntimeOverlayItem *item = NULL;
  for (ReplayRuntimeOverlayItem &candidate : g_replayRuntimeOverlayItems) {
    if (abs(candidate.x - x) <= 4 && abs(candidate.y - y) <= 4) {
      item = &candidate;
      break;
    }
  }
  if (!item) {
    if (g_replayRuntimeOverlayItems.size() >= 80)
      g_replayRuntimeOverlayItems.erase(g_replayRuntimeOverlayItems.begin());
    g_replayRuntimeOverlayItems.push_back(ReplayRuntimeOverlayItem{});
    item = &g_replayRuntimeOverlayItems.back();
  }

  item->x = x;
  item->y = y;
  item->widthHint = widthHint;
  item->lastSeen = GetTickCount();
  item->text = text;
  LeaveCriticalSection(&g_replayMarqueeLock);
}

static void DrawReplayRuntimeOverlays(HDC hdc) {
  if (!hdc || InterlockedCompareExchange(&g_replayMarqueeLockState, 0, 0) != 2)
    return;

  std::vector<ReplayRuntimeOverlayItem> items;
  DWORD now = GetTickCount();
  EnterCriticalSection(&g_replayMarqueeLock);
  for (const ReplayRuntimeOverlayItem &item : g_replayRuntimeOverlayItems) {
    if ((DWORD)(now - item.lastSeen) <= 900)
      items.push_back(item);
  }
  LeaveCriticalSection(&g_replayMarqueeLock);

  if (items.empty())
    return;

  HFONT oldFont = SelectReplayTextFont(hdc, -8);
  int oldBkMode = SetBkMode(hdc, TRANSPARENT);
  COLORREF oldTextColor = SetTextColor(hdc, RGB(0, 56, 190));
  int oldCharExtra = GetTextCharacterExtra(hdc);
  SetTextCharacterExtra(hdc, 0);

  for (const ReplayRuntimeOverlayItem &item : items) {
    int width = item.widthHint;
    if (width < 168)
      width = 168;
    if (item.x + width > 735)
      width = 735 - item.x;
    if (width < 120)
      width = item.widthHint;

    RECT rc = {item.x - 2, item.y - 2, item.x + width + 2, item.y + 25};
    HBRUSH brush = CreateSolidBrush(RGB(84, 229, 223));
    if (brush) {
      FillRect(hdc, &rc, brush);
      DeleteObject(brush);
    }

    std::vector<std::wstring> lines;
    std::wstring current;
    size_t start = 0;
    while (start < item.text.size()) {
      while (start < item.text.size() && item.text[start] == L' ')
        ++start;
      size_t end = item.text.find(L' ', start);
      std::wstring word =
          item.text.substr(start, end == std::wstring::npos
                                      ? std::wstring::npos
                                      : end - start);
      if (word.empty())
        break;
      std::wstring candidate = current.empty() ? word : current + L" " + word;
      SIZE sz = {};
      Real_GetTextExtentPoint32W(hdc, candidate.c_str(), (int)candidate.size(),
                                 &sz);
      if (!current.empty() && sz.cx > width && lines.size() < 1) {
        lines.push_back(current);
        current = word;
      } else {
        current = candidate;
      }
      start = (end == std::wstring::npos) ? item.text.size() : end + 1;
    }
    if (!current.empty())
      lines.push_back(current);
    if (lines.empty())
      lines.push_back(item.text);
    if (lines.size() > 2) {
      for (size_t i = 2; i < lines.size(); ++i) {
        if (!lines[1].empty())
          lines[1].push_back(L' ');
        lines[1] += lines[i];
      }
      lines.resize(2);
    }

    for (size_t i = 0; i < lines.size(); ++i) {
      RECT lineRc = {item.x, item.y + (int)i * 10, item.x + width,
                     item.y + (int)i * 10 + 11};
      Real_ExtTextOutW(hdc, item.x, lineRc.top, ETO_CLIPPED, &lineRc,
                       lines[i].c_str(), (UINT)lines[i].size(), NULL);
    }
  }

  SetTextCharacterExtra(hdc, oldCharExtra);
  SetTextColor(hdc, oldTextColor);
  SetBkMode(hdc, oldBkMode);
  if (oldFont)
    SelectObject(hdc, oldFont);
}

struct ReplayGlyphRun {
  HDC hdc;
  int startX;
  int lastX;
  int y;
  DWORD lastSeen;
  std::wstring text;
};

static std::vector<ReplayGlyphRun> g_replayGlyphRuns;

static bool ReplayMapHasPrefixW(const std::wstring &text) {
  if (text.empty())
    return true;
  LoadReplayTranslationMap();
  for (const auto &entry : g_replayDisplayMapW) {
    if (entry.first.size() >= text.size() &&
        entry.first.compare(0, text.size(), text) == 0)
      return true;
  }
  for (const auto &entry : g_replayMapW) {
    if (entry.first.size() >= text.size() &&
        entry.first.compare(0, text.size(), text) == 0)
      return true;
  }
  return false;
}

static const std::wstring *ReplayMapExactW(const std::wstring &text) {
  if (text.empty())
    return NULL;
  LoadReplayTranslationMap();
  auto displayIt = g_replayDisplayMapW.find(text);
  if (displayIt != g_replayDisplayMapW.end())
    return &displayIt->second;
  auto it = g_replayMapW.find(text);
  return it == g_replayMapW.end() ? NULL : &it->second;
}

static bool IsReplayTrackableGlyphW(UINT ch) {
  if (ch < 0x20)
    return false;
  return true;
}

static void TrackReplayGlyphW(HDC hdc, UINT ch) {
  if (!hdc || !IsReplayTrackableGlyphW(ch))
    return;

  POINT pt = {};
  if (!GetCurrentPositionEx(hdc, &pt))
    return;

  DWORD now = GetTickCount();
  ReplayGlyphRun *run = NULL;
  for (ReplayGlyphRun &candidate : g_replayGlyphRuns) {
    if (candidate.hdc == hdc && abs(candidate.y - pt.y) <= 3 &&
        now - candidate.lastSeen <= 700 && pt.x >= candidate.lastX - 4 &&
        pt.x <= candidate.lastX + 36) {
      run = &candidate;
      break;
    }
  }

  if (!run) {
    if (g_replayGlyphRuns.size() >= 64)
      g_replayGlyphRuns.erase(g_replayGlyphRuns.begin());
    g_replayGlyphRuns.push_back(ReplayGlyphRun{});
    run = &g_replayGlyphRuns.back();
    run->hdc = hdc;
    run->startX = pt.x;
    run->y = pt.y;
    run->text.clear();
  }

  run->lastX = pt.x;
  run->lastSeen = now;
  run->text.push_back((wchar_t)ch);

  while (!run->text.empty() && !ReplayMapHasPrefixW(run->text))
    run->text.erase(run->text.begin());

  if (const std::wstring *translated = ReplayMapExactW(run->text)) {
    std::wstring render = *translated;
    NormalizeReplayTextW(render);
    RecordReplayRuntimeOverlay(run->startX, run->y - 2,
                               GetReplayTextWidthHint(run->startX), render);
    run->text.clear();
  }
}

static void UpdateReplayMarqueeItem(HDC hdc, const RECT &hoverRect,
                                    const RECT &textRect, int x, int y,
                                    int widthHint, int textWidth,
                                    const std::wstring &text) {
  if (textWidth <= widthHint || text.size() <= 22)
    return;

  EnsureReplayMarqueeLock();
  EnterCriticalSection(&g_replayMarqueeLock);
  ReplayMarqueeItem *item = NULL;
  for (ReplayMarqueeItem &candidate : g_replayMarqueeItems) {
    if (abs(candidate.x - x) <= 2 && abs(candidate.y - y) <= 2) {
      item = &candidate;
      break;
    }
  }
  if (!item) {
    if (g_replayMarqueeItems.size() >= 64)
      g_replayMarqueeItems.erase(g_replayMarqueeItems.begin());
    g_replayMarqueeItems.push_back(ReplayMarqueeItem{});
    item = &g_replayMarqueeItems.back();
  }

  item->hoverRect = hoverRect;
  item->textRect = textRect;
  item->x = x;
  item->y = y;
  item->widthHint = widthHint;
  item->textWidth = textWidth;
  item->lastSeen = GetTickCount();
  item->textColor = GetTextColor(hdc);
  item->bgColor = SafeGetPixelColor(hdc, x + widthHint / 2, y + 6,
                                    RGB(101, 235, 230));
  item->text = text;
  LogReplayMarqueeDebug("item.update", x, y, widthHint, textWidth,
                        text.c_str());
  LeaveCriticalSection(&g_replayMarqueeLock);
}

static bool GetHoveredReplayMarqueeItem(HWND hwnd, ReplayMarqueeItem *outItem) {
  if (!hwnd || !outItem ||
      InterlockedCompareExchange(&g_replayMarqueeLockState, 0, 0) != 2)
    return false;

  POINT pt = {};
  if (!GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt))
    return false;

  bool found = false;
  DWORD now = GetTickCount();
  EnterCriticalSection(&g_replayMarqueeLock);
  for (const ReplayMarqueeItem &item : g_replayMarqueeItems) {
    if ((DWORD)(now - item.lastSeen) > 60000)
      continue;
    if (PtInRect(&item.hoverRect, pt) && item.textWidth > item.widthHint) {
      *outItem = item;
      LogReplayMarqueeDebug("item.hover", pt.x, pt.y, item.widthHint,
                            item.textWidth, item.text.c_str());
      found = true;
      break;
    }
  }
  LeaveCriticalSection(&g_replayMarqueeLock);
  return found;
}

static void DrawReplayMarqueeOverlayToHdc(HWND hwnd, HDC hdc, bool releaseHdc) {
  if (!hdc)
    return;

  DrawReplayRuntimeOverlays(hdc);

  ReplayMarqueeItem item = {};
  if (!GetHoveredReplayMarqueeItem(hwnd, &item)) {
    if (releaseHdc)
      ReleaseDC(hwnd, hdc);
    return;
  }

  int overflow = item.textWidth - item.widthHint + 18;
  if (overflow < 1)
    overflow = 1;
  DWORD tick = GetTickCount();
  int phase = (int)((tick / 28) % (DWORD)(overflow * 2));
  int shift = phase <= overflow ? phase : (overflow * 2 - phase);
  LogReplayMarqueeDebug("overlay.draw", item.x, item.y, shift, overflow,
                        item.text.c_str());

  RECT rc = item.textRect;
  rc.left -= 1;
  rc.right += 1;
  rc.top -= 1;
  rc.bottom += 2;

  HBRUSH brush = CreateSolidBrush(item.bgColor);
  if (brush) {
    FillRect(hdc, &rc, brush);
    DeleteObject(brush);
  }

  HFONT oldFont = SelectReplayTextFont(hdc, -13);
  int oldBkMode = SetBkMode(hdc, TRANSPARENT);
  COLORREF oldTextColor = SetTextColor(hdc, item.textColor);
  int oldCharExtra = GetTextCharacterExtra(hdc);
  SetTextCharacterExtra(hdc, 0);

  Real_ExtTextOutW(hdc, item.x - shift, item.y, ETO_CLIPPED, &rc,
                   item.text.c_str(), (UINT)item.text.size(), NULL);

  SetTextCharacterExtra(hdc, oldCharExtra);
  SetTextColor(hdc, oldTextColor);
  SetBkMode(hdc, oldBkMode);
  if (oldFont)
    SelectObject(hdc, oldFont);
  if (releaseHdc)
    ReleaseDC(hwnd, hdc);
}

static void DrawReplayMarqueeOverlay(HWND hwnd) {
  if (!hwnd)
    return;
  HDC hdc = GetDC(hwnd);
  DrawReplayMarqueeOverlayToHdc(hwnd, hdc, true);
}

static HWND FindCurrentProcessWindow() {
  DWORD selfPid = GetCurrentProcessId();
  HWND hwnd = GetForegroundWindow();
  DWORD pid = 0;
  if (hwnd) {
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == selfPid)
      return hwnd;
  }

  for (hwnd = GetTopWindow(NULL); hwnd; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
      continue;
    pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == selfPid)
      return hwnd;
  }
  return NULL;
}

static DWORD WINAPI ReplayMarqueePumpThread(LPVOID) {
  while (InterlockedCompareExchange(&g_replayMarqueePumpRunning, 1, 1) == 1) {
    HWND hwnd = FindCurrentProcessWindow();
    if (hwnd)
      DrawReplayMarqueeOverlay(hwnd);
    Sleep(33);
  }
  return 0;
}

static void StartReplayMarqueePump() {
  if (InterlockedExchange(&g_replayMarqueePumpRunning, 1) == 1)
    return;
  g_replayMarqueePumpThread =
      CreateThread(NULL, 0, ReplayMarqueePumpThread, NULL, 0, NULL);
}

static void StopReplayMarqueePump() {
  InterlockedExchange(&g_replayMarqueePumpRunning, 0);
  if (g_replayMarqueePumpThread) {
    WaitForSingleObject(g_replayMarqueePumpThread, 500);
    CloseHandle(g_replayMarqueePumpThread);
    g_replayMarqueePumpThread = NULL;
  }
}

static int ComputeReplayMarqueeOffset(HDC hdc, const RECT &hoverRect,
                                      int textWidth, int widthHint) {
  if (textWidth <= widthHint)
    return 0;

  int overflow = textWidth - widthHint + 18;
  if (overflow <= 0)
    return 0;

  InterlockedExchange(&g_replayMarqueeActive, 1);
  InterlockedExchange(&g_replayMarqueeLastTick, (LONG)GetTickCount());

  static DWORD s_lastReplayMarqueeInvalidate = 0;
  DWORD tick = GetTickCount();
  if (tick - s_lastReplayMarqueeInvalidate >= 33) {
    s_lastReplayMarqueeInvalidate = tick;
    HWND hwnd = GetReplayDrawWindow(hdc);
    if (hwnd)
      RedrawWindow(hwnd, NULL, NULL,
                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
  }

  // Ping-pong scroll: starts readable from the left, reveals the clipped tail,
  // then returns. This avoids needing engine-native newline support.
  int phase = (int)((tick / 28) % (DWORD)(overflow * 2));
  int shift = phase <= overflow ? phase : (overflow * 2 - phase);

  return -shift;
}

struct ReplayTextLayout {
  int maxChars;
  int fontHeight;
  int lineHeight;
  int yOffset;
};

static ReplayTextLayout GetReplayTextLayout(size_t textLen, bool titlePosition) {
  if (!titlePosition)
    return {24, -16, 16, 0};

  // Scene replay titles sit inside narrow list cells. Use a compact Latin
  // font profile so translated titles look like the original compact layout
  // instead of oversized Japanese-spaced text.
  (void)textLen;
  return {24, -9, 10, 0};
}

static void NormalizeReplayTextW(std::wstring &text) {
  for (wchar_t &ch : text) {
    if (ch == L'\r' || ch == L'\n' || ch == L'\t' || ch == 0x3000) {
      ch = L' ';
    } else if (ch >= 0xFF01 && ch <= 0xFF5E) {
      ch = (wchar_t)(ch - 0xFEE0);
    } else if (ch == L'~' || ch == L'^') {
      ch = L' ';
    }
  }
  while (text.find(L"  ") != std::wstring::npos)
    text.replace(text.find(L"  "), 2, L" ");
  while (!text.empty() && text.front() == L' ')
    text.erase(text.begin());
  while (!text.empty() && text.back() == L' ')
    text.pop_back();
}

static void NormalizeReplayTextA(std::string &text) {
  std::wstring wide;
  if (SjisToWideString(text, wide)) {
    NormalizeReplayTextW(wide);
    std::string compact;
    if (WideToSjisString(wide, compact)) {
      text = compact;
      return;
    }
  }

  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    unsigned char b1 = (unsigned char)text[i];
    if (b1 == 0x81 && i + 1 < text.size() &&
        (unsigned char)text[i + 1] == 0x40) {
      out.push_back(' ');
      ++i;
      continue;
    }
    if (b1 == '~' || b1 == '^') {
      out.push_back(' ');
      continue;
    }
    out.push_back((char)b1);
  }
  while (out.find("  ") != std::string::npos)
    out.replace(out.find("  "), 2, " ");
  while (!out.empty() && out.front() == ' ')
    out.erase(out.begin());
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  text = out;
}

static std::vector<std::string> WrapReplayTextA(std::string text,
                                                int maxChars) {
  std::vector<std::string> lines;
  NormalizeReplayTextA(text);

  std::string line;
  size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && text[start] == ' ')
      ++start;
    size_t end = text.find(' ', start);
    std::string word =
        text.substr(start, end == std::string::npos ? std::string::npos
                                                    : end - start);
    if (word.empty())
      break;

    if (!line.empty() && (int)(line.size() + 1 + word.size()) > maxChars) {
      lines.push_back(line);
      line.clear();
    }
    if (!line.empty())
      line.push_back(' ');
    line += word;
    start = (end == std::string::npos) ? text.size() : end + 1;
  }
  if (!line.empty())
    lines.push_back(line);
  if (lines.empty())
    lines.push_back(text);
  return lines;
}

static std::vector<std::wstring> WrapReplayTextW(std::wstring text,
                                                 int maxChars) {
  std::vector<std::wstring> lines;
  NormalizeReplayTextW(text);

  std::wstring line;
  size_t start = 0;
  while (start < text.size()) {
    while (start < text.size() && text[start] == L' ')
      ++start;
    size_t end = text.find(L' ', start);
    std::wstring word =
        text.substr(start, end == std::wstring::npos ? std::wstring::npos
                                                     : end - start);
    if (word.empty())
      break;

    if (!line.empty() && (int)(line.size() + 1 + word.size()) > maxChars) {
      lines.push_back(line);
      line.clear();
    }
    if (!line.empty())
      line.push_back(L' ');
    line += word;
    start = (end == std::wstring::npos) ? text.size() : end + 1;
  }
  if (!line.empty())
    lines.push_back(line);
  if (lines.empty())
    lines.push_back(text);
  return lines;
}

static BOOL DrawReplayTextA(HDC hdc, int x, int y, UINT options,
                            const RECT *lprect, LPCSTR text, UINT len,
                            bool extTextOut) {
  std::string render(text, text + len);
  NormalizeReplayTextA(render);
  bool titlePosition = IsReplayTitleTextPosition(x, y);
  int widthHint = GetReplayTextWidthHint(x);
  ReplayTextLayout layout = GetReplayTextLayout(render.size(), titlePosition);
  std::vector<std::string> lines =
      titlePosition ? WrapReplayTextA(render, layout.maxChars)
                    : std::vector<std::string>{render};
  if (titlePosition && lines.size() > 2) {
    for (size_t i = 2; i < lines.size(); ++i) {
      if (!lines[1].empty())
        lines[1].push_back(' ');
      lines[1] += lines[i];
    }
    lines.resize(2);
  }
  int drawY = y + layout.yOffset;

  HFONT oldFont = SelectReplayTextFont(hdc, layout.fontHeight);
  int oldCharExtra = GetTextCharacterExtra(hdc);
  int oldBkMode = SetBkMode(hdc, TRANSPARENT);
  SetTextCharacterExtra(hdc, 0);
  BOOL result = TRUE;
  for (size_t i = 0; i < lines.size(); ++i) {
    int ly = drawY + (int)i * layout.lineHeight;
    RECT rc = {x, ly, x + widthHint, ly + layout.lineHeight + 2};
    const std::string &line = lines[i];
    int drawX = x;
    if (titlePosition && i == 0) {
      SIZE textSize = {};
      Real_GetTextExtentPoint32A(hdc, line.c_str(), (int)line.size(),
                                 &textSize);
      if (line.size() > 22 && textSize.cx <= widthHint)
        textSize.cx = widthHint + (int)(line.size() - 22) * 6 + 24;
      RECT hoverRect = {};
      GetReplayHoverRect(hdc, x, ly, widthHint, layout.lineHeight, lprect,
                         &hoverRect);
      std::wstring wideLine;
      if (SjisToWideString(line, wideLine)) {
        LogReplayMarqueeDebug("draw.titleA", x, ly, widthHint, textSize.cx,
                              wideLine.c_str());
        UpdateReplayMarqueeItem(hdc, hoverRect, rc, x, ly, widthHint,
                                textSize.cx, wideLine);
      }
      drawX += ComputeReplayMarqueeOffset(hdc, hoverRect, textSize.cx,
                                          widthHint);
    }
    result = Real_ExtTextOutA(hdc, drawX, ly, ETO_CLIPPED, &rc, line.c_str(),
                              (UINT)line.size(), NULL);
  }
  SetTextCharacterExtra(hdc, oldCharExtra);
  SetBkMode(hdc, oldBkMode);
  if (oldFont)
    SelectObject(hdc, oldFont);
  return result;
}

static BOOL DrawReplayTextW(HDC hdc, int x, int y, UINT options,
                            const RECT *lprect, LPCWSTR text, UINT len,
                            bool extTextOut) {
  std::wstring render(text, text + len);
  NormalizeReplayTextW(render);
  bool titlePosition = IsReplayTitleTextPosition(x, y);
  int widthHint = GetReplayTextWidthHint(x);
  ReplayTextLayout layout = GetReplayTextLayout(render.size(), titlePosition);
  std::vector<std::wstring> lines =
      titlePosition ? WrapReplayTextW(render, layout.maxChars)
                    : std::vector<std::wstring>{render};
  if (titlePosition && lines.size() > 2) {
    for (size_t i = 2; i < lines.size(); ++i) {
      if (!lines[1].empty())
        lines[1].push_back(L' ');
      lines[1] += lines[i];
    }
    lines.resize(2);
  }
  int drawY = y + layout.yOffset;

  HFONT oldFont = SelectReplayTextFont(hdc, layout.fontHeight);
  int oldCharExtra = GetTextCharacterExtra(hdc);
  int oldBkMode = SetBkMode(hdc, TRANSPARENT);
  SetTextCharacterExtra(hdc, 0);
  BOOL result = TRUE;
  for (size_t i = 0; i < lines.size(); ++i) {
    int ly = drawY + (int)i * layout.lineHeight;
    RECT rc = {x, ly, x + widthHint, ly + layout.lineHeight + 2};
    const std::wstring &line = lines[i];
    int drawX = x;
    if (titlePosition && i == 0) {
      SIZE textSize = {};
      Real_GetTextExtentPoint32W(hdc, line.c_str(), (int)line.size(),
                                 &textSize);
      if (line.size() > 22 && textSize.cx <= widthHint)
        textSize.cx = widthHint + (int)(line.size() - 22) * 6 + 24;
      RECT hoverRect = {};
      GetReplayHoverRect(hdc, x, ly, widthHint, layout.lineHeight, lprect,
                         &hoverRect);
      LogReplayMarqueeDebug("draw.titleW", x, ly, widthHint, textSize.cx,
                            line.c_str());
      UpdateReplayMarqueeItem(hdc, hoverRect, rc, x, ly, widthHint,
                              textSize.cx, line);
      drawX += ComputeReplayMarqueeOffset(hdc, hoverRect, textSize.cx,
                                          widthHint);
    }
    result = Real_ExtTextOutW(hdc, drawX, ly, ETO_CLIPPED, &rc, line.c_str(),
                              (UINT)line.size(), NULL);
  }
  SetTextCharacterExtra(hdc, oldCharExtra);
  SetBkMode(hdc, oldBkMode);
  if (oldFont)
    SelectObject(hdc, oldFont);
  return result;
}

static BOOL WINAPI Hook_BitBlt(HDC hdcDest, int xDest, int yDest, int width,
                               int height, HDC hdcSrc, int xSrc, int ySrc,
                               DWORD rop) {
  BOOL result =
      Real_BitBlt(hdcDest, xDest, yDest, width, height, hdcSrc, xSrc, ySrc, rop);
  HWND hwnd = WindowFromDC(hdcDest);
  if (!hwnd)
    hwnd = FindCurrentProcessWindow();
  if (hwnd)
    DrawReplayMarqueeOverlayToHdc(hwnd, hdcDest, false);
  return result;
}

static BOOL WINAPI Hook_StretchBlt(HDC hdcDest, int xDest, int yDest,
                                   int destWidth, int destHeight, HDC hdcSrc,
                                   int xSrc, int ySrc, int srcWidth,
                                   int srcHeight, DWORD rop) {
  BOOL result = Real_StretchBlt(hdcDest, xDest, yDest, destWidth, destHeight,
                                hdcSrc, xSrc, ySrc, srcWidth, srcHeight, rop);
  HWND hwnd = WindowFromDC(hdcDest);
  if (!hwnd)
    hwnd = FindCurrentProcessWindow();
  if (hwnd)
    DrawReplayMarqueeOverlayToHdc(hwnd, hdcDest, false);
  return result;
}

static HRESULT WINAPI Hook_DirectDrawCreate(GUID FAR *guid,
                                            LPDIRECTDRAW FAR *dd,
                                            IUnknown FAR *outer);
static HRESULT WINAPI Hook_DDS_Blt(LPDIRECTDRAWSURFACE dest, LPRECT destRect,
                                   LPDIRECTDRAWSURFACE src, LPRECT srcRect,
                                   DWORD flags, LPDDBLTFX fx);
static HRESULT WINAPI Hook_DDS_Flip(LPDIRECTDRAWSURFACE surface,
                                    LPDIRECTDRAWSURFACE targetOverride,
                                    DWORD flags);

static void DrawReplayMarqueeOverlayToSurface(LPDIRECTDRAWSURFACE surface) {
  if (!surface)
    return;
  HWND hwnd = FindCurrentProcessWindow();
  ReplayMarqueeItem probe = {};
  if (!GetHoveredReplayMarqueeItem(hwnd, &probe))
    return;

  HDC hdc = NULL;
  if (FAILED(surface->GetDC(&hdc)) || !hdc)
    return;
  LogReplayMarqueeDebug("surface.getdc.ok");
  DrawReplayMarqueeOverlayToHdc(hwnd, hdc, false);
  surface->ReleaseDC(hdc);
}

static void PatchDirectDrawSurface(LPDIRECTDRAWSURFACE surface) {
  if (!surface)
    return;
  if (InterlockedCompareExchange(&g_directDrawSurfaceHooked, 1, 0) != 0)
    return;

  void **vtbl = *(void ***)surface;
  Real_DDS_Blt = (DDSBltProc)vtbl[5];
  Real_DDS_Flip = (DDSFlipProc)vtbl[11];
  LogReplayMarqueeDebug("ddsurface.patch", (int)(uintptr_t)surface,
                        Real_DDS_Blt ? 1 : 0, Real_DDS_Flip ? 1 : 0, 0);

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  if (Real_DDS_Blt)
    DetourAttach(&(PVOID &)Real_DDS_Blt, Hook_DDS_Blt);
  if (Real_DDS_Flip)
    DetourAttach(&(PVOID &)Real_DDS_Flip, Hook_DDS_Flip);
  DetourTransactionCommit();
}

static HRESULT WINAPI Hook_DD_CreateSurface(LPDIRECTDRAW dd,
                                            LPDDSURFACEDESC desc,
                                            LPDIRECTDRAWSURFACE FAR *surface,
                                            IUnknown FAR *outer) {
  HRESULT hr = Real_DD_CreateSurface(dd, desc, surface, outer);
  LogReplayMarqueeDebug("dd.createsurface", SUCCEEDED(hr) ? 1 : 0,
                        surface && *surface ? 1 : 0, desc ? desc->dwFlags : 0,
                        desc ? desc->ddsCaps.dwCaps : 0);
  if (SUCCEEDED(hr) && surface && *surface)
    PatchDirectDrawSurface(*surface);
  return hr;
}

static void PatchDirectDraw(LPDIRECTDRAW dd) {
  if (!dd)
    return;
  if (InterlockedCompareExchange(&g_directDrawHooked, 1, 0) != 0)
    return;

  void **vtbl = *(void ***)dd;
  Real_DD_CreateSurface = (DDCreateSurfaceProc)vtbl[6];
  LogReplayMarqueeDebug("dd.patch", (int)(uintptr_t)dd,
                        Real_DD_CreateSurface ? 1 : 0, 0, 0);

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  if (Real_DD_CreateSurface)
    DetourAttach(&(PVOID &)Real_DD_CreateSurface, Hook_DD_CreateSurface);
  DetourTransactionCommit();
}

static HRESULT WINAPI Hook_DirectDrawCreate(GUID FAR *guid,
                                            LPDIRECTDRAW FAR *dd,
                                            IUnknown FAR *outer) {
  HRESULT hr = Real_DirectDrawCreate(guid, dd, outer);
  LogReplayMarqueeDebug("dd.create", SUCCEEDED(hr) ? 1 : 0,
                        dd && *dd ? 1 : 0, 0, 0);
  if (SUCCEEDED(hr) && dd && *dd)
    PatchDirectDraw(*dd);
  return hr;
}

static HRESULT WINAPI Hook_DDS_Blt(LPDIRECTDRAWSURFACE dest, LPRECT destRect,
                                   LPDIRECTDRAWSURFACE src, LPRECT srcRect,
                                   DWORD flags, LPDDBLTFX fx) {
  HRESULT hr = Real_DDS_Blt(dest, destRect, src, srcRect, flags, fx);
  LogReplayMarqueeDebug("dds.blt", SUCCEEDED(hr) ? 1 : 0, flags, 0, 0);
  if (SUCCEEDED(hr))
    DrawReplayMarqueeOverlayToSurface(dest);
  return hr;
}

static HRESULT WINAPI Hook_DDS_Flip(LPDIRECTDRAWSURFACE surface,
                                    LPDIRECTDRAWSURFACE targetOverride,
                                    DWORD flags) {
  HRESULT hr = Real_DDS_Flip(surface, targetOverride, flags);
  LogReplayMarqueeDebug("dds.flip", SUCCEEDED(hr) ? 1 : 0, flags, 0, 0);
  if (SUCCEEDED(hr))
    DrawReplayMarqueeOverlayToSurface(surface);
  return hr;
}

static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR fileName) {
  HMODULE module = Real_LoadLibraryA(fileName);
  if (fileName && module) {
    const char *base = strrchr(fileName, '\\');
    base = base ? base + 1 : fileName;
    if (_stricmp(base, "ddraw.dll") == 0) {
      DirectDrawCreateProc proc =
          (DirectDrawCreateProc)Real_GetProcAddress(module, "DirectDrawCreate");
      if (proc)
        Real_DirectDrawCreate = proc;
      LogReplayMarqueeDebug("load.ddrawA", proc ? 1 : 0, 0, 0, 0);
    }
  }
  return module;
}

static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR fileName) {
  HMODULE module = Real_LoadLibraryW(fileName);
  if (fileName && module) {
    const wchar_t *base = wcsrchr(fileName, L'\\');
    base = base ? base + 1 : fileName;
    if (_wcsicmp(base, L"ddraw.dll") == 0) {
      DirectDrawCreateProc proc =
          (DirectDrawCreateProc)Real_GetProcAddress(module, "DirectDrawCreate");
      if (proc)
        Real_DirectDrawCreate = proc;
      LogReplayMarqueeDebug("load.ddrawW", proc ? 1 : 0, 0, 0, 0);
    }
  }
  return module;
}

static FARPROC WINAPI Hook_GetProcAddress(HMODULE module, LPCSTR procName) {
  FARPROC proc = Real_GetProcAddress(module, procName);
  if (procName && HIWORD(procName) &&
      strcmp(procName, "DirectDrawCreate") == 0 && proc) {
    Real_DirectDrawCreate = (DirectDrawCreateProc)proc;
    LogReplayMarqueeDebug("getproc.ddcreate", 1, 0, 0, 0);
    return (FARPROC)Hook_DirectDrawCreate;
  }
  return proc;
}

static void InstallDirectDrawHooksFromProbe() {
  if (!Real_DirectDrawCreate)
    return;

  LPDIRECTDRAW dd = NULL;
  HRESULT hr = Real_DirectDrawCreate(NULL, &dd, NULL);
  LogReplayMarqueeDebug("probe.ddcreate", SUCCEEDED(hr) ? 1 : 0,
                        dd ? 1 : 0, 0, 0);
  if (FAILED(hr) || !dd)
    return;

  PatchDirectDraw(dd);

  DDSURFACEDESC desc = {};
  desc.dwSize = sizeof(desc);
  desc.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
  desc.dwWidth = 8;
  desc.dwHeight = 8;
  desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;

  LPDIRECTDRAWSURFACE surface = NULL;
  hr = dd->CreateSurface(&desc, &surface, NULL);
  LogReplayMarqueeDebug("probe.surface", SUCCEEDED(hr) ? 1 : 0,
                        surface ? 1 : 0, desc.dwFlags, desc.ddsCaps.dwCaps);
  if (SUCCEEDED(hr) && surface) {
    PatchDirectDrawSurface(surface);
    surface->Release();
  }
  dd->Release();
}

// ============================================================================
// Memory String Patcher - patches strings directly in the loaded EXE image
// ============================================================================
// Scans .rdata and .data sections and replaces Japanese strings with
// Indonesian translations in-place. Only patches where the translation
// fits within the original string's byte space.

static void PatchStringsInMemory() {
  if (g_fontManager.GetLanguage() != 1)
    return;

  HMODULE hExe = GetModuleHandleA(NULL);
  if (!hExe)
    return;

  BYTE *base = (BYTE *)hExe;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return;

  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return;

  IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
  int numSections = nt->FileHeader.NumberOfSections;

  // Make all target sections writable first
  DWORD oldProtects[32] = {};
  for (int s = 0; s < numSections && s < 32; s++) {
    char secName[9] = {};
    memcpy(secName, sec[s].Name, 8);
    if (strcmp(secName, ".rdata") != 0 && strcmp(secName, ".data") != 0 &&
        strcmp(secName, ".text") != 0)
      continue;
    BYTE *secStart = base + sec[s].VirtualAddress;
    DWORD secSize = sec[s].Misc.VirtualSize;
    VirtualProtect(secStart, secSize, PAGE_READWRITE, &oldProtects[s]);
  }

  for (int i = 0; g_uiTranslations[i].japanese != nullptr; i++) {
    const char *jp = g_uiTranslations[i].japanese;
    const char *id = g_uiTranslations[i].indonesian;
    int jpLen = (int)strlen(jp);
    int idLen = (int)strlen(id);

    if (idLen <= jpLen) {
      // === PASS 1: In-place replacement (translation fits) ===
      for (int s = 0; s < numSections; s++) {
        char secName[9] = {};
        memcpy(secName, sec[s].Name, 8);
        if (strcmp(secName, ".rdata") != 0 && strcmp(secName, ".data") != 0)
          continue;

        BYTE *secStart = base + sec[s].VirtualAddress;
        DWORD secSize = sec[s].Misc.VirtualSize;

        for (DWORD offset = 0; offset + jpLen <= secSize; offset++) {
          if (memcmp(secStart + offset, jp, jpLen) == 0) {
            if (secStart[offset + jpLen] != 0)
              continue;
            memcpy(secStart + offset, id, idLen);
            memset(secStart + offset + idLen, 0, jpLen - idLen + 1);
          }
        }
      }
    } else {
      // === PASS 2: Pointer redirection (translation too long) ===
      // 1. Find address of the SJIS string in the EXE's data sections
      BYTE *strAddr = nullptr;
      for (int s = 0; s < numSections && !strAddr; s++) {
        char secName[9] = {};
        memcpy(secName, sec[s].Name, 8);
        if (strcmp(secName, ".rdata") != 0 && strcmp(secName, ".data") != 0)
          continue;

        BYTE *secStart = base + sec[s].VirtualAddress;
        DWORD secSize = sec[s].Misc.VirtualSize;

        for (DWORD offset = 0; offset + jpLen <= secSize; offset++) {
          if (memcmp(secStart + offset, jp, jpLen) == 0) {
            if (secStart[offset + jpLen] != 0)
              continue;
            strAddr = secStart + offset;
            break;
          }
        }
      }

      if (!strAddr)
        continue;

      // 2. Allocate persistent memory for the longer Indonesian string
      char *newStr = (char *)VirtualAlloc(
          NULL, idLen + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      if (!newStr)
        continue;
      memcpy(newStr, id, idLen + 1);

      // 3. Scan ALL sections for pointers (4-byte values) to the original
      // string
      //    and redirect them to our new string
      DWORD oldAddr = (DWORD)(uintptr_t)strAddr;
      DWORD newAddr = (DWORD)(uintptr_t)newStr;

      for (int s = 0; s < numSections; s++) {
        char secName[9] = {};
        memcpy(secName, sec[s].Name, 8);
        // Scan .rdata, .data, and .text for pointer references
        if (strcmp(secName, ".rdata") != 0 && strcmp(secName, ".data") != 0 &&
            strcmp(secName, ".text") != 0)
          continue;

        BYTE *secStart = base + sec[s].VirtualAddress;
        DWORD secSize = sec[s].Misc.VirtualSize;

        // Scan for 4-byte pointer values matching the original string address
        for (DWORD offset = 0; offset + 4 <= secSize; offset++) {
          DWORD val;
          memcpy(&val, secStart + offset, 4);
          if (val == oldAddr) {
            memcpy(secStart + offset, &newAddr, 4);
          }
        }
      }
    }
  }

  // Restore original protections
  for (int s = 0; s < numSections && s < 32; s++) {
    if (oldProtects[s] == 0)
      continue;
    char secName[9] = {};
    memcpy(secName, sec[s].Name, 8);
    if (strcmp(secName, ".rdata") != 0 && strcmp(secName, ".data") != 0 &&
        strcmp(secName, ".text") != 0)
      continue;
    BYTE *secStart = base + sec[s].VirtualAddress;
    DWORD secSize = sec[s].Misc.VirtualSize;
    VirtualProtect(secStart, secSize, oldProtects[s], &oldProtects[s]);
  }
}

// Hook GetTextMetricsA to reduce line height in backlog
// This is the KEY hook for line spacing - the game uses tmHeight for line
// spacing
static BOOL WINAPI Hook_GetTextMetricsA(HDC hdc, LPTEXTMETRICA lptm) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_GetTextMetricsA(hdc, lptm);
  BOOL res = Real_GetTextMetricsA(hdc, lptm);
  if (res && g_inBacklogRender) {
    int spacing = g_fontManager.GetBacklogLineSpacing();
    lptm->tmHeight += spacing;
    lptm->tmAscent += spacing;
  }
  return res;
}

static BOOL WINAPI Hook_GetTextMetricsW(HDC hdc, LPTEXTMETRICW lptm) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_GetTextMetricsW(hdc, lptm);
  BOOL res = Real_GetTextMetricsW(hdc, lptm);
  if (res && g_inBacklogRender) {
    int spacing = g_fontManager.GetBacklogLineSpacing();
    lptm->tmHeight += spacing;
    lptm->tmAscent += spacing;
  }
  return res;
}

static inline bool IsNonRenderableTextControlA(UINT ch) {
  return ch < 0x20 || ch == 0x7F;
}

static inline bool IsNonRenderableTextControlW(UINT ch) {
  return ch < 0x20 || ch == 0x7F;
}

static bool IsOnlyTextControlA(LPCSTR s, UINT c) {
  if (!s || c == 0)
    return false;
  for (UINT i = 0; i < c; ++i) {
    if (!IsNonRenderableTextControlA((unsigned char)s[i]))
      return false;
  }
  return true;
}

static bool IsOnlyTextControlW(LPCWSTR s, UINT c) {
  if (!s || c == 0)
    return false;
  for (UINT i = 0; i < c; ++i) {
    if (!IsNonRenderableTextControlW((UINT)s[i]))
      return false;
  }
  return true;
}

// Translate full-width punctuation, letters, and numbers into their 1-byte
// ASCII equivalents
static const char *FilterSjisString(LPCSTR lpString, UINT *c_inout,
                                    const INT *lpDx_in, INT **lpDx_out,
                                    bool performTranslation = true,
                                    bool replaceKatakanaDot = true) {
  if (!lpString || *c_inout == 0)
    return lpString;

  static thread_local char
      s_buffer[1024]; // Safe upper bound for text out calls
  static thread_local INT
      s_dx_buffer[1024]; // Safe upper bound for text out calls

  UINT len = *c_inout;
  UINT originalC = *c_inout; // Store original length for lpDx_in indexing
  if (len >= sizeof(s_buffer)) {
    len = sizeof(s_buffer) - 1; // Truncate safely
  }

  bool modified = false;
  UINT outLen = 0;
  auto IsAsciiAlphaNum = [](char c) -> bool {
    return ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z'));
  };
  auto IsAsciiAlphaNumAt = [&](int pos) -> bool {
    if (pos < 0 || (UINT)pos >= len)
      return false;
    unsigned char c = (unsigned char)lpString[pos];
    return ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z'));
  };
  auto IsFullWidthLatinAt = [&](int pos) -> bool {
    if (pos < 0 || (UINT)(pos + 1) >= len)
      return false;
    unsigned char b1 = (unsigned char)lpString[pos];
    unsigned char b2 = (unsigned char)lpString[pos + 1];
    if (b1 == 0x82) {
      return ((b2 >= 0x4F && b2 <= 0x58) || (b2 >= 0x60 && b2 <= 0x79) ||
              (b2 >= 0x81 && b2 <= 0x9A));
    }
    return false;
  };
  for (UINT i = 0; i < len; i++) {
    unsigned char ch1 = (unsigned char)lpString[i];

    if (ch1 == '\t') {
      s_buffer[outLen++] = ' ';
      modified = true;
      continue;
    }

    if (IsNonRenderableTextControlA(ch1)) {
      modified = true;
      continue;
    }

    // Check for double-byte Shift-JIS characters
    if ((ch1 >= 0x81 && ch1 <= 0x9F) || (ch1 >= 0xE0 && ch1 <= 0xFC)) {
      if (i + 1 < len) {
        unsigned char ch2 = (unsigned char)lpString[i + 1];
        char replacement = 0;

        if (performTranslation) {
          if (ch1 == 0x81) {
            switch (ch2) {
            case 0x40:
              // Full-width space can be a synthetic separator in MES exports.
              {
                bool prevLatin = IsFullWidthLatinAt((int)i - 2);
                bool nextLatin = IsFullWidthLatinAt((int)i + 2);
                if (prevLatin && nextLatin) {
                  modified = true;
                  i++; // consume 2-byte full-width space
                  continue;
                }
                replacement = ' ';
              }
              break; // Full space
            case 0x41:
              replacement = ',';
              break; // 、
            case 0x42:
              replacement = '.';
              break; // 。
            case 0x43:
              replacement = ',';
              break; // ，
            case 0x44:
              replacement = '.';
              break; // ．
            case 0x45:
              // Middle-dot often appears as synthetic separator between
              // full-width latin chars from MES exports. Drop it there.
              {
                bool prevAlphaNum = IsAsciiAlphaNumAt((int)i - 1) ||
                                    IsFullWidthLatinAt((int)i - 2);
                bool nextAlphaNum = IsAsciiAlphaNumAt((int)i + 2) ||
                                    IsFullWidthLatinAt((int)i + 2);
                if (prevAlphaNum && nextAlphaNum) {
                  modified = true;
                  i++; // consume this 2-byte symbol
                  continue;
                }
                replacement = ' ';
              }
              break; // ・
            case 0x46:
              replacement = ':';
              break; // ：
            case 0x47:
              replacement = ';';
              break; // ；
            case 0x48:
              replacement = '?';
              break; // ？
            case 0x49:
              replacement = '!';
              break; // ！
            case 0x5B:
              replacement = '-';
              break; // ー
            case 0x5C:
              replacement = '-';
              break; // ―
            case 0x5D:
              replacement = '-';
              break; // ‐
            case 0x5E:
              replacement = '/';
              break; // ／
            case 0x5F:
              replacement = '\\';
              break; // ＼
            case 0x60:
              replacement = '~';
              break; // ～
            case 0x61:
              replacement = '|';
              break; // ∥
            case 0x62:
              replacement = '|';
              break; // ｜
            case 0x65:
              replacement = '\'';
              break; // ‘
            case 0x66:
              replacement = '\'';
              break; // ’
            case 0x67:
              replacement = '\"';
              break; // “
            case 0x68:
              replacement = '\"';
              break; // ”
            case 0x75:
              replacement = '\"';
              break; // 「
            case 0x76:
              replacement = '\"';
              break; // 」
            case 0x77:
              replacement = '\"';
              break; // 『
            case 0x78:
              replacement = '\"';
              break; // 』
            case 0x69:
              replacement = '(';
              break; // （
            case 0x6A:
              replacement = ')';
              break; // ）
            case 0x6D:
              replacement = '[';
              break; // ［
            case 0x6E:
              replacement = ']';
              break; // ］
            case 0x6F:
              replacement = '{';
              break; // ｛
            case 0x70:
              replacement = '}';
              break; // ｝
            case 0x7B:
              replacement = '+';
              break; // ＋
            case 0x7C:
              replacement = '-';
              break; // －
            case 0x81:
              replacement = '=';
              break; // ＝
            case 0x83:
              replacement = '<';
              break; // ＜
            case 0x84:
              replacement = '>';
              break; // ＞
            case 0x93:
              replacement = '%';
              break; // ％
            case 0x94:
              replacement = '#';
              break; // ＃
            case 0x95:
              replacement = '&';
              break; // ＆
            case 0x96:
              replacement = '*';
              break; // ＊
            case 0x97:
              replacement = '@';
              break; // ＠
            }
          } else if (ch1 == 0x82) {
            if (ch2 >= 0x4F && ch2 <= 0x58) { // ０-９
              replacement = '0' + (ch2 - 0x4F);
            } else if (ch2 >= 0x60 && ch2 <= 0x79) { // Ａ-Ｚ
              replacement = 'A' + (ch2 - 0x60);
            } else if (ch2 >= 0x81 && ch2 <= 0x9A) { // ａ-ｚ
              replacement = 'a' + (ch2 - 0x81);
            }
          }
        }

        if (replacement != 0) {
          s_buffer[outLen] = replacement;
          if (lpDx_in && lpDx_out) {
            s_dx_buffer[outLen] =
                lpDx_in[i]; // The first byte takes the advance width
            // What about the second byte in lpDx? The game usually sets
            // dx[i]=width, dx[i+1]=0 for Japanese characters. We can safely
            // absorb both elements' widths into this single character if they
            // were split. Typically, lpDx_in[i] has the full width of the
            // character and lpDx_in[i+1] is 0.
            if (i + 1 < originalC && lpDx_in[i + 1] > 0) {
              s_dx_buffer[outLen] += lpDx_in[i + 1];
            }
          }
          outLen++;
          modified = true;
          i++; // Skip the second byte
          continue;
        }

        // No translation found, copy the full 2-byte char as-is
        s_buffer[outLen] = lpString[i];
        if (lpDx_in && lpDx_out)
          s_dx_buffer[outLen] = lpDx_in[i];
        outLen++;

        s_buffer[outLen] = lpString[i + 1];
        if (lpDx_in && lpDx_out) {
          if (i + 1 < originalC) {
            s_dx_buffer[outLen] = lpDx_in[i + 1];
          } else {
            s_dx_buffer[outLen] = 0; // Should not happen if len is correct
          }
        }
        outLen++;

        i++;
      } else {
        // Truncated multi-byte
        s_buffer[outLen] = lpString[i];
        if (lpDx_in && lpDx_out)
          s_dx_buffer[outLen] = lpDx_in[i];
        outLen++;
      }
      continue;
    }

    // Single byte checks
    if (replaceKatakanaDot &&
        ch1 == '_') { // underscore placeholder from MES tool
      bool prevAlphaNum = false;
      bool nextAlphaNum = false;
      if (i > 0) {
        unsigned char p = (unsigned char)lpString[i - 1];
        prevAlphaNum = ((p >= '0' && p <= '9') || (p >= 'A' && p <= 'Z') ||
                        (p >= 'a' && p <= 'z'));
      }
      if (i + 1 < len) {
        unsigned char n = (unsigned char)lpString[i + 1];
        nextAlphaNum = ((n >= '0' && n <= '9') || (n >= 'A' && n <= 'Z') ||
                        (n >= 'a' && n <= 'z'));
      }

      // Common broken export pattern: A_B_C_D. Drop the separator.
      if (prevAlphaNum && nextAlphaNum) {
        modified = true;
        continue;
      }

      // Otherwise treat as normal word space.
      s_buffer[outLen] = 0x20;
      if (lpDx_in && lpDx_out)
        s_dx_buffer[outLen] = lpDx_in[i];
      outLen++;
      modified = true;
    } else {
      s_buffer[outLen] = lpString[i];
      if (lpDx_in && lpDx_out)
        s_dx_buffer[outLen] = lpDx_in[i];
      outLen++;
    }
  }

  // If this line is anomalously split into single-letter tokens
  // ("A w a l d a r i ..."), collapse synthetic inter-letter spaces.
  if (performTranslation && outLen >= 8) {
    int tokenCount = 0;
    int singleTokenCount = 0;
    int currLen = 0;
    for (UINT k = 0; k < outLen; k++) {
      char ch = s_buffer[k];
      if (ch == ' ' || ch == '\t') {
        if (currLen > 0) {
          tokenCount++;
          if (currLen == 1)
            singleTokenCount++;
          currLen = 0;
        }
      } else if (IsAsciiAlphaNum(ch)) {
        currLen++;
      } else {
        if (currLen > 0) {
          tokenCount++;
          if (currLen == 1)
            singleTokenCount++;
          currLen = 0;
        }
      }
    }
    if (currLen > 0) {
      tokenCount++;
      if (currLen == 1)
        singleTokenCount++;
    }

    bool spacedAnomaly = (tokenCount >= 8) &&
                         (singleTokenCount * 100 >= tokenCount * 75);
    if (spacedAnomaly) {
      UINT write = 0;
      for (UINT k = 0; k < outLen; k++) {
        char ch = s_buffer[k];
        if (ch == ' ' && k > 0 && k + 1 < outLen &&
            IsAsciiAlphaNum(s_buffer[k - 1]) &&
            IsAsciiAlphaNum(s_buffer[k + 1])) {
          continue; // drop synthetic inter-letter space
        }
        s_buffer[write] = ch;
        if (lpDx_in && lpDx_out)
          s_dx_buffer[write] = s_dx_buffer[k];
        write++;
      }
      if (write < outLen)
        modified = true;
      outLen = write;
    }
  }

  if (modified) {
    s_buffer[outLen] = '\0';
    *c_inout = outLen;
    if (lpDx_in && lpDx_out) {
      *lpDx_out = s_dx_buffer;
    }
    return s_buffer;
  }
  return lpString; // Return original if no changes needed
}

static const wchar_t *FilterWideString(LPCWSTR lpString, UINT *c_inout,
                                       const INT *lpDx_in, INT **lpDx_out,
                                       bool performTranslation = true) {
  if (!lpString || *c_inout == 0)
    return lpString;

  static thread_local wchar_t s_wbuffer[1024];
  static thread_local INT s_wdx_buffer[1024];

  UINT len = *c_inout;
  UINT originalC = *c_inout;
  if (len >= (sizeof(s_wbuffer) / sizeof(s_wbuffer[0])))
    len = (sizeof(s_wbuffer) / sizeof(s_wbuffer[0])) - 1;

  bool modified = false;
  UINT outLen = 0;
  auto IsWideAsciiAlphaNum = [](wchar_t c) -> bool {
    return ((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') ||
            (c >= L'a' && c <= L'z'));
  };
  for (UINT i = 0; i < len; i++) {
    wchar_t wc = lpString[i];
    wchar_t replacement = 0;

    if (IsNonRenderableTextControlW((UINT)wc)) {
      modified = true;
      continue;
    }

    if (performTranslation) {
      if (wc == L'_' || wc == 0x30FB || wc == 0xFF65) {
        bool prevAlphaNum = false;
        bool nextAlphaNum = false;
        if (i > 0) {
          wchar_t p = lpString[i - 1];
          prevAlphaNum = ((p >= L'0' && p <= L'9') || (p >= L'A' && p <= L'Z') ||
                          (p >= L'a' && p <= L'z'));
        }
        if (i + 1 < len) {
          wchar_t n = lpString[i + 1];
          nextAlphaNum = ((n >= L'0' && n <= L'9') || (n >= L'A' && n <= L'Z') ||
                          (n >= L'a' && n <= L'z'));
        }
        if (prevAlphaNum && nextAlphaNum) {
          modified = true;
          continue;
        }
        replacement = L' ';
      } else
      if (wc >= 0xFF01 && wc <= 0xFF5E) {
        replacement = (wchar_t)(wc - 0xFEE0);
      } else if (wc == 0x3000) {
        bool prevAlphaNum = false;
        bool nextAlphaNum = false;
        if (i > 0) {
          wchar_t p = lpString[i - 1];
          prevAlphaNum = IsWideAsciiAlphaNum(p) ||
                         ((p >= 0xFF10 && p <= 0xFF19) ||
                          (p >= 0xFF21 && p <= 0xFF3A) ||
                          (p >= 0xFF41 && p <= 0xFF5A));
        }
        if (i + 1 < len) {
          wchar_t n = lpString[i + 1];
          nextAlphaNum = IsWideAsciiAlphaNum(n) ||
                         ((n >= 0xFF10 && n <= 0xFF19) ||
                          (n >= 0xFF21 && n <= 0xFF3A) ||
                          (n >= 0xFF41 && n <= 0xFF5A));
        }
        if (prevAlphaNum && nextAlphaNum) {
          modified = true;
          continue;
        }
        replacement = L' ';
      } else if (wc == 0x3001 || wc == 0xFF0C) {
        replacement = L',';
      } else if (wc == 0x3002 || wc == 0xFF0E) {
        replacement = L'.';
      } else if (wc == 0xFF1F) {
        replacement = L'?';
      } else if (wc == 0xFF01) {
        replacement = L'!';
      } else if (wc == 0x300C || wc == 0x300D || wc == 0x300E ||
                 wc == 0x300F) {
        replacement = L'"';
      } else if (wc == L'~') {
        replacement = L'_';
      }

    }

    if (replacement != 0) {
      s_wbuffer[outLen] = replacement;
      if (lpDx_in && lpDx_out && i < originalC)
        s_wdx_buffer[outLen] = lpDx_in[i];
      outLen++;
      modified = true;
      continue;
    }

    s_wbuffer[outLen] = wc;
    if (lpDx_in && lpDx_out && i < originalC)
      s_wdx_buffer[outLen] = lpDx_in[i];
    outLen++;
  }

  if (performTranslation && outLen >= 8) {
    int tokenCount = 0;
    int singleTokenCount = 0;
    int currLen = 0;
    for (UINT k = 0; k < outLen; k++) {
      wchar_t ch = s_wbuffer[k];
      if (ch == L' ' || ch == L'\t' || ch == 0x3000) {
        if (currLen > 0) {
          tokenCount++;
          if (currLen == 1)
            singleTokenCount++;
          currLen = 0;
        }
      } else if (IsWideAsciiAlphaNum(ch)) {
        currLen++;
      } else {
        if (currLen > 0) {
          tokenCount++;
          if (currLen == 1)
            singleTokenCount++;
          currLen = 0;
        }
      }
    }
    if (currLen > 0) {
      tokenCount++;
      if (currLen == 1)
        singleTokenCount++;
    }

    bool spacedAnomaly = (tokenCount >= 8) &&
                         (singleTokenCount * 100 >= tokenCount * 75);
    if (spacedAnomaly) {
      UINT write = 0;
      for (UINT k = 0; k < outLen; k++) {
        wchar_t ch = s_wbuffer[k];
        if ((ch == L' ' || ch == 0x3000) && k > 0 && k + 1 < outLen &&
            IsWideAsciiAlphaNum(s_wbuffer[k - 1]) &&
            IsWideAsciiAlphaNum(s_wbuffer[k + 1])) {
          continue;
        }
        s_wbuffer[write] = ch;
        if (lpDx_in && lpDx_out)
          s_wdx_buffer[write] = s_wdx_buffer[k];
        write++;
      }
      if (write < outLen)
        modified = true;
      outLen = write;
    }
  }

  if (modified) {
    s_wbuffer[outLen] = L'\0';
    *c_inout = outLen;
    if (lpDx_in && lpDx_out)
      *lpDx_out = s_wdx_buffer;
    return s_wbuffer;
  }

  return lpString;
}

// Safe 1:1 normalization for full-width ASCII in W-path rendering.
// This function never drops characters; it only remaps each codepoint.
static const wchar_t *NormalizeWideAsciiSafe(LPCWSTR lpString, UINT *c_inout) {
  if (!lpString || !c_inout || *c_inout == 0)
    return lpString;

  static thread_local wchar_t s_norm_buf[4096];
  UINT len = *c_inout;
  if (len >= (sizeof(s_norm_buf) / sizeof(s_norm_buf[0])))
    len = (sizeof(s_norm_buf) / sizeof(s_norm_buf[0])) - 1;

  bool changed = false;
  for (UINT i = 0; i < len; i++) {
    wchar_t wc = lpString[i];
    if (wc >= 0xFF01 && wc <= 0xFF5E) {
      s_norm_buf[i] = (wchar_t)(wc - 0xFEE0);
      changed = true;
    } else if (wc == 0x3000) {
      s_norm_buf[i] = L' ';
      changed = true;
    } else {
      s_norm_buf[i] = wc;
    }
  }
  s_norm_buf[len] = 0;
  *c_inout = len;
  return changed ? s_norm_buf : lpString;
}

static const INT *CompressDxSafe(const INT *lpDx, UINT c, int percent) {
  if (!lpDx || c == 0)
    return lpDx;
  if (percent <= 0)
    percent = 100;
  static thread_local INT s_dx_buf[4096];
  UINT n = c;
  if (n >= (sizeof(s_dx_buf) / sizeof(s_dx_buf[0])))
    n = (sizeof(s_dx_buf) / sizeof(s_dx_buf[0])) - 1;
  for (UINT i = 0; i < n; i++) {
    int v = (lpDx[i] * percent + 50) / 100;
    if (v < 1)
      v = 1;
    s_dx_buf[i] = v;
  }
  return s_dx_buf;
}

static const INT *CompressDxForDialogueA(const INT *lpDx, UINT c, LPCSTR text,
                                         int percent, int spacePercent) {
  if (!lpDx || c == 0)
    return lpDx;
  if (percent <= 0)
    percent = 100;
  if (spacePercent < 0)
    spacePercent = 0;
  static thread_local INT s_dx_buf[4096];
  UINT n = c;
  if (n >= (sizeof(s_dx_buf) / sizeof(s_dx_buf[0])))
    n = (sizeof(s_dx_buf) / sizeof(s_dx_buf[0])) - 1;
  for (UINT i = 0; i < n; ++i) {
    unsigned char ch = text ? (unsigned char)text[i] : 0;
    bool isSpace = (ch == ' ' || ch == '\t');
    int scale = isSpace ? spacePercent : percent;
    int v = (lpDx[i] * scale + 50) / 100;
    if (!isSpace && v < 1)
      v = 1;
    if (isSpace && v < 0)
      v = 0;
    s_dx_buf[i] = v;
  }
  return s_dx_buf;
}

static const INT *CompressDxForDialogueW(const INT *lpDx, UINT c, LPCWSTR text,
                                         int percent, int spacePercent) {
  if (!lpDx || c == 0)
    return lpDx;
  if (percent <= 0)
    percent = 100;
  if (spacePercent < 0)
    spacePercent = 0;
  static thread_local INT s_dx_buf[4096];
  UINT n = c;
  if (n >= (sizeof(s_dx_buf) / sizeof(s_dx_buf[0])))
    n = (sizeof(s_dx_buf) / sizeof(s_dx_buf[0])) - 1;
  for (UINT i = 0; i < n; ++i) {
    wchar_t ch = text ? text[i] : 0;
    bool isSpace = (ch == L' ' || ch == L'\t' || ch == 0x3000);
    int scale = isSpace ? spacePercent : percent;
    int v = (lpDx[i] * scale + 50) / 100;
    if (!isSpace && v < 1)
      v = 1;
    if (isSpace && v < 0)
      v = 0;
    s_dx_buf[i] = v;
  }
  return s_dx_buf;
}

static void CompressCumulativeDxA(LPINT dx, UINT c, LPCSTR text, int percent,
                                  int spacePercent) {
  if (!dx || c == 0)
    return;
  if (percent <= 0)
    percent = 100;
  if (spacePercent < 0)
    spacePercent = 0;
  int prevOrig = 0;
  int out = 0;
  for (UINT i = 0; i < c; ++i) {
    int orig = dx[i];
    int delta = orig - prevOrig;
    prevOrig = orig;
    if (delta < 0)
      delta = 0;
    unsigned char ch = text ? (unsigned char)text[i] : 0;
    bool isSpace = (ch == ' ' || ch == '\t');
    int scaled = (delta * (isSpace ? spacePercent : percent) + 50) / 100;
    if (!isSpace && scaled < 1)
      scaled = 1;
    out += scaled;
    dx[i] = out;
  }
}

static void CompressCumulativeDxW(LPINT dx, UINT c, LPCWSTR text, int percent,
                                  int spacePercent) {
  if (!dx || c == 0)
    return;
  if (percent <= 0)
    percent = 100;
  if (spacePercent < 0)
    spacePercent = 0;
  int prevOrig = 0;
  int out = 0;
  for (UINT i = 0; i < c; ++i) {
    int orig = dx[i];
    int delta = orig - prevOrig;
    prevOrig = orig;
    if (delta < 0)
      delta = 0;
    wchar_t ch = text ? text[i] : 0;
    bool isSpace = (ch == L' ' || ch == L'\t' || ch == 0x3000);
    int scaled = (delta * (isSpace ? spacePercent : percent) + 50) / 100;
    if (!isSpace && scaled < 1)
      scaled = 1;
    out += scaled;
    dx[i] = out;
  }
}

static bool IsUiElement(LPCSTR str, UINT c) {
  if (!str || c == 0)
    return false;

  // Ensure null termination for safe strstr scanning if it's not already
  // null-terminated
  static thread_local char s_ui_buffer[1024];
  UINT len = (c >= sizeof(s_ui_buffer)) ? sizeof(s_ui_buffer) - 1 : c;
  memcpy(s_ui_buffer, str, len);
  s_ui_buffer[len] = '\0';

  const char *ui_elements[] = {"Scene Jump",
                               "Putar Suara",
                               "Kembali",
                               "Aktifkan Thumbnail",
                               "シーンジャンプ",
                               "音声再生",
                               "戻る",
                               "サムネイル有効",
                               "LOG",
                               "Log",
                               "log"};
  for (const auto &elem : ui_elements) {
    if (strstr(s_ui_buffer, elem) != nullptr) {
      return true;
    }
  }
  return false;
}

static inline bool IsSjisLeadByte(unsigned char ch) {
  return ((ch >= 0x81 && ch <= 0x9F) || (ch >= 0xE0 && ch <= 0xFC));
}

static inline bool IsSjisFullWidthLatinPair(unsigned char ch1,
                                            unsigned char ch2) {
  if (ch1 == 0x82) {
    if ((ch2 >= 0x4F && ch2 <= 0x58) || (ch2 >= 0x60 && ch2 <= 0x79) ||
        (ch2 >= 0x81 && ch2 <= 0x9A)) {
      return true; // full-width 0-9 A-Z a-z
    }
  }
  if (ch1 == 0x81) {
    switch (ch2) {
    case 0x40:
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47:
    case 0x48:
    case 0x49:
    case 0x5B:
    case 0x5C:
    case 0x5D:
    case 0x5E:
    case 0x5F:
    case 0x60:
    case 0x61:
    case 0x62:
    case 0x65:
    case 0x66:
    case 0x67:
    case 0x68:
    case 0x69:
    case 0x6A:
    case 0x6D:
    case 0x6E:
    case 0x6F:
    case 0x70:
    case 0x7B:
    case 0x7C:
    case 0x81:
    case 0x83:
    case 0x84:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97:
      return true; // full-width latin punctuation/operators
    default:
      return false;
    }
  }
  return false;
}

static bool IsLikelyDialogueLatinA(LPCSTR str, UINT len) {
  if (!str || len == 0)
    return false;
  int latinCount = 0;
  int neutralCount = 0;
  int nonLatinCount = 0;
  for (UINT i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)str[i];
    if (ch < 0x80) {
      if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= 'a' && ch <= 'z')) {
        latinCount++;
      } else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
                 ch == '.' || ch == ',' || ch == '!' || ch == '?' ||
                 ch == ';' || ch == ':' || ch == '\'' || ch == '\"' ||
                 ch == '-' || ch == '_' || ch == '(' || ch == ')' ||
                 ch == '[' || ch == ']' || ch == '{' || ch == '}') {
        neutralCount++;
      } else {
        nonLatinCount++;
      }
      continue;
    }
    if (IsSjisLeadByte(ch) && i + 1 < len) {
      unsigned char ch2 = (unsigned char)str[i + 1];
      if (IsSjisFullWidthLatinPair(ch, ch2)) {
        latinCount++;
      } else {
        nonLatinCount++;
      }
      i++;
      continue;
    }
    nonLatinCount++;
  }
  if (nonLatinCount != 0)
    return false;
  if (latinCount > 0)
    return true;
  return (len <= 2 && neutralCount > 0);
}

static bool IsLikelyDialogueLatinW(LPCWSTR str, UINT len) {
  if (!str || len == 0)
    return false;
  int latinCount = 0;
  int neutralCount = 0;
  int nonLatinCount = 0;
  for (UINT i = 0; i < len; i++) {
    wchar_t wc = str[i];
    if ((wc >= L'0' && wc <= L'9') || (wc >= L'A' && wc <= L'Z') ||
        (wc >= L'a' && wc <= L'z')) {
      latinCount++;
    } else if ((wc >= 0xFF10 && wc <= 0xFF19) || (wc >= 0xFF21 && wc <= 0xFF3A) ||
               (wc >= 0xFF41 && wc <= 0xFF5A)) {
      latinCount++;
    } else if ((wc >= 0xFF01 && wc <= 0xFF5E) || wc == 0x3000 || wc == L' ' ||
               wc == L'\t' || wc == L'\r' || wc == L'\n') {
      neutralCount++;
    } else {
      nonLatinCount++;
    }
  }
  if (nonLatinCount != 0)
    return false;
  if (latinCount > 0)
    return true;
  return (len <= 2 && neutralCount > 0);
}

static bool IsLikelyDialogueTextboxArea(HDC hdc, int y) {
  if (!hdc)
    return false;
  RECT rc = {};
  int r = GetClipBox(hdc, &rc);
  if (r == ERROR)
    return false;
  int h = rc.bottom - rc.top;
  if (h < 220)
    return false;
  int relY = y - rc.top;
  return relY >= (h * 55) / 100;
}

static bool IsLikelyDialogueMetricsContext(HDC hdc) {
  if (!hdc)
    return false;

  RECT rc = {};
  int r = GetClipBox(hdc, &rc);
  if (r == ERROR)
    return false;

  int w = rc.right - rc.left;
  int h = rc.bottom - rc.top;

  // Typical dialogue textbox clipping is wide, short, and near the lower area.
  if (w >= 520 && h >= 60 && h <= 280 && rc.top >= 280) {
    return true;
  }

  // Fallback when clipbox is not tight: use current pen position heuristics.
  POINT pt = {};
  if (GetCurrentPositionEx(hdc, &pt)) {
    if (IsLikelyDialogueTextboxArea(hdc, pt.y))
      return true;
  }
  return false;
}

static void LogRenderPathTelemetryA(const char *hookName, HDC hdc, int x, int y,
                                    int c, bool likelyByArea,
                                    bool likelyDialogueLatinOrig,
                                    bool useDialogueTuning, bool translated,
                                    const INT *dx, LPCSTR text) {
  if (!hookName)
    return;
  if (c <= 0 || !text)
    return;

  WCHAR exePath[MAX_PATH];
  GetModuleFileNameW(NULL, exePath, MAX_PATH);
  std::wstring exeDir = exePath;
  size_t pos = exeDir.find_last_of(L"\\/");
  if (pos != std::wstring::npos) {
    exeDir = exeDir.substr(0, pos);
  }
  std::wstring logPath = exeDir + L"\\dc2ty_path.log";
  FILE *f = _wfopen(logPath.c_str(), L"a");
  if (!f)
    return;

  RECT rc = {0, 0, 0, 0};
  int clipRes = hdc ? GetClipBox(hdc, &rc) : ERROR;
  TEXTMETRICA tm = {};
  BOOL hasTm = hdc ? GetTextMetricsA(hdc, &tm) : FALSE;
  int charExtra = hdc ? GetTextCharacterExtra(hdc) : 0;
  UINT align = hdc ? GetTextAlign(hdc) : 0;

  fprintf(
      f,
      "[PATH-A][%s] tid=%lu x=%d y=%d c=%d area=%d latin=%d tune=%d trans=%d "
      "charExtra=%d align=0x%X clip=(%ld,%ld,%ld,%ld) clipRes=%d tmH=%d\n",
      hookName, (unsigned long)GetCurrentThreadId(), x, y, c,
      likelyByArea ? 1 : 0, likelyDialogueLatinOrig ? 1 : 0,
      useDialogueTuning ? 1 : 0, translated ? 1 : 0, charExtra, align, rc.left,
      rc.top, rc.right, rc.bottom, clipRes, hasTm ? tm.tmHeight : -1);

  int sample = (c < 40) ? c : 40;
  fprintf(f, "    txt='");
  for (int i = 0; i < sample; ++i) {
    unsigned char ch = (unsigned char)text[i];
    fputc((ch >= 0x20 && ch <= 0x7E) ? ch : '.', f);
  }
  fprintf(f, "'\n");

  fprintf(f, "    hex=");
  for (int i = 0; i < sample; ++i) {
    fprintf(f, "%02X ", (unsigned char)text[i]);
  }
  fprintf(f, "\n");

  if (dx) {
    int d = (c < 12) ? c : 12;
    fprintf(f, "    dx=");
    for (int i = 0; i < d; ++i) {
      fprintf(f, "%d ", dx[i]);
    }
    fprintf(f, "\n");
  }

  fclose(f);
}

static void LogPatchLoad(const char *phase) {
  WCHAR exePath[MAX_PATH];
  GetModuleFileNameW(NULL, exePath, MAX_PATH);
  std::wstring exeDir = exePath;
  size_t pos = exeDir.find_last_of(L"\\/");
  if (pos != std::wstring::npos) {
    exeDir = exeDir.substr(0, pos);
  }
  std::wstring logPath = exeDir + L"\\dc2ty_load.log";
  FILE *f = _wfopen(logPath.c_str(), L"a");
  if (!f)
    return;
  SYSTEMTIME st = {};
  GetLocalTime(&st);
  WCHAR dllPath[MAX_PATH] = {};
  HMODULE selfModule = NULL;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR)&LogPatchLoad, &selfModule);
  if (selfModule) {
    GetModuleFileNameW(selfModule, dllPath, MAX_PATH);
  }
  fprintf(f,
          "[%04d-%02d-%02d %02d:%02d:%02d.%03d] phase=%s pid=%lu tid=%lu\n",
          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
          st.wMilliseconds, phase ? phase : "(null)",
          (unsigned long)GetCurrentProcessId(),
          (unsigned long)GetCurrentThreadId());
  fwprintf(f, L"    exe=%s\n", exePath);
  fwprintf(f, L"    dll=%s\n", dllPath);
  fclose(f);
}

static bool IsReadablePtr(const void *p, size_t minBytes) {
  if (!p)
    return false;
  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
    return false;
  if (mbi.State != MEM_COMMIT)
    return false;
  DWORD protect = mbi.Protect & 0xff;
  if (protect == PAGE_NOACCESS || protect == PAGE_EXECUTE)
    return false;
  const BYTE *b = (const BYTE *)p;
  const BYTE *end = (const BYTE *)mbi.BaseAddress + mbi.RegionSize;
  return (b + minBytes) <= end;
}

static bool IsWritablePtr(void *p, size_t minBytes) {
  if (!p)
    return false;
  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
    return false;
  if (mbi.State != MEM_COMMIT)
    return false;
  DWORD protect = mbi.Protect & 0xff;
  if (protect == PAGE_NOACCESS || protect == PAGE_READONLY ||
      protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ)
    return false;
  BYTE *b = (BYTE *)p;
  BYTE *end = (BYTE *)mbi.BaseAddress + mbi.RegionSize;
  return (b + minBytes) <= end;
}

static bool TryReadAnsiProbe(const char *p, char *out, size_t outCap) {
  if (!p || !out || outCap < 4)
    return false;
  if (!IsReadablePtr(p, 8))
    return false;
  size_t n = 0;
  int printable = 0;
  while (n + 1 < outCap && n < 80) {
    if (!IsReadablePtr(p + n, 1))
      break;
    unsigned char ch = (unsigned char)p[n];
    if (ch == 0)
      break;
    out[n] = (char)ch;
    if ((ch >= 0x20 && ch <= 0x7e) || ch == '\t' || ch == '\n' || ch == '\r')
      printable++;
    n++;
  }
  out[n] = 0;
  if (n < 3)
    return false;
  return (printable * 100 / (int)n) >= 70;
}

static bool TryReadWideProbe(const wchar_t *p, wchar_t *out, size_t outCap) {
  if (!p || !out || outCap < 4)
    return false;
  if (!IsReadablePtr(p, sizeof(wchar_t) * 4))
    return false;
  size_t n = 0;
  int printable = 0;
  while (n + 1 < outCap && n < 80) {
    if (!IsReadablePtr(p + n, sizeof(wchar_t)))
      break;
    wchar_t ch = p[n];
    if (ch == 0)
      break;
    out[n] = ch;
    if ((ch >= 0x20 && ch <= 0x7e) || (ch >= 0x3040 && ch <= 0x30ff) ||
        (ch >= 0x4e00 && ch <= 0x9fff))
      printable++;
    n++;
  }
  out[n] = 0;
  if (n < 3)
    return false;
  return (printable * 100 / (int)n) >= 55;
}

static void LogHsxf4Probe(const char *line) {
  WCHAR exePath[MAX_PATH];
  GetModuleFileNameW(NULL, exePath, MAX_PATH);
  std::wstring exeDir = exePath;
  size_t pos = exeDir.find_last_of(L"\\/");
  if (pos != std::wstring::npos)
    exeDir = exeDir.substr(0, pos);
  std::wstring logPath = exeDir + L"\\dc2ty_hsxf4.log";
  FILE *f = _wfopen(logPath.c_str(), L"a");
  if (!f)
    return;
  fprintf(f, "%s\n", line ? line : "");
  fclose(f);
}

static bool IsLikelySjisLatinPair(BYTE b1, BYTE b2) {
  if (b1 == 0x82 &&
      ((b2 >= 0x4F && b2 <= 0x58) || (b2 >= 0x60 && b2 <= 0x79) ||
       (b2 >= 0x81 && b2 <= 0x9A))) {
    return true;
  }
  return false;
}

static DWORD(__cdecl *Real_Hsxf4_22BB0)(DWORD, DWORD, DWORD, DWORD) = NULL;

static DWORD *GetHsxf4PitchPtr() {
  HMODULE hExe = GetModuleHandleA(NULL);
  if (!hExe)
    return NULL;
  return (DWORD *)((BYTE *)hExe + (0xA66FD4 - 0x400000));
}

static DWORD *GetHsxf4CursorXPtr() {
  HMODULE hExe = GetModuleHandleA(NULL);
  if (!hExe)
    return NULL;
  return (DWORD *)((BYTE *)hExe + (0xCAD388 - 0x400000));
}

static DWORD *GetHsxf4CursorYPtr() {
  HMODULE hExe = GetModuleHandleA(NULL);
  if (!hExe)
    return NULL;
  return (DWORD *)((BYTE *)hExe + (0xCAD384 - 0x400000));
}

static bool IsAsciiDialogueByte(BYTE b) {
  return (b >= 0x20 && b <= 0x7E && b != '@');
}

static bool IsHsxf4TextControlByte(BYTE b) {
  return b < 0x20 || b == 0x7F;
}

static constexpr int kHsxf4LatinLowLine = '.'; // Pause marks render as Latin dots.
static constexpr int kHsxf4GreekSmallMu = 0x03BC;

static int DecodeHsxf4AsciiByte(BYTE b1, BYTE b2) {
  if (IsAsciiDialogueByte(b1))
    return b1;
  // Shift-JIS Greek small mu. Treat it as a Latin Unicode glyph so it uses
  // the translated dialogue/nameplate font instead of the original JP font.
  if (b1 == 0x83 && b2 == 0xCA)
    return kHsxf4GreekSmallMu;
  if (b1 == 0x82) {
    if (b2 >= 0x4F && b2 <= 0x58)
      return '0' + (b2 - 0x4F);
    if (b2 >= 0x60 && b2 <= 0x79)
      return 'A' + (b2 - 0x60);
    if (b2 >= 0x81 && b2 <= 0x9A)
      return 'a' + (b2 - 0x81);
  }
  if (b1 == 0xFA && b2 == 0x56)
    return '\'';
  // ADV32 sometimes expands an ellipsis into 81 44 + 84 9F + 84 9F.
  // Route the continuation through the Latin low-line path; otherwise it keeps
  // the Japanese glyph baseline and can appear in the wrong row.
  if (b1 == 0x84 && b2 == 0x9F)
    return kHsxf4LatinLowLine;
  if (b1 == 0x81) {
    switch (b2) {
    case 0x40:
      return ' ';
    case 0x43:
      return ',';
    case 0x44:
      return '.';
    case 0x46:
      return ':';
    case 0x47:
      return ';';
    case 0x48:
      return '?';
    case 0x49:
      return '!';
    case 0x93:
      return '%';
    case 0x95:
      return '&';
    case 0x7E:
      return 0x00D7; // × multiplication sign - route through Latin sizing.
    case 0x99:
      return 0x2606; // ☆ white star - route through Latin sizing.
    case 0x9B:
      return 0x25CB; // ○ white circle - route through Latin sizing.
    case 0x9F:
      return 0x25C6; // ◆: keep the diamond, but route it through Latin sizing.
    case 0xA0:
      return 0x25C7; // ◇
    case 0xA2:
      return 0x25B3; // △ white up-pointing triangle
    case 0x60:
      return '~';
    case 0x63:
      return kHsxf4LatinLowLine; // Ellipsis rendered as Latin low-line.
    case 0x65:
    case 0x66:
      return '\'';
    case 0x69:
      return '(';
    case 0x6A:
      return ')';
    case 0xF4:
      return 0x266A; // ♪ music note - route through Latin path
    case 0xF5:
      return 0x266B; // ♫ beamed eighth notes
    case 0xF6:
      return 0x266C; // ♬ beamed sixteenth notes
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
      return '"';
    case 0x5B:
    case 0x5D:
    case 0x7C:
      return '-';
    case 0x5C:
      return kHsxf4LatinLowLine; // Long dash / "──" rendered as low-line.
    default:
      break;
    }
  }
  return 0;
}

static inline bool IsHsxf4LatinDiamondSymbol(int ch) {
  return ch == 0x25C6 || ch == 0x25C7;
}

static inline bool IsHsxf4LatinCompactSymbol(int ch) {
  return IsHsxf4LatinDiamondSymbol(ch) || ch == 0x00D7 || ch == 0x2606 ||
         ch == 0x25B3 || ch == 0x25CB;
}

static inline bool IsHsxf4LatinLowLineSymbol(int ch) {
  return ch == 0xFF3F;
}

static inline bool IsHsxf4LatinMusicSymbol(int ch) {
  return ch == 0x266A || ch == 0x266B || ch == 0x266C;
}

static inline bool IsHsxf4LatinGreekSymbol(int ch) {
  return ch == kHsxf4GreekSmallMu || ch == 0x00B5;
}

static inline bool IsHsxf4LatinWideOverrideSymbol(int ch) {
  return IsHsxf4LatinMusicSymbol(ch) || IsHsxf4LatinGreekSymbol(ch);
}

static inline bool IsHsxf4NameplateGlyph(int ch) {
  return (ch >= 0x20 && ch <= 0x7E) || IsHsxf4LatinGreekSymbol(ch);
}

static bool IsHsxf4ChoiceText(const BYTE *p, DWORD len) {
  if (!p || len < 2)
    return false;
  DWORD scan = len;
  if (scan > 96)
    scan = 96;
  for (DWORD i = 0; i + 1 < scan; ++i) {
    if (!IsReadablePtr(p + i, 2))
      break;
    // Shift-JIS ○. Choice labels in MES are "○ <text>" after numeric trigger.
    if (p[i] == 0x81 && p[i + 1] == 0x9B)
      return true;
    // Imported choice labels are stored in the engine's encrypted form:
    // SJIS ○ (81 9B) is persisted as 61 7B. Older imports used ^ as a compact
    // choice-space placeholder; newer imports use encrypted SJIS full-width
    // space (61 20). Treat both markers as choice runs.
    if (p[i] == 0x61 && p[i + 1] == 0x7B)
      return true;
  }
  return false;
}

static bool IsHsxf4EncryptedChoiceText(const BYTE *p, DWORD len) {
  if (!p || len < 3)
    return false;
  DWORD scan = len;
  if (scan > 96)
    scan = 96;
  for (DWORD i = 0; i + 2 < scan; ++i) {
    if (!IsReadablePtr(p + i, 3))
      break;
    if (p[i] == 0x61 && p[i + 1] == 0x7B)
      return true;
  }
  return false;
}

static bool IsHsxf4PlainLatinChoiceCandidate(const BYTE *p, DWORD len) {
  if (!p || len < 4)
    return false;

  DWORD scan = len;
  if (scan > 180)
    scan = 180;

  int alpha = 0;
  int spaces = 0;
  int punctuation = 0;
  int decodedCount = 0;
  bool startsQuoted = false;
  bool hasControl = false;

  for (DWORD i = 0; i < scan;) {
    if (!IsReadablePtr(p + i, 1))
      break;
    BYTE b1 = p[i];
    if (b1 == 0)
      break;

    BYTE b2 = 0;
    bool hasPair = i + 1 < scan && IsReadablePtr(p + i + 1, 1);
    if (hasPair)
      b2 = p[i + 1];

    int ch = DecodeHsxf4AsciiByte(b1, b2);
    bool sjisLead = ((b1 >= 0x81 && b1 <= 0x9F) ||
                     (b1 >= 0xE0 && b1 <= 0xFC));
    DWORD step = (sjisLead && hasPair) ? 2 : 1;
    if (decodedCount == 0 && (ch == '"' || ch == '\'' || ch == 0x300C ||
                              b1 == 0x81 && (b2 == 0x75 || b2 == 0x76)))
      startsQuoted = true;
    if (b1 < 0x20 && b1 != '\t')
      hasControl = true;
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
      ++alpha;
    else if (ch == ' ')
      ++spaces;
    else if (ch == '.' || ch == ',' || ch == '?' || ch == '!' || ch == '-')
      ++punctuation;

    ++decodedCount;
    i += step;
  }

  if (startsQuoted || hasControl)
    return false;
  if (decodedCount < 4 || alpha < 4)
    return false;
  if (spaces >= 1)
    return true;

  return false;
}

static int DecodeHsxf4EncryptedChoiceByte(BYTE b1, BYTE b2) {
  if (b1 == 0x61 && b2 == 0x7B)
    return 'o';
  if (b1 == 0x61 && b2 == 0x20)
    return ' ';
  if (b1 == 0x5E)
    return ' ';
  if (b1 >= 0x01 && b1 <= 0x5E)
    return b1 + 0x20;
  return DecodeHsxf4AsciiByte(b1, b2);
}

static bool IsHsxf4PopupPhraseText(const char *text) {
  if (!text || !*text)
    return false;

  std::string s(text);
  return s.find("Ingin keluar") != std::string::npos ||
         s.find("Ingin ke") != std::string::npos ||
         s.find("Muat Permainan") != std::string::npos ||
         s.find("Muat Per") != std::string::npos ||
         s.find("Muat data") != std::string::npos ||
         s.find("Simpan permainan") != std::string::npos ||
         s.find("Mau keluar") != std::string::npos ||
         s.find("Simpan Disini") != std::string::npos ||
         s.find("Simpan di sini") != std::string::npos ||
         s.find("Simpan Di sini") != std::string::npos ||
         s.find("Simpan D") != std::string::npos ||
         s.find("Mau kembali") != std::string::npos ||
         s.find("Mau kemb") != std::string::npos ||
         s.find("Reset Config") != std::string::npos ||
         s.find("Reset Co") != std::string::npos;
}

static bool BuildHsxf4MixedAsciiText(const BYTE *p, DWORD len,
                                     char *out, size_t outSize) {
  if (!p || !out || outSize == 0)
    return false;

  out[0] = '\0';
  DWORD scan = len;
  if (scan > 180)
    scan = 180;

  size_t count = 0;
  for (DWORD i = 0; i < scan && count + 1 < outSize;) {
    if (!IsReadablePtr(p + i, 1))
      break;

    BYTE b1 = p[i];
    if (b1 == 0)
      break;
    if (b1 == '\t') {
      out[count++] = ' ';
      ++i;
      continue;
    }
    if (IsAsciiDialogueByte(b1)) {
      out[count++] = (char)b1;
      ++i;
      continue;
    }

    if (((b1 >= 0x81 && b1 <= 0x9F) || (b1 >= 0xE0 && b1 <= 0xFC)) &&
        i + 1 < scan && IsReadablePtr(p + i, 2)) {
      int ch = DecodeHsxf4AsciiByte(p[i], p[i + 1]);
      if (ch >= 0x20 && ch <= 0x7E) {
        out[count++] = (char)ch;
        i += 2;
        continue;
      }
    }

    if (count > 0)
      out[count++] = ' ';
    ++i;
  }

  out[count] = '\0';
  return count > 0;
}

static DWORD GetHsxf4VisibleScanLen(const BYTE *p, DWORD len, DWORD maxLen) {
  if (!p || len == 0)
    return 0;
  DWORD scan = len < maxLen ? len : maxLen;
  for (DWORD i = 0; i < scan; ++i) {
    if (!IsReadablePtr(p + i, 1))
      return i;
    if (p[i] == 0)
      return i;
  }
  return scan;
}

static bool IsHsxf4PopupConfirmText(const BYTE *p, DWORD len) {
  if (!p || len < 4)
    return false;

  char mixed[128] = {};
  if (BuildHsxf4MixedAsciiText(p, len, mixed, sizeof(mixed)) &&
      IsHsxf4PopupPhraseText(mixed))
    return true;

  DWORD scan = GetHsxf4VisibleScanLen(p, len, 180);
  if (scan < 4)
    return false;

  for (DWORD start = 0; start + 1 < scan; ++start) {
    char buf[96] = {};
    size_t count = 0;
    for (DWORD i = start; i + 1 < scan && count + 1 < sizeof(buf); i += 2) {
      if (!IsReadablePtr(p + i, 2))
        break;
      if (p[i] == 0 || p[i + 1] == 0)
        break;
      int ch = DecodeHsxf4AsciiByte(p[i], p[i + 1]);
      if (p[i] == '\t')
        ch = ' ';
      if (ch < 0x20 || ch > 0x7E)
        break;
      buf[count++] = (char)ch;
      if (ch == '?' && count >= 4)
        break;
    }
    if (count >= 4 && IsHsxf4PopupPhraseText(buf))
      return true;
  }

  char raw[96] = {};
  size_t rawCount = 0;
  for (DWORD i = 0; i < scan && rawCount + 1 < sizeof(raw); ++i) {
    if (!IsReadablePtr(p + i, 1))
      break;
    BYTE b = p[i];
    if (b == 0)
      break;
    if (b == '\t') {
      raw[rawCount++] = ' ';
      continue;
    }
    if (b >= 0x20 && b <= 0x7E) {
      raw[rawCount++] = (char)b;
      continue;
    }
    if (rawCount >= 4 && IsHsxf4PopupPhraseText(raw))
      return true;
    rawCount = 0;
    raw[0] = '\0';
  }
  return rawCount >= 4 && IsHsxf4PopupPhraseText(raw);
}

struct Hsxf4TextFlags {
  bool choiceMarker;
  bool popupConfirm;
  bool encryptedChoice;
};

static DWORD ComputeHsxf4TextSignature(const BYTE *p, DWORD len) {
  if (!p || len == 0)
    return 0;
  DWORD scan = len < 32 ? len : 32;
  if (!IsReadablePtr(p, scan))
    return 0;

  DWORD hash = 2166136261u;
  for (DWORD i = 0; i < scan; ++i) {
    hash ^= p[i];
    hash *= 16777619u;
  }
  return hash;
}

static Hsxf4TextFlags GetCachedHsxf4TextFlags(const BYTE *p, DWORD textPtr,
                                              DWORD len) {
  static thread_local DWORD s_textPtr = 0;
  static thread_local DWORD s_len = 0;
  static thread_local DWORD s_sig = 0;
  static thread_local DWORD s_tick = 0;
  static thread_local Hsxf4TextFlags s_flags = {};

  DWORD now = GetTickCount();
  DWORD sig = ComputeHsxf4TextSignature(p, len);
  if (textPtr == s_textPtr && len == s_len && sig == s_sig &&
      s_tick != 0 && now - s_tick < 1200)
    return s_flags;

  s_textPtr = textPtr;
  s_len = len;
  s_sig = sig;
  s_tick = now;
  s_flags.encryptedChoice = IsHsxf4EncryptedChoiceText(p, len);
  s_flags.choiceMarker = s_flags.encryptedChoice || IsHsxf4ChoiceText(p, len);
  s_flags.popupConfirm =
      !s_flags.choiceMarker && IsHsxf4PopupConfirmText(p, len);
  return s_flags;
}

static bool TrackHsxf4ChoiceRun(DWORD textPtr, DWORD offset, int decodedCh,
                                bool markerFound, DWORD cursorY) {
  (void)decodedCh;
  static thread_local DWORD s_choicePtr = 0;
  static thread_local DWORD s_choiceStartTick = 0;

  DWORD now = GetTickCount();
  if (textPtr == 0 && offset == 0 && !markerFound) {
    s_choicePtr = 0;
    s_choiceStartTick = 0;
    return false;
  }

  bool inChoiceBand = (cursorY >= 110 && cursorY <= 360);
  if (markerFound) {
    s_choicePtr = textPtr;
    s_choiceStartTick = now;
    return true;
  }

  if (offset == 0 && !markerFound) {
    if (inChoiceBand) {
      s_choicePtr = textPtr;
      s_choiceStartTick = now;
      return true;
    }
    s_choicePtr = 0;
    s_choiceStartTick = 0;
    return false;
  }

  if (inChoiceBand && s_choicePtr == textPtr && s_choiceStartTick != 0 &&
      (now - s_choiceStartTick) < 2500) {
    return true;
  }

  return false;
}

static bool TrackHsxf4PopupRun(DWORD textPtr, DWORD offset, bool popupFound,
                               DWORD cursorY) {
  static thread_local DWORD s_popupPtr = 0;
  static thread_local DWORD s_popupStartTick = 0;

  DWORD now = GetTickCount();
  bool inPopupBand = (cursorY >= 210 && cursorY <= 380);
  if (popupFound) {
    s_popupPtr = textPtr;
    s_popupStartTick = now;
    return true;
  }

  if (offset == 0 && !popupFound && s_popupPtr == textPtr) {
    s_popupPtr = 0;
    s_popupStartTick = 0;
    return false;
  }

  if (inPopupBand && s_popupPtr == textPtr && s_popupStartTick != 0 &&
      (now - s_popupStartTick) < 2500) {
    return true;
  }

  return false;
}

static int GetHsxf4DialoguePitch(int ch) {
  if (ch <= 0)
    return 0;
  if (IsHsxf4LatinCompactSymbol(ch))
    return 20;
  if (ch == 0x2026 || IsHsxf4LatinLowLineSymbol(ch))
    return 16;
  if (ch == ' ')
    return 8;
  if (ch == '.' || ch == ',' || ch == ':' || ch == ';' || ch == '!' ||
      ch == '?' || ch == '\'' || ch == '"' || ch == '-' || ch == '~' ||
      ch == '_' || ch == '(' || ch == ')' || ch == '%' || ch == '&')
    return 14;
  // Full-width SJIS Latin glyphs need the original ADV32 cell width. Smaller
  // values make the engine clip the glyph bitmap, producing half-rendered text.
  return 28;
}

static int GetHsxf4HalfWidthDialoguePitch(int ch) {
  if (ch <= 0)
    return 0;
  if (IsHsxf4LatinCompactSymbol(ch))
    return 20;
  if (ch == 0x2026 || IsHsxf4LatinLowLineSymbol(ch))
    return 16;
  if (ch == ' ')
    return 8;
  if (ch == '-')
    return 12;
  if (ch == '?' || ch == '!' || ch == '~' || ch == '_' || ch == '%' || ch == '&')
    return 16;
  if (ch == 0x266A || ch == 0x266B || ch == 0x266C)
    return 16; // music notes: same pitch as other special chars
  if (ch == '.' || ch == ',' || ch == ':' || ch == ';')
    return 12;
  if (ch == '(' || ch == ')')
    return 14;
  if (ch == '\'')
    return 8;
  if (ch == '"')
    return 10;
  // HSXF4 uses this global both as advance and as glyph cell size. Values near
  // 13 look compact but clip lowercase descenders; keep enough vertical cell.
  return 20;
}

static int GetHsxf4HalfWidthDialogueAdvance(int ch) {
  if (ch <= 0)
    return 0;
  if (IsHsxf4LatinCompactSymbol(ch))
    return 12;
  if (ch == 0x2026 || IsHsxf4LatinLowLineSymbol(ch))
    return 12;
  if (ch == ' ')
    return 5;
  if (ch == '-')
    return 9;
  if (ch == '~' || ch == '_')
    return 14;
  if (ch == '?' || ch == '!' || ch == '%' || ch == '&')
    return 8;
  if (ch == '.' || ch == ',' || ch == ':' || ch == ';')
    return 6;
  if (ch == '(' || ch == ')')
    return 7;
  if (ch == '\'')
    return 3;
  if (ch == '"')
    return 6;
  // Keep most Latin letters nearly uniform. The previous aggressive
  // proportional table made words look uneven; only narrow letters get a
  // small reduction so "i/l" no longer feel too loose.
  if (ch == 'i' || ch == 'l' || ch == 'I')
    return 8;
  if (ch == 'j' || ch == 't' || ch == 'f' || ch == 'r')
    return 9;
  if (ch == 'm' || ch == 'w' || ch == 'M' || ch == 'W')
    return 11;
  return 10;
}

static int GetHsxf4ReplayPitch(int ch) {
  if (ch <= 0)
    return 0;
  if (ch == ' ')
    return 6;
  if (ch == '.' || ch == ',' || ch == ':' || ch == ';' || ch == '\'' ||
      ch == '"' || ch == '-' || ch == '(' || ch == ')')
    return 8;
  if (ch == '?' || ch == '!')
    return 9;
  return 12;
}

static int GetHsxf4ReplayAdvance(int ch) {
  if (ch <= 0)
    return 0;
  if (ch == ' ')
    return 3;
  if (ch == '.' || ch == ',' || ch == ':' || ch == ';' || ch == '\'' ||
      ch == '"' || ch == '-' || ch == '(' || ch == ')')
    return 4;
  if (ch == 'i' || ch == 'l' || ch == 'I')
    return 5;
  if (ch == 'm' || ch == 'w' || ch == 'M' || ch == 'W')
    return 8;
  return 6;
}

static int GetHsxf4HalfWidthChoicePitch(int ch) {
  if (ch <= 0)
    return 0;
  if (IsHsxf4LatinCompactSymbol(ch))
    return 10;
  if (ch == ' ' || ch == '~')
    return 1;
  if (ch == '-' || ch == '?' || ch == '!')
    return 7;
  if (ch == '.' || ch == ',' || ch == ':' || ch == ';' || ch == '\'' ||
      ch == '"' || ch == '(' || ch == ')')
    return 5;
  if (ch == 'i' || ch == 'l' || ch == 'I')
    return 6;
  if (ch == 'j' || ch == 't' || ch == 'f' || ch == 'r')
    return 7;
  if (ch == 'm' || ch == 'w' || ch == 'M' || ch == 'W')
    return 10;
  return 8;
}

static int GetHsxf4HalfWidthChoiceAdvance(int ch) {
  if (ch <= 0)
    return 0;
  if (IsHsxf4LatinCompactSymbol(ch))
    return 5;
  if (ch == ' ' || ch == '~')
    return 1;
  if (ch == '-' || ch == '?' || ch == '!')
    return 3;
  if (ch == '.' || ch == ',' || ch == ':' || ch == ';' || ch == '\'' ||
      ch == '"' || ch == '(' || ch == ')')
    return 2;
  if (ch == 'i' || ch == 'l' || ch == 'I')
    return 2;
  if (ch == 'j' || ch == 't' || ch == 'f' || ch == 'r')
    return 3;
  if (ch == 'm' || ch == 'w' || ch == 'M' || ch == 'W')
    return 5;
  return 4;
}

static int GetHsxf4HalfWidthPopupPitch(int ch) {
  if (ch <= 0)
    return 0;
  if (IsHsxf4LatinCompactSymbol(ch))
    return 14;
  if (ch == ' ')
    return 7;
  if (ch == '?' || ch == '!' || ch == '~')
    return 12;
  if (ch == '-' || ch == '.' || ch == ',' || ch == ':' || ch == ';' ||
      ch == '\'' || ch == '"' || ch == '(' || ch == ')')
    return 11;
  if (ch == 'i' || ch == 'l' || ch == 'I')
    return 11;
  if (ch == 'm' || ch == 'w' || ch == 'M' || ch == 'W')
    return 17;
  return 15;
}

static int GetHsxf4HalfWidthPopupAdvance(int ch) {
  if (ch <= 0)
    return 0;
  if (IsHsxf4LatinCompactSymbol(ch))
    return 6;
  if (ch == ' ')
    return 3;
  if (ch == '?' || ch == '!' || ch == '~')
    return 5;
  if (ch == '-' || ch == '.' || ch == ',' || ch == ':' || ch == ';' ||
      ch == '\'' || ch == '"' || ch == '(' || ch == ')')
    return 4;
  if (ch == 'i' || ch == 'l' || ch == 'I')
    return 5;
  if (ch == 'j' || ch == 't' || ch == 'f' || ch == 'r')
    return 6;
  if (ch == 'm' || ch == 'w' || ch == 'M' || ch == 'W')
    return 10;
  return 8;
}

static bool GetHsxf4TightLineY(DWORD a1, DWORD a2, DWORD originalY,
                               DWORD *adjustedY) {
  if (!adjustedY)
    return false;

  static thread_local DWORD s_textPtr = 0;
  static thread_local DWORD s_baseY = 0;
  static thread_local DWORD s_lastOriginalY = 0;
  static thread_local int s_lineIndex = 0;
  static thread_local DWORD s_lastTick = 0;

  const DWORD now = GetTickCount();
  const int kLineStep = 30;

  bool reset = false;
  if (s_textPtr != a1 || s_baseY == 0)
    reset = true;
  if (s_lastTick != 0 && (now - s_lastTick) > 900)
    reset = true;
  if (a2 == 0 && originalY <= s_baseY + 2)
    reset = true;
  if (originalY + 8 < s_lastOriginalY)
    reset = true;
  if (s_lineIndex >= 5)
    reset = true;

  if (reset) {
    s_textPtr = a1;
    s_baseY = originalY;
    s_lastOriginalY = originalY;
    s_lineIndex = 0;
    s_lastTick = now;
  } else {
    if (originalY > s_lastOriginalY + 8) {
      ++s_lineIndex;
      s_lastOriginalY = originalY;
    }
    s_lastTick = now;
  }

  if (s_lineIndex <= 0)
    return false;

  *adjustedY = s_baseY + (DWORD)(s_lineIndex * kLineStep);
  return *adjustedY != originalY;
}

static bool IsHsxf4TightPunctuation(int ch) {
  return ch == '.' || ch == ',' || ch == ':' || ch == ';' || ch == '!' ||
         ch == '?' || ch == '\'' || ch == '"' || ch == '-' || ch == '(' ||
         ch == ')';
}

static bool IsHsxf4QuoteSpacing(int ch) {
  return ch == '\'' || ch == '"';
}

static int CountHsxf4DecodedAsciiRun(BYTE *p, DWORD maxBytes) {
  if (!p || maxBytes == 0)
    return 0;
  int count = 0;
  for (DWORD i = 0; i < maxBytes && i < 80;) {
    if (!IsReadablePtr(p + i, 1))
      break;
    BYTE b1 = p[i];
    if (b1 == 0)
      break;
    if (IsAsciiDialogueByte(b1)) {
      ++count;
      ++i;
      continue;
    }
    if (i + 1 >= maxBytes || !IsReadablePtr(p + i, 2))
      break;
    int ch = DecodeHsxf4AsciiByte(b1, p[i + 1]);
    if ((ch < 0x20 || ch > 0x7E) && !IsHsxf4LatinGreekSymbol(ch))
      break;
    ++count;
    i += 2;
  }
  return count;
}

static bool GetHsxf4ReplayBreakLayout(BYTE *p, DWORD len, DWORD offset,
                                      DWORD *lineOffset,
                                      DWORD *firstLineAdvance,
                                      DWORD *lineAdvance) {
  if (!p || !lineOffset || !firstLineAdvance || !lineAdvance ||
      len == 0 || len > 256 ||
      offset >= len)
    return false;

  DWORD breakOffset = 0xFFFFFFFF;
  for (DWORD i = 0; i < len && i < 128; ++i) {
    if (!IsReadablePtr(p + i, 1))
      return false;
    if (p[i] == 0)
      break;
    if (p[i] == '\n') {
      breakOffset = i;
      break;
    }
  }

  if (breakOffset == 0xFFFFFFFF || offset <= breakOffset)
    return false;

  DWORD firstAdvance = 0;
  for (DWORD i = 0; i < breakOffset && i < len; ++i) {
    if (!IsReadablePtr(p + i, 1))
      break;
    BYTE b = p[i];
    if (b == 0)
      break;
    int ch = DecodeHsxf4AsciiByte(b, 0);
    if (ch <= 0)
      continue;
    firstAdvance += (DWORD)GetHsxf4HalfWidthChoiceAdvance(ch);
  }

  DWORD advance = 0;
  for (DWORD i = breakOffset + 1; i < offset && i < len; ++i) {
    if (!IsReadablePtr(p + i, 1))
      break;
    BYTE b = p[i];
    if (b == 0 || b == '\n')
      break;
    int ch = DecodeHsxf4AsciiByte(b, 0);
    if (ch <= 0)
      continue;
    advance += (DWORD)GetHsxf4HalfWidthChoiceAdvance(ch);
  }

  *lineOffset = breakOffset;
  *firstLineAdvance = firstAdvance;
  *lineAdvance = advance;
  return true;
}

static DWORD __cdecl Hook_Hsxf4_22BB0(DWORD a1, DWORD a2, DWORD a3, DWORD a4) {
  static volatile LONG s_count = 0;
  LONG n = InterlockedIncrement(&s_count);

  bool tuned = false;
  BYTE b1 = 0;
  BYTE b2 = 0;
  DWORD oldPitch = 0;
  DWORD newPitch = 0;
  DWORD newAdvance = 0;
  DWORD nameVisualShift = 0;
  DWORD *pitch = NULL;
  DWORD *cursorX = NULL;
  DWORD *cursorY = NULL;
  DWORD nameStartShift = 0;
  DWORD oldY = 0;
  DWORD newY = 0;
  bool tunedY = false;
  int decodedCh = 0;
  int prevCh = 0;
  int nextCh = 0;
  BYTE *p = NULL;
  bool encryptedChoice = false;
  bool renderHalfWidth = false;
  bool tuneAsciiDialogueAdvance = false;
  bool renderAsciiChoice = false;
  bool renderPopupText = false;
  bool renderPopupSpacePlaceholder = false;
  bool renderHsxf4SpacePlaceholder = false;
  bool renderReplayTranslatedGlyph = false;
  bool renderLatinWideGlyph = false;
  bool renderAsciiParenGlyph = false;
  bool choiceRender = false;
  bool popupRender = false;
  bool nameplateRender = false;
  bool nameplateLongRender = false;
  bool ellipsisDotRender = false;
  bool extraPauseDot = false;
  bool suppressPauseDash = false;
  bool choiceMarkerSeparatorSpace = false;
  bool plainChoiceCandidate = false;
  bool triplePauseGlyph = false;
  bool replayRender = false;
  bool replaySecondLine = false;
  DWORD replayLineAdvance = 0;
  DWORD replayFirstLineAdvance = 0;
  DWORD replayLineBreakOffset = 0;
  BYTE savedB1 = 0;
  BYTE savedB2 = 0;
  if (g_fontManager.GetLanguage() != 0 && a1 != 0 && a2 < 0x2000) {
    p = (BYTE *)a1;
    if (IsReadablePtr(p + a2, 2)) {
      Hsxf4TextFlags textFlags = GetCachedHsxf4TextFlags(p, a1, a3);
      bool markerFound = textFlags.choiceMarker;
      encryptedChoice = textFlags.encryptedChoice;
      bool popupFound = textFlags.popupConfirm;
      popupRender = popupFound;
      b1 = p[a2];
      b2 = p[a2 + 1];
      decodedCh = encryptedChoice ? DecodeHsxf4EncryptedChoiceByte(b1, b2)
                                  : DecodeHsxf4AsciiByte(b1, b2);
      if (!encryptedChoice && a3 > 0 && a3 < 512)
        replayRender = IsReplayTranslatedValueA(p, a3);
      if (!encryptedChoice && a3 > 0 && a3 < 512) {
        UINT replayLen = 0;
        DWORD keyLen = 0;
        while (keyLen < a3 && keyLen < 256 &&
               IsReadablePtr(p + keyLen, 1) && p[keyLen] != 0) {
          ++keyLen;
        }
        if (keyLen > 0) {
          if (const char *replayText =
                  TranslateReplayTextA((LPCSTR)p, keyLen, &replayLen)) {
            renderReplayTranslatedGlyph = IsWritablePtr(p + a2, 2);
            if (a2 == 0) {
              DWORD *replayCursorX = GetHsxf4CursorXPtr();
              DWORD *replayCursorY = GetHsxf4CursorYPtr();
              if (replayCursorX && replayCursorY &&
                  IsReadablePtr(replayCursorX, sizeof(DWORD)) &&
                  IsReadablePtr(replayCursorY, sizeof(DWORD))) {
                std::string sjisReplay(replayText, replayText + replayLen);
                std::wstring wideReplay;
                if (SjisToWideString(sjisReplay, wideReplay)) {
                  NormalizeReplayTextW(wideReplay);
                  RecordReplayRuntimeOverlay((int)*replayCursorX,
                                             (int)*replayCursorY,
                                             GetReplayTextWidthHint((int)*replayCursorX),
                                             wideReplay);
                }
              }
            }
          }
        }
      }
      if ((b1 == 0x61 && b2 == 0x20) ||
          (b1 == 0x81 && b2 == 0x40)) {
        static volatile LONG s_spaceProbeCount = 0;
        LONG spn = InterlockedIncrement(&s_spaceProbeCount);
        if (spn <= 120) {
          char line[384];
          sprintf_s(line,
                    "[CHOICE-SPACE-BYTE] #%ld a1=%08X a2=%08X a3=%08X "
                    "b=%02X,%02X decoded='%c' enc=%d flagsChoice=%d",
                    (long)spn, a1, a2, a3, (unsigned)b1, (unsigned)b2,
                    (decodedCh >= 0x20 && decodedCh <= 0x7E) ? decodedCh : '.',
                    encryptedChoice ? 1 : 0,
                    textFlags.choiceMarker ? 1 : 0);
          LogHsxf4Probe(line);
        }
      }
      if (popupRender && b1 == '\t')
        decodedCh = ' ';
      if (decodedCh == '^')
        decodedCh = ' ';
      if (IsHsxf4TextControlByte(b1) && !encryptedChoice &&
          !(popupRender && b1 == '\t')) {
        decodedCh = ' ';
        suppressPauseDash = true;
      }
      bool rawPrevDash = (a2 >= 2 && IsReadablePtr(p + a2 - 2, 2) &&
                          p[a2 - 2] == 0x81 && p[a2 - 1] == 0x5C);
      bool rawNextDash = (IsReadablePtr(p + a2 + 2, 2) &&
                          p[a2 + 2] == 0x81 && p[a2 + 3] == 0x5C);
      bool rawPrevPeriod = (a2 >= 2 && IsReadablePtr(p + a2 - 2, 2) &&
                            p[a2 - 2] == 0x81 && p[a2 - 1] == 0x44);
      bool rawNextPeriod = (IsReadablePtr(p + a2 + 2, 2) &&
                            p[a2 + 2] == 0x81 && p[a2 + 3] == 0x44);
      bool rawPrevPrevPeriod =
          (a2 >= 4 && IsReadablePtr(p + a2 - 4, 2) && p[a2 - 4] == 0x81 &&
           p[a2 - 3] == 0x44);
      bool rawNextNextPeriod =
          (IsReadablePtr(p + a2 + 4, 2) &&
           p[a2 + 4] == 0x81 && p[a2 + 5] == 0x44);
      bool rawNextContinuationDot =
          (IsReadablePtr(p + a2 + 2, 2) &&
           p[a2 + 2] == 0x84 && p[a2 + 3] == 0x9F);
      bool rawPrevContinuationDot =
          (a2 >= 2 && IsReadablePtr(p + a2 - 2, 2) &&
           p[a2 - 2] == 0x84 && p[a2 - 1] == 0x9F);
      if (b1 == 0x81 && b2 == 0x5C && rawNextDash && !rawPrevDash) {
        decodedCh = ' ';
        suppressPauseDash = true;
      } else if (b1 == 0x81 && b2 == 0x5C && rawPrevDash) {
        decodedCh = '.';
        triplePauseGlyph = true;
      } else if (b1 == 0x81 && b2 == 0x44 && rawNextPeriod &&
                 !rawPrevPeriod && !rawNextNextPeriod) {
        // Some imported "──" lines arrive as exactly two Japanese period
        // glyphs. Treat that pair as one Latin ellipsis: hide the first dot
        // and render the second as "...".
        decodedCh = ' ';
        suppressPauseDash = true;
      } else if (b1 == 0x81 && b2 == 0x44 && rawPrevPeriod &&
                 !rawNextPeriod && !rawPrevPrevPeriod) {
        decodedCh = '.';
        triplePauseGlyph = true;
      } else if (b1 == 0x81 && b2 == 0x44 && rawNextContinuationDot &&
                 !rawPrevPeriod && !rawPrevContinuationDot) {
        // Runtime often expands two-dot pauses as 81 44 + 84 9F. Draw the
        // visible first glyph as a full Latin ellipsis, then suppress the
        // continuation dot.
        decodedCh = '.';
        triplePauseGlyph = true;
      } else if (b1 == 0x84 && b2 == 0x9F && rawNextContinuationDot &&
                 !rawPrevPeriod && !rawPrevContinuationDot) {
        // Some engine-side pauses reach HSXF4 only as continuation dots
        // (84 9F + 84 9F). Render the first as a full Latin "...".
        decodedCh = '.';
        triplePauseGlyph = true;
      } else if (b1 == 0x84 && b2 == 0x9F &&
                 (rawPrevPeriod || rawPrevContinuationDot)) {
        decodedCh = ' ';
        suppressPauseDash = true;
      }
      DWORD trackY = 0;
      DWORD *trackCursorY = GetHsxf4CursorYPtr();
      if (trackCursorY && IsReadablePtr(trackCursorY, sizeof(DWORD)))
        trackY = *trackCursorY;
      plainChoiceCandidate = IsHsxf4PlainLatinChoiceCandidate(p, a3);
      popupRender = TrackHsxf4PopupRun(a1, a2, popupFound, trackY);
      if (popupRender) {
        TrackHsxf4ChoiceRun(0, 0, 0, false, 0);
        g_choiceGlyphRunHdc = NULL;
        g_choiceGlyphRunBudget = 0;
        g_choiceGlyphPendingLead = 0;
        g_choiceGlyphPendingLeadHdc = NULL;
        choiceRender = false;
        markerFound = false;
      } else {
        choiceRender =
            TrackHsxf4ChoiceRun(a1, a2, decodedCh, markerFound, trackY);
        if (!replayRender && !choiceRender && plainChoiceCandidate &&
            trackY >= 80 && trackY <= 390)
          choiceRender = true;
      }
      // if (choiceRender && decodedCh == '~')
      //  decodedCh = ' ';
      if (decodedCh != 0 && a2 >= 2 && IsReadablePtr(p + a2 - 2, 2))
        prevCh = encryptedChoice ? DecodeHsxf4EncryptedChoiceByte(p[a2 - 2], p[a2 - 1])
                                 : DecodeHsxf4AsciiByte(p[a2 - 2], p[a2 - 1]);
      if (decodedCh != 0 && a2 + 3 < a3 && IsReadablePtr(p + a2 + 2, 2))
        nextCh = encryptedChoice ? DecodeHsxf4EncryptedChoiceByte(p[a2 + 2], p[a2 + 3])
                                 : DecodeHsxf4AsciiByte(p[a2 + 2], p[a2 + 3]);
      // if (choiceRender && prevCh == '~')
      //  prevCh = ' ';
      // if (choiceRender && nextCh == '~')
      //  nextCh = ' ';
      choiceMarkerSeparatorSpace =
          choiceRender && decodedCh == ' ' &&
          ((encryptedChoice && a2 >= 2 && IsReadablePtr(p + a2 - 2, 2) &&
            p[a2 - 2] == 0x61 && p[a2 - 1] == 0x7B) ||
           (!encryptedChoice && a2 >= 2 && IsReadablePtr(p + a2 - 2, 2) &&
            p[a2 - 2] == 0x81 && p[a2 - 1] == 0x9B));
      ellipsisDotRender =
          (decodedCh == 0x2026 || IsHsxf4LatinLowLineSymbol(decodedCh)) ||
          (b1 == 0x81 && (b2 == 0x63 || b2 == 0x5C)) ||
          (b1 == 0x84 && b2 == 0x9F) ||
          (decodedCh == '.' &&
           (prevCh == '.' || nextCh == '.' ||
            IsHsxf4LatinLowLineSymbol(prevCh) ||
            IsHsxf4LatinLowLineSymbol(nextCh)));
      extraPauseDot =
          (ellipsisDotRender && b1 == 0x81 && b2 == 0x5C && p && a2 >= 2 &&
           IsReadablePtr(p + a2 - 2, 2) && p[a2 - 2] == 0x81 &&
           p[a2 - 1] == 0x5C);
      if (suppressPauseDash)
        ellipsisDotRender = false;
      if (triplePauseGlyph) {
        ellipsisDotRender = true;
        extraPauseDot = false;
      }
      if (decodedCh != 0) {
        cursorY = GetHsxf4CursorYPtr();
        if (cursorY && IsWritablePtr(cursorY, sizeof(DWORD))) {
          oldY = *cursorY;
          nameplateRender =
              !popupRender && a4 == 0 && oldY >= 360 && oldY <= 430 &&
              IsHsxf4NameplateGlyph(decodedCh);
          if (nameplateRender) {
            choiceRender = false;
            replayRender = false;
            renderReplayTranslatedGlyph = false;
          }
          if (!popupRender && !replayRender && !nameplateRender &&
              !choiceRender && plainChoiceCandidate && oldY >= 80 &&
              oldY <= 390)
            choiceRender = true;
          if (plainChoiceCandidate) {
            static volatile LONG s_plainChoiceProbeCount = 0;
            LONG cpn = InterlockedIncrement(&s_plainChoiceProbeCount);
            if (cpn <= 160) {
              char line[384];
              sprintf_s(line,
                        "[HSXF4-PLAIN-CHOICE-CAND] #%ld a1=%08X a2=%08X "
                        "a3=%08X ch='%c' trackY=%lu oldY=%lu choice=%d popup=%d",
                        (long)cpn, a1, a2, a3,
                        (decodedCh >= 0x20 && decodedCh <= 0x7E) ? decodedCh : '.',
                        (unsigned long)trackY, (unsigned long)oldY,
                        choiceRender ? 1 : 0, popupRender ? 1 : 0);
              LogHsxf4Probe(line);
            }
          }
          if (!popupRender && !replayRender)
            choiceRender = choiceRender || (oldY >= 120 && oldY <= 360);
          if (nameplateRender)
            choiceRender = false;
          replaySecondLine = false;
          if (replaySecondLine) {
            static thread_local DWORD s_replayPtr = 0;
            static thread_local DWORD s_replayBaseX = 0;
            static thread_local DWORD s_replayBaseY = 0;
            static thread_local DWORD s_replayTick = 0;

            DWORD now = GetTickCount();
            DWORD *replayCursorX = GetHsxf4CursorXPtr();
            if (s_replayPtr != a1 || (s_replayTick != 0 && now - s_replayTick > 900)) {
              s_replayPtr = a1;
              s_replayBaseY = oldY;
              if (replayCursorX && IsReadablePtr(replayCursorX, sizeof(DWORD))) {
                DWORD currentX = *replayCursorX;
                DWORD consumed = replayFirstLineAdvance + replayLineAdvance;
                s_replayBaseX = currentX > consumed
                                    ? currentX - consumed
                                    : currentX;
              } else {
                s_replayBaseX = 0;
              }
            }
            s_replayTick = now;

            if (replayCursorX && IsWritablePtr(replayCursorX, sizeof(DWORD)))
              *replayCursorX = s_replayBaseX + replayLineAdvance;
            *cursorY = s_replayBaseY + 13;
            newY = *cursorY;
            tunedY = true;
            choiceRender = true;
          }
          if (!choiceRender && GetHsxf4TightLineY(a1, a2, oldY, &newY)) {
            *cursorY = newY;
            tunedY = true;
          }
          if (!ellipsisDotRender &&
              (decodedCh == '.' || decodedCh == ',' || decodedCh == '-' ||
               decodedCh == '~')) {
            DWORD baseY = tunedY ? *cursorY : oldY;
            DWORD punctuationDrop = 0;
            if (decodedCh == '.')
              punctuationDrop = 4;
            else if (decodedCh == ',')
              punctuationDrop = 3;
            else if (decodedCh == '-' || decodedCh == '~')
              punctuationDrop = 4;
            *cursorY = baseY + punctuationDrop;
            newY = *cursorY;
            tunedY = true;
          }
          if (!choiceRender && a4 == 0 && a2 == 0 && oldY >= 360 &&
              oldY <= 430 && IsHsxf4NameplateGlyph(decodedCh)) {
            int nameLen = CountHsxf4DecodedAsciiRun(p, a3);
            if (nameLen > 5) {
              if (nameLen > 11) {
                // Adjusted formula to bring shorter names (12-13 chars) slightly left
                // while keeping longer names (17+ chars) at roughly the same position.
                nameStartShift = 15 + (nameLen * 8);
                // Specifically for very long names like "Akane & Koko & Suginami" (23 chars),
                // cap the shift so it doesn't get covered by the right-side sakura decoration.
                // 185 touched left flower, 199 touched right flower. 192 is the exact center.
                if (nameStartShift > 192) {
                  nameStartShift = 192;
                }
              } else {
                nameStartShift = (DWORD)((nameLen - 5) * 9);
                if (nameStartShift > 54)
                  nameStartShift = 54;
              }
              cursorX = GetHsxf4CursorXPtr();
              if (cursorX && IsWritablePtr(cursorX, sizeof(DWORD)))
                *cursorX += nameStartShift;
            }
          }
        }
        // DC2TY.EXE+22BB0 reads global 0xA66FD4 as the next glyph advance
        // before it returns. Changing the function return is too late; tune
        // the advance only for the current Latin/space glyph draw.
        pitch = GetHsxf4PitchPtr();
        if (pitch && IsReadablePtr(pitch, sizeof(DWORD))) {
          oldPitch = *pitch;
          renderHalfWidth =
              !IsAsciiDialogueByte(b1) && IsWritablePtr(p + a2, 2) &&
              ((decodedCh >= 0x20 && decodedCh <= 0x7E) ||
               IsHsxf4LatinWideOverrideSymbol((int)decodedCh));
          renderLatinWideGlyph =
              !renderHalfWidth &&
              (decodedCh == 0x2026 || IsHsxf4LatinCompactSymbol(decodedCh) ||
               IsHsxf4LatinLowLineSymbol(decodedCh));
          renderAsciiParenGlyph = !choiceRender && IsAsciiDialogueByte(b1) &&
                                  (decodedCh == '(' || decodedCh == ')');
          renderAsciiChoice = choiceRender && IsAsciiDialogueByte(b1) &&
                              decodedCh >= 0x20 && decodedCh <= 0x7E;
          renderPopupSpacePlaceholder = popupRender && b1 == '\t';
          renderHsxf4SpacePlaceholder =
              !popupRender && IsAsciiDialogueByte(b1) && b1 == '^';
          renderPopupText =
              popupRender &&
              ((IsAsciiDialogueByte(b1) && decodedCh >= 0x20 &&
                decodedCh <= 0x7E) ||
               renderPopupSpacePlaceholder);
          // Ruby/furigana Latin often reaches HSXF4 as raw single-byte ASCII,
          // unlike normal dialogue Latin which is full-width SJIS here. Keep
          // the glyph path untouched, but tighten the cursor advance so ruby
          // words are readable instead of spaced out per character.
          tuneAsciiDialogueAdvance =
              !popupRender && !choiceRender && !renderAsciiParenGlyph && IsAsciiDialogueByte(b1) &&
              decodedCh >= 0x20 && decodedCh <= 0x7E &&
              !(a4 == 0 && oldY >= 380 && oldY <= 410);
          if (nameplateRender) {
            int nameLen = CountHsxf4DecodedAsciiRun(p, a3);
            nameplateLongRender = nameLen > 11;
            if (nameplateLongRender) {
              nameVisualShift = 0;
              newPitch = 18;
              newAdvance = decodedCh == ' ' ? 5 : 8;
              if (decodedCh == '&')
                newAdvance = 8;
              if (decodedCh == '~')
                newAdvance = 14;
            } else {
              newPitch = 18;
              newAdvance = decodedCh == ' ' ? 5 : 8;
              if (decodedCh == '~')
                newAdvance = 14;
            }
          } else if (replayRender) {
            newPitch = (DWORD)GetHsxf4ReplayPitch(decodedCh);
            newAdvance = (DWORD)GetHsxf4ReplayAdvance(decodedCh);
          } else if (popupRender) {
            newPitch = (DWORD)GetHsxf4HalfWidthPopupPitch(decodedCh);
            newAdvance = (DWORD)GetHsxf4HalfWidthPopupAdvance(decodedCh);
          } else if (choiceRender) {
            newPitch = (DWORD)GetHsxf4HalfWidthChoicePitch(decodedCh);
            newAdvance = (DWORD)GetHsxf4HalfWidthChoiceAdvance(decodedCh);
            if (choiceMarkerSeparatorSpace) {
              newPitch = 1;
              newAdvance = 0;
            }
          } else if (tuneAsciiDialogueAdvance) {
            newPitch = (DWORD)GetHsxf4HalfWidthDialoguePitch(decodedCh);
            newAdvance = (DWORD)GetHsxf4HalfWidthDialogueAdvance(decodedCh);
          } else if (suppressPauseDash) {
            newPitch = 0;
            newAdvance = 0;
          } else if (triplePauseGlyph) {
            newPitch = 20;
            newAdvance = 20;
          } else if (renderHalfWidth) {
            newPitch = (DWORD)GetHsxf4HalfWidthDialoguePitch(decodedCh);
            newAdvance = (DWORD)GetHsxf4HalfWidthDialogueAdvance(decodedCh);
            if (ellipsisDotRender) {
              newPitch = 16;
              newAdvance = 12;
            }
          } else {
            newPitch = (DWORD)GetHsxf4DialoguePitch(decodedCh);
            newAdvance = newPitch;
            if (ellipsisDotRender) {
              newPitch = 16;
              newAdvance = 12;
            }
          }
          bool forceQuoteSpace =
              decodedCh == ' ' &&
              (IsHsxf4QuoteSpacing(prevCh) || IsHsxf4QuoteSpacing(nextCh));
          if (forceQuoteSpace)
            newPitch = newAdvance = 5;
          else if (decodedCh == ' ' && IsHsxf4TightPunctuation(nextCh))
            newPitch = newAdvance = 1;
          else if (decodedCh == ' ' && (prevCh == '\'' || prevCh == '"'))
            newPitch = newAdvance = 2;
          else if (decodedCh == ' ' && prevCh == '-')
            newPitch = newAdvance = 2;
          bool forceZeroPitchPunctuation =
              (oldPitch == 0 &&
               (IsHsxf4TightPunctuation(decodedCh) || decodedCh == '-')) ||
              forceQuoteSpace;
          bool forceUiPitch =
              (choiceRender || popupRender || replayRender || nameplateRender) &&
              newPitch > 0;
          if (newPitch > 0 &&
              (forceUiPitch || oldPitch > newPitch || forceZeroPitchPunctuation)) {
            *pitch = newPitch;
            tuned = true;
          }
        }
      }
    }
  }

  if (nameVisualShift > 0) {
    cursorX = GetHsxf4CursorXPtr();
    if (cursorX && IsWritablePtr(cursorX, sizeof(DWORD)))
      *cursorX += nameVisualShift;
  }

  bool savedExtraPauseDot = g_hsxf4DrawExtraPauseDot;
  int savedExtraPauseDotCount = g_hsxf4ExtraPauseDotCount;
  bool savedTriplePauseGlyph = g_hsxf4TriplePauseGlyph;
  bool savedReplayRender = g_hsxf4ReplayRender;
  bool savedNameplateRender = g_hsxf4NameplateRender;
  bool savedNameplateLongRender = g_hsxf4NameplateLongRender;
  bool enteredLatinRender = (renderHalfWidth || tuneAsciiDialogueAdvance ||
                             renderAsciiChoice || renderPopupText ||
                             renderLatinWideGlyph || renderAsciiParenGlyph ||
                             renderHsxf4SpacePlaceholder ||
                             renderReplayTranslatedGlyph || replayRender ||
                             nameplateRender);
  bool isTildeProxy = false;
  if (enteredLatinRender && p) {
    savedB1 = p[a2];
    savedB2 = p[a2 + 1];
    if (renderHalfWidth) {
      if (!IsHsxf4LatinWideOverrideSymbol((int)decodedCh)) {
        // For the wave-dash proxy ('~'), write '-' instead so that the
        // isTildeProxy guard below does not replace it with '_'.
        // g_hsxf4GlyphOverride will redirect the '-' glyph back to '~'
        // inside Hook_GetGlyphOutlineA, yielding the correct tilde shape
        // from Verdana at mid-height.
        p[a2] = (decodedCh == '~') ? '-' : (BYTE)decodedCh;
        p[a2 + 1] = ' ';
      }
      // Unicode symbols: keep the original SJIS bytes intact. The engine will
      // convert them to Unicode and g_hsxf4GlyphOverride selects the intended
      // glyph/font inside the GetGlyphOutline hooks.
    } else if (renderReplayTranslatedGlyph) {
      p[a2] = 0x81;
      p[a2 + 1] = 0x40;
    } else if (renderPopupSpacePlaceholder) {
      p[a2] = ' ';
    } else if (renderHsxf4SpacePlaceholder) {
      p[a2] = ' ';
    }
    g_hsxf4ChoiceRender = choiceRender;
    g_hsxf4PopupRender = popupRender;
    g_hsxf4ReplayRender = replayRender;
    g_hsxf4NameplateRender = nameplateRender;
    g_hsxf4NameplateLongRender = nameplateLongRender;
    g_hsxf4EllipsisDotRender = ellipsisDotRender;
    g_hsxf4GlyphOverride =
        IsHsxf4LatinWideOverrideSymbol((int)decodedCh) ? (UINT)decodedCh :
        (renderLatinWideGlyph && decodedCh > 0) ? (UINT)decodedCh :
        (renderHalfWidth && decodedCh == '~') ? (UINT)'~' :
        (((choiceRender || popupRender) && decodedCh >= 0x20) ? (UINT)decodedCh
                                                              : 0);
    g_hsxf4DrawExtraPauseDot = extraPauseDot || triplePauseGlyph;
    g_hsxf4ExtraPauseDotCount = triplePauseGlyph ? 2 : (extraPauseDot ? 1 : 0);
    g_hsxf4TriplePauseGlyph = triplePauseGlyph;
    // Original safeguard: replace raw '~' with '_' so the game engine
    // does not interpret '~' as a script operator.  For the wave-proxy
    // path (renderHalfWidth with decodedCh=='~') we already wrote '-'
    // to the buffer above, so this check will NOT fire for that case.
    if (p && p[a2] == '~' && IsWritablePtr(&p[a2], 1)) {
      p[a2] = '_';
      isTildeProxy = true;
      // Redirect: hook will render '~' from Verdana (raised) instead of '_'
      g_hsxf4GlyphOverride = '~';
    }
    ++g_hsxf4LatinRenderDepth;
  }



  DWORD ret = Real_Hsxf4_22BB0(a1, a2, a3, a4);

  if (isTildeProxy) {
    p[a2] = '~';
  }
  g_hsxf4DrawExtraPauseDot = savedExtraPauseDot;
  g_hsxf4ExtraPauseDotCount = savedExtraPauseDotCount;
  g_hsxf4TriplePauseGlyph = savedTriplePauseGlyph;

  if ((enteredLatinRender || tuneAsciiDialogueAdvance) && p) {
    if (enteredLatinRender) {
      --g_hsxf4LatinRenderDepth;
      g_hsxf4ChoiceRender = false;
      g_hsxf4PopupRender = false;
      g_hsxf4ReplayRender = savedReplayRender;
      g_hsxf4NameplateRender = savedNameplateRender;
      g_hsxf4NameplateLongRender = savedNameplateLongRender;
      g_hsxf4EllipsisDotRender = false;
      g_hsxf4GlyphOverride = 0;
      g_hsxf4DrawExtraPauseDot = false;
      g_hsxf4ExtraPauseDotCount = 0;
      g_hsxf4TriplePauseGlyph = false;
      if (renderHalfWidth) {
        p[a2] = savedB1;
        p[a2 + 1] = savedB2;
        ret = a2 + 2;
      } else if (renderReplayTranslatedGlyph) {
        p[a2] = savedB1;
        p[a2 + 1] = savedB2;
      } else if (renderPopupSpacePlaceholder) {
        p[a2] = savedB1;
      } else if (renderHsxf4SpacePlaceholder) {
        p[a2] = savedB1;
      }
    }
    if (nameVisualShift > 0) {
      cursorX = GetHsxf4CursorXPtr();
      if (cursorX && IsWritablePtr(cursorX, sizeof(DWORD))) {
        if (*cursorX >= nameVisualShift)
          *cursorX -= nameVisualShift;
        else
          *cursorX = 0;
      }
    }
    if (newPitch > newAdvance) {
      cursorX = GetHsxf4CursorXPtr();
      if (cursorX && IsWritablePtr(cursorX, sizeof(DWORD))) {
        *cursorX -= (newPitch - newAdvance);
      }
    }
  }

  if (tunedY && cursorY)
    *cursorY = oldY;

  if (tuned && pitch)
    *pitch = oldPitch;

  static volatile LONG s_nameplateProbeCount = 0;
  LONG nameplateProbe =
      nameplateRender ? InterlockedIncrement(&s_nameplateProbeCount) : 0;
  if (n <= 90 || (nameplateRender && nameplateProbe <= 120)) {
    char line[512];
    sprintf_s(line,
              "[HSXF4-PITCH] #%ld a1=%08X a2=%08X a3=%08X a4=%08X "
              "b=%02X,%02X prev='%c' ch='%c' next='%c' "
              "pitch=%lu->%lu adv=%lu y=%lu->%lu ret=%08X tuned=%d norm=%d "
              "name=%d choice=%d popup=%d replay=%d skip=%d tri=%d sup=%d extra=%d",
              (long)n, a1, a2, a3, a4, (unsigned)b1, (unsigned)b2,
              (prevCh >= 0x20 && prevCh <= 0x7E) ? prevCh : '.',
              (decodedCh >= 0x20 && decodedCh <= 0x7E) ? decodedCh : '.',
              (nextCh >= 0x20 && nextCh <= 0x7E) ? nextCh : '.',
              (unsigned long)oldPitch, (unsigned long)newPitch,
              (unsigned long)newAdvance, (unsigned long)oldY,
              (unsigned long)(tunedY ? newY : oldY), ret,
              tuned ? 1 : 0, renderHalfWidth ? 1 : 0,
              nameplateRender ? 1 : 0, choiceRender ? 1 : 0,
              popupRender ? 1 : 0, replayRender ? 1 : 0,
              (unsigned)nameStartShift, triplePauseGlyph ? 1 : 0,
              suppressPauseDash ? 1 : 0,
              triplePauseGlyph ? 2 : (extraPauseDot ? 1 : 0));
    LogHsxf4Probe(line);
  }
  static volatile LONG s_choiceProbeCount = 0;
  LONG choiceProbe = choiceRender ? InterlockedIncrement(&s_choiceProbeCount) : 0;
  if (choiceRender && choiceProbe <= 240) {
    char line[512];
    sprintf_s(line,
              "[HSXF4-CHOICE] #%ld a1=%08X a2=%08X a3=%08X b=%02X,%02X "
              "ch='%c' prev='%c' next='%c' popup=%d enc=%d ascii=%d "
              "pitch=%lu->%lu adv=%lu entered=%d glyph=%04X",
              (long)choiceProbe, a1, a2, a3, (unsigned)b1, (unsigned)b2,
              (decodedCh >= 0x20 && decodedCh <= 0x7E) ? decodedCh : '.',
              (prevCh >= 0x20 && prevCh <= 0x7E) ? prevCh : '.',
              (nextCh >= 0x20 && nextCh <= 0x7E) ? nextCh : '.',
              popupRender ? 1 : 0,
              encryptedChoice ? 1 : 0,
              renderAsciiChoice ? 1 : 0,
              (unsigned long)oldPitch, (unsigned long)newPitch,
              (unsigned long)newAdvance, enteredLatinRender ? 1 : 0,
              (unsigned)g_hsxf4GlyphOverride);
    LogHsxf4Probe(line);
  }
  return ret;
}

static void LogRenderPathTelemetryW(const char *hookName, HDC hdc, int x, int y,
                                    int c, bool likelyByArea,
                                    bool likelyDialogueLatinOrig,
                                    bool useDialogueTuning, bool translated,
                                    const INT *dx, LPCWSTR text) {
  if (!hookName)
    return;
  if (c <= 0 || !text)
    return;

  WCHAR exePath[MAX_PATH];
  GetModuleFileNameW(NULL, exePath, MAX_PATH);
  std::wstring exeDir = exePath;
  size_t pos = exeDir.find_last_of(L"\\/");
  if (pos != std::wstring::npos) {
    exeDir = exeDir.substr(0, pos);
  }
  std::wstring logPath = exeDir + L"\\dc2ty_path.log";
  FILE *f = _wfopen(logPath.c_str(), L"a");
  if (!f)
    return;

  RECT rc = {0, 0, 0, 0};
  int clipRes = hdc ? GetClipBox(hdc, &rc) : ERROR;
  TEXTMETRICA tm = {};
  BOOL hasTm = hdc ? GetTextMetricsA(hdc, &tm) : FALSE;
  int charExtra = hdc ? GetTextCharacterExtra(hdc) : 0;
  UINT align = hdc ? GetTextAlign(hdc) : 0;

  fprintf(
      f,
      "[PATH-W][%s] tid=%lu x=%d y=%d c=%d area=%d latin=%d tune=%d trans=%d "
      "charExtra=%d align=0x%X clip=(%ld,%ld,%ld,%ld) clipRes=%d tmH=%d\n",
      hookName, (unsigned long)GetCurrentThreadId(), x, y, c,
      likelyByArea ? 1 : 0, likelyDialogueLatinOrig ? 1 : 0,
      useDialogueTuning ? 1 : 0, translated ? 1 : 0, charExtra, align, rc.left,
      rc.top, rc.right, rc.bottom, clipRes, hasTm ? tm.tmHeight : -1);

  int sample = (c < 40) ? c : 40;
  fprintf(f, "    txt='");
  for (int i = 0; i < sample; ++i) {
    wchar_t wc = text[i];
    if (wc >= 0x20 && wc <= 0x7E) {
      fputc((char)wc, f);
    } else {
      fputc('.', f);
    }
  }
  fprintf(f, "'\n");

  fprintf(f, "    whex=");
  for (int i = 0; i < sample; ++i) {
    fprintf(f, "%04X ", (unsigned int)text[i]);
  }
  fprintf(f, "\n");

  if (dx) {
    int d = (c < 12) ? c : 12;
    fprintf(f, "    dx=");
    for (int i = 0; i < d; ++i) {
      fprintf(f, "%d ", dx[i]);
    }
    fprintf(f, "\n");
  }

  fclose(f);
}

static bool ShouldSamplePathTelemetry() {
  static volatile LONG s_counter = 0;
  LONG v = InterlockedIncrement(&s_counter);
  return ((v % 25) == 0);
}

struct ScopedDialogueXScale {
  HDC hdc = NULL;
  bool applied = false;
  int oldMode = 0;
  XFORM oldXf = {1, 0, 0, 1, 0, 0};

  ScopedDialogueXScale(HDC dc, bool enable, int anchorX, int anchorY,
                       float scaleX = 0.38f, float scaleY = 1.0f)
      : hdc(dc) {
    if (!enable || !hdc)
      return;
    oldMode = SetGraphicsMode(hdc, GM_ADVANCED);
    if (oldMode == 0)
      return;
    if (!GetWorldTransform(hdc, &oldXf)) {
      oldXf.eM11 = 1.0f;
      oldXf.eM12 = 0.0f;
      oldXf.eM21 = 0.0f;
      oldXf.eM22 = 1.0f;
      oldXf.eDx = 0.0f;
      oldXf.eDy = 0.0f;
    }
    XFORM xf = oldXf;
    float sx = scaleX;
    float sy = scaleY;
    if (sx < 0.35f)
      sx = 0.35f;
    if (sx > 1.00f)
      sx = 1.00f;
    if (sy < 0.55f)
      sy = 0.55f;
    if (sy > 1.40f)
      sy = 1.40f;
    xf.eM11 = oldXf.eM11 * sx;
    xf.eM22 = oldXf.eM22 * sy;
    xf.eDx = oldXf.eDx + (float)anchorX * (1.0f - sx);
    xf.eDy = oldXf.eDy + (float)anchorY * (1.0f - sy);
    if (SetWorldTransform(hdc, &xf)) {
      applied = true;
    }
  }

  ~ScopedDialogueXScale() {
    if (!hdc)
      return;
    if (applied) {
      SetWorldTransform(hdc, &oldXf);
    }
    if (oldMode == GM_COMPATIBLE) {
      SetGraphicsMode(hdc, GM_COMPATIBLE);
    }
  }
};

static float GetDialogueSizeScaleY() {
  int raw = g_fontManager.GetDialogueFontSize();
  if (raw == 0)
    raw = -19;
  if (raw > 0)
    raw = -raw;
  int absH = -raw;
  if (absH < 8)
    absH = 8;
  if (absH > 48)
    absH = 48;
  float s = (float)absH / 19.0f;
  if (s < 0.55f)
    s = 0.55f;
  if (s > 1.40f)
    s = 1.40f;
  return s;
}

static int CompressDialogueRunXSafe(bool enable, int x, int y) {
  static thread_local int s_baseX = 0;
  static thread_local int s_lastX = 0;
  static thread_local int s_lastY = 0x7fffffff;
  static thread_local DWORD s_lastTick = 0;
  if (!enable)
    return x;
  DWORD now = GetTickCount();
  bool newRun = false;
  if (s_lastY == 0x7fffffff)
    newRun = true;
  if (abs(y - s_lastY) >= 3)
    newRun = true;
  if (x + 3 < s_lastX)
    newRun = true;
  if (s_lastTick != 0 && (now - s_lastTick) > 140)
    newRun = true;
  if (newRun)
    s_baseX = x;
  int dx = x - s_baseX;
  if (dx < 0)
    dx = 0;
  int scaledDx = (dx * 16 + 50) / 100;
  int outX = s_baseX + scaledDx;
  s_lastX = x;
  s_lastY = y;
  s_lastTick = now;
  return outX;
}

static float ComputeFitScaleA(HDC hdc, int drawX, const RECT *rc, LPCSTR s,
                              UINT len, float baseScale) {
  if (!hdc || !rc || !s || len == 0)
    return baseScale;
  SIZE sz = {};
  if (!GetTextExtentPoint32A(hdc, s, (int)len, &sz))
    return baseScale;
  int avail = rc->right - drawX - 6;
  if (avail <= 8 || sz.cx <= 0)
    return baseScale;
  float fit = (float)avail / (float)sz.cx;
  if (fit > baseScale)
    fit = baseScale;
  if (fit < 0.35f)
    fit = 0.35f;
  return fit;
}

static float ComputeFitScaleW(HDC hdc, int drawX, const RECT *rc, LPCWSTR s,
                              UINT len, float baseScale) {
  if (!hdc || !rc || !s || len == 0)
    return baseScale;
  SIZE sz = {};
  if (!GetTextExtentPoint32W(hdc, s, (int)len, &sz))
    return baseScale;
  int avail = rc->right - drawX - 6;
  if (avail <= 8 || sz.cx <= 0)
    return baseScale;
  float fit = (float)avail / (float)sz.cx;
  if (fit > baseScale)
    fit = baseScale;
  if (fit < 0.35f)
    fit = 0.35f;
  return fit;
}

static void LogGlyphPathTelemetry(const char *hookName, HDC hdc, UINT uChar,
                                  const GLYPHMETRICS *gm, bool force = false) {
  if (!hookName || !hdc || !gm)
    return;
  if (!force && !ShouldSamplePathTelemetry())
    return;

  WCHAR exePath[MAX_PATH];
  GetModuleFileNameW(NULL, exePath, MAX_PATH);
  std::wstring exeDir = exePath;
  size_t pos = exeDir.find_last_of(L"\\/");
  if (pos != std::wstring::npos) {
    exeDir = exeDir.substr(0, pos);
  }
  std::wstring logPath = exeDir + L"\\dc2ty_path.log";
  FILE *f = _wfopen(logPath.c_str(), L"a");
  if (!f)
    return;

  RECT rc = {0, 0, 0, 0};
  int clipRes = GetClipBox(hdc, &rc);
  POINT pt = {0, 0};
  BOOL hasPt = GetCurrentPositionEx(hdc, &pt);
  TEXTMETRICA tm = {};
  BOOL hasTm = GetTextMetricsA(hdc, &tm);

  fprintf(f,
          "[GLYPH][%s] tid=%lu ch=0x%X('%c') cell=%d black=(%lu,%lu) "
          "origin=(%d,%d) cur=(%ld,%ld) clip=(%ld,%ld,%ld,%ld) "
          "clipRes=%d tmH=%d\n",
          hookName, (unsigned long)GetCurrentThreadId(), (unsigned int)uChar,
          (uChar >= 0x20 && uChar <= 0x7E) ? (char)uChar : '.', gm->gmCellIncX,
          (unsigned long)gm->gmBlackBoxX, (unsigned long)gm->gmBlackBoxY,
          gm->gmptGlyphOrigin.x, gm->gmptGlyphOrigin.y, hasPt ? pt.x : -1,
          hasPt ? pt.y : -1, rc.left, rc.top, rc.right, rc.bottom, clipRes,
          hasTm ? tm.tmHeight : -1);
  fclose(f);
}

static int AdjustDialogueDrawX(HDC hdc, int originalX, int y, UINT c,
                               int charCode) {
  if (g_inBacklogRender)
    return originalX;

  static thread_local int s_prevY = 0;
  static thread_local int s_prevOrigX = 0;
  static thread_local int s_prevDrawX = 0;
  static thread_local bool s_hasPrev = false;

  // Dialogue often draws one logical char as 1/2 bytes, but some paths batch
  // small chunks. Keep compression active up to short chunks as well.
  if (c > 8 || c == 0) {
    s_prevY = y;
    s_prevOrigX = originalX;
    s_prevDrawX = originalX;
    s_hasPrev = true;
    return originalX;
  }

  int step = GetDialogueAdvanceStep();
  if (charCode == ' ') {
    step = (step * g_fontManager.GetDialogueSpaceScale() + 50) / 100;
    step = (step * 30 + 50) / 100;
  }
  if (step < 2)
    step = 2;

  if (!s_hasPrev || abs(y - s_prevY) > 2 ||
      originalX < s_prevOrigX || (originalX - s_prevOrigX) > 120) {
    s_prevY = y;
    s_prevOrigX = originalX;
    s_prevDrawX = originalX;
    s_hasPrev = true;
    return originalX;
  }

  int rawDelta = originalX - s_prevOrigX;
  int adjustedX = originalX;
  if (rawDelta > step) {
    adjustedX = s_prevDrawX + step;
  }

  s_prevY = y;
  s_prevOrigX = originalX;
  s_prevDrawX = adjustedX;
  return adjustedX;
}

static bool ShouldSkipSyntheticSpaceA(HDC hdc, int y, LPCSTR s, UINT c,
                                      bool useDialogueTuning) {
  (void)hdc;
  if (!useDialogueTuning || !s || c != 1)
    return false;
  static thread_local int s_y = 0;
  static thread_local int s_charCount = 0;
  static thread_local int s_alphaCount = 0;
  static thread_local int s_spaceCount = 0;
  static thread_local bool s_synthMode = false;

  if (abs(y - s_y) > 2 || s_charCount > 256) {
    s_y = y;
    s_charCount = 0;
    s_alphaCount = 0;
    s_spaceCount = 0;
    s_synthMode = false;
  }

  unsigned char ch = (unsigned char)s[0];
  bool isAlphaNum = ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
                     (ch >= 'a' && ch <= 'z'));
  bool isSpace = (ch == ' ' || ch == '\t');

  s_charCount++;
  if (isAlphaNum)
    s_alphaCount++;
  if (isSpace)
    s_spaceCount++;

  if (!s_synthMode && s_charCount >= 4 && s_spaceCount >= 2 &&
      s_spaceCount * 2 >= s_alphaCount) {
    s_synthMode = true;
  }
  if (s_synthMode && isSpace)
    return true;
  return false;
}

static bool ShouldSkipSyntheticSpaceW(HDC hdc, int y, LPCWSTR s, UINT c,
                                      bool useDialogueTuning) {
  (void)hdc;
  if (!useDialogueTuning || !s || c != 1)
    return false;
  static thread_local int s_y = 0;
  static thread_local int s_charCount = 0;
  static thread_local int s_alphaCount = 0;
  static thread_local int s_spaceCount = 0;
  static thread_local bool s_synthMode = false;

  if (abs(y - s_y) > 2 || s_charCount > 256) {
    s_y = y;
    s_charCount = 0;
    s_alphaCount = 0;
    s_spaceCount = 0;
    s_synthMode = false;
  }

  wchar_t ch = s[0];
  bool isAlphaNum = ((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z') ||
                     (ch >= L'a' && ch <= L'z'));
  bool isSpace = (ch == L' ' || ch == L'\t' || ch == 0x3000);

  s_charCount++;
  if (isAlphaNum)
    s_alphaCount++;
  if (isSpace)
    s_spaceCount++;

  if (!s_synthMode && s_charCount >= 4 && s_spaceCount >= 2 &&
      s_spaceCount * 2 >= s_alphaCount) {
    s_synthMode = true;
  }
  if (s_synthMode && isSpace)
    return true;
  return false;
}

static HFONT SelectChoiceTextFont(HDC hdc);
static HFONT SelectPopupUiTextFont(HDC hdc);
static HFONT GetChoiceUiFont();

static HFONT SelectHsxf4LatinFont(HDC hdc, UINT glyphChar = 0) {
  if (!hdc || !InHsxf4LatinRender())
    return NULL;

  if (g_hsxf4ChoiceRender)
    return SelectChoiceTextFont(hdc);
  if (g_hsxf4PopupRender)
    return SelectPopupUiTextFont(hdc);
  if (g_hsxf4ReplayRender)
    return SelectReplayTextFont(hdc, -10);

  int userSize = g_fontManager.GetDialogueFontSize();
  int absSize = userSize;
  if (absSize == 0)
    absSize = 19;
  if (absSize < 0)
    absSize = -absSize;

  int drawSize = absSize + 9;
  if (drawSize < 27)
    drawSize = 27;
  if (drawSize > 34)
    drawSize = 34;

  HFONT font = NULL;
  if (glyphChar == 0x266A || glyphChar == 0x266B || glyphChar == 0x266C ||
      IsHsxf4LatinCompactSymbol((int)glyphChar)) {
    // Symbol glyphs use Segoe UI Symbol, then get scaled/tuned below.
    font = g_fontManager.GetMusicNoteFont(drawSize);
  } else if (glyphChar == 0x301C) {
    font = g_fontManager.GetDialogueFont(-drawSize);
  } else {
    font = g_fontManager.GetDialogueOutlineFontExact(-drawSize);
  }

  if (!font)
    return NULL;
  return (HFONT)SelectObject(hdc, font);
}

static void TuneHsxf4LatinGlyphMetrics(UINT ch, GLYPHMETRICS *gm) {
  if (!gm)
    return;

  // Some fonts report the period too high for ADV32's per-glyph baseline.
  // Move punctuation down without changing the font size or layout pitch.
  if (ch == '.') {
    if (g_hsxf4EllipsisDotRender) {
      if (gm->gmCellIncX > 8)
        gm->gmCellIncX = 8;
      gm->gmptGlyphOrigin.y -= 18;
    } else {
      gm->gmptGlyphOrigin.y -= 14;
    }
  } else if (ch == 0x2026 || IsHsxf4LatinLowLineSymbol((int)ch)) {
    // Pause marks from Japanese ellipsis/dash are normalized to Latin dots.
    // Keep the glyph Latin-sized; vertical placement is handled at draw time.
    DWORD maxInc = g_hsxf4TriplePauseGlyph ? 20 : 16;
    if (gm->gmCellIncX > maxInc)
      gm->gmCellIncX = maxInc;
    if (ch == 0x2026)
      gm->gmptGlyphOrigin.y -= 10;
  } else if (ch == ',') {
    gm->gmptGlyphOrigin.y -= 6;
  } else if (ch == '-') {
    gm->gmptGlyphOrigin.y -= 13;
  } else if (ch == '~') {
    gm->gmptGlyphOrigin.y += 10; // Raise wave-dash to mid x-height
  } else if (IsHsxf4LatinGreekSymbol((int)ch)) {
    // Keep mu in the same visual cell as the surrounding Latin dialogue.
    if (gm->gmCellIncX > 20)
      gm->gmCellIncX = 20;
  } else if (IsHsxf4LatinCompactSymbol((int)ch)) {
    if (gm->gmCellIncX > 20)
      gm->gmCellIncX = 20;
    gm->gmptGlyphOrigin.y -= 3;
  }
}

static void ShiftLongNameplateGlyph(GLYPHMETRICS *gm) {
  if (!gm || !g_hsxf4NameplateLongRender)
    return;
  // The engine anchors long speaker names too far left, under the flower.
  // Moving the glyph origin is the path that actually affects HSXF4 nameplates.
  gm->gmptGlyphOrigin.x += 46;
}

static void EmboldenHsxf4LatinGlyphBitmap(UINT fuFormat,
                                          const GLYPHMETRICS *gm,
                                          DWORD resultSize, DWORD cjBuffer,
                                          LPVOID pvBuffer, UINT ch = 0) {
  if (!gm || !pvBuffer || resultSize == GDI_ERROR || resultSize == 0 ||
      cjBuffer == 0 || gm->gmBlackBoxX == 0 || gm->gmBlackBoxY == 0)
    return;

  DWORD usable = (resultSize < cjBuffer) ? resultSize : cjBuffer;
  UINT fmt = fuFormat & 0x7;
  BYTE *bits = reinterpret_cast<BYTE *>(pvBuffer);
  DWORD w = gm->gmBlackBoxX;
  DWORD h = gm->gmBlackBoxY;

  if (fmt == GGO_GRAY2_BITMAP || fmt == GGO_GRAY4_BITMAP ||
      fmt == GGO_GRAY8_BITMAP) {
    DWORD stride = (w + 3) & ~3u;
    if (stride == 0 || stride * h > usable)
      return;
    bool smallPunctuation = (w <= 8 && h <= 8);
    bool tinyPeriod = (ch == '.' && smallPunctuation);
    int passes = tinyPeriod ? 4 : (smallPunctuation ? 2 : 1);
    BYTE minDotValue = 0;
    if (tinyPeriod) {
      minDotValue = (fmt == GGO_GRAY2_BITMAP) ? 3 :
                    (fmt == GGO_GRAY4_BITMAP) ? 12 : 48;
    }
    for (int pass = 0; pass < passes; ++pass) {
    for (DWORD y = 0; y < h; ++y) {
      BYTE *row = bits + y * stride;
      for (LONG x = (LONG)w - 1; x >= 1; --x) {
        BYTE left = row[x - 1];
        if (left > row[x])
          row[x] = left;
      }
    }
      if (smallPunctuation) {
        for (LONG y = (LONG)h - 1; y >= 1; --y) {
          BYTE *row = bits + y * stride;
          BYTE *above = bits + (y - 1) * stride;
          for (DWORD x = 0; x < w; ++x) {
            if (above[x] > row[x])
              row[x] = above[x];
        }
      }
    }
      if (tinyPeriod) {
        for (DWORD y = 0; y < h; ++y) {
          BYTE *row = bits + y * stride;
          for (DWORD x = 0; x < w; ++x) {
            if (row[x] > 0 && row[x] < minDotValue)
              row[x] = minDotValue;
          }
        }
      }
    }
    return;
  }

  if (fmt == GGO_BITMAP) {
    DWORD stride = ((w + 31) & ~31u) >> 3;
    if (stride == 0 || stride * h > usable)
      return;
    bool smallPunctuation = (w <= 8 && h <= 8);
    bool tinyPeriod = (ch == '.' && smallPunctuation);
    int passes = tinyPeriod ? 4 : (smallPunctuation ? 2 : 1);
    for (int pass = 0; pass < passes; ++pass) {
    for (DWORD y = 0; y < h; ++y) {
      BYTE *row = bits + y * stride;
      for (LONG x = (LONG)w - 1; x >= 1; --x) {
        DWORD src = (DWORD)(x - 1);
        DWORD dst = (DWORD)x;
        BYTE srcMask = (BYTE)(0x80u >> (src & 7));
        if (row[src >> 3] & srcMask) {
          BYTE dstMask = (BYTE)(0x80u >> (dst & 7));
          row[dst >> 3] |= dstMask;
        }
      }
    }
      if (smallPunctuation) {
        for (LONG y = (LONG)h - 1; y >= 1; --y) {
          BYTE *row = bits + y * stride;
          BYTE *above = bits + (y - 1) * stride;
          for (DWORD x = 0; x < w; ++x) {
            BYTE mask = (BYTE)(0x80u >> (x & 7));
            if (above[x >> 3] & mask)
              row[x >> 3] |= mask;
          }
        }
      }
    }
  }
}

static DWORD BuildHsxf4TripleDotGlyphBitmap(UINT fuFormat, GLYPHMETRICS *gm,
                                            DWORD resultSize, DWORD cjBuffer,
                                            LPVOID pvBuffer) {
  if (!gm || resultSize == GDI_ERROR || resultSize == 0 ||
      gm->gmBlackBoxX == 0 || gm->gmBlackBoxY == 0)
    return resultSize;

  // Keep the real glyph as one Latin period. The other two dots are drawn
  // explicitly by the HSXF4 TextOut hooks, which is more reliable than trying
  // to pack three period bitmaps into GDI's glyph buffer for every format.
  gm->gmCellIncX = 20;
  return resultSize;

  UINT fmt = fuFormat & 0x7;
  DWORD oldW = gm->gmBlackBoxX;
  DWORD h = gm->gmBlackBoxY;
  DWORD gap = (oldW <= 4) ? 2 : 3;
  DWORD newW = oldW * 3 + gap * 2;
  if (newW < 14)
    newW = 14;
  if (newW > 30)
    newW = 30;

  auto grayStride = [](DWORD w) -> DWORD { return (w + 3) & ~3u; };
  auto monoStride = [](DWORD w) -> DWORD { return ((w + 31) & ~31u) >> 3; };

  DWORD oldStride = 0;
  DWORD newStride = 0;
  bool grayBitmap = (fmt == GGO_GRAY2_BITMAP || fmt == GGO_GRAY4_BITMAP ||
                     fmt == GGO_GRAY8_BITMAP);
  bool monoBitmap = (fmt == GGO_BITMAP);

  if (grayBitmap) {
    oldStride = grayStride(oldW);
    newStride = grayStride(newW);
  } else if (monoBitmap) {
    oldStride = monoStride(oldW);
    newStride = monoStride(newW);
  } else {
    // Vector/native formats keep the original glyph but reserve triple-dot
    // advance, so layout does not collapse if GDI chooses this path.
    gm->gmBlackBoxX = newW;
    gm->gmCellIncX = 20;
    return resultSize;
  }

  DWORD oldSize = oldStride * h;
  DWORD newSize = newStride * h;
  gm->gmBlackBoxX = newW;
  gm->gmCellIncX = (newW + 2 > 20) ? (newW + 2) : 20;

  if (!pvBuffer || cjBuffer == 0)
    return newSize;
  if (oldStride == 0 || newStride == 0 || resultSize < oldSize ||
      cjBuffer < newSize)
    return newSize;

  BYTE *bits = reinterpret_cast<BYTE *>(pvBuffer);
  std::vector<BYTE> original(oldSize);
  memcpy(original.data(), bits, oldSize);
  memset(bits, 0, newSize);

  DWORD offsets[3] = {0, oldW + gap, oldW * 2 + gap * 2};
  if (offsets[2] + oldW > newW && newW > oldW)
    offsets[2] = newW - oldW;

  if (grayBitmap) {
    for (DWORD y = 0; y < h; ++y) {
      const BYTE *src = original.data() + y * oldStride;
      BYTE *dst = bits + y * newStride;
      for (DWORD dot = 0; dot < 3; ++dot) {
        DWORD dx = offsets[dot];
        if (dx >= newW)
          continue;
        DWORD copyW = oldW;
        if (dx + copyW > newW)
          copyW = newW - dx;
        memcpy(dst + dx, src, copyW);
      }
    }
  } else {
    for (DWORD y = 0; y < h; ++y) {
      const BYTE *src = original.data() + y * oldStride;
      BYTE *dst = bits + y * newStride;
      for (DWORD sx = 0; sx < oldW; ++sx) {
        BYTE srcMask = (BYTE)(0x80u >> (sx & 7));
        if (!(src[sx >> 3] & srcMask))
          continue;
        for (DWORD dot = 0; dot < 3; ++dot) {
          DWORD dx = offsets[dot] + sx;
          if (dx >= newW)
            continue;
          BYTE dstMask = (BYTE)(0x80u >> (dx & 7));
          dst[dx >> 3] |= dstMask;
        }
      }
    }
  }

  return newSize;
}

static inline int AdjustLatinPunctuationDrawY(int y, UINT ch) {
  // ADV32 draws Latin punctuation through the Japanese glyph baseline, so
  // single glyph periods/hyphens sit too high unless the draw call is lowered.
  if (ch == '-')
    return y + 12;
  if (ch == ',')
    return y + 6;
  if (ch == '.')
    return y + (g_hsxf4EllipsisDotRender ? 18 : 12);
  if (ch == '~')
    return y + 12;
  if (IsHsxf4LatinLowLineSymbol((int)ch))
    return y + 18;
  return y;
}

static inline UINT NormalizeHsxf4GlyphChar(UINT ch) {
  switch (ch) {
  case '_': // Our proxy character for ~
    return '~';
  case 0x819F: // SJIS ◆
  case 0x25C6:
    return 0x25C6;
  case 0x81A0: // SJIS ◇
  case 0x25C7:
    return 0x25C7;
  case 0x8169: // SJIS full-width (
  case 0xFF08:
    return '(';
  case 0x816A: // SJIS full-width )
  case 0xFF09:
    return ')';
  case 0x8163: // SJIS ellipsis
  case 0x2026:
  case 0xFF3F:
    return kHsxf4LatinLowLine;
  case 0x8193: // SJIS full-width %
  case 0xFF05:
    return '%';
  case 0x8195: // SJIS full-width &
  case 0xFF06:
    return '&';
  case 0x849F: // ADV32 ellipsis continuation dot
    return kHsxf4LatinLowLine;
  case 0x8165: // SJIS ‘
  case 0x8166: // SJIS ’
  case 0x2018:
  case 0x2019:
    return '\'';
  case 0x815B: // SJIS prolonged sound mark
  case 0x815C: // SJIS em dash
  case 0x2500: // box drawing light horizontal from JSON "──"
    return kHsxf4LatinLowLine;
  case 0x815D: // SJIS hyphen
  case 0x817C: // SJIS minus sign
  case 0x2010:
  case 0x2011:
  case 0x2012:
  case 0x2013:
  case 0x2014:
  case 0x2212:
    return '-';
  case '`':
    return '~'; // ∿ proxy – tilde renders at mid x-height (wave appearance)
  case 0x81F4: // SJIS music note ♪
  case 0x266A:
    return 0x266A;
  case 0x81F5: // SJIS beamed eighth notes ♫
  case 0x266B:
    return 0x266B;
  case 0x81F6: // SJIS beamed sixteenth notes ♬
  case 0x266C:
    return 0x266C;
  case 0x83CA: // SJIS Greek small mu
  case 0x00B5: // Unicode micro sign, normalize to Greek small mu
  case 0x03BC:
    return kHsxf4GreekSmallMu;
  default:
    return ch;
  }
}

static inline UINT FirstCharA(LPCSTR s, UINT len) {
  return (s && len == 1) ? (UINT)(unsigned char)s[0] : 0;
}

static inline UINT FirstCharW(LPCWSTR s, UINT len) {
  return (s && len == 1) ? (UINT)s[0] : 0;
}

static void DrawLatinExtraPassExtTextOutA(HDC hdc, int x, int y, UINT options,
                                          const RECT *rc, LPCSTR s, UINT len,
                                          const INT *dx) {
  if (!hdc || !s || len == 0)
    return;
  UINT extraOptions = options & ~ETO_OPAQUE;
  Real_ExtTextOutA(hdc, x + 1, y, extraOptions, rc, s, len, dx);
  Real_ExtTextOutA(hdc, x, y + 1, extraOptions, rc, s, len, dx);
}

static void DrawLatinExtraPassExtTextOutW(HDC hdc, int x, int y, UINT options,
                                          const RECT *rc, LPCWSTR s, UINT len,
                                          const INT *dx) {
  if (!hdc || !s || len == 0)
    return;
  UINT extraOptions = options & ~ETO_OPAQUE;
  Real_ExtTextOutW(hdc, x + 1, y, extraOptions, rc, s, len, dx);
  Real_ExtTextOutW(hdc, x, y + 1, extraOptions, rc, s, len, dx);
}

static void DrawLatinExtraPassTextOutA(HDC hdc, int x, int y, LPCSTR s,
                                       int len) {
  if (!hdc || !s || len <= 0)
    return;
  Real_TextOutA(hdc, x + 1, y, s, len);
  Real_TextOutA(hdc, x, y + 1, s, len);
}

static void DrawLatinExtraPassTextOutW(HDC hdc, int x, int y, LPCWSTR s,
                                       int len) {
  if (!hdc || !s || len <= 0)
    return;
  Real_TextOutW(hdc, x + 1, y, s, len);
  Real_TextOutW(hdc, x, y + 1, s, len);
}

static void DrawHsxf4ExtraPauseDotA(HDC hdc, int x, int y) {
  if (!hdc || !g_hsxf4DrawExtraPauseDot || g_hsxf4ExtraPauseDotCount <= 0)
    return;
  const char dot = '.';
  for (int i = 0; i < g_hsxf4ExtraPauseDotCount; ++i) {
    int dotX = x + 6 * (i + 1);
    DrawLatinExtraPassTextOutA(hdc, dotX, y, &dot, 1);
    Real_TextOutA(hdc, dotX, y, &dot, 1);
  }
}

static void DrawHsxf4ExtraPauseDotW(HDC hdc, int x, int y) {
  if (!hdc || !g_hsxf4DrawExtraPauseDot || g_hsxf4ExtraPauseDotCount <= 0)
    return;
  const wchar_t dot = L'.';
  for (int i = 0; i < g_hsxf4ExtraPauseDotCount; ++i) {
    int dotX = x + 6 * (i + 1);
    DrawLatinExtraPassTextOutW(hdc, dotX, y, &dot, 1);
    Real_TextOutW(hdc, dotX, y, &dot, 1);
  }
}

static void DrawHsxf4ExtraPauseDotExtA(HDC hdc, int x, int y, UINT options,
                                       const RECT *rc) {
  if (!hdc || !g_hsxf4DrawExtraPauseDot || g_hsxf4ExtraPauseDotCount <= 0)
    return;
  const char dot = '.';
  for (int i = 0; i < g_hsxf4ExtraPauseDotCount; ++i) {
    int dotX = x + 6 * (i + 1);
    DrawLatinExtraPassExtTextOutA(hdc, dotX, y, options, rc, &dot, 1, NULL);
    Real_ExtTextOutA(hdc, dotX, y, options, rc, &dot, 1, NULL);
  }
}

static void DrawHsxf4ExtraPauseDotExtW(HDC hdc, int x, int y, UINT options,
                                       const RECT *rc) {
  if (!hdc || !g_hsxf4DrawExtraPauseDot || g_hsxf4ExtraPauseDotCount <= 0)
    return;
  const wchar_t dot = L'.';
  for (int i = 0; i < g_hsxf4ExtraPauseDotCount; ++i) {
    int dotX = x + 6 * (i + 1);
    DrawLatinExtraPassExtTextOutW(hdc, dotX, y, options, rc, &dot, 1, NULL);
    Real_ExtTextOutW(hdc, dotX, y, options, rc, &dot, 1, NULL);
  }
}

struct ScopedHsxf4TextRender {
  HDC hdc = NULL;
  int saved = 0;
  HFONT oldFont = NULL;

  explicit ScopedHsxf4TextRender(HDC dc) : hdc(dc) {
    if (!hdc || !InHsxf4LatinRender())
      return;
    saved = SaveDC(hdc);

    RECT clip = {};
    int clipRes = GetClipBox(hdc, &clip);
    if (clipRes != ERROR && clipRes != NULLREGION) {
      SelectClipRgn(hdc, NULL);
      IntersectClipRect(hdc, clip.left - 8, clip.top - 8, clip.right + 8,
                        clip.bottom + 14);
    }

    oldFont = SelectHsxf4LatinFont(hdc);
  }

  ~ScopedHsxf4TextRender() {
    if (!hdc)
      return;
    if (oldFont)
      SelectObject(hdc, oldFont);
    if (saved)
      RestoreDC(hdc, saved);
  }
};

static const RECT *ExpandHsxf4TextRect(const RECT *src, RECT *dst) {
  if (!src || !dst || !InHsxf4LatinRender())
    return src;
  *dst = *src;
  dst->left -= 4;
  dst->top -= 6;
  dst->right += 4;
  dst->bottom += 16;
  return dst;
}

static bool IsLikelyChoiceTextArea(int x, int y) {
  return x >= 40 && x <= 760 && y >= 90 && y <= 380;
}

static bool IsLikelyChoiceLatinA(LPCSTR text, UINT len) {
  if (!text || len == 0)
    return false;
  bool hasAlpha = false;
  for (UINT i = 0; i < len; ++i) {
    unsigned char ch = (unsigned char)text[i];
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
      hasAlpha = true;
      break;
    }
  }
  return hasAlpha && !IsUiElement(text, len);
}

static bool IsLikelyChoiceLatinW(LPCWSTR text, UINT len) {
  if (!text || len == 0)
    return false;
  bool hasAlpha = false;
  for (UINT i = 0; i < len; ++i) {
    wchar_t ch = text[i];
    if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
        (ch >= 0xFF21 && ch <= 0xFF3A) || (ch >= 0xFF41 && ch <= 0xFF5A)) {
      hasAlpha = true;
      break;
    }
  }
  return hasAlpha;
}

static LPCSTR NormalizeChoiceUiSpacesA(LPCSTR text, UINT *lenInOut,
                                       bool *changedOut = NULL) {
  if (changedOut)
    *changedOut = false;
  if (!text || !lenInOut || *lenInOut == 0)
    return text;

  UINT len = *lenInOut;
  static thread_local char s_choiceText[512];
  UINT out = 0;
  bool changed = false;
  bool prevWasMarker = false;
  for (UINT i = 0; i < len && out < (UINT)(sizeof(s_choiceText) - 1); ++i) {
    unsigned char ch = (unsigned char)text[i];
    if (prevWasMarker && (ch == ' ' || ch == '~' || ch == '^')) {
      prevWasMarker = false;
      changed = true;
      continue;
    }
    if (ch == 0x81 && i + 1 < len &&
        (unsigned char)text[i + 1] == 0x9B) {
      if (out + 2 >= (UINT)(sizeof(s_choiceText) - 1))
        break;
      s_choiceText[out++] = text[i];
      s_choiceText[out++] = text[i + 1];
      ++i;
      prevWasMarker = true;
      continue;
    }
    if (ch == 0x81 && i + 1 < len &&
        (unsigned char)text[i + 1] == 0x40) {
      if (prevWasMarker) {
        prevWasMarker = false;
        ++i;
        changed = true;
        continue;
      }
      s_choiceText[out++] = ' ';
      ++i;
      changed = true;
      continue;
    }
    if (ch == '~' || ch == '^') {
      s_choiceText[out++] = ' ';
      changed = true;
      prevWasMarker = false;
      continue;
    }
    prevWasMarker = (out == 0 && (ch == 'o' || ch == 'O'));
    s_choiceText[out++] = text[i];
  }

  if (!changed)
    return text;

  s_choiceText[out] = '\0';
  *lenInOut = out;
  if (changedOut)
    *changedOut = true;
  return s_choiceText;
}

static LPCSTR HideChoiceSpacePlaceholderA(LPCSTR text, UINT len) {
  if (!text || len == 0)
    return text;
  bool hasPlaceholder = false;
  for (UINT i = 0; i < len; ++i) {
    if (text[i] == '~') {
      hasPlaceholder = true;
      break;
    }
  }
  if (!hasPlaceholder)
    return text;

  static thread_local char s_choiceText[512];
  UINT n = len < (UINT)(sizeof(s_choiceText) - 1)
               ? len
               : (UINT)(sizeof(s_choiceText) - 1);
  for (UINT i = 0; i < n; ++i)
    s_choiceText[i] = (text[i] == '~') ? ' ' : text[i];
  s_choiceText[n] = '\0';
  return s_choiceText;
}

static LPCWSTR NormalizeChoiceUiSpacesW(LPCWSTR text, UINT *lenInOut,
                                        bool *changedOut = NULL) {
  if (changedOut)
    *changedOut = false;
  if (!text || !lenInOut || *lenInOut == 0)
    return text;

  UINT len = *lenInOut;
  bool hasPlaceholder = false;
  for (UINT i = 0; i < len; ++i) {
    if (text[i] == L'~' || text[i] == L'^' || text[i] == 0x3000 ||
        ((text[i] == 0x25CB || text[i] == L'o' || text[i] == L'O') &&
         i + 1 < len &&
         (text[i + 1] == L' ' || text[i + 1] == L'~' ||
          text[i + 1] == L'^' ||
          text[i + 1] == 0x3000))) {
      hasPlaceholder = true;
      break;
    }
  }
  if (!hasPlaceholder)
    return text;

  static thread_local wchar_t s_choiceText[512];
  UINT n = len < (UINT)((sizeof(s_choiceText) / sizeof(s_choiceText[0])) - 1)
               ? len
               : (UINT)((sizeof(s_choiceText) / sizeof(s_choiceText[0])) - 1);
  UINT out = 0;
  bool prevWasMarker = false;
  for (UINT i = 0; i < n && out < (UINT)((sizeof(s_choiceText) / sizeof(s_choiceText[0])) - 1); ++i) {
    wchar_t ch = text[i];
    if (prevWasMarker &&
        (ch == L' ' || ch == L'~' || ch == L'^' || ch == 0x3000)) {
      prevWasMarker = false;
      continue;
    }
    s_choiceText[out++] =
        (ch == L'~' || ch == L'^' || ch == 0x3000) ? L' ' : ch;
    prevWasMarker = (ch == 0x25CB || (out == 1 && (ch == L'o' || ch == L'O')));
  }
  s_choiceText[out] = L'\0';
  *lenInOut = out;
  if (changedOut)
    *changedOut = true;
  return s_choiceText;
}

static LPCWSTR HideChoiceSpacePlaceholderW(LPCWSTR text, UINT len) {
  if (!text || len == 0)
    return text;
  bool hasPlaceholder = false;
  for (UINT i = 0; i < len; ++i) {
    if (text[i] == L'~') {
      hasPlaceholder = true;
      break;
    }
  }
  if (!hasPlaceholder)
    return text;

  static thread_local wchar_t s_choiceText[512];
  UINT n = len < (UINT)((sizeof(s_choiceText) / sizeof(s_choiceText[0])) - 1)
               ? len
               : (UINT)((sizeof(s_choiceText) / sizeof(s_choiceText[0])) - 1);
  for (UINT i = 0; i < n; ++i)
    s_choiceText[i] = (text[i] == L'~') ? L' ' : text[i];
  s_choiceText[n] = L'\0';
  return s_choiceText;
}

static void LogChoiceRenderProbeA(const char *hookName, HDC hdc, int x, int y,
                                  LPCSTR text, UINT len, const INT *dx) {
  static volatile LONG s_count = 0;
  LONG n = InterlockedIncrement(&s_count);
  if (n > 120 || !hookName || !text)
    return;

  TEXTMETRICA tm = {};
  BOOL hasTm = hdc ? GetTextMetricsA(hdc, &tm) : FALSE;
  int charExtra = hdc ? GetTextCharacterExtra(hdc) : 0;
  char sample[96] = {};
  UINT sampleLen = len < (UINT)(sizeof(sample) - 1) ? len : (UINT)(sizeof(sample) - 1);
  for (UINT i = 0; i < sampleLen; ++i) {
    unsigned char ch = (unsigned char)text[i];
    sample[i] = (ch >= 0x20 && ch <= 0x7E) ? (char)ch : '.';
  }
  char dxText[96] = {};
  if (dx) {
    char *out = dxText;
    size_t remain = sizeof(dxText);
    UINT dxLen = len < 8 ? len : 8;
    for (UINT i = 0; i < dxLen && remain > 1; ++i) {
      int written = sprintf_s(out, remain, "%d ", dx[i]);
      if (written <= 0 || (size_t)written >= remain)
        break;
      out += written;
      remain -= (size_t)written;
    }
  }
  char line[384];
  sprintf_s(line,
            "[GDI-CHOICE-A] #%ld hook=%s x=%d y=%d len=%u tmH=%d extra=%d "
            "txt='%s' dx=%s",
            (long)n, hookName, x, y, (unsigned)len,
            hasTm ? tm.tmHeight : -1, charExtra, sample, dx ? dxText : "-");
  LogHsxf4Probe(line);
}

static void LogChoiceRenderProbeW(const char *hookName, HDC hdc, int x, int y,
                                  LPCWSTR text, UINT len, const INT *dx) {
  static volatile LONG s_count = 0;
  LONG n = InterlockedIncrement(&s_count);
  if (n > 120 || !hookName || !text)
    return;

  TEXTMETRICA tm = {};
  BOOL hasTm = hdc ? GetTextMetricsA(hdc, &tm) : FALSE;
  int charExtra = hdc ? GetTextCharacterExtra(hdc) : 0;
  char sample[96] = {};
  UINT sampleLen = len < (UINT)(sizeof(sample) - 1) ? len : (UINT)(sizeof(sample) - 1);
  for (UINT i = 0; i < sampleLen; ++i) {
    wchar_t ch = text[i];
    sample[i] = (ch >= 0x20 && ch <= 0x7E) ? (char)ch : '.';
  }
  char dxText[96] = {};
  if (dx) {
    char *out = dxText;
    size_t remain = sizeof(dxText);
    UINT dxLen = len < 8 ? len : 8;
    for (UINT i = 0; i < dxLen && remain > 1; ++i) {
      int written = sprintf_s(out, remain, "%d ", dx[i]);
      if (written <= 0 || (size_t)written >= remain)
        break;
      out += written;
      remain -= (size_t)written;
    }
  }
  char line[384];
  sprintf_s(line,
            "[GDI-CHOICE-W] #%ld hook=%s x=%d y=%d len=%u tmH=%d extra=%d "
            "txt='%s' dx=%s",
            (long)n, hookName, x, y, (unsigned)len,
            hasTm ? tm.tmHeight : -1, charExtra, sample, dx ? dxText : "-");
  LogHsxf4Probe(line);
}

static UINT ConvertFullWidthToHalfWidth(UINT sjisChar);

static HFONT SelectChoiceTextFont(HDC hdc) {
  if (!hdc)
    return NULL;
  HFONT font = GetChoiceUiFont();
  return font ? (HFONT)SelectObject(hdc, font) : NULL;
}

static bool IsLikelyPopupUiTextPoint(int x, int y) {
  bool inConfirmPopup = x >= 260 && x <= 545 && y >= 230 && y <= 365;
  bool inAdvMenu = x >= 570 && x <= 795 && y >= 55 && y <= 250;
  return inConfirmPopup || inAdvMenu;
}

static bool IsLikelyPopupUiTextRect(const RECT *rc) {
  if (!rc)
    return false;
  int cx = (rc->left + rc->right) / 2;
  int cy = (rc->top + rc->bottom) / 2;
  return IsLikelyPopupUiTextPoint(rc->left, rc->top) ||
         IsLikelyPopupUiTextPoint(cx, cy);
}

static bool IsLikelyPopupUiLatinA(LPCSTR text, UINT len) {
  if (!text || len == 0 || len > 40)
    return false;
  bool hasAlpha = false;
  for (UINT i = 0; i < len; ++i) {
    unsigned char ch = (unsigned char)text[i];
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
      hasAlpha = true;
    } else if (ch >= 0x80) {
      return false;
    }
  }
  return hasAlpha;
}

static bool IsLikelyPopupUiLatinW(LPCWSTR text, UINT len) {
  if (!text || len == 0 || len > 40)
    return false;
  bool hasAlpha = false;
  for (UINT i = 0; i < len; ++i) {
    wchar_t ch = text[i];
    if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
        (ch >= 0xFF21 && ch <= 0xFF3A) || (ch >= 0xFF41 && ch <= 0xFF5A)) {
      hasAlpha = true;
    } else if (ch > 0x007F && !(ch >= 0xFF01 && ch <= 0xFF5E)) {
      return false;
    }
  }
  return hasAlpha;
}

static HFONT GetPopupUiFont() {
  if (g_popupUiFont)
    return g_popupUiFont;

  const wchar_t *face = L"Verdana";
  if (!IsInstalledFontFace(face))
    face = IsInstalledFontFace(L"Tahoma") ? L"Tahoma" : L"MS PGothic";

  g_popupUiFont = CreateFontW(
      -12, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
      CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
  return g_popupUiFont;
}

static HFONT GetChoiceUiFont() {
  if (g_choiceUiFont)
    return g_choiceUiFont;

  const wchar_t *face = L"Verdana";
  if (!IsInstalledFontFace(face))
    face = IsInstalledFontFace(L"Tahoma") ? L"Tahoma" : L"MS PGothic";

  g_choiceUiFont = CreateFontW(
      -8, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
      CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
  return g_choiceUiFont;
}

static HFONT SelectPopupUiTextFont(HDC hdc) {
  if (!hdc)
    return NULL;
  HFONT font = GetPopupUiFont();
  return font ? (HFONT)SelectObject(hdc, font) : NULL;
}

static int GetPopupUiGlyphAdvance(UINT ch) {
  if (ch >= 0xFF01 && ch <= 0xFF5E)
    ch = ch - 0xFEE0;
  else if (ch == 0x3000)
    ch = ' ';
  if (ch > 0xFF) {
    UINT converted = ConvertFullWidthToHalfWidth(ch);
    if (converted <= 0xFF)
      ch = converted;
  }
  if (ch == ' ')
    return 3;
  if (ch == '.' || ch == ',' || ch == '\'' || ch == '"' || ch == '!' ||
      ch == '?' || ch == ':' || ch == ';' || ch == '-')
    return 4;
  if (ch == 'i' || ch == 'l' || ch == 'I')
    return 4;
  if (ch == 'm' || ch == 'w' || ch == 'M' || ch == 'W')
    return 8;
  return 6;
}

static void TunePopupUiGlyphMetrics(UINT ch, GLYPHMETRICS *gm) {
  if (!gm)
    return;
  int advance = GetPopupUiGlyphAdvance(ch);
  int minCell = (int)gm->gmBlackBoxX + 1;
  if (advance < minCell && ch != ' ')
    advance = minCell;
  gm->gmCellIncX = advance;
  gm->gmCellIncY = 0;
  if (gm->gmptGlyphOrigin.x > 1)
    gm->gmptGlyphOrigin.x = 1;
  if (gm->gmptGlyphOrigin.x < -1)
    gm->gmptGlyphOrigin.x = -1;
}

static void StartChoiceGlyphRun(HDC hdc) {
  if (!hdc)
    return;
  g_choiceGlyphRunHdc = hdc;
  g_choiceGlyphRunBudget = 512;
  g_choiceGlyphRunTick = GetTickCount();
  g_choiceGlyphWordStart = true;
}

static bool InChoiceGlyphRun(HDC hdc) {
  if (!hdc || g_choiceGlyphRunHdc != hdc || g_choiceGlyphRunBudget <= 0)
    return false;
  if (GetTickCount() - g_choiceGlyphRunTick > 4000) {
    g_choiceGlyphRunHdc = NULL;
    g_choiceGlyphRunBudget = 0;
    return false;
  }
  return true;
}

static void ConsumeChoiceGlyphRun() {
  if (g_choiceGlyphRunBudget > 0)
    --g_choiceGlyphRunBudget;
}

static UINT DecodeChoiceRunGlyph(UINT ch) {
  if (ch == 0x5E)
    return ' ';
  if (ch >= 0x01 && ch <= 0x5E)
    return ch + 0x20;
  return ch;
}

static UINT NormalizeChoiceRunGlyph(UINT ch) {
  bool marker = (ch == 0x819B || ch == 0x25CB || ch == 0x617B);
  bool spacer = (ch == 0x8140 || ch == 0x3000 || ch == 0x8145 ||
                 ch == 0x30FB || ch == 0x6120 || ch == '~' || ch == ' ');
  if (marker || spacer) {
    g_choiceGlyphWordStart = true;
    return ch;
  }

  if (ch >= 'A' && ch <= 'Z') {
    if (!g_choiceGlyphWordStart)
      ch = ch - 'A' + 'a';
    g_choiceGlyphWordStart = false;
    return ch;
  }

  if (ch >= 'a' && ch <= 'z') {
    g_choiceGlyphWordStart = false;
    return ch;
  }

  if (ch >= '0' && ch <= '9')
    g_choiceGlyphWordStart = false;
  else if (ch == '.' || ch == ',' || ch == '?' || ch == '!' || ch == '-' ||
           ch == '\'' || ch == '"')
    g_choiceGlyphWordStart = false;
  return ch;
}

static bool IsChoiceMarkerGlyph(UINT ch) {
  return ch == 0x819B || ch == 0x25CB || ch == 0x617B;
}

static bool IsChoiceSpacerGlyph(UINT ch) {
  return ch == 0x8140 || ch == 0x3000 || ch == 0x8145 || ch == 0x30FB ||
         ch == 0x6120 || ch == '~';
}

static bool IsSjisLeadByteForChoice(UINT ch) {
  unsigned char b = (unsigned char)(ch & 0xFF);
  return (ch & 0xFF00) == 0 &&
         ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC));
}

static bool IsSjisTrailByteForChoice(UINT ch) {
  unsigned char b = (unsigned char)(ch & 0xFF);
  return (ch & 0xFF00) == 0 &&
         ((b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFC));
}

static DWORD ReturnEmptyChoiceGlyph(LPGLYPHMETRICS lpgm, DWORD cjBuffer,
                                    LPVOID pvBuffer) {
  if (lpgm) {
    lpgm->gmBlackBoxX = 0;
    lpgm->gmBlackBoxY = 0;
    lpgm->gmptGlyphOrigin.x = 0;
    lpgm->gmptGlyphOrigin.y = 0;
    lpgm->gmCellIncX = 0;
    lpgm->gmCellIncY = 0;
  }
  if (pvBuffer && cjBuffer > 0)
    memset(pvBuffer, 0, cjBuffer);
  return 0;
}

static void LogRawChoiceGlyphProbeA(const char *hookName, HDC hdc, UINT uChar) {
  static volatile LONG s_count = 0;
  if (g_fontManager.GetLanguage() == 0 || g_inBacklogRender)
    return;
  if (!((uChar >= 0x20 && uChar <= 0x7E) || IsChoiceMarkerGlyph(uChar) ||
        IsSjisLeadByteForChoice(uChar) || IsSjisTrailByteForChoice(uChar)))
    return;
  LONG n = InterlockedIncrement(&s_count);
  if (n > 500)
    return;

  POINT pt = {};
  BOOL hasPt = hdc ? GetCurrentPositionEx(hdc, &pt) : FALSE;
  RECT rc = {};
  int clipRes = hdc ? GetClipBox(hdc, &rc) : ERROR;
  TEXTMETRICA tm = {};
  BOOL hasTm = hdc ? GetTextMetricsA(hdc, &tm) : FALSE;
  char line[384];
  sprintf_s(line,
            "[RAW-GLYPH-A] #%ld hook=%s ch=%04X '%c' pt=(%ld,%ld) hasPt=%d "
            "clip=(%ld,%ld,%ld,%ld) clipRes=%d tmH=%d extra=%d hsxf4=%d run=%d",
            (long)n, hookName ? hookName : "-", (unsigned)uChar,
            (uChar >= 0x20 && uChar <= 0x7E) ? (char)uChar : '.',
            pt.x, pt.y, hasPt ? 1 : 0, rc.left, rc.top, rc.right, rc.bottom,
            clipRes, hasTm ? tm.tmHeight : -1,
            hdc ? GetTextCharacterExtra(hdc) : 0, InHsxf4LatinRender() ? 1 : 0,
            InChoiceGlyphRun(hdc) ? 1 : 0);
  LogHsxf4Probe(line);
}

static void LogRawChoiceGlyphProbeW(const char *hookName, HDC hdc, UINT uChar) {
  static volatile LONG s_count = 0;
  if (g_fontManager.GetLanguage() == 0 || g_inBacklogRender)
    return;
  if (!((uChar >= 0x20 && uChar <= 0x7E) || uChar == 0x25CB ||
        uChar == 0x3000))
    return;
  LONG n = InterlockedIncrement(&s_count);
  if (n > 500)
    return;

  POINT pt = {};
  BOOL hasPt = hdc ? GetCurrentPositionEx(hdc, &pt) : FALSE;
  RECT rc = {};
  int clipRes = hdc ? GetClipBox(hdc, &rc) : ERROR;
  TEXTMETRICA tm = {};
  BOOL hasTm = hdc ? GetTextMetricsA(hdc, &tm) : FALSE;
  char line[384];
  sprintf_s(line,
            "[RAW-GLYPH-W] #%ld hook=%s ch=%04X '%c' pt=(%ld,%ld) hasPt=%d "
            "clip=(%ld,%ld,%ld,%ld) clipRes=%d tmH=%d extra=%d hsxf4=%d run=%d",
            (long)n, hookName ? hookName : "-", (unsigned)uChar,
            (uChar >= 0x20 && uChar <= 0x7E) ? (char)uChar : '.',
            pt.x, pt.y, hasPt ? 1 : 0, rc.left, rc.top, rc.right, rc.bottom,
            clipRes, hasTm ? tm.tmHeight : -1,
            hdc ? GetTextCharacterExtra(hdc) : 0, InHsxf4LatinRender() ? 1 : 0,
            InChoiceGlyphRun(hdc) ? 1 : 0);
  LogHsxf4Probe(line);
}

static int GetChoiceGlyphAdvance(UINT ch) {
  if (IsChoiceSpacerGlyph(ch) || ch == ' ')
    return 1;
  if (ch == '.' || ch == ',' || ch == '\'' || ch == '"')
    return 2;
  if (ch == '?' || ch == '!' || ch == ':' || ch == ';' || ch == '-' ||
      ch == '~')
    return 3;
  if (IsChoiceMarkerGlyph(ch))
    return 8;
  if (ch == 'i' || ch == 'l' || ch == 'I')
    return 3;
  if (ch == 'j' || ch == 't' || ch == 'f' || ch == 'r')
    return 4;
  if (ch == 'm' || ch == 'w' || ch == 'M' || ch == 'W')
    return 6;
  return 4;
}

static DWORD RenderChoiceSpacerGlyph(HDC hdc, UINT uChar, UINT fuFormat,
                                     LPGLYPHMETRICS lpgm, DWORD cjBuffer,
                                     LPVOID pvBuffer, const MAT2 *lpmat2) {
  (void)hdc;
  (void)uChar;
  (void)fuFormat;
  (void)lpmat2;
  if (pvBuffer && cjBuffer > 0)
    memset(pvBuffer, 0, cjBuffer);
  if (lpgm) {
    lpgm->gmBlackBoxX = 0;
    lpgm->gmBlackBoxY = 0;
    lpgm->gmptGlyphOrigin.x = 0;
    lpgm->gmptGlyphOrigin.y = 0;
    lpgm->gmCellIncX = 1;
    lpgm->gmCellIncY = 0;
  }
  return 0;
}

static DWORD RenderChoiceSpacerGlyphW(HDC hdc, UINT uChar, UINT fuFormat,
                                      LPGLYPHMETRICS lpgm, DWORD cjBuffer,
                                      LPVOID pvBuffer, const MAT2 *lpmat2) {
  (void)hdc;
  (void)uChar;
  (void)fuFormat;
  (void)lpmat2;
  if (pvBuffer && cjBuffer > 0)
    memset(pvBuffer, 0, cjBuffer);
  if (lpgm) {
    lpgm->gmBlackBoxX = 0;
    lpgm->gmBlackBoxY = 0;
    lpgm->gmptGlyphOrigin.x = 0;
    lpgm->gmptGlyphOrigin.y = 0;
    lpgm->gmCellIncX = 1;
    lpgm->gmCellIncY = 0;
  }
  return 0;
}

static bool IsLikelyChoiceGlyphContext(HDC hdc) {
  if (!hdc || g_inBacklogRender || g_fontManager.GetLanguage() == 0)
    return false;
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) ||
      IsVersionInfoDialogDrawContext(hdc))
    return false;

  RECT rc = {};
  int clipRes = GetClipBox(hdc, &rc);
  if (clipRes == ERROR || clipRes == NULLREGION)
    return false;
  int clipW = rc.right - rc.left;
  int clipH = rc.bottom - rc.top;
  if (clipW < 500 || clipH < 350)
    return false;

  POINT pt = {};
  bool hasPt = !!GetCurrentPositionEx(hdc, &pt);
  int relX = hasPt ? (pt.x - rc.left) : 160;
  int relY = hasPt ? (pt.y - rc.top) : 210;

  // Choice options are rendered in the upper/middle overlay. The textbox lives
  // in the lower ~35% of the screen, so never tune glyphs there.
  if (hasPt && relY >= (clipH * 55) / 100)
    return false;
  if (hasPt && (relX < 80 || relX > clipW - 80 || relY < 110 || relY > 360))
    return false;

  TEXTMETRICA tm = {};
  if (!Real_GetTextMetricsA(hdc, &tm))
    return false;

  // The choice renderer uses a much larger non-HSXF4 glyph font than the
  // dialogue textbox. Limit this branch to that oversized path.
  return tm.tmHeight >= 20 && tm.tmHeight <= 70;
}

static bool IsChoiceLargeFontContext(HDC hdc) {
  if (!hdc || g_inBacklogRender || g_fontManager.GetLanguage() == 0)
    return false;
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) ||
      IsVersionInfoDialogDrawContext(hdc))
    return false;
  TEXTMETRICA tm = {};
  if (!Real_GetTextMetricsA(hdc, &tm))
    return false;
  return tm.tmHeight >= 20 && tm.tmHeight <= 70;
}

static bool IsReplayRawGlyphContext(HDC hdc) {
  if (!hdc || g_inBacklogRender || g_fontManager.GetLanguage() == 0)
    return false;
  if (InHsxf4LatinRender() || InPopupUiLatinRender() ||
      InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) ||
      IsVersionInfoDialogDrawContext(hdc))
    return false;

  RECT rc = {};
  int clipRes = GetClipBox(hdc, &rc);
  if (clipRes == ERROR || clipRes == NULLREGION)
    return false;

  POINT pt = {};
  bool hasPt = !!GetCurrentPositionEx(hdc, &pt);
  int relX = hasPt ? (pt.x - rc.left) : 360;
  int relY = hasPt ? (pt.y - rc.top) : 220;

  // Scene Replay titles are drawn in the two-column grid, not in the ADV
  // textbox, popup, or choice overlays. Keep this guard positional so the
  // compact replay font cannot bleed into other UI paths.
  if (hasPt && (relX < 220 || relX > 760 || relY < 90 || relY > 540))
    return false;

  TEXTMETRICA tm = {};
  if (!Real_GetTextMetricsA(hdc, &tm))
    return false;
  return tm.tmHeight >= 12 && tm.tmHeight <= 70;
}

static int GetReplayRawGlyphAdvance(UINT ch) {
  if (ch == ' ' || ch == '~' || ch == '^' || ch == 0x3000 || ch == 0x8140)
    return 2;
  if (ch == '.' || ch == ',' || ch == '\'' || ch == '"' || ch == ':' ||
      ch == ';' || ch == '!' || ch == '?' || ch == '-' || ch == '(' ||
      ch == ')')
    return 3;
  if (ch == 'i' || ch == 'l' || ch == 'I')
    return 3;
  if (ch == 'm' || ch == 'w' || ch == 'M' || ch == 'W')
    return 6;
  return 5;
}

static void TuneReplayRawGlyphMetrics(UINT ch, GLYPHMETRICS *gm) {
  if (!gm)
    return;
  int adv = GetReplayRawGlyphAdvance(ch);
  if (ch == ' ' || ch == '~' || ch == '^' || ch == 0x3000 || ch == 0x8140) {
    gm->gmBlackBoxX = 0;
    gm->gmBlackBoxY = 0;
    gm->gmptGlyphOrigin.x = 0;
    gm->gmCellIncX = adv;
    return;
  }
  int minCell = (int)gm->gmBlackBoxX + 1;
  if (adv < minCell)
    adv = minCell;
  if (gm->gmCellIncX > adv)
    gm->gmCellIncX = adv;
  if (gm->gmptGlyphOrigin.x > 1)
    gm->gmptGlyphOrigin.x = 1;
  if (gm->gmptGlyphOrigin.x < -1)
    gm->gmptGlyphOrigin.x = -1;
}

static void TuneWideSpaceGlyphMetrics(GLYPHMETRICS *gm) {
  if (!gm)
    return;
  gm->gmBlackBoxX = 0;
  gm->gmCellIncX = 4;
  gm->gmptGlyphOrigin.x = 0;
}

static bool IsChoiceGlyphA(UINT ch) {
  return (ch >= 0x20 && ch <= 0x7E) || ch == 0x8140 || ch == 0x3000 ||
         ch == 0x819B || ch == 0x6120 || ch == 0x617B;
}

static bool IsChoiceGlyphW(UINT ch) {
  return (ch >= 0x20 && ch <= 0x7E) || ch == 0x3000 || ch == 0x25CB;
}

static void TuneChoiceGlyphMetrics(UINT ch, GLYPHMETRICS *gm) {
  if (!gm)
    return;

  bool isWideSpace = (ch == 0x8140 || ch == 0x3000 || ch == 0x8145 ||
                      ch == 0x30FB || ch == 0x6120);
  bool isAsciiSpace = (ch == ' ');
  bool isDot = (ch == '.');
  bool isPunct = (ch == ',' || ch == '?' || ch == '!' || ch == '\'' ||
                  ch == '"' || ch == ':' || ch == ';');
  bool isHyphen = (ch == '-');
  bool isCircle = (ch == 0x819B || ch == 0x25CB || ch == 0x617B);

  if (isWideSpace || isAsciiSpace) {
    gm->gmBlackBoxX = 0;
    gm->gmCellIncX = 1;
    gm->gmptGlyphOrigin.x = 0;
    return;
  }

  if (isDot) {
    if (gm->gmCellIncX > 2)
      gm->gmCellIncX = 2;
    gm->gmptGlyphOrigin.y -= 8;
  } else if (isPunct || isHyphen) {
    if (gm->gmCellIncX > 3)
      gm->gmCellIncX = 3;
    if (isHyphen)
      gm->gmptGlyphOrigin.y -= 4;
  } else if (isCircle) {
    if (gm->gmCellIncX > 8)
      gm->gmCellIncX = 8;
  } else if (gm->gmCellIncX > 5) {
    gm->gmCellIncX = 5;
  }

  if (gm->gmptGlyphOrigin.x < -1)
    gm->gmptGlyphOrigin.x = -1;
}

// Hook ExtTextOutA - UI Text Rendering (e.g. Character Names in Backlog)
static BOOL WINAPI Hook_ExtTextOutA(HDC hdc, int x, int y, UINT options,
                                    const RECT *lprect, LPCSTR lpString, UINT c,
                                    const INT *lpDx) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_ExtTextOutA(hdc, x, y, options, lprect, lpString, c, lpDx);
  if (InHsxf4LatinRender()) {
    if (IsOnlyTextControlA(lpString, c))
      return TRUE;
    ScopedHsxf4TextRender renderGuard(hdc);
    RECT expanded = {};
    const RECT *drawRect = ExpandHsxf4TextRect(lprect, &expanded);
    int firstCh = FirstCharA(lpString, c);
    int drawX = AdjustHsxf4DrawX(x);
    int drawY = AdjustLatinPunctuationDrawY(y, firstCh);


    if (!InHsxf4UiRender())
      DrawLatinExtraPassExtTextOutA(hdc, drawX, drawY, options, drawRect, lpString,
                                    c, lpDx);
    BOOL result =
        Real_ExtTextOutA(hdc, drawX, drawY, options, drawRect, lpString, c, lpDx);
    DrawHsxf4ExtraPauseDotExtA(hdc, drawX, drawY, options, drawRect);
    return result;
  }
  if (!g_inBacklogRender && IsLikelyReplayTextArea(x, y) && lpString) {
    UINT len = c;
    int actualLen = lstrlenA(lpString);
    if ((len == 0 || (actualLen > 0 && actualLen > (int)len &&
                      actualLen < (int)len + 80)) &&
        actualLen > 0) {
      len = (UINT)actualLen;
    }
    UINT replayLen = len;
    if (const char *replayText =
            TranslateReplayTextA(lpString, len, &replayLen)) {
      return DrawReplayTextA(hdc, x, y, options, lprect, replayText, replayLen,
                             true);
    }
    if (IsReplayTitleTextPosition(x, y)) {
      replayLen = len;
      if (const char *displayText =
              TranslateReplayDisplayTextA(lpString, len, &replayLen)) {
        return DrawReplayTextA(hdc, x, y, options, lprect, displayText,
                               replayLen, true);
      }
    }
    if (IsReplayTitleTextPosition(x, y) &&
        IsLikelyDialogueLatinA(lpString, len)) {
      return DrawReplayTextA(hdc, x, y, options, lprect, lpString, len, true);
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      IsLikelyPopupUiTextPoint(x, y) && lpString) {
    UINT renderLen = c;
    int actualLen = lstrlenA(lpString);
    if ((renderLen == 0 || (actualLen > 0 && actualLen > (int)renderLen &&
                            actualLen < (int)renderLen + 80)) &&
        actualLen > 0) {
      renderLen = (UINT)actualLen;
    }
    LPCSTR renderStr =
        FilterSjisString(lpString, &renderLen, NULL, NULL, true, true);
    if (!renderStr)
      renderStr = lpString;
    if (IsLikelyPopupUiLatinA(renderStr, renderLen)) {
      HFONT oldFont = SelectPopupUiTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, 0);
      ScopedPopupUiLatinRender popupRender(true);
      BOOL result = Real_ExtTextOutA(hdc, x, y, options, lprect, renderStr,
                                     renderLen, NULL);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      IsLikelyChoiceTextArea(x, y) && lpString) {
    UINT len = c;
    int actualLen = lstrlenA(lpString);
    if ((len == 0 || (actualLen > 0 && actualLen > (int)len &&
                      actualLen < (int)len + 80)) &&
        actualLen > 0) {
      len = (UINT)actualLen;
    }
    UINT renderLen = len;
    INT *renderDx = NULL;
    LPCSTR renderStr =
        FilterSjisString(lpString, &renderLen, lpDx, &renderDx, true, true);
    if (!renderStr)
      renderStr = lpString;
    if (IsLikelyChoiceLatinA(renderStr, renderLen)) {
      bool choiceSpacesChanged = false;
      renderStr = NormalizeChoiceUiSpacesA(renderStr, &renderLen,
                                           &choiceSpacesChanged);
      if (choiceSpacesChanged)
        renderDx = NULL;
      LogChoiceRenderProbeA("ExtTextOutA", hdc, x, y, renderStr, renderLen,
                            renderDx ? renderDx : lpDx);
      HFONT oldFont = SelectChoiceTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, -2);
      const INT *safeDx = NULL;
      int drawY = AdjustLatinPunctuationDrawY(y, FirstCharA(renderStr, renderLen));
      BOOL result = Real_ExtTextOutA(hdc, x, drawY, options, lprect, renderStr,
                                     renderLen, safeDx);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender)
    return Real_ExtTextOutA(hdc, x, y, options, lprect, lpString, c, lpDx);
  if (!g_inBacklogRender && g_dialogueSafePassthrough) {
    UINT len = c;
    if (len == 0 && lpString)
      len = (UINT)lstrlenA(lpString);
    UINT renderLen = len;
    INT *renderDx = NULL;
    LPCSTR renderStr = FilterSjisString(lpString, &renderLen, lpDx, &renderDx,
                                        true, true);
    if (!renderStr)
      renderStr = lpString;
    bool isUi = IsUiElement(renderStr, renderLen);
    bool applyDialogueTuning =
        (g_fontManager.GetLanguage() != 0) && (renderLen > 0) && !isUi;
    bool likelyDialogueLatin =
        applyDialogueTuning &&
        (IsLikelyDialogueLatinA(renderStr, renderLen) ||
         IsLikelyDialogueTextboxArea(hdc, y) || renderLen >= 6);
    HFONT oldFont = NULL;
    bool usedConfiguredDialogueFont = false;
    int oldCharExtra = 0;
    if (applyDialogueTuning) {
      HFONT customFont = g_fontManager.GetDialogueFont(g_fontManager.GetDialogueFontSize());
      if (customFont) {
        oldFont = (HFONT)SelectObject(hdc, customFont);
        usedConfiguredDialogueFont = true;
      } else {
        TEXTMETRICA tm;
        if (Real_GetTextMetricsA(hdc, &tm)) {
          int tunedH = (tm.tmHeight * 90 + 50) / 100;
          if (tunedH < 12)
            tunedH = 12;
          customFont = g_fontManager.GetEngineScaledFont(tunedH);
          if (customFont)
            oldFont = (HFONT)SelectObject(hdc, customFont);
        }
      }
      oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, -4);
    }
    int drawX = x;
    if (likelyDialogueLatin && renderStr && renderLen > 0) {
      drawX =
          AdjustDialogueDrawX(hdc, x, y, renderLen, (unsigned char)renderStr[0]);
    }
    if (ShouldSkipSyntheticSpaceA(hdc, y, renderStr, renderLen,
                                  applyDialogueTuning)) {
      if (applyDialogueTuning) {
        SetTextCharacterExtra(hdc, oldCharExtra);
        if (oldFont)
          SelectObject(hdc, oldFont);
      }
      return TRUE;
    }
    const INT *safeDx = (renderDx != NULL) ? renderDx : lpDx;
    float fitScale = 0.38f;
    if (applyDialogueTuning) {
      safeDx = CompressDxForDialogueA(safeDx, renderLen, renderStr, 24, 6);
      fitScale =
          ComputeFitScaleA(hdc, drawX, lprect, renderStr, renderLen, 0.38f);
    }
    if (applyDialogueTuning)
      drawX = CompressDialogueRunXSafe(true, drawX, y);
    float yScale =
        applyDialogueTuning ? (usedConfiguredDialogueFont ? 1.0f
                                                          : GetDialogueSizeScaleY())
                            : 1.0f;
    ScopedDialogueXScale scaleGuard(hdc, applyDialogueTuning, drawX, y, fitScale,
                                    yScale);
    if (likelyDialogueLatin) {
      LogRenderPathTelemetryA("ExtTextOutA_SAFE", hdc, drawX, y, (int)renderLen,
                              true, true, true, true, safeDx, renderStr);
    }
    int drawY = AdjustLatinPunctuationDrawY(y, FirstCharA(renderStr, renderLen));
    DrawLatinExtraPassExtTextOutA(hdc, drawX, drawY, options, lprect, renderStr,
                                  renderLen, safeDx);
    BOOL result = Real_ExtTextOutA(hdc, drawX, drawY, options, lprect, renderStr,
                                   renderLen, safeDx);
    if (applyDialogueTuning) {
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
    }
    return result;
  }

  UINT originalC = c;
  int actualLen = lstrlenA(lpString);
  if (c > 0 && actualLen > c && actualLen < c + 150) {
    c = actualLen;
  }

  UINT renderLen = c;
  INT *renderDx = NULL;
  UINT latinProbeLen = c;
  bool likelyByArea =
      (!g_inBacklogRender && IsLikelyDialogueTextboxArea(hdc, y));
  bool likelyDialogueLatinOrig =
      g_inBacklogRender || likelyByArea ||
      IsLikelyDialogueLatinA(lpString, latinProbeLen);
  bool performTranslation =
      (!(g_inBacklogRender && g_fontManager.GetDisableBacklogTranslation()) &&
       likelyDialogueLatinOrig);
  // Need to pass the original C so FilterSjisString can read lpDx fully safely
  LPCSTR renderStr = FilterSjisString(lpString, &renderLen, lpDx, &renderDx,
                                      performTranslation, performTranslation);
  bool useDialogueTuning =
      (!g_inBacklogRender &&
       (likelyByArea || IsLikelyDialogueLatinA(renderStr, renderLen)));
  const INT *finalDx = (!g_inBacklogRender ? NULL : lpDx);
  if (!g_inBacklogRender && (likelyByArea || useDialogueTuning) &&
      ShouldSamplePathTelemetry()) {
    LogRenderPathTelemetryA("ExtTextOutA", hdc, x, y, (int)renderLen,
                            likelyByArea, likelyDialogueLatinOrig,
                            useDialogueTuning, performTranslation, finalDx,
                            renderStr);
  }

  if (!IsUiElement(lpString, c) && !g_inBacklogRender) {
    LogTextTelemetry("ExtTextOutA (IN)", lpString, c, lpDx, NULL);
    LogTextTelemetry("ExtTextOutA (OUT)", renderStr, renderLen, finalDx, NULL);
  }

  static thread_local HDC s_lastHdc_ExtTextOut = NULL;
  static thread_local HFONT s_lastBgFont_ExtTextOut = NULL;
  static thread_local int s_lastTmHeight_ExtTextOut = 0;

  HFONT currFont = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
  if (hdc != s_lastHdc_ExtTextOut || currFont != s_lastBgFont_ExtTextOut ||
      s_lastHdc_ExtTextOut == NULL) {
    TEXTMETRICA tm;
    Real_GetTextMetricsA(hdc, &tm);
    s_lastTmHeight_ExtTextOut = tm.tmHeight;
    s_lastHdc_ExtTextOut = hdc;
    s_lastBgFont_ExtTextOut = currFont;
  }

  bool isName = false;
  if (g_inBacklogRender && !IsUiElement(lpString, c)) {
    isName = (s_lastTmHeight_ExtTextOut <= 18);
  }

  int nameSpacing = isName ? g_fontManager.GetBacklogNameSpacing() : 0;

  if (g_inBacklogRender && g_fontManager.GetDisableBacklogSpacing()) {
    nameSpacing = 0; // Skip character spacing
  }

  HFONT customFont = NULL;
  HFONT oldFont = NULL;
  int oldCharExtra = 0;

  bool hasCustomFont = false;
  if (isName) {
    hasCustomFont =
        ((g_fontManager.GetBacklogNameFontSize() != 0) ||
         (g_fontManager.GetBacklogNameFontName() != L"MS Gothic" &&
          g_fontManager.GetBacklogNameFontName() != L"ＭＳ ゴシック") ||
         (nameSpacing != 0));
  } else if (g_inBacklogRender) {
    hasCustomFont = ((g_fontManager.GetBacklogFontSize() != 0) ||
                     (g_fontManager.GetBacklogFontName() != L"MS Gothic" &&
                      g_fontManager.GetBacklogFontName() != L"ＭＳ ゴシック"));
  }

  if (g_inBacklogRender && g_fontManager.GetDisableBacklogFont()) {
    hasCustomFont = false;
  }

  if (hasCustomFont) {
    if (isName) {
      customFont = g_fontManager.GetBacklogNameFont(s_lastTmHeight_ExtTextOut);
    } else {
      customFont = g_fontManager.GetBacklogFont(s_lastTmHeight_ExtTextOut);
    }
    if (customFont) {
      oldFont = (HFONT)SelectObject(hdc, customFont);
    }
  }

  int renderY = y;
  if (!g_fontManager.GetDisableBacklogSpacing()) {
    if (hasCustomFont) {
      if (isName) {
        renderY += g_fontManager.GetBacklogNameYOffset();
      }
    }
  }

  if (nameSpacing != 0) {
    SetTextCharacterExtra(hdc, nameSpacing);
  }
  if (useDialogueTuning) {
    oldCharExtra = GetTextCharacterExtra(hdc);
    SetTextCharacterExtra(hdc, GetDialogueCharExtraTight());
  }

  if (g_inBacklogRender && !isName) {
    // Enable opaque background for Dialog Text to fill the spaced gaps
    SetBkMode(hdc, OPAQUE);
    // Dark transparent-like blue matching the BKLOGCHIP theme (RGB(0, 140,
    // 255))
    SetBkColor(hdc, RGB(0, 140, 255));
  } else {
    // Names and normal text remain transparent
    SetBkMode(hdc, TRANSPARENT);
  }

  int drawX = x;
  if (useDialogueTuning && renderStr && renderLen > 0) {
    drawX = AdjustDialogueDrawX(hdc, x, renderY, renderLen,
                                (unsigned char)renderStr[0]);
  }
  if (ShouldSkipSyntheticSpaceA(hdc, renderY, renderStr, renderLen,
                                useDialogueTuning)) {
    return TRUE;
  }

  ScopedDialogueLatinRender latinGuard(useDialogueTuning);
  int drawY =
      AdjustLatinPunctuationDrawY(renderY, FirstCharA(renderStr, renderLen));
  if (useDialogueTuning)
    DrawLatinExtraPassExtTextOutA(hdc, drawX, drawY, options, lprect, renderStr,
                                  renderLen, finalDx);
  BOOL result = Real_ExtTextOutA(hdc, drawX, drawY, options, lprect, renderStr,
                                 renderLen, finalDx);

  SetBkMode(hdc, TRANSPARENT);

  if (nameSpacing != 0) {
    SetTextCharacterExtra(hdc, 0); // Restore
  }
  if (useDialogueTuning) {
    SetTextCharacterExtra(hdc, oldCharExtra);
  }

  if (oldFont) {
    SelectObject(hdc, oldFont);
  }

  return result;
}

static BOOL WINAPI Hook_ExtTextOutW(HDC hdc, int x, int y, UINT options,
                                    const RECT *lprect, LPCWSTR lpString, UINT c,
                                    const INT *lpDx) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);
  if (InHsxf4LatinRender()) {
    if (IsOnlyTextControlW(lpString, c))
      return TRUE;
    ScopedHsxf4TextRender renderGuard(hdc);
    RECT expanded = {};
    const RECT *drawRect = ExpandHsxf4TextRect(lprect, &expanded);
    int firstCh = FirstCharW(lpString, c);
    int drawX = AdjustHsxf4DrawX(x);
    int drawY = AdjustLatinPunctuationDrawY(y, firstCh);



    if (!InHsxf4UiRender())
      DrawLatinExtraPassExtTextOutW(hdc, drawX, drawY, options, drawRect, lpString,
                                    c, lpDx);
    BOOL result =
        Real_ExtTextOutW(hdc, drawX, drawY, options, drawRect, lpString, c, lpDx);
    DrawHsxf4ExtraPauseDotExtW(hdc, drawX, drawY, options, drawRect);
    return result;
  }
  if (!g_inBacklogRender && IsLikelyReplayTextArea(x, y) && lpString) {
    UINT len = c;
    int actualLen = lstrlenW(lpString);
    if ((len == 0 || (actualLen > 0 && actualLen > (int)len &&
                      actualLen < (int)len + 80)) &&
        actualLen > 0) {
      len = (UINT)actualLen;
    }
    UINT replayLen = len;
    if (const wchar_t *replayText =
            TranslateReplayTextW(lpString, len, &replayLen)) {
      return DrawReplayTextW(hdc, x, y, options, lprect, replayText, replayLen,
                             true);
    }
    if (IsReplayTitleTextPosition(x, y)) {
      replayLen = len;
      if (const wchar_t *displayText =
              TranslateReplayDisplayTextW(lpString, len, &replayLen)) {
        return DrawReplayTextW(hdc, x, y, options, lprect, displayText,
                               replayLen, true);
      }
    }
    if (IsReplayTitleTextPosition(x, y) &&
        IsLikelyDialogueLatinW(lpString, len)) {
      return DrawReplayTextW(hdc, x, y, options, lprect, lpString, len, true);
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      IsLikelyPopupUiTextPoint(x, y) && lpString) {
    UINT renderLen = c;
    int actualLen = lstrlenW(lpString);
    if ((renderLen == 0 || (actualLen > 0 && actualLen > (int)renderLen &&
                            actualLen < (int)renderLen + 80)) &&
        actualLen > 0) {
      renderLen = (UINT)actualLen;
    }
    LPCWSTR renderStr = NormalizeWideAsciiSafe(lpString, &renderLen);
    if (!renderStr)
      renderStr = lpString;
    if (IsLikelyPopupUiLatinW(renderStr, renderLen)) {
      HFONT oldFont = SelectPopupUiTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, 0);
      ScopedPopupUiLatinRender popupRender(true);
      BOOL result = Real_ExtTextOutW(hdc, x, y, options, lprect, renderStr,
                                     renderLen, NULL);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      IsLikelyChoiceTextArea(x, y) && lpString) {
    UINT renderLen = c;
    int actualLen = lstrlenW(lpString);
    if ((renderLen == 0 || (actualLen > 0 && actualLen > (int)renderLen &&
                            actualLen < (int)renderLen + 80)) &&
        actualLen > 0) {
      renderLen = (UINT)actualLen;
    }
    LPCWSTR renderStr = NormalizeWideAsciiSafe(lpString, &renderLen);
    if (!renderStr)
      renderStr = lpString;
    if (IsLikelyChoiceLatinW(renderStr, renderLen)) {
      renderStr = NormalizeChoiceUiSpacesW(renderStr, &renderLen);
      LogChoiceRenderProbeW("ExtTextOutW", hdc, x, y, renderStr, renderLen,
                            lpDx);
      HFONT oldFont = SelectChoiceTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, -2);
      const INT *safeDx = NULL;
      int drawY = AdjustLatinPunctuationDrawY(y, FirstCharW(renderStr, renderLen));
      BOOL result = Real_ExtTextOutW(hdc, x, drawY, options, lprect, renderStr,
                                     renderLen, safeDx);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender)
    return Real_ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);
  if (!g_inBacklogRender && g_dialogueSafePassthrough) {
    UINT renderLen = c;
    if (renderLen == 0 && lpString)
      renderLen = (UINT)lstrlenW(lpString);
    INT *renderDx = NULL;
    LPCWSTR renderStr =
        FilterWideString(lpString, &renderLen, lpDx, &renderDx, true);
    if (!renderStr)
      renderStr = lpString;
    bool applyDialogueTuning =
        (g_fontManager.GetLanguage() != 0) && (renderLen > 0);
    bool likelyDialogueLatin =
        applyDialogueTuning &&
        (IsLikelyDialogueLatinW(renderStr, renderLen) ||
         IsLikelyDialogueTextboxArea(hdc, y) || (renderLen >= 6));
    HFONT oldFont = NULL;
    bool usedConfiguredDialogueFont = false;
    int oldCharExtra = 0;
    if (applyDialogueTuning) {
      HFONT customFont = g_fontManager.GetDialogueFont(g_fontManager.GetDialogueFontSize());
      if (customFont) {
        oldFont = (HFONT)SelectObject(hdc, customFont);
        usedConfiguredDialogueFont = true;
      } else {
        TEXTMETRICA tm;
        if (Real_GetTextMetricsA(hdc, &tm)) {
          int tunedH = (tm.tmHeight * 90 + 50) / 100;
          if (tunedH < 12)
            tunedH = 12;
          customFont = g_fontManager.GetEngineScaledFont(tunedH);
          if (customFont)
            oldFont = (HFONT)SelectObject(hdc, customFont);
        }
      }
      oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, -4);
    }
    int drawX = x;
    if (likelyDialogueLatin && renderStr && renderLen > 0) {
      drawX = AdjustDialogueDrawX(hdc, x, y, renderLen, (int)renderStr[0]);
    }
    if (ShouldSkipSyntheticSpaceW(hdc, y, renderStr, renderLen,
                                  applyDialogueTuning)) {
      if (applyDialogueTuning) {
        SetTextCharacterExtra(hdc, oldCharExtra);
        if (oldFont)
          SelectObject(hdc, oldFont);
      }
      return TRUE;
    }
    const INT *safeDx = (renderDx != NULL) ? renderDx : lpDx;
    float fitScale = 0.38f;
    if (applyDialogueTuning) {
      safeDx = CompressDxForDialogueW(safeDx, renderLen, renderStr, 24, 6);
      fitScale =
          ComputeFitScaleW(hdc, drawX, lprect, renderStr, renderLen, 0.38f);
    }
    if (applyDialogueTuning)
      drawX = CompressDialogueRunXSafe(true, drawX, y);
    float yScale =
        applyDialogueTuning ? (usedConfiguredDialogueFont ? 1.0f
                                                          : GetDialogueSizeScaleY())
                            : 1.0f;
    ScopedDialogueXScale scaleGuard(hdc, applyDialogueTuning, drawX, y, fitScale,
                                    yScale);
    if (likelyDialogueLatin) {
      LogRenderPathTelemetryW("ExtTextOutW_SAFE", hdc, drawX, y, (int)renderLen,
                              true, true, true, true, safeDx, renderStr);
    }
    int drawY = AdjustLatinPunctuationDrawY(y, FirstCharW(renderStr, renderLen));
    DrawLatinExtraPassExtTextOutW(hdc, drawX, drawY, options, lprect, renderStr,
                                  renderLen, safeDx);
    BOOL result = Real_ExtTextOutW(hdc, drawX, drawY, options, lprect, renderStr,
                                   renderLen, safeDx);
    if (applyDialogueTuning) {
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
    }
    return result;
  }
  int actualLen = lstrlenW(lpString);
  if (c > 0 && actualLen > (int)c && actualLen < (int)c + 150) {
    c = (UINT)actualLen;
  }

  UINT renderLen = c;
  INT *renderDx = NULL;
  UINT latinProbeLen = c;
  bool likelyByArea =
      (!g_inBacklogRender && IsLikelyDialogueTextboxArea(hdc, y));
  bool likelyDialogueLatinOrig =
      g_inBacklogRender || likelyByArea ||
      IsLikelyDialogueLatinW(lpString, latinProbeLen);
  bool performTranslation =
      (!(g_inBacklogRender && g_fontManager.GetDisableBacklogTranslation()) &&
       likelyDialogueLatinOrig);
  LPCWSTR renderStr =
      FilterWideString(lpString, &renderLen, lpDx, &renderDx, performTranslation);
  bool useDialogueTuning =
      (!g_inBacklogRender &&
       (likelyByArea || IsLikelyDialogueLatinW(renderStr, renderLen)));
  const INT *finalDx = (!g_inBacklogRender ? NULL : lpDx);
  if (!g_inBacklogRender && (likelyByArea || useDialogueTuning) &&
      ShouldSamplePathTelemetry()) {
    LogRenderPathTelemetryW("ExtTextOutW", hdc, x, y, (int)renderLen,
                            likelyByArea, likelyDialogueLatinOrig,
                            useDialogueTuning, performTranslation, finalDx,
                            renderStr);
  }

  HFONT oldFont = NULL;
  HFONT customFont = NULL;
  int oldCharExtra = 0;
  if (useDialogueTuning) {
    oldCharExtra = GetTextCharacterExtra(hdc);
    SetTextCharacterExtra(hdc, GetDialogueCharExtraTight());
  }

  int drawX = x;
  if (useDialogueTuning && renderStr && renderLen > 0) {
    drawX = AdjustDialogueDrawX(hdc, x, y, renderLen, (int)renderStr[0]);
  }
  if (ShouldSkipSyntheticSpaceW(hdc, y, renderStr, renderLen,
                                useDialogueTuning)) {
    return TRUE;
  }

  ScopedDialogueLatinRender latinGuard(useDialogueTuning);
  int drawY = AdjustLatinPunctuationDrawY(y, FirstCharW(renderStr, renderLen));
  if (useDialogueTuning)
    DrawLatinExtraPassExtTextOutW(hdc, drawX, drawY, options, lprect, renderStr,
                                  renderLen, finalDx);
  BOOL result = Real_ExtTextOutW(hdc, drawX, drawY, options, lprect, renderStr,
                                 renderLen, finalDx);

  if (oldFont) {
    SelectObject(hdc, oldFont);
  }
  if (useDialogueTuning) {
    SetTextCharacterExtra(hdc, oldCharExtra);
  }
  return result;
}

// ----------------------------------------------------------------------------
// Extra Text Rendering Hooks for Telemetry
// ----------------------------------------------------------------------------

static BOOL WINAPI Hook_TextOutA(HDC hdc, int x, int y, LPCSTR lpString,
                                 int c) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_TextOutA(hdc, x, y, lpString, c);
  if (InHsxf4LatinRender()) {
    ScopedHsxf4TextRender renderGuard(hdc);
    UINT len = (UINT)((c == -1 && lpString) ? lstrlenA(lpString)
                                            : (c < 0 ? 0 : c));
    if (IsOnlyTextControlA(lpString, len))
      return TRUE;
    int firstCh = FirstCharA(lpString, len);
    int drawX = AdjustHsxf4DrawX(x);
    int drawY = AdjustLatinPunctuationDrawY(y, firstCh);
    if (!InHsxf4UiRender())
      DrawLatinExtraPassTextOutA(hdc, drawX, drawY, lpString, (int)len);
    BOOL result = Real_TextOutA(hdc, drawX, drawY, lpString, c);
    DrawHsxf4ExtraPauseDotA(hdc, drawX, drawY);
    return result;
  }
  if (!g_inBacklogRender && IsLikelyReplayTextArea(x, y) && lpString) {
    UINT len = (UINT)((c == -1) ? lstrlenA(lpString) : (c < 0 ? 0 : c));
    UINT replayLen = len;
    if (const char *replayText =
            TranslateReplayTextA(lpString, len, &replayLen)) {
      return DrawReplayTextA(hdc, x, y, 0, NULL, replayText, replayLen, false);
    }
    if (IsReplayTitleTextPosition(x, y)) {
      replayLen = len;
      if (const char *displayText =
              TranslateReplayDisplayTextA(lpString, len, &replayLen)) {
        return DrawReplayTextA(hdc, x, y, 0, NULL, displayText, replayLen,
                               false);
      }
    }
    if (IsReplayTitleTextPosition(x, y) &&
        IsLikelyDialogueLatinA(lpString, len)) {
      return DrawReplayTextA(hdc, x, y, 0, NULL, lpString, len, false);
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      IsLikelyPopupUiTextPoint(x, y) && lpString) {
    UINT renderLen = (UINT)((c == -1) ? lstrlenA(lpString)
                                      : (c < 0 ? 0 : c));
    LPCSTR renderStr =
        FilterSjisString(lpString, &renderLen, NULL, NULL, true, true);
    if (!renderStr)
      renderStr = lpString;
    if (IsLikelyPopupUiLatinA(renderStr, renderLen)) {
      HFONT oldFont = SelectPopupUiTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, 0);
      ScopedPopupUiLatinRender popupRender(true);
      BOOL result = Real_TextOutA(hdc, x, y, renderStr, (int)renderLen);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      IsLikelyChoiceTextArea(x, y) && lpString) {
    UINT renderLen = (UINT)((c == -1) ? lstrlenA(lpString)
                                      : (c < 0 ? 0 : c));
    LPCSTR renderStr =
        FilterSjisString(lpString, &renderLen, NULL, NULL, true, true);
    if (!renderStr)
      renderStr = lpString;
    if (IsLikelyChoiceLatinA(renderStr, renderLen)) {
      renderStr = NormalizeChoiceUiSpacesA(renderStr, &renderLen);
      LogChoiceRenderProbeA("TextOutA", hdc, x, y, renderStr, renderLen,
                            NULL);
      HFONT oldFont = SelectChoiceTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, -2);
      int drawY = AdjustLatinPunctuationDrawY(y, FirstCharA(renderStr, renderLen));
      BOOL result = Real_TextOutA(hdc, x, drawY, renderStr, (int)renderLen);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender)
    return Real_TextOutA(hdc, x, y, lpString, c);
  if (!g_inBacklogRender && g_dialogueSafePassthrough) {
    UINT len = (UINT)((c == -1) ? lstrlenA(lpString) : (c < 0 ? 0 : c));
    UINT renderLen = len;
    LPCSTR renderStr =
        FilterSjisString(lpString, &renderLen, NULL, NULL, true, true);
    if (!renderStr)
      renderStr = lpString;
    bool isUi = IsUiElement(renderStr, renderLen);
    bool applyDialogueTuning =
        (g_fontManager.GetLanguage() != 0) && (renderLen > 0) && !isUi;
    bool likelyDialogueLatin =
        applyDialogueTuning &&
        (IsLikelyDialogueLatinA(renderStr, renderLen) ||
         IsLikelyDialogueTextboxArea(hdc, y) || renderLen >= 6);
    HFONT oldFont = NULL;
    bool usedConfiguredDialogueFont = false;
    int oldCharExtra = 0;
    if (applyDialogueTuning) {
      HFONT customFont = g_fontManager.GetDialogueFont(g_fontManager.GetDialogueFontSize());
      if (customFont) {
        oldFont = (HFONT)SelectObject(hdc, customFont);
        usedConfiguredDialogueFont = true;
      } else {
        TEXTMETRICA tm;
        if (Real_GetTextMetricsA(hdc, &tm)) {
          int tunedH = (tm.tmHeight * 90 + 50) / 100;
          if (tunedH < 12)
            tunedH = 12;
          customFont = g_fontManager.GetEngineScaledFont(tunedH);
          if (customFont)
            oldFont = (HFONT)SelectObject(hdc, customFont);
        }
      }
      oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, -4);
    }
    int drawX = x;
    if (applyDialogueTuning && renderLen > 0)
      drawX =
          AdjustDialogueDrawX(hdc, x, y, renderLen, (unsigned char)renderStr[0]);
    if (ShouldSkipSyntheticSpaceA(hdc, y, renderStr, renderLen,
                                  applyDialogueTuning)) {
      if (applyDialogueTuning) {
        SetTextCharacterExtra(hdc, oldCharExtra);
        if (oldFont)
          SelectObject(hdc, oldFont);
      }
      return TRUE;
    }
    if (applyDialogueTuning)
      drawX = CompressDialogueRunXSafe(true, drawX, y);
    const float baseScale = applyDialogueTuning ? 0.38f : 1.0f;
    const float yScale =
        applyDialogueTuning ? (usedConfiguredDialogueFont ? 1.0f
                                                          : GetDialogueSizeScaleY())
                            : 1.0f;
    ScopedDialogueXScale scaleGuard(hdc, applyDialogueTuning, drawX, y, baseScale,
                                    yScale);
    if (likelyDialogueLatin) {
      LogRenderPathTelemetryA("TextOutA_SAFE", hdc, drawX, y, (int)renderLen,
                              true, true, true, true, NULL, renderStr);
    }
    int drawY = AdjustLatinPunctuationDrawY(y, FirstCharA(renderStr, renderLen));
    DrawLatinExtraPassTextOutA(hdc, drawX, drawY, renderStr, (int)renderLen);
    BOOL result = Real_TextOutA(hdc, drawX, drawY, renderStr, (int)renderLen);
    if (applyDialogueTuning) {
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
    }
    return result;
  }
  if (!g_inBacklogRender) {
    LogTextTelemetry("TextOutA", lpString, c, NULL, NULL);
  }
  bool likelyByArea =
      (!g_inBacklogRender && IsLikelyDialogueTextboxArea(hdc, y));
  bool useDialogueTuning = (!g_inBacklogRender && lpString && c > 0 &&
                            (IsLikelyDialogueLatinA(lpString, (UINT)c) ||
                             likelyByArea));
  if (!g_inBacklogRender && lpString && c > 0 &&
      (likelyByArea || useDialogueTuning) && ShouldSamplePathTelemetry()) {
    LogRenderPathTelemetryA("TextOutA", hdc, x, y, c, likelyByArea,
                            IsLikelyDialogueLatinA(lpString, (UINT)c),
                            useDialogueTuning, false, NULL, lpString);
  }
  int drawX = x;
  if (useDialogueTuning) {
    drawX = AdjustDialogueDrawX(hdc, x, y, (UINT)c, (unsigned char)lpString[0]);
  }
  if (ShouldSkipSyntheticSpaceA(hdc, y, lpString, (UINT)(c < 0 ? 0 : c),
                                useDialogueTuning)) {
    return TRUE;
  }
  ScopedDialogueLatinRender latinGuard(useDialogueTuning);
  UINT drawLen = (UINT)((c == -1 && lpString) ? lstrlenA(lpString)
                                             : (c < 0 ? 0 : c));
  int drawY = AdjustLatinPunctuationDrawY(y, FirstCharA(lpString, drawLen));
  if (useDialogueTuning)
    DrawLatinExtraPassTextOutA(hdc, drawX, drawY, lpString, (int)drawLen);
  return Real_TextOutA(hdc, drawX, drawY, lpString, c);
}

static BOOL WINAPI Hook_TextOutW(HDC hdc, int x, int y, LPCWSTR lpString,
                                 int c) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_TextOutW(hdc, x, y, lpString, c);
  if (InHsxf4LatinRender()) {
    ScopedHsxf4TextRender renderGuard(hdc);
    UINT len = (UINT)((c == -1 && lpString) ? lstrlenW(lpString)
                                            : (c < 0 ? 0 : c));
    if (IsOnlyTextControlW(lpString, len))
      return TRUE;
    int firstCh = FirstCharW(lpString, len);
    int drawX = AdjustHsxf4DrawX(x);
    int drawY = AdjustLatinPunctuationDrawY(y, firstCh);
    if (!InHsxf4UiRender())
      DrawLatinExtraPassTextOutW(hdc, drawX, drawY, lpString, (int)len);
    BOOL result = Real_TextOutW(hdc, drawX, drawY, lpString, c);
    DrawHsxf4ExtraPauseDotW(hdc, drawX, drawY);
    return result;
  }
  if (!g_inBacklogRender && IsLikelyReplayTextArea(x, y) && lpString) {
    UINT len = (UINT)((c == -1) ? lstrlenW(lpString) : (c < 0 ? 0 : c));
    UINT replayLen = len;
    if (const wchar_t *replayText =
            TranslateReplayTextW(lpString, len, &replayLen)) {
      return DrawReplayTextW(hdc, x, y, 0, NULL, replayText, replayLen, false);
    }
    if (IsReplayTitleTextPosition(x, y)) {
      replayLen = len;
      if (const wchar_t *displayText =
              TranslateReplayDisplayTextW(lpString, len, &replayLen)) {
        return DrawReplayTextW(hdc, x, y, 0, NULL, displayText, replayLen,
                               false);
      }
    }
    if (IsReplayTitleTextPosition(x, y) &&
        IsLikelyDialogueLatinW(lpString, len)) {
      return DrawReplayTextW(hdc, x, y, 0, NULL, lpString, len, false);
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      IsLikelyPopupUiTextPoint(x, y) && lpString) {
    UINT renderLen = (UINT)((c == -1) ? lstrlenW(lpString)
                                      : (c < 0 ? 0 : c));
    LPCWSTR renderStr = NormalizeWideAsciiSafe(lpString, &renderLen);
    if (!renderStr)
      renderStr = lpString;
    if (IsLikelyPopupUiLatinW(renderStr, renderLen)) {
      HFONT oldFont = SelectPopupUiTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, 0);
      ScopedPopupUiLatinRender popupRender(true);
      BOOL result = Real_TextOutW(hdc, x, y, renderStr, (int)renderLen);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      IsLikelyChoiceTextArea(x, y) && lpString) {
    UINT renderLen = (UINT)((c == -1) ? lstrlenW(lpString)
                                      : (c < 0 ? 0 : c));
    LPCWSTR renderStr = NormalizeWideAsciiSafe(lpString, &renderLen);
    if (!renderStr)
      renderStr = lpString;
    if (IsLikelyChoiceLatinW(renderStr, renderLen)) {
      renderStr = NormalizeChoiceUiSpacesW(renderStr, &renderLen);
      LogChoiceRenderProbeW("TextOutW", hdc, x, y, renderStr, renderLen,
                            NULL);
      HFONT oldFont = SelectChoiceTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, -2);
      int drawY = AdjustLatinPunctuationDrawY(y, FirstCharW(renderStr, renderLen));
      BOOL result = Real_TextOutW(hdc, x, drawY, renderStr, (int)renderLen);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender)
    return Real_TextOutW(hdc, x, y, lpString, c);
  if (!g_inBacklogRender && g_dialogueSafePassthrough) {
    UINT renderLen = (UINT)((c == -1) ? lstrlenW(lpString) : (c < 0 ? 0 : c));
    LPCWSTR renderStr = FilterWideString(lpString, &renderLen, NULL, NULL, true);
    if (!renderStr)
      renderStr = lpString;
    bool applyDialogueTuning =
        (g_fontManager.GetLanguage() != 0) && (renderLen > 0);
    bool likelyDialogueLatin =
        applyDialogueTuning &&
        (IsLikelyDialogueLatinW(renderStr, renderLen) ||
         IsLikelyDialogueTextboxArea(hdc, y) || (renderLen >= 6));
    HFONT oldFont = NULL;
    bool usedConfiguredDialogueFont = false;
    int oldCharExtra = 0;
    if (applyDialogueTuning) {
      HFONT customFont = g_fontManager.GetDialogueFont(g_fontManager.GetDialogueFontSize());
      if (customFont) {
        oldFont = (HFONT)SelectObject(hdc, customFont);
        usedConfiguredDialogueFont = true;
      } else {
        TEXTMETRICA tm;
        if (Real_GetTextMetricsA(hdc, &tm)) {
          int tunedH = (tm.tmHeight * 90 + 50) / 100;
          if (tunedH < 12)
            tunedH = 12;
          customFont = g_fontManager.GetEngineScaledFont(tunedH);
          if (customFont)
            oldFont = (HFONT)SelectObject(hdc, customFont);
        }
      }
      oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, -4);
    }
    int drawX = x;
    if (applyDialogueTuning && renderLen > 0)
      drawX = AdjustDialogueDrawX(hdc, x, y, renderLen, (int)renderStr[0]);
    if (ShouldSkipSyntheticSpaceW(hdc, y, renderStr, renderLen,
                                  applyDialogueTuning)) {
      if (applyDialogueTuning) {
        SetTextCharacterExtra(hdc, oldCharExtra);
        if (oldFont)
          SelectObject(hdc, oldFont);
      }
      return TRUE;
    }
    if (applyDialogueTuning)
      drawX = CompressDialogueRunXSafe(true, drawX, y);
    const float baseScale = applyDialogueTuning ? 0.38f : 1.0f;
    const float yScale =
        applyDialogueTuning ? (usedConfiguredDialogueFont ? 1.0f
                                                          : GetDialogueSizeScaleY())
                            : 1.0f;
    ScopedDialogueXScale scaleGuard(hdc, applyDialogueTuning, drawX, y, baseScale,
                                    yScale);
    if (likelyDialogueLatin) {
      LogRenderPathTelemetryW("TextOutW_SAFE", hdc, drawX, y, (int)renderLen,
                              true, true, true, true, NULL, renderStr);
    }
    int drawY = AdjustLatinPunctuationDrawY(y, FirstCharW(renderStr, renderLen));
    DrawLatinExtraPassTextOutW(hdc, drawX, drawY, renderStr, (int)renderLen);
    BOOL result = Real_TextOutW(hdc, drawX, drawY, renderStr, (int)renderLen);
    if (applyDialogueTuning) {
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
    }
    return result;
  }
  int len = (c == -1) ? lstrlenW(lpString) : c;
  UINT filteredLen = (UINT)(len < 0 ? 0 : len);
  bool likelyByArea =
      (!g_inBacklogRender && IsLikelyDialogueTextboxArea(hdc, y));
  bool likelyDialogueLatinOrig =
      g_inBacklogRender || likelyByArea ||
      IsLikelyDialogueLatinW(lpString, filteredLen);
  bool performTranslation =
      (!(g_inBacklogRender && g_fontManager.GetDisableBacklogTranslation()) &&
       likelyDialogueLatinOrig);
  LPCWSTR renderStr =
      FilterWideString(lpString, &filteredLen, NULL, NULL, performTranslation);
  bool useDialogueTuning =
      (!g_inBacklogRender &&
       (likelyByArea || IsLikelyDialogueLatinW(renderStr, filteredLen)));
  if (!g_inBacklogRender && renderStr && filteredLen > 0 &&
      (likelyByArea || useDialogueTuning) && ShouldSamplePathTelemetry()) {
    LogRenderPathTelemetryW("TextOutW", hdc, x, y, (int)filteredLen,
                            likelyByArea, likelyDialogueLatinOrig,
                            useDialogueTuning, performTranslation, NULL,
                            renderStr);
  }

  HFONT customFont = NULL;
  HFONT oldFont = NULL;
  int oldCharExtra = 0;
  if (useDialogueTuning) {
    oldCharExtra = GetTextCharacterExtra(hdc);
    SetTextCharacterExtra(hdc, GetDialogueCharExtraTight());
  }

  int drawX = x;
  if (useDialogueTuning && renderStr && filteredLen > 0) {
    drawX = AdjustDialogueDrawX(hdc, x, y, filteredLen, (int)renderStr[0]);
  }
  if (ShouldSkipSyntheticSpaceW(hdc, y, renderStr, filteredLen,
                                useDialogueTuning)) {
    return TRUE;
  }
  ScopedDialogueLatinRender latinGuard(useDialogueTuning);
  int drawY = AdjustLatinPunctuationDrawY(y, FirstCharW(renderStr, filteredLen));
  if (useDialogueTuning)
    DrawLatinExtraPassTextOutW(hdc, drawX, drawY, renderStr,
                               (int)filteredLen);
  BOOL result = Real_TextOutW(hdc, drawX, drawY, renderStr, (int)filteredLen);

  if (useDialogueTuning) {
    SetTextCharacterExtra(hdc, oldCharExtra);
  }
  if (oldFont) {
    SelectObject(hdc, oldFont);
  }
  return result;
}

static decltype(&DrawTextA) Real_DrawTextA = DrawTextA;
static int WINAPI Hook_DrawTextA(HDC hdc, LPCSTR lpchText, int cchText,
                                 LPRECT lprc, UINT format) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_DrawTextA(hdc, lpchText, cchText, lprc, format);
  int choiceX = lprc ? lprc->left : 0;
  int choiceY = lprc ? lprc->top : 0;
  if (!g_inBacklogRender && lprc && IsLikelyReplayTextArea(choiceX, choiceY) &&
      lpchText) {
    UINT len = (UINT)((cchText == -1) ? lstrlenA(lpchText)
                                      : (cchText < 0 ? 0 : cchText));
    UINT replayLen = len;
    if (const char *replayText =
            TranslateReplayTextA(lpchText, len, &replayLen)) {
      return DrawReplayTextA(hdc, choiceX, choiceY, 0, lprc, replayText,
                             replayLen, false);
    }
    if (IsReplayTitleTextPosition(choiceX, choiceY)) {
      replayLen = len;
      if (const char *displayText =
              TranslateReplayDisplayTextA(lpchText, len, &replayLen)) {
        return DrawReplayTextA(hdc, choiceX, choiceY, 0, lprc, displayText,
                               replayLen, false)
                   ? (lprc->bottom - lprc->top)
                   : 0;
      }
    }
    if (IsReplayTitleTextPosition(choiceX, choiceY) &&
        IsLikelyDialogueLatinA(lpchText, len)) {
      return DrawReplayTextA(hdc, choiceX, choiceY, 0, lprc, lpchText, len,
                             false)
                 ? (lprc->bottom - lprc->top)
                 : 0;
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lprc &&
      IsLikelyPopupUiTextRect(lprc) && lpchText) {
    UINT renderLen = (UINT)((cchText == -1) ? lstrlenA(lpchText)
                                            : (cchText < 0 ? 0 : cchText));
    LPCSTR renderStr =
        FilterSjisString(lpchText, &renderLen, NULL, NULL, true, true);
    if (!renderStr)
      renderStr = lpchText;
    if (IsLikelyPopupUiLatinA(renderStr, renderLen)) {
      HFONT oldFont = SelectPopupUiTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, 0);
      ScopedPopupUiLatinRender popupRender(true);
      int result = Real_DrawTextA(hdc, renderStr, (int)renderLen, lprc, format);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      IsLikelyChoiceTextArea(choiceX, choiceY) && lpchText) {
    UINT renderLen = (UINT)((cchText == -1) ? lstrlenA(lpchText)
                                            : (cchText < 0 ? 0 : cchText));
    LPCSTR renderStr =
        FilterSjisString(lpchText, &renderLen, NULL, NULL, true, true);
    if (!renderStr)
      renderStr = lpchText;
    if (IsLikelyChoiceLatinA(renderStr, renderLen)) {
      renderStr = NormalizeChoiceUiSpacesA(renderStr, &renderLen);
      HFONT oldFont = SelectChoiceTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, -1);
      int result = Real_DrawTextA(hdc, renderStr, (int)renderLen, lprc, format);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender)
    return Real_DrawTextA(hdc, lpchText, cchText, lprc, format);
  if (!g_inBacklogRender && lpchText) {
    int len = (cchText == -1) ? lstrlenA(lpchText) : cchText;
    LogTextTelemetry("DrawTextA", lpchText, len, NULL, NULL);
  }
  return Real_DrawTextA(hdc, lpchText, cchText, lprc, format);
}

static int WINAPI Hook_DrawTextW(HDC hdc, LPCWSTR lpchText, int cchText,
                                 LPRECT lprc, UINT format) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_DrawTextW(hdc, lpchText, cchText, lprc, format);
  int choiceX = lprc ? lprc->left : 0;
  int choiceY = lprc ? lprc->top : 0;
  if (!g_inBacklogRender && lprc && IsLikelyReplayTextArea(choiceX, choiceY) &&
      lpchText) {
    UINT len = (UINT)((cchText == -1) ? lstrlenW(lpchText)
                                      : (cchText < 0 ? 0 : cchText));
    UINT replayLen = len;
    if (const wchar_t *replayText =
            TranslateReplayTextW(lpchText, len, &replayLen)) {
      return DrawReplayTextW(hdc, choiceX, choiceY, 0, lprc, replayText,
                             replayLen, false);
    }
    if (IsReplayTitleTextPosition(choiceX, choiceY)) {
      replayLen = len;
      if (const wchar_t *displayText =
              TranslateReplayDisplayTextW(lpchText, len, &replayLen)) {
        return DrawReplayTextW(hdc, choiceX, choiceY, 0, lprc, displayText,
                               replayLen, false)
                   ? (lprc->bottom - lprc->top)
                   : 0;
      }
    }
    if (IsReplayTitleTextPosition(choiceX, choiceY) &&
        IsLikelyDialogueLatinW(lpchText, len)) {
      return DrawReplayTextW(hdc, choiceX, choiceY, 0, lprc, lpchText, len,
                             false)
                 ? (lprc->bottom - lprc->top)
                 : 0;
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lprc &&
      IsLikelyPopupUiTextRect(lprc) && lpchText) {
    UINT renderLen = (UINT)((cchText == -1) ? lstrlenW(lpchText)
                                            : (cchText < 0 ? 0 : cchText));
    LPCWSTR renderStr = NormalizeWideAsciiSafe(lpchText, &renderLen);
    if (!renderStr)
      renderStr = lpchText;
    if (IsLikelyPopupUiLatinW(renderStr, renderLen)) {
      HFONT oldFont = SelectPopupUiTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, 0);
      ScopedPopupUiLatinRender popupRender(true);
      int result = Real_DrawTextW(hdc, renderStr, (int)renderLen, lprc, format);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      IsLikelyChoiceTextArea(choiceX, choiceY) && lpchText) {
    UINT renderLen = (UINT)((cchText == -1) ? lstrlenW(lpchText)
                                            : (cchText < 0 ? 0 : cchText));
    LPCWSTR renderStr = NormalizeWideAsciiSafe(lpchText, &renderLen);
    if (!renderStr)
      renderStr = lpchText;
    if (IsLikelyChoiceLatinW(renderStr, renderLen)) {
      renderStr = NormalizeChoiceUiSpacesW(renderStr, &renderLen);
      HFONT oldFont = SelectChoiceTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, -1);
      int result = Real_DrawTextW(hdc, renderStr, (int)renderLen, lprc, format);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender)
    return Real_DrawTextW(hdc, lpchText, cchText, lprc, format);
  if (!g_inBacklogRender && g_dialogueSafePassthrough) {
    UINT renderLen = (UINT)((cchText == -1) ? lstrlenW(lpchText)
                                            : (cchText < 0 ? 0 : cchText));
    LPCWSTR renderStr =
        FilterWideString(lpchText, &renderLen, NULL, NULL, true);
    if (!renderStr)
      renderStr = lpchText;
    bool likelyDialogueLatin = IsLikelyDialogueLatinW(renderStr, renderLen);
    int oldCharExtra = 0;
    if (likelyDialogueLatin) {
      oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, GetDialogueCharExtraTight());
    }
    int result = Real_DrawTextW(hdc, renderStr, (int)renderLen, lprc, format);
    if (likelyDialogueLatin) {
      SetTextCharacterExtra(hdc, oldCharExtra);
    }
    return result;
  }
  int len = (cchText == -1) ? lstrlenW(lpchText) : cchText;
  UINT filteredLen = (UINT)(len < 0 ? 0 : len);
  bool likelyByArea =
      (!g_inBacklogRender &&
       IsLikelyDialogueTextboxArea(hdc, lprc ? lprc->top : 0));
  bool likelyDialogueLatinOrig =
      g_inBacklogRender || likelyByArea ||
      IsLikelyDialogueLatinW(lpchText, filteredLen);
  bool performTranslation =
      (!(g_inBacklogRender && g_fontManager.GetDisableBacklogTranslation()) &&
       likelyDialogueLatinOrig);
  LPCWSTR renderStr =
      FilterWideString(lpchText, &filteredLen, NULL, NULL, performTranslation);
  bool useDialogueTuning =
      (!g_inBacklogRender &&
       (likelyByArea || IsLikelyDialogueLatinW(renderStr, filteredLen)));
  if (!g_inBacklogRender && renderStr && filteredLen > 0 &&
      (likelyByArea || useDialogueTuning) && ShouldSamplePathTelemetry()) {
    LogRenderPathTelemetryW("DrawTextW", hdc, lprc ? lprc->left : 0,
                            lprc ? lprc->top : 0, (int)filteredLen,
                            likelyByArea, likelyDialogueLatinOrig,
                            useDialogueTuning, performTranslation, NULL,
                            renderStr);
  }

  HFONT customFont = NULL;
  HFONT oldFont = NULL;
  int oldCharExtra = 0;
  if (useDialogueTuning) {
    oldCharExtra = GetTextCharacterExtra(hdc);
    SetTextCharacterExtra(hdc, GetDialogueCharExtraTight());
  }

  ScopedDialogueLatinRender latinGuard(useDialogueTuning);
  int result = Real_DrawTextW(hdc, renderStr, (int)filteredLen, lprc, format);

  if (useDialogueTuning) {
    SetTextCharacterExtra(hdc, oldCharExtra);
  }
  if (oldFont) {
    SelectObject(hdc, oldFont);
  }
  return result;
}

static decltype(&DrawTextExA) Real_DrawTextExA = DrawTextExA;
static int WINAPI Hook_DrawTextExA(HDC hdc, LPSTR lpchText, int cchText,
                                   LPRECT lprc, UINT format,
                                   LPDRAWTEXTPARAMS lpdtp) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_DrawTextExA(hdc, lpchText, cchText, lprc, format, lpdtp);
  int replayX = lprc ? lprc->left : 0;
  int replayY = lprc ? lprc->top : 0;
  if (!g_inBacklogRender && lprc && IsLikelyReplayTextArea(replayX, replayY) &&
      lpchText) {
    UINT len = (UINT)((cchText == -1) ? lstrlenA(lpchText)
                                      : (cchText < 0 ? 0 : cchText));
    UINT replayLen = len;
    if (const char *replayText =
            TranslateReplayTextA(lpchText, len, &replayLen)) {
      return DrawReplayTextA(hdc, replayX, replayY, 0, lprc, replayText,
                             replayLen, false);
    }
    if (IsReplayTitleTextPosition(replayX, replayY)) {
      replayLen = len;
      if (const char *displayText =
              TranslateReplayDisplayTextA(lpchText, len, &replayLen)) {
        return DrawReplayTextA(hdc, replayX, replayY, 0, lprc, displayText,
                               replayLen, false)
                   ? (lprc->bottom - lprc->top)
                   : 0;
      }
    }
    if (IsReplayTitleTextPosition(replayX, replayY) &&
        IsLikelyDialogueLatinA(lpchText, len)) {
      return DrawReplayTextA(hdc, replayX, replayY, 0, lprc, lpchText, len,
                             false)
                 ? (lprc->bottom - lprc->top)
                 : 0;
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lprc &&
      IsLikelyPopupUiTextRect(lprc) && lpchText) {
    UINT renderLen = (UINT)((cchText == -1) ? lstrlenA(lpchText)
                                            : (cchText < 0 ? 0 : cchText));
    LPCSTR renderStr =
        FilterSjisString(lpchText, &renderLen, NULL, NULL, true, true);
    if (!renderStr)
      renderStr = lpchText;
    if (IsLikelyPopupUiLatinA(renderStr, renderLen)) {
      HFONT oldFont = SelectPopupUiTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, 0);
      ScopedPopupUiLatinRender popupRender(true);
      int result =
          Real_DrawTextExA(hdc, const_cast<LPSTR>(renderStr), (int)renderLen,
                           lprc, format, lpdtp);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender)
    return Real_DrawTextExA(hdc, lpchText, cchText, lprc, format, lpdtp);
  if (!g_inBacklogRender && lpchText) {
    int len = (cchText == -1) ? lstrlenA(lpchText) : cchText;
    LogTextTelemetry("DrawTextExA", lpchText, len, NULL, NULL);
  }
  return Real_DrawTextExA(hdc, lpchText, cchText, lprc, format, lpdtp);
}

static int WINAPI Hook_DrawTextExW(HDC hdc, LPWSTR lpchText, int cchText,
                                   LPRECT lprc, UINT format,
                                   LPDRAWTEXTPARAMS lpdtp) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);
  int choiceX = lprc ? lprc->left : 0;
  int choiceY = lprc ? lprc->top : 0;
  if (!g_inBacklogRender && lprc && IsLikelyReplayTextArea(choiceX, choiceY) &&
      lpchText) {
    UINT len = (UINT)((cchText == -1) ? lstrlenW(lpchText)
                                      : (cchText < 0 ? 0 : cchText));
    UINT replayLen = len;
    if (const wchar_t *replayText =
            TranslateReplayTextW(lpchText, len, &replayLen)) {
      return DrawReplayTextW(hdc, choiceX, choiceY, 0, lprc, replayText,
                             replayLen, false);
    }
    if (IsReplayTitleTextPosition(choiceX, choiceY)) {
      replayLen = len;
      if (const wchar_t *displayText =
              TranslateReplayDisplayTextW(lpchText, len, &replayLen)) {
        return DrawReplayTextW(hdc, choiceX, choiceY, 0, lprc, displayText,
                               replayLen, false)
                   ? (lprc->bottom - lprc->top)
                   : 0;
      }
    }
    if (IsReplayTitleTextPosition(choiceX, choiceY) &&
        IsLikelyDialogueLatinW(lpchText, len)) {
      return DrawReplayTextW(hdc, choiceX, choiceY, 0, lprc, lpchText, len,
                             false)
                 ? (lprc->bottom - lprc->top)
                 : 0;
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lprc &&
      IsLikelyPopupUiTextRect(lprc) && lpchText) {
    UINT renderLen = (UINT)((cchText == -1) ? lstrlenW(lpchText)
                                            : (cchText < 0 ? 0 : cchText));
    LPCWSTR renderStr = NormalizeWideAsciiSafe(lpchText, &renderLen);
    if (!renderStr)
      renderStr = lpchText;
    if (IsLikelyPopupUiLatinW(renderStr, renderLen)) {
      HFONT oldFont = SelectPopupUiTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, 0);
      ScopedPopupUiLatinRender popupRender(true);
      int result =
          Real_DrawTextExW(hdc, const_cast<LPWSTR>(renderStr), (int)renderLen,
                           lprc, format, lpdtp);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      IsLikelyChoiceTextArea(choiceX, choiceY) && lpchText) {
    UINT renderLen = (UINT)((cchText == -1) ? lstrlenW(lpchText)
                                            : (cchText < 0 ? 0 : cchText));
    LPCWSTR renderStr = NormalizeWideAsciiSafe(lpchText, &renderLen);
    if (!renderStr)
      renderStr = lpchText;
    if (IsLikelyChoiceLatinW(renderStr, renderLen)) {
      renderStr = NormalizeChoiceUiSpacesW(renderStr, &renderLen);
      HFONT oldFont = SelectChoiceTextFont(hdc);
      int oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, -1);
      int result = Real_DrawTextExW(hdc, const_cast<LPWSTR>(renderStr),
                                    (int)renderLen, lprc, format, lpdtp);
      SetTextCharacterExtra(hdc, oldCharExtra);
      if (oldFont)
        SelectObject(hdc, oldFont);
      return result;
    }
  }
  if (!g_inBacklogRender)
    return Real_DrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);
  if (!g_inBacklogRender && g_dialogueSafePassthrough) {
    UINT renderLen = (UINT)((cchText == -1) ? lstrlenW(lpchText)
                                            : (cchText < 0 ? 0 : cchText));
    LPCWSTR renderStr =
        FilterWideString(lpchText, &renderLen, NULL, NULL, true);
    if (!renderStr)
      renderStr = lpchText;
    bool likelyDialogueLatin =
        IsLikelyDialogueLatinW(renderStr, renderLen) ||
        IsLikelyDialogueTextboxArea(hdc, lprc ? lprc->top : 0);
    int oldCharExtra = 0;
    if (likelyDialogueLatin) {
      oldCharExtra = GetTextCharacterExtra(hdc);
      SetTextCharacterExtra(hdc, GetDialogueCharExtraTight());
    }
    int result = Real_DrawTextExW(hdc, const_cast<LPWSTR>(renderStr),
                                  (int)renderLen,
                                  lprc, format, lpdtp);
    if (likelyDialogueLatin) {
      SetTextCharacterExtra(hdc, oldCharExtra);
    }
    return result;
  }
  int len = (cchText == -1) ? lstrlenW(lpchText) : cchText;
  UINT filteredLen = (UINT)(len < 0 ? 0 : len);
  bool likelyByArea =
      (!g_inBacklogRender &&
       IsLikelyDialogueTextboxArea(hdc, lprc ? lprc->top : 0));
  bool likelyDialogueLatinOrig =
      g_inBacklogRender || likelyByArea ||
      IsLikelyDialogueLatinW(lpchText, filteredLen);
  bool performTranslation =
      (!(g_inBacklogRender && g_fontManager.GetDisableBacklogTranslation()) &&
       likelyDialogueLatinOrig);
  LPCWSTR renderStr =
      FilterWideString(lpchText, &filteredLen, NULL, NULL, performTranslation);
  bool useDialogueTuning =
      (!g_inBacklogRender &&
       (likelyByArea || IsLikelyDialogueLatinW(renderStr, filteredLen)));
  if (!g_inBacklogRender && renderStr && filteredLen > 0 &&
      (likelyByArea || useDialogueTuning) && ShouldSamplePathTelemetry()) {
    LogRenderPathTelemetryW("DrawTextExW", hdc, lprc ? lprc->left : 0,
                            lprc ? lprc->top : 0, (int)filteredLen,
                            likelyByArea, likelyDialogueLatinOrig,
                            useDialogueTuning, performTranslation, NULL,
                            renderStr);
  }

  HFONT customFont = NULL;
  HFONT oldFont = NULL;
  int oldCharExtra = 0;
  if (useDialogueTuning) {
    oldCharExtra = GetTextCharacterExtra(hdc);
    SetTextCharacterExtra(hdc, GetDialogueCharExtraTight());
  }

  ScopedDialogueLatinRender latinGuard(useDialogueTuning);
  int result = Real_DrawTextExW(hdc, const_cast<LPWSTR>(renderStr),
                                (int)filteredLen, lprc, format, lpdtp);

  if (useDialogueTuning) {
    SetTextCharacterExtra(hdc, oldCharExtra);
  }
  if (oldFont) {
    SelectObject(hdc, oldFont);
  }
  return result;
}

static int WINAPI Hook_SetTextCharacterExtra(HDC hdc, int nCharExtra) {
  if (!hdc)
    return Real_SetTextCharacterExtra(hdc, nCharExtra);
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) ||
      IsVersionInfoDialogDrawContext(hdc)) {
    return Real_SetTextCharacterExtra(hdc, nCharExtra);
  }
  if (!g_inBacklogRender)
    return Real_SetTextCharacterExtra(hdc, nCharExtra);
  if (!g_inBacklogRender) {
    // ADV32 often pushes large positive tracking for dialogue; clamp it so
    // Latin text doesn't become "D e n g a n ...".
    if (nCharExtra > 0) {
      nCharExtra = 0;
    } else if (nCharExtra < -16) {
      nCharExtra = -16;
    }
  }
  return Real_SetTextCharacterExtra(hdc, nCharExtra);
}

static BOOL WINAPI Hook_SetTextJustification(HDC hdc, int nBreakExtra,
                                             int nBreakCount) {
  if (!hdc)
    return Real_SetTextJustification(hdc, nBreakExtra, nBreakCount);
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) ||
      IsVersionInfoDialogDrawContext(hdc)) {
    return Real_SetTextJustification(hdc, nBreakExtra, nBreakCount);
  }
  if (!g_inBacklogRender)
    return Real_SetTextJustification(hdc, nBreakExtra, nBreakCount);
  if (!g_inBacklogRender && nBreakCount > 0 && nBreakExtra > 0) {
    // Disable GDI justification stretch in dialogue so words don't split apart.
    nBreakExtra = 0;
  }
  return Real_SetTextJustification(hdc, nBreakExtra, nBreakCount);
}

static BOOL WINAPI Hook_GetTextExtentPoint32A(HDC hdc, LPCSTR lpString, int c,
                                              LPSIZE lpSize) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_GetTextExtentPoint32A(hdc, lpString, c, lpSize);
  if (!g_inBacklogRender)
    return Real_GetTextExtentPoint32A(hdc, lpString, c, lpSize);
  if (!g_inBacklogRender && g_dialogueSafePassthrough) {
    BOOL result = Real_GetTextExtentPoint32A(hdc, lpString, c, lpSize);
    UINT len = (UINT)((c > 0) ? c : (lpString ? lstrlenA(lpString) : 0));
    if (result && lpSize && g_fontManager.GetLanguage() != 0 && lpString &&
        IsLikelyDialogueLatinA(lpString, len)) {
      lpSize->cx = (lpSize->cx * 35 + 50) / 100;
      if (lpSize->cx < (int)len)
        lpSize->cx = (int)len;
    }
    return result;
  }
  int actualLen = lstrlenA(lpString);
  if (c > 0 && actualLen > c && actualLen < c + 150)
    c = actualLen;

  UINT filteredLen = c;
  UINT latinProbeLen = (c > 0) ? (UINT)c : (UINT)lstrlenA(lpString);
  bool likelyByArea =
      (!g_inBacklogRender && IsLikelyDialogueMetricsContext(hdc));
  bool likelyDialogueLatinOrig =
      g_inBacklogRender || likelyByArea ||
      IsLikelyDialogueLatinA(lpString, latinProbeLen);
  bool performTranslation =
      (!(g_inBacklogRender && g_fontManager.GetDisableBacklogTranslation()) &&
       likelyDialogueLatinOrig);
  lpString = FilterSjisString(lpString, &filteredLen, NULL, NULL,
                              performTranslation, performTranslation);
  c = filteredLen;
  bool useDialogueTuning =
      (!g_inBacklogRender &&
       (likelyByArea || IsLikelyDialogueLatinA(lpString, filteredLen)));
  if (!g_inBacklogRender && lpString && c > 0 &&
      (likelyByArea || useDialogueTuning) && ShouldSamplePathTelemetry()) {
    LogRenderPathTelemetryA("GetTextExtentPoint32A", hdc, 0, 0, c,
                            likelyByArea, likelyDialogueLatinOrig,
                            useDialogueTuning, performTranslation, NULL,
                            lpString);
  }

  static thread_local HDC s_lastHdc_GetTextExtent = NULL;
  static thread_local HFONT s_lastBgFont_GetTextExtent = NULL;
  static thread_local int s_lastTmHeight_GetTextExtent = 0;

  HFONT currFont = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
  if (hdc != s_lastHdc_GetTextExtent ||
      currFont != s_lastBgFont_GetTextExtent ||
      s_lastHdc_GetTextExtent == NULL) {
    TEXTMETRICA tm;
    Real_GetTextMetricsA(hdc, &tm);
    s_lastTmHeight_GetTextExtent = tm.tmHeight;
    s_lastHdc_GetTextExtent = hdc;
    s_lastBgFont_GetTextExtent = currFont;
  }

  bool isName = false;
  if (g_inBacklogRender && !IsUiElement(lpString, c)) {
    isName = (s_lastTmHeight_GetTextExtent <= 18);
  }

  int nameSpacing = isName ? g_fontManager.GetBacklogNameSpacing() : 0;
  if (g_inBacklogRender && g_fontManager.GetDisableBacklogSpacing()) {
    nameSpacing = 0;
  }

  HFONT customFont = NULL;
  HFONT oldFont = NULL;

  bool hasCustomFont = false;
  if (isName) {
    hasCustomFont =
        ((g_fontManager.GetBacklogNameFontSize() != 0) ||
         (g_fontManager.GetBacklogNameFontName() != L"MS Gothic" &&
          g_fontManager.GetBacklogNameFontName() != L"ＭＳ ゴシック") ||
         (nameSpacing != 0));
  } else if (g_inBacklogRender) {
    hasCustomFont = ((g_fontManager.GetBacklogFontSize() != 0) ||
                     (g_fontManager.GetBacklogFontName() != L"MS Gothic" &&
                      g_fontManager.GetBacklogFontName() != L"ＭＳ ゴシック"));
  }

  if (g_inBacklogRender && g_fontManager.GetDisableBacklogFont()) {
    hasCustomFont = false;
  }

  if (hasCustomFont) {
    if (isName) {
      customFont =
          g_fontManager.GetBacklogNameFont(s_lastTmHeight_GetTextExtent);
    } else {
      customFont = g_fontManager.GetBacklogFont(s_lastTmHeight_GetTextExtent);
    }
    oldFont = (HFONT)SelectObject(hdc, customFont);
  }

  if (nameSpacing != 0) {
    SetTextCharacterExtra(hdc, nameSpacing);
  }

  // The original string is ALREADY translated at the top of
  // Hook_GetTextExtentPoint32A! Look at line 1345-1350. The loop uses
  // `lpString` which has been updated. So we just pass lpString and c, which
  // are already the translated ones.
  BOOL result = Real_GetTextExtentPoint32A(hdc, lpString, c, lpSize);
  if (result && lpSize && useDialogueTuning) {
    lpSize->cx = (lpSize->cx * 35 + 50) / 100;
    if (lpSize->cx < c)
      lpSize->cx = c;
  }

  if (useDialogueTuning) {
    LogTextTelemetry("GetTextExtent (OUT)", lpString, c, NULL, lpSize);
  }

  if (!g_fontManager.GetDisableBacklogSpacing()) {
    if (result && g_inBacklogRender && !isName) {
      lpSize->cy += g_fontManager.GetBacklogLineSpacing();
    }
  }

  if (nameSpacing != 0) {
    SetTextCharacterExtra(hdc, 0);
  }
  if (oldFont) {
    SelectObject(hdc, oldFont);
  }

  return result;
}

static BOOL WINAPI Hook_GetTextExtentPoint32W(HDC hdc, LPCWSTR lpString, int c,
                                              LPSIZE lpSize) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_GetTextExtentPoint32W(hdc, lpString, c, lpSize);
  if (!g_inBacklogRender)
    return Real_GetTextExtentPoint32W(hdc, lpString, c, lpSize);
  if (!g_inBacklogRender && g_dialogueSafePassthrough) {
    UINT renderLen = (UINT)((c > 0) ? c : lstrlenW(lpString));
    LPCWSTR renderStr = FilterWideString(lpString, &renderLen, NULL, NULL, true);
    if (!renderStr)
      renderStr = lpString;
    BOOL result = Real_GetTextExtentPoint32W(hdc, renderStr, (int)renderLen, lpSize);
    if (result && lpSize && g_fontManager.GetLanguage() != 0 && renderStr &&
        IsLikelyDialogueLatinW(renderStr, renderLen)) {
      lpSize->cx = (lpSize->cx * 35 + 50) / 100;
      if (lpSize->cx < (int)renderLen)
        lpSize->cx = (int)renderLen;
    }
    return result;
  }
  int actualLen = lstrlenW(lpString);
  if (c > 0 && actualLen > c && actualLen < c + 150)
    c = actualLen;

  UINT filteredLen = (UINT)c;
  UINT latinProbeLen = (c > 0) ? (UINT)c : (UINT)lstrlenW(lpString);
  bool likelyByArea =
      (!g_inBacklogRender && IsLikelyDialogueMetricsContext(hdc));
  bool likelyDialogueLatinOrig =
      g_inBacklogRender || likelyByArea ||
      IsLikelyDialogueLatinW(lpString, latinProbeLen);
  bool performTranslation =
      (!(g_inBacklogRender && g_fontManager.GetDisableBacklogTranslation()) &&
       likelyDialogueLatinOrig);
  lpString =
      FilterWideString(lpString, &filteredLen, NULL, NULL, performTranslation);
  c = (int)filteredLen;
  bool useDialogueTuning =
      (!g_inBacklogRender &&
       (likelyByArea || IsLikelyDialogueLatinW(lpString, filteredLen)));
  if (!g_inBacklogRender && lpString && c > 0 &&
      (likelyByArea || useDialogueTuning) && ShouldSamplePathTelemetry()) {
    LogRenderPathTelemetryW("GetTextExtentPoint32W", hdc, 0, 0, c,
                            likelyByArea, likelyDialogueLatinOrig,
                            useDialogueTuning, performTranslation, NULL,
                            lpString);
  }

  BOOL result = Real_GetTextExtentPoint32W(hdc, lpString, c, lpSize);
  if (result && lpSize && useDialogueTuning) {
    lpSize->cx = (lpSize->cx * 35 + 50) / 100;
    if (lpSize->cx < c)
      lpSize->cx = c;
  }

  (void)useDialogueTuning;
  return result;
}

// GetTextExtentExPoint Hooks
static decltype(&GetTextExtentExPointA) Real_GetTextExtentExPointA =
    GetTextExtentExPointA;
static decltype(&GetTextExtentExPointW) Real_GetTextExtentExPointW =
    GetTextExtentExPointW;

static BOOL WINAPI Hook_GetTextExtentExPointA(HDC hdc, LPCSTR lpszString,
                                              int cchString, int nMaxExtent,
                                              LPINT lpnFit, LPINT lpnDx,
                                              LPSIZE lpSize) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_GetTextExtentExPointA(hdc, lpszString, cchString, nMaxExtent,
                                      lpnFit, lpnDx, lpSize);
  if (!g_inBacklogRender)
    return Real_GetTextExtentExPointA(hdc, lpszString, cchString, nMaxExtent,
                                      lpnFit, lpnDx, lpSize);
  if (!g_inBacklogRender && g_dialogueSafePassthrough) {
    BOOL result = Real_GetTextExtentExPointA(hdc, lpszString, cchString,
                                             nMaxExtent, lpnFit, lpnDx, lpSize);
    UINT len = (UINT)((cchString > 0)
                          ? cchString
                          : (lpszString ? lstrlenA(lpszString) : 0));
    if (result && g_fontManager.GetLanguage() != 0 && lpszString &&
        IsLikelyDialogueLatinA(lpszString, len)) {
      CompressCumulativeDxA(lpnDx, len, lpszString, 35, 8);
      if (lpSize) {
        lpSize->cx = lpnDx && len > 0 ? lpnDx[len - 1]
                                      : (lpSize->cx * 35 + 50) / 100;
      }
    }
    return result;
  }
  int actualLen = lstrlenA(lpszString);
  if (cchString > 0 && actualLen > cchString && actualLen < cchString + 150)
    cchString = actualLen;

  UINT filteredLen = cchString;
  UINT latinProbeLen = (cchString > 0) ? (UINT)cchString : (UINT)lstrlenA(lpszString);
  bool likelyByArea =
      (!g_inBacklogRender && IsLikelyDialogueMetricsContext(hdc));
  bool likelyDialogueLatinOrig =
      g_inBacklogRender || likelyByArea ||
      IsLikelyDialogueLatinA(lpszString, latinProbeLen);
  bool performTranslation =
      (!(g_inBacklogRender && g_fontManager.GetDisableBacklogTranslation()) &&
       likelyDialogueLatinOrig);
  lpszString = FilterSjisString(lpszString, &filteredLen, NULL, NULL,
                                performTranslation, performTranslation);
  cchString = filteredLen;
  bool useDialogueTuning =
      (!g_inBacklogRender &&
       (likelyByArea || IsLikelyDialogueLatinA(lpszString, filteredLen)));
  if (!g_inBacklogRender && lpszString && cchString > 0 &&
      (likelyByArea || useDialogueTuning) && ShouldSamplePathTelemetry()) {
    LogRenderPathTelemetryA("GetTextExtentExPointA", hdc, 0, 0, cchString,
                            likelyByArea, likelyDialogueLatinOrig,
                            useDialogueTuning, performTranslation, NULL,
                            lpszString);
  }

  static thread_local HDC s_lastHdc_GetTextExtentEx = NULL;
  static thread_local HFONT s_lastBgFont_GetTextExtentEx = NULL;
  static thread_local int s_lastTmHeight_GetTextExtentEx = 0;

  HFONT currFont = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
  if (hdc != s_lastHdc_GetTextExtentEx ||
      currFont != s_lastBgFont_GetTextExtentEx ||
      s_lastHdc_GetTextExtentEx == NULL) {
    TEXTMETRICA tm;
    Real_GetTextMetricsA(hdc, &tm);
    s_lastTmHeight_GetTextExtentEx = tm.tmHeight;
    s_lastHdc_GetTextExtentEx = hdc;
    s_lastBgFont_GetTextExtentEx = currFont;
  }

  bool isName = false;
  if (g_inBacklogRender && !IsUiElement(lpszString, cchString)) {
    isName = (s_lastTmHeight_GetTextExtentEx <= 18);
  }

  int nameSpacing = isName ? g_fontManager.GetBacklogNameSpacing() : 0;
  if (g_inBacklogRender && g_fontManager.GetDisableBacklogSpacing()) {
    nameSpacing = 0;
  }

  HFONT customFont = NULL;
  HFONT oldFont = NULL;

  bool hasCustomFont = false;
  if (isName) {
    hasCustomFont =
        ((g_fontManager.GetBacklogNameFontSize() != 0) ||
         (g_fontManager.GetBacklogNameFontName() != L"MS Gothic" &&
          g_fontManager.GetBacklogNameFontName() != L"ＭＳ ゴシック") ||
         (nameSpacing != 0));
  } else if (g_inBacklogRender) {
    hasCustomFont = ((g_fontManager.GetBacklogFontSize() != 0) ||
                     (g_fontManager.GetBacklogFontName() != L"MS Gothic" &&
                      g_fontManager.GetBacklogFontName() != L"ＭＳ ゴシック"));
  }

  if (g_inBacklogRender && g_fontManager.GetDisableBacklogFont()) {
    hasCustomFont = false;
  }

  if (hasCustomFont) {
    if (isName) {
      customFont =
          g_fontManager.GetBacklogNameFont(s_lastTmHeight_GetTextExtentEx);
    } else {
      customFont = g_fontManager.GetBacklogFont(s_lastTmHeight_GetTextExtentEx);
    }
    oldFont = (HFONT)SelectObject(hdc, customFont);
  }

  if (nameSpacing != 0) {
    SetTextCharacterExtra(hdc, nameSpacing);
  }

  BOOL result = Real_GetTextExtentExPointA(hdc, lpszString, cchString,
                                           nMaxExtent, lpnFit, lpnDx, lpSize);
  if (result && useDialogueTuning) {
    CompressCumulativeDxA(lpnDx, (UINT)cchString, lpszString, 35, 8);
    if (lpSize) {
      lpSize->cx = lpnDx && cchString > 0 ? lpnDx[cchString - 1]
                                          : (lpSize->cx * 35 + 50) / 100;
    }
  }

  (void)useDialogueTuning;

  if (!g_fontManager.GetDisableBacklogSpacing()) {
    if (result && g_inBacklogRender && !isName && lpSize != NULL) {
      lpSize->cy += g_fontManager.GetBacklogLineSpacing();
    }
  }

  if (nameSpacing != 0) {
    SetTextCharacterExtra(hdc, 0);
  }
  if (oldFont) {
    SelectObject(hdc, oldFont);
  }
  return result;
}

static BOOL WINAPI Hook_GetTextExtentExPointW(HDC hdc, LPCWSTR lpszString,
                                              int cchString, int nMaxExtent,
                                              LPINT lpnFit, LPINT lpnDx,
                                              LPSIZE lpSize) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_GetTextExtentExPointW(hdc, lpszString, cchString, nMaxExtent,
                                      lpnFit, lpnDx, lpSize);
  if (!g_inBacklogRender)
    return Real_GetTextExtentExPointW(hdc, lpszString, cchString, nMaxExtent,
                                      lpnFit, lpnDx, lpSize);
  if (!g_inBacklogRender && g_dialogueSafePassthrough) {
    UINT renderLen = (UINT)((cchString > 0) ? cchString : lstrlenW(lpszString));
    LPCWSTR renderStr =
        FilterWideString(lpszString, &renderLen, NULL, NULL, true);
    if (!renderStr)
      renderStr = lpszString;
    BOOL result = Real_GetTextExtentExPointW(hdc, renderStr, (int)renderLen,
                                             nMaxExtent, lpnFit, lpnDx, lpSize);
    if (result && g_fontManager.GetLanguage() != 0 && renderStr &&
        IsLikelyDialogueLatinW(renderStr, renderLen)) {
      CompressCumulativeDxW(lpnDx, renderLen, renderStr, 35, 8);
      if (lpSize) {
        lpSize->cx = lpnDx && renderLen > 0
                         ? lpnDx[renderLen - 1]
                         : (lpSize->cx * 35 + 50) / 100;
      }
    }
    return result;
  }
  int actualLen = lstrlenW(lpszString);
  if (cchString > 0 && actualLen > cchString && actualLen < cchString + 150)
    cchString = actualLen;

  UINT filteredLen = (UINT)cchString;
  UINT latinProbeLen =
      (cchString > 0) ? (UINT)cchString : (UINT)lstrlenW(lpszString);
  bool likelyByArea =
      (!g_inBacklogRender && IsLikelyDialogueMetricsContext(hdc));
  bool likelyDialogueLatinOrig =
      g_inBacklogRender || likelyByArea ||
      IsLikelyDialogueLatinW(lpszString, latinProbeLen);
  bool performTranslation =
      (!(g_inBacklogRender && g_fontManager.GetDisableBacklogTranslation()) &&
       likelyDialogueLatinOrig);
  lpszString = FilterWideString(lpszString, &filteredLen, NULL, NULL,
                                performTranslation);
  cchString = (int)filteredLen;
  bool useDialogueTuning =
      (!g_inBacklogRender &&
       (likelyByArea || IsLikelyDialogueLatinW(lpszString, filteredLen)));
  if (!g_inBacklogRender && lpszString && cchString > 0 &&
      (likelyByArea || useDialogueTuning) && ShouldSamplePathTelemetry()) {
    LogRenderPathTelemetryW("GetTextExtentExPointW", hdc, 0, 0, cchString,
                            likelyByArea, likelyDialogueLatinOrig,
                            useDialogueTuning, performTranslation, NULL,
                            lpszString);
  }

  BOOL result = Real_GetTextExtentExPointW(hdc, lpszString, cchString,
                                           nMaxExtent, lpnFit, lpnDx, lpSize);
  if (result && useDialogueTuning) {
    CompressCumulativeDxW(lpnDx, (UINT)cchString, lpszString, 35, 8);
    if (lpSize) {
      lpSize->cx = lpnDx && cchString > 0 ? lpnDx[cchString - 1]
                                          : (lpSize->cx * 35 + 50) / 100;
    }
  }
  (void)useDialogueTuning;
  return result;
}

// Helper to force proportional widths for fixed-width Japanese fonts
static int GetProportionalAdvanceWidth(HDC hdc, UINT uChar, int fontHeight) {
  int hint = fontHeight;
  if (hint <= 0)
    hint = g_fontManager.GetDialogueGlyphHeightHint();
  if (hint <= 0)
    hint = 19;
  int step = (hint * g_fontManager.GetDialogueAdvanceScale() + 50) / 100;
  // ADV32 dialogue path applies additional internal scaling. Keep compact
  // advances here to avoid "P e m a n d a n g a n" style spacing.
  step = (step * 22 + 50) / 100;
  if (step < 1)
    step = 1;
  if (step > 4)
    step = 4;

  if (uChar == ' ') {
    int w = (step * g_fontManager.GetDialogueSpaceScale() + 50) / 100;
    w = (w * 20 + 50) / 100;
    if (w < 0)
      w = 0;
    if (w > 1)
      w = 1;
    return w;
  }

  if (uChar == 'i' || uChar == 'l' || uChar == 'I' || uChar == '.' ||
      uChar == ',' || uChar == '\'' || uChar == '!' || uChar == ';' ||
      uChar == ':') {
    int w = (step * 65 + 50) / 100;
    if (w < 1)
      w = 1;
    if (w > 2)
      w = 2;
    return w;
  }

  if (uChar == 'm' || uChar == 'w' || uChar == 'M' || uChar == 'W' ||
      uChar == '@') {
    int w = (step * 95 + 50) / 100;
    if (w < 1)
      w = 1;
    if (w > 3)
      w = 3;
    return w;
  }

  int w = (step * 78 + 50) / 100;
  if (w < 1)
    w = 1;
  if (w > 2)
    w = 2;
  return w;
}

static int GetDialogueAdvanceStep() {
  int h = g_fontManager.GetDialogueGlyphHeightHint();
  int scale = g_fontManager.GetDialogueAdvanceScale();
  int step = (h * scale + 50) / 100;
  step = (step * 16 + 50) / 100;
  if (step < 1)
    step = 1;
  if (step > 2)
    step = 2;
  return step;
}

static int GetDialogueCharExtraTight() {
  int h = g_fontManager.GetDialogueGlyphHeightHint();
  if (h <= 0)
    h = 18;
  int extra = -((h * 60 + 50) / 100);
  if (extra > -4)
    extra = -4;
  if (extra < -18)
    extra = -18;
  return extra;
}

// GetCharWidth32 Hooks
static decltype(&GetCharWidth32A) Real_GetCharWidth32A = GetCharWidth32A;
static decltype(&GetCharWidth32W) Real_GetCharWidth32W = GetCharWidth32W;
static decltype(&GetCharABCWidthsA) Real_GetCharABCWidthsA = GetCharABCWidthsA;
static decltype(&GetCharABCWidthsW) Real_GetCharABCWidthsW = GetCharABCWidthsW;

static BOOL WINAPI Hook_GetCharWidth32A(HDC hdc, UINT iFirst, UINT iLast,
                                        LPINT lpBuffer) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_GetCharWidth32A(hdc, iFirst, iLast, lpBuffer);
  if (InPopupUiLatinRender() && lpBuffer) {
    BOOL res = Real_GetCharWidth32A(hdc, iFirst, iLast, lpBuffer);
    if (res) {
      for (UINT i = iFirst; i <= iLast; i++) {
        if (i >= 0x20 && i <= 0x7E)
          lpBuffer[i - iFirst] = GetPopupUiGlyphAdvance(i);
      }
    }
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpBuffer &&
      IsLikelyChoiceGlyphContext(hdc)) {
    BOOL res = Real_GetCharWidth32A(hdc, iFirst, iLast, lpBuffer);
    if (res) {
      for (UINT i = iFirst; i <= iLast; i++) {
        if (IsChoiceGlyphA(i) || IsChoiceSpacerGlyph(i))
          lpBuffer[i - iFirst] = GetChoiceGlyphAdvance(i);
      }
    }
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpBuffer &&
      IsChoiceLargeFontContext(hdc) && iFirst <= '~' && iLast >= '~') {
    BOOL res = Real_GetCharWidth32A(hdc, iFirst, iLast, lpBuffer);
    if (res)
      lpBuffer['~' - iFirst] = 1;
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpBuffer &&
      (InChoiceGlyphRun(hdc) || IsChoiceMarkerGlyph(iFirst) ||
       IsChoiceMarkerGlyph(iLast))) {
    BOOL res = Real_GetCharWidth32A(hdc, iFirst, iLast, lpBuffer);
    if (res) {
      if (IsChoiceMarkerGlyph(iFirst) || IsChoiceMarkerGlyph(iLast))
        StartChoiceGlyphRun(hdc);
      for (UINT i = iFirst; i <= iLast; i++) {
        if (IsChoiceGlyphA(i) || IsChoiceSpacerGlyph(i))
          lpBuffer[i - iFirst] = GetChoiceGlyphAdvance(i);
      }
    }
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpBuffer &&
      IsReplayRawGlyphContext(hdc)) {
    BOOL res = Real_GetCharWidth32A(hdc, iFirst, iLast, lpBuffer);
    if (res) {
      for (UINT i = iFirst; i <= iLast; i++) {
        if (i >= 0x20 && i <= 0x7E)
          lpBuffer[i - iFirst] = GetReplayRawGlyphAdvance(i);
      }
    }
    return res;
  }
  if (!g_inBacklogRender)
    return Real_GetCharWidth32A(hdc, iFirst, iLast, lpBuffer);
  BOOL res = Real_GetCharWidth32A(hdc, iFirst, iLast, lpBuffer);
  if (res && !g_inBacklogRender) {
    TEXTMETRICA tm;
    if (Real_GetTextMetricsA(hdc, &tm)) {
      for (UINT i = iFirst; i <= iLast; i++) {
        if (i >= 0x20 && i <= 0x7E) {
          lpBuffer[i - iFirst] =
              GetProportionalAdvanceWidth(hdc, i, tm.tmHeight);
        }
      }
    }
  }
  return res;
}

static BOOL WINAPI Hook_GetCharWidth32W(HDC hdc, UINT iFirst, UINT iLast,
                                        LPINT lpBuffer) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc))
    return Real_GetCharWidth32W(hdc, iFirst, iLast, lpBuffer);
  if (InPopupUiLatinRender() && lpBuffer) {
    BOOL res = Real_GetCharWidth32W(hdc, iFirst, iLast, lpBuffer);
    if (res) {
      for (UINT i = iFirst; i <= iLast; i++) {
        UINT converted = ConvertFullWidthToHalfWidth(i);
        if ((i >= 0x20 && i <= 0x7E) || converted <= 0x7E)
          lpBuffer[i - iFirst] = GetPopupUiGlyphAdvance(i);
      }
    }
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpBuffer &&
      IsLikelyChoiceGlyphContext(hdc)) {
    BOOL res = Real_GetCharWidth32W(hdc, iFirst, iLast, lpBuffer);
    if (res) {
      for (UINT i = iFirst; i <= iLast; i++) {
        UINT converted = ConvertFullWidthToHalfWidth(i);
        if (IsChoiceGlyphW(i) || IsChoiceSpacerGlyph(i) ||
            (converted >= 0x20 && converted <= 0x7E))
          lpBuffer[i - iFirst] = GetChoiceGlyphAdvance(converted <= 0x7E ? converted : i);
      }
    }
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpBuffer &&
      IsChoiceLargeFontContext(hdc) && iFirst <= L'~' && iLast >= L'~') {
    BOOL res = Real_GetCharWidth32W(hdc, iFirst, iLast, lpBuffer);
    if (res)
      lpBuffer[L'~' - iFirst] = 1;
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpBuffer &&
      (InChoiceGlyphRun(hdc) || IsChoiceMarkerGlyph(iFirst) ||
       IsChoiceMarkerGlyph(iLast))) {
    BOOL res = Real_GetCharWidth32W(hdc, iFirst, iLast, lpBuffer);
    if (res) {
      if (IsChoiceMarkerGlyph(iFirst) || IsChoiceMarkerGlyph(iLast))
        StartChoiceGlyphRun(hdc);
      for (UINT i = iFirst; i <= iLast; i++) {
        if (IsChoiceGlyphW(i) || IsChoiceSpacerGlyph(i))
          lpBuffer[i - iFirst] = GetChoiceGlyphAdvance(i);
      }
    }
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpBuffer &&
      IsReplayRawGlyphContext(hdc)) {
    BOOL res = Real_GetCharWidth32W(hdc, iFirst, iLast, lpBuffer);
    if (res) {
      for (UINT i = iFirst; i <= iLast; i++) {
        UINT converted = ConvertFullWidthToHalfWidth(i);
        if (converted >= 0x20 && converted <= 0x7E)
          lpBuffer[i - iFirst] = GetReplayRawGlyphAdvance(converted);
      }
    }
    return res;
  }
  if (!g_inBacklogRender)
    return Real_GetCharWidth32W(hdc, iFirst, iLast, lpBuffer);
  BOOL res = Real_GetCharWidth32W(hdc, iFirst, iLast, lpBuffer);
  if (res && !g_inBacklogRender) {
    TEXTMETRICA tm;
    if (Real_GetTextMetricsA(hdc, &tm)) {
      for (UINT i = iFirst; i <= iLast; i++) {
        if (i >= 0x20 && i <= 0x7E) {
          lpBuffer[i - iFirst] =
              GetProportionalAdvanceWidth(hdc, i, tm.tmHeight);
        }
      }
    }
  }
  return res;
}

static BOOL WINAPI Hook_GetCharABCWidthsA(HDC hdc, UINT iFirst, UINT iLast,
                                          LPABC lpabc) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) ||
      IsVersionInfoDialogDrawContext(hdc))
    return Real_GetCharABCWidthsA(hdc, iFirst, iLast, lpabc);
  if (InPopupUiLatinRender() && lpabc) {
    BOOL res = Real_GetCharABCWidthsA(hdc, iFirst, iLast, lpabc);
    if (res) {
      for (UINT i = iFirst; i <= iLast; i++) {
        if (i >= 0x20 && i <= 0x7E) {
          lpabc[i - iFirst].abcA = 0;
          lpabc[i - iFirst].abcB = GetPopupUiGlyphAdvance(i);
          lpabc[i - iFirst].abcC = 0;
        }
      }
    }
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpabc &&
      IsLikelyChoiceGlyphContext(hdc)) {
    BOOL res = Real_GetCharABCWidthsA(hdc, iFirst, iLast, lpabc);
    if (res) {
      for (UINT i = iFirst; i <= iLast; i++) {
        if (IsChoiceGlyphA(i) || IsChoiceSpacerGlyph(i)) {
          lpabc[i - iFirst].abcA = 0;
          lpabc[i - iFirst].abcB = GetChoiceGlyphAdvance(i);
          lpabc[i - iFirst].abcC = 0;
        }
      }
    }
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpabc &&
      IsChoiceLargeFontContext(hdc) && iFirst <= '~' && iLast >= '~') {
    BOOL res = Real_GetCharABCWidthsA(hdc, iFirst, iLast, lpabc);
    if (res) {
      lpabc['~' - iFirst].abcA = 0;
      lpabc['~' - iFirst].abcB = 1;
      lpabc['~' - iFirst].abcC = 0;
    }
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpabc &&
      (InChoiceGlyphRun(hdc) || IsChoiceMarkerGlyph(iFirst) ||
       IsChoiceMarkerGlyph(iLast))) {
    BOOL res = Real_GetCharABCWidthsA(hdc, iFirst, iLast, lpabc);
    if (res) {
      if (IsChoiceMarkerGlyph(iFirst) || IsChoiceMarkerGlyph(iLast))
        StartChoiceGlyphRun(hdc);
      for (UINT i = iFirst; i <= iLast; i++) {
        if (IsChoiceGlyphA(i) || IsChoiceSpacerGlyph(i)) {
          lpabc[i - iFirst].abcA = 0;
          lpabc[i - iFirst].abcB = GetChoiceGlyphAdvance(i);
          lpabc[i - iFirst].abcC = 0;
        }
      }
    }
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpabc &&
      IsReplayRawGlyphContext(hdc)) {
    BOOL res = Real_GetCharABCWidthsA(hdc, iFirst, iLast, lpabc);
    if (res) {
      for (UINT i = iFirst; i <= iLast; i++) {
        if (i >= 0x20 && i <= 0x7E) {
          lpabc[i - iFirst].abcA = 0;
          lpabc[i - iFirst].abcB = GetReplayRawGlyphAdvance(i);
          lpabc[i - iFirst].abcC = 0;
        }
      }
    }
    return res;
  }
  if (!g_inBacklogRender)
    return Real_GetCharABCWidthsA(hdc, iFirst, iLast, lpabc);
  BOOL res = Real_GetCharABCWidthsA(hdc, iFirst, iLast, lpabc);
  if (res && !g_inBacklogRender && lpabc) {
    TEXTMETRICA tm;
    if (Real_GetTextMetricsA(hdc, &tm)) {
      for (UINT i = iFirst; i <= iLast; i++) {
        if (i >= 0x20 && i <= 0x7E) {
          int w = GetProportionalAdvanceWidth(hdc, i, tm.tmHeight);
          lpabc[i - iFirst].abcA = 0;
          lpabc[i - iFirst].abcB = w;
          lpabc[i - iFirst].abcC = 0;
        }
      }
    }
  }
  return res;
}

static BOOL WINAPI Hook_GetCharABCWidthsW(HDC hdc, UINT iFirst, UINT iLast,
                                          LPABC lpabc) {
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) ||
      IsVersionInfoDialogDrawContext(hdc))
    return Real_GetCharABCWidthsW(hdc, iFirst, iLast, lpabc);
  if (InPopupUiLatinRender() && lpabc) {
    BOOL res = Real_GetCharABCWidthsW(hdc, iFirst, iLast, lpabc);
    if (res) {
      for (UINT i = iFirst; i <= iLast; i++) {
        UINT converted = ConvertFullWidthToHalfWidth(i);
        if ((i >= 0x20 && i <= 0x7E) || converted <= 0x7E) {
          lpabc[i - iFirst].abcA = 0;
          lpabc[i - iFirst].abcB = GetPopupUiGlyphAdvance(i);
          lpabc[i - iFirst].abcC = 0;
        }
      }
    }
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpabc &&
      IsLikelyChoiceGlyphContext(hdc)) {
    BOOL res = Real_GetCharABCWidthsW(hdc, iFirst, iLast, lpabc);
    if (res) {
      for (UINT i = iFirst; i <= iLast; i++) {
        UINT converted = ConvertFullWidthToHalfWidth(i);
        if (IsChoiceGlyphW(i) || IsChoiceSpacerGlyph(i) ||
            (converted >= 0x20 && converted <= 0x7E)) {
          lpabc[i - iFirst].abcA = 0;
          lpabc[i - iFirst].abcB =
              GetChoiceGlyphAdvance(converted <= 0x7E ? converted : i);
          lpabc[i - iFirst].abcC = 0;
        }
      }
    }
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpabc &&
      IsChoiceLargeFontContext(hdc) && iFirst <= L'~' && iLast >= L'~') {
    BOOL res = Real_GetCharABCWidthsW(hdc, iFirst, iLast, lpabc);
    if (res) {
      lpabc[L'~' - iFirst].abcA = 0;
      lpabc[L'~' - iFirst].abcB = 1;
      lpabc[L'~' - iFirst].abcC = 0;
    }
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpabc &&
      (InChoiceGlyphRun(hdc) || IsChoiceMarkerGlyph(iFirst) ||
       IsChoiceMarkerGlyph(iLast))) {
    BOOL res = Real_GetCharABCWidthsW(hdc, iFirst, iLast, lpabc);
    if (res) {
      if (IsChoiceMarkerGlyph(iFirst) || IsChoiceMarkerGlyph(iLast))
        StartChoiceGlyphRun(hdc);
      for (UINT i = iFirst; i <= iLast; i++) {
        if (IsChoiceGlyphW(i) || IsChoiceSpacerGlyph(i)) {
          lpabc[i - iFirst].abcA = 0;
          lpabc[i - iFirst].abcB = GetChoiceGlyphAdvance(i);
          lpabc[i - iFirst].abcC = 0;
        }
      }
    }
    return res;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 && lpabc &&
      IsReplayRawGlyphContext(hdc)) {
    BOOL res = Real_GetCharABCWidthsW(hdc, iFirst, iLast, lpabc);
    if (res) {
      for (UINT i = iFirst; i <= iLast; i++) {
        UINT converted = ConvertFullWidthToHalfWidth(i);
        if (converted >= 0x20 && converted <= 0x7E) {
          lpabc[i - iFirst].abcA = 0;
          lpabc[i - iFirst].abcB = GetReplayRawGlyphAdvance(converted);
          lpabc[i - iFirst].abcC = 0;
        }
      }
    }
    return res;
  }
  if (!g_inBacklogRender)
    return Real_GetCharABCWidthsW(hdc, iFirst, iLast, lpabc);
  BOOL res = Real_GetCharABCWidthsW(hdc, iFirst, iLast, lpabc);
  if (res && !g_inBacklogRender && lpabc) {
    TEXTMETRICA tm;
    if (Real_GetTextMetricsA(hdc, &tm)) {
      for (UINT i = iFirst; i <= iLast; i++) {
        if (i >= 0x20 && i <= 0x7E) {
          int w = GetProportionalAdvanceWidth(hdc, i, tm.tmHeight);
          lpabc[i - iFirst].abcA = 0;
          lpabc[i - iFirst].abcB = w;
          lpabc[i - iFirst].abcC = 0;
        }
      }
    }
  }
  return res;
}

// Convert full-width SJIS character codes to half-width ASCII.
// The MesScript tool encodes ASCII as full-width so the engine's
// 2-byte-per-char parser can read it. We convert back here to get
// narrow, proportional glyphs from the font.
static UINT ConvertFullWidthToHalfWidth(UINT sjisChar) {
  // Already a single-byte char
  if ((sjisChar >> 8) == 0x00)
    return sjisChar;

  char sjisBytes[3];
  sjisBytes[0] = (char)(sjisChar >> 8);
  sjisBytes[1] = (char)(sjisChar & 0xFF);
  sjisBytes[2] = 0;

  wchar_t wc[2] = {0};
  if (MultiByteToWideChar(932, 0, sjisBytes, 2, wc, 2) >= 1) {
    // Full-width ASCII: U+FF01 ('!') through U+FF5E ('~')
    if (wc[0] >= 0xFF01 && wc[0] <= 0xFF5E) {
      return (UINT)(wc[0] - 0xFEE0); // Convert to half-width ASCII
    }
    // Full-width space U+3000 -> ASCII space
    if (wc[0] == 0x3000) {
      return (UINT)' ';
    }
  }
  return sjisChar; // Not full-width ASCII, return as-is
}

static bool IsDialogueAsciiGlyph(UINT ch) {
  return ch >= 0x20 && ch <= 0x7E;
}

static DWORD NormalizeDialogueLatinGlyphBitmap(UINT ch, UINT fuFormat,
                                               LPGLYPHMETRICS gm, DWORD r,
                                               DWORD cjBuffer, LPVOID pvBuffer) {
  if (!gm || g_inBacklogRender || g_fontManager.GetLanguage() == 0 ||
      !IsDialogueAsciiGlyph(ch))
    return r;

  (void)fuFormat;
  (void)cjBuffer;
  (void)pvBuffer;

  if (gm->gmptGlyphOrigin.x > 1)
    gm->gmptGlyphOrigin.x = 1;
  if (gm->gmptGlyphOrigin.x < -1)
    gm->gmptGlyphOrigin.x = -1;

  // ADV32 positions each glyph bitmap manually. A fixed baseline still leaves
  // Latin lowercase visually wavy because x-height/ascenders produce different
  // black-box tops. Align the visible top of the glyph to a small fixed inset
  // instead; this keeps "Seorang diri" and "konyol" from looking like every
  // character sits on a different vertical track.
  int topInset = 3;
  int visualOriginY = (int)gm->gmBlackBoxY + topInset;
  if (visualOriginY < 20)
    visualOriginY = 20;
  if (visualOriginY > 26)
    visualOriginY = 26;
  gm->gmptGlyphOrigin.y = visualOriginY;

  // Very narrow stems are hard to see after ADV32 caches/scales the bitmap.
  // Keep enough cell width so i/l do not collapse into punctuation-like dots.
  if ((ch == 'i' || ch == 'l' || ch == 'I') && gm->gmCellIncX < 10)
    gm->gmCellIncX = 10;
  return r;
}

static LONG FixedToRaw(FIXED v) {
  return ((LONG)v.value << 16) | (LONG)v.fract;
}

static FIXED RawToFixed(LONG raw) {
  FIXED v;
  v.value = (SHORT)(raw >> 16);
  v.fract = (WORD)(raw & 0xFFFF);
  return v;
}

static FIXED ScaleFixed(FIXED v, int percent) {
  LONG raw = FixedToRaw(v);
  raw = (LONG)(((LONGLONG)raw * percent + 50) / 100);
  return RawToFixed(raw);
}

static MAT2 MakeDialogueGlyphMat2(const MAT2 *src, int percent) {
  MAT2 m = {};
  if (src) {
    m = *src;
  } else {
    m.eM11.value = 1;
    m.eM22.value = 1;
  }
  m.eM11 = ScaleFixed(m.eM11, percent);
  m.eM22 = ScaleFixed(m.eM22, percent);
  return m;
}

static MAT2 MakeDialogueGlyphMat2XY(const MAT2 *src, int xPercent,
                                    int yPercent) {
  MAT2 m = {};
  if (src) {
    m = *src;
  } else {
    m.eM11.value = 1;
    m.eM22.value = 1;
  }
  m.eM11 = ScaleFixed(m.eM11, xPercent);
  m.eM22 = ScaleFixed(m.eM22, yPercent);
  return m;
}


static DWORD WINAPI Hook_GetGlyphOutlineA(HDC hdc, UINT uChar, UINT fuFormat,
                                          LPGLYPHMETRICS lpgm, DWORD cjBuffer,
                                          LPVOID pvBuffer, const MAT2 *lpmat2) {
  if (InHsxf4LatinRender()) {
    if (IsNonRenderableTextControlA(uChar))
      return ReturnEmptyChoiceGlyph(lpgm, cjBuffer, pvBuffer);
    UINT glyphChar = NormalizeHsxf4GlyphChar(uChar);
    if (g_hsxf4GlyphOverride != 0)
      glyphChar = g_hsxf4GlyphOverride;
    if (g_hsxf4TriplePauseGlyph)
      glyphChar = '.';
    else if (g_hsxf4EllipsisDotRender && glyphChar == '.')
      glyphChar = kHsxf4LatinLowLine;
    HFONT oldFont = SelectHsxf4LatinFont(hdc, glyphChar);
    MAT2 symbolMat = {};
    const MAT2 *glyphMat = lpmat2;
    if (IsHsxf4LatinCompactSymbol((int)glyphChar)) {
      symbolMat = MakeDialogueGlyphMat2(lpmat2, 72);
      glyphMat = &symbolMat;
    }
    DWORD result =
        (glyphChar == 0x2026 || glyphChar == 0x00D7 || glyphChar > 0xFF)
            ? Real_GetGlyphOutlineW(hdc, glyphChar, fuFormat, lpgm, cjBuffer,
                                    pvBuffer, glyphMat)
            : Real_GetGlyphOutlineA(hdc, glyphChar, fuFormat, lpgm, cjBuffer,
                                    pvBuffer, glyphMat);
    if (oldFont)
      SelectObject(hdc, oldFont);
    if (result != GDI_ERROR && lpgm) {
      TuneHsxf4LatinGlyphMetrics(glyphChar, lpgm);
      ShiftLongNameplateGlyph(lpgm);
      if (g_hsxf4TriplePauseGlyph && glyphChar == '.')
        result = BuildHsxf4TripleDotGlyphBitmap(fuFormat, lpgm, result,
                                                cjBuffer, pvBuffer);
      if (!g_hsxf4ChoiceRender && !g_hsxf4PopupRender &&
          !g_hsxf4ReplayRender)
        EmboldenHsxf4LatinGlyphBitmap(fuFormat, lpgm, result, cjBuffer,
                                      pvBuffer, glyphChar);
    }
    if (result != GDI_ERROR && lpgm && glyphChar >= 0x20 && glyphChar <= 0x7E) {
      LogGlyphPathTelemetry("GetGlyphOutlineA_HSXF4", hdc, glyphChar, lpgm, true);
    }
    return result;
  }
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) || IsVersionInfoDialogDrawContext(hdc)) {
    return Real_GetGlyphOutlineA(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer,
                                 lpmat2);
  }
  if (InPopupUiLatinRender()) {
    UINT glyphChar = ConvertFullWidthToHalfWidth(uChar);
    HFONT oldFont = SelectPopupUiTextFont(hdc);
    DWORD result = Real_GetGlyphOutlineA(hdc, glyphChar, fuFormat, lpgm,
                                         cjBuffer, pvBuffer, lpmat2);
    if (oldFont)
      SelectObject(hdc, oldFont);
    if (result != GDI_ERROR && lpgm) {
      TunePopupUiGlyphMetrics(glyphChar, lpgm);
      EmboldenHsxf4LatinGlyphBitmap(fuFormat, lpgm, result, cjBuffer,
                                    pvBuffer, glyphChar);
    }
    return result;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      IsReplayRawGlyphContext(hdc) && uChar >= 0x20 && uChar <= 0x7E) {
    TrackReplayGlyphW(hdc, uChar);
    HFONT oldFont = SelectReplayTextFont(hdc, -8);
    DWORD result = Real_GetGlyphOutlineA(hdc, uChar, fuFormat, lpgm, cjBuffer,
                                         pvBuffer, lpmat2);
    if (oldFont)
      SelectObject(hdc, oldFont);
    if (result != GDI_ERROR && lpgm)
      TuneReplayRawGlyphMetrics(uChar, lpgm);
    return result;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      uChar == '~' && IsChoiceLargeFontContext(hdc)) {
    return RenderChoiceSpacerGlyph(hdc, uChar, fuFormat, lpgm, cjBuffer,
                                   pvBuffer, lpmat2);
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      (InChoiceGlyphRun(hdc) || IsLikelyChoiceGlyphContext(hdc))) {
    if (g_choiceGlyphPendingLeadHdc != NULL &&
        g_choiceGlyphPendingLeadHdc != hdc) {
      g_choiceGlyphPendingLead = 0;
      g_choiceGlyphPendingLeadHdc = NULL;
    }

    if (g_choiceGlyphPendingLead && IsSjisTrailByteForChoice(uChar)) {
      UINT pair = ((UINT)g_choiceGlyphPendingLead << 8) |
                  (UINT)(uChar & 0xFF);
      g_choiceGlyphPendingLead = 0;
      g_choiceGlyphPendingLeadHdc = NULL;

      if (IsChoiceMarkerGlyph(pair)) {
        StartChoiceGlyphRun(hdc);
        HFONT oldFont = SelectChoiceTextFont(hdc);
        MAT2 choiceMat = MakeDialogueGlyphMat2(lpmat2, 72);
        DWORD result = Real_GetGlyphOutlineA(hdc, pair, fuFormat, lpgm,
                                             cjBuffer, pvBuffer, &choiceMat);
        if (oldFont)
          SelectObject(hdc, oldFont);
        if (result != GDI_ERROR && lpgm)
          TuneChoiceGlyphMetrics(pair, lpgm);
        return result;
      }

      if (IsChoiceSpacerGlyph(pair)) {
        ConsumeChoiceGlyphRun();
        return RenderChoiceSpacerGlyph(hdc, pair, fuFormat, lpgm, cjBuffer,
                                       pvBuffer, lpmat2);
      }
    } else if (IsSjisLeadByteForChoice(uChar)) {
      g_choiceGlyphPendingLead = (unsigned char)(uChar & 0xFF);
      g_choiceGlyphPendingLeadHdc = hdc;
      return ReturnEmptyChoiceGlyph(lpgm, cjBuffer, pvBuffer);
    }

    if (IsChoiceSpacerGlyph(uChar) || uChar == ' ') {
      ConsumeChoiceGlyphRun();
      return RenderChoiceSpacerGlyph(hdc, uChar, fuFormat, lpgm, cjBuffer,
                                     pvBuffer, lpmat2);
    }

    if (IsChoiceMarkerGlyph(uChar)) {
      StartChoiceGlyphRun(hdc);
      HFONT oldFont = SelectChoiceTextFont(hdc);
      MAT2 choiceMat = MakeDialogueGlyphMat2(lpmat2, 72);
      DWORD result = Real_GetGlyphOutlineA(hdc, uChar, fuFormat, lpgm,
                                           cjBuffer, pvBuffer, &choiceMat);
      if (oldFont)
        SelectObject(hdc, oldFont);
      if (result != GDI_ERROR && lpgm)
        TuneChoiceGlyphMetrics(uChar, lpgm);
      return result;
    }

    if (InChoiceGlyphRun(hdc) && IsChoiceGlyphA(uChar)) {
      ConsumeChoiceGlyphRun();
      UINT choiceChar = DecodeChoiceRunGlyph(uChar);
      if (IsChoiceSpacerGlyph(choiceChar) || choiceChar == ' ') {
        return RenderChoiceSpacerGlyph(hdc, choiceChar, fuFormat, lpgm,
                                       cjBuffer, pvBuffer, lpmat2);
      }
      HFONT oldFont = SelectChoiceTextFont(hdc);
      MAT2 choiceMat = MakeDialogueGlyphMat2(lpmat2, 72);
      DWORD result = Real_GetGlyphOutlineA(hdc, choiceChar, fuFormat, lpgm,
                                           cjBuffer, pvBuffer, &choiceMat);
      if (oldFont)
        SelectObject(hdc, oldFont);
      if (result != GDI_ERROR && lpgm) {
        TuneChoiceGlyphMetrics(choiceChar, lpgm);
        LogGlyphPathTelemetry("GetGlyphOutlineA_CHOICE", hdc, choiceChar,
                              lpgm, true);
      }
      return result;
    }
  }
  if (false && !g_inBacklogRender && g_fontManager.GetLanguage() != 0) {
    if (IsChoiceMarkerGlyph(uChar)) {
      StartChoiceGlyphRun(hdc);
      HFONT oldFont = SelectChoiceTextFont(hdc);
      DWORD result = Real_GetGlyphOutlineA(hdc, uChar, fuFormat, lpgm,
                                           cjBuffer, pvBuffer, lpmat2);
      if (oldFont)
        SelectObject(hdc, oldFont);
      if (result != GDI_ERROR && lpgm) {
        TuneChoiceGlyphMetrics(uChar, lpgm);
        EmboldenHsxf4LatinGlyphBitmap(fuFormat, lpgm, result, cjBuffer,
                                      pvBuffer, uChar);
      }
      return result;
    }

    if (InChoiceGlyphRun(hdc)) {
      ConsumeChoiceGlyphRun();
      if (g_choiceGlyphPendingLeadHdc != NULL &&
          g_choiceGlyphPendingLeadHdc != hdc) {
        g_choiceGlyphPendingLead = 0;
        g_choiceGlyphPendingLeadHdc = NULL;
      }
      if (g_choiceGlyphPendingLead && IsSjisTrailByteForChoice(uChar)) {
        UINT pair = ((UINT)g_choiceGlyphPendingLead << 8) |
                    (UINT)(uChar & 0xFF);
        g_choiceGlyphPendingLead = 0;
        g_choiceGlyphPendingLeadHdc = NULL;
        if (IsChoiceSpacerGlyph(pair)) {
          return RenderChoiceSpacerGlyph(hdc, pair, fuFormat, lpgm, cjBuffer,
                                         pvBuffer, lpmat2);
        }
        if (IsChoiceMarkerGlyph(pair)) {
          HFONT oldFont = SelectChoiceTextFont(hdc);
          DWORD result = Real_GetGlyphOutlineA(hdc, pair, fuFormat, lpgm,
                                               cjBuffer, pvBuffer, lpmat2);
          if (oldFont)
            SelectObject(hdc, oldFont);
          if (result != GDI_ERROR && lpgm) {
            TuneChoiceGlyphMetrics(pair, lpgm);
            EmboldenHsxf4LatinGlyphBitmap(fuFormat, lpgm, result, cjBuffer,
                                          pvBuffer, pair);
          }
          return result;
        }
      } else if (IsSjisLeadByteForChoice(uChar)) {
        g_choiceGlyphPendingLead = (unsigned char)(uChar & 0xFF);
        g_choiceGlyphPendingLeadHdc = hdc;
        return ReturnEmptyChoiceGlyph(lpgm, cjBuffer, pvBuffer);
      } else {
        g_choiceGlyphPendingLead = 0;
        g_choiceGlyphPendingLeadHdc = NULL;
      }
      UINT choiceChar = DecodeChoiceRunGlyph(uChar);
      if (IsChoiceSpacerGlyph(choiceChar)) {
        return RenderChoiceSpacerGlyph(hdc, choiceChar, fuFormat, lpgm, cjBuffer,
                                       pvBuffer, lpmat2);
      }
      if ((choiceChar >= 0x20 && choiceChar <= 0x7E) ||
          IsChoiceMarkerGlyph(choiceChar)) {
        HFONT oldFont = SelectChoiceTextFont(hdc);
        DWORD result = Real_GetGlyphOutlineA(hdc, choiceChar, fuFormat, lpgm,
                                             cjBuffer, pvBuffer, lpmat2);
        if (oldFont)
          SelectObject(hdc, oldFont);
        if (result != GDI_ERROR && lpgm) {
          TuneChoiceGlyphMetrics(choiceChar, lpgm);
          EmboldenHsxf4LatinGlyphBitmap(fuFormat, lpgm, result, cjBuffer,
                                        pvBuffer, choiceChar);
        }
        return result;
      }
    }
  }
  if (!g_inBacklogRender && uChar == 0x8140) {
    DWORD result = Real_GetGlyphOutlineA(hdc, uChar, fuFormat, lpgm, cjBuffer,
                                         pvBuffer, lpmat2);
    if (result != GDI_ERROR && lpgm) {
      if (pvBuffer && result > 0)
        memset(pvBuffer, 0, result);
      TuneWideSpaceGlyphMetrics(lpgm);
    }
    return result;
  }
  if (!g_inBacklogRender && IsChoiceGlyphA(uChar) &&
      IsLikelyChoiceGlyphContext(hdc)) {
    HFONT oldFont = SelectChoiceTextFont(hdc);
    MAT2 choiceMat = MakeDialogueGlyphMat2(lpmat2, 72);
    DWORD result = Real_GetGlyphOutlineA(hdc, uChar, fuFormat, lpgm, cjBuffer,
                                         pvBuffer, &choiceMat);
    if (oldFont)
      SelectObject(hdc, oldFont);
    if (result != GDI_ERROR && lpgm) {
      TuneChoiceGlyphMetrics(uChar, lpgm);
      LogGlyphPathTelemetry("GetGlyphOutlineA_CHOICE", hdc, uChar, lpgm, true);
    }
    return result;
  }
  if (!g_inBacklogRender) {
    return Real_GetGlyphOutlineA(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer,
                                 lpmat2);
  }
  if (!g_inBacklogRender && g_dialogueSafePassthrough &&
      g_fontManager.GetLanguage() == 0 && !IsLikelyDialogueMetricsContext(hdc)) {
    return Real_GetGlyphOutlineA(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer,
                                 lpmat2);
  }
  // Some ADV32 paths feed SJIS as split bytes (lead then trail) into ANSI
  // glyph APIs. Rebuild the pair here so full-width Latin can be normalized.
  bool wasFullWidth = false;
  UINT originalChar = uChar;
  if (!g_inBacklogRender) {
    static thread_local unsigned char s_pendingLead = 0;
    static thread_local HDC s_pendingLeadHdc = NULL;
    unsigned char b = (unsigned char)(uChar & 0xFF);
    auto IsLeadByte = [](unsigned char x) -> bool {
      return ((x >= 0x81 && x <= 0x9F) || (x >= 0xE0 && x <= 0xFC));
    };
    auto IsTrailByte = [](unsigned char x) -> bool {
      return ((x >= 0x40 && x <= 0x7E) || (x >= 0x80 && x <= 0xFC));
    };
    auto IsLikelyOrphanFullWidthTrail = [](unsigned char x) -> bool {
      // These are common SJIS trail bytes for full-width space/dash variants.
      // If ADV32 calls the trail byte without its lead byte, it appears as a
      // tiny stray glyph in the textbox.
      return x == 0x40 || x == 0x5B || x == 0x5C || x == 0x7C;
    };

    if (s_pendingLeadHdc != NULL && s_pendingLeadHdc != hdc) {
      s_pendingLead = 0;
      s_pendingLeadHdc = NULL;
    }

    if (s_pendingLead != 0) {
      if (IsTrailByte(b)) {
        UINT sjisPair = ((UINT)s_pendingLead << 8) | b;
        s_pendingLead = 0;
        s_pendingLeadHdc = NULL;
        originalChar = sjisPair;
        UINT converted = ConvertFullWidthToHalfWidth(sjisPair);
        if (converted != sjisPair) {
          // Keep the visual glyph on ADV32's native full-width SJIS path.
          // The stable old path only tuned HSXF4 pitch; half-width/custom font
          // outlines caused the wavy/cut Latin glyphs reported in-game.
          uChar = sjisPair;
          wasFullWidth = true;
        } else {
          uChar = sjisPair;
        }
      } else {
        s_pendingLead = 0;
        s_pendingLeadHdc = NULL;
      }
    } else if ((uChar & 0xFF00) == 0x0000 && IsLeadByte(b)) {
      s_pendingLead = b;
      s_pendingLeadHdc = hdc;
      if (lpgm) {
        lpgm->gmBlackBoxX = 0;
        lpgm->gmBlackBoxY = 0;
        lpgm->gmptGlyphOrigin.x = 0;
        lpgm->gmptGlyphOrigin.y = 0;
        lpgm->gmCellIncX = 0;
        lpgm->gmCellIncY = 0;
      }
      if (pvBuffer && cjBuffer > 0) {
        memset(pvBuffer, 0, cjBuffer);
      }
      return 0;
    } else if ((uChar & 0xFF00) == 0x0000 && b >= 0x80 && b <= 0xFC &&
               g_fontManager.GetLanguage() != 0) {
      // Orphan high-byte in translated mode tends to render as floating marks.
      // Drop it so Latin dialogue does not get corrupted.
      if (lpgm) {
        lpgm->gmBlackBoxX = 0;
        lpgm->gmBlackBoxY = 0;
        lpgm->gmptGlyphOrigin.x = 0;
        lpgm->gmptGlyphOrigin.y = 0;
        lpgm->gmCellIncX = 0;
        lpgm->gmCellIncY = 0;
      }
      if (pvBuffer && cjBuffer > 0) {
        memset(pvBuffer, 0, cjBuffer);
      }
      return 0;
    } else if ((uChar & 0xFF00) == 0x0000 &&
               g_fontManager.GetLanguage() != 0 &&
               IsLikelyDialogueMetricsContext(hdc) &&
               IsLikelyOrphanFullWidthTrail(b)) {
      if (lpgm) {
        lpgm->gmBlackBoxX = 0;
        lpgm->gmBlackBoxY = 0;
        lpgm->gmptGlyphOrigin.x = 0;
        lpgm->gmptGlyphOrigin.y = 0;
        lpgm->gmCellIncX = 0;
        lpgm->gmCellIncY = 0;
      }
      if (pvBuffer && cjBuffer > 0) {
        memset(pvBuffer, 0, cjBuffer);
      }
      return 0;
    } else {
      UINT converted = ConvertFullWidthToHalfWidth(uChar);
      if (converted != uChar) {
        // Preserve full-width visual glyphs. HSXF4 controls spacing.
        wasFullWidth = true;
      }
    }
  }

  bool isKatakanaDot = (uChar == '_');
  bool tuneLatinGlyph = false;

  DWORD r = GDI_ERROR;

  static thread_local HDC s_lastHdc_GlyphOutline = NULL;
  static thread_local HFONT s_lastBgFont_GlyphOutline = NULL;
  static thread_local int s_lastTmHeight_GlyphOutline = 0;

  HFONT currFont = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
  if (hdc != s_lastHdc_GlyphOutline || currFont != s_lastBgFont_GlyphOutline ||
      s_lastHdc_GlyphOutline == NULL) {
    TEXTMETRICA tm;
    if (Real_GetTextMetricsA(hdc, &tm)) {
      s_lastTmHeight_GlyphOutline = tm.tmHeight;
    } else {
      s_lastTmHeight_GlyphOutline = 0; // Fallback
    }
    s_lastHdc_GlyphOutline = hdc;
    s_lastBgFont_GlyphOutline = currFont;
  }

  bool isName = false;
  HFONT f = NULL;

  if (s_lastTmHeight_GlyphOutline > 0) {
    if (g_inBacklogRender) {
      if (!g_fontManager.GetDisableBacklogFont()) {
        int fontSize = ScaleBacklogFontSize(s_lastTmHeight_GlyphOutline);
        isName = (s_lastTmHeight_GlyphOutline <= 18);

        if (isName) {
          f = g_fontManager.GetBacklogNameFont(fontSize);
        } else {
          f = g_fontManager.GetBacklogFont(fontSize);
        }
      }
    } else if (tuneLatinGlyph) {
      // Force dialogue font override for translated Latin paths so
      // DialogueSize is always reflected in-game.
      if (g_fontManager.GetLanguage() != 0) {
        f = g_fontManager.GetDialogueOutlineFontExact(
            -g_fontManager.GetDialogueGlyphHeightHint());
      }
      if (!f && s_lastTmHeight_GlyphOutline > 0) {
        f = g_fontManager.GetEngineScaledFont(s_lastTmHeight_GlyphOutline);
      }
    }
  }

  if (f) {
    SelectObject(hdc, f); // Permanently mutate HDC for this loop
    r = Real_GetGlyphOutlineA(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer,
                              lpmat2);
  }

  if (r == GDI_ERROR) {
    r = Real_GetGlyphOutlineA(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer,
                              lpmat2);
  }

  // Do not normalize dialogue glyph metrics here. ADV32 expects its native
  // full-width glyph metrics; changing them caused clipped/wavy Latin text.

  if (r != GDI_ERROR && isKatakanaDot) {
    if (pvBuffer != NULL && r > 0) {
      memset(pvBuffer, 0, r);
    }
    // We let lpgm->gmCellIncX retain its original Katakana Dot width naturally
    // here.
  }

  if (false && r != GDI_ERROR && lpgm && !g_inBacklogRender) {
    // Common SJIS separators that cause exaggerated spacing in ADV32 exports.
    if (originalChar == 0x8140 || originalChar == 0x8145 ||
        originalChar == 0x819A) {
      if (pvBuffer != NULL && r > 0) {
        memset(pvBuffer, 0, r);
      }
      lpgm->gmCellIncX = (originalChar == 0x8140) ? 1 : 0;
      lpgm->gmBlackBoxX = 0;
    }
  }

  if (r != GDI_ERROR && lpgm && g_inBacklogRender &&
      !g_fontManager.GetDisableBacklogSpacing()) {
    if (!isName) {
      lpgm->gmptGlyphOrigin.x += g_fontManager.GetBacklogXOffset();
      lpgm->gmptGlyphOrigin.y += g_fontManager.GetBacklogYOffset();

      lpgm->gmCellIncX += g_fontManager.GetBacklogDialogSpacing();
    } else {
      lpgm->gmptGlyphOrigin.x += g_fontManager.GetBacklogNameXOffset();
    }
  }

  // Hook for main dialogue: force proportional advancing width for English
  // characters
  if (r != GDI_ERROR && lpgm && !g_inBacklogRender) {
    // Force advance width for dialogue regardless of char code. This keeps
    // spacing consistent even when engine sends mixed full/half-width codes.
    UINT advChar = uChar;
    bool isLatinChar = false;
    if (advChar <= 0x7E) {
      isLatinChar = true;
    }
    if (advChar > 0xFF) {
      UINT converted = ConvertFullWidthToHalfWidth(advChar);
      if (converted <= 0xFF) {
        advChar = converted;
        isLatinChar = true;
      }
    }
    // IMPORTANT: only tune Latin/full-width-Latin. Do not touch Japanese
    // glyph advances, otherwise in-game JP prompts become garbled.
    if (isLatinChar) {
      if (!g_hsxf4ReplayRender)
        EmboldenHsxf4LatinGlyphBitmap(fuFormat, lpgm, r, cjBuffer, pvBuffer,
                                      advChar);
      if (advChar == '.' || advChar == '-')
        lpgm->gmptGlyphOrigin.y -= 4;
      // HSXF4 now controls dialogue placement. Keep the glyph cell wide enough
      // so the engine does not clip the Latin bitmap/outline into tiny shards.
      int minCell = (int)lpgm->gmBlackBoxX + 2;
      if (advChar == ' ') {
        if (minCell < 4)
          minCell = 4;
      } else if (advChar == '.' || advChar == ',' || advChar == '!' ||
                 advChar == '?' || advChar == ':' || advChar == ';' ||
                 advChar == '\'' || advChar == '"') {
        if (minCell < 5)
          minCell = 5;
      } else {
        if (minCell < 8)
          minCell = 8;
      }
      if (lpgm->gmCellIncX < minCell)
        lpgm->gmCellIncX = minCell;
      // Reduce horizontal offset and excessive ascent to avoid stretched gaps
      // and top clipping on ADV32 textbox.
      if (lpgm->gmptGlyphOrigin.x > 1)
        lpgm->gmptGlyphOrigin.x = 1;
      if (lpgm->gmptGlyphOrigin.x < -1)
        lpgm->gmptGlyphOrigin.x = -1;
    }
  }

  // Telemetry: Log the character and its advance width
  if (r != GDI_ERROR && lpgm) {
    char chStr[2] = {(char)(uChar & 0xFF), 0};
    if (uChar > 0xFF)
      chStr[0] = '?'; // Multibyte approx for simple testing

    // Only log printable ascii to avoid huge logs
    if (uChar >= 0x20 && uChar <= 0x7E) {
      SIZE dummySz = {0};
      dummySz.cx = lpgm->gmCellIncX;
      dummySz.cy = lpgm->gmCellIncY;
      LogTextTelemetry("GlyphOutline", chStr, 1, NULL, &dummySz);
      if (!g_inBacklogRender) {
        bool forceGlyphTrace =
            (uChar == 't' || uChar == 'l' || uChar == 'j' || uChar == 'i' ||
             uChar == 'g' || uChar == 'y' || uChar == 'p' || uChar == 'f');
        LogGlyphPathTelemetry("GetGlyphOutlineA", hdc, uChar, lpgm,
                              forceGlyphTrace);
      }
    }
  }

  return r;
}

static DWORD WINAPI Hook_GetGlyphOutlineW(HDC hdc, UINT uChar, UINT fuFormat,
                                          LPGLYPHMETRICS lpgm, DWORD cjBuffer,
                                          LPVOID pvBuffer, const MAT2 *lpmat2) {
  if (InHsxf4LatinRender()) {
    if (IsNonRenderableTextControlW(uChar))
      return ReturnEmptyChoiceGlyph(lpgm, cjBuffer, pvBuffer);
    UINT glyphChar = NormalizeHsxf4GlyphChar(uChar);
    if (g_hsxf4GlyphOverride != 0)
      glyphChar = g_hsxf4GlyphOverride;
    if (g_hsxf4TriplePauseGlyph)
      glyphChar = '.';
    else if (g_hsxf4EllipsisDotRender && glyphChar == '.')
      glyphChar = kHsxf4LatinLowLine;
    HFONT oldFont = SelectHsxf4LatinFont(hdc, glyphChar);
    MAT2 symbolMat = {};
    const MAT2 *glyphMat = lpmat2;
    if (IsHsxf4LatinCompactSymbol((int)glyphChar)) {
      symbolMat = MakeDialogueGlyphMat2(lpmat2, 72);
      glyphMat = &symbolMat;
    }
    DWORD result = Real_GetGlyphOutlineW(hdc, glyphChar, fuFormat, lpgm, cjBuffer,
                                         pvBuffer, glyphMat);
    if (oldFont)
      SelectObject(hdc, oldFont);
    if (result != GDI_ERROR && lpgm) {
      TuneHsxf4LatinGlyphMetrics(glyphChar, lpgm);
      ShiftLongNameplateGlyph(lpgm);
      if (g_hsxf4TriplePauseGlyph && glyphChar == '.')
        result = BuildHsxf4TripleDotGlyphBitmap(fuFormat, lpgm, result,
                                                cjBuffer, pvBuffer);
      if (!g_hsxf4ChoiceRender && !g_hsxf4PopupRender)
        EmboldenHsxf4LatinGlyphBitmap(fuFormat, lpgm, result, cjBuffer,
                                      pvBuffer, glyphChar);
    }
    if (result != GDI_ERROR && lpgm && glyphChar >= 0x20 && glyphChar <= 0x7E) {
      LogGlyphPathTelemetry("GetGlyphOutlineW_HSXF4", hdc, glyphChar, lpgm, true);
    }
    return result;
  }
  if (InPatchSettingsUi() || InSystemMenuLoop() || IsMenuWindowDrawContext(hdc) ||
      IsVersionInfoDialogDrawContext(hdc)) {
    return Real_GetGlyphOutlineW(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer,
                                 lpmat2);
  }
  if (InPopupUiLatinRender()) {
    UINT glyphChar = uChar;
    if (glyphChar >= 0xFF01 && glyphChar <= 0xFF5E)
      glyphChar -= 0xFEE0;
    else if (glyphChar == 0x3000)
      glyphChar = ' ';
    HFONT oldFont = SelectPopupUiTextFont(hdc);
    DWORD result = Real_GetGlyphOutlineW(hdc, glyphChar, fuFormat, lpgm,
                                         cjBuffer, pvBuffer, lpmat2);
    if (oldFont)
      SelectObject(hdc, oldFont);
    if (result != GDI_ERROR && lpgm) {
      TunePopupUiGlyphMetrics(glyphChar, lpgm);
      EmboldenHsxf4LatinGlyphBitmap(fuFormat, lpgm, result, cjBuffer,
                                    pvBuffer, glyphChar);
    }
    return result;
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      IsReplayRawGlyphContext(hdc)) {
    TrackReplayGlyphW(hdc, uChar);
    UINT glyphChar = uChar;
    if (glyphChar >= 0xFF01 && glyphChar <= 0xFF5E)
      glyphChar -= 0xFEE0;
    else if (glyphChar == 0x3000)
      glyphChar = ' ';
    if (glyphChar >= 0x20 && glyphChar <= 0x7E) {
      HFONT oldFont = SelectReplayTextFont(hdc, -8);
      DWORD result = Real_GetGlyphOutlineW(hdc, glyphChar, fuFormat, lpgm,
                                           cjBuffer, pvBuffer, lpmat2);
      if (oldFont)
        SelectObject(hdc, oldFont);
      if (result != GDI_ERROR && lpgm)
        TuneReplayRawGlyphMetrics(glyphChar, lpgm);
      return result;
    }
  }
  if (!g_inBacklogRender && g_fontManager.GetLanguage() != 0 &&
      uChar == L'~' && IsChoiceLargeFontContext(hdc)) {
    return RenderChoiceSpacerGlyphW(hdc, uChar, fuFormat, lpgm, cjBuffer,
                                    pvBuffer, lpmat2);
  }
  if (false && !g_inBacklogRender && g_fontManager.GetLanguage() != 0) {
    if (IsChoiceMarkerGlyph(uChar)) {
      StartChoiceGlyphRun(hdc);
      HFONT oldFont = SelectChoiceTextFont(hdc);
      DWORD result = Real_GetGlyphOutlineW(hdc, uChar, fuFormat, lpgm,
                                           cjBuffer, pvBuffer, lpmat2);
      if (oldFont)
        SelectObject(hdc, oldFont);
      if (result != GDI_ERROR && lpgm) {
        TuneChoiceGlyphMetrics(uChar, lpgm);
        EmboldenHsxf4LatinGlyphBitmap(fuFormat, lpgm, result, cjBuffer,
                                      pvBuffer, uChar);
      }
      return result;
    }

    if (InChoiceGlyphRun(hdc)) {
      ConsumeChoiceGlyphRun();
      if (IsChoiceSpacerGlyph(uChar)) {
        return RenderChoiceSpacerGlyphW(hdc, uChar, fuFormat, lpgm, cjBuffer,
                                        pvBuffer, lpmat2);
      }
      if ((uChar >= 0x20 && uChar <= 0x7E) || IsChoiceMarkerGlyph(uChar)) {
        HFONT oldFont = SelectChoiceTextFont(hdc);
        DWORD result = Real_GetGlyphOutlineW(hdc, uChar, fuFormat, lpgm,
                                             cjBuffer, pvBuffer, lpmat2);
        if (oldFont)
          SelectObject(hdc, oldFont);
        if (result != GDI_ERROR && lpgm) {
          TuneChoiceGlyphMetrics(uChar, lpgm);
          EmboldenHsxf4LatinGlyphBitmap(fuFormat, lpgm, result, cjBuffer,
                                        pvBuffer, uChar);
        }
        return result;
      }
    }
  }
  if (!g_inBacklogRender && uChar == 0x3000) {
    DWORD result = Real_GetGlyphOutlineW(hdc, uChar, fuFormat, lpgm, cjBuffer,
                                         pvBuffer, lpmat2);
    if (result != GDI_ERROR && lpgm) {
      if (pvBuffer && result > 0)
        memset(pvBuffer, 0, result);
      TuneWideSpaceGlyphMetrics(lpgm);
    }
    return result;
  }
  if (!g_inBacklogRender && IsChoiceGlyphW(uChar) &&
      IsLikelyChoiceGlyphContext(hdc)) {
    HFONT oldFont = SelectChoiceTextFont(hdc);
    DWORD result = Real_GetGlyphOutlineW(hdc, uChar, fuFormat, lpgm, cjBuffer,
                                         pvBuffer, lpmat2);
    if (oldFont)
      SelectObject(hdc, oldFont);
    if (result != GDI_ERROR && lpgm) {
      TuneChoiceGlyphMetrics(uChar, lpgm);
      LogGlyphPathTelemetry("GetGlyphOutlineW_CHOICE", hdc, uChar, lpgm, true);
    }
    return result;
  }
  if (!g_inBacklogRender) {
    return Real_GetGlyphOutlineW(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer,
                                 lpmat2);
  }
  if (!g_inBacklogRender && g_dialogueSafePassthrough &&
      g_fontManager.GetLanguage() == 0 && !IsLikelyDialogueMetricsContext(hdc)) {
    return Real_GetGlyphOutlineW(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer,
                                 lpmat2);
  }

  UINT originalChar = uChar;
  if (!g_inBacklogRender) {
    // Keep Unicode full-width ASCII on the native visual path. The stable
    // layout path only tightens HSXF4 pitch.
    if (uChar >= 0xFF01 && uChar <= 0xFF5E) {
      // no-op
    } else if (uChar == 0x3000) {
      // no-op
    }
  }

  bool isKatakanaDot = (uChar == '_');
  bool tuneLatinGlyph = false;

  DWORD r = GDI_ERROR;

  static thread_local HDC s_lastHdc_GlyphOutlineW = NULL;
  static thread_local HFONT s_lastBgFont_GlyphOutlineW = NULL;
  static thread_local int s_lastTmHeight_GlyphOutlineW = 0;

  HFONT currFont = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
  if (hdc != s_lastHdc_GlyphOutlineW || currFont != s_lastBgFont_GlyphOutlineW ||
      s_lastHdc_GlyphOutlineW == NULL) {
    TEXTMETRICA tm;
    if (Real_GetTextMetricsA(hdc, &tm)) {
      s_lastTmHeight_GlyphOutlineW = tm.tmHeight;
    } else {
      s_lastTmHeight_GlyphOutlineW = 0;
    }
    s_lastHdc_GlyphOutlineW = hdc;
    s_lastBgFont_GlyphOutlineW = currFont;
  }

  bool isName = false;
  HFONT f = NULL;
  if (s_lastTmHeight_GlyphOutlineW > 0) {
    if (g_inBacklogRender) {
      if (!g_fontManager.GetDisableBacklogFont()) {
        int fontSize = ScaleBacklogFontSize(s_lastTmHeight_GlyphOutlineW);
        isName = (s_lastTmHeight_GlyphOutlineW <= 18);
        if (isName) {
          f = g_fontManager.GetBacklogNameFont(fontSize);
        } else {
          f = g_fontManager.GetBacklogFont(fontSize);
        }
      }
    } else if (tuneLatinGlyph) {
      if (g_fontManager.GetLanguage() != 0) {
        f = g_fontManager.GetDialogueOutlineFontExact(
            -g_fontManager.GetDialogueGlyphHeightHint());
      }
      if (!f && s_lastTmHeight_GlyphOutlineW > 0) {
        f = g_fontManager.GetEngineScaledFont(s_lastTmHeight_GlyphOutlineW);
      }
    }
  }

  if (f) {
    SelectObject(hdc, f);
    r = Real_GetGlyphOutlineW(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer,
                              lpmat2);
  }
  if (r == GDI_ERROR) {
    r = Real_GetGlyphOutlineW(hdc, uChar, fuFormat, lpgm, cjBuffer, pvBuffer,
                              lpmat2);
  }

  // Do not normalize dialogue glyph metrics here. ADV32 expects its native
  // full-width glyph metrics; changing them caused clipped/wavy Latin text.

  if (r != GDI_ERROR && isKatakanaDot) {
    if (pvBuffer != NULL && r > 0) {
      memset(pvBuffer, 0, r);
    }
  }

  if (false && r != GDI_ERROR && lpgm && !g_inBacklogRender) {
    if (originalChar == 0x3000 || originalChar == 0x30FB ||
        originalChar == 0xFF65) {
      if (pvBuffer != NULL && r > 0) {
        memset(pvBuffer, 0, r);
      }
      lpgm->gmCellIncX = (originalChar == 0x3000) ? 1 : 0;
      lpgm->gmBlackBoxX = 0;
    }
  }

  if (r != GDI_ERROR && lpgm && g_inBacklogRender &&
      !g_fontManager.GetDisableBacklogSpacing()) {
    if (!isName) {
      lpgm->gmptGlyphOrigin.x += g_fontManager.GetBacklogXOffset();
      lpgm->gmptGlyphOrigin.y += g_fontManager.GetBacklogYOffset();
      lpgm->gmCellIncX += g_fontManager.GetBacklogDialogSpacing();
    } else {
      lpgm->gmptGlyphOrigin.x += g_fontManager.GetBacklogNameXOffset();
    }
  }

  if (r != GDI_ERROR && lpgm && !g_inBacklogRender) {
    UINT advChar = uChar;
    bool isLatinChar = false;
    if (advChar <= 0x7E) {
      isLatinChar = true;
    }
    if (advChar >= 0xFF01 && advChar <= 0xFF5E) {
      advChar = advChar - 0xFEE0;
      isLatinChar = true;
    } else if (advChar == 0x3000) {
      advChar = (UINT)' ';
      isLatinChar = true;
    }
    if (isLatinChar) {
      int minCell = (int)lpgm->gmBlackBoxX + 2;
      if (advChar == ' ') {
        if (minCell < 4)
          minCell = 4;
      } else if (advChar == '.' || advChar == ',' || advChar == '!' ||
                 advChar == '?' || advChar == ':' || advChar == ';' ||
                 advChar == '\'' || advChar == '"') {
        if (minCell < 5)
          minCell = 5;
      } else {
        if (minCell < 8)
          minCell = 8;
      }
      if (lpgm->gmCellIncX < minCell)
        lpgm->gmCellIncX = minCell;
      if (lpgm->gmptGlyphOrigin.x > 1)
        lpgm->gmptGlyphOrigin.x = 1;
      if (lpgm->gmptGlyphOrigin.x < -1)
        lpgm->gmptGlyphOrigin.x = -1;
    }
  }

  if (r != GDI_ERROR && lpgm && !g_inBacklogRender) {
    if (uChar >= 0x20 && uChar <= 0x7E) {
      char chStr[2] = {(char)(uChar & 0xFF), 0};
      SIZE dummySz = {0};
      dummySz.cx = lpgm->gmCellIncX;
      dummySz.cy = lpgm->gmCellIncY;
      LogTextTelemetry("GlyphOutlineW", chStr, 1, NULL, &dummySz);
      bool forceGlyphTrace =
          (uChar == 't' || uChar == 'l' || uChar == 'j' || uChar == 'i' ||
           uChar == 'g' || uChar == 'y' || uChar == 'p' || uChar == 'f');
      LogGlyphPathTelemetry("GetGlyphOutlineW", hdc, uChar, lpgm,
                            forceGlyphTrace);
    }
  }

  return r;
}

// System confirmations enter EXE+24240 as a complete string. EXE+22EA0
// then draws one temporary glyph at a time through EXE+11710. Keep the
// native cursor/background work, suppress those glyphs during a matched
// label, and draw proportional text through the native alpha blitter.
static void(__cdecl *Real_SystemText_11710)(int, int, const char *, int, int) = NULL;
static void(__cdecl *DrawSystemGlyph_11220)(int, int, const BYTE *, int) = NULL;

static void(__cdecl *Real_SystemLabel_24240)(int, int, int, const char *, int) = NULL;
static thread_local bool g_suppressSystemPopupGlyphs = false;

// EXE+1C2C0 draws text layers belonging to UI button records. Only an owned
// story button whose expanded label starts with the choice marker is eligible.
// The scope is absent from dialogue, save/replay lists and popup labels.
static char(__cdecl *Real_StoryChoiceLayer_1C2C0)(const int *, const char *) = NULL;
static void(__cdecl *SetStoryChoiceClip_DE00)(int, int, int, int) = NULL;
static BYTE *g_storyChoiceExe = nullptr;
static thread_local const RECT *g_storyChoicePanel = nullptr;
static thread_local bool g_suppressStoryChoiceGlyphs = false;

static bool GetStoryChoicePanel(const int *layer, RECT &panel) {
  if (!g_storyChoiceExe || g_fontManager.GetLanguage() == 0)
    return false;
  if (*(const int *)(g_storyChoiceExe + 0x6D43C4) != 9)
    return false;
  const BYTE *buttons = *(const BYTE **)(g_storyChoiceExe + 0x105494);
  int count = *(const int *)(g_storyChoiceExe + 0x65CA94);
  constexpr size_t buttonSize = 880, firstLayer = 52, layerSize = 44;
  uintptr_t start = (uintptr_t)buttons, address = (uintptr_t)layer;
  if (!buttons || count <= 0 || count > 256 || address < start)
    return false;
  size_t offset = address - start;
  size_t within = offset % buttonSize;
  if (offset / buttonSize >= (size_t)count || within < firstLayer ||
      (within - firstLayer) % layerSize ||
      (within - firstLayer) / layerSize >= 16 ||
      !IsReadablePtr(buttons + offset - within, buttonSize))
    return false;
  const int *button = (const int *)(buttons + offset - within);
  panel = {button[3], button[4], button[5], button[6]};
  return panel.left >= 0 && panel.top >= 0 && panel.right <= 800 &&
         panel.bottom <= 600 && panel.right - panel.left >= 64 &&
         panel.bottom - panel.top >= 24 && panel.bottom - panel.top <= 64 &&
         layer[0] >= panel.left && layer[0] < panel.right &&
         layer[1] >= panel.top && layer[1] < panel.bottom;
}

static char __cdecl Hook_StoryChoiceLayer_1C2C0(const int *layer,
                                               const char *text) {
  RECT panel = {};
  const RECT *previous = g_storyChoicePanel;
  g_storyChoicePanel = GetStoryChoicePanel(layer, panel) ? &panel : nullptr;
  // Preserve the native variable expansion, background and cursor handling.
  char result = Real_StoryChoiceLayer_1C2C0(layer, text);
  g_storyChoicePanel = previous;
  return result;
}

static bool ReadStoryChoiceLabel(const char *text, std::wstring &label) {
  if (!text)
    return false;
  for (int length = 0; length < 256; ++length) {
    if (!IsReadablePtr(text + length, 1))
      return false;
    if (!text[length]) {
      if (!length)
        return false;
      wchar_t wide[256];
      int chars = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, text, length,
                                      wide, 256);
      // Check decoded characters: 0x40 is also the trail byte of a CP932
      // ideographic space, so a byte-level '@' check would reject every gap.
      std::wstring decoded(wide, chars > 0 ? chars : 0);
      return chars > 0 && decoded.find(L'@') == std::wstring::npos &&
             choice_layout::Normalize(decoded, label);
    }
  }
  return false;
}

static void DrawStoryChoiceBitmap(const choice_layout::Bitmap &bitmap,
                                   const RECT &panel, int color, int offset) {
  int drawX = panel.left + 12 + offset;
  int drawY = panel.top + (panel.bottom - panel.top - bitmap.height) / 2 + offset;
  for (int start = 0; start < bitmap.width; start += 28) {
    BYTE tile[28 * 28] = {};
    int width = min(28, bitmap.width - start);
    for (int row = 0; row < bitmap.height; ++row)
      memcpy(tile + row * 28,
             bitmap.pixels.data() + row * bitmap.width + start, width);
    DrawSystemGlyph_11220(drawX + start, drawY, tile, color);
  }
}

static bool RenderStoryChoiceLabel(int x, int y, int lineHeight,
                                    const char *text, int style) {
  if (!g_storyChoicePanel || !DrawSystemGlyph_11220 || !SetStoryChoiceClip_DE00)
    return false;
  std::wstring label;
  if (!ReadStoryChoiceLabel(text, label))
    return false;
  const RECT panel = *g_storyChoicePanel;
  // Reserve the final pixel for the native shadow.
  int maxWidth = panel.right - panel.left - 25;
  int maxHeight = min(28, panel.bottom - panel.top - 9);
  std::wstring key = std::to_wstring(maxWidth) + L":" +
                     std::to_wstring(maxHeight) + L":" + label;
  static thread_local std::map<std::wstring, choice_layout::Bitmap> cache;
  auto found = cache.find(key);
  if (found == cache.end()) {
    choice_layout::Bitmap built;
    if (!choice_layout::Build(label, maxWidth, maxHeight,
                              Real_GetTextExtentPoint32W,
                              Real_GetGlyphOutlineW, built))
      return false;
    if (cache.size() >= 128)
      cache.clear();
    found = cache.emplace(key, std::move(built)).first;
    char message[256];
    sprintf_s(message,
              "[CHOICE-LAYOUT] chars=%u panel=(%ld,%ld,%ld,%ld) bitmap=%dx%d margin=12",
              (unsigned)label.size(), panel.left, panel.top, panel.right,
              panel.bottom, found->second.width, found->second.height);
    LogHsxf4Probe(message);
  }
  bool previous = g_suppressStoryChoiceGlyphs;
  g_suppressStoryChoiceGlyphs = true;
  Real_SystemLabel_24240(x, y, lineHeight, text, style);
  g_suppressStoryChoiceGlyphs = previous;
  // EXE+DE00 intersects with the engine's outer clip. Restore the exact
  // per-layer clip afterward so later UI and textbox drawing are unaffected.
  int *clip = (int *)(g_storyChoiceExe + 0x8C8678);
  int savedClip[4];
  memcpy(savedClip, clip, sizeof(savedClip));
  SetStoryChoiceClip_DE00(panel.left + 12, panel.top + 4,
                         panel.right - 12, panel.bottom - 4);
  if (style & 0x80)
    DrawStoryChoiceBitmap(found->second, panel, (style & 0x3f) ? 0 : 9, 1);
  DrawStoryChoiceBitmap(found->second, panel, style & 0x3f, 0);
  memcpy(clip, savedClip, sizeof(savedClip));
  return true;
}

struct PopupLabelBitmap {
  int width = 0;
  int height = 0;
  std::vector<BYTE> pixels;
};

static bool ReadSystemPopupLabel(const char *text, std::string &label) {
  if (!text)
    return false;
  label.clear();
  for (size_t i = 0; i < 96;) {
    const BYTE *p = (const BYTE *)text + i;
    if (!IsReadablePtr(p, 1))
      return false;
    if (!*p) {
      std::string key = label;
      for (char &ch : key)
        ch = (char)tolower((unsigned char)ch);
      return key == "simpan permainan?" || key == "muat data?" ||
             key == "reset config?" || key == "mau kembali?" ||
             key == "ingin keluar?" || key == "mau keluar?" ||
             key == "muat permainan?" || key == "simpan disini?" ||
             key == "simpan di sini?";
    }
    if (*p >= 0x20 && *p <= 0x7e) {
      label.push_back((char)*p);
      ++i;
    } else if (IsReadablePtr(p, 2) &&
               ((*p >= 0x81 && *p <= 0x9f) || (*p >= 0xe0 && *p <= 0xfc))) {
      int ch = DecodeHsxf4AsciiByte(p[0], p[1]);
      if (ch < 0x20 || ch > 0x7e)
        return false;
      label.push_back((char)ch);
      i += 2;
    } else {
      return false;
    }
  }
  return false;
}

static bool BuildSystemPopupBitmap(const std::string &label,
                                   PopupLabelBitmap &bitmap) {
  HDC dc = CreateCompatibleDC(NULL);
  if (!dc)
    return false;
  std::wstring wide(label.begin(), label.end());
  bool success = false;
  // The panel's inner width is 228 px. Keep at least 6 px on either side.
  for (int size = 28; size >= 18 && !success; --size) {
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH, L"Arial");
    if (!font)
      break;
    HGDIOBJ previous = SelectObject(dc, font);
    SIZE extent = {};
    bool fits = Real_GetTextExtentPoint32W(dc, wide.c_str(),
                                          (int)wide.size(), &extent) &&
                extent.cx <= 216;
    constexpr int kCanvasWidth = 256;
    constexpr int kCanvasHeight = 64;
    std::vector<BYTE> canvas(kCanvasWidth * kCanvasHeight, 0);
    MAT2 matrix = {};
    matrix.eM11.value = matrix.eM22.value = 1;
    for (size_t i = 0; fits && i < wide.size(); ++i) {
      SIZE prefix = {};
      if (i && !Real_GetTextExtentPoint32W(dc, wide.c_str(), (int)i, &prefix)) {
        fits = false;
        break;
      }
      GLYPHMETRICS gm = {};
      DWORD bytes = Real_GetGlyphOutlineW(dc, wide[i], GGO_GRAY8_BITMAP, &gm,
                                          0, NULL, &matrix);
      if (bytes == GDI_ERROR || bytes > 65536) {
        fits = false;
        break;
      }
      if (!bytes)
        continue;
      std::vector<BYTE> glyph(bytes);
      if (Real_GetGlyphOutlineW(dc, wide[i], GGO_GRAY8_BITMAP, &gm,
                                bytes, glyph.data(), &matrix) == GDI_ERROR) {
        fits = false;
        break;
      }
      DWORD stride = (gm.gmBlackBoxX + 3) & ~3u;
      if ((ULONGLONG)stride * gm.gmBlackBoxY > bytes) {
        fits = false;
        break;
      }
      for (DWORD gy = 0; gy < gm.gmBlackBoxY; ++gy) {
        for (DWORD gx = 0; gx < gm.gmBlackBoxX; ++gx) {
          int x = 4 + prefix.cx + gm.gmptGlyphOrigin.x + (int)gx;
          int y = 40 - gm.gmptGlyphOrigin.y + (int)gy;
          if (x < 0 || x >= kCanvasWidth || y < 0 || y >= kCanvasHeight) {
            fits = false;
            continue;
          }
          BYTE alpha = (BYTE)((glyph[gy * stride + gx] + 8) / 16);
          BYTE &pixel = canvas[y * kCanvasWidth + x];
          if (alpha > pixel)
            pixel = alpha;
        }
      }
    }
    SelectObject(dc, previous);
    DeleteObject(font);
    if (!fits)
      continue;
    int left = kCanvasWidth, top = kCanvasHeight, right = -1, bottom = -1;
    for (int y = 0; y < kCanvasHeight; ++y) {
      for (int x = 0; x < kCanvasWidth; ++x) {
        if (canvas[y * kCanvasWidth + x]) {
          left = min(left, x);
          right = max(right, x);
          top = min(top, y);
          bottom = max(bottom, y);
        }
      }
    }
    int width = right - left + 1, height = bottom - top + 1;
    if (width <= 0 || width > 216 || height <= 0 || height > 28)
      continue;
    bitmap.width = width;
    bitmap.height = height;
    bitmap.pixels.resize(width * height);
    for (int y = 0; y < height; ++y)
      memcpy(bitmap.pixels.data() + y * width,
             canvas.data() + (top + y) * kCanvasWidth + left, width);
    success = true;
  }
  DeleteDC(dc);
  return success;
}

static void __cdecl Hook_SystemText_11710(int x, int y, const char *text,
                                         int color, int size) {
  if (!g_suppressSystemPopupGlyphs && !g_suppressStoryChoiceGlyphs)
    Real_SystemText_11710(x, y, text, color, size);
}

static void DrawSystemPopupBitmap(const PopupLabelBitmap &bitmap, int color,
                                  int offset) {
  const int drawX = (800 - bitmap.width) / 2 + offset;
  const int drawY = 278 - bitmap.height / 2 + offset;
  for (int start = 0; start < bitmap.width; start += 28) {
    BYTE tile[28 * 28] = {};
    int width = min(28, bitmap.width - start);
    for (int row = 0; row < bitmap.height; ++row)
      memcpy(tile + row * 28,
             bitmap.pixels.data() + row * bitmap.width + start, width);
    DrawSystemGlyph_11220(drawX + start, drawY, tile, color);
  }
}

static void __cdecl Hook_SystemLabel_24240(int x, int y, int lineHeight,
                                          const char *text, int style) {
  if (RenderStoryChoiceLabel(x, y, lineHeight, text, style))
    return;
  std::string label;
  const PopupLabelBitmap *bitmap = nullptr;
  // Match the entire known confirmation and its panel position. Other UI
  // strings, dialogue and Japanese mode must retain the stock renderer.
  if (g_fontManager.GetLanguage() != 0 && DrawSystemGlyph_11220 &&
      x >= 250 && x <= 520 && y >= 230 && y <= 310 &&
      ReadSystemPopupLabel(text, label)) {
    static thread_local std::map<std::string, PopupLabelBitmap> cache;
    auto found = cache.find(label);
    if (found == cache.end()) {
      PopupLabelBitmap built;
      if (BuildSystemPopupBitmap(label, built)) {
        found = cache.emplace(label, std::move(built)).first;
        char message[256];
        sprintf_s(message, "[POPUP-LAYOUT] text='%s' original=(%d,%d) bitmap=%dx%d",
                  label.c_str(), x, y, found->second.width, found->second.height);
        LogHsxf4Probe(message);
      }
    }
    if (found != cache.end())
      bitmap = &found->second;
  }
  bool previous = g_suppressSystemPopupGlyphs;
  g_suppressSystemPopupGlyphs = previous || bitmap != nullptr;
  Real_SystemLabel_24240(x, y, lineHeight, text, style);
  g_suppressSystemPopupGlyphs = previous;
  if (bitmap && !previous) {
    if (style & 0x80)
      DrawSystemPopupBitmap(*bitmap, (style & 0x3f) ? 0 : 9, 1);
    DrawSystemPopupBitmap(*bitmap, style & 0x3f, 0);
  }
}

static HANDLE WINAPI Hook_CreateFileA(
    LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile) {

  static thread_local bool s_inCreateFile = false;
  if (s_inCreateFile) {
    return Real_CreateFileA(lpFileName, dwDesiredAccess, dwShareMode,
                            lpSecurityAttributes, dwCreationDisposition,
                            dwFlagsAndAttributes, hTemplateFile);
  }

  s_inCreateFile = true;

  int lang = g_fontManager.GetLanguage();
  if (lang == 1 || lang == 2) {
    std::string newPath = ReplacePathA(lpFileName);
    if (!newPath.empty()) {
      // Resolve to absolute path to avoid CWD-dependent resolution issues
      char absPath[MAX_PATH];
      DWORD absLen = GetFullPathNameA(newPath.c_str(), MAX_PATH, absPath, NULL);
      const char *finalPath =
          (absLen > 0 && absLen < MAX_PATH) ? absPath : newPath.c_str();

      HANDLE h = Real_CreateFileA(finalPath, dwDesiredAccess, dwShareMode,
                                  lpSecurityAttributes, dwCreationDisposition,
                                  dwFlagsAndAttributes, hTemplateFile);
      s_inCreateFile = false;
      return h;
    }
  }

  HANDLE h = Real_CreateFileA(lpFileName, dwDesiredAccess, dwShareMode,
                              lpSecurityAttributes, dwCreationDisposition,
                              dwFlagsAndAttributes, hTemplateFile);
  s_inCreateFile = false;
  return h;
}

static int WINAPI Hook_MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption,
                                   UINT uType) {
  std::string textUtf8 = DiagSjisToUtf8(lpText);
  std::string captionUtf8 = DiagSjisToUtf8(lpCaption);
  DiagLog("MessageBoxA type=0x%08X caption='%s' text='%s'", uType,
          captionUtf8.empty() ? "(empty)" : captionUtf8.c_str(),
          textUtf8.empty() ? "(empty)" : textUtf8.c_str());

  if (IsDvdInsertMessageA(lpText)) {
    DiagLog("MessageBoxA DVD check suppressed");
    return IDOK;
  }
  if (lpCaption && DiagContainsNoCaseA(lpCaption, "InstKey")) {
    DiagLog("MessageBoxA serial key check suppressed");
    return IDOK;
  }

  // Translate Japanese text to Indonesian
  const char *translatedText = TranslateUI(lpText);
  const char *translatedCaption = TranslateUI(lpCaption);
  if (!translatedText)
    translatedText = TranslateUIPartial(lpText);
  if (!translatedCaption)
    translatedCaption = TranslateUIPartial(lpCaption);

  return Real_MessageBoxA(hWnd, translatedText ? translatedText : lpText,
                          translatedCaption ? translatedCaption : lpCaption,
                          uType);
}

static int WINAPI Hook_MessageBoxW(HWND hWnd, LPCWSTR lpText, LPCWSTR lpCaption,
                                   UINT uType) {
  std::string textUtf8 = DiagWideToUtf8(lpText);
  std::string captionUtf8 = DiagWideToUtf8(lpCaption);
  DiagLog("MessageBoxW type=0x%08X caption='%s' text='%s'", uType,
          captionUtf8.empty() ? "(empty)" : captionUtf8.c_str(),
          textUtf8.empty() ? "(empty)" : textUtf8.c_str());

  return Real_MessageBoxW(hWnd, lpText, lpCaption, uType);
}

// Menu hooks - translate Japanese menu item text to Indonesian
static BOOL WINAPI Hook_AppendMenuA(HMENU hMenu, UINT uFlags,
                                    UINT_PTR uIDNewItem, LPCSTR lpNewItem) {
  if (lpNewItem && !(uFlags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))) {
    const char *translated = TranslateUI(lpNewItem);
    if (!translated)
      translated = TranslateUIPartial(lpNewItem);
    if (translated)
      return Real_AppendMenuA(hMenu, uFlags, uIDNewItem, translated);
  }
  return Real_AppendMenuA(hMenu, uFlags, uIDNewItem, lpNewItem);
}

static BOOL WINAPI Hook_InsertMenuA(HMENU hMenu, UINT uPosition, UINT uFlags,
                                    UINT_PTR uIDNewItem, LPCSTR lpNewItem) {
  if (lpNewItem && !(uFlags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))) {
    const char *translated = TranslateUI(lpNewItem);
    if (!translated)
      translated = TranslateUIPartial(lpNewItem);
    if (translated)
      return Real_InsertMenuA(hMenu, uPosition, uFlags, uIDNewItem, translated);
  }
  return Real_InsertMenuA(hMenu, uPosition, uFlags, uIDNewItem, lpNewItem);
}

static BOOL WINAPI Hook_ModifyMenuA(HMENU hMenu, UINT uPosition, UINT uFlags,
                                    UINT_PTR uIDNewItem, LPCSTR lpNewItem) {
  if (lpNewItem && !(uFlags & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR))) {
    const char *translated = TranslateUI(lpNewItem);
    if (!translated)
      translated = TranslateUIPartial(lpNewItem);
    if (translated)
      return Real_ModifyMenuA(hMenu, uPosition, uFlags, uIDNewItem, translated);
  }
  return Real_ModifyMenuA(hMenu, uPosition, uFlags, uIDNewItem, lpNewItem);
}

// ============================================================================
// KEY (Serial Key) Verification Bypass - Skip startup dialogs
// ============================================================================
static int g_dialogSkipCount = 0;
static void LocalizeVersionInfoDialog(HWND hDlg);
static thread_local std::vector<DLGPROC> g_versionInfoDlgProcStack;

static INT_PTR CALLBACK Hook_VersionInfoDialogProc(HWND hwndDlg, UINT uMsg,
                                                   WPARAM wParam,
                                                   LPARAM lParam) {
  DLGPROC origProc = nullptr;
  if (!g_versionInfoDlgProcStack.empty()) {
    origProc = g_versionInfoDlgProcStack.back();
  }

  if (uMsg == WM_INITDIALOG) {
    INT_PTR ret = FALSE;
    if (origProc) {
      ret = origProc(hwndDlg, uMsg, wParam, lParam);
    }
    LocalizeVersionInfoDialog(hwndDlg);
    return ret;
  }

  if (uMsg == WM_SHOWWINDOW || uMsg == WM_WINDOWPOSCHANGED) {
    LocalizeVersionInfoDialog(hwndDlg);
  } else if (uMsg == WM_NCDESTROY) {
    RemovePropW(hwndDlg, kVerInfoProp);
  }

  if (origProc) {
    return origProc(hwndDlg, uMsg, wParam, lParam);
  }
  return FALSE;
}

static INT_PTR WINAPI Hook_DialogBoxParamA(HINSTANCE hInstance,
                                           LPCSTR lpTemplateName,
                                           HWND hWndParent,
                                           DLGPROC lpDialogFunc,
                                           LPARAM dwInitParam) {
  if (g_expectVersionInfoDialog && lpDialogFunc) {
    g_expectVersionInfoDialog = false;
    g_versionInfoDlgProcStack.push_back(lpDialogFunc);
    INT_PTR ret = Real_DialogBoxParamA(hInstance, lpTemplateName, hWndParent,
                                       Hook_VersionInfoDialogProc, dwInitParam);
    g_versionInfoDlgProcStack.pop_back();
    return ret;
  }
  if (g_dc2tySerial.loaded && IsSerialKeyDialogTemplate(lpTemplateName)) {
    DiagLog("DialogBoxParamA serial key dialog suppressed (template=%u)",
            (UINT)(UINT_PTR)lpTemplateName);
    return IDOK;
  }
  return Real_DialogBoxParamA(hInstance, lpTemplateName, hWndParent,
                              lpDialogFunc, dwInitParam);
}

static INT_PTR WINAPI Hook_DialogBoxParamW(HINSTANCE hInstance,
                                           LPCWSTR lpTemplateName,
                                           HWND hWndParent,
                                           DLGPROC lpDialogFunc,
                                           LPARAM dwInitParam) {
  if (g_expectVersionInfoDialog && lpDialogFunc) {
    g_expectVersionInfoDialog = false;
    g_versionInfoDlgProcStack.push_back(lpDialogFunc);
    INT_PTR ret = Real_DialogBoxParamW(hInstance, lpTemplateName, hWndParent,
                                       Hook_VersionInfoDialogProc, dwInitParam);
    g_versionInfoDlgProcStack.pop_back();
    return ret;
  }
  if (g_dc2tySerial.loaded && IsSerialKeyDialogTemplate(lpTemplateName)) {
    DiagLog("DialogBoxParamW serial key dialog suppressed (template=%u)",
            (UINT)(UINT_PTR)lpTemplateName);
    return IDOK;
  }
  return Real_DialogBoxParamW(hInstance, lpTemplateName, hWndParent,
                              lpDialogFunc, dwInitParam);
}

// ============================================================================
// Settings Dialog
// ============================================================================

#define IDC_CHECK_BACKLOG_ICONS 1001
#define IDC_CHECK_FILE_REDIRECT 1002
#define IDC_BTN_CHANGE_DIALOGUE_FONT 1003
#define IDC_BTN_CHANGE_BACKLOG_FONT 1006
#define IDC_LBL_SPACING 1007
#define IDC_EDIT_SPACING 1008
#define IDC_LBL_XOFFSET 1009
#define IDC_EDIT_XOFFSET 1010
#define IDC_LBL_YOFFSET 1011
#define IDC_EDIT_YOFFSET 1012
#define IDC_BTN_CHANGE_BACKLOG_NAME_FONT 1013
#define IDC_LBL_NAME_XOFFSET 1020
#define IDC_EDIT_NAME_XOFFSET 1021
#define IDC_LBL_NAME_YOFFSET 1018
#define IDC_EDIT_NAME_YOFFSET 1019
#define IDC_LBL_NAME_SPACING 1014
#define IDC_EDIT_NAME_SPACING 1015
#define IDC_LBL_DIALOG_SPACING 1016
#define IDC_EDIT_DIALOG_SPACING 1017
#define IDC_CHK_ADVANCED 1022
#define IDC_BTN_OK 1004
#define IDC_BTN_CANCEL 1005
#define IDC_BTN_ABOUT 1023
#define IDC_BTN_ADVANCED 1024
#define IDC_COMBO_LANGUAGE 1026

static int NormalizeChooserFontHeight(int height) {
  if (height == 0)
    height = -19;
  if (height > 0)
    height = -height;
  if (height > -8)
    height = -8;
  if (height < -72)
    height = -72;
  return height;
}

static bool ShowPatchFontChooser(HWND owner, const std::wstring &currentName,
                                 int currentHeight, std::wstring &outName,
                                 int &outHeight) {
  LOGFONTW lf = {};
  std::wstring safeName = ResolveSafeFontFace(currentName);
  if (safeName.length() >= LF_FACESIZE)
    safeName = safeName.substr(0, LF_FACESIZE - 1);

  wcsncpy_s(lf.lfFaceName, safeName.c_str(), _TRUNCATE);
  lf.lfHeight = NormalizeChooserFontHeight(currentHeight);
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
  lf.lfQuality = ANTIALIASED_QUALITY;

  CHOOSEFONTW cf = {};
  cf.lStructSize = sizeof(cf);
  cf.hwndOwner = owner;
  cf.lpLogFont = &lf;
  cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS |
             CF_FORCEFONTEXIST | CF_LIMITSIZE;
  cf.nSizeMin = 8;
  cf.nSizeMax = 48;

  if (!ChooseFontW(&cf))
    return false;

  outName = lf.lfFaceName;
  outHeight = lf.lfHeight;
  if (outName.empty())
    outName = safeName;

  if (outHeight == 0) {
    int pt = cf.iPointSize / 10;
    if (pt <= 0)
      pt = 19;
    HWND dcOwner = owner ? owner : nullptr;
    HDC hdc = GetDC(dcOwner);
    int dpiY = hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96;
    if (hdc)
      ReleaseDC(dcOwner, hdc);
    outHeight = -MulDiv(pt, dpiY, 72);
  }

  outHeight = NormalizeChooserFontHeight(outHeight);
  return true;
}

static INT_PTR CALLBACK AdvancedSettingsDialogProc(HWND hwndDlg, UINT uMsg,
                                                   WPARAM wParam,
                                                   LPARAM lParam);
static void ShowAdvancedSettingsDialog(HWND parent);

static INT_PTR CALLBACK SettingsDialogProc(HWND hwndDlg, UINT uMsg,
                                           WPARAM wParam, LPARAM lParam) {
  switch (uMsg) {
  case WM_INITDIALOG: {
    // Center the dialog on the parent window
    RECT rcParent, rcDlg;
    GetWindowRect(g_mainWindow, &rcParent);
    GetWindowRect(hwndDlg, &rcDlg);
    int x = rcParent.left +
            (rcParent.right - rcParent.left - (rcDlg.right - rcDlg.left)) / 2;
    int y = rcParent.top +
            (rcParent.bottom - rcParent.top - (rcDlg.bottom - rcDlg.top)) / 2;
    SetWindowPos(hwndDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    // Populate ComboBox
    HWND hCombo = GetDlgItem(hwndDlg, IDC_COMBO_LANGUAGE);
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"Japanese (Original)");
    SendMessageW(hCombo, CB_ADDSTRING, 0,
                 (LPARAM)L"Indonesian / Bahasa Indonesia (id_Data)");
    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"English (eng_data)");

    // Set selection
    SendMessageW(hCombo, CB_SETCURSEL, g_fontManager.GetLanguage(), 0);

    return TRUE;
  }

  case WM_COMMAND: {
    if (LOWORD(wParam) == IDC_BTN_ADVANCED) {
      ShowAdvancedSettingsDialog(hwndDlg);
      return TRUE;
    }

    switch (LOWORD(wParam)) {
    case IDC_BTN_CHANGE_DIALOGUE_FONT: {
      std::wstring chosenName;
      int chosenHeight = 0;
      if (ShowPatchFontChooser(hwndDlg, g_fontManager.GetDialogueFontName(),
                               g_fontManager.GetDialogueFontSize(), chosenName,
                               chosenHeight)) {
        g_fontManager.SetDialogueFont(chosenName, chosenHeight);
        MessageBoxW(hwndDlg, L"Dialogue font updated!", L"DC2TY Patch",
                    MB_OK | MB_ICONINFORMATION);
      }
      return TRUE;
    }

    case IDC_BTN_CHANGE_BACKLOG_FONT: {
      std::wstring chosenName;
      int chosenHeight = 0;
      if (ShowPatchFontChooser(hwndDlg, g_fontManager.GetBacklogFontName(),
                               g_fontManager.GetBacklogFontSize(), chosenName,
                               chosenHeight)) {
        g_fontManager.SetBacklogFont(chosenName, chosenHeight);
        MessageBoxW(hwndDlg, L"Backlog font updated!", L"DC2TY Patch",
                    MB_OK | MB_ICONINFORMATION);
      }
      return TRUE;
    }

    case IDC_BTN_CHANGE_BACKLOG_NAME_FONT: {
      std::wstring chosenName;
      int chosenHeight = 0;
      if (ShowPatchFontChooser(hwndDlg, g_fontManager.GetBacklogNameFontName(),
                               g_fontManager.GetBacklogNameFontSize(),
                               chosenName, chosenHeight)) {
        g_fontManager.SetBacklogNameFont(chosenName, chosenHeight);
        MessageBoxW(hwndDlg, L"Backlog Character Name font updated!",
                    L"DC2TY Patch", MB_OK | MB_ICONINFORMATION);
      }
      return TRUE;
    }

    case IDC_BTN_OK: {
      // Save language settings
      HWND hCombo = GetDlgItem(hwndDlg, IDC_COMBO_LANGUAGE);
      int selectedLang = (int)SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
      if (selectedLang == CB_ERR)
        selectedLang = 0;

      bool anyChanged = (selectedLang != g_fontManager.GetLanguage());

      if (anyChanged) {
        g_fontManager.SetLanguage(selectedLang);
        MessageBoxW(hwndDlg,
                    L"\u26a0\ufe0f File redirection setting changed.\n\n"
                    L"Return to title screen or restart the game\n"
                    L"for this to take full effect.",
                    L"DC2TY Patch", MB_OK | MB_ICONWARNING);
      }

      EndDialog(hwndDlg, IDOK);
      return TRUE;
    }

    case IDC_BTN_ABOUT:
      MessageBoxW(
          hwndDlg,
          L"This patch has been made by:\nSakura Symphony Re;Translation",
          L"About DC2TY Patch", MB_OK | MB_ICONINFORMATION);
      return TRUE;

    case IDC_BTN_CANCEL:
      EndDialog(hwndDlg, IDCANCEL);
      return TRUE;
    }
    break;
  }

  case WM_CLOSE:
    EndDialog(hwndDlg, IDCANCEL);
    return TRUE;
  }
  return FALSE;
}

// In-memory DIALOG resource construction
static void ShowSettingsDialog() {
  if (!g_mainWindow)
    return;

  // Allocate a buffer for the DLGTEMPLATE and controls
  const int bufSize = 4096;
  BYTE *buffer = new BYTE[bufSize];
  memset(buffer, 0, bufSize);

  WORD *pw = (WORD *)buffer;

  // Dialog Header
  DLGTEMPLATE *pDlg = (DLGTEMPLATE *)pw;
  pDlg->style = WS_POPUP | WS_BORDER | WS_SYSMENU | WS_CAPTION | DS_MODALFRAME |
                DS_CENTER | DS_SETFONT;
  pDlg->dwExtendedStyle = 0;
  pDlg->cdit = 11; // 2 Warning Labels, 2 Checkboxes, 3 Font Buttons, 1 Adv
                   // Button, OK, Cancel, About
  pDlg->x = 0;
  pDlg->y = 0;
  pDlg->cx = 250;
  pDlg->cy = 200;

  pw = (WORD *)(pDlg + 1);
  *pw++ = 0; // Menu
  *pw++ = 0; // Class
  // Title: null-terminated Unicode string
  const WCHAR *dlgTitle = L"DC2TY Patch Settings";
  wcscpy((WCHAR *)pw, dlgTitle);
  pw += wcslen(dlgTitle) + 1;
  // Font (because DS_SETFONT): point size + font name
  *pw++ = 9; // point size
  const WCHAR *fontName = L"Segoe UI";
  wcscpy((WCHAR *)pw, fontName);
  pw += wcslen(fontName) + 1;

  // Helper: align pointer to DWORD boundary (required before each
  // DLGITEMTEMPLATE)
  auto AlignDword = [](WORD *&p) {
    while ((ULONG_PTR)p & 3)
      p++;
  };

  // Control: Warning Label English
  AlignDword(pw);
  DLGITEMTEMPLATE *pItem = (DLGITEMTEMPLATE *)pw;
  pItem->style = WS_CHILD | WS_VISIBLE | SS_LEFT;
  pItem->dwExtendedStyle = 0;
  pItem->x = 10;
  pItem->y = 10;
  pItem->cx = 230;
  pItem->cy = 10;
  pItem->id = -1;
  pw = (WORD *)(pItem + 1);
  *pw++ = 0xFFFF;
  *pw++ = 0x0082; // Static class
  const WCHAR *textWarnEn = L"Warning: Wrong setting could lead to crashing.";
  wcscpy((WCHAR *)pw, textWarnEn);
  pw += wcslen(textWarnEn) + 1;
  *pw++ = 0;

  // Control: Warning Label Indonesian
  AlignDword(pw);
  pItem = (DLGITEMTEMPLATE *)pw;
  pItem->style = WS_CHILD | WS_VISIBLE | SS_LEFT;
  pItem->dwExtendedStyle = 0;
  pItem->x = 10;
  pItem->y = 20;
  pItem->cx = 230;
  pItem->cy = 10;
  pItem->id = -1;
  pw = (WORD *)(pItem + 1);
  *pw++ = 0xFFFF;
  *pw++ = 0x0082; // Static class
  const WCHAR *textWarnId =
      L"Peringatan: Pengaturan yang salah dapat menyebabkan crash.";
  wcscpy((WCHAR *)pw, textWarnId);
  pw += wcslen(textWarnId) + 1;
  *pw++ = 0;

  // Control 1: Language Label
  AlignDword(pw);
  pItem = (DLGITEMTEMPLATE *)pw;
  pItem->style = WS_CHILD | WS_VISIBLE | SS_LEFT;
  pItem->dwExtendedStyle = 0;
  pItem->x = 10;
  pItem->y = 35;
  pItem->cx = 230;
  pItem->cy = 10;
  pItem->id = -1;
  pw = (WORD *)(pItem + 1);
  *pw++ = 0xFFFF;
  *pw++ = 0x0082; // Static class
  const WCHAR *text2 = L"Game Text Language / Bahasa / \x8A00\x8A9E:";
  wcscpy((WCHAR *)pw, text2);
  pw += wcslen(text2) + 1;
  *pw++ = 0;

  // Control 1.5: Language ComboBox
  AlignDword(pw);
  pItem = (DLGITEMTEMPLATE *)pw;
  pItem->style =
      WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL;
  pItem->dwExtendedStyle = 0;
  pItem->x = 10;
  pItem->y = 48;
  pItem->cx = 200;
  pItem->cy = 100; // Dropdown height
  pItem->id = IDC_COMBO_LANGUAGE;
  pw = (WORD *)(pItem + 1);
  *pw++ = 0xFFFF;
  *pw++ = 0x0085; // ComboBox class
  *pw++ = 0;      // Empty title
  *pw++ = 0;      // No creation data

  // Control 3: Change Dialogue Font Button
  AlignDword(pw);
  pItem = (DLGITEMTEMPLATE *)pw;
  pItem->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
  pItem->dwExtendedStyle = 0;
  pItem->x = 10;
  pItem->y = 72;
  pItem->cx = 200;
  pItem->cy = 18;
  pItem->id = IDC_BTN_CHANGE_DIALOGUE_FONT;
  pw = (WORD *)(pItem + 1);
  *pw++ = 0xFFFF;
  *pw++ = 0x0080;
  const WCHAR *text3 = L"Change Dialogue Font...";
  wcscpy((WCHAR *)pw, text3);
  pw += wcslen(text3) + 1;
  *pw++ = 0;

  // Control 3.5: Change Backlog Font Button
  AlignDword(pw);
  pItem = (DLGITEMTEMPLATE *)pw;
  pItem->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
  pItem->dwExtendedStyle = 0;
  pItem->x = 10;
  pItem->y = 92;
  pItem->cx = 200;
  pItem->cy = 18;
  pItem->id = IDC_BTN_CHANGE_BACKLOG_FONT;
  pw = (WORD *)(pItem + 1);
  *pw++ = 0xFFFF;
  *pw++ = 0x0080;
  const WCHAR *text35 = L"Change Backlog Font...";
  wcscpy((WCHAR *)pw, text35);
  pw += wcslen(text35) + 1;
  *pw++ = 0;

  // Control 3.75: Change Backlog Name Font Button
  AlignDword(pw);
  pItem = (DLGITEMTEMPLATE *)pw;
  pItem->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
  pItem->dwExtendedStyle = 0;
  pItem->x = 10;
  pItem->y = 112;
  pItem->cx = 200;
  pItem->cy = 18;
  pItem->id = IDC_BTN_CHANGE_BACKLOG_NAME_FONT;
  pw = (WORD *)(pItem + 1);
  *pw++ = 0xFFFF;
  *pw++ = 0x0080;
  const WCHAR *text375 = L"Change Backlog Name Font...";
  wcscpy((WCHAR *)pw, text375);
  pw += wcslen(text375) + 1;
  *pw++ = 0;

  // Control: Advanced Settings Button
  AlignDword(pw);
  pItem = (DLGITEMTEMPLATE *)pw;
  pItem->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON;
  pItem->dwExtendedStyle = 0;
  pItem->x = 10;
  pItem->y = 132;
  pItem->cx = 200;
  pItem->cy = 18;
  pItem->id = IDC_BTN_ADVANCED;
  pw = (WORD *)(pItem + 1);
  *pw++ = 0xFFFF;
  *pw++ = 0x0080; // Button class
  const WCHAR *textAdv = L"Advanced Backlog Spacing...";
  wcscpy((WCHAR *)pw, textAdv);
  pw += wcslen(textAdv) + 1;
  *pw++ = 0;

  // Control 4: OK Button
  AlignDword(pw);
  pItem = (DLGITEMTEMPLATE *)pw;
  pItem->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP;
  pItem->dwExtendedStyle = 0;
  pItem->x = 50;
  pItem->y = 162;
  pItem->cx = 50;
  pItem->cy = 18;
  pItem->id = IDC_BTN_OK;
  pw = (WORD *)(pItem + 1);
  *pw++ = 0xFFFF;
  *pw++ = 0x0080;
  const WCHAR *text4 = L"OK";
  wcscpy((WCHAR *)pw, text4);
  pw += wcslen(text4) + 1;
  *pw++ = 0;

  // Control 5: Cancel Button
  AlignDword(pw);
  pItem = (DLGITEMTEMPLATE *)pw;
  pItem->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP;
  pItem->dwExtendedStyle = 0;
  pItem->x = 110;
  pItem->y = 162;
  pItem->cx = 50;
  pItem->cy = 18;
  pItem->id = IDC_BTN_CANCEL;
  pw = (WORD *)(pItem + 1);
  *pw++ = 0xFFFF;
  *pw++ = 0x0080;
  const WCHAR *text5 = L"Cancel";
  wcscpy((WCHAR *)pw, text5);
  pw += wcslen(text5) + 1;
  *pw++ = 0;

  // Control 6: About Button
  AlignDword(pw);
  pItem = (DLGITEMTEMPLATE *)pw;
  pItem->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP;
  pItem->dwExtendedStyle = 0;
  pItem->x = 170;
  pItem->y = 162;
  pItem->cx = 50;
  pItem->cy = 18;
  pItem->id = IDC_BTN_ABOUT;
  pw = (WORD *)(pItem + 1);
  *pw++ = 0xFFFF;
  *pw++ = 0x0080;
  const WCHAR *text6 = L"About";
  wcscpy((WCHAR *)pw, text6);
  pw += wcslen(text6) + 1;
  *pw++ = 0;

  // Show dialog
  ScopedPatchSettingsUi patchUiGuard;
  DialogBoxIndirectW(GetModuleHandleA(NULL), (DLGTEMPLATE *)buffer, g_mainWindow,
                     SettingsDialogProc);
}

static INT_PTR CALLBACK AdvancedSettingsDialogProc(HWND hwndDlg, UINT uMsg,
                                                   WPARAM wParam,
                                                   LPARAM lParam) {
  switch (uMsg) {
  case WM_INITDIALOG: {
    RECT rcParent, rcDlg;
    GetWindowRect(GetParent(hwndDlg), &rcParent);
    GetWindowRect(hwndDlg, &rcDlg);
    int x = rcParent.left +
            (rcParent.right - rcParent.left - (rcDlg.right - rcDlg.left)) / 2;
    int y = rcParent.top +
            (rcParent.bottom - rcParent.top - (rcDlg.bottom - rcDlg.top)) / 2;
    SetWindowPos(hwndDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    SetDlgItemInt(hwndDlg, IDC_EDIT_SPACING,
                  g_fontManager.GetBacklogLineSpacing(), TRUE);
    SetDlgItemInt(hwndDlg, IDC_EDIT_XOFFSET, g_fontManager.GetBacklogXOffset(),
                  TRUE);
    SetDlgItemInt(hwndDlg, IDC_EDIT_YOFFSET, g_fontManager.GetBacklogYOffset(),
                  TRUE);
    SetDlgItemInt(hwndDlg, IDC_EDIT_NAME_XOFFSET,
                  g_fontManager.GetBacklogNameXOffset(), TRUE);
    SetDlgItemInt(hwndDlg, IDC_EDIT_NAME_YOFFSET,
                  g_fontManager.GetBacklogNameYOffset(), TRUE);
    SetDlgItemInt(hwndDlg, IDC_EDIT_NAME_SPACING,
                  g_fontManager.GetBacklogNameSpacing(), TRUE);
    SetDlgItemInt(hwndDlg, IDC_EDIT_DIALOG_SPACING,
                  g_fontManager.GetBacklogDialogSpacing(), TRUE);
    return TRUE;
  }

  case WM_COMMAND: {
    if (LOWORD(wParam) == IDC_BTN_OK) {
      int spacing = GetDlgItemInt(hwndDlg, IDC_EDIT_SPACING, NULL, TRUE);
      int xOffset = GetDlgItemInt(hwndDlg, IDC_EDIT_XOFFSET, NULL, TRUE);
      int yOffset = GetDlgItemInt(hwndDlg, IDC_EDIT_YOFFSET, NULL, TRUE);
      int nameXOffset =
          GetDlgItemInt(hwndDlg, IDC_EDIT_NAME_XOFFSET, NULL, TRUE);
      int nameYOffset =
          GetDlgItemInt(hwndDlg, IDC_EDIT_NAME_YOFFSET, NULL, TRUE);
      int nameSpacing =
          GetDlgItemInt(hwndDlg, IDC_EDIT_NAME_SPACING, NULL, TRUE);
      int dialogSpacing =
          GetDlgItemInt(hwndDlg, IDC_EDIT_DIALOG_SPACING, NULL, TRUE);

      g_fontManager.SetBacklogOffsets(xOffset, yOffset, spacing, nameXOffset,
                                      nameYOffset, nameSpacing, dialogSpacing);
      g_fontManager.SetAdvancedSettings(true); // Implicitly enabled when used
      EndDialog(hwndDlg, IDOK);
      return TRUE;
    } else if (LOWORD(wParam) == IDC_BTN_CANCEL) {
      EndDialog(hwndDlg, IDCANCEL);
      return TRUE;
    }
    break;
  }

  case WM_CLOSE:
    EndDialog(hwndDlg, IDCANCEL);
    return TRUE;
  }
  return FALSE;
}

static void ShowAdvancedSettingsDialog(HWND parent) {
  const int bufSize = 4096;
  BYTE *buffer = new BYTE[bufSize];
  memset(buffer, 0, bufSize);
  WORD *pw = (WORD *)buffer;

  DLGTEMPLATE *pDlg = (DLGTEMPLATE *)pw;
  pDlg->style = WS_POPUP | WS_BORDER | WS_SYSMENU | WS_CAPTION | DS_MODALFRAME |
                DS_CENTER | DS_SETFONT;
  pDlg->cdit = 16; // 7 labels, 7 edits, 2 buttons
  pDlg->cx = 200;
  pDlg->cy = 200;

  pw = (WORD *)(pDlg + 1);
  *pw++ = 0;
  *pw++ = 0;
  wcscpy((WCHAR *)pw, L"Advanced Backlog Settings");
  pw += wcslen((WCHAR *)pw) + 1;
  *pw++ = 9;
  wcscpy((WCHAR *)pw, L"Segoe UI");
  pw += wcslen((WCHAR *)pw) + 1;

  auto AlignDword = [](WORD *&p) {
    while ((ULONG_PTR)p & 3)
      p++;
  };
  auto AddLabelAndEdit = [&](int idLbl, int idEdit, const WCHAR *labelText,
                             int yPos) {
    AlignDword(pw);
    DLGITEMTEMPLATE *pItem = (DLGITEMTEMPLATE *)pw;
    pItem->style = WS_CHILD | WS_VISIBLE | SS_LEFT;
    pItem->x = 10;
    pItem->y = yPos + 2;
    pItem->cx = 100;
    pItem->cy = 14;
    pItem->id = idLbl;
    pw = (WORD *)(pItem + 1);
    *pw++ = 0xFFFF;
    *pw++ = 0x0082;
    wcscpy((WCHAR *)pw, labelText);
    pw += wcslen(labelText) + 1;
    *pw++ = 0;

    AlignDword(pw);
    pItem = (DLGITEMTEMPLATE *)pw;
    pItem->style =
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
    pItem->dwExtendedStyle = WS_EX_CLIENTEDGE;
    pItem->x = 115;
    pItem->y = yPos;
    pItem->cx = 40;
    pItem->cy = 14;
    pItem->id = idEdit;
    pw = (WORD *)(pItem + 1);
    *pw++ = 0xFFFF;
    *pw++ = 0x0081;
    wcscpy((WCHAR *)pw, L"");
    pw += wcslen(L"") + 1;
    *pw++ = 0;
  };

  AddLabelAndEdit(IDC_LBL_SPACING, IDC_EDIT_SPACING, L"Line Spacing:", 10);
  AddLabelAndEdit(IDC_LBL_XOFFSET, IDC_EDIT_XOFFSET, L"X Offset:", 30);
  AddLabelAndEdit(IDC_LBL_YOFFSET, IDC_EDIT_YOFFSET, L"Y Offset:", 50);
  AddLabelAndEdit(IDC_LBL_NAME_XOFFSET, IDC_EDIT_NAME_XOFFSET,
                  L"Name X Offset:", 70);
  AddLabelAndEdit(IDC_LBL_NAME_YOFFSET, IDC_EDIT_NAME_YOFFSET,
                  L"Name Y Offset:", 90);
  AddLabelAndEdit(IDC_LBL_NAME_SPACING, IDC_EDIT_NAME_SPACING,
                  L"Name Ext Spacing:", 110);
  AddLabelAndEdit(IDC_LBL_DIALOG_SPACING, IDC_EDIT_DIALOG_SPACING,
                  L"Base Ext Spacing:", 130);

  AlignDword(pw);
  DLGITEMTEMPLATE *pItem = (DLGITEMTEMPLATE *)pw;
  pItem->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP;
  pItem->x = 30;
  pItem->y = 160;
  pItem->cx = 50;
  pItem->cy = 16;
  pItem->id = IDC_BTN_OK;
  pw = (WORD *)(pItem + 1);
  *pw++ = 0xFFFF;
  *pw++ = 0x0080;
  wcscpy((WCHAR *)pw, L"OK");
  pw += wcslen(L"OK") + 1;
  *pw++ = 0;

  AlignDword(pw);
  pItem = (DLGITEMTEMPLATE *)pw;
  pItem->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP;
  pItem->x = 100;
  pItem->y = 160;
  pItem->cx = 50;
  pItem->cy = 16;
  pItem->id = IDC_BTN_CANCEL;
  pw = (WORD *)(pItem + 1);
  *pw++ = 0xFFFF;
  *pw++ = 0x0080;
  wcscpy((WCHAR *)pw, L"Cancel");
  pw += wcslen(L"Cancel") + 1;
  *pw++ = 0;

  ScopedPatchSettingsUi patchUiGuard;
  DialogBoxIndirectW(GetModuleHandleA(NULL), (DLGTEMPLATE *)buffer, parent,
                     AdvancedSettingsDialogProc);
  delete[] buffer;
}

// Window procedure hook to add menu item and keyboard shortcut
static LRESULT CALLBACK MenuWndProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                    LPARAM lParam);
static WNDPROC g_OrigWndProc = nullptr;

static bool IsVersionInfoTitleW(const wchar_t *title) {
  if (!title || !*title)
    return false;
  return (wcsstr(title, L"バージョン情報") != nullptr) ||
         (wcsstr(title, L"Informasi Versi") != nullptr) ||
         (wcsstr(title, L"Version Info") != nullptr);
}

static bool IsVersionInfoTitleA(const char *titleA) {
  if (!titleA || !*titleA)
    return false;

  // Fast path: after localization.
  if (strstr(titleA, "Informasi Versi") != nullptr)
    return true;

  // Decode as Shift-JIS to match バージョン情報 reliably.
  wchar_t tmp[256] = {};
  if (MultiByteToWideChar(932, 0, titleA, -1, tmp, 256) > 0) {
    return IsVersionInfoTitleW(tmp);
  }
  return false;
}

static HFONT GetVersionInfoUiFont() {
  if (g_versionInfoUiFont)
    return g_versionInfoUiFont;
  g_versionInfoUiFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH, L"Segoe UI");
  return g_versionInfoUiFont;
}

struct VerInfoEnumState {
  HWND hHeader = NULL;
  HWND hGroup = NULL;
  HWND hCheckDirectX = NULL;
  HWND hCheckNoMovie = NULL;
  HWND hOk = NULL;
};

static BOOL CALLBACK LocalizeVersionInfoChildProc(HWND hwnd, LPARAM lParam) {
  VerInfoEnumState *state = reinterpret_cast<VerInfoEnumState *>(lParam);
  if (!state)
    return TRUE;

  HFONT hFont = GetVersionInfoUiFont();
  if (hFont)
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)hFont, TRUE);

  WCHAR cls[32] = {};
  GetClassNameW(hwnd, cls, 32);

  WCHAR text[512] = {};
  int len = GetWindowTextW(hwnd, text, 511);

  if (_wcsicmp(cls, L"Button") == 0) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    UINT btnType = (UINT)(style & BS_TYPEMASK);

    if (btnType == BS_GROUPBOX) {
      state->hGroup = hwnd;
      SetWindowTextW(hwnd, L"Pengaturan Sistem");
      return TRUE;
    }

    if (btnType == BS_AUTOCHECKBOX || btnType == BS_CHECKBOX ||
        btnType == BS_AUTO3STATE || btnType == BS_3STATE) {
      if (state->hCheckDirectX == NULL) {
        state->hCheckDirectX = hwnd;
        SetWindowTextW(hwnd, L"Gunakan DirectX untuk perpindahan layar");
      } else if (state->hCheckNoMovie == NULL) {
        state->hCheckNoMovie = hwnd;
        SetWindowTextW(hwnd, L"Jangan putar movie");
      }
      return TRUE;
    }

    if (wcsstr(text, L"画面切り替えにDirectXを使う") != nullptr &&
        state->hCheckDirectX == NULL) {
      state->hCheckDirectX = hwnd;
      SetWindowTextW(hwnd, L"Gunakan DirectX untuk perpindahan layar");
      return TRUE;
    }
    if (wcsstr(text, L"Movieを再生しない") != nullptr &&
        state->hCheckNoMovie == NULL) {
      state->hCheckNoMovie = hwnd;
      SetWindowTextW(hwnd, L"Jangan putar movie");
      return TRUE;
    }

    if (wcsstr(text, L"OK") != nullptr || wcsstr(text, L"ＯＫ") != nullptr ||
        ((btnType == BS_DEFPUSHBUTTON || btnType == BS_PUSHBUTTON) &&
         state->hOk == NULL)) {
      state->hOk = hwnd;
      SetWindowTextW(hwnd, L"OK");
    }
    return TRUE;
  }

  if (_wcsicmp(cls, L"Static") != 0)
    return TRUE;
  if (len <= 0)
    return TRUE;

  // Header text.
  if (wcsstr(text, L"DC2TY") != nullptr ||
      wcsstr(text, L"パッチ") != nullptr ||
      wcsstr(text, L"Patch") != nullptr || state->hHeader == NULL) {
    state->hHeader = hwnd;
    SetWindowTextW(hwnd, L"Patch bahasa Indonesia untuk\r\nDC2TY");
    return TRUE;
  }

  return TRUE;
}

static void LayoutVersionInfoDialog(HWND hDlg, VerInfoEnumState &state) {
  const int dlgW = 460;
  const int dlgH = 250;
  SetWindowPos(hDlg, NULL, 0, 0, dlgW, dlgH,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

  if (state.hHeader) {
    MoveWindow(state.hHeader, 12, 12, dlgW - 24, 44, TRUE);
  }
  if (state.hGroup) {
    MoveWindow(state.hGroup, 12, 62, dlgW - 24, 104, TRUE);
  }
  if (state.hCheckDirectX) {
    MoveWindow(state.hCheckDirectX, 24, 90, dlgW - 48, 24, TRUE);
  }
  if (state.hCheckNoMovie) {
    MoveWindow(state.hCheckNoMovie, 24, 122, dlgW - 48, 24, TRUE);
  }
  if (state.hOk) {
    MoveWindow(state.hOk, (dlgW - 88) / 2, 192, 88, 26, TRUE);
  }
}

static void LocalizeVersionInfoDialog(HWND hDlg) {
  if (!hDlg)
    return;
  if (GetPropW(hDlg, kVerInfoProp) != NULL)
    return;

  SetPropW(hDlg, kVerInfoProp, (HANDLE)1);
  SetWindowTextW(hDlg, L"Informasi Versi");

  HFONT hFont = GetVersionInfoUiFont();
  if (hFont)
    SendMessageW(hDlg, WM_SETFONT, (WPARAM)hFont, TRUE);

  VerInfoEnumState state;
  EnumChildWindows(hDlg, LocalizeVersionInfoChildProc, (LPARAM)&state);
  LayoutVersionInfoDialog(hDlg, state);
  InvalidateRect(hDlg, NULL, TRUE);
  UpdateWindow(hDlg);
}

static void LocalizeVersionInfoMenuOnly(HMENU hMenu) {
  if (!hMenu)
    return;

  int count = GetMenuItemCount(hMenu);
  bool changed = false;
  for (int i = 0; i < count; ++i) {
    if (GetMenuState(hMenu, (UINT)i, MF_BYPOSITION) & MF_SEPARATOR)
      continue;

    WCHAR text[256] = {};
    if (GetMenuStringW(hMenu, (UINT)i, text, 255, MF_BYPOSITION) <= 0)
      continue;

    if (wcsstr(text, L"バージョン情報")) {
      UINT id = GetMenuItemID(hMenu, i);
      if (id != (UINT)-1)
        g_versionInfoMenuCommandId = id;
      if (id != (UINT)-1) {
        ModifyMenuW(hMenu, id, MF_BYCOMMAND | MF_STRING, id,
                    L"Informasi Versi");
      } else {
        ModifyMenuW(hMenu, (UINT)i, MF_BYPOSITION | MF_STRING, (UINT_PTR)-1,
                    L"Informasi Versi");
      }
      changed = true;
      break;
    }
  }

  // Fallback for builds/locales where the Japanese text cannot be matched
  // reliably via GetMenuStringW. We only touch the bottom non-separator item
  // that isn't the patch settings command.
  if (!changed && count > 0) {
    for (int i = count - 1; i >= 0; --i) {
      if (GetMenuState(hMenu, (UINT)i, MF_BYPOSITION) & MF_SEPARATOR)
        continue;
      UINT id = GetMenuItemID(hMenu, i);
      if (id == 0x1919999)
        continue;

      WCHAR text[256] = {};
      GetMenuStringW(hMenu, (UINT)i, text, 255, MF_BYPOSITION);
      if (wcsstr(text, L"DC2TY Patch Settings") != nullptr)
        continue;

      if (id != (UINT)-1) {
        g_versionInfoMenuCommandId = id;
        ModifyMenuW(hMenu, id, MF_BYCOMMAND | MF_STRING, id,
                    L"Informasi Versi");
      } else {
        ModifyMenuW(hMenu, (UINT)i, MF_BYPOSITION | MF_STRING, (UINT_PTR)-1,
                    L"Informasi Versi");
      }
      break;
    }
  }

  // Deduplicate accidental duplicates: keep only the last "Informasi Versi"
  // entry (the original slot is typically near the bottom of the system menu).
  std::vector<int> infoPositions;
  count = GetMenuItemCount(hMenu);
  for (int i = 0; i < count; ++i) {
    if (GetMenuState(hMenu, (UINT)i, MF_BYPOSITION) & MF_SEPARATOR)
      continue;
    WCHAR text[256] = {};
    if (GetMenuStringW(hMenu, (UINT)i, text, 255, MF_BYPOSITION) <= 0)
      continue;
    if (wcsstr(text, L"Informasi Versi") != nullptr) {
      infoPositions.push_back(i);
    }
  }

  if (infoPositions.size() > 1) {
    for (int i = (int)infoPositions.size() - 2; i >= 0; --i) {
      RemoveMenu(hMenu, (UINT)infoPositions[i], MF_BYPOSITION);
    }
  }

  // Collapse adjacent separators (visual cleanup).
  count = GetMenuItemCount(hMenu);
  bool prevSep = false;
  for (int i = 0; i < count;) {
    bool isSep = (GetMenuState(hMenu, (UINT)i, MF_BYPOSITION) & MF_SEPARATOR) != 0;
    if (isSep && prevSep) {
      RemoveMenu(hMenu, (UINT)i, MF_BYPOSITION);
      count--;
      continue;
    }
    prevSep = isSep;
    i++;
  }
}

static void InitMainGameWindow(HWND hWnd) {
  if (!hWnd)
    return;

  // Normalize native window chrome: keep close/minimize, disable maximize.
  LONG_PTR style = GetWindowLongPtrW(hWnd, GWL_STYLE);
  style |= (WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
  style &= ~(WS_MAXIMIZEBOX | WS_THICKFRAME);
  SetWindowLongPtrW(hWnd, GWL_STYLE, style);

  // Some game classes set CS_NOCLOSE which greys out the X button.
  // Remove it so close can be clicked and handled by our confirm dialog.
  LONG_PTR clsStyle = GetClassLongPtrW(hWnd, GCL_STYLE);
  if (clsStyle & CS_NOCLOSE) {
    SetClassLongPtrW(hWnd, GCL_STYLE, clsStyle & ~CS_NOCLOSE);
  }

  SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                   SWP_FRAMECHANGED);

  // Override game title.
  SetWindowTextW(hWnd, kMainWindowTitle);

  // Add patch options to system menu.
  HMENU hSysMenu = GetSystemMenu(hWnd, FALSE);
  if (hSysMenu) {
    EnableMenuItem(hSysMenu, SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(hSysMenu, SC_MAXIMIZE, MF_BYCOMMAND | MF_GRAYED);

    if (GetMenuState(hSysMenu, 0x1919999, MF_BYCOMMAND) == (UINT)-1) {
      int cnt = GetMenuItemCount(hSysMenu);
      bool needSep = true;
      if (cnt > 0) {
        needSep =
            (GetMenuState(hSysMenu, (UINT)(cnt - 1), MF_BYPOSITION) &
             MF_SEPARATOR) == 0;
      }
      if (needSep) {
        AppendMenuW(hSysMenu, MF_SEPARATOR, 0, nullptr);
      }
      AppendMenuW(hSysMenu, MF_STRING, 0x1919999, L"DC2TY Patch Settings... (F11)");
    }
    LocalizeVersionInfoMenuOnly(hSysMenu);
  }
}

static HWND WINAPI Hook_CreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName,
                                        LPCSTR lpWindowName, DWORD dwStyle,
                                        int X, int Y, int nWidth, int nHeight,
                                        HWND hWndParent, HMENU hMenu,
                                        HINSTANCE hInstance, LPVOID lpParam) {

  HWND hWnd = Real_CreateWindowExA(dwExStyle, lpClassName, lpWindowName,
                                   dwStyle, X, Y, nWidth, nHeight, hWndParent,
                                   hMenu, hInstance, lpParam);

  // Check if this is the main game window (has a title and is a top-level
  // window)
  bool classNameIsString = lpClassName && !IS_INTRESOURCE(lpClassName);
  bool windowNameIsString = lpWindowName && !IS_INTRESOURCE(lpWindowName);

  if (hWnd && windowNameIsString && hWndParent == NULL &&
      g_mainWindow == nullptr) {
    g_mainWindow = hWnd;
    InitMainGameWindow(hWnd);

    // Subclass the window to handle our menu command
    g_OrigWndProc =
        (WNDPROC)SetWindowLongPtrA(hWnd, GWLP_WNDPROC, (LONG_PTR)MenuWndProc);
  }

  // Version-info popup created from system menu.
  if (hWnd && hWndParent == NULL) {
    bool looksLikeVersionDialog = g_expectVersionInfoDialog;
    if (!looksLikeVersionDialog && classNameIsString &&
        strcmp(lpClassName, "#32770") == 0) {
      looksLikeVersionDialog = IsVersionInfoTitleA(lpWindowName);
    } else if (!looksLikeVersionDialog && windowNameIsString) {
      looksLikeVersionDialog = IsVersionInfoTitleA(lpWindowName);
    }
    if (looksLikeVersionDialog) {
      LocalizeVersionInfoDialog(hWnd);
      g_expectVersionInfoDialog = false;
    }
  } else if (hWnd && hWndParent != NULL && GetPropW(hWndParent, kVerInfoProp)) {
    VerInfoEnumState state;
    LocalizeVersionInfoChildProc(hWnd, (LPARAM)&state);
  }

  return hWnd;
}

static HWND WINAPI Hook_CreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName,
                                        LPCWSTR lpWindowName, DWORD dwStyle,
                                        int X, int Y, int nWidth, int nHeight,
                                        HWND hWndParent, HMENU hMenu,
                                        HINSTANCE hInstance, LPVOID lpParam) {
  HWND hWnd = Real_CreateWindowExW(dwExStyle, lpClassName, lpWindowName, dwStyle,
                                   X, Y, nWidth, nHeight, hWndParent, hMenu,
                                   hInstance, lpParam);

  bool classNameIsString = lpClassName && !IS_INTRESOURCE(lpClassName);
  bool windowNameIsString = lpWindowName && !IS_INTRESOURCE(lpWindowName);

  if (hWnd && windowNameIsString && hWndParent == NULL &&
      g_mainWindow == nullptr) {
    g_mainWindow = hWnd;
    InitMainGameWindow(hWnd);
    g_OrigWndProc =
        (WNDPROC)SetWindowLongPtrW(hWnd, GWLP_WNDPROC, (LONG_PTR)MenuWndProc);
  }

  if (hWnd && hWndParent == NULL) {
    bool looksLikeVersionDialog = g_expectVersionInfoDialog;
    if (!looksLikeVersionDialog && classNameIsString &&
        wcscmp(lpClassName, L"#32770") == 0) {
      looksLikeVersionDialog = IsVersionInfoTitleW(lpWindowName);
    } else if (!looksLikeVersionDialog && windowNameIsString) {
      looksLikeVersionDialog = IsVersionInfoTitleW(lpWindowName);
    }
    if (looksLikeVersionDialog) {
      LocalizeVersionInfoDialog(hWnd);
      g_expectVersionInfoDialog = false;
    }
  } else if (hWnd && hWndParent != NULL && GetPropW(hWndParent, kVerInfoProp)) {
    VerInfoEnumState state;
    LocalizeVersionInfoChildProc(hWnd, (LPARAM)&state);
  }

  return hWnd;
}

static LRESULT CALLBACK MenuWndProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                    LPARAM lParam) {
  if (uMsg == WM_ENTERMENULOOP) {
    ++g_systemMenuLoopDepth;
  } else if (uMsg == WM_EXITMENULOOP) {
    if (g_systemMenuLoopDepth > 0)
      --g_systemMenuLoopDepth;
  }

  if (uMsg == WM_CLOSE) {
    if (g_allowCloseWithoutPrompt) {
      return CallWindowProcA(g_OrigWndProc, hWnd, uMsg, wParam, lParam);
    }
    if (!g_closeRequestedFromSysCommand) {
      return CallWindowProcA(g_OrigWndProc, hWnd, uMsg, wParam, lParam);
    }
    g_closeRequestedFromSysCommand = false;

    ScopedCloseConfirmUi closeUiGuard;
    int answer = MessageBoxW(
        hWnd, L"Apakah Anda ingin keluar dari permainan?", kMainWindowTitle,
        MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
    if (answer == IDYES) {
      g_allowCloseWithoutPrompt = true;
      LRESULT ret = CallWindowProcA(g_OrigWndProc, hWnd, uMsg, wParam, lParam);
      g_allowCloseWithoutPrompt = false;
      return ret;
    }
    return 0;
  }

  // Handle F11 key press
  if (uMsg == WM_KEYDOWN && wParam == VK_F11) {
    ShowSettingsDialog();
    return 0;
  }

  if (uMsg == WM_INITMENUPOPUP) {
    HMENU menu = (HMENU)wParam;
    if (menu) {
      EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
      EnableMenuItem(menu, SC_MAXIMIZE, MF_BYCOMMAND | MF_GRAYED);
      LocalizeVersionInfoMenuOnly(menu);
    }
  }

  if (uMsg == WM_COMMAND) {
    UINT cmdId = LOWORD(wParam);
    if (g_versionInfoMenuCommandId != 0 && cmdId == g_versionInfoMenuCommandId) {
      g_expectVersionInfoDialog = true;
    }
  }

  if (uMsg == WM_SYSCOMMAND) {
    UINT sysIdRaw = (UINT)wParam;
    UINT sysIdMasked = sysIdRaw & 0xFFF0;
    if (sysIdMasked == SC_CLOSE) {
      g_closeRequestedFromSysCommand = true;
    }
    if (g_versionInfoMenuCommandId != 0 &&
        (sysIdRaw == g_versionInfoMenuCommandId ||
         sysIdMasked == g_versionInfoMenuCommandId)) {
      g_expectVersionInfoDialog = true;
    }

    if (wParam == 0x1919999) {
      // Show settings dialog
      ShowSettingsDialog();
      return 0;
    }

    // Legacy individual handlers (kept for backwards compatibility)
  if (wParam == 0x114514) {
      std::wstring chosenName;
      int chosenHeight = 0;
      if (ShowPatchFontChooser(hWnd, g_fontManager.GetDialogueFontName(),
                               g_fontManager.GetDialogueFontSize(), chosenName,
                               chosenHeight)) {
        g_fontManager.SetDialogueFont(chosenName, chosenHeight);
        MessageBoxW(hWnd, L"Font updated!", L"DC2TY Patch",
                    MB_OK | MB_ICONINFORMATION);
      }
      return 0;
    }

    if (wParam == 0x1919810) {
      // Toggle backlog show all icons
      HMENU hSysMenu = GetSystemMenu(hWnd, FALSE);
      if (hSysMenu) {
        MENUITEMINFOW mii = {};
        mii.cbSize = sizeof(MENUITEMINFOW);
        mii.fMask = MIIM_STATE;
        GetMenuItemInfoW(hSysMenu, 0x1919810, FALSE, &mii);

        // Toggle the state
        bool isEnabled = !(mii.fState & MF_CHECKED);
        g_fontManager.SetEnableBacklogAllIcon(isEnabled);

        ModifyMenuW(hSysMenu, 0x1919810, isEnabled ? MF_CHECKED : MF_UNCHECKED,
                    0x1919810, L"Backlog Show All Icons");
      }
      return 0;
    }

    if (wParam == 0x1919811) {
      // Toggle file redirection
      HMENU hSysMenu = GetSystemMenu(hWnd, FALSE);
      if (hSysMenu) {
        MENUITEMINFOW mii = {};
        mii.cbSize = sizeof(MENUITEMINFOW);
        mii.fMask = MIIM_STATE;
        GetMenuItemInfoW(hSysMenu, 0x1919811, FALSE, &mii);

        bool isEnabled = !(mii.fState & MF_CHECKED);
        g_fontManager.SetLanguage(isEnabled ? 1 : 0);

        ModifyMenuW(hSysMenu, 0x1919811, isEnabled ? MF_CHECKED : MF_UNCHECKED,
                    0x1919811, L"Enable id_Data File Redirection");

        MessageBoxW(
            hWnd,
            isEnabled
                ? L"File redirection ENABLED.\nTranslated files from "
                  L"id_Data\\ "
                  L"will be used.\n\n⚠️ RESTART REQUIRED:\nReturn to title "
                  L"screen or restart the game for this to take full "
                  L"effect.\n(Already-loaded files are cached in memory.)"
                : L"File redirection DISABLED.\nOriginal files from "
                  L"AdvData\\ "
                  L"will be used.\n\n⚠️ RESTART REQUIRED:\nReturn to title "
                  L"screen or restart the game for this to take full "
                  L"effect.\n(Already-loaded files are cached in memory.)",
            L"DC2TY Patch", MB_OK | MB_ICONWARNING);
      }
      return 0;
    }
  }
  return CallWindowProcA(g_OrigWndProc, hWnd, uMsg, wParam, lParam);
}

// ============================================================================
// IAT (Import Address Table) Patching
// Safer than Detours for system APIs - patches the game's import table
// directly
// ============================================================================
static bool PatchIAT(HMODULE hModule, const char *dllName, PROC oldFunc,
                     PROC newFunc) {
  BYTE *base = (BYTE *)hModule;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return false;

  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return false;

  IMAGE_IMPORT_DESCRIPTOR *imp =
      (IMAGE_IMPORT_DESCRIPTOR
           *)(base +
              nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
                  .VirtualAddress);

  for (; imp->Name; imp++) {
    const char *name = (const char *)(base + imp->Name);
    if (_stricmp(name, dllName) != 0)
      continue;

    IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
    for (; thunk->u1.Function; thunk++) {
      PROC *funcPtr = (PROC *)&thunk->u1.Function;
      if (*funcPtr == oldFunc) {
        DWORD oldProtect;
        VirtualProtect(funcPtr, sizeof(PROC), PAGE_READWRITE, &oldProtect);
        *funcPtr = newFunc;
        VirtualProtect(funcPtr, sizeof(PROC), oldProtect, &oldProtect);
        return true;
      }
    }
  }
  return false;
}

// ============================================================================
// Crash Logger (Vectored Exception Handler)
// ============================================================================
static LONG WINAPI CrashLogger(EXCEPTION_POINTERS *ep) {
  DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW ||
      code == EXCEPTION_ILLEGAL_INSTRUCTION ||
      code == EXCEPTION_PRIV_INSTRUCTION) {
    FILE *f = fopen("dc2ty_crash.log", "a");
    if (f) {
      fprintf(f, "CRASH: code=0x%08X EIP=0x%08X", code, ep->ContextRecord->Eip);
      if (code == EXCEPTION_ACCESS_VIOLATION)
        fprintf(f, " addr=0x%08X (%s)",
                (DWORD)ep->ExceptionRecord->ExceptionInformation[1],
                ep->ExceptionRecord->ExceptionInformation[0] ? "write"
                                                             : "read");
      fprintf(f, "\n  EAX=%08X EBX=%08X ECX=%08X EDX=%08X\n",
              ep->ContextRecord->Eax, ep->ContextRecord->Ebx,
              ep->ContextRecord->Ecx, ep->ContextRecord->Edx);
      fprintf(f, "  ESP=%08X EBP=%08X ESI=%08X EDI=%08X\n",
              ep->ContextRecord->Esp, ep->ContextRecord->Ebp,
              ep->ContextRecord->Esi, ep->ContextRecord->Edi);
      fclose(f);
    }
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// ============================================================================
// id_Data -> AdvData Hardlink Sync
// ============================================================================
// The CIRCUS engine's internal CRX image loader bypasses Win32 CreateFileA for
// some rendering codepaths (e.g. Scenario Mode navigation). To ensure
// translated graphics always load, we create hardlinks (or copies) from id_Data
// into the AdvData directories at startup, making files appear where the engine
// expects.

static void EnsureDirectoryExists(const char *path) {
  char tmp[MAX_PATH];
  strcpy_s(tmp, path);
  for (char *p = tmp; *p; p++) {
    if (*p == '\\' || *p == '/') {
      char saved = *p;
      *p = '\0';
      CreateDirectoryA(tmp, NULL);
      *p = saved;
    }
  }
  CreateDirectoryA(tmp, NULL);
}

static int SyncDirectoryRecursive(const char *srcDir, const char *srcRoot,
                                  const char *dstRoot, const char *cacheRoot,
                                  FILE *manifest) {
  int count = 0;
  char searchPath[MAX_PATH];
  sprintf_s(searchPath, "%s\\*", srcDir);

  WIN32_FIND_DATAA fd;
  HANDLE hFind = FindFirstFileA(searchPath, &fd);
  if (hFind == INVALID_HANDLE_VALUE)
    return 0;

  size_t srcRootLen = strlen(srcRoot);

  do {
    if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
      continue;

    // CRITICAL: Ignore cache and manifest files to prevent infinite loop of
    // copying backups!
    if (_stricmp(fd.cFileName, ".original_cache") == 0 ||
        _stricmp(fd.cFileName, ".sync_manifest") == 0) {
      continue;
    }

    char fullSrc[MAX_PATH];
    sprintf_s(fullSrc, "%s\\%s", srcDir, fd.cFileName);

    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      count += SyncDirectoryRecursive(fullSrc, srcRoot, dstRoot, cacheRoot,
                                      manifest);
    } else {
      // Only sync graphic/animation files
      const char *ext = strrchr(fd.cFileName, '.');
      if (!ext)
        continue;
      if (_stricmp(ext, ".crx") != 0 && _stricmp(ext, ".grp") != 0 &&
          _stricmp(ext, ".crm") != 0 && _stricmp(ext, ".pck") != 0)
        continue;

      // Build destination path: dstRoot + relative path after srcRoot
      const char *relPath = fullSrc + srcRootLen;
      char fullDst[MAX_PATH];
      sprintf_s(fullDst, "%s%s", dstRoot, relPath);

      // Ensure target directory exists
      char dstDir[MAX_PATH];
      strcpy_s(dstDir, fullDst);
      char *lastSep = strrchr(dstDir, '\\');
      if (lastSep) {
        *lastSep = '\0';
        EnsureDirectoryExists(dstDir);
      }

      // Back up the original AdvData file to cache before overwriting
      DWORD dstAttr = GetFileAttributesA(fullDst);
      if (dstAttr != INVALID_FILE_ATTRIBUTES) {
        char cachePath[MAX_PATH];
        sprintf_s(cachePath, "%s%s", cacheRoot, relPath);

        // Ensure cache directory exists
        char cacheDir[MAX_PATH];
        strcpy_s(cacheDir, cachePath);
        char *cacheSep = strrchr(cacheDir, '\\');
        if (cacheSep) {
          *cacheSep = '\0';
          EnsureDirectoryExists(cacheDir);
        }

        // Only back up if cache doesn't already have this file
        DWORD cacheAttr = GetFileAttributesA(cachePath);
        if (cacheAttr == INVALID_FILE_ATTRIBUTES) {
          MoveFileA(fullDst, cachePath);
        } else {
          DeleteFileA(fullDst);
        }
      }

      // Create hardlink (fall back to copy)
      if (CreateHardLinkA(fullDst, fullSrc, NULL) ||
          CopyFileA(fullSrc, fullDst, FALSE)) {
        count++;
        // Write to manifest
        if (manifest) {
          fprintf(manifest, "%s\n", fullDst);
        }
      }
    }
  } while (FindNextFileA(hFind, &fd));

  FindClose(hFind);
  return count;
}

static void SyncGraphicsFromDir(const char *srcRelDir, const char *manifestFile,
                                const char *cacheRelDir) {
  char srcAbs[MAX_PATH], dstAbs[MAX_PATH], cacheAbs[MAX_PATH];
  GetFullPathNameA(srcRelDir, MAX_PATH, srcAbs, NULL);
  GetFullPathNameA(".\\AdvData", MAX_PATH, dstAbs, NULL);
  GetFullPathNameA(cacheRelDir, MAX_PATH, cacheAbs, NULL);

  DWORD attr = GetFileAttributesA(srcAbs);
  if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
    return;

  FILE *manifest = fopen(manifestFile, "w");
  int count =
      SyncDirectoryRecursive(srcAbs, srcAbs, dstAbs, cacheAbs, manifest);
  if (manifest)
    fclose(manifest);

  char msg[256];
  sprintf_s(msg, "DC2TYPatch: Synced %d graphic files from %s to AdvData\n",
            count, srcRelDir);
  OutputDebugStringA(msg);
}

static void CleanupSyncedFrom(const char *manifestFile,
                              const char *cacheRelDir) {
  FILE *manifest = fopen(manifestFile, "r");
  if (!manifest)
    return;

  char cacheAbs[MAX_PATH], dstAbs[MAX_PATH];
  GetFullPathNameA(cacheRelDir, MAX_PATH, cacheAbs, NULL);
  GetFullPathNameA(".\\AdvData", MAX_PATH, dstAbs, NULL);

  size_t dstRootLen = strlen(dstAbs);

  int count = 0;
  char line[MAX_PATH];
  while (fgets(line, MAX_PATH, manifest)) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
      line[--len] = '\0';
    if (len == 0)
      continue;

    DeleteFileA(line);

    if (len > dstRootLen) {
      const char *relPath = line + dstRootLen;
      char cachePath[MAX_PATH];
      sprintf_s(cachePath, "%s%s", cacheAbs, relPath);

      DWORD cacheAttr = GetFileAttributesA(cachePath);
      if (cacheAttr != INVALID_FILE_ATTRIBUTES) {
        MoveFileA(cachePath, line);
      }
    }
    count++;
  }
  fclose(manifest);
  DeleteFileA(manifestFile);

  char msg[256];
  sprintf_s(
      msg,
      "DC2TYPatch: Cleaned up %d synced files, restored originals from cache\n",
      count);
  OutputDebugStringA(msg);
}

// ============================================================================
// Initialization
// ============================================================================

static void InitPatch() {
  LogPatchLoad("InitPatch.begin");
  DiagLogProcessState("InitPatch.begin");
  InitDvdBypass();
  if (g_dvdSpoofDriveLetter)
    DiagLog("DVD bypass: spoofing drive %c: as CD-ROM", g_dvdSpoofDriveLetter);
  InitSerialKeyBypass();
  // Install crash logger first so we can diagnose any crashes
  AddVectoredExceptionHandler(1, CrashLogger);
  // Scene replay marquee experiments are disabled for stability. DirectDraw
  // probing can deadlock this ADV32 build during startup.

  g_fontManager.Init();

  // Read language setting
  int lang = g_fontManager.GetLanguage();

  // Handle id_Data (Indonesian) sync/cleanup
  if (lang == 1) {
    SyncGraphicsFromDir(".\\id_Data", ".\\id_Data\\.sync_manifest",
                        ".\\id_Data\\.original_cache");
  } else {
    CleanupSyncedFrom(".\\id_Data\\.sync_manifest",
                      ".\\id_Data\\.original_cache");
  }

  // Handle eng_data (English) sync/cleanup
  if (lang == 2) {
    SyncGraphicsFromDir(".\\eng_data", ".\\eng_data\\.sync_manifest",
                        ".\\eng_data\\.original_cache");
  } else {
    CleanupSyncedFrom(".\\eng_data\\.sync_manifest",
                      ".\\eng_data\\.original_cache");
  }

  // Patch Japanese strings in the EXE image with Indonesian translations
  PatchStringsInMemory();

  // Set Japanese codepage (Shift-JIS) as early as possible.
  // This fixes encoding on Wine/Proton where GetACP() returns the wrong
  // value. LCID/LangID hooks are intentionally NOT included - they crash the
  // backlog.
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(&(PVOID &)Real_GetACP, Hook_GetACP);
  DetourAttach(&(PVOID &)Real_GetOEMCP, Hook_GetOEMCP);
  DetourTransactionCommit();

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  HMODULE hExe = GetModuleHandleA(NULL);
  if (hExe) {
    BYTE *systemText = (BYTE *)hExe + 0x11710;
    BYTE *systemGlyph = (BYTE *)hExe + 0x11220;
    BYTE *systemLabel = (BYTE *)hExe + 0x24240;
    const BYTE labelSignature[] = {0x8B, 0x44, 0x24, 0x08, 0x55};
    const BYTE textSignature[] = {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x1C};
    const BYTE glyphSignature[] = {0x83, 0xEC, 0x0C, 0xA1};
    if (memcmp(systemText, textSignature, sizeof(textSignature)) == 0 &&
        memcmp(systemGlyph, glyphSignature, sizeof(glyphSignature)) == 0 &&
        memcmp(systemLabel, labelSignature, sizeof(labelSignature)) == 0) {
      Real_SystemText_11710 =
          (void(__cdecl *)(int, int, const char *, int, int))systemText;
      DrawSystemGlyph_11220 =
          (void(__cdecl *)(int, int, const BYTE *, int))systemGlyph;
      Real_SystemLabel_24240 =
          (void(__cdecl *)(int, int, int, const char *, int))systemLabel;
      DetourAttach(&(PVOID &)Real_SystemText_11710, Hook_SystemText_11710);
      DetourAttach(&(PVOID &)Real_SystemLabel_24240, Hook_SystemLabel_24240);
      BYTE *choiceLayer = (BYTE *)hExe + 0x1C2C0;
      BYTE *choiceClip = (BYTE *)hExe + 0xDE00;
      const BYTE choiceSignature[] = {0x8B, 0x44, 0x24, 0x08, 0x81,
                                      0xEC, 0x00, 0x01, 0x00, 0x00};
      // The absolute operand also verifies the clip globals for this build.
      const BYTE clipSignature[] = {0xA1, 0x94, 0x86, 0xCC, 0x00, 0x56, 0x57};
      if (memcmp(choiceLayer, choiceSignature, sizeof(choiceSignature)) == 0 &&
          memcmp(choiceClip, clipSignature, sizeof(clipSignature)) == 0) {
        g_storyChoiceExe = (BYTE *)hExe;
        SetStoryChoiceClip_DE00 =
            (void(__cdecl *)(int, int, int, int))choiceClip;
        Real_StoryChoiceLayer_1C2C0 =
            (char(__cdecl *)(const int *, const char *))choiceLayer;
        DetourAttach(&(PVOID &)Real_StoryChoiceLayer_1C2C0,
                     Hook_StoryChoiceLayer_1C2C0);
      }
    }
    Real_Hsxf4_22BB0 =
        (DWORD(__cdecl *)(DWORD, DWORD, DWORD, DWORD))((BYTE *)hExe + 0x22BB0);
    BYTE *hsxf4 = (BYTE *)Real_Hsxf4_22BB0;
    if (hsxf4[0] == 0x53 && hsxf4[1] == 0x8B && hsxf4[2] == 0x5C &&
        hsxf4[3] == 0x24 && hsxf4[4] == 0x08) {
      DetourAttach(&(PVOID &)Real_Hsxf4_22BB0, Hook_Hsxf4_22BB0);
    }
  }
  // GetGlyphOutline is only customized while HSXF4 is drawing the temporary
  // half-width Latin glyph. All other in-game dialogue calls pass through.
  DetourAttach(&(PVOID &)Real_GetGlyphOutlineA, Hook_GetGlyphOutlineA);
  DetourAttach(&(PVOID &)Real_GetGlyphOutlineW, Hook_GetGlyphOutlineW);
  DetourAttach(&(PVOID &)Real_CreateFileA, Hook_CreateFileA);
  DetourAttach(&(PVOID &)Real_CreateFileW, Hook_CreateFileW);
  DetourAttach(&(PVOID &)Real_CreateWindowExA, Hook_CreateWindowExA);
  DetourAttach(&(PVOID &)Real_CreateWindowExW, Hook_CreateWindowExW);
  DetourAttach(&(PVOID &)Real_GetTextExtentPoint32A,
               Hook_GetTextExtentPoint32A);
  DetourAttach(&(PVOID &)Real_GetTextExtentPoint32W,
               Hook_GetTextExtentPoint32W);
  DetourAttach(&(PVOID &)Real_GetTextExtentExPointA,
               Hook_GetTextExtentExPointA);
  DetourAttach(&(PVOID &)Real_GetTextExtentExPointW,
               Hook_GetTextExtentExPointW);
  DetourAttach(&(PVOID &)Real_GetCharWidth32A, Hook_GetCharWidth32A);
  DetourAttach(&(PVOID &)Real_GetCharWidth32W, Hook_GetCharWidth32W);
  DetourAttach(&(PVOID &)Real_GetCharABCWidthsA, Hook_GetCharABCWidthsA);
  DetourAttach(&(PVOID &)Real_GetCharABCWidthsW, Hook_GetCharABCWidthsW);
  // DVD/KEY verification bypass — spoof install drive as CD-ROM + suppress dialog
  DetourAttach(&(PVOID &)Real_MessageBoxA, Hook_MessageBoxA);
  DetourAttach(&(PVOID &)Real_MessageBoxW, Hook_MessageBoxW);
  DetourAttach(&(PVOID &)Real_DialogBoxParamA, Hook_DialogBoxParamA);
  DetourAttach(&(PVOID &)Real_DialogBoxParamW, Hook_DialogBoxParamW);
  DetourAttach(&(PVOID &)Real_RegOpenKeyExA, Hook_RegOpenKeyExA);
  DetourAttach(&(PVOID &)Real_RegOpenKeyExW, Hook_RegOpenKeyExW);
  DetourAttach(&(PVOID &)Real_RegQueryValueExA, Hook_RegQueryValueExA);
  DetourAttach(&(PVOID &)Real_RegQueryValueExW, Hook_RegQueryValueExW);
  DetourAttach(&(PVOID &)Real_GetDriveTypeA, Hook_GetDriveTypeA);
  DetourAttach(&(PVOID &)Real_GetDriveTypeW, Hook_GetDriveTypeW);
  DetourAttach(&(PVOID &)Real_GetLogicalDrives, Hook_GetLogicalDrives);
  // Menu translation hooks are disabled for stability: they can corrupt
  // system-menu rendering on some Windows DPI/codepage combinations.
  // Backlog line spacing and text position
  DetourAttach(&(PVOID &)Real_GetTextMetricsA, Hook_GetTextMetricsA);
  DetourAttach(&(PVOID &)Real_GetTextMetricsW, Hook_GetTextMetricsW);
  DetourAttach(&(PVOID &)Real_ExtTextOutA, Hook_ExtTextOutA);
  DetourAttach(&(PVOID &)Real_ExtTextOutW, Hook_ExtTextOutW);
  DetourAttach(&(PVOID &)Real_TextOutA, Hook_TextOutA);
  DetourAttach(&(PVOID &)Real_TextOutW, Hook_TextOutW);
  DetourAttach(&(PVOID &)Real_DrawTextA, Hook_DrawTextA);
  DetourAttach(&(PVOID &)Real_DrawTextW, Hook_DrawTextW);
  DetourAttach(&(PVOID &)Real_DrawTextExA, Hook_DrawTextExA);
  DetourAttach(&(PVOID &)Real_DrawTextExW, Hook_DrawTextExW);
  DetourAttach(&(PVOID &)Real_SetTextCharacterExtra, Hook_SetTextCharacterExtra);
  DetourAttach(&(PVOID &)Real_SetTextJustification, Hook_SetTextJustification);

  DetourTransactionCommit();
  LogPatchLoad("InitPatch.end");
  DiagLogProcessState("InitPatch.end");
}

static void CleanupPatch() {
  // Detach codepage hooks
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourDetach(&(PVOID &)Real_GetACP, Hook_GetACP);
  DetourDetach(&(PVOID &)Real_GetOEMCP, Hook_GetOEMCP);
  DetourTransactionCommit();

  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  if (Real_Hsxf4_22BB0)
    DetourDetach(&(PVOID &)Real_Hsxf4_22BB0, Hook_Hsxf4_22BB0);
  if (Real_SystemLabel_24240)
    DetourDetach(&(PVOID &)Real_SystemLabel_24240, Hook_SystemLabel_24240);
  if (Real_StoryChoiceLayer_1C2C0)
    DetourDetach(&(PVOID &)Real_StoryChoiceLayer_1C2C0,
                 Hook_StoryChoiceLayer_1C2C0);
  if (Real_SystemText_11710)
    DetourDetach(&(PVOID &)Real_SystemText_11710, Hook_SystemText_11710);
  DetourDetach(&(PVOID &)Real_GetGlyphOutlineA, Hook_GetGlyphOutlineA);
  DetourDetach(&(PVOID &)Real_GetGlyphOutlineW, Hook_GetGlyphOutlineW);
  DetourDetach(&(PVOID &)Real_CreateFileA, Hook_CreateFileA);
  DetourDetach(&(PVOID &)Real_CreateFileW, Hook_CreateFileW);
  DetourDetach(&(PVOID &)Real_CreateWindowExA, Hook_CreateWindowExA);
  DetourDetach(&(PVOID &)Real_CreateWindowExW, Hook_CreateWindowExW);
  DetourDetach(&(PVOID &)Real_GetTextExtentPoint32A,
               Hook_GetTextExtentPoint32A);
  DetourDetach(&(PVOID &)Real_GetTextExtentPoint32W,
               Hook_GetTextExtentPoint32W);
  DetourDetach(&(PVOID &)Real_GetTextExtentExPointA,
               Hook_GetTextExtentExPointA);
  DetourDetach(&(PVOID &)Real_GetTextExtentExPointW,
               Hook_GetTextExtentExPointW);
  DetourDetach(&(PVOID &)Real_GetCharWidth32A, Hook_GetCharWidth32A);
  DetourDetach(&(PVOID &)Real_GetCharWidth32W, Hook_GetCharWidth32W);
  DetourDetach(&(PVOID &)Real_GetCharABCWidthsA, Hook_GetCharABCWidthsA);
  DetourDetach(&(PVOID &)Real_GetCharABCWidthsW, Hook_GetCharABCWidthsW);
  DetourDetach(&(PVOID &)Real_MessageBoxA, Hook_MessageBoxA);
  DetourDetach(&(PVOID &)Real_MessageBoxW, Hook_MessageBoxW);
  DetourDetach(&(PVOID &)Real_DialogBoxParamA, Hook_DialogBoxParamA);
  DetourDetach(&(PVOID &)Real_DialogBoxParamW, Hook_DialogBoxParamW);
  DetourDetach(&(PVOID &)Real_RegOpenKeyExA, Hook_RegOpenKeyExA);
  DetourDetach(&(PVOID &)Real_RegOpenKeyExW, Hook_RegOpenKeyExW);
  DetourDetach(&(PVOID &)Real_RegQueryValueExA, Hook_RegQueryValueExA);
  DetourDetach(&(PVOID &)Real_RegQueryValueExW, Hook_RegQueryValueExW);
  DetourDetach(&(PVOID &)Real_GetDriveTypeA, Hook_GetDriveTypeA);
  DetourDetach(&(PVOID &)Real_GetDriveTypeW, Hook_GetDriveTypeW);
  DetourDetach(&(PVOID &)Real_GetLogicalDrives, Hook_GetLogicalDrives);
  DetourDetach(&(PVOID &)Real_GetTextMetricsA, Hook_GetTextMetricsA);
  DetourDetach(&(PVOID &)Real_GetTextMetricsW, Hook_GetTextMetricsW);
  DetourDetach(&(PVOID &)Real_ExtTextOutA, Hook_ExtTextOutA);
  DetourDetach(&(PVOID &)Real_ExtTextOutW, Hook_ExtTextOutW);
  DetourDetach(&(PVOID &)Real_TextOutA, Hook_TextOutA);
  DetourDetach(&(PVOID &)Real_TextOutW, Hook_TextOutW);
  DetourDetach(&(PVOID &)Real_DrawTextA, Hook_DrawTextA);
  DetourDetach(&(PVOID &)Real_DrawTextW, Hook_DrawTextW);
  DetourDetach(&(PVOID &)Real_DrawTextExA, Hook_DrawTextExA);
  DetourDetach(&(PVOID &)Real_DrawTextExW, Hook_DrawTextExW);
  DetourDetach(&(PVOID &)Real_SetTextCharacterExtra, Hook_SetTextCharacterExtra);
  DetourDetach(&(PVOID &)Real_SetTextJustification, Hook_SetTextJustification);

  DetourTransactionCommit();
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
    LogPatchLoad("DllMain.attach");
    DisableThreadLibraryCalls(hModule);
    InitPatch();
  } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
    LogPatchLoad("DllMain.detach");
    CleanupPatch();
  }
  return TRUE;
}

