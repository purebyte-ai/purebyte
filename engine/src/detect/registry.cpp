// The profiles this library implements. Adding one: a new module with its Profile, and one line here.
#include "detect/profile.h"

namespace pb::detect {

namespace {

struct Entry {
    const char* name;
    const Profile& (*get)();
};

const Entry kProfiles[] = {
    {"none", none_profile},
    {"secrets-code", secrets_code_profile},
    {"secrets-binary", secrets_binary_profile},
    {"redact", redact_profile},
};

}  // namespace

const Profile* find_profile(const std::string& name) {
    for (const Entry& e : kProfiles)
        if (name == e.name) return &e.get();
    return nullptr;
}

std::string known_profiles() {
    std::string names;
    for (const Entry& e : kProfiles) names += (names.empty() ? "" : ", ") + std::string(e.name);
    return names;
}

}  // namespace pb::detect
