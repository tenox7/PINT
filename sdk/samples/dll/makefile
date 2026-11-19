# greet.dll, a DLL, and usegreet.exe, which calls it; both with the static run-time: nmake
all: greet.dll usegreet.exe

greet.dll: greet.c
    cl -nologo -W3 -O2 -LD greet.c

usegreet.exe: usegreet.c greet.dll
    cl -nologo -W3 -O2 usegreet.c greet.lib

clean:
    -del greet.obj greet.dll greet.lib greet.exp usegreet.obj usegreet.exe
