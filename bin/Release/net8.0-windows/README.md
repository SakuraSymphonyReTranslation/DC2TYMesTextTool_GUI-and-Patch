# DC2TY MES Text Tool — GUI dan CLI

**Bahasa Indonesia** | [English](Readme_en.md)

Importer/exporter naskah skenario dan patch runtime untuk **D.C.II To You ~Da Capo II~ Side Episodes** (Da Capo 2 To You).

Distribusi ini hanya mendukung **Da Capo 2 To You** (`dc2ty`, versi MES `0x5F69`). GUI, CLI, tabel referensi C++, patch, dan launcher dikhususkan untuk game ini. Source utama patch berada di `DC2TYPatch a/src/dc2typatch.cpp`.

## Unduhan dan folder instalasi

Unduh kedua paket terbaru dari [GitHub Releases](https://github.com/SakuraSymphonyReTranslation/DC2TYMesTextTool_GUI-and-Patch/releases/latest):

- **DC2TYMesTextTool_GUI.rar**: Importer/exporter MES ↔ JSON (GUI dan CLI), termasuk dokumentasi bahasa Indonesia dan Inggris.
- **Released_patch.rar**: Launcher kustom (`DC2TYLauncher.exe`), DLL patch (`DC2TYPatch.dll`), konfigurasi INI (`DC2TYPatch.ini`), reference executable (`DC2TY.exe`), dan library Locale Emulator.

`<GAME_DIR>` berarti **folder utama instalasi game pilihan pemain**, yaitu folder yang berisi `DC2TY.exe` dan `AdvData`. Ini placeholder dokumentasi: ganti dengan lokasi instalasi game Anda di komputer (misalnya `C:\Games\DC2TY` atau folder pilihan lainnya), tanpa tanda `<` dan `>`. Tidak ada drive atau lokasi instalasi wajib.

### Langkah Instalasi Patch

1. Tutup game dan buat cadangan file asli yang akan diganti, terutama executable dan file INI jika sebelumnya sudah ada.
2. Ekstrak isi `Released_patch.rar` langsung ke `<GAME_DIR>`, sehingga `DC2TYLauncher.exe`, `DC2TYPatch.dll`, dan `DC2TYPatch.ini` berada satu level di sebelah `DC2TY.exe`.
3. Jalankan **`<GAME_DIR>\DC2TYLauncher.exe`** dengan working directory `<GAME_DIR>`. Jika Anda membuat shortcut di desktop, pastikan kolom **Start in** / **Working directory** mengarah ke `<GAME_DIR>`. Launcher akan menginisialisasi locale Jepang (CP932) via Locale Emulator dan menginjeksi `DC2TYPatch.dll` ke dalam game secara otomatis.
4. Letakkan file skenario MES terjemahan bahasa Indonesia di dalam `<GAME_DIR>\id_Data\MES`. Jika menggunakan terjemahan bahasa Inggris, letakkan di `<GAME_DIR>\eng_data\MES`. Pastikan opsi `Language` pada `DC2TYPatch.ini` sesuai (`1` untuk Indonesia, `2` untuk Inggris).

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

Kedua arsip RAR menyediakan tool dan runtime patch; keduanya tidak berisi seluruh naskah terjemahan game. Repositori source dapat ditempatkan di folder mana saja. Paket patch siap pakai juga tersedia di dalam repositori pada folder `DC2TYPatch a/build/Released`.

## Buka aplikasi

Ekstrak `DC2TYMesTextTool_GUI.rar` ke folder pilihan Anda dan jalankan `DC2TYMesTextTool_GUI.exe`. Jika memakai source repositori, jalankan `bin\Release\net8.0-windows\DC2TYMesTextTool_GUI.exe`.
Aplikasi membutuhkan [.NET 8 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/8.0).

## Single File

### MES → JSON (Exporter)

1. Isi **Input MES/JSON** dengan file `.mes` asli.
2. Isi **Output JSON/MES**, atau kosongkan untuk otomatis membuat file `.json` di sebelah file input.
3. Klik **Export (MES -> JSON)**.

Hasil default menggabungkan nama karakter dan dialog berdasarkan struktur opcode skrip DC2TY:

```json
[
  { "message": "Narasi cerita." },
  { "name": "Yoshiyuki", "message": "「Dialog karakter.」" }
]
```

Ekspor dari tab Single File dan Batch selalu menggunakan format gabungan nama/dialog di atas. Teks, nama Jepang/Indonesia, dan baris baru dipertahankan secara akurat saat ekspor.

### JSON → MES (Importer)

1. Isi **Input MES/JSON** dengan file `.json` terjemahan.
2. Pastikan file template `.mes` asli dengan nama yang sama berada di folder yang sama (atau tentukan file MES asli sebagai basis).
3. Tentukan file **Output JSON/MES** yang berbeda dari file MES asli.
4. Atur **Word Wrap Width**: default **70** pada Single File, Batch, dan CLI. Jika teks tampak terlalu panjang atau sempit pada dialog box di game, nilai ini dapat disesuaikan.
5. Klik **Import (JSON -> MES)**.

Importer menerima JSON gabungan, format legacy, serta campuran keduanya. Jumlah token teks dan struktur label dicocokkan dengan MES asli untuk menjamin integritas alur permainan.

## Batch

- **Batch Export:** Isi **MES Folder** dan **Output Folder**, lalu klik **Batch Export (MES -> JSON)**. Semua file `.mes` di folder tersebut akan diekspor menjadi `.json`.
- **Batch Import:** Isi **JSON Folder**, **Original MES Folder** (template asli), dan **Output Folder** (tempat file `.mes` baru dihasilkan), lalu klik **Batch Import (JSON -> MES)**.
- Pencocokan nama file otomatis: misalnya `scene01.json` akan menggunakan template `scene01.mes`.
- File yang gagal diproses akan dicatat di panel log, dan proses batch tetap berlanjut ke file berikutnya.

## Pengaturan Layout & Patch Runtime

Rendering teks di layar diatur secara otomatis saat game berjalan oleh `DC2TYPatch.dll`. Pengaturan konfigurasi tersedia di `DC2TYPatch.ini`:

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

- **Font & Render Scaling**: `DialogueRenderScale=74`, `DialogueAdvanceScale=62`, dan `DialogueSpaceScale=24` mengatur proporsi lebar font Latin agar proporsional dan tidak saling menumpuk.
- **Redirection**: `EnableFileRedirection=1` mengalihkan pembacaan naskah ke folder terjemahan `id_Data\MES` atau `eng_data\MES` tanpa menimpa arsip asli game.
- **Backlog Icons**: `ShowBacklogIcon=1` mempertahankan dan menyesuaikan rendering ikon pembicara di backlog game.

Petunjuk teknis patch dan build C++ tersedia di [README Patch](DC2TYPatch%20a/README.md).

## Validasi dan Encoding

- Format teks skrip Circus dikodekan dalam **CP932 (Shift-JIS)**.
- Karakter Latin di luar tabel dasar Shift-JIS dipetakan ke padanan yang aman (misalnya tanda hubung panjang `— → ―`, huruf beraksen `û → u`, `ë → e`, `é → e`, `à → a`). Karakter yang tidak valid akan ditolak secara aman dengan pesan error yang jelas pada log.
- Penulisan berkas baru selalu dilakukan melalui file temporer dan divalidasi sebelum disimpan, untuk mencegah kerusakan file jika proses terhenti.

## CLI

Dari folder executable (`bin\Release\net8.0-windows` atau folder hasil ekstrak RAR):

```powershell
.\DC2TYMesTextTool_GUI.exe export "scene.mes" "scene.json"
.\DC2TYMesTextTool_GUI.exe import "scene.json" "original\scene.mes" "new\scene.mes" -w 70
.\DC2TYMesTextTool_GUI.exe batch-export "original_mes" "json_output"
.\DC2TYMesTextTool_GUI.exe batch-import "json_folder" "original_mes" "mes_output" -w 70
```

- Parameter `-w <lebar>`: Mengatur batas wrap kata (default: `70`).
- Mendukung opsi drag-and-drop: Tarik file `.mes` atau folder ke file `.exe` untuk mengekspor otomatis.

## Hasil Verifikasi

- Format ekspor dan impor naskah telah diverifikasi terhadap skenario asli DC2TY.
- Executable referensi game dan paket rilis memiliki hash SHA-256:

```text
c9b94b549406a71a073203e4de0087c889d6e7e55ed3ad350935a77ae185aadc  DC2TYMesTextTool_GUI.rar
c7bd70335f9cabf313085ad3e10d6ed15dafb43910e41804c877fd0a57726aec  Released_patch.rar
21988ca3227d494ad8fd816fd6363670b1a6245f82ac28314c4bb4e8353d19bd  DC2TY.EXE
```

## Build dari Source

Dari root folder repositori:

```powershell
# Build C# GUI & CLI (.NET 8 SDK diperlukan)
dotnet build -c Release
```

Untuk build patch C++ (Visual Studio 2022 x86 Native Tools):

```powershell
cd "DC2TYPatch a"
build.bat
```
