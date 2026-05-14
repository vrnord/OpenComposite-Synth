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

	// See IVRCompositorExt_001.h for parameter contract. This stub validates
	// inputs, logs, and returns success. Future commits will route the
	// submission through a dedicated synth thread to OpenXR xrEndFrame with
	// explicit displayTime.
	//
	// Return type is the interface-version-scoped enum (matches the
	// codegen-generated proxy class's signature). The codegen also generates
	// a cast at the proxy call site for vr::namespace types, so this
	// strictly only needs to be implicit-convertible — but using the exact
	// type keeps the API clean.
	vr::IVRCompositorExt_001::EVRCompositorError SubmitInterpolatedFrame(
		const vr::Texture_t * synthTexture,
		const vr::VRTextureBounds_t * boundsLeft,
		const vr::VRTextureBounds_t * boundsRight,
		const vr::HmdMatrix34_t * pose,
		double displayTimeOffsetSeconds
	);

private:
	// Call counter so we can verify the call site is being hit without
	// spamming once-per-frame log lines.
	uint64_t callCount = 0;
};
