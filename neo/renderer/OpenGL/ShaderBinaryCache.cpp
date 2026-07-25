#include "ShaderBinaryCache.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <list>
#include <vector>
#include <string>
#include <cstring>
#include <ctime>
#include <functional>
#include "framework/Common.h"
#include "SDL3/SDL_system.h"

class idShaderBinaryCache::Impl {
public:
    Impl() : maxRAMCacheBytes(32 * 1024 * 1024), currentRAMCacheBytes(0),
             binarySupported(false), numBinaryFormats(0), stopThread(false) {}

    ~Impl() { Shutdown(); }

    void Init() {
        if (!GLAD_GL_OES_get_program_binary) {
            idLib::Printf("idShaderBinaryCache: GL_OES_get_program_binary not supported.\n");
            binarySupported = false;
            return;
        }
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &numBinaryFormats);
        if (numBinaryFormats <= 0) {
            idLib::Printf("idShaderBinaryCache: No program binary formats available.\n");
            binarySupported = false;
            return;
        }
        supportedFormats.resize(numBinaryFormats);
        glGetIntegerv(GL_PROGRAM_BINARY_FORMATS, (GLint*)supportedFormats.data());
        binarySupported = true;
        idLib::Printf("idShaderBinaryCache: %d program binary formats supported.\n", numBinaryFormats);

        cacheFolder = std::string(SDL_GetAndroidCachePath()) + "/id_tech_4_5_shaders_cache/";
        fileSystem->CreateOSPath(cacheFolder.c_str());

        stopThread = false;
        saveThread = std::thread(&Impl::SaveWorker, this);
    }

    void Shutdown() {
        if (saveThread.joinable()) {
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                stopThread = true;
                queueCond.notify_one();
            }
            saveThread.join();
        }
        std::lock_guard<std::mutex> lock(ramCacheMutex);
        ramCache.clear();
        lruList.clear();
        currentRAMCacheBytes = 0;
    }

    bool LoadBinary(GLuint program, const std::string& shaderSource,
                    const std::string& name, const std::string& additional) {
        if (!binarySupported) return false;

        std::string hash = ComputeHash(shaderSource, additional);
        std::vector<unsigned char> binaryData;
        GLenum format;

        {
            std::lock_guard<std::mutex> lock(ramCacheMutex);
            if (GetFromRAMCacheLocked(hash, binaryData, format)) {
                idLib::Printf("Loading shader binary from RAM cache: %s\n", name.c_str());
            }
        }
        if (!binaryData.empty()) {
            glProgramBinaryOES(program, format, binaryData.data(), binaryData.size());
            GLint linkStatus;
            glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
            if (linkStatus == GL_TRUE) return true;
            else {
                std::lock_guard<std::mutex> lock(ramCacheMutex);
                RemoveFromRAMCacheLocked(hash);
                return false;
            }
        }

        if (ReadBinaryFromFile(hash, binaryData, format)) {
            idLib::Printf("Loading shader binary from disk: %s\n", name.c_str());
            glProgramBinaryOES(program, format, binaryData.data(), binaryData.size());
            GLint linkStatus;
            glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
            if (linkStatus == GL_TRUE) {
                std::lock_guard<std::mutex> lock(ramCacheMutex);
                AddToRAMCacheLocked(hash, binaryData.data(), binaryData.size(), format, name);
                return true;
            } else {
                fileSystem->RemoveFile(GetCachePath(hash).c_str());
                return false;
            }
        }
        return false;
    }

    void SaveBinary(GLuint program, const std::string& shaderSource,
                    const std::string& name, const std::string& additional) {
        if (!binarySupported) return;

        GLint binaryLength = 0;
        glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH_OES, &binaryLength);
        if (binaryLength <= 0) return;

        GLenum format = 0;
        std::vector<unsigned char> binaryData(binaryLength);
        glGetProgramBinaryOES(program, binaryLength, nullptr, &format, binaryData.data());

        if (!IsFormatSupported(format)) {
            idLib::Warning("Unsupported binary format for shader %s", name.c_str());
            return;
        }

        std::string hash = ComputeHash(shaderSource, additional);
        {
            std::lock_guard<std::mutex> lock(ramCacheMutex);
            if (ramCache.find(hash) == ramCache.end()) {
                AddToRAMCacheLocked(hash, binaryData.data(), binaryLength, format, name);
            }
        }

        SaveTask task{hash, name, std::move(binaryData), format};
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            saveQueue.push_back(std::move(task));
            queueCond.notify_one();
        }
    }

    void Clear() {
        if (saveThread.joinable()) {
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                stopThread = true;
                saveQueue.clear();
                queueCond.notify_one();
            }
            saveThread.join();
            stopThread = false;
            saveThread = std::thread(&Impl::SaveWorker, this);
        }

        std::lock_guard<std::mutex> lock(ramCacheMutex);
        ramCache.clear();
        lruList.clear();
        currentRAMCacheBytes = 0;
    }

    void SetMaxRAMCacheSize(size_t maxBytes) {
        std::lock_guard<std::mutex> lock(ramCacheMutex);
        maxRAMCacheBytes = maxBytes;
        if (currentRAMCacheBytes > maxRAMCacheBytes) EvictLRULocked(0);
    }

private:
    struct CachedBinary {
        std::vector<unsigned char> data;
        GLenum format;
        size_t size;
        std::string hash;
        std::string name;
        time_t lastAccess;
    };
    struct SaveTask {
        std::string hash;
        std::string name;
        std::vector<unsigned char> data;
        GLenum format;
    };

    std::unordered_map<std::string, CachedBinary> ramCache;
    std::list<std::string> lruList;
    std::mutex ramCacheMutex;

    std::vector<SaveTask> saveQueue;
    std::mutex queueMutex;
    std::condition_variable queueCond;
    std::thread saveThread;
    std::atomic<bool> stopThread;

    size_t maxRAMCacheBytes;
    size_t currentRAMCacheBytes;
    bool binarySupported;
    GLint numBinaryFormats;
    std::vector<GLenum> supportedFormats;
    std::string cacheFolder;

    bool GetFromRAMCacheLocked(const std::string& hash, std::vector<unsigned char>& outData, GLenum& outFormat) {
        auto it = ramCache.find(hash);
        if (it == ramCache.end()) return false;
        CachedBinary& bin = it->second;
        bin.lastAccess = time(nullptr);
        lruList.remove(hash);
        lruList.push_back(hash);
        outData = bin.data;
        outFormat = bin.format;
        return true;
    }

    void AddToRAMCacheLocked(const std::string& hash, const unsigned char* data, size_t size, GLenum format, const std::string& name) {
        if (!binarySupported) return;
        if (currentRAMCacheBytes + size > maxRAMCacheBytes) EvictLRULocked(size);
        RemoveFromRAMCacheLocked(hash);
        CachedBinary bin;
        bin.data.assign(data, data + size);
        bin.format = format;
        bin.size = size;
        bin.hash = hash;
        bin.name = name;
        bin.lastAccess = time(nullptr);
        ramCache.emplace(hash, std::move(bin));
        lruList.push_back(hash);
        currentRAMCacheBytes += size;
    }

    void RemoveFromRAMCacheLocked(const std::string& hash) {
        auto it = ramCache.find(hash);
        if (it != ramCache.end()) {
            currentRAMCacheBytes -= it->second.size;
            ramCache.erase(it);
            lruList.remove(hash);
        }
    }

    void EvictLRULocked(size_t neededSpace) {
        while (currentRAMCacheBytes + neededSpace > maxRAMCacheBytes && !lruList.empty()) {
            const std::string oldestKey = lruList.front();
            lruList.pop_front();
            auto it = ramCache.find(oldestKey);
            if (it != ramCache.end()) {
                currentRAMCacheBytes -= it->second.size;
                idLib::Printf("Evicted shader binary from RAM cache: %s (%zu bytes)\n",
                              it->second.name.c_str(), it->second.size);
                ramCache.erase(it);
            }
        }
    }

    std::string ComputeHash(const std::string& shaderSource, const std::string& additional) {
        std::string combined = shaderSource + additional;
        size_t h = std::hash<std::string>{}(combined);
        char buf[32];
        snprintf(buf, sizeof(buf), "%zx", h);
        return std::string(buf);
    }
    std::string GetCachePath(const std::string& hash) { return cacheFolder + hash + ".bin"; }

    bool ReadBinaryFromFile(const std::string& hash, std::vector<unsigned char>& outData, GLenum& outFormat) {
        std::string path = GetCachePath(hash);
        void* fileBuffer = nullptr;
        int length = fileSystem->ReadFile(path.c_str(), &fileBuffer);
        if (length <= 4) {
            if (fileBuffer) Mem_Free(fileBuffer);
            return false;
        }
        outFormat = *(GLenum*)fileBuffer;
        if (!IsFormatSupported(outFormat)) {
            Mem_Free(fileBuffer);
            fileSystem->RemoveFile(path.c_str());
            return false;
        }
        size_t dataSize = length - 4;
        outData.resize(dataSize);
        memcpy(outData.data(), (unsigned char*)fileBuffer + 4, dataSize);
        Mem_Free(fileBuffer);
        return true;
    }

    bool WriteBinaryToFile(const std::string& hash, const unsigned char* data, size_t size, GLenum format) {
        std::string path = GetCachePath(hash);
        std::vector<unsigned char> buffer(size + 4);
        *(GLenum*)buffer.data() = format;
        memcpy(buffer.data() + 4, data, size);
        return fileSystem->WriteFile(path.c_str(), buffer.data(), buffer.size(), "fs_savepath");
    }

    bool IsFormatSupported(GLenum format) {
        for (GLenum f : supportedFormats) if (f == format) return true;
        return false;
    }

    void SaveWorker() {
        while (true) {
            SaveTask task;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCond.wait(lock, [this]() { return !saveQueue.empty() || stopThread; });
                if (stopThread && saveQueue.empty()) break;
                task = std::move(saveQueue.front());
                saveQueue.erase(saveQueue.begin());
            }
            if (!WriteBinaryToFile(task.hash, task.data.data(), task.data.size(), task.format)) {
                idLib::Warning("Failed to write shader binary to disk: %s", task.name.c_str());
            } else {
                idLib::Printf("Saved shader binary to disk: %s (%zu bytes)\n", task.name.c_str(), task.data.size());
            }
        }
    }
};

idShaderBinaryCache::idShaderBinaryCache() : pImpl(std::make_unique<Impl>()) {}
idShaderBinaryCache::~idShaderBinaryCache() = default;
void idShaderBinaryCache::Init() { pImpl->Init();
}
void idShaderBinaryCache::Shutdown() { pImpl->Shutdown(); }
bool idShaderBinaryCache::LoadBinary(GLuint program, const std::string& shaderSource,
                                     const std::string& name, const std::string& additional) {
    return pImpl->LoadBinary(program, shaderSource, name, additional);
}
void idShaderBinaryCache::SaveBinary(GLuint program, const std::string& shaderSource,
                                     const std::string& name, const std::string& additional) {
    pImpl->SaveBinary(program, shaderSource, name, additional);
}
void idShaderBinaryCache::Clear() { pImpl->Clear(); }
void idShaderBinaryCache::SetMaxRAMCacheSize(size_t maxBytes) { pImpl->SetMaxRAMCacheSize(maxBytes); }