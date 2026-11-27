# 7-Zip, the file archiver, an unofficial port of 7-Zip 9.20 to Windows NT 4.0, https://github.com/danifunker/nt4ports
# (folder 7zip): the zip for ARM of the newest release that has one (built there with the PINT 4.0 SDK), a folder with
# the file manager (7zFM), 7z, 7za, the Formats folder (RAR5 extraction) and the licences, into \APPS\7ZIP. The file
# manager is in Programs > Apps (syssetup.inf).
REPO=danifunker/nt4ports
GET="7zip-*-nt4-arm.zip"
DEST=APPS/7ZIP
FILES="7zip-*-nt4-arm/*"
