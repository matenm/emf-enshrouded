// EMF.exe: the desktop mod manager. A Win32 window hosts WebView2, which shows the page in the ui folder next to the
// exe (index.html, app.css, app.js) as https://emf.app/ (a virtual host: no file:// and no network). The bridge
// (bridge.h) turns the page's messages into changes in the game folder; the state goes back every few seconds, so a game
// start or stop shows up.
//
//   EMF.exe [--game <dir>] [--nxm <link>] [--smoke <out.json>] [--capture <out.png>] [--select <source>:<id>]
//           [--page <tab>] [--eval <js>] [--devtools] [--nexus-fixture <dir>] [--probe-wait <ms>]   (args.h)
//
// --nxm (or a bare nxm:// argument) is how Windows hands over Nexus's Mod Manager Download links once the player let
// EMF open them (nxm_handler.h). When another EMF.exe already runs, this one sends it the link (WM_COPYDATA) and
// exits; otherwise it starts and downloads the link once the page is up. --release-nxm (no window): when this EMF.exe
// opens nxm:// links, give them back to the program that had them before (the package's uninstall.ps1 runs it before
// it deletes EMF.exe); exit 0, or 1 when Windows refused.
// --game shows that folder (not remembered). --smoke waits for the page's first render of the mod list, writes
// {rendered, count, titles, probe} to out.json and exits 0 (1 after 15 s without it); it runs off-screen without taking
// the focus and keeps WebView2's data in %TEMP%\emf-smoke-webview, away from the player's. --capture also saves a PNG
// of the page. --select and --page open that mod's details and that tab (mods, browse, warnings, emf), for captures.
// --eval runs a script in the page after that first render (the smoke test clicks a switch with it); a second later
// (--probe-wait <ms>: later) out.json gets "probe": {probe: window.emfProbe, href: location.href}. --nexus-fixture
// answers the Browse page from recorded Nexus replies in <dir> (browse.h) and its pictures from <dir>\images, with no
// network at all (the API v1 and the CDN too: v1_*.json and downloads\). --eval, --devtools, --probe-wait and
// --nexus-fixture work only with the environment variable EMF_TEST=1 (tests), so a shortcut cannot turn them on for a
// player. A smoke run keeps the Nexus key and downloads in a temporary folder (EMF_TEST_APPDATA names another one)
// and uses a registry in memory for the nxm:// setting (EMF_TEST_NXM_COMMAND seeds the current handler). A smoke run
// takes no part in handing links over, unless EMF_TEST_HANDOFF=<name> gives it a window class and mutex of its own
// (EMFApp-<name>), so the smoke test can hand a link from one test run to another without reaching a player's EMF.exe.
// Nexus Mods (bridge.h nexus_mode): without EMF's application slug from Nexus (kSsoSlug) EMF.exe makes no Nexus
// request at all (no Browse, no Check for updates, no account, no pictures from Nexus), unless the tester switch is on:
// the line personal_key=1 under [nexus] in app.ini in EMF's app-data folder (%LOCALAPPDATA%\EMF\app.ini; a smoke run
// reads the app.ini in its own app-data folder, EMF_TEST_APPDATA). It is read once at start, also by an EMF.exe
// Windows starts for an nxm:// link. A smoke run never reaches the network (recorded or refused replies) and writes
// "nexus_requests" to out.json: how many Nexus requests (API, CDN, pictures) EMF.exe answered or let through.
// The page reaches nothing outside https://emf.app/ except pictures (GET image requests) from the hosts in
// image_hosts() (browse.h); Nexus requests go through the host (bridge.h), on worker threads.
// EMF.exe lives in <game>\emf\ (never next to enshrouded.exe, where mods put proxy DLLs) and imports none of the
// proxy names (dinput8, dbghelp, dbgcore, winmm, powrprof, version, xinput1_4).
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "WebView2.h"
#include "args.h"
#include "bridge.h"
#include "browse.h"
#include "game_folder.h"
#include "locate.h"
#include "server_process.h"
#include "servers.h"
#include "text.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using namespace emf::app;

namespace {

constexpr wchar_t kTitle[] = L"EMF - Enshrouded mods";
constexpr wchar_t kHost[] = L"emf.app";
constexpr wchar_t kOrigin[] = L"https://emf.app/";
constexpr wchar_t kRuntimeUrl[] = L"https://developer.microsoft.com/microsoft-edge/webview2/";
constexpr UINT_PTR kRefreshTimer = 1, kSmokeTimer = 2, kCaptureTimer = 3, kProbeTimer = 4;
constexpr UINT kAsyncDone = WM_APP + 1;   // closures wait in UiQueue::done (the message carries nothing)
constexpr UINT kNxmArrived = WM_APP + 2;  // links from another EMF.exe wait in UiQueue::links (the message carries nothing)
constexpr UINT kWebDeferred = WM_APP + 3; // page messages that came while a dialog was open (App::deferred_web_)
constexpr wchar_t kClass[] = L"EMFApp";         // the window another EMF.exe hands nxm:// links to
constexpr wchar_t kSmokeClass[] = L"EMFAppTest"; // a smoke run's window, which never receives them
constexpr wchar_t kRunningMutex[] = L"Local\\EMF.App.Running";  // exists while a (non-test) EMF.exe runs
constexpr COLORREF kBackground = RGB(11, 16, 32);  // the page's background, so nothing flashes white

std::wstring exe_file() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::wstring(buf, n);
}

std::wstring exe_dir() {
    const std::wstring p = exe_file();
    return p.substr(0, p.find_last_of(L'\\'));
}

std::wstring env(const wchar_t* name) {
    wchar_t v[1024] = {};
    const DWORD n = GetEnvironmentVariableW(name, v, 1024);
    return n > 0 && n < 1024 ? std::wstring(v, n) : std::wstring();
}

// %TEMP%\emf-smoke-webview-<pid>: a smoke run's own folder (WebView2's data, and the Nexus key and downloads).
std::wstring smoke_dir() {
    wchar_t tmp[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    return std::wstring(tmp, n > 0 && n <= MAX_PATH ? n : 0) + L"emf-smoke-webview-" + std::to_wstring(GetCurrentProcessId());
}

// Hands an nxm:// link to an EMF.exe that already runs (its window answers WM_COPYDATA). A starting EMF.exe may not
// have its window yet, so this tries for up to five seconds. False when none took it.
bool hand_off(const std::wstring& url, const std::wstring& window_class) {
    COPYDATASTRUCT cds{};
    cds.dwData = emf::app::kHandoffTag;
    cds.cbData = static_cast<DWORD>((url.size() + 1) * sizeof(wchar_t));
    cds.lpData = const_cast<wchar_t*>(url.c_str());
    for (int attempt = 0; attempt < 50; ++attempt) {
        HWND w = nullptr;
        while ((w = FindWindowExW(nullptr, w, window_class.c_str(), nullptr)) != nullptr) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            AllowSetForegroundWindow(pid);  // so it may come to the front
            DWORD_PTR answer = 0;
            if (SendMessageTimeoutW(w, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000,
                                    &answer) &&
                answer == TRUE)
                return true;
        }
        Sleep(100);
    }
    return false;
}

std::wstring parent_of(const std::wstring& dir) {
    const size_t at = dir.find_last_of(L'\\');
    return at == std::wstring::npos ? std::wstring() : dir.substr(0, at);
}

// %LOCALAPPDATA%\EMF (made if missing): app.ini and WebView2's data; %TEMP%\EMF if Windows does not say where that is.
std::wstring app_data_dir() {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p && *p) dir = std::wstring(p) + L"\\EMF";
    CoTaskMemFree(p);
    if (dir.empty()) {
        wchar_t tmp[MAX_PATH + 1];
        const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
        if (n > 0 && n <= MAX_PATH) dir = std::wstring(tmp, n) + L"EMF";
    }
    if (!dir.empty()) CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring ini_path() { return app_data_dir() + L"\\app.ini"; }

// A value from the app.ini in `dir` ("" when it or the file is missing).
std::wstring ini_value_in(const std::wstring& dir, const wchar_t* section, const wchar_t* key) {
    wchar_t buf[64];
    GetPrivateProfileStringW(section, key, L"", buf, 64, (dir + L"\\app.ini").c_str());
    return buf;
}

std::wstring ini_read(const wchar_t* section, const wchar_t* key) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(section, key, L"", buf, 2048, ini_path().c_str());
    return buf;
}

void ini_write(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), ini_path().c_str());
}

bool write_bytes(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const BOOL ok = WriteFile(h, data, static_cast<DWORD>(size), &put, nullptr);
    CloseHandle(h);
    return ok && put == size;
}

std::wstring url_encode(const std::wstring& s) {
    std::wstring out;
    for (unsigned char c : utf8(s)) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<wchar_t>(c);
        else {
            wchar_t b[4];
            swprintf_s(b, L"%%%02X", c);
            out += b;
        }
    }
    return out;
}

// The game folder at start: --game (tests), the game this EMF.exe is installed in (<game>\emf\EMF.exe), the folder the
// player picked last time, then Steam's. A smoke run never reads the player's app.ini.
std::wstring initial_game(const std::wstring& arg, bool smoke) {
    if (!arg.empty()) return arg;
    const std::wstring around = parent_of(exe_dir());
    if (is_game_folder(around)) return around;
    if (!smoke) {
        const std::wstring picked = ini_read(L"app", L"game");
        if (is_game_folder(picked)) return picked;
    }
    return find_game_in_steam(steam_root_from_registry());
}

// EMF_TEST=1: the test-only switches (--eval, --devtools) are allowed.
bool test_mode() {
    wchar_t v[8] = {};
    return GetEnvironmentVariableW(L"EMF_TEST", v, 8) > 0 && wcscmp(v, L"1") == 0;
}

// What worker threads and other EMF.exe copies hand to the UI thread. The messages that wake it carry no pointer (any
// program on the desktop can post a message), and a worker that finishes after the window is gone still has the queue.
struct UiQueue {
    std::mutex m;
    std::deque<BridgeHost::Done> done;
    std::deque<std::wstring> links;
};

class App final : public BridgeHost {
public:
    std::shared_ptr<UiQueue> queue = std::make_shared<UiQueue>();
    HWND hwnd = nullptr;
    std::wstring game_dir, smoke_out, capture_out, select, page, eval_js, nexus_fixture, nxm;
    std::vector<std::wstring> early_links_;  // links that came before the bridge existed, in order
    std::wstring app_data_;                     // %LOCALAPPDATA%\EMF, or a smoke run's folder
    std::unique_ptr<RegistryLayer> registry_;  // the real one, or one in memory for smoke runs
    bool game_from_arg = false, devtools = false, testing = false;
    bool takes_links = false;  // answers WM_COPYDATA hand-offs (not a smoke run, unless EMF_TEST_HANDOFF)
    bool personal_key_switch = false;  // [nexus] personal_key=1 in app.ini: the tester switch (bridge.h nexus_mode)
    // Nexus requests the test transports saw and pictures the page got (smoke runs: "nexus_requests"); from any thread.
    std::shared_ptr<std::atomic<int>> nexus_requests_ = std::make_shared<std::atomic<int>>(0);
    unsigned probe_wait = 1000;  // ms from the test script to its probe
    int exit_code = 0;

    // ---- BridgeHost ----
    std::wstring game() const override { return game_dir; }
    void choose_game(const std::wstring& dir) override {
        game_dir = dir;
        if (smoke_out.empty()) ini_write(L"app", L"game", dir);
    }
    // While a file dialog runs, its own message loop would hand EMF the wake messages (finished workers, links) and the
    // page's messages in the middle of Bridge::handle. They wait instead and are handled once the dialog is closed.
    int modal_ = 0;
    std::vector<std::string> deferred_web_;
    struct ModalScope {
        App& app;
        explicit ModalScope(App& a) : app(a) { ++app.modal_; }
        ~ModalScope() {
            --app.modal_;
            for (UINT m : {kAsyncDone, kNxmArrived, kWebDeferred}) PostMessageW(app.hwnd, m, 0, 0);
        }
    };
    std::optional<std::wstring> pick_folder() override {
        ModalScope modal(*this);
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return std::nullopt;
        FILEOPENDIALOGOPTIONS opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Choose the Enshrouded folder (with enshrouded.exe or enshrouded_server.exe)");
        ComPtr<IShellItem> start;
        if (!game_dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(game_dir.c_str(), nullptr, IID_PPV_ARGS(&start))))
            dlg->SetFolder(start.Get());
        if (FAILED(dlg->Show(hwnd))) return std::nullopt;  // cancelled
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
        std::wstring dir = path;
        CoTaskMemFree(path);
        return dir;
    }
    bool open(const std::wstring& target, Open how) override {
        if (how == Open::SelectFile) {  // Explorer with the file selected
            PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(target.c_str());
            if (!pidl) return false;
            const HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ILFree(pidl);
            return SUCCEEDED(hr);
        }
        const wchar_t* verb = how == Open::Folder ? L"explore" : L"open";
        return reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, verb, target.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    }
    std::string running_reason() override { return game_running_reason(game_dir, game_processes()); }
    std::optional<std::wstring> pick_archive() override {
        ModalScope modal(*this);
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return std::nullopt;
        FILEOPENDIALOGOPTIONS opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        const COMDLG_FILTERSPEC types[] = {{L"Mod archives (*.zip, *.7z, *.rar)", L"*.zip;*.7z;*.rar"}};
        dlg->SetFileTypes(1, types);
        dlg->SetTitle(L"Install a downloaded mod file");
        ComPtr<IShellItem> downloads;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&downloads))))
            dlg->SetDefaultFolder(downloads.Get());
        if (FAILED(dlg->Show(hwnd))) return std::nullopt;  // cancelled
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
        std::wstring file = path;
        CoTaskMemFree(path);
        return file;
    }
    // Mod profiles: Import opens a profile file, Export saves one (a .json file a player can share).
    std::optional<std::wstring> pick_profile_file() override {
        ModalScope modal(*this);
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return std::nullopt;
        FILEOPENDIALOGOPTIONS opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        const COMDLG_FILTERSPEC types[] = {{L"EMF mod profiles (*.json)", L"*.json"}};
        dlg->SetFileTypes(1, types);
        dlg->SetTitle(L"Import a mod profile");
        ComPtr<IShellItem> downloads;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&downloads))))
            dlg->SetDefaultFolder(downloads.Get());
        if (FAILED(dlg->Show(hwnd))) return std::nullopt;  // cancelled
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
        std::wstring file = path;
        CoTaskMemFree(path);
        return file;
    }
    std::optional<std::wstring> pick_profile_save(const std::wstring& suggested) override {
        ModalScope modal(*this);
        ComPtr<IFileSaveDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return std::nullopt;
        FILEOPENDIALOGOPTIONS opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT);
        const COMDLG_FILTERSPEC types[] = {{L"EMF mod profiles (*.json)", L"*.json"}};
        dlg->SetFileTypes(1, types);
        dlg->SetDefaultExtension(L"json");
        dlg->SetFileName(suggested.c_str());
        dlg->SetTitle(L"Export a mod profile");
        ComPtr<IShellItem> documents;
        if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&documents))))
            dlg->SetDefaultFolder(documents.Get());
        if (FAILED(dlg->Show(hwnd))) return std::nullopt;  // cancelled
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
        std::wstring file = path;
        CoTaskMemFree(path);
        return file;
    }
    // The work runs on its own thread; what it returns comes back to the UI thread as kAsyncDone (dropped, and freed,
    // when the window is gone).
    void run_async(std::function<Done()> work) override {
        const HWND h = hwnd;
        auto q = queue;
        std::thread([h, q, work = std::move(work)]() {
            Done done;
            try {
                done = work();
            } catch (...) {
                done = nullptr;
            }
            {
                std::lock_guard lock(q->m);
                q->done.push_back(std::move(done));
            }
            PostMessageW(h, kAsyncDone, 0, 0);
        }).detach();
    }
    // The closures that are waiting, on the UI thread; their messages go to the page.
    void drain() {
        if (modal_) return;  // after the dialog (ModalScope posts again)
        std::deque<Done> ready;
        {
            std::lock_guard lock(queue->m);
            ready.swap(queue->done);
        }
        for (auto& done : ready)
            if (done && bridge_)
                for (const auto& m : done()) post(m);
    }
    // The links other EMF.exe copies handed over, on the UI thread.
    void drain_links() {
        if (modal_) return;
        std::deque<std::wstring> ready;
        {
            std::lock_guard lock(queue->m);
            ready.swap(queue->links);
        }
        for (const auto& url : ready) receive_nxm(url);
    }
    // A page message: now, or after the dialog that is open.
    void page_message(const std::string& text) {
        if (modal_) {
            if (deferred_web_.size() < 64) deferred_web_.push_back(text);
            return;
        }
        if (bridge_)
            for (const auto& reply : bridge_->handle(text)) post(reply);
    }
    void drain_web() {
        if (modal_) return;
        std::vector<std::string> ready;
        ready.swap(deferred_web_);
        for (const auto& text : ready) page_message(text);
    }
    // nullptr is WinHTTP (a player's EMF.exe); tests get recorded replies, and a smoke run without them gets refusals
    // (it never reaches the network). Each request a test transport sees is counted (nexus_requests).
    emf::nexus::Client::Transport nexus_transport() override {
        if (!nexus_fixture.empty()) return counted(fixture_transport(nexus_fixture));
        return smoke_out.empty() ? nullptr : counted(offline_v1_transport());
    }
    emf::nexus::V1Client::Transport nexus_v1_transport() override {
        if (!nexus_fixture.empty()) return counted(fixture_v1_transport(nexus_fixture));
        return smoke_out.empty() ? nullptr : counted(offline_v1_transport());
    }
    emf::nexus::Downloader::Fetch nexus_fetch() override {
        emf::nexus::Downloader::Fetch inner = !nexus_fixture.empty() ? fixture_fetch(nexus_fixture)
                                              : smoke_out.empty()   ? nullptr
                                                                    : offline_fetch();
        if (!inner) return nullptr;
        return [n = nexus_requests_, inner](const emf::nexus::StreamRequest& r, const emf::nexus::StreamSink& sink) {
            ++*n;
            return inner(r, sink);
        };
    }
    emf::nexus::Client::Transport counted(emf::nexus::Client::Transport inner) const {
        return [n = nexus_requests_, inner](const emf::nexus::HttpRequest& r) {
            ++*n;
            return inner(r);
        };
    }
    // Nexus features on (a slug, or the tester switch): bridge.h nexus_mode.
    bool nexus_on() const { return emf::app::nexus_mode(sso_slug(), personal_key_switch) != NexusMode::Off; }
    std::wstring app_data() const override { return app_data_; }
    RegistryLayer* registry() override { return registry_.get(); }
    std::wstring exe_path() const override { return exe_file(); }
    void run_on_ui(Done done) override {  // runs like a finished worker's closure
        {
            std::lock_guard lock(queue->m);
            queue->done.push_back(std::move(done));
        }
        PostMessageW(hwnd, kAsyncDone, 0, 0);
    }
    // For worker threads: holds the queue and the window handle by value, so it is safe after App is gone (the post
    // then fails and the closure is freed with the queue).
    std::function<void(Done)> ui_poster() override {
        return [q = queue, h = hwnd](Done done) {
            {
                std::lock_guard lock(q->m);
                q->done.push_back(std::move(done));
            }
            PostMessageW(h, kAsyncDone, 0, 0);
        };
    }
    bool personal_key_allowed() const override { return personal_key_switch; }
    // A link another EMF.exe handed over: to the front, then downloaded (or kept until the page listens).
    void receive_nxm(const std::wstring& url) {
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        if (bridge_ && bridge_->page_ready()) {
            for (const auto& m : bridge_->handle_nxm(utf8(url))) post(m);
        } else if (bridge_) {
            bridge_->queue_nxm(utf8(url));
        } else if (early_links_.size() < 16) {
            early_links_.push_back(url);
        }
    }
    bool test_mode() const override { return testing; }
    bool changing() const { return bridge_ && bridge_->changing(); }
    bool server_stopping() const { return bridge_ && bridge_->server_stopping(); }
    bool collecting() const { return collecting_; }
    std::wstring steam_game() override { return find_game_in_steam(steam_root_from_registry()); }
    // Save backups: the real Steam and Saved Games folders and %LOCALAPPDATA%\EMF; a smoke run only what EMF_TEST_STEAM_ROOT
    // and EMF_TEST_SAVED_GAMES name (never the player's saves) and its own app data folder.
    emf::saves::SaveRoots save_roots() override {
        if (!smoke_out.empty()) return emf::saves::with_app_data(emf::saves::default_roots(true), app_data_);
        // never the %TEMP% stand-in app_data_dir() falls back to: without %LOCALAPPDATA% EMF keeps no backups (the tab
        // says so), rather than backups Windows may clean away
        return emf::saves::with_app_data(emf::saves::default_roots(false), emf::saves::default_app_data());
    }
    // A smoke run counts only a stand-in game running from the folder shown: the player may be playing meanwhile.
    std::string saves_busy_for(const std::wstring& game) override {  // any thread: reads only what it is given
        if (smoke_out.empty()) return BridgeHost::saves_busy_for(game);
        std::vector<ProcessInfo> mine;
        for (const auto& p : game_processes())
            if (!p.path.empty() && path_inside(game, p.path)) mine.push_back(p);
        return saves_busy_reason(mine, game);
    }

    // ---- dedicated servers ----
    std::shared_ptr<ServerRunner> runner_ = std::make_shared<ServerRunner>();  // the servers this EMF.exe started
    std::optional<EmfPackage> package_;    // the EMF this EMF.exe came with (found once)
    bool closing_ = false;                 // stopping servers before the window closes
    std::vector<std::wstring> steam_servers() override { return find_servers_in_steam(steam_root_from_registry()); }
    std::vector<ProcessInfo> processes() override { return game_processes(); }
    ServerStart start_server(const std::wstring& folder, bool stop_on_close) override {
        // a smoke run's server console stays hidden, like its window
        return runner_->start(folder, stop_on_close, smoke_out.empty() ? ConsoleShow::Minimized : ConsoleShow::Hidden);
    }
    bool set_server_stop_on_close(const std::wstring& folder, bool on) override { return runner_->set_stop_on_close(folder, on); }
    bool server_started_here(unsigned long pid) override { return runner_->started_here(pid); }
    EmfPackage emf_package() override {
        if (!package_) {  // EMF_TEST_EMF_PACKAGE (tests): the folder to look from instead of EMF.exe's own
            const std::wstring from = testing ? env(L"EMF_TEST_EMF_PACKAGE") : std::wstring();
            package_ = find_emf_package(from.empty() ? exe_dir() : from);
        }
        return *package_;
    }
    // The window is closing: the servers this EMF.exe started that are set to stop are stopped first (cleanly, all at
    // once, on a worker, through the runner's own list: stop_servers_on_close), then it closes. True when that started
    // (the window closes when it is done). When a server does not stop, EMF.exe stays open and says so; it never ends
    // one (a cut save cannot be repaired).
    bool stop_servers_then_close() {
        if (closing_) return true;
        if (runner_->running(true).empty()) return false;
        closing_ = true;
        post("{\"type\":\"closing\",\"text\":\"Stopping the server before EMF closes. It saves the world first; this can take "
             "up to a minute.\"}");
        auto runner = runner_;
        const std::wstring helper = exe_file();
        run_async([this, runner, helper]() -> Done {
            std::string failed;
            for (const auto& o : stop_servers_on_close(*runner, helper))
                if (!o.stopped) failed += (failed.empty() ? "" : " ") + o.text;
            return [this, failed]() -> std::vector<std::string> {
                closing_ = false;
                if (!failed.empty())
                    return {"{\"type\":\"closing_failed\",\"text\":" + json_quote("EMF stays open: a server did not stop. " + failed) + "}"};
                if (changing() || server_stopping())  // an install, an uninstall or a Stop began meanwhile
                    return {"{\"type\":\"closing_failed\",\"text\":" +
                            json_quote("The server stopped. EMF stays open: it is busy (an install, an uninstall or a Stop); "
                                       "close it when that is done.") + "}"};
                DestroyWindow(hwnd);
                return {};
            };
        });
        return true;
    }
    // Windows ends the session (logoff, restart, shutdown): the servers set to stop get Ctrl+C at once (the server also
    // gets Windows' own logoff or shutdown signal, which it handles the same way) and EMF.exe waits a few seconds for
    // them. It never ends one: one that is still saving when Windows ends it was not cut by EMF.exe.
    void end_session() {
        const auto servers = runner_->running_servers(true);
        if (servers.empty()) return;
        ShutdownBlockReasonCreate(hwnd, L"EMF is asking the dedicated server to stop cleanly");
        std::vector<HANDLE> waits;
        for (const auto& srv : servers) {
            run_stop_helper(exe_file(), srv.pid, false, 3000, true);
            if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, srv.pid)) waits.push_back(h);
        }
        if (!waits.empty()) WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), TRUE, 4000);
        for (HANDLE h : waits) CloseHandle(h);
        ShutdownBlockReasonDestroy(hwnd);
    }
    bool copy_text(const std::string& text) override {
        std::wstring w;  // Windows' line breaks, so the text pastes into Notepad and bug forms as lines
        for (wchar_t ch : wide(text)) {
            if (ch == L'\n' && (w.empty() || w.back() != L'\r')) w += L'\r';
            w += ch;
        }
        if (!OpenClipboard(hwnd)) return false;
        EmptyClipboard();
        bool ok = false;
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
            if (void* p = GlobalLock(mem)) {
                memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            }
            if (!ok) GlobalFree(mem);  // the clipboard owns it only once it is set
        }
        CloseClipboard();
        return ok;
    }
    void rendered(int count, const std::vector<std::string>& titles) override {
        if (smoke_out.empty() || smoke_done_) return;
        smoke_done_ = true;
        smoke_json_ = "{\"rendered\":true,\"count\":" + std::to_string(count) + ",\"titles\":[";
        for (size_t i = 0; i < titles.size(); ++i) smoke_json_ += (i ? "," : "") + json_quote(titles[i]);
        smoke_json_ += "]";
        if (!eval_js.empty() && view_) {  // the test's script, then poll for it to finish (window.emfDone) up to probe_wait
            view_->ExecuteScript(eval_js.c_str(), Callback<ICoreWebView2ExecuteScriptCompletedHandler>([this](HRESULT, LPCWSTR) -> HRESULT {
                                                      probe_deadline_ = GetTickCount64() + probe_wait;
                                                      SetTimer(hwnd, kProbeTimer, 100, nullptr);
                                                      return S_OK;
                                                  }).Get());
            return;
        }
        SetTimer(hwnd, kProbeTimer, 50, nullptr);
    }
    // Polls the page for window.emfDone and collects once it is set (or the deadline passes), so the smoke test waits on
    // a real condition instead of a fixed sleep.
    void poll_probe() {
        if (!view_) return write_smoke("null");
        if (GetTickCount64() >= probe_deadline_) return collect_probe();
        const HRESULT hr = view_->ExecuteScript(
            L"(typeof window.emfDone !== 'undefined' && window.emfDone) ? 1 : 0",
            Callback<ICoreWebView2ExecuteScriptCompletedHandler>([this](HRESULT r, LPCWSTR result) -> HRESULT {
                if (SUCCEEDED(r) && result && std::wstring(result) == L"1") collect_probe();
                else SetTimer(hwnd, kProbeTimer, 100, nullptr);
                return S_OK;
            }).Get());
        if (FAILED(hr)) SetTimer(hwnd, kProbeTimer, 100, nullptr);
    }
    // What the test's script left in window.emfProbe, and where the page is now, into out.json. A read can come back
    // empty for a moment while a (cancelled) navigation settles, so it retries a few times before giving up.
    void collect_probe() {
        collecting_ = true;
        if (!view_) return write_smoke("null");
        const HRESULT hr = view_->ExecuteScript(
            L"({probe: typeof window.emfProbe === 'undefined' ? null : window.emfProbe, href: location.href})",
            Callback<ICoreWebView2ExecuteScriptCompletedHandler>([this](HRESULT r, LPCWSTR result) -> HRESULT {
                const bool got = SUCCEEDED(r) && result && *result && std::wstring(result) != L"null";
                if (got || ++collect_tries_ > 8) write_smoke(got ? utf8(result) : "null");
                else SetTimer(hwnd, kProbeTimer, 150, nullptr);
                return S_OK;
            }).Get());
        if (FAILED(hr) && ++collect_tries_ > 8) write_smoke("null");
    }
    void write_smoke(const std::string& probe) {
        KillTimer(hwnd, kSmokeTimer);
        const std::string json = smoke_json_ + ",\"nexus_requests\":" + std::to_string(nexus_requests_->load()) +
                                 ",\"probe\":" + probe + "}\n";
        exit_code = write_bytes(smoke_out, json.data(), json.size()) ? 0 : 1;
        // a moment for the frame to be painted, then the PNG (if asked) and the end
        SetTimer(hwnd, kCaptureTimer, capture_out.empty() ? 10 : 800, nullptr);
    }

    // ---- window and WebView2 ----
    void start_webview();
    // A message to the page; when it does not get there, the next refresh sends the whole state again.
    void post(const std::string& json) {
        if (!view_ || FAILED(view_->PostWebMessageAsJson(wide(json).c_str())))
            if (bridge_) bridge_->forget_state();
    }
    void refresh() {
        if (modal_) return;  // the timer fires inside a file dialog's loop too
        if (bridge_)
            if (auto s = bridge_->state_if_changed(false)) post(*s);
    }
    void resize() {
        if (!controller_) return;
        RECT rc;
        GetClientRect(hwnd, &rc);
        controller_->put_Bounds(rc);
    }
    void smoke_timeout() {
        const std::string json = smoke_done_
            ? smoke_json_ + ",\"probe\":null,\"error\":\"timed out after the first render\"}\n"
            : std::string("{\"rendered\":false,\"error\":\"the page did not report its mod list within 15 s\"}\n");
        if (!smoke_out.empty()) write_bytes(smoke_out, json.data(), json.size());
        exit_code = 1;
        DestroyWindow(hwnd);
    }
    void finish_smoke();
    void fail_runtime(HRESULT hr, const wchar_t* step);
    bool picture_request(ICoreWebView2WebResourceRequestedEventArgs* args, ICoreWebView2WebResourceRequest* req, const std::wstring& uri);
    void focus_view() {
        if (controller_ && smoke_out.empty()) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }
    // Before COM goes away: the view and its browser processes end here.
    void close() {
        bridge_.reset();
        if (controller_) controller_->Close();
        view_.Reset();
        controller_.Reset();
        env_.Reset();
    }
    void save_placement() {
        if (!smoke_out.empty()) return;
        WINDOWPLACEMENT wp{sizeof wp};
        if (!GetWindowPlacement(hwnd, &wp)) return;
        const RECT& r = wp.rcNormalPosition;
        ini_write(L"window", L"x", std::to_wstring(r.left));
        ini_write(L"window", L"y", std::to_wstring(r.top));
        ini_write(L"window", L"w", std::to_wstring(r.right - r.left));
        ini_write(L"window", L"h", std::to_wstring(r.bottom - r.top));
        ini_write(L"window", L"max", wp.showCmd == SW_SHOWMAXIMIZED ? L"1" : L"0");
    }

private:
    bool smoke_done_ = false;
    ULONGLONG probe_deadline_ = 0;  // when to stop polling for window.emfDone and collect whatever is there
    bool collecting_ = false;       // past the poll stage, now reading the probe (with retries)
    int collect_tries_ = 0;
    std::string smoke_json_;  // the smoke result up to its titles; the probe and the closing brace come last
    ComPtr<ICoreWebView2Environment> env_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> view_;
    std::unique_ptr<Bridge> bridge_;
};

App* g_app = nullptr;

void App::fail_runtime(HRESULT hr, const wchar_t* step) {
    wchar_t code[32];
    swprintf_s(code, L"0x%08X", static_cast<unsigned>(hr));
    if (!smoke_out.empty()) {
        const std::string json = "{\"rendered\":false,\"error\":" + json_quote("WebView2 " + utf8(step) + " failed: " + utf8(code)) + "}\n";
        write_bytes(smoke_out, json.data(), json.size());
    } else {
        const std::wstring text =
            std::wstring(L"EMF shows its window with the Microsoft Edge WebView2 Runtime, and it could not start it (") + step +
            L", error " + code + L").\n\nWindows 11 and current Windows 10 have it built in. If it is missing, install the "
            L"\"WebView2 Runtime\" (the Evergreen version) from Microsoft:\n\n" + kRuntimeUrl +
            L"\n\nThen start EMF.exe again. Open that page now?";
        if (MessageBoxW(hwnd, text.c_str(), kTitle, MB_YESNO | MB_ICONERROR) == IDYES)
            ShellExecuteW(nullptr, L"open", kRuntimeUrl, nullptr, nullptr, SW_SHOWNORMAL);
    }
    exit_code = 1;
    DestroyWindow(hwnd);
}

void App::finish_smoke() {
    if (capture_out.empty() || !view_) {
        DestroyWindow(hwnd);
        return;
    }
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
        exit_code = 1;
        DestroyWindow(hwnd);
        return;
    }
    const HRESULT hr = view_->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>([this, stream](HRESULT result) -> HRESULT {
            HGLOBAL mem = nullptr;
            STATSTG st{};
            bool ok = SUCCEEDED(result) && SUCCEEDED(stream->Stat(&st, STATFLAG_NONAME)) && SUCCEEDED(GetHGlobalFromStream(stream.Get(), &mem));
            if (ok) {
                const void* data = GlobalLock(mem);
                ok = data && write_bytes(capture_out, data, static_cast<size_t>(st.cbSize.QuadPart));
                GlobalUnlock(mem);
            }
            if (!ok) exit_code = 1;
            DestroyWindow(hwnd);
            return S_OK;
        }).Get());
    if (FAILED(hr)) {
        exit_code = 1;
        DestroyWindow(hwnd);
    }
}

// A picture the page may show: a GET image request to one of image_hosts(). It goes to the network, or with
// --nexus-fixture is answered from <dir>\images (404 when the fixture has no such picture). True when handled. Without
// the Nexus features (NexusMode::Off) none is: the caller answers 403, so no picture is fetched.
bool App::picture_request(ICoreWebView2WebResourceRequestedEventArgs* args, ICoreWebView2WebResourceRequest* req, const std::wstring& uri) {
    const std::string url = utf8(uri);
    if (!nexus_on() || !image_url_allowed(url)) return false;
    COREWEBVIEW2_WEB_RESOURCE_CONTEXT ctx{};
    if (FAILED(args->get_ResourceContext(&ctx)) || ctx != COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE) return false;
    LPWSTR method = nullptr;
    const bool get = SUCCEEDED(req->get_Method(&method)) && method && _wcsicmp(method, L"GET") == 0;
    CoTaskMemFree(method);
    if (!get) return false;
    ++*nexus_requests_;
    if (nexus_fixture.empty()) return true;  // to the network, as asked
    std::string bytes;
    const std::wstring file = fixture_image(nexus_fixture, url);
    ComPtr<ICoreWebView2WebResourceResponse> resp;
    if (!file.empty() && read_file(file, bytes, 16u << 20)) {
        const std::wstring ext = file.substr(file.find_last_of(L'.') + 1);
        const wchar_t* type = iequals(ext, L"png") ? L"image/png" : iequals(ext, L"gif") ? L"image/gif"
                              : iequals(ext, L"webp") ? L"image/webp" : L"image/jpeg";
        ComPtr<IStream> stream;
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size() ? bytes.size() : 1)) {
            if (void* p = GlobalLock(mem)) {
                memcpy(p, bytes.data(), bytes.size());
                GlobalUnlock(mem);
            }
            if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) GlobalFree(mem);
        }
        if (stream && SUCCEEDED(env_->CreateWebResourceResponse(stream.Get(), 200, L"OK", (std::wstring(L"Content-Type: ") + type).c_str(), &resp)))
            args->put_Response(resp.Get());
        return true;
    }
    if (SUCCEEDED(env_->CreateWebResourceResponse(nullptr, 404, L"Not in the fixture", L"", &resp))) args->put_Response(resp.Get());
    return true;
}

void App::start_webview() {
    LPWSTR version = nullptr;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    if (FAILED(hr) || !version) return fail_runtime(FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), L"finding the runtime");
    CoTaskMemFree(version);

    // a smoke run keeps its own browser data, away from the player's
    const std::wstring data = !smoke_out.empty() ? smoke_dir() : app_data_dir() + L"\\webview";
    hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || !env) {
                fail_runtime(FAILED(result) ? result : E_FAIL, L"creating the environment");
                return S_OK;
            }
            env_ = env;
            env_->CreateCoreWebView2Controller(
                hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                          [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                              if (FAILED(result) || !controller) {
                                  fail_runtime(FAILED(result) ? result : E_FAIL, L"creating the view");
                                  return S_OK;
                              }
                              controller_ = controller;
                              controller_->get_CoreWebView2(&view_);
                              ComPtr<ICoreWebView2Controller2> c2;
                              if (SUCCEEDED(controller_.As(&c2)))
                                  c2->put_DefaultBackgroundColor({255, 11, 16, 32});  // kBackground

                              ComPtr<ICoreWebView2Settings> settings;
                              view_->get_Settings(&settings);
                              settings->put_AreDevToolsEnabled(devtools);
                              settings->put_AreDefaultContextMenusEnabled(devtools);
                              settings->put_IsStatusBarEnabled(FALSE);
                              settings->put_IsWebMessageEnabled(TRUE);

                              // the page: the ui folder next to the exe, as https://emf.app/
                              const std::wstring ui = exe_dir() + L"\\ui";
                              ComPtr<ICoreWebView2_3> v3;
                              if (!is_dir(ui) || FAILED(view_.As(&v3)) ||
                                  FAILED(v3->SetVirtualHostNameToFolderMapping(kHost, ui.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS))) {
                                  if (smoke_out.empty())
                                      MessageBoxW(hwnd, (L"EMF's page is missing: the folder " + ui + L" should be next to EMF.exe.").c_str(), kTitle, MB_ICONERROR);
                                  exit_code = 1;
                                  DestroyWindow(hwnd);
                                  return S_OK;
                              }

                              // no network: every request outside https://emf.app/ is answered 403 here
                              view_->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                              view_->add_WebResourceRequested(
                                  Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                      [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                          ComPtr<ICoreWebView2WebResourceRequest> req;
                                          LPWSTR uri = nullptr;
                                          if (FAILED(args->get_Request(&req)) || FAILED(req->get_Uri(&uri))) return S_OK;
                                          const std::wstring u = uri;
                                          CoTaskMemFree(uri);
                                          if (istarts_with(u, kOrigin)) return S_OK;
                                          if (picture_request(args, req.Get(), u)) return S_OK;
                                          ComPtr<ICoreWebView2WebResourceResponse> resp;
                                          if (SUCCEEDED(env_->CreateWebResourceResponse(nullptr, 403, L"Blocked by EMF", L"", &resp)))
                                              args->put_Response(resp.Get());
                                          return S_OK;
                                      }).Get(),
                                  nullptr);
                              // the page never leaves https://emf.app/; Nexus links go to the browser
                              view_->add_NavigationStarting(
                                  Callback<ICoreWebView2NavigationStartingEventHandler>(
                                      [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                          LPWSTR uri = nullptr;
                                          if (FAILED(args->get_Uri(&uri))) return S_OK;
                                          const std::wstring u = uri;
                                          CoTaskMemFree(uri);
                                          if (istarts_with(u, kOrigin)) return S_OK;
                                          args->put_Cancel(TRUE);
                                          if (nexus_url_allowed(utf8(u))) open(u, Open::Url);
                                          return S_OK;
                                      }).Get(),
                                  nullptr);
                              view_->add_NewWindowRequested(
                                  Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                      [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                          args->put_Handled(TRUE);
                                          LPWSTR uri = nullptr;
                                          if (SUCCEEDED(args->get_Uri(&uri))) {
                                              if (nexus_url_allowed(utf8(uri))) open(uri, Open::Url);
                                              CoTaskMemFree(uri);
                                          }
                                          return S_OK;
                                      }).Get(),
                                  nullptr);
                              // a crashed page is reloaded; a crashed browser ends the app with a message
                              view_->add_ProcessFailed(
                                  Callback<ICoreWebView2ProcessFailedEventHandler>(
                                      [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                                          COREWEBVIEW2_PROCESS_FAILED_KIND kind;
                                          args->get_ProcessFailedKind(&kind);
                                          if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) fail_runtime(E_FAIL, L"running (its browser process ended)");
                                          else if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) view_->Reload();
                                          return S_OK;
                                      }).Get(),
                                  nullptr);

                              bridge_ = std::make_unique<Bridge>(*this);
                              if (!nxm.empty()) bridge_->queue_nxm(utf8(nxm));  // the link EMF.exe was started with
                              for (const auto& url : early_links_) bridge_->queue_nxm(utf8(url));  // and handed over since
                              early_links_.clear();
                              view_->add_WebMessageReceived(
                                  Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                      [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                          LPWSTR src = nullptr;
                                          if (SUCCEEDED(args->get_Source(&src))) {
                                              const bool ours = istarts_with(src, kOrigin);
                                              CoTaskMemFree(src);
                                              if (!ours) return S_OK;  // only our page talks to the host
                                          }
                                          LPWSTR msg = nullptr;
                                          if (FAILED(args->get_WebMessageAsJson(&msg)) || !msg) return S_OK;
                                          const std::string text = utf8(msg);
                                          CoTaskMemFree(msg);
                                          page_message(text);
                                          return S_OK;
                                      }).Get(),
                                  nullptr);

                              resize();
                              std::wstring url = std::wstring(kOrigin) + L"index.html";
                              url += L"?select=" + url_encode(select) + L"&tab=" + url_encode(page);
                              view_->Navigate(url.c_str());
                              focus_view();
                              SetTimer(hwnd, kRefreshTimer, 3000, nullptr);
                              return S_OK;
                          })
                          .Get());
            return S_OK;
        }).Get());
    if (FAILED(hr)) fail_runtime(hr, L"starting");
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = g_app;
    switch (msg) {
        case WM_SIZE:
            if (app) app->resize();
            return 0;
        case WM_SETFOCUS:
            if (app) app->focus_view();
            return 0;
        case WM_TIMER:
            if (!app) return 0;
            if (wp == kRefreshTimer) app->refresh();
            else if (wp == kSmokeTimer) app->smoke_timeout();
            else if (wp == kProbeTimer) {
                KillTimer(hwnd, kProbeTimer);
                if (app->collecting() || app->eval_js.empty()) app->collect_probe();
                else app->poll_probe();
            } else if (wp == kCaptureTimer) {
                KillTimer(hwnd, kCaptureTimer);
                app->finish_smoke();
            }
            return 0;
        case kAsyncDone:
            if (app) app->drain();
            return 0;
        case WM_COPYDATA: {  // an nxm:// link from another EMF.exe (a smoke run's window never takes one)
            if (!app || !app->takes_links) return FALSE;
            const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            const auto url = emf::app::decode_handoff(cds->dwData, cds->lpData, cds->cbData);
            if (!url) return FALSE;
            // handled after this message returns: the sender waits, and WebView2 must not be called from inside it
            {
                std::lock_guard lock(app->queue->m);
                if (app->queue->links.size() >= 16) return FALSE;  // a flood of links is not a player clicking
                app->queue->links.push_back(*url);
            }
            PostMessageW(hwnd, kNxmArrived, 0, 0);
            return TRUE;
        }
        case kNxmArrived:
            if (app) app->drain_links();
            return 0;
        case kWebDeferred:
            if (app) app->drain_web();
            return 0;
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {  // a sensible minimum, but never larger than the screen's work area
            const UINT dpi = GetDpiForWindow(hwnd) ? GetDpiForWindow(hwnd) : 96;
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {(std::min)(MulDiv(860, dpi, 96), static_cast<int>(mi.rcWork.right - mi.rcWork.left)),
                                  (std::min)(MulDiv(560, dpi, 96), static_cast<int>(mi.rcWork.bottom - mi.rcWork.top))};
            return 0;
        }
        case WM_CLOSE:
            if (app && app->changing()) {  // ending now would stop a copy half-way, before its record is written
                MessageBoxW(hwnd, L"EMF is busy: installing or uninstalling (a mod, or EMF into a server), or backing up or "
                                  L"restoring saves. Close it when that is done.",
                            kTitle, MB_ICONINFORMATION);
                return 0;
            }
            if (app && app->server_stopping()) {  // a Stop from the Server tab is under way
                MessageBoxW(hwnd, L"EMF is stopping a server (it saves the world first). Close EMF when that is done.", kTitle,
                            MB_ICONINFORMATION);
                return 0;
            }
            if (app && app->closing_) {  // stopping a server first: the window closes when that is done
                MessageBoxW(hwnd, L"EMF is stopping the server cleanly before it closes (the server saves the world first). "
                                  L"The window closes when that is done.", kTitle, MB_ICONINFORMATION);
                return 0;
            }
            if (app) app->save_placement();
            if (app && app->stop_servers_then_close()) return 0;  // servers set to stop with EMF.exe
            DestroyWindow(hwnd);
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;  // EMF.exe never holds up a logoff or a restart
        case WM_ENDSESSION:
            if (app && wp) app->end_session();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(app ? app->exit_code : 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// A smoke run's window: past the right edge of every screen, shown without taking the focus (verify.sh runs it while
// the player works). The page still renders there, and CapturePreview still takes its picture.
void place_smoke_window(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd) ? GetDpiForWindow(hwnd) : 96;
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 64;
    SetWindowPos(hwnd, nullptr, x, GetSystemMetrics(SM_YVIRTUALSCREEN), MulDiv(1280, dpi, 96), MulDiv(820, dpi, 96),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
}

// The window where it was last time (when that is still on a screen), else centred at a size for the screen's DPI.
void place_window(HWND hwnd, bool remembered) {
    const UINT dpi = GetDpiForWindow(hwnd) ? GetDpiForWindow(hwnd) : 96;
    if (remembered) {
        const int x = _wtoi(ini_read(L"window", L"x").c_str()), y = _wtoi(ini_read(L"window", L"y").c_str());
        const int w = _wtoi(ini_read(L"window", L"w").c_str()), h = _wtoi(ini_read(L"window", L"h").c_str());
        RECT r{x, y, x + w, y + h};
        if (w >= 400 && h >= 300 && MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            WINDOWPLACEMENT wp{sizeof wp};
            wp.rcNormalPosition = r;
            wp.showCmd = ini_read(L"window", L"max") == L"1" ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
            SetWindowPlacement(hwnd, &wp);
            return;
        }
    }
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& wa = mi.rcWork;
    const int w = (std::min)(MulDiv(1280, dpi, 96), static_cast<int>(wa.right - wa.left));
    const int h = (std::min)(MulDiv(820, dpi, 96), static_cast<int>(wa.bottom - wa.top));
    SetWindowPos(hwnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // DLLs loaded by name come from System32 only, never from this folder or the current one: WebView2's browser DLL
    // loads winmm.dll and version.dll, names that mods use for their proxies next to the game.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetDllDirectoryW(L"");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    App app;
    g_app = &app;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> list;
    for (int i = 1; argv && i < argc; ++i) list.emplace_back(argv[i]);
    if (argv) LocalFree(argv);
    emf::app::AppArgs args = emf::app::parse_app_args(list);
    // without EMF_TEST=1: a link from Windows brings nothing else along, and no test switch (--smoke writes a file) works
    emf::app::finalize_args(args, test_mode());
    const std::wstring game_arg = args.game;
    app.smoke_out = args.smoke;
    app.capture_out = args.capture;
    app.select = args.select;
    app.page = args.page;
    app.eval_js = args.eval;
    app.devtools = args.devtools;
    app.nexus_fixture = args.nexus_fixture;
    app.probe_wait = args.probe_wait;
    app.nxm = args.nxm;
    app.testing = test_mode();
    if (args.stop_server) {  // no window: Ctrl+C (or Ctrl+Break) to a dedicated server's console (server_process.h)
        const int code = send_stop_signal(args.stop_pid, args.stop_break, args.stop_shared_ok);
        CoUninitialize();
        return code;
    }
    if (args.release_nxm) {  // no window: the uninstaller gives the links back before it deletes this EMF.exe
        auto reg = windows_registry();
        const int code = nxm_owner(*reg, exe_file()).ours && !nxm_give_back(*reg, exe_file()).ok ? 1 : 0;
        CoUninitialize();
        return code;
    }
    const bool smoke_run = !app.smoke_out.empty();
    // One EMF.exe takes the links: a second one started for an nxm:// link hands it over and exits. A smoke run never
    // takes part (its window has its own class and it holds no mutex).
    std::wstring suffix = app.testing ? env(L"EMF_TEST_HANDOFF") : std::wstring();
    if (suffix.size() > 32 || suffix.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::wstring::npos)
        suffix.clear();
    const std::wstring window_class = smoke_run && suffix.empty() ? std::wstring(kSmokeClass)
                                                                  : std::wstring(kClass) + (suffix.empty() ? L"" : L"-" + suffix);
    app.takes_links = !smoke_run || !suffix.empty();
    HANDLE running = nullptr;
    if (app.takes_links) {
        running = CreateMutexW(nullptr, FALSE, (std::wstring(kRunningMutex) + (suffix.empty() ? L"" : L"-" + suffix)).c_str());
        const bool another = running && GetLastError() == ERROR_ALREADY_EXISTS;
        if (another && !app.nxm.empty() && hand_off(app.nxm, window_class)) {
            CloseHandle(running);
            CoUninitialize();
            return 0;
        }
    }
    // The Nexus key and downloads; the registry for the nxm:// setting. A smoke run never touches the player's.
    if (smoke_run) {
        const std::wstring chosen = app.testing ? env(L"EMF_TEST_APPDATA") : std::wstring();
        app.app_data_ = !chosen.empty() ? chosen : smoke_dir() + L"\\appdata";
        auto memory = std::make_unique<MemoryRegistry>();
        if (const std::wstring seed = app.testing ? env(L"EMF_TEST_NXM_COMMAND") : std::wstring(); !seed.empty())
            memory->set(L"Software\\Classes\\nxm\\shell\\open\\command", L"", seed);
        app.registry_ = std::move(memory);
    } else {
        app.app_data_ = app_data_dir();
        app.registry_ = windows_registry();
    }
    // The tester switch for personal Nexus API keys, from the app.ini next to the key (a smoke run's own).
    app.personal_key_switch = !app.app_data_.empty() && ini_value_in(app.app_data_, L"nexus", L"personal_key") == L"1";

    std::string tools;  // the known-tools table shipped next to EMF.exe, for folders without EMF
    if (read_file(exe_dir() + L"\\known_tools.txt", tools, 1u << 20)) set_fallback_known_tools(tools);
    app.game_from_arg = !game_arg.empty();
    app.game_dir = initial_game(game_arg, !app.smoke_out.empty());

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = CreateSolidBrush(kBackground);
    const bool smoke = smoke_run;
    wc.lpszClassName = window_class.c_str();
    RegisterClassExW(&wc);
    app.hwnd = CreateWindowExW(smoke ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : 0, wc.lpszClassName, kTitle,
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820, nullptr, nullptr, inst, nullptr);
    if (!app.hwnd) return 1;
    const BOOL dark = TRUE;  // a dark title bar to match the page (Windows 10 20H1 and later)
    DwmSetWindowAttribute(app.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    if (smoke) place_smoke_window(app.hwnd);
    else place_window(app.hwnd, true);
    UpdateWindow(app.hwnd);

    if (!app.smoke_out.empty()) SetTimer(app.hwnd, kSmokeTimer, 15000, nullptr);
    app.start_webview();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    app.close();
    g_app = nullptr;
    if (running) CloseHandle(running);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
