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

# name, pattern, offset of the byte(s) the fix actually touches (None = reference only)
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
    ("menu cursor delta X",   "F3 0F 10 86 88 00 00 00 0F 28 E0 F3 0F 59 E2 F3 0F 59 E3 F3 0F 2C CC", 19),
    ("menu cursor delta Y",   "F3 0F 59 C1 F3 0F 59 C3 C1 E8 10 F3 0F 2C D0 66 2B C2", 11),
    ("mouse speed cap",       "F3 0F 10 4F 0C F3 0F 5E C2 F3 0F 59 47 10 F3 0F 59 C4 0F 28 F8 0F 54 FD 0F 2F F9 76", 5),
    ("render device global",  "8B 0D ? ? ? ? 8B 01 8B 90 EC 00 00 00 FF D2", 0),
    ("d3d9 present call site", "8B 46 38 8B 08 83 C4 08 53 52 8B 54 24 24 52 8B 54 24 2C 52 50 8B 41 44 FF D0", 0),
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
    for name, pattern, patch_off in PATTERNS:
        hits = [m.start() for m in to_regex(pattern).finditer(data)]
        status = "OK  " if len(hits) == 1 else "FAIL"
        if len(hits) != 1:
            failures += 1

        line = f"[{status}] {name:26s} matches={len(hits)}"
        if hits:
            va = text_va + hits[0]
            line += f"  at {va:#x}"
            if patch_off is not None:
                line += f"  patch site {va + patch_off:#x}"
        print(line)

        # A relocated byte that is not wildcarded makes the pattern machine-dependent.
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
        print("All patterns unique and relocation-safe.")
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
