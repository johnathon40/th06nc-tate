// Routes Dear ImGui asserts to th06nc-tate-log.txt instead of stopping the game.
#pragma once
#ifdef __cplusplus
extern "C"
#endif
void tate_assert_fail(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) do { if (!(_EXPR)) tate_assert_fail(#_EXPR, __FILE__, __LINE__); } while (0)
