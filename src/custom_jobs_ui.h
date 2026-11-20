#pragma once
#include "custom_jobs.h"
#include <imgui.h>
#include <array>
#include <set>

namespace custom_jobs {
// UI owns strings only; native pointers never cross the render/game boundary.
inline void DrawCatalog(float scale, bool russian, const std::function<void()>& scan) {
    const auto state = Snapshot();
    auto tr = [&](const char* en, const char* ru) { return russian ? ru : en; };
    static std::uint64_t session = UINT64_MAX;
    static std::string origin_city, origin_id, cargo_id, destination_city, destination_id;
    static std::array<char, 128> search{};
    static int market = 0;
    if (session != state->session) {
        session = state->session;
        origin_city.clear(); origin_id.clear(); cargo_id.clear(); destination_city.clear(); destination_id.clear();
        search.fill(0);
    }
    ImGui::SetWindowFontScale(scale * .5f);
    if (ImGui::Button(tr("Read loaded session", "Считать загруженную сессию"))) scan();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", tr("Experimental", "Экспериментальная функция"));
    if (!state->supported) {
        ImGui::TextWrapped("%s", tr("Session catalog is unavailable for this game build.",
                                  "Для этой сборки игры чтение каталога пока недоступно."));
        const char* reason = state->availability == Availability::UnreadableExe ?
            tr("Could not read the game EXE.","Не удалось прочитать EXE игры.") :
            state->availability == Availability::CoreMismatch ?
            tr("Core function addresses differ from the catalog profile.","Адреса функций отличаются от профиля каталога.") :
            state->availability == Availability::CodeMismatch ?
            tr("The token pool code check failed.","Проверка кода доступа к данным не пройдена.") :
            tr("No verified catalog profile for this EXE. See game.log.txt.","Для этого EXE нет проверенного профиля каталога. Подробности в game.log.txt.");
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
    ImGui::TextDisabled("%s", tr("Prototype lists show game identifiers.", "В прототипе показаны игровые идентификаторы."));
    ImGui::RadioButton(tr("Freight market", "Грузоперевозки"), &market, 0); ImGui::SameLine();
    ImGui::RadioButton(tr("Owned trailer", "Собственный прицеп"), &market, 1);
    ImGui::SetNextItemWidth(320 * scale);
    ImGui::InputText(tr("Filter names", "Поиск по названиям"), search.data(), search.size());
    auto visible = [&](const std::string& value) { return value.find(search.data()) != std::string::npos; };
    auto find_company = [&](const std::string& id) -> const Company* {
        for (const auto& company : catalog.companies) if (company.id == id) return &company;
        return nullptr;
    };
    const auto* origin = find_company(origin_id);
    if (!origin && !origin_id.empty()) { origin_id.clear(); cargo_id.clear(); destination_id.clear(); }
    auto city_select = [&](const char* label, std::string& selected, bool destination) {
        std::set<std::string> cities;
        for (const auto& company : catalog.companies)
            if (!destination || (!cargo_id.empty() && company.id != origin_id && Accepts(company, cargo_id)))
                cities.insert(company.city);
        bool changed = false;
        ImGui::SetNextItemWidth(310 * scale);
        if (ImGui::BeginCombo(label, selected.empty() ? tr("Select...", "Выберите...") : selected.c_str())) {
            for (const auto& city : cities) if (visible(city) && ImGui::Selectable(city.c_str(), selected == city)) {
                selected = city; changed = true;
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
            for (const auto& base : catalog.companies) {
                if (base.city != city || !visible(base.name) ||
                    (destination && (cargo_id.empty() || base.id == origin_id || !Accepts(base, cargo_id)))) continue;
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
    if (ImGui::BeginCombo(tr("Cargo", "Груз"), cargo_id.empty() ? tr("Select...", "Выберите...") : cargo_id.c_str())) {
        if (origin) for (const auto& cargo : origin->outgoing) if (visible(cargo) && ImGui::Selectable(cargo.c_str(), cargo_id == cargo)) {
            cargo_id = cargo; destination_city.clear(); destination_id.clear();
        }
        ImGui::EndCombo();
    }
    if (city_select(tr("Destination city", "Город назначения"), destination_city, true)) destination_id.clear();
    base_select(tr("Destination company", "База назначения"), destination_city, destination_id, true);
    const auto* destination = find_company(destination_id);
    if (destination && !Accepts(*destination, cargo_id)) destination_id.clear();
    ImGui::TextWrapped("%s", tr("Trailer compatibility and routes are not checked yet. Job creation is not connected.",
                              "Совместимость прицепа и маршрут пока не проверяются. Создание заказа ещё не подключено."));
    ImGui::BeginDisabled(); ImGui::Button(tr("Create job", "Создать заказ")); ImGui::EndDisabled();
}
}
