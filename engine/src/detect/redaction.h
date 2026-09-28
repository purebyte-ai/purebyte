// Redaction BY COPY. The output is the input, byte for byte, except the detected spans, each replaced by a typed
// marker such as [EMAIL_1]. Nothing is generated, so nothing can be invented: the only possible errors are a span that
// was missed or a span marked in excess.
//
//   * Markers are consistent pseudonyms: the same value, after a per-type normalisation (an e-mail in any case, a
//     phone with or without its international prefix, an IBAN with or without spaces, a name with or without accents),
//     gets the same marker everywhere in the document, and two different values never share one: a phone number
//     keeps all its digits, and a name in any script keeps its letters. Numbering follows the order of appearance and
//     starts after the highest [TYPE_n] already present in the input, so an existing marker is never reused.
//   * The copy property (every byte outside the spans is in the output, in order and unchanged) is checked before
//     the result is returned.
//   * The report never holds the redacted values, nor their hashes (a short value is brute-forceable from its hash).
//     The reversible map does hold them; it is built only on request.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pb::detect {

struct RedactionSpan {
    int64_t start = 0, end = 0;  // bytes of the input; out-of-range and empty spans are ignored
    std::string type;            // e.g. EMAIL
    float confidence = 0.f;
    std::string source = "model";
};

struct RedactionRequest {
    std::string input_name;          // for the report
    std::string model_name;          // for the report (may be empty)
    std::vector<std::string> types;  // type names in the model's order (ties in span resolution go to the earlier)
    bool with_map = false;
};

struct Redaction {
    std::vector<uint8_t> output;
    std::string report_json;  // {"input", "model", "bytes_in", "bytes_out", "sha256_in", "sha256_out", "spans",
                              // "counts", "guarantee"}
    std::string
        map_json;  // {"version": 1, "input", "sha256_out", "spans": [{"marker", "out_start", "out_end", "value_b64"}]}
};

// Spans may come from a model, rules or any other source, in any order, and may overlap: they are resolved first
// (detect/spans.h: same type overlapping -> union; different types -> the better span keeps the overlap).
Redaction redact(const std::vector<uint8_t>& input, const std::vector<RedactionSpan>& spans,
                 const RedactionRequest& request);

// The same for a UTF-16 input (with its byte order mark) that a profile read as `text`, its UTF-8 conversion
// (detect/text.h, utf16_to_utf8), and in which it found `spans`: the redaction is a copy of the UTF-16 bytes. Each span
// covers the whole characters it touches, the markers are written in UTF-16 of the input's byte order, and the report
// and the map (both with "encoding": "utf-16le" or "utf-16be") give offsets in the UTF-16 bytes.
Redaction redact_converted(const std::vector<uint8_t>& original, const std::vector<uint8_t>& text,
                           const std::vector<RedactionSpan>& spans, const RedactionRequest& request);

// The original bytes from a redacted output and its map, by offsets (never by searching for markers, so a marker-like
// text in the input is harmless). Throws Failure(PB_ERR_ARGUMENT) when the map is not one pb_redact writes (no
// `version`, no `sha256_out`, an offset that is not an integer within the output) or when the output was edited after
// redaction (its SHA-256 is not the map's `sha256_out`, or a marker is not where the map says), and
// Failure(PB_ERR_UNSUPPORTED) for a map version other than 1.
std::vector<uint8_t> restore(const std::vector<uint8_t>& redacted, const std::string& map_json);

// The normalised value that decides whether two spans of one type get the same marker.
std::string normalize_value(const std::string& type, const uint8_t* value, size_t size);

}  // namespace pb::detect
