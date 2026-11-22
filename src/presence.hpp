#pragma once
#include "json.hpp"
#include <algorithm>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace presence {
using json = nlohmann::json;
struct Settings {
    std::string clientId, name, details, state, largeImage, largeText, smallImage, smallText;
    int timer = 1, minutes = 0, interval = 15;
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Settings, clientId, name, details, state,
    largeImage, largeText, smallImage, smallText, timer, minutes, interval)
inline std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> result;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) result.push_back(line);
    }
    return result;
}
inline std::string selected(const std::string& text, size_t index) {
    auto values = lines(text);
    return values.empty() ? "" : values[index % values.size()];
}
inline size_t characters(const std::string& text) {
    return std::count_if(text.begin(), text.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
}
inline void validate(const Settings& s) {
    if (s.clientId.size() < 17 || s.clientId.size() > 20 ||
        s.clientId.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Application ID: введите 17–20 цифр из Discord Developer Portal.");
    if (s.timer < 0 || s.timer > 2 || s.minutes < 0 || s.minutes > 525600 ||
        (s.timer == 2 && s.minutes == 0))
        throw std::runtime_error("Таймер: от 0 до 525600 минут; для обратного отсчёта — больше нуля.");
    if (s.interval < 15 || s.interval > 86400)
        throw std::runtime_error("Интервал смены текста: 15–86400 секунд.");
    for (auto* value : {&s.name, &s.largeText, &s.smallText})
        if (characters(*value) > 128) throw std::runtime_error("Название и подписи: максимум 128 символов.");
    for (auto* value : {&s.details, &s.state})
        for (auto& line : lines(*value))
            if (characters(line) > 128) throw std::runtime_error("Каждая строка текста: максимум 128 символов.");
    for (auto* value : {&s.largeImage, &s.smallImage})
        if (value->size() > 1024) throw std::runtime_error("Адрес картинки слишком длинный (максимум 1024 байта).");
}
inline json activity(const Settings& s, size_t index, int64_t epoch) {
    json a = {{"type", 0}, {"instance", false}};
    auto put = [](json& object, const char* key, const std::string& value) {
        if (!value.empty()) object[key] = value;
    };
    put(a, "name", s.name);
    put(a, "details", selected(s.details, index));
    put(a, "state", selected(s.state, index));
    json assets = json::object();
    put(assets, "large_image", s.largeImage); put(assets, "large_text", s.largeText);
    put(assets, "small_image", s.smallImage); put(assets, "small_text", s.smallText);
    if (!assets.empty()) a["assets"] = assets;
    if (s.timer == 1) a["timestamps"] = {{"start", epoch - int64_t(s.minutes) * 60}};
    if (s.timer == 2) a["timestamps"] = {{"start", epoch}, {"end", epoch + int64_t(s.minutes) * 60}};
    return a;
}
inline json command(const json& a, uint32_t pid, uint64_t nonce) {
    return {{"cmd", "SET_ACTIVITY"}, {"args", {{"pid", pid}, {"activity", a}}},
            {"nonce", std::to_string(nonce)}};
}
}
