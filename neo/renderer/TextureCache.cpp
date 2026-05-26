#include "TextureCache.h"
#include <sys/stat.h>
#include <dirent.h>
#include <ctime>
#include <algorithm>
#include <sys/time.h>
#include "precompiled.h"
#include "RenderCommon.h"

static const char CACHE_MAGIC[4] = {'T', 'C', '0', '1'};
static const uint32_t CACHE_VERSION = 1;

#pragma pack(push, 1)
struct CacheHeader {
    char magic[4];
    uint32_t version;
    uint64_t hash;
    uint32_t width;
    uint32_t height;
    uint32_t mipCount;
    uint32_t format;
    uint64_t timestamp;
    uint32_t dataSize;
};
#pragma pack(pop)

idTextureCache& idTextureCache::Instance() {
    static idTextureCache instance;
    return instance;
}

void idTextureCache::Init(const char* cacheDir, size_t maxSizeBytes) {
    std::lock_guard<std::mutex> lock(m_indexMutex);
    if (m_initialized) return;

    m_cacheDir = cacheDir;
    m_maxSizeBytes = maxSizeBytes;

    struct stat st;
    if (stat(m_cacheDir.c_str(), &st) != 0) {
        std::string cmd = "mkdir -p " + m_cacheDir;
        system(cmd.c_str());
    }

    DIR* dir = opendir(m_cacheDir.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.size() < 6 || name.substr(name.size() - 6) != ".cache") continue;

            std::string fullPath = m_cacheDir + "/" + name;
            struct stat fileStat;
            if (stat(fullPath.c_str(), &fileStat) == 0) {
                CacheEntry e;
                e.filename = fullPath;
                e.size = fileStat.st_size;
                e.lastAccessTime = fileStat.st_mtime;
                e.hash = 0;
                m_entries[name] = e;
                m_currentSizeBytes += fileStat.st_size;
            }
        }
        closedir(dir);
    }

    m_initialized = true;

    m_shutdownRequested.store(false);
    m_workerThread = std::thread(&idTextureCache::WorkerThreadFunc, this);

    struct sched_param param;
    param.sched_priority = 0;
    pthread_setschedparam(m_workerThread.native_handle(), SCHED_NORMAL, &param);

    common->Printf("TextureCache initialized: %zu entries, %zu MB used, worker thread started\n",
                   m_entries.size(), m_currentSizeBytes / (1024 * 1024));
}

void idTextureCache::Shutdown() {
    Flush();

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_shutdownRequested.store(true);
    }
    m_queueCV.notify_one();

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    std::lock_guard<std::mutex> lock(m_indexMutex);
    m_entries.clear();
    m_currentSizeBytes = 0;
    m_initialized = false;
}

void idTextureCache::Flush() {
    while (m_pendingJobs.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    common->Printf("TextureCache: flush complete\n");
}

std::string idTextureCache::GetCachePath(uint64_t hash) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%016llx.cache", (unsigned long long)hash);
    return m_cacheDir + "/" + buf;
}

bool idTextureCache::ValidateCacheFile(const char* path, uint64_t expectedHash,
                                         int expectedWidth, int expectedHeight,
                                         int expectedFormat, int expectedMipCount) {
    idFile* file = fileSystem->OpenFileRead(path);
    if (!file) return false;

    CacheHeader header;
    if (file->Read(&header, sizeof(header)) != sizeof(header)) {
        delete file;
        return false;
    }
    delete file;

    if (memcmp(header.magic, CACHE_MAGIC, 4) != 0) return false;
    if (header.version != CACHE_VERSION) return false;
    if (header.hash != expectedHash) return false;
    if ((int)header.width != expectedWidth) return false;
    if ((int)header.height != expectedHeight) return false;
    if ((int)header.format != expectedFormat) return false;
    if ((int)header.mipCount != expectedMipCount) return false;

    return true;
}

bool idTextureCache::TryGetCachedETC2(const char* textureName,
                                       const void* dxtData, size_t dxtSize,
                                       int width, int height, int format, int mipCount,
                                       std::byte** outBuffer, size_t* outSize) {
    std::lock_guard<std::mutex> lock(m_indexMutex);
    if (!m_initialized) return false;

    uint64_t hash = ComputeTextureHash(dxtData, dxtSize, width, height, format);
    std::string cachePath = GetCachePath(hash);

    struct stat st;
    if (stat(cachePath.c_str(), &st) != 0) return false;

    if (!ValidateCacheFile(cachePath.c_str(), hash, width, height, format, mipCount)) {
        remove(cachePath.c_str());
        std::string fileName = cachePath.substr(cachePath.find_last_of('/') + 1);
        auto it = m_entries.find(fileName);
        if (it != m_entries.end()) {
            m_currentSizeBytes -= it->second.size;
            m_entries.erase(it);
        }
        return false;
    }

    idFile* file = fileSystem->OpenFileRead(cachePath.c_str());
    if (!file) return false;

    CacheHeader header;
    file->Read(&header, sizeof(header));

    std::byte* buffer = (std::byte*)Mem_Alloc(header.dataSize, TAG_TEMP);
    if (!buffer) {
        delete file;
        return false;
    }

    if (file->Read(buffer, header.dataSize) != (int)header.dataSize) {
        Mem_Free(buffer);
        delete file;
        return false;
    }
    delete file;

    utimes(cachePath.c_str(), nullptr);
    std::string fileName = cachePath.substr(cachePath.find_last_of('/') + 1);
    auto it = m_entries.find(fileName);
    if (it != m_entries.end()) {
        it->second.lastAccessTime = time(nullptr);
    }

    *outBuffer = buffer;
    *outSize = header.dataSize;
    return true;
}

void idTextureCache::SaveToCacheAsync(const char* textureName,
                                       const void* dxtData, size_t dxtSize,
                                       int width, int height, int format, int mipCount,
                                       const void* etc2Data, size_t etc2Size) {
    if (!m_initialized) return;

    if (m_pendingJobs.load() >= m_maxQueueSize.load()) {
        common->DPrintf("TextureCache: queue full, skipping %s\n", textureName);
        return;
    }

    uint64_t hash = ComputeTextureHash(dxtData, dxtSize, width, height, format);
    std::string cachePath = GetCachePath(hash);

    {
        std::lock_guard<std::mutex> lock(m_indexMutex);
        if (m_entries.find(cachePath.substr(cachePath.find_last_of('/') + 1)) != m_entries.end()) {
            return;
        }
    }

    CacheWriteJob job;
    job.cachePath = cachePath;
    job.tempPath = cachePath + ".tmp";
    job.hash = hash;
    job.width = width;
    job.height = height;
    job.mipCount = mipCount;
    job.format = format;
    job.timestamp = time(nullptr);

    job.etc2Data.resize(etc2Size);
    memcpy(job.etc2Data.data(), etc2Data, etc2Size);

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_jobQueue.push(std::move(job));
        m_pendingJobs.fetch_add(1);
    }
    m_queueCV.notify_one();
}

void idTextureCache::WorkerThreadFunc() {
    common->Printf("TextureCache: worker thread started (tid=%d)\n", 
                   (int)std::hash<std::thread::id>{}(std::this_thread::get_id()));

    while (true) {
        CacheWriteJob job;

        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCV.wait(lock, [this]() {
                return !m_jobQueue.empty() || m_shutdownRequested.load();
            });

            if (m_shutdownRequested.load() && m_jobQueue.empty()) {
                break;
            }

            job = std::move(m_jobQueue.front());
            m_jobQueue.pop();
        }

        ProcessJob(job);
        m_pendingJobs.fetch_sub(1);
    }

    common->Printf("TextureCache: worker thread exiting\n");
}

void idTextureCache::ProcessJob(const CacheWriteJob& job) {
    idFile* file = fileSystem->OpenFileWrite(job.tempPath.c_str());
    if (!file) {
        common->Warning("TextureCache: failed to open %s for writing", job.tempPath.c_str());
        return;
    }

    CacheHeader header;
    memcpy(header.magic, CACHE_MAGIC, 4);
    header.version = CACHE_VERSION;
    header.hash = job.hash;
    header.width = job.width;
    header.height = job.height;
    header.mipCount = job.mipCount;
    header.format = job.format;
    header.timestamp = job.timestamp;
    header.dataSize = (uint32_t)job.etc2Data.size();

    file->Write(&header, sizeof(header));
    file->Write(job.etc2Data.data(), job.etc2Data.size());
    delete file;

    if (rename(job.tempPath.c_str(), job.cachePath.c_str()) != 0) {
        remove(job.tempPath.c_str());
        common->Warning("TextureCache: rename failed for %s", job.cachePath.c_str());
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_indexMutex);
        std::string fileName = job.cachePath.substr(job.cachePath.find_last_of('/') + 1);
        CacheEntry entry;
        entry.filename = job.cachePath;
        entry.hash = job.hash;
        entry.size = sizeof(CacheHeader) + job.etc2Data.size();
        entry.lastAccessTime = job.timestamp;

        m_entries[fileName] = entry;
        m_currentSizeBytes += entry.size;

        EvictIfNeeded();
    }
}

void idTextureCache::EvictIfNeeded() {
    if (m_currentSizeBytes <= m_maxSizeBytes) return;

    std::vector<std::pair<std::string, CacheEntry*>> sorted;
    sorted.reserve(m_entries.size());
    for (auto& kv : m_entries) {
        sorted.push_back({kv.first, &kv.second});
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) {
                  return a.second->lastAccessTime < b.second->lastAccessTime;
              });

    size_t toRemove = sorted.size() / 5;
    if (toRemove < 1) toRemove = 1;

    for (size_t i = 0; i < toRemove && i < sorted.size(); ++i) {
        const std::string& name = sorted[i].first;
        CacheEntry* entry = sorted[i].second;

        if (remove(entry->filename.c_str()) == 0) {
            m_currentSizeBytes -= entry->size;
            m_entries.erase(name);
        }
    }

    common->Printf("TextureCache: evicted %zu entries, now %zu MB used\n",
                   toRemove, m_currentSizeBytes / (1024 * 1024));
}