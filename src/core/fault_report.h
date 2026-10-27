#pragma once
#include <functional>
#include <string>

namespace emf::core {

// File name of the module (DLL/EXE) that contains `addr`, e.g. "my_mod.dll", or "unknown module".
std::string module_name_of(const void* addr);

// Where fault reports go (the core logs them). Set once at start-up; nullptr turns reporting off.
void set_fault_reporter(std::function<void(const std::string&)> reporter);
// Reports "<what> in <module of callback> faulted and was disabled".
void report_fault(const std::string& what, const void* callback);

}  // namespace emf::core
