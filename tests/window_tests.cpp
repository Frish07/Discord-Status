// Exercise the real window lifecycle without publishing to a Discord account.
#include "../src/main.cpp"
#include <iostream>

void expect(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        canvas=std::make_unique<design::Canvas>();
        inputBrush=CreateSolidBrush(RGB(13,20,32));
        settingsPath=std::filesystem::current_path()/(L"presence-ui-test-"+std::to_wstring(GetCurrentProcessId())+L".json");
        INITCOMMONCONTROLSEX common{sizeof common,ICC_STANDARD_CLASSES}; InitCommonControlsEx(&common);
        WNDCLASSW cls{}; cls.lpfnWndProc=procedure; cls.hInstance=GetModuleHandleW(nullptr);
        cls.lpszClassName=L"PresenceStudioLifecycleTest"; RegisterClassW(&cls);
        auto hwnd=CreateWindowW(cls.lpszClassName,L"Presence Studio lifecycle test",WS_OVERLAPPEDWINDOW|WS_VSCROLL,
            0,0,1120,780,nullptr,nullptr,cls.hInstance,nullptr);
        expect(hwnd!=nullptr,"Window creation failed");
        // An isolated pipe prefix guarantees no connection to a real Discord client.
        rpc=std::make_unique<DiscordIpc>([](std::string){},L"\\\\?\\pipe\\presence-ui-test-"+std::to_wstring(GetCurrentProcessId())+L"-");
        active.clientId="123456789012345678"; active.details="First\nSecond"; active.interval=15;
        fill(active); running=true; epoch=12345; lastRotation=GetTickCount64()-15000;
        auto originalWorker=rpc.get();
        SendMessageW(hwnd,WM_CLOSE,0,0);
        expect(IsWindow(hwnd)!=FALSE,"Close destroyed the background window");
        expect(!trayAdded || !IsWindowVisible(hwnd),"Close did not hide the window in tray");
        expect(running && epoch==12345 && rpc.get()==originalWorker,"Close stopped or reset presence");
        // Pump the actual hidden window timer, rather than calling its handler directly.
        auto until=GetTickCount64()+1250;
        while (GetTickCount64()<until) {
            MSG msg;
            while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            Sleep(10);
        }
        expect(rotation==1,"Text rotation stopped while hidden");
        expect(epoch==12345 && running,"Hidden timer changed its start epoch");
        SetWindowTextW(entryLabel,L"Первый"); SetWindowTextW(entrySeconds,L"15");
        changePlaylist(120);
        auto second=active; second.name="Second status"; second.largeImage="second-cover"; second.timer=2; second.minutes=5;
        fill(second); SetWindowTextW(entryLabel,L"Второй"); SetWindowTextW(entrySeconds,L"20");
        changePlaylist(120);
        expect(load(settingsPath).playlist.size()==2,"Playlist did not persist with the profile");
        SendMessageW(playlistList,LB_SETCURSEL,0,0); selectMetadata();
        expect(readForm().largeImage.empty(),"Selecting a row did not load its own presence fields");
        SetWindowTextW(entrySeconds,L"45"); changePlaylist(121);
        expect(playlist[0].seconds==45 && playlist[0].presence.largeImage.empty(),
            "Editing duration overwrote the selected entry with another status");
        SetWindowTextW(entrySeconds,L"15"); changePlaylist(121);
        SendMessageW(playlistList,LB_SETCURSEL,1,0); selectMetadata();
        changePlaylist(124); expect(playlist[0].label=="Второй","Move up failed");
        changePlaylist(125); expect(playlist[1].label=="Второй","Move down failed");
        startCycle(); expect(player.isPlaying() && active.largeImage.empty(),"Cycle did not start at first entry");
        player.start(playlist,GetTickCount64()-14900);
        activateCycleEntry(GetTickCount64());
        auto switchUntil=GetTickCount64()+1250;
        while (GetTickCount64()<switchUntil) {
            MSG msg;
            while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            Sleep(10);
        }
        expect(player.currentIndex()==1 && active.largeImage=="second-cover" && active.timer==2,
            "Hidden window did not switch the complete presence snapshot");
        stopCycle(); expect(running && !player.isPlaying(),"Stopping the cycle removed the current presence");
        startCycle(); pausePresence();
        expect(!running && !player.isPlaying(),"Hide did not disable automatic republishing");
        SendMessageW(hwnd,WM_COMMAND,113,0);
        expect(page==3 && (GetWindowLongW(playlistList,GWL_STYLE)&WS_VISIBLE),"Playlist tab unavailable");
        SendMessageW(hwnd,WM_COMMAND,111,0);
        expect(page==1 && (GetWindowLongW(fields[4],GWL_STYLE)&WS_VISIBLE),"Images page unavailable");
        expect(!(GetWindowLongW(fields[0],GWL_STYLE)&WS_VISIBLE),"Hidden page participates in UI");
        SendMessageW(hwnd,WM_COMMAND,112,0);
        expect(page==2 && (GetWindowLongW(fields[8],GWL_STYLE)&WS_VISIBLE),"Timer page unavailable");
        RECT changed{0,0,1500,900}; SendMessageW(hwnd,WM_DPICHANGED,MAKELONG(144,144),reinterpret_cast<LPARAM>(&changed));
        LOGFONTW face{}; GetObjectW(font,sizeof face,&face);
        expect(dpi==144 && face.lfHeight==-24,"Font did not rescale for monitor DPI");
        SendMessageW(hwnd,WM_COMMAND,ExitCommand,0);
        expect(!IsWindow(hwnd) && !rpc && !trayAdded,"Explicit exit leaked worker/window/tray icon");
        std::filesystem::remove(settingsPath);
        DeleteObject(font); DeleteObject(inputBrush); canvas.reset();
        std::cout<<"Tray, background playlist snapshots, persistence, reorder, stop/hide, tabs, DPI and exit passed\n";
        return 0;
    } catch (const std::exception& e) {
        if (window && IsWindow(window)) exitApplication();
        std::cerr<<e.what()<<'\n'; return 1;
    }
}
