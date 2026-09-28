"""Detection through the bindings with tiny random models: profiles, input policy, archives, masking, ensembles."""
import hashlib
import io
import json
import os
import pathlib
import shutil
import unittest
import zipfile
from unittest import mock

from support import MARK_ALL, MARK_NOTHING, TEXT, TempDir, random_models, write_model

import purebyte


@unittest.skipIf(random_models is None, "the model generator (tests/gen) is not available")
class Detection(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = TempDir()
        cls.path = write_model(cls.tmp)
        cls.model = purebyte.Model(cls.path)
        cls.session = purebyte.Session(2)

    @classmethod
    def tearDownClass(cls):
        cls.session.close()
        cls.model.close()
        cls.tmp.cleanup()

    def test_profiles_declare_what_they_read(self):
        code = purebyte.Detector(self.model, "secrets-code")
        self.assertEqual(code.profile, "secrets-code")
        self.assertTrue(code.wants_file("app.py") and code.wants_file("Dockerfile.prod") and code.wants_file(".env"))
        self.assertFalse(code.wants_file("photo.png") or code.wants_file("lib.so"))
        self.assertTrue(code.wants_directory("src"))
        self.assertFalse(code.wants_directory("node_modules") or code.wants_directory(".git"))
        self.assertEqual(code.max_input_bytes, 4000000)
        plain = purebyte.Detector(self.model)  # the model declares no profile
        self.assertEqual(plain.profile, "none")
        self.assertTrue(plain.wants_file("photo.png") and plain.wants_directory("node_modules"))
        self.assertFalse(plain.wants_directory(".git"))
        self.assertEqual(plain.max_input_bytes, 64 << 20)

    def test_unknown_profile(self):
        with self.assertRaises(purebyte.PureByteError) as e:
            purebyte.Detector(self.model, "no-such-profile")
        self.assertEqual(e.exception.status, "unsupported")
        self.assertIn("secrets-code", str(e.exception))

    def test_values_are_masked_unless_revealed(self):
        detector = purebyte.Detector(self.model)
        masked = detector.detect([("a.cfg", TEXT)], self.session, bias=MARK_ALL)
        self.assertTrue(masked.findings)
        for f in masked.findings:
            self.assertIsNone(f.snippet)
            self.assertIsNotNone(f.snippet_masked)
        revealed = detector.detect([("a.cfg", TEXT)], self.session, bias=MARK_ALL, reveal=True)
        for f in revealed.findings:
            self.assertEqual(f.snippet.encode(), TEXT[f.start:f.end])
        self.assertFalse(detector.detect([("a.cfg", TEXT)], self.session, bias=MARK_NOTHING).findings)

    def test_binary_findings_touch_a_printable_string_by_default(self):
        detector = purebyte.Detector(self.model, "secrets-binary")
        no_string = bytes(128 + (i * 37) % 128 for i in range(300))  # no printable string at all
        self.assertFalse(detector.detect([("a.bin", no_string)], self.session, bias=MARK_ALL).findings)
        all_bytes = detector.detect([("a.bin", no_string)], self.session, bias=MARK_ALL, strings_only=False)
        self.assertTrue(all_bytes.findings)

    def test_results_follow_inputs_and_archive_members(self):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("src/settings.py", TEXT)
            z.writestr("lib/Main.class", b"\xca\xfe\xba\xbe" + bytes(range(256)))
        detector = purebyte.Detector(self.model, "secrets-code")
        r = detector.detect([("a.cfg", TEXT), ("b.jar", buffer.getvalue())], self.session)
        self.assertEqual([(x["file"], x["input"]) for x in r.results], [("a.cfg", 0), ("b.jar!/src/settings.py", 1)])
        r = detector.detect([("b.jar", buffer.getvalue())], self.session, archives=False)
        self.assertEqual([x["file"] for x in r.results], ["b.jar"])

    def test_inputs_over_the_limit_and_short_inputs(self):
        detector = purebyte.Detector(self.model, "secrets-code")
        r = detector.detect([("big.txt", b"a" * 4000001), ("tiny.txt", b"x = 1")], self.session)
        self.assertEqual(r.failures, [{"file": "big.txt",
                                       "reason": "4000001 bytes, over this model's limit of 4000000 bytes: not scanned"}])
        self.assertEqual(len(r.results), 1)
        self.assertEqual(r.results[0]["windows"], 0)
        self.assertIn("note", r.results[0])

    def test_scan_walks_like_the_cli(self):
        tree = TempDir()
        try:
            tree.file("src/app.py", TEXT)
            tree.file("src/node_modules/dep.js", TEXT)
            tree.file("src/image.png", TEXT)
            tree.file(".git/config", TEXT)
            tree.file("empty.txt", b"")
            r = purebyte.scan(tree.path, self.path, profile="secrets-code", threads=2)
            self.assertEqual([x["file"] for x in r.results], [tree.path.replace(os.sep, "/") + "/src/app.py"])
            self.assertEqual(r.failures, [])
        finally:
            tree.cleanup()

    def test_severity_by_path(self):
        code = purebyte.Detector(self.model, "secrets-code")
        self.assertEqual(code.path_severity("tests/fixtures/app.cfg"), "warning")
        self.assertEqual(code.path_severity("src\\test\\Keys.java"), "warning")
        self.assertEqual(code.path_severity("docs/README.md"), "warning")
        self.assertEqual(code.path_severity("src/app.cfg"), "error")
        self.assertEqual(purebyte.Detector(self.model).path_severity("tests/app.cfg"), "error")  # profile none
        # The second input's folders above the project hold a `test`: its path inside the project is what counts.
        inputs = [("src/a.cfg", TEXT), ("/home/me/test/repo/src/a.cfg", TEXT), ("tests/a.cfg", TEXT)]
        r = code.detect(inputs, self.session, bias=MARK_ALL, paths=[None, "src/a.cfg", None])
        seen = {}
        for f in r.findings:
            seen.setdefault(f.file, set()).add(f.severity)
        self.assertEqual(seen, {"src/a.cfg": {"error"}, "/home/me/test/repo/src/a.cfg": {"error"},
                                "tests/a.cfg": {"warning"}})
        warnings = sum(f.severity == "warning" for f in r.findings)
        self.assertEqual(r.by_severity, {"error": len(r.findings) - warnings, "warning": warnings})
        self.assertIn(f"{warnings} warning(s)", repr(r))
        with self.assertRaises(ValueError):
            code.detect(inputs, self.session, paths=["src/a.cfg"])

    def test_scan_judges_paths_inside_the_folder_named(self):
        tree = TempDir()
        try:  # a project inside a folder named `test`: that folder is above the project and does not count
            tree.file("test/project/src/app.cfg", TEXT)
            tree.file("test/project/tests/app.cfg", TEXT)
            project = os.path.join(tree.path, "test", "project")
            r = purebyte.scan(project, self.path, profile="secrets-code", bias=MARK_ALL, session=self.session)
            seen = {}
            for f in r.findings:
                seen.setdefault(f.file[len(project) + 1:], set()).add(f.severity)
            self.assertEqual(seen, {"src/app.cfg": {"error"}, "tests/app.cfg": {"warning"}})
            r = purebyte.scan(os.path.join(project, "tests", "app.cfg"), self.path, profile="secrets-code",
                              bias=MARK_ALL, session=self.session)  # a file named by an absolute path: its name
            self.assertEqual({f.severity for f in r.findings}, {"error"})
        finally:
            tree.cleanup()

    def test_ensemble_vote(self):
        detector = purebyte.Detector([self.model, self.model, self.model], "secrets-code")
        r = detector.detect([("a.cfg", TEXT)], self.session, bias=MARK_ALL, per_model=True)
        self.assertTrue(r.findings)
        self.assertTrue(all(f.votes == 3 for f in r.findings))
        self.assertEqual(len(r.results[0]["per_model"]), 3)
        with self.assertRaises(purebyte.PureByteError):
            detector.detect([("a.cfg", TEXT)], self.session, votes=4)

    def test_windows_without_head_outputs(self):
        """Windows that the exit head stops or the prefilter skips have no head outputs: the summaries skip them."""
        tree = TempDir()
        try:
            model = purebyte.Model(write_model(tree, "ssm-causal-ternary"))  # exit head at layer 2
            detector = purebyte.Detector(model)
            data = bytes((i * 131 + 7) % 256 for i in range(2000)) + TEXT
            r = detector.detect([("mixed.bin", data)], self.session, early_exit=True, prefilter=True)
            result = r.results[0]
            self.assertGreater(result.get("windows_exited", 0) + result.get("windows_skipped", 0), 0)
            self.assertIn("choice", result["decisions"])
            model.close()
        finally:
            tree.cleanup()

    def test_installed_specialist_by_name(self):
        """Names resolve as the CLI resolves them: PUREBYTE_HOME, the catalog's files, SHA-256, profile hint."""
        home = TempDir()
        try:
            installed = os.path.join(home.path, "purebyte-demo-1.0.0.gguf")
            shutil.copyfile(self.path, installed)
            with open(self.path, "rb") as fh:
                sha = hashlib.sha256(fh.read()).hexdigest()
            entry = {"name": "demo", "version": "1.0.0", "profile": "secrets-code", "files": [
                {"role": "model", "file": "purebyte-demo-1.0.0.gguf", "url": "TBD", "sha256": sha, "size": None}]}
            catalog = home.file("catalog.json", json.dumps({"schema": 1, "models": [entry]}).encode())
            with mock.patch.dict(os.environ, {"PUREBYTE_HOME": home.path, "PUREBYTE_CATALOG": catalog}):
                self.assertEqual(purebyte.models_directory(), home.path)
                self.assertEqual(purebyte.load_detector("demo@1.0.0").profile, "secrets-code")  # the file declares none
                # Errors are PureByteError, and the built-in exception that describes them.
                with self.assertRaises(LookupError) as unknown:
                    purebyte.load_detector("no-such-model")
                self.assertIsInstance(unknown.exception, purebyte.PureByteError)
                self.assertEqual(unknown.exception.status, "argument")
                with open(installed, "ab") as fh:
                    fh.write(b"tampered")
                with self.assertRaises(ValueError) as tampered:
                    purebyte.load_detector("demo")
                self.assertIsInstance(tampered.exception, purebyte.PureByteError)
                os.remove(installed)
                with self.assertRaises(FileNotFoundError) as missing:
                    purebyte.load_detector("demo")
                self.assertIsInstance(missing.exception, purebyte.PureByteError)
                self.assertEqual(missing.exception.status, "io")
        finally:
            home.cleanup()

    def catalog_home(self, entries):
        """A models directory holding the test model as purebyte-demo-1.0.0.gguf, and a catalog of `entries`."""
        home = TempDir()
        shutil.copyfile(self.path, os.path.join(home.path, "purebyte-demo-1.0.0.gguf"))
        catalog = home.file("catalog.json", json.dumps({"schema": 1, "models": entries}).encode())
        return home, {"PUREBYTE_HOME": home.path, "PUREBYTE_CATALOG": catalog}

    def entry(self, **changes):
        with open(self.path, "rb") as fh:
            sha = hashlib.sha256(fh.read()).hexdigest()
        entry = {"name": "demo", "version": "1.0.0", "files": [
            {"role": "model", "file": "purebyte-demo-1.0.0.gguf", "url": "TBD", "sha256": sha, "size": None}]}
        entry.update(changes)
        return entry

    def test_a_name_is_never_a_file_of_the_current_folder(self):
        """As the CLI's --model: `demo` is the installed specialist even when the current folder (a repository being
        scanned) holds a file named `demo`; `./demo` is that file."""
        home, env = self.catalog_home([self.entry()])
        planted = TempDir()
        cwd = os.getcwd()
        try:
            shutil.copyfile(write_model(planted, "legacy-v2"), os.path.join(planted.path, "demo"))
            os.chdir(planted.path)
            with mock.patch.dict(os.environ, env):
                self.assertEqual(purebyte.load_detector("demo").models[0].path,
                                 os.path.join(home.path, "purebyte-demo-1.0.0.gguf"))
                self.assertEqual(purebyte.load_detector("./demo").models[0].path, "./demo")
                self.assertEqual(purebyte.load_detector(pathlib.Path("demo")).models[0].path, "demo")  # a path object
        finally:
            os.chdir(cwd)
            home.cleanup()
            planted.cleanup()

    def test_catalog_file_names_must_be_plain(self):
        for bad in ("../escaped.gguf", "/abs/escaped.gguf", "sub\\escaped.gguf", "C:escaped.gguf", "escaped.bin"):
            entry = self.entry()
            entry["files"][0]["file"] = bad
            home, env = self.catalog_home([entry])
            try:
                with self.subTest(bad), mock.patch.dict(os.environ, env):
                    with self.assertRaises(purebyte.PureByteError) as refused:
                        purebyte.load_detector("demo")
                    self.assertIsInstance(refused.exception, ValueError)
                    self.assertIn("plain name", str(refused.exception))
            finally:
                home.cleanup()

    def test_min_runtime(self):
        """A specialist that needs a newer runtime is refused; without a version, the highest one this runtime can
        run is chosen."""
        home, env = self.catalog_home([self.entry(version="9.0.0", min_runtime="99.0.0")])
        try:
            with mock.patch.dict(os.environ, env):
                with self.assertRaises(purebyte.PureByteError) as refused:
                    purebyte.load_detector("demo")
                self.assertEqual(refused.exception.status, "unsupported")
        finally:
            home.cleanup()
        home, env = self.catalog_home([self.entry(), self.entry(version="2.0.0", min_runtime="99.0.0")])
        try:
            with mock.patch.dict(os.environ, env):
                self.assertEqual(len(purebyte.load_detector("demo").models), 1)
        finally:
            home.cleanup()

    def test_no_models_directory(self):
        bare = {k: v for k, v in os.environ.items()
                if k.upper() not in ("PUREBYTE_HOME", "HOME", "XDG_DATA_HOME", "LOCALAPPDATA")}
        with mock.patch.dict(os.environ, bare, clear=True):
            with self.assertRaises(purebyte.PureByteError) as refused:
                purebyte.models_directory()
            self.assertIn("PUREBYTE_HOME", str(refused.exception))

    def test_scan_leaves_out_what_the_cli_leaves_out(self):
        """.purebyteignore and `exclude` (the CLI's --exclude), and links: a symbolic link, or a junction on Windows,
        pointing to the folder above would make the walk endless."""
        tree = TempDir()
        try:
            for name in ("app.cfg", "generated/out.cfg", "notes.min.cfg", "deep/a/fixtures/x.cfg"):
                tree.file(name, TEXT)
            tree.file(".purebyteignore", b"# generated code\ngenerated/\n**/fixtures/**\n")
            linked = os.path.join(tree.path, "deep", "back")
            if os.name == "nt":
                try:
                    import _winapi
                    _winapi.CreateJunction(tree.path, linked)
                except (ImportError, AttributeError, OSError):
                    pass
            else:
                os.symlink(tree.path, linked, target_is_directory=True)
            r = purebyte.scan(tree.path, self.path, profile="none", exclude=["*.min.cfg"], session=self.session)
            top = tree.path.replace(os.sep, "/")
            self.assertEqual([x["file"] for x in r.results], [top + "/.purebyteignore", top + "/app.cfg"])
            self.assertEqual(r.failures, [])
            with self.assertRaises(purebyte.PureByteError):
                purebyte.scan(tree.path, self.path, exclude=["!app.cfg"], session=self.session)
        finally:
            tree.cleanup()

    def test_scan_works_in_batches_and_retries_one_by_one(self):
        """Inputs go to the library in batches (merged into one result, `input` counting across them); when the
        library fails on a batch, its inputs are retried one by one and one that fails alone becomes a failure."""
        tree = TempDir()
        try:
            for name in ("a.cfg", "b.cfg", "c.cfg"):
                tree.file(name, TEXT)
            original = purebyte.Detector.detect

            def failing(detector, inputs, *args, **kwargs):
                if any(name.endswith("b.cfg") for name, _ in inputs):
                    raise purebyte.PureByteError("internal", "the scan failed: a test failure")
                return original(detector, inputs, *args, **kwargs)

            top = tree.path.replace(os.sep, "/")
            with mock.patch.object(purebyte._api, "BATCH_BYTES", 2 * len(TEXT)):  # two files per batch
                r = purebyte.scan(tree.path, self.path, profile="none", bias=MARK_ALL, session=self.session)
                self.assertEqual([(x["file"], x["input"]) for x in r.results],
                                 [(top + "/a.cfg", 0), (top + "/b.cfg", 1), (top + "/c.cfg", 2)])
                with mock.patch.object(purebyte.Detector, "detect", failing):
                    r = purebyte.scan(tree.path, self.path, profile="none", bias=MARK_ALL, session=self.session)
            self.assertEqual([x["file"] for x in r.results], [top + "/a.cfg", top + "/c.cfg"])
            self.assertEqual(r.failures, [{"file": top + "/b.cfg", "reason": "the scan failed: a test failure"}])
            self.assertTrue(r.findings)
        finally:
            tree.cleanup()

    def test_redact_refuses_an_input_over_the_limit(self):
        detector = purebyte.Detector(self.model, "secrets-code")
        with self.assertRaises(ValueError) as refused:
            detector.redact(b"a" * 4000001, self.session)
        self.assertIsInstance(refused.exception, purebyte.PureByteError)

    def test_decide(self):
        self.assertEqual(purebyte.decide(TEXT[:200], self.path, bias=MARK_ALL)["decision"], "positive")
        self.assertEqual(purebyte.decide(TEXT[:200], self.path, bias=MARK_NOTHING)["decision"], "negative")
        with self.assertRaises(ValueError):
            purebyte.decide(b"x" * 5000, self.path)

    def test_redact_with_a_model(self):
        detector = purebyte.Detector(self.model, "redact")
        r = detector.redact(TEXT, self.session, bias=MARK_ALL, with_map=True)
        self.assertNotEqual(r.output, TEXT)
        self.assertTrue(r.report["spans"])
        self.assertEqual(purebyte.restore(r.output, r.map), TEXT)
        short = detector.redact(b"x = 1", self.session, bias=MARK_ALL)  # shorter than any window: still redacted
        self.assertNotEqual(short.output, b"x = 1")


if __name__ == "__main__":
    unittest.main()
