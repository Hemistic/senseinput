#include "stability_tracker.h"

#include <cassert>

int main() {
    StabilityTracker tracker(3, 2);
    auto first = tracker.update("微信输入");
    assert(first.stable.empty());

    auto second = tracker.update("微信输入法");
    assert(second.stable == "微信");
    assert(second.unstable == "输入法");

    auto third = tracker.update("微信输入法很好用");
    assert(third.stable == "微信");
    assert(!third.conflict);

    auto fourth = tracker.update("微信输入法非常好用");
    assert(fourth.stable == "微信输");

    auto final = tracker.finalize("微信输入法非常好用。");
    assert(final.stable == "微信输入法非常好用。");
    assert(final.unstable.empty());

    tracker.reset();
    tracker.update("SenseVoice is");
    auto english = tracker.update("SenseVoice is fast");
    assert(english.stable == "SenseVoice ");
    return 0;
}
