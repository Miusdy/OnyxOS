#!/usr/bin/env python3
"""Host tests for tools/onyxfs.py and tools/symbolize.py.

Uses only the standard library and the mkfs binary, so it runs on Linux, WSL2
and macOS without QEMU.  Every image the tests read is built in a temporary
directory by mkfs/mkfs.

    python3 test-tools.py            # layout, fsck, ls/cat/stat, put, symbolize
    python3 test-tools.py --qemu     # also boot one guest and cat a put file
"""

import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent
TOOLS = ROOT / "tools"
MKFS = ROOT / "mkfs" / "mkfs"
PREFIXES = ("riscv64-elf-", "riscv64-linux-gnu-", "riscv64-unknown-elf-",
            "riscv64-none-elf-", "riscv64-unknown-linux-gnu-")
SMOKE_NAME = "tools-put.txt"
SMOKE_TEXT = b"hello from onyxfs put\n"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


onyxfs = load("onyxfs", TOOLS / "onyxfs.py")


def find_binutils(name):
    for prefix in (os.environ.get("TOOLPREFIX"),) + PREFIXES:
        if prefix:
            path = shutil.which(prefix + name)
            if path:
                return path
    return None


NM = find_binutils("nm")


def cli(*arguments, binary=False):
    return subprocess.run([sys.executable, str(TOOLS / "onyxfs.py"),
                           *map(str, arguments)],
                          capture_output=True, text=not binary)


def symbolize(*arguments, **kwargs):
    return subprocess.run([sys.executable, str(TOOLS / "symbolize.py"),
                           *map(str, arguments)], capture_output=True, text=True,
                          **kwargs)


def setUpModule():
    if not MKFS.exists():
        subprocess.run(["make", "mkfs/mkfs"], cwd=ROOT, check=True,
                       capture_output=True)


class Workspace(unittest.TestCase):
    """One temporary directory holding a freshly built image."""

    files = {"hello.txt": b"hello from mkfs\n", "_prog.bin": b"\x7fELF fake\n"}

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="onyxfs-test-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)
        for name, content in self.files.items():
            (self.tmp / name).write_bytes(content)
        self.image = self.tmp / "fs.img"
        result = subprocess.run([str(MKFS), "fs.img", *self.files],
                                cwd=self.tmp, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def copy(self, name="copy.img"):
        target = self.tmp / name
        shutil.copyfile(self.image, target)
        return target

    def cat(self, path, image=None):
        return cli("cat", image or self.image, path, binary=True).stdout

    def fsck(self, image=None):
        return cli("fsck", image or self.image)

    def assertClean(self, image=None):
        result = self.fsck(image)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def put(self, host, name=None, image=None):
        arguments = ["put", image or self.image, host]
        if name:
            arguments.append(name)
        return cli(*arguments)


class LayoutTests(unittest.TestCase):
    def test_layout_is_self_consistent(self):
        layout = onyxfs.Layout(ROOT)
        self.assertEqual(layout.ipb * layout.dinode_size, layout.bsize)
        self.assertEqual(layout.bpb, layout.bsize * 8)
        self.assertEqual(layout.maxfile, layout.ndirect + layout.nindirect)
        self.assertEqual(layout.nindirect, layout.bsize // 4)
        self.assertEqual(layout.dirent_size, 2 + layout.dirsiz)

    def test_layout_matches_an_mkfs_image(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / "f").write_bytes(b"x")
            subprocess.run([str(MKFS), "fs.img", "f"], cwd=directory,
                           check=True, capture_output=True)
            layout = onyxfs.Layout(ROOT)
            image = onyxfs.Image.open(directory / "fs.img", layout)
            self.assertTrue(image.fsck().clean,
                            [item["message"] for item in image.fsck().errors])
            self.assertEqual(image.sb["ninodes"], layout.mkfs_ninodes)
            self.assertEqual(image.sb["nlog"], layout.logblocks + 1)

    def broken_checkout(self, edit):
        directory = Path(tempfile.mkdtemp(prefix="onyxfs-headers-"))
        self.addCleanup(shutil.rmtree, directory, True)
        (directory / "kernel").mkdir()
        (directory / "mkfs").mkdir()
        for name in ("types.h", "param.h"):
            shutil.copyfile(ROOT / "kernel" / name, directory / "kernel" / name)
        shutil.copyfile(ROOT / "mkfs" / "mkfs.c", directory / "mkfs" / "mkfs.c")
        (directory / "kernel" / "fs.h").write_text(
            edit((ROOT / "kernel" / "fs.h").read_text()))
        return directory

    def test_missing_define_names_file_and_symbol(self):
        directory = self.broken_checkout(lambda source: "\n".join(
            line for line in source.splitlines()
            if not line.startswith("#define BSIZE")))
        with self.assertRaisesRegex(onyxfs.LayoutError, "BSIZE"):
            onyxfs.Layout(directory)

    def test_unknown_struct_field_names_the_struct(self):
        directory = self.broken_checkout(lambda source: source.replace(
            "struct dinode", "struct dinod"))
        with self.assertRaisesRegex(onyxfs.LayoutError, "dinode"):
            onyxfs.Layout(directory)

    def test_broken_header_is_a_usage_error(self):
        directory = self.broken_checkout(lambda source: source.replace(
            "struct dinode", "struct dinod"))
        result = cli("--root", directory, "fsck", ROOT / "fs.img")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("dinode", result.stderr)


class ReadTests(Workspace):
    def test_ls_shows_the_files_mkfs_wrote(self):
        result = cli("--json", "ls", self.image)
        self.assertEqual(result.returncode, 0, result.stderr)
        names = {entry["path"] for entry in json.loads(result.stdout)["entries"]}
        self.assertIn("/hello.txt", names)
        self.assertIn("/prog.bin", names)  # mkfs drops the leading underscore
        self.assertIn("/home", names)

    def test_cat_and_stat_agree_with_the_host_file(self):
        self.assertEqual(self.cat("/hello.txt"), self.files["hello.txt"])
        stat = cli("--json", "stat", self.image, "/hello.txt")
        self.assertEqual(stat.returncode, 0, stat.stderr)
        entry = json.loads(stat.stdout)
        self.assertEqual(entry["size"], len(self.files["hello.txt"]))
        self.assertEqual(entry["nlink"], 1)
        self.assertEqual(entry["mode"], "0o0644")

    def test_nested_directories(self):
        result = cli("--json", "ls", self.image, "/etc", "/home")
        self.assertEqual(result.returncode, 0, result.stderr)
        paths = {entry["path"] for entry in json.loads(result.stdout)["entries"]}
        self.assertIn("/etc/passwd", paths)
        self.assertIn("/home/alice", paths)
        passwd = cli("stat", self.image, "/etc/passwd")
        self.assertEqual(passwd.returncode, 0, passwd.stderr)
        self.assertIn("0o0600", passwd.stdout)

    def test_missing_path_is_an_error(self):
        result = cli("cat", self.image, "/nope")
        self.assertEqual(result.returncode, 1)
        self.assertIn("no such file", result.stderr)


class FsckTests(Workspace):
    def corrupt(self, offset, fmt, value):
        image = self.copy()
        data = bytearray(image.read_bytes())
        struct.pack_into(fmt, data, offset, value)
        image.write_bytes(data)
        return image

    def test_clean_image(self):
        self.assertIn("clean", self.assertClean().stdout)

    def test_bad_magic(self):
        layout = onyxfs.Layout(ROOT)
        image = self.corrupt(layout.bsize, "<I", 0x12345678)
        result = self.fsck(image)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not an OnyxOS file system", result.stdout)

    def test_old_inode_format(self):
        layout = onyxfs.Layout(ROOT)
        image = self.corrupt(layout.bsize, "<I", layout.old_magic)
        result = self.fsck(image)
        self.assertEqual(result.returncode, 1)
        self.assertIn("old inode format", result.stdout)

    def test_truncated_image(self):
        layout = onyxfs.Layout(ROOT)
        image = self.copy()
        image.write_bytes(image.read_bytes()[:-layout.bsize])
        result = self.fsck(image)
        self.assertEqual(result.returncode, 1)
        self.assertIn("superblock describes", result.stdout)

    def test_image_too_small_to_hold_a_superblock(self):
        image = self.copy()
        image.write_bytes(b"\0" * 100)
        result = self.fsck(image)
        self.assertEqual(result.returncode, 1)
        self.assertIn("smaller than", result.stderr)

    def test_leaked_block_is_named(self):
        layout = onyxfs.Layout(ROOT)
        image = onyxfs.Image.open(self.image, layout)
        free = next(block for block in range(100, image.sb["size"])
                    if not image._bit(block))
        base = image.sb["bmapstart"] * layout.bsize + free // 8
        data = bytearray(self.image.read_bytes())
        data[base] |= 1 << (free % 8)
        self.image.write_bytes(data)
        result = self.fsck()
        self.assertEqual(result.returncode, 1)
        self.assertIn(f"block {free}", result.stdout)
        self.assertIn("no inode uses it", result.stdout)

    def test_bad_inode_size_is_named(self):
        layout = onyxfs.Layout(ROOT)
        image = onyxfs.Image.open(self.image, layout)
        inode = 2
        offset = (image.sb["inodestart"] * layout.bsize
                  + inode * layout.dinode_size
                  + layout.field("dinode", "size")["offset"])
        path = self.corrupt(offset, "<I", layout.maxfile * layout.bsize + 1)
        result = self.fsck(path)
        self.assertEqual(result.returncode, 1)
        self.assertIn("inode 2", result.stdout)
        self.assertIn("MAXFILE", result.stdout)

    def test_dirty_log_is_reported(self):
        layout = onyxfs.Layout(ROOT)
        image = self.corrupt(2 * layout.bsize, "<i", 1)
        result = self.fsck(image)
        self.assertEqual(result.returncode, 1)
        self.assertIn("committed log", result.stdout)


class PutTests(Workspace):
    def test_put_keeps_the_image_clean_and_other_files_intact(self):
        before = self.cat("/hello.txt")
        source = self.tmp / "note.txt"
        source.write_bytes(b"written by the host tool\n")
        result = self.put(source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertClean()
        self.assertEqual(self.cat("/note.txt"), source.read_bytes())
        self.assertEqual(self.cat("/hello.txt"), before)

    def test_put_leaves_no_temporary_file(self):
        source = self.tmp / "note.txt"
        source.write_bytes(b"x")
        self.assertEqual(self.put(source).returncode, 0)
        leftovers = sorted(p.name for p in self.tmp.iterdir())
        self.assertEqual(leftovers, sorted([*self.files, "note.txt", "fs.img"]))

    def test_put_keeps_the_image_file_mode(self):
        os.chmod(self.image, 0o640)
        source = self.tmp / "note.txt"
        source.write_bytes(b"x")
        self.assertEqual(self.put(source).returncode, 0)
        self.assertEqual(self.image.stat().st_mode & 0o777, 0o640)

    def test_put_accepts_a_file_that_needs_the_indirect_block(self):
        layout = onyxfs.Layout(ROOT)
        source = self.tmp / "big.bin"
        payload = (bytes(range(256)) * (layout.maxfile * 4))[
            :layout.maxfile * layout.bsize]
        source.write_bytes(payload)
        result = self.put(source)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertClean()
        self.assertEqual(hashlib.sha256(self.cat("/big.bin")).hexdigest(),
                         hashlib.sha256(payload).hexdigest())

    def test_put_grows_the_root_directory(self):
        for index in range(24):
            source = self.tmp / f"g{index}"
            source.write_bytes(b"payload")
            result = self.put(source, f"g{index}")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertClean()
        listing = cli("--json", "ls", self.image)
        names = {entry["path"] for entry in json.loads(listing.stdout)["entries"]}
        self.assertIn("/g23", names)

    def test_refused_puts_leave_the_image_byte_identical(self):
        layout = onyxfs.Layout(ROOT)
        digest = hashlib.sha256(self.image.read_bytes()).hexdigest()
        oversize = self.tmp / "huge.bin"
        oversize.write_bytes(b"x" * (layout.maxfile * layout.bsize + 1))
        cases = {
            "duplicate": lambda: self.put(self.tmp / "hello.txt"),
            "oversize": lambda: self.put(oversize),
            "long name": lambda: self.put(self.tmp / "hello.txt",
                                          "s" * (layout.dirsiz + 1)),
            "directory": lambda: self.put(self.tmp),
            "missing": lambda: self.put(self.tmp / "absent"),
        }
        for label, case in cases.items():
            result = case()
            self.assertEqual(result.returncode, 1, f"{label}: {result.stderr}")
            self.assertEqual(hashlib.sha256(self.image.read_bytes()).hexdigest(),
                             digest, f"{label} modified the image")

    def test_dirty_image_is_refused(self):
        layout = onyxfs.Layout(ROOT)
        image = self.copy()
        data = bytearray(image.read_bytes())
        struct.pack_into("<i", data, 2 * layout.bsize, 1)
        image.write_bytes(data)
        source = self.tmp / "note.txt"
        source.write_bytes(b"x")
        result = self.put(source, image=image)
        self.assertEqual(result.returncode, 1)
        self.assertIn("not clean", result.stderr)


class JsonTests(Workspace):
    def test_outputs_are_stable_json(self):
        cases = ((("fsck", self.image), "errors"),
                 (("ls", self.image), "entries"),
                 (("stat", self.image, "/hello.txt"), "size"))
        for arguments, key in cases:
            result = cli("--json", *arguments)
            self.assertEqual(result.returncode, 0, result.stderr)
            payload = json.loads(result.stdout)
            self.assertIn(key, payload)
            self.assertEqual(payload["image"], str(self.image))


@unittest.skipUnless(NM, "no RISC-V nm on PATH")
class SymbolizeTests(unittest.TestCase):
    def symbol(self, elf, name):
        result = subprocess.run([NM, str(elf)], capture_output=True, text=True)
        for line in result.stdout.splitlines():
            fields = line.split()
            if len(fields) == 3 and fields[2] == name:
                return "0x" + fields[0]
        self.fail(f"{name} not found in {elf}")

    def setUp(self):
        self.kernel = ROOT / "kernel" / "kernel"
        if not self.kernel.exists():
            self.skipTest("kernel/kernel is not built")

    def test_kernel_symbol(self):
        result = symbolize("--kernel", self.symbol(self.kernel, "panic"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("panic", result.stdout)
        bare = symbolize(self.symbol(self.kernel, "panic"))
        self.assertEqual(bare.returncode, 0, bare.stderr)
        self.assertIn("panic", bare.stdout)

    def test_user_symbol_and_file_prefix(self):
        program = ROOT / "user" / "_ls"
        if not program.exists():
            self.skipTest("user/_ls is not built")
        result = symbolize(f"{program}:{self.symbol(program, 'main')}")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("main", result.stdout)

    def test_stdin_and_json(self):
        result = subprocess.run(
            [sys.executable, str(TOOLS / "symbolize.py"), "--json"],
            input=f"kernel:{self.symbol(self.kernel, 'syscall')}\n",
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(payload["addresses"][0]["function"], "syscall")

    def test_unknown_address_is_not_silent(self):
        result = symbolize("0x1")
        self.assertEqual(result.returncode, 1)
        self.assertIn("??", result.stdout)
        self.assertIn("no symbol", result.stderr)

    def test_missing_addr2line_explains_what_to_do(self):
        empty = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, empty, True)
        result = subprocess.run(
            [sys.executable, str(TOOLS / "symbolize.py"), "0x80000000"],
            capture_output=True, text=True, env=dict(os.environ, PATH=empty))
        self.assertEqual(result.returncode, 2)
        self.assertIn("no addr2line found", result.stderr)
        self.assertIn("--toolprefix", result.stderr)


class QemuSmoke(unittest.TestCase):
    """Boot the real kernel against an image written by put."""

    def test_put_file_is_visible_to_the_guest(self):
        image = ROOT / "fs.img"
        if not image.exists() or not (ROOT / "kernel" / "kernel").exists():
            self.skipTest("build kernel/kernel and fs.img first")
        harness = load("harness", ROOT / "test-xv6.py")
        harness.config.cpus = 1
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "guest.txt"
            source.write_bytes(SMOKE_TEXT)
            shutil.copyfile(image, directory / "fs.img")
            # put must not run against an image a guest may be writing, so the
            # smoke test edits a copy and swaps it in for exactly one boot.
            result = cli("put", directory / "fs.img", source, SMOKE_NAME)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(cli("fsck", directory / "fs.img").returncode, 0)
            original = image.read_bytes()
            shutil.copyfile(directory / "fs.img", image)
            try:
                with harness.QEMU(False) as q:
                    q.wait_shell()
                    q.cmd(f"cat /{SMOKE_NAME}\n")
                    q.monitor(r"hello from onyxfs put", timeout=30)
            finally:
                image.write_bytes(original)


def build_suite(with_qemu):
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    for name, case in sorted(vars(sys.modules[__name__]).items()):
        if not (isinstance(case, type) and issubclass(case, unittest.TestCase)):
            continue
        if case in (unittest.TestCase, unittest.FunctionTestCase):
            continue
        if case is QemuSmoke and not with_qemu:
            continue
        suite.addTests(loader.loadTestsFromTestCase(case))
    return suite


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--qemu", action="store_true",
                        help="also boot QEMU and read the put file back")
    args = parser.parse_args(argv)
    result = unittest.TextTestRunner(verbosity=2).run(build_suite(args.qemu))
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
