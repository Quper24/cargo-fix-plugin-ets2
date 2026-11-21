#pragma once
// Execute the checked native append/validate routines in private memory. The
// process and saves are untouched. Naming, vector growth and duration services
// are replaced with bounded fixtures; native leg bookkeeping is executed intact.
namespace coach_probe {
struct Instruction {unsigned char bytes[15];unsigned char size,offset,width,kind;short next,target,callee,symbol;};
struct Signature {const char* name;const Instruction* instructions;std::size_t count;};
#include "coaches_signatures_162.inc"
inline void RequireAt(bool value,int line){if(!value){std::fprintf(stderr,"Coach native builder probe failed: line %d\n",line);std::exit(20);}}
#define Require(value) RequireAt((value),__LINE__)
template<class T>T Read(const void* ptr,unsigned offset){T value{};std::memcpy(&value,static_cast<const unsigned char*>(ptr)+offset,sizeof(value));return value;}
template<class T>void Write(void* ptr,unsigned offset,T value){std::memcpy(static_cast<unsigned char*>(ptr)+offset,&value,sizeof(value));}
struct Vector {std::uintptr_t table=0;void* data=nullptr;std::uint64_t count=0,capacity=32;std::uintptr_t allocator=0;};
inline void* __fastcall Name(void* out,void* stop){std::memcpy(out,static_cast<unsigned char*>(stop)+0x20,16);return out;}
inline void __fastcall PushTarget(Vector* vector,const void* pair){Require(vector->count<vector->capacity);std::memcpy(static_cast<unsigned char*>(vector->data)+vector->count++*16,pair,16);}
inline void __fastcall PushDistance(Vector* vector,const void* distance){Require(vector->count<vector->capacity);static_cast<unsigned*>(vector->data)[vector->count++]=Read<unsigned short>(distance,0);}
inline unsigned __fastcall Duration(unsigned,unsigned base_time,void*,void*,unsigned){return base_time;}
inline void __fastcall Unexpected(){Require(false);}
inline void __fastcall IgnoreDiagnostic(){}
inline std::vector<std::uint32_t> Addresses(const std::vector<unsigned char>& image,const Signature& signature,std::uint32_t root){
    std::vector<std::uint32_t> result(signature.count);result[0]=root;std::vector<unsigned> todo{0};
    auto bind=[&](short node,std::uint32_t pc){if(node<0)return;Require(!result[node]||result[node]==pc);if(!result[node]){result[node]=pc;todo.push_back(node);}};
    while(!todo.empty()){
        auto n=todo.back();todo.pop_back();auto pc=result[n];const auto& ins=signature.instructions[n];
        Require(pc+ins.size<=image.size());
        for(unsigned j=0;j<ins.size;++j)if(!(ins.width&&j>=ins.offset&&j<ins.offset+ins.width))Require(image[pc+j]==ins.bytes[j]);
        bind(ins.next,pc+ins.size);
        if(ins.target>=0){const auto disp=ins.width==1?static_cast<std::int32_t>(Read<std::int8_t>(image.data()+pc,ins.offset)):Read<std::int32_t>(image.data()+pc,ins.offset);bind(ins.target,pc+ins.size+disp);}
    }
    for(auto pc:result)Require(pc!=0);return result;
}
inline void Run(const std::vector<unsigned char>& image,const CoachesCode& code){
    auto* memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,16384,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));Require(memory!=nullptr);
    const auto& app=signatures[append_index];const auto& val=signatures[validate_index];
    auto app_pc=Addresses(image,app,code.append),val_pc=Addresses(image,val,code.validate);
    constexpr unsigned validation=0x1000;unsigned stub=0x2000,constant=0x3000;
    auto relocate=[&](const Signature& sig,const std::vector<std::uint32_t>& pc,unsigned output){
        const auto root=pc[0];unsigned end=root;
        for(std::size_t n=0;n<pc.size();++n)end=std::max(end,pc[n]+sig.instructions[n].size);
        Require(end-root<0x1000);std::memcpy(memory+output,image.data()+root,end-root);
        for(std::size_t n=0;n<pc.size();++n){const auto& ins=sig.instructions[n];if(!ins.width||ins.kind==3)continue;
            const auto at=output+pc[n]-root;std::uintptr_t destination=0;
            auto original=pc[n]+ins.size+Read<std::int32_t>(image.data()+pc[n],ins.offset);
            if(ins.kind==1){Require(original+16<=image.size()&&constant+16<=16384);std::memcpy(memory+constant,image.data()+original,16);destination=reinterpret_cast<std::uintptr_t>(memory+constant);constant+=16;}
            else if(ins.kind==2){
                if(ins.callee==static_cast<short>(validate_index))destination=reinterpret_cast<std::uintptr_t>(memory+validation);
                else{
                    std::uintptr_t function=reinterpret_cast<std::uintptr_t>(Unexpected);
                    if(ins.callee==static_cast<short>(stop_name_index))function=reinterpret_cast<std::uintptr_t>(Name);
                    // The other three external helpers are identified by their
                    // checked native call sites, not an absolute EXE address.
                    const auto relative=pc[n]-root;
                    if(&sig==&val)function=reinterpret_cast<std::uintptr_t>(IgnoreDiagnostic);
                    else{
                        if(relative==0x7c)function=reinterpret_cast<std::uintptr_t>(PushTarget);
                        if(relative==0x8a)function=reinterpret_cast<std::uintptr_t>(PushDistance);
                        if(relative==0x158)function=reinterpret_cast<std::uintptr_t>(Duration);
                    }
                    Require(stub+16<0x3000);memory[stub]=0x48;memory[stub+1]=0xb8;std::memcpy(memory+stub+2,&function,8);memory[stub+10]=0xff;memory[stub+11]=0xe0;
                    destination=reinterpret_cast<std::uintptr_t>(memory+stub);stub+=16;
                }
            }
            Require(destination!=0&&ins.width==4);const auto disp=static_cast<std::int32_t>(destination-reinterpret_cast<std::uintptr_t>(memory+at+ins.size));std::memcpy(memory+at+ins.offset,&disp,4);
        }
    };
    relocate(app,app_pc,0);relocate(val,val_pc,validation);
    DWORD old=0;Require(VirtualProtect(memory,0x3000,PAGE_EXECUTE_READ,&old)!=0);FlushInstructionCache(GetCurrentProcess(),memory,0x3000);
    alignas(16) std::array<unsigned char,0x128> planner{};
    std::array<unsigned char,0x90> settings{};Write<unsigned>(settings.data(),0x40,55);Write<unsigned>(settings.data(),0x3c,0);
    Write<std::uintptr_t>(planner.data(),8,reinterpret_cast<std::uintptr_t>(settings.data()));Write<unsigned>(planner.data(),0x1c,23);Write<unsigned>(planner.data(),0x20,23);
    std::array<std::array<unsigned char,0x70>,24> stops{};std::array<std::array<unsigned char,24>,23> rows{};
    std::array<std::array<std::uint64_t,2>,32> targets{};std::array<std::array<unsigned,32>,4> numbers{};
    Vector target_vector;target_vector.data=targets.data();std::memcpy(planner.data()+0x28,&target_vector,sizeof(target_vector));
    const unsigned offsets[]={0x50,0x78,0xa0,0xc8};
    for(unsigned i=0;i<4;++i){Vector vector;vector.data=numbers[i].data();std::memcpy(planner.data()+offsets[i],&vector,sizeof(vector));}
    for(unsigned i=0;i<stops.size();++i){Write<unsigned>(stops[i].data(),8,0x80000001);Write<std::uint64_t>(stops[i].data(),0x20,0x1234);Write<std::uint64_t>(stops[i].data(),0x28,i+1);}
    Write<std::uintptr_t>(planner.data(),0x10,reinterpret_cast<std::uintptr_t>(stops[0].data()));Write<std::uintptr_t>(planner.data(),0xf0,reinterpret_cast<std::uintptr_t>(stops[0].data()));
    for(unsigned i=0;i<rows.size();++i){
        Write<std::uintptr_t>(rows[i].data(),0,reinterpret_cast<std::uintptr_t>(stops[i+1].data()));Write<unsigned short>(rows[i].data(),8,100+i);Write<unsigned short>(rows[i].data(),0xa,0xffff);Write<unsigned short>(rows[i].data(),0x10,120+i);
        const bool more=reinterpret_cast<bool(__fastcall*)(void*,void*)>(memory)(planner.data(),rows[i].data());Require(more==(i+1<23));
    }
    Require(Read<std::uint64_t>(planner.data(),0x38)==23);
    for(unsigned i=0;i<4;++i)Require(Read<std::uint64_t>(planner.data(),offsets[i]+16)==23);
    for(unsigned i=0;i<23;++i){Require(targets[i][1]==i+2);Require(numbers[0][i]==100+i);Require(numbers[2][i]==0);Require(numbers[3][i]==120+i);}
    Require(reinterpret_cast<bool(__fastcall*)(void*)>(memory+validation)(planner.data()));
    Write<unsigned>(planner.data(),0x118,Read<unsigned>(planner.data(),0x114));Require(!reinterpret_cast<bool(__fastcall*)(void*)>(memory+validation)(planner.data()));
    Require(VirtualFree(memory,0,MEM_RELEASE)!=0);
    std::puts("Native Coaches append/validate built 23 ordered legs (24 points); ferry rejection checked. Engine services use fixtures.");
}
}
#undef Require
