#pragma once
#include "company_list.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <vector>

struct GameOfferVector {
    std::uintptr_t vtable;
    std::uintptr_t* data;
    std::uint64_t count, capacity;
};
static_assert(sizeof(GameOfferVector) == 32);

inline bool ValidOfferVector(const GameOfferVector& offers) {
    return offers.count <= offers.capacity && offers.count <= 100000 &&
           (!offers.capacity || offers.data) &&
           (!offers.count || ReadableMemory(reinterpret_cast<std::uintptr_t>(offers.data),
                                           offers.count * sizeof(std::uintptr_t)));
}

template<class Release>
void RemoveExpiredOffers(GameOfferVector& offers, std::uint32_t now, Release release) {
    std::vector<std::uintptr_t> expired;
    std::uint64_t kept = 0;
    for (std::uint64_t i = 0; i < offers.count; ++i) {
        const auto offer = offers.data[i];
        if (offer && *reinterpret_cast<const std::uint32_t*>(offer + 0x48) > now) {
            offers.data[kept++] = offer;
        } else if (offer) {
            expired.push_back(offer);
        }
    }
    for (auto i = kept; i < offers.count; ++i) offers.data[i] = 0;
    offers.count = kept;
    if (!expired.empty()) {
        GameOfferVector removed{offers.vtable, expired.data(), expired.size(), expired.size()};
        release(removed);
    }
}

template<class Generate, class Release>
void RefillEmptyOwnedOffers(GameOfferVector& offers, std::uint32_t now, Generate generate, Release release) {
    RemoveExpiredOffers(offers, now, release);
    if (!offers.count) generate();
}

// One automatic attempt; manual calls use at most the number of offers still needed.
inline std::uint32_t ManualAttemptBudget(std::uint64_t added, std::uint64_t timers, bool special_first_slot = false) {
    // Reserve slot zero when it can repeat the fixed special offer. Even when only
    // one new offer is missing, reach ordinary slot one. Pruning caps unique additions at five.
    return added >= 5 ? 0 : static_cast<std::uint32_t>(std::min<std::uint64_t>(5 - added + unsigned(special_first_slot), timers));
}
inline std::uint32_t ManualTimerSeed(std::uint32_t deadline, std::uint32_t seed, std::uint32_t slot) {
    // Distinct inputs for each native slot, including the regular slots after the special first slot.
    return deadline + 1u + (std::uint64_t(seed) + std::uint64_t(slot) * 104729u) % 1000000u;
}
class ManualGenerationScope {
public:
    ManualGenerationScope(bool& manual, std::uint32_t& budget, std::uint32_t attempts)
        : manual_(manual), budget_(budget), previous_manual_(manual), previous_budget_(budget) {
        manual_ = true; budget_ = attempts;
    }
    ~ManualGenerationScope() { manual_ = previous_manual_; budget_ = previous_budget_; }
    ManualGenerationScope(const ManualGenerationScope&) = delete;
    ManualGenerationScope& operator=(const ManualGenerationScope&) = delete;
private:
    bool& manual_;
    std::uint32_t& budget_;
    bool previous_manual_;
    std::uint32_t previous_budget_;
};

// Called in place of: mov qword ptr [rsp + slot], immediate32.
// CALL and PUSH add 16 bytes to the original stack displacement. MOV/PUSH/POP/RET preserve flags.
inline std::array<unsigned char, 23> MakeAttemptBudgetStub(std::uintptr_t budget_address, unsigned slot) {
    std::array<unsigned char, 23> code{
        0x50,                                      // push rax
        0x48,0xb8,0,0,0,0,0,0,0,0,                // mov rax, &budget
        0x8b,0x00,                                 // mov eax, [rax] (zero-extends)
        0x48,0x89,0x84,0x24,0,0,0,0,              // mov [rsp + slot + 16], rax
        0x58,0xc3                                  // pop rax; ret
    };
    const std::uint32_t displacement = slot + 16;
    std::memcpy(code.data()+3, &budget_address, sizeof(budget_address));
    std::memcpy(code.data()+17, &displacement, sizeof(displacement));
    return code;
}
// The one-byte PUSH RAX prologue allocates eight bytes. RAX is volatile during unwinding.
// UWOP_ALLOC_SMALL(8) restores RSP for exception handling and stack walking inside the stub.
inline constexpr std::array<unsigned char,8> AttemptBudgetUnwindInfo{1,1,1,0,1,2,0,0};

struct OfferIdentity {
    std::uintptr_t cargo = 0;
    std::string_view destination;
    bool operator==(const OfferIdentity& other) const {
        return cargo == other.cargo && destination == other.destination;
    }
};
inline bool ReadOfferIdentity(std::uintptr_t offer, OfferIdentity& identity) {
    identity = {};
    if (!ReadableMemory(offer, 0x60)) return false;
    std::uintptr_t cargo = 0, text = 0;
    std::uint32_t length = 0;
    std::memcpy(&cargo, reinterpret_cast<const void*>(offer+0x58), sizeof(cargo));
    std::memcpy(&text, reinterpret_cast<const void*>(offer+0x18), sizeof(text));
    std::memcpy(&length, reinterpret_cast<const void*>(offer+0x20), sizeof(length));
    if (!cargo || length > 4096 || !ReadableMemory(text, std::size_t(length)+1)) return false;
    const auto* characters = reinterpret_cast<const char*>(text);
    if (characters[length] || std::memchr(characters, 0, length)) return false;
    identity = {cargo, std::string_view(characters, length)};
    return true;
}

// Match the native ordinary generator's duplicate key: cargo pointer + destination string.
// The special first-slot path does not perform that check. Keep every pre-existing offer.
struct NewOfferCleanup { std::uint64_t duplicates = 0, excess = 0; };
template<class Release> NewOfferCleanup PruneNewOffers(GameOfferVector& offers,
                                                     std::uint64_t preserved, Release release) {
    if (!offers.count || preserved > offers.count || !ValidOfferVector(offers)) return {};
    NewOfferCleanup result;
    std::vector<OfferIdentity> identities;
    std::vector<std::uintptr_t> removed;
    for (std::uint64_t i = 0; i < preserved; ++i) {
        OfferIdentity identity;
        if (ReadOfferIdentity(offers.data[i], identity)) identities.push_back(identity);
    }
    auto retained = preserved;
    const auto original_count = offers.count;
    for (auto i = preserved; i < original_count; ++i) {
        const auto offer = offers.data[i];
        OfferIdentity identity;
        const bool known = ReadOfferIdentity(offer, identity);
        if (known && std::find(identities.begin(), identities.end(), identity) != identities.end()) {
            removed.push_back(offer); ++result.duplicates;
        } else if (retained - preserved >= 5) {
            removed.push_back(offer); ++result.excess;
        }
        else {
            offers.data[retained++] = offer;
            if (known) identities.push_back(identity);
        }
    }
    std::fill(offers.data+retained, offers.data+original_count, 0);
    offers.count = retained;
    if (!removed.empty()) {
        GameOfferVector duplicates{offers.vtable, removed.data(), removed.size(), removed.size()};
        release(duplicates); // Use the game's destructor/refcount path, once per removed reference.
    }
    return result;
}

template<class Generate, class Release>
NewOfferCleanup ReplaceOwnedOffers(GameOfferVector& offers, std::uint64_t timers, bool special_first_slot,
                                   Generate generate, Release release) {
    if (!timers || !ValidOfferVector(offers)) return {};
    if (offers.count) release(offers);

    NewOfferCleanup result;
    for (unsigned attempt = 0; attempt < 20 && offers.count < 5; ++attempt) {
        generate(ManualAttemptBudget(offers.count, timers, special_first_slot), attempt);
        const auto cleaned = PruneNewOffers(offers, 0, release);
        result.duplicates += cleaned.duplicates;
        result.excess += cleaned.excess;
    }
    return result;
}

struct SafeReplacement {NewOfferCleanup cleanup;bool committed=false;};
template<class Generate,class Release,class Reserve>
SafeReplacement ReplaceOwnedOffersSafely(GameOfferVector& offers,std::uint64_t timers,bool special,
                                         Generate generate,Release release,Reserve reserve){
    if(!timers || !ValidOfferVector(offers))return {};
    const auto previous=offers;
    std::array<std::uintptr_t,16> storage{};
    offers={previous.vtable,storage.data(),0,storage.size()};
    struct Restore {GameOfferVector& target;GameOfferVector previous;bool restored=false;
        void Now(){if(!restored){target=previous;restored=true;}}
        ~Restore(){Now();}
    } restore{offers,previous};
    const auto cleanup=ReplaceOwnedOffers(offers,timers,special,generate,release);
    auto generated=offers;
    restore.Now();
    if(!generated.count)return {cleanup,false};
    if(!reserve(offers,generated.count)){release(generated);return {cleanup,false};}
    release(offers);
    std::copy_n(generated.data,generated.count,offers.data);offers.count=generated.count;
    return {cleanup,true};
}
