
#include "stdafx.h"

BOOL WINAPI DllMain(
    HINSTANCE hinstDLL,  // handle to DLL module
    DWORD fdwReason,     // reason for calling function
    LPVOID lpvReserved)  // reserved
{
    // Perform actions based on the reason for calling.
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        // Initialize once for each new process.
        // Return FALSE to fail DLL load.
        g_hinstDLL = hinstDLL;
        break;

    case DLL_PROCESS_DETACH:
        break;

    default:
        break;
    }
    return TRUE;  // Successful DLL_PROCESS_ATTACH.
}


