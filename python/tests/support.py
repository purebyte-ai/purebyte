"""Shared helpers of the tests: tiny random models generated from the spec (tests/gen, deterministic) and the CLI.

    PUREBYTE_LIBRARY  test the package of this source tree with this shared library (else the installed package)
    PUREBYTE_CLI      the `purebyte` executable to test (else the one inside the package)
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, os.pardir, os.pardir))
if os.environ.get("PUREBYTE_LIBRARY"):
    sys.path.insert(0, os.path.join(HERE, os.pardir))
sys.path.insert(0, os.path.join(ROOT, "tests", "gen"))

try:
    import random_models  # noqa: E402
except ImportError:  # an installed package tested without the repository
    random_models = None

from purebyte.__main__ import cli_path  # noqa: E402

# A model with an ungated span head (one entity type): a large operating bias makes it mark nearly every byte, a
# very negative one marks nothing. Window 80, stride 60.
SPAN_MODEL = "attention-lookahead"
MARK_ALL, MARK_NOTHING = 30.0, -30.0

CLI = cli_path()
HAVE_CLI = os.path.isfile(CLI)


class TempDir:
    """A temporary directory removed at exit (Windows keeps files locked a little longer: errors ignored)."""

    def __init__(self):
        self.path = tempfile.mkdtemp(prefix="purebyte-test-")

    def file(self, relative, data=b""):
        path = os.path.join(self.path, *relative.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as fh:
            fh.write(data)
        return path

    def cleanup(self):
        shutil.rmtree(self.path, ignore_errors=True)


def write_model(directory: TempDir, name: str = SPAN_MODEL) -> str:
    return directory.file(name + ".gguf", random_models.MODELS[name]().tobytes())


# Text that looks like configuration: long enough for several windows of the test model.
TEXT = (b"# settings\nhost = example.internal\nport = 8080\n" + b"user = admin\nmode = fast\n" * 12 +
        b"note = the quick brown fox jumps over the lazy dog\n")
