#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include "ipc.hpp"
#include "design.hpp"
#include "playlist.hpp"

namespace {
using design::Box;
constexpr UINT StatusMessage = WM_APP+1, TrayMessage = WM_APP+2, RestoreMessage = WM_APP+3;
constexpr int ShowCommand = 401, PauseCommand = 402, ExitCommand = 403;
constexpr float ContentHeight = 752;
HWND window{}, fields[11]{}, buttons[19]{}, playlistList{}, entryLabel{}, entrySeconds{};
HFONT font{};
HBRUSH inputBrush{};
UINT dpi = 96, taskbarCreated{};
int page = 0, scrollY = 0;
float width = 1120, height = ContentHeight;
bool running = false, trayAdded = false, trayHintShown = false;
bool shuttingDown = false;
std::unique_ptr<design::Canvas> canvas;
std::unique_ptr<DiscordIpc> rpc;
presence::Settings active;
presence::Settings savedDraft;
std::vector<presence::PlaylistEntry> playlist;
presence::PlaylistPlayer player;
size_t rotation = 0;
int64_t epoch = 0;
ULONGLONG lastRotation = 0;
std::wstring status = L"Готов к работе. Настройте статус и нажмите «Применить».";
std::filesystem::path settingsPath;
NOTIFYICONDATAW tray{};
Box inputBoxes[11]{};

int px(float value) { return static_cast<int>(value*dpi/96.f+0.5f); }
float leftWidth() { return width-428; }
float rightX() { return width-364; }
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0);
    std::wstring result(n,0); MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),result.data(),n);
    return result;
}
std::string utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    std::string result(n,0); WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),result.data(),n,nullptr,nullptr);
    return result;
}
std::string controlValue(HWND item) {
    int n = GetWindowTextLengthW(item);
    std::wstring text(n+1,0); GetWindowTextW(item,text.data(),n+1); text.resize(n);
    return utf8(text);
}
std::string value(int i) { return controlValue(fields[i]); }
int controlNumber(HWND item) {
    auto text = controlValue(item); size_t consumed = 0;
    try { int n = std::stoi(text,&consumed); if (consumed == text.size()) return n; } catch (...) {}
    throw std::runtime_error("Время и интервал должны быть целыми числами.");
}
int number(int i) { return controlNumber(fields[i]); }
presence::Settings readForm(bool check=true) {
    presence::Settings s;
    s.clientId=value(0); s.name=value(1); s.details=value(2); s.state=value(3);
    s.largeImage=value(4); s.largeText=value(5); s.smallImage=value(6); s.smallText=value(7);
    s.timer=static_cast<int>(SendMessageW(fields[8],CB_GETCURSEL,0,0));
    s.minutes=number(9); s.interval=number(10);
    if (check) presence::validate(s);
    return s;
}
void fill(const presence::Settings& s) {
    std::string values[]={s.clientId,s.name,s.details,s.state,s.largeImage,s.largeText,s.smallImage,s.smallText};
    for (int i=0;i<8;++i) SetWindowTextW(fields[i],wide(values[i]).c_str());
    SendMessageW(fields[8],CB_SETCURSEL,s.timer,0);
    SetWindowTextW(fields[9],std::to_wstring(s.minutes).c_str());
    SetWindowTextW(fields[10],std::to_wstring(s.interval).c_str());
}
void save(const std::filesystem::path& path,const presence::Settings& s) {
    auto temp=path; temp+=L".tmp";
    { std::ofstream file(temp,std::ios::binary|std::ios::trunc); file<<presence::json(presence::Profile{s,playlist}).dump(2); file.flush();
      if (!file) throw std::runtime_error("Не удалось записать настройки."); }
    if (!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Не удалось сохранить файл настроек.");
}
presence::Profile load(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path)>4*1024*1024) throw std::runtime_error("Слишком большой файл профиля (максимум 4 МБ).");
    std::ifstream file(path,std::ios::binary);
    return presence::json::parse(file).get<presence::Profile>();
}
void layout();
void refreshPlaylist(int selection=-1);
void repaint() {
    if (buttons[18]) EnableWindow(buttons[18],player.isPlaying());
    if (IsWindowVisible(window)) InvalidateRect(window,nullptr,FALSE);
}
void updateTray() {
    if (!trayAdded) return;
    std::wstring tip=L"Presence Studio — ";
    if (player.isPlaying()) tip+=wide(player.current().label)+L" · ";
    tip+=status;
    wcsncpy_s(tray.szTip,tip.c_str(),_TRUNCATE);
    tray.uFlags=NIF_TIP|NIF_SHOWTIP; Shell_NotifyIconW(NIM_MODIFY,&tray);
}
bool addTray() {
    tray={}; tray.cbSize=sizeof tray; tray.hWnd=window; tray.uID=1;
    tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP; tray.uCallbackMessage=TrayMessage;
    tray.hIcon=static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(1),IMAGE_ICON,32,32,LR_SHARED));
    wcscpy_s(tray.szTip,L"Presence Studio — управление статусом Discord");
    trayAdded=Shell_NotifyIconW(NIM_ADD,&tray)!=FALSE;
    if (trayAdded) { tray.uVersion=NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION,&tray); updateTray(); }
    return trayAdded;
}
void restoreWindow() {
    ShowWindow(window,IsIconic(window)?SW_RESTORE:SW_SHOW); SetForegroundWindow(window); repaint();
}
void minimizeToTray() {
    if (!trayAdded && !addTray()) { ShowWindow(window,SW_MINIMIZE); return; }
    ShowWindow(window,SW_HIDE);
    if (!trayHintShown) {
        trayHintShown=true; tray.uFlags=NIF_INFO;
        wcscpy_s(tray.szInfoTitle,L"Presence Studio работает в фоне");
        wcscpy_s(tray.szInfo,L"Статус и таймер продолжают работать. Нажмите значок, чтобы открыть окно. Для завершения выберите «Выйти» в меню значка.");
        tray.dwInfoFlags=NIIF_INFO; Shell_NotifyIconW(NIM_MODIFY,&tray);
    }
}
void pausePresence() {
    player.stop(); running=false; rpc->update(active.clientId,nullptr);
    status=L"Скрытие активности в очереди…"; updateTray(); repaint();
}
void publish(bool reset) {
    auto s=readForm(); save(settingsPath,s); savedDraft=s;
    player.stop(); active=s;
    if (reset || !running) { epoch=static_cast<int64_t>(time(nullptr)); rotation=0; }
    running=true; lastRotation=GetTickCount64();
    rpc->update(active.clientId,presence::activity(active,rotation,epoch));
    status=L"Применяем изменения. Это может занять до 15 секунд…"; updateTray(); repaint();
}
void profile(bool exporting) {
    wchar_t path[32768]=L"presence-profile.json";
    OPENFILENAMEW dialog{}; dialog.lStructSize=sizeof dialog; dialog.hwndOwner=window;
    dialog.lpstrFilter=L"Rich Presence profile (*.json)\0*.json\0\0";
    dialog.lpstrFile=path; dialog.nMaxFile=32768; dialog.lpstrDefExt=L"json";
    dialog.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|(exporting?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    if (exporting?GetSaveFileNameW(&dialog):GetOpenFileNameW(&dialog)) {
        if (exporting) {
            auto draft=readForm(false);
            if (!draft.clientId.empty()) presence::validate(draft);
            save(path,draft);
        } else {
            auto loaded=load(path);
            player.stop(); savedDraft=loaded.draft; playlist=std::move(loaded.playlist);
            fill(savedDraft); refreshPlaylist();
        }
        status=exporting?L"Профиль сохранён. Ваш стиль всегда под рукой.":L"Профиль загружен. Нажмите «Применить», чтобы включить его.";
        updateTray(); repaint();
    }
}
int selectedEntry() { return static_cast<int>(SendMessageW(playlistList,LB_GETCURSEL,0,0)); }
void selectMetadata() {
    int selected=selectedEntry();
    if (selected>=0 && static_cast<size_t>(selected)<playlist.size()) {
        const auto& entry=playlist[static_cast<size_t>(selected)];
        SetWindowTextW(entryLabel,wide(entry.label).c_str());
        SetWindowTextW(entrySeconds,std::to_wstring(entry.seconds).c_str());
    }
    bool found=selected>=0 && static_cast<size_t>(selected)<playlist.size();
    for (int slot:{12,13,14}) EnableWindow(buttons[slot],found);
    EnableWindow(buttons[15],found && selected>0);
    EnableWindow(buttons[16],found && static_cast<size_t>(selected)+1<playlist.size());
    EnableWindow(buttons[17],!playlist.empty());
}
void refreshPlaylist(int selection) {
    SendMessageW(playlistList,LB_RESETCONTENT,0,0);
    for (const auto& entry:playlist) {
        auto label=wide(entry.label)+L" — "+std::to_wstring(entry.seconds)+L" сек.";
        SendMessageW(playlistList,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
    }
    if (!playlist.empty()) SendMessageW(playlistList,LB_SETCURSEL,
        std::min(std::max(0,selection),static_cast<int>(playlist.size())-1),0);
    selectMetadata(); repaint();
}
void stopCycle() {
    player.stop();
    status=L"Автосмена остановлена. Текущий статус остаётся в Discord.";
    updateTray(); repaint();
}
void activateCycleEntry(ULONGLONG now) {
    active=player.current().presence; running=true; rotation=0;
    epoch=static_cast<int64_t>(time(nullptr)); lastRotation=now;
    rpc->update(active.clientId,presence::activity(active,rotation,epoch));
    status=L"Автосмена: "+wide(player.current().label)+L" — "+std::to_wstring(player.current().seconds)+L" сек.";
    updateTray(); repaint();
}
void startCycle() {
    presence::validate(playlist);
    if (playlist.empty()) throw std::runtime_error("Добавьте хотя бы один статус в список.");
    save(settingsPath,savedDraft);
    auto now=GetTickCount64(); player.start(playlist,now); activateCycleEntry(now);
}
void changePlaylist(int command) {
    int selected=selectedEntry();
    auto changed=playlist;
    auto draftToSave=savedDraft;
    if (command==120 || command==121) {
        auto settings=readForm();
        draftToSave=settings;
        auto label=controlValue(entryLabel);
        if (label.empty()) label=settings.name.empty()?"Статус "+std::to_string(playlist.size()+1):settings.name;
        presence::PlaylistEntry entry{label,controlNumber(entrySeconds),settings};
        if (command==120) { changed.push_back(std::move(entry)); selected=static_cast<int>(changed.size())-1; }
        else {
            if (selected<0) throw std::runtime_error("Сначала выберите статус в списке.");
            changed.at(static_cast<size_t>(selected))=std::move(entry);
        }
    } else {
        if (selected<0) return;
        if (command==122) changed.erase(changed.begin()+selected);
        if (command==124 && selected>0) { std::swap(changed[selected],changed[selected-1]); --selected; }
        if (command==125 && static_cast<size_t>(selected)+1<changed.size()) { std::swap(changed[selected],changed[selected+1]); ++selected; }
    }
    presence::validate(changed);
    // Persist first; a disk error leaves the old list and playback intact.
    auto previous=playlist; playlist=std::move(changed);
    try { save(settingsPath,draftToSave); } catch (...) { playlist=std::move(previous); throw; }
    savedDraft=std::move(draftToSave);
    player.stop(); refreshPlaylist(selected);
    status=L"Список сохранён. Нажмите «Запустить цикл», чтобы включить автосмену.";
    updateTray(); repaint();
}
void exitApplication() {
    if (shuttingDown) return;
    shuttingDown=true; KillTimer(window,1);
    if (trayAdded) { Shell_NotifyIconW(NIM_DELETE,&tray); trayAdded=false; }
    rpc.reset();
    MSG msg; while (PeekMessageW(&msg,window,StatusMessage,StatusMessage,PM_REMOVE)) delete reinterpret_cast<std::wstring*>(msg.lParam);
    DestroyWindow(window);
}
void trayMenu() {
    HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING,ShowCommand,L"Открыть Presence Studio");
    AppendMenuW(menu,MF_STRING|(running?0:MF_GRAYED),PauseCommand,L"Скрыть активность");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_STRING,ExitCommand,L"Выйти");
    SetMenuDefaultItem(menu,ShowCommand,FALSE);
    POINT point; GetCursorPos(&point); SetForegroundWindow(window);
    UINT command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,point.x,point.y,0,window,nullptr);
    DestroyMenu(menu); PostMessageW(window,WM_NULL,0,0);
    if (command) PostMessageW(window,WM_COMMAND,command,0);
}

void move(HWND item,Box b) {
    SetWindowPos(item,nullptr,px(b.x),px(b.y-scrollY),px(b.w),px(b.h),SWP_NOZORDER|SWP_NOACTIVATE);
}
int fieldPage(int i) { return i<4?0:i<8?1:2; }
void layout() {
    if (!fields[0]) return;
    RECT client; GetClientRect(window,&client);
    width=std::max(1000.f,(client.right-client.left)*96.f/dpi);
    height=(client.bottom-client.top)*96.f/dpi;
    scrollY=std::clamp(scrollY,0,std::max(0,static_cast<int>(ContentHeight-height)));
    SCROLLINFO scroll{sizeof scroll,SIF_RANGE|SIF_PAGE|SIF_POS,0,static_cast<int>(ContentHeight)-1,
        static_cast<UINT>(height),scrollY,0}; SetScrollInfo(window,SB_VERT,&scroll,TRUE);
    // Showing the scrollbar can change the available client width.
    GetClientRect(window,&client); width=std::max(1000.f,client.right*96.f/dpi);
    float inner=leftWidth()-48, half=(inner-20)/2;
    inputBoxes[0]={56,294,inner-156,42}; inputBoxes[1]={56,380,inner,42};
    inputBoxes[2]={56,468,half,104}; inputBoxes[3]={76+half,468,half,104};
    inputBoxes[4]={56,294,inner,42}; inputBoxes[5]={56,380,inner,42};
    inputBoxes[6]={56,466,inner,42}; inputBoxes[7]={56,552,inner,42};
    inputBoxes[8]={56,294,inner,42}; inputBoxes[9]={56,394,170,42}; inputBoxes[10]={56,500,170,42};
    for (int i=0;i<11;++i) {
        auto b=inputBoxes[i];
        if (i==8) { b.x+=10; b.y+=5; b.w-=20; b.h=180; }
        else { b.x+=12; b.y+=10; b.w-=24; b.h-=20; }
        move(fields[i],b); ShowWindow(fields[i],fieldPage(i)==page?SW_SHOW:SW_HIDE);
    }
    move(buttons[0],{32,112,114,40}); move(buttons[1],{154,112,164,40}); move(buttons[2],{326,112,110,40});
    move(buttons[10],{444,112,176,40});
    move(buttons[3],{56+inner-140,294,140,42}); ShowWindow(buttons[3],page==0?SW_SHOW:SW_HIDE);
    move(buttons[4],{32,640,196,46}); move(buttons[5],{page==3?432.f:240.f,640,148,46});
    move(buttons[6],{400,640,174,46});
    ShowWindow(buttons[4],page==3?SW_HIDE:SW_SHOW); ShowWindow(buttons[6],page==3?SW_HIDE:SW_SHOW);
    move(buttons[7],{rightX(),112,160,40}); move(buttons[8],{rightX()+172,112,160,40});
    move(buttons[9],{width-198,31,166,36});
    move(playlistList,{56,272,inner,140});
    move(entryLabel,{68,452,inner-202,22}); move(entrySeconds,{56+inner-158,452,146,22});
    move(buttons[11],{56,498,140,38}); move(buttons[12],{206,498,140,38}); move(buttons[13],{356,498,128,38});
    move(buttons[14],{56,548,190,36}); move(buttons[15],{258,548,94,36}); move(buttons[16],{364,548,94,36});
    move(buttons[17],{32,640,196,46}); move(buttons[18],{240,640,180,46});
    for (int slot=11;slot<=18;++slot) ShowWindow(buttons[slot],page==3?SW_SHOW:SW_HIDE);
    for (HWND item:{playlistList,entryLabel,entrySeconds}) ShowWindow(item,page==3?SW_SHOW:SW_HIDE);
    repaint();
}
void updateFonts() {
    HFONT next=CreateFontW(-px(16),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    for (auto item:fields) if (item) SendMessageW(item,WM_SETFONT,reinterpret_cast<WPARAM>(next),TRUE);
    for (auto item:{playlistList,entryLabel,entrySeconds}) if (item) SendMessageW(item,WM_SETFONT,reinterpret_cast<WPARAM>(next),TRUE);
    if (playlistList) SendMessageW(playlistList,LB_SETITEMHEIGHT,0,px(44));
    if (font) DeleteObject(font);
    font=next;
}
LRESULT CALLBACK editProcedure(HWND hwnd,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    if (message==WM_SETFOCUS || message==WM_KILLFOCUS) repaint();
    return DefSubclassProc(hwnd,message,wp,lp);
}
LRESULT CALLBACK buttonProcedure(HWND hwnd,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    if (message==WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof track,TME_LEAVE,hwnd,0}; TrackMouseEvent(&track); InvalidateRect(hwnd,nullptr,FALSE);
    }
    if (message==WM_MOUSELEAVE) InvalidateRect(hwnd,nullptr,FALSE);
    if (message==WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr,IDC_HAND)); return TRUE; }
    return DefSubclassProc(hwnd,message,wp,lp);
}
void button(int slot,int id,const wchar_t* text) {
    buttons[slot]=CreateWindowExW(0,L"BUTTON",text,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
        0,0,0,0,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    SetWindowSubclass(buttons[slot],buttonProcedure,1,0);
}
void createControls() {
    const wchar_t* names[]={L"Application ID",L"Название игры",L"Первая строка статуса",L"Вторая строка статуса",
        L"Большая картинка",L"Подпись большой картинки",L"Малая картинка",L"Подпись малой картинки",L"Режим таймера",L"Минуты",L"Интервал автосмены"};
    for (int i=0;i<11;++i) {
        if (i==8) {
            fields[i]=CreateWindowExW(0,L"COMBOBOX",names[i],WS_CHILD|WS_TABSTOP|CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS,
                0,0,0,0,window,reinterpret_cast<HMENU>(208),GetModuleHandleW(nullptr),nullptr);
            for (auto text:{L"Без таймера",L"Прошедшее время",L"Обратный отсчёт"}) SendMessageW(fields[i],CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
        } else {
            bool multi=i==2 || i==3;
            fields[i]=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_TABSTOP|
                (multi?ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN:ES_AUTOHSCROLL),0,0,0,0,window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(200+i)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(fields[i],EM_SETLIMITTEXT,multi?8192:1024,0);
            SendMessageW(fields[i],EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);
            SetWindowSubclass(fields[i],editProcedure,1,0);
        }
    }
    button(0,110,L"01   Статус"); button(1,111,L"02   Изображения"); button(2,112,L"03   Время");
    button(3,101,L"Получить ID  ↗"); button(4,102,L"Применить статус"); button(5,103,L"Скрыть статус");
    button(6,104,L"Сбросить таймер"); button(7,105,L"Импорт профиля"); button(8,106,L"Экспорт профиля");
    button(9,107,L"Свернуть в трей  ↘");
    button(10,113,L"04   Автосмена");
    button(11,120,L"+ Добавить"); button(12,121,L"Заменить"); button(13,122,L"Удалить");
    button(14,123,L"Редактировать статус"); button(15,124,L"↑ Выше"); button(16,125,L"↓ Ниже");
    button(17,126,L"Запустить цикл"); button(18,127,L"Остановить цикл");
    playlistList=CreateWindowExW(0,L"LISTBOX",L"Список статусов",WS_CHILD|WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOINTEGRALHEIGHT,
        0,0,0,0,window,reinterpret_cast<HMENU>(230),GetModuleHandleW(nullptr),nullptr);
    entryLabel=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL,
        0,0,0,0,window,reinterpret_cast<HMENU>(231),GetModuleHandleW(nullptr),nullptr);
    entrySeconds=CreateWindowExW(0,L"EDIT",L"60",WS_CHILD|WS_TABSTOP|ES_AUTOHSCROLL|ES_NUMBER,
        0,0,0,0,window,reinterpret_cast<HMENU>(232),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(entryLabel,EM_SETLIMITTEXT,64,0); SendMessageW(entrySeconds,EM_SETLIMITTEXT,5,0);
    for (HWND item:{entryLabel,entrySeconds}) SetWindowSubclass(item,editProcedure,1,0);
    updateFonts(); SendMessageW(fields[8],CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),px(28));
    SendMessageW(fields[8],CB_SETITEMHEIGHT,0,px(32));
    const wchar_t* cues[]={L"Например, 123456789012345678",L"Во что вы играете?",L"Занят любимым делом",L"В своём ритме",
        L"Ключ Art Assets или https://…",L"Что это за картинка?",L"Ключ Art Assets или https://…",L"Подпись при наведении"};
    for (int i=0;i<8;++i) if (i!=2 && i!=3) SendMessageW(fields[i],EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(cues[i]));
    refreshPlaylist(); layout();
}
void drawButton(const DRAWITEMSTRUCT& draw) {
    RECT bounds=draw.rcItem; int w=bounds.right-bounds.left,h=bounds.bottom-bounds.top;
    if (!canvas->begin(draw.hDC,bounds,dpi)) return;
    canvas->clear();
    POINT cursor; GetCursorPos(&cursor); ScreenToClient(draw.hwndItem,&cursor);
    bool hover=PtInRect(&bounds,cursor)!=FALSE, down=(draw.itemState&ODS_SELECTED)!=0;
    bool selected=draw.CtlID>=110 && draw.CtlID<=113 && page==static_cast<int>(draw.CtlID)-110;
    bool primary=draw.CtlID==102 || draw.CtlID==126;
    UINT32 fill=primary?(down?0x6552d6:hover?0x9a8bff:0x8271f3):selected?0x272443:hover?0x202c40:0x172131;
    Box b{1,1,w*96.f/dpi-2,h*96.f/dpi-2};
    canvas->box(b,fill,9,selected?0x6154a2:primary?0x9f91ff:design::Border);
    wchar_t text[128]; GetWindowTextW(draw.hwndItem,text,128);
    bool disabled=(draw.itemState&ODS_DISABLED)!=0;
    canvas->text(text,{b.x+8,b.y,b.w-16,b.h},14,disabled?0x6b7890:primary?0xffffff:selected?0xd9d1ff:design::Text,true,DWRITE_TEXT_ALIGNMENT_CENTER,true);
    if (draw.itemState&ODS_FOCUS) canvas->box({3,3,b.w-4,b.h-4},fill,7,design::Accent);
    // Redraw the label after the focus ring's fill.
    if (draw.itemState&ODS_FOCUS) canvas->text(text,{b.x+8,b.y,b.w-16,b.h},14,0xffffff,true,DWRITE_TEXT_ALIGNMENT_CENTER,true);
    canvas->end();
}
void drawCombo(const DRAWITEMSTRUCT& draw) {
    if (draw.itemID==static_cast<UINT>(-1)) return;
    if (!canvas->begin(draw.hDC,draw.rcItem,dpi)) return;
    canvas->clear((draw.itemState&ODS_SELECTED)?0x302a53:design::Input);
    wchar_t text[128]; SendMessageW(fields[8],CB_GETLBTEXT,draw.itemID,reinterpret_cast<LPARAM>(text));
    canvas->text(text,{6,0,(draw.rcItem.right-draw.rcItem.left)*96.f/dpi-12,(draw.rcItem.bottom-draw.rcItem.top)*96.f/dpi},15,design::Text,false,DWRITE_TEXT_ALIGNMENT_LEADING,true);
    canvas->end();
}
void drawPlaylistItem(const DRAWITEMSTRUCT& draw) {
    if (draw.itemID==static_cast<UINT>(-1) || draw.itemID>=playlist.size()) return;
    if (!canvas->begin(draw.hDC,draw.rcItem,dpi)) return;
    bool selected=(draw.itemState&ODS_SELECTED)!=0;
    canvas->clear(selected?0x302a53:design::Input);
    float w=(draw.rcItem.right-draw.rcItem.left)*96.f/dpi;
    const auto& entry=playlist[draw.itemID];
    auto label=std::to_wstring(draw.itemID+1)+L". "+wide(entry.label);
    if (player.isPlaying() && player.currentIndex()==draw.itemID) label=L"● "+label;
    canvas->text(label,{12,3,w-125,22},14,design::Text,true);
    canvas->text(wide(entry.presence.name),{12,25,w-125,18},11,design::Muted);
    canvas->text(std::to_wstring(entry.seconds)+L" сек.",{w-105,0,90,44},13,design::Accent,false,DWRITE_TEXT_ALIGNMENT_TRAILING,true);
    canvas->end();
}
void drawInput(int index) {
    auto b=inputBoxes[index];
    canvas->box(b,design::Input,9,GetFocus()==fields[index]?design::Accent:design::Border);
}
std::wstring timerText() {
    if (!running || !active.timer) return L"Таймер не запущен";
    int64_t seconds=active.timer==1?time(nullptr)-epoch+int64_t(active.minutes)*60:epoch+int64_t(active.minutes)*60-time(nullptr);
    seconds=std::max<int64_t>(0,seconds);
    wchar_t duration[80]; swprintf_s(duration,L"%02lld:%02lld:%02lld  %ls",seconds/3600,(seconds/60)%60,seconds%60,active.timer==1?L"прошло":L"осталось");
    return duration;
}
void paint() {
    PAINTSTRUCT ps; HDC screen=BeginPaint(window,&ps);
    RECT bounds; GetClientRect(window,&bounds);
    HDC buffer=CreateCompatibleDC(screen); HBITMAP bitmap=CreateCompatibleBitmap(screen,std::max(1L,bounds.right),std::max(1L,bounds.bottom));
    HGDIOBJ old=SelectObject(buffer,bitmap);
    if (canvas->begin(buffer,bounds,dpi)) {
        auto& c=*canvas; c.clear(); c.offset(-static_cast<float>(scrollY));
        c.mark(32,28,44);
        c.text(L"Presence Studio",{94,23,460,38},29,design::Text,true);
        c.text(L"Ваш статус. Ваш стиль.",{95,65,460,24},14,design::Muted);
        c.line(32,94,width-32,94,0x202a3b);
        c.box({32,174,leftWidth(),442},design::Panel,16,design::Border);
        const wchar_t* titles[]={L"Создайте своё присутствие",L"Добавьте характер",L"Задайте свой ритм",L"Статусы по кругу"};
        const wchar_t* descriptions[]={L"Название и две строки, которые расскажут о вас.",L"Большая обложка, маленькая деталь — ваш визуальный стиль.",L"Пусть время и смена текста работают за вас.",L"Настройте поля во вкладках и добавьте статус в список."};
        c.text(titles[page],{56,197,leftWidth()-48,32},23,design::Text,true);
        c.text(descriptions[page],{56,237,leftWidth()-48,24},14,design::Muted);
        if (page==0) {
            float half=(leftWidth()-68)/2;
            c.text(L"APPLICATION ID",{56,271,300,19},12,design::Muted,true);
            c.text(L"НАЗВАНИЕ ИГРЫ",{56,357,400,19},12,design::Muted,true);
            c.text(L"ДЕТАЛИ",{56,445,half,19},12,design::Muted,true);
            c.text(L"СОСТОЯНИЕ",{76+half,445,half,19},12,design::Muted,true);
            for (int i=0;i<4;++i) drawInput(i);
            c.text(L"Новая строка — следующий вариант для автосмены.",{56,586,leftWidth()-48,22},13,design::Muted);
        } else if (page==1) {
            const wchar_t* labels[]={L"БОЛЬШАЯ КАРТИНКА · КЛЮЧ ИЛИ HTTPS URL",L"ПОДПИСЬ БОЛЬШОЙ КАРТИНКИ",L"МАЛАЯ КАРТИНКА · КЛЮЧ ИЛИ HTTPS URL",L"ПОДПИСЬ МАЛОЙ КАРТИНКИ"};
            for (int i=4;i<8;++i) { c.text(labels[i-4],{56,inputBoxes[i].y-23,leftWidth()-48,19},12,design::Muted,true); drawInput(i); }
        } else if (page==2) {
            c.text(L"РЕЖИМ ТАЙМЕРА",{56,271,400,19},12,design::Muted,true); drawInput(8);
            c.text(L"МИНУТЫ",{56,371,170,19},12,design::Muted,true); drawInput(9);
            c.text(L"Для прошедшего времени — сколько уже прошло.\nДля отсчёта — сколько осталось.",{248,386,leftWidth()-240,58},14,design::Muted);
            c.text(L"СМЕНА ТЕКСТА · СЕКУНДЫ",{56,477,230,19},12,design::Muted,true); drawInput(10);
            c.text(L"От 15 секунд. Меняет оба списка текстов\nпо очереди, пока приложение работает.",{248,493,leftWidth()-240,58},14,design::Muted);
            c.text(L"«Применить» сохраняет таймер. «Сбросить» запускает заново.",{56,580,leftWidth()-48,28},13,design::Muted);
        } else {
            float inner=leftWidth()-48;
            c.box({55,271,inner+2,142},design::Input,9,design::Border);
            c.text(L"НАЗВАНИЕ В СПИСКЕ",{56,423,inner-190,19},12,design::Muted,true);
            c.text(L"ПОКАЗ · СЕКУНДЫ",{56+inner-170,423,170,19},12,design::Muted,true);
            c.box({56,442,inner-190,42},design::Input,9,GetFocus()==entryLabel?design::Accent:design::Border);
            c.box({56+inner-170,442,170,42},design::Input,9,GetFocus()==entrySeconds?design::Accent:design::Border);
            c.text(L"От 15 секунд. После последнего статуса — снова первый.",{56,592,inner,18},12,design::Muted);
        }
        float x=rightX();
        c.box({x,174,332,304},0x171e2e,16,0x36415b);
        c.text(L"ВАШ СТАТУС В DISCORD",{x+24,195,284,22},11,design::Muted,true);
        c.box({x+24,238,64,64},0x2d2851,14);
        // A vector placeholder: artwork itself is rendered by Discord.
        c.mark(x+34,248,42);
        c.dot(x+300,205,4,running?design::Green:0x6a7890);
        auto name=player.isPlaying()?active.name:value(1);
        auto details=presence::selected(player.isPlaying()?active.details:value(2),rotation);
        auto state=presence::selected(player.isPlaying()?active.state:value(3),rotation);
        c.text(wide(name.empty()?"Ваша игра":name),{x+106,240,202,62},21,design::Text,true);
        c.text(wide(details.empty()?"Занят любимым делом":details),{x+24,322,284,38},16,0xe0e6f4);
        c.text(wide(state.empty()?"В своём ритме":state),{x+24,369,284,38},14,design::Muted);
        c.box({x+24,424,284,30},0x102e2c,8);
        c.dot(x+38,439,3,design::Green);
        c.text(timerText(),{x+49,424,247,30},13,design::Green,false,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        c.box({x,494,332,122},design::Panel,16,design::Border);
        if (player.isPlaying()) {
            c.text(L"Цикл · "+std::to_wstring(player.currentIndex()+1)+L" / "+std::to_wstring(playlist.size()),{x+24,513,284,28},17,design::Green,true);
            c.text(wide(player.current().label),{x+24,548,284,22},13,design::Text,true);
            c.text(L"До смены: "+std::to_wstring(player.remainingSeconds(GetTickCount64()))+L" сек. · Работает и в трее.",{x+24,577,284,25},12,design::Muted);
        } else {
            c.text(L"Всегда на связи",{x+24,513,284,28},17,design::Text,true);
            c.text(L"Крестик спрячет окно в трей.\nСтатус, таймер и автосмена\nпродолжат работать в фоне.",{x+24,548,284,58},13,design::Muted);
        }
        c.dot(x+8,653,4,design::Green);
        c.text(L"Без токенов и паролей",{x+23,641,309,24},14,design::Text,true);
        c.text(page==1?L"Картинки проверяйте в профиле Discord.":L"Прямое подключение к вашему Discord.",{x+23,666,309,24},12,design::Muted);
        c.line(32,709,width-32,709,0x202a3b);
        c.dot(39,731,4,running?design::Green:design::Accent);
        c.text(status,{54,717,width-200,30},13,design::Muted,false,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        c.text(L"v1.2.0",{width-108,717,76,30},12,0x71829d,false,DWRITE_TEXT_ALIGNMENT_TRAILING,true);
        c.end(); BitBlt(screen,0,0,bounds.right,bounds.bottom,buffer,0,0,SRCCOPY);
    }
    SelectObject(buffer,old); DeleteObject(bitmap); DeleteDC(buffer); EndPaint(window,&ps);
}

LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if (taskbarCreated && message==taskbarCreated) {
        trayAdded=false;
        if (!addTray() && !IsWindowVisible(hwnd)) restoreWindow();
        return 0;
    }
    switch (message) {
    case WM_CREATE: {
        window=hwnd; dpi=GetDpiForWindow(hwnd); createControls();
        fill(presence::Settings{});
        try {
            if (std::filesystem::exists(settingsPath)) {
                auto loaded=load(settingsPath); savedDraft=loaded.draft;
                playlist=std::move(loaded.playlist); fill(savedDraft); refreshPlaylist();
            }
        }
        catch (const std::exception& e) { status=L"Профиль не загружен: "+wide(e.what()); }
        rpc=std::make_unique<DiscordIpc>([hwnd](std::string text) {
            auto payload=new std::wstring(wide(text));
            if (!PostMessageW(hwnd,StatusMessage,0,reinterpret_cast<LPARAM>(payload))) delete payload;
        });
        taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated"); addTray();
        SetTimer(hwnd,1,1000,nullptr); return 0;
    }
    case WM_COMMAND:
        try {
            int id=LOWORD(wp);
            if (id>=110 && id<=113) {
                page=id-110; layout();
                for (int i=0;i<3;++i) InvalidateRect(buttons[i],nullptr,FALSE);
                InvalidateRect(buttons[10],nullptr,FALSE);
                SetFocus(page==3?buttons[10]:buttons[page]);
            }
            switch (id) {
            case 101: ShellExecuteW(hwnd,L"open",L"https://discord.com/developers/applications",nullptr,nullptr,SW_SHOWNORMAL); break;
            case 102: publish(false); break;
            case 103: case PauseCommand: pausePresence(); break;
            case 104: publish(true); break;
            case 105: profile(false); break;
            case 106: profile(true); break;
            case 107: minimizeToTray(); break;
            case 120: case 121: case 122: case 124: case 125: changePlaylist(id); break;
            case 123: {
                auto selected=selectedEntry();
                if (selected>=0) {
                    fill(playlist.at(static_cast<size_t>(selected)).presence);
                    SendMessageW(hwnd,WM_COMMAND,110,0);
                    status=L"Статус загружен в форму. После правок откройте «Автосмена» и нажмите «Заменить».";
                    repaint();
                }
                break;
            }
            case 126: startCycle(); break;
            case 127: stopCycle(); break;
            case 230: if (HIWORD(wp)==LBN_SELCHANGE) selectMetadata(); break;
            case ShowCommand: restoreWindow(); break;
            case ExitCommand: exitApplication(); return 0;
            }
        } catch (const std::exception& e) { MessageBoxW(hwnd,wide(e.what()).c_str(),L"Проверьте настройки",MB_OK|MB_ICONWARNING); }
        if (HIWORD(wp)==EN_CHANGE || HIWORD(wp)==EN_SETFOCUS || HIWORD(wp)==EN_KILLFOCUS) repaint();
        return 0;
    case WM_DRAWITEM: {
        auto& draw=*reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (draw.CtlType==ODT_BUTTON) drawButton(draw);
        else if (draw.CtlType==ODT_COMBOBOX) drawCombo(draw);
        else if (draw.CtlType==ODT_LISTBOX) drawPlaylistItem(draw);
        return TRUE;
    }
    case WM_MEASUREITEM: {
        auto item=reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        item->itemHeight=px(item->CtlType==ODT_LISTBOX?44.f:30.f); return TRUE;
    }
    case WM_TIMER: {
        auto now=GetTickCount64();
        if (player.tick(now)) {
            activateCycleEntry(now); InvalidateRect(playlistList,nullptr,FALSE);
        }
        if (running && now-lastRotation>=static_cast<ULONGLONG>(active.interval)*1000) {
            lastRotation=now;
            if (presence::lines(active.details).size()>1 || presence::lines(active.state).size()>1)
                rpc->update(active.clientId,presence::activity(active,++rotation,epoch));
        }
        if (running) repaint();
        return 0;
    }
    case StatusMessage: {
        std::unique_ptr<std::wstring> payload(reinterpret_cast<std::wstring*>(lp)); status=*payload;
        updateTray(); repaint(); return 0;
    }
    case TrayMessage:
        switch (LOWORD(lp)) {
        case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONDBLCLK: case NIN_BALLOONUSERCLICK: restoreWindow(); break;
        case WM_CONTEXTMENU: case WM_RBUTTONUP: trayMenu(); break;
        }
        return 0;
    case RestoreMessage: restoreWindow(); return 0;
    case WM_SIZE:
        if (wp==SIZE_MINIMIZED) minimizeToTray(); else layout(); return 0;
    case WM_DPICHANGED: {
        dpi=HIWORD(wp); updateFonts();
        SendMessageW(fields[8],CB_SETITEMHEIGHT,static_cast<WPARAM>(-1),px(28)); SendMessageW(fields[8],CB_SETITEMHEIGHT,0,px(32));
        auto& rect=*reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd,nullptr,rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);
        layout(); return 0;
    }
    case WM_GETMINMAXINFO: {
        RECT minimum{0,0,px(1000),px(450)};
        AdjustWindowRectExForDpi(&minimum,WS_OVERLAPPEDWINDOW|WS_VSCROLL,FALSE,0,dpi);
        auto info=reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize={minimum.right-minimum.left,minimum.bottom-minimum.top}; return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof info,SIF_ALL,0,0,0,0,0}; GetScrollInfo(hwnd,SB_VERT,&info);
        switch (LOWORD(wp)) {
        case SB_LINEUP: scrollY-=28; break; case SB_LINEDOWN: scrollY+=28; break;
        case SB_PAGEUP: scrollY-=static_cast<int>(height)-48; break;
        case SB_PAGEDOWN: scrollY+=static_cast<int>(height)-48; break;
        case SB_THUMBTRACK: scrollY=info.nTrackPos; break;
        } layout(); return 0;
    }
    case WM_MOUSEWHEEL: scrollY-=GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA*60; layout(); return 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        SetTextColor(reinterpret_cast<HDC>(wp),RGB(240,244,252)); SetBkColor(reinterpret_cast<HDC>(wp),RGB(13,20,32));
        return reinterpret_cast<LRESULT>(inputBrush);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(); return 0;
    case WM_CLOSE: minimizeToTray(); return 0;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION: if (wp) exitApplication(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    HANDLE singleton=CreateMutexW(nullptr,FALSE,L"Local\\Frish07.DiscordPresenceManager");
    if (GetLastError()==ERROR_ALREADY_EXISTS) {
        if (auto existing=FindWindowW(L"DiscordPresenceManager",nullptr)) PostMessageW(existing,RestoreMessage,0,0);
        if (singleton) CloseHandle(singleton);
        return 0;
    }
    wchar_t appData[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA|CSIDL_FLAG_CREATE,nullptr,0,appData))) return 1;
    try {
        settingsPath=std::filesystem::path(appData)/L"DiscordPresenceManager";
        std::filesystem::create_directories(settingsPath); settingsPath/=L"settings.json";
        canvas=std::make_unique<design::Canvas>();
    } catch (const std::exception& e) { MessageBoxW(nullptr,wide(e.what()).c_str(),L"Presence Studio",MB_ICONERROR); return 1; }
    inputBrush=CreateSolidBrush(RGB(13,20,32));
    INITCOMMONCONTROLSEX common{sizeof common,ICC_STANDARD_CLASSES}; InitCommonControlsEx(&common);
    WNDCLASSW cls{}; cls.lpfnWndProc=procedure; cls.hInstance=instance; cls.lpszClassName=L"DiscordPresenceManager";
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.hIcon=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,0,0,LR_DEFAULTSIZE|LR_SHARED));
    RegisterClassW(&cls);
    dpi=GetDpiForSystem();
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);
    RECT rect{0,0,px(1120),px(ContentHeight)}; DWORD style=WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN|WS_VSCROLL;
    AdjustWindowRectExForDpi(&rect,style,FALSE,0,dpi);
    int w=std::min(rect.right-rect.left,work.right-work.left-24),h=std::min(rect.bottom-rect.top,work.bottom-work.top-24);
    HWND hwnd=CreateWindowW(cls.lpszClassName,L"Presence Studio · Discord Rich Presence",style,
        work.left+(work.right-work.left-w)/2,work.top+(work.bottom-work.top-h)/2,w,h,nullptr,nullptr,instance,nullptr);
    if (!hwnd) return 1;
    BOOL dark=TRUE; DwmSetWindowAttribute(hwnd,20,&dark,sizeof dark);
    COLORREF caption=RGB(12,16,25); DwmSetWindowAttribute(hwnd,35,&caption,sizeof caption);
    SendMessageW(hwnd,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,GetSystemMetricsForDpi(SM_CXSMICON,dpi),GetSystemMetricsForDpi(SM_CYSMICON,dpi),LR_SHARED)));
    ShowWindow(hwnd,show); MSG msg;
    while (GetMessageW(&msg,nullptr,0,0)>0) {
        // Tabs hidden on other pages never participate in dialog navigation.
        if (!IsDialogMessageW(hwnd,&msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    DeleteObject(font); DeleteObject(inputBrush); canvas.reset();
    if (singleton) CloseHandle(singleton);
    return 0;
}
