/* SPDX-License-Identifier: Apache-2.0 */
/* Compile with just the NDK C headers: do not pull in libc++ fallbacks. */
#include <ctype.h>
#include <locale.h>
#ifdef WNV_PROBE_CTYPE_L_FALLBACKS
#define WNV_CTYPE_L_FALLBACK(name) \
    inline int name##_l(int c, locale_t) { return ::name(c); }
WNV_CTYPE_L_FALLBACK(isalnum)
WNV_CTYPE_L_FALLBACK(isalpha)
WNV_CTYPE_L_FALLBACK(isblank)
WNV_CTYPE_L_FALLBACK(iscntrl)
WNV_CTYPE_L_FALLBACK(isdigit)
WNV_CTYPE_L_FALLBACK(isgraph)
WNV_CTYPE_L_FALLBACK(islower)
WNV_CTYPE_L_FALLBACK(isprint)
WNV_CTYPE_L_FALLBACK(ispunct)
WNV_CTYPE_L_FALLBACK(isspace)
WNV_CTYPE_L_FALLBACK(isupper)
WNV_CTYPE_L_FALLBACK(isxdigit)
WNV_CTYPE_L_FALLBACK(toupper)
WNV_CTYPE_L_FALLBACK(tolower)
#undef WNV_CTYPE_L_FALLBACK
#endif
int (*const hwc_ctype_l_functions[])(int, locale_t) = {
    isalnum_l, isalpha_l, isblank_l, iscntrl_l, isdigit_l, isgraph_l,
    islower_l, isprint_l, ispunct_l, isspace_l, isupper_l, isxdigit_l,
    toupper_l, tolower_l,
};
