// Fixture lights: lights inferred from what a surface's TEXTURE is, for fixtures
// the map never gave a light thing to. Doom 64 painted most of its lamps into
// the art, so without this a ceiling inset lamp is a bright picture of a lamp
// that emits nothing.
//
//   RT_UploadCeilingInsetLamps - recessed ceiling lamps, incl. the faux-panel
//                                and solo-bulb lattices
//   RT_UploadSpinPanelLights   - the CTEL spinning panels
//   RT_UploadWallStripLights   - lit wall strips
//   RT_UploadCeilingEdgeLamps  - flat-mounted bulb arrays (SFLATAS/SFLATAQ)
//   RT_UploadHangingTechLamps  - hanging tech lamp actors
//   RT_UploadHandGlowLights    - the Hell Knight fist lights
//   RT_DebugNearbyWallTextures - the diagnostic that names what is actually on
//                                the walls around the camera
//
// Split out of rt_main.cpp. Behaviour unchanged; this is a move.

#include "rt_internal.h"

// The shared internals (RG_CHECK, ONEGAMEUNIT_IN_METERS, RT_SectorHue, the
// light-ID bases) come in unqualified, exactly as when this code lived inside
// rt_main.cpp's anonymous namespace.
using namespace rtx;

// Sector planes that got real bulb-lattice lights on the last uploaded frame, keyed by
// secIndex*2 + isCeiling. See the rebuild in RT_UploadCeilingEdgeLamps and the reader
// RT_IsLatticeLitPlane below.
static std::unordered_set< uint32_t > g_latticeLitPlanes;

// Did this sector plane get real lights, i.e. may its painted glow be switched off?
//
// The question is NOT "is this one of the lamp textures". MAP03 hangs SFLATAQ on 46
// ceilings AND their 46 matching floors, and most of those are thin recessed strips --
// small panes where the lattice places nothing. Suppressing by texture name put every one
// of those strips out: no glow, no light, a dead groove in the ceiling. So the glow comes
// off exactly the panes that got something to replace it, and nowhere else.
bool RT_IsLatticeLitPlane( unsigned secIndex, bool ceiling )
{
    return g_latticeLitPlanes.find( ( secIndex << 1 ) | unsigned( ceiling ) ) !=
           g_latticeLitPlanes.end();
}

static bool RT_IsCeilingInsetLampTexture( const char* name )
{
    if( !name || !*name )
    {
        return false;
    }
    // Doom 64 inset ceiling lamps: round bright blobs on dark flats (MAP01 spawn
    // booths over the first zombies use SFLATAS).
    //
    // NOT SFLATAP: it is a recessed grille/vent panel with slats, and the original game
    // does not light it. It was in this list from the start and only became visible once
    // the flat walk covered floors and stopped letting one sector eat the budget — a
    // false positive can sit unnoticed for as long as the path around it is broken
    // (2026-08-08).
    if( strncmp( name, "SFLATAS", 7 ) == 0 || strncmp( name, "SFLATAQ", 7 ) == 0 )
    {
        return true;
    }
    if( strncmp( name, "SPORT", 5 ) == 0 )
    {
        return true;
    }
    return false;
}

// The faux pair, kept strictly apart from the real bulb classifiers above.
//
// SFLATC and SPACECE are not lamps. There are no bulbs in the art and the original game
// never lights them; MAP03's stair hall is ceilinged in SFLATC and is simply dark. This
// treats them as bulb arrays anyway, to lift rooms that read as too dark under RT.
//
// The usage split is why there are two predicates rather than one, and it mirrors the
// real pair exactly. Across the game SFLATC appears 76 times and is a FLAT (33 floor,
// 43 ceiling) like SFLATAQ, so it belongs to the flat perimeter walk. SPACECE appears 61
// times and is a WALL texture (60 on sidedefs, 1 stray floor) like SPACEAZ, so it belongs
// to the wall strip walk. Feeding either to the wrong walk would match almost nothing.
static bool RT_IsFauxLampFlat( const char* name )
{
    return cvar::rt_faux_lamps && name && *name && strcmp( name, "SFLATC" ) == 0;
}

static bool RT_IsFauxLampWall( const char* name )
{
    return cvar::rt_faux_lamps && name && *name && strcmp( name, "SPACECE" ) == 0;
}

// Raw, not hue-normalised. RT_SectorHue forces the peak channel to 1 so a tint can never
// darken a light; here darkness is the requested behaviour, so the colour is used as it
// is written and rt_faux_lamp_intensity carries the brightness.
static FVector3 RT_FauxLampHue()
{
    const uint32_t c = uint32_t( cvar::rt_faux_lamp_color );
    return FVector3{ float( ( c >> 16 ) & 0xFF ) / 255.0f,
                     float( ( c >> 8 ) & 0xFF ) / 255.0f,
                     float( c & 0xFF ) / 255.0f };
}

// The lamp's OWN colour, multiplied onto the sector's colormap hue for the bulb lattice.
//
// It exists because SFLATAQ/SFLATAS stopped being emissive. While the flat glowed, the
// beige of the art was mixed into every bounce in the room and the walls came out warm
// (measured R-B +21.8 in the lab). Real point lights carry no such tint -- they took the
// sector hue, which is neutral in most rooms -- so the same room came out grey-blue at
// +5.5 and lost the look, even at matched brightness.
//
// The default is not invented. Sampling the brightest 3% of each texture's own _e map
// gives FFFFED for SFLATAQ and FFF2E6 for SFLATAS; FFF2E6 is the warmer of the two and is
// what a "white / beige" ceiling pane should throw.
//
// Multiplied, not substituted: a coloured room must still colour its own ceiling lamps,
// which is what RT_SectorHue is for. A neutral sector leaves this tint showing; MAP02's
// blue armor room still goes blue.
// Takes the sector hue rather than returning a tint to multiply, because FVector3 has no
// componentwise product -- only operator*(vec, scalar) -- and writing the three multiplies
// at the call site is where a typo becomes a colour bug nobody traces.
static FVector3 RT_CeilingBulbHue( FVector3 sectorHue )
{
    const uint32_t c = uint32_t( cvar::rt_ceiling_bulb_color );
    return FVector3{ sectorHue.X * float( ( c >> 16 ) & 0xFF ) / 255.0f,
                     sectorHue.Y * float( ( c >> 8 ) & 0xFF ) / 255.0f,
                     sectorHue.Z * float( c & 0xFF ) / 255.0f };
}

// The solo pair: SFLATDE and SFLATCH. Different from the faux pair in the one way that
// matters — these textures DO show a lit bulb baked into the art (a bright white blob
// dead centre in an X-shaped or ringed housing), the base game simply never wired a light
// to it. So this is not an invention like rt_faux_lamps; it is the same "texture implies a
// fixture" reasoning as the real bulb arrays (SFLATAS/SFLATAQ/SPORT*), just for two names
// that classifier does not cover. Kept off that classifier and given its own cvars/budget
// rather than folded in, because RT_UploadCeilingInsetLamps' shared intensity (700, and
// currently switched off entirely via rt_ceiling_lamps 0 in the launcher) is tuned for a
// different fixture family and reusing it would either relight nothing (feature off) or
// retune those fixtures as a side effect of this one.
//
// Each texture is single-bulb, not a lattice — the geometry is one offset per 64-unit
// tile, not a grid within it — so unlike SFLATC's shared 4-value array, the two textures
// carry their OWN centre, detected the same way (flood-fill centroid of the bright blob):
//   SFLATDE  centre (31.5, 30.5)
//   SFLATCH  centre (32.0, 32.0)
struct SoloBulbTex
{
    const char* name;
    double      ox, oy;
};
static constexpr SoloBulbTex SoloBulbTextures[] = {
    { "SFLATDE", 31.5, 30.5 },
    { "SFLATCH", 32.0, 32.0 },
};

static bool RT_FindSoloBulbOffset( const char* name, double& ox, double& oy )
{
    if( !cvar::rt_solo_lamps || !name || !*name )
    {
        return false;
    }
    for( const SoloBulbTex& t : SoloBulbTextures )
    {
        if( strcmp( name, t.name ) == 0 )
        {
            ox = t.ox;
            oy = t.oy;
            return true;
        }
    }
    return false;
}

// Plain white, used raw like RT_FauxLampHue — but unlike the faux colour, there is no
// darkening intent here, so this exists mainly so the colour is a cvar rather than a
// hardcoded constant, in case a texture is added later whose bulb is not white.
static FVector3 RT_SoloLampHue()
{
    const uint32_t c = uint32_t( cvar::rt_solo_lamp_color );
    return FVector3{ float( ( c >> 16 ) & 0xFF ) / 255.0f,
                     float( ( c >> 8 ) & 0xFF ) / 255.0f,
                     float( c & 0xFF ) / 255.0f };
}

// CTEL: the telemetry panel whose eight rim gems light in turn, clockwise.
//
// Measured from the artwork, per gem cluster, frame by frame. Every rim cluster carries
// the identical luminance set cyclically shifted by one frame, so exactly one is at its
// peak at a time and their combined output is 891 in EVERY frame. The panel depicts a
// light that travels and does not pulse -- which is why this walk moves a constant light
// rather than modulating a fixed one.
//
// Bearings are degrees in world convention (+x east, +y north), taken from each peaking
// cluster's centroid relative to the tile centre. The steps come out at 38-50 degrees,
// i.e. an eighth of a turn per frame, all negative: clockwise.
//
//   frame  centroid    bearing
//     1    ( 6.5,39.5)   197.7
//     2    ( 6.5,22.5)   160.2
//     3    (21.5, 6.5)   111.8
//     4    (42.5, 6.5)    66.3
//     5    (55.5,22.5)    20.6
//     6    (56.5,40.5)   340.2
//     7    (41.5,56.5)   291.8
//     8    (21.5,56.5)   248.2
static constexpr float SpinPanelBearing[ 8 ] = {
    197.7f, 160.2f, 111.8f, 66.3f, 20.6f, 340.2f, 291.8f, 248.2f,
};

static bool RT_IsSpinPanelFlat( const char* name )
{
    return cvar::rt_spin_panels && name && *name && strncmp( name, "CTEL", 4 ) == 0;
}

// "CTEL5" -> 5. Returns 0 for anything unexpected, which the caller treats as "skip"
// rather than "frame 1" -- guessing a frame would put the light on the wrong gem.
static int RT_SpinPanelFrame( const char* name )
{
    if( !name )
    {
        return 0;
    }
    const size_t n = strlen( name );
    if( n < 5 )
    {
        return 0;
    }
    const char c = name[ n - 1 ];
    return ( c >= '1' && c <= '8' ) ? ( c - '0' ) : 0;
}

static FVector3 RT_SpinPanelHue()
{
    const uint32_t c = uint32_t( cvar::rt_spin_panel_color );
    return FVector3{ float( ( c >> 16 ) & 0xFF ) / 255.0f,
                     float( ( c >> 8 ) & 0xFF ) / 255.0f,
                     float( c & 0xFF ) / 255.0f };
}

void RT_UploadSpinPanelLights()
{
    if( !cvar::rt_spin_panels || !primaryLevel )
    {
        return;
    }
    const float intensity = std::max( 0.f, float{ cvar::rt_spin_panel_intensity } );
    const int   maxLights = std::max( 0, int{ cvar::rt_spin_panel_max } );
    if( intensity <= 0.01f || maxLights <= 0 )
    {
        return;
    }

    const float    orbit = float{ cvar::rt_spin_panel_orbit };
    const float    zofs  = float{ cvar::rt_spin_panel_zofs };
    const float    yaw   = float{ cvar::rt_spin_panel_yaw };
    const float    dir   = cvar::rt_spin_panel_cw ? 1.f : -1.f;
    const FVector3 hue   = RT_SpinPanelHue();

    int uploaded = 0;
    int matched  = 0;

    struct Placed
    {
        int   sec, frame, face;
        float bearing, x, y, z;
    };
    std::vector< Placed > placed;

    for( unsigned i = 0; i < primaryLevel->sectors.Size() && uploaded < maxLights; i++ )
    {
        const sector_t& sector = primaryLevel->sectors[ i ];

        for( int face = 0; face < 2 && uploaded < maxLights; face++ )
        {
            const bool isCeiling = ( face == 1 );

            // animate=true: this is the CURRENT frame, which is the whole point --
            // the bearing comes from the artwork on screen, never from a timer.
            auto* gtex = TexMan.GetGameTexture(
                sector.GetTexture( isCeiling ? sector_t::ceiling : sector_t::floor ), true );
            if( !gtex )
            {
                continue;
            }
            const char* fname = gtex->GetName().GetChars();
            if( !RT_IsSpinPanelFlat( fname ) )
            {
                continue;
            }
            matched++;

            const int frame = RT_SpinPanelFrame( fname );
            if( frame < 1 || frame > 8 )
            {
                continue;
            }

            const double cx = double( sector.centerspot.X );
            const double cy = double( sector.centerspot.Y );
            const double zf = sector.floorplane.ZatPoint( sector.centerspot );
            const double zc = sector.ceilingplane.ZatPoint( sector.centerspot );
            if( zc - zf < 1.0 )
            {
                continue;
            }

            const float  deg = SpinPanelBearing[ frame - 1 ] * dir + yaw;
            const double rad = double( deg ) * ( M_PI / 180.0 );
            const double lx  = cx + std::cos( rad ) * orbit;
            const double ly  = cy + std::sin( rad ) * orbit;
            const double lz  = isCeiling ? ( zc - zofs ) : ( zf + zofs );

            auto sph = RgLightSphericalEXT{
                .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                .pNext     = nullptr,
                .color     = rt.rgUtilPackColorFloat4D( hue.X, hue.Y, hue.Z, 1.0f ),
                .intensity = intensity,
                .position  = { float( lx ) * ONEGAMEUNIT_IN_METERS,
                               float( ly ) * ONEGAMEUNIT_IN_METERS,
                               float( lz ) * ONEGAMEUNIT_IN_METERS },
                .radius    = std::max( 0.01f, float{ cvar::rt_spin_panel_radius } ),
            };
            auto info = RgLightInfo{
                .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                .pNext        = &sph,
                .uniqueID     = SpinPanelId_Base + ( uint64_t( i ) << 1 ) + uint64_t( face ),
                .isExportable = false,
            };
            RgResult r = rt.rgUploadLight( &info );
            RG_CHECK( r );
            uploaded++;

            if( cvar::rt_spin_panel_debug )
            {
                placed.push_back( { int( i ), frame, face, deg,
                                    float( lx ), float( ly ), float( lz ) } );
            }
        }
    }

    if( cvar::rt_spin_panel_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            Printf( "rt_spin_panel: uploaded=%d (cap %d) matchedFaces=%d I=%.0f "
                    "orbit=%.1f cw=%d yaw=%.0f\n",
                    uploaded, maxLights, matched, intensity, orbit,
                    int( cvar::rt_spin_panel_cw ), yaw );
            for( const Placed& p : placed )
            {
                Printf( "    sector %-4d %-7s CTEL%d bearing=%.0f xyz=(%.0f,%.0f,%.0f)\n",
                        p.sec, p.face ? "ceiling" : "floor", p.frame, p.bearing,
                        p.x, p.y, p.z );
            }
        }
    }
}

void RT_UploadCeilingInsetLamps()
{
    // Surface _e provides fixture albedo. These analytic lights blink + cast under
    // ceiling flats only (MAP01 spawn "head lights"). Floor lamp panels use texture
    // emissiveMult GI instead — do not upload floor analytic spheres.
    //
    // DLSS-RR skips A-SVGF, so hard on/off + dropping lights from the list
    // nukes ReSTIR temporal reservoirs and shows up as unfiltered-direct sparkle
    // in the final image. Always upload a stable uniqueID and ease intensity.
    if( !cvar::rt_ceiling_lamps || !primaryLevel )
    {
        return;
    }

    const float peak      = std::max( 0.f, float{ cvar::rt_ceiling_lamp_intensity } );
    const float srcRadius = std::max( 0.01f, float{ cvar::rt_ceiling_lamp_radius } );
    const float zOfs      = float{ cvar::rt_ceiling_lamp_zofs };
    const float offScale  = std::clamp( float{ cvar::rt_ceiling_lamp_off }, 0.f, 1.f );
    const float fadeTics  = std::max( 0.f, float{ cvar::rt_ceiling_lamp_fade } );
    if( peak <= 0.01f )
    {
        return;
    }

    // Per-sector eased blink level (survives across frames; resized on map change).
    static TArray<float> s_lampLevel;
    if( s_lampLevel.Size() != primaryLevel->sectors.Size() )
    {
        s_lampLevel.Resize( primaryLevel->sectors.Size() );
        for( unsigned n = 0; n < s_lampLevel.Size(); n++ )
        {
            s_lampLevel[ n ] = 1.f;
        }
    }

    const int maptime = primaryLevel->maptime;
    uint32_t  uploaded = 0;

    for( unsigned i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        sector_t& sector = primaryLevel->sectors[ i ];
        auto*     gtex =
            TexMan.GetGameTexture( sector.GetTexture( sector_t::ceiling ), true );
        if( !gtex )
        {
            continue;
        }
        const char* tname = gtex->GetName().GetChars();
        if( !RT_IsCeilingInsetLampTexture( tname ) )
        {
            continue;
        }

        const float zfloor =
            float( sector.floorplane.ZatPoint( sector.centerspot ) );
        const float zceiling =
            float( sector.ceilingplane.ZatPoint( sector.centerspot ) );
        if( zceiling - zfloor < 8.f )
        {
            continue;
        }

        // Large halls (MAP02 SFLATAQ corridors) only have lamp blobs on the texture
        // edges. A single analytic sphere at centerspot makes a blinking white patch
        // in empty mid-ceiling. Keep analytics for small booths (MAP01 ~96×96).
        const float maxSpan = std::max( 0.f, float{ cvar::rt_ceiling_lamp_maxspan } );
        if( maxSpan > 0.f )
        {
            float minx = 1.e9f, miny = 1.e9f, maxx = -1.e9f, maxy = -1.e9f;
            bool  any  = false;
            for( unsigned li = 0; li < sector.Lines.Size(); li++ )
            {
                const line_t* line = sector.Lines[ li ];
                if( !line )
                {
                    continue;
                }
                for( vertex_t* v : { line->v1, line->v2 } )
                {
                    if( !v )
                    {
                        continue;
                    }
                    any  = true;
                    minx = std::min( minx, float( v->fX() ) );
                    miny = std::min( miny, float( v->fY() ) );
                    maxx = std::max( maxx, float( v->fX() ) );
                    maxy = std::max( maxy, float( v->fY() ) );
                }
            }
            if( any && ( maxx - minx > maxSpan || maxy - miny > maxSpan ) )
            {
                continue;
            }
        }

        // Mostly-on cycle with short dips (same timing as before), but target
        // stays >= offScale so the light never leaves the ReSTIR list.
        const int phase = int( ( maptime * 4 + int( i ) * 23 ) % 256 );
        const bool blackout =
            ( phase < 40 ) || ( phase >= 110 && phase < 122 ) || ( phase >= 200 && phase < 208 );
        const float target = blackout ? offScale : 1.f;

        float& level = s_lampLevel[ i ];
        if( fadeTics <= 0.f )
        {
            level = target;
        }
        else
        {
            const float step = 1.f / fadeTics;
            if( level < target )
            {
                level = std::min( target, level + step );
            }
            else if( level > target )
            {
                level = std::max( target, level - step );
            }
        }

        // Always upload (even when dim) — hard delete was the RR noise source.
        const float intensity = std::max( peak * level, peak * offScale * 0.25f );

        const float z = zceiling - zOfs;
        const float px = float( sector.centerspot.X ) * ONEGAMEUNIT_IN_METERS;
        const float py = float( sector.centerspot.Y ) * ONEGAMEUNIT_IN_METERS;
        const float pz = z * ONEGAMEUNIT_IN_METERS;

        // Warm white base — matches inset lamp blobs better than SMON green 9802s —
        // modulated by the sector's own Doom 64 colormap hue, so colored rooms (MAP02's
        // 0x0050FF blue armor room) light up in their intended color instead of being
        // washed neutral by a hardcoded lamp.
        const FVector3 hue =
            RT_SectorHue( sector.Colormap.LightColor, float{ cvar::rt_sector_tint_lights } );

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( 1.000f * hue.X,
                                                    0.902f * hue.Y,
                                                    0.745f * hue.Z,
                                                    1.0f ),
            .intensity = intensity,
            .position  = { px, py, pz },
            .radius    = srcRadius,
        };

        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sph,
            .uniqueID     = CeilingLampId_Base + i,
            .isExportable = false,
        };

        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        if( cvar::rt_ceiling_lamp_debug )
        {
            auto markSph = RgLightSphericalEXT{
                .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                .pNext     = nullptr,
                .color     = rt.rgUtilPackColorByte4D( 0, 255, 255, 255 ),
                .intensity = 400.f,
                .position  = { px, py, pz },
                .radius    = 0.05f,
            };
            auto markInfo = RgLightInfo{
                .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                .pNext        = &markSph,
                .uniqueID     = CeilingLampId_Base + 0x08000000ull + i,
                .isExportable = false,
            };
            r = rt.rgUploadLight( &markInfo );
            RG_CHECK( r );

            if( ( maptime % 35 ) == 0 && uploaded < 8 )
            {
                Printf( "rt_ceiling_lamp: sec %u '%s' xyz=(%.0f,%.0f,%.0f) I=%.0f\n",
                        i,
                        tname,
                        float( sector.centerspot.X ),
                        float( sector.centerspot.Y ),
                        z,
                        intensity );
            }
        }

        ++uploaded;
    }

    if( cvar::rt_ceiling_lamp_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            Printf( "rt_ceiling_lamp_debug: %u ceiling inset lamp(s) uploaded\n", uploaded );
        }
    }
}

// Two shapes of tech lamp, and the difference that matters is where the bulb sits.
//
//   Hang  64LampTechLongHang (1015/LMP1), 64LampTechShortHang (1016/LMP2).
//         +SPAWNCEILING, so mo->Z() is the BOTTOM of the bbox and the bulb hangs in the
//         lower part of the fixture.
//   Pole  64TechPoleLong (1031/A035, height 80), 64TechPoleShort (1032/A036, height 60).
//         Floor-standing, bulb in the head at the TOP.
//
// Placing a pole lamp's light with the hanging fraction would bury it in the pole's
// shaft, which is solid — fully occluded, and indistinguishable from no light at all
// (§13). One enum, two height fractions (2026-08-08).
enum class RtTechLamp
{
    None,
    Hang,
    Pole,
};

static RtTechLamp RT_TechLampKind( AActor* mo )
{
    if( !mo )
    {
        return RtTechLamp::None;
    }
    if( mo->sprite >= 0 && mo->sprite < sprites.Size() )
    {
        const char* sn = sprites[ mo->sprite ].name;
        if( sn && sn[ 0 ] == 'L' && sn[ 1 ] == 'M' && sn[ 2 ] == 'P' &&
            ( sn[ 3 ] == '1' || sn[ 3 ] == '2' ) )
        {
            return RtTechLamp::Hang;
        }
        // A035 / A036 are the pole lamps' only sprite. Matched in full, not by an 'A'
        // prefix, which would swallow a large slice of the sprite table.
        if( sn && strnicmp( sn, "A035", 4 ) == 0 )
        {
            return RtTechLamp::Pole;
        }
        if( sn && strnicmp( sn, "A036", 4 ) == 0 )
        {
            return RtTechLamp::Pole;
        }
    }
    // Class-name fallback (sprite table glitches / replacements).
    if( mo->GetClass() && mo->GetClass()->TypeName.IsValidName() )
    {
        const char* cn = mo->GetClass()->TypeName.GetChars();
        if( cn )
        {
            if( stricmp( cn, "64LampTechLongHang" ) == 0 ||
                stricmp( cn, "64LampTechShortHang" ) == 0 )
            {
                return RtTechLamp::Hang;
            }
            if( stricmp( cn, "64TechPoleLong" ) == 0 ||
                stricmp( cn, "64TechPoleShort" ) == 0 )
            {
                return RtTechLamp::Pole;
            }
        }
    }
    return RtTechLamp::None;
}

// A light feature is a surface much brighter than the rest of ITS map, not one over a
// fixed number. Take the map's median sector lightlevel and require a margin above it,
// with the absolute floor still applied so a uniformly dim map does not start glowing.
// Report every sector whose lightlevel moved since the last frame.
//
// Deliberately dumb and total: no filtering by texture, tag or distance, because every
// filter this session has been an opportunity to point the instrument at the wrong
// thing and read the silence as an answer. It walks all sectors, which is ~250 on a
// Retribution map and free next to a single traced ray.

static bool RT_IsWallStripLampTexture( const char* name )
{
    if( !name || !*name )
    {
        return false;
    }
    // The bulb arrays themselves: a regular grid of round lamps.
    //
    //   SPACEAZ  4x4 bulbs, authored as a wall texture
    //   SFLATAQ  4x4 bulbs, authored as a flat but ALSO hung on wall faces
    //   SFLATAS  2x2 large bulbs
    //
    // SFLATAP is deliberately absent despite the matching name: it is a recessed grille,
    // not a lamp, and the original game does not light it.
    //
    // This used to match SPACEAR instead, which is a mistake worth recording. SPACEAR is
    // the plain trim panel that sits on the same thin step the bulb flat caps, so on
    // MAP03 it is adjacent to an SFLATAQ bulb flat on 54 of its 57 sidedefs (95%) — the
    // light landed a few units from the real fixture and looked correct. On MAP02 that
    // adjacency is 4 of 41 (10%): the same rule lights blank wall and misses every actual
    // lamp. A rule that is right by proximity on the map you tested is not a rule.
    //
    // Flats named SFLAT* reach this walk because Doom 64 hangs them on sidedefs too —
    // MAP02 carries SFLATAQ as `bottom` 26 times and `middle` 4 times. The flat-side
    // coverage is separate: see RT_UploadCeilingInsetLamps / RT_UploadCeilingEdgeLamps
    // (2026-08-08).
    return strcmp( name, "SPACEAZ" ) == 0 || strcmp( name, "SFLATAQ" ) == 0 ||
           strcmp( name, "SFLATAS" ) == 0;
}

// Doom 64 wall light strips carry their light in the texture only. Under RTGL1 an
// emissive surface is not a light source (see rt_wall_strips), so the strip glows but
// lights nothing — the corridor reads flat and shadowless. Place real area lights along
// the fixture instead.
//
// Polygonal rather than spherical on purpose: a strip is a long thin emitter, and a
// chain of point lights gives scalloped hotspots along the wall instead of an even wash.
void RT_UploadWallStripLights()
{
    if( !cvar::rt_wall_strips || !primaryLevel )
    {
        return;
    }

    const float peak     = std::max( 0.f, float{ cvar::rt_wall_strip_intensity } );
    const float minLight = float{ cvar::rt_wall_strip_minlight };
    const float segLen   = std::max( 16.f, float{ cvar::rt_wall_strip_seglen } );
    const int   maxLights = std::max( 0, int{ cvar::rt_wall_strip_max } );
    // Faux panels have their own intensity and cap, so zeroing the real strips must not
    // switch them off with it -- turning the real fixtures down to judge the fake ones is
    // exactly the comparison someone will want to run.
    const bool  fauxOn   = bool{ cvar::rt_faux_lamps } &&
                          float{ cvar::rt_faux_lamp_intensity } > 0.01f &&
                          int{ cvar::rt_faux_lamp_max } > 0;
    if( ( peak <= 0.01f || maxLights <= 0 ) && !fauxOn )
    {
        return;
    }

    // Rejection tally, not just a success count: "0 uploaded" is ambiguous on its own,
    // and the stack-attenuation hunt already showed how expensive that ambiguity is.
    int uploaded    = 0;
    int matchedTex  = 0;
    int rejLight    = 0;
    int rejBand     = 0;
    int rejShort    = 0;
    int marked      = 0;

    // Faux panels are budgeted apart from the real strips, for the same reason the flat
    // walk splits its cap: an invented fixture must never push a real one out.
    const int fauxMax      = std::max( 0, int{ cvar::rt_faux_lamp_max } );
    int       fauxWalls    = 0;
    int       fauxUploaded = 0;
    // The walk stops only when BOTH budgets are spent -- gating the loops on the real
    // count alone would let a run of real strips end the walk before any faux panel was
    // even looked at.
    auto budgetLeft = [ & ] { return uploaded < maxLights || fauxUploaded < fauxMax; };

    // Placement of the lights actually near the camera, not one arbitrary sample:
    // "the fixture matched" and "the light is where the bulbs are" are different claims,
    // and only the second explains a strip that is found but still looks unlit.
    struct Placed
    {
        double dist;
        double x, y, z;
        double bandLow, bandHigh;
        int    part;
        FString tex; // two families match now, so "which one" is part of the answer
    };
    std::vector< Placed > placed;
    const DVector3        vpos = r_viewpoint.Pos;

    for( unsigned i = 0; i < primaryLevel->lines.Size() && uploaded < maxLights; i++ )
    {
        const line_t& line = primaryLevel->lines[ i ];
        if( !line.v1 || !line.v2 )
        {
            continue;
        }

        for( int s = 0; s < 2 && budgetLeft(); s++ )
        {
            const side_t* side = line.sidedef[ s ];
            if( !side || !side->sector )
            {
                continue;
            }

            const sector_t* thisSec  = side->sector;
            const side_t*   otherSide = line.sidedef[ 1 - s ];
            const sector_t* otherSec = otherSide ? otherSide->sector : nullptr;

            for( int part = 0; part < 3 && budgetLeft(); part++ )
            {
                auto* gtex = TexMan.GetGameTexture( side->GetTexture( part ), true );
                if( !gtex )
                {
                    continue;
                }
                const char* wtname = gtex->GetName().GetChars();
                const bool  isFaux = RT_IsFauxLampWall( wtname );
                if( !isFaux && !RT_IsWallStripLampTexture( wtname ) )
                {
                    continue;
                }
                matchedTex++;
                if( isFaux )
                {
                    fauxWalls++;
                }

                // Checked after the texture match so the tally can tell "no strips in
                // this map" apart from "strips found but every one was rejected".
                //
                // Faux panels are exempt, and this is the whole point of them. The
                // minlight gate exists so a real strip in an already-bright room does not
                // double-light it; but a faux panel's only job is to lift a room that is
                // too dark, so applying the gate would reject precisely the sectors the
                // feature was asked for and leave it looking like it does nothing.
                if( !isFaux && float( thisSec->lightlevel ) < minLight )
                {
                    rejLight++;
                    continue;
                }

                const double x1 = line.v1->fX();
                const double y1 = line.v1->fY();
                const double x2 = line.v2->fX();
                const double y2 = line.v2->fY();

                const double lineLen = std::hypot( x2 - x1, y2 - y1 );
                if( lineLen < 1.0 )
                {
                    rejShort++;
                    continue;
                }

                const int segs = std::clamp(
                    int( std::ceil( lineLen / segLen ) ), 1, int( WallStripSegsPerLine ) );

                for( int sg = 0; sg < segs && budgetLeft(); sg++ )
                {
                    const double t0 = double( sg ) / segs;
                    const double t1 = double( sg + 1 ) / segs;

                    const double ax = x1 + ( x2 - x1 ) * t0;
                    const double ay = y1 + ( y2 - y1 ) * t0;
                    const double bx = x1 + ( x2 - x1 ) * t1;
                    const double by = y1 + ( y2 - y1 ) * t1;
                    const double mx = ( ax + bx ) * 0.5;
                    const double my = ( ay + by ) * 0.5;

                    const DVector2 mid{ mx, my };

                    // Which vertical band this sidedef part actually covers.
                    double zLow  = 0.0;
                    double zHigh = 0.0;
                    if( part == side_t::top && otherSec )
                    {
                        zLow  = otherSec->ceilingplane.ZatPoint( mid );
                        zHigh = thisSec->ceilingplane.ZatPoint( mid );
                    }
                    else if( part == side_t::bottom && otherSec )
                    {
                        zLow  = thisSec->floorplane.ZatPoint( mid );
                        zHigh = otherSec->floorplane.ZatPoint( mid );
                    }
                    else if( part == side_t::mid && otherSec )
                    {
                        // A middle texture on a two-sided line only covers the OPENING,
                        // not this sector's full height. Handing it floor..ceiling put
                        // MAP02's blue-room strips at mid-room height, floating in front
                        // of the fixture instead of on it (2026-08-08).
                        zLow  = std::max( thisSec->floorplane.ZatPoint( mid ),
                                         otherSec->floorplane.ZatPoint( mid ) );
                        zHigh = std::min( thisSec->ceilingplane.ZatPoint( mid ),
                                          otherSec->ceilingplane.ZatPoint( mid ) );
                    }
                    else
                    {
                        zLow  = thisSec->floorplane.ZatPoint( mid );
                        zHigh = thisSec->ceilingplane.ZatPoint( mid );
                    }

                    if( zHigh < zLow )
                    {
                        std::swap( zLow, zHigh );
                    }
                    if( zHigh - zLow < 1.0 )
                    {
                        rejBand++;
                        continue;
                    }

                    // The fixture is the bulb row, not the whole band: pull the emitter to
                    // the middle of the band and keep it thin, so a tall step does not turn
                    // into a wall-height slab of light.
                    const double zMid  = ( zLow + zHigh ) * 0.5;
                    const double zHalf = std::min( 6.0, ( zHigh - zLow ) * 0.5 );

                    // Nudge off the wall so the emitter is not coplanar with the geometry
                    // it is meant to light (self-shadowing / acne at grazing angles).
                    //
                    // Which way is "off the wall" is decided by testing against the sector's
                    // own centre rather than by Doom's front/back winding convention. Getting
                    // that convention backwards buries every light 2 units inside solid
                    // geometry, where it is fully occluded and emits nothing visible — which
                    // is exactly what happened here, and it looks identical to the lights
                    // never being uploaded at all.
                    const double segDx = bx - ax;
                    const double segDy = by - ay;
                    const double segLenXY = std::hypot( segDx, segDy ) + 1e-6;
                    const double nx = -segDy / segLenXY;
                    const double ny = segDx / segLenXY;

                    const double towardX = double( thisSec->centerspot.X ) - mx;
                    const double towardY = double( thisSec->centerspot.Y ) - my;
                    const double ofs =
                        ( nx * towardX + ny * towardY ) >= 0.0 ? 2.0 : -2.0;

                    // Per-class budget, checked here rather than in the loop guard: the
                    // guard only knows whether SOME budget remains, not whether this
                    // particular light's budget does.
                    if( isFaux ? ( fauxUploaded >= fauxMax ) : ( uploaded >= maxLights ) )
                    {
                        continue;
                    }

                    const FVector3 hue =
                        isFaux ? RT_FauxLampHue()
                               : RT_SectorHue( thisSec->Colormap.LightColor,
                                               float{ cvar::rt_sector_tint_lights } );

                    auto toM = [ & ]( double x, double y, double z ) -> RgFloat3D {
                        return { float( x + nx * ofs ) * ONEGAMEUNIT_IN_METERS,
                                 float( y + ny * ofs ) * ONEGAMEUNIT_IN_METERS,
                                 float( z ) * ONEGAMEUNIT_IN_METERS };
                    };

                    // Spherical, not polygonal: RTGL1 declares RgLightPolygonalEXT in the
                    // public header but LightManager.cpp compiles it out behind
                    // #if TRIANGLE_LIGHTS and hard-errors on upload. Emulate the strip with
                    // overlapping spheres instead — a generous source radius plus a segment
                    // length below it keeps the pools blended rather than scalloped.
                    ( void )zHalf;

                    auto sph = RgLightSphericalEXT{
                        .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                        .pNext     = nullptr,
                        .color     = rt.rgUtilPackColorFloat4D( hue.X, hue.Y, hue.Z, 1.0f ),
                        .intensity = isFaux
                                         ? std::max( 0.f,
                                                     float{ cvar::rt_faux_lamp_intensity } )
                                         : peak,
                        .position  = toM( mx, my, zMid ),
                        .radius = std::max( 0.01f, float{ cvar::rt_wall_strip_radius } ),
                    };

                    auto info = RgLightInfo{
                        .sType    = RG_STRUCTURE_TYPE_LIGHT_INFO,
                        .pNext    = &sph,
                        .uniqueID = WallStripId_Base +
                                    ( uint64_t( i ) * WallStripSegsPerLine * 8 ) +
                                    ( uint64_t( s ) * WallStripSegsPerLine * 4 ) +
                                    ( uint64_t( part ) * WallStripSegsPerLine ) + uint64_t( sg ),
                        .isExportable = false,
                    };

                    RgResult r = rt.rgUploadLight( &info );
                    RG_CHECK( r );

                    // Same aggregate limit as the flat lamps: N markers are N real lights.
                    if( cvar::rt_wall_strip_debug_marks &&
                        marked < std::max( 0, int{ cvar::rt_light_mark_max } ) )
                    {
                        marked++;
                        auto markSph = RgLightSphericalEXT{
                            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                            .pNext     = nullptr,
                            .color     = rt.rgUtilPackColorByte4D( 255, 0, 255, 255 ),
                            .intensity = std::max( 0.f, float{ cvar::rt_light_mark_intensity } ),
                            .position  = toM( mx, my, zMid ),
                            .radius    = 0.05f,
                        };
                        auto markInfo = RgLightInfo{
                            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                            .pNext        = &markSph,
                            .uniqueID     = info.uniqueID + 0x04000000ull,
                            .isExportable = false,
                        };
                        RgResult mr = rt.rgUploadLight( &markInfo );
                        RG_CHECK( mr );
                    }

                    if( cvar::rt_wall_strip_debug )
                    {
                        const double lx = mx + nx * ofs;
                        const double ly = my + ny * ofs;
                        placed.push_back( { std::hypot( lx - vpos.X, ly - vpos.Y ),
                                            lx,
                                            ly,
                                            zMid,
                                            zLow,
                                            zHigh,
                                            part } );
                    }
                    if( isFaux )
                    {
                        fauxUploaded++;
                    }
                    else
                    {
                        uploaded++;
                    }
                }
            }
        }
    }

    if( cvar::rt_wall_strip_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            Printf( "rt_wall_strip: uploaded=%d (cap %d) | matchedTex=%d rejected: "
                    "lightlevel=%d band=%d shortline=%d | I=%.0f radius=%.2f | "
                    "faux %d sidedef(s), uploaded=%d (cap %d) I=%.0f\n",
                    uploaded,
                    maxLights,
                    matchedTex,
                    rejLight,
                    rejBand,
                    rejShort,
                    peak,
                    float{ cvar::rt_wall_strip_radius },
                    fauxWalls,
                    fauxUploaded,
                    fauxMax,
                    float{ cvar::rt_faux_lamp_intensity } );
            Printf( "  viewer z=%.0f — strip lights nearest the camera:\n", vpos.Z );

            std::sort( placed.begin(), placed.end(), []( const Placed& a, const Placed& b ) {
                return a.dist < b.dist;
            } );

            static const char* partName[ 3 ] = { "top", "mid", "bot" };
            for( size_t n = 0; n < std::min< size_t >( 6, placed.size() ); n++ )
            {
                const Placed& p = placed[ n ];
                Printf( "    d=%.0f %s xyz=(%.0f,%.0f,%.0f) band=%.0f..%.0f (%.0f tall)\n",
                        p.dist,
                        partName[ p.part ],
                        p.x,
                        p.y,
                        p.z,
                        p.bandLow,
                        p.bandHigh,
                        p.bandHigh - p.bandLow );
            }
        }
    }
}

// Doom 64 lamp ceilings put their bulbs as blobs around the EDGE of the flat, not in the
// middle. RT_UploadCeilingInsetLamps answers that by putting one sphere at the sector
// centre, which only reads correctly in a small booth — so it skips anything wider than
// rt_ceiling_lamp_maxspan, and every large hall's bulbs end up casting nothing at all.
//
// Trace the sector perimeter instead. Works for both shapes: a ring of lights around a
// small square ceiling panel, and a run of lights along a long corridor's edge.
void RT_UploadCeilingEdgeLamps()
{
    if( !cvar::rt_ceiling_edge_lamps || !primaryLevel )
    {
        return;
    }

    const float peak      = std::max( 0.f, float{ cvar::rt_ceiling_edge_intensity } );
    const float segLen    = std::max( 16.f, float{ cvar::rt_ceiling_edge_seglen } );
    const float srcRadius = std::max( 0.01f, float{ cvar::rt_ceiling_edge_radius } );
    const float zOfs      = float{ cvar::rt_ceiling_edge_zofs };
    const float inset     = float{ cvar::rt_ceiling_edge_inset };
    const int   maxLights = std::max( 0, int{ cvar::rt_ceiling_edge_max } );
    // See RT_UploadWallStripLights: the faux and solo budgets are independent, so the
    // real lamps being off must not take either of them with it.
    const bool  fauxOn    = bool{ cvar::rt_faux_lamps } &&
                        float{ cvar::rt_faux_lamp_intensity } > 0.01f &&
                        int{ cvar::rt_faux_lamp_max } > 0;
    const bool  soloOn    = bool{ cvar::rt_solo_lamps } &&
                        float{ cvar::rt_solo_lamp_intensity } > 0.01f &&
                        int{ cvar::rt_solo_lamp_max } > 0;
    if( ( peak <= 0.01f || maxLights <= 0 ) && !fauxOn && !soloOn )
    {
        return;
    }

    int uploaded   = 0;
    int lampCeils  = 0;
    int lampFloors = 0;
    int fauxFlats  = 0;
    int soloFlats  = 0;
    // Counted separately from lampCeils/lampFloors, which now mean "flats that took the
    // perimeter walk" — without the split, a level whose bulb flats all moved onto the
    // lattice would report an unchanged lamp count while placing lights somewhere else
    // entirely, which is precisely the kind of silent move this debug line exists to catch.
    int bulbLattices = 0;

    // Collect first, then keep the nearest maxLights — do NOT stop the walk at the cap.
    //
    // Emitting in sector-index order and breaking at the cap lets one sector take the
    // whole budget: MAP02's sector 16 has an 11,614-unit perimeter and alone wants 364
    // lights against a cap of 320, so every other bulb sector in the level got nothing
    // and the debug line read "1 lamp ceiling + 1 lamp floor". Demand is ~800 segments on
    // both MAP02 and MAP03, so the cap always binds and *which* lights it drops is the
    // entire behaviour. Nearest-first also puts the budget where it is visible
    // (2026-08-08).
    struct Cand
    {
        double   dist2;
        double   x, y, z;
        uint64_t id;
        FVector3 hue;
        float    intensity;
        float    radius;
        // secIndex*2 + isCeiling, or NoPlane for anything that is not a real bulb
        // lattice. Carried this far so the set of planes that ACTUALLY got lights can be
        // built after the distance cap has trimmed the list -- see g_latticeLitPlanes.
        // Recording at placement time instead would mark a pane lit that the cap then
        // dropped, and that pane would lose its painted glow and gain nothing.
        uint32_t planeKey;
    };
    static constexpr uint32_t NoPlane = UINT32_MAX;
    std::vector< Cand > cand;
    // Faux panels and solo bulbs each collect into their own list and get their own cap,
    // then all three are merged. Appending them to `cand` would let invented/solo fixtures
    // compete with real bulbs for a budget that already binds hard (~800 demand vs 320),
    // so they would darken the real ones — a regression no debug counter would obviously
    // show.
    std::vector< Cand > fauxCand;
    std::vector< Cand > soloCand;

    const DVector3 vpos    = r_viewpoint.Pos;
    const double   maxDist = std::max( 64.0, double( float{ cvar::rt_ceiling_edge_maxdist } ) );
    const double   maxDist2 = maxDist * maxDist;

    // SFLATC's bulb lattice, in texture pixels within the 64x64 tile, detected from the
    // art by tools/make_bulb_textures.py rather than assumed: 4x4 sockets at 7.5, 23.5,
    // 39.5, 55.5 on both axes. Flats are mapped 1:1 to world units from the world origin,
    // so these are also world offsets modulo 64.
    static constexpr double FauxFlatLattice[] = { 7.5, 23.5, 39.5, 55.5 };
    constexpr double        TileSize          = 64.0;

    // The REAL bulb arrays' lattices, detected the same way as SFLATC's and just as much
    // NOT assumed: these are the blob centroids of the authored `_e` masks
    // (tools/gen_bulb_flat_masks.py). SFLATAS is 2x2 at 32-unit spacing, SFLATAQ 4x4 at 16.
    static constexpr double BulbLatticeAS[] = { 15.5, 47.5 };
    static constexpr double BulbLatticeAQ[] = { 7.5, 23.5, 39.5, 55.5 };
    const bool              latticeOn       = bool{ cvar::rt_ceiling_edge_lattice };

    // Stride lives in this table rather than in a cvar, and is per texture rather than
    // shared, because the two lattices are different densities and one number cannot mean
    // the same thing on both. Stride 1 on SFLATAS (bulbs already 32 units apart) and 2 on
    // SFLATAQ (16 -> 32) lands a light every ~32 units on either texture. That matters for
    // the same reason rt_ceiling_edge_intensity is pinned equal to rt_wall_strip_intensity:
    // one physical bulb band crosses between these textures, and a density step reads as a
    // brightness step at the seam.
    auto bulbLatticeFor = []( const char* n, const double*& off, int& nOff, int& stride ) {
        if( strncmp( n, "SFLATAS", 7 ) == 0 )
        {
            off = BulbLatticeAS, nOff = 2, stride = 1;
            return true;
        }
        if( strncmp( n, "SFLATAQ", 7 ) == 0 )
        {
            off = BulbLatticeAQ, nOff = 4, stride = 2;
            return true;
        }
        return false;
    };

    // One light per bulb is unaffordable: at 16-unit spacing a 512x512 room wants over a
    // thousand. The stride subsamples the lattice, so lights stay ON bulbs (which is the
    // whole point) but not on every one. Faux and solo get independent strides because
    // they are different densities of invention: SFLATC is a dense invented grid, the
    // solo textures are a handful of genuine fixtures.
    const int fauxStrideN = std::max( 1, int{ cvar::rt_faux_lamp_stride } );
    const int soloStrideN = std::max( 1, int{ cvar::rt_solo_lamp_stride } );
    const float fauxIntensity = std::max( 0.f, float{ cvar::rt_faux_lamp_intensity } );
    const float soloIntensity = std::max( 0.f, float{ cvar::rt_solo_lamp_intensity } );
    const float soloRadius    = std::max( 0.01f, float{ cvar::rt_solo_lamp_radius } );
    const float soloZofs      = float{ cvar::rt_solo_lamp_zofs };

    // Shared by both the faux (4x4 grid) and solo (single bulb) placements: walk whole
    // 64-unit tiles across a sector's bounding box, and within each tile drop a light at
    // every (offX[ox], offY[oy]) pair — a 4x4 cross product for SFLATC's grid, or a single
    // point for a solo texture's one bulb. offX/offY are separate arrays (not one shared
    // array reused for both axes) because a solo bulb's centre need not be exactly square
    // — SFLATDE's detected centre is (31.5, 30.5), not (31.5, 31.5).
    auto addLattice = [ & ]( const sector_t& sector, unsigned secIndex, bool isCeiling,
                             const double* offX, const double* offY, int nOff, int stride,
                             FVector3 hue, float intensity, float radius, float zofs,
                             uint64_t idBase, std::vector< Cand >& out,
                             uint32_t planeKey = NoPlane ) {
        double minx = 1.e9, miny = 1.e9, maxx = -1.e9, maxy = -1.e9;
        for( unsigned li = 0; li < sector.Lines.Size(); li++ )
        {
            const line_t* line = sector.Lines[ li ];
            if( !line )
            {
                continue;
            }
            for( const vertex_t* v : { line->v1, line->v2 } )
            {
                if( !v )
                {
                    continue;
                }
                minx = std::min( minx, v->fX() );
                maxx = std::max( maxx, v->fX() );
                miny = std::min( miny, v->fY() );
                maxy = std::max( maxy, v->fY() );
            }
        }
        if( maxx < minx || maxy < miny )
        {
            return;
        }

        // Walk whole tiles across the sector's bounding box, then the lattice within each.
        const long tile0x = long( std::floor( minx / TileSize ) );
        const long tile1x = long( std::floor( maxx / TileSize ) );
        const long tile0y = long( std::floor( miny / TileSize ) );
        const long tile1y = long( std::floor( maxy / TileSize ) );

        for( long ty = tile0y; ty <= tile1y; ty++ )
        {
            for( long tx = tile0x; tx <= tile1x; tx++ )
            {
                for( int oy = 0; oy < nOff; oy++ )
                {
                    for( int ox = 0; ox < nOff; ox++ )
                    {
                        // Stride against the ABSOLUTE lattice index, not a per-sector
                        // counter, so the chosen bulbs line up across tile and sector
                        // boundaries instead of jumping at every seam.
                        const long gx = tx * nOff + ox;
                        const long gy = ty * nOff + oy;
                        if( ( ( gx % stride ) + stride ) % stride != 0 ||
                            ( ( gy % stride ) + stride ) % stride != 0 )
                        {
                            continue;
                        }

                        const double px = double( tx ) * TileSize + offX[ ox ];
                        const double py = double( ty ) * TileSize + offY[ oy ];
                        if( px < minx || px > maxx || py < miny || py > maxy )
                        {
                            continue;
                        }

                        const double dx = px - vpos.X;
                        const double dy = py - vpos.Y;
                        const double d2 = dx * dx + dy * dy;
                        if( d2 > maxDist2 )
                        {
                            continue;
                        }

                        // The bounding box is not the sector: an L-shaped room would
                        // otherwise get lights hanging in the neighbouring one.
                        if( primaryLevel->PointInSector( DVector2( px, py ) ) != &sector )
                        {
                            continue;
                        }

                        const DVector2 at{ px, py };
                        const double   pz = isCeiling
                                                ? sector.ceilingplane.ZatPoint( at ) - zofs
                                                : sector.floorplane.ZatPoint( at ) + zofs;

                        // Stable ID from position, not from an emit counter: the nearest-N
                        // set changes as the camera moves, and a counter-derived ID would
                        // renumber every light and flush RR temporal history each frame.
                        const uint64_t id =
                            idBase +
                            ( uint64_t( secIndex ) << 20 ) +
                            ( uint64_t( ( gy & 0x3FF ) ) << 10 ) +
                            uint64_t( gx & 0x3FF ) +
                            ( isCeiling ? 0ull : 0x80000ull );

                        out.push_back( Cand{ d2, px, py, pz, id, hue, intensity, radius, planeKey } );
                    }
                }
            }
        }
    };

    auto addFauxLattice = [ & ]( const sector_t& sector, unsigned secIndex, bool isCeiling ) {
        addLattice( sector, secIndex, isCeiling, FauxFlatLattice, FauxFlatLattice,
                    int( std::size( FauxFlatLattice ) ), fauxStrideN, RT_FauxLampHue(),
                    fauxIntensity, srcRadius, zOfs, FauxLatticeId_Base, fauxCand );
    };

    auto addSoloLattice = [ & ]( const sector_t& sector, unsigned secIndex, bool isCeiling,
                                 double ox, double oy ) {
        const double offX[ 1 ] = { ox };
        const double offY[ 1 ] = { oy };
        addLattice( sector, secIndex, isCeiling, offX, offY, 1, soloStrideN, RT_SoloLampHue(),
                    soloIntensity, soloRadius, soloZofs, SoloLatticeId_Base, soloCand );
    };


    // Both planes, not just the ceiling. Doom 64 runs one continuous bulb band along a
    // wall and then across whichever flat it meets — the band does not care which way it
    // is facing, and neither should this. Reading only sector_t::ceiling left 19 bulb
    // floors unlit on MAP02 and 46 on MAP03: the band visibly stopped at the corner
    // where it turned onto the floor (2026-08-08).
    for( unsigned i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        const sector_t& sector = primaryLevel->sectors[ i ];

        for( int plane = 0; plane < 2; plane++ )
        {
        const bool isCeiling = ( plane == 0 );
        auto*      gtex      = TexMan.GetGameTexture(
            sector.GetTexture( isCeiling ? sector_t::ceiling : sector_t::floor ), true );
        if( !gtex )
        {
            continue;
        }
        const char* ftname = gtex->GetName().GetChars();
        const bool  isFaux = RT_IsFauxLampFlat( ftname );
        double      soloOx = 0.0, soloOy = 0.0;
        const bool  isSolo = !isFaux && RT_FindSoloBulbOffset( ftname, soloOx, soloOy );
        if( !isFaux && !isSolo && !RT_IsCeilingInsetLampTexture( ftname ) )
        {
            continue;
        }
        if( isFaux )
        {
            fauxFlats++;

            // Faux flats do NOT use the perimeter walk below, and that is the whole
            // point of this branch. The perimeter walk drops a light every
            // rt_ceiling_edge_seglen units around the sector edge, which has no relation
            // to where the art puts its bulbs — on SFLATC the sockets are a 4x4 lattice
            // at 16-unit spacing, so perimeter lights land between bulbs, in the middle
            // of blank plate, and read as light coming from nowhere. Placing them on the
            // lattice instead means every faux light sits inside a painted socket.
            //
            // Doom flats are mapped 1:1 to world units and anchored at the world origin,
            // so a socket at texture u appears at every world x with x mod 64 == u. The
            // lattice below is expressed as offsets within that 64-unit tile.
            addFauxLattice( sector, i, isCeiling );
            continue;
        }
        if( isSolo )
        {
            soloFlats++;
            // Same lattice mechanism as faux, same reason (the perimeter walk has no
            // relation to where the art puts its one bulb per tile), just one point per
            // tile instead of sixteen.
            addSoloLattice( sector, i, isCeiling, soloOx, soloOy );
            continue;
        }
        ( isCeiling ? lampCeils : lampFloors )++;

        // Lattice placement for the real bulb arrays, for exactly the reason the isFaux
        // branch above gives — it was simply never applied to them. SFLATAS/SFLATAQ tile
        // their bulbs across the WHOLE flat, so a perimeter walk lights the room's edges
        // and leaves every interior bulb casting nothing: a wide panel stayed dark down
        // its own middle while its art showed lit bulbs there (open-issues 1.6g). Feeds
        // the SAME `cand` list, budget and intensity as the perimeter path, because this
        // changes only WHERE the lights go, not how many or how bright.
        //
        // SPORT* deliberately has no entry and falls through: a teleporter pad is one
        // fixture filling its sector, not a tiled lattice, so the perimeter walk is right
        // for it.
        const double* bulbOff    = nullptr;
        int           bulbN      = 0;
        int           bulbStride = 1;
        if( latticeOn && bulbLatticeFor( ftname, bulbOff, bulbN, bulbStride ) )
        {
            bulbLattices++;

            // ENERGY-CONSERVING DECIMATION, and the reason it had to change.
            //
            // The stride SKIPS lattice points and passed `peak` through
            // untouched, so every point it dropped was light simply deleted.
            // SFLATAQ's stride 2 threw away three bulbs in four; the
            // rt_ceiling_edge_max cap then threw away three in four of what was
            // left -- measured in the lab as 1280 wanted, 320 uploaded. Neither
            // stage compensated, so a lamp ceiling delivered about a sixteenth
            // of the light its own table asked for.
            //
            // What that looked like: toggling ALL 320 lights in a lamp-lit room
            // moved the floor by +0.02 luminance out of 119. The room was ~98%
            // emissive texture GI -- which lights surfaces but is invisible to
            // the froxel volume, so smoke under a brilliantly lit ceiling
            // stayed black while a dim red door lit it perfectly.
            //
            // So: place ONE light per rt_ceiling_bulb_spacing units and give it
            // the energy of the area it stands for. A 512x512 ceiling becomes
            // ~16 real lights instead of 1024 wanted and 320 delivered -- far
            // under the cap, so nothing is dropped and nothing is patchy, and
            // the room's brightness is one number rather than an accident of
            // two decimations.
            const double bulbPitch = TileSize / double( bulbN );
            const int    spaceN    = std::max(
                1, int( std::lround( double{ cvar::rt_ceiling_bulb_spacing } / bulbPitch ) ) );
            // Area per light scales with the square of the spacing, so the
            // intensity does too. This is the whole trick: fewer lights, same
            // total energy, and rt_ceiling_bulb_gain is then a single honest
            // brightness knob on top.
            const float bulbPeak = peak * float( spaceN * spaceN ) *
                                   std::max( 0.f, float{ cvar::rt_ceiling_bulb_gain } );

            addLattice( sector,
                        i,
                        isCeiling,
                        bulbOff,
                        bulbOff,
                        bulbN,
                        spaceN,
                        RT_CeilingBulbHue( RT_SectorHue(
                            sector.Colormap.LightColor,
                            float{ cvar::rt_sector_tint_lights } ) ),
                        bulbPeak,
                        srcRadius,
                        zOfs,
                        CeilingLatticeId_Base,
                        cand,
                        // Only this path marks a plane as lattice-lit. The perimeter walk,
                        // the faux panels and the solo bulbs all pass NoPlane: none of them
                        // replaces a pane's own glow, so none of them may switch it off.
                        ( i << 1 ) | unsigned( isCeiling ) );
            continue;
        }

        for( unsigned li = 0; li < sector.Lines.Size(); li++ )
        {
            const line_t* line = sector.Lines[ li ];
            if( !line || !line->v1 || !line->v2 )
            {
                continue;
            }

            const double x1 = line->v1->fX();
            const double y1 = line->v1->fY();
            const double x2 = line->v2->fX();
            const double y2 = line->v2->fY();

            const double len = std::hypot( x2 - x1, y2 - y1 );
            if( len < 1.0 )
            {
                continue;
            }

            // Clamped so one very long line cannot dominate, and so the id packing below
            // stays collision-free.
            const int segs = std::clamp(
                int( std::ceil( len / segLen ) ), 1, int( CeilingEdgeSegsPerLine ) );

            for( int sg = 0; sg < segs; sg++ )
            {
                const double t  = ( double( sg ) + 0.5 ) / segs;
                const double px = x1 + ( x2 - x1 ) * t;
                const double py = y1 + ( y2 - y1 ) * t;

                // Pull inward toward the sector centre so the lamp is not embedded in the
                // wall. Same lesson as the wall strips: a winding-convention normal put
                // every light inside solid geometry, where it lit nothing.
                double towardX = double( sector.centerspot.X ) - px;
                double towardY = double( sector.centerspot.Y ) - py;
                const double tlen = std::hypot( towardX, towardY );
                if( tlen > 0.001 )
                {
                    towardX /= tlen;
                    towardY /= tlen;
                }

                const double lx = px + towardX * inset;
                const double ly = py + towardY * inset;
                // zOfs pulls the lamp away from its own plane, so it flips sign with the
                // plane: down from a ceiling, up from a floor. Sharing one sign would
                // bury every floor lamp below the floor, fully occluded — the same
                // failure as the wall strips' inverted normal (§13).
                const DVector2 lpos{ lx, ly };
                const double   lz = isCeiling
                                        ? sector.ceilingplane.ZatPoint( lpos ) - zOfs
                                        : sector.floorplane.ZatPoint( lpos ) + zOfs;

                const double dx = lx - vpos.X;
                const double dy = ly - vpos.Y;
                const double dz = lz - vpos.Z;
                const double d2 = dx * dx + dy * dy + dz * dz;
                if( d2 > maxDist2 )
                {
                    continue;
                }

                // Derived from map indices, not from a running counter. An idSeed++ makes
                // every light's ID depend on how many lights happened to be emitted before
                // it, so the moment the camera moves and the nearest-N set changes, every
                // ID shifts and RTGL1 sees the entire set vanish and reappear — which
                // flushes RR temporal history every frame. Map indices are stable.
                const uint64_t id =
                    CeilingEdgeId_Base +
                    ( ( uint64_t( line->Index() ) * 2 + uint64_t( plane ) ) *
                      CeilingEdgeSegsPerLine ) +
                    uint64_t( sg );

                // isFaux and isSolo are always false down here: both branch to their own
                // lattice function and `continue` before reaching this perimeter walk, so
                // this path only ever runs for the real RT_IsCeilingInsetLampTexture case.
                cand.push_back( Cand{
                    d2,
                    lx,
                    ly,
                    lz,
                    id,
                    RT_SectorHue( sector.Colormap.LightColor, float{ cvar::rt_sector_tint_lights } ),
                    peak,
                    srcRadius,
                    NoPlane } );
            }
        }
        }
    }

    const int wanted     = int( cand.size() );
    const int fauxWanted = int( fauxCand.size() );
    const int soloWanted = int( soloCand.size() );
    if( cand.size() > size_t( maxLights ) )
    {
        std::nth_element( cand.begin(),
                          cand.begin() + maxLights,
                          cand.end(),
                          []( const Cand& a, const Cand& b ) { return a.dist2 < b.dist2; } );
        cand.resize( size_t( maxLights ) );
    }

    // Same nearest-N trim, applied to the faux and solo lists against their OWN caps,
    // before all three are merged. Trimming after the merge would defeat the point of the
    // split budgets.
    const int fauxMax = std::max( 0, int{ cvar::rt_faux_lamp_max } );
    if( fauxCand.size() > size_t( fauxMax ) )
    {
        std::nth_element( fauxCand.begin(),
                          fauxCand.begin() + fauxMax,
                          fauxCand.end(),
                          []( const Cand& a, const Cand& b ) { return a.dist2 < b.dist2; } );
        fauxCand.resize( size_t( fauxMax ) );
    }
    const int soloMax = std::max( 0, int{ cvar::rt_solo_lamp_max } );
    if( soloCand.size() > size_t( soloMax ) )
    {
        std::nth_element( soloCand.begin(),
                          soloCand.begin() + soloMax,
                          soloCand.end(),
                          []( const Cand& a, const Cand& b ) { return a.dist2 < b.dist2; } );
        soloCand.resize( size_t( soloMax ) );
    }
    cand.insert( cand.end(), fauxCand.begin(), fauxCand.end() );
    cand.insert( cand.end(), soloCand.begin(), soloCand.end() );
    // Nearest-first ordering, so the marker budget below lands on the lights actually in
    // front of the camera. Cheap at this size, and it makes the upload order stable.
    std::sort( cand.begin(), cand.end(), []( const Cand& a, const Cand& b ) {
        return a.dist2 < b.dist2;
    } );

    // Rebuilt every frame from the SURVIVING candidates, so a pane that lost its lights to
    // the distance cap or the budget is not in it and keeps its painted glow. Read a frame
    // later by the draw path (RT_IsLatticeLitPlane); a one-frame lag on "does this pane
    // have real lights" is invisible, and the alternative -- deciding at placement time --
    // silently blacks out every pane the cap trims.
    g_latticeLitPlanes.clear();
    for( const Cand& c : cand )
    {
        if( c.planeKey != NoPlane )
        {
            g_latticeLitPlanes.insert( c.planeKey );
        }
    }

    const int markMax = std::max( 0, int{ cvar::rt_light_mark_max } );
    int       marked  = 0;

    for( const Cand& c : cand )
    {
        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( c.hue.X, c.hue.Y, c.hue.Z, 1.0f ),
            .intensity = c.intensity,
            .position  = { float( c.x ) * ONEGAMEUNIT_IN_METERS,
                           float( c.y ) * ONEGAMEUNIT_IN_METERS,
                           float( c.z ) * ONEGAMEUNIT_IN_METERS },
            .radius    = c.radius,
        };

        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sph,
            .uniqueID     = c.id,
            .isExportable = false,
        };

        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );
        uploaded++;

        // Cyan, not the wall strips' magenta: with both paths marked at once the only
        // useful question is which one owns a given light, and two colours answer it
        // without a second toggle.
        if( cvar::rt_ceiling_edge_debug_marks && marked < markMax )
        {
            marked++;
            auto markSph = RgLightSphericalEXT{
                .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                .pNext     = nullptr,
                .color     = rt.rgUtilPackColorByte4D( 0, 255, 255, 255 ),
                .intensity = std::max( 0.f, float{ cvar::rt_light_mark_intensity } ),
                .position  = sph.position,
                .radius    = 0.05f,
            };
            auto markInfo = RgLightInfo{
                .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                .pNext        = &markSph,
                .uniqueID     = info.uniqueID + 0x02000000ull,
                .isExportable = false,
            };
            RgResult mr = rt.rgUploadLight( &markInfo );
            RG_CHECK( mr );
        }
    }

    if( cvar::rt_ceiling_edge_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            // `wanted` vs `uploaded` is the point of this line: they were equal only
            // because the walk stopped at the cap, which hid that one sector was taking
            // the entire budget.
            // Faux and solo counted separately on purpose: the whole reason the budgets
            // are split is so a glance can tell whether invented or solo fixtures are
            // crowding real ones.
            Printf( "rt_ceiling_edge: uploaded=%d of %d wanted (cap %d, within %.0fu) "
                    "from %d lamp ceiling(s) + %d lamp floor(s) + %d bulb lattice(s) | I=%.0f | "
                    "faux %d flat(s), %d of %d wanted (cap %d) I=%.0f | "
                    "solo %d flat(s), %d of %d wanted (cap %d) I=%.0f\n",
                    uploaded,
                    wanted,
                    maxLights,
                    maxDist,
                    lampCeils,
                    lampFloors,
                    bulbLattices,
                    peak,
                    fauxFlats,
                    int( fauxCand.size() ),
                    fauxWanted,
                    fauxMax,
                    fauxIntensity,
                    soloFlats,
                    int( soloCand.size() ),
                    soloWanted,
                    soloMax,
                    soloIntensity );
        }
    }
}

// Names the wall fixtures near the camera, so a light-strip matcher can be written
// against real texture names instead of guesses. Prints sector lightlevel too, since
// that is the other half of the "is this a light fixture" test.
void RT_DebugNearbyWallTextures()
{
    if( !cvar::rt_wall_tex_debug || !primaryLevel )
    {
        return;
    }

    static int s_tick;
    if( ( ++s_tick % 60 ) != 0 )
    {
        return;
    }

    const DVector3 vp = r_viewpoint.Pos;
    const double   maxDist =
        std::max( 32.0, double( float{ cvar::rt_wall_tex_debug_dist } ) );

    struct Hit
    {
        double      dist;
        const char* tex;
        int         lightlevel;
        int         part;
        double      x, y;
        double      zLow, zHigh;
    };
    std::vector< Hit > hits;

    // Flats too: Doom 64 puts light fixtures on thin sector steps whose floor/ceiling
    // carry the lamp texture, and a sidedef-only dump cannot see those at all.
    for( unsigned i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        const sector_t& sec = primaryLevel->sectors[ i ];

        const double cx = double( sec.centerspot.X );
        const double cy = double( sec.centerspot.Y );
        const double d  = std::hypot( cx - vp.X, cy - vp.Y );
        if( d > maxDist )
        {
            continue;
        }

        for( int pl = 0; pl < 2; pl++ )
        {
            auto* gtex = TexMan.GetGameTexture(
                sec.GetTexture( pl == 0 ? sector_t::floor : sector_t::ceiling ), true );
            if( !gtex )
            {
                continue;
            }
            const char* nm = gtex->GetName().GetChars();
            if( !nm || !*nm )
            {
                continue;
            }
            const double zPlane = pl == 0 ? sec.floorplane.ZatPoint( sec.centerspot )
                                          : sec.ceilingplane.ZatPoint( sec.centerspot );
            hits.push_back(
                { d, nm, sec.lightlevel, pl == 0 ? 3 : 4, cx, cy, zPlane, zPlane } );
        }
    }

    for( unsigned i = 0; i < primaryLevel->lines.Size(); i++ )
    {
        const line_t& line = primaryLevel->lines[ i ];
        if( !line.v1 || !line.v2 )
        {
            continue;
        }

        const double mx = ( line.v1->fX() + line.v2->fX() ) * 0.5;
        const double my = ( line.v1->fY() + line.v2->fY() ) * 0.5;
        const double d  = std::hypot( mx - vp.X, my - vp.Y );
        if( d > maxDist )
        {
            continue;
        }

        for( int s = 0; s < 2; s++ )
        {
            const side_t* side = line.sidedef[ s ];
            if( !side )
            {
                continue;
            }
            const sector_t* sec = side->sector;

            const side_t*   otherSide = line.sidedef[ 1 - s ];
            const sector_t* otherSec  = otherSide ? otherSide->sector : nullptr;
            const DVector2  mid{ mx, my };

            for( int part = 0; part < 3; part++ )
            {
                auto* gtex = TexMan.GetGameTexture( side->GetTexture( part ), true );
                if( !gtex )
                {
                    continue;
                }
                const char* nm = gtex->GetName().GetChars();
                if( !nm || !*nm )
                {
                    continue;
                }

                // Height of the band this part occupies. This is what identifies a
                // ceiling-level fixture: the name alone does not say where it sits.
                double zLow = 0.0, zHigh = 0.0;
                if( sec )
                {
                    if( part == side_t::top && otherSec )
                    {
                        zLow  = otherSec->ceilingplane.ZatPoint( mid );
                        zHigh = sec->ceilingplane.ZatPoint( mid );
                    }
                    else if( part == side_t::bottom && otherSec )
                    {
                        zLow  = sec->floorplane.ZatPoint( mid );
                        zHigh = otherSec->floorplane.ZatPoint( mid );
                    }
                    else
                    {
                        zLow  = sec->floorplane.ZatPoint( mid );
                        zHigh = sec->ceilingplane.ZatPoint( mid );
                    }
                    if( zHigh < zLow )
                    {
                        std::swap( zLow, zHigh );
                    }
                }

                hits.push_back(
                    { d, nm, sec ? sec->lightlevel : -1, part, mx, my, zLow, zHigh } );
            }
        }
    }

    // Aggregate by texture name. The first version printed the 12 nearest rows, which was
    // dominated by a handful of repeated wall panels and hid the rarer fixture textures
    // entirely -- the upper light strips never appeared in the list at all.
    struct Agg
    {
        double nearest;
        int    count;
        bool   parts[ 5 ]; // top, mid, bot, floor, ceil
        int    minLight;
        int    maxLight;
        double zLow;
        double zHigh;
    };
    std::unordered_map< std::string, Agg > byTex;

    for( const Hit& h : hits )
    {
        auto it = byTex.find( h.tex );
        if( it == byTex.end() )
        {
            Agg a{ h.dist,      1,           { false, false, false, false, false },
                   h.lightlevel, h.lightlevel, h.zLow,
                   h.zHigh };
            a.parts[ h.part ] = true;
            byTex.emplace( h.tex, a );
        }
        else
        {
            Agg& a      = it->second;
            a.nearest   = std::min( a.nearest, h.dist );
            a.count++;
            a.parts[ h.part ] = true;
            a.minLight        = std::min( a.minLight, h.lightlevel );
            a.maxLight        = std::max( a.maxLight, h.lightlevel );
            a.zLow            = std::min( a.zLow, h.zLow );
            a.zHigh           = std::max( a.zHigh, h.zHigh );
        }
    }

    std::vector< std::pair< std::string, Agg > > sorted( byTex.begin(), byTex.end() );
    std::sort( sorted.begin(), sorted.end(), []( const auto& a, const auto& b ) {
        return a.second.nearest < b.second.nearest;
    } );

    Printf( "rt_wall_tex_debug: %zu sidedef texture(s), %zu distinct, within %.0fu\n",
            hits.size(),
            sorted.size(),
            maxDist );
    // Print every distinct name, not a nearest-N slice. The list is already bounded by
    // rt_wall_tex_debug_dist, and the old cap of 24 silently dropped 6 of MAP02's 30 --
    // sorted by distance, so the ones cut were the far ones, which is exactly where a
    // strip on the far side of a room lands. It printed "30 distinct" and then listed 24
    // with no truncation notice, which is the &sect;14 failure over again (2026-08-08).
    constexpr size_t MaxRows = 96;
    for( size_t n = 0; n < std::min( MaxRows, sorted.size() ); n++ )
    {
        const Agg& a = sorted[ n ].second;
        Printf( "  '%s' nearest=%.0f uses=%d parts=%s%s%s%s%s z=%.0f..%.0f lightlevel=%d..%d%s\n",
                sorted[ n ].first.c_str(),
                a.nearest,
                a.count,
                a.parts[ 0 ] ? "top " : "",
                a.parts[ 1 ] ? "mid " : "",
                a.parts[ 2 ] ? "bot " : "",
                a.parts[ 3 ] ? "FLOOR " : "",
                a.parts[ 4 ] ? "CEIL" : "",
                a.zLow,
                a.zHigh,
                a.minLight,
                a.maxLight,
                RT_IsWallStripLampTexture( sorted[ n ].first.c_str() ) ? "  <-- MATCHED as strip"
                                                                      : "" );
    }
    if( sorted.size() > MaxRows )
    {
        Printf( "  ... %zu more not shown -- lower rt_wall_tex_debug_dist to see them\n",
                sorted.size() - MaxRows );
    }
}

void RT_UploadHangingTechLamps()
{
    // MAP04 first room (and many D64 halls) hang LMP1/LMP2 props with BRIGHT sprites
    // but no co-located PointLight things — bulbs look lit, room stays flat ambient.
    // Place a warm analytic sphere at each hanging lamp actor so PT casts pools/shadows.
    if( !cvar::rt_hang_lamps || !primaryLevel )
    {
        return;
    }

    const float hangPeak  = std::max( 0.f, float{ cvar::rt_hang_lamp_intensity } );
    const float polePeak  = std::max( 0.f, float{ cvar::rt_pole_lamp_intensity } );
    const float srcRadius = std::max( 0.01f, float{ cvar::rt_hang_lamp_radius } );
    const float zOfs      = float{ cvar::rt_hang_lamp_zofs };
    if( hangPeak <= 0.01f && polePeak <= 0.01f )
    {
        return;
    }

    const int maptime  = primaryLevel->maptime;
    uint32_t  uploaded = 0;

    auto it = primaryLevel->GetThinkerIterator< AActor >();
    AActor* mo = nullptr;
    while( ( mo = it.Next() ) != nullptr )
    {
        const RtTechLamp kind = RT_TechLampKind( mo );
        if( kind == RtTechLamp::None )
        {
            continue;
        }
        const float peak = ( kind == RtTechLamp::Pole ) ? polePeak : hangPeak;
        if( peak <= 0.01f )
        {
            continue;
        }
        // Skip fully faded / non-rendered.
        if( mo->renderflags & RF_INVISIBLE )
        {
            continue;
        }
        if( mo->Alpha <= 0.01 )
        {
            continue;
        }

        // Hang: SPAWNCEILING, so actor Z is the bottom of the bbox (Top() touches the
        //       ceiling) and the bulb sits in the lower part of the fixture; zofs nudges
        //       further down.
        // Pole: floor-standing, bulb in the head at the top. zofs is NOT applied — it
        //       means "down from the bulb estimate", which on a pole walks the light
        //       into the shaft.
        const float px = float( mo->X() ) * ONEGAMEUNIT_IN_METERS;
        const float py = float( mo->Y() ) * ONEGAMEUNIT_IN_METERS;
        const float zBulb =
            ( kind == RtTechLamp::Pole )
                ? float( mo->Z() ) + float( mo->Height ) * float( cvar::rt_pole_lamp_zfrac )
                : float( mo->Z() ) + float( mo->Height ) * 0.35f - zOfs;
        const float pz = zBulb * ONEGAMEUNIT_IN_METERS;

        // Mild per-lamp phase so a dense hall doesn't hard-sync (no full blackout —
        // RR hates lights leaving the list; keep >= ~0.85).
        const int   phase = int( ( maptime * 3 + int( mo->X() ) + int( mo->Y() ) * 7 ) % 256 );
        const float flicker =
            ( phase < 6 ) ? 0.88f : ( phase >= 130 && phase < 134 ) ? 0.92f : 1.f;
        const float intensity = peak * flicker;

        // Stable ID from actor pointer (same pattern as dynlights).
        const uint64_t stableId =
            HangLampId_Base + ( uint64_t{ reinterpret_cast< uintptr_t >( mo ) } & 0xFFFFFFFFull );

        // Warm amber base — matches LMP bulb albedo better than pure white — tinted by
        // the hue of the sector the lamp actually hangs in (see RT_SectorHue).
        const sector_t* lampSector = mo->Sector;
        const FVector3  hue =
            lampSector ? RT_SectorHue( lampSector->Colormap.LightColor,
                                       float{ cvar::rt_sector_tint_lights } )
                       : FVector3{ 1.0f, 1.0f, 1.0f };

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( 1.000f * hue.X,
                                                    0.784f * hue.Y,
                                                    0.471f * hue.Z,
                                                    1.0f ),
            .intensity = intensity,
            .position  = { px, py, pz },
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

        if( cvar::rt_hang_lamp_debug )
        {
            auto markSph = RgLightSphericalEXT{
                .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                .pNext     = nullptr,
                .color     = rt.rgUtilPackColorByte4D( 255, 255, 0, 255 ),
                .intensity = 350.f,
                .position  = { px, py, pz },
                .radius    = 0.05f,
            };
            auto markInfo = RgLightInfo{
                .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                .pNext        = &markSph,
                .uniqueID     = stableId + 0x08000000ull,
                .isExportable = false,
            };
            r = rt.rgUploadLight( &markInfo );
            RG_CHECK( r );

            if( ( maptime % 35 ) == 0 && uploaded < 8 )
            {
                const char* sn = ( mo->sprite >= 0 && mo->sprite < sprites.Size() )
                                     ? sprites[ mo->sprite ].name
                                     : "?";
                Printf( "rt_hang_lamp: '%s'/%s xyz=(%.0f,%.0f,%.0f) I=%.0f\n",
                        sn,
                        mo->GetClass() ? mo->GetClass()->TypeName.GetChars() : "?",
                        float( mo->X() ),
                        float( mo->Y() ),
                        zBulb,
                        intensity );
            }
        }

        ++uploaded;
    }

    if( cvar::rt_hang_lamp_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            Printf( "rt_hang_lamp_debug: %u hanging tech lamp(s) uploaded\n", uploaded );
        }
    }
}

// Which Baron-family monster is this, if any? Both carry a magic glow on their fists:
// BOS2 the Hell Knight (green), BOSS the Baron of Hell (red). Sprite first, class name as
// the fallback — same order as RT_TechLampKind, because a sprite replacement can desync
// the two.
//
// Matched in FULL, never on a 'BOS' prefix: the two differ only in the 4th character and
// they need different colours, so a prefix match would silently give the Baron the Hell
// Knight's green.
static int RT_HandGlowMonster( AActor* mo )
{
    if( !mo )
    {
        return -1;
    }
    if( mo->sprite >= 0 && mo->sprite < int( sprites.Size() ) )
    {
        const char* sn = sprites[ mo->sprite ].name;
        if( sn && strnicmp( sn, "BOS2", 4 ) == 0 )
        {
            return RT_HAND_HELLKNIGHT;
        }
        if( sn && strnicmp( sn, "BOSS", 4 ) == 0 )
        {
            return RT_HAND_BARON;
        }
    }
    if( mo->GetClass() && mo->GetClass()->TypeName.IsValidName() )
    {
        const char* cn = mo->GetClass()->TypeName.GetChars();
        if( cn && strnicmp( cn, "64HellKnight", 12 ) == 0 )
        {
            return RT_HAND_HELLKNIGHT;
        }
        // "64BaronOfHell" — not a prefix of the Hell Knight's name, so order is safe here.
        if( cn && strnicmp( cn, "64BaronOfHell", 13 ) == 0 )
        {
            return RT_HAND_BARON;
        }
    }
    return -1;
}

void RT_UploadHandGlowLights()
{
    // The Hell Knight carries a green magic glow on its fists. That glow is texture
    // emissive, and RTGL1 emissive is never a light source (rt_wall_strips explains why),
    // so the only illumination was the sprite's attached light — which RTGL1 pins to the
    // CENTRE of the billboard quad. Result: light out of the torso, and the two fists
    // collapsed to one point between them. Here each fist gets its own analytic sphere at
    // its real body-relative position, taken from the authored brightmaps.
    if( !cvar::rt_hand_light_on || !primaryLevel )
    {
        return;
    }

    const float intensity = std::max( 0.f, float{ cvar::rt_hand_light_intensity } );
    if( intensity <= 0.01f )
    {
        return;
    }
    const float  srcRadius = std::max( 0.01f, float{ cvar::rt_hand_light_radius } );
    const double maxDist   = std::max( 64.0, double( float{ cvar::rt_hand_light_maxdist } ) );
    const double maxDist2  = maxDist * maxDist;
    const int    budget    = std::max( 0, int{ cvar::rt_hand_light_max } );
    if( budget == 0 )
    {
        return;
    }

    const DVector3 vpos = r_viewpoint.Pos;

    // Collect then trim nearest-first. Same shape as the ceiling-edge/solo systems: a
    // distance filter alone does not bound the count, and letting the far half win by
    // iteration order is what made distant lamps pop in there.
    struct HandCand
    {
        double   d2;
        float    px, py, pz;
        uint64_t id;
        int      monster; // index into RT_HAND_COLOR — green knight vs red baron
    };
    std::vector< HandCand > cand;

    auto    it = primaryLevel->GetThinkerIterator< AActor >();
    AActor* mo = nullptr;
    while( ( mo = it.Next() ) != nullptr )
    {
        const int monster = RT_HandGlowMonster( mo );
        if( monster < 0 )
        {
            continue;
        }
        if( mo->renderflags & RF_INVISIBLE )
        {
            continue;
        }
        if( mo->Alpha <= 0.01 )
        {
            continue;
        }
        // Frames I..N are death/gib. They have no entry in the table and must not light:
        // the gore reuses the hand glow's own palette ramp, so a lit corpse would flare.
        const int frame = mo->frame;
        if( frame < 0 || frame >= RT_HAND_FRAME_COUNT )
        {
            continue;
        }
        const RtHandFrame& hk = RT_HAND_FRAMES[ monster ][ frame ];
        if( hk.count <= 0 )
        {
            continue;
        }

        // Offsets are body-relative, so rotate them by the actor's facing. Doom angle 0
        // is +X; "right of facing" is yaw - 90 degrees.
        const double yaw = mo->Angles.Yaw.Radians();
        const double fx = std::cos( yaw ), fy = std::sin( yaw );
        const double rx = std::sin( yaw ), ry = -std::cos( yaw );

        for( int h = 0; h < hk.count && h < 2; ++h )
        {
            const RtHandPos& hand = hk.hands[ h ];

            const double wx = double( mo->X() ) + rx * hand.lateral + fx * hand.fwd;
            const double wy = double( mo->Y() ) + ry * hand.lateral + fy * hand.fwd;
            const double wz = double( mo->Z() ) + hand.up;

            const double dx = wx - vpos.X, dy = wy - vpos.Y, dz = wz - vpos.Z;
            const double d2 = dx * dx + dy * dy + dz * dz;
            if( d2 > maxDist2 )
            {
                continue;
            }

            // Stable across frames: actor identity + hand index. An id that shifted per
            // tick would make RTGL1 see the set vanish and reappear.
            const uint64_t id = HandLightId_Base +
                                ( ( uint64_t{ reinterpret_cast< uintptr_t >( mo ) } &
                                    0xFFFFFFFFull )
                                  << 1 ) +
                                uint64_t( h );

            cand.push_back( HandCand{
                d2,
                float( wx ) * ONEGAMEUNIT_IN_METERS,
                float( wy ) * ONEGAMEUNIT_IN_METERS,
                float( wz ) * ONEGAMEUNIT_IN_METERS,
                id,
                monster,
            } );
        }
    }

    const size_t wanted = cand.size();
    if( wanted > size_t( budget ) )
    {
        std::partial_sort( cand.begin(),
                           cand.begin() + budget,
                           cand.end(),
                           []( const HandCand& a, const HandCand& b ) { return a.d2 < b.d2; } );
        cand.resize( size_t( budget ) );
    }

    for( const HandCand& c : cand )
    {
        // Colour comes from the generated table, never a literal here: it is the same
        // value the mask generator tints with, and a hardcoded copy would drift the cast
        // light away from the glow the moment either was retuned.
        const unsigned rgb = RT_HAND_COLOR[ c.monster ];
        const float    kR  = ( ( rgb >> 16 ) & 0xFF ) / 255.0f;
        const float    kG  = ( ( rgb >> 8 ) & 0xFF ) / 255.0f;
        const float    kB  = ( rgb & 0xFF ) / 255.0f;

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( kR, kG, kB, 1.0f ),
            .intensity = intensity,
            .position  = { c.px, c.py, c.pz },
            .radius    = srcRadius,
        };
        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sph,
            .uniqueID     = c.id,
            .isExportable = false,
        };
        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        if( cvar::rt_hand_light_debug )
        {
            auto markSph = RgLightSphericalEXT{
                .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                .pNext     = nullptr,
                .color     = rt.rgUtilPackColorByte4D( 255, 0, 255, 255 ),
                .intensity = 350.f,
                .position  = { c.px, c.py, c.pz },
                .radius    = 0.05f,
            };
            auto markInfo = RgLightInfo{
                .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                .pNext        = &markSph,
                .uniqueID     = c.id + ( 1ull << 41 ),
                .isExportable = false,
            };
            r = rt.rgUploadLight( &markInfo );
            RG_CHECK( r );
        }
    }

    if( cvar::rt_hand_light_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            Printf( "rt_hand_light: uploaded=%zu of %zu wanted (cap %d, within %.0fu) I=%.0f\n",
                    cand.size(),
                    wanted,
                    budget,
                    maxDist,
                    intensity );
        }
    }
}

// Every open flame in the game, keyed by sprite name.
//
// `up` and the relative brightness are the mod's OWN authored intent, lifted from its
// GLDEFS `flickerlight` blocks (offset 0 N 0 / size N), so nothing here is invented:
//
//   CANDLE          size 16  offset 0 16 0     REDFIRE etc.     size 32  offset 0  8 0
//   *TORCH (wall)   size 28  offset 0 24 0     TORCHSHORT*      size 40  offset 0 64 0
//                                              TORCHLONG*       size 40  offset 0 80 0
//
// Colours do NOT come from GLDEFS, which asks for fully-primary hues (0.0 1.0 0.0 green,
// 1.0 0.1 0.1 red). Those are the shared flame palette instead — the same four hexes the
// _e.png mask generators tint with (tools/gen_torch_emissives.py, gen_fx_emissives.py).
// Keeping cast light and on-screen glow on one palette is the whole point: they drifted
// apart once already (the LPUF regression), and a literal here would let it happen again.

