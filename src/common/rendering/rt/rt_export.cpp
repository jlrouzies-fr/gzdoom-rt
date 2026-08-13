// Which parts of the map RTGL1 may bake into its static scene.
//
// A surface is "exportable" when it is guaranteed not to move: RTGL1 bakes those
// into a static BLAS once per level instead of re-uploading them every frame,
// which is most of the CPU win. A wrongly-exported surface is therefore a lift or
// a door frozen in place, so the predicates here are deliberately conservative --
// a tagged sector, an animated texture or a linedef special that could move
// something all disqualify it.
//
// The public entry points are declared in rt_helpers.h and called from p_setup,
// hw_bsp, hw_flats and hw_portal, not from the RT renderer itself.
//
// Split out of rt_main.cpp. Behaviour unchanged; this is a move.

#include "rt_internal.h"

// The shared internals (RG_CHECK, ONEGAMEUNIT_IN_METERS, RT_SectorHue, the
// light-ID bases) come in unqualified, exactly as when this code lived inside
// rt_main.cpp's anonymous namespace.
using namespace rtx;


// A hack to access special+tag by a linenum
extern std::vector< std::pair< int, int > > rt_linesToSpecialAndTag;

extern auto RT_GetStairsSectors( int tag, line_t* line ) -> std::vector< int >;

namespace
{

std::unordered_set< int > g_tagsSafeToIgnore{};
std::unordered_set< int > g_stairsSectors{};

void RT_CacheTagsAndSpecials()
{
    if( !primaryLevel )
    {
        g_tagsSafeToIgnore.clear();
        g_stairsSectors.clear();
    }

    assert( rt_linesToSpecialAndTag.size() == primaryLevel->lines.size() );

    // 1 tag can be referenced by N specials
    // this is the mapping from tag to its list of specials
    std::unordered_map< int, std::unordered_set< int > > tagToSpecial{};
    for( const auto& [ special, tag ] : rt_linesToSpecialAndTag )
    {
        // tag < 0 -- ignored
        // tag = 0 -- has different behavior
        if( tag > 0 )
        {
            tagToSpecial[ tag ].emplace( special );
        }
    }

    // specials that do not move the geometry, so we can export it
    auto l_isSafeToIgnoreSpecial = []( int spec ) {
        switch( spec )
        {
            case Teleport:
            case Teleport_NoStop:
            case Teleport_NoFog:
            case Light_RaiseByValue:
            case Light_LowerByValue:
            case Light_ChangeToValue:
            case Light_Stop:
            case Light_MinNeighbor:
            case Light_MaxNeighbor:
            case Light_StrobeDoom: return true;
            default: return false;
        }
    };

    // make a list 
    std::unordered_set< int > tagsSafeToIgnore{};
    for( const auto& [ tag, specials ] : tagToSpecial )
    {
        // if no specials on a tag, it's safe
        if( specials.empty() )
        {
            assert( !tagsSafeToIgnore.contains( tag ) );
            tagsSafeToIgnore.emplace( tag );
            continue;
        }

        // if only one special on this tag
        if( specials.size() == 1 )
        {
            // and it's a safe special
            int spec = *specials.begin();
            if( l_isSafeToIgnoreSpecial( spec ) )
            {
                assert( !tagsSafeToIgnore.contains( tag ) );
                tagsSafeToIgnore.emplace( tag );
                continue;
            }
        }

        // surely, we can expand to specials.size() >= 2 (e.g. 1 tag is used for Teleport and Light_Stop => we can ignore),
        // but let's play safely for now..
    }

    g_tagsSafeToIgnore = std::move( tagsSafeToIgnore );


    assert( g_stairsSectors.empty() );
    for( uint32_t i = 0; i < rt_linesToSpecialAndTag.size(); i++ )
    {
        const auto& [ special, tag ] = rt_linesToSpecialAndTag[ i ];

        const auto sectornums = RT_GetStairsSectors( tag, &primaryLevel->lines[ i ] );
        g_stairsSectors.insert( sectornums.begin(), sectornums.end() );
    }
}


// NOTE: only linedef->special, and not sector->special, as it has only light change effects,
// sector that move has tag or one of its lines marked as lift/door/etc (linedef->special)


// If some line specials have tag==0,
// then line's backsector is a target of the special's action
bool IsTaggedByTag0( const line_t* linedef, const sector_t* target )
{
    if( !linedef || !primaryLevel )
    {
        return false;
    }

    // only backsectors
    if( linedef->backsector != target )
    {
        return false;
    }

    // tag == 0
    if( !primaryLevel->tagManager.RT_LineHasZeroTag( linedef ) )
    {
        return false;
    }

    switch( linedef->special )
    {
        // case ACS_Execute:
        // case ACS_ExecuteAlways:
        // case ACS_ExecuteWithResult:
        // case ACS_LockedExecute:
        // case ACS_LockedExecuteDoor:
        // case ACS_Suspend:
        // case ACS_Terminate:
        // case Autosave:
        case Ceiling_CrushAndRaise:
        case Ceiling_CrushAndRaiseA:
        case Ceiling_CrushAndRaiseDist:
        case Ceiling_CrushAndRaiseSilentA:
        case Ceiling_CrushAndRaiseSilentDist:
        case Ceiling_CrushRaiseAndStay:
        case Ceiling_CrushRaiseAndStayA:
        case Ceiling_CrushRaiseAndStaySilA:
        case Ceiling_CrushStop:
        case Ceiling_LowerAndCrush:
        case Ceiling_LowerAndCrushDist:
        case Ceiling_LowerByTexture:
        case Ceiling_LowerByValue:
        case Ceiling_LowerByValueTimes8:
        case Ceiling_LowerInstant:
        case Ceiling_LowerToFloor:
        case Ceiling_LowerToHighestFloor:
        case Ceiling_LowerToLowest:
        case Ceiling_LowerToNearest:
        case Ceiling_MoveToValue:
        case Ceiling_MoveToValueAndCrush:
        case Ceiling_MoveToValueTimes8:
        case Ceiling_RaiseByTexture:
        case Ceiling_RaiseByValue:
        case Ceiling_RaiseByValueTimes8:
        case Ceiling_RaiseInstant:
        case Ceiling_RaiseToHighest:
        case Ceiling_RaiseToHighestFloor:
        case Ceiling_RaiseToLowest:
        case Ceiling_RaiseToNearest:
        case Ceiling_Stop:
        case Ceiling_ToFloorInstant:
        case Ceiling_ToHighestInstant:
        case Ceiling_Waggle:
        // case ChangeCamera:
        // case ChangeSkill:
        // case ClearForceField:
        // case DamageThing:
        case Door_Animated:
        case Door_AnimatedClose:
        case Door_Close:
        case Door_CloseWaitOpen:
        case Door_LockedRaise:
        case Door_Open:
        case Door_Raise:
        case Door_WaitClose:
        case Door_WaitRaise:
        case Elevator_LowerToNearest:
        case Elevator_MoveToFloor:
        case Elevator_RaiseToNearest:
        // case Exit_Normal:
        // case Exit_Secret:
        // case ExtraFloor_LightOnly:
        case Floor_CrushStop:
        case Floor_Donut:
        case Floor_LowerByTexture:
        case Floor_LowerByValue:
        case Floor_LowerByValueTimes8:
        case Floor_LowerInstant:
        case Floor_LowerToHighest:
        case Floor_LowerToHighestEE:
        case Floor_LowerToLowest:
        case Floor_LowerToLowestCeiling:
        case Floor_LowerToLowestTxTy:
        case Floor_LowerToNearest:
        case Floor_MoveToValue:
        case Floor_MoveToValueAndCrush:
        case Floor_MoveToValueTimes8:
        case Floor_RaiseAndCrush:
        case Floor_RaiseAndCrushDoom:
        case Floor_RaiseByTexture:
        case Floor_RaiseByValue:
        case Floor_RaiseByValueTimes8:
        case Floor_RaiseByValueTxTy:
        case Floor_RaiseInstant:
        case Floor_RaiseToCeiling:
        case Floor_RaiseToHighest:
        case Floor_RaiseToLowest:
        case Floor_RaiseToLowestCeiling:
        case Floor_RaiseToNearest:
        case Floor_Stop:
        case Floor_ToCeilingInstant:
        case Floor_TransferNumeric:
        case Floor_TransferTrigger:
        case Floor_Waggle:
        case FloorAndCeiling_LowerByValue:
        case FloorAndCeiling_LowerRaise:
        case FloorAndCeiling_RaiseByValue:
        // case ForceField:
        // case FS_Execute:
        case Generic_Ceiling:
        case Generic_Crusher:
        case Generic_Crusher2:
        case Generic_Door:
        case Generic_Floor:
        case Generic_Lift:
        case Generic_Stairs:
        // case GlassBreak:
        // case HealThing:
        // case Light_ChangeToValue:
        // case Light_Fade:
        // case Light_Flicker:
        // case Light_ForceLightning:
        // case Light_Glow:
        // case Light_LowerByValue:
        // case Light_MaxNeighbor:
        // case Light_MinNeighbor:
        // case Light_RaiseByValue:
        // case Light_Stop:
        // case Light_Strobe:
        // case Light_StrobeDoom:
        // case Line_AlignCeiling:
        // case Line_AlignFloor:
        // case Line_Horizon:
        // case Line_Mirror:
        // case Line_QuickPortal:
        // case Line_SetAutomapFlags:
        // case Line_SetAutomapStyle:
        // case Line_SetBlocking:
        // case Line_SetHealth:
        // case Line_SetIdentification:
        // case Line_SetPortal:
        // case Line_SetPortalTarget:
        // case Line_SetTextureOffset:
        // case Line_SetTextureScale:
        // case NoiseAlert:
        case Pillar_Build:
        case Pillar_BuildAndCrush:
        case Pillar_Open:
        // case Plane_Align:
        // case Plane_Copy:
        case Plat_DownByValue:
        case Plat_DownWaitUpStay:
        case Plat_DownWaitUpStayLip:
        case Plat_PerpetualRaise:
        case Plat_PerpetualRaiseLip:
        case Plat_RaiseAndStayTx0:
        case Plat_Stop:
        case Plat_ToggleCeiling:
        case Plat_UpByValue:
        case Plat_UpByValueStayTx:
        case Plat_UpNearestWaitDownStay:
        case Plat_UpWaitDownStay:
        // case PointPush_SetForce:
        // case Polyobj_DoorSlide:
        // case Polyobj_DoorSwing:
        // case Polyobj_ExplicitLine:
        // case Polyobj_Move:
        // case Polyobj_MoveTimes8:
        // case Polyobj_MoveTo:
        // case Polyobj_MoveToSpot:
        // case Polyobj_OR_Move:
        // case Polyobj_OR_MoveTimes8:
        // case Polyobj_OR_MoveTo:
        // case Polyobj_OR_MoveToSpot:
        // case Polyobj_OR_RotateLeft:
        // case Polyobj_OR_RotateRight:
        // case Polyobj_RotateLeft:
        // case Polyobj_RotateRight:
        // case Polyobj_StartLine:
        // case Polyobj_Stop:
        // case Polyobj_StopSound:
        // case Radius_Quake:
        // case Scroll_Ceiling:
        // case Scroll_Floor:
        // case Scroll_Texture_Both:
        // case Scroll_Texture_Down:
        // case Scroll_Texture_Left:
        // case Scroll_Texture_Model:
        // case Scroll_Texture_Offsets:
        // case Scroll_Texture_Right:
        // case Scroll_Texture_Up:
        // case Scroll_Wall:
        // case Sector_Attach3dMidtex:
        // case Sector_ChangeFlags:
        // case Sector_ChangeSound:
        // case Sector_CopyScroller:
        // case Sector_Set3DFloor:
        // case Sector_SetCeilingGlow:
        // case Sector_SetCeilingPanning:
        // case Sector_SetCeilingScale:
        // case Sector_SetCeilingScale2:
        // case Sector_SetColor:
        // case Sector_SetContents:
        // case Sector_SetCurrent:
        // case Sector_SetDamage:
        // case Sector_SetFade:
        // case Sector_SetFloorGlow:
        // case Sector_SetFloorPanning:
        // case Sector_SetFloorScale:
        // case Sector_SetFloorScale2:
        // case Sector_SetFriction:
        // case Sector_SetGravity:
        // case Sector_SetHealth:
        // case Sector_SetLink:
        // case Sector_SetPlaneReflection:
        // case Sector_SetPortal:
        // case Sector_SetRotation:
        // case Sector_SetTranslucent:
        // case Sector_SetWind:
        // case SendToCommunicator:
        // case SetGlobalFogParameter:
        // case SetPlayerProperty:
        case Stairs_BuildDown:
        case Stairs_BuildDownDoom:
        case Stairs_BuildDownDoomSync:
        case Stairs_BuildDownSync:
        case Stairs_BuildUp:
        case Stairs_BuildUpDoom:
        case Stairs_BuildUpDoomCrush:
        case Stairs_BuildUpDoomSync:
        case Stairs_BuildUpSync:
        // case StartConversation:
        // case Static_Init:
        // case Teleport:
        // case Teleport_EndGame:
        // case Teleport_Line:
        // case Teleport_NewMap:
        // case Teleport_NoFog:
        // case Teleport_NoStop:
        // case Teleport_ZombieChanger:
        // case TeleportGroup:
        // case TeleportInSector:
        // case TeleportOther:
        // case Thing_Activate:
        // case Thing_ChangeTID:
        // case Thing_Damage:
        // case Thing_Deactivate:
        // case Thing_Destroy:
        // case Thing_Hate:
        // case Thing_Move:
        // case Thing_Projectile:
        // case Thing_ProjectileAimed:
        // case Thing_ProjectileGravity:
        // case Thing_ProjectileIntercept:
        // case Thing_Raise:
        // case Thing_Remove:
        // case Thing_SetConversation:
        // case Thing_SetGoal:
        // case Thing_SetSpecial:
        // case Thing_SetTranslation:
        // case Thing_Spawn:
        // case Thing_SpawnFacing:
        // case Thing_SpawnNoFog:
        // case Thing_Stop:
        // case ThrustThing:
        // case ThrustThingZ:
        case Transfer_CeilingLight:
        case Transfer_FloorLight:
        case Transfer_Heights:
        case Transfer_WallLight:
            // case TranslucentLine:
            // case UsePuzzleItem:
            return true;
        default: return false;
    }
}

bool RT_IsSectorMovable( const sector_t* sector )
{
    if( !sector )
    {
        return false;
    }

    auto isTaggedExplicitly = []( const sector_t& s ) {
        if( !primaryLevel )
        {
            return false;
        }

        if( g_stairsSectors.contains( s.Index() ) )
        {
            return true;
        }

        auto l_safeToIgnoreTag = [ & ]( int tag ) {
            return g_tagsSafeToIgnore.contains( tag );
        };

        // if there's at least one NON-safe tag on this sector, it's tagged
        const auto sectorTags = primaryLevel->tagManager.RT_GetAllSectorTags( &s );
        return !std::ranges::all_of( sectorTags, l_safeToIgnoreTag );
    };

    auto isTaggedImplicitly = []( const sector_t& s ) {
        for( const line_t* l : s.Lines )
        {
            if( IsTaggedByTag0( l, &s ) )
            {
                return true;
            }
        }
        return false;
    };

    return isTaggedExplicitly( *sector ) || isTaggedImplicitly( *sector );
}

bool RT_IsTexAnimated( int texnum, const std::vector< bool >& animatedTexnums )
{
    if( texnum < 0 || static_cast< uint32_t >( texnum ) >= animatedTexnums.size() )
    {
        assert( 0 );
        return false;
    }
    return animatedTexnums[ texnum ];
}

bool RT_IsSectorExportable( const sector_t*            sector,
                            bool                       ceiling,
                            const std::vector< bool >& animatedTexnums )
{
    if( !sector )
    {
        assert( 0 );
        return false;
    }

    // e.g. nukage, lava
    bool isAnimated = RT_IsTexAnimated(
        sector->GetTexture( ceiling ? sector_t::ceiling : sector_t::floor ).GetIndex(),
        animatedTexnums );

    return !isAnimated && !RT_IsSectorMovable( sector );
}

bool RT_IsWallExportable( const seg_t* seg, const std::vector< bool >& animatedTexnums )
{
    if( !seg )
    {
        assert( 0 );
        return false;
    }

    // e.g. switches
    auto isAnimated = [ &animatedTexnums ]( const side_t* side ) {
        if( side )
        {
            return RT_IsTexAnimated( side->GetTexture( 0 ).GetIndex(), animatedTexnums ) ||
                   RT_IsTexAnimated( side->GetTexture( 1 ).GetIndex(), animatedTexnums ) ||
                   RT_IsTexAnimated( side->GetTexture( 2 ).GetIndex(), animatedTexnums );
        }
        return false;
    };

    auto isAdjacentSectorMovable = []( const seg_t& s ) {
        if( s.linedef )
        {
            return RT_IsSectorMovable( s.linedef->backsector ) ||
                   RT_IsSectorMovable( s.linedef->frontsector );
        }
        return true;
    };

    return !isAnimated( seg->sidedef ) && !isAdjacentSectorMovable( *seg );
}

enum
{
    RT_WALL_PEGGED_TOP    = 1,
    RT_WALL_PEGGED_BOTTOM = 2,
};

// Pegged texture moves with a Sector that moves
uint8_t RT_WallPeggedFlags( const seg_t* seg )
{
    if( !seg || !seg->linedef )
    {
        return false;
    }

    // if double sided
    if( seg->backsector )
    {
        int fs = RT_WALL_PEGGED_TOP | RT_WALL_PEGGED_BOTTOM;

        if( seg->linedef->flags & ML_DONTPEGTOP )
        {
            fs = ( fs & ~( RT_WALL_PEGGED_TOP ) );
        }

        if( seg->linedef->flags & ML_DONTPEGBOTTOM )
        {
            fs = ( fs & ~( RT_WALL_PEGGED_BOTTOM ) );
        }
        
        return uint8_t( fs );
    }
    else
    {
        // one sided always pegged
        return RT_WALL_PEGGED_TOP | RT_WALL_PEGGED_BOTTOM;
    }
}

auto rt_sectorCeilingExportable = std::vector< bool >{};
auto rt_sectorFloorExportable   = std::vector< bool >{};
auto rt_wallExportable          = std::vector< bool >{};
auto rt_wallPegged              = std::vector< uint8_t >{};

} // anonymous namespace

void RT_BakeExportables( const std::vector< bool >& animatedTexnums )
{
    rt_sectorCeilingExportable.clear();
    rt_sectorFloorExportable.clear();
    rt_wallExportable.clear();
    rt_wallPegged.clear();
    g_tagsSafeToIgnore.clear();
    g_stairsSectors.clear();

    if( !primaryLevel )
    {
        return;
    }

    RT_CacheTagsAndSpecials();

    rt_sectorCeilingExportable.resize( primaryLevel->sectors.Size(), false );
    rt_sectorFloorExportable.resize( primaryLevel->sectors.Size(), false );
    for( uint32_t i = 0; i < primaryLevel->sectors.Size(); i++ )
    {
        rt_sectorCeilingExportable[ i ] =
            RT_IsSectorExportable( &primaryLevel->sectors[ i ], true, animatedTexnums );
        rt_sectorFloorExportable[ i ] =
            RT_IsSectorExportable( &primaryLevel->sectors[ i ], false, animatedTexnums );
    }

    rt_wallExportable.resize( primaryLevel->segs.Size(), false );
    for( uint32_t i = 0; i < primaryLevel->segs.Size(); i++ )
    {
        rt_wallExportable[ i ] = RT_IsWallExportable( &primaryLevel->segs[ i ], animatedTexnums );
    }

    rt_wallPegged.resize( primaryLevel->segs.Size(), false );
    for( uint32_t i = 0; i < primaryLevel->segs.Size(); i++ )
    {
        rt_wallPegged[ i ] = RT_WallPeggedFlags( &primaryLevel->segs[ i ] );
    }
}

bool RT_IsSectorExportable2( int sectornum, bool ceiling )
{
    if( sectornum >= 0 )
    {
        const auto& arr = ceiling ? rt_sectorCeilingExportable : rt_sectorFloorExportable;

        if( sectornum < int( arr.size() ) )
        {
            return arr[ sectornum ];
        }
    }
    return false;
}

bool RT_IsSectorExportable( const sector_t* sector, bool ceiling )
{
    if( sector )
    {
        return RT_IsSectorExportable2( sector->sectornum, ceiling );
    }
    return false;
}

bool RT_IsWallExportable( const seg_t* seg )
{
    if( seg && seg->segnum >= 0 )
    {
        const auto segnum = static_cast< uint32_t >( seg->segnum );

        if( segnum < rt_wallExportable.size() )
        {
            return rt_wallExportable[ segnum ];
        }
    }
    return false;
}

bool RT_IsWallNoMotionVectors( const seg_t* seg, side_t::ETexpart part )
{
    if( part == side_t::top || part == side_t::bottom )
    {
        if( seg && seg->segnum >= 0 && uint32_t( seg->segnum ) < rt_wallPegged.size() )
        {
            if( part == side_t::top )
            {
                // inverse logic, as top grows from bottom to up
                return !( ( rt_wallPegged[ seg->segnum ] ) & RT_WALL_PEGGED_TOP );
            }
            else
            {
                return ( rt_wallPegged[ seg->segnum ] ) & RT_WALL_PEGGED_BOTTOM;
            }
        }
    }
    return true;
}



