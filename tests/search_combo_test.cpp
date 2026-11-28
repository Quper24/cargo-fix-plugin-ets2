#include "search_combo.h"
#include <cstdlib>
#include <cstdio>
#include <cwctype>
#define Check(value) do{if(!(value)){std::fprintf(stderr,"Search combo failed: line %d\n",__LINE__);std::exit(1);}}while(0)
int main(){
    std::mt19937 random{123};std::string selected="unchanged";
    Check(!controls::ChooseRandom(selected,{},random) && selected=="unchanged");
    const std::vector<controls::Choice> filtered={{"cal_city_a","A"},{"cal_city_b","B"}};
    for(int i=0;i<100;++i){controls::ChooseRandom(selected,filtered,random);Check(selected=="cal_city_a" || selected=="cal_city_b");}
    ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize={800,600};io.DeltaTime=1.f/60;
    unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    std::string city="a",company;
    std::vector<controls::Choice> cities={{"a","Alpha"},{"b","Beta"},{"c","Beta North"}},companies={{"x","Warehouse"},{"y","Market"}};
    auto matches=[](const std::string& label,const char* query){std::string a=label,b=query;for(auto& c:a)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));for(auto& c:b)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));return a.find(b)!=std::string::npos;};
    bool changed=false;
    auto frame=[&]{ImGui::NewFrame();ImGui::SetNextWindowPos({10,10});ImGui::SetNextWindowSize({600,500});ImGui::Begin("test",nullptr,ImGuiWindowFlags_NoTitleBar);
        ImGui::SetCursorScreenPos({30,40});ImGui::SetNextItemWidth(350);changed=controls::SearchCombo("city",city,cities,false,matches);
        ImGui::SetCursorScreenPos({30,80});ImGui::SetNextItemWidth(350);changed=controls::SearchCombo("company",company,companies,false,matches)||changed;
        ImGui::End();ImGui::Render();};
    auto click=[&](float x,float y){io.AddMousePosEvent(x,y);io.AddMouseButtonEvent(0,true);frame();io.AddMouseButtonEvent(0,false);frame();};
    auto key=[&](ImGuiKey k){io.AddKeyEvent(k,true);frame();const bool result=changed;io.AddKeyEvent(k,false);frame();return result;};
    frame();frame();click(70,50);frame();
    Check(io.WantTextInput);
    io.AddInputCharactersUTF8("Beta");frame();frame();
    Check(city=="a"); // Typing cannot commit an arbitrary object.
    key(ImGuiKey_DownArrow);Check(key(ImGuiKey_Enter));Check(city=="c");
    click(70,90);frame();io.AddInputCharactersUTF8("Ware");frame();frame();
    Check(key(ImGuiKey_Enter));Check(company=="x"&&city=="c");
    click(70,50);frame();io.AddInputCharactersUTF8("no such city");frame();frame();
    Check(!key(ImGuiKey_Enter));Check(city=="c");key(ImGuiKey_Escape);Check(city=="c");
    // Reopening drops the old query and permits choosing a different row.
    click(500,300);click(70,50);frame();key(ImGuiKey_DownArrow);Check(key(ImGuiKey_Enter));Check(city=="b");
    // Arrow opens the same field, with keyboard focus in its editor.
    click(365,90);frame();frame();io.AddInputCharactersUTF8("Market");frame();frame();Check(key(ImGuiKey_Enter));Check(company=="y");
    click(70,50);frame();frame();click(70,40+ImGui::GetFrameHeight()+10);frame();Check(city=="a");
    click(365,90);frame();frame();
    Check(ImGui::GetCurrentContext()->OpenPopupStack.Size>0);
    click(500,300);frame();
    Check(ImGui::GetCurrentContext()->OpenPopupStack.Size==0);
    Check(company=="y");
    click(365,90);frame();frame();
    Check(ImGui::GetCurrentContext()->OpenPopupStack.Size>0);
    click(365,90);frame();
    Check(ImGui::GetCurrentContext()->OpenPopupStack.Size==0);
    ImGui::DestroyContext();std::puts("Editable selectors: filtering, keyboard, outside-click and arrow closing passed.");
}

