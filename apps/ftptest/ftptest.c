/*
 *  ftptest [HOST [USER [PASSWORD]]]: a test of an FTP server (the FTP Server of the
 *  machine by default, as its anonymous user): logs on, lists the directory in passive
 *  and in active mode, stores a file, reads it back and deletes it, printing the
 *  dialogue. For the builds' tests of the FTP Server, where there is no other client.
 */
#include <windows.h>
#include <winsock.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SOCKET Control;
static char Line[2048];

/* a reply of the server, all of its lines; its code */
static int Reply(void)
{
    int n = 0, code = -1;
    char c;

    for (;;) {
        if (recv(Control, &c, 1, 0) != 1) {
            printf("<< (the connection closed, error %d)\n", WSAGetLastError());
            return -1;
        }
        if (c == '\r') continue;
        if (c != '\n') {
            if (n < sizeof(Line) - 1) Line[n++] = c;
            continue;
        }
        Line[n] = 0;
        printf("<< %s\n", Line);
        if (n >= 4 && Line[3] == ' ' && Line[0] >= '0' && Line[0] <= '9') {
            code = atoi(Line);
            break;
        }
        n = 0;
    }
    return code;
}

static int Command(const char *text)
{
    char buffer[512];

    printf(">> %s\n", text);
    sprintf(buffer, "%s\r\n", text);
    send(Control, buffer, strlen(buffer), 0);
    return Reply();
}

/* the data of a transfer: printed (at most 2000 bytes of it), or sent */
static void Data(SOCKET s, const char *send_text)
{
    char buffer[512];
    int n, total = 0;

    if (send_text != NULL) {
        send(s, send_text, strlen(send_text), 0);
    } else {
        while ((n = recv(s, buffer, sizeof(buffer) - 1, 0)) > 0) {
            buffer[n] = 0;
            if (total < 2000) printf("%s", buffer);
            total += n;
        }
        printf("   (%d bytes of data, error %d)\n", total, n < 0 ? WSAGetLastError() : 0);
    }
    closesocket(s);
}

/* a command with a data connection, in passive mode */
static void Passive(const char *text, const char *send_text)
{
    struct sockaddr_in a;
    unsigned h1, h2, h3, h4, p1, p2;
    char *p;
    SOCKET s;

    if (Command("PASV") != 227 || (p = strchr(Line, '(')) == NULL ||
        sscanf(p + 1, "%u,%u,%u,%u,%u,%u", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        printf("   (no passive mode)\n");
        return;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl((h1 << 24) | (h2 << 16) | (h3 << 8) | h4);
    a.sin_port = htons((u_short)((p1 << 8) | p2));
    s = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        printf("   (cannot connect to the passive port, error %d)\n", WSAGetLastError());
        closesocket(s);
        return;
    }
    if (Command(text) >= 400) {
        closesocket(s);
        return;
    }
    Data(s, send_text);
    Reply();
}

/* a command with a data connection, in active mode */
static void Active(const char *text)
{
    struct sockaddr_in a;
    int length = sizeof(a);
    char buffer[128];
    unsigned long host;
    unsigned port;
    SOCKET l, s;

    getsockname(Control, (struct sockaddr *)&a, &length);
    a.sin_port = 0;
    l = socket(AF_INET, SOCK_STREAM, 0);
    bind(l, (struct sockaddr *)&a, sizeof(a));
    listen(l, 1);
    length = sizeof(a);
    getsockname(l, (struct sockaddr *)&a, &length);
    host = ntohl(a.sin_addr.s_addr);
    port = ntohs(a.sin_port);
    sprintf(buffer, "PORT %lu,%lu,%lu,%lu,%u,%u", host >> 24, (host >> 16) & 255, (host >> 8) & 255, host & 255,
            port >> 8, port & 255);
    if (Command(buffer) >= 400 || Command(text) >= 400) {
        closesocket(l);
        return;
    }
    s = accept(l, NULL, NULL);
    closesocket(l);
    if (s == INVALID_SOCKET) {
        printf("   (no data connection, error %d)\n", WSAGetLastError());
        return;
    }
    Data(s, NULL);
    Reply();
}

int _CRTAPI1 main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *user = argc > 2 ? argv[2] : "anonymous";
    const char *password = argc > 3 ? argv[3] : "ftptest@";
    struct sockaddr_in a;
    struct hostent *he;
    char buffer[300];
    WSADATA wsa;
    int code;

    if (WSAStartup(0x0101, &wsa) != 0) {
        printf("ftptest: no Windows Sockets\n");
        return 1;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(21);
    a.sin_addr.s_addr = inet_addr(host);
    if (a.sin_addr.s_addr == INADDR_NONE) {
        if ((he = gethostbyname(host)) == NULL) {
            printf("ftptest: no host %s\n", host);
            return 1;
        }
        memcpy(&a.sin_addr, he->h_addr, 4);
    }
    Control = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(Control, (struct sockaddr *)&a, sizeof(a)) != 0) {
        printf("ftptest: cannot connect to %s, error %d\n", host, WSAGetLastError());
        return 1;
    }
    if (Reply() < 0) return 1;
    sprintf(buffer, "USER %s", user);
    code = Command(buffer);
    if (code == 331) {
        sprintf(buffer, "PASS %s", password);
        code = Command(buffer);
    }
    if (code != 230) {
        printf("ftptest: not logged on\n");
        return 1;
    }
    Command("SYST");
    Command("PWD");
    Command("TYPE A");
    Passive("LIST", NULL);
    Active("LIST");
    Passive("NLST", NULL);
    Command("TYPE I");
    Passive("STOR ftptest.tmp", "A file the FTP test stored.\r\n");
    Passive("RETR ftptest.tmp", NULL);
    Command("DELE ftptest.tmp");
    Command("QUIT");
    closesocket(Control);
    return 0;
}
