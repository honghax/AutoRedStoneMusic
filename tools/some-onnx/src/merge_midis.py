# -*- coding: utf-8 -*-
"""把多个 MIDI 合并成一个 MIDI（音符时间轴合并，丢弃弯音）。

用于多音轨流程：demucs 分离的各 stem 转写为 MIDI 后，
合并成一个完整乐曲 MIDI，再交给 gen_machine.py 单轨三档映射生成一条结构。

用法：
  python merge_midis.py <a.mid> <b.mid> ... <merged.mid>
"""
import mido
import sys

TICKS = 480        # 输出 ticks_per_beat
TEMPO = 500000     # 120bpm，与 basic-pitch 输出一致


def notes_from(path):
    """读取一个 MIDI 的全部音符：[(起始秒, 时长秒, 音高)]。"""
    mf = mido.MidiFile(str(path))
    out = []
    for tr in mf.tracks:
        tempo = TEMPO
        now = 0.0
        pending = {}
        for msg in tr:
            now += mido.tick2second(msg.time, mf.ticks_per_beat, tempo)
            if msg.type == 'set_tempo':
                tempo = msg.tempo
            elif msg.type == 'note_on' and msg.velocity > 0:
                pending[msg.note] = now
            elif msg.type in ('note_off', 'note_on'):
                if msg.note in pending:
                    start = pending.pop(msg.note)
                    if now > start:
                        out.append((start, now - start, msg.note))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    inputs = sys.argv[1:-1]
    output = sys.argv[-1]

    notes = []
    for ch, p in enumerate(inputs):
        # 每个源 MIDI 分配一个独立 channel：不同 stem 的同音高音符不再互相覆盖
        notes += [(s, d, n, ch) for s, d, n in notes_from(p)]
    notes.sort(key=lambda x: x[0])

    scale = TICKS * (1e6 / TEMPO)   # 1 秒 = 960 ticks
    events = []
    for s, d, n, ch in notes:
        t_on = int(round(s * scale))
        # 时长至少 1 tick：超短音符（on/off 同 tick）读回时会被 now>start 判为 0 时长丢弃
        t_off = max(t_on + 1, int(round((s + d) * scale)))
        events.append((t_on, 'on', n, ch))
        events.append((t_off, 'off', n, ch))
    # 同 tick 时 off 排 on 前：相邻同音高音符首尾相接（A.off==B.on 同 tick）时，
    # 若 on 在前会被读回误配对成 0 时长；off 先释放再 on 占用则配对正确。
    events.sort(key=lambda e: (e[0], 1 if e[1] == 'on' else 0))

    mf = mido.MidiFile(ticks_per_beat=TICKS)
    t0 = mido.MidiTrack()
    t0.append(mido.MetaMessage('set_tempo', tempo=TEMPO, time=0))
    mf.tracks.append(t0)
    tr = mido.MidiTrack()
    now = 0
    for t, kind, n, ch in events:
        tr.append(mido.Message('note_on' if kind == 'on' else 'note_off',
                               note=n, velocity=80 if kind == 'on' else 0,
                               channel=ch, time=t - now))
        now = t
    mf.tracks.append(tr)
    mf.save(output)
    pitches = [n for _, _, n, _ in notes]
    print(f'合并 {len(notes)} 音符（音域 {min(pitches)}~{max(pitches)}，{len(inputs)} 轨） -> {output}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
