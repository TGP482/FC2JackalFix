#!/usr/bin/env python3
"""Re-count every byte pattern FC2JackalFix uses against one or more Dunia.dll builds.

Patterns are read straight out of source/*.ixx, so the list cannot go stale. They are matched
against the raw .text section, and any byte a base relocation would rewrite is reported when it is
not wildcarded. A pattern that matches more than once needs lengthening before it ships.

    python tools/verify_patterns.py "...\\Far Cry 2\\bin\\Dunia.dll" "...\\GOG\\bin\\Dunia.dll"

Zero matches is not always a fault: the few sites where the retail builds diverge carry one pattern
per build, so its sibling on a neighbouring line is what matches there. Read a zero against the
other build's column before calling it a miss.

Requires pefile (pip install pefile).
"""

import pathlib
import re
import sys

import pefile

SOURCE = pathlib.Path(__file__).resolve().parent.parent / "source"

# A quoted run of hex bytes and ? wildcards, long enough not to be prose.
PATTERN_RE = re.compile(r'"((?:[0-9A-Fa-f?]{1,2} ){5,}[0-9A-Fa-f?]{1,2})"')

# Dunia.dll's PE TimeDateStamp, which is what the fix itself uses to tell the builds apart.
KNOWN_BUILDS = {0x4AAE9636: "Steam", 0x49FB4BF6: "GOG"}


def patterns():
    for path in sorted(SOURCE.glob("*.ixx")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), 1):
            for match in PATTERN_RE.finditer(line):
                yield f"{path.name}:{number}", match.group(1)


def to_regex(pattern):
    return re.compile(b"".join(b"." if t == "?" else re.escape(bytes([int(t, 16)])) for t in pattern.split()), re.S)


def load(path):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]])

    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")

    return {
        "label": KNOWN_BUILDS.get(pe.FILE_HEADER.TimeDateStamp, "unknown"),
        "stamp": pe.FILE_HEADER.TimeDateStamp,
        "data": text.get_data(),
        "rva": text.VirtualAddress,
        "va": pe.OPTIONAL_HEADER.ImageBase + text.VirtualAddress,
        "relocs": {r.rva for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", []) for r in block.entries if r.type != 0},
    }


def main(paths):
    builds = [load(path) for path in paths]
    for build, path in zip(builds, paths):
        print(f"{build['label']:7s} 0x{build['stamp']:08X}  {path}")
    print()

    problems = 0
    for where, pattern in patterns():
        wild = {i for i, t in enumerate(pattern.split()) if t == "?"}
        width = len(pattern.split())
        cells = []

        for build in builds:
            hits = [m.start() for m in to_regex(pattern).finditer(build["data"])]
            if len(hits) > 1:
                problems += 1

            cells.append(f"{build['label']}={len(hits)}" + (f" @{build['va'] + hits[0]:#x}" if len(hits) == 1 else ""))

            if not hits:
                continue

            start = build["rva"] + hits[0]
            leaked = [f"{start + i:#x}" for i in range(width) if start + i in build["relocs"] and i not in wild]
            if leaked:
                problems += 1
                cells.append(f"[{build['label']} relocated bytes not wildcarded: {', '.join(leaked)}]")

        print(f"{where:34s} {'  '.join(cells)}")

    print()
    print(f"{problems} problem(s) found." if problems else "Nothing matched more than once, and no relocated byte is left unwildcarded.")
    return 1 if problems else 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
