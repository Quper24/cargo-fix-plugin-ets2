#pragma once
#include "custom_jobs.h"
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
    if (ImGui::Button(tr("Read loaded session", "Считать загруженную сессию"))) scan();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("Experimental", "Экспериментальная функция"));
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
        ImGui::TextWrapped("%s", tr("Load your profile, then read the session. Lists include its map and cargo mods.",
                                  "Загрузите профиль и считайте сессию. Списки учитывают её карты и моды грузов."));
        return;
    }
    const auto& catalog = state->catalog;
    ImGui::Text(tr("Companies: %llu   Cargos: %llu", "Баз: %llu   Грузов: %llu"),
                static_cast<unsigned long long>(catalog.companies.size()),
                static_cast<unsigned long long>(catalog.cargos.size()));
    if (catalog.skipped) ImGui::TextDisabled(tr("Unreadable entries skipped: %llu", "Пропущено нечитаемых записей: %llu"),
                                            static_cast<unsigned long long>(catalog.skipped));
    ImGui::TextDisabled("%s", state->names_supported ?
        tr("Names from the loaded game and its mods.", "Названия из загруженной игры и её модов.") :
        tr("Display names unavailable; identifiers shown.", "Названия пока недоступны; показаны идентификаторы."));
    if(catalog.label_fallbacks)ImGui::TextDisabled("%s",tr("Some missing names use identifiers.","Для части ненайденных названий показаны идентификаторы."));
    ImGui::RadioButton(tr("Freight market", "Грузоперевозки"), &market, 0); ImGui::SameLine();
    ImGui::RadioButton(tr("Owned trailer", "Собственный прицеп"), &market, 1);
    ImGui::SetNextItemWidth(320 * scale);
    ImGui::InputText(tr("Filter names", "Поиск по названиям"), search.data(), search.size());
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
    if (!origin && !origin_id.empty()) { origin_id.clear(); cargo_id.clear(); destination_id.clear(); }
    auto city_select = [&](const char* label, std::string& selected, bool destination) {
        std::map<std::string,std::string> cities;
        for (const auto& company : catalog.companies)
            if (!destination || (!cargo_id.empty() && company.id != origin_id && Accepts(company, cargo_id)))
                cities.emplace(company.city,city_names[company.city]);
        std::vector<std::pair<std::string,std::string>> sorted(cities.begin(),cities.end());
        std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.second==b.second?a.first<b.first:a.second<b.second;});
        bool changed = false;
        ImGui::SetNextItemWidth(310 * scale);
        if (ImGui::BeginCombo(label, selected.empty() ? tr("Select...", "Выберите...") : city_names[selected].c_str())) {
            for (const auto& city : sorted) if (visible(city.second)) {
                ImGui::PushID(city.first.c_str());
                if(ImGui::Selectable(city.second.c_str(),selected==city.first)){selected=city.first;changed=true;}
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        return changed;
    };
    auto base_select = [&](const char* label, const std::string& city, std::string& selected, bool destination) {
        const auto* company = find_company(selected);
        bool changed = false;
        ImGui::SetNextItemWidth(310 * scale);
        if (ImGui::BeginCombo(label, company ? company->name.c_str() : tr("Select...", "Выберите..."))) {
            std::vector<const Company*> bases;
            for(const auto& base:catalog.companies)
                if(base.city==city && visible(base.name) && (!destination || (!cargo_id.empty() && base.id!=origin_id && Accepts(base,cargo_id))))bases.push_back(&base);
            std::sort(bases.begin(),bases.end(),[](const auto* a,const auto* b){return a->name==b->name?a->id<b->id:a->name<b->name;});
            for (const auto* item : bases) {
                const auto& base=*item;
                ImGui::PushID(base.id.c_str());
                if (ImGui::Selectable(base.name.c_str(), base.id == selected)) { selected = base.id; changed = true; }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        return changed;
    };
    if (city_select(tr("Origin city", "Город отправления"), origin_city, false)) {
        origin_id.clear(); cargo_id.clear(); destination_city.clear(); destination_id.clear();
    }
    if (base_select(tr("Origin company", "База отправления"), origin_city, origin_id, false)) {
        cargo_id.clear(); destination_city.clear(); destination_id.clear();
    }
    origin = find_company(origin_id);
    if (origin && std::find(origin->outgoing.begin(), origin->outgoing.end(), cargo_id) == origin->outgoing.end()) {
        cargo_id.clear(); destination_id.clear();
    }
    ImGui::SetNextItemWidth(310 * scale);
    if (ImGui::BeginCombo(tr("Cargo", "Груз"), cargo_id.empty() ? tr("Select...", "Выберите...") : cargo_label(cargo_id).c_str())) {
        std::vector<std::string> available=origin?origin->outgoing:std::vector<std::string>{};
        std::sort(available.begin(),available.end(),[&](const auto& a,const auto& b){return cargo_label(a)==cargo_label(b)?a<b:cargo_label(a)<cargo_label(b);});
        for (const auto& cargo : available) if (visible(cargo_label(cargo))) {
            ImGui::PushID(cargo.c_str());
            if(ImGui::Selectable(cargo_label(cargo).c_str(),cargo_id==cargo)) {
                cargo_id=cargo;destination_city.clear();destination_id.clear();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (city_select(tr("Destination city", "Город назначения"), destination_city, true)) destination_id.clear();
    base_select(tr("Destination company", "База назначения"), destination_city, destination_id, true);
    const auto* destination = find_company(destination_id);
    if (destination && !Accepts(*destination, cargo_id)) destination_id.clear();
    if(market==0)ImGui::Checkbox(tr("Allow replacing one source-company offer","Разрешить замену одного предложения на базе отправления"),&allow_replace);
    ImGui::TextWrapped("%s",tr("The game checks the cargo, route and trailer. Accept the created offer from its market.",
                              "Игра проверит груз, направление и прицеп. Готовое предложение можно принять на соответствующем рынке."));
    if(!state->creation_supported)ImGui::TextDisabled("%s",tr("Native job creation is unavailable for this game code.","Создание заказа недоступно для этого кода игры."));
    JobRequest request{state->session,market==1?Market::OwnedTrailer:Market::Freight,origin_id,cargo_id,destination_id,allow_replace};
    ImGui::BeginDisabled(!state->creation_supported || !ValidSelection(catalog,request));
    if(ImGui::Button(tr("Create job", "Создать заказ"))){
        if(QueueCreation(request)){request_error.clear();create();}
        else request_error=tr("Request not queued. Read the session and try again.","Запрос не поставлен в очередь. Считайте сессию и повторите действие.");
    }
    ImGui::EndDisabled();
    if(!request_error.empty())ImGui::TextWrapped("%s",request_error.c_str());
    if(state->has_creation_result)ImGui::TextWrapped("%s",CreationMessage(state->creation_result,russian).c_str());
}
}
