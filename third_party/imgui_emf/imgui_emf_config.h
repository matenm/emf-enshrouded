#pragma once
// EMF's Dear ImGui configuration (IMGUI_USER_CONFIG). A failed IM_ASSERT raises a structured exception instead of being
// compiled out (release) or ending the game (debug): the overlay's fault guard catches it and turns the overlay off.
#ifdef __cplusplus
extern "C" {
#endif
void emf_imgui_assert_failed(const char* expr, const char* file, int line);
const char* emf_imgui_last_assert(void);
#ifdef __cplusplus
}
#endif
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : emf_imgui_assert_failed(#_EXPR, __FILE__, __LINE__))
