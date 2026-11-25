#include "overlay_ui.h"
#include "branding.h"
#include "brand_palette.h"
#include "notifications.h"
#ifdef CARGO_CONTROL_COACHES
#include "coaches.h"
#endif
#include "menu_pointer.h"
#ifdef CARGO_CONTROL_ROAD_TRIP
#include "road_trip.h"
#endif
#ifdef CARGO_CONTROL_CUSTOM_JOBS
#include "custom_jobs.h"
#endif
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <algorithm>
using Microsoft::WRL::ComPtr;
#define Check(ok) do { if(!(ok)) { std::fprintf(stderr,"DX11 check failed: line %d, error %lu\n",__LINE__,GetLastError());std::exit(1); } }while(0)
void Log(const char* s) { std::puts(s); }
WNDPROC chained_previous = nullptr;
unsigned chained_calls = 0;
LRESULT CALLBACK ChainedProcedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    ++chained_calls;
    return CallWindowProcW(chained_previous, hwnd, msg, w, l);
}
void Capture(ID3D11Device* dev,ID3D11DeviceContext* gpu,IDXGISwapChain* chain,const wchar_t* path,bool menu_pixels=false,bool progress=false,bool catalog=false) {
    ComPtr<ID3D11Texture2D> back,copy;Check(SUCCEEDED(chain->GetBuffer(0,IID_PPV_ARGS(&back))));
    D3D11_TEXTURE2D_DESC desc{};back->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    Check(SUCCEEDED(dev->CreateTexture2D(&desc,nullptr,&copy)));gpu->CopyResource(copy.Get(),back.Get());
    D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(gpu->Map(copy.Get(),0,D3D11_MAP_READ,0,&map)));
    const auto* pixel=static_cast<unsigned char*>(map.pData);size_t green=0;
    for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x) {
        const auto* p=pixel+y*map.RowPitch+x*4;if(p[1]>160 && p[1]>p[0]+10 && p[1]>p[2]+10)++green;
    }
    Check(green>(progress?50:500));
    const float menu_scale=std::min(overlay_ui::ScalePercent()/100.f,std::min((desc.Width-24.f)/900.f,(desc.Height-24.f)/660.f));
    // Map the original 1024x768 reference regions to the fitted menu.
    auto menu_x=[&](float x){return static_cast<unsigned>((desc.Width-900*menu_scale)*.5f+(x-62)*menu_scale);};
    auto menu_y=[&](float y){return static_cast<unsigned>((desc.Height-660*menu_scale)*.5f+(y-54)*menu_scale);};
    if(menu_pixels) {
        const auto* p=pixel+menu_y(380)*map.RowPitch+menu_x(140)*4;
        Check(p[2]==brand_palette::Red && p[1]==brand_palette::Green && p[0]==brand_palette::Blue);
        unsigned label_pixels=0;
        for(unsigned y=menu_y(384);y<menu_y(410);++y)for(unsigned x=menu_x(192);x<menu_x(412);++x){
            const auto* q=pixel+y*map.RowPitch+x*4;
            if(q[0]<50 && q[1]<50 && q[2]<50)++label_pixels;
        }
        Check(label_pixels>20); // The green button must contain an actual readable label.
    }
    if(menu_pixels || catalog){
        unsigned accent_logo=0,white_logo=0,white_title=0;
        for(unsigned y=menu_y(105);y<menu_y(150);++y)for(unsigned x=menu_x(90);x<menu_x(300);++x){
            const auto* q=pixel+y*map.RowPitch+x*4;if(q[0]>235 && q[1]>235 && q[2]>235)++white_title;
        }
        for(unsigned y=menu_y(77);y<menu_y(174);++y)for(unsigned x=menu_x(772);x<menu_x(904);++x){
            const auto* q=pixel+y*map.RowPitch+x*4;
            if(std::abs(int(q[2])-int(brand_palette::Red))<=3 && std::abs(int(q[1])-int(brand_palette::Green))<=3 && std::abs(int(q[0])-int(brand_palette::Blue))<=3)++accent_logo;
            if(q[0]>235 && q[1]>235 && q[2]>235)++white_logo;
        }
        Check(accent_logo>10 && white_logo>100 && white_title>200);
    } // Real branded pixels must reach the DX11 backbuffer.
    if(path) {
        Gdiplus::Bitmap image(desc.Width,desc.Height,map.RowPitch,PixelFormat32bppRGB,const_cast<BYTE*>(pixel));
        UINT count=0,size=0;Gdiplus::GetImageEncodersSize(&count,&size);std::vector<BYTE> buf(size);
        auto* enc=reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());Gdiplus::GetImageEncoders(count,size,enc);
        bool saved=false;for(UINT i=0;i<count;++i)if(wcscmp(enc[i].MimeType,L"image/png")==0){saved=image.Save(path,&enc[i].Clsid)==Gdiplus::Ok;break;}Check(saved);
    }
    gpu->Unmap(copy.Get(),0);
}
int wmain(int argc,wchar_t** argv) {
    const auto settings_dir=std::filesystem::temp_directory_path()/(L"CargoControl-ui-check-"+std::to_wstring(GetCurrentProcessId()));
    Check(std::filesystem::create_directory(settings_dir));
    const auto settings=settings_dir/L"CargoControl.ini";
    notifications::Initialize(settings_dir);
    Check(notifications::SetLanguage(interface_language::Choice::Russian));
    Check(branding::Initialize());
    WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"QSCargoOfflineDx11";Check(RegisterClassW(&cls));
    HWND window=CreateWindowExW(0,cls.lpszClassName,L"",WS_POPUP,0,0,1024,768,nullptr,nullptr,cls.hInstance,nullptr);Check(window);
    const auto original_proc=GetWindowLongPtrW(window,GWLP_WNDPROC);
    DXGI_SWAP_CHAIN_DESC desc{};desc.BufferCount=1;desc.BufferDesc.Width=1024;desc.BufferDesc.Height=768;desc.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.OutputWindow=window;desc.SampleDesc.Count=1;desc.Windowed=TRUE;
    ComPtr<IDXGISwapChain> chain;ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> gpu;
    Check(SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&chain,&dev,nullptr,&gpu)));
    auto render=[&](bool menu=false,int page=-1){
        ComPtr<ID3D11Texture2D> back;ComPtr<ID3D11RenderTargetView> view;
        Check(SUCCEEDED(chain->GetBuffer(0,IID_PPV_ARGS(&back))));
        D3D11_RENDER_TARGET_VIEW_DESC target{};target.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;target.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
        Check(SUCCEEDED(dev->CreateRenderTargetView(back.Get(),&target,&view)));
        const float clear[]={.025f,.025f,.025f,1};gpu->ClearRenderTargetView(view.Get(),clear);
        overlay_ui::RenderFrame(chain.Get(),true,menu,page);
    };
    Check(overlay_ui::Initialize(settings_dir,Log,false));
    Check(overlay_ui::ScalePercent()==130);
    overlay_ui::Request(2);overlay_ui::SetProgress(123,1000);Check(overlay_ui::Tick()==0); // Feedback must be rendered before work starts.
    for(int i=0;i<3;++i)render();Sleep(160);
    Check(overlay_ui::Inspect().operation==2);
    overlay_ui::Request(1);Check(overlay_ui::Tick()==2 && overlay_ui::Tick()==0); // Duplicate action is ignored.
    overlay_ui::FinishAction();Check(overlay_ui::Inspect().operation==0);
    overlay_ui::Request(1);overlay_ui::CancelPending();Check(overlay_ui::Tick()==0);
    D3D11_VIEWPORT viewport{11,12,333,222,.2f,.8f};gpu->RSSetViewports(1,&viewport);
    for(int i=0;i<3;++i)render(true);
    const auto status=overlay_ui::Inspect();Check(status.ready && status.logo && status.frames>0);
    D3D11_VIEWPORT restored{};UINT n=1;gpu->RSGetViewports(&n,&restored);Check(n==1 && !std::memcmp(&restored,&viewport,sizeof(viewport)));
    Capture(dev.Get(),gpu.Get(),chain.Get(),argc>1?argv[1]:nullptr,true);
    Check(!IsWindowVisible(window)); // No desktop UI was shown by the test.
    Check(SUCCEEDED(chain->ResizeBuffers(0,3440,1440,DXGI_FORMAT_UNKNOWN,0)));
    SetWindowPos(window,nullptr,0,0,3440,1440,SWP_NOACTIVATE|SWP_NOZORDER);
    for(int i=0;i<3;++i)render(true);
    Capture(dev.Get(),gpu.Get(),chain.Get(),argc>12?argv[12]:nullptr,true);
    Check(SUCCEEDED(chain->ResizeBuffers(0,1280,720,DXGI_FORMAT_UNKNOWN,0)));
    SetWindowPos(window,nullptr,0,0,1280,720,SWP_NOACTIVATE|SWP_NOZORDER);
    overlay_ui::Notify(L"Биржа грузов\nДобавлено заказов: 12 480. Доступно запусков: 2 из 3.");
    for(int i=0;i<3;++i)render();
    Capture(dev.Get(),gpu.Get(),chain.Get(),argc>2?argv[2]:nullptr);
    Check(SUCCEEDED(chain->ResizeBuffers(0,1024,768,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,0)));
    SetWindowPos(window,nullptr,0,0,1024,768,SWP_NOACTIVATE|SWP_NOZORDER);
    for(int i=0;i<3;++i)render(true);
    Capture(dev.Get(),gpu.Get(),chain.Get(),nullptr,true); // Exact same palette on an sRGB backbuffer.
#ifdef CARGO_CONTROL_CUSTOM_JOBS
    custom_jobs::Catalog fixture;
    fixture.cargos.push_back({"cargo.mod_cargo9","Соль"});
    fixture.companies.push_back({"company.volatile.mod_depot.mod_city","mod_city","FedEx",{"cargo.mod_cargo9"},{},"Анн-Арбор"});
    fixture.companies.push_back({"company.volatile.other_base.other_city","other_city","Pepsi",{}, {"cargo.mod_cargo9"},"Кенора"});
    custom_jobs::PreviewCatalog(std::move(fixture));
#ifdef CARGO_CONTROL_ROAD_TRIP
    road_trip::View road;
    road.points={{"mod_depot.mod_city","mod_city","Анн-Арбор","FedEx"},{"other_base.other_city","other_city","Кенора","Pepsi"}};
    road.tasks={{"mod.task|mod.cargo","Посылки",{"mod_depot.mod_city"},{"other_base.other_city"}}};
    road_trip::Preview(std::move(road));
    for(int i=0;i<3;++i)render(true,2);
    Capture(dev.Get(),gpu.Get(),chain.Get(),argc>5?argv[5]:nullptr);
    overlay_ui::Request(5);for(int i=0;i<3;++i)render();Sleep(160);
    Check(overlay_ui::Tick()==5);overlay_ui::FinishAction(true,2);render();
    Check(overlay_ui::Inspect().menu);
#endif
    for(int i=0;i<3;++i)render(true,1);
#ifdef CARGO_CONTROL_COACHES
    coaches::View coach;coach.session=1;coach.stops={{"busstop.city.paris","Париж"},{"busstop.city.berlin","Берлин"}};
    coaches::Preview(std::move(coach));
    for(int i=0;i<3;++i)render(true,3);
    Capture(dev.Get(),gpu.Get(),chain.Get(),argc>6?argv[6]:nullptr,false,false,true);
    coaches::View long_route;long_route.session=2;
    for(unsigned i=0;i<24;++i){const auto id="busstop.city."+std::to_string(i);
        long_route.stops.push_back({id,i==0?"Санкт-Петербург - центральный автовокзал":"Станция "+std::to_string(i+1)});
        long_route.preview_route.push_back(id);
    }
    coaches::Preview(long_route);
    for(int i=0;i<3;++i)render(true,3);
    Capture(dev.Get(),gpu.Get(),chain.Get(),argc>7?argv[7]:nullptr,false,false,true);
    // Smaller displays must still render all pages without exposing a desktop window.
    Check(SUCCEEDED(chain->ResizeBuffers(0,800,600,DXGI_FORMAT_B8G8R8A8_UNORM,0)));
    SetWindowPos(window,nullptr,0,0,800,600,SWP_NOACTIVATE|SWP_NOZORDER);
    for(int page:{0,1,2,3}){for(int i=0;i<3;++i)render(true,page);Capture(dev.Get(),gpu.Get(),chain.Get(),page==3&&argc>8?argv[8]:page==0&&argc>9?argv[9]:nullptr);}
    Check(SUCCEEDED(chain->ResizeBuffers(0,1024,768,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,0)));
    SetWindowPos(window,nullptr,0,0,1024,768,SWP_NOACTIVATE|SWP_NOZORDER);
    overlay_ui::Request(8);for(int i=0;i<3;++i)render();Sleep(160);
    Check(overlay_ui::Tick()==8);overlay_ui::FinishAction(true,3);render();Check(overlay_ui::Inspect().menu);
    overlay_ui::Request(9);overlay_ui::SetProgress(12,100);for(int i=0;i<3;++i)render();Sleep(160);
    Check(overlay_ui::Tick()==9);overlay_ui::FinishAction();
    overlay_ui::Request(10);for(int i=0;i<3;++i)render();Sleep(160);
    Check(overlay_ui::Tick()==10);overlay_ui::FinishAction();
    for(int i=0;i<3;++i)render(true,1);
#endif
    Capture(dev.Get(),gpu.Get(),chain.Get(),argc>3?argv[3]:nullptr,false,false,true);
    overlay_ui::Request(3);Check(overlay_ui::Tick()==0);
    for(int i=0;i<3;++i)render();Sleep(160);
    Capture(dev.Get(),gpu.Get(),chain.Get(),argc>4?argv[4]:nullptr,false,true);
    Check(overlay_ui::Tick()==3 && overlay_ui::Tick()==0);
    overlay_ui::FinishAction(true);
    render();
    Check(overlay_ui::Inspect().operation==0 && overlay_ui::Inspect().menu);
    custom_jobs::PreviewCatalog({},"Cargo list exceeds the scan budget.");
    for(int i=0;i<3;++i)render(true,1);
    Capture(dev.Get(),gpu.Get(),chain.Get(),argc>11?argv[11]:nullptr,false,false,true);
    custom_jobs::Reset();Check(!custom_jobs::Snapshot()->scanned);
#endif
    Check(overlay_ui::SetScalePercent(999));Check(overlay_ui::ScalePercent()==300);
    Check(SUCCEEDED(chain->ResizeBuffers(0,3840,2160,DXGI_FORMAT_B8G8R8A8_UNORM,0)));
    SetWindowPos(window,nullptr,0,0,3840,2160,SWP_NOACTIVATE|SWP_NOZORDER);
    for(int i=0;i<3;++i)render(true,0);
    Capture(dev.Get(),gpu.Get(),chain.Get(),argc>14?argv[14]:nullptr,true);
    Check(SUCCEEDED(chain->ResizeBuffers(0,800,600,DXGI_FORMAT_UNKNOWN,0)));
    SetWindowPos(window,nullptr,0,0,800,600,SWP_NOACTIVATE|SWP_NOZORDER);
    for(int page:{0,1,2,3}){for(int i=0;i<3;++i)render(true,page);Capture(dev.Get(),gpu.Get(),chain.Get(),nullptr);}
    Check(overlay_ui::SetScalePercent(-1));Check(overlay_ui::ScalePercent()==75);
    for(int i=0;i<3;++i)render(true,0);
    Capture(dev.Get(),gpu.Get(),chain.Get(),nullptr,true);
    Check(overlay_ui::SetScalePercent(160));
    Check(GetPrivateProfileIntW(L"Interface",L"ScalePercent",0,settings.c_str())==160);
    Check(notifications::SetLanguage(interface_language::Choice::English));Check(!notifications::Russian());
    Check(overlay_ui::SetScalePercent(130));
    Check(SUCCEEDED(chain->ResizeBuffers(0,1920,1080,DXGI_FORMAT_UNKNOWN,0)));
    SetWindowPos(window,nullptr,0,0,1920,1080,SWP_NOACTIVATE|SWP_NOZORDER);
    for(int i=0;i<3;++i)render(true,0);
    Capture(dev.Get(),gpu.Get(),chain.Get(),argc>13?argv[13]:nullptr,true);
    Check(overlay_ui::SetScalePercent(160));
    overlay_ui::Shutdown();Check(GetWindowLongPtrW(window,GWLP_WNDPROC)==original_proc);
    Check(overlay_ui::Initialize(settings_dir,Log,true));
    Check(overlay_ui::ScalePercent()==160); // Persists across renderer/plugin restarts.
    render();
    // Simulate a plugin installed after Cargo Control. Shutdown must preserve
    // that plugin's chain and allow calls through our dormant procedure.
    chained_previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ChainedProcedure)));
    Check(chained_previous && chained_previous != reinterpret_cast<WNDPROC>(original_proc));
    overlay_ui::Shutdown();
    Check(GetWindowLongPtrW(window,GWLP_WNDPROC)==reinterpret_cast<LONG_PTR>(ChainedProcedure));
    const auto before_message = chained_calls;
    Check(SendMessageW(window, WM_NCHITTEST, 0, 0) == CallWindowProcW(reinterpret_cast<WNDPROC>(original_proc), window, WM_NCHITTEST, 0, 0));
    Check(chained_calls > before_message);
    Check(overlay_ui::Initialize(settings_dir,Log,true));
    chain->Present(0,DXGI_PRESENT_TEST);Check(overlay_ui::Inspect().present_calls>0);
    ComPtr<IDirectInput8A> input;ComPtr<IDirectInputDevice8A> mouse;
    Check(SUCCEEDED(DirectInput8Create(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,IID_IDirectInput8A,reinterpret_cast<void**>(input.GetAddressOf()),nullptr)));
    Check(SUCCEEDED(input->CreateDevice(GUID_SysMouse,&mouse,nullptr)));
    DIMOUSESTATE2 mouse_state{};
    const auto before_input=overlay_ui::Inspect().input_calls;
    const auto original_result=mouse->GetDeviceState(sizeof(mouse_state),&mouse_state); // Unacquired: no real desktop input is consumed.
    Check(overlay_ui::Inspect().input_calls>before_input); // ANSI devices reach the same mouse hook.
    overlay_ui::Shutdown();
    Check(mouse->GetDeviceState(sizeof(mouse_state),&mouse_state)==original_result);
    mouse.Reset();input.Reset();
    chain.Reset();gpu.Reset();dev.Reset();
    const auto before_destroy = chained_calls;
    DestroyWindow(window);Check(chained_calls > before_destroy);
    UnregisterClassW(cls.lpszClassName,cls.hInstance);notifications::Shutdown();
    Check(std::filesystem::remove(settings));Check(std::filesystem::remove(settings_dir));
    std::puts("DX11 pixels, branded logo, viewport preservation, resize, window procedure restore, Present/DirectInput hooks and exact UNORM/sRGB palette passed.");
}
