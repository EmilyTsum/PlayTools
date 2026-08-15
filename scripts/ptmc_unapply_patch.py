#!/usr/bin/env python3
from pathlib import Path
import argparse, hashlib, sys
INCLUDE = '#include "MetalCapture/PTMetalCapture.inc"'

def digest(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()

def main() -> None:
    ap = argparse.ArgumentParser(description='Remove PTMC overlay from PlayTools checkout')
    ap.add_argument('upstream', type=Path)
    args = ap.parse_args()
    repo = Path(__file__).resolve().parents[1]
    source = repo / 'PlayTools' / 'MetalCapture' / 'PTMetalCapture.inc'
    loader = args.upstream / 'PlayTools' / 'PlayLoader.m'
    target = args.upstream / 'PlayTools' / 'MetalCapture' / 'PTMetalCapture.inc'
    text = loader.read_text()
    count = text.count(INCLUDE)
    if count > 1:
        sys.exit('refusing: duplicate PTMC includes')
    if target.exists() and digest(target) != digest(source):
        sys.exit('refusing: target PTMC source was modified')
    if count == 1:
        text = text.replace('\n' + INCLUDE + '\n\n@implementation PlayLoader\n', '\n@implementation PlayLoader\n', 1)
        loader.write_text(text)
    if target.exists():
        target.unlink()
        parent = target.parent
        try:
            parent.rmdir()
        except OSError:
            pass
    print('PTMC patch removed' if count else 'PTMC patch already absent')

if __name__ == '__main__':
    main()
