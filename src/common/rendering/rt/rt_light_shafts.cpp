// Light shafts from ordinary lamps: which fixtures get visible air around them.
//
// THE PROBLEM. RTGL1's froxel pass scatters exactly ONE light per frame --
// LightManager::TryGetVolumetricLight picks it, and on every map here that is
// the moon. Beams of light in a dark room are the Doom 64 look, and until now
// this renderer could only produce them outdoors: every ceiling lamp, grate and
// doorway inside a level was a light with no air around it.
//
// THE THING THAT LOOKS LIKE THE ANSWER AND IS NOT. rt_fog_illum already makes
// the whole volume run an all-lights estimate, so "just turn it on everywhere"
// is the obvious move. It is wrong three times over, and each one is written up
// where it was paid for:
//
//   - It REPLACES the single-light path, and that path is the only place the
//     sun's sky-reach probe lives -- i.e. the only thing that makes the shafts
//     the game already has. Turning it on deletes them. This is exactly the
//     trap smoke hit (RtVolumetric.rgen, "TRAP 3").
//   - It shades the medium through processDirectIllumination, a SURFACE
//     integrator, with a fake normal equal to `toviewer`. A light directly
//     overhead therefore scores dot ~ 0 and is multiplied to nothing -- and a
//     light directly overhead is precisely what a ceiling lamp is. That bug is
//     why smoke was lit by the flashlight and by nothing else.
//   - It is one stochastic sample per froxel and needs a reprojected temporal
//     history to be watchable.
//
// SO THIS IS AN EXPLICIT LIST INSTEAD. The fixture walks in
// rt_lights_fixtures.cpp upload their lights exactly as they always did and
// OFFER them here; this file decides which offers are worth one shadow ray per
// froxel, and hands RTGL1 a short nearest-first list of uniqueIDs
// (RgDrawFrameLightShaftParams). The shader adds their scattering on top of the
// single-light term, so a corridor lamp and the moon are both in the air at
// once.
//
// WHY THE SELECTION IS NOT IN THE PLACEMENT WALKS. "Is this lamp bright enough
// to light the room" and "does this lamp deserve a beam" are different
// questions with different answers -- a bulb pane is ~16 correct point lights
// and exactly one worthwhile shaft -- and answering the second inside three
// unrelated loops would bury it. The offers are cheap: a struct push per light
// that already exists.
//
// See docs/plan-light-shafts.md.

#include "rt_internal.h"

using namespace rtx;

namespace
{

struct ShaftOffer
{
    uint64_t   id;
    double     x, y, z; // map units
    float      intensity;
    double     dist2; // to the camera, map units squared
    RtShaftSrc src;
};

std::vector< ShaftOffer >    g_offers;
std::vector< uint64_t >      g_selected;
std::vector< RtShaftLight >  g_selectedFull;

// The selection runs ONCE a frame and both callers get the same answer. It has
// two now -- the volumetric params and the dust -- and running it twice would
// not merely cost twice, it would append to g_selected a second time and hand
// RTGL1 a list with every id in it twice.
bool g_selectedThisFrame = false;

// Per-family offer counts, for rt_volume_shaft_verbose. Kept separately from
// g_offers.size() because the whole question this feature keeps raising is
// "which family is eating the budget", and a total cannot answer it.
int g_offered[ 3 ];

int SrcSlot( RtShaftSrc s )
{
    return s == RT_SHAFT_SRC_CEILING_INSET ? 0 : s == RT_SHAFT_SRC_CEILING_EDGE ? 1 : 2;
}

} // namespace

void RT_ShaftLightsBegin()
{
    g_offers.clear();
    g_selected.clear();
    g_selectedFull.clear();
    g_selectedThisFrame = false;
    g_offered[ 0 ] = g_offered[ 1 ] = g_offered[ 2 ] = 0;
}

void RT_ShaftLightOffer( uint64_t   id,
                         double     mapX,
                         double     mapY,
                         double     mapZ,
                         float      intensity,
                         RtShaftSrc src )
{
    if( !cvar::rt_volume_shafts )
    {
        return;
    }
    if( ( uint32_t( int{ cvar::rt_volume_shaft_src } ) & uint32_t( src ) ) == 0 )
    {
        return;
    }

    // A BLINKING LAMP IS STILL UPLOADED WHILE DARK, on purpose -- deleting the
    // light would cut ReSTIR/RR history and boil (rt_ceiling_lamp_off). A shaft
    // from a lamp that is currently off is worse than no shaft, so the offer is
    // filtered on the intensity actually being sent this frame rather than on
    // the fixture existing.
    if( intensity < float{ cvar::rt_volume_shaft_minint } )
    {
        return;
    }

    const DVector3 vpos = r_viewpoint.Pos;
    const double   dx   = mapX - vpos.X;
    const double   dy   = mapY - vpos.Y;
    const double   dz   = mapZ - vpos.Z;
    const double   d2   = dx * dx + dy * dy + dz * dz;

    // Its OWN distance cull, not rt_ceiling_edge_maxdist's 3072. That one
    // decides whether the light exists at all, and a lamp across the level
    // should still light its own room; it just has no business spending a
    // shadow ray in every froxel of the grid to do it.
    const double maxDist =
        std::max( 16.0, double( float{ cvar::rt_volume_shaft_maxdist } ) );
    if( d2 > maxDist * maxDist )
    {
        return;
    }

    g_offered[ SrcSlot( src ) ]++;
    g_offers.push_back( ShaftOffer{ id, mapX, mapY, mapZ, intensity, d2, src } );
}

const std::vector< RtShaftLight >& RT_ShaftLightsSelected()
{
    RT_ShaftLightsSelect();
    return g_selectedFull;
}

const std::vector< uint64_t >& RT_ShaftLightsSelect()
{
    if( g_selectedThisFrame )
    {
        return g_selected;
    }
    g_selectedThisFrame = true;

    if( !cvar::rt_volume_shafts || g_offers.empty() )
    {
        return g_selected;
    }

    const int maxLights =
        std::clamp( int{ cvar::rt_volume_shaft_max }, 0, int{ RG_MAX_SHAFT_LIGHTS } );
    if( maxLights <= 0 )
    {
        return g_selected;
    }

    // Nearest first. This is the order the SHADER also relies on when its ray
    // budget runs out, so it is part of the contract -- but it is NOT how the
    // slots are handed out; see the bands below.
    std::sort( g_offers.begin(), g_offers.end(), []( const ShaftOffer& a, const ShaftOffer& b ) {
        return a.dist2 < b.dist2;
    } );

    // THE DEDUPE, and without it this feature does not work at all. A Doom 64
    // lamp pane is ~16 point lights on a 16-unit lattice (rt_ceiling_bulb_spacing)
    // -- entirely correct as lighting, and as shafts it is one pane taking the
    // whole budget to produce a single blob while every other fixture in the room
    // gets nothing. Nearest wins, because the list is already sorted.
    //
    // Note this is a 3D test. The wall/ceiling walks tile in BOTH axes, and a
    // 2D gap silently merged a light with the one directly above it -- the same
    // mistake PANEL_LAMPS' min_gap made, where a vertical stack read as a
    // duplicate and the count never moved (AGENTS.md, the SMONBA traps).
    const double gap = std::max( 0.0, double( float{ cvar::rt_volume_shaft_mingap } ) );
    const double gap2 = gap * gap;

    std::vector< const ShaftOffer* > kept;
    std::vector< bool >              taken( g_offers.size(), false );
    kept.reserve( size_t( maxLights ) );

    auto l_tryTake = [ & ]( size_t i ) {
        if( taken[ i ] || int( kept.size() ) >= maxLights )
        {
            return false;
        }
        const ShaftOffer& o = g_offers[ i ];

        if( gap2 > 0.0 )
        {
            for( const ShaftOffer* k : kept )
            {
                const double dx = o.x - k->x;
                const double dy = o.y - k->y;
                const double dz = o.z - k->z;
                if( dx * dx + dy * dy + dz * dz < gap2 )
                {
                    return false;
                }
            }
        }

        taken[ i ] = true;
        kept.push_back( &o );
        return true;
    };

    // DISTANCE BANDS, and this is what "the shafts do not reach" actually was.
    //
    // Taking the nearest N with a fixed 3 m gap sounds like coverage and is not.
    // Sixteen points at 3 m spacing fill a disc of radius ~7 m around the
    // camera -- and a Doom 64 lamp room offers HUNDREDS of them (bulb lattice at
    // 16 units, faux panels every 32, the perimeter walk every 64). So every
    // slot went to the ceiling directly overhead and a lamp ten metres down the
    // corridor was never SENT AT ALL. Not dim: absent, with no shader knob able
    // to touch it. Two rounds of tuning brightness went past this because the
    // symptom -- "it works near a lamp and dies a few metres away" -- is what a
    // falloff problem looks like too.
    //
    // So the budget is split across distance bands instead: each band gets its
    // own share of the slots, and a near ceiling can no longer starve the far
    // half of the room. Leftovers roll forward, and the sweep at the end gives
    // anything still unspent back to the nearest candidates -- so an empty
    // corridor loses nothing.
    const int bands = std::clamp( int{ cvar::rt_volume_shaft_bands }, 1, 8 );
    const double maxDist =
        std::max( 16.0, double( float{ cvar::rt_volume_shaft_maxdist } ) );

    int bandKept[ 8 ] = {};

    if( bands > 1 )
    {
        for( int b = 0; b < bands; b++ )
        {
            // Slots for this band = its share, PLUS whatever earlier bands did
            // not use. Computed from what is actually in `kept` rather than
            // tracked separately, so the two cannot disagree.
            const int share  = ( maxLights * ( b + 1 ) ) / bands;
            const double lo  = maxDist * double( b ) / double( bands );
            const double hi  = maxDist * double( b + 1 ) / double( bands );
            const double lo2 = lo * lo;
            const double hi2 = hi * hi;

            for( size_t i = 0; i < g_offers.size(); i++ )
            {
                if( int( kept.size() ) >= share )
                {
                    break;
                }
                const double d2 = g_offers[ i ].dist2;
                if( d2 < lo2 || d2 >= hi2 )
                {
                    continue;
                }
                if( l_tryTake( i ) )
                {
                    bandKept[ b ]++;
                }
            }
        }
    }

    // The sweep: fill whatever the bands left over, nearest first. This is also
    // the whole algorithm when bands == 1, which is the old behaviour and what
    // tools/arms/lampshaft-noband.cfg restores.
    for( size_t i = 0; i < g_offers.size() && int( kept.size() ) < maxLights; i++ )
    {
        l_tryTake( i );
    }

    g_selected.reserve( kept.size() );
    g_selectedFull.reserve( kept.size() );
    for( const ShaftOffer* k : kept )
    {
        g_selected.push_back( k->id );
        g_selectedFull.push_back( RtShaftLight{ k->id, k->x, k->y, k->z, k->intensity } );
    }

    if( cvar::rt_volume_shaft_verbose )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            // THE FARTHEST SENT LIGHT IS THE NUMBER THAT MATTERS, and it was
            // missing from the first version of this line -- which is why two
            // rounds were spent on brightness knobs while the answer ("nothing
            // beyond seven metres is even in the list") was never printed.
            // Offered-vs-sent alone cannot show it: 16 of 300 looks like a
            // healthy cap doing its job.
            double near2 = 1.e30, far2 = 0.0;
            for( const ShaftOffer* k : kept )
            {
                near2 = std::min( near2, k->dist2 );
                far2  = std::max( far2, k->dist2 );
            }
            const double nearU = kept.empty() ? 0.0 : std::sqrt( near2 );
            const double farU  = kept.empty() ? 0.0 : std::sqrt( far2 );

            Printf( "rt_volume_shafts: sent %d of %d offered (cap %d, gap %.0fu, "
                    "within %.0fu) | reach %.0f..%.0fu = %.1f..%.1f m | "
                    "bands %d/%d/%d/%d | inset %d | edge/lattice %d | solo %d\n",
                    int( g_selected.size() ),
                    int( g_offers.size() ),
                    maxLights,
                    gap,
                    float{ cvar::rt_volume_shaft_maxdist },
                    nearU,
                    farU,
                    nearU * ONEGAMEUNIT_IN_METERS,
                    farU * ONEGAMEUNIT_IN_METERS,
                    bandKept[ 0 ],
                    bandKept[ 1 ],
                    bandKept[ 2 ],
                    bandKept[ 3 ],
                    g_offered[ 0 ],
                    g_offered[ 1 ],
                    g_offered[ 2 ] );
        }
    }

    return g_selected;
}
