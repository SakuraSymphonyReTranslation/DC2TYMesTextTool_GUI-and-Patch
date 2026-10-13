# DC2TY MES Text Tool — GUI and CLI

[Bahasa Indonesia](README.md) | **English**

Scenario script importer/exporter and runtime patch for **D.C.II To You ~Da Capo II~ Side Episodes** (Da Capo 2 To You).

This distribution specifically supports **Da Capo 2 To You** (`dc2ty`, MES version `0x5F69`). The GUI, CLI, C++ reference tables, patch, and launcher are tailored specifically for this game. The primary patch source code is located in `DC2TYPatch a/src/dc2typatch.cpp`.

## Downloads and Installation Folders

Download both latest archives from [GitHub Releases](https://github.com/SakuraSymphonyReTranslation/DC2TYMesTextTool_GUI-and-Patch/releases/latest):

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

The importer handles combined JSON, legacy formats, and mixed styles. String counts and label counts are validated against the original MES to protect narrative execution.

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

Technical details and C++ build steps can be found in the [Patch README](DC2TYPatch%20a/README.md).

## Validation and Encoding

- Circus engine scenario scripts are encoded in **CP932 (Shift-JIS)**.
- Out-of-range characters are automatically mapped to safe approximations (e.g. em-dash `— → ―`, accented letters `û → u`, `ë → e`, `é → e`, `à → a`). Unsupported characters trigger a descriptive error in the log.
- All file writes use temporary files and validation checks before replacement to guard against corrupt partial saves.

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
- Tested reference executable and release archives SHA-256:

```text
46431d412a9ef50e4eb4038d1266a7c02dad7f23873853adc3e6740321605bbc  DC2TYMesTextTool_GUI.rar
c7bd70335f9cabf313085ad3e10d6ed15dafb43910e41804c877fd0a57726aec  Released_patch.rar
21988ca3227d494ad8fd816fd6363670b1a6245f82ac28314c4bb4e8353d19bd  DC2TY.EXE
```

## Building from Source

From repository root:

```powershell
# Build C# GUI & CLI (.NET 8 SDK required)
dotnet build -c Release
```

To build C++ patch (Visual Studio 2022 x86 Native Tools):

```powershell
cd "DC2TYPatch a"
build.bat
```
