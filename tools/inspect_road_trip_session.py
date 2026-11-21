"""Read-only Road Trip session diagnostics. Never writes to a process or save."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import struct
from compare_executables import Image, ROOT

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    args = parser.parse_args()
    kernel = C.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    kernel.OpenProcess.restype = W.HANDLE
    kernel.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
    kernel.CloseHandle.argtypes = [W.HANDLE]
    process = kernel.OpenProcess(0x410, False, args.pid)
    if not process:
        raise C.WinError(C.get_last_error())
    def read(address, length):
        if not address or length > 16*1024*1024:
            raise ValueError('Invalid diagnostic read')
        buffer = C.create_string_buffer(length)
        count = C.c_size_t()
        if not kernel.ReadProcessMemory(process, address, buffer, length, C.byref(count)) or count.value != length:
            raise C.WinError(C.get_last_error())
        return buffer.raw
    def u64(address): return struct.unpack('<Q', read(address, 8))[0]
    psapi = C.WinDLL('psapi', use_last_error=True)
    psapi.EnumProcessModules.argtypes = [W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD)]
    modules = (C.c_void_p*1024)()
    needed = W.DWORD()
    if not psapi.EnumProcessModules(process, modules, C.sizeof(modules), C.byref(needed)):
        raise C.WinError(C.get_last_error())
    base = modules[0]
    im = Image(ROOT/'diagnostics/executables/ats-public-1.62.2.2/amtrucks.exe')
    # Read-only locator; production uses the existing full linked flow resolver.
    prefix = bytes.fromhex('49837e2003764d498b4e184883c168')
    start = im.data.find(prefix)
    if start < 0 or im.data.find(prefix,start+1) >= 0:
        raise ValueError('Economy source is ambiguous')
    at = start + 0x20
    if im.data[at:at+3] != bytes.fromhex('488b0d'):
        raise ValueError('Economy source differs')
    economy_rva = at+7+struct.unpack_from('<i',im.data,at+3)[0]
    economy = u64(base+economy_rva)
    generator = u64(economy+0x1d48)
    player = u64(economy+0x18)
    def words(address,size):
        return {hex(i):hex(u64(address+i)) for i in range(0,size,8)}
    def text(address):
        pointer=u64(address+8)
        length=struct.unpack('<I',read(address+16,4))[0]
        return read(pointer,length).decode('utf-8',errors='replace') if 0<length<2048 else ''
    offers=[]
    vehicles=u64(player+0x78)
    car=u64(vehicles+16) if u64(player+0x80)>2 else 0
    car_report={'object':hex(car)}
    if car:
        car_report['words']=words(car,0x60)
        definition=u64(car+0x10)
        car_report['definition_words']=words(definition,0x70)
        accessories=u64(definition+0x38)
        acc_data,acc_count=u64(accessories+8),u64(accessories+16)
        models=[]
        for i in range(min(acc_count,512)):
            accessory=u64(acc_data+i*8)
            model=u64(accessory+0x38)
            if not model:continue
            vt=u64(model);getter=u64(vt+0x28);body=read(getter,8)
            if body[:3]!=bytes.fromhex('488d05'):continue
            descriptor=getter+7+struct.unpack_from('<i',body,3)[0]
            owner=descriptor
            for _ in range(16):
                if owner==base+0x1e54d30:
                    data,n=u64(model+0x390),u64(model+0x398)
                    if n>128:raise ValueError('Invalid vehicle tags')
                    car_report['tags']=[u64(data+j*8) for j in range(n)]
                    models.append(hex(model));break
                owner=u64(owner+0x18)
                if not owner:break
        car_report['models']=models
    data,count=u64(generator+0x18),u64(generator+0x20)
    if count>10000: raise ValueError('Invalid offer count')
    for i in range(count):
        offer=u64(data+i*8)
        if i<6 or text(offer+0x10):
            offers.append({'slot':i,'address':hex(offer),'source':text(offer+0x10),'target':text(offer+0x48),
                           'words':words(offer,0xd0)})
            if len(offers)>=12:break
    # Definition list is reached by the native try_generate_offer RIP reference.
    instructions=im.ins(0x80f200)
    root=next(i for i in instructions if i.address==0x80f234)
    registry=u64(base+root.address+root.size+root.disp)
    definitions=u64(registry+0x2ff8)+0x8c8
    defs=[]
    ptr,n=u64(definitions+8),u64(definitions+16)
    if n>10000:raise ValueError('Invalid definition count')
    for i in range(n):
        obj=u64(ptr+i*8)
        defs.append({'address':hex(obj),'name':text(obj+0x10),'words':words(obj,0x140)})
    compatible=0;known_points=set();native_companies={}
    company_data,company_count=u64(economy+0x28),u64(economy+0x30)
    if company_count>1000000:raise ValueError('Invalid company count')
    for i in range(company_count):
        company=u64(company_data+i*8)
        if company:native_companies[u64(company+0x10)]=company
    for entry in defs:
        obj=int(entry['address'],16)
        data,n=u64(obj+0x68),u64(obj+0x70)
        if n>128:raise ValueError('Invalid job vehicle types')
        if not set(car_report.get('tags',[]))&{u64(data+j*8) for j in range(n)}:continue
        compatible+=1
        for offset in [0x100,0x128]:
            data,n=u64(obj+offset),u64(obj+offset+8)
            if n>1000000:raise ValueError('Invalid task points')
            for j in range(n):
                item=u64(data+j*8)
                if item in native_companies:known_points.add(item)
    result={'scope':'ReadProcessMemory only','pid':args.pid,'base':hex(base),'economy':hex(economy),
            'player':words(player,0xa0),'generator':words(generator,0x70),'offer_count':count,
            'offers':offers,'definitions':defs,'car':car_report,'compatible_tasks':compatible,'matched_points':len(known_points)}
    path=ROOT/'diagnostics/road-trip/session.json'
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({'report':str(path),'offers':count,'definitions':len(defs),'compatible_tasks':compatible,'matched_points':len(known_points),'car':car_report.get('models',[])},ensure_ascii=False))
    kernel.CloseHandle(process)

if __name__=='__main__':main()
