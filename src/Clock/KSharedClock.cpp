#include "Global.hpp"

#include "KSharedClock.hpp"

// ---------------------------------------------------------------------------
// KUSER_SHARED_DATA � the kernel maps this read-only page into every process
// at 0x7FFE0000.  InterruptTime is a 64-bit counter in 100-nanosecond units
// (10,000,000 ticks per second).  We define only the fields we need; no
// winternl.h or ntddk.h required, and zero import-table entries are added.
//
// InterruptTime = monotonic 100ns ticks. Use it for notification lifetime/fade.
// SystemTime    = UTC FILETIME 100ns ticks. Use it for exact reset deadlines.
// ---------------------------------------------------------------------------
namespace
{
    // Mirrors _KSYSTEM_TIME from WinDBG public symbols.
    struct KsTime
    {
        ULONG LowPart;    // +0x000
        LONG  High1Time;  // +0x004
        LONG  High2Time;  // +0x008
    };

    // First fields of KUSER_SHARED_DATA:
    // TickCountLowDeprecated@0, TickCountMultiplier@4,
    // InterruptTime@8, SystemTime@0x14.
    struct KuserShared
    {
        ULONG  TickCountLowDeprecated;  // +0x000
        ULONG  TickCountMultiplier;     // +0x004
        KsTime InterruptTime;           // +0x008
        KsTime SystemTime;              // +0x014
    };

    // 0x7FFE0000 is the fixed user-space VA of KUSER_SHARED_DATA on all
    // supported Windows releases (x86, x64, ARM64).
    static volatile KuserShared* const kSharedData =
        reinterpret_cast<volatile KuserShared*>(
            static_cast<ULONG_PTR>(0x7FFE0000u)
            );

    // Tear-safe 64-bit read of InterruptTime/SystemTime using the High1/Low/High2
    // spinlock exactly as NTDLL does internally.
    static ULONGLONG ReadKsTime100ns(volatile KsTime* t)
    {
        LONG h1, h2;
        ULONG lo;

        do {
            h1 = t->High1Time;
            lo = t->LowPart;
            h2 = t->High2Time;
        } while (h1 != h2);

        return (static_cast<ULONGLONG>(static_cast<ULONG>(h1)) << 32) | static_cast<ULONGLONG>(lo);
    }
}

namespace KSharedClock
{
    ULONGLONG Interrupt100ns()
    {
        return ReadKsTime100ns(&kSharedData->InterruptTime);
    }

    float InterruptSeconds()
    {
        return static_cast<float>(Interrupt100ns()) / 10000000.0f;
    }

    ULONGLONG SystemFileTime100ns()
    {
        return ReadKsTime100ns(&kSharedData->SystemTime);
    }

    LONGLONG SystemUnixSeconds()
    {
        constexpr ULONGLONG kUnixToFileTime100ns = 116444736000000000ULL;

        ULONGLONG fileTime100ns = SystemFileTime100ns();

        if (fileTime100ns < kUnixToFileTime100ns) {
            return 0;
        }

        return static_cast<LONGLONG>((fileTime100ns - kUnixToFileTime100ns) / 10000000ULL);
    }

    LONGLONG SecondsUntilUnix(LONGLONG unixSeconds)
    {
        return unixSeconds - SystemUnixSeconds();
    }
}
