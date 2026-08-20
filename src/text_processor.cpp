#include "text_processor.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace {

constexpr std::string_view number_token = "{num}";

bool is_ascii_space(unsigned char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool is_ascii_punctuation(unsigned char value) {
    return value == ',' || value == '.' || value == '?' || value == '!' ||
        value == ':' || value == ';';
}

bool is_sentence_boundary(std::string_view value, std::size_t position, std::size_t& length) {
    static constexpr std::string_view boundaries[] = {
        "。", "！", "？", "；", "!", "?", ";", "\n",
    };
    for (const std::string_view boundary : boundaries) {
        if (value.substr(position, boundary.size()) == boundary) {
            length = boundary.size();
            return true;
        }
    }
    return false;
}

bool is_clause_boundary(std::string_view value, std::size_t position, std::size_t& length) {
    if (is_sentence_boundary(value, position, length)) {
        return true;
    }
    static constexpr std::string_view boundaries[] = {"，", "、", ","};
    for (const std::string_view boundary : boundaries) {
        if (value.substr(position, boundary.size()) == boundary) {
            length = boundary.size();
            return true;
        }
    }
    return false;
}

std::string trim_ascii_space(std::string value);
std::size_t next_char_boundary(std::string_view text, std::size_t start);

bool is_terminal_punctuation(std::string_view value) {
    static constexpr std::string_view punctuation[] = {
        "。", "！", "？", "；", ".", "!", "?", ";",
    };
    return std::any_of(std::begin(punctuation), std::end(punctuation), [&](std::string_view item) {
        return value.ends_with(item);
    });
}

std::string strip_terminal_punctuation(std::string value) {
    static constexpr std::string_view punctuation[] = {
        "。", "！", "？", "；", ".", "!", "?", ";",
    };
    for (const std::string_view item : punctuation) {
        if (value.ends_with(item)) {
            value.resize(value.size() - item.size());
            break;
        }
    }
    return trim_ascii_space(std::move(value));
}

bool is_utf8_lead(unsigned char value) {
    return value >= 0xC0U;
}

std::string normalize_dictation_punctuation(std::string_view text) {
    std::string output;
    output.reserve(text.size() + 4);
    for (std::size_t position = 0; position < text.size();) {
        const unsigned char value = static_cast<unsigned char>(text[position]);
        if (value >= 0x80U) {
            const std::size_t next = next_char_boundary(text, position);
            output.append(text.substr(position, next - position));
            position = next;
            continue;
        }
        const bool previous_is_chinese = !output.empty() &&
            (static_cast<unsigned char>(output.back()) & 0xC0U) == 0x80U;
        const bool next_is_chinese = position + 1 < text.size() &&
            is_utf8_lead(static_cast<unsigned char>(text[position + 1]));
        const bool chinese_context = previous_is_chinese || next_is_chinese;
        const bool decimal_point = value == '.' && position > 0 && position + 1 < text.size() &&
            std::isdigit(static_cast<unsigned char>(text[position - 1])) != 0 &&
            std::isdigit(static_cast<unsigned char>(text[position + 1])) != 0;
        if (chinese_context && !decimal_point) {
            switch (value) {
            case ',': output += "，"; break;
            case '.': output += "。"; break;
            case '?': output += "？"; break;
            case '!': output += "！"; break;
            case ':': output += "："; break;
            case ';': output += "；"; break;
            default: output.push_back(static_cast<char>(value)); break;
            }
        } else {
            output.push_back(static_cast<char>(value));
        }
        ++position;
    }
    return output;
}

std::string remove_pause_fillers(std::string text) {
    static constexpr std::string_view fillers[] = {
        "嗯嗯，", "呃呃，", "嗯，", "呃，", "额，",
        "，嗯，", "，呃，", "，额，",
    };
    for (const std::string_view filler : fillers) {
        std::size_t position = 0;
        while ((position = text.find(filler, position)) != std::string::npos) {
            const bool has_leading_comma = filler.starts_with("，");
            text.replace(position, filler.size(), has_leading_comma ? "，" : "");
            if (position > 0) --position;
        }
    }
    return text;
}

std::vector<std::string> split_sentences(std::string_view text) {
    std::vector<std::string> sentences;
    std::size_t start = 0;
    for (std::size_t position = 0; position < text.size();) {
        std::size_t boundary_length = 0;
        if (is_sentence_boundary(text, position, boundary_length)) {
            const std::size_t end = position + boundary_length;
            std::string sentence = trim_ascii_space(std::string(text.substr(start, end - start)));
            if (!sentence.empty()) sentences.push_back(std::move(sentence));
            start = end;
            position = end;
            continue;
        }
        position = next_char_boundary(text, position);
    }
    std::string tail = trim_ascii_space(std::string(text.substr(start)));
    if (!tail.empty()) sentences.push_back(std::move(tail));
    return sentences;
}

std::string trim_ascii_space(std::string value) {
    std::size_t first = 0;
    while (first < value.size() && is_ascii_space(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && is_ascii_space(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return value.substr(first, last - first);
}

bool is_number_char(char value) {
    return (value >= '0' && value <= '9');
}

std::size_t next_char_boundary(std::string_view text, std::size_t start) {
    if (start >= text.size()) {
        return text.size();
    }
    const unsigned char value = static_cast<unsigned char>(text[start]);
    if ((value & 0x80U) == 0) {
        return start + 1;
    }
    if ((value & 0xE0U) == 0xC0U) {
        return std::min(text.size(), start + 2);
    }
    if ((value & 0xF0U) == 0xE0U) {
        return std::min(text.size(), start + 3);
    }
    return std::min(text.size(), start + 4);
}

std::string apply_num_rule(
    std::string_view text,
    std::string_view pattern,
    std::string_view replacement) {
    const std::size_t token_position = pattern.find(number_token);
    if (token_position == std::string_view::npos ||
        pattern.find(number_token, token_position + number_token.size()) !=
            std::string_view::npos) {
        return std::string(text);
    }
    const std::string_view prefix = pattern.substr(0, token_position);
    const std::string_view suffix = pattern.substr(token_position + number_token.size());
    if (prefix.empty() && suffix.empty()) {
        return std::string(text);
    }

    std::string output;
    output.reserve(text.size());
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        const std::size_t prefix_position = prefix.empty()
            ? text.find_first_of("0123456789", cursor)
            : text.find(prefix, cursor);
        if (prefix_position == std::string_view::npos) {
            break;
        }
        const std::size_t number_start = prefix.empty()
            ? prefix_position
            : prefix_position + prefix.size();
        std::size_t number_end = number_start;
        while (number_end < text.size() && is_number_char(text[number_end])) {
            ++number_end;
        }
        if (number_end == number_start || text.substr(number_end, suffix.size()) != suffix) {
            const std::size_t next = next_char_boundary(text, prefix_position);
            output.append(text.substr(cursor, next - cursor));
            cursor = next;
            continue;
        }
        output.append(text.substr(cursor, prefix_position - cursor));
        std::string substituted(replacement);
        const std::size_t replacement_token = substituted.find(number_token);
        if (replacement_token != std::string::npos) {
            substituted.replace(
                replacement_token,
                number_token.size(),
                text.substr(number_start, number_end - number_start));
        }
        output += substituted;
        cursor = number_end + suffix.size();
    }
    output.append(text.substr(cursor));
    return output;
}

std::string apply_rule(std::string_view text, const CorrectionRule& rule) {
    const std::string pattern = trim_ascii_space(rule.pattern);
    if (pattern.empty() || rule.replacement.find(number_token) != std::string::npos &&
        pattern.find(number_token) == std::string::npos) {
        return std::string(text);
    }
    if (pattern.find(number_token) == std::string::npos) {
        std::string output(text);
        std::size_t position = 0;
        while ((position = output.find(pattern, position)) != std::string::npos) {
            output.replace(position, pattern.size(), rule.replacement);
            position += rule.replacement.size();
        }
        return output;
    }
    return apply_num_rule(text, pattern, rule.replacement);
}

bool looks_ascii_only(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x80;
    });
}

bool ascii_equal_at(std::string_view value, std::size_t position, std::string_view needle) {
    if (position + needle.size() > value.size()) {
        return false;
    }
    for (std::size_t index = 0; index < needle.size(); ++index) {
        const auto left = static_cast<unsigned char>(value[position + index]);
        const auto right = static_cast<unsigned char>(needle[index]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

bool matches_at(std::string_view value, std::size_t position, std::string_view needle) {
    return looks_ascii_only(needle)
        ? ascii_equal_at(value, position, needle)
        : value.substr(position, needle.size()) == needle;
}

bool parse_bool(std::string_view value) {
    return value != "0" && value != "false" && value != "off";
}

std::vector<std::string> split_tabs(std::string_view line) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = line.find('\t', start);
        fields.emplace_back(line.substr(start, end == std::string_view::npos ? end : end - start));
        if (end == std::string_view::npos) {
            return fields;
        }
        start = end + 1;
    }
}

} // namespace

TextProcessor::TextProcessor() = default;
TextProcessor::~TextProcessor() = default;

struct TextProcessor::CompactSegmenter {
    std::unordered_set<std::string> words;
    std::size_t maximum_word_bytes = 0;

    void add(std::string word) {
        if (word.empty()) {
            return;
        }
        maximum_word_bytes = std::max(maximum_word_bytes, word.size());
        words.insert(std::move(word));
    }

    void cut(std::string_view text, std::vector<std::string>& output) const {
        output.clear();
        for (std::size_t position = 0; position < text.size();) {
            const unsigned char value = static_cast<unsigned char>(text[position]);
            if (value < 0x80U) {
                if (std::isalnum(value) != 0 || value == '_' || value == '-') {
                    const std::size_t start = position++;
                    while (position < text.size()) {
                        const unsigned char next = static_cast<unsigned char>(text[position]);
                        const bool decimal_separator = next == '.' &&
                            position > start && position + 1 < text.size() &&
                            std::isdigit(static_cast<unsigned char>(text[position - 1])) != 0 &&
                            std::isdigit(static_cast<unsigned char>(text[position + 1])) != 0;
                        if (next >= 0x80U || (std::isalnum(next) == 0 &&
                            next != '_' && next != '-' && !decimal_separator)) {
                            break;
                        }
                        ++position;
                    }
                    output.emplace_back(text.substr(start, position - start));
                    continue;
                }
                output.emplace_back(text.substr(position, 1));
                ++position;
                continue;
            }

            std::size_t best_end = position;
            const std::size_t maximum_end = std::min(text.size(), position + maximum_word_bytes);
            for (std::size_t end = next_char_boundary(text, position); end <= maximum_end;) {
                if (words.find(std::string(text.substr(position, end - position))) != words.end()) {
                    best_end = end;
                }
                if (end == text.size()) {
                    break;
                }
                end = next_char_boundary(text, end);
            }
            if (best_end == position) {
                best_end = next_char_boundary(text, position);
            }
            output.emplace_back(text.substr(position, best_end - position));
            position = best_end;
        }
    }
};

void TextProcessor::set_hotwords(std::vector<HotwordEntry> entries) {
    hotwords_ = std::move(entries);
    if (segmenter_ != nullptr) {
        for (const HotwordEntry& entry : hotwords_) {
            if (entry.enabled) segmenter_->add(entry.phrase);
        }
    }
}

void TextProcessor::set_correction_rules(std::vector<CorrectionRule> rules) {
    correction_rules_ = std::move(rules);
}

bool TextProcessor::initialize_segmenter(
    const std::filesystem::path& dictionary_directory,
    std::string& error) {
    const auto dictionary = dictionary_directory / "jieba.dict.utf8";
    const auto user_dictionary = dictionary_directory / "user.dict.utf8";
    if (!std::filesystem::exists(dictionary)) {
        error = "segmentation dictionary is missing: " +
            dictionary_directory.string();
        return false;
    }
    try {
        auto segmenter = std::make_unique<CompactSegmenter>();
        std::ifstream input(dictionary);
        std::string line;
        while (std::getline(input, line)) {
            const std::size_t separator = line.find(' ');
            if (separator == std::string::npos) {
                continue;
            }
            int frequency = 0;
            try {
                frequency = std::stoi(line.substr(separator + 1));
            } catch (...) {
                continue;
            }
            if (frequency >= 10) {
                segmenter->add(line.substr(0, separator));
            }
        }
        if (!input.good() && !input.eof()) {
            error = "failed to read segmentation dictionary: " + dictionary.string();
            return false;
        }
        if (std::filesystem::exists(user_dictionary)) {
            std::ifstream user_input(user_dictionary);
            while (std::getline(user_input, line)) {
                const std::size_t separator = line.find_first_of(" \t");
                segmenter->add(line.substr(0, separator));
            }
        }
        segmenter_ = std::move(segmenter);
    } catch (const std::exception& exception) {
        error = "failed to initialize segmenter: " + std::string(exception.what());
        return false;
    }
    for (const HotwordEntry& entry : hotwords_) {
        if (entry.enabled) {
            segmenter_->add(entry.phrase);
        }
    }
    return true;
}

bool TextProcessor::add_hotword(std::string phrase, std::vector<std::string> aliases) {
    phrase = trim_ascii_space(std::move(phrase));
    if (phrase.empty()) {
        return false;
    }
    if (std::find_if(hotwords_.begin(), hotwords_.end(), [&](const HotwordEntry& entry) {
        return entry.phrase == phrase;
    }) != hotwords_.end()) {
        return false;
    }
    aliases.erase(std::remove_if(aliases.begin(), aliases.end(), [](std::string& alias) {
        alias = trim_ascii_space(std::move(alias));
        return alias.empty();
    }), aliases.end());
    hotwords_.push_back({std::move(phrase), std::move(aliases), true, 0});
    return true;
}

bool TextProcessor::remove_hotword(std::string_view phrase) {
    const auto iterator = std::remove_if(hotwords_.begin(), hotwords_.end(), [&](const HotwordEntry& entry) {
        return entry.phrase == phrase;
    });
    if (iterator == hotwords_.end()) {
        return false;
    }
    hotwords_.erase(iterator, hotwords_.end());
    return true;
}

bool TextProcessor::set_hotword_enabled(std::string_view phrase, bool enabled) {
    const auto iterator = std::find_if(hotwords_.begin(), hotwords_.end(), [&](const HotwordEntry& entry) {
        return entry.phrase == phrase;
    });
    if (iterator == hotwords_.end()) {
        return false;
    }
    iterator->enabled = enabled;
    return true;
}

bool TextProcessor::add_correction_rule(std::string pattern, std::string replacement) {
    pattern = trim_ascii_space(std::move(pattern));
    if (pattern.empty()) {
        return false;
    }
    correction_rules_.push_back({std::move(pattern), std::move(replacement), true});
    return true;
}

const std::vector<HotwordEntry>& TextProcessor::hotwords() const {
    return hotwords_;
}

const std::vector<CorrectionRule>& TextProcessor::correction_rules() const {
    return correction_rules_;
}

TextProcessResult TextProcessor::process(std::string_view text, bool record_hotword_hits) {
    std::string current(text);
    for (const CorrectionRule& rule : correction_rules_) {
        if (rule.enabled) {
            current = apply_rule(current, rule);
        }
    }
    current = normalize(current);

    TextProcessResult result;
    result.text.reserve(current.size());
    std::vector<std::pair<std::string_view, std::size_t>> aliases;
    for (std::size_t position = 0; position < current.size();) {
        std::size_t best_length = 0;
        HotwordEntry* best_entry = nullptr;
        for (HotwordEntry& entry : hotwords_) {
            if (!entry.enabled) {
                continue;
            }
            aliases.clear();
            aliases.emplace_back(entry.phrase, 0);
            for (const std::string& alias : entry.aliases) {
                aliases.emplace_back(alias, 0);
            }
            for (const auto& [alias, unused] : aliases) {
                if (alias.size() > best_length && matches_at(current, position, alias)) {
                    best_length = alias.size();
                    best_entry = &entry;
                }
            }
        }
        if (best_entry == nullptr) {
            const std::size_t next = next_char_boundary(current, position);
            result.text.append(current.substr(position, next - position));
            position = next;
            continue;
        }
        result.text += best_entry->phrase;
        if (record_hotword_hits) {
            ++best_entry->hits;
        }
        if (std::find(result.matched_hotwords.begin(), result.matched_hotwords.end(), best_entry->phrase) ==
            result.matched_hotwords.end()) {
            result.matched_hotwords.push_back(best_entry->phrase);
        }
        position += best_length;
    }
    result.text = normalize(result.text);
    if (segmenter_ != nullptr && !result.text.empty()) {
        segmenter_->cut(result.text, result.tokens);
    }
    result.clauses = split_clauses(result.text);
    return result;
}

bool TextProcessor::load_hotwords(const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "failed to open hotwords file: " + path.string();
        return false;
    }
    std::vector<HotwordEntry> entries;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto fields = split_tabs(line);
        if (fields.empty() || trim_ascii_space(fields[0]).empty()) {
            continue;
        }
        HotwordEntry entry;
        entry.phrase = trim_ascii_space(fields[0]);
        if (fields.size() > 1 && !fields[1].empty()) {
            std::stringstream aliases(fields[1]);
            std::string alias;
            while (std::getline(aliases, alias, '|')) {
                alias = trim_ascii_space(std::move(alias));
                if (!alias.empty()) {
                    entry.aliases.push_back(std::move(alias));
                }
            }
        }
        if (fields.size() > 2) {
            entry.enabled = parse_bool(fields[2]);
        }
        if (fields.size() > 3) {
            try {
                entry.hits = std::stoull(fields[3]);
            } catch (...) {
                entry.hits = 0;
            }
        }
        if (fields.size() > 4) {
            try {
                entry.boost = std::clamp(std::stof(fields[4]), 0.0F, 12.0F);
            } catch (...) {
                entry.boost = 3.0F;
            }
        }
        entries.push_back(std::move(entry));
    }
    if (!input.good() && !input.eof()) {
        error = "failed to read hotwords file: " + path.string();
        return false;
    }
    hotwords_ = std::move(entries);
    return true;
}

bool TextProcessor::save_hotwords(const std::filesystem::path& path, std::string& error) const {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        error = "failed to open hotwords file for writing: " + path.string();
        return false;
    }
    output << "# phrase\taliases separated by |\tenabled\thits\tctc boost (0-12)\n";
    for (const HotwordEntry& entry : hotwords_) {
        output << entry.phrase << '\t';
        for (std::size_t index = 0; index < entry.aliases.size(); ++index) {
            if (index != 0) {
                output << '|';
            }
            output << entry.aliases[index];
        }
        output << '\t' << (entry.enabled ? 1 : 0) << '\t' << entry.hits << '\t'
               << entry.boost << '\n';
    }
    if (!output) {
        error = "failed to write hotwords file: " + path.string();
        return false;
    }
    return true;
}

bool TextProcessor::load_correction_rules(const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path);
    if (!input) {
        error = "failed to open correction rules file: " + path.string();
        return false;
    }
    std::vector<CorrectionRule> rules;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const auto fields = split_tabs(line);
        if (fields.size() < 2 || trim_ascii_space(fields[0]).empty()) {
            continue;
        }
        rules.push_back({trim_ascii_space(fields[0]), fields[1], fields.size() < 3 || parse_bool(fields[2])});
    }
    if (!input.good() && !input.eof()) {
        error = "failed to read correction rules file: " + path.string();
        return false;
    }
    correction_rules_ = std::move(rules);
    return true;
}

bool TextProcessor::save_correction_rules(const std::filesystem::path& path, std::string& error) const {
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        error = "failed to open correction rules file for writing: " + path.string();
        return false;
    }
    output << "# pattern\treplacement\tenabled\n";
    for (const CorrectionRule& rule : correction_rules_) {
        output << rule.pattern << '\t' << rule.replacement << '\t'
               << (rule.enabled ? 1 : 0) << '\n';
    }
    if (!output) {
        error = "failed to write correction rules file: " + path.string();
        return false;
    }
    return true;
}

std::string TextProcessor::normalize(std::string_view text) {
    std::string output;
    output.reserve(text.size());
    bool pending_space = false;
    for (std::size_t position = 0; position < text.size();) {
        const unsigned char value = static_cast<unsigned char>(text[position]);
        if (value < 0x80U && is_ascii_space(value)) {
            pending_space = true;
            position += 1;
            continue;
        }
        if (pending_space) {
            const unsigned char previous = output.empty()
                ? 0
                : static_cast<unsigned char>(output.back());
            const bool around_punctuation = is_ascii_punctuation(previous) || value < 0x80U && is_ascii_punctuation(value);
            if (!output.empty() && !around_punctuation) {
                output.push_back(' ');
            }
            pending_space = false;
        }
        if (value < 0x80U) {
            const char mapped = value == ',' ? ',' : static_cast<char>(value);
            if (!output.empty() && mapped == output.back() && is_ascii_punctuation(value)) {
                position += 1;
                continue;
            }
            output.push_back(mapped);
            position += 1;
            continue;
        }
        const std::size_t next = next_char_boundary(text, position);
        const std::string_view codepoint = text.substr(position, next - position);
        if (!output.empty() && codepoint == "。" && output.back() == '。') {
            position = next;
            continue;
        }
        output.append(codepoint);
        position = next;
    }
    return trim_ascii_space(std::move(output));
}

std::string TextProcessor::polish_dictation(std::string_view text) {
    std::string current = normalize_dictation_punctuation(normalize(text));
    current = normalize(remove_pause_fillers(std::move(current)));
    const std::vector<std::string> sentences = split_sentences(current);

    std::string output;
    output.reserve(current.size() + 8);
    std::string previous_body;
    for (std::string sentence : sentences) {
        std::string body = strip_terminal_punctuation(sentence);
        if (body.empty() || body == previous_body) {
            continue;
        }
        previous_body = std::move(body);
        const bool contains_non_ascii = std::any_of(
            sentence.begin(), sentence.end(), [](unsigned char value) { return value >= 0x80U; });
        if (contains_non_ascii && !is_terminal_punctuation(sentence)) {
            sentence += "。";
        }
        output += sentence;
    }
    return trim_ascii_space(std::move(output));
}

std::vector<std::string> TextProcessor::split_clauses(std::string_view text) {
    std::vector<std::string> clauses;
    std::size_t start = 0;
    for (std::size_t position = 0; position < text.size();) {
        std::size_t boundary_length = 0;
        if (is_clause_boundary(text, position, boundary_length)) {
            const std::size_t end = position + boundary_length;
            const std::string clause = trim_ascii_space(std::string(text.substr(start, end - start)));
            if (!clause.empty()) {
                clauses.push_back(clause);
            }
            start = end;
            position = end;
            continue;
        }
        position = next_char_boundary(text, position);
    }
    const std::string tail = trim_ascii_space(std::string(text.substr(start)));
    if (!tail.empty()) {
        clauses.push_back(tail);
    }
    return clauses;
}
