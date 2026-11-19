/*
 * cl.exe and link.exe of the PINT 4.0 SDK (one program, by two names): runs the tool of its
 * name of Microsoft C for ARM, the next one on the PATH, then marks the image the tool
 * linked for Windows NT 4.0. The linker for ARM gives every image subsystem and OS version
 * 6.02, whatever -subsystem says (LNK4010), and NT 4.0 starts no program marked for a
 * later Windows than its own.
 *
 * cl also gets what the C of today needs for the headers and run-time of NT 4.0: _ARM_ (the
 * CPU define of win32.mak), no security checks, thread-safe statics or sized delete, wchar_t
 * as unsigned short, no warning C4391 for the Interlocked functions of winbase.h (declared
 * with PVOID where the intrinsic has long, the same on ARM: the intrinsic is still used);
 * and, when it links, the libraries a Visual C++ project links. Both take
 * the version off a -subsystem:NAME,X.Y (win32.mak's -subsystem:console,$(APPVER)), which
 * the linker would warn about, and give it to the image.
 *
 * A driver (-driver) is laid out on 0x20, as the NT build linked every driver (makefile.def's
 * -align:$(DRIVER_ALIGNMENT)), and an image linked with -align:N below a page on N: the linker
 * for ARM refuses that (LNK1246), so both take the option off the command and do it after it.
 * -align:0x1000 (a page or more) is the linker's.
 *
 * KERNEL32 and SHELL32 only, no C run-time: built with clang for the Windows of the host
 * (x64, arm64), which may be Wine.
 */

#define WINAPI __stdcall
#define IMPORT __declspec(dllimport)
typedef unsigned long DWORD;
typedef int BOOL;
typedef void *HANDLE;
typedef __SIZE_TYPE__ SIZE_T;
typedef __WCHAR_TYPE__ wchar_t;
typedef struct {
    DWORD cb;
    wchar_t *lpReserved, *lpDesktop, *lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    unsigned short wShowWindow, cbReserved2;
    unsigned char *lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW;
typedef struct { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION;

IMPORT wchar_t *WINAPI GetCommandLineW(void);
IMPORT DWORD WINAPI GetModuleFileNameW(HANDLE, wchar_t *, DWORD);
IMPORT DWORD WINAPI GetEnvironmentVariableW(const wchar_t *, wchar_t *, DWORD);
IMPORT BOOL WINAPI SetEnvironmentVariableW(const wchar_t *, const wchar_t *);
IMPORT DWORD WINAPI GetFileAttributesW(const wchar_t *);
IMPORT BOOL WINAPI CreateProcessW(const wchar_t *, wchar_t *, void *, void *, BOOL, DWORD, void *,
                                  const wchar_t *, STARTUPINFOW *, PROCESS_INFORMATION *);
IMPORT DWORD WINAPI WaitForSingleObject(HANDLE, DWORD);
IMPORT BOOL WINAPI GetExitCodeProcess(HANDLE, DWORD *);
IMPORT BOOL WINAPI CloseHandle(HANDLE);
IMPORT void WINAPI ExitProcess(unsigned);
IMPORT HANDLE WINAPI CreateFileW(const wchar_t *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
IMPORT BOOL WINAPI ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
IMPORT BOOL WINAPI WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
IMPORT DWORD WINAPI SetFilePointer(HANDLE, long, long *, DWORD);
IMPORT DWORD WINAPI GetFileSize(HANDLE, DWORD *);
IMPORT HANDLE WINAPI GetStdHandle(DWORD);
IMPORT HANDLE WINAPI GetProcessHeap(void);
IMPORT void *WINAPI HeapAlloc(HANDLE, DWORD, SIZE_T);
IMPORT int WINAPI MultiByteToWideChar(unsigned, DWORD, const char *, int, wchar_t *, int);
IMPORT int WINAPI WideCharToMultiByte(unsigned, DWORD, const wchar_t *, int, char *, int, const char *, BOOL *);
IMPORT BOOL WINAPI SetConsoleCtrlHandler(void *, BOOL);
IMPORT wchar_t **WINAPI CommandLineToArgvW(const wchar_t *, int *);

#define BAD_HANDLE ((HANDLE)(SIZE_T)-1)
#define MAX_TOKENS 8192

/* what cl gets ahead of the command line, and after it when it links */
static const wchar_t ClOptions[] = L"-D_ARM_=1 -GS- -Zc:wchar_t- -Zc:threadSafeInit- -Zc:sizedDealloc- -wd4391";
static const wchar_t ClLibraries[] = L"kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib "
                                     L"advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib";

static wchar_t Self[1024], Tool[64], Real[1024], Path[32768], Skip[8192], Out[1024];
static wchar_t *Tokens[MAX_TOKENS];
static int TokenCount, Cl, Dll, HasLink, Driver;
static unsigned Major = 4, Minor = 0;
static DWORD Align;

static void *alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), 8, n); }
static int len(const wchar_t *s) { int n = 0; while (s[n]) n++; return n; }
static wchar_t lower(wchar_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int eq(const wchar_t *a, const wchar_t *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static int ieq(const wchar_t *a, const wchar_t *b)
{
    while (*a && lower(*a) == lower(*b)) a++, b++;
    return *a == *b;
}
/* the rest of S after the prefix P (case is ignored with I), or 0 */
static const wchar_t *prefix(const wchar_t *s, const wchar_t *p, int i)
{
    while (*p && (i ? lower(*s) == lower(*p) : *s == *p)) s++, p++;
    return *p ? 0 : s;
}
static wchar_t *add(wchar_t *d, const wchar_t *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static int option(const wchar_t *t) { return t[0] == '/' || t[0] == '-'; }

static void say(const wchar_t *a, const wchar_t *b)
{
    static char m[4096];
    static wchar_t w[2048];
    DWORD n;
    add(add(add(add(w, Tool), L": "), a), b);
    n = WideCharToMultiByte(0, 0, w, -1, m, sizeof m, 0, 0);
    if (n > 1) {
        m[n - 1] = '\n';
        WriteFile(GetStdHandle((DWORD)-12), m, n, &n, 0);
    }
}

/* the file name part of a path, and its extension with the dot ("" when it has none) */
static const wchar_t *name(const wchar_t *p)
{
    const wchar_t *n = p;
    for (; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':') n = p + 1;
    return n;
}
static const wchar_t *extension(const wchar_t *p)
{
    const wchar_t *n = name(p), *e = 0;
    for (; *n; n++)
        if (*n == '.') e = n;
    return e ? e : n;
}

/* two directories the same, as Windows compares them (case, / or \, a trailing \) */
static int samedir(const wchar_t *a, int an, const wchar_t *b, int bn)
{
    while (an && (a[an - 1] == '\\' || a[an - 1] == '/')) an--;
    while (bn && (b[bn - 1] == '\\' || b[bn - 1] == '/')) bn--;
    if (an != bn) return 0;
    while (an--) {
        wchar_t x = lower(*a++), y = lower(*b++);
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x != y) return 0;
    }
    return 1;
}

/* the tool of Microsoft C: the first TOOL.exe on the PATH outside this directory and those
   of the wrappers that ran this one (PINTSDK_SKIP), so that two copies of the SDK on the
   PATH cannot run each other */
static int find(void)
{
    const wchar_t *p = Path, *e, *s, *t, *q;
    wchar_t *r;
    int k = len(Skip), n, i, skip;
    DWORD a;

    if (k) Skip[k++] = ';';
    for (q = Self; q < name(Self); ) Skip[k++] = *q++;
    Skip[k] = 0;
    for (; *p; p = *e ? e + 1 : e) {
        for (e = p; *e && *e != ';'; e++) ;
        q = p, n = (int)(e - p);
        if (n >= 2 && q[0] == '"' && q[n - 1] == '"') q++, n -= 2;
        skip = n == 0 || n > 900;
        for (s = Skip; !skip && *s; s = *t ? t + 1 : t) {
            for (t = s; *t && *t != ';'; t++) ;
            skip = t > s && samedir(q, n, s, (int)(t - s));
        }
        if (skip) continue;
        for (r = Real, i = 0; i < n; i++) *r++ = q[i];
        if (r[-1] != '\\' && r[-1] != '/') *r++ = '\\';
        add(add(r, Tool), L".exe");
        a = GetFileAttributesW(Real);
        if (a != 0xFFFFFFFF && !(a & 0x10)) return 1;
    }
    return 0;
}

/* the arguments of a command line, with those of its response files (@FILE) */
static void tokens(const wchar_t *line, int top)
{
    int n, i;
    wchar_t **v = CommandLineToArgvW(line, &n), *w, *s;
    for (i = 1; v && i < n && TokenCount < MAX_TOKENS; i++) {
        if (v[i][0] != '@' || !top) {
            Tokens[TokenCount++] = v[i];
            continue;
        }
        HANDLE h = CreateFileW(v[i] + 1, 0x80000000, 1, 0, 3, 0, 0);
        DWORD size, got = 0, hi, j;
        unsigned char *b;
        if (h == BAD_HANDLE) continue;
        size = GetFileSize(h, &hi);
        b = alloc(size + 2);
        if (hi || !ReadFile(h, b, size, &got, 0)) got = 0;
        CloseHandle(h);
        w = alloc((got + 4) * sizeof(wchar_t));
        add(w, L"@ ");
        if (got >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
            for (j = 2; j + 1 < got; j += 2) w[j / 2 + 1] = b[j] | b[j + 1] << 8;
            w[got / 2 + 1] = 0;
        } else
            w[2 + MultiByteToWideChar(0, 0, (char *)b, got, w + 2, got + 1)] = 0;
        for (s = w; *s; s++)
            if (*s == '\r' || *s == '\n' || *s == '\t') *s = ' ';
        tokens(w, 0);
    }
}

/* the version of a -subsystem:NAME,X.Y */
static void version(const wchar_t *s)
{
    unsigned a = 0, b = 0, d = 0;
    while (*s && *s != ',') s++;
    if (!*s++) return;
    while (*s >= '0' && *s <= '9') a = a * 10 + *s++ - '0';
    if (*s == '.')
        for (s++; *s >= '0' && *s <= '9' && d < 2; d++) b = b * 10 + *s++ - '0';
    Major = a, Minor = d == 1 ? b * 10 : b;
}

/* a number, decimal or 0x hexadecimal (0 when it is none) */
static DWORD number(const wchar_t *s)
{
    DWORD v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        for (s += 2; ; s++) {
            wchar_t c = lower(*s);
            if (c >= '0' && c <= '9') v = v * 16 + c - '0';
            else if (c >= 'a' && c <= 'f') v = v * 16 + c - 'a' + 10;
            else break;
        }
    else
        while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0';
    return v;
}

/* take each -align:N off a command line */
static void unalign(wchar_t *s)
{
    int i = 0, j;
    wchar_t *d, *e;
    while (s[i]) {
        if (option(s + i) && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t') && prefix(s + i + 1, L"align:", 1)) {
            for (j = i; s[j] && s[j] != ' ' && s[j] != '\t'; j++) ;
            for (d = s + i, e = s + j; (*d++ = *e++); ) ;
            continue;
        }
        i++;
    }
}

/* take the version off each -subsystem:NAME,X.Y of a command line */
static void unversion(wchar_t *s)
{
    for (; *s; s++) {
        const wchar_t *n;
        wchar_t *d, *e;
        if (!option(s) || !(n = prefix(s + 1, L"subsystem:", 1))) continue;
        while ((*n >= 'A' && *n <= 'Z') || (*n >= 'a' && *n <= 'z') || *n == '_') n++;
        if (*n != ',') continue;
        for (d = e = (wchar_t *)n; *e && *e != ' ' && *e != '\t' && *e != '"'; e++) ;
        while ((*d++ = *e++)) ;
    }
}

/* Out: the image the command links (none when it compiles only, or makes a library), Dll
   when it is one. cl names it after its -Fe or the first source or object, link after the
   first object; -out: of the link names it in both. */
static void output(void)
{
    const wchar_t *first = 0, *fe = 0, *out = 0, *t, *v, *x;
    const wchar_t *ext;
    int i, links = 1, inlink = !Cl;
    wchar_t *o;

    if (!Cl && TokenCount && option(Tokens[0]) &&
        (ieq(Tokens[0] + 1, L"lib") || ieq(Tokens[0] + 1, L"dump") || ieq(Tokens[0] + 1, L"edit")))
        return;
    for (i = 0; i < TokenCount; i++) {
        t = Tokens[i];
        if (!option(t)) {
            x = extension(t);
            x += *x == '.';
            if (!first && (ieq(x, L"obj") || ieq(x, L"o") || (!inlink && (ieq(x, L"c") || ieq(x, L"cpp") ||
                ieq(x, L"cxx") || ieq(x, L"cc") || ieq(x, L"cp") || ieq(x, L"c++")))))
                first = t;
            continue;
        }
        t++;
        if (inlink) {
            if ((v = prefix(t, L"out:", 1))) out = v;
            else if (ieq(t, L"dll")) Dll = 1;
            else if (prefix(t, L"subsystem:", 1)) version(t);
            else if (ieq(t, L"driver") || prefix(t, L"driver:", 1)) Driver = 1;
            else if ((v = prefix(t, L"align:", 1))) Align = number(v);
            continue;
        }
        if (ieq(t, L"link")) {
            inlink = HasLink = 1;
            continue;
        }
        if (eq(t, L"c") || eq(t, L"E") || eq(t, L"EP") || eq(t, L"P") || eq(t, L"Zs") || eq(t, L"Zg") ||
            eq(t, L"?") || ieq(t, L"help"))
            links = 0;
        else if (eq(t, L"LD") || eq(t, L"LDd"))
            Dll = 1;
        else if ((v = prefix(t, L"Fe", 0))) {
            if (*v == ':') v++;
            if (!*v && i + 1 < TokenCount) v = Tokens[++i];
            fe = v;
        } else if ((v = prefix(t, L"Tc", 0)) || (v = prefix(t, L"Tp", 0))) {
            if (!*v && i + 1 < TokenCount) v = Tokens[++i];
            if (!first) first = v;
        } else if (eq(t, L"I") || eq(t, L"D") || eq(t, L"U") || eq(t, L"AI") || eq(t, L"FI") || eq(t, L"FU"))
            i++;
    }
    if (!links || !(out || fe || first)) return;
    ext = Dll ? L".dll" : L".exe";
    if (out) {
        add(Out, out);
        return;
    }
    if (fe && *fe && fe[len(fe) - 1] != '\\' && fe[len(fe) - 1] != '/') {
        add(Out, fe);
        if (!*extension(fe)) add(Out + len(Out), ext);
        return;
    }
    if (!first) return;
    o = fe ? add(Out, fe) : Out;
    for (v = name(first), x = extension(first); v < x; ) *o++ = *v++;
    add(o, ext);
}

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static DWORD rd32(const unsigned char *p) { return rd16(p) | (DWORD)rd16(p + 2) << 16; }
static void wr16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v, p[1] = (unsigned char)(v >> 8); }
static void wr32(unsigned char *p, DWORD v) { wr16(p, v & 0xFFFF), wr16(p + 2, v >> 16); }
static DWORD up(DWORD v, DWORD a) { return (v + a - 1) & ~(a - 1); }
static void copy(unsigned char *d, const unsigned char *s, DWORD n)
{
    while (n--) *(volatile unsigned char *)d++ = *s++;
}

/* lay an ARM image out on AL, as the linker's -align below a page does (tools/pe2img -align
   of PINT): the section and file alignment become AL, and each section lies in the file at
   its address with its uninitialized data, so that the file is the image as mapped, one
   subsection to the memory manager (MM\CREASECT.C). Debug data that is not mapped follows
   the sections. mark() makes the checksum again. */
static int lay(const wchar_t *f, DWORD al)
{
    HANDLE h = CreateFileW(f, 0x80000000, 1, 0, 3, 0, 0);
    DWORD n, hi, got, pe, nsec, hdrs, end, size, i, dbg, ndbg, add = 0, va, vs, rs;
    unsigned char *b, *o, *sec, *img, *s, *d;

    if (h == BAD_HANDLE) return 0;
    n = GetFileSize(h, &hi);
    b = alloc(n + 1);
    got = 0;
    if (hi || n < 0x200 || !ReadFile(h, b, n, &got, 0)) got = 0;
    CloseHandle(h);
    if (got != n || n < 0x200) return 0;
    pe = rd32(b + 0x3C);
    if (b[0] != 'M' || b[1] != 'Z' || pe > n - 0x100 || rd32(b + pe) != 0x4550) return 0;
    o = b + pe + 24;
    nsec = rd16(b + pe + 6);
    sec = o + rd16(b + pe + 20);
    if (rd16(o) != 0x10B || (al & (al - 1)) || al > rd32(o + 32) || rd32(o + 96 + 4 * 8 + 4)) return 0;
    if (sec + nsec * 40 > b + n) return 0;
    hdrs = up(rd32(o + 60), al);
    end = hdrs;
    for (i = 0; i < nsec; i++) {
        s = sec + i * 40;
        va = rd32(s + 12), vs = rd32(s + 8), rs = rd32(s + 16);
        if ((va & (al - 1)) || va < end || rd32(s + 20) + rs > n) return 0;
        end = va + up(vs > rs ? vs : rs, al);
    }
    if (end > rd32(o + 56)) return 0;
    dbg = rd32(o + 96 + 6 * 8);
    ndbg = rd32(o + 96 + 6 * 8 + 4) / 28;
    for (i = 0; dbg && i < ndbg; i++) {
        d = 0;
        for (s = sec; s < sec + nsec * 40; s += 40)
            if (dbg >= rd32(s + 12) && dbg < rd32(s + 12) + rd32(s + 16))
                d = b + rd32(s + 20) + dbg - rd32(s + 12) + i * 28;
        if (!d || d + 28 > b + n) return 0;
        if (!rd32(d + 20) && rd32(d + 24)) {
            if (rd32(d + 24) + rd32(d + 16) > n) return 0;
            add += up(rd32(d + 16), al);
        }
    }
    img = alloc(end + add);
    copy(img, b, rd32(o + 60));
    for (i = 0; i < nsec; i++) {
        s = sec + i * 40;
        if (rd32(s + 20)) copy(img + rd32(s + 12), b + rd32(s + 20), rd32(s + 16));
    }
    /* the headers of the new layout, in the copy */
    o = img + pe + 24;
    s = o + rd16(img + pe + 20);
    wr32(o + 32, al);
    wr32(o + 36, al);
    wr32(o + 60, hdrs);
    for (i = 0; i < nsec; i++, s += 40) {
        vs = rd32(s + 8), rs = rd32(s + 16);
        wr32(s + 16, up(vs > rs ? vs : rs, al));
        wr32(s + 20, rd32(s + 12));
    }
    size = end;
    for (i = 0; dbg && i < ndbg; i++) {
        d = img + dbg + i * 28;
        if (rd32(d + 20))
            wr32(d + 24, rd32(d + 20));
        else if (rd32(d + 24)) {
            copy(img + size, b + rd32(d + 24), rd32(d + 16));
            wr32(d + 24, size);
            size += up(rd32(d + 16), al);
        }
    }
    h = CreateFileW(f, 0x40000000, 0, 0, 2, 0, 0);
    if (h == BAD_HANDLE) return 0;
    got = 0;
    WriteFile(h, img, size, &got, 0);
    CloseHandle(h);
    return got == size;
}

/* mark an ARM image for Windows NT 4.0: OS version 4.00, the subsystem version asked for
   (4.00 when none), and its checksum (when it has one: -release) made again */
static int mark(const wchar_t *f)
{
    HANDLE h = CreateFileW(f, 0xC0000000, 1, 0, 3, 0, 0);
    DWORD n, hi, got, pe, sum = 0, i, at;
    unsigned char *b, *o;
    int ok = 0;

    if (h == BAD_HANDLE) return 0;
    n = GetFileSize(h, &hi);
    b = alloc(n + 1);
    if (hi || n < 0x200 || !ReadFile(h, b, n, &got, 0) || got != n) goto done;
    pe = rd32(b + 0x3C);
    if (b[0] != 'M' || b[1] != 'Z' || pe > n - 0x100 || rd32(b + pe) != 0x4550) goto done;
    o = b + pe + 24;
    ok = 1;
    if (rd16(b + pe + 4) != 0x1C4 || rd16(o) != 0x10B) goto done;
    wr16(o + 40, 4), wr16(o + 42, 0);
    wr16(o + 48, Major), wr16(o + 50, Minor);
    if (rd32(o + 64)) {
        at = (DWORD)(o + 64 - b);
        for (i = 0; i + 1 < n; i += 2)
            if (i != at && i != at + 2) sum += rd16(b + i), sum = (sum & 0xFFFF) + (sum >> 16);
        if (n & 1) sum += b[n - 1], sum = (sum & 0xFFFF) + (sum >> 16);
        sum = (sum & 0xFFFF) + (sum >> 16) + n;
        for (i = 0; i < 4; i++) b[at + i] = (unsigned char)(sum >> 8 * i);
    }
    ok = !SetFilePointer(h, 0, 0, 0) && WriteFile(h, b, (DWORD)(o + 68 - b), &got, 0);
done:
    CloseHandle(h);
    return ok;
}

/* Ctrl+C is the tool's: this waits for it to end (a handler, as a process that ignores
   Ctrl+C gives that to the processes it starts) */
static BOOL WINAPI ctrl(DWORD type) { return type == 0 || type == 1; }

void start(void)
{
    wchar_t *line = GetCommandLineW(), *args = line, *cmd;
    const wchar_t *n, *e;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1, i, al;

    SetConsoleCtrlHandler(ctrl, 1);
    GetModuleFileNameW(0, Self, 1000);
    for (n = name(Self), e = extension(Self), i = 0; n < e && i < 60; ) Tool[i++] = *n++;
    Cl = ieq(Tool, L"cl");
    GetEnvironmentVariableW(L"PATH", Path, 32760);
    GetEnvironmentVariableW(L"PINTSDK_SKIP", Skip, 4000);
    if (!find()) {
        say(L"Microsoft C for ARM is not on the PATH (setenv.cmd)", L"");
        ExitProcess(1);
    }
    SetEnvironmentVariableW(L"PINTSDK_SKIP", Skip);

    if (*args == '"') {
        for (args++; *args && *args != '"'; args++) ;
        if (*args) args++;
    } else
        while (*args && *args != ' ' && *args != '\t') args++;
    while (*args == ' ' || *args == '\t') args++;
    tokens(line, 1);
    output();

    cmd = alloc((len(Real) + len(args) + sizeof ClOptions + sizeof ClLibraries + 32) * sizeof(wchar_t));
    add(add(add(add(add(cmd, L"\""), Real), L"\" "), Cl ? ClOptions : L""), Cl ? L" " : L"");
    add(cmd + len(cmd), args);
    unversion(cmd);
    al = Align ? (Align < 0x1000 ? Align : 0) : Driver ? 0x20 : 0;
    if (Align && Align < 0x1000) unalign(cmd);
    if (Cl && Out[0] && !Driver) add(add(cmd + len(cmd), HasLink ? L" " : L" /link "), ClLibraries);

    for (i = 0; i < sizeof si; i++) ((volatile unsigned char *)&si)[i] = 0;
    si.cb = sizeof si;
    if (!CreateProcessW(Real, cmd, 0, 0, 1, 0, 0, 0, &si, &pi)) {
        say(L"cannot run ", Real);
        ExitProcess(1);
    }
    WaitForSingleObject(pi.hProcess, 0xFFFFFFFF);
    GetExitCodeProcess(pi.hProcess, &code);
    if (code == 0 && Out[0] && al && !lay(Out, al)) {
        say(L"cannot lay the image out on its alignment: ", Out);
        code = 1;
    }
    if (code == 0 && Out[0] && !mark(Out)) {
        say(L"cannot mark for Windows NT 4.0: ", Out);
        code = 1;
    }
    ExitProcess(code);
}
