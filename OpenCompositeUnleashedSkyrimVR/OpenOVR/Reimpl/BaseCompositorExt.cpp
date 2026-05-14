#include "stdafx.h"
#include "BaseCompositorExt.h"

vr::IVRCompositorExt_001::EVRCompositorError BaseCompositorExt::SubmitInterpolatedFrame(
	const vr::Texture_t * synthTexture,
	const vr::VRTextureBounds_t * boundsLeft,
	const vr::VRTextureBounds_t * boundsRight,
	const vr::HmdMatrix34_t * pose,
	double displayTimeOffsetSeconds)
{
	(void)pose;  // Silence unused-param until synth thread consumes pose for runtime warp.

	callCount++;

	// Log first few calls + every 300 thereafter to avoid log spam.
	const bool shouldLog = (callCount <= 5) || (callCount % 300 == 0);

	if (shouldLog) {
		OOVR_LOGF("BaseCompositorExt::SubmitInterpolatedFrame call #%llu",
		    (unsigned long long)callCount);
		if (synthTexture) {
			OOVR_LOGF("  texture handle=%p type=%d colorSpace=%d",
			    synthTexture->handle, (int)synthTexture->eType,
			    (int)synthTexture->eColorSpace);
		} else {
			OOVR_LOG("  texture: NULL");
		}
		if (boundsLeft) {
			OOVR_LOGF("  boundsLeft: u[%.3f..%.3f] v[%.3f..%.3f]",
			    boundsLeft->uMin, boundsLeft->uMax,
			    boundsLeft->vMin, boundsLeft->vMax);
		}
		if (boundsRight) {
			OOVR_LOGF("  boundsRight: u[%.3f..%.3f] v[%.3f..%.3f]",
			    boundsRight->uMin, boundsRight->uMax,
			    boundsRight->vMin, boundsRight->vMax);
		}
		OOVR_LOGF("  displayTimeOffsetSeconds: %.6f", displayTimeOffsetSeconds);
	}

	// Stub: validate inputs, return success.
	if (!synthTexture || !synthTexture->handle) {
		return vr::IVRCompositorExt_001::VRCompositorError_InvalidTexture;
	}
	if (synthTexture->eType != vr::TextureType_DirectX) {
		return vr::IVRCompositorExt_001::VRCompositorError_InvalidTexture;
	}

	// Future: enqueue submission request for the synth thread.
	return vr::IVRCompositorExt_001::VRCompositorError_None;
}
