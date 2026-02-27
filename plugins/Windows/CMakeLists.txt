cmake_minimum_required(VERSION 3.16)
project(WebViewPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# WebView2 SDK paths
set(WEBVIEW2_DIR "${CMAKE_SOURCE_DIR}/packages/Microsoft.Web.WebView2/build/native")

add_library(WebViewPlugin SHARED src/WebViewPlugin.cpp)

target_include_directories(WebViewPlugin PRIVATE
    "${WEBVIEW2_DIR}/include"
)

target_link_libraries(WebViewPlugin PRIVATE
    "${WEBVIEW2_DIR}/x64/WebView2LoaderStatic.lib"
    d3d11
    dxgi
    dcomp
    shlwapi
    version
    windowscodecs
    windowsapp
)

target_compile_options(WebViewPlugin PRIVATE /bigobj)

target_compile_definitions(WebViewPlugin PRIVATE
    UNICODE
    _UNICODE
    WEBVIEWPLUGIN_EXPORTS
)

set_target_properties(WebViewPlugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}"
)
