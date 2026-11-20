#pragma once
#include "custom_job_catalog.h"
#include <array>
#include <cstddef>

namespace custom_jobs {
enum class TrailerCargoStatus { Unsupported, NoTrailer, Ready, Failed };
struct TrailerCargos {
    TrailerCargoStatus status=TrailerCargoStatus::Unsupported;
    std::vector<std::string> cargos;
    bool Allows(const std::string& cargo) const {
        return status==TrailerCargoStatus::Ready && std::find(cargos.begin(),cargos.end(),cargo)!=cargos.end();
    }
};
inline std::vector<std::string> AvailableCargos(const Catalog& catalog,const Company* company,bool owned,const TrailerCargos& filter){
    std::vector<std::string> result;
    if(owned){for(const auto& cargo:catalog.cargos)if(filter.Allows(cargo.id))result.push_back(cargo.id);}
    else if(company)result=company->outgoing;
    return result;
}
// Read-only inputs for the fully validated native owned-candidate routine.
// That routine reads only company+68, definition+8 and definition+A0/A8.
// No live company definition or cargo relationship is changed.
struct OwnedCargoProbe {
    struct Definition {
        std::array<unsigned char,8> reserved{};
        std::uint32_t flags=0x80000000u;
        std::array<unsigned char,0xa0-12> padding{};
        const std::uintptr_t* data=nullptr;
        std::uint64_t count=0;
    } definition;
    struct CompanyView {
        std::array<unsigned char,0x68> reserved{};
        const Definition* definition=nullptr;
    } company;
    OwnedCargoProbe(const std::uintptr_t* data,std::uint64_t count){definition.data=data;definition.count=count;company.definition=&definition;}
    OwnedCargoProbe(const OwnedCargoProbe&)=delete;
    OwnedCargoProbe& operator=(const OwnedCargoProbe&)=delete;
    std::uintptr_t Address() const{return reinterpret_cast<std::uintptr_t>(&company);}
};
static_assert(offsetof(OwnedCargoProbe::Definition,flags)==8);
static_assert(offsetof(OwnedCargoProbe::Definition,data)==0xa0);
static_assert(offsetof(OwnedCargoProbe::Definition,count)==0xa8);
static_assert(offsetof(OwnedCargoProbe::CompanyView,definition)==0x68);
}
