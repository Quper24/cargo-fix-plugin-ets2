#pragma once
#include "custom_job_catalog.h"
#include <array>
#include <cstddef>

namespace custom_jobs {
// city_data's country token is at 0x158 in the saved native schema. The
// adjacent cached pointer is used for labels only after its unit name matches.
// ATS uses the same country field for states and provinces.
inline std::uintptr_t CityRegion(const Reader& reader,std::uintptr_t pools,std::uintptr_t city,std::string& id){
    id.clear();
    std::uint64_t token=0;std::uintptr_t candidate=0;std::vector<std::string> parts;
    if(!LiveUnit(reader,city) || city>UINTPTR_MAX-0x168 || !Read(reader,city+0x158,token))return 0;
    id=DecodeToken(token);
    if(id.empty() || !Read(reader,city+0x160,candidate) || !UnitName(reader,pools,candidate,parts) ||
       parts.size()!=3 || parts[0]!="country" || parts[1]!="data" || parts[2]!=id)return 0;
    return candidate;
}
// The native string interface used by the game's fixed-size stack buffers.
// Its real vtable is resolved/validated from the game's localization function.
struct NativeNameBuffer {
    std::uintptr_t vtable = 0;
    char* data = nullptr;
    std::uint32_t length = 0, capacity = 2048;
    std::array<char, 2048> bytes{};
    explicit NativeNameBuffer(std::uintptr_t table) : vtable(table), data(bytes.data()) {}
    NativeNameBuffer(const NativeNameBuffer&) = delete;
    NativeNameBuffer& operator=(const NativeNameBuffer&) = delete;
};
static_assert(offsetof(NativeNameBuffer, bytes) == 0x18);
static_assert(sizeof(NativeNameBuffer) == 0x818);
inline std::string ReadNativeName(const Reader& reader, std::uintptr_t object, unsigned offset) {
    if (!object || object > UINTPTR_MAX - offset - 24) return {};
    std::uintptr_t data = 0;
    std::uint32_t length = 0, capacity = 0;
    if (!Read(reader, object + offset + 8, data) || !Read(reader, object + offset + 16, length) ||
        !Read(reader, object + offset + 20, capacity) || !length || length >= 2048 ||
        capacity <= length || !data || data > UINTPTR_MAX - length - 1) return {};
    std::string text(length + 1, '\0');
    if (!reader(data, text.data(), text.size()) || text[length] || std::find(text.begin(),text.end()-1,'\0') != text.end()-1) return {};
    text.resize(length); return text;
}
}
