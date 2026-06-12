#pragma once


#define FORCENOINLINE __declspec(noinline)
#define FORCEINLINE __forceinline
#define RESTRICT __restrict

#ifndef RC_UNREAL_EXPORTS
#ifndef RC_UNREAL_BUILD_STATIC
#ifndef RC_UE_API
#define RC_UE_API __declspec(dllimport)
#endif
#else
#ifndef RC_UE_API
#define RC_UE_API
#endif
#endif
#else
#ifndef RC_UE_API
#define RC_UE_API __declspec(dllexport)
#endif
#endif


