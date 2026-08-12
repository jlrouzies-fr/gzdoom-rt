// Sector-derived lights: the ones that come from the map's own sector records
// rather than from a texture or an actor.
//
//   RT_MakeLightstyles            - per-sector lightlevel snapshot, so an RTGL1
//                                   lightstyle can animate a light without us
//                                   re-uploading it every frame
//   RT_UploadExportableSectorLights - one light per sector (or only per sector
//                                   with a light-changing special)
//   RT_UploadGzDoomDynamicLights  - forwards FDynamicLight (9800/9802 things,
//                                   GLDEFS attached lights) into RTGL1
//   RT_WatchLightlevels           - periodic lightlevel report for a sector list
//   RT_UpdateSectorEmisThreshold  - derives rt_sector_emis's cutoff from THIS
//                                   map's lightlevel distribution
//
// Split out of rt_main.cpp. Behaviour unchanged; this is a move.

#include "rt_internal.h"

// The shared internals (RG_CHECK, ONEGAMEUNIT_IN_METERS, RT_SectorHue, the
// light-ID bases) come in unqualified, exactly as when this code lived inside
// rt_main.cpp's anonymous namespace.
using namespace rtx;

std::vector< uint8_t > g_sectorlightlevels = {};

void RT_MakeLightstyles()
{
    if( !primaryLevel || primaryLevel->sectors.Size() == 0 )
    {
        g_sectorlightlevels.clear();
        return;
    }
    g_sectorlightlevels.resize( primaryLevel->sectors.Size() );

    for( uint32_t i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        g_sectorlightlevels[ i ] = uint8_t( std::clamp( //
            primaryLevel->sectors[ i ].GetLightLevel(),
            0,
            255 ) );
    }
}

static bool RT_IsSectorLightChangingSpecial( int special )
{
    return ( special == Light_Phased ) || ( special == LightSequenceStart ) ||
           ( special == LightSequenceSpecial1 ) || ( special == LightSequenceSpecial2 ) ||
           ( special == dLight_Flicker ) || ( special == dLight_StrobeFast ) ||
           ( special == dLight_StrobeSlow ) || ( special == dLight_Strobe_Hurt ) ||
           ( special == dLight_Glow ) || ( special == dLight_StrobeSlowSync ) ||
           ( special == dLight_StrobeFastSync ) || ( special == dLight_FireFlicker ) ||
           ( special == sLight_Strobe_Hurt ) || ( special == Light_OutdoorLightning ) ||
           ( special == Light_IndoorLightning1 ) || ( special == Light_IndoorLightning2 );
}

void RT_UploadExportableSectorLights()
{
    // Stock path uploads a white sphere at every sector center. On Retribution that
    // was a lingering wash. Two modes:
    //   rt_sector_lights  = all sectors (stock / Doom II export)
    //   rt_sector_flicker = only sectors with light-changing specials (blink without wash)
    //
    // MAP01 note: spawn booth ceilings (SFLATAS) have special 0 / steady lightlevel —
    // sector lights do NOT blink them. The wall SMON alcoves are dLight_Flicker (65)
    // and DO blink with this path — that is the wrong target for "head lights".
    const bool allSectors   = bool{ cvar::rt_sector_lights };
    const bool flickerOnly  = bool{ cvar::rt_sector_flicker };
    if( !allSectors && !flickerOnly )
    {
        return;
    }
    if( !primaryLevel )
    {
        return;
    }
    // BeginFrame normally fills this; rebuild if DrawFrame somehow races ahead.
    if( g_sectorlightlevels.size() != primaryLevel->sectors.Size() )
    {
        RT_MakeLightstyles();
    }
    if( g_sectorlightlevels.size() != primaryLevel->sectors.Size() )
    {
        return;
    }

    assert( g_sectorlightlevels.size() == primaryLevel->sectors.Size() );

    for( uint32_t i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        const sector_t& sector = primaryLevel->sectors[ i ];

        if( !allSectors && !RT_IsSectorLightChangingSpecial( sector.special ) )
        {
            continue;
        }

        float z;
        {
            auto zfloor   = float( sector.floorplane.ZatPoint( sector.centerspot ) );
            auto zceiling = float( sector.ceilingplane.ZatPoint( sector.centerspot ) );

            // if too thin
            if( std::abs( zfloor - zceiling ) < 0.1f )
            {
                if( !RT_IsSectorLightChangingSpecial( sector.special ) )
                {
                    continue;
                }
            }

            z = ( zfloor + zceiling ) / 2;
        }

        // Flicker-only path uses a milder intensity than stock autoexport (200)
        // so we get blink + cast without room-wide wash.
        const float intensity =
            allSectors ? float{ cvar::rt_autoexport_light }
                       : std::min( 80.f, float{ cvar::rt_autoexport_light } * 0.35f );

        const auto center = FVector3{
            float( sector.centerspot.X ),
            float( sector.centerspot.Y ),
            z,
        };

        auto adt = RgLightAdditionalEXT{
            .sType      = RG_STRUCTURE_TYPE_LIGHT_ADDITIONAL_EXT,
            .pNext      = nullptr,
            .flags      = RG_LIGHT_ADDITIONAL_LIGHTSTYLE,
            .lightstyle = int( i ), // references g_sectorlightlevels
            .hashName   = "",
        };

        // Was RG_PACKED_COLOR_WHITE — that is the "fake white wash" this cvar's own
        // description warns about. A Doom 64 sector's light IS its colormap color, so
        // carry the hue: a red corridor gets a red sector light, not a white one.
        const FVector3 hue =
            RT_SectorHue( sector.Colormap.LightColor, float{ cvar::rt_sector_tint_lights } );

        auto lsph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = &adt,
            .color     = rt.rgUtilPackColorFloat4D( hue.X, hue.Y, hue.Z, 1.0f ),
            .intensity = intensity,
            .position  = { center.X * ONEGAMEUNIT_IN_METERS,
                           center.Y * ONEGAMEUNIT_IN_METERS,
                           center.Z * ONEGAMEUNIT_IN_METERS },
            .radius    = 0.05f,
        };

        auto linfo = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &lsph,
            .uniqueID     = SectorLightId_Base + i,
            .isExportable = allSectors, // flicker-only lights are runtime-only
        };

        RgResult r = rt.rgUploadLight( &linfo );
        RG_CHECK( r );
    }
}

void RT_UploadGzDoomDynamicLights()
{
    // DLSS-RR: previous frame's set of uploaded light IDs, for the
    // appear/disappear diff at the bottom of this function.
    static std::unordered_set< uint64_t > s_prevDynIds;

    // Stock gzdoom-rt never forwarded FDynamicLight (map things 9800/9802, GLDEFS
    // attached lights) into RTGL. Retribution MAP01 spawn blink lamps are
    // PointLightFlicker (9802) beside SMONAA — without this they are invisible in PT.
    //
    // Note: deliberately NOT bailing out when primaryLevel->lights is null (list
    // went empty) — the diff below still needs to run to catch "last light
    // just disappeared"; the loops below simply do zero iterations in that case.
    if( !cvar::rt_dynlight || !primaryLevel )
    {
        s_prevDynIds.clear();
        return;
    }

    std::unordered_set< uint64_t > curDynIds;

    const float intensityScale = std::max( 0.f, float{ cvar::rt_dynlight_intensity } );
    const float intensityMax   = std::max( 0.f, float{ cvar::rt_dynlight_max } );
    const float srcRadius      = std::max( 0.01f, float{ cvar::rt_dynlight_radius } );
    const bool  stackAtten     = bool{ cvar::rt_dynlight_stack_atten };

    // Doom64 key doors place 3 PointLights on the same XY at different heights so the
    // classic HW path lights a tall jamb strip. In PT those spheres add, so bloom goes
    // nuclear-white. Count co-located XY first, then divide each upload by the stack size.
    auto xyKey = []( double x, double y ) -> uint64_t {
        const int qx = int( std::lround( x / 4.0 ) );
        const int qy = int( std::lround( y / 4.0 ) );
        return ( uint64_t( uint32_t( qx ) ) << 32 ) | uint32_t( qy );
    };

    // Also count when only debugging: the histogram below is how you tell whether
    // stack attenuation is doing anything at all.
    std::unordered_map< uint64_t, int > stackCount;
    if( stackAtten || bool{ cvar::rt_dynlight_debug } )
    {
        for( FDynamicLight* light = primaryLevel->lights; light != nullptr; light = light->next )
        {
            if( !light->IsActive() || light->IsSubtractive() || light->DontLightMap() )
            {
                continue;
            }
            if( light->X() < -1.0e6 )
            {
                continue;
            }
            if( !cvar::rt_dynlight_flicker &&
                ( light->lighttype == FlickerLight || light->lighttype == RandomFlickerLight ) )
            {
                continue;
            }
            if( light->m_currentRadius <= 0.01f )
            {
                continue;
            }
            if( light->m_currentRadius < float{ cvar::rt_dynlight_minradius } )
            {
                continue;
            }
            if( light->GetRed() + light->GetGreen() + light->GetBlue() <= 0 )
            {
                continue;
            }
            stackCount[ xyKey( light->X(), light->Y() ) ]++;
        }
    }

    uint32_t index = 0;
    for( FDynamicLight* light = primaryLevel->lights; light != nullptr; light = light->next )
    {
        if( !light->IsActive() || light->IsSubtractive() || light->DontLightMap() )
        {
            continue;
        }

        // Skip uninitialized lights (GetLight seeds Pos.X = -1e7 until UpdateLocation).
        if( light->X() < -1.0e6 )
        {
            continue;
        }

        // The wall SMON monitors are PointLightFlicker (9802) — 199 of the 205 in the
        // game. This gate therefore decides whether Retribution's animated wall panels
        // cast light at all, and with the old `false` default they did not: they showed
        // their _e emissive glow, which illuminates nothing, and read as animated but
        // dead. See rt_dynlight_flicker for how that was found (2026-08-11).
        if( !cvar::rt_dynlight_flicker &&
            ( light->lighttype == FlickerLight || light->lighttype == RandomFlickerLight ) )
        {
            continue;
        }

        // Stable ID across frames (index shifts when other lights activate/deactivate
        // and breaks ReSTIR temporal matching — flicker gets smoothed away).
        const uint64_t stableId =
            DynLightId_Base + ( uint64_t{ reinterpret_cast< uintptr_t >( light ) } & 0xFFFFFFFFull );

        // DLSS-RR: track which lights are present this frame so a newly appeared/
        // disappeared one (barrel/rocket explosion flash, pickup glow, etc.) can
        // flush RR history below.
        //
        // Recorded HERE, before the brightness cutoffs below, deliberately: a pulse
        // light whose m_currentRadius (or scaled intensity) dips under 0.01 for a few
        // tics is still the same light, and must not read as disappear-then-reappear.
        // Doing this after those cutoffs made steady flicker/pulse lights churn the
        // set — membership changing while the *count* stayed flat, so the
        // rt_dynlight_debug count check never caught it — and fired a history flush
        // almost every frame, i.e. permanent RR noise. Presence here means "this
        // FDynamicLight exists and is active", which is what actually maps to the
        // scene-lighting cut we care about.
        curDynIds.insert( stableId );

        // GZDoom stores intensity as light radius in map units; flicker/pulse update
        // m_currentRadius each tic. MAP01 9802 uses 24/20 — only ~17% HW delta, invisible
        // under RR. Remap [lo,hi] → [0.15,1.0] * peak so blink reads as on/off.
        const float mapRadius = light->m_currentRadius;
        if( mapRadius <= 0.01f )
        {
            continue;
        }
        // Below the fixture threshold: a raster-era helper light, not a real source.
        // Filtered after curDynIds.insert above on purpose, so skipping it does not
        // register as a light appearing/disappearing and flush RR temporal history.
        if( mapRadius < float{ cvar::rt_dynlight_minradius } )
        {
            continue;
        }

        const float lo = float( std::min( light->GetIntensity(), light->GetSecondaryIntensity() ) );
        const float hi = float( std::max( light->GetIntensity(), light->GetSecondaryIntensity() ) );
        float       blink = 1.f;
        if( hi > lo + 0.5f &&
            ( light->lighttype == FlickerLight || light->lighttype == RandomFlickerLight ||
              light->lighttype == PulseLight ) )
        {
            const float t     = std::clamp( ( mapRadius - lo ) / ( hi - lo ), 0.f, 1.f );
            const float floor = std::clamp( float{ cvar::rt_dynlight_blink_floor }, 0.f, 1.f );
            blink             = floor + ( 1.f - floor ) * t;
        }

        // Flicker-only trim. rt_dynlight_intensity is GLOBAL to every FDynamicLight, so
        // using it to bring the SMON monitor wall down dimmed the key-door lights and
        // every other 9800 with it -- reported immediately and rolled back (2026-08-12).
        // This scale applies to exactly the class rt_dynlight_flicker gates, which is
        // 199-of-205 SMON panels; doors are steady PointLights and never reach it.
        // PulseLight is deliberately NOT included: it is a different authored fixture
        // (the MAP29 SMONDA 9801 among them) and folding it in would retune content
        // nobody has complained about.
        float flickerScale = 1.f;
        if( light->lighttype == FlickerLight || light->lighttype == RandomFlickerLight )
        {
            flickerScale = std::max( 0.f, float{ cvar::rt_dynlight_flicker_scale } );
        }

        float intensity = hi * intensityScale * flickerScale * blink;
        if( stackAtten )
        {
            const int n = std::max( 1, stackCount[ xyKey( light->X(), light->Y() ) ] );
            intensity /= float( n );
        }
        if( intensityMax > 0.f )
        {
            intensity = std::min( intensity, intensityMax );
        }
        // Large map-radius PointLights (MAP04 yellow hall r=88) otherwise all sit at
        // rt_dynlight_max and read as flat sector fill, drowning hanging-lamp pools.
        // Inv-square roll-off above rsoft keeps jamb-sized lights (r~32) unchanged.
        //
        // Rolled off on `hi`, the fixture's NOMINAL radius, not on the instantaneous
        // mapRadius -- and that distinction is the whole flicker bug (2026-08-12).
        // Using mapRadius made the roll-off fight the blink term above: blink rises with
        // the current radius while the roll-off falls with it, so on a 24/20 monitor the
        // CREST was divided by (20/24)^2 and the trough was not. That compressed the top
        // of the swing, and at any blink floor above ~0.36 it INVERTED the pulse outright
        // -- brightest at the dim end -- which is the same "radius is not brightness" trap
        // the LAMPS table in make_seqlight_fix.py documents for map things.
        //
        // hi is constant per light, so the roll-off is now a fixed per-fixture scale and
        // rt_dynlight_blink_floor alone governs the swing. Steady lights are unaffected:
        // for them mapRadius == hi already.
        {
            const float rSoft = float{ cvar::rt_dynlight_rsoft };
            if( rSoft > 1.f && hi > rSoft )
            {
                const float t = rSoft / hi;
                intensity *= t * t;
            }
        }
        if( intensity <= 0.01f )
        {
            continue;
        }

        const int cr = light->GetRed();
        const int cg = light->GetGreen();
        const int cb = light->GetBlue();
        if( cr + cg + cb <= 0 )
        {
            continue;
        }

        const auto color = rt.rgUtilPackColorByte4D( cr, cg, cb, 255 );

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = color,
            .intensity = intensity,
            .position  = { float( light->X() ) * ONEGAMEUNIT_IN_METERS,
                           float( light->Y() ) * ONEGAMEUNIT_IN_METERS,
                           float( light->Z() ) * ONEGAMEUNIT_IN_METERS },
            .radius    = srcRadius,
        };

        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sph,
            .uniqueID     = stableId,
            .isExportable = false,
        };

        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        // Magenta hotspot markers so light sources are visible as blobs in-world
        // (no RTGL "draw all lights" overlay exists; this is the closest engine-side debug).
        if( cvar::rt_dynlight_debug_marks )
        {
            auto markSph = RgLightSphericalEXT{
                .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                .pNext     = nullptr,
                .color     = rt.rgUtilPackColorByte4D( 255, 0, 255, 255 ),
                .intensity = std::max( 0.f, float{ cvar::rt_light_mark_intensity } ),
                .position  = sph.position,
                .radius    = 0.05f,
            };
            auto markInfo = RgLightInfo{
                .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                .pNext        = &markSph,
                .uniqueID     = stableId + 0x50000000ull,
                .isExportable = false,
            };
            r = rt.rgUploadLight( &markInfo );
            RG_CHECK( r );
        }

        ++index;
    }

    if( cvar::rt_dynlight_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            // stack_atten only divides when co-located lights land in the SAME 4-unit
            // xyKey bucket. If maxStack is 1 the attenuation is a no-op by construction
            // — which is exactly what "toggling stack_atten changes nothing" looks like
            // — and Doom 64's triple-PointLight key-door jambs upload at full 3x.
            int stackedBuckets = 0;
            int maxStack       = 0;
            for( const auto& [ bucket, n ] : stackCount )
            {
                stackedBuckets += ( n > 1 );
                maxStack = std::max( maxStack, n );
            }
            Printf( "rt_dynlight_debug: %u active GZDoom light(s) uploaded this frame; "
                    "xy-buckets with >1 light: %d, max stack: %d\n",
                    index,
                    stackedBuckets,
                    maxStack );

            // Identify one specific offending light (the white one on the MAP02 blue-room
            // switch) by walking up to it and reading this. Owner class + color is what
            // lets it be filtered by something meaningful; filtering by map position would
            // be the same one-room hack the sector-tint work already had to undo.
            struct NearLight
            {
                double         dist2;
                FDynamicLight* light;
            };
            std::vector< NearLight > nearest;
            const DVector3           vp = r_viewpoint.Pos;

            for( FDynamicLight* light = primaryLevel->lights; light != nullptr; light = light->next )
            {
                if( !light->IsActive() || light->X() < -1.0e6 )
                {
                    continue;
                }
                const double dx = light->X() - vp.X;
                const double dy = light->Y() - vp.Y;
                const double dz = light->Z() - vp.Z;
                nearest.push_back( { dx * dx + dy * dy + dz * dz, light } );
            }

            std::sort( nearest.begin(),
                       nearest.end(),
                       []( const NearLight& a, const NearLight& b ) { return a.dist2 < b.dist2; } );

            for( size_t n = 0; n < std::min< size_t >( 5, nearest.size() ); n++ )
            {
                FDynamicLight* l     = nearest[ n ].light;
                AActor*        owner = l->target.Get();
                Printf( "  near[%zu] dist=%.0f owner='%s' rgb=(%d,%d,%d) r=%.0f type=%d "
                        "xyz=(%.0f,%.0f,%.0f)\n",
                        n,
                        std::sqrt( nearest[ n ].dist2 ),
                        ( owner && owner->GetClass() ) ? owner->GetClass()->TypeName.GetChars()
                                                       : "?",
                        l->GetRed(),
                        l->GetGreen(),
                        l->GetBlue(),
                        l->m_currentRadius,
                        int( l->lighttype ),
                        float( l->X() ),
                        float( l->Y() ),
                        float( l->Z() ) );
            }
        }
    }

    // DLSS-RR: any ID present in exactly one of the two sets means a light
    // appeared or disappeared this frame -- flush temporal history.
    if( bool{ cvar::rt_rr_reset_on_dynlight } && curDynIds != s_prevDynIds )
    {
        // Detail line, throttled: if this trigger is over-firing it can hit every
        // single frame, and 60 console lines/s would drown everything else. Print
        // at most one line per 15 changes and carry the skipped count on it.
        static uint32_t s_dbgSince = 0;
        if( cvar::rt_rr_reset_debug && ( s_dbgSince++ % 15 ) == 0 )
        {
            uint32_t appeared = 0;
            uint32_t vanished = 0;
            for( uint64_t id : curDynIds )
            {
                appeared += ( s_prevDynIds.count( id ) == 0 );
            }
            for( uint64_t id : s_prevDynIds )
            {
                vanished += ( curDynIds.count( id ) == 0 );
            }
            Printf( "rt_rr_reset: dynlight set changed +%u/-%u (present %u, was %u) "
                    "[change #%u]\n",
                    appeared,
                    vanished,
                    uint32_t( curDynIds.size() ),
                    uint32_t( s_prevDynIds.size() ),
                    s_dbgSince );
        }
        g_rt_lightcut     = true;
        g_rt_lightcut_why = "dynlight";
    }
    s_prevDynIds = std::move( curDynIds );
}


void RT_WatchLightlevels()
{
    static std::vector< int > s_prev;
    static const void*        s_level = nullptr;

    if( !cvar::rt_lightlevel_watch || !primaryLevel )
    {
        if( !cvar::rt_lightlevel_watch )
        {
            s_prev.clear();
            s_level = nullptr;
        }
        return;
    }

    const unsigned n = primaryLevel->sectors.Size();
    if( s_level != primaryLevel || s_prev.size() != n )
    {
        s_prev.assign( n, INT_MIN );
        s_level = primaryLevel;
        Printf( "rt_lightlevel_watch: armed on %u sectors\n", n );

        // Dump the running light thinkers at the same moment, automatically. The CCMD
        // exists too, but it cannot be put on the launcher command line -- +commands
        // run before the level is loaded, so it would only ever print "no level" and
        // the arm would look like it had answered when it had not.
        {
            auto it = TThinkerIterator< DLighting >( primaryLevel, STAT_LIGHT );
            int  k  = 0;
            while( DLighting* l = it.Next() )
            {
                sector_t* s = l->GetSector();
                Printf( "rt_lightlevel_watch: thinker %-16s sector %-4d tag=%d\n",
                        l->GetClass()->TypeName.GetChars(),
                        s ? s->Index() : -1,
                        s ? primaryLevel->GetFirstSectorTag( s ) : -1 );
                k++;
            }
            Printf( "rt_lightlevel_watch: %d light thinker(s) running at load\n", k );
        }
    }

    for( unsigned i = 0; i < n; i++ )
    {
        const int now = primaryLevel->sectors[ i ].lightlevel;
        const int was = s_prev[ i ];
        s_prev[ i ]   = now;
        if( was == INT_MIN || was == now )
        {
            continue;
        }
        Printf( "rt_lightlevel_watch: sector %-4u  %3d -> %3d\n", i, was, now );
    }
}

void RT_UpdateSectorEmisThreshold()
{
    static const void* s_cachedLevel   = nullptr;
    static unsigned    s_cachedSectors = 0;
    static float       s_cachedMargin  = -1.f;
    static float       s_cachedFloor   = -1.f;

    if( !primaryLevel || primaryLevel->sectors.Size() == 0 )
    {
        g_sectorEmisThreshold = 255.f;
        return;
    }

    const float margin   = float{ cvar::rt_sector_emis_margin };
    const float absFloor = float{ cvar::rt_sector_emis_minlight };

    // Recompute only on map change or when the tuning cvars move.
    if( s_cachedLevel == primaryLevel && s_cachedSectors == primaryLevel->sectors.Size() &&
        s_cachedMargin == margin && s_cachedFloor == absFloor )
    {
        return;
    }

    std::vector< int > levels;
    levels.reserve( primaryLevel->sectors.Size() );
    for( unsigned i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        levels.push_back( primaryLevel->sectors[ i ].lightlevel );
    }

    const size_t mid = levels.size() / 2;
    std::nth_element( levels.begin(), levels.begin() + mid, levels.end() );
    const float median = float( levels[ mid ] );

    g_sectorEmisThreshold = std::max( absFloor, median + margin );

    s_cachedLevel   = primaryLevel;
    s_cachedSectors = primaryLevel->sectors.Size();
    s_cachedMargin  = margin;
    s_cachedFloor   = absFloor;

    if( cvar::rt_sector_emis_debug )
    {
        Printf( "rt_sector_emis: map median lightlevel=%.0f margin=%.0f floor=%.0f "
                "-> only sectors above %.0f self-emit\n",
                median,
                margin,
                absFloor,
                g_sectorEmisThreshold );
    }
}


