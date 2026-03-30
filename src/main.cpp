// octaneServGrpc main.cpp
// Windows tray application that embeds the Octane Render SDK and serves gRPC.
// Lifted from octaneservermodule/src/appserver_octane/main.cpp, adapted for octaneServGrpc.

#ifdef _WIN32
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#   include <shellapi.h>
#   include <tchar.h>
#else
#   include <unistd.h>
#   include <signal.h>
#   include <iostream>
#endif

#include <thread>
#include <string>
#include <atomic>
#include <memory>
#include <iostream>

#include "resource.h"
#include "sdk_engine.h"
#include "grpc_server.h"
#include "util/server_log.h"

// Default gRPC server port (same as Octane's built-in gRPC server)
static constexpr uint16_t DEFAULT_PORT = 51022;

//--------------------------------------------------------------------------------
// AppThread — runs the gRPC server in a worker thread
// Lifted from octaneservermodule/src/appserver_octane/main.cpp
//--------------------------------------------------------------------------------
class AppThread {
public:
    AppThread(OctaneServ::GrpcServer* server) : mServer(server) {
        mThread = std::make_unique<std::thread>(&start, this);
    }

    void stop() {
        mServer->StopServer();
        if (mThread && mThread->joinable())
            mThread->join();
    }

private:
    std::unique_ptr<std::thread> mThread;
    OctaneServ::GrpcServer* mServer = nullptr;

    static unsigned int start(void* param) {
        AppThread* thread = (AppThread*)param;
        thread->mServer->RunServer();
        return 0;
    }
};

static AppThread* gAppThread = nullptr;
static OctaneServ::GrpcServer* gServer = nullptr;

#if defined(_WIN32) && defined(SERV_WIN32_APP)
//--------------------------------------------------------------------------------
// Windows tray app (lifted from appserver_octane/main.cpp)
// Only compiled when SERV_WIN32_APP is defined (release tray app builds)
//--------------------------------------------------------------------------------

#define MAX_LOADSTRING 100
#define IDR_LOG_WINDOW     1003
#define IDR_PREFERENCES    1004
#define IDR_DEVICE_SETTINGS 1005
#define IDR_MAIN_WINDOW    1007
#define IDR_AUTH            1008
#define IDR_SERV_LOG       1009
#define IDR_EXIT           1002

// Global Variables
static HWND            hWnd;
static HINSTANCE       hInst;
static TCHAR           szTitle[MAX_LOADSTRING];
static TCHAR           szWindowClass[MAX_LOADSTRING];
static NOTIFYICONDATA  TaskStruct;

// Forward declarations
ATOM                RegisterServerWindowClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);
BOOL                ShowPopupMenu(HWND hWnd, POINT* curpos, int wDefaultItem);
void                UpdateTaskBarInfo(bool init = false);

//--------------------------------------------------------------------------------
int APIENTRY _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);

    // Single-instance guard — only one octaneServGrpc process allowed
    HANDLE hMutex = CreateMutexW(NULL, FALSE, L"Global\\OctaneServGrpc_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        OutputDebugStringA("[OctaneServGrpc] Another instance is already running. Exiting.\n");
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    // Parse command line for port and log level
    uint16_t port = DEFAULT_PORT;
    OctaneServ::LogLevel logLevel = OctaneServ::LogLevel::Off;
    std::string cmdline = lpCmdLine;
    if (!cmdline.empty()) {
        try { port = static_cast<uint16_t>(std::stoi(cmdline)); } catch (...) {}
    }
    const char* envLevel = std::getenv("SERV_LOG_LEVEL");
    if (envLevel) logLevel = OctaneServ::ServerLog::parseLevel(envLevel);

    // Initialize file logging (parity with console entry point)
    OctaneServ::ServerLog::instance().init("octaneServGrpc", logLevel);

    // Initialize global strings
    _tcscpy_s(szTitle, MAX_LOADSTRING, _T("OctaneServGrpc"));
    _tcscpy_s(szWindowClass, MAX_LOADSTRING, _T("OCTANESERVGRPC"));

    // Register window class and create hidden window
    RegisterServerWindowClass(hInstance);
    if (!InitInstance(hInstance, nCmdShow))
        return 1;

    // Set up tray icon
    HICON hIconSmall = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    if (!hIconSmall) {
        ::MessageBox(0, _T("Failed to load tray icon resource"), _T("OctaneServGrpc"), MB_OK | MB_ICONWARNING);
    }
    TaskStruct.cbSize = sizeof(NOTIFYICONDATA);
    TaskStruct.hWnd = hWnd;
    TaskStruct.uID = 1;
    TaskStruct.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    TaskStruct.uCallbackMessage = WM_APP + 100;
    TaskStruct.hIcon = hIconSmall;

    // Initialize Octane SDK
    if (!OctaneServ::SdkEngine::Init("PLORTEST", true)) {
        ::MessageBox(0, _T("Failed to initialize Octane SDK"), _T("OctaneServGrpc"), MB_OK | MB_ICONERROR);
        return 1;
    }
    OctaneServ::SdkEngine::RegisterCallbacks();

    // Create and start the gRPC server
    gServer = new OctaneServ::GrpcServer(port);
    UpdateTaskBarInfo(true);
    gAppThread = new AppThread(gServer);

    // Message loop
    MSG msg;
    while (::GetMessage(&msg, hWnd, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessage(&msg);
    }

    // Shutdown — unregister SDK callbacks first to prevent firing into torn-down state
    OctaneServ::SdkEngine::UnregisterCallbacks();

    gAppThread->stop();
    delete gServer;
    gServer = nullptr;

    OctaneServ::SdkEngine::Exit();

    ::Shell_NotifyIcon(NIM_DELETE, &TaskStruct);
    if (hMutex) CloseHandle(hMutex);
    return 0;
}

//--------------------------------------------------------------------------------
ATOM RegisterServerWindowClass(HINSTANCE hInstance) {
    WNDCLASSEX wcex;
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    wcex.hCursor        = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground  = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName   = NULL;
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_ICON1));
    return RegisterClassEx(&wcex);
}

//--------------------------------------------------------------------------------
BOOL InitInstance(HINSTANCE hInstance, int /*nCmdShow*/) {
    hInst = hInstance;
    hWnd = CreateWindow(szWindowClass, szTitle, WS_OVERLAPPEDWINDOW | WS_DISABLED,
                        CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, NULL, NULL, hInstance, NULL);
    if (!hWnd) return FALSE;
    ShowWindow(hWnd, SW_HIDE);
    UpdateWindow(hWnd);
    return TRUE;
}

//--------------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;
        case IDM_EXIT:
            DestroyWindow(hWnd);
            break;
        case IDR_LOG_WINDOW:
            OctaneServ::SdkEngine::OpenLogWindow();
            break;
        case IDR_MAIN_WINDOW:
            OctaneServ::SdkEngine::OpenMainWindow();
            break;
        case IDR_DEVICE_SETTINGS:
            OctaneServ::SdkEngine::OpenDeviceSettings();
            break;
        case IDR_PREFERENCES:
            OctaneServ::SdkEngine::OpenPreferences();
            break;
        case IDR_AUTH:
            OctaneServ::SdkEngine::OpenAuthWindow();
            break;
        case IDR_SERV_LOG: {
            auto& path = OctaneServ::ServerLog::instance().logPath();
            if (!path.empty())
                ShellExecuteA(NULL, "open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        case IDR_EXIT:
            ::PostMessage(hWnd, WM_QUIT, 0, 0);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_APP + 100:
        if (lParam == WM_RBUTTONUP) {
            SetForegroundWindow(hWnd);
            ShowPopupMenu(hWnd, NULL, -1);
            PostMessage(hWnd, WM_APP + 1, 0, 0);
            return 0;
        } else {
            UpdateTaskBarInfo();
        }
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

//--------------------------------------------------------------------------------
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    UNREFERENCED_PARAMETER(lParam);
    switch (message) {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}

//--------------------------------------------------------------------------------
void UpdateTaskBarInfo(bool init) {
    std::string tip = "OctaneServGrpc";

    if (OctaneServ::SdkEngine::IsReady()) {
        tip += " v." + std::to_string(OctaneServ::SdkEngine::GetVersion());
        if (OctaneServ::SdkEngine::IsActivated()) {
            tip += " (Activated)";
        } else {
            tip += " (Deactivated)";
        }
    }

    if (gServer) {
        tip += " :" + std::to_string(gServer->GetPort());
    }

    strncpy_s(TaskStruct.szTip, tip.c_str(), sizeof(TaskStruct.szTip) - 1);
    if (!::Shell_NotifyIcon(init ? NIM_ADD : NIM_MODIFY, &TaskStruct)) {
        OctaneServ::ServerLog::instance().err("TrayIcon", "Shell_NotifyIcon",
            init ? "NIM_ADD failed" : "NIM_MODIFY failed");
    }
}

//--------------------------------------------------------------------------------
BOOL ShowPopupMenu(HWND hWnd, POINT* curpos, int wDefaultItem) {
    HMENU hPop = CreatePopupMenu();
    InsertMenu(hPop, 0, MF_BYPOSITION | MF_STRING, IDR_LOG_WINDOW,     "Log Window");
    InsertMenu(hPop, 1, MF_BYPOSITION | MF_STRING, IDR_SERV_LOG,      "Server Log");
    InsertMenu(hPop, 2, MF_BYPOSITION | MF_STRING, IDR_MAIN_WINDOW,   "Octane Standalone");
    InsertMenu(hPop, 3, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenu(hPop, 4, MF_BYPOSITION | MF_STRING, IDR_DEVICE_SETTINGS, "Device Settings");
    InsertMenu(hPop, 5, MF_BYPOSITION | MF_STRING, IDR_PREFERENCES,   "Preferences");
    InsertMenu(hPop, 6, MF_BYPOSITION | MF_STRING, IDR_AUTH,          "Activation");
    InsertMenu(hPop, 7, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenu(hPop, 8, MF_BYPOSITION | MF_STRING, IDR_EXIT,          "Exit");
    SetFocus(hWnd);
    SendMessage(hWnd, WM_INITMENUPOPUP, (WPARAM)hPop, 0);
    POINT pt;
    if (!curpos) {
        GetCursorPos(&pt);
        curpos = &pt;
    }
    WORD cmd = TrackPopupMenu(hPop, TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                              curpos->x, curpos->y, 0, hWnd, NULL);
    SendMessage(hWnd, WM_COMMAND, cmd, 0);
    DestroyMenu(hPop);
    return 0;
}

#elif !defined(_WIN32)
//--------------------------------------------------------------------------------
// Console mode (Linux/macOS)
//--------------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    uint16_t port = DEFAULT_PORT;
    if (argc > 1) {
        try { port = static_cast<uint16_t>(std::stoi(argv[1])); } catch (...) {}
    }

    std::cout << "[OctaneServGrpc] OctaneServGrpc starting..." << std::endl;

    if (!OctaneServ::SdkEngine::Init("PLORTEST", true)) {
        std::cerr << "[OctaneServGrpc] Failed to initialize Octane SDK" << std::endl;
        return 1;
    }
    OctaneServ::SdkEngine::RegisterCallbacks();

    gServer = new OctaneServ::GrpcServer(port);
    gAppThread = new AppThread(gServer);

    std::cout << "[OctaneServGrpc] Press 'e' + Enter to exit, 'a' for activation" << std::endl;
    while (true) {
        char c;
        std::cin >> c;
        if (c == 'a') {
            OctaneServ::SdkEngine::OpenAuthWindow();
        } else if (c == 'e') {
            break;
        }
    }

    OctaneServ::SdkEngine::UnregisterCallbacks();
    gAppThread->stop();
    delete gServer;
    OctaneServ::SdkEngine::Exit();
    return 0;
}
#else
//--------------------------------------------------------------------------------
// Windows console entry point (default for debug builds)
// Provides visible stdout/stderr for debugging.
//--------------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // Single-instance guard
    HANDLE hMutex = CreateMutexW(NULL, FALSE, L"Global\\OctaneServGrpc_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "[OctaneServGrpc] ERROR: Another instance is already running." << std::endl;
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    uint16_t port = DEFAULT_PORT;
    if (argc > 1) {
        try { port = static_cast<uint16_t>(std::stoi(argv[1])); } catch (...) {}
    }

    std::cout << "[OctaneServGrpc] OctaneServGrpc starting (console mode)..." << std::endl;
    std::cout << "[OctaneServGrpc] Port: " << port << std::endl;

    // Init file logging — parse --log-level arg or SERV_LOG_LEVEL env
    {
        OctaneServ::LogLevel logLevel = OctaneServ::LogLevel::Off;
        const char* envLevel = std::getenv("SERV_LOG_LEVEL");
        if (envLevel) logLevel = OctaneServ::ServerLog::parseLevel(envLevel);
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg.substr(0, 12) == "--log-level=") {
                logLevel = OctaneServ::ServerLog::parseLevel(arg.substr(12));
            }
        }
        OctaneServ::ServerLog::instance().init(argv[0], logLevel);
    }

    if (!OctaneServ::SdkEngine::Init("PLORTEST", true)) {
        std::cerr << "[OctaneServGrpc] FAILED to initialize Octane SDK." << std::endl;
        std::cerr << "[OctaneServGrpc] Press Enter to exit..." << std::endl;
        std::cin.get();
        return 1;
    }

    std::cout << "[OctaneServGrpc] SDK initialized. Registering callbacks..." << std::endl;
    OctaneServ::SdkEngine::RegisterCallbacks();

    std::cout << "[OctaneServGrpc] Starting gRPC server..." << std::endl;
    gServer = new OctaneServ::GrpcServer(port);
    gAppThread = new AppThread(gServer);

    std::cout << "[OctaneServGrpc] Running. Press 'e' + Enter to exit, 'a' for activation." << std::endl;
    while (true) {
        char c;
        std::cin >> c;
        if (c == 'a') {
            OctaneServ::SdkEngine::OpenAuthWindow();
        } else if (c == 'e') {
            break;
        }
    }

    std::cout << "[OctaneServGrpc] Shutting down..." << std::endl;
    OctaneServ::SdkEngine::UnregisterCallbacks();
    gAppThread->stop();
    delete gServer;
    gServer = nullptr;
    OctaneServ::SdkEngine::Exit();
    if (hMutex) CloseHandle(hMutex);
    std::cout << "[OctaneServGrpc] Done." << std::endl;
    return 0;
}
#endif // SERV_WIN32_APP / !_WIN32 / else
