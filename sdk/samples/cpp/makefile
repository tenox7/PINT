# shapes.exe, a C++ console program with exceptions (-EHsc) and <iostream.h>, whose
# library is msvcirt.lib with the run-time DLL (msvcrt40.dll, msvcrt.dll): nmake
!include <win32.mak>

all: shapes.exe

shapes.obj: shapes.cpp
    $(cc) $(cdebug) $(cflags) $(cvarsdll) -EHsc shapes.cpp

shapes.exe: shapes.obj
    $(link) $(ldebug) $(conlflags) -out:shapes.exe shapes.obj msvcirt.lib $(conlibsdll)

clean:
    -del shapes.obj shapes.exe
