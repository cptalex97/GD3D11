#pragma once
// ---------------------------------------------------------------------------------------------
// GOUC DLSS bridge -- wire protocol between the 32-bit renderer (g2a*.dll) and the 64-bit helper
// (GD3D11\Bin\dlss\gouc_dlss_host.exe). Shared by both builds, so every field has a fixed size
// and the layout is identical for x86 and x64 (no pointers, no size_t, explicit padding).
//
// Why a second process at all: NVIDIA ships DLSS (NGX, nvngx_dlss.dll) for x64 only, Gothic and
// GD3D11 are 32-bit. Frames never touch system memory: the renderer creates the textures with NT
// share handles + keyed mutexes, duplicates the handles into the helper and only small messages go
// through the named pipe.
//
// License: MIT (see DlssHost/LICENSE), so the GPL renderer and the separately licensed helper can
// both include it. The helper itself links NVIDIA's DLSS SDK, which must never end up in the GPL part.
//
// Keyed-mutex protocol (one key pair per texture):
//   inputs (colour, depth, motion vectors):  renderer writes under key 0, releases to 1;
//                                            helper reads under key 1, releases back to 0.
//   output:                                  helper writes under key 0, releases to 1;
//                                            renderer reads under key 1, releases back to 0.
// ---------------------------------------------------------------------------------------------

#include <cstdint>

namespace GoucDlssWire {

constexpr uint32_t MAGIC   = 0x534C4447; // 'GDLS'
constexpr uint32_t VERSION = 1;

enum MsgType : uint32_t {
    MSG_HELLO      = 1,   // helper -> renderer: NGX initialised? DLSS available?
    MSG_CONFIG     = 2,   // renderer -> helper: shared textures + sizes, (re)create the feature
    MSG_CONFIG_ACK = 3,   // helper -> renderer: feature created?
    MSG_FRAME      = 4,   // renderer -> helper: evaluate one frame
    MSG_FRAME_ACK  = 5,   // helper -> renderer: evaluate result (read without blocking)
    MSG_QUIT       = 6,   // renderer -> helper: shut down
};

struct Header {
    uint32_t Magic;
    uint32_t Version;
    uint32_t Type;
    uint32_t Size;        // total size of the message including this header
};

struct MsgHello {
    Header   H;
    uint32_t Ok;          // 1 = NGX initialised and DLSS super resolution is available
    int32_t  NgxResult;   // NVSDK_NGX_Result of the failing call (or of the availability query)
    char     Text[256];   // human readable reason, shown in the renderer menu
};

struct MsgConfig {
    Header   H;
    uint64_t ColorHandle; // NT handles, already duplicated into the helper process
    uint64_t DepthHandle;
    uint64_t MotionHandle;
    uint64_t OutputHandle;
    uint32_t RenderWidth, RenderHeight;   // DLSS input size (render resolution)
    uint32_t OutputWidth, OutputHeight;   // DLSS output size (display resolution)
    int32_t  PerfQuality;                 // NVSDK_NGX_PerfQuality_Value
    uint32_t CreateFlags;                 // NVSDK_NGX_DLSS_Feature_Flags
    uint32_t Preset;                      // NVSDK_NGX_DLSS_Hint_Render_Preset, 0 = default
    uint32_t Reserved;
};

struct MsgConfigAck {
    Header   H;
    uint32_t Ok;
    int32_t  NgxResult;
    char     Text[256];
};

struct MsgFrame {
    Header   H;
    uint32_t FrameIndex;
    uint32_t Reset;                       // 1 = drop history (first frame, resize, teleport)
    float    JitterX, JitterY;            // render pixels
    float    MotionScaleX, MotionScaleY;  // multiplies the motion vectors into render pixels
    float    FrameTimeMs;
    uint32_t RenderWidth, RenderHeight;   // render subrect actually used this frame
    uint32_t Reserved;
};

struct MsgFrameAck {
    Header   H;
    uint32_t FrameIndex;
    int32_t  NgxResult;
};

inline Header MakeHeader( MsgType type, uint32_t size ) {
    return Header{ MAGIC, VERSION, static_cast<uint32_t>(type), size };
}

} // namespace GoucDlssWire
