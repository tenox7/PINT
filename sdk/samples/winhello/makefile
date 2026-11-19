# winhello.exe, a Windows program with resources: nmake
winhello.exe: winhello.c winhello.h winhello.res
    cl -nologo -W3 -O2 winhello.c winhello.res -link -subsystem:windows

winhello.res: winhello.rc winhello.h
    rc -r winhello.rc

clean:
    -del winhello.obj winhello.res winhello.exe
