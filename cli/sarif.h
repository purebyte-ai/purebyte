// SARIF 2.1.0 (https://docs.oasis-open.org/sarif/sarif/v2.1.0/) for code scanning dashboards: one result per
// finding, one rule per finding kind, and the inputs that could not be scanned as tool notifications. Values are
// masked unless the caller revealed them.
#pragma once

#include <string>
#include <vector>

#include "core/json.h"
#include "inputs.h"

namespace cli {

struct SarifFinding {
    const pb::json::Value* finding = nullptr;  // an object of the `findings` list
    long long column = 0;                      // 1-based column in Unicode code points; 0 when unknown
};

std::string render_sarif(const std::vector<SarifFinding>& findings, const std::vector<Failure>& failures,
                         const std::string& model_name, const std::string& model_version, bool reveal);

// A path as a SARIF artifact URI: relative paths stay relative (with '/'), absolute ones become file:// URIs.
std::string artifact_uri(const std::string& path);

}  // namespace cli
