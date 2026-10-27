// A helper DLL that a folder mod ships next to its own DLL (tests/test_mod_loading.cpp).
extern "C" __declspec(dllexport) int emf_test_helper_value() { return 42; }
