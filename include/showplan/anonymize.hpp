// SPDX-License-Identifier: MIT
// Identifier name map for anonymising ShowPlan-related surfaces.
//
// Assigns each distinct identifier (case-insensitive) a deterministic
// placeholder ("Other_1", "Other_2", ...) in first-seen order, so a
// caller can apply the same names consistently across plan XML, SQL
// text and other surfaces it rewrites itself.
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace showplan {

class AnonymizeMapper {
public:
    // Original (lowercased) identifier -> placeholder.
    const std::unordered_map<std::string, std::string>& mapping() const {
        return by_name_;
    }

    // Ensure `name` has a placeholder, creating one ("Other_N") if
    // missing, and return it. Empty input returns empty.
    std::string ensure(std::string_view name);

private:
    std::unordered_map<std::string, std::string> by_name_;
    int next_other_ = 0;
};

}  // namespace showplan
