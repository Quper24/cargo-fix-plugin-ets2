#include "road_trip.h"
#include "custom_jobs.h"
#include "custom_job_names.h"
#include "custom_job_request.h"
#include "owned_offers.h"
#include "overlay_ui.h"
#include "notifications.h"
#include "ui_controls.h"
#include "search_combo.h"
#include <Windows.h>
#include <imgui.h>
#include <mutex>
#include <map>
#include <set>
#include <optional>
#include <atomic>
#include <cstring>

namespace road_trip {
namespace {
using namespace custom_jobs;
std::mutex mutex;
std::shared_ptr<const View> view=std::make_shared<View>();
std::optional<Request> pending;
std::uintptr_t base=0,pools=0,last_economy=0,last_car=0;
GameProfile core{};CreationCode allocation{};RoadTripCode code{};CatalogNameCode names{};
Logger log=nullptr;std::atomic<bool> ats{false};bool supported=false;
std::uint64_t epoch=0;
struct NativePoint {Point text;std::uintptr_t item=0,company=0;};
struct NativeTask {Task text;std::uintptr_t object=0,cargo=0;};
std::map<std::string,NativePoint> points;
std::map<std::string,NativeTask> tasks;
struct Batch {bool active=false;std::uintptr_t economy=0,generator=0,car=0;std::uint64_t index=0,count=0,changed=0,kept=0;} batch;
bool Memory(std::uintptr_t address,void* out,std::size_t size){if(!ReadableMemory(address,size))return false;std::memcpy(out,reinterpret_cast<const void*>(address),size);return true;}
template<class T> T Get(std::uintptr_t at){T result{};Read(Memory,at,result);return result;}
template<class T> void Put(std::uintptr_t at,T value){std::memcpy(reinterpret_cast<void*>(at),&value,sizeof(value));}
template<class Fn> Fn Native(std::uint32_t rva){return reinterpret_cast<Fn>(base+rva);}
void Publish(std::shared_ptr<View> next){std::lock_guard<std::mutex> lock(mutex);view=std::move(next);}
void Message(const char* en,const char* ru){
    const auto* text=notifications::Russian()?ru:en;
    if(log)log(en);
    const int n=MultiByteToWideChar(CP_UTF8,0,text,-1,nullptr,0);std::wstring wide(n,L'\0');
    if(n){MultiByteToWideChar(CP_UTF8,0,text,-1,wide.data(),n);wide.resize(n-1);}notifications::Show(wide);
    auto next=std::make_shared<View>(*Snapshot());next->message=text;Publish(std::move(next));
}
std::uintptr_t Car(std::uintptr_t economy){
    auto player=Get<std::uintptr_t>(economy+0x18);
    // Native Road Trip preparation uses player.my_vehicles[car mode == 2].
    if(!LiveUnit(Memory,player)||Get<unsigned>(player+0x68)!=2)return 0;
    std::vector<std::uintptr_t> vehicles;
    if(!PointerList(Memory,player,0x78,16,vehicles)||vehicles.size()<3||!LiveUnit(Memory,vehicles[2]))return 0;
    return vehicles[2];
}
bool Tags(std::uintptr_t car,std::vector<std::uintptr_t>& tags){
    const auto data=Get<std::uintptr_t>(car+0x10),accessory=Get<std::uintptr_t>(data+0x38);
    std::vector<std::uintptr_t> accessories;
    if(!data||!PointerList(Memory,accessory,8,512,accessories)||accessories.empty())return false;
    for(auto item:accessories)if(!LiveUnit(Memory,item))return false;
    const auto model=Native<std::uintptr_t(__fastcall*)(std::uintptr_t,std::uintptr_t)>(code.lookup_car)(base+code.vehicle_registry,accessory);
    return LiveUnit(Memory,model)&&PointerList(Memory,model,0x390,128,tags)&&!tags.empty();
}
bool Fits(std::uintptr_t job,const std::vector<std::uintptr_t>& tags){
    std::vector<std::uintptr_t> accepted;
    if(!PointerList(Memory,job,0x68,128,accepted))return false;
    for(auto tag:tags)if(std::find(accepted.begin(),accepted.end(),tag)!=accepted.end())return true;
    return false;
}
std::string Label(std::uintptr_t unit,unsigned offset=0x10){
    auto raw=ReadNativeName(Memory,unit,offset);
    if(raw.find("@@")==std::string::npos)return raw;
    if(!names.localize)return {};
    NativeNameBuffer output(base+names.buffer_vtable);const char* input=raw.c_str();
    if(!Native<bool(__fastcall*)(NativeNameBuffer*,const char**)>(names.localize)(&output,&input)||output.data!=output.bytes.data()||output.length>=output.bytes.size()||output.bytes[output.length])return {};
    return std::string(output.bytes.data(),output.length);
}
std::string UnitId(std::uintptr_t unit){std::vector<std::string> parts;return UnitName(Memory,pools,unit,parts)?Join(parts):std::string{};}
std::uintptr_t Generator(std::uintptr_t economy){
    const auto value=Get<std::uintptr_t>(economy+0x1d48);
    return LiveUnit(Memory,value)&&ReadableMemory(value,0x60)?value:0;
}
bool OfferVector(std::uintptr_t generator,GameOfferVector*& out){
    out=reinterpret_cast<GameOfferVector*>(generator+0x10);
    return generator&&ReadableMemory(generator+0x10,sizeof(*out))&&ValidOfferVector(*out)&&out->count<=10000;
}
std::uintptr_t Allocate(){
    auto index=Get<unsigned>(base+allocation.allocator_index);
    auto pool=index<4?Get<std::uintptr_t>(base+allocation.allocator_array+index*8):0;
    if(!ReadableMemory(pool,0x18))return 0;
    auto offer=Native<std::uintptr_t(__fastcall*)(std::uintptr_t)>(code.factory)(pool);
    if(offer)InterlockedExchangeAdd(reinterpret_cast<volatile LONG*>(offer+8),0x20000001);
    return offer;
}
void Release(std::uintptr_t offer,std::uintptr_t table){
    if(!offer)return;GameOfferVector one{table,&offer,1,1};Native<void(__fastcall*)(GameOfferVector*)>(core.clear_vector)(&one);
}
bool Install(std::uintptr_t economy,std::uintptr_t generator,std::uint64_t slot,std::uintptr_t replacement){
    GameOfferVector* offers=nullptr;
    const auto scheduler=Get<std::uintptr_t>(economy+0x168);
    const auto now=Get<std::uint32_t>(economy+0x19c);
    if(!scheduler||now>UINT32_MAX-2880||!OfferVector(generator,offers)||slot>=offers->count)return false;
    const auto old=offers->data[slot];
    // Remove only this generator/slot event, then publish a completely built unit.
    Native<void(__fastcall*)(std::uintptr_t,std::uintptr_t,std::uint32_t)>(code.remove_event)(scheduler,generator,static_cast<unsigned>(slot));
    Put(replacement+0x80,now+2880);offers->data[slot]=replacement;
    Native<void(__fastcall*)(std::uintptr_t,std::uintptr_t,std::uint32_t,std::uintptr_t,std::uint32_t)>(code.schedule)(scheduler,economy,now+2880,generator,static_cast<unsigned>(slot));
    Release(old,offers->vtable);return true;
}
bool Contains(const std::vector<std::string>& values,const std::string& value){return std::find(values.begin(),values.end(),value)!=values.end();}
bool ContentLoaded(std::uintptr_t economy){
    if(!supported||!Generator(economy))return false;
    const auto registry=Get<std::uintptr_t>(base+code.registry);
    const auto storage=Get<std::uintptr_t>(registry+0x2ff8);
    const auto data=Get<std::uintptr_t>(storage+0x8d0);
    const auto count=Get<std::uint64_t>(storage+0x8d8);
    return storage&&count&&count<=10000&&data&&ReadableMemory(data,count*sizeof(std::uintptr_t));
}
}
#ifdef CARGO_CONTROL_CATALOG_PREVIEW
void Preview(View fixture){ats=true;fixture.available=true;fixture.scanned=true;fixture.own_car=true;fixture.status=SessionStatus::Ready;Publish(std::make_shared<View>(std::move(fixture)));}
#endif
std::shared_ptr<const View> Snapshot(){std::lock_guard<std::mutex> lock(mutex);return view;}
bool Visible(){return ats;}
void Initialize(std::uintptr_t module,std::size_t size,GameKind game,const GameProfile& profile,const CreationCode& alloc,Logger logger){
    base=module;core=profile;allocation=alloc;log=logger;ats=game==GameKind::ATS;
    std::string reason;std::uint32_t pool_rva=0;
    supported=ResolveRoadTripCode(reinterpret_cast<const unsigned char*>(base),size,game,code,reason)&&
        alloc.allocator_array&&ResolveCatalogPools(reinterpret_cast<const unsigned char*>(base),size,pool_rva,reason);
    pools=pool_rva?base+pool_rva:0;
    if(supported)ResolveCatalogNames(reinterpret_cast<const unsigned char*>(base),size,names,reason);
    if(log)log((supported?"Road Trip native code/flow checks passed.":("Road Trip disabled: "+reason)).c_str());
    Reset();
}
void Reset(){batch={};points.clear();tasks.clear();last_economy=last_car=0;++epoch;{std::lock_guard<std::mutex> lock(mutex);pending.reset();}
    auto next=std::make_shared<View>();next->session=epoch;next->available=supported;Publish(std::move(next));}
void Observe(std::uintptr_t economy,bool ready){
    if(!ats)return;
    const auto car=ready&&supported?Car(economy):0;
    if((last_economy&&(!ready||last_economy!=economy))||(last_car&&last_car!=car))Reset();
    last_economy=ready?economy:0;last_car=car;
    const auto status=!ready?SessionStatus::NotLoaded:ContentLoaded(economy)?SessionStatus::Ready:SessionStatus::ContentUnavailable;
    const auto current=Snapshot();
    if(current->status!=status){
        auto next=std::make_shared<View>(*current);next->status=status;Publish(std::move(next));
        if(log)log(status==SessionStatus::Ready?"Road Trip session content detected: native job definitions and generator are loaded.":
            status==SessionStatus::ContentUnavailable?"Road Trip session content is unavailable; load the DLC/content in a game profile.":"Road Trip is waiting for a loaded session.");
    }
}
void Scan(std::uintptr_t economy){
    if(!ContentLoaded(economy)){Message("Road Trip data is unavailable. Load a profile with Road Trip content enabled.","Данные Road Trip недоступны. Загрузите профиль с подключённым Road Trip.");return;}
    points.clear();tasks.clear();auto next=std::make_shared<View>();next->available=true;next->session=epoch;
    next->status=SessionStatus::Ready;
    const auto car=Car(economy);std::vector<std::uintptr_t> car_tags;
    next->own_car=car&&Tags(car,car_tags);
    const auto registry=Get<std::uintptr_t>(base+code.registry);
    const auto storage=Get<std::uintptr_t>(registry+0x2ff8);
    std::vector<std::uintptr_t> jobs,companies;
    if(!storage||!PointerList(Memory,storage,0x8d0,10000,jobs)||!PointerList(Memory,economy,0x28,1000000,companies)){
        Message("Cannot read Road Trip catalog.","Не удалось считать каталог Road Trip.");return;
    }
    std::map<std::uintptr_t,std::uintptr_t> by_item;
    std::map<std::uintptr_t,std::string> item_ids;
    for(auto company:companies){auto item=Get<std::uintptr_t>(company+0x10);if(item)by_item[item]=company;}
    auto collect=[&](std::uintptr_t job,unsigned offset,std::vector<std::string>& result){
        std::vector<std::uintptr_t> items;if(!PointerList(Memory,job,offset,1000000,items))return;
        for(auto item:items){
            const auto cached=item_ids.find(item);
            if(cached!=item_ids.end()){if(!cached->second.empty())result.push_back(cached->second);continue;}
            item_ids.emplace(item,"");
            auto city=DecodeToken(Get<std::uint64_t>(item+0x48)),depot=DecodeToken(Get<std::uint64_t>(item+0xb8));
            if(city.empty()||depot.empty()||!by_item.count(item))continue;
            const auto id=depot+"."+city;item_ids[item]=id;result.push_back(id);
            if(points.count(id))continue;
            auto company=by_item[item],context=Get<std::uintptr_t>(company+0x68);
            Point text{id,city,city,Label(context)};if(text.name.empty())text.name=depot;
            if(names.city_location){auto location=Native<std::uintptr_t(__fastcall*)(std::uintptr_t)>(names.city_location)(company);
                if(LiveUnit(Memory,location)){auto label=Label(location,Get<unsigned char>(base+names.city_localized_flag)?0x30:0x10);if(!label.empty())text.city_name=label;}}
            points.emplace(id,NativePoint{text,item,company});
        }
        std::sort(result.begin(),result.end());result.erase(std::unique(result.begin(),result.end()),result.end());
    };
    for(auto job:jobs){
        if(!LiveUnit(Memory,job)||!next->own_car||!Fits(job,car_tags))continue;
        std::vector<std::uintptr_t> cargos;if(!PointerList(Memory,job,0x40,10000,cargos))continue;
        Task task;task.id=UnitId(job);if(task.id.empty())continue;
        collect(job,0x100,task.sources);collect(job,0x128,task.targets);
        if(task.sources.empty()||task.targets.empty())continue;
        // Each selectable row retains the exact job/cargo pair, including mod definitions.
        if(cargos.empty())cargos.push_back(0);
        for(auto cargo:cargos){
            if(cargo&&!LiveUnit(Memory,cargo))continue;
            Task entry=task;entry.id+="|"+(cargo?UnitId(cargo):"vehicle");
            entry.name=cargo?Label(cargo):Label(job);
            if(entry.name.empty())entry.name=cargo?UnitId(cargo):"Перегон автомобиля";
            tasks.emplace(entry.id,NativeTask{entry,job,cargo});next->tasks.push_back(std::move(entry));
        }
    }
    for(const auto& item:points)next->points.push_back(item.second.text);
    std::sort(next->tasks.begin(),next->tasks.end(),[](const auto& a,const auto& b){return a.name<b.name;});
    next->scanned=true;Publish(next);
    if(log)log(("Road Trip catalog: points="+std::to_string(next->points.size())+", tasks="+std::to_string(next->tasks.size())+", own_car="+std::to_string(next->own_car)).c_str());
}
bool Queue(const Request& request){std::lock_guard<std::mutex> lock(mutex);if(pending)return false;pending=request;return true;}
void Create(std::uintptr_t economy){
    std::optional<Request> request;{std::lock_guard<std::mutex> lock(mutex);request=std::move(pending);pending.reset();}
    const auto car=Car(economy);std::vector<std::uintptr_t> tags;auto generator=Generator(economy);GameOfferVector* offers=nullptr;
    if(!request||request->session!=epoch||!supported||!car||!Tags(car,tags)||!OfferVector(generator,offers)||!tasks.count(request->task)||!points.count(request->source)||!points.count(request->target)||request->source==request->target){Message("Read the Road Trip session again.","Повторно считайте сессию Road Trip.");return;}
    const auto& task=tasks.at(request->task);const auto& from=points.at(request->source);const auto& to=points.at(request->target);
    if(!Fits(task.object,tags)||!Contains(task.text.sources,request->source)||!Contains(task.text.targets,request->target)){Message("This task is incompatible with the selected car or depot.","Задание несовместимо с выбранной машиной или базой.");return;}
    DirectRouteRow route;
    if(!ReadDirectRoute(Memory,from.company,to.company,route)){Message("Route data is unavailable for this pair of depots.","Для этой пары баз нет данных маршрута в загруженной сессии.");return;}
    std::uint64_t slot=offers->count;auto now=Get<std::uint32_t>(economy+0x19c);
    // Initial three slots are reserved by the native generator for local special tasks.
    for(std::uint64_t i=3;i<offers->count;++i)if(Get<std::uint32_t>(offers->data[i]+0x80)<=now){slot=i;break;}
    if(slot==offers->count&&offers->count>3)slot=3;
    if(slot==offers->count){Message("No ordinary Road Trip offer slot is available.","Нет доступного обычного слота предложения Road Trip.");return;}
    auto offer=Allocate();if(!offer){Message("Cannot allocate Road Trip offer.","Не удалось выделить предложение Road Trip.");return;}
    auto format=Native<void(__cdecl*)(std::uintptr_t,const char*,...)>(code.format_text);
    auto write_point=[&](unsigned offset,const Point& point){const auto dot=point.id.find('.');auto depot=point.id.substr(0,dot);format(offer+offset,reinterpret_cast<const char*>(base+code.point_format),depot.c_str(),point.city.c_str());};
    write_point(0x10,from.text);write_point(0x48,to.text);
    auto link=Native<void(__fastcall*)(std::uintptr_t,std::uintptr_t)>(code.assign_link);
    link(offer+0x98,task.object);if(task.cargo)link(offer+0xa0,task.cargo);
    Put<unsigned>(offer+0x8c,task.cargo?0:1);Put<unsigned>(offer+0xc8,task.cargo?1:0);
    Put<std::uint16_t>(offer+0x90,route.distance);
    Put<unsigned>(offer+0x84,Get<unsigned char>(task.object+0xd8)?1:0);
    for(unsigned i=0;i<3;++i)Put<unsigned char>(offer+0x88+i,0);
    unsigned best=0;for(unsigned i=1;i<3;++i)if(Get<unsigned>(task.object+0xdc+i*4)>Get<unsigned>(task.object+0xdc+best*4))best=i;
    Put<unsigned char>(offer+0x88+best,1);
    const bool selected=Native<bool(__fastcall*)(std::uintptr_t,std::uintptr_t,std::uintptr_t)>(code.select_car)(generator,offer+0xa8,task.object+0x60);
    if(!selected||ReadNativeName(Memory,offer,0x10)!=from.text.id||ReadNativeName(Memory,offer,0x48)!=to.text.id||!Install(economy,generator,slot,offer)){
        Release(offer,offers->vtable);Message("Road Trip offer was not created. Existing offers were preserved.","Заказ Road Trip не создан. Прежние предложения сохранены.");return;
    }
    if(log)log(("Road Trip offer created: slot="+std::to_string(slot)+", source="+from.text.id+", target="+to.text.id+", task="+request->task).c_str());
    Message("Road Trip offer created. Reopen Dispatch Market and select the departure city.","Заказ Road Trip создан. Заново откройте рынок доставки и выберите город отправления.");
}
void Refresh(std::uintptr_t economy){
    const auto generator=Generator(economy),car=Car(economy);GameOfferVector* offers=nullptr;
    if(!supported||!car||!OfferVector(generator,offers)||!offers->count){Message("Load Road Trip and select your car first.","Сначала загрузите Road Trip и выберите свою машину.");return;}
    batch={true,economy,generator,car,0,offers->count,0,0};overlay_ui::SetProgress(0,batch.count);
}
bool Busy(){return batch.active;}
void Tick(std::uintptr_t economy){
    if(!batch.active)return;
    GameOfferVector* offers=nullptr;
    if(economy!=batch.economy||Generator(economy)!=batch.generator||Car(economy)!=batch.car||!OfferVector(batch.generator,offers)||offers->count!=batch.count){batch={};Message("Road Trip refresh cancelled: session or vehicle changed.","Обновление Road Trip отменено: сессия или машина изменилась.");return;}
    const auto slot=batch.index;auto offer=Allocate();bool success=false;
    if(offer){
        const unsigned local=slot<3?2:slot<100?1:0,tier=slot>=900?2:slot>=800?1:0;
        success=Native<bool(__fastcall*)(std::uintptr_t,std::uintptr_t,std::uintptr_t,unsigned,unsigned)>(code.generate)(batch.generator,offer,economy,local,tier);
        success=success&&!ReadNativeName(Memory,offer,0x10).empty()&&!ReadNativeName(Memory,offer,0x48).empty()&&Get<std::uintptr_t>(offer+0x98);
        if(success)success=Install(economy,batch.generator,slot,offer);
        if(!success)Release(offer,offers->vtable);
    }
    if(success)++batch.changed;else ++batch.kept;
    ++batch.index;overlay_ui::SetProgress(batch.index,batch.count);
    if(batch.index==batch.count){
        if(log)log(("Road Trip refresh finished: replaced="+std::to_string(batch.changed)+", preserved="+std::to_string(batch.kept)).c_str());
        const auto en="Road Trip: refreshed "+std::to_string(batch.changed)+", preserved "+std::to_string(batch.kept)+". Reopen the Road Trip market.";
        const auto ru="Road Trip: обновлено "+std::to_string(batch.changed)+", сохранено прежних "+std::to_string(batch.kept)+". Заново откройте рынок Road Trip.";
        batch.active=false;Message(en.c_str(),ru.c_str());
    }
}
void Draw(float scale,bool russian){
    auto tr=[&](const char* en,const char* ru){return russian?ru:en;};const auto state=Snapshot();
    ImGui::SetWindowFontScale(.5f*scale);
    if(!state->available){ImGui::TextWrapped("%s",tr("Could not initialize the Road Trip module. See game.log.txt.","Не удалось подключить модуль Road Trip. Подробности в game.log.txt."));return;}
    if(state->status==SessionStatus::NotLoaded){ImGui::TextWrapped("%s",tr("Load your game profile to detect Road Trip content.","Загрузите игровой профиль, чтобы проверить доступность Road Trip."));return;}
    if(state->status==SessionStatus::ContentUnavailable){ImGui::TextWrapped("%s",tr("Road Trip content is not loaded. Check that the DLC is available and enabled, then load a Road Trip profile.","Данные Road Trip не загружены. Проверьте, что DLC доступен и подключён, затем загрузите профиль Road Trip."));return;}
    if(controls::PrimaryButton(tr("Read Road Trip session","Считать сессию Road Trip"),{286*scale,40*scale}))overlay_ui::Request(5);
    controls::Hint(tr("The window closes while reading and reopens automatically.","На время чтения окно закроется и откроется автоматически."));
    ImGui::SameLine(0,14*scale);
    if(controls::PrimaryButton(tr("Refresh Road Trip offers","Обновить заказы Road Trip"),{286*scale,40*scale}))overlay_ui::Request(6);
    controls::Hint(tr("Replace Quick Jobs and Dispatch offers using the game generator.","Заменить предложения быстрых заказов и доставки штатным генератором игры."));
    ImGui::TextWrapped("%s",tr("Quick Jobs and Dispatch offers are refreshed together. Existing offers survive a failed generation.","Обновляются предложения быстрых заказов и доставки. Если подбор не удался, старое предложение сохраняется."));
    if(!state->scanned){ImGui::TextWrapped("%s",tr("Select your car and read the loaded session. The window will reopen automatically.","Выберите свою машину и считайте загруженную сессию. Окно откроется автоматически."));return;}
    if(!state->own_car){ImGui::TextWrapped("%s",tr("Select your own car in Road Trip, then read the session again.","Выберите собственную машину в Road Trip и считайте сессию повторно."));return;}
    static std::uint64_t seen=UINT64_MAX;static std::string task,from,to,from_city,to_city;
    if(seen!=state->session){seen=state->session;task.clear();from.clear();to.clear();from_city.clear();to_city.clear();}
    ImGui::Text("%s: %zu   %s: %zu",tr("Depots","Базы"),state->points.size(),tr("Tasks for your car","Задания для вашей машины"),state->tasks.size());
    const Task* selected=nullptr;for(const auto& item:state->tasks)if(item.id==task)selected=&item;
    ImGui::TextUnformatted(tr("Delivery / vehicle transfer","Отправление / перегон автомобиля"));ImGui::SetNextItemWidth(-1);
    std::vector<controls::Choice> task_choices;
    for(const auto& item:state->tasks)task_choices.push_back({item.id,item.name});
    if(controls::SearchCombo("rt-task",task,task_choices,russian,ContainsName)){from.clear();to.clear();}
    selected=nullptr;for(const auto& item:state->tasks)if(item.id==task)selected=&item;
    auto point_fields=[&](bool source){
        auto& city=source?from_city:to_city;auto& id=source?from:to;
        const auto* allowed=selected?(source?&selected->sources:&selected->targets):nullptr;
        const auto width=(ImGui::GetContentRegionAvail().x-16*scale)/2;
        std::map<std::string,std::string> cities;
        for(const auto& p:state->points)if(allowed&&Contains(*allowed,p.id))cities[p.city]=p.city_name;
        ImGui::PushID(source?"rt-source":"rt-target");
        const auto row_y=ImGui::GetCursorPosY();
        ImGui::BeginGroup();ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(source?tr("Departure city","Город отправления"):tr("Destination city","Город назначения"));ImGui::SetNextItemWidth(width);
        std::vector<controls::Choice> city_choices;
        for(const auto& item:cities)city_choices.push_back({item.first,item.second});
        std::sort(city_choices.begin(),city_choices.end(),[](const auto& a,const auto& b){return a.name<b.name;});
        if(controls::SearchCombo("rt-city",city,city_choices,russian,ContainsName))id.clear();
        ImGui::EndGroup();ImGui::SameLine(0,16*scale);ImGui::SetCursorPosY(row_y);ImGui::BeginGroup();
        ImGui::AlignTextToFramePadding();ImGui::TextUnformatted(source?tr("Departure depot","База отправления"):tr("Destination depot","База назначения"));ImGui::SetNextItemWidth(width);
        std::vector<controls::Choice> depot_choices;
        for(const auto& p:state->points)if(p.city==city&&allowed&&Contains(*allowed,p.id))depot_choices.push_back({p.id,p.name});
        std::sort(depot_choices.begin(),depot_choices.end(),[](const auto& a,const auto& b){return a.name<b.name;});
        controls::SearchCombo("rt-depot",id,depot_choices,russian,ContainsName);
        ImGui::EndGroup();ImGui::PopID();
    };
    point_fields(true);point_fields(false);
    ImGui::TextWrapped("%s",tr("One existing offer may be replaced automatically.","Одно существующее предложение может быть заменено автоматически."));
    const bool valid=selected&&!from.empty()&&!to.empty()&&from!=to;
    if(controls::PrimaryButton(tr("Create Road Trip job","Создать заказ Road Trip"),{260*scale,36*scale},valid)&&Queue({state->session,task,from,to}))overlay_ui::Request(7);
    if(!valid)controls::Hint(tr("Select a task, departure and a different destination.","Выберите задание, базу отправления и другую базу назначения."));
    if(!state->message.empty())ImGui::TextWrapped("%s",state->message.c_str());
}
}
