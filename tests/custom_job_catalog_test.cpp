#include "custom_job_catalog.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#define Check(value) do { if (!(value)) { std::fprintf(stderr,"Catalog check failed: %d\n",__LINE__); std::exit(1); } } while(0)
using namespace custom_jobs;
struct World {
    std::vector<unsigned char> bytes = std::vector<unsigned char>(1024 * 1024);
    std::uintptr_t cursor = 0x1000, pools, economy;
    std::uintptr_t token_tables[2]{};
    unsigned next_row[2]{1,1};
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
        for(unsigned index=0;index<2;++index) {
            const auto pool = Alloc(0x90); token_tables[index] = Alloc(64 * (index ? 4 : 2) * 8);
            Put(pools+index*8,pool); Put(pool,token_tables[index]); Put<std::uint8_t>(pool+0x88,index ? 4 : 2);
        }
    }
    std::uintptr_t Unit(std::initializer_list<const char*> parts) {
        const unsigned index = parts.size()==4 ? 1 : 0;
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
    const auto custom = first.Unit({"cargo","mod_cargo9"});
    const auto ordinary = first.Unit({"cargo","apples"});
    const auto source = first.CompanyUnit("mod_depot","mod_city",{custom,ordinary,custom},{ordinary});
    const auto target = first.CompanyUnit("mod_depot","other_city",{ordinary},{custom});
    first.List(first.economy,0x28,{source,target,source});
    Catalog catalog; std::string error;
    Check(BuildCatalog(first.reader,first.economy,first.pools,catalog,error));
    Check(error.empty() && catalog.companies.size()==2 && catalog.cargos.size()==2);
    const auto& from = catalog.companies[0]; const auto& to = catalog.companies[1];
    Check(from.id=="company.volatile.mod_depot.mod_city" && from.outgoing.size()==2);
    Check(to.name==from.name && to.city!=from.city); // Distinct bases with the same brand.
    Check(Accepts(to,"cargo.mod_cargo9") && !Accepts(to,"cargo.apples"));
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
    std::puts("Session catalog: mod data, base identity, rescan, new world, invalid counts, pointers and tokens passed.");
}
