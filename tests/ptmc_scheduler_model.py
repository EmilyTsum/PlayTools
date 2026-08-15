#!/usr/bin/env python3

def capture_count(source_fps: float, target_fps: float, seconds: float = 10.0):
    interval = 1.0 / target_fps
    tolerance = min(interval / 8.0, 0.001)
    next_tick = None
    captured = skipped = 0
    frames = int(source_fps * seconds)
    for i in range(frames):
        tick = i / source_fps
        if next_tick is None:
            captured += 1
            next_tick = tick + interval
        elif tick + tolerance < next_tick:
            skipped += 1
        else:
            captured += 1
            next_tick = tick + interval
    return captured / seconds, skipped / seconds

# A slightly-fast nominal ~60 Hz source must not collapse to ~30 fps.
fps, skips = capture_count(62.5, 60.0)
assert fps >= 58.0, (fps, skips)

# A 120 Hz source intentionally sampled at 60 fps should capture ~60 and skip ~60 each second.
fps, skips = capture_count(120.0, 60.0)
assert 58.0 <= fps <= 62.0, (fps, skips)
assert 58.0 <= skips <= 62.0, (fps, skips)

# A 60 Hz source at a 60 fps target should not intentionally skip.
fps, skips = capture_count(60.0, 60.0)
assert fps >= 59.0 and skips <= 1.0, (fps, skips)
print('PTMC scheduler model: PASS')
