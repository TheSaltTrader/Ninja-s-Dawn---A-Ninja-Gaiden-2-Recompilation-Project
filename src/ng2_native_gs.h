/**
 * @file        ng2_native_gs.h
 * @brief       FULL NATIVE, P5 step 2 (2026-09-27): the game's own graphics system, in place of rexgpu-xenos.dll's.
 *
 *              The runtime asks the app for an IGraphicsSystem (ReXApp::OnPreSetup may supply one). This one owns
 *              everything the plugin's graphics system owned, without a Xenos command processor:
 *                - presentation: a D3D12 provider and the runtime's presenter (window, swap chain, ImGui overlays);
 *                - the guest GPU's register window (MMIO 0x7FC80000): reads as the plugin answered them, the command
 *                  ring's write pointer (CP_RB_WPTR) handed straight to the native front end on the kicking thread;
 *                - an executor XThread that runs the side-effect batches the front end produces (fences, the read
 *                  pointer, interrupts, memory waits, register writes, the swap count) - the interrupt context;
 *                - the vblank worker (counter + interrupt 0).
 *              The native backend runs on this provider's device and direct queue and presents into this presenter
 *              from the guest thread at each swap (the bridge). Switch: NG2_NATIVE_GS=1 (implies the native front
 *              end). The plugin DLL is still loaded as a library so the settings it defines stay registered.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

struct ID3D12Device;
struct ID3D12CommandQueue;

namespace rex::system {
class IGraphicsSystem;
}
namespace rex::ui {
class Presenter;
}

namespace ng2::gs {

// NG2_NATIVE_GS=1 in the environment (read once).
bool Requested();
// The game's graphics system; nullptr if D3D12 is unavailable.
std::unique_ptr<rex::system::IGraphicsSystem> Create();
// True once Create() returned a system (the bridge and the front end switch on it).
bool Active();
// Valid after SetupPresentation.
ID3D12Device* Device();
ID3D12CommandQueue* Queue();
rex::ui::Presenter* Presenter();
// The front end's side-effect batch for this kick (records of 4 dwords, as the plugin executor's).
void PushSideEffects(const uint32_t* recs, uint32_t count4);
// The pipeline storage the runtime handed the graphics system (false until it did).
bool ShaderStorage(std::filesystem::path& cache_root, uint32_t& title_id);

}  // namespace ng2::gs
