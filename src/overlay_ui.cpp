#include "overlay_ui.h"
#include "settings_file.h"
#include "notifications.h"
#include "branding.h"
#include "hotkey_binding.h"
#include "notification_lifetime.h"
#include "operation_feedback.h"
#include "menu_pointer.h"
#include "brand_palette.h"
#include "plugin_version.h"
#ifdef CARGO_CONTROL_ROAD_TRIP
#include "road_trip.h"
#include "coaches.h"
#endif
#ifdef CARGO_CONTROL_CUSTOM_JOBS
#include "custom_jobs_ui.h"
#endif
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
#include <unordered_map>
using Microsoft::WRL::ComPtr;
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace overlay_ui {
namespace {
std::recursive_mutex mutex;
std::atomic<bool> stopping{true};
OperationFeedback operation;
bool busy_drawn=false,reopen_catalog=false;
IDXGISwapChain* busy_chain=nullptr;
int menu_page=0,reopen_page=1;
std::uint64_t progress_done=0,progress_total=0;
std::atomic<unsigned> callbacks{0};
Logger logger = nullptr;
std::filesystem::path ini;
ImGuiContext* context = nullptr;
ImFont* font = nullptr;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> gpu;
ComPtr<ID3D11ShaderResourceView> logo;
HWND game = nullptr;
// A later plugin can keep our procedure in its chain even after renderer shutdown.
// Preserve each window's forwarding target independently of renderer lifetime.
std::unordered_map<HWND, WNDPROC> window_procedures;
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
WORD menu_binding=hotkeys::Menu,draft_menu=hotkeys::Menu;
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
#ifdef CARGO_CONTROL_CUSTOM_JOBS
    custom_jobs::RefreshTrailerCargos();
#endif
    menu=true; capture=0; error.clear(); draft_cargo=cargo; draft_freight=freight;draft_menu=menu_binding;
    RECT bounds{};GetClientRect(game,&bounds);pointer.Begin(float(bounds.right),float(bounds.bottom));mouse_source=MouseSource::None;
    ImGui::GetIO().ClearInputMouse();
    Log("Menu opened: relative DirectInput cursor enabled; system cursor position is ignored.");
}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    WNDPROC previous = nullptr;
    {
        std::lock_guard<std::recursive_mutex> guard(mutex);
        const auto found = window_procedures.find(hwnd);
        if(found != window_procedures.end()) previous = found->second;
        if(context && !stopping && hwnd == game) {
            ContextScope scope;
            const bool mouse_message=(msg>=WM_MOUSEFIRST && msg<=WM_MOUSELAST) || msg==WM_MOUSELEAVE || msg==WM_NCMOUSEMOVE || msg==WM_NCMOUSELEAVE;
            if(!(menu && mouse_message)) ImGui_ImplWin32_WndProcHandler(hwnd,msg,w,l);
            if(menu) {
                if((msg>=WM_MOUSEFIRST && msg<=WM_MOUSELAST) || (msg>=WM_KEYFIRST && msg<=WM_KEYLAST) || msg==WM_CHAR) return 0;
            }
            if(msg==WM_KILLFOCUS) CloseMenu();
        }
        if(msg == WM_NCDESTROY) window_procedures.erase(hwnd);
    }
    // Never hold our renderer lock while calling another plugin's procedure.
    return previous ? CallWindowProcW(previous,hwnd,msg,w,l) : DefWindowProcW(hwnd,msg,w,l);
}
void ReleaseRenderer() {
    CloseMenu();
    const auto found = window_procedures.find(game);
    if(found != window_procedures.end() && reinterpret_cast<WNDPROC>(GetWindowLongPtrW(game,GWLP_WNDPROC))==WindowProc) {
        if(SetWindowLongPtrW(game,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(found->second)))
            window_procedures.erase(found);
    }
    game=nullptr;
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
        if(window_procedures.find(game) == window_procedures.end()) {
            // Raw WndProc chains have no API for removing a procedure buried under
            // another plugin. Keep our code mapped until process exit so any saved
            // callback remains callable, including after telemetry shutdown.
            HMODULE pinned_module = nullptr;
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&WindowProc), &pinned_module)) {
                Log("Cannot retain window callback module; interface initialization stopped.");
                ReleaseRenderer();return false;
            }
            window_procedures.emplace(game, nullptr);
            const auto previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(game,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(WindowProc)));
            if(!previous) { window_procedures.erase(game);ReleaseRenderer();return false; }
            window_procedures[game] = previous;
        }
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
    const bool m=Held(menu_binding),a=Held(cargo),b=Held(freight);
    if(operation.Active()){
        menu_was_down=m;cargo_down=a;freight_down=b;
        for(unsigned k=0;k<256;++k)keys[k]=Down(k);
        return;
    }
    if(focused) {
        if(m&&!menu_was_down&&!(menu&&capture)) { if(menu)CloseMenu();else OpenMenu(); }
        else if(menu && Down(VK_ESCAPE) && !keys[VK_ESCAPE]) { if(capture)capture=0;else if(!ImGui::GetIO().WantTextInput&&!ImGui::IsPopupOpen("",ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel))CloseMenu(); }
        else if(menu && capture) {
            for(unsigned key=8;key<255;++key) if(Down(key)&&!keys[key]&&hotkeys::Valid(key)&&key!=VK_ESCAPE) {
                const WORD value=static_cast<WORD>(key|(Down(VK_CONTROL)?0x200:0)|(Down(VK_SHIFT)?0x100:0)|(Down(VK_MENU)?0x400:0));
                if(capture!=3&&value==draft_menu) { error=Tr("This shortcut opens the menu.","Это сочетание открывает меню.");break; }
                (capture==3?draft_menu:capture==1?draft_cargo:draft_freight)=value;capture=0;error.clear();break;
            }
        } else if(!menu) {
            if(release_actions) { if(!a&&!b)release_actions=false; }
            else if(a&&!cargo_down)Request(1);else if(b&&!freight_down)Request(2);
        }
    } else if(menu) CloseMenu();
    menu_was_down=m;cargo_down=a;freight_down=b;
    for(unsigned k=0;k<256;++k)keys[k]=Down(k);
}
void DrawMenu(int preview_page = -1) {
    auto& io=ImGui::GetIO();const float scale=std::min(1.f,std::min((io.DisplaySize.x-24)/900,(io.DisplaySize.y-24)/660));
    if(scale<=0)return;
    static ImVec2 last_display{};
    const bool resized=last_display.x!=io.DisplaySize.x||last_display.y!=io.DisplaySize.y;
    last_display=io.DisplaySize;
    ImGui::SetNextWindowSize({900*scale,660*scale});
    ImGui::SetNextWindowPos({io.DisplaySize.x/2,io.DisplaySize.y/2},resized?ImGuiCond_Always:ImGuiCond_Appearing,{.5f,.5f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
    ImGui::Begin("Cargo Control",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar);
    const auto origin=ImGui::GetWindowPos(); auto* draw=ImGui::GetWindowDrawList();
    auto pos=[&](float x,float y){return ImVec2(origin.x+x*scale,origin.y+y*scale);};
    auto text=[&](float x,float y,float size,ImU32 color,const char* value){draw->AddText(font,size*scale,pos(x,y),color,value,nullptr,0);};
    auto card=[&](float x,float y,float w,float h){draw->AddRectFilled(pos(x,y),pos(x+w,y+h),IM_COL32(14,14,14,255),14*scale);draw->AddRect(pos(x,y),pos(x+w,y+h),IM_COL32(65,65,65,255),14*scale);};
    auto button=[&](const char* label,float x,float y,float w,float h,bool primary=false){
        ImGui::SetCursorScreenPos(pos(x,y));ImGui::SetWindowFontScale(scale*.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{12*scale,std::max(0.f,(h*scale-ImGui::GetTextLineHeight())*.5f)});
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,{.5f,.5f});
        if(primary) { ImGui::PushStyleColor(ImGuiCol_Button,accent);ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(std::min(1.f,accent.x*1.08f),std::min(1.f,accent.y*1.08f),std::min(1.f,accent.z*1.08f),1));ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(accent.x*.85f,accent.y*.85f,accent.z*.85f,1));ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0,0,0,1)); }
        bool hit=ImGui::Button(label,{w*scale,h*scale});if(primary)ImGui::PopStyleColor(4);ImGui::PopStyleVar(2);return hit;
    };
    text(32,25,14,green,"QUPER SIMULATOR");text(30,54,34,white,"Cargo Control");
    text(32,108,17,muted,Tr("Your cargo market. Your rules.","Ваш рынок грузов. Ваши правила."));
    if(logo)draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(logo.Get())),pos(710,23),pos(842,120));
    if(button("X##top",850,18,30,30))CloseMenu();
#ifdef CARGO_CONTROL_CUSTOM_JOBS
    int& page=menu_page;
    const int visible_page = preview_page >= 0 ? preview_page : page;
    if(button(Tr("Markets", "Рынки"),32,136,164,40,visible_page==0))page=0;
    if(button(Tr("Create job", "Создать заказ"),212,136,208,40,visible_page==1))page=1;
#ifdef CARGO_CONTROL_COACHES
    if(coaches::Visible()&&button("Coaches",436,136,164,40,visible_page==3))page=3;
    if((preview_page>=0?preview_page:page)==3&&coaches::Visible()){
        ImGui::SetWindowFontScale(1.f);ImGui::SetCursorScreenPos(pos(32,194));
        ImGui::BeginChild("coach-catalog",{836*scale,390*scale});
        ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(.08f,.08f,.08f,1));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(.15f,.15f,.15f,1));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,ImVec4(.2f,.2f,.2f,1));
        ImGui::PushStyleColor(ImGuiCol_CheckMark,accent);
        ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(.2f,.3f,.18f,1));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,ImVec4(.25f,.35f,.22f,1));
        coaches::Draw(scale,notifications::Russian());ImGui::PopStyleColor(6);ImGui::EndChild();
        text(32,632,13,muted,"v" CARGO_CONTROL_VERSION_TEXT);
        if(button(Tr("Close","Закрыть"),734,588,138,44))CloseMenu();
        ImGui::End();ImGui::PopStyleVar();return;
    }
#endif
#ifdef CARGO_CONTROL_ROAD_TRIP
    if(road_trip::Visible()&&button("Road Trip",436,136,164,40,visible_page==2))page=2;
    if((preview_page>=0?preview_page:page)==2&&road_trip::Visible()){
        ImGui::SetWindowFontScale(1.f);ImGui::SetCursorScreenPos(pos(32,194));
        ImGui::BeginChild("road-trip-catalog",{836*scale,390*scale});
        ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(.08f,.08f,.08f,1));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(.15f,.15f,.15f,1));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,ImVec4(.2f,.2f,.2f,1));
        ImGui::PushStyleColor(ImGuiCol_CheckMark,accent);
        ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(.2f,.3f,.18f,1));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,ImVec4(.25f,.35f,.22f,1));
        road_trip::Draw(scale,notifications::Russian());ImGui::PopStyleColor(6);ImGui::EndChild();
        text(32,632,13,muted,"v" CARGO_CONTROL_VERSION_TEXT);
        if(button(Tr("Close","Закрыть"),734,588,138,44))CloseMenu();
        ImGui::End();ImGui::PopStyleVar();return;
    }
#endif
    if((preview_page >= 0 ? preview_page : page)==1) {
        ImGui::SetWindowFontScale(1.f); // Child font scaling must not multiply the header button scale.
        ImGui::SetCursorScreenPos(pos(32,194));
        ImGui::BeginChild("session-catalog",{836*scale,390*scale});
        ImGui::PushStyleColor(ImGuiCol_FrameBg,ImVec4(.08f,.08f,.08f,1));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,ImVec4(.15f,.15f,.15f,1));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,ImVec4(.2f,.2f,.2f,1));
        ImGui::PushStyleColor(ImGuiCol_CheckMark,accent);
        ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(.2f,.3f,.18f,1));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,ImVec4(.25f,.35f,.22f,1));
        custom_jobs::DrawCatalog(scale,notifications::Russian(),[]{Request(3);CloseMenu();},[]{Request(4);CloseMenu();});
        ImGui::PopStyleColor(6);
        ImGui::EndChild();
        text(32,632,13,muted,"v" CARGO_CONTROL_VERSION_TEXT);
        if(button(Tr("Close","Закрыть"),734,588,138,44))CloseMenu();
        ImGui::End();ImGui::PopStyleVar();return;
    }
#endif
#ifdef CARGO_CONTROL_CUSTOM_JOBS
    const float cards_y=194,cards_height=190,keyboard_y=410;
#else
    const float cards_y=156,cards_height=204,keyboard_y=385;
#endif
    card(28,cards_y,410,cards_height);card(462,cards_y,410,cards_height);
    text(48,cards_y+20,23,white,Tr("Owned-trailer cargo","Грузы для прицепа"));
    text(48,cards_y+64,16,muted,Tr("Replace current owned-trailer offers.\nUp to 5 new offers per company.","Заменить текущие грузы для прицепа.\nДо 5 новых предложений на компанию."));
    text(482,cards_y+20,23,white,Tr("Freight & Quick jobs","Рынок и агентства"));
    text(482,cards_y+64,16,muted,Tr("Refresh offers across all companies.\nExisting orders will be replaced.","Обновить предложения во всех компаниях.\nСуществующие заказы будут заменены."));
    if(button(Tr("Refresh cargo","Обновить грузы"),48,cards_y+cards_height-62,370,42,true)) { Request(1);CloseMenu(); }
    if(button(Tr("Refresh orders","Обновить заказы"),482,cards_y+cards_height-62,370,42,true)) { Request(2);CloseMenu(); }
    text(32,keyboard_y,21,white,Tr("Keyboard shortcuts","Горячие клавиши"));
    constexpr float columns[]={32,318,604};
    constexpr float field_width=172,control_gap=8,reset_width=88,control_height=38;
    const float label_y=keyboard_y+36,control_y=keyboard_y+60;
    text(columns[0],label_y,15,muted,Tr("Open menu","Открыть меню"));
    text(columns[1],label_y,15,muted,Tr("Refresh cargo","Обновление грузов"));
    text(columns[2],label_y,15,muted,Tr("Refresh orders","Обновление заказов"));
    const auto menu_key=(capture==3?std::string(Tr("Press a shortcut...","Нажмите сочетание...")):BindingName(draft_menu))+"##menu";
    const auto a=(capture==1?std::string(Tr("Press a shortcut...","Нажмите сочетание...")):BindingName(draft_cargo))+"##cargo";
    const auto b=(capture==2?std::string(Tr("Press a shortcut...","Нажмите сочетание...")):BindingName(draft_freight))+"##freight";
    if(button(menu_key.c_str(),columns[0],control_y,field_width,control_height))capture=3;
    if(button(Tr("Default##menu","По умолч.##menu"),columns[0]+field_width+control_gap,control_y,reset_width,control_height)){draft_menu=hotkeys::Menu;capture=0;}
    if(button(a.c_str(),columns[1],control_y,field_width,control_height))capture=1;
    if(button(b.c_str(),columns[2],control_y,field_width,control_height))capture=2;
    if(button(Tr("Clear##cargo","Сброс##cargo"),columns[1]+field_width+control_gap,control_y,reset_width,control_height)){draft_cargo=0;capture=0;}
    if(button(Tr("Clear##freight","Сброс##freight"),columns[2]+field_width+control_gap,control_y,reset_width,control_height)){draft_freight=0;capture=0;}
    text(32,keyboard_y+114,15,muted,Tr("Select a field and press a shortcut. Esc cancels input.","Выберите поле и нажмите сочетание. Esc отменяет ввод."));
    const float warning_y=keyboard_y+138;
    draw->AddRectFilled(pos(32,warning_y),pos(872,warning_y+32),IM_COL32(32,27,17,255),8*scale);
    text(46,warning_y+8,15,IM_COL32(235,190,85,255),Tr("Forgot your shortcut? Close the game and delete CargoControl.ini next to the DLL.","Забыли клавиши? Закройте игру и удалите CargoControl.ini рядом с DLL."));
    button("?##restore-shortcuts",838,warning_y+3,26,26);
    if(ImGui::IsItemHovered()){
        ImGui::BeginTooltip();ImGui::PushTextWrapPos(420*scale);
        ImGui::TextUnformatted(Tr("Close the game and delete CargoControl.ini from bin/win_x64/plugins, next to Cargo Control's DLL. On the next launch, the menu shortcut returns to Ctrl + Alt + F9. All settings in this INI are reset. If the older CargoControlETS2.ini also exists, delete it too to prevent old action shortcuts from being imported.",
            "Закройте игру и удалите CargoControl.ini из bin/win_x64/plugins, рядом с DLL Cargo Control. При следующем запуске меню снова открывается по Ctrl + Alt + F9. Все настройки из этого INI будут сброшены. Если остался старый CargoControlETS2.ini, удалите его тоже, чтобы прежние сочетания действий не загрузились повторно."));
        ImGui::PopTextWrapPos();ImGui::EndTooltip();
    }
    text(32,599,14,error.empty()?muted:white,error.empty()?Tr("Save to apply your shortcuts.","Изменения применяются после сохранения."):error.c_str());
    text(32,632,13,muted,"v" CARGO_CONTROL_VERSION_TEXT);
    if(button(Tr("Save","Сохранить"),576,588,142,44,true)) {
        if(!hotkeys::PairAllowed(draft_cargo,draft_freight,draft_menu))error=Tr("Choose different shortcuts for the menu and actions.","Назначьте разные сочетания для меню и действий.");
        else { wchar_t values[160]{};swprintf_s(values,L"Menu=%u%cCargo=%u%cFreight=%u%c",unsigned(draft_menu),0,unsigned(draft_cargo),0,unsigned(draft_freight),0);
            if(!WritePrivateProfileSectionW(L"Bindings",values,ini.c_str()))error=Tr("Could not save settings.","Не удалось сохранить настройки.");
            else {menu_binding=draft_menu;cargo=draft_cargo;freight=draft_freight;error.clear();CloseMenu();menu_was_down=Held(menu_binding);}
        }
    }
    if(button(Tr("Close","Закрыть"),734,588,138,44))CloseMenu();
    ImGui::End();ImGui::PopStyleVar();
}
void DrawBusy(){
    const int action=operation.Active();if(!action)return;
    auto& io=ImGui::GetIO();const float scale=std::min(1.f,(io.DisplaySize.x-32)/520.f);
    const ImVec2 size{520*scale,176*scale};
    ImGui::GetBackgroundDrawList()->AddRectFilled({0,0},io.DisplaySize,IM_COL32(0,0,0,145));
    ImGui::SetNextWindowPos({io.DisplaySize.x*.5f,io.DisplaySize.y*.5f},ImGuiCond_Always,{.5f,.5f});
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##cargo-operation",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoScrollbar);
    const auto p=ImGui::GetWindowPos();auto* draw=ImGui::GetWindowDrawList();
    const ImVec2 center{p.x+40*scale,p.y+47*scale};
    draw->AddCircle(center,13*scale,IM_COL32(45,45,45,255),32,3*scale);
    const float phase=static_cast<float>(ImGui::GetTime())*4.f;
    draw->PathArcTo(center,13*scale,phase,phase+4.7f,28);draw->PathStroke(green,0,3*scale);
    const char* title=action==1?Tr("Refreshing trailer cargo...","Обновление грузов для прицепа..."):
        action==2?Tr("Refreshing job market...","Обновление рынка заказов..."):
        (action==3||action==5||action==8)?Tr("Reading loaded session...","Чтение загруженной сессии..."):
        action==6?Tr("Refreshing Road Trip offers...","Обновление заказов Road Trip..."):
        action==9?Tr("Refreshing coach offers...","Обновление автобусных рейсов..."):
                  Tr("Creating job...","Создание заказа...");
    draw->AddText(font,20*scale,{p.x+72*scale,p.y+34*scale},white,title);
    const char* body=action==8?Tr("Reading bus stations. The window will reopen automatically. Please wait.","Считываю автобусные станции. Окно откроется автоматически. Пожалуйста, подождите."):
        (action==3||action==5)?Tr("Reading cities, companies and cargo. The order window will reopen automatically. Please wait.","Считываю города, базы и грузы. Окно заказа откроется автоматически после завершения. Пожалуйста, подождите."):
        Tr("Updating offers. Please wait.","Обновление выполняется. Пожалуйста, подождите.");
    draw->AddText(font,17*scale,{p.x+28*scale,p.y+91*scale},muted,body,nullptr,(size.x-56*scale));
    if(progress_total){
        char count[160];sprintf_s(count,action==9?Tr("Stations processed: %llu / %llu","Обработано станций: %llu из %llu"):action==6?Tr("Offers processed: %llu / %llu","Обработано предложений: %llu из %llu"):Tr("Companies processed: %llu / %llu","Обработано баз: %llu из %llu"),progress_done,progress_total);
        draw->AddText(font,15*scale,{p.x+28*scale,p.y+124*scale},muted,count);
        const ImVec2 left{p.x+28*scale,p.y+151*scale},right{p.x+size.x-28*scale,p.y+157*scale};
        draw->AddRectFilled(left,right,IM_COL32(40,40,40,255),3*scale);
        draw->AddRectFilled(left,{left.x+(right.x-left.x)*float(progress_done)/float(progress_total),right.y},green,3*scale);
    }
    ImGui::End();
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
    draw->AddText(font,16,{p.x+24,p.y+16},green,"CARGO CONTROL");
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
    if(SUCCEEDED(result) && !(flags&DXGI_PRESENT_TEST)){
        std::lock_guard<std::recursive_mutex> guard(mutex);
        if(busy_drawn && busy_chain==chain){operation.Presented();busy_drawn=false;busy_chain=nullptr;}
    }
    if(result==DXGI_ERROR_DEVICE_REMOVED || result==DXGI_ERROR_DEVICE_RESET) {
        std::lock_guard<std::recursive_mutex> guard(mutex);ReleaseRenderer();
    }
    --callbacks;return result;
}
}
void RenderFrame(IDXGISwapChain* chain,bool allow_hidden,bool preview_menu,int preview_page) {
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
    if(reopen_catalog && focused){reopen_catalog=false;menu_page=reopen_page;OpenMenu();}
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
    const bool visible_notice=focused&&!operation.Active()&&!menu&&!preview_menu&&!notice.empty();
    const bool alive=lifetime.Tick(GetTickCount64(),visible_notice);
    if(!operation.Active()&&(menu || preview_menu)&&focused)DrawMenu(allow_hidden&&preview_menu?preview_page:-1);else if(alive&&visible_notice)DrawNotice();
    const bool loading=focused && operation.Active();
    if(loading)DrawBusy();
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
    busy_drawn=loading;busy_chain=loading?chain:nullptr;
    if(allow_hidden && loading)operation.Presented(); // Offscreen tests simulate the successful present.
}
bool Initialize(const std::filesystem::path& directory,Logger log,bool install_hook) {
    std::lock_guard<std::recursive_mutex> guard(mutex);logger=log;ini=directory/L"CargoControl.ini";
    const auto read_ini=SettingsReadPath(directory);
    menu_binding=hotkeys::MenuBinding(GetPrivateProfileIntW(L"Bindings",L"Menu",hotkeys::Menu,ini.c_str()));
    cargo=hotkeys::Valid(GetPrivateProfileIntW(L"Bindings",L"Cargo",0,read_ini.c_str()));
    freight=hotkeys::Valid(GetPrivateProfileIntW(L"Bindings",L"Freight",0,read_ini.c_str()));
    if(cargo==menu_binding)cargo=0;if(!hotkeys::PairAllowed(cargo,freight,menu_binding))freight=0;
    draft_menu=menu_binding;draft_cargo=cargo;draft_freight=freight;
    stopping=false;operation.Complete();busy_chain=nullptr;busy_drawn=reopen_catalog=false;menu_page=0;menu=false;capture=0;rendered=0;present_calls=0;input_calls=0;
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
void Request(int action) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    bool allowed=action==1 || action==2;
#ifdef CARGO_CONTROL_COACHES
    allowed=allowed || (coaches::Visible()&&action>=8&&action<=10);
#endif
#ifdef CARGO_CONTROL_CUSTOM_JOBS
    allowed=allowed || action==3 || action==4;
#endif
#ifdef CARGO_CONTROL_ROAD_TRIP
    allowed=allowed || (road_trip::Visible()&&action>=5&&action<=7);
#endif
    if(allowed && operation.Request(action,GetTickCount64())){
        notice.clear();busy_drawn=false;busy_chain=nullptr;progress_done=progress_total=0;
        if(action>=5)CloseMenu();
        Log(("Operation queued: action="+std::to_string(action)+"; waiting for visible feedback frames.").c_str());
    }
}
int Tick(bool ready) {std::lock_guard<std::recursive_mutex> guard(mutex);const auto action=operation.Take(!ready,GetTickCount64());if(action)Log(("Operation started after feedback: action="+std::to_string(action)+".").c_str());return action;}
void FinishAction(bool show_catalog,int page){std::lock_guard<std::recursive_mutex> guard(mutex);if(operation.Active())Log("Operation feedback completed.");operation.Complete();busy_drawn=false;busy_chain=nullptr;progress_done=progress_total=0;reopen_catalog=show_catalog;reopen_page=page;}
void SetProgress(std::uint64_t done,std::uint64_t total){std::lock_guard<std::recursive_mutex> guard(mutex);progress_total=total;progress_done=std::min(done,total);}
void CancelPending(){FinishAction(false);}
Diagnostics Inspect() { std::lock_guard<std::recursive_mutex> guard(mutex);return {rendered,present_calls.load(),input_calls.load(),logo!=nullptr,context!=nullptr,operation.Active(),menu}; }
void Shutdown() {
    stopping=true;
    DisableMouseHooks();
    if(hook_owned)MH_DisableHook(present_site);
    // Disabling prevents new entries. Let already-running Present calls finish before freeing their trampoline.
    while(callbacks.load())Sleep(1);
    std::lock_guard<std::recursive_mutex> guard(mutex);ReleaseRenderer();
    RemoveMouseHooks();if(hook_owned)MH_RemoveHook(present_site);if(minhook_owned)MH_Uninitialize();hook_owned=minhook_owned=false;
    operation.Complete();busy_chain=nullptr;busy_drawn=reopen_catalog=false;notice.clear();keys.fill(false);menu_was_down=cargo_down=freight_down=false;
    previous_present=nullptr;present_site=nullptr;logger=nullptr;
}
}
