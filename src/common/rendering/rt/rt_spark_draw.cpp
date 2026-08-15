// Doom64-RT: EVERY QUAD AND EVERY LIGHT THE IMPACT SYSTEM PUTS ON SCREEN.
//
// Split out of rt_sparks.cpp, which had reached 4126 lines -- of which this was
// the largest single piece by a wide margin. RT_DrawSparks alone ran to about
// 1400 lines and drew, in one function: spark particles, debris chips, the
// contact-AO blobs under settled debris, the scorch decal, the embers in it,
// their halos, the arc filigree, its creepers and its core.
//
// WHAT IS SHARED AND WHY. Sparks, arc branches and embers all land in ONE
// additive batch (s_batchSpark): same blend, same untextured white material,
// same mesh id, so a second batch would be a second uploadMeshPrimitive for no
// difference in state. Debris does NOT -- it is opaque traced geometry bucketed
// by colour, because the traced albedo comes from the PRIMITIVE and not from the
// vertices (pitfall 33), so one batch per distinct colour is the only way to
// give chips different albedos at all.
//
// THE TWO RENDERING PATHS BEHAVE DIFFERENTLY AND IT MATTERS AT EVERY TURN:
//   ADDITIVE (sparks, arcs, embers) -- TRANSLUCENT with emissive > 0, which
//   RasterizedDataCollector::ToPipelineState turns into a real additive blend.
//   Outside the acceleration structure, so it appears in no reflection, casts
//   no GI, and LIGHTS NOTHING. Brightness rides vertex ALPHA and nothing else.
//   TRACED (debris) -- opaque, in the BLAS, lit by the room. Alpha there is not
//   a look knob at all: it is the flag that decides which path you are on.
// Anything analytic -- every real light this system casts -- is collected here
// during the draw and uploaded by the light pass, from the geometry that was
// ACTUALLY emitted rather than recomputed. See ArcLightCand.

#include "rt_sparks_internal.h"

#include <algorithm>

using namespace rtx;

namespace rtsp
{

// CONTACT OCCLUSION UNDER SETTLED DEBRIS.
//
// Straight from docs/sprite-shadows-and-ao.md 2: a chip lying on a floor has the
// same problem a dropped weapon does -- with no contact term it reads as
// hovering, and a cast shadow cannot supply one because a flat fragment lit from
// a shallow angle throws almost nothing. A blob answers "is something touching
// this floor", which has the same answer from every light direction.
//
// Everything about the technique is that document's, and the three rules it
// bought the hard way are all load-bearing here too:
//   - WORLD-SPACE VERTS, IDENTITY TRANSFORM. RsDecal.vert writes
//     outWorldPos = position untransformed while transforming gl_Position, so a
//     transform makes the quad rasterize in the right place and then discard
//     100% of its fragments -- invisible, silently, with nothing logged.
//   - A SINGLE FAN per blob, centre at alpha and rim at 0. A high-alpha ring
//     creases along every quad diagonal; more segments does not fix it.
//   - emissive = 0, or the decal shader falls back to ldrEmis = albedo and the
//     blob GLOWS.
QuadBatch s_batchDebrisAo;
// Scratch for one scorch fan. Reused per mark rather than accumulated: a decal
// is uploaded one primitive at a time (pitfall 34), so there is never more than
// a single fan in here.
QuadBatch s_batchArcBurn;

QuadBatch s_batchSpark;

// ONE BATCH PER PIECE OF BARREL ART. A primitive carries exactly one texture,
// so this is not a grouping choice the way the debris buckets are -- it is the
// minimum: four pieces of art is four primitives however few shards are alive.
QuadBatch s_batchShard[ RT_BARREL_ART_SLOTS ];

// The coals, when they wear the authored ember sheet.
//
// OPAQUE AND ALPHA-TESTED, NOT ADDITIVE, and that is not a style choice -- it is
// the only path where the art exists at all.
//
// The first version put them in the additive batch with the texture name on it,
// which is the obvious thing: a coal glows, additive is how everything else here
// glows. It rendered NOTHING. The isolation run is what settled it -- with
// rt_barrel_ember_tex 0 the same fifty coals came back as blazing white squares,
// so the geometry, the positions and the count were all fine and the TEXTURE was
// the difference.
//
// The reason is the two-path split this file already documents from the other
// side. An additive primitive is TRANSLUCENT, i.e. a RASTERIZED overlay, and the
// rasterized path samples the texture that was PROVIDED through
// rgProvideOriginalTexture -- which for art on disk is the 1x1 transparent
// placeholder standing in for the rt/mat override. Transparent times additive is
// nothing. The rt/mat substitution happens in the MATERIAL system, which only
// the traced path consults; that is exactly why the barrel plate, which is
// opaque, has worked from its first run.
//
// So a textured coal is real traced geometry with an emissive term instead: it
// glows because the primitive is emissive, not because the blend adds. That is
// the better answer anyway -- it is in the acceleration structure, so it shows
// up in reflections, which an overlay never could.
//
// ONE PRIMITIVE PER MARK rather than one batch for all of them, because the
// emissive strength is the only place a coal's COOLING can live once the texture
// owns its colour: the primitive colour is ignored when an albedo texture is
// present (HitInfo.inl), so a per-mark emissive scaled by the mark's fade is
// what makes the bed dim as it dies.
QuadBatch s_batchEmberArt;

// DEBRIS IS BATCHED BY COLOUR, and that is forced by how RTGL1 shades traced
// geometry rather than by anything about the effect.
//
// HitInfo.inl, for a surface with no albedo texture:
//
//     // if no albedo textures, use primary color
//     dst = mix( unpackUintColor( layerColors[0] ).rgb, dst, float( hasAnyAlbedoTexture ) );
//
// `layerColors[0]` is the PER-PRIMITIVE colour (RgMeshPrimitiveInfo::color).
// The per-VERTEX colour is stored -- ShVertex has a `color` field -- but the
// traced albedo path never reads it. So while debris was rasterized the vertex
// colours worked, and the moment it became traced geometry every chip took
// prim.color, which was RG_PACKED_COLOR_WHITE. Chips were white BY CONSTRUCTION,
// and no value of rt_spark_debris_albedo or _tint could move them; the arm that
// proved it set albedo to 0.02 and they stayed white.
//
// A primitive therefore carries exactly one albedo, so per-particle colour needs
// either one primitive per particle (thousands of uploads and BLAS entries) or
// particles grouped by colour. Grouping is far cheaper and the quantisation is
// invisible: chip colour comes from a handful of wall textures and a slow age
// ramp, so a room in play uses a few buckets out of the cap.
//
// The key carries the CLASS as well as the colour, so a class that later names a
// sprite in its profile still gets its own primitive and its own material.
constexpr int RT_DEBRIS_BUCKETS = 32;

struct DebrisBucket
{
    uint32_t  key;   // class << 12 | quantised rgb
    uint32_t  rgb;   // the representative colour uploaded as prim.color
    SurfKind  kind;
    QuadBatch batch;
};

DebrisBucket s_debrisBuckets[ RT_DEBRIS_BUCKETS ];
int          s_debrisBucketCount = 0;

// 4 bits a channel. Finer buckets do not buy anything the eye can see here and
// only raise the chance of hitting the cap in a room with many wall textures.
uint32_t DebrisColorKey( SurfKind k, float r, float g, float b )
{
    const uint32_t qr = uint32_t( std::clamp( r, 0.f, 1.f ) * 15.f + 0.5f );
    const uint32_t qg = uint32_t( std::clamp( g, 0.f, 1.f ) * 15.f + 0.5f );
    const uint32_t qb = uint32_t( std::clamp( b, 0.f, 1.f ) * 15.f + 0.5f );
    return ( uint32_t( k ) << 12 ) | ( qr << 8 ) | ( qg << 4 ) | qb;
}

QuadBatch& DebrisBucketFor( SurfKind k, float r, float g, float b )
{
    const uint32_t key = DebrisColorKey( k, r, g, b );

    for( int i = 0; i < s_debrisBucketCount; i++ )
    {
        if( s_debrisBuckets[ i ].key == key )
        {
            return s_debrisBuckets[ i ].batch;
        }
    }

    if( s_debrisBucketCount < RT_DEBRIS_BUCKETS )
    {
        DebrisBucket& nb = s_debrisBuckets[ s_debrisBucketCount++ ];
        nb.key           = key;
        nb.kind          = k;
        nb.rgb           = ( uint32_t( std::clamp( r, 0.f, 1.f ) * 255.f + 0.5f ) << 16 ) |
                 ( uint32_t( std::clamp( g, 0.f, 1.f ) * 255.f + 0.5f ) << 8 ) |
                 uint32_t( std::clamp( b, 0.f, 1.f ) * 255.f + 0.5f );
        return nb.batch;
    }

    // Cap reached: fold into the first bucket rather than dropping the chip. A
    // slightly wrong colour is a far smaller artefact than debris vanishing,
    // and the cap is generous enough that this should not be reachable in play.
    return s_debrisBuckets[ 0 ].batch;
}

void UploadBatch( const QuadBatch&     b,
                  uint64_t             meshId,
                  bool                 additive,
                  const char*          texName,
                  RgColor4DPacked32    primColor,
                  RgMeshPrimitiveFlags extra,
                  float                emissive )
{
    if( b.verts.empty() )
    {
        return;
    }

    // WORLD-SPACE VERTICES, IDENTITY TRANSFORM. Not for the decal shader's sake
    // -- this is not a decal -- but because the quads are already positioned in
    // world space and a transform would move them twice.
    auto mesh = RgMeshInfo{
        .sType          = RG_STRUCTURE_TYPE_MESH_INFO,
        .pNext          = nullptr,
        .flags          = 0,
        .uniqueObjectID = meshId,
        // No mesh name, or RTGL1 hunts for an rt/replace/*.gltf substitute and
        // could swap a model in for the whole batch.
        .pMeshName = nullptr,
        .transform =
            RgTransform{ {
                { 1, 0, 0, 0 },
                { 0, 1, 0, 0 },
                { 0, 0, 1, 0 },
            } },
        .isExportable  = false,
        .animationTime = 0.f,
        // 1.0 IS THE DOCUMENTED DEFAULT (RgMeshInfo, "Default: 1.0"). This was
        // 0, which is a real value meaning "take no local light" -- harmless for
        // the additive spark batch, which is unlit either way, but wrong for
        // debris the moment it became traced geometry.
        .localLightsIntensity = 1.f,
    };

    // EXPLICIT PBR FOR THE TRACED BATCH, and it is worth being clear about what
    // this does and does not fix.
    //
    // Asked from play, after debris still blew out under the flashlight: "should
    // we give them light absorption property? like PBR?" Right instinct, wrong
    // lever -- and the header says why. RgMeshPrimitivePBREXT documents its
    // defaults as roughness 1.0 and metallic 0.0 when no roughness-metallic
    // texture is present, which debris has none of. So it was ALREADY the
    // roughest, least shiny dielectric available, and a rough diffuse surface
    // returns albedo * E / pi whatever its roughness. Nothing here can reduce a
    // whiteout; that is albedo against irradiance, and rt_spark_debris_albedo is
    // the only surface-side term in it.
    //
    // Set anyway, because "no material at all" leaves what ASManager falls back
    // to unstated, and an unstated default is exactly the kind of thing that has
    // cost this project days. Stating it costs one struct and removes the doubt.
    auto pbr = RgMeshPrimitivePBREXT{
        .sType           = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_PBR_EXT,
        .pNext           = nullptr,
        .metallicDefault = std::clamp( float{ cvar::rt_spark_debris_metal }, 0.f, 1.f ),
        .roughnessDefault = std::clamp( float{ cvar::rt_spark_debris_rough }, 0.f, 1.f ),
    };

    auto prim = RgMeshPrimitiveInfo{
        .sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
        // Only the traced batch takes it: the additive spark overlay is not
        // shaded at all, so a PBR block on it would be noise.
        .pNext = additive ? nullptr : &pbr,
        // THE EMISSIVE BELOW IS WHAT PICKS THE BLEND MODE, and that is the entire
        // reason these are two batches rather than one.
        // RasterizedDataCollector::ToPipelineState turns a TRANSLUCENT primitive
        // with emissive > 0 into ADDITIVE (SRC_ALPHA, ONE); with emissive == 0 it
        // stays an ordinary alpha blend (SRC_ALPHA, ONE_MINUS_SRC_ALPHA).
        //
        // For a translucent SPRITE the additive promotion is the bug behind
        // rt_spectre_alpha reading as inert; for a hot spark it is precisely the
        // blend wanted, which is why sparks need no art and no alpha tuning.
        //
        // The cost is real and applies to SPARKS ONLY: a translucent primitive
        // is a rasterized overlay kept out of the acceleration structure, so the
        // spark batch appears in no reflection and casts no GI. The lights in
        // RT_UploadSparkLights are its traced half. DEBRIS pays none of that --
        // it is opaque and traced, which is exactly why it takes the room's
        // light and sparks do not.
        // ADDITIVE (sparks) -> TRANSLUCENT, a rasterized overlay.
        // OPAQUE (debris)    -> no flags at all, which with the alpha-1 colour
        //                       below is RTGL1's rule for entering the
        //                       ACCELERATION STRUCTURE -- so debris is real
        //                       ray-traced geometry, lit by the scene, casting
        //                       and receiving shadows, visible in reflections.
        .flags = RgMeshPrimitiveFlags(
            ( additive ? RG_MESH_PRIMITIVE_TRANSLUCENT : RgMeshPrimitiveFlags( 0 ) ) | extra ),
        .primitiveIndexInMesh = 0,
        .pVertices            = b.verts.data(),
        .vertexCount          = uint32_t( b.verts.size() ),
        .pIndices             = b.idx.data(),
        .indexCount           = uint32_t( b.idx.size() ),
        // No texture: RTGL1 samples its 1x1 white, so the colour is entirely the
        // vertex colour. That is what makes a spark one flat pixel.
        .pTextureName = texName,
        .textureFrame = 0,
        // THE ALBEDO, for the traced batch -- see the note above the buckets.
        // Alpha must stay 1: below 0.98 RTGL1 demotes the primitive to the
        // rasterized overlay and it goes fullbright again.
        .color = primColor,
        // -1 means "whatever this batch's blend implies"; a caller passes a real
        // value only when it has an _e map to scale.
        .emissive = emissive >= 0.f ? emissive : ( additive ? 1.f : 0.f ),
        .classicLight = 1.f,
    };

    RgResult r = rt.rgUploadMeshPrimitive( &mesh, &prim );
    RG_CHECK( r );
}

// ONE PRIMITIVE PER BLOB, and that is not an arbitrary choice -- it is the only
// structural difference between this and the sprite AO in rt_draw.cpp, which
// ships and is correct.
//
// The first version batched every blob into a single decal primitive. It is
// geometrically sound -- no triangle spans two blobs, and each fan interpolates
// only its own vertices -- and it still produced AO "lines" reaching away from
// the chips, through a distance cull that ruled out the documented 5 cm
// grazing-floor limit. Rather than keep reasoning about what a batched decal
// means to RTGL1's rasterizer, this matches the shape of the implementation that
// is known to work: one fan, one primitive, one uniqueObjectID, exactly as
// docs/sprite-shadows-and-ao.md describes.
//
// The cost is bounded by rt_spark_debris_ao_max rather than by the pool, because
// only SETTLED chips get a blob and a decal upload is not free.
void UploadAoBlob( const QuadBatch& b, uint64_t meshId )
{
    if( b.verts.empty() )
    {
        return;
    }

    auto mesh = RgMeshInfo{
        .sType          = RG_STRUCTURE_TYPE_MESH_INFO,
        .pNext          = nullptr,
        .flags          = 0,
        .uniqueObjectID = meshId,
        .pMeshName      = nullptr,
        // IDENTITY -- see the note above the batch.
        .transform =
            RgTransform{ {
                { 1, 0, 0, 0 },
                { 0, 1, 0, 0 },
                { 0, 0, 1, 0 },
            } },
        .isExportable         = false,
        .animationTime        = 0.f,
        .localLightsIntensity = 0.f,
    };

    auto prim = RgMeshPrimitiveInfo{
        .sType                = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
        .pNext                = nullptr,
        .flags                = RG_MESH_PRIMITIVE_DECAL,
        .primitiveIndexInMesh = 0,
        .pVertices            = b.verts.data(),
        .vertexCount          = uint32_t( b.verts.size() ),
        .pIndices             = b.idx.data(),
        .indexCount           = uint32_t( b.idx.size() ),
        // No texture: the falloff is vertex-colour interpolation, so this ships
        // no art and touches no material. RTGL1 samples its 1x1 white.
        .pTextureName = nullptr,
        .textureFrame = 0,
        .color        = RG_PACKED_COLOR_WHITE,
        // MUST be 0 -- see the note above the batch.
        .emissive     = 0.f,
        .classicLight = 1.f,
    };

    RgResult r = rt.rgUploadMeshPrimitive( &mesh, &prim );
    RG_CHECK( r );
}


void UploadSparkGlowLights()
{
    if( !cvar::rt_spark_glow )
    {
        return;
    }

    const float gi = std::max( 0.f, float{ cvar::rt_spark_glow_intensity } );
    const int   gmax = std::max( 0, int{ cvar::rt_spark_glow_max } );
    if( gi <= 0.f || gmax == 0 || g_sparkCount == 0 )
    {
        return;
    }

    const auto&    vp = r_viewpoint;
    const FVector3 eye{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                        float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                        float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };

    struct GCand
    {
        float    key;
        uint32_t idx;
    };
    static std::vector< GCand > cand;
    cand.clear();
    cand.reserve( g_sparkCount );

    for( uint32_t i = 0; i < g_sparkCount; i++ )
    {
        const Spark& sp = s_sparks[ i ];
        if( sp.kind != SparkKind::Spark )
        {
            continue;
        }
        const float t = std::clamp( sp.age / std::max( 1e-4f, sp.life ), 0.f, 1.f );
        // Rank by distance, biased by age: a young spark is both brighter and
        // more interesting than an old one at the same range, and without the
        // bias the cap fills with settled embers while the shower you just made
        // stays dark.
        const float d2 = ( sp.pos - eye ).LengthSquared();
        cand.push_back( GCand{ d2 * ( 1.f + 4.f * t * t ), i } );
    }

    if( cand.size() > size_t( gmax ) )
    {
        std::partial_sort( cand.begin(),
                           cand.begin() + gmax,
                           cand.end(),
                           []( const GCand& a, const GCand& b ) { return a.key < b.key; } );
        cand.resize( size_t( gmax ) );
    }

    for( const GCand& c : cand )
    {
        const Spark& sp = s_sparks[ c.idx ];

        const float t = std::clamp( sp.age / std::max( 1e-4f, sp.life ), 0.f, 1.f );
        const float k = ( 1.f - t ) * ( 1.f - t );
        if( k <= 0.001f )
        {
            continue;
        }

        // The particle's CURRENT palette colour, so the light it casts is the
        // same colour as the dot casting it -- a spark that has cooled to brown
        // must not still be throwing pale yellow on the wall, and an ember that
        // has gone dull red must not still be throwing yellow on the floor.
        const int      ci  = std::min( RT_SPARK_RAMP_N - 1,
                                       int( t * float( RT_SPARK_RAMP_N ) ) );
        const uint32_t rgb = RT_SPARK_RAMP[ ci ];

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( ( ( rgb >> 16 ) & 0xFF ) / 255.f,
                                                    ( ( rgb >> 8 ) & 0xFF ) / 255.f,
                                                    ( rgb & 0xFF ) / 255.f,
                                                    1.0f ),
            .intensity = gi * k,
            .position  = { sp.pos.X, sp.pos.Y, sp.pos.Z },
            .radius    = 0.03f,
        };
        auto info = RgLightInfo{
            .sType = RG_STRUCTURE_TYPE_LIGHT_INFO,
            // THE EXTENSION HANGS OFF pNext, AND OMITTING IT IS SILENT-ISH.
            // RTGL1's UploadLight looks for RgLightSphericalEXT (or one of its
            // three siblings) by walking pNext; find nothing and it warns
            // "Couldn't find RgLightDirectionalEXT, RgLightSphericalEXT, ... on
            // RgLightInfo (uniqueID=N)" and DROPS the light. Every other field
            // is valid, so nothing else complains -- the light simply never
            // exists. This shipped broken exactly once, and the symptom was
            // "the sparks cast no light, only the impact does": the flash below
            // set pNext and these did not.
            .pNext = &sph,
            // THE SPARK'S OWN ID, never the candidate slot. A slot is reassigned
            // to a different spark as sparks die, so a slot-keyed light would
            // teleport across the room between frames -- worse for ReSTIR than
            // dying, because RTGL1 would carry the old reservoir to a new place.
            // Keyed on the spark, a glow is born and dies with it.
            .uniqueID     = SparkGlowId_Base + uint64_t( sp.sid ),
            .isExportable = false,
        };
        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        s_dbgLights++;
    }
}

// THE ARCS' TRACED HALF, and the answer to "can those arcs emit light".
//
// The branch quads cannot: they are TRANSLUCENT with emissive > 0, which
// RasterizedDataCollector::ToPipelineState turns ADDITIVE, and an additive
// rasterized overlay lives outside the acceleration structure. It adds to the
// screen and contributes nothing to any surface, reflection or bounce. So the
// arcs glow on screen and light nothing -- unless a real analytic light is put
// where they are, which is what this does.
//
// The candidates were collected during the DRAW, so they are exactly the
// branches that were visible: the crackle and the re-path are already applied
// and cannot drift out of step with the geometry.
void UploadArcBranchLights()
{
    if( !cvar::rt_arc || !cvar::rt_arc_glow || s_arcLights.empty() )
    {
        return;
    }

    // NOT GATED ON rt_arc_glow_intensity ANY MORE. Each candidate carries its
    // own intensity now, so testing the arcs' value here would have switched off
    // the EMBERS' lights whenever someone turned the arcs' down to zero -- the
    // same silent cross-coupling this file has had to undo three times.
    const int gmax = std::max( 0, int{ cvar::rt_arc_glow_max } );
    if( gmax == 0 )
    {
        return;
    }

    const auto&    vp = r_viewpoint;
    const FVector3 eye{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                        float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                        float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };

    // NEAREST-FIRST, then truncate -- the RT_UploadFlameLights pattern. A plasma
    // rifle held down is 32 marks x 9 branches of candidates and the cap is what
    // makes per-branch lights affordable at all (rt-lighting-practices.md 20).
    // Sorting a COPY matters: the id is the light's identity and must not be
    // reassigned by the sort.
    if( s_arcLights.size() > size_t( gmax ) )
    {
        std::partial_sort( s_arcLights.begin(),
                           s_arcLights.begin() + gmax,
                           s_arcLights.end(),
                           [ & ]( const ArcLightCand& a, const ArcLightCand& b ) {
                               return ( a.pos - eye ).LengthSquared() <
                                      ( b.pos - eye ).LengthSquared();
                           } );
        s_arcLights.resize( size_t( gmax ) );
    }

    for( const ArcLightCand& c : s_arcLights )
    {
        if( c.k <= 0.001f || c.intensity <= 0.f )
        {
            continue;
        }

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( c.r, c.g, c.b, 1.0f ),
            // THE CANDIDATE'S OWN, not one shared cvar. Arcs and embers ride
            // this list together and are not the same kind of light.
            .intensity = c.intensity * c.k,
            .position  = { c.pos.X, c.pos.Y, c.pos.Z },
            .radius    = std::max( 0.005f, c.radius ),
        };
        auto info = RgLightInfo{
            .sType = RG_STRUCTURE_TYPE_LIGHT_INFO,
            // THE EXTENSION HANGS OFF pNext, AND OMITTING IT IS SILENT-ISH:
            // RTGL1 warns once and DROPS the light, every other field being
            // valid. That shipped broken once already on the spark glows, and
            // the symptom was "the sparks cast no light".
            .pNext        = &sph,
            .uniqueID     = c.id,
            .isExportable = false,
        };
        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        s_dbgLights++;
    }
}

} // namespace rtsp

// The entry point below is at GLOBAL scope, beside every other RT_* call in
// rt_internal.h. The directive goes HERE rather than at the top of the file:
// with it in scope while defining things INSIDE the namespace, every unqualified
// name is findable twice and MSVC reports an ambiguous call to
// rtsp::rtsp::whatever, which is a confusing way to say it.
using namespace rtsp;

void RT_DrawSparks()
{
    s_dbgQuads = 0;
    // Cleared BEFORE the early return below, not after the walk. On a frame that
    // draws nothing the branch lights must go too -- leaving last frame's
    // candidates would keep lighting a wall whose arcs have expired.
    s_arcLights.clear();

    // BOTH populations, and the arc term is not redundant. Arcs live in their
    // own pool, so with rt_spark off g_sparkCount is permanently 0 and the old
    // `g_sparkCount == 0` return would have skipped the arc geometry for ever --
    // the exact shape of bug the shared master gate was introduced to avoid, one
    // level down.
    if( !SparkSystemOn() || !primaryLevel || ( g_sparkCount == 0 && s_arcCount == 0 ) )
    {
        return;
    }

    // A camera-facing basis, built once for both batches.
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

    // THE TWO STYLES. Same particles, same palette, same sizes -- only how they
    // are quantized and shaped. Pixelated snaps to a world grid and steps
    // through seven hard palette entries; realistic interpolates and lets a
    // spark stretch along its own velocity. See rt_spark_style.
    const bool  pixel  = ( int{ cvar::rt_spark_style } == 0 );
    const float grid   = pixel ? std::max( 0.f, float{ cvar::rt_spark_grid } ) : 0.f;
    const float streak = std::max( 1.f, float{ cvar::rt_spark_streak } );
    const float bright = std::max( 0.f, float{ cvar::rt_spark_bright } );
    const float aoStrength =
        std::clamp( float{ cvar::rt_spark_debris_ao_strength }, 0.f, 1.f );
    const float aoFar = std::max( 0.f, float{ cvar::rt_spark_debris_ao_dist } );
    const int   aoMax = std::max( 0, int{ cvar::rt_spark_debris_ao_max } );
    int         aoEmitted = 0;
    s_dbgAo               = 0;

    const FVector3 eyeM{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                         float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                         float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };

    // Scanned and registered here rather than at startup:
    // rgProvideOriginalTexture wants a live renderer, and this is the only place
    // that is guaranteed. Runs once and then costs a bool test.
    ScanShardArt();

    s_batchSpark.verts.clear();
    s_batchSpark.idx.clear();
    for( QuadBatch& sb : s_batchShard )
    {
        sb.verts.clear();
        sb.idx.clear();
    }
    s_batchDebrisAo.verts.clear();
    s_batchDebrisAo.idx.clear();
    for( int i = 0; i < s_debrisBucketCount; i++ )
    {
        s_debrisBuckets[ i ].batch.verts.clear();
        s_debrisBuckets[ i ].batch.idx.clear();
    }
    s_debrisBucketCount = 0;

    const RgNormalPacked32 nrm = rt.rgUtilPackNormal( -fwd.X, -fwd.Y, -fwd.Z );

    for( uint32_t i = 0; i < g_sparkCount; i++ )
    {
        const Spark& sp = s_sparks[ i ];

        const bool           isDbr   = IsChunk( sp.kind );
        const bool           isShard = ( sp.kind == SparkKind::Shard );
        const DebrisProfile& pr      = ProfileFor( sp.surf );

        const uint32_t* ramp  = isDbr ? pr.ramp : RT_SPARK_RAMP;
        const int       rampN = isDbr ? pr.rampN : RT_SPARK_RAMP_N;

        const float t = std::clamp( sp.age / std::max( 1e-4f, sp.life ), 0.f, 1.f );

        float r, g, b;
        if( pixel )
        {
            // NOT INTERPOLATED. A blend between two entries would put colours on
            // screen that are not in the sprite's palette, which is exactly the
            // smooth look this mode exists to avoid.
            const int      ci  = std::min( rampN - 1, int( t * float( rampN ) ) );
            const uint32_t rgb = ramp[ ci ];
            r                  = ( ( rgb >> 16 ) & 0xFF ) / 255.f;
            g                  = ( ( rgb >> 8 ) & 0xFF ) / 255.f;
            b                  = ( rgb & 0xFF ) / 255.f;
        }
        else
        {
            // REALISTIC: the same seven colours, blended. The ramp is unchanged
            // on purpose -- this is the Doom 64 palette drawn smoothly, not a
            // different palette. "Like the base game" means the colours and the
            // mood; only the quantization is being relaxed.
            const float f  = t * float( rampN - 1 );
            const int   i0 = std::clamp( int( f ), 0, rampN - 1 );
            const int   i1 = std::min( i0 + 1, rampN - 1 );
            const float fr = f - float( i0 );

            const uint32_t c0 = ramp[ i0 ];
            const uint32_t c1 = ramp[ i1 ];

            auto l_lerp = [ & ]( int shift ) {
                const float a0 = ( ( c0 >> shift ) & 0xFF ) / 255.f;
                const float a1 = ( ( c1 >> shift ) & 0xFF ) / 255.f;
                return a0 + ( a1 - a0 ) * fr;
            };
            r = l_lerp( 16 );
            g = l_lerp( 8 );
            b = l_lerp( 0 );
        }

        // A CHIP TAKES THE COLOUR OF THE WALL IT CAME OFF.
        //
        // The built-in ramp is kept as the AGE CURVE rather than thrown away:
        // the texture average supplies the hue, and the ramp supplies how much
        // that hue darkens over the chip's life. Replacing the ramp outright
        // would give every chip one flat colour for 20 seconds; using only the
        // ramp gives grey rubble off a rust-red wall. This is the same "keep the
        // art, add shading on top" rule the material work follows.
        //
        // NORMALISED TO A TARGET ALBEDO, not used raw. A Doom wall texture
        // averages wherever its art happens to sit -- some are near-white -- and
        // an albedo of 0.8 is the mistake that made the first debris ramps read
        // as "too bright colors". rt_spark_debris_albedo pins the LUMINANCE and
        // lets the texture supply only the hue, so a pale wall and a dark one
        // produce chips of the same believable darkness in different colours.
        if( isShard && sp.baseRgb != 0u )
        {
            // A SHARD KEEPS THE SPRITE'S OWN VALUE, and that is the one place it
            // parts company with debris.
            //
            // The debris path deliberately throws a texture's brightness away
            // and pins every chip to rt_spark_debris_albedo, because a wall
            // texture's MEAN is an accident of its art -- some average near
            // white -- and only its hue is worth keeping. A shard's colour is
            // not a mean of anything: it is one entry chosen out of BAR1A0's
            // fourteen, and the spread between those entries IS the effect.
            // Pinning the luminance would hand every chunk of a burst the same
            // brightness and delete exactly the variety the palette was sampled
            // for. Nor is the chroma expanded: the barrel is grey by design, and
            // saturating grey manufactures a colour the artist never used.
            //
            // So the sprite value passes straight through, scaled by one knob.
            // That knob defaults ABOVE 1 on purpose: sprite pixels are already
            // LIT values, and feeding a lit value in as an albedo -- which the
            // path tracer then lights again -- lands a shade darker than the
            // barrel it came off.
            const float k = std::max( 0.f, float{ cvar::rt_barrel_albedo } );

            float tr = ( ( sp.baseRgb >> 16 ) & 0xFF ) / 255.f;
            float tg = ( ( sp.baseRgb >> 8 ) & 0xFF ) / 255.f;
            float tb = ( sp.baseRgb & 0xFF ) / 255.f;

            // The ramp's own darkening, as a fraction of its first entry --
            // identical to the debris path, and for the same reason: a shard
            // should dull with age without being recoloured into something the
            // sprite never contained.
            const uint32_t r0    = ramp[ 0 ];
            const float    l0    = ( ( r0 >> 16 ) & 0xFF ) / 255.f;
            const float    curve = l0 > 0.01f ? std::clamp( r / l0, 0.f, 1.f ) : 1.f;

            r = std::min( 1.f, tr * k * curve );
            g = std::min( 1.f, tg * k * curve );
            b = std::min( 1.f, tb * k * curve );
        }
        else if( isDbr && sp.baseRgb != 0u )
        {
            // The class scales both, and fluid pushes tint past 1 so it CLAMPS at
            // full texture colour -- see its row.
            const float tint =
                std::clamp( float{ cvar::rt_spark_debris_tint } * pr.tint, 0.f, 1.f );
            if( tint > 0.f )
            {
                float tr = ( ( sp.baseRgb >> 16 ) & 0xFF ) / 255.f;
                float tg = ( ( sp.baseRgb >> 8 ) & 0xFF ) / 255.f;
                float tb = ( sp.baseRgb & 0xFF ) / 255.f;

                // A WHOLE-TEXTURE MEAN IS A CHROMA KILLER, and this is the step
                // that makes the tint visible at all.
                //
                // Measured from play: a wall that reads plainly beige/orange
                // averages to #574D4B, and another to #504848. Those ARE warm --
                // R > G > B in both -- but by about 14%, because beige highlights
                // average against shadow and black gaps and collapse toward
                // neutral. Normalising luminance then scales all three channels
                // equally and preserves that 14%, so the chip came out grey and
                // the whole feature read as not working. The sampling was right
                // and the DATA was too dull to use.
                //
                // gzdoom hits the same wall: FGameTexture::GetGlowColor passes
                // maxout=153 to averageColor precisely because a plain mean is
                // too dull to be a glow colour. It solves it by normalising the
                // PEAK channel, which also blows the brightness out -- wrong for
                // an albedo. So the chroma is expanded about the pixel's own
                // luminance instead, which leaves brightness for the step below
                // to pin and keeps the two corrections independent.
                {
                    const float sat = std::max( 0.f, float{ cvar::rt_spark_debris_sat } );
                    const float grey = 0.2126f * tr + 0.7152f * tg + 0.0722f * tb;
                    tr = std::clamp( grey + ( tr - grey ) * sat, 0.f, 1.f );
                    tg = std::clamp( grey + ( tg - grey ) * sat, 0.f, 1.f );
                    tb = std::clamp( grey + ( tb - grey ) * sat, 0.f, 1.f );
                }

                const float lum = 0.2126f * tr + 0.7152f * tg + 0.0722f * tb;
                if( lum > 0.01f )
                {
                    const float target =
                        std::clamp( float{ cvar::rt_spark_debris_albedo } * pr.albedo, 0.02f, 1.f );
                    const float k = target / lum;
                    tr = std::min( 1.f, tr * k );
                    tg = std::min( 1.f, tg * k );
                    tb = std::min( 1.f, tb * k );
                }

                // The ramp's own darkening, as a fraction of its first entry, so
                // a chip still fades with age whatever colour it took.
                const uint32_t r0    = ramp[ 0 ];
                const float    l0    = ( ( r0 >> 16 ) & 0xFF ) / 255.f;
                const float    lnow  = r;   // the ramp value resolved just above
                const float    curve = l0 > 0.01f ? std::clamp( lnow / l0, 0.f, 1.f ) : 1.f;

                tr *= curve;
                tg *= curve;
                tb *= curve;

                r += ( tr - r ) * tint;
                g += ( tg - g ) * tint;
                b += ( tb - b ) * tint;
            }
        }

        // DEBRIS IS RAY-TRACED GEOMETRY, and that is what makes it take the
        // room's light. See UploadBatch: the debris batch carries no TRANSLUCENT
        // flag and a primitive alpha of 1, which is RTGL1's rule
        // (VulkanDevice::IsRasterized) for going into the acceleration structure
        // instead of the rasterized overlay. Its colour below is therefore an
        // ALBEDO the path tracer shades, not a final pixel.
        //
        // The first attempt kept debris rasterized and multiplied the sector's
        // light level into the vertex colour by hand. That was wrong twice over:
        // it read the PAINTED lightlevel rather than the traced result, and it
        // could not escape the raster path's
        //
        //     outColor.rgb *= max( vec3( 1 ), tonemapping.avgLuminance )
        //
        // whose avgLuminance is EYE ADAPTATION -- it decays over seconds, so
        // chips visibly took time to darken on walking into an unlit room even
        // though the sector term was instant. Reported as "debris take time to
        // get dark as the room". No engine-side multiply can fix a term the
        // engine cannot see; the fix was to stop being rasterized.

        // Snapped to a whole number of grid cells, and never to ZERO: the
        // smallest a spark may be is exactly one cell, i.e. one pixel. Rounding
        // a sub-cell spark down would delete it silently. In realistic style
        // grid is 0 and both snaps collapse to the identity.
        // DEBRIS FADES BY SHRINKING, NEVER BY ALPHA. Its batch is in the
        // acceleration structure only while its primitive alpha stays at or
        // above RTGL1's MESH_TRANSLUCENT_ALPHA_THRESHOLD (0.98); fading it out
        // would silently demote the whole batch to the rasterized overlay --
        // i.e. back to being fullbright -- for the last part of every chip's
        // life. So the last quarter of the life shrinks the quad to nothing
        // instead, and the alpha below stays at 1.
        float sizeScale = 1.f;
        if( isDbr )
        {
            constexpr float kShrinkFrom = 0.75f;
            if( t > kShrinkFrom )
            {
                sizeScale = 1.f - ( t - kShrinkFrom ) / ( 1.f - kShrinkFrom );
            }
        }

        const float rawSize = sp.size * sizeScale;
        const float edge    = grid > 0.f ? std::max( grid, snap( rawSize, grid ) ) : rawSize;
        const float half    = edge * 0.5f;

        const FVector3 c{ snap( sp.pos.X, grid ), snap( sp.pos.Y, grid ), snap( sp.pos.Z, grid ) };

        FVector3 ex = right * half;
        FVector3 ey = up * half;

        if( isDbr && !isShard && !pixel )
        {
            // RUBBLE, NOT PIXELS. A chip gets its own orientation, its own
            // tumble and its own aspect ratio, so a burst reads as assorted
            // fragments rather than as a grid of identical squares. It costs
            // three floats per particle and one sin/cos per frame.
            //
            // The tumble is deliberately SLOW relative to the spin of a real
            // fragment: at 20 s of life a fast spin turns into a flicker as the
            // quad passes edge-on-ish every rotation, and a flickering chip
            // reads as a rendering fault rather than as motion. A settled chip
            // stops turning: the sim bakes its final angle into `phase` and
            // zeroes `spin`, so this expression stays uniform and the chip does
            // not SNAP back to its birth orientation the moment it lands.
            const float ang = sp.phase + sp.spin * sp.age;
            const float ca  = std::cos( ang );
            const float sa  = std::sin( ang );

            const FVector3 rx = right * ca + up * sa;
            const FVector3 ry = up * ca - right * sa;

            ex = rx * ( half * sp.aspect );
            ey = ry * ( half / std::max( 0.2f, sp.aspect ) );
        }
        else if( !pixel && streak > 1.f )
        {
            // A REAL SPARK IS A STREAK, because it moves far within one exposure.
            // Project its velocity onto the screen basis and stretch the quad
            // along that direction, keeping the across-axis at the original
            // width so the fragment gets LONGER rather than BIGGER.
            //
            // CAPPED, not proportional to speed: an uncapped streak turns a fast
            // spark into a long thin line that reads as tracer fire rather than
            // as a fragment. This is the "not overdone" constraint, and it is a
            // cap rather than a judgement call for that reason.
            const float vs = sp.vel.Length();
            if( vs > 0.05f )
            {
                const FVector3 vdir = sp.vel / vs;
                FVector3       sdir = right * ( vdir | right ) + up * ( vdir | up );
                if( sdir.LengthSquared() > 1e-6f )
                {
                    sdir.MakeUnit();
                    const FVector3 sperp = up * ( sdir | right ) - right * ( sdir | up );

                    // Ramp into the cap with speed, so a slow-moving ember stays
                    // a dot and only a fast one draws a line.
                    const float k = std::clamp( vs / 6.f, 0.f, 1.f );
                    const float L = half * ( 1.f + ( streak - 1.f ) * k );

                    ex = sdir * L;
                    ey = sperp * half;
                }
            }
        }

        // ALPHA CARRIES BOTH THE FADE AND THE BRIGHTNESS, and for the additive
        // spark batch it is the only thing that can. RsWorld.inl's emission line
        // is
        //
        //     outColor.rgb += ldrEmis * ldrColor.a * emissionMaxScreenColor
        //
        // of which ldrEmis is the palette colour (already f8f8b0 at the hot end,
        // so scaling it clips to white and loses the match to the PUFF sprite)
        // and emissionMaxScreenColor is rt_emis_maxscrcolor, GLOBAL to every
        // world emissive in the game. That leaves alpha. See rt_spark_bright.
        //
        // The clamp is the mechanism, not a safety net: alpha is packed to 8
        // bits, so a multiplier above 1 does not raise the peak, it WIDENS it --
        // the spark holds full brightness for a while instead of dimming from
        // the frame it was born.
        //
        // DEBRIS DOES NOT TAKE rt_spark_bright, AND ITS ALPHA IS PINNED AT 1.
        // It is opaque ray-traced geometry: alpha there is not a look knob at
        // all, it is the flag that decides whether the batch is traced or
        // rasterized (see the shrink above). rt_spark_bright is meaningless for
        // it either way -- that knob widens an ADDITIVE peak, and debris does
        // not add.
        // WHICH BATCH, and there are now three answers.
        //
        // A TEXTURED SHARD goes to its own art's batch and nothing else decides
        // it: a primitive carries exactly one texture, so the piece of art IS
        // the batch key, and the colour resolved above is not consulted at all.
        // HitInfo.inl mixes the primitive colour in only where there is no
        // albedo texture -- with one present the art wins outright, which is the
        // whole point of authoring it.
        //
        // DEBRIS goes to a colour bucket, and the bucket can only be chosen once
        // the colour is resolved, since the colour IS its key.
        // baseRgb 0 on a shard means "wear the authored art"; non-zero means the
        // spawn chose to make this one small generated grit and handed it a
        // colour. See the note in SpawnBarrelShards.
        const int artN  = ShardArtCount();
        const int artIx = ( isShard && artN > 0 && sp.baseRgb == 0u )
                              ? int( hash01( sp.sid * 0x27D4EB2Fu ) * float( artN ) ) % artN
                              : -1;

        QuadBatch& batch = ( artIx >= 0 ) ? s_batchShard[ artIx ]
                                          : ( isDbr ? DebrisBucketFor( sp.surf, r, g, b )
                                                    : s_batchSpark );

        const float fade = 1.f - t * t;
        const float a    = isDbr ? 1.f : std::clamp( bright * fade, 0.f, 1.f );

        const RgColor4DPacked32 col = rt.rgUtilPackColorFloat4D( r, g, b, a );

        // Sparks face the camera and are unlit, so their normal is cosmetic.
        // Debris is shaded, so it takes the surface it was knocked off -- a
        // camera-facing normal would swing the lighting as the player turns.
        const RgNormalPacked32 vnrm =
            isDbr ? rt.rgUtilPackNormal( sp.nrm.X, sp.nrm.Y, sp.nrm.Z ) : nrm;

        const uint32_t base = uint32_t( batch.verts.size() );

        if( isShard && !pixel )
        {
            // A PIECE OF THE BARREL, and the reason it is a hundred lines rather
            // than a bigger debris chip is that the two differ in KIND.
            //
            // Everything above builds a CAMERA-FACING quad from the screen's
            // right/up basis. At 2 cm that is invisible -- a chip has no
            // silhouette to give it away. At 25 cm it is the whole problem:
            // asked for "bigger pieces that really look like the barrel metal
            // sprite parts, not just square particle pixels", and a large flat
            // billboard that keeps turning to face you as you walk round it is
            // the definition of a square particle pixel however it is coloured.
            //
            // So a shard is built in WORLD SPACE, out of three things a torn
            // barrel plate actually has:
            //
            //   ART.    Its OUTLINE and its colour come from a hand-cut piece of
            //           the barrel sprite, sampled through the alpha channel --
            //           see the note in rt_barrel.cpp. The first version
            //           generated the outline procedurally, from hashed tears
            //           around a curved strip, and it was reported as "still
            //           particles / parts you generate yourself, its ugly". It
            //           was: no tear generator produces charred metal with hot
            //           orange along the break, because what makes those read is
            //           that someone drew them.
            //   CURVE.  It is a section of a cylinder, not a plane. The bend is
            //           real geometry with real normals across it, so one edge
            //           catches a light while the other does not -- which is
            //           what the eye reads as sheet metal rather than as card,
            //           and it is the one thing the flat art cannot supply.
            //   TUMBLE. About a WORLD axis, not within the screen plane, so the
            //           plate genuinely presents its edge sometimes. That is
            //           only affordable BECAUSE it is double-sided below.
            //
            // DOUBLE-SIDED, and not as a safety net. A world-oriented plate is
            // seen from behind roughly half the time; single-sided it would wink
            // out or shade black at exactly the moments the tumble is most
            // legible. Twenty triangles a shard against a dozen shards is
            // nothing next to that.
            //
            // WITH NO ART (artIx < 0) this falls back to generating the outline:
            // hashed tears, palette colour, the version that was replaced. It is
            // kept because a missing file must degrade to something visible
            // rather than to an empty room -- and because it is what proves,
            // when the pieces do not show up, that the problem is the art path
            // and not the geometry.
            const int segs = std::clamp( int{ cvar::rt_barrel_segs }, 2, 12 );

            // THE ART'S OWN PROPORTIONS. A piece 32x15 renders twice as long as
            // it is wide, so the outline that was cut is the outline that lands
            // in the room. Without art, the spawn-time aspect stands in.
            const float aspect = artIx >= 0 ? ShardArtAspect( artIx )
                                            : std::max( 0.15f, sp.aspect );

            // THE FRAME, in two cases, and the split is what lets a settled
            // shard lie FLAT. While it tumbles the whole frame turns about a
            // world axis; once the sim has stopped it -- spin zeroed, the
            // floor plane's normal written into nrm -- only the yaw about its
            // own normal is left, and a yaw cannot lift the plate off the floor.
            FVector3 nn = sp.nrm;
            if( nn.LengthSquared() < 1e-6f )
            {
                nn = FVector3{ 0, 0, 1 };
            }
            nn.MakeUnit();

            FVector3 tt = std::abs( nn.Z ) < 0.9f ? ( nn ^ FVector3{ 0, 0, 1 } )
                                                  : ( nn ^ FVector3{ 1, 0, 0 } );
            tt.MakeUnit();

            const float ang = sp.phase + sp.spin * sp.age;

            if( std::abs( sp.spin ) > 1e-6f )
            {
                // Rodrigues about a stable per-shard axis. Hashed from the sid
                // rather than drawn per frame, or the plate would jitter instead
                // of turn.
                FVector3 ax{ hash01( sp.sid * 0x9E3779B9u ) * 2.f - 1.f,
                             hash01( sp.sid * 0x85EBCA6Bu ) * 2.f - 1.f,
                             hash01( sp.sid * 0xC2B2AE35u ) * 2.f - 1.f };
                if( ax.LengthSquared() < 1e-6f )
                {
                    ax = FVector3{ 0, 0, 1 };
                }
                ax.MakeUnit();

                const float ca     = std::cos( ang );
                const float sa     = std::sin( ang );
                auto        l_rot = [ & ]( const FVector3& v ) {
                    return v * ca + ( ax ^ v ) * sa + ax * ( ( ax | v ) * ( 1.f - ca ) );
                };
                nn = l_rot( nn );
                tt = l_rot( tt );
                nn.MakeUnit();
                // Re-orthogonalised: the two are rotated independently, and at a
                // twenty-second life the float error is not academic.
                tt = tt - nn * ( tt | nn );
                if( tt.LengthSquared() < 1e-6f )
                {
                    tt = std::abs( nn.Z ) < 0.9f ? ( nn ^ FVector3{ 0, 0, 1 } )
                                                 : ( nn ^ FVector3{ 1, 0, 0 } );
                }
                tt.MakeUnit();
            }
            else
            {
                const FVector3 bb = nn ^ tt;
                tt                = tt * std::cos( ang ) + bb * std::sin( ang );
                tt.MakeUnit();
            }

            const FVector3 ez = nn ^ tt; // the length axis, along the bend's spine

            // rt_barrel_size is the piece's LONG side whichever way round the
            // art is, so a wide piece and a tall one of the same size setting
            // are the same size on screen -- otherwise the knob would mean
            // something different for each of the four.
            const float W  = aspect >= 1.f ? half : half * aspect;
            const float Lh = aspect >= 1.f ? half / aspect : half;

            // The bend. R is derived so the plate still spans W to either side
            // whatever the arc is, so this knob changes how CURVED a shard is
            // and not how big -- which is what makes it tunable on its own.
            const float arc = std::clamp( float{ cvar::rt_barrel_curve }, 0.05f, 2.8f );
            const float R   = W / std::max( 0.05f, std::sin( arc * 0.5f ) );

            for( int k = 0; k <= segs; k++ )
            {
                const float u   = float( k ) / float( segs ) - 0.5f;
                const float phi = u * arc;

                const FVector3 mid =
                    c + nn * ( R * ( std::cos( phi ) - 1.f ) ) + tt * ( R * std::sin( phi ) );
                FVector3 vn = nn * std::cos( phi ) + tt * std::sin( phi );
                vn.MakeUnit();

                // THE OUTLINE. With art the quad stays a clean rectangle and the
                // ALPHA CHANNEL cuts the shape -- jagging the mesh as well would
                // chew a bite out of art that is already torn, and hide part of
                // the drawing. Without art the mesh has to be the outline, so
                // the edges are pulled in by a per-station hash instead.
                //
                // THE RANGE REACHES DOWN TO 0.3, not 0.55. At 0.55 the two edges
                // never depart far enough from parallel and a piece with an
                // aspect near 1 comes out a rounded square -- which is exactly
                // what "just don't make them simple squares" was about. Going
                // deeper makes a station genuinely pinch, so a fragment can be a
                // wedge or a sliver rather than a lozenge every time.
                const float ja =
                    artIx >= 0
                        ? 1.f
                        : 0.30f + 0.70f * hash01( sp.sid * 2654435761u + uint32_t( k ) * 7919u );
                const float jb =
                    artIx >= 0 ? 1.f
                               : 0.30f + 0.70f * hash01( sp.sid * 40503u +
                                                         uint32_t( k ) * 22695477u + 17u );

                const RgNormalPacked32 pn = rt.rgUtilPackNormal( vn.X, vn.Y, vn.Z );

                // u runs ACROSS the bend, so the art curves the way a stave
                // does; v runs along the spine. Both span the full image.
                const float uu = float( k ) / float( segs );

                const FVector3 pa = mid + ez * ( Lh * ja );
                const FVector3 pb = mid - ez * ( Lh * jb );

                batch.verts.push_back( RgPrimitiveVertex{
                    .position     = { pa.X, pa.Y, pa.Z },
                    .normalPacked = pn,
                    .texCoord     = { uu, 0.f },
                    .color        = col,
                } );
                batch.verts.push_back( RgPrimitiveVertex{
                    .position     = { pb.X, pb.Y, pb.Z },
                    .normalPacked = pn,
                    .texCoord     = { uu, 1.f },
                    .color        = col,
                } );

                // The back face: the same two points with the normal flipped.
                // Recomputed rather than read back out of the packed one --
                // rgUtilPackNormal is lossy and there is no unpack helper here.
                const RgNormalPacked32 pn2 = rt.rgUtilPackNormal( -vn.X, -vn.Y, -vn.Z );

                batch.verts.push_back( RgPrimitiveVertex{
                    .position     = { pa.X, pa.Y, pa.Z },
                    .normalPacked = pn2,
                    .texCoord     = { uu, 0.f },
                    .color        = col,
                } );
                batch.verts.push_back( RgPrimitiveVertex{
                    .position     = { pb.X, pb.Y, pb.Z },
                    .normalPacked = pn2,
                    .texCoord     = { uu, 1.f },
                    .color        = col,
                } );
            }

            // Four vertices per station: front pair, then back pair. The back
            // triangles are wound the other way round, so whichever side faces
            // the camera is a front face with its normal pointing at it.
            for( int k = 0; k < segs; k++ )
            {
                const uint32_t a0 = base + uint32_t( k * 4 );
                const uint32_t a1 = a0 + 1;
                const uint32_t a2 = a0 + 4;
                const uint32_t a3 = a0 + 5;

                batch.idx.push_back( a0 );
                batch.idx.push_back( a1 );
                batch.idx.push_back( a3 );
                batch.idx.push_back( a0 );
                batch.idx.push_back( a3 );
                batch.idx.push_back( a2 );

                const uint32_t b0 = a0 + 2;
                const uint32_t b1 = b0 + 1;
                const uint32_t b2 = b0 + 4;
                const uint32_t b3 = b0 + 5;

                batch.idx.push_back( b0 );
                batch.idx.push_back( b3 );
                batch.idx.push_back( b1 );
                batch.idx.push_back( b0 );
                batch.idx.push_back( b2 );
                batch.idx.push_back( b3 );
            }
        }
        else if( isDbr && !pixel )
        {
            // AN IRREGULAR POLYGON, NOT A QUAD. A rotated rectangle is still a
            // rectangle: reported as debris looking like "just a pixel /
            // square" even with the tumble and the aspect ratio already in.
            // Rubble is angular and no two pieces are the same outline, so each
            // chip gets 5-7 corners at its own radii -- stable for its life,
            // because the radii are hashed from its `sid` rather than drawn
            // fresh each frame.
            //
            // Fan-triangulated from corner 0, which is safe here because the
            // polygon is convex by construction (radii vary, angles are
            // monotonic). Pixelated style keeps the quad below: there a chip is
            // deliberately one square texel.
            const int n = 5 + int( hash01( sp.sid * 2654435761u ) * 3.f );

            for( int k = 0; k < n; k++ )
            {
                const float a =
                    ( 2.f * rt_pi() * float( k ) ) / float( n ) +
                    hash01( sp.sid * 40503u + uint32_t( k ) ) * ( rt_pi() / float( n ) );
                const float rr = 0.55f + 0.45f * hash01( sp.sid * 22695477u + uint32_t( k ) * 7919u );

                const FVector3 pv = c + ex * ( std::cos( a ) * rr ) + ey * ( std::sin( a ) * rr );

                batch.verts.push_back( RgPrimitiveVertex{
                    .position     = { pv.X, pv.Y, pv.Z },
                    .normalPacked = vnrm,
                    .texCoord     = { 0.5f + 0.5f * std::cos( a ), 0.5f + 0.5f * std::sin( a ) },
                    .color        = col,
                } );
            }

            for( int k = 1; k + 1 < n; k++ )
            {
                batch.idx.push_back( base );
                batch.idx.push_back( base + uint32_t( k ) );
                batch.idx.push_back( base + uint32_t( k + 1 ) );
            }
        }
        else
        {
            const FVector3 corner[ 4 ] = {
                c - ex - ey,
                c + ex - ey,
                c + ex + ey,
                c - ex + ey,
            };
            const float uv[ 4 ][ 2 ] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

            for( int k = 0; k < 4; k++ )
            {
                batch.verts.push_back( RgPrimitiveVertex{
                    .position     = { corner[ k ].X, corner[ k ].Y, corner[ k ].Z },
                    .normalPacked = vnrm,
                    .texCoord     = { uv[ k ][ 0 ], uv[ k ][ 1 ] },
                    .color        = col,
                } );
            }

            batch.idx.push_back( base + 0 );
            batch.idx.push_back( base + 1 );
            batch.idx.push_back( base + 2 );
            batch.idx.push_back( base + 0 );
            batch.idx.push_back( base + 2 );
            batch.idx.push_back( base + 3 );
        }

        s_dbgQuads++;

        // The blob, for debris that has come to REST on a floor. Only settled
        // pieces get one: the whole claim of contact occlusion is that the thing
        // is touching this floor, and a chip still bouncing is not. That is the
        // same honesty rt_sprite_ao_fade buys for a flying enemy.
        // AO IS DISTANCE-CULLED, and not for cost. RsDecal.frag keeps a fragment
        // only where the traced surface under it is within 5 cm of the quad, and
        // at range a pixel footprint on a grazing floor exceeds that -- the same
        // limit docs/sprite-shadows-and-ao.md bounds with rt_sprite_ao_dist 30
        // and the game's own bullet-hole decals share. Reported here as AO
        // "lines" reaching away from the chips.
        const bool aoNear = ( sp.pos - eyeM ).LengthSquared() <= aoFar * aoFar;

        if( isDbr && sp.settled && aoNear && aoStrength > 0.f && cvar::rt_spark_debris_ao &&
            aoEmitted < aoMax )
        {
            const float rad = edge * std::max( 0.f, float{ cvar::rt_spark_debris_ao_radius } );
            if( rad > 1e-4f )
            {
                s_batchDebrisAo.verts.clear();
                s_batchDebrisAo.idx.clear();
                const uint32_t abase = 0;

                const RgNormalPacked32  up = rt.rgUtilPackNormal( 0.f, 0.f, 1.f );
                const RgColor4DPacked32 cc =
                    rt.rgUtilPackColorFloat4D( 0.f, 0.f, 0.f, aoStrength );
                const RgColor4DPacked32 cr = rt.rgUtilPackColorFloat4D( 0.f, 0.f, 0.f, 0.f );

                // 1 mm of bias keeps the quad off the floor's exact plane
                // without spending the decal shader's 5 cm budget.
                const float az = sp.pos.Z + 0.001f;

                s_batchDebrisAo.verts.push_back( RgPrimitiveVertex{
                    .position     = { sp.pos.X, sp.pos.Y, az },
                    .normalPacked = up,
                    .texCoord     = { 0.5f, 0.5f },
                    .color        = cc,
                } );

                constexpr int kAoSegs = 10;
                for( int k = 0; k < kAoSegs; k++ )
                {
                    const float a = ( 2.f * rt_pi() * float( k ) ) / float( kAoSegs );
                    s_batchDebrisAo.verts.push_back( RgPrimitiveVertex{
                        .position     = { sp.pos.X + std::cos( a ) * rad,
                                          sp.pos.Y + std::sin( a ) * rad,
                                          az },
                        .normalPacked = up,
                        .texCoord     = { 0.5f + 0.5f * std::cos( a ), 0.5f + 0.5f * std::sin( a ) },
                        .color        = cr,
                    } );
                }
                for( int k = 0; k < kAoSegs; k++ )
                {
                    s_batchDebrisAo.idx.push_back( abase );
                    s_batchDebrisAo.idx.push_back( abase + 1 + uint32_t( k ) );
                    s_batchDebrisAo.idx.push_back( abase + 1 + uint32_t( ( k + 1 ) % kAoSegs ) );
                }

                // A distinct ID per blob. RTGL1 keeps one upload per ID and it is
                // the LATER one that loses, so a shared ID would silently show
                // only the first chip's blob.
                UploadAoBlob( s_batchDebrisAo, RT_DEBRIS_AO_MESH_ID + uint64_t( aoEmitted ) );
                aoEmitted++;
                s_dbgAo++;
            }
        }
    }

    // -----------------------------------------------------------------------
    // ARC MARKS. Regenerated from each mark's seed every frame -- see the
    // ArcMark banner for why the geometry is not stored.
    //
    // They go into the SPARK batch rather than one of their own: same additive
    // blend, same untextured white material, same mesh ID. A second batch would
    // be a second uploadMeshPrimitive for no difference in state.
    // -----------------------------------------------------------------------
    if( cvar::rt_arc && s_arcCount > 0 )
    {
        const int   nbr   = std::clamp( int{ cvar::rt_arc_branches }, 1, RT_ARC_MAX_BRANCH );
        const int   nseg  = std::clamp( int{ cvar::rt_arc_segments }, 1, RT_ARC_MAX_SEG );
        // The PLASMA-RIFLE values. Each mark scales these by its own flavour
        // inside the loop, so `reach` and `wid` there are per-weapon.
        const float reachBase = std::max( 0.02f, float{ cvar::rt_arc_reach } );
        const float widBase   = std::max( 0.001f, float{ cvar::rt_arc_width } );
        const float jit   = std::max( 0.f, float{ cvar::rt_arc_jitter } );
        const float flk   = std::clamp( float{ cvar::rt_arc_flicker }, 0.f, 1.f );
        const float flkR  = std::max( 0.f, float{ cvar::rt_arc_flicker_rate } );
        const float coreR = std::max( 0.f, float{ cvar::rt_arc_core } );
        const float forkP = std::clamp( float{ cvar::rt_arc_fork }, 0.f, 1.f );
        const float abr   = std::max( 0.f, float{ cvar::rt_arc_bright } );
        const float churn = std::max( 0.f, float{ cvar::rt_arc_churn } );
        const float crReach = std::max( 0.f, float{ cvar::rt_arc_creep_reach } );
        const float arcLife = std::max( 0.05f, float{ cvar::rt_arc_life } );

        const bool  wantBurn = cvar::rt_arc_burn;
        const float burnRad  = std::max( 0.f, float{ cvar::rt_arc_burn_radius } );
        const float burnStr  = std::clamp( float{ cvar::rt_arc_burn_strength }, 0.f, 1.f );
        const float burnDist = std::max( 0.f, float{ cvar::rt_arc_burn_dist } );
        const int   burnMax  = std::max( 0, int{ cvar::rt_arc_burn_max } );
        const float burnVar  = std::clamp( float{ cvar::rt_arc_burn_color_var }, 0.f, 1.f );
        const float burnMottle = std::clamp( float{ cvar::rt_arc_burn_mottle }, 0.f, 1.f );
        int         burnEmitted = 0;
        const float wander = std::max( 0.f, float{ cvar::rt_arc_wander } );
        const bool  wantGlow = cvar::rt_arc_glow;

        const bool  wantEmber   = cvar::rt_ember;
        // NOTE: the ember COUNT is resolved per mark inside the loop, not here.
        // A barrel's scorch carries more coals than a rocket's and the two can
        // be alive at once, so it rides the mark. EmberCountFor is the one place
        // that answers it -- see the note on it.
        const float emberLife   = std::max( 0.05f, float{ cvar::rt_ember_life } );
        const float emberSize   = std::max( 0.002f, float{ cvar::rt_ember_size } );
        // Scaled PER MARK below by m.emberBright: fifty coals at the rocket's
        // brightness is fifty blown-out white squares, because the additive peak
        // is per coal and raising the count alone turns a bed into a light box.
        const float emberBrightBase = std::max( 0.f, float{ cvar::rt_ember_bright } );
        const float emberHalo  = std::max( 0.f, float{ cvar::rt_ember_halo } );
        const float emberHaloA = std::clamp( float{ cvar::rt_ember_halo_alpha }, 0.f, 1.f );
        const float emberGlowInt = std::max( 0.f, float{ cvar::rt_ember_glow_intensity } );
        const float emberGlowRad = std::max( 0.005f, float{ cvar::rt_ember_glow_radius } );
        const float emberFlicker = std::clamp( float{ cvar::rt_ember_flicker }, 0.f, 1.f );
        const float emberRate    = std::max( 0.f, float{ cvar::rt_ember_flicker_rate } );

        for( uint32_t ai = 0; ai < s_arcCount; ai++ )
        {
            const ArcMark&  m  = s_arcs[ ai ];
            const ArcStyle& st = ArcStyleFor( m.flavor );

            // THE SCORCH, on the mark's own long clock. Emitted BEFORE the
            // filigree and outside its `t >= 1` early-out below, because it
            // outlives the arcs by a factor of fifty -- that is the whole point
            // of it, and putting it after the arc work would have quietly tied
            // it back to the short life.
            const float mBurnRad = burnRad * m.burnScale;
            if( wantBurn && burnEmitted < burnMax && mBurnRad > 1e-4f && burnStr > 0.f )
            {
                // A FOREVER scorch has m.life == FLT_MAX, so this is 0 for any
                // age a session can reach and the hold-then-fade curve below
                // never leaves its hold. No special case needed, which is the
                // point of using a sentinel rather than a very large number.
                const float bt = std::clamp( m.age / std::max( 1e-4f, m.life ), 0.f, 1.f );

                // The distance test is the DECAL PROXIMITY one, not a cost cull:
                // RsDecal.frag keeps a fragment only where the traced surface
                // under it is within 5 cm, and at range a pixel footprint on a
                // grazing wall exceeds that. Skipping it is what produced AO
                // "lines" reaching away from settled debris.
                if( ( m.at - eyeM ).LengthSquared() <= burnDist * burnDist )
                {
                    // HOLDS, THEN FADES. A scorch that starts fading the instant
                    // it is made is a scorch you never see at full strength;
                    // real soot sits and then weathers. Flat for the first 60%
                    // of its life, then out over the last 40%.
                    const float bfade =
                        bt < 0.6f ? 1.f : std::clamp( ( 1.f - bt ) / 0.4f, 0.f, 1.f );

                    s_batchArcBurn.verts.clear();
                    s_batchArcBurn.idx.clear();

                    const RgNormalPacked32 bn =
                        rt.rgUtilPackNormal( m.nrm.X, m.nrm.Y, m.nrm.Z );
                    // NOT BLACK, and this is the fix for the scorch swallowing
                    // the embers' light.
                    //
                    // The decal lerps into the albedo buffer -- SRC_ALPHA /
                    // ONE_MINUS_SRC_ALPHA, DecalManager.cpp:599 -- so what
                    // lands is colour*strength + original*(1-strength). Black
                    // at strength 0.9 therefore leaves 0.1x the original and
                    // nothing else: a surface that by construction reflects
                    // almost nothing, so every light aimed at it does almost
                    // nothing too. That is why the embers' glow vanished inside
                    // their own mark and why raising their intensity could not
                    // bring it back.
                    //
                    // An earlier comment here asserted the colour "must be
                    // black rather than dark-grey", reasoning that the decal
                    // multiplies and a grey centre would lighten a dark wall.
                    // That is wrong twice: it is a lerp and not a multiply, and
                    // real soot reflects 4-10% rather than being a void. A dark
                    // warm charcoal reads as burnt AND has something to bounce.
                    const uint32_t bhex = uint32_t( cvar::rt_arc_burn_color );
                    float          bcr  = ( ( bhex >> 16 ) & 0xFF ) / 255.f;
                    float          bcg  = ( ( bhex >> 8 ) & 0xFF ) / 255.f;
                    float          bcb  = ( bhex & 0xFF ) / 255.f;

                    // NO TWO SCORCHES THE SAME COLOUR. One tint repeated across a
                    // wall reads as a stamp -- the eye picks up the repetition
                    // long before it questions the shape. Real burns differ with
                    // what burned: sootier or ashier, warmer or colder.
                    //
                    // Two independent axes off the mark's seed, which between
                    // them cover the whole believable range from a warm brown
                    // through neutral greys to near black:
                    //   LUMINANCE -- how hard this one charred;
                    //   SATURATION -- soot is brown, ash is grey, and this
                    //   crosses between them, including past 1 for the
                    //   occasional distinctly brown one.
                    if( burnVar > 0.f )
                    {
                        const float lum =
                            1.f + ( hash01( m.seed * 2654435761u ) * 2.f - 1.f ) * burnVar * 0.75f;
                        const float sat =
                            1.f + ( hash01( m.seed * 40503u + 17u ) * 2.f - 1.f ) * burnVar * 1.1f;

                        const float grey = 0.2126f * bcr + 0.7152f * bcg + 0.0722f * bcb;
                        bcr = std::clamp( ( grey + ( bcr - grey ) * sat ) * lum, 0.f, 1.f );
                        bcg = std::clamp( ( grey + ( bcg - grey ) * sat ) * lum, 0.f, 1.f );
                        bcb = std::clamp( ( grey + ( bcb - grey ) * sat ) * lum, 0.f, 1.f );
                    }

                    const RgColor4DPacked32 bc =
                        rt.rgUtilPackColorFloat4D( bcr, bcg, bcb, burnStr * bfade );
                    // The ring vertices build their own colours in l_vert below
                    // -- each is mottled independently, so there is no single
                    // mid or rim colour to precompute any more.

                    // 1 mm off the wall, well inside the shader's 5 cm budget.
                    const FVector3 bcen = m.at + m.nrm * 0.001f;

                    s_batchArcBurn.verts.push_back( RgPrimitiveVertex{
                        .position     = { bcen.X, bcen.Y, bcen.Z },
                        .normalPacked = bn,
                        .texCoord     = { 0.5f, 0.5f },
                        .color        = bc,
                    } );

                    // TWO RINGS AND A LOT MORE SEGMENTS. The first version was a
                    // 12-segment fan with each rim radius hashed INDEPENDENTLY
                    // between 0.72x and 1.22x, and it produced the artefact in
                    // screen/triangleChurn2.png: hard straight-edged wedges
                    // radiating from the centre, the mark reading as a black
                    // star rather than a burn.
                    //
                    // Two separate causes, and fixing only one leaves it:
                    //
                    // 1. INDEPENDENT RADII MAKE A STAR. Neighbouring rim points
                    //    could differ by 70% of the radius, so the silhouette
                    //    was a spiky polygon. The radii are now a sum of two low
                    //    harmonics of the angle, which is smooth AND periodic --
                    //    it closes seamlessly at k = 0, which a hash cannot.
                    //
                    // 2. ONE LINEAR RAMP ACROSS A 30 DEGREE WEDGE. With alpha
                    //    opaque at the centre and 0 at the rim, each triangle
                    //    interpolates alpha linearly over a huge area, so the
                    //    iso-alpha contours are straight lines and you see the
                    //    triangulation itself. A mid ring bends those contours
                    //    and 40 segments makes each wedge 9 degrees.
                    //
                    // A fan is still the right topology -- a ring or a plateau
                    // puts a visible crease at every boundary, per
                    // docs/sprite-shadows-and-ao.md -- it was just far too
                    // coarse for something this large. The sprite AO blobs get
                    // away with 10 segments because they are the size of a
                    // dropped weapon; a scorch is half a metre across.
                    // LAYERED FANS, and the shape was settled in the lab rather
                    // than argued about. tools\impact-lab.ps1 plants one mark
                    // and photographs it; three captures decided this.
                    //
                    // WHAT FAILED, and why more of it did not help. A ring mesh
                    // -- centre, N rings, rim -- shows hard light/dark wedges
                    // radiating outward. The cause is the quad split: of the two
                    // triangles in each ring quad, one gets corner alphas
                    // (A,B,B) and the other (A,B,A), so they have genuinely
                    // different averages and alternate visibly all the way
                    // round. That is true for ANY ring-to-ring step, which is
                    // why going from 2 rings to 5 changed nothing, and why 48
                    // segments only made the wedges narrower. See
                    // screen/pointyTriangles.png and screen/worseCh.png.
                    //
                    // A SINGLE FAN has no such artefact: every triangle is
                    // structurally identical -- one centre vertex at full alpha,
                    // two rim vertices at zero -- so there is no pair of
                    // neighbours to disagree. screen/lab/lab-ember-fan1.png is
                    // clean at point-blank range where the ring meshes were at
                    // their worst.
                    //
                    // But one fan is one flat wash, with only a centre and a rim
                    // to vary between. So the mark is several fans: a big one
                    // and rt_arc_burn_blobs smaller ones, each offset, each with
                    // its own darkness. They overlap and blend per fragment, so
                    // what comes out is genuinely blotchy -- dark where they
                    // pile up, grey where one lies alone -- while every triangle
                    // in every one of them stays identical.
                    //
                    // The remaining honest limit: alpha still falls off LINEARLY
                    // from each centre, because that is all a fan can express. A
                    // radial-gradient TEXTURE would interpolate per pixel and
                    // give any curve wanted, plus real noise. That is one asset
                    // away and is the right answer if this ever needs to be
                    // perfect; the layering is what makes it good enough without
                    // adding art.
                    constexpr int kBurnSegs = 64;

                    const int nBlobs =
                        std::clamp( int{ cvar::rt_arc_burn_blobs }, 1, 8 );

                    for( int bi = 0; bi < nBlobs; bi++ )
                    {
                        // ONE DECAL PRIMITIVE PER BLOB, and this is the fix for
                        // the straight-edged CUTS across the mark.
                        //
                        // All nine fans used to go into one primitive. It is
                        // geometrically sound -- no triangle spans two blobs,
                        // every fan interpolates only its own vertices -- and it
                        // is wrong anyway, for a reason this file already
                        // records twenty lines up in UploadAoBlob: the sprite AO
                        // did exactly this, produced "lines" reaching away from
                        // the blobs, and was fixed by uploading one primitive
                        // per blob. Same batching, same signature, same fix.
                        //
                        // A decal is not ordinary geometry: RsDecal.frag keeps a
                        // fragment only where the traced surface beneath it is
                        // within a few centimetres, so what a decal primitive
                        // covers is decided per fragment against the world. Nine
                        // overlapping fans sharing one primitive share that
                        // test, and the discard boundary lands as a hard,
                        // perfectly straight edge through the middle of the
                        // mark. Reported as "it never looks smoothly spread,
                        // always breaks" -- and the breaks are straight because
                        // a plane intersecting a plane is a line.
                        s_batchArcBurn.verts.clear();
                        s_batchArcBurn.idx.clear();

                        const uint32_t bs = m.seed + uint32_t( bi ) * 2654435761u;

                        // Blob 0 is the mark itself, centred and full size. The
                        // rest are smaller, offset, and darker -- they are the
                        // heavy soot inside the scorch rather than more scorch.
                        // A FAN'S ALPHA FALLS LINEARLY from centre to rim -- that
                        // is all a fan can express -- so one of them is a faint
                        // smudge with no solid middle however high its centre
                        // alpha is, because most of its AREA is out near the
                        // thin end. Photographed at
                        // screen/lab/lab-ember-framed.png and it read as a warm
                        // stain rather than a burn.
                        //
                        // The layers are what build the core. Alpha compounds as
                        // 1-(1-a1)(1-a2)..., so a few overlapping centres reach
                        // genuinely black in the middle while each individual
                        // fan stays a clean artefact-free gradient. So the
                        // offsets are kept SMALL: these are the heavy soot at
                        // the heart of the mark, not satellites around it.
                        // SPREAD ACROSS THE DISC, not clustered at its centre,
                        // and this is the second thing the lab corrected.
                        //
                        // A fan's alpha falls LINEARLY from centre to rim -- all
                        // a fan can express -- so the area where it is actually
                        // visible is only the inner third or so. One big fan is
                        // therefore a small faint smudge inside a large invisible
                        // circle, whatever its centre alpha
                        // (screen/lab/lab-ember-framed.png), and piling the
                        // smaller ones on the middle only darkened that same
                        // small spot (lab-ember-core.png).
                        //
                        // Scattering them over the disc instead makes their UNION
                        // the shape: alpha compounds as 1-(1-a1)(1-a2)..., so
                        // overlapping soft discs give a broad, solid, irregular
                        // mark with a real edge -- which is how a blotch is
                        // normally built, and it keeps every triangle identical.
                        // sqrt() on the offset spreads them evenly by AREA rather
                        // than bunching them in the middle.
                        const bool  primary = ( bi == 0 );
                        const float bscale =
                            primary ? 0.72f : ( 0.34f + 0.30f * hash01( bs * 7919u ) );
                        const float boff =
                            primary ? 0.f
                                    : std::sqrt( hash01( bs * 40503u ) ) * 0.55f * mBurnRad;
                        const float bang = hash01( bs * 22695477u ) * 2.f * rt_pi();

                        const FVector3 c0 = bcen + m.tan * ( std::cos( bang ) * boff ) +
                                            m.bit * ( std::sin( bang ) * boff );

                        // Each blob its own darkness, so the pile-ups read as
                        // soot and the outliers as ash. Biased DARK -- the note
                        // from play was too much brown, not too little.
                        // EACH BLOB IS A KIND, NOT A SHADE. rt_arc_burn_color is
                        // the BASE, and a real blast does not leave one tint of
                        // it -- it leaves char that is genuinely black where the
                        // heat sat, ash that has no hue left at all, and scorch
                        // that only darkened what was already there.
                        //
                        // Scaling every blob by a random factor -- what this used
                        // to do -- gives six shades of the same brown, and six
                        // overlapping shades of one brown average back into one
                        // brown. Picking a KIND per blob is what puts distinct
                        // patches in: alpha compounds where blobs overlap, so a
                        // black blob laid over a brown one really is black there,
                        // not a blend of the two.
                        float vr = bcr, vg = bcg, vb = bcb;
                        float aMul = primary ? 1.f : ( 0.80f + 0.20f * hash01( bs * 69069u ) );
                        if( burnMottle > 0.f )
                        {
                            const float jit  = hash01( bs * 374761393u );
                            const float grey = 0.2126f * vr + 0.7152f * vg + 0.0722f * vb;

                            // KIND BY RADIUS, NOT BY COIN FLIP, and this is the
                            // correction that made it read as a burn.
                            //
                            // The kind used to be a hash: a blob was char, ash
                            // or scorch at random wherever it sat. That scatters
                            // brown patches through the middle of the mark and
                            // black ones out at the rim, which is backwards --
                            // and averaged over nine overlapping blobs it comes
                            // out as one muddy brown everywhere, which is
                            // exactly what "still not black enough" was looking
                            // at.
                            //
                            // A real blast is not random about this. The core
                            // sat in the heat and CARBONISED -- black, then soot
                            // grey around it. Only the outskirts merely got hot
                            // enough to BROWN the surface. So distance from the
                            // mark's centre picks the kind, and the black and
                            // grey now own most of the disc with brown confined
                            // to the edge.
                            //
                            // The hash is still in it, as a wobble on the
                            // threshold rather than the choice itself -- without
                            // that the three zones would be clean concentric
                            // rings, which reads as a target rather than as
                            // damage.
                            const float rad01 =
                                std::clamp( boff / std::max( 1e-4f, 0.55f * mBurnRad ), 0.f, 1.f );
                            const float pick =
                                std::clamp( rad01 + ( hash01( bs * 2246822519u ) - 0.5f ) * 0.30f,
                                            0.f,
                                            1.f );

                            // Where the zones sit. Mottle scales how far from
                            // "all base" it goes, so 0 still means one flat
                            // colour everywhere -- at 0 both thresholds reach 1
                            // and every blob is char.
                            const float pSoot = 1.f - 0.64f * burnMottle;
                            const float pAsh  = 0.43f * burnMottle;

                            // EACH KIND GETS ITS OWN COLOUR, not a multiplier on
                            // one base -- and that swap is the whole fix for
                            // "still too unicolor".
                            //
                            // The three kinds used to be sat/brightness knobs
                            // applied to rt_arc_burn_color. That colour is
                            // 0x0A0807: an albedo of about 0.035. Multiplying it
                            // by anything in a 0.1..1.4 range lands every kind
                            // between 0.004 and 0.05 -- so "char", "ash" and
                            // "scorch" were three names for three shades of
                            // black, and six overlapping shades of black average
                            // back to black. The names were right and the maths
                            // could never deliver them, because you cannot
                            // saturate your way to brown from something that has
                            // no brown in it.
                            //
                            // So ash and scorch are real destinations now, and
                            // rt_arc_burn_color stays the CHAR anchor, which is
                            // what it always meant. rt_arc_burn_spread says how
                            // far the other two travel from it, so the whole
                            // range still collapses to one colour at 0.
                            const float spread =
                                std::clamp( float{ cvar::rt_arc_burn_spread }, 0.f, 1.f );

                            // ALL THREE ARE DERIVED FROM rt_arc_burn_color, not
                            // hardcoded beside it, and that is what makes the
                            // knob mean something.
                            //
                            // They were absolute colours for one round, and the
                            // moment the base moved from 3.5% to 15% the fixed
                            // ash and scorch (10% and 8%) became DARKER than the
                            // char they were supposed to sit above -- the ladder
                            // silently inverted and the core stopped being the
                            // dark part. Anchoring them to the base means the
                            // order survives any colour: char is the base
                            // pushed down, ash is the base drained of hue and
                            // lifted, scorch is the base warmed. Pick black and
                            // the whole mark is black; pick dark brown and the
                            // core is still the darkest thing in it.
                            //
                            // A BURN IS NEVER LIGHTER THAN WHAT IT BURNED, and
                            // the first attempt at these was: ash at 0.42 and
                            // scorch at 0.29 turned the mark into a pale tan
                            // blob standing out BRIGHTER than the wall. "Ash is
                            // light" is true of a cold fireplace and false of a
                            // scorch on a dark wall -- what matters is that the
                            // three read as different from EACH OTHER while all
                            // three stay darker than the surface. So the ladder
                            // below is black -> soot grey -> dark umber, and the
                            // whole of it sits under the wall's own value.
                            //
                            // Ash: soot grey. Lighter than char, hueless.
                            const float kAshR = grey * 1.35f;
                            const float kAshG = grey * 1.32f;
                            const float kAshB = grey * 1.28f;
                            // Scorch: burnt umber. Where the heat browned the
                            // surface rather than carbonising it.
                            const float kScoR = std::min( 1.f, vr * 1.30f );
                            const float kScoG = vg * 0.92f;
                            const float kScoB = vb * 0.62f;

                            float tr = vr, tg = vg, tb = vb;

                            if( pick < pSoot )
                            {
                                // CHAR. The base, pushed darker still.
                                const float mul = 0.22f + 0.45f * jit;
                                tr = vr * mul;
                                tg = vg * mul;
                                tb = vb * mul;
                            }
                            else if( pick < pSoot + pAsh )
                            {
                                const float mul = 0.70f + 0.60f * jit;
                                tr = ( vr + ( kAshR - vr ) * spread ) * mul;
                                tg = ( vg + ( kAshG - vg ) * spread ) * mul;
                                tb = ( vb + ( kAshB - vb ) * spread ) * mul;
                            }
                            else
                            {
                                const float mul = 0.70f + 0.60f * jit;
                                tr = ( vr + ( kScoR - vr ) * spread ) * mul;
                                tg = ( vg + ( kScoG - vg ) * spread ) * mul;
                                tb = ( vb + ( kScoB - vb ) * spread ) * mul;
                            }

                            (void)grey;
                            vr = std::clamp( tr, 0.f, 1.f );
                            vg = std::clamp( tg, 0.f, 1.f );
                            vb = std::clamp( tb, 0.f, 1.f );
                        }

                        const float av = std::clamp( burnStr * bfade * aMul, 0.f, 1.f );
                        const RgColor4DPacked32 cc =
                            rt.rgUtilPackColorFloat4D( vr, vg, vb, av );
                        const RgColor4DPacked32 cr =
                            rt.rgUtilPackColorFloat4D( vr, vg, vb, 0.f );

                        // Per-blob outline phases, so no two are the same lumpy
                        // shape even at the same size.
                        const float w1 = hash01( bs * 668265263u ) * 2.f * rt_pi();
                        const float w2 = hash01( bs * 3266489917u ) * 2.f * rt_pi();

                        const uint32_t base = uint32_t( s_batchArcBurn.verts.size() );
                        s_batchArcBurn.verts.push_back( RgPrimitiveVertex{
                            .position     = { c0.X, c0.Y, c0.Z },
                            .normalPacked = bn,
                            .texCoord     = { 0.5f, 0.5f },
                            .color        = cc,
                        } );
                        for( int k = 0; k < kBurnSegs; k++ )
                        {
                            const float a2 = ( 2.f * rt_pi() * float( k ) ) / float( kBurnSegs );
                            // Periodic by construction -- whole multiples of the
                            // angle only, so the outline closes seamlessly. A
                            // burn is not round, but its edge is CONTINUOUS:
                            // lumpy, not spiked. An independently hashed radius
                            // per vertex is what makes a star.
                            const float wob = 1.f + 0.20f * std::sin( a2 * 2.f + w1 ) +
                                              0.12f * std::sin( a2 * 3.f + w2 );
                            const float rr = mBurnRad * bscale * wob;
                            const FVector3 pv = c0 + m.tan * ( std::cos( a2 ) * rr ) +
                                                m.bit * ( std::sin( a2 ) * rr );
                            s_batchArcBurn.verts.push_back( RgPrimitiveVertex{
                                .position     = { pv.X, pv.Y, pv.Z },
                                .normalPacked = bn,
                                .texCoord     = { 0.5f + 0.5f * std::cos( a2 ),
                                                  0.5f + 0.5f * std::sin( a2 ) },
                                .color        = cr,
                            } );
                        }
                        for( int k = 0; k < kBurnSegs; k++ )
                        {
                            s_batchArcBurn.idx.push_back( base );
                            s_batchArcBurn.idx.push_back( base + 1 + uint32_t( k ) );
                            s_batchArcBurn.idx.push_back(
                                base + 1 + uint32_t( ( k + 1 ) % kBurnSegs ) );
                        }

                        // The blob's own primitive. The id has to be unique
                        // across every blob of every live mark, so the mark's
                        // uid is strided by the blob ceiling -- RTGL1 keeps one
                        // upload per id and it is the LATER one that loses, so a
                        // collision would silently delete a blob rather than
                        // misplace it.
                        UploadAoBlob( s_batchArcBurn,
                                      RT_ARC_BURN_MESH_ID +
                                          uint64_t( m.uid % 4096u ) * 8ull + uint64_t( bi ) );
                    }

                    // Each blob uploaded itself above. The ids are keyed on the
                    // mark's uid rather than on a running counter: RTGL1 keeps
                    // one upload per id and it is the LATER one that loses, so a
                    // counter -- which renumbers every mark whenever an older
                    // one is evicted -- would make scorches swap places.
                    // Pitfall 34.
                    burnEmitted++;
                }
            }

            // THE FILIGREE'S OWN CLOCK, and the MARK's copy of it -- not m.life,
            // which is the scorch's and far longer, and not the cvar, which
            // would hand a plasma mark the BFG's life whenever a BFG fired last.
            // EMBERS: a few hot spots still glowing IN the churn.
            //
            // Drawn here, on the mark, because that is what they are. They were
            // first built as long-lived particles thrown out of the blast, and
            // that was the same mistake the arcs began with one feature earlier:
            // a thing that belongs ON a surface is not a thing in flight, and no
            // amount of gravity, drag or bounce tuning turns one into the other.
            //
            // Small, few, and stationary. The count is single digits by design.
            const int emberN = EmberCountFor( m );

            if( m.fx == ImpactFx::Ember && wantEmber && emberN > 0 )
            {
                const float et = std::clamp( m.age / emberLife, 0.f, 1.f );
                if( et < 1.f )
                {
                    const float efade       = ( 1.f - et ) * ( 1.f - et );
                    const float emberBright = emberBrightBase * m.emberBright;

                    // The cooling colour, interpolated across MISL's own ten
                    // entries. Not quantized: an ember is watched for seconds,
                    // and hard steps on something held still read as banding
                    // rather than as style.
                    float er, eg, eb;
                    {
                        const float f  = et * float( RT_EMBER_RAMP_N - 1 );
                        const int   i0 = std::clamp( int( f ), 0, RT_EMBER_RAMP_N - 1 );
                        const int   i1 = std::min( i0 + 1, RT_EMBER_RAMP_N - 1 );
                        const float fr = f - float( i0 );
                        const uint32_t c0 = RT_EMBER_RAMP[ i0 ];
                        const uint32_t c1 = RT_EMBER_RAMP[ i1 ];
                        auto l_l = [ & ]( int sh ) {
                            const float a0 = ( ( c0 >> sh ) & 0xFF ) / 255.f;
                            const float a1 = ( ( c1 >> sh ) & 0xFF ) / 255.f;
                            return a0 + ( a1 - a0 ) * fr;
                        };
                        er = l_l( 16 );
                        eg = l_l( 8 );
                        eb = l_l( 0 );
                    }

                    const RgNormalPacked32 en =
                        rt.rgUtilPackNormal( m.nrm.X, m.nrm.Y, m.nrm.Z );

                    // The textured coals accumulate here and are uploaded as ONE
                    // traced primitive once this mark's loop is done. Cleared
                    // per mark, not per frame -- see the note on the batch.
                    s_batchEmberArt.verts.clear();
                    s_batchEmberArt.idx.clear();

                    for( int e = 0; e < emberN; e++ )
                    {
                        // Each ember its own size and its own slightly offset
                        // cooling phase, so they do not all go dark together --
                        // a handful of spots fading in perfect unison is the
                        // tell that they are one object rather than several.
                        const uint32_t eh = m.seed + 0x9E3779B9u + uint32_t( e ) * 40503u;
                        // PER MARK, like the count: a coal sized for a rocket's
                        // mark is a speck in a scorch several times as wide.
                        const float sz = emberSize * m.emberSize * ( 0.6f + 0.8f * hash01( eh ) );

                        // EMBERS BREATHE. A real coal does not cool smoothly --
                        // it pulses as the draught over it changes, brightening
                        // and dimming on its own schedule, and that irregularity
                        // is most of what says the thing is still burning rather
                        // than being a painted orange dot.
                        //
                        // TWO SINES AT INCOMMENSURATE RATES, plus a per-ember
                        // phase. One sine is a heartbeat and the eye locks onto
                        // its period immediately; two that never line up read as
                        // unpredictable while costing one more multiply. The
                        // rates are deliberately not related by a simple ratio.
                        //
                        // Driven off the mark's AGE, not the frame clock: an
                        // ember is a fixed thing and its glow must not depend on
                        // frame rate the way a per-frame random would.
                        float pulse = 1.f;
                        if( emberFlicker > 0.f )
                        {
                            const float ph = hash01( eh * 22695477u ) * 2.f * rt_pi();
                            const float s1 =
                                std::sin( m.age * emberRate * 2.f * rt_pi() + ph );
                            const float s2 =
                                std::sin( m.age * emberRate * 2.61803f * rt_pi() + ph * 1.7f );
                            // 0..1, weighted so the BRIGHT end is narrow and the
                            // dim end is broad -- a coal spends more of its time
                            // dull than glowing, and an even sine reads as a
                            // pulsing lamp instead.
                            const float w = ( s1 * 0.65f + s2 * 0.35f ) * 0.5f + 0.5f;
                            pulse = 1.f - emberFlicker * ( 1.f - w * w );
                        }

                        // PER EMBER, because the pulse is. Packing the colour
                        // once outside the loop is what made them all breathe
                        // in lockstep, which is the one thing a set of coals
                        // must not do.
                        const RgColor4DPacked32 ec = rt.rgUtilPackColorFloat4D(
                            er, eg, eb, std::clamp( emberBright * efade * pulse, 0.f, 1.f ) );

                        // The pulse SIZE as well as its brightness, gently. A
                        // coal that only changes brightness reads as a light
                        // being dimmed; one that also swells slightly reads as
                        // something burning. Kept small -- at more than a few
                        // percent it becomes a throb.
                        const FVector3 c = EmberPos( m, e, mBurnRad ) + m.nrm * 0.006f;

                        // IN THE SURFACE PLANE, not camera-facing. It is burnt
                        // into the wall and must foreshorten with it; a
                        // camera-facing quad would stay a full disc at grazing
                        // angles and give away that nothing is really on the
                        // surface.
                        const float szp = sz * ( 0.92f + 0.08f * pulse );

                        // ART OR A FLAT QUAD, decided per MARK. A barrel's coals
                        // wear the authored ember sheet; a rocket's five stay the
                        // flat quad they were accepted as. One switch for both
                        // would be the coupling this file keeps having to undo.
                        //
                        // Both go into an ADDITIVE batch, so nothing here needs
                        // an alpha test: transparent texels multiply the
                        // contribution to zero and add nothing. That is the one
                        // real difference from the plate, which is opaque and
                        // does need ALPHA_TESTED to get a shape at all.
                        const bool  useArt = m.emberArt && EmberArtName() != nullptr;
                        QuadBatch&  eb_    = useArt ? s_batchEmberArt : s_batchSpark;

                        // The sheet is taller than it is wide, and squashing it
                        // into a square would be the one thing that makes hand-
                        // drawn art look procedural again.
                        const float ea = useArt ? EmberArtAspect() : 1.f;

                        FVector3 ex, ey, cc = c;

                        if( useArt )
                        {
                            // IT STANDS UP, and this is what made the textured
                            // coals visible at all.
                            //
                            // An untextured coal is IN THE SURFACE PLANE on
                            // purpose -- it is a hot spot burnt into the wall,
                            // and a camera-facing disc would stay a full circle
                            // at grazing angles and give away that nothing is
                            // really on the surface. That reasoning is right for
                            // a burn mark and wrong for this art, which is not a
                            // burn mark: it is FLAME. Lying flat, a 7 cm quad
                            // seen from standing height projects to a two-pixel
                            // sliver, which is why three isolation runs looked
                            // identical to the eye while a pixel diff proved the
                            // size knob was moving three thousand of them.
                            //
                            // So a textured coal is a billboard standing ON the
                            // surface: vertical, turned to face the camera about
                            // that vertical. Which is how every fire sprite in
                            // the game works, and for the same reason.
                            FVector3 sideways = right - FVector3{ 0, 0, 1 } * right.Z;
                            if( sideways.LengthSquared() < 1e-6f )
                            {
                                sideways = FVector3{ 1, 0, 0 };
                            }
                            sideways.MakeUnit();

                            ex = sideways * ( szp * ea );
                            ey = FVector3{ 0, 0, 1 } * szp;
                            // Its FOOT sits on the surface rather than its
                            // middle, or half the flame is under the floor.
                            cc = c + FVector3{ 0, 0, 1 } * szp;
                        }
                        else
                        {
                            ex = m.tan * ( ea >= 1.f ? szp : szp * ea );
                            ey = m.bit * ( ea >= 1.f ? szp / ea : szp );
                        }

                        const FVector3 cr[ 4 ] = {
                            cc - ex - ey, cc + ex - ey, cc + ex + ey, cc - ex + ey
                        };

                        // FLIPPED PER COAL. Fifty coals off one sheet would
                        // otherwise be fifty copies of one shape lying in the
                        // same orientation, which reads as a stamp rather than
                        // as fire. Two hashed bits: mirror in u, mirror in v.
                        const bool fu = useArt && hash01( m.seed + uint32_t( e ) * 2654435761u ) > 0.5f;
                        const bool fv = useArt && hash01( m.seed + uint32_t( e ) * 40503u + 7u ) > 0.5f;

                        float uv[ 4 ][ 2 ] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
                        for( int k = 0; k < 4; k++ )
                        {
                            if( fu ) uv[ k ][ 0 ] = 1.f - uv[ k ][ 0 ];
                            if( fv ) uv[ k ][ 1 ] = 1.f - uv[ k ][ 1 ];
                        }

                        const uint32_t base = uint32_t( eb_.verts.size() );
                        for( int k = 0; k < 4; k++ )
                        {
                            eb_.verts.push_back( RgPrimitiveVertex{
                                .position     = { cr[ k ].X, cr[ k ].Y, cr[ k ].Z },
                                .normalPacked = en,
                                .texCoord     = { uv[ k ][ 0 ], uv[ k ][ 1 ] },
                                .color        = ec,
                            } );
                        }
                        eb_.idx.push_back( base );
                        eb_.idx.push_back( base + 1 );
                        eb_.idx.push_back( base + 2 );
                        eb_.idx.push_back( base );
                        eb_.idx.push_back( base + 2 );
                        eb_.idx.push_back( base + 3 );

                        // BOTH WINDINGS FOR THE TEXTURED COAL, and this is the
                        // whole reason it rendered nothing.
                        //
                        // The winding here comes out of tan/bitangent, which are
                        // derived from the surface normal and may face either
                        // way round the mark. That never mattered while a coal
                        // was ADDITIVE, because the rasterized overlay does not
                        // cull; the moment it became traced geometry a
                        // back-facing quad simply vanished. Three isolation runs
                        // at different sizes and emissive strengths came back
                        // PIXEL-IDENTICAL, which is what ruled out every value
                        // and pointed at the geometry.
                        //
                        // Two more triangles rather than reasoning about the
                        // sign: the shard path reached the same conclusion for
                        // the same reason, and a coal seen from its back should
                        // be a coal either way.
                        if( useArt )
                        {
                            eb_.idx.push_back( base );
                            eb_.idx.push_back( base + 2 );
                            eb_.idx.push_back( base + 1 );
                            eb_.idx.push_back( base );
                            eb_.idx.push_back( base + 3 );
                            eb_.idx.push_back( base + 2 );
                        }
                        s_dbgQuads++;

                        // THE HALO, and it is standing in for something the
                        // renderer genuinely cannot do here.
                        //
                        // The ember's analytic light is cast correctly and
                        // traced correctly -- but it lands on wall the scorch
                        // decal has multiplied down to a tenth of its albedo, so
                        // there is nothing left to bounce it and the glow dies
                        // exactly where the embers are. Reported from play as
                        // the churn "hiding" their glow, which is precisely
                        // right. No light VALUE can fix it: ten times almost
                        // nothing is still almost nothing.
                        //
                        // Additive geometry does not consult albedo at all, so
                        // this reads over the black burn. Screen glow only --
                        // it lights nothing, and the real light above is still
                        // what illuminates the clean wall outside the mark.
                        if( emberHalo > 0.f && emberHaloA > 0.f )
                        {
                            const float hr = szp * emberHalo;
                            const RgColor4DPacked32 hc = rt.rgUtilPackColorFloat4D(
                                er,
                                eg,
                                eb,
                                std::clamp( emberBright * efade * pulse * emberHaloA,
                                            0.f,
                                            1.f ) );
                            // RIM ALPHA ZERO. A uniform disc has a hard edge and
                            // reads as a sticker; the whole point of a halo is
                            // that it has no edge.
                            const RgColor4DPacked32 hrim =
                                rt.rgUtilPackColorFloat4D( er, eg, eb, 0.f );

                            constexpr int kHaloSegs = 10;
                            const uint32_t hb = uint32_t( s_batchSpark.verts.size() );
                            s_batchSpark.verts.push_back( RgPrimitiveVertex{
                                .position     = { c.X, c.Y, c.Z },
                                .normalPacked = en,
                                .texCoord     = { 0.5f, 0.5f },
                                .color        = hc,
                            } );
                            for( int k = 0; k < kHaloSegs; k++ )
                            {
                                const float ha =
                                    ( 2.f * rt_pi() * float( k ) ) / float( kHaloSegs );
                                const FVector3 hp = c + m.tan * ( std::cos( ha ) * hr ) +
                                                    m.bit * ( std::sin( ha ) * hr );
                                s_batchSpark.verts.push_back( RgPrimitiveVertex{
                                    .position     = { hp.X, hp.Y, hp.Z },
                                    .normalPacked = en,
                                    .texCoord     = { 0.5f + 0.5f * std::cos( ha ),
                                                      0.5f + 0.5f * std::sin( ha ) },
                                    .color        = hrim,
                                } );
                            }
                            for( int k = 0; k < kHaloSegs; k++ )
                            {
                                s_batchSpark.idx.push_back( hb );
                                s_batchSpark.idx.push_back( hb + 1 + uint32_t( k ) );
                                s_batchSpark.idx.push_back(
                                    hb + 1 + uint32_t( ( k + 1 ) % kHaloSegs ) );
                            }
                            s_dbgQuads++;
                        }

                        // AN EMBER CASTS LIGHT, on the same terms the arc
                        // branches do: the quad is an additive rasterized
                        // overlay and lights nothing by itself. Linear fade for
                        // the same reason -- what has to agree is when the two
                        // reach zero, not the shape in between.
                        if( wantGlow )
                        {
                            s_arcLights.push_back( ArcLightCand{
                                c + m.nrm * 0.05f,
                                er,
                                eg,
                                eb,
                                // The pulse rides the light too, so the glow on
                                // the wall breathes with the ember casting it.
                                // Linear fade for the reason the arcs' is --
                                // what has to agree is when the two reach zero.
                                std::clamp( emberBright * ( 1.f - et ) * 0.7f * pulse, 0.f, 1.f ),
                                emberGlowInt,
                                emberGlowRad,
                                // The creepers' upper half is already taken, so
                                // embers ride the branch half -- an ember mark
                                // never draws branches, so the two can never be
                                // alive on the same mark and cannot collide.
                                //
                                // THE STRIDE IS THE EMBER CAP, NOT THE BRANCH
                                // ONE, and this was a live bug the moment coals
                                // stopped borrowing the branch ceiling. With a
                                // stride of 2 * 24 and up to 96 coals, mark N's
                                // ember ids ran straight through mark N+1's
                                // whole range -- so a barrel's fiftieth coal and
                                // a nearby ROCKET's first claimed the same light
                                // id. RTGL1 keeps one light per id, so one of
                                // them silently lost its glow, and which one
                                // depended on upload order. Widening the stride
                                // to the largest thing that can hang off a mark
                                // makes the ranges disjoint by construction.
                                ArcGlowId_Base +
                                    uint64_t( m.uid ) *
                                        uint64_t( RT_ARC_MAX_BRANCH * 2 + RT_ARC_MAX_EMBER ) +
                                    uint64_t( e ),
                            } );
                        }
                    }

                    // THE TEXTURED COALS, one traced primitive for this mark.
                    // The emissive term carries the whole bed's cooling, because
                    // with an albedo texture present RTGL1 ignores the primitive
                    // colour and there is nowhere else for a fade to live.
                    if( !s_batchEmberArt.verts.empty() )
                    {
                        UploadBatch( s_batchEmberArt,
                                     RT_EMBER_ART_MESH_ID + uint64_t( ai ),
                                     /*additive=*/false,
                                     EmberArtName(),
                                     RG_PACKED_COLOR_WHITE,
                                     RG_MESH_PRIMITIVE_ALPHA_TESTED,
                                     std::max( 0.f, float{ cvar::rt_barrel_ember_emis } ) * efade );
                    }
                }
            }

            // A ROCKET MARK IS SCORCH AND EMBERS ONLY: no filigree ever. Tested
            // before the clock, because its arcLife is 0 and dividing by the
            // guard would give t = 1 anyway -- but relying on that would make
            // the intent depend on an arithmetic accident.
            if( !m.arcs )
            {
                continue;
            }

            const float t = std::clamp( m.age / std::max( 1e-4f, m.arcLife ), 0.f, 1.f );
            if( t >= 1.f )
            {
                continue; // arcs done; the scorch above carries on alone
            }

            // THE WEAPON'S SIZE MULTIPLIER. The cvars are authored for the
            // plasma rifle; a BFG mark is twice the thing in every dimension.
            const float mScale = st.scale;
            const float reach  = reachBase * mScale;
            const float wid    = widBase * mScale;

            // INTERPOLATED, always -- rt_spark_style is not consulted here. The
            // pixelated mode exists because a spark is a fragment the size of a
            // texel and quantizing it is the whole look. An arc is a hairline,
            // already far thinner than a texel, and stepping its colour through
            // eight hard entries makes it read as a dashed line rather than as a
            // discharge. This is the "do not over-apply retro styling" rule.
            float ar, ag, ab;
            {
                const float    f  = t * float( st.rampN - 1 );
                const int      i0 = std::clamp( int( f ), 0, st.rampN - 1 );
                const int      i1 = std::min( i0 + 1, st.rampN - 1 );
                const float    fr = f - float( i0 );
                const uint32_t c0 = st.ramp[ i0 ];
                const uint32_t c1 = st.ramp[ i1 ];
                auto l_lerp = [ & ]( int shift ) {
                    const float a0 = ( ( c0 >> shift ) & 0xFF ) / 255.f;
                    const float a1 = ( ( c1 >> shift ) & 0xFF ) / 255.f;
                    return a0 + ( a1 - a0 ) * fr;
                };
                ar = l_lerp( 16 );
                ag = l_lerp( 8 );
                ab = l_lerp( 0 );
            }

            const float fade = ( 1.f - t ) * ( 1.f - t );

            // OFF THE SURFACE BY A CENTIMETRE. Coplanar with the wall the quads
            // z-fight against it, which reads as the arcs flashing in and out at
            // grazing angles -- and would be easy to misdiagnose as the flicker
            // below misbehaving.
            const FVector3         lift = m.nrm * 0.012f;
            const RgNormalPacked32 anrm = rt.rgUtilPackNormal( m.nrm.X, m.nrm.Y, m.nrm.Z );

            // One in-plane quad, from p to q, `hw` half-width either side.
            auto l_seg = [ & ]( const FVector3& p, const FVector3& q, float hw, float alpha ) {
                FVector3 d = q - p;
                if( d.LengthSquared() < 1e-10f )
                {
                    return;
                }
                d.MakeUnit();
                FVector3 perp = m.nrm ^ d;
                if( perp.LengthSquared() < 1e-10f )
                {
                    return;
                }
                perp.MakeUnit();
                perp *= hw;

                const RgColor4DPacked32 acol =
                    rt.rgUtilPackColorFloat4D( ar, ag, ab, std::clamp( alpha, 0.f, 1.f ) );

                const FVector3 cr[ 4 ] = { p - perp, q - perp, q + perp, p + perp };
                const float    uv[ 4 ][ 2 ] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };

                const uint32_t base = uint32_t( s_batchSpark.verts.size() );
                for( int k = 0; k < 4; k++ )
                {
                    s_batchSpark.verts.push_back( RgPrimitiveVertex{
                        .position     = { cr[ k ].X, cr[ k ].Y, cr[ k ].Z },
                        .normalPacked = anrm,
                        .texCoord     = { uv[ k ][ 0 ], uv[ k ][ 1 ] },
                        .color        = acol,
                    } );
                }
                s_batchSpark.idx.push_back( base );
                s_batchSpark.idx.push_back( base + 1 );
                s_batchSpark.idx.push_back( base + 2 );
                s_batchSpark.idx.push_back( base );
                s_batchSpark.idx.push_back( base + 2 );
                s_batchSpark.idx.push_back( base + 3 );
                s_dbgQuads++;
            };

            // THE RANDOM WALK, and the kinks are the whole point. Straight rays
            // out of a centre read as a starburst or a sun; what makes a
            // discharge legible as one is that it changes direction at every
            // step and no two branches agree.
            // THE RE-PATH CLOCK. Quantizing the age is what turns "re-roll the
            // walk" into visible motion instead of white noise: every hash below
            // is stable for 1/rt_arc_churn of a second and then changes
            // wholesale, so the branch VISIBLY jumps to a new path. Hashing
            // against a continuous time would re-roll every frame, and at 100+
            // fps the eye integrates that straight back into a smooth blur --
            // the motion vanishes exactly when the most of it is added. Same
            // trap as rt_arc_flicker_rate, and the same fix.
            const uint32_t churnIdx =
                churn > 0.f ? uint32_t( std::max( 0.f, m.age ) * churn ) : 0u;

            auto l_walk = [ & ]( FVector3 p, float ang, int steps, uint32_t rs, float wscale ) {
                const float seg = reach / float( nseg );
                for( int s = 0; s < steps; s++ )
                {
                    // The churn index is mixed into EVERY per-step hash, so a
                    // re-path changes the whole skeleton rather than nudging it.
                    const uint32_t ss =
                        rs + uint32_t( s ) * 40503u + churnIdx * 2246822519u;

                    // RETRACTS FROM THE TIPS AS IT DIES, rather than the whole
                    // filigree dimming uniformly. A discharge collapses back
                    // toward its source; a uniform fade reads as someone turning
                    // a dimmer down, which is the one thing electricity does not
                    // do.
                    const float along = float( s + 1 ) / float( nseg );
                    if( along > 1.f - t * 0.85f )
                    {
                        break;
                    }

                    ang += ( hash01( ss ) * 2.f - 1.f ) * jit;
                    const float    L = seg * ( 0.55f + 0.9f * hash01( ss * 7919u ) );
                    const FVector3 d = m.tan * std::cos( ang ) + m.bit * std::sin( ang );
                    const FVector3 q = p + d * L;

                    // Tapered toward the tip in both width and brightness: a
                    // constant-width polyline reads as a drawn line, and the
                    // taper is what makes it read as energy dissipating.
                    const float tip = 1.f - float( s ) / float( nseg );
                    l_seg( p,
                           q,
                           wid * 0.5f * wscale * ( 0.45f + 0.55f * tip ),
                           abr * fade * ( 0.30f + 0.70f * tip ) );

                    p = q;
                }
                return p;
            };

            for( int b = 0; b < nbr; b++ )
            {
                const uint32_t bs = m.seed + uint32_t( b ) * 2654435761u;

                // CRACKLE: a branch may be absent for a frame entirely, and that
                // is more of what sells this as electric than any amount of
                // brightness modulation. Quantizing the age means a branch stays
                // out for a few frames rather than strobing every one, which at
                // 100+ fps would average back out to "always on, slightly dim".
                if( flk > 0.f )
                {
                    const uint32_t ph =
                        uint32_t( std::max( 0.f, m.age ) * flkR * 12.f );
                    if( hash01( bs ^ ( ph * 374761393u ) ) < flk * 0.35f )
                    {
                        continue;
                    }
                }

                // Evenly spaced around the impact, then jittered by up to one
                // full slot so the ring is not visibly regular.
                //
                // THE ROOT WANDERS, IT DOES NOT TELEPORT. The base angle keeps
                // its per-branch offset for the mark's whole life and only
                // drifts by +/- rt_arc_wander across a re-path. Re-rolling the
                // root along with everything else was tried in the walk above
                // and reads as noise: branches swap places around the ring
                // between frames and the eye stops tracking any one of them.
                // Anchored ends with a restless middle is what a real arc does.
                const float ang0 =
                    ( 2.f * rt_pi() * float( b ) ) / float( nbr ) +
                    hash01( bs ) * ( 2.f * rt_pi() / float( nbr ) ) +
                    ( hash01( bs + churnIdx * 668265263u ) * 2.f - 1.f ) * wander;

                const FVector3 endp = l_walk( m.at + lift, ang0, nseg, bs, 1.f );

                // THE BRANCH'S LIGHT, taken from where the branch actually
                // ENDED this frame. Collected here rather than recomputed in
                // the light pass: a branch the crackle dropped never reaches
                // this line, so its light disappears with it for free, and the
                // re-pathed position can never disagree with the drawn one.
                if( wantGlow && abr > 0.f )
                {
                    // Lifted further off the wall than the quads are. A light
                    // sitting 12 mm off a surface lights almost none of it --
                    // the surface is nearly edge-on to it everywhere -- so the
                    // spill would be a tight hot dot instead of a wash.
                    // THE LIGHT TRACKS THE QUAD'S ALPHA, not the raw fade, and
                    // that mismatch is why the light used to die early.
                    //
                    // A branch quad's alpha is clamp(rt_arc_bright * fade, 0, 1),
                    // and at the shipping bright 3.0 that clamp holds it at FULL
                    // brightness until fade drops below 1/3 -- i.e. the arcs look
                    // constant for the first ~42% of the mark's life and only
                    // then start dimming. The light was using `fade` directly,
                    // which is already down to 0.25 by half-life. So the arcs
                    // were still visibly bright while the light they cast had
                    // all but gone, and the two came apart exactly where the eye
                    // is most likely to notice.
                    //
                    // A GENTLER CURVE THAN THE QUADS', deliberately, and this is
                    // the second correction to it. Matching the quad's alpha was
                    // already better than the raw squared fade, but it still
                    // read as dying early -- because `fade` is (1-t)^2 and even
                    // clamped it is falling steeply through the back half.
                    //
                    // The light uses LINEAR (1-t) instead, so at the shipping
                    // bright 3.0 it holds full until t=0.67 and then runs down
                    // to nothing exactly as the last hairlines go. The rule the
                    // earlier comment stated -- share one expression -- was the
                    // right instinct for the wrong quantity: what has to agree
                    // is when the two REACH ZERO, not the shape in between. A
                    // light and an emissive quad are not judged the same way,
                    // because one is being looked at and the other is lighting
                    // a wall several metres across.
                    s_arcLights.push_back( ArcLightCand{
                        endp + m.nrm * 0.05f,
                        ar,
                        ag,
                        ab,
                        std::clamp( abr * ( 1.f - t ), 0.f, 1.f ),
                        std::max( 0.f, float{ cvar::rt_arc_glow_intensity } ),
                        0.04f,
                        // Stride is 2 * RT_ARC_MAX_BRANCH, not RT_ARC_MAX_BRANCH:
                        // the creepers below occupy the upper half of every
                        // mark's block. Widening the stride and leaving the
                        // branches in the lower half is what keeps the two from
                        // overlapping -- and an overlap here is silent, since
                        // RTGL1 simply keeps one upload per ID.
                        ArcGlowId_Base +
                            uint64_t( m.uid ) * uint64_t( RT_ARC_MAX_BRANCH * 2 + RT_ARC_MAX_EMBER ) +
                            uint64_t( b ),
                    } );
                }

                // A FORK. Real arcs branch, and a set of unbranched polylines
                // still reads as a starburst however kinked each one is. Cheap
                // version: a shorter, thinner child off the end of the parent.
                if( forkP > 0.f && hash01( bs * 22695477u ) < forkP )
                {
                    l_walk( endp,
                            ang0 + ( hash01( bs * 69069u ) * 2.f - 1.f ) * 1.2f,
                            std::max( 1, nseg / 2 ),
                            bs * 40503u + 17u,
                            0.6f );
                }
            }

            // CREEPERS: short arcs that are NOT attached to the core and that
            // jump to a new place on the wall at every re-path.
            //
            // This is what makes the churn legible on the SURFACE. The radial
            // branches move, but they all still leave one fixed point in a fixed
            // number of directions, so their motion reads as wagging rather than
            // as the wall being energised -- reported from play as "just a ball +
            // arcs". A discharge that re-strikes somewhere it was not a moment
            // ago is the thing that says the surface itself is live, and it costs
            // one more walk per creeper.
            //
            // Their POSITION is hashed against churnIdx, not just their path, so
            // they teleport rather than drift. Teleporting is correct here and is
            // the opposite of the rule for the branch roots above: a root that
            // jumps destroys the identity of a branch you were tracking, whereas
            // a creeper has no identity to destroy -- it is a strike, and the
            // next one is a different strike.
            const int ncr = std::clamp( int{ cvar::rt_arc_creep }, 0, RT_ARC_MAX_BRANCH );
            for( int c = 0; c < ncr; c++ )
            {
                const uint32_t cs =
                    m.seed + 0x5F356495u + uint32_t( c ) * 2654435761u + churnIdx * 2246822519u;

                // Crackle applies to creepers too, and harder: at any moment a
                // good share of them should simply not be there. A full set
                // present every frame reads as a texture rather than as
                // sporadic discharge.
                if( flk > 0.f && hash01( cs * 668265263u ) < flk * 0.5f )
                {
                    continue;
                }

                const float cang = hash01( cs ) * 2.f * rt_pi();
                const float crad = std::sqrt( hash01( cs * 7919u ) ) * reach * crReach;
                const FVector3 cp = m.at + lift + m.tan * ( std::cos( cang ) * crad ) +
                                    m.bit * ( std::sin( cang ) * crad );

                // Short, thin, and dimmed with distance from the impact, so the
                // mark still has a clear centre of mass and does not turn into
                // an even field of scribble.
                const float falloff =
                    std::clamp( 1.f - ( crad / std::max( 0.01f, reach * crReach ) ) * 0.6f,
                                0.f,
                                1.f );
                const FVector3 cend = l_walk( cp,
                                              hash01( cs * 22695477u ) * 2.f * rt_pi(),
                                              std::max( 1, nseg / 2 ),
                                              cs,
                                              0.55f * falloff );

                // CREEPERS LIGHT THE WALL TOO, and leaving them out was why the
                // illumination stayed bunched around the centre while the
                // visible discharge had spread well past it. The creepers are
                // the outermost thing on the wall by design -- rt_arc_creep_reach
                // is 1.35x the branches -- so lighting only the radial tips lit
                // the one part of the mark that was never the point.
                //
                // Dimmer than a branch light, in the same proportion the creeper
                // is drawn dimmer, so the wall's bright spot still agrees with
                // the wall's bright geometry.
                if( wantGlow && abr > 0.f )
                {
                    s_arcLights.push_back( ArcLightCand{
                        cend + m.nrm * 0.05f,
                        ar,
                        ag,
                        ab,
                        std::clamp( abr * ( 1.f - t ) * 0.6f * falloff, 0.f, 1.f ),
                        std::max( 0.f, float{ cvar::rt_arc_glow_intensity } ),
                        0.04f,
                        // A DISJOINT ID RANGE from the branches above: the
                        // stride is 2 * RT_ARC_MAX_BRANCH and creepers sit in
                        // the upper half. Overlapping them would mean RTGL1
                        // keeping one of the pair and silently dropping the
                        // other, with no error anywhere.
                        ArcGlowId_Base +
                            uint64_t( m.uid ) * uint64_t( RT_ARC_MAX_BRANCH * 2 + RT_ARC_MAX_EMBER ) +
                            uint64_t( RT_ARC_MAX_BRANCH ) + uint64_t( c ),
                    } );
                }
            }

            // THE CORE -- the remains of the ball itself, sitting on the wall.
            // Shrinks and dies FASTER than the filigree (the cube against the
            // arcs' square), so what is left at the end is the last few
            // hairlines rather than a bright dot with nothing attached.
            if( coreR > 0.f )
            {
                const float cfade = ( 1.f - t ) * ( 1.f - t ) * ( 1.f - t );
                const float cr    = coreR * ( 0.35f + 0.65f * ( 1.f - t ) );
                const FVector3 c  = m.at + lift;

                const RgColor4DPacked32 ccol = rt.rgUtilPackColorFloat4D(
                    ar, ag, ab, std::clamp( abr * cfade, 0.f, 1.f ) );

                // An in-plane disc rather than a camera-facing one: it is a mark
                // ON the wall and must foreshorten with it. Seen edge-on it
                // nearly vanishes, which is correct -- a camera-facing core
                // would stay a full circle at grazing angles and give away that
                // nothing is really lying on the surface.
                constexpr int kCoreSegs = 10;
                const uint32_t cbase = uint32_t( s_batchSpark.verts.size() );
                s_batchSpark.verts.push_back( RgPrimitiveVertex{
                    .position     = { c.X, c.Y, c.Z },
                    .normalPacked = anrm,
                    .texCoord     = { 0.5f, 0.5f },
                    .color        = ccol,
                } );
                // Rim alpha at zero, so the core has no cut edge.
                const RgColor4DPacked32 rimcol =
                    rt.rgUtilPackColorFloat4D( ar, ag, ab, 0.f );
                for( int k = 0; k < kCoreSegs; k++ )
                {
                    const float a2 = ( 2.f * rt_pi() * float( k ) ) / float( kCoreSegs );
                    const FVector3 pv =
                        c + m.tan * ( std::cos( a2 ) * cr ) + m.bit * ( std::sin( a2 ) * cr );
                    s_batchSpark.verts.push_back( RgPrimitiveVertex{
                        .position     = { pv.X, pv.Y, pv.Z },
                        .normalPacked = anrm,
                        .texCoord     = { 0.5f + 0.5f * std::cos( a2 ),
                                          0.5f + 0.5f * std::sin( a2 ) },
                        .color        = rimcol,
                    } );
                }
                for( int k = 0; k < kCoreSegs; k++ )
                {
                    s_batchSpark.idx.push_back( cbase );
                    s_batchSpark.idx.push_back( cbase + 1 + uint32_t( k ) );
                    s_batchSpark.idx.push_back( cbase + 1 + uint32_t( ( k + 1 ) % kCoreSegs ) );
                }
                s_dbgQuads++;
            }
        }
    }

    UploadBatch( s_batchSpark, RT_SPARK_MESH_ID, true, nullptr, RG_PACKED_COLOR_WHITE );

    // THE BARREL PLATE. ALPHA_TESTED is what makes the cut-out a shape rather
    // than the rectangle its art sits in; WHITE because with an albedo texture
    // present RTGL1 ignores the primitive colour entirely (HitInfo.inl), so the
    // art is the whole of the look and nothing here can tint it.
    for( int i = 0; i < ShardArtCount() && i < RT_BARREL_ART_SLOTS; i++ )
    {
        UploadBatch( s_batchShard[ i ],
                     RT_SHARD_MESH_ID + uint64_t( i ),
                     false,
                     ShardArtName( i ),
                     RG_PACKED_COLOR_WHITE,
                     RG_MESH_PRIMITIVE_ALPHA_TESTED );
    }

    for( int i = 0; i < s_debrisBucketCount; i++ )
    {
        const DebrisBucket& db = s_debrisBuckets[ i ];

        // Distinct mesh IDs: RTGL1 keeps only one upload per ID and it is the
        // LATER one that loses, so a shared ID would silently drop every bucket
        // after the first.
        UploadBatch( db.batch,
                     RT_DEBRIS_MESH_ID + uint64_t( i ),
                     false,
                     ProfileFor( db.kind ).texture,
                     rt.rgUtilPackColorFloat4D( ( ( db.rgb >> 16 ) & 0xFF ) / 255.f,
                                                ( ( db.rgb >> 8 ) & 0xFF ) / 255.f,
                                                ( db.rgb & 0xFF ) / 255.f,
                                                1.0f ) );
    }
}


void RT_UploadSparkLights()
{
    s_dbgLights = 0;

    if( !SparkSystemOn() )
    {
        return;
    }

    UploadSparkGlowLights();
    UploadArcBranchLights();

    // EITHER flash source may be on. The per-flash test below is what keeps them
    // apart; this only skips the walk when neither can produce anything.
    if( ( !cvar::rt_spark_light && !cvar::rt_arc_light ) || g_sparkFlashCount == 0 )
    {
        return;
    }

    const auto&    vp = r_viewpoint;
    const FVector3 eye{ float( vp.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                        float( vp.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                        float( vp.Pos.Z ) * ONEGAMEUNIT_IN_METERS };

    // Nearest-first, then truncate -- the RT_UploadFlameLights pattern. The
    // INDEX is the light's identity, so sorting a copy rather than the pool
    // itself is not optional: a light whose id moved between frames would make
    // RTGL1 see the whole set die and respawn and throw away its temporal
    // reservoirs, and a flash lives ~0.18 s, so it would be reborn for its
    // entire life.
    struct Cand
    {
        float    d2;
        uint32_t slot;
    };
    static std::vector< Cand > cand;
    cand.clear();
    cand.reserve( g_sparkFlashCount );

    for( uint32_t i = 0; i < g_sparkFlashCount; i++ )
    {
        // Whichever source is switched off contributes no CANDIDATES, so it also
        // spends none of the budget below. Filtering after the sort would let a
        // disabled source crowd out an enabled one.
        if( s_flashes[ i ].isArc ? !cvar::rt_arc_light : !cvar::rt_spark_light )
        {
            continue;
        }
        cand.push_back( Cand{ float( ( s_flashes[ i ].pos - eye ).LengthSquared() ), i } );
    }
    if( cand.empty() )
    {
        return;
    }

    const size_t budget =
        size_t( std::max( 0, int{ cvar::rt_spark_light_max } ) );
    if( cand.size() > budget )
    {
        std::partial_sort( cand.begin(),
                           cand.begin() + budget,
                           cand.end(),
                           []( const Cand& a, const Cand& b ) { return a.d2 < b.d2; } );
        cand.resize( budget );
    }

    for( const Cand& c : cand )
    {
        const SparkFlash& fl = s_flashes[ c.slot ];

        // Fade over the life. The flash is the moment of impact, not a lamp.
        const float t = std::clamp( fl.age / std::max( 1e-4f, fl.life ), 0.f, 1.f );
        const float k = ( 1.f - t ) * ( 1.f - t );
        if( k <= 0.001f )
        {
            continue;
        }

        // The hot end of the ramp the particles are using, so the light and the
        // particles agree -- amber for a bullet, cyan for plasma, white-green for
        // the BFG. Read from the flash's own recorded flavour rather than from a
        // particle: by the time a flash is uploaded its arcs may all be dead.
        const uint32_t rgb =
            fl.isArc ? ArcStyleFor( fl.arc ).ramp[ 0 ] : RT_SPARK_RAMP[ 0 ];
        const float    kR  = ( ( rgb >> 16 ) & 0xFF ) / 255.f;
        const float    kG  = ( ( rgb >> 8 ) & 0xFF ) / 255.f;
        const float    kB  = ( rgb & 0xFF ) / 255.f;

        const float intensity =
            std::max( 0.f,
                      fl.isArc ? float{ cvar::rt_arc_light_intensity }
                               : float{ cvar::rt_spark_light_intensity } );

        auto sph = RgLightSphericalEXT{
            .sType     = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT,
            .pNext     = nullptr,
            .color     = rt.rgUtilPackColorFloat4D( kR, kG, kB, 1.0f ),
            .intensity = intensity * k,
            .position  = { fl.pos.X, fl.pos.Y, fl.pos.Z },
            .radius    = 0.05f,
        };
        auto info = RgLightInfo{
            .sType = RG_STRUCTURE_TYPE_LIGHT_INFO,
            .pNext = &sph,
            // THE SLOT, never the age or the tick -- see SparkFlashId_Base.
            .uniqueID     = SparkFlashId_Base + uint64_t( c.slot ),
            .isExportable = false,
        };
        RgResult r = rt.rgUploadLight( &info );
        RG_CHECK( r );

        s_dbgLights++;
    }
}

