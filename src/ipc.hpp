#pragma once
#include <windows.h>
#include "presence.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

// All pipe I/O lives on one worker. UI updates only replace the desired activity.
class DiscordIpc {
    using json = presence::json;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::string clientId;
    json desired = nullptr;
    uint64_t revision = 0;
    std::thread worker;
    std::function<void(std::string)> notify;
    std::wstring pipePrefix;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::vector<char> input;
    bool send(uint32_t op, const std::string& body) {
        std::vector<char> bytes(8 + body.size());
        uint32_t length = static_cast<uint32_t>(body.size());
        memcpy(bytes.data(), &op, 4); memcpy(bytes.data() + 4, &length, 4);
        memcpy(bytes.data() + 8, body.data(), body.size());
        OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return false;
        DWORD written = 0;
        bool ok = WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &written, &ov) != FALSE;
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 2000) == WAIT_OBJECT_0)
                ok = GetOverlappedResult(pipe, &ov, &written, FALSE) != FALSE;
            else { CancelIoEx(pipe, &ov); GetOverlappedResult(pipe, &ov, &written, TRUE); }
        }
        CloseHandle(ov.hEvent);
        return ok && written == bytes.size();
    }
    void disconnect() {
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        pipe = INVALID_HANDLE_VALUE; input.clear();
    }
    void run() {
        using namespace std::chrono;
        std::string connectedId, pending;
        uint64_t sentRevision = 0, nonce = 0;
        bool ready = false;
        auto retry = steady_clock::now(), deadline = retry;
        auto lastSend = retry - seconds(15);
        while (!stopping) {
            std::string id; json activity; uint64_t rev;
            { std::lock_guard<std::mutex> lock(mutex); id = clientId; activity = desired; rev = revision; }
            auto now = steady_clock::now();
            if (pipe != INVALID_HANDLE_VALUE && id != connectedId) {
                disconnect(); ready = false; pending.clear(); retry = now;
            }
            if (pipe == INVALID_HANDLE_VALUE && !id.empty() && now >= retry) {
                for (int i = 0; i < 10; ++i) {
                    auto path = pipePrefix + std::to_wstring(i);
                    pipe = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                    if (pipe != INVALID_HANDLE_VALUE) break;
                }
                connectedId = id; ready = false; pending.clear(); sentRevision = 0;
                if (pipe != INVALID_HANDLE_VALUE && send(0, json({{"v", 1}, {"client_id", id}}).dump())) {
                    deadline = now + seconds(10); notify("Подключение к Discord…");
                } else { disconnect(); retry = now + seconds(5); notify("Discord недоступен. Повтор через 5 секунд…"); }
            }
            if (pipe != INVALID_HANDLE_VALUE) {
                DWORD available = 0;
                bool healthy = PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) != FALSE;
                if (healthy && available) {
                    char buffer[8192]; DWORD got = 0;
                    OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    BOOL result = ReadFile(pipe, buffer, std::min<DWORD>(available, sizeof buffer), &got, &ov);
                    if (!result && GetLastError() == ERROR_IO_PENDING) {
                        if (WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0) result = GetOverlappedResult(pipe, &ov, &got, FALSE);
                        else { CancelIoEx(pipe, &ov); GetOverlappedResult(pipe, &ov, &got, TRUE); }
                    }
                    CloseHandle(ov.hEvent);
                    healthy = result && got;
                    if (healthy) input.insert(input.end(), buffer, buffer + got);
                }
                while (healthy && input.size() >= 8) {
                    uint32_t op, size;
                    memcpy(&op, input.data(), 4); memcpy(&size, input.data() + 4, 4);
                    if (size > 1024 * 1024) { healthy = false; break; }
                    if (input.size() < size + 8) break;
                    std::string body(input.data() + 8, size);
                    input.erase(input.begin(), input.begin() + 8 + size);
                    if (op == 2) { healthy = false; break; }
                    if (op == 3) { healthy = send(4, body); continue; }
                    if (op != 1) continue;
                    auto msg = json::parse(body, nullptr, false);
                    if (!msg.is_object()) { healthy = false; break; }
                    if (msg.value("evt", json()) == "READY") { ready = true; notify("Discord подключён"); }
                    if (msg.value("nonce", json()) == pending && !pending.empty()) {
                        pending.clear();
                        if (msg.value("evt", json()) == "ERROR")
                            notify("Ошибка Discord: " + msg.value("data", json::object()).value("message", std::string("неизвестная ошибка")));
                        else notify(activity.is_null() ? "Активность скрыта" : "Rich Presence обновлён");
                    }
                }
                if ((!ready || !pending.empty()) && now > deadline) healthy = false;
                if (healthy && ready && pending.empty() && rev != sentRevision && now - lastSend >= seconds(15)) {
                    pending = std::to_string(++nonce);
                    healthy = send(1, presence::command(activity, GetCurrentProcessId(), nonce).dump());
                    sentRevision = rev; lastSend = now; deadline = now + seconds(10);
                    if (healthy) notify("Ожидание подтверждения Discord…");
                }
                if (!healthy) { disconnect(); retry = now + seconds(5); notify("Соединение потеряно. Повтор через 5 секунд…"); }
            }
            std::this_thread::sleep_for(milliseconds(50));
        }
        if (pipe != INVALID_HANDLE_VALUE && ready)
            send(1, presence::command(nullptr, GetCurrentProcessId(), ++nonce).dump());
        disconnect();
    }
public:
    explicit DiscordIpc(std::function<void(std::string)> callback,
                        std::wstring prefix = L"\\\\?\\pipe\\discord-ipc-")
        : notify(std::move(callback)), pipePrefix(std::move(prefix)) {
        worker = std::thread([this] { try { run(); } catch (...) { disconnect(); notify("Ошибка RPC. Перезапустите приложение."); } });
    }
    ~DiscordIpc() { stopping = true; if (worker.joinable()) worker.join(); }
    void update(const std::string& id, const json& activity) {
        std::lock_guard<std::mutex> lock(mutex); clientId = id; desired = activity; ++revision;
    }
};
