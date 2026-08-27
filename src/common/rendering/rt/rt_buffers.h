#pragma once

// The GPU-resource classes behind FRenderState: gzdoom's IVertexBuffer /
// IIndexBuffer / IHardwareTexture, implemented over RTGL1's upload calls.
//
// Pure engine glue, and the only part of the renderer with no Doom64-RT
// behaviour in it at all -- which is exactly why it is worth having out of the
// way of the code that does. Split out of rt_main.cpp; behaviour unchanged.
//
// RTDataBuffer is NOT here: it reaches into RTRenderState to fetch the viewpoint
// matrices, so it has to be defined after that class.

#include "rt_internal.h"

#include "hw_renderstate.h"
#include "i_modelvertexbuffer.h"
#include "image.h"
#include "flatvertices.h"
// GPalette / TranslationToTable / IsLuminosityTranslation, for the translated
// material names in MakeTextureName.
#include "palettecontainer.h"
// FSkyVertex: the sky dome hands the vertex buffer its own vertex type, which is
// why the buffer holds a variant rather than one layout.
#include "hw_skydome.h"

#include <variant>

namespace rtx
{



class VectorAsBuffer : virtual public IBuffer
{
public:
    ~VectorAsBuffer() override = default;

    void SetSubData( size_t offset, size_t size, const void* data ) override
    {
        if( offset + size > m_buffer.size() )
        {
            m_buffer.resize( offset + size );
        }

        if( data )
        {
            memcpy( &m_buffer[ offset ], data, size );
        }

        buffersize = m_buffer.size();
        if( map )
        {
            map = m_buffer.data();
        }
    }
    void SetData( size_t size, const void* data, BufferUsageType type ) override
    {
        SetSubData( 0, size, data );
    }
    void* Lock( unsigned size ) override
    {
        SetSubData( 0, size, nullptr );
        return m_buffer.data();
    }
    void Unlock() override {}
    void Resize( size_t newsize ) override { m_buffer.resize( newsize ); }
    void Upload( size_t start, size_t size ) override {}
    void Map() override { map = m_buffer.data(); }
    void Unmap() override { map = nullptr; }
    void GPUDropSync() override {}
    void GPUWaitSync() override {}

protected:
    auto AccessBuffer() const { return std::span{ m_buffer }; }

private:
    std::vector< uint8_t > m_buffer;
};

class RTVertexBuffer
    : public IVertexBuffer
    , public VectorAsBuffer
{
    using Super            = VectorAsBuffer;
    using VertexTypeHolder = std::
        variant< std::monostate, FSkyVertex, FModelVertex, FFlatVertex, F2DDrawer::TwoDVertex >;

public:
    void SetFormat( int                           numBindingPoints,
                    int                           numAttributes,
                    size_t                        stride,
                    const FVertexBufferAttribute* attrs ) override
    {
        static_assert( sizeof( FSkyVertex ) != sizeof( FModelVertex ) );
        static_assert( sizeof( FSkyVertex ) != sizeof( FFlatVertex ) );
        static_assert( sizeof( FSkyVertex ) != sizeof( F2DDrawer::TwoDVertex ) );
        static_assert( sizeof( FModelVertex ) != sizeof( FFlatVertex ) );
        static_assert( sizeof( FModelVertex ) != sizeof( F2DDrawer::TwoDVertex ) );
        static_assert( sizeof( FFlatVertex ) != sizeof( F2DDrawer::TwoDVertex ) );

        if( numBindingPoints == 1 && numAttributes == 4 && stride == sizeof( FSkyVertex ) )
        {
            m_vertextype = FSkyVertex{};
        }
        else if( numBindingPoints == 2 && numAttributes == 8 && stride == sizeof( FModelVertex ) )
        {
            m_vertextype = FModelVertex{};
        }
        else if( numBindingPoints == 1 && numAttributes == 3 && stride == sizeof( FFlatVertex ) )
        {
            m_vertextype = FFlatVertex{};
        }
        else if( numBindingPoints == 1 && numAttributes == 3 &&
                 stride == sizeof( F2DDrawer::TwoDVertex ) )
        {
            m_vertextype = F2DDrawer::TwoDVertex{};
        }
        else
        {
            assert( 0 );
            m_vertextype = std::monostate{};
        }
        m_formatted.clear();
    }

    static void MakeFormatted( std::vector< RgPrimitiveVertex >& dst,
                               size_t                            targetCount,
                               std::span< const uint8_t >        srcbuf,
                               const VertexTypeHolder&           vertextype )
    {
        // TODO: mStreamData.uVertexColor for lightstyled?


        static auto gz_unpacknormal_x = []( uint32_t packedNormal ) -> float {
            int inx = ( packedNormal & 1023 );
            return float( inx ) / 512.0f;
        };
        static auto gz_unpacknormal_y = []( uint32_t packedNormal ) -> float {
            int iny = ( ( packedNormal >> 10 ) & 1023 );
            return float( iny ) / 512.0f;
        };
        static auto gz_unpacknormal_z = []( uint32_t packedNormal ) -> float {
            int inz = ( ( packedNormal >> 20 ) & 1023 );
            return float( inz ) / 512.0f;
        };

        static auto rg_packednormal_fallback = rt.rgUtilPackNormal( 0, 1, 0 );

        // make by type
        std::visit(
            [ & ]< typename T >( const T& ) {
                assert( srcbuf.size_bytes() % sizeof( T ) == 0 );

                dst.reserve( targetCount );
                for( size_t i = dst.size(); i < targetCount; i++ )
                {
                    static_assert( sizeof( decltype( srcbuf )::value_type ) == 1 );
                    const auto* ptr = &srcbuf[ i * sizeof( T ) ];

                    if constexpr( std::is_same_v< T, FSkyVertex > )
                    {
                        auto src = reinterpret_cast< const FSkyVertex* >( ptr );

                        dst.push_back( RgPrimitiveVertex{
                            .position     = { src->x * ONEGAMEUNIT_IN_METERS,
                                              src->y * ONEGAMEUNIT_IN_METERS,
                                              src->z * ONEGAMEUNIT_IN_METERS },
                            .normalPacked = rg_packednormal_fallback,
                            .texCoord     = { src->u, src->v },
                            .color        = rtcolor( src->color ),
                        } );
                    }
                    else if constexpr( std::is_same_v< T, FModelVertex > )
                    {
                        auto src = reinterpret_cast< const FModelVertex* >( ptr );

                        dst.push_back( RgPrimitiveVertex{
                            .position = { src->x * ONEGAMEUNIT_IN_METERS,
                                          src->y * ONEGAMEUNIT_IN_METERS,
                                          src->z * ONEGAMEUNIT_IN_METERS },
                            .normalPacked =
                                rt.rgUtilPackNormal( gz_unpacknormal_x( src->packedNormal ),
                                                     gz_unpacknormal_y( src->packedNormal ),
                                                     gz_unpacknormal_z( src->packedNormal ) ),
                            .texCoord = { src->u, src->v },
                            .color    = RG_PACKED_COLOR_WHITE,
                        } );
                    }
                    else if constexpr( std::is_same_v< T, FFlatVertex > )
                    {
                        auto src = reinterpret_cast< const FFlatVertex* >( ptr );

                        dst.push_back( RgPrimitiveVertex{
                            .position     = { src->x * ONEGAMEUNIT_IN_METERS,
                                              src->y * ONEGAMEUNIT_IN_METERS,
                                              src->z * ONEGAMEUNIT_IN_METERS },
                            .normalPacked = rg_packednormal_fallback,
                            .texCoord     = { src->u, src->v },
                            .color        = RG_PACKED_COLOR_WHITE,
                        } );
                    }
                    else if constexpr( std::is_same_v< T, F2DDrawer::TwoDVertex > )
                    {
                        auto src = reinterpret_cast< const F2DDrawer::TwoDVertex* >( ptr );

                        dst.push_back( RgPrimitiveVertex{
                            .position     = { src->x, src->y, src->z },
                            .normalPacked = rg_packednormal_fallback,
                            .texCoord     = { src->u, src->v },
                            .color        = rtcolor_bgr_alphagamma( src->color0 ),
                        } );
                    }
                    else
                    {
                        assert( 0 );
                    }
                }
            },
            vertextype );
    }

    auto AccessFormatted( uint32_t first, uint32_t count ) -> std::span< const RgPrimitiveVertex >
    {
        if( std::holds_alternative< std::monostate >( m_vertextype ) )
        {
            return {};
        }

        // Doom64-RT: never format past the end of the source buffer. The index
        // path derives `first`/`count` from whatever indices it read; a bad index
        // (see DrawIndexed) used to send MakeFormatted on a reserve()+loop to
        // 0xFFFFFFFF on the main thread -- a freeze, not a crash. Clamp here, and
        // return an empty span for anything that does not fit, so the caller draws
        // nothing instead of hanging.
        {
            const size_t stride = std::visit(
                []< typename T >( const T& ) -> size_t {
                    if constexpr( std::is_same_v< T, std::monostate > )
                    {
                        return 0;
                    }
                    else
                    {
                        return sizeof( T );
                    }
                },
                m_vertextype );
            const size_t available = stride ? AccessBuffer().size_bytes() / stride : 0;
            if( size_t( first ) + size_t( count ) > available )
            {
                static bool warned = false;
                if( !warned )
                {
                    warned = true;
                    Printf( PRINT_HIGH,
                            "RT: vertex range [%u, +%u) exceeds buffer (%zu), draw skipped\n",
                            first,
                            count,
                            available );
                }
                return {};
            }
        }

        if( first + count > m_formatted.size() )
        {
            MakeFormatted( m_formatted, first + count, AccessBuffer(), m_vertextype );
        }

        assert( first + count <= m_formatted.size() );

        return std::span{
            &m_formatted[ first ],
            count,
        };
    }

    void SetData( size_t size, const void* data, BufferUsageType type ) override
    {
        m_formatted.clear();
        Super::SetData( size, data, type );
    }

    void SetSubData( size_t offset, size_t size, const void* data ) override
    {
        m_formatted.clear();
        Super::SetSubData( offset, size, data );
    }

    void Unmap() override
    {
        m_formatted.clear();
        Super::Unmap();
    }

    bool IsSky() const { return std::holds_alternative< FSkyVertex >( m_vertextype ); }
    bool IsUI() const { return std::holds_alternative< F2DDrawer::TwoDVertex >( m_vertextype ); }

private:
    VertexTypeHolder m_vertextype;

    std::vector< RgPrimitiveVertex > m_formatted;
};

class RTIndexBuffer
    : public IIndexBuffer
    , public VectorAsBuffer
{
    using IndexType = uint32_t;

public:
    auto AccessFormatted( uint32_t first, uint32_t count )
    {
        const auto rawbuf = AccessBuffer();
        // loose type check
        assert( rawbuf.size_bytes() % sizeof( IndexType ) == 0 );
        // alignment
        assert( uint64_t( rawbuf.data() ) % sizeof( IndexType ) == 0 );
        // overflow
        assert( sizeof( IndexType ) * ( first + count ) <= rawbuf.size_bytes() );

        return std::span{
            reinterpret_cast< const IndexType* >( rawbuf.data() ) + first,
            count,
        };
    }

    static auto CalcFirstVertexAndVertexCount( std::span< const IndexType > indices )
    {
        uint32_t imin = std::numeric_limits< uint32_t >::max();
        uint32_t imax = std::numeric_limits< uint32_t >::lowest();
        for( const auto& i : indices )
        {
            imin = std::min( imin, i );
            imax = std::max( imax, i );
        }
        return std::pair{
            imax > imin ? imin : 0,
            imax > imin ? imax - imin + 1 : 0,
        };
    }

    auto MakeWithNewFirstIndex( std::span< const IndexType > indices, IndexType newFirst )
    {
        m_cache.clear();
        m_cache.reserve( indices.size() );

        for( const auto& i : indices )
        {
            assert( i >= newFirst );
            m_cache.push_back( i - newFirst );
        }

        return m_cache;
    }

private:
    std::vector< IndexType > m_cache;
};



class RTHardwareTexture : public IHardwareTexture
{
public:
    // Empty, as it's only used for software renderer
    uint32_t CreateTexture( uint8_t*, int, int, int, bool, const char* ) override { return 0; }
    void     AllocateBuffer( int, int, int ) override {}
    uint8_t* MapBuffer() override { return nullptr; }

    void CreateIfWasnt( FGameTexture&       src,
                        int                 clampmode,
                        int                 translation,
                        int                 flags,
                        const FRenderStyle& renderStyle )
    {
        auto rtclamp_x = []( int clampmode ) {
            switch( clampmode )
            {
                case CLAMP_X:
                case CLAMP_XY:
                case CLAMP_XY_NOMIP:
                case CLAMP_NOFILTER_X:
                case CLAMP_NOFILTER_XY:
                case CLAMP_CAMTEX: return RG_SAMPLER_ADDRESS_MODE_CLAMP;
                default: return RG_SAMPLER_ADDRESS_MODE_REPEAT;
            }
        };
        auto rtclamp_y = []( int clampmode ) {
            switch( clampmode )
            {
                case CLAMP_Y:
                case CLAMP_XY:
                case CLAMP_XY_NOMIP:
                case CLAMP_NOFILTER_Y:
                case CLAMP_NOFILTER_XY:
                case CLAMP_CAMTEX: return RG_SAMPLER_ADDRESS_MODE_CLAMP;
                default: return RG_SAMPLER_ADDRESS_MODE_REPEAT;
            }
        };
        auto desaturateIfNeed = []( FTextureBuffer& data, int flags, const char* lumpname ) {
            // special case for the SmallFont...
            const bool isSTCFNFont = !( flags & CTF_Indexed ) && lumpname &&
                                     strlen( lumpname ) == 8 &&
                                     strncmp( lumpname, "STCFN", 5 ) == 0;
            if( isSTCFNFont )
            {
                for( int i = 0; i < data.mWidth; i++ )
                {
                    for( int j = 0; j < data.mHeight; j++ )
                    {
                        uint8_t* pix =
                            &data.mBuffer[ 4 *
                                           ( i * static_cast< uint64_t >( data.mHeight ) + j ) ];
                        const uint8_t gray = std::max( pix[ 0 ], std::max( pix[ 1 ], pix[ 2 ] ) );
                        pix[ 0 ] = pix[ 1 ] = pix[ 2 ] = gray;
                    }
                }
            }
        };
        auto calculateAlphaIfNeed = []( FTextureBuffer& data, bool redIsAlpha ) {
            if( redIsAlpha )
            {
                for( int i = 0; i < data.mWidth; i++ )
                {
                    for( int j = 0; j < data.mHeight; j++ )
                    {
                        uint8_t* pix =
                            &data.mBuffer[ 4 *
                                           ( i * static_cast< uint64_t >( data.mHeight ) + j ) ];

                        // alpha = red
                        pix[ 3 ] = pix[ 0 ];
                    }
                }
            }
        };
        // Doom64-RT: RGBA PNGs are always Masked. Soft garbage alpha hole-punches solid
        // walls; real fences have many low-alpha pixels. Heuristic: if <8% of pixels are
        // "see-through" (A<32), force opaque; otherwise keep alpha for fences/grates.
        auto forceOpaqueAlphaIfNeed = []( FTextureBuffer& data, bool forceOpaque ) {
            if( !forceOpaque || !data.mBuffer || data.mWidth <= 0 || data.mHeight <= 0 )
            {
                return;
            }
            const size_t n =
                size_t( data.mWidth ) * size_t( data.mHeight );
            for( size_t i = 0; i < n; i++ )
            {
                data.mBuffer[ 4 * i + 3 ] = 255;
            }
        };
        auto looksLikeRealMask = []( FTextureBuffer& data ) -> bool {
            if( !data.mBuffer || data.mWidth <= 0 || data.mHeight <= 0 )
            {
                return false;
            }
            const size_t n = size_t( data.mWidth ) * size_t( data.mHeight );
            size_t       holes = 0;
            for( size_t i = 0; i < n; i++ )
            {
                if( data.mBuffer[ 4 * i + 3 ] < 32 )
                {
                    holes++;
                }
            }
            return holes * 100 >= n * 8; // >= 8% transparent-ish pixels
        };

        if( m_created )
        {
            return;
        }

        m_created = true;
        m_name    = MakeTextureName( src, translation );

        if( m_name.empty() || !src.GetTexture() )
        {
            assert( 0 );
            return;
        }

        auto texbuffer = src.GetTexture()->CreateTexBuffer( translation, flags | CTF_ProcessData );
        desaturateIfNeed( texbuffer, flags, fileSystem.GetFileShortName( src.GetSourceLump() ) );
        calculateAlphaIfNeed( texbuffer, renderStyle.Flags & STYLEF_RedIsAlpha );
        {
            const auto use = src.GetUseType();
            const bool keepAlpha = ( renderStyle.Flags & STYLEF_RedIsAlpha ) ||
                                   use == ETextureType::Sprite ||
                                   use == ETextureType::FontChar ||
                                   use == ETextureType::SkinSprite ||
                                   looksLikeRealMask( texbuffer );
            forceOpaqueAlphaIfNeed( texbuffer, rt_mod_compat != 0 && !keepAlpha );
        }

        if( texbuffer.mWidth <= 0 || texbuffer.mHeight <= 0 )
        {
            assert( 0 );
            return;
        }

        const bool exportseparately = m_name.starts_with( "vx_" );

        // Doom64-RT: the sky is the one place NEAREST filtering is wrong.
        //
        // rt_smoothtextures is off by default and pinned off, which is right for
        // the game -- Doom's art is 64x64 pixel work and wants crisp texels. The
        // sky is the opposite case: the cloud deck stretches a 1024px slice
        // across a disc that fills the upper hemisphere, so near the zenith one
        // texel covers a large angular area and NEAREST turns a soft cloud into
        // visible square blocks. That is what "the clouds look low quality"
        // actually was.
        //
        // Per-texture, not global: RTGL1's SamplerManager only rebinds the
        // dynamic filter for handles created with RG_SAMPLER_FILTER_AUTO
        // (hasDynamicSamplerFilter), so naming a filter explicitly here opts
        // this texture out of rt_smoothtextures and leaves everything else
        // exactly as it was.
        const bool smoothsky = m_name.starts_with( "CLOUDV" ) || //
                               m_name.starts_with( "BOLT" ) ||   //
                               m_name == "MOONDISC";

        auto details = RgOriginalTextureDetailsEXT{
            .sType  = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_DETAILS_EXT,
            .pNext  = nullptr,
            .flags  = exportseparately ? RG_ORIGINAL_TEXTURE_INFO_FORCE_EXPORT_AS_EXTERNAL : 0u,
            .format = flags & CTF_Indexed ? RG_FORMAT_R8_SRGB : RG_FORMAT_B8G8R8A8_SRGB,
        };

        auto info = RgOriginalTextureInfo{
            .sType        = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO,
            .pNext        = &details,
            .pTextureName = m_name.c_str(),
            .pPixels      = texbuffer.mBuffer,
            .size         = { static_cast< uint32_t >( texbuffer.mWidth ),
                              static_cast< uint32_t >( texbuffer.mHeight ) },
            .filter       = smoothsky ? RG_SAMPLER_FILTER_LINEAR : RG_SAMPLER_FILTER_AUTO,
            .addressModeU = RG_SAMPLER_ADDRESS_MODE_REPEAT, //  rtclamp_x( clampmode ),
            .addressModeV = RG_SAMPLER_ADDRESS_MODE_REPEAT, //  rtclamp_y( clampmode ),
        };

        RgResult r = rt.rgProvideOriginalTexture( &info );
        RG_CHECK( r );
    }

    ~RTHardwareTexture() override
    {
// HACKHACK: TODO: why this is being called only on Release? (and destroying actually used textures)
#if 0
        RgResult r = rt.rgMarkOriginalTextureAsDeleted( m_name.c_str() );
        RG_CHECK( r );
#endif
    }

    auto GetRTName() const -> const char*
    {
        return m_created && !m_name.empty() ? m_name.c_str() : nullptr;
    }

private:
    // Doom64-RT: the suffix that makes a PALETTE-TRANSLATED texture its own
    // RTGL1 material.
    //
    // Without it, every translation of a sprite is uploaded under the same name
    // and RTGL1 keeps only the first (PreferExistingMaterials in its
    // TextureManager: "Material with the same name already exists, ignoring new
    // data"). GZDoom does the remap correctly -- it allocates a separate
    // hardware texture per translation and CreateTexBuffer produces the right
    // pixels -- and then the upload is silently dropped. That is why a monster's
    // DECORATE BloodColor never rendered: BLUDA0 was already taken by the red
    // one. Worse, WHICH one wins is upload order, so a session where a
    // purple-blooded monster bled first would tint every other monster's blood.
    //
    // The index comes from GPalette, normalised exactly the way gzdoom's own
    // FHardwareTextureContainer::GetTexID does (hw_texcontainer.h), so equal
    // translations collapse onto one name. FRemapTable::Index is a global,
    // CRC-deduplicated index into uniqueRemaps, so two different translations
    // can never land on the same suffix.
    //
    // Kept to filesystem-safe characters on purpose: RTGL1 uses this name as a
    // FILE STEM when it looks for material overrides (<ovrd>/<name>_e.ktx2 and
    // friends), so anything exotic here becomes a bad path.
    static auto TranslationSuffix( int translation ) -> std::string
    {
        if( !cvar::rt_tex_translations || translation <= 0 )
        {
            return {};
        }

        if( IsLuminosityTranslation( translation ) )
        {
            // Only the colour range identifies these; same packing as GetTexID.
            return "_tl" + std::to_string( ( translation >> 16 ) & 0x3fff );
        }

        const FRemapTable* remap = GPalette.TranslationToTable( translation );
        const int          index = remap == nullptr ? 0 : remap->Index;

        // Index 0 is the identity table -- not a translation at all, and it must
        // keep the untranslated name or it would fork every texture in the game.
        return index == 0 ? std::string{} : "_tr" + std::to_string( index );
    }

    static auto MakeTextureName( FGameTexture& fgametex, int translation ) -> std::string
    {
        const std::string suffix = TranslationSuffix( translation );

        // highest priority: FGameTexture name
        if( !fgametex.GetName().IsEmpty() )
        {
            std::string name = fgametex.GetName().GetChars();
            if( !suffix.empty() )
            {
                if( cvar::rt_tex_translations_debug )
                {
                    Printf( "rt_tex_translations: %s  translation %d  ->  %s%s\n",
                            name.c_str(),
                            translation,
                            name.c_str(),
                            suffix.c_str() );
                }
                name += suffix;
            }
            return name;
        }

        // if no lump name, stringify the image ID;
        // this is undesirable for textures that require a replacement
        // (which are found by texname; and because ID is assigned at runtime,
        // replacements can't be found correctly)
        if( FTexture* ftex = fgametex.GetTexture() )
        {
            if( FImageSource* imgsrc = ftex->GetImage() )
            {
                // MSVC's std::string has 16 chars inlined,
                // so no allocation should happen
                return std::to_string( imgsrc->GetId() ) + suffix;
            }
        }

        assert( 0 );
        return {};
    }

private:
    bool        m_created{ false };
    std::string m_name{};
};

} // namespace rtx
