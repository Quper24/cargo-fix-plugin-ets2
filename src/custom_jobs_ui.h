#pragma once
#include "custom_jobs.h"
#include "brand_palette.h"
#include <imgui.h>
#include <array>
#include <set>

namespace custom_jobs {
// UI owns strings only; native pointers never cross the render/game boundary.
inline void DrawCatalog(float scale, bool russian, const std::function<void()>& scan,const std::function<void()>& create) {
    const auto state = Snapshot();
    auto tr = [&](const char* en, const char* ru) { return russian ? ru : en; };
    static std::uint64_t session = UINT64_MAX;
    static std::string origin_city, origin_id, cargo_id, destination_city, destination_id;
    static std::array<char, 128> search{};
    static int market = 0;
    static bool allow_replace=false;
    static std::string request_error;
    if (session != state->session) {
        session = state->session;
        origin_city.clear(); origin_id.clear(); cargo_id.clear(); destination_city.clear(); destination_id.clear();
        search.fill(0);
        allow_replace=false;request_error.clear();
    }
    ImGui::SetWindowFontScale(scale * .5f);
    const ImVec4 scan_color(brand_palette::Red/255.f,brand_palette::Green/255.f,brand_palette::Blue/255.f,1);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{16*scale,11*scale});
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,{.5f,.5f});
    ImGui::PushStyleColor(ImGuiCol_Button,scan_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(std::min(1.f,scan_color.x*1.08f),std::min(1.f,scan_color.y*1.08f),std::min(1.f,scan_color.z*1.08f),1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(scan_color.x*.85f,scan_color.y*.85f,scan_color.z*.85f,1));
    ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0,0,0,1));
    if(ImGui::Button(tr("Read loaded session","Считать загруженную сессию"),{300*scale,40*scale}))scan();
    ImGui::PopStyleColor(4);ImGui::PopStyleVar(2);
    ImGui::SameLine(0,16*scale);
    ImGui::TextDisabled("%s", tr("Experimental", "Экспериментальная функция"));
    ImGui::Dummy({0,4*scale});
    if (!state->supported) {
        ImGui::TextWrapped("%s", tr("Session catalog is unavailable for this game build.",
                                  "Для этой сборки игры чтение каталога пока недоступно."));
        const char* reason = tr("The catalog code or data references differ. See game.log.txt.",
                               "Код чтения или связи данных отличаются. Подробности в game.log.txt.");
        ImGui::TextWrapped("%s",reason);
        return;
    }
    if (!state->error.empty()) {
        ImGui::TextWrapped("%s", tr("Could not read the session catalog. See game.log.txt.",
                                  "Не удалось прочитать каталог сессии. Подробности в game.log.txt."));
    }
    if (!state->scanned) {
        ImGui::TextWrapped("%s",tr("The window is temporarily hidden while reading. It reopens automatically when the data is ready.",
            "Во время чтения окно временно скроется и откроется автоматически, когда данные будут готовы."));
        ImGui::TextWrapped("%s", tr("Load your profile, then read the session. Lists include its map and cargo mods.",
                                  "Загрузите профиль и считайте сессию. Списки учитывают её карты и моды грузов."));
        return;
    }
    const auto& catalog = state->catalog;
    ImGui::Text(tr("Data ready. Companies: %llu   Cargos: %llu", "Данные загружены. Баз: %llu   Грузов: %llu"),
                static_cast<unsigned long long>(catalog.companies.size()),
                static_cast<unsigned long long>(catalog.cargos.size()));
    if(catalog.skipped){
        ImGui::SameLine();ImGui::TextDisabled("%s",tr("Some entries unavailable","Есть пропущенные записи"));
        if(ImGui::IsItemHovered())ImGui::SetTooltip(tr("Unreadable entries skipped: %llu","Пропущено нечитаемых записей: %llu"),static_cast<unsigned long long>(catalog.skipped));
    }
    if(!state->names_supported)ImGui::TextDisabled("%s",tr("Display names unavailable; identifiers shown.","Названия пока недоступны; показаны идентификаторы."));
    else if(catalog.label_fallbacks)ImGui::TextDisabled("%s",tr("Some missing names use identifiers.","Для части ненайденных названий показаны идентификаторы."));
    ImGui::RadioButton(tr("Freight market", "Грузоперевозки"), &market, 0); ImGui::SameLine();
    if(ImGui::RadioButton(tr("Owned trailer", "Собственный прицеп"), &market, 1))RefreshTrailerCargos();
    if(market==1){
        const auto status=state->trailer_cargos.status;
        if(status==TrailerCargoStatus::Ready)
            ImGui::TextWrapped("%s",tr("Cargo is filtered for your trailer. The list is shared by all origin companies.","Грузы отфильтрованы по вашему прицепу. Список общий для всех баз отправления."));
        else if(status==TrailerCargoStatus::NoTrailer)
            ImGui::TextWrapped("%s",tr("Select an owned trailer in the game to see its cargo.","Выберите собственный прицеп в игре, чтобы увидеть подходящие грузы."));
        else
            ImGui::TextWrapped("%s",tr("Trailer cargo list unavailable. Read the loaded session again.","Не удалось получить грузы для прицепа. Считайте загруженную сессию заново."));
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##catalog-search",tr("Search cities, companies or cargo","Поиск города, базы или груза"),search.data(),search.size());
    auto visible = [&](const std::string& value) { return ContainsName(value,search.data()); };
    std::map<std::string,std::string> city_names,cargo_names;
    for(const auto& company:catalog.companies)city_names.emplace(company.city,company.city_name.empty()?company.city:company.city_name);
    for(const auto& cargo:catalog.cargos)cargo_names.emplace(cargo.id,cargo.name);
    auto cargo_label=[&](const std::string& id) -> const std::string& {
        const auto found=cargo_names.find(id);return found==cargo_names.end()?id:found->second;
    };
    auto find_company = [&](const std::string& id) -> const Company* {
        for (const auto& company : catalog.companies) if (company.id == id) return &company;
        return nullptr;
    };
    const auto* origin = find_company(origin_id);
    if (!origin && !origin_id.empty()) { origin_id.clear(); if(market==0)cargo_id.clear(); destination_id.clear(); }
    auto receiver_allowed=[&](const Company& company){
        return company.id!=origin_id && !cargo_id.empty() && (market==1 || Accepts(company,cargo_id));
    };
    auto city_select = [&](const char* label, std::string& selected, bool destination) {
        std::map<std::string,std::string> cities;
        for (const auto& company : catalog.companies)
            if (!destination || receiver_allowed(company))
                cities.emplace(company.city,city_names[company.city]);
        std::vector<std::pair<std::string,std::string>> sorted(cities.begin(),cities.end());
        std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.second==b.second?a.first<b.first:a.second<b.second;});
        bool changed = false;
        ImGui::TextUnformatted(label);ImGui::PushID(label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##value", selected.empty() ? tr("Select...", "Выберите...") : city_names[selected].c_str())) {
            for (const auto& city : sorted) if (visible(city.second)) {
                ImGui::PushID(city.first.c_str());
                if(ImGui::Selectable(city.second.c_str(),selected==city.first)){selected=city.first;changed=true;search.fill(0);}
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();return changed;
    };
    auto base_select = [&](const char* label, const std::string& city, std::string& selected, bool destination) {
        const auto* company = find_company(selected);
        bool changed = false;
        ImGui::TextUnformatted(label);ImGui::PushID(label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##value", company ? company->name.c_str() : tr("Select...", "Выберите..."))) {
            std::vector<const Company*> bases;
            for(const auto& base:catalog.companies)
                if(base.city==city && visible(base.name) && (!destination || receiver_allowed(base)))bases.push_back(&base);
            std::sort(bases.begin(),bases.end(),[](const auto* a,const auto* b){return a->name==b->name?a->id<b->id:a->name<b->name;});
            for (const auto* item : bases) {
                const auto& base=*item;
                ImGui::PushID(base.id.c_str());
                if (ImGui::Selectable(base.name.c_str(), base.id == selected)) { selected = base.id; changed = true;search.fill(0); }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();return changed;
    };
    if(ImGui::BeginTable("origin-fields",2,ImGuiTableFlags_SizingStretchSame)){
        ImGui::TableNextColumn();
        if(city_select(tr("Origin city","Город отправления"),origin_city,false)){
            origin_id.clear();if(market==0)cargo_id.clear();destination_city.clear();destination_id.clear();
        }
        ImGui::TableNextColumn();
        if(base_select(tr("Origin company","База отправления"),origin_city,origin_id,false)){
            if(market==0)cargo_id.clear();destination_city.clear();destination_id.clear();
        }
        ImGui::EndTable();
    }
    origin = find_company(origin_id);
    const auto available=AvailableCargos(catalog,origin,market==1,state->trailer_cargos);
    if (!cargo_id.empty() && std::find(available.begin(), available.end(), cargo_id) == available.end()) {
        cargo_id.clear(); destination_city.clear();destination_id.clear();
    }
    ImGui::TextUnformatted(tr("Cargo","Груз"));ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##cargo", cargo_id.empty() ? tr("Select...", "Выберите...") : cargo_label(cargo_id).c_str())) {
        auto sorted=available;
        std::sort(sorted.begin(),sorted.end(),[&](const auto& a,const auto& b){return cargo_label(a)==cargo_label(b)?a<b:cargo_label(a)<cargo_label(b);});
        for (const auto& cargo : sorted) if (visible(cargo_label(cargo))) {
            ImGui::PushID(cargo.c_str());
            if(ImGui::Selectable(cargo_label(cargo).c_str(),cargo_id==cargo)) {
                cargo_id=cargo;destination_city.clear();destination_id.clear();search.fill(0);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if(ImGui::BeginTable("destination-fields",2,ImGuiTableFlags_SizingStretchSame)){
        ImGui::TableNextColumn();
        if(city_select(tr("Destination city","Город назначения"),destination_city,true))destination_id.clear();
        ImGui::TableNextColumn();
        base_select(tr("Destination company","База назначения"),destination_city,destination_id,true);
        ImGui::EndTable();
    }
    const auto* destination = find_company(destination_id);
    if(destination && !receiver_allowed(*destination)){destination_city.clear();destination_id.clear();}
    if(market==0)ImGui::Checkbox(tr("Allow replacing one source-company offer","Разрешить замену одного предложения на базе отправления"),&allow_replace);
    ImGui::TextWrapped("%s",tr("Select your companies and a compatible cargo. Accept the created offer from its market.",
                              "Выберите базы и подходящий прицепу груз. Готовое предложение можно принять на соответствующем рынке."));
    if(!state->creation_supported)ImGui::TextDisabled("%s",tr("Native job creation is unavailable for this game code.","Создание заказа недоступно для этого кода игры."));
    JobRequest request{state->session,market==1?Market::OwnedTrailer:Market::Freight,origin_id,cargo_id,destination_id,allow_replace};
    const bool can_create=state->creation_supported && ValidSelection(catalog,request);
    ImGui::BeginDisabled(!can_create);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{12*scale,9*scale});
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign,{.5f,.5f});
    if(can_create){
        const ImVec4 color(brand_palette::Red/255.f,brand_palette::Green/255.f,brand_palette::Blue/255.f,1);
        ImGui::PushStyleColor(ImGuiCol_Button,color);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(std::min(1.f,color.x*1.08f),std::min(1.f,color.y*1.08f),std::min(1.f,color.z*1.08f),1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,ImVec4(color.x*.85f,color.y*.85f,color.z*.85f,1));
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(0,0,0,1));
    }
    if(ImGui::Button(tr("Create job", "Создать заказ"),{180*scale,36*scale})){
        if(QueueCreation(request)){request_error.clear();create();}
        else request_error=tr("Request not queued. Read the session and try again.","Запрос не поставлен в очередь. Считайте сессию и повторите действие.");
    }
    if(can_create)ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);ImGui::EndDisabled();
    if(market==1 && origin && available.empty() && state->trailer_cargos.status==TrailerCargoStatus::Ready)
        ImGui::TextWrapped("%s",tr("No cargo compatible with your trailer was found in the loaded session.","В загруженной сессии не найдено грузов для вашего прицепа."));
    if(!request_error.empty())ImGui::TextWrapped("%s",request_error.c_str());
    if(state->has_creation_result)ImGui::TextWrapped("%s",CreationMessage(state->creation_result,russian).c_str());
}
}
