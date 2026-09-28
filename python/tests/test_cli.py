"""End-to-end tests of the `purebyte` command (the native CLI the package ships) with tiny random models."""
import http.client
import json
import os
import shutil
import socket
import subprocess
import time
import unittest

from support import CLI, HAVE_CLI, MARK_ALL, MARK_NOTHING, TEXT, TempDir, random_models, write_model


def run(*args, stdin=None, env=None, cwd=None):
    return subprocess.run([CLI] + [str(a) for a in args], input=stdin, capture_output=True, timeout=300,
                          env=dict(os.environ, **(env or {})), cwd=cwd)


@unittest.skipUnless(HAVE_CLI and random_models, "needs the purebyte CLI (PUREBYTE_CLI) and tests/gen")
class Scan(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = TempDir()
        cls.model = write_model(cls.tmp)
        cls.a = cls.tmp.file("tree/a.cfg", TEXT)
        cls.b = cls.tmp.file("tree/sub/b.cfg", TEXT[::-1])

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def scan(self, *args, **kw):
        return run("scan", "--model", self.model, "--threads", 2, *args, **kw)

    def test_version(self):
        r = run("version")
        self.assertEqual(r.returncode, 0)
        self.assertRegex(r.stdout.decode(), r"^purebyte \d+\.\d+\.\d+\s*$")

    def test_exit_status(self):
        self.assertEqual(self.scan("--bias", MARK_ALL, self.a).returncode, 1)
        self.assertEqual(self.scan("--bias", MARK_NOTHING, self.a).returncode, 0)
        missing = self.scan("--bias", MARK_NOTHING, self.a, os.path.join(self.tmp.path, "missing.cfg"))
        self.assertEqual(missing.returncode, 2)
        doc = json.loads(missing.stdout)
        self.assertEqual([f["reason"] for f in doc["failures"]], ["does not exist"])
        self.assertEqual(run("scan", "--no-such-option").returncode, 2)

    def test_json_document(self):
        r = self.scan("--bias", MARK_ALL, os.path.join(self.tmp.path, "tree"))
        doc = json.loads(r.stdout)
        files = sorted(x["file"] for x in doc["results"])
        self.assertEqual(files, sorted([self.tree("a.cfg"), self.tree("sub/b.cfg")]))
        self.assertEqual(doc["stats"]["findings"], len(doc["findings"]))
        for f in doc["findings"]:
            for key in ("file", "kind", "start", "end", "line", "col", "confidence", "votes", "snippet_masked"):
                self.assertIn(key, f)
            self.assertNotIn("snippet", f)

    def tree(self, relative):  # reports write paths with "/" on every platform (spec/OUTPUT.md, section 1)
        return (os.path.join(self.tmp.path, "tree") + "/" + relative).replace(os.sep, "/")

    def test_paths_use_forward_slashes_and_sizes_are_exact(self):
        big = self.tmp.file("big/large.cfg", b"a" * 4000001)
        missing = os.path.join(self.tmp.path, "no", "such.cfg")
        r = self.scan("--profile", "secrets-code", "--bias", MARK_ALL, os.path.join(self.tmp.path, "tree"), big,
                      missing)
        self.assertEqual(r.returncode, 2)
        doc = json.loads(r.stdout)
        self.assertEqual(doc["failures"], [
            {"file": missing.replace(os.sep, "/"), "reason": "does not exist"},
            {"file": big.replace(os.sep, "/"),
             "reason": "4000001 bytes, over this model's limit of 4000000 bytes: not scanned"}])
        names = [x["file"] for x in doc["results"] + doc["findings"]] + [m["path"] for m in doc["model"]["files"]]
        self.assertTrue(doc["findings"])
        self.assertEqual([n for n in names if "\\" in n], [])

    def test_jsonl_has_one_line_per_input(self):
        r = self.scan("--format", "jsonl", "--bias", MARK_ALL, self.a, self.b, os.path.join(self.tmp.path, "nope.cfg"))
        lines = [json.loads(line) for line in r.stdout.decode().splitlines()]
        self.assertEqual(len(lines), 3)
        self.assertEqual(sum("not_scanned" in x for x in lines), 1)
        self.assertTrue(all("findings" in x for x in lines if "not_scanned" not in x))

    def test_sarif(self):
        r = self.scan("--format", "sarif", "--bias", MARK_ALL, self.a)
        sarif = json.loads(r.stdout)
        self.assertEqual(sarif["version"], "2.1.0")
        run_ = sarif["runs"][0]
        self.assertTrue(run_["results"])
        self.assertTrue(run_["invocations"][0]["executionSuccessful"])
        region = run_["results"][0]["locations"][0]["physicalLocation"]["region"]
        self.assertGreaterEqual(region["startLine"], 1)
        self.assertGreaterEqual(region["startColumn"], 1)

    def test_standard_input_and_out_file(self):
        out = os.path.join(self.tmp.path, "report.json")
        r = self.scan("--bias", MARK_ALL, "--out", out, "-", stdin=TEXT)
        self.assertEqual(r.returncode, 1)
        self.assertEqual(r.stdout, b"")
        with open(out, encoding="utf-8") as fh:
            self.assertEqual(json.load(fh)["results"][0]["file"], "<stdin>")

    def test_values_appear_only_when_revealed(self):
        secret = "".join(chr(65 + 7 * i % 26) + str(i % 10) for i in range(16)).encode()  # a look-alike, made here
        text = b"config value here and more padding text " + secret + b" end of the line padding\n" * 3
        masked = self.scan("--bias", MARK_ALL, "-", stdin=text)
        self.assertNotIn(secret, masked.stdout)
        revealed = self.scan("--bias", MARK_ALL, "--reveal", "-", stdin=text)
        self.assertIn(secret.decode(), json.dumps(json.loads(revealed.stdout)))

    def test_staged_changes(self):
        if not shutil.which("git"):
            self.skipTest("git is not installed")
        repo = TempDir()
        try:
            git = ["git", "-C", repo.path, "-c", "user.email=t@example.invalid", "-c", "user.name=t",
                   "-c", "core.autocrlf=false", "-c", "commit.gpgsign=false"]
            subprocess.run(git + ["init", "-q"], check=True)
            repo.file("app.cfg", TEXT)
            subprocess.run(git + ["add", "app.cfg"], check=True)
            subprocess.run(git + ["commit", "-q", "-m", "first"], check=True)
            repo.file("app.cfg", TEXT + b"added = one more line of text here\n")
            subprocess.run(git + ["add", "app.cfg"], check=True)
            r = run("scan", "--model", self.model, "--threads", 2, "--bias", MARK_ALL, "--staged", repo.path)
            doc = json.loads(r.stdout)
            added_line = TEXT.count(b"\n") + 1
            self.assertTrue(doc["findings"])
            self.assertTrue(all(f["line"] <= added_line <= f["end_line"] for f in doc["findings"]))
            self.assertEqual(doc["results"][0]["file"], "app.cfg")
        finally:
            repo.cleanup()


def relative(name):
    return name[2:] if name.startswith("./") else name


def severities(findings, key=relative):
    """{file: {severity, ...}} of a list of findings."""
    out = {}
    for f in findings:
        out.setdefault(key(f["file"]), set()).add(f["severity"])
    return out


@unittest.skipUnless(HAVE_CLI and random_models, "needs the purebyte CLI (PUREBYTE_CLI) and tests/gen")
class Severity(unittest.TestCase):
    """secrets-code: findings in test, example and documentation paths are warnings; only errors fail a scan."""

    WARNINGS = {"tests/fixtures/app.cfg", "docs/guide.md", "pkg/handler_test.go"}

    @classmethod
    def setUpClass(cls):
        cls.tmp = TempDir()
        cls.model = write_model(cls.tmp)
        # The project sits inside a folder named `test`, which is not part of it.
        cls.project = os.path.join(cls.tmp.path, "test", "project")
        for name in ["src/app.cfg"] + sorted(cls.WARNINGS):
            cls.tmp.file("test/project/" + name, TEXT)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def scan(self, *args, cwd=None, bias=MARK_ALL):
        return run("scan", "--model", self.model, "--profile", "secrets-code", "--bias", bias, "--threads", 2, *args,
                   cwd=cwd or self.project)

    def expected(self, names):
        return {n: {"warning" if n in self.WARNINGS else "error"} for n in names}

    def test_severity_by_path_and_counts(self):
        r = self.scan(".")
        self.assertEqual(r.returncode, 1, r.stderr)  # src/app.cfg has errors
        doc = json.loads(r.stdout)
        seen = severities(doc["findings"])
        self.assertEqual(seen, self.expected(["src/app.cfg"] + sorted(self.WARNINGS)))
        warnings = sum(f["severity"] == "warning" for f in doc["findings"])
        self.assertEqual(doc["stats"]["by_severity"], {"error": len(doc["findings"]) - warnings, "warning": warnings})
        self.assertIn(b"(%d error(s), %d warning(s))" % (len(doc["findings"]) - warnings, warnings), r.stderr)

    def test_warnings_alone_exit_0_unless_strict(self):
        only = ["tests", "docs", "pkg/handler_test.go"]
        r = self.scan(*only)
        doc = json.loads(r.stdout)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(doc["findings"])
        self.assertEqual(doc["stats"]["by_severity"]["error"], 0)
        self.assertIn(b"--strict", r.stderr)
        self.assertEqual(self.scan("--strict", *only).returncode, 1)
        self.assertEqual(self.scan("--strict", *only, bias=MARK_NOTHING).returncode, 0)

    def test_tests_no_skips_those_paths(self):
        r = self.scan("--tests=no", ".")
        doc = json.loads(r.stdout)
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertEqual([relative(x["file"]) for x in doc["results"]], ["src/app.cfg"])
        self.assertEqual(doc["stats"]["by_severity"]["warning"], 0)
        r = self.scan("--tests", "no", "tests", "docs")
        self.assertEqual((r.returncode, json.loads(r.stdout)["stats"]["files"]), (0, 0))
        # A file named explicitly is still scanned, but its warnings are left out of the report.
        doc = json.loads(self.scan("--tests=no", "pkg/handler_test.go").stdout)
        self.assertEqual((doc["stats"]["files"], doc["findings"]), (1, []))
        self.assertEqual(self.scan("--tests=maybe", ".").returncode, 2)

    def test_folders_above_the_project_do_not_count(self):
        prefix = self.project.replace(os.sep, "/") + "/"
        r = self.scan(self.project, cwd=self.tmp.path)  # an absolute path: only what is inside it counts
        seen = severities(json.loads(r.stdout)["findings"], key=lambda name: name[len(prefix):])
        self.assertEqual(seen, self.expected(["src/app.cfg"] + sorted(self.WARNINGS)))
        self.assertEqual(r.returncode, 1)
        # A path that climbs with `..` counts from the folder it names; a file named that way, by its name.
        docs = os.path.join(self.project, "docs")
        r = self.scan(os.path.join(os.pardir, "src"), os.path.join(os.pardir, "tests", "fixtures", "app.cfg"), cwd=docs)
        self.assertEqual({f["severity"] for f in json.loads(r.stdout)["findings"]}, {"error"})
        # A relative path counts as it is written: here `test/` is part of it.
        r = self.scan(os.path.join("test", "project", "src"), cwd=self.tmp.path)
        self.assertEqual({f["severity"] for f in json.loads(r.stdout)["findings"]}, {"warning"})
        self.assertEqual(r.returncode, 0)

    def test_sarif_levels_follow_the_severity(self):
        r = self.scan("--format", "sarif", ".")
        self.assertEqual(r.returncode, 1)
        run_ = json.loads(r.stdout)["runs"][0]
        levels = {}
        for result in run_["results"]:
            uri = result["locations"][0]["physicalLocation"]["artifactLocation"]["uri"]
            levels.setdefault(uri, set()).add(result["level"])
        self.assertEqual(levels, self.expected(["src/app.cfg"] + sorted(self.WARNINGS)))
        self.assertEqual({rule["defaultConfiguration"]["level"] for rule in run_["tool"]["driver"]["rules"]}, {"error"})
        warning = next(x for x in run_["results"] if x["level"] == "warning")
        self.assertTrue(warning["message"]["text"].endswith("(in a test, example or documentation path)"))
        r = self.scan("--format", "sarif", "docs")
        self.assertEqual(r.returncode, 0)
        self.assertEqual({x["level"] for x in json.loads(r.stdout)["runs"][0]["results"]}, {"warning"})

    def test_jsonl_findings_carry_the_severity(self):
        r = self.scan("--format", "jsonl", ".")
        self.assertEqual(r.returncode, 1)
        lines = [json.loads(line) for line in r.stdout.decode().splitlines()]
        seen = {}
        for line in lines:
            seen.update(severities(line["findings"]))
        self.assertEqual(seen, self.expected(["src/app.cfg"] + sorted(self.WARNINGS)))

    def test_staged_test_files_do_not_block_a_commit(self):
        if not shutil.which("git"):
            self.skipTest("git is not installed")
        repo = TempDir()
        try:
            git = ["git", "-C", repo.path, "-c", "core.autocrlf=false"]
            subprocess.run(git + ["init", "-q"], check=True)
            repo.file("tests/app.cfg", TEXT)
            subprocess.run(git + ["add", "tests/app.cfg"], check=True)
            r = self.scan("--staged", repo.path)
            doc = json.loads(r.stdout)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(severities(doc["findings"]), {"tests/app.cfg": {"warning"}})
            self.assertEqual(self.scan("--strict", "--staged", repo.path).returncode, 1)
            r = self.scan("--tests=no", "--staged", repo.path)
            self.assertEqual((r.returncode, json.loads(r.stdout)["stats"]["files"]), (0, 0))
            repo.file("src/app.cfg", TEXT)
            subprocess.run(git + ["add", "src/app.cfg"], check=True)
            self.assertEqual(self.scan("--staged", repo.path).returncode, 1)
        finally:
            repo.cleanup()


@unittest.skipUnless(HAVE_CLI and random_models, "needs the purebyte CLI (PUREBYTE_CLI) and tests/gen")
class OtherCommands(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = TempDir()
        cls.model = write_model(cls.tmp)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_decide(self):
        self.assertEqual(run("decide", "--model", self.model, "--bias", MARK_ALL, "-", stdin=TEXT[:300]).returncode, 1)
        r = run("decide", "--model", self.model, "--bias", MARK_NOTHING, "--text", "port = 8080 and more text here")
        self.assertEqual(r.returncode, 0)
        self.assertEqual(json.loads(r.stdout)["decision"], "negative")
        self.assertEqual(run("decide", "--model", self.model, "-", stdin=b"x" * 5000).returncode, 2)

    def test_redact_and_restore(self):
        source = self.tmp.file("in.txt", b"mail ana@mail.example or call 600 111 222\n")
        spans = [[5, 21, "EMAIL", 0.9], {"start": 30, "end": 41, "type": "PHONE"}]
        spans = self.tmp.file("spans.json", json.dumps(spans).encode())
        out, map_, report = (os.path.join(self.tmp.path, n) for n in ("out.txt", "map.json", "report.json"))
        r = run("redact", "--spans", spans, "--out", out, "--map", map_, "--report", report, source)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out, "rb") as fh:
            self.assertEqual(fh.read(), b"mail [EMAIL_1] or call [PHONE_1]\n")
        with open(report, encoding="utf-8") as fh:
            self.assertNotIn("ana@", fh.read())
        restored = run("redact", "--restore", out, "--map", map_)
        self.assertEqual(restored.stdout, b"mail ana@mail.example or call 600 111 222\n")
        r = run("redact", "--model", self.model, "--profile", "redact", "--bias", MARK_ALL, "-", stdin=TEXT)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotEqual(r.stdout, TEXT)

    def test_info(self):
        r = run("info", self.model)
        self.assertEqual(r.returncode, 0)
        declared = json.loads(r.stdout)["files"][0]["declaration"]
        self.assertEqual(declared["window"]["size"], 80)
        self.assertIn("kernel", json.loads(run("info").stdout))

    def test_models_offline(self):
        home = TempDir()
        try:
            entry = {"name": "demo", "version": "1.0.0", "files": [
                {"role": "model", "file": "purebyte-demo-1.0.0.gguf", "url": "TBD", "sha256": "TBD", "size": None}]}
            catalog = home.file("catalog.json", json.dumps({"schema": 1, "models": [entry]}).encode())
            env = {"PUREBYTE_HOME": home.path, "PUREBYTE_CATALOG": catalog}
            listed = json.loads(run("models", "list", "--json", env=env).stdout)
            self.assertEqual(listed["models"][0]["state"], "not installed")
            self.assertEqual(run("models", "pull", "demo", env=env).returncode, 2)  # not published: nothing downloaded
            shutil.copyfile(self.model, os.path.join(home.path, "purebyte-demo-1.0.0.gguf"))
            r = run("scan", "--model", "demo", "--threads", 1, "--bias", MARK_NOTHING, "-", stdin=TEXT, env=env)
            self.assertEqual(r.returncode, 0, r.stderr)
        finally:
            home.cleanup()


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@unittest.skipUnless(HAVE_CLI and random_models, "needs the purebyte CLI (PUREBYTE_CLI) and tests/gen")
class Serve(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = TempDir()
        cls.model = write_model(cls.tmp)
        cls.port = free_port()
        cls.server = subprocess.Popen([CLI, "serve", "--model", cls.model, "--port", str(cls.port), "--threads", "2",
                                       "--token", "t0ken", "--quiet"], stderr=subprocess.PIPE)
        for _ in range(100):
            try:
                if cls.request("GET", "/health")[0] == 200:
                    break
            except OSError:
                time.sleep(0.1)

    @classmethod
    def tearDownClass(cls):
        cls.server.kill()
        cls.server.wait()
        cls.tmp.cleanup()

    @classmethod
    def request(cls, method, path, body=None, headers=None, token=True):
        c = http.client.HTTPConnection("127.0.0.1", cls.port, timeout=60)
        h = dict(headers or {})
        if token:
            h.setdefault("Authorization", "Bearer t0ken")
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, json.loads(data) if data else None

    def test_health_needs_no_token(self):
        status, body = self.request("GET", "/health", token=False)
        self.assertEqual(status, 200)
        self.assertEqual(body["status"], "ok")

    def test_token_and_host(self):
        self.assertEqual(self.request("GET", "/v1/models", token=False)[0], 401)
        status, body = self.request("GET", "/v1/models", headers={"Host": "attacker.example"})
        self.assertEqual((status, body["error"]["code"]), (403, "bad_host"))

    def test_decide_and_scan(self):
        body = json.dumps({"text": TEXT[:300].decode(), "bias": MARK_ALL})
        status, out = self.request("POST", "/v1/decide", body, {"Content-Type": "application/json"})
        self.assertEqual((status, out["decision"]), (200, "positive"))
        status, out = self.request("POST", "/v1/scan?filename=a.cfg&bias=30", TEXT)
        self.assertEqual(status, 200)
        self.assertEqual(out["results"][0]["file"], "a.cfg")
        self.assertEqual(out["stats"], {"findings": len(out["findings"]),
                                        "by_severity": {"error": len(out["findings"]), "warning": 0}})
        status, out = self.request("POST", "/v1/scan?format=sarif&bias=30", TEXT)
        self.assertEqual((status, out["version"]), (200, "2.1.0"))

    def test_redact(self):
        """/v1/redact has the input checks of /v1/scan: an empty input is refused before any inference."""
        status, out = self.request("POST", "/v1/redact?bias=30", TEXT)
        self.assertEqual(status, 200)
        self.assertNotEqual(out["redacted"], TEXT.decode())
        self.assertTrue(out["report"]["spans"])
        self.assertEqual(self.request("POST", "/v1/redact", b"")[1]["error"]["code"], "empty_input")
        self.assertEqual(self.request("POST", "/v1/redact?format=sarif", TEXT)[1]["error"]["code"], "bad_parameter")

    def test_errors(self):
        self.assertEqual(self.request("POST", "/v1/decide", b"x" * 5000)[1]["error"]["code"], "too_large_for_decide")
        self.assertEqual(self.request("POST", "/v1/scan?colour=red", TEXT)[1]["error"]["code"], "bad_parameter")
        self.assertEqual(self.request("POST", "/v1/scan", b"")[1]["error"]["code"], "empty_input")
        self.assertEqual(self.request("GET", "/v1/scan")[0], 405)
        self.assertEqual(self.request("POST", "/v1/scan?model=nope", TEXT)[1]["error"]["code"], "unknown_model")
        bad = self.request("POST", "/v1/scan", b"{not json", {"Content-Type": "application/json"})
        self.assertEqual(bad[1]["error"]["code"], "bad_json")


@unittest.skipUnless(HAVE_CLI and random_models, "needs the purebyte CLI (PUREBYTE_CLI) and tests/gen")
class ServeSeverity(unittest.TestCase):
    """POST /v1/scan with secrets-code: the severity comes from the `filename` parameter."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = TempDir()
        cls.model = write_model(cls.tmp)
        cls.port = free_port()
        cls.server = subprocess.Popen([CLI, "serve", "--model", cls.model, "--profile", "secrets-code", "--port",
                                       str(cls.port), "--threads", "2", "--quiet"], stderr=subprocess.PIPE)
        for _ in range(100):
            try:
                if cls.post("/health", None, "GET")[0] == 200:
                    break
            except OSError:
                time.sleep(0.1)

    @classmethod
    def tearDownClass(cls):
        cls.server.kill()
        cls.server.wait()
        cls.tmp.cleanup()

    @classmethod
    def post(cls, path, body, method="POST"):
        c = http.client.HTTPConnection("127.0.0.1", cls.port, timeout=60)
        # Raw bytes need a type no web page can send without asking (docs/api.md, "Sending an input").
        c.request(method, path, body=body, headers={"Content-Type": "application/octet-stream"} if body else {})
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, json.loads(data) if data else None

    def test_severity_and_counts(self):
        status, out = self.post("/v1/scan?filename=tests/app.cfg&bias=30", TEXT)
        self.assertEqual(status, 200)
        self.assertTrue(out["findings"])
        self.assertEqual({f["severity"] for f in out["findings"]}, {"warning"})
        self.assertEqual(out["stats"], {"findings": len(out["findings"]),
                                        "by_severity": {"error": 0, "warning": len(out["findings"])}})
        status, out = self.post("/v1/scan?filename=src/app.cfg&bias=30", TEXT)
        self.assertEqual({f["severity"] for f in out["findings"]}, {"error"})
        self.assertEqual(out["stats"]["by_severity"], {"error": len(out["findings"]), "warning": 0})

    def test_sarif_levels(self):
        status, out = self.post("/v1/scan?filename=docs/guide.md&bias=30&format=sarif", TEXT)
        self.assertEqual(status, 200)
        self.assertEqual({r["level"] for r in out["runs"][0]["results"]}, {"warning"})
        status, out = self.post("/v1/scan?filename=app.cfg&bias=30&format=sarif", TEXT)
        self.assertEqual({r["level"] for r in out["runs"][0]["results"]}, {"error"})


if __name__ == "__main__":
    unittest.main()
