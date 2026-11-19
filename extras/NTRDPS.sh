# ntrdps, an RDP server for the console of Windows NT, https://github.com/tenox7/ntrdps: its installer for PINT 3.5,
# ntrdpset.exe, from the newest release that has it (built there with the PINT 3.5 SDK), into \NTRDPS, with the
# LICENSE and README.md of that release and the license of its BearSSL (bearssl.txt). It is not installed: the user
# runs the installer, which also asks for the account clients log on with.
REPO=tenox7/ntrdps
GET="ntrdpset.exe=ntrdpset-pint35.exe LICENSE=/LICENSE README.md=/README.md bearssl.txt=/src/tls/bearssl/LICENSE.txt"
BUILD='perl -pi -e "s/\r*\$/\r/" LICENSE README.md bearssl.txt'
DEST=NTRDPS
FILES="ntrdpset.exe LICENSE README.md bearssl.txt"
