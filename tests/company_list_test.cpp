#include "company_list.h"
#include <cstdlib>
#include <iostream>
#include <vector>

void Require(bool value) {
    if (!value) { std::cerr << "Company list validation failed\n"; std::exit(1); }
}
int main() {
    std::vector<std::uintptr_t> companies(25000);
    const auto data = reinterpret_cast<std::uintptr_t>(companies.data());
    Require(ValidCompanyList(data, 10000));
    Require(ValidCompanyList(data, 10001));
    Require(ValidCompanyList(data, companies.size()));
    Require(!ValidCompanyList(0, 1));
    Require(!ValidCompanyList(data, 0));
    Require(!ValidCompanyList(data, 1000001));
    Require(!ValidCompanyList(UINTPTR_MAX - 4, 2));
    auto* pages = static_cast<unsigned char*>(VirtualAlloc(nullptr, 131072,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Require(pages != nullptr);
    SYSTEM_INFO info{}; GetSystemInfo(&info);
    const auto page_size = info.dwPageSize;
    Require(ValidCompanyList(reinterpret_cast<std::uintptr_t>(pages), 2 * page_size / sizeof(std::uintptr_t)));
    DWORD old = 0;
    Require(VirtualProtect(pages + page_size, page_size, PAGE_NOACCESS, &old) != 0);
    Require(!ValidCompanyList(reinterpret_cast<std::uintptr_t>(pages), 2 * page_size / sizeof(std::uintptr_t)));
    Require(VirtualProtect(pages + page_size, page_size, PAGE_READWRITE | PAGE_GUARD, &old) != 0);
    Require(!ValidCompanyList(reinterpret_cast<std::uintptr_t>(pages), 2 * page_size / sizeof(std::uintptr_t)));
    VirtualFree(pages, 0, MEM_RELEASE);
}
