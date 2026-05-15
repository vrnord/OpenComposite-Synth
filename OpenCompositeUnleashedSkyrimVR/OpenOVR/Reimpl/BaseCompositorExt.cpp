#include "stdafx.h"
#include "BaseCompositorExt.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// OpenComposite globals: xr_session, OpenXR primitives, oovr_global_configuration.
// OpenOVR/ is on the include path (see other Reimpl/*.cpp).
#include "Misc/Config.h"
#include "Misc/xrutil.h"

namespace {

// === Synth submission request ===
struct SynthRequest {
	uint64_t sequence = 0;
	void* textureHandle = nullptr;        // ID3D11Texture2D* (opaque here)
	vr::ETextureType textureType = vr::TextureType_DirectX;
	vr::EColorSpace colorSpace = vr::ColorSpace_Auto;
	vr::VRTextureBounds_t boundsLeft  = { 0.0f, 0.0f, 0.5f, 1.0f };
	vr::VRTextureBounds_t boundsRight = { 0.5f, 0.0f, 1.0f, 1.0f };
	vr::HmdMatrix34_t pose = {};
	double displayTimeOffsetSeconds = 0.0;
};

// === Thread state (per-process; OpenXR has one session per process) ===
std::mutex                  g_synthMutex;
std::condition_variable     g_synthCv;
std::deque<SynthRequest>    g_synthQueue;
std::thread                 g_synthThread;
std::atomic<bool>           g_synthShutdown{ false };
std::atomic<bool>           g_synthRunning{ false };
std::atomic<uint64_t>       g_synthEnqueueCount{ 0 };
std::atomic<uint64_t>       g_synthDequeueCount{ 0 };
std::atomic<uint64_t>       g_synthFrameCount{ 0 };
std::atomic<uint64_t>       g_synthWaitErrors{ 0 };
std::atomic<uint64_t>       g_synthBeginErrors{ 0 };
std::atomic<uint64_t>       g_synthEndErrors{ 0 };

// Fallback toggle: when true, SubmitInterpolatedFrame runs the OpenXR
// frame cycle synchronously on the calling (engine) thread instead of
// enqueueing for the synth thread. Read once at StartSynthThread time.
bool                        g_fallbackSingleThread = false;

// Queue cap to prevent unbounded growth if the synth thread can't keep up.
// Hard ceiling; new requests drop the oldest pending request.
constexpr size_t            kMaxQueueDepth = 4;

// Helper: should we log this iteration? Throttles to keep logs manageable.
bool ShouldLogRate(uint64_t count) {
	return (count <= 10) || (count % 300 == 0);
}

// Execute one synth frame cycle: xrWaitFrame → xrBeginFrame → xrEndFrame
// with layerCount=0. Called from either the synth thread (default) or
// the engine thread (fallback). Returns true if the cycle completed
// without aborting; failure modes are individually logged.
bool RunSynthFrameCycle(const SynthRequest& req) {
	// Acquire shared lock on the OpenXR session (matches the pattern at
	// XrBackend.cpp:394 — auto lock = xr_session.lock_shared()).
	auto lock = xr_session.lock_shared();
	XrSession session = xr_session.get();
	if (session == XR_NULL_HANDLE) {
		OOVR_LOGF("[SynthThread] cycle #%llu skipped: session is XR_NULL_HANDLE",
			(unsigned long long)req.sequence);
		return false;
	}

	const bool shouldLogThis = ShouldLogRate(req.sequence);

	// === xrWaitFrame ===
	XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState state{ XR_TYPE_FRAME_STATE };
	XrResult waitRes = xrWaitFrame(session, &waitInfo, &state);
	if (XR_FAILED(waitRes)) {
		g_synthWaitErrors.fetch_add(1);
		OOVR_LOGF("[SynthThread] xrWaitFrame failed: result=%d seq=%llu",
			(int)waitRes, (unsigned long long)req.sequence);
		return false;
	}
	if (shouldLogThis) {
		OOVR_LOGF("[SynthThread] cycle #%llu: xrWaitFrame ok, "
			"predictedDisplayTime=%lld shouldRender=%d",
			(unsigned long long)req.sequence,
			(long long)state.predictedDisplayTime,
			(int)state.shouldRender);
	}

	// Compute the target display time. Negative offset → earlier slot.
	XrTime targetTime = state.predictedDisplayTime
		+ (XrTime)(req.displayTimeOffsetSeconds * 1e9);

	// === xrBeginFrame ===
	XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
	XrResult beginRes = xrBeginFrame(session, &beginInfo);
	if (XR_FAILED(beginRes)) {
		g_synthBeginErrors.fetch_add(1);
		OOVR_LOGF("[SynthThread] xrBeginFrame failed: result=%d seq=%llu",
			(int)beginRes, (unsigned long long)req.sequence);
		return false;
	}

	// === xrEndFrame with layerCount=0 ===
	// Phase C2: no composition layers yet. This tests whether the runtime
	// accepts a frame with no content from a thread other than engine's.
	XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.displayTime = targetTime;
	endInfo.layerCount = 0;
	endInfo.layers = nullptr;
	XrResult endRes = xrEndFrame(session, &endInfo);
	if (XR_FAILED(endRes)) {
		g_synthEndErrors.fetch_add(1);
		OOVR_LOGF("[SynthThread] xrEndFrame failed: result=%d seq=%llu "
			"displayTime=%lld",
			(int)endRes, (unsigned long long)req.sequence,
			(long long)targetTime);
		return false;
	}
	if (shouldLogThis) {
		OOVR_LOGF("[SynthThread] cycle #%llu: xrEndFrame ok, "
			"displayTime=%lld (offset=%.6fs)",
			(unsigned long long)req.sequence, (long long)targetTime,
			req.displayTimeOffsetSeconds);
	}

	g_synthFrameCount.fetch_add(1);
	return true;
}

void SynthThreadFunc() {
	OOVR_LOG("[SynthThread] thread started");

	while (!g_synthShutdown.load()) {
		SynthRequest req;
		{
			std::unique_lock<std::mutex> lock(g_synthMutex);
			// Wait for a request or shutdown signal, with a 100ms timeout
			// so we periodically wake to re-check the shutdown flag.
			g_synthCv.wait_for(lock, std::chrono::milliseconds(100), []() {
				return g_synthShutdown.load() || !g_synthQueue.empty();
			});

			if (g_synthShutdown.load()) {
				break;
			}
			if (g_synthQueue.empty()) {
				continue;
			}

			req = g_synthQueue.front();
			g_synthQueue.pop_front();
		}

		uint64_t dq = g_synthDequeueCount.fetch_add(1) + 1;
		if (ShouldLogRate(dq)) {
			OOVR_LOGF("[SynthThread] dequeued request seq=%llu (total dq=%llu)",
				(unsigned long long)req.sequence,
				(unsigned long long)dq);
		}

		RunSynthFrameCycle(req);
	}

	OOVR_LOGF("[SynthThread] thread exiting. enq=%llu dq=%llu frames=%llu "
		"waitErr=%llu beginErr=%llu endErr=%llu",
		(unsigned long long)g_synthEnqueueCount.load(),
		(unsigned long long)g_synthDequeueCount.load(),
		(unsigned long long)g_synthFrameCount.load(),
		(unsigned long long)g_synthWaitErrors.load(),
		(unsigned long long)g_synthBeginErrors.load(),
		(unsigned long long)g_synthEndErrors.load());
}

} // anonymous namespace

// === Static lifecycle methods ===

void BaseCompositorExt::StartSynthThread() {
	if (g_synthRunning.load()) {
		OOVR_LOG("[SynthThread] StartSynthThread called but already running — ignoring");
		return;
	}

	// Read the fallback toggle from opencomposite.ini at thread start time.
	// Setting key: synthFallbackSingleThread (under default section).
	// See opencomposite.ini.example for documentation.
	g_fallbackSingleThread = oovr_global_configuration.SynthFallbackSingleThread();
	OOVR_LOGF("[SynthThread] StartSynthThread: fallback_singlethread=%d",
		(int)g_fallbackSingleThread);

	if (g_fallbackSingleThread) {
		OOVR_LOG("[SynthThread] FALLBACK MODE active — submissions will "
			"run synchronously on the calling thread. No worker thread spawned.");
		g_synthRunning.store(true);  // logical "running" — fallback path
		return;
	}

	g_synthShutdown.store(false);
	g_synthEnqueueCount.store(0);
	g_synthDequeueCount.store(0);
	g_synthFrameCount.store(0);
	g_synthWaitErrors.store(0);
	g_synthBeginErrors.store(0);
	g_synthEndErrors.store(0);
	{
		std::lock_guard<std::mutex> lock(g_synthMutex);
		g_synthQueue.clear();
	}

	g_synthThread = std::thread(SynthThreadFunc);
	g_synthRunning.store(true);
	OOVR_LOG("[SynthThread] StartSynthThread: worker thread spawned");
}

void BaseCompositorExt::StopSynthThread() {
	if (!g_synthRunning.load()) {
		OOVR_LOG("[SynthThread] StopSynthThread called but not running — ignoring");
		return;
	}

	OOVR_LOGF("[SynthThread] StopSynthThread: stopping. "
		"final stats: enq=%llu dq=%llu frames=%llu "
		"waitErr=%llu beginErr=%llu endErr=%llu",
		(unsigned long long)g_synthEnqueueCount.load(),
		(unsigned long long)g_synthDequeueCount.load(),
		(unsigned long long)g_synthFrameCount.load(),
		(unsigned long long)g_synthWaitErrors.load(),
		(unsigned long long)g_synthBeginErrors.load(),
		(unsigned long long)g_synthEndErrors.load());

	if (!g_fallbackSingleThread) {
		g_synthShutdown.store(true);
		g_synthCv.notify_all();
		if (g_synthThread.joinable()) {
			g_synthThread.join();
		}
		{
			std::lock_guard<std::mutex> lock(g_synthMutex);
			g_synthQueue.clear();
		}
	}

	g_synthRunning.store(false);
	OOVR_LOG("[SynthThread] StopSynthThread: complete");
}

// === Instance method: SubmitInterpolatedFrame ===

vr::IVRCompositorExt_001::EVRCompositorError BaseCompositorExt::SubmitInterpolatedFrame(
	const vr::Texture_t* synthTexture,
	const vr::VRTextureBounds_t* boundsLeft,
	const vr::VRTextureBounds_t* boundsRight,
	const vr::HmdMatrix34_t* pose,
	double displayTimeOffsetSeconds)
{
	callCount++;

	// Input validation (same as prior stub).
	if (!synthTexture || !synthTexture->handle) {
		return vr::IVRCompositorExt_001::VRCompositorError_InvalidTexture;
	}
	if (synthTexture->eType != vr::TextureType_DirectX) {
		return vr::IVRCompositorExt_001::VRCompositorError_InvalidTexture;
	}

	// If the thread (or fallback) hasn't been started, drop the call.
	if (!g_synthRunning.load()) {
		if (ShouldLogRate(callCount)) {
			OOVR_LOGF("[SynthThread] SubmitInterpolatedFrame call #%llu "
				"dropped: thread not running (session not active?)",
				(unsigned long long)callCount);
		}
		return vr::IVRCompositorExt_001::VRCompositorError_RequestFailed;
	}

	// Build the request.
	SynthRequest req;
	req.sequence = callCount;
	req.textureHandle = synthTexture->handle;
	req.textureType = synthTexture->eType;
	req.colorSpace = synthTexture->eColorSpace;
	if (boundsLeft)  req.boundsLeft  = *boundsLeft;
	if (boundsRight) req.boundsRight = *boundsRight;
	if (pose)        req.pose        = *pose;
	req.displayTimeOffsetSeconds = displayTimeOffsetSeconds;

	// === FALLBACK PATH: synchronous on calling (engine) thread ===
	if (g_fallbackSingleThread) {
		if (ShouldLogRate(callCount)) {
			OOVR_LOGF("[SynthThread] SubmitInterpolatedFrame call #%llu "
				"FALLBACK: running synchronously on engine thread",
				(unsigned long long)callCount);
		}
		bool ok = RunSynthFrameCycle(req);
		return ok ? vr::IVRCompositorExt_001::VRCompositorError_None
		          : vr::IVRCompositorExt_001::VRCompositorError_RequestFailed;
	}

	// === DEFAULT PATH: enqueue for synth thread ===
	size_t queueDepthAfter = 0;
	{
		std::lock_guard<std::mutex> lock(g_synthMutex);
		// Apply queue cap — drop oldest if full.
		while (g_synthQueue.size() >= kMaxQueueDepth) {
			g_synthQueue.pop_front();
		}
		g_synthQueue.push_back(req);
		queueDepthAfter = g_synthQueue.size();
	}
	g_synthCv.notify_one();

	uint64_t eq = g_synthEnqueueCount.fetch_add(1) + 1;
	if (ShouldLogRate(eq)) {
		OOVR_LOGF("[SynthThread] enqueued request seq=%llu (total eq=%llu, "
			"queue_depth_after=%zu)",
			(unsigned long long)req.sequence,
			(unsigned long long)eq,
			queueDepthAfter);
	}

	return vr::IVRCompositorExt_001::VRCompositorError_None;
}
