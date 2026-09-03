#include "Global.hpp"

#include "ClaudeCookieStore.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <tlhelp32.h>
#endif

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ClaudeCookieStore {
namespace {

    constexpr std::uint32_t kWalMagicLittleChecksum = 0x377F0682u;
    constexpr std::uint32_t kWalMagicBigChecksum = 0x377F0683u;
    constexpr std::size_t kDatabaseHeaderSize = 100;
    constexpr std::size_t kWalHeaderSize = 32;
    constexpr std::size_t kWalFrameHeaderSize = 24;
    constexpr std::uint64_t kMaximumPayloadBytes = 32ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t kMaximumDatabaseBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t kMaximumWalBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
    constexpr std::uint32_t kMaximumTraversalDepth = 64;
    constexpr int kMaximumConsistencyAttempts = 4;

    static std::filesystem::path SidecarPath(
        const std::filesystem::path& databasePath,
        const char* suffix
    ) {
        std::filesystem::path path = databasePath;
        path += suffix;
        return path;
    }

    static std::string PathText(const std::filesystem::path& path) {
#ifdef _WIN32
        const std::wstring wide = path.wstring();
        if (wide.empty()) {
            return {};
        }

        int needed = WideCharToMultiByte(
            CP_UTF8,
            0,
            wide.data(),
            static_cast<int>(wide.size()),
            nullptr,
            0,
            nullptr,
            nullptr
        );

        if (needed <= 0) {
            return path.string();
        }

        std::string output(static_cast<std::size_t>(needed), '\0');
        if (WideCharToMultiByte(
            CP_UTF8,
            0,
            wide.data(),
            static_cast<int>(wide.size()),
            output.data(),
            needed,
            nullptr,
            nullptr
        ) != needed) {
            return path.string();
        }

        return output;
#else
        return path.string();
#endif
    }

#ifdef _WIN32
    class UniqueHandle final {
    public:
        UniqueHandle() = default;
        explicit UniqueHandle(HANDLE handle) : m_handle(handle) {}

        ~UniqueHandle() {
            Reset();
        }

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept
            : m_handle(other.Release()) {
        }

        UniqueHandle& operator=(UniqueHandle&& other) noexcept {
            if (this != &other) {
                Reset(other.Release());
            }
            return *this;
        }

        bool Valid() const {
            return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
        }

        HANDLE Get() const {
            return m_handle;
        }

        HANDLE Release() {
            HANDLE handle = m_handle;
            m_handle = INVALID_HANDLE_VALUE;
            return handle;
        }

        void Reset(HANDLE handle = INVALID_HANDLE_VALUE) {
            if (Valid()) {
                CloseHandle(m_handle);
            }
            m_handle = handle;
        }

    private:
        HANDLE m_handle = INVALID_HANDLE_VALUE;
    };
#else
    class UniqueFd final {
    public:
        UniqueFd() = default;
        explicit UniqueFd(int fd) : m_fd(fd) {}

        ~UniqueFd() {
            Reset();
        }

        UniqueFd(const UniqueFd&) = delete;
        UniqueFd& operator=(const UniqueFd&) = delete;

        UniqueFd(UniqueFd&& other) noexcept
            : m_fd(other.Release()) {
        }

        UniqueFd& operator=(UniqueFd&& other) noexcept {
            if (this != &other) {
                Reset(other.Release());
            }
            return *this;
        }

        bool Valid() const {
            return m_fd >= 0;
        }

        int Get() const {
            return m_fd;
        }

        int Release() {
            int fd = m_fd;
            m_fd = -1;
            return fd;
        }

        void Reset(int fd = -1) {
            if (Valid()) {
                ::close(m_fd);
            }
            m_fd = fd;
        }

    private:
        int m_fd = -1;
    };
#endif

#ifdef _WIN32
    // -----------------------------------------------------------------------
    // Locked-file fallback: borrow Claude Desktop's own open handle
    //
    // Recent Claude Desktop builds open Network\Cookies with a share mode that
    // denies concurrent readers (CreateFileW then fails with Win32 32,
    // ERROR_SHARING_VIOLATION). The file cannot be opened or copied while
    // Desktop runs. But Desktop is a process we own, so we can duplicate the
    // handle it already holds and read through that.
    //
    // Reading through a shared handle is safe here for one specific reason: the
    // only other user of this handle is Chromium's SQLite, whose Win32 VFS
    // always reads and writes at absolute offsets (OVERLAPPED.Offset) and never
    // relies on the file's current position. SharedReadFile does the same, so
    // neither side disturbs the other. This is strictly read-only.
    // -----------------------------------------------------------------------

    typedef LONG(NTAPI* NtQuerySystemInformationFn)(
        ULONG SystemInformationClass,
        PVOID SystemInformation,
        ULONG SystemInformationLength,
        PULONG ReturnLength
    );

    struct SystemHandleEntry {
        PVOID Object;
        ULONG_PTR UniqueProcessId;
        ULONG_PTR HandleValue;
        ULONG GrantedAccess;
        USHORT CreatorBackTraceIndex;
        USHORT ObjectTypeIndex;
        ULONG HandleAttributes;
        ULONG Reserved;
    };

    struct SystemHandleInformationEx {
        ULONG_PTR NumberOfHandles;
        ULONG_PTR Reserved;
        SystemHandleEntry Handles[1];
    };

    static std::wstring FinalPathForHandle(HANDLE handle) {
        // FILE_TYPE_DISK gates out pipes/sockets, whose metadata queries can
        // block; a real file never hangs GetFinalPathNameByHandleW.
        if (GetFileType(handle) != FILE_TYPE_DISK) {
            return {};
        }

        DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
        if (needed == 0 || needed > 4096) {
            return {};
        }

        std::wstring path(needed, L'\0');
        DWORD written = GetFinalPathNameByHandleW(
            handle,
            path.data(),
            static_cast<DWORD>(path.size()),
            FILE_NAME_NORMALIZED
        );
        if (written == 0 || written >= path.size()) {
            return {};
        }
        path.resize(written);
        return path;
    }

    static std::wstring NormalizeFilePath(std::wstring path) {
        // Strip the \\?\ prefix GetFinalPathNameByHandleW adds and lowercase for
        // a case-insensitive compare against a plain absolute path.
        if (path.rfind(L"\\\\?\\", 0) == 0) {
            path.erase(0, 4);
        }
        for (wchar_t& c : path) {
            c = static_cast<wchar_t>(towlower(c));
        }
        return path;
    }

    static std::vector<DWORD> ClaudeDesktopProcessIds() {
        std::vector<DWORD> pids;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return pids;
        }

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);

        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, L"claude.exe") == 0) {
                    pids.push_back(entry.th32ProcessID);
                }
            } while (Process32NextW(snapshot, &entry));
        }

        CloseHandle(snapshot);
        return pids;
    }

    // Returns a duplicated, readable handle to targetPath borrowed from a Claude
    // Desktop process, or INVALID_HANDLE_VALUE. The caller owns the handle.
    static HANDLE DuplicateLockedFileHandle(const std::wstring& targetPath) {
        static NtQuerySystemInformationFn queryFn = []() -> NtQuerySystemInformationFn {
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            return ntdll
                ? reinterpret_cast<NtQuerySystemInformationFn>(
                    GetProcAddress(ntdll, "NtQuerySystemInformation"))
                : nullptr;
        }();

        if (!queryFn) {
            return INVALID_HANDLE_VALUE;
        }

        const std::vector<DWORD> pids = ClaudeDesktopProcessIds();
        if (pids.empty()) {
            return INVALID_HANDLE_VALUE;
        }

        const std::wstring wanted = NormalizeFilePath(targetPath);

        constexpr ULONG kSystemExtendedHandleInformation = 64;
        constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004);

        std::vector<unsigned char> buffer(1u << 20);
        ULONG returnLength = 0;
        LONG status = 0;

        for (int attempt = 0; attempt < 8; ++attempt) {
            status = queryFn(
                kSystemExtendedHandleInformation,
                buffer.data(),
                static_cast<ULONG>(buffer.size()),
                &returnLength
            );

            if (status == kStatusInfoLengthMismatch) {
                buffer.resize(buffer.size() * 2);
                continue;
            }
            break;
        }

        if (status < 0) {
            return INVALID_HANDLE_VALUE;
        }

        const auto* infoBlock = reinterpret_cast<const SystemHandleInformationEx*>(buffer.data());
        const ULONG_PTR count = infoBlock->NumberOfHandles;

        // Open each source process at most once.
        std::unordered_map<DWORD, HANDLE> processHandles;
        HANDLE result = INVALID_HANDLE_VALUE;

        for (ULONG_PTR i = 0; i < count && result == INVALID_HANDLE_VALUE; ++i) {
            const SystemHandleEntry& entry = infoBlock->Handles[i];
            const DWORD ownerPid = static_cast<DWORD>(entry.UniqueProcessId);

            if (std::find(pids.begin(), pids.end(), ownerPid) == pids.end()) {
                continue;
            }

            HANDLE sourceProcess = nullptr;
            auto cached = processHandles.find(ownerPid);
            if (cached != processHandles.end()) {
                sourceProcess = cached->second;
            }
            else {
                sourceProcess = OpenProcess(PROCESS_DUP_HANDLE, FALSE, ownerPid);
                processHandles.emplace(ownerPid, sourceProcess);
            }

            if (!sourceProcess) {
                continue;
            }

            HANDLE duplicated = nullptr;
            if (!DuplicateHandle(
                    sourceProcess,
                    reinterpret_cast<HANDLE>(entry.HandleValue),
                    GetCurrentProcess(),
                    &duplicated,
                    0,
                    FALSE,
                    DUPLICATE_SAME_ACCESS) ||
                !duplicated) {
                continue;
            }

            const std::wstring path = FinalPathForHandle(duplicated);

            if (!path.empty() && NormalizeFilePath(path) == wanted) {
                result = duplicated;
                break;
            }

            CloseHandle(duplicated);
        }

        for (const auto& pair : processHandles) {
            if (pair.second) {
                CloseHandle(pair.second);
            }
        }

        return result;
    }
#endif

    class SharedReadFile final {
    public:
        SharedReadFile() = default;

        SharedReadFile(const std::filesystem::path& path, bool optional)
            : m_path(path) {
            Open(optional);
        }

        SharedReadFile(const SharedReadFile&) = delete;
        SharedReadFile& operator=(const SharedReadFile&) = delete;
        SharedReadFile(SharedReadFile&&) noexcept = default;
        SharedReadFile& operator=(SharedReadFile&&) noexcept = default;

        bool Valid() const {
#ifdef _WIN32
            return m_handle.Valid();
#else
            return m_fd.Valid();
#endif
        }

        std::uint64_t Size() const {
            return QuerySize();
        }

        void ReadExact(std::uint64_t offset, void* destination, std::size_t size) const {
            if (size == 0) {
                return;
            }

            if (!Valid() || !destination) {
                throw std::runtime_error("Invalid read requested for " + PathText(m_path));
            }

            unsigned char* output = static_cast<unsigned char*>(destination);
            std::size_t completed = 0;

            while (completed < size) {
                const std::size_t remaining = size - completed;
                const std::size_t chunk = std::min<std::size_t>(
                    remaining,
                    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())
                );

#ifdef _WIN32
                const std::uint64_t absoluteOffset = offset + completed;
                OVERLAPPED overlapped{};
                overlapped.Offset = static_cast<DWORD>(absoluteOffset & 0xFFFFFFFFULL);
                overlapped.OffsetHigh = static_cast<DWORD>((absoluteOffset >> 32) & 0xFFFFFFFFULL);

                DWORD bytesRead = 0;
                BOOL readStarted = ReadFile(
                    m_handle.Get(),
                    output + completed,
                    static_cast<DWORD>(chunk),
                    &bytesRead,
                    &overlapped
                );

                if (!readStarted) {
                    const DWORD error = GetLastError();
                    if (error != ERROR_IO_PENDING ||
                        !GetOverlappedResult(m_handle.Get(), &overlapped, &bytesRead, TRUE)) {
                        const DWORD finalError = error == ERROR_IO_PENDING
                            ? GetLastError()
                            : error;
                        throw std::runtime_error(
                            "Read-only access to \"" + PathText(m_path) +
                            "\" failed (Win32 " + std::to_string(finalError) + ")"
                        );
                    }
                }

                if (bytesRead == 0) {
                    throw std::runtime_error(
                        "The live file changed while it was being read: " + PathText(m_path)
                    );
                }

                completed += static_cast<std::size_t>(bytesRead);
#else
                const ssize_t bytesRead = ::pread(
                    m_fd.Get(),
                    output + completed,
                    chunk,
                    static_cast<off_t>(offset + completed)
                );

                if (bytesRead < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    throw std::runtime_error(
                        "Read-only access to \"" + PathText(m_path) +
                        "\" failed (errno " + std::to_string(errno) + ")"
                    );
                }

                if (bytesRead == 0) {
                    throw std::runtime_error(
                        "The live file changed while it was being read: " + PathText(m_path)
                    );
                }

                completed += static_cast<std::size_t>(bytesRead);
#endif
            }
        }

        std::vector<unsigned char> ReadBytes(std::uint64_t offset, std::size_t size) const {
            std::vector<unsigned char> output(size);
            ReadExact(offset, output.data(), output.size());
            return output;
        }

        const std::filesystem::path& Path() const {
            return m_path;
        }

    private:
        void Open(bool optional) {
#ifdef _WIN32
            const std::wstring nativePath = m_path.wstring();
            HANDLE handle = CreateFileW(
                nativePath.c_str(),
                GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED,
                nullptr
            );

            if (handle == INVALID_HANDLE_VALUE) {
                const DWORD error = GetLastError();

                // Claude Desktop opens Cookies with no read sharing, so a normal
                // open fails with a sharing violation. Borrow the handle Desktop
                // already holds instead of giving up. This is what lets Auto and
                // Desktop modes read live usage while Desktop is running.
                if (error == ERROR_SHARING_VIOLATION) {
                    HANDLE borrowed = DuplicateLockedFileHandle(m_path.wstring());
                    if (borrowed != INVALID_HANDLE_VALUE) {
                        // m_handle now OWNS the duplicated handle. UniqueHandle
                        // closes it via CloseHandle in its destructor, and this
                        // SharedReadFile lives only for the duration of one
                        // ReadOnce call, so the borrowed handle is always closed
                        // as soon as the read finishes - it never leaks to app
                        // shutdown, and no duplicated handle is ever left open.
                        m_handle.Reset(borrowed);
                        return;
                    }
                }

                if (optional &&
                    (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
                    return;
                }

                throw std::runtime_error(
                    "Could not open \"" + PathText(m_path) +
                    "\" for shared read-only access (Win32 " +
                    std::to_string(error) + ")"
                );
            }

            m_handle.Reset(handle);
#else
            const int fd = ::open(m_path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                if (optional && errno == ENOENT) {
                    return;
                }

                throw std::runtime_error(
                    "Could not open \"" + PathText(m_path) +
                    "\" for read-only access (errno " +
                    std::to_string(errno) + ")"
                );
            }

            m_fd.Reset(fd);
#endif
        }

        std::uint64_t QuerySize() const {
            if (!Valid()) {
                return 0;
            }

#ifdef _WIN32
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(m_handle.Get(), &size) || size.QuadPart < 0) {
                throw std::runtime_error(
                    "Could not read the size of \"" + PathText(m_path) + "\""
                );
            }
            return static_cast<std::uint64_t>(size.QuadPart);
#else
            struct stat info{};
            if (::fstat(m_fd.Get(), &info) != 0 || info.st_size < 0) {
                throw std::runtime_error(
                    "Could not read the size of \"" + PathText(m_path) + "\""
                );
            }
            return static_cast<std::uint64_t>(info.st_size);
#endif
        }

        std::filesystem::path m_path;
#ifdef _WIN32
        UniqueHandle m_handle;
#else
        UniqueFd m_fd;
#endif
    };

    static std::uint16_t ReadBe16(const unsigned char* data) {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(data[0]) << 8) |
            static_cast<std::uint16_t>(data[1])
        );
    }

    static std::uint32_t ReadBe32(const unsigned char* data) {
        return
            (static_cast<std::uint32_t>(data[0]) << 24) |
            (static_cast<std::uint32_t>(data[1]) << 16) |
            (static_cast<std::uint32_t>(data[2]) << 8) |
            static_cast<std::uint32_t>(data[3]);
    }

    static std::uint64_t ReadUnsignedBigEndian(const unsigned char* data, std::size_t size) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < size; ++i) {
            value = (value << 8) | static_cast<std::uint64_t>(data[i]);
        }
        return value;
    }

    static std::int64_t ReadSignedBigEndian(const unsigned char* data, std::size_t size) {
        if (size == 0 || size > 8) {
            throw std::runtime_error("Invalid SQLite integer width");
        }

        std::uint64_t value = ReadUnsignedBigEndian(data, size);
        if (size < 8 && (data[0] & 0x80u) != 0) {
            value |= (~0ULL) << (size * 8);
        }
        return static_cast<std::int64_t>(value);
    }

    static std::uint32_t ReadChecksumWord(const unsigned char* data, bool littleEndian) {
        if (littleEndian) {
            return
                static_cast<std::uint32_t>(data[0]) |
                (static_cast<std::uint32_t>(data[1]) << 8) |
                (static_cast<std::uint32_t>(data[2]) << 16) |
                (static_cast<std::uint32_t>(data[3]) << 24);
        }
        return ReadBe32(data);
    }

    struct WalChecksum {
        std::uint32_t first = 0;
        std::uint32_t second = 0;
    };

    static WalChecksum UpdateWalChecksum(
        const unsigned char* data,
        std::size_t size,
        bool littleEndian,
        WalChecksum checksum
    ) {
        if (!data || (size % 8) != 0) {
            throw std::runtime_error("Invalid SQLite WAL checksum input");
        }

        for (std::size_t offset = 0; offset < size; offset += 8) {
            const std::uint32_t firstWord = ReadChecksumWord(data + offset, littleEndian);
            const std::uint32_t secondWord = ReadChecksumWord(data + offset + 4, littleEndian);
            checksum.first += firstWord + checksum.second;
            checksum.second += secondWord + checksum.first;
        }

        return checksum;
    }

    struct Varint {
        std::uint64_t value = 0;
        std::size_t size = 0;
    };

    static Varint ReadVarint(
        const std::vector<unsigned char>& data,
        std::size_t offset,
        std::size_t limit
    ) {
        if (limit > data.size() || offset >= limit) {
            throw std::runtime_error("SQLite varint is outside the page");
        }

        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            if (offset + i >= limit) {
                throw std::runtime_error("Truncated SQLite varint");
            }

            const unsigned char byte = data[offset + i];
            value = (value << 7) | static_cast<std::uint64_t>(byte & 0x7Fu);
            if ((byte & 0x80u) == 0) {
                return { value, i + 1 };
            }
        }

        if (offset + 8 >= limit) {
            throw std::runtime_error("Truncated nine-byte SQLite varint");
        }

        value = (value << 8) | static_cast<std::uint64_t>(data[offset + 8]);
        return { value, 9 };
    }

    enum class ValueKind {
        Null,
        Integer,
        Real,
        Text,
        Blob
    };

    struct SqlValue {
        ValueKind kind = ValueKind::Null;
        std::int64_t integer = 0;
        double real = 0.0;
        std::string text;
        std::vector<unsigned char> blob;
    };

    static std::size_t SerialTypeSize(std::uint64_t serialType) {
        switch (serialType) {
        case 0: return 0;
        case 1: return 1;
        case 2: return 2;
        case 3: return 3;
        case 4: return 4;
        case 5: return 6;
        case 6: return 8;
        case 7: return 8;
        case 8: return 0;
        case 9: return 0;
        case 10:
        case 11:
            throw std::runtime_error("Reserved SQLite serial type encountered");
        default:
            if ((serialType & 1ULL) == 0) {
                return static_cast<std::size_t>((serialType - 12ULL) / 2ULL);
            }
            return static_cast<std::size_t>((serialType - 13ULL) / 2ULL);
        }
    }

    static std::vector<SqlValue> ParseRecord(const std::vector<unsigned char>& payload) {
        if (payload.empty()) {
            throw std::runtime_error("Empty SQLite record payload");
        }

        const Varint headerSizeVarint = ReadVarint(payload, 0, payload.size());
        if (headerSizeVarint.value < headerSizeVarint.size ||
            headerSizeVarint.value > payload.size()) {
            throw std::runtime_error("Invalid SQLite record header size");
        }

        const std::size_t headerSize = static_cast<std::size_t>(headerSizeVarint.value);
        std::size_t headerOffset = headerSizeVarint.size;
        std::vector<std::uint64_t> serialTypes;

        while (headerOffset < headerSize) {
            const Varint serial = ReadVarint(payload, headerOffset, headerSize);
            serialTypes.push_back(serial.value);
            headerOffset += serial.size;
        }

        if (headerOffset != headerSize) {
            throw std::runtime_error("SQLite record header did not end on a field boundary");
        }

        std::size_t dataOffset = headerSize;
        std::vector<SqlValue> values;
        values.reserve(serialTypes.size());

        for (std::uint64_t serialType : serialTypes) {
            const std::size_t fieldSize = SerialTypeSize(serialType);
            if (fieldSize > payload.size() - dataOffset) {
                throw std::runtime_error("SQLite record field exceeds its payload");
            }

            SqlValue value;
            if (serialType == 0) {
                value.kind = ValueKind::Null;
            }
            else if (serialType >= 1 && serialType <= 6) {
                value.kind = ValueKind::Integer;
                value.integer = ReadSignedBigEndian(payload.data() + dataOffset, fieldSize);
            }
            else if (serialType == 7) {
                value.kind = ValueKind::Real;
                const std::uint64_t bits = ReadUnsignedBigEndian(payload.data() + dataOffset, 8);
                static_assert(sizeof(bits) == sizeof(value.real));
                std::memcpy(&value.real, &bits, sizeof(bits));
#if defined(_WIN32) || defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                // bits is already the host-order integer representation of the
                // big-endian bytes. memcpy therefore produces the correct IEEE
                // value on little-endian and Windows hosts.
#endif
            }
            else if (serialType == 8 || serialType == 9) {
                value.kind = ValueKind::Integer;
                value.integer = serialType == 8 ? 0 : 1;
            }
            else if ((serialType & 1ULL) == 0) {
                value.kind = ValueKind::Blob;
                value.blob.assign(
                    payload.begin() + static_cast<std::ptrdiff_t>(dataOffset),
                    payload.begin() + static_cast<std::ptrdiff_t>(dataOffset + fieldSize)
                );
            }
            else {
                value.kind = ValueKind::Text;
                value.text.assign(
                    reinterpret_cast<const char*>(payload.data() + dataOffset),
                    fieldSize
                );
            }

            dataOffset += fieldSize;
            values.push_back(std::move(value));
        }

        return values;
    }

    struct WalFrame {
        std::uint32_t pageNumber = 0;
        std::uint32_t databaseSizeAfterCommit = 0;
        std::uint64_t pageDataOffset = 0;
    };

    class LivePager final {
    public:
        explicit LivePager(const std::filesystem::path& databasePath)
            : m_database(databasePath, false),
              m_wal(SidecarPath(databasePath, "-wal"), true),
              m_rollbackJournalPath(SidecarPath(databasePath, "-journal")) {
            RejectActiveRollbackJournal();
            LoadDatabaseHeader();
            LoadWal();
        }

        std::uint32_t PageSize() const {
            return m_pageSize;
        }

        std::uint32_t UsablePageSize() const {
            return m_pageSize - m_reservedBytes;
        }

        std::vector<unsigned char> ReadPage(std::uint32_t pageNumber) const {
            if (pageNumber == 0 || pageNumber > m_databasePageCount) {
                throw std::runtime_error("SQLite page number is outside the live database");
            }

            auto walPage = m_walPageOffsets.find(pageNumber);
            if (walPage != m_walPageOffsets.end()) {
                return m_wal.ReadBytes(walPage->second, m_pageSize);
            }

            const std::uint64_t offset =
                (static_cast<std::uint64_t>(pageNumber) - 1ULL) *
                static_cast<std::uint64_t>(m_pageSize);

            if (offset > m_database.Size() ||
                static_cast<std::uint64_t>(m_pageSize) > m_database.Size() - offset) {
                throw std::runtime_error("SQLite main database page is truncated");
            }

            return m_database.ReadBytes(offset, m_pageSize);
        }

        bool IsStable() const {
            try {
                if (m_database.Size() != m_databaseSizeAtOpen) {
                    return false;
                }

                const std::vector<unsigned char> currentHeader =
                    m_database.ReadBytes(0, kDatabaseHeaderSize);
                if (currentHeader != m_databaseHeader) {
                    return false;
                }

                if (HasActiveRollbackJournal()) {
                    return false;
                }

                if (!m_walWasPresent) {
                    return !std::filesystem::exists(m_wal.Path());
                }

                if (!m_wal.Valid() || m_wal.Size() != m_walSizeAtOpen) {
                    return false;
                }

                if (m_walSizeAtOpen == 0) {
                    return true;
                }

                const std::vector<unsigned char> currentWalHeader =
                    m_wal.ReadBytes(0, kWalHeaderSize);
                return currentWalHeader == m_walHeader;
            }
            catch (...) {
                return false;
            }
        }

    private:
        bool HasActiveRollbackJournal() const {
            std::error_code error;
            const bool exists = std::filesystem::exists(m_rollbackJournalPath, error);
            if (error) {
                return true;
            }
            if (!exists) {
                return false;
            }

            const std::uintmax_t size =
                std::filesystem::file_size(m_rollbackJournalPath, error);
            return error || size > 0;
        }

        void RejectActiveRollbackJournal() const {
            if (HasActiveRollbackJournal()) {
                throw std::runtime_error(
                    "Claude Desktop Cookies database has an active rollback journal; retry after the write completes"
                );
            }
        }

        void LoadDatabaseHeader() {
            m_databaseSizeAtOpen = m_database.Size();
            if (m_databaseSizeAtOpen > kMaximumDatabaseBytes) {
                throw std::runtime_error("Claude Desktop Cookies database is unexpectedly large");
            }
            if (m_databaseSizeAtOpen < kDatabaseHeaderSize) {
                throw std::runtime_error("Claude Desktop Cookies database is truncated");
            }

            m_databaseHeader = m_database.ReadBytes(0, kDatabaseHeaderSize);
            static const unsigned char signature[] = {
                'S','Q','L','i','t','e',' ','f','o','r','m','a','t',' ','3','\0'
            };

            if (!std::equal(
                std::begin(signature),
                std::end(signature),
                m_databaseHeader.begin()
            )) {
                throw std::runtime_error("Claude Desktop Cookies file is not a SQLite database");
            }

            std::uint32_t pageSize = ReadBe16(m_databaseHeader.data() + 16);
            if (pageSize == 1) {
                pageSize = 65536;
            }

            if (pageSize < 512 || pageSize > 65536 ||
                (pageSize & (pageSize - 1)) != 0) {
                throw std::runtime_error("Claude Desktop Cookies database has an invalid page size");
            }

            m_pageSize = pageSize;
            m_reservedBytes = m_databaseHeader[20];
            if (m_reservedBytes >= m_pageSize || UsablePageSize() < 480) {
                throw std::runtime_error("Claude Desktop Cookies database has an invalid reserved-byte count");
            }

            if ((m_databaseSizeAtOpen % static_cast<std::uint64_t>(m_pageSize)) != 0) {
                throw std::runtime_error("Claude Desktop Cookies database ends inside a SQLite page");
            }

            const std::uint64_t pageCount =
                m_databaseSizeAtOpen / static_cast<std::uint64_t>(m_pageSize);
            if (pageCount > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error("Claude Desktop Cookies database contains too many pages");
            }
            m_databasePageCount = static_cast<std::uint32_t>(pageCount);

            if (m_databasePageCount == 0) {
                throw std::runtime_error("Claude Desktop Cookies database contains no pages");
            }
        }

        void LoadWal() {
            m_walWasPresent = m_wal.Valid();
            if (!m_walWasPresent) {
                return;
            }

            m_walSizeAtOpen = m_wal.Size();
            if (m_walSizeAtOpen > kMaximumWalBytes) {
                throw std::runtime_error("Claude Desktop Cookies WAL is unexpectedly large");
            }
            if (m_walSizeAtOpen == 0) {
                return;
            }

            if (m_walSizeAtOpen < kWalHeaderSize) {
                throw std::runtime_error("Claude Desktop Cookies WAL is truncated");
            }

            m_walHeader = m_wal.ReadBytes(0, kWalHeaderSize);
            const std::uint32_t magic = ReadBe32(m_walHeader.data());
            if (magic != kWalMagicLittleChecksum && magic != kWalMagicBigChecksum) {
                throw std::runtime_error("Claude Desktop Cookies WAL has an invalid magic value");
            }

            const bool littleEndianChecksum = magic == kWalMagicLittleChecksum;
            const std::uint32_t walPageSize = ReadBe32(m_walHeader.data() + 8);
            if (walPageSize != m_pageSize) {
                throw std::runtime_error("Claude Desktop Cookies WAL page size does not match its database");
            }

            WalChecksum checksum = UpdateWalChecksum(
                m_walHeader.data(),
                24,
                littleEndianChecksum,
                {}
            );

            if (checksum.first != ReadBe32(m_walHeader.data() + 24) ||
                checksum.second != ReadBe32(m_walHeader.data() + 28)) {
                throw std::runtime_error("Claude Desktop Cookies WAL header checksum is invalid");
            }

            const std::uint32_t saltFirst = ReadBe32(m_walHeader.data() + 16);
            const std::uint32_t saltSecond = ReadBe32(m_walHeader.data() + 20);
            const std::uint64_t frameSize =
                static_cast<std::uint64_t>(kWalFrameHeaderSize) +
                static_cast<std::uint64_t>(m_pageSize);
            const std::uint64_t completeFrameBytes = m_walSizeAtOpen - kWalHeaderSize;
            const std::uint64_t frameCount = completeFrameBytes / frameSize;

            std::vector<WalFrame> validFrames;
            validFrames.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
                frameCount,
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())
            )));

            std::size_t lastCommitCount = 0;

            for (std::uint64_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
                const std::uint64_t frameOffset =
                    kWalHeaderSize + frameIndex * frameSize;
                const std::vector<unsigned char> frameHeader =
                    m_wal.ReadBytes(frameOffset, kWalFrameHeaderSize);

                if (ReadBe32(frameHeader.data() + 8) != saltFirst ||
                    ReadBe32(frameHeader.data() + 12) != saltSecond) {
                    break;
                }

                const std::uint32_t pageNumber = ReadBe32(frameHeader.data());
                if (pageNumber == 0) {
                    break;
                }

                const std::vector<unsigned char> page =
                    m_wal.ReadBytes(frameOffset + kWalFrameHeaderSize, m_pageSize);

                checksum = UpdateWalChecksum(
                    frameHeader.data(),
                    8,
                    littleEndianChecksum,
                    checksum
                );
                checksum = UpdateWalChecksum(
                    page.data(),
                    page.size(),
                    littleEndianChecksum,
                    checksum
                );

                if (checksum.first != ReadBe32(frameHeader.data() + 16) ||
                    checksum.second != ReadBe32(frameHeader.data() + 20)) {
                    break;
                }

                WalFrame frame;
                frame.pageNumber = pageNumber;
                frame.databaseSizeAfterCommit = ReadBe32(frameHeader.data() + 4);
                frame.pageDataOffset = frameOffset + kWalFrameHeaderSize;
                validFrames.push_back(frame);

                if (frame.databaseSizeAfterCommit != 0) {
                    lastCommitCount = validFrames.size();
                }
            }

            if (lastCommitCount == 0) {
                return;
            }

            for (std::size_t i = 0; i < lastCommitCount; ++i) {
                const WalFrame& frame = validFrames[i];
                m_walPageOffsets[frame.pageNumber] = frame.pageDataOffset;
            }

            const std::uint32_t committedSize =
                validFrames[lastCommitCount - 1].databaseSizeAfterCommit;
            if (committedSize != 0) {
                m_databasePageCount = committedSize;
            }
        }

        SharedReadFile m_database;
        SharedReadFile m_wal;
        std::filesystem::path m_rollbackJournalPath;
        std::uint32_t m_pageSize = 0;
        std::uint32_t m_reservedBytes = 0;
        std::uint32_t m_databasePageCount = 0;
        std::uint64_t m_databaseSizeAtOpen = 0;
        std::uint64_t m_walSizeAtOpen = 0;
        bool m_walWasPresent = false;
        std::vector<unsigned char> m_databaseHeader;
        std::vector<unsigned char> m_walHeader;
        std::unordered_map<std::uint32_t, std::uint64_t> m_walPageOffsets;
    };

    static std::size_t LocalPayloadSize(
        std::uint64_t payloadSize,
        std::uint32_t usablePageSize
    ) {
        const std::uint64_t maxLocal = usablePageSize - 35ULL;
        const std::uint64_t minLocal =
            ((static_cast<std::uint64_t>(usablePageSize) - 12ULL) * 32ULL / 255ULL) - 23ULL;

        if (payloadSize <= maxLocal) {
            return static_cast<std::size_t>(payloadSize);
        }

        std::uint64_t local = minLocal +
            ((payloadSize - minLocal) % (static_cast<std::uint64_t>(usablePageSize) - 4ULL));
        if (local > maxLocal) {
            local = minLocal;
        }

        return static_cast<std::size_t>(local);
    }

    static std::vector<unsigned char> ReadCellPayload(
        const LivePager& pager,
        const std::vector<unsigned char>& page,
        std::size_t payloadOffset,
        std::uint64_t payloadSize
    ) {
        if (payloadSize > kMaximumPayloadBytes) {
            throw std::runtime_error("SQLite record payload is unexpectedly large");
        }

        const std::size_t localSize = LocalPayloadSize(payloadSize, pager.UsablePageSize());
        if (payloadOffset > page.size() || localSize > page.size() - payloadOffset) {
            throw std::runtime_error("SQLite cell payload exceeds its page");
        }

        std::vector<unsigned char> payload;
        payload.reserve(static_cast<std::size_t>(payloadSize));
        payload.insert(
            payload.end(),
            page.begin() + static_cast<std::ptrdiff_t>(payloadOffset),
            page.begin() + static_cast<std::ptrdiff_t>(payloadOffset + localSize)
        );

        std::uint64_t remaining = payloadSize - localSize;
        if (remaining == 0) {
            return payload;
        }

        if (payloadOffset + localSize + 4 > page.size()) {
            throw std::runtime_error("SQLite overflow-page pointer is truncated");
        }

        std::uint32_t overflowPage = ReadBe32(page.data() + payloadOffset + localSize);
        std::unordered_map<std::uint32_t, bool> visited;

        while (remaining > 0) {
            if (overflowPage == 0 || visited.find(overflowPage) != visited.end()) {
                throw std::runtime_error("Invalid SQLite overflow-page chain");
            }
            visited[overflowPage] = true;

            const std::vector<unsigned char> overflow = pager.ReadPage(overflowPage);
            if (overflow.size() < 4) {
                throw std::runtime_error("SQLite overflow page is truncated");
            }

            const std::size_t available = pager.UsablePageSize() - 4ULL;
            const std::size_t take = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, available)
            );

            if (4 + take > overflow.size()) {
                throw std::runtime_error("SQLite overflow payload exceeds its page");
            }

            payload.insert(
                payload.end(),
                overflow.begin() + 4,
                overflow.begin() + static_cast<std::ptrdiff_t>(4 + take)
            );

            remaining -= take;
            overflowPage = ReadBe32(overflow.data());
        }

        return payload;
    }

    using RowVisitor = std::function<void(std::uint64_t, const std::vector<SqlValue>&)>;

    static void VisitTablePage(
        const LivePager& pager,
        std::uint32_t pageNumber,
        std::uint32_t depth,
        std::unordered_map<std::uint32_t, bool>& visited,
        const RowVisitor& visitor
    ) {
        if (depth > kMaximumTraversalDepth) {
            throw std::runtime_error("SQLite B-tree traversal exceeded its depth limit");
        }

        if (pageNumber == 0 || visited.find(pageNumber) != visited.end()) {
            throw std::runtime_error("Invalid or cyclic SQLite B-tree page");
        }
        visited[pageNumber] = true;

        const std::vector<unsigned char> page = pager.ReadPage(pageNumber);
        const std::size_t headerOffset = pageNumber == 1 ? kDatabaseHeaderSize : 0;
        if (headerOffset + 8 > page.size()) {
            throw std::runtime_error("SQLite B-tree page header is truncated");
        }

        const unsigned char pageType = page[headerOffset];
        const bool interiorTable = pageType == 0x05;
        const bool leafTable = pageType == 0x0D;
        if (!interiorTable && !leafTable) {
            throw std::runtime_error("Expected a SQLite table B-tree page");
        }

        const std::size_t btreeHeaderSize = interiorTable ? 12 : 8;
        const std::uint16_t cellCount = ReadBe16(page.data() + headerOffset + 3);
        const std::size_t pointerArrayOffset = headerOffset + btreeHeaderSize;
        const std::size_t pointerArrayBytes = static_cast<std::size_t>(cellCount) * 2ULL;
        if (pointerArrayOffset > page.size() ||
            pointerArrayBytes > page.size() - pointerArrayOffset) {
            throw std::runtime_error("SQLite cell-pointer array is truncated");
        }

        if (interiorTable) {
            for (std::uint16_t index = 0; index < cellCount; ++index) {
                const std::uint16_t cellOffset = ReadBe16(
                    page.data() + pointerArrayOffset + static_cast<std::size_t>(index) * 2ULL
                );
                if (cellOffset > page.size() || 4 > page.size() - cellOffset) {
                    throw std::runtime_error("SQLite interior-table cell is truncated");
                }

                const std::uint32_t childPage = ReadBe32(page.data() + cellOffset);
                VisitTablePage(pager, childPage, depth + 1, visited, visitor);
            }

            const std::uint32_t rightMostPage = ReadBe32(page.data() + headerOffset + 8);
            VisitTablePage(pager, rightMostPage, depth + 1, visited, visitor);
            return;
        }

        for (std::uint16_t index = 0; index < cellCount; ++index) {
            const std::uint16_t cellOffset = ReadBe16(
                page.data() + pointerArrayOffset + static_cast<std::size_t>(index) * 2ULL
            );
            if (cellOffset >= page.size()) {
                throw std::runtime_error("SQLite leaf-table cell offset is invalid");
            }

            const Varint payloadSize = ReadVarint(page, cellOffset, page.size());
            const std::size_t rowIdOffset = cellOffset + payloadSize.size;
            const Varint rowId = ReadVarint(page, rowIdOffset, page.size());
            const std::size_t payloadOffset = rowIdOffset + rowId.size;
            const std::vector<unsigned char> payload = ReadCellPayload(
                pager,
                page,
                payloadOffset,
                payloadSize.value
            );

            visitor(rowId.value, ParseRecord(payload));
        }
    }

    static void VisitTable(
        const LivePager& pager,
        std::uint32_t rootPage,
        const RowVisitor& visitor
    ) {
        std::unordered_map<std::uint32_t, bool> visited;
        VisitTablePage(pager, rootPage, 0, visited, visitor);
    }

    static std::string LowerAscii(std::string text) {
        for (char& character : text) {
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character))
            );
        }
        return text;
    }

    static std::string Trim(std::string text) {
        auto isSpace = [](unsigned char character) {
            return std::isspace(character) != 0;
        };

        while (!text.empty() && isSpace(static_cast<unsigned char>(text.front()))) {
            text.erase(text.begin());
        }
        while (!text.empty() && isSpace(static_cast<unsigned char>(text.back()))) {
            text.pop_back();
        }
        return text;
    }

    static std::vector<std::string> SplitColumnDefinitions(const std::string& createSql) {
        const std::size_t open = createSql.find('(');
        const std::size_t close = createSql.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close <= open) {
            throw std::runtime_error("Could not parse the Chromium cookies table definition");
        }

        std::vector<std::string> definitions;
        std::size_t itemStart = open + 1;
        int parenthesisDepth = 0;
        char quote = '\0';
        bool bracketQuote = false;

        for (std::size_t index = open + 1; index < close; ++index) {
            const char character = createSql[index];

            if (bracketQuote) {
                if (character == ']') {
                    bracketQuote = false;
                }
                continue;
            }

            if (quote != '\0') {
                if (character == quote) {
                    if (index + 1 < close && createSql[index + 1] == quote) {
                        ++index;
                    }
                    else {
                        quote = '\0';
                    }
                }
                continue;
            }

            if (character == '[') {
                bracketQuote = true;
            }
            else if (character == '\'' || character == '"' || character == '`') {
                quote = character;
            }
            else if (character == '(') {
                ++parenthesisDepth;
            }
            else if (character == ')') {
                if (parenthesisDepth > 0) {
                    --parenthesisDepth;
                }
            }
            else if (character == ',' && parenthesisDepth == 0) {
                definitions.push_back(Trim(createSql.substr(itemStart, index - itemStart)));
                itemStart = index + 1;
            }
        }

        if (itemStart < close) {
            definitions.push_back(Trim(createSql.substr(itemStart, close - itemStart)));
        }

        return definitions;
    }

    static std::string FirstIdentifier(const std::string& definition) {
        std::string text = Trim(definition);
        if (text.empty()) {
            return {};
        }

        const std::string lower = LowerAscii(text);
        for (const char* prefix : {
            "constraint ", "primary ", "unique ", "check ", "foreign "
        }) {
            if (lower.rfind(prefix, 0) == 0) {
                return {};
            }
        }

        if (text.front() == '[') {
            const std::size_t end = text.find(']', 1);
            return end == std::string::npos ? std::string() : text.substr(1, end - 1);
        }

        if (text.front() == '"' || text.front() == '\'' || text.front() == '`') {
            const char quote = text.front();
            std::string identifier;
            for (std::size_t index = 1; index < text.size(); ++index) {
                if (text[index] == quote) {
                    if (index + 1 < text.size() && text[index + 1] == quote) {
                        identifier.push_back(quote);
                        ++index;
                        continue;
                    }
                    return identifier;
                }
                identifier.push_back(text[index]);
            }
            return {};
        }

        std::size_t end = 0;
        while (end < text.size() &&
               std::isspace(static_cast<unsigned char>(text[end])) == 0 &&
               text[end] != '(') {
            ++end;
        }
        return text.substr(0, end);
    }

    static std::unordered_map<std::string, std::size_t> ParseColumnIndexes(
        const std::string& createSql
    ) {
        std::unordered_map<std::string, std::size_t> indexes;
        std::size_t columnIndex = 0;

        for (const std::string& definition : SplitColumnDefinitions(createSql)) {
            const std::string identifier = FirstIdentifier(definition);
            if (identifier.empty()) {
                continue;
            }
            indexes[LowerAscii(identifier)] = columnIndex++;
        }

        return indexes;
    }

    static std::string ValueText(const std::vector<SqlValue>& values, std::size_t index) {
        if (index >= values.size() || values[index].kind != ValueKind::Text) {
            return {};
        }
        return values[index].text;
    }

    static std::vector<unsigned char> ValueBlob(
        const std::vector<SqlValue>& values,
        std::size_t index
    ) {
        if (index >= values.size() || values[index].kind != ValueKind::Blob) {
            return {};
        }
        return values[index].blob;
    }

    static long long ValueInteger(const std::vector<SqlValue>& values, std::size_t index) {
        if (index >= values.size() || values[index].kind != ValueKind::Integer) {
            return 0;
        }
        return static_cast<long long>(values[index].integer);
    }

    static bool IsTargetCookieName(const std::string& name) {
        return name == "lastActiveOrg" ||
            name == "sessionKeyV2" ||
            name == "sessionKey" ||
            name == "sessionKeyV3";
    }

    static bool IsClaudeHost(const std::string& host) {
        std::string lower = LowerAscii(host);
        while (!lower.empty() && lower.front() == '.') {
            lower.erase(lower.begin());
        }

        auto isDomainOrSubdomain = [&](const char* domain) {
            const std::string expected(domain);
            if (lower == expected) {
                return true;
            }
            return lower.size() > expected.size() &&
                lower.compare(lower.size() - expected.size(), expected.size(), expected) == 0 &&
                lower[lower.size() - expected.size() - 1] == '.';
        };

        return isDomainOrSubdomain("claude.ai") ||
            isDomainOrSubdomain("claude.com");
    }

    static ReadResult ReadOnce(const std::filesystem::path& cookiePath) {
        LivePager pager(cookiePath);

        const std::vector<unsigned char> liveHeader = pager.ReadPage(1);
        if (liveHeader.size() < kDatabaseHeaderSize) {
            throw std::runtime_error("Claude Desktop Cookies database header is truncated");
        }
        // Bytes 18/19 are SQLite's file write/read format versions: 1 =
        // rollback journal, 2 = WAL. Both are readable here. In WAL mode recent
        // frames overlay the DB (LivePager handles that); in rollback-journal
        // mode the main DB file is itself the committed state and the journal
        // only holds pages to undo, so reading the DB directly is correct. A
        // hot journal or a bumped change counter forces a consistency retry, so
        // a mid-commit torn read is never returned. Claude Desktop currently
        // ships the Cookies DB in rollback-journal mode.
        const unsigned char writeVersion = liveHeader[18];
        const unsigned char readVersion = liveHeader[19];
        if ((writeVersion != 1 && writeVersion != 2) ||
            (readVersion != 1 && readVersion != 2)) {
            throw std::runtime_error(
                "Claude Desktop Cookies database uses an unsupported SQLite journal format"
            );
        }

        const std::uint32_t encoding = ReadBe32(liveHeader.data() + 56);
        if (encoding != 1) {
            throw std::runtime_error("Claude Desktop Cookies database is not UTF-8 encoded");
        }

        std::uint32_t cookiesRootPage = 0;
        std::string cookiesCreateSql;
        std::uint32_t metaRootPage = 0;
        std::string metaCreateSql;

        VisitTable(
            pager,
            1,
            [&](std::uint64_t, const std::vector<SqlValue>& values) {
                if (values.size() < 5 || ValueText(values, 0) != "table") {
                    return;
                }

                const std::string name = ValueText(values, 1);
                if (name == "cookies") {
                    cookiesRootPage = static_cast<std::uint32_t>(ValueInteger(values, 3));
                    cookiesCreateSql = ValueText(values, 4);
                }
                else if (name == "meta") {
                    metaRootPage = static_cast<std::uint32_t>(ValueInteger(values, 3));
                    metaCreateSql = ValueText(values, 4);
                }
            }
        );

        if (cookiesRootPage == 0 || cookiesCreateSql.empty()) {
            throw std::runtime_error("Chromium cookies table was not found");
        }

        const auto indexes = ParseColumnIndexes(cookiesCreateSql);
        const std::array<const char*, 7> requiredColumns = {
            "name",
            "host_key",
            "value",
            "encrypted_value",
            "expires_utc",
            "last_access_utc",
            "creation_utc"
        };

        for (const char* column : requiredColumns) {
            if (indexes.find(column) == indexes.end()) {
                throw std::runtime_error(
                    std::string("Chromium cookies table is missing required column: ") + column
                );
            }
        }

        int databaseVersion = 0;
        if (metaRootPage != 0 && !metaCreateSql.empty()) {
            const auto metaIndexes = ParseColumnIndexes(metaCreateSql);
            const auto keyIndex = metaIndexes.find("key");
            const auto valueIndex = metaIndexes.find("value");

            if (keyIndex != metaIndexes.end() && valueIndex != metaIndexes.end()) {
                VisitTable(
                    pager,
                    metaRootPage,
                    [&](std::uint64_t, const std::vector<SqlValue>& values) {
                        if (ValueText(values, keyIndex->second) != "version") {
                            return;
                        }

                        const std::string versionText = ValueText(values, valueIndex->second);
                        if (!versionText.empty()) {
                            try {
                                databaseVersion = std::stoi(versionText);
                            }
                            catch (...) {
                                databaseVersion = 0;
                            }
                        }
                        else {
                            databaseVersion = static_cast<int>(
                                ValueInteger(values, valueIndex->second)
                            );
                        }
                    }
                );
            }
        }

        std::vector<RawCookie> cookies;
        VisitTable(
            pager,
            cookiesRootPage,
            [&](std::uint64_t, const std::vector<SqlValue>& values) {
                const std::string name = ValueText(values, indexes.at("name"));
                if (!IsTargetCookieName(name)) {
                    return;
                }

                const std::string host = ValueText(values, indexes.at("host_key"));
                if (!IsClaudeHost(host)) {
                    return;
                }

                RawCookie cookie;
                cookie.name = name;
                cookie.hostKey = host;
                cookie.value = ValueText(values, indexes.at("value"));
                cookie.encryptedValue = ValueBlob(values, indexes.at("encrypted_value"));
                cookie.expiresUtc = ValueInteger(values, indexes.at("expires_utc"));
                cookie.lastAccessUtc = ValueInteger(values, indexes.at("last_access_utc"));
                cookie.creationUtc = ValueInteger(values, indexes.at("creation_utc"));
                cookies.push_back(std::move(cookie));
            }
        );

        if (!pager.IsStable()) {
            throw std::runtime_error(
                "Claude Desktop changed its Cookies database during the read"
            );
        }

        ReadResult result;
        result.databaseVersion = databaseVersion;
        result.cookies = std::move(cookies);
        return result;
    }

} // namespace

ReadResult ReadLive(const std::filesystem::path& cookiePath) {
    std::string lastError;

    for (int attempt = 0; attempt < kMaximumConsistencyAttempts; ++attempt) {
        try {
            return ReadOnce(cookiePath);
        }
        catch (const std::exception& error) {
            lastError = error.what();
            if (attempt + 1 < kMaximumConsistencyAttempts) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        }
    }

    throw std::runtime_error(
        lastError.empty()
            ? "Could not read Claude Desktop Cookies database"
            : lastError
    );
}

}
