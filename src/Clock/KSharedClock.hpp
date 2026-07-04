#pragma once

#include <windows.h>

namespace KSharedClock {
    ULONGLONG Interrupt100ns();
    float InterruptSeconds();

    ULONGLONG SystemFileTime100ns();
    LONGLONG SystemUnixSeconds();
    LONGLONG SecondsUntilUnix(LONGLONG unixSeconds);
}