#include "text_processor.h"

#include <algorithm>
#include <cassert>
#include <filesystem>

int main() {
    TextProcessor processor;
    assert(processor.add_hotword("SenseVoice", {"sense voice"}));
    assert(processor.add_hotword("微信输入法", {"微信 输 入 法"}));
    assert(processor.add_correction_rule("几粒", "几例"));

    const TextProcessResult result = processor.process("  sense voice  几粒样品。  ");
    assert(result.text == "SenseVoice 几例样品。");
    assert(result.clauses.size() == 1);
    assert(result.matched_hotwords.size() == 1);
    assert(result.matched_hotwords.front() == "SenseVoice");
    assert(processor.hotwords().front().hits == 1);


    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "sensevoice-text-processor.tsv";
    std::string error;
    assert(processor.save_hotwords(path, error));
    TextProcessor loaded;
    assert(loaded.load_hotwords(path, error));
    assert(loaded.hotwords().size() == processor.hotwords().size());
    assert(loaded.hotwords().front().boost == 3.0F);
    std::filesystem::remove(path);

    const std::filesystem::path dictionary =
        std::filesystem::path(SENSEVOICE_TEST_SOURCE_DIR) /
        "third_party" / "cppjieba" / "dict";
    assert(processor.initialize_segmenter(dictionary, error));
    const TextProcessResult segmented = processor.process("微信输入法支持SenseVoice 3.5");
    assert(!segmented.tokens.empty());
    assert(std::find(segmented.tokens.begin(), segmented.tokens.end(), "微信输入法") !=
        segmented.tokens.end());
    assert(std::find(segmented.tokens.begin(), segmented.tokens.end(), "SenseVoice") !=
        segmented.tokens.end());
    assert(std::find(segmented.tokens.begin(), segmented.tokens.end(), "3.5") !=
        segmented.tokens.end());

    const std::string polished = TextProcessor::polish_dictation(
        "嗯,先测试。先测试。呃,然后继续");
    assert(polished == "先测试。然后继续。");

    const std::string decimal = TextProcessor::polish_dictation("版本是3.5,可以使用");
    assert(decimal == "版本是3.5，可以使用。");

    const std::string continuous = TextProcessor::polish_dictation(
        "第一句用于确认断句。第二句继续显示而不换行。第三句仍然只使用标点分隔。"
        "第四句把总长度推进到旧版自动分段阈值以上。第五句确认最终文本保持连续。");
    assert(continuous.find('\n') == std::string::npos);
    return 0;
}
