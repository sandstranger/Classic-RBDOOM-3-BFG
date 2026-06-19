#include <sys/stat.h>
#include <dirent.h>
#include <ctime>
#include <algorithm>
#include <sys/time.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <queue>
#include "precompiled.h"
#include "RenderCommon.h"
#include "TextureCache.h"

static const char CACHE_MAGIC[4] = {'T', 'C', '0', '1'};
static const uint32_t CACHE_VERSION = 1;
extern bool g_enableTextureCache;

class BufferPool {
public:
    static BufferPool& Instance() {
        static BufferPool instance;
        return instance;
    }

    uint8_t* Acquire(size_t requestedSize) {
        if (requestedSize == 0) return nullptr;

        size_t bucketSize = RoundUpToPowerOfTwo(requestedSize);
        if (bucketSize < 4096) bucketSize = 4096;

        size_t idx = GetBucketIndex(bucketSize);
        if (idx >= NUM_BUCKETS) return nullptr;

        Bucket& bucket = m_buckets[idx];
        std::lock_guard<std::mutex> lock(bucket.mutex);

        if (bucket.freeBuffers.empty()) {
            uint8_t* newBuf = new (std::nothrow) uint8_t[bucketSize];
            if (!newBuf) return nullptr;
            bucket.allBuffers.push_back(newBuf);
            m_totalAllocated.fetch_add(bucketSize, std::memory_order_relaxed);
            return newBuf;
        }
        uint8_t* buf = bucket.freeBuffers.front();
        bucket.freeBuffers.pop();
        return buf;
    }

    void Release(uint8_t* buf, size_t originalRequestedSize) {
        if (!buf) return;

        size_t bucketSize = RoundUpToPowerOfTwo(originalRequestedSize);
        if (bucketSize < 4096) bucketSize = 4096;

        size_t idx = GetBucketIndex(bucketSize);
        if (idx >= NUM_BUCKETS) return;

        Bucket& bucket = m_buckets[idx];
        std::lock_guard<std::mutex> lock(bucket.mutex);
        bucket.freeBuffers.push(buf);
    }

    size_t GetTotalAllocated() const { return m_totalAllocated.load(std::memory_order_relaxed); }

    void Shutdown() {
        for (auto& bucket : m_buckets) {
            std::lock_guard<std::mutex> lock(bucket.mutex);
            for (uint8_t* buf : bucket.allBuffers) {
                delete[] buf;
            }
            bucket.allBuffers.clear();
            while (!bucket.freeBuffers.empty()) bucket.freeBuffers.pop();
        }
        m_totalAllocated.store(0, std::memory_order_relaxed);
    }

private:
    BufferPool() = default;
    ~BufferPool() { Shutdown(); }

    static inline size_t RoundUpToPowerOfTwo(size_t v) {
        if (v <= 1) return 1;
        return (size_t)1 << (64 - __builtin_clzll(v - 1));
    }

    static inline size_t GetBucketIndex(size_t size) {
        return 63 - __builtin_clzll(size) - 12;
    }

    struct Bucket {
        std::vector<uint8_t*> allBuffers;
        std::queue<uint8_t*> freeBuffers;
        std::mutex mutex;
    };

    static constexpr size_t NUM_BUCKETS = 20;
    Bucket m_buckets[NUM_BUCKETS];
    std::atomic<size_t> m_totalAllocated{0};
};

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

    common->Printf("TextureCache initialized: %zu entries, %zu MB used\n",
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
    while (m_pendingJobs.load(std::memory_order_relaxed) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::string idTextureCache::GetCachePath(uint64_t hash) {
    char buf[32];
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

    return (memcmp(header.magic, CACHE_MAGIC, 4) == 0 &&
            header.version == CACHE_VERSION &&
            header.hash == expectedHash &&
            (int)header.width == expectedWidth &&
            (int)header.height == expectedHeight &&
            (int)header.format == expectedFormat &&
            (int)header.mipCount == expectedMipCount);
}

bool idTextureCache::TryGetCachedETC2(const char* textureName,
                                      const void* dxtData, size_t dxtSize,
                                      int width, int height, int format, int mipCount,
                                      std::vector<uint8_t>& outBuffer, size_t* outSize) {
    uint64_t hash = ComputeTextureHash(dxtData, dxtSize, width, height, format);
    std::string cachePath = GetCachePath(hash);
    std::string fileName = cachePath.substr(cachePath.find_last_of('/') + 1);

    {
        std::lock_guard<std::mutex> lock(m_indexMutex);
        if (!m_initialized) return false;

        auto it = m_entries.find(fileName);
        if (it == m_entries.end()) return false;

        it->second.lastAccessTime = time(nullptr);
    }

    struct stat st;
    if (stat(cachePath.c_str(), &st) != 0) return false;

    if (!ValidateCacheFile(cachePath.c_str(), hash, width, height, format, mipCount)) {
        remove(cachePath.c_str());
        std::lock_guard<std::mutex> lock(m_indexMutex);
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
    if (outBuffer.size() < header.dataSize)
    {
        outBuffer.resize(header.dataSize);
    }
    auto* buffer = reinterpret_cast<std::byte *>(outBuffer.data());
    if (file->Read(buffer, header.dataSize) != (int)header.dataSize) {
        delete file;
        return false;
    }
    delete file;
    utimes(cachePath.c_str(), nullptr);
    *outSize = header.dataSize;
    return true;
}

void idTextureCache::SaveToCacheAsync(const char* textureName,
                                      const void* dxtData, size_t dxtSize,
                                      int width, int height, int format, int mipCount,
                                      const void* etc2Data, size_t etc2Size) {
    if (!m_initialized) return;

    if (m_pendingJobs.load(std::memory_order_relaxed) >= m_maxQueueSize.load(std::memory_order_relaxed)) {
        return;
    }

    uint64_t hash = ComputeTextureHash(dxtData, dxtSize, width, height, format);
    std::string cachePath = GetCachePath(hash);
    std::string fileName = cachePath.substr(cachePath.find_last_of('/') + 1);

    {
        std::lock_guard<std::mutex> lock(m_indexMutex);
        if (m_entries.find(fileName) != m_entries.end()) return;
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
        m_pendingJobs.fetch_add(1, std::memory_order_release);
    }
    m_queueCV.notify_one();
}

void idTextureCache::WorkerThreadFunc() {
    while (true) {
        CacheWriteJob job;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCV.wait(lock, [this]() {
                return !m_jobQueue.empty() || m_shutdownRequested.load(std::memory_order_acquire);
            });

            if (m_shutdownRequested.load(std::memory_order_acquire) && m_jobQueue.empty()) {
                break;
            }

            job = std::move(m_jobQueue.front());
            m_jobQueue.pop();
        }

        ProcessJob(job);
        m_pendingJobs.fetch_sub(1, std::memory_order_release);
    }
}

void idTextureCache::ProcessJob(const CacheWriteJob& job) {
    idFile* file = fileSystem->OpenFileWrite(job.tempPath.c_str());
    if (!file) return;

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

    size_t toRemove = sorted.size() / 5;
    if (toRemove < 1) toRemove = 1;

    std::nth_element(sorted.begin(), sorted.begin() + toRemove, sorted.end(),
                     [](const auto& a, const auto& b) {
                         return a.second->lastAccessTime < b.second->lastAccessTime;
                     });

    for (size_t i = 0; i < toRemove && i < sorted.size(); ++i) {
        const std::string& name = sorted[i].first;
        CacheEntry* entry = sorted[i].second;

        if (remove(entry->filename.c_str()) == 0) {
            m_currentSizeBytes -= entry->size;
            m_entries.erase(name);
        }
    }
}

bool idTextureCache::TryGetFromRamCache(uint64_t hash, std::vector<uint8_t>& outBuffer, size_t* outSize) {
    size_t dataSize = 0;

    {
        std::lock_guard<std::mutex> lock(m_ramCacheMutex);
        auto it = m_ramCacheIndex.find(hash);
        if (it == m_ramCacheIndex.end()) return false;
        dataSize = it->second->etc2Data.size();
    }

    if (outBuffer.size() < dataSize)
    {
        outBuffer.resize(dataSize);
    }

    uint8_t* buffer = outBuffer.data();
    if (!buffer) return false;

    {
        std::lock_guard<std::mutex> lock(m_ramCacheMutex);
        auto it = m_ramCacheIndex.find(hash);
        if (it == m_ramCacheIndex.end()) {
            return false;
        }

        auto& entry = *it->second;
        m_ramCacheList.splice(m_ramCacheList.begin(), m_ramCacheList, it->second);

        static thread_local uint64_t accessCounter = 0;
        if (++accessCounter % 128 == 0) {
            entry.lastAccessTime = std::chrono::steady_clock::now().time_since_epoch().count();
        }

        memcpy(buffer, entry.etc2Data.data(), dataSize);
    }

    *outSize = dataSize;
    return true;
}

void idTextureCache::SaveToRamCache(uint64_t hash, const void* etc2Data, size_t etc2Size,
                                    uint32_t width, uint32_t height, uint32_t format) {
    if (etc2Data == nullptr || etc2Size == 0) return;

    std::lock_guard<std::mutex> lock(m_ramCacheMutex);

    if (m_ramCacheIndex.find(hash) != m_ramCacheIndex.end()) return;

    m_ramCacheList.emplace_front();
    auto& entry = m_ramCacheList.front();

    entry.hash = hash;
    entry.etc2Data.resize(etc2Size);
    std::memcpy(entry.etc2Data.data(), etc2Data, etc2Size);
    entry.width = width;
    entry.height = height;
    entry.format = format;
    entry.lastAccessTime = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    entry.size = etc2Size;

    m_ramCacheIndex.emplace(hash, m_ramCacheList.begin());
    m_ramCacheCurrentSize += etc2Size;

    EvictRamCacheIfNeeded();
}

void idTextureCache::EvictRamCacheIfNeeded() {
    if (m_ramCacheCurrentSize <= m_ramCacheMaxSize) return;

    size_t toRemove = m_ramCacheList.size() / 5;
    if (toRemove < 1) toRemove = 1;

    for (size_t i = 0; i < toRemove && !m_ramCacheList.empty(); ++i) {
        auto& oldest = m_ramCacheList.back();
        m_ramCacheCurrentSize -= oldest.size;
        m_ramCacheIndex.erase(oldest.hash);
        m_ramCacheList.pop_back();
    }
}

void idTextureCache::ClearRamCache() {
    std::lock_guard<std::mutex> lock(m_ramCacheMutex);
    m_ramCacheList.clear();
    m_ramCacheIndex.clear();
    m_ramCacheCurrentSize = 0;
}

void idTextureCache::ForceEvictRamCache(float fraction) {
    std::lock_guard<std::mutex> lock(m_ramCacheMutex);

    size_t toRemove = static_cast<size_t>(m_ramCacheList.size() * fraction);
    if (toRemove < 1 && !m_ramCacheList.empty()) toRemove = 1;

    for (size_t i = 0; i < toRemove && !m_ramCacheList.empty(); ++i) {
        auto& oldest = m_ramCacheList.back();
        m_ramCacheCurrentSize -= oldest.size;
        m_ramCacheIndex.erase(oldest.hash);
        m_ramCacheList.pop_back();
    }
}

extern "C" {
__attribute__((used)) __attribute__((visibility("default")))
void ClearRamCache() {
    if (g_enableTextureCache) {
        idTextureCache::Instance().Flush();
        idTextureCache::Instance().ClearRamCache();
    }
}
}