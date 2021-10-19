#pragma once
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

#define VERSION_MAJOR 5
#define VERSION_MINOR 4
#define VERSION_PATCH 2

#define VERSION_DISPLAY STR(VERSION_MAJOR) "."  STR(VERSION_MINOR) "." STR(VERSION_PATCH)

namespace Constants {
    static const char* const DisplayVersion = "v" VERSION_DISPLAY;
    static const char* const ModDir = "\\ManualTransmission";
    static const char* const NotificationPrefix =  "~b~Manual Transmission~w~";
}
