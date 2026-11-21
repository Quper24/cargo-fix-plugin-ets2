#include "coaches.h"
#include "coach_route.h"
#include "custom_jobs.h"
#include "custom_job_names.h"
#include "owned_offers.h"
#include "overlay_ui.h"
#include "notifications.h"
#include "ui_controls.h"
#include <Windows.h>
#include <imgui.h>
#include <atomic>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>

namespace coaches {
namespace {
using namespace custom_jobs;
std::mutex mutex;
std::shared_ptr<const View> view=std::make_shared<View>();
std::optional<Request> pending;
std::atomic<bool> visible{false};
bool supported=false;
std::uintptr_t base=0,pools=0,last_economy=0;
GameProfile core{};CreationCode allocation{};CoachesCode code{};CatalogNameCode names{};
void(*log)(const char*)=nullptr;
std::uint64_t epoch=0;
std::map<std::string,std::uintptr_t> stops;
struct Batch {std::uintptr_t economy=0;std::vector<std::uintptr_t> stops;std::size_t index=0,changed=0,kept=0;} batch;
bool Memory(std::uintptr_t at,void* out,std::size_t n){if(!ReadableMemory(at,n))return false;std::memcpy(out,reinterpret_cast<const void*>(at),n);return true;}
template<class T>T Get(std::uintptr_t at){T value{};Read(Memory,at,value);return value;}
template<class T>void Put(std::uintptr_t at,T value){std::memcpy(reinterpret_cast<void*>(at),&value,sizeof(value));}
template<class T>T Native(std::uint32_t rva){return reinterpret_cast<T>(base+rva);}
void Publish(std::shared_ptr<View> next){std::lock_guard<std::mutex> lock(mutex);view=std::move(next);}
void Message(const char* en,const char* ru){
    const auto text=notifications::Russian()?ru:en;
    if(log)log(en);
    const int n=MultiByteToWideChar(CP_UTF8,0,text,-1,nullptr,0);std::wstring wide(n,L'\0');
    if(n){MultiByteToWideChar(CP_UTF8,0,text,-1,wide.data(),n);wide.resize(n-1);}notifications::Show(wide);
    auto next=std::make_shared<View>(*Snapshot());next->message=text;Publish(next);
}
bool Content(std::uintptr_t economy){
    if(!supported||!LiveUnit(Memory,economy)||!LiveUnit(Memory,Get<std::uintptr_t>(economy+0x1d58)))return false;
    const auto count=Get<std::uint64_t>(economy+0x1d70),data=Get<std::uintptr_t>(economy+0x1d68);
    return count&&count<=100000&&ReadableMemory(data,count*8)&&Get<std::uintptr_t>(economy+0x168)&&Get<std::uintptr_t>(economy+0x7c0);
}
bool Station(std::uintptr_t stop){return LiveUnit(Memory,stop)&&ReadableMemory(stop,0x90)&&Get<unsigned>(stop+0x44)==0;}
bool Offers(std::uintptr_t stop,GameOfferVector*& result){
    result=reinterpret_cast<GameOfferVector*>(stop+0x68);
    return Station(stop)&&ReadableMemory(stop+0x68,sizeof(*result))&&ValidOfferVector(*result)&&result->count<=1024;
}
bool Models(std::uintptr_t economy,std::uintptr_t stop){
    auto context=Get<std::uintptr_t>(stop+0x60);if(!ReadableMemory(context,0x9b))return false;
    const auto vector=economy+(Get<unsigned char>(context+0x9a)?0x1db0:0x1d88);
    const auto count=Get<std::uint64_t>(vector+16),data=Get<std::uintptr_t>(vector+8);
    return count&&count<=1024&&ReadableMemory(data,count*32);
}
bool Complete(std::uintptr_t offer){
    if(!LiveUnit(Memory,offer)||!ReadableMemory(offer,0x148))return false;
    const auto count=Get<std::uint64_t>(offer+0x30);
    if(!count||count>MaxPoints-1||!ReadableMemory(Get<std::uintptr_t>(offer+0x28),count*16))return false;
    for(auto offset:{0x48,0x70,0x98,0xc0}){
        if(Get<std::uint64_t>(offer+offset+16)!=count||!ReadableMemory(Get<std::uintptr_t>(offer+offset+8),count*4))return false;
    }
    return true;
}
void Release(std::uintptr_t unit,std::uintptr_t table){
    if(unit){GameOfferVector one{table,&unit,1,1};Native<void(__fastcall*)(GameOfferVector*)>(code.clear_vector)(&one);}
}
void Schedule(std::uintptr_t economy,std::uintptr_t stop,GameOfferVector& offers){
    const auto now=Get<unsigned>(economy+0x19c);
    for(std::uint64_t i=0;i<offers.count;++i){
        auto expiry=Get<unsigned>(offers.data[i]+0x110);
        // Empty native slots retry after the native minimum lifetime.
        if(expiry<=now)expiry=now+60;
        Native<void(__fastcall*)(std::uintptr_t,std::uintptr_t,unsigned,std::uintptr_t,unsigned)>(code.schedule)
            (Get<std::uintptr_t>(economy+0x168),economy,expiry,stop,static_cast<unsigned>(i));
    }
}
// Clear retains this native vector's storage. Refcounts transfer the prepared
// list across native selection cleanup and removal of all old station events.
bool Install(std::uintptr_t economy,std::uintptr_t stop,std::size_t slot,std::uintptr_t replacement){
    GameOfferVector* offers=nullptr;
    if(!Offers(stop,offers)||slot>=offers->count)return false;
    std::vector<std::uintptr_t> prepared(offers->data,offers->data+offers->count);
    for(std::size_t i=0;i<prepared.size();++i){
        if(!LiveUnit(Memory,prepared[i]))return false;
    }
    for(std::size_t i=0;i<prepared.size();++i)if(i!=slot)
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(prepared[i]+8));
    prepared[slot]=replacement;
    Native<void(__fastcall*)(std::uintptr_t,std::uintptr_t)>(code.clear)(stop,economy);
    std::copy(prepared.begin(),prepared.end(),offers->data);offers->count=prepared.size();
    Schedule(economy,stop,*offers);return true;
}
std::string Label(std::uintptr_t unit,unsigned offset){
    auto text=ReadNativeName(Memory,unit,offset);
    if(text.find("@@")==std::string::npos)return text;
    if(!names.localize)return {};
    NativeNameBuffer buffer(base+names.buffer_vtable);const char* input=text.c_str();
    if(!Native<bool(__fastcall*)(NativeNameBuffer*,const char**)>(names.localize)(&buffer,&input)||buffer.data!=buffer.bytes.data()||buffer.length>=buffer.bytes.size()||buffer.bytes[buffer.length])return {};
    return std::string(buffer.bytes.data(),buffer.length);
}
std::uintptr_t Route(std::uintptr_t source,std::uintptr_t target){
    const auto count=Get<std::uint64_t>(source+0x28),data=Get<std::uintptr_t>(source+0x20);
    if(count>100000||!ReadableMemory(data,count*24))return 0;
    for(std::uint64_t i=0;i<count;++i){
        const auto row=data+i*24;
        if(Get<std::uintptr_t>(row)==target&&Get<std::uint16_t>(row+8)&&Get<std::uint16_t>(row+16))return row;
    }
    return 0;
}
struct Planner {
    alignas(16) std::array<unsigned char,0x128> data{};
    bool initialized=false;
    ~Planner(){if(initialized){
        data[0xf8]=0;
        for(auto offset:{0xc8,0xa0,0x78,0x50})Native<void(__fastcall*)(void*)>(code.free_numbers)(data.data()+offset);
        Native<void(__fastcall*)(void*)>(code.free_targets)(data.data()+0x28);
    }}
};
std::uintptr_t Allocate(){
    const auto index=Get<unsigned>(base+allocation.allocator_index);
    const auto pool=index<4?Get<std::uintptr_t>(base+allocation.allocator_array+index*8):0;
    if(!ReadableMemory(pool,24))return 0;
    auto offer=Native<std::uintptr_t(__fastcall*)(std::uintptr_t)>(code.factory)(pool);
    if(offer)InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(offer+8),0x20000001);
    return offer;
}
}
std::shared_ptr<const View> Snapshot(){std::lock_guard<std::mutex> lock(mutex);return view;}
bool Visible(){return visible;}
#ifdef CARGO_CONTROL_CATALOG_PREVIEW
void Preview(View fixture){visible=true;fixture.available=fixture.loaded=fixture.scanned=true;Publish(std::make_shared<View>(std::move(fixture)));}
#endif
void Initialize(std::uintptr_t module,std::size_t size,GameKind game,const GameProfile& profile,const CreationCode& alloc,void(*logger)(const char*)){
    base=module;core=profile;allocation=alloc;log=logger;visible=game==GameKind::ETS2;
    std::string reason;std::uint32_t pool_rva=0;
    supported=ResolveCoachesCode(reinterpret_cast<const unsigned char*>(base),size,game,code,reason)&&
        alloc.allocator_array&&ResolveCatalogPools(reinterpret_cast<const unsigned char*>(base),size,pool_rva,reason);
    pools=pool_rva?base+pool_rva:0;
    if(supported)ResolveCatalogNames(reinterpret_cast<const unsigned char*>(base),size,names,reason);
    if(log&&visible)log((supported?"Coaches native code/flow checks passed.":"Coaches disabled: "+reason).c_str());
    Reset();
}
void Reset(){batch={};stops.clear();last_economy=0;++epoch;
    {std::lock_guard<std::mutex> lock(mutex);pending.reset();}
    auto next=std::make_shared<View>();next->available=supported;next->session=epoch;Publish(next);
}
void Observe(std::uintptr_t economy,bool ready){
    if(!visible)return;
    if(!ready){std::lock_guard<std::mutex> lock(mutex);pending.reset();}
    if(last_economy&&(!ready||last_economy!=economy))Reset();
    last_economy=ready?economy:0;
    const auto loaded=ready&&Content(economy);auto old=Snapshot();
    if(old->loaded!=loaded){auto next=std::make_shared<View>(*old);next->loaded=loaded;Publish(next);}
}
void Scan(std::uintptr_t economy){
    if(!Content(economy)){Message("Coaches content is not loaded.","Данные Coaches не загружены. Загрузите профиль с подключённым DLC.");return;}
    std::vector<std::uintptr_t> all;
    if(!PointerList(Memory,economy,0x1d68,100000,all))return;
    {std::lock_guard<std::mutex> lock(mutex);pending.reset();}
    stops.clear();++epoch;auto next=std::make_shared<View>();next->available=next->loaded=next->scanned=true;next->session=epoch;
    for(auto stop:all){
        if(!Station(stop))continue;
        std::vector<std::string> parts;
        if(!UnitName(Memory,pools,stop,parts)||parts.size()<3)continue;
        const auto id=Join(parts);auto name=parts.back();
        // The stop stores its city/location unit; use the loaded localized name,
        // including overrides supplied by map/translation mods.
        auto location=Get<std::uintptr_t>(stop+0x48);
        if(LiveUnit(Memory,location)){
            auto translated=Label(location,Get<unsigned char>(base+names.city_localized_flag)?0x30:0x10);
            if(!translated.empty())name=translated;
        }
        stops.emplace(id,stop);next->stops.push_back({id,name});
    }
    std::sort(next->stops.begin(),next->stops.end(),[](const auto& a,const auto& b){return a.name<b.name;});Publish(next);
    if(log)log(("Coaches catalog: stations="+std::to_string(stops.size())).c_str());
}
bool Queue(const Request& request){
    if(!ValidRoute(request.stops))return false;
    std::lock_guard<std::mutex> lock(mutex);if(pending)return false;pending=request;return true;
}
void Create(std::uintptr_t economy){
    std::optional<Request> request;{std::lock_guard<std::mutex> lock(mutex);request=std::move(pending);pending.reset();}
    if(!Content(economy)||!request||request->session!=epoch||!ValidRoute(request->stops)){
        Message("Read the Coaches session again.","Повторно считайте сессию Coaches.");return;
    }
    const auto settings=Get<std::uintptr_t>(economy+0x1d58);
    const auto traffic_policy=Get<unsigned>(settings+0x3c);
    std::vector<std::uintptr_t> ordered,rows;
    for(const auto& id:request->stops){
        auto found=stops.find(id);
        if(found==stops.end()||!Station(found->second)){Message("A station is no longer available. Read the session again.","Остановка больше недоступна. Считайте сессию повторно.");return;}
        ordered.push_back(found->second);
    }
    for(std::size_t i=1;i<ordered.size();++i){
        auto row=Route(ordered[i-1],ordered[i]);
        if(!row){
            if(log)log(("Coaches route cache missing: "+request->stops[i-1]+" -> "+request->stops[i]).c_str());
            Message("No drivable route between two selected stations.","Между выбранными остановками нет данных проезжаемого маршрута.");return;
        }
        auto a=Get<std::uintptr_t>(ordered.front()+0x60),b=Get<std::uintptr_t>(ordered[i]+0x60);
        if(!ReadableMemory(a,0x9b)||!ReadableMemory(b,0x9b))return;
        const auto opposite=Get<unsigned char>(a+0x9a)!=Get<unsigned char>(b+0x9a);
        // The stock planner stops after crossing when policy == 1; policy == 2
        // forbids the crossing. Preserve that rule for the manually chosen bus.
        if(opposite&&(traffic_policy==2||(traffic_policy==1&&i+1<ordered.size()))){
            Message("The game's driving-side policy rejects this sequence of stations.","Правила игры для стороны движения не допускают такую последовательность остановок.");return;
        }
        rows.push_back(row);
    }
    const auto from=ordered.front();GameOfferVector* offers=nullptr;
    if(!Offers(from,offers)||!offers->count||!Models(economy,from)){
        Message("Refresh Coaches offers first. No offer slot or suitable bus is available at departure.","Сначала обновите рейсы Coaches. На станции отправления нет слота или подходящего автобуса.");return;
    }
    const auto now=Get<unsigned>(economy+0x19c);
    if(now>UINT32_MAX-Get<unsigned>(settings+0x88)-1440)return;
    auto offer=Allocate();if(!offer){Message("Cannot allocate a coach offer.","Не удалось выделить автобусное предложение.");return;}
    Planner planner;
    Native<void(__fastcall*)(void*,std::uintptr_t,std::uintptr_t,std::uintptr_t,unsigned)>(code.planner_init)
        (planner.data.data(),economy,settings,from,0);
    planner.initialized=true;
    // Private draft limits only: never patch the global 10-stop generator setting.
    Put<unsigned>(reinterpret_cast<std::uintptr_t>(planner.data.data())+0x1c,static_cast<unsigned>(rows.size()));
    Put<unsigned>(reinterpret_cast<std::uintptr_t>(planner.data.data())+0x20,static_cast<unsigned>(rows.size()));
    Native<void(__fastcall*)(void*,std::uintptr_t,unsigned)>(code.prepare)(planner.data.data(),offer,now);
    for(auto row:rows)Native<bool(__fastcall*)(void*,std::uintptr_t)>(code.append)(planner.data.data(),row);
    bool valid=Get<std::uint64_t>(reinterpret_cast<std::uintptr_t>(planner.data.data())+0x38)==rows.size()&&
        Native<bool(__fastcall*)(void*)>(code.validate)(planner.data.data());
    if(log)log(("Coaches draft: requested legs="+std::to_string(rows.size())+", built="+
        std::to_string(Get<std::uint64_t>(reinterpret_cast<std::uintptr_t>(planner.data.data())+0x38))+", valid="+std::to_string(valid)).c_str());
    if(valid){
        Native<void(__fastcall*)(void*,std::uintptr_t)>(code.commit)(planner.data.data(),offer);
        valid=Complete(offer)&&Get<std::uintptr_t>(offer+0x18)==from&&!ReadNativeName(Memory,offer,0xf0).empty();
    }
    // Verify the native writer retained the entire ordered route, not a prefix.
    if(valid){
        auto data=Get<std::uintptr_t>(offer+0x28);
        for(std::size_t i=1;i<ordered.size()&&valid;++i){
            std::array<std::uint64_t,2> name{};
            Native<void*(__fastcall*)(void*,std::uintptr_t)>(code.stop_name)(name.data(),ordered[i]);
            valid=Get<std::uint64_t>(data+(i-1)*16)==name[0]&&Get<std::uint64_t>(data+(i-1)*16+8)==name[1];
        }
    }
    std::size_t slot=0;
    for(std::size_t i=0;i<offers->count;++i)if(Get<unsigned>(offers->data[i]+0x10)==0){slot=i;break;}
    if(!valid||!Install(economy,from,slot,offer)){
        Release(offer,offers->vtable);Message("The coach route was rejected. Existing offers were preserved.","Не удалось создать рейс. Прежние предложения сохранены.");return;
    }
    if(log)log(("Coaches manual offer installed: points="+std::to_string(ordered.size())+", slot="+std::to_string(slot)+", source="+request->stops.front()).c_str());
    Message("Coach route created. Choose it at the departure station in the appropriate bus market.","Рейс создан. Выберите его на станции отправления на рынке для своего или предоставленного автобуса.");
}
void Refresh(std::uintptr_t economy){
    if(!Content(economy)){Message("Coaches content is not loaded.","Данные Coaches не загружены.");return;}
    std::vector<std::uintptr_t> all;if(!PointerList(Memory,economy,0x1d68,100000,all))return;
    batch={};batch.economy=economy;
    for(auto stop:all)if(Station(stop)&&Models(economy,stop))batch.stops.push_back(stop);
    overlay_ui::SetProgress(0,batch.stops.size());
    if(batch.stops.empty()){batch={};Message("No coach departure stations are available.","Нет доступных станций отправления Coaches.");}
}
bool Busy(){return batch.economy!=0;}
void Tick(std::uintptr_t economy){
    if(!Busy())return;
    if(batch.economy!=economy||!Content(economy)){batch={};Message("Coach refresh interrupted: loaded session changed.","Обновление рейсов остановлено: загруженная сессия изменилась.");return;}
    if(batch.index<batch.stops.size()){
        auto stop=batch.stops[batch.index++];GameOfferVector* offers=nullptr;
        if(Offers(stop,offers)&&Models(economy,stop)){
            std::vector<std::uintptr_t> previous;
            if(offers->count)previous.assign(offers->data,offers->data+offers->count);
            const bool valid=std::all_of(previous.begin(),previous.end(),[](auto unit){return LiveUnit(Memory,unit);});
            if(valid){
                const auto table=offers->vtable;
                for(auto unit:previous)InterlockedIncrement(reinterpret_cast<volatile LONG*>(unit+8));
                Native<void(__fastcall*)(std::uintptr_t,std::uintptr_t)>(code.clear)(stop,economy);
                Native<void(__fastcall*)(std::uintptr_t,std::uintptr_t)>(code.fill)(stop,economy);
                bool added=false;
                if(Offers(stop,offers))for(std::uint64_t i=0;i<offers->count;++i)if(Complete(offers->data[i]))added=true;
                if(!added){
                    // Native clear keeps capacity; generation may grow it but
                    // does not shrink it. Transfer retained units back on failure.
                    Native<void(__fastcall*)(std::uintptr_t,std::uintptr_t)>(code.clear)(stop,economy);
                    std::copy(previous.begin(),previous.end(),offers->data);offers->count=previous.size();
                    Schedule(economy,stop,*offers);++batch.kept;
                }else{
                    for(auto unit:previous)Release(unit,table);
                    ++batch.changed;
                }
            }else ++batch.kept;
        }else ++batch.kept;
        overlay_ui::SetProgress(batch.index,batch.stops.size());
    }
    if(batch.index==batch.stops.size()){
        if(log)log(("Coaches native refresh completed: stations="+std::to_string(batch.index)+", generated="+std::to_string(batch.changed)+", empty_or_skipped="+std::to_string(batch.kept)).c_str());
        const auto changed=batch.changed;batch={};
        Message(changed?"Coach offers refreshed. Reopen the bus market to see the new routes.":"The game generated no coach routes. Check the available stations and Coaches content.",
            changed?"Автобусные рейсы обновлены. Заново откройте рынок, чтобы увидеть новые предложения.":"Игра не создала автобусных рейсов. Проверьте доступные станции и данные Coaches.");
    }
}
void Draw(float scale,bool russian){
    auto tr=[&](const char* en,const char* ru){return russian?ru:en;};const auto state=Snapshot();
    ImGui::SetWindowFontScale(.5f*scale);
    if(!state->available){ImGui::TextWrapped("%s",tr("Coaches code is incompatible with this game build. See game.log.txt.","Код Coaches несовместим с этой сборкой игры. Подробности в game.log.txt."));return;}
    if(!state->loaded){ImGui::TextWrapped("%s",tr("Load a profile with Coaches content enabled.","Загрузите профиль с подключённым Coaches."));return;}
    if(controls::PrimaryButton(tr("Read Coaches session","Считать сессию Coaches"),{280*scale,40*scale}))overlay_ui::Request(8);
    controls::Hint(tr("The window closes while reading and reopens automatically.","На время чтения окно закроется и откроется автоматически."));
    ImGui::SameLine(0,14*scale);
    if(controls::PrimaryButton(tr("Refresh all coach offers","Обновить все автобусные рейсы"),{310*scale,40*scale}))overlay_ui::Request(9);
    controls::Hint(tr("Replace offers at all stations with routes from the game generator. Reopen the market afterwards.","Заменить предложения всех станций рейсами штатного генератора. После завершения заново откройте рынок."));
    ImGui::TextWrapped("%s",tr("Game generator: up to 10 points, subject to skills. Manual route: 2-24 points including departure and final stop.",
        "Штатная генерация: до 10 точек с учётом навыков. Ручной маршрут: 2-24 точки, включая отправление и конечную."));
    ImGui::TextWrapped("%s",tr("Accept the route at its departure station, using your own or a provided bus.",
        "Рейс можно принять на станции отправления на своём или предоставленном автобусе."));
    if(!state->scanned)ImGui::TextWrapped("%s",tr("Read the session to choose stations. The window will reopen automatically.","Считайте сессию для выбора остановок. Окно откроется автоматически."));
    if(!state->scanned)return;
    static std::uint64_t seen=UINT64_MAX;static std::vector<std::string> route;static std::string selected;static char search[128]{};
    if(seen!=state->session){seen=state->session;route.clear();selected.clear();search[0]=0;
#ifdef CARGO_CONTROL_CATALOG_PREVIEW
        route=state->preview_route;
#endif
    }
    ImGui::Text(tr("Stations: %zu  |  Route: %zu / 24","Станций: %zu  |  Маршрут: %zu из 24"),state->stops.size(),route.size());
    ImGui::SetNextItemWidth(-1);ImGui::InputTextWithHint("##coach-search",tr("Search city or station","Поиск города или станции"),search,sizeof(search));
    const Stop* choice=nullptr;for(const auto& stop:state->stops)if(stop.id==selected)choice=&stop;
    ImGui::SetNextItemWidth(-180*scale);
    if(ImGui::BeginCombo("##coach-station",choice?choice->name.c_str():tr("Choose station...","Выберите остановку..."))){
        bool found=false;
        for(const auto& stop:state->stops){
            if(std::find(route.begin(),route.end(),stop.id)!=route.end()||!ContainsName(stop.name,search))continue;
            found=true;
            ImGui::PushID(stop.id.c_str());if(ImGui::Selectable(stop.name.c_str(),stop.id==selected)){selected=stop.id;search[0]=0;}
            ImGui::PopID();
        }controls::NoMatches(found,russian);ImGui::EndCombo();
    }
    ImGui::SameLine();ImGui::BeginDisabled(selected.empty()||route.size()>=MaxPoints);
    if(ImGui::Button(tr("Add stop","Добавить"),{160*scale,28*scale})){route.push_back(selected);selected.clear();search[0]=0;}ImGui::EndDisabled();
    controls::Hint(route.size()>=MaxPoints?tr("Maximum: 24 points. Remove a stop to add another.","Максимум 24 точки. Удалите остановку, чтобы добавить другую."):tr("The first point is departure; the last is the final stop.","Первая точка - отправление, последняя - конечная остановка."));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{10*scale,8*scale});
    ImGui::BeginChild("coach-route",{0,120*scale},true,ImGuiWindowFlags_AlwaysUseWindowPadding);
    if(route.empty())ImGui::TextWrapped("%s",tr("Choose a station and click Add. Add departure first, then stops in travel order; the last point is the destination.","Выберите станцию и нажмите «Добавить». Сначала отправление, затем остановки по порядку; последняя точка станет конечной."));
    int remove=-1,move=-1,delta=0;
    for(std::size_t i=0;i<route.size();++i){
        std::string label=route[i];for(const auto& stop:state->stops)if(stop.id==route[i]){label=stop.name;break;}
        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginDisabled(i==0);if(ImGui::Button("^",{28*scale,26*scale})){move=static_cast<int>(i);delta=-1;}ImGui::EndDisabled();controls::Hint(tr("Move earlier","Переместить раньше"));ImGui::SameLine();
        ImGui::BeginDisabled(i+1==route.size());if(ImGui::Button("v",{28*scale,26*scale})){move=static_cast<int>(i);delta=1;}ImGui::EndDisabled();controls::Hint(tr("Move later","Переместить позже"));ImGui::SameLine();
        if(ImGui::Button(tr("Remove","Удалить"),{76*scale,26*scale}))remove=static_cast<int>(i);ImGui::SameLine();
        ImGui::AlignTextToFramePadding();ImGui::TextWrapped("%zu. %s%s",i+1,label.c_str(),i==0?tr(" - departure"," - отправление"):i+1==route.size()?tr(" - final stop"," - конечная"):"");ImGui::PopID();
    }
    if(remove>=0)route.erase(route.begin()+remove);
    else if(move>=0)std::swap(route[move],route[move+delta]);
    ImGui::EndChild();ImGui::PopStyleVar();
    const bool valid=ValidRoute(route);
    if(controls::PrimaryButton(tr("Create coach route","Создать автобусный рейс"),{270*scale,36*scale},valid)&&Queue({state->session,route}))overlay_ui::Request(10);
    if(!valid)controls::Hint(tr("Add at least two different stations: departure and destination.","Добавьте минимум две разные станции: отправление и назначение."));
    ImGui::SameLine();
    ImGui::BeginDisabled(route.empty());
    if(ImGui::Button(tr("Clear route","Очистить маршрут"),{220*scale,36*scale}))route.clear();
    ImGui::EndDisabled();
    if(!state->message.empty())ImGui::TextWrapped("%s",state->message.c_str());
}
}
