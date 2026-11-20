#pragma once
#include "custom_job_catalog.h"
#include <cmath>

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
// Same physical body/load checks as the validated native candidate routine.
// Random-market probability, skills and trailer country lists are not inputs.
inline bool CargoFitsTrailer(const Reader& read,std::uintptr_t cargo,std::uintptr_t trailer){
    if(!cargo || cargo>UINTPTR_MAX-0xb8 || !trailer || trailer>UINTPTR_MAX-0x50)return false;
    std::uintptr_t types=0;std::uint64_t count=0,body=0;
    if(!Read(read,cargo+0xa8,types)||!Read(read,cargo+0xb0,count)||!count||count>10000||
       !types||types>UINTPTR_MAX-count*8||!Read(read,trailer+0x48,body))return false;
    bool matches=false;
    for(std::uint64_t n=0;n<count;++n){std::uint64_t type=0;if(!Read(read,types+n*8,type))return false;if(type==body)matches=true;}
    if(!matches)return false;
    float volume=0,unit_volume=0,limit=0,chassis=0,body_mass=0,unit_mass=0;
    if(!Read(read,trailer+0x40,volume)||!Read(read,cargo+0x98,unit_volume)||!Read(read,trailer+0x30,limit)||
       !Read(read,trailer+0x38,chassis)||!Read(read,trailer+0x34,body_mass)||!Read(read,cargo+0x9c,unit_mass))return false;
    return std::isfinite(volume)&&std::isfinite(unit_volume)&&std::isfinite(limit)&&std::isfinite(chassis)&&
        std::isfinite(body_mass)&&std::isfinite(unit_mass)&&volume>=unit_volume&&chassis+body_mass+unit_mass<=limit;
}
}
