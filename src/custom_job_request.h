#pragma once
#include "custom_job_catalog.h"
#include <optional>

namespace custom_jobs {
enum class Market { Freight, OwnedTrailer };
struct JobRequest {
    std::uint64_t session=0;
    Market market=Market::Freight;
    std::string origin, cargo, destination;
    bool allow_replace=false;
    bool delivery_time_x4=false;
};
enum class CreateStatus {
    Created, AlreadyPresent, Unsupported, NotReady, Cancelled, InvalidSelection,
    TrailerRequired, CargoUnavailable, DestinationUnavailable, NoSlot, ReplaceRequired,
    InvalidGameData, AllocationFailed, GenerationFailed, SchedulingFailed, RouteDataUnavailable
};
struct CreateResult {
    CreateStatus status=CreateStatus::Cancelled;
    Market market=Market::Freight;
    bool Success() const { return status==CreateStatus::Created || status==CreateStatus::AlreadyPresent; }
};
struct DirectRouteRow {
    std::uintptr_t destination=0;
    std::uint16_t distance=0,ferry=UINT16_MAX;
    std::uint32_t reserved=0;
};
static_assert(sizeof(DirectRouteRow)==16);
inline bool ReadDirectRoute(const Reader& read,std::uintptr_t company,std::uintptr_t destination,DirectRouteRow& result){
    result={};
    std::uintptr_t rows=0;std::uint64_t count=0;
    if(!company || company>UINTPTR_MAX-0x30 || !destination || !Read(read,company+0x20,rows)||
       !Read(read,company+0x28,count)||count>1000000||!rows||rows>UINTPTR_MAX-count*sizeof(DirectRouteRow))return false;
    for(std::uint64_t n=0;n<count;++n){
        DirectRouteRow row;
        if(!Read(read,rows+n*sizeof(row),row))return false;
        if(row.destination==destination){result=row;return true;}
    }
    return false;
}
inline bool ValidSelection(const Catalog& catalog,const JobRequest& request) {
    if(request.origin.empty() || request.origin==request.destination || request.origin.size()>512 || request.destination.size()>512 || request.cargo.size()>512 ||
       (request.market!=Market::Freight && request.market!=Market::OwnedTrailer))return false;
    const Company *from=nullptr,*to=nullptr;
    for(const auto& company:catalog.companies){if(company.id==request.origin)from=&company;if(company.id==request.destination)to=&company;}
    const bool known_cargo=request.market==Market::OwnedTrailer?
        std::any_of(catalog.cargos.begin(),catalog.cargos.end(),[&](const auto& cargo){return cargo.id==request.cargo;}):
        from && std::find(from->outgoing.begin(),from->outgoing.end(),request.cargo)!=from->outgoing.end();
    return from && to && known_cargo && (request.market==Market::OwnedTrailer || Accepts(*to,request.cargo));
}
class RequestQueue {
public:
    bool Push(const JobRequest& request) { if(pending)return false;pending=request;return true; }
    std::optional<JobRequest> Take() { auto result=std::move(pending);pending.reset();return result; }
    void Clear() {pending.reset();}
private:
    std::optional<JobRequest> pending;
};
struct OfferState {
    std::uintptr_t cargo=0;
    std::string destination;
    std::uint32_t expires=0;
};
struct SlotChoice { CreateStatus status=CreateStatus::NoSlot; std::size_t index=0; };
inline SlotChoice ChooseFreightSlot(const std::vector<OfferState>& offers,std::uintptr_t cargo,const std::string& destination,
                                   std::uint32_t now,bool replace) {
    std::optional<std::size_t> free,same,first;
    for(std::size_t n=0;n<offers.size();++n){
        const auto& offer=offers[n];
        if(offer.cargo==cargo && offer.destination==destination && offer.expires>now)return {CreateStatus::AlreadyPresent,n};
        if(!first)first=n;
        if(!offer.cargo || offer.expires<=now){if(!free)free=n;}
        else if(offer.cargo==cargo && !same)same=n;
    }
    // The ordinary native generator forbids another offer for the same cargo.
    if(same)return {replace?CreateStatus::Created:CreateStatus::ReplaceRequired,*same};
    if(free)return {CreateStatus::Created,*free};
    if(first)return {replace?CreateStatus::Created:CreateStatus::ReplaceRequired,*first};
    return {};
}
// Native temporary candidate vectors contain borrowed pointers. Filter without
// releasing objects or changing permanent company/cargo definitions.
inline std::uint64_t KeepCandidate(std::uintptr_t* data,std::uint64_t count,std::uintptr_t wanted) {
    for(std::uint64_t n=0;n<count;++n)if(data[n]==wanted){data[0]=wanted;return 1;}
    return 0;
}
inline bool KeepDestination(const Reader& read,std::uintptr_t* rows,std::uint64_t& count,
                            std::uint64_t cutoff,std::uintptr_t wanted) {
    for(std::uint64_t n=0;n<count;++n){
        std::uintptr_t company=0;
        if((cutoff==UINT64_MAX || n<cutoff) && Read(read,rows[n],company) && company==wanted){
            rows[0]=rows[n];count=1;return true;
        }
    }
    count=0;return false;
}
}
