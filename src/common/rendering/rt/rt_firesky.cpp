// The alternative fire sky: rt_fireskies_new.
//
// WHAT IT REPLACES. Retribution's five hell maps run FRSKYNRM (MAP22/24/28) or
// FRSKYGRN (MAP23/32): a 63x128 flipbook, 100 frames at 2 tics. Two things are
// wrong with it under a path tracer and only one of them is brightness.
//
//   * 63 pixels wide. FSkyVertexBuffer::SetupMatrices sets
//     `xscale = texw < 1024 ? floorf(1024/texw) : 1`, so that art is wrapped
//     SIXTEEN times around the dome -- one copy every 22.5 degrees -- and the
//     wrap is not even seamless (column 0 vs column 62 differ by a mean of
//     12/255). Every other sky in the game is 1024 wide and gets xscale 1.
//   * All of its light is in a band at and below the horizon: measured over the
//     100 frames, the top quarter of the image carries 0.0% of the total energy
//     and the bottom quarter carries 60%. That is the one part of a sky a Doom
//     wall blocks, which is why raising rt_sky helps a courtyard and does
//     nothing for a corridor. See docs/plan-fire-skies.md.
//
// So this mode does not light the level with a picture. It is MAP12's
// arrangement moved to hell -- and MAP12 is the one map in the game that uses
// the cloud deck as a LIGHT rather than as scenery:
//
//   1. NO PAINTED SKY AT ALL. The dome goes to ALLBLACK. Two earlier passes
//      got this wrong -- first a generated ember backdrop (a second fire on
//      screen), then MOONSKY (a starfield showing through wherever the deck
//      does not reach). Black is the only one that leaves the deck as the sky.
//   2. THE CLOUD DECK, maxed exactly as MAP12 maxes it -- full alpha, all 8
//      shells, double thickness -- and tinted hard orange or green. Under total
//      cover the tint is the only colour the outdoors gets, which is why it is
//      saturated here rather than tasteful.
//   3. A MOON STRAIGHT OVERHEAD, azimuth 90 altitude 90. It is the light the
//      deck filters: rt_clouds_transmit passes a fraction of it, coloured by
//      the tint, so what reaches the level is ember light with real shadows.
//      RT_MOON_PRESETS turns the moon OFF on these maps, so this mode has to
//      turn it back on -- which is why RT_FireSkyOnLevelLoad runs after it.
//   4. EMBERS falling out of that sky, and STRIKES recoloured to match.
//
// WHAT WAS TRIED AND REMOVED. Sprite meteors, split out of
// screen/fire_meteor*.png -- five flight frames and three impact frames, arcs
// across the dome, mirrored for variety. They read as sprites, which is exactly
// what they were, and no amount of sizing fixed it. Reported 2026-08-23 and
// taken out whole rather than left behind a cvar: a dead effect with a switch
// is still a dead effect, and the art and its generator went with it.
//
// WHY THE EMBERS ARE REAL PARTICLES AND THE SKY IS NOT. The RT sky is a
// rasterised cubemap sampled on ray miss and is NOT importance-sampled, so
// anything painted into it is scenery and cannot light a room. Embers are
// therefore not painted on the dome -- they are Spark-pool particles in the
// world, traced and lit like every other one, close enough to the player to be
// resolved. That is also what lets them carry a glow at all.
//
// WHY IT IS OPT-IN. It is a different sky, not a fix to the old one, and the
// old one now works at the corrected rt_sky (175/400, RT_MOON_PRESETS). Default
// off, pinned off, and both are runnable from tools/arms/firesky-*.cfg.

#include "rt_internal.h"
#include "rt_sparks_internal.h"

#include "gamestate.h"

#include <cstdio>

using namespace rtx;
// The shared particle pool. Sky embers are Spark entries in it, so the sim,
// the collision, the draw, the distance cull and the glow budget are the ones
// rt_sparks.cpp already owns -- nothing here re-implements a particle system.
using namespace rtsp;

namespace
{

// FColorCVarRef has no assignment operator -- it is commented out in c_cvars.h
// -- so a colour cvar is written through FBaseCVar::SetGenericRep. Same helper
// as rt_presets.cpp's RT_SetColorCVar, duplicated rather than shared because
// that one is a file-local template in a .cpp and hoisting it into a header for
// two call sites is a worse trade than eight lines.
template< class TColorCVar >
void SetColorCVar( TColorCVar& c, uint32_t rgb )
{
    UCVarValue v;
    v.Int = int( rgb );
    c->SetGenericRep( v, CVAR_Int );
}

//-----------------------------------------------------------------------------
// Which maps, and in which colour.
//
// The same five rows RT_MOON_PRESETS carries, and they have to agree: that
// table is what turns the moon off on these maps, and this one turns it back
// on. Kept as a separate list rather than a flag on MoonPreset because a fire
// sky is not a property of the moon.
//-----------------------------------------------------------------------------
struct FireMap
{
    const char* map;
    bool        green;
};

constexpr FireMap RT_FIRE_MAPS[] = {
    { "map22", false }, { "map24", false }, { "map28", false },
    { "map23", true },  { "map32", true },
};

// Deck colour per family, and it is SATURATED on purpose. rt_clouds_tint is a
// multiply over near-achromatic slice art, so it owns the hue outright -- and
// under the total cover this mode runs, the tint is the only colour the whole
// outdoors gets, picture and transmitted light alike. MAP12's 0x6135A0 is the
// precedent: it went up into the neon range for exactly this reason and came
// back down only a little.
constexpr uint32_t FIRE_TINT_ORANGE = 0x6E1403;
constexpr uint32_t FIRE_TINT_GREEN  = 0x1A6600;

// The TOP of the stack: near-black smoke, so the deck goes incandescent at the
// base and dies out overhead. One tint over the whole stack is what made a hot
// colour still read as weather -- see rt_clouds_tint_top.
constexpr uint32_t FIRE_TOP_ORANGE = 0x1E0A04;
constexpr uint32_t FIRE_TOP_GREEN  = 0x081404;

// The standing underglow, and the colour a strike tints the deck with. Both
// are the fire's own colour rather than the storm's cold white.
constexpr uint32_t FIRE_GLOW_ORANGE = 0xFF6A18;
constexpr uint32_t FIRE_GLOW_GREEN  = 0x5AFF18;

// The fire BEHIND the deck -- the layer the shells silhouette against. Hot and
// saturated, because only a small fraction of it survives the cloud in front:
// measured over the references, p99.9 of the sky reaches luminance ~200 while
// the median sits at ~30.
constexpr uint32_t FIRE_BACK_ORANGE = 0xFF5210;
constexpr uint32_t FIRE_BACK_GREEN  = 0x64FF10;

// Strike colour per family. The bolt quad and the analytic directional both
// read rt_lightning_color, so setting it once keeps scenery and source agreeing
// -- which is the property the whole storm arrangement is built on.
constexpr uint32_t FIRE_BOLT_ORANGE = 0xFFB060;
constexpr uint32_t FIRE_BOLT_GREEN  = 0xA8FF60;

// The moon, straight overhead. Altitude 90 clamps theta to 0 in the direction
// code, so the light falls vertically -- through the deck, which is the point:
// a low moon would rake under the cloud instead of being filtered by it. This
// is MAP01's arrangement and MAP01's reason.
constexpr float FIRE_MOON_AZI = 90.f;
constexpr float FIRE_MOON_ALT = 90.f;

//-----------------------------------------------------------------------------
// Sky embers.
//
// The same particles as a rocket's cooling coals -- hot, additive, casting a
// real spherical glow -- but falling out of the sky instead of scattered across
// a scorch. They enter the SHARED spark pool (rt_sparks.cpp), so the sim, the
// collision, the draw, the distance cull and the glow budget are the ones that
// already exist and are already tuned. Nothing here re-implements a particle
// system.
//
// THEY SPAWN ONLY UNDER OPEN SKY. A point is sampled on a disc around the
// camera, its sector looked up, and it is rejected unless that sector's ceiling
// is skyflatnum. Cheap, exact for the case that matters, and it is the gate
// rt_dust_moon already uses for the same reason: an ember drifting through a
// sealed room came from nowhere.
//-----------------------------------------------------------------------------
// Sky embers are retired by HEIGHT, not only by age, and they have to be
// findable in the shared pool to do it. A ring of the sids we spawned is the
// whole bookkeeping: sids are monotonic and never reused, so a slot that has
// been recycled into somebody else's particle simply will not match.
constexpr int MAX_TRACKED = 256;

struct Tracked
{
    uint32_t sid  = 0;
    float    killZ = 0.f; // world Z in METRES below which it burns out
    double   nextSmoke = 0.0; // when this ember next breathes a wisp
};

Tracked g_tracked[ MAX_TRACKED ];
int     g_trackedNext = 0;

double g_next_ember  = 0.0;
double g_next_strike = 0.0;
bool   g_active      = false;
bool   g_green       = false;
double g_last_tick   = 0.0;

// Private RNG, seeded fixed. Deliberately NOT the playsim's: this is pure
// visual, it runs on the wall clock, and drawing from pr_ would desync a demo.
// Same argument rt_weather.cpp makes for RT_LightningRng.
std::mt19937& RT_FireRng()
{
    static std::mt19937 rng{ 0xF17Eu };
    return rng;
}

float RT_FireRand( float lo, float hi )
{
    return std::uniform_real_distribution< float >{ lo, hi }( RT_FireRng() );
}

// Captured once, so that turning the mode off -- or walking onto a map that is
// not a fire map -- puts the deck and the moon back where the launcher left
// them. The moon and cloud tables do the same for the same reason: these are
// CVAR_ARCHIVE, so without a restore one visit to MAP23 would leave an ember
// deck and an overhead moon on every map for the rest of the session AND in
// the ini.
struct Base
{
    bool     set      = false;
    bool     on       = false;
    uint32_t tint     = 0;
    float    alpha    = 0.f;
    float    wind     = 0.f;
    int      shells   = 0;
    float    thick    = 0.f;
    float    transmit = 0.f;
    float    dark     = 0.f;
    uint32_t bolt     = 0;
    bool     sun      = false;
    float    sun_i    = 0.f;
    float    sun_a    = 0.f;
    float    sun_b    = 0.f;
    bool     disc     = false;
    float    aim      = 0.f;
    float    horizon  = 0.f;
    float    decay    = 0.f;
    float    boltmin  = 0.f;
    float    cflash   = 0.f;
    float    lint     = 0.f;
    bool     boltadd  = false;
    float    boltgain = 0.f;
    float    boltsize = 0.f;
    uint32_t tinttop  = 0;
    int      litfrom  = 0;
    float    underlit = 0.f;
    uint32_t undercol = 0;
    float    fback    = 0.f;
    uint32_t fbackcol = 0;
    bool     vclouds  = false; // rt_clouds_volumetric
    float    vlight   = 0.f;   // rt_vclouds_light
};

Base g_base;

void CaptureBase()
{
    if( g_base.set )
    {
        return;
    }
    g_base.set      = true;
    g_base.on       = bool{ cvar::rt_clouds };
    g_base.tint     = *( cvar::rt_clouds_tint );
    g_base.alpha    = float{ cvar::rt_clouds_alpha };
    g_base.wind     = float{ cvar::rt_clouds_wind };
    g_base.shells   = int{ cvar::rt_clouds_shells };
    g_base.thick    = float{ cvar::rt_clouds_thick };
    g_base.transmit = float{ cvar::rt_clouds_transmit };
    g_base.dark     = float{ cvar::rt_clouds_dark };
    g_base.bolt     = *( cvar::rt_lightning_color );
    g_base.sun      = bool{ cvar::rt_sun };
    g_base.sun_i    = float{ cvar::rt_sun_intensity };
    g_base.sun_a    = float{ cvar::rt_sun_a };
    g_base.sun_b    = float{ cvar::rt_sun_b };
    g_base.disc     = bool{ cvar::rt_moon_geo };
    g_base.aim      = float{ cvar::rt_lightning_aim_view };
    g_base.horizon  = float{ cvar::rt_clouds_horizon };
    g_base.decay    = float{ cvar::rt_lightning_decay };
    g_base.boltmin  = float{ cvar::rt_lightning_bolt_min };
    g_base.cflash   = float{ cvar::rt_clouds_flash };
    g_base.lint     = float{ cvar::rt_lightning_intensity };
    g_base.boltadd  = bool{ cvar::rt_lightning_bolt_add };
    g_base.boltgain = float{ cvar::rt_lightning_bolt_gain };
    g_base.boltsize = float{ cvar::rt_lightning_bolt_size };
    g_base.tinttop  = *( cvar::rt_clouds_tint_top );
    g_base.litfrom  = int{ cvar::rt_clouds_litfrom };
    g_base.underlit = float{ cvar::rt_clouds_underlit };
    g_base.undercol = *( cvar::rt_clouds_underlit_color );
    g_base.fback    = float{ cvar::rt_clouds_fireback };
    g_base.fbackcol = *( cvar::rt_clouds_fireback_color );
    g_base.vclouds  = bool{ cvar::rt_clouds_volumetric };
    g_base.vlight   = float{ cvar::rt_vclouds_light };
}

void RestoreBase()
{
    if( !g_base.set )
    {
        return;
    }
    cvar::rt_clouds          = g_base.on;
    SetColorCVar( cvar::rt_clouds_tint, g_base.tint );
    cvar::rt_clouds_alpha    = g_base.alpha;
    cvar::rt_clouds_wind     = g_base.wind;
    cvar::rt_clouds_shells   = g_base.shells;
    cvar::rt_clouds_thick    = g_base.thick;
    cvar::rt_clouds_transmit = g_base.transmit;
    cvar::rt_clouds_dark     = g_base.dark;
    SetColorCVar( cvar::rt_lightning_color, g_base.bolt );
    cvar::rt_sun             = g_base.sun;
    cvar::rt_sun_intensity   = g_base.sun_i;
    cvar::rt_sun_a           = g_base.sun_a;
    cvar::rt_sun_b           = g_base.sun_b;
    cvar::rt_moon_geo        = g_base.disc;
    cvar::rt_lightning_aim_view = g_base.aim;
    cvar::rt_clouds_horizon     = g_base.horizon;
    cvar::rt_lightning_decay    = g_base.decay;
    cvar::rt_lightning_bolt_min = g_base.boltmin;
    cvar::rt_clouds_flash        = g_base.cflash;
    cvar::rt_lightning_intensity = g_base.lint;
    cvar::rt_lightning_bolt_add  = g_base.boltadd;
    cvar::rt_lightning_bolt_gain = g_base.boltgain;
    cvar::rt_lightning_bolt_size = g_base.boltsize;
    SetColorCVar( cvar::rt_clouds_tint_top, g_base.tinttop );
    cvar::rt_clouds_litfrom  = g_base.litfrom;
    cvar::rt_clouds_underlit = g_base.underlit;
    SetColorCVar( cvar::rt_clouds_underlit_color, g_base.undercol );
    cvar::rt_clouds_fireback = g_base.fback;
    SetColorCVar( cvar::rt_clouds_fireback_color, g_base.fbackcol );
    cvar::rt_clouds_volumetric = g_base.vclouds;
    cvar::rt_vclouds_light     = g_base.vlight;
}

void ScheduleNextEmber( double now )
{
    const float rate = std::max( 0.01f, float{ cvar::rt_firesky_ember_rate } );
    g_next_ember = now + RT_FireRand( 0.35f / rate, 1.65f / rate );
}

void ScheduleNextStrike( double now )
{
    const float lo = std::min( float{ cvar::rt_firesky_strike_min },
                               float{ cvar::rt_firesky_strike_max } );
    const float hi = std::max( float{ cvar::rt_firesky_strike_min },
                               float{ cvar::rt_firesky_strike_max } );
    g_next_strike = now + RT_FireRand( std::max( 0.5f, lo ), std::max( 0.6f, hi ) );
}

// One falling ember, or nothing if there is no open sky to drop it from.
void SpawnSkyEmber()
{
    if( !primaryLevel )
    {
        return;
    }

    // NOT WHILE THE LEVEL IS ON ITS WAY OUT. The schedulers run on the WALL
    // CLOCK (see RT_FireSkyTick), which is right for a look that must not
    // stutter with the playsim -- but it also means they keep firing through
    // the intermission, where there is no world to see them in and the level
    // they would cache a sector_t* from is about to be freed. That is one half
    // of the MAP22 -> MAP23 crash; the other half is fixed in rt_sparks.cpp,
    // and both are kept because a renderer effect has no business seeding
    // world-space particles outside GS_LEVEL either way.
    if( gamestate != GS_LEVEL )
    {
        return;
    }

    const auto&    vp = r_viewpoint;
    const FVector3 cam{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                        float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                        float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };

    // A few tries, then give up for this tick. Indoors every one of them fails
    // and that is the correct outcome -- there is no sky to fall from -- so
    // this has to stay cheap rather than searching until it succeeds.
    for( int attempt = 0; attempt < 6; attempt++ )
    {
        const float spawnR = std::max( 2.f, float{ cvar::rt_firesky_ember_radius } );
        const float ang = RT_FireRand( 0.f, 2.f * float( M_PI ) );
        const float rad = std::sqrt( RT_FireRand( 0.f, 1.f ) ) * spawnR;
        const float px  = cam.X + std::cos( ang ) * rad;
        const float py  = cam.Y + std::sin( ang ) * rad;

        // Map units -- PointInSector is playsim space, not renderer space.
        const double mx = double( px ) / ONEGAMEUNIT_IN_METERS;
        const double my = double( py ) / ONEGAMEUNIT_IN_METERS;

        sector_t* sec = primaryLevel->PointInSector( DVector2( mx, my ) );
        if( !sec || sec->GetTexture( sector_t::ceiling ) != skyflatnum )
        {
            continue;
        }

        Spark* slotp = AllocSpark( SparkKind::Spark );
        if( !slotp )
        {
            return;
        }

        // rt_firesky_ember_height above the eye -- ABOVE the sector's sky
        // ceiling if that is lower. The sky ceiling is map geometry, often
        // only a few metres over the courtyard, and there is nothing up there
        // to collide with: the sim skips the ceiling clamp for an ember under
        // F_SKY1 (rt_sparks.cpp), so the fall can start as high as asked.
        const float pz = cam.Z + std::max( 0.5f, float{ cvar::rt_firesky_ember_height } );

        Spark& sp = *slotp;
        sp.pos = FVector3( px, py, pz );
        // Slow, with a little lateral wander. The shared sim adds gravity at
        // rt_spark_gravity, so this is the starting drift and not the whole
        // descent -- what makes an ember read as an ember rather than as a
        // spark is that it falls SLOWLY, and rt_firesky_ember_fall is the knob.
        const float fall = std::max( 0.01f, float{ cvar::rt_firesky_ember_fall } );
        sp.vel     = FVector3( RT_FireRand( -0.35f, 0.35f ),
                               RT_FireRand( -0.35f, 0.35f ),
                               -fall * RT_FireRand( 0.6f, 1.4f ) );
        sp.age     = 0.f;
        sp.life    = std::max( 0.5f, float{ cvar::rt_firesky_ember_life } )
                   * RT_FireRand( 0.7f, 1.3f );
        sp.size    = std::max( 0.002f, float{ cvar::rt_firesky_ember_size } )
                   * RT_FireRand( 0.7f, 1.4f );
        sp.sec     = sec;
        sp.settled = false;
        sp.kind    = SparkKind::Spark;
        // Not a wall material: an ember came out of the sky, not off a
        // surface. Other is the unlabelled row, and a hot Spark does not
        // index the debris profile anyway -- only IsChunk() kinds do.
        sp.surf    = SurfKind::Other;
        sp.phase   = RT_FireRand( 0.f, 6.283f );
        sp.spin    = RT_FireRand( -2.f, 2.f );
        sp.aspect  = 1.f;
        // The ember's tint (rt_firesky_ember_color), multiplied over the
        // spark ramp in rt_spark_draw.cpp. The fire sky sets it green or
        // orange with the rest of its look; 0xFFFFFF is the untinted ramp.
        sp.baseRgb = uint32_t( *( cvar::rt_firesky_ember_color ) ) & 0xFFFFFFu;
        if( sp.baseRgb == 0u )
        {
            // 0 = match the map's fire: the same glow colours the deck uses.
            sp.baseRgb = g_green ? FIRE_GLOW_GREEN : FIRE_GLOW_ORANGE;
        }
        sp.nrm     = FVector3( 0.f, 0.f, 1.f );
        sp.sid     = NextSparkSid();

        // Remember it, so RT_FireSkyTick can burn it out before it lands. The
        // shared sim would otherwise carry it to the floor, bounce it and
        // settle it -- correct for a rocket's coals, wrong for a sky.
        // > 0: burn out that many metres above the eye, still in the air.
        // 0 (or below): no kill at all -- the shared sim carries it to the
        // floor, bounces and settles it, and it lies there glowing until its
        // life runs out. Asked for 2026-08-23: embers that land.
        const float floorOff = float{ cvar::rt_firesky_ember_floor };
        g_tracked[ g_trackedNext ] = Tracked{
            sp.sid, floorOff <= 0.f ? -1.e30f : cam.Z + floorOff, 0.0 };
        g_trackedNext = ( g_trackedNext + 1 ) % MAX_TRACKED;
        return;
    }
}

} // namespace


//-----------------------------------------------------------------------------
// Public surface.
//-----------------------------------------------------------------------------

bool RT_FireSkyActive()
{
    return g_active && bool{ cvar::rt_fireskies_new };
}

bool RT_FireSkyIsGreen()
{
    return g_green;
}

// Is the CURRENT map one of the five fire-sky maps, whether or not the mode
// is on? The global volumetric haze is cut on them (rt_main.cpp): a grey
// scattering medium under a burning sky reads as fog, not fire.
bool RT_FireSkyMap()
{
    return g_active;
}

// By NAME, for callers that run before RT_FireSkyOnLevelLoad has classified
// the new map (the preset tables do): RT_FireSkyMap() still answers for the
// previous level at that point.
bool RT_FireSkyMapName( const char* mapname )
{
    if( !mapname || !mapname[ 0 ] )
    {
        return false;
    }
    for( const auto& f : RT_FIRE_MAPS )
    {
        if( stricmp( mapname, f.map ) == 0 )
        {
            return true;
        }
    }
    return false;
}

// The dome texture this mode wants instead of the WAD's animated fire sky.
// Returned by NAME rather than as a texture so the caller owns the lookup and a
// missing texture degrades to the map's own dome instead of to a blank sky.
//
// ALLBLACK, the WAD's own black texture. Asked for as "do not put any
// background in the skybox", and the first pass got it wrong by reaching for
// MOONSKY: the deck is a bowl overhead, so the starfield read straight through
// everywhere the deck does not cover -- a night sky behind hell. Black means
// the deck and the strikes are the only things up there.
const char* RT_FireSkyDomeName()
{
    if( !g_active )
    {
        return nullptr;
    }
    // Volumetric clouds over a fire map: black underneath them whether or
    // not the full mode is on. The painted flipbook showing through the gaps
    // of a ray-marched deck is the one combination nobody wants (reported
    // 2026-08-23 on MAP23).
    if( RT_VCloudsActive() )
    {
        return "ALLBLACK";
    }
    if( !RT_FireSkyActive() || !bool{ cvar::rt_firesky_dome } )
    {
        return nullptr;
    }
    return "ALLBLACK";
}

void RT_FireSkyOnLevelLoad( const char* mapname )
{
    g_active = false;
    g_green  = false;

    // The tracked-ember ring belongs to the level that spawned them. The pool
    // itself is cleared in RT_UpdateSparks, so leaving stale sids here could
    // not match anything (sids are monotonic and never reused) -- but a ring
    // that outlives its level is a lie in a debugger and costs nothing to
    // reset.
    for( auto& t : g_tracked )
    {
        t = Tracked{};
    }
    g_trackedNext = 0;

    if( mapname && mapname[ 0 ] )
    {
        for( const auto& f : RT_FIRE_MAPS )
        {
            if( stricmp( mapname, f.map ) == 0 )
            {
                g_active = true;
                g_green  = f.green;
                break;
            }
        }
    }

    // LEAVING A FIRE MAP: put back what was captured when the mode took over,
    // then RE-RUN THIS MAP'S PRESETS on top. This site runs AFTER
    // RT_OnLevelLoadPresets (g_level.cpp, so the mode can override them on
    // its own maps), which means a plain restore here lands after the
    // presets and overwrites them: MAP19 -> MAP12 in the console kept MAP19's
    // red deck because the Base held red (captured on MAP19, and the old code
    // captured on EVERY map) and the restore clobbered MAP12's purple
    // (reported 2026-08-23). The capture is now taken only when the mode
    // activates, consumed by exactly one restore, and the presets get the
    // last word on every map that is not a fire map.
    //
    // The restore still has to happen: RT_CloudApplyPresets returns early
    // when rt_clouds_presets is 0 and writes nothing, so without it one visit
    // to MAP23 would put an ember deck on every later map -- and, since these
    // are CVAR_ARCHIVE, into the ini.
    if( !RT_FireSkyActive() )
    {
        if( g_base.set )
        {
            RestoreBase();
            g_base.set = false;
            RT_OnLevelLoadPresets( mapname );
        }
        return;
    }

    // TAKING OVER: capture the values the launcher and this map's presets
    // left, once, so the restore above can undo exactly this.
    CaptureBase();

    // THE VOLUMETRIC CLOUDS, on these maps only. rt_clouds_volumetric is
    // captured in Base and restored with the rest, so MAP12's shell deck is
    // exactly what it was on the next map; the slab's own fire terms
    // (rt_vclouds_fire/back/cascade) are gated on the fire maps inside
    // rt_vclouds.cpp as well, so they cannot leak even if this is left on.
    if( bool{ cvar::rt_firesky_vclouds } )
    {
        cvar::rt_clouds_volumetric = true;
        // The moon's light ON the cloud, the fire sky's own value: the global
        // rt_vclouds_light is tuned for a moonlit storm deck (100) and would
        // wash a burning one white.
        cvar::rt_vclouds_light = std::max( 0.f, float{ cvar::rt_firesky_vcloud_light } );
    }

    if( bool{ cvar::rt_firesky_clouds } )
    {
        // MAP12's row, moved to hell. Full alpha, all 8 shells, double
        // thickness: solid cloud with no clear patches, which is what makes the
        // deck a light source rather than scenery.
        cvar::rt_clouds        = true;
        SetColorCVar( cvar::rt_clouds_tint,
                      g_green ? FIRE_TINT_GREEN : FIRE_TINT_ORANGE );
        cvar::rt_clouds_alpha  = 1.0f;
        cvar::rt_clouds_wind   = float{ cvar::rt_firesky_cloud_wind };
        cvar::rt_clouds_shells = 8;
        cvar::rt_clouds_thick  = 1.0f;
        // What a FULLY covered patch passes, coloured by the tint. MAP12 ships
        // 0.45 against the global 0.22, for MAP12's reason: at this thickness a
        // lower value makes the deck a lid rather than a sky.
        cvar::rt_clouds_transmit = float{ cvar::rt_firesky_cloud_transmit };
        // The bottom shell is what you look at from under this, and unlike the
        // storm there is a moon directly above lighting the stack, so it is not
        // darkened as hard. 0.45 is the storm's; this sits well above it.
        cvar::rt_clouds_dark   = 0.75f;
        // How close to the horizon the deck reaches. THE STORM'S 9 DEGREES IS
        // WHY THE SKY LOOKED EMPTY: the deck is a bowl overhead, so at 9 you
        // have to look up to find any cloud and the whole band above the
        // horizon is bare dome.
        cvar::rt_clouds_horizon = std::clamp(
            float{ cvar::rt_firesky_cloud_horizon }, 2.f, 60.f );

        // THE THREE THAT TURN WEATHER INTO A BURNING SKY, and none of them is
        // the tint on its own -- which is why tint sweeps kept disappointing.
        //
        //   the GRADIENT: incandescent at the base, black smoke on top, so the
        //     deck changes hue through its depth the way fire-lit cloud does
        //     rather than only changing brightness;
        //   the RAMP INVERTED: the slice art is baked lit-from-above and the
        //     underside is what you look at, so on a fire map the shipping ramp
        //     is upside down;
        //   the UNDERGLOW: a standing additive pass weighted to the lower
        //     shells, which is what makes it read as lit BY something.
        SetColorCVar( cvar::rt_clouds_tint_top,
                      g_green ? FIRE_TOP_GREEN : FIRE_TOP_ORANGE );
        cvar::rt_clouds_litfrom  = 1;
        cvar::rt_clouds_underlit = std::max( 0.f, float{ cvar::rt_firesky_underlit } );
        SetColorCVar( cvar::rt_clouds_underlit_color,
                      g_green ? FIRE_GLOW_GREEN : FIRE_GLOW_ORANGE );

        // AND THE FIRE BEHIND IT, which is the one that matters. See
        // rt_clouds_fireback: the references are a dark sky with a thin hot
        // tail, and that comes from cloud silhouetted against fire, not from
        // cloud tinted hot. The underglow above is now a small supporting term
        // rather than the effect -- it was 0.6 and read as a flat wash.
        cvar::rt_clouds_fireback = std::max( 0.f, float{ cvar::rt_firesky_fireback } );
        SetColorCVar( cvar::rt_clouds_fireback_color,
                      g_green ? FIRE_BACK_GREEN : FIRE_BACK_ORANGE );
    }
    else
    {
        cvar::rt_clouds = g_base.on;
    }

    // The moon has to be turned back ON: RT_MOON_PRESETS sets intensity 0 and
    // disc false on all five of these maps, and this runs after it.
    if( bool{ cvar::rt_firesky_moon } )
    {
        cvar::rt_sun           = true;
        cvar::rt_sun_intensity = std::max( 0.f, float{ cvar::rt_firesky_moon_intensity } );
        cvar::rt_sun_a         = FIRE_MOON_ALT;
        cvar::rt_sun_b         = FIRE_MOON_AZI;
        // No visible disc. A cold moon hanging in a burning sky is the brightest
        // wrong thing on screen -- the one part of RT_MOON_PRESETS' call for
        // these maps worth keeping. The moon here is a LIGHT for the deck to
        // filter, not something to look at.
        cvar::rt_moon_geo      = false;
    }

    SetColorCVar( cvar::rt_lightning_color,
                  g_green ? FIRE_BOLT_GREEN : FIRE_BOLT_ORANGE );
    // Aim most strikes into view. A strike here is a scheduled EVENT rather
    // than weather, and at a uniform bearing the bolt is behind the player most
    // of the time -- reported as "bolts rarely appear, and they appear without
    // a flash from their location". See rt_lightning_aim_view.
    cvar::rt_lightning_aim_view = std::clamp( float{ cvar::rt_firesky_strike_aim }, 0.f, 1.f );
    // Longer strikes, asked for in the same report. The bolt is drawn only
    // while the envelope is above rt_lightning_bolt_min, so these two together
    // are how long one is on screen -- and the light lengthens with it, which
    // is the point: a flash with no bolt to explain it is the complaint.
    cvar::rt_lightning_decay    = std::max( 0.02f, float{ cvar::rt_firesky_strike_decay } );
    cvar::rt_lightning_bolt_min = std::clamp( float{ cvar::rt_firesky_strike_boltmin }, 0.005f, 0.9f );
    // THE BOLT WAS BEING HIDDEN BY ITS OWN FLASH. Reported 2026-08-23, and the
    // fix is mostly not about the bolt: the deck is re-drawn additively across
    // the whole sky on every strike and the directional drives the tonemapper,
    // so at the storm's values there is no headroom left for anything to be
    // brighter than the cloud behind it. Both come down here. The bolt then
    // gets the two levers that are its own -- additive blending, so it sits
    // ABOVE the sky instead of lerping toward it, and a higher gain.
    cvar::rt_clouds_flash        = std::max( 0.f, float{ cvar::rt_firesky_flash } );
    cvar::rt_lightning_intensity = std::max( 0.f, float{ cvar::rt_firesky_strike_intensity } );
    cvar::rt_lightning_bolt_add  = true;
    cvar::rt_lightning_bolt_gain = 2.6f;
    // A SMALL bolt. The pinned 55 degrees is MAP11's, where a strike is weather
    // filling half the sky; here it is a discrete event over a solid deck, and
    // at 55 the quad is so large it reads as a lit patch of cloud rather than
    // as a channel. Scoped to this mode rather than moved in the pin, because
    // 55 is a shipped look on the storm map.
    cvar::rt_lightning_bolt_size = std::clamp(
        float{ cvar::rt_firesky_bolt_size }, 5.f, 120.f );

    const double now = RT_GetCurrentTime();
    g_last_tick      = now;
    ScheduleNextEmber( now );
    ScheduleNextStrike( now );

    // ANNOUNCE UNCONDITIONALLY, not behind rt_firesky_debug. rt_presets.cpp
    // already paid for this: "the cloud and fog tables announce themselves and
    // this one did not, which makes a row impossible to confirm from a log."
    // Exactly that happened here -- a level load where this mode had been
    // silently overwritten by the SECOND preset apply (g_level.cpp's, which
    // runs later) looked identical in the log to one where it had engaged
    // correctly. RT_DiagPrintLevel keeps it off the notify overlay under
    // rt_verbose 0 while still reaching rt-console.log.
    //
    // It reads the CVARS back rather than reporting the constants it just
    // wrote, and that distinction is the whole value of the line: had it
    // printed rt_clouds instead of "clouds requested", the overwrite would have
    // been one launch to find instead of three.
    Printf( RT_DiagPrintLevel(),
            "rt_firesky: %s -> %s, dome %s, deck %s (tint %06X, %d shells, "
            "horizon %.1f), moon %.0f at az %.0f alt %.0f\n",
            mapname, g_green ? "GREEN" : "ORANGE",
            RT_FireSkyDomeName() ? RT_FireSkyDomeName() : "(map's own)",
            bool{ cvar::rt_clouds_volumetric } ? "VOLUMETRIC"
            : bool{ cvar::rt_clouds }          ? "ON"
                                               : "OFF",
            *( cvar::rt_clouds_tint ) & 0xFFFFFF,
            int{ cvar::rt_clouds_shells },
            float{ cvar::rt_clouds_horizon },
            float{ cvar::rt_sun_intensity },
            float{ cvar::rt_sun_b }, float{ cvar::rt_sun_a } );
}

// Step the two schedulers. Called once per frame from rt_main's effects block,
// NOT from the sky draw: a sealed room submits no sky portal, and scheduling
// strikes there would stop the storm exactly while the player is indoors --
// which is where a flash through a doorway is worth most.
void RT_FireSkyTick()
{
    if( !RT_FireSkyActive() )
    {
        return;
    }

    const double now = RT_GetCurrentTime();
    // A level load, a long menu pause or a save-game restore can leave an
    // arbitrary gap here. Re-base rather than firing the whole backlog at once,
    // which would drop a hundred embers on the frame the menu closes.
    if( now < g_last_tick || now - g_last_tick > 2.0 )
    {
        ScheduleNextEmber( now );
        ScheduleNextStrike( now );
    }
    g_last_tick = now;

    // Burn out any tracked ember that has fallen past its floor. Retiring by
    // `age = life` is the pool's own idiom for "gone" (rt_sparks.cpp uses it
    // when a spark hits the sky plane), so the free happens in the sim's next
    // step and nothing here touches the pool's bookkeeping.
    for( auto& t : g_tracked )
    {
        if( t.sid == 0 )
        {
            continue;
        }
        for( uint32_t i = 0; i < g_sparkCount; i++ )
        {
            Spark& sp = s_sparks[ i ];
            if( sp.sid != t.sid )
            {
                continue;
            }
            if( sp.pos.Z <= t.killZ )
            {
                sp.age = sp.life;
                t.sid  = 0;
                break;
            }

            // A THREAD OF SMOKE off each ember, while it is still hot: one
            // tiny parcel every rt_firesky_ember_smoke seconds, left where
            // the ember was so the thread is drawn by the ember's own fall
            // -- the same split the muzzle trail and the Lost Soul use. Not
            // once it has settled and cooled (the last third of its life),
            // and never past the shared ambient budget, so a sky full of
            // embers cannot spend the pool the player's smoke needs.
            const float every = float{ cvar::rt_firesky_ember_smoke };
            if( every > 0.f && sp.age < sp.life * 0.66f && now >= t.nextSmoke &&
                g_smokeAmbientCount <
                    uint32_t( std::max( 0, int{ cvar::rt_smoke_ambient_budget } ) ) )
            {
                t.nextSmoke = now + every * RT_FireRand( 0.7f, 1.3f );

                SmokeProfile p{};
                p.cls        = "firesky";
                p.count      = 1.f / std::max( 1.f, float( int{ cvar::rt_smoke_count } ) );
                p.radius     = 0.12f / std::max( 0.001f, float{ cvar::rt_smoke_radius } );
                p.density    = std::max( 0.f, float{ cvar::rt_firesky_ember_smoke_density } );
                p.life       = 0.5f;
                p.speed      = 0.f;
                p.spread     = 0.05f;
                p.rise       = 0.3f;
                p.growth     = 0.5f;
                p.trail      = 0;
                p.trailEvery = 0;
                p.note       = "ember";
                p.ambient    = true;

                RT_SpawnSmokePuffs( sp.pos, sp.pos, FVector3{ 0, 0, 1 }, FVector3{ 0, 0, 0 }, p );
            }
            break;
        }
    }

    if( bool{ cvar::rt_firesky_embers } && now >= g_next_ember )
    {
        SpawnSkyEmber();
        ScheduleNextEmber( now );
    }

    if( bool{ cvar::rt_firesky_strikes } && now >= g_next_strike )
    {
        RT_OnLightningFlash();
        ScheduleNextStrike( now );
        if( int{ cvar::rt_firesky_debug } != 0 )
        {
            Printf( "rt_firesky: scheduled strike\n" );
        }
    }
}

// `firesky` -- report state, and force events for tuning.
//
// Worth having for exactly the reason `thunder` is: the schedulers are
// deliberately unpredictable, so waiting for one in order to judge it is most
// of an evening.
CCMD( firesky )
{
    if( argv.argc() >= 2 && stricmp( argv[ 1 ], "ember" ) == 0 )
    {
        if( !RT_FireSkyActive() )
        {
            Printf( "firesky: not active here (rt_fireskies_new %d, map %s)\n",
                    int( bool{ cvar::rt_fireskies_new } ), RT_GetMapName() );
            return;
        }
        const int n = std::clamp( ( argv.argc() >= 3 ) ? atoi( argv[ 2 ] ) : 20, 1, 400 );
        for( int i = 0; i < n; i++ )
        {
            SpawnSkyEmber();
        }
        Printf( "firesky: tried to spawn %d ember(s). None appear if you are not "
                "standing under an open sky -- that gate is deliberate.\n", n );
        return;
    }
    if( argv.argc() >= 2 && stricmp( argv[ 1 ], "strike" ) == 0 )
    {
        RT_OnLightningFlash();
        Printf( "firesky: forced a strike (colour %06X)\n",
                *( cvar::rt_lightning_color ) & 0xFFFFFF );
        return;
    }

    Printf( "firesky: rt_fireskies_new %d, map %s -> %s\n",
            int( bool{ cvar::rt_fireskies_new } ), RT_GetMapName(),
            g_active ? ( g_green ? "GREEN fire map" : "ORANGE fire map" )
                     : "not a fire map" );
    Printf( "  dome %s, deck %s (tint %06X, %d shells), moon %.0f at az %.0f alt %.0f\n",
            RT_FireSkyDomeName() ? RT_FireSkyDomeName() : "(map's own)",
            bool{ cvar::rt_clouds } ? "on" : "off",
            *( cvar::rt_clouds_tint ) & 0xFFFFFF,
            int{ cvar::rt_clouds_shells },
            float{ cvar::rt_sun_intensity },
            float{ cvar::rt_sun_b }, float{ cvar::rt_sun_a } );
    Printf( "  `firesky ember [n]` drops embers, `firesky strike` forces a flash.\n" );
}
