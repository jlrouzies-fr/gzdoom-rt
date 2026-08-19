#pragma once

#if HAVE_RT

struct sector_t;
struct seg_t;
struct FLevelLocals;

void RT_BakeExportables( const std::vector< bool >& animatedTexnums );
bool RT_IsSectorExportable( const sector_t* sector, bool ceiling );
bool RT_IsSectorExportable2( int sectornum, bool ceiling );
// Did this sector plane get real bulb-lattice lights on the last uploaded frame? Drives
// RtPrim::LatticeLitFlat, i.e. whether a lamp pane's painted glow is switched off because
// something real replaced it. Defined in rt_lights_fixtures.cpp.
bool RT_IsLatticeLitPlane( unsigned secIndex, bool ceiling );
bool RT_IsWallExportable( const seg_t* seg );
// True when RT must upload map geometry every frame (no baked rt/scenes for this map).
bool RT_ModMapNeedsLiveGeometryUpload();

// Sector self-emission holds still while a light thinker animates the sector --
// see the block comment in rt_lights_sector.cpp. The snapshot has to be taken
// before any thinker spawns, which is why MapLoader::LoadLevel calls it rather
// than anything on the RT side; the substitution belongs to whoever pushes a
// sector's light into the RT state (hw_flats, hw_walls).
void RT_SnapshotSectorLight( FLevelLocals* level );
int  RT_EmisLightLevel( const sector_t* sec, int live );

void RT_RequestMelt();
bool RT_IsMeltActive();
bool RT_IgnoreUserInput();

auto RT_GetCurrentTime() -> double;
auto RT_GetVramUsage( bool* ok = nullptr ) -> const char*;

#endif
