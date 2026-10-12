#!/usr/bin/env python3
"""Overlay only the obsolete narrow ctype fallbacks when Bionic provides them."""
import argparse
from pathlib import Path
import re


def prepare(headers: Path, overlay: Path):
    relative = Path('support/xlocale/__posix_l_fallback.h')
    text = (headers / relative).read_text()
    # These 14 definitions conflict with modern Bionic ctype.h. Keep wide
    # character, collation and time fallbacks exactly as shipped by AOSP.
    functions = ('isalnum_l', 'isalpha_l', 'isblank_l', 'iscntrl_l', 'isdigit_l',
                 'isgraph_l', 'islower_l', 'isprint_l', 'ispunct_l', 'isspace_l',
                 'isupper_l', 'isxdigit_l', 'toupper_l', 'tolower_l')
    for name in functions:
        pattern = (r'inline _LIBCPP_INLINE_VISIBILITY int ' + name
                   + r'\(int c, locale_t\) \{\n  return ::\w+\(c\);\n\}\n')
        text, count = re.subn(pattern, '', text)
        if count != 1:
            raise ValueError(f'expected one pinned AOSP definition of {name}, got {count}')
    destination = overlay / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(text)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('headers', type=Path)
    parser.add_argument('overlay', type=Path)
    args = parser.parse_args()
    prepare(args.headers, args.overlay)
