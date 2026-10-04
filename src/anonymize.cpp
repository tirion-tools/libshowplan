// SPDX-License-Identifier: MIT
#include "showplan/anonymize.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace showplan {

namespace {

std::string lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back((c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c);
    return out;
}

}  // namespace

std::string AnonymizeMapper::ensure(std::string_view name) {
    if (name.empty()) return {};
    std::string key = lower(name);
    auto it = by_name_.find(key);
    if (it != by_name_.end()) return it->second;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Other_%d", ++next_other_);
    std::string replacement = buf;
    by_name_.emplace(std::move(key), replacement);
    return replacement;
}

}  // namespace showplan
