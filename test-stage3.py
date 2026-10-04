#!/usr/bin/env python3
"""Real login, backoff, password persistence and on-disk compatibility checks.

Runs on a fresh disposable fs.img, like test-xv6.py. Never run concurrently
with another QEMU test using this checkout's image.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import struct
import time

spec = importlib.util.spec_from_file_location("harness", Path(__file__).with_name("test-xv6.py"))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)


def expect(q, pattern, start=0, timeout=30):
    deadline = time.monotonic() + timeout
    while True:
        q.read()
        text = q.output[start:]
        if re.search(pattern, text, re.M):
            return text
        if q.proc.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError(f"stage3: expected {pattern!r}, got {text[-1500:]!r}")
        time.sleep(0.02)


def send(q, text, pattern):
    q.read()
    start = len(q.output)
    q.cmd(text + "\n")
    return expect(q, pattern, start)


def login(q, user, password, good=True):
    send(q, user, r"Password: $")
    start = len(q.output)
    q.cmd(password + "\n")
    output = expect(q, r"\$ $" if good else "Login incorrect", start)
    assert password not in output, "password echoed to terminal"
    return len(q.output)


def command(q, text, contains=None):
    output = send(q, text, r"\$ $")
    if contains:
        assert re.search(contains, output, re.M), output
    return output


def logout(q):
    send(q, "exit", r"login: $")


def test_login():
    with h.QEMU(True) as q:
        expect(q, r"login: $")
        for i in range(2):
            start = login(q, "alice", "wrong-password", False)
            began = time.monotonic()
            q.cmd(b"\x03")  # SIG_IGN must not shorten pause-based backoff.
            expect(q, r"login: $", start)
            assert time.monotonic() - began >= (0.7 if i == 0 else 1.6), "missing backoff"
        login(q, "root", "root")
        command(q, "id", r"uid=0 gid=0")
        # A mismatch must leave the previous hash usable.
        send(q, "passwd alice", r"New password: $")
        send(q, "temporary-pass", r"Retype password: $")
        send(q, "different-pass", r"passwd: update failed")
        expect(q, r"\$ $")
        logout(q)
        login(q, "alice", "alice")
        command(q, "id", r"uid=1001 gid=1001")
        command(q, "whoami", r"^alice$")
        # Physical console SIGINT must use foreground credentials, not the
        # identity of whichever hart happened to service the UART interrupt.
        send(q, "top", r"pid.*ppid")
        start = len(q.output)
        q.cmd(b"\x03")
        expect(q, r"\$ $", start)
        command(q, "id", r"uid=1001 gid=1001")
        command(q, "echo alice-secret > /home/alice/private")
        command(q, "chmod 600 /home/alice/private")
        command(q, "login", "login: requires root")
        command(q, "passwd bob", "root only")
        command(q, "dmesg", "dmesg: klog failed")
        command(q, "ps", r"pid  ppid uid gid")
        logout(q)
        login(q, "bob", "bob")
        command(q, "id", r"uid=1002 gid=1002")
        command(q, "cat /home/alice/private", "cat: cannot open")
        command(q, "echo stolen > /home/alice/private", "open.*failed")
        command(q, "rm /home/alice/private", "rm:.*failed")
        command(q, "echo bob-secret > /home/bob/private")
        command(q, "chmod 600 /home/bob/private")
        logout(q)
        login(q, "root", "root")
        send(q, "passwd alice", r"New password: $")
        send(q, "new-alice-password", r"Retype password: $")
        send(q, "new-alice-password", "Password updated")
        expect(q, r"\$ $")
        # An overlong password must not authenticate as its 63-byte prefix.
        send(q, "passwd bob", r"New password: $")
        send(q, "b" * 63, r"Retype password: $")
        send(q, "b" * 63, "Password updated")
        expect(q, r"\$ $")
        logout(q)
        start = login(q, "bob", "b" * 64, False)
        expect(q, r"login: $", start)
        login(q, "bob", "b" * 63)
        command(q, "id", r"uid=1002 gid=1002")
        logout(q)
        login(q, "root", "root")
        command(q, "sync")
    # Keep the image. Verify both authentication and inode permissions survive reboot.
    with h.QEMU(False) as q:
        expect(q, r"login: $")
        start = login(q, "alice", "alice", False)
        expect(q, r"login: $", start)
        login(q, "alice", "new-alice-password")
        command(q, "cat /home/alice/private", r"^alice-secret$")
        command(q, "ls -l /home/alice/private", r"-rw-------.*1001")
        command(q, "cat /home/bob/private", "cat: cannot open")
        command(q, "echo stolen > /home/bob/private", "open.*failed")
        logout(q)
        start = login(q, "unknown", "unknown", False)
        expect(q, r"login: $", start)
        login(q, "root", "root")
        command(q, "cat /home/bob/private", r"^bob-secret$")


def test_invalid_images():
    image = Path("fs.img")
    saved = image.read_bytes()
    try:
        old = bytearray(saved)
        struct.pack_into("<I", old, 1024, 0x10203040)
        image.write_bytes(old)
        with h.QEMU(False) as q:
            expect(q, "old inode format")
            assert "login: " not in q.output
    finally:
        image.write_bytes(saved)
    # Keep a byte-for-byte image backup so malformed-account tests cannot lock
    # subsequent tests out or make failure diagnostics depend on test order.
    try:
        with h.QEMU(False) as q:
            q.wait_shell()
            command(q, "chmod 644 /etc/passwd")
            start = len(q.output)
            q.cmd("exit\n")
            expect(q, "login: invalid account database", start)
            assert not re.search(r"\$ $", q.output[start:], re.M)
    finally:
        image.write_bytes(saved)
    try:
        with h.QEMU(False) as q:
            q.wait_shell()
            command(q, "echo corrupt > /etc/passwd")
            start = len(q.output)
            q.cmd("exit\n")
            expect(q, "login: invalid account database", start)
            assert not re.search(r"\$ $", q.output[start:], re.M)
    finally:
        image.write_bytes(saved)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpus", type=int, choices=(1, 3), default=int(os.environ.get("CPUS", 3)))
    args = parser.parse_args()
    h.config.cpus = args.cpus
    results = []
    started = time.time_ns()
    try:
        for test in (test_login, test_invalid_images):
            row = {"test": test.__name__, "status": "FAIL"}
            results.append(row)
            test()
            row["status"] = "PASS"
            print(f"PASS {test.__name__} CPU={args.cpus}")
    finally:
        Path(h.config.artifacts).mkdir(exist_ok=True)
        (Path(h.config.artifacts) / f"{started}-stage3-summary.json").write_text(json.dumps({
            "cpus": args.cpus, "commit": h.git_metadata("rev-parse", "HEAD"),
            "dirty": h.git_metadata("status", "--short"), "results": results,
            "sessions": h.sessions,
        }, indent=2))


if __name__ == "__main__":
    main()
