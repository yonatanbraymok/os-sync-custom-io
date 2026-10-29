#!/usr/bin/env python3
"""
Sanity checker for OS Exercise 2.

Run this before submitting. It compiles your code and runs each part with
small inputs, checking only that things basically work:
  * `make` succeeds with no warnings.
  * Each part runs without crashing or timing out.
  * Parts 2 and 3 produce well-formed output at small load.
  * Part 1 produces the right number of output bytes (we DON'T check format
    here — Part 1 is supposed to produce corrupt output sometimes).
  * The `elapsed_ms=...` line shows up on stderr.

This is NOT a grader. Passing this script does not mean your code is correct
or complete — it just means it's not obviously broken. Edge cases and stress
loads are tested separately.

Usage (from the repo root):
    python3 tests/sanity.py
"""
import re
import subprocess
import sys
from pathlib import Path

class C:
    OK = "\033[32m"; FAIL = "\033[31m"; INFO = "\033[36m"
    DIM = "\033[2m"; BOLD = "\033[1m"; RST = "\033[0m"
    if not sys.stdout.isatty():
        OK = FAIL = INFO = DIM = BOLD = RST = ""

def info(m):  print(f"{C.INFO}[..]{C.RST} {m}")
def ok(m):    print(f"{C.OK}[ok]{C.RST} {m}")
def fail(m):  print(f"{C.FAIL}[XX]{C.RST} {m}")

LINE_RE = re.compile(
    rb'^\[Type: (PROCESS|THREAD)\] '
    rb'\[ID: \d+\] '
    rb'\[Seq: \d{3}\] '
    rb'.+$'
)
TIMING_RE = re.compile(rb'elapsed_ms=\d+\.\d{3}')

REPO   = Path(__file__).resolve().parent.parent
SRC    = REPO / "src"

def run(cmd, cwd=None, timeout=30):
    try:
        p = subprocess.run(cmd, cwd=cwd, timeout=timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired as e:
        return None, e.stdout or b"", e.stderr or b""

def expected_line_len(message, type_, id_digits=1):
    sample = f"[Type: {type_}] [ID: {'0'*id_digits}] [Seq: 000] {message}\n"
    return len(sample.encode("utf-8"))

failures = 0
def report_fail(msg):
    global failures
    failures += 1
    fail(msg)

# --- 1. compile ---
print(f"{C.BOLD}Sanity check{C.RST}")
info("compiling…")
run(["make", "clean"], cwd=SRC)
rc, _, err = run(["make", "-B"], cwd=SRC, timeout=60)
err_text = err.decode("utf-8", errors="replace")
if rc != 0:
    report_fail("make failed")
    sys.stderr.write(err_text)
    sys.exit(1)
warns = [ln for ln in err_text.splitlines() if "warning:" in ln.lower()]
if warns:
    report_fail(f"build produced {len(warns)} warning(s):")
    for w in warns[:5]: print(f"   {C.DIM}{w}{C.RST}")
else:
    ok("compiled cleanly")

# --- 2. each part runs and produces output ---
def check_part(name, args, expect_type, num_workers, writes_per_worker, strict):
    info(f"running {name} {' '.join(args)} …")
    binary = SRC / name
    if not binary.exists():
        report_fail(f"{name}: binary not found"); return
    output = SRC / "output.txt"
    output.unlink(missing_ok=True)
    rc, _, err = run([str(binary)] + args, cwd=SRC, timeout=30)
    if rc is None:
        report_fail(f"{name}: timed out"); return
    if rc != 0:
        report_fail(f"{name}: exit code {rc}"); return
    if not TIMING_RE.search(err):
        report_fail(f"{name}: no `elapsed_ms=…` on stderr")
    if not output.exists():
        report_fail(f"{name}: output.txt was not created"); return
    if strict:
        lines = output.read_bytes().splitlines()
        expected = num_workers * writes_per_worker
        if len(lines) != expected:
            report_fail(f"{name}: expected {expected} lines, got {len(lines)}"); return
        bad = sum(1 for ln in lines if not LINE_RE.match(ln))
        if bad:
            report_fail(f"{name}: {bad} line(s) failed the format regex"); return
        # check expected_type
        wrong = sum(1 for ln in lines
                    if (m := LINE_RE.match(ln)) and m.group(1).decode() != expect_type)
        if wrong:
            report_fail(f"{name}: {wrong} line(s) had wrong Type (expected {expect_type})"); return
        ok(f"{name}: {len(lines)} well-formed lines")
    else:
        elen = expected_line_len(args[0], expect_type)
        actual = output.stat().st_size
        expected_bytes = num_workers * writes_per_worker * elen
        if actual != expected_bytes:
            report_fail(f"{name}: expected {expected_bytes} bytes, got {actual}"); return
        ok(f"{name}: {actual} bytes (correct total)")

check_part("part1", ["hi", "4", "50"],  "PROCESS", 4, 50,  strict=False)
check_part("part2", ["hi", "4", "100"], "THREAD",  4, 100, strict=True)
check_part("part3", ["hi", "4", "100"], "PROCESS", 4, 100, strict=True)

# --- 3. argv validation ---
info("checking argv handling…")
rc, _, err = run([str(SRC / "part1")], cwd=SRC)
if rc == 1 and b"Usage" in err:
    ok("part1 with no args: prints Usage and exits 1")
else:
    report_fail(f"part1 with no args: rc={rc}, expected 1 with Usage on stderr")

# --- cleanup ---
(SRC / "output.txt").unlink(missing_ok=True)

print()
if failures == 0:
    print(f"{C.OK}{C.BOLD}Sanity check passed.{C.RST}")
    print(f"{C.DIM}Your code compiles and runs at a small load. This is the minimum bar — "
          f"larger scenarios may still uncover issues.{C.RST}")
    sys.exit(0)
else:
    print(f"{C.FAIL}{C.BOLD}{failures} sanity check(s) failed.{C.RST}")
    sys.exit(1)
