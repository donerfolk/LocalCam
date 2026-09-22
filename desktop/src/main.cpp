// LocalCam: use your phone as a webcam. Phone page → secure WebSocket (H.264) → Media Foundation decode →
// softcam virtual camera. Runs in the tray; the window shows the QR code the phone scans.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#include <mfapi.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <dwmapi.h>
#include <share.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "convert.h"
#include "decoder.h"
#include "qrcodegen.hpp"
#include "server.h"
#include "softcam.h"

// Common Controls 6: themed buttons.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

using Clock = std::chrono::steady_clock;

constexpr UINT WM_TRAY = WM_APP + 1, WM_REFRESH = WM_APP + 2, WM_SHOW = WM_APP + 3;
constexpr wchar_t kWindowClass[] = L"LocalCamWindow";
constexpr wchar_t kSettingsKey[] = L"Software\\LocalCam";
constexpr wchar_t kCameraKey[] = L"CLSID\\{DD86CB9D-7004-4ECC-80AD-103B020E7974}\\InprocServer32";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr DWORD kDefaultPort = 47474;
enum Command { kShow = 1, kCopyLink, kStartup, kNewCode, k720 = 10, k1080, k30 = 20, k60, kMirror = 30, kRotate0 = 40,
               kRotateLeft = 50, kRotateRight, kShape0 = 60, kQuality0 = 70, kPower = 80, kExit = 99, kAddress0 = 100 };
enum Shape { kWide, kStandard, kPortrait };

// Settings live in HKCU\Software\LocalCam. The phone-facing ones are read from connection threads.
std::atomic<int> g_height{1080}, g_fps{30}, g_rotate{0}, g_shape{kWide}, g_quality{1};  // g_height: the short side
std::atomic<bool> g_mirror{false};
std::atomic<bool> g_on{true}, g_phoneConnected{false};  // g_on: the on/off switch, on at every start
std::string g_token, g_url, g_address;  // g_url: the phone page at g_address, one of g_ips
std::vector<std::string> g_ips;
int g_port;
PCCERT_CONTEXT g_cert;
std::wstring g_dumpPath;

HWND g_hwnd, g_rotateLeft, g_rotateRight, g_power;
HFONT g_buttonFont;
NOTIFYICONDATAW g_tray{};
UINT g_taskbarCreated;

std::mutex g_statusMutex;
std::wstring g_status = L"Waiting for your phone";

DWORD regGet(const wchar_t* name, DWORD fallback) {
    DWORD value, size = sizeof value;
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) ? fallback : value;
}

void regSet(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &value, sizeof value);
}

std::string regGetString(const wchar_t* name) {
    wchar_t buf[64];
    DWORD size = sizeof buf;
    std::string s;
    if (!RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_SZ, nullptr, buf, &size))
        for (const wchar_t* p = buf; *p; p++) s += char(*p);
    return s;
}

void regSetString(const wchar_t* name, const std::string& value) {
    const std::wstring w(value.begin(), value.end());
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_SZ, w.c_str(), DWORD((w.size() + 1) * sizeof(wchar_t)));
}

// Start at sign-in: the same Run value the installer's checkbox writes.
bool startsWithWindows() {
    return !RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"LocalCam", RRF_RT_REG_SZ, nullptr, nullptr, nullptr);
}

void setStartsWithWindows(bool on) {
    if (!on) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"LocalCam");
        return;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --tray";
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"LocalCam", REG_SZ, cmd.c_str(), DWORD((cmd.size() + 1) * sizeof(wchar_t)));
}

// The virtual camera's size: g_height is its short side, g_shape its aspect ratio.
void outputSize(int& w, int& h) {
    const int s = g_height, shape = g_shape;
    w = shape == kPortrait ? s : shape == kStandard ? s * 4 / 3 : s * 16 / 9;
    h = shape == kPortrait ? s * 16 / 9 : s;
}

void setAddress(const std::string& ip) {
    g_address = ip;
    g_url = "https://" + ip + ":" + std::to_string(g_port) + "/?t=" + g_token;
}

// New random pairing token, saved; empty if Windows can't provide randomness.
std::string newToken() {
    unsigned char r[16];
    if (BCryptGenRandom(nullptr, r, sizeof r, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) return {};
    std::string token;
    for (unsigned char b : r) token += "0123456789abcdef"[b >> 4], token += "0123456789abcdef"[b & 15];
    regSetString(L"Token", token);
    return token;
}

// Kept across runs so a bookmarked phone page keeps working, until "New pairing code".
std::string loadToken() {
    const std::string token = regGetString(L"Token");
    return token.size() == 32 ? token : newToken();
}

void setStatus(const std::wstring& s) {
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_status = s;
    }
    PostMessageW(g_hwnd, WM_REFRESH, 0, 0);
}

std::wstring status() {
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_status;
}

// "Waiting for phone" card, drawn with GDI, as BGR24.
std::vector<uint8_t> renderPlaceholder(int w, int h, const std::wstring& headline, const std::wstring& hint) {
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    RECT all{0, 0, w, h};
    HBRUSH bg = CreateSolidBrush(RGB(24, 24, 27));
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    auto text = [&](const std::wstring& s, int size, int weight, COLORREF color, int top) {
        HFONT f = CreateFontW(-size, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, f);
        SetTextColor(dc, color);
        RECT r{w / 10, top, w - w / 10, h};
        DrawTextW(dc, s.c_str(), -1, &r, DT_CENTER | DT_WORDBREAK);
        SelectObject(dc, oldFont);
        DeleteObject(f);
    };
    const int s = std::min(w, h);  // text size follows the short side, so portrait cards match landscape ones
    text(L"LocalCam", s / 20, FW_SEMIBOLD, RGB(161, 161, 170), h / 2 - s * 16 / 100);
    text(headline, s / 11, FW_SEMIBOLD, RGB(244, 244, 245), h / 2 - s * 8 / 100);
    text(hint, s / 28, FW_NORMAL, RGB(161, 161, 170), h / 2 + s * 6 / 100);
    GdiFlush();
    std::vector<uint8_t> out(size_t(w) * h * 3);
    const auto* src = static_cast<const uint8_t*>(bits);
    for (size_t i = 0, n = size_t(w) * h; i < n; i++) std::memcpy(&out[i * 3], src + i * 4, 3);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    return out;
}

// The softcam virtual camera. Its size is fixed while it exists (video apps can't follow a change),
// so phone video of any size or orientation is fitted into it.
class Camera {
public:
    bool open(int width, int height) {
        std::lock_guard<std::mutex> lock(m_);
        if (cam_) scDeleteCamera(cam_);
        w_ = width;
        h_ = height;
        cam_ = scCreateCamera(w_, h_, 0);  // 0 = deliver each frame immediately, no pacing delay
        frame_.assign(size_t(w_) * h_ * 3, 0);
        placeholder_ = renderPlaceholder(w_, h_, headline_, hint_);
        return cam_ != nullptr;
    }

    void live(const Nv12& f, int rotate, bool mirror) {
        std::lock_guard<std::mutex> lock(m_);
        if (!cam_) return;
        drawNv12(f, frame_.data(), w_, h_, rotate, mirror);
        scSendFrame(cam_, frame_.data());
        lastLive_ = Clock::now();
    }

    void idle(const std::wstring& headline, const std::wstring& hint) {
        std::lock_guard<std::mutex> lock(m_);
        headline_ = headline;
        hint_ = hint;
        placeholder_ = renderPlaceholder(w_, h_, headline_, hint_);
    }

    // Called every 100 ms: shows the placeholder once live video has stopped for a second.
    void tick() {
        std::lock_guard<std::mutex> lock(m_);
        if (cam_ && Clock::now() - lastLive_ > std::chrono::seconds(1)) scSendFrame(cam_, placeholder_.data());
    }

    // Copies the latest live frame (what video apps get) for the window's preview. False when not live.
    bool snapshot(std::vector<uint8_t>& out, int& w, int& h) {
        std::lock_guard<std::mutex> lock(m_);
        if (!cam_ || Clock::now() - lastLive_ > std::chrono::seconds(1)) return false;
        out = frame_;
        w = w_;
        h = h_;
        return true;
    }

private:
    std::mutex m_;
    scCamera cam_ = nullptr;
    int w_ = 0, h_ = 0;
    std::vector<uint8_t> frame_, placeholder_;
    std::wstring headline_ = L"Waiting for your phone";
    std::wstring hint_ = L"Open LocalCam on this PC and scan the QR code with your phone.";
    Clock::time_point lastLive_{};
};

Camera g_camera;

// IPv4 addresses, best first: Wi-Fi/Ethernet with a gateway (the home network), then VPNs, then the rest.
std::vector<std::string> localAddresses() {
    ULONG size = 32 * 1024;
    std::vector<char> buf(size);
    const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    std::vector<std::pair<int, std::string>> found;
    for (auto* a = rc ? nullptr : reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        const bool physical = a->IfType == IF_TYPE_IEEE80211 || a->IfType == IF_TYPE_ETHERNET_CSMACD;
        const int rank = a->FirstGatewayAddress ? (physical ? 0 : 1) : 2;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            char s[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr, s, sizeof s);
            if (std::strncmp(s, "169.254.", 8)) found.push_back({rank, s});
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::string> ips;
    for (auto& f : found) ips.push_back(f.second);
    return ips;
}

// The phone asks its camera for the output size held sideways: cameras deliver landscape, and a portrait
// output takes the phone held upright (or a rotation).
std::string configJson() {
    int w, h;
    outputSize(w, h);
    const char* quality[] = {"low", "normal", "high"};
    return "{\"type\":\"config\",\"width\":" + std::to_string(std::max(w, h)) + ",\"height\":" +
           std::to_string(std::min(w, h)) + ",\"fps\":" + std::to_string(g_fps) + ",\"rotate\":" +
           std::to_string(g_rotate * 90) + ",\"quality\":\"" + quality[g_quality] + "\",\"on\":" + (g_on ? "true" : "false") + "}";
}

void showOff() {
    g_camera.idle(L"Camera off", L"Turn it back on in LocalCam, on your PC or your phone.");
    setStatus(L"Camera off. Your phone's camera is stopped until you turn it back on.");
}

// The on/off switch: off stops the phone's camera and shows a "Camera off" card. Tray, window or phone.
void setOn(bool on) {
    if (on == g_on) return;
    g_on = on;
    sendToPhone(configJson());
    if (!on) {
        showOff();
    } else if (g_phoneConnected) {
        g_camera.idle(L"Starting your phone's camera", L"Allow camera access on your phone if it asks.");
        setStatus(L"Phone connected, starting its camera…");
    } else {
        g_camera.idle(L"Waiting for your phone", L"Open LocalCam on this PC and scan the QR code with your phone.");
        setStatus(L"Waiting for your phone");
    }
}

// Rotation (quarter turns clockwise) is one PC-side setting, changed from the tray menu, the window or the phone.
void setRotate(int quarters) {
    g_rotate = quarters & 3;
    regSet(L"Rotate", g_rotate);
    sendToPhone(configJson());
    PostMessageW(g_hwnd, WM_REFRESH, 0, 0);
}

double jsonNumber(const std::string& s, const char* key) {
    const size_t p = s.find("\"" + std::string(key) + "\":");
    return p == std::string::npos ? 0 : std::strtod(s.c_str() + p + std::strlen(key) + 3, nullptr);
}

bool isType(const std::string& s, const char* type) {
    return s.find("\"type\":\"" + std::string(type) + "\"") != std::string::npos;
}

// One phone stream, on its connection thread.
void handlePhone(Conn* c) {
    logf("phone connected");
    g_phoneConnected = true;
    if (g_on) {
        setStatus(L"Phone connected, starting its camera…");
        g_camera.idle(L"Starting your phone's camera", L"Allow camera access on your phone if it asks.");
    }
    sendToPhone(configJson());
    auto decoder = std::make_unique<Decoder>();
    if (!decoder->ok()) {
        logf("can't create the H.264 decoder");
        setStatus(L"Can't start Windows' H.264 decoder.");
        g_phoneConnected = false;
        return;
    }
    FILE* dump = g_dumpPath.empty() ? nullptr : _wfsopen(g_dumpPath.c_str(), L"wb", _SH_DENYWR);
    int phoneRotate = 0;  // orientation the phone's encoder reports, on top of the user's setting
    bool phoneFlip = false;
    bool paused = false;  // the phone page went to the background, which stops its camera
    int frames = 0, width = 0, height = 0;
    size_t bytes = 0;
    double decodeMs = 0, encodeMs = 0, rttMs = 0;
    auto windowStart = Clock::now();
    std::string msg;
    for (int op; (op = readMessage(c, msg));) {
        if (op == 2) {
            if (dump) std::fwrite(msg.data(), 1, msg.size(), dump);
            const auto t0 = Clock::now();
            const bool ok = decoder->decode(reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), [&](const Nv12& f) {
                if (!g_on) return;  // frames still in flight after switching off
                g_camera.live(f, (g_rotate + phoneRotate) & 3, g_mirror != phoneFlip);
                frames++;
                width = f.width;
                height = f.height;
            });
            decodeMs += std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            bytes += msg.size();
            if (paused && frames) {
                paused = false;
                g_camera.idle(L"Waiting for your phone's video", L"Keep the LocalCam page open on your phone.");
            }
            if (!ok) {
                logf("decode error 0x%08lX, asking the phone for a keyframe", static_cast<unsigned long>(decoder->lastError()));
                decoder = std::make_unique<Decoder>();
                sendToPhone(R"({"type":"key"})");
            }
        } else if (isType(msg, "pong")) {
            rttMs = double(GetTickCount64()) - jsonNumber(msg, "t");
        } else if (isType(msg, "stats")) {
            encodeMs = jsonNumber(msg, "encodeMs");
        } else if (isType(msg, "rotate")) {
            setRotate(int(jsonNumber(msg, "rotate")) / 90);
        } else if (isType(msg, "orientation")) {
            phoneRotate = int(jsonNumber(msg, "rotation")) / 90 & 3;
            phoneFlip = msg.find("\"flip\":true") != std::string::npos;
        } else if (isType(msg, "power")) {
            setOn(msg.find("\"on\":true") != std::string::npos);
        } else if (isType(msg, "paused")) {
            paused = true;
            g_camera.idle(L"Camera paused", L"Bring the LocalCam page back to the front on your phone, and keep it unlocked.");
            setStatus(L"Camera paused: the LocalCam page on your phone is in the background or locked.");
        }

        const double elapsed = std::chrono::duration<double>(Clock::now() - windowStart).count();
        if (elapsed >= 1) {
            if (frames) {
                wchar_t s[160];
                swprintf(s, 160, L"Live: %d×%d at %d fps, %.1f Mbps, ~%d ms latency", width, height,
                         int(frames / elapsed + 0.5), bytes * 8 / elapsed / 1e6, int(encodeMs + rttMs / 2 + decodeMs / frames + 0.5));
                setStatus(s);
            }
            frames = 0;
            bytes = 0;
            decodeMs = 0;
            windowStart = Clock::now();
        }
    }
    if (dump) std::fclose(dump);
    logf("phone disconnected");
    g_phoneConnected = false;
    if (paused || !g_on) return;  // keep saying why the video stopped
    g_camera.idle(L"Phone disconnected", L"Open the LocalCam page on your phone again to reconnect.");
    setStatus(L"Phone disconnected. Open the LocalCam page on your phone to reconnect.");
}

// Makes video apps load the softcam.dll next to this exe. Unless the installer already registered this very
// DLL for everyone, registers it for the current user, which needs no admin. Run before starting threads:
// the HKCR override is process-wide.
bool registerCamera() {
    wchar_t exe[MAX_PATH], current[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dll = exe;
    dll = dll.substr(0, dll.find_last_of(L'\\') + 1) + L"softcam.dll";
    DWORD size = sizeof current;
    RegGetValueW(HKEY_CLASSES_ROOT, kCameraKey, nullptr, RRF_RT_REG_SZ, nullptr, current, &size);
    if (!_wcsicmp(current, dll.c_str())) return true;

    using RegisterFn = HRESULT(STDAPICALLTYPE*)();
    const auto registerServer = reinterpret_cast<RegisterFn>(GetProcAddress(GetModuleHandleW(L"softcam.dll"), "DllRegisterServer"));
    HKEY classes;
    if (!registerServer ||
        RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &classes, nullptr))
        return false;
    RegOverridePredefKey(HKEY_CLASSES_ROOT, classes);  // the DLL's HKCR writes land in this user's classes
    const HRESULT hr = registerServer();
    RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
    RegCloseKey(classes);
    logf("registered the camera for this user: 0x%08lX", static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

bool g_cameraRegistered = false;

// Window palette: dark zinc, the same as the phone page and the "waiting" card.
constexpr COLORREF kBg = RGB(24, 24, 27), kCard = RGB(39, 39, 42), kPressed = RGB(63, 63, 70), kInk = RGB(244, 244, 245),
                   kMuted = RGB(161, 161, 170), kWarn = RGB(248, 113, 113), kLive = RGB(239, 68, 68), kWait = RGB(245, 158, 11);

void fill(HDC dc, const RECT& r, COLORREF color, int radius = 0) {
    HBRUSH b = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(dc, b), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);  // +1: NULL_PEN draws a pixel short
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(b);
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC dc = CreateCompatibleDC(wdc);  // double-buffered: status updates every second
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    const int dpi = int(GetDpiForWindow(hwnd));
    auto px = [dpi](int v) { return MulDiv(v, dpi, 96); };
    fill(dc, rc, kBg);
    SetBkMode(dc, TRANSPARENT);

    int y = px(20);
    // Wraps s between the window margins (or left/right), draws it unless measuring, and moves y below it.
    auto text = [&](const std::wstring& s, int pt, int weight, COLORREF color, int left = 0, int right = 0,
                    UINT align = DT_CENTER, bool draw = true) {
        HFONT f = CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, f);
        SetTextColor(dc, color);
        if (!left) left = px(24), right = rc.right - px(24);
        RECT r{left, y, right, rc.bottom};
        DrawTextW(dc, s.c_str(), -1, &r, align | DT_WORDBREAK | DT_EDITCONTROL | DT_CALCRECT);
        r.left = left;
        r.right = right;
        if (draw) DrawTextW(dc, s.c_str(), -1, &r, align | DT_WORDBREAK | DT_EDITCONTROL);  // EDITCONTROL: wrap the long URL
        y = r.bottom;
        SelectObject(dc, oldFont);
        DeleteObject(f);
    };

    text(L"LocalCam", 20, FW_SEMIBOLD, kInk);
    static std::vector<uint8_t> frame;  // reused: only the UI thread paints
    int fw, fh;
    if (g_camera.snapshot(frame, fw, fh)) {
        text(L"Preview: what your video apps see", 10, FW_NORMAL, kMuted);
        y += px(16);
        int pw = rc.right - px(48), ph = pw * fh / fw;
        if (ph > px(300)) ph = px(300), pw = ph * fw / fh;  // portrait output
        const int x = (rc.right - pw) / 2;
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), fw, -fh, 1, 24, BI_RGB};  // rows are 4-byte aligned at 720p/1080p
        HRGN corners = CreateRoundRectRgn(x, y, x + pw + 1, y + ph + 1, px(16), px(16));
        SelectClipRgn(dc, corners);
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc, x, y, pw, ph, 0, 0, fw, fh, frame.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        SelectClipRgn(dc, nullptr);
        DeleteObject(corners);
        y += ph + px(16);
    } else if (g_url.empty()) {
        y += px(24);
        text(L"This PC isn't on a network. Connect it to the same Wi-Fi as your phone, then restart LocalCam.", 11, FW_NORMAL, kInk);
    } else {
        text(L"Scan this with your phone's camera", 10, FW_NORMAL, kMuted);
        y += px(16);
        const auto qr = qrcodegen::QrCode::encodeText(g_url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int n = qr.getSize(), module = std::max(1, px(200) / n), pad = px(16), x0 = (rc.right - module * n) / 2;
        fill(dc, {x0 - pad, y, x0 + module * n + pad, y + module * n + 2 * pad}, RGB(255, 255, 255), px(16));  // the quiet zone scanners need
        y += pad;
        HBRUSH ink = CreateSolidBrush(kBg);
        for (int qy = 0; qy < n; qy++)
            for (int qx = 0; qx < n; qx++)
                if (qr.getModule(qx, qy)) {
                    RECT m{x0 + qx * module, y + qy * module, x0 + (qx + 1) * module, y + (qy + 1) * module};
                    FillRect(dc, &m, ink);
                }
        DeleteObject(ink);
        y += module * n + pad + px(12);
        text(std::wstring(g_url.begin(), g_url.end()), 9, FW_NORMAL, kMuted);
        y += px(4);
    }

    // Status card: a dot (red when live, amber while waiting, grey otherwise) beside the status line.
    y += px(12);
    const std::wstring s = status();
    const COLORREF dot = !s.rfind(L"Live", 0) ? kLive : !s.rfind(L"Waiting", 0) || !s.rfind(L"Phone connected", 0) ? kWait : kMuted;
    const int top = y, pad = px(12), d = px(8), left = px(24) + 2 * pad + d, right = rc.right - px(24) - pad;
    y += pad;
    text(s, 10, FW_SEMIBOLD, kInk, left, right, DT_LEFT, false);  // measure, so the card fits the wrapped text
    fill(dc, {px(24), top, rc.right - px(24), y + pad}, kCard, px(12));
    const int mid = top + pad + MulDiv(10, dpi, 72) * 2 / 3;  // middle of the first line
    fill(dc, {px(24) + pad, mid - d / 2, px(24) + pad + d, mid + d / 2}, dot, d);
    y = top + pad;
    text(s, 10, FW_SEMIBOLD, kInk, left, right, DT_LEFT);
    y += pad + px(16);

    text(L"First time only: your phone warns that the connection isn't private. That's expected for a "
         L"local device. Tap Advanced (Android) or Show Details (iPhone), then continue to the site.",
         9, FW_NORMAL, kMuted);
    if (!g_cameraRegistered) {
        y += px(16);
        text(L"Couldn't register the LocalCam camera, so video apps can't see it. Reinstall LocalCam.", 9, FW_SEMIBOLD, kWarn);
    }

    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

// The buttons are owner-drawn pills, like the phone page's; a ring shows keyboard focus.
void drawButton(const DRAWITEMSTRUCT& d) {
    RECT r = d.rcItem;
    fill(d.hDC, r, kBg);
    if ((d.itemState & ODS_FOCUS) && !(d.itemState & ODS_NOFOCUSRECT)) {
        fill(d.hDC, r, kMuted, r.bottom - r.top);
        const int ring = MulDiv(2, int(GetDpiForWindow(d.hwndItem)), 96);
        InflateRect(&r, -ring, -ring);
    }
    fill(d.hDC, r, d.itemState & ODS_SELECTED ? kPressed : kCard, r.bottom - r.top);
    wchar_t label[64];
    GetWindowTextW(d.hwndItem, label, 64);
    HGDIOBJ oldFont = SelectObject(d.hDC, g_buttonFont);
    SetBkMode(d.hDC, TRANSPARENT);
    SetTextColor(d.hDC, kInk);
    DrawTextW(d.hDC, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(d.hDC, oldFont);
}

// The rotate buttons sit at the bottom of the window; re-laid out on resize and DPI change.
void layoutButtons() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    const int dpi = int(GetDpiForWindow(g_hwnd));
    auto px = [dpi](int v) { return MulDiv(v, dpi, 96); };
    if (g_buttonFont) DeleteObject(g_buttonFont);
    g_buttonFont = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    const int w = px(112), h = px(36), gap = px(8), y = rc.bottom - h - px(20), x = (rc.right - 3 * w - 2 * gap) / 2;
    HWND buttons[] = {g_power, g_rotateLeft, g_rotateRight};
    for (int i = 0; i < 3; i++) {
        SetWindowPos(buttons[i], nullptr, x + i * (w + gap), y, w, h, SWP_NOZORDER);
        InvalidateRect(buttons[i], nullptr, FALSE);
    }
}

void addTrayIcon() {
    g_tray.cbSize = sizeof g_tray;
    g_tray.hWnd = g_hwnd;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wcscpy_s(g_tray.szTip, L"LocalCam");
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}

void trayTip() {
    g_tray.uFlags = NIF_TIP;
    wcsncpy_s(g_tray.szTip, (L"LocalCam: " + status()).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void balloon(const wchar_t* text) {
    g_tray.uFlags = NIF_INFO;
    wcscpy_s(g_tray.szInfoTitle, L"LocalCam");
    wcsncpy_s(g_tray.szInfo, text, _TRUNCATE);
    g_tray.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

void copyText(const std::wstring& s) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        std::memcpy(GlobalLock(mem), s.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

void showWindow() {
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(g_hwnd);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void showMenu() {
    HMENU menu = CreatePopupMenu(), res = CreatePopupMenu(), fps = CreatePopupMenu(), rot = CreatePopupMenu(),
          addr = CreatePopupMenu(), shape = CreatePopupMenu(), quality = CreatePopupMenu();
    auto item = [](bool checked) { return UINT(MF_STRING | (checked ? MF_CHECKED : 0)); };
    AppendMenuW(res, item(g_height == 720), k720, L"720p");
    AppendMenuW(res, item(g_height == 1080), k1080, L"1080p");
    AppendMenuW(fps, item(g_fps == 30), k30, L"30 fps");
    AppendMenuW(fps, item(g_fps == 60), k60, L"60 fps");
    const wchar_t* shapes[] = {L"Widescreen (16:9)", L"Standard (4:3)", L"Portrait (9:16)"};
    for (int i = 0; i < 3; i++) AppendMenuW(shape, item(g_shape == i), kShape0 + i, shapes[i]);
    const wchar_t* qualities[] = {L"Low (weak Wi-Fi)", L"Normal", L"High"};
    for (int i = 0; i < 3; i++) AppendMenuW(quality, item(g_quality == i), kQuality0 + i, qualities[i]);
    const wchar_t* angles[] = {L"None", L"90° clockwise", L"180°", L"90° counterclockwise"};
    for (int i = 0; i < 4; i++) AppendMenuW(rot, item(g_rotate == i), kRotate0 + i, angles[i]);
    for (size_t i = 0; i < g_ips.size(); i++)
        AppendMenuW(addr, item(g_ips[i] == g_address), kAddress0 + i, std::wstring(g_ips[i].begin(), g_ips[i].end()).c_str());
    AppendMenuW(menu, item(g_on), kPower, L"Camera on");
    AppendMenuW(menu, MF_STRING, kShow, L"Show QR code");
    if (!g_url.empty()) AppendMenuW(menu, MF_STRING, kCopyLink, L"Copy phone link");
    AppendMenuW(menu, MF_STRING, kNewCode, L"New pairing code…");
    if (g_ips.size() > 1) AppendMenuW(menu, MF_POPUP, UINT_PTR(addr), L"PC address");  // for PCs with VPNs, Hyper-V…
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_POPUP, UINT_PTR(res), L"Resolution");
    AppendMenuW(menu, MF_POPUP, UINT_PTR(shape), L"Shape");
    AppendMenuW(menu, MF_POPUP, UINT_PTR(fps), L"Frame rate");
    AppendMenuW(menu, MF_POPUP, UINT_PTR(quality), L"Quality");
    AppendMenuW(menu, MF_POPUP, UINT_PTR(rot), L"Rotate");
    AppendMenuW(menu, item(g_mirror), kMirror, L"Mirror");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, item(startsWithWindows()), kStartup, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, kExit, L"Exit");
    SetMenuDefaultItem(menu, kShow, FALSE);
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// Recreates the virtual camera at the new size; video apps only pick it up when they reopen it.
void resizeCamera() {
    int w, h;
    outputSize(w, h);
    g_camera.open(w, h);
    sendToPhone(configJson());
    balloon(L"Camera size changed. Reopen the camera in your video app to use it.");
}

void onCommand(int id) {
    if (id == kShow) {
        showWindow();
    } else if (id == kPower) {
        setOn(!g_on);
    } else if (id == kCopyLink) {
        copyText(std::wstring(g_url.begin(), g_url.end()));
        balloon(L"Phone link copied. It works only on this network.");
    } else if (id == kStartup) {
        setStartsWithWindows(!startsWithWindows());
    } else if (id == kNewCode) {
        if (MessageBoxW(g_hwnd, L"Old QR codes, links and bookmarks stop working, and the phone streaming now is "
                                L"disconnected. Scan the new QR code to connect again.",
                        L"New pairing code", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
            return;
        const std::string token = newToken();
        if (token.empty()) {
            balloon(L"Couldn't make a new pairing code.");
            return;
        }
        g_token = token;
        setToken(token);
        if (!g_address.empty()) setAddress(g_address);
        showWindow();
    } else if (id >= kAddress0 && id < kAddress0 + int(g_ips.size())) {
        const std::string ip = g_ips[id - kAddress0];
        regSetString(L"Address", ip);
        if (certUsable(g_cert, ip)) {  // the certificate must name the address, or phones refuse it
            setAddress(ip);
            InvalidateRect(g_hwnd, nullptr, FALSE);
        } else {
            balloon(L"Restart LocalCam to use this address.");
        }
    } else if (id == k720 || id == k1080) {
        const int h = id == k720 ? 720 : 1080;
        if (h == g_height) return;
        g_height = h;
        regSet(L"Height", h);
        resizeCamera();
    } else if (id >= kShape0 && id < kShape0 + 3) {
        if (id - kShape0 == g_shape) return;
        g_shape = id - kShape0;
        regSet(L"Shape", g_shape);
        resizeCamera();
    } else if (id >= kQuality0 && id < kQuality0 + 3) {
        g_quality = id - kQuality0;
        regSet(L"Quality", g_quality);
        sendToPhone(configJson());
    } else if (id == k30 || id == k60) {
        g_fps = id == k30 ? 30 : 60;
        regSet(L"Fps", g_fps);
        sendToPhone(configJson());
    } else if (id == kMirror) {
        g_mirror = !g_mirror;
        regSet(L"Mirror", g_mirror);
    } else if (id >= kRotate0 && id < kRotate0 + 4) {
        setRotate(id - kRotate0);
    } else if (id == kRotateLeft || id == kRotateRight) {
        setRotate(g_rotate + (id == kRotateRight ? 1 : -1));
    } else if (id == kExit) {
        DestroyWindow(g_hwnd);
    }
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated) {  // Explorer restarted
        addTrayIcon();
        return 0;
    }
    switch (msg) {
    case WM_PAINT: paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;  // keep running in the tray: the camera needs us
    case WM_SHOW: showWindow(); return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP) showWindow();
        else if (lp == WM_RBUTTONUP) showMenu();
        return 0;
    case WM_COMMAND: onCommand(LOWORD(wp)); return 0;
    case WM_DRAWITEM: drawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp)); return TRUE;
    case WM_TIMER:  // preview repaint, ~30 fps while the window is open
        if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_REFRESH:
        InvalidateRect(hwnd, nullptr, FALSE);
        SetWindowTextW(g_power, g_on ? L"Turn off" : L"Turn on");
        InvalidateRect(g_power, nullptr, FALSE);  // owner-drawn: repaint the new label
        trayTip();
        return 0;
    case WM_SIZE: layoutButtons(); return 0;
    case WM_DPICHANGED: {
        const auto* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_tray);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::string loadPage() {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));  // RT_RCDATA
    return std::string(static_cast<const char*>(LockResource(LoadResource(nullptr, r))), SizeofResource(nullptr, r));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    bool startHidden = false;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--tray")) startHidden = true;               // launched at sign-in
        else if (!wcscmp(argv[i], L"--dump") && i + 1 < argc) g_dumpPath = argv[++i];  // raw H.264, for debugging
    }
    LocalFree(argv);

    CreateMutexW(nullptr, TRUE, L"Local\\LocalCam");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kWindowClass, nullptr)) PostMessageW(other, WM_SHOW, 0, 0);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    g_height = regGet(L"Height", 1080) == 720 ? 720 : 1080;
    g_fps = regGet(L"Fps", 30) == 60 ? 60 : 30;
    g_rotate = int(regGet(L"Rotate", 0) & 3);
    g_mirror = regGet(L"Mirror", 0) != 0;
    g_shape = int(std::min<DWORD>(regGet(L"Shape", kWide), kPortrait));
    g_quality = int(std::min<DWORD>(regGet(L"Quality", 1), 2));
    g_port = int(regGet(L"Port", kDefaultPort));
    g_token = loadToken();
    if (g_token.empty()) {
        MessageBoxW(nullptr, L"Can't create a pairing code: Windows didn't provide random numbers.", L"LocalCam", MB_ICONERROR);
        return 1;
    }
    g_ips = localAddresses();
    const std::string chosen = regGetString(L"Address");  // picked in the tray menu; first, so the certificate covers it
    std::stable_partition(g_ips.begin(), g_ips.end(), [&](const std::string& ip) { return ip == chosen; });
    if (!g_ips.empty()) setAddress(g_ips[0]);
    // The address only: logs end up in bug reports, and the full link carries the pairing token.
    logf("LocalCam %s", g_address.empty() ? "(no network address)" : ("https://" + g_address + ":" + std::to_string(g_port)).c_str());

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    const UINT dpi = GetDpiForSystem();
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r{0, 0, MulDiv(400, dpi, 96), MulDiv(630, dpi, 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    g_hwnd = CreateWindowExW(0, kWindowClass, L"LocalCam", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    const BOOL dark = TRUE;  // dark title bar to match; 20 is DWMWA_USE_IMMERSIVE_DARK_MODE, missing from older SDKs
    DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof dark);
    const DWORD buttonStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
    g_power = CreateWindowW(L"BUTTON", L"Turn off", buttonStyle, 0, 0, 0, 0, g_hwnd, HMENU(kPower), inst, nullptr);
    g_rotateLeft = CreateWindowW(L"BUTTON", L"↺  Rotate left", buttonStyle, 0, 0, 0, 0, g_hwnd, HMENU(kRotateLeft), inst, nullptr);
    g_rotateRight = CreateWindowW(L"BUTTON", L"Rotate right  ↻", buttonStyle, 0, 0, 0, 0, g_hwnd, HMENU(kRotateRight), inst, nullptr);
    layoutButtons();

    g_cameraRegistered = registerCamera();
    std::string error;
    g_cert = loadOrCreateCert(g_ips, &error);
    if (!g_cert || !startServer(g_port, g_token, loadPage(), g_cert, handlePhone, &error)) {
        MessageBoxA(nullptr, error.c_str(), "LocalCam", MB_ICONERROR);
        return 1;
    }
    int camW, camH;
    outputSize(camW, camH);
    if (!g_camera.open(camW, camH)) {
        MessageBoxW(nullptr, L"Can't create the LocalCam camera. Is another copy of LocalCam running?", L"LocalCam", MB_ICONERROR);
        return 1;
    }
    std::thread([] {
        for (unsigned i = 0;; i++) {
            g_camera.tick();
            if (i % 20 == 0) sendToPhone("{\"type\":\"ping\",\"t\":" + std::to_string(GetTickCount64()) + "}");
            Sleep(100);
        }
    }).detach();

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    addTrayIcon();
    SetTimer(g_hwnd, 1, 33, nullptr);
    if (!startHidden) showWindow();
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_hwnd, &m)) continue;  // Tab and Enter/Space for the buttons
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
