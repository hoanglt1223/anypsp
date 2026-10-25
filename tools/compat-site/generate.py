#!/usr/bin/env python3
"""Renders compat/*.toml (schema: compat/schema.md) and coverage numbers into a static progress page and SVG badges.

usage: python tools/compat-site/generate.py [--compat DIR] [--out DIR] [--project NAME]
                                            [--coverage coverage.json] [--source DIR] [--check]

Writes <out>/progress.html (also as index.html), <out>/compat.json, <out>/badges/compat.svg (titles
in-game or better), one badge per stage and per title, and with coverage: badges/hle.svg (HLE
functions of user-mode libraries, by NID), badges/vfpu.svg (VFPU instructions translated natively)
and badges/ge.svg (GE commands the GE front end reads). --coverage takes the JSON anypsp-coverage
prints; GE commands are counted from <source>/runtime/host/ge.cpp. --check only validates.
Same format as AnyGBA's generator; Python 3.11+ (tomllib), no other packages.
"""

from __future__ import annotations

import argparse
import datetime
import html
import json
import pathlib
import re
import sys
import tomllib

STAGES = ["fails", "boots", "menus", "in-game", "playable", "perfect"]
COLORS = {
    "fails": "#e05d44",
    "boots": "#fe7d37",
    "menus": "#dfb317",
    "in-game": "#a4a61d",
    "playable": "#97ca00",
    "perfect": "#4c1",
}
KINDS = ["commercial", "homebrew", "test-suite"]
RUNNERS = ["translated", "interpreter"]
HOSTS = ["windows-x64", "linux-x64", "linux-arm64", "macos-arm64", "android-arm64", "android-x86_64"]
REQUIRED = {"title": str, "kind": str, "stage": str}
DISC_ID = re.compile(r"^[A-Z]{4}-\d{5}$")

# The PSP's GE commands (number -> name), as documented by pspsdk and PPSSPP. Numbers missing here
# are unused by the hardware.
GE_COMMANDS: dict[int, str] = {
    0x00: "NOP", 0x01: "VADDR", 0x02: "IADDR", 0x04: "PRIM", 0x05: "BEZIER", 0x06: "SPLINE", 0x07: "BOUNDINGBOX",
    0x08: "JUMP", 0x09: "BJUMP", 0x0A: "CALL", 0x0B: "RET", 0x0C: "END", 0x0E: "SIGNAL", 0x0F: "FINISH",
    0x10: "BASE", 0x12: "VERTEXTYPE", 0x13: "OFFSETADDR", 0x14: "ORIGIN", 0x15: "REGION1", 0x16: "REGION2",
    0x17: "LIGHTINGENABLE", 0x1C: "DEPTHCLAMPENABLE", 0x1D: "CULLFACEENABLE", 0x1E: "TEXTUREMAPENABLE",
    0x1F: "FOGENABLE", 0x20: "DITHERENABLE", 0x21: "ALPHABLENDENABLE", 0x22: "ALPHATESTENABLE", 0x23: "ZTESTENABLE",
    0x24: "STENCILTESTENABLE", 0x25: "ANTIALIASENABLE", 0x26: "PATCHCULLENABLE", 0x27: "COLORTESTENABLE",
    0x28: "LOGICOPENABLE", 0x2A: "BONEMATRIXNUMBER", 0x2B: "BONEMATRIXDATA", 0x36: "PATCHDIVISION",
    0x37: "PATCHPRIMITIVE", 0x38: "PATCHFACING", 0x3A: "WORLDMATRIXNUMBER", 0x3B: "WORLDMATRIXDATA",
    0x3C: "VIEWMATRIXNUMBER", 0x3D: "VIEWMATRIXDATA", 0x3E: "PROJMATRIXNUMBER", 0x3F: "PROJMATRIXDATA",
    0x40: "TGENMATRIXNUMBER", 0x41: "TGENMATRIXDATA", 0x42: "VIEWPORTXSCALE", 0x43: "VIEWPORTYSCALE",
    0x44: "VIEWPORTZSCALE", 0x45: "VIEWPORTXCENTER", 0x46: "VIEWPORTYCENTER", 0x47: "VIEWPORTZCENTER",
    0x48: "TEXSCALEU", 0x49: "TEXSCALEV", 0x4A: "TEXOFFSETU", 0x4B: "TEXOFFSETV", 0x4C: "OFFSETX", 0x4D: "OFFSETY",
    0x50: "SHADEMODE", 0x51: "REVERSENORMAL", 0x53: "MATERIALUPDATE", 0x54: "MATERIALEMISSIVE",
    0x55: "MATERIALAMBIENT", 0x56: "MATERIALDIFFUSE", 0x57: "MATERIALSPECULAR", 0x58: "MATERIALALPHA",
    0x5B: "MATERIALSPECULARCOEF", 0x5C: "AMBIENTCOLOR", 0x5D: "AMBIENTALPHA", 0x5E: "LIGHTMODE", 0x9B: "CULL",
    0x9C: "FRAMEBUFPTR", 0x9D: "FRAMEBUFWIDTH", 0x9E: "ZBUFPTR", 0x9F: "ZBUFWIDTH", 0xB0: "CLUTADDR",
    0xB1: "CLUTADDRUPPER", 0xB2: "TRANSFERSRC", 0xB3: "TRANSFERSRCW", 0xB4: "TRANSFERDST", 0xB5: "TRANSFERDSTW",
    0xC0: "TEXMAPMODE", 0xC1: "TEXSHADELS", 0xC2: "TEXMODE", 0xC3: "TEXFORMAT", 0xC4: "LOADCLUT",
    0xC5: "CLUTFORMAT", 0xC6: "TEXFILTER", 0xC7: "TEXWRAP", 0xC8: "TEXLEVEL", 0xC9: "TEXFUNC", 0xCA: "TEXENVCOLOR",
    0xCB: "TEXFLUSH", 0xCC: "TEXSYNC", 0xCD: "FOG1", 0xCE: "FOG2", 0xCF: "FOGCOLOR", 0xD0: "TEXLODSLOPE",
    0xD2: "FRAMEBUFPIXFORMAT", 0xD3: "CLEARMODE", 0xD4: "SCISSOR1", 0xD5: "SCISSOR2", 0xD6: "MINZ", 0xD7: "MAXZ",
    0xD8: "COLORTEST", 0xD9: "COLORREF", 0xDA: "COLORTESTMASK", 0xDB: "ALPHATEST", 0xDC: "STENCILTEST",
    0xDD: "STENCILOP", 0xDE: "ZTEST", 0xDF: "BLENDMODE", 0xE0: "BLENDFIXEDA", 0xE1: "BLENDFIXEDB",
    0xE6: "LOGICOP", 0xE7: "ZWRITEDISABLE", 0xE8: "MASKRGB", 0xE9: "MASKALPHA", 0xEA: "TRANSFERSTART",
    0xEB: "TRANSFERSRCPOS", 0xEC: "TRANSFERDSTPOS", 0xEE: "TRANSFERSIZE",
}
# Indexed families: first command, count, name.
GE_FAMILIES = [
    (0x18, 4, "LIGHTENABLE"), (0x2C, 8, "MORPHWEIGHT"), (0x5F, 4, "LIGHTTYPE"), (0x63, 12, "LIGHTPOS"),
    (0x6F, 12, "LIGHTDIR"), (0x7B, 12, "LIGHTATT"), (0x87, 4, "LIGHTSPOTEXP"), (0x8B, 4, "LIGHTSPOTCUTOFF"),
    (0x8F, 12, "LIGHTCOLOR"), (0xA0, 8, "TEXADDR"), (0xA8, 8, "TEXBUFWIDTH"), (0xB8, 8, "TEXSIZE"),
    (0xE2, 4, "DITHER"), (0xF0, 10, "IMMEDIATEVERTEX"),
]
for _first, _count, _name in GE_FAMILIES:
    for _i in range(_count):
        GE_COMMANDS[_first + _i] = f"{_name}{_i}"


class CompatError(Exception):
    pass


def load(directory: pathlib.Path) -> list[dict]:
    entries = []
    for path in sorted(directory.glob("*.toml")):
        try:
            data = tomllib.loads(path.read_text(encoding="utf-8"))
        except tomllib.TOMLDecodeError as e:
            raise CompatError(f"{path}: {e}") from e
        for key, kind in REQUIRED.items():
            if not isinstance(data.get(key), kind):
                raise CompatError(f"{path}: '{key}' is required and must be a {kind.__name__}")
        if data["stage"] not in STAGES:
            raise CompatError(f"{path}: stage '{data['stage']}' is not one of {', '.join(STAGES)}")
        if data["kind"] not in KINDS:
            raise CompatError(f"{path}: kind '{data['kind']}' is not one of {', '.join(KINDS)}")
        if data["kind"] == "commercial":
            if any(k in data for k in ("module", "checkpoint_hash", "sha256")):
                raise CompatError(f"{path}: commercial titles carry only disc_id, never a file path or hash")
            if not DISC_ID.match(str(data.get("disc_id", ""))):
                raise CompatError(f"{path}: commercial titles need disc_id like ULUS-10299")
        if data.get("runner", "translated") not in RUNNERS:
            raise CompatError(f"{path}: runner '{data['runner']}' is not one of {', '.join(RUNNERS)}")
        hosts = data.get("hosts", [])
        if not isinstance(hosts, list) or any(h not in HOSTS for h in hosts):
            raise CompatError(f"{path}: 'hosts' must be a list of {', '.join(HOSTS)}")
        blockers = data.get("blockers", [])
        if not isinstance(blockers, list) or not all(isinstance(b, str) for b in blockers):
            raise CompatError(f"{path}: 'blockers' must be a list of strings")
        if STAGES.index(data["stage"]) >= STAGES.index("in-game") and not data.get("evidence"):
            raise CompatError(f"{path}: stage '{data['stage']}' needs evidence")
        data["id"] = path.stem
        entries.append(data)
    if not entries:
        raise CompatError(f"no *.toml files in {directory}")
    return entries


def ge_coverage(ge_source: pathlib.Path) -> dict:
    """GE commands the front end reads: an approximate static scan of runtime/host/ge.cpp.

    A command counts when the source names it outside the opcode enum (regs_[kX], case kX, cmd == kX,
    kX + n for an indexed family), or reads regs_[0xNN] / compares cmd == 0xNN outside log output.
    """
    text = ge_source.read_text(encoding="utf-8")
    enum = re.search(r"enum\s*:\s*std::uint32_t\s*\{(.*?)\};", text, re.S)
    if not enum:
        raise CompatError(f"{ge_source}: no GE opcode enum found")
    names = {m.group(1): int(m.group(2), 16) for m in re.finditer(r"(k\w+)\s*=\s*0x([0-9A-Fa-f]+)", enum.group(1))}
    body = text[:enum.start()] + text[enum.end():]
    body = "\n".join(line for line in body.splitlines() if "std::cerr" not in line and "std::cout" not in line)
    handled: set[int] = set()
    for name, value in names.items():
        uses = re.findall(rf"\b{name}\b(\s*\+)?", body)
        if not uses:
            continue
        handled.add(value)
        if any(plus for plus in uses):
            for first, count, _ in GE_FAMILIES:
                if first <= value < first + count:
                    handled.update(range(first, first + count))
    for m in re.finditer(r"(?:regs_\[\s*|cmd\s*==\s*)0x([0-9A-Fa-f]{2})u?", body):
        handled.add(int(m.group(1), 16))
    known = sorted(GE_COMMANDS)
    covered = [c for c in known if c in handled]
    return {"handled": len(covered), "total": len(known),
            "missing": [GE_COMMANDS[c] for c in known if c not in handled]}


def text_width(text: str) -> int:
    # Close enough to Verdana 11px for badge layout.
    return int(sum(7 if c.isupper() or c in "mw%" else 6 for c in text)) + 10


def badge(label: str, value: str, color: str) -> str:
    lw, vw = text_width(label), text_width(value)
    w = lw + vw
    label_e, value_e = html.escape(label), html.escape(value)
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="20" role="img" aria-label="{label_e}: {value_e}">
<title>{label_e}: {value_e}</title>
<linearGradient id="s" x2="0" y2="100%"><stop offset="0" stop-color="#bbb" stop-opacity=".1"/><stop offset="1" stop-opacity=".1"/></linearGradient>
<clipPath id="r"><rect width="{w}" height="20" rx="3" fill="#fff"/></clipPath>
<g clip-path="url(#r)"><rect width="{lw}" height="20" fill="#555"/><rect x="{lw}" width="{vw}" height="20" fill="{color}"/><rect width="{w}" height="20" fill="url(#s)"/></g>
<g fill="#fff" text-anchor="middle" font-family="Verdana,Geneva,DejaVu Sans,sans-serif" font-size="11">
<text x="{lw / 2}" y="14">{label_e}</text><text x="{lw + vw / 2}" y="14">{value_e}</text></g>
</svg>
"""


def ratio_color(done: int, total: int) -> str:
    share = done / total if total else 0.0
    for limit, stage in ((0.9, "perfect"), (0.7, "playable"), (0.5, "in-game"), (0.3, "menus"), (0.1, "boots")):
        if share >= limit:
            return COLORS[stage]
    return COLORS["fails"]


def coverage_rows(coverage: dict) -> list[tuple[str, int, int, str]]:
    """(label, done, total, note) for the page and the badges."""
    rows = []
    if "hle" in coverage:
        h = coverage["hle"]
        rows.append(("HLE functions (user-mode libraries, by NID)", h["user_implemented"], h["user_total"],
                     f"{h['implemented']} of {h['total']} including kernel-mode libraries"))
    if "vfpu" in coverage:
        v = coverage["vfpu"]
        rows.append(("VFPU instructions translated natively", v["lowered"], v["total"],
                     "the rest run in the reference interpreter from generated code"
                     + (f"; partly native: {', '.join(v['partial'])}" if v.get("partial") else "")))
    if "ge" in coverage:
        g = coverage["ge"]
        rows.append(("GE commands the GE front end reads", g["handled"], g["total"],
                     "static scan of runtime/host/ge.cpp; missing: " + (", ".join(g["missing"]) or "none")))
    return rows


def body(project: str, entries: list[dict], coverage: dict, generated: str) -> str:
    """The page content without the document shell, so a website can embed it in its own layout."""
    counts = {s: sum(1 for e in entries if e["stage"] == s) for s in STAGES}
    total = len(entries)
    bar = "".join(
        f'<div style="width:{100 * counts[s] / total:.3f}%;background:{COLORS[s]}" title="{s}: {counts[s]}"></div>'
        for s in STAGES if counts[s])
    legend = " ".join(
        f'<span class="tag" style="background:{COLORS[s]}">{s} {counts[s]}</span>' for s in reversed(STAGES))
    rows = []
    order = {s: i for i, s in enumerate(STAGES)}
    for e in sorted(entries, key=lambda e: (-order[e["stage"]], e["kind"], e["title"].lower())):
        title = html.escape(e["title"])
        if e.get("source", "").startswith(("https://", "http://")):  # commercial titles name a dump, not a link
            title = f'<a href="{html.escape(e["source"])}">{title}</a>'
        if e.get("disc_id"):
            title += f' <span class="id">{html.escape(e["disc_id"])}</span>'
        blockers = "<br>".join(html.escape(b) for b in e.get("blockers", [])) or "&ndash;"
        hosts = ", ".join(html.escape(h) for h in e.get("hosts", [])) or "&ndash;"
        rows.append(
            "<tr>"
            f'<td>{title}</td><td>{html.escape(e["kind"])}</td>'
            f'<td><span class="tag" style="background:{COLORS[e["stage"]]}">{e["stage"]}</span></td>'
            f'<td>{html.escape(e.get("runner", "translated"))}</td><td>{hosts}</td>'
            f'<td>{html.escape(str(e.get("evidence", "")))}</td><td>{blockers}</td>'
            f'<td>{html.escape(str(e.get("updated", "")))}</td>'
            "</tr>")
    cov = ""
    if coverage:
        items = []
        for label, done, total_, note in coverage_rows(coverage):
            pct = 100 * done / total_ if total_ else 0
            items.append(f'<tr><td>{html.escape(label)}</td><td class="num">{done} / {total_}</td>'
                         f'<td><div class="meter"><div style="width:{pct:.1f}%;background:{ratio_color(done, total_)}"></div></div></td>'
                         f'<td>{html.escape(note)}</td></tr>')
        libs = ""
        if "hle" in coverage:
            lib_rows = "".join(
                f'<tr><td>{html.escape(name)}</td><td class="num">{d} / {t}</td></tr>'
                for name, (d, t) in sorted(coverage["hle"]["libraries"].items()) if "ForKernel" not in name)
            libs = (f"<details><summary>HLE functions per user-mode library</summary><table>"
                    f"<thead><tr><th>Library</th><th>Implemented</th></tr></thead><tbody>{lib_rows}</tbody></table></details>")
        cov = (f"<h2>Coverage</h2><table><thead><tr><th>What</th><th>Covered</th><th></th><th>Notes</th></tr></thead>"
               f"<tbody>{''.join(items)}</tbody></table>{libs}")
    return f"""<h1>{html.escape(project)} compatibility</h1>
<p>{total} titles. Stages and evidence rules: <code>compat/schema.md</code>. Generated {html.escape(generated)}.</p>
<div class="bar" role="img" aria-label="titles by stage">{bar}</div>
<p>{legend}</p>
<div class="table-wrap">
<table>
<thead><tr><th scope="col">Title</th><th scope="col">Kind</th><th scope="col">Stage</th><th scope="col">Runner</th>
<th scope="col">Hosts</th><th scope="col">Evidence</th><th scope="col">Blockers</th><th scope="col">Updated</th></tr></thead>
<tbody>
{chr(10).join(rows)}
</tbody>
</table>
</div>
{cov}
"""


def page(project: str, entries: list[dict], coverage: dict, generated: str) -> str:
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(project)} compatibility</title>
<style>
body {{ font-family: system-ui, sans-serif; margin: 2rem auto; max-width: 72rem; padding: 0 1rem; color: #222; }}
.bar {{ display: flex; height: 1.25rem; border-radius: 4px; overflow: hidden; margin: .5rem 0 1rem; }}
.tag {{ color: #fff; padding: .1rem .45rem; border-radius: 3px; font-size: .85rem; white-space: nowrap; }}
.id {{ color: #666; font-size: .8rem; }}
table {{ border-collapse: collapse; width: 100%; font-size: .92rem; }}
th, td {{ text-align: left; padding: .35rem .5rem; border-bottom: 1px solid #ddd; vertical-align: top; }}
th {{ background: #f4f4f4; position: sticky; top: 0; }}
.num {{ text-align: right; white-space: nowrap; }}
.meter {{ width: 10rem; height: .8rem; background: #eee; border-radius: 3px; overflow: hidden; }}
.meter div {{ height: 100%; }}
.table-wrap {{ overflow-x: auto; }}
details {{ margin: 1rem 0; }}
</style>
</head>
<body>
{body(project, entries, coverage, generated)}</body>
</html>
"""


def main(argv: list[str]) -> int:
    root = pathlib.Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compat", type=pathlib.Path, default=root / "compat")
    parser.add_argument("--out", type=pathlib.Path, default=root / "out" / "compat-site")
    parser.add_argument("--project", default="AnyPSP")
    parser.add_argument("--coverage", type=pathlib.Path, help="JSON written by anypsp-coverage")
    parser.add_argument("--source", type=pathlib.Path, default=root, help="AnyPSP source tree (GE command scan)")
    parser.add_argument("--check", action="store_true", help="validate compat/*.toml and the GE scan only")
    args = parser.parse_args(argv)
    try:
        entries = load(args.compat)
        coverage: dict = {}
        if args.coverage:
            coverage = json.loads(args.coverage.read_text(encoding="utf-8"))
        ge_source = args.source / "runtime" / "host" / "ge.cpp"
        if ge_source.exists():
            coverage["ge"] = ge_coverage(ge_source)
    except (CompatError, OSError, json.JSONDecodeError) as e:
        print(f"generate.py: {e}", file=sys.stderr)
        return 1
    if args.check:
        print(f"generate.py: {len(entries)} titles valid" + (f", GE {coverage['ge']['handled']}/{coverage['ge']['total']}" if "ge" in coverage else ""))
        return 0

    generated = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    badges = args.out / "badges"
    badges.mkdir(parents=True, exist_ok=True)
    text = page(args.project, entries, coverage, generated)
    for name in ("progress.html", "index.html"):
        (args.out / name).write_bytes(text.encode("utf-8"))
    good = sum(1 for e in entries if STAGES.index(e["stage"]) >= STAGES.index("in-game"))
    (badges / "compat.svg").write_bytes(
        badge("compatibility", f"{good}/{len(entries)} in-game+", COLORS["in-game"] if good else COLORS["fails"]).encode())
    for s in STAGES:
        n = sum(1 for e in entries if e["stage"] == s)
        (badges / f"stage-{s}.svg").write_bytes(badge(s, str(n), COLORS[s]).encode())
    for e in entries:
        (badges / f"{e['id']}.svg").write_bytes(badge(e["title"], e["stage"], COLORS[e["stage"]]).encode())
    for key, short, done_key, total_key in (("hle", "HLE NIDs", "user_implemented", "user_total"),
                                            ("vfpu", "VFPU native", "lowered", "total"),
                                            ("ge", "GE commands", "handled", "total")):
        if key in coverage:
            done, total = coverage[key][done_key], coverage[key][total_key]
            (badges / f"{key}.svg").write_bytes(badge(short, f"{done}/{total}", ratio_color(done, total)).encode())
    summary = {
        "titles": [{k: v for k, v in e.items() if not isinstance(v, (datetime.date, datetime.datetime))}
                   | {"updated": str(e.get("updated", ""))} for e in entries],
        "coverage": coverage,
    }
    (args.out / "compat.json").write_bytes(json.dumps(summary, indent=1, sort_keys=True).encode())
    print(f"generate.py: {len(entries)} titles -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
