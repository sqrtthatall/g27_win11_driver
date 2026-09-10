#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>
#include <iomanip>
#include <sstream>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")

const char* SUPPORT_URL = "https://github.com/sqrtthatall/g27_win11_driver";

constexpr USHORT LOGITECH_VID = 0x046D;
constexpr USHORT G27_NATIVE_PID = 0xC29B;
constexpr USHORT G27_COMPAT_PID1 = 0xC294;
constexpr USHORT G27_COMPAT_PID2 = 0xC299;


#define WM_TRAYICON       (WM_USER + 101)
#define IDM_TRAY_RESTORE  2001
#define IDM_TRAY_EXIT     2002

NOTIFYICONDATAW g_nid = { sizeof(NOTIFYICONDATAW) };
bool g_isTrayActive = false;

// =========================================================================
// Wheel set
// =========================================================================
struct WheelSettings {
    bool combinedPedals = false;
    int rotationDeg = 900;

    bool enableFFB = true;
    int overallStrength = 90;
    int springStrength = 60;
    int damperStrength = 25;

    bool enableCenteringSpring = false;
    int centeringSpringStrength = 0;

    bool allowGameToAdjust = true;
};

WheelSettings g_Cfg;

// =========================================================================
// G27 Control Class
// =========================================================================
class LogitechG27 {
private:
    HANDLE hDevice = INVALID_HANDLE_VALUE;
    std::atomic<bool> isRunning{ false };
    std::thread workerThread;

public:
    uint8_t buffer[32] = { 0 };
    std::atomic<bool> isConnected{ false };

    ~LogitechG27() {
        EmergencyStopMotors();
        Stop();
        CloseDevice();
    }

    void CloseDevice() {
        if (hDevice != INVALID_HANDLE_VALUE) {
            CloseHandle(hDevice);
            hDevice = INVALID_HANDLE_VALUE;
        }
        isConnected = false;
    }

    bool Open() {
        if (isConnected) return true;

        GUID hidGuid;
        HidD_GetHidGuid(&hidGuid);
        HDEVINFO devInfo = SetupDiGetClassDevs(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (devInfo == INVALID_HANDLE_VALUE) return false;

        SP_DEVICE_INTERFACE_DATA ifData{ sizeof(SP_DEVICE_INTERFACE_DATA) };

        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifData); ++i) {
            DWORD size = 0;
            SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, nullptr, 0, &size, nullptr);
            auto detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(size);
            if (!detail) continue;
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

            if (SetupDiGetDeviceInterfaceDetail(devInfo, &ifData, detail, size, nullptr, nullptr)) {
                HANDLE h = CreateFile(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);

                if (h != INVALID_HANDLE_VALUE) {
                    HIDD_ATTRIBUTES attr{ sizeof(HIDD_ATTRIBUTES) };
                    if (HidD_GetAttributes(h, &attr)) {
                        if (attr.VendorID == LOGITECH_VID &&
                            (attr.ProductID == G27_NATIVE_PID ||
                                attr.ProductID == G27_COMPAT_PID1 ||
                                attr.ProductID == G27_COMPAT_PID2)) {

                            hDevice = h;
                            isConnected = true;
                            free(detail);
                            SetupDiDestroyDeviceInfoList(devInfo);

                            ApplyConfig();
                            Start();
                            return true;
                        }
                    }
                    CloseHandle(h);
                }
            }
            free(detail);
        }
        SetupDiDestroyDeviceInfoList(devInfo);
        return false;
    }

    bool SendCmd(const std::vector<uint8_t>& cmd) {
        if (hDevice == INVALID_HANDLE_VALUE) return false;

        std::vector<uint8_t> b(cmd.size() + 1, 0);
        memcpy(&b[1], cmd.data(), cmd.size());

        HANDLE hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!hEvent) return false;

        OVERLAPPED ov{ 0 };
        ov.hEvent = hEvent;
        DWORD written = 0;

        BOOL ok = WriteFile(hDevice, b.data(), static_cast<DWORD>(b.size()), &written, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            WaitForSingleObject(hEvent, 50);
        }
        CloseHandle(hEvent);
        return true;
    }

    void SetRotation(uint16_t deg) {
        deg = std::clamp<uint16_t>(deg, 40, 900);
        SendCmd({ 0xF8, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00 });
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        uint8_t low = deg & 0xFF;
        uint8_t high = (deg >> 8) & 0xFF;
        SendCmd({ 0xF8, 0x81, low, high, 0x00, 0x00, 0x00 });
    }

    void EmergencyStopMotors() {
        SendCmd({ 0xF5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
    }

    void SetAutocenterSpring(uint8_t percent) {
        if (percent > 100) percent = 100;
        if (percent == 0) {
            EmergencyStopMotors();
            return;
        }

        uint16_t magnitude = static_cast<uint16_t>((percent * 65535) / 100);
        uint32_t expand_a, expand_b;
        if (magnitude <= 0xAAAA) {
            expand_a = 0x0C * magnitude;
            expand_b = 0x80 * magnitude;
        }
        else {
            expand_a = (0x0C * 0xAAAA) + 0x06 * (magnitude - 0xAAAA);
            expand_b = (0x80 * 0xAAAA) + 0xFF * (magnitude - 0xAAAA);
        }

        uint8_t forceA = static_cast<uint8_t>(expand_a / 0xAAAA);
        uint8_t forceB = static_cast<uint8_t>(expand_b / 0xAAAA);

        SendCmd({ 0xFE, 0x0D, forceA, forceA, forceB, 0x00, 0x00 });
        SendCmd({ 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
    }

    void ApplyConfig() {
        if (!isConnected) return;
        SetRotation(static_cast<uint16_t>(g_Cfg.rotationDeg));

        if (!g_Cfg.enableFFB) {
            EmergencyStopMotors();
            return;
        }

        int effectiveSpring = 0;
        if (g_Cfg.enableCenteringSpring) {
            effectiveSpring = (g_Cfg.centeringSpringStrength * g_Cfg.overallStrength) / 100;
        }
        else {
            effectiveSpring = (g_Cfg.springStrength * g_Cfg.overallStrength) / 100;
        }
        SetAutocenterSpring(static_cast<uint8_t>(effectiveSpring));
    }

    void TriggerLedTest() {
        std::thread([this]() {
            uint8_t leds[] = { 0x01, 0x03, 0x07, 0x0F, 0x1F, 0x00 };
            for (int r = 0; r < 2; ++r) {
                for (uint8_t m : leds) {
                    SendCmd({ 0xF8, 0x12, m, 0x00, 0x00, 0x00, 0x00 });
                    std::this_thread::sleep_for(std::chrono::milliseconds(90));
                }
            }
            }).detach();
    }

    void Start() {
        if (isRunning) return;
        isRunning = true;
        workerThread = std::thread([this]() {
            uint8_t raw[64];
            HANDLE hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            if (!hEvent) return;

            OVERLAPPED ov{ 0 };
            ov.hEvent = hEvent;

            while (isRunning) {
                ResetEvent(hEvent);
                DWORD read = 0;
                BOOL ok = ReadFile(hDevice, raw, sizeof(raw), &read, &ov);
                if (!ok && GetLastError() == ERROR_IO_PENDING) {
                    if (WaitForSingleObject(hEvent, 20) == WAIT_OBJECT_0) {
                        GetOverlappedResult(hDevice, &ov, &read, FALSE);
                    }
                }
                if (read >= 12) {
                    DWORD copySize = (read < 32) ? read : 32;
                    memcpy(buffer, raw, copySize);
                }
            }
            CloseHandle(hEvent);
            });
    }

    void Stop() {
        if (isRunning) {
            isRunning = false;
            if (workerThread.joinable()) workerThread.join();
        }
    }
} g_Wheel;

// =========================================================================
// UI STructers
// =========================================================================
struct UiSlider {
    int id;
    RECT rect;
    int minVal, maxVal;
    int* valPtr;
    std::wstring label;
    std::wstring suffix;
    COLORREF color;
    bool isDragging = false;
    bool isHovered = false;
};

struct UiToggle {
    int id;
    RECT rect;
    bool* valPtr;
    std::wstring label;
    bool isHovered = false;
};

struct UiButton {
    int id;
    RECT rect;
    std::wstring label;
    bool isHovered = false;
    bool isActive = false;
    COLORREF accent;
};

std::vector<UiSlider> g_Sliders;
std::vector<UiToggle> g_Toggles;
std::vector<UiButton> g_Buttons;
int g_ActiveSliderDrag = -1;

void ResetToDefaults() {
    g_Cfg.combinedPedals = false;
    g_Cfg.rotationDeg = 900;
    g_Cfg.enableFFB = true;
    g_Cfg.overallStrength = 90;
    g_Cfg.springStrength = 60;
    g_Cfg.damperStrength = 25;
    g_Cfg.enableCenteringSpring = false;
    g_Cfg.centeringSpringStrength = 0;
    g_Cfg.allowGameToAdjust = true;
    g_Wheel.ApplyConfig();
}


void SetupUiLayout() {
    g_Sliders.clear();
    g_Toggles.clear();
    g_Buttons.clear();

    // 1. Pedals Mode 
    g_Toggles.push_back({ 101, { 45, 85, 455, 115 }, &g_Cfg.combinedPedals, L"Combined (single axis - used for most games)" });

    // 2. Steering Rotation (Card: y = 140..275)
    g_Sliders.push_back({ 201, { 45, 195, 455, 215 }, 40, 900, &g_Cfg.rotationDeg, L"Degrees of Rotation", L"°", RGB(0, 195, 255) });
    g_Buttons.push_back({ 301, { 45,  230, 175, 260 }, L"900° SIM",    false, false, RGB(0, 195, 255) });
    g_Buttons.push_back({ 302, { 185, 230, 315, 260 }, L"540° DRIFT",  false, false, RGB(0, 195, 255) });
    g_Buttons.push_back({ 303, { 325, 230, 455, 260 }, L"360° ARCADE", false, false, RGB(0, 195, 255) });

    // 3. Force Feedback
    g_Toggles.push_back({ 102, { 45, 320, 455, 350 }, &g_Cfg.enableFFB, L"Enable Force Feedback" });
    g_Sliders.push_back({ 202, { 45, 375, 455, 395 }, 0, 150, &g_Cfg.overallStrength, L"Overall Effects Strength", L"%", RGB(255, 170, 0) });
    g_Sliders.push_back({ 203, { 45, 425, 455, 445 }, 0, 150, &g_Cfg.springStrength,  L"Spring Effect Strength",  L"%", RGB(255, 170, 0) });
    g_Sliders.push_back({ 204, { 45, 475, 455, 495 }, 0, 150, &g_Cfg.damperStrength,  L"Damper Effect Strength",  L"%", RGB(255, 170, 0) });

    // 4.Centering Spring
    g_Toggles.push_back({ 103, { 45, 545, 455, 575 }, &g_Cfg.enableCenteringSpring, L"Enable Centering Spring in Force Feedback Games" });
    g_Sliders.push_back({ 205, { 45, 600, 455, 620 }, 0, 100, &g_Cfg.centeringSpringStrength, L"Centering Spring Strength", L"%", RGB(255, 80, 100) });

    // 5. Game Override
    g_Toggles.push_back({ 104, { 45, 668, 455, 698 }, &g_Cfg.allowGameToAdjust, L"Allow Game To Adjust Settings" });


    g_Buttons.push_back({ 304, { 35,  725, 170, 760 }, L"Defaults", false, false, RGB(180, 190, 210) });
    g_Buttons.push_back({ 305, { 180, 725, 320, 760 }, L"Minimize to Tray", false, false, RGB(0, 195, 255) });
    g_Buttons.push_back({ 308, { 330, 725, 470, 760 }, L"Close", false, false, RGB(180, 190, 210) });


    g_Buttons.push_back({ 306, { 510, 665, 660, 700 }, L"TEST RPM LEDS", false, false, RGB(0, 230, 118) });
    g_Buttons.push_back({ 307, { 675, 665, 835, 700 }, L"★ GITHUB / INFO", false, false, RGB(255, 185, 0) });
}

// =========================================================================
// tray func
// =========================================================================
void InitTrayIcon(HWND hWnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"Logitech G27");
}

void ShowTray(HWND hWnd, bool show) {
    if (show && !g_isTrayActive) {
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        g_isTrayActive = true;
    }
    else if (!show && g_isTrayActive) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_isTrayActive = false;
    }
}

void MinimizeToTray(HWND hWnd) {
    ShowTray(hWnd, true);
    ShowWindow(hWnd, SW_HIDE);
}

void RestoreFromTray(HWND hWnd) {
    ShowWindow(hWnd, SW_SHOW);
    ShowWindow(hWnd, SW_RESTORE);
    SetForegroundWindow(hWnd);
    ShowTray(hWnd, false);
}

void ShowTrayContextMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_RESTORE, L"Развернуть (Open G27 Control)");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Выход (Exit)");

    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}


void DrawCard(HDC hdc, int x, int y, int w, int h, const wchar_t* header) {
    HBRUSH bg = CreateSolidBrush(RGB(22, 25, 33));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(38, 44, 58));
    HBRUSH ob = (HBRUSH)SelectObject(hdc, bg);
    HPEN op = (HPEN)SelectObject(hdc, border);

    RoundRect(hdc, x, y, x + w, y + h, 10, 10);

    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(bg);
    DeleteObject(border);

    if (header) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(115, 125, 145));
        RECT tr = { x + 16, y + 10, x + w - 16, y + 26 };
        DrawTextW(hdc, header, -1, &tr, DT_LEFT | DT_SINGLELINE);
    }
}

void DrawToggleSwitch(HDC hdc, const UiToggle& t) {
    int swW = 38, swH = 20;
    int swX = t.rect.left;
    int swY = t.rect.top + (t.rect.bottom - t.rect.top - swH) / 2;

    COLORREF trackClr = *t.valPtr ? RGB(0, 180, 255) : (t.isHovered ? RGB(48, 54, 70) : RGB(35, 40, 52));
    HBRUSH hTrack = CreateSolidBrush(trackClr);
    HPEN hTrackP = CreatePen(PS_SOLID, 1, trackClr);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, hTrack);
    HPEN op = (HPEN)SelectObject(hdc, hTrackP);
    RoundRect(hdc, swX, swY, swX + swW, swY + swH, 20, 20);

    int knobX = *t.valPtr ? (swX + swW - 17) : (swX + 3);
    HBRUSH hKnob = CreateSolidBrush(RGB(255, 255, 255));
    HPEN hKnobP = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    SelectObject(hdc, hKnob);
    SelectObject(hdc, hKnobP);
    Ellipse(hdc, knobX, swY + 3, knobX + 14, swY + 17);

    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(hTrack); DeleteObject(hTrackP);
    DeleteObject(hKnob); DeleteObject(hKnobP);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, t.isHovered ? RGB(255, 255, 255) : RGB(215, 220, 235));
    RECT tr = { swX + swW + 12, t.rect.top, t.rect.right, t.rect.bottom };
    DrawTextW(hdc, t.label.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void DrawModernSlider(HDC hdc, const UiSlider& s) {
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(185, 195, 210));
    RECT tr = { s.rect.left, s.rect.top - 18, s.rect.right - 60, s.rect.top };
    DrawTextW(hdc, s.label.c_str(), -1, &tr, DT_LEFT | DT_SINGLELINE);

    std::wstringstream ss;
    ss << *s.valPtr << s.suffix;
    SetTextColor(hdc, s.color);
    RECT valR = { s.rect.right - 55, s.rect.top - 18, s.rect.right, s.rect.top };
    DrawTextW(hdc, ss.str().c_str(), -1, &valR, DT_RIGHT | DT_SINGLELINE);

    int w = s.rect.right - s.rect.left;
    int h = 6;
    int y = s.rect.top + (s.rect.bottom - s.rect.top - h) / 2;

    HBRUSH hSlot = CreateSolidBrush(RGB(32, 38, 50));
    HPEN hSlotP = CreatePen(PS_SOLID, 1, RGB(45, 52, 68));
    HBRUSH ob = (HBRUSH)SelectObject(hdc, hSlot);
    HPEN op = (HPEN)SelectObject(hdc, hSlotP);
    RoundRect(hdc, s.rect.left, y, s.rect.right, y + h, 6, 6);

    float frac = (float)(*s.valPtr - s.minVal) / (float)(s.maxVal - s.minVal);
    frac = std::clamp(frac, 0.0f, 1.0f);
    int fillW = static_cast<int>(w * frac);

    if (fillW > 0) {
        HBRUSH hFill = CreateSolidBrush(s.color);
        HPEN hFillP = CreatePen(PS_SOLID, 1, s.color);
        SelectObject(hdc, hFill);
        SelectObject(hdc, hFillP);
        RoundRect(hdc, s.rect.left, y, s.rect.left + fillW, y + h, 6, 6);
        DeleteObject(hFill); DeleteObject(hFillP);
    }

    int thumbX = s.rect.left + fillW;
    int thumbY = y + h / 2;
    int rad = s.isDragging ? 8 : (s.isHovered ? 7 : 6);
    HBRUSH hThumb = CreateSolidBrush(RGB(255, 255, 255));
    HPEN hThumbP = CreatePen(PS_SOLID, 2, s.color);
    SelectObject(hdc, hThumb);
    SelectObject(hdc, hThumbP);
    Ellipse(hdc, thumbX - rad, thumbY - rad, thumbX + rad, thumbY + rad);

    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(hSlot); DeleteObject(hSlotP);
    DeleteObject(hThumb); DeleteObject(hThumbP);
}

void DrawModernButton(HDC hdc, const UiButton& btn) {
    COLORREF bg = btn.isActive ? RGB(30, 42, 60) : (btn.isHovered ? RGB(38, 44, 58) : RGB(25, 29, 40));
    COLORREF border = btn.isActive ? btn.accent : (btn.isHovered ? RGB(105, 120, 145) : RGB(42, 48, 64));
    COLORREF textClr = btn.isActive ? btn.accent : (btn.isHovered ? RGB(255, 255, 255) : RGB(200, 210, 225));

    HBRUSH hBg = CreateSolidBrush(bg);
    HPEN hPen = CreatePen(PS_SOLID, btn.isActive ? 2 : 1, border);
    HBRUSH oldB = (HBRUSH)SelectObject(hdc, hBg);
    HPEN oldP = (HPEN)SelectObject(hdc, hPen);

    RoundRect(hdc, btn.rect.left, btn.rect.top, btn.rect.right, btn.rect.bottom, 8, 8);

    SelectObject(hdc, oldB);
    SelectObject(hdc, oldP);
    DeleteObject(hBg); DeleteObject(hPen);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, textClr);
    RECT tr = btn.rect;
    DrawTextW(hdc, btn.label.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void DrawTelemetryBar(HDC hdc, int x, int y, int w, int h, float frac, COLORREF color, const wchar_t* title) {
    frac = std::clamp(frac, 0.0f, 1.0f);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(160, 170, 190));
    RECT tr = { x, y - 18, x + w, y };
    DrawTextW(hdc, title, -1, &tr, DT_LEFT | DT_SINGLELINE);

    HBRUSH hSlot = CreateSolidBrush(RGB(28, 33, 44));
    HPEN hSlotBorder = CreatePen(PS_SOLID, 1, RGB(42, 48, 64));
    HBRUSH ob = (HBRUSH)SelectObject(hdc, hSlot);
    HPEN op = (HPEN)SelectObject(hdc, hSlotBorder);
    RoundRect(hdc, x, y, x + w, y + h, 6, 6);

    int fillW = static_cast<int>((w - 2) * frac);
    if (fillW > 2) {
        HBRUSH hFill = CreateSolidBrush(color);
        HPEN hFillP = CreatePen(PS_SOLID, 1, color);
        SelectObject(hdc, hFill);
        SelectObject(hdc, hFillP);
        RoundRect(hdc, x + 1, y + 1, x + 1 + fillW, y + h - 1, 4, 4);
        DeleteObject(hFill); DeleteObject(hFillP);
    }

    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(hSlot); DeleteObject(hSlotBorder);
}

// =========================================================================
// WNDPROC
// =========================================================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static POINT mousePos = { 0, 0 };

    switch (msg) {
    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        InitTrayIcon(hWnd);
        SetupUiLayout();
        SetTimer(hWnd, 1, 16, NULL);
        break;
    }


    case WM_SYSCOMMAND: {
        if ((wParam & 0xFFF0) == SC_MINIMIZE) {
            MinimizeToTray(hWnd);
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }


    case WM_TRAYICON: {
        if (lParam == WM_LBUTTONDBLCLK) {
            RestoreFromTray(hWnd);
        }
        else if (lParam == WM_RBUTTONUP) {
            ShowTrayContextMenu(hWnd);
        }
        break;
    }

    case WM_COMMAND: {
        int wmId = LOWORD(wParam);
        if (wmId == IDM_TRAY_RESTORE) {
            RestoreFromTray(hWnd);
        }
        else if (wmId == IDM_TRAY_EXIT) {
            ShowTray(hWnd, false);
            DestroyWindow(hWnd);
        }
        break;
    }

    case WM_TIMER: {
        if (!g_Wheel.isConnected) {
            g_Wheel.Open();
        }
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    }

    case WM_MOUSEMOVE: {
        mousePos.x = LOWORD(lParam);
        mousePos.y = HIWORD(lParam);

        if (g_ActiveSliderDrag != -1) {
            auto& s = g_Sliders[g_ActiveSliderDrag];
            float frac = (float)(mousePos.x - s.rect.left) / (float)(s.rect.right - s.rect.left);
            frac = std::clamp(frac, 0.0f, 1.0f);
            *s.valPtr = s.minVal + static_cast<int>(frac * (s.maxVal - s.minVal));
            g_Wheel.ApplyConfig();
        }

        for (auto& s : g_Sliders) s.isHovered = PtInRect(&s.rect, mousePos);
        for (auto& t : g_Toggles) t.isHovered = PtInRect(&t.rect, mousePos);
        for (auto& b : g_Buttons) b.isHovered = PtInRect(&b.rect, mousePos);
        break;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };

        for (size_t i = 0; i < g_Sliders.size(); ++i) {
            RECT hitArea = g_Sliders[i].rect;
            InflateRect(&hitArea, 6, 8);
            if (PtInRect(&hitArea, pt)) {
                g_ActiveSliderDrag = static_cast<int>(i);
                g_Sliders[i].isDragging = true;
                SetCapture(hWnd);

                float frac = (float)(pt.x - g_Sliders[i].rect.left) / (float)(g_Sliders[i].rect.right - g_Sliders[i].rect.left);
                frac = std::clamp(frac, 0.0f, 1.0f);
                *g_Sliders[i].valPtr = g_Sliders[i].minVal + static_cast<int>(frac * (g_Sliders[i].maxVal - g_Sliders[i].minVal));
                g_Wheel.ApplyConfig();
                break;
            }
        }

        for (auto& t : g_Toggles) {
            if (PtInRect(&t.rect, pt)) {
                *t.valPtr = !(*t.valPtr);
                g_Wheel.ApplyConfig();
                break;
            }
        }

        for (auto& b : g_Buttons) {
            if (PtInRect(&b.rect, pt)) {
                if (b.id == 301) { g_Cfg.rotationDeg = 900; g_Wheel.ApplyConfig(); }
                else if (b.id == 302) { g_Cfg.rotationDeg = 540; g_Wheel.ApplyConfig(); }
                else if (b.id == 303) { g_Cfg.rotationDeg = 360; g_Wheel.ApplyConfig(); }
                else if (b.id == 304) { ResetToDefaults(); }
                else if (b.id == 305) { MinimizeToTray(hWnd); }
                else if (b.id == 306) { g_Wheel.TriggerLedTest(); }
                else if (b.id == 307) { ShellExecuteA(NULL, "open", SUPPORT_URL, NULL, NULL, SW_SHOWNORMAL); }
                else if (b.id == 308) { DestroyWindow(hWnd); }
                break;
            }
        }
        break;
    }

    case WM_LBUTTONUP: {
        if (g_ActiveSliderDrag != -1) {
            g_Sliders[g_ActiveSliderDrag].isDragging = false;
            g_ActiveSliderDrag = -1;
            ReleaseCapture();
        }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        HDC mdc = CreateCompatibleDC(hdc);
        HBITMAP mbmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP obmp = (HBITMAP)SelectObject(mdc, mbmp);

        HBRUSH bg = CreateSolidBrush(RGB(14, 16, 22));
        FillRect(mdc, &rc, bg);
        DeleteObject(bg);

        HFONT fHeader = CreateFontW(20, 0, 0, 0, FW_BOLD, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT fCardTitle = CreateFontW(13, 0, 0, 0, FW_BOLD, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT fText = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT fGear = CreateFontW(38, 0, 0, 0, FW_BOLD, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");


        SelectObject(mdc, fHeader);
        SetBkMode(mdc, TRANSPARENT);
        SetTextColor(mdc, RGB(245, 245, 250));
        TextOutW(mdc, 25, 18, L"LOGITECH G27", 39);

        // status
        SelectObject(mdc, fText);
        if (g_Wheel.isConnected) {
            SetTextColor(mdc, RGB(0, 230, 118));
            TextOutW(mdc, 675, 20, L"● DEVICE CONNECTED", 18);
        }
        else {
            SetTextColor(mdc, RGB(255, 60, 80));
            TextOutW(mdc, 675, 20, L"○ SEARCHING USB...", 18);
        }


        DrawCard(mdc, 25, 55, 455, 75, L"PEDALS MODE");
        DrawCard(mdc, 25, 140, 455, 135, L"STEERING ROTATION");
        DrawCard(mdc, 25, 285, 455, 215, L"FORCE FEEDBACK EFFECTS");
        DrawCard(mdc, 25, 510, 455, 120, L"CENTERING SPRING");
        DrawCard(mdc, 25, 640, 455, 68, L"GAME OVERRIDE");


        DrawCard(mdc, 495, 55, 355, 220, L"STEERING ANGLE TELEMETRY");
        DrawCard(mdc, 495, 285, 355, 215, L"PEDAL TELEMETRY");
        DrawCard(mdc, 495, 510, 355, 135, L"SHIFTER & RPM LEDS");

        SelectObject(mdc, fText);
        for (const auto& t : g_Toggles) DrawToggleSwitch(mdc, t);
        for (const auto& s : g_Sliders) DrawModernSlider(mdc, s);

        // degress buttonss
        g_Buttons[0].isActive = (g_Cfg.rotationDeg == 900);
        g_Buttons[1].isActive = (g_Cfg.rotationDeg == 540);
        g_Buttons[2].isActive = (g_Cfg.rotationDeg == 360);

        for (const auto& b : g_Buttons) DrawModernButton(mdc, b);

        // =========================================================================
        // Telemerty
        // =========================================================================
        uint16_t rawSteer = (static_cast<uint16_t>(g_Wheel.buffer[5]) << 8) | g_Wheel.buffer[4];
        float steerDeg = ((float)rawSteer - 32768.0f) * ((float)g_Cfg.rotationDeg / 65535.0f);
        float steerFrac = (float)rawSteer / 65535.0f;

        float gasFrac = (float)(255 - g_Wheel.buffer[6]) / 255.0f;
        float brakeFrac = (float)(255 - g_Wheel.buffer[7]) / 255.0f;
        float clutchFrac = (float)(255 - g_Wheel.buffer[12]) / 255.0f;

        gasFrac = std::clamp(gasFrac, 0.0f, 1.0f);
        brakeFrac = std::clamp(brakeFrac, 0.0f, 1.0f);
        clutchFrac = std::clamp(clutchFrac, 0.0f, 1.0f);

        // Текущий угол руля
        std::wstringstream steerStr;
        steerStr << std::fixed << std::setprecision(1) << steerDeg << L"°";
        SelectObject(mdc, fGear);
        SetTextColor(mdc, RGB(0, 195, 255));
        RECT degR = { 495, 90, 850, 145 };
        DrawTextW(mdc, steerStr.str().c_str(), -1, &degR, DT_CENTER | DT_SINGLELINE);

        SelectObject(mdc, fCardTitle);
        SetTextColor(mdc, RGB(140, 150, 175));
        RECT degLabelR = { 495, 145, 850, 165 };
        DrawTextW(mdc, L"CURRENT STEERING ANGLE", -1, &degLabelR, DT_CENTER | DT_SINGLELINE);

        SelectObject(mdc, fText);
        DrawTelemetryBar(mdc, 515, 195, 315, 14, steerFrac, RGB(255, 255, 255), L"Travel Range");


        if (g_Cfg.combinedPedals) {
            float combinedVal = 0.5f + (gasFrac - brakeFrac) * 0.5f;
            DrawTelemetryBar(mdc, 515, 350, 315, 16, combinedVal, RGB(255, 170, 0), L"Combined Axis (Brake <-> Throttle)");
            DrawTelemetryBar(mdc, 515, 415, 315, 16, clutchFrac, RGB(0, 229, 255), L"Clutch");
        }
        else {
            DrawTelemetryBar(mdc, 515, 340, 315, 13, clutchFrac, RGB(0, 229, 255), L"Clutch");
            DrawTelemetryBar(mdc, 515, 390, 315, 13, brakeFrac, RGB(255, 23, 68), L"Brake");
            DrawTelemetryBar(mdc, 515, 440, 315, 13, gasFrac, RGB(0, 230, 118), L"Throttle");
        }

        // H-Shifter
        uint8_t rawX = g_Wheel.buffer[9];
        uint8_t rawY = g_Wheel.buffer[10];
        uint8_t extra = g_Wheel.buffer[11];
        bool isRev = (extra & 0x40) || (extra & 0x20);

        std::wstring gearStr = L"N";
        COLORREF gearColor = RGB(160, 165, 180);
        if (isRev && rawX > 140 && rawY < 60) { gearStr = L"R"; gearColor = RGB(255, 40, 40); }
        else if (rawY > 160) {
            if (rawX < 90)       gearStr = L"1";
            else if (rawX > 140) gearStr = L"5";
            else                 gearStr = L"3";
            gearColor = RGB(255, 190, 0);
        }
        else if (rawY < 80) {
            if (rawX < 90)       gearStr = L"2";
            else if (rawX > 140) gearStr = L"6";
            else                 gearStr = L"4";
            gearColor = RGB(255, 190, 0);
        }

        // Gear Badge
        HBRUSH hGearBg = CreateSolidBrush(RGB(28, 33, 44));
        HPEN hGearBorder = CreatePen(PS_SOLID, 2, gearColor);
        HBRUSH ogb = (HBRUSH)SelectObject(mdc, hGearBg);
        HPEN ogp = (HPEN)SelectObject(mdc, hGearBorder);
        RoundRect(mdc, 515, 545, 585, 625, 8, 8);
        SelectObject(mdc, ogb); SelectObject(mdc, ogp);
        DeleteObject(hGearBg); DeleteObject(hGearBorder);

        SelectObject(mdc, fGear);
        SetTextColor(mdc, gearColor);
        RECT gr = { 515, 545, 585, 625 };
        DrawTextW(mdc, gearStr.c_str(), -1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // RPM Shift LEDs
        SelectObject(mdc, fCardTitle);
        SetTextColor(mdc, RGB(140, 150, 170));
        RECT ledTitle = { 610, 545, 830, 565 };
        DrawTextW(mdc, L"SHIFT RPM LEDS", -1, &ledTitle, DT_LEFT | DT_SINGLELINE);

        int ledStartX = 612;
        int ledY = 575;
        COLORREF ledColors[5] = { RGB(0, 230, 118), RGB(0, 230, 118), RGB(255, 170, 0), RGB(255, 170, 0), RGB(255, 23, 68) };
        for (int i = 0; i < 5; ++i) {
            bool lit = (gasFrac * 5.0f) > (float)i;
            HBRUSH hLed = CreateSolidBrush(lit ? ledColors[i] : RGB(35, 40, 54));
            HPEN hLedP = CreatePen(PS_SOLID, 1, lit ? ledColors[i] : RGB(50, 58, 76));
            SelectObject(mdc, hLed); SelectObject(mdc, hLedP);
            Ellipse(mdc, ledStartX + i * 42, ledY, ledStartX + i * 42 + 26, ledY + 26);
            DeleteObject(hLed); DeleteObject(hLedP);
        }


        DeleteObject(fHeader);
        DeleteObject(fCardTitle);
        DeleteObject(fText);
        DeleteObject(fGear);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, obmp);
        DeleteObject(mbmp);
        DeleteDC(mdc);

        EndPaint(hWnd, &ps);
        break;
    }

    case WM_DESTROY:
        ShowTray(hWnd, false);
        g_Wheel.EmergencyStopMotors();
        g_Wheel.CloseDevice();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}


int RunApp(HINSTANCE hInstance, int nCmdShow) {
    WNDCLASSEXW wc = { sizeof(wc), CS_HREDRAW | CS_VREDRAW, WndProc, 0, 0, hInstance, NULL, LoadCursor(NULL, IDC_ARROW), NULL, NULL, L"LogitechG27ModernUI", NULL };
    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(
        0, L"LogitechG27ModernUI", L"Logitech G27 // Windows 11 Edition",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 890, 810,
        nullptr, nullptr, hInstance, nullptr
    );

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) { return RunApp(hInstance, nCmdShow); }
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) { return RunApp(hInstance, nCmdShow); }
int main() { return RunApp(GetModuleHandle(NULL), SW_SHOW); }