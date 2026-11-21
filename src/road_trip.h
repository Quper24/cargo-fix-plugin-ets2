#pragma once
#include "game_resolver.h"
#include <memory>
#include <string>
#include <vector>
namespace road_trip {
struct Point {std::string id,city,city_name,name;};
struct Task {std::string id,name;std::vector<std::string> sources,targets;};
enum class SessionStatus { NotLoaded, ContentUnavailable, Ready };
struct View {
    bool available=false,scanned=false,own_car=false;
    SessionStatus status=SessionStatus::NotLoaded;
    std::uint64_t session=0;
    std::vector<Point> points;
    std::vector<Task> tasks;
    std::string message;
};
struct Request {std::uint64_t session=0;std::string task,source,target;bool replace=false;};
using Logger=void(*)(const char*);
void Initialize(std::uintptr_t base,std::size_t size,GameKind game,const GameProfile& profile,const CreationCode& allocation,Logger logger);
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
