// Doom64-RT: frame-cost instrumentation for the RT path. See rt_stats.h for why.

#include "rt_internal.h"
#include "rt_stats.h"

#include "c_dispatch.h"
#include "printf.h"
#include "i_time.h"
#include "hw_clock.h"

glcycle_t RTStartFrame, RTLightGen, RTFx, RTUploadPrim, RTUploadLight, RTDrawFrame;

int rt_prims_uploaded  = 0;
int rt_lights_uploaded = 0;
int rt_prims_failed    = 0;
// Read by hw_clock.cpp's checkBenchActive(), which owns glcycle_t::active. A
// plain bool rather than the cvar itself so that common/ code does not have to
// know about the RT cvar table.
bool rt_stat_force_counters = false;

int rt_prims_peak      = 0;
int rt_lights_peak     = 0;

namespace
{

// The real entry points, captured before the swap. Both may legitimately be
// null: RT_Shutdown does `rt = {}`, and a failed load leaves the struct zeroed.
PFN_rgUploadMeshPrimitive g_realUploadMeshPrimitive = nullptr;
PFN_rgUploadLight         g_realUploadLight         = nullptr;

RgResult RGAPI_PTR StatUploadMeshPrimitive( const RgMeshInfo*          pMesh,
                                            const RgMeshPrimitiveInfo* pPrimitive )
{
    if( !g_realUploadMeshPrimitive )
    {
        return RG_RESULT_NOT_INITIALIZED;
    }

    RTUploadPrim.Clock();
    RgResult r = g_realUploadMeshPrimitive( pMesh, pPrimitive );
    RTUploadPrim.Unclock();

    // Count what we SENT, not what survived: a primitive RTGL1 rejects still
    // cost us the vertex build and the call. The two are reported separately so
    // "we are uploading 6000 primitives" and "600 of them are being dropped" do
    // not look like the same number.
    rt_prims_uploaded++;
    if( r != RG_RESULT_SUCCESS )
    {
        rt_prims_failed++;
    }
    return r;
}

RgResult RGAPI_PTR StatUploadLight( const RgLightInfo* pInfo )
{
    if( !g_realUploadLight )
    {
        return RG_RESULT_NOT_INITIALIZED;
    }

    RTUploadLight.Clock();
    RgResult r = g_realUploadLight( pInfo );
    RTUploadLight.Unclock();

    rt_lights_uploaded++;
    return r;
}

} // namespace

void RT_InstallStatThunks()
{
    // Idempotent, and self-protecting against double-install: if rt already
    // holds our thunk, capturing it as "the real one" would build an infinite
    // recursion the first time anything uploaded.
    if( rt.rgUploadMeshPrimitive && rt.rgUploadMeshPrimitive != &StatUploadMeshPrimitive )
    {
        g_realUploadMeshPrimitive = rt.rgUploadMeshPrimitive;
        rt.rgUploadMeshPrimitive  = &StatUploadMeshPrimitive;
    }
    if( rt.rgUploadLight && rt.rgUploadLight != &StatUploadLight )
    {
        g_realUploadLight = rt.rgUploadLight;
        rt.rgUploadLight  = &StatUploadLight;
    }
}

void RT_StatsNewFrame()
{
    // rt_stat_every implies rt_stat_force: asking for periodic numbers and
    // getting 0.000 because the counters were gated would be the worst of both.
    rt_stat_force_counters = cvar::rt_stat_force || int{ cvar::rt_stat_every } > 0;

    rt_prims_peak  = std::max( rt_prims_peak, rt_prims_uploaded );
    rt_lights_peak = std::max( rt_lights_peak, rt_lights_uploaded );

    RTStartFrame.Reset();
    RTLightGen.Reset();
    RTFx.Reset();
    RTUploadPrim.Reset();
    RTUploadLight.Reset();
    RTDrawFrame.Reset();

    rt_prims_uploaded  = 0;
    rt_lights_uploaded = 0;
    rt_prims_failed    = 0;
}

// LIGHT_ARRAY_MAX_SIZE in RTGL1's LightManager.cpp. Not exported by the public
// header, so it is restated here purely to mark the overflow line on the stat.
// If RTGL1 ever raises it, this number being stale makes the stat pessimistic,
// never wrong in the dangerous direction.
static constexpr int RT_LIGHT_ARRAY_MAX = 4096;

namespace
{

// One formatter for both the on-screen stat and the console dump, so a
// benchmark log and the overlay can never disagree about what was measured.
FString FormatRtStats()
{
    const double startMs = RTStartFrame.TimeMS();
    const double lightMs = RTLightGen.TimeMS();
    const double lupMs   = RTUploadLight.TimeMS();
    const double fxMs    = RTFx.TimeMS();
    const double primMs  = RTUploadPrim.TimeMS();
    const double drawMs  = RTDrawFrame.TimeMS();

    // RTUploadLight is NOT added into the total: it is spent inside the
    // RTLightGen bracket, so adding it would count it twice. It is printed
    // beside lightgen so "the walks are slow" can be told apart from "handing
    // the lights to RTGL1 is slow" -- those have completely different fixes.
    FString out;
    out.Format(
        "RT: start=%2.3f lightgen=%2.3f (upload %2.3f) fx=%2.3f primupload=%2.3f "
        "drawframe=%2.3f  total=%2.3f\n"
        "prims=%d (peak %d, failed %d)  lights=%d of %d (peak %d)%s\n",
        startMs,
        lightMs,
        lupMs,
        fxMs,
        primMs,
        drawMs,
        startMs + lightMs + fxMs + primMs + drawMs,
        rt_prims_uploaded,
        rt_prims_peak,
        rt_prims_failed,
        rt_lights_uploaded,
        RT_LIGHT_ARRAY_MAX,
        rt_lights_peak,
        rt_lights_peak >= RT_LIGHT_ARRAY_MAX
            ? "  *** LIGHT ARRAY FULL - lights are being dropped ***"
            : "" );
    // The gzdoom half of the same frame, from hw_clock.cpp's counters. Without
    // it the RT numbers have no denominator: 6 ms in rgDrawFrame means one
    // thing beside a 1 ms BSP and another beside a 9 ms one. These are the same
    // globals `stat rendertimes` reads, and they are live because rt_stat_force
    // turns glcycle_t on.
    out.AppendFormat(
        "GZD: bsp=%2.3f wall=%2.3f flat=%2.3f sprite=%2.3f 2d=%2.3f  scene=%2.3f\n"
        "walls=%d flats=%d sprites=%d decals=%d portals=%d verts=%d\n",
        Bsp.TimeMS(),
        SetupWall.TimeMS() + RenderWall.TimeMS(),
        SetupFlat.TimeMS() + RenderFlat.TimeMS(),
        SetupSprite.TimeMS() + RenderSprite.TimeMS(),
        twoD.TimeMS(),
        All.TimeMS(),
        rendered_lines,
        rendered_flats,
        rendered_sprites,
        rendered_decals,
        rendered_portals,
        vertexcount );

    return out;
}

} // namespace

void RT_StatsPeriodicDump()
{
    const int every = int{ cvar::rt_stat_every };
    if( every <= 0 || !primaryLevel )
    {
        return;
    }

    // Keyed on maptime rather than a frame counter so the cadence is the same
    // whatever the frame rate -- otherwise a fast scene logs more often than a
    // slow one and the log is densest exactly where the frames are cheapest.
    static int s_lastDump = -1;
    const int  t          = primaryLevel->maptime;
    if( s_lastDump >= 0 && t - s_lastDump < every )
    {
        return;
    }
    s_lastDump = t;

    Printf( "[rt_stat t=%d] %s", t, FormatRtStats().GetChars() );
}

ADD_STAT( rt )
{
    // Sampled once a second like ADD_STAT(rendertimes), so the numbers hold
    // still long enough to read while the game is moving.
    static FString buff;
    static int64_t lasttime = 0;

    int64_t t = I_msTime();
    if( t - lasttime > 1000 )
    {
        lasttime = t;
        buff     = FormatRtStats();
    }
    return buff;
}

// The console face of the same numbers, for scripted benchmark runs where
// nothing is on screen to read. Needs rt_stat_force 1, or every phase reads
// 0.000 -- so say so rather than printing a convincing row of zeroes.
CCMD( rt_stat_dump )
{
    if( !glcycle_t::active )
    {
        Printf( "rt_stat_dump: counters are OFF -- set rt_stat_force 1 (or show `stat rt`) first. Counts below are still valid.\n" );
    }
    Printf( "%s", FormatRtStats().GetChars() );
}

// The peaks are the only sticky state, so they need a way back to zero: a spike
// from a level transition would otherwise sit on the stat line for the session
// and hide every later one.
CCMD( rt_stat_reset )
{
    rt_prims_peak  = 0;
    rt_lights_peak = 0;
    Printf( "rt stat peaks reset\n" );
}
