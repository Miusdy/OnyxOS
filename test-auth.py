#!/usr/bin/env python3
"""Independent reference check for the exact PBKDF2 code linked into login."""
import ctypes
import hashlib
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent

class HashTests(unittest.TestCase):
    def test_pbkdf2_matches_hashlib(self):
        with tempfile.TemporaryDirectory() as tmp:
            lib = pathlib.Path(tmp) / "auth.so"
            subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-I", str(ROOT),
                            str(ROOT / "common/auth.c"), "-o", str(lib)], check=True)
            auth = ctypes.CDLL(str(lib))
            auth.auth_hash.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p]
            auth.auth_equal.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
            for password in (b"", b"root", b"long password with spaces", b"a" * 55,
                             b"b" * 56, b"c" * 63):
                for salt in (bytes(16), bytes(range(16)), bytes([255]) * 16):
                    with self.subTest(password=password, salt=salt):
                        out = ctypes.create_string_buffer(32)
                        auth.auth_hash(password, salt, out)
                        expected = hashlib.pbkdf2_hmac("sha256", password, salt, 10000)
                        self.assertEqual(out.raw, expected)
                        self.assertEqual(auth.auth_equal(out.raw, expected), 1)
                        wrong = bytes([expected[0] ^ 1]) + expected[1:]
                        self.assertEqual(auth.auth_equal(out.raw, wrong), 0)

if __name__ == "__main__":
    unittest.main()
