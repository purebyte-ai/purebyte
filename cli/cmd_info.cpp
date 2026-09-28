// purebyte info [MODEL] and purebyte version.
#include <cstdio>
#include <thread>

#include "args.h"
#include "catalog.h"
#include "commands.h"
#include "core/json.h"
#include "engine.h"

namespace cli {

int command_info(const std::vector<std::string>& argv) {
    const std::vector<OptionSpec> spec = {{"help", nullptr, "show this help"}};
    const Args a(argv, 2, spec);
    if (a.has("help")) {
        std::fputs(
            "usage: purebyte info [MODEL]\n\nThe runtime (version, CPU kernel, threads, models directory), or what a model "
            "file declares (window, blocks, heads, labels, profile) as JSON.\n",
            stdout);
        return 0;
    }
    if (a.positional().size() > 1) throw UsageError("info takes at most one model");
    pb::json::Writer w;
    if (a.positional().empty()) {
        const std::shared_ptr<pb_session> session = make_session(1, 0, "auto");
        w.begin_object()
            .field("purebyte", pb_version())
            .field("abi", static_cast<int64_t>(pb_abi_version()))
            .field("model_format", static_cast<int64_t>(pb_format_version()))
            .field("kernel", pb_session_kernel(session.get()))
            .field("cores", static_cast<int64_t>(std::thread::hardware_concurrency()))
            .field("default_threads", default_threads())
            .key("models_directory");
        try {
            w.string(display_path(models_directory()));
        } catch (const UsageError& e) {  // no PUREBYTE_HOME and no per-user directory: null, and why
            w.null();
            std::fprintf(stderr, "purebyte: %s\n", e.what());
        }
        w.end_object();
    } else {
        const Specialist s = load_specialist(a.positional()[0], {}, true);
        w.begin_object().field("name", s.name).field("version", s.version).key("files").begin_array();
        for (const ModelFile& m : s.members)
            w.begin_object()
                .field("path", m.path)
                .field("size", m.size)
                .field("sha256", m.sha256)
                .key("declaration")
                .raw(pb_model_describe(m.handle.get()))
                .end_object();
        w.end_array().end_object();
    }
    std::printf("%s\n", w.str().c_str());
    return 0;
}

int command_version(const std::vector<std::string>& argv) {
    const std::vector<OptionSpec> spec = {{"help", nullptr, "show this help"}};
    const Args a(argv, 2, spec);
    if (a.has("help")) {
        std::fputs("usage: purebyte version\n\nPrints the version of purebyte.\n", stdout);
        return 0;
    }
    if (!a.positional().empty()) throw UsageError("version takes no argument");
    std::printf("purebyte %s\n", pb_version());
    return 0;
}

}  // namespace cli
