"""Tests of the Python bindings that need no model: redaction by copy, restore, archives.

    PUREBYTE_LIBRARY=build/purebyte.dll PUREBYTE_CLI=build/cli/purebyte.exe python -m unittest discover -s python/tests
    python -m unittest discover -s python/tests      # the installed package
"""
import base64
import gzip
import io
import json
import os
import tarfile
import unittest
import zipfile

from support import HERE

import purebyte

DATA = os.path.join(HERE, "data")
# The entity types of the PII specialists, in their model order: it breaks ties between overlapping spans.
PII_TYPES = ["EMAIL", "PHONE", "IBAN", "CREDIT_CARD", "IP", "DNI_NIE", "PASSPORT", "DATE_OF_BIRTH", "URL_CREDENTIALS",
             "SECRET", "PERSON_NAME", "ADDRESS"]


class Versions(unittest.TestCase):
    def test_versions_agree(self):
        self.assertEqual(purebyte.version(), purebyte.__version__)


class Redaction(unittest.TestCase):
    """redact_golden.jsonl: input + spans -> the exact output and report entries of the reference implementation."""

    def cases(self):
        with open(os.path.join(DATA, "redact_golden.jsonl"), encoding="utf-8") as fh:
            return [json.loads(line) for line in fh]

    def test_golden(self):
        cases = self.cases()
        self.assertEqual(len(cases), 17)
        for case in cases:
            with self.subTest(case["case"]):
                data = base64.b64decode(case["input_b64"])
                r = purebyte.redact_spans(data, [tuple(s) for s in case["spans"]], PII_TYPES, with_map=True)
                self.assertEqual(r.output, base64.b64decode(case["output_b64"]))
                self.assertEqual(r.report["spans"], case["entries"])
                self.assertEqual(purebyte.restore(r.output, r.map), data)

    def test_report_never_holds_values(self):
        data = b"mail ana@mail.example now"
        r = purebyte.redact_spans(data, [(5, 21, "EMAIL", 0.9)])
        self.assertIsNone(r.map)
        self.assertNotIn(b"ana@mail", json.dumps(r.report).encode())
        self.assertEqual(r.report["guarantee"], {"outside_spans_identical": True, "checked": True})

    def test_restore_refuses_an_edited_copy(self):
        r = purebyte.redact_spans(b"call 600 111 222 now", [(5, 16, "PHONE", 0.9)], with_map=True)
        for edited in (r.output.replace(b"[PHONE_1]", b"[PHONE_2]"), r.output.replace(b"now", b"NOW")):
            with self.assertRaises(purebyte.PureByteError) as e:
                purebyte.restore(edited, r.map)
            self.assertEqual(e.exception.status, "argument")

    def test_span_without_type_is_refused(self):
        with self.assertRaises(purebyte.PureByteError):
            purebyte.redact_spans(b"abc", [(0, 1, "")])


def zip_bytes(members, method=zipfile.ZIP_DEFLATED):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", method) as z:
        for name, data in members:
            z.writestr(name, data)
    return buffer.getvalue()


class Archives(unittest.TestCase):
    def test_nested_zip_gzip_tar(self):
        inner = zip_bytes([("config/app.env", b"TOKEN=abc\n"), ("lib/x.class", b"\xca\xfe\xba\xbe" + b"\0" * 40)])
        tar_buffer = io.BytesIO()
        with tarfile.open(fileobj=tar_buffer, mode="w", format=tarfile.PAX_FORMAT) as t:
            long_name = "deep/" + "d" * 120 + "/settings.py"
            info = tarfile.TarInfo(long_name)
            payload = b"PASSWORD = 'x'\n"
            info.size = len(payload)
            t.addfile(info, io.BytesIO(payload))
            info = tarfile.TarInfo("inner.zip")
            info.size = len(inner)
            t.addfile(info, io.BytesIO(inner))
        outer = gzip.compress(tar_buffer.getvalue())
        members, failures = purebyte.expand_archive(outer, "bundle.tar.gz")
        names = [n for n, _ in members]
        self.assertEqual(failures, [])
        self.assertIn(f"bundle.tar.gz!/bundle.tar!/{long_name}", names)
        self.assertIn("bundle.tar.gz!/bundle.tar!/inner.zip!/config/app.env", names)
        self.assertEqual(dict(members)["bundle.tar.gz!/bundle.tar!/inner.zip!/config/app.env"], b"TOKEN=abc\n")

    def test_concatenated_gzip_members_are_one_stream(self):
        data = gzip.compress(b"first part\n") + gzip.compress(b"second part\n")
        members, failures = purebyte.expand_archive(data, "log.gz")
        self.assertEqual(failures, [])
        self.assertEqual(members, [("log.gz!/log", b"first part\nsecond part\n")])

    def test_bomb_is_refused(self):
        bomb = zip_bytes([("zeros.bin", b"\0" * (80 << 20))])  # 80 MiB of zeros in about 80 KB
        self.assertLess(len(bomb), 1 << 20)
        members, failures = purebyte.expand_archive(bomb, "bomb.zip")
        self.assertEqual(members, [])
        self.assertEqual(len(failures), 1)
        self.assertIn("archive bomb", failures[0])

    def test_depth_limit(self):
        data = b"KEY=value\n"
        name = "leaf.txt"
        for level in range(5):
            data, name = zip_bytes([(name, data)]), f"level{level}.zip"
        members, failures = purebyte.expand_archive(data, "top.zip", max_depth=3)
        self.assertEqual(len(members), 1)  # the archive at depth 4, kept as it is
        self.assertTrue(any("nested deeper than the limit" in p for p in failures))

    def test_hostile_names_are_only_labels(self):
        data = zip_bytes([("../../escape.txt", b"x" * 30), ("/abs/path.txt", b"y" * 30)])
        members, failures = purebyte.expand_archive(data, "evil.zip")
        self.assertEqual([n for n, _ in members], ["evil.zip!/../../escape.txt", "evil.zip!//abs/path.txt"])
        self.assertFalse(os.path.exists("escape.txt"))

    def test_corrupt_archives_report_failures(self):
        good = zip_bytes([("a.txt", b"hello " * 100)])
        members, failures = purebyte.expand_archive(good[: len(good) // 2], "cut.zip")
        self.assertEqual(members, [])
        self.assertEqual(len(failures), 1)
        members, failures = purebyte.expand_archive(b"\x1f\x8b\x08\x00" + b"\0" * 6 + b"\xff" * 20, "bad.gz")
        self.assertEqual(members, [])
        self.assertIn("corrupt", failures[0])

    def test_not_an_archive(self):
        self.assertEqual(purebyte.expand_archive(b"plain text, not an archive", "a.txt"), ([], []))


if __name__ == "__main__":
    unittest.main()
