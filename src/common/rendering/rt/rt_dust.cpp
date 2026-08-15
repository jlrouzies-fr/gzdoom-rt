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
//     (rt_sparks.cpp). So these are opaque with alpha 1 and no BLEND flag,
//     which is RTGL1's rule for entering the AS, and their vertex colour is an
//     ALBEDO the path tracer shades rather than a final pixel. (The one flag
//     they do carry, NO_MOTION_VECTORS, is about the denoiser and does not
//     affect that -- see the streaks note below.)
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
// DUST IS MOSTLY FOR THE SHAFTS, so it is GATED ON THEM. Traced lighting alone
// does not give that: a room's ambient and its bounced GI reach everywhere, so
// every mote in the level picks up something and the field reads as an even
// haze of specks rather than as air made visible by a beam. Asked for from play
// as "mostly visible under shafts, barely when not".
//
// The gate is a per-mote WEIGHT, computed on the CPU from the very list that
// decides where shafts are (RT_ShaftLightsSelected) plus the moon, and it
// multiplies the mote's ALBEDO. Two things make that honest rather than a fudge:
//
//   - It is a proximity-and-phase weight, NOT a radiance. The tracer still
//     supplies how much light actually arrives, so no term is counted twice.
//   - VISIBILITY IS STILL THE TRACER'S. A mote near a lamp but behind a wall
//     gets a high weight and no light, so it stays black. The CPU never has to
//     answer the question it could not answer cheaply.
//
// The phase function is the part that makes it feel volumetric rather than
// merely local: the same forward bias RtVolumetric.rgen uses, so a mote between
// you and the lamp flares and one lit from behind you does not -- which is why
// a shaft is strong looking into it and weak from the side.
//
// THE STREAKS: A BATCHED FIELD HAS NO MOTION VECTORS, AND MUST SAY SO.
//
// screen/pointyTriangles.png -- long pale wedges fanning out from the screen
// centre, over a field of blocky squares. The squares are the motes; the wedges
// were the denoiser smearing them.
//
// RTGL1 matches a primitive to its previous-frame self by uniqueObjectID and
// then takes vertex i of this frame against vertex i of the last one
// (GeomInfoManager::FindPrevFrameData -> prevBaseVertexIndex). That is right for
// a mesh whose vertices mean the same thing every frame, and this batch is the
// opposite: the walk below is a CAMERA-RELATIVE cell sweep with a distance cull
// and a cone cull, so vertex i is a different mote as soon as the player moves
// and the vertex count changes with them. Every mote therefore reported a
// world-space delta of metres, drawn from whichever unrelated mote happened to
// occupy its slot last frame -- and a huge screen-space motion vector fanning
// out from the centre is exactly what a camera turn smears into.
//
// RG_MESH_PRIMITIVE_NO_MOTION_VECTORS is the answer and it is not a workaround:
// this field genuinely has no per-vertex correspondence between frames, so the
// honest thing is to say so and let the denoiser treat every mote as new. The
// alternative -- world-oriented motes with stable identity -- costs two or three
// quads each to stay visible from any angle and buys motion vectors for
// something that is a few pixels across and moves at a centimetre a second.
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

// The Schlick approximation to Henyey-Greenstein, NORMALISED so isotropic is 1.
//
// Deliberately the same function RtVolumetric.rgen scatters with, and normalised
// by the isotropic value 1/(4*pi) so it can be used as a weight rather than as
// radiance. At the shipping asymmetry of 0.5 it runs about 0.17 (light behind
// the viewer) to 5.8 (light in front) -- the ~11x swing that is why a shaft
// reads strongly looking into it and weakly from the side. Giving dust the same
// swing is what stops it looking like specks that merely happen to be lit.
//
// `tolight` and `toviewer` both point AWAY from the sample, matching the
// shader's convention: the peak is at cos = -1, i.e. looking toward the light.
inline float DustPhase( const FVector3& tolight, const FVector3& toviewer, float g )
{
    const float k = 1.55f * g - 0.55f * g * g * g;
    const float c = tolight.X * toviewer.X + tolight.Y * toviewer.Y + tolight.Z * toviewer.Z;
    const float d = 1.f + k * c;

    return ( 1.f - k * k ) / std::max( 1e-4f, d * d );
}

} // namespace

void RT_DrawDust()
{
    s_verts.clear();
    s_idx.clear();

    // primaryLevel NON-NULL IS NOT THE SAME AS "A MAP IS LOADED". On the title
    // screen and in the menus -- i.e. any launch without +map -- primaryLevel
    // exists but holds no geometry, and the PointInSector() call further down
    // walks a BSP that was never built. That is a null read at address 0 and it
    // took out the whole title screen: "GZDoom Very Fatal Error" before the first
    // frame, with plain DOOM II and no mod at all. +map hid it completely, which
    // is why it survived -- every launcher here passes one.
    //
    // Same emptiness test RT_UpdateSectorEmisThreshold already uses, so the two
    // agree on what "no map" means.
    if( !cvar::rt_dust || !primaryLevel || primaryLevel->sectors.Size() == 0 )
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

    // PER-MOTE VARIATION, and the reason it matters more than it sounds.
    //
    // Every mote had exactly one albedo and exactly one size, so a lit field
    // came out as a uniform sheet of identical squares -- reported as "too flat
    // / brown, not enough gray / black parts". That is not a lighting problem:
    // real dust is a mixture, mostly dark grit with a few bright flecks, and it
    // is the SPREAD that reads as dust rather than as a texture.
    //
    // Two knobs rather than one shared "variety", because brightness and size
    // are different looks and a single dial would move a settled one to reach
    // the other.
    const float varA = std::clamp( float{ cvar::rt_dust_variance }, 0.f, 1.f );
    const float varS = std::clamp( float{ cvar::rt_dust_size_var }, 0.f, 1.f );

    // How much of the far end motes shrink out over, as a fraction of the reach.
    // Was hardcoded at the last quarter; reported as fading too fast.
    const float fadeFrac = std::clamp( float{ cvar::rt_dust_fade }, 0.01f, 1.f );

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

    // THE SHAFT GATE. Set up once, applied per mote below.
    //
    // rt_dust_shaft_floor is the albedo a mote keeps where there is no shaft at
    // all: 1 is the ungated behaviour and 0 makes dust exist only inside beams.
    const float shaftFloor = std::clamp( float{ cvar::rt_dust_shaft_floor }, 0.f, 1.f );
    const bool  gateOn     = shaftFloor < 0.999f;

    // METRES. Its own radius rather than the light's, because this is asking
    // "is this mote in the beam", not "how bright is the lamp" -- the tracer
    // already answers the second one and would be counted twice.
    const float shaftR  = std::max( 0.05f, float{ cvar::rt_dust_shaft_radius } );
    const float shaftR2 = shaftR * shaftR;

    // The same asymmetry the lamp shafts scatter with, so the two agree by
    // construction: the sentinel below -1 means the shafts are sharing
    // rt_volume_lassymetry, and dust has to share whatever they did.
    const float asymCv = float{ cvar::rt_volume_shaft_asym };
    const float asym   = std::clamp(
        asymCv < -1.f ? float{ cvar::rt_volume_lassymetry } : asymCv, -1.f, 1.f );

    // Bound through a named empty rather than a ternary: `cond ? lvalue-ref :
    // prvalue` yields a PRVALUE, so the reference would bind to a full copy of
    // the light list every frame -- lifetime-extended and therefore silent.
    static const std::vector< RtShaftLight > s_noShafts;

    const std::vector< RtShaftLight >& shafts =
        gateOn ? RT_ShaftLightsSelected() : s_noShafts;

    // THE MOON, and it needs a gate of its own. It is the strongest shaft in the
    // game and a mote in one should blaze -- but a directional light has no
    // position, so proximity says nothing and the phase term alone would
    // brighten every mote in the level whenever the player happens to face the
    // moon's bearing, including in a sealed room it cannot reach. So the moon
    // only counts for a mote whose sector has a SKY CEILING, which is cheap,
    // exact for the case that matters, and wrong only for a mote under an
    // overhang -- where the tracer will find it shadowed and black it anyway.
    const bool moonOn = bool{ cvar::rt_dust_moon } && bool{ cvar::rt_sun };

    FVector3 moonToLight{ 0, 0, 1 };
    if( moonOn )
    {
        // rt_main.cpp builds the directional light's `direction` as the way the
        // light TRAVELS. A weight wants the way back to it, so this is negated.
        const float alt = float( cvar::rt_sun_a ) * rt_pi() / 180.f;
        const float azi = float( cvar::rt_sun_b ) * rt_pi() / 180.f;
        const float th  = std::clamp( rt_pi() / 2 - alt, 0.f, rt_pi() );

        moonToLight = FVector3{ std::sin( th ) * std::cos( azi ),
                                std::sin( th ) * std::sin( azi ),
                                std::cos( th ) };
        moonToLight.MakeUnit();
    }

    const float moonWeight = std::clamp( float{ cvar::rt_dust_moon_weight }, 0.f, 4.f );

    // Skip motes that are not in open air. A mote inside a floor or a wall is
    // lit by nothing and therefore invisible, so this is not a look change --
    // it is the quads not being built, which is density that goes where it can
    // be seen instead. It also gives the moon gate the sector it needs.
    const bool clip = bool{ cvar::rt_dust_clip };

    const int32_t c0x = int32_t( std::floor( ( cam.X - farM ) / cell ) );
    const int32_t c1x = int32_t( std::floor( ( cam.X + farM ) / cell ) );
    const int32_t c0y = int32_t( std::floor( ( cam.Y - farM ) / cell ) );
    const int32_t c1y = int32_t( std::floor( ( cam.Y + farM ) / cell ) );
    const int32_t c0z = int32_t( std::floor( ( cam.Z - farM ) / cell ) );
    const int32_t c1z = int32_t( std::floor( ( cam.Z + farM ) / cell ) );

    const float far2   = farM * farM;
    const float near2  = nearM * nearM;
    const float fadeAt = farM * ( 1.f - fadeFrac );

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

                // Open air only, and the sector the moon gate needs. Map units
                // here -- PointInSector is playsim, not renderer, space.
                const sector_t* sec = nullptr;
                if( clip || moonOn )
                {
                    const double mx = double( p.X ) / ONEGAMEUNIT_IN_METERS;
                    const double my = double( p.Y ) / ONEGAMEUNIT_IN_METERS;
                    const double mz = double( p.Z ) / ONEGAMEUNIT_IN_METERS;

                    sec = primaryLevel->PointInSector( DVector2( mx, my ) );

                    if( clip )
                    {
                        if( !sec )
                        {
                            continue;
                        }
                        const DVector2 at{ mx, my };
                        if( mz < sec->floorplane.ZatPoint( at ) ||
                            mz > sec->ceilingplane.ZatPoint( at ) )
                        {
                            continue;
                        }
                    }
                }

                // THE SHAFT WEIGHT. A proximity-and-phase term in 0..1, never a
                // radiance: how much light actually arrives is the tracer's
                // answer, and computing it here too would count it twice.
                float shaftW = 0.f;
                if( gateOn )
                {
                    // toviewer, matching the shader's convention -- away from
                    // the sample, toward the camera.
                    const FVector3 tv = -d / len;

                    for( const RtShaftLight& L : shafts )
                    {
                        FVector3 tl{ float( L.x ) * ONEGAMEUNIT_IN_METERS - p.X,
                                     float( L.y ) * ONEGAMEUNIT_IN_METERS - p.Y,
                                     float( L.z ) * ONEGAMEUNIT_IN_METERS - p.Z };

                        const float l2 = tl.LengthSquared();
                        if( l2 > shaftR2 * 16.f )
                        {
                            // Four radii out the proximity term is under 1/17
                            // and no phase peak recovers it. Skipping keeps the
                            // inner loop off most of the 32-light list.
                            continue;
                        }

                        // 1 at the light, 1/2 at rt_dust_shaft_radius. Not
                        // inverse square: that is the LIGHT's falloff and the
                        // tracer has already applied it.
                        const float prox = 1.f / ( 1.f + l2 / shaftR2 );

                        tl.MakeUnit();
                        shaftW = std::max( shaftW, prox * DustPhase( tl, tv, asym ) );

                        if( shaftW >= 1.f )
                        {
                            break;
                        }
                    }

                    if( moonOn && sec &&
                        sec->GetTexture( sector_t::ceiling ) == skyflatnum )
                    {
                        shaftW = std::max( shaftW,
                                           moonWeight * DustPhase( moonToLight, tv, asym ) );
                    }

                    shaftW = std::clamp( shaftW, 0.f, 1.f );
                }

                // ANGULAR SIZE FLOOR -- see the header. Whichever is larger of
                // the mote's world size and a constant number of pixels.
                float half = 0.5f * std::max( sizeW, len * sizeA );

                // Size spread. Uniform, unlike the brightness below: a dust
                // field wants a range of sizes, not a heap of tiny ones.
                if( varS > 0.f )
                {
                    half *= 1.f - varS * ( 1.f - DustHash01( cx, cy, cz, 12 ) );
                }

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

                // The gate, applied to the ALBEDO. Not to the size: shrinking
                // a mote outside a shaft would also delete it from reflections
                // and from the ordinary lighting a room legitimately gives it,
                // and "barely visible" is not "absent".
                float k = 1.f;
                if( gateOn )
                {
                    k = shaftFloor + ( 1.f - shaftFloor ) * shaftW;
                }

                // Brightness spread, SQUARED so the distribution is skewed dark:
                // a uniform draw gives as many bright motes as dim ones and
                // still reads as a sheet. Squaring puts most of the field near
                // the dark end with a few flecks catching the light, which is
                // what dust actually looks like -- and it is the grey and black
                // that were missing.
                if( varA > 0.f )
                {
                    const float u = DustHash01( cx, cy, cz, 11 );
                    k *= 1.f - varA * ( 1.f - u * u );
                }

                RgColor4DPacked32 mcol = col;
                if( k < 0.999f )
                {
                    mcol = rt.rgUtilPackColorFloat4D( hue.X * k, hue.Y * k, hue.Z * k, 1.f );
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
                        .color        = mcol,
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
                    "size %.1f..%.1f mm | shaft gate %s (floor %.2f, r %.1f m, "
                    "%d shaft lights, moon %s)\n",
                    emitted,
                    maxQuads,
                    cell,
                    farM,
                    1000.f * std::max( sizeW, nearM * sizeA ),
                    1000.f * std::max( sizeW, farM * sizeA ),
                    gateOn ? "on" : "off",
                    shaftFloor,
                    shaftR,
                    int( shafts.size() ),
                    moonOn ? "on" : "off" );
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
        // NO_MOTION_VECTORS -- see the header. Without it the denoiser reads
        // vertex i against a completely different mote's previous position and
        // smears the field into radial wedges. Verified live: the flag reaches
        // GeomInfoManager::FindPrevFrameData, which returns nullptr for it and
        // leaves prevBaseVertexIndex at UINT32_MAX, i.e. "no history" rather
        // than "wrong history".
        .flags                = RG_MESH_PRIMITIVE_NO_MOTION_VECTORS,
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
