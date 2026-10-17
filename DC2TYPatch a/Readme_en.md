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
│   ├── choice_layout.h  # Choice label normalization and bitmaps
│   ├── launcher.cpp      # DC2TYLauncher launcher code (DLL injection + Locale Emulator)
│   ├── launcher.rc       # Launcher resources (icon & metadata)
│   ├── d3dx9_proxy.cpp   # Optional D3DX9 proxy
│   └── dsound_proxy.cpp  # Optional DSound proxy
├── Detours/              # Microsoft Detours library
├── Locale-Emulator-2.5.0.1/ # Locale Emulator library
├── tests/                # Layout tests and JSON/MES choice audit
└── build/
    ├── choice-layout/    # Win32 Release build and choice tests
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

- **Story Choices `○`**:
  The **v1.0.0 refresh, 17 October 2026** uses a 12 px left margin, proportional Arial text, and single visual spacing after the circle and between words. Long labels shrink to fit the bar. The rule uses story buttons and the `○` prefix, with no phrase whitelist. Bar dimensions, click areas, colors, and shadows continue to follow the game.
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

### MES ↔ JSON Support

The GUI/CLI tool preserves `○` during export and import, including Batch mode. Use one ordinary space in JSON, for example `{ "message": "○ Panggil Bos" }`. The importer stores choice spaces as U+3000 for MES encryption; the exporter restores ordinary spaces, while the patch adjusts their visual width at rendering time.

The choice renderer is active in translation mode (`Language=1` or `2`). Japanese mode and labels containing `@` formatting commands use the original renderer. There are no new INI settings or changes to notification popup or textbox size. Implementation and audit details are available in [Choice Layout](CHOICE_LAYOUT.md) (Indonesian).

### Verification

- An audit of 138 translated JSON files found 3 choices in `ty_myu02_a1_myu.json`, corresponding to the original and active MES files.
- Indonesian JSON → MES → JSON conversion preserved all three labels and their spaces.
- Win32 Release layout tests passed. All three choices were checked visually in-game and fit the bars with aligned circles.
- The textbox stayed the same in the tested scene. Popup functions and the INI file are unchanged; popups were not visually retested in this session.

## Building (Visual Studio 2022, Win32)

Install **Desktop development with C++**, then run from the `DC2TYPatch a` directory:

```powershell
cmake -S . -B build-local -G "Visual Studio 17 2022" -A Win32 -DDC2TY_BUILD_CHOICE_TESTS=ON
cmake --build build-local --config Release
ctest --test-dir build-local -C Release --output-on-failure
```

New binaries appear in `build-local/Release`. The `DC2TY_BUILD_CHOICE_TESTS` option adds standalone layout checks and can be disabled for a regular build.

The entire `build/` directory is included in the repository, including caches, generated artifacts, logs, and the `build/Released` package. Caches contain paths from the build machine, so a fresh build uses `build-local`. The legacy `build.bat` script deletes `build` before rebuilding.
