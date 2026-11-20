#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace custom_jobs {
using Reader = std::function<bool(std::uintptr_t, void*, std::size_t)>;
struct Cargo { std::string id, name; };
struct Company { std::string id, city, name; std::vector<std::string> outgoing, incoming; std::string city_name; };
enum class LabelKind { Cargo, Company, City };
using LabelReader = std::function<std::string(LabelKind, std::uintptr_t)>;
struct Catalog {
    std::vector<Company> companies;
    std::vector<Cargo> cargos;
    std::uint64_t skipped = 0;
    std::uint64_t label_fallbacks = 0;
};
template<class T> bool Read(const Reader& reader, std::uintptr_t address, T& value) {
    value = {};
    return address && address <= UINTPTR_MAX - sizeof(T) && reader(address, &value, sizeof(T));
}
inline std::string DecodeToken(std::uint64_t token) {
    constexpr char alphabet[] = "\0" "0123456789abcdefghijklmnopqrstuvwxyz_";
    std::string text;
    while (token && text.size() < 13) {
        const auto digit = token % 38;
        if (!digit) return {};
        text += alphabet[digit]; token /= 38;
    }
    return token ? std::string{} : text;
}
inline bool LiveUnit(const Reader& reader, std::uintptr_t unit) {
    std::uint32_t flags = 0;
    return unit && unit <= UINTPTR_MAX - 0x10 && Read(reader, unit + 8, flags) && (flags & 0x80000000u);
}
// Read the same registered-unit token pool used by the native unit-name code.
// Anonymous unit names require another representation and are rejected here.
inline bool UnitName(const Reader& reader, std::uintptr_t pools, std::uintptr_t unit,
                     std::vector<std::string>& parts) {
    parts.clear();
    std::uint32_t id = 0;
    std::uintptr_t pool = 0, tokens = 0;
    std::uint8_t width = 0;
    if (!pools || pools > UINTPTR_MAX - 2048 * 8 ||
        !LiveUnit(reader, unit) || !Read(reader, unit + 0xc, id) || !id ||
        !Read(reader, pools + std::uintptr_t((id >> 20) & 0x7ff) * 8, pool) ||
        !pool || pool > UINTPTR_MAX - 0x89 || !Read(reader, pool + 0x88, width) ||
        !width || width > 16 || !Read(reader, pool, tokens)) return false;
    const auto offset = std::uintptr_t(id & 0xffff) * width * 8;
    if (!tokens || tokens > UINTPTR_MAX - offset - std::uintptr_t(width) * 8) return false;
    for (unsigned i = 0; i < width; ++i) {
        std::uint64_t token = 0;
        if (!Read(reader, tokens + offset + i * 8, token)) return false;
        auto text = DecodeToken(token);
        if (text.empty()) return false;
        parts.push_back(std::move(text));
    }
    return true;
}
inline std::string Join(const std::vector<std::string>& parts) {
    std::string id;
    for (const auto& part : parts) { if (!id.empty()) id += '.'; id += part; }
    return id;
}
inline bool PointerList(const Reader& reader, std::uintptr_t object, unsigned data_offset,
                        std::uint64_t limit, std::vector<std::uintptr_t>& values) {
    values.clear();
    std::uintptr_t data = 0;
    std::uint64_t count = 0;
    if (!object || object > UINTPTR_MAX - data_offset - 16 ||
        !Read(reader, object + data_offset, data) ||
        !Read(reader, object + data_offset + 8, count) || count > limit ||
        (count && (!data || data > UINTPTR_MAX - count * 8))) return false;
    values.resize(static_cast<std::size_t>(count));
    return !count || reader(data, values.data(), values.size() * sizeof(std::uintptr_t));
}
inline bool BuildCatalog(const Reader& reader, std::uintptr_t economy, std::uintptr_t pools,
                         Catalog& catalog, std::string& error, const LabelReader& labels = {}) {
    catalog = {}; error.clear();
    std::vector<std::uintptr_t> companies;
    if (!pools || pools > UINTPTR_MAX - 2048 * 8 ||
        !PointerList(reader, economy, 0x28, 1000000, companies) || companies.empty()) {
        error = "Company list is unavailable."; return false;
    }
    Catalog result;
    std::map<std::string, Cargo> cargos;
    std::map<std::string, Company> unique;
    std::vector<std::string> parts;
    std::vector<std::uintptr_t> cargo_pointers;
    std::uint64_t budget = 2000000;
    for (const auto company : companies) {
        Company entry;
        std::map<std::string, Cargo> local_cargos;
        std::uintptr_t context = 0, definition = 0;
        if (!company || company > UINTPTR_MAX - 0x70 ||
            !Read(reader, company + 0x10, definition) || !definition ||
            !Read(reader, company + 0x68, context) || !LiveUnit(reader, context) ||
            !UnitName(reader, pools, company, parts) || parts.size() != 4 ||
            parts[0] != "company" || parts[1] != "volatile") { ++result.skipped; continue; }
        entry.id = Join(parts); entry.city = parts[3]; entry.name = parts[2];
        entry.city_name = entry.city;
        if (labels) {
            auto company_name = labels(LabelKind::Company, context);
            auto city_name = labels(LabelKind::City, company);
            if (!company_name.empty()) entry.name = std::move(company_name); else ++result.label_fallbacks;
            if (!city_name.empty()) entry.city_name = std::move(city_name); else ++result.label_fallbacks;
        }
        bool valid = true;
        for (const unsigned offset : {0xa0u, 0xc8u}) {
            if (!PointerList(reader, context, offset, 100000, cargo_pointers)) { valid = false; break; }
            if (cargo_pointers.size() > budget) { error = "Cargo list exceeds the scan budget."; return false; }
            budget -= cargo_pointers.size();
            auto& list = offset == 0xa0 ? entry.outgoing : entry.incoming;
            for (const auto cargo : cargo_pointers) {
                if (!UnitName(reader, pools, cargo, parts) || parts.size() != 2 || parts[0] != "cargo") {
                    ++result.skipped; continue;
                }
                auto id = Join(parts);
                auto name = labels ? labels(LabelKind::Cargo, cargo) : std::string{};
                if (name.empty()) { name = parts.back(); if (labels) ++result.label_fallbacks; }
                local_cargos.emplace(id, Cargo{id, std::move(name)}); list.push_back(std::move(id));
            }
            std::sort(list.begin(), list.end()); list.erase(std::unique(list.begin(), list.end()), list.end());
        }
        if (!valid) { ++result.skipped; continue; }
        cargos.insert(local_cargos.begin(), local_cargos.end());
        unique.emplace(entry.id, std::move(entry));
    }
    if (unique.empty()) { error = "No named companies could be read from the loaded session."; return false; }
    for (auto& item : unique) result.companies.push_back(std::move(item.second));
    for (auto& item : cargos) result.cargos.push_back(std::move(item.second));
    catalog = std::move(result); return true;
}
inline bool Accepts(const Company& destination, const std::string& cargo) {
    return std::find(destination.incoming.begin(), destination.incoming.end(), cargo) != destination.incoming.end();
}
}
