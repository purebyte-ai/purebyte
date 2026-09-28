// From a caller's inputs to the documents a profile runs on. Inputs over the profile's size limit are left out; with
// archive expansion, an archive is replaced by the members the profile wants. Whatever is left out is a failure.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "detect/archive.h"
#include "detect/document.h"
#include "detect/profile.h"

namespace pb::detect {

struct Input {
    std::string name;
    const uint8_t* data = nullptr;
    size_t size = 0;
    std::string path;  // where it is in the project scanned, for the profile's path rules; empty = its name
};

struct Intake {
    std::vector<Document> documents;
    std::vector<size_t> origin;      // the index of the input each document comes from
    std::vector<std::string> paths;  // each document's path for the profile's path rules (an archive member's:
                                     // its archive's path, then "!/" and its name inside the archive)
    std::vector<ScanFailure> failures;
};

// `archives` = nullptr: archives are scanned as they are.
Intake take_inputs(const std::vector<Input>& inputs, const Profile& profile, const ArchiveLimits* archives);

// "4100010 bytes, over this model's limit of 4000000 bytes: not scanned" (exact sizes, so the two never look equal).
std::string over_limit(uint64_t size, uint64_t limit);

}  // namespace pb::detect
