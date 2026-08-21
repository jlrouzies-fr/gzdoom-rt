// RTRenderState::InternalDraw -- the funnel every primitive in the game passes
// through: it classifies the draw, resolves its material and hands it to RTGL1.
//
// Split out of the class body in rt_renderstate.h. Behaviour unchanged; the only
// edits are the RTRenderState:: qualification and one level of de-indentation.

#include "rt_renderstate.h"
#include "r_utility.h" // r_viewpoint, for the shadow-proxy distance cap

// The shared internals come in unqualified, exactly as when this code lived
// inside rt_main.cpp's anonymous namespace.
using namespace rtx;

namespace
{
// Doom64-RT: sprite shadow proxies -- see rt_sprite_shadow.
//
// Added to the sprite's own uniqueObjectID (an ACTOR POINTER) to name the proxy
// meshes. It has to be beyond any pointer the game can produce or a proxy would
// collide with a real object and RTGL1 would drop one of the two with "Trying to
// upload but a primitive with the same ID already exists" -- silently, and it is
// the proxy that would lose. Windows x64 user-space pointers stop at
// 0x00007FFF'FFFFFFFF, so bit 62 is unreachable by construction.
constexpr uint64_t RT_SPRITE_SHADOW_ID_BASE = 0x4000000000000000ull;

// Doom64-RT: sprite contact occlusion -- see rt_sprite_ao.
//
// Same reasoning as RT_SPRITE_SHADOW_ID_BASE, one bit up. Bits 61 and 62 are
// both beyond the 0x00007FFF'FFFFFFFF ceiling on a Windows x64 user pointer, and
// a proxy ID is (actor + bit62 + k<=3), so bit 61 cannot be reached by carrying
// out of one.
constexpr uint64_t RT_SPRITE_AO_ID_BASE = 0x2000000000000000ull;

// Fixed WORLD yaw for proxy plane k of n, spread evenly over 180 degrees.
// World-fixed rather than camera-relative is the entire point: the visible
// billboard turns to face the viewer, so the shadow it casts changes shape as
// the player rotates, which nothing physical does.
//
// 180 and not 360 because a plane's orientation is modulo pi -- a quad and its
// 180-degree twin are the same occluder. So the spacing is k*pi/n, DERIVED, not
// a table.
//
// It was a fixed table {0, pi/2, pi/6, pi/3} and that was wrong for n=3 and n=4:
// it packed every plane into a single quadrant (0/30/60/90) and left the
// 90-180 half uncovered, so how wide a shadow an actor cast depended on the
// LIGHT'S BEARING. Worst case fell to 0.71x the actor's width against 0.97x at
// the best -- a 27% swing with compass direction, which is the kind of thing
// that gets blamed on the art. n=2 was correct by luck (0/90) and is unchanged.
inline float RT_SpriteShadowYaw( int k, int n )
{
    return ( 3.14159265f * float( k ) ) / float( std::max( n, 1 ) );
}
} // namespace

void RTRenderState::InternalDraw( std::span< const RgPrimitiveVertex > verts,
                                  std::span< const uint32_t >          indices,
                                  const bool                           isUI,
                                  const bool                           islines )
{
    assert( RG_PACKED_COLOR_WHITE == rt.rgUtilPackColorByte4D( 255, 255, 255, 255 ) );

    if( islines && !isUI )
    {
        assert( 0 );
        return;
    }

    if( verts.empty() )
    {
        assert( 0 );
        return;
    }

    const char* texname = nullptr;
    if( mTextureEnabled && mMaterial.mMaterial )
    {
        if( FGameTexture* gametex = mMaterial.mMaterial->sourcetex )
        {
            if( FTexture* base = gametex->GetTexture() )
            {
                if( auto hwtex = static_cast< RTHardwareTexture* >( base->GetHardwareTexture(
                        mMaterial.mTranslation, mMaterial.mMaterial->GetScaleFlags() ) ) )
                {
                    hwtex->CreateIfWasnt( *gametex,
                                          mMaterial.mClampMode,
                                          mMaterial.mTranslation,
                                          mMaterial.mMaterial->GetScaleFlags(),
                                          mRenderStyle );
                    texname = hwtex->GetRTName();
                }
            }
        }
    }

    if( !texname && !isUI && !rtstate.is< RtPrim::Sky >() &&
        !rtstate.is< RtPrim::SkyVisibility >() )
    {
        // assert( 0 );
    }

    // TODO: apply texture matrix on gpu
    if( mTextureMatrixEnabled )
    {
        m_tempverts.clear();
        m_tempverts.assign( verts.begin(), verts.end() );

        // Doom64-RT: the TRANSLATION column, for sky primitives only.
        //
        // The stock code below applies the 2x2 linear part and nothing else,
        // so any texture-matrix TRANSLATION is silently dropped under RT.
        // That is not a small omission -- a scroll IS a translation, which
        // is why the cloud deck's wind moved nothing at all until this was
        // found. (VSMatrix is GL column-major: translate() writes mMatrix[12]
        // and [13], i.e. m(0,3) and m(1,3) here.)
        //
        // GATED ON SKY, deliberately, and this is the whole reason it is not
        // just fixed outright. hw_flats.cpp's hw_SetPlaneTextureRotation puts
        // a flat's panning offset (uoffs/voffs) and its rotation in here, so
        // every offset flat in the game has been drawn without them for as
        // long as this code has existed. Turning that on globally would shift
        // texture alignment across every map at once -- a real fix, but a
        // separate change with its own blast radius and its own playtest.
        //
        // The transposed 2x2 below (m(1,0) where the maths wants m(0,1)) is
        // left alone for the same reason: it only bites on rotation/shear,
        // which is again flats, and both sky matrices here are diagonal.
        const bool fullaffine = rtstate.is< RtPrim::Sky >();

        auto applyTexMatrix = [ & ]( float u, float v ) {
            auto m = [ & ]( int i, int j ) {
                return mTextureMatrix.get()[ i + j * 4 ];
            };

            const float tu = fullaffine ? m( 0, 3 ) : 0.f;
            const float tv = fullaffine ? m( 1, 3 ) : 0.f;

            return std::pair{
                m( 0, 0 ) * u + m( 1, 0 ) * v + tu,
                m( 0, 1 ) * u + m( 1, 1 ) * v + tv,
            };
        };

        for( RgPrimitiveVertex& v : m_tempverts )
        {
            std::tie( v.texCoord[ 0 ], v.texCoord[ 1 ] ) =
                applyTexMatrix( v.texCoord[ 0 ], v.texCoord[ 1 ] );
        }

        verts = m_tempverts;
    }

    if( rtstate.is< RtPrim::Sky >() && texname )
    {
        m_fb->RT_MarkWasSky();
    }

    // Doom64-RT: true = this draw is an ACTOR BILLBOARD whose camera-facing
    // rotation has been factored into `transform`, leaving `verts` un-rotated in
    // the sprite's own space. That is exactly the input a shadow proxy needs --
    // the same verts under a different rotation -- so it is recorded here rather
    // than re-derived at the bottom of the function.
    bool isSpriteBillboard = false;

    RgTransform transform;
    if( rtstate.is< RtPrim::FirstPerson >() )
    {
        std::tie( transform, verts ) = MakeFirstPersonQuadInWorldSpace( verts );

        // Anchor for rt_gunglow. Only the plasma frames — this is the weapon whose
        // art has a lit core; anchoring on any weapon would move the light onto guns
        // that are not supposed to emit.
        if( texname && verts.size() == 4 &&
            ( strncmp( texname, "PLSG", 4 ) == 0 || strncmp( texname, "PLSF", 4 ) == 0 ) )
        {
            FVector3 c{ 0, 0, 0 };
            for( const auto& v : verts )
            {
                c += FVector3{ v.position[ 0 ], v.position[ 1 ], v.position[ 2 ] };
            }
            m_gunAnchorView = c / float( verts.size() );
            m_haveGunAnchor = true;
        }

        // Ground truth for authoring a replacement viewmodel. A psprite model is
        // NOT at 1px = 1 map unit: this quad's size comes out of GetWeaponRect,
        // which depends on viewwidth/viewheight, SCREENWIDTH/HEIGHT,
        // WidescreenRatio, screenblocks and baseScale, and then goes through the
        // inverse projection here. Deriving that offline is guesswork, so just
        // print the view-space box the engine actually produced — a generated
        // model is authored to fill it. Values are metres, in the same space the
        // replacement's own vertices live in.
        if( cvar::rt_wpn_debug && texname && verts.size() == 4 )
        {
            static std::unordered_set< std::string > s_seenQuad;
            if( s_seenQuad.insert( texname ).second )
            {
                FVector3 lo{ verts[ 0 ].position[ 0 ],
                             verts[ 0 ].position[ 1 ],
                             verts[ 0 ].position[ 2 ] };
                FVector3 hi = lo;
                for( const auto& v : verts )
                {
                    for( int k = 0; k < 3; k++ )
                    {
                        lo[ k ] = std::min( lo[ k ], v.position[ k ] );
                        hi[ k ] = std::max( hi[ k ], v.position[ k ] );
                    }
                }
                Printf( "RTWPNQUAD %s min=(%.4f %.4f %.4f) max=(%.4f %.4f %.4f) "
                        "size=(%.4f %.4f %.4f) m\n",
                        texname,
                        lo[ 0 ], lo[ 1 ], lo[ 2 ],
                        hi[ 0 ], hi[ 1 ], hi[ 2 ],
                        hi[ 0 ] - lo[ 0 ], hi[ 1 ] - lo[ 1 ], hi[ 2 ] - lo[ 2 ] );
            }
        }
    }
    else if( RequiresTrueTransform() )
    {
        std::tie( transform, verts ) = CalculateTrueTransformAndItsVerts( verts );
        isSpriteBillboard            = true;
    }
    else
    {
        transform = MakeTransform( rtstate.is< RtPrim::Sky >() );
    }

    auto ui = RgMeshPrimitiveSwapchainedEXT{
        .sType       = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_SWAPCHAINED_EXT,
        .pNext       = nullptr,
        .flags       = islines ? uint32_t{ RG_MESH_PRIMITIVE_SWAPCHAINED_DRAW_AS_LINES } : 0,
        .pViewport   = &m_viewport,
        .pView       = m_view,
        .pProjection = m_projection,
        .pViewProjection = nullptr,
    };

    auto l_makeInstanceFlags = [ & ]() -> RgMeshInfoFlags {
        if( rtstate.is< RtPrim::FirstPersonViewer >() )
        {
            return RG_MESH_FIRST_PERSON_VIEWER;
        }
        if( rtstate.is< RtPrim::FirstPerson >() )
        {
            return RG_MESH_FIRST_PERSON;
        }
        return 0;
    };

    auto l_makeSpectreFlags = [ & ]() -> RgMeshInfoFlags {
        if( IsSpectre() )
        {
            // suppress inter-reflection on spectres
            return RG_MESH_FORCE_IGNORE_REFRACT_AFTER;
        }
        return 0;
    };

    auto mesh = RgMeshInfo{
        .sType = RG_STRUCTURE_TYPE_MESH_INFO,
        .pNext = nullptr,
        .flags =
            l_makeInstanceFlags() | l_makeSpectreFlags() |
            ( rtstate.is< RtPrim::ExportInstance >() ? RG_MESH_EXPORT_AS_SEPARATE_FILE : 0 ),
        .uniqueObjectID = rtstate.get_uniqueid(),
        .pMeshName      = rtstate.is< RtPrim::ExportMap >() ? RT_GetMapName()
                          : rtstate.is< RtPrim::ExportInstance >()
                              ? rtstate.get_exportinstance_name()
                              : nullptr,
        .transform      = transform,
        .isExportable =
            rtstate.is< RtPrim::ExportMap >() || rtstate.is< RtPrim::ExportInstance >(),
        .animationTime        = 0.0f,
        .localLightsIntensity = MapLightLevel( rtstate.m_lightlevel ),
    };

    auto makePrimFlags = [ this, &verts, &isSpriteBillboard ]( bool isUI ) -> RgMeshPrimitiveFlags {
        if( isUI )
        {
            return RG_MESH_PRIMITIVE_TRANSLUCENT;
        }
        if( rtstate.is< RtPrim::Decal >() )
        {
            assert( verts.size() == 4 );
            return RG_MESH_PRIMITIVE_DECAL;
        }
        if( rtstate.is< RtPrim::SkyVisibility >() )
        {
            RT_NoteSkyPrim( verts );
            return RG_MESH_PRIMITIVE_SKY_VISIBILITY;
        }
        if( rtstate.is< RtPrim::Sky >() )
        {
            return RG_MESH_PRIMITIVE_SKY | RG_MESH_PRIMITIVE_TRANSLUCENT;
        }
        if( rtstate.is< RtPrim::Particle >() )
        {
            return RG_MESH_PRIMITIVE_TRANSLUCENT;
        }
        if( rtstate.is< RtPrim::Mirror >() )
        {
            return RG_MESH_PRIMITIVE_MIRROR;
        }
        if( rtstate.is< RtPrim::Glass >() )
        {
            return RG_MESH_PRIMITIVE_GLASS;
        }

        RgMeshPrimitiveFlags add;
        switch( int( cvar::rt_wall_nomv ) )
        {
            case 0: add = 0; break;
            case 2: add = RG_MESH_PRIMITIVE_NO_MOTION_VECTORS; break;
            default:
                add = rtstate.is< RtPrim::NoMotionVectors >()
                          ? RG_MESH_PRIMITIVE_NO_MOTION_VECTORS
                          : 0;
                break;
        }
        
        bool alphaTest = mAlphaThreshold > 0;
        if( rt_mod_compat )
        {
            if( rtstate.is< RtPrim::ExportInstance >() )
            {
                // Soft blends under RT:
                //  - SAR2 / classic Fuzz spectre → IsSpectre() → rasterized
                //    TRANSLUCENT overlay (the see-through ghost look).
                //  - Any other LIVING soft-blend monster (64NightmareImp / TRO2) →
                //    same treatment, see IsLivingGhost() below.
                //  - Additive (DestAlpha One): fire/muzzle — TRANSLUCENT.
                //  - Other sprites (and the spectre/imp CORPSE): ALPHA_TESTED cutout.
                const bool additiveBlend =
                    mRenderStyle.BlendOp == STYLEOP_Add &&
                    mRenderStyle.DestAlpha == STYLEALPHA_One;

                if( additiveBlend )
                {
                    add |= RG_MESH_PRIMITIVE_TRANSLUCENT;
                    alphaTest = false;
                }
                else if( IsSpectre() || IsLivingGhost() )
                {
                    // Spectre: rasterized TRANSLUCENT overlay with alpha floor.
                    // Gives the purple-dark see-through look (like classic alpha blend).
                    //
                    // IsLivingGhost() extends this to 64NightmareImp, which used to
                    // fall through to the ALPHA_TESTED branch below. That was wrong for
                    // a "RenderStyle Translucent, Alpha 0.60" monster and it actively
                    // broke it: RsWorld.inl ends with
                    //
                    //     if( alphaTest != 0 ) { if( outColor.a < 0.5 ) discard; }
                    //
                    // and `discard` kills the WHOLE fragment — including
                    // outScreenEmission, so the emissive eyes died with the body. The
                    // imp only ever survived because the old max(a, 0.80) floor happened
                    // to hold it above 0.5; that floor's real job was clearing this
                    // threshold, not looks. Dropping the floor to the authored 0.60 left
                    // almost no margin, and once rt_ghost_lightscale dimmed a room past
                    // ~0.83x the imp did not fade, it POPPED OUT ENTIRELY. At
                    // rt_nightmareimp_alpha 0.35 it was invisible everywhere.
                    //
                    // As a TRANSLUCENT overlay there is no threshold to fall off: the
                    // body fades smoothly to nothing and the eyes, on an ONE/ONE
                    // attachment, are never discarded at any alpha (2026-08-09).
                    add |= RG_MESH_PRIMITIVE_TRANSLUCENT;
                    alphaTest = false;
                }
                else
                {
                    add |= RG_MESH_PRIMITIVE_ALPHA_TESTED;
                    alphaTest = true;
                }
            }
            // World: keep alpha-test for real masks (fences). Soft-alpha solids were
            // forced opaque at upload time via looksLikeRealMask heuristic.
        }

        // Doom64-RT: tell RTGL1 this is a BILLBOARD, so the sprite material
        // dials apply to it instead of the world ones (rt_sprite_pbr and
        // friends). isSpriteBillboard is already computed above for
        // rt_sprite_shadow and rt_sprite_ao -- this just lets the shader in on
        // it, which is what makes a sprite-only switch possible at all.
        // FirstPerson is in here deliberately. The viewmodel does NOT go through
        // the billboard branch -- it takes MakeFirstPersonQuadInWorldSpace and
        // leaves isSpriteBillboard false -- so flagging billboards alone left the
        // weapon in your hands as the one sprite rt_sprite_pbr could not switch
        // off. It is a camera-facing quad carrying a sprite's material like any
        // other, and it is the sprite on screen the most.
        if( isSpriteBillboard || rtstate.is< RtPrim::FirstPerson >() )
        {
            add |= RG_MESH_PRIMITIVE_SPRITE;
        }

        return ( alphaTest ? RG_MESH_PRIMITIVE_ALPHA_TESTED : 0 ) | add;
    };

    auto l_isemis = [ & ]() {
        if( mRenderStyle.BlendOp == STYLEOP_Add && mRenderStyle.DestAlpha == STYLEALPHA_One )
        {
            return true;
        }
        if( rt_mod_compat & 2 )
        {
            // Auto: brightmaps/glowmaps on sprites AND world geometry -> RT emissive
            if( rtstate.is< RtPrim::ExportInstance >() ||
                rtstate.is< RtPrim::ExportMap >() )
            {
                if( mBrightmapEnabled )
                {
                    if( mTextureModeFlags & TEXF_Glowmap )
                    {
                        if( mMaterial.mMaterial && mMaterial.mMaterial->sourcetex &&
                            mMaterial.mMaterial->sourcetex->Layers.get() &&
                            mMaterial.mMaterial->sourcetex->Layers->Glowmap.get() &&
                            mMaterial.mMaterial->sourcetex->Layers->Glowmap->GetSourceLump() >= 0 &&
                            mMaterial.mMaterial->sourcetex->Layers->Glowmap->GetWidth() > 0 &&
                            mMaterial.mMaterial->sourcetex->Layers->Glowmap->GetHeight() > 0 )
                        {
                            return true;
                        }
                    }

                    if( mTextureModeFlags & TEXF_Brightmap )
                    {
                        if( mMaterial.mMaterial && mMaterial.mMaterial->sourcetex &&
                            mMaterial.mMaterial->sourcetex->Brightmap.get() &&
                            mMaterial.mMaterial->sourcetex->Brightmap->GetSourceLump() >= 0 &&
                            mMaterial.mMaterial->sourcetex->Brightmap->GetWidth() > 0 &&
                            mMaterial.mMaterial->sourcetex->Brightmap->GetHeight() > 0 )
                        {
                            return true;
                        }
                    }
                }
            }
        }
        return false;
    };

    // Masked world geometry — fences, grates, the MAP01 cage — is alpha-TESTED,
    // not translucent: the texture's alpha cuts the holes, and the surface between
    // the holes is fully solid. But it arrives carrying a vertex alpha below 1, and
    // RTGL1 rasterizes any primitive whose packed vertex alpha is under
    // MESH_TRANSLUCENT_ALPHA_THRESHOLD (0.98, Const.h). A rasterized primitive is
    // never added to the acceleration structure at all, so it renders perfectly and
    // casts NOTHING — no shadow, at any light intensity, from any light.
    //
    // That is why the MAP01 fence cast no shadow while sprites and props in the same
    // room did: it was not in the BLAS to be hit. Forcing vertex alpha to 1 puts it
    // back in; the cutout still works, because RtAlphaTest.rahit tests the TEXTURE's
    // alpha per-texel and ignores intersections through the holes.
    //
    // Gated on mAlphaThreshold > 0 so only genuinely alpha-tested surfaces are
    // affected — real translucents (water, glass, additive FX) keep their alpha and
    // stay rasterized, which is correct for them (2026-08-08).
    const bool worldMaskedCutout =
        cvar::rt_force_mask_opaque && rt_mod_compat && !isUI && mAlphaThreshold > 0 &&
        !rtstate.is< RtPrim::ExportInstance >() && !rtstate.is< RtPrim::FirstPerson >() &&
        !rtstate.is< RtPrim::FirstPersonViewer >() && !rtstate.is< RtPrim::Sky >();

    // HACKHACK: replacements are ignored if a prim is rasterized, force alpha=1.0
    // Doom64-RT: always force opaque vertex color on world geometry under mod_compat.
    const bool forcealpha1 = ( mesh.flags & RG_MESH_FORCE_GLASS ) ||
                             ( mesh.flags & RG_MESH_FORCE_MIRROR ) ||
                             ( mesh.flags & RG_MESH_FORCE_WATER ) ||
                             ( rt_mod_compat && rtstate.is< RtPrim::ExportMap >() ) ||
                             worldMaskedCutout;

    // Doom64-RT: sector lightlevel / lightcolor must NOT bake into PT albedo.
    // Otherwise: yellow key-door sectors look neon-emissive, and lightlevel-0 rooms
    // get black vertex color so flashlight / ceiling lamps are absorbed.
    // IMPORTANT: doors/lifts are NOT ExportMap (movable) — still force white on them.
    // rt_world_white DECOUPLES THIS FROM rt_mod_compat. The two behaviours have
    // nothing to do with each other and only ever shared a cvar by accident:
    // rt_mod_compat also gates FLevelLocals::ReplaceTextures (p_sectors.cpp), so a
    // mod that retextures maps at runtime -- Doom 64: Unseen Evil -- MUST run
    // rt_mod_compat 0 to be visible at all, and that silently took this protection
    // with it. The result is the bug the comment above warns about, across a whole
    // level: Unseen Evil's Terraformer paints every sector from an authored palette
    // (9E321E, C85F4B, FF7E66 ...), all of which then baked into albedo and read as
    // a red glow with no light source anywhere in the room.
    //
    // Default off, so rt_mod_compat alone decides exactly as before and Retribution
    // is untouched; Unseen Evil pins it on in tools/d64ue-lighting.cfg.
    const bool forceWorldWhiteRgb =
        ( rt_mod_compat || bool{ cvar::rt_world_white } ) && !isUI &&
        !rtstate.is< RtPrim::ExportInstance >() &&
        !rtstate.is< RtPrim::FirstPerson >() &&
        !rtstate.is< RtPrim::FirstPersonViewer >() &&
        !rtstate.is< RtPrim::Sky >() &&
        !rtstate.is< RtPrim::SkyVisibility >() &&
        !rtstate.is< RtPrim::Particle >() &&
        !rtstate.is< RtPrim::Decal >();

    // Same bake issue on sprites / weapon: lightlevel-0 → black uVertexColor →
    // silhouette even after world white fix. Keep uObjectColor (ThingColor / weapon
    // ObjectColor / sector sprite tint); drop lightlevel from uVertexColor RGB.
    const bool forceSpriteUnlitAlbedo =
        rt_mod_compat && !isUI &&
        ( rtstate.is< RtPrim::ExportInstance >() ||
          rtstate.is< RtPrim::FirstPerson >() ||
          rtstate.is< RtPrim::FirstPersonViewer >() );

    auto l_spriteAlpha = [ &, this ]() -> float {
        if( forcealpha1 )
        {
            return 1.0f;
        }
        float a = mStreamData.uObjectColor.a * mStreamData.uVertexColor[ 3 ];
        // Dropping the TRANSLUCENT flag is only half of going solid: RTGL1 also
        // rasterizes anything whose packed vertex alpha is under
        // MESH_TRANSLUCENT_ALPHA_THRESHOLD (0.98). The spectre pulses down to 0.20
        // and the nightmare imp sits at a flat 0.60, and rt_translucent_minalpha
        // floors both around 0.72 — all well under the bar, so without this they
        // would still miss the BLAS and still take no light.
        if( IsSolidGhost() )
        {
            return 1.0f;
        }

        bool             ghostCorpse = false;
        const GhostActor ghost       = GhostSprite( &ghostCorpse );
        const bool       livingGhost = ( ghost != GhostActor::None ) && !ghostCorpse;

        if( livingGhost && ghost == GhostActor::Spectre )
        {
            // FORCE, don't cap. 64Spectre's DECORATE only lowers alpha in states that
            // call A_SetTranslucent, and its Spawn/Idle loop never does:
            //
            //     Spawn:  SAR2 A 0 A_SetTranslucent(1.0, 0)
            //             SAR2 BD 10 A_Look
            //             Goto Spawn+1          <- loops here, alpha stays 1.0
            //     See:    ... A_SetTranslucent(0.75) 0.50 0.25 0.20
            //
            // so an idle spectre sits at 1.0 while a chasing one sits at 0.20. The old
            // min(a, minalpha) only clipped the top, leaving idle at 0.80 — 4x more
            // opaque than the same monster once it wakes up. That is why an idle
            // spectre still read as lit in a pitch black room while a charging one
            // looked right: rt_ghost_lightscale was working, it was just scaling a body
            // 4x more opaque to begin with (2026-08-09).
            //
            // Forcing one value is what the old comment here already claimed to do
            // ("uniformly semi-transparent") but min() never actually did. Cost: the
            // Idle state's 0.25->1.0 alpha pulse is flattened out. That pulse is what
            // reads as "glowing on and off" under PT — set rt_spectre_alpha 0 to hand
            // control back to DECORATE if the shimmer is wanted.
            const float forced = float( cvar::rt_spectre_alpha );
            if( forced > 0.f )
            {
                a = std::min( forced, 1.f );
            }
        }
        else if( livingGhost )
        {
            // 64NightmareImp. Same treatment as the spectre above — one forced alpha,
            // and critically NOT run through the rt_translucent_minalpha floor further
            // down, because max(0.60, 0.80) would push it MORE opaque than DECORATE
            // asks for: the same too-visible-in-a-dark-room complaint by another route.
            //
            // Unlike the spectre there is no idle-vs-active discrepancy to repair here:
            // 64NightmareImp declares a flat "Alpha 0.60" and none of its states ever
            // call A_SetTranslucent, so Spawn and See already agree. The default is 0.35
            // — tuned by eye, below the authored 0.60, for the same reason the spectre
            // settled at 0.20: a faint enough sprite makes the imperfect light tracking
            // stop being visible (2026-08-09).
            const float forced = float( cvar::rt_nightmareimp_alpha );
            if( forced > 0.f )
            {
                a = std::min( forced, 1.f );
            }
        }
        else if( IsSpectre() )
        {
            // Classic Fuzz spectres (not SAR2): cap only, no authored alpha to respect.
            a = std::min( a, float( cvar::rt_translucent_minalpha ) );
        }
        else if( rt_mod_compat && rtstate.is< RtPrim::ExportInstance >() )
        {
            // Other soft-blend sprites: floor so they're not ghostly-clear.
            a = std::max( a, float( cvar::rt_translucent_minalpha ) );
        }
        // Applied AFTER the minalpha floor/cap on purpose: those pin how see-through
        // the ghost is at full light, this then fades that whole look out with the
        // room. Folding it in before would let minalpha clamp the darkness back off.
        return a * GhostLightScale();
    };

    // The map's bright surfaces become the emitters. primColor below already carries
    // the sector hue at full strength, so a red corridor panel emits red without any
    // extra colour plumbing here.
    auto l_worldemissive = [ & ]() -> float {
        if( l_isemis() )
        {
            return float{ cvar::rt_emis_additive_dflt };
        }
        if( !forceWorldWhiteRgb )
        {
            return 0.f;
        }

        const float strength = float{ cvar::rt_sector_emis };
        // Map-relative, not absolute — see RT_UpdateSectorEmisThreshold.
        const float minLight = g_sectorEmisThreshold;
        if( strength <= 0.f )
        {
            return 0.f;
        }

        // A threshold at or above 255 means THIS map has no light features: its own
        // median is already so high that nothing stands out above it. Bail out.
        //
        // This used to clamp minLight to 254 instead, which inverted the whole
        // feature on exactly the maps it was meant to protect. MAP09 and MAP31 are
        // authored fullbright (median lightlevel 255), so the threshold comes out at
        // 295 — "nothing emits" — but the clamp pulled it back to 254 and the ramp
        // below then evaluated (255-254)/(255-254) = 1.0, i.e. FULL strength, on
        // every one of MAP09's 74 lightlevel-255 sectors out of 81. The result was a
        // night courtyard where every wall, pillar and floor self-glowed evenly with
        // no shadow anywhere, while the imps and barrels standing in it stayed pitch
        // black — RTGL1 emissive is not a light source (see rt_wall_strips), so the
        // glow lit the surface it was on and nothing else. MAP07 (threshold 260) was
        // the same bug in milder form, 119 sectors of 429.
        if( minLight >= 255.f )
        {
            return 0.f;
        }

        const float ll = float( rtstate.m_sectorLightLevel );
        if( ll <= minLight )
        {
            return 0.f;
        }

        // rt_sector_emis was written for MAP02's red corridor panels -- a genuinely
        // COLOURED room, not merely a bright one -- but everything above this line only
        // ever looked at lightlevel. A plain white/grey wall in an equally bright room
        // (MAP03 sector 54 SPACEAP1, sector 70 SPACECL1; MAP04 sector 68 SPACEAF, all
        // reported via `whatsthat` 2026-08-21) qualified the exact same way and self-lit
        // with nothing in the world casting it -- it reads as a fake light bolted onto a
        // wall that is just bright, not as the room's actual light source the way MAP02's
        // panels do.
        //
        // primColor (below) tints this surface with the sector's colormap hue -- see
        // RT_SectorHue -- so that hue is also the right signal for whether this counts as
        // "a coloured room" at all: a default/near-white colormap has near-zero
        // saturation, MAP02's red fade does not. Gating on it keeps the coloured case and
        // drops the neutral one without a per-texture list, and needs no shader access to
        // the texture's own painted pixels, which this CPU-side function does not have.
        const FVector3& sc    = rtstate.m_sectorLightColor;
        const float     maxc  = std::max( { sc.X, sc.Y, sc.Z } );
        const float     minc  = std::min( { sc.X, sc.Y, sc.Z } );
        const float     satur = maxc > 1.e-4f ? ( maxc - minc ) / maxc : 0.f;
        if( satur < float{ cvar::rt_sector_emis_saturation } )
        {
            return 0.f;
        }

        // Ramp from the threshold to full bright so a lightlevel-200 panel glows
        // less than a lightlevel-255 one, instead of a hard on/off step.
        return ( ( ll - minLight ) / ( 255.f - minLight ) ) * strength;
    };

    RgColor4DPacked32 primColor;
    if( forceWorldWhiteRgb )
    {
        // Not white but hue-only: lightlevel stays dropped (that is what fixed the
        // black rooms), while the sector's colormap hue survives — see RT_SectorHue.
        //
        // Emissive world surfaces get the full light-strength hue, because under
        // this mod's launch config they ARE the room's light source: the launcher
        // runs rt_ceiling_lamps 0 / rt_sector_lights 0, so the glow in a MAP02
        // ceiling recess is texture emissive under rt_emis_mapboost, not an
        // analytic sphere. Tinting only the analytic lamps would have missed it.
        // Everything else gets the weak albedo strength — that is paint, not light.
        const float tintStrength = l_isemis() ? float{ cvar::rt_sector_tint_lights }
                                              : float{ cvar::rt_sector_tint_albedo };

        const FVector3 tint = RT_SectorHue( rtstate.m_sectorLightColor.X,
                                            rtstate.m_sectorLightColor.Y,
                                            rtstate.m_sectorLightColor.Z,
                                            tintStrength );
        primColor =
            rt.rgUtilPackColorFloat4D( tint.X, tint.Y, tint.Z, l_spriteAlpha() );
    }
    else if( forceSpriteUnlitAlbedo )
    {
        // Do NOT try to dim a spectre via the RGB here to stop it reading as self-lit
        // in a dark room. It was tried and it cannot work: RsWorld.inl builds its
        // emissive out of baseColor() too —
        //
        //     ldrEmis = baseColor().rgb * emisTex.rgb;   // then *= emissiveMult
        //
        // so scaling this colour down to darken the body scales the eye mask down by
        // exactly the same factor. Body and eyes are inseparable *in this channel*.
        //
        // The channel that does separate them is ALPHA, one stage later: the body
        // (attachment 0) blends SRC_ALPHA while outScreenEmission (attachment 1)
        // blends ONE,ONE and never sees alpha. That is what GhostLightScale() in
        // l_spriteAlpha() uses. The living ghost stays rasterized on purpose.
        primColor = rt.rgUtilPackColorFloat4D( mStreamData.uObjectColor.r,
                                              mStreamData.uObjectColor.g,
                                              mStreamData.uObjectColor.b,
                                              l_spriteAlpha() );
    }
    else
    {
        primColor = rtcolor_multiply(
            mStreamData.uObjectColor, mStreamData.uVertexColor, forcealpha1 );
    }

    // Doom64-RT: WHY DOES rt_spectre_alpha / rt_nightmareimp_alpha DO NOTHING?
    //
    // Reported 2026-08-14: rt_nightmareimp_alpha changes nothing at any value,
    // 1.0 included. That single symptom has at least four causes and the final
    // image cannot separate them -- the sprite never reaches the ghost branch
    // (name mismatch, not an ExportInstance, rt_mod_compat off), it reaches it
    // and the value is overwritten downstream, it is packed correctly and RTGL1
    // ignores it, or it is packed correctly and something else dominates what
    // you see. So this prints the chain rather than the outcome: what the
    // sprite is CALLED here, how GhostSprite() classified it, the alpha in,
    // the cvar, and the alpha actually PACKED into prim.color.
    //
    // Mode 2 prints EVERY sprite, not only the ones already classified as
    // ghosts. That is deliberate and it is the whole value of the instrument: a
    // filter built around the theory can only ever confirm it, and "no TRO2
    // line appeared" would be indistinguishable from "the imp is not on
    // screen". Mode 2 shows what the imp is actually called.
    if( cvar::rt_ghost_debug && !isUI && rtstate.is< RtPrim::ExportInstance >() )
    {
        bool             dbgCorpse = false;
        const GhostActor dbgGhost  = GhostSprite( &dbgCorpse );
        const char*      dbgName   = rtstate.get_exportinstance_name();

        if( int{ cvar::rt_ghost_debug } >= 2 || dbgGhost != GhostActor::None )
        {
            struct GhostRow
            {
                uint64_t lastPrint;
                int      count;
            };
            static std::unordered_map< std::string, GhostRow > s_ghost;
            static uint64_t                                    s_gframe;
            s_gframe++;

            auto& row = s_ghost[ dbgName ? dbgName : "(null)" ];
            row.count++;
            if( s_gframe - row.lastPrint > 60 )
            {
                row.lastPrint = s_gframe;

                const auto pf = makePrimFlags( isUI );
                Printf( "rt_ghost_debug %-6s class=%-12s corpse=%d living=%d "
                        "objA=%.3f vertA=%.3f cvar=%.3f lightscale=%.3f "
                        "packedA=%u/255 flags=0x%X%s%s modcompat=%d x%d\n",
                        dbgName ? dbgName : "(null)",
                        dbgGhost == GhostActor::Spectre        ? "Spectre"
                        : dbgGhost == GhostActor::NightmareImp ? "NightmareImp"
                                                               : "None",
                        int( dbgCorpse ),
                        int( IsLivingGhost() ),
                        mStreamData.uObjectColor.a,
                        mStreamData.uVertexColor[ 3 ],
                        dbgGhost == GhostActor::Spectre
                            ? float( cvar::rt_spectre_alpha )
                            : float( cvar::rt_nightmareimp_alpha ),
                        GhostLightScale(),
                        unsigned( uint32_t( primColor ) >> 24 ),
                        unsigned( pf ),
                        ( pf & RG_MESH_PRIMITIVE_TRANSLUCENT ) ? " TRANSLUCENT" : "",
                        ( pf & RG_MESH_PRIMITIVE_ALPHA_TESTED ) ? " ALPHA_TESTED" : "",
                        int( rt_mod_compat ),
                        row.count );
            }
        }
    }

    // Reports what RTGL1 will actually DO with each world primitive, not what we
    // hoped it would do. Two prior fixes were judged by eye from the final image and
    // both nulls were worthless, because "renders but casts nothing" and "casts but
    // the shadow is washed out" look identical there.
    //
    // RASTERIZED is the field that matters: RTGL1 keeps a primitive out of the
    // acceleration structure entirely when its vertex alpha is below 0.98 or it
    // carries TRANSLUCENT (VulkanDevice.cpp IsRasterized, Const.h:54), and geometry
    // outside the BLAS can never block a shadow ray. Printing alphaThr alongside it
    // shows whether rt_force_mask_opaque's gate even fired on this surface
    // (2026-08-08).
    // Answers, for one named surface, the questions this session kept guessing at:
    // WHICH file the texture came from (load-order / replacement wad actually in
    // use), WHICH animation frame is on screen right now, what lightlevel the
    // sector is feeding it, and whether the engine is making it self-emit.
    //
    // Prints one line per distinct texture name per second, so an 8-frame
    // animation prints as 8 rolling lines rather than a flood.
    if( !isUI && texname && *( cvar::rt_tex_probe ) )
    {
        const char* want = cvar::rt_tex_probe;
        const size_t wl  = strlen( want );
        if( wl > 0 && _strnicmp( texname, want, wl ) == 0 )
        {
            const char* srcfile = "?";
            int         srclump = -1;
            if( mMaterial.mMaterial && mMaterial.mMaterial->sourcetex )
            {
                srclump = mMaterial.mMaterial->sourcetex->GetSourceLump();
                if( srclump >= 0 )
                {
                    const int fnum = fileSystem.GetFileContainer( srclump );
                    if( fnum >= 0 )
                    {
                        srcfile = fileSystem.GetResourceFileName( fnum );
                    }
                }
            }

            struct ProbeRow
            {
                const char* file;
                int         lump;
                int         lightlevel;
                float       emis;
                uint32_t    color;
                int         count;
                uint64_t    lastPrint;
            };
            static std::unordered_map< std::string, ProbeRow > s_probe;
            static uint64_t                                    s_frame;
            s_frame++;

            auto& row = s_probe[ texname ];
            row.file       = srcfile;
            row.lump       = srclump;
            row.lightlevel = rtstate.m_sectorLightLevel;
            row.emis       = l_worldemissive();
            row.count++;
            // Packed RGBA8; printed raw rather than unpacked because RgInterface
            // exposes no unpack helper, only rgUtilPackColor*.
            row.color = uint32_t( primColor );

            // ~1s at 60fps, per NAME, so each animation frame reports in turn.
            if( s_frame - row.lastPrint > 60 )
            {
                row.lastPrint = s_frame;
                Printf( "rt_tex_probe %-10s file=%-24s lump=%-6d lightlevel=%3d "
                        "sector_emis=%.3f color=0x%08X drawn=%d\n",
                        texname,
                        row.file,
                        row.lump,
                        row.lightlevel,
                        row.emis,
                        row.color,
                        row.count );
            }
        }
    }

    if( cvar::rt_prim_debug && !isUI && texname )
    {
        const float vAlpha = mStreamData.uObjectColor.a * mStreamData.uVertexColor[ 3 ];
        const float sent   = forcealpha1 ? 1.0f : vAlpha;
        const auto  pf     = makePrimFlags( isUI );
        const bool  raster = ( pf & RG_MESH_PRIMITIVE_TRANSLUCENT ) || sent < 0.98f;

        struct PrimStat
        {
            float vAlpha, sent, alphaThr;
            uint32_t flags;
            bool raster, forced;
            int count;
        };
        static std::unordered_map< std::string, PrimStat > s_stats;
        static int                                         s_tick;

        auto& st = s_stats[ texname ];
        st = PrimStat{ vAlpha, sent, mAlphaThreshold, uint32_t( pf ),
                       raster, forcealpha1, st.count + 1 };

        if( ( ++s_tick % 600 ) == 0 )
        {
            Printf( "rt_prim_debug: %zu distinct world texture(s) this frame\n",
                    s_stats.size() );
            // Print EVERY world texture, not just the rasterized ones. The first
            // version filtered to `raster` only -- and the answer turned out to be
            // that the surface in question was NOT in that list, which the filter
            // made indistinguishable from it not being drawn at all. Same mistake
            // as the truncated nearby-texture dump (§14): a filtered instrument can
            // only ever confirm the theory it was built around.
            std::vector< std::pair< std::string, PrimStat > > rows( s_stats.begin(),
                                                                    s_stats.end() );
            std::sort( rows.begin(), rows.end(), []( const auto& a, const auto& b ) {
                return a.second.count > b.second.count;
            } );
            for( const auto& [ nm, s ] : rows )
            {
                Printf( "  %-10s x%-4d a=%.2f sent=%.2f thr=%.3f flags=0x%-5X "
                        "f1=%d  %s%s\n",
                        nm.c_str(), s.count, s.vAlpha, s.sent, s.alphaThr, s.flags,
                        int( s.forced ),
                        s.raster ? "RASTERIZED(no shadow)" : "in BLAS",
                        ( s.flags & RG_MESH_PRIMITIVE_ALPHA_TESTED ) ? " ALPHATESTED"
                                                                    : "" );
            }
            s_stats.clear();
        }
    }

    // Doom64-RT: tag the water flats engine-side rather than through the JSON
    // meta. RTGL only runs its water path on primitives carrying
    // RG_MESH_PRIMITIVE_WATER, and the JSON route (isWater in
    // rt/data/textures.json) proved impossible to verify from here: RTGL's
    // own messages are gated behind BOTH -rtdebug and its private
    // g_printSeverity, so a meta that silently failed to apply looks exactly
    // like a meta that applied and did nothing.
    //
    // Tagging here is also more durable. rt/data/textures.json lives under
    // build/ (gitignored) and is rewritten wholesale by the PBR tooling, so
    // the tag had to be re-applied by hand after every regen. A texture-name
    // match in the engine is under launcher control like rt_faux_lamps and
    // rt_solo_lamps already are, and survives everything.
    auto l_waterflag = [ & ]() -> RgMeshPrimitiveFlags {
        if( !cvar::rt_water_style || isUI || !texname )
        {
            return RgMeshPrimitiveFlags( 0 );
        }
        // PREFIX match, and that is the whole point. D64W2_01 is not a
        // texture, it is frame 1 of a 64-frame ANIMDEFS sequence
        // (D64W2_01..D64W2_64, 2 tics each); the map's sector names frame 1
        // but GZDoom swaps in a different frame every 2 tics, so the name
        // that actually reaches RTGL is almost never "D64W2_01".
        //
        // An exact match therefore tagged the water for 2 tics out of ~128 —
        // the water became water for one frame per ~3.7s cycle and reverted,
        // which is exactly the "regular bright flash" this was reported as.
        // RTGL's own GeomInfoManager says the same thing: "can't use texture
        // / mesh name, as texture can be just 1 frame of animation sequence".
        //
        // D64WATR1/2 and friends are the 192x192 source patches (warp2 in
        // ANIMDEFS), kept in case a map places one directly.
        //
        // All FOUR Doom 64 liquids run through here, not just water. They
        // are the same animated flat design in four palettes, and every one
        // of them has the same problem the water had: near-black under a
        // path tracer, opaque floor flat with nothing to refract into. The
        // liquid id (2 bits) picks the body/crest colour in the shader:
        //
        //   0 water   D64W1_/D64W2_, D64WATR1/2   dark blue
        //   1 nukage  D64N1_/D64N2_, D64NUKG1/2   green
        //   2 sludge  D64S1_/D64S2_, D64SLDG1/2   brown / dark yellow
        //   3 blood   D64B1_/D64B2_, D64BLOD1/2   dark red
        //
        // FLOOR FLATS ONLY. WFALL/SFALL/BFALL — the WALL sheets, and 64-frame
        // sequences just like the rest ("WATER FALL" / "SLIME FALL" /
        // "BLOOD FALL" in ANIMDEFS) — were tagged too for one revision and
        // are deliberately not any more.
        //
        // The reason is structural, not a tuning failure. RG_MESH_PRIMITIVE_WATER
        // makes the primitive refractive, and ASManager rewrites a refractive
        // instance's TLAS mask to INSTANCE_MASK_REFRACT *only*, dropping every
        // INSTANCE_MASK_WORLD_* bit. On a floor that is survivable: a pool does
        // not shadow much. On a wall it is not — the fall stops blocking shadow
        // rays, so light pours through the solid line it is painted on, and the
        // stylized surface reflects it back. Reported from MAP10 as the falls
        // leaking light and reflections, which is exactly what the mask says
        // should happen.
        //
        // Fixing it properly means keeping the world bits on a refractive
        // instance, i.e. an ASManager change with consequences for glass and
        // stock water. Not worth it for 45 placements across three maps.
        // Their frame-01-only material overlays stay quarantined either way —
        // that is a separate defect (see tools/set_water_meta.py).
        struct LiquidMatch
        {
            const char* name;  // prefix, or full name when exact
            bool        exact;
            int         id;    // 0 water, 1 nukage, 2 sludge, 3 blood
        };
        static const LiquidMatch kLiquids[] = {
            { "D64W1_", false, 0 },   { "D64W2_", false, 0 },
            { "D64WATR1", true, 0 },  { "D64WATR2", true, 0 },
            { "D64N1_", false, 1 },   { "D64N2_", false, 1 },
            { "D64NUKG1", true, 1 },  { "D64NUKG2", true, 1 },
            { "D64S1_", false, 2 },   { "D64S2_", false, 2 },
            { "D64SLDG1", true, 2 },  { "D64SLDG2", true, 2 },
            { "D64B1_", false, 3 },   { "D64B2_", false, 3 },
            { "D64BLOD1", true, 3 },  { "D64BLOD2", true, 3 },

            // Doom 64: Unseen Evil's liquids. Same four-palette design, different
            // names, so the treatment above did nothing on a DOOM 1/2 map and its
            // pools stayed opaque and near-black -- the exact problem this table
            // exists to solve.
            //
            // The ids are NOT guessed from the names; they come from the mod's own
            // replacement table (resources/d64ue_textures.floors), which is what
            // decides which stock liquid becomes which of its flats:
            //
            //   FWATER1..4  -> WATERA    water
            //   NUKAGE1..3  -> SLIMEB    nukage  (Doom's green nukage)
            //   SLIME01..08 -> SLUDGEA/B sludge  (NOT nukage, despite the name)
            //   BLOOD1..3   -> BLOODB    blood
            //
            // That SLIME/SLUDGE crossover is the trap: reading "SLIMEA" as sludge
            // would swap green for brown on every nukage pool in DOOM II.
            //
            // EXACT, not prefix. These are single flats rather than 64-frame
            // sequences, so there is no frame suffix to match past, and a prefix
            // on a name this short would over-reach ("BLOOD" would also catch the
            // mod's BLOODFALL wall sheets, which must stay opaque for the
            // shadow-leak reason given above).
            //
            // textures/special/{WATERS,SLIMES,BLOODS}.png are the mod's swimmable
            // surfaces and are named by full path when placed directly.
            { "WATERA", true, 0 },  { "WATERB", true, 0 },
            { "SLIMEA", true, 1 },  { "SLIMEB", true, 1 },
            { "SLUDGEA", true, 2 }, { "SLUDGEB", true, 2 },
            { "BLOODA", true, 3 },  { "BLOODB", true, 3 },
            { "textures/special/WATERS.png", true, 0 },
            { "textures/special/SLIMES.png", true, 1 },
            { "textures/special/BLOODS.png", true, 3 },
        };
        static const char* const kLiquidName[] = { "water", "nukage", "sludge", "blood" };

        for( const LiquidMatch& m : kLiquids )
        {
            const bool hit = m.exact ? ( strcmp( texname, m.name ) == 0 )
                                     : ( strncmp( texname, m.name, strlen( m.name ) ) == 0 );
            if( !hit )
            {
                continue;
            }
            if( m.id != 0 && !cvar::rt_water_liquids )
            {
                return RgMeshPrimitiveFlags( 0 );
            }

            // One line per distinct frame name per session. Printf is
            // gzdoom's own, so it is NOT subject to RTGL's message gates.
            static std::unordered_set< std::string > s_seen;
            if( s_seen.insert( texname ).second )
            {
                Printf( RT_DiagPrintLevel(),
                        "RT water: tagging \"%s\" as RG_MESH_PRIMITIVE_WATER, "
                        "liquid %d (%s)\n",
                        texname,
                        m.id,
                        kLiquidName[ m.id ] );
            }

            return RgMeshPrimitiveFlags(
                RG_MESH_PRIMITIVE_WATER |
                ( m.id & 1 ? RG_MESH_PRIMITIVE_LIQUID_BIT0 : 0 ) |
                ( m.id & 2 ? RG_MESH_PRIMITIVE_LIQUID_BIT1 : 0 ) );
        }
        return RgMeshPrimitiveFlags( 0 );
    };

    // Sprites do not RECEIVE projected water caustics. A caustic is light
    // thrown across a surface; a camera-facing billboard has no surface for
    // it to lie on, so the pattern just tints the enemy or the weapon and
    // swims about as the camera turns. Decals and particles likewise.
    auto l_nocausticsflag = [ & ]() -> RgMeshPrimitiveFlags {
        if( isUI || rtstate.is< RtPrim::ExportInstance >() ||
            rtstate.is< RtPrim::FirstPerson >() ||
            rtstate.is< RtPrim::FirstPersonViewer >() ||
            rtstate.is< RtPrim::Particle >() || rtstate.is< RtPrim::Decal >() ||
            rtstate.is< RtPrim::Sky >() || rtstate.is< RtPrim::SkyVisibility >() )
        {
            return RG_MESH_PRIMITIVE_NO_WATER_CAUSTICS;
        }
        return RgMeshPrimitiveFlags( 0 );
    };

    // Doom64-RT: mark lava so the shader can boost and animate its emission.
    // Same prefix match as the lights, for the same reason -- HLAVA1 is one
    // frame of a 5-frame ping-pong and the name reaching RTGL is rarely the
    // one on the sector.
    auto l_lavaflag = [ & ]() -> RgMeshPrimitiveFlags {
        if( isUI || !texname || !RT_IsLavaFlat( texname ) )
        {
            return RgMeshPrimitiveFlags( 0 );
        }
        static std::unordered_set< std::string > s_seenLava;
        if( s_seenLava.insert( texname ).second )
        {
            Printf( RT_DiagPrintLevel(),
                    "RT lava: tagging \"%s\" as RG_MESH_PRIMITIVE_LAVA "
                    "(rt_lava_emis %.1f)\n",
                    texname,
                    float{ cvar::rt_lava_emis } );
        }
        return RG_MESH_PRIMITIVE_LAVA;
    };

    // A lamp pane that got real bulb lights keeps only a RESIDUE of its painted glow:
    // enough for the bulbs to read as lit and to bloom, not enough to light the room a
    // second time. Full suppression was tried and the bulbs went dead flat -- correct
    // arithmetic, wrong picture, because a real lamp IS a bright thing to look at.
    //
    // The flag is what makes the number survive: TextureMeta overwrites the primitive's
    // emissive with the material's a moment later unless it is told not to.
    auto l_lampglow = [ & ]() -> std::pair< RgMeshPrimitiveFlags, float > {
        if( isUI || !cvar::rt_ceiling_bulb_noemis || !rtstate.is< RtPrim::LatticeLitFlat >() )
        {
            const float we = l_worldemissive();

            // SECTOR SELF-EMISSION HAS TO CLAIM THE SAME FLAG, or a texture that
            // merely OWNS a textures.json entry loses it.
            //
            // TextureMeta.cpp does, unless the flag is set:
            //     prim.emissive = std::max( 0.0f, meta->emissiveMult );
            // and JsonParser.h defaults emissiveMult to 0.0f. So an entry that says
            // nothing whatsoever about emission -- a metallicDefault/roughnessDefault
            // PBR label, say -- still overwrites whatever we computed here with ZERO.
            //
            // rt_sector_emis was therefore resting on the ABSENCE of a meta entry.
            // The metal/roughness labelling passes (e1c8944, then 8976584 across 898
            // textures) removed that absence, and every painted light feature sitting
            // on a labelled texture went dark at once -- 2091 of 3015 entries carry no
            // emissiveMult. MAP02's red corridor panels are the case rt_sector_emis
            // was written for and they are in that set.
            //
            // It is silent by construction: gzdoom computes and sends the right
            // number, so `whatsthat` still says SELF-EMITS and rt_tex_probe still
            // prints sector_emis=0.350. Both are telling the truth; the value simply
            // does not survive the next call. Do not trust either as evidence that
            // the emission reached the screen.
            //
            // ONLY the sector ramp is claimed. l_worldemissive() also returns
            // rt_emis_additive_dflt for l_isemis() surfaces, and those MUST keep
            // deferring to the material -- an SMON panel's authored emissiveMult 2.8
            // would otherwise be replaced by 0.15 and the monitors would go dim. That
            // is the same "one owner per value" mistake in the other direction.
            const bool fromSectorRamp = !l_isemis() && forceWorldWhiteRgb && we > 0.f;

            if( fromSectorRamp && bool{ cvar::rt_sector_emis_override } )
            {
                return { RG_MESH_PRIMITIVE_EMISSIVE_OVERRIDE, we };
            }
            return { RgMeshPrimitiveFlags( 0 ), we };
        }
        return { RG_MESH_PRIMITIVE_EMISSIVE_OVERRIDE,
                 std::max( 0.f, float{ cvar::rt_ceiling_bulb_emis } ) };
    };
    const auto [ lampGlowFlag, lampGlowEmis ] = l_lampglow();

    // Doom64-RT: does this draw get shadow proxies? ONE decision, used twice --
    // here to silence the visible billboard, and at the bottom to emit them.
    // They must agree: hiding the caster for a sprite that then gets no proxy
    // deletes its shadow outright, which is how a feature meant to ADD shadows
    // ends up removing them.
    const bool emitShadowProxies = [ & ]() -> bool {
        if( !cvar::rt_sprite_shadow || isUI || !isSpriteBillboard || verts.empty() )
        {
            return false;
        }
        // Not the player's own body -- see the note at the emission site.
        if( rtstate.is< RtPrim::FirstPerson >() || rtstate.is< RtPrim::FirstPersonViewer >() )
        {
            return false;
        }
        // WHICH THINGS GET A PROXY AT ALL -- rt_sprite_shadow_scope.
        //
        // A proxy is an assumption about SHAPE, and a vertical cross only fits
        // something standing up. Through a corpse, a gib or a dropped weapon it
        // becomes four cards on the ground throwing radiating spokes
        // (screen/shadowissue.png), so those keep their stock billboard shadow,
        // exactly as before this feature existed.
        //
        // scope 1 goes further and asks for a LIVE MONSTER: everything the
        // proxies were built for is there, and every class the assumption fits
        // badly is outside it by construction.
        if( cvar::rt_sprite_shadow_scope >= 1 ? !rtstate.m_lastthinglivemonster
                                              : !rtstate.m_lastthingupright )
        {
            return false;
        }
        // A translucent sprite is RASTERIZED by RTGL1 -- never in the
        // acceleration structure, so it casts nothing today and giving it a
        // solid shadow would be a change in look, not a fix. That is the
        // spectre, the nightmare imp and every additive fire/muzzle sprite.
        if( makePrimFlags( isUI ) & RG_MESH_PRIMITIVE_TRANSLUCENT )
        {
            return false;
        }
        // Distance cap. The cost is per visible actor per frame, and a shadow
        // 40 m away is a few pixels. Metres, to match the pivot.
        const float maxdist = std::max( 0.f, float{ cvar::rt_sprite_shadow_dist } );
        if( maxdist > 0.f )
        {
            const float dx =
                transform.matrix[ 0 ][ 3 ] - float( r_viewpoint.Pos.X ) * ONEGAMEUNIT_IN_METERS;
            const float dy =
                transform.matrix[ 1 ][ 3 ] - float( r_viewpoint.Pos.Y ) * ONEGAMEUNIT_IN_METERS;
            const float dz =
                transform.matrix[ 2 ][ 3 ] - float( r_viewpoint.Pos.Z ) * ONEGAMEUNIT_IN_METERS;
            if( std::sqrt( dx * dx + dy * dy + dz * dz ) > maxdist )
            {
                return false;
            }
        }
        return true;
    }();

    // When proxies do the occluding, the visible billboard must stop, or the
    // umbra is the UNION of the camera-facing quad and the world-fixed ones:
    // fatter than the sprite and still changing shape as the player turns, i.e.
    // the artefact the proxies exist to remove, merely diluted.
    auto l_spriteshadowcaster = [ & ]() -> RgMeshPrimitiveFlags {
        if( emitShadowProxies && cvar::rt_sprite_shadow_hidecaster )
        {
            return RG_MESH_PRIMITIVE_NO_SHADOW;
        }
        return RgMeshPrimitiveFlags( 0 );
    };

    // Doom64-RT: WHY rt_nightmareimp_alpha / rt_spectre_alpha DO NOTHING.
    //
    // A living ghost's alpha is packed correctly all the way into prim.color --
    // measured, 0.42 -> packedA 89/255 and 1.00 -> 214/255 -- and the image does
    // not move. The alpha is not the problem: the BLEND MODE is.
    //
    // These sprites carry an eye mask, so textures.json gives them
    // "emissiveMult": 2.0, and RTGL1's TextureMeta.cpp overwrites prim.emissive
    // with it unless the caller sets EMISSIVE_OVERRIDE. RasterizedDataCollector's
    // ToPipelineState then reads:
    //
    //     if( emissive > eps ) if( TRANSLUCENT ) -> ADDITIVE
    //     ADDITIVE: blendSrc = SRC_ALPHA, blendDst = ONE
    //
    // so the whole body draws ADDITIVELY. Under additive blending alpha only
    // scales how much the sprite ADDS -- it can never make the body occlude what
    // is behind it, so no value can read as "more solid", and on art as dark as
    // the nightmare imp's the difference between 0.35 and 0.84 of a dark colour
    // added to a lit room is close to invisible. That is the reported symptom
    // exactly, and it applies to the spectre for the same reason.
    //
    // Two ways out of that, and they are NOT alternatives -- the second is the
    // shipping one and the first is now only a probe:
    //
    //   rt_ghost_emis 0        forces emissive to 0 with EMISSIVE_OVERRIDE so
    //                          the material cannot put it back. Alpha works;
    //                          the glowing EYES go dark with it, because
    //                          prim.emissive is also what multiplies
    //                          outScreenEmission. Kept as the one-flag probe
    //                          that proved the diagnosis.
    //   rt_ghost_emis_split 1  the body drops its emissive exactly as above, and
    //                          the eye mask is re-uploaded on a SECOND primitive
    //                          whose alpha is zero (see the upload below). Both
    //                          halves, no RTGL1 change.
    const bool livingGhostLike = IsSpectre() || IsLivingGhost();
    // The body carries no emissive in either mode -- that is what buys the
    // alpha blending. The two modes differ only in whether anything else
    // carries the eyes.
    const bool ghostNoEmis =
        livingGhostLike && ( !cvar::rt_ghost_emis || cvar::rt_ghost_emis_split );
    const bool ghostEyeCopy =
        livingGhostLike && cvar::rt_ghost_emis && cvar::rt_ghost_emis_split;

    auto prim = RgMeshPrimitiveInfo{
        .sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
        .pNext = isUI ? &ui : nullptr,
        .flags = makePrimFlags( isUI ) | l_waterflag() | l_nocausticsflag() | l_lavaflag() |
                 lampGlowFlag | l_spriteshadowcaster() |
                 ( ghostNoEmis ? RG_MESH_PRIMITIVE_EMISSIVE_OVERRIDE
                               : RgMeshPrimitiveFlags( 0 ) ) |
                 RG_MESH_PRIMITIVE_FORCE_EXACT_NORMALS |
                 ( rtstate.is< RtPrim::ExportInvertNormals >()
                       ? RG_MESH_PRIMITIVE_EXPORT_INVERT_NORMALS
                       : 0 ),
        .primitiveIndexInMesh = rtstate.next_primitiveindex(),
        .pVertices            = verts.data(),
        .vertexCount          = static_cast< uint32_t >( verts.size() ),
        .pIndices             = indices.empty() ? nullptr : indices.data(),
        .indexCount           = static_cast< uint32_t >( indices.size() ),
        .pTextureName         = texname,
        .textureFrame         = 0,
        .color        = primColor,
        .emissive     = ghostNoEmis ? 0.f : lampGlowEmis,
        .classicLight = lightlevel_to_classic( isUI, mLightParms[ 3 ] ),
    };

#ifndef NDEBUG
    if( cvar::_rt_showexportable )
    {
        if( !rtstate.is< RtPrim::ExportMap >() && !isUI )
        {
            return;
        }
    }
#endif

    // Why this exists: the plasma rifle goes half see-through while firing and no
    // other weapon does. The sprite's own alpha is binary (0/255, measured on every
    // frame) and RsWorld.inl's alpha test is a discard, so neither can produce a
    // PARTIAL fade — that needs alpha BLENDING, which means the quad is being
    // rasterized as translucent. RTGL1 rasterizes anything whose packed vertex alpha
    // is under MESH_TRANSLUCENT_ALPHA_THRESHOLD (0.98), so the question is simply
    // what the alpha chain below evaluates to on the fire frames. Print it.
    if( cvar::rt_wpn_debug &&
        ( rtstate.is< RtPrim::FirstPerson >() || rtstate.is< RtPrim::FirstPersonViewer >() ) )
    {
        // Also print what decides whether an rt/replace/*.gltf model can stand in for
        // this quad. RTGL1's Scene::UploadPrimitive only looks for a replacement when
        // isExportable is set, pMeshName is non-empty AND RG_MESH_EXPORT_AS_SEPARATE_FILE
        // is on the mesh; the lookup key is then pMeshName verbatim, which must match a
        // glTF NODE name character for character ("SHTG" + 'A'+frame). Printing the key
        // is the whole diagnostic: name matches a node + all three conditions true means
        // the lookup hits, and no RTGL1 rebuild is needed to tell.
        const char* meshname = mesh.pMeshName ? mesh.pMeshName : "(null)";
        const bool  canreplace =
            mesh.isExportable && mesh.pMeshName && mesh.pMeshName[ 0 ] &&
            ( mesh.flags & RG_MESH_EXPORT_AS_SEPARATE_FILE );

        // Dedup on mesh name+texture+flags+quantised alpha so a held trigger does not
        // spam. The mesh name is in the key because one weapon's layers differ in it:
        // a model/voxel psprite layer gets a null name while its sprite layers do not,
        // and collapsing those would hide exactly what we are looking for.
        static std::unordered_set< uint64_t > s_seen;
        const uint32_t                        a8 = uint32_t( primColor >> 24 );
        const uint64_t key = ( uint64_t( prim.flags ) << 32 ) ^
                             ( uint64_t( a8 ) << 24 ) ^
                             std::hash< std::string_view >{}( texname ? texname : "" ) ^
                             ( std::hash< std::string_view >{}( meshname ) * 31 ) ^
                             ( uint64_t( canreplace ) << 40 );
        if( s_seen.insert( key ).second )
        {
            Printf( "RTWPN %s mesh=\"%s\" exportable=%d sepfile=%d replaceable=%d "
                    "flags=0x%X alphaThresh=%.2f objA=%.3f vertA=%.3f "
                    "packedA=%u emis=%.3f blendop=%d destalpha=%d\n",
                    texname ? texname : "?",
                    meshname,
                    int( mesh.isExportable ),
                    int( ( mesh.flags & RG_MESH_EXPORT_AS_SEPARATE_FILE ) != 0 ),
                    int( canreplace ),
                    unsigned( prim.flags ),
                    mAlphaThreshold,
                    mStreamData.uObjectColor.a,
                    mStreamData.uVertexColor[ 3 ],
                    a8,
                    prim.emissive,
                    int( mRenderStyle.BlendOp ),
                    int( mRenderStyle.DestAlpha ) );
        }
    }

    RgResult r = rt.rgUploadMeshPrimitive( &mesh, &prim );
    RG_CHECK( r );

    // Doom64-RT: THE GHOST'S EYES, ON THEIR OWN PRIMITIVE -- rt_ghost_emis_split.
    //
    // The problem this solves is the one above: a living ghost's eye mask gives
    // it emissiveMult 2.0, RTGL1 reads that as "emissive + translucent =
    // ADDITIVE", and an additive body cannot occlude anything at any alpha, so
    // rt_nightmareimp_alpha and rt_spectre_alpha are inert. Zeroing the emissive
    // (rt_ghost_emis 0) restores real alpha blending but takes the glowing eyes
    // with it, which is a bad trade -- the eyes are the better half.
    //
    // Both are available because the two live on DIFFERENT ATTACHMENTS with
    // different blend factors (RasterizerPipelines.cpp): the body is attachment
    // 0 at SRC_ALPHA, and outScreenEmission is attachment 1 at ONE,ONE, which
    // never looks at alpha at all. So the emission does not need this
    // primitive's alpha, and the body does not need this primitive's emissive.
    // Split them:
    //
    //   BODY  -- emissive forced to 0 with EMISSIVE_OVERRIDE (above), so the
    //            material cannot put it back and the pipeline stays plain
    //            SRC_ALPHA / ONE_MINUS_SRC_ALPHA. The alpha cvars now mean what
    //            they say, and a ghost can genuinely occlude the wall behind it.
    //   EYES  -- the same quad again with its packed alpha set to ZERO, and the
    //            emissive left alone so TextureMeta fills in emissiveMult. Its
    //            contribution to attachment 0 is `SRC_ALPHA x colour` = exactly
    //            nothing, so it adds no body; its contribution to attachment 1
    //            is the full eye mask, because that attachment ignores alpha.
    //
    // Cost is one extra rasterized quad per visible ghost per frame, with no
    // texture work and no geometry work -- it is the same vertices under the
    // same transform, which is the same trick the shadow proxies below use.
    // Order does not matter: neither attachment reads the other, and translucent
    // primitives do not write depth.
    if( ghostEyeCopy )
    {
        auto eyeprim = prim;
        // A distinct index within the mesh, or RTGL1 sees two primitives with the
        // same ID and drops one -- silently, and the copy is the one that loses.
        eyeprim.primitiveIndexInMesh = rtstate.next_primitiveindex();
        // Packed RGBA8, alpha in the top byte. Masking it off leaves the colour
        // (which the emission is multiplied by, so the eyes keep their hue and
        // the sector tint) and removes the body.
        eyeprim.color = RgColor4DPacked32( uint32_t( primColor ) & 0x00FFFFFFu );
        // Hand the emissive back to the material. Dropping EMISSIVE_OVERRIDE is
        // the whole point: TextureMeta writes emissiveMult here, which is what
        // lights the mask.
        eyeprim.flags &= ~RgMeshPrimitiveFlags( RG_MESH_PRIMITIVE_EMISSIVE_OVERRIDE );
        eyeprim.flags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
        eyeprim.emissive = lampGlowEmis;
        // It contributes no body, so it must not contribute a shadow either --
        // the real body primitive is the caster (or the proxies are).
        eyeprim.flags |= RG_MESH_PRIMITIVE_NO_SHADOW;

        RgResult re = rt.rgUploadMeshPrimitive( &mesh, &eyeprim );
        RG_CHECK( re );
    }

    // ---------------------------------------------------------------------
    // Doom64-RT: SPRITE SHADOW PROXIES (rt_sprite_shadow).
    //
    // A sprite is a camera-facing quad with no thickness, so its shadow is the
    // projection of a plane: when the light lies IN that plane the shadow
    // collapses to a line, and because the quad turns to face the viewer, the
    // shadow's shape changes as the player rotates. Both are visible in play --
    // an enemy lit from the side casts nothing, and turning on the spot makes a
    // shadow breathe.
    //
    // The fix is invisible crossed quads at FIXED WORLD ANGLES, flagged
    // RG_MESH_PRIMITIVE_SHADOW_ONLY so they occlude shadow rays and nothing
    // else. With two planes at 90 degrees no light direction is degenerate: as
    // one goes edge-on the other is face-on. Being world-fixed, they also stop
    // the shadow changing with the camera.
    //
    // This is cheap here only because CalculateTrueTransformAndItsVerts has
    // already factored the billboard into (rotation, pivot) + un-rotated local
    // verts -- so a proxy is the SAME vertices under a different transform, with
    // no vertex maths, no second copy of the geometry and no texture work. The
    // alpha test comes along in the flags, so the shadow keeps the sprite's
    // silhouette rather than being a rectangle.
    // NOT the player's own body. It is a FirstPersonViewer sprite that is ALSO an
    // ExportInstance, so it takes the true-transform path like any actor and
    // isSpriteBillboard is true for it -- but the proxy is submitted with cleared
    // mesh flags, which drops RG_MESH_FIRST_PERSON_VIEWER and turns it into an
    // ordinary WORLD occluder standing at the camera. The flashlight is a light
    // at ~0 m from that point, so it lit the proxies edge-on and threw swinging
    // corner shadows across the whole beam (reported 2026-08-13).
    //
    // Keeping the viewer flag instead would not work: PV_FIRST_PERSON_VIEWER is
    // tested BEFORE PV_SHADOW_ONLY when the instance mask is chosen, so the proxy
    // would become visible geometry hanging in front of the camera. The player
    // already casts through its own viewer sprite, so there is nothing to regain.
    if( emitShadowProxies )
    {
        const float px = transform.matrix[ 0 ][ 3 ];
        const float py = transform.matrix[ 1 ][ 3 ];
        const float pz = transform.matrix[ 2 ][ 3 ];

        {
            const int planes = std::clamp( int{ cvar::rt_sprite_shadow_planes }, 1, 4 );

            // Narrow the proxies to the ACTOR, not to the sprite's canvas.
            //
            // A sprite quad is the art's bounding box: a marine is drawn on ~64
            // units of quad around a body radius of 17. A proxy built at the
            // quad's width therefore reaches about twice as far toward the
            // viewer as the actor physically does -- and the flashlight, which
            // sits at the eye and is tipped down, throws that overhang onto the
            // floor IN FRONT of the sprite, where a real body's shadow could
            // never fall (reported 2026-08-13). Scaling the horizontal axis to
            // the collision radius puts the occluder back inside the actor.
            //
            // Local Y is the sprite's width axis: CalculateTrueTransformAndItsVerts
            // leaves local X as the facing normal and Z as up, so scaling Y alone
            // narrows the silhouette without touching its height or its footing
            // on the floor. The alpha mask is untouched, so the shadow stays the
            // sprite's outline, merely squeezed to body width.
            // CANONICALISE THE QUAD FIRST, and this is the correction that makes
            // everything below mean what it says.
            //
            // The local verts are NOT axis-aligned. hw_sprites.cpp hands
            // push_apply_spriterotation the ACTOR'S OWN yaw (rtangles.Yaw), so
            // CalculateTrueTransformAndItsVerts un-rotates by the actor's
            // facing, not by the billboard's -- and the quad keeps its
            // camera-facing orientation baked in, offset by
            // (camera_yaw - actor_yaw).
            //
            // Everything that followed inherited that error: a proxy sat at
            // yaw_k + that offset, so the "world-fixed" planes ROTATED WITH THE
            // CAMERA; local Y was not the width axis, so rt_sprite_shadow_width
            // scaled the wrong one; and the mirror test's normal was off by the
            // same angle -- worst when the actor is side-on to the viewer, which
            // is exactly where the mirrored shadow survived (2026-08-13).
            //
            // So derive the quad's own frame from its vertices rather than
            // assuming one: take its normal, rotate the verts about local Z
            // until that normal is +X, and the quad is then canonical -- normal
            // +X, width along Y, upright along Z. A rigid rotation cannot mirror
            // it, so the texture's handedness relative to the normal survives.
            const auto l_localnormal = [ & ]() -> FVector3 {
                if( verts.size() < 3 )
                {
                    return {};
                }
                const FVector3 p0{ verts[ 0 ].position[ 0 ],
                                   verts[ 0 ].position[ 1 ],
                                   verts[ 0 ].position[ 2 ] };
                const FVector3 p1{ verts[ 1 ].position[ 0 ],
                                   verts[ 1 ].position[ 1 ],
                                   verts[ 1 ].position[ 2 ] };
                const FVector3 p2{ verts[ 2 ].position[ 0 ],
                                   verts[ 2 ].position[ 1 ],
                                   verts[ 2 ].position[ 2 ] };
                return ( p1 - p0 ) ^ ( p2 - p0 );
            };

            FVector3 nloc = l_localnormal();

            // Winding is not guaranteed, so pick the side the CAMERA is on --
            // the billboard faces the viewer, so that is its textured face.
            // transform's 3x3 takes local to world.
            const float nw_x = transform.matrix[ 0 ][ 0 ] * nloc.X +
                               transform.matrix[ 0 ][ 1 ] * nloc.Y +
                               transform.matrix[ 0 ][ 2 ] * nloc.Z;
            const float nw_y = transform.matrix[ 1 ][ 0 ] * nloc.X +
                               transform.matrix[ 1 ][ 1 ] * nloc.Y +
                               transform.matrix[ 1 ][ 2 ] * nloc.Z;
            if( nw_x * ( float( r_viewpoint.Pos.X ) * ONEGAMEUNIT_IN_METERS - px ) +
                    nw_y * ( float( r_viewpoint.Pos.Y ) * ONEGAMEUNIT_IN_METERS - py ) <
                0.f )
            {
                nloc = -nloc;
            }

            const float nlen = std::sqrt( nloc.X * nloc.X + nloc.Y * nloc.Y );
            if( nlen < 1e-6f )
            {
                // A quad with no horizontal facing at all -- nothing sane to
                // build a vertical proxy from. Leave it on its billboard.
                return;
            }

            // Rotate local Z by -atan2(n) so the normal lands on +X.
            const float ca = nloc.X / nlen;
            const float sa = -nloc.Y / nlen;

            float halfw = 0.f;
            {
                for( const auto& v : verts )
                {
                    const float y = v.position[ 0 ] * sa + v.position[ 1 ] * ca;
                    halfw         = std::max( halfw, std::fabs( y ) );
                }
            }

            const float wantHalf = rtstate.m_lastthingradius * ONEGAMEUNIT_IN_METERS *
                                   std::max( 0.f, float{ cvar::rt_sprite_shadow_width } );

            // Only ever narrows: a proxy wider than its own art would cast a
            // shadow the sprite cannot account for. 0 (or an unknown radius)
            // means "keep the quad", which is the pre-2026-08-13 behaviour.
            const float wscale = ( halfw > 1e-5f && wantHalf > 1e-5f )
                                     ? std::min( 1.f, wantHalf / halfw )
                                     : 1.f;

            // Scratch buffers. Static because InternalDraw is on the render
            // thread and this runs per visible actor per frame; reallocating a
            // vector there is the kind of thing that shows up in a profile as
            // "the shadow feature is expensive" when it is not.
            static std::vector< RgPrimitiveVertex > s_basev;      // narrowed
            static std::vector< RgPrimitiveVertex > s_proxyverts; // + per-plane flip

            s_basev.assign( verts.begin(), verts.end() );
            for( auto& v : s_basev )
            {
                // canonicalise (normal -> +X, width -> Y), then narrow the width
                const float x = v.position[ 0 ] * ca - v.position[ 1 ] * sa;
                const float y = v.position[ 0 ] * sa + v.position[ 1 ] * ca;

                v.position[ 0 ] = x;
                v.position[ 1 ] = y * wscale;
            }

            // The U span, for mirroring below. Taken from the verts rather than
            // assumed to be 0..1: a sprite's quad is a page of an atlas.
            float umin = s_basev[ 0 ].texCoord[ 0 ];
            float umax = umin;
            for( const auto& v : s_basev )
            {
                umin = std::min( umin, v.texCoord[ 0 ] );
                umax = std::max( umax, v.texCoord[ 0 ] );
            }

            // Which way is the camera, horizontally? Used to decide each plane's
            // texture handedness, below.
            const float tocam_x = float( r_viewpoint.Pos.X ) * ONEGAMEUNIT_IN_METERS - px;
            const float tocam_y = float( r_viewpoint.Pos.Y ) * ONEGAMEUNIT_IN_METERS - py;

            for( int k = 0; k < planes; k++ )
            {
                const float yaw = RT_SpriteShadowYaw( k, planes );
                const float c   = std::cos( yaw );
                const float s   = std::sin( yaw );

                // Yaw about the world up axis (index 2 -- map Z), pitch dropped
                // on purpose: a shadow caster should stand upright, and the
                // billboard's pitch exists only to face the camera.
                auto proxymesh      = mesh;
                proxymesh.transform = RgTransform{ {
                    { c, -s, 0, px },
                    { s, c, 0, py },
                    { 0, 0, 1, pz },
                } };

                // Its own object, and it must never be mistaken for the actor:
                // no export, no mesh name -- a name would make RTGL1 look for a
                // gltf replacement and could substitute a model for a shadow.
                proxymesh.uniqueObjectID = mesh.uniqueObjectID + RT_SPRITE_SHADOW_ID_BASE +
                                           uint64_t( k );
                proxymesh.isExportable = false;
                proxymesh.pMeshName    = nullptr;
                proxymesh.flags        = 0;

                // MIRRORING. A textured plane's shadow is the mirror image of
                // its mask when the light is on the plane's far side -- so with
                // world-fixed planes, roughly half of them cast a FLIPPED
                // silhouette, and the union reads as the actor plus his mirror
                // twin (screen/invertedSpriteShadow.png). It appeared on some
                // enemies of a class and not others because which planes present
                // their back depends on each actor's bearing to the light, not
                // on the actor's type.
                //
                // A single quad cannot be correct for lights on both sides, so
                // the tie is broken toward the CAMERA: gzdoom already chose this
                // sprite's rotation frame for the camera's viewing angle, so
                // "un-mirrored as seen from the camera side" is the only
                // definition of correct available -- and it is the right one for
                // the flashlight, which is at the camera and is where this was
                // reported. The plane's +normal is its un-mirrored face (local X
                // is the facing axis, which for the real billboard points at the
                // viewer), so flip U whenever that normal points away.
                //
                // The choice can flip as the player circles an actor. It flips
                // exactly when a plane turns edge-on to the camera, which is
                // also when that plane's own shadow is at its thinnest, so the
                // change lands where it is least visible.
                const bool mirror = ( c * tocam_x + s * tocam_y ) < 0.f;

                s_proxyverts.assign( s_basev.begin(), s_basev.end() );
                if( mirror )
                {
                    for( auto& v : s_proxyverts )
                    {
                        v.texCoord[ 0 ] = ( umin + umax ) - v.texCoord[ 0 ];
                    }
                }

                auto proxyprim = prim;
                proxyprim.pNext       = nullptr;
                proxyprim.pVertices   = s_proxyverts.data();
                proxyprim.vertexCount = uint32_t( s_proxyverts.size() );
                // Keep ONLY the alpha test (the silhouette) and add shadow-only.
                // Everything else -- water, lava, emissive overrides, decal,
                // no-shadow -- is about how a surface LOOKS, and this surface is
                // never seen.
                proxyprim.flags = ( prim.flags & RG_MESH_PRIMITIVE_ALPHA_TESTED ) |
                                  RG_MESH_PRIMITIVE_SHADOW_ONLY;
                proxyprim.emissive = 0.f;

                RgResult pr = rt.rgUploadMeshPrimitive( &proxymesh, &proxyprim );
                RG_CHECK( pr );
            }
        }
    }

    // Doom64-RT: CONTACT OCCLUSION under sprites -- rt_sprite_ao.
    //
    // The other half of "a sprite is a board". rt_sprite_shadow above answers
    // WHERE THE LIGHT IS, and does it as well as a board can; this answers
    // WHETHER SOMETHING IS TOUCHING THIS FLOOR, which has the same answer from
    // every direction. That is why it is not redundant with the proxies: the one
    // case a cast shadow cannot survive is a light lying IN the caster's plane,
    // which projects it to a line -- and an occlusion term does not care.
    //
    // It also reaches the classes the proxies deliberately refuse. A vertical
    // cross through a corpse, a gib or a dropped shotgun is the wrong shape and
    // threw radiating spokes (screen/shadowissue.png), so those were left on
    // their stock billboard, i.e. with effectively nothing. A blob on the floor
    // is the RIGHT shape for a flat thing lying on a floor, which is the whole
    // reason a second mechanism is worth having rather than more planes.
    //
    // Implemented as an RTGL1 DECAL, so it is rasterized into the G-buffer and
    // multiplied into the floor's ALBEDO. Three properties fall out of that and
    // all three are wanted:
    //   - it scales with the light already on that floor. Full strength in a lit
    //     room, invisible in a dark one -- an occlusion term should never be able
    //     to make a surface darker than unlit.
    //   - it is never in the acceleration structure, so it cannot be hit by a
    //     reflection, refraction or bounce ray, and cannot occlude anything.
    //   - the decal shader discards where the underlying traced surface is more
    //     than 5 cm from the quad, so the blob stops at a step edge instead of
    //     smearing over it, and is hidden behind the sprite's own pixels for
    //     free. Its cost is that at long range the checkerboard neighbour it
    //     compares against can fall outside that 5 cm; rt_sprite_ao_dist is the
    //     bound, and it is tighter than the proxies' for that reason.
    if( cvar::rt_sprite_ao && !isUI && isSpriteBillboard && rtstate.m_lastthinghasfloor &&
        rtstate.m_lastthingradius > 0.f &&
        // ONE blob per actor per frame. A sprite with a fog layer draws twice
        // (hw_sprites.cpp), and a decal is alpha-blended with no ID collision
        // check to save us -- two passes would square the darkening. The
        // primitive index resets per actor, so index 0 is the first draw.
        prim.primitiveIndexInMesh == 0 &&
        // Never the viewer's own body: it is drawn at the eye, and a blob under
        // it is a dark ring painted around the camera.
        !rtstate.is< RtPrim::FirstPerson >() && !rtstate.is< RtPrim::FirstPersonViewer >() )
    {
        // SCOPE. 1 restricts the blob to the things rt_sprite_shadow skips.
        // Keyed on the CLASS (live monster), not on emitShadowProxies, so that a
        // monster which merely fell outside the proxies' distance cap does not
        // silently change category at 40 m.
        const bool scopeok =
            ( int{ cvar::rt_sprite_ao_scope } < 1 ) || !rtstate.m_lastthinglivemonster;

        // A LIVING SEE-THROUGH MONSTER gets no blob -- rt_sprite_ao_ghosts.
        //
        // The blob's claim is that a solid body is standing here and blocking
        // the light that would otherwise reach this floor. For the 64Spectre
        // and the 64NightmareImp that claim is false by construction: they are
        // rasterized TRANSLUCENT overlays and the room is visible through them
        // at rt_spectre_alpha / rt_nightmareimp_alpha, so an opaque smudge
        // under a body you can see through reads as a disc painted on the floor
        // following the monster -- and on the two creatures whose whole point
        // is being hard to see, it is the most visible thing about them.
        //
        // Same test makePrimFlags uses to choose the translucent overlay, so
        // the two can never disagree: whatever renders see-through is exactly
        // what is refused here. It is living-only for free -- IsLivingGhost()
        // excludes corpses, and rt_spectre_corpse_solid takes the dead ones out
        // of IsSpectre() too -- so a dead spectre is an ordinary solid sprite
        // and keeps its blob, which is what a body lying on the floor wants.
        const bool ghostok = bool( cvar::rt_sprite_ao_ghosts ) ||
                             !( IsSpectre() || IsLivingGhost() );

        // HEIGHT FADE. This is the honest part: contact occlusion asserts the
        // thing is ON the floor. A lost soul crossing a room, a cacodemon, a
        // rocket in flight and a jumping player must not carry a disc under
        // them, so it fades out over rt_sprite_ao_fade map units and is gone.
        const float fadeover = std::max( 1.f, float{ cvar::rt_sprite_ao_fade } );
        const float above    = rtstate.m_lastthingposition.Z - rtstate.m_lastthingfloorz;
        const float fade     = std::clamp( 1.f - above / fadeover, 0.f, 1.f );

        const float strength = std::clamp( float{ cvar::rt_sprite_ao_strength }, 0.f, 1.f );

        // A STANDING BODY GETS A FAINTER BLOB THAN A THING LYING ON THE FLOOR.
        //
        // Reported 2026-08-13: the shotgun reads right, the soldier is about 30%
        // too visible. Size alone does not explain that and rt_sprite_ao_sizefall
        // cannot fix it -- after the size falloff the two footprints are within a
        // few centimetres of each other. What actually differs is that the
        // shotgun's is a thin ellipse and the soldier's is a full circle, so the
        // soldier's covers roughly three times the area at the same strength.
        //
        // And there is a real reason it should be fainter, which is what makes
        // this a class rule rather than a fudge: a dropped weapon is IN CONTACT
        // with the floor across its whole footprint, so the occlusion under it is
        // strong and tight. A standing body touches the floor only at its feet --
        // everything above the ankles is far enough up that the floor still sees
        // most of the sky. Real contact occlusion under a standing figure IS
        // weaker and broader than under something lying flat.
        //
        // Keyed on m_lastthingupright, which already separates exactly these two
        // cases: the soldier passes (h 56 >= 1.5*r 20), the shotgun pickup fails
        // on the absolute height floor (h 20 < 24). Nothing new has to be
        // classified, and corpses correctly fall on the lying-down side the moment
        // gzdoom quarters their Height.
        const float uprightmul =
            rtstate.m_lastthingupright
                ? std::clamp( float{ cvar::rt_sprite_ao_upright }, 0.f, 1.f )
                : 1.f;

        const float alpha = strength * fade * uprightmul;

        // Distance cap, in metres, against the actor's own position. Matches the
        // proxies' cull in form so the two read the same way.
        const float aox = rtstate.m_lastthingposition.X * ONEGAMEUNIT_IN_METERS;
        const float aoy = rtstate.m_lastthingposition.Y * ONEGAMEUNIT_IN_METERS;
        const float aoz = rtstate.m_lastthingfloorz * ONEGAMEUNIT_IN_METERS;

        const bool inrange = [ & ]() -> bool {
            const float maxdist = std::max( 0.f, float{ cvar::rt_sprite_ao_dist } );
            if( maxdist <= 0.f )
            {
                return true;
            }
            const float dx = aox - float( r_viewpoint.Pos.X ) * ONEGAMEUNIT_IN_METERS;
            const float dy = aoy - float( r_viewpoint.Pos.Y ) * ONEGAMEUNIT_IN_METERS;
            const float dz = aoz - float( r_viewpoint.Pos.Z ) * ONEGAMEUNIT_IN_METERS;
            return std::sqrt( dx * dx + dy * dy + dz * dz ) <= maxdist;
        }();

        // 1/255 -- below this the blob is quantised to nothing by the packed
        // vertex colour anyway, so building the fan would be pure cost.
        if( scopeok && ghostok && inrange && alpha > ( 1.f / 255.f ) )
        {
            const int segs = std::clamp( int{ cvar::rt_sprite_ao_segments }, 3, 64 );

            // THE FOOTPRINT'S SHAPE, and it cannot be one shape for every class.
            //
            // A circle at the collision radius is right for something STANDING:
            // a body's footprint really is roughly round, it is camera-
            // independent, and the collision radius is the honest measure of it
            // (the sprite's canvas is about twice the body -- the marine drawn on
            // 64 units around a radius of 17 -- which is the trap
            // rt_sprite_shadow_width exists for).
            //
            // It is wrong for something LYING DOWN. A shotgun on the floor is a
            // long thin object, and its occlusion should be a long thin smudge
            // along the barrel, not a disc the size of its pickup radius. For
            // these the art is the only description of the shape there is, so the
            // footprint is fitted to the SPRITE QUAD: the along-axis is the
            // quad's real horizontal extent in world space, which turns with the
            // billboard, exactly as the drawn object does.
            //
            // The across-axis is NOT measurable. A single billboard cannot say
            // how deep an object is -- that is the same wall docs/rt-voxel-models
            // hits -- so it is a declared assumption, rt_sprite_ao_aspect, and
            // not a derived number pretending to be one.
            const bool useellipse =
                ( int{ cvar::rt_sprite_ao_shape } >= 2 ) ||
                ( int{ cvar::rt_sprite_ao_shape } == 1 && !rtstate.m_lastthingupright );

            // Along-axis direction (world, horizontal) and half-length.
            float axu = 1.f, axv = 0.f;
            float rada = rtstate.m_lastthingradius * ONEGAMEUNIT_IN_METERS *
                         std::max( 0.f, float{ cvar::rt_sprite_ao_radius } );
            float radb = rada;

            if( useellipse && verts.size() >= 3 )
            {
                // The quad's horizontal footprint, from its own world vertices.
                //
                // Derived by principal axis rather than by "take local Y", which
                // is the mistake §5.3 spent three fixes on: the local verts carry
                // a baked (camera_yaw - actor_yaw) offset, so no local axis means
                // what its name says. A 2x2 covariance of the world XY positions
                // has no such assumption in it and is correct for a pitched quad
                // too, where the up axis also has a horizontal component.
                float cx = 0.f, cy = 0.f;
                float wx[ 4 ], wy[ 4 ];
                const int n = int( std::min< size_t >( verts.size(), 4 ) );

                for( int i = 0; i < n; i++ )
                {
                    const float lx = verts[ i ].position[ 0 ];
                    const float ly = verts[ i ].position[ 1 ];
                    const float lz = verts[ i ].position[ 2 ];

                    wx[ i ] = transform.matrix[ 0 ][ 0 ] * lx + transform.matrix[ 0 ][ 1 ] * ly +
                              transform.matrix[ 0 ][ 2 ] * lz + transform.matrix[ 0 ][ 3 ];
                    wy[ i ] = transform.matrix[ 1 ][ 0 ] * lx + transform.matrix[ 1 ][ 1 ] * ly +
                              transform.matrix[ 1 ][ 2 ] * lz + transform.matrix[ 1 ][ 3 ];
                    cx += wx[ i ];
                    cy += wy[ i ];
                }
                cx /= float( n );
                cy /= float( n );

                float sxx = 0.f, syy = 0.f, sxy = 0.f;
                for( int i = 0; i < n; i++ )
                {
                    const float dx = wx[ i ] - cx;
                    const float dy = wy[ i ] - cy;
                    sxx += dx * dx;
                    syy += dy * dy;
                    sxy += dx * dy;
                }

                if( sxx + syy > 1e-9f )
                {
                    const float th = 0.5f * std::atan2( 2.f * sxy, sxx - syy );
                    axu            = std::cos( th );
                    axv            = std::sin( th );

                    float half = 0.f;
                    for( int i = 0; i < n; i++ )
                    {
                        half = std::max(
                            half, std::fabs( ( wx[ i ] - cx ) * axu + ( wy[ i ] - cy ) * axv ) );
                    }

                    if( half > 1e-5f )
                    {
                        rada = half * std::max( 0.f, float{ cvar::rt_sprite_ao_fit } );
                        radb = rada * std::clamp( float{ cvar::rt_sprite_ao_aspect }, 0.05f, 1.f );
                    }
                }
            }

            // SIZE FALLOFF -- the bigger the sprite, the less blob it gets.
            //
            // Both sizing paths above are LINEAR in the object: the circle is a
            // multiple of the collision radius, the ellipse a multiple of the
            // sprite's own width. So a soldier ends up with a visibly larger
            // footprint than a shotgun lying next to him, in the same proportion
            // as their bodies -- and that reads wrong, because contact occlusion
            // is not a scale model of the object. It is the gap between the object
            // and the floor, and that gap does NOT grow with the object: a
            // soldier's boots meet the floor on about as much area as a shotgun's
            // receiver does.
            //
            // So the footprint is pulled toward a reference size by an exponent:
            //
            //     f = (ref / size) ^ sizefall
            //
            // sizefall 0 leaves the linear behaviour untouched, 1 makes every blob
            // exactly ref across whatever its owner's size, and the default sits
            // between -- big things still read bigger, just far less than linearly.
            // An exponent rather than a clamp because it has no discontinuity: no
            // actor size is a special case, and nothing pops as a thing grows or
            // shrinks (gzdoom quarters Height on death, so this happens in play).
            //
            // ref is fixed at one metre -- 32 map units, a Doom actor of ordinary
            // size -- rather than being a cvar of its own. Moving the pivot point
            // is what rt_sprite_ao_radius and rt_sprite_ao_fit already do; this
            // knob only decides how much SPREAD there is around it.
            {
                const float sizefall = std::clamp( float{ cvar::rt_sprite_ao_sizefall }, 0.f, 1.f );
                const float size     = std::max( rada, radb );

                if( sizefall > 0.f && size > 1e-4f )
                {
                    constexpr float AoRefSize = 1.f; // metres
                    const float     f         = std::pow( AoRefSize / size, sizefall );
                    rada *= f;
                    radb *= f;
                }
            }

            const float rad = std::max( rada, radb );

            if( rada > 1e-4f && radb > 1e-4f )
            {
                // A TRIANGLE FAN, not a textured quad: the radial falloff is
                // then vertex-colour interpolation and the feature ships no art
                // and touches no material. Centre alpha = strength, rim alpha =
                // 0. RGB is black, so the blend (src-alpha over) leaves the
                // floor at albedo * (1 - a) -- a pure multiplicative darkening.
                // With no texture bound RTGL1 samples its 1x1 white, which is
                // exactly the identity this needs.
                static std::vector< RgPrimitiveVertex > s_aoverts;
                static std::vector< uint32_t >          s_aoidx;

                s_aoverts.clear();
                s_aoidx.clear();
                s_aoverts.reserve( size_t( segs ) + 1 );
                s_aoidx.reserve( size_t( segs ) * 3 );

                const RgNormalPacked32 up      = rt.rgUtilPackNormal( 0.f, 0.f, 1.f );
                const RgColor4DPacked32 cCentre = rt.rgUtilPackColorFloat4D( 0.f, 0.f, 0.f, alpha );
                const RgColor4DPacked32 cRim    = rt.rgUtilPackColorFloat4D( 0.f, 0.f, 0.f, 0.f );

                // WORLD-SPACE VERTICES, IDENTITY TRANSFORM -- and this is the one
                // thing about the decal path that cannot be guessed from the API.
                //
                // RsDecal.vert writes `outWorldPos = position`, the raw LOCAL
                // vertex, while it transforms only gl_Position (the push constant
                // is model*viewProj premultiplied in RasterizedPushConst). So the
                // fragment shader's 5 cm test compares an UNTRANSFORMED position
                // against a true world-space surface position.
                //
                // Put the blob's location in the transform, as the shadow proxies
                // above legitimately do, and the quad rasterizes in exactly the
                // right place on screen and then discards every single fragment,
                // because its "world" position is still near the origin and the
                // floor is tens of metres away. Nothing is drawn and nothing is
                // logged -- which is precisely what was reported the first time
                // this shipped.
                //
                // gzdoom's own wall decals (hw_decal.cpp) never hit this because
                // world geometry takes the MakeTransform branch, where the
                // transform is identity and local already IS world. A sprite is
                // the only caller that arrives here with a real transform.
                // 1 mm of bias keeps the quad off the floor's exact plane
                // without spending the 5 cm test's budget.
                constexpr float AoBias = 0.001f;
                const float     aoz_w  = aoz + AoBias;

                s_aoverts.push_back( RgPrimitiveVertex{
                    .position     = { aox, aoy, aoz_w },
                    .normalPacked = up,
                    .texCoord     = { 0.5f, 0.5f },
                    .color        = cCentre,
                } );

                // A SINGLE FAN: one centre vertex at full alpha, one rim ring
                // at zero, every triangle sharing the centre.
                //
                // This topology is the reason the blob is smooth, and it was
                // briefly replaced by a core-plateau plus a banded penumbra --
                // which put TRIANGLES around every blob (reported 2026-08-13).
                // The cause is structural, so do not reintroduce a high-alpha
                // ring expecting more segments to save it:
                //
                //   a ring quad carries alpha A at both INNER corners and 0 at
                //   both OUTER ones. Split into (i0,o0,o1) and (i0,o1,i1), the
                //   first interpolates as A*L(i0) -- iso-lines parallel to the
                //   outer edge -- and the second as A*(L(i0)+L(i1)) -- iso-lines
                //   parallel to the inner edge. Two gradient directions meeting
                //   at the diagonal is a crease, in every segment at once.
                //
                // A fan has no such quad. Every triangle's alpha is A*L(centre),
                // so the gradients agree, and the only polygonal boundary is the
                // rim, which sits at alpha 0 where a crease cannot be seen.
                //
                // Swept as an ELLIPSE on the two world-horizontal axes: (axu,axv)
                // is the along-axis and its perpendicular (-axv,axu) the across.
                // For the circle case rada == radb and the axes cancel out, so one
                // loop serves both shapes.
                for( int k = 0; k < segs; k++ )
                {
                    const float a  = ( 2.f * 3.14159265f * float( k ) ) / float( segs );
                    const float cx = std::cos( a );
                    const float sy = std::sin( a );

                    const float ox = cx * rada;
                    const float oy = sy * radb;

                    s_aoverts.push_back( RgPrimitiveVertex{
                        .position     = { aox + ox * axu - oy * axv,
                                          aoy + ox * axv + oy * axu,
                                          aoz_w },
                        .normalPacked = up,
                        .texCoord     = { 0.5f + 0.5f * cx, 0.5f + 0.5f * sy },
                        .color        = cRim,
                    } );

                    s_aoidx.push_back( 0 );
                    s_aoidx.push_back( uint32_t( 1 + k ) );
                    s_aoidx.push_back( uint32_t( 1 + ( ( k + 1 ) % segs ) ) );
                }

                // IDENTITY, to match the world-space verts above. Anything else
                // would move the quad on screen without moving the position the
                // 5 cm test reads, i.e. it would silently discard the blob.
                auto aomesh      = mesh;
                aomesh.transform = RgTransform{ {
                    { 1, 0, 0, 0 },
                    { 0, 1, 0, 0 },
                    { 0, 0, 1, 0 },
                } };
                aomesh.uniqueObjectID = mesh.uniqueObjectID + RT_SPRITE_AO_ID_BASE;
                aomesh.isExportable   = false;
                // No mesh name, or RTGL1 would look for a gltf replacement and
                // could substitute a model for the actor's shadow blob.
                aomesh.pMeshName            = nullptr;
                aomesh.flags                = 0;
                aomesh.localLightsIntensity = 0.f;

                auto aoprim                 = prim;
                aoprim.pNext                = nullptr;
                aoprim.flags                = RG_MESH_PRIMITIVE_DECAL;
                aoprim.primitiveIndexInMesh = 0;
                aoprim.pVertices            = s_aoverts.data();
                aoprim.vertexCount          = uint32_t( s_aoverts.size() );
                aoprim.pIndices             = s_aoidx.data();
                aoprim.indexCount           = uint32_t( s_aoidx.size() );
                // No texture: the falloff is in the vertex colours. Keeping the
                // sprite's own texture here would stamp the enemy's artwork onto
                // the floor.
                aoprim.pTextureName = nullptr;
                aoprim.textureFrame = 0;
                aoprim.color        = RG_PACKED_COLOR_WHITE;
                // The decal shader falls back to ldrEmis = albedo when no
                // emissive texture is bound, so a non-zero emissive here would
                // make the blob GLOW black-on-nothing and, worse, write screen
                // emission where there should be none.
                aoprim.emissive = 0.f;
                // Unused on a decal (only WORLD_CLASSIC consumes it), but it is
                // a float and inheriting the sprite's would be noise.
                aoprim.classicLight = 1.f;

                RgResult ar = rt.rgUploadMeshPrimitive( &aomesh, &aoprim );
                RG_CHECK( ar );

                // rt_sprite_ao_debug -- see the cvar. This counts UPLOADS, which
                // is the only number that separates "the gates rejected it" from
                // "it was drawn and every fragment was discarded". The two look
                // identical on screen and the second one is what shipped first.
                if( cvar::rt_sprite_ao_debug )
                {
                    static int      s_frames  = 0;
                    static int      s_emitted = 0;
                    static float    s_nearest = -1.f;
                    static float    s_np[ 3 ] = { 0, 0, 0 };
                    static float    s_nr = 0.f, s_na = 0.f;

                    const float dx = aox - float( r_viewpoint.Pos.X ) * ONEGAMEUNIT_IN_METERS;
                    const float dy = aoy - float( r_viewpoint.Pos.Y ) * ONEGAMEUNIT_IN_METERS;
                    const float dz = aoz - float( r_viewpoint.Pos.Z ) * ONEGAMEUNIT_IN_METERS;
                    const float d  = std::sqrt( dx * dx + dy * dy + dz * dz );

                    s_emitted++;
                    if( s_nearest < 0.f || d < s_nearest )
                    {
                        s_nearest = d;
                        s_np[ 0 ] = aox;
                        s_np[ 1 ] = aoy;
                        s_np[ 2 ] = aoz_w;
                        s_nr      = rad;
                        s_na      = alpha;
                    }

                    if( ++s_frames >= 60 )
                    {
                        Printf( "rt_sprite_ao: %d emitted over 60 draws; nearest at "
                                "%.2f m, pos=(%.2f %.2f %.2f) r=%.2f a=%.2f\n",
                                s_emitted,
                                s_nearest,
                                s_np[ 0 ],
                                s_np[ 1 ],
                                s_np[ 2 ],
                                s_nr,
                                s_na );
                        s_frames  = 0;
                        s_emitted = 0;
                        s_nearest = -1.f;
                    }
                }
            }
        }
    }
}
