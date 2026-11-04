/* greet.dll: a DLL. Greet writes a greeting with the path of the DLL into a buffer. */
#include <windows.h>
#include <stdio.h>

static HINSTANCE Module;

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        Module = instance;
    return TRUE;
}

__declspec(dllexport) int Greet(char *buffer, int size)
{
    char path[MAX_PATH];

    GetModuleFileName(Module, path, sizeof path);
    return _snprintf(buffer, size, "Hello from %s.", path);
}
