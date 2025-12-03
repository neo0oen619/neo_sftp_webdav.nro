#include <fstream>
#include <regex>
#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <switch.h>
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>
#include "common.h"
#include "clients/remote_client.h"
#include "clients/webdav.h"
#include "pugixml/pugiext.hpp"
#include "fs.h"
#include "lang.h"
#include "config.h"
#include "util.h"
#include "windows.h"
#include "logger.h"

static const char *months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

namespace
{
    struct SplitFileWriter;

    static std::string SanitizePathComponent(const std::string &name)
    {
        // Preserve extension (e.g. ".nsp") but aggressively sanitize the
        // basename so local filesystem constraints (invalid UTF-8, symbols)
        // don't cause mkdir() failures for large DBI-style split folders.
        std::string base = name;
        std::string ext;
        std::size_t dot = name.find_last_of('.');
        if (dot != std::string::npos && dot > 0)
        {
            base = name.substr(0, dot);
            ext = name.substr(dot);
        }

        std::string safe;
        safe.reserve(base.size());
        for (unsigned char ch : base)
        {
            if (std::isalnum(ch) || ch == ' ' || ch == '-' || ch == '_' ||
                ch == '[' || ch == ']' || ch == '(' || ch == ')' || ch == '+')
            {
                safe.push_back(static_cast<char>(ch));
            }
            else
            {
                safe.push_back('_');
            }
        }

        // Trim spaces/underscores from both ends to avoid awkward names.
        auto is_trim = [](char c) { return c == ' ' || c == '_'; };
        while (!safe.empty() && is_trim(safe.front()))
            safe.erase(safe.begin());
        while (!safe.empty() && is_trim(safe.back()))
            safe.pop_back();

        if (safe.empty())
            safe = "file";

        // Keep path component reasonably short.
        const std::size_t max_base = 80;
        if (safe.size() > max_base)
            safe.resize(max_base);

        return safe + ext;
    }

    static bool EnsureDirectoryTree(const std::string &directory)
    {
        if (directory.empty() || directory == "/")
            return true;

        std::string current;
        for (char ch : directory)
        {
            current.push_back(ch);
            if (ch == '/' && current.size() > 1)
            {
                if (mkdir(current.c_str(), 0777) != 0 && errno != EEXIST)
                {
                    Logger::Logf("WEBDAV MKDIR failed path=%s errno=%d", current.c_str(), errno);
                    // Keep going; final stat check below will decide success.
                }
            }
        }

        if (!directory.empty() && directory.back() != '/')
        {
            if (mkdir(directory.c_str(), 0777) != 0 && errno != EEXIST)
            {
                Logger::Logf("WEBDAV MKDIR failed path=%s errno=%d", directory.c_str(), errno);
            }
        }

        struct stat st = {0};
        if (stat(directory.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            return true;

        return false;
    }

    static bool EnsureParentDirectory(const std::string &filePath)
    {
        size_t pos = filePath.find_last_of('/');
        if (pos == std::string::npos || pos == 0)
        {
            // If there is no slash or it's just the root ("/file"), fall back
            // to a known-writable base under the app data directory.
            std::string parent = std::string(DATA_PATH) + "/downloads";
            return EnsureDirectoryTree(parent);
        }

        std::string parent = filePath.substr(0, pos);
        // If creating the requested parent directory fails, fall back to a
        // safe downloads directory under the app data path so that large
        // transfers can still succeed even when the chosen local directory
        // is not writable (e.g. a custom root like "/Download").
        if (EnsureDirectoryTree(parent))
            return true;

        Logger::Logf("WEBDAV MKDIR parent failed for path=%s, falling back to DATA_PATH/downloads", filePath.c_str());
        std::string fallback = std::string(DATA_PATH) + "/downloads";
        return EnsureDirectoryTree(fallback);
    }

    struct WebDAVChunkTask
    {
        std::string url;
        std::string user;
        std::string pass;
        std::string rangeHeader;
        CHTTPClient *client = nullptr;
        int64_t start = 0;
        int64_t end = 0;
        long httpCode = 0;
        std::string err;
        std::string data;
        bool ok = false;
    };

    struct WebDAVParallelContext
    {
        std::string url;
        std::string outputPath;
        FILE *file = nullptr;
        int64_t size = 0;
        int64_t chunk_size = 0;

        std::mutex stateMutex;
        std::mutex fileMutex;

        int64_t nextOffset = 0;
        bool hadError = false;
        long lastHttpCode = 0;
        std::string errorMessage;
    };

    struct WebDAVWorkerArgs
    {
        WebDAVParallelContext *ctx = nullptr;
        CHTTPClient *client = nullptr;
    };

    struct WebDAVParallelSplitContext
    {
        std::string url;
        SplitFileWriter *sink = nullptr;
        int64_t size = 0;
        int64_t chunk_size = 0;

        std::mutex stateMutex;
        std::mutex sinkMutex;

        int64_t nextOffset = 0;
        bool hadError = false;
        long lastHttpCode = 0;
        std::string errorMessage;
    };

    struct WebDAVSplitWorkerArgs
    {
        WebDAVParallelSplitContext *ctx = nullptr;
        CHTTPClient *client = nullptr;
    };

    struct SplitFileWriter
    {
        std::string basePath;
        std::string dirPath;
        uint64_t partSize;
        FILE *currentPart = nullptr;
        int currentIndex = -1;

        explicit SplitFileWriter(const std::string &base, uint64_t part_sz)
            : basePath(base), partSize(part_sz)
        {
            dirPath = basePath;
            if (!FS::hasEndSlash(dirPath.c_str()))
                dirPath.push_back('/');
        }

        ~SplitFileWriter()
        {
            close();
        }

        bool open()
        {
            // If a plain file with this name already exists (from a previous
            // non-split download attempt), remove it so we can create a
            // directory with the same path for DBI-style splits.
            if (FS::FileExists(basePath))
            {
                FS::Rm(basePath);
            }

            // Create the split directory (and parents) using a local helper
            // that walks the path and calls mkdir() directly, so we don't
            // depend on any existing MkDirs behaviour.
            if (!EnsureDirectoryTree(basePath))
            {
                Logger::Logf("WEBDAV GET split mkdirs failed base=%s", basePath.c_str());
                return false;
            }

            return true;
        }

        void close()
        {
            if (currentPart)
            {
                fclose(currentPart);
                currentPart = nullptr;
            }
            currentIndex = -1;
        }

        bool openPart(int index)
        {
            if (currentIndex == index && currentPart)
                return true;

            if (currentPart)
            {
                fclose(currentPart);
                currentPart = nullptr;
            }

            char name[16];
            std::snprintf(name, sizeof(name), "%02d", index);
            std::string partPath = dirPath + name;

            currentPart = std::fopen(partPath.c_str(), "r+b");
            if (!currentPart)
                currentPart = std::fopen(partPath.c_str(), "w+b");
            if (!currentPart)
            {
                Logger::Logf("WEBDAV SPLIT fopen failed path=%s errno=%d", partPath.c_str(), errno);
                return false;
            }

            currentIndex = index;
            return true;
        }

        bool write(uint64_t offset, const void *data, size_t size)
        {
            const unsigned char *ptr = static_cast<const unsigned char *>(data);
            uint64_t curOffset = offset;
            size_t remaining = size;

            while (remaining > 0)
            {
                uint64_t index = curOffset / partSize;
                uint64_t offsetInPart = curOffset % partSize;
                uint64_t spaceInPart = partSize - offsetInPart;
                size_t toWrite = remaining;
                if (toWrite > spaceInPart)
                    toWrite = static_cast<size_t>(spaceInPart);

                if (!openPart(static_cast<int>(index)))
                    return false;

                if (fseeko(currentPart, (off_t)offsetInPart, SEEK_SET) != 0)
                {
                    Logger::Logf("WEBDAV SPLIT fseeko failed base=%s index=%llu offsetInPart=%llu errno=%d",
                                 basePath.c_str(),
                                 static_cast<unsigned long long>(index),
                                 static_cast<unsigned long long>(offsetInPart),
                                 errno);
                    return false;
                }

                size_t written = std::fwrite(ptr, 1, toWrite, currentPart);
                if (written != toWrite)
                {
                    Logger::Logf("WEBDAV SPLIT fwrite failed base=%s index=%llu expected=%zu written=%zu errno=%d",
                                 basePath.c_str(),
                                 static_cast<unsigned long long>(index),
                                 toWrite,
                                 written,
                                 errno);
                    return false;
                }

                ptr += written;
                remaining -= written;
                curOffset += written;
            }

            return true;
        }
    };

    static int64_t GetSplitLocalSize(const std::string &basePath, uint64_t partSize)
    {
        int64_t total = 0;
        std::string dirPath = basePath;
        if (!FS::hasEndSlash(dirPath.c_str()))
            dirPath.push_back('/');

        for (int index = 0;; ++index)
        {
            char name[16];
            std::snprintf(name, sizeof(name), "%02d", index);
            std::string partPath = dirPath + name;
            int64_t sz = FS::GetSize(partPath);
            if (sz <= 0)
                break;
            total += sz;
            if ((uint64_t)sz < partSize)
                break;
        }
        return total;
    }

    void WebDAVChunkDownloadThread(void *argp)
    {
        WebDAVChunkTask *task = static_cast<WebDAVChunkTask *>(argp);
        if (!task)
            return;

        const int kMaxAttempts = 6;
        const int64_t kRetryDelayNs = 5000000000ll; // 5 seconds

        CHTTPClient *http = task->client;
        if (!http)
        {
            task->err = "internal error";
            task->httpCode = 0;
            task->ok = false;
            return;
        }

        for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
        {
            if (stop_activity)
            {
                task->err = lang_strings[STR_CANCEL_ACTION_MSG];
                task->httpCode = 0;
                task->ok = false;
                return;
            }

            CHTTPClient::HttpResponse res;
            CHTTPClient::HeadersMap headers;
            if (!task->rangeHeader.empty())
                headers["Range"] = task->rangeHeader;

            bool ok = http->Get(task->url, headers, res);
            task->httpCode = res.iCode;

            if (!ok)
            {
                task->err = res.errMessage;

                bool retryable = (task->httpCode == 0);
                Logger::Logf("WEBDAV GET parallel range error url=%s range=%s code=%ld err=%s attempt=%d/%d",
                             task->url.c_str(),
                             task->rangeHeader.c_str(),
                             task->httpCode,
                             task->err.c_str(),
                             attempt + 1,
                             kMaxAttempts);

                if (!retryable || attempt == kMaxAttempts - 1)
                {
                    task->ok = false;
                    return;
                }

                // Sleep in small slices so user cancel is responsive.
                const int slices = 50;
                const int64_t slice = kRetryDelayNs / slices;
                for (int s = 0; s < slices; ++s)
                {
                    if (stop_activity)
                    {
                        task->err = lang_strings[STR_CANCEL_ACTION_MSG];
                        task->httpCode = 0;
                        task->ok = false;
                        return;
                    }
                    svcSleepThread(slice);
                }
                continue;
            }

            if (res.iCode != 206)
            {
                task->err = "unexpected http code";
                Logger::Logf("WEBDAV GET parallel unexpected code url=%s range=%s code=%ld attempt=%d/%d",
                             task->url.c_str(),
                             task->rangeHeader.c_str(),
                             res.iCode,
                             attempt + 1,
                             kMaxAttempts);

                bool retryable = (res.iCode >= 500 && res.iCode < 600);
                if (!retryable || attempt == kMaxAttempts - 1)
                {
                    task->ok = false;
                    return;
                }

                const int slices = 50;
                const int64_t slice = kRetryDelayNs / slices;
                for (int s = 0; s < slices; ++s)
                {
                    if (stop_activity)
                    {
                        task->err = lang_strings[STR_CANCEL_ACTION_MSG];
                        task->httpCode = 0;
                        task->ok = false;
                        return;
                    }
                    svcSleepThread(slice);
                }
                continue;
            }

            if (res.strBody.empty())
            {
                task->err = "empty body";
                Logger::Logf("WEBDAV GET parallel empty body url=%s range=%s attempt=%d/%d",
                             task->url.c_str(),
                             task->rangeHeader.c_str(),
                             attempt + 1,
                             kMaxAttempts);

                if (attempt == kMaxAttempts - 1)
                {
                    task->ok = false;
                    return;
                }

                const int slices = 50;
                const int64_t slice = kRetryDelayNs / slices;
                for (int s = 0; s < slices; ++s)
                {
                    if (stop_activity)
                    {
                        task->err = lang_strings[STR_CANCEL_ACTION_MSG];
                        task->httpCode = 0;
                        task->ok = false;
                        return;
                    }
                    svcSleepThread(slice);
                }
                continue;
            }

            task->data.swap(res.strBody);
            task->ok = true;
            return;
        }
    }

    void WebDAVParallelWorkerThread(void *argp)
    {
        WebDAVWorkerArgs *args = static_cast<WebDAVWorkerArgs *>(argp);
        if (!args || !args->ctx || !args->client)
            return;

        WebDAVParallelContext *ctx = args->ctx;
        CHTTPClient *http = args->client;

        const int kMaxAttempts = 6;
        const int64_t kRetryDelayNs = 5000000000ll; // 5 seconds

        while (true)
        {
            int64_t start = 0;
            int64_t end = 0;

            {
                std::lock_guard<std::mutex> lock(ctx->stateMutex);
                if (ctx->hadError)
                    return;
                if (ctx->nextOffset >= ctx->size)
                    return;

                start = ctx->nextOffset;
                end = start + ctx->chunk_size - 1;
                if (end >= ctx->size)
                    end = ctx->size - 1;

                ctx->nextOffset = end + 1;
            }

            char range_header[64];
            std::snprintf(range_header, sizeof(range_header), "bytes=%lld-%lld",
                          static_cast<long long>(start),
                          static_cast<long long>(end));

            for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
            {
                if (stop_activity)
                {
                    std::lock_guard<std::mutex> lock(ctx->stateMutex);
                    if (!ctx->hadError)
                    {
                        ctx->hadError = true;
                        ctx->errorMessage = lang_strings[STR_CANCEL_ACTION_MSG];
                    }
                    return;
                }

                CHTTPClient::HttpResponse res;
                CHTTPClient::HeadersMap headers;
                headers["Range"] = range_header;

                bool ok = http->Get(ctx->url, headers, res);
                long httpCode = res.iCode;

                if (!ok)
                {
                    std::string err = res.errMessage;

                    bool retryable = (httpCode == 0);
                    Logger::Logf("WEBDAV GET parallel range error url=%s range=%s code=%ld err=%s attempt=%d/%d",
                                 ctx->url.c_str(),
                                 range_header,
                                 httpCode,
                                 err.c_str(),
                                 attempt + 1,
                                 kMaxAttempts);

                    if (!retryable || attempt == kMaxAttempts - 1)
                    {
                        std::lock_guard<std::mutex> lock(ctx->stateMutex);
                        if (!ctx->hadError)
                        {
                            ctx->hadError = true;
                            ctx->errorMessage = err;
                        }
                        return;
                    }

                    const int slices = 50;
                    const int64_t slice = kRetryDelayNs / slices;
                    for (int s = 0; s < slices; ++s)
                    {
                        if (stop_activity)
                        {
                            std::lock_guard<std::mutex> lock(ctx->stateMutex);
                            if (!ctx->hadError)
                            {
                                ctx->hadError = true;
                                ctx->errorMessage = lang_strings[STR_CANCEL_ACTION_MSG];
                            }
                            return;
                        }
                        svcSleepThread(slice);
                    }
                    continue;
                }

                if (res.iCode != 206)
                {
                    std::string err = "unexpected http code";
                    Logger::Logf("WEBDAV GET parallel unexpected code url=%s range=%s code=%ld attempt=%d/%d",
                                 ctx->url.c_str(),
                                 range_header,
                                 res.iCode,
                                 attempt + 1,
                                 kMaxAttempts);

                    bool retryable = (res.iCode >= 500 && res.iCode < 600);
                    if (!retryable || attempt == kMaxAttempts - 1)
                    {
                        std::lock_guard<std::mutex> lock(ctx->stateMutex);
                        if (!ctx->hadError)
                        {
                            ctx->hadError = true;
                            ctx->errorMessage = err;
                        }
                        return;
                    }

                    const int slices = 50;
                    const int64_t slice = kRetryDelayNs / slices;
                    for (int s = 0; s < slices; ++s)
                    {
                        if (stop_activity)
                        {
                            std::lock_guard<std::mutex> lock(ctx->stateMutex);
                            if (!ctx->hadError)
                            {
                                ctx->hadError = true;
                                ctx->errorMessage = lang_strings[STR_CANCEL_ACTION_MSG];
                            }
                            return;
                        }
                        svcSleepThread(slice);
                    }
                    continue;
                }

                if (res.strBody.empty())
                {
                    std::string err = "empty body";
                    Logger::Logf("WEBDAV GET parallel empty body url=%s range=%s attempt=%d/%d",
                                 ctx->url.c_str(),
                                 range_header,
                                 attempt + 1,
                                 kMaxAttempts);

                    if (attempt == kMaxAttempts - 1)
                    {
                        std::lock_guard<std::mutex> lock(ctx->stateMutex);
                        if (!ctx->hadError)
                        {
                            ctx->hadError = true;
                            ctx->errorMessage = err;
                        }
                        return;
                    }

                    const int slices = 50;
                    const int64_t slice = kRetryDelayNs / slices;
                    for (int s = 0; s < slices; ++s)
                    {
                        if (stop_activity)
                        {
                            std::lock_guard<std::mutex> lock(ctx->stateMutex);
                            if (!ctx->hadError)
                            {
                                ctx->hadError = true;
                                ctx->errorMessage = lang_strings[STR_CANCEL_ACTION_MSG];
                            }
                            return;
                        }
                        svcSleepThread(slice);
                    }
                    continue;
                }

                {
                    std::lock_guard<std::mutex> fileLock(ctx->fileMutex);
                    if (fseeko(ctx->file, (off_t)start, SEEK_SET) != 0)
                    {
                        std::lock_guard<std::mutex> lock(ctx->stateMutex);
                        if (!ctx->hadError)
                        {
                            ctx->hadError = true;
                            ctx->errorMessage = lang_strings[STR_FAIL_DOWNLOAD_MSG];
                        }
                        return;
                    }

                    size_t written = std::fwrite(res.strBody.data(), 1, res.strBody.size(), ctx->file);
                    if (written != res.strBody.size())
                    {
                        Logger::Logf("WEBDAV GET parallel write failed path=%s expected=%zu written=%zu",
                                     ctx->outputPath.c_str(),
                                     res.strBody.size(),
                                     written);
                        std::lock_guard<std::mutex> lock(ctx->stateMutex);
                        if (!ctx->hadError)
                        {
                            ctx->hadError = true;
                            ctx->errorMessage = lang_strings[STR_FAIL_DOWNLOAD_MSG];
                        }
                        return;
                    }

                    bytes_transfered += static_cast<int64_t>(written);
                    ctx->lastHttpCode = res.iCode;
                }

                break;
            }
        }
    }

    void WebDAVParallelSplitWorkerThread(void *argp)
    {
        WebDAVSplitWorkerArgs *args = static_cast<WebDAVSplitWorkerArgs *>(argp);
        if (!args || !args->ctx || !args->client)
            return;

        WebDAVParallelSplitContext *ctx = args->ctx;
        CHTTPClient *http = args->client;

        const int kMaxAttempts = 10;
        const int64_t kRetryDelayNs = 5000000000ll; // 5 seconds

        while (true)
        {
            int64_t start = 0;
            int64_t end = 0;

            {
                std::lock_guard<std::mutex> lock(ctx->stateMutex);
                if (ctx->hadError)
                    return;
                if (ctx->nextOffset >= ctx->size)
                    return;

                start = ctx->nextOffset;
                end = start + ctx->chunk_size - 1;
                if (end >= ctx->size)
                    end = ctx->size - 1;

                ctx->nextOffset = end + 1;
            }

            char range_header[64];
            std::snprintf(range_header, sizeof(range_header), "bytes=%lld-%lld",
                          static_cast<long long>(start),
                          static_cast<long long>(end));

            for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
            {
                if (stop_activity)
                {
                    std::lock_guard<std::mutex> lock(ctx->stateMutex);
                    if (!ctx->hadError)
                    {
                        ctx->hadError = true;
                        ctx->errorMessage = lang_strings[STR_CANCEL_ACTION_MSG];
                    }
                    return;
                }

                CHTTPClient::HttpResponse res;
                CHTTPClient::HeadersMap headers;
                headers["Range"] = range_header;

                bool ok = http->Get(ctx->url, headers, res);
                long httpCode = res.iCode;

                if (!ok)
                {
                    std::string err = res.errMessage;

                    bool retryable = (httpCode == 0);
                    Logger::Logf("WEBDAV GET split-parallel range error url=%s range=%s code=%ld err=%s attempt=%d/%d",
                                 ctx->url.c_str(),
                                 range_header,
                                 httpCode,
                                 err.c_str(),
                                 attempt + 1,
                                 kMaxAttempts);

                    if (!retryable || attempt == kMaxAttempts - 1)
                    {
                        std::lock_guard<std::mutex> lock(ctx->stateMutex);
                        if (!ctx->hadError)
                        {
                            ctx->hadError = true;
                            ctx->errorMessage = err;
                        }
                        return;
                    }

                    const int slices = 50;
                    const int64_t slice = kRetryDelayNs / slices;
                    for (int s = 0; s < slices; ++s)
                    {
                        if (stop_activity)
                        {
                            std::lock_guard<std::mutex> lock(ctx->stateMutex);
                            if (!ctx->hadError)
                            {
                                ctx->hadError = true;
                                ctx->errorMessage = lang_strings[STR_CANCEL_ACTION_MSG];
                            }
                            return;
                        }
                        svcSleepThread(slice);
                    }
                    continue;
                }

                if (res.iCode != 206)
                {
                    std::string err = "unexpected http code";
                    Logger::Logf("WEBDAV GET split-parallel unexpected code url=%s range=%s code=%ld attempt=%d/%d",
                                 ctx->url.c_str(),
                                 range_header,
                                 res.iCode,
                                 attempt + 1,
                                 kMaxAttempts);

                    bool retryable = (res.iCode >= 500 && res.iCode < 600);
                    if (!retryable || attempt == kMaxAttempts - 1)
                    {
                        std::lock_guard<std::mutex> lock(ctx->stateMutex);
                        if (!ctx->hadError)
                        {
                            ctx->hadError = true;
                            ctx->errorMessage = err;
                        }
                        return;
                    }

                    const int slices = 50;
                    const int64_t slice = kRetryDelayNs / slices;
                    for (int s = 0; s < slices; ++s)
                    {
                        if (stop_activity)
                        {
                            std::lock_guard<std::mutex> lock(ctx->stateMutex);
                            if (!ctx->hadError)
                            {
                                ctx->hadError = true;
                                ctx->errorMessage = lang_strings[STR_CANCEL_ACTION_MSG];
                            }
                            return;
                        }
                        svcSleepThread(slice);
                    }
                    continue;
                }

                if (res.strBody.empty())
                {
                    std::string err = "empty body";
                    Logger::Logf("WEBDAV GET split-parallel empty body url=%s range=%s attempt=%d/%d",
                                 ctx->url.c_str(),
                                 range_header,
                                 attempt + 1,
                                 kMaxAttempts);

                    if (attempt == kMaxAttempts - 1)
                    {
                        std::lock_guard<std::mutex> lock(ctx->stateMutex);
                        if (!ctx->hadError)
                        {
                            ctx->hadError = true;
                            ctx->errorMessage = err;
                        }
                        return;
                    }

                    const int slices = 50;
                    const int64_t slice = kRetryDelayNs / slices;
                    for (int s = 0; s < slices; ++s)
                    {
                        if (stop_activity)
                        {
                            std::lock_guard<std::mutex> lock(ctx->stateMutex);
                            if (!ctx->hadError)
                            {
                                ctx->hadError = true;
                                ctx->errorMessage = lang_strings[STR_CANCEL_ACTION_MSG];
                            }
                            return;
                        }
                        svcSleepThread(slice);
                    }
                    continue;
                }

                {
                    std::lock_guard<std::mutex> sinkLock(ctx->sinkMutex);
                    if (!ctx->sink->write(static_cast<uint64_t>(start),
                                          res.strBody.data(),
                                          res.strBody.size()))
                    {
                        std::lock_guard<std::mutex> lock(ctx->stateMutex);
                        if (!ctx->hadError)
                        {
                            ctx->hadError = true;
                            ctx->errorMessage = lang_strings[STR_FAIL_DOWNLOAD_MSG];
                        }
                        return;
                    }

                    bytes_transfered += static_cast<int64_t>(res.strBody.size());
                    ctx->lastHttpCode = res.iCode;
                }

                break;
            }
        }
    }
}

std::string WebDAVClient::GetHttpUrl(std::string url)
{
    std::string http_url = std::regex_replace(url, std::regex("webdav://"), "http://");
    http_url = std::regex_replace(http_url, std::regex("webdavs://"), "https://");
    return http_url;
}

int WebDAVClient::Connect(const std::string &host, const std::string &user, const std::string &pass)
{
    std::string url = GetHttpUrl(host);
    return BaseClient::Connect(url, user, pass);
}

bool WebDAVClient::PropFind(const std::string &path, int depth, CHTTPClient::HttpResponse &res)
{
    CHTTPClient::HeadersMap headers;
    headers["Accept"] = "*/*";
    headers["Depth"] = std::to_string(depth);
    std::string encoded_path = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(path));

    return client->CustomRequest("PROPFIND", encoded_path, headers, res);
}

int WebDAVClient::Size(const std::string &path, int64_t *size)
{
    CHTTPClient::HttpResponse res;

    Logger::Logf("WEBDAV Size path='%s'", path.c_str());

    if (PropFind(path, 1, res))
    {
        pugi::xml_document document;
        document.load_buffer(res.strBody.data(), res.strBody.size());
        auto multistatus = document.select_node("*[local-name()='multistatus']").node();
        auto responses = multistatus.select_nodes("*[local-name()='response']");

        // Normalize target path to the logical path the UI uses (without
        // any WebDAV base path like "/dav"). This makes it match the
        // href values returned by SFTPGo and other servers that expose
        // physical paths (eg "/F:2TB/...") directly.
        std::string target_path_without_sep = path;
        Util::Trim(target_path_without_sep, " ");
        Util::Rtrim(target_path_without_sep, "/");
        if (target_path_without_sep.empty())
            target_path_without_sep = "/";

        Logger::Logf("WEBDAV Size normalized target_path='%s', responses=%zu",
                     target_path_without_sep.c_str(), responses.size());

        for (auto response : responses)
        {
            pugi::xml_node href = response.node().select_node("*[local-name()='href']").node();
            if (!href || !href.first_child())
                continue;

            std::string resource_path = CHTTPClient::DecodeUrl(href.first_child().value(), true);
            auto resource_path_without_sep = resource_path.erase(resource_path.find_last_not_of('/') + 1);
            // Strip any WebDAV base path (eg "/dav") from the beginning,
            // so URLs like "/dav/F:2TB/..." match our logical path
            // "/F:2TB/...".
            if (!this->base_path.empty() && this->base_path != "/")
            {
                if (resource_path_without_sep.compare(0, this->base_path.size(), this->base_path) == 0)
                {
                    resource_path_without_sep.erase(0, this->base_path.size());
                    if (resource_path_without_sep.empty())
                        resource_path_without_sep = "/";
                }
            }

            Logger::Logf("WEBDAV Size href='%s' -> resource_path='%s' target='%s'",
                         href.first_child().value(),
                         resource_path_without_sep.c_str(),
                         target_path_without_sep.c_str());

            if (resource_path_without_sep != target_path_without_sep)
                continue;

            auto propstat = response.node().select_node("*[local-name()='propstat']").node();
            auto prop = propstat.select_node("*[local-name()='prop']").node();
            auto len_node = prop.select_node("*[local-name()='getcontentlength']").node();
            if (!len_node || !len_node.first_child())
                continue;

            std::string content_length = len_node.first_child().value();
            *size = atoll(content_length.c_str());
            return 1;
        }
    }
    else
    {
        sprintf(this->response, "%s", res.errMessage.c_str());
        Logger::Logf("WEBDAV Size PROPFIND failed err=%s", this->response);
    }

    return 0;
}

int WebDAVClient::Get(const std::string &outputfile, const std::string &path, uint64_t offset)
{
    bytes_transfered = 0;
    prev_tick = Util::GetTick();

    if (stop_activity)
    {
        sprintf(this->response, "%s", lang_strings[STR_CANCEL_ACTION_MSG]);
        Logger::Logf("WEBDAV GET cancelled before start path=%s", path.c_str());
        return 0;
    }

    // First, try to get the full size via PROPFIND so we can download
    // in smaller HTTP range chunks (more robust with Tailscale/Funnel).
    int64_t size = 0;
    if (!Size(path, &size) || size <= 0)
    {
        Logger::Logf("WEBDAV GET unable to determine size for path='%s', falling back to single GET",
                     path.c_str());
        // Fall back to the simple one-shot GET path for small files.
        std::string encoded_url_fallback = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(path));

        CHTTPClient::HttpResponse res_fallback;
        CHTTPClient::HeadersMap headers_fallback;

        if (!client->Get(encoded_url_fallback, headers_fallback, res_fallback))
        {
            sprintf(this->response, "%s", res_fallback.errMessage.c_str());
            Logger::Logf("WEBDAV GET fallback error url=%s err=%s",
                         encoded_url_fallback.c_str(), res_fallback.errMessage.c_str());
            return 0;
        }

        if (!HTTP_SUCCESS(res_fallback.iCode))
        {
            sprintf(this->response, "%ld - %s", res_fallback.iCode, lang_strings[STR_FAIL_DOWNLOAD_MSG]);
            Logger::Logf("WEBDAV GET fallback http error url=%s code=%ld",
                         encoded_url_fallback.c_str(), res_fallback.iCode);
            return 0;
        }

        FILE *file_fallback = std::fopen(outputfile.c_str(), "wb");
        if (!file_fallback)
        {
            sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
            Logger::Logf("WEBDAV GET fallback fopen failed path=%s", outputfile.c_str());
            return 0;
        }

        size_t written_fb = std::fwrite(res_fallback.strBody.data(), 1, res_fallback.strBody.size(), file_fallback);
        std::fclose(file_fallback);

        if (written_fb != res_fallback.strBody.size())
        {
            sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
            Logger::Logf("WEBDAV GET fallback write failed path=%s expected=%zu written=%zu",
                         outputfile.c_str(), res_fallback.strBody.size(), written_fb);
            return 0;
        }

        bytes_to_download = static_cast<int64_t>(res_fallback.strBody.size());
        bytes_transfered = bytes_to_download;

        uint64_t now = Util::GetTick();
        double elapsed_sec = (now - prev_tick) * 1.0 / 1000000.0;
        double mb = (res_fallback.strBody.size() / 1048576.0);
        double avg_mbps = (elapsed_sec > 0.0) ? (mb / elapsed_sec) : 0.0;

        Logger::Logf("WEBDAV PERF fallback url=%s bytes=%zu elapsed=%.2fs avg=%.2f MiB/s",
                     encoded_url_fallback.c_str(),
                     res_fallback.strBody.size(),
                     elapsed_sec,
                     avg_mbps);

        Logger::Logf("WEBDAV GET fallback done url=%s code=%ld bytes=%zu",
                     encoded_url_fallback.c_str(), res_fallback.iCode, res_fallback.strBody.size());
        return 1;
    }

    std::string encoded_url = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(path));

    // Use configurable HTTP range chunk size (in MiB), defaulting to 8 MiB.
    // This keeps per-request overhead low over high-latency links while
    // still fitting comfortably in Switch memory. Value is clamped in
    // CONFIG::LoadConfig(), but guard here as well.
    int chunk_mb = webdav_chunk_size_mb;
    if (chunk_mb < 1)
        chunk_mb = 1;
    else if (chunk_mb > 32)
        chunk_mb = 32;
    int64_t chunk_size = static_cast<int64_t>(chunk_mb) * 1024 * 1024;

    const uint64_t kSplitPartSize = 4294901760ULL; // 4 GiB - 64 KiB
    // For files larger than 4 GiB, always use the DBI-style split layout
    // so downloads succeed even on FAT32 cards. The force_fat32 flag can
    // also be used to force this layout for testing or for smaller files.
    bool need_split = (size > 0xFFFFFFFFLL) || force_fat32;

    if (need_split)
    {
        // Derive a safe local base directory for DBI-style split files.
        // Start from the user-selected output path, but sanitize the final
        // component so that exotic characters (UTF-8 punctuation, etc.)
        // don't cause mkdir() failures on the SD filesystem.
        const char *slash = strrchr(outputfile.c_str(), '/');
        std::string parentDir;
        std::string fileName;
        if (slash)
        {
            parentDir.assign(outputfile.c_str(), slash - outputfile.c_str());
            fileName = slash + 1;
        }
        else
        {
            parentDir.clear();
            fileName = outputfile;
        }

        std::string safeName = SanitizePathComponent(fileName);

        // Ensure parent directory exists; if that fails, fall back to a
        // known-writable downloads directory under the app data path.
        if (parentDir.empty() || !EnsureDirectoryTree(parentDir))
        {
            Logger::Logf("WEBDAV GET split parent '%s' not usable, falling back to DATA_PATH/downloads",
                         parentDir.c_str());
            parentDir = std::string(DATA_PATH) + "/downloads";
            EnsureDirectoryTree(parentDir);
        }

        std::string splitBase = parentDir;
        if (!FS::hasEndSlash(splitBase.c_str()))
            splitBase.push_back('/');
        splitBase += safeName;

        int64_t local_split_size = GetSplitLocalSize(splitBase, kSplitPartSize);
        if (local_split_size >= size)
        {
            bytes_to_download = size;
            bytes_transfered = size;
            Logger::Logf("WEBDAV GET split already complete path=%s size=%lld",
                         splitBase.c_str(),
                         static_cast<long long>(size));
            return 1;
        }

        bytes_to_download = size;
        bytes_transfered = (local_split_size > 0) ? local_split_size : 0;

        int parallel = webdav_parallel_connections;
        if (parallel < 1)
            parallel = 1;
        else if (parallel > 16)
            parallel = 16;

        const int64_t max_window = 256LL * 1024 * 1024; // 256 MiB
        if (chunk_size * parallel > max_window)
        {
            chunk_size = max_window / parallel;
            if (chunk_size < (1LL * 1024 * 1024))
                chunk_size = 1LL * 1024 * 1024;
            chunk_mb = static_cast<int>(chunk_size / (1024 * 1024));
        }

        Logger::Logf("WEBDAV GET split (ranged) url=%s -> output=%s remote_size=%lld local_size=%lld chunk_size=%lld parallel=%d",
                     encoded_url.c_str(),
                     splitBase.c_str(),
                     static_cast<long long>(size),
                     static_cast<long long>(local_split_size),
                     static_cast<long long>(chunk_size),
                     parallel);

        const bool wants_parallel_split = (parallel > 1 && size > chunk_size);
        if (wants_parallel_split && ProbeRangeSupport(encoded_url))
        {
            Logger::Logf("WEBDAV GET using parallel split ranged download url=%s", encoded_url.c_str());
            return GetRangedParallelSplit(splitBase,
                                          encoded_url,
                                          size,
                                          chunk_size,
                                          parallel,
                                          kSplitPartSize);
        }

        Logger::Logf("WEBDAV GET using sequential split ranged download url=%s", encoded_url.c_str());
        return GetRangedSequentialSplit(splitBase,
                                        encoded_url,
                                        size,
                                        chunk_size,
                                        (local_split_size > 0) ? local_split_size : 0,
                                        kSplitPartSize);
    }

    // For non-split single-file downloads, derive a safe local file path by
    // sanitizing the filename and ensuring the parent directory exists. This
    // avoids fopen() failures when the remote filename contains characters
    // that the SD filesystem doesn't like or when the chosen parent path is
    // not writable (we fall back to DATA_PATH/downloads in that case).
    const char *slash = strrchr(outputfile.c_str(), '/');
    std::string singleParentDir;
    std::string singleFileName;
    if (slash)
    {
        singleParentDir.assign(outputfile.c_str(), slash - outputfile.c_str());
        singleFileName = slash + 1;
    }
    else
    {
        singleParentDir.clear();
        singleFileName = outputfile;
    }

    std::string safeSingleName = SanitizePathComponent(singleFileName);
    if (singleParentDir.empty() || !EnsureDirectoryTree(singleParentDir))
    {
        Logger::Logf("WEBDAV GET single parent '%s' not usable, falling back to DATA_PATH/downloads",
                     singleParentDir.c_str());
        singleParentDir = std::string(DATA_PATH) + "/downloads";
        EnsureDirectoryTree(singleParentDir);
    }

    std::string singleOutput = singleParentDir;
    if (!FS::hasEndSlash(singleOutput.c_str()))
        singleOutput.push_back('/');
    singleOutput += safeSingleName;

    // Check for an existing partial file to support simple resume when we
    // are writing to a single file. If we find a local file smaller than
    // the remote size, resume from that offset using a sequential ranged
    // download to avoid restarting.
    int64_t local_size = FS::GetSize(singleOutput);
    if (local_size > 0 && local_size < size)
    {
        if (!EnsureParentDirectory(outputfile))
        {
            Logger::Logf("WEBDAV GET resume cannot create parent for output=%s", outputfile.c_str());
        }
        else
        {
            bytes_to_download = size;
            bytes_transfered = local_size;

            Logger::Logf("WEBDAV GET resume url=%s -> output=%s remote_size=%lld local_size=%lld chunk_size=%lld",
                         encoded_url.c_str(),
                         singleOutput.c_str(),
                         static_cast<long long>(size),
                         static_cast<long long>(local_size),
                         static_cast<long long>(chunk_size));

            return GetRangedSequential(singleOutput, encoded_url, size, chunk_size, local_size);
        }
    }

    bytes_to_download = size;
    bytes_transfered = 0;

    int parallel = webdav_parallel_connections;
    if (parallel < 1)
        parallel = 1;
    else if (parallel > 32)
        parallel = 32;

    // Keep the total in-flight window (chunk_size * parallel) under a
    // cap to avoid excessive memory usage when the user configures both
    // a large chunk size and many workers.
    const int64_t max_window = 256LL * 1024 * 1024; // 256 MiB
    if (chunk_size * parallel > max_window)
    {
        chunk_size = max_window / parallel;
        if (chunk_size < (1LL * 1024 * 1024))
            chunk_size = 1LL * 1024 * 1024;
        chunk_mb = static_cast<int>(chunk_size / (1024 * 1024));
    }

    Logger::Logf("WEBDAV GET (ranged) url=%s -> output=%s size=%lld chunk_size=%lld parallel=%d",
                 encoded_url.c_str(),
                 singleOutput.c_str(),
                 static_cast<long long>(size),
                 static_cast<long long>(chunk_size),
                 parallel);

    // Only attempt the more aggressive parallel ranged path when:
    // - The file is at least larger than a single chunk.
    // - The server clearly supports HTTP Range requests.
    const bool wants_parallel = (parallel > 1 && size > chunk_size);
    if (wants_parallel && ProbeRangeSupport(encoded_url))
    {
        Logger::Logf("WEBDAV GET using parallel ranged download url=%s", encoded_url.c_str());
        return GetRangedParallel(singleOutput, encoded_url, size, chunk_size, parallel);
    }

    Logger::Logf("WEBDAV GET using sequential ranged download url=%s", encoded_url.c_str());
    return GetRangedSequential(singleOutput, encoded_url, size, chunk_size);
}

bool WebDAVClient::ProbeRangeSupport(const std::string &encoded_url)
{
    CHTTPClient::HttpResponse res;
    CHTTPClient::HeadersMap headers;
    headers["Range"] = "bytes=0-0";

    if (!client->Get(encoded_url, headers, res))
    {
        Logger::Logf("WEBDAV GET range probe error url=%s err=%s",
                     encoded_url.c_str(), res.errMessage.c_str());
        return false;
    }

    if (res.iCode == 206)
    {
        Logger::Logf("WEBDAV GET range probe ok url=%s code=%ld",
                     encoded_url.c_str(), res.iCode);
        return true;
    }

    Logger::Logf("WEBDAV GET range probe unsupported url=%s code=%ld",
                 encoded_url.c_str(), res.iCode);
    return false;
}

int WebDAVClient::GetRangedSequential(const std::string &outputfile,
                                      const std::string &encoded_url,
                                      int64_t size,
                                      int64_t chunk_size)
{
    return GetRangedSequential(outputfile, encoded_url, size, chunk_size, 0);
}

int WebDAVClient::GetRangedSequential(const std::string &outputfile,
                                      const std::string &encoded_url,
                                      int64_t size,
                                      int64_t chunk_size,
                                      int64_t start_offset)
{
    FILE *file = std::fopen(outputfile.c_str(), start_offset > 0 ? "r+b" : "wb");
    if (!file)
    {
        sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
        Logger::Logf("WEBDAV GET fopen failed path=%s", outputfile.c_str());
        return 0;
    }

    int64_t offset_bytes = start_offset;
    long last_code = 0;

    if (offset_bytes > 0)
    {
        if (fseeko(file, (off_t)offset_bytes, SEEK_SET) != 0)
        {
            std::fclose(file);
            sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
            Logger::Logf("WEBDAV GET ranged resume seek failed path=%s offset=%lld",
                         outputfile.c_str(),
                         static_cast<long long>(offset_bytes));
            return 0;
        }
    }

    while (offset_bytes < size)
    {
        if (stop_activity)
        {
            std::fclose(file);
            sprintf(this->response, "%s", lang_strings[STR_CANCEL_ACTION_MSG]);
            Logger::Logf("WEBDAV GET range cancelled url=%s bytes=%lld",
                         encoded_url.c_str(),
                         static_cast<long long>(offset_bytes));
            return 0;
        }

        int64_t end = offset_bytes + chunk_size - 1;
        if (end >= size)
            end = size - 1;

        char range_header[64];
        std::snprintf(range_header, sizeof(range_header), "bytes=%lld-%lld",
                      static_cast<long long>(offset_bytes),
                      static_cast<long long>(end));

        CHTTPClient::HeadersMap headers;
        headers["Range"] = range_header;

        CHTTPClient::HttpResponse res;
        if (!client->Get(encoded_url, headers, res))
        {
            sprintf(this->response, "%s", res.errMessage.c_str());
            Logger::Logf("WEBDAV GET range error url=%s range=%s err=%s",
                         encoded_url.c_str(), range_header, res.errMessage.c_str());
            std::fclose(file);
            return 0;
        }

        last_code = res.iCode;
        if (!(res.iCode == 206 || res.iCode == 200))
        {
            sprintf(this->response, "%ld - %s", res.iCode, lang_strings[STR_FAIL_DOWNLOAD_MSG]);
            Logger::Logf("WEBDAV GET range http error url=%s range=%s code=%ld",
                         encoded_url.c_str(), range_header, res.iCode);
            std::fclose(file);
            return 0;
        }

        if (res.strBody.empty())
        {
            Logger::Logf("WEBDAV GET range empty body url=%s range=%s", encoded_url.c_str(), range_header);
            break;
        }

        size_t written = std::fwrite(res.strBody.data(), 1, res.strBody.size(), file);
        if (written != res.strBody.size())
        {
            sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
            Logger::Logf("WEBDAV GET range write failed path=%s expected=%zu written=%zu",
                         outputfile.c_str(), res.strBody.size(), written);
            std::fclose(file);
            return 0;
        }

        offset_bytes += static_cast<int64_t>(res.strBody.size());
        bytes_transfered = offset_bytes;

        // If server ignored Range and returned the full file with 200,
        // we are done after the first iteration.
        if (res.iCode == 200)
        {
            break;
        }
    }

    std::fclose(file);

    if (offset_bytes <= 0)
    {
        sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
        Logger::Logf("WEBDAV GET ranged download produced no data url=%s", encoded_url.c_str());
        return 0;
    }

    uint64_t now = Util::GetTick();
    double elapsed_sec = (now - prev_tick) * 1.0 / 1000000.0;
    double mb = (offset_bytes / 1048576.0);
    double avg_mbps = (elapsed_sec > 0.0) ? (mb / elapsed_sec) : 0.0;

    Logger::Logf("WEBDAV PERF ranged-seq url=%s size=%lld chunk_mb=%lld elapsed=%.2fs avg=%.2f MiB/s",
                 encoded_url.c_str(),
                 static_cast<long long>(offset_bytes),
                 static_cast<long long>(chunk_size / (1024 * 1024)),
                 elapsed_sec,
                 avg_mbps);

    Logger::Logf("WEBDAV GET ranged done url=%s code=%ld bytes=%lld",
                 encoded_url.c_str(), last_code, static_cast<long long>(offset_bytes));
    return 1;
}

int WebDAVClient::GetRangedSequentialSplit(const std::string &outputfile,
                                           const std::string &encoded_url,
                                           int64_t size,
                                           int64_t chunk_size,
                                           int64_t start_offset,
                                           uint64_t partSize)
{
    SplitFileWriter sink(outputfile, partSize);
    if (!sink.open())
    {
        sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
        Logger::Logf("WEBDAV GET split open failed base=%s", outputfile.c_str());
        return 0;
    }

    int64_t offset_bytes = start_offset;
    long last_code = 0;

    while (offset_bytes < size)
    {
        if (stop_activity)
        {
            sprintf(this->response, "%s", lang_strings[STR_CANCEL_ACTION_MSG]);
            Logger::Logf("WEBDAV GET split range cancelled url=%s bytes=%lld",
                         encoded_url.c_str(),
                         static_cast<long long>(offset_bytes));
            return 0;
        }

        int64_t end = offset_bytes + chunk_size - 1;
        if (end >= size)
            end = size - 1;

        char range_header[64];
        std::snprintf(range_header, sizeof(range_header), "bytes=%lld-%lld",
                      static_cast<long long>(offset_bytes),
                      static_cast<long long>(end));

        CHTTPClient::HeadersMap headers;
        headers["Range"] = range_header;

        CHTTPClient::HttpResponse res;
        if (!client->Get(encoded_url, headers, res))
        {
            sprintf(this->response, "%s", res.errMessage.c_str());
            Logger::Logf("WEBDAV GET split range error url=%s range=%s err=%s",
                         encoded_url.c_str(), range_header, res.errMessage.c_str());
            return 0;
        }

        last_code = res.iCode;
        if (!(res.iCode == 206 || res.iCode == 200))
        {
            sprintf(this->response, "%ld - %s", res.iCode, lang_strings[STR_FAIL_DOWNLOAD_MSG]);
            Logger::Logf("WEBDAV GET split range http error url=%s range=%s code=%ld",
                         encoded_url.c_str(), range_header, res.iCode);
            return 0;
        }

        if (res.strBody.empty())
        {
            Logger::Logf("WEBDAV GET split range empty body url=%s range=%s",
                         encoded_url.c_str(), range_header);
            break;
        }

        if (!sink.write(static_cast<uint64_t>(offset_bytes),
                        res.strBody.data(),
                        res.strBody.size()))
        {
            sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
            Logger::Logf("WEBDAV GET split write failed base=%s offset=%lld size=%zu",
                         outputfile.c_str(),
                         static_cast<long long>(offset_bytes),
                         res.strBody.size());
            return 0;
        }

        offset_bytes += static_cast<int64_t>(res.strBody.size());
        bytes_transfered = offset_bytes;

        if (res.iCode == 200)
        {
            break;
        }
    }

    if (offset_bytes <= 0)
    {
        sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
        Logger::Logf("WEBDAV GET split ranged download produced no data url=%s", encoded_url.c_str());
        return 0;
    }

    uint64_t now = Util::GetTick();
    double elapsed_sec = (now - prev_tick) * 1.0 / 1000000.0;
    double mb = (offset_bytes / 1048576.0);
    double avg_mbps = (elapsed_sec > 0.0) ? (mb / elapsed_sec) : 0.0;

    Logger::Logf("WEBDAV PERF split-ranged url=%s size=%lld chunk_mb=%lld elapsed=%.2fs avg=%.2f MiB/s",
                 encoded_url.c_str(),
                 static_cast<long long>(offset_bytes),
                 static_cast<long long>(chunk_size / (1024 * 1024)),
                 elapsed_sec,
                 avg_mbps);

    Logger::Logf("WEBDAV GET split ranged done url=%s code=%ld bytes=%lld",
                 encoded_url.c_str(), last_code, static_cast<long long>(offset_bytes));
    return 1;
}

int WebDAVClient::GetRangedParallelSplit(const std::string &outputfile,
                                         const std::string &encoded_url,
                                         int64_t size,
                                         int64_t chunk_size,
                                         int parallel,
                                         uint64_t partSize)
{
    SplitFileWriter sink(outputfile, partSize);
    if (!sink.open())
    {
        sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
        Logger::Logf("WEBDAV GET split-parallel open failed base=%s", outputfile.c_str());
        return 0;
    }

    bytes_transfered = 0;

    WebDAVParallelSplitContext ctx;
    ctx.url = encoded_url;
    ctx.sink = &sink;
    ctx.size = size;
    ctx.chunk_size = chunk_size;
    ctx.nextOffset = 0;
    ctx.hadError = false;
    ctx.lastHttpCode = 0;

    std::vector<std::unique_ptr<CHTTPClient>> httpClients;
    httpClients.reserve(parallel);
    for (int i = 0; i < parallel; ++i)
    {
        auto client = std::make_unique<CHTTPClient>([](const std::string &) {});
        client->SetBasicAuth(http_username, http_password);
        client->InitSession(false, CHTTPClient::SettingsFlag::NO_FLAGS);
        client->SetCertificateFile(CACERT_FILE);
        httpClients.push_back(std::move(client));
    }

    std::vector<Thread> threads(parallel);
    std::vector<WebDAVSplitWorkerArgs> workerArgs(parallel);

    bool threadError = false;
    for (int i = 0; i < parallel; ++i)
    {
        workerArgs[i].ctx = &ctx;
        workerArgs[i].client = httpClients[i].get();

        Result rc = threadCreate(&threads[i],
                                 WebDAVParallelSplitWorkerThread,
                                 &workerArgs[i],
                                 nullptr,
                                 0x4000,
                                 0x3B,
                                 -2);
        if (R_FAILED(rc))
        {
            threadError = true;
            Logger::Logf("WEBDAV GET split-parallel threadCreate failed index=%d rc=0x%x", i, rc);
            break;
        }
    }

    if (!threadError)
    {
        for (int i = 0; i < parallel; ++i)
        {
            if (threads[i].handle == 0)
                continue;
            threadStart(&threads[i]);
        }

        for (int i = 0; i < parallel; ++i)
        {
            if (threads[i].handle == 0)
                continue;
            threadWaitForExit(&threads[i]);
            threadClose(&threads[i]);
        }
    }

    {
        std::lock_guard<std::mutex> lock(ctx.stateMutex);
        if (ctx.hadError)
        {
            sprintf(this->response, "%s", ctx.errorMessage.c_str());
            Logger::Logf("WEBDAV GET split-parallel error url=%s err=%s",
                         encoded_url.c_str(),
                         ctx.errorMessage.c_str());
            return 0;
        }
    }

    if (bytes_transfered <= 0)
    {
        sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
        Logger::Logf("WEBDAV GET split-parallel produced no data url=%s", encoded_url.c_str());
        return 0;
    }

    uint64_t now = Util::GetTick();
    double elapsed_sec = (now - prev_tick) * 1.0 / 1000000.0;
    double mb = (bytes_transfered / 1048576.0);
    double avg_mbps = (elapsed_sec > 0.0) ? (mb / elapsed_sec) : 0.0;

    Logger::Logf("WEBDAV PERF split-parallel url=%s size=%lld chunk_mb=%lld elapsed=%.2fs avg=%.2f MiB/s",
                 encoded_url.c_str(),
                 static_cast<long long>(bytes_transfered),
                 static_cast<long long>(chunk_size / (1024 * 1024)),
                 elapsed_sec,
                 avg_mbps);

    Logger::Logf("WEBDAV GET split-parallel ranged done url=%s code=%ld bytes=%lld",
                 encoded_url.c_str(),
                 ctx.lastHttpCode,
                 static_cast<long long>(bytes_transfered));

    return 1;
}

int WebDAVClient::GetRangedParallel(const std::string &outputfile,
                                    const std::string &encoded_url,
                                    int64_t size,
                                    int64_t chunk_size,
                                    int parallel)
{
    FILE *file = std::fopen(outputfile.c_str(), "wb");
    if (!file)
    {
        sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
        Logger::Logf("WEBDAV GET parallel fopen failed path=%s", outputfile.c_str());
        return 0;
    }

    if (size > 0 && size <= 0xFFFFFFFFLL)
    {
        if (fseeko(file, (off_t)(size - 1), SEEK_SET) == 0)
        {
            int c = fputc(0, file);
            (void)c;
            fflush(file);
        }
        fseeko(file, 0, SEEK_SET);
    }

    bytes_transfered = 0;

    WebDAVParallelContext ctx;
    ctx.url = encoded_url;
    ctx.outputPath = outputfile;
    ctx.file = file;
    ctx.size = size;
    ctx.chunk_size = chunk_size;
    ctx.nextOffset = 0;
    ctx.hadError = false;
    ctx.lastHttpCode = 0;

    std::vector<std::unique_ptr<CHTTPClient>> httpClients;
    httpClients.reserve(parallel);
    for (int i = 0; i < parallel; ++i)
    {
        auto client = std::make_unique<CHTTPClient>([](const std::string &){});
        client->SetBasicAuth(http_username, http_password);
        client->InitSession(false, CHTTPClient::SettingsFlag::NO_FLAGS);
        client->SetCertificateFile(CACERT_FILE);
        httpClients.push_back(std::move(client));
    }

    std::vector<Thread> threads(parallel);
    std::vector<WebDAVWorkerArgs> workerArgs(parallel);

    bool threadError = false;
    for (int i = 0; i < parallel; ++i)
    {
        workerArgs[i].ctx = &ctx;
        workerArgs[i].client = httpClients[i].get();

        Result rc = threadCreate(&threads[i],
                                 WebDAVParallelWorkerThread,
                                 &workerArgs[i],
                                 nullptr,
                                 0x10000,
                                 0x3B,
                                 -2);
        if (R_FAILED(rc))
        {
            Logger::Logf("WEBDAV GET parallel failed to create worker thread url=%s rc=0x%08x",
                         encoded_url.c_str(),
                         rc);
            threadError = true;
            break;
        }

        threadStart(&threads[i]);
    }

    for (int i = 0; i < parallel; ++i)
    {
        if (threads[i].handle != 0)
        {
            threadWaitForExit(&threads[i]);
            threadClose(&threads[i]);
        }
    }

    std::fclose(file);

    if (threadError)
    {
        sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
        return 0;
    }

    if (ctx.hadError)
    {
        if (!ctx.errorMessage.empty())
            snprintf(this->response, sizeof(this->response), "%s", ctx.errorMessage.c_str());
        else
            sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);

        Logger::Logf("WEBDAV GET ranged-parallel error url=%s err=%s",
                     encoded_url.c_str(),
                     ctx.errorMessage.c_str());
        return 0;
    }

    if (bytes_transfered <= 0)
    {
        sprintf(this->response, "%s", lang_strings[STR_FAIL_DOWNLOAD_MSG]);
        Logger::Logf("WEBDAV GET ranged-parallel produced no data url=%s", encoded_url.c_str());
        return 0;
    }

    uint64_t now = Util::GetTick();
    double elapsed_sec = (now - prev_tick) * 1.0 / 1000000.0;
    double mb = (bytes_transfered / 1048576.0);
    double avg_mbps = (elapsed_sec > 0.0) ? (mb / elapsed_sec) : 0.0;

    Logger::Logf("WEBDAV PERF ranged-parallel url=%s size=%lld chunk_mb=%lld parallel=%d elapsed=%.2fs avg=%.2f MiB/s",
                 encoded_url.c_str(),
                 static_cast<long long>(bytes_transfered),
                 static_cast<long long>(chunk_size / (1024 * 1024)),
                 parallel,
                 elapsed_sec,
                 avg_mbps);

    Logger::Logf("WEBDAV GET ranged-parallel done url=%s code=%ld bytes=%lld parallel=%d",
                 encoded_url.c_str(),
                 ctx.lastHttpCode,
                 static_cast<long long>(bytes_transfered),
                 parallel);
    return 1;
}

std::vector<DirEntry> WebDAVClient::ListDir(const std::string &path)
{
    CHTTPClient::HttpResponse res;
    std::vector<DirEntry> out;
    DirEntry entry;
    Util::SetupPreviousFolder(path, &entry);
    out.push_back(entry);

    Logger::Logf("WEBDAV ListDir path='%s'", path.c_str());

    if (PropFind(path, 1, res))
    {
        pugi::xml_document document;
        document.load_buffer(res.strBody.data(), res.strBody.size());
        auto multistatus = document.select_node("*[local-name()='multistatus']").node();
        auto responses = multistatus.select_nodes("*[local-name()='response']");

        // Normalize the current logical path (what the user sees in the UI)
        // without any WebDAV base path (eg "/dav").
        std::string target_path_without_sep = path;
        Util::Trim(target_path_without_sep, " ");
        Util::Rtrim(target_path_without_sep, "/");
        if (target_path_without_sep.empty())
            target_path_without_sep = "/";

        Logger::Logf("WEBDAV ListDir normalized target_path='%s', responses=%zu",
                     target_path_without_sep.c_str(), responses.size());

        for (auto response : responses)
        {
            pugi::xml_node href = response.node().select_node("*[local-name()='href']").node();
            if (!href || !href.first_child())
                continue;

            std::string resource_path = CHTTPClient::DecodeUrl(href.first_child().value(), true);
            auto resource_path_without_sep = resource_path.erase(resource_path.find_last_not_of('/') + 1);
            if (!this->base_path.empty() && this->base_path != "/")
            {
                if (resource_path_without_sep.compare(0, this->base_path.size(), this->base_path) == 0)
                {
                    resource_path_without_sep.erase(0, this->base_path.size());
                    if (resource_path_without_sep.empty())
                        resource_path_without_sep = "/";
                }
            }

            Logger::Logf("WEBDAV ListDir href='%s' -> resource_path='%s'",
                         href.first_child().value(),
                         resource_path_without_sep.c_str());

            if (resource_path_without_sep == target_path_without_sep)
                continue;

            size_t pos2 = resource_path_without_sep.find_last_of('/');
            auto name = resource_path_without_sep.substr(pos2 + 1);
            auto propstat = response.node().select_node("*[local-name()='propstat']").node();
            auto prop = propstat.select_node("*[local-name()='prop']").node();

            std::string creation_date;
            std::string content_length;
            std::string m_date;
            std::string resource_type;

            auto cd_node = prop.select_node("*[local-name()='creationdate']").node();
            if (cd_node && cd_node.first_child())
                creation_date = cd_node.first_child().value();

            auto len_node = prop.select_node("*[local-name()='getcontentlength']").node();
            if (len_node && len_node.first_child())
                content_length = len_node.first_child().value();

            auto md_node = prop.select_node("*[local-name()='getlastmodified']").node();
            if (md_node && md_node.first_child())
                m_date = md_node.first_child().value();

            auto rt_node = prop.select_node("*[local-name()='resourcetype']").node();
            if (rt_node && rt_node.first_child())
                resource_type = rt_node.first_child().name();

            DirEntry entry;
            memset(&entry, 0, sizeof(entry));
            entry.selectable = true;
            sprintf(entry.directory, "%s", path.c_str());
            sprintf(entry.name, "%s", name.c_str());

            if (path.length() == 1 and path[0] == '/')
            {
                sprintf(entry.path, "%s%s", path.c_str(), name.c_str());
            }
            else
            {
                sprintf(entry.path, "%s/%s", path.c_str(), name.c_str());
            }

            entry.isDir = resource_type.find("collection") != std::string::npos;
            entry.file_size = 0;
            if (!entry.isDir)
            {
                entry.file_size = atoll(content_length.c_str());
                DirEntry::SetDisplaySize(&entry);
            }
            else
            {
                sprintf(entry.display_size, "%s", lang_strings[STR_FOLDER]);
            }

            char modified_date[32];
            char *p_char = NULL;
            sprintf(modified_date, "%s", m_date.c_str());
            p_char = strchr(modified_date, ' ');
            if (p_char)
            {
                char month[5];
                sscanf(p_char, "%d %s %d %d:%d:%d", &entry.modified.day, month, &entry.modified.year, &entry.modified.hours, &entry.modified.minutes, &entry.modified.seconds);
                for (int k = 0; k < 12; k++)
                {
                    if (strcmp(month, months[k]) == 0)
                    {
                        entry.modified.month = k + 1;
                        break;
                    }
                }
            }
            out.push_back(entry);
        }
    }
    else
    {
        sprintf(this->response, "%s", res.errMessage.c_str());
        Logger::Logf("WEBDAV ListDir PROPFIND failed err=%s", this->response);
        return out;
    }

    return out;
}

int WebDAVClient::Put(const std::string &inputfile, const std::string &path, uint64_t offset)
{
    size_t bytes_remaining = FS::GetSize(inputfile);
    bytes_transfered = 0;
    prev_tick = Util::GetTick();

    client->SetProgressFnCallback(&bytes_transfered, UploadProgressCallback);
    std::string encode_url = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(path));
    long status;

    if (client->UploadFile(inputfile, encode_url, status))
    {
        if (HTTP_SUCCESS(status))
        {
            return 1;
        }
    }

    return 0;
}

int WebDAVClient::Mkdir(const std::string &path)
{
    CHTTPClient::HeadersMap headers;
    CHTTPClient::HttpResponse res;

    headers["Accept"] =  "*/*";
    headers["Connection"] = "Keep-Alive";
    std::string encode_url = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(path));

    if (client->CustomRequest("MKCOL", encode_url, headers, res))
    {
        if (HTTP_SUCCESS(res.iCode))
            return 1;
    }

    return 0;
}

int WebDAVClient::Rmdir(const std::string &path, bool recursive)
{
    return Delete(path);
}

int WebDAVClient::Rename(const std::string &src, const std::string &dst)
{
    return Move(src, dst);
}

int WebDAVClient::Delete(const std::string &path)
{
    CHTTPClient::HeadersMap headers;
    CHTTPClient::HttpResponse res;

    headers["Accept"] =  "*/*";
    headers["Connection"] = "Keep-Alive";
    std::string encode_url = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(path));

    if (client->CustomRequest("DELETE", encode_url, headers, res))
    {
        if (HTTP_SUCCESS(res.iCode))
            return 1;
    }
    
    return 0;

}

int WebDAVClient::Copy(const std::string &from, const std::string &to)
{
    CHTTPClient::HeadersMap headers;
    CHTTPClient::HttpResponse res;

    headers["Accept"] =  "*/*";
    headers["Destination"] = GetFullPath(to);
    std::string encode_url = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(from));

    if (client->CustomRequest("COPY", encode_url, headers, res))
    {
        if (HTTP_SUCCESS(res.iCode))
            return 1;
    }

    return 0;
}

int WebDAVClient::Move(const std::string &from, const std::string &to)
{
    CHTTPClient::HeadersMap headers;
    CHTTPClient::HttpResponse res;

    headers["Accept"] =  "*/*";
    headers["Destination"] = GetFullPath(to);
    std::string encode_url = this->host_url + CHTTPClient::EncodeUrl(GetFullPath(from));

    if (client->CustomRequest("MOVE", encode_url, headers, res))
    {
        if (HTTP_SUCCESS(res.iCode))
            return 1;
    }

    return 0;
}

ClientType WebDAVClient::clientType()
{
    return CLIENT_TYPE_WEBDAV;
}

uint32_t WebDAVClient::SupportedActions()
{
    return REMOTE_ACTION_ALL ^ REMOTE_ACTION_RAW_READ;
}
