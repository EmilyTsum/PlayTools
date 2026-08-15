#!/usr/bin/env python3
from pathlib import Path
import argparse, hashlib, shutil, sys

INCLUDE = '#include "MetalCapture/PTMetalCapture.inc"'
ANCHOR = '\n@implementation PlayLoader\n'

def digest(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()

def main() -> None:
    ap = argparse.ArgumentParser(description='Apply PTMC overlay to a fresh PlayTools checkout')
    ap.add_argument('upstream', type=Path)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[1]
    source = repo / 'PlayTools' / 'MetalCapture' / 'PTMetalCapture.inc'
    loader = args.upstream / 'PlayTools' / 'PlayLoader.m'
    target_dir = args.upstream / 'PlayTools' / 'MetalCapture'
    target = target_dir / 'PTMetalCapture.inc'
    if not loader.exists():
        sys.exit(f'missing upstream PlayLoader.m: {loader}')
    text = loader.read_text()
    count = text.count(INCLUDE)
    if args.check:
        if count != 1 or not target.exists() or digest(target) != digest(source):
            sys.exit('PTMC patch verification failed')
        print('PTMC patch verified')
        return
    if count > 1:
        sys.exit('refusing: duplicate PTMC include')
    if count == 0:
        if text.count(ANCHOR) != 1:
            sys.exit('upstream PlayLoader anchor changed; refusing silent mispatch')
        loader.write_text(text.replace(ANCHOR, '\n' + INCLUDE + '\n' + ANCHOR, 1))
    target_dir.mkdir(parents=True, exist_ok=True)
    if target.exists() and digest(target) != digest(source):
        sys.exit('refusing to overwrite modified PTMC source')
    shutil.copy2(source, target)
    print('PTMC patch applied' if count == 0 else 'PTMC patch already applied')

if __name__ == '__main__':
    main()
