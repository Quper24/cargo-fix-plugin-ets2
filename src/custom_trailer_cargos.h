#pragma once
#include "custom_job_catalog.h"

namespace custom_jobs {
enum class TrailerCargoStatus { Unsupported, NoTrailer, Ready, Failed };
struct TrailerCargos {
    TrailerCargoStatus status=TrailerCargoStatus::Unsupported;
    std::map<std::string,std::vector<std::string>> companies;
    bool Allows(const std::string& company,const std::string& cargo) const {
        const auto found=companies.find(company);
        return status==TrailerCargoStatus::Ready && found!=companies.end() &&
            std::find(found->second.begin(),found->second.end(),cargo)!=found->second.end();
    }
};
inline std::vector<std::string> AvailableCargos(const Company* company,bool owned,const TrailerCargos& filter){
    std::vector<std::string> result;
    if(company)for(const auto& cargo:company->outgoing)if(!owned || filter.Allows(company->id,cargo))result.push_back(cargo);
    return result;
}
}
