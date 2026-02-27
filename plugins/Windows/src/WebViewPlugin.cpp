/*
 * Copyright (C) 2011 Keijiro Takahashi
 * Copyright (C) 2012 GREE, Inc.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <vector>
#include <map>
#include <atomic>
#include <memory>
#include <regex>
#include <wrl.h>
#include <dcomp.h>
#include <WebView2.h>

#include <d3d11.h>
#include <dxgi.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;

#define EXPORT __declspec(dllexport)

enum {
    WM_WEBVIEW_LOADURL = WM_USER + 1,
    WM_WEBVIEW_LOADHTML,
    WM_WEBVIEW_EVALUATEJS,
    WM_WEBVIEW_GOBACK,
    WM_WEBVIEW_GOFORWARD,
    WM_WEBVIEW_RELOAD,
    WM_WEBVIEW_SETVISIBILITY,
    WM_WEBVIEW_SETRECT,
    WM_WEBVIEW_DESTROY,
    WM_WEBVIEW_CAPTURE,
    WM_WEBVIEW_MOUSEEVENT,
    WM_WEBVIEW_SETZOOM,
    WM_WEBVIEW_CLEARCACHE,
    WM_WEBVIEW_SETSCROLLBARSVISIBILITY,
    WM_WEBVIEW_PAUSE,
    WM_WEBVIEW_RESUME,
    WM_WEBVIEW_GETCOOKIES,
    WM_WEBVIEW_CLEARCOOKIE,
    WM_WEBVIEW_CLEARALLCOOKIES,
};

struct MouseEventData {
    int x;
    int y;
    float deltaY;
    int mouseState;
};

struct CookieOpData {
    std::wstring url;
    std::wstring name;
};

class WebViewInstance;

static bool s_inEditor = false;
static std::vector<WebViewInstance*> s_instances;
static std::mutex s_instancesMutex;

static std::wstring Utf8ToWide(const char* utf8) {
    if (!utf8 || !*utf8) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring result(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &result[0], len);
    return result;
}

static std::string WideToUtf8(const wchar_t* wide) {
    if (!wide || !*wide) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string result(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &result[0], len, nullptr, nullptr);
    return result;
}

static int HexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    return -1;
}

static std::wstring PercentDecode(const std::wstring& s) {
    std::string bytes;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            int hi = HexVal(s[i + 1]);
            int lo = HexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                bytes += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        // Non-percent characters: encode back to UTF-8
        wchar_t wc = s[i];
        char buf[4];
        int len = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, buf, sizeof(buf), nullptr, nullptr);
        bytes.append(buf, len);
    }
    // Convert the complete UTF-8 byte sequence to wide string
    if (bytes.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring result(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), static_cast<int>(bytes.size()), &result[0], len);
    return result;
}

static std::wstring GetUserDataPath() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring path(tempPath);
    path += L"UnityWebView";
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

static bool IsWGCSupported() {
    typedef LONG(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    auto fn = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!fn) return false;
    RTL_OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    fn(&vi);
    return vi.dwBuildNumber >= 19041;
}

class WebViewInstance {
    std::thread m_thread;
    DWORD m_threadId = 0;
    HANDLE m_readyEvent = nullptr;

    ComPtr<ICoreWebView2Environment> m_environment;
    ComPtr<ICoreWebView2Controller> m_controller;
    ComPtr<ICoreWebView2CompositionController> m_compositionController;
    ComPtr<ICoreWebView2> m_webview;
    ComPtr<ICoreWebView2CookieManager> m_cookieManager;

    ComPtr<IDCompositionDevice> m_dcompDevice;
    ComPtr<IDCompositionTarget> m_dcompTarget;
    ComPtr<IDCompositionVisual> m_dcompVisual;

    HWND m_hwnd = nullptr;
    HWND m_browserHwnd = nullptr;

    std::string m_gameObject;
    bool m_transparent = false;
    bool m_zoom = true;
    bool m_separated = false;
    std::atomic<int> m_width{0};
    std::atomic<int> m_height{0};
    bool m_visible = true;
    std::string m_userAgent;

    std::queue<std::string> m_messages;
    std::mutex m_messageMutex;

    std::atomic<bool> m_initialized{false};

    std::map<std::string, std::string> m_customHeaders;
    std::mutex m_headerMutex;

    std::unique_ptr<std::wregex> m_allowRegex;
    std::unique_ptr<std::wregex> m_denyRegex;
    std::unique_ptr<std::wregex> m_hookRegex;
    std::mutex m_patternMutex;

    std::string m_pendingUrl;
    std::atomic<int> m_devicePixelRatio{1};

    // Map folder paths to unique virtual host names for file:// URL serving
    std::map<std::wstring, std::wstring> m_folderHostMap;
    int m_hostCounter = 0;

    // Double-buffered bitmap storage for offscreen capture
    std::vector<uint8_t> m_bitmaps[2];
    int m_currentBitmap = 0;
    int m_bitmapWidth = 0;
    int m_bitmapHeight = 0;
    bool m_needsDisplay = false;
    std::atomic<bool> m_inRendering{false};
    std::mutex m_bitmapMutex;

    std::string m_basicAuthUser;
    std::string m_basicAuthPass;
    std::mutex m_authMutex;
    std::atomic<bool> m_interactionEnabled{true};
    std::atomic<bool> m_alertDialogEnabled{true};
    std::atomic<bool> m_scrollbarsVisible{true};

    // State for canGoBack/canGoForward/progress
    std::atomic<bool> m_canGoBack{false};
    std::atomic<bool> m_canGoForward{false};
    std::atomic<int> m_progress{0};

    // WIC factory for PNG decoding (cached to avoid per-frame CoCreateInstance)
    ComPtr<IWICImagingFactory> m_wicFactory;

    // Windows Graphics Capture
    std::atomic<bool> m_useWGC{false};
    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<ID3D11DeviceContext> m_d3dContext;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_captureSession{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::FrameArrived_revoker m_frameArrivedRevoker;
    ComPtr<ID3D11Texture2D> m_stagingTexture;
    std::atomic<bool> m_wgcNeedsResize{false};

public:
    WebViewInstance(const char* gameObject, bool transparent, bool zoom,
                    int width, int height, const char* ua, bool separated)
        : m_gameObject(gameObject ? gameObject : "")
        , m_transparent(transparent)
        , m_zoom(zoom)
        , m_separated(separated)
        , m_width(width > 0 ? width : 960)
        , m_height(height > 0 ? height : 600)
        , m_userAgent(ua ? ua : "")
    {
        if (!m_separated)
            m_scrollbarsVisible.store(false);
        m_readyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_thread = std::thread(&WebViewInstance::threadProc, this);
        WaitForSingleObject(m_readyEvent, 10000);
    }

    ~WebViewInstance() {
        if (m_threadId != 0) {
            PostThreadMessageW(m_threadId, WM_WEBVIEW_DESTROY, 0, 0);
        }
        if (m_thread.joinable()) {
            auto handle = m_thread.native_handle();
            if (WaitForSingleObject(handle, 5000) != WAIT_OBJECT_0) {
                // Clean shutdown didn't complete; post WM_QUIT as secondary signal
                if (m_threadId != 0) {
                    PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
                }
                if (WaitForSingleObject(handle, 2000) == WAIT_OBJECT_0) {
                    m_thread.join();
                } else {
                    m_thread.detach();
                }
            } else {
                m_thread.join();
            }
        }
        if (m_readyEvent) {
            CloseHandle(m_readyEvent);
            m_readyEvent = nullptr;
        }
    }

    bool isInitialized() { return m_initialized.load(); }

    void addMessage(const std::string& msg) {
        std::lock_guard<std::mutex> lock(m_messageMutex);
        m_messages.push(msg);
    }

    const char* getMessage() {
        std::lock_guard<std::mutex> lock(m_messageMutex);
        if (m_messages.empty()) return nullptr;
        std::string msg = m_messages.front();
        m_messages.pop();
        size_t len = msg.size() + 1;
        char* r = (char*)CoTaskMemAlloc(len);
        if (!r) return nullptr;
        memcpy(r, msg.c_str(), len);
        return r;
    }

    void loadURL(const char* url) {
        if (!url) return;
        auto* copy = _strdup(url);
        if (!PostThreadMessageW(m_threadId, WM_WEBVIEW_LOADURL, 0, reinterpret_cast<LPARAM>(copy))) {
            free(copy);
        }
    }

    void loadHTML(const char* html, const char* baseUrl) {
        if (!html) return;
        auto* copy = _strdup(html);
        if (!PostThreadMessageW(m_threadId, WM_WEBVIEW_LOADHTML, 0, reinterpret_cast<LPARAM>(copy))) {
            free(copy);
        }
    }

    void evaluateJS(const char* js) {
        if (!js) return;
        auto* copy = _strdup(js);
        if (!PostThreadMessageW(m_threadId, WM_WEBVIEW_EVALUATEJS, 0, reinterpret_cast<LPARAM>(copy))) {
            free(copy);
        }
    }

    void goBack() {
        PostThreadMessageW(m_threadId, WM_WEBVIEW_GOBACK, 0, 0);
    }

    void goForward() {
        PostThreadMessageW(m_threadId, WM_WEBVIEW_GOFORWARD, 0, 0);
    }

    void reload() {
        PostThreadMessageW(m_threadId, WM_WEBVIEW_RELOAD, 0, 0);
    }

    void setRect(int width, int height) {
        m_width = width;
        m_height = height;
        PostThreadMessageW(m_threadId, WM_WEBVIEW_SETRECT, 0, 0);
    }

    void setVisibility(bool visible) {
        m_visible = visible;
        PostThreadMessageW(m_threadId, WM_WEBVIEW_SETVISIBILITY, static_cast<WPARAM>(visible), 0);
    }

    bool setURLPattern(const char* allow, const char* deny, const char* hook) {
        std::lock_guard<std::mutex> lock(m_patternMutex);
        try {
            if (allow && *allow) {
                m_allowRegex = std::make_unique<std::wregex>(Utf8ToWide(allow));
            } else {
                m_allowRegex.reset();
            }
            if (deny && *deny) {
                m_denyRegex = std::make_unique<std::wregex>(Utf8ToWide(deny));
            } else {
                m_denyRegex.reset();
            }
            if (hook && *hook) {
                m_hookRegex = std::make_unique<std::wregex>(Utf8ToWide(hook));
            } else {
                m_hookRegex.reset();
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    int progress() { return m_progress.load(); }
    bool canGoBack() { return m_canGoBack.load(); }
    bool canGoForward() { return m_canGoForward.load(); }

    // Find the actual WebView2 browser child HWND for input forwarding
    HWND getBrowserHwnd() {
        if (m_browserHwnd) return m_browserHwnd;
        if (!m_hwnd) return nullptr;
        // WebView2 creates a child window hierarchy: our HWND > intermediate > Chrome_WidgetWin_0
        // We need the deepest child that accepts input
        HWND child = m_hwnd;
        HWND next = nullptr;
        while ((next = FindWindowExW(child, nullptr, nullptr, nullptr)) != nullptr) {
            child = next;
        }
        if (child != m_hwnd)
            m_browserHwnd = child;
        return (child != m_hwnd) ? child : m_hwnd;
    }

    void sendMouseEvent(int x, int y, float deltaY, int mouseState) {
        if (!m_hwnd || !m_controller) return;
        if (!m_interactionEnabled.load()) return;

        if (m_compositionController) {
            // Marshal to WebView2 thread — SendMouseInput is a COM call
            auto* data = new MouseEventData{x, y, deltaY, mouseState};
            if (!PostThreadMessageW(m_threadId, WM_WEBVIEW_MOUSEEVENT, 0, reinterpret_cast<LPARAM>(data))) {
                delete data;
            }
        } else {
            // Separated/fallback: post Win32 messages to browser HWND
            int wy = m_height.load() - y;
            HWND target = getBrowserHwnd();
            LPARAM lParam = MAKELPARAM(x, wy);

            switch (mouseState) {
            case 1: // mouse down
                PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, lParam);
                break;
            case 2: // mouse drag
                PostMessageW(target, WM_MOUSEMOVE, MK_LBUTTON, lParam);
                break;
            case 3: // mouse up
                PostMessageW(target, WM_LBUTTONUP, 0, lParam);
                break;
            default: // mouse move (no button)
                PostMessageW(target, WM_MOUSEMOVE, 0, lParam);
                break;
            }
            if (deltaY != 0.0f) {
                int scrollAmount = static_cast<int>(deltaY * -120);
                char js[256];
                snprintf(js, sizeof(js),
                    "window.scrollBy({top:%d,behavior:'smooth'})", scrollAmount);
                auto* copy = _strdup(js);
                PostThreadMessageW(m_threadId, WM_WEBVIEW_EVALUATEJS, 0, reinterpret_cast<LPARAM>(copy));
            }
        }
    }

    void sendKeyEvent(int x, int y, const wchar_t* keyChars, unsigned short keyCode, int keyState) {
        if (!m_hwnd) return;
        if (!m_interactionEnabled.load()) return;
        HWND target = getBrowserHwnd();

        // Map control character codes to virtual key codes for WM_KEYDOWN
        UINT vk = 0;
        switch (keyCode) {
        case 0x08: vk = VK_BACK; break;
        case 0x09: vk = VK_TAB; break;
        case 0x0D: case 0x0A: vk = VK_RETURN; break;
        case 0x1B: vk = VK_ESCAPE; break;
        case 0x7F: vk = VK_DELETE; break;
        }

        switch (keyState) {
        case 1: // key down
        case 2: // key repeat
        {
            LPARAM lp = keyState == 2 ? (1 << 30) : 0;
            if (vk) {
                PostMessageW(target, WM_KEYDOWN, vk, lp);
            } else if (keyChars && keyChars[0]) {
                PostMessageW(target, WM_CHAR, static_cast<WPARAM>(keyChars[0]), lp);
            }
            break;
        }
        case 3: // key up
            if (vk) {
                PostMessageW(target, WM_KEYUP, vk, (1 << 30) | (1 << 31));
            }
            break;
        }
    }

    void update(bool refreshBitmap, int devicePixelRatio) {
        if (devicePixelRatio < 1) devicePixelRatio = 1;
        if (devicePixelRatio != m_devicePixelRatio) {
            m_devicePixelRatio = devicePixelRatio;
            // Resize HWND to CSS pixel dimensions; WGC captures at this size
            PostThreadMessageW(m_threadId, WM_WEBVIEW_SETRECT, 0, 0);
        }
        if (m_useWGC) return;
        if (refreshBitmap && !m_inRendering && m_webview) {
            m_inRendering = true;
            PostThreadMessageW(m_threadId, WM_WEBVIEW_CAPTURE, 0, 0);
        }
    }

    int bitmapWidth() {
        std::lock_guard<std::mutex> lock(m_bitmapMutex);
        return m_bitmapWidth;
    }

    int bitmapHeight() {
        std::lock_guard<std::mutex> lock(m_bitmapMutex);
        return m_bitmapHeight;
    }

    void render(void* textureBuffer) {
        std::lock_guard<std::mutex> lock(m_bitmapMutex);
        if (!m_needsDisplay) return;
        if (m_bitmaps[m_currentBitmap].empty()) return;
        m_needsDisplay = false;
        memcpy(textureBuffer, m_bitmaps[m_currentBitmap].data(),
               m_bitmapWidth * m_bitmapHeight * 4);
    }

    void addCustomHeader(const char* key, const char* value) {
        if (!key || !value) return;
        std::lock_guard<std::mutex> lock(m_headerMutex);
        m_customHeaders[key] = value;
    }

    void removeCustomHeader(const char* key) {
        if (!key) return;
        std::lock_guard<std::mutex> lock(m_headerMutex);
        m_customHeaders.erase(key);
    }

    const char* getCustomHeaderValue(const char* key) {
        if (!key) return nullptr;
        std::lock_guard<std::mutex> lock(m_headerMutex);
        auto it = m_customHeaders.find(key);
        if (it == m_customHeaders.end()) return nullptr;
        const std::string& val = it->second;
        size_t len = val.size() + 1;
        char* r = (char*)CoTaskMemAlloc(len);
        if (!r) return nullptr;
        memcpy(r, val.c_str(), len);
        return r;
    }

    void clearCustomHeader() {
        std::lock_guard<std::mutex> lock(m_headerMutex);
        m_customHeaders.clear();
    }

    void getCookies(const char* url) {
        if (!url) return;
        auto* data = new CookieOpData{Utf8ToWide(url), L""};
        if (!PostThreadMessageW(m_threadId, WM_WEBVIEW_GETCOOKIES, 0, reinterpret_cast<LPARAM>(data))) {
            delete data;
        }
    }

    bool hasCookieManager() { return m_cookieManager != nullptr; }

    void setBasicAuthInfo(const char* user, const char* pass) {
        std::lock_guard<std::mutex> lock(m_authMutex);
        m_basicAuthUser = user ? user : "";
        m_basicAuthPass = pass ? pass : "";
    }

    void clearCache(bool includeDiskFiles) {
        PostThreadMessageW(m_threadId, WM_WEBVIEW_CLEARCACHE, static_cast<WPARAM>(includeDiskFiles), 0);
    }

    void setInteractionEnabled(bool enabled) {
        m_interactionEnabled.store(enabled);
    }

    void setScrollbarsVisibility(bool visible) {
        PostThreadMessageW(m_threadId, WM_WEBVIEW_SETSCROLLBARSVISIBILITY, static_cast<WPARAM>(visible), 0);
    }

    void setAlertDialogEnabled(bool enabled) {
        m_alertDialogEnabled.store(enabled);
    }

    void pause() {
        PostThreadMessageW(m_threadId, WM_WEBVIEW_PAUSE, 0, 0);
    }

    void resume() {
        PostThreadMessageW(m_threadId, WM_WEBVIEW_RESUME, 0, 0);
    }

    void clearAllCookies() {
        PostThreadMessageW(m_threadId, WM_WEBVIEW_CLEARALLCOOKIES, 0, 0);
    }

    void clearCookie(const char* url, const char* name) {
        if (!url || !name) return;
        auto* data = new CookieOpData{Utf8ToWide(url), Utf8ToWide(name)};
        if (!PostThreadMessageW(m_threadId, WM_WEBVIEW_CLEARCOOKIE, 0, reinterpret_cast<LPARAM>(data))) {
            delete data;
        }
    }

    static std::wstring getScrollbarHideScript() {
        return
            L"(function() {"
            L"  if (document.getElementById('__wv_no_scrollbar')) return;"
            L"  var s = document.createElement('style');"
            L"  s.id = '__wv_no_scrollbar';"
            L"  s.textContent = '"
            L"    html::-webkit-scrollbar, body::-webkit-scrollbar, *::-webkit-scrollbar"
            L"      { display: none !important; width: 0 !important; height: 0 !important; }"
            L"    html, body, * { scrollbar-width: none !important; -ms-overflow-style: none !important; }"
            L"  ';"
            L"  (document.head || document.documentElement).appendChild(s);"
            L"  if (document.readyState === 'loading') {"
            L"    document.addEventListener('DOMContentLoaded', function() {"
            L"      if (!document.getElementById('__wv_no_scrollbar')) {"
            L"        document.head.appendChild(s.cloneNode(true));"
            L"      }"
            L"    });"
            L"  }"
            L"})();";
    }

private:
    void decodePngFromStream(IStream* stream, std::vector<uint8_t>& buffer, int& width, int& height) {
        if (!m_wicFactory) {
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&m_wicFactory))))
                return;
        }
        auto& factory = m_wicFactory;

        ComPtr<IWICBitmapDecoder> decoder;
        HRESULT hr = factory->CreateDecoderFromStream(stream, nullptr,
                                               WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(hr)) return;

        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr)) return;

        UINT w, h;
        hr = frame->GetSize(&w, &h);
        if (FAILED(hr)) return;

        ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(&converter);
        if (FAILED(hr)) return;

        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) return;

        width = static_cast<int>(w);
        height = static_cast<int>(h);
        buffer.resize(w * h * 4);
        converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(buffer.size()), buffer.data());
    }

    bool initD3D11Device() {
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels, 1, D3D11_SDK_VERSION,
            &m_d3dDevice, nullptr, &m_d3dContext);
        if (FAILED(hr)) return false;

        ComPtr<IDXGIDevice> dxgiDevice;
        hr = m_d3dDevice.As(&dxgiDevice);
        if (FAILED(hr)) return false;

        winrt::com_ptr<::IInspectable> inspectable;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put());
        if (FAILED(hr)) return false;

        m_winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        return true;
    }

    bool initWGC() {
        if (!IsWGCSupported()) return false;
        if (!initD3D11Device()) return false;

        try {
            auto interopFactory = winrt::get_activation_factory<
                winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                IGraphicsCaptureItemInterop>();

            winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
            HRESULT hr = interopFactory->CreateForWindow(
                m_hwnd,
                winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                winrt::put_abi(item));
            if (FAILED(hr) || !item) {
                teardownWGC();
                return false;
            }

            auto size = item.Size();
            m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                m_winrtDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                size);

            m_frameArrivedRevoker = m_framePool.FrameArrived(
                winrt::auto_revoke,
                [this](auto& sender, auto&) { onFrameArrived(sender); });

            m_captureSession = m_framePool.CreateCaptureSession(item);
            m_captureSession.IsCursorCaptureEnabled(false);

            // Disable yellow border on Windows 11+
            if (auto session3 = m_captureSession.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession3>()) {
                session3.IsBorderRequired(false);
            }

            m_captureSession.StartCapture();
            m_useWGC = true;
            return true;
        } catch (...) {
            teardownWGC();
            return false;
        }
    }

    void onFrameArrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender) {
        if (m_inRendering.exchange(true)) return;

        auto frame = sender.TryGetNextFrame();
        if (!frame) {
            m_inRendering.store(false);
            return;
        }

        if (m_wgcNeedsResize.exchange(false)) {
            auto size = frame.ContentSize();
            sender.Recreate(
                m_winrtDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                size);
            m_inRendering.store(false);
            return;
        }

        auto surface = frame.Surface();
        auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> frameTexture;
        HRESULT hr = access->GetInterface(IID_PPV_ARGS(&frameTexture));
        if (FAILED(hr)) {
            m_inRendering.store(false);
            return;
        }

        D3D11_TEXTURE2D_DESC desc;
        frameTexture->GetDesc(&desc);
        int w = static_cast<int>(desc.Width);
        int h = static_cast<int>(desc.Height);
        if (w <= 0 || h <= 0) {
            m_inRendering.store(false);
            return;
        }

        ensureStagingTexture(w, h);
        if (!m_stagingTexture) {
            m_inRendering.store(false);
            return;
        }

        m_d3dContext->CopyResource(m_stagingTexture.Get(), frameTexture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = m_d3dContext->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            m_inRendering.store(false);
            return;
        }

        int backBuffer;
        {
            std::lock_guard<std::mutex> lock(m_bitmapMutex);
            backBuffer = 1 - m_currentBitmap;
        }
        size_t bufSize = static_cast<size_t>(w) * h * 4;
        m_bitmaps[backBuffer].resize(bufSize);

        // BGRA -> RGBA swizzle
        uint8_t* dst = m_bitmaps[backBuffer].data();
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
        for (int row = 0; row < h; row++) {
            const uint8_t* srcRow = src + row * mapped.RowPitch;
            uint8_t* dstRow = dst + row * w * 4;
            for (int col = 0; col < w; col++) {
                dstRow[col * 4 + 0] = srcRow[col * 4 + 2]; // R <- B
                dstRow[col * 4 + 1] = srcRow[col * 4 + 1]; // G
                dstRow[col * 4 + 2] = srcRow[col * 4 + 0]; // B <- R
                dstRow[col * 4 + 3] = srcRow[col * 4 + 3]; // A
            }
        }

        m_d3dContext->Unmap(m_stagingTexture.Get(), 0);

        {
            std::lock_guard<std::mutex> lock(m_bitmapMutex);
            m_bitmapWidth = w;
            m_bitmapHeight = h;
            m_currentBitmap = backBuffer;
            m_needsDisplay = true;
        }

        m_inRendering.store(false);
    }

    void ensureStagingTexture(int width, int height) {
        if (m_stagingTexture) {
            D3D11_TEXTURE2D_DESC existing;
            m_stagingTexture->GetDesc(&existing);
            if (existing.Width == static_cast<UINT>(width) &&
                existing.Height == static_cast<UINT>(height))
                return;
        }
        m_stagingTexture = nullptr;
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        m_d3dDevice->CreateTexture2D(&desc, nullptr, &m_stagingTexture);
    }

    void teardownWGC() {
        m_useWGC = false;
        m_frameArrivedRevoker.revoke();
        if (m_captureSession) {
            m_captureSession.Close();
            m_captureSession = nullptr;
        }
        if (m_framePool) {
            m_framePool.Close();
            m_framePool = nullptr;
        }
        // Brief wait for any in-flight onFrameArrived to finish
        for (int i = 0; i < 100 && m_inRendering.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        m_inRendering.store(false);
        m_stagingTexture = nullptr;
        m_winrtDevice = nullptr;
        m_d3dContext = nullptr;
        m_d3dDevice = nullptr;
    }

    void threadProc() {
        m_threadId = GetCurrentThreadId();

        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) {
            SetEvent(m_readyEvent);
            return;
        }

        const wchar_t* className = L"WebViewPluginWindow";
        static std::once_flag s_classOnce;
        std::call_once(s_classOnce, [&]() {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(WNDCLASSEXW);
            wc.lpfnWndProc = WndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = className;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            RegisterClassExW(&wc);
        });

        DWORD style;
        int x, y, w, h;
        if (m_separated) {
            style = WS_OVERLAPPEDWINDOW;
            x = CW_USEDEFAULT;
            y = CW_USEDEFAULT;
            w = m_width;
            h = m_height;
        } else {
            style = WS_POPUP;
            x = -10000;
            y = -10000;
            w = m_width;
            h = m_height;
        }

        m_hwnd = CreateWindowExW(
            0, className,
            L"WebView",
            style,
            x, y, w, h,
            nullptr, nullptr,
            GetModuleHandleW(nullptr),
            this);

        if (!m_hwnd) {
            SetEvent(m_readyEvent);
            CoUninitialize();
            return;
        }

        if (m_separated) {
            ShowWindow(m_hwnd, SW_SHOW);
            UpdateWindow(m_hwnd);
        } else {
            // Must show for DComp visual tree to be active (window is off-screen)
            ShowWindow(m_hwnd, SW_SHOWNA);
        }

        SetEvent(m_readyEvent);

        initWebView2();

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            if (msg.message == WM_WEBVIEW_DESTROY) {
                break;
            }
            if (msg.hwnd == nullptr) {
                handleThreadMessage(msg);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        teardownWGC();
        m_compositionController = nullptr;
        m_dcompVisual = nullptr;
        m_dcompTarget = nullptr;
        m_dcompDevice = nullptr;
        if (m_controller) {
            m_controller->Close();
            m_controller = nullptr;
        }
        m_webview = nullptr;
        m_environment = nullptr;

        if (m_hwnd) {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }

        CoUninitialize();
    }

    void handleThreadMessage(const MSG& msg) {
        switch (msg.message) {
        case WM_WEBVIEW_LOADURL: {
            auto* url = reinterpret_cast<char*>(msg.lParam);
            if (!m_webview && url) {
                m_pendingUrl = url;
                free(url);
                break;
            }
            if (m_webview && url) {
                std::wstring wurl = Utf8ToWide(url);
                std::wstring navigateUrl = wurl;

                // Map file:// URLs to a virtual host so local content loads correctly.
                // WebView2 blocks JS modules/CORS on file:// origins.
                if (wurl.find(L"file://") == 0 || wurl.find(L"file:///") == 0) {
                    ComPtr<ICoreWebView2_3> webview3;
                    if (SUCCEEDED(m_webview.As(&webview3)) && webview3) {
                        // Strip file:// prefix
                        std::wstring path = wurl.substr(wurl.find(L"file://") + 7);
                        // Strip leading slash for Windows paths like /C:/...
                        if (path.size() > 2 && path[0] == L'/' && path[2] == L':') {
                            path = path.substr(1);
                        }

                        // Split off fragment (#...) and query (?...) BEFORE slash conversion
                        std::wstring suffix;
                        auto hashPos = path.find(L'#');
                        auto queryPos = path.find(L'?');
                        auto splitPos = hashPos < queryPos ? hashPos : queryPos;
                        if (splitPos != std::wstring::npos) {
                            suffix = path.substr(splitPos);
                            path = path.substr(0, splitPos);
                        }

                        // Replace forward slashes with backslashes for filesystem path
                        for (auto& c : path) {
                            if (c == L'/') c = L'\\';
                        }

                        // Percent-decode the filesystem path (e.g. %20→space, %C3%A9→é)
                        path = PercentDecode(path);

                        // Extract directory and filename
                        auto lastSlash = path.rfind(L'\\');
                        if (lastSlash != std::wstring::npos) {
                            std::wstring folder = path.substr(0, lastSlash);
                            std::wstring filename = path.substr(lastSlash + 1);

                            // Reuse existing host for this folder, or create a new one
                            auto it = m_folderHostMap.find(folder);
                            std::wstring hostName;
                            if (it != m_folderHostMap.end()) {
                                hostName = it->second;
                            } else {
                                hostName = L"localapp" + std::to_wstring(m_hostCounter++) + L".webview";
                                m_folderHostMap[folder] = hostName;
                                webview3->SetVirtualHostNameToFolderMapping(
                                    hostName.c_str(),
                                    folder.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            }

                            navigateUrl = L"https://" + hostName + L"/" + filename + suffix;
                        }
                    }
                }

                m_webview->Navigate(navigateUrl.c_str());
            }
            free(url);
            break;
        }
        case WM_WEBVIEW_LOADHTML: {
            auto* html = reinterpret_cast<char*>(msg.lParam);
            if (m_webview && html) {
                std::wstring whtml = Utf8ToWide(html);
                m_webview->NavigateToString(whtml.c_str());
            }
            free(html);
            break;
        }
        case WM_WEBVIEW_EVALUATEJS: {
            auto* js = reinterpret_cast<char*>(msg.lParam);
            if (m_webview && js) {
                std::wstring wjs = Utf8ToWide(js);
                m_webview->ExecuteScript(wjs.c_str(), nullptr);
            }
            free(js);
            break;
        }
        case WM_WEBVIEW_GOBACK:
            if (m_webview) m_webview->GoBack();
            break;
        case WM_WEBVIEW_GOFORWARD:
            if (m_webview) m_webview->GoForward();
            break;
        case WM_WEBVIEW_RELOAD:
            if (m_webview) m_webview->Reload();
            break;
        case WM_WEBVIEW_SETVISIBILITY:
            if (m_controller) {
                m_controller->put_IsVisible(static_cast<BOOL>(msg.wParam));
            }
            break;
        case WM_WEBVIEW_SETRECT:
            if (m_controller && m_hwnd) {
                int w = m_width.load();
                int h = m_height.load();
                int dpr = m_devicePixelRatio.load();
                if (dpr < 1) dpr = 1;
                int cssW = m_separated ? w : w / dpr;
                int cssH = m_separated ? h : h / dpr;
                RECT rc = {0, 0, cssW, cssH};
                m_controller->put_Bounds(rc);
                if (m_separated) {
                    SetWindowPos(m_hwnd, nullptr, 0, 0, w, h,
                                 SWP_NOMOVE | SWP_NOZORDER);
                } else {
                    SetWindowPos(m_hwnd, nullptr, 0, 0, cssW, cssH,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                    if (m_useWGC) {
                        m_wgcNeedsResize.store(true);
                    }
                }
            }
            break;
        case WM_WEBVIEW_SETZOOM:
            // Handled via SETRECT — HWND resized to CSS dimensions
            break;
        case WM_WEBVIEW_CAPTURE: {
            if (!m_webview) {
                m_inRendering = false;
                break;
            }
            ComPtr<IStream> stream;
            CreateStreamOnHGlobal(nullptr, TRUE, &stream);
            m_webview->CapturePreview(
                COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
                stream.Get(),
                Callback<ICoreWebView2CapturePreviewCompletedHandler>(
                    [this, stream](HRESULT errorCode) -> HRESULT {
                        if (SUCCEEDED(errorCode)) {
                            LARGE_INTEGER li = {};
                            stream->Seek(li, STREAM_SEEK_SET, nullptr);

                            int newBitmap;
                            {
                                std::lock_guard<std::mutex> lock(m_bitmapMutex);
                                newBitmap = 1 - m_currentBitmap;
                            }
                            int w = 0, h = 0;
                            decodePngFromStream(stream.Get(), m_bitmaps[newBitmap], w, h);

                            if (w > 0 && h > 0) {
                                std::lock_guard<std::mutex> lock(m_bitmapMutex);
                                m_bitmapWidth = w;
                                m_bitmapHeight = h;
                                m_currentBitmap = newBitmap;
                                m_needsDisplay = true;
                            }
                        }
                        m_inRendering = false;
                        return S_OK;
                    }).Get());
            break;
        }
        case WM_WEBVIEW_MOUSEEVENT: {
            auto* data = reinterpret_cast<MouseEventData*>(msg.lParam);
            if (data && m_compositionController) {
                int dpr = m_devicePixelRatio.load();
                if (dpr < 1) dpr = 1;
                int mx = data->x / dpr;
                int wy = (m_height.load() - data->y) / dpr;
                POINT point = {mx, wy};
                COREWEBVIEW2_MOUSE_EVENT_KIND kind;
                COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS vkeys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE;
                UINT32 mouseData = 0;

                switch (data->mouseState) {
                case 1: // mouse down
                    kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN;
                    vkeys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
                    break;
                case 2: // mouse drag
                    kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
                    vkeys = COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_LEFT_BUTTON;
                    break;
                case 3: // mouse up
                    kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP;
                    break;
                default: // mouse move (no button)
                    kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE;
                    break;
                }
                m_compositionController->SendMouseInput(kind, vkeys, mouseData, point);

                if (data->deltaY != 0.0f) {
                    POINT wheelPoint = {mx, wy};
                    mouseData = static_cast<UINT32>(static_cast<int>(data->deltaY * WHEEL_DELTA));
                    m_compositionController->SendMouseInput(
                        COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL,
                        COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE,
                        mouseData, wheelPoint);
                }
            }
            delete data;
            break;
        }
        case WM_WEBVIEW_CLEARCACHE: {
            if (!m_webview) break;
            bool includeDisk = static_cast<bool>(msg.wParam);
            ComPtr<ICoreWebView2_13> webview13;
            if (SUCCEEDED(m_webview.As(&webview13)) && webview13) {
                ComPtr<ICoreWebView2Profile> profile;
                if (SUCCEEDED(webview13->get_Profile(&profile)) && profile) {
                    ComPtr<ICoreWebView2Profile2> profile2;
                    if (SUCCEEDED(profile.As(&profile2)) && profile2) {
                        COREWEBVIEW2_BROWSING_DATA_KINDS kinds = includeDisk
                            ? COREWEBVIEW2_BROWSING_DATA_KINDS_ALL_SITE
                            : COREWEBVIEW2_BROWSING_DATA_KINDS_CACHE_STORAGE;
                        profile2->ClearBrowsingData(
                            kinds,
                            Callback<ICoreWebView2ClearBrowsingDataCompletedHandler>(
                                [](HRESULT result) -> HRESULT {
                                    return S_OK;
                                }).Get());
                    }
                }
            }
            break;
        }
        case WM_WEBVIEW_SETSCROLLBARSVISIBILITY: {
            bool visible = static_cast<bool>(msg.wParam);
            m_scrollbarsVisible = visible;
            if (!m_webview) break;
            if (visible) {
                m_webview->ExecuteScript(
                    L"(function() {"
                    L"  var s = document.getElementById('__wv_no_scrollbar');"
                    L"  if (s) s.remove();"
                    L"})()",
                    nullptr);
            } else {
                auto script = getScrollbarHideScript();
                m_webview->ExecuteScript(script.c_str(), nullptr);
            }
            break;
        }
        case WM_WEBVIEW_PAUSE: {
            if (!m_webview) break;
            ComPtr<ICoreWebView2_3> webview3;
            if (SUCCEEDED(m_webview.As(&webview3)) && webview3) {
                webview3->TrySuspend(
                    Callback<ICoreWebView2TrySuspendCompletedHandler>(
                        [](HRESULT errorCode, BOOL isSuccessful) -> HRESULT {
                            return S_OK;
                        }).Get());
            }
            break;
        }
        case WM_WEBVIEW_RESUME: {
            if (!m_webview) break;
            ComPtr<ICoreWebView2_3> webview3;
            if (SUCCEEDED(m_webview.As(&webview3)) && webview3) {
                webview3->Resume();
            }
            break;
        }
        case WM_WEBVIEW_GETCOOKIES: {
            auto* data = reinterpret_cast<CookieOpData*>(msg.lParam);
            if (data && m_cookieManager) {
                m_cookieManager->GetCookies(
                    data->url.c_str(),
                    Callback<ICoreWebView2GetCookiesCompletedHandler>(
                        [this](HRESULT result, ICoreWebView2CookieList* cookieList) -> HRESULT {
                            if (FAILED(result) || !cookieList) return S_OK;
                            UINT count = 0;
                            cookieList->get_Count(&count);
                            std::string cookieStr;
                            for (UINT i = 0; i < count; i++) {
                                ComPtr<ICoreWebView2Cookie> cookie;
                                cookieList->GetValueAtIndex(i, &cookie);
                                if (!cookie) continue;
                                LPWSTR name = nullptr, value = nullptr, domain = nullptr, path = nullptr;
                                cookie->get_Name(&name);
                                cookie->get_Value(&value);
                                cookie->get_Domain(&domain);
                                cookie->get_Path(&path);
                                if (name && value) {
                                    cookieStr += WideToUtf8(name) + "=" + WideToUtf8(value);
                                    if (domain) cookieStr += "; Domain=" + WideToUtf8(domain);
                                    if (path) cookieStr += "; Path=" + WideToUtf8(path);
                                    cookieStr += "; Version=0\n";
                                }
                                CoTaskMemFree(name);
                                CoTaskMemFree(value);
                                CoTaskMemFree(domain);
                                CoTaskMemFree(path);
                            }
                            addMessage("CallOnCookies:" + cookieStr);
                            return S_OK;
                        }).Get());
            }
            delete data;
            break;
        }
        case WM_WEBVIEW_CLEARCOOKIE: {
            auto* data = reinterpret_cast<CookieOpData*>(msg.lParam);
            if (data && m_cookieManager) {
                m_cookieManager->GetCookies(
                    data->url.c_str(),
                    Callback<ICoreWebView2GetCookiesCompletedHandler>(
                        [this, wname = data->name](HRESULT result, ICoreWebView2CookieList* cookieList) -> HRESULT {
                            if (FAILED(result) || !cookieList) return S_OK;
                            UINT count = 0;
                            cookieList->get_Count(&count);
                            for (UINT i = 0; i < count; i++) {
                                ComPtr<ICoreWebView2Cookie> cookie;
                                cookieList->GetValueAtIndex(i, &cookie);
                                if (!cookie) continue;
                                LPWSTR cookieName = nullptr;
                                cookie->get_Name(&cookieName);
                                if (cookieName && wname == cookieName) {
                                    m_cookieManager->DeleteCookie(cookie.Get());
                                }
                                CoTaskMemFree(cookieName);
                            }
                            return S_OK;
                        }).Get());
            }
            delete data;
            break;
        }
        case WM_WEBVIEW_CLEARALLCOOKIES: {
            if (m_cookieManager) {
                m_cookieManager->DeleteAllCookies();
            }
            break;
        }
        }
    }

    void initWebView2() {
        std::wstring userDataPath = GetUserDataPath();

        HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, userDataPath.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(result) || !env) {
                        addMessage("CallOnError:Failed to create WebView2 environment");
                        return S_OK;
                    }
                    m_environment = env;

                    if (!m_separated) {
                        // Offscreen: try CompositionController for native input
                        ComPtr<ICoreWebView2Environment3> env3;
                        if (SUCCEEDED(env->QueryInterface(IID_PPV_ARGS(&env3))) && env3) {
                            env3->CreateCoreWebView2CompositionController(
                                m_hwnd,
                                Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                                    [this](HRESULT result, ICoreWebView2CompositionController* compositionController) -> HRESULT {
                                        if (FAILED(result) || !compositionController) {
                                            // Fallback to regular controller
                                            createRegularController();
                                            return S_OK;
                                        }
                                        onCompositionControllerCreated(compositionController);
                                        return S_OK;
                                    }).Get());
                            return S_OK;
                        }
                    }

                    // Separated mode or env3 QI failed: use regular controller
                    createRegularController();
                    return S_OK;
                }).Get());

        if (FAILED(hr)) {
            addMessage("CallOnError:WebView2 runtime not found");
        }
    }

    void createRegularController() {
        m_environment->CreateCoreWebView2Controller(
            m_hwnd,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                    if (FAILED(result) || !controller) {
                        addMessage("CallOnError:Failed to create WebView2 controller");
                        return S_OK;
                    }
                    onWebView2Created(controller);
                    return S_OK;
                }).Get());
    }

    void onCompositionControllerCreated(ICoreWebView2CompositionController* compositionController) {
        m_compositionController = compositionController;

        // The composition controller implements ICoreWebView2Controller
        ComPtr<ICoreWebView2Controller> controller;
        HRESULT hr = compositionController->QueryInterface(IID_PPV_ARGS(&controller));
        if (FAILED(hr) || !controller) {
            // Cannot use composition path, fall back
            m_compositionController = nullptr;
            createRegularController();
            return;
        }

        if (!initDirectComposition()) {
            // DComp setup failed, fall back to regular controller
            m_compositionController = nullptr;
            m_dcompVisual = nullptr;
            m_dcompTarget = nullptr;
            m_dcompDevice = nullptr;
            createRegularController();
            return;
        }

        onWebView2Created(controller.Get());
    }

    bool initDirectComposition() {
        HRESULT hr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&m_dcompDevice));
        if (FAILED(hr)) return false;

        hr = m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, &m_dcompTarget);
        if (FAILED(hr)) return false;

        hr = m_dcompDevice->CreateVisual(&m_dcompVisual);
        if (FAILED(hr)) return false;

        m_dcompTarget->SetRoot(m_dcompVisual.Get());
        m_compositionController->put_RootVisualTarget(m_dcompVisual.Get());
        m_dcompDevice->Commit();
        return true;
    }

    void onWebView2Created(ICoreWebView2Controller* controller) {
        m_controller = controller;
        controller->get_CoreWebView2(&m_webview);
        if (!m_webview) {
            addMessage("CallOnError:Failed to get CoreWebView2");
            return;
        }

        // Obtain cookie manager from ICoreWebView2_2
        ComPtr<ICoreWebView2_2> webview2;
        if (SUCCEEDED(m_webview.As(&webview2)) && webview2) {
            webview2->get_CookieManager(&m_cookieManager);
        }

        RECT rc;
        GetClientRect(m_hwnd, &rc);
        controller->put_Bounds(rc);

        // Disable WebView2's own DPI scaling — we manage scaling via HWND resize
        ComPtr<ICoreWebView2Controller3> controller3;
        if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller3))) && controller3) {
            controller3->put_RasterizationScale(1.0);
            controller3->put_ShouldDetectMonitorScaleChanges(FALSE);
        }

        // Set transparent background if requested
        if (m_transparent) {
            ComPtr<ICoreWebView2Controller2> controller2;
            if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&controller2))) && controller2) {
                COREWEBVIEW2_COLOR bgColor = {0, 0, 0, 0};
                controller2->put_DefaultBackgroundColor(bgColor);
            }
        }

        ComPtr<ICoreWebView2Settings> settings;
        m_webview->get_Settings(&settings);
        if (settings) {
            settings->put_IsScriptEnabled(TRUE);
            settings->put_IsWebMessageEnabled(TRUE);
            settings->put_AreDevToolsEnabled(TRUE);
            settings->put_IsZoomControlEnabled(m_zoom ? TRUE : FALSE);
            if (!m_separated) {
                settings->put_AreDefaultContextMenusEnabled(FALSE);
            }
        }

        if (!m_userAgent.empty()) {
            ComPtr<ICoreWebView2Settings2> settings2;
            if (SUCCEEDED(settings.As(&settings2)) && settings2) {
                std::wstring wua = Utf8ToWide(m_userAgent.c_str());
                settings2->put_UserAgent(wua.c_str());
            }
        }

        // Inject Unity.call JS bridge and scrollbar hiding for offscreen mode
        std::wstring bridgeScript =
            L"window.Unity = { call: function(msg) { window.chrome.webview.postMessage(msg); } };";
        if (!m_separated && !m_scrollbarsVisible) {
            bridgeScript += L"\n" + getScrollbarHideScript();
        }
        m_webview->AddScriptToExecuteOnDocumentCreated(
            bridgeScript.c_str(),
            Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                [](HRESULT errorCode, LPCWSTR id) -> HRESULT {
                    return S_OK;
                }).Get());

        // WebMessageReceived handler
        EventRegistrationToken token;
        m_webview->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    LPWSTR messageRaw = nullptr;
                    HRESULT hr = args->TryGetWebMessageAsString(&messageRaw);
                    if (SUCCEEDED(hr) && messageRaw) {
                        std::string msg = WideToUtf8(messageRaw);
                        addMessage("CallFromJS:" + msg);
                        CoTaskMemFree(messageRaw);
                    }
                    return S_OK;
                }).Get(), &token);

        // NavigationStarting handler
        m_webview->add_NavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                    LPWSTR uriRaw = nullptr;
                    args->get_Uri(&uriRaw);
                    if (!uriRaw) return S_OK;

                    std::wstring wurl(uriRaw);
                    std::string url = WideToUtf8(uriRaw);
                    CoTaskMemFree(uriRaw);

                    m_progress.store(10);

                    // Check "unity:" scheme
                    if (wurl.find(L"unity:") == 0) {
                        std::string rest = url.substr(6);
                        addMessage("CallFromJS:" + rest);
                        args->put_Cancel(TRUE);
                        return S_OK;
                    }

                    std::lock_guard<std::mutex> lock(m_patternMutex);

                    // Check hook pattern
                    if (m_hookRegex) {
                        try {
                            if (std::regex_search(wurl, *m_hookRegex)) {
                                addMessage("CallOnHooked:" + url);
                                args->put_Cancel(TRUE);
                                return S_OK;
                            }
                        } catch (...) {}
                    }

                    // Check allow/deny patterns
                    bool pass = true;
                    if (m_denyRegex) {
                        try {
                            if (std::regex_search(wurl, *m_denyRegex)) {
                                // Deny matched, check if allow overrides
                                if (m_allowRegex) {
                                    if (!std::regex_search(wurl, *m_allowRegex)) {
                                        pass = false;
                                    }
                                } else {
                                    pass = false;
                                }
                            }
                        } catch (...) {}
                    }

                    if (!pass) {
                        args->put_Cancel(TRUE);
                        return S_OK;
                    }

                    addMessage("CallOnStarted:" + url);
                    return S_OK;
                }).Get(), &token);

        // NavigationCompleted handler
        m_webview->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                    BOOL isSuccess = FALSE;
                    args->get_IsSuccess(&isSuccess);
                    m_progress.store(100);

                    // Update navigation state
                    BOOL canGoBack = FALSE, canGoForward = FALSE;
                    sender->get_CanGoBack(&canGoBack);
                    sender->get_CanGoForward(&canGoForward);
                    m_canGoBack.store(canGoBack != FALSE);
                    m_canGoForward.store(canGoForward != FALSE);

                    LPWSTR uriRaw = nullptr;
                    sender->get_Source(&uriRaw);
                    std::string url = uriRaw ? WideToUtf8(uriRaw) : "";
                    if (uriRaw) CoTaskMemFree(uriRaw);

                    if (isSuccess) {
                        addMessage("CallOnLoaded:" + url);
                        // Re-inject scrollbar hiding CSS after navigation
                        if (!m_separated && !m_scrollbarsVisible.load()) {
                            auto script = getScrollbarHideScript();
                            sender->ExecuteScript(script.c_str(), nullptr);
                        }
                    } else {
                        COREWEBVIEW2_WEB_ERROR_STATUS status;
                        args->get_WebErrorStatus(&status);
                        addMessage("CallOnError:" + url + " (error: " + std::to_string(static_cast<int>(status)) + ")");
                    }
                    return S_OK;
                }).Get(), &token);

        // Handle target="_blank" links by navigating in the same webview
        m_webview->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                    args->put_Handled(TRUE);
                    LPWSTR uri = nullptr;
                    args->get_Uri(&uri);
                    if (uri) {
                        sender->Navigate(uri);
                        CoTaskMemFree(uri);
                    }
                    return S_OK;
                }).Get(), &token);

        // Apply custom headers to all requests
        m_webview->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        m_webview->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                    ComPtr<ICoreWebView2WebResourceRequest> request;
                    args->get_Request(&request);
                    if (!request) return S_OK;

                    ComPtr<ICoreWebView2HttpRequestHeaders> headers;
                    request->get_Headers(&headers);
                    if (!headers) return S_OK;

                    std::lock_guard<std::mutex> lock(m_headerMutex);
                    for (const auto& pair : m_customHeaders) {
                        std::wstring key = Utf8ToWide(pair.first.c_str());
                        std::wstring value = Utf8ToWide(pair.second.c_str());
                        headers->SetHeader(key.c_str(), value.c_str());
                    }
                    return S_OK;
                }).Get(), &token);

        // WebResourceResponseReceived handler for HTTP error codes
        ComPtr<ICoreWebView2_2> webview2ForResponse;
        if (SUCCEEDED(m_webview.As(&webview2ForResponse)) && webview2ForResponse) {
            webview2ForResponse->add_WebResourceResponseReceived(
                Callback<ICoreWebView2WebResourceResponseReceivedEventHandler>(
                    [this](ICoreWebView2* sender, ICoreWebView2WebResourceResponseReceivedEventArgs* args) -> HRESULT {
                        ComPtr<ICoreWebView2WebResourceResponseView> response;
                        args->get_Response(&response);
                        if (!response) return S_OK;

                        int statusCode = 0;
                        response->get_StatusCode(&statusCode);
                        if (statusCode >= 400) {
                            addMessage("CallOnHttpError:" + std::to_string(statusCode));
                        }
                        return S_OK;
                    }).Get(), &token);
        }

        // BasicAuthenticationRequested handler
        ComPtr<ICoreWebView2_10> webview10;
        if (SUCCEEDED(m_webview.As(&webview10)) && webview10) {
            webview10->add_BasicAuthenticationRequested(
                Callback<ICoreWebView2BasicAuthenticationRequestedEventHandler>(
                    [this](ICoreWebView2* sender, ICoreWebView2BasicAuthenticationRequestedEventArgs* args) -> HRESULT {
                        std::lock_guard<std::mutex> lock(m_authMutex);
                        if (m_basicAuthUser.empty() && m_basicAuthPass.empty()) {
                            return S_OK;
                        }
                        ComPtr<ICoreWebView2BasicAuthenticationResponse> response;
                        args->get_Response(&response);
                        if (response) {
                            response->put_UserName(Utf8ToWide(m_basicAuthUser.c_str()).c_str());
                            response->put_Password(Utf8ToWide(m_basicAuthPass.c_str()).c_str());
                        }
                        return S_OK;
                    }).Get(), &token);
        }

        // ScriptDialogOpening handler — suppress dialogs when disabled
        m_webview->add_ScriptDialogOpening(
            Callback<ICoreWebView2ScriptDialogOpeningEventHandler>(
                [this](ICoreWebView2* sender, ICoreWebView2ScriptDialogOpeningEventArgs* args) -> HRESULT {
                    if (!m_alertDialogEnabled.load()) {
                        args->Accept();
                    }
                    return S_OK;
                }).Get(), &token);

        m_initialized.store(true);

        if (!m_separated) {
            initWGC();
        }

        // Navigate to any URL that was requested before WebView2 was ready
        if (!m_pendingUrl.empty()) {
            auto* copy = _strdup(m_pendingUrl.c_str());
            m_pendingUrl.clear();
            PostThreadMessageW(GetCurrentThreadId(), WM_WEBVIEW_LOADURL, 0, reinterpret_cast<LPARAM>(copy));
        }
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        auto* self = reinterpret_cast<WebViewInstance*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg) {
        case WM_SIZE:
            if (self && self->m_controller) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                self->m_controller->put_Bounds(rc);
            }
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            // Forward scroll to WebView2's direct child window
            if (self) {
                HWND child = GetWindow(hwnd, GW_CHILD);
                if (child) {
                    return SendMessageW(child, msg, wParam, lParam);
                }
            }
            break;
        case WM_CLOSE:
            // User closed the separated window — trigger clean shutdown
            // instead of letting DefWindowProcW destroy the HWND prematurely
            if (self) {
                PostThreadMessageW(self->m_threadId, WM_WEBVIEW_DESTROY, 0, 0);
            }
            return 0;
        case WM_DESTROY:
            if (self) {
                self->m_hwnd = nullptr;
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
};

extern "C" {

EXPORT void _CWebViewPlugin_InitStatic(bool inEditor, bool useMetal) {
    s_inEditor = inEditor;
}

EXPORT bool _CWebViewPlugin_IsInitialized(void* instance) {
    if (!instance) return false;
    return static_cast<WebViewInstance*>(instance)->isInitialized();
}

EXPORT void* _CWebViewPlugin_Init(
    const char* gameObject, bool transparent, bool zoom,
    int width, int height, const char* ua, bool separated) {
    auto* instance = new WebViewInstance(gameObject, transparent, zoom, width, height, ua, separated);
    {
        std::lock_guard<std::mutex> lock(s_instancesMutex);
        s_instances.push_back(instance);
    }
    return instance;
}

EXPORT void _CWebViewPlugin_Destroy(void* instance) {
    if (!instance) return;
    auto* inst = static_cast<WebViewInstance*>(instance);
    {
        std::lock_guard<std::mutex> lock(s_instancesMutex);
        auto it = std::find(s_instances.begin(), s_instances.end(), inst);
        if (it != s_instances.end()) s_instances.erase(it);
    }
    delete inst;
}

EXPORT void _CWebViewPlugin_SetRect(void* instance, int width, int height) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->setRect(width, height);
}

EXPORT void _CWebViewPlugin_SetVisibility(void* instance, bool visibility) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->setVisibility(visibility);
}

EXPORT bool _CWebViewPlugin_SetURLPattern(
    void* instance, const char* allowPattern,
    const char* denyPattern, const char* hookPattern) {
    if (!instance) return false;
    return static_cast<WebViewInstance*>(instance)->setURLPattern(allowPattern, denyPattern, hookPattern);
}

EXPORT void _CWebViewPlugin_LoadURL(void* instance, const char* url) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->loadURL(url);
}

EXPORT void _CWebViewPlugin_LoadHTML(void* instance, const char* html, const char* baseUrl) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->loadHTML(html, baseUrl);
}

EXPORT void _CWebViewPlugin_EvaluateJS(void* instance, const char* js) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->evaluateJS(js);
}

EXPORT int _CWebViewPlugin_Progress(void* instance) {
    if (!instance) return 0;
    return static_cast<WebViewInstance*>(instance)->progress();
}

EXPORT bool _CWebViewPlugin_CanGoBack(void* instance) {
    if (!instance) return false;
    return static_cast<WebViewInstance*>(instance)->canGoBack();
}

EXPORT bool _CWebViewPlugin_CanGoForward(void* instance) {
    if (!instance) return false;
    return static_cast<WebViewInstance*>(instance)->canGoForward();
}

EXPORT void _CWebViewPlugin_GoBack(void* instance) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->goBack();
}

EXPORT void _CWebViewPlugin_GoForward(void* instance) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->goForward();
}

EXPORT void _CWebViewPlugin_Reload(void* instance) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->reload();
}

EXPORT void _CWebViewPlugin_SendMouseEvent(
    void* instance, int x, int y, float deltaY, int mouseState) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->sendMouseEvent(x, y, deltaY, mouseState);
}

EXPORT void _CWebViewPlugin_SendKeyEvent(
    void* instance, int x, int y,
    const wchar_t* keyChars, unsigned short keyCode, int keyState) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->sendKeyEvent(x, y, keyChars, keyCode, keyState);
}

EXPORT void _CWebViewPlugin_Update(void* instance, bool refreshBitmap, int devicePixelRatio) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->update(refreshBitmap, devicePixelRatio);
}

EXPORT int _CWebViewPlugin_BitmapWidth(void* instance) {
    if (!instance) return 0;
    return static_cast<WebViewInstance*>(instance)->bitmapWidth();
}

EXPORT int _CWebViewPlugin_BitmapHeight(void* instance) {
    if (!instance) return 0;
    return static_cast<WebViewInstance*>(instance)->bitmapHeight();
}

EXPORT void _CWebViewPlugin_Render(void* instance, void* textureBuffer) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->render(textureBuffer);
}

EXPORT void _CWebViewPlugin_AddCustomHeader(
    void* instance, const char* headerKey, const char* headerValue) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->addCustomHeader(headerKey, headerValue);
}

EXPORT void _CWebViewPlugin_RemoveCustomHeader(void* instance, const char* headerKey) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->removeCustomHeader(headerKey);
}

EXPORT const char* _CWebViewPlugin_GetCustomHeaderValue(
    void* instance, const char* headerKey) {
    if (!instance) return nullptr;
    return static_cast<WebViewInstance*>(instance)->getCustomHeaderValue(headerKey);
}

EXPORT void _CWebViewPlugin_ClearCustomHeader(void* instance) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->clearCustomHeader();
}

EXPORT void _CWebViewPlugin_ClearCookie(const char* url, const char* name) {
    std::lock_guard<std::mutex> lock(s_instancesMutex);
    for (auto* inst : s_instances) {
        if (inst && inst->hasCookieManager()) {
            inst->clearCookie(url, name);
            break;
        }
    }
}

EXPORT void _CWebViewPlugin_ClearCookies() {
    std::lock_guard<std::mutex> lock(s_instancesMutex);
    for (auto* inst : s_instances) {
        if (inst && inst->hasCookieManager()) {
            inst->clearAllCookies();
            break;
        }
    }
}

EXPORT void _CWebViewPlugin_SaveCookies() {
    // WebView2 auto-persists cookies, nothing to do
}

EXPORT void _CWebViewPlugin_GetCookies(void* instance, const char* url) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->getCookies(url);
}

EXPORT const char* _CWebViewPlugin_GetMessage(void* instance) {
    if (!instance) return nullptr;
    return static_cast<WebViewInstance*>(instance)->getMessage();
}

EXPORT void _CWebViewPlugin_SetBasicAuthInfo(void* instance, const char* userName, const char* password) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->setBasicAuthInfo(userName, password);
}

EXPORT void _CWebViewPlugin_ClearCache(void* instance, bool includeDiskFiles) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->clearCache(includeDiskFiles);
}

EXPORT void _CWebViewPlugin_SetInteractionEnabled(void* instance, bool enabled) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->setInteractionEnabled(enabled);
}

EXPORT void _CWebViewPlugin_SetScrollbarsVisibility(void* instance, bool visibility) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->setScrollbarsVisibility(visibility);
}

EXPORT void _CWebViewPlugin_SetAlertDialogEnabled(void* instance, bool enabled) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->setAlertDialogEnabled(enabled);
}

EXPORT void _CWebViewPlugin_Pause(void* instance) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->pause();
}

EXPORT void _CWebViewPlugin_Resume(void* instance) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->resume();
}

} // extern "C"
