#include "weapon_hooks.h"
#include "game.h"
#include "MinHook.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace {
// Checked against the live PE and calls in the reflected native thunks.
constexpr uintptr_t coreTickRva=0x6412cc0, recoilRva=0x6509610,
    multiplierRva=0x6509660, spreadRva=0x6509b40, stackRva=0x64c5dc0;
constexpr uintptr_t bulletDamageRva=0x643c1e0, pointDamageRva=0x643c2e0;
constexpr uintptr_t objectsRva=0xc134f60, worldRva=0xc2c6b98, namesRva=0xc050e00;
constexpr DWORD timestamp=0x41e01d02, imageSize=0xcf84000;
constexpr UINT cleanupMessage=WM_APP+0x517;

struct ResourceDefinition {
    const wchar_t* tag;
    const char* label;
};
constexpr auto resources=std::to_array<ResourceDefinition>({
    {L"Resource.Metal", "金属"},
    {L"Resource.Oil", "石油"},
    {L"Resource.Concrete", "混凝土"},
    {L"Resource.Food", "食物"},
});
static_assert(resources.size()==static_cast<size_t>(HideoutResource::Count));

struct FactionDefinition {
    const char* tag;
    const char* label;
};
constexpr auto factions = std::to_array<FactionDefinition>({
    {"Mission.Faction.BLUEFOR", "UICS"},
    {"Mission.Faction.GREYFOR", "VLF"},
    {"Mission.Faction.OPFOR", "IGC"},
});
static_assert(factions.size() == static_cast<size_t>(Faction::Count));

void log(const char* text) {
    wchar_t path[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH,path)) return;
    wcscat_s(path,L"PQWeaponHooks.log");
    HANDLE f=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
        OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (f==INVALID_HANDLE_VALUE) return;
    DWORD written{}; WriteFile(f,text,DWORD(std::strlen(text)),&written,nullptr); CloseHandle(f);
}
template<class T> T get(uintptr_t address) {
    T result{};
    if (!address || address>0x7fffffffffffULL-sizeof(T)) return result;
    __try { std::memcpy(&result,reinterpret_cast<void*>(address),sizeof(T)); }
    __except(EXCEPTION_EXECUTE_HANDLER) { result={}; }
    return result;
}
template<class T> bool put(uintptr_t address,const T& value) {
    if (!address || address>0x7fffffffffffULL-sizeof(T)) return false;
    __try { std::memcpy(reinterpret_cast<void*>(address),&value,sizeof(T)); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
struct Array { uintptr_t data{}; int32_t count{},capacity{}; };
struct LoyaltyEntry {
    uint32_t factionNameIndex, factionNameNumber;
    float points;
};
static_assert(sizeof(LoyaltyEntry) == 0x0c && offsetof(LoyaltyEntry, points) == 8);
struct Guid { uint64_t a{},b{}; bool operator==(const Guid&) const=default; };
struct GuidHash { size_t operator()(const Guid& id) const { return size_t(id.a^(id.b*0x9e3779b97f4a7c15ULL)); } };
struct Object {
    uintptr_t address{}; int32_t index{-1},serial{};
    bool operator==(const Object&) const=default;
};
struct Field {
    uintptr_t property{},type{}; uint32_t offset{},size{};
    explicit operator bool() const { return property!=0; }
};
struct Snapshot {
    bool ammo{},grenades{},durability{},recoil{},spread{},oneHit{},instantSearch{};
    float melee{1.f}; bool operator==(const Snapshot&) const=default;
};
struct WeatherOption { uintptr_t preset{}; std::string label; };
using Tick=void(__fastcall*)(void*,float,uint32_t,void*);
using Getter=float(__fastcall*)(void*);
using GetterArg=float(__fastcall*)(void*,void*);
using Stack=bool(__fastcall*)(void*,void*,int32_t);
using BulletDamage=void(__fastcall*)(void*,void*,float,void*,const Vec3*,uint64_t,
    const Vec3*,void*,void*,void*);
using PointDamage=void(__fastcall*)(void*,void*,float,void*,const Vec3*,void*,uint64_t,
    const Vec3*,void*,void*);
using ProcessEvent=void(__fastcall*)(void*,void*,void*);
Tick originalTick{};
Getter originalRecoil{},originalSpread{};
GetterArg originalMultiplier{};
Stack originalStack{};
BulletDamage originalBulletDamage{};
PointDamage originalPointDamage{};
ProcessEvent originalProcessEvent{};
std::atomic_uint inFlight{};
struct CallGuard { CallGuard(){++inFlight;} ~CallGuard(){--inFlight;} };
struct SavedAmmo { Object inventory; bool original{}; };
struct SavedMontage { Object anim,montage; float original{},applied{}; };

class Runtime {
public:
    uintptr_t module{};
    std::atomic_bool enabled{},cleaning{},cleanDone{};
    std::atomic_bool noRecoil{},noSpread{},infiniteGrenades{},oneHit{};
    std::atomic_bool addMoneyRequested{},addCoinsRequested{},addExperienceRequested{},freeUpgradeRequested{};
    std::atomic_int addMoneyAmount{},addCoinsAmount{},addExperienceAmount{};
    std::array<std::atomic_int, resources.size()> addResourceAmounts{};
    std::array<std::atomic<float>, factions.size()> addReputationPoints{};
    std::atomic<float> requestedGameSpeed{1.f};
    std::atomic_int requestedHour{-1},currentWorldMinute{-1};
    std::atomic_bool worldTimeRefreshRequested{};
    std::atomic_bool weatherRequestRequested{};
    std::atomic_int requestedWeatherIndex{};
    std::atomic_uintptr_t localWeapon{},localInventory{},localController{};
    std::atomic_int killed{-1};
    std::atomic_bool killRequested{};
    DWORD gameThread{};
    HWND window{}; WNDPROC previousWndProc{};
    std::vector<void*> hooks;
    std::mutex requestMutex,statusMutex;
    std::recursive_mutex stateMutex;
    Snapshot requested{},active{};
    std::string statusText{"等待武器功能初始化"},actionStatus;
    Object world{},pawn{},core{},inventory{},stash{},manager{},equipped{},lastRateWeapon{};
    Object searchSettings{};
    Object speedWorld{};
    Object weatherActor{};
    uintptr_t weatherSetterParameter{};
    std::string weatherSetterName,weatherStatusText;
    std::vector<WeatherOption> weatherEntries;
    ULONGLONG nextWeatherProbe{};
    bool weatherGlobalScanDone{};
    float originalGameSpeed{};
    ULONGLONG nextWorldUpdate{};
    float originalSearchTime{},appliedSearchTime{};
    uintptr_t inventoryLibrary{},aiLibrary{},missionLibrary{},stringLibrary{},gameplayStatics{},searchSettingsDefault{};
    uintptr_t resourceLibrary{};
    std::unordered_map<uint32_t,std::string> names;
    std::unordered_map<uintptr_t,std::unordered_map<std::string,Field>> fieldCache;
    std::unordered_map<uintptr_t,std::unordered_map<std::string,uintptr_t>> functionCache;
    std::unordered_map<uintptr_t,SavedAmmo> ammo;
    std::unordered_map<uintptr_t,SavedMontage> montages;
    std::unordered_map<Guid,int32_t,GuidHash> grenades;
    ULONGLONG lastSlowTick{},lastLookup{};
    uint64_t tickCount{},grenadeRestores{};
    size_t durabilityWrites{};

    void setStatus(const char* text) { std::lock_guard lock(statusMutex); statusText=text; }
    void setActionStatus(const char* text) { std::lock_guard lock(statusMutex); actionStatus=text; }
    uintptr_t slot(int32_t index) const {
        const int32_t count=get<int32_t>(module+objectsRva+20);
        if (index<0 || index>=count || count>4000000) return 0;
        const auto chunk=get<uintptr_t>(get<uintptr_t>(module+objectsRva)+uintptr_t(index/65536)*8);
        return chunk ? chunk+uintptr_t(index%65536)*24 : 0;
    }
    Object object(uintptr_t address) const {
        if (!address || (address&7)) return {};
        const auto vtable=get<uintptr_t>(address);
        if (vtable<module || vtable>=module+imageSize || (get<uint32_t>(address+8)&0x18000)) return {};
        const auto index=get<int32_t>(address+12); const auto entry=slot(index);
        if (!entry || get<uintptr_t>(entry)!=address) return {};
        return {address,index,get<int32_t>(entry+16)};
    }
    bool alive(const Object& saved) const { return saved.address && object(saved.address)==saved; }
    std::string name(uint32_t index) {
        if (const auto found=names.find(index); found!=names.end()) return found->second;
        const auto block=get<uintptr_t>(module+namesRva+16+uintptr_t(index>>16)*8);
        const auto entry=block+uintptr_t(index&65535)*2;
        const auto header=get<uint16_t>(entry); const size_t n=header>>6;
        if (!block || !n || n>1023) return {};
        std::string text;
        if (header&1) {
            wchar_t wide[1024]{};
            for (size_t i=0;i<n;++i) wide[i]=get<wchar_t>(entry+2+i*2);
            const int count=WideCharToMultiByte(CP_UTF8,0,wide,int(n),nullptr,0,nullptr,nullptr);
            if (count<=0) return {};
            text.resize(count);
            WideCharToMultiByte(CP_UTF8,0,wide,int(n),text.data(),count,nullptr,nullptr);
        } else {
            text.resize(n); for (size_t i=0;i<n;++i) text[i]=get<char>(entry+2+i);
        }
        names.emplace(index,text); return text;
    }
    std::string objectName(uintptr_t address) { return name(get<uint32_t>(address+24)); }
    uintptr_t cls(uintptr_t address) const { return address ? get<uintptr_t>(address+16) : 0; }
    bool isA(uintptr_t address,const char* expected) {
        if (!object(address).address) return false;
        auto type=cls(address);
        for (int i=0;i<40 && object(type).address;++i,type=get<uintptr_t>(type+0x40))
            if (objectName(type)==expected) return true;
        return false;
    }
    Field field(uintptr_t type,const char* key) {
        auto& cache=fieldCache[type];
        if (const auto found=cache.find(key);found!=cache.end()) return found->second;
        for (int depth=0;depth<40 && object(type).address;++depth,type=get<uintptr_t>(type+0x40)) {
            auto property=get<uintptr_t>(type+0x50);
            for (int i=0;property && i<2048;++i,property=get<uintptr_t>(property+0x18)) {
                if (name(get<uint32_t>(property+0x20))!=key) continue;
                Field f{property,get<uintptr_t>(property+0x70),get<uint32_t>(property+0x44),get<uint32_t>(property+0x34)};
                const auto size=get<uint32_t>(type+0x58);
                if (!f.size || f.offset>size || f.size>size-f.offset) return {};
                return cache.emplace(key,f).first->second;
            }
        }
        return cache.emplace(key,Field{}).first->second;
    }
    uintptr_t referenced(uintptr_t owner,const char* key) {
        if (!object(owner).address) return 0;
        const auto f=field(cls(owner),key);
        return f && f.size==8 ? object(get<uintptr_t>(owner+f.offset)).address : 0;
    }
    uintptr_t arrayType(const Field& f) const {
        return f ? get<uintptr_t>(get<uintptr_t>(f.property+0x78)+0x70) : 0;
    }
    Array array(uintptr_t address,int limit=8192) const {
        const auto a=get<Array>(address);
        if (a.count<0 || a.count>limit || a.capacity<a.count || a.capacity>4000000 || (a.count && !a.data)) return {};
        return a;
    }
    uintptr_t function(uintptr_t type,const char* key) {
        auto& cache=functionCache[type];
        if (const auto found=cache.find(key);found!=cache.end()) return found->second;
        for (int depth=0;depth<40 && object(type).address;++depth,type=get<uintptr_t>(type+0x40)) {
            auto fn=get<uintptr_t>(type+0x48);
            for (int i=0;fn && i<2048;++i,fn=get<uintptr_t>(fn+0x28))
                if (objectName(fn)==key && isA(fn,"Function")) return cache.emplace(key,fn).first->second;
        }
        return cache.emplace(key,0).first->second;
    }
    uintptr_t processEventTarget() const {
        const auto chunks=get<uintptr_t>(module+objectsRva);
        const auto count=get<int32_t>(module+objectsRva+20);
        if (!chunks || count<=0 || count>4000000) return 0;
        for (int32_t i=0;i<count;++i) {
            const auto chunk=get<uintptr_t>(chunks+uintptr_t(i/65536)*8);
            const auto address=get<uintptr_t>(chunk+uintptr_t(i%65536)*24);
            if (!object(address).address) continue;
            const auto vtable=get<uintptr_t>(address);
            const auto entry=get<uintptr_t>(vtable+0x260);
            if (entry>=module && entry<module+imageSize) return entry;
        }
        return 0;
    }
    static bool dispatch(uintptr_t entry,uintptr_t target,uintptr_t fn,void* parameters) {
        __try {
            reinterpret_cast<ProcessEvent>(entry)(reinterpret_cast<void*>(target),reinterpret_cast<void*>(fn),parameters);
            return true;
        } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool invoke(uintptr_t target,const char* key,void* parameters,size_t size) {
        if (!object(target).address) return false;
        const auto fn=function(cls(target),key);
        if (!object(fn).address || get<uint16_t>(fn+0xb6)!=size) return false;
        // Proven by the game's RequestConsumeItem wrapper: vtable + 0x260.
        const auto entry=get<uintptr_t>(get<uintptr_t>(target)+0x260);
        if (entry<module || entry>=module+imageSize) return false;
        return dispatch(entry,target,fn,parameters);
    }
    uintptr_t contextResult(uintptr_t library,const char* key,uintptr_t context) {
        struct Params { uintptr_t context,result; } p{context,0};
        return invoke(library,key,&p,sizeof(p)) ? object(p.result).address : 0;
    }
    uintptr_t objectResult(uintptr_t target,const char* key) {
        uintptr_t result{};
        return invoke(target,key,&result,8) ? object(result).address : 0;
    }
    void findLibraries() {
        if (inventoryLibrary && aiLibrary && missionLibrary && stringLibrary && gameplayStatics && resourceLibrary) return;
        const auto chunks=get<uintptr_t>(module+objectsRva);
        const auto count=get<int32_t>(module+objectsRva+20);
        if (!chunks || count<=0 || count>4000000) return;
        for (int32_t i=0;i<count;++i) {
            const auto chunk=get<uintptr_t>(chunks+uintptr_t(i/65536)*8);
            const auto address=get<uintptr_t>(chunk+uintptr_t(i%65536)*24);
            if (!address || objectName(cls(address))!="Class") continue;
            const auto key=objectName(address);
            if (key=="InventoryEnhancedUISettings") {
                searchSettingsDefault=get<uintptr_t>(address+0x110);
                continue;
            }
            if (key=="KismetStringLibrary") {
                stringLibrary=get<uintptr_t>(address+0x110);
                continue;
            }
            if (key=="GameplayStatics") {
                gameplayStatics=get<uintptr_t>(address+0x110);
                continue;
            }
            if (key!="InventoryFunctionLibrary" && key!="AIFunctionLibrary" &&
                key!="IRRMissionSystemFunctionLibrary" && key!="IRRResourceFunctionLibrary") continue;
            const auto cdo=get<uintptr_t>(address+0x110);
            if (!object(cdo).address) continue;
            if (key=="InventoryFunctionLibrary") inventoryLibrary=cdo;
            else if (key=="AIFunctionLibrary") aiLibrary=cdo;
            else if (key=="IRRMissionSystemFunctionLibrary") missionLibrary=cdo;
            else resourceLibrary=cdo;
            if (inventoryLibrary && aiLibrary && missionLibrary && stringLibrary && gameplayStatics && resourceLibrary) break;
        }
    }
    bool globalSpeed(const Object& target,const char* operation,float& value) {
        struct Params {uintptr_t context;float value;} p{target.address,value};
        static_assert(offsetof(Params,value)==8);
        if (!gameplayStatics || !alive(target) || !invoke(gameplayStatics,operation,&p,0x0c)) return false;
        value=p.value;return true;
    }
    std::string weatherLabel(std::string value) {
        std::string lower=value;
        std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return char(std::tolower(c));});
        if (lower.find("clear")!=std::string::npos || lower.find("sunny")!=std::string::npos) return "晴朗";
        if (lower.find("overcast")!=std::string::npos) return "阴天";
        if (lower.find("cloud")!=std::string::npos) return "多云";
        if (lower.find("fog")!=std::string::npos) return "雾";
        if (lower.find("snow")!=std::string::npos || lower.find("blizzard")!=std::string::npos) return "降雪";
        if (lower.find("dust")!=std::string::npos) return lower.find("storm")!=std::string::npos ? "沙尘暴" : "浮尘";
        if (lower.find("storm")!=std::string::npos || lower.find("thunder")!=std::string::npos) return "雷暴";
        if (lower.find("rain")!=std::string::npos) {
            if (lower.find("heavy")!=std::string::npos || lower.find("strong")!=std::string::npos) return "暴雨";
            if (lower.find("light")!=std::string::npos) return "小雨";
            return "降雨";
        }
        std::replace(value.begin(),value.end(),'_',' ');
        return value;
    }
    void setWeatherStatus(const std::string& text) {
        std::lock_guard lock(statusMutex);
        weatherStatusText=text;
    }
    std::vector<WeatherOption> findWeatherPresets() {
        std::vector<WeatherOption> options;
        const auto chunks=get<uintptr_t>(module+objectsRva);
        const auto count=get<int32_t>(module+objectsRva+20);
        if (!chunks || count<=0 || count>4000000) return options;
        std::unordered_set<uintptr_t> seen;
        std::unordered_set<std::string> seenLabels;
        for (int32_t i=0;i<count;++i) {
            const auto chunk=get<uintptr_t>(chunks+uintptr_t(i/65536)*8);
            if (!chunk) continue;
            const auto address=get<uintptr_t>(chunk+uintptr_t(i%65536)*24);
            if (!object(address).address || !isA(address,"IRRWeatherSettings")) continue;
            const auto label=objectName(address);
            const auto className=objectName(cls(address));
            if (label.empty() || label==className || label.rfind("Default__",0)==0 ||
                label.rfind("NewEnumerator",0)==0 || !seen.emplace(address).second) continue;
            const auto display=weatherLabel(label);
            if (!seenLabels.emplace(display).second) continue;
            options.push_back({address,display});
        }
        const auto order=[](const std::string& label) {
            static constexpr const char* labels[]={"晴朗","多云","阴天","雾","小雨","降雨","雷暴","降雪","沙尘暴","浮尘"};
            for (int i=0;i<static_cast<int>(std::size(labels));++i) if (label==labels[i]) return i;
            return 100;
        };
        std::stable_sort(options.begin(),options.end(),[&](const auto& a,const auto& b){
            const auto ao=order(a.label),bo=order(b.label);
            return ao==bo ? a.label<b.label : ao<bo;
        });
        return options;
    }
    bool discoverWeatherInterface() {
        if (!alive(world)) return false;
        Object actor{};
        const auto level=referenced(world.address,"PersistentLevel");
        const auto actorsField=level ? field(cls(level),"Actors") : Field{};
        if (level && actorsField) {
            const auto actors=array(level+actorsField.offset,65536);
            for (int i=0;i<actors.count;++i) {
                const auto candidate=object(get<uintptr_t>(actors.data+uintptr_t(i)*8));
                if (candidate.address && objectName(cls(candidate.address)).find("BP_IRR_UDS_DynamicWeather")!=std::string::npos) {
                    actor=candidate;break;
                }
            }
        }
        if (!actor.address && !weatherGlobalScanDone) {
            weatherGlobalScanDone=true;
            const auto chunks=get<uintptr_t>(module+objectsRva);
            const auto count=get<int32_t>(module+objectsRva+20);
            if (chunks && count>0 && count<=4000000) {
                for (int32_t i=0;i<count;++i) {
                    const auto chunk=get<uintptr_t>(chunks+uintptr_t(i/65536)*8);
                    if (!chunk) continue;
                    const auto candidate=object(get<uintptr_t>(chunk+uintptr_t(i%65536)*24));
                    if (!candidate.address || objectName(cls(candidate.address)).find("BP_IRR_UDS_DynamicWeather")==std::string::npos) continue;
                    actor=candidate;break;
                }
            }
        }
        if (!actor.address) return false;
        weatherActor=actor;
        weatherSetterName="Change Weather";
        const auto setter=function(cls(actor.address),weatherSetterName.c_str());
        const auto newWeather=field(setter,"New Weather Type");
        const auto transition=field(setter,"Time To Transition To New Weather (Seconds)");
        if (!newWeather || !transition || newWeather.size!=sizeof(uintptr_t) || transition.size!=sizeof(double)) return false;
        if (newWeather.offset!=0 || transition.offset!=sizeof(uintptr_t)) return false;
        weatherSetterParameter=newWeather.property;
        weatherEntries=findWeatherPresets();
        return !weatherEntries.empty();
    }
    void updateWeatherControl() {
        const auto now=GetTickCount64();
        if ((!alive(weatherActor) || weatherEntries.empty() || !weatherSetterParameter) && now>=nextWeatherProbe) {
            nextWeatherProbe=now+(weatherGlobalScanDone ? 5000 : 1000);
            if (!alive(weatherActor)) {
                weatherActor={};weatherSetterParameter=0;weatherSetterName.clear();weatherEntries.clear();
            }
            if (!discoverWeatherInterface()) setWeatherStatus("当前地图天气接口未就绪");
            else setWeatherStatus("天气接口已就绪");
        }
        if (!weatherRequestRequested.exchange(false)) return;
        if (!alive(weatherActor) || weatherEntries.empty() || !weatherSetterParameter) {
            setWeatherStatus("当前地图天气接口未就绪");return;
        }
        const int index=std::clamp(requestedWeatherIndex.load(),0,static_cast<int>(weatherEntries.size())-1);
        const auto selected=weatherEntries[index];
        struct ChangeWeatherParams { uintptr_t preset; double transition; } params{selected.preset,1.0};
        static_assert(sizeof(ChangeWeatherParams)==0x10);
        if (!invoke(weatherActor.address,weatherSetterName.c_str(),&params,sizeof(params))) {
            setWeatherStatus("天气切换失败");return;
        }
        setWeatherStatus("当前天气："+selected.label);
    }
    void restoreGameSpeed() {
        if (!speedWorld.address) return;
        if (alive(speedWorld) && !globalSpeed(speedWorld,"SetGlobalTimeDilation",originalGameSpeed)) return;
        speedWorld={};originalGameSpeed=0.f;
    }
    void updateWorldFeatures(uintptr_t instance,ULONGLONG now) {
        const bool refresh=worldTimeRefreshRequested.exchange(false);
        int32_t hour=requestedHour.exchange(-1);
        if (!refresh && hour<0 && now<nextWorldUpdate) return;
        nextWorldUpdate=now+100;
        const float desired=requestedGameSpeed.load();
        if (desired==1.f || (speedWorld.address && speedWorld!=world)) restoreGameSpeed();
        if (desired!=1.f && (!speedWorld.address || speedWorld==world)) {
            float current{};
            if (globalSpeed(world,"GetGlobalTimeDilation",current) && std::isfinite(current) &&
                current>0.f && std::abs(current-desired)>.0001f) {
                float speed=desired;
                if (globalSpeed(world,"SetGlobalTimeDilation",speed) && !speedWorld.address) {
                    speedWorld=world;originalGameSpeed=current;
                }
            }
        }
        // Only opening the menu requests a game-time read; speed maintenance
        // must not continuously refresh the time slider's cached value.
        if (!refresh && hour<0) return;
        int minuteOfDay=-1;
        const auto worldManager=contextResult(missionLibrary,"GetIRRWorldManager",world.address);
        if (isA(worldManager,"IRRWorldManager")) {
            // The native setter updates RuntimeDayTime and runs the game's day/night update.
            if (hour>=0 && hour<24 && isA(instance,"GeneralGameInstance") &&
                invoke(instance,"SetTimeOfDay",&hour,sizeof(hour)))
                currentWorldMinute=hour*60;
            if (!refresh) return;
            // Read RuntimeDayTime with the current map's time-zone adjustment.
            struct LocalTime {
                uint64_t useSecondTimeZone,header;
                int32_t hours,minutes,seconds,padding;
            } time{0,0,-1,-1,-1,0};
            static_assert(sizeof(LocalTime)==0x20 && offsetof(LocalTime,hours)==0x10);
            if (invoke(worldManager,"GetDayTimeByCurrentLevel",&time,sizeof(time)) &&
                time.hours>=0 && time.hours<24 && time.minutes>=0 && time.minutes<60)
                minuteOfDay=time.hours*60+time.minutes;
        }
        if (refresh) currentWorldMinute=minuteOfDay;
    }
    void restoreSearchTime() {
        if (alive(searchSettings)) {
            const auto f=field(cls(searchSettings.address),"SearchTimeByItem");
            if (f && f.size==sizeof(float) &&
                std::abs(get<float>(searchSettings.address+f.offset)-appliedSearchTime)<.0001f) {
                put(searchSettings.address+f.offset,originalSearchTime);
                char line[128]{};std::snprintf(line,sizeof(line),"instant-search restored value=%.3f\r\n",originalSearchTime);log(line);
            }
        }
        searchSettings={};originalSearchTime=0.f;appliedSearchTime=0.f;
    }
    void updateSearchTime() {
        if (!active.instantSearch || !alive(inventory)) {restoreSearchTime();return;}
        const auto settingsField=field(cls(inventory.address),"UISettings");
        auto settings=settingsField && settingsField.size==sizeof(uintptr_t)
            ? object(get<uintptr_t>(inventory.address+settingsField.offset)) : Object{};
        if (!settings.address && inventoryLibrary)
            settings=object(contextResult(inventoryLibrary,"GetInventoryUISettings",pawn.address));
        const auto timeField=settings.address ? field(cls(settings.address),"SearchTimeByItem") : Field{};
        if (!settings.address || !timeField || timeField.size!=sizeof(float)) {restoreSearchTime();return;}
        if (searchSettings!=settings) {
            restoreSearchTime();
            searchSettings=settings;originalSearchTime=get<float>(settings.address+timeField.offset);appliedSearchTime=0.f;
            if (!std::isfinite(originalSearchTime) || originalSearchTime<=0.f) {
                const auto fallback=searchSettingsDefault ? field(cls(searchSettingsDefault),"SearchTimeByItem") : Field{};
                const auto value=fallback && fallback.size==sizeof(float)
                    ? get<float>(searchSettingsDefault+fallback.offset) : 0.f;
                if (std::isfinite(value) && value>0.f) originalSearchTime=value;
            }
        }
        constexpr float instantSearchTime=.01f;
        if (std::isfinite(originalSearchTime)) {
            const float current=get<float>(settings.address+timeField.offset);
            if (std::abs(current-instantSearchTime)>.0001f && put(settings.address+timeField.offset,instantSearchTime)) {
                appliedSearchTime=instantSearchTime;
                char line[128]{};std::snprintf(line,sizeof(line),"instant-search applied original=%.3f current=%.3f\r\n",
                    originalSearchTime,instantSearchTime);log(line);
            } else if (std::abs(current-instantSearchTime)<=.0001f) appliedSearchTime=instantSearchTime;
        }
    }
    void addCurrency(const wchar_t* currencyName,int32_t count,const char* label) {
        if (!stringLibrary || !inventoryLibrary || !alive(world) || !alive(pawn)) {
            setActionStatus("游戏货币接口未就绪");log("currency add failed: libraries, world, or pawn unavailable\r\n");return;
        }
        const auto playerInventory=contextResult(inventoryLibrary,"GetPlayerInventoryComponent",pawn.address);
        const auto stashInventory=contextResult(inventoryLibrary,"GetStashInventoryComponent",pawn.address);
        if (!isA(playerInventory,"InventoryComponent") || !isA(stashInventory,"InventoryComponent")) {
            setActionStatus("玩家库存或仓库未就绪");log("currency add failed: player or stash inventory unavailable\r\n");return;
        }
        struct NameParams {uintptr_t data;int32_t count,capacity;uint64_t result;} nameParams{
            reinterpret_cast<uintptr_t>(currencyName),int32_t(std::wcslen(currencyName)+1),int32_t(std::wcslen(currencyName)+1),0};
        if (!invoke(stringLibrary,"Conv_StringToName",&nameParams,sizeof(nameParams)) || !uint32_t(nameParams.result)) {
            setActionStatus("无法转换文档中的货币名称");log("currency add failed: Conv_StringToName failed\r\n");return;
        }
        const auto gameInstance=referenced(world.address,"OwningGameInstance");
        if (!isA(gameInstance,"GeneralGameInstance")) {
            setActionStatus("游戏货币接口未就绪");log("currency add failed: GeneralGameInstance unavailable\r\n");return;
        }
        struct Params {uint64_t name;int32_t amount;} p{nameParams.result,count};
        if (!invoke(gameInstance,"AddItem",&p,0x0c)) {
            setActionStatus("游戏未接受货币添加请求");
            char line[128]{};std::snprintf(line,sizeof(line),"currency AddItem invoke failed: amount=%d\r\n",count);log(line);return;
        }
        char message[128]{};std::snprintf(message,sizeof(message),"已发送%s添加请求：%d",label,count);setActionStatus(message);
        char line[192]{};std::snprintf(line,sizeof(line),"currency AddItem dispatched: name=%ls id=%u amount=%d player=%p stash=%p gameInstance=%p\r\n",
            currencyName,uint32_t(nameParams.result),count,reinterpret_cast<void*>(playerInventory),
            reinterpret_cast<void*>(stashInventory),reinterpret_cast<void*>(gameInstance));log(line);
    }
    void addResource(const ResourceDefinition& resource, int32_t amount) {
        if (!alive(world)) {
            setActionStatus("当前据点未就绪");
            return;
        }
        const auto netDriver = field(cls(world.address), "NetDriver");
        if (!netDriver || netDriver.size != sizeof(uintptr_t) || get<uintptr_t>(world.address + netDriver.offset)) {
            setActionStatus("添加资源仅支持单人游戏");
            return;
        }
        const auto component = contextResult(resourceLibrary, "GetResourceComponent", world.address);
        if (!isA(component, "IRRResourceComponent") || !object(stringLibrary).address) {
            setActionStatus("据点资源接口未就绪");
            return;
        }

        const auto length = static_cast<int32_t>(std::wcslen(resource.tag) + 1);
        struct NameParams { uintptr_t data; int32_t count, capacity; uint64_t result; };
        NameParams tag{reinterpret_cast<uintptr_t>(resource.tag), length, length, 0};
        if (!invoke(stringLibrary, "Conv_StringToName", &tag, sizeof(tag)) || !uint32_t(tag.result)) {
            setActionStatus("无法识别据点资源");
            return;
        }

        // FGameplayTag wraps one FName. Both getters place the int32 result at offset 8.
        struct AmountParams { uint64_t tag; int32_t value; };
        constexpr size_t amountParamsSize = 0x0c;
        AmountParams before{tag.result, -1}, capacity{tag.result, -1};
        if (!invoke(component, "GetAmount", &before, amountParamsSize) || before.value < 0 ||
            !invoke(component, "GetMaxAmount", &capacity, amountParamsSize)) {
            setActionStatus("读取据点资源余额失败");
            return;
        }
        const int32_t limit = capacity.value > 0 ? capacity.value : INT32_MAX;
        if (before.value >= limit) {
            setActionStatus("该据点资源已达容量上限");
            return;
        }

        struct AddParams { uint64_t tag; int32_t delta; uint8_t clampToCapacity; };
        // The reflected parameter size ends at the bool, before the compiler's tail padding.
        constexpr auto addParamsSize = offsetof(AddParams, clampToCapacity) + sizeof(uint8_t);
        static_assert(addParamsSize == 0x0d);
        AddParams request{tag.result, std::min(amount, limit - before.value), 1};
        AmountParams after{tag.result, -1};
        if (!invoke(component, "AddAmount", &request, addParamsSize) ||
            !invoke(component, "GetAmount", &after, amountParamsSize) || after.value <= before.value) {
            setActionStatus("未确认据点资源增加");
            return;
        }

        const auto autoSave = field(cls(component), "bAutoSaveOnChange");
        if (!autoSave || autoSave.size != 1 || !get<uint8_t>(component + autoSave.offset)) {
            uint8_t saved{};
            if (!invoke(component, "SaveToSlot", &saved, sizeof(saved)) || !saved) {
                setActionStatus("资源已增加，保存失败");
                return;
            }
        }
        char message[128]{};
        std::snprintf(message, sizeof(message), "已添加%s %d，当前余额 %d",
            resource.label, after.value - before.value, after.value);
        setActionStatus(message);
    }
    bool readFactionLoyalty(uintptr_t mission, const char* factionTag, LoyaltyEntry& result) {
        const auto progress = field(cls(mission), "ProgressData");
        const auto loyalty = field(progress.type, "LoyaltyData");
        if (!progress || !loyalty || loyalty.size != sizeof(Array) ||
            get<uint32_t>(arrayType(loyalty) + 0x58) != sizeof(LoyaltyEntry)) return false;

        const auto entries = array(mission + progress.offset + loyalty.offset, 64);
        for (int32_t i = 0; i < entries.count; ++i) {
            const auto entry = get<LoyaltyEntry>(entries.data + i * sizeof(LoyaltyEntry));
            if (entry.factionNameNumber == 0 && name(entry.factionNameIndex) == factionTag &&
                std::isfinite(entry.points)) {
                result = entry;
                return true;
            }
        }
        return false;
    }

    void addReputation(const FactionDefinition& faction, float points) {
        if (!alive(world) || !alive(pawn)) {
            setActionStatus("任务进度组件未就绪");
            return;
        }
        const auto netDriver = field(cls(world.address), "NetDriver");
        if (!netDriver || netDriver.size != sizeof(uintptr_t) || get<uintptr_t>(world.address + netDriver.offset)) {
            setActionStatus("添加阵营声望仅支持单人游戏");
            return;
        }

        const auto mission = contextResult(missionLibrary, "GetIRRMissionComponent", pawn.address);
        LoyaltyEntry before{};
        if (!isA(mission, "IRRMissionComponent") || !readFactionLoyalty(mission, faction.tag, before)) {
            setActionStatus("无法读取该阵营的声望");
            return;
        }
        if (before.points >= 100.f) {
            char message[96]{};
            std::snprintf(message, sizeof(message), "%s 声望已达上限 100", faction.label);
            setActionStatus(message);
            return;
        }

        LoyaltyEntry delta{before.factionNameIndex, before.factionNameNumber, points};
        struct AddLoyaltyParams {
            Array changes;
            uint8_t saveAfter;
        };
        constexpr size_t parameterSize = offsetof(AddLoyaltyParams, saveAfter) + sizeof(uint8_t);
        static_assert(parameterSize == 0x11);
        AddLoyaltyParams request{{reinterpret_cast<uintptr_t>(&delta), 1, 1}, 1};
        LoyaltyEntry after{};
        if (!invoke(mission, "AddFactionLoyalty", &request, parameterSize) ||
            !readFactionLoyalty(mission, faction.tag, after) || after.points <= before.points) {
            setActionStatus("未确认阵营声望增加");
            return;
        }

        char message[128]{};
        std::snprintf(message, sizeof(message), "%s 声望已增加 %.2f，当前 %.2f / 100",
            faction.label, after.points - before.points, after.points);
        setActionStatus(message);
    }

    void addExperience(int32_t amount) {
        if (!missionLibrary || !alive(pawn)) {setActionStatus("经验接口未就绪");log("XP request failed: interface or pawn unavailable\r\n");return;}
        const auto mission=contextResult(missionLibrary,"GetIRRMissionComponent",pawn.address);
        if (!isA(mission,"IRRMissionComponent")) {setActionStatus("任务进度组件未就绪");log("XP request failed: IRRMissionComponent unavailable\r\n");return;}
        std::array<unsigned char,0x90> before{},after{};
        if (!invoke(mission,"GetProgressData",before.data(),before.size())) {
            setActionStatus("无法读取当前任务进度");log("XP request failed: GetProgressData before call\r\n");return;
        }
        int32_t oldLevel{};float oldXP{};
        std::memcpy(&oldLevel,before.data(),sizeof(oldLevel));
        std::memcpy(&oldXP,before.data()+4,sizeof(oldXP));
        struct Params {float amount;uint8_t save,result;} p{static_cast<float>(amount),1,0};
        static_assert(offsetof(Params,save)==4 && offsetof(Params,result)==5);
        if (!invoke(mission,"AddXP",&p,6) || !p.result) {
            setActionStatus("游戏未接受经验添加请求");
            char line[128]{};std::snprintf(line,sizeof(line),"XP AddXP rejected: amount=%d result=%u\r\n",amount,unsigned(p.result));log(line);return;
        }
        if (!invoke(mission,"GetProgressData",after.data(),after.size())) {
            setActionStatus("经验请求已接受；暂时无法读取更新后的进度");log("XP AddXP accepted; GetProgressData after call failed\r\n");return;
        }
        int32_t newLevel{};float newXP{};
        std::memcpy(&newLevel,after.data(),sizeof(newLevel));
        std::memcpy(&newXP,after.data()+4,sizeof(newXP));
        if (newLevel>oldLevel || (newLevel==oldLevel && std::isfinite(newXP) && newXP>oldXP)) {
            setActionStatus("经验添加成功");
            char line[160]{};std::snprintf(line,sizeof(line),"XP verified: amount=%d level=%d->%d currentXP=%.2f->%.2f\r\n",
                amount,oldLevel,newLevel,oldXP,newXP);log(line);
        } else {
            setActionStatus("请求已接受，但任务进度没有变化");
            char line[160]{};std::snprintf(line,sizeof(line),"XP unchanged: amount=%d level=%d->%d currentXP=%.2f->%.2f\r\n",
                amount,oldLevel,newLevel,oldXP,newXP);log(line);
        }
    }
    void freeUpgrade() {
        if (!missionLibrary || !alive(pawn)) {setActionStatus("据点接口未就绪");return;}
        const auto hideout=contextResult(missionLibrary,"GetIRRHideoutUnitManager",pawn.address);
        const auto unit=referenced(hideout,"ActiveHideoutUnit");
        if (!isA(unit,"IRRHideoutUnitComponent")) {setActionStatus("请先打开一个据点设施升级页面");return;}
        int32_t level=-1;
        if (!invoke(unit,"GetHideoutUnitLevel",&level,sizeof(level)) || level<0 || level>=1000) {
            setActionStatus("无法读取当前设施等级");return;
        }
        int32_t next=level+1;
        if (invoke(unit,"ExecuteUpgradeHideoutUnit",&next,sizeof(next)))
            setActionStatus("升级请求已发送；请重进安全屋确认设施等级");
        else setActionStatus("设施升级调用失败");
    }
    void processRequests() {
        if (addMoneyRequested.exchange(false))
            addCurrency(L"cash",addMoneyAmount.load(),"金钱");
        if (addCoinsRequested.exchange(false))
            addCurrency(L"MarkedCoin",addCoinsAmount.load(),"标记硬币");
        if (missionLibrary && addExperienceRequested.exchange(false)) addExperience(addExperienceAmount.load());
        for (size_t i = 0; i < resources.size(); ++i) {
            const int32_t amount = addResourceAmounts[i].exchange(0);
            if (amount > 0) addResource(resources[i], amount);
        }
        for (size_t i = 0; i < factions.size(); ++i) {
            const float points = addReputationPoints[i].exchange(0.f);
            if (points > 0.f) addReputation(factions[i], points);
        }
        if (missionLibrary && freeUpgradeRequested.exchange(false)) freeUpgrade();
    }
    template<class Callback> void eachItem(uintptr_t component,Callback callback) {
        if (!isA(component,"InventoryComponent")) return;
        const auto f=field(cls(component),"MainContainers");
        const auto ct=arrayType(f); const auto itemsField=field(ct,"ContainerItems");
        const auto it=arrayType(itemsField);
        const auto cs=get<uint32_t>(ct+0x58),is=get<uint32_t>(it+0x58);
        if (!f || !itemsField || cs!=0x38 || is!=0xe8) return;
        const auto containers=array(component+f.offset,1024);
        for (int i=0;i<containers.count;++i) {
            const auto items=array(containers.data+uintptr_t(i)*cs+itemsField.offset,4096);
            for (int j=0;j<items.count;++j) callback(items.data+uintptr_t(j)*is,it);
        }
    }
    bool isGrenade(uintptr_t item) {
        const auto definition=get<uintptr_t>(item+0x10);
        if (!isA(definition,"IRRItemDefinition")) return false;
        const auto f=field(cls(definition),"ItemIdentifier");
        if (!f) return false;
        const auto tag=name(get<uint32_t>(definition+f.offset));
        return tag=="Inventory.Items.Grenades" || tag.starts_with("Inventory.Items.Grenades.");
    }
    void setAmmo(uintptr_t component,std::unordered_set<uintptr_t>& seen) {
        if (!isA(component,"InventoryComponent") || !seen.emplace(component).second) return;
        const auto id=object(component);
        if (const auto old=ammo.find(component);old!=ammo.end() && old->second.inventory!=id) ammo.erase(old);
        auto found=ammo.find(component);
        if (found==ammo.end()) {
            // The getter also ORs the GameInstance flag. Save the instance flag
            // from its verified native read, not the combined getter result.
            const auto original=get<uint8_t>(component+0x708);
            if (original>1) return;
            found=ammo.emplace(component,SavedAmmo{id,original!=0}).first;
        }
        uint8_t yes=1;
        if (get<uint8_t>(component+0x708)!=1 && invoke(component,"SetInfiniteAmmunition",&yes,1)) {
            uint8_t result{}; invoke(component,"GetInfiniteAmmunition",&result,1);
            char line[160]{};
            std::snprintf(line,sizeof(line),"ammo setter inventory=%p original=%d current=%u verified=%u\r\n",
                reinterpret_cast<void*>(component),int(found->second.original),unsigned(get<uint8_t>(component+0x708)),unsigned(result)); log(line);
        }
    }
    void restoreAmmo(const std::unordered_set<uintptr_t>& keep={}) {
        for (auto it=ammo.begin();it!=ammo.end();) {
            if (keep.contains(it->first) && alive(it->second.inventory)) {++it;continue;}
            if (alive(it->second.inventory)) {
                uint8_t value=0; invoke(it->first,"SetInfiniteAmmunition",&value,1);
            }
            it=ammo.erase(it);
        }
    }
    void disableAmmo(uintptr_t component) {
        if (!isA(component,"InventoryComponent") || get<uint8_t>(component+0x708)==0) return;
        uint8_t no=0;
        invoke(component,"SetInfiniteAmmunition",&no,1);
    }
    void updateGrenades() {
        if (!active.grenades) {grenades.clear();return;}
        std::unordered_map<Guid,int32_t,GuidHash> seen;
        bool changed=false;
        eachItem(inventory.address,[&](uintptr_t item,uintptr_t type){
            if (!isGrenade(item)) return;
            const auto countField=field(type,"Count"),uidField=field(type,"ItemUID");
            if (!countField || !uidField) return;
            const auto uid=get<Guid>(item+uidField.offset); const auto count=get<int32_t>(item+countField.offset);
            if (count<=0 || count>10000) return;
            auto& saved=seen[uid]; saved=count;
            if (const auto old=grenades.find(uid);old!=grenades.end()) {
                saved=(std::max)(count,old->second);
                if (count<saved && put(item+countField.offset,saved)) {changed=true;++grenadeRestores;log("grenade count restored by ItemUID\r\n");}
            }
        });
        grenades.swap(seen);
        if (changed) invoke(inventory.address,"OnRep_MainContainers",nullptr,0);
    }
    size_t repairItem(uintptr_t item,uintptr_t type) {
        const auto defField=field(type,"ItemDefinition"),statsField=field(type,"CurrentStats");
        if (!defField || !statsField) return 0;
        const auto definition=get<uintptr_t>(item+defField.offset);
        if (!isA(definition,"IRRItemDefinition")) return 0;
        const auto defaultsField=field(cls(definition),"ItemStats");
        const auto st=arrayType(statsField),bt=arrayType(defaultsField);
        const auto tagField=field(st,"Tag"),valueField=field(st,"Value"),maxField=field(st,"MaxValue");
        const auto configField=field(bt,"StatItemConfig"),baseValue=field(bt,"Value");
        const auto ss=get<uint32_t>(st+0x58),bs=get<uint32_t>(bt+0x58);
        if (!tagField || !valueField || !configField || !baseValue || ss!=32 || bs!=32) return 0;
        const auto stats=array(item+statsField.offset,128),defaults=array(definition+defaultsField.offset,128);
        size_t changed=0;
        for (int i=0;i<stats.count;++i) {
            const auto stat=stats.data+uintptr_t(i)*ss; const auto tag=get<uint64_t>(stat+tagField.offset);
            if (name(uint32_t(tag))!="Inventory.Stats.Durability") continue;
            float maximum=maxField ? get<float>(stat+maxField.offset) : 0.f;
            if (!std::isfinite(maximum) || maximum<=0.f) {
                maximum=0.f;
                for (int j=0;j<defaults.count;++j) {
                    const auto base=defaults.data+uintptr_t(j)*bs;
                    const auto config=get<uintptr_t>(base+configField.offset);
                    if (!object(config).address) continue;
                    const auto configTag=field(cls(config),"StatTag");
                    if (configTag && get<uint64_t>(config+configTag.offset)==tag) {
                        maximum=get<float>(base+baseValue.offset);break;
                    }
                }
            }
            const float current=get<float>(stat+valueField.offset);
            if (std::isfinite(maximum) && maximum>0.f && maximum<10000000.f && std::isfinite(current) &&
                current<maximum && put(stat+valueField.offset,maximum)) ++changed;
        }
        return changed;
    }
    size_t repairInventoryDurability(uintptr_t component) {
        if (!isA(component,"InventoryComponent")) return 0;
        size_t changed=0;
        eachItem(component,[&](auto item,auto type){changed+=repairItem(item,type);});
        // EquippableComponent::GetContainerItem reads this cache at
        // LocInventoryComp->PickUpContainerItem.ContainerItem, not MainContainers.
        const auto pickup=field(cls(component),"PickUpContainerItem");
        const auto item=field(pickup.type,"ContainerItem");
        if (pickup && item) changed+=repairItem(component+pickup.offset+item.offset,item.type);
        const auto attachments=field(pickup.type,"Attachments");
        const auto type=arrayType(attachments);const auto size=get<uint32_t>(type+0x58);
        if (pickup && attachments && size>0 && size<=1024) {
            const auto list=array(component+pickup.offset+attachments.offset,128);
            for (int i=0;i<list.count;++i) {
                const auto entry=list.data+uintptr_t(i)*size;
                if (objectName(type)=="ContainerItem") changed+=repairItem(entry,type);
                else {
                    const auto nested=field(type,"ContainerItem");
                    if (nested) changed+=repairItem(entry+nested.offset,nested.type);
                }
            }
        }
        return changed;
    }
    void updateDurability() {
        if (!active.durability) return;
        std::unordered_set<uintptr_t> seen;
        for (auto component:{inventory.address,stash.address,referenced(equipped.address,"OwningInventoryComp"),
            referenced(equipped.address,"LocInventoryComp")}) {
            if (!component || !seen.emplace(component).second) continue;
            const auto changed=repairInventoryDurability(component);
            if (changed) {
                // Restore the original in-place repair behavior. A replication
                // rebuild is not a stat refresh: it clears/rebuilds inventory
                // state and dispatches equipment callbacks during live firing.
                durabilityWrites+=changed;
                char line[128]{}; std::snprintf(line,sizeof(line),"durability inventory=%p repaired=%zu total=%zu\r\n",
                    reinterpret_cast<void*>(component),changed,durabilityWrites);log(line);
            }
        }
        const auto equipment=referenced(pawn.address,"EquipmentComponent");
        for (const auto key:{"RepEquippedItem","LocEquippedItem"}) {
            const auto data=field(cls(equipment),key),item=field(data.type,"Item");
            if (equipment && data && item) repairItem(equipment+data.offset+item.offset,item.type);
        }
        // A stat repair is not an equipment switch. Replaying the equipment
        // replication handler here restarts weapon state while it is firing.
    }
    void clearRecoil() {
        if (!active.recoil || !alive(core)) return;
        const auto f=field(cls(core.address),"FirstPersonCoreSubObjects"); if (!f) return;
        const auto objects=array(core.address+f.offset,128); constexpr Vec3 zero{};
        for (int i=0;i<objects.count;++i) {
            const auto recoil=get<uintptr_t>(objects.data+uintptr_t(i)*8);
            if (!isA(recoil,"FirstPersonWeaponRecoil") || referenced(recoil,"Character")!=pawn.address) continue;
            for (auto key:{"TargetCameraRecoil","CurrentCameraRecoil","TargetCameraRecoilCompensation",
                "CurrentCameraRecoilCompensation","FinalViewmodelRecoilLocation","FinalViewmodelRecoilRotation",
                "CurrentViewmodelRecoilLocation","CurrentViewmodelRecoilRotation","TargetLocation","TargetRotation",
                "CurrentLocation","CurrentRotation","TargetNoiseLocation","TargetNoiseRotation","CurrentNoiseLocation","CurrentNoiseRotation"}) {
                const auto value=field(cls(recoil),key);
                if (value && value.size==sizeof(zero)) put(recoil+value.offset,zero);
            }
        }
    }
    float montageRate(uintptr_t anim,uintptr_t montage) {
        struct Params {uintptr_t montage;float result;uint32_t pad;} p{montage,0,0};
        return invoke(anim,"Montage_GetPlayRate",&p,12) ? p.result : 0.f;
    }
    bool setMontageRate(uintptr_t anim,uintptr_t montage,float rate) {
        struct Params {uintptr_t montage;float rate;uint32_t pad;} p{montage,rate,0};
        return invoke(anim,"Montage_SetPlayRate",&p,12);
    }
    void restoreMontages(const std::unordered_set<uintptr_t>& keep={}) {
        for (auto it=montages.begin();it!=montages.end();) {
            const auto& saved=it->second;
            if (keep.contains(it->first) && alive(saved.anim) && alive(saved.montage)) {++it;continue;}
            if (alive(saved.anim) && alive(saved.montage) &&
                std::abs(montageRate(saved.anim.address,saved.montage.address)-saved.applied)<.0001f)
                setMontageRate(saved.anim.address,saved.montage.address,saved.original);
            it=montages.erase(it);
        }
    }
    void updateMelee() {
        std::unordered_set<uintptr_t> seen;
        if (std::abs(active.melee-1.f)<.0001f || !isA(equipped.address,"MeleeComponent")) {restoreMontages();return;}
        const auto data=objectResult(equipped.address,"GetMeleeDataAsset");
        if (!data) {restoreMontages();return;}
        const auto f=field(cls(data),"AttackMontageSets");const auto type=arrayType(f);
        const auto size=get<uint32_t>(type+0x58);
        if (!f || size<24 || size>256) {restoreMontages();return;}
        std::unordered_set<uintptr_t> attacks,instances;
        const auto sets=array(data+f.offset,128);
        for (int i=0;i<sets.count;++i) for (auto key:{"FP_Animation","TP_Animation","Weapon_Animation"}) {
            const auto animation=field(type,key);
            const auto montage=animation ? get<uintptr_t>(sets.data+uintptr_t(i)*size+animation.offset) : 0;
            if (isA(montage,"AnimMontage")) attacks.emplace(montage);
        }
        for (auto key:{"CharacterAnimInstance","EquippableAnimInstance"}) {
            const auto anim=referenced(equipped.address,key);if (isA(anim,"AnimInstance")) instances.emplace(anim);
        }
        const auto third=referenced(referenced(pawn.address,"Mesh"),"AnimScriptInstance");
        if (isA(third,"AnimInstance")) instances.emplace(third);
        for (auto anim:instances) {
            const auto montage=objectResult(anim,"GetCurrentActiveMontage");if (!attacks.contains(montage)) continue;
            const float current=montageRate(anim,montage);
            if (!std::isfinite(current) || current<=0.f || current>100.f) continue;
            if (auto old=montages.find(anim);old!=montages.end() &&
                (old->second.anim!=object(anim) || old->second.montage!=object(montage))) {
                if (alive(old->second.anim) && alive(old->second.montage))
                    setMontageRate(anim,old->second.montage.address,old->second.original);
                montages.erase(old);
            }
            auto [entry,inserted]=montages.try_emplace(anim,SavedMontage{object(anim),object(montage),current,current});
            const float rate=entry->second.original*active.melee;
            if (std::abs(current-rate)>.0001f && setMontageRate(anim,montage,rate)) {
                entry->second.applied=rate;
                if (inserted) {
                    char line[144]{};std::snprintf(line,sizeof(line),"melee montage=%p original=%.3f multiplier=%.3f applied=%.3f\r\n",
                        reinterpret_cast<void*>(montage),current,active.melee,rate);log(line);
                }
            }
            seen.emplace(anim);
        }
        restoreMontages(seen);
    }
    bool dead(uintptr_t attribute) {
        const auto f=field(cls(attribute),"bIsDead"); return !f || get<uint8_t>(attribute+f.offset)!=0;
    }
    template<class Callback> void eachEnemy(Callback callback) {
        if (!alive(manager)) return;
        const auto f=field(cls(manager.address),"AIPawns");if (!f) return;
        const auto list=array(manager.address+f.offset,16384);
        std::vector<uintptr_t> actors;
        for (int i=0;i<list.count;++i) actors.push_back(get<uintptr_t>(list.data+uintptr_t(i)*8));
        for (auto actor:actors) {
            if (actor==pawn.address || !isA(actor,"IRRBaseCharacter")) continue;
            const auto attribute=referenced(referenced(actor,"HealthComponent"),"Health");
            if (!attribute || dead(attribute)) continue;
            const auto data=field(cls(attribute),"CurrentData"),current=field(data.type,"BaseValue");
            if (!data || !current || current.size!=4) continue;
            const auto offset=data.offset+current.offset;const auto value=get<float>(attribute+offset);
            if (std::isfinite(value) && value>0.f) callback(actor,attribute,offset,value);
        }
    }
    float damageForHit(uintptr_t component,uintptr_t actor,uintptr_t instigator,float damage) {
        if (!oneHit.load() || !std::isfinite(damage) || damage<=0.f ||
            !instigator || instigator!=localController.load()) return damage;
        std::lock_guard lock(stateMutex);
        if (!alive(pawn) || actor==pawn.address || !alive(manager) ||
            referenced(actor,"HealthComponent")!=component) return damage;
        const auto f=field(cls(manager.address),"AIPawns");
        const auto list=f ? array(manager.address+f.offset,16384) : Array{};
        for (int i=0;i<list.count;++i) {
            if (get<uintptr_t>(list.data+uintptr_t(i)*8)!=actor) continue;
            // Change only the damage of a confirmed local-player hit. Never
            // pre-write enemy Health: ESP continues to show the real value.
            return (std::max)(damage,100000.f);
        }
        return damage;
    }
    void reset() {
        restoreGameSpeed();currentWorldMinute=-1;nextWorldUpdate=0;
        weatherActor={};weatherSetterParameter=0;weatherSetterName.clear();weatherEntries.clear();
        weatherGlobalScanDone=false;nextWeatherProbe=0;weatherRequestRequested=false;
        setWeatherStatus("");
        restoreAmmo();restoreMontages();restoreSearchTime();grenades.clear();
        localWeapon=0;localInventory=0;localController=0;
        core={};pawn={};inventory={};stash={};manager={};equipped={};lastRateWeapon={};lastSlowTick=0;lastLookup=0;
    }
    void tick(uintptr_t component,bool after) {
        if (!enabled.load()) return;
        std::lock_guard lock(stateMutex);gameThread=GetCurrentThreadId();
        if (cleaning.load()) {reset();cleanDone=true;return;}
        const auto currentWorld=object(get<uintptr_t>(module+worldRva));
        const auto instance=referenced(currentWorld.address,"OwningGameInstance");
        const auto playersField=field(cls(instance),"LocalPlayers");
        const auto players=playersField ? array(instance+playersField.offset,8) : Array{};
        const auto player=players.count ? get<uintptr_t>(players.data) : 0;
        const auto controller=referenced(player,"PlayerController");
        const auto currentPawn=object(referenced(controller,"AcknowledgedPawn"));
        const auto currentCore=object(referenced(currentPawn.address,"BP_FirstPersonCoreComponent"));
        if (!currentCore.address || component!=currentCore.address) return;
        if (after) {
            if (core==currentCore) {
                clearRecoil();updateMelee();
                if (active.durability)
                    repairInventoryDurability(referenced(equipped.address,"LocInventoryComp"));
            }
            return;
        }
        if (currentWorld!=world || currentPawn!=pawn) {
            reset();world=currentWorld;pawn=currentPawn;core=currentCore;
            functionCache.clear();fieldCache.clear();
            char line[160]{};std::snprintf(line,sizeof(line),"weapon-v2 world=%p pawn=%p gameThread=%lu\r\n",
                reinterpret_cast<void*>(world.address),reinterpret_cast<void*>(pawn.address),gameThread);log(line);
        }
        Snapshot next;{std::lock_guard requestLock(requestMutex);next=requested;}
        if (next!=active) {
            active=next;lastSlowTick=0;
            char line[200]{};std::snprintf(line,sizeof(line),"weapon-v2 settings ammo=%d grenades=%d durability=%d recoil=%d spread=%d melee=%.2f oneHit=%d\r\n",
                int(active.ammo),int(active.grenades),int(active.durability),int(active.recoil),int(active.spread),active.melee,int(active.oneHit));log(line);
        }
        noRecoil=active.recoil;noSpread=active.spread;infiniteGrenades=active.grenades;
        oneHit=active.oneHit;localController=controller;
        const auto now=GetTickCount64();
        findLibraries();
        updateWorldFeatures(instance,now);
        updateWeatherControl();
        auto currentInventory=contextResult(inventoryLibrary,"GetPlayerInventoryComponent",pawn.address);
        if (!currentInventory) currentInventory=referenced(pawn.address,"BP_InventoryComponent");
        if (object(currentInventory)!=inventory) {restoreAmmo();grenades.clear();inventory=object(currentInventory);}
        localInventory=inventory.address;
        updateSearchTime();processRequests();
        const auto weapon=referenced(pawn.address,"WeaponInHands");localWeapon=weapon;equipped=object(weapon);
        if (weapon && object(weapon)!=lastRateWeapon) {
            float interval{};
            if (invoke(weapon,"GetFireRate",&interval,sizeof(interval))) {
                char line[144]{};
                std::snprintf(line,sizeof(line),"native firearm interval=%.9f seconds rpm=%.3f (read-only; no fire-rate hook)\r\n",
                    interval,interval>0.f ? 60.f/interval : 0.f);log(line);
            }
            lastRateWeapon=object(weapon);
        }
        const auto equipment=referenced(pawn.address,"EquipmentComponent");
        if (!weapon && equipment) {
            const auto data=field(cls(equipment),"LocEquippedItem"),item=field(data.type,"Item");
            if (data && item) {
                alignas(8) unsigned char params[0xf0]{};
                for (size_t i=0;i<0xe8;++i) params[i]=get<uint8_t>(equipment+data.offset+item.offset+i);
                if (invoke(inventoryLibrary,"GetMeleeComponentByContainerItem",params,sizeof(params)))
                    equipped=object(get<uintptr_t>(reinterpret_cast<uintptr_t>(params)+0xe8));
            }
        }
        std::unordered_set<uintptr_t> ammoTargets;
        if (active.ammo) {
            setAmmo(inventory.address,ammoTargets);
            setAmmo(referenced(equipped.address,"OwningInventoryComp"),ammoTargets);
        } else {
            disableAmmo(inventory.address);
            disableAmmo(referenced(equipped.address,"OwningInventoryComp"));
            const auto globalAmmo=field(cls(instance),"bInfiniteAmmunition");
            if (globalAmmo && globalAmmo.size==1) {
                constexpr uint8_t no=0;
                put(instance+globalAmmo.offset,no);
            }
        }
        restoreAmmo(ammoTargets);
        if (!lastSlowTick || now-lastSlowTick>=100) {
            lastSlowTick=now;
            if (!lastLookup || now-lastLookup>=1000) {
                stash=object(contextResult(inventoryLibrary,"GetStashInventoryComponent",pawn.address));
                manager=object(contextResult(aiLibrary,"GetAIManager",pawn.address));lastLookup=now;
            }
            updateGrenades();updateDurability();
            if (killRequested.exchange(false)) {
                int before=0;eachEnemy([&](auto,auto,auto,auto){++before;});
                // This build exposes the native AI-manager route on GameInstance.
                if (alive(manager) && invoke(instance,"KillAllAI",nullptr,0)) {
                    int remaining=0;eachEnemy([&](auto,auto,auto,auto){++remaining;});
                    killed=(std::max)(0,before-remaining);
                    char line[128]{};std::snprintf(line,sizeof(line),"KillAllAI before=%d remaining=%d killed=%d\r\n",before,remaining,killed.load());log(line);
                } else {killed=-3;log("KillAllAI unavailable in current world\r\n");}
            }
        }
        if (++tickCount==1 || tickCount%600==0) {
            char line[200]{};std::snprintf(line,sizeof(line),"weapon-v3 tick=%llu inventory=%p equipped=%p stash=%p manager=%p oneHit=on-hit-only\r\n",
                static_cast<unsigned long long>(tickCount),reinterpret_cast<void*>(inventory.address),reinterpret_cast<void*>(equipped.address),
                reinterpret_cast<void*>(stash.address),reinterpret_cast<void*>(manager.address));log(line);
        }
        if (!inventoryLibrary || !aiLibrary || !missionLibrary) setStatus("游戏接口未就绪");
        else if (std::abs(active.melee-1.f)>.0001f && !isA(equipped.address,"MeleeComponent"))
            setStatus("武器功能已连接；近战攻速等待手持近战武器");
        else setStatus("");
    }
} runtime;

void __fastcall processEventImplementation(void* target,void* function,void* parameters) {
    CallGuard guard;
    if (runtime.enabled.load() && runtime.infiniteGrenades.load() && function && parameters) {
        const auto functionName=runtime.objectName(reinterpret_cast<uintptr_t>(function));
        const auto data=reinterpret_cast<uintptr_t>(parameters);
        bool consumed=false;
        if (functionName=="RequestConsumeItem" || functionName=="TryConsumeItem" ||
            functionName=="RequestDropItem") {
            const auto owner=get<uintptr_t>(data);
            const auto item=data+8;
            consumed=owner==runtime.localInventory.load() && runtime.isGrenade(item);
            if (consumed) put(data+0xf0,int32_t(0));
        } else if (functionName=="UpdateItemStack") {
            const auto owner=runtime.localInventory.load();
            consumed=owner && owner==reinterpret_cast<uintptr_t>(target) && runtime.isGrenade(data);
            if (consumed) put(data+0xe8,get<int32_t>(data+0xd8));
        } else if (functionName=="RemoveItem") {
            const auto owner=runtime.localInventory.load();
            consumed=owner && owner==reinterpret_cast<uintptr_t>(target) && runtime.isGrenade(data);
            if (consumed) { put(data+0xea,uint8_t(1)); return; }
        }
        if (consumed) {
            char line[160]{};std::snprintf(line,sizeof(line),"grenade non-consume ProcessEvent=%s\r\n",functionName.c_str());log(line);
        }
    }
    originalProcessEvent(target,function,parameters);
}
void __fastcall tickImplementation(void* component,float delta,uint32_t tickType,void* tickFunction) {
    CallGuard guard;
    runtime.tick(reinterpret_cast<uintptr_t>(component),false);
    originalTick(component,delta,tickType,tickFunction);
    runtime.tick(reinterpret_cast<uintptr_t>(component),true);
}
float __fastcall recoilImplementation(void* component) {
    CallGuard guard;const auto value=originalRecoil(component);
    return reinterpret_cast<uintptr_t>(component)==runtime.localWeapon.load() && runtime.noRecoil.load() ? 0.f : value;
}
float __fastcall multiplierImplementation(void* component,void* argument) {
    CallGuard guard;const auto value=originalMultiplier(component,argument);
    return reinterpret_cast<uintptr_t>(component)==runtime.localWeapon.load() && runtime.noRecoil.load() ? 0.f : value;
}
float __fastcall spreadImplementation(void* component) {
    CallGuard guard;const auto value=originalSpread(component);
    return reinterpret_cast<uintptr_t>(component)==runtime.localWeapon.load() && runtime.noSpread.load() ? 0.f : value;
}
bool __fastcall stackImplementation(void* inventory,void* item,int32_t count) {
    CallGuard guard;
    if (runtime.enabled.load() && runtime.infiniteGrenades.load() &&
        reinterpret_cast<uintptr_t>(inventory)==runtime.localInventory.load()) {
        std::lock_guard lock(runtime.stateMutex);
        const auto address=reinterpret_cast<uintptr_t>(item);const auto old=get<int32_t>(address+0xd8);
        if (count>=0 && count<old && runtime.isGrenade(address)) {
            char line[128]{};std::snprintf(line,sizeof(line),"grenade carried old=%d requested=%d restored=%d\r\n",old,count,old);log(line);
            count=old;++runtime.grenadeRestores;
        }
    }
    // The original owns and destroys this by-value ContainerItem. Its unchanged
    // count path also protects the final grenade without leaking its members.
    return originalStack(inventory,item,count);
}
void __fastcall bulletDamageImplementation(void* component,void* actor,float damage,void* instigator,
    const Vec3* location,uint64_t bone,const Vec3* direction,void* type,void* causer,void* itemData) {
    CallGuard guard;
    damage=runtime.damageForHit(reinterpret_cast<uintptr_t>(component),reinterpret_cast<uintptr_t>(actor),
        reinterpret_cast<uintptr_t>(instigator),damage);
    originalBulletDamage(component,actor,damage,instigator,location,bone,direction,type,causer,itemData);
}
void __fastcall pointDamageImplementation(void* component,void* actor,float damage,void* instigator,
    const Vec3* location,void* hitComponent,uint64_t bone,const Vec3* direction,void* type,void* causer) {
    CallGuard guard;
    damage=runtime.damageForHit(reinterpret_cast<uintptr_t>(component),reinterpret_cast<uintptr_t>(actor),
        reinterpret_cast<uintptr_t>(instigator),damage);
    originalPointDamage(component,actor,damage,instigator,location,hitComponent,bone,direction,type,causer);
}
LRESULT CALLBACK cleanupWndProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    if (message==cleanupMessage && wparam==reinterpret_cast<WPARAM>(&runtime)) {
        std::lock_guard lock(runtime.stateMutex);runtime.reset();runtime.cleanDone=true;return 0;
    }
    return CallWindowProcW(runtime.previousWndProc,window,message,wparam,lparam);
}
bool createHook(uintptr_t rva,const unsigned char* prefix,size_t count,void* detour,void** original) {
    auto target=reinterpret_cast<void*>(runtime.module+rva);
    if (std::memcmp(target,prefix,count)!=0 || MH_CreateHook(target,detour,original)!=MH_OK) return false;
    runtime.hooks.push_back(target);return true;
}
}

bool WeaponHooks::initialize(uintptr_t module) {
    if (runtime.enabled.load()) return true;
    const auto dos=get<IMAGE_DOS_HEADER>(module);
    if (dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<=0 || dos.e_lfanew>0x1000) return false;
    const auto pe=get<IMAGE_NT_HEADERS64>(module+dos.e_lfanew);
    if (pe.Signature!=IMAGE_NT_SIGNATURE || pe.FileHeader.TimeDateStamp!=timestamp || pe.OptionalHeader.SizeOfImage!=imageSize) {
        runtime.setStatus("游戏版本与已验证武器接口不符");return false;
    }
    runtime.module=module;if (runtime.name(0)!="None") return false;
    const auto result=MH_Initialize();if (result!=MH_OK && result!=MH_ERROR_ALREADY_INITIALIZED) return false;
    const auto processEvent=runtime.processEventTarget();
    if (!processEvent || MH_CreateHook(reinterpret_cast<void*>(processEvent),&processEventImplementation,
        reinterpret_cast<void**>(&originalProcessEvent))!=MH_OK) {
        runtime.setStatus("ProcessEvent 接口校验失败");return false;
    }
    runtime.hooks.push_back(reinterpret_cast<void*>(processEvent));
    constexpr unsigned char tick[]{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57};
    constexpr unsigned char normal[]{0x48,0x89,0x5c,0x24,0x08,0x57};
    constexpr unsigned char multiplier[]{0x48,0x89,0x74,0x24,0x10,0x57};
    constexpr unsigned char stack[]{0x48,0x89,0x54,0x24,0x10,0x55,0x53,0x41,0x56,0x41,0x57};
    constexpr unsigned char bullet[]{0x48,0x8b,0xc4,0x48,0x89,0x58,0x08,0x48,0x89,0x68,0x10};
    constexpr unsigned char point[]{0xf3,0x0f,0x11,0x54,0x24,0x18,0x4c,0x8b,0xdc};
    if (!createHook(coreTickRva,tick,sizeof(tick),&tickImplementation,reinterpret_cast<void**>(&originalTick)) ||
        !createHook(recoilRva,normal,sizeof(normal),&recoilImplementation,reinterpret_cast<void**>(&originalRecoil)) ||
        !createHook(multiplierRva,multiplier,sizeof(multiplier),&multiplierImplementation,reinterpret_cast<void**>(&originalMultiplier)) ||
        !createHook(spreadRva,normal,sizeof(normal),&spreadImplementation,reinterpret_cast<void**>(&originalSpread)) ||
        !createHook(stackRva,stack,sizeof(stack),&stackImplementation,reinterpret_cast<void**>(&originalStack)) ||
        !createHook(bulletDamageRva,bullet,sizeof(bullet),&bulletDamageImplementation,reinterpret_cast<void**>(&originalBulletDamage)) ||
        !createHook(pointDamageRva,point,sizeof(point),&pointDamageImplementation,reinterpret_cast<void**>(&originalPointDamage))) {
        for (auto target:runtime.hooks) MH_RemoveHook(target);
        runtime.hooks.clear();runtime.setStatus("武器接口校验失败");return false;
    }
    EnumWindows([](HWND window,LPARAM)->BOOL{
        DWORD pid{};GetWindowThreadProcessId(window,&pid);wchar_t name[128]{};GetClassNameW(window,name,128);
        if (pid==GetCurrentProcessId() && std::wcscmp(name,L"UnrealWindow")==0) {runtime.window=window;return FALSE;}
        return TRUE;
    },0);
    if (runtime.window) runtime.previousWndProc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(runtime.window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&cleanupWndProc)));
    runtime.enabled=true;
    for (auto target:runtime.hooks) if (MH_EnableHook(target)!=MH_OK) {shutdown();return false;}
    runtime.setStatus("等待主角进入关卡");
    log("weapon-v3 enabled; oneHit=local-hit-damage-only firearm-rate=unmodified\r\n");return true;
}
void WeaponHooks::update(const Features& f) {
    runtime.requestedGameSpeed=std::isfinite(f.gameSpeedMultiplier)
        ? std::clamp(f.gameSpeedMultiplier,.1f,5.f) : 1.f;
    runtime.requestedWeatherIndex=std::clamp(f.weatherIndex,0,255);
    std::lock_guard lock(runtime.requestMutex);
    runtime.requested={f.weaponInfiniteAmmo,f.weaponInfiniteGrenades,f.weaponInfiniteDurability,
        f.weaponNoRecoil,f.weaponNoSpread,f.weaponOneHitKill,f.instantSearch,
        std::clamp(f.meleeAttackSpeedMultiplier,.5f,5.f)};
}
void WeaponHooks::requestKillAll() {runtime.killed=-2;runtime.killRequested=true;}
void WeaponHooks::requestAddMoney(int amount) {
    runtime.addMoneyAmount=std::clamp(amount,1,100000000);runtime.addMoneyRequested=true;
    runtime.setActionStatus("金钱请求已排队");log("UI queued add-money request\r\n");
}
void WeaponHooks::requestAddCoins(int amount) {
    runtime.addCoinsAmount=std::clamp(amount,1,100000000);runtime.addCoinsRequested=true;
    runtime.setActionStatus("硬币请求已排队");log("UI queued add-coins request\r\n");
}
void WeaponHooks::requestAddExperience(int amount) {
    runtime.addExperienceAmount=std::clamp(amount,1,100000000);runtime.addExperienceRequested=true;
    runtime.setActionStatus("经验请求已排队");log("UI queued add-XP request\r\n");
}
void WeaponHooks::requestAddResource(HideoutResource resource, int amount) {
    const auto index = static_cast<size_t>(resource);
    if (index >= resources.size()) return;
    runtime.setActionStatus("资源添加请求已排队");
    runtime.addResourceAmounts[index] = std::clamp(amount, 1, 100000000);
}
void WeaponHooks::requestAddReputation(Faction faction, float points) {
    const auto index = static_cast<size_t>(faction);
    if (index >= factions.size() || !std::isfinite(points) || points <= 0.f) return;
    runtime.setActionStatus("声望添加请求已排队");
    runtime.addReputationPoints[index] = std::min(points, 100.f);
}
void WeaponHooks::requestFreeHideoutUpgrade() {
    runtime.freeUpgradeRequested=true;runtime.setActionStatus("设施升级请求已排队");log("UI queued hideout-upgrade request\r\n");
}
void WeaponHooks::requestSetTimeOfDay(int hour) {runtime.requestedHour=std::clamp(hour,0,23);}
void WeaponHooks::requestSetWeather(int index) {
    runtime.requestedWeatherIndex=std::clamp(index,0,255);
    runtime.weatherRequestRequested=true;
}
void WeaponHooks::refreshWorldTime() {runtime.currentWorldMinute=-1;runtime.worldTimeRefreshRequested=true;}
std::vector<std::string> WeaponHooks::weatherOptions() const {
    std::lock_guard lock(runtime.stateMutex);
    std::vector<std::string> result;result.reserve(runtime.weatherEntries.size());
    for (const auto& option:runtime.weatherEntries) result.push_back(option.label);
    return result;
}
std::string WeaponHooks::weatherStatus() const {
    std::lock_guard lock(runtime.statusMutex);return runtime.weatherStatusText;
}
int WeaponHooks::currentWorldMinute() const {return runtime.currentWorldMinute.load();}
int WeaponHooks::lastKillCount() const {return runtime.killed.load();}
std::string WeaponHooks::status() const {
    std::lock_guard lock(runtime.statusMutex);
    if (runtime.statusText.empty()) return runtime.actionStatus;
    return runtime.actionStatus.empty() ? runtime.statusText : runtime.statusText+" | "+runtime.actionStatus;
}
void WeaponHooks::shutdown() {
    if (!runtime.enabled.load()) return;
    runtime.cleaning=true;
    if (runtime.window && runtime.previousWndProc) {
        DWORD_PTR ignored{};SendMessageTimeoutW(runtime.window,cleanupMessage,reinterpret_cast<WPARAM>(&runtime),0,SMTO_ABORTIFHUNG|SMTO_BLOCK,3000,&ignored);
    }
    if (!runtime.cleanDone.load()) {std::lock_guard lock(runtime.stateMutex);runtime.reset();}
    runtime.enabled=false;runtime.noRecoil=false;runtime.noSpread=false;runtime.infiniteGrenades=false;runtime.oneHit=false;
    for (auto target:runtime.hooks) MH_DisableHook(target);
    while (inFlight.load()) Sleep(1);
    for (auto target:runtime.hooks) MH_RemoveHook(target);
    runtime.hooks.clear();
    if (runtime.window && runtime.previousWndProc && GetWindowLongPtrW(runtime.window,GWLP_WNDPROC)==reinterpret_cast<LONG_PTR>(&cleanupWndProc))
        SetWindowLongPtrW(runtime.window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(runtime.previousWndProc));
    log("weapon-v3 infinite ammo disabled and melee montage rates restored; hooks removed\r\n");
}
