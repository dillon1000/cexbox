cmake_minimum_required(VERSION 3.20)

# Configure with -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 so the
# DLL is built as a UWP component and uses the app version of the C++ runtime.
project(xbloader LANGUAGES CXX)

add_library(xbloader SHARED
    src/console.cpp
    src/host.cpp
    src/loader.cpp
    src/net.cpp
    src/shims.cpp
    src/stubs.cpp
    src/tls.cpp
)

target_include_directories(xbloader PRIVATE include)
target_compile_features(xbloader PRIVATE cxx_std_17)
target_compile_definitions(xbloader PRIVATE
    XBLOADER_EXPORTS
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000006
)
target_compile_options(xbloader PRIVATE /W4 /permissive- /EHsc /GS)
set_property(TARGET xbloader PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

# Only the functions Xbox apps may import. A call to anything else fails to link.
target_link_options(xbloader PRIVATE /NODEFAULTLIB:kernel32.lib /NODEFAULTLIB:user32.lib /NODEFAULTLIB:advapi32.lib /NODEFAULTLIB:ole32.lib)
target_link_libraries(xbloader PRIVATE WindowsApp.lib ws2_32.lib)
