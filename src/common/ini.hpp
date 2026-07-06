// src/common/ini.hpp
//
// Tiny INI parser/writer used by config.cpp (and later by controller_mapper.cpp
// for mapping.ini). Single-section support is enough for our use cases.
//
// Format supported:
//   - `key=value` and `key = value` (whitespace trimmed on both sides)
//   - `[section]` headers (preserved; getSection/setSection let you scope keys)
//   - `#` and `;` line comments
//   - Multi-line values via trailing `\` (continuation)
//   - Blank lines ignored
//
// Not supported:
//   - Quoted values with embedded `=` (use escapes if you really need them)
//   - Duplicate keys in the same section (last one wins)
//
// All paths are 8-bit; we don't deal with Unicode INI files.

#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace caster::common::ini {

// A section is just a case-sensitive ordered map of key→string.
// We use std::map (not unordered_map) so writeFile() produces stable output.
using Section = std::map<std::string, std::string>;

// In-memory INI document. Top-level keys (before any [section]) go into
// the empty-string section "".
class Document {
public:
    Document() = default;

    // Parse text into this document. Returns false on syntax error (but
    // still consumes as much as it could — caller decides what to do).
    // Idempotent: clears existing content first.
    bool parse(std::string_view text);

    // Render this document back to INI text. Stable order: sections in
    // insertion order, keys alphabetically within each section.
    std::string dump() const;

    // Get/set, scoped to a section. Empty section name = top-level.
    // Getters return nullptr if the key doesn't exist.
    const std::string* get(std::string_view section,
                           std::string_view key) const;
    void set(std::string_view section,
             std::string_view key,
             std::string_view value);

    // Convenience: typed getters with default fallback.
    std::string getString(std::string_view section,
                          std::string_view key,
                          std::string_view fallback) const;
    long long   getInt   (std::string_view section,
                          std::string_view key,
                          long long fallback) const;
    bool        getBool  (std::string_view section,
                          std::string_view key,
                          bool fallback) const;

    // Returns true if the file was empty / didn't exist (caller can use
    // this to fall back to defaults on first run).
    bool empty() const { return sections_.empty() ||
                                (sections_.size() == 1 &&
                                 sections_.at("").empty()); }

private:
    // Section name → ordered map of keys.
    // We preserve insertion order of sections via a parallel vector.
    std::map<std::string, Section> sections_;
    std::vector<std::string>       section_order_;
};

// Convenience: load a Document from disk. Returns false if file missing
// or unreadable. Missing file is NOT an error — caller typically wants
// to fall back to defaults and write a fresh file.
bool loadFile(const std::string& path, Document& out);

// Write a Document to disk atomically: write to `path.tmp` then rename.
// Returns false on I/O error.
bool writeFile(const std::string& path, const Document& doc);

} // namespace caster::common::ini
