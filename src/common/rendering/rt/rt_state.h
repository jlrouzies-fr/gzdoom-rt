#pragma once

#if HAVE_RT


    #include "unordered_dense/include/ankerl/unordered_dense.h"

    #ifndef NDEBUG
        #define RT_DEBUG_UNIQUEID 1
    #else
        #define RT_DEBUG_UNIQUEID 0
    #endif


    #include "defer.h"
    #include "palentry.h"
    #include "rt_helpers.h"

    #include <optional>


struct FRtState;

namespace detail
{
template< typename Payload, typename DestroyFunc >
struct AutoPop
{
    explicit AutoPop( Payload pl, DestroyFunc&& func, FRtState& st )
        : m_st{ st }, m_payload{ pl }, m_func{ func }
    {
    }

    ~AutoPop() { m_func( m_st, m_payload ); }

    AutoPop( const AutoPop& )                = delete;
    AutoPop( AutoPop&& ) noexcept            = delete;
    AutoPop& operator=( const AutoPop& )     = delete;
    AutoPop& operator=( AutoPop&& ) noexcept = delete;

private:
    FRtState&   m_st;
    Payload     m_payload;
    DestroyFunc m_func;
};
} // namespace detail



enum class RtPrim : uint32_t
{
    Identity            = 0,
    Ignored             = 1 << 0,
    FirstPerson         = 1 << 1,
    FirstPersonViewer   = 1 << 2,
    Sky                 = 1 << 3,
    SkyVisibility       = 1 << 4,
    Decal               = 1 << 5,
    Particle            = 1 << 6,
    Mirror              = 1 << 7,
    Glass               = 1 << 8,
    ExportMap           = 1 << 9,
    ExportInstance      = 1 << 10,
    ExportInvertNormals = 1 << 11,
    NoMotionVectors     = 1 << 12,
    // "this sector plane got real bulb-lattice lights this frame". Pushed by
    // HWFlat::DrawFlat, which is the only place that knows both the sector and which
    // plane is being drawn.
    // Doom 64 hangs the SAME texture on both: SFLATAQ is a lamp ceiling in one place
    // and a wall light strip in another (MAP02 alone: 33 ceilings, 18 floors, 30 wall
    // faces). emissiveMult lives on the TEXTURE, so it cannot tell those apart -- which
    // is how switching the lamp ceilings to real point lights also put out the wall
    // strips in MAP02's bloom room. This flag is what lets rt_draw suppress the glow on
    // the planes that now have real lights, and leave the wall strips glowing.
    LatticeLitFlat      = 1 << 13,
};

enum class RtManyPrimsPerId : uint32_t
{
    No,
    Set0,
    Set1,
};

struct FRtState
{
    FRtState()                                       = default;
    ~FRtState()                                      = default;
    FRtState( const FRtState& other )                = delete;
    FRtState( FRtState&& other ) noexcept            = delete;
    FRtState& operator=( const FRtState& other )     = delete;
    FRtState& operator=( FRtState&& other ) noexcept = delete;

    void reset()
    {
        m_uniqueIdToLastPrimIndex_0.clear();
        m_uniqueIdToLastPrimIndex_1.clear();
    #if RT_DEBUG_UNIQUEID
        m_seenUniqueIds.clear();
    #endif
    }

    //

    [[nodiscard]] auto push_type( RtPrim s )
    {
        m_state |= uint32_t( s );

        return detail::AutoPop{
            uint32_t( s ), // payload
            []( FRtState& fthis, uint32_t s ) {
                assert( ( fthis.m_state & s ) || ( s == uint32_t( RtPrim::Identity ) ) );
                fthis.m_state &= ~s;
            },
            *this,
        };
    }

    // Deliberately separate from m_lightlevel: that one is sprite-only and feeds
    // RgMeshInfo::localLightsIntensity for every primitive. Writing world lightlevel
    // into it would silently change local-light response on all map geometry.
    [[nodiscard]] auto push_sectorlight( const PalEntry& lightcolor, int lightlevel )
    {
        std::pair< FVector3, int > prev{ m_sectorLightColor, m_sectorLightLevel };

        m_sectorLightColor = FVector3{ lightcolor.r / 255.0f,
                                       lightcolor.g / 255.0f,
                                       lightcolor.b / 255.0f };
        m_sectorLightLevel = lightlevel;

        return detail::AutoPop{
            prev, // payload
            []( FRtState& fthis, std::pair< FVector3, int > p ) {
                fthis.m_sectorLightColor = p.first;
                fthis.m_sectorLightLevel = p.second;
            },
            *this,
        };
    }

    template< RtPrim Value >
        requires( Value != RtPrim::Identity )
    [[nodiscard]] bool is() const
    {
        // assert( m_curUniqueID != 0 );
        return m_state & uint32_t( Value );
    }

    //

    template< RtManyPrimsPerId Seq = RtManyPrimsPerId::No >
    [[nodiscard]] auto push_uniqueid( uint64_t id )
    {
        assert( id != 0 );
        assert( m_curUniqueID == 0 );
        assert( m_curPrimitiveIndex == 0 );
    #if RT_DEBUG_UNIQUEID
        if( Seq == RtManyPrimsPerId::No )
        {
            if( !( m_state & uint32_t( RtPrim::Ignored ) ) )
            {
                auto [ iter, isnew ] = m_seenUniqueIds.insert( id );
                assert( isnew );
            }
        }
    #endif

        m_curUniqueID       = id;
        m_curPrimitiveIndex = ( Seq == RtManyPrimsPerId::Set0 ) ? m_uniqueIdToLastPrimIndex_0[ id ]
                              : ( Seq == RtManyPrimsPerId::Set1 )
                                  ? m_uniqueIdToLastPrimIndex_1[ id ]
                                  : 0;

        return detail::AutoPop{
            id, // payload
            []( FRtState& fthis, uint64_t id ) {
                assert( fthis.m_curUniqueID == id );

                if( Seq == RtManyPrimsPerId::Set0 )
                {
                    fthis.m_uniqueIdToLastPrimIndex_0[ fthis.m_curUniqueID ] =
                        fthis.m_curPrimitiveIndex;
                }
                else if( Seq == RtManyPrimsPerId::Set1 )
                {
                    fthis.m_uniqueIdToLastPrimIndex_1[ fthis.m_curUniqueID ] =
                        fthis.m_curPrimitiveIndex;
                }
                fthis.m_curUniqueID       = 0;
                fthis.m_curPrimitiveIndex = 0;
            },
            *this,
        };
    }

    template< RtManyPrimsPerId Seq = RtManyPrimsPerId::No, typename T >
        requires( sizeof( T* ) == sizeof( uint64_t ) && !std::is_pointer_v< T > )
    [[nodiscard]] auto push_uniqueid( const T* obj, uint64_t salt = 0 )
    {
        return push_uniqueid< Seq >( reinterpret_cast< uint64_t >( obj ) + salt );
    }

    [[nodiscard]] auto get_uniqueid() const
    {
        assert( m_curUniqueID != 0 );
        return m_curUniqueID;
    }

    //

    [[nodiscard]] auto next_primitiveindex()
    {
        assert( m_curUniqueID != 0 );
        return m_curPrimitiveIndex++;
    }

    //

    #define RT_MAX_SPRITE_FRAMES 29
    [[nodiscard]] auto push_exportinstance_name( const char* spritename, uint8_t animframe )
    {
        if( spritename )
        {
            assert( spritename[ 0 ] != '\0' && spritename[ 1 ] != '\0' && spritename[ 2 ] != '\0' &&
                    spritename[ 3 ] != '\0' );
            assert( animframe < RT_MAX_SPRITE_FRAMES );
        }
        assert( m_curExportInstanceName[ 0 ] == '\0' );

        m_curExportInstanceName[ 0 ] = spritename ? spritename[ 0 ] : '\0';
        m_curExportInstanceName[ 1 ] = spritename ? spritename[ 1 ] : '\0';
        m_curExportInstanceName[ 2 ] = spritename ? spritename[ 2 ] : '\0';
        m_curExportInstanceName[ 3 ] = spritename ? spritename[ 3 ] : '\0';
        m_curExportInstanceName[ 4 ] = spritename ? 'A' + animframe : '\0';
        m_curExportInstanceName[ 5 ] = '\0';

        return detail::AutoPop{ 0,
                                []( FRtState& fthis, int ) {
                                    // empty on pop
                                    fthis.m_curExportInstanceName[ 0 ] = '\0';
                                },
                                *this };
    }

    [[nodiscard]] auto get_exportinstance_name() -> const char*
    {
        return m_curExportInstanceName[ 0 ] == '\0' ? nullptr : m_curExportInstanceName;
    }

    //

    [[nodiscard]] auto push_apply_spriterotation( float pitch, float yaw )
    {
        assert( !m_spriteRotation );
        m_spriteRotation = std::pair{ pitch, yaw };
        return detail::AutoPop{ 0,
                                []( FRtState& fthis, int ) {
                                    // empty on pop
                                    fthis.m_spriteRotation = {};
                                },
                                *this };
    }

    [[nodiscard]] auto get_spriterotation()
    {
        assert( m_spriteRotation );
        return m_spriteRotation.value_or( std::pair{ 0.f, 0.f } );
    }

    //

    FVector3 m_lastthingposition{};
    // The actor's collision radius in MAP UNITS, for sizing sprite shadow
    // proxies (rt_sprite_shadow). A sprite's quad is the art's canvas, which is
    // far wider than the body it draws -- a marine is 64 units of sprite around
    // a radius of 17 -- so a proxy built at the quad's width sticks out toward
    // the viewer about twice as far as the actor really does. 0 = unknown.
    float    m_lastthingradius{ 0.f };

    // Is this actor a STANDING body, or something lying on the floor?
    //
    // A shadow proxy is an assumption about 3D shape, and "a vertical cross"
    // only fits an upright thing. A corpse, a gib or a dropped weapon is
    // essentially a flat plate on the ground; standing four vertical cards up
    // through one gives radiating shadow spokes that swing with the light
    // (screen/shadowissue.png). The distinction is in the playsim, not in the
    // renderer -- gzdoom quarters an actor's Height when it dies, and pickups
    // are short by definition -- so it is decided in hw_sprites.cpp where the
    // actor is in hand, and the renderer only honours the answer.
    bool     m_lastthingupright{ false };

    // Is it a LIVE MONSTER? The narrowest scope for shadow proxies, and the
    // shipping one (rt_sprite_shadow_scope 1). Everything the proxies were built
    // for is here -- an enemy is the thing the eye tracks and the thing whose
    // missing shadow reads as "pasted onto the floor" -- while every class the
    // assumption fits badly is outside it by definition.
    bool     m_lastthinglivemonster{ false };

    // The FLOOR under this actor, in MAP UNITS (actor->floorz), for the contact
    // occlusion blob (rt_sprite_ao). Not derivable in the renderer: the sprite's
    // own quad starts at the actor's Z, which is where the actor IS, not what it
    // is standing on -- a flying enemy, a jumping one and an item on a ledge all
    // need the surface below them, and only the playsim has traced it.
    //
    // NaN-free sentinel: equal to m_lastthingposition.Z when unknown, so the
    // blob's height fade reads "on the ground" rather than "infinitely high".
    float    m_lastthingfloorz{ 0.f };
    // Is this thing a candidate for a contact blob at all? False for the player's
    // own viewer sprite and for particles, which have no floor to sit on.
    bool     m_lastthinghasfloor{ false };

    uint8_t  m_berserkBlend{ 0 };

    int m_lightlevel{ 255 };

    // Doom 64 per-sector colormap color + lightlevel for the world primitive being
    // drawn. Written only by walls/flats, so they must be scoped: left as plain
    // assignments they stay set after the flat is done and leak the last sector's
    // values onto any later world geometry that doesn't set them. Use
    // push_sectorlight(). Lightlevel defaults to 0 so anything that never sets it
    // (sprites, sky, portals) is treated as non-emissive.
    FVector3 m_sectorLightColor{ 1.0, 1.0, 1.0 };
    int      m_sectorLightLevel{ 0 };

private:
    uint32_t m_state{ 0 };             // RtPrim flags
    uint64_t m_curUniqueID{ 0 };       // to identify an object between frames
    uint32_t m_curPrimitiveIndex{ 0 }; // index of a part in the current object

    char m_curExportInstanceName[ 6 ]; // to assign names to exportable meshes
                                       // char 0-4: sprite name
                                       // char   5: animation index ['A'..'A'+MAX_SPRITE_FRAMES)

    std::optional< std::pair< float, float > > m_spriteRotation{};

    ankerl::unordered_dense::map< uint64_t, uint32_t > m_uniqueIdToLastPrimIndex_0{};
    ankerl::unordered_dense::map< uint64_t, uint32_t > m_uniqueIdToLastPrimIndex_1{};

    #if RT_DEBUG_UNIQUEID
    ankerl::unordered_dense::set< uint64_t > m_seenUniqueIds{};
    #endif
};



extern FRtState rtstate;

#endif
