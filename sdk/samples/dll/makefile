# greet.dll, a DLL, and usegreet.exe, which calls it; both with the run-time DLL
# (msvcrt.dll): nmake
!include <win32.mak>

all: greet.dll usegreet.exe

greet.obj: greet.c
    $(cc) $(cdebug) $(cflags) $(cvarsdll) greet.c

greet.dll: greet.obj
    $(link) $(ldebug) $(dlllflags) -out:greet.dll greet.obj $(conlibsdll)

usegreet.obj: usegreet.c
    $(cc) $(cdebug) $(cflags) $(cvarsdll) usegreet.c

usegreet.exe: usegreet.obj greet.dll
    $(link) $(ldebug) $(conlflags) -out:usegreet.exe usegreet.obj greet.lib $(conlibsdll)

clean:
    -del greet.obj greet.dll greet.lib greet.exp usegreet.obj usegreet.exe
