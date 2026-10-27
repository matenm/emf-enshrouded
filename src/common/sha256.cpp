#include "sha256.h"

#include <windows.h>
#include <bcrypt.h>

#include <vector>

namespace emf {
namespace {
class Hasher {
public:
    Hasher() {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return;
        if (BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) != 0) hash_ = nullptr;
    }
    ~Hasher() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    bool ok() const { return hash_ != nullptr; }
    bool update(const void* data, size_t size) {
        return BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0) == 0;
    }
    std::string finish() {
        unsigned char digest[32];
        if (BCryptFinishHash(hash_, digest, sizeof(digest), 0) != 0) return {};
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (unsigned char b : digest) {
            out += hex[b >> 4];
            out += hex[b & 15];
        }
        return out;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};
}  // namespace

std::string sha256_hex(const void* data, size_t size) {
    Hasher h;
    if (!h.ok() || !h.update(data, size)) return {};
    return h.finish();
}

std::string sha256_file(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    Hasher h;
    std::vector<unsigned char> buf(1 << 20);
    bool good = h.ok();
    DWORD got = 0;
    while (good && ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got > 0)
        good = h.update(buf.data(), got);
    CloseHandle(f);
    return good ? h.finish() : std::string{};
}

}  // namespace emf
