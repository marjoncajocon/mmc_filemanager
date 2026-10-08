/* fwin.h -- <windows.h> plus what tcc's trimmed Windows headers lack.
**
** Include this instead of <windows.h> in Windows-only code. With zig/clang
** (mingw-w64 headers) it is just windows.h.
*/
#ifndef FWIN_H
#define FWIN_H

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef __TINYC__
#  ifndef CP_UTF8
#    define CP_UTF8 65001
#  endif
WINBASEAPI int WINAPI MultiByteToWideChar(UINT, DWORD, LPCSTR, int, LPWSTR, int);
WINBASEAPI int WINAPI WideCharToMultiByte(UINT, DWORD, LPCWSTR, int, LPSTR, int, LPCSTR, LPBOOL);
WINBASEAPI ULONGLONG WINAPI GetTickCount64(void);
#  ifndef FIND_FIRST_EX_LARGE_FETCH
#    define FIND_FIRST_EX_LARGE_FETCH 2
#  endif
/* tcc's FINDEX_INFO_LEVELS has no FindExInfoBasic (value 1). */
#  define FindExInfoBasic ((FINDEX_INFO_LEVELS)1)
int __cdecl _snwprintf(wchar_t *, size_t, const wchar_t *, ...);
#endif

#endif
