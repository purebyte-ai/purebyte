"""End-to-end tests of the `purebyte` command-line tool on the random models of tests/gen: exit statuses, the JSON,
JSON Lines and SARIF reports of spec/OUTPUT.md, standard input, masking (no detected value in clear unless --reveal),
severities (secrets-code: warnings in test, example and documentation paths), the secrets-binary default (findings
touch a printable string unless --all-bytes), and the defenses of the CLI: programs found in absolute PATH folders
only, a model name that a file in the current folder cannot replace, catalog file names that cannot leave the models
directory, file names taken literally by git, links never followed, exclusion patterns that cannot make a scan hang,
a failed batch retried input by input, writes that fail loudly, and a local server that web pages cannot use.

    python tests/python/test_cli.py --purebyte build/cli/purebyte --work build/tests/work
"""
import argparse
import ctypes
import hashlib
import http.client
import io
import json
import os
import pathlib
import shutil
import socket
import stat
import subprocess
import sys
import threading
import time
import zipfile

import common

FAILURES = []
EXE = ".exe" if os.name == "nt" else ""


def check(condition, what):
    if not condition:
        FAILURES.append(what)
    print(f"{'ok  ' if condition else 'FAIL'} {what}")


def skip(what, why):
    print(f"skip {what}: {why}")


def run(tool, *args, stdin=None, cwd=None, env=None, timeout=300):
    try:
        done = subprocess.run([tool, *args], input=stdin, capture_output=True, cwd=cwd, env=env, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "", f"timed out after {timeout} s"
    return done.returncode, done.stdout.decode("utf-8", "replace"), done.stderr.decode("utf-8", "replace")


def fresh(path):
    """An empty folder (what a previous run left there is removed)."""
    def writable_again(function, target, _):  # git makes its objects read-only, which Windows will not delete
        os.chmod(target, stat.S_IWRITE)
        function(target)

    if os.path.lexists(path):
        shutil.rmtree(path, onerror=writable_again)
    os.makedirs(path)
    return path


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return path


def lines_text(tag, count=30):
    lines = (f"{tag}_{i:02d} = value number {i:02d} for the settings file\n" for i in range(1, count + 1))
    return "".join(lines).encode()


def findings_of(report):
    return json.loads(report).get("findings", []) if report.strip() else []


def results_of(report):
    return [r["file"] for r in json.loads(report).get("results", [])] if report.strip() else []


def git(executable, repo, *args):
    """git on `repo`, started from elsewhere by its absolute path (the tests put fake programs in repositories)."""
    settings = ["user.name=purebyte-tests", "user.email=tests@example.invalid", "core.autocrlf=false",
                "commit.gpgsign=false"]
    options = [x for setting in settings for x in ("-c", setting)]
    done = subprocess.run([executable, "-C", repo, *options, *args], capture_output=True)
    if done.returncode != 0:
        raise RuntimeError(f"git {args[0]} failed: {done.stderr.decode(errors='replace').strip()}")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Server:
    """`purebyte serve` on a free loopback port, stopped when the `with` block ends. `quiet=False` keeps the access
    log, which stop() returns."""

    def __init__(self, tool, *args, env=None, quiet=True):
        self.log = None
        self.port = free_port()
        self.process = subprocess.Popen([tool, "serve", "--port", str(self.port), "--threads", "2",
                                         *(["--quiet"] if quiet else []), *args],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
        deadline = time.time() + 60
        while time.time() < deadline and self.process.poll() is None:
            try:
                if self.request("GET", "/health")[0] == 200:
                    return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError(f"purebyte serve did not start: {self.stop()}")

    def request(self, method, path, body=None, headers=None, chunked=False, with_headers=False):
        """`chunked`: send the body with Transfer-Encoding: chunked (no Content-Length)."""
        if chunked:  # an iterable body is sent chunked by http.client
            body = iter([body[i:i + 8192] for i in range(0, len(body), 8192)])
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=60)
        try:
            c.request(method, path, body=body, headers=headers or {})
            r = c.getresponse()
            data = r.read()
        finally:
            c.close()
        answer = (r.status, json.loads(data) if data else None)
        return answer + (dict(r.getheaders()),) if with_headers else answer

    def stop(self):
        """Stops the server (once) and returns what it wrote on standard error."""
        if self.log is None:
            self.process.kill()
            _, errors = self.process.communicate()
            self.log = errors.decode("utf-8", "replace")
        return self.log

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


def error_code(response):
    status, body = response
    return status, (body or {}).get("error", {}).get("code")


def test_all_bytes(tool, folder, model):
    """secrets-binary: by default a finding must touch a printable string (16 ASCII or UTF-16LE characters or more);
    --all-bytes also reports the others."""
    folder = fresh(os.path.join(folder, "all-bytes"))
    no_string = bytes(128 + (i * 37) % 128 for i in range(300))  # bytes 128..255: no printable string at all
    write(os.path.join(folder, "blob.bin"), no_string)
    write(os.path.join(folder, "text.bin"), b"\x00\x01" + b"ab" * 40 + b"\x00\x02" + no_string)
    binary = ["scan", "--model", model, "--profile", "secrets-binary", "--bias", "30"]
    code, out, _ = run(tool, *binary, "blob.bin", "text.bin", cwd=folder)
    spans = [(f["file"], f["start"], f["end"]) for f in findings_of(out)]
    check(code == 1 and spans and all(f == "text.bin" and s < 82 and e > 2 for f, s, e in spans),
          f"secrets-binary: by default only findings that touch a printable string (got {code}, {spans})")
    code, out, _ = run(tool, *binary, "--all-bytes", "blob.bin", "text.bin", cwd=folder)
    check(code == 1 and "blob.bin" in {f["file"] for f in findings_of(out)},
          "secrets-binary: --all-bytes also reports findings that touch no printable string")
    code, out, _ = run(tool, "decide", "--model", model, "--profile", "secrets-binary", "--bias", "30", "blob.bin",
                       cwd=folder)
    check(code == 0 and json.loads(out)["decision"] == "negative", "decide: the same default (negative on no string)")
    code, _, _ = run(tool, "decide", "--model", model, "--profile", "secrets-binary", "--bias", "30", "--all-bytes",
                     "blob.bin", cwd=folder)
    check(code == 1, "decide --all-bytes: positive on the same bytes")


def test_batch_retry(tool, folder, model):
    """An engine failure on a batch costs only the inputs that fail on their own: the others are reported, and the
    failed one is listed as not scanned in every format (PUREBYTE_TEST_FAIL_INPUT makes the engine calls that include
    that input fail)."""
    folder = fresh(os.path.join(folder, "batch"))
    for name in ("a.cfg", "b.cfg", "c.cfg"):
        write(os.path.join(folder, name), lines_text(name[0]))
    env = dict(os.environ, PUREBYTE_TEST_FAIL_INPUT="b.cfg")
    scan = ["scan", "--model", model, "--profile", "none", "a.cfg", "b.cfg", "c.cfg"]
    code, out, err = run(tool, *scan, cwd=folder, env=env)
    report = json.loads(out) if out.strip() else {}
    failures = report.get("failures", [])
    check(code == 2 and results_of(out) == ["a.cfg", "c.cfg"] and [f["file"] for f in failures] == ["b.cfg"] and
          "the scan failed" in failures[0]["reason"] and report["stats"]["not_scanned"] == 1 and "one by one" in err,
          f"a failed batch is retried input by input: a report, with the failing input not scanned (got {code})")
    code, out, _ = run(tool, *scan, "--format", "sarif", cwd=folder, env=env)
    run0 = (json.loads(out)["runs"] if out.strip() else [{}])[0]
    invocation = (run0.get("invocations") or [{}])[0]
    notes = [n["locations"][0]["physicalLocation"]["artifactLocation"]["uri"]
             for n in invocation.get("toolExecutionNotifications", [])]
    uris = {r["locations"][0]["physicalLocation"]["artifactLocation"]["uri"] for r in run0.get("results", [])}
    check(code == 2 and notes == ["b.cfg"] and uris == {"a.cfg", "c.cfg"} and
          invocation.get("executionSuccessful") is False, "sarif: the same, the failing input as a notification")
    code, out, _ = run(tool, *scan, "--format", "jsonl", cwd=folder, env=env)
    lines = [json.loads(line) for line in out.splitlines() if line.strip()]
    check(code == 2 and [line["file"] for line in lines] == ["a.cfg", "b.cfg", "c.cfg"] and
          "not_scanned" in lines[1], "jsonl: the same, one line per input")


def test_programs_from_path(tool, folder, model):
    """git and curl are run from the absolute folders of PATH only: a `git` or `curl` in the current folder (a
    repository scanned by a hook or in CI may ship one) or in a relative PATH entry is never run. The fakes are copies
    of purebyte, which would answer `unknown command`."""
    env = dict(os.environ, PATH=os.pathsep.join([".", "", os.environ.get("PATH", "")]))
    for key in [k for k in env if k.upper() == "NODEFAULTCURRENTDIRECTORYINEXEPATH"]:
        del env[key]  # Windows: the default search of CreateProcess, current folder included
    real_git = shutil.which("git")
    if not real_git:
        skip("scan --staged ignores a git in the current folder", "git is not installed")
    else:
        repo = fresh(os.path.join(folder, "fake-git"))
        git(real_git, repo, "init", "-q")
        write(os.path.join(repo, "app.cfg"), lines_text("app"))
        git(real_git, repo, "add", "app.cfg")
        shutil.copyfile(tool, os.path.join(repo, "git" + EXE))
        os.chmod(os.path.join(repo, "git" + EXE), 0o755)
        code, out, err = run(tool, "scan", "--staged", "--model", model, "--profile", "none", cwd=repo, env=env)
        check(code in (0, 1) and "unknown command" not in err and results_of(out) == ["app.cfg"],
              f"scan --staged runs the git of PATH, never the one in the repository (got {code}: {err.strip()[:200]})")
    real_curl = shutil.which("curl")
    if not real_curl:
        skip("models pull ignores a curl in the current folder", "curl is not installed")
    else:
        home = fresh(os.path.join(folder, "fake-curl"))
        with open(model, "rb") as f:
            data = f.read()
        source = write(os.path.join(home, "mirror", "demo.gguf"), data)
        entry = {"name": "demo", "version": "1.0.0", "summary": "a test model", "files": [
            {"role": "model", "file": "purebyte-demo-1.0.0.gguf", "url": pathlib.Path(source).as_uri(),
             "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}]}
        catalog = write(os.path.join(home, "catalog.json"), json.dumps({"schema": 1, "models": [entry]}).encode())
        shutil.copyfile(tool, os.path.join(home, "curl" + EXE))
        os.chmod(os.path.join(home, "curl" + EXE), 0o755)
        models = os.path.join(home, "models")
        code, _, err = run(tool, "models", "pull", "demo", cwd=home,
                           env=dict(env, PUREBYTE_HOME=models, PUREBYTE_CATALOG=catalog))
        check(code == 0 and "unknown command" not in err and
              os.path.isfile(os.path.join(models, "purebyte-demo-1.0.0.gguf")),
              f"models pull runs the curl of PATH, never the one in the current folder (got {code}: {err[:200]})")
    missing = os.path.join(folder, "empty-path")
    os.makedirs(missing, exist_ok=True)
    code, _, err = run(tool, "scan", "--staged", "--model", model, cwd=folder, env=dict(os.environ, PATH=missing))
    check(code == 2 and "not found" in err, f"without git on PATH, --staged says so (got {code}: {err.strip()[:200]})")


def test_literal_paths(tool, folder, model):
    """--staged: file names are paths, never patterns or revision syntax. `[a].cfg` must not also match `a.cfg` (whose
    added lines would count as its own); on POSIX, `0:app.cfg` is not stage 0 of `app.cfg`, and `:(glob)app.cfg` is not
    a pattern that misses itself (which dropped its findings)."""
    real_git = shutil.which("git")
    if not real_git:
        skip("--staged takes file names literally", "git is not installed")
        return
    repo = fresh(os.path.join(folder, "literal"))
    git(real_git, repo, "init", "-q")
    write(os.path.join(repo, "a.cfg"), lines_text("plain"))
    write(os.path.join(repo, "[a].cfg"), lines_text("bracket"))
    git(real_git, repo, "--literal-pathspecs", "add", "--", "a.cfg", "[a].cfg")
    git(real_git, repo, "commit", "-q", "-m", "base")
    write(os.path.join(repo, "a.cfg"), b"top_line = a new first line of the settings\n" + lines_text("plain"))
    write(os.path.join(repo, "[a].cfg"), lines_text("bracket") + b"end_line = a new last line of the settings\n")
    staged = ["a.cfg", "[a].cfg"]
    odd = os.name != "nt"  # names Windows does not allow
    if odd:
        write(os.path.join(repo, "app.cfg"), lines_text("other", 3))
        write(os.path.join(repo, "0:app.cfg"), lines_text("zero", 5))
        write(os.path.join(repo, ":(glob)app.cfg"), lines_text("glob", 4))
        staged += ["app.cfg", "0:app.cfg", ":(glob)app.cfg"]
    git(real_git, repo, "--literal-pathspecs", "add", "--", *staged)
    code, out, err = run(tool, "scan", "--staged", "--model", model, "--profile", "secrets-code", "--bias", "30", repo)
    lines = {}
    for f in findings_of(out):
        lines.setdefault(f["file"], []).append((f["line"], f.get("end_line", f["line"])))
    check(lines.get("a.cfg") and all(a <= 1 <= b for a, b in lines["a.cfg"]),
          f"--staged: a.cfg keeps findings on its added first line only (got {lines.get('a.cfg')})")
    check(lines.get("[a].cfg") and all(a <= 31 <= b for a, b in lines["[a].cfg"]),
          f"--staged: `[a].cfg` is one file, not a pattern that adds a.cfg's lines (got {lines.get('[a].cfg')})")
    if odd:
        sizes = {r["file"]: r["bytes"] for r in json.loads(out).get("results", [])} if out.strip() else {}
        check(sizes.get("0:app.cfg") == len(lines_text("zero", 5)),
              f"--staged: `0:app.cfg` is read as itself, not as stage 0 of app.cfg (got {sizes})")
        check(len(lines.get(":(glob)app.cfg", [])) == 4,
              f"--staged: `:(glob)app.cfg` is not a pattern: its findings are kept (got {lines.get(':(glob)app.cfg')})")
    check(code == 1, f"--staged on tricky names exits 1 (got {code}: {err.strip()[:200]})")


def make_junction(link, target):
    """A Windows directory junction (no privilege needed); False when it cannot be made."""
    try:
        import _winapi
        _winapi.CreateJunction(target, link)
    except (ImportError, AttributeError, OSError):
        subprocess.run(["cmd", "/c", "mklink", "/J", link, target], capture_output=True)
    return os.path.isdir(link)


def make_symlink(link, target):
    """A symbolic link; False where the system does not let this user make one (Windows without developer mode)."""
    try:
        os.symlink(target, link, target_is_directory=os.path.isdir(target))
    except (OSError, NotImplementedError):
        return False
    if os.name == "nt":
        try:
            import ctypes
            attrs = ctypes.windll.kernel32.GetFileAttributesW(str(link))
            if attrs == -1 or not (attrs & 0x400):
                return False
        except Exception:
            pass
    return os.path.islink(link)


def test_links_not_followed(tool, folder, model):
    """Links are never followed: a walk does not enter a junction (Windows) or a symbolic link to a folder, one that
    points to a folder above it would never end; --git-diff does not read through a link of the working tree, which
    may point anywhere on the machine."""
    root = fresh(os.path.join(folder, "links"))
    write(os.path.join(root, "tree", "app.cfg"), lines_text("app"))
    link, target = os.path.join(root, "tree", "back"), os.path.join(root, "tree")
    kind = "junction" if os.name == "nt" else "symbolic link"
    if not (make_junction(link, target) if os.name == "nt" else make_symlink(link, target)):
        skip(f"walks do not follow a {kind}", f"cannot make a {kind} here")
    else:
        code, out, err = run(tool, "scan", "--model", model, "--profile", "none", "tree", cwd=root, timeout=120)
        check(code in (0, 1) and results_of(out) == ["tree/app.cfg"],
              f"a walk does not follow a {kind} to its own folder (got {code}, {results_of(out)[:3]}: {err[:200]})")
    real_git = shutil.which("git")
    outside = write(os.path.join(root, "outside", "private.cfg"), lines_text("private"))
    repo = fresh(os.path.join(root, "repo"))
    if not real_git:
        skip("--git-diff does not follow links", "git is not installed")
        return
    git(real_git, repo, "init", "-q")
    write(os.path.join(repo, "app.cfg"), lines_text("app"))
    git(real_git, repo, "add", "app.cfg")
    git(real_git, repo, "commit", "-q", "-m", "base")
    write(os.path.join(repo, "app.cfg"), lines_text("app") + b"new_line = one more line of settings here\n")
    if not make_symlink(os.path.join(repo, "leak.cfg"), outside):
        skip("--git-diff does not follow links", "cannot make a symbolic link here")
        return
    git(real_git, repo, "add", "app.cfg", "leak.cfg")
    code, out, err = run(tool, "scan", "--git-diff", "HEAD", "--model", model, "--profile", "none", repo)
    check(code in (0, 1) and results_of(out) == ["app.cfg"],
          f"--git-diff does not read through a symbolic link of the working tree (got {code}, {results_of(out)})")


def multipart(filename, data):
    boundary = "purebyte-test-boundary"
    head = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{filename}\"\r\n"
            "Content-Type: application/octet-stream\r\n\r\n")
    return head.encode() + data + f"\r\n--{boundary}--\r\n".encode(), f"multipart/form-data; boundary={boundary}"


def port_is_exclusive(port):
    """Whether another socket fails to bind 127.0.0.1:port while the server listens there (it must)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            try:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            except OSError:
                pass
        s.bind(("127.0.0.1", port))
        return False
    except OSError:
        return True
    finally:
        s.close()


def test_exclusions(tool, folder, model):
    """--exclude and .purebyteignore leave paths out of a walk and of --git-diff; a bad pattern is a usage error."""
    root = fresh(os.path.join(folder, "exclusions"))
    for name in ("app.cfg", "keys.cfg", "generated/out.cfg", "docs/examples/demo.cfg", "vendor/lib/lib.cfg",
                 "deep/a/fixtures/x.cfg", "notes.min.cfg"):
        write(os.path.join(root, "tree", *name.split("/")), lines_text(name.replace("/", "_").replace(".", "_")))
    write(os.path.join(root, "tree", ".purebyteignore"),
          b"# generated and vendored code\ngenerated/\n/vendor\n\n**/fixtures/**\n")
    code, out, err = run(tool, "scan", "--model", model, "--profile", "none", "--exclude", "keys.cfg",
                         "--exclude", "docs/examples/*", "--exclude", "*.min.cfg", "tree", cwd=root, timeout=120)
    # The ignore file itself is an ordinary file of the tree (the `none` profile reads every file).
    check(code in (0, 1) and results_of(out) == ["tree/.purebyteignore", "tree/app.cfg"],
          f".purebyteignore and --exclude leave paths out of a walk (got {code}, {results_of(out)}: {err[:200]})")
    code, out, err = run(tool, "scan", "--model", model, "--profile", "none", os.path.join("tree", "keys.cfg"),
                         "--exclude", "keys.cfg", cwd=root)
    check(code in (0, 1) and results_of(out) == ["tree/keys.cfg"], "a file named explicitly is always scanned")
    for bad in ("!keys.cfg", "[ab].cfg", "  "):
        code, _, err = run(tool, "scan", "--model", model, "--exclude", bad, "tree", cwd=root)
        check(code == 2 and "exclusion" in err, f"--exclude {bad!r} is refused with a message (got {code})")
    real_git = shutil.which("git")
    if not real_git:
        skip("--git-diff reads the repository's .purebyteignore", "git is not installed")
        return
    repo = fresh(os.path.join(root, "repo"))
    git(real_git, repo, "init", "-q")
    for name in ("app.cfg", "generated/out.cfg"):
        write(os.path.join(repo, *name.split("/")), lines_text("base"))
    write(os.path.join(repo, ".purebyteignore"), b"generated/\n")
    git(real_git, repo, "add", "-A")
    git(real_git, repo, "commit", "-q", "-m", "base")
    for name in ("app.cfg", "generated/out.cfg"):
        write(os.path.join(repo, *name.split("/")), lines_text("base") + b"new_line = one more line of settings\n")
    code, out, err = run(tool, "scan", "--git-diff", "HEAD", "--model", model, "--profile", "none", repo)
    check(code in (0, 1) and results_of(out) == ["app.cfg"],
          f"--git-diff reads the repository's .purebyteignore (got {code}, {results_of(out)}: {err[:200]})")


def test_serve(tool, folder, model):
    """The local server answers programs, not web pages: an Origin of another site is refused, and so is a POST a page
    could send without asking (a form, plain text, no type) unless it has X-PureByte or the token. Archives are
    expanded only with --archives; secrets-binary findings touch a printable string unless strings_only=no; the port
    is the server's alone; the token can come from a file or the environment."""
    folder = fresh(os.path.join(folder, "serve"))
    text = lines_text("serve", 4)
    no_string = bytes(128 + (i * 37) % 128 for i in range(300))
    body = json.dumps({"text": text.decode()})
    as_json = {"Content-Type": "application/json"}
    with Server(tool, "--model", model, "--profile", "secrets-binary", "--bias", "30") as server:
        check(error_code(server.request("POST", "/v1/scan", body, dict(as_json, Origin="https://evil.example"))) ==
              (403, "bad_origin"), "serve: a request from a page of another site is refused (403 bad_origin)")
        check(error_code(server.request("POST", "/v1/scan", body, dict(as_json, Origin="null"))) ==
              (403, "bad_origin"), "serve: Origin null is refused")
        check(error_code(server.request("GET", "/v1/models", None, {"Origin": "http://localhost.evil.example"})) ==
              (403, "bad_origin"), "serve: GET requests carry the same Origin check")
        check(server.request("POST", "/v1/scan", body, dict(as_json, Origin="http://localhost:5173"))[0] == 200,
              "serve: a page served by this machine (a loopback Origin) is answered")
        for content_type in ("text/plain; charset=utf-8", "application/x-www-form-urlencoded", None):
            headers = {"Content-Type": content_type} if content_type else {}
            check(error_code(server.request("POST", "/v1/scan", text, headers)) == (403, "missing_header"),
                  f"serve: a POST of type {content_type} without X-PureByte is refused (403 missing_header)")
            check(server.request("POST", "/v1/scan", text, dict(headers, **{"X-PureByte": "1"}))[0] == 200,
                  "serve: the same POST with X-PureByte is answered")
        form, form_type = multipart("app.cfg", text)
        check(error_code(server.request("POST", "/v1/scan", form, {"Content-Type": form_type})) ==
              (403, "missing_header"), "serve: a multipart upload without X-PureByte is refused")
        status, out = server.request("POST", "/v1/scan", form, {"Content-Type": form_type, "X-PureByte": "1"})
        check(status == 200 and out["results"][0]["file"] == "app.cfg", "serve: a multipart upload with X-PureByte")
        raw = {"Content-Type": "application/octet-stream"}
        status, out = server.request("POST", "/v1/scan", no_string, raw)
        check(status == 200 and out["findings"] == [], "serve: application/octet-stream is answered; secrets-binary "
                                                        "findings touch a printable string by default")
        status, out = server.request("POST", "/v1/scan?strings_only=no", no_string, raw)
        check(status == 200 and out["findings"], "serve: strings_only=no reports findings outside printable strings")
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("inner.cfg", text)
        status, out = server.request("POST", "/v1/scan?filename=bundle.zip", archive.getvalue(), raw)
        check(status == 200 and [r["file"] for r in out["results"]] == ["bundle.zip"],
              "serve: archives are not expanded by default")
        check(port_is_exclusive(server.port), "serve: no other socket can bind the server's port")
        code, _, err = run(tool, "serve", "--model", model, "--port", str(server.port), "--token", "x", timeout=60)
        check(code == 2 and "cannot listen" in err and "--token-file" in err,
              f"serve: a second server cannot take the port; --token warns that it is visible (got {code})")
    with Server(tool, "--model", model, "--profile", "secrets-binary", "--archives") as server:
        status, out = server.request("POST", "/v1/scan?filename=bundle.zip", archive.getvalue(), raw)
        check(status == 200 and "bundle.zip!/inner.cfg" in [r["file"] for r in out["results"]],
              "serve --archives: archive members are scanned")
    env = dict(os.environ, PUREBYTE_SERVE_TOKEN="t0ken-from-env")
    with Server(tool, "--model", model, env=env) as server:
        check(server.request("GET", "/v1/models")[0] == 401 and
              server.request("GET", "/v1/models", None, {"Authorization": "Bearer t0ken-from-env"})[0] == 200,
              "serve: the token can come from PUREBYTE_SERVE_TOKEN")
        check(server.request("POST", "/v1/scan", text, {"Authorization": "Bearer t0ken-from-env"})[0] == 200,
              "serve: with the token, a POST needs no X-PureByte")
    token_file = write(os.path.join(folder, "token.txt"), b"\xef\xbb\xbft0ken-from-file\r\nsecond line\n")  # BOM, CRLF
    with Server(tool, "--model", model, "--token-file", token_file) as server:
        check(server.request("GET", "/v1/models")[0] == 401 and
              server.request("GET", "/v1/models", None, {"Authorization": "Bearer t0ken-from-file"})[0] == 200,
              "serve: --token-file reads the token on the first line of the file (UTF-8, with or without a BOM)")
    code, _, err = run(tool, "serve", "--model", model, "--token", "a", "--token-file", token_file)
    check(code == 2 and "alternatives" in err, "serve: --token and --token-file together are refused")


def sha256_of(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def catalog_entry(name, model, file_name=None, version="1.0.0", **extra):
    """A catalog entry whose one file is `model`, with its checksum, size and a file:// URL."""
    with open(model, "rb") as f:
        data = f.read()
    entry = {"name": name, "version": version, "summary": "a test model", "files": [
        {"role": "model", "file": file_name or f"purebyte-{name}-{version}.gguf", "url": pathlib.Path(model).as_uri(),
         "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}]}
    entry.update(extra)
    return entry


def write_catalog(folder, entries):
    return write(os.path.join(folder, "catalog.json"), json.dumps({"schema": 1, "models": entries}).encode())


def test_model_resolution(tool, folder, model, other):
    """A bare name is always a name of the catalog: a file of that name in the current folder (the integrations run
    from the root of the repository they scan, which can ship one) is never loaded in its place. A model file is
    given by its path: with a folder in it, a leading `.`, or the .gguf extension."""
    folder = fresh(os.path.join(folder, "resolution"))
    home = fresh(os.path.join(folder, "home"))
    write(os.path.join(folder, "app.cfg"), lines_text("app"))
    shutil.copyfile(other, os.path.join(folder, "secrets-code"))  # planted where the scan runs
    env = {k: v for k, v in os.environ.items() if k != "PUREBYTE_CATALOG"}
    env["PUREBYTE_HOME"] = home
    code, out, err = run(tool, "scan", "--model", "secrets-code", "app.cfg", cwd=folder, env=env)
    check(code == 2 and "is not installed" in err and not out.strip(),
          f"--model secrets-code never loads a file named secrets-code from the current folder (got {code}: "
          f"{err.strip()[:200]})")
    shutil.copyfile(model, os.path.join(home, "purebyte-demo-1.0.0.gguf"))
    env["PUREBYTE_CATALOG"] = write_catalog(home, [catalog_entry("demo", model)])
    shutil.copyfile(other, os.path.join(folder, "demo"))
    for spec, expected, what in (("demo", model, "the installed specialist, not the file ./demo"),
                                 ("./demo", other, "the file ./demo, named by its path")):
        code, out, err = run(tool, "scan", "--model", spec, "--profile", "none", "app.cfg", cwd=folder, env=env)
        files = json.loads(out)["model"]["files"] if out.strip() else []
        check(code in (0, 1) and [f["sha256"] for f in files] == [sha256_of(expected)],
              f"--model {spec} loads {what} (got {code}: {err.strip()[:200]})")
    code, out, _ = run(tool, "info", "demo", cwd=folder, env=env)
    check(code == 0 and json.loads(out)["files"][0]["sha256"] == sha256_of(model), "info demo: the same rule")


def test_catalog_file_names(tool, folder, model):
    """A catalog whose `file` is not a plain name ending in .gguf (a path, `..`, a drive, a stream) is refused before
    anything is downloaded or written: `models pull` writes where `file` says."""
    root = fresh(os.path.join(folder, "catalog-names"))
    home = fresh(os.path.join(root, "home"))
    outside = os.path.join(root, "escaped.gguf")
    env = dict(os.environ, PUREBYTE_HOME=home)
    for bad in ("../escaped.gguf", os.path.abspath(outside), "sub/escaped.gguf", "..\\escaped.gguf", "C:escaped.gguf",
                "escaped.gguf:stream", ".escaped.gguf", "escaped.bin"):
        env["PUREBYTE_CATALOG"] = write_catalog(root, [catalog_entry("demo", model, file_name=bad)])
        code, _, err = run(tool, "models", "pull", "demo", env=env)
        check(code == 2 and "plain name" in err and not os.path.exists(outside) and
              not os.path.exists(outside + ".sha256") and os.listdir(home) == [],
              f"models pull refuses the catalog file name {bad!r}, before any download (got {code}: {err[:160]})")
    code, _, err = run(tool, "models", "list", env=env)
    check(code == 2 and "plain name" in err, "models list refuses the same catalog")
    code, _, err = run(tool, "scan", "--model", "demo", "-", stdin=b"x" * 64, env=env)
    check(code == 2 and "plain name" in err, "scan --model refuses it too")


def test_models_actions(tool, folder, model):
    """`models verify NAME` fails for a name the catalog does not know and for a specialist that is not installed
    completely; options of another action are refused; a specialist that needs a newer runtime is refused before any
    download and shown so by `list`; without a models directory the CLI asks for PUREBYTE_HOME."""
    root = fresh(os.path.join(folder, "models-actions"))
    home = fresh(os.path.join(root, "home"))
    catalog = write_catalog(root, [catalog_entry("demo", model), catalog_entry("future", model, min_runtime="99.0.0")])
    env = dict(os.environ, PUREBYTE_HOME=home, PUREBYTE_CATALOG=catalog)
    code, _, err = run(tool, "models", "verify", "demoo", env=env)
    check(code == 2 and "unknown model" in err, f"models verify: an unknown name exits 2 (got {code})")
    code, out, _ = run(tool, "models", "verify", "demo", env=env)
    check(code == 2 and "MISSING" in out, f"models verify NAME: a specialist that is not installed exits 2 (got {code})")
    code, out, _ = run(tool, "models", "verify", env=env)
    check(code == 0 and "no installed model to verify in" in out and "\\" not in out,
          "models verify: nothing installed passes, and the directory is written with /")
    code, _, err = run(tool, "models", "list", env=dict(env, PUREBYTE_CATALOG=os.path.join(root, "no", "such.json")))
    check(code == 2 and "cannot read the model catalog" in err and "\\" not in err,
          f"a missing PUREBYTE_CATALOG is named with / (got {code}: {err.strip()[:160]})")
    for args in (["list", "--force"], ["list", "extra"], ["pull", "demo", "--json"], ["verify", "--json"],
                 ["verify", "demo", "extra"]):
        code, _, _ = run(tool, "models", *args, env=env)
        check(code == 2, f"models {' '.join(args)}: an argument that does not apply is refused (got {code})")
    code, _, err = run(tool, "models", "pull", "future", env=env)
    check(code == 2 and "needs purebyte 99.0.0" in err and os.listdir(home) == [],
          f"models pull refuses a specialist that needs a newer runtime, before any download (got {code})")
    states = {m["name"]: m["state"] for m in json.loads(run(tool, "models", "list", "--json", env=env)[1])["models"]}
    check(states.get("future") == "needs purebyte 99.0.0 or newer", f"models list shows it (got {states})")
    code, _, err = run(tool, "scan", "--model", "future", "-", stdin=lines_text("x"), env=env)
    check(code == 2 and "needs purebyte 99.0.0" in err, "scan --model refuses it too")
    shutil.copyfile(model, os.path.join(home, "purebyte-demo-1.0.0.gguf"))
    env["PUREBYTE_CATALOG"] = write_catalog(root, [catalog_entry("demo", model),
                                                   catalog_entry("demo", model, version="2.0.0", min_runtime="99.0.0")])
    code, out, _ = run(tool, "scan", "--model", "demo", "--profile", "none", "-", stdin=lines_text("x"), env=env)
    check(code in (0, 1) and json.loads(out)["model"]["version"] == "1.0.0",
          "without a version, the highest version this runtime can run is chosen")
    code, out, _ = run(tool, "models", "verify", "demo", env=env)
    check(code == 0 and "OK" in out, f"models verify NAME: an installed specialist passes (got {code})")
    bare = {k: v for k, v in os.environ.items()
            if k.upper() not in ("PUREBYTE_HOME", "HOME", "XDG_DATA_HOME", "LOCALAPPDATA", "PUREBYTE_CATALOG")}
    code, _, err = run(tool, "models", "list", env=bare)
    check(code == 2 and "PUREBYTE_HOME" in err, f"no models directory: models list asks for PUREBYTE_HOME (got {code})")
    code, out, _ = run(tool, "info", env=bare)
    check(code == 0 and json.loads(out)["models_directory"] is None, "no models directory: info says null")


def test_git_changes(tool, folder, model):
    """--staged scans a file whose type changed (a link that became a file), keeps no finding of a file that was only
    moved, never enters a submodule, skips an empty file and reports one over the limit without reading it; --git-diff
    runs no textconv filter, so the added lines are those of the file scanned."""
    real_git = shutil.which("git")
    if not real_git:
        skip("--staged: type changes, renames and submodules", "git is not installed")
        return
    repo = fresh(os.path.join(folder, "git-changes"))
    git(real_git, repo, "init", "-q")
    write(os.path.join(repo, "move_me.cfg"), lines_text("moved"))
    git(real_git, repo, "add", "move_me.cfg")
    link = subprocess.run([real_git, "-C", repo, "hash-object", "-w", "--stdin"], input=b"move_me.cfg",
                          capture_output=True, check=True).stdout.decode().strip()
    git(real_git, repo, "update-index", "--add", "--cacheinfo", f"120000,{link},config.cfg")  # a link
    git(real_git, repo, "commit", "-q", "-m", "base")
    head = subprocess.run([real_git, "-C", repo, "rev-parse", "HEAD"], capture_output=True,
                          check=True).stdout.decode().strip()
    write(os.path.join(repo, "config.cfg"), lines_text("typed", 5))  # the link becomes a file
    git(real_git, repo, "-c", "core.symlinks=true", "add", "config.cfg")
    git(real_git, repo, "mv", "move_me.cfg", "moved.cfg")
    write(os.path.join(repo, "empty.cfg"), b"")
    git(real_git, repo, "add", "empty.cfg")
    git(real_git, repo, "update-index", "--add", "--cacheinfo", f"160000,{head},deps/sub")  # a submodule
    code, out, err = run(tool, "scan", "--staged", "--model", model, "--profile", "none", "--bias", "30", repo)
    report = json.loads(out) if out.strip() else {}
    found = {f["file"] for f in report.get("findings", [])}
    check("config.cfg" in found, f"--staged scans a link that became a file (type change) (got {sorted(found)})")
    check(found.isdisjoint({"moved.cfg", "move_me.cfg"}), "--staged: a file only moved adds no line, so no finding")
    check(code == 1 and report.get("failures") == [],
          f"--staged: a submodule is not entered, and is no failure (got {code}, {report.get('failures')})")
    check("empty.cfg" not in results_of(out), "--staged: an empty file is skipped, as in a walk")
    write(os.path.join(repo, "big.cfg"), b"a = b\n" * 666667)  # 4,000,002 bytes
    git(real_git, repo, "add", "big.cfg")
    code, out, _ = run(tool, "scan", "--staged", "--model", model, "--profile", "secrets-code", repo)
    failures = json.loads(out).get("failures", []) if out.strip() else []
    check(code == 2 and failures == [{"file": "big.cfg", "reason": "4000002 bytes, over this model's limit of "
                                                                   "4000000 bytes: not scanned"}],
          f"--staged: a file over the limit is reported as a walk reports it (got {code}, {failures})")
    if os.name == "nt" or not shutil.which("sed"):
        skip("--git-diff runs no textconv filter", "needs sed (POSIX)")
        return
    repo = fresh(os.path.join(folder, "git-textconv"))
    git(real_git, repo, "init", "-q")
    write(os.path.join(repo, "t.cfg"), lines_text("t", 2))
    git(real_git, repo, "add", "t.cfg")
    git(real_git, repo, "commit", "-q", "-m", "base")
    write(os.path.join(repo, ".gitattributes"), b"*.cfg diff=shift\n")
    git(real_git, repo, "config", "diff.shift.textconv", "sed 1d")  # would shift the hunks by one line
    write(os.path.join(repo, "t.cfg"), lines_text("t", 2) + b"added_line = the third line of the settings file\n")
    code, out, _ = run(tool, "scan", "--git-diff", "HEAD", "--model", model, "--profile", "none", "--bias", "30", repo)
    spans = [(f["line"], f.get("end_line", f["line"])) for f in findings_of(out)]
    check(code == 1 and spans and all(a <= 3 <= b for a, b in spans),
          f"--git-diff ignores textconv: findings on the added line 3 only (got {code}, {spans})")


def test_hostile_exclusions(tool, folder, model):
    """Exclusion patterns come with the repository scanned: many `**` cannot make a walk slow, a folder whose name is
    not ASCII still has its .purebyteignore read (Windows), and a .purebyteignore that is a link is refused."""
    root = fresh(os.path.join(folder, "hostile-exclusions"))
    deep = os.path.join(root, "deep")
    write(os.path.join(deep, *(["d"] * 20), "secret.cfg"), lines_text("deep"))
    hostile = "/".join(["**"] * 30) + "/zzz\n" + "**/d/" * 15 + "zzz\n"  # hours with a search that tries every split
    write(os.path.join(deep, ".purebyteignore"), hostile.encode())
    started = time.time()
    code, out, err = run(tool, "scan", "--model", model, "--profile", "none", "deep", cwd=root, timeout=120)
    check(code in (0, 1) and results_of(out) == ["deep/.purebyteignore", "deep/" + "d/" * 20 + "secret.cfg"] and
          time.time() - started < 60, f"30 `**` in a pattern keep a walk fast (got {code}, {err.strip()[:160]})")
    write(os.path.join(deep, ".purebyteignore"), ("**/d/" * 15 + "secret.cfg\n").encode())
    code, out, _ = run(tool, "scan", "--model", model, "--profile", "none", "deep", cwd=root, timeout=120)
    check(code in (0, 1) and results_of(out) == ["deep/.purebyteignore"], "... and such a pattern still matches")
    accented = os.path.join(root, "tést")
    write(os.path.join(accented, "app.cfg"), lines_text("app"))
    write(os.path.join(accented, "skip.cfg"), lines_text("skip"))
    write(os.path.join(accented, ".purebyteignore"), b"skip.cfg\n")
    code, out, _ = run(tool, "scan", "--model", model, "--profile", "none", "tést", cwd=root)
    check(code in (0, 1) and results_of(out) == ["tést/.purebyteignore", "tést/app.cfg"],
          f"the .purebyteignore of a folder whose name is not ASCII is read (got {results_of(out)})")
    linked = os.path.join(root, "linked")
    write(os.path.join(linked, "app.cfg"), lines_text("app"))
    if not make_symlink(os.path.join(linked, ".purebyteignore"), os.path.join(accented, ".purebyteignore")):
        skip("a .purebyteignore that is a link is refused", "cannot make a symbolic link here")
        return
    code, _, err = run(tool, "scan", "--model", model, "--profile", "none", "linked", cwd=root)
    check(code == 2 and "symbolic link" in err, f"a .purebyteignore that is a symbolic link is refused (got {code})")


def test_outputs(tool, folder, model):
    """A write that fails is exit status 2 (and `redact` does not call a copy it could not write "checked"); the map
    of `redact` is made readable by its owner only, also when it existed, and a link in its place is refused."""
    folder = fresh(os.path.join(folder, "outputs"))
    source = write(os.path.join(folder, "in.txt"), b"mail ana@mail.example now\n")
    spans = write(os.path.join(folder, "spans.json"), b'[[5, 21, "EMAIL"]]')
    if os.path.exists("/dev/full"):
        code, _, err = run(tool, "scan", "--model", model, "--profile", "none", "--out", "/dev/full", "-",
                           stdin=lines_text("x"))
        check(code == 2 and "cannot write" in err, f"scan --out on a full disk exits 2 (got {code})")
        code, _, err = run(tool, "redact", "--spans", spans, "--out", "/dev/full", source)
        check(code == 2 and "cannot write" in err and "(checked)" not in err,
              f"redact --out on a full disk exits 2 without claiming a checked copy (got {code})")
    else:
        skip("writes that fail exit 2", "no /dev/full here")
    if os.name == "nt":
        skip("the redaction map is owner-only", "POSIX permissions")
        return
    map_path = write(os.path.join(folder, "map.json"), b"{}")
    os.chmod(map_path, 0o644)
    code, _, _ = run(tool, "redact", "--spans", spans, "--map", map_path, "--out", os.devnull, source)
    check(code == 0 and stat.S_IMODE(os.stat(map_path).st_mode) == 0o600,
          "redact --map makes an existing map readable by its owner only")
    target = write(os.path.join(folder, "target.txt"), b"untouched")
    link = os.path.join(folder, "link.json")
    if make_symlink(link, target):
        code, _, err = run(tool, "redact", "--spans", spans, "--map", link, "--out", os.devnull, source)
        with open(target, "rb") as f:
            check(code == 2 and "symbolic link" in err and f.read() == b"untouched",
                  "redact --map refuses a symbolic link instead of writing through it")


def test_arguments(tool, folder, model):
    """Nothing given on the command line is silently ignored."""
    for args, words in ((["version", "--bogus"], "unknown option"),
                        (["version", "extra"], "no argument"),
                        (["info", model, "extra"], "at most one"),
                        (["scan", "--model", model, "--ensemble", model, "--no-ensemble", "-"], "alternatives"),
                        (["decide", "--model", model, "--type-bias", "entity_0=nan", "--text", "x" * 40], "finite"),
                        (["redact", "--restore", "r.txt", "--map", "m.json", "--model", model], "does not apply"),
                        (["redact", "--restore", "r.txt", "--map", "m.json", "other.txt"], "no other input"),
                        (["redact", "--spans", "s.json", "--bias", "1", "-"], "does not apply"),
                        (["serve", "--model", model, "extra"], "no argument")):
        code, _, err = run(tool, *args, stdin=b"", cwd=folder, timeout=60)
        check(code == 2 and words in err, f"purebyte {' '.join(args[:3])} ...: refused (got {code}: {err.strip()[:120]})")


def peak_memory(pid):
    """The peak resident memory of a running process in bytes, or None where this test cannot read it."""
    if sys.platform.startswith("linux"):
        with open(f"/proc/{pid}/status", encoding="ascii") as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1]) * 1024
    if os.name == "nt":
        class Counters(ctypes.Structure):
            _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong)] + \
                       [(n, ctypes.c_size_t) for n in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                                                       "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
                                                       "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.restype = ctypes.c_void_p
        psapi = ctypes.WinDLL("psapi")
        psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters), ctypes.c_ulong]
        handle = kernel32.OpenProcess(0x1000 | 0x0010, False, pid)  # query limited information, read memory
        counters = Counters()
        counters.cb = ctypes.sizeof(Counters)
        try:
            if handle and psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                return counters.PeakWorkingSetSize
        finally:
            if handle:
                kernel32.CloseHandle(ctypes.c_void_p(handle))
    return None


def serve_banner(tool, *args):
    """What `purebyte serve ARGS` prints on standard error until it listens (or fails to), then it is stopped."""
    process = subprocess.Popen([tool, "serve", "--quiet", *args], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    lines = []

    def read():
        for line in process.stderr:
            lines.append(line.decode("utf-8", "replace"))
            if "listening on http" in lines[-1]:
                return

    reader = threading.Thread(target=read, daemon=True)
    reader.start()
    reader.join(60)
    process.kill()
    process.wait()
    return "".join(lines)


def test_serve_endpoints(tool, folder, model, quiet_model):
    """/v1/redact has the input checks of /v1/scan (empty input, the model's limit); every parameter belongs to its
    endpoints; other methods get 405; media types are case-insensitive; a chunked body to /v1/decide stops being read
    at its limit; a form body is not parsed into parameters (memory); an empty token is refused; without a token the
    Host check applies off loopback too; the access log cannot be forged; the Bearer scheme is case-insensitive."""
    text = lines_text("serve", 6)
    raw = {"Content-Type": "application/octet-stream"}
    for args, env, what in ((["--token", ""], {}, "--token \"\""),
                            ([], {"PUREBYTE_SERVE_TOKEN": "   "}, "a blank PUREBYTE_SERVE_TOKEN"),
                            ([], {"PUREBYTE_SERVE_TOKEN": ""}, "an empty PUREBYTE_SERVE_TOKEN")):
        if os.name == "nt" and env.get("PUREBYTE_SERVE_TOKEN") == "":
            skip(f"serve refuses {what}", "Windows drops empty environment variables")
            continue
        code, _, err = run(tool, "serve", "--model", model, "--port", str(free_port()), *args,
                           env=dict(os.environ, **env), timeout=60)
        check(code == 2 and "empty" in err, f"serve refuses {what} instead of running without a token (got {code})")
    with Server(tool, "--model", model, "--model", quiet_model, "--profile", "secrets-code", "--bias", "30") as server:
        status, out = server.request("POST", "/v1/redact", text, raw)
        check(status == 200 and out.get("redacted") and out["redacted"] != text.decode() and out["report"]["spans"] and
              "map" not in out, f"serve: /v1/redact returns a redacted copy and its report (got {status})")
        status, out = server.request("POST", "/v1/redact?map=yes", text, raw)
        check(status == 200 and "map" in out, "serve: /v1/redact?map=yes adds the map")
        check(error_code(server.request("POST", "/v1/redact", b"", raw)) == (400, "empty_input"),
              "serve: /v1/redact refuses an empty input (400 empty_input)")
        check(error_code(server.request("POST", "/v1/redact", b"a" * 4000001, raw)) == (413, "too_large"),
              "serve: /v1/redact refuses an input over the model's limit (413 too_large)")
        check(error_code(server.request("POST", "/v1/redact?model=legacy-v2.gguf", text, raw)) ==
              (400, "bad_parameter"), "serve: /v1/redact with a model without typed spans is 400 bad_parameter")
        for path, what in (("/v1/redact?format=sarif", "format on /v1/redact"), ("/v1/scan?map=yes", "map on /v1/scan"),
                           ("/v1/redact?votes=1", "votes on /v1/redact"), ("/v1/decide?format=json", "format on "
                                                                                                    "/v1/decide"),
                           ("/v1/scan?text=abc", "text in the query string")):
            check(error_code(server.request("POST", path, text, raw)) == (400, "bad_parameter"),
                  f"serve: {what} is refused (400 bad_parameter)")
        for method, path, allow in (("PUT", "/v1/scan", "POST"), ("DELETE", "/v1/redact", "POST"),
                                    ("GET", "/v1/decide", "POST"), ("POST", "/health", "GET, HEAD")):
            status, out, headers = server.request(method, path, b"x" if method != "GET" else None,
                                                  raw if method != "GET" else {}, with_headers=True)
            check((status, (out or {}).get("error", {}).get("code"), headers.get("Allow")) ==
                  (405, "method_not_allowed", allow), f"serve: {method} {path} is 405 with Allow: {allow} (got {status})")
        body = json.dumps({"text": text.decode()})
        status, out = server.request("POST", "/v1/scan", body, {"Content-Type": "Application/JSON; charset=UTF-8"})
        check(status == 200 and out["results"][0]["bytes"] == len(text),
              "serve: `Application/JSON` is JSON: the text is scanned, not the JSON document")
        status, out = server.request("POST", "/v1/scan", body, {"Content-Type": "application/jsonx"})
        check(status == 200 and out["results"][0]["bytes"] == len(body), "serve: `application/jsonx` is not JSON")
    with Server(tool, "--model", model, quiet=False) as server:
        check(error_code(server.request("POST", "/v1/decide", b"x" * 200000, raw, chunked=True)) ==
              (413, "too_large_for_decide"), "serve: a chunked body over the limit of /v1/decide is refused")
        distinct = b"&".join(b"k%07d=v" % i for i in range(3 << 20))  # about 34 MiB of form fields
        status, out = server.request("POST", "/v1/scan?model=nope", distinct,
                                     {"Content-Type": "application/x-www-form-urlencoded", "X-PureByte": "1"})
        peak = peak_memory(server.process.pid)
        check(status == 404 and out["error"]["code"] == "unknown_model", "serve: a large form body is read as input")
        if peak is None:
            skip("a form body costs its size in memory, not ten times it", "cannot read the peak memory here")
        else:
            check(peak < 5 * len(distinct) + (64 << 20),
                  f"serve: a form body is not parsed into parameters ({peak >> 20} MiB at most for "
                  f"{len(distinct) >> 20} MiB of body)")
        status, _ = server.request("GET", "/x%0APOST%20/v1/scan%20-%3E%20200%20(1%20B%20in,%202%20B%20out)")
        log = server.stop()
        check(status == 404 and not any(line.startswith("POST /v1/scan -> 200 (1 B in") for line in log.splitlines())
              and "\\x0a" in log, "serve: the access log escapes control characters: a request cannot forge a line")
        check(f"POST /v1/scan -> 404 ({len(distinct)} B in" in log, "serve: the access log counts the bytes read")
        decide = [line for line in log.splitlines() if line.startswith("POST /v1/decide -> 413 (")]
        check(decide and int(decide[0].split("(")[1].split()[0]) <= (64 << 10) + (16 << 10),
              f"serve: /v1/decide stops reading a chunked body at its limit (got {decide})")
    with Server(tool, "--model", model, env=dict(os.environ, PUREBYTE_SERVE_TOKEN="t0ken-from-env")) as server:
        check(server.request("GET", "/v1/models", None, {"Authorization": "bearer t0ken-from-env"})[0] == 200,
              "serve: the Bearer scheme is case-insensitive")
    banner = serve_banner(tool, "--model", model, "--host", "::1", "--port", str(free_port()))
    if "cannot listen" in banner:
        skip("serve --host ::1 writes its URL with brackets", "no IPv6 loopback here")
    else:
        check("listening on http://[::1]:" in banner and "no token" in banner,
              f"serve --host ::1: the URL has brackets, and the banner says there is no token (got {banner.strip()})")
    if not sys.platform.startswith("linux"):
        skip("the Host check applies off loopback without a token", "binds 0.0.0.0: Linux only")
        return
    with Server(tool, "--model", model, "--host", "0.0.0.0") as server:
        check(error_code(server.request("GET", "/v1/models", None, {"Host": "rebind.example"})) == (403, "bad_host"),
              "serve --host 0.0.0.0 without a token: a request for another name is refused (DNS rebinding)")
        check(server.request("GET", "/v1/models", None, {"Host": f"localhost:{server.port}"})[0] == 200,
              "serve --host 0.0.0.0 without a token: a request for localhost is answered")
    with Server(tool, "--model", model, "--host", "0.0.0.0", "--allow-host", "scanner.internal") as server:
        check(server.request("GET", "/v1/models", None, {"Host": f"scanner.internal:{server.port}"})[0] == 200,
              "serve --allow-host: that name is answered")
    with Server(tool, "--model", model, "--host", "0.0.0.0",
                env=dict(os.environ, PUREBYTE_SERVE_TOKEN="t0ken-from-env")) as server:
        check(server.request("GET", "/v1/models", None, {"Host": "scanner.example",
                                                         "Authorization": "Bearer t0ken-from-env"})[0] == 200,
              "serve --host 0.0.0.0 with a token: any name, with the token")


def test_github_action(folder):
    """integrations/github-action: scan.sh compares with the base of the event (a pull request's base, a merge group's
    base in a merge queue, the commit before a push), refuses pull_request_target and --reveal when the report is
    uploaded; install.sh installs by default the version of the checkout it runs from. Fake `purebyte` and `curl`
    programs record what the scripts run."""
    bash, real_git = shutil.which("bash"), shutil.which("git")
    if os.name == "nt" or not bash or not real_git:
        skip("the GitHub Action's scripts", "needs bash and git (POSIX)")
        return
    root = fresh(os.path.join(folder, "action"))
    record = os.path.join(root, "record.txt")
    fake = write(os.path.join(root, "bin", "purebyte"), b"""#!/bin/sh
printf '%s\\n' "$@" > "$RECORD"
out=""; previous=""
for a in "$@"; do [ "$previous" = --out ] && out=$a; previous=$a; done
printf '{"version": "2.1.0", "runs": [{"results": [], "invocations": [{}]}]}' > "$out"
""")
    curl = write(os.path.join(root, "bin", "curl"), b"#!/bin/sh\nexit 22\n")
    for program in (fake, curl):
        os.chmod(program, 0o755)
    repo = fresh(os.path.join(root, "repo"))
    git(real_git, repo, "init", "-q")
    write(os.path.join(repo, "app.cfg"), lines_text("app"))
    git(real_git, repo, "add", "app.cfg")
    git(real_git, repo, "commit", "-q", "-m", "base")
    base = subprocess.run([real_git, "-C", repo, "rev-parse", "HEAD"], capture_output=True,
                          check=True).stdout.decode().strip()
    action = os.path.join(common.ROOT, "integrations", "github-action")
    clean = {k: v for k, v in os.environ.items() if not k.startswith(("GITHUB_", "PUREBYTE_", "PR_", "PUSH_",
                                                                       "MERGE_GROUP_"))}

    def scan(event, **env):
        if os.path.exists(record):
            os.remove(record)
        environment = dict(clean, PATH=os.path.join(root, "bin") + os.pathsep + clean.get("PATH", ""), RECORD=record,
                           GITHUB_EVENT_NAME=event, GITHUB_OUTPUT=os.path.join(root, "output.txt"),
                           PUREBYTE_PATH=repo, PUREBYTE_SARIF=os.path.join(root, "report.sarif"), **env)
        done = subprocess.run([bash, os.path.join(action, "scan.sh")], capture_output=True, env=environment,
                              timeout=120)
        args = open(record, encoding="utf-8").read().split("\n") if os.path.exists(record) else []
        return done.returncode, args, done.stdout.decode(errors="replace") + done.stderr.decode(errors="replace")

    for event, env in (("pull_request", {"PR_BASE_SHA": base}), ("merge_group", {"MERGE_GROUP_BASE_SHA": base}),
                       ("push", {"PUSH_BEFORE_SHA": base})):
        code, args, said = scan(event, **env)
        check(code == 0 and "--git-diff" in args and args[args.index("--git-diff") + 1] == base,
              f"action: on {event} the changes since the event's base are scanned (got {code}: {said.strip()[:160]})")
    code, args, _ = scan("merge_group", PR_BASE_SHA=base)
    check(code == 0 and "--git-diff" not in args, "action: a merge group ignores the pull request's base")
    code, args, said = scan("pull_request_target", PR_BASE_SHA=base)
    check(code == 2 and not args and "pull_request_target" in said, "action: pull_request_target is refused")
    code, args, said = scan("push", PUREBYTE_ARGS="--reveal", PUREBYTE_UPLOAD="true")
    check(code == 2 and not args and "--reveal" in said, "action: --reveal is refused when the report is uploaded")
    code, args, _ = scan("push", PUREBYTE_ARGS="--reveal", PUREBYTE_UPLOAD="false")
    check(code == 0 and "--reveal" in args, "action: --reveal is passed on when the report stays on the runner")
    with open(os.path.join(common.ROOT, "CMakeLists.txt"), encoding="utf-8") as f:
        version = next(line.split()[2] for line in f if line.startswith("project(purebyte VERSION "))
    done = subprocess.run([bash, os.path.join(action, "install.sh")], capture_output=True, timeout=120,
                          env=dict(clean, PATH=os.path.join(root, "bin") + os.pathsep + clean.get("PATH", ""),
                                   RUNNER_TEMP=root, RUNNER_OS="Linux", RUNNER_ARCH="X64"))
    check(done.returncode != 0 and f"Downloading purebyte-{version}-linux-x86_64.tar.gz" in done.stdout.decode(),
          f"action: install.sh installs the version of its own checkout by default ({version})")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--purebyte", required=True, help="path of the purebyte binary")
    p.add_argument("--work", required=True)
    args = p.parse_args()
    # Some runs change directory: a path to the binary must not depend on the current one (a bare name stays on PATH).
    tool = os.path.abspath(args.purebyte) if os.path.dirname(args.purebyte) else args.purebyte
    models = common.generate_models(args.work)
    folder = os.path.join(args.work, "cli")
    os.makedirs(folder, exist_ok=True)
    # attention-lookahead has an ungated tag head: it reports spans in any text; legacy-v2 has no tag head.
    spans_model = os.path.join(models, "attention-lookahead.gguf")
    quiet_model = os.path.join(models, "legacy-v2.gguf")
    rng = common.random_models.Rng(99)
    words = ["".join(chr(ord("a") + rng.below(26)) for _ in range(3 + rng.below(9))) for _ in range(400)]
    text = " ".join(words).encode()
    files = {"notes.txt": text, "tiny.txt": b"too short", "config.ini": b"[section]\nkey = " + text[:300] + b"\n"}
    for name, data in files.items():
        common.write_if_changed(os.path.join(folder, name), data)
    notes, tiny, config = (os.path.join(folder, n) for n in ("notes.txt", "tiny.txt", "config.ini"))

    code, out, _ = run(tool, "version")
    check(code == 0 and "1.0.0" in out, "version prints 1.0.0 and exits 0")
    code, _, _ = run(tool)
    check(code == 2, "no command exits 2")
    code, _, err = run(tool, "no-such-command")
    check(code == 2 and "unknown command" in err, "an unknown command exits 2 and says so")

    code, out, _ = run(tool, "info", spans_model)
    check(code == 0 and '"attention"' in out and '"format_version":3' in out.replace(" ", ""), "info describes a model file")

    code, out, err = run(tool, "scan", "--model", spans_model, "--profile", "none", notes, config)
    check(code == 1, f"scan with findings exits 1 (got {code}: {err.strip()[:200]})")
    report = json.loads(out) if out.strip() else {}
    results, findings = report.get("results", []), report.get("findings", [])
    check(len(results) == 2 and len(findings) > 0, "scan --format json: one result per input and findings")
    check(all({"file", "kind", "start", "end", "confidence", "votes", "snippet_masked"} <= set(f) for f in findings),
          "every finding has the fields of spec/OUTPUT.md section 5")
    check(all("snippet" not in f and "context" not in f for f in findings), "no clear value without --reveal")

    code, revealed, _ = run(tool, "scan", "--model", spans_model, "--profile", "none", "--reveal", notes, config)
    clear = [f["snippet"] for f in json.loads(revealed).get("findings", []) if len(f.get("snippet", "")) >= 6]
    check(code == 1 and clear, "--reveal adds the clear values")
    check(not any(value in out for value in clear), "the masked report holds none of the revealed values")

    code, out, _ = run(tool, "scan", "--model", spans_model, "--profile", "none", "--format", "jsonl", notes, config)
    lines = [json.loads(line) for line in out.splitlines() if line.strip()]
    check(code == 1 and len(lines) == 2 and all("findings" in line for line in lines), "jsonl: one line per input")

    code, out, _ = run(tool, "scan", "--model", spans_model, "--profile", "none", "--format", "sarif", notes)
    sarif = json.loads(out) if out.strip() else {}
    run0 = (sarif.get("runs") or [{}])[0]
    check(sarif.get("version") == "2.1.0" and run0.get("tool", {}).get("driver", {}).get("name") == "purebyte",
          "sarif: version 2.1.0, driver purebyte")
    check(all(r["ruleId"].startswith("purebyte/") and r["locations"][0]["physicalLocation"]["region"]["byteLength"] > 0
              for r in run0.get("results", [])) and run0.get("results"), "sarif: one result per finding, with a region")
    check(all(r.get("level") == "error" for r in run0.get("results", [])),
          "sarif: a profile without path rules reports every finding as an error")

    # secrets-code: findings in test, example and documentation paths are warnings; only errors fail a scan (exit 1),
    # or warnings too with --strict. This folder sits under the build's tests/ folder: named by an absolute path, only
    # what is inside it counts.
    project = os.path.abspath(os.path.join(folder, "project"))
    config_text = (b"# settings\nhost = example.internal\nport = 8080\n" + b"user = admin\nmode = fast\n" * 12 +
                   b"note = the quick brown fox jumps over the lazy dog\n")
    for name in ("src/app.cfg", "tests/app.cfg", "docs/guide.md"):
        os.makedirs(os.path.dirname(os.path.join(project, name)), exist_ok=True)
        common.write_if_changed(os.path.join(project, name), config_text)
    secrets = ["scan", "--model", os.path.abspath(spans_model), "--profile", "secrets-code", "--bias", "30"]
    code, out, _ = run(tool, *secrets, project)
    prefix = project.replace(os.sep, "/") + "/"
    seen = {}
    for f in (json.loads(out).get("findings", []) if out.strip() else []):
        seen.setdefault(f["file"][len(prefix):], set()).add(f.get("severity"))
    check(code == 1 and seen == {"src/app.cfg": {"error"}, "tests/app.cfg": {"warning"}, "docs/guide.md": {"warning"}},
          f"secrets-code: errors in src/, warnings in tests/ and docs/ (got {code}, {seen})")
    code, out, _ = run(tool, *secrets, "tests", "docs", cwd=project)
    stats = json.loads(out).get("stats", {}) if out.strip() else {}
    check(code == 0 and stats.get("by_severity", {}).get("warning", 0) > 0 and stats["by_severity"]["error"] == 0,
          "secrets-code: warnings alone exit 0")
    code, _, _ = run(tool, *secrets, "--strict", "tests", "docs", cwd=project)
    check(code == 1, "secrets-code: --strict exits 1 on warnings")
    code, out, _ = run(tool, *secrets, "--format", "sarif", ".", cwd=project)
    levels = {r["locations"][0]["physicalLocation"]["artifactLocation"]["uri"]: r["level"]
              for r in (json.loads(out)["runs"][0]["results"] if out.strip() else [])}
    check(code == 1 and levels == {"src/app.cfg": "error", "tests/app.cfg": "warning", "docs/guide.md": "warning"},
          f"secrets-code sarif: level error or warning by path (got {levels})")
    code, out, _ = run(tool, *secrets, "--tests=no", ".", cwd=project)
    files = [r["file"] for r in json.loads(out).get("results", [])] if out.strip() else []
    check(code == 1 and files == ["./src/app.cfg"], "secrets-code: --tests=no skips test and documentation paths")

    code, out, _ = run(tool, "scan", "--model", spans_model, "--profile", "none", "-", stdin=text)
    check(code == 1 and json.loads(out).get("findings"), "scan - reads standard input")

    code, out, _ = run(tool, "scan", "--model", quiet_model, "--profile", "none", tiny)
    check(code == 0, "an input shorter than a window is clean: exit 0")

    code, _, err = run(tool, "scan", "--model", spans_model, "--profile", "none", os.path.join(folder, "missing.txt"))
    check(code == 2, "a file that cannot be read exits 2")

    code, _, err = run(tool, "scan", "--model", os.path.join(folder, "notes.txt"), notes)
    check(code == 2 and err.strip(), "a model file that is not a model exits 2 with a message")

    test_all_bytes(tool, folder, spans_model)
    test_batch_retry(tool, folder, spans_model)
    test_programs_from_path(tool, folder, spans_model)
    test_literal_paths(tool, folder, spans_model)
    test_links_not_followed(tool, folder, spans_model)
    test_exclusions(tool, folder, spans_model)
    test_serve(tool, folder, spans_model)
    test_model_resolution(tool, folder, spans_model, quiet_model)
    test_catalog_file_names(tool, folder, spans_model)
    test_models_actions(tool, folder, spans_model)
    test_git_changes(tool, folder, spans_model)
    test_hostile_exclusions(tool, folder, spans_model)
    test_outputs(tool, folder, spans_model)
    test_arguments(tool, folder, spans_model)
    test_serve_endpoints(tool, folder, spans_model, quiet_model)
    test_github_action(folder)

    print(f"cli: {'all passed' if not FAILURES else f'{len(FAILURES)} failed'}")
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
