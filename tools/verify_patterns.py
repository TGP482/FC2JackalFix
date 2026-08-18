#!/usr/bin/env python3
"""Re-count every byte pattern used by FC2JackalFix against a given Dunia.dll.

Patterns are matched against the raw .text section with base-relocated bytes forced to
wildcards, which is what the loader would change if the module ever rebases. A pattern that
matches anything other than exactly once needs lengthening before it ships.

    python tools/verify_patterns.py "E:\\...\\Far Cry 2\\bin\\Dunia.dll"

Requires pefile (pip install pefile).
"""

import re
import sys

import pefile

PATTERNS = [
    ("skipintro branch",       "80 BE 64 01 00 00 00 75 1B A1 ? ? ? ? 83 B8 90 00 00 00 00 76 11", 21),
    ("title screen state cmp", "8B 86 6C 01 00 00 83 E8 01 74 ? 8B 86 68 01 00 00", 8),
    ("fov fFOV setter",        "F3 0F 10 44 24 04 F3 0F 59 05 ? ? ? ? F3 0F 11 41 70 C2 04 00", 0),
    ("fov viewmodel near pass","D9 86 28 02 00 00 D9 1C 24 E8 ? ? ? ? D9 45 14", 9),
    ("settings singleton ptr", "A1 ? ? ? ? 83 B8 B4 00 00 00 00 F3 0F 10 88 B0 00 00 00 8B 88 B8 00 00 00", 1),
    ("jackal tapes fix",      "8B 4C 24 0C 8B 15 ? ? ? ? 90 80 7E 74 00 75 0A 3B CA 75 0A 80 7E 75 00 75 16", 16),
    ("predecessor tapes",     "8B 49 0C 85 C9 74 16 8B 44 24 04 50 E8 ? ? ? ? 84 C0 74 08 B8 01 00 00 00 C2 04 00", 5),
    ("machetes unlock",       "75 02 B3 01 8B 54 24 08 52 FF 15 ? ? ? ? 8A C3 5B 83 C4 10 C3", 15),
    ("mesh highlight lookup",  "E8 ? ? ? ? 53 8B CA 68 ? ? ? ? 89 56 6C 51 8B D0 89 46 68", 5),
    ("item highlight gate",    "8B 44 24 40 8B 4C 24 44 8B 54 24 18 89 43 10 89 4B 14 F6 82 9C 00 00 00 04 0F 84", 24),
    ("marker set archetype",   "8B 44 24 04 83 F8 09 56 8B F1 77 38 8B 4C 24 0C 6A FF", 4),
    ("archetype slot visit 68", "8B C1 8B 50 10 8B 4C 24 08 56 8B 31 57 8D 3C D5 00 00 00 00 2B FA 8B 50 0C 8D 14 BA 03 54 24 0C 83 C0 04 52 50 8B 46 68 FF D0 5F 5E C2 08 00", 0),
    ("archetype slot visit dc", "8B C1 8B 50 10 8B 4C 24 08 56 8B 31 57 8D 3C D5 00 00 00 00 2B FA 8B 50 0C 8D 14 BA 03 54 24 0C 83 C0 04 52 50 8B 86 DC 00 00 00 FF D0 5F 5E C2 08 00", 0),
    ("sign colour select",     "8B 74 24 18 85 F6 7C 1C 3B 75 70 7D 11 8B 45 6C 8B 0C B0 83 C1 02 C1 E1 04 03 CD 51 EB 0A 8D 55 30 52 EB 04 8D 45 20 50", 6),
    ("hit indicator loop head", "F3 0F 10 00 F3 0F 10 48 04 F3 0F 10 50 08 33 FF 8D 83 20 02 00 00", 14),
    ("fcb name hash",          "8B 44 24 04 85 C0 56 8B F1 74 29 80 38 00 74 24 80 7C 24 0C 00 50 74 0E E8 ? ? ? ? 83 C4 04 89 06 5E C2 0C 00", 0),
    ("entity library index",   "55 8B EC 83 E4 F8 83 EC 3C F6 05 ? ? ? ? 01 53 56 57 89 4C 24 1C 75 1B", 0),
    ("pawn input update",      "33 C0 38 44 24 08 56 8B F1 74 0A 88 46 04 88 46 05 5E C2 08 00 8B 4E 20 3B C8 74 4A", 0),
    ("pawn set pov axis",      "83 EC 08 56 57 8B 7C 24 14 83 7F 0C 02 8B F1 0F 82 07 01 00 00 0F 57 C9 F3 0F 11 4E 10 F3 0F 11 4E 14", 0),
    ("pawn get vehicle",       "83 EC 08 8B 4C 24 0C 8B 01 8B 50 7C 53 FF D2 83 CB FF B9 01 00 00 00", 0),
    ("decal desc post load",   "55 8B EC 83 E4 F0 83 EC 24 53 56 57 8B F9 83 7F 24 00 75 26 8D 44 24 20 8D 8F D0 00 00 00", 0),
    ("menu cursor delta X",   "F3 0F 10 86 88 00 00 00 0F 28 E0 F3 0F 59 E2 F3 0F 59 E3 F3 0F 2C CC", 19),
    ("menu cursor delta Y",   "F3 0F 59 C1 F3 0F 59 C3 C1 E8 10 F3 0F 2C D0 66 2B C2", 11),
    ("mouse speed cap",       "F3 0F 10 4F 0C F3 0F 5E C2 F3 0F 59 47 10 F3 0F 59 C4 0F 28 F8 0F 54 FD 0F 2F F9 76", 5),
    ("look sensitivity spill","F3 0F 10 88 B0 00 00 00 8B 88 B8 00 00 00 F3 0F 10 15 ? ? ? ? F3 0F 11 4C 24 08", 22),
    ("sprint turn modifier",  "F3 0F 10 80 A4 00 00 00 F3 0F 59 46 14 F3 0F 11 46 14", 8, 2),
    ("pad axis emitter",      "0F 28 C8 0F 54 CA F3 0F 10 51 08 56 0F 2F D1 0F 57 C9 57", 12),
    ("aim assist sticky",     "80 BE 5D 01 00 00 00 74 15 8D 4C 24 24 E8 ? ? ? ? 8D 4C 24 24 51 8B CE E8 ? ? ? ?", 9),
    ("aim assist followEnemy", "80 BE 5C 01 00 00 00 74 15 8D 4C 24 24 E8 ? ? ? ? 8D 54 24 24 52 8B CE E8 ? ? ? ?", 9),
    ("aim assist shootCorrect","80 BE 5F 01 00 00 00 74 15 8D 4C 24 24 E8 ? ? ? ? 8D 44 24 24 50 8B CE E8 ? ? ? ?", 9),
    ("aim assist ironSight",   "80 BE 5E 01 00 00 00 74 15 8D 4C 24 24 E8 ? ? ? ? 8D 4C 24 24 51 8B CE E8 ? ? ? ?", 9),
    ("render config float prop", "56 8B C1 8B 70 0C 03 74 24 08 8B 4C 24 0C 8B 11 83 C0 04 56 50 8B 82 0C 01 00 00 FF D0 5E C2 08 00", 0),
    ("render config int prop",   "56 8B C1 8B 70 0C 03 74 24 08 8B 4C 24 0C 8B 11 83 C0 04 56 50 8B 82 14 01 00 00 FF D0 5E C2 08 00", 0),
    ("realtree instance alloc",   "BF 90 18 00 00 39 BE 84 0B 00 00 73 33 8B 86 80 0B 00 00 3B C3 74 09 50 E8 ? ? ? ? 83 C4 04 53 68 80 C4 00 00 E8 ? ? ? ? 83 C4 08 89 86 80 0B 00 00", 1),
    ("realtree job caps",         "89 4E 14 89 46 1C 8B 44 24 20 C7 06 00 04 00 00 C7 46 04 24 06 00 00 8B 8D 94 0B 00 00", 19),
    ("realtree job slice",        "68 20 31 00 00 8B CB 69 C9 20 31 00 00 03 8D 80 0B 00 00 51 8B CF E8", 1),
    ("realtree fill job input",   "8B 86 80 0B 00 00 68 80 C4 00 00 50 8B CF E8", 7),
    ("realtree batches desc",     "89 9E 28 0B 00 00 89 96 34 0B 00 00 C7 86 38 0B 00 00 90 18 00 00", 18),
    ("realtree merge array",      "8B 8E 80 0B 00 00 68 80 C4 00 00 51 8B CF E8", 7),
    ("realtree merge scalars",    "68 00 04 00 00 8B CF E8 ? ? ? ? 68 90 18 00 00 8B CF E8", 13),
    ("show fps global",        "D9 5C 24 0C 83 3D ? ? ? ? 00 74 ? 83 3D ? ? ? ? 00 8B 35 ? ? ? ? 75 07 33 C9", 6),
    ("render device global",  "8B 0D ? ? ? ? 8B 01 8B 90 EC 00 00 00 FF D2", 0),
    ("d3d9 present call site", "8B 46 38 8B 08 83 C4 08 53 52 8B 54 24 24 52 8B 54 24 2C 52 50 8B 41 44 FF D0", 0),
    ("set resolution entry",   "81 EC 48 02 00 00 53 55 56 57 33 ED 55 8D 84 24 B4 00 00 00", 0),.
    ("present params write",   "8B 56 1C 89 15 ? ? ? ? 8B 46 20 A3 ? ? ? ? 8A 4D 08 F6 D9", 0x11),
    ("rt wrapper surface set", "8B 44 24 04 89 41 1C C2 04 00 CC CC CC CC CC CC 56 57 8B F9 33 F6 39 77", 0),
    ("renderer frame end",     "81 EC 80 00 00 00 53 55 56 8B F1 8B 46 78 8B 48 14 8B 41 1C 8B 10 57 50", 0),
    ("borderless switch",      "85 C0 8B 84 24 50 04 00 00 0F 95 C1 3B C3 88 4C 24 30", 0),
    ("display mode flag",      "39 58 28 89 5C 24 78 0F 95 C1 39 58 2C 88 4C 24 30 0F 95 C0 3A CB", 0x0D),
    ("window sizer entry",     "83 EC 10 8B 44 24 18 56 8B 74 24 18 57 50 56 FF 15 ? ? ? ?", 0),
    ("setresolution cfg load", "8B AC 24 64 02 00 00 80 7D 08 00 74 24 8B 8C 24 5C 02 00 00", 0),
    ("windowed resize demand", "2B 7C 24 28 2B 6C 24 2C 89 44 24 18 8B 01 8B 90 34 01 00 00 FF D2 84 C0 75 75 81 E3 00 00 00 80", 0x1A),
    ("present params tail",    "8B 94 24 7C 02 00 00 8B 84 24 78 02 00 00 8B 8C 24 74 02 00 00", 0),
    ("maximized showwindow",   "8B 51 08 39 5A 2C 74 0A A1 ? ? ? ? 6A 03 50 EB 09", 0),
    ("pre-reset release pass", "A1 ? ? ? ? 83 EC 18 53 55 56 8B 35 ? ? ? ? 8D 04 80 57 8D 3C 86 3B F7 8B D9 C6 05 ? ? ? ? 00 74 10", 0),
]


def to_regex(pattern):
    out = b""
    for token in pattern.split():
        out += b"." if token == "?" else re.escape(bytes([int(token, 16)]))
    return re.compile(out, re.S)


def wildcard_indices(pattern):
    return {i for i, token in enumerate(pattern.split()) if token == "?"}


def main(path):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]]
    )

    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
    data = text.get_data()
    text_va = base + text.VirtualAddress

    relocs = {
        r.rva
        for block in getattr(pe, "DIRECTORY_ENTRY_BASERELOC", [])
        for r in block.entries
        if r.type != 0
    }

    print(f"{path}\n  image base {base:#x}  .text RVA {text.VirtualAddress:#x} "
          f"vsize {text.Misc_VirtualSize:#x}  relocs {len(relocs)}\n")

    failures = 0
    for name, pattern, patch_off, *rest in PATTERNS:
        expected = rest[0] if rest else 1
        hits = [m.start() for m in to_regex(pattern).finditer(data)]
        status = "OK  " if len(hits) == expected else "FAIL"
        if len(hits) != expected:
            failures += 1

        line = f"[{status}] {name:26s} matches={len(hits)}"
        if expected != 1:
            line += f"/{expected}"
        if hits:
            for hit in hits:
                va = text_va + hit
                line += f"  at {va:#x}"
                if patch_off is not None:
                    line += f"  patch site {va + patch_off:#x}"
        print(line)

        if hits:
            start_rva = text.VirtualAddress + hits[0]
            width = len(pattern.split())
            wild = wildcard_indices(pattern)
            leaked = [
                hex(start_rva + i)
                for i in range(width)
                if start_rva + i in relocs and i not in wild
            ]
            if leaked:
                failures += 1
                print(f"         relocated bytes not wildcarded: {leaked}")

    print()
    if failures:
        print(f"{failures} problem(s) found - do not ship against this build as-is.")
    else:
        print("All patterns matched the expected number of times and are relocation-safe.")
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
