// Effect lights: the ones that flicker, animate or follow a live actor.
//
//   RT_UploadSwitchLights - lit switch faces, keyed on sidedef index so the id
//                           is stable across frames (a moving id makes RTGL1
//                           throw away its temporal reservoirs)
//   RT_UploadLavaLights   - a grid of lights over each lava sector
//   RT_UploadFlameLights  - torches, fires and candles, per RT_FLAME_KINDS
//
// Split out of rt_main.cpp. Behaviour unchanged; this is a move.

#include "rt_internal.h"

#include <array>

// The shared internals (RG_CHECK, ONEGAMEUNIT_IN_METERS, RT_SectorHue, the
// light-ID bases) come in unqualified, exactly as when this code lived inside
// rt_main.cpp's anonymous namespace.
using namespace rtx;

struct RtFlameKind
{
    char     sprite[ 5 ];
    unsigned rgb;
    float    up;        // map units above the actor origin, from GLDEFS `offset 0 N 0`
    float    intensity; // RT intensity, scaled by GLDEFS `size` relative to the others
};

constexpr unsigned RT_FLAME_BLUE   = 0x4488FF;
constexpr unsigned RT_FLAME_GREEN  = 0x44FF66;
constexpr unsigned RT_FLAME_RED    = 0xFF4020;
constexpr unsigned RT_FLAME_YELLOW = 0xFFCC33;
// The candle is deliberately NOT FLAME_YELLOW. A candle is a single wick, not a pitch
// torch: it should read as a dim red ember at the edge of a dark room, so it takes a
// warm red of its own at a fraction of the intensity.
constexpr unsigned RT_FLAME_CANDLE = 0xFF4A14;
// 64BigFire's orange, NOT FLAME_YELLOW. GLDEFS BIGFIRE asks for 1.0 0.9 0.0, but this is
// the one flame whose glow tint was picked off the art rather than the palette
// (gen_fx_emissives.py forces "ff8020" on the FIRE _e mask), and cast light must follow
// the mask or the two drift apart — the LPUF regression again.
constexpr unsigned RT_FLAME_BIGFIRE = 0xFF8020;

constexpr RtFlameKind RT_FLAME_KINDS[] = {
    // standing torches, long (27x100) — GLDEFS TORCHLONG*
    { "TLBL", RT_FLAME_BLUE, 80.f, 900.f },
    { "TLGR", RT_FLAME_GREEN, 80.f, 900.f },
    { "TLRD", RT_FLAME_RED, 80.f, 900.f },
    { "TLYL", RT_FLAME_YELLOW, 80.f, 900.f },
    // standing torches, short (18x85) — GLDEFS TORCHSHORT*, same size, lower offset
    { "TSBL", RT_FLAME_BLUE, 64.f, 900.f },
    { "TSGR", RT_FLAME_GREEN, 64.f, 900.f },
    { "TSRD", RT_FLAME_RED, 64.f, 900.f },
    { "TSYL", RT_FLAME_YELLOW, 64.f, 900.f },
    // wall sconces — GLDEFS size 28, so below the standing torches
    { "A030", RT_FLAME_YELLOW, 24.f, 700.f },
    { "A031", RT_FLAME_BLUE, 24.f, 700.f },
    { "A032", RT_FLAME_RED, 24.f, 700.f },
    { "GTCH", RT_FLAME_GREEN, 24.f, 700.f },
    // loose fires burning on the floor — GLDEFS size 32, offset only 8 up
    { "BFLM", RT_FLAME_BLUE, 8.f, 650.f },
    { "GFLM", RT_FLAME_GREEN, 8.f, 650.f },
    { "RFLM", RT_FLAME_RED, 8.f, 650.f },
    { "YFLM", RT_FLAME_YELLOW, 8.f, 650.f },
    // 64BigFire (32x50), the bonfire — GLDEFS BIGFIRE, size 32 like the loose fires but
    // offset 32 up, so it is the one flame here whose GLDEFS offset lands ABOVE the
    // sprite's own midpoint (~25u). This row was missing until 2026-08-10 on the stated
    // grounds that FIRE "is not a GLDEFS flame prop"; the WAD's GLDEFS says otherwise, and
    // at 117 placements across nine maps it is by far the most common fire in the game.
    // The sprite is shared with 64MotherFire and 64MotherFireTrail, which GLDEFS also
    // binds to BIGFIRE, so the projectile and its trail are lit by this row too.
    { "FIRE", RT_FLAME_BIGFIRE, 32.f, 650.f },
    // candle — GLDEFS size 16, the smallest flame in the game
    { "CAND", RT_FLAME_CANDLE, 16.f, 260.f },
};

static const RtFlameKind* RT_FlameKindOf( AActor* mo )
{
    if( !mo || mo->sprite < 0 || mo->sprite >= int( sprites.Size() ) )
    {
        return nullptr;
    }
    const char* sn = sprites[ mo->sprite ].name;
    if( !sn )
    {
        return nullptr;
    }
    // Full 4-character match, never a prefix. TL/TS families differ only in characters
    // 3-4, and A030/A031/A032 only in the last, so a prefix match would hand three wall
    // torches the same colour.
    for( const RtFlameKind& k : RT_FLAME_KINDS )
    {
        if( strnicmp( sn, k.sprite, 4 ) == 0 )
        {
            return &k;
        }
    }
    return nullptr;
}

// The flame offset, for rt_smoke.cpp's ambient emitters.
//
// Smoke must rise from where the FLAME is, and the flame is not at the actor's
// origin: a long torch's fire sits 80 map units up, a wall sconce's 24, a floor
// fire's 8. Those numbers are GLDEFS's and they are already right here, so smoke
// asks rather than keeping a second copy -- the light and the smoke coming off
// one point is the same property the muzzle flash and its smoke have.
bool rtx::RT_FlameSpriteOffset( AActor* mo, float* upMapUnits )
{
    if( const RtFlameKind* k = RT_FlameKindOf( mo ) )
    {
        if( upMapUnits )
        {
            *upMapUnits = k->up;
        }
        return true;
    }
    return false;
}

// A thrown switch changes its own texture (ANIMDEFS: CMPSW##A -> ON -> CMPSW##B) and the
// new art has lit eyes or a lit gem. Nothing else needs to happen for this to be correct
// across a save, a level reset or a switch thrown back off: the walk reads
// side->GetTexture(), which IS the swapped texture, so the light exists exactly while the
// lit face is on the wall. There is no state to track and nothing to keep in sync.
//
// Which frame is "on" is not the A/B letter -- SWXCL and SWXCKL light on A, their eyes go
// OUT when pressed -- but that is settled in the generated table, which is built from
// which textures actually carry an emissive mask.
static const RtSwitchLight* RT_SwitchLightFor( FGameTexture* gtex )
{
    if( !gtex )
    {
        return nullptr;
    }
    // Built once. 67 rows is small enough that a linear scan per sidedef part per frame
    // would still be cheap, but this runs 3x over every sidedef in the level every frame.
    static const std::unordered_map< std::string, const RtSwitchLight* > lookup = [] {
        std::unordered_map< std::string, const RtSwitchLight* > m;
        for( const RtSwitchLight& s : RT_SWITCH_LIGHTS )
        {
            m.emplace( s.tex, &s );
        }
        return m;
    }();

    const char* nm = gtex->GetName().GetChars();
    if( !nm || !*nm )
    {
        // A texture with NO name is not a non-event, and treating it as one is
        // what hid this for a whole round of testing: Unseen Evil's plain pk3
        // art (textures/pepy/..., textures/cage/...) reaches the renderer
        // nameless, so a table keyed on names can never match it and a census
        // that skips nameless textures reports the map as empty. Under
        // rt_switch_light_debug, say so and name the FILE instead.
        if( bool{ cvar::rt_switch_light_debug } )
        {
            static std::unordered_set< int > s_noname;
            const int lump = gtex->GetSourceLump();
            if( s_noname.insert( lump ).second )
            {
                const char* file = fileSystem.GetFileFullName( lump, false );
                Printf( RT_DiagPrintLevel(),
                        "RT UE fixture: <unnamed> lump %d = %s\n",
                        lump,
                        file ? file : "?" );
            }
        }
        return nullptr;
    }
    std::string key = nm;
    for( char& c : key )
    {
        c = char( toupper( (unsigned char)c ) );
    }
    auto it = lookup.find( key );
    return it == lookup.end() ? nullptr : it->second;
}

// The same question for Doom 64: Unseen Evil, against its own generated table.
//
// A SEPARATE TABLE, not extra rows in RT_SWITCH_LIGHTS, and that is deliberate:
// tools/gen_switch_lights.py rewrites rt_switch_lights.h wholesale from
// Retribution's rt/mat masks, so any Unseen Evil row parked there would be
// silently destroyed the next time anyone ran it.
//
// It also covers more than switches. Unseen Evil paints three kinds of lit wall
// face -- a door indicator that pulses on a 4-frame ANIMDEFS ping-pong, a switch
// lamp that comes on when thrown, and a steady light strip -- and none of them
// needs its mechanism modelled here, because the caller asks TexMan for the
// texture being drawn RIGHT NOW. A frame with no row emits nothing, so the blink,
// the throw and the steady burn all fall out of this one lookup.
//
// `scaleOut` is the part RtSwitchLight cannot express: a door's mid frame is the
// same lamp at part brightness, and without it a three-step pulse would collapse
// to on/off.
// One lit band of one Unseen Evil fixture.
struct UeHit
{
    const RtSwitchLight* row;
    float                scale; // this frame's share of the fixture's full brightness
    bool                 band;  // tiling wall strip: light it as a CHAIN, not one sphere
    int                  x0, x1, y0, y1; // lit extent in texels -- a strip's long axis
};

static const std::vector< UeHit >* RT_UeFixtureLightFor( FGameTexture* gtex )
{
    static const std::vector< UeHit > none;
    if( !gtex || !bool{ cvar::rt_ue_fixture_lights } || !RT_IsUnseenEvil() )
    {
        return &none;
    }
    // Built once, and the RtSwitchLight views are built once WITH it: the walk
    // stores a pointer into `cand` that outlives this call, so these have to have
    // static storage duration and a stable address. A vector never touched after
    // construction gives both.
    struct UeTable
    {
        std::vector< RtSwitchLight >                            rows;
        std::vector< float >                                    scale;
        std::vector< bool >                                     band;
        std::vector< std::array< int, 4 > >                     ext;
        // A VECTOR PER KEY, not one entry. Twelve of these fixtures carry TWO lit
        // bands -- d64_widedoor's gem AND the bar below it, d64_silver2's two rows
        // of rectangles, d64_d1liteblu1's two lines -- and an unordered_map::emplace
        // per row silently keeps only the first. That is not a missing feature, it
        // is half of a dozen fixtures going dark with no error anywhere: "some door
        // green lights are ok, some not".
        std::unordered_map< std::string, std::vector< size_t > > lookup;
    };
    static const UeTable table = [] {
        UeTable t;
        t.rows.reserve( RT_UE_FIXTURE_LIGHT_COUNT );
        for( const RtUeFixtureLight& f : RT_UE_FIXTURE_LIGHTS )
        {
            t.rows.push_back( RtSwitchLight{ f.tex, f.tw, f.th, f.cx, f.cy, f.lit, f.rgb } );
            t.scale.push_back( f.scale );
            t.band.push_back( f.band );
            t.ext.push_back( { f.x0, f.x1, f.y0, f.y1 } );
            t.lookup[ f.tex ].push_back( t.rows.size() - 1 );
        }
        return t;
    }();

    // MOST OF THIS MOD'S ART HAS NO TEXTURE NAME AT ALL, and that is the whole
    // reason this function does not simply mirror RT_SwitchLightFor.
    //
    // GZDoom only fills FGameTexture::Name for a pk3 texture whose basename fits
    // the classic 8 characters. Unseen Evil's own art does not: `C201` and
    // `SPACEAU` out of textures/d64/ arrive named, but d64_metal7,
    // d64_talldoor_2 and d64_greydoor_0 arrive with an EMPTY name and are
    // reachable only through the file they came from. Keying on the name alone
    // matched exactly the TEXTURES composites and silently missed every plain
    // PNG -- which is most of the doors and every light strip. The generated
    // table carries both forms per fixture for this reason.
    //
    // Cached per FGameTexture, misses included: GetFileFullName is a filesystem
    // call and this runs three times per sidedef per frame.
    static std::unordered_map< const FGameTexture*, std::vector< UeHit > > s_cache;
    auto cached = s_cache.find( gtex );
    if( cached != s_cache.end() )
    {
        return &cached->second;
    }

    auto upper = []( const char* p ) {
        std::string k = p ? p : "";
        for( char& c : k )
        {
            c = char( toupper( (unsigned char)c ) );
        }
        return k;
    };

    std::vector< UeHit > found;
    std::string          shown;

    auto collect = [ & ]( const std::string& key ) {
        auto it = table.lookup.find( key );
        if( it == table.lookup.end() )
        {
            return;
        }
        for( size_t idx : it->second )
        {
            found.push_back( UeHit{ &table.rows[ idx ],
                                    table.scale[ idx ],
                                    table.band[ idx ],
                                    table.ext[ idx ][ 0 ],
                                    table.ext[ idx ][ 1 ],
                                    table.ext[ idx ][ 2 ],
                                    table.ext[ idx ][ 3 ] } );
        }
    };

    const char* nm = gtex->GetName().GetChars();
    if( nm && *nm )
    {
        shown = upper( nm );
        collect( shown );
    }
    if( found.empty() )
    {
        const int   lump = gtex->GetSourceLump();
        const char* file = lump >= 0 ? fileSystem.GetFileFullName( lump, false ) : nullptr;
        if( file && *file )
        {
            const std::string key = upper( file );
            if( shown.empty() )
            {
                shown = key;
            }
            collect( key );
        }
    }

    // One line per distinct texture per session, hit OR miss, like the key-trim
    // tagging in rt_lights_fixtures.cpp and for the same reason: without it "this
    // map has no fixtures" and "the fixtures are there and every lookup missed"
    // produce identical output, and that ambiguity has already cost this feature
    // one full round of testing. Misses are limited to names that look like this
    // mod's art so a Retribution log cannot be spammed -- unless
    // rt_switch_light_debug is on, which lists every wall texture in the level
    // and is the only view that can answer "what does this arrive as".
    static std::unordered_set< std::string > s_seen;
    const bool looksUe = shown.rfind( "D64_", 0 ) == 0 || shown.find( "/D64_" ) != std::string::npos;
    if( ( !found.empty() || looksUe || bool{ cvar::rt_switch_light_debug } ) && !shown.empty() &&
        s_seen.insert( shown ).second )
    {
        Printf( RT_DiagPrintLevel(),
                "RT UE fixture: %s %s\n",
                shown.c_str(),
                found.empty() ? "no row (not a fixture)"
                              : ( found.size() > 1 ? "LIT (multi-band)" : "LIT" ) );
    }

    return &s_cache.emplace( gtex, std::move( found ) ).first->second;
}

void RT_UploadSwitchLights()
{
    // Either family keeps the walk alive: Unseen Evil's fixture table rides this
    // same walk (see RT_UeFixtureLightFor), and gating the whole thing on
    // rt_switch_lights alone would make rt_ue_fixture_lights silently do nothing
    // for anyone who turned Retribution's switches off.
    const bool wantSwitches = bool{ cvar::rt_switch_lights };
    const bool wantUe       = bool{ cvar::rt_ue_fixture_lights } && RT_IsUnseenEvil();
    if( ( !wantSwitches && !wantUe ) || !primaryLevel )
    {
        return;
    }
    const float baseI = std::max( 0.f, float{ cvar::rt_switch_light_intensity } );
    // Unseen Evil's fixtures bring their own slots. See rt_ue_fixture_max: a strip
    // is a chain, so one corridor can want more lights than the whole switch budget.
    const int   budget = std::max( 0, int{ cvar::rt_switch_light_max } ) +
                       ( wantUe ? std::max( 0, int{ cvar::rt_ue_fixture_max } ) : 0 );
    if( baseI <= 0.01f || budget == 0 )
    {
        return;
    }

    const float  srcRadius = std::max( 0.01f, float{ cvar::rt_switch_light_radius } );
    const double wallOfs   = double( float{ cvar::rt_switch_light_ofs } );
    const double zNudge    = double( float{ cvar::rt_switch_light_zofs } );
    const double maxDist   = std::max( 64.0, double( float{ cvar::rt_switch_light_maxdist } ) );
    const double maxDist2  = maxDist * maxDist;
    const bool   dbg       = bool{ cvar::rt_switch_light_debug };

    struct SwCand
    {
        double              d2;
        double              x, y, z;
        const RtSwitchLight* sw;
        uint64_t            id;
        const char*         why;   // debug only: which pegging branch placed it
        float               scale; // this frame's share of the fixture's full brightness
        bool                ue;    // from the Unseen Evil table, which is sized differently
        bool                band;  // one link of a strip chain, not a lone fixture
    };
    std::vector< SwCand > cand;

    const DVector3 vpos = r_viewpoint.Pos;

    for( unsigned i = 0; i < primaryLevel->lines.Size(); i++ )
    {
        const line_t& line = primaryLevel->lines[ i ];
        if( !line.v1 || !line.v2 )
        {
            continue;
        }
        const double x1 = line.v1->fX(), y1 = line.v1->fY();
        const double x2 = line.v2->fX(), y2 = line.v2->fY();
        const double lineLen = std::hypot( x2 - x1, y2 - y1 );
        if( lineLen < 1.0 )
        {
            continue;
        }

        for( int s = 0; s < 2; s++ )
        {
            side_t* side = line.sidedef[ s ];
            if( !side || !side->sector )
            {
                continue;
            }
            const sector_t* thisSec   = side->sector;
            const side_t*   otherSide = line.sidedef[ 1 - s ];
            const sector_t* otherSec  = otherSide ? otherSide->sector : nullptr;

            for( int part = 0; part < 3; part++ )
            {
                // ANIMATED, and that is load-bearing for both tables: this is the
                // frame on the wall this instant, so a door's indicator, a thrown
                // switch and a steady strip all resolve without any state here.
                FGameTexture* gtex = TexMan.GetGameTexture( side->GetTexture( part ), true );

                // One texture can carry SEVERAL lit bands, so this is a list, not a
                // hit: d64_widedoor has a gem near the top and a bar near the floor,
                // and lighting only the first of them is what "some door green lights
                // are ok, some not" looks like from inside the game.
                std::vector< UeHit > hits;
                bool                 fromUe = false;
                if( const RtSwitchLight* d64 = wantSwitches ? RT_SwitchLightFor( gtex ) : nullptr )
                {
                    hits.push_back( UeHit{ d64, 1.0f, false, 0, 0, 0, 0 } );
                }
                else
                {
                    hits   = *RT_UeFixtureLightFor( gtex );
                    fromUe = !hits.empty();
                }
                if( hits.empty() )
                {
                    continue;
                }

                for( size_t hi = 0; hi < hits.size(); hi++ )
                {
                const RtSwitchLight* sw       = hits[ hi ].row;
                const float          rowScale = hits[ hi ].scale;
                const bool           isBand   = hits[ hi ].band;
                const bool           isUe     = fromUe;
                const UeHit&         hit      = hits[ hi ];

                // Vertical band this part covers, same derivation as the wall strips.
                const DVector2 mid{ ( x1 + x2 ) * 0.5, ( y1 + y2 ) * 0.5 };
                double         zLow = 0.0, zHigh = 0.0;
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
                    continue;
                }

                // World units per texel. Doom 64 Retribution authors these at 1.0, but a
                // scaled sidedef would otherwise put the light at a fraction of the right
                // height and look like a pegging bug rather than a scale one.
                const double sx = side->GetTextureXScale( part );
                const double sy = side->GetTextureYScale( part );
                const double wptX = ( std::abs( sx ) > 1e-6 ) ? 1.0 / sx : 1.0;
                const double wptY = ( std::abs( sy ) > 1e-6 ) ? 1.0 / sy : 1.0;

                // Where the TOP of the texture sits in world Z. This is the part that is a
                // convention rather than a measurement -- see rt_switch_light_zofs.
                const bool  pegTop = ( line.flags & ML_DONTPEGTOP ) != 0;
                const bool  pegBot = ( line.flags & ML_DONTPEGBOTTOM ) != 0;
                const double texH  = double( sw->th ) * wptY;
                double       zTexTop;
                const char*  why;
                if( part == side_t::top )
                {
                    // Unpegged upper: texture hangs from this sector's ceiling. Pegged:
                    // it rises from the bottom of the upper band.
                    zTexTop = pegTop ? zHigh : ( zLow + texH );
                    why     = pegTop ? "top/pegTop" : "top/bottomup";
                }
                else if( part == side_t::bottom )
                {
                    // Unpegged lower takes its origin from the CEILING of this sector,
                    // not from the step it is drawn on -- that is the whole point of the
                    // flag, and getting it backwards is what makes a lower texture appear
                    // to slide when a floor moves.
                    zTexTop = pegBot ? thisSec->ceilingplane.ZatPoint( mid ) : zHigh;
                    why     = pegBot ? "bot/pegBot" : "bot/bandtop";
                }
                else
                {
                    zTexTop = pegBot ? ( zLow + texH ) : zHigh;
                    why     = pegBot ? "mid/pegBot" : "mid/bandtop";
                }
                zTexTop -= side->GetTextureYOffset( part );

                const double z = zTexTop - double( sw->cy ) * wptY + zNudge;
                // A switch whose lit face is outside the band it is drawn in means the
                // pegging branch above guessed wrong for this sidedef. Drop it rather than
                // put a red light inside a wall, where it is invisible and indistinguishable
                // from the feature not working.
                if( z < zLow - 8.0 || z > zHigh + 8.0 )
                {
                    continue;
                }

                // Horizontal: the texture repeats every tw texels along the line, so solve
                // for every column at which the lit centroid lands inside this line.
                const double texW = double( sw->tw ) * wptX;
                if( texW < 1.0 )
                {
                    continue;
                }
                const double xoff = side->GetTextureXOffset( part );
                double       d0   = std::fmod( double( sw->cx ) * wptX - xoff, texW );
                if( d0 < 0.0 )
                {
                    d0 += texW;
                }

                const double ux = ( x2 - x1 ) / lineLen;
                const double uy = ( y2 - y1 ) / lineLen;
                // Outward normal decided against the sector centre, NOT against Doom's
                // winding: getting that convention backwards buries every light 2 units
                // inside solid geometry, which looks exactly like uploading nothing. Same
                // trap, same fix, as RT_UploadWallStripLights.
                const double nx = -uy, ny = ux;
                const double towardX = double( thisSec->centerspot.X ) - ( x1 + x2 ) * 0.5;
                const double towardY = double( thisSec->centerspot.Y ) - ( y1 + y2 ) * 0.5;
                const double ofs = ( nx * towardX + ny * towardY ) >= 0.0 ? wallOfs : -wallOfs;

                // A STRIP IS LIT ALONG ITS LONG AXIS, AND NEVER WITH NOTHING.
                //
                // A door or a switch is ONE object on one sidedef: it goes where the
                // texture puts it and repeats only where the texture repeats. A strip
                // is wall art with an EXTENT, and the extent says how to light it.
                //
                //   d64_metal7      a 64x7 bar along the bottom  -> a row along the wall
                //   d64_lite_thin   a 16x128 tube                 -> a column UP the wall
                //
                // The first chain ran along the line for every strip, and on MAP01's
                // two lite_thin sidedefs -- both 16 units long, both one-sided -- it
                // began half a segment in (24 units) and so placed NOTHING on a line
                // shorter than that. The strips that had one light each before the
                // chain had zero after it; "nothing shows up" was exact.
                //
                // So: rows up the wall = the band's height over the segment length,
                // at least one. Along the wall, a band that spans the texture's width
                // is a continuous chain spread evenly over the line, at least one; a
                // narrower band sits at its own column once per texture repeat.
                //
                // RTGL1 offers nothing better than a chain of overlapping spheres. An
                // emissive surface casts no light at all -- rt_wall_strips says so in
                // as many words -- and RgLightPolygonalEXT is compiled out behind
                // #if TRIANGLE_LIGHTS and hard-errors on upload.
                const double step = isBand
                                        ? std::max( 8.0, double( float{ cvar::rt_ue_strip_seglen } ) )
                                        : texW;

                double along[ 64 ];
                double zrow[ 16 ];
                int    nH = 0, nV = 0;
                if( !isBand )
                {
                    for( double d = d0; d <= lineLen && nH < 4; d += texW )
                    {
                        along[ nH++ ] = d;
                    }
                    zrow[ nV++ ] = z;
                }
                else
                {
                    const double extW = double( hit.x1 - hit.x0 + 1 );
                    const double extH = double( hit.y1 - hit.y0 + 1 );
                    nV = std::clamp( int( std::lround( extH * wptY / step ) ), 1, 16 );
                    for( int r = 0; r < nV; r++ )
                    {
                        const double ty = double( hit.y0 ) + extH * ( double( r ) + 0.5 ) / double( nV );
                        zrow[ r ]       = zTexTop - ty * wptY + zNudge;
                    }
                    // Both counts are bounded so their product stays inside the 128 id
                    // slots a lamp owns per sidedef -- an id that wraps onto another
                    // light's is an id that moves, and RTGL1 answers that by throwing
                    // its temporal history away.
                    const int hMax = std::max( 1, 128 / nV );
                    if( extW * wptX >= 0.75 * texW )
                    {
                        nH = std::clamp( int( std::lround( lineLen / step ) ), 1, std::min( 64, hMax ) );
                        for( int k = 0; k < nH; k++ )
                        {
                            along[ k ] = lineLen * ( double( k ) + 0.5 ) / double( nH );
                        }
                    }
                    else
                    {
                        for( double d = d0; d <= lineLen && nH < std::min( 64, hMax ); d += texW )
                        {
                            along[ nH++ ] = d;
                        }
                    }
                }

                int rep = 0;
                for( int hI = 0; hI < nH; hI++ )
                {
                for( int vI = 0; vI < nV; vI++, rep++ )
                {
                    const double d  = along[ hI ];
                    const double zz = zrow[ vI ];
                    if( zz < zLow - 8.0 || zz > zHigh + 8.0 )
                    {
                        continue;
                    }
                    const double px = x1 + ux * d + nx * ofs;
                    const double py = y1 + uy * d + ny * ofs;
                    const double dx = px - vpos.X, dy = py - vpos.Y, dz = zz - vpos.Z;
                    const double d2 = dx * dx + dy * dy + dz * dz;
                    if( d2 > maxDist2 )
                    {
                        continue;
                    }
                    cand.push_back( SwCand{
                        d2,
                        px,
                        py,
                        zz,
                        sw,
                        // Sidedef, part, WHICH LAMP and repeat all fold in, so two
                        // switches on one line never collide, a fixture with several
                        // lamps cannot overwrite itself, and the id never moves between
                        // frames -- a moving id makes RTGL1 throw away its temporal
                        // reservoirs, which is the whole reason this is packed at all.
                        //
                        // 4096 slots per sidedef: part (3) x lamp (8) x repeat (128).
                        // Widened twice, and the second time was a NEAR MISS worth
                        // recording -- when lamps began splitting horizontally a lever
                        // grew to three, and the previous `hi & 3` would have wrapped a
                        // fifth lamp onto the first one's id. SwitchLightId_Base has
                        // 2^46 of room under LavaLightId_Base at 2^47 and the widest
                        // level here uses about 2^28 of it, so the space is free.
                        SwitchLightId_Base + ( uint64_t( side->Index() ) * 4096ull ) +
                            uint64_t( part ) * 1024ull + uint64_t( hi & 7 ) * 128ull +
                            uint64_t( rep & 127 ),
                        why,
                        rowScale,
                        isUe,
                        isBand,
                    } );
                }
                }
                } // per lit band
            }
        }
    }

    const size_t wanted = cand.size();
    if( wanted > size_t( budget ) )
    {
        std::partial_sort( cand.begin(),
                           cand.begin() + budget,
                           cand.end(),
                           []( const SwCand& a, const SwCand& b ) { return a.d2 < b.d2; } );
        cand.resize( size_t( budget ) );
    }

    for( const SwCand& c : cand )
    {
        // sqrt, not linear: the SWXSG gem is 46 lit texels against the SWXC eyes' 7, and
        // a linear scale would hand it 6.5x the intensity and turn a gem into a spotlight.
        //
        // UNSEEN EVIL'S FIXTURES ARE NORMALISED AGAINST A DIFFERENT REFERENCE, and
        // they have to be. Retribution's rows run 7 to 70 lit texels because every
        // one is a switch face; Unseen Evil's run 17 to 3328, because a full-height
        // light strip is a fixture too. Through the /7 rule d64_lite_wide comes out
        // at 21.8x baseI -- a corridor lamp brighter than the sun. So its family is
        // measured against a door indicator's ~64 texels and CAPPED at 3x: past a
        // point a longer strip is a WIDER emitter, not a fiercer one, and the sphere
        // standing in for it should not pretend otherwise.
        // A BAND SEGMENT DOES NOT GET THE AREA SCALE, and must not: on a chain the
        // fixture's size is already expressed by HOW MANY segments it emits. Scaling
        // each one by the strip's total lit area as well counts the same length of
        // tube twice and turns a corridor into a searchlight -- the same reasoning
        // rt_wall_strip_intensity encodes by being a flat per-segment number.
        const float areaScale =
            c.band ? 1.0f
                   : c.ue ? std::clamp( std::sqrt( float( std::max( 1, c.sw->lit ) ) / 64.0f ),
                                        0.5f,
                                        3.0f )
                          : std::sqrt( float( std::max( 1, c.sw->lit ) ) / 7.0f );
        // c.scale is the frame's own brightness: a door's mid frame is the same lamp
        // turned down, and without it the three-step pulse the art paints would
        // arrive as a two-step on/off.
        const float I = ( c.band ? std::max( 0.f, float{ cvar::rt_ue_strip_intensity } )
                          : c.ue ? std::max( 0.f, float{ cvar::rt_ue_fixture_intensity } )
                                 : baseI ) *
                        areaScale * c.scale;
        // A wide soft source is what makes discrete spheres read as one band instead
        // of a row of dots -- the same 0.35 m rt_wall_strip_radius settled on, against
        // the 0.06 m a switch gem wants.
        const float radius = c.band ? std::max( 0.01f, float{ cvar::rt_ue_strip_radius } )
                             : c.ue ? std::max( 0.01f, float{ cvar::rt_ue_fixture_radius } )
                                    : srcRadius;

        float kR = ( ( c.sw->rgb >> 16 ) & 0xFF ) / 255.0f;
        float kG = ( ( c.sw->rgb >> 8 ) & 0xFF ) / 255.0f;
        float kB = ( c.sw->rgb & 0xFF ) / 255.0f;

        // Optional extra saturation on an Unseen Evil fixture, in HSV so the hue
        // cannot move -- pulling the two lesser channels toward the dominant one
        // is exactly S scaling, without the trig. See rt_ue_fixture_sat for why a
        // measured-correct lamp colour can still read wrong in the room.
        if( c.ue )
        {
            const float sat = std::max( 0.f, float{ cvar::rt_ue_fixture_sat } );
            if( std::abs( sat - 1.0f ) > 0.001f )
            {
                const float mx = std::max( { kR, kG, kB } );
                if( mx > 0.001f )
                {
                    auto pull = [ & ]( float v ) {
                        return std::clamp( mx - ( mx - v ) * sat, 0.f, mx );
                    };
                    kR = pull( kR );
                    kG = pull( kG );
                    kB = pull( kB );
                }
            }
        }

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( kR, kG, kB, 1.0f ),
            .intensity = I,
            .position  = { float( c.x ) * ONEGAMEUNIT_IN_METERS,
                           float( c.y ) * ONEGAMEUNIT_IN_METERS,
                           float( c.z ) * ONEGAMEUNIT_IN_METERS },
            .radius    = radius,
        };
        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sph,
            .uniqueID     = c.id,
            .isExportable = false,
        };
        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        if( dbg )
        {
            auto markSph = RgLightSphericalEXT{
                .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                .pNext     = nullptr,
                .color     = rt.rgUtilPackColorByte4D( 0, 255, 255, 255 ),
                .intensity = float{ cvar::rt_light_mark_intensity },
                .position  = sph.position,
                .radius    = 0.04f,
            };
            auto markInfo = RgLightInfo{
                .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
                .pNext        = &markSph,
                .uniqueID     = c.id + ( 1ull << 41 ),
                .isExportable = false,
            };
            RgResult mr = rt.rgUploadLight( &markInfo );
            RG_CHECK( mr );
        }
    }

    if( dbg )
    {
        static int frame = 0;
        if( ( frame++ % 60 ) == 0 )
        {
            Printf( "rt_switch_light: uploaded=%d of %d wanted (cap %d, within %.0fu)\n",
                    int( cand.size() ),
                    int( wanted ),
                    budget,
                    maxDist );
            for( size_t k = 0; k < cand.size() && k < 6; k++ )
            {
                Printf( "    %-28s %-12s (%.0f, %.0f, %.0f)  lit=%d %s x%.2f\n",
                        cand[ k ].sw->tex,
                        cand[ k ].why,
                        cand[ k ].x,
                        cand[ k ].y,
                        cand[ k ].z,
                        cand[ k ].sw->lit,
                        cand[ k ].ue ? "UE" : "d64",
                        cand[ k ].scale );
            }
        }
    }
}


// Lava lights.
//
// The lava's own emissive shades the lava and nothing else -- RTGL1 emissive is not
// a light source -- and the lightIntensity sitting in textures.json only ever
// produced a light for sprites. So a lava room path-traces as a black box with a
// glowing net on the floor, which is what screen/lavanoemit_needshader.png is.
//
// A lake is an AREA, so one light at the sector's centerspot is wrong twice: a
// concave lake gets its light outside itself, and a long one gets a single bright
// spot in the middle with dark ends. These are scattered on a world-space grid and
// tested against the BSP, which handles both and costs one PointInSubsector per
// candidate point.
//
// Grid points are anchored to WORLD coordinates, not to each subsector's own
// bounding box. Two neighbouring subsectors of the same lake would otherwise each
// start their grid at their own corner, and the seam between them would get a
// double row of lights -- a bright line across the lake exactly where the BSP
// happened to split it.
void RT_UploadLavaLights()
{
    if( !primaryLevel )
    {
        return;
    }

    // BEFORE the rt_lava_light_on early-out, deliberately. The arms that turn
    // the analytic grid OFF (gi, flagcheck) are exactly the ones being judged,
    // and having them silently skip the teleport left two of them evaluated
    // from the far side of the map.
    if( cvar::rt_lava_autogoto )
    {
        static const void* s_goto = nullptr;
        if( s_goto != primaryLevel )
        {
            s_goto = primaryLevel;
            AddCommandString( "rt_lava_goto" );
        }
    }

    if( !cvar::rt_lava_light_on )
    {
        return;
    }

    const float baseIntensity = std::max( 0.f, float{ cvar::rt_lava_light_intensity } );
    const int   budget        = std::max( 0, int{ cvar::rt_lava_light_max } );
    if( baseIntensity <= 0.001f || budget == 0 )
    {
        return;
    }

    const double spacing  = std::max( 16.0, double( float{ cvar::rt_lava_light_spacing } ) );
    const float  srcRad   = std::max( 0.01f, float{ cvar::rt_lava_light_radius } );
    const double zOff     = double( float{ cvar::rt_lava_light_z } );
    const double maxDist  = std::max( 64.0, double( float{ cvar::rt_lava_light_dist } ) );
    const double maxDist2 = maxDist * maxDist;

    // Constant total output as the grid changes density: one light covers spacing^2
    // of floor, so its share of the lake scales with the square. Without this,
    // halving the spacing to smooth the falloff quadruples the room's brightness and
    // every other value has to be retuned around it.
    const float intensity = baseIntensity * float( ( spacing * spacing ) / ( 96.0 * 96.0 ) );

    const RgColor4DPacked32 color =
        rt.rgUtilPackColorByte4D( uint8_t( std::clamp( *cvar::rt_lava_light_r, 0, 255 ) ),
                                  uint8_t( std::clamp( *cvar::rt_lava_light_g, 0, 255 ) ),
                                  uint8_t( std::clamp( *cvar::rt_lava_light_b, 0, 255 ) ),
                                  255 );

    const DVector3 vpos = r_viewpoint.Pos;

    struct LavaCand
    {
        double   d2;
        double   x, y, z;
        uint64_t id;
    };
    std::vector< LavaCand > cand;

    int matchedSecs = 0, gridPoints = 0;

    for( unsigned i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        const sector_t& sec = primaryLevel->sectors[ i ];

        auto* gtex = TexMan.GetGameTexture( sec.GetTexture( sector_t::floor ), true );
        if( !gtex || !RT_IsLavaFlat( gtex->GetName().GetChars() ) )
        {
            continue;
        }
        matchedSecs++;

        double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
        for( unsigned li = 0; li < sec.Lines.Size(); li++ )
        {
            const line_t* ln = sec.Lines[ li ];
            if( !ln || !ln->v1 || !ln->v2 )
            {
                continue;
            }
            for( const vertex_t* v : { ln->v1, ln->v2 } )
            {
                minx = std::min( minx, v->fX() );
                maxx = std::max( maxx, v->fX() );
                miny = std::min( miny, v->fY() );
                maxy = std::max( maxy, v->fY() );
            }
        }
        if( minx > maxx )
        {
            continue;
        }

        // Snap to the world grid, then walk it -- see the note above about seams.
        const long long gx0 = (long long)std::floor( minx / spacing );
        const long long gx1 = (long long)std::floor( maxx / spacing );
        const long long gy0 = (long long)std::floor( miny / spacing );
        const long long gy1 = (long long)std::floor( maxy / spacing );

        for( long long gy = gy0; gy <= gy1; gy++ )
        {
            for( long long gx = gx0; gx <= gx1; gx++ )
            {
                const double px = ( double( gx ) + 0.5 ) * spacing;
                const double py = ( double( gy ) + 0.5 ) * spacing;

                const double dx = px - vpos.X;
                const double dy = py - vpos.Y;
                const double d2 = dx * dx + dy * dy;
                if( d2 > maxDist2 )
                {
                    continue;
                }

                // The BSP decides, not the bounding box. A lava sector is rarely
                // convex and its box always overlaps the walkway beside it, so a box
                // test alone hangs lights over dry land. This also deduplicates for
                // free: a world grid point is inside exactly one sector.
                if( primaryLevel->PointInSector( px, py ) != &sec )
                {
                    continue;
                }
                gridPoints++;

                const double pz = sec.floorplane.ZatPoint( DVector2{ px, py } ) + zOff;

                // Stable id: sector index and the grid CELL within it, never the
                // insertion order. The candidate list is re-sorted by camera distance
                // every frame, so an order-derived id would rename every light as the
                // player walks -- and RTGL1 answers a changed id by throwing away that
                // light's temporal reservoir, which on sources this large boils the
                // whole room.
                // Cell coordinates, masked to a byte each. Two cells can only
                // collide if they are 256 cells apart in one sector, i.e. 24576
                // map units at the default spacing -- larger than any Doom sector.
                const uint64_t slot = ( uint64_t( gy & 0xFF ) << 8 ) | uint64_t( gx & 0xFF );

                cand.push_back( { d2,
                                  px,
                                  py,
                                  pz,
                                  LavaLightId_Base + uint64_t( i ) * LavaSlotsPerSector +
                                      slot } );
            }
        }
    }

    // Nearest first. A lake three rooms away contributes nothing a player can see and
    // would otherwise spend the whole budget before the one underfoot is reached.
    if( int( cand.size() ) > budget )
    {
        std::partial_sort( cand.begin(),
                           cand.begin() + budget,
                           cand.end(),
                           []( const LavaCand& a, const LavaCand& b ) { return a.d2 < b.d2; } );
        cand.resize( budget );
    }

    for( const LavaCand& c : cand )
    {
        // METRES. RTGL1 world space is metres and every other light family in
        // this file converts (ONEGAMEUNIT_IN_METERS); this one did not, so the
        // grid was uploaded 32x too far out -- a light meant to sit 12 units
        // over the lava landed 12 METRES over it, and one 40 units away landed
        // 40 metres away. Irradiance goes as 1/d^2, so the whole lake arrived
        // ~1000x too weak AND in the wrong place. It looked exactly like the
        // lights not existing, which is where three rounds went.
        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = color,
            .intensity = intensity,
            .position  = { float( c.x ) * ONEGAMEUNIT_IN_METERS,
                           float( c.y ) * ONEGAMEUNIT_IN_METERS,
                           float( c.z ) * ONEGAMEUNIT_IN_METERS },
            .radius    = srcRad,
        };
        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sph,
            .uniqueID     = c.id,
            .isExportable = false,
        };
        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );
    }

    // rt_lava_light_debug 2: a control light sitting on the camera.
    //
    // Every stage up to LightManager::Add is now verified by the RTGL probe --
    // nothing rejected, position converted correctly, radiance non-zero, array
    // far from full -- and the room is still black. That leaves two very
    // different possibilities which no amount of grid tuning can separate:
    // these lights are lost after Add, or analytic lights do not work in this
    // build at all. A 2000 lm light one metre over the camera answers it: if
    // THAT does not light the room, the lava is not the subject.
    if( int{ cvar::rt_lava_light_debug } >= 2 )
    {
        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorByte4D( 255, 255, 255, 255 ),
            .intensity = 2000.f,
            .position  = { float( vpos.X ) * ONEGAMEUNIT_IN_METERS,
                           float( vpos.Y ) * ONEGAMEUNIT_IN_METERS,
                           float( vpos.Z + 32.0 ) * ONEGAMEUNIT_IN_METERS },
            .radius    = 0.2f,
        };
        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sph,
            .uniqueID     = LavaLightId_Base - 1,
            .isExportable = false,
        };
        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        static int s_once = 0;
        if( ( s_once++ % 600 ) == 0 )
        {
            Printf( "rt_lava_light: CONTROL light 2000 lm at the camera "
                    "map(%.0f %.0f %.0f). If the room is still black, analytic "
                    "lights are broken here and the lava grid is not the bug.\n",
                    vpos.X,
                    vpos.Y,
                    vpos.Z + 32.0 );
        }
    }

    // The "found lava, lit nothing" case announces itself, with no cvar.
    //
    // A feature that produces zero output is indistinguishable from a feature
    // that was never called, and this one has now been debugged twice from the
    // wrong end because of that. If there IS lava in the map and no light came
    // out of it, that is always worth a line.
    if( matchedSecs > 0 && cand.empty() )
    {
        static std::unordered_set< const void* > s_warned;
        if( s_warned.insert( primaryLevel ).second )
        {
            Printf( RT_DiagPrintLevel(),
                    "RT lava: %d lava sector(s) found, but NO light placed. "
                    "Grid points inside a sector: %d (spacing %.0f, cull %.0f). "
                    "If grid points is 0 the sectors are smaller than the spacing "
                    "or further than the cull radius.\n",
                    matchedSecs,
                    gridPoints,
                    spacing,
                    maxDist );
        }
    }

    if( cvar::rt_lava_light_debug )
    {
        static int s_last = -1;
        if( int( cand.size() ) != s_last )
        {
            s_last = int( cand.size() );
            Printf( "rt_lava_light: %d lava sector(s), %d grid point(s) in range, "
                    "%d uploaded (spacing %.0f, intensity %.1f lm each, radius %.2f m)\n",
                    matchedSecs,
                    gridPoints,
                    int( cand.size() ),
                    spacing,
                    intensity,
                    srcRad );
            // Positions in BOTH spaces, and the camera with them. A light that
            // is correct in map units and wrong in metres is invisible in every
            // other symptom, so the conversion is printed rather than trusted.
            //
            // cand.front() is NOT the nearest: the partial_sort above only runs
            // when the list is over budget, so under it the order is scan order.
            // That printed a light 990 units away while the player stood in the
            // middle of the lake and cost a round. Compute it properly, and say
            // how many are actually close -- one number that answers "is this
            // being judged from somewhere it could possibly matter".
            if( !cand.empty() )
            {
                size_t  nearestIdx = 0;
                int     within256  = 0;
                for( size_t k = 0; k < cand.size(); k++ )
                {
                    if( cand[ k ].d2 < cand[ nearestIdx ].d2 )
                    {
                        nearestIdx = k;
                    }
                    if( cand[ k ].d2 < 256.0 * 256.0 )
                    {
                        within256++;
                    }
                }
                Printf( "rt_lava_light: %d light(s) within 256 map units of the camera\n",
                        within256 );
                const LavaCand& c = cand[ nearestIdx ];
                Printf( "rt_lava_light: nearest light map(%.0f %.0f %.0f) -> "
                        "rt(%.2f %.2f %.2f) m, camera map(%.0f %.0f %.0f), "
                        "%.1f map units away\n",
                        c.x,
                        c.y,
                        c.z,
                        c.x * ONEGAMEUNIT_IN_METERS,
                        c.y * ONEGAMEUNIT_IN_METERS,
                        c.z * ONEGAMEUNIT_IN_METERS,
                        vpos.X,
                        vpos.Y,
                        vpos.Z,
                        std::sqrt( c.d2 ) );
            }
        }
    }
}

// --- LOCALISED SMOKE -------------------------------------------------------
//
// A puff is a sphere of medium in the froxel volume (RgDrawFrameSmokeParams ->
// Smoke.h). Positions and velocities here are METRES and metres/second, the
// same space RTGL1 takes light positions in, because the shader tests a puff
// against volume_getCenter's output directly.
//
// Why the simulation is on this side rather than on the GPU: this is the only
// side that can see the level. A puff pools under a low ceiling because
// PointInSector and ceilingplane are here. The counts are tens, so it is free,
// and what caps the detail is the froxel grid -- 64 slices over rt_smoke_far --
// not the number of particles. See docs/rt-smoke.md.

void RT_UploadFlameLights()
{
    // Fire is the one light source in this game that must not hold still. See the
    // rt_flame_light_on cvar text for why neither half of that (the flicker, and the
    // offset up onto the flame) can be expressed in RTGL1 texture meta.
    if( !cvar::rt_flame_light_on || !primaryLevel )
    {
        return;
    }

    const float scale = std::max( 0.f, float{ cvar::rt_flame_light_scale } );
    if( scale <= 0.001f )
    {
        return;
    }
    const float  srcRadius = std::max( 0.01f, float{ cvar::rt_flame_light_radius } );
    const float  flicker   = std::clamp( float{ cvar::rt_flame_light_flicker }, 0.f, 1.f );
    const float  speed     = std::max( 0.f, float{ cvar::rt_flame_light_speed } );
    const float  wobble    = std::max( 0.f, float{ cvar::rt_flame_light_wobble } );
    const double maxDist   = std::max( 64.0, double( float{ cvar::rt_flame_light_maxdist } ) );
    const double maxDist2  = maxDist * maxDist;
    const int    budget    = std::max( 0, int{ cvar::rt_flame_light_max } );
    if( budget == 0 )
    {
        return;
    }

    // maptime, not wall clock: a paused or console-open game must freeze the fire with
    // everything else. 35 Hz stepping is not a limitation — GLDEFS' own flickerlight
    // re-rolls once per tic, so this is already the smoother of the two.
    const float t = float( primaryLevel->maptime ) * speed;

    const DVector3 vpos = r_viewpoint.Pos;

    struct FlameCand
    {
        double             d2;
        float              px, py, pz;
        float              intensity;
        uint64_t           id;
        const RtFlameKind* kind;
    };
    std::vector< FlameCand > cand;

    auto    it = primaryLevel->GetThinkerIterator< AActor >();
    AActor* mo = nullptr;
    while( ( mo = it.Next() ) != nullptr )
    {
        const RtFlameKind* kind = RT_FlameKindOf( mo );
        if( !kind )
        {
            continue;
        }
        if( ( mo->renderflags & RF_INVISIBLE ) || mo->Alpha <= 0.01 )
        {
            continue;
        }

        // Per-actor phase. Without it every torch in the level flickers in unison, which
        // is exactly the failure that rules out doing this from the sprite animation:
        // the props all spawn at map load, so their frame counters are already in lockstep.
        const uint64_t h = uint64_t( reinterpret_cast< uintptr_t >( mo ) ) >> 4;
        const float phase = float( h & 0xFFFF ) * ( 6.2831853f / 65536.0f );

        // Three incommensurate harmonics: the sum has no short period, so a torch the
        // player stands next to for a minute never visibly loops. Normalised by the sum
        // of the weights so `flicker` stays a true fraction of base intensity.
        const float f = ( 0.55f * std::sin( t + phase ) +          //
                          0.30f * std::sin( t * 2.37f + phase * 1.7f ) +
                          0.15f * std::sin( t * 4.11f + phase * 2.9f ) );

        const float intensity =
            std::max( 0.f, kind->intensity * scale * ( 1.0f + flicker * f ) );
        if( intensity <= 0.01f )
        {
            continue;
        }

        // The same three-harmonic trick on position, at different frequencies so the
        // drift does not simply track the brightness. Lateral only gets the full wobble;
        // vertical gets half, because a flame licks upward far more than it slides.
        const float wx = wobble * std::sin( t * 0.83f + phase * 2.1f );
        const float wy = wobble * std::sin( t * 1.19f + phase * 3.3f );
        const float wz = wobble * 0.5f * std::sin( t * 1.61f + phase * 1.3f );

        const double lx = double( mo->X() ) + wx;
        const double ly = double( mo->Y() ) + wy;
        const double lz = double( mo->Z() ) + double( kind->up ) + wz;

        const double dx = lx - vpos.X, dy = ly - vpos.Y, dz = lz - vpos.Z;
        const double d2 = dx * dx + dy * dy + dz * dz;
        if( d2 > maxDist2 )
        {
            continue;
        }

        cand.push_back( FlameCand{
            d2,
            float( lx ) * ONEGAMEUNIT_IN_METERS,
            float( ly ) * ONEGAMEUNIT_IN_METERS,
            float( lz ) * ONEGAMEUNIT_IN_METERS,
            intensity,
            // Stable across frames: derived from actor identity alone, never from the
            // tick. An id that moved would make RTGL1 see the whole set die and respawn
            // every frame, throwing away its temporal reservoirs.
            FlameLightId_Base + ( uint64_t( reinterpret_cast< uintptr_t >( mo ) ) & 0xFFFFFFFFull ),
            kind,
        } );
    }

    const size_t wanted = cand.size();
    if( wanted > size_t( budget ) )
    {
        std::partial_sort( cand.begin(),
                           cand.begin() + budget,
                           cand.end(),
                           []( const FlameCand& a, const FlameCand& b ) { return a.d2 < b.d2; } );
        cand.resize( size_t( budget ) );
    }

    for( const FlameCand& c : cand )
    {
        const float kR = ( ( c.kind->rgb >> 16 ) & 0xFF ) / 255.0f;
        const float kG = ( ( c.kind->rgb >> 8 ) & 0xFF ) / 255.0f;
        const float kB = ( c.kind->rgb & 0xFF ) / 255.0f;

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( kR, kG, kB, 1.0f ),
            .intensity = c.intensity,
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

        if( cvar::rt_flame_light_debug )
        {
            auto markSph = RgLightSphericalEXT{
                .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
                .pNext     = nullptr,
                .color     = rt.rgUtilPackColorByte4D( 0, 255, 255, 255 ),
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

    if( cvar::rt_flame_light_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            Printf( "rt_flame_light: uploaded=%zu of %zu wanted (cap %d, within %.0fu) "
                    "scale=%.2f flicker=%.2f wobble=%.1f\n",
                    cand.size(),
                    wanted,
                    budget,
                    maxDist,
                    scale,
                    flicker,
                    wobble );
        }
    }
}

// UE PROJECTILE LIGHTS THAT CANNOT REACH RT THROUGH THE MOD'S OWN DEFINITIONS.
//
// UE defines D64UE_Rocket with an attached #C43F21 light and a PUF2 smoke
// trail, but the renderer package's CheelloRocket replacement is the class that
// actually reaches the thinker list. The volumetric smoke tracker still sees
// it by the "Rocket" class substring and drops every parcel correctly; what is
// missing in UE's deliberately dark IWAD rooms is the moving light that makes
// those parcels visible. Restore exactly UE's intended radius/colour here.
//
// The Unmaker mismatch is the inverse. Retribution draws a FastProjectile plus
// a dense UNML sprite trail; every UNML card has authored red emission/light.
// UE does an instant line trace and represents the whole laser with one
// stretched D64UE_UnmakerBolt OBJ. There is no UNML texture or projectile
// dynlight for the shared material metadata to find. Sample three stable lights
// along the model's current segment instead. The separate viewer muzzle flash
// remains responsible for lighting the gun at the instant of firing; these
// lights belong to the visible beam and therefore follow it as it retracts.
//
// These are deliberately NOT broad projectile rules. Retribution already
// lights both weapons through authored RT material metadata, and a global alias
// would double those sources. The same identity UE already uses for its scene
// namespace (rt_mod_compat=0 + rt_world_white=1) plus exact live classes makes
// this path unreachable everywhere else. A future engine where D64UE_Rocket
// wins again naturally stops matching and goes back to its own A_AttachLight.
void RT_UploadUnseenEvilProjectileLights()
{
    if( rt_mod_compat || !cvar::rt_world_white || !cvar::rt_dynlight || !primaryLevel )
    {
        return;
    }

    constexpr float UE_ROCKET_RADIUS = 20.f; // map units, from D64UE_Rocket
    constexpr float UE_LASER_RADIUS  = 8.f;  // intensity-equivalent map radius per sample

    const float dynScale     = std::max( 0.f, float{ cvar::rt_dynlight_intensity } );
    const float intensityMax = std::max( 0.f, float{ cvar::rt_dynlight_max } );
    auto scaledIntensity = [ & ]( float radius ) {
        float value = radius * dynScale;
        if( intensityMax > 0.f )
        {
            value = std::min( value, intensityMax );
        }
        return value;
    };

    const float rocketIntensity = scaledIntensity( UE_ROCKET_RADIUS );
    const float laserIntensity  = scaledIntensity( UE_LASER_RADIUS );
    const float minRadius       = std::max( 0.f, float{ cvar::rt_dynlight_minradius } );

    auto upload = [ & ]( uint64_t id, const FVector3& pos, RgColor4DPacked32 color,
                         float intensity ) {
        if( intensity <= 0.01f )
        {
            return;
        }
        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = color,
            .intensity = intensity,
            .position  = { pos.X, pos.Y, pos.Z },
            .radius    = std::max( 0.01f, float{ cvar::rt_dynlight_radius } ),
        };
        auto info = RgLightInfo{
            .sType        = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext        = &sph,
            .uniqueID     = id,
            .isExportable = false,
        };
        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );
    };

    auto    it = primaryLevel->GetThinkerIterator< AActor >();
    AActor* mo = nullptr;
    while( ( mo = it.Next() ) != nullptr )
    {
        if( !mo->GetClass() )
        {
            continue;
        }
        const char* cls = mo->GetClass()->TypeName.GetChars();
        if( !cls )
        {
            continue;
        }

        // Four slots per actor: rocket uses 0, the laser segment uses 1..3.
        // Mask before shifting so the pointer-derived part cannot climb into
        // another reserved ID range.
        const uint64_t actorId =
            UEProjectileLightId_Base +
            ( ( uint64_t( reinterpret_cast< uintptr_t >( mo ) ) & 0x3FFFFFFFull ) << 2 );

        if( strcmp( cls, "CheelloRocket" ) == 0 )
        {
            if( !( mo->flags & MF_MISSILE ) || UE_ROCKET_RADIUS < minRadius )
            {
                continue;
            }

            FVector3 pos{ float( mo->X() ) * ONEGAMEUNIT_IN_METERS,
                          float( mo->Y() ) * ONEGAMEUNIT_IN_METERS,
                          float( mo->Z() + mo->Height * 0.5 ) * ONEGAMEUNIT_IN_METERS };
            FVector3 vel{ float( mo->Vel.X ), float( mo->Vel.Y ), float( mo->Vel.Z ) };
            if( vel.LengthSquared() > 0.0001f )
            {
                // UE's A_AttachLight offset is (-32,0,0): one metre behind the
                // projectile, precisely where the newest smoke parcel is dropped.
                pos -= vel.Unit() * ( 32.f * ONEGAMEUNIT_IN_METERS );
            }

            upload( actorId,
                    pos,
                    rt.rgUtilPackColorByte4D( 0xC4, 0x3F, 0x21, 255 ),
                    rocketIntensity );
            continue;
        }

        if( strcmp( cls, "D64UE_UnmakerBolt" ) != 0 || UE_LASER_RADIUS < minRadius )
        {
            continue;
        }

        // ZScript orients the model's local Y axis down the trace by storing
        // (shot pitch - 90 degrees) in Actor.Pitch. Undo that model correction
        // to recover the world-space segment direction.
        constexpr double PI = 3.14159265358979323846;
        const double yaw     = mo->Angles.Yaw.Radians();
        const double pitch   = mo->Angles.Pitch.Radians() + PI * 0.5;
        const double cp      = std::cos( pitch );
        const FVector3 dir{ float( cp * std::cos( yaw ) ),
                            float( cp * std::sin( yaw ) ),
                            float( -std::sin( pitch ) ) };

        // UE sets scale.y = (remaining half-length + 16) * 1.2. Inverting
        // that expression gives the half-length of the model segment around
        // its actor origin, in map units.
        const float halfLength =
            std::max( 0.f, ( float( mo->Scale.Y ) / 1.2f ) - 16.f ) *
            ONEGAMEUNIT_IN_METERS;
        const FVector3 center{ float( mo->X() ) * ONEGAMEUNIT_IN_METERS,
                               float( mo->Y() ) * ONEGAMEUNIT_IN_METERS,
                               float( mo->Z() ) * ONEGAMEUNIT_IN_METERS };
        constexpr float SAMPLE_OFFSETS[] = { -0.65f, 0.f, 0.65f };
        const auto red = rt.rgUtilPackColorByte4D( 0xFF, 0x14, 0x08, 255 );
        for( uint64_t i = 0; i < std::size( SAMPLE_OFFSETS ); i++ )
        {
            upload( actorId + 1 + i,
                    center + dir * ( halfLength * SAMPLE_OFFSETS[ i ] ),
                    red,
                    laserIntensity );
        }
    }
}

