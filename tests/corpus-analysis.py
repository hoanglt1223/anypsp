#!/usr/bin/env python3
"""Runs anypsp-analyze over the fetched homebrew corpus and checks the phase-02 bar.

  python tests/corpus-analysis.py <anypsp-analyze executable> <output dir>

Every module must load with no invalid relocation; every import must be resolved
or listed as unknown; in every ELF with a symbol table, every function symbol
must be a discovered function entry. Exits 77 (skipped) when the corpus has not
been fetched (python tests/fetch-corpus.py).
"""

import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent
CORPUS = ROOT / "corpus" / "pspautotests"
# Programs analysed together with the modules they load.
GROUPS = {
    "tests/modules/cutrel/cutrel.prx": ["tests/modules/cutrel/child.prx", "tests/modules/cutrel/child_cut.prx",
                                        "tests/modules/cutrel/child_n.prx"],
    "tests/modules/loadrace/loadrace.prx": ["tests/modules/loadrace/child.prx"],
    "tests/modules/startoptions/startoptions.prx": ["tests/modules/startoptions/child.prx"],
}


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    analyze, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    if not CORPUS.exists():
        print("corpus not fetched; run python tests/fetch-corpus.py")
        return 77
    grouped = {child for children in GROUPS.values() for child in children}
    mains = sorted(p.relative_to(CORPUS).as_posix() for p in CORPUS.rglob("*") if p.suffix in (".prx", ".elf"))
    failures = []
    totals = {"modules": 0, "imports": 0, "unknown_imports": 0, "symbols": 0, "symbols_found": 0}
    for main in mains:
        if main in grouped:
            continue
        inputs = [CORPUS / main] + [CORPUS / child for child in GROUPS.get(main, [])]
        target = out / main.replace("/", "_")
        result = subprocess.run([str(analyze), "--out", str(target)] + [str(i) for i in inputs], capture_output=True, text=True)
        if result.returncode != 0:
            failures.append(f"{main}: anypsp-analyze failed: {result.stderr.strip()[:300]}")
            continue
        report = json.loads((target / "report.json").read_text(encoding="utf-8"))
        for module in report["modules"]:
            name = pathlib.Path(module["path"]).name
            totals["modules"] += 1
            totals["imports"] += module["imports"]["total"]
            totals["unknown_imports"] += module["imports"]["unknown"]
            totals["symbols"] += module["symbols"]["functions"]
            totals["symbols_found"] += module["symbols"]["found"]
            if module["relocations"]["invalid"] or module["relocations"]["unsupported"]:
                failures.append(f"{name}: {module['relocations']}")
            if module["imports"]["unknown"] != len(module["imports"]["unknown_list"]):
                failures.append(f"{name}: unknown imports are not all listed")
            if module["symbols"]["missing"]:
                failures.append(f"{name}: functions not found: {module['symbols']['missing'][:5]}")
    print(json.dumps(totals))
    for failure in failures:
        print("FAIL", failure)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
