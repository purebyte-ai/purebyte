"""The exceptions of the package. Every error it raises is a PureByteError, whose `status` names the pb_status of the
C API ("argument", "io", "format", "unsupported", ...). The errors found by the Python code itself are also instances
of the built-in exception that describes them (ValueError, LookupError, FileNotFoundError), so a handler of either kind
catches them."""


class PureByteError(Exception):
    """A failed call. `status` is the pb_status name ("argument", "io", "format", "unsupported", ...)."""

    def __init__(self, status: str, message: str):
        super().__init__(message)
        self.status = status


class ArgumentError(PureByteError, ValueError):
    """An argument the call cannot take, such as an input over a limit (status "argument")."""

    def __init__(self, message: str):
        super().__init__("argument", message)


class UnknownModelError(PureByteError, LookupError):
    """A model name the catalog does not list (status "argument")."""

    def __init__(self, message: str):
        super().__init__("argument", message)


class ModelNotInstalledError(PureByteError, FileNotFoundError):
    """A specialist of the catalog whose files are not in the models directory (status "io")."""

    def __init__(self, message: str):
        super().__init__("io", message)


class ModelFileError(PureByteError, ValueError):
    """A model file or a catalog that cannot be used: a checksum that differs, an unknown schema, a file name that is
    not a plain name (status "format"), or a specialist that needs a newer runtime (status "unsupported")."""

    def __init__(self, message: str, status: str = "format"):
        super().__init__(status, message)
