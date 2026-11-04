/* winhello: a Windows program. A window with a menu (File: About, Exit) and a dialog box. */
#include <windows.h>
#include "winhello.h"

static HINSTANCE Instance;

static BOOL CALLBACK AboutProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_INITDIALOG:
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL) {
            EndDialog(dialog, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    PAINTSTRUCT paint;
    RECT client;
    HDC dc;

    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDM_ABOUT:
            DialogBox(Instance, MAKEINTRESOURCE(IDD_ABOUT), window, AboutProc);
            return 0;
        case IDM_EXIT:
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_PAINT:
        dc = BeginPaint(window, &paint);
        GetClientRect(window, &client);
        DrawText(dc, TEXT("Hello, Windows NT 4.0 on ARM!"), -1, &client,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(window, &paint);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(window, message, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    WNDCLASS wndclass;
    HWND window;
    MSG msg;

    Instance = instance;
    wndclass.style = CS_HREDRAW | CS_VREDRAW;
    wndclass.lpfnWndProc = WindowProc;
    wndclass.cbClsExtra = 0;
    wndclass.cbWndExtra = 0;
    wndclass.hInstance = instance;
    wndclass.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wndclass.hCursor = LoadCursor(NULL, IDC_ARROW);
    wndclass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wndclass.lpszMenuName = MAKEINTRESOURCE(IDM_WINHELLO);
    wndclass.lpszClassName = TEXT("WinHello");
    if (!RegisterClass(&wndclass))
        return 1;
    window = CreateWindow(TEXT("WinHello"), TEXT("Hello"), WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, 400, 200, NULL, NULL, instance, NULL);
    if (!window)
        return 1;
    ShowWindow(window, show);
    UpdateWindow(window);
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
