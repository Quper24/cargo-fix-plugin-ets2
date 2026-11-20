#include "custom_jobs.h"
#include "company_list.h"
#include <Windows.h>
#include "game_resolver.h"
#include "custom_job_names.h"
#include <cstring>
#include <mutex>

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
bool Memory(std::uintptr_t address, void* bytes, std::size_t length) {
    if (!ReadableMemory(address, length)) return false;
    std::memcpy(bytes, reinterpret_cast<const void*>(address), length); return true;
}
void Publish(std::shared_ptr<View> next) {
    std::lock_guard<std::mutex> lock(mutex); view = std::move(next);
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
    auto next = std::make_shared<View>(); next->session = ++session; next->supported = pools != 0;
    next->availability = availability;
    next->names_supported = name_code.localize != 0;
    Publish(std::move(next));
}
void Observe(std::uintptr_t economy, bool ready) {
    if (was_ready && (!ready || economy != last_economy)) Reset();
    was_ready = ready; last_economy = ready ? economy : 0;
}
void Scan(std::uintptr_t economy) {
    if (last_economy && last_economy != economy) Reset();
    last_economy = economy;
    auto next = std::make_shared<View>(); next->session = session; next->supported = pools != 0;
    next->availability = availability;
    next->names_supported = name_code.localize != 0;
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
    Publish(std::move(next));
}
std::shared_ptr<const View> Snapshot() { std::lock_guard<std::mutex> lock(mutex); return view; }
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
void Shutdown() { pools = module = 0; name_code = {}; log = nullptr; availability = Availability::UnsupportedCode; Reset(); }
#ifdef CARGO_CONTROL_CATALOG_PREVIEW
void PreviewCatalog(Catalog catalog) {
    auto next = std::make_shared<View>(); next->session = ++session;
    next->supported = next->scanned = true; next->catalog = std::move(catalog);
    next->availability = Availability::Ready;
    next->names_supported = true;
    Publish(std::move(next));
}
#endif
}
