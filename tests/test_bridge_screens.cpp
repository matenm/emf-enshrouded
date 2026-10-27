// The bridge's screenshot files: the path rule (only <emf>\screens\<time stamp>.png), packing a swapchain copy into
// BGR, the PNG encoder (decoded back with WIC), and writing a new file only inside emf\screens.
#include <windows.h>

#include <objbase.h>
#include <wincodec.h>

#include <doctest.h>

#include <string>
#include <vector>

#include "core/bridge/screens.h"
#include "test_scratch.h"

using namespace emf::core::bridge;

namespace {

std::wstring temp_emf_dir() {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    static int n = 0;
    std::wstring dir = std::wstring(base) + L"emf_test_screens_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                       std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(++n);
    CreateDirectoryW(dir.c_str(), nullptr);
    test_scratch::remove_at_exit(dir);  // when the run ends
    return dir + L"\\emf";
}

// Decodes a PNG with WIC into 24-bit BGR rows.
bool decode_png(const std::vector<uint8_t>& png, uint32_t* w, uint32_t* h, std::vector<uint8_t>* bgr) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    IWICImagingFactory* f = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    const GUID clsid = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
    const GUID bgr24 = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};
    if (SUCCEEDED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                   reinterpret_cast<void**>(&f))) &&
        SUCCEEDED(f->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size()))) &&
        SUCCEEDED(f->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(w, h)) &&
        SUCCEEDED(f->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, bgr24, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        bgr->resize(static_cast<size_t>(*w) * *h * 3);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, *w * 3, static_cast<UINT>(bgr->size()), bgr->data()));
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{conv, frame, dec, stream, f})
        if (u) u->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

}  // namespace

TEST_CASE("screenshots: the file name is a time stamp, with a counter for the same millisecond") {
    SYSTEMTIME t{};
    t.wYear = 2026;
    t.wMonth = 10;
    t.wDay = 26;
    t.wHour = 20;
    t.wMinute = 5;
    t.wSecond = 9;
    t.wMilliseconds = 7;
    CHECK(screenshot_name(t, 1) == L"2026-10-26_20-05-09-007.png");
    CHECK(screenshot_name(t, 2) == L"2026-10-26_20-05-09-007_2.png");
}

TEST_CASE("screenshots: only a .png directly inside emf\\screens is a valid path") {
    const std::wstring emf = L"C:\\Games\\Enshrouded\\emf";
    CHECK(screenshot_path_ok(emf, L"C:\\Games\\Enshrouded\\emf\\screens\\2026-10-26_20-05-09-007.png"));
    CHECK(screenshot_path_ok(emf, L"c:\\games\\enshrouded\\EMF\\Screens\\a.png"));  // the folder ignoring case
    CHECK_FALSE(screenshot_path_ok(emf, L"C:\\Games\\Enshrouded\\emf\\screens\\..\\emf_core.dll"));
    CHECK_FALSE(screenshot_path_ok(emf, L"C:\\Games\\Enshrouded\\emf\\screens\\..\\x.png"));
    CHECK_FALSE(screenshot_path_ok(emf, L"C:\\Games\\Enshrouded\\emf\\screens\\sub\\x.png"));
    CHECK_FALSE(screenshot_path_ok(emf, L"C:\\Games\\Enshrouded\\emf\\x.png"));
    CHECK_FALSE(screenshot_path_ok(emf, L"C:\\Windows\\x.png"));
    CHECK_FALSE(screenshot_path_ok(emf, L"C:\\Games\\Enshrouded\\emf\\screens\\x.png:stream"));
    CHECK_FALSE(screenshot_path_ok(emf, L"C:\\Games\\Enshrouded\\emf\\screens\\x.dll"));
    CHECK_FALSE(screenshot_path_ok(emf, L"C:\\Games\\Enshrouded\\emf\\screens\\.png"));
    CHECK_FALSE(screenshot_path_ok(emf, L"C:\\Games\\Enshrouded\\emf\\screens/x.png"));
    CHECK_FALSE(screenshot_path_ok(emf, L"C:\\Games\\Enshrouded\\emf\\screens\\"));
    CHECK_FALSE(screenshot_path_ok(L"", L"\\screens\\x.png"));
}

TEST_CASE("screenshots: a swapchain copy is packed into tight BGR rows, alpha left out") {
    // 2x2 pixels, rows 12 bytes apart (4 bytes of padding)
    const uint8_t bgra[] = {1, 2, 3, 99, 4, 5, 6, 99, 0xEE, 0xEE, 0xEE, 0xEE,  //
                            7, 8, 9, 99, 10, 11, 12, 99, 0xEE, 0xEE, 0xEE, 0xEE};
    CHECK(pack_bgr(bgra, 12, 2, 2, PixelOrder::BGRA) == std::vector<uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
    CHECK(pack_bgr(bgra, 12, 2, 2, PixelOrder::RGBA) == std::vector<uint8_t>{3, 2, 1, 6, 5, 4, 9, 8, 7, 12, 11, 10});
}

TEST_CASE("screenshots: the PNG decodes back to the same pixels") {
    std::vector<uint8_t> bgr;
    for (int i = 0; i < 3 * 2; ++i) {
        bgr.push_back(static_cast<uint8_t>(i * 40));
        bgr.push_back(static_cast<uint8_t>(255 - i * 30));
        bgr.push_back(static_cast<uint8_t>(i * 7));
    }
    std::string why;
    const auto png = encode_png_bgr(bgr.data(), 3, 2, &why);
    REQUIRE_MESSAGE(!png.empty(), why);
    CHECK(png.size() > 8);
    CHECK(png[1] == 'P');
    CHECK(png[2] == 'N');
    CHECK(png[3] == 'G');
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> back;
    REQUIRE(decode_png(png, &w, &h, &back));
    CHECK(w == 3);
    CHECK(h == 2);
    CHECK(back == bgr);
}

TEST_CASE("screenshots: a picture is written as a new file in emf\\screens, nowhere else, never over another") {
    const std::wstring emf = temp_emf_dir();
    CreateDirectoryW(emf.c_str(), nullptr);
    const std::vector<uint8_t> bytes = {0x89, 'P', 'N', 'G'};
    std::string why;
    const std::wstring path = emf + L"\\screens\\a.png";
    REQUIRE_MESSAGE(write_screenshot(emf, path, bytes, &why), why);  // the folder is created
    CHECK(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK_FALSE(write_screenshot(emf, path, bytes, &why));  // never over an existing file
    CHECK(why.find("exists") != std::string::npos);
    CHECK_FALSE(write_screenshot(emf, emf + L"\\emf_core.png", bytes, &why));
    CHECK(why.find("emf\\screens") != std::string::npos);
    CHECK_FALSE(write_screenshot(emf, emf + L"\\screens\\..\\b.png", bytes, &why));
    CHECK(GetFileAttributesW((emf + L"\\b.png").c_str()) == INVALID_FILE_ATTRIBUTES);
}
