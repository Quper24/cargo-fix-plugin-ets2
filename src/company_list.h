#pragma once
#include <Windows.h>
#include <cstdint>

// Global map company lists can exceed 10,000 entries on map combinations.
// Keep a corruption guard, and verify the backing memory before traversal.
inline bool ReadableMemory(std::uintptr_t data, std::uintptr_t bytes)
{
    if (!data || !bytes || data > UINTPTR_MAX - bytes) return false;
    const auto end = data + bytes;
    while (data < end) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(data), &region, sizeof(region)) ||
            region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD)) return false;
        const auto protection = region.Protect & 0xff;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
            protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        const auto start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (start > UINTPTR_MAX - region.RegionSize) return false;
        const auto next = start + region.RegionSize;
        if (next <= data) return false;
        data = next;
    }
    return true;
}
inline bool ValidCompanyList(std::uintptr_t data, std::uint64_t count)
{
    return count && count <= 1000000 && ReadableMemory(data, static_cast<std::uintptr_t>(count * sizeof(std::uintptr_t)));
}
