#ifndef WEBDAV_H
#define WEBDAV_H

#include <string>
#include <vector>
#include "clients/baseclient.h"
#include "clients/remote_client.h"
#include "common.h"

class WebDAVClient : public BaseClient
{
public:
    int Connect(const std::string &url, const std::string &user, const std::string &pass);
    int Mkdir(const std::string &path);
    int Rmdir(const std::string &path, bool recursive);
    int Rename(const std::string &src, const std::string &dst);
    int Delete(const std::string &path);
    int Copy(const std::string &from, const std::string &to);
    int Move(const std::string &from, const std::string &to);
    int Put(const std::string &inputfile, const std::string &path, uint64_t offset = 0);
    int Size(const std::string &path, int64_t *size);
    int Get(const std::string &outputfile, const std::string &path, uint64_t offset = 0) override;
    std::vector<DirEntry> ListDir(const std::string &path);
    ClientType clientType();
    uint32_t SupportedActions();
    static std::string GetHttpUrl(std::string url);

private:
    bool PropFind(const std::string &path, int depth, CHTTPClient::HttpResponse &res);
    bool ProbeRangeSupport(const std::string &encodedUrl);
    int GetRangedSequential(const std::string &outputfile,
                            const std::string &encodedUrl,
                            int64_t size,
                            int64_t chunk_size);
    int GetRangedSequential(const std::string &outputfile,
                            const std::string &encodedUrl,
                            int64_t size,
                            int64_t chunk_size,
                            int64_t start_offset);
    int GetRangedSequentialSplit(const std::string &outputfile,
                                 const std::string &encodedUrl,
                                 int64_t size,
                                 int64_t chunk_size,
                                 int64_t start_offset,
                                 uint64_t partSize);
    int GetRangedParallelSplit(const std::string &outputfile,
                               const std::string &encodedUrl,
                               int64_t size,
                               int64_t chunk_size,
                               int parallel,
                               uint64_t partSize);
    int GetRangedParallel(const std::string &outputfile,
                          const std::string &encodedUrl,
                          int64_t size,
                          int64_t chunk_size,
                          int parallel);
};

#endif
