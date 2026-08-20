#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct HotwordEntry {
    std::string phrase;
    std::vector<std::string> aliases;
    bool enabled = true;
    std::uint64_t hits = 0;
    float boost = 3.0F;
};

struct CorrectionRule {
    std::string pattern;
    std::string replacement;
    bool enabled = true;
};

struct TextProcessResult {
    std::string text;
    std::vector<std::string> clauses;
    std::vector<std::string> tokens;
    std::vector<std::string> matched_hotwords;
};

class TextProcessor {
public:
    TextProcessor();
    ~TextProcessor();

    TextProcessor(const TextProcessor&) = delete;
    TextProcessor& operator=(const TextProcessor&) = delete;

    void set_hotwords(std::vector<HotwordEntry> entries);
    void set_correction_rules(std::vector<CorrectionRule> rules);
    bool initialize_segmenter(const std::filesystem::path& dictionary_directory, std::string& error);

    bool add_hotword(
        std::string phrase,
        std::vector<std::string> aliases = {});
    bool remove_hotword(std::string_view phrase);
    bool set_hotword_enabled(std::string_view phrase, bool enabled);
    bool add_correction_rule(std::string pattern, std::string replacement);

    [[nodiscard]] const std::vector<HotwordEntry>& hotwords() const;
    [[nodiscard]] const std::vector<CorrectionRule>& correction_rules() const;

    TextProcessResult process(std::string_view text, bool record_hotword_hits = true);

    bool load_hotwords(const std::filesystem::path& path, std::string& error);
    bool save_hotwords(const std::filesystem::path& path, std::string& error) const;
    bool load_correction_rules(const std::filesystem::path& path, std::string& error);
    bool save_correction_rules(const std::filesystem::path& path, std::string& error) const;

    static std::string normalize(std::string_view text);
    static std::string polish_dictation(std::string_view text);
    static std::vector<std::string> split_clauses(std::string_view text);

private:
    struct CompactSegmenter;

    std::vector<HotwordEntry> hotwords_;
    std::vector<CorrectionRule> correction_rules_;
    std::unique_ptr<CompactSegmenter> segmenter_;
};
