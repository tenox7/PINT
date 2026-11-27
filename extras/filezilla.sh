# FileZilla, the FTP and SFTP client, an unofficial port of FileZilla 2.2.32 to Windows NT 4.0,
# https://github.com/danifunker/nt4ports (folder filezilla): the zip for ARM of the newest release that has one (built
# there with the PINT 4.0 SDK), a folder with FileZilla, its SFTP helper, its map (to read a crash report with) and the
# licences, into \APPS\FILEZILLA. In Programs > Apps (syssetup.inf), started in its folder, where it keeps its settings.
REPO=danifunker/nt4ports
GET="filezilla-*-nt4-arm.zip"
DEST=APPS/FILEZILLA
FILES="filezilla-*-nt4-arm/*"
