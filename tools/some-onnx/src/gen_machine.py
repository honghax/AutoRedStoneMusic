# -*- coding: utf-8 -*-
"""MIDI → 可自行运行的红石音乐机器（vanilla 结构 .nbt）生成器。

用法:
    python gen_machine.py <输入.mid> <输出.nbt> [--seconds 10] [--resolution 2]

时间模型:
    1 列 = --resolution 个游戏刻（默认 2 刻 = 0.1s），共 columns = seconds*20/resolution 列。
    抽头时刻 = resolution * 列号 (游戏刻)；音符发声 = resolution * 列号 + 2 (游戏刻，支线 delay=1=2刻)。
    中继器 delay 属性单位是**红石刻**(1 红石刻 = 2 游戏刻)，故属性 1..4 = 2/4/6/8 游戏刻 = 1..4 列。

拓扑（多层蛇形折叠；时序不变）:
    · facing 语义（实测）：facing = 读输入方向，输出在反面。正向层(主干 +X) 写 facing='west'，
      反向层(主干 -X) 写 facing='east'。
    · 主干每级占 2 格：中继器 → 其输出粉（= 本列抽头 & 前进下一级）→ 下一级中继器。
    · 只为“有音符的列”设抽头；相邻抽头列相差 g 列时，用尽量少的 delay=4(8刻) 中继器跨越 2g 刻，
      余数 r=2g%8 再加 1 个 delay=r//2 的中继器。稀疏段因此大幅缩短。
    · 多层折叠：每层主干 X 走窗口 [W_LO,W_HI]=[-4,30]（≤35 格），到边界即折返到上一层
      （层高 3：工作层 y=1,4,7,…，中间为空气隔层 + 支撑层）。折返用 3 步「横 1 + 升 1」对角
      红石粉阶梯（0 延迟）；台阶支撑必须是 glowstone（非不透明，否则切断对角连接）。
      层序蛇形：第 1 层 +X、第 2 层 -X、第 3 层 +X …，各层布局按前进方向镜像。
    · 支线从 z=2 起，与主干(z=0)隔 1 格（音符盒被强充能会向 6 邻供电，串到毗邻主干元件会
      提前触发、破坏时序）。总线粉 (b,1,2..N+1)、支线中继器 (b±1,1,z)、音符盒 (b±2,1,z)、材质
      dirt (b±2,0,z)，z 连续（同列紧贴无害）。抽头 → z=1 的一格同层跨接粉 → 总线。
    · 列间隔离：第 c 列音符盒 X 与第 c+1 列总线/支线元件 X 之间≥1 格空气 → 同层抽头间距 ≥4；
      不足时补 z=0 红石粉(0 延迟)。
    · 脉冲源：lever → 正对的 observer(facing=west)，输出在其反面 east = 列0 抽头 (-2,1,0)。
    · 末端指示灯：最后一个抽头后再加 1 级 delay=1 中继器，正面接 redstone_lamp（诊断脉冲是否走完）。
    · 支撑层：每个工作层下方铺 dirt（折返处的 glowstone 不被覆盖），并作音符盒的垫块/材质；
      其中真正决定乐器的是音符盒正下方那格：dirt→harp、bamboo_planks→bass、gold_block→bell，
      借此把音域从 harp 的 F#3~F#5 扩到 F#1~F#7（见 INSTRUMENT_BANKS）。
    · 红石粉对音符盒无效（已实测），必须靠支线中继器正向强充能。
"""
import argparse
import heapq
import math
import os
import statistics
import sys
from collections import defaultdict

import mido
import nbtlib
from nbtlib import Compound, File, Int, List, String

MC_TICKS_PER_SEC = 20.0        # 1 秒 = 20 游戏刻
NOTE_BASE_MIDI = 54           # note=0 对应 F#3 = MIDI 54（harp 档）
NOTE_COUNT = 25               # 音符盒音高 0..24
NOTE_CENTER = 12             # 音域中心
# 乐器垫块分档（借鉴 hyperchoron）：垫在音符盒下方的方块决定乐器及其自然基频，
# 于是把音域从单一 harp 的 F#3~F#5 扩到 F#1~F#7，超出的音不必硬折回、不产生同列撞车。
# 元组 = (note_block 的 instrument 属性, 垫块, 该档最低 MIDI)；note 值 = pitch - 最低MIDI ∈ [0,24]
INSTRUMENT_BANKS = (
    ('bass', 'minecraft:bamboo_planks', 30),   # F#1..F#3
    ('harp', 'minecraft:dirt', 54),            # F#3..F#5（dirt 即 harp 材质）
    ('bell', 'minecraft:gold_block', 78),      # F#5..F#7
)
DIRS = {'east': (1, 0), 'west': (-1, 0), 'south': (0, 1), 'north': (0, -1)}
WIRE_CONNECTABLE = ('minecraft:redstone_wire', 'minecraft:repeater',
                    'minecraft:observer', 'minecraft:lever')
Z_CAP = 128                   # 单列音符盒 Z 方向容量上限（超出则报错，不静默截断）
Z_FOLD_REPS = 2               # 每个 Z 折返走廊插入的中继器数（各 delay=1 → 各 2 游戏刻）
TIMING_TOL_TICKS = 10         # 抽头/发声刻数允许的绝对误差（游戏刻）；超过则报错


# --------------------------------------------------------------------------- #
# MIDI 读取 / 音高映射
# --------------------------------------------------------------------------- #
def load_notes(path):
    """返回 [(midi 音高, 起始秒, 结束秒), ...]（遍历所有轨道，逐轨维护 tempo）。"""
    mf = mido.MidiFile(str(path))
    out = []
    for tr in mf.tracks:
        tempo = 500000
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
                        out.append((msg.note, start, now))
    out.sort(key=lambda x: x[1])
    return out


def build_pitch_map(pitches):
    """按中位数对齐到 harp 音域中心，返回 (映射函数, 中位数, 移调量)。

    映射函数 f(p) -> (instrument, 垫块, note 0..24)：以 harp(F#3~F#5) 为中间档，
    偏低走 bass(F#1~F#3)、偏高走 bell(F#5~F#7)（借鉴 hyperchoron 的垫块分档）。
    只有超出 F#1~F#7 整体音域的极端音，才在档内按八度折回。
    """
    med = statistics.median(pitches)
    shift = int(round((NOTE_BASE_MIDI + NOTE_CENTER) - med))

    def conv(p):
        q = int(p) + shift
        bank = INSTRUMENT_BANKS[0] if q < 54 else (
            INSTRUMENT_BANKS[2] if q > 78 else INSTRUMENT_BANKS[1])
        inst, mat, base = bank
        n = q - base
        while n < 0:
            n += 12
        while n > NOTE_COUNT - 1:
            n -= 12
        return inst, mat, n

    return conv, med, shift


# --------------------------------------------------------------------------- #
# 结构生成
# --------------------------------------------------------------------------- #
def segment_delays(ticks):
    """把 ticks 个游戏刻拆成尽量少的中继器：先 delay=4(8刻)，余数 r 用 delay=r//2（r 为偶数）。"""
    ds = [4] * (ticks // 8)
    r = ticks % 8
    if r:
        ds.append(r // 2)          # r∈{2,4,6} → delay∈{1,2,3}，8刻×n + 2×delay == ticks
    return ds


def build(columns, resolution, col_notes, max_per_col, simple=False):
    """二维蛇形（层内多条带）构造。

    单条带 Z 结构（pitch = SLOT0 + MAX_SLOT + 1，MAX_SLOT 按本曲实际最大同列音符数收缩）：
    base+0 主干行 / +1 跨接行 / +2..+1+MAX_SLOT 音符槽 / 末格条带间隔。
    一层 6 条；条带内沿 X 走约 31 格，条带间同层折返（走廊里插 delay=1 中继器切开长粉段，
    其延迟已在段首从主干延迟里扣除）；6 条走满后按「对角步 + glowstone 支撑 + 0 中继器」升一层(层高 3)。
    层内方向交替。
    返回 (blocks, tap_of_col, bus_dust, branch_reps, note_blocks, col_layer,
          zfolds, yfold_climbs, yfold_cells, layer_ys, (X_MIN, X_MAX), N_STRIP, PITCH, SLOT0, MAX_SLOT)
    """
    blocks = {}

    def put(x, y, z, name, props=None):
        blocks[(x, y, z)] = (name, dict(props) if props else {})

    def make_rep(d, facing):
        return {'facing': facing, 'delay': str(d), 'locked': 'false', 'powered': 'false'}

    N_STRIP, SLOT0 = 6, 2
    MAX_SLOT = max(1, max_per_col)            # 条带容量按本曲实际最大同列音符数收缩，缩短走廊
    PITCH = SLOT0 + MAX_SLOT + 1              # base+0 主干 / +1 跨接 / +2..+1+MAX_SLOT 音符槽 / 末格条带间隔
    X_MIN, X_MAX = -2, 28                     # 主干行走（抽头）范围；折返走廊在其外 1 格
    st = {'x': -2, 'y': 1, 'dir': 1, 'base_z': 0, 'zdir': 1, 'in_layer': 0}
    acc = [0]                                 # 累计游戏刻（起点=列0抽头=0）
    layer_ys = {1}
    zfolds, yfold_climbs, yfold_cells = [], [], set()

    def facing():
        return 'west' if st['dir'] > 0 else 'east'

    def at_strip_end():
        if simple:                      # 简单版：不折返，主干一直沿 +X 走成一条直线
            return False
        return (st['dir'] > 0 and st['x'] >= X_MAX) or (st['dir'] < 0 and st['x'] <= X_MIN)

    def near_layer_end():
        if simple:
            return False
        # 最后一格条带且在 X 边界前 4 格：给走廊/对角步留位，避免音符盒贴到折返走廊
        return st['in_layer'] >= N_STRIP - 1 and (
            (st['dir'] > 0 and st['x'] >= X_MAX - 4) or (st['dir'] < 0 and st['x'] <= X_MIN + 4))

    def do_zfold():
        """同层同 Y 的 Z 向折返：X 段(4 格) → 拐角 → Z 段(14 格)，主体红石粉。
        st['fold_reps'] 决定插几个 delay=1 中继器（各 2 游戏刻，已在段首从主干延迟里扣除）：
          · 2 个：X 段首格 1 个（隔离折返列总线）+ Z 段中点 1 个（切开 14 格长粉段）；
          · 1 个：仅 Z 段偏前处 1 个（间隔太短、扣不起 4 刻时的退让方案）。"""
        x, y, bz, d, zd = st['x'], st['y'], st['base_z'], st['dir'], st['zdir']
        reps = st.get('fold_reps', Z_FOLD_REPS)
        xd = (X_MAX + 4) if d > 0 else (X_MIN - 4)
        xc = x
        if reps >= 2:
            xc = x + d
            put(xc, y, bz, 'minecraft:redstone_wire')            # 缓冲粉：中继器不可直连中继器
            xc = x + 2 * d
            put(xc, y, bz, 'minecraft:repeater', make_rep(1, 'west' if d > 0 else 'east'))
        while xc != xd:
            xc += d
            put(xc, y, bz, 'minecraft:redstone_wire')
        zm = max(1, PITCH - 1)      # Z 段中继器贴近远端：尾段只剩 1 格，给返回端的密列总线留足余量
        zfacing = 'north' if zd > 0 else 'south'
        for i in range(1, PITCH + 1):
            if i == zm:
                put(xd, y, bz + i * zd, 'minecraft:repeater', make_rep(1, zfacing))
            else:
                put(xd, y, bz + i * zd, 'minecraft:redstone_wire')
        zfolds.append(((x, y, bz), (xd, y, bz + PITCH * zd), reps))
        st['x'] = xd
        st['base_z'] = bz + PITCH * zd
        st['dir'] = -d
        st['in_layer'] += 1

    def sim_path(x, y, bz, d, zd, il, pad, ds, reps, simple=False):
        """不落方块地预演 pad 个 dust + len(ds) 个 repeater，返回 (z 折返次数, y 折返次数)。
        reps == Z_FOLD_REPS 时边界折返走 Z 折返（带 Z_FOLD_REPS 个 delay=1 中继器）；
        reps < Z_FOLD_REPS 时走 0 延迟的 Y 折返。simple 模式不折返，恒返回 (0, 0)。"""
        nz = ny = 0
        if simple:
            return nz, ny

        def chk():
            """返回是否发生折返（发生则调用方不得再前进一格）。"""
            nonlocal x, y, bz, d, zd, il, nz, ny
            if il >= N_STRIP - 1 and ((d > 0 and x >= X_MAX - 4) or (d < 0 and x <= X_MIN + 4)):
                x += d; y += 3; d = -d; zd = -zd; il = 0; ny += 1
                return True
            if (d > 0 and x >= X_MAX) or (d < 0 and x <= X_MIN):
                if reps >= Z_FOLD_REPS:
                    x = (X_MAX + 4) if d > 0 else (X_MIN - 4)
                    bz += PITCH * zd; d = -d; il += 1; nz += 1
                else:
                    x += d; y += 3; d = -d; zd = -zd; il = 0; ny += 1
                return True
            return False

        def chk_z():                              # 输出格前的检查：仅同层 Z 折返
            nonlocal x, bz, d, il, nz
            if (d > 0 and x >= X_MAX) or (d < 0 and x <= X_MIN):
                x = (X_MAX + 4) if d > 0 else (X_MIN - 4)
                bz += PITCH * zd; d = -d; il += 1; nz += 1
        for _ in range(pad):
            if not chk():
                x += d
        for _ in range(len(ds)):
            chk(); x += d
            chk_z(); x += d
        return nz, ny

    def do_yfold():
        """升一层：3 步对角（横 1 + 升 1），支撑 glowstone，0 中继器。"""
        x, y, d, bz = st['x'], st['y'], st['dir'], st['base_z']
        s1, s2, end = (x + d, y + 1, bz), (x + d, y + 2, bz + 1), (x + d, y + 3, bz)
        put(s1[0], y, bz, 'minecraft:glowstone')
        put(s1[0], s1[1], s1[2], 'minecraft:redstone_wire')
        put(s2[0], y + 1, bz + 1, 'minecraft:glowstone')
        put(s2[0], s2[1], s2[2], 'minecraft:redstone_wire')
        put(end[0], y + 2, bz, 'minecraft:glowstone')
        put(end[0], end[1], end[2], 'minecraft:redstone_wire')
        yfold_climbs.append([(x, y, bz), s1, s2, end])
        yfold_cells.update({(x, y, bz), s1, s2, end,
                            (s1[0], y, bz), (s2[0], y + 1, bz + 1), (end[0], y + 2, bz)})
        st['x'], st['y'], st['dir'] = end[0], end[1], -d
        st['zdir'] = -st['zdir']
        st['in_layer'] = 0
        layer_ys.add(st['y'])

    def fold_if_needed():
        if near_layer_end():
            do_yfold(); return True
        if at_strip_end():
            if st.get('fold_reps', Z_FOLD_REPS) < Z_FOLD_REPS:
                # 本段列间隔太短、扣不起 2 个走廊中继器 → 改走 0 延迟的 Y 折返（仅 3 格对角粉，
                # 无长粉段问题，也不必补偿时序）
                do_yfold()
            else:
                do_zfold()
            return True
        return False

    def step_dust():
        if not fold_if_needed():
            st['x'] += st['dir']
            put(st['x'], st['y'], st['base_z'], 'minecraft:redstone_wire')

    def step_repeater(d):
        fold_if_needed()
        st['x'] += st['dir']
        put(st['x'], st['y'], st['base_z'], 'minecraft:repeater', make_rep(d, facing()))
        if at_strip_end():          # 输出格越界 → 同层折返（y 折返的斜线必须从粉格起步，不能在此触发）
            do_zfold()
        st['x'] += st['dir']
        put(st['x'], st['y'], st['base_z'], 'minecraft:redstone_wire')

    tap_of_col, bus_dust, branch_reps, note_blocks, col_layer = {}, {}, defaultdict(list), defaultdict(list), {}

    def column_at(c, x, y, bz, d):
        put(x, y, bz + 1, 'minecraft:redstone_wire')      # 跨接粉
        bus_dust[c] = []
        for i, (inst, mat, nv) in enumerate(col_notes[c]):
            z = bz + SLOT0 + i
            put(x, y, z, 'minecraft:redstone_wire')
            bus_dust[c].append((x, y, z))
            put(x + d, y, z, 'minecraft:repeater', make_rep(1, 'west' if d > 0 else 'east'))
            branch_reps[c].append((x + d, y, z))
            put(x + 2 * d, y - 1, z, mat)                  # 垫块决定乐器（bass/harp/bell）
            put(x + 2 * d, y, z, 'minecraft:note_block',
                {'instrument': inst, 'note': str(nv), 'powered': 'false'})
            note_blocks[c].append((x + 2 * d, y, z))
        col_layer[c] = y

    # ---- 脉冲源（第一条带起点） ----
    put(-4, 1, 0, 'minecraft:lever', {'face': 'floor', 'facing': 'west', 'powered': 'false'})
    put(-3, 1, 0, 'minecraft:observer', {'facing': 'west', 'powered': 'false'})
    put(-2, 1, 0, 'minecraft:redstone_wire')
    tap_of_col[0] = (-2, 1, 0)
    col_layer[0] = 1

    # ---- 主干走线 + 逐列 ----
    prev = 0
    for c in sorted(col_notes):
        gap = resolution * (c - prev)
        ds = segment_delays(gap)
        reps = Z_FOLD_REPS
        for _ in range(12):                       # 折返中继器会吃掉延迟，迭代扣减至收敛
            pad = max(0, 4 - 2 * len(ds))
            nz, _ny = sim_path(st['x'], st['y'], st['base_z'], st['dir'],
                               st['zdir'], st['in_layer'], pad, ds, reps, simple)
            need = gap - 2 * reps * nz
            if need < 0 and reps > 1:
                # 列间隔不够扣 Z 折返的 4 刻 → 该折返退回 0 延迟的 Y 折返
                reps = 1
                nz, _ny = sim_path(st['x'], st['y'], st['base_z'], st['dir'],
                                   st['zdir'], st['in_layer'], pad, ds, reps, simple)
                need = gap - 2 * reps * nz
            if need < 0:
                raise SystemExit(f'列 {c}: 折返延迟 {2 * reps * nz} 刻 > 列间隔 {gap} 刻，无法补偿')
            newds = segment_delays(need)
            if newds == ds:
                break
            ds = newds
        st['fold_reps'] = reps
        for _ in range(max(0, 4 - 2 * len(ds))):
            step_dust()
        for d in ds:
            step_repeater(d)
        # 抽头必须落在主干窗口内：折返刚结束时可能停在走廊格里，用 0 延迟粉走回窗口
        # （简单版无折返、主干一路 +X，不需要这个约束）
        if not simple:
            while not (X_MIN <= st['x'] <= X_MAX):
                step_dust()
        tap_of_col[c] = (st['x'], st['y'], st['base_z'])
        column_at(c, st['x'], st['y'], st['base_z'], st['dir'])
        prev = c

    # ---- 末端指示灯 ----
    fold_if_needed()
    st['x'] += st['dir']
    put(st['x'], st['y'], st['base_z'], 'minecraft:repeater', make_rep(1, facing()))
    st['x'] += st['dir']
    put(st['x'], st['y'], st['base_z'], 'minecraft:redstone_lamp', {'lit': 'false'})

    # ---- 支撑层：工作层下方铺 dirt（折返处 glowstone 已显式放置，不被覆盖） ----
    for (x, y, z) in list(blocks.keys()):
        if y in layer_ys:
            blocks.setdefault((x, y - 1, z), ('minecraft:dirt', {}))

    # ---- 红石粉连接状态（视觉；放置后 MC 重算） ----
    for pos, (name, props) in list(blocks.items()):
        if name != 'minecraft:redstone_wire':
            continue
        x, y, z = pos
        p = {'power': '0'}
        for key, (dx, dz) in DIRS.items():
            nb = blocks.get((x + dx, y, z + dz))
            if nb and nb[0] in WIRE_CONNECTABLE:
                p[key] = 'side'
            elif blocks.get((x + dx, y + 1, z + dz), (None,))[0] == 'minecraft:redstone_wire':
                p[key] = 'up'
            else:
                p[key] = 'none'
        blocks[pos] = (name, p)

    return (blocks, tap_of_col, bus_dust, branch_reps, note_blocks, col_layer,
            zfolds, yfold_climbs, yfold_cells, layer_ys, (X_MIN, X_MAX),
            N_STRIP, PITCH, SLOT0, MAX_SLOT)


# --------------------------------------------------------------------------- #
# 纯 Python 红石时序模拟（Dijkstra）
# --------------------------------------------------------------------------- #
def simulate(blocks):
    """红石粉 0 延迟；中继器从输入侧(facing 方向)到输出侧(反面)，权重 = delay*2 游戏刻。"""
    dust, repeaters = set(), {}
    for pos, (name, props) in blocks.items():
        if name == 'minecraft:redstone_wire':
            dust.add(pos)
        elif name == 'minecraft:repeater':
            repeaters[pos] = props

    src = (-2, 1, 0)
    dist = {src: 0}
    pq = [(0, src)]
    while pq:
        d, u = heapq.heappop(pq)
        if d > dist.get(u, math.inf):
            continue
        if u in dust:                                   # 粉 → 相邻粉：同层或跨 1 格高度，均 0 延迟
            x, y, z = u
            for dx, dz in DIRS.values():
                for vy in (y, y + 1, y - 1):
                    v = (x + dx, vy, z + dz)
                    if v in dust and d < dist.get(v, math.inf):
                        dist[v] = d
                        heapq.heappush(pq, (d, v))
        for rp, rprops in repeaters.items():
            dx, dz = DIRS[rprops['facing']]
            inp = (rp[0] + dx, rp[1], rp[2] + dz)       # 输入侧 = facing 方向
            out = (rp[0] - dx, rp[1], rp[2] - dz)       # 输出侧 = facing 反面
            if u == inp and out in blocks:
                nd = d + int(rprops['delay']) * 2
                if nd < dist.get(out, math.inf):
                    dist[out] = nd
                    heapq.heappush(pq, (nd, out))
    return dist


def dust_reachable_from(dust, start):
    """从 start 出发、经连续红石粉（同层或跨 1 格高度）可达的所有红石粉格。"""
    seen, stack = {start}, [start]
    while stack:
        x, y, z = stack.pop()
        for dx, dz in DIRS.values():
            for vy in (y, y + 1, y - 1):
                v = (x + dx, vy, z + dz)
                if v in dust and v not in seen:
                    seen.add(v)
                    stack.append(v)
    return seen


def note_dust_adjacency(blocks):
    """返回 [(音符盒坐标, 相邻红石粉坐标), ...]。

    音符盒被支线中继器正向强充能后会向 6 邻供电；若 6 个面里贴了红石粉，信号会回灌到
    走廊/主干造成锁存或提前触发。故该清单必须恒为空——这是硬约束，生成前后各查一次。
    """
    bad = []
    for pos, (name, _props) in blocks.items():
        if name != 'minecraft:note_block':
            continue
        x, y, z = pos
        for dx, dy, dz in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
            q = (x + dx, y + dy, z + dz)
            if blocks.get(q, (None,))[0] == 'minecraft:redstone_wire':
                bad.append((pos, q))
    return bad


def load_blocks_from_nbt(path):
    """把写出的 .nbt 读回成 {pos: (name, props)}（pos 为相对原点），用于产物级复核。"""
    root = nbtlib.load(str(path))
    palette = list(root['palette'])
    blocks = {}
    for b in root['blocks']:
        entry = palette[int(b['state'])]
        name = str(entry['Name'])
        props = {}
        if 'Properties' in entry:
            props = {str(k): str(v) for k, v in entry['Properties'].items()}
        blocks[tuple(int(v) for v in b['pos'])] = (name, props)
    return blocks, tuple(int(v) for v in root['size'])


def validate(resolution, blocks, dist, tap_of_col, bus_dust, branch_reps, note_blocks,
             col_layer, zfolds, yfold_climbs, yfold_cells, layer_ys, window,
             N_STRIP, PITCH, SLOT0, max_per_col, total_ticks, max_slot):
    """逐项返回实测统计 dict。"""
    out = {'problems': []}
    problems = out['problems']

    # (0) 每个抽头累计游戏刻 ≈ resolution*列号（容差 TIMING_TOL_TICKS 游戏刻）
    out['max_dev'] = 0
    for c, tap in tap_of_col.items():
        got, expect = dist.get(tap), resolution * c
        if got is None:
            problems.append(f'列 {c} 抽头不可达')
            continue
        out['max_dev'] = max(out['max_dev'], abs(got - expect))
        if abs(got - expect) > TIMING_TOL_TICKS:
            problems.append(f'列 {c} 抽头刻数={got} 期望={expect} 偏差={got - expect}')
    out['total_ticks'] = total_ticks

    # (1) Y 向折返：对角步 / glowstone 支撑 / 0 中继器
    out['yfold_pair_bad'], out['yfold_support_bad'], out['yfold_repeaters'] = [], [], []
    for cl in yfold_climbs:
        for a, b in zip(cl, cl[1:]):
            if not (b[1] - a[1] == 1 and abs(b[0] - a[0]) + abs(b[2] - a[2]) == 1):
                out['yfold_pair_bad'].append((a, b))
        for p in cl[1:]:
            below = (p[0], p[1] - 1, p[2])
            if blocks.get(below, (None,))[0] != 'minecraft:glowstone':
                out['yfold_support_bad'].append(below)
        for p in cl:
            if blocks.get(p, (None,))[0] == 'minecraft:repeater':
                out['yfold_repeaters'].append(p)

    # (2) Z 向折返走廊内中继器数 == 设计值（每折返 reps 个 delay=1）
    out['zfold_bad'] = []
    for a, b, reps in zfolds:
        x0, y0, z0 = a
        x1, _, z1 = b
        cells = [(xx, y0, z0) for xx in range(min(x0, x1) + 1, max(x0, x1) + 1)]   # 走廊 X 段(不含起点)
        cells += [(x1, y0, zz) for zz in range(min(z0, z1), max(z0, z1) + 1)]       # 走廊 Z 段
        cnt = sum(1 for p in cells if blocks.get(p, (None,))[0] == 'minecraft:repeater')
        if cnt != reps:
            out['zfold_bad'].append(((a, b), cnt, reps))
    zfold_cells = set()
    for a, b, _reps in zfolds:
        zfold_cells.add(a)
        lo, hi = sorted((a[2], b[2]))
        for z in range(lo, hi + 1):
            zfold_cells.add((b[0], a[1], z))
            zfold_cells.add((a[0], a[1], z))

    # (3) 条带间隔（z % PITCH == PITCH-1）除折返走廊外必须为空
    out['strip_gap_cells'] = [p for p in blocks if p[2] % PITCH == PITCH - 1]
    out['strip_gap_bad'] = [p for p in out['strip_gap_cells']
                            if p not in zfold_cells and blocks[p][0] != 'minecraft:dirt']
    out['strip_gap_dirt'] = [p for p in out['strip_gap_cells']
                             if p not in zfold_cells and blocks[p][0] == 'minecraft:dirt']

    # (4) 跨层相邻违规（不同工作层，折返格除外）；隔层非折返方块
    out['cross_adj'] = []
    for (x, y, z) in blocks:
        if y not in layer_ys or (x, y, z) in yfold_cells:
            continue
        for d in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
            q = (x + d[0], y + d[1], z + d[2])
            if q in blocks and q[1] in layer_ys and q[1] != y and q not in yfold_cells:
                out['cross_adj'].append(((x, y, z), q))
    gaps = [y for y in range(1, max(layer_ys) + 1) if y not in layer_ys and (y - 1) in layer_ys]
    out['gap_blocks'] = [p for p in blocks if p[1] in gaps and p not in yfold_cells]

    # (5) 同列音符盒 Z 连续（条带内）
    out['z_gaps'] = []
    for c, nbs in note_blocks.items():
        zs = sorted(z for (_, _, z) in nbs)
        for a, b in zip(zs, zs[1:]):
            if b - a != 1:
                out['z_gaps'].append((c, a, b))

    # (6) 条带内列间 X 隔离 ≥1 格空气
    out['iso'] = []
    by_strip = defaultdict(list)
    for c in note_blocks:
        by_strip[(tap_of_col[c][1], tap_of_col[c][2])].append(c)
    for key, cols in by_strip.items():
        cols = sorted(cols)
        for i in range(len(cols) - 1):
            c, c2 = cols[i], cols[i + 1]
            nxs = {x for (x, _, _) in note_blocks[c]}
            exs = {x for (x, _, _) in list(bus_dust[c2]) + list(branch_reps[c2])}
            for a in nxs:
                for b2 in exs:
                    if abs(b2 - a) < 2:
                        out['iso'].append((c, c2, a, b2))

    # (7) 跨接粉：每列 (tap_x, y, base+1) 恰 1 格，north/south 均 side
    out['bridge_bad'] = []
    for c in note_blocks:
        x, y, bz = tap_of_col[c]
        b = (x, y, bz + 1)
        nb = blocks.get(b)
        if not (nb and nb[0] == 'minecraft:redstone_wire'
                and nb[1].get('north') == 'side' and nb[1].get('south') == 'side'):
            out['bridge_bad'].append(b)

    # (8) 条带内：支线元素不得与主干元素(该条带 z=base 行)相邻
    trunk_all = {p for p, (n, _) in blocks.items()
                 if n in ('minecraft:redstone_wire', 'minecraft:repeater') and p[2] % PITCH == 0}
    out['bt_adj'] = []
    for c in note_blocks:
        for (x, y, z) in list(bus_dust[c]) + list(branch_reps[c]) + list(note_blocks[c]):
            for dx, dz in DIRS.values():
                if (x + dx, y, z + dz) in trunk_all:
                    out['bt_adj'].append((x, y, z))
                    break

    # (9) 每个音符盒旁必须有输出正指向它的支线中继器
    out['uncovered'] = []
    for c, nbs in note_blocks.items():
        for (x, y, z) in nbs:
            ok = False
            for dx, dz in DIRS.values():
                nb = blocks.get((x + dx, y, z + dz))
                if nb and nb[0] == 'minecraft:repeater':
                    fdx, fdz = DIRS[nb[1]['facing']]
                    if ((x + dx) - fdx, y, z + dz - fdz) == (x, y, z):
                        ok = True
                        break
            if not ok:
                out['uncovered'].append((x, y, z))

    # (10) 音符发声刻数 ≈ resolution*c + 2（同样容差）
    for c, nbs in note_blocks.items():
        for (x, y, z) in nbs:
            got, expect = dist.get((x, y, z)), resolution * c + 2
            if got is None or abs(got - expect) > TIMING_TOL_TICKS:
                problems.append(f'列 {c} 音符盒 {(x, y, z)} 发声刻数={got} 期望={expect}')

    # (11) 总线红石粉连续无断点
    out['broken_bus'] = []
    for c, cells in bus_dust.items():
        for p in cells:
            if blocks.get(p, (None,))[0] != 'minecraft:redstone_wire':
                out['broken_bus'].append((c, p))

    # (12) 支线中继器输入侧是总线粉，且经连续红石粉可达本列抽头
    dust = {p for p, (n, _) in blocks.items() if n == 'minecraft:redstone_wire'}
    out['disconnected'] = []
    for c, reps in branch_reps.items():
        reach = dust_reachable_from(dust, tap_of_col[c])
        for (x, y, z) in reps:
            fdx, fdz = DIRS[blocks[(x, y, z)][1]['facing']]
            inp = (x + fdx, y, z + fdz)
            b = blocks.get(inp)
            if not (b and b[0] == 'minecraft:redstone_wire' and inp in reach):
                out['disconnected'].append((x, y, z))

    # (13) 最长连续红石粉段（被中继器/电源/元件隔开）
    dust_all = {p for p, (n, _) in blocks.items() if n == 'minecraft:redstone_wire'}
    seen, runs = set(), []
    for s in dust_all:
        if s in seen:
            continue
        stack, comp = [s], []
        seen.add(s)
        while stack:
            u = stack.pop(); comp.append(u)
            for dx, dz in DIRS.values():
                for vy in (u[1], u[1] + 1, u[1] - 1):
                    v = (u[0] + dx, vy, u[2] + dz)
                    if v in dust_all and v not in seen:
                        seen.add(v); stack.append(v)
        runs.append(comp)
    runs.sort(key=len, reverse=True)
    out['runs'] = runs[:5]
    out['max_run'] = len(runs[0]) if runs else 0
    out['run_bad'] = [len(c) for c in runs if len(c) > 15]

    # (14) 音符盒 6 面不得有红石粉（音符盒被支线中继器强充能后会向邻格供电，
    #      贴着粉会把信号串回走廊/主干造成锁存或提前触发）
    out['note_dust_adj'] = note_dust_adjacency(blocks)

    out['max_per_col'] = max_per_col
    lamps = [p for p, (n, _) in blocks.items() if n == 'minecraft:redstone_lamp']
    out['lamp_pos'] = lamps[0] if lamps else None
    out['lamp_ok'] = bool(lamps) and dist.get(lamps[0], math.inf) != math.inf
    out['lamp_tick'] = dist.get(lamps[0]) if lamps else None
    return out


# --------------------------------------------------------------------------- #
# NBT 写出
# --------------------------------------------------------------------------- #
def write_nbt(path, blocks):
    xs = [p[0] for p in blocks]
    ys = [p[1] for p in blocks]
    zs = [p[2] for p in blocks]
    minx, miny, minz = min(xs), min(ys), min(zs)
    size = [max(xs) - minx + 1, max(ys) - miny + 1, max(zs) - minz + 1]

    palette, index = [], {}

    def pid(name, props):
        key = (name, tuple(sorted(props.items())))
        if key not in index:
            index[key] = len(palette)
            entry = {'Name': String(name)}
            if props:
                entry['Properties'] = Compound({k: String(v) for k, v in props.items()})
            palette.append(Compound(entry))
        return index[key]

    block_list = []
    for (x, y, z), (name, props) in sorted(blocks.items()):
        block_list.append(Compound({
            'pos': List[Int]([Int(x - minx), Int(y - miny), Int(z - minz)]),
            'state': Int(pid(name, props)),
        }))

    root = Compound({
        'DataVersion': Int(3465),           # 1.20.1
        'size': List[Int]([Int(s) for s in size]),
        'palette': List[Compound](palette),
        'blocks': List[Compound](block_list),
        'entities': List[Compound]([]),
    })
    File(root).save(str(path), gzipped=True)
    return size


# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input')
    ap.add_argument('output')
    ap.add_argument('--seconds', type=float, default=10.0)
    ap.add_argument('--resolution', type=int, default=2, help='每列游戏刻数（默认 2）')
    ap.add_argument('--sustain', action='store_true', help='启用延音（按音符时长重复触发）')
    ap.add_argument('--sustain-interval', type=int, default=2, help='延音重复间隔（列，默认 2）')
    ap.add_argument('--layout', choices=('folded', 'simple'), default='folded',
                    help='folded=蛇形折叠紧凑版（默认）；simple=一条到底的直线版，便于玩家跟看')
    args = ap.parse_args()
    simple = args.layout == 'simple'

    res = args.resolution
    assert res in (2, 4, 6, 8), 'resolution 必须为 2/4/6/8 之一'
    col_seconds = res / MC_TICKS_PER_SEC
    columns = int(round(args.seconds / col_seconds))
    total_ticks_expect = int(round(args.seconds * MC_TICKS_PER_SEC))
    print(f'输入: {args.input}')
    print(f'参数: seconds={args.seconds} resolution={res} → 列宽 {col_seconds:.3f}s, 共 {columns} 列'
          f'；布局 {"一条到底(直线)" if simple else "蛇形折叠"}'
          f'；延音 {"开(间隔 %d 列)" % args.sustain_interval if args.sustain else "关"}')

    # 1) 音符（保留结束时间）+ 量化 + 可选延音扩展
    all_notes = load_notes(args.input)
    notes_q, dropped = [], 0
    for pitch, s, e in all_notes:
        if s >= args.seconds:
            continue
        c0 = int(round(s / col_seconds))
        if not (0 <= c0 < columns):
            dropped += 1
            continue
        D = max(1, int(round((e - s) / col_seconds)))     # 时长列数（至少 1）
        notes_q.append((pitch, c0, D))

    # 2) 音高映射（乐器分档）+ 逐列汇总（延音扩展 + (乐器,note) 去重）
    conv, med, shift = build_pitch_map([p for p, _, _ in all_notes])
    col_key = defaultdict(set)                            # 每列已用 (乐器,垫块,note)
    placed_from = defaultdict(list)                       # (列, (乐器,垫块,note)) -> [(c0,D)...]
    sustain_added = 0
    for pitch, c0, D in notes_q:
        key = conv(pitch)                                 # (instrument, 垫块, note)
        cols = [c0]                                       # 主敲击（必需）
        if args.sustain and D > 1:                        # 长音才延音
            K = max(1, args.sustain_interval)
            c = c0 + K
            while c < c0 + D:                             # 只在 [c0, c0+D) 内
                cols.append(c)
                c += K
        for c in cols:
            if c >= columns:                              # 不越过机尾
                continue
            if key not in col_key[c]:
                col_key[c].add(key)
                if c != c0:
                    sustain_added += 1
            placed_from[(c, key)].append((c0, D))
    # 列内按音高排序，保证同列音符盒 Z 连续且顺序稳定
    col_notes = {c: sorted(ks, key=lambda k: (k[2], k[0])) for c, ks in col_key.items()}

    after = [k for ks in col_notes.values() for k in ks]
    midi_pitches = [p for p, _, _ in all_notes]
    bank_notes = defaultdict(list)
    for inst, _mat, n in after:
        bank_notes[inst].append(n)
    print(f'\n[音高] 全曲中位数={med} 移调={shift:+d}（harp note0↔MIDI54，垫块分档）')
    for inst, _mat, base in INSTRUMENT_BANKS:
        if bank_notes.get(inst):
            print(f'       {inst:<5} 音符 {len(bank_notes[inst]):>3}  note {min(bank_notes[inst])}~{max(bank_notes[inst])}')
    print(f'       全曲 MIDI 音域 {min(midi_pitches)}~{max(midi_pitches)}')
    if dropped:
        print(f'       超出 {columns} 列被丢弃的音符: {dropped}')

    total_notes = sum(len(v) for v in col_notes.values())
    max_per_col = max((len(v) for v in col_notes.values()), default=0)
    print(f'[量化] 抽头列数 {len(col_notes)}，音符盒总数 {total_notes}'
          + (f'（延音额外 +{sustain_added}）' if args.sustain else ''))
    print(f'       Z 长度(最大同列音符数) {max_per_col}，最多音符的列: ' +
          ', '.join(f'{c}:{len(v)}' for c, v in sorted(col_notes.items(), key=lambda kv: -len(kv[1]))[:5]))

    # 3) 生成结构
    (blocks, tap_of_col, bus_dust, branch_reps, note_blocks, col_layer,
     zfolds, yfold_climbs, yfold_cells, layer_ys, window,
     N_STRIP, PITCH, SLOT0, MAX_SLOT) = build(columns, res, col_notes, max_per_col, simple)
    if max_per_col > MAX_SLOT:
        raise SystemExit(f'错误：单列音符数 {max_per_col} 超过条带容量 {MAX_SLOT}（未截断）')

    # 4) 模拟 + 自校验
    dist = simulate(blocks)
    v = validate(res, blocks, dist, tap_of_col, bus_dust, branch_reps, note_blocks,
                 col_layer, zfolds, yfold_climbs, yfold_cells, layer_ys, window,
                 N_STRIP, PITCH, SLOT0, max_per_col, columns * res, MAX_SLOT)

    n_taps = len(col_notes)
    n_bus = sum(len(cells) for cells in bus_dust.values())
    if simple:
        print(f'\n[层] 直线布局：单层 y=1，单条带，Z 宽 {PITCH}')
    else:
        print(f'\n[层] 层数 {len(layer_ys)}，工作层 y={sorted(layer_ys)}；层内 {N_STRIP} 条带，Z 总宽 {N_STRIP * PITCH}')
    print(f'[时序] 总时长 = {v["total_ticks"]} 游戏刻 = {v["total_ticks"] / MC_TICKS_PER_SEC:.2f}s'
          f'  (音符统一 +2 游戏刻偏移)')
    print('[自校验] 抽样列 (抽头刻 / 发声刻):')
    noted = sorted(tap_of_col)
    mid = noted[len(noted) // 2]
    for c in sorted(set(noted[:3] + [mid] + noted[-1:])):
        t = dist.get(tap_of_col[c])
        print(f'       列 {c:>2}: 抽头 {t} 刻 (期望 {res * c}) → 发声 {t + 2} 刻')
    print(f'       抽头总数 {n_taps}，总线粉 {n_bus} 格；Z 向折返 {len(zfolds)} 处，Y 向折返 {len(yfold_climbs)} 处')
    print(f'   (1) 条带间隔(z%{PITCH}=={PITCH - 1}) 方块 = {len(v["strip_gap_cells"])}，'
          f'其中折返走廊 {len(v["strip_gap_cells"]) - len(v["strip_gap_bad"])}，非走廊 = {len(v["strip_gap_bad"])} (须 0)  '
          f'{"✓" if not v["strip_gap_bad"] else "✗"}')
    print(f'   (2) 单列最大音符数 = {max_per_col}（条带容量 {MAX_SLOT}）  '
          f'{"✓" if max_per_col <= MAX_SLOT else "✗"}')
    print(f'   (3) Z 向折返延迟 = 中继器数×2 刻: 违规 = {len(v["zfold_bad"])}/{len(zfolds)} (须 0)  '
          f'{"✓" if not v["zfold_bad"] else "✗"}')
    print(f'   (4) Y 向折返: 非对角 {len(v["yfold_pair_bad"])}，非 glowstone {len(v["yfold_support_bad"])}，'
          f'中继器 {len(v["yfold_repeaters"])} (均须 0)  '
          f'{"✓" if not v["yfold_pair_bad"] and not v["yfold_support_bad"] and not v["yfold_repeaters"] else "✗"}')
    print(f'   (5) 总时长 = {v["total_ticks"]} 刻 (须 {total_ticks_expect})  '
          f'{"✓" if v["total_ticks"] == total_ticks_expect else "✗"}；'
          f'跨层相邻 {len(v["cross_adj"])}，隔层非折返 {len(v["gap_blocks"])} (须 0)')
    print(f'   (6) 条带内 支线-主干相邻 = {len(v["bt_adj"])}，音符盒朝向违规 = {len(v["uncovered"])} (须 0)  '
          f'{"✓" if not v["bt_adj"] and not v["uncovered"] else "✗"}')
    print(f'   (7) 总线断点 {len(v["broken_bus"])}，支线输入断开 {len(v["disconnected"])}，'
          f'跨接粉异常 {len(v["bridge_bad"])} (须 0)  '
          f'{"✓" if not v["broken_bus"] and not v["disconnected"] and not v["bridge_bad"] else "✗"}')
    print(f'       抽头刻数偏差: 最大 {v["max_dev"]} 刻 (容差 {TIMING_TOL_TICKS})  '
          f'{"✓" if not v["problems"] else "✗ " + v["problems"][0]}'
          f'；同列 Z 间隔 {len(v["z_gaps"])}，条带内 X 隔离违规 {len(v["iso"])}')
    print(f'       末端指示灯 tick={v["lamp_tick"]} 可达 = {"✓" if v["lamp_ok"] else "✗"}')
    print(f'   (8) 最长连续粉段 = {v["max_run"]} 格 (须 ≤15)，>15 的段数 = {len(v["run_bad"])}  '
          f'{"✓" if not v["run_bad"] else "✗"}')
    for r in v['runs']:
        print(f'         段长 {len(r):>3}  起点 {r[0]}')
    print(f'   (10) 音符盒 6 面邻接红石粉 = {len(v["note_dust_adj"])} (须 0)  '
          f'{"✓" if not v["note_dust_adj"] else "✗"}')
    for nb, q in v['note_dust_adj'][:5]:
        print(f'         音符盒 {nb} 旁有粉 {q}')
    ddist = defaultdict(int)
    for _n, _p in blocks.values():
        if _n == 'minecraft:repeater':
            ddist[_p['delay']] += 1
    print(f'   (9) 中继器总数 {sum(ddist.values())}，delay 分布 ' +
          ', '.join(f'{k}:{ddist[k]}' for k in sorted(ddist)))
    range_bad = [(c, key, c0, D) for (c, key), srcs in placed_from.items()
                 for (c0, D) in srcs if not (c0 <= c < c0 + D)]
    assert not v['problems'], '时序断言失败:\n' + '\n'.join(v['problems'][:10])
    assert not v['bt_adj'] and not v['cross_adj'] and not v['uncovered'], '隔离/触发检查失败'
    assert v['total_ticks'] == total_ticks_expect, '总时长检查失败'
    assert not v['gap_blocks'], '隔层异常方块: %s' % v['gap_blocks'][:5]
    assert not v['yfold_pair_bad'] and not v['yfold_support_bad'] and not v['yfold_repeaters'], 'Y 折返检查失败'
    assert not v['zfold_bad'], 'Z 折返延迟异常: %s' % v['zfold_bad'][:5]
    assert not v['strip_gap_bad'], '条带间隔非空: %s' % v['strip_gap_bad'][:5]
    assert not range_bad, '延音列越界: %s' % range_bad[:5]
    assert not v['run_bad'], '存在超过 15 格的连续粉段（无法保证信号强度）: %s' % v['run_bad'][:5]
    assert not v['note_dust_adj'], '音符盒 6 面邻接红石粉（会锁存/提前触发）: %s' % v['note_dust_adj'][:5]
    if max_per_col > Z_CAP:
        raise SystemExit(f'错误：单列音符数 {max_per_col} 超过 Z 容量 {Z_CAP}，布局容纳不下（已停止，未截断）')

    # 5) 写出
    size = write_nbt(args.output, blocks)

    # 5b) 产物级复核：直接从写出的 .nbt 读回，确保 100% 满足硬约束（防序列化意外）。
    #     任一项不过 → 删除产物并以非 0 退出，绝不留下不合规文件。
    rb, rsize = load_blocks_from_nbt(args.output)
    minx = min(p[0] for p in blocks)
    miny = min(p[1] for p in blocks)
    minz = min(p[2] for p in blocks)
    rb_abs = {(x + minx, y + miny, z + minz): v for (x, y, z), v in rb.items()}
    missing = [p for p, (n, _) in blocks.items() if rb_abs.get(p, (None,))[0] != n]
    adj_bad = note_dust_adjacency(rb_abs)
    n_notes = sum(1 for n, _ in rb_abs.values() if n == 'minecraft:note_block')
    if missing or adj_bad or list(rsize) != list(size) or len(rb_abs) != len(blocks):
        os.remove(args.output)
        raise SystemExit(
            f'产物复核失败（已删除 {args.output}）：方块缺失/名称不符={len(missing)}，'
            f'音符盒 6 面贴粉={len(adj_bad)}，方块数={len(rb_abs)} vs 预期 {len(blocks)}，'
            f'size={rsize} vs 预期 {tuple(size)}'
            + (f'；示例 {adj_bad[:3]}' if adj_bad else ''))
    print(f'[产物复核] 重新读回 .nbt：方块 {len(rb_abs)} 个（音符盒 {n_notes}），'
          f'音符盒 6 面贴粉 = {len(adj_bad)} ✓')

    counts = defaultdict(int)
    for name, _ in blocks.values():
        counts[name] += 1
    print(f'\n[尺寸] size={size} (X={size[0]}, Y={size[1]}, Z={size[2]})  '
          f'方块总数={len(blocks)}  音符盒={total_notes}  Z长度={max_per_col}')
    print(f'[产物] {args.output}')
    for name in sorted(counts):
        print(f'       {name:<28} {counts[name]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())