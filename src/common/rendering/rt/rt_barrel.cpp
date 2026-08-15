// Doom64-RT: WHAT AN EXPLODING BARREL LEAVES BEHIND.
//
// A barrel currently goes off, throws a smoke burst, and leaves the room
// exactly as it found it. It should leave a SCENE: a scorch churned into the
// floor under it, and pieces of the barrel itself lying about.
//
// THREE THINGS TO KNOW BEFORE CHANGING ANYTHING HERE.
//
// 1. THE TRIGGER IS NOT THE ACTOR DISAPPEARING, and this is the one place the
//    projectile rule in rt_impacts.cpp cannot be reused. A projectile is gone
//    the tic after it explodes, which is what makes "tracked last tic, absent
//    this one" a reliable impact edge. `64ExplosiveBarrel` does the opposite:
//    after A_Explode it lingers for a 1050-tic respawn timer, so that rule
//    would put the whole effect roughly twenty SECONDS late. The health test
//    the monster paths use is no help either -- a barrel in its death state has
//    no health, that is the point of the state.
//
//    So the edge is the SPRITE FRAME, exactly as RT_BarrelSmoke already finds
//    it: BEXP frame E is where A_Explode sits in the Death state. Frame E is
//    held for 5 + 7 + 9 tics across three state lines, so the rising edge has
//    to be detected rather than merely tested for, or everything below fires
//    twenty-one times.
//
// 2. IT DOES NOT RIDE rt_smoke'S WALK, though that is where the barrel's smoke
//    burst lives and the edge detection there is identical. Hanging off it
//    would make barrel debris depend on rt_smoke AND rt_smoke_barrel as well as
//    its own cvar -- three-deep gating for an effect that is not smoke, which
//    is the coupling docs/plan-projectile-impact-fx.md 2 rejects and the reason
//    the projectile impacts got their own walk. This rides the IMPACT walk
//    instead, which is already visiting every actor once a tic for free.
//
// 3. THE CHUNKS ARE NOT DEBRIS CHIPS. That was the explicit ask -- "bigger
//    pieces that really look like the barrel metal sprite parts, not just
//    square particle pixels" -- and it is a construction difference, not a size
//    multiplier. A debris chip is a camera-facing billboard, which is invisible
//    as a cheat at 2 cm and glaring at 25 cm. SparkKind::Shard exists for that
//    reason and rt_spark_draw.cpp builds it as world-oriented curved plate.
//    See the note there before reaching for a size cvar.

#include "rt_sparks_internal.h"

#include <cstring>
#include <fstream>

// NOTE: no `using namespace rtsp;` up here -- this file DEFINES things in that
// namespace, and a using-directive in scope while doing so makes every
// unqualified name findable twice. It goes after the closing brace, for the
// global-scope CCMD below. Same as rt_impacts.cpp.
using namespace rtx;

namespace rtsp
{

// One tracked barrel. Deliberately the smallest thing that can answer "was this
// actor already in frame E last time I looked": the AActor* is NEVER
// dereferenced outside the walk that just handed it to us, so a barrel removed
// between tics can only ever leave a stale entry, which the sweep drops.
struct BarrelMark
{
    AActor* mo;
    int     lastTic;
    bool    blowing;
};

std::vector< BarrelMark > g_barrels;

// ---------------------------------------------------------------------------
// THE AUTHORED PIECES, and this replaced geometry I had generated myself.
//
// The first version built each chunk out of maths: a curved strip with hashed
// tears along both edges and a colour picked from a palette sampled out of
// BAR1A0. It was reported, correctly, as "still particles / parts you generate
// yourself, its ugly" -- and the offer that followed is the better answer by a
// wide margin. Four pieces were cut from the barrel art by hand: torn, charred,
// with hot orange along the break. No procedural tear generator was going to
// arrive at that, because the thing that makes them read is authorship.
//
// HOW ART GETS IN WITHOUT TOUCHING A WAD. RTGL1 resolves a primitive's
// pTextureName against rt/mat/<name>.png on disk, which is the same route the
// map title cards take: rt_titles.cpp registers the NAME with a 1x1 placeholder
// via rgProvideOriginalTexture and RTGL1 substitutes the file. So a piece is a
// file at
//
//     rt/mat/d64rt/barrel/shardN.png        (and rt/mat_dev, see below)
//
// referred to as "d64rt/barrel/shardN", and adding a fifth piece is dropping a
// fifth file -- no code, no lump, no DECORATE.
//
// BOTH TREES OR IT IS A GHOST. The staged RTGL1.json sets developerMode true,
// which makes RTGL1 read mat_dev in preference to mat. Art placed in only one
// of them looks exactly like a plumbing failure.
//
// THE SHAPE IS THE ALPHA CHANNEL, which is why the primitive is uploaded
// ALPHA_TESTED. Without that flag every piece renders as the full rectangle its
// art sits in -- a barrel-coloured playing card.

constexpr int RT_BARREL_ART_MAX = 8;

struct ShardArt
{
    FString name;   // the RTGL1 material name, e.g. "d64rt/barrel/shard1"
    float   aspect; // width / height of the source image
};

std::vector< ShardArt > g_shardArt;
bool                    g_shardArtScanned = false;

// The ember sheet, found and registered by the same scan. Separate from the
// shard list because there is one of it and it is not interchangeable with a
// piece of plate.
FString g_emberArt;
float   g_emberArtAspect = 1.f;

// PNG dimensions straight out of the IHDR. Twenty lines against pulling in an
// image decoder, and it cannot go wrong quietly: the signature and the chunk
// name are both checked, so a file that is not a PNG is rejected rather than
// producing a nonsense aspect ratio nobody would trace back to here.
//
// A PLAIN FILE, NOT A LUMP. rt/mat is RTGL1's own directory next to gzdoom.exe
// and is NOT in gzdoom's lump filesystem -- fileSystem.CheckNumForFullName on
// an rt/ path always answers -1 however present the file is. That mistake cost
// this project a silent failure once already, in LoadSparkSurfaces.
bool ReadPngSize( const char* path, int* outW, int* outH )
{
    std::ifstream f( path, std::ios::binary );
    if( !f.is_open() )
    {
        return false;
    }

    unsigned char hdr[ 24 ]{};
    f.read( reinterpret_cast< char* >( hdr ), sizeof( hdr ) );
    if( f.gcount() < std::streamsize( sizeof( hdr ) ) )
    {
        return false;
    }

    static const unsigned char kSig[ 8 ] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    if( memcmp( hdr, kSig, 8 ) != 0 || memcmp( hdr + 12, "IHDR", 4 ) != 0 )
    {
        return false;
    }

    *outW = ( int( hdr[ 16 ] ) << 24 ) | ( int( hdr[ 17 ] ) << 16 ) | ( int( hdr[ 18 ] ) << 8 ) |
            int( hdr[ 19 ] );
    *outH = ( int( hdr[ 20 ] ) << 24 ) | ( int( hdr[ 21 ] ) << 16 ) | ( int( hdr[ 22 ] ) << 8 ) |
            int( hdr[ 23 ] );

    return *outW > 0 && *outH > 0;
}

// Register the name so RTGL1 has something to hang the rt/mat override on.
// Exactly RT_RegisterFullscreenImage's trick, with NEAREST rather than LINEAR:
// everything in this game is nearest-filtered (rt_smoothtextures ships false),
// and a smoothed 20-pixel cut-out would be the one soft-edged object on screen.
void RegisterShardTexture( const char* name )
{
    constexpr uint8_t empty[] = { 0, 0, 0, 0 };

    auto info = RgOriginalTextureInfo{
        .sType        = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO,
        .pNext        = nullptr,
        .pTextureName = name,
        .pPixels      = empty,
        .size         = { 1, 1 },
        .filter       = RG_SAMPLER_FILTER_NEAREST,
        .addressModeU = RG_SAMPLER_ADDRESS_MODE_CLAMP,
        .addressModeV = RG_SAMPLER_ADDRESS_MODE_CLAMP,
    };

    RgResult r = rt.rgProvideOriginalTexture( &info );
    RG_CHECK( r );
}

// Called from the DRAW, not from startup: rgProvideOriginalTexture wants a live
// renderer, and the draw is the only place this file's work is guaranteed to be
// inside one. Runs once.
void ScanShardArt()
{
    if( g_shardArtScanned )
    {
        return;
    }
    g_shardArtScanned = true;

    for( int i = 1; i <= RT_BARREL_ART_MAX; i++ )
    {
        FString rel;
        rel.Format( "d64rt/barrel/shard%d", i );

        // mat_dev first, mirroring RTGL1's own precedence under developerMode,
        // so the dimensions read here always belong to the file that will
        // actually be sampled.
        FString devPath;
        devPath.Format( "rt/mat_dev/%s.png", rel.GetChars() );
        FString matPath;
        matPath.Format( "rt/mat/%s.png", rel.GetChars() );

        int w = 0, h = 0;
        if( !ReadPngSize( devPath.GetChars(), &w, &h ) &&
            !ReadPngSize( matPath.GetChars(), &w, &h ) )
        {
            continue;
        }

        RegisterShardTexture( rel.GetChars() );
        g_shardArt.push_back( ShardArt{ rel, float( w ) / float( h ) } );
    }

    // THE EMBER SHEET, one file rather than a numbered set. Scattered coals on
    // transparent; a coal takes the WHOLE image, with its UVs flipped at random
    // per coal so fifty of them in one scorch are not fifty copies of one shape.
    {
        const char* rel = "d64rt/embers/embers";
        FString     devPath, matPath;
        devPath.Format( "rt/mat_dev/%s.png", rel );
        matPath.Format( "rt/mat/%s.png", rel );

        int w = 0, h = 0;
        if( ReadPngSize( devPath.GetChars(), &w, &h ) ||
            ReadPngSize( matPath.GetChars(), &w, &h ) )
        {
            RegisterShardTexture( rel );
            g_emberArt       = rel;
            g_emberArtAspect = float( w ) / float( h );
            // QUIET unless asked. This is a success line on a path that is
            // shipped OFF; it was loud while the art was being got working and
            // has no business in a normal session's log. The FAILURE below
            // stays loud, because a missing file and a working one that simply
            // does not show up look identical and only one is fixed by editing
            // the art.
            if( cvar::rt_barrel_debug )
            {
                Printf( "rt_barrel: ember art %s (%.2f)\n", rel, g_emberArtAspect );
            }
        }
        else if( cvar::rt_barrel_ember_tex )
        {
            // Only when the feature is actually SWITCHED ON. Warning about
            // missing art for a path nobody asked for is noise; warning about
            // it when it was asked for is the difference between a missing file
            // and a broken effect.
            Printf( TEXTCOLOR_ORANGE "rt_barrel: NO ember art -- tried '%s' and '%s' "
                                     "(cwd-relative)\n" TEXTCOLOR_NORMAL,
                    devPath.GetChars(),
                    matPath.GetChars() );
        }
    }

    // ALWAYS SAYS SOMETHING, and the zero case is the loud one. With no art the
    // shards fall back to generated geometry, which looks like a feature that
    // was never changed rather than like a missing file -- the single most
    // expensive kind of failure in this project's history.
    if( g_shardArt.empty() )
    {
        Printf( TEXTCOLOR_ORANGE
                "rt_barrel: NO shard art found -- looked for "
                "rt/mat_dev/d64rt/barrel/shard1..%d.png and rt/mat/...\n" TEXTCOLOR_NORMAL
                "  Falling back to generated plate. Art goes in BOTH authored trees:\n"
                "  Doom64-Retribution/Retribution-RT-Materials/rt/{mat,mat_dev}/d64rt/barrel/\n",
                RT_BARREL_ART_MAX );
    }
    else
    {
        // Quiet unless asked, for the reason the ember line is: the art loading
        // correctly is the normal case, and a line per session about it is the
        // kind of noise that trains people to stop reading the log.
        if( cvar::rt_barrel_debug )
        {
            FString sizes;
            for( const ShardArt& a : g_shardArt )
            {
                sizes.AppendFormat( " %s(%.2f)", a.name.GetChars(), a.aspect );
            }
            Printf( "rt_barrel: %d shard pieces:%s\n", int( g_shardArt.size() ), sizes.GetChars() );
        }
    }
}

int ShardArtCount()
{
    return int( g_shardArt.size() );
}

const char* EmberArtName()
{
    return g_emberArt.IsEmpty() ? nullptr : g_emberArt.GetChars();
}

float EmberArtAspect()
{
    return g_emberArtAspect;
}

const char* ShardArtName( int i )
{
    return g_shardArt[ size_t( i ) ].name.GetChars();
}

float ShardArtAspect( int i )
{
    return g_shardArt[ size_t( i ) ].aspect;
}

// ---------------------------------------------------------------------------

// The barrel's own palette, one entry per shard, held for its whole life.
//
// Spread across the sprite's value range rather than drawn from the sprite by
// pixel frequency -- see the note on RT_BARREL_SHADES. A `sid`-keyed hash rather
// than the sim RNG so that the same shard is the same colour every frame, which
// matters here in a way it does not for a chip: a quarter-metre plate that
// changed shade between frames would be the single most visible thing on screen.
uint32_t ShardShade( uint32_t sid )
{
    const int i = std::clamp( int( hash01( sid * 0x27D4EB2Fu ) * float( RT_BARREL_SHADES_N ) ),
                              0,
                              RT_BARREL_SHADES_N - 1 );
    return RT_BARREL_SHADES[ i ];
}

// `at` is METRES; `up` is the direction the burst is biased toward, normally the
// floor's normal under the barrel.
void SpawnBarrelShards( const FVector3& at, const FVector3& up )
{
    if( !primaryLevel || !cvar::rt_barrel || !cvar::rt_barrel_debris )
    {
        return;
    }

    const int want = std::max( 0, int{ cvar::rt_barrel_count } );
    if( want == 0 )
    {
        return;
    }

    // How many authored pieces exist. Zero is a legitimate state -- no art
    // installed -- and the whole burst falls back to generated grit rather than
    // to nothing.
    const int artCount = ShardArtCount();

    const DebrisProfile& pr    = ProfileFor( SurfKind::Barrel );
    const float          size  = std::max( 0.01f, float{ cvar::rt_barrel_size } );
    // NOT CLAMPED UP FROM ZERO: 0 is the "forever" sentinel, and a max() here
    // would silently turn permanent wreckage into half a second of it.
    const float          life  = float{ cvar::rt_barrel_life };
    const float          speed = std::max( 0.f, float{ cvar::rt_barrel_speed } );
    const float          bias  = std::max( 0.f, float{ cvar::rt_barrel_up } );

    FVector3 upn = up;
    if( upn.LengthSquared() < 1e-6f )
    {
        upn = FVector3{ 0, 0, 1 };
    }
    upn.MakeUnit();

    // The sector the burst happens in, resolved ONCE. Every shard starts in it,
    // and the sim's tier-1 collision needs a starting sector or it cannot find a
    // floor to bounce off -- a shard born with a null sector falls forever.
    sector_t* sec = primaryLevel->PointInSector( double( at.X ) / double{ ONEGAMEUNIT_IN_METERS },
                                                double( at.Y ) / double{ ONEGAMEUNIT_IN_METERS } );

    int made = 0;
    for( int i = 0; i < want; i++ )
    {
        Spark* slotp = AllocSpark( SparkKind::Shard );
        if( !slotp )
        {
            break;
        }
        Spark& sp = *slotp;

        // A FULL SPHERE, then biased up. A barrel bursts outward in every
        // direction -- unlike an impact, there is no surface to reflect off and
        // no cone to build -- but with a purely spherical throw half the plate
        // goes straight into the floor and is never seen. The bias is what puts
        // the burst in the air where it can be watched.
        FVector3 dir{ rnd11(), rnd11(), rnd11() };
        if( dir.LengthSquared() < 1e-6f )
        {
            dir = upn;
        }
        dir.MakeUnit();
        dir += upn * bias;
        dir.MakeUnit();

        // Born a little off the centre rather than all from one point, or the
        // burst reads as a fountain from a pinhole instead of a barrel coming
        // apart. Scaled by the piece size so it stays proportionate.
        const FVector3 jitter{ rnd11(), rnd11(), rnd11() };

        // TWO POPULATIONS, AND A BARREL NEEDS BOTH.
        //
        // The authored cut-outs are the big recognisable plates -- charred, torn,
        // hot along the break -- and four of them are the whole vocabulary. A
        // burst made only of those reads as four objects rather than as
        // something coming apart, and with a dozen pieces the same drawing is on
        // screen three times at once.
        //
        // So a share of every burst is SMALL GENERATED GRIT instead: the curved
        // torn strip in one of the barrel's own greys, at a fraction of the
        // size. It is exactly the geometry the art replaced, kept because at
        // fragment scale it is the right tool -- nobody can tell a 4 cm chip was
        // procedural, and hand-cutting a dozen of them would be work spent where
        // it cannot be seen.
        //
        // baseRgb IS THE SWITCH, and it is worth saying so out loud because it
        // is doing double duty: for a shard, 0 means "wear the authored art" and
        // non-zero means "generate the outline, in this colour". That saves a
        // field on a struct there are four thousand of.
        const bool isGrit = ( artCount == 0 ) ||
                           ( rnd01() < std::clamp( float{ cvar::rt_barrel_small_frac }, 0.f, 1.f ) );

        const float sizeMul =
            isGrit ? std::max( 0.05f, float{ cvar::rt_barrel_small_size } ) : 1.f;

        sp.pos  = at + jitter * ( size * 1.5f );
        sp.vel  = dir * ( speed * ( 0.45f + 0.85f * rnd01() ) );
        sp.age = 0.f;
        // 0 MEANS FOREVER, the same sentinel rt_arc_burn_life uses, and it means
        // the same thing there: forever is really "until the pool evicts it".
        // Wreckage has no reason to weather on the timescale of a firefight, so
        // what bounds how much of it a level accumulates is rt_spark_max and the
        // evict-within-kind rule, not a clock. Saying so with a sentinel is
        // clearer than picking a number large enough to look permanent.
        sp.life = life <= 0.f ? FLT_MAX : life * ( 0.75f + 0.5f * rnd01() );
        // A WIDE size spread, wider than debris takes. A barrel does not come
        // apart into equal pieces: there are two or three big plates and a
        // scatter of smaller stuff, and uniform chunks read as manufactured.
        // THE SIZE SPREAD, and it is a CURVE rather than a range because the
        // range alone gave the wrong shape of burst.
        //
        // A uniform 0.45x..1.7x makes every piece roughly medium: the extremes
        // are as likely as the middle, so twelve pieces come out twelve similar
        // sizes and the burst reads as a set of parts rather than as something
        // that broke. What a barrel actually leaves is ONE OR TWO BIG PANELS and
        // a lot of smaller stuff.
        //
        // Raising rnd01() to a power biases the draw toward the low end, so the
        // big multiplier at the top is RARE rather than typical -- which is what
        // lets the ceiling be as high as 3x without every burst being a pile of
        // giant plates. Reported as needing "more variety in term of bigger":
        // the answer is not a bigger uniform size, it is a longer tail.
        const float lo   = std::max( 0.05f, float{ cvar::rt_barrel_size_min } );
        const float hi   = std::max( lo, float{ cvar::rt_barrel_size_max } );
        const float bias = std::max( 0.2f, float{ cvar::rt_barrel_size_bias } );
        const float tsz  = std::pow( rnd01(), bias );

        sp.size = size * sizeMul * ( lo + ( hi - lo ) * tsz );
        sp.sec     = sec;
        sp.settled = false;
        sp.kind    = SparkKind::Shard;
        sp.surf    = SurfKind::Barrel;

        // THE BIRTH ORIENTATION, and for a shard this is real geometry rather
        // than the shading hint it is for debris: it is the plate's own face
        // normal, which the draw tumbles about a world axis and which the sim
        // overwrites with the floor's normal when the piece comes to rest.
        // Random, because a barrel's staves point every way round it.
        FVector3 n{ rnd11(), rnd11(), rnd11() };
        if( n.LengthSquared() < 1e-6f )
        {
            n = upn;
        }
        n.MakeUnit();
        sp.nrm = n;

        sp.phase = rnd01() * 2.f * rt_pi();
        // Grit tumbles faster than plate. A quarter-metre panel turning quickly
        // strobes as it passes edge-on and reads as a rendering fault; a 4 cm
        // chip doing the same just reads as a chip.
        sp.spin = rnd11() * 3.2f * pr.spin * ( isGrit ? 2.2f : 1.f );
        // A MUCH WIDER SHAPE RANGE FOR THE GRIT, and this is the answer to "just
        // don't make them simple squares". Aspect 1.0 with a low tear is a
        // square; the range below reaches from a sliver to a stubby wedge and
        // never sits at 1 for long. The authored pieces ignore this -- their
        // proportions come from the image.
        sp.aspect = isGrit ? ( 0.28f + rnd01() * 1.15f )
                          : ( pr.aspectLo + rnd01() * ( pr.aspectHi - pr.aspectLo ) );
        sp.sid    = NextSparkSid();
        // 0 = wear the art; non-zero = generate the outline in this grey.
        sp.baseRgb = isGrit ? ShardShade( sp.sid ) : 0u;

        made++;
        s_dbgSpawned++;
    }

    if( cvar::rt_barrel_debug )
    {
        Printf( "rt_barrel: %d shards at %.2f %.2f %.2f (pool %u)\n",
                made,
                at.X,
                at.Y,
                at.Z,
                g_sparkCount );
    }
}

// ---------------------------------------------------------------------------

// Everything one exploding barrel produces. `mo` is alive here -- this is called
// straight out of the walk that found it.
void BarrelBlow( AActor* mo )
{
    // THE SCORCH GOES ON THE FLOOR, NOT AT THE BARREL'S MIDDLE. A barrel is 50
    // units tall and the blast is modelled at its centre, so a mark placed at
    // the actor's position would hang in mid-air a metre up, facing nothing.
    const double px = mo->X();
    const double py = mo->Y();

    sector_t* sec = mo->Sector ? mo->Sector : primaryLevel->PointInSector( px, py );

    double   fz = mo->Z();
    FVector3 fn{ 0, 0, 1 };
    if( sec )
    {
        fz = sec->floorplane.ZatPoint( px, py );
        const DVector3 n = sec->floorplane.Normal();
        FVector3       nf{ float( n.X ), float( n.Y ), float( n.Z ) };
        if( nf.LengthSquared() > 1e-6f )
        {
            nf.MakeUnit();
            fn = nf;
        }
    }

    const FVector3 floorAt{ float( px ) * ONEGAMEUNIT_IN_METERS,
                            float( py ) * ONEGAMEUNIT_IN_METERS,
                            float( fz ) * ONEGAMEUNIT_IN_METERS };

    // The burst centre: the barrel's own middle, which is where the fireball is.
    const FVector3 midAt{ float( px ) * ONEGAMEUNIT_IN_METERS,
                          float( py ) * ONEGAMEUNIT_IN_METERS,
                          float( mo->Z() + mo->Height * 0.5 ) * ONEGAMEUNIT_IN_METERS };

    // A SPAWN cull, not a render cull, for the reason rt_spark_far is one:
    // eviction is oldest-out while the thing worth keeping is the blast in front
    // of you, so a barrel going off across the map must not push it out.
    {
        const auto&    vp = r_viewpoint;
        const FVector3 eye{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                            float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                            float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };
        const float    far_m = std::max( 0.f, float{ cvar::rt_barrel_far } );
        if( ( midAt - eye ).LengthSquared() > far_m * far_m )
        {
            if( cvar::rt_barrel_debug )
            {
                Printf( "rt_barrel: blast at %.0f %.0f CULLED (beyond %.0f m)\n",
                        px,
                        py,
                        far_m );
            }
            return;
        }
    }

    if( cvar::rt_barrel_debug )
    {
        Printf( "rt_barrel: BLAST %s at %.0f %.0f %.0f, floor %.0f (tic %d)\n",
                mo->GetClass()->TypeName.GetChars(),
                px,
                py,
                mo->Z(),
                fz,
                primaryLevel->maptime );
    }

    // THE SCORCH IS THE ROCKET'S, WIDER. Not a new primitive and deliberately
    // not new knobs: it is one kind of thing -- burnt floor -- and the whole
    // rt_arc_burn_* group (darkness, mottle, blob count, lifetime) already
    // describes it. Only the RADIUS is the barrel's own, because that is the
    // only thing a barrel scorch has that a rocket's does not.
    //
    // ImpactFx::Ember rather than Arc is what puts coals in it: the same
    // mechanism the rocket uses, which also gives them their smoke threads for
    // free. An exploding barrel leaving embers in the char is exactly right, and
    // rt_barrel_embers switches it back to a bare scorch if it ever is not.
    if( cvar::rt_barrel_scorch )
    {
        SpawnArcMark( floorAt,
                      fn,
                      ArcFlavor::Plasma, // unread: the ember path never indexes the arc ramp
                      /*withArcs=*/false,
                      std::max( 0.f, float{ cvar::rt_barrel_scorch_scale } ),
                      cvar::rt_barrel_embers ? ImpactFx::Ember : ImpactFx::Arc,
                      std::max( 0.f, float{ cvar::rt_barrel_ember_scale } ),
                      bool( cvar::rt_barrel_ember_tex ),
                      std::max( 0.05f, float{ cvar::rt_barrel_ember_size } ),
                      std::max( 0.f, float{ cvar::rt_barrel_ember_bright } ),
                      std::max( 0.f, float{ cvar::rt_barrel_ember_scatter } ) );
    }

    SpawnBarrelShards( midAt, fn );
}

// ---------------------------------------------------------------------------

void BarrelWalkActor( AActor* mo, int tic )
{
    if( !cvar::rt_barrel || !mo )
    {
        return;
    }

    // BY SPRITE, NOT BY CLASS NAME, and that is the same choice RT_BarrelSmoke
    // makes. The actor classes belong to the Retribution WAD; matching the art
    // catches every barrel that plays the explosion, including any a future WAD
    // adds, without this file having to hold a list of class names it cannot
    // edit.
    if( mo->sprite < 0 || mo->sprite >= int( sprites.Size() ) )
    {
        return;
    }
    const char* sn = sprites[ mo->sprite ].name;
    if( !sn || strnicmp( sn, "BEXP", 4 ) != 0 )
    {
        return;
    }

    BarrelMark* mark = nullptr;
    for( BarrelMark& b : g_barrels )
    {
        if( b.mo == mo )
        {
            mark = &b;
            break;
        }
    }
    if( !mark )
    {
        g_barrels.push_back( BarrelMark{ mo, tic, false } );
        mark = &g_barrels.back();
    }
    mark->lastTic = tic;

    // Frame 'E', where A_Explode sits. Held for 21 tics across three state
    // lines, so this MUST be an edge and not a test.
    const bool blowing = ( mo->frame == 4 );
    const bool edge    = blowing && !mark->blowing;
    mark->blowing      = blowing;

    if( edge )
    {
        BarrelBlow( mo );
    }
}

void BarrelSweep( int tic )
{
    for( size_t i = 0; i < g_barrels.size(); )
    {
        if( g_barrels[ i ].lastTic == tic )
        {
            i++;
            continue;
        }
        g_barrels[ i ] = g_barrels.back();
        g_barrels.pop_back();
    }
}

void BarrelForgetAll()
{
    g_barrels.clear();
}

} // namespace rtsp

using namespace rtsp;

// ---------------------------------------------------------------------------
// THE LAB HOOK, and it exists for the reason `arc_here` does.
//
// Judging barrel plate by finding a barrel and shooting it means the burst
// lands wherever the barrel happened to be, in whatever room, with the
// explosion's own smoke and light on top of it for the first second, and the
// pieces gone before you can walk round them. None of that is the thing being
// looked at and none of it repeats between runs.
//
// This drops one burst at the crosshair with nothing else happening, so a value
// can be changed, rebuilt, captured and compared with only the value having
// moved. The per-shard seeds still vary, which is deliberate: the shapes are
// SUPPOSED to differ, and a capture that hid that would be lying about the
// effect.
//
//   barrel_here              scorch + plate, 150 units in front
//   barrel_here shards       plate only, no scorch
//   barrel_here scorch       scorch only, no plate
//   barrel_here all 90       both, 90 units in front (closer, for shape)
CCMD( barrel_here )
{
    if( !primaryLevel )
    {
        Printf( "barrel_here: no level\n" );
        return;
    }

    bool wantShards = true;
    bool wantScorch = true;
    if( argv.argc() > 1 )
    {
        if( stricmp( argv[ 1 ], "shards" ) == 0 )
        {
            wantScorch = false;
        }
        else if( stricmp( argv[ 1 ], "scorch" ) == 0 )
        {
            wantShards = false;
        }
    }

    // ON THE FLOOR A SHORT WAY IN FRONT, not at whatever a forward trace hits.
    // A barrel sits on a floor and bursts upward off it; a lab that planted the
    // burst on a wall would be reproducing something the effect never does.
    // Ignoring pitch is the point -- where you happen to be looking must not
    // change the capture.
    // HOW FAR IN FRONT, and both ends of the range have bitten already.
    //
    // The first try was 72 units. That is about a metre, which puts the burst
    // directly BEHIND THE WEAPON SPRITE covering the bottom centre of the frame:
    // the ladder reported 12 shards live and 17 quads uploaded while the capture
    // showed an empty room, so it looked exactly like a draw bug and was a
    // framing one. 220 cleared the gun but is far enough that a 20-unit piece is
    // a few pixels. 150 is the compromise, and the argument exists because the
    // right answer depends on what is being judged.
    double dist = 150.0;
    if( argv.argc() > 2 )
    {
        dist = std::clamp( atof( argv[ 2 ] ), 48.0, 1024.0 );
    }

    const auto&  vp  = r_viewpoint;
    const double yaw = vp.Angles.Yaw.Radians();
    const double ox  = vp.Pos.X + std::cos( yaw ) * dist;
    const double oy  = vp.Pos.Y + std::sin( yaw ) * dist;

    sector_t* sec = primaryLevel->PointInSector( ox, oy );

    double   fz = vp.Pos.Z;
    FVector3 fn{ 0, 0, 1 };
    if( sec )
    {
        fz               = sec->floorplane.ZatPoint( ox, oy );
        const DVector3 n = sec->floorplane.Normal();
        FVector3       nf{ float( n.X ), float( n.Y ), float( n.Z ) };
        if( nf.LengthSquared() > 1e-6f )
        {
            nf.MakeUnit();
            fn = nf;
        }
    }

    const FVector3 floorAt{ float( ox ) * ONEGAMEUNIT_IN_METERS,
                            float( oy ) * ONEGAMEUNIT_IN_METERS,
                            float( fz ) * ONEGAMEUNIT_IN_METERS };
    const FVector3 midAt{ floorAt.X, floorAt.Y, floorAt.Z + 25.f * ONEGAMEUNIT_IN_METERS };

    if( wantScorch )
    {
        SpawnArcMark( floorAt,
                      fn,
                      ArcFlavor::Plasma,
                      false,
                      std::max( 0.f, float{ cvar::rt_barrel_scorch_scale } ),
                      cvar::rt_barrel_embers ? ImpactFx::Ember : ImpactFx::Arc,
                      std::max( 0.f, float{ cvar::rt_barrel_ember_scale } ),
                      bool( cvar::rt_barrel_ember_tex ),
                      std::max( 0.05f, float{ cvar::rt_barrel_ember_size } ),
                      std::max( 0.f, float{ cvar::rt_barrel_ember_bright } ),
                      std::max( 0.f, float{ cvar::rt_barrel_ember_scatter } ) );
    }
    if( wantShards )
    {
        SpawnBarrelShards( midAt, fn );
    }

    Printf( "barrel_here: %s%s at %.0f %.0f %.0f\n",
            wantScorch ? "scorch " : "",
            wantShards ? "shards" : "",
            ox,
            oy,
            fz );
}
