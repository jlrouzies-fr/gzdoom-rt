// Fullscreen images, the map title cards and the fluid spawner.
//
// The title card is drawn by RT rather than by the 2D layer so it can fade with
// the same clock as the rest of the frame; RT_InjectTitleIntoDoomMap is what
// puts a Doom 64 map's name on screen at level start.
//
// Split out of rt_main.cpp. Behaviour unchanged; this is a move.

#include "rt_internal.h"

// The shared internals (RG_CHECK, ONEGAMEUNIT_IN_METERS, RT_SectorHue, the
// light-ID bases) come in unqualified, exactly as when this code lived inside
// rt_main.cpp's anonymous namespace.
using namespace rtx;

void RT_SpawnFluid( int             count,
                    const FVector3& position,
                    const FVector3& velocity,
                    float           dispersionDegrees )
{
    if( count <= 0 || !cvar::rt_fluid_available || !cvar::rt_fluid )
    {
        return;
    }
    count = std::min( count, 10000 );

    if( rt.rgSpawnFluid )
    {
        auto info = RgSpawnFluidInfo{
            .sType                  = RG_STRUCTURE_TYPE_SPAWN_FLUID_INFO,
            .pNext                  = nullptr,
            .position               = { float( position.X ) * ONEGAMEUNIT_IN_METERS,
                                        float( position.Y ) * ONEGAMEUNIT_IN_METERS,
                                        float( position.Z ) * ONEGAMEUNIT_IN_METERS },
            .radius                 = 0.05f,
            .velocity               = { float( velocity.X ) * ONEGAMEUNIT_IN_METERS,
                                        float( velocity.Y ) * ONEGAMEUNIT_IN_METERS,
                                        float( velocity.Z ) * ONEGAMEUNIT_IN_METERS },
            .dispersionVelocity     = 0.9f,
            .dispersionAngleDegrees = dispersionDegrees,
            .count                  = uint32_t( count ),
        };

        RgResult r = rt.rgSpawnFluid( &info );
        RG_CHECK( r );
    }
}

void RT_RegisterFullscreenImage( const char* texture )
{
    if( !texture || texture[ 0 ] == '\0' )
    {
        return;
    }

    constexpr uint8_t empty[] = { 0, 0, 0, 0 };

    auto info = RgOriginalTextureInfo{
        .sType        = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO,
        .pNext        = nullptr,
        .pTextureName = texture,
        .pPixels      = empty,
        .size         = { 1, 1 },
        .filter       = RG_SAMPLER_FILTER_LINEAR,
        .addressModeU = RG_SAMPLER_ADDRESS_MODE_CLAMP,
        .addressModeV = RG_SAMPLER_ADDRESS_MODE_CLAMP,
    };

    RgResult r = rt.rgProvideOriginalTexture( &info );
    RG_CHECK( r );
}

void RT_DeleteFullscreenImage( const char* texture )
{
    if( !texture || texture[ 0 ] == '\0' )
    {
        return;
    }

    RgResult r = rt.rgMarkOriginalTextureAsDeleted( texture );
    RG_CHECK( r );
}

void RT_DrawFullscreenImage( const char* texture,
                             float       opacity,
                             FVector4    background_color,
                             FVector4    foreground_color,
                             float       splitef = 0,
                             float       scale   = 1 )
{
    // samplers are hardcoded to 'repeat' in the wrapper + primitive.color is ignored
    // so don't play anything :(
    if( g_isremix )
    {
        return;
    }

    if( !texture || texture[ 0 ] == '\0' )
    {
        return;
    }

    if( opacity < 0.001f )
    {
        return;
    }

    static constexpr uint32_t indices[] = { 0, 1, 2, 2, 3, 0 };

    static constexpr RgPrimitiveVertex verts_fullscreen[] = {
        { .position = { -1, +1, 0 }, .texCoord = { 0, 1 }, .color = 0xFFFFFFFF },
        { .position = { -1, -1, 0 }, .texCoord = { 0, 0 }, .color = 0xFFFFFFFF },
        { .position = { +1, -1, 0 }, .texCoord = { 1, 0 }, .color = 0xFFFFFFFF },
        { .position = { +1, +1, 0 }, .texCoord = { 1, 1 }, .color = 0xFFFFFFFF },
    };

    RgPrimitiveVertex verts_16by9[] = {
        verts_fullscreen[ 0 ],
        verts_fullscreen[ 1 ],
        verts_fullscreen[ 2 ],
        verts_fullscreen[ 3 ],
    };

    {
        const RgExtent2D wnd = RT_GetCurrentWindowSize();

        float xwin = ( float )wnd.width / ( float )wnd.height;
        float ximg = 16.0f / 9.0f;

        float tx, ty;
        if( ximg < xwin )
        {
            tx = ximg / xwin;
            ty = 1.0f;
        }
        else
        {
            tx = 1.0f;
            ty = xwin / ximg;
        }

#define VectorSet2( ptr, x, y ) \
    ( ptr )[ 0 ] = ( x );      \
    ( ptr )[ 1 ] = ( y )

        tx = ( 1 - 1 / tx ) / 2;
        ty = ( 1 - 1 / ty ) / 2;

        VectorSet2( verts_16by9[ 0 ].texCoord, tx, 1 - ty );
        VectorSet2( verts_16by9[ 1 ].texCoord, tx, ty );
        VectorSet2( verts_16by9[ 2 ].texCoord, 1 - tx, ty );
        VectorSet2( verts_16by9[ 3 ].texCoord, 1 - tx, 1 - ty );
    }

    // scale
    {
        for( RgPrimitiveVertex& v : verts_16by9 )
        {
            v.texCoord[ 0 ] = ( ( v.texCoord[ 0 ] - 0.5f ) / scale ) + 0.5f;
            v.texCoord[ 1 ] = ( ( v.texCoord[ 1 ] - 0.5f ) / scale ) + 0.5f;
        }
    }

    constexpr static float viewproj[ 16 ] = {
        1, 0, 0, 0, //
        0, 1, 0, 0, //
        0, 0, 1, 0, //
        0, 0, 0, 1, //
    };

    auto l_drawcolor = []( const RgPrimitiveVertex( &verts )[ 4 ],
                           RgColor4DPacked32        color ) {
        auto ui = RgMeshPrimitiveSwapchainedEXT{
            .sType           = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_SWAPCHAINED_EXT,
            .pNext           = nullptr,
            .flags           = 0,
            .pViewport       = nullptr,
            .pView           = nullptr,
            .pProjection     = nullptr,
            .pViewProjection = viewproj,
        };

        auto prim = RgMeshPrimitiveInfo{
            .sType                = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
            .pNext                = &ui,
            .flags                = RG_MESH_PRIMITIVE_TRANSLUCENT,
            .primitiveIndexInMesh = 0,
            .pVertices            = verts,
            .vertexCount          = uint32_t( std::size( verts ) ),
            .pIndices             = indices,
            .indexCount           = std::size( indices ),
            .pTextureName         = nullptr,
            .textureFrame         = 0,
            .color                = color,
            .emissive             = 0,
            .classicLight         = 1.0f,
        };

        RgResult r = rt.rgUploadMeshPrimitive( nullptr, &prim );
        RG_CHECK( r );
    };

    // back color
    if( background_color.W > 0 )
    {
        l_drawcolor( verts_fullscreen,
                     rt.rgUtilPackColorFloat4D( background_color.X, //
                                                background_color.Y,
                                                background_color.Z,
                                                background_color.W ) );
    }

    if( splitef > 0 )
    {
        RgPrimitiveVertex half[ 4 ];
        static_assert( sizeof( half ) == sizeof( verts_fullscreen ) );

        // left, rises top -> bottom
        {
            memcpy( half, verts_fullscreen, sizeof( verts_fullscreen ) );
            VectorSet2( half[ 0 ].position, -1, +1 );
            VectorSet2( half[ 1 ].position, -1, std::lerp( 1, -1, splitef ) );
            VectorSet2( half[ 2 ].position, 0, std::lerp( 1, -1, splitef ) );
            VectorSet2( half[ 3 ].position, 0, +1 );
            l_drawcolor( half, RG_PACKED_COLOR_WHITE );
        }
        // right, rises bottom -> top
        {
            memcpy( half, verts_fullscreen, sizeof( verts_fullscreen ) );
            VectorSet2( half[ 0 ].position, 0, std::lerp( -1, 1, splitef ) );
            VectorSet2( half[ 1 ].position, 0, -1 );
            VectorSet2( half[ 2 ].position, +1, -1 );
            VectorSet2( half[ 3 ].position, +1, std::lerp( -1, 1, splitef ) );
            l_drawcolor( half, RG_PACKED_COLOR_WHITE );
        }
    }

    // image
    {
        auto ui = RgMeshPrimitiveSwapchainedEXT{
            .sType           = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_SWAPCHAINED_EXT,
            .pNext           = nullptr,
            .flags           = 0,
            .pViewport       = nullptr,
            .pView           = nullptr,
            .pProjection     = nullptr,
            .pViewProjection = viewproj,
        };

        auto prim = RgMeshPrimitiveInfo{
            .sType                = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO,
            .pNext                = &ui,
            .flags                = RG_MESH_PRIMITIVE_TRANSLUCENT,
            .primitiveIndexInMesh = 0,
            .pVertices            = verts_16by9,
            .vertexCount          = std::size( verts_16by9 ),
            .pIndices             = indices,
            .indexCount           = std::size( indices ),
            .pTextureName         = texture,
            .textureFrame         = 0,
            .color                = rt.rgUtilPackColorFloat4D( 1.0f, 1.0f, 1.0f, opacity ),
            .emissive             = 0,
            .classicLight         = 1.0f,
        };

        RgResult r = rt.rgUploadMeshPrimitive( nullptr, &prim );
        RG_CHECK( r );
    }

    // foreground color
    if( foreground_color.W > 0 )
    {
        l_drawcolor( verts_fullscreen,
                     rt.rgUtilPackColorFloat4D( foreground_color.X, //
                                                foreground_color.Y,
                                                foreground_color.Z,
                                                foreground_color.W ) );
    }

    #undef VectorSet2
}

extern FSoundID T_FindSound( const char* name );

static int         g_title_begintick{ -1 };
static int         g_title_endtick{ -1 };
static int         g_title_fadeouttics{ 0 };
static std::string g_title_requested{};
static std::string g_title_uploaded{};
static bool        g_title_soundplayed{ false };

void RT_StartTitleImage( const char* imagepath,
                         int         begin_maptime,
                         int         end_maptime,
                         int         fadeout_tics )
{
    // samplers are hardcoded to 'repeat' in the wrapper + primitive.color is ignored
    // so don't play anything :(
    if( g_isremix )
    {
        return;
    }

    if( !imagepath || imagepath[ 0 ] == '\0' )
    {
        g_title_requested.clear();
        g_title_endtick     = -1;
        g_title_begintick   = -1;
        g_title_fadeouttics = 0;
        g_title_soundplayed = false;
        return;
    }

    g_title_requested   = imagepath;
    g_title_begintick   = begin_maptime;
    g_title_endtick     = end_maptime;
    g_title_fadeouttics = fadeout_tics;
    g_title_soundplayed = false;
}

void RT_DrawTitle()
{
    if( g_title_requested.empty() )
    {
        RT_ClearTitles();
        return;
    }

    if( level.sectors.Size() <= 0 )
    {
        RT_ClearTitles();
        return;
    }

    if( level.maptime >= g_title_endtick )
    {
        RT_ClearTitles();
        return;
    }

    // upload texture
    if( g_title_uploaded != g_title_requested )
    {
        if( !g_title_uploaded.empty() )
        {
            RT_DeleteFullscreenImage( g_title_uploaded.c_str() );
        }

        RT_RegisterFullscreenImage( g_title_requested.c_str() );
        g_title_uploaded = g_title_requested;
    }

    if( g_title_begintick > 0 )
    {
        if( level.maptime < g_title_begintick )
        {
            return;
        }
    }

    float alpha = 1.0f;
    if( g_title_fadeouttics > 0 )
    {
        int ticksleft = g_title_endtick - level.maptime;
        if( ticksleft < g_title_fadeouttics )
        {
            alpha = float( ticksleft ) / float( g_title_fadeouttics );

            // gamma
            alpha = alpha * alpha;
        }
    }

    RT_DrawFullscreenImage( g_title_uploaded.c_str(), //
                            alpha,
                            { 0, 0, 0, alpha * 0.3f },
                            { 0, 0, 0, 0 } );
    
    if( !g_title_soundplayed )
    {
        g_title_soundplayed = true;

        if( soundEngine )
        {
            // HACKHACK
            if( g_title_uploaded == "title/iconofsin" )
            {
                return;
            }

            FSoundID sound = T_FindSound( "sounds/cutscene/boom.ogg" );
            soundEngine->StartSound(
                SOURCE_None, nullptr, nullptr, CHAN_AUTO, CHANF_UI, sound, 1.0f, ATTN_NONE );
        }
    }
}

void RT_ClearTitles()
{
    if( !g_title_uploaded.empty() )
    {
        RT_DeleteFullscreenImage( g_title_uploaded.c_str() );
    }
    g_title_requested.clear();
    g_title_uploaded.clear();
    g_title_begintick   = -1;
    g_title_endtick     = -1;
    g_title_fadeouttics = 0;
    g_title_soundplayed = false;
}

extern bool rt_isdoom2;

void RT_InjectTitleIntoDoomMap( const char* mapname )
{
    if( !rt_isdoom2 )
    {
        return;
    }
    
    if( !mapname || mapname[ 0 ] == '\0' )
    {
        return;
    }

    // rt_isdoom2 only says "the IWAD is doom2.wad", and Doom 64: Retribution
    // *requires* doom2.wad -- so without a second test the DOOM II RT chapter
    // cards fire on Doom 64's maps, at Doom II's act boundaries. That is exactly
    // what used to happen: "EPISODE III / HELL" landed on MAP21 "Pitfalls",
    // which is 94% stone and one of the least hellish maps in the back half.
    //
    // Test on where the MAP LUMP came from, not on a lump lookup by name:
    // RT_GetMapWadName() returns empty for maps out of doom2.wad (it special-
    // cases the IWAD) and the pwad's name otherwise. DoLoadLevel already uses
    // exactly this call to build RT_MapName, so it is load-bearing code rather
    // than a guess about filesystem namespaces -- a CheckNumForName("DBIGFONT")
    // here looked obvious and silently returned -1, which is why the DOOM II
    // cards kept firing.
    extern FString RT_GetMapWadName( const char* mapname );
    const FString  wad_of_map = RT_GetMapWadName( mapname );
    const bool     ispwadmap  = !wad_of_map.IsEmpty();

    const char* titlename = nullptr;
    if( ispwadmap )
    {
        // Doom 64's own act boundaries. Deduced from texture families across the
        // 25 campaign maps: MAP01-08 are 99-100% SPACE/SFLAT tech with zero
        // stone and zero hell; MAP09-19 are stone-dominant; hell takes over at
        // MAP20 and stays. MAP08 -> INTER03 is also the campaign's only authored
        // story intermission, which corroborates the first seam independently.
        if( stricmp( mapname, "map01" ) == 0 )
        {
            titlename = "title/act1"; // INCURSION
        }
        else if( stricmp( mapname, "map09" ) == 0 )
        {
            titlename = "title/act2"; // CITADEL
        }
        else if( stricmp( mapname, "map20" ) == 0 )
        {
            titlename = "title/act3"; // HELL
        }
    }
    else
    {
        if( stricmp( mapname, "map12" ) == 0 )
        {
            titlename = "title/ep2";
        }
        else if( stricmp( mapname, "map21" ) == 0 )
        {
            titlename = "title/ep3";
        }
    }

    // Say what happened on every map that could produce a card. Without this a
    // silent no-card is indistinguishable from a wrong-card, and both have now
    // cost a launch to diagnose.
    const bool candidate = stricmp( mapname, "map01" ) == 0 || //
                           stricmp( mapname, "map09" ) == 0 || //
                           stricmp( mapname, "map12" ) == 0 || //
                           stricmp( mapname, "map20" ) == 0 || //
                           stricmp( mapname, "map21" ) == 0;
    if( candidate )
    {
        Printf( RT_DiagPrintLevel(),
                "RT_Title: %s from wad \"%s\" (%s) -> %s\n",
                mapname,
                wad_of_map.GetChars(),
                ispwadmap ? "mod" : "stock doom2",
                titlename ? titlename : "no card" );
    }

    if( !titlename )
    {
        return;
    }

    // 7s, not the stock 5s: the card now carries two lines of Doom 64 type with
    // real air between them, and 5s was not long enough to read it and still see
    // it settle. The 3s fade-out is the tail END of the 7s, not extra on top --
    // RT_DrawTitle() derives alpha from (endtick - maptime), so the card is up
    // from 1.5s to 8.5s and spends the last 3s of that fading.
    constexpr int BEGIN_TICS    = int( 1.5f * TICRATE );
    constexpr int DURATION_TICS = int( 7.0f * TICRATE );
    constexpr int FADEOUT_TICS  = int( 3.0f * TICRATE );

    RT_StartTitleImage( titlename, BEGIN_TICS, BEGIN_TICS + DURATION_TICS, FADEOUT_TICS );
}
