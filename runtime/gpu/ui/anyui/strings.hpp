// SPDX-License-Identifier: MIT
// anyui (shared by AnyGBA and AnyPSP; keep the copies identical): the menu's translations,
// read from a tab-separated table. The first line names the columns, "key", then one
// language code per column ("en", "vi"); '#' starts a comment line. Every other line is a
// key and its text in each language. A missing translation falls back to English, a
// missing key to the key itself (labels that come from elsewhere use their English text as
// the key).
#pragma once

#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace anyui {

class Strings {
public:
    Strings() = default;
    // Throws std::runtime_error naming the line for a malformed table.
    explicit Strings(std::string_view tsv) {
        std::size_t pos = 0;
        int number = 0;
        bool header = true;
        while (pos < tsv.size()) {
            std::size_t end = tsv.find('\n', pos);
            if (end == std::string_view::npos) end = tsv.size();
            std::string_view line = tsv.substr(pos, end - pos);
            pos = end + 1;
            ++number;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty() || line.front() == '#') continue;
            std::vector<std::string> cells;
            std::size_t start = 0;
            for (;;) {
                const std::size_t tab = line.find('\t', start);
                cells.emplace_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
                if (tab == std::string_view::npos) break;
                start = tab + 1;
            }
            if (header) {
                if (cells.size() < 2 || cells[0] != "key" || cells[1] != "en") {
                    throw std::runtime_error("strings line " + std::to_string(number) + ": expected 'key<TAB>en<TAB>...'");
                }
                languages_.assign(cells.begin() + 1, cells.end());
                header = false;
                continue;
            }
            if (cells.size() < 2 || cells.size() > languages_.size() + 1 || cells[0].empty()) {
                throw std::runtime_error("strings line " + std::to_string(number) + ": expected a key and 1 to " +
                                         std::to_string(languages_.size()) + " texts");
            }
            if (!table_.emplace(cells[0], std::vector<std::string>(cells.begin() + 1, cells.end())).second) {
                throw std::runtime_error("strings line " + std::to_string(number) + ": duplicate key " + cells[0]);
            }
        }
        if (header) throw std::runtime_error("strings: no header line");
    }

    const std::vector<std::string>& languages() const { return languages_; }
    const std::string& language() const { return languages_.empty() ? kEnglish : languages_[column_]; }
    // An unknown code selects English.
    void set_language(std::string_view code) {
        column_ = 0;
        for (std::size_t i = 0; i < languages_.size(); ++i) {
            if (languages_[i] == code) column_ = i;
        }
    }

    std::string tr(std::string_view key) const {
        const auto it = table_.find(key);
        if (it == table_.end()) return std::string(key);
        const auto& texts = it->second;
        if (column_ < texts.size() && !texts[column_].empty()) return texts[column_];
        return texts.empty() ? std::string(key) : texts[0];
    }

    // tr(key) with {0}, {1}... replaced by `args`.
    std::string fmt(std::string_view key, std::initializer_list<std::string_view> args) const {
        std::string text = tr(key);
        std::size_t i = 0;
        for (std::string_view a : args) {
            const std::string mark = "{" + std::to_string(i++) + "}";
            for (std::size_t at = text.find(mark); at != std::string::npos; at = text.find(mark, at + a.size())) {
                text.replace(at, mark.size(), a);
            }
        }
        return text;
    }

    bool has(std::string_view key) const { return table_.find(key) != table_.end(); }
    // Keys with no text for `code` (for tests: every language is complete).
    std::vector<std::string> missing(std::string_view code) const {
        std::size_t col = languages_.size();
        for (std::size_t i = 0; i < languages_.size(); ++i) {
            if (languages_[i] == code) col = i;
        }
        std::vector<std::string> out;
        for (const auto& [key, texts] : table_) {
            if (col >= texts.size() || texts[col].empty()) out.push_back(key);
        }
        return out;
    }

private:
    inline static const std::string kEnglish = "en";
    std::vector<std::string> languages_;
    std::map<std::string, std::vector<std::string>, std::less<>> table_;
    std::size_t column_ = 0;
};

}  // namespace anyui
