#pragma once
#include <filesystem>
namespace control_menu {
void Initialize(const std::filesystem::path& directory);
// Returns queued action: 1 cargo, 2 freight. Actions execute on the game thread.
int Tick(bool driving);
void CancelPending();
void Shutdown();
}
