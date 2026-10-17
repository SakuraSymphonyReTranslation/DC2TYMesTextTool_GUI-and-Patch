# Tata letak choice DC2TY

Patch 17 Oktober 2026 merapikan teks choice cerita: margin kiri 12 piksel,
satu spasi setelah lingkaran dan antarkata, serta huruf Arial proporsional.
Batas bar, area klik, warna, dan bayangan tetap mengikuti game.
Tidak ada opsi INI baru atau perubahan ukuran popup dan textbox.

## Hasil audit naskah

Audit seluruh 138 JSON pada folder `JSON_Translated` yang diuji
menemukan tiga label choice, semuanya di `ty_myu02_a1_myu.json`:

| Indeks JSON (mulai 0) | Label | Lebar bitmap dalam tes standalone, font 28 px |
| --- | --- | --- |
| 66 | ○ Panggil Bos | 172 px |
| 67 | ○ Panggil Pangeran | 244 px |
| 68 | ○ Panggil Shinemon-san | 304 px |

Ketiganya cocok dengan opcode `0x4D` pada MES aktif. Audit MES asli juga
menemukan tiga choice. Tidak ada kesalahan parsing. Label terpanjang berisi
22 karakter, termasuk lingkaran dan spasi.

JSON sudah memakai satu spasi biasa. Importir mengubahnya menjadi U+3000
karena enkripsi MES mengurangi setiap byte dengan `0x20`: spasi ASCII akan
menjadi NUL dan memutus string. Normalisasi dilakukan setelah game mendekripsi
dan mengembangkan variabel; JSON dan MES tidak perlu diubah.

## Batas perubahan

`EXE+1C2C0` menggambar layer teks sebuah tombol. Hook memverifikasi mode cerita
dan keanggotaan layer di tabel tombol utama, lalu membaca batas bar pemiliknya.
`EXE+24240` menerima label lengkap setelah ekspansi variabel. Hanya label
berawalan `○` dalam scope tersebut yang memakai renderer baru; tidak ada
daftar khusus tiga kalimat di kode. Mode Jepang menggunakan renderer bawaan.

Bar aktif berukuran 480 × 40 px. Area teks menyediakan 455 px ditambah satu
pixel untuk bayangan. Font dimulai pada 28 px dan diperkecil sampai 18 px bila
perlu; teks yang masih terlalu panjang dikompresi secara horizontal. Label
dengan perintah format `@` tetap memakai interpretasi asli engine.

Gambar menggunakan alpha 0–4 melalui blitter asli `EXE+11220`. Clip untuk
layer disimpan dan dipulihkan setelah gambar selesai. State tombol dan area
klik tidak dimodifikasi. Bitmap disimpan dalam cache yang dibatasi 128 label.

`ReadSystemPopupLabel`, `BuildSystemPopupBitmap`, `DrawSystemPopupBitmap`,
dan renderer dialog `Hook_Hsxf4_22BB0` sama persis dengan versi sebelumnya.
Ukuran popup, posisi popup, dan `DC2TYPatch.ini` tidak diubah.

## Build dan pemeriksaan

```powershell
cmake -S . -B build-local -G "Visual Studio 17 2022" -A Win32 -DDC2TY_BUILD_CHOICE_TESTS=ON
cmake --build build-local --config Release --target DC2TYPatch choice_layout_test
ctest --test-dir build-local -C Release --output-on-failure
python tests/audit_choices.py "<JSON_Translated>" "<MES_asli>" "<MES_aktif>" "<laporan.json>"
```

Jalankan dari folder `DC2TYPatch a` dan ganti placeholder dengan path lokal.
Hasil build sesi pengujian tersimpan di `build/choice-layout`; gunakan
direktori baru saat membangun dari clone agar cache path lokal tidak dipakai.

Build Win32 Release dan CTest lulus. Tes mencakup tiga label aktual, spasi
U+3000/tab/ganda, Latin fullwidth, CP932 dengan trail byte `0x40`, pengecualian
popup/dialog, teks panjang, batas bar, alpha native, dan kebocoran handle GDI.

DLL siap pakai tersedia di `build/Released/DC2TYPatch.dll`. Audit bersifat
read-only dan menulis hasil ke file laporan yang ditentukan pada perintah.
Verifikasi visual selesai pada 17 Oktober 2026 melalui screenshot pengguna
setelah game dibuka ulang. Ketiga lingkaran sejajar, spasi antarkata normal,
dan seluruh label muat di dalam bar. Textbox pada adegan yang sama tetap
memiliki ukuran dan tata letak sebelumnya.

Log game mencatat tiga bitmap aktual berukuran 170 × 26, 233 × 26, dan
293 × 26 px, semuanya dengan margin 12 px. Hash DLL terpasang cocok dengan
hasil build.
Ukuran popup tidak diuji ulang secara visual pada sesi ini; fungsi popup dan
file INI telah dibandingkan dengan cadangan dan tetap sama.
