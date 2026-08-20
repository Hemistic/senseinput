#pragma once

#include <memory>
#include <string>

class SystemAudioMute final {
public:
    SystemAudioMute();
    ~SystemAudioMute();

    SystemAudioMute(const SystemAudioMute&) = delete;
    SystemAudioMute& operator=(const SystemAudioMute&) = delete;

    bool mute(std::string& error);
    void restore();
    [[nodiscard]] bool active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
