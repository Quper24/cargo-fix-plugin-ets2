"""Read-only Coaches inspection: no hooks, process writes or save changes."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import struct
from compare_executables import Image, ROOT

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid',type=int)
    args=parser.parse_args()
    image=Image(ROOT/'diagnostics/executables/ets2-public-1.62.3.3/eurotrucks2.exe')
    kernel=C.WinDLL('kernel32',use_last_error=True)
    kernel.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];kernel.OpenProcess.restype=W.HANDLE
    kernel.ReadProcessMemory.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.POINTER(C.c_size_t)]
    kernel.CloseHandle.argtypes=[W.HANDLE]
    process=kernel.OpenProcess(0x410,False,args.pid)
    if not process:raise C.WinError(C.get_last_error())
    def read(address,length):
        if not address or length>16*1024*1024:raise ValueError('Invalid diagnostic read')
        buffer=C.create_string_buffer(length);count=C.c_size_t()
        if not kernel.ReadProcessMemory(process,address,buffer,length,C.byref(count)) or count.value!=length:raise C.WinError(C.get_last_error())
        return buffer.raw
    def u64(at):return struct.unpack('<Q',read(at,8))[0]
    def u32(at):return struct.unpack('<I',read(at,4))[0]
    def words(at,length):return {hex(i):hex(u64(at+i)) for i in range(0,length,8)}
    def pointers(at,limit):
        data,count=u64(at),u64(at+8)
        if count>limit:raise ValueError('Invalid diagnostic pointer count')
        return list(struct.unpack('<'+'Q'*count,read(data,count*8))) if count else []
    def native_text(at):
        data,length=u64(at+8),u32(at+16)
        return read(data,length).decode('utf-8',errors='replace') if 0<length<2048 else ''
    def token(value):
        alphabet='\0'+'0123456789abcdefghijklmnopqrstuvwxyz_';out=''
        while value and len(out)<13:
            value,digit=divmod(value,38)
            if not digit:return ''
            out+=alphabet[digit]
        return out if not value else ''
    psapi=C.WinDLL('psapi',use_last_error=True)
    psapi.EnumProcessModules.argtypes=[W.HANDLE,C.c_void_p,W.DWORD,C.POINTER(W.DWORD)]
    modules=(C.c_void_p*1024)();needed=W.DWORD()
    if not psapi.EnumProcessModules(process,modules,C.sizeof(modules),C.byref(needed)):raise C.WinError(C.get_last_error())
    base=modules[0]
    prefix=bytes.fromhex('49837e2003764d498b4e184883c168');start=image.data.find(prefix)
    if start<0 or image.data.find(prefix,start+1)>=0:raise ValueError('Ambiguous economy source')
    at=start+0x20
    if image.data[at:at+3]!=bytes.fromhex('488b0d'):raise ValueError('Unexpected economy source')
    economy=u64(base+at+7+struct.unpack_from('<i',image.data,at+3)[0])
    if not economy:raise ValueError('No loaded ETS2 economy')
    player=u64(economy+0x18)
    stops=pointers(economy+0x1d68,1000000)
    rows=[];offers_total=0;multi=0;segments=[];max_targets=0;route_types={};lengths={};mismatched=0
    stop_set=set(stops);cache_matches={16:0,24:0,32:0};cache_checked=0
    for stop in stops:
        cache_data,cache_count=u64(stop+0x20),u64(stop+0x28)
        if cache_count>1000000:raise ValueError('Invalid stop route cache')
        if cache_count:
            take=min(cache_count,20);cache_checked+=take
            for stride in cache_matches:
                block=read(cache_data,take*stride)
                cache_matches[stride]+=sum(struct.unpack_from('<Q',block,j*stride)[0] in stop_set for j in range(take))
        offers=pointers(stop+0x70,10000);offers_total+=len(offers)
        if len(rows)<6:
            row={'address':hex(stop),'offer_count':len(offers),'words':words(stop,0x98),'offers':[]}
        else:row=None
        for offer in offers:
            data,n=u64(offer+0x28),u64(offer+0x30)
            if n>1024:raise ValueError('Invalid target count')
            raw=read(data,n*16) if n else b''
            targets=['.'.join(token(x) for x in struct.unpack_from('<QQ',raw,j*16)) for j in range(n)]
            max_targets=max(max_targets,n);lengths[n]=lengths.get(n,0)+1
            kind=u32(offer+0x10);route_types[kind]=route_types.get(kind,0)+1
            arrays={}
            for key,offset in [('distances',0x48),('times',0x70),('ferry_times',0x98),('base_times',0xc0)]:
                array_data,array_count=u64(offer+offset+8),u64(offer+offset+16)
                if array_count>1024:raise ValueError('Invalid leg array count')
                arrays[key]=list(struct.unpack('<'+'I'*array_count,read(array_data,array_count*4))) if array_count else []
            if any(len(values)!=n for values in arrays.values()):mismatched+=1
            if len(targets)>1:multi+=1
            if row is not None and len(row['offers'])<2:
                row['offers'].append({'address':hex(offer),'bus':native_text(offer+0xf0),'words':words(offer,0x148),'targets':targets,'legs':arrays})
            if len(segments)<20 and targets:
                segments.append({'source':hex(u64(offer+0x18)),'target_count':len(targets),'targets':targets})
        if row is not None:
            item=u64(stop+0x10)
            if item:
                row['map_item_words']=words(item,0xc8)
                row['city_token_at_48']=token(u64(item+0x48))
            rows.append(row)
    bus_settings=u64(economy+0x1d58)
    report={'scope':'ReadProcessMemory only; offsets are diagnostic hypotheses until checked against native code',
            'pid':args.pid,'base':hex(base),'economy':hex(economy),'player':words(player,0x290),
            'economy_bus_region':{hex(i):hex(u64(economy+i)) for i in range(0x1d40,0x1df0,8)},
            'settings_at_1d58':words(bus_settings,0x90) if bus_settings else {},
            'stop_count':len(stops),'offer_count':offers_total,'multi_stop_offers':multi,'max_targets':max_targets,
            'target_count_histogram':lengths,'route_types':route_types,'parallel_array_mismatches':mismatched,
            'route_cache_probe':{'checked':cache_checked,'matching_destinations_by_stride':cache_matches},'stops':rows,'segments':segments}
    path=ROOT/'diagnostics/coaches/session.json';path.parent.mkdir(parents=True,exist_ok=True)
    path.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({'report':str(path),'stops':len(stops),'offers':offers_total,'multi_stop_offers':multi,'max_targets':max_targets,
        'parallel_array_mismatches':mismatched,'vehicle_mode':u32(player+0x68),'configured_stops_max':u32(bus_settings+0x18),
        'target_count_histogram':lengths,'route_cache_probe':report['route_cache_probe']},ensure_ascii=False))
    kernel.CloseHandle(process)

if __name__=='__main__':main()
