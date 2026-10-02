// Stage 0: pure forwarding proxy. No hooks, no threads. If the game hangs with this, the proxy approach itself is blocked.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
extern "C" BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
