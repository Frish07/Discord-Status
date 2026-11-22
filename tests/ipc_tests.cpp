#include "ipc.hpp"
#include <iostream>
#include <stdexcept>
using presence::json;
void require(bool ok) { if (!ok) throw std::runtime_error("IPC integration test failed"); }
bool transfer(HANDLE pipe, bool writing, void* bytes, DWORD size, DWORD timeout = 22000) {
    OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    DWORD count = 0;
    BOOL ok = writing ? WriteFile(pipe,bytes,size,&count,&ov) : ReadFile(pipe,bytes,size,&count,&ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent,timeout) == WAIT_OBJECT_0) ok = GetOverlappedResult(pipe,&ov,&count,FALSE);
        else { CancelIoEx(pipe,&ov); GetOverlappedResult(pipe,&ov,&count,TRUE); }
    }
    CloseHandle(ov.hEvent); return ok && count == size;
}
std::pair<uint32_t,json> receive(HANDLE pipe) {
    uint32_t header[2]; require(transfer(pipe,false,header,8)); require(header[1] < 65536);
    std::string body(header[1],0); require(transfer(pipe,false,body.data(),header[1]));
    return {header[0],json::parse(body)};
}
void sendFrame(HANDLE pipe, uint32_t op, json body, bool fragment = false) {
    auto text = body.dump(); uint32_t header[2] = {op,static_cast<uint32_t>(text.size())};
    std::vector<char> bytes(8+text.size()); memcpy(bytes.data(),header,8); memcpy(bytes.data()+8,text.data(),text.size());
    if (fragment) {
        require(transfer(pipe,true,bytes.data(),3)); std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(transfer(pipe,true,bytes.data()+3,static_cast<DWORD>(bytes.size()-3)));
    } else require(transfer(pipe,true,bytes.data(),static_cast<DWORD>(bytes.size())));
}
int main() {
    HANDLE server = INVALID_HANDLE_VALUE;
    try {
        auto prefix = L"\\\\?\\pipe\\presence-test-" + std::to_wstring(GetCurrentProcessId()) + L"-";
        server = CreateNamedPipeW((prefix+L"0").c_str(),PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,1,65536,65536,0,nullptr);
        require(server != INVALID_HANDLE_VALUE);
        DiscordIpc client([](std::string){},prefix);
        presence::Settings s; s.clientId = "123456789012345678"; s.details = "IPC test";
        client.update(s.clientId,presence::activity(s,0,1000));
        OVERLAPPED connect{}; connect.hEvent = CreateEventW(nullptr,TRUE,FALSE,nullptr);
        BOOL connected = ConnectNamedPipe(server,&connect);
        if (!connected) {
            auto e = GetLastError();
            if (e == ERROR_IO_PENDING) require(WaitForSingleObject(connect.hEvent,4000) == WAIT_OBJECT_0);
            else require(e == ERROR_PIPE_CONNECTED);
        }
        CloseHandle(connect.hEvent);
        auto handshake = receive(server); require(handshake.first == 0 && handshake.second["client_id"] == s.clientId);
        sendFrame(server,1,{{"evt","READY"}},true);
        auto update = receive(server); require(update.first == 1 && update.second["args"]["activity"]["details"] == "IPC test");
        sendFrame(server,1,{{"cmd","SET_ACTIVITY"},{"nonce",update.second["nonce"]},{"data",json::object()}});
        sendFrame(server,3,{{"test",true}});
        auto pong = receive(server); require(pong.first == 4 && pong.second["test"] == true);
        client.update(s.clientId,nullptr);
        auto clear = receive(server); require(clear.second["args"]["activity"].is_null());
        sendFrame(server,1,{{"nonce",clear.second["nonce"]},{"data",nullptr}});
        std::cout << "IPC handshake, fragmented frames, activity, ping/pong and clear passed\n";
    } catch (const std::exception& e) { if (server != INVALID_HANDLE_VALUE) CloseHandle(server); std::cerr << e.what(); return 1; }
    CloseHandle(server); return 0;
}
