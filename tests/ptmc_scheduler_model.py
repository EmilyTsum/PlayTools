#!/usr/bin/env python3


def capture_count(source_fps: float, target_fps: float, seconds: float = 10.0):
    interval = 1.0 / target_fps
    next_tick = None
    last_present = None
    ewma = None
    captured = skipped = 0
    frames = int(source_fps * seconds)
    for i in range(frames):
        tick = i / source_fps
        if last_present is not None:
            sample = tick - last_present
            ewma = sample if ewma is None else (ewma * 7 + sample) / 8
        last_present = tick
        clearly_faster = ewma is not None and ewma * 100 < interval * 94
        if not clearly_faster:
            captured += 1
            next_tick = tick + interval
            continue
        tolerance = min(interval / 16.0, 0.00025)
        if next_tick is None:
            captured += 1
            next_tick = tick + interval
        elif tick + tolerance < next_tick:
            skipped += 1
        else:
            captured += 1
            if tick > next_tick + interval * 4:
                next_tick = tick + interval
            else:
                next_tick += interval
                while next_tick <= tick:
                    next_tick += interval
    return captured / seconds, skipped / seconds


for source in (59.0, 60.0, 61.0, 62.5):
    fps, skips = capture_count(source, 60.0)
    assert fps >= source - 0.5, (source, fps, skips)
    assert skips <= 0.5, (source, fps, skips)

fps, skips = capture_count(120.0, 60.0)
assert 58.0 <= fps <= 63.0, (fps, skips)
assert 57.0 <= skips <= 62.0, (fps, skips)

fps, skips = capture_count(90.0, 60.0)
assert 55.0 <= fps <= 65.0, (fps, skips)
assert skips >= 20.0, (fps, skips)
print('PTMC scheduler model: PASS')
