#pragma once
#include "game_resolver.h"
#include <memory>
#include <string>
#include <vector>
namespace coaches {
inline constexpr std::size_t MaxPoints=24;
struct Stop {std::string id,name;};
struct View {
    bool available=false,loaded=false,scanned=false;
    std::uint64_t session=0;
    std::vector<Stop> stops;
    std::string message;
#ifdef CARGO_CONTROL_CATALOG_PREVIEW
    std::vector<std::string> preview_route;
#endif
};
struct Request {std::uint64_t session=0;std::vector<std::string> stops;};
void Initialize(std::uintptr_t base,std::size_t size,GameKind game,const GameProfile& profile,
                const CreationCode& allocation,void(*logger)(const char*));
bool Visible();
std::shared_ptr<const View> Snapshot();
void Observe(std::uintptr_t economy,bool ready);
void Reset();
void Scan(std::uintptr_t economy);
bool Queue(const Request& request);
void Create(std::uintptr_t economy);
void Refresh(std::uintptr_t economy);
void Tick(std::uintptr_t economy);
bool Busy();
void Draw(float scale,bool russian);
#ifdef CARGO_CONTROL_CATALOG_PREVIEW
void Preview(View fixture);
#endif
}
