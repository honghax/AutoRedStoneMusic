# -*- coding: utf-8 -*-
"""Standalone MIDI to Minecraft structure entry point."""
import runpy
import sys
from pathlib import Path


def add_midi_duration(arguments):
    if "--seconds" in arguments:
        return arguments
    import mido

    if len(arguments) < 3:
        return arguments
    midi = mido.MidiFile(arguments[1])
    resolution = 2
    if "--resolution" in arguments:
        index = arguments.index("--resolution")
        if index + 1 < len(arguments):
            resolution = int(arguments[index + 1])
    column_seconds = resolution / 20.0
    columns = max(1, round(max(midi.length, 0.1) / column_seconds))
    seconds = columns * column_seconds
    return arguments[:2] + ["--seconds", f"{seconds:.6f}"] + arguments[2:]


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    source = Path(__file__).with_name("gen_machine.py")
    if not source.exists():
        raise SystemExit(f"generator source not found: {source}")
    sys.argv = add_midi_duration(sys.argv)
    runpy.run_path(str(source), run_name="__main__")
