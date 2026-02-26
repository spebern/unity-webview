#include <windows.h>
#include <shlwapi.h>
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <vector>
#include <map>
#include <atomic>
#include <regex>
#include <wrl.h>
#include <WebView2.h>

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
};

class WebViewInstance;

static bool s_inEditor = false;
static std::vector<WebViewInstance*> s_instances;

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

static std::wstring GetUserDataPath() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring path(tempPath);
    path += L"UnityWebView";
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

class WebViewInstance {
    std::thread m_thread;
    DWORD m_threadId = 0;
    HANDLE m_readyEvent = nullptr;

    ComPtr<ICoreWebView2Environment> m_environment;
    ComPtr<ICoreWebView2Controller> m_controller;
    ComPtr<ICoreWebView2> m_webview;

    HWND m_hwnd = nullptr;

    std::string m_gameObject;
    bool m_transparent = false;
    bool m_zoom = true;
    bool m_separated = false;
    int m_width = 0;
    int m_height = 0;
    bool m_visible = true;
    std::string m_userAgent;

    std::queue<std::string> m_messages;
    std::mutex m_messageMutex;

    std::atomic<bool> m_initialized{false};

    std::map<std::string, std::string> m_customHeaders;
    std::mutex m_headerMutex;

    std::wstring m_allowPattern;
    std::wstring m_denyPattern;
    std::wstring m_hookPattern;
    bool m_hasAllowPattern = false;
    bool m_hasDenyPattern = false;
    bool m_hasHookPattern = false;
    std::mutex m_patternMutex;

    // Bitmap placeholders for Task 6
    std::vector<uint8_t> m_bitmaps[2];
    int m_currentBitmap = 0;
    int m_bitmapWidth = 0;
    int m_bitmapHeight = 0;
    bool m_needsDisplay = false;
    bool m_inRendering = false;
    std::mutex m_bitmapMutex;

    // State for canGoBack/canGoForward
    std::atomic<bool> m_canGoBack{false};
    std::atomic<bool> m_canGoForward{false};

public:
    WebViewInstance(const char* gameObject, bool transparent, bool zoom,
                    int width, int height, const char* ua, bool separated)
        : m_gameObject(gameObject ? gameObject : "")
        , m_transparent(transparent)
        , m_zoom(zoom)
        , m_width(width > 0 ? width : 960)
        , m_height(height > 0 ? height : 600)
        , m_userAgent(ua ? ua : "")
        , m_separated(separated)
    {
        m_readyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_thread = std::thread(&WebViewInstance::threadProc, this);
        WaitForSingleObject(m_readyEvent, 10000);
    }

    ~WebViewInstance() {
        if (m_threadId != 0) {
            PostThreadMessageW(m_threadId, WM_WEBVIEW_DESTROY, 0, 0);
        }
        if (m_thread.joinable()) {
            m_thread.join();
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
        static thread_local std::string s_lastMessage;
        s_lastMessage = m_messages.front();
        m_messages.pop();
        return s_lastMessage.c_str();
    }

    void loadURL(const char* url) {
        if (!url) return;
        auto* copy = _strdup(url);
        PostThreadMessageW(m_threadId, WM_WEBVIEW_LOADURL, 0, reinterpret_cast<LPARAM>(copy));
    }

    void loadHTML(const char* html, const char* baseUrl) {
        if (!html) return;
        auto* copy = _strdup(html);
        PostThreadMessageW(m_threadId, WM_WEBVIEW_LOADHTML, 0, reinterpret_cast<LPARAM>(copy));
    }

    void evaluateJS(const char* js) {
        if (!js) return;
        auto* copy = _strdup(js);
        PostThreadMessageW(m_threadId, WM_WEBVIEW_EVALUATEJS, 0, reinterpret_cast<LPARAM>(copy));
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
                m_allowPattern = Utf8ToWide(allow);
                std::wregex test(m_allowPattern);
                m_hasAllowPattern = true;
            } else {
                m_hasAllowPattern = false;
                m_allowPattern.clear();
            }
            if (deny && *deny) {
                m_denyPattern = Utf8ToWide(deny);
                std::wregex test(m_denyPattern);
                m_hasDenyPattern = true;
            } else {
                m_hasDenyPattern = false;
                m_denyPattern.clear();
            }
            if (hook && *hook) {
                m_hookPattern = Utf8ToWide(hook);
                std::wregex test(m_hookPattern);
                m_hasHookPattern = true;
            } else {
                m_hasHookPattern = false;
                m_hookPattern.clear();
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    int progress() { return 0; }
    bool canGoBack() { return m_canGoBack.load(); }
    bool canGoForward() { return m_canGoForward.load(); }

    // Stubs for Task 7
    void sendMouseEvent(int x, int y, float deltaY, int mouseState) {}
    void sendKeyEvent(int x, int y, char* keyChars, unsigned short keyCode, int keyState) {}

    // Stubs for Task 6
    void update(bool refreshBitmap, int devicePixelRatio) {}
    int bitmapWidth() { return 0; }
    int bitmapHeight() { return 0; }
    void render(void* textureBuffer) {}

    // Stubs for Task 8
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
        static thread_local std::string s_headerValue;
        s_headerValue = it->second;
        return s_headerValue.c_str();
    }

    void clearCustomHeader() {
        std::lock_guard<std::mutex> lock(m_headerMutex);
        m_customHeaders.clear();
    }

    void getCookies(const char* url) {}

private:
    void threadProc() {
        m_threadId = GetCurrentThreadId();

        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) {
            SetEvent(m_readyEvent);
            return;
        }

        const wchar_t* className = L"WebViewPluginWindow";
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);

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

        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        if (m_separated) {
            ShowWindow(m_hwnd, SW_SHOW);
            UpdateWindow(m_hwnd);
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
            if (m_webview && url) {
                std::wstring wurl = Utf8ToWide(url);
                m_webview->Navigate(wurl.c_str());
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
                RECT rc = {0, 0, m_width, m_height};
                m_controller->put_Bounds(rc);
                if (m_separated) {
                    SetWindowPos(m_hwnd, nullptr, 0, 0, m_width, m_height,
                                 SWP_NOMOVE | SWP_NOZORDER);
                }
            }
            break;
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
                    env->CreateCoreWebView2Controller(
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
                    return S_OK;
                }).Get());

        if (FAILED(hr)) {
            addMessage("CallOnError:WebView2 runtime not found");
        }
    }

    void onWebView2Created(ICoreWebView2Controller* controller) {
        m_controller = controller;
        controller->get_CoreWebView2(&m_webview);
        if (!m_webview) {
            addMessage("CallOnError:Failed to get CoreWebView2");
            return;
        }

        RECT rc;
        GetClientRect(m_hwnd, &rc);
        controller->put_Bounds(rc);

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

        // Inject Unity.call JS bridge
        std::wstring bridgeScript = L"window.Unity = { call: function(msg) { window.chrome.webview.postMessage(msg); } };";
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

                    // Check "unity:" scheme
                    if (wurl.find(L"unity:") == 0) {
                        std::string rest = url.substr(6);
                        addMessage("CallFromJS:" + rest);
                        args->put_Cancel(TRUE);
                        return S_OK;
                    }

                    std::lock_guard<std::mutex> lock(m_patternMutex);

                    // Check hook pattern
                    if (m_hasHookPattern) {
                        try {
                            std::wregex hookRegex(m_hookPattern);
                            if (std::regex_search(wurl, hookRegex)) {
                                addMessage("CallOnHooked:" + url);
                                args->put_Cancel(TRUE);
                                return S_OK;
                            }
                        } catch (...) {}
                    }

                    // Check allow/deny patterns
                    bool pass = true;
                    if (m_hasAllowPattern) {
                        try {
                            std::wregex allowRegex(m_allowPattern);
                            if (std::regex_search(wurl, allowRegex)) {
                                pass = true;
                            }
                        } catch (...) {}
                    }
                    if (pass && m_hasDenyPattern) {
                        try {
                            std::wregex denyRegex(m_denyPattern);
                            if (std::regex_search(wurl, denyRegex)) {
                                // Deny matched, check if allow also matches
                                if (m_hasAllowPattern) {
                                    try {
                                        std::wregex allowRegex(m_allowPattern);
                                        if (!std::regex_search(wurl, allowRegex)) {
                                            pass = false;
                                        }
                                    } catch (...) {
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
                    } else {
                        COREWEBVIEW2_WEB_ERROR_STATUS status;
                        args->get_WebErrorStatus(&status);
                        addMessage("CallOnError:" + url + " (error: " + std::to_string(static_cast<int>(status)) + ")");
                    }
                    return S_OK;
                }).Get(), &token);

        m_initialized.store(true);
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<WebViewInstance*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg) {
        case WM_SIZE:
            if (self && self->m_controller) {
                RECT rc;
                GetClientRect(hwnd, &rc);
                self->m_controller->put_Bounds(rc);
            }
            return 0;
        case WM_DESTROY:
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
    s_instances.push_back(instance);
    return instance;
}

EXPORT void _CWebViewPlugin_Destroy(void* instance) {
    if (!instance) return;
    auto* inst = static_cast<WebViewInstance*>(instance);
    auto it = std::find(s_instances.begin(), s_instances.end(), inst);
    if (it != s_instances.end()) s_instances.erase(it);
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
    char* keyChars, unsigned short keyCode, int keyState) {
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

EXPORT void _CWebViewPlugin_ClearCookie(const char* url, const char* name) {}
EXPORT void _CWebViewPlugin_ClearCookies() {}
EXPORT void _CWebViewPlugin_SaveCookies() {}

EXPORT void _CWebViewPlugin_GetCookies(void* instance, const char* url) {
    if (!instance) return;
    static_cast<WebViewInstance*>(instance)->getCookies(url);
}

EXPORT const char* _CWebViewPlugin_GetMessage(void* instance) {
    if (!instance) return nullptr;
    return static_cast<WebViewInstance*>(instance)->getMessage();
}

} // extern "C"
