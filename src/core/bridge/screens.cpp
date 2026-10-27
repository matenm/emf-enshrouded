#include "screens.h"

#include <objbase.h>
#include <wincodec.h>

#include <cstdio>
#include <cwchar>

namespace emf::core::bridge {

std::wstring screenshot_name(const SYSTEMTIME& t, unsigned n) {
    wchar_t b[64];
    std::swprintf(b, 64, L"%04u-%02u-%02u_%02u-%02u-%02u-%03u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond, t.wMilliseconds);
    std::wstring name = b;
    if (n > 1) name += L"_" + std::to_wstring(n);
    return name + L".png";
}

bool screenshot_path_ok(const std::wstring& emf_dir, const std::wstring& path) {
    if (emf_dir.empty()) return false;
    const std::wstring prefix = emf_dir + L"\\screens\\";
    if (path.size() <= prefix.size() || _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) != 0) return false;
    const std::wstring name = path.substr(prefix.size());
    if (name.size() <= 4 || name[0] == L'.' || _wcsicmp(name.c_str() + name.size() - 4, L".png") != 0) return false;
    for (wchar_t c : name) {
        const bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_' ||
                        c == L'-' || c == L'.';
        if (!ok) return false;  // no folders, streams, drives or anything else
    }
    return name.find(L"..") == std::wstring::npos;
}

std::vector<uint8_t> pack_bgr(const uint8_t* src, size_t row_pitch, uint32_t width, uint32_t height, PixelOrder order) {
    std::vector<uint8_t> out(static_cast<size_t>(width) * height * 3);
    uint8_t* d = out.data();
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* s = src + static_cast<size_t>(y) * row_pitch;
        for (uint32_t x = 0; x < width; ++x, s += 4, d += 3) {
            if (order == PixelOrder::BGRA) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            } else {
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
            }
        }
    }
    return out;
}

namespace {

// WIC through ole32, loaded on first use (emf_core.dll does not import ole32).
struct Ole {
    using CoInitializeExFn = HRESULT(WINAPI*)(LPVOID, DWORD);
    using CoUninitializeFn = void(WINAPI*)();
    using CoCreateInstanceFn = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
    using CreateStreamOnHGlobalFn = HRESULT(WINAPI*)(HGLOBAL, BOOL, LPSTREAM*);
    CoInitializeExFn init = nullptr;
    CoUninitializeFn uninit = nullptr;
    CoCreateInstanceFn create = nullptr;
    CreateStreamOnHGlobalFn stream = nullptr;
    bool ok() const { return init && uninit && create && stream; }
};

const Ole& ole() {
    static const Ole o = [] {
        Ole r;
        if (HMODULE m = LoadLibraryW(L"ole32.dll")) {  // stays loaded
            r.init = reinterpret_cast<Ole::CoInitializeExFn>(GetProcAddress(m, "CoInitializeEx"));
            r.uninit = reinterpret_cast<Ole::CoUninitializeFn>(GetProcAddress(m, "CoUninitialize"));
            r.create = reinterpret_cast<Ole::CoCreateInstanceFn>(GetProcAddress(m, "CoCreateInstance"));
            r.stream = reinterpret_cast<Ole::CreateStreamOnHGlobalFn>(GetProcAddress(m, "CreateStreamOnHGlobal"));
        }
        return r;
    }();
    return o;
}

constexpr GUID kWicFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
constexpr GUID kPngContainer = {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
constexpr GUID kBgr24 = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

std::string hr_text(const char* what, HRESULT hr) {
    char b[32];
    std::snprintf(b, sizeof b, " (0x%08lx)", static_cast<unsigned long>(hr));
    return std::string(what) + b;
}

}  // namespace

std::vector<uint8_t> encode_png_bgr(const uint8_t* bgr, uint32_t width, uint32_t height, std::string* why) {
    std::vector<uint8_t> out;
    auto fail = [&](std::string text) {
        if (why) *why = std::move(text);
        out.clear();
    };
    const Ole& o = ole();
    if (!o.ok()) {
        fail("the PNG encoder is not available (ole32)");
        return out;
    }
    if (!width || !height || width > 16384 || height > 16384) {
        fail("the picture has no size or is too big");
        return out;
    }
    const HRESULT init = o.init(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    HRESULT hr = o.create(kWicFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                          reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) fail(hr_text("WIC could not start", hr));
    if (SUCCEEDED(hr) && FAILED(hr = o.stream(nullptr, TRUE, &stream))) fail(hr_text("no memory stream", hr));
    if (SUCCEEDED(hr) && FAILED(hr = factory->CreateEncoder(kPngContainer, nullptr, &enc)))
        fail(hr_text("no PNG encoder", hr));
    if (SUCCEEDED(hr) && FAILED(hr = enc->Initialize(stream, WICBitmapEncoderNoCache)))
        fail(hr_text("the PNG encoder did not start", hr));
    if (SUCCEEDED(hr) && FAILED(hr = enc->CreateNewFrame(&frame, &props))) fail(hr_text("no PNG frame", hr));
    if (SUCCEEDED(hr) && FAILED(hr = frame->Initialize(props))) fail(hr_text("the PNG frame did not start", hr));
    if (SUCCEEDED(hr) && FAILED(hr = frame->SetSize(width, height))) fail(hr_text("the PNG size was refused", hr));
    WICPixelFormatGUID fmt = kBgr24;
    if (SUCCEEDED(hr) && FAILED(hr = frame->SetPixelFormat(&fmt))) fail(hr_text("the pixel format was refused", hr));
    if (SUCCEEDED(hr) && fmt != kBgr24) {
        hr = E_FAIL;
        fail("the PNG encoder wants another pixel format");
    }
    const UINT stride = width * 3;
    if (SUCCEEDED(hr) &&
        FAILED(hr = frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(bgr))))
        fail(hr_text("writing the pixels failed", hr));
    if (SUCCEEDED(hr) && FAILED(hr = frame->Commit())) fail(hr_text("the PNG frame was not finished", hr));
    if (SUCCEEDED(hr) && FAILED(hr = enc->Commit())) fail(hr_text("the PNG was not finished", hr));
    if (SUCCEEDED(hr)) {
        STATSTG st{};
        const LARGE_INTEGER zero{};
        if (FAILED(hr = stream->Stat(&st, STATFLAG_NONAME)) || st.cbSize.QuadPart > (512ull << 20) ||
            FAILED(hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
            fail(hr_text("the PNG could not be read back", hr));
        } else {
            out.resize(static_cast<size_t>(st.cbSize.QuadPart));
            ULONG got = 0;
            if (FAILED(hr = stream->Read(out.data(), static_cast<ULONG>(out.size()), &got)) || got != out.size())
                fail(hr_text("the PNG could not be read back", hr));
        }
    }
    release(props);
    release(frame);
    release(enc);
    release(stream);
    release(factory);
    if (SUCCEEDED(init)) o.uninit();
    return out;
}

bool write_screenshot(const std::wstring& emf_dir, const std::wstring& path, const std::vector<uint8_t>& bytes,
                      std::string* why) {
    auto fail = [why](std::string text) {
        if (why) *why = std::move(text);
        return false;
    };
    if (!screenshot_path_ok(emf_dir, path)) return fail("not written: pictures only go into emf\\screens");
    const std::wstring dir = emf_dir + L"\\screens";
    DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return fail("emf\\screens could not be created (error " + std::to_string(GetLastError()) + ")");
        attrs = GetFileAttributesW(dir.c_str());
    }
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
        return fail("not written: emf\\screens is a link or not a folder");
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return fail(e == ERROR_FILE_EXISTS ? "not written: the file already exists"
                                           : "the picture could not be written (error " + std::to_string(e) + ")");
    }
    size_t done = 0;
    bool ok = true;
    while (ok && done < bytes.size()) {
        DWORD n = 0;
        const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(bytes.size() - done, 1u << 20));
        ok = WriteFile(h, bytes.data() + done, chunk, &n, nullptr) && n == chunk;
        done += n;
    }
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(path.c_str());
        return fail("the picture could not be written (disk full?)");
    }
    return true;
}

}  // namespace emf::core::bridge
