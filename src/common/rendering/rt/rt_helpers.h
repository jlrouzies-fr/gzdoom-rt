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
// The chase crest, evaluated for one wall. Returns false when this sidedef is not a
// bulb panel on a chase pillar; otherwise stores the emissive strength its painted
// bulbs should have THIS FRAME and returns true, so RtPrim::ChasedPanel can be
// pushed. Read the value back with RT_ChasePanelEmisCurrent(). Split in two because
// the flag is a bitfield and the strength is a float; both are set immediately
// before the draw and read inside it, on the one render thread.
// Defined in rt_lights_fixtures.cpp.
struct side_t;
bool  RT_ChasePanelBegin( const side_t* side );
float RT_ChasePanelEmisCurrent();   // the screen value: the bulbs, on the LAGGED crest
float RT_ChasePanelGiCurrent();     // the GI value: the light, on the true crest
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
