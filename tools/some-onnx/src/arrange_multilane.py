# -*- coding: utf-8 -*-
"""arrange_multilane.py — 最终 MIDI → 多轨直排红石结构（独立工具，不打包进模组）。

与 arrange_straight.py 的区别：
  - 不按分离 stem 分轨；在「最终 MIDI → NBT」这一步按音高/音色把音符拆进多条并行轨道。
  - 每条轨道是一条独立的直排红石骨架（各自 repeater + 支线 + 音符盒）。
  - 所有轨道在 Z 方向平行排布，共用一条起始红石粉总线 + 一个拉杆/observer，
    因此所有轨道同一刻起步，脉冲并行前进，观赏性更强。

同步约束：
  起始总线是 0 延迟红石粉。中继器/红石粉的传输距离上限 15 格，所以轨道带宽
  （最远轨道到中心的 Z 距离）必须 <=14。3 轨 + 默认 lane-gap 通常满足；
  超过时工具会警告（可用更小的 --lane-gap 或减少 --lanes）。

用法:
    python arrange_multilane.py --midi final.mid --out multi.nbt \
        --lanes 3 --split pitch --resolution 2 --poly-cap 6 --lane-gap 2

    --split pitch : 按音高排序三等分（低/中/高），三条轨道音符数均衡（默认）
    --split bank  : 按音色档位分轨（bass / harp / bell）

依赖: mido, nbtlib（与 gen_machine 相同）。
"""
import argparse
import json
import os
import statistics
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_machine as g

BANK_LANE = {'bass': 0, 'harp': 1, 'bell': 2}


def lane_offsets(lanes, lane_width):
    """以 0 为中心、左右对称的 Z 偏移；中心不一定是某条轨道（偶数轨时在轨间）。"""
    mid = (lanes - 1) / 2.0
    return [int(round((i - mid) * lane_width)) for i in range(lanes)]


def assign_lanes(notes, conv, lanes, split):
    """返回 {lane: [note_index, ...]}。"""
    assignments = defaultdict(list)
    if split == 'pitch':
        order = sorted(range(len(notes)), key=lambda i: (notes[i][0], notes[i][1]))
        n = len(order)
        for rank, idx in enumerate(order):
            lane = min(lanes - 1, int(rank * lanes / max(1, n)))
            assignments[lane].append(idx)
    else:
        for idx, (pitch, _s, _e) in enumerate(notes):
            inst, _mat, _nv = conv(pitch)
            lane = min(lanes - 1, BANK_LANE.get(inst, 1))
            assignments[lane].append(idx)
    return assignments


def build_lane_columns(notes, indices, conv, col_seconds, columns, poly_cap):
    """量化 + 同列去重 + 复音上限；返回 (col_notes, stats)。"""
    cand = defaultdict(list)
    out_of_range = 0
    for idx in indices:
        pitch, start, _end = notes[idx]
        c0 = int(round(start / col_seconds))
        if not (0 <= c0 < columns):
            out_of_range += 1
            continue
        cand[c0].append(conv(pitch))
    col_notes = {}
    merged_dupes = 0
    capped = 0
    for c, items in cand.items():
        uniq = {}
        for inst, mat, nv in items:
            key = (inst, nv)
            if key in uniq:
                merged_dupes += 1
            else:
                uniq[key] = (inst, mat, nv)
        arr = sorted(uniq.values(), key=lambda k: k[2])
        if len(arr) > poly_cap:
            capped += len(arr) - poly_cap
            arr = arr[-poly_cap:]          # 保留音高最高的（同轨音高相近）
        col_notes[c] = arr
    return col_notes, {
        'notes': len(indices),
        'out_of_range': out_of_range,
        'merged_duplicates': merged_dupes,
        'capped': capped,
        'max_per_col': max((len(v) for v in col_notes.values()), default=0),
        'used_columns': len(col_notes),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--midi', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--lanes', type=int, default=3, help='轨道数（默认 3）')
    ap.add_argument('--split', choices=('pitch', 'bank'), default='pitch')
    ap.add_argument('--resolution', type=int, default=2, choices=(2, 4, 6, 8))
    ap.add_argument('--poly-cap', type=int, default=20, help='每轨单列复音上限')
    ap.add_argument('--lane-gap', type=int, default=2, help='相邻轨道 Z 间隔（格）')
    ap.add_argument('--json', help='统计另存 JSON')
    args = ap.parse_args()

    if args.lanes < 1:
        raise SystemExit('--lanes 必须 >= 1')
    res = args.resolution
    col_seconds = res / 20.0

    notes = g.load_notes(args.midi)
    if not notes:
        raise SystemExit('MIDI 没有音符')
    pitches = [p for p, _s, _e in notes]
    conv, median, shift = g.build_pitch_map(pitches, None)

    last_end = max(e for _p, _s, e in notes)
    columns = max(1, int(round(last_end / col_seconds)))

    lanes_assigned = assign_lanes(notes, conv, args.lanes, args.split)
    lane_data = {}
    global_max_per_col = 0
    for lane, idxs in lanes_assigned.items():
        if not idxs:
            continue
        col_notes, stats = build_lane_columns(notes, idxs, conv, col_seconds,
                                              columns, args.poly_cap)
        if not col_notes:
            continue
        lane_data[lane] = (col_notes, stats)
        global_max_per_col = max(global_max_per_col, stats['max_per_col'])

    if not lane_data:
        raise SystemExit('所有轨道都没有音符')

    # 所有轨道共享同一条时间轴列集合，保证 X 方向骨架与脉冲前后对齐
    union_cols = sorted({c for col_notes, _stats in lane_data.values()
                         for c in col_notes})

    # 轨道宽度 = 单列最大槽位对应的条带宽度 + 间隔
    base_pitch = 2 + max(1, global_max_per_col) + 1
    lane_width = base_pitch + args.lane_gap
    offsets = lane_offsets(args.lanes, lane_width)

    merged = {}
    lane_reports = {}
    for lane, (col_notes, stats) in sorted(lane_data.items()):
        z_off = offsets[lane]
        (blocks, _tap, _bus, _reps, _nbs, _cl, _zf, _yf, _yc, _lys, _win,
         _nstrip, _pitch, _slot0, _maxslot) = g.build(
            columns, res, col_notes, stats['max_per_col'], simple=True, track=None,
            timeline=union_cols)
        lane_blocks = {}
        for (x, y, z), (name, props) in blocks.items():
            if name in ('minecraft:lever', 'minecraft:observer'):
                continue        # 起始源统一由中心共享
            pos = (x, y, z + z_off)
            if pos in merged and merged[pos][0] != name:
                raise SystemExit(f'轨道 {lane} 在 {pos} 与已有方块冲突')
            lane_blocks[pos] = (name, dict(props))
        merged.update(lane_blocks)
        lane_reports[lane] = dict(stats, z_offset=z_off,
                                  pitch_min=min(p for p, _s, _e in
                                                (notes[i] for i in lanes_assigned[lane])),
                                  pitch_max=max(p for p, _s, _e in
                                                (notes[i] for i in lanes_assigned[lane])))

    # 共享起始总线：一个拉杆 + observer 驱动 x=-2 的整条 Z 向红石粉
    z_min, z_max = min(offsets), max(offsets)
    merged[(-4, 1, 0)] = ('minecraft:lever',
                          {'face': 'floor', 'facing': 'west', 'powered': 'false'})
    merged[(-3, 1, 0)] = ('minecraft:observer', {'facing': 'west', 'powered': 'false'})
    for z in range(z_min, z_max + 1):
        merged[(-2, 1, z)] = ('minecraft:redstone_wire', {'power': '0'})
        merged.setdefault((-2, 0, z), ('minecraft:smooth_quartz', {}))
    for x in (-4, -3):
        merged.setdefault((x, 0, 0), ('minecraft:smooth_quartz', {}))

    # 重算红石粉连接属性（视觉；放置后 MC 也会重算）
    for pos, (name, props) in list(merged.items()):
        if name != 'minecraft:redstone_wire':
            continue
        x, y, z = pos
        p = {'power': '0'}
        for key, (dx, dz) in g.DIRS.items():
            nb = merged.get((x + dx, y, z + dz))
            if nb and nb[0] in g.WIRE_CONNECTABLE:
                p[key] = 'side'
            elif merged.get((x + dx, y + 1, z + dz), (None,))[0] == 'minecraft:redstone_wire':
                p[key] = 'up'
            else:
                p[key] = 'none'
        merged[pos] = (name, p)

    size = g.write_nbt(args.out, merged)
    dist = g.simulate(merged)
    note_positions = [p for p, (n, _) in merged.items() if n == 'minecraft:note_block']
    unreachable = sum(1 for p in note_positions if p not in dist)
    adjacency_bad = g.note_dust_adjacency(merged)
    bus_half = max(abs(z_min), abs(z_max))

    report = {
        'out': args.out,
        'midi': args.midi,
        'lanes_requested': args.lanes,
        'lanes_used': len(lane_data),
        'split': args.split,
        'resolution': res,
        'cell_seconds': col_seconds,
        'columns': columns,
        'global_median': median,
        'global_shift_semitones': shift,
        'lane_width': lane_width,
        'lane_offsets': offsets,
        'bus_z_span': [z_min, z_max],
        'bus_max_distance': bus_half,
        'lanes': lane_reports,
        'size': list(size),
        'blocks': len(merged),
        'note_blocks': len(note_positions),
        'unreachable_note_blocks': unreachable,
        'note_dust_adjacency': len(adjacency_bad),
    }

    print(f'[MIDI] 音符={len(notes)}  音域={min(pitches)}~{max(pitches)}  时长={last_end:.2f}s')
    print(f'[移调] 中位数={median} → {shift:+d} 半音（单轨三档映射）')
    print(f'[量化] resolution={res}（{col_seconds:.3f}s），共 {columns} 列')
    for lane, r in sorted(lane_reports.items()):
        print(f'  轨 {lane}: z={r["z_offset"]:+d}  音符={r["notes"]}  音域={r["pitch_min"]}~{r["pitch_max"]}  '
              f'列={r["used_columns"]}  峰值={r["max_per_col"]}  '
              f'去重={r["merged_duplicates"]}  上限丢弃={r["capped"]}')
    print(f'[总线] Z 跨度 {z_min}~{z_max}，中心最远 {bus_half} 格'
          + ('  [警告] >14，远端轨道可能收不到起始脉冲' if bus_half > 14 else ''))
    print(f'[结构] size={size} 方块={len(merged)} 音符盒={len(note_positions)}')
    print(f'[校验] 音符盒不可达={unreachable}，音符盒贴粉={len(adjacency_bad)}')
    print(f'[产物] {args.out}')

    if args.json:
        with open(args.json, 'w', encoding='utf-8') as fh:
            json.dump(report, fh, ensure_ascii=False, indent=2)
        print(f'[统计] {args.json}')
    if unreachable or adjacency_bad:
        raise SystemExit('自校验失败：存在不可达音符盒或音符盒贴粉')
    return 0


if __name__ == '__main__':
    sys.exit(main())
