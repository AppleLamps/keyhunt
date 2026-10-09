#!/usr/bin/env python3
"""Small, dependency-free curses front end for keyhunt.

Run with ``make tui`` or ``python3 keyhunt_tui.py`` from Linux/WSL.
"""

from __future__ import annotations

import argparse
import curses
import errno
import json
import os
from pathlib import Path
import pty
import re
import shlex
import signal
import subprocess
import sys
import time
import zipfile
from dataclasses import dataclass
from typing import Dict, List, Sequence
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parent
DEFAULT_CONFIG_PATH = ROOT / ".keyhunt-tui.json"
PUZZLE_WORKBOOK = ROOT / "puzzle-all.xlsx"
PUZZLE_TARGET_DIR = ROOT / ".keyhunt-puzzles"
MODES = ("address", "rmd160", "xpoint", "bsgs", "vanity", "minikeys")
MODE_TARGET_DEFAULTS = {
    "address": "tests/66.txt",
    "rmd160": "tests/66.rmd",
    "xpoint": "tests/substracted40.txt",
    "bsgs": "tests/63.pub",
    "vanity": "tests/vanitytargets.txt",
    "minikeys": "tests/minikeys.txt",
}
MODE_BITS_DEFAULTS = {"address": "66", "rmd160": "66", "xpoint": "40",
                      "bsgs": "63", "vanity": "256"}
ANSI_RE = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\))")
PRIVATE_KEY_RE = re.compile(
    r"\b(?:private\s+key|privkey)\s*:?\s*(?:0x)?([0-9a-f]{1,64})(?![0-9a-f])",
    re.IGNORECASE,
)


def default_bsgs_cache_dir() -> str:
    override = os.environ.get("KEYHUNT_BSGS_CACHE_DIR", "").strip()
    if override:
        return override
    for drive in (Path("/mnt/d"), Path("D:/")):
        if drive.is_dir():
            return str(drive / "keyhunt-cache")
    return str(ROOT / ".keyhunt-cache")


@dataclass(frozen=True)
class HardwareProfile:
    model: str
    logical_cpus: int
    physical_cores: int
    memory_gib: float
    simd: str

    @property
    def summary(self) -> str:
        topology = f"{self.physical_cores}C/{self.logical_cpus}T" if self.physical_cores else f"{self.logical_cpus} threads"
        return f"{self.model} · {topology} · {self.memory_gib:.1f} GiB · {self.simd}"


def detect_hardware() -> HardwareProfile:
    logical = os.cpu_count() or 1
    model = "CPU"
    flags = set()
    physical_ids = set()
    current_physical = current_core = None
    try:
        for raw_line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines() + [""]:
            if not raw_line:
                if current_physical is not None and current_core is not None:
                    physical_ids.add((current_physical, current_core))
                current_physical = current_core = None
                continue
            key, _, value = raw_line.partition(":")
            key, value = key.strip(), value.strip()
            if key == "model name":
                model = value
            elif key == "flags":
                flags.update(value.split())
            elif key == "physical id":
                current_physical = value
            elif key == "core id":
                current_core = value
    except OSError:
        pass
    memory_gib = 0.0
    try:
        match = re.search(r"^MemTotal:\s+(\d+)\s+kB", Path("/proc/meminfo").read_text(encoding="utf-8"), re.MULTILINE)
        if match:
            memory_gib = int(match.group(1)) / 1024 / 1024
    except OSError:
        pass
    if "avx512ifma" in flags and "avx512f" in flags:
        simd = "AVX-512 IFMA"
    elif "avx512f" in flags:
        simd = "AVX-512"
    elif "avx2" in flags:
        simd = "AVX2"
    else:
        simd = "scalar/SSE"
    return HardwareProfile(model, logical, len(physical_ids), memory_gib, simd)


def _xlsx_column(reference: str) -> int:
    letters = "".join(char for char in reference if char.isalpha())
    value = 0
    for char in letters:
        value = value * 26 + ord(char.upper()) - ord("A") + 1
    return value - 1


def load_puzzle_catalog(path: Path = PUZZLE_WORKBOOK) -> Dict[int, Dict[str, str]]:
    """Read the 160 puzzle records from the bundled OOXML workbook."""
    namespace = {"m": "http://schemas.openxmlformats.org/spreadsheetml/2006/main"}
    with zipfile.ZipFile(path) as workbook:
        shared_root = ET.fromstring(workbook.read("xl/sharedStrings.xml"))
        shared = ["".join(item.itertext()) for item in shared_root.findall("m:si", namespace)]
        sheet = ET.fromstring(workbook.read("xl/worksheets/sheet1.xml"))

    decoded_rows: List[Dict[int, str]] = []
    for row in sheet.findall(".//m:sheetData/m:row", namespace):
        decoded: Dict[int, str] = {}
        for cell in row.findall("m:c", namespace):
            reference = cell.attrib.get("r", "")
            value_node = cell.find("m:v", namespace)
            if not reference or value_node is None or value_node.text is None:
                continue
            value = value_node.text
            if cell.attrib.get("t") == "s":
                value = shared[int(value)]
            decoded[_xlsx_column(reference)] = value.strip()
        decoded_rows.append(decoded)
    if not decoded_rows:
        raise ValueError("puzzle workbook has no rows")

    headers = {column: value.strip().lower() for column, value in decoded_rows[0].items()}
    catalog: Dict[int, Dict[str, str]] = {}
    for decoded in decoded_rows[1:]:
        record = {headers[column]: value for column, value in decoded.items() if column in headers}
        try:
            number = int(float(record.get("bits", "")))
        except ValueError:
            continue
        catalog[number] = record
    if set(catalog) != set(range(1, 161)):
        raise ValueError(f"expected puzzles 1-160, found {len(catalog)} records")
    return catalog


def _floor_power_of_two(value: int) -> int:
    return 1 << max(0, value.bit_length() - 1)


def puzzle_signature(number: int, hardware: HardwareProfile) -> str:
    memory_tier = _floor_power_of_two(max(1, int(hardware.memory_gib * 64)))
    signature = f"puzzle-{number}|threads-{hardware.logical_cpus}|memory-k-{memory_tier}"
    return signature + ("|scan-v1" if number == 71 else "")


def apply_puzzle_preset(config: Dict[str, object], number: int,
                        catalog: Dict[int, Dict[str, str]], hardware: HardwareProfile,
                        target_dir: Path = PUZZLE_TARGET_DIR) -> str:
    if number not in catalog:
        raise ValueError("puzzle must be from 1 to 160")
    record = catalog[number]
    bits = int(float(record["bits"]))
    public_key = record.get("public_key", "").strip()
    hash160 = record.get("hash160_compressed", "").strip()
    address = record.get("address", "").strip()
    solved = bool(record.get("private_key", "").strip())
    use_bsgs = bits >= 22 and bool(re.fullmatch(r"(?:02|03)[0-9a-fA-F]{64}|04[0-9a-fA-F]{128}", public_key))

    target_dir.mkdir(parents=True, exist_ok=True)
    if use_bsgs:
        suffix, target = "pub", public_key
    elif re.fullmatch(r"[0-9a-fA-F]{40}", hash160):
        suffix, target = "rmd", hash160
    elif address:
        suffix, target = "txt", address
    else:
        raise ValueError(f"puzzle {number} has no usable target")
    target_path = target_dir / f"puzzle-{number:03d}.{suffix}"
    target_path.write_text(target + "\n", encoding="ascii")
    try:
        target_name = str(target_path.relative_to(ROOT))
    except ValueError:
        target_name = str(target_path)

    config.update({
        "puzzle": str(number),
        "target_file": target_name,
        "range_kind": "bits",
        "range_value": str(bits),
        "threads": str(hardware.logical_cpus),
        "stats_seconds": "10",
        "search_type": "compress",
        "crypto": "btc",
        "stride": "",
        "bloom_multiplier": "",
        "vanity_prefixes": "",
        "minikey_base": "",
        "alphabet": "",
        "endomorphism": False,
        "quiet": True,
        "skip_checksum": False,
        "extra_args": "",
        "checkpoint_file": "",
    })
    if use_bsgs:
        # N must be below the 2^(bits-1) puzzle range. The project's measured
        # sweet spot is N=2^44; smaller puzzles use the largest legal even N.
        n_exponent = min(44, bits - 2)
        n_exponent -= n_exponent % 2
        max_factor = 1 << ((n_exponent - 20) // 2)
        normal_factor = _floor_power_of_two(max(1, int(hardware.memory_gib * 64)))
        compact_factor = min(max_factor, normal_factor * 2)
        use_compact = compact_factor > normal_factor
        factor = compact_factor if use_compact else min(max_factor, normal_factor)
        config.update({
            "mode": "bsgs",
            "block_size": f"0x{1 << n_exponent:x}",
            "bsgs_order": "sequential" if solved else "random",
            "bsgs_factor": str(factor),
            "compact_filter": use_compact,
            "save_tables": True,
        })
        strategy = f"BSGS, N=2^{n_exponent}, k={factor}{' with compact filter' if use_compact else ''}"
    else:
        block_exponent = min(32, max(10, bits - 1))
        config.update({
            "mode": "rmd160" if suffix == "rmd" else "address",
            "block_size": f"0x{1 << block_exponent:x}",
            "search_order": "sequential" if solved else "random",
            "bsgs_factor": "",
            "compact_filter": False,
            "save_tables": False,
            "checkpoint_file": str(Path(target_name).with_suffix(".scan")) if not solved else "",
        })
        strategy = f"compressed {config['mode']}, {'sequential' if solved else 'random'}"
    state = "solved" if solved else "unsolved"
    config["puzzle_summary"] = f"Puzzle #{number} ({bits}-bit, {state}): {strategy}; {hardware.logical_cpus} threads"
    config["preset_signature"] = puzzle_signature(number, hardware)
    return str(config["puzzle_summary"])


def default_config() -> Dict[str, object]:
    return {
        "puzzle": "66",
        "puzzle_summary": "Puzzle #66 preset",
        "preset_signature": "",
        "mode": "address",
        "target_file": MODE_TARGET_DEFAULTS["address"],
        "range_kind": "bits",
        "range_value": "66",
        "search_type": "compress",
        "crypto": "btc",
        "threads": str(os.cpu_count() or 1),
        "block_size": "",
        "stats_seconds": "10",
        "search_order": "random",
        "bsgs_order": "random",
        "bsgs_factor": "",
        "stride": "",
        "bloom_multiplier": "",
        "vanity_prefixes": "",
        "minikey_base": "",
        "alphabet": "",
        "endomorphism": False,
        "quiet": True,
        "save_tables": False,
        "cache_dir": default_bsgs_cache_dir(),
        "checkpoint_file": "",
        "compact_filter": False,
        "skip_checksum": False,
        "extra_args": "",
    }


@dataclass(frozen=True)
class Field:
    key: str
    label: str
    kind: str = "text"
    choices: Sequence[str] = ()
    help: str = ""


def visible_fields(config: Dict[str, object]) -> List[Field]:
    mode = str(config["mode"])
    fields = [
        Field("puzzle", "Puzzle preset", "puzzle",
              help=str(config.get("puzzle_summary", "Type 1-160, or 'manual'. Left/Right steps through puzzles."))),
        Field("mode", "Mode", "choice", MODES, "Choose the Keyhunt search mode."),
        Field("target_file", "Target file", help="One target per line, relative to the repository or absolute."),
    ]
    if mode != "minikeys":
        fields.append(Field("range_kind", "Range input", "choice", ("bits", "range", "default"),
                            "Use -b bits, -r start:end, or Keyhunt's default range."))
        if config["range_kind"] != "default":
            label = "Bit range" if config["range_kind"] == "bits" else "Hex range"
            help_text = "1 to 256." if config["range_kind"] == "bits" else "Hex start:end, for example 1:FFFFFFFF."
            fields.append(Field("range_value", label, help=help_text))

    if mode in ("address", "rmd160", "vanity"):
        fields.append(Field("search_type", "Key form", "choice", ("compress", "uncompress", "both"),
                            "Compressed is fastest for Bitcoin puzzle addresses."))
    if mode == "address":
        fields.append(Field("crypto", "Cryptocurrency", "choice", ("btc", "eth")))
    if mode == "bsgs":
        fields.extend([
            Field("bsgs_order", "BSGS order", "choice",
                  ("random", "sequential", "backward", "both", "dance")),
            Field("bsgs_factor", "Table factor (-k)", help="Power-of-two speed/RAM multiplier; blank uses the default."),
            Field("compact_filter", "Compact filter (-F)", "bool", help="About half the RAM; use roughly twice the -k."),
            Field("skip_checksum", "Skip checksum (-6)", "bool", help="Only affects loading saved BSGS tables."),
        ])
    else:
        fields.append(Field("search_order", "Search order", "choice", ("random", "sequential")))

    fields.extend([
        Field("block_size", "Block/table N (-n)", help="Blank uses Keyhunt's mode-specific default."),
        Field("threads", "Threads", help="Number of CPU worker threads."),
        Field("stats_seconds", "Stats interval", help="Seconds between speed reports; 0 disables reports."),
    ])
    if mode in ("address", "rmd160", "xpoint"):
        fields.append(Field("stride", "Stride (-I)", help="Blank uses consecutive keys."))
    if mode in ("address", "rmd160", "xpoint", "vanity"):
        fields.extend([
            Field("bloom_multiplier", "Bloom multiplier (-z)", help="Blank uses 1."),
            Field("endomorphism", "Endomorphism (-e)", "bool"),
        ])
    if mode == "vanity":
        fields.append(Field("vanity_prefixes", "Vanity prefixes", help="Comma-separated prefixes; target file may be blank."))
    if mode == "minikeys":
        fields.extend([
            Field("minikey_base", "Base minikey (-C)", help="Optional 22-character starting minikey."),
            Field("alphabet", "Base58 alphabet (-8)", help="Blank uses Bitcoin's alphabet."),
        ])
    if mode != "vanity":
        fields.append(Field("save_tables", "Save/load tables (-S)", "bool"))
    if mode == "bsgs":
        fields.append(Field("cache_dir", "BSGS cache directory (-o)",
                            help="Large .blm/.tbl files; defaults to D:\\keyhunt-cache under WSL."))
    if mode in ("address", "rmd160"):
        fields.append(Field("checkpoint_file", "Scan checkpoint (-P)",
                            help="Save/resume completed blocks; blank disables. Same target/range/settings required."))
    fields.extend([
        Field("quiet", "Quiet workers (-q)", "bool", help="Speed reports and hits are still shown."),
        Field("extra_args", "Advanced arguments", help="Optional extra CLI arguments, parsed without a shell."),
    ])
    return fields


def build_command(config: Dict[str, object], binary: str = "./keyhunt") -> List[str]:
    """Translate TUI state to a shell-free argv list."""
    mode = str(config["mode"])
    command = [binary, "-m", mode]

    target_file = str(config.get("target_file", "")).strip()
    if target_file:
        command += ["-f", target_file]

    range_kind = str(config.get("range_kind", "default"))
    range_value = str(config.get("range_value", "")).strip()
    if mode != "minikeys" and range_kind == "bits" and range_value:
        command += ["-b", range_value]
    elif mode != "minikeys" and range_kind == "range" and range_value:
        command += ["-r", range_value]

    for key, option in (("block_size", "-n"), ("threads", "-t"), ("stats_seconds", "-s")):
        value = str(config.get(key, "")).strip()
        if value:
            command += [option, value]

    if mode in ("address", "rmd160", "vanity"):
        command += ["-l", str(config["search_type"])]
    if mode == "address":
        command += ["-c", str(config["crypto"])]

    if mode == "bsgs":
        command += ["-B", str(config["bsgs_order"])]
        factor = str(config.get("bsgs_factor", "")).strip()
        if factor:
            command += ["-k", factor]
        if config.get("compact_filter"):
            command.append("-F")
        if config.get("skip_checksum"):
            command.append("-6")
    elif not (mode == "minikeys" and str(config.get("minikey_base", "")).strip()):
        command.append("-L" if config.get("search_order") == "sequential" else "-R")

    if mode in ("address", "rmd160", "xpoint"):
        stride = str(config.get("stride", "")).strip()
        if stride:
            command += ["-I", stride]
    if mode in ("address", "rmd160"):
        checkpoint = str(config.get("checkpoint_file", "")).strip()
        if checkpoint:
            command += ["-P", checkpoint]
    if mode in ("address", "rmd160", "xpoint", "vanity"):
        bloom = str(config.get("bloom_multiplier", "")).strip()
        if bloom:
            command += ["-z", bloom]
        if config.get("endomorphism"):
            command.append("-e")
    if mode == "vanity":
        prefixes = str(config.get("vanity_prefixes", ""))
        for prefix in (part.strip() for part in prefixes.split(",")):
            if prefix:
                command += ["-v", prefix]
    if mode == "minikeys":
        base = str(config.get("minikey_base", "")).strip()
        alphabet = str(config.get("alphabet", "")).strip()
        if base:
            command += ["-C", base]
        if alphabet:
            command += ["-8", alphabet]
    if mode != "vanity" and config.get("save_tables"):
        command.append("-S")
    if mode == "bsgs":
        cache_dir = str(config.get("cache_dir", "")).strip()
        if cache_dir:
            command += ["-o", cache_dir]
    if config.get("quiet"):
        command.append("-q")

    extra = str(config.get("extra_args", "")).strip()
    if extra:
        command.extend(shlex.split(extra))
    return command


def validate_config(config: Dict[str, object], binary: str = "./keyhunt") -> List[str]:
    errors: List[str] = []
    mode = str(config.get("mode", ""))
    if mode not in MODES:
        errors.append("Unknown mode.")

    target = str(config.get("target_file", "")).strip()
    prefixes = str(config.get("vanity_prefixes", "")).strip()
    if not target and not (mode == "vanity" and prefixes):
        errors.append("A target file is required for this mode.")
    if target and not (ROOT / Path(target)).expanduser().exists() and not Path(target).expanduser().is_absolute():
        errors.append(f"Target file does not exist: {target}")
    elif target and Path(target).expanduser().is_absolute() and not Path(target).expanduser().exists():
        errors.append(f"Target file does not exist: {target}")

    if mode != "minikeys" and config.get("range_kind") == "bits":
        try:
            bits = int(str(config.get("range_value", "")))
            if not 1 <= bits <= 256:
                raise ValueError
        except ValueError:
            errors.append("Bit range must be an integer from 1 to 256.")
    elif mode != "minikeys" and config.get("range_kind") == "range" and not str(config.get("range_value", "")).strip():
        errors.append("Enter a hexadecimal range.")

    for key, label, allow_zero in (("threads", "Threads", False), ("stats_seconds", "Stats interval", True)):
        try:
            value = int(str(config.get(key, "")))
            if value < 0 or (value == 0 and not allow_zero):
                raise ValueError
        except ValueError:
            errors.append(f"{label} must be {'zero or greater' if allow_zero else 'greater than zero'}.")

    base = str(config.get("minikey_base", "")).strip()
    if mode in ("address", "rmd160") and str(config.get("checkpoint_file", "")).strip():
        if (mode == "address" and config.get("crypto") != "btc") or config.get("endomorphism"):
            errors.append("Scan checkpoints require Bitcoin mode without endomorphism.")
        stride = str(config.get("stride", "")).strip()
        if stride:
            try:
                if int(stride, 16 if stride.lower().startswith("0x") else 10) != 1:
                    raise ValueError
            except ValueError:
                errors.append("Scan checkpoints require stride 1.")
    if mode == "minikeys" and base and len(base) != 22:
        errors.append("The base minikey must be exactly 22 characters.")
    try:
        build_command(config, binary)
    except ValueError as exc:
        errors.append(f"Advanced arguments: {exc}")
    if not (ROOT / binary).exists() and not Path(binary).is_absolute():
        errors.append("The keyhunt binary is missing; press B to build it.")
    elif Path(binary).is_absolute() and not Path(binary).exists():
        errors.append(f"Binary does not exist: {binary}")
    return errors


def prepare_bsgs_cache(config: Dict[str, object]) -> Path | None:
    """Create the selected BSGS cache directory before launching Keyhunt."""
    if config.get("mode") != "bsgs" or not config.get("save_tables"):
        return None
    value = str(config.get("cache_dir", "")).strip()
    if not value:
        raise OSError("choose a BSGS cache directory when table saving is enabled")
    path = Path(value).expanduser()
    path.mkdir(parents=True, exist_ok=True)
    if not path.is_dir():
        raise OSError(f"not a directory: {path}")
    return path


def load_config(path: Path) -> tuple[Dict[str, object], str]:
    config = default_config()
    try:
        saved = json.loads(path.read_text(encoding="utf-8"))
        if isinstance(saved, dict):
            config.update({key: value for key, value in saved.items() if key in config})
        return config, f"Loaded {path.name}"
    except FileNotFoundError:
        return config, "Ready"
    except (OSError, ValueError) as exc:
        return config, f"Could not load settings: {exc}"


def save_config(config: Dict[str, object], path: Path) -> str:
    try:
        path.write_text(json.dumps(config, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return f"Saved {path.name}"
    except OSError as exc:
        return f"Could not save settings: {exc}"


def safe_add(window, y: int, x: int, text: object, width: int, attr: int = 0) -> None:
    height, screen_width = window.getmaxyx()
    if y < 0 or y >= height or x >= screen_width or width <= 0:
        return
    try:
        window.addnstr(y, x, str(text), min(width, screen_width - x), attr)
    except curses.error:
        pass


def edit_value(stdscr, label: str, current: object) -> str:
    height, width = stdscr.getmaxyx()
    prompt = f"{label} [current: {current}] > "
    if len(prompt) >= width - 2:
        prompt = f"{label[:max(1, width // 3)]} > "
    stdscr.move(height - 1, 0)
    stdscr.clrtoeol()
    safe_add(stdscr, height - 1, 0, prompt, width, curses.A_BOLD)
    stdscr.refresh()
    curses.echo()
    curses.curs_set(1)
    try:
        stdscr.move(height - 1, len(prompt))
        raw = stdscr.getstr(height - 1, len(prompt), max(1, width - len(prompt) - 1))
        return raw.decode("utf-8", "replace")
    except curses.error:
        return str(current)
    finally:
        curses.noecho()
        curses.curs_set(0)


def cycle_field(config: Dict[str, object], field: Field, direction: int = 1) -> None:
    previous = str(config[field.key])
    if field.kind == "bool":
        config[field.key] = not bool(config[field.key])
    elif field.kind == "puzzle":
        choices = ["manual"] + [str(number) for number in range(1, 161)]
        try:
            index = choices.index(previous)
        except ValueError:
            index = 0
        config[field.key] = choices[(index + direction) % len(choices)]
    elif field.kind == "choice":
        choices = list(field.choices)
        try:
            index = choices.index(str(config[field.key]))
        except ValueError:
            index = 0
        config[field.key] = choices[(index + direction) % len(choices)]
    if field.key == "mode" and str(config[field.key]) != previous and config.get("puzzle") == "manual":
        new_mode = str(config[field.key])
        if config.get("target_file") == MODE_TARGET_DEFAULTS.get(previous):
            config["target_file"] = MODE_TARGET_DEFAULTS[new_mode]
        old_bits = MODE_BITS_DEFAULTS.get(previous)
        if (previous == "minikeys" and config.get("range_kind") == "default") or (
                config.get("range_kind") == "bits" and config.get("range_value") == old_bits):
            if new_mode == "minikeys":
                config["range_kind"] = "default"
            else:
                config["range_kind"] = "bits"
                config["range_value"] = MODE_BITS_DEFAULTS[new_mode]


def feed_output(chunk: str, lines: List[str], current: str,
                pending_cr: bool = False) -> tuple[str, bool]:
    """Apply newline/carriage-return terminal semantics to captured output.

    PTYs normally turn a newline into CRLF, while Keyhunt uses a bare CR to
    refresh its speed counter. Delay acting on CR until the next character so
    both forms retain the expected terminal behavior, even across reads.
    """
    chunk = ANSI_RE.sub("", chunk)
    for char in chunk:
        if pending_cr:
            pending_cr = False
            if char == "\n":
                lines.append(current.rstrip())
                current = ""
                continue
            current = ""
        if char == "\r":
            pending_cr = True
        elif char == "\n":
            lines.append(current.rstrip())
            current = ""
        elif char == "\b":
            current = current[:-1]
        elif char >= " " or char == "\t":
            current += char
            if len(current) > 8192:
                current = current[-8192:]
    if len(lines) > 5000:
        del lines[:-5000]
    return current, pending_cr


def extract_private_keys(text: str) -> List[str]:
    """Return private-key hits as canonical, zero-padded 256-bit hex."""
    found: List[str] = []
    for match in PRIVATE_KEY_RE.finditer(ANSI_RE.sub("", text)):
        value = match.group(1).lower()
        if int(value, 16) != 0:
            found.append(value.zfill(64))
    return found


def wrap_output_lines(lines: Sequence[str], width: int) -> List[str]:
    """Hard-wrap terminal output so long keys are never clipped."""
    width = max(1, width)
    wrapped: List[str] = []
    for line in lines:
        value = str(line)
        if not value:
            wrapped.append("")
            continue
        wrapped.extend(value[start:start + width] for start in range(0, len(value), width))
    return wrapped


def run_process(stdscr, command: Sequence[str], title: str) -> int:
    """Run a child in a PTY and show its live output until Enter/q."""
    master_fd, slave_fd = pty.openpty()
    try:
        process = subprocess.Popen(
            list(command), cwd=ROOT, stdin=subprocess.DEVNULL,
            stdout=slave_fd, stderr=slave_fd, start_new_session=True,
            close_fds=True,
        )
    except OSError as exc:
        os.close(master_fd)
        os.close(slave_fd)
        raise RuntimeError(str(exc)) from exc
    os.close(slave_fd)
    os.set_blocking(master_fd, False)

    lines: List[str] = []
    current = ""
    scroll = 0
    done = False
    stop_requested = False
    stop_requested_at = 0.0
    terminate_sent = False
    pending_cr = False
    started = time.monotonic()
    decoder_buffer = b""
    found_keys: List[str] = []
    found_key_set = set()
    key_scan_tail = ""

    def remember_private_keys(text: str) -> None:
        nonlocal key_scan_tail
        combined = key_scan_tail + text
        for private_key in extract_private_keys(combined):
            if private_key not in found_key_set:
                found_key_set.add(private_key)
                found_keys.append(private_key)
        key_scan_tail = ANSI_RE.sub("", combined)[-192:]

    stdscr.nodelay(True)
    try:
        while True:
            while True:
                try:
                    data = os.read(master_fd, 65536)
                    if not data:
                        break
                    decoder_buffer += data
                    try:
                        text = decoder_buffer.decode("utf-8")
                        decoder_buffer = b""
                    except UnicodeDecodeError as exc:
                        if exc.end == len(decoder_buffer):
                            text = decoder_buffer[:exc.start].decode("utf-8", "replace")
                            decoder_buffer = decoder_buffer[exc.start:]
                        else:
                            text = decoder_buffer.decode("utf-8", "replace")
                            decoder_buffer = b""
                    remember_private_keys(text)
                    current, pending_cr = feed_output(text, lines, current, pending_cr)
                except BlockingIOError:
                    break
                except OSError as exc:
                    if exc.errno != errno.EIO:
                        raise
                    break

            return_code = process.poll()
            if return_code is None and stop_requested:
                stop_age = time.monotonic() - stop_requested_at
                next_signal = signal.SIGKILL if stop_age > 4.0 else signal.SIGTERM if stop_age > 2.0 else None
                if next_signal is not None and (next_signal == signal.SIGKILL or not terminate_sent):
                    try:
                        os.killpg(process.pid, next_signal)
                        terminate_sent = True
                    except ProcessLookupError:
                        pass
            if return_code is not None and not done:
                if decoder_buffer:
                    text = decoder_buffer.decode("utf-8", "replace")
                    remember_private_keys(text)
                    current, pending_cr = feed_output(text, lines, current, pending_cr)
                    decoder_buffer = b""
                if current:
                    lines.append(current.rstrip())
                    current = ""
                lines.append("")
                lines.append(f"Process finished with exit code {return_code}.")
                done = True

            height, width = stdscr.getmaxyx()
            stdscr.erase()
            elapsed = time.monotonic() - started
            state = "finished" if done else ("stopping" if stop_requested else "running")
            safe_add(stdscr, 0, 0, f" {title} — {state} — {elapsed:,.1f}s ", width,
                     curses.color_pair(1) | curses.A_BOLD)
            safe_add(stdscr, 1, 0, shlex.join(command), width, curses.A_DIM)
            display_width = max(1, width - 1)
            output_top = 2
            if found_keys:
                result_label = f" PRIVATE KEY FOUND ({len(found_keys)} total) — full 64-digit hex "
                safe_add(stdscr, output_top, 0, result_label, width,
                         curses.color_pair(4) | curses.A_BOLD)
                output_top += 1
                for key_part in wrap_output_lines([found_keys[-1]], display_width):
                    safe_add(stdscr, output_top, 0, key_part, width,
                             curses.color_pair(4) | curses.A_BOLD)
                    output_top += 1
            output_height = max(1, height - output_top - 1)
            display = lines + ([current] if current else [])
            visual_display = wrap_output_lines(display, display_width)
            end = max(0, len(visual_display) - scroll)
            begin = max(0, end - output_height)
            for row, line in enumerate(visual_display[begin:end], start=output_top):
                if row >= height - 1:
                    break
                safe_add(stdscr, row, 0, line, width)
            if found_keys:
                footer = "KEY FOUND — Enter/q: back   Up/Down/PgUp/PgDn: scroll" if done else \
                         "KEY FOUND — q: stop   Up/Down/PgUp/PgDn: scroll"
            else:
                footer = "Enter/q: back   Up/Down/PgUp/PgDn: scroll" if done else \
                         "q: stop   Up/Down/PgUp/PgDn: scroll"
            safe_add(stdscr, height - 1, 0, footer, width, curses.A_REVERSE)
            stdscr.refresh()

            key = stdscr.getch()
            if key in (curses.KEY_UP, ord("k")):
                scroll = min(max(0, len(visual_display) - 1), scroll + 1)
            elif key in (curses.KEY_DOWN, ord("j")):
                scroll = max(0, scroll - 1)
            elif key == curses.KEY_PPAGE:
                scroll = min(max(0, len(visual_display) - 1), scroll + output_height)
            elif key == curses.KEY_NPAGE:
                scroll = max(0, scroll - output_height)
            elif done and key in (ord("q"), 10, 13, 27):
                return return_code
            elif not done and key in (ord("q"), 27) and not stop_requested:
                stop_requested = True
                stop_requested_at = time.monotonic()
                lines.append("Stopping process…")
                try:
                    os.killpg(process.pid, signal.SIGINT)
                except ProcessLookupError:
                    pass
            time.sleep(0.04)
    finally:
        stdscr.nodelay(False)
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=2)
            except (ProcessLookupError, subprocess.TimeoutExpired):
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        os.close(master_fd)


def draw_main(stdscr, config: Dict[str, object], fields: Sequence[Field], selected: int,
              offset: int, status: str, binary: str, hardware: HardwareProfile) -> None:
    height, width = stdscr.getmaxyx()
    stdscr.erase()
    safe_add(stdscr, 0, 0, " Keyhunt CPU launcher ", width, curses.color_pair(1) | curses.A_BOLD)
    safe_add(stdscr, 1, 0, hardware.summary, width, curses.A_DIM)
    list_top = 3
    list_height = max(1, height - 10)
    label_width = min(24, max(14, width // 3))
    for screen_row, field_index in enumerate(range(offset, min(len(fields), offset + list_height)), start=list_top):
        field = fields[field_index]
        active = field_index == selected
        attr = curses.color_pair(2) | curses.A_BOLD if active else 0
        value = config[field.key]
        if field.kind == "bool":
            shown = "[x] enabled" if value else "[ ] disabled"
        elif field.kind == "puzzle":
            shown = "< manual >" if value == "manual" else f"< puzzle #{value} >"
        elif field.kind == "choice":
            shown = f"< {value} >"
        else:
            shown = str(value) if str(value) else "(default / blank)"
        safe_add(stdscr, screen_row, 1, field.label, label_width - 1, attr)
        safe_add(stdscr, screen_row, label_width, shown, width - label_width - 1, attr)

    preview_row = max(list_top + 1, height - 6)
    try:
        preview = shlex.join(build_command(config, binary))
    except ValueError as exc:
        preview = f"Invalid advanced arguments: {exc}"
    safe_add(stdscr, preview_row, 0, "Command preview", width, curses.A_BOLD)
    safe_add(stdscr, preview_row + 1, 0, preview, width, curses.A_DIM)
    help_text = fields[selected].help or "Enter edits; Left/Right/Space changes selections."
    safe_add(stdscr, preview_row + 3, 0, help_text, width, curses.color_pair(3))
    safe_add(stdscr, preview_row + 4, 0, status, width)
    safe_add(stdscr, height - 1, 0,
             "↑↓ navigate  Enter edit  ←→/Space change  R run  B build  S save  D reset  Q quit",
             width, curses.A_REVERSE)
    stdscr.refresh()


def select_puzzle(config: Dict[str, object], value: object,
                  catalog: Dict[int, Dict[str, str]], hardware: HardwareProfile) -> str:
    selected = str(value).strip().lower()
    if selected == "manual":
        config["puzzle"] = "manual"
        config["puzzle_summary"] = "Manual configuration; puzzle auto-tuning is disabled."
        config["preset_signature"] = ""
        return str(config["puzzle_summary"])
    try:
        number = int(selected.lstrip("#"))
    except ValueError as exc:
        raise ValueError("enter a puzzle number from 1 to 160, or 'manual'") from exc
    if not 1 <= number <= 160:
        raise ValueError("enter a puzzle number from 1 to 160, or 'manual'")
    return apply_puzzle_preset(config, number, catalog, hardware)


def curses_main(stdscr, config_path: Path, binary: str) -> None:
    curses.curs_set(0)
    curses.use_default_colors()
    if curses.has_colors():
        curses.init_pair(1, curses.COLOR_CYAN, -1)
        curses.init_pair(2, curses.COLOR_BLACK, curses.COLOR_CYAN)
        curses.init_pair(3, curses.COLOR_YELLOW, -1)
        curses.init_pair(4, curses.COLOR_GREEN, -1)
    hardware = detect_hardware()
    config, status = load_config(config_path)
    try:
        catalog = load_puzzle_catalog()
        puzzle_value = str(config.get("puzzle", "manual"))
        if puzzle_value != "manual":
            number = int(puzzle_value)
            if config.get("preset_signature") != puzzle_signature(number, hardware):
                status = select_puzzle(config, puzzle_value, catalog, hardware)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as exc:
        catalog = {}
        config["puzzle"] = "manual"
        config["puzzle_summary"] = "Puzzle catalog unavailable; manual mode is active."
        status = f"Could not load puzzle presets: {exc}"
    selected = 0
    offset = 0

    while True:
        fields = visible_fields(config)
        selected = min(selected, len(fields) - 1)
        height, _ = stdscr.getmaxyx()
        list_height = max(1, height - 10)
        if selected < offset:
            offset = selected
        elif selected >= offset + list_height:
            offset = selected - list_height + 1
        offset = min(offset, max(0, len(fields) - list_height))
        draw_main(stdscr, config, fields, selected, offset, status, binary, hardware)
        key = stdscr.getch()

        if key in (ord("q"), ord("Q"), 27):
            save_config(config, config_path)
            return
        if key in (curses.KEY_UP, ord("k")):
            selected = (selected - 1) % len(fields)
        elif key in (curses.KEY_DOWN, ord("j"), 9):
            selected = (selected + 1) % len(fields)
        elif key in (curses.KEY_LEFT,):
            field = fields[selected]
            cycle_field(config, field, -1)
            if field.key == "puzzle" and catalog:
                status = select_puzzle(config, config["puzzle"], catalog, hardware)
        elif key in (curses.KEY_RIGHT, ord(" ")):
            field = fields[selected]
            cycle_field(config, field, 1)
            if field.key == "puzzle" and catalog:
                status = select_puzzle(config, config["puzzle"], catalog, hardware)
        elif key in (10, 13):
            field = fields[selected]
            if field.kind in ("text", "puzzle"):
                old_value = config[field.key]
                config[field.key] = edit_value(stdscr, field.label, config[field.key])
                if field.key == "puzzle":
                    try:
                        status = select_puzzle(config, config[field.key], catalog, hardware)
                    except ValueError as exc:
                        config[field.key] = old_value
                        status = str(exc)
            else:
                cycle_field(config, field)
        elif key in (ord("s"), ord("S")):
            status = save_config(config, config_path)
        elif key in (ord("d"), ord("D")):
            config = default_config()
            if catalog:
                status = select_puzzle(config, config["puzzle"], catalog, hardware)
            selected = offset = 0
            status = "Machine-tuned puzzle #66 defaults restored"
        elif key in (ord("b"), ord("B")):
            try:
                jobs = str(os.cpu_count() or 1)
                code = run_process(stdscr, ["make", f"-j{jobs}", "keyhunt"], "Build")
                status = "Build completed" if code == 0 else f"Build failed with exit code {code}"
            except RuntimeError as exc:
                status = f"Could not start build: {exc}"
        elif key in (ord("r"), ord("R"), curses.KEY_F5):
            errors = validate_config(config, binary)
            if errors:
                status = "Cannot run: " + "  ".join(errors)
            else:
                try:
                    cache_path = prepare_bsgs_cache(config)
                    status = save_config(config, config_path)
                    if cache_path is not None:
                        status += f"; BSGS cache: {cache_path}"
                    code = run_process(stdscr, build_command(config, binary), "Keyhunt")
                    status = f"Last run exited with code {code}"
                except (OSError, RuntimeError) as exc:
                    status = f"Could not start Keyhunt: {exc}"


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Terminal UI for configuring and running keyhunt")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG_PATH,
                        help="settings file (default: .keyhunt-tui.json)")
    parser.add_argument("--binary", default="./keyhunt", help="keyhunt executable to launch")
    parser.add_argument("--print-command", action="store_true",
                        help="print the command represented by saved/default settings and exit")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.print_command:
        config, _ = load_config(args.config)
        puzzle_value = str(config.get("puzzle", "manual"))
        if puzzle_value != "manual":
            hardware = detect_hardware()
            try:
                number = int(puzzle_value)
                if config.get("preset_signature") != puzzle_signature(number, hardware):
                    select_puzzle(config, puzzle_value, load_puzzle_catalog(), hardware)
            except (OSError, ValueError, KeyError, zipfile.BadZipFile) as exc:
                print(f"Could not apply puzzle preset: {exc}", file=sys.stderr)
                return 2
        print(shlex.join(build_command(config, args.binary)))
        return 0
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        print("keyhunt_tui.py needs an interactive terminal. Use --print-command for non-interactive use.",
              file=sys.stderr)
        return 2
    try:
        curses.wrapper(curses_main, args.config, args.binary)
        return 0
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
