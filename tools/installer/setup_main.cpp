#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <commctrl.h>
#include <compressapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <set>
#include <thread>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

#pragma pack(push, 1)
struct SfxFooter {
    char magic[8];              // "BOOSFX01"
    uint64_t payloadOffset;     // Byte offset of compressed archive inside .exe
    uint64_t compressedBytes;   // Compressed LZMS byte length
    uint64_t uncompressedBytes; // Uncompressed archive byte length
    uint64_t fnv1a64;           // FNV-1a 64-bit integrity hash of uncompressed archive
};
#pragma pack(pop)
static_assert(sizeof(SfxFooter) == 40, "SfxFooter must be 40 bytes");

constexpr char kSfxMagic[8] = {'B', 'O', 'O', 'S', 'F', 'X', '0', '1'};
constexpr const wchar_t* kAppTitle        = L"BOO SAH SAH \u2014 The Glass Orchid Protocol";
constexpr const wchar_t* kSetupWindowTitle = L"BOO SAH SAH \u2014 Setup";
constexpr const wchar_t* kAppExeName      = L"boo_sah_sah.exe";
constexpr const wchar_t* kUninstallExe    = L"Uninstall-BooSahSah.exe";
constexpr const wchar_t* kManifestFile    = L"install_manifest.txt";
constexpr const wchar_t* kUninstallRegKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\BooSahSah";

uint64_t computeFnv1a64(const uint8_t* data, size_t len) noexcept {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

fs::path currentExecutablePath() {
    std::wstring buf(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0 || len >= buf.size()) return {};
    buf.resize(len);
    return fs::path(buf);
}

fs::path defaultInstallDirectory() {
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) && localAppData) {
        fs::path p = fs::path(localAppData) / L"Programs" / L"BooSahSah";
        CoTaskMemFree(localAppData);
        return p;
    }
    return fs::temp_directory_path() / L"BooSahSah";
}

fs::path knownShortcutFolder(const KNOWNFOLDERID& id) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) && raw) {
        fs::path p(raw);
        CoTaskMemFree(raw);
        return p;
    }
    return {};
}

bool createShellShortcut(
    const fs::path& shortcutLnkPath,
    const fs::path& targetExePath,
    const fs::path& workingDir,
    const wchar_t* description) {
    std::error_code ec;
    fs::create_directories(shortcutLnkPath.parent_path(), ec);

    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl));
    if (FAILED(hr) || !psl) return false;

    psl->SetPath(targetExePath.c_str());
    psl->SetWorkingDirectory(workingDir.c_str());
    psl->SetDescription(description);
    psl->SetIconLocation(targetExePath.c_str(), 0);

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_PPV_ARGS(&ppf));
    bool ok = false;
    if (SUCCEEDED(hr) && ppf) {
        ok = SUCCEEDED(ppf->Save(shortcutLnkPath.c_str(), TRUE));
        ppf->Release();
    }
    psl->Release();
    return ok;
}

bool isSafeRelativeArchivePath(const fs::path& rel) {
    if (rel.empty() || rel.is_absolute() || rel.has_root_name() || rel.has_root_directory()) {
        return false;
    }
    for (const auto& part : rel) {
        if (part.empty() || part.wstring().find(L'\0') != std::wstring::npos || part == L".." || part == L"." || part.wstring().find_first_of(L":\"<>|?*\r\n") != std::wstring::npos ||
            part.wstring().back() == L'.' || part.wstring().back() == L' ') {
            return false;
        }
    }
    return true;
}

void appendU32(std::vector<uint8_t>& buf, uint32_t v) {
    const uint8_t b[4] = {
        static_cast<uint8_t>(v & 0xFFu),
        static_cast<uint8_t>((v >> 8u) & 0xFFu),
        static_cast<uint8_t>((v >> 16u) & 0xFFu),
        static_cast<uint8_t>((v >> 24u) & 0xFFu)};
    buf.insert(buf.end(), b, b + 4);
}

void appendU64(std::vector<uint8_t>& buf, uint64_t v) {
    for (unsigned s = 0; s < 64u; s += 8u) {
        buf.push_back(static_cast<uint8_t>((v >> s) & 0xFFu));
    }
}

bool readU32(const uint8_t* data, size_t total, size_t& pos, uint32_t& out) {
    if (pos + 4u > total) return false;
    out = static_cast<uint32_t>(data[pos]) |
          (static_cast<uint32_t>(data[pos + 1u]) << 8u) |
          (static_cast<uint32_t>(data[pos + 2u]) << 16u) |
          (static_cast<uint32_t>(data[pos + 3u]) << 24u);
    pos += 4u;
    return true;
}

bool readU64(const uint8_t* data, size_t total, size_t& pos, uint64_t& out) {
    if (pos + 8u > total) return false;
    out = 0;
    for (unsigned s = 0; s < 64u; s += 8u) {
        out |= (static_cast<uint64_t>(data[pos++]) << s);
    }
    return true;
}

bool readWholeFile(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto sz = f.tellg();
    if (sz < 0) return false;
    out.resize(static_cast<size_t>(sz));
    f.seekg(0, std::ios::beg);
    if (!out.empty() && !f.read(reinterpret_cast<char*>(out.data()), sz)) return false;
    return true;
}

bool writeWholeFile(const fs::path& path, const uint8_t* data, size_t len) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (len > 0 && !f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len))) {
        return false;
    }
    f.flush();
    return f.good();
}

// Build-time packer: `boo_installer_stub.exe --pack <stub.exe> <stage_dir> <out_setup.exe>`
int runPackMode(const fs::path& stubPath, const fs::path& stageDir, const fs::path& outSetupPath) {
    std::vector<uint8_t> stubBytes;
    if (!readWholeFile(stubPath, stubBytes) || stubBytes.empty()) {
        return 10;
    }

    std::error_code ec;
    if (!fs::exists(stageDir, ec) || !fs::is_directory(stageDir, ec)) {
        return 11;
    }

    std::vector<fs::path> relFiles;
    for (const auto& entry : fs::recursive_directory_iterator(stageDir, ec)) {
        if (ec) return 12;
        if (entry.is_regular_file(ec)) {
            relFiles.push_back(fs::relative(entry.path(), stageDir, ec));
        }
    }
    std::sort(relFiles.begin(), relFiles.end());
    if (relFiles.empty()) {
        return 13;
    }

    // Deterministic archive layout: [fileCount:u32] ([pathUtf8Len:u32][pathUtf8][fileBytesLen:u64][fileBytes])...
    std::vector<uint8_t> rawArchive;
    rawArchive.reserve(32 * 1024 * 1024);
    appendU32(rawArchive, static_cast<uint32_t>(relFiles.size()));

    for (const auto& rel : relFiles) {
        const std::string genericUtf8 = wideToUtf8(rel.generic_wstring());
        if (genericUtf8.empty()) return 14;
        std::vector<uint8_t> fileBytes;
        if (!readWholeFile(stageDir / rel, fileBytes)) return 15;

        appendU32(rawArchive, static_cast<uint32_t>(genericUtf8.size()));
        rawArchive.insert(rawArchive.end(), genericUtf8.begin(), genericUtf8.end());
        appendU64(rawArchive, static_cast<uint64_t>(fileBytes.size()));
        rawArchive.insert(rawArchive.end(), fileBytes.begin(), fileBytes.end());
    }

    COMPRESSOR_HANDLE compressor = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &compressor)) {
        return 16;
    }

    SIZE_T compressedCap = 0;
    Compress(compressor, rawArchive.data(), rawArchive.size(), nullptr, 0, &compressedCap);
    if (compressedCap == 0) {
        CloseCompressor(compressor);
        return 17;
    }

    std::vector<uint8_t> compressed(compressedCap);
    SIZE_T actualCompressed = 0;
    const BOOL compOk = Compress(
        compressor,
        rawArchive.data(),
        rawArchive.size(),
        compressed.data(),
        compressed.size(),
        &actualCompressed);
    CloseCompressor(compressor);
    if (!compOk || actualCompressed == 0) {
        return 18;
    }
    compressed.resize(actualCompressed);

    SfxFooter footer{};
    std::memcpy(footer.magic, kSfxMagic, sizeof(kSfxMagic));
    footer.payloadOffset     = static_cast<uint64_t>(stubBytes.size());
    footer.compressedBytes   = static_cast<uint64_t>(compressed.size());
    footer.uncompressedBytes = static_cast<uint64_t>(rawArchive.size());
    footer.fnv1a64           = computeFnv1a64(rawArchive.data(), rawArchive.size());

    if (outSetupPath.has_parent_path()) {
        fs::create_directories(outSetupPath.parent_path(), ec);
    }
    std::ofstream out(outSetupPath, std::ios::binary | std::ios::trunc);
    if (!out) return 19;
    out.write(reinterpret_cast<const char*>(stubBytes.data()), static_cast<std::streamsize>(stubBytes.size()));
    out.write(reinterpret_cast<const char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
    out.write(reinterpret_cast<const char*>(&footer), sizeof(footer));
    out.flush();
    return out.good() ? 0 : 20;
}

struct InstallOptions {
    fs::path installDir;
    bool silent = false;
    bool createStartMenuShortcut = true;
    bool createDesktopShortcut = true;
    bool registerUninstall = true;
    bool launchAfterInstall = false;
    bool noActivateLaunch = false;
    bool preserveConfig = true;
};

void writeUninstallRegistry(const fs::path& installDir, uint64_t totalInstalledBytes) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return;
    }
    auto setStr = [&](const wchar_t* name, const std::wstring& val) {
        RegSetValueExW(
            hKey,
            name,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(val.c_str()),
            static_cast<DWORD>((val.size() + 1u) * sizeof(wchar_t)));
    };
    auto setDword = [&](const wchar_t* name, DWORD val) {
        RegSetValueExW(hKey, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(DWORD));
    };

    const fs::path appExe       = installDir / kAppExeName;
    const fs::path uninstallExe = installDir / kUninstallExe;
    const std::wstring uninstCmd      = L"\"" + uninstallExe.wstring() + L"\" --uninstall";
    const std::wstring quietUninstCmd = L"\"" + uninstallExe.wstring() + L"\" --uninstall --silent";

    setStr(L"DisplayName", kAppTitle);
    setStr(L"DisplayVersion", L"" BOO_RELEASE_VERSION);
    setStr(L"Publisher", L"BOO SAH SAH Project");
    setStr(L"InstallLocation", installDir.wstring());
    setStr(L"DisplayIcon", appExe.wstring());
    setStr(L"UninstallString", uninstCmd);
    setStr(L"QuietUninstallString", quietUninstCmd);
    setDword(L"NoModify", 1);
    setDword(L"NoRepair", 1);
    setDword(L"EstimatedSize", static_cast<DWORD>(std::max<uint64_t>(1u, totalInstalledBytes / 1024ULL)));
    RegCloseKey(hKey);
}

// Reject junctions/symlinks in every existing ancestor before modifying a destination.
bool safeDestination(const fs::path& path) {
    for (fs::path p = fs::absolute(path).lexically_normal(); !p.empty();) {
        const DWORD attrs = GetFileAttributesW(p.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return true;
}

bool appIsRunning(const fs::path& directory) {
    const auto executable = directory / kAppExeName;
    std::error_code ec;
    if (!fs::exists(executable, ec) && !ec) return false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true; // Cannot prove a safe upgrade.
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool running = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kAppExeName) != 0) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) {
                if (GetLastError() == ERROR_INVALID_PARAMETER) continue; // Exited during enumeration.
                running = true;
                break;
            }
            std::wstring image(32768, L'\0');
            DWORD length = static_cast<DWORD>(image.size());
            const BOOL queried = QueryFullProcessImageNameW(process, 0, image.data(), &length);
            CloseHandle(process);
            image.resize(length);
            if (!queried || fs::equivalent(executable, image, ec) || ec) {
                running = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return running;
}

struct InstallProgress {
    std::atomic<bool> cancel{false};
    std::function<void(int)> report;
    bool checkpoint(std::wstring& error) const {
        if (!cancel.load()) return true;
        error = L"Installation cancelled. Previous files have been restored.";
        return false;
    }
    void update(int percent) const { if (report) report(percent); }
};

struct SetupLock {
    HANDLE handle = CreateMutexW(nullptr, FALSE, L"Local\\BooSahSah.SetupTransaction");
    bool acquired = false;
    SetupLock() {
        if (handle) { const DWORD result = WaitForSingleObject(handle, 0); acquired = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED; }
    }
    ~SetupLock() { if (acquired) ReleaseMutex(handle); if (handle) CloseHandle(handle); }
};

// Files are journaled on the destination volume before replacement. A failed write,
// exception, or Cancel restores old files and removes only newly-created paths.
struct InstallTransaction {
    fs::path root, backup;
    std::wstring& error;
    struct Entry { fs::path relative; bool hadOriginal; };
    std::vector<Entry> journal;
    std::vector<fs::path> createdDirs;
    bool committed = false;
    bool rollbackFailed = false;

    InstallTransaction(fs::path destination, std::wstring& report) : root(std::move(destination)), error(report) {}
    bool makeDirectories(const fs::path& dir) {
        std::error_code ec;
        if (fs::exists(dir, ec)) return fs::is_directory(dir, ec);
        if (dir.empty() || dir == dir.parent_path() || !makeDirectories(dir.parent_path())) return false;
        if (!fs::create_directory(dir, ec)) return false;
        createdDirs.push_back(dir);
        return true;
    }
    bool begin() {
        if (!safeDestination(root) || !makeDirectories(root)) return false;
        // Exclusive name: never reuse or delete somebody else's transaction folder.
        for (unsigned n = 0; n < 100; ++n) {
            backup = root / (L".boo-rollback-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(n));
            std::error_code ec;
            if (fs::create_directory(backup, ec)) return true;
        }
        backup.clear();
        return false;
    }
    bool stash(const fs::path& relative) {
        const fs::path dest = root / relative;
        if (!isSafeRelativeArchivePath(relative) || !safeDestination(dest)) return false;
        std::error_code ec;
        const bool exists = fs::exists(dest, ec);
        if (ec || (exists && !fs::is_regular_file(dest, ec))) return false;
        journal.push_back({relative, exists}); // Allocate before moving the only original copy.
        if (exists) {
            fs::create_directories((backup / relative).parent_path(), ec);
            if (ec) { journal.pop_back(); return false; }
            fs::rename(dest, backup / relative, ec);
            if (ec) { journal.pop_back(); return false; }
        }
        return true;
    }
    bool write(const fs::path& relative, const uint8_t* bytes, size_t size) {
        if (!stash(relative) || !makeDirectories((root / relative).parent_path())) return false;
        return writeWholeFile(root / relative, bytes, size);
    }
    void finish() noexcept {
        std::error_code ec;
        if (!committed) {
            for (auto it = journal.rbegin(); it != journal.rend(); ++it) {
                fs::remove(root / it->relative, ec);
                if (ec) rollbackFailed = true;
                if (it->hadOriginal) {
                    fs::rename(backup / it->relative, root / it->relative, ec);
                    if (ec) rollbackFailed = true;
                }
            }
        }
        // Retain backups if restore failed (e.g. another process locked a file).
        if (!backup.empty() && !rollbackFailed) fs::remove_all(backup, ec);
        if (rollbackFailed) error = L"Some files could not be restored. Close other applications and recover the original files from: " + backup.wstring();
        if (!committed) for (auto it = createdDirs.rbegin(); it != createdDirs.rend(); ++it) fs::remove(*it, ec);
        journal.clear();
    }
    ~InstallTransaction() { finish(); }
};

bool performInstall(const InstallOptions& opts, std::wstring& outError, InstallProgress* suppliedProgress = nullptr) {
    SetupLock lock;
    if (!lock.acquired) { outError = L"Another BOO SAH SAH setup is running. Wait for it to finish and try again."; return false; }
    InstallProgress localProgress;
    InstallProgress& progress = suppliedProgress ? *suppliedProgress : localProgress;
    if (appIsRunning(opts.installDir)) {
        outError = L"Close BOO SAH SAH before installing or upgrading, then try again.";
        return false;
    }
    if (!opts.installDir.is_absolute() || opts.installDir == opts.installDir.root_path() || !safeDestination(opts.installDir)) {
        outError = L"Choose an absolute application folder without symbolic links or junctions.";
        return false;
    }
    if (!progress.checkpoint(outError)) return false;
    progress.update(-1); // Inbox LZMS is indivisible; marquee until decompression completes.

    const fs::path selfExe = currentExecutablePath();
    std::ifstream in(selfExe, std::ios::binary | std::ios::ate);
    if (!in) {
        outError = L"Unable to open setup executable for reading.";
        return false;
    }
    const int64_t totalExeBytes = static_cast<int64_t>(in.tellg());
    if (totalExeBytes <= static_cast<int64_t>(sizeof(SfxFooter))) {
        outError = L"Setup payload footer is missing.";
        return false;
    }

    SfxFooter footer{};
    in.seekg(totalExeBytes - static_cast<int64_t>(sizeof(SfxFooter)), std::ios::beg);
    if (!in.read(reinterpret_cast<char*>(&footer), sizeof(footer)) ||
        std::memcmp(footer.magic, kSfxMagic, sizeof(kSfxMagic)) != 0) {
        outError = L"Invalid or missing BOO SAH SAH installer payload.";
        return false;
    }

    if (footer.payloadOffset > static_cast<uint64_t>(totalExeBytes) - sizeof(SfxFooter) ||
        footer.compressedBytes != static_cast<uint64_t>(totalExeBytes) - sizeof(SfxFooter) - footer.payloadOffset ||
        footer.uncompressedBytes == 0 || footer.uncompressedBytes > (1024ULL * 1024ULL * 1024ULL)) {
        outError = L"Installer payload bounds validation failed.";
        return false;
    }

    std::vector<uint8_t> stubBytes(static_cast<size_t>(footer.payloadOffset));
    in.seekg(0, std::ios::beg);
    if (!in.read(reinterpret_cast<char*>(stubBytes.data()), static_cast<std::streamsize>(stubBytes.size()))) {
        outError = L"Failed reading uninstaller stub image.";
        return false;
    }

    std::vector<uint8_t> compressed(static_cast<size_t>(footer.compressedBytes));
    in.seekg(static_cast<std::streamoff>(footer.payloadOffset), std::ios::beg);
    if (!in.read(reinterpret_cast<char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()))) {
        outError = L"Failed reading compressed setup payload.";
        return false;
    }
    in.close();

    DECOMPRESSOR_HANDLE decomp = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &decomp)) {
        outError = L"Windows LZMS decompressor initialization failed.";
        return false;
    }

    std::vector<uint8_t> rawArchive(static_cast<size_t>(footer.uncompressedBytes));
    SIZE_T actualUncomp = 0;
    const BOOL decOk = Decompress(
        decomp,
        compressed.data(),
        compressed.size(),
        rawArchive.data(),
        rawArchive.size(),
        &actualUncomp);
    CloseDecompressor(decomp);
    compressed.clear();
    compressed.shrink_to_fit();

    if (!decOk || actualUncomp != rawArchive.size() ||
        computeFnv1a64(rawArchive.data(), rawArchive.size()) != footer.fnv1a64) {
        outError = L"Setup payload integrity checksum verification failed.";
        return false;
    }

    if (!progress.checkpoint(outError)) return false;
    progress.update(0);
    std::error_code ec;
    InstallTransaction transaction(opts.installDir, outError);
    if (!transaction.begin()) {
        outError = L"Cannot create destination folder: " + opts.installDir.wstring();
        return false;
    }

    size_t pos = 0;
    uint32_t fileCount = 0;
    if (!readU32(rawArchive.data(), rawArchive.size(), pos, fileCount) || fileCount == 0 || fileCount > 10000u) {
        outError = L"Malformed setup archive header.";
        return false;
    }

    std::vector<std::wstring> installedRelFiles;
    installedRelFiles.reserve(fileCount + 2u);
    uint64_t totalBytesWritten = 0;
    std::set<std::wstring> archivePaths;
    std::vector<fs::path> previousFiles;
    std::vector<fs::path> previousShortcuts;
    bool previouslyRegistered = false;
    {
        std::ifstream oldManifest(opts.installDir / kManifestFile);
        std::string line;
        while (std::getline(oldManifest, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("FILE=", 0) == 0) {
                fs::path rel(utf8ToWide(line.substr(5)));
                if (isSafeRelativeArchivePath(rel)) previousFiles.push_back(rel);
            } else if (line.rfind("SHORTCUT=", 0) == 0) {
                const fs::path shortcut(utf8ToWide(line.substr(9)));
                if (shortcut == knownShortcutFolder(FOLDERID_Programs) / L"BOO SAH SAH.lnk" ||
                    shortcut == knownShortcutFolder(FOLDERID_Desktop) / L"BOO SAH SAH.lnk") previousShortcuts.push_back(shortcut);
            } else if (line == "REGISTER_UNINSTALL=1") {
                previouslyRegistered = true;
            }
        }
    }

    for (uint32_t i = 0; i < fileCount; ++i) {
        if (!progress.checkpoint(outError)) return false;
        uint32_t pathLen = 0;
        if (!readU32(rawArchive.data(), rawArchive.size(), pos, pathLen) || pathLen == 0 || pathLen > 1024u ||
            pos + pathLen > rawArchive.size()) {
            outError = L"Malformed setup archive entry path.";
            return false;
        }
        const std::string relUtf8(reinterpret_cast<const char*>(rawArchive.data() + pos), pathLen);
        pos += pathLen;

        uint64_t dataLen = 0;
        if (!readU64(rawArchive.data(), rawArchive.size(), pos, dataLen) || dataLen > rawArchive.size() - pos) {
            outError = L"Malformed setup archive entry payload.";
            return false;
        }

        const fs::path relPath = fs::path(utf8ToWide(relUtf8)).make_preferred();
        if (!isSafeRelativeArchivePath(relPath)) {
            outError = L"Rejected unsafe archive path entry.";
            return false;
        }

        std::wstring key = relPath.wstring();
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (!archivePaths.insert(key).second || key == kManifestFile || key == L"uninstall-boosahsah.exe") {
            outError = L"Duplicate or reserved archive entry.";
            return false;
        }
        const fs::path destPath = opts.installDir / relPath;
        const bool keepConfig = opts.preserveConfig && _wcsicmp(relPath.begin()->c_str(), L"config") == 0 && fs::exists(destPath, ec);
        if (!keepConfig && !transaction.write(relPath, rawArchive.data() + pos, static_cast<size_t>(dataLen))) {
            outError = L"Failed writing file (ensure BOO SAH SAH is closed): " + destPath.wstring();
            return false;
        }

        if (!keepConfig) installedRelFiles.push_back(relPath.wstring());
        totalBytesWritten += dataLen;
        pos += static_cast<size_t>(dataLen);
        progress.update(static_cast<int>((i + 1ULL) * 90 / fileCount));
    }
    if (pos != rawArchive.size()) { outError = L"Trailing archive data."; return false; }

    // Write lightweight uninstaller stub (without multi-MB payload)
    if (!progress.checkpoint(outError)) return false;
    if (transaction.write(kUninstallExe, stubBytes.data(), stubBytes.size())) {
        installedRelFiles.push_back(kUninstallExe);
        totalBytesWritten += stubBytes.size();
    } else { outError = L"Could not write uninstaller."; return false; }

    // Remove obsolete files from earlier versions using the same rollback journal.
    for (const auto& rel : previousFiles) {
        std::wstring key = rel.wstring();
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (key == L"uninstall-boosahsah.exe" || key == kManifestFile || archivePaths.count(key) ||
            (opts.preserveConfig && _wcsicmp(rel.begin()->c_str(), L"config") == 0)) continue;
        if (fs::exists(opts.installDir / rel, ec) && !transaction.stash(rel)) {
            outError = L"Could not replace obsolete files; close the application and retry."; return false;
        }
        archivePaths.insert(key); // Do not journal duplicate entries in an old manifest twice.
    }
    if (!progress.checkpoint(outError)) return false;

    std::vector<fs::path> createdShortcuts;
    const fs::path appExePath = opts.installDir / kAppExeName;
    if (opts.createStartMenuShortcut) {
        const fs::path folder = knownShortcutFolder(FOLDERID_Programs);
        if (!folder.empty()) createdShortcuts.push_back(folder / L"BOO SAH SAH.lnk");
    }
    if (opts.createDesktopShortcut) {
        const fs::path folder = knownShortcutFolder(FOLDERID_Desktop);
        if (!folder.empty()) createdShortcuts.push_back(folder / L"BOO SAH SAH.lnk");
    }
    // Record exact installed files & shortcuts so uninstaller never deletes untracked user files
    {
        std::ostringstream manifest;
        manifest << "REGISTER_UNINSTALL=" << ((opts.registerUninstall || previouslyRegistered) ? "1" : "0") << "\n";
        for (const auto& rel : installedRelFiles) {
            manifest << "FILE=" << wideToUtf8(rel) << "\n";
        }
        for (const auto& sc : createdShortcuts) {
            manifest << "SHORTCUT=" << wideToUtf8(sc.wstring()) << "\n";
        }
        for (const auto& sc : previousShortcuts) {
            if (std::find(createdShortcuts.begin(), createdShortcuts.end(), sc) == createdShortcuts.end())
                manifest << "SHORTCUT=" << wideToUtf8(sc.wstring()) << "\n";
        }
        const std::string mstr = manifest.str();
        if (!transaction.write(
            kManifestFile,
            reinterpret_cast<const uint8_t*>(mstr.data()),
            mstr.size())) { outError = L"Could not save install manifest."; return false; }
    }
    transaction.committed = true;
    progress.update(95);
    for (const auto& shortcut : createdShortcuts) createShellShortcut(shortcut, appExePath, opts.installDir, kAppTitle);

    if (opts.registerUninstall) {
        writeUninstallRegistry(opts.installDir, totalBytesWritten);
    }

    if (opts.launchAfterInstall && fs::exists(appExePath, ec)) {
        std::wstring cmdLine = L"\"" + appExePath.wstring() + L"\"";
        if (opts.noActivateLaunch) {
            cmdLine += L" --no-activate";
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const std::wstring workDir = opts.installDir.wstring();
        if (CreateProcessW(
                appExePath.c_str(),
                cmdLine.data(),
                nullptr,
                nullptr,
                FALSE,
                0,
                nullptr,
                workDir.c_str(),
                &si,
                &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    progress.update(100);
    return true;
}

std::wstring quoteArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

// Defer only the running uninstaller image. Content removal and its exit status
// stay synchronous. Paths never pass through cmd.exe, so metacharacters are literal.
bool launchUninstallHelper(const fs::path& target) {
    wchar_t temp[MAX_PATH]{};
    wchar_t name[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp) || !GetTempFileNameW(temp, L"boo", 0, name)) return false;
    const fs::path helper(name);
    if (!CopyFileW(currentExecutablePath().c_str(), helper.c_str(), FALSE)) { DeleteFileW(name); return false; }
    std::wstring command = quoteArgument(helper.wstring()) + L" --cleanup-self " +
        std::to_wstring(GetCurrentProcessId()) + L" " + quoteArgument(target.wstring());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(helper.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, temp, &si, &pi);
    if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
    else DeleteFileW(name);
    return ok != FALSE;
}

int runUninstallMode(const fs::path& targetDir, bool silent, bool forceSkipRegistry) {
    SetupLock lock;
    if (!lock.acquired) return 8;
    if (!targetDir.is_absolute() || targetDir == targetDir.root_path() || !safeDestination(targetDir)) return 3;
    if (appIsRunning(targetDir)) {
        if (!silent) MessageBoxW(nullptr, L"Close BOO SAH SAH before uninstalling, then try again.", kSetupWindowTitle, MB_ICONWARNING | MB_OK);
        return 4;
    }
    const fs::path manifestPath = targetDir / kManifestFile;
    std::error_code ec;
    if (!fs::exists(manifestPath, ec)) {
        if (!silent) {
            MessageBoxW(nullptr, L"Install manifest not found in application directory.", kSetupWindowTitle, MB_ICONERROR | MB_OK);
        }
        return 2;
    }

    if (!silent) {
        const int ans = MessageBoxW(
            nullptr,
            L"Uninstall BOO SAH SAH \u2014 The Glass Orchid Protocol from your computer?",
            kSetupWindowTitle,
            MB_ICONQUESTION | MB_YESNO);
        if (ans != IDYES) return 0;
    }

    const fs::path selfPath = currentExecutablePath();
    const fs::path uninstallExe = targetDir / kUninstallExe;
    const bool deferSelfRemoval = _wcsicmp(selfPath.c_str(), uninstallExe.c_str()) == 0;

    bool registeredInRegistry = false;
    std::vector<fs::path> relFiles;
    std::vector<fs::path> shortcuts;

    {
        std::ifstream f(manifestPath);
        if (!f) return 2;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line == "REGISTER_UNINSTALL=1") {
                registeredInRegistry = true;
            } else if (line.rfind("FILE=", 0) == 0) {
                const fs::path rel = fs::path(utf8ToWide(line.substr(5))).make_preferred();
                if (isSafeRelativeArchivePath(rel)) relFiles.push_back(rel);
            } else if (line.rfind("SHORTCUT=", 0) == 0) {
                shortcuts.emplace_back(utf8ToWide(line.substr(9)));
            }
        }
        if (f.bad()) return 2;
    }

    const fs::path selfExe = fs::weakly_canonical(currentExecutablePath(), ec);

    // A manifest may only reference this product's two shortcut names.
    const fs::path startShortcut = knownShortcutFolder(FOLDERID_Programs) / L"BOO SAH SAH.lnk";
    const fs::path desktopShortcut = knownShortcutFolder(FOLDERID_Desktop) / L"BOO SAH SAH.lnk";
    for (const auto& sc : shortcuts) {
        if ((sc == startShortcut || sc == desktopShortcut) && safeDestination(sc)) fs::remove(sc, ec);
    }

    bool removalFailed = false;
    std::set<fs::path> installedDirectories;
    for (const auto& rel : relFiles) {
        if (_wcsicmp(rel.c_str(), kUninstallExe) == 0) continue; // Keep a retry route until all content was removed.
        const fs::path full = targetDir / rel;
        const fs::path canon = fs::weakly_canonical(full, ec);
        if (!selfExe.empty() && canon == selfExe) {
            removalFailed = true;
            continue;
        }
        if (!safeDestination(full)) { removalFailed = true; continue; }
        fs::remove(full, ec);
        if (ec) removalFailed = true;
        for (auto parent = rel.parent_path(); !parent.empty(); parent = parent.parent_path())
            installedDirectories.insert(parent);
    }
    if (removalFailed) return 6; // Keep manifest so the uninstall can be retried.
    if (!safeDestination(uninstallExe)) return 6;
    if (deferSelfRemoval) {
        // Start before removing the manifest so a failed helper launch is retryable.
        // The helper preserves this executable if manifest removal below fails.
        if (!launchUninstallHelper(targetDir)) return 5;
    } else {
        fs::remove(uninstallExe, ec);
        if (ec) return 6;
    }
    fs::remove(manifestPath, ec);
    if (ec) return 6;

    // Component-wise path ordering puts descendants after their parents.
    // Prune from the leaves, and preserve directories that still contain user files.
    for (auto it = installedDirectories.rbegin(); it != installedDirectories.rend(); ++it) {
        const fs::path p = targetDir / *it;
        if (safeDestination(p) && fs::is_directory(p, ec) && fs::is_empty(p, ec)) fs::remove(p, ec);
    }

    if (registeredInRegistry && !forceSkipRegistry) {
        RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallRegKey);
    }

    if (fs::exists(targetDir, ec) && fs::is_empty(targetDir, ec)) fs::remove(targetDir, ec);

    if (!silent) {
        MessageBoxW(nullptr, L"BOO SAH SAH has been uninstalled.", kSetupWindowTitle, MB_ICONINFORMATION | MB_OK);
    }
    return 0;
}

// Interactive Win32 Setup Dialog
enum DialogCtrlId : int {
    kIdEditDir       = 101,
    kIdChkStartMenu  = 102,
    kIdChkDesktop    = 103,
    kIdChkLaunch     = 104,
    kIdLblStatus     = 105,
    kIdBtnInstall    = 106,
    kIdBtnCancel     = 107,
    kIdChkPreserve   = 108
};

struct GuiSetupState {
    InstallOptions opts{};
    HWND hwndEditDir = nullptr;
    HWND hwndChkStartMenu = nullptr;
    HWND hwndChkDesktop = nullptr;
    HWND hwndChkLaunch = nullptr;
    HWND hwndStatus = nullptr;
    HWND hwndBtnInstall = nullptr;
    HWND hwndBtnCancel = nullptr;
    HWND hwndChkPreserve = nullptr;
    HWND hwndProgress = nullptr;
    std::thread worker;
    InstallProgress progress;
    bool working = false;
    bool succeeded = false;
    std::wstring error;
    HFONT hFontTitle = nullptr;
    HFONT hFontBody = nullptr;
    int exitCode = 0;
};

constexpr UINT kProgressMessage = WM_APP + 1;
constexpr UINT kFinishedMessage = WM_APP + 2;

LRESULT CALLBACK setupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<GuiSetupState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        st = reinterpret_cast<GuiSetupState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

        st->hFontTitle = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        st->hFontBody  = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

        HWND hTitle = CreateWindowExW(0, L"STATIC", kAppTitle, WS_CHILD | WS_VISIBLE,
                                      24, 18, 500, 28, hwnd, nullptr, cs->hInstance, nullptr);
        SendMessageW(hTitle, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFontTitle), TRUE);

        HWND hSub = CreateWindowExW(
            0, L"STATIC",
            L"Single-app Windows 11 Direct3D 12 demoscene installation (no administrator elevation required).",
            WS_CHILD | WS_VISIBLE, 24, 48, 500, 22, hwnd, nullptr, cs->hInstance, nullptr);
        SendMessageW(hSub, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFontBody), TRUE);

        HWND hDirLbl = CreateWindowExW(0, L"STATIC", L"Install folder:", WS_CHILD | WS_VISIBLE,
                                       24, 84, 200, 20, hwnd, nullptr, cs->hInstance, nullptr);
        SendMessageW(hDirLbl, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFontBody), TRUE);

        st->hwndEditDir = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", st->opts.installDir.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            24, 106, 496, 26, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEditDir)), cs->hInstance, nullptr);
        SendMessageW(st->hwndEditDir, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFontBody), TRUE);

        st->hwndChkStartMenu = CreateWindowExW(
            0, L"BUTTON", L"Create Start Menu shortcut",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            24, 146, 320, 22, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkStartMenu)), cs->hInstance, nullptr);
        SendMessageW(st->hwndChkStartMenu, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFontBody), TRUE);
        SendMessageW(st->hwndChkStartMenu, BM_SETCHECK, BST_CHECKED, 0);

        st->hwndChkDesktop = CreateWindowExW(
            0, L"BUTTON", L"Create Desktop shortcut",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            24, 174, 320, 22, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkDesktop)), cs->hInstance, nullptr);
        SendMessageW(st->hwndChkDesktop, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFontBody), TRUE);
        SendMessageW(st->hwndChkDesktop, BM_SETCHECK, BST_CHECKED, 0);

        st->hwndChkLaunch = CreateWindowExW(
            0, L"BUTTON", L"Launch BOO SAH SAH when setup finishes",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            24, 202, 360, 22, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkLaunch)), cs->hInstance, nullptr);
        SendMessageW(st->hwndChkLaunch, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFontBody), TRUE);
        SendMessageW(st->hwndChkLaunch, BM_SETCHECK, BST_CHECKED, 0);

        st->hwndChkPreserve = CreateWindowExW(0, L"BUTTON", L"Keep existing config overrides when upgrading",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            24, 230, 490, 22, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdChkPreserve)), cs->hInstance, nullptr);
        SendMessageW(st->hwndChkPreserve, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFontBody), TRUE);
        SendMessageW(st->hwndChkPreserve, BM_SETCHECK, st->opts.preserveConfig ? BST_CHECKED : BST_UNCHECKED, 0);
        st->hwndProgress = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            24, 265, 496, 18, hwnd, nullptr, cs->hInstance, nullptr);
        SendMessageW(st->hwndProgress, PBM_SETRANGE32, 0, 100);

        st->hwndStatus = CreateWindowExW(
            0, L"STATIC", L"Ready to install.", WS_CHILD | WS_VISIBLE,
            24, 294, 496, 40, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdLblStatus)), cs->hInstance, nullptr);
        SendMessageW(st->hwndStatus, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFontBody), TRUE);

        st->hwndBtnInstall = CreateWindowExW(
            0, L"BUTTON", L"Install",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            326, 340, 92, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnInstall)), cs->hInstance, nullptr);
        SendMessageW(st->hwndBtnInstall, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFontBody), TRUE);

        st->hwndBtnCancel = CreateWindowExW(
            0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            428, 340, 92, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBtnCancel)), cs->hInstance, nullptr);
        SendMessageW(st->hwndBtnCancel, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFontBody), TRUE);
        return 0;
    }

    case WM_COMMAND: {
        if (!st) break;
        const int id = LOWORD(wParam);
        if (id == kIdBtnCancel) {
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (id == kIdBtnInstall) {
            if (st->working) return 0;
            wchar_t dirBuf[2048]{};
            GetWindowTextW(st->hwndEditDir, dirBuf, 2048);
            st->opts.installDir = fs::path(dirBuf);
            if (st->opts.installDir.empty()) {
                MessageBoxW(hwnd, L"Please enter a valid destination folder.", kSetupWindowTitle, MB_ICONWARNING | MB_OK);
                return 0;
            }
            st->opts.createStartMenuShortcut = (SendMessageW(st->hwndChkStartMenu, BM_GETCHECK, 0, 0) == BST_CHECKED);
            st->opts.createDesktopShortcut   = (SendMessageW(st->hwndChkDesktop, BM_GETCHECK, 0, 0) == BST_CHECKED);
            st->opts.launchAfterInstall      = (SendMessageW(st->hwndChkLaunch, BM_GETCHECK, 0, 0) == BST_CHECKED);

            st->opts.preserveConfig = SendMessageW(st->hwndChkPreserve, BM_GETCHECK, 0, 0) == BST_CHECKED;
            st->working = true;
            st->progress.cancel.store(false);
            st->error.clear();
            for (HWND control : {st->hwndBtnInstall, st->hwndEditDir, st->hwndChkStartMenu,
                    st->hwndChkDesktop, st->hwndChkLaunch, st->hwndChkPreserve}) EnableWindow(control, FALSE);
            SetWindowTextW(st->hwndStatus, L"Decompressing and checking payload...");
            st->progress.report = [hwnd](int percent) { PostMessageW(hwnd, kProgressMessage, static_cast<WPARAM>(percent), 0); };
            try {
                st->worker = std::thread([st, hwnd] {
                    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    try { st->succeeded = performInstall(st->opts, st->error, &st->progress); }
                    catch (...) { st->succeeded = false; if (st->error.empty()) st->error = L"Setup could not complete. Previous files were restored where possible."; }
                    if (SUCCEEDED(hr)) CoUninitialize();
                    PostMessageW(hwnd, kFinishedMessage, 0, 0);
                });
            } catch (...) {
                st->succeeded = false;
                st->error = L"Unable to start setup worker.";
                PostMessageW(hwnd, kFinishedMessage, 0, 0);
            }
            return 0;
        }
        break;
    }

    case kProgressMessage:
        if (st) {
            const int percent = static_cast<int>(wParam);
            LONG_PTR style = GetWindowLongPtrW(st->hwndProgress, GWL_STYLE);
            SetWindowLongPtrW(st->hwndProgress, GWL_STYLE, percent < 0 ? style | PBS_MARQUEE : style & ~PBS_MARQUEE);
            SendMessageW(st->hwndProgress, PBM_SETMARQUEE, percent < 0, 30);
            if (percent >= 0) SendMessageW(st->hwndProgress, PBM_SETPOS, percent, 0);
            if (!st->progress.cancel.load() && percent >= 0) SetWindowTextW(st->hwndStatus,
                percent < 95 ? L"Extracting files..." : L"Finishing installation...");
        }
        return 0;
    case kFinishedMessage:
        if (st) {
            if (st->worker.joinable()) st->worker.join();
            st->working = false;
            if (st->succeeded) { st->exitCode = 0; DestroyWindow(hwnd); }
            else if (st->progress.cancel.load() && st->error.rfind(L"Installation cancelled.", 0) == 0) { st->exitCode = ERROR_CANCELLED; DestroyWindow(hwnd); }
            else {
                st->exitCode = 1;
                SetWindowTextW(st->hwndStatus, L"Installation failed. You can correct the problem and retry.");
                MessageBoxW(hwnd, st->error.c_str(), kSetupWindowTitle, MB_ICONERROR | MB_OK);
                for (HWND control : {st->hwndBtnInstall, st->hwndBtnCancel, st->hwndEditDir, st->hwndChkStartMenu,
                        st->hwndChkDesktop, st->hwndChkLaunch, st->hwndChkPreserve}) EnableWindow(control, TRUE);
            }
        }
        return 0;
    case WM_CLOSE:
        if (st && st->working) {
            st->progress.cancel.store(true);
            EnableWindow(st->hwndBtnCancel, FALSE);
            SetWindowTextW(st->hwndStatus, L"Cancelling safely... waiting for the current operation, then restoring files.");
        } else DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (st) {
            if (st->hFontTitle) DeleteObject(st->hFontTitle);
            if (st->hFontBody) DeleteObject(st->hFontBody);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int runInteractiveSetup(HINSTANCE hInstance, const InstallOptions& initialOpts) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);

    GuiSetupState st{};
    st.opts = initialOpts;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = setupWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    wc.hIconSm       = wc.hIcon;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"BooSahSahSetupWnd";
    RegisterClassExW(&wc);

    RECT rc{0, 0, 548, 392};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rc, style, FALSE);
    const int winW = rc.right - rc.left;
    const int winH = rc.bottom - rc.top;
    const int posX = std::max(0, (GetSystemMetrics(SM_CXSCREEN) - winW) / 2);
    const int posY = std::max(0, (GetSystemMetrics(SM_CYSCREEN) - winH) / 2);

    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        kSetupWindowTitle,
        style,
        posX,
        posY,
        winW,
        winH,
        nullptr,
        nullptr,
        hInstance,
        &st);
    if (!hwnd) return 1;

    ShowWindow(hwnd, initialOpts.noActivateLaunch ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return st.exitCode;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }
        LocalFree(argv);
    }

    // Build-time packer invocation: `boo_installer_stub.exe --pack <stub.exe> <stage_dir> <out_setup.exe>`
    if (args.size() == 4u && args[0] == L"--pack") {
        return runPackMode(fs::path(args[1]), fs::path(args[2]), fs::path(args[3]));
    }

    if (args.size() == 3 && args[0] == L"--cleanup-self") {
        HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(wcstoul(args[1].c_str(), nullptr, 10)));
        int result = 0;
        if (parent) {
            const DWORD wait = WaitForSingleObject(parent, INFINITE);
            CloseHandle(parent);
            if (wait != WAIT_OBJECT_0) result = 7;
        } else if (GetLastError() != ERROR_INVALID_PARAMETER) result = 7;
        if (result == 0) {
            SetupLock lock;
            const fs::path target(args[2]);
            std::error_code ec;
            if (!lock.acquired) result = 8;
            else if (!target.is_absolute() || target == target.root_path() || !safeDestination(target / kUninstallExe)) result = 3;
            // A remaining/new manifest means failure or a reinstall: keep its retry route.
            else if (!fs::exists(target / kManifestFile, ec) && !ec) {
                fs::remove(target / kUninstallExe, ec);
                if (ec) result = 6;
                else if (fs::is_empty(target, ec) && !ec) fs::remove(target, ec);
            }
        }
        // Windows 11 POSIX disposition can unlink this temporary executable while mapped.
        HANDLE self = CreateFileW(currentExecutablePath().c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (self != INVALID_HANDLE_VALUE) {
            FILE_DISPOSITION_INFO_EX disposition{};
            disposition.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS | FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
            SetFileInformationByHandle(self, FileDispositionInfoEx, &disposition, sizeof(disposition));
            CloseHandle(self);
        }
        return result;
    }

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    InstallOptions opts{};
    opts.installDir = defaultInstallDirectory();
    bool uninstallMode = false;
    bool forceSkipRegistry = false;

    const std::wstring selfStem = currentExecutablePath().stem().wstring();
    if (selfStem.rfind(L"Uninstall", 0) == 0 || selfStem.rfind(L"uninstall", 0) == 0) {
        uninstallMode = true;
    }

    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == L"--silent" || a == L"/S" || a == L"/quiet") {
            opts.silent = true;
        } else if (a == L"--uninstall") {
            uninstallMode = true;
        } else if (a == L"--install-dir" && i + 1u < args.size()) {
            opts.installDir = fs::path(args[++i]);
        } else if (a == L"--preserve-config") {
            opts.preserveConfig = true;
        } else if (a == L"--replace-config") {
            opts.preserveConfig = false;
        } else if (a == L"--no-shortcuts") {
            opts.createStartMenuShortcut = false;
            opts.createDesktopShortcut = false;
        } else if (a == L"--no-registry") {
            opts.registerUninstall = false;
            forceSkipRegistry = true;
        } else if (a == L"--launch") {
            opts.launchAfterInstall = true;
        } else if (a == L"--no-launch") {
            opts.launchAfterInstall = false;
        } else if (a == L"--no-activate") {
            opts.noActivateLaunch = true;
        }
    }

    int rc = 0;
    if (uninstallMode) {
        const fs::path targetDir = (selfStem.rfind(L"Uninstall", 0) == 0)
                                       ? currentExecutablePath().parent_path()
                                       : opts.installDir;
        rc = runUninstallMode(targetDir, opts.silent, forceSkipRegistry);
        if (!opts.silent && rc != 0 && rc != 2 && rc != 4) {
            MessageBoxW(nullptr, L"Uninstall could not finish. Close other applications using the installed files, then try again.",
                kSetupWindowTitle, MB_OK | MB_ICONERROR);
        }
    } else if (opts.silent) {
        std::wstring err;
        try { rc = performInstall(opts, err) ? 0 : 1; }
        catch (...) { rc = 1; }
    } else {
        rc = runInteractiveSetup(hInstance, opts);
    }

    if (SUCCEEDED(comHr)) {
        CoUninitialize();
    }
    return rc;
}
