#include <windows.h>
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <vector>
#include <map>
#include <regex>
#include <wrl.h>
#include <WebView2.h>

using Microsoft::WRL::ComPtr;

#define EXPORT __declspec(dllexport)

// Forward declaration
class WebViewInstance;

// Global state
static bool s_inEditor = false;
static std::vector<WebViewInstance*> s_instances;

class WebViewInstance {
public:
    bool isInitialized() { return false; }
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
    return nullptr;
}

EXPORT void _CWebViewPlugin_Destroy(void* instance) {}
EXPORT void _CWebViewPlugin_SetRect(void* instance, int width, int height) {}
EXPORT void _CWebViewPlugin_SetVisibility(void* instance, bool visibility) {}

EXPORT bool _CWebViewPlugin_SetURLPattern(
    void* instance, const char* allowPattern,
    const char* denyPattern, const char* hookPattern) {
    return false;
}

EXPORT void _CWebViewPlugin_LoadURL(void* instance, const char* url) {}
EXPORT void _CWebViewPlugin_LoadHTML(void* instance, const char* html, const char* baseUrl) {}
EXPORT void _CWebViewPlugin_EvaluateJS(void* instance, const char* js) {}
EXPORT int  _CWebViewPlugin_Progress(void* instance) { return 0; }
EXPORT bool _CWebViewPlugin_CanGoBack(void* instance) { return false; }
EXPORT bool _CWebViewPlugin_CanGoForward(void* instance) { return false; }
EXPORT void _CWebViewPlugin_GoBack(void* instance) {}
EXPORT void _CWebViewPlugin_GoForward(void* instance) {}
EXPORT void _CWebViewPlugin_Reload(void* instance) {}

EXPORT void _CWebViewPlugin_SendMouseEvent(
    void* instance, int x, int y, float deltaY, int mouseState) {}

EXPORT void _CWebViewPlugin_SendKeyEvent(
    void* instance, int x, int y,
    char* keyChars, unsigned short keyCode, int keyState) {}

EXPORT void _CWebViewPlugin_Update(void* instance, bool refreshBitmap, int devicePixelRatio) {}
EXPORT int  _CWebViewPlugin_BitmapWidth(void* instance) { return 0; }
EXPORT int  _CWebViewPlugin_BitmapHeight(void* instance) { return 0; }
EXPORT void _CWebViewPlugin_Render(void* instance, void* textureBuffer) {}

EXPORT void _CWebViewPlugin_AddCustomHeader(
    void* instance, const char* headerKey, const char* headerValue) {}

EXPORT void _CWebViewPlugin_RemoveCustomHeader(void* instance, const char* headerKey) {}

EXPORT const char* _CWebViewPlugin_GetCustomHeaderValue(
    void* instance, const char* headerKey) {
    return nullptr;
}

EXPORT void _CWebViewPlugin_ClearCustomHeader(void* instance) {}
EXPORT void _CWebViewPlugin_ClearCookie(const char* url, const char* name) {}
EXPORT void _CWebViewPlugin_ClearCookies() {}
EXPORT void _CWebViewPlugin_SaveCookies() {}
EXPORT void _CWebViewPlugin_GetCookies(void* instance, const char* url) {}

EXPORT const char* _CWebViewPlugin_GetMessage(void* instance) {
    return nullptr;
}

} // extern "C"
