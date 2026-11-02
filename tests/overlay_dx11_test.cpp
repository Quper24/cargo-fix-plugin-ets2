#include "overlay_ui.h"
#include "branding.h"
#include "brand_palette.h"
#include "menu_pointer.h"
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
using Microsoft::WRL::ComPtr;
namespace notifications { bool Russian() { return true; } }
#define Check(ok) do { if(!(ok)) { std::fprintf(stderr,"DX11 check failed: line %d, error %lu\n",__LINE__,GetLastError());std::exit(1); } }while(0)
void Log(const char* s) { std::puts(s); }
void Capture(ID3D11Device* dev,ID3D11DeviceContext* gpu,IDXGISwapChain* chain,const wchar_t* path,bool menu_pixels=false) {
    ComPtr<ID3D11Texture2D> back,copy;Check(SUCCEEDED(chain->GetBuffer(0,IID_PPV_ARGS(&back))));
    D3D11_TEXTURE2D_DESC desc{};back->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    Check(SUCCEEDED(dev->CreateTexture2D(&desc,nullptr,&copy)));gpu->CopyResource(copy.Get(),back.Get());
    D3D11_MAPPED_SUBRESOURCE map{};Check(SUCCEEDED(gpu->Map(copy.Get(),0,D3D11_MAP_READ,0,&map)));
    const auto* pixel=static_cast<unsigned char*>(map.pData);size_t green=0;
    for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x) {
        const auto* p=pixel+y*map.RowPitch+x*4;if(p[1]>160 && p[1]>p[0]+10 && p[1]>p[2]+10)++green;
    }
    Check(green>500);
    if(menu_pixels) {
        const auto* p=pixel+375*map.RowPitch+140*4;
        Check(p[2]==brand_palette::Red && p[1]==brand_palette::Green && p[0]==brand_palette::Blue);
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
    Check(branding::Initialize());
    WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"QSCargoOfflineDx11";Check(RegisterClassW(&cls));
    HWND window=CreateWindowExW(0,cls.lpszClassName,L"",WS_POPUP,0,0,1024,768,nullptr,nullptr,cls.hInstance,nullptr);Check(window);
    const auto original_proc=GetWindowLongPtrW(window,GWLP_WNDPROC);
    DXGI_SWAP_CHAIN_DESC desc{};desc.BufferCount=1;desc.BufferDesc.Width=1024;desc.BufferDesc.Height=768;desc.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.OutputWindow=window;desc.SampleDesc.Count=1;desc.Windowed=TRUE;
    ComPtr<IDXGISwapChain> chain;ComPtr<ID3D11Device> dev;ComPtr<ID3D11DeviceContext> gpu;
    Check(SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&chain,&dev,nullptr,&gpu)));
    Check(overlay_ui::Initialize(std::filesystem::temp_directory_path(),Log,false));
    D3D11_VIEWPORT viewport{11,12,333,222,.2f,.8f};gpu->RSSetViewports(1,&viewport);
    for(int i=0;i<3;++i)overlay_ui::RenderFrame(chain.Get(),true,true);
    const auto status=overlay_ui::Inspect();Check(status.ready && status.logo && status.frames>0);
    D3D11_VIEWPORT restored{};UINT n=1;gpu->RSGetViewports(&n,&restored);Check(n==1 && !std::memcmp(&restored,&viewport,sizeof(viewport)));
    Capture(dev.Get(),gpu.Get(),chain.Get(),argc>1?argv[1]:nullptr,true);
    Check(!IsWindowVisible(window)); // No desktop UI was shown by the test.
    Check(SUCCEEDED(chain->ResizeBuffers(0,1280,720,DXGI_FORMAT_UNKNOWN,0)));
    SetWindowPos(window,nullptr,0,0,1280,720,SWP_NOACTIVATE|SWP_NOZORDER);
    overlay_ui::Notify(L"Биржа грузов\nДобавлено заказов: 12 480. Доступно запусков: 2 из 3.");
    for(int i=0;i<3;++i)overlay_ui::RenderFrame(chain.Get(),true,false);
    Capture(dev.Get(),gpu.Get(),chain.Get(),argc>2?argv[2]:nullptr);
    Check(SUCCEEDED(chain->ResizeBuffers(0,1024,768,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,0)));
    SetWindowPos(window,nullptr,0,0,1024,768,SWP_NOACTIVATE|SWP_NOZORDER);
    for(int i=0;i<3;++i)overlay_ui::RenderFrame(chain.Get(),true,true);
    Capture(dev.Get(),gpu.Get(),chain.Get(),nullptr,true); // Exact same palette on an sRGB backbuffer.
    overlay_ui::Shutdown();Check(GetWindowLongPtrW(window,GWLP_WNDPROC)==original_proc);
    Check(overlay_ui::Initialize(std::filesystem::temp_directory_path(),Log,true));
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
    chain.Reset();gpu.Reset();dev.Reset();DestroyWindow(window);UnregisterClassW(cls.lpszClassName,cls.hInstance);branding::Shutdown();
    std::puts("DX11 pixels, branded logo, viewport preservation, resize, window procedure restore, Present/DirectInput hooks and exact UNORM/sRGB palette passed.");
}
