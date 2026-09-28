// cpp-httplib (third_party/cpp-httplib, MIT), configured once for the whole CLI. Include this header before any other
// header: on Windows it must bring winsock2.h in before windows.h. The routes that take an input read their bodies
// themselves (http_request.h), so the library never parses a form-encoded body into parameters.
#pragma once

#include "third_party/cpp-httplib/httplib.h"
