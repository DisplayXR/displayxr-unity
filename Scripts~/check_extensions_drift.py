#!/usr/bin/env python3
# Copyright 2026, DisplayXR contributors
# SPDX-License-Identifier: Apache-2.0
"""Check native~/displayxr_extensions.h against the published XR_DXR_* headers.

The plugin has no source dependency on the runtime: displayxr_extensions.h is a
hand-written mirror carrying only the constants, structs and entry points the
provider uses. That is deliberate, but a mirrored value that drifts from the
published one is a silent wire break (the runtime reads a struct type or a
field the plugin never meant). This check fetches nothing itself; CI clones
DisplayXR/displayxr-extensions at a pinned sha and passes its include dir.

What must hold (a failure exits 1):
  - every XrStructureType / XrResult / other extended-enum value the mirror
    #defines with the same name as a published one has the same number;
  - every mirrored XR_TYPE_*_DXR name exists in the published headers;
  - every mirrored XR_DXR_* extension-name string is a published extension;
  - every mirrored struct that also exists upstream has the same members, in
    the same order, with the same types (so the same layout);
  - every mirrored xr*DXR entry point / PFN exists upstream;
  - a mirrored SPEC_VERSION is not NEWER than the published one.

What is allowed: the mirror covering only a subset, and a mirrored SPEC_VERSION
that LAGS the published one (it records the version the mirror was last
reconciled against). Lags are printed as notes, never failures.

    python3 Scripts~/check_extensions_drift.py <published-include-dir>
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

MIRROR = Path(__file__).resolve().parent.parent / "native~" / "displayxr_extensions.h"

ENUM_RE = re.compile(r"^\s*#\s*define\s+(\w+)\s+\(\s*\(\s*(Xr\w+)\s*\)\s*(-?\d+)\s*\)", re.M)
SPEC_RE = re.compile(r"^\s*#\s*define\s+(XR_DXR_\w+?)_SPEC_VERSION\s+(\d+)", re.M)
EXT_NAME_RE = re.compile(r'^\s*#\s*define\s+\w+_EXTENSION_NAME\s+"(XR_DXR_\w+)"', re.M)
STRUCT_RE = re.compile(r"typedef\s+struct\s+(Xr\w+DXR)\s*\{(.*?)\}\s*\1\s*;", re.S)
FUNC_RE = re.compile(r"\b(?:PFN_)?(xr[A-Z]\w*DXR)\b")

# Layout-identical spellings: the mirror keeps platform types opaque so it
# needs no Wayland / Xlib headers. Any object pointer is pointer-sized, and the
# published xlib header's window macro is Xlib's Window, an unsigned long.
TYPE_EQUIV = [
    (re.compile(r"^(?:const\s+)?(?:struct\s+)?\w+\*"), "void*"),
    (re.compile(r"^XR_XLIB_WINDOW_TYPE_\b"), "unsigned long"),
]

# Drift that is known, harmless and owned elsewhere. Each entry is printed on
# every run so it stays visible; delete it once the mirror is reconciled.
KNOWN_DRIFT: dict[str, str] = {}


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def members(body: str) -> list[str]:
    body = re.sub(r"^\s*#.*$", " ", body, flags=re.M)
    out = []
    for decl in body.split(";"):
        decl = " ".join(decl.replace("XR_MAY_ALIAS", "").split())
        if decl:
            decl = re.sub(r"\s*\*\s*", "* ", decl)  # "void *x" == "void* x"
            for pattern, repl in TYPE_EQUIV:
                decl = pattern.sub(repl, decl)
            if decl not in out:  # identical #if/#else arms
                out.append(decl)
    return out


def scan(texts: list[str]) -> dict:
    enums, specs, exts, structs, funcs = {}, {}, set(), {}, set()
    for raw in texts:
        for name, _typ, val in ENUM_RE.findall(raw):
            enums[name] = int(val)
        for prefix, ver in SPEC_RE.findall(raw):
            specs[prefix.lower()] = int(ver)
        exts.update(EXT_NAME_RE.findall(raw))
        code = strip_comments(raw)
        for name, body in STRUCT_RE.findall(code):
            structs[name] = members(body)
        funcs.update(FUNC_RE.findall(code))
    return {"enums": enums, "specs": specs, "exts": exts, "structs": structs, "funcs": funcs}


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    published_dir = Path(sys.argv[1])
    headers = sorted(published_dir.glob("XR_DXR_*.h")) or sorted(published_dir.glob("openxr/XR_DXR_*.h"))
    if not headers:
        print(f"no XR_DXR_*.h under {published_dir}", file=sys.stderr)
        return 2
    pub = scan([h.read_text() for h in headers])
    mir = scan([MIRROR.read_text()])

    errors, notes = [], []
    for name, val in sorted(mir["enums"].items()):
        if name in pub["enums"]:
            if pub["enums"][name] != val:
                errors.append(f"{name} = {val} in the mirror, {pub['enums'][name]} published")
        elif name.startswith("XR_TYPE_") and name.endswith("_DXR"):
            errors.append(f"{name} is not in the published headers (renamed or removed?)")
    for ext in sorted(mir["exts"] - pub["exts"]):
        errors.append(f"extension {ext} is not published")
    for key, ver in sorted(mir["specs"].items()):
        if key not in pub["specs"]:
            continue  # spelling the mirror invented for a value it does not version
        if ver > pub["specs"][key]:
            errors.append(f"{key.upper()}_SPEC_VERSION = {ver} in the mirror, newer than published {pub['specs'][key]}")
        elif ver < pub["specs"][key]:
            notes.append(f"{key.upper()}: mirror reconciled at spec {ver}, published {pub['specs'][key]}")
    for name, mem in sorted(mir["structs"].items()):
        theirs = pub["structs"].get(name)
        if theirs is not None and theirs != mem:
            errors.append(f"struct {name} differs:\n      mirror:    {mem}\n      published: {theirs}")
    for fn in sorted(mir["funcs"] - pub["funcs"]):
        errors.append(f"entry point {fn} is not in the published headers")

    checked = (f"{len(mir['enums'])} enum values, {len(mir['exts'])} extension names, "
               f"{len(mir['structs'])} structs, {len(mir['funcs'])} entry points, {len(mir['specs'])} spec versions")
    known = []
    for e in list(errors):
        key = next((k for k in KNOWN_DRIFT if e.startswith(k)), None)
        if key:
            errors.remove(e)
            known.append(f"{key}: {KNOWN_DRIFT[key]}")
    for n in notes:
        print(f"note: {n}")
    for k in known:
        print(f"known drift (allowed): {k}")
    for k in sorted(set(KNOWN_DRIFT) - {k.split(':')[0] for k in known}):
        errors.append(f"KNOWN_DRIFT entry '{k}' no longer drifts: delete it")
    if errors:
        for e in errors:
            print(f"::error file=native~/displayxr_extensions.h::{e}")
        print(f"FAIL: {len(errors)} drift(s) between the mirror and the published headers ({checked}).")
        return 1
    print(f"OK: mirror matches the published headers ({checked}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
