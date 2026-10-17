# DC2TY Patch — Instalasi dan Build

**Bahasa Indonesia** | [English](Readme_en.md)

Patch runtime dan custom launcher ini dikhususkan untuk **D.C.II To You ～Da Capo II～ Side Episodes** (Da Capo II To You). Source utama implementasi patch: `src/dc2typatch.cpp` dan launcher: `src/launcher.cpp`.

## Struktur Direktori

```text
DC2TYPatch a/
├── CMakeLists.txt        # Konfigurasi CMake build
├── build.bat             # Script build otomatis (NMake / MSVC)
├── icon.ico              # Ikon game DC2TY
├── src/
│   ├── dc2typatch.cpp    # Kode utama hook DLL patch (injected version)
│   ├── choice_layout.h  # Normalisasi dan bitmap label choice
│   ├── launcher.cpp      # Kode peluncur DC2TYLauncher (injeksi DLL + Locale Emulator)
│   ├── launcher.rc       # Resource peluncur (ikon & metadata)
│   ├── d3dx9_proxy.cpp   # Proxy D3DX9 opsional
│   └── dsound_proxy.cpp  # Proxy DSound opsional
├── Detours/              # Microsoft Detours library
├── Locale-Emulator-2.5.0.1/ # Library Locale Emulator
├── tests/                # Tes layout dan audit choice JSON/MES
└── build/
    ├── choice-layout/    # Build Win32 Release dan tes choice
    └── Released/         # Paket siap pakai (DC2TYLauncher.exe, DC2TYPatch.dll, dll)
```

## Instalasi

Unduh paket `Released_patch.rar` dari [GitHub Releases](https://github.com/SakuraSymphonyReTranslation/DC2TYMesTextTool_GUI-and-Patch/releases/latest), atau gunakan berkas di `build/Released`.

`<GAME_DIR>` berarti **folder utama instalasi game pilihan pemain**, yaitu folder yang memuat `DC2TY.exe` dan `AdvData`. Ganti placeholder ini dengan lokasi game di komputer Anda; tidak ada drive yang diwajibkan.

1. Tutup game dan buat cadangan file lama yang akan diganti.
2. Ekstrak isi `Released_patch.rar` langsung ke `<GAME_DIR>`, sehingga file `DC2TYLauncher.exe`, `DC2TYPatch.dll`, dan `DC2TYPatch.ini` berada di sebelah `DC2TY.exe`.
3. Jalankan **`<GAME_DIR>\DC2TYLauncher.exe`** dengan working directory `<GAME_DIR>`. Jika menggunakan shortcut, setel **Start in** ke `<GAME_DIR>`. Launcher akan memuat DLL patch dan mengaktifkan locale Jepang secara otomatis.
4. Letakkan naskah terjemahan Indonesia di `<GAME_DIR>\id_Data\MES` atau bahasa Inggris di `<GAME_DIR>\eng_data\MES`.

Executable referensi yang diuji memiliki SHA256:

```text
21988CA3227D494AD8FD816FD6363670B1A6245F82AC28314C4BB4E8353D19BD  DC2TY.EXE
```

## Fitur Patch & Konfigurasi

- **Choice Cerita `○`**:
  Pembaruan **v1.0.0, 17 Oktober 2026** memakai margin kiri 12 px, font Arial proporsional, dan satu spasi visual setelah lingkaran maupun antarkata. Label panjang diperkecil hingga muat di bar. Aturan berdasarkan tombol cerita dan awalan `○`, tanpa daftar kalimat khusus. Ukuran bar, area klik, warna, dan bayangan tetap mengikuti game.
- **Font Rendering & Advanced Scaling**:
  Diatur melalui `DC2TYPatch.ini`:
  - `DialogueRenderScale=74`
  - `DialogueAdvanceScale=62`
  - `DialogueSpaceScale=24`
  Menjaga keterbacaan teks alfabet Latin/Indonesia pada dialog tanpa tumpang-tindih.
- **Backlog & Popups**:
  `ShowBacklogIcon=1` memelihara ikon pembicara di backlog, serta memusatkan tampilan dialog konfirmasi sistem.
- **File Redirection**:
  `EnableFileRedirection=1` mengarahkan pembacaan file MES ke folder `id_Data\MES` atau `eng_data\MES`.
- **Locale Emulator**:
  Terintegrasi langsung di launcher untuk Windows native serta kompatibel dengan Wine / Proton pada Linux / Steam Deck.

### Dukungan MES ↔ JSON

Tool GUI/CLI mempertahankan `○` pada ekspor dan impor, termasuk mode Batch. Tulis satu spasi biasa di JSON, misalnya `{ "message": "○ Panggil Bos" }`. Importer menyimpan spasi choice sebagai U+3000 agar aman untuk enkripsi MES; exporter mengembalikan spasi biasa, sedangkan patch merapikan jaraknya saat digambar.

Renderer choice aktif pada mode terjemahan (`Language=1` atau `2`). Mode Jepang dan label dengan perintah format `@` menggunakan renderer asli. Tidak ada pengaturan INI baru, perubahan ukuran popup notifikasi, atau perubahan ukuran textbox. Detail implementasi dan hasil audit tersedia di [Tata Letak Choice](CHOICE_LAYOUT.md).

### Hasil Verifikasi

- Audit 138 JSON terjemahan menemukan 3 choice di `ty_myu02_a1_myu.json`, sesuai MES asli dan MES aktif.
- Impor JSON Indonesia → MES → ekspor JSON mempertahankan ketiga label dan spasinya.
- Tes layout Win32 Release lulus. Ketiga choice telah diperiksa secara visual di game dan muat di bar dengan lingkaran sejajar.
- Textbox pada adegan pengujian tetap sama. Fungsi popup dan file INI tidak berubah; popup tidak diuji ulang secara visual pada sesi ini.

## Cara Build (Visual Studio 2022, Win32)

Pasang komponen **Desktop development with C++**, lalu jalankan dari folder `DC2TYPatch a`:

```powershell
cmake -S . -B build-local -G "Visual Studio 17 2022" -A Win32 -DDC2TY_BUILD_CHOICE_TESTS=ON
cmake --build build-local --config Release
ctest --test-dir build-local -C Release --output-on-failure
```

Binary baru berada di `build-local/Release`. Opsi `DC2TY_BUILD_CHOICE_TESTS` menambahkan tes layout standalone dan dapat dinonaktifkan untuk build biasa.

Seluruh isi `build/` disertakan dalam repositori, termasuk cache, artefak build, log, dan paket `build/Released`. Cache berisi path mesin pembuatnya, sehingga build baru memakai direktori `build-local`. Skrip lama `build.bat` menghapus folder `build` sebelum membangun ulang.
