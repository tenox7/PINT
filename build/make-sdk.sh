#!/usr/bin/env bash
# Build the PINT 3.5 SDK (sdk/README.txt): obj/sdk/pint35sdk, the SDK as it unzips (text
# files CRLF), obj/pint35sdk.zip of it, and obj/sdk/docker, the context of the image
# tenox7/pint35sdk (docker build -t tenox7/pint35sdk obj/sdk/docker).
# - The headers PINT's components compile with: PUBLIC\SDK\INC and its run-time headers as
#   the farm has them (mkfarm.sh), with the port's stdarg.h (that of the run-time has no
#   ARM) and stcrt.h; and ntwin32.mak, that of the Win32 SDK for NT 3.5 with ARM
#   (sdk/ntwin32.mak.ntdiff).
# - PINT's libraries (obj/um): the import libraries of its DLLs, the static C run-times with
#   new[] and delete[] (crt/cxx/newarray.cxx: the C++ of today calls them), the static
#   libraries of the SDK and the link objects. Not in it: crtdll (cl's -MD names msvcrt,
#   which NT 3.5 does not have), ntdll, and the libraries of NT's own components.
# - The driver kit: the headers of NT 3.5's DDK in include\ddk (those of PUBLIC\OAK\INC,
#   those the header pass makes for drivers, as ntddk.h and miniport.h, and those of
#   PRIVATE\NTOS\INC they include or the DDK had, as video.h), the libraries of kernel mode
#   (obj): the import libraries of ntoskrnl, the HAL, videoprt, ndis and scsiport, and
#   libcntpr; and winsrv.lib, whose graphics engine a display driver of NT 3.5 calls.
# - The SDK's cl and link (sdk/pintwrap.c), with clang for Windows on x64 and arm64.
#
# Usage:  build/make-sdk.sh
set -euo pipefail
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
NT35=$(cd $R/.. && pwd)
O=$R/obj/sdk
rm -rf $O $R/obj/pint35sdk.zip
mkdir -p $O

docker run --rm -v "$NT35/msvc":/opt/msvc:ro -v "$NT35":/work -v "$NT35/PRIVATE":/work/PRIVATE:ro \
    -v "$NT35/PUBLIC":/work/PUBLIC:ro -w /work/RASPI/build --entrypoint bash tenox7/msvc-arm32 -c '
set -eu
. ./msvc.sh
bash mkfarm.sh /tmp/f
S=/work/RASPI/sdk O=/work/RASPI/obj/sdk/pint35sdk U=/work/RASPI/obj/um
mkdir -p $O/include $O/lib $O/bin

echo ">> headers"
cp -r /tmp/f/pub/. $O/include/
cp -r /tmp/f/crt $O/include/crt
cp $K/inc/stdarg.h $O/include/crt/stdarg.h
cp $K/inc/stcrt.h $S/ntwin32.mak $O/include/

echo ">> libraries"
for l in advapi32 comctl32 comdlg32 gdi32 imagehlp kernel32 lz32 mpr msacm32 nddeapi netapi32 ole32 oleaut32 \
    olecli32 olesvr32 rpcns4 rpcrt4 shell32 user32 version winmm winspool winstrm wsock32 \
    rpcndr scrnsave uuid; do
    cp $U/$l.lib $O/lib/
done
cp $U/binmode.obj $U/commode.obj $U/setargv.obj $O/lib/
ntcc /tmp/newarray.obj /work/RASPI/crt/cxx/newarray.cxx
for l in libc libcmt; do
    lib -nologo -out:$O/lib/$l.lib $U/$l.lib /tmp/newarray.obj > /tmp/lib.err 2>&1 || { cat /tmp/lib.err; exit 1; }
done

echo ">> driver kit"
mkdir -p $O/include/ddk
cp /tmp/f/oak/*.h $O/include/ddk/
for h in ntddk.h bugcodes.h ntiologc.h miniport.h ndis.h; do
    cp /work/RASPI/obj/gen/farm/priv/$h $O/include/ddk/
done
for h in exlevels.h video.h srb.h scsi.h tape.h afilter.h efilter.h tfilter.h ffilter.h; do
    cp /tmp/f/priv/$h $O/include/ddk/
done
for l in ntoskrnl hal videoprt ndis scsiport libcntpr; do
    cp /work/RASPI/obj/$l.lib $O/lib/
done
cp $U/winsrv.lib $O/lib/

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

echo ">> pint35sdk.zip, docker"
(cd $O && zip -q -r -X -9 $R/obj/pint35sdk.zip pint35sdk)
mkdir -p $O/docker
cp -R $O/pint35sdk $O/docker/
cp $R/sdk/docker/Dockerfile $R/sdk/docker/pint-env $R/sdk/docker/pint-entrypoint $R/sdk/docker/wine-sdk $R/sdk/docker/cl \
    $R/sdk/docker/link $O/docker/
ls -l $R/obj/pint35sdk.zip
