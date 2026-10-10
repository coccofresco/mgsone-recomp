#!/usr/bin/env python3
"""Isolated, fingerprinted native MGS TAS sessions. No game assets enter Git."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import struct
import sys
import time
import tomllib

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/ps1Runtime/ps1Runtime"
CONFIG = ROOT / "data/mgs.usa.native.toml"


def sha256(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def game_state(ram):
    """Read-only USA v1.0 witnesses; never use Integral global addresses.

    GM_InitGameSystem at 8002BF54 stores GM_GameStatus at GP+140 and
    GM_PlayerPosition at GP+78C (GP=800AD9FC). CONTROL::mov begins at +0.
    """
    if len(ram) != 2 * 1024 * 1024:
        raise ValueError("game state requires complete main RAM")
    word = lambda address: struct.unpack_from("<I", ram, address & 0x1FFFFF)[0]
    control = word(0x800AE16C)
    # Keep the historical stage field as GM_SaveStageName for old recordings.
    # USA GM_SetArea (8002BD84) writes GM_CurrentStageName at 800ADB34;
    # returning to title does not overwrite the saved gameplay area.
    result = {"stage": ram[0xB7500:0xB7508].split(b"\0")[0].decode("ascii", errors="replace"),
              "current_stage": ram[0xADB34:0xADB3C].split(b"\0")[0].decode("ascii", errors="replace"),
              "player_position": list(struct.unpack_from("<3h", ram, 0xAE188)),
              "game_status": word(0x800ADB3C), "load_request": word(0x800ADB40),
              "player_status": word(0x800AE1C8), "alert_mode": word(0x800AE178),
              "alert_level": word(0x800AE190), "pause_level": word(0x800AE0A0),
              "control": control}
    # GCL_GetVar's USA linkvarbuf operand is 800B7510; linkvar.h supplies
    # the script ABI offsets. No gameplay variable is written by telemetry.
    linked = struct.unpack_from('<64h', ram, 0xB7510)
    result.update(difficulty=linked[1], disc=linked[4], life=linked[11],
                  max_life=linked[12], weapon=linked[14], item=linked[15],
                  stance=linked[16], weapons=list(linked[17:27]),
                  items=list(linked[37:61]))
    physical = control & 0x1FFFFFFF
    if control and physical + 6 <= len(ram):
        result["control_position"] = list(struct.unpack_from("<3h", ram, physical))
    return result


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")
    temporary.replace(path)


def identity(asset_cache=None, shaders=False):
    config = tomllib.loads(CONFIG.read_text())
    files = {"runtime": BINARY, "config": CONFIG,
             "exe": Path(config["disc"]["exe_path"]),
             "cue": Path(config["disc"]["cue_path"])}
    # The CUE is only a descriptor: fingerprint every referenced data track.
    for index, name in enumerate(re.findall(r'^\s*FILE\s+"([^"]+)"', files["cue"].read_text(), re.M)):
        files[f"track_{index}"] = files["cue"].parent / name
    for disc, path in enumerate(config['disc'].get('additional_cues', [])):
        cue = Path(path)
        files[f'additional_cue_{disc}'] = cue
        for index, name in enumerate(re.findall(r'^\s*FILE\s+"([^"]+)"', cue.read_text(), re.M)):
            files[f'additional_track_{disc}_{index}'] = cue.parent / name
    candidates = ([asset_cache.resolve()] if asset_cache else
                  [ROOT / f"data/presentation/{name}/assets.mgsc" for name in ("usa-source", "usa")])
    for cache in candidates:
        if cache.is_file():
            files["asset_cache"] = cache
            break
    if shaders:
        for shader in sorted((BINARY.parent/'shaders').glob('*.spv')):
            files['shader_' + shader.name] = shader
    return {key: {"path": str(path), "sha256": sha256(path)} for key, path in files.items()}


def recording_extent_error(frames, requested_frames, at_checkpoint, route_status=None):
    """A completed adaptive route may end before its frame budget.

    Fixed recordings must still reach their requested length; portable
    checkpoints permit an extended tail, never an arbitrary short recording.
    """
    if route_status is not None:
        return None if route_status == 'complete' else 'adaptive route did not complete'
    if requested_frames and (frames < requested_frames or
                             (frames != requested_frames and not at_checkpoint)):
        return f'recorded {frames} frames; required {requested_frames}'
    return None


def run(args):
    if getattr(args, 'require_hardware', False) and args.headless:
        raise ValueError('--require-hardware cannot be combined with --headless')
    session = args.session.resolve()
    replay = args.command == "replay"
    if replay:
        source = json.loads((session / "manifest.json").read_text())
        if source.get("format") != "mgs-native-tas-1" or source.get("status") != "complete":
            raise ValueError("sessione incompleta: non è una registrazione validata")
        current = identity(getattr(args, 'asset_cache', None), shaders=not args.headless)
        for key, expected in source["identity"].items():
            if ((key == "runtime" or key.startswith('shader_')) and args.allow_runtime_change) or (key == "config" and args.allow_config_change):
                continue
            if current.get(key, {}).get("sha256") != expected["sha256"]:
                raise ValueError(f"identità diversa: {key}; replay rifiutato")
        for name, expected in source["artifacts"].items():
            if sha256(session / name) != expected:
                raise ValueError(f"artefatto modificato: {name}")
        output = args.output.resolve() if args.output else session / ("replay-" + time.strftime("%Y%m%d-%H%M%S"))
        output.mkdir(parents=True, exist_ok=False)
        initial = session / "initial.ps1s"
        movie = session / "movie.mgst"
        cards = session / "initial_cards"
    else:
        initial = args.state.resolve()
        if not initial.is_file():
            raise ValueError(f"savestate iniziale mancante: {initial}")
        if args.cards:
            for name in ("card1.mcd", "card2.mcd"):
                if not (args.cards / name).is_file():
                    raise ValueError(f"memory card iniziale mancante: {args.cards / name}")
        output = session
        output.mkdir(parents=True, exist_ok=False)
        shutil.copy2(initial, output / "initial.ps1s")
        initial = output / "initial.ps1s"
        movie = output / "movie.mgst"
        cards = output / "initial_cards"
        cards.mkdir()
        for path in (args.cards or ROOT / "data/memory_cards").glob("card[12].mcd"):
            shutil.copy2(path, cards / path.name)
        source = {"format": "mgs-native-tas-1", "status": "recording",
                  "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                  "identity": identity(getattr(args, 'asset_cache', None), shaders=not args.headless), "initial_source": str(args.state.resolve()),
                  "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
                  "validation": {"min_game_ticks": args.min_game_ticks},
                  "termination": {"requested_frames": args.frames,
                                  "at_checkpoint": args.end_at_checkpoint},
                  "coverage": "only the recorded route; not a full-game certification"}
        write_json(output / "manifest.json", source)
    states = output / "states"
    states.mkdir()
    shutil.copy2(initial, states / "quick_slot_0.ps1s")
    shutil.copytree(cards, output / "cards")
    # Do not inherit diagnostic/TAS controls from a previous shell session.
    env = {key: value for key, value in os.environ.items() if not key.startswith("PS1_")}
    env.update(PS1_NATIVE_RUNTIME=str(BINARY), PS1_NATIVE_CONFIG=str(CONFIG))
    if getattr(args, 'require_hardware', False):
        env.update(PS1_VULKAN_REQUIRE_HARDWARE='1', PS1_VULKAN_GPU_TIMINGS='1',
                   PS1_VULKAN_REQUIRE_VALIDATION='1')
    env.update(PS1_TAS_REPLAY=str(movie)) if replay else env.update(PS1_TAS_RECORD=str(movie))
    env.update(PS1_DIAGNOSTIC_SAVESTATE_DIR=str(states), PS1_AUTOLOAD_VBLANK="1",
               PS1_MEMORY_CARD_DIR=str(output / "cards"),
               PS1_TAS_MAX_FRAMES=str(args.frames),
               PS1_TAS_CHECKPOINT_INTERVAL=str(args.checkpoint_interval),
               PS1_METRICS=str(output / "metrics.json"), PS1_METRICS_FLUSH_VBLANKS="120",
               PS1_RAM_DUMP_PATH=str(output / "final-ram.bin"))
    if not replay and args.end_at_checkpoint:
        env["PS1_TAS_END_AT_CHECKPOINT"] = "1"
    if replay:
        terminal = movie.read_text().splitlines()[-1].split()
        if len(terminal) != 6 or terminal[0] != "E":
            raise ValueError("testimone terminale TAS mancante")
        final_vblank = str(int(terminal[3]))
        env.update(PS1_NATIVE_DISPLAY_DUMP_DIRECTORY=str(output / "native"),
                   PS1_NATIVE_DISPLAY_DUMP_FROM=final_vblank,
                   PS1_NATIVE_DISPLAY_DUMP_TO=final_vblank,
                   PS1_NATIVE_DISPLAY_DUMP_STEP="1")
    if args.headless:
        env.update(PS1_DIAGNOSTIC_HEADLESS="1", SDL_AUDIODRIVER="dummy")
    elif args.hidden:
        env.update(PS1_VULKAN_HIDDEN_WINDOW="1", PS1_PRESENT_WINDOWED="1", SDL_AUDIODRIVER="dummy")
    if getattr(args, 'headless_surface', False):
        if args.headless:
            raise ValueError('--headless-surface requires active Vulkan, not --headless')
        env.update(PS1_VULKAN_HEADLESS_SURFACE='1', SDL_VIDEODRIVER='dummy',
                   SDL_AUDIODRIVER='dummy', PS1_PRESENT_WINDOWED='1')
    if getattr(args, 'asset_cache', None):
        env.update(PS1_MGS_ASSET_CACHE=str(args.asset_cache.resolve()),
                   PS1_MGS_MESH_REPLAY='1', PS1_MGS_ASSET_VERIFY='1')
    if getattr(args, 'trace_presentation', False):
        env.update(PS1_TRACE_JSONL=str(output/'presentation.jsonl'),
                   PS1_TRACE_EVENTS='mgs.material.missing_input,mgs.material.invalid_normal,mgs.source.material_missing,mgs.texture.miss')
    if args.capture_every:
        if args.headless:
            raise ValueError("--capture-every richiede Vulkan")
        env.update(PS1_VULKAN_REPLAY_READBACK="all",
                   PS1_VULKAN_REPLAY_READBACK_INTERVAL=str(args.capture_every),
                   PS1_VULKAN_REPLAY_READBACK_DUMP_DIRECTORY=str(output / "gpu"))
    if args.native_every:
        env.update(PS1_NATIVE_DISPLAY_DUMP_DIRECTORY=str(output / "native"),
                   PS1_NATIVE_DISPLAY_DUMP_FROM="0",
                   PS1_NATIVE_DISPLAY_DUMP_TO=str(0xFFFFFFFF),
                   PS1_NATIVE_DISPLAY_DUMP_STEP=str(args.native_every))
    if args.period_us:
        env["PS1_VBLANK_PERIOD_US"] = str(args.period_us)
    if args.trace_private:
        env.update(PS1_TRACE_JSONL=str(output / "private-calls.jsonl"),
                   PS1_TRACE_EVENTS="mips.static_private_entry,mips.static_private_exit")
    if args.trace_cd_clock:
        events = env.get("PS1_TRACE_EVENTS", "")
        env.update(PS1_TRACE_JSONL=str(output / "cd-clock.jsonl"),
                   PS1_TRACE_EVENTS=(events + "," if events else "") +
                   "cdrom.credit_discarded,cdrom.read_begin,cdrom.command,cdrom.interrupt,dma.transfer,callback.dispatch")
    if getattr(args, 'trace_loading', False):
        events = env.get("PS1_TRACE_EVENTS", "")
        env.update(PS1_TRACE_JSONL=str(output / "loading.jsonl"),
                   PS1_TRACE_EVENTS=(events + "," if events else "") +
                   "cdrom.read_begin,cdrom.sector_read,mgs.loading_sample")
    if not replay and args.input_script:
        shutil.copy2(args.input_script, output / "seed.input")
        env["PS1_INPUT_SCRIPT"] = str(output / "seed.input")
    if not replay and args.native_route:
        if args.input_script:
            raise ValueError('route adattiva e input assoluto sono alternativi')
        shutil.copy2(args.native_route, output / 'route.mgsr')
        env['PS1_TAS_ROUTE'] = str(output / 'route.mgsr')
    print(f"TAS {'replay' if replay else 'record'}: {output}", flush=True)
    print("F5 checkpoint · F10 pausa · F12 un VBlank · Esc termina", flush=True)
    started = time.monotonic()
    timed_out = False
    launched_runtime_sha = sha256(BINARY)
    expected_runtime_sha = (current if replay else source["identity"])["runtime"]["sha256"]
    if launched_runtime_sha != expected_runtime_sha:
        raise ValueError("runtime ricompilato durante la preparazione; ripetere con una nuova destinazione")
    with (output / "runtime.log").open("w") as log:
        command = [str(ROOT / "scripts/run_native.sh")]
        if getattr(args, 'wine', None):
            from mgs_windows_check import configuration, win
            windows_config = output / 'windows-runtime.toml'
            configuration(CONFIG, windows_config, output / 'cards', states)
            env = {key:win(value) if key.startswith('PS1_') and value.startswith('/') else value
                   for key,value in env.items()}
            command = [str(args.wine), str(BINARY), '--config', win(windows_config)]
        process = subprocess.Popen(command, cwd=ROOT, env=env,
                                   stdout=log, stderr=subprocess.STDOUT)
        previous = {}
        for sig in (signal.SIGINT, signal.SIGTERM):
            previous[sig] = signal.signal(sig, lambda number, frame: process.send_signal(number))
        try:
            try:
                code = process.wait(timeout=args.timeout or None)
            except subprocess.TimeoutExpired:
                timed_out = True
                process.terminate()
                try:
                    code = process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    code = process.wait()
        finally:
            for sig, handler in previous.items():
                signal.signal(sig, handler)
    text = (output / "runtime.log").read_text(errors="replace")
    frames = 0
    footer = False
    if movie.exists():
        for line in movie.read_text().splitlines():
            if line.startswith("F "):
                frames += 1
            elif line.startswith(f"E {frames} "):
                footer = True
    complete = code == 0 and not timed_out and footer and frames > 0
    extent_error = None
    if not replay:
        route_statuses = re.findall(r'^\[Route\] status: (\w+)', text, re.M)
        route_status = (route_statuses[-1] if route_statuses else 'missing') if args.native_route else None
        extent_error = recording_extent_error(frames, args.frames, args.end_at_checkpoint, route_status)
        if extent_error:
            complete = False
    result = {"status": "complete" if complete else "failed", "returncode": code,
              "timeout": timed_out, "movie_frames": frames,
              "elapsed_seconds": round(time.monotonic() - started, 3),
              "runtime_sha256": launched_runtime_sha,
              "identity": current if replay else source["identity"],
              "tas_log": [line for line in text.splitlines() if line.startswith("[TAS]")]}
    result['route_log'] = [line for line in text.splitlines() if line.startswith('[Route]')]
    if extent_error:
        result['termination_error'] = extent_error
    if replay:
        result["identity_overrides"] = {"runtime": args.allow_runtime_change,
                                        "config": args.allow_config_change}
    counts = re.findall(r"^\[TAS\] (\d+) frame\(s\), boundary (\d+), status:", text, re.M)
    if counts:
        result["completed_frames"], result["terminal_boundary"] = map(int, counts[-1])
    if (output / "metrics.json").is_file():
        metrics = json.loads((output / "metrics.json").read_text())
        result["interpreter_counters"] = {key: value for key, value in metrics.get("counters", {}).items()
                                           if key.startswith("mips.") and value}
    result["game_errors"] = [line for line in text.splitlines()
                              if ("[Game]" in line and "exception" in line.lower())
                              or "[DISPATCH] FATAL:" in line]
    result["playability"] = "unverified beyond this recorded route"
    # USA GV_Time is independently identified by gvd.c's GP-relative operand
    # (also used by mgs_screen_effects.cpp). v6-v12 retain this disk RAM prefix.
    seed = initial.read_bytes()
    ram_path = output / "final-ram.bin"
    minimum = max(args.min_game_ticks, source.get("validation", {}).get("min_game_ticks", 0))
    if len(seed) >= 476 + 2 * 1024 * 1024 and seed[:4] == b"PS1S" and struct.unpack_from("<I", seed, 4)[0] in (6, 7, 8, 9, 10, 11, 12) and ram_path.is_file():
        ram = ram_path.read_bytes()
        if len(ram) == 2 * 1024 * 1024:
            result["game_state"] = game_state(ram)
            before = struct.unpack_from("<I", seed, 476 + 0xADA48)[0]
            after = struct.unpack_from("<I", ram, 0xADA48)[0]
            delta = (after - before) & 0xFFFFFFFF
            result["game_ticks"] = {"initial": before, "final": after, "delta": delta}
            if delta < minimum:
                complete = False
                result.update(status="failed", progress_error=f"GV_Time advanced {delta}; required {minimum}")
    if minimum and "game_ticks" not in result:
        complete = False
        result.update(status="failed", progress_error="missing GV_Time witness")
    result["checkpoints"] = [{"file": str(path.relative_to(output)), "sha256": sha256(path)}
                             for path in sorted(states.glob("checkpoint_*.ps1s"))]
    if (states / "desync.ps1s").exists():
        result["desync_checkpoint"] = str(states / "desync.ps1s")
    write_json(output / "result.json", result)
    if not replay:
        source["status"] = result["status"]
        source["frames"] = frames
        source["artifacts"] = {str(path.relative_to(output)): sha256(path)
                               for path in [initial, movie, output / "seed.input", output / 'route.mgsr', *cards.glob("*.mcd")] if path.exists()}
        write_json(output / "manifest.json", source)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if complete else 1


def export_donor(args):
    session = args.session.resolve()
    manifest = json.loads((session / "manifest.json").read_text())
    if manifest.get("status") != "complete" or sha256(session / "movie.mgst") != manifest["artifacts"]["movie.mgst"]:
        raise ValueError("registrazione incompleta o modificata")
    names = ["select", "l3", "r3", "start", "up", "right", "down", "left",
             "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"]
    intervals = []
    active = {}
    count = 0
    for line in (session / "movie.mgst").read_text().splitlines():
        if not line.startswith("F "):
            continue
        values = list(map(int, line.split()[1:]))
        frame = args.first_frame + count
        for port in range(2):
            buttons, lx, ly, rx, ry, kind = values[4 + port * 6:10 + port * 6]
            if kind != 65 or (lx, ly, rx, ry) != (128, 128, 128, 128):
                raise ValueError("export intervalli donor limitato ai pad digitali con assi centrati")
            for bit, name in enumerate(names):
                key = (port, bit)
                pressed = not buttons & (1 << bit)
                if pressed and key not in active:
                    active[key] = frame
                elif not pressed and key in active:
                    intervals.append((active.pop(key), frame - 1, name, port))
        count += 1
    for (port, bit), first in active.items():
        intervals.append((first, args.first_frame + count - 1, names[bit], port))
    # Exporting input does not align native and emulator execution timelines.
    with args.destination.open("x") as output:
        output.write(f"# MGS TAS input only; {count} frames; first donor frame {args.first_frame}\n")
        output.write(f"# Source movie SHA256 {manifest['artifacts']['movie.mgst']}\n")
        output.write("# Align an independent SwanStation checkpoint first; native states are incompatible.\n")
        for first, last, name, port in sorted(intervals):
            output.write(f"{first} {last} {name} {port}\n")
    print(f"Esportati {count} frame, {len(intervals)} intervalli: {args.destination}")
    return 0


def bootstrap(args):
    """Make an early portable checkpoint with isolated blank or copied cards."""
    output = args.destination.resolve()
    source_cards = {}
    if args.cards:
        for name in ('card1.mcd', 'card2.mcd'):
            path = args.cards / name
            if path.stat().st_size != 131072:
                raise ValueError(f'memory card non valida: {path}')
            source_cards[name] = {'path': str(path.resolve()), 'sha256': sha256(path)}
    output.mkdir(parents=True, exist_ok=False)
    if source_cards:
        (output / 'cards').mkdir()
        for name, source in source_cards.items():
            shutil.copy2(source['path'], output / 'cards' / name)
    env = {key: value for key, value in os.environ.items() if not key.startswith("PS1_")}
    env.update(PS1_DIAGNOSTIC_HEADLESS="1", SDL_AUDIODRIVER="dummy",
               PS1_NATIVE_RUNTIME=str(BINARY), PS1_NATIVE_CONFIG=str(CONFIG),
               PS1_VBLANK_PERIOD_US="1000", PS1_AUTOSAVE_VBLANK="1",
               PS1_EXIT_AFTER_AUTOSAVE="1",
               PS1_DIAGNOSTIC_SAVESTATE_DIR=str(output / "states"),
               PS1_MEMORY_CARD_DIR=str(output / "cards"),
               PS1_METRICS=str(output / "metrics.json"))
    with (output / "runtime.log").open("w") as log:
        process = subprocess.run([str(ROOT / "scripts/run_native.sh")], cwd=ROOT, env=env,
                                 stdout=log, stderr=subprocess.STDOUT, timeout=60)
    state = output / "states/quick_slot_0.ps1s"
    metrics = json.loads((output / "metrics.json").read_text())
    if process.returncode or not state.is_file() or metrics.get("counters", {}).get("host.savestate.saved") != 1:
        raise ValueError(f"checkpoint di avvio non riuscito; dettagli in {output}")
    shutil.copy2(state, output / "initial.ps1s")
    write_json(output / "manifest.json", {"format": "mgs-tas-boot-seed-1",
               "identity": identity(), "initial_sha256": sha256(state),
               "source_cards": source_cards,
               "initial_vblank": metrics.get("counters", {}).get("timing.vblank"),
               "note": "earliest resumable native checkpoint, not a frame-zero power-on state"})
    print(f"Checkpoint iniziale: {output / 'initial.ps1s'}")
    return 0


def recover_prefix(args):
    """Seal a crashed recording at its last known pre-input RAM witness."""
    source = args.session.resolve()
    manifest = json.loads((source / "manifest.json").read_text())
    if manifest.get("format") != "mgs-native-tas-1":
        raise ValueError("formato sessione non supportato")
    lines = (source / "movie.mgst").read_text().splitlines()
    if not lines or lines[0] != "MGSTAS 1":
        raise ValueError("intestazione TAS mancante o non supportata")
    for name, expected in manifest.get("artifacts", {}).items():
        if name != "movie.mgst" and sha256(source / name) != expected:
            raise ValueError(f"artefatto iniziale modificato: {name}")
    frames = []
    for line in lines[1:]:
        tokens = line.split()
        if len(tokens) != 17 or tokens[0] != "F" or any(not token.isascii() or not token.isdecimal() for token in tokens[1:]):
            break
        values = list(map(int, tokens[1:]))
        if any(value > 0xFFFFFFFFFFFFFFFF for value in values[:3]) or values[3] > 0xFFFFFFFF:
            raise ValueError("campo TAS fuori intervallo")
        if values[0] < 1 or values[1] < 1 or (frames and
                (values[0] <= frames[-1][0] or values[1] != frames[-1][1] + 1)):
            raise ValueError("prefisso TAS non contiguo")
        for offset in (4, 10):
            if values[offset] > 65535 or any(n > 255 for n in values[offset + 1:offset + 5]) or values[offset + 5] not in (0, 65, 115, 121):
                raise ValueError("campione controller non valido")
        frames.append(values)
    if len(frames) < 2:
        raise ValueError("servono almeno due frame completi per recuperare un prefisso verificabile")
    length = args.frames if args.frames is not None else len(frames) - 1
    if not 1 <= length < len(frames):
        raise ValueError('il prefisso richiede almeno un input e il testimone del frame seguente')
    terminal = frames[length]
    frames = frames[:length]
    output = args.destination.resolve()
    output.mkdir(parents=True, exist_ok=False)
    shutil.copy2(source / "initial.ps1s", output / "initial.ps1s")
    shutil.copytree(source / "initial_cards", output / "initial_cards")
    with (output / "movie.mgst").open("x") as stream:
        stream.write("MGSTAS 1\n")
        for frame in frames:
            stream.write("F " + " ".join(map(str, frame)) + "\n")
        stream.write(f"E {len(frames)} {terminal[0]} {terminal[1] - 1} {terminal[2]} {terminal[3]}\n")
    manifest.update(status="complete", frames=len(frames), recovered_from=str(source),
                    validation={"min_game_ticks": 0},
                    coverage="recovered prefix only; last input and failed/unrecorded tail excluded")
    manifest["artifacts"] = {str(p.relative_to(output)): sha256(p)
                             for p in [output / "initial.ps1s", output / "movie.mgst", *(output / "initial_cards").glob("*.mcd")]}
    write_json(output / "manifest.json", manifest)
    print(f"Recuperati {len(frames)} frame verificabili. Ultimo input e coda esclusi: {output}")
    return 0


def main():
    global BINARY, CONFIG
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    seed = commands.add_parser("bootstrap", help="crea un checkpoint iniziale con memory card isolate")
    seed.add_argument("destination", type=Path)
    seed.add_argument("--cards", type=Path, help="copia queste card nella nuova sessione prima del bootstrap")
    seed.add_argument("--runtime", type=Path, default=BINARY)
    seed.add_argument("--config", type=Path, default=CONFIG)
    recover = commands.add_parser("recover-prefix", help="recupera un prefisso verificabile dopo un arresto")
    recover.add_argument("session", type=Path)
    recover.add_argument("destination", type=Path)
    recover.add_argument("--frames", type=int, help="sigilla solo questi input usando il successivo testimone RAM")
    export = commands.add_parser("export-donor", help="esporta input digitali per l'oracolo SwanStation")
    export.add_argument("session", type=Path)
    export.add_argument("destination", type=Path)
    export.add_argument("--first-frame", type=int, default=1)
    for command in ("record", "replay"):
        sub = commands.add_parser(command)
        sub.add_argument("session", type=Path)
        sub.add_argument("--runtime", type=Path, default=BINARY)
        sub.add_argument("--wine", type=Path, help="execute the real Windows PE runtime via this Wine binary (isolated WINEPREFIX recommended)")
        sub.add_argument("--config", type=Path, default=CONFIG)
        sub.add_argument("--frames", type=int, default=0)
        sub.add_argument("--min-game-ticks", type=int, default=0, help="richiede almeno N avanzamenti di GV_Time")
        sub.add_argument("--headless", action="store_true")
        sub.add_argument("--hidden", action="store_true", help="Vulkan 4K in una finestra nascosta")
        sub.add_argument("--headless-surface", action="store_true", help="execute Vulkan via VK_EXT_headless_surface (no desktop required)")
        sub.add_argument("--require-hardware", action="store_true", help="require an integrated/discrete Vulkan GPU and collect GPU timestamps; reject CPU/virtual devices")
        sub.add_argument("--asset-cache", type=Path, help="explicit original-asset Vulkan cache with upload readback verification")
        sub.add_argument("--trace-presentation", action="store_true", help="record original material/pose rejection evidence")
        sub.add_argument("--capture-every", type=int, default=0, help="cattura Vulkan ogni N VBlank")
        sub.add_argument("--native-every", type=int, default=0, help="cattura diagnostica del framebuffer nativo ogni N VBlank")
        sub.add_argument("--period-us", type=int, default=0)
        sub.add_argument("--trace-cd-clock", action="store_true", help="trace CD credit loss and DMA/callback ordering")
        sub.add_argument("--trace-loading", action="store_true", help="measure original loader lifetime and per-file CD delivery")
        sub.add_argument("--trace-private", action="store_true", help="trace unresolved native entries and their callers")
        sub.add_argument("--checkpoint-interval", type=int, default=3600)
        sub.add_argument("--timeout", type=int, default=0)
        if command == "record":
            sub.add_argument("--end-at-checkpoint", action="store_true",
                             help="extend the recorded tail until a portable terminal checkpoint is available")
            sub.add_argument("--state", type=Path, required=True)
            sub.add_argument("--input-script", type=Path)
            sub.add_argument("--native-route", type=Path, help="programma adattivo compilato da mgs_tas_route.py")
            sub.add_argument("--cards", type=Path, help="memory card della sessione da cui si prosegue")
        else:
            sub.add_argument("--output", type=Path)
            sub.add_argument("--allow-runtime-change", action="store_true",
                             help="test a runtime fix against the same recorded RAM witnesses")
            sub.add_argument("--allow-config-change", action="store_true",
                             help="test a new overlay catalog against unchanged recorded RAM witnesses")
    args = parser.parse_args()
    if args.command in ("record", "replay", "bootstrap"):
        BINARY, CONFIG = args.runtime.resolve(), args.config.resolve()
    if args.command == "export-donor" and args.first_frame < 1:
        parser.error("il primo frame donor è almeno 1")
    if args.command in ("record", "replay") and (args.frames < 0 or args.min_game_ticks < 0 or args.period_us < 0 or args.checkpoint_interval < 0 or args.timeout < 0 or args.capture_every < 0 or args.native_every < 0):
        parser.error("i limiti non possono essere negativi")
    try:
        if args.command == "export-donor":
            return export_donor(args)
        if args.command == "bootstrap":
            return bootstrap(args)
        if args.command == "recover-prefix":
            return recover_prefix(args)
        return run(args)
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        print(f"TAS: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
