#include "custom_jobs.h"
#include "company_list.h"
#include <Windows.h>
#include "game_resolver.h"
#include "custom_job_names.h"
#include <cstring>
#include <mutex>
#include <atomic>

namespace custom_jobs {
namespace {
std::mutex mutex;
std::shared_ptr<const View> view = std::make_shared<View>();
std::uintptr_t module = 0, pools = 0, last_economy = 0;
std::uint64_t session = 0;
bool was_ready = false;
Logger log = nullptr;
Availability availability = Availability::UnsupportedCode;
CatalogNameCode name_code{};
bool creation_supported=false;
RequestQueue creation_queue;
std::uintptr_t last_trailer=UINTPTR_MAX;
std::atomic<bool> refresh_trailer{false};
bool Memory(std::uintptr_t address, void* bytes, std::size_t length) {
    if (!ReadableMemory(address, length)) return false;
    std::memcpy(bytes, reinterpret_cast<const void*>(address), length); return true;
}
void Publish(std::shared_ptr<View> next) {
    std::lock_guard<std::mutex> lock(mutex); view = std::move(next);
}
void UpdateTrailerCargos(View& next,std::uintptr_t economy){
#ifdef CARGO_CONTROL_EXPERIMENTAL_JOBS
    const auto started=GetTickCount64();
    next.trailer_cargos=creation::ReadTrailerCargos(economy,pools,next.catalog);
    last_trailer=creation::TrailerIdentity(economy);
    if(log)log(("Owned-trailer cargo filter: status="+std::to_string(static_cast<int>(next.trailer_cargos.status))+
        ", companies="+std::to_string(next.trailer_cargos.companies.size())+", elapsed_ms="+std::to_string(GetTickCount64()-started)+".").c_str());
#else
    (void)next;(void)economy;
#endif
}
}
void Initialize(std::uintptr_t base, std::size_t image_size, Logger logger) {
    module = base; pools = 0; last_economy = 0; log = logger;
    availability = Availability::UnsupportedCode;
    std::uint32_t rva = 0;
    std::string reason;
    if (ResolveCatalogPools(reinterpret_cast<const unsigned char*>(module), image_size, rva, reason)) {
        pools = module + rva; availability = Availability::Ready;
        if (log) log("Session catalog code/flow checks passed; executable hash and patch/build version do not gate compatibility.");
    }
    if (!pools && log) {
        const auto message = "Experimental catalog unavailable: " + reason;
        log(message.c_str());
    }
    name_code = {};
    if (pools && !ResolveCatalogNames(reinterpret_cast<const unsigned char*>(module),image_size,name_code,reason) && log)
        log(("Display names unavailable; identifiers retained: " + reason).c_str());
    else if (name_code.localize && log) log("Live definition names and native localization are enabled; mod names are not mapped from a built-in dictionary.");
    Reset();
}
void Reset() {
    last_economy = 0; was_ready = false;
    last_trailer=UINTPTR_MAX;refresh_trailer=false;
    auto next = std::make_shared<View>(); next->session = ++session; next->supported = pools != 0;
    next->availability = availability;
    next->names_supported = name_code.localize != 0;
    next->creation_supported = creation_supported;
    std::lock_guard<std::mutex> lock(mutex);creation_queue.Clear();view=std::move(next);
}
void Observe(std::uintptr_t economy, bool ready) {
    if (was_ready && (!ready || economy != last_economy)) Reset();
    was_ready = ready; last_economy = ready ? economy : 0;
#ifdef CARGO_CONTROL_EXPERIMENTAL_JOBS
    if(ready && creation_supported){
        const auto current=Snapshot();
        if(current->scanned && (refresh_trailer.exchange(false) || creation::TrailerIdentity(economy)!=last_trailer)){
            auto next=std::make_shared<View>(*current);UpdateTrailerCargos(*next,economy);Publish(std::move(next));
        }
    }
#endif
}
void RefreshTrailerCargos(){refresh_trailer=true;}
void Scan(std::uintptr_t economy) {
    if (last_economy && last_economy != economy) Reset();
    last_economy = economy;
    auto next = std::make_shared<View>(); next->session = session; next->supported = pools != 0;
    next->availability = availability;
    next->names_supported = name_code.localize != 0;
    next->creation_supported = creation_supported;
    std::map<std::pair<LabelKind,std::uintptr_t>,std::string> cache;
    LabelReader labels;
    if (name_code.localize) labels = [&](LabelKind kind,std::uintptr_t object) -> std::string {
        if (kind == LabelKind::City) {
            if (!object || object>UINTPTR_MAX-0x118) return {};
            std::uintptr_t city = 0;
            if (!Read(Memory,object+0x100,city)) return {};
            if (!LiveUnit(Memory,city)) {
                if (!ReadableMemory(object,0x118)) return {};
                using GetCity = std::uintptr_t(__fastcall*)(std::uintptr_t);
                city = reinterpret_cast<GetCity>(module+name_code.city_location)(object);
            }
            object = city;
        }
        if (!LiveUnit(Memory,object)) return {};
        const auto key = std::make_pair(kind,object);
        const auto known = cache.find(key);
        if (known != cache.end()) return known->second;
        unsigned offset = 0x10;
        if (kind == LabelKind::City) {
            unsigned char localized = 0;
            if (!Read(Memory,module+name_code.city_localized_flag,localized)) return {};
            if (localized) offset = 0x30;
        }
        auto raw = ReadNativeName(Memory,object,offset);
        if (raw.empty() && offset != 0x10) raw = ReadNativeName(Memory,object,0x10);
        auto text = raw;
        if (raw.find("@@") != std::string::npos) {
            NativeNameBuffer output(module+name_code.buffer_vtable);
            const char* input = raw.c_str();
            using Localize = bool(__fastcall*)(NativeNameBuffer*,const char**);
            const bool ok = reinterpret_cast<Localize>(module+name_code.localize)(&output,&input);
            if (!ok || output.data != output.bytes.data() || output.length >= output.bytes.size() || output.bytes[output.length]) text.clear();
            else text.assign(output.bytes.data(),output.length);
        }
        if (text.find("@@") != std::string::npos || (!text.empty() && !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0))) text.clear();
        cache.emplace(key,text); return text;
    };
    if (pools) next->scanned = BuildCatalog(Memory, economy, pools, next->catalog, next->error, labels);
    else next->error = "The session catalog code is incompatible or ambiguous.";
    if (log) {
        const auto message = "Session catalog scan: companies=" + std::to_string(next->catalog.companies.size()) +
            ", cargos=" + std::to_string(next->catalog.cargos.size()) +
            ", skipped=" + std::to_string(next->catalog.skipped) + ". " + next->error;
        log(message.c_str());
        if (next->names_supported) log(("Session display names: unresolved occurrences="+std::to_string(next->catalog.label_fallbacks)+".").c_str());
    }
    if(next->scanned){refresh_trailer=false;UpdateTrailerCargos(*next,economy);}
    Publish(std::move(next));
}
std::shared_ptr<const View> Snapshot() { std::lock_guard<std::mutex> lock(mutex); return view; }
void EnableCreation(const GameProfile& core,const CreationCode& code,creation::Backend backend){
#ifdef CARGO_CONTROL_EXPERIMENTAL_JOBS
    creation_supported=creation::Initialize(module,core,code,backend,log);
#else
    (void)core;(void)code;(void)backend;creation_supported=false;
#endif
    auto next=std::make_shared<View>(*Snapshot());next->creation_supported=creation_supported;Publish(std::move(next));
}
bool QueueCreation(const JobRequest& request){
    std::lock_guard<std::mutex> lock(mutex);
    return view->creation_supported && view->scanned && request.session==view->session && ValidSelection(view->catalog,request) &&
        (request.market!=Market::OwnedTrailer || view->trailer_cargos.Allows(request.origin,request.cargo)) && creation_queue.Push(request);
}
void CancelCreation(){std::lock_guard<std::mutex> lock(mutex);creation_queue.Clear();}
CreateResult ExecuteCreation(std::uintptr_t economy){
    std::optional<JobRequest> request;
    {std::lock_guard<std::mutex> lock(mutex);request=creation_queue.Take();}
    if(!request)return {};
    CreateResult result{CreateStatus::Cancelled,request->market};
    if(!was_ready || economy!=last_economy)result.status=CreateStatus::NotReady;
    else if(request->session!=session)result.status=CreateStatus::Cancelled;
    else if(!creation_supported)result.status=CreateStatus::Unsupported;
#ifdef CARGO_CONTROL_EXPERIMENTAL_JOBS
    else result=creation::Execute(economy,pools,*request);
#endif
    const auto current=Snapshot();
    if(current->session==request->session){auto next=std::make_shared<View>(*current);next->has_creation_result=true;next->creation_result=result;Publish(std::move(next));}
    if(log){const auto message="Custom job request completed: status="+std::to_string(static_cast<int>(result.status))+", source="+request->origin+", cargo="+request->cargo+", destination="+request->destination+".";log(message.c_str());}
    return result;
}
std::string CreationMessage(CreateResult result,bool ru){
    auto tr=[&](const char* en,const char* russian){return std::string(ru?russian:en);};
    switch(result.status){
    case CreateStatus::Created:return result.market==Market::OwnedTrailer?
        tr("Job created. Reopen the owned-trailer cargo market and find the offer at the selected source company.","Заказ создан. Заново откройте биржу грузов для своего прицепа и найдите предложение на выбранной базе отправления."):
        tr("Job created. Reopen the Freight market and find the offer at the selected source company.","Заказ создан. Заново откройте рынок грузоперевозок и найдите предложение на выбранной базе отправления.");
    case CreateStatus::AlreadyPresent:return tr("This job is already available at the selected company.","Такой заказ уже есть на выбранной базе.");
    case CreateStatus::Unsupported:return tr("Job creation is unavailable for this game code. See game.log.txt.","Создание заказа недоступно для этого кода игры. Подробности в game.log.txt.");
    case CreateStatus::NotReady:return tr("The profile is not ready. Load the session and try again.","Профиль ещё не готов. Загрузите сессию и повторите действие.");
    case CreateStatus::Cancelled:return tr("The request was cancelled. Read the loaded session again.","Запрос отменён. Заново считайте загруженную сессию.");
    case CreateStatus::InvalidSelection:return tr("The selected source, cargo or destination is no longer available. Read the session again.","Выбранная база, груз или получатель больше недоступны. Заново считайте сессию.");
    case CreateStatus::TrailerRequired:return tr("Select an owned trailer before creating its cargo offer.","Перед созданием груза выберите свой прицеп.");
    case CreateStatus::CargoUnavailable:return tr("This cargo is not compatible with the selected owned trailer or its limits.","Этот груз не подходит выбранному собственному прицепу или его ограничениям.");
    case CreateStatus::DestinationUnavailable:return tr("The game rejected this destination for the cargo, route or trailer.","Игра не допускает этого получателя для выбранного груза, маршрута или прицепа.");
    case CreateStatus::NoSlot:return tr("This company has no available native job slots. Open its market or choose another company.","У этой базы нет доступных штатных слотов заказов. Откройте её рынок или выберите другую базу.");
    case CreateStatus::ReplaceRequired:return tr("Enable replacement of one offer at the source company, then try again.","Разрешите замену одного предложения на базе отправления и повторите действие.");
    case CreateStatus::AllocationFailed:return tr("The game could not allocate the new offer.","Игра не смогла выделить память для нового предложения.");
    case CreateStatus::SchedulingFailed:return tr("The game could not register the offer expiry. The request was not committed.","Не удалось зарегистрировать срок предложения. Запрос не был применён.");
    case CreateStatus::GenerationFailed:return tr("The native generator did not create the exact requested job. Try another cargo or destination.","Штатный генератор не создал точно выбранный заказ. Попробуйте другой груз или получателя.");
    default:return tr("Game data failed validation. No custom offer was committed. See game.log.txt.","Данные игры не прошли проверку. Новый заказ не применён. Подробности в game.log.txt.");
    }
}
bool ContainsName(std::string_view text, std::string_view query) {
    if (query.empty()) return true;
    auto wide=[](std::string_view value) {
        const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
        std::wstring result(length,L'\0');
        if(length)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),length);
        return result;
    };
    const auto value=wide(text), needle=wide(query);
    if (needle.empty() || value.size()<needle.size()) return false;
    for(std::size_t n=0;n+needle.size()<=value.size();++n)
        if(CompareStringOrdinal(value.data()+n,static_cast<int>(needle.size()),needle.data(),static_cast<int>(needle.size()),TRUE)==CSTR_EQUAL)return true;
    return false;
}
void Shutdown() {
#ifdef CARGO_CONTROL_EXPERIMENTAL_JOBS
    creation::Shutdown();
#endif
    creation_supported=false;pools=module=0;name_code={};log=nullptr;availability=Availability::UnsupportedCode;Reset();
}
#ifdef CARGO_CONTROL_CATALOG_PREVIEW
void PreviewCatalog(Catalog catalog) {
    auto next = std::make_shared<View>(); next->session = ++session;
    next->supported = next->scanned = true; next->catalog = std::move(catalog);
    next->availability = Availability::Ready;
    next->names_supported = true;
    next->creation_supported = true;
    Publish(std::move(next));
}
#endif
}
