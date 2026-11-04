/* usegreet: a console program that calls the DLL greet.dll. */
#include <stdio.h>

__declspec(dllimport) int Greet(char *buffer, int size);

int main(void)
{
    char text[300];

    Greet(text, sizeof text);
    printf("%s\n", text);
    return 0;
}
