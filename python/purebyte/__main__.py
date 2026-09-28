"""The `purebyte` command of the Python package: the native CLI shipped next to the library, run with the same
arguments, so `pip install purebyte` gives exactly the command of the release archives (docs/cli.md).

The CLI is found at PUREBYTE_CLI, else in purebyte/_lib/.
"""
import os
import subprocess
import sys


def cli_path() -> str:
    explicit = os.environ.get("PUREBYTE_CLI")
    if explicit:
        return explicit
    name = "purebyte.exe" if sys.platform == "win32" else "purebyte"
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "_lib", name)


def main() -> int:
    path = cli_path()
    if not os.path.isfile(path):
        sys.stderr.write(f"purebyte: the command-line tool was not found at {path}: reinstall the package\n")
        return 2
    arguments = [path] + sys.argv[1:]
    if sys.platform == "win32":  # no exec on Windows: run it, pass its exit status on
        try:
            return subprocess.call(arguments)
        except KeyboardInterrupt:
            return 130
    os.execv(path, arguments)
    return 2  # not reached


if __name__ == "__main__":
    sys.exit(main())
