# winhello.exe, a Windows program with resources: nmake
!include <win32.mak>

all: winhello.exe

winhello.obj: winhello.c winhello.h
    $(cc) $(cdebug) $(cflags) $(cvarsmt) winhello.c

winhello.res: winhello.rc winhello.h
    $(rc) $(rcflags) $(rcvars) winhello.rc

winhello.exe: winhello.obj winhello.res
    $(link) $(ldebug) $(guilflags) -out:winhello.exe winhello.obj winhello.res $(guilibsmt)

clean:
    -del winhello.obj winhello.res winhello.exe
