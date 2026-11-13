#!/usr/bin/env python3
"""Turn guest addresses into function names with addr2line.

Input, on the command line or on stdin:

    0x80001234              address in the kernel (default)
    kernel:0x80001234       address in kernel/kernel
    user/_ls:0x1234         address in that ELF
    ls:0x1234               address in user/_ls
    --kernel 0x80001234     same as the first form, spelled out

The tool prefix is taken from --toolprefix, then TOOLPREFIX, then probed as
riscv64-elf-, riscv64-linux-gnu- and the other common cross prefixes, so the
same command works on macOS, WSL2 and the Linux CI image.  If no addr2line
can be found the tool names every prefix it tried and exits 2; it never
prints an empty result as if the address had no symbol.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREFIXES = ("riscv64-elf-", "riscv64-linux-gnu-", "riscv64-unknown-elf-",
            "riscv64-none-elf-", "riscv64-unknown-linux-gnu-")


class ToolError(Exception):
    pass


def find_addr2line(prefixes):
    tried = []
    for prefix in prefixes:
        name = prefix + "addr2line"
        if name not in tried:
            tried.append(name)
        path = shutil.which(name)
        if path:
            return path, name
    raise ToolError(
        "no addr2line found; tried " + ", ".join(tried)
        + "\n  install the RISC-V binutils, put them on PATH, or pass "
          "--toolprefix <prefix>/ set TOOLPREFIX (for example "
          "--toolprefix riscv64-linux-gnu-)")


def candidates(explicit):
    seen = []
    for prefix in (explicit, os.environ.get("TOOLPREFIX")) + PREFIXES:
        if prefix and prefix not in seen:
            seen.append(prefix)
    return seen


def parse_address(text, where):
    text = text.strip()
    try:
        return int(text, 16)
    except ValueError:
        raise ToolError(f"{where}: {text!r} is not a hexadecimal address") from None


def resolve_elf(name, root):
    if name in ("kernel", "kernel/kernel"):
        return os.path.join(root, "kernel", "kernel")
    if os.sep in name or name.endswith(".elf"):
        return name if os.path.isabs(name) else os.path.join(root, name)
    for candidate in (name, "_" + name):
        path = os.path.join(root, "user", candidate)
        if os.path.exists(path):
            return path
    raise ToolError(
        f"{name}: no such ELF (looked for {os.path.join(root, 'user', name)} "
        f"and {os.path.join(root, 'user', '_' + name)})")


def parse_item(token):
    """Split "file:addr" into (elf, address); a bare address has no elf."""
    left, _, right = token.rpartition(":")
    if left and re.fullmatch(r"0[xX][0-9a-fA-F]+|[0-9a-fA-F]+", right):
        return left, parse_address(right, token)
    return None, parse_address(token, token)


def collect(arguments, stream):
    items = []
    for argument in arguments:
        items.append(parse_item(argument))
    if arguments:
        return items
    for number, line in enumerate(stream, 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) > 1 and parts[0].endswith(":"):
            items.append((parts[0][:-1], parse_address(parts[1], line)))
        else:
            items.extend(parse_item(token) for token in parts)
    return items


def symbolize(addr2line, elf, addresses):
    result = subprocess.run(
        [addr2line, "-f", "-C", "-e", elf] + [hex(a) for a in addresses],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise ToolError(f"{addr2line} failed for {elf}: "
                        f"{result.stderr.strip() or result.stdout.strip()}")
    lines = result.stdout.splitlines()
    if len(lines) < 2 * len(addresses):
        raise ToolError(
            f"{addr2line} returned {len(lines)} lines for {len(addresses)} "
            f"addresses from {elf}")
    out = []
    for index, address in enumerate(addresses):
        function = lines[2 * index].strip()
        location = lines[2 * index + 1].strip()
        out.append({"elf": elf, "address": hex(address), "function": function,
                    "location": location})
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0], epilog=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("addresses", nargs="*",
                        help="0x..., file:addr or none to read stdin")
    parser.add_argument("--root", default=ROOT,
                        help="checkout holding kernel/ and user/ (default: %(default)s)")
    parser.add_argument("--kernel", action="append", default=[], metavar="ADDR",
                        help="address in the kernel ELF (repeatable)")
    parser.add_argument("--elf", metavar="PATH",
                        help="ELF for the positional addresses (default: kernel/kernel)")
    parser.add_argument("--user", metavar="NAME",
                        help="ELF for the positional addresses: user/_NAME")
    parser.add_argument("--toolprefix", help="binutils prefix, e.g. riscv64-linux-gnu-")
    parser.add_argument("--json", action="store_true",
                        help="stable machine-readable output")
    args = parser.parse_args(argv)
    try:
        if args.elf and args.user:
            parser.error("--elf and --user are mutually exclusive")
        if args.user:
            default_elf = resolve_elf(args.user, args.root)
        elif args.elf:
            default_elf = args.elf
        else:
            default_elf = os.path.join(args.root, "kernel", "kernel")
        addr2line, name = find_addr2line(candidates(args.toolprefix))
        items = [parse_item(a) for a in args.addresses]
        items += [("kernel", parse_address(a, a)) for a in args.kernel]
        if not items:
            if sys.stdin.isatty():
                parser.error("no addresses given (pass one, or pipe them on stdin)")
            items = collect([], sys.stdin)
        if not items:
            parser.error("no addresses on stdin")
        groups = {}
        for elf, address in items:
            path = resolve_elf(elf, args.root) if elf else default_elf
            if not os.path.exists(path):
                raise ToolError(f"{path}: no such file; build it first")
            groups.setdefault(path, []).append(address)
        rows = []
        for path, addresses in groups.items():
            for row in symbolize(addr2line, path, addresses):
                rows.append(row)
        unresolved = [row for row in rows if row["function"] == "??"]
        if args.json:
            print(json.dumps({"command": "symbolize", "addr2line": name,
                              "resolved": not unresolved,
                              "addresses": rows}, indent=2, sort_keys=True))
        else:
            for row in rows:
                print(f"{row['address']:>18} {row['function']} at {row['location']}"
                      f"  [{row['elf']}]")
            if unresolved:
                print(f"symbolize: {len(unresolved)} address(es) have no symbol; "
                      "the address may not be a code address in this ELF",
                      file=sys.stderr)
        return 1 if unresolved else 0
    except ToolError as exc:
        print(f"symbolize: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
