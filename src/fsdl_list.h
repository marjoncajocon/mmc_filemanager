/* fsdl_list.h -- every SDL function the program calls, one per line.
**
** Read by fsdl.h/fsdl.c (X-macro) and by build.sh, which turns each line
** into `#define SDL_Name fmsdl_SDL_Name` for builds that load SDL2 at run
** time (tcc, Linux, macOS, BSD). A call to an SDL function missing here
** fails to link in those builds, so add the line when you use a new one.
**
** Rules: one FM_SDL_FN per line, return type without commas, the name
** starting with SDL_, the parameter list in parentheses. FM_SDL_FX lines
** are loaded the same way but get no #define redirect.
*/
#ifndef FM_SDL_FX
#  define FM_SDL_FX FM_SDL_FN
#  define FM_SDL_FX_LOCAL
#endif

/* ---- core --------------------------------------------------------------- */
FM_SDL_FN(int, SDL_Init, (Uint32))
FM_SDL_FN(void, SDL_Quit, (void))
FM_SDL_FN(int, SDL_InitSubSystem, (Uint32))
FM_SDL_FN(void, SDL_QuitSubSystem, (Uint32))
FM_SDL_FN(Uint32, SDL_WasInit, (Uint32))
FM_SDL_FN(const char *, SDL_GetError, (void))
FM_SDL_FN(void, SDL_ClearError, (void))
FM_SDL_FN(SDL_bool, SDL_SetHint, (const char *, const char *))
FM_SDL_FN(void, SDL_Log, (const char *, ...))
FM_SDL_FN(void, SDL_GetVersion, (SDL_version *))
FM_SDL_FN(const char *, SDL_GetPlatform, (void))
FM_SDL_FN(int, SDL_GetCPUCount, (void))
FM_SDL_FN(int, SDL_GetSystemRAM, (void))
FM_SDL_FN(SDL_bool, SDL_HasSSE2, (void))
FM_SDL_FN(SDL_bool, SDL_HasAVX2, (void))
FM_SDL_FN(SDL_bool, SDL_HasNEON, (void))
FM_SDL_FN(char *, SDL_GetBasePath, (void))
FM_SDL_FN(char *, SDL_GetPrefPath, (const char *, const char *))
FM_SDL_FN(void, SDL_free, (void *))
FM_SDL_FN(int, SDL_OpenURL, (const char *))
FM_SDL_FN(SDL_PowerState, SDL_GetPowerInfo, (int *, int *))
FM_SDL_FN(int, SDL_ShowSimpleMessageBox, (Uint32, const char *, const char *, SDL_Window *))
FM_SDL_FN(int, SDL_ShowMessageBox, (const SDL_MessageBoxData *, int *))

/* ---- video -------------------------------------------------------------- */
FM_SDL_FN(SDL_Window *, SDL_CreateWindow, (const char *, int, int, int, int, Uint32))
FM_SDL_FN(void, SDL_DestroyWindow, (SDL_Window *))
FM_SDL_FN(void, SDL_SetWindowTitle, (SDL_Window *, const char *))
FM_SDL_FN(void, SDL_GetWindowSize, (SDL_Window *, int *, int *))
FM_SDL_FN(void, SDL_SetWindowSize, (SDL_Window *, int, int))
FM_SDL_FN(void, SDL_GetWindowPosition, (SDL_Window *, int *, int *))
FM_SDL_FN(void, SDL_SetWindowPosition, (SDL_Window *, int, int))
FM_SDL_FN(void, SDL_SetWindowMinimumSize, (SDL_Window *, int, int))
FM_SDL_FN(Uint32, SDL_GetWindowFlags, (SDL_Window *))
FM_SDL_FN(int, SDL_SetWindowFullscreen, (SDL_Window *, Uint32))
FM_SDL_FN(void, SDL_MaximizeWindow, (SDL_Window *))
FM_SDL_FN(void, SDL_RestoreWindow, (SDL_Window *))
FM_SDL_FN(void, SDL_MinimizeWindow, (SDL_Window *))
FM_SDL_FN(void, SDL_SetWindowBordered, (SDL_Window *, SDL_bool))
FM_SDL_FN(int, SDL_SetWindowHitTest, (SDL_Window *, SDL_HitTest, void *))
FM_SDL_FN(void, SDL_ShowWindow, (SDL_Window *))
FM_SDL_FN(void, SDL_RaiseWindow, (SDL_Window *))
FM_SDL_FN(void, SDL_SetWindowIcon, (SDL_Window *, SDL_Surface *))
FM_SDL_FN(Uint32, SDL_GetWindowID, (SDL_Window *))
FM_SDL_FN(int, SDL_GetWindowDisplayIndex, (SDL_Window *))
FM_SDL_FN(int, SDL_GetDisplayDPI, (int, float *, float *, float *))
FM_SDL_FN(int, SDL_GetDisplayUsableBounds, (int, SDL_Rect *))
FM_SDL_FN(int, SDL_GetCurrentDisplayMode, (int, SDL_DisplayMode *))
FM_SDL_FN(SDL_DisplayOrientation, SDL_GetDisplayOrientation, (int))
FM_SDL_FN(void, SDL_DisableScreenSaver, (void))
FM_SDL_FN(void, SDL_EnableScreenSaver, (void))

/* ---- render ------------------------------------------------------------- */
FM_SDL_FN(SDL_Renderer *, SDL_CreateRenderer, (SDL_Window *, int, Uint32))
FM_SDL_FN(void, SDL_DestroyRenderer, (SDL_Renderer *))
FM_SDL_FN(int, SDL_GetRendererInfo, (SDL_Renderer *, SDL_RendererInfo *))
FM_SDL_FN(int, SDL_GetRendererOutputSize, (SDL_Renderer *, int *, int *))
FM_SDL_FN(int, SDL_SetRenderDrawColor, (SDL_Renderer *, Uint8, Uint8, Uint8, Uint8))
FM_SDL_FN(int, SDL_SetRenderDrawBlendMode, (SDL_Renderer *, SDL_BlendMode))
FM_SDL_FN(int, SDL_RenderClear, (SDL_Renderer *))
FM_SDL_FN(void, SDL_RenderPresent, (SDL_Renderer *))
FM_SDL_FN(int, SDL_RenderFillRect, (SDL_Renderer *, const SDL_Rect *))
FM_SDL_FN(int, SDL_RenderFillRectF, (SDL_Renderer *, const SDL_FRect *))
FM_SDL_FN(int, SDL_RenderDrawLineF, (SDL_Renderer *, float, float, float, float))
FM_SDL_FN(int, SDL_RenderGeometry, (SDL_Renderer *, SDL_Texture *, const SDL_Vertex *, int, const int *, int))
FM_SDL_FN(int, SDL_RenderCopy, (SDL_Renderer *, SDL_Texture *, const SDL_Rect *, const SDL_Rect *))
FM_SDL_FN(int, SDL_RenderCopyF, (SDL_Renderer *, SDL_Texture *, const SDL_Rect *, const SDL_FRect *))
FM_SDL_FN(int, SDL_RenderCopyExF, (SDL_Renderer *, SDL_Texture *, const SDL_Rect *, const SDL_FRect *, const double, const SDL_FPoint *, const SDL_RendererFlip))
FM_SDL_FN(int, SDL_RenderSetClipRect, (SDL_Renderer *, const SDL_Rect *))
FM_SDL_FN(void, SDL_RenderGetClipRect, (SDL_Renderer *, SDL_Rect *))
FM_SDL_FN(int, SDL_RenderSetScale, (SDL_Renderer *, float, float))
FM_SDL_FN(int, SDL_RenderSetVSync, (SDL_Renderer *, int))
FM_SDL_FN(int, SDL_RenderReadPixels, (SDL_Renderer *, const SDL_Rect *, Uint32, void *, int))
FM_SDL_FN(int, SDL_SetRenderTarget, (SDL_Renderer *, SDL_Texture *))
FM_SDL_FN(int, SDL_RenderFlush, (SDL_Renderer *))

/* ---- textures and surfaces ---------------------------------------------- */
FM_SDL_FN(SDL_Texture *, SDL_CreateTexture, (SDL_Renderer *, Uint32, int, int, int))
FM_SDL_FN(SDL_Texture *, SDL_CreateTextureFromSurface, (SDL_Renderer *, SDL_Surface *))
FM_SDL_FN(void, SDL_DestroyTexture, (SDL_Texture *))
FM_SDL_FN(int, SDL_UpdateTexture, (SDL_Texture *, const SDL_Rect *, const void *, int))
FM_SDL_FN(int, SDL_UpdateYUVTexture, (SDL_Texture *, const SDL_Rect *, const Uint8 *, int, const Uint8 *, int, const Uint8 *, int))
FM_SDL_FN(int, SDL_LockTexture, (SDL_Texture *, const SDL_Rect *, void **, int *))
FM_SDL_FN(void, SDL_UnlockTexture, (SDL_Texture *))
FM_SDL_FN(int, SDL_SetTextureBlendMode, (SDL_Texture *, SDL_BlendMode))
FM_SDL_FN(int, SDL_SetTextureAlphaMod, (SDL_Texture *, Uint8))
FM_SDL_FN(int, SDL_SetTextureColorMod, (SDL_Texture *, Uint8, Uint8, Uint8))
FM_SDL_FN(int, SDL_SetTextureScaleMode, (SDL_Texture *, SDL_ScaleMode))
FM_SDL_FN(int, SDL_QueryTexture, (SDL_Texture *, Uint32 *, int *, int *, int *))
FM_SDL_FN(SDL_Surface *, SDL_CreateRGBSurfaceWithFormat, (Uint32, int, int, int, Uint32))
FM_SDL_FN(SDL_Surface *, SDL_CreateRGBSurfaceWithFormatFrom, (void *, int, int, int, int, Uint32))
FM_SDL_FN(void, SDL_FreeSurface, (SDL_Surface *))

/* ---- events and input --------------------------------------------------- */
FM_SDL_FN(int, SDL_PollEvent, (SDL_Event *))
FM_SDL_FN(int, SDL_WaitEventTimeout, (SDL_Event *, int))
FM_SDL_FN(int, SDL_PushEvent, (SDL_Event *))
FM_SDL_FN(Uint32, SDL_RegisterEvents, (int))
FM_SDL_FN(void, SDL_FlushEvent, (Uint32))
FM_SDL_FN(Uint8, SDL_EventState, (Uint32, int))
FM_SDL_FN(void, SDL_StartTextInput, (void))
FM_SDL_FN(void, SDL_StopTextInput, (void))
FM_SDL_FN(SDL_bool, SDL_IsTextInputActive, (void))
FM_SDL_FN(void, SDL_SetTextInputRect, (const SDL_Rect *))
FM_SDL_FN(SDL_bool, SDL_HasScreenKeyboardSupport, (void))
FM_SDL_FN(SDL_Keymod, SDL_GetModState, (void))
FM_SDL_FN(Uint32, SDL_GetMouseState, (int *, int *))
FM_SDL_FN(int, SDL_CaptureMouse, (SDL_bool))
FM_SDL_FN(int, SDL_ShowCursor, (int))
FM_SDL_FN(SDL_Cursor *, SDL_CreateSystemCursor, (SDL_SystemCursor))
FM_SDL_FN(void, SDL_SetCursor, (SDL_Cursor *))
FM_SDL_FN(void, SDL_FreeCursor, (SDL_Cursor *))
FM_SDL_FN(int, SDL_SetClipboardText, (const char *))
FM_SDL_FN(char *, SDL_GetClipboardText, (void))
FM_SDL_FN(SDL_bool, SDL_HasClipboardText, (void))

/* ---- time --------------------------------------------------------------- */
FM_SDL_FN(Uint32, SDL_GetTicks, (void))
FM_SDL_FN(Uint64, SDL_GetTicks64, (void))
FM_SDL_FN(Uint64, SDL_GetPerformanceCounter, (void))
FM_SDL_FN(Uint64, SDL_GetPerformanceFrequency, (void))
FM_SDL_FN(void, SDL_Delay, (Uint32))
FM_SDL_FN(SDL_TimerID, SDL_AddTimer, (Uint32, SDL_TimerCallback, void *))
FM_SDL_FN(SDL_bool, SDL_RemoveTimer, (SDL_TimerID))

/* ---- threads ------------------------------------------------------------ */
/* SDL_CreateThread is a macro over a 5-argument function on Windows; it is
** loaded but not redirected: call fm_thread_create (fsdl.h) instead. */
#ifdef _WIN32
FM_SDL_FX(SDL_Thread *, SDL_CreateThread, (SDL_ThreadFunction, const char *, void *, pfnSDL_CurrentBeginThread, pfnSDL_CurrentEndThread))
#else
FM_SDL_FX(SDL_Thread *, SDL_CreateThread, (SDL_ThreadFunction, const char *, void *))
#endif
FM_SDL_FN(void, SDL_WaitThread, (SDL_Thread *, int *))
FM_SDL_FN(void, SDL_DetachThread, (SDL_Thread *))
FM_SDL_FN(int, SDL_SetThreadPriority, (SDL_ThreadPriority))
FM_SDL_FN(SDL_mutex *, SDL_CreateMutex, (void))
FM_SDL_FN(void, SDL_DestroyMutex, (SDL_mutex *))
FM_SDL_FN(int, SDL_LockMutex, (SDL_mutex *))
FM_SDL_FN(int, SDL_UnlockMutex, (SDL_mutex *))
FM_SDL_FN(SDL_cond *, SDL_CreateCond, (void))
FM_SDL_FN(void, SDL_DestroyCond, (SDL_cond *))
FM_SDL_FN(int, SDL_CondSignal, (SDL_cond *))
FM_SDL_FN(int, SDL_CondBroadcast, (SDL_cond *))
FM_SDL_FN(int, SDL_CondWait, (SDL_cond *, SDL_mutex *))
FM_SDL_FN(int, SDL_CondWaitTimeout, (SDL_cond *, SDL_mutex *, Uint32))
FM_SDL_FN(SDL_sem *, SDL_CreateSemaphore, (Uint32))
FM_SDL_FN(void, SDL_DestroySemaphore, (SDL_sem *))
FM_SDL_FN(int, SDL_SemWait, (SDL_sem *))
FM_SDL_FN(int, SDL_SemWaitTimeout, (SDL_sem *, Uint32))
FM_SDL_FN(int, SDL_SemPost, (SDL_sem *))
FM_SDL_FN(int, SDL_AtomicGet, (SDL_atomic_t *))
FM_SDL_FN(int, SDL_AtomicSet, (SDL_atomic_t *, int))
FM_SDL_FN(int, SDL_AtomicAdd, (SDL_atomic_t *, int))
FM_SDL_FN(SDL_bool, SDL_AtomicCAS, (SDL_atomic_t *, int, int))
FM_SDL_FN(void *, SDL_AtomicGetPtr, (void **))
FM_SDL_FN(void *, SDL_AtomicSetPtr, (void **, void *))

/* ---- audio -------------------------------------------------------------- */
FM_SDL_FN(SDL_AudioDeviceID, SDL_OpenAudioDevice, (const char *, int, const SDL_AudioSpec *, SDL_AudioSpec *, int))
FM_SDL_FN(void, SDL_CloseAudioDevice, (SDL_AudioDeviceID))
FM_SDL_FN(void, SDL_PauseAudioDevice, (SDL_AudioDeviceID, int))
FM_SDL_FN(void, SDL_LockAudioDevice, (SDL_AudioDeviceID))
FM_SDL_FN(void, SDL_UnlockAudioDevice, (SDL_AudioDeviceID))
FM_SDL_FN(SDL_AudioStatus, SDL_GetAudioDeviceStatus, (SDL_AudioDeviceID))
FM_SDL_FN(int, SDL_QueueAudio, (SDL_AudioDeviceID, const void *, Uint32))
FM_SDL_FN(Uint32, SDL_GetQueuedAudioSize, (SDL_AudioDeviceID))
FM_SDL_FN(void, SDL_ClearQueuedAudio, (SDL_AudioDeviceID))
FM_SDL_FN(SDL_AudioStream *, SDL_NewAudioStream, (const SDL_AudioFormat, const Uint8, const int, const SDL_AudioFormat, const Uint8, const int))
FM_SDL_FN(int, SDL_AudioStreamPut, (SDL_AudioStream *, const void *, int))
FM_SDL_FN(int, SDL_AudioStreamGet, (SDL_AudioStream *, void *, int))
FM_SDL_FN(int, SDL_AudioStreamAvailable, (SDL_AudioStream *))
FM_SDL_FN(int, SDL_AudioStreamFlush, (SDL_AudioStream *))
FM_SDL_FN(void, SDL_AudioStreamClear, (SDL_AudioStream *))
FM_SDL_FN(void, SDL_FreeAudioStream, (SDL_AudioStream *))

/* ---- added by the panels / file operations code ------------------------- */
FM_SDL_FN(int, SDL_GetNumVideoDisplays, (void))
#ifdef __ANDROID__
FM_SDL_FN(const char *, SDL_AndroidGetInternalStoragePath, (void))
#endif

/* ---- added by the archive code ------------------------------------------ */

/* ---- added by the viewers / media code ---------------------------------- */

/* ---- added by the platform code ----------------------------------------- */
#ifdef __ANDROID__
FM_SDL_FN(void *, SDL_AndroidGetJNIEnv, (void))
FM_SDL_FN(void *, SDL_AndroidGetActivity, (void))
#endif

#ifdef FM_SDL_FX_LOCAL
#  undef FM_SDL_FX
#  undef FM_SDL_FX_LOCAL
#endif
