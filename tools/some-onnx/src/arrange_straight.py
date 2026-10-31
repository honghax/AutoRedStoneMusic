# -*- coding: utf-8 -*-
"""arrange_straight.py — 独立工具：多角色 MIDI → 直排红石音乐 NBT（不打包进模组）。

借鉴 McMusicMaker 的分声部/量化/复音上限思路，但保留本项目「原版音符盒 + 单条
红石跑道」的产物形态（等价 gen_machine 的 --layout simple）。

链路:
    多角色 MIDI（bass/vocals/other/drums ...）
      → 角色 → 音色档位（bass 低档 / vocals·other 中档 / drums basedrum）
      → 全曲统一移调（所有有音高角色共用同一个 shift，保证声部间音程不断）
      → 量化到 resolution 刻网格（默认 2 刻 = 0.1s）
      → 同列 (乐器,note) 去重
      → 单列复音上限 + 溢出丢音（默认优先级：lead/vocals > other/pad > bass > drums）
      → gen_machine.build(simple=True) → .nbt

用法:
    python arrange_straight.py --out debug.nbt \
        --role drums=drums.mid --role bass=bass.mid \
        --role other=other.mid --role vocals=vocals.mid \
        --resolution 2 --poly-cap 12 --overflow priority

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

# 角色 → (乐器, 垫块, 该档最低 MIDI)
ROLE_BANK = {
    'bass':   ('bass',   'minecraft:bamboo_planks', 30),
    'vocals': ('harp',   'minecraft:dirt',          54),
    'other':  ('harp',   'minecraft:dirt',          54),
    'lead':   ('harp',   'minecraft:dirt',          54),
    'pad':    ('harp',   'minecraft:dirt',          54),
    'piano':  ('harp',   'minecraft:dirt',          54),
    'guitar': ('harp',   'minecraft:dirt',          54),
}
DRUM = ('basedrum', 'minecraft:stone', 0)
# 溢出时保留优先级（数字越小越优先）
ROLE_PRIORITY = {
    'lead': 0, 'vocals': 0,
    'other': 1, 'piano': 1,
    'pad': 2, 'guitar': 2,
    'bass': 3, 'drums': 4,
}
NOTE_CENTER = 66   # harp 档中心（base 54 + note 12）


def parse_roles(items):
    roles = {}
    for item in items:
        if '=' not in item:
            raise SystemExit(f'--role 需要 NAME=PATH 形式，收到: {item}')
        name, path = item.split('=', 1)
        name = name.strip().lower()
        if name != 'drums' and name not in ROLE_BANK:
            raise SystemExit(f'未知角色 {name}；可用: {sorted(list(ROLE_BANK) + ["drums"])}')
        if not os.path.isfile(path):
            raise SystemExit(f'MIDI 不存在: {path}')
        roles[name] = path
    if not roles:
        raise SystemExit('至少需要一个 --role NAME=PATH')
    return roles


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True, help='输出 .nbt 路径')
    ap.add_argument('--role', action='append', default=[], metavar='NAME=PATH',
                    help='可重复；角色: bass/vocals/other/lead/pad/piano/guitar/drums')
    ap.add_argument('--resolution', type=int, default=2, choices=(2, 4, 6, 8),
                    help='每列游戏刻数（默认 2 = 0.1s）')
    ap.add_argument('--poly-cap', type=int, default=12, help='单列复音上限（默认 12）')
    ap.add_argument('--overflow', choices=('priority', 'highest'), default='priority',
                    help='超上限时的丢音策略：priority=按角色优先级，highest=保留最高音')
    ap.add_argument('--shift', default='auto',
                    help='全局基准移调半音数（0..11）；auto=搜索使八度折回最少的值（默认）')
    ap.add_argument('--no-role-octave', action='store_true',
                    help='禁用角色级八度归位（默认允许每个角色整体 ±0/±12/±24 归位）')
    ap.add_argument('--json', help='统计另存 JSON')
    args = ap.parse_args()

    roles = parse_roles(args.role)
    res = args.resolution
    col_seconds = res / 20.0

    role_notes = {role: g.load_notes(path) for role, path in roles.items()}
    role_counts = {role: len(n) for role, n in role_notes.items()}
    if sum(role_counts.values()) == 0:
        raise SystemExit('所有角色 MIDI 都没有音符')

    # 全曲统一移调：所有有音高角色共用同一个 shift，避免各声部独立移调导致音程断裂。
    # auto 在 ±12 内选「八度折回音符数最少」的 shift（并列时取更接近原调的）。
    pitched = [p for role, notes in role_notes.items() if role != 'drums'
               for p, _s, _e in notes]
    global_median = statistics.median(pitched) if pitched else 0

    def fold_steps(pitch, base):
        n = pitch - base
        steps = 0
        while n < 0:
            n += 12
            steps += 1
        while n > 24:
            n -= 12
            steps += 1
        return steps

    def role_cost(role, base, shift, octave):
        affected = steps = 0
        for p, _s, _e in role_notes[role]:
            f = fold_steps(p + shift + 12 * octave, base)
            if f:
                affected += 1
                steps += f
        return affected, steps

    def best_role_octave(role, base, shift):
        """角色级八度归位：整条声部只按 ±12 的整数倍平移，保留音级与声部内音程。"""
        if args.no_role_octave:
            return 0
        best = None
        for o in range(-2, 3):
            cost = role_cost(role, base, shift, o)
            key = (cost[0], cost[1], abs(o))
            if best is None or key < best[0]:
                best = (key, o)
        return best[1]

    def total_cost(shift):
        affected = steps = 0
        octaves = {}
        for role in role_notes:
            if role == 'drums':
                continue
            base = ROLE_BANK[role][2]
            o = best_role_octave(role, base, shift)
            octaves[role] = o
            cost = role_cost(role, base, shift, o)
            affected += cost[0]
            steps += cost[1]
        return affected, steps, octaves

    if args.shift == 'auto':
        best = None
        for s in range(0, 12):
            affected, steps, octaves = total_cost(s)
            key = (affected, steps, s)
            if best is None or key < best[0]:
                best = (key, s, octaves)
        global_shift, role_octave = best[1], best[2]
    else:
        global_shift = int(args.shift) % 12
        _affected, _steps, role_octave = total_cost(global_shift)

    # 时长 → 列数（列是时间量化网格）
    last_end = max(e for notes in role_notes.values() for _p, _s, e in notes)
    columns = max(1, int(round(last_end / col_seconds)))

    candidates = defaultdict(list)
    folded_notes = 0
    fold_steps_total = 0
    folded_by_role = Counter()
    out_of_range = 0
    for role, notes in role_notes.items():
        for pitch, start, _end in notes:
            c0 = int(round(start / col_seconds))
            if not (0 <= c0 < columns):
                out_of_range += 1
                continue
            if role == 'drums':
                inst, mat, nv = DRUM
            else:
                inst, mat, base = ROLE_BANK[role]
                q = int(pitch) + global_shift + 12 * role_octave.get(role, 0)
                f = fold_steps(q, base)
                nv = q - base
                while nv < 0:
                    nv += 12
                while nv > 24:
                    nv -= 12
                if f:
                    folded_notes += 1
                    fold_steps_total += f
                    folded_by_role[role] += 1
            candidates[c0].append({'role': role, 'inst': inst, 'mat': mat, 'nv': nv})

    # 同列去重 + 复音上限
    col_notes = {}
    merged_duplicates = 0
    capped = 0
    capped_by_role = Counter()
    for c, items in candidates.items():
        best = {}
        for it in items:
            key = (it['inst'], it['nv'])
            if key not in best:
                best[key] = it
            else:
                merged_duplicates += 1
                if ROLE_PRIORITY[it['role']] < ROLE_PRIORITY[best[key]['role']]:
                    best[key] = it
        arr = list(best.values())
        if len(arr) > args.poly_cap:
            if args.overflow == 'highest':
                arr.sort(key=lambda x: -x['nv'])
            else:
                arr.sort(key=lambda x: (ROLE_PRIORITY[x['role']], -x['nv']))
            for dropped in arr[args.poly_cap:]:
                capped += 1
                capped_by_role[dropped['role']] += 1
            arr = arr[:args.poly_cap]
        col_notes[c] = sorted(((x['inst'], x['mat'], x['nv']) for x in arr),
                              key=lambda k: (k[2], k[0]))

    max_per_col = max((len(v) for v in col_notes.values()), default=0)
    if max_per_col > g.Z_CAP:
        raise SystemExit(f'单列音符 {max_per_col} 超过结构容量 {g.Z_CAP}（未截断）')

    (blocks, tap_of_col, bus_dust, branch_reps, note_blocks, col_layer,
     zfolds, yfold_climbs, yfold_cells, layer_ys, window,
     N_STRIP, PITCH, SLOT0, MAX_SLOT) = g.build(
        columns, res, col_notes, max_per_col, simple=True, track=None)

    size = g.write_nbt(args.out, blocks)
    dist = g.simulate(blocks)
    unreachable = sum(1 for nbs in note_blocks.values() for p in nbs if p not in dist)
    adjacency_bad = g.note_dust_adjacency(blocks)

    deduped_notes = sum(len(v) for v in col_notes.values())
    report = {
        'out': args.out,
        'role_counts': role_counts,
        'global_median': global_median,
        'global_shift_semitones': global_shift,
        'role_octave_semitones': {r: 12 * o for r, o in role_octave.items()},
        'effective_shift_by_role': {r: global_shift + 12 * o
                                    for r, o in role_octave.items()},
        'resolution': res,
        'cell_seconds': col_seconds,
        'columns': columns,
        'used_columns': len(col_notes),
        'max_polyphony': max_per_col,
        'poly_cap': args.poly_cap,
        'overflow_policy': args.overflow,
        'input_notes': sum(role_counts.values()),
        'output_notes': deduped_notes,
        'merged_duplicates': merged_duplicates,
        'capped_by_limit': capped,
        'capped_by_role': dict(capped_by_role),
        'octave_folded_notes': folded_notes,
        'octave_fold_pct': folded_notes / max(1, len(pitched)) * 100,
        'octave_fold_steps': fold_steps_total,
        'octave_folded_by_role': dict(folded_by_role),
        'out_of_range_dropped': out_of_range,
        'size': list(size),
        'blocks': len(blocks),
        'note_blocks': sum(len(v) for v in note_blocks.values()),
        'unreachable_note_blocks': unreachable,
        'note_dust_adjacency': len(adjacency_bad),
    }

    print(f'[角色] ' + ', '.join(f'{k}={v}' for k, v in sorted(role_counts.items())))
    octave_desc = ', '.join(f'{r}{12 * o:+d}' for r, o in sorted(role_octave.items()))
    print(f'[移调] 全曲中位数={global_median}；全局基准 {global_shift:+d} 半音'
          + (f'；角色八度归位 {octave_desc}' if octave_desc else '')
          + f'（{args.shift}）')
    print(f'[量化] resolution={res}（每列 {col_seconds:.3f}s），共 {columns} 列，'
          f'有音符 {len(col_notes)} 列')
    print(f'[去重] 同列 (乐器,note) 重复合并 {merged_duplicates} 个')
    print(f'[复音] 单列峰值 {max_per_col}，上限 {args.poly_cap}（{args.overflow}），'
          f'超限丢弃 {capped} 个' + (f' {dict(capped_by_role)}' if capped else ''))
    fold_pct = folded_notes / max(1, len(pitched)) * 100
    print(f'[音高] 八度折回 {folded_notes} 个音符（{fold_pct:.1f}%，共 {fold_steps_total} 步）'
          + (f' {dict(folded_by_role)}' if folded_notes else '')
          + f'；超时长丢弃 {out_of_range} 个')
    if fold_pct > 25:
        print('       [警告] 折回比例 >25%，旋律在该档位窗口边缘会听出跳变')
    print(f'[结构] 直排 size={size} 方块={len(blocks)} 音符盒={report["note_blocks"]}')
    print(f'[校验] 音符盒不可达={unreachable}，音符盒贴粉={len(adjacency_bad)}')
    print(f'[产物] {args.out}')

    if args.json:
        with open(args.json, 'w', encoding='utf-8') as fh:
            json.dump(report, fh, ensure_ascii=False, indent=2)
        print(f'[统计] {args.json}')
    if unreachable or adjacency_bad:
        raise SystemExit('自校验失败：结构存在不可达音符盒或音符盒贴粉')
    return 0


if __name__ == '__main__':
    sys.exit(main())
