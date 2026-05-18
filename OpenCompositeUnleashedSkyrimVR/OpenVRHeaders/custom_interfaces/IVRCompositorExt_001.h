#pragma once
#include "generated/interfaces/openvr.h"
#include "generated/interfaces/vrtypes.h"
#include "generated/interfaces/vrannotation.h"

// IVRCompositorExt_001 — VRNord/CS-Fork extension to OpenComposite providing
// synth frame submission for ASW-style frame interpolation between engine
// real frames.
//
// Discovered by client applications via vr::VR_GetGenericInterface using
// the version string "IVRCompositorExt_001". Returns nullptr on stock
// SteamVR or upstream OpenComposite — clients gracefully degrade to
// no-synth-submit.

namespace vr
{
namespace IVRCompositorExt_001
{

// OpenVR convention: each interface version redeclares EVRCompositorError so
// Valve can change values across versions. Matches IVRCompositor_028's values
// for the subset we care about; only None / InvalidTexture / RequestFailed
// are actually returned by SubmitInterpolatedFrame.
enum EVRCompositorError
{
	VRCompositorError_None                          = 0,
	VRCompositorError_RequestFailed                 = 1,
	VRCompositorError_IncompatibleVersion           = 100,
	VRCompositorError_DoNotHaveFocus                = 101,
	VRCompositorError_InvalidTexture                = 102,
	VRCompositorError_IsNotSceneApplication         = 103,
	VRCompositorError_TextureIsOnWrongDevice        = 104,
	VRCompositorError_TextureUsesUnsupportedFormat  = 105,
	VRCompositorError_SharedTexturesNotSupported    = 106,
	VRCompositorError_IndexOutOfRange               = 107,
	VRCompositorError_AlreadySubmitted              = 108,
	VRCompositorError_InvalidBounds                 = 109,
	VRCompositorError_AlreadySet                    = 110,
};

class IVRCompositorExt
{
public:
	// Submit a synth (interpolated) frame for display at a specific time
	// relative to the next predicted real-frame display.
	//
	// Parameters:
	//   synthTexture: SBS-packed synth texture (both eyes in one D3D11
	//                 texture). Must be a TextureType_DirectX texture.
	//   boundsLeft:   UV bounds for the left eye (typically {0.0, 0.0, 0.5, 1.0}).
	//   boundsRight:  UV bounds for the right eye (typically {0.5, 0.0, 1.0, 1.0}).
	//   pose:         The HMD pose at which the synth content was rendered
	//                 (in absolute tracking space). Used for runtime warp
	//                 to display pose.
	//   displayTimeOffsetSeconds: When to display this frame, relative to
	//                 the next predicted real-frame display time. Negative
	//                 for "before next real frame", positive for after.
	//                 Typical value: -frameInterval/2 (about -0.0055 at 90Hz).
	//
	// Returns:
	//   VRCompositorError_None on success
	//   VRCompositorError_InvalidTexture if texture is null or wrong type
	//   VRCompositorError_RequestFailed if synth submission is not active
	//
	// NOTE: declared on one line — the codegen parser (scripts/libparse.py)
	// requires `virtual TYPE NAME(ARGS);` on a single line. nice_lines
	// joins continuations only on trailing `,` or `\\`, so a `(`-trailing
	// line wouldn't get joined to the args lines below it. Keep this in
	// one line to ensure the codegen extracts SubmitInterpolatedFrame.
	virtual EVRCompositorError SubmitInterpolatedFrame(const Texture_t * synthTexture, const VRTextureBounds_t * boundsLeft, const VRTextureBounds_t * boundsRight, const HmdMatrix34_t * pose, double displayTimeOffsetSeconds) = 0;

	// Phase C2.8c dual-cycle: close the synth cycle that was opened in
	// WaitForTrackingData (copying the given synth texture into the
	// synth swapchain via per-eye bounds), then open engine's cycle so
	// engine can continue its normal render path. Called from CS-Fork's
	// H4 hook after H1+H4 dispatches have produced synth content.
	// Must be on ONE LINE — codegen parser drops continuations across `(`.
	virtual void SubmitSynthAndOpenEngineCycle(const Texture_t * synthTexture, const VRTextureBounds_t * boundsLeft, const VRTextureBounds_t * boundsRight) = 0;
};

static const char * const IVRCompositorExt_Version = "IVRCompositorExt_001";

} // namespace IVRCompositorExt_001

} // Close custom namespace
