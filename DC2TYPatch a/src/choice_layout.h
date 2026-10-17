#pragma once

#include <windows.h>
#include <string>
#include <vector>

// Story choices use encrypted CP932 strings: an ASCII space would encode as
// NUL. Normalize only after decoding, without changing MES or JSON contents.
namespace choice_layout {
struct Bitmap {
  int width = 0;
  int height = 0;
  std::vector<BYTE> pixels;
};

inline bool Normalize(const std::wstring &input, std::wstring &output) {
  output.clear();
  bool space = false;
  for (wchar_t ch : input) {
    if (ch >= 0xff01 && ch <= 0xff5e)
      ch -= 0xfee0;
    if (ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' ||
        ch == 0x3000 || ch == 0xa0) {
      space = !output.empty();
      continue;
    }
    if (ch < 0x20)
      return false;
    if (space)
      output.push_back(L' ');
    output.push_back(ch);
    space = false;
  }
  if (output.size() < 2 || output[0] != 0x25cb)
    return false;
  // Consistent gap even when the original Japanese label has no space.
  if (output[1] != L' ')
    output.insert(1, 1, L' ');
  return output.size() > 2;
}

// Use unhooked GDI metrics so dialogue font/spacing settings cannot affect a
// choice bitmap. The engine's alpha blitter expects values from zero to four.
inline bool Build(const std::wstring &label, int maxWidth, int maxHeight,
                  decltype(&GetTextExtentPoint32W) extentFn,
                  decltype(&GetGlyphOutlineW) glyphFn, Bitmap &bitmap) {
  if (label.empty() || label.size() > 255 || maxWidth < 28 ||
      maxWidth > 800 || maxHeight < 18 || maxHeight > 40)
    return false;
  HDC dc = CreateCompatibleDC(NULL);
  if (!dc)
    return false;
  bool success = false;
  for (int size = 28; size >= 18 && !success; --size) {
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH, L"Arial");
    if (!font)
      break;
    HGDIOBJ previous = SelectObject(dc, font);
    SIZE extent = {};
    bool fits = extentFn(dc, label.c_str(), (int)label.size(), &extent) &&
                extent.cx > 0 && extent.cx <= 8192 &&
                (extent.cx <= maxWidth || size == 18);
    int canvasWidth = fits ? extent.cx + 16 : 16;
    constexpr int canvasHeight = 64;
    std::vector<BYTE> canvas(canvasWidth * canvasHeight, 0);
    MAT2 matrix = {};
    matrix.eM11.value = matrix.eM22.value = 1;
    for (size_t i = 0; fits && i < label.size(); ++i) {
      SIZE prefix = {};
      if (i && !extentFn(dc, label.c_str(), (int)i, &prefix)) {
        fits = false;
        break;
      }
      GLYPHMETRICS gm = {};
      DWORD bytes = glyphFn(dc, label[i], GGO_GRAY8_BITMAP, &gm, 0, NULL,
                            &matrix);
      if (bytes == GDI_ERROR || bytes > 65536) {
        fits = false;
        break;
      }
      if (!bytes)
        continue;
      std::vector<BYTE> glyph(bytes);
      DWORD stride = (gm.gmBlackBoxX + 3) & ~3u;
      if ((ULONGLONG)stride * gm.gmBlackBoxY > bytes ||
          glyphFn(dc, label[i], GGO_GRAY8_BITMAP, &gm, bytes, glyph.data(),
                  &matrix) == GDI_ERROR) {
        fits = false;
        break;
      }
      for (DWORD gy = 0; gy < gm.gmBlackBoxY; ++gy) {
        for (DWORD gx = 0; gx < gm.gmBlackBoxX; ++gx) {
          int x = 8 + prefix.cx + gm.gmptGlyphOrigin.x + (int)gx;
          int y = 40 - gm.gmptGlyphOrigin.y + (int)gy;
          if (x < 0 || x >= canvasWidth || y < 0 || y >= canvasHeight) {
            fits = false;
            continue;
          }
          BYTE alpha = (BYTE)((glyph[gy * stride + gx] + 8) / 16);
          BYTE &pixel = canvas[y * canvasWidth + x];
          if (alpha > pixel)
            pixel = alpha;
        }
      }
    }
    SelectObject(dc, previous);
    DeleteObject(font);
    if (!fits)
      continue;
    int left = canvasWidth, top = canvasHeight, right = -1, bottom = -1;
    for (int y = 0; y < canvasHeight; ++y) {
      for (int x = 0; x < canvasWidth; ++x) {
        if (canvas[y * canvasWidth + x]) {
          left = min(left, x);
          right = max(right, x);
          top = min(top, y);
          bottom = max(bottom, y);
        }
      }
    }
    int width = right - left + 1, height = bottom - top + 1;
    if (width <= 0 || height <= 0 || height > maxHeight)
      continue;
    bitmap.width = min(width, maxWidth);
    bitmap.height = height;
    bitmap.pixels.assign(bitmap.width * height, 0);
    // Very long future translations still fit at the minimum font size.
    // Area averaging keeps thin strokes when horizontal compression is needed.
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < bitmap.width; ++x) {
        int begin = x * width / bitmap.width;
        int end = (x + 1) * width / bitmap.width;
        int alpha = 0;
        for (int sx = begin; sx < end; ++sx)
          alpha += canvas[(top + y) * canvasWidth + left + sx];
        bitmap.pixels[y * bitmap.width + x] =
            (BYTE)((alpha + (end - begin) / 2) / (end - begin));
      }
    }
    success = true;
  }
  DeleteDC(dc);
  return success;
}
} // namespace choice_layout
