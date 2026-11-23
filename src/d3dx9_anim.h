#ifndef WIN32HLE_D3DX9_ANIM_H
#define WIN32HLE_D3DX9_ANIM_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Public ABI (method ORDER and SIGNATURES only), from Wine's public header
 *   https://raw.githubusercontent.com/wine-mirror/wine/master/include/d3dx9anim.h
 * ID3DXKeyframedAnimationSet: 35 slots (0..34).
 * ID3DXAnimationController:   42 slots (0..41).
 * No Microsoft DLL or binary was consulted; nothing but order/signatures is
 * reproduced.  Slot counts are part of the contract and are checked by tests.
 * --------------------------------------------------------------------------- */
#define ID3DXKEYFRAMEDANIMATIONSET_SLOTS 35
#define ID3DXANIMATIONCONTROLLER_SLOTS   42

/* Public struct sizes/field offsets (same header): */
#define ANIM_VKEY_SIZE      16u   /* D3DXKEY_VECTOR3:  float Time; vec3 Value */
#define ANIM_QKEY_SIZE      20u   /* D3DXKEY_QUATERNION: float Time; quat Value */
#define ANIM_CKEY_SIZE       8u   /* D3DXKEY_CALLBACK: float Time; void* data  */
#define ANIM_TRACKDESC_SIZE 32u   /* D3DXTRACK_DESC on x86 (MSVC packing 8)   */

/* D3DX create shims (stdcall). */
void s_D3DXCreateKeyframedAnimationSet(void);
void s_D3DXCreateAnimationController(void);

/* ---- ID3DXKeyframedAnimationSet, slots 0..34 ---- */
void ka_QueryInterface(void);
void ka_AddRef(void);
void ka_Release(void);
void ka_GetName(void);
void ka_GetPeriod(void);
void ka_GetPeriodicPosition(void);
void ka_GetNumAnimations(void);
void ka_GetAnimationNameByIndex(void);
void ka_GetAnimationIndexByName(void);
void ka_GetSRT(void);
void ka_GetCallback(void);
void ka_GetPlaybackType(void);
void ka_GetSourceTicksPerSecond(void);
void ka_GetNumScaleKeys(void);
void ka_GetScaleKeys(void);
void ka_GetScaleKey(void);
void ka_SetScaleKey(void);
void ka_GetNumRotationKeys(void);
void ka_GetRotationKeys(void);
void ka_GetRotationKey(void);
void ka_SetRotationKey(void);
void ka_GetNumTranslationKeys(void);
void ka_GetTranslationKeys(void);
void ka_GetTranslationKey(void);
void ka_SetTranslationKey(void);
void ka_GetNumCallbackKeys(void);
void ka_GetCallbackKeys(void);
void ka_GetCallbackKey(void);
void ka_SetCallbackKey(void);
void ka_UnregisterScaleKey(void);
void ka_UnregisterRotationKey(void);
void ka_UnregisterTranslationKey(void);
void ka_RegisterAnimationSRTKeys(void);
void ka_Compress(void);
void ka_UnregisterAnimation(void);

/* ---- ID3DXAnimationController, slots 0..41 ---- */
void ac_QueryInterface(void);
void ac_AddRef(void);
void ac_Release(void);
void ac_GetMaxNumAnimationOutputs(void);
void ac_GetMaxNumAnimationSets(void);
void ac_GetMaxNumTracks(void);
void ac_GetMaxNumEvents(void);
void ac_RegisterAnimationOutput(void);
void ac_RegisterAnimationSet(void);
void ac_UnregisterAnimationSet(void);
void ac_GetNumAnimationSets(void);
void ac_GetAnimationSet(void);
void ac_GetAnimationSetByName(void);
void ac_AdvanceTime(void);
void ac_ResetTime(void);
void ac_GetTime(void);
void ac_SetTrackAnimationSet(void);
void ac_GetTrackAnimationSet(void);
void ac_SetTrackPriority(void);
void ac_SetTrackSpeed(void);
void ac_SetTrackWeight(void);
void ac_SetTrackPosition(void);
void ac_SetTrackEnable(void);
void ac_SetTrackDesc(void);
void ac_GetTrackDesc(void);
void ac_SetPriorityBlend(void);
void ac_GetPriorityBlend(void);
void ac_KeyTrackSpeed(void);
void ac_KeyTrackWeight(void);
void ac_KeyTrackPosition(void);
void ac_KeyTrackEnable(void);
void ac_KeyPriorityBlend(void);
void ac_UnkeyEvent(void);
void ac_UnkeyAllTrackEvents(void);
void ac_UnkeyAllPriorityBlends(void);
void ac_GetCurrentTrackEvent(void);
void ac_GetCurrentPriorityBlend(void);
void ac_GetUpcomingTrackEvent(void);
void ac_GetUpcomingPriorityBlend(void);
void ac_ValidateEvent(void);
void ac_GetEventDesc(void);
void ac_CloneAnimationController(void);

#endif /* WIN32HLE_D3DX9_ANIM_H */
