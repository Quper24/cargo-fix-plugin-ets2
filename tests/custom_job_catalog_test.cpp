#include "custom_job_catalog.h"
#include "custom_job_names.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#define Check(value) do { if (!(value)) { std::fprintf(stderr,"Catalog check failed: %d\n",__LINE__); std::exit(1); } } while(0)
using namespace custom_jobs;
struct World {
    std::vector<unsigned char> bytes = std::vector<unsigned char>(1024 * 1024);
    std::uintptr_t cursor = 0x1000, pools, economy;
    std::uintptr_t token_tables[3]{};
    unsigned next_row[3]{1,1,1};
    Reader reader = [this](std::uintptr_t at, void* output, std::size_t length) {
        if (at < 0x1000 || at > bytes.size() || length > bytes.size() - at) return false;
        std::memcpy(output, bytes.data() + at, length); return true;
    };
    std::uintptr_t Alloc(std::size_t size) { auto at = cursor; cursor += (size + 15) & ~std::size_t(15); Check(cursor < bytes.size()); return at; }
    template<class T> void Put(std::uintptr_t at, T value) { Check(at + sizeof(T) <= bytes.size()); std::memcpy(bytes.data()+at,&value,sizeof(value)); }
    static std::uint64_t Encode(const std::string& text) {
        constexpr char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz_";
        std::uint64_t token = 0, power = 1;
        for (const char c : text) {
            const char* p = std::strchr(alphabet,c); Check(p);
            token += (p-alphabet+1)*power; power *= 38;
        }
        return token;
    }
    World() {
        pools = Alloc(2048 * 8); economy = Alloc(0x200);
        for(unsigned index=0;index<3;++index) {
            const unsigned width=index==2?3:index?4:2;
            const auto pool = Alloc(0x90); token_tables[index] = Alloc(64 * width * 8);
            Put(pools+index*8,pool); Put(pool,token_tables[index]); Put<std::uint8_t>(pool+0x88,width);
        }
    }
    std::uintptr_t Unit(std::initializer_list<const char*> parts) {
        const unsigned index = parts.size()==3?2:parts.size()==4 ? 1 : 0;
        const auto row = next_row[index]++; Check(row < 64);
        const auto unit = Alloc(0x80); Put<std::uint32_t>(unit+8,0x80000001u);
        Put<std::uint32_t>(unit+0xc,(index<<20)|row);
        unsigned n=0; for(const auto part:parts) Put(token_tables[index]+(row*parts.size()+n++)*8,Encode(part));
        return unit;
    }
    void List(std::uintptr_t object, unsigned offset, std::initializer_list<std::uintptr_t> values) {
        const auto at = values.size() ? Alloc(values.size()*8) : 0;
        unsigned i=0; for(const auto value:values) Put(at+i++*8,value);
        Put(object+offset,at); Put<std::uint64_t>(object+offset+8,values.size());
    }
    std::uintptr_t CompanyUnit(const char* name,const char* city,
                               std::initializer_list<std::uintptr_t> outgoing,
                               std::initializer_list<std::uintptr_t> incoming) {
        const auto unit = Unit({"company","volatile",name,city});
        const auto context = Alloc(0x150), definition = Alloc(0x100);
        Put<std::uint32_t>(context+8,0x80000001u);
        Put(unit+0x10,definition); Put(unit+0x68,context);
        List(context,0xa0,outgoing); List(context,0xc8,incoming); return unit;
    }
};
int main() {
    Check(DecodeToken(World::Encode("mod_cargo9"))=="mod_cargo9");
    Check(DecodeToken(0).empty()); Check(DecodeToken(38).empty());
    World first;
    const auto rt=first.Alloc(0x120),rt_def=first.Alloc(0x180),rt_context=first.Alloc(0x180);
    first.Put(rt+0x10,rt_def);first.Put(rt+0x68,rt_context);
    Check(RoadTripOnlyDepot(first.reader,rt,"pmrt1"));
    Check(!RoadTripOnlyDepot(first.reader,rt,"nls_rd_wrk"));
    Check(!RoadTripOnlyDepot(first.reader,rt,"my_pmrt1"));
    first.Put<std::uint64_t>(rt_context+0xa8,1);Check(!RoadTripOnlyDepot(first.reader,rt,"pmrt1"));
    first.Put<std::uint64_t>(rt_context+0xa8,0);first.Put<std::uint32_t>(rt_def+0xc0,1);
    Check(!RoadTripOnlyDepot(first.reader,rt,"pmrt1"));
    const auto custom = first.Unit({"cargo","mod_cargo9"});
    const auto ordinary = first.Unit({"cargo","apples"});
    const auto source = first.CompanyUnit("mod_depot","mod_city",{custom,ordinary,custom},{ordinary});
    const auto target = first.CompanyUnit("mod_depot","other_city",{ordinary},{custom});
    const auto road_trip=first.CompanyUnit("pmrt1","road_city",{},{});
    first.List(first.economy,0x28,{source,target,source,road_trip});
    Catalog catalog; std::string error;
    Check(BuildCatalog(first.reader,first.economy,first.pools,catalog,error));
    Check(error.empty() && catalog.companies.size()==2 && catalog.cargos.size()==2);
    Check(catalog.excluded_road_trip==1);
    const auto& from = catalog.companies[0]; const auto& to = catalog.companies[1];
    Check(from.id=="company.volatile.mod_depot.mod_city" && from.outgoing.size()==2);
    Check(to.name==from.name && to.city!=from.city); // Distinct bases with the same brand.
    Check(Accepts(to,"cargo.mod_cargo9") && !Accepts(to,"cargo.apples"));
    const auto source_id=from.id;
    std::string mod_brand="FedEx";
    LabelReader labels=[&](LabelKind kind,std::uintptr_t object) -> std::string {
        if(kind==LabelKind::Company)return mod_brand;
        if(kind==LabelKind::City)return object==source?"Анн-Арбор":"Кенора";
        if(kind==LabelKind::RegionId)return object==source?"michigan":"ontario";
        if(kind==LabelKind::Region)return object==source?"Мичиган":"Онтарио";
        return object==custom?"Соль":std::string{};
    };
    Check(BuildCatalog(first.reader,first.economy,first.pools,catalog,error,labels));
    Check(catalog.companies[0].id==source_id && catalog.companies[0].name=="FedEx" && catalog.companies[0].city_name=="Анн-Арбор");
    Check(catalog.companies[0].region=="michigan" && catalog.companies[0].region_name=="Мичиган");
    Check(InRegion(catalog.companies[0],"michigan") && !InRegion(catalog.companies[0],"ontario"));
    Check(InRegion(catalog.companies[0],""));
    Company unknown;Check(InRegion(unknown,"#unknown") && !InRegion(unknown,"michigan"));
    Check(catalog.label_fallbacks>0);
    bool translated=false;
    for(const auto& cargo:catalog.cargos)if(cargo.id=="cargo.mod_cargo9")translated=cargo.name=="Соль";
    Check(translated && Accepts(catalog.companies[1],"cargo.mod_cargo9"));
    mod_brand="Pepsi";
    Check(BuildCatalog(first.reader,first.economy,first.pools,catalog,error,labels));
    Check(catalog.companies[0].id==source_id && catalog.companies[0].name=="Pepsi"); // A renamed mod never changes object identity.
    const auto native_text=first.Alloc(16);
    const std::string salt="Соль";
    std::memcpy(first.bytes.data()+native_text,salt.c_str(),salt.size()+1);
    first.Put(custom+0x18,native_text);first.Put<std::uint32_t>(custom+0x20,static_cast<std::uint32_t>(salt.size()));
    first.Put<std::uint32_t>(custom+0x24,16);
    Check(ReadNativeName(first.reader,custom,0x10)==salt);
    first.Put<std::uint32_t>(custom+0x24,static_cast<std::uint32_t>(salt.size()));
    Check(ReadNativeName(first.reader,custom,0x10).empty());
    first.Put<std::uint32_t>(custom+0x24,16);first.bytes[native_text+salt.size()]='x';
    Check(ReadNativeName(first.reader,custom,0x10).empty());
    Check(ReadNativeName(first.reader,UINTPTR_MAX-4,0x10).empty());
    NativeNameBuffer output(123);
    Check(output.data==output.bytes.data() && output.capacity==2048 && !output.length && !output.bytes[0]);
    Check(BuildCatalog(first.reader,first.economy,first.pools,catalog,error));
    Check(catalog.companies.size()==2 && catalog.cargos.size()==2); // Rescan does not append.
    first.Put<std::uint64_t>(first.economy+0x30,1000001);
    Check(!BuildCatalog(first.reader,first.economy,first.pools,catalog,error));
    Check(catalog.companies.empty() && catalog.cargos.empty() && !error.empty());
    first.Put<std::uint64_t>(first.economy+0x30,1);
    first.Put<std::uintptr_t>(first.economy+0x28,UINTPTR_MAX-4);
    Check(!BuildCatalog(first.reader,first.economy,first.pools,catalog,error));
    std::vector<std::string> parts;
    Check(!UnitName(first.reader,UINTPTR_MAX-4,source,parts));
    // A large mod map reuses company definitions and cargo pointers in many
    // cities. Repeated instances must not exhaust the unique-read budget.
    World large;large.bytes.resize(4*1024*1024);
    const auto shared_cargo=large.Unit({"cargo","shared_mod"});
    const auto shared_base=large.CompanyUnit("shared_base","city0",{},{});
    std::uintptr_t shared_context=0;Check(Read(large.reader,shared_base+0x68,shared_context));
    const auto shared_list=large.Alloc(50000*8);
    for(unsigned i=0;i<50000;++i)large.Put(shared_list+i*8,shared_cargo);
    for(auto offset:{0xa0u,0xc8u}){large.Put(shared_context+offset,shared_list);large.Put<std::uint64_t>(shared_context+offset+8,50000);}
    const auto base_array=large.Alloc(24*8);
    for(unsigned i=0;i<24;++i){
        const auto city="city"+std::to_string(i);
        const auto unit=large.Unit({"company","volatile","shared_base",city.c_str()});
        large.Put(unit+0x10,shared_base);large.Put(unit+0x68,shared_context);large.Put(base_array+i*8,unit);
    }
    large.Put(large.economy+0x28,base_array);large.Put<std::uint64_t>(large.economy+0x30,24);
    unsigned cargo_labels=0;
    LabelReader count_labels=[&](LabelKind kind,std::uintptr_t){if(kind==LabelKind::Cargo)++cargo_labels;return std::string("Loaded name");};
    CatalogScan incremental;Check(incremental.Begin(large.reader,large.economy,large.pools,count_labels));
    for(unsigned i=0;i<256;++i)incremental.Advance();
    Check(incremental.Active()&&incremental.Done()<incremental.Total());
    while(incremental.Active())incremental.Advance();
    Check(incremental.Error().empty()&&incremental.Done()==24&&incremental.CompanyDefinitions()==1&&incremental.CargoDefinitions()==1);
    auto large_catalog=incremental.Take();Check(large_catalog.companies.size()==24&&large_catalog.cargos.size()==1&&cargo_labels==1);
    for(const auto& company:large_catalog.companies)Check(company.outgoing.size()==1&&company.incoming.size()==1);
    // Restarting/cancelling a partial scan must discard old definitions.
    Check(incremental.Begin(large.reader,large.economy,large.pools));incremental.Advance();
    Check(!incremental.Begin(large.reader,0,large.pools)&&!incremental.Active()&&!incremental.Error().empty());
    Check(incremental.Take().companies.empty());
    // A separately loaded world completely replaces the old mod catalog.
    World second;
    const auto newcargo = second.Unit({"cargo","new_mod"});
    const auto newbase = second.CompanyUnit("new_base","new_city",{newcargo},{newcargo});
    second.List(second.economy,0x28,{newbase});
    Check(BuildCatalog(second.reader,second.economy,second.pools,catalog,error));
    Check(catalog.companies.size()==1 && catalog.cargos.size()==1 && catalog.cargos[0].id=="cargo.new_mod");
    std::uintptr_t context = 0; Check(Read(second.reader,newbase+0x68,context));
    second.Put<std::uint64_t>(context+0xa8,100001);
    Check(!BuildCatalog(second.reader,second.economy,second.pools,catalog,error));
    second.Put<std::uint64_t>(context+0xa8,1);
    second.Put<std::uint32_t>(newbase+0xc,0x100001);
    second.Put<std::uint32_t>(newbase+8,1); // A released unit must not appear even if its bytes remain readable.
    Check(!BuildCatalog(second.reader,second.economy,second.pools,catalog,error));
    second.Put<std::uint32_t>(newbase+8,0x80000001u);
    second.Put<std::uint32_t>(newbase+0xc,0); // Anonymous/invalid units are never guessed.
    Check(!BuildCatalog(second.reader,second.economy,second.pools,catalog,error));
    World regions;
    const auto city=regions.Alloc(0x200);regions.Put<std::uint32_t>(city+8,0x80000001u);
    const auto country=regions.Unit({"country","data","california"});
    regions.Put(city+0x158,World::Encode("california"));regions.Put(city+0x160,country);
    std::string region_id;
    Check(CityRegion(regions.reader,regions.pools,city,region_id)==country && region_id=="california");
    const auto other=regions.Unit({"country","data","nevada"});regions.Put(city+0x160,other);
    Check(!CityRegion(regions.reader,regions.pools,city,region_id) && region_id=="california");
    regions.Put<std::uint64_t>(city+0x158,0);
    Check(!CityRegion(regions.reader,regions.pools,city,region_id) && region_id.empty());
    Check(!CityRegion(regions.reader,regions.pools,UINTPTR_MAX-1,region_id));
    std::puts("Session catalog: live mod labels, regions, native strings, rescan and corrupt data passed.");
}
