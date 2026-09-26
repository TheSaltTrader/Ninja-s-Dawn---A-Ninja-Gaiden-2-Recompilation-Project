#pragma once
// BACKEND TRANSPLANT (2026-09-26): NATIVE stand-in for the plugin's GraphicsSystem / D3D12GraphicsSystem - only what
// the vendored CommandProcessor base and D3D12CommandProcessor touch. The native renderer owns presentation, so
// presenter() is null (IssueSwap's presenter path returns early; the replay driver takes the swap texture itself).
#include <cstdint>
#include <functional>

#include <rex/graphics/register_file.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ui/presenter.h>

#include "rtc_d3d12/facade.h"

namespace rex::system { class KernelState; }

namespace rex::graphics {

class CommandProcessor;

class GraphicsSystem {
 public:
  GraphicsSystem(memory::Memory* memory, system::KernelState* kernel_state, RegisterFile* register_file)
      : memory_(memory), kernel_state_(kernel_state), register_file_(register_file) {}
  virtual ~GraphicsSystem() = default;

  memory::Memory* memory() const { return memory_; }
  system::KernelState* kernel_state() const { return kernel_state_; }
  RegisterFile* register_file() { return register_file_; }
  // The native D3D12 provider (plume's device and queue). The plugin's code static_casts provider() to its D3D12
  // provider type; here it already is one.
  ::rex::ui::ngpu_d3d12::D3D12Provider* provider() const { return &::rex::ui::ngpu_d3d12::NativeProvider(); }
  ::rex::ui::Presenter* presenter() const { return nullptr; }
  bool has_presentation() const { return false; }

  // The guest's interrupt callback belongs to the PLUGIN's command processor (it drives the game); the native
  // replay never raises one.
  void DispatchInterruptCallback(uint32_t, uint32_t) {}
  void OnHostGpuLossFromAnyThread(bool is_responsible) {
    REXLOG_ERROR("[ngpu] BACKEND: host GPU loss reported by the transplanted command processor (responsible: {})", is_responsible);
  }

 private:
  memory::Memory* memory_;
  system::KernelState* kernel_state_;
  RegisterFile* register_file_;
};

}  // namespace rex::graphics

// The plugin's D3D12Presenter pieces the swap lambda uses (d3d12_presenter.h): the guest-output format / state and the
// refresh context carrying the UAV-capable output texture.
namespace rex::ui::ngpu_d3d12 {
class D3D12Presenter {
 public:
  static constexpr DXGI_FORMAT kGuestOutputFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
  static constexpr D3D12_RESOURCE_STATES kGuestOutputInternalState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  class D3D12GuestOutputRefreshContext final : public ::rex::ui::Presenter::GuestOutputRefreshContext {
   public:
    D3D12GuestOutputRefreshContext(bool& is_8bpc_out_ref, ID3D12Resource* resource)
        : GuestOutputRefreshContext(is_8bpc_out_ref), resource_(resource) {}
    ID3D12Resource* resource_uav_capable() const { return resource_; }
   private:
    ID3D12Resource* resource_;
  };
};
}  // namespace rex::ui::ngpu_d3d12

namespace ng2::ngpu::rtc {
// Stands in for Presenter::RefreshGuestOutput: provides a native guest-output texture (kGuestOutputFormat, UAV, in
// kGuestOutputInternalState) of the requested size and runs the plugin's refresher on it.
bool NativeRefreshGuestOutput(uint32_t frontbuffer_width, uint32_t frontbuffer_height, uint32_t display_width,
                              uint32_t display_height,
                              const std::function<bool(::rex::ui::Presenter::GuestOutputRefreshContext& context)>& refresher);
}  // namespace ng2::ngpu::rtc

namespace rex::graphics::ngpu_d3d12 {
class D3D12GraphicsSystem : public GraphicsSystem {
 public:
  using GraphicsSystem::GraphicsSystem;
};
}  // namespace rex::graphics::ngpu_d3d12
