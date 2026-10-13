/*
 * DC2TY Patch Launcher
 *
 * Launches DC2TY.exe with Japanese locale and injects DC2TYPatch.dll.
 *
 * On Wine/Proton: Sets Japanese locale in registry + LC_ALL=ja_JP.UTF-8.
 * On Windows:     Uses Locale Emulator (LoaderDll.dll), with silent fallback.
 *
 * Distribution: Only DC2TYLauncher.exe + DC2TYPatch.dll needed.
 */

#define WIN32_LEAN_AND_MEAN
#include "resource.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <windows.h>

static char g_traceDir[MAX_PATH] = {0};
static void TraceLauncher(const char *fmt, ...) {
  if (g_traceDir[0] == '\0')
    return;
  char logPath[MAX_PATH];
  sprintf_s(logPath, "%s\\launcher_trace.log", g_traceDir);
  FILE *f = fopen(logPath, "a");
  if (!f)
    return;
  SYSTEMTIME st = {};
  GetLocalTime(&st);
  fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
          st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
          st.wMilliseconds);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fprintf(f, "\n");
  fclose(f);
}


// ============================================================================
// Wine/Proton detection
// ============================================================================
typedef const char *(CDECL *pfnWineGetVersion)(void);

static bool IsRunningUnderWine() {
  HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
  if (!hNtdll)
    return false;
  pfnWineGetVersion pWineGetVersion =
      (pfnWineGetVersion)GetProcAddress(hNtdll, "wine_get_version");
  return (pWineGetVersion != NULL);
}

// ============================================================================
// Locale Emulator structures (from LEProc/LoaderWrapper.cs)
// ============================================================================

#pragma pack(push, 1)

struct TIME_FIELDS {
  USHORT Year;
  USHORT Month;
  USHORT Day;
  USHORT Hour;
  USHORT Minute;
  USHORT Second;
  USHORT Milliseconds;
  USHORT Weekday;
};

struct RTL_TIME_ZONE_INFORMATION {
  LONG Bias;
  WCHAR StandardName[32];
  TIME_FIELDS StandardDate;
  LONG StandardBias;
  WCHAR DaylightName[32];
  TIME_FIELDS DaylightDate;
  LONG DaylightBias;
};

struct LEB {
  DWORD AnsiCodePage;
  DWORD OemCodePage;
  DWORD LocaleID;
  DWORD DefaultCharset;
  DWORD HookUILanguageAPI;
  BYTE DefaultFaceName[64];
  RTL_TIME_ZONE_INFORMATION Timezone;
};

#pragma pack(pop)

typedef DWORD(WINAPI *pfnLeCreateProcess)(
    void *leb, LPCWSTR applicationName, LPCWSTR commandLine,
    LPCWSTR currentDirectory, DWORD creationFlags, LPSTARTUPINFOW startupInfo,
    LPPROCESS_INFORMATION processInformation, LPVOID processAttributes,
    LPVOID threadAttributes, LPVOID environment, LPVOID token);

// ============================================================================
// Extract embedded resource to file
// ============================================================================
static bool ExtractResource(HMODULE hModule, LPCSTR resourceName,
                            const char *outputPath) {
  HRSRC hRes = FindResourceA(hModule, resourceName, RT_RCDATA);
  if (!hRes)
    return false;
  HGLOBAL hData = LoadResource(hModule, hRes);
  if (!hData)
    return false;
  DWORD size = SizeofResource(hModule, hRes);
  void *data = LockResource(hData);
  if (!data || size == 0)
    return false;
  HANDLE hFile = CreateFileA(outputPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
  if (hFile == INVALID_HANDLE_VALUE)
    return false;
  DWORD written;
  BOOL ok = WriteFile(hFile, data, size, &written, NULL);
  CloseHandle(hFile);
  return ok && (written == size);
}

// ============================================================================
// Inject DLL into a running process
// ============================================================================
static bool InjectDLL(HANDLE hProcess, const char *dllPath) {
  SIZE_T pathLen = strlen(dllPath) + 1;
  LPVOID remotePath = VirtualAllocEx(hProcess, NULL, pathLen,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!remotePath) {
    MessageBoxA(NULL, "Failed to allocate memory in target process",
                "Injection Error", MB_ICONERROR);
    return false;
  }
  if (!WriteProcessMemory(hProcess, remotePath, dllPath, pathLen, NULL)) {
    MessageBoxA(NULL, "Failed to write to target process memory",
                "Injection Error", MB_ICONERROR);
    VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
    return false;
  }
  HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
  LPVOID pLoadLibraryA = (LPVOID)GetProcAddress(hKernel32, "LoadLibraryA");
  if (!pLoadLibraryA) {
    MessageBoxA(NULL, "Failed to get LoadLibraryA address", "Injection Error",
                MB_ICONERROR);
    VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
    return false;
  }
  HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0,
                                      (LPTHREAD_START_ROUTINE)pLoadLibraryA,
                                      remotePath, 0, NULL);
  if (!hThread) {
    char msg[256];
    sprintf_s(msg, "Failed to create remote thread. Error: %lu",
              GetLastError());
    MessageBoxA(NULL, msg, "Injection Error", MB_ICONERROR);
    VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
    return false;
  }
  WaitForSingleObject(hThread, 5000);
  CloseHandle(hThread);
  VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);
  return true;
}

// ============================================================================
// Set Japanese locale in registry (Wine/Proton path)
// Wine reads HKCU\Control Panel\International to determine ANSI codepage.
// Setting these before launch makes DC2TY.exe see Japanese locale (CP932).
// ============================================================================
static void SetJapaneseLocaleRegistry() {
  HKEY hKey;
  if (RegCreateKeyExA(HKEY_CURRENT_USER, "Control Panel\\International", 0,
                      NULL, 0, KEY_SET_VALUE, NULL, &hKey,
                      NULL) == ERROR_SUCCESS) {
    RegSetValueExA(hKey, "ACP", 0, REG_SZ, (const BYTE *)"932", 4);
    RegSetValueExA(hKey, "OEMCP", 0, REG_SZ, (const BYTE *)"932", 4);
    RegSetValueExA(hKey, "MACCP", 0, REG_SZ, (const BYTE *)"10001", 6);
    RegSetValueExA(hKey, "Locale", 0, REG_SZ, (const BYTE *)"00000411", 9);
    RegSetValueExA(hKey, "LocaleName", 0, REG_SZ, (const BYTE *)"ja-JP", 6);
    RegCloseKey(hKey);
  }
  if (RegCreateKeyExA(HKEY_CURRENT_USER, "Control Panel\\Nls\\CodePage", 0,
                      NULL, 0, KEY_SET_VALUE, NULL, &hKey,
                      NULL) == ERROR_SUCCESS) {
    RegSetValueExA(hKey, "ACP", 0, REG_SZ, (const BYTE *)"932", 4);
    RegSetValueExA(hKey, "OEMCP", 0, REG_SZ, (const BYTE *)"932", 4);
    RegCloseKey(hKey);
  }
}

// ============================================================================
// Create process with Japanese locale via environment (Wine/Proton path)
// ============================================================================
static bool CreateProcessWithLC_ALL(const char *exePath, const char *currentDir,
                                    DWORD extraFlags, PROCESS_INFORMATION *pi) {
  // Write Japanese locale to Wine registry before spawning the process
  SetJapaneseLocaleRegistry();

  LPCH envBlock = GetEnvironmentStringsA();
  if (!envBlock)
    return false;

  SIZE_T envSize = 0;
  {
    const char *p = envBlock;
    while (*p) {
      SIZE_T len = strlen(p) + 1;
      envSize += len;
      p += len;
    }
    envSize++;
  }

  const char *lcAll = "LC_ALL=ja_JP.UTF-8";
  const char *lang = "LANG=ja_JP.UTF-8";
  SIZE_T lcAllLen = strlen(lcAll) + 1;
  SIZE_T langLen = strlen(lang) + 1;

  char *newEnv = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   envSize + lcAllLen + langLen + 64);
  if (!newEnv) {
    FreeEnvironmentStringsA(envBlock);
    return false;
  }

  char *dst = newEnv;
  const char *src = envBlock;
  while (*src) {
    SIZE_T varLen = strlen(src) + 1;
    if (_strnicmp(src, "LC_ALL=", 7) != 0 && _strnicmp(src, "LANG=", 5) != 0) {
      memcpy(dst, src, varLen);
      dst += varLen;
    }
    src += varLen;
  }
  memcpy(dst, lcAll, lcAllLen);
  dst += lcAllLen;
  memcpy(dst, lang, langLen);
  dst += langLen;
  *dst = '\0';

  FreeEnvironmentStringsA(envBlock);

  STARTUPINFOA siA = {sizeof(siA)};
  *pi = {};
  BOOL ok = CreateProcessA(exePath, NULL, NULL, NULL, FALSE,
                           CREATE_SUSPENDED | extraFlags, newEnv, currentDir,
                           &siA, pi);

  HeapFree(GetProcessHeap(), 0, newEnv);
  return ok ? true : false;
}

// ============================================================================
// Launch with Locale Emulator (Windows path)
// ============================================================================
static bool LaunchWithLocaleEmulator(HINSTANCE hInstance, const char *exePath,
                                     const char *currentDir,
                                     PROCESS_INFORMATION *pi) {
  char loaderDllPath[MAX_PATH];
  char localeEmuPath[MAX_PATH];
  sprintf_s(loaderDllPath, "%s\\LoaderDll.dll", currentDir);
  sprintf_s(localeEmuPath, "%s\\LocaleEmulator.dll", currentDir);

  if (GetFileAttributesA(loaderDllPath) == INVALID_FILE_ATTRIBUTES)
    if (!ExtractResource(hInstance, MAKEINTRESOURCEA(IDR_LOADERDLL),
                         loaderDllPath))
      return false;

  if (GetFileAttributesA(localeEmuPath) == INVALID_FILE_ATTRIBUTES)
    if (!ExtractResource(hInstance, MAKEINTRESOURCEA(IDR_LOCALEEMU),
                         localeEmuPath))
      return false;

  HMODULE hLoaderDll = LoadLibraryA(loaderDllPath);
  if (!hLoaderDll)
    return false;

  pfnLeCreateProcess pLeCreateProcess =
      (pfnLeCreateProcess)GetProcAddress(hLoaderDll, "LeCreateProcess");
  if (!pLeCreateProcess) {
    FreeLibrary(hLoaderDll);
    return false;
  }

  LEB leb = {};
  leb.AnsiCodePage = 932;
  leb.OemCodePage = 932;
  leb.LocaleID = 0x0411;
  leb.DefaultCharset = 128; // SHIFTJIS_CHARSET
  leb.HookUILanguageAPI = 0;
  memset(&leb.Timezone, 0, sizeof(leb.Timezone));
  leb.Timezone.Bias = -540;
  wcscpy_s(leb.Timezone.StandardName, 32, L"Tokyo Standard Time");
  wcscpy_s(leb.Timezone.DaylightName, 32, L"Tokyo Standard Time");

  wchar_t wExePath[MAX_PATH], wCurrentDir[MAX_PATH], wCommandLine[MAX_PATH * 2];
  MultiByteToWideChar(CP_ACP, 0, exePath, -1, wExePath, MAX_PATH);
  MultiByteToWideChar(CP_ACP, 0, currentDir, -1, wCurrentDir, MAX_PATH);
  swprintf_s(wCommandLine, L"\"%s\"", wExePath);

  STARTUPINFOW si = {sizeof(si)};
  *pi = {};
  BYTE lebBuffer[sizeof(LEB) + 4] = {};
  memcpy(lebBuffer, &leb, sizeof(LEB));

  DWORD ret = pLeCreateProcess(lebBuffer, wExePath, wCommandLine, wCurrentDir,
                               0x00000004, &si, pi, NULL, NULL, NULL, NULL);

  FreeLibrary(hLoaderDll);
  return (ret == 0);
}

// ============================================================================
// Main
// ============================================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
  char currentDir[MAX_PATH] = {0};
  char launcherPath[MAX_PATH] = {0};
  GetModuleFileNameA(NULL, launcherPath, MAX_PATH);
  lstrcpynA(currentDir, launcherPath, MAX_PATH);
  for (int i = lstrlenA(currentDir) - 1; i >= 0; --i) {
    if (currentDir[i] == '\\' || currentDir[i] == '/') {
      currentDir[i] = '\0';
      break;
    }
  }
  lstrcpynA(g_traceDir, currentDir, MAX_PATH);
  TraceLauncher("launcher start; currentDir=%s", currentDir);

  char exePath[MAX_PATH];
  char dllPath[MAX_PATH];
  sprintf_s(exePath, "%s\\DC2TY.exe", currentDir);
  sprintf_s(dllPath, "%s\\DC2TYPatch.dll", currentDir);
  TraceLauncher("resolved exe=%s", exePath);
  TraceLauncher("resolved dll=%s", dllPath);

  if (GetFileAttributesA(exePath) == INVALID_FILE_ATTRIBUTES) {
    MessageBoxA(NULL,
                "DC2TY.exe not found!\nPlease place this launcher in the DC2TY "
                "game folder.",
                "DC2TY Patch Launcher", MB_ICONERROR);
    return 1;
  }
  if (GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
    MessageBoxA(NULL,
                "DC2TYPatch.dll not found!\nPlease place DC2TYPatch.dll in the "
                "game folder.",
                "DC2TY Patch Launcher", MB_ICONERROR);
    return 1;
  }

  PROCESS_INFORMATION pi = {};
  bool isWine = IsRunningUnderWine();
  TraceLauncher("isWine=%d", isWine ? 1 : 0);

  if (isWine) {
    // Wine/Proton: LE kernel hooks unavailable — use registry + LC_ALL
    if (!CreateProcessWithLC_ALL(exePath, currentDir, 0, &pi)) {
      char msg[256];
      sprintf_s(msg, "Failed to start game under Wine. Error: %lu",
                GetLastError());
      MessageBoxA(NULL, msg, "Patch Launcher", MB_ICONERROR);
      return 1;
    }
  } else {
    // Windows: try Locale Emulator, then silent plain fallback
    if (!LaunchWithLocaleEmulator(hInstance, exePath, currentDir, &pi)) {
      STARTUPINFOA siA = {sizeof(siA)};
      pi = {};
      if (!CreateProcessA(exePath, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED,
                          NULL, currentDir, &siA, &pi)) {
        char msg[256];
        sprintf_s(msg, "Failed to start game. Error: %lu", GetLastError());
        MessageBoxA(NULL, msg, "Patch Launcher", MB_ICONERROR);
        return 1;
      }
      TraceLauncher("CreateProcessA fallback ok; pid=%lu", (unsigned long)pi.dwProcessId);
    }
  }

  bool injected = InjectDLL(pi.hProcess, dllPath);
  TraceLauncher("InjectDLL result=%d pid=%lu", injected ? 1 : 0,
                (unsigned long)pi.dwProcessId);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  if (!injected) {
    MessageBoxA(NULL,
                "DLL injection failed, but game is running without the patch.",
                "Patch Launcher", MB_ICONWARNING);
    return 1;
  }
  return 0;
}
