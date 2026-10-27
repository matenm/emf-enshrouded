#include "account_store.h"

#include <windows.h>
#include <dpapi.h>

#include <cmath>
#include <filesystem>

#include "core/json.h"
#include "json_out.h"
#include "text.h"

namespace emf::app {

namespace {

using core::json::Value;
using VT = Value::Type;

// EMF's entropy: another program running as the same user cannot unprotect the blob by accident with an empty one.
constexpr char kEntropy[] = "EMF Nexus API key v1";

DATA_BLOB entropy_blob() {
    DATA_BLOB e;
    e.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy));
    e.cbData = static_cast<DWORD>(sizeof kEntropy - 1);
    return e;
}

void wipe(std::string& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    s.clear();
}

std::string jstr(const Value& v, const char* k) {
    const Value* x = v.get(k);
    return x && x->is(VT::String) ? x->s : std::string();
}

}  // namespace

std::optional<std::string> dpapi_protect(const std::string& plain) {
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()));
    in.cbData = static_cast<DWORD>(plain.size());
    DATA_BLOB ent = entropy_blob();
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"EMF Nexus key", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return std::nullopt;
    std::string blob(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return blob;
}

std::optional<std::string> dpapi_unprotect(const std::string& blob) {
    if (blob.empty()) return std::nullopt;
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()));
    in.cbData = static_cast<DWORD>(blob.size());
    DATA_BLOB ent = entropy_blob();
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return std::nullopt;
    std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

bool save_account(const std::wstring& path, const StoredAccount& a) {
    std::string json = Obj()
                           .str("key", a.key).num("user_id", static_cast<double>(a.user_id)).str("name", a.name)
                           .boolean("premium", a.premium).boolean("supporter", a.supporter).str("checked_at", a.checked_at)
                           .text();
    const auto blob = dpapi_protect(json);
    wipe(json);
    if (!blob) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(lp(tmp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    // on the disk before the rename, so a crash or a power cut leaves the old key or the new one, never a torn file
    const BOOL ok = WriteFile(h, blob->data(), static_cast<DWORD>(blob->size()), &put, nullptr) && put == blob->size() &&
                    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || !MoveFileExW(lp(tmp).c_str(), lp(path).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(lp(tmp).c_str());
        return false;
    }
    return true;
}

AccountFile read_account_file(const std::wstring& path) {
    AccountFile f;
    DeleteFileW(lp(path + L".tmp").c_str());  // EMF ended while saving: the old file (or none) is the real one
    std::string blob;
    if (!read_file(path, blob, 64 * 1024)) return f;
    f.status = AccountFile::Status::Unreadable;
    if (blob.empty()) return f;
    auto plain = dpapi_unprotect(blob);
    if (!plain) {
        if (GetLastError() == static_cast<DWORD>(NTE_BAD_KEY_STATE)) f.status = AccountFile::Status::OtherUser;
        return f;
    }
    const auto doc = core::json::parse(*plain);
    wipe(*plain);
    if (!doc.value || !doc.value->is(VT::Object)) return f;
    const Value& v = *doc.value;
    StoredAccount a;
    a.key = jstr(v, "key");
    if (a.key.empty()) return f;
    if (const Value* id = v.get("user_id"); id && id->is(VT::Number) && std::isfinite(id->n)) a.user_id = static_cast<int64_t>(id->n);
    a.name = jstr(v, "name");
    const Value* p = v.get("premium");
    a.premium = p && p->is(VT::Bool) && p->b;
    const Value* s = v.get("supporter");
    a.supporter = s && s->is(VT::Bool) && s->b;
    a.checked_at = jstr(v, "checked_at");
    f.status = AccountFile::Status::Ok;
    f.account = std::move(a);
    return f;
}

std::optional<StoredAccount> load_account(const std::wstring& path) { return read_account_file(path).account; }

bool delete_account(const std::wstring& path) {
    DeleteFileW(lp(path + L".tmp").c_str());
    if (DeleteFileW(lp(path).c_str())) return true;
    return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
}

}  // namespace emf::app
