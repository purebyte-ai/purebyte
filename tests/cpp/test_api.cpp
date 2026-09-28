// The C API contract (pb.h): statuses instead of exceptions, messages that say what happened, option structs that
// only grow, and the engine entry points on the random models of tests/gen.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "crafted_models.h"
#include "purebyte/pb.h"
#include "test.h"

TEST(api_versions_and_names) {
    CHECK(pb_abi_version() == PB_ABI_VERSION);
    CHECK(std::strlen(pb_version()) >= 5);
    CHECK(pb_format_version() == 3);
    CHECK(std::string(pb_status_name(PB_OK)) == "ok");
    CHECK(std::string(pb_status_name(PB_ERR_UNSUPPORTED)) == "unsupported");
    CHECK(std::string(pb_status_name(static_cast<pb_status>(99))) == "unknown");
}

TEST(api_refuses_garbage_models_with_a_message) {
    pb_model* model = nullptr;
    pb_error err;
    const char junk[] = "definitely not a model file, just some bytes";
    CHECK(pb_model_load_memory(junk, sizeof junk, &model, &err) == PB_ERR_FORMAT);
    CHECK(model == nullptr);
    CHECK(err.status == PB_ERR_FORMAT && std::strstr(err.message, "GGUF") != nullptr);
    CHECK(pb_model_load("/this/path/does/not/exist.gguf", &model, &err) == PB_ERR_IO);
    CHECK(pb_model_load(nullptr, &model, nullptr) == PB_ERR_ARGUMENT);  // a null error pointer is allowed
}

TEST(api_option_structs_only_grow) {
    pb_session_options o;
    pb_session_options_init(&o);
    CHECK(o.struct_size == sizeof(pb_session_options) && o.threads == 1 && o.intra_threads == 0);
    pb_session* session = nullptr;
    pb_error err;
    CHECK(pb_session_create(&o, &session, &err) == PB_OK);
    CHECK(pb_session_threads(session) == 1);
    CHECK(std::strlen(pb_session_kernel(session)) > 0);
    pb_session_free(session);

    pb_session_options small = o;
    small.struct_size = 4;
    CHECK(pb_session_create(&small, &session, &err) == PB_ERR_ARGUMENT);
    CHECK(std::strstr(err.message, "struct_size") != nullptr);
    pb_session_options big = o;
    big.struct_size = sizeof(pb_session_options) + 8;
    CHECK(pb_session_create(&big, &session, &err) == PB_ERR_UNSUPPORTED);
    CHECK(pb_session_create(nullptr, &session, &err) == PB_OK);  // NULL options = the defaults
    pb_session_free(session);

    pb_session_options bad = o;
    bad.threads = 0;
    CHECK(pb_session_create(&bad, &session, &err) == PB_ERR_ARGUMENT);
    bad = o;
    bad.kernel = "quantum";
    CHECK(pb_session_create(&bad, &session, &err) == PB_ERR_ARGUMENT);
    CHECK(std::strstr(err.message, "quantum") != nullptr);
}

TEST(api_scan_and_encode_a_model) {
    const std::string path = pbtest::model_path("ssm-causal-ternary");
    if (path.empty()) return;
    pb_model* model = nullptr;
    pb_session* session = nullptr;
    pb_error err;
    CHECK(pb_model_load(path.c_str(), &model, &err) == PB_OK);
    if (!model) return;
    CHECK(pb_model_d_model(model) == 32);
    CHECK(pb_model_head_count(model) == 3);
    CHECK(pb_model_find_head(model, "tag") == 0 && pb_model_find_head(model, "exit") == 2);
    CHECK(pb_model_find_head(model, "ordinal") == -1);
    CHECK(std::strstr(pb_model_describe(model), "\"ssm_v2\"") != nullptr);
    CHECK(pb_session_create(nullptr, &session, &err) == PB_OK);

    std::vector<uint8_t> bytes(200);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>("key = value; "[i % 13]);
    const pb_input input{bytes.data(), bytes.size()};
    pb_scan_options so;
    pb_scan_options_init(&so);
    so.flags = PB_SCAN_DIGEST;
    pb_scan_result* result = nullptr;
    CHECK(pb_scan(session, model, &input, 1, &so, &result, &err) == PB_OK);
    const pb_window* windows = nullptr;
    const size_t n = pb_scan_result_windows(result, 0, &windows);
    CHECK(n == 4);  // 64-byte windows every 48 bytes over 200 bytes
    for (size_t w = 0; w < n; ++w) {
        size_t count = 0;
        const float* p = pb_scan_result_head_values(result, 0, w, 1, &count);
        CHECK(p != nullptr && count == 2);
        if (p) CHECK(std::fabs(p[0] + p[1] - 1.f) < 1e-6f && std::fabs(windows[w].p_positive - (1.f - p[0])) < 1e-7f);
        CHECK(windows[w].digest != 0);
        CHECK(pb_scan_result_head_values(result, 0, w, 7, &count) == nullptr && count == 0);
    }
    CHECK(pb_scan_result_windows(result, 5, &windows) == 0 && windows == nullptr);
    pb_scan_result_free(result);

    so.flags = PB_SCAN_EARLY_EXIT;
    so.struct_size = 12;  // smaller than any version of the struct
    CHECK(pb_scan(session, model, &input, 1, &so, &result, &err) == PB_ERR_ARGUMENT);

    std::vector<float> hidden(bytes.size() * 32);
    CHECK(pb_encode(session, model, bytes.data(), bytes.size(), hidden.data(), hidden.size() - 1, &err) ==
          PB_ERR_BUFFER_TOO_SMALL);
    CHECK(pb_encode(session, model, bytes.data(), bytes.size(), hidden.data(), hidden.size(), &err) == PB_OK);
    pb_session_free(session);
    pb_model_free(model);
}

TEST(api_refuses_what_a_model_cannot_do) {
    const std::string path = pbtest::model_path("legacy-v2");
    if (path.empty()) return;
    pb_model* model = nullptr;
    pb_session* session = nullptr;
    pb_error err;
    CHECK(pb_model_load(path.c_str(), &model, &err) == PB_OK);
    CHECK(pb_session_create(nullptr, &session, &err) == PB_OK);
    if (!model || !session) return;
    const uint8_t bytes[64] = {0};
    const pb_input input{bytes, sizeof bytes};
    pb_scan_options so;
    pb_scan_options_init(&so);
    pb_scan_result* result = nullptr;
    so.flags = PB_SCAN_EARLY_EXIT;
    CHECK(pb_scan(session, model, &input, 1, &so, &result, &err) == PB_ERR_UNSUPPORTED);  // no exit head
    so.flags = 0;
    const char* queries[] = {"name"};
    so.queries = queries;
    so.query_count = 1;
    CHECK(pb_scan(session, model, &input, 1, &so, &result, &err) == PB_ERR_ARGUMENT);  // not a query model
    so.query_count = 0;
    so.window = 64;
    so.stride = 65;
    CHECK(pb_scan(session, model, &input, 1, &so, &result, &err) == PB_ERR_ARGUMENT);  // bytes would be skipped
    pb_session_free(session);
    pb_model_free(model);
}

TEST(api_refuses_values_it_cannot_hold) {
    // A window or stride beyond what the runtime holds is refused, not wrapped into another value; a per-type operating
    // point of the wrong size is refused before any window runs (review R1, findings 23 and 40).
    pb_model* model = nullptr;
    pb_session* session = nullptr;
    pb_error err;
    std::string message;
    CHECK_MSG(pbtest::load_model(pbtest::flag_model(pbtest::is_upper), &model, &message) == PB_OK, message);
    CHECK(pb_session_create(nullptr, &session, &err) == PB_OK);
    if (!model || !session) return;
    const std::string text = "short";
    const pb_input input{reinterpret_cast<const uint8_t*>(text.data()), text.size()};
    pb_scan_options so;
    pb_scan_options_init(&so);
    pb_scan_result* result = nullptr;
    so.window = 0x80000000u;
    CHECK(pb_scan(session, model, &input, 1, &so, &result, &err) == PB_ERR_ARGUMENT);
    so.window = 0;
    so.stride = 0xFFFFFFFFu;
    CHECK(pb_scan(session, model, &input, 1, &so, &result, &err) == PB_ERR_ARGUMENT);
    so.stride = 0;
    const float biases[] = {0.5f, 0.5f};
    so.type_bias = biases;
    so.type_bias_count = 2;  // the tag head has one entity type
    const pb_input empty{nullptr, 0};
    CHECK(pb_scan(session, model, &empty, 1, &so, &result, &err) == PB_ERR_ARGUMENT);
    CHECK_MSG(std::string(err.message).find("2 per-type biases") != std::string::npos, err.message);
    so.type_bias_count = 1;
    CHECK(pb_scan(session, model, &input, 1, &so, &result, &err) == PB_OK);
    pb_scan_result_free(result);
    pb_session_free(session);
    pb_model_free(model);
}

TEST(api_open_errors_say_why) {
    // A path that is not UTF-8 (refused on Windows, where paths are UTF-16) or that does not exist: the message gives
    // the reason, never "No error" (review R1, finding 35).
    pb_model* model = nullptr;
    pb_error err;
    CHECK(pb_model_load("\xff\xfe-no-such-model.gguf", &model, &err) == PB_ERR_IO);
    CHECK_MSG(std::string(err.message).find("No error") == std::string::npos &&
                  std::string(err.message).find("Success") == std::string::npos,
              err.message);
    for (const unsigned char c : std::string(err.message)) CHECK(c >= 0x20 && c != 0x7f);
}
