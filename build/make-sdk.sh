#!/usr/bin/env bash
# Build the PINT 4.0 SDK (sdk/README.txt): obj/sdk/pint40sdk, the SDK as it unzips (text
# files CRLF), obj/pint40sdk.zip of it, and obj/sdk/docker, the context of the image
# tenox7/pint40sdk (docker build -t tenox7/pint40sdk obj/sdk/docker).
# - The headers PINT's components compile with: public\sdk\inc and its run-time headers as
#   the farm has them (mkfarm.sh), with the port's stdarg.h (that of the run-time has no
#   ARM) and stcrt.h, the SDK's win32.mak with ARM (sdk/win32.mak.ntdiff), ntwin32.mak and
#   sehmap.h.
# - PINT's libraries (obj/um): the import libraries of its DLLs, the C run-times with new[]
#   and delete[] (crt/cxx/newarray.cxx: the C++ of today calls them), the static libraries
#   of the SDK and its link objects. Not in it: MFC, the libraries of NetDDE (nddeapi),
#   printing (win32spl), the VDM (vdmdbg), pdh (no pdh.dll on PINT), ntdll and crtdll.
# - The SDK's cl and link (sdk/pintwrap.c), with clang for Windows on x64 and arm64.
#
# Usage:  build/make-sdk.sh
set -euo pipefail
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
NT40=$(cd $R/.. && pwd)
O=$R/obj/sdk
rm -rf $O $R/obj/pint40sdk.zip
mkdir -p $O

docker run --rm -v "$NT40/../nt35/msvc":/opt/msvc:ro -v "$NT40/../nt35":/nt35:ro -v "$NT40/../XPSP1":/XPSP1:ro -v "$NT40":/work \
    -v "$NT40/private":/work/private:ro -v "$NT40/public":/work/public:ro -w /work/RASPI/build --entrypoint bash tenox7/msvc-arm32 -c '
set -eu
. ./msvc.sh
bash mkfarm.sh /tmp/f
S=/work/RASPI/sdk O=/work/RASPI/obj/sdk/pint40sdk U=/work/RASPI/obj/um
mkdir -p $O/include $O/lib $O/bin

echo ">> headers"
cp -r /tmp/f/pub/. $O/include/
cp -r /tmp/f/crt $O/include/crt
cp $K/inc/stdarg.h $O/include/crt/stdarg.h
cp $K/inc/stcrt.h $S/win32.mak /work/private/sdktools/jetadmin/inc/ntwin32.mak /work/private/sdktools/jetadmin/inc/sehmap.h $O/include/

echo ">> libraries"
for l in advapi32 comctl32 comdlg32 ddraw dlcapi dplay dsound gdi32 glu32 imagehlp imm32 kernel32 loadperf lz32 mgmtapi mpr \
    msacm32 mswsock netapi32 ole32 oleaut32 oledlg opengl32 rasapi32 rasdlg rassapi rpcns4 rpcrt4 setupapi shell32 snmp \
    snmpapi tapi32 user32 version vfw32 winmm winspool winstrm wintrust ws2_32 wsock32 \
    fdi glaux rpcndr scrnsave uuid uuid2 libcpsx psxdll psxrtl msvcirt oldnames; do
    cp $U/$l.lib $O/lib/
done
cp $U/binmode.obj $U/commode.obj $U/setargv.obj $O/lib/
ntcc /tmp/newarray.obj /work/RASPI/crt/cxx/newarray.cxx
for l in libc libcmt msvcrt; do
    lib -nologo -out:$O/lib/$l.lib $U/$l.lib /tmp/newarray.obj > /tmp/lib.err 2>&1 || { cat /tmp/lib.err; exit 1; }
done

echo ">> cl and link"
mkdir -p /tmp/w
{ echo "LIBRARY kernel32.dll"; echo EXPORTS
  for f in CloseHandle CreateFileW CreateProcessW ExitProcess GetCommandLineW GetEnvironmentVariableW GetExitCodeProcess \
      GetFileAttributesW GetFileSize GetModuleFileNameW GetProcessHeap GetStdHandle HeapAlloc MultiByteToWideChar ReadFile \
      SetConsoleCtrlHandler SetEnvironmentVariableW SetFilePointer WaitForSingleObject WideCharToMultiByte WriteFile; do echo $f; done
} > /tmp/w/kernel32.def
printf "LIBRARY shell32.dll\nEXPORTS\nCommandLineToArgvW\n" > /tmp/w/shell32.def
for h in x64:x86_64:i386:x86-64 arm64:aarch64:arm64; do
    d=${h%%:*} t=$(echo $h | cut -d: -f2) m=$(echo $h | cut -d: -f3-)
    mkdir -p /tmp/w/$d $O/bin/$d
    llvm-dlltool -m $m -d /tmp/w/kernel32.def -l /tmp/w/$d/kernel32.lib
    llvm-dlltool -m $m -d /tmp/w/shell32.def -l /tmp/w/$d/shell32.lib
    clang --target=$t-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector -Wall -c $S/pintwrap.c -o /tmp/w/$d/pintwrap.obj
    lld-link -nologo -nodefaultlib -entry:start -subsystem:console -out:$O/bin/$d/cl.exe /tmp/w/$d/pintwrap.obj \
        /tmp/w/$d/kernel32.lib /tmp/w/$d/shell32.lib
    cp $O/bin/$d/cl.exe $O/bin/$d/link.exe
done

echo ">> setenv, README, samples"
cp $S/setenv.cmd $S/README.txt $O/
cp -r $S/samples $O/
find $O -type f ! -name "*.lib" ! -name "*.obj" ! -name "*.exe" -exec sed -i "s/\r$//; s/$/\r/" {} +
'

echo ">> pint40sdk.zip, docker"
(cd $O && zip -q -r -X -9 $R/obj/pint40sdk.zip pint40sdk)
mkdir -p $O/docker
cp -R $O/pint40sdk $O/docker/
cp $R/sdk/docker/Dockerfile $R/sdk/docker/pint-env $R/sdk/docker/pint-entrypoint $R/sdk/docker/wine-sdk $R/sdk/docker/cl \
    $R/sdk/docker/link $O/docker/
ls -l $R/obj/pint40sdk.zip
