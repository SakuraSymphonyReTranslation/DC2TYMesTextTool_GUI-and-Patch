# DC2TY Patch — Installation and Build

[Bahasa Indonesia](README.md) | **English**

Runtime patch and custom launcher dedicated to **D.C.II To You ～Da Capo II～ Side Episodes** (Da Capo II To You). Main implementation sources: patch `src/dc2typatch.cpp` and launcher `src/launcher.cpp`.

## Directory Structure

```text
DC2TYPatch a/
├── CMakeLists.txt        # CMake build configuration
├── build.bat             # Automated build script (NMake / MSVC)
├── icon.ico              # Game icon
├── src/
│   ├── dc2typatch.cpp    # Main hook DLL patch code (injected version)
│   ├── launcher.cpp      # DC2TYLauncher launcher code (DLL injection + Locale Emulator)
│   ├── launcher.rc       # Launcher resources (icon & metadata)
│   ├── d3dx9_proxy.cpp   # Optional D3DX9 proxy
│   └── dsound_proxy.cpp  # Optional DSound proxy
├── Detours/              # Microsoft Detours library
├── Locale-Emulator-2.5.0.1/ # Locale Emulator library
└── build/
    └── Released/         # Ready-to-use binaries (DC2TYLauncher.exe, DC2TYPatch.dll, etc.)
```

## Installation

Download the `Released_patch.rar` package from [GitHub Releases](https://github.com/SakuraSymphonyReTranslation/DC2TYMesTextTool_GUI-and-Patch/releases/latest), or use files in `build/Released`.

`<GAME_DIR>` refers to **your main game installation directory** containing `DC2TY.exe` and `AdvData`. Replace this placeholder with your local path; no particular drive is enforced.

1. Close the game and back up any files to be replaced.
2. Extract the contents of `Released_patch.rar` directly into `<GAME_DIR>`, placing `DC2TYLauncher.exe`, `DC2TYPatch.dll`, and `DC2TYPatch.ini` next to `DC2TY.exe`.
3. Run **`<GAME_DIR>\DC2TYLauncher.exe`** with `<GAME_DIR>` as working directory. If using a shortcut, set **Start in** to `<GAME_DIR>`. The launcher will inject the DLL and initialize Japanese locale automatically.
4. Place Indonesian translation scripts in `<GAME_DIR>\id_Data\MES` or English in `<GAME_DIR>\eng_data\MES`.

Tested reference executable SHA256:

```text
21988CA3227D494AD8FD816FD6363670B1A6245F82AC28314C4BB4E8353D19BD  DC2TY.EXE
```

## Patch Features & Configuration

- **Font Rendering & Advanced Scaling**:
  Configured via `DC2TYPatch.ini`:
  - `DialogueRenderScale=74`
  - `DialogueAdvanceScale=62`
  - `DialogueSpaceScale=24`
  Ensures legible Latin/Indonesian character spacing in dialogue without overlap.
- **Backlog & Popups**:
  `ShowBacklogIcon=1` maintains speaker icons in backlog and centers system confirmation popups.
- **File Redirection**:
  `EnableFileRedirection=1` redirects MES script loading to `id_Data\MES` or `eng_data\MES`.
- **Locale Emulator**:
  Integrated directly into the launcher for native Windows and Wine / Proton on Linux / Steam Deck.

## Building (Visual Studio 2022, x86 Native Tools)

From `DC2TYPatch a` directory:

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release
```

Or run `build.bat`.
Output binaries will be placed in `build/Release` or `build/Released`.
