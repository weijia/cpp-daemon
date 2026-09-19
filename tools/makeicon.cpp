// 一次性工具：把一张 PNG 生成多尺寸（16/32/48/64/128/256）PNG 压缩的 .ico。
// 仅用于构建期生成 assets/appicon.ico，不参与最终程序。
#include <windows.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
  UINT num = 0, size = 0;
  Gdiplus::GetImageEncodersSize(&num, &size);
  if (size == 0) return -1;
  auto* encoders = (Gdiplus::ImageCodecInfo*)malloc(size);
  Gdiplus::GetImageEncoders(num, size, encoders);
  int found = -1;
  for (UINT j = 0; j < num; ++j) {
    if (wcscmp(encoders[j].MimeType, format) == 0) {
      *pClsid = encoders[j].Clsid;
      found = (int)j;
      break;
    }
  }
  free(encoders);
  return found;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: makeicon <in.png> <out.ico>\n");
    return 1;
  }
  Gdiplus::GdiplusStartupInput gsi;
  ULONG_PTR token = 0;
  Gdiplus::GdiplusStartup(&token, &gsi, nullptr);

  std::wstring win;
  int n = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
  win.resize(n);
  MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, &win[0], n);
  Gdiplus::Bitmap* src = Gdiplus::Bitmap::FromFile(win.c_str());
  if (!src || src->GetLastStatus() != Gdiplus::Ok) {
    fprintf(stderr, "load png failed\n");
    return 1;
  }

  CLSID pngClsid;
  if (GetEncoderClsid(L"image/png", &pngClsid) < 0) {
    fprintf(stderr, "png encoder not found\n");
    return 1;
  }

  const int sizes[] = {16, 32, 48, 64, 128, 256};
  std::vector<std::vector<BYTE>> frames;
  for (int s : sizes) {
    Gdiplus::Bitmap bmp(s, s, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.DrawImage(src, 0, 0, s, s);

    IStream* pstm = nullptr;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &pstm)) || !pstm) {
      fprintf(stderr, "stream failed\n");
      return 1;
    }
    if (bmp.Save(pstm, &pngClsid, NULL) != Gdiplus::Ok) {
      fprintf(stderr, "encode failed\n");
      return 1;
    }
    HGLOBAL hg = NULL;
    GetHGlobalFromStream(pstm, &hg);
    SIZE_T len = GlobalSize(hg);
    void* p = GlobalLock(hg);
    frames.emplace_back((BYTE*)p, (BYTE*)p + len);
    GlobalUnlock(hg);
    pstm->Release();
  }
  delete src;

  // 组装 ICONDIR + 多个 ICONDIRENTRY + PNG 数据
  FILE* f = fopen(argv[2], "wb");
  if (!f) {
    fprintf(stderr, "open out failed\n");
    return 1;
  }
  WORD reserved = 0, type = 1;
  WORD count = (WORD)frames.size();
  fwrite(&reserved, 1, 2, f);
  fwrite(&type, 1, 2, f);
  fwrite(&count, 1, 2, f);

  DWORD offset = 6 + (DWORD)frames.size() * 16;
  for (size_t i = 0; i < frames.size(); ++i) {
    int s = sizes[i];
    BYTE w = (s >= 256) ? 0 : (BYTE)s;
    BYTE hdr[16] = {0};
    hdr[0] = w;            // width
    hdr[1] = w;            // height
    hdr[2] = 0;            // colors
    hdr[3] = 0;            // reserved
    // planes (2 bytes, little endian) at 4
    hdr[4] = 1; hdr[5] = 0;
    // bpp (2 bytes) at 6: 32
    hdr[6] = 32; hdr[7] = 0;
    // image size (4 bytes) at 8
    DWORD sz = (DWORD)frames[i].size();
    memcpy(hdr + 8, &sz, 4);
    // image offset (4 bytes) at 12
    memcpy(hdr + 12, &offset, 4);
    fwrite(hdr, 1, 16, f);
    offset += sz;
  }
  for (auto& fr : frames) fwrite(fr.data(), 1, fr.size(), f);
  fclose(f);

  Gdiplus::GdiplusShutdown(token);
  printf("wrote %s (%u frames)\n", argv[2], count);
  return 0;
}
