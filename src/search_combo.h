#pragma once
#include "ui_controls.h"
#include <imgui_internal.h>
#include <array>
#include <map>
#include <string>
#include <vector>
#include <cstdio>
#include <functional>

namespace controls {
struct Choice {std::string id,name;};
struct SearchState {
    std::array<char,2048> text{};
    std::string selected;
    bool edited=false,focus=false;
    int cursor=0,move=0;
};
inline int SearchHistory(ImGuiInputTextCallbackData* data){
    auto& state=*static_cast<SearchState*>(data->UserData);
    state.move=data->EventKey==ImGuiKey_UpArrow?-1:1;return 0;
}
// Editable field + a list anchored below it. Only selected IDs leave this UI;
// typing an arbitrary name never turns it into a valid game object.
inline bool SearchCombo(const char* id,std::string& selected,const std::vector<Choice>& choices,bool russian,
                        const std::function<bool(const std::string&,const char*)>& matches){
    static std::map<ImGuiID,SearchState> states;
    ImGui::PushID(id);auto& state=states[ImGui::GetID("field")];
    const auto popup_id="##options";
    bool open=ImGui::IsPopupOpen(popup_id);
    if(!open||state.selected!=selected){
        state.selected=selected;state.edited=false;state.cursor=0;
        std::string label;
        for(const auto& item:choices)if(item.id==selected){label=item.name;break;}
        std::snprintf(state.text.data(),state.text.size(),"%s",label.c_str());
    }
    const float width=ImGui::CalcItemWidth(),arrow=ImGui::GetFrameHeight();
    const auto origin=ImGui::GetCursorScreenPos();
    ImGui::BeginGroup();ImGui::SetNextItemWidth(std::max(1.f,width-arrow));
    if(state.focus){ImGui::SetKeyboardFocusHere();state.focus=false;}
    const bool enter=ImGui::InputTextWithHint("##text",russian?"Выберите или введите название...":"Choose or type a name...",
        state.text.data(),state.text.size(),ImGuiInputTextFlags_AutoSelectAll|ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_CallbackHistory,SearchHistory,&state);
    if(ImGui::IsItemEdited()){state.edited=true;state.cursor=0;}
    if(ImGui::IsItemActivated()){
        ImGui::OpenPopup(popup_id);open=true;
    }
    ImGui::SameLine(0,0);
    if(ImGui::ArrowButton("##arrow",ImGuiDir_Down)){
        if(open){state.focus=false;}else{ImGui::OpenPopup(popup_id);state.focus=true;state.edited=false;state.cursor=0;}
    }
    ImGui::EndGroup();
    std::vector<std::size_t> filtered;
    for(std::size_t i=0;i<choices.size();++i)if(!state.edited||matches(choices[i].name,state.text.data()))filtered.push_back(i);
    if(!filtered.empty())state.cursor=std::clamp(state.cursor+state.move,0,static_cast<int>(filtered.size())-1);
    const bool moved=state.move!=0;state.move=0;
    const auto& io=ImGui::GetIO();
    const float below=io.DisplaySize.y-origin.y-arrow-12,above=origin.y-12;
    const bool upwards=below<ImGui::GetTextLineHeightWithSpacing()*5&&above>below;
    const float height=std::max(1.f,std::min(ImGui::GetTextLineHeightWithSpacing()*9+16,upwards?above:below));
    ImGui::SetNextWindowPos({origin.x,upwards?origin.y-height:origin.y+arrow});
    ImGui::SetNextWindowSize({width,height});
    bool changed=false;
    if(ImGui::BeginPopup(popup_id,ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNavInputs|ImGuiWindowFlags_NoNavFocus)){
        // Keep typing focus in the editor while drawing/hit-testing the list
        // above its parent. Focusing the popup would deactivate InputText.
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        if(ImGui::IsKeyPressed(ImGuiKey_Escape)){ImGui::CloseCurrentPopup();}
        else{
            controls::NoMatches(!filtered.empty(),russian);
            for(std::size_t row=0;row<filtered.size();++row){
                const auto& item=choices[filtered[row]];ImGui::PushID(item.id.c_str());
                if(ImGui::Selectable(item.name.c_str(),state.cursor==static_cast<int>(row))||(enter&&state.cursor==static_cast<int>(row))){
                    selected=item.id;state.selected=selected;state.edited=false;changed=true;ImGui::CloseCurrentPopup();
                }
                if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",item.name.c_str());
                if(moved&&state.cursor==static_cast<int>(row))ImGui::SetScrollHereY();
                ImGui::PopID();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();return changed;
}
}
