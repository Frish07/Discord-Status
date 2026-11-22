#include "presence.hpp"
#include <iostream>
void check(bool result) { if (!result) throw std::runtime_error("Test failed"); }
int main() {
    using namespace presence;
    Settings s; s.clientId = "123456789012345678"; s.name = "Моя игра";
    s.details = "Первый\r\n\nВторой"; s.state = "Один\nДва\nТри";
    validate(s);
    auto a = activity(s, 3, 1000);
    check(a["details"] == "Второй" && a["state"] == "Один" && a["name"] == s.name);
    check(a["timestamps"]["start"] == 1000 && !a.contains("assets"));
    s.minutes = 5; check(activity(s,0,1000)["timestamps"]["start"] == 700);
    s.timer = 2; check(activity(s,0,1000)["timestamps"]["end"] == 1300);
    s.timer = 0; check(!activity(s,0,1000).contains("timestamps"));
    s.largeImage = "https://example.com/image.png"; s.smallText = "Подпись";
    check(activity(s,0,1000)["assets"]["large_image"] == s.largeImage);
    auto restored = json::parse(json(s).dump()).get<Settings>();
    check(restored.details == s.details && restored.smallText == s.smallText);
    check(command(nullptr,42,7)["args"]["activity"].is_null());
    check(command(a,42,7)["nonce"] == "7");
    auto rejects = [](Settings bad) { try { validate(bad); } catch (...) { return true; } return false; };
    auto bad = s; bad.clientId = "bad"; check(rejects(bad));
    bad = s; bad.interval = 14; check(rejects(bad));
    bad = s; bad.timer = 2; bad.minutes = 0; check(rejects(bad));
    bad = s; bad.name = std::string(129,'a'); check(rejects(bad));
    check(characters("Привет 🌍") == 8);
    std::cout << "All presence tests passed\n";
}
