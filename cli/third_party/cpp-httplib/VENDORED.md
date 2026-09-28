# cpp-httplib (vendored)

| | |
|---|---|
| Upstream | https://github.com/yhirose/cpp-httplib |
| Version | v0.57.1 (`CPPHTTPLIB_VERSION` in `httplib.h`) |
| License | MIT, see `LICENSE` in this folder; keep it next to any redistribution of the `purebyte` binary |
| SHA-256 | `httplib.h` 9bd590b04d9a73848f280e01799d0e08692c0ba454c9d362c9b626dce1c23171, `LICENSE` 4b45cbe16d7b71b89ae6127e26e0d90a029198ca5e958ad8e3d0b8bbed364d8b |
| Modifications | None. The CLI configures it with macros before the include, in one place (`cli/http.h`) |
| Used for | The HTTP/1.1 server of `purebyte serve`. No TLS, no compression, no client |

To update: replace both files with an upstream release, update this table, and run the HTTP end-to-end tests.
