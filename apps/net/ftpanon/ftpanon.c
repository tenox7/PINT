/*++

Module Name:

    ftpanon.c

Abstract:

    Makes the account the FTP Server (ftpsvc) logs an anonymous user on as
    (AnonymousUsername of its Parameters key): a user with the password
    given, which does not expire and which the user cannot change, a member
    of the local group Guests. Then it stores the password in the LSA secret
    the service reads it from, FTPD_ANONYMOUS_DATA, as the FTP Server dialog
    of the network control panel does when the service is installed. Without
    the secret the service allows no anonymous logon. The first boot runs it
    (registry.ini).

    NET USER /ADD cannot make the account there: it asks the Workstation
    service for the role of the machine first, and that service is not
    running in the first boot's script (nor on a machine without a network).
    The calls below go to the SAM of the machine.

    ftpanon name password

--*/

#include <nt.h>
#include <ntrtl.h>
#include <nturtl.h>
#include <ntlsa.h>
#include <windows.h>
#include <lm.h>
#include <stdio.h>

#define ANONYMOUS_SECRET L"FTPD_ANONYMOUS_DATA"

int __cdecl
main(
    int argc,
    char **argv
    )
{
    OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING SecretName, Password;
    ANSI_STRING AnsiPassword;
    LSA_HANDLE Policy, Secret;
    NTSTATUS Status;
    WCHAR Name[UNLEN + 1], PasswordBuffer[PWLEN + 1], Domain[DNLEN + 1];
    UCHAR Sid[128];
    DWORD SidLength = sizeof(Sid), DomainLength = DNLEN + 1, Parameter = 0;
    SID_NAME_USE Use;
    USER_INFO_1 User;
    USER_INFO_1011 FullName;
    NET_API_STATUS Error;

    if (argc != 3 ||
        !MultiByteToWideChar(CP_ACP, 0, argv[1], -1, Name, UNLEN + 1) ||
        !MultiByteToWideChar(CP_ACP, 0, argv[2], -1, PasswordBuffer, PWLEN + 1)) {
        printf("\nusage: ftpanon name password\n");
        return 1;
    }

    //
    // The account, if there is none of the name yet.
    //

    memset(&User, 0, sizeof(User));
    User.usri1_name = Name;
    User.usri1_password = PasswordBuffer;
    User.usri1_priv = USER_PRIV_USER;
    User.usri1_comment = L"FTP Server anonymous access";
    User.usri1_flags = UF_SCRIPT | UF_NORMAL_ACCOUNT | UF_DONT_EXPIRE_PASSWD | UF_PASSWD_CANT_CHANGE;

    Error = NetUserAdd(NULL, 1, (LPBYTE)&User, &Parameter);
    if (Error == NERR_Success) {
        FullName.usri1011_full_name = L"FTP Guest Account";
        NetUserSetInfo(NULL, Name, 1011, (LPBYTE)&FullName, NULL);
    } else if (Error != NERR_UserExists) {
        printf("ftpanon: cannot make the account %s (error %lu, parameter %lu)\n", argv[1], Error, Parameter);
        return 1;
    }

    //
    // A member of Guests (NT 3.5 adds a member by its SID).
    //

    if (!LookupAccountNameW(NULL, Name, (PSID)Sid, &SidLength, Domain, &DomainLength, &Use)) {
        printf("ftpanon: cannot find the account %s (error %lu)\n", argv[1], GetLastError());
        return 1;
    }

    Error = NetLocalGroupAddMember(NULL, L"Guests", (PSID)Sid);
    if (Error != NERR_Success && Error != ERROR_MEMBER_IN_ALIAS) {
        printf("ftpanon: cannot add %s to Guests (error %lu)\n", argv[1], Error);
        return 1;
    }

    //
    // The secret.
    //

    InitializeObjectAttributes(&Attributes, NULL, 0, NULL, NULL);
    Status = LsaOpenPolicy(NULL, &Attributes, POLICY_CREATE_SECRET, &Policy);
    if (!NT_SUCCESS(Status)) {
        printf("ftpanon: cannot open the LSA policy (%08lX)\n", Status);
        return 1;
    }

    RtlInitUnicodeString(&SecretName, ANONYMOUS_SECRET);
    Status = LsaCreateSecret(Policy, &SecretName, SECRET_SET_VALUE, &Secret);
    if (Status == STATUS_OBJECT_NAME_COLLISION) {
        Status = LsaOpenSecret(Policy, &SecretName, SECRET_SET_VALUE, &Secret);
    }

    if (!NT_SUCCESS(Status)) {
        printf("ftpanon: cannot open the secret (%08lX)\n", Status);
        LsaClose(Policy);
        return 1;
    }

    RtlInitAnsiString(&AnsiPassword, argv[2]);
    Status = RtlAnsiStringToUnicodeString(&Password, &AnsiPassword, TRUE);
    if (NT_SUCCESS(Status)) {
        Status = LsaSetSecret(Secret, &Password, NULL);
        RtlFreeUnicodeString(&Password);
    }

    LsaClose(Secret);
    LsaClose(Policy);

    if (!NT_SUCCESS(Status)) {
        printf("ftpanon: cannot set the secret (%08lX)\n", Status);
        return 1;
    }

    printf("ftpanon: %s is the anonymous user of the FTP Server\n", argv[1]);
    return 0;
}
