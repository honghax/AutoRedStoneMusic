# -*- coding: utf-8 -*-
"""MuScriptor → 多乐器多轨红石结构（独立原型）。

流程：
  1) 音频 → MuScriptor 在线转写（可选），或直接读取现成的多轨 MIDI；
  2) 按 McMusicMaker 的轨道跟踪逻辑，把每条 MIDI 轨道/程序映射到乐器角色：
       channel 9 → drum；否则 GM program // 8 → pno/pad/lead/bass/drum；
  3) 每个音符按“轨道角色”决定乐器（bass / harp / bell / basedrum），
     再按乐器拆成并行轨道：轨道数 = 乐器数，每条轨道只放这一类乐器；
  4) 各轨共享同一条 X 时间轴与同一个拉杆起始总线，复用 gen_machine.build(simple=True)
     生成一条多轨 NBT 结构。

在线转写使用网页版内部接口 POST /transcribe（SSE）。它不是官方公开 API，
只在用户明确同意上传音频时使用，并且只允许单任务、失败重试有限。

用法：
  # 已有多轨 MIDI（不会联网）
  python muscriptor_to_nbt.py --midi test2.mid --out test2_multi.nbt

  # 在线转写后生成 NBT
  python muscriptor_to_nbt.py --audio song.mp3 --out song_multi.nbt --consent-online

  # 自建 muscriptor serve
  python muscriptor_to_nbt.py --audio song.mp3 --out song_multi.nbt \
      --consent-online --endpoint http://127.0.0.1:8222/transcribe
"""
import argparse
import base64
import json
import os
import statistics
import sys
import time
import uuid
from collections import Counter, defaultdict

import mido
import requests

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_machine as g

# 与 McMusicMaker/poc/redpiano_map_multi.json 的 family_map 保持一致
FAMILY_MAP = {
    0: 'pno', 1: 'pno', 2: 'pad', 3: 'lead', 4: 'bass',
    5: 'pad', 6: 'pad', 7: 'lead', 8: 'lead', 9: 'lead',
    10: 'lead', 11: 'pad', 12: 'pad', 13: 'lead', 14: 'drum', 15: 'drum',
}

# 我们可用的原版音色档位：最低 MIDI、乐器名、垫块
BANK_INFO = {
    'bass': (30, 'bass', 'minecraft:bamboo_planks'),
    'harp': (54, 'harp', 'minecraft:dirt'),
    'bell': (78, 'bell', 'minecraft:gold_block'),
}

DRUM_INFO = ('basedrum', 'minecraft:stone', 0)

# 乐器轨顺序（Z 方向）：bass 在一端、旋律在中间、鼓在另一端
LANE_ORDER = ('bass', 'harp', 'bell', 'basedrum')
BUS_LIMIT = 14                # 0 延迟共享总线：最远轨道到中心 <=14 格


def classify(channel, program, track_name):
    """McMusicMaker 风格轨道跟踪：channel 9 → drum，否则 GM program // 8 → family。"""
    name = (track_name or '').lower()
    if channel == 9 or 'drum' in name:
        return 'drum'
    family = FAMILY_MAP.get(program // 8, 'lead')
    return family


def parse_notes(path):
    """返回每条音符的 dict，带轨道、channel、program、role。"""
    mf = mido.MidiFile(path)
    notes = []
    for track_index, track in enumerate(mf.tracks):
        tempo = 500000
        now = 0.0
        program = 0
        track_name = ''
        pending = {}
        for msg in track:
            now += mido.tick2second(msg.time, mf.ticks_per_beat, tempo)
            if msg.type == 'set_tempo':
                tempo = msg.tempo
            elif msg.type == 'track_name':
                track_name = msg.name
            elif msg.type == 'program_change':
                program = msg.program
            elif msg.type == 'note_on' and msg.velocity > 0:
                pending[(msg.channel, msg.note)] = (now, program, track_name)
            elif msg.type in ('note_off', 'note_on'):
                key = (msg.channel, msg.note)
                if key in pending:
                    start, prog, name = pending.pop(key)
                    if now > start:
                        notes.append({
                            'pitch': msg.note,
                            'start': start,
                            'end': now,
                            'channel': msg.channel,
                            'program': prog,
                            'track': track_index,
                            'track_name': name or f'track {track_index}',
                            'role': classify(msg.channel, prog, name),
                        })
    notes.sort(key=lambda n: (n['start'], n['track'], n['pitch']))
    return notes, mf


def mime_for(path):
    ext = os.path.splitext(path)[1].lower()
    return {
        '.mp3': 'audio/mpeg',
        '.wav': 'audio/wav',
        '.flac': 'audio/flac',
        '.ogg': 'audio/ogg',
        '.m4a': 'audio/mp4',
        '.aiff': 'audio/aiff',
        '.opus': 'audio/opus',
    }.get(ext, 'application/octet-stream')


def online_transcribe(audio, midi_out, endpoint, instruments, consent,
                      max_retries, retry_delay):
    if not consent:
        raise SystemExit('在线转写会把音频上传到 MuScriptor 服务器；'
                         '必须加 --consent-online 明确同意。')

    client_id = str(uuid.uuid4())
    headers = {
        'Origin': 'https://muscriptor.kyutai.org',
        'Referer': 'https://muscriptor.kyutai.org/',
        'X-Client-Id': client_id,
    }

    for attempt in range(1, max_retries + 1):
        with open(audio, 'rb') as fh:
            form = [('instruments', inst) for inst in instruments]
            resp = requests.post(
                endpoint,
                files={'file': (os.path.basename(audio), fh, mime_for(audio))},
                data=form,
                headers=headers,
                stream=True,
                timeout=(30, 300),
            )
        print(f'[online] attempt {attempt}: HTTP {resp.status_code}')
        if resp.status_code == 503:
            resp.close()
            wait = retry_delay
            print(f'[online] server busy, retry in {wait:.1f}s')
            time.sleep(wait)
            continue
        if resp.status_code != 200:
            print(resp.text[:500])
            raise SystemExit(f'MuScriptor 返回 HTTP {resp.status_code}')

        events = 0
        for line in resp.iter_lines(decode_unicode=True):
            if not line or not line.startswith('data: '):
                continue
            event = json.loads(line[6:])
            events += 1
            kind = event.get('type')
            if kind == 'progress':
                print(f"[online] progress {event.get('completed')}/{event.get('total')}")
            elif kind == 'transcription_complete':
                raw = base64.b64decode(event['data'])
                with open(midi_out, 'wb') as out:
                    out.write(raw)
                print(f'[online] MIDI {len(raw)} bytes -> {midi_out} ({events} events)')
                return midi_out
        raise SystemExit('MuScriptor 流结束但没有 transcription_complete')

    raise SystemExit(f'MuScriptor 连续 {max_retries} 次 503，稍后再试')


def map_note(note, shift):
    """按轨道角色映射到音符盒乐器；不按整曲中/高/低分档。"""
    role = note['role']
    if role == 'drum':
        return DRUM_INFO

    q = int(note['pitch']) + shift
    if role == 'bass':
        bank = 'bass'
    else:
        # 轨道保持 pno/pad/lead 的竖琴音色；只在超出 F#3~F#5 时借用低/高音档
        if q < 54:
            bank = 'bass'
        elif q > 78:
            bank = 'bell'
        else:
            bank = 'harp'

    base, inst, mat = BANK_INFO[bank]
    n = q - base
    while n < 0:
        n += 12
    while n > g.NOTE_COUNT - 1:
        n -= 12
    return inst, mat, n


def lane_layout(lane_cols, gap):
    """按每条乐器轨的实际槽宽首尾相接排布 Z，再整体居中。返回 (偏移表, 总线半径)。"""
    order = [inst for inst in LANE_ORDER if inst in lane_cols]
    offsets, pos = {}, 0
    for inst in order:
        offsets[inst] = pos
        max_per = max((len(items) for items in lane_cols[inst].values()), default=0)
        # 与 gen_machine.build 的单条带宽一致：主干 1 + 跨接 1 + 音符槽 max_per + 末格间隔 1
        pos += 2 + max(1, max_per) + 1 + gap
    if not order:
        return {}, 0
    shift = -int(round(offsets[order[-1]] / 2.0))
    offsets = {inst: off + shift for inst, off in offsets.items()}
    return offsets, max(abs(off) for off in offsets.values())


def build_multi_nbt(notes, out_path, resolution, shift_mode, lane_gap):
    if not notes:
        raise SystemExit('MIDI 中没有音符')

    pitched = [n['pitch'] for n in notes if n['role'] != 'drum']
    if shift_mode == 'auto' and pitched:
        median = statistics.median(pitched)
        shift = int(round((g.NOTE_BASE_MIDI + g.NOTE_CENTER) - median))
    else:
        shift = int(shift_mode)

    col_seconds = resolution / g.MC_TICKS_PER_SEC
    last_end = max(n['end'] for n in notes)
    columns = max(1, int(round(last_end / col_seconds)))

    # 量化 + 乐器映射 → 按乐器（而不是按音高中/高/低）分轨
    lane_cols = defaultdict(lambda: defaultdict(set))
    dropped = 0
    for note in notes:
        c0 = int(round(note['start'] / col_seconds))
        if not (0 <= c0 < columns):
            dropped += 1
            continue
        inst, mat, nv = map_note(note, shift)
        lane_cols[inst][c0].add((inst, mat, nv))

    offsets, bus_half = lane_layout(lane_cols, lane_gap)
    merged_into = None
    if bus_half > BUS_LIMIT and 'bell' in lane_cols:
        target = next((i for i in ('harp', 'bass', 'basedrum') if i in lane_cols), None)
        if target:
            for c, items in lane_cols.pop('bell').items():
                lane_cols[target][c] |= items
            merged_into = f'bell→{target}'
            offsets, bus_half = lane_layout(lane_cols, lane_gap)
    if bus_half > BUS_LIMIT:
        print(f'[警告] 总线跨度 ±{bus_half} 超过 {BUS_LIMIT} 格，远端轨道可能收不到起始脉冲')

    lane_max = {inst: max((len(items) for items in cols.values()), default=0)
                for inst, cols in lane_cols.items()}
    for inst, mx in lane_max.items():
        if mx > g.Z_CAP:
            raise SystemExit(f'轨道 {inst} 单列复音 {mx} 超过 Z 容量 {g.Z_CAP}')

    # 所有轨道共享同一列集合，保证 X 骨架与脉冲前后对齐
    union_cols = sorted({c for cols in lane_cols.values() for c in cols})

    merged = {}
    lane_reports = {}
    for inst in [i for i in LANE_ORDER if i in lane_cols]:
        cols = lane_cols[inst]
        col_notes = {c: sorted(items, key=lambda k: (k[2], k[0]))
                     for c, items in cols.items()}
        blocks, *_ = g.build(columns, resolution, col_notes, lane_max[inst],
                             simple=True, timeline=union_cols)
        z_off = offsets[inst]
        for (x, y, z), (name, props) in blocks.items():
            if name in ('minecraft:lever', 'minecraft:observer'):
                continue        # 起始源统一由中心共享
            pos = (x, y, z + z_off)
            if pos in merged and merged[pos][0] != name:
                raise SystemExit(f'轨道 {inst} 在 {pos} 与已有方块冲突')
            merged[pos] = (name, dict(props))
        inst_used = Counter(k[0] for items in cols.values() for k in items)
        lane_reports[inst] = {
            'z_offset': z_off,
            'notes': sum(len(items) for items in cols.values()),
            'used_columns': len(cols),
            'max_per_col': lane_max[inst],
            'instruments': dict(inst_used),
        }

    # 共享起始总线：一个拉杆 + observer 驱动 x=-2 的整条 Z 向红石粉，所有轨道同一刻起步
    z_min, z_max = min(offsets.values()), max(offsets.values())
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

    size = g.write_nbt(out_path, merged)
    dist = g.simulate(merged)
    note_positions = [p for p, (name, _) in merged.items()
                      if name == 'minecraft:note_block']
    unreachable = sum(1 for p in note_positions if p not in dist)
    adjacency = g.note_dust_adjacency(merged)

    role_counts = Counter(n['role'] for n in notes)
    inst_counts = Counter()
    for cols in lane_cols.values():
        for items in cols.values():
            for inst, _mat, _n in items:
                inst_counts[inst] += 1
    track_counts = defaultdict(Counter)
    for n in notes:
        track_counts[n['track_name']][n['role']] += 1

    report = {
        'midi_notes': len(notes),
        'columns': columns,
        'resolution': resolution,
        'shift': shift,
        'dropped': dropped,
        'lanes': [i for i in LANE_ORDER if i in lane_cols],
        'lane_gap': lane_gap,
        'bus_half': bus_half,
        'merged_lane': merged_into,
        'lane_reports': lane_reports,
        'size': list(size),
        'blocks': len(merged),
        'note_blocks': len(note_positions),
        'unreachable': unreachable,
        'note_dust_adjacency': len(adjacency),
        'roles': dict(role_counts),
        'instruments': dict(inst_counts),
        'tracks': {name: dict(counts) for name, counts in track_counts.items()},
    }

    print(f'[MIDI] 音符={len(notes)} 轨道={len(track_counts)} 时长={last_end:.2f}s')
    for name, counts in sorted(track_counts.items()):
        print(f'  轨道 {name!r}: {dict(counts)}')
    print(f'[角色] {dict(role_counts)}')
    print(f'[乐器] {dict(inst_counts)}')
    print(f'[分轨] 轨道数={len(lane_reports)} 乐器={list(lane_reports)}'
          + (f'  合并 {merged_into}' if merged_into else ''))
    for inst, r in lane_reports.items():
        print(f'  轨 {inst}: z={r["z_offset"]:+d} 音符={r["notes"]} 列={r["used_columns"]} '
              f'峰值={r["max_per_col"]} 乐器分布={r["instruments"]}')
    print(f'[总线] 最远轨道到中心 {bus_half} 格（上限 {BUS_LIMIT}）')
    print(f'[结构] size={size} 方块={len(merged)} 音符盒={len(note_positions)} 移调={shift:+d}')
    print(f'[校验] 不可达={unreachable} 音符盒贴粉={len(adjacency)} 越界丢弃={dropped}')
    print(f'[产物] {out_path}')
    return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--audio', help='在线转写的输入音频')
    ap.add_argument('--midi', help='已有 MIDI；给定时跳过在线转写')
    ap.add_argument('--midi-out', help='在线 MIDI 保存路径')
    ap.add_argument('--out', required=True, help='输出 .nbt')
    ap.add_argument('--endpoint', default='https://muscriptor.kyutai.org/transcribe')
    ap.add_argument('--instruments', action='append', default=[],
                    help='MuScriptor 条件乐器，可重复，例如 acoustic_piano')
    ap.add_argument('--consent-online', action='store_true',
                    help='明确同意把音频上传到 MuScriptor 服务器')
    ap.add_argument('--resolution', type=int, default=2, choices=(2, 4, 6, 8))
    ap.add_argument('--lane-gap', type=int, default=2, help='相邻乐器轨 Z 间隔（格）')
    ap.add_argument('--shift', default='0', help='0 / auto / 整数半音')
    ap.add_argument('--retries', type=int, default=12)
    ap.add_argument('--retry-delay', type=float, default=5.0)
    ap.add_argument('--json')
    args = ap.parse_args()

    if not args.midi:
        if not args.audio:
            raise SystemExit('需要 --midi，或 --audio + --consent-online')
        midi_path = args.midi_out or (os.path.splitext(args.out)[0] + '.mid')
        online_transcribe(args.audio, midi_path, args.endpoint,
                          args.instruments, args.consent_online,
                          args.retries, args.retry_delay)
        args.midi = midi_path

    notes, mf = parse_notes(args.midi)
    report = build_multi_nbt(notes, args.out, args.resolution, args.shift, args.lane_gap)
    if args.json:
        with open(args.json, 'w', encoding='utf-8') as fh:
            json.dump(report, fh, ensure_ascii=False, indent=2)
        print(f'[统计] {args.json}')
    if report['unreachable'] or report['note_dust_adjacency']:
        raise SystemExit('自校验失败：存在不可达音符盒或音符盒贴粉')
    return 0


if __name__ == '__main__':
    sys.exit(main())
