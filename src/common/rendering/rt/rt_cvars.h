#pragma once

#include "c_cvars.h"

#include <type_traits>

// Maps the C++ type of a cvar's default value onto gzdoom's cvar-ref class, so a
// declaration in rt_cvars.inc can carry its type implicitly in the default rather
// than repeating it. Lives here rather than in rt_cvars.cpp because BOTH faces of
// the .inc -- the declaring one below and the defining one in rt_cvars.cpp -- have
// to expand it to the same type, or the extern and the definition disagree.
// clang-format off
template< typename T >
using ValueToCVarRef =
    std::conditional_t< std::is_same_v< T, bool  >, FBoolCVarRef,
    std::conditional_t< std::is_same_v< T, int   >, FIntCVarRef,
    std::conditional_t< std::is_same_v< T, float >, FFloatCVarRef,
    void > > >;

template< typename T >
constexpr ECVarType ValueToCVarType =
    std::is_same_v< T, bool  > ? ECVarType::CVAR_Bool :
    std::is_same_v< T, int   > ? ECVarType::CVAR_Int :
    std::is_same_v< T, float > ? ECVarType::CVAR_Float :
                                 ECVarType::CVAR_Dummy;
// clang-format on

namespace cvar
{

// The declaring face of rt_cvars.inc. See rt_cvars.cpp for the defining one.
#define RT_CVAR( name, default_value, description ) \
    extern ValueToCVarRef< decltype( default_value ) > name;
#define RT_CVAR_NOARCH( name, default_value, description ) \
    extern ValueToCVarRef< decltype( default_value ) > name;
#define RT_CVAR_COLOR( name, default_value, description ) EXTERN_CVAR( Color, name )
#define RT_CVAR_STRING( name, default_value, flags, description ) EXTERN_CVAR( String, name )

#include "rt_cvars.inc"

#undef RT_CVAR
#undef RT_CVAR_NOARCH
#undef RT_CVAR_COLOR
#undef RT_CVAR_STRING

// Not cvars: capability flags and the reasons a capability was refused, written
// once during device init and read by the menu and the upscaler mapping.
extern bool rt_available_dlss2;
extern bool rt_available_dlss3fg;
extern bool rt_available_fsr2;
extern bool rt_available_fsr3fg;
extern bool rt_available_dxgi;

extern const char* rt_failreason_dlss2;
extern const char* rt_failreason_dlss3fg;
extern const char* rt_failreason_fsr2;
extern const char* rt_failreason_fsr3fg;
extern const char* rt_failreason_dxgi;

extern bool rt_hdr_available;
extern bool rt_fluid_available;

extern bool rt_firststart;

} // namespace cvar

// Declared at global scope, not in `namespace cvar`, because that is where its
// definition has always been -- RTRenderState reads it unqualified.
extern ValueToCVarRef< decltype( 3 ) > rt_mod_compat;

extern int rt_cullmode;
#define rt_only_one_side_wall true
#define rt_nocull_flat true
