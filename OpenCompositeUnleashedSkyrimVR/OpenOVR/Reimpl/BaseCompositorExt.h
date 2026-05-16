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
	// path — selected by opencomposite.ini setting `synthFallbackSingleThread=true`
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

	// Called by XrBackend::SubmitFrames each time engine commits a frame
	// via xrEndFrame. Records the engine's most recent displayTime so the
	// synth thread can target its own submissions BETWEEN engine's frames.
	// Idempotent and lock-free (single writer, single reader, atomic store).
	static void RecordEngineFrameSubmit(int64_t engineDisplayTime);

	// Set the predicted frame interval in nanoseconds. Called once when
	// session info is available (typically auto-estimated by
	// RecordEngineFrameSubmit after two valid samples). Synth thread uses
	// half of this value as its offset from engineDisplayTime.
	static void SetFrameIntervalNs(int64_t frameIntervalNs);

private:
	// Call counter so we can verify the call site is being hit without
	// spamming once-per-frame log lines. Also used as the SynthRequest
	// sequence number.
	uint64_t callCount = 0;
};
