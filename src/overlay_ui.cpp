#include "overlay_ui.h"
#include "notifications.h"
#include "branding.h"
#include "hotkey_binding.h"
#include "notification_lifetime.h"
#include "menu_pointer.h"
#include "brand_palette.h"
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <atomic>
#include <mutex>
#include <array>
#include <vector>
#include <algorithm>
#include <cstdio>
using Microsoft::WRL::ComPtr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace overlay_ui {
namespace {
std::recursive_mutex mutex;
std::atomic<bool> stopping{true}, driving{false};
std::atomic<int> requested{0};
std::atomic<unsigned> callbacks{0};
Logger logger = nullptr;
std::filesystem::path ini;
ImGuiContext* context = nullptr;
ImFont* font = nullptr;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> gpu;
ComPtr<ID3D11ShaderResourceView> logo;
HWND game = nullptr;
WNDPROC previous_proc = nullptr;
void* present_site = nullptr;
using Present = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
Present previous_present = nullptr;
bool hook_owned = false, minhook_owned = false, menu = false, menu_was_down = false;
bool cargo_down = false, freight_down = false, release_actions = true;
MenuPointer pointer;
enum class MouseSource { None, State, Buffered };
MouseSource mouse_source = MouseSource::None;
void* mouse_state_site = nullptr;
void* mouse_data_site = nullptr;
bool mouse_state_hook = false, mouse_data_hook = false;
using MouseState = HRESULT (STDMETHODCALLTYPE*)(void*,DWORD,LPVOID);
using MouseData = HRESULT (STDMETHODCALLTYPE*)(void*,DWORD,LPDIDEVICEOBJECTDATA,LPDWORD,DWORD);
MouseState previous_mouse_state = nullptr;
MouseData previous_mouse_data = nullptr;
ULONGLONG last_frame_time = 0;
WORD cargo=0, freight=0, draft_cargo=0, draft_freight=0;
int capture = 0;
std::array<bool,256> keys{};
std::string notice, error;
NotificationLifetime lifetime;
std::uint64_t rendered = 0;
std::atomic<std::uint64_t> present_calls{0}, input_calls{0};
const ImVec4 accent{brand_palette::Red/255.f,brand_palette::Green/255.f,brand_palette::Blue/255.f,1};
constexpr ImU32 white=IM_COL32(255,255,255,255), muted=IM_COL32(200,200,200,255), green=IM_COL32(brand_palette::Red,brand_palette::Green,brand_palette::Blue,255);
void Log(const char* s) { if (logger) logger(s); }
std::string Utf8(const std::wstring& s) {
    int n=WideCharToMultiByte(CP_UTF8,0,s.c_str(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    std::string out(n,0); if(n) WideCharToMultiByte(CP_UTF8,0,s.c_str(),static_cast<int>(s.size()),out.data(),n,nullptr,nullptr); return out;
}
const char* Tr(const char* en,const char* ru) { return notifications::Russian()?ru:en; }
bool Down(int k) { return (GetAsyncKeyState(k)&0x8000)!=0; }
bool Held(WORD binding) { return !Down(VK_LWIN)&&!Down(VK_RWIN)&&hotkeys::Matches(binding,(binding&255)&&Down(binding&255),Down(VK_CONTROL),Down(VK_SHIFT),Down(VK_MENU)); }
struct ContextScope {
    ImGuiContext* previous;
    ContextScope():previous(ImGui::GetCurrentContext()) { ImGui::SetCurrentContext(context); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};
void CloseMenu() {
    menu=false; capture=0; release_actions=true;
    if(context) { ContextScope scope;ImGui::GetIO().ClearInputMouse(); }
}
void OpenMenu() {
    menu=true; capture=0; error.clear(); draft_cargo=cargo; draft_freight=freight;
    RECT bounds{};GetClientRect(game,&bounds);pointer.Begin(float(bounds.right),float(bounds.bottom));mouse_source=MouseSource::None;
    ImGui::GetIO().ClearInputMouse();
    Log("Menu opened: relative DirectInput cursor enabled; system cursor position is ignored.");
}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if(context && !stopping) {
        ContextScope scope;
        const bool mouse_message=(msg>=WM_MOUSEFIRST && msg<=WM_MOUSELAST) || msg==WM_MOUSELEAVE || msg==WM_NCMOUSEMOVE || msg==WM_NCMOUSELEAVE;
        if(!(menu && mouse_message)) ImGui_ImplWin32_WndProcHandler(hwnd,msg,w,l);
        if(menu) {
            if((msg>=WM_MOUSEFIRST && msg<=WM_MOUSELAST) || (msg>=WM_KEYFIRST && msg<=WM_KEYLAST) || msg==WM_CHAR) return 0;
        }
        if(msg==WM_KILLFOCUS) CloseMenu();
    }
    return previous_proc ? CallWindowProcW(previous_proc,hwnd,msg,w,l) : DefWindowProcW(hwnd,msg,w,l);
}
void ReleaseRenderer() {
    CloseMenu();
    if(game && previous_proc && reinterpret_cast<WNDPROC>(GetWindowLongPtrW(game,GWLP_WNDPROC))==WindowProc)
        SetWindowLongPtrW(game,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(previous_proc));
    previous_proc=nullptr; game=nullptr;
    if(context) {
        auto* old=ImGui::GetCurrentContext(); ImGui::SetCurrentContext(context);
        if(ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX11_Shutdown();
        if(ImGui::GetIO().BackendPlatformUserData) ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(context); if(old!=context) ImGui::SetCurrentContext(old); context=nullptr;
    }
    logo.Reset(); gpu.Reset(); device.Reset(); font=nullptr;
}
bool LoadLogo() {
    constexpr int w=192,h=141;
    HDC dc=CreateCompatibleDC(nullptr); BITMAPINFO info{}; info.bmiHeader={sizeof(BITMAPINFOHEADER),w,-h,1,32,BI_RGB};
    void* pixels=nullptr; HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if(!dc || !bitmap) { if(bitmap) DeleteObject(bitmap); if(dc) DeleteDC(dc); return false; }
    auto old=SelectObject(dc,bitmap); RECT r{0,0,w,h}; FillRect(dc,&r,reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    branding::PaintLogo(dc,r); GdiFlush();
    std::vector<unsigned char> rgba(w*h*4); const auto* src=static_cast<unsigned char*>(pixels);
    for(int i=0;i<w*h;++i) { rgba[i*4]=src[i*4+2]; rgba[i*4+1]=src[i*4+1]; rgba[i*4+2]=src[i*4]; rgba[i*4+3]=255; }
    SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc);
    D3D11_TEXTURE2D_DESC desc{}; desc.Width=w;desc.Height=h;desc.MipLevels=1;desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{rgba.data(),w*4,0}; ComPtr<ID3D11Texture2D> texture;
    return SUCCEEDED(device->CreateTexture2D(&desc,&data,&texture)) && SUCCEEDED(device->CreateShaderResourceView(texture.Get(),nullptr,&logo));
}
bool StartRenderer(IDXGISwapChain* chain,const DXGI_SWAP_CHAIN_DESC& desc) {
    if(FAILED(chain->GetDevice(IID_PPV_ARGS(&device)))) return false;
    device->GetImmediateContext(&gpu); if(!gpu) { device.Reset(); return false; }
    auto* saved_context=ImGui::GetCurrentContext();
    context=ImGui::CreateContext();
    ImGui::SetCurrentContext(saved_context);
    {
        ContextScope scope;
        auto& io=ImGui::GetIO();io.IniFilename=nullptr; io.LogFilename=nullptr;
        io.ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;
        wchar_t windows[MAX_PATH]{};GetWindowsDirectoryW(windows,MAX_PATH);
        auto font_path=Utf8(std::wstring(windows)+L"\\Fonts\\segoeui.ttf");
        font=io.Fonts->AddFontFromFileTTF(font_path.c_str(),36.f,nullptr,io.Fonts->GetGlyphRangesCyrillic());
        if(!font) font=io.Fonts->AddFontDefault();
        if(!ImGui_ImplWin32_Init(desc.OutputWindow) || !ImGui_ImplDX11_Init(device.Get(),gpu.Get())) { ReleaseRenderer(); return false; }
        ImGui::StyleColorsDark();auto& style=ImGui::GetStyle();style.WindowRounding=14;style.FrameRounding=8;style.WindowBorderSize=1;
        style.Colors[ImGuiCol_WindowBg]={0,0,0,1};style.Colors[ImGuiCol_Border]={.24f,.24f,.24f,1};
        style.Colors[ImGuiCol_Button]={.09f,.09f,.09f,1};style.Colors[ImGuiCol_ButtonHovered]={.19f,.19f,.19f,1};
        style.Colors[ImGuiCol_ButtonActive]={.27f,.27f,.27f,1};style.Colors[ImGuiCol_Text]={1,1,1,1};
        style.Colors[ImGuiCol_NavCursor]=accent;style.Colors[ImGuiCol_TextSelectedBg]={accent.x,accent.y,accent.z,.4f};
        game=desc.OutputWindow;
        SetLastError(0);previous_proc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(game,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(WindowProc)));
        if(!previous_proc) { ReleaseRenderer();return false; }
        LoadLogo();
    }
    Log("DirectX 11 in-game interface attached; no desktop overlay windows."); return true;
}
std::string BindingName(WORD binding) {
    if(!(binding&255)) return Tr("Not assigned","Не назначено");
    std::string out;
    if(binding&0x200)out+="Ctrl + ";if(binding&0x100)out+="Shift + ";if(binding&0x400)out+="Alt + ";
    wchar_t name[100]{};UINT scan=MapVirtualKeyW(binding&255,MAPVK_VK_TO_VSC_EX);
    LONG flags=static_cast<LONG>((scan&255)<<16);if(scan&0xff00)flags|=1<<24;
    if(GetKeyNameTextW(flags,name,100))out+=Utf8(name);else out+=std::to_string(binding&255);return out;
}
void Poll(bool focused) {
    const bool m=Held(hotkeys::Menu),a=Held(cargo),b=Held(freight);
    if(focused) {
        if(m&&!menu_was_down) { if(menu)CloseMenu();else OpenMenu(); }
        else if(menu && Down(VK_ESCAPE) && !keys[VK_ESCAPE]) { if(capture)capture=0;else CloseMenu(); }
        else if(menu && capture) {
            for(unsigned key=8;key<255;++key) if(Down(key)&&!keys[key]&&hotkeys::Valid(key)&&key!=VK_ESCAPE) {
                const WORD value=static_cast<WORD>(key|(Down(VK_CONTROL)?0x200:0)|(Down(VK_SHIFT)?0x100:0)|(Down(VK_MENU)?0x400:0));
                if(value==hotkeys::Menu) { error=Tr("This shortcut opens the menu.","Это сочетание открывает меню.");break; }
                (capture==1?draft_cargo:draft_freight)=value;capture=0;error.clear();break;
            }
        } else if(!menu && driving) {
            if(release_actions) { if(!a&&!b)release_actions=false; }
            else if(a&&!cargo_down)requested=1;else if(b&&!freight_down)requested=2;
        }
    } else if(menu) CloseMenu();
    menu_was_down=m;cargo_down=a;freight_down=b;
    for(unsigned k=0;k<256;++k)keys[k]=Down(k);
}
void DrawMenu() {
    auto& io=ImGui::GetIO();const float scale=std::min(1.f,std::min((io.DisplaySize.x-24)/900,(io.DisplaySize.y-24)/660));
    if(scale<=0)return;
    ImGui::SetNextWindowSize({900*scale,660*scale});
    ImGui::SetNextWindowPos({io.DisplaySize.x/2,io.DisplaySize.y/2},ImGuiCond_Appearing,{.5f,.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
    ImGui::Begin("Cargo Control ETS2",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar);
    const auto origin=ImGui::GetWindowPos(); auto* draw=ImGui::GetWindowDrawList();
    auto pos=[&](float x,float y){return ImVec2(origin.x+x*scale,origin.y+y*scale);};
    auto text=[&](float x,float y,float size,ImU32 color,const char* value){draw->AddText(font,size*scale,pos(x,y),color,value,nullptr,0);};
    auto card=[&](float x,float y,float w,float h){draw->AddRectFilled(pos(x,y),pos(x+w,y+h),IM_COL32(14,14,14,255),14*scale);draw->AddRect(pos(x,y),pos(x+w,y+h),IM_COL32(65,65,65,255),14*scale);};
    auto button=[&](const char* label,float x,float y,float w,float h,bool primary=false){
        ImGui::SetCursorScreenPos(pos(x,y));ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{8*scale,8*scale});
        ImGui::SetWindowFontScale(scale*.5f);
        if(primary) { ImGui::PushStyleColor(ImGuiCol_Button,accent);ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(accent.x+(1-accent.x)*.15f,accent.y+(1-accent.y)*.15f,accent.z+(1-accent.z)*.15f,1));ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(accent.x*.85f,accent.y*.85f,accent.z*.85f,1));ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0,0,0,1)); }
        bool hit=ImGui::Button(label,{w*scale,h*scale});if(primary)ImGui::PopStyleColor(4);ImGui::PopStyleVar();return hit;
    };
    text(32,25,14,green,"QUPER SIMULATOR");text(30,54,34,white,"Cargo Control ETS2");
    text(32,108,17,muted,Tr("Your cargo market. Your rules.","Ваш рынок грузов. Ваши правила."));
    if(logo)draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(logo.Get())),pos(710,23),pos(842,120));
    if(button("X##top",850,18,30,30))CloseMenu();
    card(28,156,410,204);card(462,156,410,204);
    text(48,176,23,white,Tr("Owned-trailer cargo","Грузы для прицепа"));
    text(48,220,16,muted,Tr("Add up to 5 offers per company.\nKeep your existing orders.","Добавить до 5 грузов в каждой компании.\nСуществующие заказы сохраняются."));
    text(482,176,23,white,Tr("Freight & Quick jobs","Рынок и агентства"));
    text(482,220,16,muted,Tr("Refresh offers across all companies.\nExisting orders will be replaced.","Обновить предложения во всех компаниях.\nСуществующие заказы будут заменены."));
    if(button(Tr("Add cargo","Добавить грузы"),48,298,370,42,true)) { requested=1;CloseMenu(); }
    if(button(Tr("Refresh orders","Обновить заказы"),482,298,370,42,true)) { requested=2;CloseMenu(); }
    text(32,385,21,white,Tr("Keyboard shortcuts","Горячие клавиши"));
    text(48,428,15,muted,Tr("Add cargo","Добавление грузов"));text(482,428,15,muted,Tr("Refresh orders","Обновление заказов"));
    const auto a=(capture==1?std::string(Tr("Press a shortcut…","Нажмите сочетание…")):BindingName(draft_cargo))+"##cargo";
    const auto b=(capture==2?std::string(Tr("Press a shortcut…","Нажмите сочетание…")):BindingName(draft_freight))+"##freight";
    if(button(a.c_str(),48,458,292,38))capture=1;
    if(button(b.c_str(),482,458,292,38))capture=2;
    if(button(Tr("Clear##cargo","Сброс##cargo"),350,458,68,38)){draft_cargo=0;capture=0;}
    if(button(Tr("Clear##freight","Сброс##freight"),784,458,68,38)){draft_freight=0;capture=0;}
    text(32,518,15,muted,Tr("Select a field and press a shortcut. Clear disables the binding.","Выберите поле и нажмите сочетание. «Сброс» отключает назначение."));
    text(32,545,15,muted,Tr("Stop your truck first. Menu: Ctrl + Alt + F9.","Перед генерацией остановите грузовик. Меню: Ctrl + Alt + F9."));
    text(32,599,14,error.empty()?muted:white,error.empty()?Tr("Save to apply your shortcuts.","Изменения применяются после сохранения."):error.c_str());
    if(button(Tr("Save","Сохранить"),576,588,142,44,true)) {
        if(!hotkeys::PairAllowed(draft_cargo,draft_freight))error=Tr("Choose different shortcuts.","Назначьте разные сочетания.");
        else { wchar_t values[160]{};swprintf_s(values,L"Cargo=%u%cFreight=%u%c",unsigned(draft_cargo),0,unsigned(draft_freight),0);
            if(!WritePrivateProfileSectionW(L"Bindings",values,ini.c_str()))error=Tr("Could not save settings.","Не удалось сохранить настройки.");
            else {cargo=draft_cargo;freight=draft_freight;error.clear();CloseMenu();}
        }
    }
    if(button(Tr("Close","Закрыть"),734,588,138,44))CloseMenu();
    ImGui::End();ImGui::PopStyleVar();
}
void DrawNotice() {
    auto& io=ImGui::GetIO();const float w=std::min(800.f,io.DisplaySize.x-24);
    const float logo_width=132;const float body_width=std::max(120.f,w-logo_width-64);
    ImVec2 size=font->CalcTextSizeA(20,body_width,body_width,notice.c_str());
    const float h=std::max(164.f,size.y+76);
    ImGui::SetNextWindowPos({io.DisplaySize.x/2,40},ImGuiCond_Always,{.5f,0});ImGui::SetNextWindowSize({w,h});
    ImGui::Begin("##cargo-result",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoScrollbar);
    const auto p=ImGui::GetWindowPos();auto* draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(p,{p.x+4,p.y+h},green);
    draw->AddText(font,16,{p.x+24,p.y+16},green,"CARGO CONTROL ETS2");
    draw->AddText(font,20,{p.x+24,p.y+50},white,notice.c_str(),nullptr,body_width);
    if(logo)draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(logo.Get())),{p.x+w-152,p.y+h-116},{p.x+w-20,p.y+h-19});
    ImGui::End();
}
bool IsMouse(void* instance) {
    DIDEVICEINSTANCEW wide{};wide.dwSize=sizeof(wide);
    auto* mouse=static_cast<IDirectInputDevice8W*>(instance);
    if(SUCCEEDED(mouse->GetDeviceInfo(&wide)))return IsEqualGUID(wide.guidInstance,GUID_SysMouse) || GET_DIDEVICE_TYPE(wide.dwDevType)==DI8DEVTYPE_MOUSE;
    DIDEVICEINSTANCEA ansi{};ansi.dwSize=sizeof(ansi);
    return SUCCEEDED(static_cast<IDirectInputDevice8A*>(instance)->GetDeviceInfo(&ansi)) && GET_DIDEVICE_TYPE(ansi.dwDevType)==DI8DEVTYPE_MOUSE;
}
void FeedButtons(const std::array<bool,5>& before) {
    auto& io=ImGui::GetIO();io.AddMousePosEvent(pointer.x,pointer.y);
    for(unsigned i=0;i<before.size();++i)if(before[i]!=pointer.buttons[i])io.AddMouseButtonEvent(i,pointer.buttons[i]);
    if(pointer.wheel) {io.AddMouseWheelEvent(0,pointer.wheel);pointer.wheel=0;}
}
HRESULT STDMETHODCALLTYPE MouseStateBridge(void* instance,DWORD size,LPVOID bytes) {
    ++callbacks;++input_calls;const HRESULT result=previous_mouse_state(instance,size,bytes);
    if(!stopping && SUCCEEDED(result) && bytes && (size==sizeof(DIMOUSESTATE)||size==sizeof(DIMOUSESTATE2))) {
        std::lock_guard<std::recursive_mutex> guard(mutex);
        if(context && menu && GetForegroundWindow()==game && IsMouse(instance)) {
            ContextScope scope;
            if(mouse_source==MouseSource::None){mouse_source=MouseSource::State;Log("Menu mouse input: DirectInput state.");}
            const auto before=pointer.buttons;pointer.State(bytes,size,mouse_source==MouseSource::State);FeedButtons(before);
        }
    }
    --callbacks;return result;
}
HRESULT STDMETHODCALLTYPE MouseDataBridge(void* instance,DWORD stride,LPDIDEVICEOBJECTDATA bytes,LPDWORD count,DWORD flags) {
    ++callbacks;++input_calls;const DWORD capacity=count?*count:0;
    const HRESULT result=previous_mouse_data(instance,stride,bytes,count,flags);
    if(!stopping && SUCCEEDED(result) && bytes && count && *count<=capacity && *count<=65536 && (stride==sizeof(DIDEVICEOBJECTDATA)||stride==16)) {
        std::lock_guard<std::recursive_mutex> guard(mutex);
        if(context && menu && GetForegroundWindow()==game && IsMouse(instance)) {
            ContextScope scope;
            if(mouse_source==MouseSource::None && *count && !(flags&DIGDD_PEEK)){mouse_source=MouseSource::Buffered;Log("Menu mouse input: buffered DirectInput.");}
            for(DWORD i=0;i<*count;++i) {
                auto* record=reinterpret_cast<unsigned char*>(bytes)+i*stride;
                DIDEVICEOBJECTDATA event{};std::memcpy(&event,record,stride);
                const auto before=pointer.buttons;pointer.Event(event,mouse_source==MouseSource::Buffered && !(flags&DIGDD_PEEK));FeedButtons(before);
                std::memcpy(record,&event,stride);
            }
        }
    }
    --callbacks;return result;
}
bool InstallMouseHooks() {
    ComPtr<IDirectInput8W> input;ComPtr<IDirectInputDevice8W> mouse;
    if(FAILED(DirectInput8Create(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,IID_IDirectInput8W,reinterpret_cast<void**>(input.GetAddressOf()),nullptr)) ||
       FAILED(input->CreateDevice(GUID_SysMouse,&mouse,nullptr)))return false;
    auto** table=*reinterpret_cast<void***>(mouse.Get());mouse_state_site=table[9];mouse_data_site=table[10];
    if(MH_CreateHook(mouse_state_site,reinterpret_cast<void*>(MouseStateBridge),reinterpret_cast<void**>(&previous_mouse_state))!=MH_OK)return false;
    mouse_state_hook=true;
    if(MH_CreateHook(mouse_data_site,reinterpret_cast<void*>(MouseDataBridge),reinterpret_cast<void**>(&previous_mouse_data))!=MH_OK)return false;
    mouse_data_hook=true;
    return MH_EnableHook(mouse_state_site)==MH_OK && MH_EnableHook(mouse_data_site)==MH_OK;
}
void DisableMouseHooks() {
    if(mouse_state_hook)MH_DisableHook(mouse_state_site);if(mouse_data_hook)MH_DisableHook(mouse_data_site);
}
void RemoveMouseHooks() {
    if(mouse_state_hook)MH_RemoveHook(mouse_state_site);if(mouse_data_hook)MH_RemoveHook(mouse_data_site);
    mouse_state_hook=mouse_data_hook=false;mouse_state_site=mouse_data_site=nullptr;
}
HRESULT STDMETHODCALLTYPE PresentBridge(IDXGISwapChain* chain,UINT sync,UINT flags) {
    ++callbacks; ++present_calls;
    if(!stopping && !(flags&DXGI_PRESENT_TEST)) RenderFrame(chain);
    const HRESULT result=previous_present(chain,sync,flags);
    if(result==DXGI_ERROR_DEVICE_REMOVED || result==DXGI_ERROR_DEVICE_RESET) {
        std::lock_guard<std::recursive_mutex> guard(mutex);ReleaseRenderer();
    }
    --callbacks;return result;
}
}
void RenderFrame(IDXGISwapChain* chain,bool allow_hidden,bool preview_menu) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if(stopping || !chain)return;
    DXGI_SWAP_CHAIN_DESC desc{};if(FAILED(chain->GetDesc(&desc)))return;
    DWORD pid=0;GetWindowThreadProcessId(desc.OutputWindow,&pid);
    if(pid!=GetCurrentProcessId() || (!allow_hidden && (!IsWindowVisible(desc.OutputWindow) || IsIconic(desc.OutputWindow))))return;
    RECT rect{};GetClientRect(desc.OutputWindow,&rect);if(rect.right<320 || rect.bottom<200)return;
    if(context && game!=desc.OutputWindow)return; // Ignore secondary swap chains.
    if(!context && !StartRenderer(chain,desc))return;
    ContextScope scope;
    const bool focused=GetForegroundWindow()==game || allow_hidden;
    if(!allow_hidden) Poll(focused);
    ImGui_ImplDX11_NewFrame();auto& io=ImGui::GetIO();
    if(menu&&focused) {
        pointer.Resize(float(rect.right),float(rect.bottom));io.DisplaySize={float(rect.right),float(rect.bottom)};
        io.AddMousePosEvent(pointer.x,pointer.y); // Never poll the game's recentered OS cursor.
    } else ImGui_ImplWin32_NewFrame();
    const auto now_tick=GetTickCount64();io.DeltaTime=last_frame_time?std::clamp(float(now_tick-last_frame_time)/1000.f,.001f,.25f):1.f/60;
    last_frame_time=now_tick;io.MouseDrawCursor=menu&&focused;
    if(allow_hidden) { io.ClearInputKeys();io.ClearInputMouse();io.MousePos={-FLT_MAX,-FLT_MAX}; }
    ImGui::NewFrame();
    const bool visible_notice=focused&&!menu&&!preview_menu&&!notice.empty();
    const bool alive=lifetime.Tick(GetTickCount64(),visible_notice);
    if((menu || preview_menu)&&focused)DrawMenu();else if(alive&&visible_notice)DrawNotice();
    ImGui::Render();
    if(ImGui::GetDrawData()->TotalVtxCount==0)return;
    ComPtr<ID3D11Texture2D> backbuffer;ComPtr<ID3D11RenderTargetView> target;
    if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&backbuffer))))return;
    D3D11_TEXTURE2D_DESC back_desc{};backbuffer->GetDesc(&back_desc);
    D3D11_RENDER_TARGET_VIEW_DESC view{};view.ViewDimension=back_desc.SampleDesc.Count>1?D3D11_RTV_DIMENSION_TEXTURE2DMS:D3D11_RTV_DIMENSION_TEXTURE2D;
    view.Format=back_desc.Format;
    if(view.Format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    if(view.Format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)view.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    if(FAILED(device->CreateRenderTargetView(backbuffer.Get(),&view,&target)))return;
    ID3D11RenderTargetView* previous[8]{};ComPtr<ID3D11DepthStencilView> depth;
    gpu->OMGetRenderTargets(8,previous,depth.GetAddressOf());
    auto* rtv=target.Get();gpu->OMSetRenderTargets(1,&rtv,nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    gpu->OMSetRenderTargets(8,previous,depth.Get());for(auto* view:previous)if(view)view->Release();
    ++rendered;
}
bool Initialize(const std::filesystem::path& directory,Logger log,bool install_hook) {
    std::lock_guard<std::recursive_mutex> guard(mutex);logger=log;ini=directory/L"CargoControlETS2.ini";
    cargo=hotkeys::Valid(GetPrivateProfileIntW(L"Bindings",L"Cargo",0,ini.c_str()));
    freight=hotkeys::Valid(GetPrivateProfileIntW(L"Bindings",L"Freight",0,ini.c_str()));
    if(cargo==hotkeys::Menu)cargo=0;if(!hotkeys::PairAllowed(cargo,freight))freight=0;
    stopping=false;requested=0;menu=false;capture=0;rendered=0;present_calls=0;input_calls=0;
    if(!install_hook)return true;
    WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"QSCargoDxProbe";
    RegisterClassW(&cls);HWND probe=CreateWindowExW(0,cls.lpszClassName,L"",WS_POPUP,0,0,8,8,nullptr,nullptr,cls.hInstance,nullptr);
    DXGI_SWAP_CHAIN_DESC desc{};desc.BufferCount=1;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.Width=8;desc.BufferDesc.Height=8;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow=probe;desc.SampleDesc.Count=1;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    ComPtr<IDXGISwapChain> chain;ComPtr<ID3D11Device> temp_device;ComPtr<ID3D11DeviceContext> temp_context;
    HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&chain,&temp_device,nullptr,&temp_context);
    if(FAILED(hr))hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&desc,&chain,&temp_device,nullptr,&temp_context);
    bool ok=false;
    if(SUCCEEDED(hr)) {
        present_site=(*reinterpret_cast<void***>(chain.Get()))[8];
        const auto init=MH_Initialize();minhook_owned=init==MH_OK;
        if(init==MH_OK || init==MH_ERROR_ALREADY_INITIALIZED) {
            if(MH_CreateHook(present_site,reinterpret_cast<void*>(PresentBridge),reinterpret_cast<void**>(&previous_present))==MH_OK) {
                hook_owned=true;ok=InstallMouseHooks() && MH_EnableHook(present_site)==MH_OK;
            }
        }
    }
    chain.Reset();temp_context.Reset();temp_device.Reset();if(probe)DestroyWindow(probe);UnregisterClassW(cls.lpszClassName,cls.hInstance);
    if(!ok) {
        DisableMouseHooks();RemoveMouseHooks();if(hook_owned)MH_RemoveHook(present_site);if(minhook_owned)MH_Uninitialize();hook_owned=minhook_owned=false;
        stopping=true;Log("DirectX 11 interface failed to initialize. No external-window fallback enabled.");
    } else Log("DirectX 11 Present hook enabled; awaiting the game swap chain.");
    return ok;
}
void Notify(const std::wstring& text) { std::lock_guard<std::recursive_mutex> guard(mutex);notice=Utf8(text);lifetime.Start(); }
int Tick(bool active) { driving=active;return active?requested.exchange(0):0; }
void CancelPending() { requested=0; }
Diagnostics Inspect() { std::lock_guard<std::recursive_mutex> guard(mutex);return {rendered,present_calls.load(),input_calls.load(),logo!=nullptr,context!=nullptr}; }
void Shutdown() {
    stopping=true;
    DisableMouseHooks();
    if(hook_owned)MH_DisableHook(present_site);
    // Disabling prevents new entries. Let already-running Present calls finish before freeing their trampoline.
    while(callbacks.load())Sleep(1);
    std::lock_guard<std::recursive_mutex> guard(mutex);ReleaseRenderer();
    RemoveMouseHooks();if(hook_owned)MH_RemoveHook(present_site);if(minhook_owned)MH_Uninitialize();hook_owned=minhook_owned=false;
    requested=0;driving=false;notice.clear();keys.fill(false);menu_was_down=cargo_down=freight_down=false;
    previous_present=nullptr;present_site=nullptr;logger=nullptr;
}
}
