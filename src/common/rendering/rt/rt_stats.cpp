// Doom64-RT: frame-cost instrumentation for the RT path. See rt_stats.h for why.

#include "rt_internal.h"
#include "rt_stats.h"

#include "c_dispatch.h"
#include "printf.h"
#include "i_time.h"
#include "hw_clock.h"
#include "r_utility.h" // r_viewpoint, so a spike line says where to walk back to

glcycle_t RTStartFrame, RTLightGen, RTFx, RTUploadPrim, RTUploadLight, RTDrawFrame;
glcycle_t RTPlaysim;
glcycle_t RTDisplay;
double    rt_display_ms = 0.0;
glcycle_t RTTexUpload;
int       rt_tex_uploaded = 0;
double    rt_tex_slowest_ms = 0.0;
char      rt_tex_slowest_name[ 64 ] = {};

int rt_prims_uploaded  = 0;
int rt_lights_uploaded = 0;
int rt_prims_failed    = 0;
// Read by hw_clock.cpp's checkBenchActive(), which owns glcycle_t::active. A
// plain bool rather than the cvar itself so that common/ code does not have to
// know about the RT cvar table.
bool rt_stat_force_counters = false;

// End-to-end frame time, which none of the phase counters measure: they are CPU
// wall-clock inside four brackets, and the frame also contains the playsim, the
// 2D pass, audio, and whatever the driver does between our last submit and our
// next BeginFrame. Reporting a phase total as if it were a frame time is how a
// "3.5 ms saved" turns into a claimed FPS that never materialises.
//
// Averaged over the interval between reports rather than sampled per frame: at
// 300 fps a single frame is ~3 ms and I_nsTime's jitter is a real fraction of
// that, while frames/elapsed over a second is solid.
static uint64_t s_fpsWindowStartNs = 0;
static int      s_fpsFrames        = 0;
static double   s_lastFps          = 0.0;
static double   s_lastFrameMs      = 0.0;

// THE OUTLIER, beside the average -- because the average above is blind to the
// only thing a stutter is. s_lastFrameMs is frames/elapsed over half a second,
// so a single 24 ms frame inside that window shifts it by ~0.02 ms and a run
// taken to explain two felt hitches reads as flat as one that had none. That
// happened on MAP02 on 2026-08-19 and cost an afternoon of reading phase
// columns that were never going to show it.
//
// Worst-frame and over-threshold are kept for the SAME half-second window as
// the average and roll over with it, so the FRAME line describes one interval
// throughout rather than mixing an average of one window with a peak of all
// time (the mistake rt_prims_peak makes on purpose, and which is why that one
// needs rt_stat_reset).
static uint64_t s_prevFrameNs   = 0;
static double   s_frameWorstMs  = 0.0;
static int      s_frameOverCnt  = 0;
static double   s_lastWorstMs   = 0.0;
static int      s_lastOverCnt   = 0;

// The playsim of the last frame that actually RAN a tic. Sampling RTPlaysim on
// whatever frame the stat happens to land on reads 0.000 about two times in
// three at 115 fps -- a tic runs 35x/sec and most frames simply carry none. A
// stat that reports "the playsim costs nothing" two thirds of the time is worse
// than no stat, so the periodic line reports the last real one. The SPIKE line
// keeps the raw per-frame value, because there the frame in question is by
// definition the one that was slow.
static double   s_lastPlaysimMs = 0.0;

// Outlier-resistant estimate of what a frame normally costs right now. See the
// note at its update site for why the plain average could not be used.
static double   s_typicalMs     = 0.0;

int rt_prims_peak      = 0;
int rt_lights_peak     = 0;

namespace
{

// The real entry points, captured before the swap. Both may legitimately be
// null: RT_Shutdown does `rt = {}`, and a failed load leaves the struct zeroed.
PFN_rgUploadMeshPrimitive g_realUploadMeshPrimitive = nullptr;
PFN_rgUploadLight         g_realUploadLight         = nullptr;
PFN_rgProvideOriginalTexture g_realProvideOriginalTexture = nullptr;

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

RgResult RGAPI_PTR StatProvideOriginalTexture( const RgOriginalTextureInfo* pInfo )
{
    if( !g_realProvideOriginalTexture )
    {
        return RG_RESULT_NOT_INITIALIZED;
    }

    // Timed individually as well as summed, because "textures=1 texupload=60ms"
    // and "textures=60 texupload=60ms" are completely different problems and the
    // sum cannot tell them apart. I_nsTime rather than glcycle_t: this needs to
    // survive being read per call, and there are only a handful per frame once
    // the level is running.
    const uint64_t t0 = I_nsTime();

    RTTexUpload.Clock();
    RgResult r = g_realProvideOriginalTexture( pInfo );
    RTTexUpload.Unclock();

    const double ms = double( I_nsTime() - t0 ) / 1e6;
    if( ms > rt_tex_slowest_ms )
    {
        rt_tex_slowest_ms = ms;

        const char* n = pInfo && pInfo->pTextureName ? pInfo->pTextureName : "(unnamed)";
        std::snprintf( rt_tex_slowest_name, sizeof( rt_tex_slowest_name ), "%s", n );
    }

    rt_tex_uploaded++;
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
    if( rt.rgProvideOriginalTexture &&
        rt.rgProvideOriginalTexture != &StatProvideOriginalTexture )
    {
        g_realProvideOriginalTexture = rt.rgProvideOriginalTexture;
        rt.rgProvideOriginalTexture  = &StatProvideOriginalTexture;
    }
}

// One line per outlier frame. Called from RT_StatsNewFrame BEFORE the phase
// timers and counters are reset, so everything it prints belongs to the frame
// that just ended -- the one that was actually slow. Printing it after the
// resets would report the spike with the next frame's (zeroed) numbers, which
// is the kind of off-by-one-frame that makes an instrument lie confidently.
static void RT_ReportSpike( double frameMs, double thresholdMs )
{
    const DVector3 pos = r_viewpoint.Pos;

    // THE SUBSECTOR ARRAY MUST BE NON-EMPTY, and a null check on the result is
    // not enough. PointInSector is `PointInSubsector(x,y)->sector`
    // (g_levellocals.h), and PointInSubsector returns `&subsectors[0]` whenever
    // there is no BSP head node (p_maputl.cpp:1977). On an empty TArray that
    // address is null, so the `->sector` read faults before this function ever
    // sees a pointer to test.
    //
    // This is the normal path, not an edge case: startup frames run to hundreds
    // of milliseconds and trip any threshold, so the first spike is reported
    // long before a level exists. It crashed the release install on 2026-08-20
    // -- and every test run had survived only because it passed +map, which is
    // precisely the flag that skips the menu boot the players actually use.
    int secnum = -1;
    if( primaryLevel && primaryLevel->subsectors.Size() > 0 )
    {
        if( const sector_t* sec = primaryLevel->PointInSector( pos.X, pos.Y ) )
        {
            secnum = sec->sectornum;
        }
    }

    // RT_DiagPrintLevel: console buffer and logfile, but NOT the on-screen
    // notify overlay unless rt_verbose is on. The whole point of a spike log is
    // that it can be left running during normal play -- an instrument that
    // paints text over the game every time it fires is one you turn off, and
    // then it is not measuring the session you actually wanted measured.
    Printf( RT_DiagPrintLevel(),
            "[rt_spike t=%d] frame=%.2f ms (over %.1f)  start=%.3f lightgen=%.3f fx=%.3f "
            "primupload=%.3f drawframe=%.3f playsim=%.3f display=%.3f texupload=%.3f\n"
            "            prims=%d (failed %d)  lights=%d  textures=%d (slowest %.1f ms: %s)  bsp=%.3f wall=%.3f flat=%.3f sprite=%.3f scene=%.3f  pos=(%.0f,%.0f,%.0f) sector=%d\n",
            primaryLevel ? primaryLevel->maptime : -1,
            frameMs,
            thresholdMs,
            RTStartFrame.TimeMS(),
            RTLightGen.TimeMS(),
            RTFx.TimeMS(),
            RTUploadPrim.TimeMS(),
            RTDrawFrame.TimeMS(),
            RTPlaysim.TimeMS(),
            rt_display_ms,
            RTTexUpload.TimeMS(),
            rt_prims_uploaded,
            rt_prims_failed,
            rt_lights_uploaded,
            rt_tex_uploaded,
            rt_tex_slowest_ms,
            rt_tex_slowest_name[ 0 ] ? rt_tex_slowest_name : "-",
            Bsp.TimeMS(),
            SetupWall.TimeMS() + RenderWall.TimeMS(),
            SetupFlat.TimeMS() + RenderFlat.TimeMS(),
            SetupSprite.TimeMS() + RenderSprite.TimeMS(),
            All.TimeMS(),
            pos.X,
            pos.Y,
            pos.Z,
            secnum );
}

void RT_StatsNewFrame()
{
    const uint64_t now = I_nsTime();

    {
        if( s_fpsWindowStartNs == 0 )
        {
            s_fpsWindowStartNs = now;
            s_fpsFrames        = 0;
        }
        s_fpsFrames++;

        const uint64_t elapsed = now - s_fpsWindowStartNs;
        if( elapsed >= 500000000ull ) // half a second
        {
            s_lastFps          = double( s_fpsFrames ) * 1e9 / double( elapsed );
            s_lastFrameMs      = double( elapsed ) / 1e6 / double( s_fpsFrames );
            s_fpsWindowStartNs = now;
            s_fpsFrames        = 0;

            // Roll the outlier over with the average it sits beside, so the two
            // always describe the same interval.
            s_lastWorstMs  = s_frameWorstMs;
            s_lastOverCnt  = s_frameOverCnt;
            s_frameWorstMs = 0.0;
            s_frameOverCnt = 0;
        }
    }

    // rt_stat_every implies rt_stat_force: asking for periodic numbers and
    // getting 0.000 because the counters were gated would be the worst of both.
    // rt_stat_spike likewise -- a spike line whose phase split is all zeroes
    // tells you a frame was slow and nothing about where it went.
    rt_stat_force_counters = cvar::rt_stat_force || int{ cvar::rt_stat_every } > 0 ||
                             float{ cvar::rt_stat_spike } > 0.0f ||
                             float{ cvar::rt_stat_spike_rel } > 0.0f;

    // The duration of the frame that just ENDED. s_prevFrameNs == 0 is the
    // first frame ever, which has no predecessor to measure and would otherwise
    // report the whole of startup as one enormous spike.
    if( s_prevFrameNs != 0 )
    {
        const double frameMs = double( now - s_prevFrameNs ) / 1e6;

        s_frameWorstMs = std::max( s_frameWorstMs, frameMs );

        // THE BASELINE MUST IGNORE ITS OWN OUTLIERS. The half-second average is
        // the wrong thing to scale a threshold from: it includes the spikes. A
        // menu left open for 105 seconds is ONE frame of 105 seconds, and it
        // dragged the average up hard enough to put the threshold at 7700 ms --
        // after which the recorder was blind to every real stutter until the
        // average decayed back, which is exactly the window a player would have
        // been complaining about (2026-08-20, MAP01 session).
        //
        // So the threshold scales from a typical-frame estimate instead: an EMA
        // that simply refuses any frame more than 4x itself. A pause, a level
        // load and a disk hitch all fail that test and leave it untouched, while
        // an honest change of pace (a new map, a resolution change) walks it to
        // the new value within a second or so.
        if( s_typicalMs <= 0.0 )
        {
            s_typicalMs = frameMs;
        }
        else if( frameMs <= 4.0 * s_typicalMs )
        {
            s_typicalMs += 0.05 * ( frameMs - s_typicalMs );
        }

        // Relative wins when set, with the absolute value as a floor.
        double threshold = double{ float{ cvar::rt_stat_spike } };

        const double rel = double{ float{ cvar::rt_stat_spike_rel } };
        if( rel > 0.0 && s_typicalMs > 0.0 )
        {
            threshold = std::max( threshold, rel * s_typicalMs );
        }
        if( threshold > 0.0 && frameMs > threshold )
        {
            s_frameOverCnt++;
            RT_ReportSpike( frameMs, threshold );
        }
    }
    s_prevFrameNs = now;

    if( RTPlaysim.TimeMS() > 0.0 )
    {
        s_lastPlaysimMs = RTPlaysim.TimeMS();
    }

    rt_prims_peak  = std::max( rt_prims_peak, rt_prims_uploaded );
    rt_lights_peak = std::max( rt_lights_peak, rt_lights_uploaded );

    RTStartFrame.Reset();
    RTLightGen.Reset();
    RTFx.Reset();
    RTUploadPrim.Reset();
    RTUploadLight.Reset();
    RTDrawFrame.Reset();
    RTPlaysim.Reset();
    RTTexUpload.Reset();

    rt_tex_uploaded    = 0;
    rt_tex_slowest_ms  = 0.0;
    rt_tex_slowest_name[ 0 ] = 0;
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
    // The outlier belongs on the same line as the average, because reading the
    // average alone is what made a two-hitch run look flat. "worst" is the
    // longest single frame of the same half-second window; the over-count only
    // appears once rt_stat_spike gives it a threshold to count against.
    const float spike = float{ cvar::rt_stat_spike };

    FString outlier;
    outlier.Format( "  worst %2.3f ms", s_lastWorstMs );
    if( spike > 0.0f )
    {
        outlier.AppendFormat( ", %d over %.1f", s_lastOverCnt, spike );
    }

    FString out;
    out.Format(
        "FRAME: %2.3f ms avg (%.1f fps)%s  [half-second window]\n"
        "RT: start=%2.3f lightgen=%2.3f (upload %2.3f) fx=%2.3f primupload=%2.3f "
        "drawframe=%2.3f  total=%2.3f\n"
        "PLAYSIM: %2.3f ms on the last frame that ran a tic  (NOT a render phase; "
        "a tic runs 35x/sec, so most frames carry none)\n"
        "prims=%d (peak %d, failed %d)  lights=%d of %d (peak %d)%s\n",
        s_lastFrameMs,
        s_lastFps,
        outlier.GetChars(),
        startMs,
        lightMs,
        lupMs,
        fxMs,
        primMs,
        drawMs,
        startMs + lightMs + fxMs + primMs + drawMs,
        s_lastPlaysimMs,
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

    Printf( RT_DiagPrintLevel(), "[rt_stat t=%d] %s", t, FormatRtStats().GetChars() );
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
