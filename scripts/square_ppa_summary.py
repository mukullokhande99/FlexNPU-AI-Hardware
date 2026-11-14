#!/usr/bin/env python3
"""Parse Yosys generic-cell synthesis results and longest path estimates.
NOTE: Generic gate counts and longest topological path are NOT PDK area/ns.
"""
import re, json, pathlib, sys
results=[]
for name in ("int8","square_v0","square_v1"):
    p=pathlib.Path("build/square_ppa")/(name+".log")
    if not p.exists():
        print(f"missing {p}",file=sys.stderr);sys.exit(2)
    txt=p.read_text()
    # Yosys 'stat' usually prints Number of cells and a per-cell breakdown.
    count=re.findall(r"\b(?:Number of cells:|cells\s+)\s*(\d+)",txt)
    paths=re.findall(r"length=(\d+)",txt)
    results.append({"variant":name,
                    "generic_cells":int(count[-1]) if count else None,
                    "longest_topological_path":int(paths[-1]) if paths else None,
                    "log":str(p)})
out=pathlib.Path("build/square_ppa/report.json")
out.write_text(json.dumps(results,indent=2)+"\n")
print(json.dumps(results,indent=2))
print("These are technology-independent proxies, not mapped area or timing.")
