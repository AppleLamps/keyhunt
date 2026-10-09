#!/usr/bin/env python3
"""End-to-end checkpoint tests on Linux/WSL, including a hard-killed scanner."""
import hashlib
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = Path(sys.argv[1] if len(sys.argv) > 1 else "./keyhunt").resolve()
sys.argv[1:] = []
P = 2**256 - 2**32 - 977
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)


def add(a, b):
    if a is None:
        return b
    if b is None:
        return a
    slope = ((3 * a[0] * a[0]) * pow(2 * a[1], -1, P) if a == b else
             (b[1] - a[1]) * pow(b[0] - a[0], -1, P)) % P
    x = (slope * slope - a[0] - b[0]) % P
    return x, (slope * (a[0] - x) - a[1]) % P


def hash160(key, compressed=True):
    point, q = None, G
    while key:
        if key & 1:
            point = add(point, q)
        q = add(q, q)
        key >>= 1
    x, y = point
    public = bytes([2 + (y & 1)]) + x.to_bytes(32, "big") if compressed else b"\x04" + x.to_bytes(32, "big") + y.to_bytes(32, "big")
    return hashlib.new("ripemd160", hashlib.sha256(public).digest()).hexdigest()


def read_state(path):
    data = path.read_bytes()
    body, checksum = data.rsplit(b"sha256 ", 1)
    if checksum.strip().decode() != hashlib.sha256(body).hexdigest():
        raise ValueError("torn checksum")
    lines = body.decode().splitlines()
    result = dict(line.split(" ", 1) for line in lines[1:9])
    result["pending"] = [int(line, 16) for line in lines[10:]]
    result["next"] = int(result["next"], 16)
    return result


class CheckpointTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="keyhunt-resume-")
        self.work = Path(self.temporary.name)
        self.target = self.work / "target.rmd"
        self.target.write_text("00" * 20 + "\n")
        self.path = self.work / "progress.scan"

    def tearDown(self):
        self.temporary.cleanup()

    def command(self, order="-L", threads=4, end="200000001", block="0x100000"):
        return [str(BINARY), "-m", "rmd160", "-f", str(self.target), "-r", f"1:{end}",
                "-n", block, "-t", str(threads), "-l", "compress", "-s", "0", "-M",
                order, "-P", str(self.path)]

    def run_scan(self, command, env=None):
        return subprocess.run(command, cwd=self.work, env=env, capture_output=True, text=True, timeout=60)

    def test_sequential_and_random_coverage_tail_and_finished_resume(self):
        end = 4099
        expected = {1, 2, 1024, 2048, end - 1}
        # Targets just outside the end catch padding lanes accidentally reported.
        for simd in ("none", "avx2"):
            for order in ("-L", "-R"):
                for form in ("compress", "uncompress", "both"):
                    with self.subTest(simd=simd, order=order, form=form):
                        hashes = [hash160(k, form != "uncompress") for k in sorted(expected | {end, end + 1})]
                        self.target.write_text("\n".join(hashes + ["00" * 20]) + "\n")
                        self.path = self.work / f"{simd}-{order}-{form}.scan"
                        command = self.command(order, end=f"{end:x}", block="0x400")
                        command[command.index("-l") + 1] = form
                        result = self.run_scan(command, dict(os.environ, KEYHUNT_SIMD=simd))
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        found = {int(k, 16) for k in re.findall(r"Private Key: ([0-9a-f]+)", result.stdout)}
                        self.assertEqual(found, expected)
                        bases = [int(k, 16) for k in re.findall(r"Base key: ([0-9a-f]+)", result.stdout)]
                        self.assertEqual(sorted(bases), [1 + i * 1024 for i in range(5)])
                        state = read_state(self.path)
                        self.assertEqual(state["next"], 5)
                        self.assertEqual(state["pending"], [])
                        resumed = self.run_scan(command, dict(os.environ, KEYHUNT_SIMD=simd))
                        self.assertEqual(resumed.returncode, 0, resumed.stdout + resumed.stderr)
                        self.assertNotIn("Base key:", resumed.stdout)
                        self.assertNotIn("Private Key:", resumed.stdout)

    def wait_state(self, predicate):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            try:
                state = read_state(self.path)
                if predicate(state):
                    return state
            except (FileNotFoundError, ValueError):
                pass
            time.sleep(0.05)
        self.fail("checkpoint did not advance")

    def test_hard_kill_resume_different_threads_locking_and_clean_stop(self):
        output_path = self.work / "first.log"
        with output_path.open("wb") as output:
            process = subprocess.Popen(self.command(), cwd=self.work, stdout=output, stderr=subprocess.STDOUT)
            try:
                self.wait_state(lambda s: s["next"] - len(s["pending"]) >= 4)
                # Pause before testing contention so progress stays stable.
                process.send_signal(signal.SIGSTOP)
                locked = self.run_scan(self.command(threads=1))
                self.assertNotEqual(locked.returncode, 0)
                self.assertIn("cannot lock", locked.stderr)
            finally:
                process.kill()
                process.wait(timeout=5)
        saved = read_state(self.path)
        completed = set(range(saved["next"])) - set(saved["pending"])
        self.assertGreaterEqual(len(completed), 4)
        with (self.work / "second.log").open("wb") as output:
            resumed = subprocess.Popen(self.command(threads=6), cwd=self.work, stdout=output, stderr=subprocess.STDOUT)
            try:
                self.wait_state(lambda s: s["next"] > saved["next"] + 4)
                resumed.send_signal(signal.SIGINT)
                resumed.wait(timeout=20)
                self.assertEqual(resumed.returncode, 0)
            finally:
                if resumed.poll() is None:
                    resumed.kill()
                    resumed.wait(timeout=5)
        text = (self.work / "second.log").read_text()
        ids = [(int(base, 16) - 1) // 0x100000 for base in re.findall(r"Base key: ([0-9a-f]+)", text)]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertFalse(completed.intersection(ids), "previously completed work was repeated")
        self.assertTrue(set(saved["pending"]).issubset(ids), "unfinished work was skipped")

        incompatible = self.command(end="300000001")
        result = self.run_scan(incompatible)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("settings differ", result.stderr)
        self.target.write_text("11" * 20 + "\n")
        result = self.run_scan(self.command())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("settings differ", result.stderr)
        self.path.write_text("broken checkpoint")
        result = self.run_scan(self.command())
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checksum failed", result.stderr)

    def test_unsupported_settings_are_rejected(self):
        for extra in (["-e"], ["-I", "2"], ["-c", "eth"], ["-m", "xpoint"]):
            result = self.run_scan(self.command() + extra)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(self.path.exists())


if __name__ == "__main__":
    unittest.main()
