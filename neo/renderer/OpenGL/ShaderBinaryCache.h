#pragma once
#include "precompiled.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <list>
#include <memory>

class idShaderBinaryCache {
public:
    idShaderBinaryCache();
    ~idShaderBinaryCache();

    void Init();
    void Shutdown();

    bool LoadBinary( GLuint program, const std::string& shaderSource,
                     const std::string& name, const std::string& additional = "" );
    void SaveBinary( GLuint program, const std::string& shaderSource,
                     const std::string& name, const std::string& additional = "" );
    void Clear();
    void SetMaxRAMCacheSize( size_t maxBytes );

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};