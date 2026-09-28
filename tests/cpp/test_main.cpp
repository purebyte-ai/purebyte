// Runs the registered tests: all of them, those whose name contains ARG, or (with -ARG) all but those.
#include <cstring>
#include <exception>

#include "test.h"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    const bool exclude = filter && filter[0] == '-';
    if (exclude) ++filter;
    int run = 0;
    for (const pbtest::Case& c : pbtest::registry()) {
        if (filter && (std::strstr(c.name, filter) != nullptr) == exclude) continue;
        const int before = pbtest::failures();
        try {
            c.body();
        } catch (const std::exception& e) {
            pbtest::report(c.name, 0, std::string("unexpected exception: ") + e.what());
        } catch (...) {
            pbtest::report(c.name, 0, "unexpected exception");
        }
        std::printf("%s %s\n", pbtest::failures() == before ? "ok  " : "FAIL", c.name);
        std::fflush(stdout);
        ++run;
    }
    std::printf("%d tests, %d failed checks\n", run, pbtest::failures());
    return pbtest::failures() ? 1 : 0;
}
