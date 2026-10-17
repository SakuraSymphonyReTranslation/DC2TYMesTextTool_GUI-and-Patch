# DC2TY MES Text Tool — GUI dan CLI

**Bahasa Indonesia** | [English](Readme_en.md)

Importer/exporter naskah skenario dan patch runtime untuk **D.C.II To You ～Da Capo II～ Side Episodes** (Da Capo 2 To You).

Distribusi ini hanya mendukung **Da Capo 2 To You** (`dc2ty`, versi MES `0x5F69`). GUI, CLI, tabel referensi C++, patch, dan launcher dikhususkan untuk game ini. Source utama patch berada di `DC2TYPatch a/src/dc2typatch.cpp`.

## Unduhan dan folder instalasi

Unduh kedua paket terbaru dari [GitHub Releases](https://github.com/SakuraSymphonyReTranslation/DC2TYMesTextTool_GUI-and-Patch/releases/latest):

Release **v1.0.0** diperbarui pada **17 Oktober 2026** dengan tata letak choice yang lebih rapat dan pemeriksaan konversi label `○`.

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

Importer menerima JSON gabungan, format legacy, serta campuran keduanya. MES asli digunakan sebagai template untuk menyusun kembali naskah dan memperbarui offset label. Pertahankan urutan entri JSON dan simpan hasil impor ke file terpisah.

### Label Choice `○`

Tulis label choice di kolom `message`, dengan satu spasi biasa setelah `○` dan di antara kata:

```json
[
  { "message": "○ Panggil Bos" },
  { "message": "○ Panggil Pangeran" },
  { "message": "○ Panggil Shinemon-san" }
]
```

Contoh di atas hanya memperlihatkan entri choice; saat mengimpor, gunakan JSON lengkap hasil ekspor dengan urutan entri yang sama. Simbol `○` dipertahankan pada MES → JSON dan JSON → MES melalui GUI maupun CLI, termasuk mode Batch yang memakai mesin konversi yang sama.

Spasi choice disimpan sebagai U+3000 di MES agar tidak berubah menjadi terminator NUL saat enkripsi. Exporter mengembalikannya menjadi spasi biasa di JSON. Tidak perlu mengganti lingkaran, menambahkan spasi ganda, atau mengedit byte MES secara manual.

Patch runtime terbaru merapikan tampilan choice cerita berawalan `○` pada mode terjemahan: margin kiri **12 px**, font Arial proporsional, dan satu spasi visual antarkata. Teks panjang diperkecil agar muat di bar yang ada. Aturan berlaku berdasarkan jenis tombol dan awalan label, tanpa daftar kalimat khusus. Label dengan perintah format `@` tetap ditangani renderer asli; mode Jepang juga memakai tampilan asli.

Ukuran bar dan area klik dipertahankan. Pembaruan choice tidak mengubah ukuran popup notifikasi, textbox, maupun pengaturan `DC2TYPatch.ini`.

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
- **Choice Cerita**: Spasi dan margin label `○` dirapikan otomatis pada mode terjemahan, tanpa opsi INI baru. Detail teknis tersedia di [Tata Letak Choice](https://github.com/SakuraSymphonyReTranslation/DC2TYMesTextTool_GUI-and-Patch/blob/master/DC2TYPatch%20a/CHOICE_LAYOUT.md).

Petunjuk teknis patch dan build C++ tersedia di [README Patch](https://github.com/SakuraSymphonyReTranslation/DC2TYMesTextTool_GUI-and-Patch/blob/master/DC2TYPatch%20a/README.md).

## Validasi dan Encoding

- Format teks skrip Circus dikodekan dalam **CP932 (Shift-JIS)**.
- Beberapa karakter Latin dan tanda baca dipetakan ke padanan CP932 (misalnya tanda hubung panjang `— → ―`, huruf beraksen `û → u`, `ë → e`, `é → e`, `à → a`). Periksa hasil ekspor ulang untuk karakter lain yang tidak tersedia dalam CP932.
- Hasil konversi ditulis langsung ke lokasi output. Gunakan file output terpisah, simpan cadangan MES asli, dan periksa panel log setelah impor.

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
- Audit **138 JSON terjemahan** menemukan **3 choice**, seluruhnya di `ty_myu02_a1_myu.json`; ketiganya cocok dengan label pada MES asli/aktif.
- Impor JSON Indonesia ke MES lalu ekspor ulang mempertahankan ketiga label `○` beserta spasinya. Build Win32 Release dan tes layout choice lulus; ketiga label juga sudah diperiksa secara visual di game.
- Ukuran dan tata letak textbox tetap sama pada adegan pengujian. Fungsi popup dan file INI tidak berubah; popup tidak diuji ulang secara visual pada sesi choice ini.
- Executable referensi game yang diuji memiliki hash SHA-256:

```text
21988CA3227D494AD8FD816FD6363670B1A6245F82AC28314C4BB4E8353D19BD  DC2TY.EXE
```

## Build dari Source

Dari root folder repositori:

```powershell
# Build C# GUI & CLI (.NET 8 SDK diperlukan)
dotnet build -c Release
```

Untuk build patch C++ (Visual Studio 2022 dengan komponen Desktop development with C++):

```powershell
cmake -S "DC2TYPatch a" -B "DC2TYPatch a/build-local" -G "Visual Studio 17 2022" -A Win32 -DDC2TY_BUILD_CHOICE_TESTS=ON
cmake --build "DC2TYPatch a/build-local" --config Release
ctest --test-dir "DC2TYPatch a/build-local" -C Release --output-on-failure
```

Seluruh isi `DC2TYPatch a/build/` disertakan dalam repositori, termasuk paket `Released` dan artefak build. Cache CMake menyimpan path mesin pembuatnya; gunakan `build-local` yang baru untuk build di komputer lain. Binary baru berada di `build-local/Release`. Skrip lama `build.bat` menghapus folder `build` sebelum membangun ulang.
