#pragma once
// The debug bridge's switch: on only with [debug] bridge=1 in emf\emf_core.ini (any other value is off).
#include <windows.h>

#include <string>

namespace emf::core::bridge {

inline bool bridge_switch_on(const std::wstring& ini_path) {
    return GetPrivateProfileIntW(L"debug", L"bridge", 0, ini_path.c_str()) == 1;
}

}  // namespace emf::core::bridge
