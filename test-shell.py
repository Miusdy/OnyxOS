#!/usr/bin/env python3
"""Interactive shell regressions; uses a fresh, disposable fs.img per case.

Run serially with other QEMU tests in this checkout. Both CPU configurations
are covered in CI. Every assertion examines output after its own command.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import time

spec = importlib.util.spec_from_file_location("harness", Path(__file__).with_name("test-xv6.py"))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)


def expect(q, start, pattern, timeout=15):
    deadline = time.monotonic() + timeout
    while True:
        q.read()
        output = q.output[start:]
        if re.search(r"panic:|init: starting login|login: ", output):
            raise RuntimeError(f"shell/session failed: {output[-1500:]!r}")
        if re.search(pattern, output, re.M):
            return output
        if q.proc.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError(f"expected {pattern!r}: {output[-1500:]!r}")
        time.sleep(0.005)


def command(q, text):
    q.read()
    start = len(q.output)
    q.cmd(text + "\n")
    return expect(q, start, r"\$ \Z")


def command_async(q, text, timeout=15):
    """Like command(), for use while another process group writes to the
    console: the writer may print after the prompt, so do not require the
    prompt to be the last output."""
    q.read()
    start = len(q.output)
    q.cmd(text + "\n")
    return expect(q, start, r"\$ ", timeout=timeout)


def joblist(q, timeout=15):
    """The shell's job table; a background writer may interleave it."""
    return command_async(q, "jobs", timeout=timeout)


def stopped(q, pg, timeout=5):
    """Wait for the shell to report job pg as stopped.

    Group signals are delivered per member, not atomically.
    """
    deadline = time.monotonic() + timeout
    output = joblist(q, timeout=timeout)
    while f"[{pg}] stopped" not in output:
        assert time.monotonic() < deadline, f"[{pg}] did not stop"
        time.sleep(0.02)
        output = joblist(q, timeout=timeout)
    return output


def gone(q, pg, timeout=5):
    """Wait for job pg to leave the shell's job table.

    waitpg() reports a group's first zombie, so members of an interrupted
    group may still be exiting when the prompt returns. The deadline is
    what separates "the interrupt reached the job" from "it did not".
    """
    deadline = time.monotonic() + timeout
    while True:
        output = joblist(q, timeout=timeout)
        if f"[{pg}]" not in output:
            return output
        assert time.monotonic() < deadline, output
        time.sleep(0.02)


def shell_info(q):
    output = command(q, "/ps")
    rows = [line.split() for line in output.splitlines()
            if line.endswith("  sh") and line.split()[1] == "1"]
    assert len(rows) == 1, output
    row = rows[0]
    return int(row[0]), int(row[5]), int(row[6])  # pid, VSZ KiB, RSS KiB


def syntax(q):
    pid = shell_info(q)[0]
    command(q, "cd /home/alice")
    for text in ("echo >", "(echo x", "echo 1 2 3 4 5 6 7 8 9", "echo x )",
                 "echo x | >", "(echo x >)"):
        output = command(q, text)
        assert "sh: " in output, output
        assert shell_info(q)[0] == pid, "syntax error replaced shell"
        assert "\nrecovered\n" in command(q, "/echo recovered")
    command(q, "/echo cwd-preserved > marker")
    assert "\ncwd-preserved\n" in command(q, "/cat /home/alice/marker")


def memory(q):
    command(q, "echo warm | cat > shwarm")
    before = shell_info(q)
    for _ in range(1200):
        command(q, "echo x")
    after = shell_info(q)
    print(f"shell memory: {before} -> {after}", flush=True)
    assert after[0] == before[0], "shell restarted"
    assert after[1] == before[1] and after[2] <= before[2] + 4, (before, after)
    for _ in range(100):
        command(q, "echo x | >")  # reclaim incomplete trees too
    after_errors = shell_info(q)
    assert after_errors[0:2] == before[0:2] and after_errors[2] <= before[2] + 4, (before, after_errors)


def append(q):
    command(q, "echo abcdef > afile")
    command(q, "echo xy >> afile")
    assert "\nabcdef\nxy\n" in command(q, "cat afile")
    command(q, "echo first >> newfile")
    command(q, "echo second >> newfile")
    assert "\nfirst\nsecond\n" in command(q, "cat newfile")
    command(q, "echo reset > afile")
    assert "\nreset\n$ " in command(q, "cat afile")


def jobs(q):
    pid = shell_info(q)[0]
    output = command(q, "top > bgout &")
    bg = int(re.search(r"^\[(\d+)\] \d+$", output, re.M)[1])
    for program in ("top", "top | cat"):
        for _ in range(3):
            # Interrupt a foreground job. The job is stopped before fg, and
            # because the console stream is the only clock this test has,
            # nothing else may write to it: fg installs the job as the
            # terminal's foreground group before resuming it, so a refresh
            # seen after fg proves a ^C sent now reaches the job.
            start = len(q.output)
            q.cmd(program + "\n")
            expect(q, start, r"timer ticks, 1 tick = 100 ms\)\n")
            start = len(q.output)
            q.cmd(b"\x1a")
            output = expect(q, start, r"\$ \Z")
            pg = int(re.search(r"\[(\d+)\] stopped$", output, re.M)[1])
            stopped(q, pg)
            start = len(q.output)
            q.cmd(f"fg {pg}\n")
            expect(q, start, r"timer ticks, 1 tick = 100 ms\)\n")
            start = len(q.output)
            q.cmd(b"\x03")
            expect(q, start, r"\$ \Z")
            output = gone(q, pg)
            assert f"[{bg}] running" in output, output
            assert shell_info(q)[0] == pid
    # bg resumes a stopped job without handing over the terminal, so the
    # job keeps writing to the console; assert only on the job table.
    start = len(q.output)
    q.cmd("top\n")
    expect(q, start, r"timer ticks, 1 tick = 100 ms\)\n")
    start = len(q.output)
    q.cmd(b"\x1a")
    output = expect(q, start, r"\$ \Z")
    pg = int(re.search(r"\[(\d+)\] stopped$", output, re.M)[1])
    stopped(q, pg)
    start = len(q.output)
    q.cmd(f"bg {pg}\n")
    deadline = time.monotonic() + 5
    while f"[{pg}] running" not in joblist(q):
        assert time.monotonic() < deadline, "group did not resume"
        time.sleep(0.02)
    command_async(q, f"kill {pg}")
    command_async(q, f"kill {bg}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cpus", type=int, choices=(1, 3), default=int(os.environ.get("CPUS", "3")))
    parser.add_argument("--case", choices=("syntax", "memory", "append", "jobs"))
    args = parser.parse_args()
    h.config.cpus = args.cpus
    results = []
    try:
        for name, test in (("syntax", syntax), ("memory", memory), ("append", append), ("jobs", jobs)):
            if args.case and args.case != name:
                continue
            row = {"test": name, "status": "FAIL"}
            results.append(row)
            start = time.monotonic()
            try:
                with h.QEMU(True) as q:
                    q.wait_shell()
                    test(q)
                row["status"] = "PASS"
                print(f"PASS shell {name} CPU={args.cpus}", flush=True)
            except BaseException as exc:
                row["error"] = str(exc)
                raise
            finally:
                row["seconds"] = round(time.monotonic() - start, 3)
    finally:
        path = Path(h.config.artifacts) / f"{time.time_ns()}-shell-summary.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"cpus": args.cpus, "commit": h.git_metadata("rev-parse", "HEAD"),
                                    "dirty": h.git_metadata("status", "--short"),
                                    "results": results, "sessions": h.sessions}, indent=2))
        print(f"Summary: {path}", flush=True)
