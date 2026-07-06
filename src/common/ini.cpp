// src/common/ini.cpp

#include "ini.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace caster::common::ini {

namespace {

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r')) --b;
    return std::string(s.substr(a, b - a));
}

} // namespace

bool Document::parse(std::string_view text) {
    sections_.clear();
    section_order_.clear();
    sections_[""] = Section{};
    section_order_.push_back("");

    std::string current_section = "";
    size_t pos = 0;
    while (pos < text.size()) {
        // Read one line.
        size_t nl = text.find('\n', pos);
        std::string line = std::string(
            text.substr(pos, (nl == std::string_view::npos)
                                 ? std::string_view::npos
                                 : nl - pos));
        pos = (nl == std::string_view::npos) ? text.size() : nl + 1;

        // Strip trailing CR (already handled by trim, but cheap to do here too).
        std::string t = trim(line);
        if (t.empty()) continue;

        // Comment?
        if (t[0] == '#' || t[0] == ';') continue;

        // Section header?
        if (t.front() == '[' && t.back() == ']') {
            current_section = trim(t.substr(1, t.size() - 2));
            if (sections_.find(current_section) == sections_.end()) {
                sections_[current_section] = Section{};
                section_order_.push_back(current_section);
            }
            continue;
        }

        // key=value
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            // Malformed line — skip silently. INI is forgiving.
            continue;
        }
        std::string k = trim(t.substr(0, eq));
        std::string v = trim(t.substr(eq + 1));
        if (k.empty()) continue;

        // Continuation: if value ends with '\', append next line.
        while (!v.empty() && v.back() == '\\' && pos < text.size()) {
            v.pop_back();  // drop the backslash
            size_t nl2 = text.find('\n', pos);
            std::string next = std::string(
                text.substr(pos, (nl2 == std::string_view::npos)
                                     ? std::string_view::npos
                                     : nl2 - pos));
            pos = (nl2 == std::string_view::npos) ? text.size() : nl2 + 1;
            v += trim(next);
        }

        sections_[current_section][k] = v;
    }
    return true;
}

std::string Document::dump() const {
    std::ostringstream os;
    bool first_section = true;
    for (const auto& name : section_order_) {
        const auto it = sections_.find(name);
        if (it == sections_.end()) continue;
        if (it->second.empty()) continue;

        if (!first_section) os << "\n";
        first_section = false;

        if (!name.empty()) {
            os << "[" << name << "]\n";
        }
        for (const auto& [k, v] : it->second) {
            os << k << "=" << v << "\n";
        }
    }
    return os.str();
}

const std::string* Document::get(std::string_view section,
                                 std::string_view key) const {
    auto sit = sections_.find(std::string(section));
    if (sit == sections_.end()) return nullptr;
    auto kit = sit->second.find(std::string(key));
    if (kit == sit->second.end()) return nullptr;
    return &kit->second;
}

void Document::set(std::string_view section,
                   std::string_view key,
                   std::string_view value) {
    auto sit = sections_.find(std::string(section));
    if (sit == sections_.end()) {
        sections_[std::string(section)] = Section{};
        section_order_.push_back(std::string(section));
        sit = sections_.find(std::string(section));
    }
    sit->second[std::string(key)] = std::string(value);
}

std::string Document::getString(std::string_view section,
                                std::string_view key,
                                std::string_view fallback) const {
    const std::string* v = get(section, key);
    return v ? *v : std::string(fallback);
}

long long Document::getInt(std::string_view section,
                           std::string_view key,
                           long long fallback) const {
    const std::string* v = get(section, key);
    if (!v) return fallback;
    try {
        return std::stoll(*v);
    } catch (...) {
        return fallback;
    }
}

bool Document::getBool(std::string_view section,
                       std::string_view key,
                       bool fallback) const {
    const std::string* v = get(section, key);
    if (!v) return fallback;
    std::string lower = *v;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
        return true;
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
        return false;
    return fallback;
}

bool loadFile(const std::string& path, Document& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    return out.parse(ss.str());
}

bool writeFile(const std::string& path, const Document& doc) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        const std::string text = doc.dump();
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) return false;
    }
    // Atomic rename (on POSIX; on Windows replace via std::rename — works
    // when destination doesn't exist or both are on same volume).
    if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
        std::remove(tmp.c_str());
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace caster::common::ini
