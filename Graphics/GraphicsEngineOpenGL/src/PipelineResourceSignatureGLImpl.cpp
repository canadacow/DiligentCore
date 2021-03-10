/*
 *  Copyright 2019-2021 Diligent Graphics LLC
 *  Copyright 2015-2019 Egor Yusov
 *  
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *  
 *      http://www.apache.org/licenses/LICENSE-2.0
 *  
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence), 
 *  contract, or otherwise, unless required by applicable law (such as deliberate 
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental, 
 *  or consequential damages of any character arising as a result of this License or 
 *  out of the use or inability to use the software (including but not limited to damages 
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and 
 *  all other commercial damages or losses), even if such Contributor has been advised 
 *  of the possibility of such damages.
 */

#include "pch.h"
#include "PipelineResourceSignatureGLImpl.hpp"
#include "RenderDeviceGLImpl.hpp"

namespace Diligent
{

namespace
{

inline bool ResourcesCompatible(const PipelineResourceSignatureGLImpl::ResourceAttribs& lhs,
                                const PipelineResourceSignatureGLImpl::ResourceAttribs& rhs)
{
    // Ignore sampler index.
    // clang-format off
    return lhs.CacheOffset          == rhs.CacheOffset &&
           lhs.ImtblSamplerAssigned == rhs.ImtblSamplerAssigned;
    // clang-format on
}

} // namespace


const char* GetBindingRangeName(BINDING_RANGE Range)
{
    static_assert(BINDING_RANGE_COUNT == 4, "Please update the switch below to handle the new shader resource range");
    switch (Range)
    {
        // clang-format off
        case BINDING_RANGE_UNIFORM_BUFFER: return "Uniform buffer";
        case BINDING_RANGE_TEXTURE:        return "Texture";
        case BINDING_RANGE_IMAGE:          return "Image";
        case BINDING_RANGE_STORAGE_BUFFER: return "Storage buffer";
        // clang-format on
        default:
            return "Unknown";
    }
}

BINDING_RANGE PipelineResourceToBindingRange(const PipelineResourceDesc& Desc)
{
    static_assert(SHADER_RESOURCE_TYPE_LAST == 8, "Please update the switch below to handle the new shader resource type");
    switch (Desc.ResourceType)
    {
        // clang-format off
        case SHADER_RESOURCE_TYPE_CONSTANT_BUFFER:  return BINDING_RANGE_UNIFORM_BUFFER;
        case SHADER_RESOURCE_TYPE_TEXTURE_SRV:      return BINDING_RANGE_TEXTURE;
        case SHADER_RESOURCE_TYPE_BUFFER_SRV:       return (Desc.Flags & PIPELINE_RESOURCE_FLAG_FORMATTED_BUFFER) ? BINDING_RANGE_TEXTURE : BINDING_RANGE_STORAGE_BUFFER;
        case SHADER_RESOURCE_TYPE_TEXTURE_UAV:      return BINDING_RANGE_IMAGE;
        case SHADER_RESOURCE_TYPE_BUFFER_UAV:       return (Desc.Flags & PIPELINE_RESOURCE_FLAG_FORMATTED_BUFFER) ? BINDING_RANGE_IMAGE : BINDING_RANGE_STORAGE_BUFFER;
        case SHADER_RESOURCE_TYPE_INPUT_ATTACHMENT: return BINDING_RANGE_TEXTURE;
        // clang-format on
        case SHADER_RESOURCE_TYPE_SAMPLER:
        case SHADER_RESOURCE_TYPE_ACCEL_STRUCT:
        default:
            UNEXPECTED("Unsupported resource type");
            return BINDING_RANGE_UNKNOWN;
    }
}


PipelineResourceSignatureGLImpl::PipelineResourceSignatureGLImpl(IReferenceCounters*                  pRefCounters,
                                                                 RenderDeviceGLImpl*                  pDeviceGL,
                                                                 const PipelineResourceSignatureDesc& Desc,
                                                                 bool                                 bIsDeviceInternal) :
    TPipelineResourceSignatureBase{pRefCounters, pDeviceGL, Desc, bIsDeviceInternal}
{
    try
    {
        auto& RawAllocator{GetRawAllocator()};
        auto  MemPool = AllocateInternalObjects(RawAllocator, Desc,
                                               [&](FixedLinearAllocator& MemPool) //
                                               {
                                                   MemPool.AddSpace<ResourceAttribs>(Desc.NumResources);
                                                   MemPool.AddSpace<SamplerPtr>(Desc.NumImmutableSamplers);
                                               });

        static_assert(std::is_trivially_destructible<ResourceAttribs>::value,
                      "ResourceAttribs objects must be constructed to be properly destructed in case an excpetion is thrown");
        m_pResourceAttribs  = MemPool.Allocate<ResourceAttribs>(m_Desc.NumResources);
        m_ImmutableSamplers = MemPool.ConstructArray<SamplerPtr>(m_Desc.NumImmutableSamplers);

        CreateLayout();

        const auto NumStaticResStages = GetNumStaticResStages();
        if (NumStaticResStages > 0)
        {
            constexpr SHADER_RESOURCE_VARIABLE_TYPE AllowedVarTypes[] = {SHADER_RESOURCE_VARIABLE_TYPE_STATIC};
            for (Uint32 i = 0; i < m_StaticResStageIndex.size(); ++i)
            {
                Int8 Idx = m_StaticResStageIndex[i];
                if (Idx >= 0)
                {
                    VERIFY_EXPR(static_cast<Uint32>(Idx) < NumStaticResStages);
                    const auto ShaderType = GetShaderTypeFromPipelineIndex(i, GetPipelineType());
                    m_StaticVarsMgrs[Idx].Initialize(*this, RawAllocator, AllowedVarTypes, _countof(AllowedVarTypes), ShaderType);
                }
            }
        }

        if (m_Desc.SRBAllocationGranularity > 1)
        {
            std::array<size_t, MAX_SHADERS_IN_PIPELINE> ShaderVariableDataSizes = {};
            for (Uint32 s = 0; s < GetNumActiveShaderStages(); ++s)
            {
                constexpr SHADER_RESOURCE_VARIABLE_TYPE AllowedVarTypes[] = {SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE, SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC};

                ShaderVariableDataSizes[s] = ShaderVariableManagerGL::GetRequiredMemorySize(*this, AllowedVarTypes, _countof(AllowedVarTypes), GetActiveShaderStageType(s));
            }

            const size_t CacheMemorySize = ShaderResourceCacheGL::GetRequriedMemorySize(m_BindingCount);
            m_SRBMemAllocator.Initialize(m_Desc.SRBAllocationGranularity, GetNumActiveShaderStages(), ShaderVariableDataSizes.data(), 1, &CacheMemorySize);
        }

        m_Hash = CalculateHash();
    }
    catch (...)
    {
        Destruct();
        throw;
    }
}

void PipelineResourceSignatureGLImpl::CreateLayout()
{
    TBindings StaticResCounter = {};

    for (Uint32 s = 0; s < m_Desc.NumImmutableSamplers; ++s)
        GetDevice()->CreateSampler(m_Desc.ImmutableSamplers[s].Desc, &m_ImmutableSamplers[s]);

    for (Uint32 i = 0; i < m_Desc.NumResources; ++i)
    {
        const auto& ResDesc = m_Desc.Resources[i];
        VERIFY(i == 0 || ResDesc.VarType >= m_Desc.Resources[i - 1].VarType, "Resources must be sorted by variable type");

        if (ResDesc.ResourceType == SHADER_RESOURCE_TYPE_SAMPLER)
        {
            const auto ImtblSamplerIdx = FindImmutableSampler(ResDesc.ShaderStages, ResDesc.Name);
            // Create sampler resource without cache space
            new (m_pResourceAttribs + i) ResourceAttribs //
                {
                    ResourceAttribs::InvalidCacheOffset,
                    ImtblSamplerIdx == InvalidImmutableSamplerIndex ? ResourceAttribs::InvalidSamplerInd : ImtblSamplerIdx,
                    ImtblSamplerIdx != InvalidImmutableSamplerIndex //
                };
        }
        else
        {
            const auto Range = PipelineResourceToBindingRange(ResDesc);
            VERIFY_EXPR(Range != BINDING_RANGE_UNKNOWN);

            auto ImtblSamplerIdx = InvalidImmutableSamplerIndex;
            auto SamplerIdx      = ResourceAttribs::InvalidSamplerInd;
            if (ResDesc.ResourceType == SHADER_RESOURCE_TYPE_TEXTURE_SRV)
            {
                // Do not use combined sampler suffix - in OpenGL immutable samplers should be defined for textures directly
                ImtblSamplerIdx = Diligent::FindImmutableSampler(m_Desc.ImmutableSamplers, m_Desc.NumImmutableSamplers,
                                                                 ResDesc.ShaderStages, ResDesc.Name, nullptr);
                if (ImtblSamplerIdx != InvalidImmutableSamplerIndex)
                    SamplerIdx = ImtblSamplerIdx;
                else
                    SamplerIdx = FindAssignedSampler(ResDesc, ResourceAttribs::InvalidSamplerInd);
            }

            Uint32& CacheOffset = m_BindingCount[Range];
            new (m_pResourceAttribs + i) ResourceAttribs //
                {
                    CacheOffset,
                    SamplerIdx,
                    ImtblSamplerIdx != InvalidImmutableSamplerIndex // _ImtblSamplerAssigned
                };
            CacheOffset += ResDesc.ArraySize;

            if (ResDesc.VarType == SHADER_RESOURCE_VARIABLE_TYPE_STATIC)
                StaticResCounter[Range] += ResDesc.ArraySize;
        }
    }

    if (m_pStaticResCache)
    {
        m_pStaticResCache->Initialize(StaticResCounter, GetRawAllocator());
    }
}

size_t PipelineResourceSignatureGLImpl::CalculateHash() const
{
    if (m_Desc.NumResources == 0 && m_Desc.NumImmutableSamplers == 0)
        return 0;

    auto Hash = CalculatePipelineResourceSignatureDescHash(m_Desc);
    for (Uint32 i = 0; i < m_Desc.NumResources; ++i)
    {
        const auto& Attr = m_pResourceAttribs[i];
        HashCombine(Hash, Attr.CacheOffset);
    }

    return Hash;
}

PipelineResourceSignatureGLImpl::~PipelineResourceSignatureGLImpl()
{
    Destruct();
}

void PipelineResourceSignatureGLImpl::Destruct()
{
    if (m_ImmutableSamplers != nullptr)
    {
        for (Uint32 s = 0; s < m_Desc.NumImmutableSamplers; ++s)
            m_ImmutableSamplers[s].~SamplerPtr();

        m_ImmutableSamplers = nullptr;
    }

    m_pResourceAttribs = nullptr;

    TPipelineResourceSignatureBase::Destruct();
}

void PipelineResourceSignatureGLImpl::ApplyBindings(GLObjectWrappers::GLProgramObj& GLProgram,
                                                    GLContextState&                 State,
                                                    SHADER_TYPE                     Stages,
                                                    const TBindings&                BaseBindings) const
{
    VERIFY(GLProgram != 0, "Null GL program");
    State.SetProgram(GLProgram);

    for (Uint32 r = 0; r < GetTotalResourceCount(); ++r)
    {
        const auto& ResDesc = m_Desc.Resources[r];
        const auto& ResAttr = m_pResourceAttribs[r];

        if (ResDesc.ResourceType == SHADER_RESOURCE_TYPE_SAMPLER)
            continue;

        if ((ResDesc.ShaderStages & Stages) == 0)
            continue;

        const auto   Range        = PipelineResourceToBindingRange(ResDesc);
        const Uint32 BindingIndex = BaseBindings[Range] + ResAttr.CacheOffset;

        static_assert(BINDING_RANGE_COUNT == 4, "Please update the switch below to handle the new shader resource range");
        switch (Range)
        {
            case BINDING_RANGE_UNIFORM_BUFFER:
            {
                auto UniformBlockIndex = glGetUniformBlockIndex(GLProgram, ResDesc.Name);
                if (UniformBlockIndex == GL_INVALID_INDEX)
                    break; // Uniform block is defined in resource signature, but not presented in shader program.

                for (Uint32 ArrInd = 0; ArrInd < ResDesc.ArraySize; ++ArrInd)
                {
                    glUniformBlockBinding(GLProgram, UniformBlockIndex + ArrInd, BindingIndex + ArrInd);
                    CHECK_GL_ERROR("glUniformBlockBinding() failed");
                }
                break;
            }
            case BINDING_RANGE_TEXTURE:
            {
                auto UniformLocation = glGetUniformLocation(GLProgram, ResDesc.Name);
                if (UniformLocation < 0)
                    break; // Uniform is defined in resource signature, but not presented in shader program.

                for (Uint32 ArrInd = 0; ArrInd < ResDesc.ArraySize; ++ArrInd)
                {
                    glUniform1i(UniformLocation + ArrInd, BindingIndex + ArrInd);
                    CHECK_GL_ERROR("Failed to set binding point for sampler uniform '", ResDesc.Name, '\'');
                }
                break;
            }
#if GL_ARB_shader_image_load_store
            case BINDING_RANGE_IMAGE:
            {
                auto UniformLocation = glGetUniformLocation(GLProgram, ResDesc.Name);
                if (UniformLocation < 0)
                    break; // Uniform defined in resource signature, but not presented in shader program.

                for (Uint32 ArrInd = 0; ArrInd < ResDesc.ArraySize; ++ArrInd)
                {
                    // glUniform1i for image uniforms is not supported in at least GLES3.2.
                    // glProgramUniform1i is not available in GLES3.0
                    const Uint32 ImgBinding = BindingIndex + ArrInd;
                    glUniform1i(UniformLocation + ArrInd, ImgBinding);
                    if (glGetError() != GL_NO_ERROR)
                    {
                        if (ResDesc.ArraySize > 1)
                        {
                            LOG_WARNING_MESSAGE("Failed to set binding for image uniform '", ResDesc.Name, "'[", ArrInd,
                                                "]. Expected binding: ", ImgBinding,
                                                ". Make sure that this binding is explicitly assigned in shader source code."
                                                " Note that if the source code is converted from HLSL and if images are only used"
                                                " by a single shader stage, then bindings automatically assigned by HLSL->GLSL"
                                                " converter will work fine.");
                        }
                        else
                        {
                            LOG_WARNING_MESSAGE("Failed to set binding for image uniform '", ResDesc.Name,
                                                "'. Expected binding: ", ImgBinding,
                                                ". Make sure that this binding is explicitly assigned in shader source code."
                                                " Note that if the source code is converted from HLSL and if images are only used"
                                                " by a single shader stage, then bindings automatically assigned by HLSL->GLSL"
                                                " converter will work fine.");
                        }
                    }
                }
                break;
            }
#endif
#if GL_ARB_shader_storage_buffer_object
            case BINDING_RANGE_STORAGE_BUFFER:
            {
                auto SBIndex = glGetProgramResourceIndex(GLProgram, GL_SHADER_STORAGE_BLOCK, ResDesc.Name);
                if (SBIndex == GL_INVALID_INDEX)
                    break; // Storage block defined in resource signature, but not presented in shader program.

                if (glShaderStorageBlockBinding)
                {
                    for (Uint32 ArrInd = 0; ArrInd < ResDesc.ArraySize; ++ArrInd)
                    {
                        glShaderStorageBlockBinding(GLProgram, SBIndex + ArrInd, BindingIndex + ArrInd);
                        CHECK_GL_ERROR("glShaderStorageBlockBinding() failed");
                    }
                }
                else
                {
                    const GLenum props[]                 = {GL_BUFFER_BINDING};
                    GLint        params[_countof(props)] = {};
                    glGetProgramResourceiv(GLProgram, GL_SHADER_STORAGE_BLOCK, SBIndex, _countof(props), props, _countof(params), nullptr, params);
                    CHECK_GL_ERROR("glGetProgramResourceiv() failed");

                    if (BindingIndex != static_cast<Uint32>(params[0]))
                    {
                        LOG_WARNING_MESSAGE("glShaderStorageBlockBinding is not available on this device and "
                                            "the engine is unable to automatically assign shader storage block bindindg for '",
                                            ResDesc.Name, "' variable. Expected binding: ", BindingIndex, ", actual binding: ", params[0],
                                            ". Make sure that this binding is explicitly assigned in shader source code."
                                            " Note that if the source code is converted from HLSL and if storage blocks are only used"
                                            " by a single shader stage, then bindings automatically assigned by HLSL->GLSL"
                                            " converter will work fine.");
                    }
                }
                break;
            }
#endif
            default:
                UNEXPECTED("Unsupported shader resource range type.");
        }
    }

    State.SetProgram(GLObjectWrappers::GLProgramObj::Null());
}

void PipelineResourceSignatureGLImpl::CopyStaticResources(ShaderResourceCacheGL& DstResourceCache) const
{
    if (m_pStaticResCache == nullptr)
        return;

    // SrcResourceCache contains only static resources.
    // DstResourceCache contains static, mutable and dynamic resources.
    const auto& SrcResourceCache  = *m_pStaticResCache;
    const auto  StaticResIdxRange = GetResourceIndexRange(SHADER_RESOURCE_VARIABLE_TYPE_STATIC);

    VERIFY_EXPR(SrcResourceCache.GetContentType() == ResourceCacheContentType::Signature);
    VERIFY_EXPR(DstResourceCache.GetContentType() == ResourceCacheContentType::SRB);

    for (Uint32 r = StaticResIdxRange.first; r < StaticResIdxRange.second; ++r)
    {
        const auto& ResDesc = GetResourceDesc(r);
        const auto& ResAttr = GetResourceAttribs(r);
        VERIFY_EXPR(ResDesc.VarType == SHADER_RESOURCE_VARIABLE_TYPE_STATIC);

        if (ResDesc.ResourceType == SHADER_RESOURCE_TYPE_SAMPLER)
            continue; // Skip separate samplers

        static_assert(BINDING_RANGE_COUNT == 4, "Please update the switch below to handle the new shader resource range");
        switch (PipelineResourceToBindingRange(ResDesc))
        {
            case BINDING_RANGE_UNIFORM_BUFFER:
                for (Uint32 ArrInd = 0; ArrInd < ResDesc.ArraySize; ++ArrInd)
                {
                    const auto& SrcCachedRes = SrcResourceCache.GetConstUB(ResAttr.CacheOffset + ArrInd);
                    if (!SrcCachedRes.pBuffer)
                        LOG_ERROR_MESSAGE("No resource is assigned to static shader variable '", GetShaderResourcePrintName(ResDesc, ArrInd), "' in pipeline resource signature '", m_Desc.Name, "'.");

                    DstResourceCache.SetUniformBuffer(ResAttr.CacheOffset + ArrInd, RefCntAutoPtr<BufferGLImpl>{SrcCachedRes.pBuffer});
                }
                break;
            case BINDING_RANGE_STORAGE_BUFFER:
                for (Uint32 ArrInd = 0; ArrInd < ResDesc.ArraySize; ++ArrInd)
                {
                    const auto& SrcCachedRes = SrcResourceCache.GetConstSSBO(ResAttr.CacheOffset + ArrInd);
                    if (!SrcCachedRes.pBufferView)
                        LOG_ERROR_MESSAGE("No resource is assigned to static shader variable '", GetShaderResourcePrintName(ResDesc, ArrInd), "' in pipeline resource signature '", m_Desc.Name, "'.");

                    DstResourceCache.SetSSBO(ResAttr.CacheOffset + ArrInd, RefCntAutoPtr<BufferViewGLImpl>{SrcCachedRes.pBufferView});
                }
                break;
            case BINDING_RANGE_TEXTURE:
                for (Uint32 ArrInd = 0; ArrInd < ResDesc.ArraySize; ++ArrInd)
                {
                    const auto& SrcCachedRes = SrcResourceCache.GetConstTexture(ResAttr.CacheOffset + ArrInd);
                    if (!SrcCachedRes.pView)
                        LOG_ERROR_MESSAGE("No resource is assigned to static shader variable '", GetShaderResourcePrintName(ResDesc, ArrInd), "' in pipeline resource signature '", m_Desc.Name, "'.");

                    if (ResDesc.ResourceType == SHADER_RESOURCE_TYPE_TEXTURE_SRV ||
                        ResDesc.ResourceType == SHADER_RESOURCE_TYPE_INPUT_ATTACHMENT)
                    {
                        const auto HasImmutableSampler = GetImmutableSamplerIdx(ResAttr) != InvalidImmutableSamplerIndex;

                        auto* const pTexViewGl = SrcCachedRes.pView.RawPtr<TextureViewGLImpl>();
                        DstResourceCache.SetTexture(ResAttr.CacheOffset + ArrInd, RefCntAutoPtr<TextureViewGLImpl>{pTexViewGl}, !HasImmutableSampler);
                        if (HasImmutableSampler)
                        {
                            VERIFY(DstResourceCache.GetConstTexture(ResAttr.CacheOffset + ArrInd).pSampler, "Immutable sampler is not initialized in the cache");
                        }
                    }
                    else if (ResDesc.ResourceType == SHADER_RESOURCE_TYPE_BUFFER_SRV)
                    {
                        auto* const pViewGl = SrcCachedRes.pView.RawPtr<BufferViewGLImpl>();
                        DstResourceCache.SetTexelBuffer(ResAttr.CacheOffset + ArrInd, RefCntAutoPtr<BufferViewGLImpl>{pViewGl});
                    }
                    else
                    {
                        UNEXPECTED("Unexpected resource type");
                    }
                }
                break;
            case BINDING_RANGE_IMAGE:
                for (Uint32 ArrInd = 0; ArrInd < ResDesc.ArraySize; ++ArrInd)
                {
                    const auto& SrcCachedRes = SrcResourceCache.GetConstImage(ResAttr.CacheOffset + ArrInd);
                    if (!SrcCachedRes.pView)
                        LOG_ERROR_MESSAGE("No resource is assigned to static shader variable '", GetShaderResourcePrintName(ResDesc, ArrInd), "' in pipeline resource signature '", m_Desc.Name, "'.");

                    if (ResDesc.ResourceType == SHADER_RESOURCE_TYPE_TEXTURE_UAV)
                    {
                        auto* const pTexViewGl = SrcCachedRes.pView.RawPtr<TextureViewGLImpl>();
                        DstResourceCache.SetTexImage(ResAttr.CacheOffset + ArrInd, RefCntAutoPtr<TextureViewGLImpl>{pTexViewGl});
                    }
                    else if (ResDesc.ResourceType == SHADER_RESOURCE_TYPE_BUFFER_UAV ||
                             ResDesc.ResourceType == SHADER_RESOURCE_TYPE_BUFFER_SRV)
                    {
                        auto* const pViewGl = SrcCachedRes.pView.RawPtr<BufferViewGLImpl>();
                        DstResourceCache.SetBufImage(ResAttr.CacheOffset + ArrInd, RefCntAutoPtr<BufferViewGLImpl>{pViewGl});
                    }
                    else
                    {
                        UNEXPECTED("Unexpected resource type");
                    }
                }
                break;
            default:
                UNEXPECTED("Unsupported shader resource range type.");
        }
    }

#ifdef DILIGENT_DEVELOPMENT
    DstResourceCache.SetStaticResourcesInitialized();
#endif
}

void PipelineResourceSignatureGLImpl::InitSRBResourceCache(ShaderResourceCacheGL& ResourceCache)
{
    ResourceCache.Initialize(m_BindingCount, m_SRBMemAllocator.GetResourceCacheDataAllocator(0));

    // Initialize immutable samplers
    for (Uint32 r = 0; r < m_Desc.NumResources; ++r)
    {
        const auto& ResDesc = GetResourceDesc(r);
        const auto& ResAttr = GetResourceAttribs(r);

        if (ResDesc.ResourceType != SHADER_RESOURCE_TYPE_TEXTURE_SRV)
            continue;

        const auto ImtblSamplerIdx = GetImmutableSamplerIdx(ResAttr);
        if (ImtblSamplerIdx != InvalidImmutableSamplerIndex)
        {
            ISampler* pSampler = m_ImmutableSamplers[ImtblSamplerIdx];
            VERIFY(pSampler != nullptr, "Immutable sampler is not initialized - this is a bug");

            for (Uint32 ArrInd = 0; ArrInd < ResDesc.ArraySize; ++ArrInd)
                ResourceCache.SetSampler(ResAttr.CacheOffset + ArrInd, pSampler);
        }
    }
}

bool PipelineResourceSignatureGLImpl::IsCompatibleWith(const PipelineResourceSignatureGLImpl& Other) const
{
    if (this == &Other)
        return true;

    if (GetHash() != Other.GetHash())
        return false;

    if (m_BindingCount != Other.m_BindingCount)
        return false;

    if (!PipelineResourceSignaturesCompatible(GetDesc(), Other.GetDesc()))
        return false;

    const auto ResCount = GetTotalResourceCount();
    VERIFY_EXPR(ResCount == Other.GetTotalResourceCount());
    for (Uint32 r = 0; r < ResCount; ++r)
    {
        if (!ResourcesCompatible(GetResourceAttribs(r), Other.GetResourceAttribs(r)))
            return false;
    }

    return true;
}

#ifdef DILIGENT_DEVELOPMENT
bool PipelineResourceSignatureGLImpl::DvpValidateCommittedResource(const ShaderResourcesGL::GLResourceAttribs& GLAttribs,
                                                                   RESOURCE_DIMENSION                          ResourceDim,
                                                                   bool                                        IsMultisample,
                                                                   Uint32                                      ResIndex,
                                                                   const ShaderResourceCacheGL&                ResourceCache,
                                                                   const char*                                 ShaderName,
                                                                   const char*                                 PSOName) const
{
    VERIFY_EXPR(ResIndex < m_Desc.NumResources);
    const auto& ResDesc = m_Desc.Resources[ResIndex];
    const auto& ResAttr = m_pResourceAttribs[ResIndex];
    VERIFY(strcmp(ResDesc.Name, GLAttribs.Name) == 0, "Inconsistent resource names");

    if (ResDesc.ResourceType == SHADER_RESOURCE_TYPE_SAMPLER)
        return true; // Skip separate samplers

    VERIFY_EXPR(GLAttribs.ArraySize <= ResDesc.ArraySize);

    bool BindingsOK = true;

    static_assert(BINDING_RANGE_COUNT == 4, "Please update the switch below to handle the new shader resource range");
    switch (PipelineResourceToBindingRange(ResDesc))
    {
        case BINDING_RANGE_UNIFORM_BUFFER:
            for (Uint32 ArrInd = 0; ArrInd < ResDesc.ArraySize; ++ArrInd)
            {
                if (!ResourceCache.IsUBBound(ResAttr.CacheOffset + ArrInd))
                {
                    LOG_ERROR_MESSAGE("No resource is bound to variable '", GetShaderResourcePrintName(GLAttribs, ArrInd),
                                      "' in shader '", ShaderName, "' of PSO '", PSOName, "'");
                    BindingsOK = false;
                }
            }
            break;

        case BINDING_RANGE_STORAGE_BUFFER:
            for (Uint32 ArrInd = 0; ArrInd < ResDesc.ArraySize; ++ArrInd)
            {
                if (!ResourceCache.IsSSBOBound(ResAttr.CacheOffset + ArrInd))
                {
                    LOG_ERROR_MESSAGE("No resource is bound to variable '", GetShaderResourcePrintName(GLAttribs, ArrInd),
                                      "' in shader '", ShaderName, "' of PSO '", PSOName, "'");
                    BindingsOK = false;
                }
            }
            break;

        case BINDING_RANGE_TEXTURE:
            for (Uint32 ArrInd = 0; ArrInd < ResDesc.ArraySize; ++ArrInd)
            {
                const bool IsTexView = (ResDesc.ResourceType == SHADER_RESOURCE_TYPE_TEXTURE_SRV || ResDesc.ResourceType == SHADER_RESOURCE_TYPE_INPUT_ATTACHMENT);
                if (!ResourceCache.IsTextureBound(ResAttr.CacheOffset + ArrInd, IsTexView))
                {
                    LOG_ERROR_MESSAGE("No resource is bound to variable '", GetShaderResourcePrintName(GLAttribs, ArrInd),
                                      "' in shader '", ShaderName, "' of PSO '", PSOName, "'");
                    BindingsOK = false;
                    continue;
                }

                const auto& Tex = ResourceCache.GetConstTexture(ResAttr.CacheOffset + ArrInd);
                if (Tex.pTexture)
                    ValidateResourceViewDimension(ResDesc.Name, ResDesc.ArraySize, ArrInd, Tex.pView.RawPtr<ITextureView>(), ResourceDim, IsMultisample);
                else
                    ValidateResourceViewDimension(ResDesc.Name, ResDesc.ArraySize, ArrInd, Tex.pView.RawPtr<IBufferView>(), ResourceDim, IsMultisample);

                const auto ImmutableSamplerIdx = GetImmutableSamplerIdx(ResAttr);
                if (ImmutableSamplerIdx != InvalidImmutableSamplerIndex)
                {
                    VERIFY(Tex.pSampler != nullptr, "Immutable sampler is not initialized in the cache - this is a bug");
                    VERIFY(Tex.pSampler == m_ImmutableSamplers[ImmutableSamplerIdx], "Immutable sampler initialized in the cache is not valid");
                }
            }
            break;

        case BINDING_RANGE_IMAGE:
            for (Uint32 ArrInd = 0; ArrInd < ResDesc.ArraySize; ++ArrInd)
            {
                const bool IsTexView = (ResDesc.ResourceType == SHADER_RESOURCE_TYPE_TEXTURE_SRV || ResDesc.ResourceType == SHADER_RESOURCE_TYPE_TEXTURE_UAV);
                if (!ResourceCache.IsImageBound(ResAttr.CacheOffset + ArrInd, IsTexView))
                {
                    LOG_ERROR_MESSAGE("No resource is bound to variable '", GetShaderResourcePrintName(GLAttribs, ArrInd),
                                      "' in shader '", ShaderName, "' of PSO '", PSOName, "'");
                    BindingsOK = false;
                    continue;
                }

                const auto& Img = ResourceCache.GetConstImage(ResAttr.CacheOffset + ArrInd);
                if (Img.pTexture)
                    ValidateResourceViewDimension(ResDesc.Name, ResDesc.ArraySize, ArrInd, Img.pView.RawPtr<ITextureView>(), ResourceDim, IsMultisample);
                else
                    ValidateResourceViewDimension(ResDesc.Name, ResDesc.ArraySize, ArrInd, Img.pView.RawPtr<IBufferView>(), ResourceDim, IsMultisample);
            }
            break;

        default:
            UNEXPECTED("Unsupported shader resource range type.");
    }

    return BindingsOK;
}
#endif // DILIGENT_DEVELOPMENT

} // namespace Diligent
