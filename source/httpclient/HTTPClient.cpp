#include "httpclient/HTTPClient.h"

#include <algorithm>
#include <cstring>
#include "util.h"
#include "logger.h"

namespace
{
    int CurlDebugCallback(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr)
    {
        (void)handle;
        (void)userptr;
        if (type == CURLINFO_TEXT && size > 0)
        {
            while (size > 0 && (data[size - 1] == '\n' || data[size - 1] == '\r'))
                --size;
            std::string msg(data, size);
            Logger::Logf("HTTP DEBUG: %s", msg.c_str());
        }
        return 0;
    }
}

CHTTPClient::CHTTPClient(LogFn logFn)
    : curl(curl_easy_init()), logger(std::move(logFn))
{
}

CHTTPClient::~CHTTPClient()
{
    if (curl)
        curl_easy_cleanup(curl);
}

void CHTTPClient::SetBasicAuth(const std::string &u, const std::string &p)
{
    user = u;
    pass = p;
}

void CHTTPClient::InitSession(bool verifyPeer, SettingsFlag)
{
    if (!curl)
        curl = curl_easy_init();

    // Prefer HTTP/2 over TLS connections when available; libcurl will
    // gracefully fall back to HTTP/1.1 if HTTP/2 is not supported by the
    // server or build.
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verifyPeer ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verifyPeer ? 2L : 0L);
}

void CHTTPClient::SetCertificateFile(const std::string &path)
{
    caFile = path;
}

void CHTTPClient::SetProgressFnCallback(void *owner, int (*fn)(void *, double, double, double, double))
{
    // Progress callbacks through libcurl's XFERINFOFUNCTION have caused
    // instability on some Switch setups (depending on libcurl build and
    // optimization). For stability, disable per-request progress updates
    // for now and rely on size-based UI updates instead.
    (void)owner;
    (void)fn;
    progressOwner.pOwner = nullptr;
    progressFn = nullptr;
}

void CHTTPClient::applyCommonOptions(const std::string &url)
{
    activeUrl = url;

    curl_easy_setopt(curl, CURLOPT_URL, activeUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "neo_sftp/1.0");
    // Disable libcurl per-request verbose logging in production builds;
    // logging every line to SD can stall the UI on Switch.
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 0L);
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, nullptr);

    if (!user.empty())
    {
        std::string auth = user + ":" + pass;
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth.c_str());
    }

    if (!caFile.empty())
        curl_easy_setopt(curl, CURLOPT_CAINFO, caFile.c_str());

    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, progressFn ? 0L : 1L);
    if (progressFn)
    {
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &CHTTPClient::progressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    }

    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);          // no overall timeout
    // Disable low-speed aborts; Tailscale + SFTPGo can be bursty/slow.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 0L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 0L);
    // Tune receive buffering and TCP behavior for better throughput over
    // high-latency links like Tailscale/Funnel. Use a 1 MiB receive
    // buffer to reduce syscall overhead at the cost of a bit more RAM.
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 1048576L); // 1 MiB
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    // Encourage connection reuse and keep-alives so that multiple range
    // requests can share underlying TLS sessions when possible.
    curl_easy_setopt(curl, CURLOPT_PIPEWAIT, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 8L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 60L);
    curl_easy_setopt(curl, CURLOPT_PATH_AS_IS, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTP09_ALLOWED, 1L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 64L);
}

size_t CHTTPClient::writeBodyCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *res = static_cast<HttpResponse *>(userdata);
    size_t total = size * nmemb;
    res->strBody.append(ptr, total);
    return total;
}

size_t CHTTPClient::writeHeaderCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *res = static_cast<HttpResponse *>(userdata);
    size_t total = size * nmemb;
    std::string line(ptr, total);

    auto pos = line.find(':');
    if (pos != std::string::npos)
    {
        std::string name = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.erase(value.begin());
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
            value.pop_back();

        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        res->mapHeaders[name] = value;
        res->mapHeadersLowercase[lower] = value;
    }

    return total;
}

int CHTTPClient::progressCallback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
{
    CHTTPClient *self = static_cast<CHTTPClient *>(clientp);
    if (!self || !self->progressFn)
        return 0;
    return self->progressFn(self->progressOwner.pOwner, dltotal, dlnow, ultotal, ulnow);
}

bool CHTTPClient::Head(const std::string &url, const HeadersMap &headers, HttpResponse &out)
{
    if (!curl)
        curl = curl_easy_init();
    if (!curl)
        return false;

    out = HttpResponse{};

    applyCommonOptions(url);
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &CHTTPClient::writeHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out);

    struct curl_slist *hdrs = nullptr;
    for (const auto &kv : headers)
    {
        std::string line = kv.first + ": " + kv.second;
        hdrs = curl_slist_append(hdrs, line.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode res = curl_easy_perform(curl);
    if (hdrs)
        curl_slist_free_all(hdrs);

    if (res != CURLE_OK)
    {
        out.errMessage = curl_easy_strerror(res);
        Logger::Logf("HTTP HEAD error url=%s err=%s", url.c_str(), out.errMessage.c_str());
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.iCode);
    return true;
}

bool CHTTPClient::Get(const std::string &url, const HeadersMap &headers, HttpResponse &out)
{
    if (!curl)
        curl = curl_easy_init();
    if (!curl)
        return false;

    out = HttpResponse{};

    applyCommonOptions(url);
    // Ensure we perform a clean HTTP GET, not reusing any previous
    // custom method (e.g. PROPFIND) or upload settings from earlier calls.
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CHTTPClient::writeBodyCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &CHTTPClient::writeHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out);

    struct curl_slist *hdrs = nullptr;
    for (const auto &kv : headers)
    {
        std::string line = kv.first + ": " + kv.second;
        hdrs = curl_slist_append(hdrs, line.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode res = curl_easy_perform(curl);
    if (hdrs)
        curl_slist_free_all(hdrs);

    if (res != CURLE_OK)
    {
        out.errMessage = curl_easy_strerror(res);
        Logger::Logf("HTTP GET error url=%s err=%s", url.c_str(), out.errMessage.c_str());
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.iCode);
    return true;
}

bool CHTTPClient::DownloadFile(const std::string &outputPath, const std::string &url, long &status)
{
    if (!curl)
        curl = curl_easy_init();
    if (!curl)
        return false;

    FILE *file = std::fopen(outputPath.c_str(), "wb");
    if (!file)
        return false;

    applyCommonOptions(url);
    // Ensure we are doing a clean GET with no stale options from previous
    // requests on this CURL handle (e.g. PROPFIND with custom headers).
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, nullptr);
    curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, 0L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    // Disable header callbacks for plain file downloads; previous calls may
    // have installed a header callback with a stack-based user pointer, which
    // is unsafe to reuse here.
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, nullptr);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, nullptr);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nullptr);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);

    CURLcode res = curl_easy_perform(curl);
    std::fclose(file);

    if (res != CURLE_OK)
    {
        status = 0;
        Logger::Logf("HTTP download error url=%s err=%s", url.c_str(), curl_easy_strerror(res));
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    Logger::Logf("HTTP download done url=%s code=%ld", url.c_str(), status);
    return true;
}

bool CHTTPClient::UploadFile(const std::string &inputPath, const std::string &url, long &status)
{
    if (!curl)
        curl = curl_easy_init();
    if (!curl)
        return false;

    FILE *file = std::fopen(inputPath.c_str(), "rb");
    if (!file)
        return false;

    applyCommonOptions(url);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, file);

    CURLcode res = curl_easy_perform(curl);
    std::fclose(file);

    if (res != CURLE_OK)
    {
        status = 0;
        Logger::Logf("HTTP upload error url=%s err=%s", url.c_str(), curl_easy_strerror(res));
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    return true;
}

bool CHTTPClient::CustomRequest(const std::string &method, const std::string &url, const HeadersMap &headers, HttpResponse &out)
{
    if (!curl)
        curl = curl_easy_init();
    if (!curl)
        return false;

    out = HttpResponse{};

    applyCommonOptions(url);
    // Ensure we actually read the response body even if a previous HEAD
    // request set NOBODY or other flags on this CURL handle.
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 0L);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &CHTTPClient::writeBodyCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &CHTTPClient::writeHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out);

    struct curl_slist *hdrs = nullptr;
    for (const auto &kv : headers)
    {
        std::string line = kv.first + ": " + kv.second;
        hdrs = curl_slist_append(hdrs, line.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode res = curl_easy_perform(curl);
    if (hdrs)
        curl_slist_free_all(hdrs);

    if (res != CURLE_OK)
    {
        out.errMessage = curl_easy_strerror(res);
        Logger::Logf("HTTP custom error url=%s err=%s", url.c_str(), out.errMessage.c_str());
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.iCode);
    Logger::Logf("HTTP %s code=%ld url=%s", method.c_str(), out.iCode, url.c_str());
    return true;
}

void CHTTPClient::CleanupSession()
{
    if (curl)
    {
        curl_easy_cleanup(curl);
        curl = nullptr;
    }
}

std::string CHTTPClient::EncodeUrl(const std::string &url)
{
    CURL *c = curl_easy_init();
    if (!c)
        return url;

    std::string out;
    out.reserve(url.size());

    size_t start = 0;
    while (start < url.size())
    {
        size_t slash = url.find('/', start);
        std::string segment = url.substr(start, (slash == std::string::npos) ? std::string::npos : slash - start);

        if (!segment.empty())
        {
            char *escaped = curl_easy_escape(c, segment.c_str(), static_cast<int>(segment.length()));
            if (escaped)
            {
                out += escaped;
                curl_free(escaped);
            }
            else
            {
                out += segment;
            }
        }

        if (slash == std::string::npos)
            break;

        out.push_back('/');
        start = slash + 1;
    }

    curl_easy_cleanup(c);
    return out;
}

std::string CHTTPClient::DecodeUrl(const std::string &url, bool plusAsSpace)
{
    CURL *c = curl_easy_init();
    if (!c)
        return url;
    int len = 0;
    char *decoded = curl_easy_unescape(c, url.c_str(), static_cast<int>(url.length()), &len);
    std::string out = decoded ? std::string(decoded, len) : url;
    if (decoded)
        curl_free(decoded);
    curl_easy_cleanup(c);

    if (plusAsSpace)
    {
        for (char &ch : out)
        {
            if (ch == '+')
                ch = ' ';
        }
    }

    return out;
}
