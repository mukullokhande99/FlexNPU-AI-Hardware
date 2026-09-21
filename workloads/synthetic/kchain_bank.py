"""
kchain_bank — generate a synthetic K-sweep trace at fixed M, N.

Emits a JSON list of {name, K, N, N_padded, count} records that any
ctb_mech / roofline / RTL replay can consume. Deliberately tiny —
duplicate this file and edit if you need a different shape bank.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Iterable, List


def kchain(M: int = 1, N: int = 16, K_values: Iterable[int] = (64, 128, 256, 512, 1024),
           count: int = 1, array_n: int = 16) -> List[dict]:
    sites = []
    for i, K in enumerate(K_values):
        Npad = int(math.ceil(N / array_n) * array_n)
        sites.append(dict(
            idx=i, name=f"synth_k{K}",
            M=M, K=int(K), N=N, N_padded=Npad, count=count,
        ))
    return sites


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--M", type=int, default=1)
    ap.add_argument("--N", type=int, default=16)
    ap.add_argument("--K-list", default="64,128,256,512,1024",
                    help="comma-separated K values")
    ap.add_argument("--count", type=int, default=1)
    ap.add_argument("--array-n", type=int, default=16)
    ap.add_argument("--out", default="-",
                    help="output path; '-' writes to stdout")
    args = ap.parse_args()

    Ks = [int(x) for x in args.K_list.split(",") if x.strip()]
    sites = kchain(M=args.M, N=args.N, K_values=Ks,
                   count=args.count, array_n=args.array_n)
    if args.out == "-":
        print(json.dumps(sites, indent=2))
    else:
        Path(args.out).write_text(json.dumps(sites, indent=2) + "\n")
        print(f"wrote {len(sites)} sites to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
