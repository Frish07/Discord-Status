#include "playlist.hpp"
#include <iostream>
void expect(bool ok) { if (!ok) throw std::runtime_error("Playlist test failed"); }
int main() {
    using namespace presence;
    Settings a; a.clientId="123456789012345678"; a.name="Игра"; a.details="Один";
    Settings b=a; b.name="Работа"; b.largeImage="cover"; b.timer=2; b.minutes=5;
    std::vector<PlaylistEntry> values={{"Первый",15,a},{"Второй",30,b}};
    PlaylistPlayer player;
    player.start(values,1000);
    expect(player.currentIndex()==0 && player.remainingSeconds(1000)==15);
    expect(!player.tick(15999) && player.remainingSeconds(15999)==1);
    expect(player.tick(16000) && player.currentIndex()==1);
    expect(player.current().presence.largeImage=="cover" && player.remainingSeconds(16000)==30);
    expect(player.tick(46000) && player.currentIndex()==0);
    expect(player.tick(1000+45000*1000+16000) && player.currentIndex()==1);
    player.stop(); expect(!player.tick(999999999) && !player.isPlaying());
    player.start({values[0]},50);
    expect(player.tick(15050) && player.currentIndex()==0 && player.remainingSeconds(15050)==15);
    values[0].label="Изменён"; expect(player.current().label=="Первый"); // Playback owns a snapshot.
    player.start(values,700); expect(player.currentIndex()==0 && player.remainingSeconds(700)==15);
    Profile p{a,values}; auto restored=json::parse(json(p).dump()).get<Profile>();
    expect(restored.playlist.size()==2 && restored.playlist[1].presence.timer==2 && restored.draft.name=="Игра");
    expect(json(a).get<Profile>().playlist.empty()); // v1.1 migration
    auto emptyDraft=Profile{}; emptyDraft.playlist=values;
    expect(json(emptyDraft).get<Profile>().playlist.size()==2);
    auto rejects=[](auto action) { try { action(); } catch (const std::exception&) { return true; } return false; };
    expect(rejects([&] { player.start({},0); }));
    auto bad=values; bad[0].seconds=14; expect(rejects([&] { validate(bad); }));
    bad=values; bad[1].presence.clientId="bad"; expect(rejects([&] { validate(bad); }));
    bad=values; bad[0].seconds=86401; expect(rejects([&] { validate(bad); }));
    bad=values; bad[0].label=""; expect(rejects([&] { validate(bad); }));
    expect(rejects([&] { validate(std::vector<PlaylistEntry>(101,values[0])); }));
    std::cout<<"Playlist durations, boundaries, wrap, resume, snapshots, profiles and validation passed\n";
}
