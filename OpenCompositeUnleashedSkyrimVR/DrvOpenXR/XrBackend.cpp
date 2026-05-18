//
// Created by ZNix on 25/10/2020.
//

#include "XrBackend.h"
#include "generated/interfaces/vrtypes.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#if defined(SUPPORT_GL) && !defined(_WIN32)
#include <GL/glx.h>
#endif

#include <openxr/openxr_platform.h>

// On Android, the app has to pass the OpenGLES setup data through
#ifdef ANDROID
#include "../OpenOVR/Misc/android_api.h"
#endif

// FIXME find a better way to send the OnPostFrame call?
#include "../OpenOVR/Reimpl/BaseCompositorExt.h"
#include "../OpenOVR/Reimpl/BaseInput.h"
#include "../OpenOVR/Reimpl/BaseOverlay.h"
#include "../OpenOVR/Reimpl/BaseSystem.h"
#include "../OpenOVR/convert.h"
#include "../OpenOVR/Misc/Config.h"
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
#include "../OpenOVR/Compositor/dx11compositor.h"
#endif
#include "generated/static_bases.gen.h"

#include "generated/interfaces/IVRCompositor_018.h"

#include "tmp_gfx/TemporaryGraphics.h"

#if defined(SUPPORT_VK)
#include "tmp_gfx/TemporaryVk.h"
#endif

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
#include "tmp_gfx/TemporaryD3D11.h"
#endif

#include <chrono>
#include <ranges>
#include <thread>
#include <type_traits>

using namespace vr;

static bool testOnlyOne = false;

std::unique_ptr<TemporaryGraphics> XrBackend::temporaryGraphics = nullptr;
XrBackend::XrBackend(bool useVulkanTmpGfx, bool useD3D11TmpGfx)
{
	memset(projectionViews, 0, sizeof(projectionViews));

	// setup temporaryGraphics

#if defined(SUPPORT_VK)
	if (useVulkanTmpGfx) {
		temporaryGraphics = std::make_unique<TemporaryVk>();
	}
#endif

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	// To prevent error code XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING with Unity games
	if (temporaryGraphics) {
		XrGraphicsRequirementsD3D11KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
		OOVR_FAILED_XR_ABORT(xr_ext->xrGetD3D11GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));
	}

	if (!temporaryGraphics && useD3D11TmpGfx) {
		temporaryGraphics = std::make_unique<TemporaryD3D11>();
	}
#endif

	OOVR_FALSE_ABORT(temporaryGraphics);

	// setup the device indexes
	for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++) {
		ITrackedDevice* dev = GetDevice(i);

		if (dev)
			dev->InitialiseDevice(i);
	}
}

XrBackend::~XrBackend()
{
	// First clear out the compositors, since they might try and access the OpenXR instance
	// in their destructor.
	PrepareForSessionShutdown();

	DrvOpenXR::FullShutdown();

	graphicsBinding = nullptr;

	// This must happen after session destruction (which occurs in FullShutdown), as runtimes (namely Monado)
	// may try to access these resources while destroying the session.
	temporaryGraphics.reset();

	// Phase C2.8d-fix1: release the high-resolution waitable timer if we created one.
#ifdef _WIN32
	if (hHighResTimer) {
		CloseHandle(hHighResTimer);
		hHighResTimer = nullptr;
	}
#endif
}

XrSessionState XrBackend::GetSessionState()
{
	return sessionState;
}

IHMD* XrBackend::GetPrimaryHMD()
{
	return hmd.get();
}

ITrackedDevice* XrBackend::GetDevice(
    vr::TrackedDeviceIndex_t index)
{
	switch (index) {
	case vr::k_unTrackedDeviceIndex_Hmd:
		return GetPrimaryHMD();
	case 1:
		return hand_left.get();
	case 2:
		return hand_right.get();
	default:
		return nullptr;
	}
}

ITrackedDevice* XrBackend::GetDeviceByHand(
    ITrackedDevice::HandType hand)
{
	switch (hand) {
	case ITrackedDevice::HAND_LEFT:
		return hand_left.get();
	case ITrackedDevice::HAND_RIGHT:
		return hand_right.get();
	default:
		OOVR_SOFT_ABORTF("Cannot get hand by type '%d'", (int)hand);
		return nullptr;
	}
}

void XrBackend::GetDeviceToAbsoluteTrackingPose(
    vr::ETrackingUniverseOrigin toOrigin,
    float predictedSecondsToPhotonsFromNow,
    vr::TrackedDevicePose_t* poseArray,
    uint32_t poseArrayCount)
{
	for (uint32_t i = 0; i < poseArrayCount; ++i) {
		ITrackedDevice* dev = GetDevice(i);
		if (dev) {
			dev->GetPose(toOrigin, &poseArray[i], ETrackingStateType::TrackingStateType_Rendering);
		} else {
			poseArray[i] = BackendManager::InvalidPose();
		}
	}
}

#ifdef SUPPORT_VK
static void find_queue_family_and_queue_idx(VkDevice dev, VkPhysicalDevice pdev, VkQueue desired_queue, uint32_t& out_queueFamilyIndex, uint32_t& out_queueIndex)
{
	uint32_t queue_family_count;
	vkGetPhysicalDeviceQueueFamilyProperties(pdev, &queue_family_count, NULL);

	std::vector<VkQueueFamilyProperties> hi(queue_family_count);
	vkGetPhysicalDeviceQueueFamilyProperties(pdev, &queue_family_count, hi.data());
	OOVR_LOGF("number of queue families is %d", queue_family_count);

	for (int i = 0; i < queue_family_count; i++) {
		OOVR_LOGF("queue family %d has %d queues", i, hi[i].queueCount);
		for (int j = 0; j < hi[i].queueCount; j++) {
			VkQueue tmp;
			vkGetDeviceQueue(dev, i, j, &tmp);
			if (tmp == desired_queue) {
				OOVR_LOGF("Got desired queue: %d %d", i, j);
				out_queueFamilyIndex = i;
				out_queueIndex = j;
				return;
			}
		}
	}
	OOVR_ABORT("Couldn't find the queue family index/queue index of the queue that the OpenVR app gave us!"
	           "This is really odd and really shouldn't ever happen");
}
#endif // SUPPORT_VK

/* Submitting Frames */
void XrBackend::CheckOrInitCompositors(const vr::Texture_t* tex)
{
	// Check we're using the session with the application's device
	if (!usingApplicationGraphicsAPI) {
		usingApplicationGraphicsAPI = true;

		OOVR_LOG("Recreating OpenXR session for application graphics API");

		// Shutdown old session - apparently Varjo doesn't like the session being destroyed
		// after querying for graphics requirements.
		DrvOpenXR::ShutdownSession();

		switch (tex->eType) {
		case vr::TextureType_DirectX: {
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
			// The spec requires that we call this before starting a session using D3D. Unfortunately we
			// can't actually do anything with this information, since the game has already created the device.
			XrGraphicsRequirementsD3D11KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetD3D11GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			auto* d3dTex = (ID3D11Texture2D*)tex->handle;
			ID3D11Device* dev = nullptr;
			d3dTex->GetDevice(&dev);

			XrGraphicsBindingD3D11KHR d3dInfo{};
			d3dInfo.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
			d3dInfo.device = dev;
			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingD3D11KHR>>(d3dInfo);
			DrvOpenXR::SetupSession();

			dev->Release();
#else
			OOVR_ABORT("Application is trying to submit a D3D11 texture, which OpenComposite supports but is disabled in this build");
#endif
			break;
		}
		case vr::TextureType_DirectX12: {
#if defined(SUPPORT_DX) && defined(SUPPORT_DX12)
			// The spec requires that we call this before starting a session using D3D. Unfortunately we
			// can't actually do anything with this information, since the game has already created the device.
			XrGraphicsRequirementsD3D12KHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetD3D12GraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			D3D12TextureData_t* d3dTexData = (D3D12TextureData_t*)tex->handle;
			ComPtr<ID3D12Device> device;
			d3dTexData->m_pResource->GetDevice(IID_PPV_ARGS(&device));

			XrGraphicsBindingD3D12KHR d3dInfo{};
			d3dInfo.type = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
			d3dInfo.device = device.Get();
			d3dInfo.queue = d3dTexData->m_pCommandQueue;
			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingD3D12KHR>>(d3dInfo);
			DrvOpenXR::SetupSession();

#ifdef _DEBUG
			ComPtr<ID3D12Debug> debugController;
			D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
			debugController->EnableDebugLayer();
#endif

			device->Release();
#else
			OOVR_ABORT("Application is trying to submit a D3D12 texture, which OpenComposite supports but is disabled in this build");
#endif
			break;
		}
		case vr::TextureType_Vulkan: {
#ifdef SUPPORT_VK
			const vr::VRVulkanTextureData_t* vktex = (vr::VRVulkanTextureData_t*)tex->handle;

			VkPhysicalDevice xr_desire;
			// Regardless of error checking, we have to call this or we get crazy validation errors.
			xr_ext->xrGetVulkanGraphicsDeviceKHR(xr_instance, xr_system, vktex->m_pInstance, &xr_desire);

			if (xr_desire != vktex->m_pPhysicalDevice) {
				OOVR_ABORTF("The VkPhysicalDevice that the OpenVR app (%p) used is different from the one that the OpenXR runtime used (%p)!\n"
				            "This should never happen, except for on multi-gpu, in which case DRI_PRIME=1 should fix things on Linux.",
				    vktex->m_pPhysicalDevice, xr_desire);
			}

			XrGraphicsBindingVulkanKHR binding;
			binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
			binding.next = nullptr;
			binding.instance = vktex->m_pInstance;
			binding.physicalDevice = vktex->m_pPhysicalDevice;
			binding.device = vktex->m_pDevice;

			find_queue_family_and_queue_idx( //
			    binding.device, //
			    binding.physicalDevice, //
			    vktex->m_pQueue, //
			    binding.queueFamilyIndex, //
			    binding.queueIndex //
			);

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingVulkanKHR>>(binding);
			DrvOpenXR::SetupSession();
#else
			// VRNord/CS-Fork build has SUPPORT_VK disabled — no Vulkan SDK shipped.
			// Match the D3D12 branch's pattern above for missing-API support.
			OOVR_ABORT("Application is trying to submit a Vulkan texture, which OpenComposite supports but is disabled in this build");
#endif
			break;
		}
		case vr::TextureType_OpenGL: {
#ifdef SUPPORT_GL
			// The spec requires that we call this before starting a session using OpenGL. Unfortunately we
			// can't actually do anything with this information, since the game has already created the context.
			XrGraphicsRequirementsOpenGLKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetOpenGLGraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			// Platform-specific OpenGL context stuff:
#ifdef _WIN32
			XrGraphicsBindingOpenGLWin32KHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
			binding.hGLRC = wglGetCurrentContext();
			binding.hDC = wglGetCurrentDC();

			if (!binding.hGLRC || !binding.hDC) {
				OOVR_ABORTF("Null OpenGL GLRC or DC: %p,%p", (void*)binding.hGLRC, (void*)binding.hDC);
			}

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLWin32KHR>>(binding);
			DrvOpenXR::SetupSession();
#else
			// Only support xlib for now (same as Monado)
			// TODO wayland
			// TODO xcb

			// Unfortunately we're in a bit of a sticky situation here. We can't (as far as I can tell) get
			// the GLXFBConfig from the context or drawable, and the display might give us multiple, so
			// we can't pass it onto the runtime. If we have it we can use it to find the visual info, but
			// otherwise we can't find that either.
			//    GLXFBConfig config = some_magic_function();
			//    XVisualInfo* vi = glXGetVisualFromFBConfig(glXGetCurrentDisplay(), config);
			//    uint32_t visualid = vi->visualid;
			// So... FIXME FIXME FIXME HAAAAACK! Just pass in invalid values and hope the runtime doesn't notice!
			// Monado doesn't (and hopefully in the future, won't) use these values, so it ought to work for now.
			//
			// Note: on re-reading the spec it does appear there's no requirement that the config is the one used
			//  to create the context. That seems a bit odd so we could be technically compliant by just grabbing
			//  the first one, but it's probably better (IMO) to pass null and make the potential future issue
			//  obvious rather than wasting lots of time of the poor person who has to track it down.
			GLXFBConfig config = nullptr;
			uint32_t visualid = 0xffffffff;

			XrGraphicsBindingOpenGLXlibKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR };
			binding.xDisplay = glXGetCurrentDisplay();
			binding.visualid = visualid;
			binding.glxFBConfig = config;
			binding.glxDrawable = glXGetCurrentDrawable();
			binding.glxContext = glXGetCurrentContext();

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLXlibKHR>>(binding);
			DrvOpenXR::SetupSession();
#endif
			// End of platform-specific code

#elif defined(SUPPORT_GLES)
			// The spec requires that we call this before starting a session using OpenGL. We could actually handle this properly
			// on android since the app has to be modified to work with us, but for now don't bother.
			XrGraphicsRequirementsOpenGLESKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
			OOVR_FAILED_XR_ABORT(xr_ext->xrGetOpenGLESGraphicsRequirementsKHR(xr_instance, xr_system, &graphicsRequirements));

			if (!OpenComposite_Android_GLES_Binding_Info)
				OOVR_ABORT("App is trying to use GLES, but OpenComposite_Android_GLES_Binding_Info global is not set.\n"
				           "Please ensure this is set by the application.");

			XrGraphicsBindingOpenGLESAndroidKHR binding = *OpenComposite_Android_GLES_Binding_Info;
			OOVR_FALSE_ABORT(binding.type == XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR);
			binding.next = nullptr;

			graphicsBinding = std::make_unique<BindingWrapper<XrGraphicsBindingOpenGLESAndroidKHR>>(binding);
			DrvOpenXR::SetupSession();

#else
			OOVR_ABORT("Application is trying to submit an OpenGL texture, which OpenComposite supports but is disabled in this build");
#endif
			break;
		}
		default:
			OOVR_ABORTF("Invalid/unknown texture type %d", tex->eType);
		}

		// Real graphics binding should be setup now - get rid of temporary graphics
		temporaryGraphics.reset();
	}

	for (std::unique_ptr<Compositor>& compositor : compositors) {
		// Skip a compositor if it's already set up
		if (compositor)
			continue;

		compositor.reset(BaseCompositor::CreateCompositorAPI(tex));
	}
}

void XrBackend::WaitForTrackingData()
{
	// Make sure the OpenXR session is active before doing anything else, and if not then skip
	if (!sessionActive) {
		renderingFrame = false;
		return;
	}

	if (oovr_global_configuration.SynthDualCycle()) {
		// Phase C2.8c: open synth cycle. CS-Fork's H4 hook (or SubmitFrames
		// fallback) will close it and open engine's cycle. renderingFrame
		// stays false until engine cycle opens, so StoreEyeTexture won't
		// try to write before it's safe.
		OpenSynthCycle();
	} else {
		// Phase C2.7a baseline: no-synth flow calls both phases back-to-back.
		WaitForTrackingData_WaitAndPoses();
		OpenEngineFrameCycle();
	}
}

void XrBackend::WaitForTrackingData_WaitAndPoses()
{
	// Caller has already verified sessionActive. Re-check defensively.
	if (!sessionActive)
		return;

	XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState state{ XR_TYPE_FRAME_STATE };

	{
		auto lock = xr_session.lock_shared();
		OOVR_FAILED_XR_ABORT(xrWaitFrame(xr_session.get(), &waitInfo, &state));
		xr_gbl->nextPredictedFrameTime = state.predictedDisplayTime;

		// FIXME loop until this returns true?
		// OOVR_FALSE_ABORT(state.shouldRender);
	}

	XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = xr_gbl->nextPredictedFrameTime;
	locateInfo.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
	XrViewState viewState = { XR_TYPE_VIEW_STATE };
	uint32_t viewCount = 0;
	XrView views[XruEyeCount] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	OOVR_FAILED_XR_SOFT_ABORT(xrLocateViews(xr_session.get(), &locateInfo, &viewState, XruEyeCount, &viewCount, views));

	for (int eye = 0; eye < XruEyeCount; eye++) {
		projectionViews[eye].fov = views[eye].fov;

		XrPosef pose = views[eye].pose;

		// Make sure we at least have halfway-sane values if the runtime isn't providing them. In particular
		// if the runtime gives us an invalid orientation, that'd otherwise cause XR_ERROR_POSE_INVALID errors later.
		if ((viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
			pose.orientation = XrQuaternionf{ 0, 0, 0, 1 };
		}
		if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0) {
			// About 1.75m up above the origin
			pose.position = XrVector3f{ 0, 1.75, 0 };
		}

		projectionViews[eye].pose = pose;
	}
}

void XrBackend::OpenEngineFrameCycle()
{
	// Caller has already verified sessionActive. Re-check defensively.
	if (!sessionActive)
		return;

	{
		auto lock = xr_session.lock_shared();
		XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
		OOVR_FAILED_XR_ABORT(xrBeginFrame(xr_session.get(), &beginInfo));
	}

	// If we're not on the game's graphics API yet, don't actually mark us as having started the frame.
	// Instead, set a different flag so we'll call this method again when it's available.
	if (!usingApplicationGraphicsAPI) {
		deferredRenderingStart = true;
	} else {
		renderingFrame = true;
	}
}

// === Phase C2.8c dual-cycle helpers ===

bool XrBackend::SynthSwapchain_EnsureInit()
{
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	if (synthSwapchain.swapchain[0] != XR_NULL_HANDLE)
		return true;

	if (!compositors[0]) {
		OOVR_LOGF("[SynthDC] EnsureInit: engine compositor[0] not ready yet");
		return false;
	}

	DX11Compositor* dx11Comp = dynamic_cast<DX11Compositor*>(compositors[0].get());
	if (!dx11Comp) {
		OOVR_LOGF("[SynthDC] EnsureInit: engine compositor[0] is not DX11Compositor — only DX11 supported");
		return false;
	}

	const uint32_t width = (uint32_t)dx11Comp->GetSrcSize().width;
	const uint32_t height = (uint32_t)dx11Comp->GetSrcSize().height;

	// Phase C2.8c-fix3: compositors[0] non-null does NOT imply its
	// CheckCreateSwapChain has populated createInfo. Engine's first eye
	// Submit may go through CheckOrInitCompositors (creating the
	// DX11Compositor) but skip the inner Invoke that fills createInfo if
	// !sessionActive || !renderingFrame at that time (e.g. during the
	// D3D11 session-recreate at XrBackend.cpp:197). Without this guard,
	// fix2's hardcoded non-zero format defeated fix1's retry mechanism,
	// causing 0x0 xrCreateSwapchain → runtime corruption → access violation.
	if (width == 0 || height == 0) {
		static uint64_t s_zeroDimLogCount = 0;
		if (s_zeroDimLogCount < 10) {
			OOVR_LOGF("[SynthDC] EnsureInit: engine compositor has zero dimensions "
				"(width=%u height=%u) — CheckCreateSwapChain not yet called for "
				"this compositor, retry next frame",
				width, height);
		}
		s_zeroDimLogCount++;
		return false;
	}

	// Phase C2.8c-fix2: hardcode synth swapchain format to RGBA16F to match
	// CS-Fork's synthColorTex (RGBA16F per SynthFrameCS.h). Engine's eye
	// swapchain is SRGB (format 29) but synthColorTex isn't, so copying
	// engine-format → synth-format via CopySubresourceRegion silently fails.
	// SteamVR-OpenXR's supported format list includes RGBA16F (format 10).
	const int64_t format = (int64_t)DXGI_FORMAT_R16G16B16A16_FLOAT;

	synthSwapchain.width = width;
	synthSwapchain.height = height;
	synthSwapchain.format = format;

	OOVR_LOGF("[SynthDC] creating synth swapchains: width=%u height=%u "
		"openxr_format=%lld (RGBA16F, hardcoded to match synthColorTex)",
		width, height, (long long)format);

	auto lock = xr_session.lock_shared();
	XrSession session = xr_session.get();
	for (int eye = 0; eye < XruEyeCount; eye++) {
		XrSwapchainCreateInfo info{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
		    | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT
		    | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		info.format = format;
		info.sampleCount = 1;
		info.width = width;
		info.height = height;
		info.faceCount = 1;
		info.arraySize = 1;
		info.mipCount = 1;

		XrResult cr = xrCreateSwapchain(session, &info, &synthSwapchain.swapchain[eye]);
		if (XR_FAILED(cr)) {
			char buf[XR_MAX_RESULT_STRING_SIZE] = "";
			xrResultToString(xr_instance, cr, buf);
			OOVR_LOGF("[SynthDC] xrCreateSwapchain eye=%d failed: result=%d (%s)",
				eye, (int)cr, buf);
			SynthSwapchain_Shutdown();
			return false;
		}

		uint32_t imageCount = 0;
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(synthSwapchain.swapchain[eye], 0, &imageCount, nullptr));
		synthSwapchain.images[eye].resize(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(synthSwapchain.swapchain[eye], imageCount, &imageCount,
		    (XrSwapchainImageBaseHeader*)synthSwapchain.images[eye].data()));

		OOVR_LOGF("[SynthDC] eye=%d swapchain created with %u images", eye, imageCount);

		// Phase C2.8c-fix2: create RTVs per swapchain image for the
		// magenta debug-fill path.
		synthSwapchain.rtvs[eye].resize(imageCount, nullptr);
		ID3D11Device* d3dDevice = nullptr;
		if (imageCount > 0 && synthSwapchain.images[eye][0].texture) {
			synthSwapchain.images[eye][0].texture->GetDevice(&d3dDevice);
		}
		if (d3dDevice) {
			for (uint32_t i = 0; i < imageCount; i++) {
				D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
				rtvDesc.Format = (DXGI_FORMAT)format;
				rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				rtvDesc.Texture2D.MipSlice = 0;
				HRESULT hr = d3dDevice->CreateRenderTargetView(
				    synthSwapchain.images[eye][i].texture,
				    &rtvDesc,
				    &synthSwapchain.rtvs[eye][i]);
				if (FAILED(hr)) {
					OOVR_LOGF("[SynthDC] eye=%d image=%u CreateRenderTargetView failed hr=0x%08x",
					    eye, i, (unsigned)hr);
				}
			}
			d3dDevice->Release();
		}
	}

	return true;
#else
	return false;
#endif
}

void XrBackend::SynthSwapchain_Shutdown()
{
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	for (int eye = 0; eye < XruEyeCount; eye++) {
		for (ID3D11RenderTargetView* rtv : synthSwapchain.rtvs[eye]) {
			if (rtv) rtv->Release();
		}
		synthSwapchain.rtvs[eye].clear();
		synthSwapchain.images[eye].clear();
		if (synthSwapchain.swapchain[eye] != XR_NULL_HANDLE) {
			xrDestroySwapchain(synthSwapchain.swapchain[eye]);
			synthSwapchain.swapchain[eye] = XR_NULL_HANDLE;
		}
	}
#endif
}

void XrBackend::OpenSynthCycle()
{
	if (!sessionActive) return;
	if (synthCyclePending.load()) {
		OOVR_LOGF("[SynthDC] OpenSynthCycle called but synth cycle already pending — skipping");
		return;
	}
	if (engineCycleOpen.load()) {
		OOVR_LOGF("[SynthDC] OpenSynthCycle called but engine cycle already open — skipping");
		return;
	}

	// Phase C2.8d: engine throttle. Sleep until >= 2 * display period has
	// elapsed since the last WGP entry. Requires at least one prior
	// completed synth cycle (minDisplayPeriodNs < INT64_MAX). On first call
	// after enable, lastEntry is 0 and minDisplayPeriodNs is INT64_MAX, so
	// throttle is a no-op and these values get seeded at the bottom of
	// this function.
	//
	// Phase C2.8d-fix1: precision sleep via Win32 high-resolution waitable
	// timer. std::this_thread::sleep_for's precision is bounded by Windows
	// scheduler quantum (~15.6ms default, ~1ms with timeBeginPeriod(1)),
	// which is fatal for 11ms targets (90Hz) and worse at higher refresh
	// rates. CreateWaitableTimerExW with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
	// (Win10 1803+, universal in 2026) gives 100ns precision.
	if (oovr_global_configuration.SynthEngineThrottle()) {
		const int64_t lastEntry = tWGPLastEntryNs.load();
		const int64_t period = minDisplayPeriodNs.load();
		// period < INT64_MAX means we've observed at least one valid period.
		if (lastEntry > 0 && period > 0 && period < INT64_MAX) {
			using clock = std::chrono::steady_clock;
			const int64_t targetNs = lastEntry + 2 * period;
			const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
			    clock::now().time_since_epoch()).count();
			if (nowNs < targetNs) {
				int64_t sleepNs = targetNs - nowNs;

				// Phase C2.8d-fix2: hard sanity ceiling. Caps engine at ~20Hz
				// floor at any refresh rate. Prevents runaway throttle from
				// any future pathology (memory corruption, multi-headset
				// session switch, etc.) producing catastrophic slowdown.
				// Normal operation should never reach this cap.
				constexpr int64_t kMaxSleepNs = 50'000'000; // 50ms
				if (sleepNs > kMaxSleepNs) {
					static uint64_t s_capLogCount = 0;
					if (s_capLogCount < 10) {
						OOVR_LOGF("[SynthDC] WARNING: throttle sleep %.2fms exceeds "
						    "%.0fms ceiling (min observed period %.3fms) — capping. "
						    "If this fires often, min-observed-period tracking is broken.",
						    sleepNs / 1e6, kMaxSleepNs / 1e6, period / 1e6);
					}
					s_capLogCount++;
					sleepNs = kMaxSleepNs;
				}

				bool didHighResSleep = false;

#ifdef _WIN32
				// Lazily create the waitable timer on first use.
				if (!hHighResTimer && !throttleHighResTimerFailed) {
					hHighResTimer = CreateWaitableTimerExW(
					    nullptr,
					    nullptr,
					    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION | CREATE_WAITABLE_TIMER_MANUAL_RESET,
					    TIMER_ALL_ACCESS);
					if (!hHighResTimer) {
						const DWORD err = GetLastError();
						OOVR_LOGF("[SynthDC] WARNING: CreateWaitableTimerExW(HIGH_RESOLUTION) "
						    "failed (err=%lu) — falling back to std::this_thread::sleep_for "
						    "for engine throttle; pacing precision will be ~15ms instead of "
						    "<1ms. This is unexpected on Win10 1803+ and may cause visible "
						    "frame-pacing jitter.", (unsigned long)err);
						throttleHighResTimerFailed = true;
					}
				}

				if (hHighResTimer) {
					// SetWaitableTimer takes a LARGE_INTEGER due time in 100ns units.
					// Negative = relative to now. So sleepNs / 100 negated.
					LARGE_INTEGER due;
					due.QuadPart = -(LONGLONG)(sleepNs / 100);
					if (SetWaitableTimer(hHighResTimer, &due, 0, nullptr, nullptr, FALSE)) {
						// Timeout cap: ceil(sleepNs/1e6) + 10ms safety margin.
						// Hitting this means something went wrong; better to
						// return than block indefinitely.
						const DWORD timeoutMs = (DWORD)((sleepNs + 999999) / 1000000) + 10;
						const DWORD waitResult = WaitForSingleObject(hHighResTimer, timeoutMs);
						if (waitResult == WAIT_OBJECT_0) {
							didHighResSleep = true;
						} else {
							static uint64_t s_waitFailLogCount = 0;
							if (s_waitFailLogCount < 5) {
								OOVR_LOGF("[SynthDC] WARNING: WaitForSingleObject on high-res "
								    "timer returned 0x%08x (expected 0 = WAIT_OBJECT_0); "
								    "throttle precision may be degraded",
								    (unsigned)waitResult);
							}
							s_waitFailLogCount++;
						}
					}
				}
#endif

				if (!didHighResSleep) {
					// Fallback path: best-effort std::this_thread::sleep_for.
					std::this_thread::sleep_for(std::chrono::nanoseconds(sleepNs));
				}

				static uint64_t s_throttleLogCount = 0;
				if (s_throttleLogCount < 5 || (s_throttleLogCount % 600) == 0) {
					// Measure the ACTUAL slept duration so we can detect timer
					// precision issues in the field.
					const int64_t postNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
					    clock::now().time_since_epoch()).count();
					const int64_t actualSleepNs = postNs - nowNs;
					OOVR_LOGF("[SynthDC] throttle: requested %.3fms slept %.3fms "
					    "(target=2x%.3fms MIN-observed period, mechanism=%s)",
					    sleepNs / 1e6, actualSleepNs / 1e6, period / 1e6,
					    didHighResSleep ? "high-res-timer" : "sleep_for");
				}
				s_throttleLogCount++;
			}
		}
	}

	auto lock = xr_session.lock_shared();
	XrSession session = xr_session.get();

	XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState state{ XR_TYPE_FRAME_STATE };
	XrResult wr = xrWaitFrame(session, &waitInfo, &state);
	if (XR_FAILED(wr)) {
		char buf[XR_MAX_RESULT_STRING_SIZE] = "";
		xrResultToString(xr_instance, wr, buf);
		OOVR_LOGF("[SynthDC] OpenSynthCycle xrWaitFrame failed: %d (%s)", (int)wr, buf);
		return;
	}

	// Synth's wait fills xr_gbl->nextPredictedFrameTime; engine's later wait
	// will overwrite this with engine's slot time.
	xr_gbl->nextPredictedFrameTime = state.predictedDisplayTime;

	// Locate views for synth's slot. (Spike: engine reuses these poses for
	// its render; refinement would locate twice — once per slot.)
	XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = state.predictedDisplayTime;
	locateInfo.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
	XrViewState viewState = { XR_TYPE_VIEW_STATE };
	uint32_t viewCount = 0;
	XrView views[XruEyeCount] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	OOVR_FAILED_XR_SOFT_ABORT(xrLocateViews(session, &locateInfo, &viewState, XruEyeCount, &viewCount, views));

	for (int eye = 0; eye < XruEyeCount; eye++) {
		projectionViews[eye].fov = views[eye].fov;
		XrPosef pose = views[eye].pose;
		if ((viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0)
			pose.orientation = XrQuaternionf{ 0, 0, 0, 1 };
		if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0)
			pose.position = XrVector3f{ 0, 1.75, 0 };
		projectionViews[eye].pose = pose;
	}

	XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
	XrResult br = xrBeginFrame(session, &beginInfo);
	if (XR_FAILED(br)) {
		char buf[XR_MAX_RESULT_STRING_SIZE] = "";
		xrResultToString(xr_instance, br, buf);
		OOVR_LOGF("[SynthDC] OpenSynthCycle xrBeginFrame failed: %d (%s)", (int)br, buf);
		return;
	}

	synthCyclePending.store(true);

	// Phase C2.8d-fix2: record this WGP's wall-clock entry and update
	// the minimum observed predictedDisplayPeriod. Min, not most-recent,
	// to defeat the SteamVR-inflated-period feedback loop. The runtime
	// cannot physically report a period shorter than the headset's true
	// native vsync interval, so min converges to that ground truth.
	if (oovr_global_configuration.SynthEngineThrottle()) {
		using clock = std::chrono::steady_clock;
		const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
		    clock::now().time_since_epoch()).count();
		tWGPLastEntryNs.store(nowNs);

		const int64_t newPeriod = (int64_t)state.predictedDisplayPeriod;
		if (newPeriod > 0) {
			// CAS-loop to atomically update min. Contended only during the
			// rare case of period changing — single-writer in practice
			// (this function called only from engine's WGP thread).
			int64_t current = minDisplayPeriodNs.load();
			while (newPeriod < current
			       && !minDisplayPeriodNs.compare_exchange_weak(current, newPeriod)) {
				// current was reloaded by compare_exchange_weak; loop until
				// we win the CAS or someone else lowered min below newPeriod.
			}
		}
	}

	static uint64_t s_logCount = 0;
	if (s_logCount < 10 || (s_logCount % 300) == 0) {
		const int64_t minPeriod = minDisplayPeriodNs.load();
		OOVR_LOGF("[SynthDC] synth cycle opened: predictedDisplayTime=%lld "
			"predictedDisplayPeriod=%lld minObservedPeriod=%lld",
			(long long)state.predictedDisplayTime,
			(long long)state.predictedDisplayPeriod,
			(long long)(minPeriod < INT64_MAX ? minPeriod : -1));
	}
	s_logCount++;
}

void XrBackend::CloseSynthCycleAndOpenEngineCycle(
    const vr::Texture_t* synthTexture,
    const vr::VRTextureBounds_t* boundsLeft,
    const vr::VRTextureBounds_t* boundsRight)
{
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	if (!sessionActive) return;
	if (!synthCyclePending.load()) {
		OOVR_LOGF("[SynthDC] CloseSynthCycle called but synth cycle not pending — skipping");
		return;
	}

	if (!SynthSwapchain_EnsureInit()) {
		OOVR_LOGF("[SynthDC] synth swapchain init failed — falling back to placeholder");
		CloseSynthCycleAsPlaceholderAndOpenEngineCycle();
		return;
	}

	auto lock = xr_session.lock_shared();
	XrSession session = xr_session.get();

	// Capture synth displayTime BEFORE engine's wait stomps xr_gbl->nextPredictedFrameTime.
	const int64_t synthDisplayTime = (int64_t)xr_gbl->nextPredictedFrameTime;

	XrCompositionLayerProjectionView synthViews[XruEyeCount] = {
		{ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
		{ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }
	};

	auto* d3dSourceTex = (ID3D11Texture2D*)synthTexture->handle;
	ID3D11Device* device = nullptr;
	d3dSourceTex->GetDevice(&device);
	ID3D11DeviceContext* context = nullptr;
	device->GetImmediateContext(&context);

	// Phase C2.8c-fix2: diagnostic log of source tex state. Confirms the H4
	// hook is firing with valid data and that source format matches our
	// synth swapchain format (10 = DXGI_FORMAT_R16G16B16A16_FLOAT).
	{
		static uint64_t s_diagLogCount = 0;
		if (s_diagLogCount < 5 || (s_diagLogCount % 600) == 0) {
			D3D11_TEXTURE2D_DESC srcDesc{};
			d3dSourceTex->GetDesc(&srcDesc);
			OOVR_LOGF("[SynthDC] source tex=%p width=%u height=%u format=%d "
			    "(synth swapchain format=%lld) boundsL=[%.2f,%.2f,%.2f,%.2f] boundsR=[%.2f,%.2f,%.2f,%.2f]",
			    (void*)d3dSourceTex, srcDesc.Width, srcDesc.Height, (int)srcDesc.Format,
			    (long long)synthSwapchain.format,
			    boundsLeft->uMin, boundsLeft->vMin, boundsLeft->uMax, boundsLeft->vMax,
			    boundsRight->uMin, boundsRight->vMin, boundsRight->uMax, boundsRight->vMax);
		}
		s_diagLogCount++;
	}

	const bool forceMagenta = oovr_global_configuration.SynthDebugForceMagenta();
	const float magentaColor[4] = { 1.0f, 0.0f, 1.0f, 1.0f };

	for (int eye = 0; eye < XruEyeCount; eye++) {
		XrSwapchain sc = synthSwapchain.swapchain[eye];

		XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t index = 0;
		OOVR_FAILED_XR_ABORT(xrAcquireSwapchainImage(sc, &acquireInfo, &index));

		XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		waitInfo.timeout = XR_INFINITE_DURATION;
		OOVR_FAILED_XR_ABORT(xrWaitSwapchainImage(sc, &waitInfo));

		const vr::VRTextureBounds_t* bounds = (eye == 0) ? boundsLeft : boundsRight;
		D3D11_TEXTURE2D_DESC sourceDesc{};
		d3dSourceTex->GetDesc(&sourceDesc);
		D3D11_BOX srcBox{};
		srcBox.left   = (UINT)(bounds->uMin * sourceDesc.Width);
		srcBox.right  = (UINT)(bounds->uMax * sourceDesc.Width);
		srcBox.top    = (UINT)(bounds->vMin * sourceDesc.Height);
		srcBox.bottom = (UINT)(bounds->vMax * sourceDesc.Height);
		srcBox.front  = 0;
		srcBox.back   = 1;

		if (forceMagenta) {
			// Diagnostic: fill with magenta via ClearRenderTargetView. If magenta
			// appears in HMD between engine frames, the dual-cycle architecture
			// is working end-to-end and only the source texture copy is the issue.
			if (index < synthSwapchain.rtvs[eye].size() && synthSwapchain.rtvs[eye][index]) {
				context->ClearRenderTargetView(synthSwapchain.rtvs[eye][index], magentaColor);
			}
		} else {
			context->CopySubresourceRegion(
			    synthSwapchain.images[eye][index].texture,
			    0, 0, 0, 0,
			    d3dSourceTex,
			    0,
			    &srcBox);
		}

		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		OOVR_FAILED_XR_ABORT(xrReleaseSwapchainImage(sc, &releaseInfo));

		synthViews[eye] = projectionViews[eye];
		synthViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		synthViews[eye].subImage.swapchain = sc;
		synthViews[eye].subImage.imageRect.offset = { 0, 0 };
		synthViews[eye].subImage.imageRect.extent = {
		    (int32_t)synthSwapchain.width, (int32_t)synthSwapchain.height
		};
		synthViews[eye].subImage.imageArrayIndex = 0;
	}

	context->Release();
	device->Release();

	XrCompositionLayerProjection synthLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	synthLayer.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
	synthLayer.viewCount = XruEyeCount;
	synthLayer.views = synthViews;

	const XrCompositionLayerBaseHeader* synthLayerPtr =
	    (const XrCompositionLayerBaseHeader*)&synthLayer;

	XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.displayTime = (XrTime)synthDisplayTime;
	endInfo.layerCount = 1;
	endInfo.layers = &synthLayerPtr;
	XrResult er = xrEndFrame(session, &endInfo);
	if (XR_FAILED(er)) {
		char buf[XR_MAX_RESULT_STRING_SIZE] = "";
		xrResultToString(xr_instance, er, buf);
		OOVR_LOGF("[SynthDC] synth xrEndFrame failed: %d (%s) synthDisplayTime=%lld",
			(int)er, buf, (long long)synthDisplayTime);
		// Continue — still must clear flag + open engine cycle.
	}
	synthCyclePending.store(false);

	// === Open engine cycle (wait + locateViews + begin) ===
	XrFrameWaitInfo eWaitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState eState{ XR_TYPE_FRAME_STATE };
	XrResult ewr = xrWaitFrame(session, &eWaitInfo, &eState);
	if (XR_FAILED(ewr)) {
		char buf[XR_MAX_RESULT_STRING_SIZE] = "";
		xrResultToString(xr_instance, ewr, buf);
		OOVR_LOGF("[SynthDC] engine xrWaitFrame failed: %d (%s)", (int)ewr, buf);
		return;
	}

	xr_gbl->nextPredictedFrameTime = eState.predictedDisplayTime;
	engineCyclePredictedTime.store((int64_t)eState.predictedDisplayTime);

	XrViewLocateInfo eLocateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
	eLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	eLocateInfo.displayTime = eState.predictedDisplayTime;
	eLocateInfo.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
	XrViewState eViewState = { XR_TYPE_VIEW_STATE };
	uint32_t eViewCount = 0;
	XrView eViews[XruEyeCount] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	OOVR_FAILED_XR_SOFT_ABORT(xrLocateViews(session, &eLocateInfo, &eViewState, XruEyeCount, &eViewCount, eViews));

	for (int eye = 0; eye < XruEyeCount; eye++) {
		projectionViews[eye].fov = eViews[eye].fov;
		XrPosef pose = eViews[eye].pose;
		if ((eViewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0)
			pose.orientation = XrQuaternionf{ 0, 0, 0, 1 };
		if ((eViewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0)
			pose.position = XrVector3f{ 0, 1.75, 0 };
		projectionViews[eye].pose = pose;
	}

	XrFrameBeginInfo eBeginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
	XrResult ebr = xrBeginFrame(session, &eBeginInfo);
	if (XR_FAILED(ebr)) {
		char buf[XR_MAX_RESULT_STRING_SIZE] = "";
		xrResultToString(xr_instance, ebr, buf);
		OOVR_LOGF("[SynthDC] engine xrBeginFrame failed: %d (%s)", (int)ebr, buf);
		return;
	}
	engineCycleOpen.store(true);

	if (!usingApplicationGraphicsAPI)
		deferredRenderingStart = true;
	else
		renderingFrame = true;

	static uint64_t s_logCount = 0;
	if (s_logCount < 10 || (s_logCount % 300) == 0) {
		OOVR_LOGF("[SynthDC] cycle pair: synthDisplayTime=%lld engineDisplayTime=%lld delta=%lld",
			(long long)synthDisplayTime,
			(long long)eState.predictedDisplayTime,
			(long long)((int64_t)eState.predictedDisplayTime - synthDisplayTime));
	}
	s_logCount++;
#endif
}

void XrBackend::CloseSynthCycleAsPlaceholderAndOpenEngineCycle()
{
	if (!sessionActive) return;
	if (!synthCyclePending.load()) return;

	auto lock = xr_session.lock_shared();
	XrSession session = xr_session.get();

	const int64_t synthDisplayTime = (int64_t)xr_gbl->nextPredictedFrameTime;

	XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.displayTime = (XrTime)synthDisplayTime;
	endInfo.layerCount = 0;
	endInfo.layers = nullptr;
	OOVR_FAILED_XR_SOFT_ABORT(xrEndFrame(session, &endInfo));
	synthCyclePending.store(false);

	XrFrameWaitInfo eWaitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState eState{ XR_TYPE_FRAME_STATE };
	OOVR_FAILED_XR_ABORT(xrWaitFrame(session, &eWaitInfo, &eState));
	xr_gbl->nextPredictedFrameTime = eState.predictedDisplayTime;
	engineCyclePredictedTime.store((int64_t)eState.predictedDisplayTime);

	XrViewLocateInfo eLocateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
	eLocateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	eLocateInfo.displayTime = eState.predictedDisplayTime;
	eLocateInfo.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
	XrViewState eViewState = { XR_TYPE_VIEW_STATE };
	uint32_t eViewCount = 0;
	XrView eViews[XruEyeCount] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	OOVR_FAILED_XR_SOFT_ABORT(xrLocateViews(session, &eLocateInfo, &eViewState, XruEyeCount, &eViewCount, eViews));

	for (int eye = 0; eye < XruEyeCount; eye++) {
		projectionViews[eye].fov = eViews[eye].fov;
		XrPosef pose = eViews[eye].pose;
		if ((eViewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0)
			pose.orientation = XrQuaternionf{ 0, 0, 0, 1 };
		if ((eViewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0)
			pose.position = XrVector3f{ 0, 1.75, 0 };
		projectionViews[eye].pose = pose;
	}

	XrFrameBeginInfo eBeginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
	OOVR_FAILED_XR_ABORT(xrBeginFrame(session, &eBeginInfo));
	engineCycleOpen.store(true);

	if (!usingApplicationGraphicsAPI)
		deferredRenderingStart = true;
	else
		renderingFrame = true;
}

void XrBackend::StoreEyeTexture(
    vr::EVREye eye,
    const vr::Texture_t* texture,
    const vr::VRTextureBounds_t* bounds,
    vr::EVRSubmitFlags submitFlags,
    bool isFirstEye)
{
	CheckOrInitCompositors(texture);

	XrCompositionLayerProjectionView& layer = projectionViews[eye];
	layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;

	std::unique_ptr<Compositor>& compPtr = compositors[eye];
	OOVR_FALSE_ABORT(compPtr.get() != nullptr);
	Compositor& comp = *compPtr;

	// If the session is inactive, we may be unable to write to the surface
	if (sessionActive && renderingFrame)
		comp.Invoke((XruEye)eye, texture, bounds, submitFlags, layer);

	submittedEyeTextures = true;

	// TODO store view somewhere and use it for submitting our frame

	// If WaitGetPoses was called before the first texture was submitted, we're in a kinda weird state
	// The application will expect it can submit it's frames (and we do too) however xrBeginFrame was
	// never called for this session - it was called for the early session, then when the first texture
	// was published we switched to that, but this new session hasn't had xrBeginFrame called yet.
	// To get around this, we set a flag if we should begin a frame but are still in the early session. At
	// this point we can check for that flag and call xrBeginFrame a second time, on the right session.
	if (deferredRenderingStart && usingApplicationGraphicsAPI) {
		deferredRenderingStart = false;
		WaitForTrackingData();
	}
}

void XrBackend::SubmitFrames(bool showSkybox, bool postPresent)
{
	// Always pump events, even if the session isn't active - this is what makes the session active
	// in the first place.
	PumpEvents();

	// If we are getting calls from PostPresentHandOff then skip the calls from other functions as
	//  there will be other data such as GUI layers to be added before ending the frame.
	bool skipRender = postPresentStatus && !postPresent;
	postPresentStatus = postPresent;

	// Phase C2.8c fallback: if synth cycle was opened but never closed
	// (CS-Fork H4 hook didn't fire — toggle off mid-frame, extension not
	// connected, hook conditions not met), close it now with a placeholder
	// and open engine cycle inline so this SubmitFrames can close it.
	if (oovr_global_configuration.SynthDualCycle()
	    && synthCyclePending.load() && !engineCycleOpen.load()) {
		CloseSynthCycleAsPlaceholderAndOpenEngineCycle();
	}

	if (!renderingFrame || skipRender)
		return;

	// All data submitted, rendering has finished, frame can be ended.
	renderingFrame = false;

	// Make sure the OpenXR session is active before doing anything else
	// Note that if the session becomes ready after WaitGetTrackingPoses was called, then
	// renderingFrame will still be false so this won't be a problem in that case.
	if (!sessionActive)
		return;

	XrFrameEndInfo info{ XR_TYPE_FRAME_END_INFO };
	info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	info.displayTime = xr_gbl->nextPredictedFrameTime;

	XrCompositionLayerBaseHeader const* const* headers = nullptr;
	XrCompositionLayerBaseHeader* app_layer = nullptr;

	int layer_count = 0;

	// Apps can use layers to provide GUIs and loading screens where a 3D environment is not being rendered.
	// Only create the projection layer if we have a 3D environment to submit.
	XrCompositionLayerProjection mainLayer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	if (submittedEyeTextures) {
		// We have eye textures so setup a projection layer
		mainLayer.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
		mainLayer.views = projectionViews;
		mainLayer.viewCount = 2;

		app_layer = (XrCompositionLayerBaseHeader*)&mainLayer;
		for (int i = 0; i < mainLayer.viewCount; ++i) {
			XrCompositionLayerProjectionView& layer = projectionViews[i];
			layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			if (layer.subImage.swapchain == XR_NULL_HANDLE)
				app_layer = nullptr;
		}
		submittedEyeTextures = false;
	}

	// Ensure the BaseOverlay singleton exists so the keyboard shortcut
	// detection in _BuildLayers runs every frame. Without this, the overlay
	// is only created when the game explicitly requests IVROverlay, which
	// some games do late (or not at all until a cell transition).
	static std::shared_ptr<BaseOverlay> overlayHolder;
	if (!GetUnsafeBaseOverlay()) {
		overlayHolder = GetCreateBaseOverlay();
	}

	// If we have an overlay then add
	BaseOverlay* overlay = GetUnsafeBaseOverlay();
	if (overlay) {
		layer_count = overlay->_BuildLayers(app_layer, headers);
	} else if (app_layer) {
		layer_count = 1;
		headers = &app_layer;
	}

	// It's ok if no layers have been added at this point,
	// it will just cause the display to be blanked
	info.layers = headers;
	info.layerCount = layer_count;

	// Phase C2.8c: in dual-cycle mode, engine's displayTime came from engine's
	// xrWaitFrame (stashed in engineCyclePredictedTime). xr_gbl->nextPredictedFrameTime
	// may still hold synth's wait result if engine's wait was the most-recent
	// thing to touch it — but the safe value is the one we explicitly stashed.
	if (oovr_global_configuration.SynthDualCycle()) {
		int64_t enginePred = engineCyclePredictedTime.load();
		if (enginePred > 0) {
			info.displayTime = (XrTime)enginePred;
		}
	}

	OOVR_FAILED_XR_SOFT_ABORT(xrEndFrame(xr_session.get(), &info));
	engineCycleOpen.store(false);

	// Record engine's submitted displayTime for the synth thread to anchor
	// against. The synth thread will target a time half a frame interval
	// forward from this. (VRNord synth extension — Phase C2.5.)
	BaseCompositorExt::RecordEngineFrameSubmit((int64_t)info.displayTime);

	BaseSystem* sys = GetUnsafeBaseSystem();
	if (sys) {
		sys->_OnPostFrame();
	}

	auto now = std::chrono::system_clock::now().time_since_epoch();
	frameSubmitTimeUs = (double)std::chrono::duration_cast<std::chrono::microseconds>(now).count() / 1000000.0;

	nFrameIndex++;
}

IBackend::openvr_enum_t XrBackend::SetSkyboxOverride(const vr::Texture_t* pTextures, uint32_t unTextureCount)
{
	// Needed for rFactor2 loading screens
	if (unTextureCount && pTextures) {
		CheckOrInitCompositors(pTextures);

		if (!sessionActive || !usingApplicationGraphicsAPI)
			return 0;

		// Make sure any unfinished frames don't call xrEndFrame after this call
		renderingFrame = false;

		XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState state{ XR_TYPE_FRAME_STATE };

		OOVR_FAILED_XR_ABORT(xrWaitFrame(xr_session.get(), &waitInfo, &state));
		xr_gbl->nextPredictedFrameTime = state.predictedDisplayTime;

		// This submits a frame when a skybox override is set. This is designed around rFactor2 where the skybox is used as
		// a loading screen and is frequently updated, and most other games probably behave in a similar manner. It'd be
		// ideal to run a separate thread while the skybox override is set to submit frames if IVRCompositor->Submit is not
		// being called frequently enough, and that'd need to be carefully synchronised with the main submit thread. That's
		// not yet implemented since it's not currently worth the hassle, but if someone in the future wants to do it:
		// TODO submit skybox frames in their own thread.
		XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
		OOVR_FAILED_XR_ABORT(xrBeginFrame(xr_session.get(), &beginInfo));

		static std::unique_ptr<Compositor> compositor = nullptr;

		if (compositor == nullptr)
			compositor.reset(BaseCompositor::CreateCompositorAPI(pTextures));

		vr::VRTextureBounds_t bounds;
		bounds.uMin = 0.0;
		bounds.uMax = 1.0;
		bounds.vMin = 1.0;
		bounds.vMax = 0.0;

		compositor->Invoke(pTextures, &bounds);
		XrCompositionLayerQuad layerQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		layerQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		layerQuad.next = NULL;
		layerQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		layerQuad.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
		layerQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		layerQuad.pose = { { 0.f, 0.f, 0.f, 1.f },
			{ 0.0f, 0.0f, -0.65f } };
		layerQuad.size = { 1.0f, 1.0f / 1.333f };
		layerQuad.subImage = {
			compositor->GetSwapChain(),
			{ { 0, 0 },
			    { (int32_t)compositor->GetSrcSize().width,
			        (int32_t)compositor->GetSrcSize().height } },
			0
		};

		XrCompositionLayerBaseHeader* layers[1];
		layers[0] = (XrCompositionLayerBaseHeader*)&layerQuad;
		XrFrameEndInfo info{ XR_TYPE_FRAME_END_INFO };
		info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		info.displayTime = xr_gbl->nextPredictedFrameTime;
		info.layers = layers;
		info.layerCount = 1;

		OOVR_FAILED_XR_SOFT_ABORT(xrEndFrame(xr_session.get(), &info));

	} else {
		OOVR_SOFT_ABORT("Unsupported texture count");
	}

	return 0;
}

void XrBackend::ClearSkyboxOverride()
{
	OOVR_SOFT_ABORT("No implementation");
}

/* Misc compositor */

/**
 * Get frame timing information to be passed to the application
 *
 * Returns true if successful
 */
bool XrBackend::GetFrameTiming(OOVR_Compositor_FrameTiming* pTiming, uint32_t unFramesAgo)
{
	// Zero everything except the size field
	memset(reinterpret_cast<unsigned char*>(pTiming) + sizeof(pTiming->m_nSize), 0, pTiming->m_nSize - sizeof(pTiming->m_nSize));

	if (pTiming->m_nSize >= sizeof(IVRCompositor_018::Compositor_FrameTiming)) {
		pTiming->m_flSystemTimeInSeconds = frameSubmitTimeUs;
		pTiming->m_nFrameIndex = nFrameIndex;

		// A lot of these values we can't get the data for so just use sensible values
		pTiming->m_nNumFramePresents = 1; // number of times this frame was presented
		pTiming->m_nNumMisPresented = 0; // number of times this frame was presented on a vsync other than it was originally predicted to
		pTiming->m_nNumDroppedFrames = 0; // number of additional times previous frame was scanned out
		pTiming->m_nReprojectionFlags = 0;

		// Just use sensible values until GPU timers implemented
		pTiming->m_flPreSubmitGpuMs = 8.0f;
		pTiming->m_flPostSubmitGpuMs = 1.0f;
		pTiming->m_flTotalRenderGpuMs = 9.0f;

		// Use very conservative guesses for these. They are used in F1 22 for dynamic resolution calculations but are not something that is provided
		// through OpenXR. Using conservative values will give a bit more headroom for the game to target realistic frame times.
		pTiming->m_flCompositorRenderGpuMs = 1.5f; // time spend performing distortion correction, rendering chaperone, overlays, etc.
		pTiming->m_flCompositorRenderCpuMs = 3.0f; // time spent on cpu submitting the above work for this frame

		pTiming->m_flCompositorIdleCpuMs = 0.1f;

		/** Miscellaneous measured intervals. */
		pTiming->m_flClientFrameIntervalMs = 11.1f; // time between calls to WaitGetPoses
		pTiming->m_flPresentCallCpuMs = 0.0f; // time blocked on call to present (usually 0.0, but can go long)
		pTiming->m_flWaitForPresentCpuMs = 0.0f; // time spent spin-waiting for frame index to change (not near-zero indicates wait object failure)
		pTiming->m_flSubmitFrameMs = 0.0f; // time spent in IVRCompositor::Submit (not near-zero indicates driver issue)

		/** The following are all relative to this frame's SystemTimeInSeconds */
		pTiming->m_flWaitGetPosesCalledMs = 0.0f;
		pTiming->m_flNewPosesReadyMs = 0.0f;
		pTiming->m_flNewFrameReadyMs = 0.0f; // second call to IVRCompositor::Submit
		pTiming->m_flCompositorUpdateStartMs = 0.0f;
		pTiming->m_flCompositorUpdateEndMs = 0.0f;
		pTiming->m_flCompositorRenderStartMs = 0.0f;

		GetPrimaryHMD()->GetPose(vr::ETrackingUniverseOrigin::TrackingUniverseSeated, &pTiming->m_HmdPose, ETrackingStateType::TrackingStateType_Rendering);

		return true;
	}

	return false;
}

/* D3D Mirror textures */
/* #if defined(SUPPORT_DX) */
IBackend::openvr_enum_t XrBackend::GetMirrorTextureD3D11(vr::EVREye eEye, void* pD3D11DeviceOrResource, void** ppD3D11ShaderResourceView)
{
	OOVR_SOFT_ABORT("No implementation");
	return 0;
}
void XrBackend::ReleaseMirrorTextureD3D11(void* pD3D11ShaderResourceView)
{
	OOVR_SOFT_ABORT("No implementation");
}
/* #endif */
/** Returns the points of the Play Area. */
bool XrBackend::GetPlayAreaPoints(vr::HmdVector3_t* points, int* count)
{
	if (count)
		*count = 0;

	XrExtent2Df bounds;
	XrResult res = xrGetReferenceSpaceBoundsRect(xr_session.get(), XR_REFERENCE_SPACE_TYPE_STAGE, &bounds);

	if (res == XR_SPACE_BOUNDS_UNAVAILABLE)
		return false;

	OOVR_FAILED_XR_ABORT(res);

	if (count)
		*count = 4;

	// The origin of the free space is centred around the player
	// TODO if we're using the Oculus runtime, grab it's native handle and get the full polygon
	if (points) {
		points[0] = vr::HmdVector3_t{ -bounds.width / 2, 0, -bounds.height / 2 };
		points[1] = vr::HmdVector3_t{ bounds.width / 2, 0, -bounds.height / 2 };
		points[2] = vr::HmdVector3_t{ bounds.width / 2, 0, bounds.height / 2 };
		points[3] = vr::HmdVector3_t{ -bounds.width / 2, 0, bounds.height / 2 };
	}

	return true;
}
/** Determine whether the bounds are showing right now **/
bool XrBackend::AreBoundsVisible()
{
	OOVR_SOFT_ABORT("No implementation");
	return false;
}
/** Set the boundaries to be visible or not (although setting this to false shouldn't affect
 * what happens if the player moves their hands too close and shows it that way) **/
void XrBackend::ForceBoundsVisible(bool status)
{
	OOVR_SOFT_ABORT("No implementation");
}

bool XrBackend::IsInputAvailable()
{
	return sessionState == XR_SESSION_STATE_FOCUSED;
}

void XrBackend::PumpEvents()
{
	BaseInput* input = GetUnsafeBaseInput();
	if (!testOnlyOne && input && !input->AreActionsLoaded() && sessionState == XR_SESSION_STATE_FOCUSED && !hand_left && !hand_right) {
		QueryForInteractionProfile();
		testOnlyOne = true;
	}
	// Poll for OpenXR events
	// TODO filter by session?
	while (true) {
		XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
		XrResult res;
		OOVR_FAILED_XR_ABORT(res = xrPollEvent(xr_instance, &ev));

		if (res == XR_EVENT_UNAVAILABLE) {
			break;
		}

		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			auto* changed = (XrEventDataSessionStateChanged*)&ev;
			OOVR_FALSE_ABORT(changed->session == xr_session.get());
			sessionState = changed->state;

			// Monado bug: it returns 0 for this value (at least for the first two states)
			// Make sure this is actually greater than 0, otherwise this will mess up xr_gbl->GetBestTime()
			if (changed->time > 0 && xr_gbl)
				xr_gbl->latestTime = changed->time;

			OOVR_LOGF("Switch to OpenXR state %d", sessionState);

			switch (sessionState) {
			case XR_SESSION_STATE_READY: {
				OOVR_LOG("Hit ready state, begin session...");
				// Start the session running - this means we're supposed to start submitting frames
				XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
				beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				OOVR_FAILED_XR_ABORT(xrBeginSession(xr_session.get(), &beginInfo));
				sessionActive = true;
				break;
			}
			case XR_SESSION_STATE_STOPPING: {
				// End the session. The session is still valid and we can still query some information
				// from it, but we're not allowed to submit frames anymore. This is done when the engagement
				// sensor detects the user has taken off the headset, for example.
				OOVR_FAILED_XR_ABORT(xrEndSession(xr_session.get()));
				sessionActive = false;
				renderingFrame = false;
				break;
			}
			case XR_SESSION_STATE_EXITING: {
				OOVR_LOGF("Exiting");
				break;
			}
			case XR_SESSION_STATE_LOSS_PENDING: {
				// If the headset is unplugged or the user decides to exit the app
				// TODO just kill the app after awhile, unless it sends a message to stop that - read the OpenVR wiki docs for more info
				VREvent_t quit = { VREvent_Quit };
				auto system = GetBaseSystem();
				if (system)
					system->_EnqueueEvent(quit);
				break;
			}
			default:
				// suppress clion warning about missing branches
				break;
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
			UpdateInteractionProfile();
			break;
		}

	} // while loop

	/*
	   We check for AreActionsLoaded here because:
	   1. Games using legacy input call xrSyncActions every frame anyway, so the runtime should
	      give us an interaction profile without us forcing it
	   2. Games using an action manifest should be calling UpdateActionState every frame, which calls xrSyncActions.
	      This means the only games we wouldn't be able to confidently grab an interaction profile from
	      would be ones where an action manifest is loaded but UpdateActionState is not being called
	      (because the game checks IsTrackedDeviceConnected or something),
	      and hopefully no game like that exists.
	   Some runtimes (WMR) do not instantly return an interaction profile,
	   so we will keep tryinig to query it until it does.

	   Note that we check that the session is focused because this means that the application
	   has already submitted a frame, that frame is visible, and we have input focus.
	   Waiting until the application has input focus allows us to avoid unnecessarily restarting the
	   session when we can't even receive input anyway, as well as before the session is restarted for
	   the temporary session.
   */
}

void XrBackend::OnSessionCreated()
{
	sessionState = XR_SESSION_STATE_UNKNOWN;
	sessionActive = false;
	renderingFrame = false;

	PumpEvents();

	// Wait until we transition to the idle state.
	// This sets the time, so OpenXR calls which use that will work correctly.
	while (sessionState == XR_SESSION_STATE_UNKNOWN) {
		const int durationMs = 250;

		OOVR_LOGF("No session transition yet received, waiting %dms ...", durationMs);

#ifdef _WIN32
		Sleep(durationMs);
#else
		struct timespec ts = { 0, durationMs * 1000000 };
		nanosleep(&ts, &ts);
#endif

		PumpEvents();
	}

	// Start synth submission worker thread (VRNord/CS-Fork extension).
	// Reads opencomposite.ini setting `synthFallbackSingleThread` at this
	// point to decide between worker-thread mode (default) and synchronous-
	// on-caller mode (diagnostic).
	BaseCompositorExt::StartSynthThread();
}

void XrBackend::PrepareForSessionShutdown()
{
	// Phase C2.8c: destroy dual-cycle synth swapchains before primary teardown.
	SynthSwapchain_Shutdown();

	// Stop synth submission worker thread first, before tearing down any
	// session resources it may be using (VRNord/CS-Fork extension).
	BaseCompositorExt::StopSynthThread();

	for (std::unique_ptr<Compositor>& c : compositors) {
		c.reset();
	}
	if (infoSet != XR_NULL_HANDLE) {
		OOVR_FAILED_XR_ABORT(xrDestroyActionSet(infoSet));
		infoSet = XR_NULL_HANDLE;
		infoAction = XR_NULL_HANDLE;
	}
}

// On Android, add an event poll function for use while sleeping
#ifdef ANDROID
void OpenComposite_Android_EventPoll()
{
	BackendManager::Instance().PumpEvents();
}
#endif

bool XrBackend::IsGraphicsConfigured()
{
	return usingApplicationGraphicsAPI;
}

void XrBackend::OnOverlayTexture(const vr::Texture_t* texture)
{
	if (!usingApplicationGraphicsAPI)
		CheckOrInitCompositors(texture);
}

void XrBackend::UpdateInteractionProfile()
{
	struct hand_info {
		const char* pathstr;
		std::unique_ptr<XrController>& controller;
		const XrController::XrControllerType hand;
	};

	hand_info hands[] = {
		{ .pathstr = "/user/hand/left", .controller = hand_left, .hand = XrController::XCT_LEFT },
		{ .pathstr = "/user/hand/right", .controller = hand_right, .hand = XrController::XCT_RIGHT }
	};

	for (hand_info& info : hands) {
		XrInteractionProfileState state{ XR_TYPE_INTERACTION_PROFILE_STATE };
		XrPath path;
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, info.pathstr, &path));
		OOVR_FAILED_XR_ABORT(xrGetCurrentInteractionProfile(xr_session.get(), path, &state));

		// interaction profile detected
		if (state.interactionProfile != XR_NULL_PATH) {
			uint32_t tmp;
			char path_name[XR_MAX_PATH_LENGTH];
			OOVR_FAILED_XR_ABORT(xrPathToString(xr_instance, state.interactionProfile, XR_MAX_PATH_LENGTH, &tmp, path_name));

			for (const std::unique_ptr<InteractionProfile>& profile : InteractionProfile::GetProfileList()) {
				if (profile->GetPath() == path_name) {
					OOVR_LOGF("%s - Using interaction profile: %s", info.pathstr, path_name);
					info.controller = std::make_unique<XrController>(info.hand, *profile);
					hmd->SetInteractionProfile(profile.get());
					BaseSystem* system = GetUnsafeBaseSystem();
					if (system) {
						VREvent_t event = {
							.eventType = VREvent_TrackedDeviceActivated,
							.trackedDeviceIndex = (TrackedDeviceIndex_t)info.hand + 1
						};
						system->_EnqueueEvent(event);
						event = {
							.eventType = VREvent_TrackedDeviceUpdated,
							.trackedDeviceIndex = 0
						};
						system->_EnqueueEvent(event);
					}
					break;
				}
			}
			if (!hand_left && !hand_right) {
				// Runtime returned an unknown interaction profile!
				OOVR_ABORTF("Runtiime unexpectedly returned an unknown interaction profile: %s", path_name);
			}
		} else {
			// interaction profile lost/not detected
			OOVR_LOGF("%s - No interaction profile detected", info.pathstr);
			if (info.controller) {
				info.controller.reset();
				BaseSystem* system = GetUnsafeBaseSystem();
				if (system) {
					VREvent_t event = {
						.eventType = VREvent_TrackedDeviceDeactivated,
						.trackedDeviceIndex = (TrackedDeviceIndex_t)info.hand + 1
					};
					system->_EnqueueEvent(event);
				}
			}
		}
	}
}

void XrBackend::MaybeRestartForInputs()
{
	// if we haven't attached any actions to the session (infoSet or game actions), no need to restart
	BaseInput* input = GetUnsafeBaseInput();
	// if (infoSet == XR_NULL_HANDLE && (!input || !input->AreActionsLoaded()))
	// return;

	OOVR_LOG("Restarting session for inputs...");
	DrvOpenXR::SetupSession();
	OOVR_LOG("Session restart successful!");
}

void XrBackend::QueryForInteractionProfile()
{
	// Note that we want to avoid using BaseInput here because it would allow for games to call GetControllerState before rendering
	// and then we'd have to recreate the session twice, once for the input state and once for when the game submits a frame
	if (subactionPaths[0] == XR_NULL_PATH) {
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, "/user/hand/left", &subactionPaths[0]));
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, "/user/hand/right", &subactionPaths[1]));
	}

	if (infoSet == XR_NULL_HANDLE) {
		OOVR_LOG("Creating infoset");
		CreateInfoSet();
		BindInfoSet();
	}

	// Interaction profiles are updated after xrSyncActions, so we'll try to make the runtime give us one by calling xrSyncActions.
	XrActiveActionSet active[2] = {
		{ .actionSet = infoSet, .subactionPath = subactionPaths[0] },
		{ .actionSet = infoSet, .subactionPath = subactionPaths[1] },
	};

	XrActionsSyncInfo info{ XR_TYPE_ACTIONS_SYNC_INFO };
	info.countActiveActionSets = 2;
	info.activeActionSets = active;

	OOVR_FAILED_XR_ABORT(xrSyncActions(xr_session.get(), &info));
}

void XrBackend::CreateInfoSet()
{
	XrActionSetCreateInfo set_info{ XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(set_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "opencomposite-internal-info-set");
	strcpy_s(set_info.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "OpenComposite internal info set");
	OOVR_FAILED_XR_ABORT(xrCreateActionSet(xr_instance, &set_info, &infoSet));

	XrActionCreateInfo act_info{ XR_TYPE_ACTION_CREATE_INFO };
	strcpy_s(act_info.actionName, XR_MAX_ACTION_NAME_SIZE, "opencomposite-internal-info-act");
	strcpy_s(act_info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "OpenComposite internal info action");
	act_info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
	act_info.countSubactionPaths = std::size(subactionPaths);
	act_info.subactionPaths = subactionPaths;
	OOVR_FAILED_XR_ABORT(xrCreateAction(infoSet, &act_info, &infoAction));
}

void XrBackend::BindInfoSet()
{
	for (const std::unique_ptr<InteractionProfile>& profile : InteractionProfile::GetProfileList()) {
		XrPath interactionProfilePath;
		OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, profile->GetPath().c_str(), &interactionProfilePath));
		XrInteractionProfileSuggestedBinding suggestedBindings{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };

		std::vector<XrActionSuggestedBinding> bindings;
		// grabs the first found paths ending in /click for each subaction path
		for (const std::string& path_name : { "/user/hand/left", "/user/hand/right" }) {
			auto click_path = std::ranges::find_if(profile->GetValidInputPaths(),
			    [&path_name](std::string s) -> bool {
				    return s.find("/click") != s.npos && s.find(path_name) != s.npos;
			    });
			XrPath path;
			OOVR_FAILED_XR_ABORT(xrStringToPath(xr_instance, click_path->c_str(), &path));
			bindings.push_back({ .action = infoAction, .binding = path });

			suggestedBindings.interactionProfile = interactionProfilePath;
			suggestedBindings.suggestedBindings = bindings.data();
			suggestedBindings.countSuggestedBindings = bindings.size();

			OOVR_FAILED_XR_ABORT(xrSuggestInteractionProfileBindings(xr_instance, &suggestedBindings));
		}
	}

	// Attach the info set by itself. We will have to restart the session once the game attaches its real inputs.
	XrSessionActionSetsAttachInfo info{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	info.countActionSets = 1;
	info.actionSets = &infoSet;
	OOVR_FAILED_XR_ABORT(xrAttachSessionActionSets(xr_session.get(), &info));
}

const void* XrBackend::GetCurrentGraphicsBinding()
{
	if (graphicsBinding) {
		return graphicsBinding->asVoid();
	}
	OOVR_FALSE_ABORT(temporaryGraphics);
	return temporaryGraphics->GetGraphicsBinding();
}

#ifdef SUPPORT_VK
void XrBackend::VkGetPhysicalDevice(VkInstance instance, VkPhysicalDevice* out)
{
	*out = VK_NULL_HANDLE;

	TemporaryVk* vk = temporaryGraphics->GetAsVk();
	if (vk == nullptr)
		OOVR_ABORT("Not using temporary Vulkan instance");

	// Find the UUID of the physical device the temporary instance is running on
	VkPhysicalDeviceIDProperties idProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
	VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idProps };
	vkGetPhysicalDeviceProperties2(vk->physicalDevice, &props);

	// Look through all the physical devices on the target instance and find the matching one
	uint32_t devCount;
	OOVR_FAILED_VK_ABORT(vkEnumeratePhysicalDevices(instance, &devCount, nullptr));
	std::vector<VkPhysicalDevice> physicalDevices(devCount);
	OOVR_FAILED_VK_ABORT(vkEnumeratePhysicalDevices(instance, &devCount, physicalDevices.data()));

	for (VkPhysicalDevice phy : physicalDevices) {
		VkPhysicalDeviceIDProperties devIdProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
		VkPhysicalDeviceProperties2 devProps = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &devIdProps };
		vkGetPhysicalDeviceProperties2(phy, &devProps);

		if (memcmp(devIdProps.deviceUUID, idProps.deviceUUID, sizeof(devIdProps.deviceUUID)) != 0)
			continue;

		// Found it
		*out = phy;
		return;
	}

	OOVR_ABORT("Could not find matching Vulkan physical device for instance");
}

#endif
