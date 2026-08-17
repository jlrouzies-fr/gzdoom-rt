#ifndef NOMINMAX
    #define NOMINMAX
#endif

#include "i_mainwindow.h"
#include "i_time.h"
#include "m_argv.h"
#include "win32rtvideo.h"

#include "base_sysfb.h"
#include "c_dispatch.h"
#include "hw_renderstate.h"
#include "g_levellocals.h"
#include "a_dynlight.h"
#include "r_utility.h"
#include "v_draw.h"
#include "flatvertices.h"
#include "hw_bonebuffer.h"
#include "hw_lightbuffer.h"
#include "hw_skydome.h"
#include "hw_viewpointbuffer.h"
#include "i_modelvertexbuffer.h"
#include "p_lnspec.h"
// rt_dump_lightthinkers: DLighting and its subclasses, so a running light effect can be
// named at runtime when the map file does not explain one.
#include "mapthinkers/a_lights.h"
#include "image.h"
#include "texturemanager.h"
#include "actor.h"
#include "d_player.h" // player_t::ReadyWeapon, for RT_AddWeaponGlow
// whatsthat: name the surface under the crosshair instead of guessing it from a
// screenshot. P_LineTrace + the hit's texture/sector.
#include "p_linetracedata.h"
#include "p_local.h"
#include "r_state.h"

#include "rt_state.h"

#include <shellapi.h>

#include <filesystem>
#include <cmath>
#include <random>
#include <span>
#include <variant>
#include <ranges>
#include <unordered_map>
#include <unordered_set>


//
//
//
//
//
//

#define RG_USE_SURFACE_WIN32
#include <RTGL1/RTGL1.h>

// Generated fist offsets + colours for RT_UploadHandGlowLights.
#include "rt_hand_lights.h"

// Generated lit-switch-face table for RT_UploadSwitchLights.
#include "rt_switch_lights.h"

RgInterface rt      = {};
FRtState    rtstate = {};

bool g_isremix{ false };

//
//
//
//
//
//

#include "rt_cvars.h"
#include "rt_internal.h"
#include "rt_buffers.h"
#include "rt_renderstate.h"


EXTERN_CVAR( Float, blood_fade_scalar );
EXTERN_CVAR( Float, pickup_fade_scalar );

//
//
//
//
//
//

const char* g_rt_cutscenename        = nullptr;
bool        g_rt_showfirststartscene = false;
int         g_rt_skipinitframes      = -10; // to prevent flashing when starting the game
bool        g_rt_forcenofocuschange  = true;
int         rt_cullmode              = 2; // 0 -- balanced,  1 -- original gzdoom,  2 -- none

extern float RT_CutsceneTime();
extern void  RT_ForceIntroCutsceneMusicStop();

extern void RT_CloseLauncherWindow();

auto RT_MakeUpRightForwardVectors( const DRotator& rotation ) -> std::tuple< RgFloat3D, RgFloat3D, RgFloat3D >;

// Called from rt_presets.cpp and from the CCMDs that moved out of here, so it
// cannot live in the anonymous namespace below.
const char* RT_GetMapName()
{
    if( g_rt_cutscenename && g_rt_cutscenename[ 0 ] != '\0' )
    {
        return g_rt_cutscenename;
    }

    if( primaryLevel && !primaryLevel->RT_MapName.IsEmpty() )
    {
        // Official modcompat: RT_MapName is set in p_openmap for PWAD maps
        // so Doom II rt/scenes/map## do not collide with mod MAP01 etc.
        return primaryLevel->RT_MapName.GetChars();
    }

    if( g_rt_showfirststartscene )
    {
        // HACKHACK: do not show scene at the first frame: cutscene's firststart::draw is not called at that time :(
        static bool HACKHACK_firstframeskipped = false;
        if( !HACKHACK_firstframeskipped )
        {
            HACKHACK_firstframeskipped = true;
            return nullptr;
        }

        return "mainmenu";
    }

    return nullptr;
}

bool RT_ForceNoClassicMode()
{
    if( g_rt_cutscenename && g_rt_cutscenename[ 0 ] != '\0' )
    {
        return true;
    }
    if( g_rt_showfirststartscene )
    {
        return true;
    }
    return false;
}

// Doom64-RT: where are the sky openings, in world units?
//
// The leak hunt kept stalling on "which opening is feeding this room", a question
// no screenshot answers on its own -- the sky geometry that admits the light is
// usually not the sky geometry you can see. So every SKY_VISIBILITY primitive
// submitted this frame is recorded here with its bounding box, and `rt_sky_here`
// prints the ones nearest the camera.
//
// Recorded per frame rather than per level because sky portals are submitted by
// the renderer as it walks the BSP -- the set depends on where you are standing,
// which is exactly what makes it useful.
namespace
{
struct SkyPrimNote
{
    float min[ 3 ];
    float max[ 3 ];
};

std::vector< SkyPrimNote > g_skyprims;
std::vector< SkyPrimNote > g_skyprims_prev;
} // namespace

void RT_NoteSkyPrim( std::span< const RgPrimitiveVertex > verts )
{
    if( !bool{ cvar::rt_sky_log } || verts.empty() )
    {
        return;
    }
    SkyPrimNote n{};
    for( int c = 0; c < 3; c++ )
    {
        n.min[ c ] = n.max[ c ] = verts[ 0 ].position[ c ];
    }
    for( const auto& v : verts )
    {
        for( int c = 0; c < 3; c++ )
        {
            n.min[ c ] = std::min( n.min[ c ], v.position[ c ] );
            n.max[ c ] = std::max( n.max[ c ], v.position[ c ] );
        }
    }
    if( g_skyprims.size() < 4096 )
    {
        g_skyprims.push_back( n );
    }
}

void RT_SkyPrimsEndFrame()
{
    g_skyprims_prev.swap( g_skyprims );
    g_skyprims.clear();
}

namespace
{

// RG_CHECK, ONEGAMEUNIT_IN_METERS, RT_SectorHue, the light-ID bases and the rest
// of the shared internals now live in rt_internal.h so the feature files split
// out of here can see them too. Pulled in unqualified so nothing below changed.
using namespace rtx;




// RT_BIT, the powerup flags and the colour/gamma helpers moved to rt_internal.h,
// so rt_buffers.h and the split draw path can see them.




// VectorAsBuffer, RTVertexBuffer, RTIndexBuffer and RTHardwareTexture moved to
// rt_buffers.h, included at the top. RTDataBuffer stays below: it reads the
// viewpoint matrices off RTRenderState, so it has to follow that class.


// The matrix helpers moved to rt_internal.h -- rt_draw.cpp needs them.

// RTFrameBuffer and RTRenderState moved to rt_renderstate.h, with their two
// largest members split off into rt_draw.cpp (InternalDraw) and rt_weapon.cpp
// (the first-person weapon lighting).



class RTDataBuffer
    : public IDataBuffer
    , public VectorAsBuffer
{
    void BindRange( FRenderState* state, size_t start, size_t length ) override
    {
        auto hwstate = static_cast< RTRenderState* >( state );

        // ugly way to fetch viewpoint info
        if( this == hwstate->m_fb->mViewpoints->DataBuffer() )
        {
            const HWViewpointUniforms& vp = hwstate->m_fb->mViewpoints->FetchViewpoint( start );
            hwstate->RT_SetMatrices( vp.mViewMatrix, vp.mProjectionMatrix );
        }
    }
};



void RT_Print( const char* pMessage, RgMessageSeverityFlags flags, void* pUserData )
{
    if( !pMessage )
    {
        DPrintf( DMSG_ERROR, "RT_Print: pMessage is NULL\n" );
        return;
    }

    if( flags & RG_MESSAGE_SEVERITY_ERROR )
    {
        DPrintf( DMSG_ERROR, "%s\n", pMessage );

#ifdef WIN32
        static bool g_breakOnError = true;
        if( g_breakOnError )
        {
            auto msg = std::string_view{ pMessage };
            auto str = std::format( "{}{}\n"
                                    "\n\'Abort\' to exit the game."
                                    "\n\'Retry\' to skip only this error message."
                                    "\n\'Ignore\' to ignore all such error messages.",
                                    msg,
                                    msg.ends_with( '.' ) ? "" : "." );

            int ok = MessageBoxA( nullptr,
                                  str.c_str(), // null-terminated
                                  "Renderer Error",
                                  MB_ABORTRETRYIGNORE | MB_DEFBUTTON2 | MB_ICONERROR );
            switch( ok )
            {
                case IDIGNORE: g_breakOnError = false; break;
                case IDRETRY: break;
                case IDABORT:
                default: exit( -1 );
            }
        }
#endif
    }
    else if( flags & RG_MESSAGE_SEVERITY_WARNING )
    {
        // Printf, not DPrintf: DPrintf( DMSG_WARNING, ... ) is additionally
        // gated behind gzdoom's `developer` cvar being >= 2, which was a third
        // independent layer of silence on top of RgInstanceCreateInfo::
        // allowedMessages and RTGL's own g_printSeverity. Renderer warnings
        // (DLSS-RR failing to initialise, denoiser path changing) must reach
        // the console and the logfile unconditionally -- muting them by
        // default is what hid the compiled-out-RR bug for an entire
        // investigation.
        //
        // RT_DiagPrintLevel() adds PRINT_NONOTIFY under `rt_verbose 0` (the
        // release default), which takes these off the on-screen notify overlay
        // WITHOUT taking them out of the console buffer or the logfile -- so
        // the reasoning above still holds, while a release build stops painting
        // "Denoiser path: ...", "ReSTIR: initialSamples=..." and friends across
        // the picture on every level load. One line, and it covers every
        // message RTGL1 emits.
        Printf( RT_DiagPrintLevel(), "%s\n", pMessage );
    }
    else if( flags & RG_MESSAGE_SEVERITY_INFO )
    {
        DPrintf( DMSG_NOTIFY, "%s\n", pMessage );
    }
    else
    {
        DPrintf( DMSG_SPAMMY, "%s\n", pMessage );
    }
}

} // anonymous namespace

bool RT_ModMapNeedsLiveGeometryUpload()
{
    // Does this map have baked rt/scenes geometry to fall back on? If not, the
    // world has to be uploaded live or it is not drawn AT ALL -- walls and flats
    // are skipped as "static exportables" and the player is left looking at the
    // sky dome with only sprites in it.
    //
    // THE UNDERSCORE TEST THIS REPLACES WAS A PROXY, AND IT WAS WRONG TWICE.
    // It read "name contains '_' => PWAD => no scene", which covers Retribution
    // (d64rtr_v15_map01) but silently assumes every plain `map01` HAS a scene.
    // That assumption is a property of the install, not of the name: this tree
    // ships Retribution's rt/scenes and keeps Doom II's in scenes_doom2_backup,
    // so stock doom2.wad rendered as pure sky. It also fails the other way for
    // any PWAD that DOES ship a scene, whose baked geometry would be ignored.
    //
    // Asking the filesystem answers both, and it is the same question RTGL1
    // itself asks when it loads rt/scenes/<name>/<name>.gltf.
    const char* mapname = RT_GetMapName();
    if( mapname == nullptr || mapname[ 0 ] == '\0' )
    {
        return false;
    }

    // Cached because this is called per SEG, per frame -- tens of thousands of
    // times a second -- and a stat() on each would be a hitch, not a cost.
    // Keyed on the name so it re-resolves on level change and nothing else.
    static std::string g_cached_mapname;
    static bool        g_cached_needslive = false;

    if( g_cached_mapname != mapname )
    {
        g_cached_mapname = mapname;

        std::error_code ec;
        const auto      scene = std::filesystem::path{ "rt" } / "scenes" / mapname /
                           ( std::string{ mapname } + ".gltf" );

        g_cached_needslive = !std::filesystem::exists( scene, ec );

        Printf( RT_DiagPrintLevel(),
                "RT geometry: %s -- %s\n",
                mapname,
                g_cached_needslive ? "no baked scene, uploading world live"
                                   : "baked scene found, static geometry" );
    }

    return g_cached_needslive;
}

#ifdef _WIN32
std::atomic< HWND > g_msgbox_parent{};
#endif



//
//
//
//
//
//



RG_D3D12CORE_HELPER( "rt/" )

Win32RTVideo::Win32RTVideo()
{
    extern std::atomic_bool g_continueMain;
    extern std::atomic_bool g_forceLnchThreadStop;
    while( !g_continueMain )
    {
    }
    if( g_forceLnchThreadStop.load() )
    {
        exit( 1 );
    }

    // warn if no needed dll-s
    if( !Args->CheckParm( "-nodllcheck" ) )
    {
        enum rt_feature_flag_t
        {
            RT_FEATURE_FSR2     = 1,
            RT_FEATURE_FSR3_FG  = 2,
            RT_FEATURE_DLSS2    = 4,
            RT_FEATURE_DLSS3_FG = 8,
            RT_FEATURE_DLSS_RR  = 16,
        };

        const std::pair< std::filesystem::path, int > dlls[] = {
            { "rt/bin/D3D12Core.dll", RT_FEATURE_FSR3_FG | RT_FEATURE_DLSS3_FG },
            { "rt/bin/nvngx_dlss.dll", RT_FEATURE_DLSS2 },
            { "rt/bin/nvngx_dlssd.dll", RT_FEATURE_DLSS_RR },
            { "rt/bin/nvngx_dlssg.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/NvLowLatencyVk.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/sl.dlss.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/sl.dlss_g.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/sl.reflex.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/sl.pcl.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/sl.common.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/sl.interposer.dll", RT_FEATURE_DLSS3_FG },
            { "rt/bin/ffx_fsr2_x64.dll", RT_FEATURE_FSR2 },
            { "rt/bin/ffx_fsr3_x64.dll", RT_FEATURE_FSR3_FG },
            { "rt/bin/ffx_fsr3upscaler_x64.dll", RT_FEATURE_FSR3_FG },
            { "rt/bin/ffx_frameinterpolation_x64.dll", RT_FEATURE_FSR3_FG },
            { "rt/bin/ffx_opticalflow_x64.dll", RT_FEATURE_FSR3_FG },
            { "rt/bin/ffx_backend_dx12_x64.dll", RT_FEATURE_FSR3_FG },
            { "rt/bin/ffx_backend_vk_x64.dll", RT_FEATURE_FSR2 | RT_FEATURE_FSR3_FG },
        };

        auto failedPaths    = std::string{};
        int  failedFeatures = 0;
        for( const auto& [ dll, feature ] : dlls )
        {
            if( !exists( dll ) )
            {
                failedPaths += "    " + dll.filename().string() + '\n';
                failedFeatures |= feature;
            }
        }

        if( !failedPaths.empty() )
        {
            auto msg = std::string{};

            if( failedFeatures == 0 )
            {
                msg = "Some features will NOT be available!";
            }
            else
            {
                // clang-format off
                if( failedFeatures & RT_FEATURE_DLSS3_FG) msg += "NVIDIA DLSS3 (AI Frame Generation)\n";
                if( failedFeatures & RT_FEATURE_DLSS2   ) msg += "NVIDIA DLSS2 (AI Upscaling)\n";
                if( failedFeatures & RT_FEATURE_DLSS_RR ) msg += "NVIDIA DLSS Ray Reconstruction\n";
                if( failedFeatures & RT_FEATURE_FSR3_FG ) msg += "AMD FSR 3 (Frame Generation)\n";
                if( failedFeatures & RT_FEATURE_FSR2    ) msg += "AMD FSR 2 (Upscaling)\n";
                // clang-format on
                msg += "                                   will NOT be available!\n";
            }

            msg += "Reason: \'rt/bin/\' folder doesn't contain:\n";
            msg += failedPaths;
            // msg += "\n(To suppress this warning, use \'-nodllcheck\' argument)";
            msg += "\n\nDo you want to download the missing files?\n";
            msg += "\nYES - open renderer's Download page";
            msg += "\nNO  - proceed with a limited feature set";
            
            int l = MessageBoxA( g_msgbox_parent.load(),
                                 msg.c_str(),
                                 "DLL check failure",
                                 MB_ICONEXCLAMATION | MB_YESNO );
            if( l == IDYES )
            {
                ShellExecute(
                    nullptr, 0, L"https://github.com/vs-shirokii/RTGL/releases", 0, 0, SW_SHOW );
                exit( -1 );
            }
        }
    }

    rt = RgInterface{};

#ifdef WIN32
    auto win32Info = RgWin32SurfaceCreateInfo{
        .hinstance = GetModuleHandle( NULL ),
        .hwnd      = mainwindow.GetHandle(),
    };
#else
    RgXlibSurfaceCreateInfo x11Info = { .dpy    = wmInfo.info.x11.display,
                                        .window = wmInfo.info.x11.window };
#endif

    auto info = RgInstanceCreateInfo
    {
        .sType = RG_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pNext = NULL,

        .version = RG_RTGL_VERSION_API, .sizeOfRgInterface = sizeof( RgInterface ),

        .pAppName = "GZDoom", .pAppGUID = "8cbd354f-38d3-4173-92b9-c16b5a210b37",

#if WIN32
        .pWin32SurfaceInfo = &win32Info,
#else
        .pXlibSurfaceCreateInfo  = &x11Info,
#endif

        .pOverrideFolderPath = "rt/",

        .pfnPrint = RT_Print, .pUserPrintData = nullptr,
        // WARNING and ERROR are ALWAYS allowed; -rtdebug only adds the chatty
        // VERBOSE/INFO stream. This used to be `0` without -rtdebug, i.e. RTGL
        // failures were muted by default -- which is precisely how "DLSS-RR was
        // compiled out of RTGL1.dll" survived undetected (every DLSSRR: failure
        // string was suppressed), and how a null nvDlssRr still silently falls
        // back to A-SVGF today. A renderer must never swallow its own errors.
        .allowedMessages =
            Args->CheckParm( "-rtdebug" )
                ? RgMessageSeverityFlags{ RG_MESSAGE_SEVERITY_VERBOSE | RG_MESSAGE_SEVERITY_INFO |
                                          RG_MESSAGE_SEVERITY_WARNING | RG_MESSAGE_SEVERITY_ERROR }
                : RgMessageSeverityFlags{ RG_MESSAGE_SEVERITY_WARNING |
                                          RG_MESSAGE_SEVERITY_ERROR },

        .primaryRaysMaxAlbedoLayers = 1, .indirectIlluminationMaxAlbedoLayers = 1,

        .replacementsMaxVertexCount = 32 * 1024 * 1024, .dynamicMaxVertexCount = 2 * 1024 * 1024,

        .rayCullBackFacingTriangles = 0,
        .allowTexCoordLayer1 = false, .allowTexCoordLayer2 = false, .allowTexCoordLayer3 = false,

        .lightmapTexCoordLayerIndex = 1,

        .rasterizedMaxVertexCount = 1 << 20, .rasterizedMaxIndexCount = 1 << 21,
        .rasterizedVertexColorGamma = true,

        .rasterizedSkyCubemapSize = 256,

        .textureSamplerForceMinificationFilterLinear = true,
        .textureSamplerForceNormalMapFilterLinear    = true,

        .pbrTextureSwizzling = RG_TEXTURE_SWIZZLING_NULL_ROUGHNESS_METALLIC,

        .effectWipeIsUsed = true,

        .worldUp = { 0, 0, 1 }, .worldForward = { 0, 1, 0 }, .worldScale = 1.0f,

        .importedLightIntensityScaleDirectional = 1.0f / 50,
        .importedLightIntensityScaleSphere      = 1.0f / 500,
        .importedLightIntensityScaleSpot        = 1.0f / 500,
    };

#ifndef NDEBUG
    constexpr bool isdebug = true;
#else
    constexpr bool isdebug = false;
#endif

    const char* remixdll = g_isremix ? "\\bin_remix\\RTGL1.dll" : nullptr;

    RgResult r = rgLoadLibraryAndCreate( &info, isdebug, remixdll, &rt, nullptr );
    if( r != RG_RESULT_SUCCESS )
    {
        auto msg = std::string{ "RgResult code: " };

        switch( r )
        {
            case RG_RESULT_CANT_FIND_DYNAMIC_LIBRARY:
                msg = remixdll  ? "Can't load Remix Renderer DLLs"
                      : isdebug ? "Can't find \'rt/bin/debug/RTGL1.dll\' file"
                                : "Can't find \'rt/bin/RTGL1.dll\' file";
                break;
            case RG_RESULT_CANT_FIND_ENTRY_FUNCTION_IN_DYNAMIC_LIBRARY:
                msg =
                    remixdll  ? "Can't find rgCreateInstance function in Remix Renderer wrapper DLL"
                    : isdebug ? "Can't find rgCreateInstance function in \'rt/bin/debug/RTGL1.dll\'"
                              : "Can't find rgCreateInstance function in \'rt/bin/RTGL1.dll\'";
                break;

            // clang-format off
            case RG_RESULT_NOT_INITIALIZED:                     msg += "RG_RESULT_NOT_INITIALIZED";                     break;
            case RG_RESULT_ALREADY_INITIALIZED:                 msg += "RG_RESULT_ALREADY_INITIALIZED";                 break;
            case RG_RESULT_GRAPHICS_API_ERROR:                  msg += "RG_RESULT_GRAPHICS_API_ERROR";                  break;
            case RG_RESULT_INTERNAL_ERROR:                      msg += "RG_RESULT_INTERNAL_ERROR";                      break;
            case RG_RESULT_CANT_FIND_SUPPORTED_PHYSICAL_DEVICE: msg += "RG_RESULT_CANT_FIND_SUPPORTED_PHYSICAL_DEVICE"; break;
            case RG_RESULT_FRAME_WASNT_STARTED:                 msg += "RG_RESULT_FRAME_WASNT_STARTED";                 break;
            case RG_RESULT_FRAME_WASNT_ENDED:                   msg += "RG_RESULT_FRAME_WASNT_ENDED";                   break;
            case RG_RESULT_WRONG_FUNCTION_CALL:                 msg += "RG_RESULT_WRONG_FUNCTION_CALL";                 break;
            case RG_RESULT_WRONG_FUNCTION_ARGUMENT:             msg += "RG_RESULT_WRONG_FUNCTION_ARGUMENT";             break;
            case RG_RESULT_WRONG_STRUCTURE_TYPE:                msg += "RG_RESULT_WRONG_STRUCTURE_TYPE";                break;
            case RG_RESULT_ERROR_CANT_FIND_HARDCODED_RESOURCES: msg += "RG_RESULT_ERROR_CANT_FIND_HARDCODED_RESOURCES"; break;
            case RG_RESULT_ERROR_CANT_FIND_SHADER:              msg += "RG_RESULT_ERROR_CANT_FIND_SHADER";              break;
            case RG_RESULT_ERROR_MEMORY_ALIGNMENT:              msg += "RG_RESULT_ERROR_MEMORY_ALIGNMENT";              break;
            case RG_RESULT_ERROR_NO_VULKAN_EXTENSION:           msg += "RG_RESULT_ERROR_NO_VULKAN_EXTENSION";           break;
                // clang-format on

            default: msg += std::to_string( r ); break;
        }

        MessageBoxA(
            nullptr, msg.c_str(), "Failed to initialize RT renderer", MB_ICONEXCLAMATION | MB_OK );
        exit( -1 );
    }

    // on first start, try to set DLSS, if available
    if( cvar::rt_firststart )
    {
        if( rt.rgUtilIsUpscaleTechniqueAvailable( RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS, //
                                                  RG_FRAME_GENERATION_MODE_OFF,
                                                  nullptr ) )
        {
            cvar::rt_upscale_dlss = 2;
            cvar::rt_upscale_fsr2 = 0;
            cvar::rt_remix_taa    = 0;
            cvar::rt_ef_vintage   = 0;
        }
        else if( rt.rgUtilIsUpscaleTechniqueAvailable( RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2, //
                                                       RG_FRAME_GENERATION_MODE_OFF,
                                                       nullptr ) )
        {
            cvar::rt_upscale_dlss = 0;
            cvar::rt_upscale_fsr2 = 2;
            cvar::rt_remix_taa    = 0;
            cvar::rt_ef_vintage   = 0;
        }
        else
        {
            cvar::rt_upscale_dlss = 0;
            cvar::rt_upscale_fsr2 = 0;
            cvar::rt_remix_taa    = g_isremix ? 2 : 0;
            cvar::rt_ef_vintage   = g_isremix ? 0 : RT_VINTAGE_480_DITHER;
        }
    }
    else
    {
        if( g_isremix )
        {
            if( cvar::rt_upscale_dlss == 0 && //
                cvar::rt_upscale_fsr2 > 0 &&  //
                cvar::rt_remix_taa == 0 &&    //
                cvar::rt_ef_vintage == 0 )
            {
                cvar::rt_remix_taa = cvar::rt_upscale_fsr2;
            }
            cvar::rt_upscale_fsr2 = 0;
            cvar::rt_ef_vintage   = 0;
        }
        else
        {
            if( cvar::rt_upscale_dlss == 0 && //
                cvar::rt_upscale_fsr2 == 0 &&  //
                cvar::rt_remix_taa > 0 &&    //
                cvar::rt_ef_vintage == 0 )
            {
                cvar::rt_upscale_fsr2 = cvar::rt_remix_taa;
            }
            cvar::rt_remix_taa = 0;
        }
    }
}

DFrameBuffer* Win32RTVideo::CreateFrameBuffer()
{
    return new RTFrameBuffer{ m_hMonitor, vid_fullscreen };
}

TArray< uint8_t > rtx::RTFrameBuffer::GetScreenshotBuffer( int& pitch, ESSType& color_type, float& gamma )
{
    // RTGL1 exposes no GPU readback for the presented frame. Capture the HWND
    // after present so `screenshot` / Level.MakeScreenShot work (and don't need focus).
    const int w = GetClientWidth() > 0 ? GetClientWidth() : GetWidth();
    const int h = GetClientHeight() > 0 ? GetClientHeight() : GetHeight();
    if( w <= 0 || h <= 0 )
    {
        return {};
    }

    HWND hwnd = mainwindow.GetHandle();
    if( !hwnd )
    {
        return {};
    }

    HDC hdcWin = GetDC( hwnd );
    if( !hdcWin )
    {
        return {};
    }

    HDC     hdcMem = CreateCompatibleDC( hdcWin );
    HBITMAP hbm    = CreateCompatibleBitmap( hdcWin, w, h );
    HGDIOBJ old    = SelectObject( hdcMem, hbm );

    // PW_RENDERFULLCONTENT: ask DWM for the redirected surface (Vulkan/DXGI).
    BOOL ok = PrintWindow( hwnd, hdcMem, 0x00000002 );
    if( !ok )
    {
        ok = BitBlt( hdcMem, 0, 0, w, h, hdcWin, 0, 0, SRCCOPY );
    }

    TArray< uint8_t > out;
    if( ok )
    {
        BITMAPINFOHEADER bi{};
        bi.biSize        = sizeof( bi );
        bi.biWidth       = w;
        bi.biHeight      = h; // bottom-up DIB
        bi.biPlanes      = 1;
        bi.biBitCount    = 32;
        bi.biCompression = BI_RGB;

        TArray< uint8_t > bgra( size_t( w ) * size_t( h ) * 4u, true );
        if( GetDIBits( hdcMem,
                       hbm,
                       0,
                       UINT( h ),
                       bgra.Data(),
                       reinterpret_cast< BITMAPINFO* >( &bi ),
                       DIB_RGB_COLORS ) )
        {
            // Reject obviously empty / failed captures (all black).
            uint64_t sum = 0;
            for( int i = 0; i < w * h; ++i )
            {
                sum += bgra[ size_t( i ) * 4u + 0u ];
                sum += bgra[ size_t( i ) * 4u + 1u ];
                sum += bgra[ size_t( i ) * 4u + 2u ];
            }
            if( sum > 0 )
            {
                out.Resize( size_t( w ) * size_t( h ) * 3u );
                for( int y = 0; y < h; ++y )
                {
                    const uint8_t* src = bgra.Data() + size_t( y ) * size_t( w ) * 4u;
                    uint8_t*       dst = out.Data() + size_t( h - 1 - y ) * size_t( w ) * 3u;
                    for( int x = 0; x < w; ++x )
                    {
                        dst[ x * 3 + 0 ] = src[ x * 4 + 2 ];
                        dst[ x * 3 + 1 ] = src[ x * 4 + 1 ];
                        dst[ x * 3 + 2 ] = src[ x * 4 + 0 ];
                    }
                }
                pitch      = w * 3;
                color_type = SS_RGB;
                gamma      = 1.0f;
            }
        }
    }

    SelectObject( hdcMem, old );
    DeleteObject( hbm );
    DeleteDC( hdcMem );
    ReleaseDC( hwnd, hdcWin );
    return out;
}

void Win32RTVideo::Shutdown()
{
    if( !rt.rgDestroyInstance )
    {
        return;
    }

    RgResult r = rt.rgDestroyInstance();
    if( r != RG_RESULT_SUCCESS )
    {
        MessageBoxA(
            nullptr, "rgDestroyAndUnloadLibrary has failed", "Fail", MB_ICONEXCLAMATION | MB_OK );
        exit( -1 );
    }

    rt = {};
}

void RT_ShowWarningMessageBox( const char* msg )
{
#ifdef _WIN32
    MessageBoxA( g_msgbox_parent.load(), msg, "Warning - Ray Tracing", MB_ICONEXCLAMATION | MB_OK );
#else
    assert( 0 );
#endif
}

bool RT_AskToOpenUrl( const char* heading, const char* msg, const wchar_t* url )
{
    int l = MessageBoxA( g_msgbox_parent.load(), msg, heading, MB_ICONEXCLAMATION | MB_YESNO );
    if( l == IDYES )
    {
        ShellExecute( nullptr, 0, url, 0, 0, SW_SHOW );
        return true;
    }
    return false;
}

//
//
//

auto RT_GetCurrentTime() -> double
{
    auto ns = []() {
        using namespace std::chrono;
        return duration_cast< nanoseconds >( steady_clock::now().time_since_epoch() ).count();
    };

    static int64_t startupTimeNS = ns();
    return static_cast< double >( ns() - startupTimeNS ) / 1000000000.0;
}

auto RT_GetVramUsage( bool* ok ) -> const char*
{
    const RgUtilMemoryUsage vram = rt.rgUtilRequestMemoryUsage();

    if( ok )
    {
        // < 80% is ok
        *ok = ( vram.vramUsed <= 0.8 * vram.vramTotal );
    }

    static char buf[ 64 ];
    snprintf( buf,
              std::size( buf ),
              "%d / %d MB",
              int( std::round( double( vram.vramUsed ) / 1024 / 1024 ) ),
              int( std::round( double( vram.vramTotal ) / 1024 / 1024 ) ) );

    buf[ std::size( buf ) - 1 ] = '\0';
    return buf;
}

// Outside the anonymous namespace below: rt_titles.cpp sizes its fullscreen
// quads with it.
RgExtent2D RT_GetCurrentWindowSize()
{
    return {
        static_cast< uint32_t >( screen->GetWidth() ),
        static_cast< uint32_t >( screen->GetHeight() ),
    };
}

namespace
{

void RT_ResolutionToRtgl( RgStartFrameRenderResolutionParams* dst, const RgExtent2D winsize )
{
    const auto aspect =
        static_cast< double >( winsize.width ) / static_cast< double >( winsize.height );

    if( cvar::rt_renderscale > 0.2f )
    {
        auto scale = std::clamp( double( *cvar::rt_renderscale ), 0.2, 1.0 );

        dst->customRenderSize.width    = static_cast< uint32_t >( winsize.width * scale );
        dst->customRenderSize.height   = static_cast< uint32_t >( winsize.height * scale );
        dst->pixelizedRenderSizeEnable = false;

        return;
    }
    else
    {
        if( int{ cvar::rt_ef_vintage } != RT_VINTAGE_OFF )
        {
            uint32_t h_pixelized = 0;
            uint32_t h_render    = 0;

            switch( int{ cvar::rt_ef_vintage } )
            {
                case RT_VINTAGE_200:
                case RT_VINTAGE_200_DITHER:
                    h_pixelized = 200;
                    h_render    = 400;
                    break;

                case RT_VINTAGE_480:
                case RT_VINTAGE_480_DITHER:
                    h_pixelized = 480;
                    h_render    = 600;
                    break;

                case RT_VINTAGE_CRT:
                case RT_VINTAGE_VHS:
                case RT_VINTAGE_VHS_CRT:
                    h_pixelized = 480;
                    h_render    = 480;
                    break;

                default:
                    cvar::rt_ef_vintage            = 0;
                    dst->customRenderSize          = winsize;
                    dst->pixelizedRenderSizeEnable = false;
                    return;
            }

            assert( h_render > 0 && h_pixelized > 0 );

            dst->pixelizedRenderSize.height = h_pixelized;
            dst->pixelizedRenderSize.width  = static_cast< uint32_t >( h_pixelized * aspect );
            dst->pixelizedRenderSizeEnable  = true;
            dst->customRenderSize.height    = h_render;
            dst->customRenderSize.width     = static_cast< uint32_t >( h_render * aspect );

            return;
        }
    }

    dst->customRenderSize          = winsize;
    dst->pixelizedRenderSizeEnable = false;
}

auto RT_GetSharpenTechniqueFromCvar( bool dlssOrFsr2 ) -> RgRenderSharpenTechnique
{
    switch( cvar::rt_sharpen )
    {
        case 3: return RG_RENDER_SHARPEN_TECHNIQUE_NONE;
        case 2: return RG_RENDER_SHARPEN_TECHNIQUE_AMD_CAS;
        case 1: return RG_RENDER_SHARPEN_TECHNIQUE_NAIVE;
        default: {
            if( dlssOrFsr2 )
            {
                return RG_RENDER_SHARPEN_TECHNIQUE_AMD_CAS;
            }
            // to accentuate a chunky look, because of the linear (not nearest) downscale mode
            switch( cvar::rt_ef_vintage )
            {
                case RT_VINTAGE_CRT:
                case RT_VINTAGE_VHS:
                case RT_VINTAGE_VHS_CRT: return RG_RENDER_SHARPEN_TECHNIQUE_NAIVE;
                case RT_VINTAGE_200:
                case RT_VINTAGE_200_DITHER:
                case RT_VINTAGE_480:
                case RT_VINTAGE_480_DITHER: return RG_RENDER_SHARPEN_TECHNIQUE_AMD_CAS;
                default: return RG_RENDER_SHARPEN_TECHNIQUE_NONE;
            }
        }
    }
}

// Snapshot of the last RT_UpscaleCvarsToRtgl() decision, for rt_rr_status.
static bool g_rr_dbg_isremix     = false;
static bool g_rr_dbg_wantNative  = false;
static int  g_rr_dbg_nvDlss      = 0;
static bool g_rr_dbg_rrRequested = false;

void RT_UpscaleCvarsToRtgl( RgStartFrameRenderResolutionParams* pDst )
{
    cvar::rt_available_dlss2 =
        rt.rgUtilIsUpscaleTechniqueAvailable( RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS,
                                              RG_FRAME_GENERATION_MODE_OFF,
                                              &cvar::rt_failreason_dlss2 );
    cvar::rt_available_dlss3fg =
        rt.rgUtilIsUpscaleTechniqueAvailable( RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS,
                                              RG_FRAME_GENERATION_MODE_ON,
                                              &cvar::rt_failreason_dlss3fg );
    cvar::rt_available_fsr2 =
        rt.rgUtilIsUpscaleTechniqueAvailable( RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2,
                                              RG_FRAME_GENERATION_MODE_OFF,
                                              &cvar::rt_failreason_fsr2 );
    cvar::rt_available_fsr3fg =
        rt.rgUtilIsUpscaleTechniqueAvailable( RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2,
                                              RG_FRAME_GENERATION_MODE_ON,
                                              &cvar::rt_failreason_fsr3fg );
    cvar::rt_available_dxgi = rt.rgUtilDXGIAvailable( &cvar::rt_failreason_dxgi );

    const RgFeatureFlags features = rt.rgUtilGetSupportedFeatures();

    cvar::rt_hdr_available   = ( features & RG_FEATURE_HDR );
    cvar::rt_fluid_available = ( features & RG_FEATURE_FLUID );

    int nvDlss = cvar::rt_available_dlss2 || cvar::rt_available_dlss3fg //
                     ? int( cvar::rt_upscale_dlss )
                     : 0;
    int amdFsr = cvar::rt_available_fsr2 || cvar::rt_available_fsr3fg //
                     ? int( cvar::rt_upscale_fsr2 )
                     : 0;

    // Native Ray Reconstruction needs a DLSS quality mode; default to Balanced.
    const bool wantNativeRr = !g_isremix && bool( cvar::rt_rayreconstr );
    if( wantNativeRr && nvDlss == 0 && ( cvar::rt_available_dlss2 || cvar::rt_available_dlss3fg ) )
    {
        nvDlss                = 2;
        cvar::rt_upscale_dlss = 2;
    }

    // DLSS and FSR2 both write pDst->upscaleTechnique and the FSR switch below
    // runs *second*, so a non-zero rt_upscale_fsr2 silently overwrites the DLSS
    // choice. rayReconstruction is still set afterwards (it only tests
    // nvDlss != 0), so gzdoom would hand RTGL "upscaler=FSR2 + RR=on" -- a
    // contradiction RTGL resolves by quietly dropping RR and running A-SVGF.
    //
    // rt_upscale_fsr2 is CVAR_ARCHIVE like every RT_CVAR, so a stale 2 in the
    // ini disabled Ray Reconstruction across every launch while rt_rayreconstr
    // still read 1 (2026-08-07). DLSS wins when both are set; RR depends on it.
    if( nvDlss != 0 && amdFsr != 0 )
    {
        static bool s_warned = false;
        if( !s_warned )
        {
            s_warned = true;
            Printf( "RT: both rt_upscale_dlss (%d) and rt_upscale_fsr2 (%d) are set; "
                    "they share one upscaler slot. Using DLSS and ignoring FSR2 "
                    "(Ray Reconstruction requires DLSS). Set rt_upscale_fsr2 0 to silence.\n",
                    nvDlss,
                    amdFsr );
        }
        amdFsr = 0;
    }

    switch( nvDlss )
    {
        case 1:
            // start with Quality
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_QUALITY;
            break;
        case 2:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_BALANCED;
            break;
        case 3:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_PERFORMANCE;
            break;
        case 4:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE;
            break;

        case 5:
            // use DLSS with rt_renderscale
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_CUSTOM;
            break;

        case 6:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_NATIVE_AA;
            break;

        default: nvDlss = 0; break;
    }

    switch( amdFsr )
    {
        case 1:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_QUALITY;
            break;
        case 2:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_BALANCED;
            break;
        case 3:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_PERFORMANCE;
            break;
        case 4:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE;
            break;

        case 5:
            // use FSR2 with rt_renderscale
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_CUSTOM;
            break;

        case 6:
            pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_AMD_FSR2;
            pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_NATIVE_AA;
            break;

        default: amdFsr = 0; break;
    }

    // both disabled
    if( nvDlss == 0 && amdFsr == 0 )
    {
        pDst->upscaleTechnique = RG_RENDER_UPSCALE_TECHNIQUE_NEAREST;
        pDst->resolutionMode   = RG_RENDER_RESOLUTION_MODE_CUSTOM;
        pDst->frameGeneration  = RG_FRAME_GENERATION_MODE_OFF;
    }
    else
    {
        if( ( nvDlss != 0 && cvar::rt_available_dlss3fg ) ||
            ( amdFsr != 0 && cvar::rt_available_fsr3fg ) )
        {
            switch( cvar::rt_framegen )
            {
                case -1: pDst->frameGeneration = RG_FRAME_GENERATION_MODE_WITHOUT_GENERATED; break;
                case 1: pDst->frameGeneration = RG_FRAME_GENERATION_MODE_ON; break;
                default: pDst->frameGeneration = RG_FRAME_GENERATION_MODE_OFF; break;
            }
        }
        else
        {
            pDst->frameGeneration = RG_FRAME_GENERATION_MODE_OFF;
        }
    }

    // Native RR replaces A-SVGF + DLSS-SR; Frame Gen is out of scope for MVP.
    // Gate on the technique that actually survived both switches above, not on
    // nvDlss alone: RTGL drops rayReconstruction whenever the upscaler isn't
    // DLSS (RenderResolutionHelper::Setup), so requesting RR alongside any
    // other upscaler is a contradiction that silently costs the denoiser.
    pDst->rayReconstruction = 0;
    if( wantNativeRr && nvDlss != 0 &&
        pDst->upscaleTechnique == RG_RENDER_UPSCALE_TECHNIQUE_NVIDIA_DLSS )
    {
        pDst->rayReconstruction = 1;
        pDst->frameGeneration   = RG_FRAME_GENERATION_MODE_OFF;
        if( int( cvar::rt_framegen ) != 0 )
        {
            cvar::rt_framegen = 0;
        }
    }

    // Cached for the rt_rr_status CCMD (RTGL's own DLSSRR messages are muted
    // unless -rtdebug, so this is the only in-game view of the decision chain).
    g_rr_dbg_isremix     = g_isremix;
    g_rr_dbg_wantNative  = wantNativeRr;
    g_rr_dbg_nvDlss      = nvDlss;
    g_rr_dbg_rrRequested = ( pDst->rayReconstruction != 0 );

    // Report the decision the FIRST time it is actually computed, and on every
    // later change. Running `rt_rr_status` from the command line reads the
    // cached globals above before this function has ever run, so it reports
    // startup defaults (DLSS2 available = NO) that look like a real negative --
    // another way this decision chain lied. The failure reason from
    // rgUtilIsUpscaleTechniqueAvailable is printed here because nothing else
    // ever surfaced it at frame time.
    {
        static bool s_have = false;
        static int  s_prev = -1;

        const int state = ( int( bool( cvar::rt_available_dlss2 ) ) << 0 ) |
                          ( int( bool( cvar::rt_available_dlss3fg ) ) << 1 ) |
                          ( int( wantNativeRr ) << 2 ) |
                          ( int( pDst->rayReconstruction != 0 ) << 3 ) | ( nvDlss << 4 );

        if( !s_have || s_prev != state )
        {
            s_have = true;
            s_prev = state;

            Printf( RT_DiagPrintLevel(),
                    "RT upscale/RR decision: DLSS2=%s DLSS3FG=%s nvDlss=%d "
                    "wantNativeRr=%s -> rayReconstruction=%s\n",
                    cvar::rt_available_dlss2 ? "yes" : "NO",
                    cvar::rt_available_dlss3fg ? "yes" : "NO",
                    nvDlss,
                    wantNativeRr ? "yes" : "no",
                    pDst->rayReconstruction ? "ON" : "OFF" );

            if( !cvar::rt_available_dlss2 && cvar::rt_failreason_dlss2 )
            {
                Printf( RT_DiagPrintLevel(),
                        "  DLSS2 unavailable, reason: %s\n",
                        static_cast< const char* >( cvar::rt_failreason_dlss2 ) );
            }
        }
    }

    pDst->sharpenTechnique = RT_GetSharpenTechniqueFromCvar( amdFsr || nvDlss );

    // Doom64-RT: which DLSS render preset RTGL1 creates the feature with. It was
    // hard-coded to E, and the DLSS runtime shipped in rt\bin is 310.7, on which
    // E is a deprecated CNN preset. Passed through raw; RTGL1 clamps nothing,
    // and NGX falls back to Default for any value it no longer recognises.
    pDst->dlssPreset =
        static_cast< uint32_t >( std::max< int >( 0, int{ cvar::rt_dlss_preset } ) );

    // Doom64-RT: the Ray Reconstruction twin. Raw
    // NVSDK_NGX_RayReconstruction_Hint_Render_Preset value; for RR only
    // 0 (Default) / 4 (D) / 5 (E) mean anything -- NGX reverts everything else
    // to Default. RTGL1 re-creates the RR feature when this changes.
    pDst->dlssRrPreset =
        static_cast< uint32_t >( std::max< int >( 0, int{ cvar::rt_rr_preset } ) );
}

template< typename T >
    requires( std::is_same_v< T, int > )
uint32_t safe_uint( T x )
{
    return static_cast< uint32_t >( std::max< int >( x, 0 ) );
}

} // anonymous namespace

//
//
//

rtx::RTFrameBuffer::RTFrameBuffer( void* hMonitor, bool fullscreen )
    : SystemBaseFrameBuffer( hMonitor, fullscreen ), m_state{ new RTRenderState{ this } }
{
}
rtx::RTFrameBuffer::~RTFrameBuffer()
{
    delete m_state;
    delete mVertexData;
    delete mSkyData;
    delete mViewpoints;
    delete mLights;
    delete mBones;
}
void rtx::RTFrameBuffer::InitializeState()
{
    m_state      = new RTRenderState{ this };
    vendorstring = "RT";
    mVertexData  = new FFlatVertexBuffer( GetWidth(), GetHeight(), screen->mPipelineNbr );
    mSkyData     = new FSkyVertexBuffer;
    mViewpoints  = new HWViewpointBuffer( screen->mPipelineNbr );
    mLights      = new FLightBuffer( screen->mPipelineNbr );
    mBones       = new BoneBuffer( screen->mPipelineNbr );
}

void rtx::RTFrameBuffer::FirstEye()
{
    m_state->RT_AddMainCamera( r_viewpoint );
    Super::FirstEye();
}

FRenderState* rtx::RTFrameBuffer::RenderState()
{
    return m_state;
}
IVertexBuffer* rtx::RTFrameBuffer::CreateVertexBuffer()
{
    return new RTVertexBuffer{};
}
IIndexBuffer* rtx::RTFrameBuffer::CreateIndexBuffer()
{
    return new RTIndexBuffer{};
}
IDataBuffer* rtx::RTFrameBuffer::CreateDataBuffer( int bindingpoint, bool ssbo, bool needsresize )
{
    return new RTDataBuffer{};
}
IHardwareTexture* rtx::RTFrameBuffer::CreateHardwareTexture( int numchannels )
{
    return new RTHardwareTexture{};
}
void rtx::RTFrameBuffer::Draw2D()
{
    ::Draw2D( twod, *m_state );
}

//
//
//

namespace
{
constexpr auto remap01( float v, float newmin, float newmax )
{
    assert( newmax > newmin );
    return newmin + std::clamp( v, 0.f, 1.f ) * ( newmax - newmin );
}

auto RT_GetPlayer() -> player_t*
{
    return players[ consoleplayer ].camera ? players[ consoleplayer ].camera->player : nullptr;
}

auto RT_DamageIntensity() -> std::optional< float >
{
    // for reference https://doom.fandom.com/wiki/Comparison_of_Doom_monsters
    constexpr float maxdmg = 100.f;

    if( auto player = RT_GetPlayer() )
    {
        if( player->damagecount > 0 )
        {
            float dmg01 =
                std::clamp( static_cast< float >( player->damagecount ) / maxdmg, 0.f, 1.f );

            // smaller damage should also have effect
            dmg01 = sqrt( dmg01 );

            assert( dmg01 > 0.005f );
            return dmg01;
        }
    }
    return {};
}
} // anonymous namespace


//
//
//

static bool   g_resetposteffects = false;
static bool   g_resetfluid       = false;
static bool   g_melt_requested   = false;
static double g_melt_endtime     = -1;
bool          g_noinput_onstart  = true;

bool   g_cpu_latency_get = false;
double g_cpu_latency     = 0;

// RT_DrawTitle / RT_ClearTitles / RT_InjectTitleIntoDoomMap now live in
// rt_titles.cpp and are declared in rt_internal.h.


// Walk to the lava, without walking to the lava.
//
// Every "the lava lights nothing" report so far was judged from wherever the
// player happened to be standing, and the debug line eventually showed the
// camera 1530 map units -- 48 metres -- from the nearest lava light. At that
// range 60 lumen delivers an irradiance of about 0.03, so "no difference" was
// the correct observation and said nothing at all about the feature.
//
// This puts the player on the lava so the comparison is actually the one being
// argued about. Prints the sectors it found either way, so "there is no lava
// here" is distinguishable from "the lava is not lit".
CCMD( rt_lava_goto )
{
    if( !primaryLevel )
    {
        Printf( "rt_lava_goto: no level\n" );
        return;
    }

    AActor* pmo = players[ consoleplayer ].mo;
    if( !pmo )
    {
        Printf( "rt_lava_goto: no player\n" );
        return;
    }

    int found = 0;
    for( unsigned i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        const sector_t& sec = primaryLevel->sectors[ i ];
        auto* gtex = TexMan.GetGameTexture( sec.GetTexture( sector_t::floor ), true );
        if( !gtex || !RT_IsLavaFlat( gtex->GetName().GetChars() ) )
        {
            continue;
        }
        found++;

        const DVector2 c{ double( sec.centerspot.X ), double( sec.centerspot.Y ) };
        // centerspot is the bounding-box centre and a concave sector's can lie
        // outside it, so only move if it really is in this sector.
        const bool inside = ( primaryLevel->PointInSector( c.X, c.Y ) == &sec );
        const double z = sec.floorplane.ZatPoint( c );

        Printf( "rt_lava_goto: sector %u floor \"%s\" at (%.0f %.0f %.0f)%s\n",
                i,
                gtex->GetName().GetChars(),
                c.X,
                c.Y,
                z,
                inside ? "" : "  [centre is outside the sector, not moving there]" );

        if( found == 1 && inside )
        {
            pmo->SetOrigin( DVector3{ c.X, c.Y, z + 8.0 }, false );
            Printf( "rt_lava_goto: moved you there. rt_lava_light_debug 1 prints "
                    "the distance to the nearest lava light.\n" );
        }
    }

    if( found == 0 )
    {
        Printf( "rt_lava_goto: no lava floor in this map. MAP15/20/21/34 have one.\n" );
    }
}

CCMD( rt_sky_here )
{
    if( !bool{ cvar::rt_sky_log } )
    {
        Printf( "rt_sky_here: set rt_sky_log 1 first, then walk into the room.\n" );
        return;
    }
    if( !primaryLevel )
    {
        Printf( "rt_sky_here: no level\n" );
        return;
    }
    Printf( "rt_sky_here: %zu sky primitives submitted last frame\n",
            g_skyprims_prev.size() );
    int shown = 0;
    for( const auto& n : g_skyprims_prev )
    {
        const float w = n.max[ 0 ] - n.min[ 0 ];
        const float h = n.max[ 1 ] - n.min[ 1 ];
        const float d = n.max[ 2 ] - n.min[ 2 ];
        // A sky WALL is thin in one horizontal axis and tall; a sky FLAT is thin
        // vertically. Naming which is which is most of the answer.
        const char* kind = ( h < 1.0f ) ? "FLAT (ceiling opening)" : "WALL (band/slot)";
        Printf( "  %-22s  size %7.1f x %7.1f x %7.1f  at (%.0f, %.0f, %.0f)\n",
                kind, w, h, d,
                ( n.min[ 0 ] + n.max[ 0 ] ) * 0.5f,
                ( n.min[ 1 ] + n.max[ 1 ] ) * 0.5f,
                ( n.min[ 2 ] + n.max[ 2 ] ) * 0.5f );
        if( ++shown >= 40 )
        {
            Printf( "  ... (%zu more)\n", g_skyprims_prev.size() - shown );
            break;
        }
    }
}

// The per-map presets (moon, clouds, tint, fog) moved to rt_presets.cpp and the
// storm to rt_weather.cpp. Both are declared in rt_internal.h.

void RT_OnLevelLoad( const char* mapname )
{
    RT_OnLevelLoadPresets( mapname );

    g_resetposteffects = true;
    g_resetfluid       = true;
    g_rt_lightcut      = true; // DLSS-RR: new scene, flush temporal history unconditionally
    g_rt_lightcut_why  = "levelload";
    RT_ClearTitles();
    RT_InjectTitleIntoDoomMap( mapname );
    RT_ForceIntroCutsceneMusicStop();
}

void RT_RequestMelt()
{
    // HACKHACK: suppress melting when getting into the first start
    {
        static bool first = true;
        if( first )
        {
            first = false; 
            return;
        }
    }
    g_melt_requested = true;
}

bool RT_IsMeltActive()
{
    return g_melt_endtime > 0 && RT_GetCurrentTime() < g_melt_endtime;
}
bool RT_IgnoreUserInput()
{
    return RT_IsMeltActive() || g_noinput_onstart;
}

static double CalcCpuLatency()
{
    static double   g_lprevtime            = RT_GetCurrentTime();
    static double   g_lprevlatencies[ 30 ] = {};
    static uint32_t g_lprevi               = 0;

    double lcurtime = RT_GetCurrentTime();

    g_lprevlatencies[ g_lprevi ] = lcurtime - g_lprevtime;

    g_lprevi    = ( g_lprevi + 1 ) % std::size( g_lprevlatencies );
    g_lprevtime = lcurtime;

    double sum = 0;
    int    cnt = 0;
    for( double t : g_lprevlatencies )
    {
        if( t > 0 )
        {
            sum += t;
            cnt++;
        }
    }

    return cnt > 0 ? sum / cnt : 0;
}

namespace
{
template< typename T >
T smoothstep( T edge0, T edge1, T x )
{
    T t = std::clamp( ( x - edge0 ) / ( edge1 - edge0 ), T( 0 ), T( 1 ) );
    return t * t * ( T( 3 ) - T( 2 ) * t );
}

namespace classic_toggle
{
    constexpr double Duration  = 0.75;
    double           g_timeend = 0.0;

    float                  g_source = 0.0f;
    std::optional< float > g_target = {};

    // Why is DLSS Ray Reconstruction on/off? RTGL's own DLSSRR messages are
    // suppressed unless gzdoom is launched with -rtdebug, so this prints the
    // whole gzdoom-side decision chain that feeds
    // RgStartFrameRenderResolutionParams::rayReconstruction.
    CCMD( rt_rr_status )
    {
        Printf( "--- DLSS Ray Reconstruction status ---\n" );
        Printf( "  rt_rayreconstr        = %d  (user request)\n", int( bool( cvar::rt_rayreconstr ) ) );
        Printf( "  rt_upscale_dlss       = %d  (0 = off; RR needs != 0)\n", int( cvar::rt_upscale_dlss ) );
        Printf( "  remix mode            = %s  (RR is native-only, disabled under Remix)\n",
                g_rr_dbg_isremix ? "YES" : "no" );
        Printf( "  DLSS2 available       = %s%s%s\n",
                cvar::rt_available_dlss2 ? "YES" : "NO",
                ( !cvar::rt_available_dlss2 && cvar::rt_failreason_dlss2 ) ? "  reason: " : "",
                ( !cvar::rt_available_dlss2 && cvar::rt_failreason_dlss2 ) ? cvar::rt_failreason_dlss2
                                                                          : "" );
        Printf( "  DLSS3-FG available    = %s%s%s\n",
                cvar::rt_available_dlss3fg ? "YES" : "NO",
                ( !cvar::rt_available_dlss3fg && cvar::rt_failreason_dlss3fg ) ? "  reason: " : "",
                ( !cvar::rt_available_dlss3fg && cvar::rt_failreason_dlss3fg )
                    ? cvar::rt_failreason_dlss3fg
                    : "" );
        Printf( "  -> wantNativeRr       = %s\n", g_rr_dbg_wantNative ? "YES" : "no" );
        Printf( "  -> nvDlss (mode)      = %d\n", g_rr_dbg_nvDlss );
        Printf( "  -> RR REQUESTED       = %s\n", g_rr_dbg_rrRequested ? "YES" : "NO" );
        Printf( "\n" );
        // Everything above is what gzdoom ASKS FOR. RTGL's Dev UI can silently
        // replace it afterwards, so none of it proves what actually ran.
        Printf( "  NOTE: this is gzdoom's REQUEST, not the applied state. RTGL's Dev UI\n"
                "  can override it -- a sticky \"DLSS Ray Reconstruction\" checkbox wins\n"
                "  even with the Override master switch OFF, and used to persist across\n"
                "  launches in rt/devmode_settings.json. That made this command report\n"
                "  \"RR REQUESTED = YES\" through several sessions that actually ran A-SVGF\n"
                "  (2026-08-07). RTGL now resets sticky flags on load and warns (-rtdebug)\n"
                "  whenever it overrides this request. To be certain: launch with -rtdebug\n"
                "  and check for a \"Dev override: DLSS Ray Reconstruction forced ...\" line,\n"
                "  or delete rt/devmode_settings.json.\n" );
        Printf( "\n" );
        if( !g_rr_dbg_rrRequested )
        {
            Printf( "  RR is NOT requested -> A-SVGF denoiser runs (image should be smooth).\n" );
        }
        else
        {
            Printf( "  RR IS requested. If the image is still raw/noisy, RTGL accepted the\n"
                    "  request but DLSSRR::Apply() bailed (VulkanDevice.cpp skips A-SVGF\n"
                    "  whenever the nvDlssRr object merely exists) -> no denoiser at all.\n"
                    "  Relaunch with -rtdebug to see the DLSSRR: lines from RTGL.\n" );
        }
    }

    // `moon`, `clouds`, `fog`, `smoke` and `thunder` used to be declared here,
    // inside namespace classic_toggle, which has nothing to do with any of them.
    // Each now sits in the file that owns the state it reports: rt_presets.cpp,
    // rt_smoke.cpp, rt_weather.cpp.


    // `whatsthat` -- name the surface under the crosshair.
    //
    // Every wrong guess in this work has been the same mistake: identifying a reported
    // surface from a screenshot by rendering candidate textures and picking the one
    // that looks right. That got C921 right and C53 wrong, HDOR10 right and C52 wrong,
    // and each miss cost a round trip. A screenshot does not carry a sector index; the
    // running game does.
    //
    // Point at the thing, type `whatsthat`, and it reports the sector, its lightlevel,
    // its tag, the texture on the exact surface hit, and whether that sector is above
    // this map's rt_sector_emis threshold -- i.e. whether it is self-emitting, which is
    // the whole question.
    CCMD( whatsthat )
    {
        if( !primaryLevel || !players[ consoleplayer ].mo )
        {
            Printf( "whatsthat: no level\n" );
            return;
        }
        AActor* pmo = players[ consoleplayer ].mo;

        FLineTraceData d{};
        const bool hit = P_LineTrace( pmo,
                                      pmo->Angles.Yaw,
                                      8192.,
                                      pmo->Angles.Pitch,
                                      0,
                                      pmo->Height * 0.5,
                                      0.,
                                      0.,
                                      &d );
        if( !hit || !d.HitSector )
        {
            Printf( "whatsthat: nothing hit within 8192 units\n" );
            return;
        }

        const int   idx  = d.HitSector->Index();
        const int   ll   = d.HitSector->lightlevel;
        const char* tex  = "?";
        if( d.HitTexture.isValid() )
        {
            if( auto* gt = TexMan.GetGameTexture( d.HitTexture, true ) )
            {
                tex = gt->GetName().GetChars();
            }
        }
        static const char* partname[] = { "top", "middle", "bottom" };
        const char* what =
            d.HitType == TRACE_HitFloor    ? "floor"
            : d.HitType == TRACE_HitCeiling ? "ceiling"
            : d.HitType == TRACE_HitWall
                ? ( d.LinePart >= 0 && d.LinePart <= 2 ? partname[ d.LinePart ] : "wall" )
                : "actor";

        Printf( "whatsthat: sector %d  lightlevel %d  tag %d  %s texture '%s'\n",
                idx,
                ll,
                primaryLevel->GetFirstSectorTag( d.HitSector ),
                what,
                tex );
        Printf( "           threshold %.0f -> %s\n",
                g_sectorEmisThreshold,
                float( ll ) > g_sectorEmisThreshold ? "ABOVE: this surface SELF-EMITS"
                                                    : "below: not self-emitting" );
        {
            // The frame test, printed: what does this element sit inside?
            int hi = -1, hiIdx = -1;
            for( auto ln : d.HitSector->Lines )
            {
                sector_t* o = ( ln->frontsector == d.HitSector ) ? ln->backsector
                                                                 : ln->frontsector;
                if( o && o != d.HitSector && o->lightlevel > hi )
                {
                    hi    = o->lightlevel;
                    hiIdx = o->Index();
                }
            }
            if( hiIdx >= 0 )
            {
                Printf( "           brightest neighbour: sector %d at %d  (delta %+d)\n",
                        hiIdx, hi, ll - hi );
            }
        }
    }

    // `rt_dump_lightthinkers` -- who is animating a sector's lightlevel.
    //
    // rt_lightlevel_watch says WHICH sectors move; this says WHAT is moving them, which
    // is the question the map data could not answer on MAP13: seven sectors on tag 29
    // sweep 221..255 forever while the map has no sector special, no linedef Light_*,
    // and no ACS call on that tag anywhere -- not in its own BEHAVIOR and not in the
    // twelve LOADACS libraries.
    //
    // A running thinker is the ground truth regardless of how it got created, so this
    // asks the playsim rather than the file. GZDoom builds one DLighting subclass per
    // animated sector (DGlow, DFlicker, DFireFlicker, DLightFlash, DStrobe, DPhased),
    // and the class name identifies the effect immediately.
    CCMD( rt_dump_lightthinkers )
    {
        if( !primaryLevel )
        {
            Printf( "rt_dump_lightthinkers: no level\n" );
            return;
        }
        auto it = TThinkerIterator< DLighting >( primaryLevel, STAT_LIGHT );
        int  n  = 0;
        while( DLighting* l = it.Next() )
        {
            sector_t* s = l->GetSector();
            const int idx = s ? s->Index() : -1;
            Printf( "  %-16s sector %-4d lightlevel=%d tag=%d\n",
                    l->GetClass()->TypeName.GetChars(),
                    idx,
                    s ? s->lightlevel : -1,
                    ( s && idx >= 0 ) ? primaryLevel->GetFirstSectorTag( s ) : -1 );
            n++;
        }
        Printf( "rt_dump_lightthinkers: %d light thinker(s) running\n", n );
    }


    CCMD( rt_dump_dynlights )
    {
        if( !primaryLevel || !primaryLevel->lights )
        {
            Printf( "rt_dump_dynlights: no level / no light list\n" );
            return;
        }
        unsigned n = 0;
        for( FDynamicLight* light = primaryLevel->lights; light != nullptr; light = light->next )
        {
            if( !light->IsActive() || light->X() < -1.0e6 )
            {
                continue;
            }
            Printf(
                "  [%u] pos=(%.0f,%.0f,%.0f) rgb=(%d,%d,%d) radius=%.1f active=%d\n",
                n,
                light->X(),
                light->Y(),
                light->Z(),
                light->GetRed(),
                light->GetGreen(),
                light->GetBlue(),
                light->m_currentRadius,
                light->IsActive() ? 1 : 0 );
            ++n;
        }
        Printf( "rt_dump_dynlights: %u listed (GZDoom FDynamicLight chain)\n", n );
    }

    CCMD( rt_classic_toggle )
    {
        if( g_isremix )
        {
            cvar::rt_classic = 0; 
            return;
        }

        g_timeend = RT_GetCurrentTime() + Duration;
        g_source  = std::clamp< float >( cvar::rt_classic, 0, 1 );

        if( g_target )
        {
            g_target = g_target.value() > 0 ? 0.f : 1.f;
        }
        else
        {
            g_target = cvar::rt_classic > 0 ? 0.f : 1.f;
        }
    }

    void Animate()
    {
        if( g_isremix )
        {
            cvar::rt_classic = 0;
            return;
        }

        if( g_target )
        {
            double dt = g_timeend - RT_GetCurrentTime();
            if( dt <= 0 )
            {
                cvar::rt_classic = *g_target;
                g_target         = {};
                return;
            }

            double ratio = 1 - std::clamp( dt / Duration, 0.0, 1.0 );
            ratio        = smoothstep( 0.0, 1.0, ratio );

            cvar::rt_classic = std::lerp( g_source, *g_target, static_cast< float >( ratio ) );
        }
    }
} // namespace classic_toggle

} // anonymous namespace

// Out of the anonymous namespace: rt_weapon.cpp's RT_AddFlashlight reads the
// flashlight bit off it.
uint32_t RT_CalcPowerupFlags()
{
    auto player = RT_GetPlayer();
    if( !player )
    {
        return 0;
    }

    uint32_t powerups = 0;

    for( AActor* in = player->mo->Inventory; in; in = in->Inventory )
    {
        if( in->IsKindOf( NAME_PowerStrength ) )
        {
            if( rtstate.m_berserkBlend > 10 )
            {
                powerups |= RT_POWERUP_FLAG_BERSERK_BIT;
            }
        }
        else if( in->IsKindOf( NAME_PowerIronFeet ) )
        {
            powerups |= RT_POWERUP_FLAG_RADIATIONSUIT_BIT;
        }
        else if( in->IsKindOf( NAME_PowerInvulnerable ) )
        {
            powerups |= RT_POWERUP_FLAG_INVUNERABILITY_BIT;
        }
        else if( in->IsKindOf( NAME_PowerLightAmp ) )
        {
            switch( *cvar::rt_pw_lightamp )
            {
                case 1: powerups |= RT_POWERUP_FLAG_THERMALVISION_BIT; break;
                case 2: powerups |= RT_POWERUP_FLAG_FLASHLIGHT_BIT; break;
                default: powerups |= RT_POWERUP_FLAG_NIGHTVISION_BIT; break;
            }
        }
        else if( in->IsKindOf( NAME_PowerInvisibility ) )
        {
            powerups |= RT_POWERUP_FLAG_INVISIBILITY_BIT;
        }

        // NAME_PowerTargeter
        // NAME_PowerWeaponLevel2
        // NAME_PowerFlight
        // NAME_PowerSpeed
        // NAME_PowerTorch
        // NAME_PowerHighJump
        // NAME_PowerReflection
        // NAME_PowerDrain
        // NAME_PowerScanner
        // NAME_PowerDoubleFiringSpeed
        // NAME_PowerInfiniteAmmo
        // NAME_PowerBuddha
    }

    if( player->bonuscount > 0 )
    {
        powerups |= RT_POWERUP_FLAG_BONUS_BIT;
    }

    return powerups;
}


// The light uploaders and the smoke simulation used to live here -- 4,200 lines
// of it, between classic_toggle and the frame loop. They are now:
//
//   rt_lights_sector.cpp    sector lights, gzdoom dynlights, lightlevel watch
//   rt_lights_fixtures.cpp  texture-inferred fixtures (inset, strip, edge, hand)
//   rt_lights_fx.cpp        switches, lava, flames
//   rt_smoke.cpp            the muzzle/rocket puff volumes
//
// All of it used to sit inside the anonymous namespace that closes just above --
// which is precisely why none of it could be moved before. Their entry points
// are declared in rt_internal.h and are all still driven from one call site, in
// RT_DrawFrame below.

//
//
//

// Special extension
#define ext_RG_STRUCTURE_TYPE_START_FRAME_REMIX_PARAMS ( ( RgStructureType )1024 )

struct ext_RgStartFrameRemixParams
{
    RgStructureType sType;
    void*           pNext;
    RgBool32        rayReconstruction;
    RgBool32        taa;
    RgBool32        nis;
    RgBool32        reflex;
};

void rtx::RTFrameBuffer::RT_BeginFrame()
{
    // HACKHACK begin
    if( g_rt_skipinitframes == -10 )
    {
        g_rt_skipinitframes = cvar::hack_initialframesskip ? 0 : 2;
    }
    if( g_rt_skipinitframes >= 0 )
    {
        if( g_rt_skipinitframes == 0 )
        {
            RT_CloseLauncherWindow(); // renderer is ready, close launcher window
            PositionWindow( IsFullscreen() );
            g_rt_forcenofocuschange = false;
        }
        --g_rt_skipinitframes;
    }
    // HACKHACK end


    m_state->RT_BeginFrame();

    classic_toggle::Animate();


    auto resolution_params = RgStartFrameRenderResolutionParams{
        .sType             = RG_STRUCTURE_TYPE_START_FRAME_RENDER_RESOLUTION_PARAMS,
        .pNext             = nullptr,
        .preferDxgiPresent = cvar::rt_available_dxgi ? cvar::rt_dxgi : false,
    };
    RT_ResolutionToRtgl( &resolution_params, RT_GetCurrentWindowSize() );
    RT_UpscaleCvarsToRtgl( &resolution_params );

    ext_RgStartFrameRemixParams remix_params;
    if( g_isremix )
    {
        remix_params = ext_RgStartFrameRemixParams{
            .sType             = ext_RG_STRUCTURE_TYPE_START_FRAME_REMIX_PARAMS,
            .pNext             = nullptr,
            .rayReconstruction = ( cvar::rt_remix_rayreconstr ? 1u : 0u ),
            .taa               = ( cvar::rt_remix_taa > 0 ? 1u : 0u ),
            .nis               = 0,
            .reflex            = ( cvar::rt_remix_reflex ? 1u : 0u ),
        };

        switch( int( cvar::rt_remix_taa ) )
        {
            case 4:
                resolution_params.resolutionMode = RG_RENDER_RESOLUTION_MODE_ULTRA_PERFORMANCE;
                break;
            case 3: resolution_params.resolutionMode = RG_RENDER_RESOLUTION_MODE_PERFORMANCE; break;
            case 2: resolution_params.resolutionMode = RG_RENDER_RESOLUTION_MODE_BALANCED; break;
            case 1: resolution_params.resolutionMode = RG_RENDER_RESOLUTION_MODE_QUALITY; break;
            case 6: resolution_params.resolutionMode = RG_RENDER_RESOLUTION_MODE_NATIVE_AA; break;
            case 5: resolution_params.resolutionMode = RG_RENDER_RESOLUTION_MODE_CUSTOM; break;
            default: remix_params.taa = 0; break;
        }

        remix_params.pNext      = resolution_params.pNext;
        resolution_params.pNext = &remix_params;
    }

    RT_MakeLightstyles();

    auto fluid_params = RgStartFrameFluidParams{
        .sType          = RG_STRUCTURE_TYPE_START_FRAME_FLUID_PARAMS,
        .pNext          = &resolution_params,
        .enabled        = cvar::rt_fluid_available ? cvar::rt_fluid : false,
        .reset          = g_resetfluid,
        .gravity        = { cvar::rt_fluid_gravity_x, //
                            cvar::rt_fluid_gravity_y,
                            cvar::rt_fluid_gravity_z },
        .color          = { cvar::rt_blood_color_r, //
                            cvar::rt_blood_color_g,
                            cvar::rt_blood_color_b },
        .particleBudget = uint32_t( std::max( 0, int( cvar::rt_fluid_budget ) ) ),
        .particleRadius = cvar::rt_fluid_pradius,
    };

    RgStaticSceneStatusFlags staticscene_status = 0;

    // Maps with no baked rt/scenes must not auto-export and must not be
    // uncull-alled: export + uncull-all freezes the main thread while the whole
    // map is uploaded at once, which is seen as a multi-second hang at the level
    // transition (press EXIT, wait, THEN the intermission appears).
    //
    // THE TEST IS "HAS A BAKED SCENE", NOT "IS A PWAD". It used to be an
    // underscore check on the map name, which protected Retribution's maps and
    // left every IWAD map exposed -- harmless while IWAD maps took their geometry
    // from a baked scene, and not harmless at all now that they upload live
    // (see RT_ModMapNeedsLiveGeometryUpload). A stock DOOM II map uploading its
    // whole world with culling disabled is the same stall, for the same reason.
    const char* mapname_for_rt = RT_GetMapName();
    const bool  is_mod_map     = RT_ModMapNeedsLiveGeometryUpload();
    const bool  allow_autoexport = cvar::rt_autoexport && !is_mod_map;

    auto info = RgStartFrameInfo{
        .sType                  = RG_STRUCTURE_TYPE_START_FRAME_INFO,
        .pNext                  = &fluid_params,
        .pMapName               = mapname_for_rt,
        .ignoreExternalGeometry = false,
        .vsync                  = cvar::rt_vsync,
        .hdr                    = cvar::rt_hdr_available ? cvar::rt_hdr : false,
        .allowMapAutoExport     = allow_autoexport,
        .lightmapScreenCoverage = RT_ForceNoClassicMode() ? 0.0f : cvar::rt_classic,
        .lightstyleValuesCount  = uint32_t( g_sectorlightlevels.size() ),
        .pLightstyleValues8     = g_sectorlightlevels.data(),
        .pResultStaticSceneStatus = &staticscene_status,
        .staticSceneAnimationTime = g_rt_cutscenename ? RT_CutsceneTime() : 0,
    };
    g_resetfluid = false;

    RgResult r = rt.rgStartFrame( &info );
    RG_CHECK( r );


    auto l_clm = [ staticscene_status, is_mod_map ]() {
        // Doom64-RT: uncull-all (mode 2) on large UDMF mods freezes the main thread
        // for a long time (looks hung; needs force-close). Never do it for mod maps.
        if( !is_mod_map )
        {
            if( staticscene_status & RG_STATIC_SCENE_STATUS_EXPORT_STARTED )
            {
                return 2; // no cull as we need to upload all geometry for the first time
            }
            if( staticscene_status & RG_STATIC_SCENE_STATUS_NEW_SCENE_STARTED )
            {
                return 2; // touch everything, to upload all resources
            }
        }
        switch( int( cvar::rt_cpu_cullmode ) )
        {
            case 1: return 1;
            case 2: return 2;
            default: return 0;
        }
    };

    rt_cullmode = l_clm();
}

void rtx::RTFrameBuffer::RT_DrawFrame()
{
    // Sky primitives are collected as the renderer walks the BSP, so the list
    // has to roll over once per frame or it just grows.
    RT_SkyPrimsEndFrame();

    const double   curtime      = RT_GetCurrentTime();
    const uint32_t powerupflags = RT_CalcPowerupFlags();

    RT_DrawTitle();

    // THE directional light -- singular, and that is a hard constraint, not a
    // simplification. RTGL1's LightManager::Add answers a second directional
    // light with debug::Error("Only one directional light is allowed"), and
    // debug::Error exits the game. Uploading lightning as its own light took
    // MAP11 down on the first strike.
    //
    // So the moon and the storm SHARE this slot, and the brighter one wins.
    // That is not a compromise: a strike peaks at rt_lightning_intensity 2200
    // against the moon's 90, and the handover happens exactly where the two are
    // equal, so there is no pop at either end -- the moon's shafts are replaced
    // only while something 20x brighter is standing in for them, and come back
    // as the flash decays past them. Both ends of the strike cross that point
    // continuously.
    {
        // The moon, dimmed by whatever cloud is currently in front of it. The
        // deck is sky geometry -- rasterised into the cubemap, never in the
        // acceleration structure -- so it cannot cast a shadow on its own and
        // the moon would otherwise pour through an overcast sky at full
        // strength. RT_DrawCloudDeck walks the moon's own ray up through the
        // shells and reports what gets through; see the comment there.
        // Per channel, so the deck's COLOUR reaches the light: moonlight under a
        // purple overcast arrives purple. The intensity carries the luminance of
        // that transmittance and the colour carries its hue, because
        // RgLightDirectionalEXT keeps the two separate -- splitting it any other
        // way would make a saturated tint quietly dim the light as well.
        const float tR = g_cloudSunTransmittance[ 0 ];
        const float tG = g_cloudSunTransmittance[ 1 ];
        const float tB = g_cloudSunTransmittance[ 2 ];
        const float tLum = std::max( 0.02f, 0.2126f * tR + 0.7152f * tG + 0.0722f * tB );

        const float sunI = ( bool{ cvar::rt_sun } && float{ cvar::rt_sun_intensity } > 0.f )
                             ? float{ cvar::rt_sun_intensity } * tLum
                             : 0.f;

        float ltngI   = 0.f;
        float ltngAzi = 0.f, ltngAlt = 0.f;
        if( const float flash = RT_LightningFlashLevel(); flash > 0.002f )
        {
            RT_LightningAim( &ltngAzi, &ltngAlt, nullptr );
            ltngI = std::max( 0.f, float{ cvar::rt_lightning_intensity } ) * flash;
        }

        const bool useLightning = ltngI > sunI;

        if( useLightning || sunI > 0.f )
        {
        const float intensity = useLightning ? ltngI : sunI;

        // The moon's own colour, multiplied by the cloud transmittance's HUE
        // (its luminance already went into sunI above, so it is divided back out
        // here -- otherwise the dimming would be applied twice). Lightning is
        // not filtered: the strike is inside or under the deck, not behind it.
        const auto color = [ & ] {
            if( useLightning )
            {
                return cvarcolor_to_rtcolor( cvar::rt_lightning_color );
            }
            const uint32_t sc = *( cvar::rt_sun_color );
            return rt.rgUtilPackColorFloat4D(
                std::clamp( ( RPART( sc ) / 255.f ) * ( tR / tLum ), 0.f, 1.f ),
                std::clamp( ( GPART( sc ) / 255.f ) * ( tG / tLum ), 0.f, 1.f ),
                std::clamp( ( BPART( sc ) / 255.f ) * ( tB / tLum ), 0.f, 1.f ),
                1.f );
        }();
        const float angdiam   = useLightning
                                  ? std::clamp( float{ cvar::rt_lightning_angdiam }, 0.01f, 90.f )
                                  : std::clamp( float{ cvar::rt_sun_angdiam }, 0.01f, 90.f );

        float altitude = to_rad( useLightning ? ltngAlt : float{ cvar::rt_sun_a } );
        float azimuth  = to_rad( useLightning ? ltngAzi : float{ cvar::rt_sun_b } );

        float theta = std::clamp( rt_pi() / 2 - altitude, 0.f, rt_pi() );
        float phi   = std::fmod( azimuth, rt_pi() * 2 );

        // negate, direction from the sun, not towards the sun
        auto dir = RgFloat3D{
            -sin( theta ) * cos( phi ),
            -sin( theta ) * sin( phi ),
            -cos( theta ),
        };

        if( useLightning && int{ cvar::rt_lightning_debug } )
        {
            Printf( "rt_lightning: directional -> lightning %.0f (moon %.0f), az %.0f alt %.0f\n",
                    ltngI, sunI, ltngAzi, ltngAlt );
        }

        auto s = RgLightDirectionalEXT{
            .sType                  = RG_STRUCTURE_TYPE_LIGHT_DIRECTIONAL_EXT,
            .pNext                  = nullptr,
            .color                  = color,
            .intensity              = intensity,
            .direction              = dir,
            // The size gate for sky leaks, and the reason it is an ANGLE.
            //
            // At 0.5 degrees -- the real moon -- this light is effectively a
            // point, so its shadow ray is a single yes/no test. One unblocked
            // ray through a hand-width crack delivers exactly as much light as
            // an open doorway, which is why a pinhole leak reads as a full-
            // strength shaft and why no per-surface rule could fix it: the wall
            // holes MAP13 wants lit and the cracks it does not are the same kind
            // of geometry.
            //
            // Widen the disc and the test stops being binary. RTGL1 samples a
            // point on it per shadow ray (sampleDirectionalLight -> sampleDisk),
            // so an opening now admits light in proportion to how much of the
            // disc it actually reveals. A doorway reveals all of it and is
            // unchanged; a narrow band reveals a sliver and dims smoothly.
            //
            // Crucially it also falls off with DISTANCE, which is the behaviour
            // actually wanted here: an opening of size d seen from L away
            // subtends d/L, so the same band still lights the surfaces beside it
            // and stops washing a ceiling 2000 units off. That is a soft
            // rolloff, not a cutoff -- "too small a hole" is only meaningful
            // relative to how far away you are standing, so a hard threshold
            // could not have been right at any single value.
            //
            // Costs sharpness on the wanted shafts too: this is one knob for
            // both, traded with rt_sun_angdiam. During a strike this is
            // rt_lightning_angdiam instead -- deliberately much wider; see there.
            .angularDiameterDegrees = angdiam,
        };

        // ONE id for both, so there is only ever one directional light alive.
        // Reusing SunLightId also means the swap is a parameter change on a
        // light RTGL1 already knows, rather than a light appearing and another
        // disappearing in the same frame.
        auto i = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &s,
            .uniqueID     = SunLightId,
            .isExportable = false,
        };

        RgResult r = rt.rgUploadLight( &i );
        RG_CHECK( r );
        }
    }

    // Unattended capture -- see rt_autoshot.
    if( primaryLevel && ( int{ cvar::rt_autoshot } > 0 || int{ cvar::rt_autoquit } > 0 ) )
    {
        const int t = primaryLevel->maptime;
        static int s_shotAt = -1;
        static int s_quitAt = -1;
        if( int{ cvar::rt_autoshot } > 0 && t >= int{ cvar::rt_autoshot } && s_shotAt != t )
        {
            s_shotAt = t;
            static int s_lastShot = -100000;
            const int  every      = std::max( 0, int{ cvar::rt_autoshot_every } );
            if( s_lastShot < 0 || ( every > 0 && t - s_lastShot >= every ) )
            {
                s_lastShot = t;
                Printf( "rt_autoshot: screenshot at maptime %d\n", t );
                AddCommandString( "screenshot" );
            }
        }
        if( int{ cvar::rt_autoquit } > 0 && t >= int{ cvar::rt_autoquit } && s_quitAt != t )
        {
            s_quitAt = t;
            static bool s_quitDone = false;
            if( !s_quitDone )
            {
                s_quitDone = true;
                Printf( "rt_autoquit: quitting at maptime %d\n", t );
                AddCommandString( "quit" );
            }
        }
    }

    // BEFORE the fixture walks: they offer their lights into this list as they
    // upload them, and RT_ShaftLightsSelect() reads it when the volumetric
    // params are built further down. See rt_light_shafts.cpp.
    RT_ShaftLightsBegin();

    RT_UploadExportableSectorLights();
    RT_UploadGzDoomDynamicLights();
    RT_UploadCeilingInsetLamps();
    RT_UploadHangingTechLamps();
    RT_UploadHandGlowLights();
    RT_UploadFlameLights();
    RT_UploadLavaLights();
    RT_UploadSwitchLights();
    RT_UpdateSectorEmisThreshold();
    RT_WatchLightlevels();
    RT_UploadWallStripLights();
    RT_UploadSpinPanelLights();
    RT_UploadCeilingEdgeLamps();
    RT_UpdateSmokePuffs();
    // Impact sparks, in this order: step the pool, draw it, then light it. The
    // draw has to follow the sim or the batch is a frame stale, and the lights
    // follow the draw only so the debug ladder's C/sent line counts both.
    //
    // The projectile walk goes FIRST, so an arc spawned by an impact this tic is
    // stepped and drawn in the same frame rather than one late. It shares the
    // spark pool, which is why it belongs to this block and not beside the smoke
    // walk it deliberately does not hook into.
    RT_UpdateProjectileImpacts();
    RT_UpdateSparks();
    RT_DrawSparks();
    // Dust motes. After the sparks so both batches are built in one place, and
    // stateless -- there is nothing to step, so there is no Update half.
    RT_DrawDust();
    RT_UploadSparkLights();
    RT_SparkDebugTick();
    RT_DebugNearbyWallTextures();

    auto tm_params = RgDrawFrameTonemappingParams{
        .sType                = RG_STRUCTURE_TYPE_DRAW_FRAME_TONEMAPPING_PARAMS,
        .pNext                = nullptr,
        .disableEyeAdaptation = false,
        .ev100Min             = cvar::rt_tnmp_ev100_min,
        .ev100Max             = cvar::rt_tnmp_ev100_max,
        .luminanceWhitePoint  = cvar::rt_classic_white,
        .saturation           = { cvar::rt_tnmp_saturation_r,
                                  cvar::rt_tnmp_saturation_g,
                                  cvar::rt_tnmp_saturation_b },
        .crosstalk            = { cvar::rt_tnmp_crosstalk_r,
                                  cvar::rt_tnmp_crosstalk_g,
                                  cvar::rt_tnmp_crosstalk_b },
        .contrast             = cvar::rt_tnmp_contrast,
        .hdrBrightness        = cvar::rt_hdr_brightness,
        .hdrContrast          = cvar::rt_hdr_contrast,
        .hdrSaturation        = { cvar::rt_hdr_saturation,
                                  cvar::rt_hdr_saturation,
                                  cvar::rt_hdr_saturation },
    };

    // 0..255 cvar triple -> RgFloat3D. The liquid palette is 24 of these.
    auto l_col255 = []( int r, int g, int b ) {
        return RgFloat3D{ std::clamp( r / 255.f, 0.f, 1.f ),
                          std::clamp( g / 255.f, 0.f, 1.f ),
                          std::clamp( b / 255.f, 0.f, 1.f ) };
    };

    auto reflrefr_params = RgDrawFrameReflectRefractParams{
        .sType                   = RG_STRUCTURE_TYPE_DRAW_FRAME_REFLECT_REFRACT_PARAMS,
        .pNext                   = &tm_params,
        .maxReflectRefractDepth  = safe_uint( *cvar::rt_reflrefr_depth ),
        .typeOfMediaAroundCamera = RG_MEDIA_TYPE_VACUUM,
        .indexOfRefractionGlass  = cvar::rt_refr_glass,
        .indexOfRefractionWater  = cvar::rt_refr_water,
        .waterWaveSpeed          = cvar::rt_water_wavespeed,
        .waterWaveNormalStrength = cvar::rt_water_wavestren,
        .waterColor              = { std::clamp( *cvar::rt_water_r / 255.f, 0.f, 1.f ),
                                     std::clamp( *cvar::rt_water_g / 255.f, 0.f, 1.f ),
                                     std::clamp( *cvar::rt_water_b / 255.f, 0.f, 1.f ) },
        .waterWaveTextureDerivativesMultiplier = 1.0f,
        .waterTextureAreaScale                 = cvar::rt_water_areascale,
        .portalNormalTwirl                     = false,
        // Doom64-RT stylized water — see rt_water_style.
        .stylizedWaterStrength  = cvar::rt_water_style ? 1.0f : 0.0f,
        .stylizedWaterCaustic   = cvar::rt_water_caustic,
        .stylizedWaterReflMax   = cvar::rt_water_reflmax,
        .stylizedWaterRoughness = cvar::rt_water_rough,
        .stylizedWaterGlow      = cvar::rt_water_glow,
        .stylizedWaterVeinRef   = cvar::rt_water_veinref,
        // Indexed by the liquid id l_waterflag() packs into the primitive
        // flags: 0 water, 1 nukage, 2 sludge, 3 blood. Order matters.
        .stylizedLiquidTint     = { l_col255( cvar::rt_water_tint_r,
                                              cvar::rt_water_tint_g,
                                              cvar::rt_water_tint_b ),
                                    l_col255( cvar::rt_nukage_tint_r,
                                              cvar::rt_nukage_tint_g,
                                              cvar::rt_nukage_tint_b ),
                                    l_col255( cvar::rt_sludge_tint_r,
                                              cvar::rt_sludge_tint_g,
                                              cvar::rt_sludge_tint_b ),
                                    l_col255( cvar::rt_blood_tint_r,
                                              cvar::rt_blood_tint_g,
                                              cvar::rt_blood_tint_b ) },
        .stylizedLiquidCrest    = { l_col255( cvar::rt_water_crest_r,
                                              cvar::rt_water_crest_g,
                                              cvar::rt_water_crest_b ),
                                    l_col255( cvar::rt_nukage_crest_r,
                                              cvar::rt_nukage_crest_g,
                                              cvar::rt_nukage_crest_b ),
                                    l_col255( cvar::rt_sludge_crest_r,
                                              cvar::rt_sludge_crest_g,
                                              cvar::rt_sludge_crest_b ),
                                    l_col255( cvar::rt_blood_crest_r,
                                              cvar::rt_blood_crest_g,
                                              cvar::rt_blood_crest_b ) },
        .lavaEmisBoost          = std::max( 0.f, float{ cvar::rt_lava_emis } ),
        .lavaFlowStrength       = std::clamp( float{ cvar::rt_lava_flow }, 0.f, 1.f ),
        .lavaFlowSpeed          = cvar::rt_lava_flow_speed,
        .lavaFlowScale          = cvar::rt_lava_flow_scale,
        .lavaFlowPixel          = cvar::rt_lava_flow_pixel,
        .lavaPulse              = std::clamp( float{ cvar::rt_lava_pulse }, 0.f, 1.f ),
        .lavaPulseSpeed         = cvar::rt_lava_pulse_speed,
        .lavaGiBoost            = std::max( 0.f, float{ cvar::rt_lava_gi } ),
        .lavaDebug              = cvar::rt_lava_debug ? 1.f : 0.f,
        .lavaTint               = l_col255( cvar::rt_lava_tint_r,
                                            cvar::rt_lava_tint_g,
                                            cvar::rt_lava_tint_b ),
        .stylizedWaterDebug     = float( *cvar::rt_water_debug ),
        .stylizedWaterReflMin   = cvar::rt_water_reflmin,
        .waterCausticGain       = cvar::rt_water_caustics,
        .waterCausticScale      = cvar::rt_water_caustic_scale,
        .waterCausticSpeed      = cvar::rt_water_caustic_speed,
        // map units -> metres: RTGL world space is metres. Passing map units
        // straight through made the probe ray 6144 m long, so a wall anywhere
        // above any water in the map got lit.
        .waterCausticDist       = cvar::rt_water_caustic_dist * ONEGAMEUNIT_IN_METERS,
        .waterCausticRise       = cvar::rt_water_caustic_rise * ONEGAMEUNIT_IN_METERS,
        .waterCausticSlant      = cvar::rt_water_caustic_slant,
        .waterCausticWallBoost  = cvar::rt_water_caustic_wall,
    };

    auto sky_params = RgDrawFrameSkyParams{
        .sType              = RG_STRUCTURE_TYPE_DRAW_FRAME_SKY_PARAMS,
        .pNext              = &reflrefr_params,
        .skyType            = m_wassky ? RG_SKY_TYPE_RASTERIZED_GEOMETRY : RG_SKY_TYPE_COLOR,
        // Dark space tint if raster sky failed (pure black often tonemaps to white voids).
        .skyColorDefault    = { 0.02f, 0.02f, 0.05f },
        .skyColorMultiplier = cvar::rt_sky,
        .skyColorSaturation = cvar::rt_sky_saturation,
        .skyViewerPosition  = { 0, 0, 0 },
        .sunRequireSky      = bool{ cvar::rt_sun_require_sky } ? 1.f : 0.f,
        .sunLeakDebug       = float( std::clamp( int{ cvar::rt_sun_leak_debug }, 0, 2 ) ),
        // RTGL folds intensity into the light's colour before the shader sees it,
        // so a debug colour of 1.0 would arrive ~90x dimmer than the moon and be
        // invisible in exactly the near-black rooms this is for. Carry the
        // intensity across so red and blue read at the same strength the moon does.
        .sunLeakDebugMul    = std::max( 1.f, float{ cvar::rt_sun_intensity } ),
        .sunSkyProbeMaxDist = std::max( 0.f, float{ cvar::rt_sun_skyprobe_dist } ),
        .sunSplit           = bool{ cvar::rt_sun_split } ? 1.f : 0.f,
    };

    // Doom64-RT: the map's own fog, if it has any. A fogged map REPLACES the
    // global rt_volume_* values rather than adding to them -- two densities
    // stacked is a fog nobody authored, and it would make the global knobs mean
    // something different on nine maps than on the other twenty-three.
    RT_ResolveFogIfPending();
    const ResolvedFog fog = RT_ResolveFog();

    // Doom64-RT: LOCALISED SMOKE. Its own struct on the pNext chain, so a frame
    // with no puffs -- which is every frame until someone fires -- reaches RTGL1
    // with puffCount 0, and the froxel shader collapses back to exactly the
    // fog's arithmetic. The four guarded edits inside volumetrics_params below
    // are all `fog.on ? <the value that shipped> : ...`, so a fogged map takes
    // the branch it has always taken and smoke cannot retune it.
    RgFloat4D smoke_puffs[ RG_MAX_SMOKE_PUFFS ]{};
    RgFloat4D smoke_albden[ RG_MAX_SMOKE_PUFFS ]{};
    RgFloat4D smoke_shape[ RG_MAX_SMOKE_PUFFS ]{};
    uint32_t  smoke_count = 0;

    const uint32_t smoke_budget =
        uint32_t( std::clamp( int{ cvar::rt_smoke_budget }, 0, int{ RG_MAX_SMOKE_PUFFS } ) );

    // ONE predicate for both the reach below and the packing loop. Splitting
    // them let `rt_smoke_budget 0` shorten the volume without sending a single
    // puff -- which on an unfogged map is rt_volume_scatter's density over 14 m
    // instead of 30, i.e. a visibly thicker global haze from the arm meant to
    // turn smoke OFF.
    const bool smoke_live = cvar::rt_smoke && g_smokePuffCount > 0 && smoke_budget > 0;

    // Smoke may only take the volume OVER when nothing else is in it -- no fog
    // AND no global rt_volume_* medium. Anything less and firing the gun rewrites
    // the settings of a medium somebody else is using: the first version tested
    // only !fog.on, so on any unfogged map a shot set the global density to 0 and
    // the reach from 30 m to 14, and the moon's light shafts -- which ARE that
    // medium being scattered -- vanished for as long as the trigger was down.
    //
    // Smoke ADDS to whatever is already there. That is the whole design, it is
    // what the density-weighted blend in Smoke.h is for, and this predicate is
    // where the engine has to honour it.
    const bool smoke_owns = !fog.on && cvar::rt_volume_type == 0 && smoke_live;

    // The volume's reach this frame, decided before the puffs are packed because
    // the density scale below depends on it. Fog always wins, and the global
    // medium wins over smoke for the same reason: shortening the reach while
    // someone else's medium is in the volume would silently thicken it too,
    // because its density is per CELL and the cells would get shorter
    // (rt-fog.md S6).
    const float smoke_far = fog.on       ? fog.far_m
                            : smoke_owns ? float{ cvar::rt_smoke_far }
                                         : float{ cvar::rt_volume_far };

    // ...and the same coupling, applied to the GLOBAL medium's own density.
    //
    // RtVolumetric.rgen multiplies its coefficient per CELL and the grid is 64
    // slices whatever the reach, so rt_volume_far is a density knob as much as a
    // reach one: raising it 30 -> 60 for smoke's render distance doubled the
    // slice thickness, which halved how many cells a shaft crosses, which halved
    // the light scattered out of it. Reported from play as the moon's shafts
    // going weak on MAP01 after the smoke work, and visible from IN FRONT of the
    // opening only -- looking up along the beam the phase function's ~11x
    // forward bias still carried it, which is what made the report read as a
    // contradiction rather than as a dimming.
    //
    // So rt_volume_scatter is normalised into a per-METRE density here, against
    // the 30 m reach it was tuned at. Same reasoning, and the same arithmetic,
    // as the slice thickness smoke already pays a few lines below -- smoke was
    // immune to this precisely because it pays it, which is why the moon was the
    // only thing that changed.
    //
    // FOG IS NOT NORMALISED, deliberately. It has its own tuned pair
    // (rt_fog_far / rt_fog_density) on nine maps, RT_FOG_PRESETS is stated in
    // those units, and rt-fog.md S6 documents the coupling as part of the
    // contract. Touching it would retune every fogged map for a MAP01 report;
    // ab-smoke.cmd fogsafe asserts it did not.
    constexpr float RT_VOLUME_REF_FAR = 30.f;

    const float volume_dens = float{ cvar::rt_volume_scatter } *
                              ( std::max( 0.001f, smoke_far ) / RT_VOLUME_REF_FAR );

    if( smoke_live )
    {
        // rt_smoke_density is optical depth per METRE, but RtVolumetric.rgen
        // applies a flat coefficient per CELL -- which is why rt_fog_far
        // silently changes what a fog density means (rt-fog.md S6). Paying the
        // slice thickness here instead means a puff looks the same whatever the
        // volume's reach is, including on a fogged map where it is not ours.
        //
        // 64 is VOLUMETRIC_SIZE_Z. It is generated into RTGL1's private
        // ShaderCommonC.h and not exported through RTGL1.h, so it has to be
        // restated here; if the grid depth ever changes, this is the second
        // place. The slices are uniform in distance (VOLUMETRIC_DISTANCE_POW 1),
        // which is what makes one thickness correct for the whole volume.
        constexpr int RT_VOLUME_SLICES = 64;

        const float sliceM = std::max( 0.001f, smoke_far / float( RT_VOLUME_SLICES ) );

        const uint32_t hex = uint32_t( cvar::rt_smoke_color );
        const FVector3 col{ float( ( hex >> 16 ) & 0xFF ) / 255.0f,
                            float( ( hex >> 8 ) & 0xFF ) / 255.0f,
                            float( hex & 0xFF ) / 255.0f };

        // ...and the shader multiplies what we send by RT_VOLUME_CELL_COEFF per
        // cell. Both factors are needed: cells-per-metre is 1/sliceM and the
        // coefficient is 0.001, so density_uniform = k * sliceM / 0.001. Sending
        // only k*sliceM -- which is what the first version did -- is a THOUSAND
        // times too thin: tau across a whole puff comes out at 0.002 and the
        // smoke is mathematically invisible, which is precisely how it looked.
        constexpr float RT_VOLUME_CELL_COEFF = 0.001f;

        // The per-metre density now lives on the PUFF (captured at spawn with its
        // weapon's multiplier applied), so two weapons' smoke can be in the air
        // at once at different densities. Only the grid conversion is shared.
        const auto& vpos = r_viewpoint.Pos;
        const auto  eye  = FVector3{ float( vpos.X ), float( vpos.Y ), float( vpos.Z ) } *
                          ONEGAMEUNIT_IN_METERS;

        // Nearest-first, then take the budget: with more puffs than the uniform
        // can carry, the ones to keep are the ones in front of your face.
        struct Cand
        {
            float    d2;
            uint32_t idx;
        };
        std::array< Cand, RG_MAX_SMOKE_PUFFS > cand{};
        uint32_t                               ncand = 0;
        for( uint32_t i = 0; i < g_smokePuffCount; i++ )
        {
            cand[ ncand++ ] = Cand{ float( ( g_smokePuffs[ i ].pos - eye ).LengthSquared() ), i };
        }

        if( ncand > smoke_budget )
        {
            std::partial_sort( cand.begin(),
                               cand.begin() + smoke_budget,
                               cand.begin() + ncand,
                               []( const Cand& a, const Cand& b ) { return a.d2 < b.d2; } );
            ncand = smoke_budget;
        }

        for( uint32_t i = 0; i < ncand; i++ )
        {
            const SmokePuff& puff = g_smokePuffs[ cand[ i ].idx ];

            // Conserve the parcel: as it expands, thin it out. Exponent ONE, not
            // two -- what a ray collects is optical DEPTH, density x path length,
            // so density ~ 1/r holds the depth through the core CONSTANT as the
            // parcel spreads. At 1/r^2 the depth falls as 1/r on top of the
            // (1-t)^2 age fade, the two compound, and a wisp that grows 8x over
            // its life disappears entirely -- which is what happened.
            const float dilute =
                puff.radius > puff.radius0 ? ( puff.radius0 / puff.radius ) : 1.f;

            const float t = std::clamp( puff.age / puff.life, 0.f, 1.f );
            // (1 - t)^2: a puff spends its last third nearly gone, so it thins
            // out rather than blinking off at the end of its life.
            const float fade = ( 1.f - t ) * ( 1.f - t );

            // A PUFF IS AN ELLIPSOID, NOT A SPHERE, and that is what lets a
            // filament be a filament.
            //
            // The froxel grid's two axes differ by a factor of forty: at 1.5 m
            // one cell is 1.7 cm across the screen and 47 cm deep. A sphere has
            // ONE radius, so to be resolvable in depth it needs half a slice --
            // and that same 23 cm is then its width on screen. That is why the
            // pistol kept coming back as a ball however small the profile asked
            // for: the depth requirement was setting the visible size.
            //
            // So the two radii are sent separately. Across the view the puff is
            // exactly what the profile asked for -- centimetres, if that is what
            // it wants. Along the view it is padded to half a slice, which is
            // invisible because it is the axis you are looking down.
            //
            // SUB-GRID PUFFS MUST NOT VANISH.
            //
            // The volume's slices are sliceM apart along the view, so a puff
            // thinner than about one slice can fall BETWEEN two sample points
            // and contribute to no cell at all -- it does not render faint, it
            // renders nothing, intermittently. That is what a thin per-weapon
            // profile (the pistol's filament) runs into: 0.17 m radius against
            // 0.47 m slices is 0.7 cells across its whole diameter.
            //
            // So widen it to the smallest footprint the grid can actually carry
            // and DIVIDE THE DENSITY BY THE SAME FACTOR, which keeps the optical
            // depth through the puff unchanged. The wisp reads slightly softer
            // than authored instead of blinking in and out, and the number the
            // profile asked for still means what it says.
            // 0.5 of a slice, not 0.75. At 0.75 the floor is 0.35 m -- exactly
            // the default radius -- so EVERY small profile was quietly widened
            // back to the default and the pistol's filament came back fat. Half
            // a slice still guarantees a cell centre can land inside the puff
            // (centres are one slice apart, so a diameter of one slice cannot be
            // stepped over) while letting a thin profile stay visibly thinner.
            const float minR   = 0.5f * sliceM;
            const float rAlong = std::max( puff.radius, minR );
            const float rPerp  = puff.radius;

            // The optical depth a ray collects is set by the ALONG-view extent,
            // since that is the direction it travels. Padding that extent would
            // thicken the puff, so divide it back out -- the puff looks the size
            // it asked for and is as dense as it asked to be.
            const float thin = puff.radius / rAlong;

            // The view direction to THIS puff, precomputed. The shader's
            // ellipsoid test needs it to split the offset into along-view and
            // across-view parts, and it used to derive it per froxel with a
            // normalize() -- a square root inside a loop that runs over every
            // uploaded puff for each of ~900k cells. It is constant per puff per
            // frame, and smokeShape.yzw were already spare, so it is computed
            // here exactly once instead.
            FVector3 vdir = puff.pos - eye;
            const float vlen = vdir.Length();
            // A puff centred exactly on the eye has no view direction. Any unit
            // vector is as good as another there -- every offset is "across" it
            // -- and leaving it unnormalized would send a NaN into the volume.
            vdir = vlen > 1e-4f ? vdir / vlen : FVector3{ 0.f, 0.f, 1.f };

            smoke_puffs[ i ]  = RgFloat4D{ puff.pos.X, puff.pos.Y, puff.pos.Z, rAlong };
            smoke_shape[ i ]  = RgFloat4D{ rPerp, vdir.X, vdir.Y, vdir.Z };
            smoke_albden[ i ] = RgFloat4D{
                col.X, col.Y, col.Z,
                puff.density * thin * dilute * sliceM / RT_VOLUME_CELL_COEFF * fade };
            smoke_count++;
        }
    }

    if( cvar::rt_smoke_debug )
    {
        // Every second, not every frame: the interesting failure is "nothing is
        // happening", and a per-frame log of that scrolls the reason for it off
        // the console. Reports what was SENT, so a live count with nothing on
        // screen separates "no puffs spawned" from "puffs are not being drawn".
        static int s_tick = 0;
        if( ( ++s_tick % 35 ) == 0 )
        {
            const auto& vp = r_viewpoint.Pos;
            const FVector3 eyeM{ float( vp.X ) * ONEGAMEUNIT_IN_METERS,
                                 float( vp.Y ) * ONEGAMEUNIT_IN_METERS,
                                 float( vp.Z ) * ONEGAMEUNIT_IN_METERS };

            if( smoke_count > 0 )
            {
                const FVector3 p0{ smoke_puffs[ 0 ].data[ 0 ],
                                   smoke_puffs[ 0 ].data[ 1 ],
                                   smoke_puffs[ 0 ].data[ 2 ] };
                // Distance from the eye matters more than the position: the froxel
                // volume only spans volumeCameraNear..far ALONG THE VIEW, so a puff
                // nearer than the near plane or past the far one is simply not in
                // the grid, and would look exactly like "the shader ignored it".
                Printf( "rt_smoke C/sent: %u live, %u sent (budget %d) | puff0 %.2f %.2f %.2f "
                        "r=%.2fm density=%.1f | eye %.2f %.2f %.2f dist=%.2fm | "
                        "far %.1fm slice %.3fm cells-across=%.2f | fog %s vol_type %d "
                        "owns=%d illum=%d DEBUGMODE=%d\n",
                        g_smokePuffCount,
                        smoke_count,
                        int{ cvar::rt_smoke_budget },
                        p0.X, p0.Y, p0.Z,
                        smoke_puffs[ 0 ].data[ 3 ],
                        smoke_albden[ 0 ].data[ 3 ],
                        eyeM.X, eyeM.Y, eyeM.Z,
                        ( p0 - eyeM ).Length(),
                        smoke_far,
                        smoke_far / 64.f,
                        2.f * smoke_puffs[ 0 ].data[ 3 ] / ( smoke_far / 64.f ),
                        fog.on ? "on" : "off",
                        int{ cvar::rt_volume_type },
                        smoke_owns ? 1 : 0,
                        bool{ cvar::rt_smoke_illum } ? 1 : 0,
                        int{ cvar::rt_smoke_debug } );
            }
            else
            {
                Printf( "rt_smoke C/sent: 0 sent | live=%u rt_smoke=%d budget=%d "
                        "DEBUGMODE=%d (live>0 with 0 sent means the packing gate "
                        "rejected them)\n",
                        g_smokePuffCount,
                        int{ cvar::rt_smoke },
                        int{ cvar::rt_smoke_budget },
                        int{ cvar::rt_smoke_debug } );
            }
        }
    }

    auto smoke_params = RgDrawFrameSmokeParams{
        .sType          = RG_STRUCTURE_TYPE_DRAW_FRAME_SMOKE_PARAMS,
        .pNext          = &sky_params,
        .puffCount      = smoke_count,
        .pPuffs         = smoke_puffs,
        .pAlbedoDensity = smoke_albden,
        .pShape         = smoke_shape,
        // Both are chosen PER FROXEL in RtVolumetric.rgen, against smoke density
        // rather than against a per-frame flag, so neither can move a fog cell.
        .lightNearFade  = std::max( 0.f, float{ cvar::rt_smoke_light_near } ),
        .illumBlend     = std::clamp( float{ cvar::rt_smoke_illum_blend }, 0.f, 1.f ),
        .allLights      = bool{ cvar::rt_smoke_illum },
        .lightFarFade   = std::max( 0.f, float{ cvar::rt_smoke_light_far } ),
        // 32, matching the clamp in RTGL's VulkanDevice.cpp -- the two have to
        // agree or the engine silently asks for more than the shader will do.
        .samplesPerCell = uint32_t( std::clamp( int{ cvar::rt_smoke_spp }, 1, 32 ) ),
        .maxLight       = std::max( 0.f, float{ cvar::rt_smoke_maxlight } ),
        .debugMode      = uint32_t( std::max( 0, int{ cvar::rt_smoke_debug } ) ),
        // Stylization. Inside smoke_evalAt rather than a screen-space filter, so
        // a frame with no puffs is untouched and smoke-fogsafe still holds.
        .stylize        = std::clamp( float{ cvar::rt_smoke_stylize }, 0.f, 1.f ),
        .stylizeSteps   = uint32_t( std::clamp( int{ cvar::rt_smoke_stylize_steps }, 1, 64 ) ),
        .stylizeGrid    = std::max( 0.f, float{ cvar::rt_smoke_stylize_grid } ),
        // SMOKE'S OWN unlit floor, per froxel. Note this is the same cvar that
        // feeds ambientColor below -- but that path only fires when smoke OWNS
        // the volume (no fog, rt_volume_type 0), which the shipping config
        // never is. So for years of this feature rt_smoke_ambient did nothing
        // at all, and smoke was visible only while a light was on it.
        .selfAmbient    = std::max( 0.f, float{ cvar::rt_smoke_ambient } ),
        .tintBias       = std::clamp( float{ cvar::rt_smoke_tint }, 0.f, 1.f ),
        .absorb         = std::max( 0.f, float{ cvar::rt_smoke_absorb } ),
    };

    // LIGHT SHAFTS FROM ORDINARY LAMPS. The list was collected by the fixture
    // walks above; this only says how it is to be scattered. Empty (and so free)
    // whenever rt_volume_shafts is off or nothing qualified.
    //
    // Its own pNext struct rather than fields on the volumetric params, for the
    // reason the smoke block is: the fog is shipped and tuned on nine maps, and
    // a struct that does not change size cannot break it.
    const std::vector< uint64_t >& shaft_ids = RT_ShaftLightsSelect();

    auto shaft_params = RgDrawFrameLightShaftParams{
        .sType           = RG_STRUCTURE_TYPE_DRAW_FRAME_LIGHT_SHAFT_PARAMS,
        .pNext           = &smoke_params,
        .count           = uint32_t( shaft_ids.size() ),
        .pLightUniqueIds = shaft_ids.empty() ? nullptr : shaft_ids.data(),
        .multiplier      = std::max( 0.f, float{ cvar::rt_volume_shaft_mult } ),
        // METRES, like every other position and radius crossing this boundary.
        // Not map units -- see ONEGAMEUNIT_IN_METERS; a light placed in map
        // units lands 32x out and reads as simply absent.
        .nearFade    = std::max( 0.f, float{ cvar::rt_volume_shaft_nearfade } ),
        .minRadiance = std::max( 0.f, float{ cvar::rt_volume_shaft_mincontrib } ),
        .maxTraced   = uint32_t( std::clamp( int{ cvar::rt_volume_shaft_trace }, 1, 32 ) ),
        // Below -1 is the "share rt_volume_lassymetry" sentinel, resolved on the
        // RTGL1 side so the two cannot drift.
        .asymmetry = float{ cvar::rt_volume_shaft_asym },
        .debugMode = uint32_t( std::clamp( int{ cvar::rt_volume_shaft_debug }, 0, 3 ) ),
        // The two knobs the "shafts do not reach" report needed: how much of the
        // inverse-square falloff is given back, and how the per-froxel ray
        // budget is decided. See rt_volume_shaft_falloff / _relcull.
        .falloffCompensation =
            std::clamp( float{ cvar::rt_volume_shaft_falloff }, 0.f, 2.f ),
        .relativeCull = std::clamp( float{ cvar::rt_volume_shaft_relcull }, 0.f, 1.f ),
    };

    auto volumetrics_params = RgDrawFrameVolumetricParams{
        .sType                   = RG_STRUCTURE_TYPE_DRAW_FRAME_VOLUMETRIC_PARAMS,
        .pNext                   = &shaft_params,
        .enable                  = fog.on || cvar::rt_volume_type != 0 || smoke_count > 0,
        // Smoke-only frames drop the history: it is tuned for fog, which moves
        // no faster than the player, and it smears a puff that does.
        // THE SECOND TEMPORAL FILTER, and the one that made smoke stay bright
        // after its muzzle flash had gone.
        //
        // The froxel's own blend (rt_smoke_illum_blend) decays in a handful of
        // frames. THIS is a per-pixel accumulation on top of it, 8 frames deep
        // and tuned for fog -- which moves no faster than the player. A muzzle
        // flash lasts 2-3 frames, so the puff it lit goes on glowing through the
        // whole window after the light is gone.
        //
        // The `smoke_owns ? 0` above was meant to prevent exactly that, and
        // never fired: smoke_owns needs rt_volume_type 0 and the shipping pin is
        // 1, the same dead gate that made rt_smoke_ambient a no-op. So the
        // shortened history is chosen on SMOKE BEING LIVE instead.
        //
        // NOT on a fogged map. Shortening the window there would retune the fog
        // every time the player pulled the trigger, which is the per-frame trap
        // docs/rt-smoke.md section 5 exists to forbid. Fogged maps keep the
        // fog's history and the smoke on them keeps the smear; that is the
        // conservative half of the trade and it costs nine maps a little.
        // rt_smoke_history is used DIRECTLY, not min()'d with rt_volume_history.
        //
        // The min was there because this knob was only ever meant to SHORTEN the
        // window: the fog's 8 frames made a muzzle-lit puff smear, so smoke asked
        // for 2. But it also silently capped the knob at 8 in the other direction,
        // so "history 20" and "history 8" were the same setting and the cvar looked
        // broken to anyone trying to use accumulation to denoise -- which is a real
        // use for it, since the volume has no spatial denoiser worth the name.
        // Shortening still works exactly as before; lengthening now does too, and
        // the smear it buys back is the caller's choice to make.
        .maxHistoryLength        = ( smoke_live && !fog.on )
                                       ? float{ cvar::rt_smoke_history }
                                   : ( fog.on || cvar::rt_volume_type == 1 )
                                       ? float{ cvar::rt_volume_history }
                                       : 0.f,
        // A fogged map always takes the froxel path: the depth-based one cannot
        // be lit, which is the whole point here. rt_volume_type 2 still selects
        // it for an A/B (tools/ab-fog.cmd flat) because that arm turns rt_fog
        // off first.
        .useSimpleDepthBased     = !fog.on && cvar::rt_volume_type == 2,
        // smoke_far is fog.far_m on a fogged map, so this is unchanged there.
        // Smoke-only it is rt_smoke_far, which is a RESOLUTION knob: 64 slices
        // over 14 m are 0.22 m thick and can resolve a puff, where the 30 m of
        // rt_volume_far gives 0.47 m and a puff reads as one slab.
        .volumetricFar           = smoke_far,
        .ambientColor            = fog.on ? RgFloat3D{ fog.ambient, fog.ambient, fog.ambient }
                                   : smoke_owns
                                       ? RgFloat3D{ cvar::rt_smoke_ambient,
                                                    cvar::rt_smoke_ambient,
                                                    cvar::rt_smoke_ambient }
                                       : RgFloat3D{ cvar::rt_volume_ambient,
                                                    cvar::rt_volume_ambient,
                                                    cvar::rt_volume_ambient },
        // Zero base density is what makes smoke-only mode free of side effects:
        // a cell with no puff in it stores vec4( 0 ) exactly as it does today,
        // and the far slice past rt_smoke_far is empty rather than a wall.
        .scaterring              = fog.on ? fog.density
                                   : smoke_owns ? 0.f
                                                : volume_dens,
        .assymetry               = cvar::rt_volume_lassymetry,
        .useIlluminationVolume   = cvar::rt_illum_volume && cvar::rt_volume_type != 0,
        .fallbackSourceColor     = { 0, 0, 0 },
        .fallbackSourceDirection = { 0, -1, 0 },
        .lightMultiplier         = fog.on ? std::max( 0.f, float{ cvar::rt_fog_lightmult } )
                                          : float{ cvar::rt_volume_lintensity },
        .allowTintUnderwater     = false,
        .underwaterColor         = {},
        // The two RTGL1 additions this feature is built on. Both are no-ops off
        // a fogged map: mediaColor { 1, 1, 1 } is the identity tint, and
        // illuminateFromAllLights false leaves the stock single-light pass
        // exactly as it was.
        // FOG ONLY. Smoke wants the all-lights estimate too, but asking for it
        // here would switch the ENTIRE volume off
        // traceDirectIllumination_SpecificLight -- and that function is the only
        // place the sun's sky-probe test lives, i.e. the only thing that makes a
        // map's light shafts. Smoke asks per FROXEL instead, via
        // RgDrawFrameSmokeParams::allLights.
        .illuminateFromAllLights = fog.on && fog.illum,
        .mediaColor              = fog.on ? RgFloat3D{ fog.r, fog.g, fog.b }
                                          : RgFloat3D{ 1.f, 1.f, 1.f },
        // Near and far are one medium with a ramp through it, not two fogs. See
        // rt_fog_density_far -- the froxel slices are uniform in distance, so
        // this costs one mix() per cell and nothing else.
        .mediaColorFar           = fog.on ? RgFloat3D{ fog.rf, fog.gf, fog.bf }
                                          : RgFloat3D{ 1.f, 1.f, 1.f },
        // Same normalisation as .scaterring: the near and far ends are one
        // medium, so they have to be in the same units or the ramp bends with
        // the reach. And the smoke_owns case has to state its zero here too --
        // near 0 with a non-zero far is a ramp from clear air into haze, which
        // is the opposite of the "far slice is empty rather than a wall" that
        // zero base density is for.
        .farScattering           = fog.on ? fog.density_far
                                   : smoke_owns ? 0.f
                                                : volume_dens,
        .densityCurve            = fog.on ? fog.curve : 1.f,
        .occludeEmission = bool{ cvar::rt_volume_occlude_emis },
        .ditherRadius  = std::max( 0.f, float{ cvar::rt_volume_dither } ),
        // The DEPTH half, on its own leash. -sampleHemisphere() is one-sided in
        // z, so this radius is a mean shortfall of 0.33 * radius froxels against
        // a prefix-summed volume rather than a symmetric jitter -- it deletes the
        // far end of every column instead of blurring it. See rt_volume_dither_z.
        .ditherRadiusZ = std::max( 0.f, float{ cvar::rt_volume_dither_z } ),
        .spatialBlur   = std::clamp( float{ cvar::rt_volume_blur }, 0.f, 1.f ),
        .lightNearFade = fog.on ? std::max( 0.f, float{ cvar::rt_fog_light_near } ) : 0.f,
        // THE FROXEL DEPTH GATE. Stops the volume lighting air the camera cannot
        // see -- see rt_volume_depthgate, and docs/plan-light-shafts.md 4d for
        // why this is not a visibility fix and why no per-light test could have
        // worked. Applies to fogged maps too: the mechanism is the trilinear
        // read of a prefix sum and it does not care which medium filled the
        // cell.
        .depthGate        = bool{ cvar::rt_volume_depthgate } ? 1.f : 0.f,
        .depthGateBias    = std::max( 0.f, float{ cvar::rt_volume_depthgate_bias } ),
        .depthGateFeather = std::max( 0.f, float{ cvar::rt_volume_depthgate_feather } ),
        .depthGateTaps    = uint32_t( int{ cvar::rt_volume_depthgate_taps } >= 5 ? 5 : 1 ),
        // THE UPSCALER BIAS MASK. The dark outline at every edge seen through a
        // medium is the upscaler's, not the froxel grid's -- measured, see
        // docs/rt-volumetric-edge-outlines.md -- and this is what tells DLSS and
        // FSR2 where the medium's silhouettes are.
        .volumeUpscaleBias      = std::clamp( float{ cvar::rt_volume_ubias }, 0.f, 1.f ),
        .volumeUpscaleBiasEdge  = std::max( 0.001f, float{ cvar::rt_volume_ubias_edge } ),
        .volumeUpscaleBiasFloor = std::clamp( float{ cvar::rt_volume_ubias_floor }, 0.f, 1.f ),
        .volumeUpscaleBiasDebug = uint32_t( bool{ cvar::rt_volume_ubias_debug } ? 1 : 0 ),
        // THE FIX, rather than the mitigation: move the composite past the
        // upscaler. RTGL gates it off under Ray Reconstruction and frame
        // generation by itself -- see rt_volume_postcomp.
        .volumePostComp = uint32_t( bool{ cvar::rt_volume_postcomp } ? 1 : 0 ),
        .volumeEdgeSoft     = std::max( 0.f, float{ cvar::rt_volume_edgesoft } ),
        .volumeEdgeSoftEdge = std::max( 0.001f, float{ cvar::rt_volume_edgesoft_edge } ),
        .volumeFp           = std::clamp( float{ cvar::rt_volume_fp }, 0.f, 2.f ),
        .volumeReproj       = ( cvar::rt_volume_reproj ? 1u : 0u ),
        .volumeSpriteShadow = ( cvar::rt_volume_spriteshadow ? 1u : 0u ),
        .volumeGridHistory  = std::clamp( float{ cvar::rt_volume_taccum }, 0.f, 64.f ),
    };

    auto texture_params = RgDrawFrameTexturesParams{
        .sType = RG_STRUCTURE_TYPE_DRAW_FRAME_TEXTURES_PARAMS,
        .pNext = &volumetrics_params,
        .dynamicSamplerFilter =
            cvar::rt_smoothtextures ? RG_SAMPLER_FILTER_LINEAR : RG_SAMPLER_FILTER_NEAREST,
        .mipLodBiasOffset       = float( cvar::rt_mip_bias ),
        .normalMapStrength      = cvar::rt_normalmap_stren,
        .emissionMapBoost       = cvar::rt_emis_mapboost,
        .emissionMaxScreenColor = cvar::rt_emis_maxscrcolor,
        .minRoughness           = cvar::rt_refl_thresh,
        .heightMapDepth         = 0.02f * cvar::rt_heightmap_stren,
        // Inverted on purpose: the cvar reads as "metals on", the API field as
        // "strip them". rt_metallic 0 turns the whole hand-labelled metalness
        // pass off without touching a single _orm.png.
        .forceNonMetallic       = !cvar::rt_metallic,
        .metallicMax            = cvar::rt_metallic_max,
        .metallicRoughCut       = cvar::rt_metallic_roughcut,
        .metallicRoughBand      = cvar::rt_metallic_roughband,
        .spritePbr              = cvar::rt_sprite_pbr
                                      ? std::clamp( float{ cvar::rt_sprite_pbr_mix }, 0.f, 1.f )
                                      : 0.f,
        .spriteMetallicMax      = cvar::rt_sprite_metallic_max,
        .spriteRoughMin         = cvar::rt_sprite_rough_min,
        .spriteNormalStrength   = cvar::rt_sprite_normal,
        .worldPbr               = std::clamp( float{ cvar::rt_tex_pbr_mix }, 0.f, 1.f ),
    };

    float dirtscale = ( ( powerupflags & RT_POWERUP_FLAG_RADIATIONSUIT_BIT ) ||
                        ( powerupflags & RT_POWERUP_FLAG_NIGHTVISION_BIT ) )
                          ? 15.f
                          : cvar::rt_bloom_dirt_scale;

    auto bloom_params = RgDrawFrameBloomParams{
        .sType             = RG_STRUCTURE_TYPE_DRAW_FRAME_BLOOM_PARAMS,
        .pNext             = &texture_params,
        .inputEV           = cvar::rt_bloom_ev,
        .inputThreshold    = cvar::rt_bloom_threshold,
        .bloomIntensity    = cvar::rt_bloom ? cvar ::rt_bloom_scale : 0.f,
        .lensDirtIntensity = cvar::rt_bloom_dirt ? dirtscale : 0.f,
    };

    auto illum_params = RgDrawFrameIlluminationParams{
        .sType                              = RG_STRUCTURE_TYPE_DRAW_FRAME_ILLUMINATION_PARAMS,
        .pNext                              = &bloom_params,
        .maxBounceShadows                   = safe_uint( *cvar::rt_shadowrays ),
        .enableSecondBounceForIndirect      = true,
        .cellWorldSize                      = 2.0f,
        .directDiffuseSensitivityToChange   = std::clamp( float( cvar::rt_illum_sens_direct ), 0.f, 1.f ),
        .indirectDiffuseSensitivityToChange = std::clamp( float( cvar::rt_illum_sens_indirect ), 0.f, 1.f ),
        .specularSensitivityToChange        = std::clamp( float( cvar::rt_illum_sens_spec ), 0.f, 1.f ),
        .polygonalLightSpotlightFactor      = 2.0f,
        .lightUniqueIdIgnoreFirstPersonViewerShadows = &FlashlightLightId,
        .enableRrTemporalPrefilter          = static_cast< RgBool32 >( bool( cvar::rt_rr_temporal ) ),
        .enableRrDisocclusionMask           = static_cast< RgBool32 >( bool( cvar::rt_rr_disocc ) ),
        .rrDisocclusionThreshold            = std::max( float( cvar::rt_rr_disocc_ratio ), 1.0f ),
        .rrDisocclusionMinDelta             = std::max( float( cvar::rt_rr_disocc_mindelta ), 0.0f ),
        .rrDisocclusionShowMask             = static_cast< RgBool32 >( bool( cvar::rt_rr_disocc_show ) ),
        .rrFireflyThreshold                 = std::max( float( cvar::rt_rr_firefly ), 0.0f ),
        .rrFireflyMinLum                    = std::max( float( cvar::rt_rr_firefly_minlum ), 0.0f ),
        .restirBlueNoise                    = static_cast< RgBool32 >( bool( cvar::rt_restir_bluenoise ) ),
        .shadowSamples                      = uint32_t( std::clamp( int( cvar::rt_shadow_samples ), 1, 8 ) ),
        .debugRestirM                       = static_cast< RgBool32 >( bool( cvar::rt_debug_restir_m ) ),
        .debugVisibility                    = uint32_t( std::clamp( int( cvar::rt_debug_visibility ), 0, 2 ) ),
        .restirTemporalJitter               = std::clamp( float( cvar::rt_restir_tjitter ), 0.0f, 8.0f ),
        .rrSpecularHitDistance              = static_cast< RgBool32 >( bool( cvar::rt_rr_spechitdist ) ),
        .directSamples                      = uint32_t( std::clamp( int( cvar::rt_spp_direct ), 1, 8 ) ),
        .indirectSamples                    = uint32_t( std::clamp( int( cvar::rt_spp_indirect ), 1, 8 ) ),
        .restirInitialSamples               = uint32_t( std::clamp( int( cvar::rt_restir_initial ), 1, 64 ) ),
        .restirSpatialSamples               = uint32_t( std::clamp( int( cvar::rt_restir_spatial ), 0, 16 ) ),
        .restirSpatialRadius                = std::clamp( float( cvar::rt_restir_spatial_radius ), 1.0f, 64.0f ),
        // RR-scoped decorrelation: ReSTIR's temporal reuse keeps a reservoir
        // winner for up to mcap frames, so a bad shadowed sample persists as a
        // STABLE dark dot -- structure a temporal denoiser preserves as
        // detail. A-SVGF's spatial atrous blurs those away; DLSS-RR has no
        // equivalent and its guide (S3.5) asks for minimally correlated
        // samples outright. Toggling the flashlight reseeds the reservoirs,
        // which is why the dot PATTERN visibly switched with it. Override only
        // on frames where RR actually runs (g_rr_dbg_rrRequested is this
        // frame's RT_UpscaleCvarsToRtgl decision); -1 disables the override.
        .restirTemporalMCap                 = uint32_t( std::clamp(
            ( g_rr_dbg_rrRequested && int( cvar::rt_rr_restir_mcap ) >= 0 )
                                ? int( cvar::rt_rr_restir_mcap )
                                : int( cvar::rt_restir_mcap ),
            1,
            64 ) ),
        .rrGuideMin                         = std::clamp( float( cvar::rt_rr_guide_min ), 0.0f, 1.0f ),
        .rrGuideMode                        = uint32_t( std::clamp( int( cvar::rt_rr_guide_mode ), 0, 2 ) ),
        .restirIndirAntilag                 = static_cast< RgBool32 >( bool( cvar::rt_restir_indir_antilag ) ),
        .rrPreExposure                      = static_cast< RgBool32 >( bool( cvar::rt_rr_preexposure ) ),
        .rrPreExposureDebug                 = static_cast< RgBool32 >( bool( cvar::rt_rr_preexp_debug ) ),
        .rrExposureTexture                  = static_cast< RgBool32 >( bool( cvar::rt_rr_exptex ) ),
        .rrTransparencyLayer                = static_cast< RgBool32 >( bool( cvar::rt_rr_translayer ) ),
        .nrdDenoiser                        = static_cast< RgBool32 >( bool( cvar::rt_nrd ) ),
        .rrGlowPre                          = static_cast< RgBool32 >( bool( cvar::rt_rr_glowpre ) ),
    };

    auto ef_wipe = RgPostEffectWipe{
        .stripWidth = 1.0f / 320.0f,
        .beginNow   = cvar::rt_melt_duration > 0.05f ? g_melt_requested : false,
        .duration   = cvar::rt_melt_duration > 0.05f ? cvar::rt_melt_duration : 0.0f,
    };
    g_melt_requested = false;

    if( ef_wipe.beginNow )
    {
        g_melt_endtime = curtime + static_cast< double >( ef_wipe.duration );
    }
    if( g_melt_endtime > 0 && curtime > g_melt_endtime )
    {
        g_melt_endtime = -1;
    }

    auto ef_radialblur = RgPostEffectRadialBlur{
        .isActive              = powerupflags & RT_POWERUP_FLAG_BERSERK_BIT,
        .transitionDurationIn  = 0.4f,
        .transitionDurationOut = 3.0f,
    };

    bool chrabr_from_powerup = ( powerupflags & RT_POWERUP_FLAG_NIGHTVISION_BIT ) ||
                               ( powerupflags & RT_POWERUP_FLAG_THERMALVISION_BIT ) ||
                               ( powerupflags & RT_POWERUP_FLAG_BERSERK_BIT );

    auto ef_chrabr = RgPostEffectChromaticAberration{
        .isActive              = chrabr_from_powerup || cvar::rt_ef_chraber > 0.f,
        .transitionDurationIn  = 0,
        .transitionDurationOut = 0,
        .intensity             = chrabr_from_powerup ? 1.2f : cvar::rt_ef_chraber,
    };
    // smooth out manually (because it's a constant active effect, i.e. without switching isActive)
    {
        constexpr auto Duration     = 0.5f;
        static double  begin_time   = curtime;
        static float   last_value   = ef_chrabr.intensity;
        static float   begin_value  = ef_chrabr.intensity;
        static float   target_value = ef_chrabr.intensity;

        if( std::abs( target_value - ef_chrabr.intensity ) > 0.001f )
        {
            begin_time   = curtime;
            begin_value  = last_value;
            target_value = ef_chrabr.intensity;
        }

        // if( begin_time <= curtime && curtime <= begin_time + double( Duration ) )
        {
            const float t = std::clamp( float( curtime - begin_time ) / Duration, 0.0f, 1.0f );
            ef_chrabr.intensity = std::lerp( begin_value, target_value, t );
        }
        last_value = ef_chrabr.intensity;
    }

    auto ef_invbw = RgPostEffectInverseBlackAndWhite{
        .isActive              = powerupflags & RT_POWERUP_FLAG_INVUNERABILITY_BIT,
        .transitionDurationIn  = 1.0f,
        .transitionDurationOut = 1.5f,
    };

    auto ef_hueshift = RgPostEffectHueShift{
        .isActive              = powerupflags & RT_POWERUP_FLAG_THERMALVISION_BIT,
        .transitionDurationIn  = 0.5f,
        .transitionDurationOut = 0.5f,
    };

    auto ef_nightvision = RgPostEffectNightVision{
        .isActive              = powerupflags & RT_POWERUP_FLAG_NIGHTVISION_BIT,
        .transitionDurationIn  = 0.5f,
        .transitionDurationOut = 0.5f,
    };

    auto ef_distortedsides = RgPostEffectDistortedSides{
        .isActive              = powerupflags & RT_POWERUP_FLAG_RADIATIONSUIT_BIT,
        .transitionDurationIn  = 1.0f,
        .transitionDurationOut = 1.0f,
    };

    // static, so prev state's transition durations
    // are preserved across frames, when flags are removed
    static auto ef_tint = RgPostEffectColorTint{};
    {
        ef_tint.isActive = false;

        if( auto dmg = RT_DamageIntensity() )
        {
            ef_tint = RgPostEffectColorTint{
                .isActive              = true,
                .transitionDurationIn  = 0.0f,
                .transitionDurationOut = remap01( *dmg, 0.5f, 1.7f ),
                .intensity             = remap01( *dmg, 1.5f, 3.0f ) * blood_fade_scalar,
                .color                 = { 1.f, 0.f, 0.f },
            };
        }
        else if( powerupflags & RT_POWERUP_FLAG_RADIATIONSUIT_BIT )
        {
            ef_tint = RgPostEffectColorTint{
                .isActive              = true,
                .transitionDurationIn  = 1.0f,
                .transitionDurationOut = 1.0f,
                .intensity             = 1.0f,
                .color                 = { 0.2f, 1.f, 0.4f },
            };
        }
        else if( powerupflags & RT_POWERUP_FLAG_BONUS_BIT )
        {
            ef_tint = RgPostEffectColorTint{
                .isActive              = true,
                .transitionDurationIn  = 0.0f,
                .transitionDurationOut = 0.7f,
                .intensity             = 0.5f * pickup_fade_scalar,
                .color                 = { 1.f, 0.91f, 0.42f },
            };
        }
    }

    const int vintage_crt = int{ cvar::rt_ef_vintage } == RT_VINTAGE_CRT ||
                            int{ cvar::rt_ef_vintage } == RT_VINTAGE_VHS_CRT;
    const int vintage_vhs = int{ cvar::rt_ef_vintage } == RT_VINTAGE_VHS ||
                            int{ cvar::rt_ef_vintage } == RT_VINTAGE_VHS_CRT;
    const int vintage_dither = int{ cvar::rt_ef_vintage } == RT_VINTAGE_200_DITHER ||
                               int{ cvar::rt_ef_vintage } == RT_VINTAGE_480_DITHER;

    auto ef_crt = RgPostEffectCRT{
        .isActive = vintage_crt || cvar::rt_ef_crt,
    };

    auto ef_vhs = RgPostEffectVHS{
        .isActive              = vintage_vhs || cvar::rt_ef_vhs > 0.f,
        .transitionDurationIn  = 0,
        .transitionDurationOut = 0,
        .intensity             = vintage_vhs ? 0.9f : float{ cvar::rt_ef_vhs },
    };

    auto ef_dither = RgPostEffectDither{
        .isActive              = vintage_dither || cvar::rt_ef_dither > 0.f,
        .transitionDurationIn  = 0,
        .transitionDurationOut = 0,
        .intensity             = vintage_dither ? 0.8f : float{ cvar::rt_ef_dither },
    };

    // some of the power-up effects need to be reset
    auto post_params = RgDrawFramePostEffectsParams{
        .sType                 = RG_STRUCTURE_TYPE_DRAW_FRAME_POST_EFFECTS_PARAMS,
        .pNext                 = &illum_params,
        .pWipe                 = &ef_wipe,
        .pRadialBlur           = g_resetposteffects ? nullptr : &ef_radialblur,
        .pChromaticAberration  = &ef_chrabr,
        .pInverseBlackAndWhite = g_resetposteffects ? nullptr : &ef_invbw,
        .pHueShift             = g_resetposteffects ? nullptr : &ef_hueshift,
        .pNightVision          = g_resetposteffects ? nullptr : &ef_nightvision,
        .pDistortedSides       = g_resetposteffects ? nullptr : &ef_distortedsides,
        .pColorTint            = g_resetposteffects ? nullptr : &ef_tint,
        .pCRT                  = &ef_crt,
        .pVHS                  = &ef_vhs,
        .pDither               = &ef_dither,
    };

    // DLSS-RR: flush temporal history this frame if any transient-light source
    // flagged an abrupt cut (flashlight on/off, a dynlight appearing/
    // disappearing, or a fresh level load -- see g_rt_lightcut's setters) or a
    // diagnostic cvar asked for it. Rate-limited so rapid triggers (e.g. quick
    // flashlight double-tap) don't chain resets back-to-back.
    bool wantResetHistory = bool{ cvar::rt_rr_reset_hold };

    // rt_rr_reset_debug tallies: how many flushes actually reached NGX this
    // second, and how many the rate limit swallowed. A trigger that over-fires
    // shows up as a fired count pinned at ~1000/rt_rr_reset_min_ms per second
    // with a large suppressed count behind it.
    static uint32_t s_rrFired      = 0;
    static uint32_t s_rrSuppressed = 0;
    static double   s_rrTallyAt    = 0.0;

    if( g_rt_lightcut )
    {
        g_rt_lightcut = false;
        if( curtime - g_rt_lastresetat >= double( cvar::rt_rr_reset_min_ms ) / 1000.0 )
        {
            wantResetHistory = true;
            g_rt_lastresetat = curtime;

            if( cvar::rt_rr_reset_debug )
            {
                ++s_rrFired;
                Printf( "rt_rr_reset: FLUSH (cause: %s)\n", g_rt_lightcut_why );
            }
        }
        else if( cvar::rt_rr_reset_debug )
        {
            ++s_rrSuppressed;
        }
    }

    if( cvar::rt_rr_reset_debug )
    {
        if( curtime - s_rrTallyAt >= 1.0 )
        {
            if( s_rrFired || s_rrSuppressed )
            {
                Printf( "rt_rr_reset: last second — %u flush(es), %u suppressed by "
                        "rt_rr_reset_min_ms\n",
                        s_rrFired,
                        s_rrSuppressed );
            }
            s_rrFired      = 0;
            s_rrSuppressed = 0;
            s_rrTallyAt    = curtime;
        }
    }

    if( bool{ cvar::rt_rr_reset_now } )
    {
        cvar::rt_rr_reset_now = false;
        wantResetHistory      = true;
        g_rt_lastresetat      = curtime;
    }

    auto info = RgDrawFrameInfo{
        .sType            = RG_STRUCTURE_TYPE_DRAW_FRAME_INFO,
        .pNext            = &post_params,
        .rayLength        = GetZFar() * ONEGAMEUNIT_IN_METERS,
        .presentPrevFrame = false,
        .resetHistory     = static_cast< RgBool32 >( wantResetHistory ),
        .currentTime      = curtime,
    };

    RgResult r = rt.rgDrawFrame( &info );
    RG_CHECK( r );

    if( g_cpu_latency_get )
    {
        g_cpu_latency = CalcCpuLatency();
    }

    // reset for next frame
    {
        m_wassky           = false;
        g_resetposteffects = false;
    }
}

//
//
//

bool rtx::RTRenderState::IsPerspectiveMatrix( const float* m )
{
    return std::abs( m[ 15 ] ) < std::numeric_limits< float >::epsilon();
}

bool rtx::RTRenderState::IsLikeIdentity( const float* m )
{
    auto areSimilar = []( float a, float b ) {
        return std::abs( a - b ) < 0.0000001f;
    };
    for( int a = 0; a < 4; a++ )
    {
        for( int b = 0; b < 4; b++ )
        {
            if( !areSimilar( m[ a * 4 + b ], ( a == b ? 1.0f : 0.0f ) ) )
            {
                return false;
            }
        }
    }
    return true;
}
bool rtx::RTRenderState::IsLikeIdentity( const double* m )
{
    auto areSimilar = []( double a, double b ) {
        return std::abs( a - b ) < 0.0000001;
    };
    for( int a = 0; a < 4; a++ )
    {
        for( int b = 0; b < 4; b++ )
        {
            if( !areSimilar( m[ a * 4 + b ], ( a == b ? 1.0 : 0.0 ) ) )
            {
                return false;
            }
        }
    }
    return true;
}

auto RT_MakeUpRightForwardVectors( const DRotator& rotation ) -> std::tuple< RgFloat3D, RgFloat3D, RgFloat3D >
{
    // based on HWDrawInfo::SetViewMatrix
    RgFloat3D up, right, forward;

    auto pitch = rotation.Pitch;
    // RT: invert yaw
    auto yaw  = FAngle::fromDeg( -( 270.0 - rotation.Yaw.Degrees() ) );
    auto roll = rotation.Roll;

    auto view = VSMatrix{ 1 };
    view.rotate( float( yaw.Degrees() ), 0, 0, 1 );   // around up
    view.rotate( float( pitch.Degrees() ), 1, 0, 0 ); // around right
    view.rotate( float( roll.Degrees() ), 0, 1, 0 );  // around forward
    const float* v = view.get();

    auto v100 = RgFloat3D{ -v[ 0 ], -v[ 1 ], -v[ 2 ] };
    auto v010 = RgFloat3D{ -v[ 4 ], -v[ 5 ], -v[ 6 ] };
    auto v001 = RgFloat3D{ v[ 8 ], v[ 9 ], v[ 10 ] };

    up      = v001;
    right   = v100;
    forward = v010;

    return { up, right, forward };
}

void RT_ForceCamera( const FVector3 position, const DRotator& rotation, float fovYDegrees )
{
    if( !rt.rgUploadCamera )
    {
        return;
    }

    const auto [ up, right, forward ] = RT_MakeUpRightForwardVectors( rotation );

    const float aspect = screen && screen->GetWidth() > 0 && screen->GetHeight() > 0
                             ? float( screen->GetWidth() ) / float( screen->GetHeight() )
                             : ( 16.f / 9.f );

    auto info = RgCameraInfo{
        .sType       = RG_STRUCTURE_TYPE_CAMERA_INFO,
        .pNext       = nullptr,
        .flags       = 0,
        .position    = { position[ 0 ], position[ 1 ], position[ 2 ] },
        .up          = up,
        .right       = right,
        .fovYRadians = fovYDegrees * pi::pif() / 180.0f,
        .aspect      = aspect,
        .cameraNear  = cvar::rt_znear,
        .cameraFar   = cvar::rt_zfar,
    };

    RgResult r = rt.rgUploadCamera( &info );
    assert( r == RG_RESULT_SUCCESS );
}

// The map-export predicates moved to rt_export.cpp (their public face is
// rt_helpers.h), and the title cards, fullscreen images and fluid spawner to
// rt_titles.cpp.
