// The storm, and how much of the moon the cloud deck lets through.
//
// A strike is turned into a bearing, a bolt drawn on the sky dome at that
// bearing (RT_DrawSkyQuad, hw_skyportal.cpp) and an analytic directional light
// down the same bearing -- scenery and source, so they agree by construction.
// Driven from DLightningThinker via RT_OnLightningFlash.
//
// Split out of rt_main.cpp. Behaviour unchanged; this is a move.

#include "rt_internal.h"

// The shared internals (RG_CHECK, ONEGAMEUNIT_IN_METERS, RT_SectorHue, the
// light-ID bases) come in unqualified, exactly as when this code lived inside
// rt_main.cpp's anonymous namespace.
using namespace rtx;

//-----------------------------------------------------------------------------
//
// Doom64-RT: the storm.
//
// WHERE A STRIKE COMES FROM. Not from here, and not from any script in the map.
// MAP11's MAPINFO carries the Hexen `lightning` keyword, which spawns
// DLightningThinker (playsim/mapthinkers/a_lightning.cpp). That thinker
// self-schedules -- 5-20s to the first strike, then 16-31 TICS for a quick
// double-flash (~20% of the time) or 2-9s / 5-20s otherwise -- and on each
// strike it raises every F_SKY1 sector's lightlevel to 200+(rand&31), plays
// `world/thunder` (a $random alias over DSTHNDR1/DSTHNDR2), and runs any
// SCRIPT_Lightning. MAP11's script 671 LIGHTNING is one of those, and it
// brightens the two sectors of a skybox room that RT does not render.
//
// So the timing, the sound and the sector flash are all stock and all correct.
// What is missing under RT is everything you would actually SEE: the map's
// clouds live in that ignored skybox room, and a flat lightlevel bump casts no
// shadows and comes from no direction. RT_OnLightningFlash is the thinker's one
// call into the renderer, and it turns each strike into three things that agree
// with each other by construction:
//
//   1. a bearing and altitude, picked once per strike;
//   2. a bolt drawn on the sky dome at that bearing (RT_DrawSkyQuad,
//      hw_skyportal.cpp) -- scenery;
//   3. an analytic directional light down the same bearing -- the source.
//
// (2) and (3) are the moon's arrangement repeated, and for the same reason: the
// RT sky is a rasterised cubemap sampled on ray miss, not importance-sampled,
// so painting something bright into it does not light anything at 1 spp. See
// rt_moon_geo.
//
// STROKE STRUCTURE. A strike is 1-3 sub-strokes at 0-140ms, each an instant
// attack and an exponential decay. They combine with MAX, not sum: summing
// makes a three-stroke strike three times as bright as a one-stroke strike,
// which turns rt_lightning_intensity into a knob whose meaning depends on
// rt_lightning_strokes. With max, strokes change the RHYTHM only.
//
// WALL CLOCK, not tics. The whole envelope is ~0.5s and it is a pure visual, so
// it is driven by RT_GetCurrentTime rather than by playsim time. That also
// means it decays away correctly while the game is paused instead of freezing
// mid-flash on the menu.
//
//-----------------------------------------------------------------------------
namespace
{
struct LightningStroke
{
    float at;  // seconds after the strike
    float amp; // 0..1 peak
};

struct LightningState
{
    bool            active   = false;
    double          t0       = 0.0;
    float           azimuth  = 0.f;
    float           altitude = 35.f;
    int             variant  = 0; // 0..3 -> BOLT1..BOLT4
    int             nstrokes = 0;
    LightningStroke strokes[ 6 ] = {};
};

LightningState g_lightning;

std::mt19937& RT_LightningRng()
{
    static std::mt19937 rng{ 0xB01Fu };
    return rng;
}

float RT_LightningRand( float lo, float hi )
{
    return std::uniform_real_distribution< float >{ lo, hi }( RT_LightningRng() );
}
} // namespace

// How much of the moon gets through the cloud deck -- PER CHANNEL, so cloud
// colour reaches the light and not just the picture. Written once per frame by
// RT_DrawCloudDeck (hw_skyportal.cpp), which owns the deck geometry and so is
// the only place that can answer it without a second copy of that maths. Reset
// to white whenever the deck is off or absent, so a stale value cannot darken
// or tint a map with no clouds in it.
float g_cloudSunTransmittance[ 3 ] = { 1.f, 1.f, 1.f };

// Is the directional slot carrying a lightning strike this frame rather than
// the moon? Written by RT_DrawFrame, read by RT_VCloudsParams.
bool g_rtSunIsLightning = false;

// How much deck there is per compass bearing, 0..1. Written by RT_DrawCloudDeck
// each frame (and zeroed by it when there is no deck), read by
// RT_OnLightningFlash so a strike lands in cloud rather than in a gap.
float g_cloudCoverAz[ RT_CLOUD_AZ_BINS ] = {};

// hw_skyportal.cpp writes this array and cannot include rt_internal.h, so it
// declares the bin count by hand. Two copies of a number that must agree is
// exactly the kind of thing that goes wrong silently -- a mismatched extern is
// a link-time success and a runtime buffer overrun -- so it is asserted here,
// where both are visible.
static_assert( RT_CLOUD_AZ_BINS == 36,
               "hw_skyportal.cpp hardcodes 36; change both or neither" );


// Drop a strike that is still in flight. Called on level load: a flash carrying
// into the next map would keep lighting a level that has no storm at all unless
// its own MAPINFO says so. A function rather than a direct write to g_lightning
// because that state stays private to this file.
void RT_StopLightning()
{
    g_lightning.active = false;
}

void RT_SetCloudSunTransmittance( float r, float g, float b )
{
    // A hard floor well under rt_clouds_transmit, which is the knob that is
    // actually meant to bound this. Purely a guard against a cvar combination
    // that would black out a map lit only by the moon.
    //
    // Floored on LUMINANCE and applied as a scale, not clamped per channel. A
    // per-channel clamp destroys the hue exactly when it binds: three channels
    // all under the floor come out (0.02, 0.02, 0.02), which is grey -- so the
    // deck's colour vanished from the light in precisely the case, a thick
    // overcast, where it should have been strongest. Scaling all three together
    // lifts the brightness to the floor and keeps the ratio between them, so a
    // heavy purple deck still delivers purple, just dim.
    constexpr float FLOOR = 0.02f;

    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    if( lum < FLOOR )
    {
        const float scale = FLOOR / std::max( lum, 1e-9f );
        r *= scale;
        g *= scale;
        b *= scale;
    }

    g_cloudSunTransmittance[ 0 ] = std::clamp( r, 0.f, 1.f );
    g_cloudSunTransmittance[ 1 ] = std::clamp( g, 0.f, 1.f );
    g_cloudSunTransmittance[ 2 ] = std::clamp( b, 0.f, 1.f );
}

// 0..1 flash strength right now. Cheap and side-effect-light, so both the
// renderer (hw_skyportal.cpp) and the light upload can just call it rather than
// sharing a per-frame cached value; they are microseconds apart.
float RT_LightningFlashLevel()
{
    if( !g_lightning.active || !bool{ cvar::rt_lightning } )
    {
        return 0.f;
    }

    const float tau = std::max( 0.02f, float{ cvar::rt_lightning_decay } );
    const float t   = float( RT_GetCurrentTime() - g_lightning.t0 );

    if( t < 0.f )
    {
        return 0.f;
    }
    // Four time constants past the last stroke is down to ~2% -- below anything
    // the tonemapper can show, and the point where holding a light on stops
    // being worth an upload. The afterglow (RT_LightningLightLevel) can hold
    // the strike alive longer than the flash; the window covers both.
    const float glow = std::max( 0.f, float{ cvar::rt_lightning_afterglow } );
    const float tail = std::max( 4.f * tau, 4.f * glow );
    if( t > g_lightning.strokes[ g_lightning.nstrokes - 1 ].at + tail )
    {
        g_lightning.active = false;
        return 0.f;
    }

    float e = 0.f;
    for( int i = 0; i < g_lightning.nstrokes; i++ )
    {
        const float dt = t - g_lightning.strokes[ i ].at;
        if( dt >= 0.f )
        {
            e = std::max( e, g_lightning.strokes[ i ].amp * std::exp( -dt / tau ) );
        }
    }
    return std::clamp( e, 0.f, 1.f );
}

// THE LIGHT'S ENVELOPE: the flash plus an AFTERGLOW. The lingering-light work
// (svgfIndirMaxHist, the RR disocclusion mask, g_rt_lightcut) made the
// denoiser stop smearing a strike for seconds after it -- correct, and it
// also took away the only thing that made a strike read as big. This puts a
// slow tail back ON PURPOSE, in the light itself rather than in the
// denoiser's history: rt_lightning_afterglow_level of the peak, decaying over
// rt_lightning_afterglow seconds from the last stroke.
//
// Only the DIRECTIONAL LIGHT reads this. The bolt, the deck's flash and the
// sector flash keep RT_LightningFlashLevel: a bolt that lingers at 30% for a
// second is the ghost that was taken out deliberately (rt_lightning_bolt_min).
float RT_LightningLightLevel()
{
    const float flash = RT_LightningFlashLevel();
    if( !g_lightning.active )
    {
        return flash;
    }
    const float glow = std::max( 0.f, float{ cvar::rt_lightning_afterglow } );
    const float lvl  = std::clamp( float{ cvar::rt_lightning_afterglow_level }, 0.f, 1.f );
    if( glow <= 0.f || lvl <= 0.f )
    {
        return flash;
    }
    const float t  = float( RT_GetCurrentTime() - g_lightning.t0 );
    const float dt = t - g_lightning.strokes[ g_lightning.nstrokes - 1 ].at;
    if( dt < 0.f )
    {
        return flash;
    }
    return std::max( flash, lvl * std::exp( -dt / glow ) );
}

// Where the current strike is, for whoever wants to draw or light along it.
// Returns false when nothing is happening.
bool RT_LightningAim( float* azimuth, float* altitude, int* variant )
{
    if( !g_lightning.active || !bool{ cvar::rt_lightning } )
    {
        return false;
    }
    if( azimuth )  *azimuth  = g_lightning.azimuth;
    if( altitude ) *altitude = g_lightning.altitude;
    if( variant )  *variant  = g_lightning.variant;
    return true;
}

// Does the stock thinker still get to flash sector lightlevels? See
// rt_lightning_sectorflash for why that is a question worth asking under RT.
bool RT_LightningWantsSectorFlash()
{
    return bool{ cvar::rt_lightning_sectorflash };
}

void RT_OnLightningFlash()
{
    if( !bool{ cvar::rt_lightning } )
    {
        return;
    }

    g_lightning.active   = true;
    g_lightning.t0       = RT_GetCurrentTime();
    g_lightning.variant  = int( RT_LightningRand( 0.f, 3.999f ) );

    // WHERE THE STRIKE IS. Uniform over the compass by default, which is what a
    // storm does and what MAP11 has always had -- but it also means the bolt is
    // behind the player most of the time, and the bolt is only drawn for the
    // few tenths of a second the envelope is above 0.12. The result reported
    // from play is "the level flashes and there is no lightning anywhere":
    // bolts rarely appear, and the flash appears to come from nothing.
    //
    // rt_lightning_aim_view aims a fraction of strikes into the current view
    // instead. The BEARING is the only thing this changes -- the bolt quad and
    // the analytic directional both read g_lightning.azimuth, so they agree
    // afterwards exactly as they agreed before.
    //
    // The azimuth convention is the view yaw's: RT_DrawSkyQuad builds its
    // direction as (sin t cos azi, cos t, sin t sin azi) in (doom_x, height,
    // doom_y), i.e. (cos azi, sin azi) in the map plane, which is what Doom
    // yaw already measures. No conversion, and none should be added.
    const float aimP = std::clamp( float{ cvar::rt_lightning_aim_view }, 0.f, 1.f );
    const bool  aimed = aimP > 0.f && RT_LightningRand( 0.f, 1.f ) < aimP;
    const float cone  = std::clamp( float{ cvar::rt_lightning_aim_cone }, 1.f, 180.f );
    const float yaw   = float( r_viewpoint.Angles.Yaw.Degrees() );

    auto roll = [ & ] {
        return aimed ? yaw + RT_LightningRand( -cone, cone )
                     : RT_LightningRand( 0.f, 360.f );
    };

    g_lightning.azimuth = roll();

    // AND PUT IT IN CLOUD. The deck only covers the whole sky at
    // rt_clouds_shells 6 or more -- which is what MAP12 wants and why the
    // default is 6 -- but turn the shells down and it has real gaps, and a bolt
    // drawn in one is a bolt hanging in clear air with nothing around it to
    // flash. Reported 2026-08-23.
    //
    // g_cloudCoverAz is the deck's own coverage per bearing, built by
    // RT_DrawCloudDeck from the same shell walk that answers the moon's
    // transmittance, so it cannot disagree with what was drawn. Roll a few
    // candidates and keep the best; give up rather than loop, because a sky
    // with NO deck at all (every bin 0) is a legitimate state -- MAP11 before
    // its deck existed, or rt_clouds 0 -- and a strike there should still
    // happen, just wherever it likes.
    // Guard the order rather than trusting the pair: these are two independent
    // archived cvars and nothing stops a console session from setting min above
    // max. rt_lightlevel_min/max spent a session inverted (200/1) and silently
    // defeated four A/B arms; not repeating that here.
    const float alo = std::min( float{ cvar::rt_lightning_alt_min },
                                float{ cvar::rt_lightning_alt_max } );
    const float ahi = std::max( float{ cvar::rt_lightning_alt_min },
                                float{ cvar::rt_lightning_alt_max } );
    auto rollAlt = [ & ] { return std::clamp( RT_LightningRand( alo, ahi ), -89.f, 89.f ); };
    g_lightning.altitude = rollAlt();

    const float need = std::clamp( float{ cvar::rt_lightning_need_cloud }, 0.f, 1.f );
    if( need > 0.f )
    {
        // The volumetric field can be asked about the EXACT direction; the
        // deck's table is per bearing, probed once at mid-band, so a bolt at
        // the edge of the altitude band could miss the cloud the probe saw.
        // Reported 2026-08-23 as bolts "in between clouds". Bearing and
        // altitude are therefore rolled TOGETHER here.
        const bool exact   = RT_VCloudsActive();
        auto       coverAt = [ & ]( float azDeg, float altDeg ) {
            if( exact )
            {
                return RT_VCloudsCoverAt( azDeg, altDeg );
            }
            float a = std::fmod( azDeg, 360.f );
            if( a < 0.f ) a += 360.f;
            const int bin = std::clamp( int( a * ( RT_CLOUD_AZ_BINS / 360.f ) ),
                                        0, RT_CLOUD_AZ_BINS - 1 );
            return g_cloudCoverAz[ bin ];
        };

        float best = coverAt( g_lightning.azimuth, g_lightning.altitude );
        for( int i = 0; i < 16 && best < need; i++ )
        {
            const float cand  = roll();
            const float calt  = rollAlt();
            const float c     = coverAt( cand, calt );
            if( c > best )
            {
                best                 = c;
                g_lightning.azimuth  = cand;
                g_lightning.altitude = calt;
            }
        }
        if( int{ cvar::rt_lightning_debug } )
        {
            Printf( "rt_lightning: bearing %.0f alt %.0f, %s coverage %.2f (want %.2f)\n",
                    g_lightning.azimuth, g_lightning.altitude,
                    exact ? "field" : "deck", best, need );
        }
    }

    const int maxstrokes = std::clamp( int{ cvar::rt_lightning_strokes }, 1, 6 );
    const int n          = 1 + int( RT_LightningRand( 0.f, float( maxstrokes ) - 0.001f ) );
    g_lightning.nstrokes = n;

    float at = 0.f;
    for( int i = 0; i < n; i++ )
    {
        g_lightning.strokes[ i ].at = at;
        // First stroke is always full; later ones are weaker, which is both
        // what real return strokes do and what keeps the burst from reading as
        // three separate strikes.
        g_lightning.strokes[ i ].amp = ( i == 0 ) ? 1.f : RT_LightningRand( 0.35f, 0.8f );
        at += RT_LightningRand( 0.04f, 0.14f );
    }

    // DLSS-RR: a strike is the largest transient light in the game by an order
    // of magnitude. Flush temporal history or the flash smears for several
    // frames after it is over.
    g_rt_lightcut     = true;
    g_rt_lightcut_why = "lightning";

    if( int{ cvar::rt_lightning_debug } )
    {
        Printf( "rt_lightning: STRIKE az %.0f alt %.0f BOLT%d, %d stroke(s) at",
                g_lightning.azimuth, g_lightning.altitude,
                g_lightning.variant + 1, n );
        for( int i = 0; i < n; i++ )
        {
            Printf( " %.0fms(%.2f)", g_lightning.strokes[ i ].at * 1000.f,
                    g_lightning.strokes[ i ].amp );
        }
        Printf( "\n" );
    }
}



// Force a strike from the console: `thunder`.
//
// Worth having because the real trigger is deliberately unpredictable -- the
// storm thinker waits 5-20 seconds for its first strike and then 2-20 for
// each of the rest, so tuning rt_lightning_* by relaunching and waiting is
// most of an evening. This drives the same RT_OnLightningFlash the thinker
// calls, so it exercises the exact path, and it works on any map: the strike
// is renderer-side, so a map with no `lightning` in its MAPINFO can still be
// used to look at one. It does NOT play the thunder sound -- that is
// S_Sound in the thinker, not here.
CCMD( thunder )
{
    if( !bool{ cvar::rt_lightning } )
    {
        Printf( "thunder: rt_lightning is 0, nothing will happen\n" );
        return;
    }
    RT_OnLightningFlash();
    Printf( "thunder: forced a strike (no sound -- that is the storm thinker's)\n" );
}
