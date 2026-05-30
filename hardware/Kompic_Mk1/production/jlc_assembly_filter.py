#!/usr/bin/env python3
"""
Generate JLCPCB assembly BOM + CPL from a full KiCad BOM/position export,
driven by the `jlc-assembly` block inside the picking-list Markdown file.

The picking list is the single source of truth for WHO PLACES WHAT.
Only designators marked Place=JLC in that block are sent to the fab.
Everything else (Place=HAND, or any designator absent from the block)
is dropped from the JLC files — you place it by hand.

Outputs (JLCPCB upload format):
    jlcpcb_bom.csv   -> Comment, Designator, Footprint, LCSC Part #
    jlcpcb_cpl.csv   -> Designator, Mid X, Mid Y, Rotation, Layer

Usage:
    python3 jlc_assembly_filter.py <picking_list.md> <bom.csv> <positions.csv>

Workflow: edit the ```jlc-assembly block in the MD (flip Place, paste LCSC
numbers after you sort the JLC order), re-run, files regenerate.
"""
import csv, sys, io, re

def read_csv(path):
    with io.open(path, newline="", encoding="utf-8-sig") as f:
        return list(csv.DictReader(f))

def split_designators(cell):
    return [d.strip() for d in cell.replace(";", ",").split(",") if d.strip()]

def parse_picking_list(md_path):
    """Return dict: designator -> {'place':..,'lcsc':..,'value':..,'footprint':..}"""
    text = io.open(md_path, encoding="utf-8").read()
    m = re.search(r"```jlc-assembly\s*\n(.*?)```", text, re.DOTALL)
    if not m:
        sys.exit("ERROR: no ```jlc-assembly block found in picking list.")
    block = m.group(1).strip().splitlines()
    if not block:
        sys.exit("ERROR: jlc-assembly block is empty.")

    out = {}
    header_seen = False
    for ln in block:
        if not ln.strip():
            continue
        cols = [c.strip() for c in ln.split("|")]
        if len(cols) < 5:
            sys.exit(f"ERROR: malformed row (need 5 cols): {ln!r}")
        desig, value, footprint, lcsc, place = cols[:5]
        if not header_seen and desig.lower() == "designator":
            header_seen = True
            continue
        place = place.upper()
        if place not in ("JLC", "HAND"):
            sys.exit(f"ERROR: Place must be JLC or HAND, got {place!r} in: {ln!r}")
        for d in split_designators(desig):
            if d in out:
                sys.exit(f"ERROR: designator {d} listed twice in jlc-assembly block.")
            out[d] = {"place": place, "lcsc": lcsc,
                      "value": value, "footprint": footprint}
    return out

def main(md_path, bom_path, pos_path):
    pick = parse_picking_list(md_path)
    bom_rows = read_csv(bom_path)
    pos_rows = read_csv(pos_path)

    jlc = {d: v for d, v in pick.items() if v["place"] == "JLC"}
    jlc_desigs = set(jlc)

    # Build assembly BOM by walking the real BOM, keeping only JLC designators.
    # LCSC comes from the picking block (authoritative); falls back to BOM column.
    bom_out, placed = [], 0
    for r in bom_rows:
        keep = [d for d in split_designators(r.get("Designator", "")) if d in jlc_desigs]
        if not keep:
            continue
        by_lcsc = {}
        for d in keep:
            lc = jlc[d]["lcsc"].strip() or (r.get("LCSC Part #") or "").strip()
            by_lcsc.setdefault(lc, []).append(d)
        for lc, ds in by_lcsc.items():
            bom_out.append({
                "Comment": r.get("Value", ""),
                "Designator": ", ".join(ds),
                "Footprint": r.get("Footprint", ""),
                "LCSC Part #": lc,
            })
            placed += len(ds)

    with open("jlcpcb_bom.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["Comment","Designator","Footprint","LCSC Part #"])
        w.writeheader(); w.writerows(bom_out)

    cpl_out = [r for r in pos_rows if r.get("Designator","").strip() in jlc_desigs]
    with open("jlcpcb_cpl.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["Designator","Mid X","Mid Y","Rotation","Layer"])
        w.writeheader(); w.writerows(cpl_out)

    # ---- diagnostics ----
    bom_all = {d for r in bom_rows for d in split_designators(r.get("Designator",""))}
    pos_all = {r.get("Designator","").strip() for r in pos_rows}

    print(f"JLC designators (picking list) : {len(jlc_desigs)}")
    print(f"JLC BOM lines written          : {len(bom_out)}")
    print(f"JLC placements (BOM)           : {placed}")
    print(f"JLC CPL rows written           : {len(cpl_out)}")

    no_lcsc = sorted(d for d in jlc if not (jlc[d]['lcsc'].strip()))
    if no_lcsc:
        print(f"\nJLC parts still missing LCSC (JLC parser will prompt): "
              f"{len(no_lcsc)} -> {', '.join(no_lcsc[:8])}{' ...' if len(no_lcsc)>8 else ''}")

    in_block_not_bom = sorted(set(pick) - bom_all)
    if in_block_not_bom:
        print(f"\nWARNING - in picking block but NOT in BOM: {in_block_not_bom}")
    jlc_not_pos = sorted(jlc_desigs - pos_all)
    if jlc_not_pos:
        print(f"WARNING - JLC part has no position row: {jlc_not_pos}")

    absent = sorted(bom_all - set(pick))
    if absent:
        print(f"\nFYI - {len(absent)} BOM designators absent from block "
              f"(treated as HAND/dropped): {', '.join(absent[:12])}"
              f"{' ...' if len(absent)>12 else ''}")

if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit("usage: python3 jlc_assembly_filter.py "
                 "<picking_list.md> <bom.csv> <positions.csv>")
    main(sys.argv[1], sys.argv[2], sys.argv[3])