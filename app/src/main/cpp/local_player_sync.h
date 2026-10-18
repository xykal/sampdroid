// Safe bridge from authoritative local-player reducer state to a prospective
// client-originated PlayerFootSync payload. It is renderer-neutral and does not
// transmit packets on its own.
#pragma once

#include "client_sync.h"
#include "input_state.h"
#include "rpc_state.h"

namespace samp::bridge {

enum class OnFootPreviewStatus {
  Ready,
  MissingPosition,
  MissingHeading,
  InvalidVitals,
  InvalidState,
};

struct OnFootPreview {
  OnFootPreviewStatus status = OnFootPreviewStatus::MissingPosition;
  sync::OnFootState state;

  bool ready() const { return status == OnFootPreviewStatus::Ready; }
};

// Builds a local-only preview from server-reduced state and native semantic
// controls. It never invents position/collision movement, active weapon,
// surfing, or animation state. Those require the renderer/game bridge later.
OnFootPreview buildOnFootPreview(const state::LocalPlayer& local,
                                 const input::WireInput& controls);

const char* onFootPreviewStatusName(OnFootPreviewStatus status);

}  // namespace samp::bridge
