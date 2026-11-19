# NTPERF3X, an NT 4-style task manager, https://github.com/tenox7/nt31taskman: its build for PINT 3.5, from the newest
# release that has it (built there with the PINT 3.5 SDK), into \APPS\TASKMAN. The default hive's Winlogon\Taskman
# starts it in place of Program Manager's Task List (ported/oak/software.ini.ntdiff).
REPO=tenox7/nt31taskman
GET="taskman.exe=taskman-pint35-arm.exe"
DEST=APPS/TASKMAN
FILES=taskman.exe
