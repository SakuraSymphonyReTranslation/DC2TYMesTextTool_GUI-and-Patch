# DC2TY MES Text Tool — GUI and CLI

[Bahasa Indonesia](README.md) | **English**

Scenario script importer/exporter and runtime patch for **D.C.II To You ～Da Capo II～ Side Episodes** (Da Capo 2 To You).

This distribution specifically supports **Da Capo 2 To You** (`dc2ty`, MES version `0x5F69`). The GUI, CLI, C++ reference tables, patch, and launcher are tailored specifically for this game. The primary patch source code is located in `DC2TYPatch a/src/dc2typatch.cpp`.

## Downloads and Installation Folders

Download both latest archives from [GitHub Releases](https://github.com/SakuraSymphonyReTranslation/DC2TYMesTextTool_GUI-and-Patch/releases/latest):

Release **v1.0.0** was refreshed on **17 October 2026** with compact choice layout and verified conversion of `○` labels.

- **DC2TYMesTextTool_GUI.rar**: MES ↔ JSON importer/exporter (GUI & CLI), including Indonesian and English documentation.
- **Released_patch.rar**: Custom launcher (`DC2TYLauncher.exe`), patch DLL (`DC2TYPatch.dll`), INI configuration (`DC2TYPatch.ini`), reference executable (`DC2TY.exe`), and Locale Emulator libraries.

`<GAME_DIR>` refers to the **main game installation folder of your choice**, where `DC2TY.exe` and `AdvData` reside. This is a documentation placeholder: replace it with your local path (such as `C:\Games\DC2TY`), without brackets. No specific drive or folder is enforced.

### Patch Installation Steps

1. Close the game and back up original files to be replaced, especially executables and INI files if present.
2. Extract the contents of `Released_patch.rar` directly into `<GAME_DIR>`, so that `DC2TYLauncher.exe`, `DC2TYPatch.dll`, and `DC2TYPatch.ini` are side-by-side with `DC2TY.exe`.
3. Launch **`<GAME_DIR>\DC2TYLauncher.exe`** with working directory set to `<GAME_DIR>`. If creating a desktop shortcut, ensure **Start in** points to `<GAME_DIR>`. The launcher initializes Japanese locale (CP932) via Locale Emulator and injects `DC2TYPatch.dll` into the game automatically.
4. Place translated Indonesian MES script files in `<GAME_DIR>\id_Data\MES`. For English translations, place them in `<GAME_DIR>\eng_data\MES`. Ensure the `Language` setting in `DC2TYPatch.ini` matches (`1` for Indonesian, `2` for English).

```text
<GAME_DIR>/
├── DC2TY.exe
├── DC2TYLauncher.exe
├── DC2TYPatch.dll
├── DC2TYPatch.ini
├── LoaderDll.dll
├── LocaleEmulator.dll
├── AdvData/
├── id_Data/
│   └── MES/
└── eng_data/
    └── MES/
```

Both RAR packages supply tools and runtime files; they do not include the full translated game script. The source repository can live anywhere. Ready-to-use binaries are also available inside the repository under `DC2TYPatch a/build/Released`.

## Running the Application

Extract `DC2TYMesTextTool_GUI.rar` to your preferred folder and run `DC2TYMesTextTool_GUI.exe`. If running from source, execute `bin\Release\net8.0-windows\DC2TYMesTextTool_GUI.exe`.
Requires [.NET 8 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/8.0).

## Single File

### MES → JSON (Exporter)

1. Set **Input MES/JSON** to the original `.mes` file.
2. Set **Output JSON/MES**, or leave blank to automatically place the `.json` next to the input file.
3. Click **Export (MES -> JSON)**.

The default output pairs character names with dialogue entries according to DC2TY opcode rules:

```json
[
  { "message": "Story narration." },
  { "name": "Yoshiyuki", "message": "「Character dialogue.」" }
]
```

Exports from both Single File and Batch tabs use this combined name/dialogue format. Japanese and translated texts, speaker tags, and line breaks are faithfully preserved.

### JSON → MES (Importer)

1. Set **Input MES/JSON** to the translated `.json` file.
2. Ensure the matching original `.mes` template is in the same folder (or select the original MES file).
3. Choose an **Output JSON/MES** destination distinct from the original MES file.
4. Adjust **Word Wrap Width**: defaults to **70** across Single File, Batch, and CLI modes. Tweak as needed if in-game text wraps too early or overflows.
5. Click **Import (JSON -> MES)**.

The importer handles combined JSON, legacy formats, and mixed styles. The original MES is used as a template to rebuild the script and update label offsets. Preserve the JSON entry order and write the imported result to a separate file.

### Choice Labels `○`

Write each choice label in the `message` field, using one ordinary space after `○` and between words:

```json
[
  { "message": "○ Panggil Bos" },
  { "message": "○ Panggil Pangeran" },
  { "message": "○ Panggil Shinemon-san" }
]
```

This example shows only the choice entries; import the complete exported JSON with its entry order preserved. The `○` symbol is retained during MES → JSON and JSON → MES conversion through both GUI and CLI, including Batch mode, which uses the same conversion engine.

Choice spaces are stored as U+3000 in MES so encryption does not turn them into NUL terminators. The exporter restores ordinary spaces in JSON. There is no need to replace the circle, add double spaces, or edit MES bytes manually.

The updated runtime patch draws story choices beginning with `○` in translation mode with a **12 px** left margin, proportional Arial text, and single visual word spacing. Long labels shrink to fit the existing bar. The rule uses the button type and label prefix, with no phrase whitelist. Labels containing `@` formatting commands retain the original renderer, as does Japanese mode.

Bar dimensions and click areas are preserved. The choice update leaves notification popup size, textbox size, and `DC2TYPatch.ini` settings unchanged.

## Batch

- **Batch Export:** Provide **MES Folder** and **Output Folder**, then click **Batch Export (MES -> JSON)**. All `.mes` files in the folder are exported to `.json`.
- **Batch Import:** Provide **JSON Folder**, **Original MES Folder** (clean templates), and **Output Folder** (destination for patched scripts), then click **Batch Import (JSON -> MES)**.
- Matching by name is automatic: e.g. `scene01.json` pairs with `scene01.mes`.
- Failures are logged in the output window while batch processing continues for remaining files.

## Layout Configuration & Runtime Patch

In-game text rendering is automatically adjusted at runtime by `DC2TYPatch.dll`. Configuration is controlled via `DC2TYPatch.ini`:

```ini
[Fonts]
BacklogFont=MS PGothic
BacklogSize=-15
DialogueFont=MS PGothic
DialogueSize=-15
BacklogNameFont=MS PGothic
BacklogNameSize=-11
BacklogXOffset=0
BacklogLineSpacing=0
BacklogYOffset=12
BacklogNameXOffset=13
BacklogNameYOffset=0
BacklogNameSpacing=0
BacklogDialogSpacing=0
DialogueRenderScale=74
DialogueAdvanceScale=62
DialogueSpaceScale=24
AdvancedSettings=1

[Settings]
ShowBacklogIcon=1
EnableFileRedirection=1
Language=1
DisableBacklogFont=0
DisableBacklogSpacing=0
DisableBacklogTranslation=0
```

- **Font & Render Scaling**: `DialogueRenderScale=74`, `DialogueAdvanceScale=62`, and `DialogueSpaceScale=24` balance Latin character proportions to prevent clipping or excessive gap.
- **Redirection**: `EnableFileRedirection=1` redirects game file reads to `id_Data\MES` or `eng_data\MES` without overwriting original assets.
- **Backlog Icons**: `ShowBacklogIcon=1` adjusts speaker icon rendering inside the backlog window.
- **Story Choices**: Spacing and margins for `○` labels are adjusted automatically in translation mode, with no new INI options. Implementation details are available in [Choice Layout](https://github.com/SakuraSymphonyReTranslation/DC2TYMesTextTool_GUI-and-Patch/blob/master/DC2TYPatch%20a/CHOICE_LAYOUT.md) (Indonesian).

Technical details and C++ build steps can be found in the [Patch README](https://github.com/SakuraSymphonyReTranslation/DC2TYMesTextTool_GUI-and-Patch/blob/master/DC2TYPatch%20a/Readme_en.md).

## Validation and Encoding

- Circus engine scenario scripts are encoded in **CP932 (Shift-JIS)**.
- Some Latin characters and punctuation are mapped to CP932 equivalents (e.g. em-dash `— → ―`, accented letters `û → u`, `ë → e`, `é → e`, `à → a`). Check a re-export for other characters outside CP932.
- Conversion results are written directly to the output path. Use a separate output file, keep the original MES backup, and check the log after import.

## CLI

From the executable folder (`bin\Release\net8.0-windows` or the extracted RAR folder):

```powershell
.\DC2TYMesTextTool_GUI.exe export "scene.mes" "scene.json"
.\DC2TYMesTextTool_GUI.exe import "scene.json" "original\scene.mes" "new\scene.mes" -w 70
.\DC2TYMesTextTool_GUI.exe batch-export "original_mes" "json_output"
.\DC2TYMesTextTool_GUI.exe batch-import "json_folder" "original_mes" "mes_output" -w 70
```

- `-w <width>`: Word wrap threshold (default: `70`).
- Supports drag-and-drop: drop a `.mes` file or a directory onto the executable to export automatically.

## Verification Data

- Script import/export routines verified against original DC2TY files.
- An audit of **138 translated JSON files** found **3 choices**, all in `ty_myu02_a1_myu.json`; all three correspond to labels in the original/active MES files.
- Importing Indonesian JSON to MES and exporting it again preserved all three `○` labels and their spaces. The Win32 Release build and choice layout tests passed; all three labels were also checked visually in-game.
- Textbox size and layout stayed the same in the tested scene. Popup functions and the INI file are unchanged; popups were not visually retested during this choice session.
- Tested reference executable SHA-256:

```text
21988CA3227D494AD8FD816FD6363670B1A6245F82AC28314C4BB4E8353D19BD  DC2TY.EXE
```

## Building from Source

From repository root:

```powershell
# Build C# GUI & CLI (.NET 8 SDK required)
dotnet build -c Release
```

To build the C++ patch (Visual Studio 2022 with Desktop development with C++):

```powershell
cmake -S "DC2TYPatch a" -B "DC2TYPatch a/build-local" -G "Visual Studio 17 2022" -A Win32 -DDC2TY_BUILD_CHOICE_TESTS=ON
cmake --build "DC2TYPatch a/build-local" --config Release
ctest --test-dir "DC2TYPatch a/build-local" -C Release --output-on-failure
```

The complete `DC2TYPatch a/build/` directory is included in the repository, covering the `Released` package and generated build artifacts. CMake caches contain paths from the build machine; use a fresh `build-local` directory on another computer. New binaries appear in `build-local/Release`. The legacy `build.bat` script deletes `build` before rebuilding.
