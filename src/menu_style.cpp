#include "menu_style.h"
#include "branding.h"
#include <objidl.h>
#include <gdiplus.h>
#include <string>
namespace menu_style {
namespace {
void Round(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius=14) {
    Gdiplus::Graphics g(dc); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    const int x=rect.left, y=rect.top, w=rect.right-x-1, h=rect.bottom-y-1, d=radius*2;
    path.AddArc(x,y,d,d,180,90); path.AddArc(x+w-d,y,d,d,270,90);
    path.AddArc(x+w-d,y+h-d,d,d,0,90); path.AddArc(x,y+h-d,d,d,90,90); path.CloseFigure();
    auto color=[](COLORREF c){ return Gdiplus::Color(255,GetRValue(c),GetGValue(c),GetBValue(c)); };
    Gdiplus::SolidBrush brush(color(fill)); Gdiplus::Pen pen(color(border));
    g.FillPath(&brush,&path); g.DrawPath(&pen,&path);
}
void Text(HDC dc, const wchar_t* text, RECT rect, int size, int weight, COLORREF color) {
    HFONT font=CreateFontW(-size,0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
    auto old=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,color);
    DrawTextW(dc,text,-1,&rect,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
    SelectObject(dc,old); DeleteObject(font);
}
}
void Paint(HDC dc, bool ru) {
    RECT bounds{0,0,Width,Height}; HBRUSH bg=CreateSolidBrush(Background); FillRect(dc,&bounds,bg); DeleteObject(bg);
    const COLORREF white=RGB(244,247,245), muted=RGB(151,166,155), line=RGB(44,59,47);
    Text(dc,L"QUPER SIMULATOR",{32,25,670,48},14,FW_SEMIBOLD,Accent);
    Text(dc,L"Cargo Control ETS2",{30,53,690,99},34,FW_SEMIBOLD,white);
    Text(dc,ru?L"Ваш рынок грузов. Ваши правила.":L"Your cargo market. Your rules.",{32,107,680,136},17,FW_NORMAL,muted);
    branding::PaintLogo(dc,{710,23,842,120});
    Round(dc,{28,156,438,360},Surface,line); Round(dc,{462,156,872,360},Surface,line);
    Text(dc,ru?L"Грузы для прицепа":L"Owned-trailer cargo",{48,176,418,207},23,FW_SEMIBOLD,white);
    Text(dc,ru?L"Добавить до 5 грузов в каждой компании.\nСуществующие заказы сохраняются.":L"Add up to 5 offers per company.\nKeep your existing orders.",{48,218,418,271},16,FW_NORMAL,muted);
    Text(dc,ru?L"Рынок и агентства":L"Freight & Quick jobs",{482,176,852,207},23,FW_SEMIBOLD,white);
    Text(dc,ru?L"Обновить предложения во всех компаниях.\nСуществующие заказы будут заменены.":L"Refresh offers across all companies.\nExisting orders will be replaced.",{482,218,852,271},16,FW_NORMAL,muted);
    Text(dc,ru?L"Горячие клавиши":L"Keyboard shortcuts",{32,385,700,417},21,FW_SEMIBOLD,white);
    Text(dc,ru?L"Добавление грузов":L"Add cargo",{48,428,418,450},15,FW_NORMAL,muted);
    Text(dc,ru?L"Обновление заказов":L"Refresh orders",{482,428,852,450},15,FW_NORMAL,muted);
    Round(dc,{46,456,340,497},Surface,line,8); Round(dc,{480,456,774,497},Surface,line,8);
    Text(dc,ru?L"Выберите поле и нажмите сочетание клавиш. «Сброс» отключает назначение.":L"Select a field and press a shortcut. Clear disables the binding.",{32,518,870,541},15,FW_NORMAL,muted);
    Text(dc,ru?L"Перед генерацией остановите грузовик. Меню: Ctrl + Alt + F9.":L"Stop your truck before generating offers. Menu: Ctrl + Alt + F9.",{32,545,870,568},15,FW_NORMAL,muted);
    Text(dc,ru?L"Изменения применяются после сохранения.":L"Save to apply your shortcut settings.",{32,599,550,627},14,FW_NORMAL,muted);
}
void Hotkey(HDC dc, RECT bounds, WORD binding, bool focused, bool ru) {
    HBRUSH bg=CreateSolidBrush(Surface); FillRect(dc,&bounds,bg); DeleteObject(bg);
    std::wstring label;
    if (binding & 0x200) label += L"Ctrl + ";
    if (binding & 0x100) label += L"Shift + ";
    if (binding & 0x400) label += L"Alt + ";
    if (binding & 255) {
        wchar_t key[100]{}; const UINT scan=MapVirtualKeyW(binding & 255,MAPVK_VK_TO_VSC_EX);
        LONG flags=static_cast<LONG>((scan & 255)<<16); if (scan & 0xff00) flags |= 1<<24;
        if (GetKeyNameTextW(flags,key,100)) label += key;
        else label += std::to_wstring(binding & 255);
    } else label = focused ? (ru?L"Нажмите сочетание…":L"Press a shortcut…") : (ru?L"Не назначено":L"Not assigned");
    bounds.left += 4; bounds.top += 2;
    Text(dc,label.c_str(),bounds,16,FW_NORMAL,(focused || (binding & 255))?Accent:RGB(151,166,155));
}
void Button(const DRAWITEMSTRUCT& item) {
    wchar_t label[180]{}; GetWindowTextW(item.hwndItem,label,180);
    const bool primary=item.CtlID==101 || item.CtlID==102 || item.CtlID==105;
    POINT pt{}; GetCursorPos(&pt); ScreenToClient(item.hwndItem,&pt);
    const bool hover=PtInRect(&item.rcItem,pt)!=0;
    const bool pressed=item.itemState & ODS_SELECTED;
    const COLORREF fill=primary?(pressed?RGB(133,176,125):hover?RGB(198,236,190):Accent):(hover?RGB(39,52,42):Surface);
    HBRUSH background=CreateSolidBrush(item.CtlID==101 || item.CtlID==102 ? Surface : Background);
    FillRect(item.hDC,&item.rcItem,background); DeleteObject(background);
    Round(item.hDC,item.rcItem,fill,(item.itemState&ODS_FOCUS)?RGB(233,252,227):primary?fill:RGB(57,76,61),8);
    SetBkMode(item.hDC,TRANSPARENT); SetTextColor(item.hDC,primary?RGB(14,25,16):RGB(215,227,217));
    auto font=reinterpret_cast<HFONT>(SendMessageW(item.hwndItem,WM_GETFONT,0,0)); auto old=SelectObject(item.hDC,font);
    RECT r=item.rcItem; DrawTextW(item.hDC,label,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    SelectObject(item.hDC,old);
}
}
