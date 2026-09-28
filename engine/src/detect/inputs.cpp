#include "detect/inputs.h"

#include <algorithm>

namespace pb::detect {

std::string over_limit(uint64_t size, uint64_t limit) {
    return std::to_string(size) + " bytes, over this model's limit of " + std::to_string(limit) + " bytes: not scanned";
}

Intake take_inputs(const std::vector<Input>& inputs, const Profile& profile, const ArchiveLimits* archives) {
    Intake in;
    const uint64_t limit = profile.max_input_bytes();
    const MemberFilter wanted = [&profile](const std::string& file_name) { return profile.wants_file(file_name); };
    for (size_t k = 0; k < inputs.size(); ++k) {
        const Input& input = inputs[k];
        const std::string& path = input.path.empty() ? input.name : input.path;
        if (archives && looks_like_archive(input.data, input.size)) {
            for (ArchiveMember& m :
                 expand_archive(input.name, input.data, input.size, *archives, wanted, in.failures)) {
                if (m.bytes.size() > limit) {
                    in.failures.push_back({m.name, over_limit(m.bytes.size(), limit)});
                    continue;
                }
                // "outer.zip!/inner/path": the member's name after its archive's.
                in.paths.push_back(path + m.name.substr(std::min(input.name.size(), m.name.size())));
                in.documents.push_back({std::move(m.name), std::move(m.bytes)});
                in.origin.push_back(k);
            }
        } else if (input.size > limit) {
            in.failures.push_back({input.name, over_limit(input.size, limit)});
        } else {
            in.paths.push_back(path);
            in.documents.push_back({input.name, std::vector<uint8_t>(input.data, input.data + input.size)});
            in.origin.push_back(k);
        }
    }
    return in;
}

}  // namespace pb::detect
