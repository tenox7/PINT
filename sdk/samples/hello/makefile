# hello.exe, a console program with the static run-time (libcmt): nmake
# (nmake NODEBUG=1 builds it without debug information)
!include <win32.mak>

all: hello.exe

hello.obj: hello.c
    $(cc) $(cdebug) $(cflags) $(cvarsmt) hello.c

hello.exe: hello.obj
    $(link) $(ldebug) $(conlflags) -out:hello.exe hello.obj $(conlibsmt)

clean:
    -del hello.obj hello.exe
