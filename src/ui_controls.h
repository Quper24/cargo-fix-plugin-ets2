#pragma once
#include "brand_palette.h"
#include <imgui.h>
#include <algorithm>

namespace controls {
inline ImFont* primary_font=nullptr;
struct PrimaryFont {
    PrimaryFont(){if(primary_font)ImGui::PushFont(primary_font);}
    ~PrimaryFont(){if(primary_font)ImGui::PopFont();}
};
// Keep the label readable through normal, hover and pressed states.
inline bool PrimaryButton(const char* label,ImVec2 size,bool enabled=true){
    PrimaryFont weight;
    ImGui::BeginDisabled(!enabled);
    if(enabled){
        const ImVec4 color(brand_palette::Red/255.f,brand_palette::Green/255.f,brand_palette::Blue/255.f,1);
        ImGui::PushStyleColor(ImGuiCol_Button,color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(std::min(1.f,color.x*1.08f),std::min(1.f,color.y*1.08f),std::min(1.f,color.z*1.08f),1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(color.x*.85f,color.y*.85f,color.z*.85f,1));
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0,0,0,1));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,{.5f,.5f});
    const bool pressed=ImGui::Button(label,size);
    ImGui::PopStyleVar();if(enabled)ImGui::PopStyleColor(4);
    ImGui::EndDisabled();return pressed;
}
inline void Hint(const char* text){
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)){
        ImGui::BeginTooltip();ImGui::PushTextWrapPos(ImGui::GetFontSize()*28);
        ImGui::TextUnformatted(text);ImGui::PopTextWrapPos();ImGui::EndTooltip();
    }
}
inline void NoMatches(bool found,bool russian){
    if(!found)ImGui::TextDisabled("%s",russian?"Нет подходящих вариантов. Измените поиск или предыдущий выбор.":"No matches. Change the search or previous selection.");
}
}
