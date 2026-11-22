#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <shellapi.h>
#include <wtsapi32.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr wchar_t appName[] = L"windows-lockscreen-mute";
constexpr wchar_t settingsKey[] = L"Software\\windows-lockscreen-mute";
constexpr wchar_t runKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr UINT trayMessage = WM_APP + 1;
constexpr UINT_PTR audioTimer = 1;
constexpr int iconResource = 101;

enum MenuItem { toggleEnabled = 1, toggleStartup, exitApp };

constexpr COLORREF menuNavy = RGB(24, 34, 56);
constexpr COLORREF menuOutline = RGB(49, 65, 94);
constexpr COLORREF menuText = RGB(244, 247, 255);
constexpr COLORREF menuTeal = RGB(110, 231, 220);

void fillMenuRect(HDC dc, const RECT& rect, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

template <typename T> struct ComRelease {
    void operator()(T* value) const { value->Release(); }
};
template <typename T> using ComPtr = std::unique_ptr<T, ComRelease<T>>;

bool readEnabled() {
    DWORD enabled = 1;
    DWORD bytes = sizeof(enabled);
    if (RegGetValueW(HKEY_CURRENT_USER, settingsKey, L"Enabled", RRF_RT_REG_DWORD,
                     nullptr, &enabled, &bytes) != ERROR_SUCCESS) {
        return true;
    }
    return enabled != 0;
}

bool writeEnabled(bool enabled) {
    const DWORD value = enabled;
    return RegSetKeyValueW(HKEY_CURRENT_USER, settingsKey, L"Enabled", REG_DWORD,
                           &value, sizeof(value)) == ERROR_SUCCESS;
}

bool startsWithWindows() {
    return RegGetValueW(HKEY_CURRENT_USER, runKey, appName, RRF_RT_REG_SZ,
                        nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

bool setStartup(bool enabled) {
    if (!enabled) {
        return RegDeleteKeyValueW(HKEY_CURRENT_USER, runKey, appName) == ERROR_SUCCESS;
    }
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) {
        return false;
    }
    const std::wstring command = L"\"" + std::wstring(path.data(), length) + L"\"";
    // Windows limits commands stored in Run to 260 characters.
    if (command.size() >= MAX_PATH) {
        return false;
    }
    return RegSetKeyValueW(HKEY_CURRENT_USER, runKey, appName, REG_SZ, command.c_str(),
                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool currentSessionLocked() {
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION,
                                     WTSSessionInfoEx, &buffer, &bytes)) {
        return false;
    }
    const auto info = reinterpret_cast<const WTSINFOEXW*>(buffer);
    const bool locked = bytes >= sizeof(*info) && info->Level == 1 &&
                        info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
    WTSFreeMemory(buffer);
    return locked;
}

ComPtr<IAudioEndpointVolume> endpointVolume(IMMDevice* device) {
    IAudioEndpointVolume* volume = nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&volume)))) {
        return {};
    }
    return ComPtr<IAudioEndpointVolume>(volume);
}

class LockAudio {
public:
    void muteActiveOutputs() {
        auto enumerator = createEnumerator();
        if (!enumerator) {
            return;
        }

        IMMDeviceCollection* rawDevices = nullptr;
        if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &rawDevices))) {
            return;
        }
        ComPtr<IMMDeviceCollection> devices(rawDevices);
        UINT count = 0;
        if (FAILED(devices->GetCount(&count))) {
            return;
        }
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* rawDevice = nullptr;
            if (FAILED(devices->Item(i, &rawDevice))) {
                continue;
            }
            ComPtr<IMMDevice> device(rawDevice);
            LPWSTR rawId = nullptr;
            if (FAILED(device->GetId(&rawId))) {
                continue;
            }
            std::wstring id(rawId);
            CoTaskMemFree(rawId);
            auto volume = endpointVolume(device.get());
            BOOL muted = FALSE;
            if (volume && SUCCEEDED(volume->GetMute(&muted)) && !muted &&
                SUCCEEDED(volume->SetMute(TRUE, nullptr))) {
                mutedByApp_.insert_or_assign(std::move(id), std::move(volume));
            }
        }
    }

    void restore() {
        if (mutedByApp_.empty()) {
            return;
        }
        auto enumerator = createEnumerator();
        for (auto& [id, volume] : mutedByApp_) {
            if (SUCCEEDED(volume->SetMute(FALSE, nullptr)) || !enumerator) {
                continue;
            }
            // An endpoint can be recreated after an audio service restart or device reconnect.
            IMMDevice* rawDevice = nullptr;
            if (SUCCEEDED(enumerator->GetDevice(id.c_str(), &rawDevice))) {
                ComPtr<IMMDevice> device(rawDevice);
                auto replacement = endpointVolume(device.get());
                if (replacement) {
                    replacement->SetMute(FALSE, nullptr);
                }
            }
        }
        mutedByApp_.clear();
    }

private:
    static ComPtr<IMMDeviceEnumerator> createEnumerator() {
        IMMDeviceEnumerator* instance = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&instance)))) {
            return {};
        }
        return ComPtr<IMMDeviceEnumerator>(instance);
    }

    std::map<std::wstring, ComPtr<IAudioEndpointVolume>> mutedByApp_;
};

class TrayApp {
public:
    explicit TrayApp(HINSTANCE instance) : instance_(instance) {}

    int run() {
        icon_ = LoadIconW(instance_, MAKEINTRESOURCEW(iconResource));
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = instance_;
        windowClass.lpszClassName = appName;
        if (!icon_ || !RegisterClassW(&windowClass)) {
            return startupError();
        }

        window_ = CreateWindowExW(0, appName, appName, WS_OVERLAPPED, 0, 0, 0, 0,
                                   nullptr, nullptr, instance_, this);
        if (!window_) {
            return startupError();
        }
        if (!WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION) || !addTrayIcon()) {
            DestroyWindow(window_);
            return startupError();
        }

        locked_ = currentSessionLocked();
        updateAudio();
        MSG message{};
        BOOL status;
        while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (status == -1) {
            DestroyWindow(window_);
            return 1;
        }
        return static_cast<int>(message.wParam);
    }

private:
    static int startupError() {
        MessageBoxW(nullptr, L"Could not start the tray utility or register for session events.",
                    appName, MB_OK | MB_ICONERROR);
        return 1;
    }

    NOTIFYICONDATAW trayData() const {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = iconResource;
        return data;
    }

    bool addTrayIcon() {
        auto data = trayData();
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = trayMessage;
        data.hIcon = icon_;
        lstrcpyW(data.szTip, appName);
        if (!Shell_NotifyIconW(NIM_ADD, &data)) {
            return false;
        }
        data.uVersion = NOTIFYICON_VERSION_4;
        trayVersion4_ = Shell_NotifyIconW(NIM_SETVERSION, &data) != FALSE;
        return true;
    }

    void updateAudio() {
        if (locked_ && enabled_) {
            audio_.muteActiveOutputs();
            // Catch newly connected outputs and retry transient audio service failures.
            SetTimer(window_, audioTimer, 2000, nullptr);
        } else {
            KillTimer(window_, audioTimer);
            audio_.restore();
        }
    }

    UINT trackMenu(HMENU menu, POINT fallback) const {
        NOTIFYICONIDENTIFIER identifier{};
        identifier.cbSize = sizeof(identifier);
        identifier.hWnd = window_;
        identifier.uID = iconResource;

        TPMPARAMS params{};
        params.cbSize = sizeof(params);
        RECT& icon = params.rcExclude;
        if (FAILED(Shell_NotifyIconGetRect(&identifier, &icon))) {
            icon = {fallback.x, fallback.y, fallback.x + 1, fallback.y + 1};
        }

        UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_VERTICAL;
        POINT anchor{icon.left, icon.bottom};
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromRect(&icon, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const RECT& work = monitor.rcWork;
            if (work.right - icon.right < icon.left - work.left) {
                anchor.x = icon.right;
                flags |= TPM_RIGHTALIGN;
            }
            if (work.bottom - icon.bottom < icon.top - work.top) {
                anchor.y = icon.top;
                flags |= TPM_BOTTOMALIGN;
            }
        }
        return TrackPopupMenuEx(menu, flags, anchor.x, anchor.y, window_, &params);
    }

    int menuPixels(int pixels) const {
        return MulDiv(pixels, static_cast<int>(menuDpi_), 96);
    }

    void measureMenuItem(MEASUREITEMSTRUCT& item) const {
        const auto label = reinterpret_cast<const wchar_t*>(item.itemData);
        if (!label) {
            item.itemWidth = 0;
            item.itemHeight = menuPixels(9);
            return;
        }
        HDC dc = GetDC(window_);
        const auto previousFont = SelectObject(dc, menuFont_);
        SIZE text{};
        GetTextExtentPoint32W(dc, label, lstrlenW(label), &text);
        SelectObject(dc, previousFont);
        ReleaseDC(window_, dc);
        item.itemWidth = text.cx + menuPixels(46);
        item.itemHeight = text.cy + menuPixels(12);
    }

    void drawMenuItem(const DRAWITEMSTRUCT& item) const {
        HDC dc = item.hDC;
        const RECT& row = item.rcItem;
        const bool selected = (item.itemState & ODS_SELECTED) != 0;
        fillMenuRect(dc, row, selected ? menuOutline : menuNavy);

        const auto label = reinterpret_cast<const wchar_t*>(item.itemData);
        const LONG center = row.top + (row.bottom - row.top) / 2;
        if (!label) {
            const RECT divider{row.left + menuPixels(10), center,
                               row.right - menuPixels(10), center + menuPixels(1)};
            fillMenuRect(dc, divider, menuOutline);
            return;
        }

        if (item.itemState & ODS_CHECKED) {
            const LONG x = row.left + menuPixels(10);
            const POINT check[] = {{x, center}, {x + menuPixels(4), center + menuPixels(4)},
                                   {x + menuPixels(11), center - menuPixels(4)}};
            const HPEN pen = CreatePen(PS_SOLID, menuPixels(2), menuTeal);
            const auto previousPen = SelectObject(dc, pen);
            Polyline(dc, check, ARRAYSIZE(check));
            SelectObject(dc, previousPen);
            DeleteObject(pen);
        }

        RECT text = row;
        text.left += menuPixels(32);
        const auto previousFont = SelectObject(dc, menuFont_);
        const int previousMode = SetBkMode(dc, TRANSPARENT);
        const COLORREF previousColor = SetTextColor(dc, selected ? menuTeal : menuText);
        DrawTextW(dc, label, -1, &text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SetTextColor(dc, previousColor);
        SetBkMode(dc, previousMode);
        SelectObject(dc, previousFont);
    }

    void showMenu(POINT position) {
        if (menuOpen_) {
            return;
        }
        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return;
        }

        // Use the system menu palette when Windows High Contrast is enabled.
        HIGHCONTRASTW contrast{};
        contrast.cbSize = sizeof(contrast);
        SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0);
        menuDpi_ = GetDpiForWindow(window_);
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);
        if (!(contrast.dwFlags & HCF_HIGHCONTRASTON) && menuDpi_ &&
            SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                       &metrics, 0, menuDpi_)) {
            menuFont_ = CreateFontIndirectW(&metrics.lfMenuFont);
        }
        HBRUSH background = nullptr;
        if (menuFont_) {
            background = CreateSolidBrush(menuNavy);
            MENUINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = MIM_BACKGROUND;
            info.hbrBack = background;
            SetMenuInfo(menu, &info);
        }

        const UINT style = menuFont_ ? MF_OWNERDRAW : MF_STRING;
        AppendMenuW(menu, style | (enabled_ ? MF_CHECKED : MF_UNCHECKED),
                    toggleEnabled, L"Mute when Windows locks");
        AppendMenuW(menu, style | (startsWithWindows() ? MF_CHECKED : MF_UNCHECKED),
                    toggleStartup, L"Launch with Windows");
        AppendMenuW(menu, menuFont_ ? MF_OWNERDRAW | MF_DISABLED : MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, style, exitApp, L"Exit");

        menuOpen_ = true;
        SetForegroundWindow(window_);
        const UINT command = trackMenu(menu, position);
        menuOpen_ = false;
        PostMessageW(window_, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (background) {
            DeleteObject(background);
        }
        if (menuFont_) {
            DeleteObject(menuFont_);
            menuFont_ = nullptr;
        }

        switch (command) {
        case toggleEnabled:
            if (!writeEnabled(!enabled_)) {
                showSettingsError();
                break;
            }
            enabled_ = !enabled_;
            updateAudio();
            break;
        case toggleStartup:
            if (!setStartup(!startsWithWindows())) {
                showSettingsError();
            }
            break;
        case exitApp:
            DestroyWindow(window_);
            break;
        }
    }

    void showSettingsError() const {
        MessageBoxW(window_, L"Windows could not save the setting.", appName, MB_OK | MB_ICONERROR);
    }

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        if (taskbarCreated_ && message == taskbarCreated_) {
            addTrayIcon();
            return 0;
        }
        switch (message) {
        case WM_MEASUREITEM: {
            auto& item = *reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (item.CtlType == ODT_MENU && menuFont_) {
                measureMenuItem(item);
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            const auto& item = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item.CtlType == ODT_MENU && menuFont_) {
                drawMenuItem(item);
                return TRUE;
            }
            break;
        }
        case trayMessage: {
            const auto event = LOWORD(lParam);
            if (trayVersion4_) {
                if (event == WM_CONTEXTMENU || event == NIN_SELECT || event == NIN_KEYSELECT) {
                    showMenu({static_cast<short>(LOWORD(wParam)), static_cast<short>(HIWORD(wParam))});
                }
            } else if (event == WM_RBUTTONUP || event == WM_LBUTTONUP) {
                POINT position{};
                GetCursorPos(&position);
                showMenu(position);
            }
            return 0;
        }
        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK || wParam == WTS_SESSION_UNLOCK) {
                locked_ = wParam == WTS_SESSION_LOCK;
                updateAudio();
            }
            return 0;
        case WM_TIMER:
            if (wParam == audioTimer && locked_ && enabled_) {
                audio_.muteActiveOutputs();
            }
            return 0;
        case WM_ENDSESSION:
            if (wParam) {
                DestroyWindow(window_);
            }
            return 0;
        case WM_DESTROY: {
            KillTimer(window_, audioTimer);
            audio_.restore();
            WTSUnRegisterSessionNotification(window_);
            auto data = trayData();
            Shell_NotifyIconW(NIM_DELETE, &data);
            PostQuitMessage(0);
            return 0;
        }
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            auto app = static_cast<TrayApp*>(create->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        const auto app = reinterpret_cast<TrayApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return app ? app->handleMessage(message, wParam, lParam)
                   : DefWindowProcW(window, message, wParam, lParam);
    }

    HINSTANCE instance_;
    HWND window_ = nullptr;
    HICON icon_ = nullptr;
    HFONT menuFont_ = nullptr;
    UINT menuDpi_ = 96;
    UINT taskbarCreated_ = 0;
    bool menuOpen_ = false;
    bool trayVersion4_ = false;
    bool enabled_ = readEnabled();
    bool locked_ = false;
    LockAudio audio_;
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE singleInstance = CreateMutexW(nullptr, FALSE, L"Local\\windows-lockscreen-mute");
    const DWORD mutexStatus = GetLastError();
    if (!singleInstance) {
        return 1;
    }
    if (mutexStatus == ERROR_ALREADY_EXISTS) {
        CloseHandle(singleInstance);
        return 0;
    }

    int result = 1;
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        {
            TrayApp app(instance);
            result = app.run();
        }
        CoUninitialize();
    } else {
        MessageBoxW(nullptr, L"Could not initialize Windows audio.", appName, MB_OK | MB_ICONERROR);
    }
    CloseHandle(singleInstance);
    return result;
}
