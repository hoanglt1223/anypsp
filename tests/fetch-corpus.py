#!/usr/bin/env python3
"""Fetch the open-source homebrew test corpus (pspautotests binaries).

Every file is pinned to one upstream commit and checked against its SHA-256.
The corpus goes to tests/corpus/ (git-ignored); nothing fetched is committed.

  python tests/fetch-corpus.py            # fetch and verify
  python tests/fetch-corpus.py --print    # print hashes of local files (to update the manifest)
"""

import hashlib
import pathlib
import sys
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent
MANIFEST = ROOT / "corpus-manifest.txt"
DEST = ROOT / "corpus"


def read_manifest():
    repo = commit = None
    files = []
    for number, raw in enumerate(MANIFEST.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, _, value = line.partition(" ")
        value = value.strip()
        if key == "repo":
            repo = value
        elif key == "commit":
            commit = value
        elif len(key) == 64 and all(c in "0123456789abcdef" for c in key) and value:
            files.append((key, value))
        else:
            sys.exit(f"{MANIFEST.name}:{number}: cannot parse {raw!r}")
    if not repo or not commit or len(commit) != 40:
        sys.exit(f"{MANIFEST.name}: needs 'repo <owner/name>' and a full 40-character 'commit <sha>'")
    return repo, commit, files


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    repo, commit, files = read_manifest()
    base = DEST / repo.split("/")[-1]
    if "--print" in sys.argv[1:]:
        for _, path in files:
            local = base / path
            if local.exists():
                print(f"{sha256(local.read_bytes())} {path}")
        return 0
    failures = 0
    for expected, path in files:
        local = base / path
        if local.exists() and sha256(local.read_bytes()) == expected:
            continue
        url = f"https://raw.githubusercontent.com/{repo}/{commit}/{path}"
        try:
            with urllib.request.urlopen(url, timeout=60) as response:
                data = response.read()
        except OSError as error:
            print(f"FAIL {path}: {error}", file=sys.stderr)
            failures += 1
            continue
        actual = sha256(data)
        if actual != expected:
            print(f"FAIL {path}: sha256 {actual}, expected {expected}", file=sys.stderr)
            failures += 1
            continue
        local.parent.mkdir(parents=True, exist_ok=True)
        local.write_bytes(data)
        print(f"ok   {path}")
    print(f"{len(files) - failures}/{len(files)} files verified in {base}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
