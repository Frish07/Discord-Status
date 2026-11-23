#pragma once
#include "presence.hpp"

namespace presence {
struct PlaylistEntry {
    std::string label;
    int seconds = 60;
    Settings presence;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PlaylistEntry, label, seconds, presence)

inline void validate(const std::vector<PlaylistEntry>& entries) {
    if (entries.size() > 100) throw std::runtime_error("В списке может быть не больше 100 статусов.");
    for (const auto& entry : entries) {
        if (entry.label.empty() || characters(entry.label) > 64)
            throw std::runtime_error("Название в списке: от 1 до 64 символов.");
        if (entry.seconds < 15 || entry.seconds > 86400)
            throw std::runtime_error("Время показа каждого статуса: от 15 до 86400 секунд.");
        validate(entry.presence);
    }
}

struct Profile {
    Settings draft;
    std::vector<PlaylistEntry> playlist;
};
// Keep the original flat fields: profiles from 1.1 remain readable.
inline void to_json(json& j, const Profile& profile) {
    j = profile.draft;
    j["playlist"] = profile.playlist;
    j["profileVersion"] = 2;
}
inline void from_json(const json& j, Profile& profile) {
    profile.draft = j.get<Settings>();
    profile.playlist = j.value("playlist", std::vector<PlaylistEntry>{});
    validate(profile.playlist);
    // Saving a playlist must also work while the new draft is still empty.
    if (!profile.draft.clientId.empty()) validate(profile.draft);
}

class PlaylistPlayer {
    std::vector<PlaylistEntry> entries;
    uint64_t began = 0, cycleMs = 0, segmentBegan = 0;
    size_t index = 0;
    bool playing = false;
public:
    void start(const std::vector<PlaylistEntry>& values, uint64_t now) {
        validate(values);
        if (values.empty()) throw std::runtime_error("Добавьте хотя бы один статус в список.");
        entries = values;
        cycleMs = 0;
        for (const auto& entry : entries) cycleMs += uint64_t(entry.seconds) * 1000;
        began = segmentBegan = now; index = 0; playing = true;
    }
    void stop() { playing = false; }
    bool isPlaying() const { return playing; }
    size_t currentIndex() const { return index; }
    const PlaylistEntry& current() const { return entries.at(index); }
    bool tick(uint64_t now) {
        if (!playing) return false;
        uint64_t elapsed = now >= began ? now - began : 0;
        uint64_t cycleStart = elapsed - elapsed % cycleMs;
        uint64_t offset = elapsed % cycleMs, start = 0;
        size_t next = 0;
        while (next + 1 < entries.size() && offset >= start + uint64_t(entries[next].seconds) * 1000) {
            start += uint64_t(entries[next].seconds) * 1000; ++next;
        }
        auto nextBegan = began + cycleStart + start;
        // Also detect a new lap of a one-entry list and long sleep/resume gaps.
        bool changed = nextBegan != segmentBegan;
        index = next; segmentBegan = nextBegan; return changed;
    }
    uint64_t remainingSeconds(uint64_t now) const {
        if (!playing) return 0;
        uint64_t end = segmentBegan + uint64_t(current().seconds) * 1000;
        return now >= end ? 0 : (end - now + 999) / 1000;
    }
};
}
