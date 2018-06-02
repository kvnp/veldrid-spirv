// veldrid-spirv.cpp : Defines the entry point for the console application.
//

#include "veldrid-spirv.hpp"
#include <fstream>
#include "../../ext/SPIRV-Cross/spirv_hlsl.hpp"
#include "ShaderSetCompilationInfo.hpp"
#include <map>
#include "assert.h"

using namespace spirv_cross;
using namespace Veldrid;

void WriteToFile(const std::string& path, const std::string& text)
{
    auto outFile = std::ofstream(path);
    outFile << text;
    outFile.close();
}

int main(int argc, char** argv)
{
    printf("Hey there -- you are actually running stuff.\n");
    auto vsInputPath = std::string(argv[1]);
    auto fsInputPath = std::string(argv[2]);

    auto vsBytes = ReadFile(vsInputPath);
    auto fsBytes = ReadFile(fsInputPath);

    ShaderSetCompilationInfo ssci = {};
    ssci.VertexShader = ShaderCompilationInfo(vsBytes);
    ssci.FragmentShader = ShaderCompilationInfo(fsBytes);

    auto result = Compile(ssci);

    WriteToFile("outvert.hlsl", std::string(result->VertexShader, result->VertexShader + result->VertexShaderLength));
    WriteToFile("outfrag.hlsl", std::string(result->FragmentShader, result->FragmentShader + result->FragmentShaderLength));

    return 0;
}

struct BindingInfo
{
    uint32_t Set;
    uint32_t Binding;
};

bool operator< (const BindingInfo& a, const BindingInfo& b)
{
    return a.Set < b.Set ||
        (a.Set == b.Set && a.Binding < b.Binding);
}

enum ResourceKind
{
    UniformBuffer,
    StorageBufferReadOnly,
    StorageBufferReadWrite,
    SampledImage,
    StorageImage,
    Sampler,
};

struct ResourceInfo
{
    std::string Name;
    ResourceKind Kind;
    std::uint32_t IDs[3]; // 0 == VS, 1 == FS, 2 == CS
};

ResourceKind ClassifyResource(const Compiler& compiler, const Resource& resource)
{
    SPIRType type = compiler.get_type(resource.type_id);
    SPIRType basetype = compiler.get_type(resource.base_type_id);
    switch (type.basetype)
    {
    case SPIRType::BaseType::Struct:
        return ResourceKind::UniformBuffer;
    case SPIRType::BaseType::Image:
        return ResourceKind::SampledImage;
    case SPIRType::BaseType::Sampler:
        return ResourceKind::Sampler;
    }

    return ResourceKind::UniformBuffer;
}

void AddResources(
    std::vector<spirv_cross::Resource> &resources,
    spirv_cross::Compiler &compiler,
    std::map<BindingInfo, ResourceInfo> &allBuffers,
    const uint32_t idIndex)
{
    for (auto& resource : resources)
    {
        ResourceKind kind = ClassifyResource(compiler, resource);
        BindingInfo bi;
        bi.Set = compiler.get_decoration(resource.id, spv::Decoration::DecorationDescriptorSet);
        bi.Binding = compiler.get_decoration(resource.id, spv::Decoration::DecorationBinding);

        ResourceInfo ri = {};
        ri.Name = resource.name;
        ri.IDs[idIndex] = resource.id;
        ri.Kind = kind;

        auto pair = allBuffers.insert(std::pair<BindingInfo, ResourceInfo>(bi, ri));
        if (!pair.second)
        {
            pair.first->second.IDs[idIndex] = resource.id;
            if (pair.first->second.Name != resource.name)
            {
                printf("Same binding slot had different names.");
            }
        }
    }
}

uint32_t GetHLSLResourceIndex(
    ResourceKind kind,
    uint32_t& bufferIndex,
    uint32_t& textureIndex,
    uint32_t& uavIndex,
    uint32_t& samplerIndex)
{
    switch (kind)
    {
    case UniformBuffer:
        return bufferIndex++;
    case StorageBufferReadWrite:
    case StorageImage:
        return uavIndex++;
    case SampledImage:
    case StorageBufferReadOnly:
        return textureIndex++;
    case Sampler:
        return samplerIndex++;
    default:
        assert(false);
    }
}

ShaderCompilationResult* Compile(const ShaderSetCompilationInfo& info)
{
    CompilerHLSL::Options opts = {};
    opts.shader_model = 50;

    std::vector<uint32_t> vsBytes(
        info.VertexShader.Value.ShaderCode,
        info.VertexShader.Value.ShaderCode + info.VertexShader.Value.Length);
    CompilerHLSL vsCompiler(vsBytes);
    vsCompiler.set_hlsl_options(opts);

    std::vector<uint32_t> fsBytes(
        info.FragmentShader.Value.ShaderCode,
        info.FragmentShader.Value.ShaderCode + info.FragmentShader.Value.Length);
    CompilerHLSL fsCompiler(fsBytes);
    fsCompiler.set_hlsl_options(opts);

    auto vsResources = vsCompiler.get_shader_resources();
    auto fsResources = fsCompiler.get_shader_resources();

    std::map<BindingInfo, ResourceInfo> allResources;

    AddResources(vsResources.uniform_buffers, vsCompiler, allResources, 0);
    AddResources(vsResources.storage_buffers, vsCompiler, allResources, 0);
    AddResources(vsResources.separate_images, vsCompiler, allResources, 0);
    AddResources(vsResources.storage_images, vsCompiler, allResources, 0);
    AddResources(vsResources.separate_samplers, vsCompiler, allResources, 0);

    AddResources(fsResources.uniform_buffers, fsCompiler, allResources, 1);
    AddResources(fsResources.storage_buffers, fsCompiler, allResources, 1);
    AddResources(fsResources.separate_images, fsCompiler, allResources, 1);
    AddResources(fsResources.storage_images, fsCompiler, allResources, 1);
    AddResources(fsResources.separate_samplers, fsCompiler, allResources, 1);

    uint32_t bufferIndex = 0;
    uint32_t textureIndex = 0;
    uint32_t uavIndex = 0;
    uint32_t samplerIndex = 0;
    for (auto& it : allResources)
    {
        uint32_t index = GetHLSLResourceIndex(it.second.Kind, bufferIndex, textureIndex, uavIndex, samplerIndex);

        uint32_t vsID = it.second.IDs[0];
        if (vsID != 0)
        {
            vsCompiler.set_decoration(it.second.IDs[0], spv::Decoration::DecorationBinding, index);
        }
        uint32_t fsID = it.second.IDs[1];
        if (fsID != 0)
        {
            fsCompiler.set_decoration(it.second.IDs[1], spv::Decoration::DecorationBinding, index);
        }
    }

    std::string vsText = vsCompiler.compile();
    std::string fsText = fsCompiler.compile();
    ShaderCompilationResult* result = new ShaderCompilationResult();
    result->Succeeded = true;

    uint32_t vsLength = static_cast<uint32_t>(vsText.length());
    result->VertexShader = new uint8_t[vsLength];
    memcpy(result->VertexShader, vsText.c_str(), vsLength);
    result->VertexShaderLength = vsLength;

    uint32_t fsLength = static_cast<uint32_t>(fsText.length());
    result->FragmentShader = new uint8_t[fsLength];
    memcpy(result->FragmentShader, fsText.c_str(), fsLength);
    result->FragmentShaderLength = fsLength;

    return result;
}

void FreeResult(ShaderCompilationResult* result)
{
    delete result;
}

std::vector<uint32_t> ReadFile(std::string path)
{
    std::ifstream is(path, std::ios::binary | std::ios::in | std::ios::ate);
    size_t size = is.tellg();
    is.seekg(0, std::ios::beg);
    char* shaderCode = new char[size];
    is.read(shaderCode, size);
    is.close();

    std::vector<uint32_t> ret(size / 4);
    memcpy(ret.data(), shaderCode, size);

    delete[] shaderCode;
    return ret;
}