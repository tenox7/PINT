/*++

Module Name:

    ftpuser.c

Abstract:

    Makes the account of the anonymous user of the FTP Publishing Service, as
    the setup of Peer Web Services makes it (IUSR_<machine>): no password,
    which does not expire and which the user cannot change, a member of the
    local group Guests. The first boot runs it (REGISTRY.INI): NET USER /ADD
    cannot make the account there, since it asks the Workstation service for
    the role of the machine first, and that service is not running then (nor
    on a machine without a network). The calls here go to the SAM of the
    machine.

    ftpuser name

--*/

#include <windows.h>
#include <lm.h>
#include <stdio.h>

int __cdecl
main(
    int argc,
    char **argv
    )
{
    WCHAR Name[UNLEN + 1];
    USER_INFO_1 User;
    USER_INFO_1011 FullName;
    LOCALGROUP_MEMBERS_INFO_3 Member;
    NET_API_STATUS Status;
    DWORD Parameter = 0;

    if (argc != 2 || !MultiByteToWideChar(CP_ACP, 0, argv[1], -1, Name, UNLEN + 1)) {
        printf("\nusage: ftpuser name\n");
        return 1;
    }

    memset(&User, 0, sizeof(User));
    User.usri1_name = Name;
    User.usri1_password = L"";
    User.usri1_priv = USER_PRIV_USER;
    User.usri1_comment = L"Internet Server Anonymous Access";
    User.usri1_flags = UF_SCRIPT | UF_NORMAL_ACCOUNT | UF_DONT_EXPIRE_PASSWD | UF_PASSWD_CANT_CHANGE;

    Status = NetUserAdd(NULL, 1, (LPBYTE)&User, &Parameter);
    if (Status == NERR_UserExists) {
        printf("ftpuser: %s exists.\n", argv[1]);
    } else if (Status != NERR_Success) {
        printf("NetUserAdd: error %lu (parameter %lu).\n", Status, Parameter);
        return 1;
    } else {
        FullName.usri1011_full_name = L"Internet Guest Account";
        NetUserSetInfo(NULL, Name, 1011, (LPBYTE)&FullName, NULL);
    }

    Member.lgrmi3_domainandname = Name;
    Status = NetLocalGroupAddMembers(NULL, L"Guests", 3, (LPBYTE)&Member, 1);
    if (Status != NERR_Success && Status != ERROR_MEMBER_IN_ALIAS) {
        printf("NetLocalGroupAddMembers: error %lu.\n", Status);
        return 1;
    }

    printf("ftpuser: %s is the anonymous user.\n", argv[1]);
    return 0;
}
