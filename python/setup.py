"""Builds the wheel of the `purebyte` package: the Python bindings plus the native library and CLI, compiled with CMake
from the repository this directory belongs to (so build from a checkout: `pip wheel python/` or `python -m build
python/`).

    PUREBYTE_PREBUILT=DIR   take purebyte.dll / libpurebyte.so / libpurebyte.dylib and the `purebyte` CLI from DIR
                            (e.g. a CMake build directory) instead of building them
    CMAKE_GENERATOR, CMAKE_BUILD_PARALLEL_LEVEL, ...   honored by the CMake build, as usual

The wheel is tagged py3-none-<platform>: it holds no Python extension, only a C library loaded through ctypes.
"""
import glob
import os
import shutil
import subprocess
import sys

from setuptools import setup
from setuptools.command.build_py import build_py
from setuptools.dist import Distribution

try:
    from setuptools.command.bdist_wheel import bdist_wheel
except ImportError:  # setuptools < 70.1
    from wheel.bdist_wheel import bdist_wheel

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The license of the package's metadata (pyproject.toml, license-files) must sit next to pyproject.toml: a copy of the
# repository's, made for the build (python/LICENSE is ignored by git).
shutil.copyfile(os.path.join(ROOT, "LICENSE"), os.path.join(HERE, "LICENSE"))

if sys.platform == "win32":
    LIBRARY, LIBRARY_PATTERNS, CLI = "purebyte.dll", ["purebyte.dll"], "purebyte.exe"
elif sys.platform == "darwin":
    LIBRARY, LIBRARY_PATTERNS, CLI = "libpurebyte.dylib", ["libpurebyte.*.*.*.dylib", "libpurebyte.dylib"], "purebyte"
else:
    LIBRARY, LIBRARY_PATTERNS, CLI = "libpurebyte.so", ["libpurebyte.so.*.*.*", "libpurebyte.so"], "purebyte"


def find(directory, patterns):
    """The first file matching one of `patterns` in `directory` or below it (build trees differ by generator)."""
    for pattern in patterns:
        for match in sorted(glob.glob(os.path.join(directory, "**", pattern), recursive=True)):
            if os.path.isfile(match) and not os.path.islink(match):
                return match
    raise RuntimeError(f"{patterns[0]} was not found under {directory}")


def native_build(temp):
    prebuilt = os.environ.get("PUREBYTE_PREBUILT")
    if prebuilt:
        return prebuilt
    build = os.path.join(temp, "native")
    subprocess.check_call(["cmake", "-S", ROOT, "-B", build, "-DCMAKE_BUILD_TYPE=Release", "-DPUREBYTE_BUILD_TESTS=OFF",
                           "-DPUREBYTE_STATIC_RUNTIME=ON"])
    subprocess.check_call(["cmake", "--build", build, "--config", "Release", "--parallel"])
    return build


class BuildWithNative(build_py):
    """build_py, plus the native library, the CLI and the model catalog copied into the package."""

    def run(self):
        super().run()
        build = native_build(os.path.abspath(self.get_finalized_command("build").build_temp))
        lib_dir = os.path.join(self.build_lib, "purebyte", "_lib")
        data_dir = os.path.join(self.build_lib, "purebyte", "_data")
        os.makedirs(lib_dir, exist_ok=True)
        os.makedirs(data_dir, exist_ok=True)
        shutil.copyfile(find(build, LIBRARY_PATTERNS), os.path.join(lib_dir, LIBRARY))
        cli = os.path.join(lib_dir, CLI)
        shutil.copyfile(find(build, [CLI]), cli)
        os.chmod(cli, 0o755)
        shutil.copyfile(os.path.join(ROOT, "models", "models.json"), os.path.join(data_dir, "models.json"))
        # Next to the binaries, as in the release archives: the license, and those of the vendored libraries.
        shutil.copyfile(os.path.join(ROOT, "LICENSE"), os.path.join(lib_dir, "LICENSE"))
        third_party = os.path.join(ROOT, "cli", "third_party")
        for library in sorted(os.listdir(third_party)):
            if os.path.isfile(os.path.join(third_party, library, "LICENSE")):
                shutil.copyfile(os.path.join(third_party, library, "LICENSE"),
                                os.path.join(lib_dir, "LICENSE-" + library))


class PlatformWheel(bdist_wheel):
    def finalize_options(self):
        super().finalize_options()
        self.root_is_pure = False

    def get_tag(self):
        return ("py3", "none") + (super().get_tag()[2],)


class BinaryDistribution(Distribution):
    def has_ext_modules(self):
        return True


setup(cmdclass={"build_py": BuildWithNative, "bdist_wheel": PlatformWheel}, distclass=BinaryDistribution)
