#include "choice_layout.h"
#include <cstdio>
#include <cstdlib>

static void Check(bool ok, const char *message) {
  if (!ok) {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

int main() {
  std::wstring normalized;
  Check(choice_layout::Normalize(L"  \u3000\u25cb\u3000 Panggil\t\u3000Bos  ", normalized) &&
            normalized == L"\u25cb Panggil Bos", "collapse MES and JSON whitespace");
  Check(choice_layout::Normalize(L"\u25cb\uff30\uff41\uff4e\uff47\uff47\uff49\uff4c", normalized) &&
            normalized == L"\u25cb Panggil", "fullwidth Latin and missing marker gap");
  Check(!choice_layout::Normalize(L"Simpan permainan?", normalized), "popup excluded");
  Check(!choice_layout::Normalize(L"Dialog dengan \u25cb di tengah", normalized), "dialogue excluded");
  Check(!choice_layout::Normalize(L"\u25cb  ", normalized), "empty choice excluded");
  // CP932 0x8140 contains an ASCII '@' byte but is a valid ideographic space.
  const char cp932[] = "\x81\x9b\x81\x40Panggil\x81\x40" "Bos";
  wchar_t wide[64] = {};
  Check(MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, cp932, -1, wide, 64) > 0,
        "decode encrypted-format choice after engine decryption");
  Check(std::wstring(wide).find(L'@') == std::wstring::npos &&
            choice_layout::Normalize(wide, normalized) &&
            normalized == L"\u25cb Panggil Bos", "CP932 trail bytes are not commands");

  const std::wstring labels[] = {L"\u25cb Panggil Bos", L"\u25cb Panggil Pangeran",
                                 L"\u25cb Panggil Shinemon-san",
                                 L"\u25cb Pilihan lain, dengan kata-kata yang lebih panjang!",
                                 L"\u25cb " + std::wstring(220, L'W')};
  DWORD handles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
  for (const auto &label : labels) {
    for (int width : {455, 160}) {
      choice_layout::Bitmap bitmap;
      Check(choice_layout::Build(label, width, 28, GetTextExtentPoint32W,
                                  GetGlyphOutlineW, bitmap), "render current and future choices");
      Check(bitmap.width > 0 && bitmap.width <= width && bitmap.height <= 28,
            "choice remains inside bar");
      Check(bitmap.pixels.size() == (size_t)bitmap.width * bitmap.height,
            "complete bitmap");
      int visible = 0;
      for (BYTE alpha : bitmap.pixels) {
        Check(alpha <= 4, "native alpha range");
        visible += alpha != 0;
      }
      Check(visible > 100, "visible glyph coverage");
      printf("PASS chars=%zu available=%d bitmap=%dx%d\n", label.size(), width,
             bitmap.width, bitmap.height);
    }
  }
  Check(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= handles,
        "no GDI handle leak");
  puts("Choice layout checks passed.");
}
