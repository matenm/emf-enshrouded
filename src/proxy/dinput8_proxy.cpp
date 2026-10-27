// Forwards the six dinput8.dll exports to the real system DLL (loaded on first use, not in DllMain).
#include <windows.h>

namespace {
HMODULE real_dinput8() {
    static HMODULE h = [] {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 16) return HMODULE{nullptr};
        wcscat_s(path, L"\\dinput8.dll");
        return LoadLibraryW(path);
    }();
    return h;
}

template <class F>
F real(const char* name) {
    HMODULE h = real_dinput8();
    return h ? reinterpret_cast<F>(GetProcAddress(h, name)) : nullptr;
}
}  // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto f = real<Fn>("DirectInput8Create");
    return f ? f(inst, version, riid, out, outer) : E_FAIL;
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
    using Fn = HRESULT(WINAPI*)();
    auto f = real<Fn>("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto f = real<Fn>("DllGetClassObject");
    return f ? f(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI Proxy_DllRegisterServer() {
    using Fn = HRESULT(WINAPI*)();
    auto f = real<Fn>("DllRegisterServer");
    return f ? f() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
    using Fn = HRESULT(WINAPI*)();
    auto f = real<Fn>("DllUnregisterServer");
    return f ? f() : E_FAIL;
}

const void* WINAPI Proxy_GetdfDIJoystick() {
    using Fn = const void*(WINAPI*)();
    auto f = real<Fn>("GetdfDIJoystick");
    return f ? f() : nullptr;
}

}  // extern "C"
