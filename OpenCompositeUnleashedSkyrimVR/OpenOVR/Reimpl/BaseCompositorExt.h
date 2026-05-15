#pragma once
#include "../BaseCommon.h"
#include "custom_interfaces/IVRCompositorExt_001.h"

#include <cstdint>

// Base implementation for IVRCompositorExt_001 (CUSTOM-flavored OpenVR
// interface). The codegen produces a CVRCompositorExt_001 proxy class that
// inherits from vr::IVRCompositorExt_001::IVRCompositorExt + CVRCommon and
// forwards method calls to a shared_ptr<BaseCompositorExt> instance.
//
// Methods on Base* are non-virtual by convention — see BaseExtendedDisplay.h
// for the canonical minimal example.
class BaseCompositorExt {
public:
	BaseCompositorExt() = default;
	~BaseCompositorExt() = default;

	// Called once per SubmitInterpolatedFrame from CS-Fork's hook. In the
	// default (multi-threaded) path, builds a SynthRequest and enqueues it
	// on the synth thread's work queue. In the fallback (single-threaded)
	// path — selected by env var OPENCOMPOSITE_SYNTH_FALLBACK_SINGLETHREAD=1
	// at thread start time — runs the OpenXR frame cycle synchronously on
	// the calling (engine) thread.
	vr::IVRCompositorExt_001::EVRCompositorError SubmitInterpolatedFrame(
		const vr::Texture_t * synthTexture,
		const vr::VRTextureBounds_t * boundsLeft,
		const vr::VRTextureBounds_t * boundsRight,
		const vr::HmdMatrix34_t * pose,
		double displayTimeOffsetSeconds
	);

	// Lifecycle hooks called from XrBackend::OnSessionCreated and
	// PrepareForSessionShutdown. Idempotent — safe to call repeatedly.
	// Static because the thread state is per-process, not per-instance
	// (OpenXR has one session per process).
	static void StartSynthThread();
	static void StopSynthThread();

private:
	// Call counter so we can verify the call site is being hit without
	// spamming once-per-frame log lines. Also used as the SynthRequest
	// sequence number.
	uint64_t callCount = 0;
};
