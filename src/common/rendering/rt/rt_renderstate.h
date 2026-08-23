#pragma once

// The RT render state: gzdoom's FRenderState and DFrameBuffer, over RTGL1.
//
// RTFrameBuffer is the frame; RTRenderState is everything the engine says while
// drawing one. Both used to be defined inline inside rt_main.cpp's anonymous
// namespace, which is exactly why neither the draw path nor the weapon lighting
// could be moved out of that file: internal linkage means no other translation
// unit can define a member.
//
// The two biggest members are declared here and defined elsewhere:
//   InternalDraw        -> rt_draw.cpp
//   the weapon lighting -> rt_weapon.cpp

#include "rt_internal.h"
#include "rt_buffers.h"

#include "base_sysfb.h"
#include "hw_renderstate.h"
#include "hw_viewpointbuffer.h"
#include "flatvertices.h"
// `twod`, the 2D drawer RTFrameBuffer::Update clears each frame.
#include "v_draw.h"

namespace rtx
{

class RTRenderState;

class RTFrameBuffer : public SystemBaseFrameBuffer
{
    using Super = SystemBaseFrameBuffer;

public:
    RTFrameBuffer( void* hMonitor, bool fullscreen );
    ~RTFrameBuffer() override;
    void InitializeState() override;
    void BeginFrame() override
    {
        SetViewportRects( nullptr );
        RT_BeginFrame();
        Super::BeginFrame();
    }
    void Update() override
    {
        this->Draw2D();
        twod->Clear();
        RT_DrawFrame();
        Super::Update();
    }
    void FirstEye() override;

    FRenderState*     RenderState() override;
    IVertexBuffer*    CreateVertexBuffer() override;
    IIndexBuffer*     CreateIndexBuffer() override;
    IDataBuffer*      CreateDataBuffer( int bindingpoint, bool ssbo, bool needsresize ) override;
    IHardwareTexture* CreateHardwareTexture( int numchannels ) override;
    void              PrecacheMaterial( FMaterial* mat, int translation ) override;

    void SetVSync( bool vsync ) override { m_vsync = vsync; }
    void SetTextureFilterMode() override {}
    void SetLevelMesh( hwrenderer::LevelMesh* mesh ) override {}

    void Draw2D() override;

    // RTGL1 has no frame-readback API; grab the presented HWND contents.
    TArray< uint8_t > GetScreenshotBuffer( int& pitch, ESSType& color_type, float& gamma ) override;

public:
    void RT_MarkWasSky() { m_wassky = true; }

private:
    void RT_BeginFrame();
    void RT_DrawFrame();

private:
    RTRenderState* m_state{ nullptr };
    bool           m_vsync{ false };
    bool           m_wassky{ false };
};

class RTRenderState : public FRenderState
{
public:
    explicit RTRenderState( RTFrameBuffer* parent ) : m_fb( parent ) {}
    virtual ~RTRenderState() = default;

    void RT_BeginFrame()
    {
        rtstate.reset();
        m_weaponDrawCallIndex = 0;
    }

    bool IsCurrentDrawIgnored() const
    {
        return rtstate.is< RtPrim::Ignored >() || mTextureMode == TM_FOGLAYER;
    }

    // Retribution's two soft-blend monsters — both DECORATE RenderStyle Translucent
    // (64Spectre at a pulsing alpha, 64NightmareImp at a flat 0.60), so both land in the
    // same RTGL1 hole: below MESH_TRANSLUCENT_ALPHA_THRESHOLD they are rasterized rather
    // than traced, and the rasterizer lights nothing. Keyed off the sprite prefix —
    // there is no actor pointer down here. n[4] is the animation frame letter
    // (rt_state.h: 'A' + animframe).
    enum class GhostActor
    {
        None,
        Spectre,       // SAR2 — living A..H, corpse I..N
        NightmareImp,  // TRO2 — living A..K, corpse/gib L..X
    };

    auto GhostSprite( bool* outIsCorpse = nullptr ) const -> GhostActor
    {
        if( outIsCorpse )
        {
            *outIsCorpse = false;
        }
        if( !rt_mod_compat || !rtstate.is< RtPrim::ExportInstance >() )
        {
            return GhostActor::None;
        }
        const char* n = rtstate.get_exportinstance_name();
        if( !n || !n[ 0 ] || !n[ 4 ] )
        {
            return GhostActor::None;
        }
        // SARG is the regular pinky and TROO the regular imp — neither is soft-blend.
        if( n[ 0 ] == 'S' && n[ 1 ] == 'A' && n[ 2 ] == 'R' && n[ 3 ] == '2' )
        {
            if( outIsCorpse )
            {
                *outIsCorpse = ( n[ 4 ] >= 'I' );
            }
            return GhostActor::Spectre;
        }
        if( n[ 0 ] == 'T' && n[ 1 ] == 'R' && n[ 2 ] == 'O' && n[ 3 ] == '2' )
        {
            if( outIsCorpse )
            {
                *outIsCorpse = ( n[ 4 ] >= 'L' );
            }
            return GhostActor::NightmareImp;
        }
        return GhostActor::None;
    }

    // Should this sprite be an ordinary solid, path-traced, lit sprite? Alive and dead
    // are separate cvars because the corpse fix landed first and is settled.
    bool IsSolidGhost() const
    {
        bool corpse = false;
        if( GhostSprite( &corpse ) == GhostActor::None )
        {
            return false;
        }
        return corpse ? bool( cvar::rt_spectre_corpse_solid ) : bool( cvar::rt_ghost_solid );
    }

    // Multiplier for a LIVING ghost's vertex alpha, so the body fades out in a dark room
    // while its eyes do not. See rt_ghost_lightscale for why alpha (and not colour) is
    // the channel that separates the two: RasterizerPipelines.cpp blends attachment 0
    // (body) with SRC_ALPHA and attachment 1 (outScreenEmission / the _e eye mask) with
    // ONE,ONE — the emission output never sees alpha at all.
    float GhostLightScale() const
    {
        bool corpse = false;
        if( GhostSprite( &corpse ) == GhostActor::None || corpse )
        {
            return 1.f;
        }

        const float amount = std::clamp( float( cvar::rt_ghost_lightscale ), 0.f, 1.f );
        if( amount <= 0.f )
        {
            return 1.f;
        }

        // m_lightlevel is the sprite-only field (hw_sprites.cpp sets it from
        // actor->Sector->GetSpriteLight(), defaulting to 255). NOT m_sectorLightLevel —
        // that one is only ever pushed for walls and flats, so on a sprite it is stale
        // and would have read as a permanently pitch-black room.
        const float ll = std::clamp( float( rtstate.m_lightlevel ) / 255.f, 0.f, 1.f );

        // sqrt, not linear: Doom lightlevels read far brighter than their numeric value,
        // so a linear curve crushes an ordinary dim-but-lit corridor down to nearly
        // invisible. Only genuinely unlit rooms should erase the body.
        const float lit = std::sqrt( ll );

        return 1.f - amount * ( 1.f - lit );
    }

    // A soft-blend monster that is alive and is NOT being forced solid. These must be
    // rasterized TRANSLUCENT overlays, never ALPHA_TESTED cutouts — see makePrimFlags.
    bool IsLivingGhost() const
    {
        bool corpse = false;
        if( GhostSprite( &corpse ) == GhostActor::None || corpse )
        {
            return false;
        }
        return !IsSolidGhost();
    }

    bool IsSpectre() const
    {
        switch( mRenderStyle.BlendOp )
        {
            case STYLEOP_Fuzz:
            case STYLEOP_FuzzOrAdd:
            case STYLEOP_FuzzOrSub:
            case STYLEOP_FuzzOrRevSub:
            case STYLEOP_Shadow: return true;
            default: break;
        }
        // Retribution 64Spectre is STYLE_Translucent + SAR2, not classic Fuzz.
        // Uses rasterized TRANSLUCENT + minalpha cap for see-through ghostly look —
        // unless it is being rendered solid, in which case it must NOT carry
        // RG_MESH_PRIMITIVE_TRANSLUCENT, because that flag forces rasterization on its
        // own regardless of alpha (VulkanDevice.cpp IsRasterized).
        if( IsSolidGhost() )
        {
            return false;
        }
        if( rt_mod_compat && rtstate.is< RtPrim::ExportInstance >() )
        {
            const char* n = rtstate.get_exportinstance_name();
            if( n && n[ 0 ] == 'S' && n[ 1 ] == 'A' && n[ 2 ] == 'R' &&
                n[ 3 ] == '2' )  // SAR2 = 64Spectre sprite prefix
            {
                // n[4] is the animation frame letter (rt_state.h: 'A' + animframe).
                // SAR2 I..N are the death frames — the WAD stores them as I0..N0, and
                // the DECORATE Death sequence fades in to A_SetTranslucent(1.0), so the
                // corpse is authored as a SOLID body, not a ghost.
                //
                // Keeping the spectre treatment on them is what made a dead spectre take
                // no light: spectres are flagged RG_MESH_PRIMITIVE_TRANSLUCENT, RTGL1
                // rasterizes any translucent primitive instead of tracing it
                // (VulkanDevice.cpp IsRasterized), and the rasterizer shader
                // (RsWorld.inl) outputs vertexColor * texture with no lighting term
                // whatsoever. So the corpse received light from nothing — not the
                // flashlight, not a lamp, not the sun — and sat at full texture
                // brightness on a dark floor. Dropping it out of IsSpectre() makes it an
                // ordinary alpha-tested sprite: alpha 1.0 clears
                // MESH_TRANSLUCENT_ALPHA_THRESHOLD, it enters the BLAS, and it is lit
                // and casts a shadow like every other corpse (2026-08-08).
                if( cvar::rt_spectre_corpse_solid && n[ 4 ] >= 'I' && n[ 4 ] <= 'N' )
                {
                    return false;
                }
                return true;
            }
        }
        return false;
    }

    void Draw( int dt, int index, int count, bool apply = true ) override
    {
        if( IsCurrentDrawIgnored() )
        {
            return;
        }

        assert( count > 0 );

        const uint32_t* pIndices   = nullptr;
        uint32_t        indexCount = 0;

        bool islines = false;

        switch( dt )
        {
            case DT_Points: assert( 0 ); return;
            case DT_Lines: islines = true; break;
            case DT_Triangles:
                // indices are sequential, just use vertex array
                break;
            case DT_TriangleFan:
                rt.rgUtilScratchGetIndices(
                    RG_UTIL_IM_SCRATCH_TOPOLOGY_TRIANGLE_FAN, count, &pIndices, &indexCount );
                break;
            case DT_TriangleStrip:
                rt.rgUtilScratchGetIndices(
                    RG_UTIL_IM_SCRATCH_TOPOLOGY_TRIANGLE_STRIP, count, &pIndices, &indexCount );
                break;
            default: break;
        }

        auto vb = static_cast< RTVertexBuffer* >( mVertexBuffer );
        if( !vb )
        {
            assert( 0 );
            return;
        }
        assert( rtstate.is< RtPrim::Sky >() == vb->IsSky() );

        InternalDraw( vb->AccessFormatted( mVertexOffsets[ 0 ] + index, count ),
                      std::span{ pIndices, indexCount },
                      vb->IsUI(),
                      islines );
    }

    void DrawIndexed( int dt, int index, int count, bool apply = true ) override
    {
        if( IsCurrentDrawIgnored() )
        {
            return;
        }

        assert( dt == DT_Triangles );
        if( count <= 0 )
        {
            // E3M2 fails
            return;
        }

        auto vb = static_cast< RTVertexBuffer* >( mVertexBuffer );
        if( !vb )
        {
            assert( 0 );
            return;
        }
        assert( rtstate.is< RtPrim::Sky >() == vb->IsSky() );

        auto ib = static_cast< RTIndexBuffer* >( mIndexBuffer );
        if( !ib )
        {
            assert( 0 );
            return;
        }

        auto indices = ib->AccessFormatted( index, count );

        auto [ vertFirst, vertCount ] = RTIndexBuffer::CalcFirstVertexAndVertexCount( indices );

        InternalDraw( vb->AccessFormatted( mVertexOffsets[ 0 ] + vertFirst, vertCount ),
                      ib->MakeWithNewFirstIndex( indices, vertFirst ),
                      vb->IsUI() );
    }

    void ClearScreen() override {}
    bool SetDepthClamp( bool on ) override { return on; }
    void SetDepthMask( bool on ) override {}
    void SetDepthFunc( int func ) override {}
    void SetDepthRange( float min, float max ) override {}
    void SetColorMask( bool r, bool g, bool b, bool a ) override {}
    void SetStencil( int offs, int op, int flags = -1 ) override {}
    void SetCulling( int mode ) override {}
    void EnableClipDistance( int num, bool state ) override {}
    void Clear( int targets ) override {}
    void EnableStencil( bool on ) override {}
    void SetScissor( int x, int y, int w, int h ) override {}
    void SetViewport( int x, int y, int w, int h ) override
    {
        m_viewport = RgViewport{
            .x        = float( x ),
            .y        = float( y ),
            .width    = float( w ),
            .height   = float( h ),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
    }
    void EnableDepthTest( bool on ) override {}
    void EnableMultisampling( bool on ) override {}
    void EnableLineSmooth( bool on ) override {}
    void EnableDrawBuffers( int count, bool apply ) override {}

private:
    static bool IsPerspectiveMatrix( const float* m );
    static bool IsLikeIdentity( const float* m );
    static bool IsLikeIdentity( const double* m );

    // If need to calculate a transform at the sprite's bottom.
    bool RequiresTrueTransform() const
    {
        if( rtstate.is< RtPrim::ExportInstance >() )
        {
            // need to make a true one, since gzdoom doesn't provide a world transform
            return !mModelMatrixEnabled;
        }
        return false;
    }

    auto CalculateTrueTransformAndItsVerts( std::span< const RgPrimitiveVertex > originalVerts )
        -> std::pair< RgTransform, std::span< const RgPrimitiveVertex > >
    {
        assert( RequiresTrueTransform() );
        assert( originalVerts.size() == 4 ); // to find a non-sprite without model matrix
        assert( !mModelMatrixEnabled );      // means that vert positions are in a metric space

        // need to offset a bit, to prevent clipping with floor (for glass spectres)
        constexpr float CLIP_FIX_OFFSET = 0.005f;

        const float pivot[] = {
            rtstate.m_lastthingposition.X * ONEGAMEUNIT_IN_METERS,
            rtstate.m_lastthingposition.Y * ONEGAMEUNIT_IN_METERS,
            rtstate.m_lastthingposition.Z * ONEGAMEUNIT_IN_METERS + CLIP_FIX_OFFSET,
        };

        m_tempverts.clear();
        m_tempverts.assign( originalVerts.begin(), originalVerts.end() );

        // make relative to pivot
        for( uint32_t v = 0; v < originalVerts.size(); v++ )
        {
            m_tempverts[ v ].position[ 0 ] -= pivot[ 0 ];
            m_tempverts[ v ].position[ 1 ] -= pivot[ 1 ];
            m_tempverts[ v ].position[ 2 ] -= pivot[ 2 ];
        }

        // un-rotate the angle
        const auto [ pitch, yaw ] = rtstate.get_spriterotation();
        
#if 0 // reference
        Matrix3x4 m;
        m.MakeIdentity();
        m.Rotate( 0, 0, 1, to_deg( yaw ) );
        m.Rotate( 0, 1, 0, to_deg( pitch ) );
#else
        const float cos_pitch = std::cos( pitch );
        const float sin_pitch = std::sin( pitch );
        const float cos_yaw   = std::cos( yaw );
        const float sin_yaw   = std::sin( yaw );

        //     |  cos_pitch, 0, sin_pitch |   | cos_yaw, -sin_yaw, 0 |
        // m = |          0, 1,         0 | x | sin_yaw,  cos_yaw, 0 |
        //     | -sin_pitch, 0, cos_pitch |   |       0,       0,  1 |

        float m[ 3 ][ 3 ] = {
            { cos_yaw * cos_pitch, -sin_yaw, cos_yaw * sin_pitch },
            { sin_yaw * cos_pitch, cos_yaw, sin_yaw * sin_pitch },
            { -sin_pitch, 0, cos_pitch },
        };
#endif
        const float m_inv[ 3 ][ 3 ] = {
            { m[ 0 ][ 0 ], m[ 1 ][ 0 ], m[ 2 ][ 0 ] },
            { m[ 0 ][ 1 ], m[ 1 ][ 1 ], m[ 2 ][ 1 ] },
            { m[ 0 ][ 2 ], m[ 1 ][ 2 ], m[ 2 ][ 2 ] },
        };
        for( auto& v : m_tempverts )
        {
            ApplyMat33ToVec3_row( m_inv, v.position );
        }

        return {
            RgTransform{ {
                { m[ 0 ][ 0 ], m[ 0 ][ 1 ], m[ 0 ][ 2 ], pivot[ 0 ] },
                { m[ 1 ][ 0 ], m[ 1 ][ 1 ], m[ 1 ][ 2 ], pivot[ 1 ] },
                { m[ 2 ][ 0 ], m[ 2 ][ 1 ], m[ 2 ][ 2 ], pivot[ 2 ] },
            } },
            std::span{ m_tempverts },
        };
    }

    auto MakeTransform( bool isSky ) const -> RgTransform
    {
        assert( !RequiresTrueTransform() );

        // also converts to metric
        auto fromGzMatrix = []( const float* m ) {
            return RgTransform{ {
                { m[ 0 ], m[ 4 ], m[ 8 ], m[ 12 ] * ONEGAMEUNIT_IN_METERS },
                { m[ 1 ], m[ 5 ], m[ 9 ], m[ 13 ] * ONEGAMEUNIT_IN_METERS },
                { m[ 2 ], m[ 6 ], m[ 10 ], m[ 14 ] * ONEGAMEUNIT_IN_METERS },
            } };
        };

        // sky has view matrix that is different from main camera, apply it
        if( isSky )
        {
            auto l_unit = []( float f ) {
                return f > +0.5f   ? +1.0f //
                       : f < -0.5f ? -1.0f //
                                   : 0.0f;
            };

            auto skyToMainCameraIrregular =
                VSMatrix::smultMatrix( m_mainCameraView_Inverse, m_view );

            const float* irr = skyToMainCameraIrregular.get();

            const float skyToMainCamera[ 16 ] = {
                l_unit( irr[ 0 ] ), l_unit( irr[ 1 ] ), l_unit( irr[ 2 ] ),  0,
                l_unit( irr[ 4 ] ), l_unit( irr[ 5 ] ), l_unit( irr[ 6 ] ),  0,
                l_unit( irr[ 8 ] ), l_unit( irr[ 9 ] ), l_unit( irr[ 10 ] ), 0,
                irr[ 12 ],          irr[ 13 ],          irr[ 14 ],           1,
            };

            auto skyTransform = mModelMatrix;
            skyTransform.scale( 1, cvar::rt_sky_stretch, 1 );

            auto t = VSMatrix::smultMatrix( skyToMainCamera, skyTransform.get() );
            return fromGzMatrix( t.get() );
        }

        if( mModelMatrixEnabled )
        {
            return fromGzMatrix( mModelMatrix.get() );
        }

        return RG_TRANSFORM_IDENTITY;
    }

    auto MapLightLevel( int lightlevel ) -> float
    {
        assert( lightlevel <= 255 );
        int lmin = std::max< int >( cvar::rt_lightlevel_min, 0 );
        int lmax = std::min< int >( cvar::rt_lightlevel_max, 255 );

        if( lmin >= lmax )
        {
            return 0.0f;
        }
        if( lightlevel <= lmin )
        {
            return 0.0f;
        }
        if( lightlevel >= lmax )
        {
            return 1.0f;
        }
        float t = float( lightlevel - lmin ) / float( lmax - lmin );

        if( std::abs( cvar::rt_lightlevel_exp - 2.f ) < 0.01f )
        {
            return t * t;
        }
        if( std::abs( cvar::rt_lightlevel_exp - 1.f ) < 0.01f )
        {
            return t;
        }
        return std::powf( t, cvar::rt_lightlevel_exp );
    }

    auto MakeFirstPersonQuadInWorldSpace( std::span< const RgPrimitiveVertex > verts )
        -> std::pair< RgTransform, std::span< const RgPrimitiveVertex > >
    {
        if( verts.size() != 4 )
        {
            // assert( 0 );
            return { RgTransform{ RG_TRANSFORM_IDENTITY }, verts };
        }

        const auto  priority = m_weaponDrawCallIndex++;
        const float z        = 0.1f / float( 1 + priority );

        auto toPix = []( const RgPrimitiveVertex& vert ) {
            // because of MakeFormatted...
            return RgFloat2D{
                vert.position[ 0 ] / ONEGAMEUNIT_IN_METERS,
                vert.position[ 2 ] / ONEGAMEUNIT_IN_METERS,
            };
        };

        auto applyViewport = []( const RgViewport& vp, const RgFloat2D& vert ) {
            return RgFloat2D{
                vert.data[ 0 ] / float( vp.width ),
                vert.data[ 1 ] / float( vp.height ),
            };
        };

        // screen space [0,1]
        RgFloat2D scr01[] = {
            applyViewport( m_viewport, toPix( verts[ 0 ] ) ),
            applyViewport( m_viewport, toPix( verts[ 1 ] ) ),
            applyViewport( m_viewport, toPix( verts[ 2 ] ) ),
            applyViewport( m_viewport, toPix( verts[ 3 ] ) ),
        };

        // remap [0,1] to [-1,1] clip space
        RgFloat4D clipspace[] = {
            RgFloat4D{ scr01[ 0 ].data[ 0 ] * 2 - 1, scr01[ 0 ].data[ 1 ] * 2 - 1, z, 1.0f },
            RgFloat4D{ scr01[ 1 ].data[ 0 ] * 2 - 1, scr01[ 1 ].data[ 1 ] * 2 - 1, z, 1.0f },
            RgFloat4D{ scr01[ 2 ].data[ 0 ] * 2 - 1, scr01[ 2 ].data[ 1 ] * 2 - 1, z, 1.0f },
            RgFloat4D{ scr01[ 3 ].data[ 0 ] * 2 - 1, scr01[ 3 ].data[ 1 ] * 2 - 1, z, 1.0f },
        };

        // inverse projection to transform clip space -> view space
        RgFloat4D viewspace[] = {
            ApplyMat44ToVec4( m_mainCameraProjection_Inverse, clipspace[ 0 ] ),
            ApplyMat44ToVec4( m_mainCameraProjection_Inverse, clipspace[ 1 ] ),
            ApplyMat44ToVec4( m_mainCameraProjection_Inverse, clipspace[ 2 ] ),
            ApplyMat44ToVec4( m_mainCameraProjection_Inverse, clipspace[ 3 ] ),
        };

#if 0
        // inverse view to transform view space -> world space
        RgFloat3D worldspace[] = {
            FromHomogeneous( ApplyMat44ToVec4( m_mainCameraView_Inverse, viewspace[ 0 ] ) ),
            FromHomogeneous( ApplyMat44ToVec4( m_mainCameraView_Inverse, viewspace[ 1 ] ) ),
            FromHomogeneous( ApplyMat44ToVec4( m_mainCameraView_Inverse, viewspace[ 2 ] ) ),
            FromHomogeneous( ApplyMat44ToVec4( m_mainCameraView_Inverse, viewspace[ 3 ] ) ),
        };

        m_tempverts.clear();
        m_tempverts.assign( verts.begin(), verts.end() );
        for( uint32_t i = 0; i < std::size( worldspace ); i++ )
        {
            // because of m_mainCameraView_Inverse, m_mainCameraProjection_Inverse,
            // vi_world already have ONEGAMEUNIT_IN_METERS applied
            m_tempverts[ i ].position[ 0 ] = worldspace[ i ].data[ 0 ];
            m_tempverts[ i ].position[ 1 ] = worldspace[ i ].data[ 1 ];
            m_tempverts[ i ].position[ 2 ] = worldspace[ i ].data[ 2 ];
        }
        return m_tempverts;
#else

        // treat m_mainCameraView_Inverse as the transform
        const float* t = m_mainCameraView_Inverse;
        
        auto transform = RgTransform{ {
            { t[ 0 ], t[ 4 ], t[ 8 ], t[ 12 ] },
            { t[ 1 ], t[ 5 ], t[ 9 ], t[ 13 ] },
            { t[ 2 ], t[ 6 ], t[ 10 ], t[ 14 ] },
        } };

        m_tempverts.clear();
        m_tempverts.assign( verts.begin(), verts.end() );
        for( uint32_t i = 0; i < std::size( viewspace ); i++ )
        {
            double w = viewspace[ i ].data[ 3 ];
            w        = std::max( w, 0.00000001 );

            // because of m_mainCameraView_Inverse, m_mainCameraProjection_Inverse,
            // vi_world already have ONEGAMEUNIT_IN_METERS applied
            m_tempverts[ i ].position[ 0 ] = float( viewspace[ i ].data[ 0 ] / w );
            m_tempverts[ i ].position[ 1 ] = float( viewspace[ i ].data[ 1 ] / w );
            m_tempverts[ i ].position[ 2 ] = float( viewspace[ i ].data[ 2 ] / w );
        }
        return { transform, m_tempverts };
#endif
    }
    // The funnel every primitive in the game passes through: classifies the draw,
    // resolves its material and hands it to RTGL1. Defined in rt_draw.cpp -- at
    // ~965 lines it was a third of this class on its own.
    void InternalDraw( std::span< const RgPrimitiveVertex > verts,
                       std::span< const uint32_t >          indices,
                       const bool                           isUI,
                       const bool                           islines = false );

public:
    void RT_SetMatrices( const VSMatrix& view, const VSMatrix& proj )
    {
        // TODO: only calculate when UI mode;
        //       can those UI elements be with perspective matrix?

        // clang-format off
        constexpr static float vkcorrection[] = {
            1,  0,    0, 0,
            0, -1,    0, 0,
            0,  0, 0.5f, 0,
            0,  0, 0.5f, 1,
        };
        // clang-format on

        auto correctedProj = VSMatrix::smultMatrix( vkcorrection, proj.get() );
        memcpy( m_projection, correctedProj.get(), sizeof( float ) * 16 );
        memcpy( m_view, view.get(), sizeof( float ) * 16 );
    }

    void RT_AddMainCamera( const FRenderViewpoint& viewpoint )
    {
        const auto [ up, right, forward ] = RT_MakeUpRightForwardVectors( viewpoint.Angles );

        const float pixelstretch =
            viewpoint.ViewLevel ? viewpoint.ViewLevel->info->pixelstretch : 1.0f;

        const auto aspectRatio = r_viewwindow.WidescreenRatio;
        const auto fovRatio    = r_viewwindow.WidescreenRatio >= 1.3f ? 1.333333f : aspectRatio;

        const auto fovy = static_cast< float >(
            2.0 * std::atan( std::tan( viewpoint.FieldOfView.Radians() / 2.0 ) /
                             static_cast< double >( fovRatio ) ) );


        auto readback = RgCameraInfoReadbackEXT{
            .sType = RG_STRUCTURE_TYPE_CAMERA_INFO_READ_BACK_EXT,
        };

        auto info = RgCameraInfo{
            .sType       = RG_STRUCTURE_TYPE_CAMERA_INFO,
            .pNext       = &readback,
            .flags       = 0,
            .position    = { float( viewpoint.Pos.X ) * ONEGAMEUNIT_IN_METERS,
                             float( viewpoint.Pos.Y ) * ONEGAMEUNIT_IN_METERS,
                             float( viewpoint.Pos.Z ) * ONEGAMEUNIT_IN_METERS },
            .up          = up,
            .right       = right,
            .fovYRadians = fovy,
            .aspect      = aspectRatio * pixelstretch,
            .cameraNear  = cvar::rt_znear,
            .cameraFar   = cvar::rt_zfar,
        };

        RgResult r = rt.rgUploadCamera( &info );
        RG_CHECK( r );


        // for first-person weapons
        memcpy( m_mainCameraView_Inverse, readback.viewInverse, 16 * sizeof( float ) );
        memcpy( m_mainCameraProjection_Inverse, readback.projectionInverse, 16 * sizeof( float ) );
        static_assert( sizeof m_mainCameraView_Inverse == sizeof readback.viewInverse );
        static_assert( sizeof m_mainCameraProjection_Inverse == sizeof readback.projectionInverse );


        RT_AddFlashlight( info.position, forward, up, right );
        RT_AddMuzzleFlash( viewpoint.ViewActor, viewpoint.extralight, info.position, forward, up );
        RT_AddWeaponGlow( viewpoint.camera, info.position, forward, up );
    }
    // First-person weapon lighting -- the flashlight, the muzzle flash and the
    // gun's own glow. Defined in rt_weapon.cpp: ~770 lines of a feature that is
    // about the PLAYER, where the rest of this class is about the scene.
    void RT_AddFlashlight( const RgFloat3D& basePosition,
                           const RgFloat3D& forward,
                           const RgFloat3D& up,
                           const RgFloat3D& right );

    static float cvarcolor_luma( const FColorCVarRef& c );

    struct MuzzleTint
    {
        RgColor4DPacked32 color;
        // rt_mzlflsh_intensity is a multiplier on the COLOUR, so a saturated hue carries
        // less light than the warm default at the same number: measured against
        // ff8c52 (luma 160), plasma 3355ff is 0.56x, unmaker ff1111 only 0.42x. Left
        // uncompensated, retinting the flash silently dimmed it — "the muzzle flash is
        // barely visible anymore". This scales intensity back so changing the hue does
        // not change how bright the flash reads. Clamped so an extreme colour cannot
        // blow the exposure.
        float intensityScale;
    };

    MuzzleTint MuzzleFlashTintFor( AActor* viewactor ) const;

    void RT_AddMuzzleFlash( AActor*          viewactor,
                            int              extralight,
                            const RgFloat3D& basePosition,
                            const RgFloat3D& forward,
                            const RgFloat3D& up );

    void RT_AddWeaponGlow( AActor*          camera,
                           const RgFloat3D& basePosition,
                           const RgFloat3D& forward,
                           const RgFloat3D& up );
private:
    RgViewport m_viewport{};
    float      m_view[ 16 ]{};
    float      m_projection[ 16 ]{};

    float m_mainCameraView_Inverse[ 16 ]{};
    float m_mainCameraProjection_Inverse[ 16 ]{};

    uint32_t m_weaponDrawCallIndex{ 0 }; // to z-sort weapon sprites

    // Where the plasma rifle's quad actually is, in VIEW space metres (origin = eye),
    // captured as it is uploaded. RT_AddWeaponGlow anchors the core light to this
    // instead of a guessed offset from the camera: the quad sits centimetres from the
    // eye, so the old 0.7m-forward placement put the light PAST the gun and lit its back
    // face, which is why it lit the room but never the sprite. Anchoring also gives the
    // weapon bob for free — it is the real geometry, not an approximation of it.
    FVector3 m_gunAnchorView{ 0, 0, 0 };
    bool     m_haveGunAnchor{ false };

    std::vector< RgPrimitiveVertex > m_tempverts{};

public:
    RTFrameBuffer* m_fb{ nullptr };
};

} // namespace rtx
