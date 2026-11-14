#!/usr/bin/env python3
"""Independent exact rational oracle for 4 low-precision MAC modes.
Generates exhaustive encoding/scale vectors; not derived from RTL.
"""
from fractions import Fraction
from pathlib import Path

POS = {
    0: Fraction(0), 1: Fraction(1,16), 2: Fraction(1,4),
    3: Fraction(1,2), 4: Fraction(1), 5: Fraction(2),
    6: Fraction(4), 7: Fraction(16),
    9: Fraction(-16), 10: Fraction(-4), 11: Fraction(-2),
    12: Fraction(-1), 13: Fraction(-1,2), 14: Fraction(-1,4),
    15: Fraction(-1,16),
}
UNITS=(-32,-12,-8,-4,-2,0,1,4,12,31)
def fp4(x):
    sign=-1 if x&8 else 1
    exp=(x>>1)&3
    frac=x&1
    if exp==0:
        return sign*Fraction(frac,2)
    return sign*Fraction(2+frac)*(Fraction(2)**(exp-2))
def fp2_lane(code,zero,e0m1):
    if zero:
        return Fraction(0)
    value=Fraction(3,2) if (e0m1 and code&1) else (
          Fraction(1,2) if (not e0m1 and code&1) else Fraction(1))
    return -value if (code&2) else value
def fp2_pair(x,e0m1):
    return [fp2_lane((x>>(2*i))&3, (x>>(4+i))&1,e0m1) for i in (0,1)]
def contribution(mode,a,b):
    if mode==0:
        if a==8 or b==8:
            return Fraction(0),1
        return POS[a]*POS[b],0
    if mode==1:
        return fp4(a)*fp4(b),0
    x,y=fp2_pair(a,mode==3),fp2_pair(b,mode==3)
    return x[0]*y[0]+x[1]*y[1],0
def trunc0(v):
    mag=abs(v.numerator)//v.denominator
    return -mag if v<0 else mag
def main():
    out=Path('build/square_ppa/oracle_vectors.txt')
    out.parent.mkdir(parents=True,exist_ok=True)
    n=0
    with out.open('w') as f:
        for mode in range(4):
            size=16 if mode<2 else 64
            for a in range(size):
                for b in range(size):
                    term,nar=contribution(mode,a,b)
                    for exp in UNITS:
                        x=trunc0(term/(Fraction(2)**exp)) & 0xFFFFFFFF
                        f.write(f'{mode} {a:02x} {b:02x} {exp} {x:08x} {nar}\n')
                        n+=1
    assert n==87040,n
    print(f'generated {n} independent arithmetic vectors: {out}')
if __name__=='__main__': main()
