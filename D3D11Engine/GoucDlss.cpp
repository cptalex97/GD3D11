#include "pch.h"
#include "GoucDlss.h"
#include "GoucDlssProtocol.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"

#include <dxgi1_2.h>

using Microsoft::WRL::ComPtr;
using namespace GoucDlssWire;

namespace GoucDlss {
namespace {

// NGX values (nvsdk_ngx_defs.h), repeated here so the 32-bit renderer needs no NVIDIA header.
enum : int32_t {
    PQ_MaxPerf = 0, PQ_Balanced = 1, PQ_MaxQuality = 2, PQ_UltraPerformance = 3, PQ_UltraQuality = 4, PQ_DLAA = 5,
};
constexpr uint32_t FLAG_MVLowRes = 1u << 1;
constexpr uint32_t FLAG_DepthInverted = 1u << 3;
constexpr int32_t NGX_SUCCESS = 1;

constexpr DWORD START_TIMEOUT_MS = 20000;   // helper start + NGX init + feature creation
constexpr DWORD FRAME_TIMEOUT_MS = 250;
constexpr DWORD MUTEX_TIMEOUT_MS = 250;
constexpr int MAX_FRAME_FAILURES = 3;

enum class State { Off, Connecting, WaitHello, NeedConfig, WaitConfig, Ready, Failed };

const char* QualityName( int q ) {
    switch ( q ) {
    case PQ_DLAA: return "DLAA";
    case PQ_MaxQuality: return "Quality";
    case PQ_Balanced: return "Balanced";
    case PQ_MaxPerf: return "Performance";
    case PQ_UltraPerformance: return "Ultra Performance";
    case PQ_UltraQuality: return "Ultra Quality";
    }
    return "?";
}

int QualityFromScale( int percent, int renderW, int outputW ) {
    if ( percent >= 100 || renderW >= outputW ) return PQ_DLAA;
    if ( percent >= 66 ) return PQ_MaxQuality;
    if ( percent >= 58 ) return PQ_Balanced;
    if ( percent >= 50 ) return PQ_MaxPerf;
    return PQ_UltraPerformance;
}

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleDir), &self );
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW( self, path, MAX_PATH );
    std::wstring dir( path, n );
    size_t slash = dir.find_last_of( L"\\/" );
    return slash == std::wstring::npos ? std::wstring() : dir.substr( 0, slash + 1 );
}

std::string Narrow( const char* text, size_t cap ) {
    size_t n = 0;
    while ( n < cap && text[n] ) ++n;
    return std::string( text, n );
}

struct SharedTex {
    ComPtr<ID3D11Texture2D> Tex;
    ComPtr<IDXGIKeyedMutex> Mutex;
    ComPtr<ID3D11ShaderResourceView> Srv;
    uint64_t Remote = 0; // handle value inside the helper process, the helper closes it

    void Reset() { Tex.Reset(); Mutex.Reset(); Srv.Reset(); Remote = 0; }
};

struct ConfigKey {
    int RenderW = 0, RenderH = 0, OutputW = 0, OutputH = 0;
    int Quality = -1, Preset = -1;
    DXGI_FORMAT ColorFormat = DXGI_FORMAT_UNKNOWN;

    bool Same( const ConfigKey& o ) const {
        return RenderW == o.RenderW && RenderH == o.RenderH && OutputW == o.OutputW && OutputH == o.OutputH
            && Quality == o.Quality && Preset == o.Preset && ColorFormat == o.ColorFormat;
    }
};

class Bridge {
public:
    TestOptions Opt;

    bool Evaluate( D3D11GraphicsEngine& engine, const FrameInputs& in ) {
        LoadOptionsOnce();
        if ( St == State::Failed ) return false;

        ConfigKey key;
        key.RenderW = in.RenderWidth;
        key.RenderH = in.RenderHeight;
        key.OutputW = in.OutputWidth;
        key.OutputH = in.OutputHeight;
        key.Quality = QualityFromScale( in.ResolutionScalePercent, in.RenderWidth, in.OutputWidth );
        key.Preset = Opt.Preset;
        D3D11_TEXTURE2D_DESC cd{};
        in.Color->GetDesc( &cd );
        key.ColorFormat = cd.Format;

        if ( St == State::Off ) {
            Start( engine );
            return false;
        }
        Poll();
        if ( St == State::NeedConfig || (St == State::Ready && !key.Same( Active )) ) {
            SendConfig( engine, key );
            return false;
        }
        if ( St != State::Ready ) return false;
        return RunFrame( engine, in );
    }

    void Stop() {
        if ( St == State::Off ) return;
        Shutdown();
        St = State::Off;
        Status = "DLSS: off";
    }

    std::string StatusText() const { return Status; }
    bool IsRunning() const { return St == State::Ready; }

    void LoadOptionsOnce() {
        if ( OptLoaded ) return;
        OptLoaded = true;
        Dir = ModuleDir();
        const std::wstring ini = Dir + L"dlss\\gouc_dlss.ini";
        Opt.Preset = GetPrivateProfileIntW( L"DLSS", L"Preset", 0, ini.c_str() );
        Opt.InvertJitterX = GetPrivateProfileIntW( L"DLSS", L"InvertJitterX", 0, ini.c_str() ) != 0;
        Opt.InvertJitterY = GetPrivateProfileIntW( L"DLSS", L"InvertJitterY", 0, ini.c_str() ) != 0;
        Opt.InvertMotionX = GetPrivateProfileIntW( L"DLSS", L"InvertMotionX", 0, ini.c_str() ) != 0;
        Opt.InvertMotionY = GetPrivateProfileIntW( L"DLSS", L"InvertMotionY", 0, ini.c_str() ) != 0;
        HelperLog = GetPrivateProfileIntW( L"DLSS", L"HelperLog", 1, ini.c_str() ) != 0;
    }

private:
    State St = State::Off;
    std::string Status = "DLSS: not started";
    std::string HelperInfo;
    bool OptLoaded = false;
    bool HelperLog = true;
    std::wstring Dir;

    HANDLE Pipe = INVALID_HANDLE_VALUE;
    HANDLE Job = nullptr;
    HANDLE Process = nullptr;
    DWORD ProcessId = 0;
    OVERLAPPED ReadOv{};
    OVERLAPPED WriteOv{};
    bool ReadPending = false;
    DWORD StartTick = 0;
    alignas(8) uint8_t ReadBuf[1024] = {};

    SharedTex Color, Depth, Motion, Out;
    ConfigKey Active, Pending;
    uint32_t FrameIndex = 0;
    bool ResetHistory = true;
    int FrameFailures = 0;
    bool LoggedFirstFrame = false;

    void Fail( const std::string& why ) {
        LogWarn() << "GOUC DLSS: " << why << " - falling back to FSR 3";
        Shutdown();
        St = State::Failed;
        Status = "DLSS unavailable: " + why + " (using FSR 3)";
    }

    void Start( D3D11GraphicsEngine& engine ) {
        const std::wstring exe = Dir + L"dlss\\gouc_dlss_host.exe";
        if ( GetFileAttributesW( exe.c_str() ) == INVALID_FILE_ATTRIBUTES ) {
            Fail( "helper missing (GD3D11\\Bin\\dlss\\gouc_dlss_host.exe)" );
            return;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC ad{};
        if ( FAILED( engine.GetDevice().As( &dxgiDevice ) ) || FAILED( dxgiDevice->GetAdapter( &adapter ) )
            || FAILED( adapter->GetDesc( &ad ) ) ) {
            Fail( "could not query the GPU" );
            return;
        }
        if ( ad.VendorId != 0x10DE ) {
            Fail( "needs an NVIDIA RTX GPU" );
            return;
        }

        wchar_t pipeName[128];
        swprintf_s( pipeName, L"\\\\.\\pipe\\gouc_dlss_%lu_%lu", GetCurrentProcessId(), GetTickCount() );
        Pipe = CreateNamedPipeW( pipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 0, nullptr );
        if ( Pipe == INVALID_HANDLE_VALUE ) {
            Fail( "could not create the pipe" );
            return;
        }
        ReadOv = {};
        WriteOv = {};
        ReadOv.hEvent = CreateEventW( nullptr, TRUE, FALSE, nullptr );
        WriteOv.hEvent = CreateEventW( nullptr, TRUE, FALSE, nullptr );

        // ConnectNamedPipe uses the read OVERLAPPED; the first read is only issued after it completed.
        ReadPending = false;
        if ( !ConnectNamedPipe( Pipe, &ReadOv ) ) {
            DWORD e = GetLastError();
            if ( e == ERROR_IO_PENDING ) {
                ReadPending = true;
            } else if ( e == ERROR_PIPE_CONNECTED ) {
                SetEvent( ReadOv.hEvent );
            } else {
                Fail( "ConnectNamedPipe failed" );
                return;
            }
        }

        // The helper must die with Gothic, whatever happens to us.
        Job = CreateJobObjectW( nullptr, nullptr );
        if ( Job ) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject( Job, JobObjectExtendedLimitInformation, &li, sizeof( li ) );
        }

        wchar_t args[512];
        swprintf_s( args, L"\"%s\" --pipe %s --luid %08lX:%08lX --log %d",
            exe.c_str(), pipeName, static_cast<unsigned long>(ad.AdapterLuid.HighPart),
            static_cast<unsigned long>(ad.AdapterLuid.LowPart), HelperLog ? 1 : 0 );
        std::wstring cmd = args;
        const std::wstring workDir = Dir + L"dlss";

        STARTUPINFOW si{ sizeof( si ) };
        PROCESS_INFORMATION pi{};
        if ( !CreateProcessW( exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, workDir.c_str(), &si, &pi ) ) {
            Fail( "could not start the helper (error " + std::to_string( GetLastError() ) + ")" );
            return;
        }
        if ( Job ) AssignProcessToJobObject( Job, pi.hProcess );
        ResumeThread( pi.hThread );
        CloseHandle( pi.hThread );
        Process = pi.hProcess;
        ProcessId = pi.dwProcessId;

        StartTick = GetTickCount();
        St = State::Connecting;
        Status = "DLSS: starting helper...";
        LogInfo() << "GOUC DLSS: helper started (pid " << ProcessId << ")";
    }

    bool BeginRead() {
        ResetEvent( ReadOv.hEvent );
        ReadPending = true;
        if ( !ReadFile( Pipe, ReadBuf, sizeof( ReadBuf ), nullptr, &ReadOv ) ) {
            DWORD e = GetLastError();
            if ( e != ERROR_IO_PENDING ) {
                ReadPending = false;
                return false;
            }
        }
        return true;
    }

    // true = finished (bytes set); false = still pending. 'broken' is set when the pipe died.
    bool ReadDone( DWORD waitMs, DWORD& bytes, bool& broken ) {
        broken = false;
        bytes = 0;
        if ( WaitForSingleObject( ReadOv.hEvent, waitMs ) != WAIT_OBJECT_0 ) return false;
        ReadPending = false;
        if ( !GetOverlappedResult( Pipe, &ReadOv, &bytes, FALSE ) ) {
            broken = true;
        }
        return true;
    }

    bool Write( const void* data, DWORD size ) {
        ResetEvent( WriteOv.hEvent );
        DWORD written = 0;
        if ( !WriteFile( Pipe, data, size, nullptr, &WriteOv ) ) {
            if ( GetLastError() != ERROR_IO_PENDING ) return false;
            if ( WaitForSingleObject( WriteOv.hEvent, FRAME_TIMEOUT_MS ) != WAIT_OBJECT_0 ) {
                CancelIoEx( Pipe, &WriteOv );
                GetOverlappedResult( Pipe, &WriteOv, &written, TRUE );
                return false;
            }
        }
        return GetOverlappedResult( Pipe, &WriteOv, &written, FALSE ) && written == size;
    }

    bool HeaderOk( DWORD bytes, MsgType type, uint32_t size ) const {
        if ( bytes < sizeof( Header ) ) return false;
        const auto* h = reinterpret_cast<const Header*>(ReadBuf);
        return h->Magic == MAGIC && h->Version == VERSION && h->Type == type && h->Size == size && bytes == size;
    }

    bool HelperGone() {
        if ( Process && WaitForSingleObject( Process, 0 ) == WAIT_OBJECT_0 ) {
            DWORD code = 0;
            GetExitCodeProcess( Process, &code );
            Fail( "helper exited (code " + std::to_string( code ) + ", see dlss\\gouc_dlss_host.log)" );
            return true;
        }
        return false;
    }

    void Poll() {
        DWORD bytes = 0;
        bool broken = false;
        switch ( St ) {
        case State::Connecting:
            if ( ReadDone( 0, bytes, broken ) ) {
                ULONG clientPid = 0;
                if ( broken || !GetNamedPipeClientProcessId( Pipe, &clientPid ) || clientPid != ProcessId ) {
                    Fail( "unexpected pipe client" );
                    return;
                }
                if ( !BeginRead() ) {
                    Fail( "pipe read failed" );
                    return;
                }
                St = State::WaitHello;
                Status = "DLSS: initialising NGX...";
            } else if ( !HelperGone() && GetTickCount() - StartTick > START_TIMEOUT_MS ) {
                Fail( "helper did not connect" );
            }
            return;

        case State::WaitHello:
            if ( ReadDone( 0, bytes, broken ) ) {
                if ( broken || !HeaderOk( bytes, MSG_HELLO, sizeof( MsgHello ) ) ) {
                    if ( !HelperGone() ) Fail( "bad hello from helper" );
                    return;
                }
                const auto* m = reinterpret_cast<const MsgHello*>(ReadBuf);
                HelperInfo = Narrow( m->Text, sizeof( m->Text ) );
                if ( !m->Ok ) {
                    Fail( HelperInfo.empty() ? "DLSS not available" : HelperInfo );
                    return;
                }
                LogInfo() << "GOUC DLSS: helper ready - " << HelperInfo;
                St = State::NeedConfig;
            } else if ( !HelperGone() && GetTickCount() - StartTick > START_TIMEOUT_MS ) {
                Fail( "NGX initialisation timed out" );
            }
            return;

        case State::WaitConfig:
            if ( ReadDone( 0, bytes, broken ) ) {
                if ( broken || !HeaderOk( bytes, MSG_CONFIG_ACK, sizeof( MsgConfigAck ) ) ) {
                    if ( !HelperGone() ) Fail( "bad config answer from helper" );
                    return;
                }
                const auto* m = reinterpret_cast<const MsgConfigAck*>(ReadBuf);
                const std::string text = Narrow( m->Text, sizeof( m->Text ) );
                if ( !m->Ok ) {
                    Fail( text.empty() ? "could not create the DLSS feature" : text );
                    return;
                }
                Active = Pending;
                St = State::Ready;
                ResetHistory = true;
                FrameFailures = 0;
                LoggedFirstFrame = false;
                Status = "DLSS active: " + text;
                LogInfo() << "GOUC DLSS: " << text;
            } else if ( !HelperGone() && GetTickCount() - StartTick > START_TIMEOUT_MS ) {
                Fail( "DLSS feature creation timed out" );
            }
            return;

        default:
            return;
        }
    }

    bool MakeShared( D3D11GraphicsEngine& engine, SharedTex& t, UINT w, UINT h, DXGI_FORMAT fmt, bool srv ) {
        t.Reset();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if ( FAILED( engine.GetDevice()->CreateTexture2D( &d, nullptr, t.Tex.GetAddressOf() ) ) ) return false;
        if ( FAILED( t.Tex.As( &t.Mutex ) ) ) return false;
        if ( srv && FAILED( engine.GetDevice()->CreateShaderResourceView( t.Tex.Get(), nullptr, t.Srv.GetAddressOf() ) ) ) return false;

        ComPtr<IDXGIResource1> res;
        HANDLE local = nullptr;
        if ( FAILED( t.Tex.As( &res ) )
            || FAILED( res->CreateSharedHandle( nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &local ) ) ) {
            return false;
        }
        HANDLE remote = nullptr;
        const BOOL dup = DuplicateHandle( GetCurrentProcess(), local, Process, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS );
        CloseHandle( local );
        if ( !dup ) return false;
        t.Remote = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(remote));
        return true;
    }

    void SendConfig( D3D11GraphicsEngine& engine, const ConfigKey& key ) {
        Color.Reset();
        Depth.Reset();
        Motion.Reset();
        Out.Reset();
        const UINT rw = static_cast<UINT>(key.RenderW), rh = static_cast<UINT>(key.RenderH);
        const UINT ow = static_cast<UINT>(key.OutputW), oh = static_cast<UINT>(key.OutputH);
        if ( !MakeShared( engine, Color, rw, rh, key.ColorFormat, false )
            || !MakeShared( engine, Depth, rw, rh, DXGI_FORMAT_R32_FLOAT, false )
            || !MakeShared( engine, Motion, rw, rh, DXGI_FORMAT_R16G16_FLOAT, false )
            || !MakeShared( engine, Out, ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, true ) ) {
            Fail( "could not create shared textures" );
            return;
        }

        MsgConfig m{};
        m.H = MakeHeader( MSG_CONFIG, sizeof( m ) );
        m.ColorHandle = Color.Remote;
        m.DepthHandle = Depth.Remote;
        m.MotionHandle = Motion.Remote;
        m.OutputHandle = Out.Remote;
        m.RenderWidth = rw;
        m.RenderHeight = rh;
        m.OutputWidth = ow;
        m.OutputHeight = oh;
        m.PerfQuality = key.Quality;
        // Input is the tonemapped LDR image: no IsHDR, no AutoExposure -> DLSS never rescales brightness.
        m.CreateFlags = FLAG_MVLowRes | FLAG_DepthInverted;
        m.Preset = static_cast<uint32_t>(std::max( 0, key.Preset ));
        if ( !Write( &m, sizeof( m ) ) || !BeginRead() ) {
            Fail( "helper did not accept the configuration" );
            return;
        }
        Pending = key;
        StartTick = GetTickCount();
        St = State::WaitConfig;
        Status = std::string( "DLSS: creating feature (" ) + QualityName( key.Quality ) + ")...";
    }

    bool CopyIn( ID3D11DeviceContext* ctx, SharedTex& t, ID3D11Texture2D* src ) {
        if ( t.Mutex->AcquireSync( 0, MUTEX_TIMEOUT_MS ) != S_OK ) return false;
        ctx->CopyResource( t.Tex.Get(), src );
        t.Mutex->ReleaseSync( 1 );
        return true;
    }

    bool RunFrame( D3D11GraphicsEngine& engine, const FrameInputs& in ) {
        ID3D11DeviceContext* ctx = engine.GetContext().Get();
        if ( !CopyIn( ctx, Color, in.Color ) || !CopyIn( ctx, Depth, in.Depth ) || !CopyIn( ctx, Motion, in.Motion ) ) {
            Fail( "input textures stayed locked" );
            return false;
        }

        MsgFrame f{};
        f.H = MakeHeader( MSG_FRAME, sizeof( f ) );
        f.FrameIndex = ++FrameIndex;
        f.Reset = ResetHistory ? 1 : 0;
        f.JitterX = Opt.InvertJitterX ? -in.JitterX : in.JitterX;
        f.JitterY = Opt.InvertJitterY ? -in.JitterY : in.JitterY;
        // Velocity buffer holds prevUV - currUV; DLSS wants render pixels, same direction.
        f.MotionScaleX = static_cast<float>(in.RenderWidth) * (Opt.InvertMotionX ? -1.0f : 1.0f);
        f.MotionScaleY = static_cast<float>(in.RenderHeight) * (Opt.InvertMotionY ? -1.0f : 1.0f);
        f.FrameTimeMs = in.FrameTimeMs;
        f.RenderWidth = static_cast<uint32_t>(in.RenderWidth);
        f.RenderHeight = static_cast<uint32_t>(in.RenderHeight);
        if ( !Write( &f, sizeof( f ) ) || !BeginRead() ) {
            if ( !HelperGone() ) Fail( "helper pipe closed" );
            return false;
        }

        DWORD bytes = 0;
        bool broken = false;
        if ( !ReadDone( FRAME_TIMEOUT_MS, bytes, broken ) ) {
            if ( !HelperGone() ) Fail( "helper did not answer in time" );
            return false;
        }
        if ( broken || !HeaderOk( bytes, MSG_FRAME_ACK, sizeof( MsgFrameAck ) ) ) {
            if ( !HelperGone() ) Fail( "bad frame answer from helper" );
            return false;
        }
        const auto* ack = reinterpret_cast<const MsgFrameAck*>(ReadBuf);
        const bool ok = ack->NgxResult == NGX_SUCCESS && ack->FrameIndex == f.FrameIndex;

        if ( Out.Mutex->AcquireSync( 1, MUTEX_TIMEOUT_MS ) != S_OK ) {
            Fail( "output texture stayed locked" );
            return false;
        }
        if ( ok ) {
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv( in.Output );
            engine.GetPfxRenderer()->CopyTextureToRTV( Out.Srv, rtv, INT2( in.OutputWidth, in.OutputHeight ) );
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ctx->PSSetShaderResources( 0, 1, &nullSrv );
        }
        Out.Mutex->ReleaseSync( 0 );

        if ( !ok ) {
            char hex[16];
            sprintf_s( hex, "0x%08X", static_cast<unsigned>(ack->NgxResult) );
            LogWarn() << "GOUC DLSS: evaluate failed, NGX result " << hex;
            if ( ++FrameFailures >= MAX_FRAME_FAILURES ) {
                Fail( std::string( "DLSS evaluate failed (NGX " ) + hex + ")" );
            }
            return false;
        }
        FrameFailures = 0;
        ResetHistory = false;
        if ( !LoggedFirstFrame ) {
            LoggedFirstFrame = true;
            LogInfo() << "GOUC DLSS: first frame upscaled " << in.RenderWidth << "x" << in.RenderHeight
                << " -> " << in.OutputWidth << "x" << in.OutputHeight;
        }
        return true;
    }

    void Shutdown() {
        if ( Pipe != INVALID_HANDLE_VALUE ) {
            if ( St == State::Ready || St == State::WaitConfig || St == State::NeedConfig ) {
                const Header quit = MakeHeader( MSG_QUIT, sizeof( Header ) );
                Write( &quit, sizeof( quit ) );
            }
            CancelIoEx( Pipe, nullptr );
            DWORD dummy = 0;
            if ( ReadPending ) GetOverlappedResult( Pipe, &ReadOv, &dummy, TRUE );
            ReadPending = false;
            CloseHandle( Pipe );
            Pipe = INVALID_HANDLE_VALUE;
        }
        if ( Process ) {
            WaitForSingleObject( Process, 500 );
            CloseHandle( Process );
            Process = nullptr;
            ProcessId = 0;
        }
        if ( Job ) {
            CloseHandle( Job ); // kills the helper if it is still alive
            Job = nullptr;
        }
        if ( ReadOv.hEvent ) { CloseHandle( ReadOv.hEvent ); ReadOv.hEvent = nullptr; }
        if ( WriteOv.hEvent ) { CloseHandle( WriteOv.hEvent ); WriteOv.hEvent = nullptr; }
        Color.Reset();
        Depth.Reset();
        Motion.Reset();
        Out.Reset();
        Active = ConfigKey();
    }
};

// Never destroyed: the helper dies with the process through the job object, and releasing D3D
// objects during static destruction is not safe.
Bridge& Get() {
    static Bridge* bridge = new Bridge();
    return *bridge;
}

} // namespace

bool Evaluate( D3D11GraphicsEngine& engine, const FrameInputs& in ) {
    if ( !in.Color || !in.Depth || !in.Motion || !in.Output ) return false;
    return Get().Evaluate( engine, in );
}

void Stop() { Get().Stop(); }
std::string StatusText() { return Get().StatusText(); }
bool IsRunning() { return Get().IsRunning(); }
TestOptions& Options() {
    Get().LoadOptionsOnce();
    return Get().Opt;
}

} // namespace GoucDlss
