#pragma once
#include "custom_job_request.h"
#include "game_resolver.h"

namespace custom_jobs::creation {
using OwnedCallback=void(*)(std::uintptr_t,std::uintptr_t);
struct Backend { OwnedCallback prepare=nullptr, generate=nullptr; };
using Logger=void(*)(const char*);
bool Initialize(std::uintptr_t module,const GameProfile& core,const CreationCode& code,Backend owned,Logger logger);
void Shutdown();
CreateResult Execute(std::uintptr_t economy,std::uintptr_t pools,const JobRequest& request);
}
