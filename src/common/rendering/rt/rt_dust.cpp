// Dust motes floating in the air.
//
// The point is the light shafts. A beam is only a beam because something is in
// it, and the froxel medium supplies the smooth half of that -- a haze. What it
// cannot supply is the SPARKLE: specks drifting through the beam, bright while
// they are inside it and invisible the moment they leave. That is the half the
// eye reads as "there is air here", and it is what makes a shaft dramatic
// rather than merely present.
//
// THEY ARE REAL TRACED GEOMETRY, LIT BY THE SCENE. Not emissive, not
// rasterized, not tinted by hand:
//
//   - Emissive dust is FIREFLIES. A mote that carries its own light glows in a
//     pitch-black room, which is the opposite of the effect -- the whole trick
//     is that a speck is visible only where light reaches it, so the beam draws
//     itself and the dark stays dark. Nothing here sets `emissive`.
//   - Rasterized dust is FULLBRIGHT. RTGL1 keeps a TRANSLUCENT primitive out of
//     the acceleration structure and shades it not at all, which is exactly the
//     trap the spark batch lives with on purpose and debris was moved off
//     (rt_sparks.cpp). So these are opaque, alpha 1, no flags -- RTGL1's rule
//     for entering the AS -- and their vertex colour is an ALBEDO the path
//     tracer shades, not a final pixel.
//   - Which also means a mote is correctly SHADOWED. A speck in the shadow of
//     the grating that makes the shaft goes dark, and that is free.
//
// NO POOL, NO STATE, NO SPAWNING. Dust is not an event, it is a property of the
// room, so there is nothing to emit and nothing to age. The motes live on a
// hashed lattice fixed in WORLD space and the frame simply draws the cells near
// the camera. Consequences worth having: the density is uniform and exact, a
// mote does not drift or pop as the player moves, walking away and back shows
// the same dust, and there is no budget that can quietly run out. The whole
// system is a pure function of (camera, time).
//
// SIZE IS ANGULAR, and it has to be. A real mote is tens of microns and would
// be invisible; even a generous 8 mm speck at 10 m subtends about a fifth of a
// pixel, which under an upscaler is not a dim speck but a shimmering one. So a
// mote is drawn at whichever is larger of its world size and a fixed ANGULAR
// size, i.e. a constant few pixels at any distance. Every particle system does
// this and it is the difference between dust and noise.
//
// See docs/plan-light-shafts.md.

#include "rt_internal.h"

using namespace rtx;

namespace
{

// Its own id, in the same space as the spark and debris batches
// (rt_sparks.cpp): one primitive, replaced every frame.
constexpr uint64_t RT_DUST_MESH_ID = 0x1000000000000200ull;

std::vector< RgPrimitiveVertex > s_verts;
std::vector< uint32_t >          s_idx;

// A 3D integer hash. Three decorrelated streams from one cell index, used for
// the mote's offset within its cell and for its wobble phases.
//
// Deliberately integer-in, integer-out and stateless: the mote's identity is its
// CELL, so the same cell must produce the same mote on every frame, from every
// direction of approach, forever. A per-frame RNG would make dust boil.
inline uint32_t DustHash( int32_t x, int32_t y, int32_t z, uint32_t salt )
{
    uint32_t h = uint32_t( x ) * 0x8DA6B343u ^ uint32_t( y ) * 0xD8163841u ^
                 uint32_t( z ) * 0xCB1AB31Fu ^ ( salt * 0x9E3779B9u );
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

inline float DustHash01( int32_t x, int32_t y, int32_t z, uint32_t salt )
{
    return float( DustHash( x, y, z, salt ) & 0xFFFFFFu ) / float( 0x1000000u );
}

} // namespace

void RT_DrawDust()
{
    s_verts.clear();
    s_idx.clear();

    if( !cvar::rt_dust || !primaryLevel )
    {
        return;
    }

    const int maxQuads = std::clamp( int{ cvar::rt_dust_max }, 0, 8000 );
    if( maxQuads <= 0 )
    {
        return;
    }

    const float farM  = std::clamp( float{ cvar::rt_dust_far }, 1.f, 60.f );
    const float nearM = std::max( 0.f, float{ cvar::rt_dust_near } );
    if( nearM >= farM )
    {
        return;
    }

    // CELL SIZE IS DERIVED, NOT SET, so the quad count cannot run away. The
    // density asks for one spacing and the cap allows another; the coarser wins.
    // Deriving it this way means rt_dust_max is a genuine hard bound rather than
    // a cull that kicks in after the work is done -- and, more usefully, that
    // raising rt_dust_far never costs more than it is allowed to. It just thins.
    const float density = std::max( 0.0005f, float{ cvar::rt_dust_density } );
    const float volume  = ( 4.f / 3.f ) * rt_pi() * farM * farM * farM;

    const float cell =
        std::max( std::cbrt( 1.f / density ), std::cbrt( volume / float( maxQuads ) ) );

    // A camera-facing basis, and the view direction for the behind-cull. Same
    // construction as the spark batch, deliberately -- a second copy of this
    // that disagreed by a sign would be a very quiet bug.
    const auto&  vp    = r_viewpoint;
    const double yaw   = vp.Angles.Yaw.Radians();
    const double pitch = vp.Angles.Pitch.Radians();

    const FVector3 fwd{ float( std::cos( yaw ) * std::cos( pitch ) ),
                        float( std::sin( yaw ) * std::cos( pitch ) ),
                        float( -std::sin( pitch ) ) };

    FVector3 right = fwd ^ FVector3{ 0, 0, 1 };
    if( right.LengthSquared() < 1e-6f )
    {
        right = FVector3{ 1, 0, 0 };
    }
    right.MakeUnit();
    FVector3 up = right ^ fwd;
    up.MakeUnit();

    const FVector3 cam{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                        float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                        float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };

    // MAPTIME, not the wall clock, so dust stops in a paused game and in the
    // menus. It is in the world, not on the camera. TicFrac keeps it smooth
    // between tics -- at 35 Hz without it the drift would visibly step.
    const float tsec =
        ( float( primaryLevel->maptime ) + float( vp.TicFrac ) ) / float( TICRATE );

    const float drift = std::max( 0.f, float{ cvar::rt_dust_drift } );
    const float speed = std::max( 0.f, float{ cvar::rt_dust_speed } );
    const float sizeW = std::max( 0.f, float{ cvar::rt_dust_size } );
    const float sizeA = std::max( 0.f, float{ cvar::rt_dust_size_ang } );
    const float cone  = std::clamp( float{ cvar::rt_dust_cone }, -1.f, 1.f );

    // The albedo. Normalised to rt_dust_albedo the same way debris colours are:
    // the hex supplies the HUE and this pins the brightness, so recolouring dust
    // cannot silently make it lighter or darker.
    const uint32_t hex = uint32_t( cvar::rt_dust_color );
    FVector3       hue{ float( ( hex >> 16 ) & 0xFF ) / 255.f,
                        float( ( hex >> 8 ) & 0xFF ) / 255.f,
                        float( hex & 0xFF ) / 255.f };
    {
        const float lum = 0.2126f * hue.X + 0.7152f * hue.Y + 0.0722f * hue.Z;
        const float tgt = std::clamp( float{ cvar::rt_dust_albedo }, 0.f, 1.f );
        hue *= ( lum > 0.001f ) ? ( tgt / lum ) : 0.f;
        hue.X = std::min( hue.X, 1.f );
        hue.Y = std::min( hue.Y, 1.f );
        hue.Z = std::min( hue.Z, 1.f );
    }

    // ALPHA 1, ALWAYS. Below RTGL1's MESH_TRANSLUCENT_ALPHA_THRESHOLD (0.98) the
    // whole batch is demoted to the rasterized overlay and goes fullbright, so a
    // mote cannot be faded out -- it is SHRUNK instead, exactly as debris is.
    const RgColor4DPacked32 col = rt.rgUtilPackColorFloat4D( hue.X, hue.Y, hue.Z, 1.f );

    // Camera-facing quads have a cosmetic normal, but it must not be left
    // unstated: a diffuse surface facing away from every light is black, and
    // "why is the dust dark" would then be a normal problem masquerading as a
    // lighting one. Facing the viewer is the only defensible choice for a
    // billboard.
    const RgNormalPacked32 nrm = rt.rgUtilPackNormal( -fwd.X, -fwd.Y, -fwd.Z );

    const int32_t c0x = int32_t( std::floor( ( cam.X - farM ) / cell ) );
    const int32_t c1x = int32_t( std::floor( ( cam.X + farM ) / cell ) );
    const int32_t c0y = int32_t( std::floor( ( cam.Y - farM ) / cell ) );
    const int32_t c1y = int32_t( std::floor( ( cam.Y + farM ) / cell ) );
    const int32_t c0z = int32_t( std::floor( ( cam.Z - farM ) / cell ) );
    const int32_t c1z = int32_t( std::floor( ( cam.Z + farM ) / cell ) );

    const float far2   = farM * farM;
    const float near2  = nearM * nearM;
    const float fadeAt = farM * 0.75f;

    int emitted = 0;

    for( int32_t cz = c0z; cz <= c1z && emitted < maxQuads; cz++ )
    {
        for( int32_t cy = c0y; cy <= c1y && emitted < maxQuads; cy++ )
        {
            for( int32_t cx = c0x; cx <= c1x && emitted < maxQuads; cx++ )
            {
                // The mote's home, jittered inside its own cell so the lattice
                // never reads as a lattice. Hashed from the cell index, so it is
                // the same every frame.
                FVector3 p{ ( float( cx ) + DustHash01( cx, cy, cz, 1 ) ) * cell,
                            ( float( cy ) + DustHash01( cx, cy, cz, 2 ) ) * cell,
                            ( float( cz ) + DustHash01( cx, cy, cz, 3 ) ) * cell };

                // A LISSAJOUS WOBBLE, not a velocity, and that is a deliberate
                // trade. Integrating a drift would need per-mote state and would
                // eventually carry every mote out of its cell, which is what
                // makes a lattice system pop; three sines with incommensurate
                // periods stay bounded by construction, cost no memory, and
                // read as slow air movement rather than as anything periodic.
                // Real dust in still air does exactly this.
                if( drift > 0.f && speed > 0.f )
                {
                    const float p1 = DustHash01( cx, cy, cz, 5 ) * 2.f * rt_pi();
                    const float p2 = DustHash01( cx, cy, cz, 6 ) * 2.f * rt_pi();
                    const float p3 = DustHash01( cx, cy, cz, 7 ) * 2.f * rt_pi();

                    p.X += drift * std::sin( tsec * speed * 0.73f + p1 );
                    p.Y += drift * std::sin( tsec * speed * 0.91f + p2 );
                    // Vertical is slower and shallower: a mote that bobs as fast
                    // sideways as it does up and down reads as a bug, not as
                    // something falling through still air.
                    p.Z += drift * 0.6f * std::sin( tsec * speed * 0.41f + p3 );
                }

                const FVector3 d  = p - cam;
                const float    l2 = d.LengthSquared();

                if( l2 > far2 || l2 < near2 )
                {
                    continue;
                }

                const float len = std::sqrt( std::max( l2, 1e-8f ) );

                // BEHIND THE CAMERA IS SKIPPED, and it is a cone rather than a
                // frustum on purpose. A frustum test would be tighter and would
                // also delete every mote a mirror or a water surface reflects,
                // which is a visible hole; a generous cone keeps the ones just
                // off-screen and still removes the half of the sphere that can
                // never contribute.
                if( ( d.X * fwd.X + d.Y * fwd.Y + d.Z * fwd.Z ) / len < cone )
                {
                    continue;
                }

                // ANGULAR SIZE FLOOR -- see the header. Whichever is larger of
                // the mote's world size and a constant number of pixels.
                float half = 0.5f * std::max( sizeW, len * sizeA );

                // Shrink out over the last quarter of the range instead of
                // fading: the alpha is not available to us (see `col`).
                if( len > fadeAt )
                {
                    half *= std::max( 0.f, ( farM - len ) / ( farM - fadeAt ) );
                }

                if( half <= 0.f )
                {
                    continue;
                }

                const FVector3 ex = right * half;
                const FVector3 ey = up * half;

                const FVector3 corner[ 4 ] = {
                    p - ex - ey,
                    p + ex - ey,
                    p + ex + ey,
                    p - ex + ey,
                };
                const float uv[ 4 ][ 2 ] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

                const uint32_t base = uint32_t( s_verts.size() );

                for( int k = 0; k < 4; k++ )
                {
                    s_verts.push_back( RgPrimitiveVertex{
                        .position     = { corner[ k ].X, corner[ k ].Y, corner[ k ].Z },
                        .normalPacked = nrm,
                        .texCoord     = { uv[ k ][ 0 ], uv[ k ][ 1 ] },
                        .color        = col,
                    } );
                }

                s_idx.push_back( base + 0 );
                s_idx.push_back( base + 1 );
                s_idx.push_back( base + 2 );
                s_idx.push_back( base + 0 );
                s_idx.push_back( base + 2 );
                s_idx.push_back( base + 3 );

                emitted++;
            }
        }
    }

    if( cvar::rt_dust_debug )
    {
        static int s_tick;
        if( ( ++s_tick % 60 ) == 0 )
        {
            Printf( "rt_dust: %d motes (cap %d), cell %.2f m, reach %.1f m, "
                    "size %.1f..%.1f mm\n",
                    emitted,
                    maxQuads,
                    cell,
                    farM,
                    1000.f * std::max( sizeW, nearM * sizeA ),
                    1000.f * std::max( sizeW, farM * sizeA ) );
        }
    }

    if( s_verts.empty() )
    {
        return;
    }

    // WORLD-SPACE VERTICES, IDENTITY TRANSFORM -- the quads are already placed,
    // and a transform would move them twice.
    auto mesh = RgMeshInfo{
        .sType          = RG_STRUCTURE_TYPE_MESH_INFO,
        .pNext          = nullptr,
        .flags          = 0,
        .uniqueObjectID = RT_DUST_MESH_ID,
        // No mesh name, or RTGL1 hunts for an rt/replace/*.gltf substitute and
        // could swap a model in for the whole batch.
        .pMeshName            = nullptr,
        .transform            = RG_TRANSFORM_IDENTITY,
        .isExportable         = false,
        .animationTime        = 0.f,
        .localLightsIntensity = 1.f,
    };

    // Stated rather than left to a fallback, for the reason the debris batch
    // states it: these are the documented defaults for a primitive with no
    // roughness-metallic texture, and an unstated default is the kind of thing
    // that costs days here. Dust is a rough dielectric; it has no highlight.
    auto pbr = RgMeshPrimitivePBREXT{
        .sType            = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_PBR_EXT,
        .pNext            = nullptr,
        .metallicDefault  = 0.f,
        .roughnessDefault = 1.f,
    };

    auto prim = RgMeshPrimitiveInfo{
        .sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
        .pNext = &pbr,
        // NO FLAGS: with the alpha-1 colour above, that is RTGL1's rule for
        // entering the acceleration structure. So a mote is real traced
        // geometry, lit by the room and shadowed by whatever is between it and
        // the lamp -- which is the entire effect. TRANSLUCENT here would make it
        // a rasterized overlay and therefore fullbright, i.e. glowing dust in a
        // dark room.
        .flags                = RgMeshPrimitiveFlags( 0 ),
        .primitiveIndexInMesh = 0,
        .pVertices            = s_verts.data(),
        .vertexCount          = uint32_t( s_verts.size() ),
        .pIndices             = s_idx.data(),
        .indexCount           = uint32_t( s_idx.size() ),
        // No texture: RTGL1 samples its 1x1 white, so the colour is entirely the
        // vertex colour.
        .pTextureName = nullptr,
        .textureFrame = 0,
        .color        = RG_PACKED_COLOR_WHITE,
        // ZERO, and this is the line that decides whether this is dust or
        // fireflies. See the header.
        .emissive     = 0.f,
        .classicLight = 1.f,
    };

    RgResult r = rt.rgUploadMeshPrimitive( &mesh, &prim );
    RG_CHECK( r );
}
