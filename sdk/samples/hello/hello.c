/* hello: a console program. It prints the version of Windows NT and its processors. */
#include <windows.h>
#include <stdio.h>

int main(void)
{
    OSVERSIONINFO version;
    SYSTEM_INFO system;

    version.dwOSVersionInfoSize = sizeof version;
    GetVersionEx(&version);
    GetSystemInfo(&system);
    printf("Hello from Windows NT %lu.%lu (build %lu) on %lu processor(s).\n",
           version.dwMajorVersion, version.dwMinorVersion, version.dwBuildNumber,
           system.dwNumberOfProcessors);
    return 0;
}
