// SPDX-License-Identifier: GPL-2.0-or-later

#include "ge_interp.hpp"

#include <algorithm>

namespace pspweb {

void FrameMatcher::next_frame() {
    std::swap(previous_, current_);
    current_.clear();
    seen_.clear();
    cursor_ = 0;
    ++totals_.frames;
}

const float *FrameMatcher::match(const PrimKey &key, std::span<const float> xyzw) {
    ++totals_.prims;
    const std::size_t occurrence = seen_[key]++;
    current_.by_key[key].push_back(current_.records.size());
    current_.records.push_back(Record{key, current_.pool.size(), false});
    current_.pool.insert(current_.pool.end(), xyzw.begin(), xyzw.end());

    auto &records = previous_.records;
    std::size_t found = records.size();
    if (const auto it = previous_.by_key.find(key); it != previous_.by_key.end() && occurrence < it->second.size()) {
        const std::size_t i = it->second[occurrence];
        if (!records[i].used) found = i;
    }
    if (found == records.size()) {
        const std::size_t end = std::min(records.size(), cursor_ + kSearch);
        for (std::size_t i = cursor_; i < end; ++i) {
            if (!records[i].used && records[i].key.same_shape(key)) {
                found = i;
                break;
            }
        }
    }
    if (found == records.size()) return nullptr;
    Record &partner = records[found];
    partner.used = true;
    cursor_ = found + 1u;
    if (previous_.pool.size() < partner.at + xyzw.size()) return nullptr;
    ++totals_.matched;
    return previous_.pool.data() + partner.at;
}

} // namespace pspweb
