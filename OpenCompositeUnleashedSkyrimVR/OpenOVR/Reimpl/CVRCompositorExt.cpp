#include "stdafx.h"
#define GENFILE
#include "BaseCommon.h"

// Codegen registration for IVRCompositorExt_001 (CUSTOM-flagged).
// Implementation lives in BaseCompositorExt; the codegen generates a
// proxy class CVRCompositorExt_001 that inherits
//   vr::IVRCompositorExt_001::IVRCompositorExt + CVRCommon
// and forwards each method through a shared_ptr<BaseCompositorExt>.
//
// No hand-implemented methods below — Base method signatures match the
// interface so the generated stubs handle everything.

GEN_INTERFACE("CompositorExt", "001", CUSTOM)

#include "generated/GVRCompositorExt.gen.h"
