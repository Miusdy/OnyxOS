#!/usr/bin/env python3
"""Inspect and modify an OnyxOS fs.img from the host.

Subcommands:

    ls    list a directory (or describe one path)
    cat   write a file's bytes to stdout
    stat  show one inode's fields
    fsck  check superblock, log, bitmap, inodes, directories and nlink
    put   add a regular file to the image (clean images only)

The on-disk layout is read from kernel/fs.h, kernel/types.h, kernel/param.h
and mkfs/mkfs.c on every run: block size, inode size, inode count, log size
and the derived counts are never written down here.  If a header cannot be
parsed the tool says which file and which name failed instead of falling
back to a default.

Exit codes: 0 success, 1 findings or refusal, 2 usage or layout error.
"""

import argparse
import base64
import hashlib
import json
import os
import re
import struct
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class LayoutError(Exception):
    """A C header did not say what the on-disk format needs."""


class ImageError(Exception):
    """The image cannot be read or does not describe a usable file system."""


class Refused(Exception):
    """The operation is unsafe for this image."""


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


_DEFINE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)(?!\()\s+(.+?)\s*$", re.M)
_STRUCT = re.compile(r"struct\s+([A-Za-z_]\w*)\s*\{(.*?)\}\s*;", re.S)
_TYPEDEF = re.compile(r"typedef\s+([A-Za-z_][\w\s]*?)\s+([A-Za-z_]\w*)\s*;")
_FIELD = re.compile(
    r"^\s*([A-Za-z_][\w\s]*?)\s+([A-Za-z_]\w*)\s*(?:\[([^\]]+)\])?\s*"
    r"(?:__attribute__\s*\(\(.*?\)\))?\s*$", re.S)

# Sizes the compiler on the far side of the ABI gives the C spellings that
# kernel/types.h is built from.  Typedef names are resolved through this
# table, so a header that adds an alias is a parse error, not a guess.
_C_TYPES = {
    "char": 1, "signed char": 1, "unsigned char": 1,
    "short": 2, "short int": 2, "signed short": 2, "unsigned short": 2,
    "unsigned short int": 2,
    "int": 4, "signed int": 4, "unsigned int": 4, "unsigned": 4,
    "long": 8, "long int": 8, "unsigned long": 8, "unsigned long int": 8,
    "long long": 8, "unsigned long long": 8, "uint64_t": 8,
}


class Layout:
    """On-disk constants and field offsets, read from the shared headers."""

    SOURCES = ("kernel/fs.h", "kernel/types.h", "kernel/param.h",
               "mkfs/mkfs.c")

    def __init__(self, root=ROOT):
        self.root = root
        self.text = {}
        for name in self.SOURCES:
            path = os.path.join(root, name)
            try:
                with open(path, encoding="utf-8") as source:
                    self.text[name] = _strip_comments(source.read())
            except OSError as exc:
                raise LayoutError(f"{path}: {exc.strerror}") from None
        self.defines = {}
        for name, text in self.text.items():
            for key, value in _DEFINE.findall(text):
                self.defines.setdefault(key, (value.strip(), name))
        self.structs = {}
        self.typedefs = {}
        for text in self.text.values():
            for spec, alias in _TYPEDEF.findall(text):
                self.typedefs[alias] = " ".join(spec.split())
        for text in self.text.values():
            for struct_name, body in _STRUCT.findall(text):
                self.structs[struct_name] = self._fields(struct_name, body)

        self.bsize = self.const("BSIZE")
        self.ndirect = self.const("NDIRECT")
        self.dirsiz = self.const("DIRSIZ")
        self.rootino = self.const("ROOTINO")
        self.magic = self.const("FSMAGIC")
        self.old_magic = self.const("FSMAGIC_OLD", required=False)
        self.nindirect = self.bsize // self._sizeof("uint")[0]
        self.maxfile = self.ndirect + self.nindirect
        self.logblocks = self.const("LOGBLOCKS")
        self.fssize = self.const("FSSIZE")
        self.mkfs_ninodes = self.const("NINODES")
        self.superblock = self.struct("superblock")
        self.dinode = self.struct("dinode")
        self.dinode_size = self.dinode["size"]
        self.dirent = self.struct("dirent")
        self.dirent_size = self.dirent["size"]
        if self.superblock["size"] != 8 * 4:
            raise LayoutError(
                f"kernel/fs.h: struct superblock is {self.superblock['size']} "
                f"bytes, mkfs writes eight 32-bit fields")
        self.ipb = self.bsize // self.dinode_size
        self.bpb = self.bsize * 8
        if self.bsize % self.dinode_size or self.bsize % self.dirent_size:
            raise LayoutError(
                f"kernel/fs.h: BSIZE {self.bsize} is not a multiple of the "
                f"inode ({self.dinode_size}) and directory entry "
                f"({self.dirent_size}) sizes; mkfs would not build this image")

    # -- header parsing ----------------------------------------------------

    def _fields(self, struct_name, body):
        offset = 0
        align = 1
        fields = {}
        for declaration in body.split(";"):
            if not declaration.strip():
                continue
            match = _FIELD.match(declaration)
            if not match:
                raise LayoutError(
                    f"cannot parse struct {struct_name} field "
                    f"{declaration!r}")
            spec, name, count = match.groups()
            size, field_align = self._sizeof(spec.strip())
            if count is not None:
                count = self.const_expr(count, f"struct {struct_name} field {name}")
                count_size = size
                size *= count
            else:
                count_size = None
            offset = -(-offset // field_align) * field_align
            fields[name] = {"offset": offset, "size": size,
                            "align": field_align, "element": count_size,
                            "count": count}
            offset += size
            align = max(align, field_align)
        offset = -(-offset // align) * align
        return {"name": struct_name, "fields": fields, "size": offset,
                "align": align}

    def _sizeof(self, spec):
        spec = " ".join(spec.split())
        if spec.startswith("struct "):
            name = spec.split(None, 1)[1]
            try:
                return self.structs[name]["size"], self.structs[name]["align"]
            except KeyError:
                raise LayoutError(f"unknown struct {name!r}") from None
        if spec in self.typedefs:
            return self._sizeof(self.typedefs[spec])
        if spec in _C_TYPES:
            return _C_TYPES[spec], _C_TYPES[spec]
        if spec.endswith("*"):
            return 8, 8
        raise LayoutError(f"unknown C type {spec!r} in kernel/types.h")

    def const(self, name, required=True):
        return self.const_expr(name, name, required=required)

    def const_expr(self, expr, where, required=True, seen=frozenset()):
        text = expr.strip()
        seen = set()

        def substitute(match):
            token = match.group(0)
            return str(self._value(token, where, seen, required))

        pattern = re.compile(r"sizeof\s*\(\s*(struct\s+\w+|\w+)\s*\)|0[xX][0-9a-fA-F]+|\d+|[A-Za-z_]\w*")
        expanded = pattern.sub(substitute, text)
        if not re.fullmatch(r"[0-9+\-*/() \t]+", expanded):
            raise LayoutError(f"{where}: cannot evaluate {expr!r}")
        try:
            return int(eval(expanded.replace("/", "//").replace(" ", "") or "0",
                            {"__builtins__": {}}, {}))
        except Exception:
            raise LayoutError(f"{where}: cannot evaluate {expr!r}") from None

    def _value(self, token, where, seen, required):
        if token.startswith("sizeof"):
            return self._sizeof(re.search(r"\((.*)\)", token).group(1))[0]
        if token[0].isdigit():
            if re.fullmatch(r"0[0-7]+", token):
                return int(token, 8)
            try:
                return int(token, 0)
            except ValueError:
                raise LayoutError(f"{where}: {token} is not an integer") from None
        if token in seen:
            raise LayoutError(f"{where}: {token} is defined in terms of itself")
        if token not in self.defines:
            if not required:
                raise LayoutError(f"{where}: {token} is not defined")
            raise LayoutError(f"{where}: {token} is not defined in "
                              f"kernel/fs.h, kernel/param.h or mkfs/mkfs.c")
        value, source = self.defines[token]
        seen = seen | {token}
        return self.const_expr(value, f"{source}: {token}", seen=seen)

    def struct(self, name):
        try:
            return self.structs[name]
        except KeyError:
            raise LayoutError(f"struct {name} is not defined in kernel/fs.h") from None

    def field(self, struct_name, field_name):
        struct = self.struct(struct_name)
        try:
            return struct["fields"][field_name]
        except KeyError:
            raise LayoutError(
                f"struct {struct_name} has no field {field_name}") from None


class Image:
    """A read-only view of one fs.img."""

    def __init__(self, data, layout, name="fs.img"):
        self.data = bytes(data)
        self.layout = layout
        self.name = name
        if len(self.data) < 2 * layout.bsize:
            raise ImageError(
                f"{name}: {len(self.data)} bytes is smaller than the boot and "
                f"super blocks ({2 * layout.bsize} bytes)")
        fields = [field for field, _ in sorted(
            self.layout.superblock["fields"].items(),
            key=lambda item: item[1]["offset"])]
        values = struct.unpack_from("<8I", self.data, layout.bsize)
        self.sb = dict(zip(fields, values))

    @classmethod
    def open(cls, path, layout):
        try:
            with open(path, "rb") as image:
                return cls(image.read(), layout, path)
        except OSError as exc:
            raise ImageError(f"{path}: {exc.strerror}") from None

    # -- raw access --------------------------------------------------------

    def block(self, number):
        start = number * self.layout.bsize
        if number < 0 or start + self.layout.bsize > len(self.data):
            raise ImageError(
                f"{self.name}: block {number} is outside the image")
        return self.data[start:start + self.layout.bsize]

    def inode(self, inum):
        layout = self.layout
        if not 0 <= inum < self.sb["ninodes"]:
            raise ImageError(f"{self.name}: inode {inum} is out of range")
        block = self.sb["inodestart"] + inum // layout.ipb
        base = block * layout.bsize + (inum % layout.ipb) * layout.dinode_size
        if base + layout.dinode_size > len(self.data):
            raise ImageError(f"{self.name}: inode {inum} is outside the image")
        raw = self.data[base:base + layout.dinode_size]
        values = {}
        for name, field in layout.dinode["fields"].items():
            if field["count"] is not None:
                fmt = _FORMAT[field["element"]]
                values[name] = list(struct.unpack_from(
                    "<%d%s" % (field["count"], fmt), raw, field["offset"]))
            else:
                values[name] = struct.unpack_from(
                    "<" + _FORMAT[field["size"]], raw, field["offset"])[0]
        return values

    def inodes(self):
        for inum in range(self.sb["ninodes"]):
            dinode = self.inode(inum)
            if dinode["type"] != 0:
                yield inum, dinode

    def bmap(self, dinode, logical):
        layout = self.layout
        addrs = dinode["addrs"]
        if logical < layout.ndirect:
            return addrs[logical]
        indirect = addrs[layout.ndirect]
        if indirect == 0:
            return 0
        entries = struct.unpack_from(
            "<%dI" % layout.nindirect, self.block(indirect))
        if logical - layout.ndirect >= layout.nindirect:
            return 0
        return entries[logical - layout.ndirect]

    def read_inode(self, inum):
        dinode = self.inode(inum)
        size = dinode["size"]
        limit = self.layout.maxfile * self.layout.bsize
        if size > limit:
            raise ImageError(
                f"{self.name}: inode {inum} claims {size} bytes, MAXFILE*BSIZE "
                f"is {limit}; run fsck")
        data = bytearray(size)
        for logical in range((size + self.layout.bsize - 1) // self.layout.bsize):
            block = self.bmap(dinode, logical)
            if block == 0:
                continue
            start = logical * self.layout.bsize
            chunk = self.block(block)[:min(self.layout.bsize, size - start)]
            data[start:start + len(chunk)] = chunk
        return bytes(data)

    def read_at(self, dinode, offset, count):
        if offset + count > dinode["size"]:
            raise ImageError(
                f"{self.name}: read past end of inode (offset {offset}, "
                f"count {count}, size {dinode['size']})")
        if dinode["size"] > self.layout.maxfile * self.layout.bsize:
            raise ImageError(
                f"{self.name}: inode claims {dinode['size']} bytes, larger "
                f"than MAXFILE*BSIZE; run fsck")
        data = bytearray(count)
        done = 0
        while done < count:
            logical, within = divmod(offset + done, self.layout.bsize)
            block = self.bmap(dinode, logical)
            take = min(count - done, self.layout.bsize - within)
            if block:
                start = block * self.layout.bsize + within
                data[done:done + take] = self.data[start:start + take]
            done += take
        return bytes(data)

    def entries(self, dinode, inum):
        size = dinode["size"]
        if size % self.layout.dirent_size:
            raise ImageError(
                f"{self.name}: inode {inum} is a directory whose size {size} "
                f"is not a multiple of the directory entry size "
                f"{self.layout.dirent_size}")
        name_field = self.layout.field("dirent", "name")
        inum_field = self.layout.field("dirent", "inum")
        out = []
        for offset in range(0, size, self.layout.dirent_size):
            raw = self.read_at(dinode, offset, self.layout.dirent_size)
            entry_inum = struct.unpack_from("<H", raw, inum_field["offset"])[0]
            raw_name = raw[name_field["offset"]:
                           name_field["offset"] + name_field["size"]]
            name = raw_name.split(b"\0", 1)[0]
            out.append({"offset": offset, "inum": entry_inum,
                        "name": name.decode("utf-8", "replace"),
                        "raw": name})
        return out

    def lookup(self, inum, name):
        dinode = self.inode(inum)
        if dinode["type"] != 1:
            raise ImageError(f"{self.name}: inode {inum} is not a directory")
        target = name.encode("utf-8")
        for entry in self.entries(dinode, inum):
            if entry["raw"] == target:
                return entry["inum"]
        return 0

    def walk(self, path):
        """Resolve an absolute or relative path to an inode number."""
        if not path:
            raise ImageError("empty path")
        inum = self.layout.rootino
        for part in [p for p in path.split("/") if p]:
            if part == ".":
                continue
            if part == "..":
                parent = self.lookup(inum, "..")
                inum = parent or self.layout.rootino
                continue
            if not inum:
                raise ImageError(f"{path}: not found in {self.name}")
            next_inum = self.lookup(inum, part)
            if next_inum == 0:
                raise ImageError(f"{path}: no such file or directory")
            inum = next_inum
        return inum

    # -- fsck --------------------------------------------------------------

    def fsck(self):
        findings = Findings()
        layout, sb = self.layout, self.sb
        if sb["magic"] == layout.old_magic:
            findings.error("superblock",
                           "fs.img uses the old inode format; re-run mkfs")
            return findings
        if sb["magic"] != layout.magic:
            findings.error(
                "superblock",
                f"magic 0x{sb['magic']:08x} is not FSMAGIC 0x{layout.magic:08x};"
                " not an OnyxOS file system")
            return findings

        inode_blocks = sb["ninodes"] // layout.ipb + 1
        bitmap_blocks = sb["size"] // layout.bpb + 1
        expects = {
            "nlog": layout.logblocks + 1,
            "logstart": 2,
            "inodestart": 2 + sb["nlog"],
            "bmapstart": sb["inodestart"] + inode_blocks,
        }
        for field, value in expects.items():
            if sb[field] != value:
                findings.error(
                    "superblock",
                    f"{field} is {sb[field]}, this layout requires {value}")
        if sb["size"] != layout.fssize:
            findings.error(
                "superblock",
                f"size is {sb['size']} blocks but param.h FSSIZE is "
                f"{layout.fssize}")
        if sb["ninodes"] != layout.mkfs_ninodes:
            findings.error(
                "superblock",
                f"ninodes is {sb['ninodes']} but mkfs NINODES is "
                f"{layout.mkfs_ninodes}")
        nmeta = sb["bmapstart"] + bitmap_blocks
        if sb["nblocks"] + nmeta != sb["size"]:
            findings.error(
                "superblock",
                f"nblocks {sb['nblocks']} + metadata {nmeta} != size "
                f"{sb['size']}")
        if nmeta > sb["size"]:
            findings.error(
                "superblock",
                f"metadata needs {nmeta} blocks, image has {sb['size']}")
            return findings
        if len(self.data) < sb["size"] * layout.bsize:
            findings.error(
                "image",
                f"{self.name} is {len(self.data)} bytes, the superblock "
                f"describes {sb['size'] * layout.bsize}")
            return findings

        self._fsck_log(findings)
        used = {}
        self._fsck_bitmap(findings, nmeta)
        reachable = self._fsck_inodes(findings, used, nmeta)
        self._fsck_directories(findings, reachable)
        self._fsck_leaks(findings, used, nmeta)
        return findings

    def _fsck_log(self, findings):
        layout, sb = self.layout, self.sb
        count = struct.unpack_from("<i", self.data, sb["logstart"] * layout.bsize)[0]
        if count < 0 or count >= sb["nlog"]:
            findings.error(
                "log", f"header counts {count} blocks, the log holds "
                f"{sb['nlog'] - 1}")
        elif count:
            findings.error(
                "log",
                f"{count} committed log block"
                f"{'' if count == 1 else 's'} pending; boot the guest to "
                "recover the image before touching it")

    def _fsck_bitmap(self, findings, nmeta):
        layout, sb = self.layout, self.sb
        for block in range(nmeta):
            if not self._bit(block):
                findings.error(
                    "bitmap", f"metadata block {block} is marked free")
        bits = (sb["size"] // layout.bpb + 1) * layout.bpb
        for block in range(sb["size"], bits):
            if self._bit(block):
                findings.error(
                    "bitmap",
                    f"bitmap marks block {block} past the end of the image")
                break

    def _bit(self, block):
        layout = self.layout
        byte = block // 8
        base = self.sb["bmapstart"] * layout.bsize + byte
        if base >= len(self.data):
            return 0
        return (self.data[base] >> (block % 8)) & 1

    def _fsck_inodes(self, findings, used, nmeta):
        layout = self.layout
        nlink_max = layout.const("NLINK_MAX")
        reachable = {}
        for inum, dinode in self.inodes():
            where = f"inode {inum}"
            if dinode["type"] not in (1, 2, 3):
                findings.error("inode", f"{where}: unknown type {dinode['type']}")
                continue
            if dinode["nlink"] == 0:
                findings.error("inode", f"{where}: type {dinode['type']} with "
                                         "nlink 0 (orphan)")
            if dinode["nlink"] > nlink_max:
                findings.error("inode", f"{where}: nlink {dinode['nlink']} "
                                         "exceeds NLINK_MAX")
            if dinode["size"] > layout.maxfile * layout.bsize:
                findings.error(
                    "inode",
                    f"{where}: size {dinode['size']} exceeds MAXFILE*BSIZE "
                    f"({layout.maxfile * layout.bsize})")
            if dinode["mode"] & ~0o777:
                findings.warning(
                    "inode", f"{where}: mode 0o{dinode['mode']:o} has bits "
                             "outside the nine permission bits")
            if dinode["type"] == 3:
                if dinode["size"]:
                    findings.error("inode", f"{where}: device with nonzero size")
                reachable[inum] = dinode
                continue
            if dinode["type"] == 1 and dinode["size"] % layout.dirent_size:
                findings.error(
                    "inode",
                    f"{where}: directory size {dinode['size']} is not a "
                    f"multiple of {layout.dirent_size}")
            self._fsck_addrs(findings, inum, dinode, used, nmeta)
            reachable[inum] = dinode
        return reachable

    def _fsck_addrs(self, findings, inum, dinode, used, nmeta):
        layout, sb = self.layout, self.sb

        def reference(block, owner):
            if block < nmeta or block >= sb["size"]:
                findings.error(
                    "block",
                    f"{owner}: block {block} is outside the data area "
                    f"[{nmeta}, {sb['size']})")
            elif not self._bit(block):
                findings.error(
                    "block", f"{owner}: block {block} is not marked allocated")
            elif block in used:
                findings.error(
                    "block", f"{owner}: block {block} is already used by "
                             f"{used[block]}")
            else:
                used[block] = owner

        blocks = (dinode["size"] + layout.bsize - 1) // layout.bsize
        for index, block in enumerate(dinode["addrs"][:layout.ndirect]):
            if block:
                reference(block, f"inode {inum} direct {index}")
            elif index < blocks:
                findings.error(
                    "inode",
                    f"inode {inum}: direct block {index} is missing but "
                    f"inside the file size")
        indirect = dinode["addrs"][layout.ndirect]
        need = max(0, blocks - layout.ndirect)
        if indirect == 0:
            if need:
                findings.error(
                    "inode",
                    f"inode {inum}: indirect block is missing but logical "
                    f"blocks {layout.ndirect}..{blocks - 1} are inside the "
                    "file size")
            return
        reference(indirect, f"inode {inum} indirect")
        entries = struct.unpack_from(
            "<%dI" % layout.nindirect, self.block(indirect))
        for index, block in enumerate(entries):
            if block:
                reference(block, f"inode {inum} indirect {index}")
            elif index < need:
                findings.error(
                    "inode",
                    f"inode {inum}: indirect entry {index} is missing but "
                    f"logical block {layout.ndirect + index} is inside the "
                    "file size")

    def _fsck_directories(self, findings, reachable):
        layout = self.layout
        root = layout.rootino
        if root not in reachable or reachable[root]["type"] != 1:
            findings.error("directory", f"root inode {root} is missing")
            return
        seen = {}
        queue = [(root, "/", root)]
        while queue:
            inum, path, parent = queue.pop(0)
            if inum in seen:
                findings.error(
                    "directory",
                    f"{path}: inode {inum} is reachable twice (also as "
                    f"{seen[inum]}); a directory has one parent")
                continue
            seen[inum] = path
            try:
                entries = self.entries(reachable[inum], inum)
            except ImageError as exc:
                findings.error("directory", str(exc))
                continue
            names = {}
            for entry in entries:
                if entry["inum"] == 0:
                    continue
                label = f"{path.rstrip('/')}/{entry['name']}"
                if not entry["name"]:
                    findings.error(
                        "directory", f"{path}: empty name at offset "
                                     f"{entry['offset']}")
                    continue
                if "/" in entry["name"]:
                    findings.error(
                        "directory", f"{path}: name {entry['name']!r} contains "
                                     "'/' and cannot be looked up")
                if entry["name"] in names:
                    findings.error(
                        "directory",
                        f"{path}: duplicate name {entry['name']!r}")
                names[entry["name"]] = entry["inum"]
                if entry["inum"] >= self.sb["ninodes"]:
                    findings.error(
                        "directory",
                        f"{path}: entry {entry['name']!r} points at inode "
                        f"{entry['inum']}, past the inode table")
                    continue
                if entry["inum"] not in reachable:
                    findings.error(
                        "directory",
                        f"{path}: entry {entry['name']!r} points at free inode "
                        f"{entry['inum']}")
                    continue
                if entry["name"] not in (".", ".."):
                    target = reachable[entry["inum"]]
                    if target["type"] == 1:
                        queue.append((entry["inum"], label, inum))
            if not entries or entries[0]["name"] != ".":
                findings.error("directory", f"{path}: first entry is not '.'")
            elif entries[0]["inum"] != inum:
                findings.error(
                    "directory",
                    f"{path}: '.' points at inode {entries[0]['inum']}, "
                    f"expected {inum}")
            if len(entries) < 2 or entries[1]["name"] != "..":
                findings.error("directory", f"{path}: second entry is not '..'")
            elif entries[1]["inum"] != parent:
                findings.error(
                    "directory",
                    f"{path}: '..' points at inode {entries[1]['inum']}, "
                    f"expected {parent}")
        self._fsck_nlink(findings, reachable, seen)

    def _fsck_nlink(self, findings, reachable, seen):
        # xv6/mkfs count a directory's own "." as the initial reference and
        # add one for each subdirectory's ".."; a file counts every entry.
        children = {inum: 0 for inum in seen}
        refs = {inum: 0 for inum in reachable}
        for inum, path in seen.items():
            try:
                entries = self.entries(reachable[inum], inum)
            except ImageError:
                continue
            for entry in entries:
                if entry["inum"] in refs:
                    refs[entry["inum"]] += 1
                if (entry["name"] not in (".", "..")
                        and entry["inum"] in reachable
                        and reachable[entry["inum"]]["type"] == 1):
                    children[inum] = children.get(inum, 0) + 1
        for inum, dinode in reachable.items():
            if refs.get(inum, 0) == 0:
                findings.error(
                    "inode", f"inode {inum} is allocated but unreachable")
                continue
            if dinode["type"] == 1:
                expected = 1 + children.get(inum, 0)
            else:
                expected = refs[inum]
            if dinode["nlink"] != expected:
                findings.error(
                    "nlink",
                    f"inode {inum} ({seen.get(inum, '?')}): nlink is "
                    f"{dinode['nlink']}, entries account for {expected}")

    def _fsck_leaks(self, findings, used, nmeta):
        for block in range(nmeta, self.sb["size"]):
            if self._bit(block) and block not in used:
                findings.error(
                    "bitmap",
                    f"block {block} is marked allocated but no inode uses it")


_FORMAT = {1: "B", 2: "H", 4: "I", 8: "Q"}


class Findings:
    def __init__(self):
        self.errors = []
        self.warnings = []

    def error(self, kind, message):
        self.errors.append({"kind": kind, "message": message})

    def warning(self, kind, message):
        self.warnings.append({"kind": kind, "message": message})

    @property
    def clean(self):
        return not self.errors

    def summary(self):
        counts = f"{len(self.errors)} error{'' if len(self.errors) == 1 else 's'}"
        return counts + (f", {len(self.warnings)} warning"
                         f"{'' if len(self.warnings) == 1 else 's'}"
                         if self.warnings else "")


class Builder:
    """A mutable copy of an image, used by put."""

    def __init__(self, image):
        self.layout = image.layout
        self.data = bytearray(image.data)
        self.image = Image(self.data, image.layout, image.name)
        self.sb = self.image.sb
        self.nmeta = (self.sb["bmapstart"]
                      + self.sb["size"] // self.layout.bpb + 1)

    def bit(self, block):
        base = self.sb["bmapstart"] * self.layout.bsize + block // 8
        return (self.data[base] >> (block % 8)) & 1

    def set_bit(self, block, value=1):
        base = self.sb["bmapstart"] * self.layout.bsize + block // 8
        if block >= self.sb["size"]:
            raise Refused(f"block {block} is outside the image")
        if value:
            self.data[base] |= 1 << (block % 8)
        else:
            self.data[base] &= ~(1 << (block % 8)) & 0xFF

    def free_block(self):
        for block in range(self.nmeta, self.sb["size"]):
            if not self.bit(block):
                self.set_bit(block)
                return block
        raise Refused("no free blocks left in the image")

    def free_inode(self):
        for inum in range(1, self.sb["ninodes"]):
            if self.image.inode(inum)["type"] == 0:
                return inum
        raise Refused("no free inodes left in the image")

    def write_block(self, block, payload):
        start = block * self.layout.bsize
        if len(payload) > self.layout.bsize:
            raise Refused(f"block {block}: payload is too large")
        self.data[start:start + len(payload)] = payload

    def write_inode(self, inum, dinode):
        layout = self.layout
        block = self.sb["inodestart"] + inum // layout.ipb
        start = block * layout.bsize + (inum % layout.ipb) * layout.dinode_size
        raw = bytearray(self.data[start:start + layout.dinode_size])
        for name, field in layout.dinode["fields"].items():
            value = dinode[name]
            if field["count"] is not None:
                struct.pack_into("<%d%s" % (field["count"],
                                            _FORMAT[field["element"]]),
                                 raw, field["offset"], *value)
            else:
                struct.pack_into("<" + _FORMAT[field["size"]], raw,
                                 field["offset"], value)
        self.data[start:start + layout.dinode_size] = raw

    def ensure_block(self, dinode, logical):
        """Return the block holding a logical offset, allocating if needed."""
        layout = self.layout
        addrs = dinode["addrs"]
        if logical < layout.ndirect:
            if addrs[logical] == 0:
                addrs[logical] = self.free_block()
            return addrs[logical]
        index = logical - layout.ndirect
        if index >= layout.nindirect:
            raise Refused(
                f"file would need logical block {logical}, past MAXFILE")
        if addrs[layout.ndirect] == 0:
            addrs[layout.ndirect] = self.free_block()
            self.write_block(addrs[layout.ndirect], b"\0" * layout.bsize)
        entries = list(struct.unpack_from(
            "<%dI" % layout.nindirect,
            self.data[addrs[layout.ndirect] * layout.bsize:]))
        if entries[index] == 0:
            entries[index] = self.free_block()
            self.write_block(addrs[layout.ndirect],
                             struct.pack("<%dI" % layout.nindirect, *entries))
        return entries[index]

    def write_data(self, dinode, offset, payload):
        while payload:
            logical, within = divmod(offset, self.layout.bsize)
            block = self.ensure_block(dinode, logical)
            take = min(len(payload), self.layout.bsize - within)
            start = block * self.layout.bsize + within
            self.data[start:start + take] = payload[:take]
            payload = payload[take:]
            offset += take


def add_file(image_path, host_path, name=None, root=ROOT):
    """Append one regular file to the root directory of image_path."""
    layout = Layout(root)
    image = Image.open(image_path, layout)
    findings = image.fsck()
    if not findings.clean:
        details = "; ".join(item["message"] for item in findings.errors[:5])
        raise Refused(f"image is not clean, refusing to write: {details}")
    try:
        with open(host_path, "rb") as source:
            payload = source.read()
    except IsADirectoryError:
        raise Refused(f"{host_path}: is a directory, put only writes regular "
                      "files") from None
    except OSError as exc:
        raise Refused(f"{host_path}: {exc.strerror}") from None
    if name is None:
        name = os.path.basename(host_path)
    raw_name = name.encode("utf-8")
    if not raw_name:
        raise Refused("file name is empty")
    if len(raw_name) > layout.dirsiz:
        raise Refused(f"name {name!r} is {len(raw_name)} bytes, DIRSIZ is "
                      f"{layout.dirsiz}")
    if "/" in name or name in (".", ".."):
        raise Refused(f"name {name!r} is not a plain file name")
    if len(payload) > layout.maxfile * layout.bsize:
        raise Refused(
            f"{host_path}: {len(payload)} bytes exceeds MAXFILE*BSIZE "
            f"({layout.maxfile * layout.bsize})")
    builder = Builder(image)
    if image.lookup(layout.rootino, name):
        raise Refused(f"/{name} already exists in {image_path}")

    inum = builder.free_inode()
    dinode = {key: 0 for key in layout.dinode["fields"]}
    dinode["type"] = 2
    dinode["mode"] = 0o644
    dinode["nlink"] = 1
    dinode["size"] = len(payload)
    dinode["addrs"] = [0] * (layout.ndirect + 1)
    if payload:
        builder.write_data(dinode, 0, payload)
    builder.write_inode(inum, dinode)

    root = image.inode(layout.rootino)
    dirent_size = layout.dirent_size
    slot = None
    for entry in image.entries(root, layout.rootino):
        if entry["inum"] == 0:
            slot = entry["offset"]
            break
    grew = slot is None
    if grew:
        slot = root["size"]
    block = builder.ensure_block(root, slot // layout.bsize)
    record = struct.pack("<H", inum) + raw_name.ljust(
        layout.dirent["fields"]["name"]["size"], b"\0")
    start = block * layout.bsize + slot % layout.bsize
    builder.data[start:start + dirent_size] = record
    if grew:
        root["size"] += dirent_size
    builder.write_inode(layout.rootino, root)

    check = Image(builder.data, layout, image_path).fsck()
    if not check.clean:
        details = "; ".join(item["message"] for item in check.errors[:5])
        raise Refused(f"put would leave the image inconsistent: {details}")

    fd, temporary = tempfile.mkstemp(
        dir=os.path.dirname(os.path.abspath(image_path)) or ".",
        prefix=os.path.basename(image_path) + ".", suffix=".tmp")
    # mkstemp creates 0600; a replaced fs.img must keep the mode mktemp gave
    # it, or the next make/QEMU run sees a file only its owner can open.
    mode = os.stat(image_path).st_mode & 0o7777
    try:
        with os.fdopen(fd, "wb") as out:
            out.write(builder.data)
            out.flush()
            os.fsync(out.fileno())
            os.fchmod(out.fileno(), mode)
        os.replace(temporary, image_path)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise
    directory = os.open(os.path.dirname(os.path.abspath(image_path)) or ".",
                        os.O_RDONLY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    return {"name": "/" + name, "inode": inum, "size": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest()}


def describe(image, inum):
    dinode = image.inode(inum)
    return {
        "ino": inum,
        "type": {1: "directory", 2: "file", 3: "device"}.get(
            dinode["type"], f"unknown({dinode['type']})"),
        "mode": f"0o{dinode['mode']:04o}",
        "uid": dinode["uid"],
        "gid": dinode["gid"],
        "nlink": dinode["nlink"],
        "size": dinode["size"],
        "major": dinode["major"],
        "minor": dinode["minor"],
    }


def emit(args, payload, text):
    if args.json:
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        sys.stdout.write(text)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0], epilog=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=ROOT,
                        help="checkout holding kernel/ and mkfs/ (default: %(default)s)")
    parser.add_argument("--json", action="store_true",
                        help="stable machine-readable output")
    subparsers = parser.add_subparsers(dest="command", required=True)

    def sub(name, help_text):
        child = subparsers.add_parser(name, help=help_text)
        child.add_argument("--json", action="store_true",
                           default=argparse.SUPPRESS,
                           help=argparse.SUPPRESS)
        return child

    ls = sub("ls", "list a directory")
    ls.add_argument("image")
    ls.add_argument("paths", nargs="*", default=["/"])

    cat = sub("cat", "write a file to stdout")
    cat.add_argument("image")
    cat.add_argument("path")

    stat = sub("stat", "show one inode")
    stat.add_argument("image")
    stat.add_argument("path")

    fsck = sub("fsck", "check the image")
    fsck.add_argument("image")

    put = sub("put", "add a regular file to the image")
    put.add_argument("image")
    put.add_argument("host_file")
    put.add_argument("name", nargs="?", help="name inside the image (default: basename)")

    args = parser.parse_args(argv)
    try:
        layout = Layout(args.root)
        if args.command == "fsck":
            image = Image.open(args.image, layout)
            findings = image.fsck()
            payload = {"command": "fsck", "image": args.image,
                       "clean": findings.clean,
                       "errors": findings.errors,
                       "warnings": findings.warnings,
                       "blocks": image.sb["size"],
                       "inodes": image.sb["ninodes"]}
            lines = [f"{args.image}: {image.sb['size']} blocks, "
                     f"{image.sb['ninodes']} inodes"]
            for item in findings.errors:
                lines.append(f"ERROR {item['kind']}: {item['message']}")
            for item in findings.warnings:
                lines.append(f"WARN {item['kind']}: {item['message']}")
            lines.append("clean" if findings.clean else findings.summary())
            emit(args, payload, "\n".join(lines) + "\n")
            return 0 if findings.clean else 1
        if args.command == "put":
            result = add_file(args.image, args.host_file, args.name, args.root)
            emit(args, {"command": "put", "image": args.image, **result},
                 f"put {result['name']} ({result['size']} bytes, inode "
                 f"{result['inode']}) into {args.image}\n")
            return 0
        image = Image.open(args.image, layout)
        if args.command == "ls":
            listing = []
            for path in args.paths:
                inum = image.walk(path)
                dinode = image.inode(inum)
                if dinode["type"] != 1:
                    listing.append({"path": path, **describe(image, inum)})
                    continue
                for entry in image.entries(dinode, inum):
                    if entry["name"] in (".", "..") or entry["inum"] == 0:
                        continue
                    listing.append({"path": path.rstrip("/") + "/" + entry["name"],
                                    **describe(image, entry["inum"])})
            if args.json:
                emit(args, {"command": "ls", "image": args.image,
                            "entries": listing}, "")
            else:
                for item in listing:
                    kind = {"directory": "d", "file": "f",
                            "device": "c"}.get(item["type"], "?")
                    emit(args, None, f"{item['ino']:6d} {kind} "
                                     f"{item['size']:8d} {item['path']}\n")
            return 0
        inum = image.walk(args.path)
        if args.command == "stat":
            payload = {"command": "stat", "image": args.image,
                       "path": args.path, **describe(image, inum)}
            text = "".join(f"{key}: {value}\n" for key, value in payload.items()
                           if key not in ("command", "image"))
            emit(args, payload, text)
            return 0
        data = image.read_inode(inum)
        if args.json:
            emit(args, {"command": "cat", "image": args.image,
                        "path": args.path, "size": len(data),
                        "sha256": hashlib.sha256(data).hexdigest(),
                        "data": base64.b64encode(data).decode()}, "")
        else:
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
        return 0
    except LayoutError as exc:
        print(f"onyxfs: layout: {exc}", file=sys.stderr)
        return 2
    except (ImageError, Refused) as exc:
        print(f"onyxfs: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
