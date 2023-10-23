// Copyright 2023 Autodesk, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "pch.h"

#include "PTScene.h"

#include "PTEnvironment.h"
#include "PTGeometry.h"
#include "PTGroundPlane.h"
#include "PTImage.h"
#include "PTLight.h"
#include "PTMaterial.h"
#include "PTRenderer.h"
#include "PTShaderLibrary.h"

#if ENABLE_MATERIALX
#include "MaterialX/MaterialGenerator.h"
#endif

#define AU_DEV_DUMP_MATERIALX_DOCUMENTS 0

BEGIN_AURORA

#define kBuiltInMissShaderCount 2 // "built-in" miss shaders: null and shadow

// Maximum number of textures per material from
#define kMaterialMaxTextures 7

// Fixed instance data stride (transform matrix and material buffer offset.)
#define kInstanceDataStride sizeof(InstanceDataHeader)

// Fix-sized header that is
// NOTE: Must be exactly 12 32-bit words long to match accessors in GlobalBufferAccessors.slang.
struct MaterialHeader
{
    int shaderIndex;
    int textureIndices[kMaterialMaxTextures];
};

// Fixed size header for instance data in global buffer.
// Followed by variable size array of layer information.
// NOTE: Must be exactly 14 32-bit words long to match accessors in GlobalBufferAccessors.slang.
struct InstanceDataHeader
{
    // Transposed row-major 4x3 transform matrix.
    float transform[3][4];

    // Offset within material buffer for this instance's material.
    int materialBufferOffset;

    // Number of material layers for this instance.
    // If non-zero, the information for each layer will be stored after this header in the buffer.
    int materialLayerCount;
};

PTLayerIndexTable::PTLayerIndexTable(PTRenderer* pRenderer, const vector<int>& indices) :
    _pRenderer(pRenderer)
{
    if (!indices.empty())
        set(indices);
}

void PTLayerIndexTable::set(const vector<int>& indices)
{
    // Create a constant buffer for the material data if it doesn't already exist.
    static const size_t BUFFER_SIZE = sizeof(int) * kMaxMaterialLayers;
    if (!_pConstantBuffer)
    {
        _pConstantBuffer = _pRenderer->createBuffer(BUFFER_SIZE);
    }

    _count = static_cast<int>(indices.size());

    // Copy the indices to the constant buffer.
    int* pMappedData = nullptr;
    checkHR(_pConstantBuffer->Map(0, nullptr, (void**)&pMappedData));
    for (int i = 0; i < kMaxMaterialLayers; i++)
    {
        if (i < indices.size())
            pMappedData[i] = indices[i];
        else
            pMappedData[i] = kInvalidOffset;
    }
    _pConstantBuffer->Unmap(0, nullptr); // no HRESULT
}

// A structure that contains the properties for a hit group shader record, laid out for direct
// copying to a shader table buffer.
// Must match the GPU layout defined by InstancePipelineState.slang
struct HitGroupShaderRecord
{
    // Constructor.
    HitGroupShaderRecord(const void* pShaderIdentifier, const PTGeometry::GeometryBuffers& geometry,
        int instanceBufferOffset, bool isOpaque)
    {
        ::memcpy_s(&ShaderIdentifier, SHADER_ID_SIZE, pShaderIdentifier, SHADER_ID_SIZE);
        IndexBufferAddress    = geometry.IndexBuffer;
        PositionBufferAddress = geometry.PositionBuffer;
        HasNormals            = geometry.NormalBuffer ? 1 : 0;
        NormalBufferAddress   = HasNormals ? geometry.NormalBuffer : 0;
        HasTangents           = geometry.TangentBuffer != 0;
        TangentBufferAddress  = HasTangents ? geometry.TangentBuffer : 0;
        HasTexCoords          = geometry.TexCoordBuffer != 0;
        TexCoordBufferAddress = HasTexCoords ? geometry.TexCoordBuffer : 0;
        IsOpaque              = isOpaque;

        InstanceBufferOffset = instanceBufferOffset;
    }

    // Copies the contents of the shader record to the specified mapped buffer.
    void copyTo(void* mappedBuffer)
    {
        ::memcpy_s(mappedBuffer, sizeof(HitGroupShaderRecord), this, sizeof(HitGroupShaderRecord));
    }

    // Get the stride (aligned size) of a shader record.
    static size_t stride()
    {
        return ALIGNED_SIZE(sizeof(HitGroupShaderRecord), SHADER_RECORD_ALIGNMENT);
    }

    // These are the hit group arguments, in the order declared in the associated local root
    // signature and the layout in InstancePipelineState.slang. These must be aligned by their size,
    // e.g. a root descriptor must be aligned on an 8-byte (its size) boundary.

    // Shader identifier which is part of the hit group record, not the user defined root signature.
    array<uint8_t, SHADER_ID_SIZE> ShaderIdentifier;

    // Geometry data defined in ByteAddressBuffers: registers t0-t4, space 1.
    D3D12_GPU_VIRTUAL_ADDRESS IndexBufferAddress;
    D3D12_GPU_VIRTUAL_ADDRESS PositionBufferAddress;
    D3D12_GPU_VIRTUAL_ADDRESS NormalBufferAddress;
    D3D12_GPU_VIRTUAL_ADDRESS TangentBufferAddress;
    D3D12_GPU_VIRTUAL_ADDRESS TexCoordBufferAddress;
    // Instance data defined in cbuffer gInstanceData: register c0, space 1.
    uint32_t HasNormals;
    uint32_t HasTangents;
    uint32_t HasTexCoords;
    uint32_t IsOpaque;
    uint32_t InstanceBufferOffset;
};

PTInstance::PTInstance(PTScene* pScene, const PTGeometryPtr& pGeometry,
    const PTMaterialPtr& pMaterial, const mat4& transform, const LayerDefinitions& layers)
{
    AU_ASSERT(pGeometry, "Geometry assigned to an cannot be null.");

    // Set the arguments directly; they are assumed to be prepared by the caller.
    _pScene    = pScene;
    _pGeometry = pGeometry;
    _transform = transform;
    setMaterial(pMaterial);
    for (size_t i = 0; i < layers.size(); i++)
    {
        _layers.push_back(make_pair(dynamic_pointer_cast<PTMaterial>(layers[i].first),
            dynamic_pointer_cast<PTGeometry>(layers[i].second)));
    }
}

PTInstance::~PTInstance()
{
    if (_pMaterial)
    {
        _pMaterial->shader()->decrementRefCount(EntryPointTypes::kInitializeMaterialExport);
    }
#if 0
    for (size_t i = 0; i < _layers.size(); i++)
    {
        _layers[i].first->shader()->decrementRefCount(EntryPointTypes::kLayerMiss);
    }
#endif
}

void PTInstance::setMaterial(const IMaterialPtr& pMaterial)
{
    if (_pMaterial)
        _pMaterial->shader()->decrementRefCount(EntryPointTypes::kInitializeMaterialExport);

    // Cast the (optional) material to the renderer implementation. Use the default material if one
    // is / not specified.
    _pMaterial = pMaterial
        ? dynamic_pointer_cast<PTMaterial>(pMaterial)
        : dynamic_pointer_cast<PTMaterial>(_pScene->defaultMaterialResource()->resource());

    _pMaterial->shader()->incrementRefCount(EntryPointTypes::kInitializeMaterialExport);

    // Set the instance as dirty.
    _bIsDirty = true;
}

void PTInstance::setTransform(const mat4& transform)
{
    // Create a GLM matrix from the (column-major) array if one is specified. Otherwise the
    // default / (identity) matrix is used.
    _transform = transform;

    // Set the instance as dirty.
    _bIsDirty = true;
}

void PTInstance::setObjectIdentifier(int /*objectId*/)
{
    // TODO: implement object id setting for Aurora
}

bool PTInstance::update()
{
    // Update the geometry and material.
    // NOTE: Whether these were dirty (i.e. return false) do not affect whether the instance was
    // considered dirty.
    _pMaterial->update();

    // Update the material and geometry for layer materials.
    for (size_t i = 0; i < _layers.size(); i++)
    {
        _layers[i].first->update();
        if (_layers[i].second)
            _layers[i].second->update();
    }

    // Clear the dirty flag.
    bool wasDirty = _bIsDirty;
    _bIsDirty     = false;

    return wasDirty;
}

PTScene::PTScene(PTRenderer* pRenderer, uint32_t numRendererDescriptors) : SceneBase(pRenderer)
{
    _pRenderer = pRenderer;

    // Initialize the shader library.
    // TODO: Should be per-scene not per-renderer.
    _pShaderLibrary = make_unique<PTShaderLibrary>(_pRenderer->dxDevice());

    _numRendererDescriptors = numRendererDescriptors;

    // Compute the shader record strides.
    _missShaderRecordStride     = HitGroupShaderRecord::stride(); // shader ID, no other parameters
    _missShaderRecordCount      = kBuiltInMissShaderCount;        // null, and shadow
    _hitGroupShaderRecordStride = HitGroupShaderRecord::stride();

    // Use the default environment and ground plane.
    _pGroundPlane = pRenderer->defaultGroundPlane();

    // Create arbritary sized material buffer (will be resized to fit material constants for scene.)
    _globalMaterialBuffer = _pRenderer->createTransferBuffer(512, "GlobalMaterialBuffer");

    // Create arbritary sized instance buffer (will be resized to fit instance constants for scene.)
    _globalInstanceBuffer = _pRenderer->createTransferBuffer(512, "GlobalInstanceBuffer");

    // Dusable layer shaders.
    // TODO: Reimplement layer shaders with non-recursive rendering.
    _pShaderLibrary->setOption("ENABLE_LAYERS", false);

#if ENABLE_MATERIALX
    // Get the materialX folder relative to the module path.
    string mtlxFolder = Foundation::getModulePath() + "MaterialX";
    // Initialize the MaterialX code generator.
    _pMaterialXGenerator = make_unique<MaterialXCodeGen::MaterialGenerator>(mtlxFolder);

    // Default to MaterialX distance unit to centimeters.
    _pShaderLibrary->setOption(
        "DISTANCE_UNIT", _pMaterialXGenerator->codeGenerator().units().indices.at("centimeter"));
#endif
}
void PTScene::setUnit(const string& unit)
{
#if ENABLE_MATERIALX
    // Get the units option.
    // Lookup the unit in the code generator, and ensure it is valid.
    auto unitIter = _pMaterialXGenerator->codeGenerator().units().indices.find(unit);
    if (unitIter == _pMaterialXGenerator->codeGenerator().units().indices.end())
    {
        AU_ERROR("Invalid unit:" + unit);
    }
    else
    {
        // Set the option in the shader library.
        shaderLibrary().setOption("DISTANCE_UNIT", unitIter->second);
    }
#endif
}

IMaterialPtr PTScene::createMaterialPointer(
    const string& materialType, const string& document, const string& name)
{
    // Validate material type.
    AU_ASSERT(materialType.compare(Names::MaterialTypes::kBuiltIn) == 0 ||
            materialType.compare(Names::MaterialTypes::kMaterialX) == 0 ||
            materialType.compare(Names::MaterialTypes::kMaterialXPath) == 0,
        "Invalid material type:", materialType.c_str());

    // Set the global "flipY" flag on the asset manager, to match option.
    // This has no overhead, so just do it each time.
    _pRenderer->assetManager()->enableVerticalFlipOnImageLoad(
        _pRenderer->asBoolean(kLabelIsFlipImageYEnabled));

    // The material shader and definition for this material.
    MaterialShaderPtr pShader;
    shared_ptr<MaterialDefinition> pDef;

    // Create a material type based on the material type name provided.
    if (materialType.compare(Names::MaterialTypes::kBuiltIn) == 0)
    {
        // Work out built-in type.
        string builtInType = document;

        // Get the built-in material type and definition for built-in.
        pShader = shaderLibrary().getBuiltInShader(builtInType);
        pDef    = shaderLibrary().getBuiltInMaterialDefinition(builtInType);

        // Print error and provide null material shader if built-in not found.
        // TODO: Proper error handling for this case.
        if (!pShader)
        {
            AU_ERROR("Unknown built-in material type %s for material %s", document.c_str(),
                name.c_str());
            pShader = nullptr;
        }
    }
    else if (materialType.compare(Names::MaterialTypes::kMaterialX) == 0)
    {
        // Generate a material shader and definition from the materialX document.
        pShader = generateMaterialX(document, &pDef);

        // If flag is set dump the document to disk for development purposes.
        if (AU_DEV_DUMP_MATERIALX_DOCUMENTS)
        {
            string mltxPath = name + "Dumped.mtlx";
            Foundation::sanitizeFileName(mltxPath);
            if (Foundation::writeStringToFile(document, mltxPath))
                AU_INFO("Dumping MTLX document to:%s", mltxPath.c_str());
            else
                AU_WARN("Failed to dump MTLX document to:%s", mltxPath.c_str());
        }
    }
    else if (materialType.compare(Names::MaterialTypes::kMaterialXPath) == 0)
    {
        // Load the MaterialX file using asset manager.
        auto pMtlxDocument = _pRenderer->assetManager()->acquireTextFile(document);

        // Print error and provide default material type if built-in not found.
        // TODO: Proper error handling for this case.
        if (!pMtlxDocument)
        {
            AU_ERROR("Failed to load MaterialX document %s for material %s", document.c_str(),
                name.c_str());
            pShader = nullptr;
        }
        else
        {
            // If Material XML document loaded, use it to generate the material shader and
            // definition.
            pShader = generateMaterialX(*pMtlxDocument, &pDef);
        }
    }
    else
    {
        // Print error and return null material shader if material type not found.
        // TODO: Proper error handling for this case.
        AU_ERROR(
            "Unrecognized material type %s for material %s.", materialType.c_str(), name.c_str());
        pShader = nullptr;
    }

    // Error case, just return null material.
    if (!pShader || !pDef)
        return nullptr;

    // Create the material object with the material shader and definition.
    auto pNewMtl = make_shared<PTMaterial>(_pRenderer, name, pShader, pDef);

    // Set the default textures on the new material.
    for (int i = 0; i < pDef->defaults().textures.size(); i++)
    {
        auto txtDef = pDef->defaults().textures[i];

        // Image default values are provided as strings and must be loaded.
        auto textureFilename = txtDef.defaultFilename;
        if (!textureFilename.empty())
        {
            // Load the pixels for the image using asset manager.
            auto pImageData = _pRenderer->assetManager()->acquireImage(textureFilename);
            if (!pImageData)
            {
                // Print error if image fails to load, and then ignore default.
                // TODO: Proper error handling here.
                AU_ERROR("Failed to load image data %s for material %s", textureFilename.c_str(),
                    name.c_str());
            }
            else
            {
                // Set the linearize flag.
                // TODO: Should effect caching.
                pImageData->data.linearize = txtDef.linearize;

                // Create image from the loaded pixels.
                // TODO: This should be cached by filename.
                auto pImage = _pRenderer->createImagePointer(pImageData->data);
                if (!pImage)
                {
                    // Print error if image creation fails, and then ignore default.
                    // TODO: Proper error handling here.
                    AU_ERROR("Failed to create image %s for material %s", textureFilename.c_str(),
                        name.c_str());
                }
                else
                {
                    // Set the default image.
                    pNewMtl->setImage(txtDef.name, pImage);
                }
            }
        }

        // If we have an address mode, create a sampler for texture.
        // Currently only the first two hardcoded textures have samplers, so only do this for first
        // two textures.
        // TODO: Move to fully data driven textures and samplers.
        if (i < 2 && (!txtDef.addressModeU.empty() || !txtDef.addressModeV.empty()))
        {
            Properties samplerProps;

            // Set U address mode.
            if (txtDef.addressModeU.compare("periodic") == 0)
                samplerProps[Names::SamplerProperties::kAddressModeU] = Names::AddressModes::kWrap;
            else if (txtDef.addressModeU.compare("clamp") == 0)
                samplerProps[Names::SamplerProperties::kAddressModeU] = Names::AddressModes::kClamp;
            else if (txtDef.addressModeU.compare("mirror") == 0)
                samplerProps[Names::SamplerProperties::kAddressModeU] =
                    Names::AddressModes::kMirror;

            // Set V address mode.
            if (txtDef.addressModeV.compare("periodic") == 0)
                samplerProps[Names::SamplerProperties::kAddressModeV] = Names::AddressModes::kWrap;
            else if (txtDef.addressModeV.compare("clamp") == 0)
                samplerProps[Names::SamplerProperties::kAddressModeV] = Names::AddressModes::kClamp;
            else if (txtDef.addressModeV.compare("mirror") == 0)
                samplerProps[Names::SamplerProperties::kAddressModeV] =
                    Names::AddressModes::kMirror;

            // Create a sampler and set in the material.
            // TODO: Don't assume hardcoded _sampler prefix.
            auto pSampler = _pRenderer->createSamplerPointer(samplerProps);
            pNewMtl->setSampler(txtDef.name + "_sampler", pSampler);
        }
    }

    // Return new material.
    return pNewMtl;
}

shared_ptr<MaterialShader> PTScene::generateMaterialX([[maybe_unused]] const string& document,
    [[maybe_unused]] shared_ptr<MaterialDefinition>* pDefOut)
{
#if ENABLE_MATERIALX
    // Generate the material definition for the materialX document, this contains the source code,
    // default values, and a unique name.
    shared_ptr<MaterialDefinition> pDef = _pMaterialXGenerator->generate(document);
    if (!pDef)
    {
        return nullptr;
    }

    // Acquire a material shader for the definition.
    // This will create a new one if needed (and trigger a rebuild), otherwise it will return an
    // existing one.
    auto pShader = shaderLibrary().acquireShader(*pDef);

    // Output the definition pointer.
    if (pDefOut)
        *pDefOut = pDef;

    return pShader;
#else
    return nullptr;
#endif
}

ILightPtr PTScene::addLightPointer(const string& lightType)
{
    // Only distant lights are currently supported.
    AU_ASSERT(lightType.compare(Names::LightTypes::kDistantLight) == 0,
        "Only distant lights currently supported");

    // The remaining operations are not yet thread safe.
    std::lock_guard<std::mutex> lock(_mutex);

    // Assign arbritary index to ensure deterministic ordering.
    int index = _currentLightIndex++;

    // Create the light object.
    PTLightPtr pLight = make_shared<PTLight>(this, lightType, index);

    // Add weak pointer to distant light map.
    _distantLights[index] = pLight;

    // Return the new light.
    return pLight;
}

IInstancePtr PTScene::addInstancePointer(const Path& /* path*/, const IGeometryPtr& pGeom,
    const IMaterialPtr& pMaterial, const mat4& transform, const LayerDefinitions& materialLayers)
{
    // Cast the (optional) material to the device implementation. Use the default material if one is
    // not specified.
    PTMaterialPtr pPTMaterial = pMaterial
        ? dynamic_pointer_cast<PTMaterial>(pMaterial)
        : dynamic_pointer_cast<PTMaterial>(_pDefaultMaterialResource->resource());

    // The remaining operations are not yet thread safe.
    std::lock_guard<std::mutex> lock(_mutex);

    // Create the instance object and add it to the list of instances for the scene.
    PTInstancePtr pPTInstance = make_shared<PTInstance>(
        this, dynamic_pointer_cast<PTGeometry>(pGeom), pPTMaterial, transform, materialLayers);

    return pPTInstance;
}

void PTScene::setGroundPlanePointer(const IGroundPlanePtr& pGroundPlane)
{
    // Use the renderer's default ground plane if a ground plane is not specified.
    // NOTE: The default ground plane is disabled ("enabled" == false), so that setting a null
    // pointer on this function will disable the ground plane.
    _pGroundPlane = pGroundPlane ? dynamic_pointer_cast<PTGroundPlane>(pGroundPlane)
                                 : _pRenderer->defaultGroundPlane();
}

ID3D12Resource* PTScene::getMissShaderTable(size_t& recordStride, uint32_t& recordCount) const
{
    // Set the shader record size and count.
    recordStride = _missShaderRecordStride;
    recordCount  = _missShaderRecordCount;

    return _pMissShaderTable.Get();
}

ID3D12Resource* PTScene::getHitGroupShaderTable(size_t& recordStride, uint32_t& recordCount) const
{
    // Set the shader record size and count.
    recordStride = HitGroupShaderRecord::stride();
    recordCount  = static_cast<uint32_t>(_instances.active().count());

    return _pHitGroupShaderTable.Get();
}

// Sort function, used to sort lights to ensure deterministic ordering.
bool compareLights(PTLight* first, PTLight* second)
{
    return (first->index() < second->index());
}

void PTScene::update()
{
    // Update base class.
    SceneBase::update();
}

int PTScene::computeMaterialTextureCount()
{
    // Clear material texture vector and lookup map.
    _activeMaterialTextures.clear();
    _materialTextureIndexLookup.clear();

    // Create a SRV (descriptor) on the descriptor heap for the default texture (this ensures heap
    // never empty.)
    PTImage& defaultImage                      = _images.active().resources<PTImage>().front();
    _materialTextureIndexLookup[&defaultImage] = int(_activeMaterialTextures.size());
    _activeMaterialTextures.push_back(&defaultImage);

    // Iterate through all active textures.
    for (PTMaterial& mtl : _materials.active().resources<PTMaterial>())
    {
        // Get the textures used by this material.
        vector<PTImage*> textures;
        mtl.getTextures(textures);

        // Add the textures to material texture vector and lookup map.
        for (int i = 0; i < textures.size(); i++)
        {
            if (textures[i] &&
                _materialTextureIndexLookup.find(textures[i]) == _materialTextureIndexLookup.end())
            {
                _materialTextureIndexLookup[textures[i]] = int(_activeMaterialTextures.size());
                _activeMaterialTextures.push_back(textures[i]);
            }
        }
    }

    // Return count.
    return int(_activeMaterialTextures.size());
}

void PTScene::updateResources()
{
    // Delete the transfer buffers that were uploaded last frame.
    _pRenderer->deleteUploadedTransferBuffers();

    // Update the environment.
    // This is called if *any* active environment objects have changed, as that will almost always
    // be the only active one.
    if (_environments.changedThisFrame())
    {
        _pEnvironment = static_pointer_cast<PTEnvironment>(_pEnvironmentResource->resource());
        _pEnvironment->update();
        _isEnvironmentDescriptorsDirty = true;
    }

    // Update the ground plane.
    _pGroundPlane->update();

    // See if any distant lights have been changed this frame, and build vector of active lights.
    bool distantLightsUpdated = false;
    vector<PTLight*> currLights;
    for (auto iter = _distantLights.begin(); iter != _distantLights.end(); iter++)
    {
        // Get the light and ensure weak pointer still valid.
        PTLightPtr pLight = iter->second.lock();
        if (pLight)
        {
            // Add to currently active light vector.
            currLights.push_back(pLight.get());

            // If the dirty flag is set, GPU data must be updated.
            if (pLight->isDirty())
            {
                distantLightsUpdated = true;
                pLight->clearDirtyFlag();
            }
        }
        else
        {
            // If the weak pointer is not valid remove it from the map, and ensure GPU data is
            // updated (as a light has been removed.)
            _distantLights.erase(iter->first);
            distantLightsUpdated = true;
        }
    }

    // If distant lights have changed update the LightData struct that is passed to the GPU.
    if (distantLightsUpdated)
    {
        // Sort the lights by index, to ensure deterministic ordering.
        sort(currLights.begin(), currLights.end(), compareLights);

        // Set the distant light count to minimum of current light vector size and the max distant
        // light limit.  Lights in the sorted array past LightLimits::kMaxDistantLights are ignored.
        _lights.distantLightCount =
            std::min(int(currLights.size()), int(LightLimits::kMaxDistantLights));

        // Add to the light data buffer that is copied to the frame data for this frame.
        for (int i = 0; i < _lights.distantLightCount; i++)
        {
            // Store the cosine of the radius for use in the shader.
            _lights.distantLights[i].cosRadius =
                cos(0.5f * currLights[i]->asFloat(Names::LightProperties::kAngularDiameter));

            // Invert the direction for use in the shader.
            _lights.distantLights[i].direction =
                -currLights[i]->asFloat3(Names::LightProperties::kDirection);

            // Store color in RGB and intensity in alpha.
            _lights.distantLights[i].colorAndIntensity =
                vec4(currLights[i]->asFloat3(Names::LightProperties::kColor),
                    currLights[i]->asFloat(Names::LightProperties::kIntensity));
        }
    }

    // If any active geometry resources have been modified, flush the vertex buffer pool in case
    // there are any pending vertex buffers that are required to update the geometry, and then
    // update the geometry.
    if (_geometry.changedThisFrame())
    {
        // Iterate through the modified geometry (which will include the newly active ones.)
        for (PTGeometry& geom : _geometry.modified().resources<PTGeometry>())
        {
            geom.update();
        }

        // Flush the vertex buffer pool.
        _pRenderer->flushVertexBufferPool();
    }

    // If any active material resources have been modified update them and build a list of unique
    // samplers for all the active materials.
    if (_materials.changedThisFrame() || _images.changedThisFrame())
    {
        // Clear the material offset map.
        _materialOffsetLookup.clear();

        // Rebuild material texture lookup map.
        computeMaterialTextureCount();

        // Starting at beginning of buffer, work out where each material is global byte address
        // buffer.
        size_t globalMaterialBufferSize = 0;

        // Iterate through the all the active materials, even ones that have not changed.
        for (PTMaterial& mtl : _materials.active().resources<PTMaterial>())
        {
            // Update the material.
            mtl.update();

            // Add the offset for this material to the lookup map.
            _materialOffsetLookup[&mtl] = int(globalMaterialBufferSize);

            // Increment by size of fixed-length header.
            globalMaterialBufferSize += sizeof(MaterialHeader);

            // Increment by size of this material's properties.
            globalMaterialBufferSize += mtl.uniformBuffer().size();
        }

        // If the global material buffer too small recreate it.
        if (globalMaterialBufferSize > _globalMaterialBuffer.size)
        {
            _globalMaterialBuffer =
                _pRenderer->createTransferBuffer(globalMaterialBufferSize, "GlobalMaterialBuffer");
        }

        // Map the global material buffer.
        uint8_t* pMtlDataStart = _globalMaterialBuffer.map();
        uint8_t* pMtlData      = pMtlDataStart;
        size_t sizeLeft        = _globalMaterialBuffer.size;
        for (PTMaterial& mtl : _materials.active().resources<PTMaterial>())
        {
            // Ensure offset matches the lookup value.
            int offset = _materialOffsetLookup[&mtl];
            AU_ASSERT(pMtlDataStart + offset == pMtlData, "Offset mismatch");

            // Get pointer to material header and add shader index.
            MaterialHeader* pHdr = (MaterialHeader*)pMtlData;
            pHdr->shaderIndex    = mtl.shader()->libraryIndex();

            // Add the material texture indices (fill in unused values as invalid)
            // TODO: No need for this to be fixed length.
            vector<PTImage*> textures;
            mtl.getTextures(textures);
            for (int j = 0; j < textures.size() && j < kMaterialMaxTextures; j++)
            {
                if (!textures[j])
                {
                    pHdr->textureIndices[j] = kInvalidOffset;
                }
                else
                {
                    pHdr->textureIndices[j] = _materialTextureIndexLookup[textures[j]];
                }
            }
            for (int j = int(textures.size()); j < kMaterialMaxTextures; j++)
            {
                pHdr->textureIndices[j] = kInvalidOffset;
            }

            // Move pointer past header.
            size_t headerSize = sizeof(MaterialHeader);
            sizeLeft -= headerSize;
            pMtlData += headerSize;

            // Write the properties from material's uniform buffer.
            auto& uniformBuffer = mtl.uniformBuffer();
            size_t bufferSize   = uniformBuffer.size();
            float* pSrcData     = (float*)uniformBuffer.data();
            ::memcpy_s(pMtlData, sizeLeft, pSrcData, bufferSize);

            // Move pointer to next material.
            sizeLeft -= bufferSize;
            pMtlData += bufferSize;
        }
        _globalMaterialBuffer.unmap();

        // Wait for previous render tasks and then clear the descriptor heap.
        // TODO: Only do this if any texture parameters have changed.
        _pRenderer->waitForTask();
        clearDesciptorHeap();
    }

    // Upload any transfer buffers that have been updated this frame.
    _pRenderer->uploadTransferBuffers();

    // Update the geometry BLAS (after any transfer buffers have been uploaded.)
    if (_geometry.changedThisFrame())
    {
        for (PTGeometry& geom : _geometry.modified().resources<PTGeometry>())
        {
            if (!geom.isIncomplete())
                geom.updateBLAS();
        }
    }

    // If any active instances have been modified or activated, update all the active instances.
    if (_instances.changedThisFrame())
    {
        for (PTInstance& instance : _instances.active().resources<PTInstance>())
        {
            instance.update();
        }
    }

    // Update the acceleration structure if any geometry or instances have been modified.
    if (_instances.changedThisFrame() || _geometry.changedThisFrame())
    {

        // Ensure the acceleration structure is no longer being accessed.
        // TODO: Is there a less drastic stall we can do here?
        _pRenderer->waitForTask();

        // Release the acceleration structure.
        _pAccelStructure.Reset();
    }

    // Update the scene resources: the acceleration structure, the descriptor heap, and the shader
    // tables.  Will only do anything if the relevant pointers have been cleared.
    updateAccelerationStructure();
    updateDescriptorHeap();
    updateShaderTables();
}

void PTScene::clearDesciptorHeap()
{
    _pDescriptorHeap.Reset();
}

void PTScene::clearShaderData()
{
    // Delete the hit group and miss shader table.
    _pHitGroupShaderTable.Reset();
    _pMissShaderTable.Reset();
}

void PTScene::updateAccelerationStructure()
{
    // Do nothing if the acceleration structure already exists.
    if (_pAccelStructure)
    {
        return;
    }

    // Build a list of instance data in the global instance buffer.  As the buffer contains a copy
    // of the transform matrix, there must be an entry (and a hit group) for every instance, even if
    // they share all the same data other than transform matrix.
    // TODO: If it was possible access the TLAS instance matrix from the ray generation shader we
    // could remove this copy of the transform data, and share hit groups between instances.
    _instanceOffsetLookup.clear();

    // Iterate through all the active instances in the scene to compute offset within instance
    // buffer.
    int instanceBufferOffset = 0;
    for (PTInstance& instance : _instances.active().resources<PTInstance>())
    {
        // Set the offset in the lookup table.
        _instanceOffsetLookup[&instance] = instanceBufferOffset;

        // Move offset past header.
        instanceBufferOffset += kInstanceDataStride;

        // Move offset past layer information.
        instanceBufferOffset += int(instance.materialLayers().size() * sizeof(int) * 2);
    }

    // Set the required instance buffer size.
    _instanceBufferSize = instanceBufferOffset;

    // Build the top-level acceleration structure (TLAS).
    _pAccelStructure = buildTLAS();

    // If the acceleration structure was rebuilt, then the descriptor heap, as well as the miss and
    // hit group shader tables must likewise be rebuilt, as they rely on the instance data.
    _pDescriptorHeap.Reset();
    _pHitGroupShaderTable.Reset();
    _pMissShaderTable.Reset();
}

void PTScene::updateDescriptorHeap()
{
    // Create the descriptor heap if needed.
    if (!_pDescriptorHeap)
    {
        // Get the device.
        ID3D12Device5Ptr pDevice = _pRenderer->dxDevice();

        // Create a descriptor heap for CBV/SRV/UAVs needed by shader records. Currently this means
        // the descriptors for the instance materials, plus the number of descriptors needed by the
        // renderer and environment.
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = _numRendererDescriptors + int(_activeMaterialTextures.size()) +
            _pEnvironment->descriptorCount();
        heapDesc.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        checkHR(pDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&_pDescriptorHeap)));

        // Create a descriptor heap for samplers needed by shader records.
        // TODO: Only the default sampler currentl support, should add full sampler support to
        // non-recursive renderer.
        D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
        samplerHeapDesc.NumDescriptors             = 1;
        samplerHeapDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        samplerHeapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        checkHR(pDevice->CreateDescriptorHeap(
            &samplerHeapDesc, IID_PPV_ARGS(&_pSamplerDescriptorHeap)));

        // If the descriptor heap was rebuilt, then the descriptors must likewise be recreated.
        _isEnvironmentDescriptorsDirty = true;
        _isHitGroupDescriptorsDirty    = true;
    }

    // If nothing is dirty early out.
    if (!_isEnvironmentDescriptorsDirty && !_materials.changedThisFrame() &&
        !_isHitGroupDescriptorsDirty)
        return;

    // Get a CPU handle to the start of the sampler descriptor heap, offset by the number of
    // descriptors reserved for the renderer.
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(_pDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    UINT handleIncrement = _pRenderer->dxDevice()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    handle.Offset(_numRendererDescriptors, handleIncrement);

    // Create the descriptors for the environment textures.
    // NOTE: This will also increment the handle past the new descriptors.
    _pEnvironment->createDescriptors(handle, handleIncrement);

    // Create the descriptors for the material textures.
    for (int i = 0; i < _activeMaterialTextures.size(); i++)
    {
        // Create a SRV (descriptor) on the descriptor heap for the texture.
        PTImage::createSRV(*_pRenderer, _activeMaterialTextures[i], handle);
        handle.Offset(handleIncrement);
    }

    // Get a CPU handle to the start of the sampler descriptor heap.
    CD3DX12_CPU_DESCRIPTOR_HANDLE samplerHandle(
        _pSamplerDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
    UINT samplerHandleIncrement = _pRenderer->dxDevice()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    // Add descriptor for default sampler.
    PTSampler::createDescriptor(*_pRenderer, _pRenderer->defaultSampler().get(), samplerHandle);
    handle.Offset(samplerHandleIncrement);

    // Clear the dirty flags.
    _isEnvironmentDescriptorsDirty = false;
    _isHitGroupDescriptorsDirty    = false;
}

void PTScene::updateShaderTables()
{

    // Create and populate the hit  shader table if it doesn't exist and there are instances.
    auto instanceCount = static_cast<uint32_t>(_instances.active().count());

    // Texture and sampler descriptors for unique materials in the scene.
    vector<CD3DX12_GPU_DESCRIPTOR_HANDLE> lstUniqueMaterialSamplerDescriptors;

    if (!_pHitGroupShaderTable && instanceCount > 0)
    {
        // Resize the global instance buffer if too small for all the instances.
        if (_globalInstanceBuffer.size < _instanceBufferSize)
        {
            _globalInstanceBuffer =
                _pRenderer->createTransferBuffer(_instanceBufferSize, "GlobalInstanceBuffer");
        }

        // Fill in the global instance data buffer.
        uint8_t* pInstanceDataStart = _globalInstanceBuffer.map();
        int offset                  = 0;
        for (PTInstance& instance : _instances.active().resources<PTInstance>())
        {
            // Validate offset witin buffer matches offset in instance lookup table.
            AU_ASSERT(offset == _instanceOffsetLookup[&instance], "Offset incorrect");

            // Get pointer to instance data in buffer.
            uint8_t* pInstanceData    = pInstanceDataStart + offset;
            InstanceDataHeader* pData = (InstanceDataHeader*)pInstanceData;

            // Copy transposed transform matrix into buffer.
            mat4 matrix = transpose(instance.transform());
            ::memcpy_s(
                pData->transform, sizeof(pData->transform), &matrix, sizeof(pData->transform));

            // Set material buffer offset from value in material lookup table.
            pData->materialBufferOffset = _materialOffsetLookup[instance.material().get()];

            // Set layer count.
            pData->materialLayerCount = int(instance.materialLayers().size());

            // Move offset past header.
            offset += kInstanceDataStride;

            // Fill in layer information.
            int* pLayerData = reinterpret_cast<int*>(pInstanceDataStart + offset);
            for (int j = 0; j < pData->materialLayerCount; j++)
            {
                auto& pLayerMtl = instance.materialLayers()[j].first;

                // Copy layer material offset to buffer.
                *pLayerData = _materialOffsetLookup[pLayerMtl.get()];
                pLayerData++;

                // Copy UV offset to buffer.
                // TODO: Implement this.
                *pLayerData = kInvalidOffset;
                pLayerData++;

                // Move offset past layer information.
                offset += sizeof(int) * 2;
            }
        }

        // Unmap buffer.
        _globalInstanceBuffer.unmap();

        size_t recordStride = HitGroupShaderRecord::stride();

        // Create a transfer buffer for the shader table, and map it for writing.
        size_t shaderTableSize = recordStride * instanceCount;
        TransferBuffer hitGroupTransferBuffer =
            _pRenderer->createTransferBuffer(shaderTableSize, "HitGroupShaderTable");
        uint8_t* pShaderTableMappedData = hitGroupTransferBuffer.map();

        // Retain the GPU buffer from the transfer buffer, the upload buffer will be deleted by the
        // renderer once upload complete.
        _pHitGroupShaderTable = hitGroupTransferBuffer.pGPUBuffer;

        // Iterate the instances, creating a hit group shader record for each one, and
        // copying the shader record data to the shader table.
        for (PTInstance& instance : _instances.active().resources<PTInstance>())
        {
            // Get the hit group shader ID from the material shader, which will change if the shader
            // library is rebuilt.
            const DirectXShaderIdentifier hitGroupShaderID =
                _pShaderLibrary->getShaderID(PTShaderLibrary::kInstanceHitGroupName);

            // Shader record data includes the geometry buffers, the instance constant buffer
            // offset, and opaque flag.
            PTGeometry::GeometryBuffers geometryBuffers = instance.dxGeometry()->buffers();
            int instanceBufferOffset                    = _instanceOffsetLookup[&instance];
            HitGroupShaderRecord record(hitGroupShaderID, geometryBuffers, instanceBufferOffset,
                instance.material()->isOpaque());
            record.copyTo(pShaderTableMappedData);
            pShaderTableMappedData += recordStride;
        }

        // Close the shader table buffer.
        hitGroupTransferBuffer.unmap();
    }

    // Create and populate the miss shader table if necessary.
    if (!_pMissShaderTable)
    {
        // Calculate miss shader record count (built-ins plus all the layer material miss shaders)
        _missShaderRecordCount = kBuiltInMissShaderCount;

        // Create a buffer for the shader table and write the shader identifiers for the miss
        // shaders.
        // NOTE: There are no arguments (and indeed no local root signatures) for these shaders.
        size_t shaderTableSize = _missShaderRecordStride * _missShaderRecordCount;

        // Create the transfer buffer.
        TransferBuffer missShaderTableTransferBuffer =
            _pRenderer->createTransferBuffer(shaderTableSize, "MissShaderTable");
        _pMissShaderTable = missShaderTableTransferBuffer.pGPUBuffer;

        // Map the shader table buffer.
        uint8_t* pShaderTableMappedData      = missShaderTableTransferBuffer.map();
        uint8_t* pEndOfShaderTableMappedData = pShaderTableMappedData + shaderTableSize;

        // Get the shader identifiers from the shader library, which will change if the library is
        // rebuilt.
        // NOTE: The first miss shader is a null shader, used when a miss shader is not needed.
        static array<uint8_t, SHADER_ID_SIZE> kNullShaderID = { 0 };
        ::memcpy_s(pShaderTableMappedData, SHADER_ID_SIZE, kNullShaderID.data(), SHADER_ID_SIZE);
        pShaderTableMappedData += _missShaderRecordStride;
        ::memcpy_s(pShaderTableMappedData, SHADER_ID_SIZE,
            _pShaderLibrary->getShaderID(PTShaderLibrary::kShadowMissEntryPointName),
            SHADER_ID_SIZE);
        pShaderTableMappedData += _missShaderRecordStride;

        // Ensure we didn't over run the shader table buffer.
        AU_ASSERT(pShaderTableMappedData == pEndOfShaderTableMappedData, "Shader table overrun");

        // Unmap the table.
        missShaderTableTransferBuffer.unmap(); // no HRESULT

        // Upload any transfer buffers that have changed to GPU, so they can be accessed by GPU
        // commands.
        _pRenderer->uploadTransferBuffers();
    }
}

ID3D12ResourcePtr PTScene::buildTLAS()
{
    // Create and populate a buffer with instance data, if there are any instances.
    auto instanceCount = static_cast<uint32_t>(_instances.active().count());
    ID3D12ResourcePtr pInstanceBuffer;
    if (instanceCount > 0)
    {
        // Create a buffer for the instance data.
        size_t bufferSize            = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instanceCount;
        pInstanceBuffer              = _pRenderer->createBuffer(bufferSize);
        uint8_t* pInstanceMappedData = nullptr;
        checkHR(pInstanceBuffer->Map(0, nullptr, reinterpret_cast<void**>(&pInstanceMappedData)));

        // Describe a set of instances with varying geometries and transforms.
        uint32_t instanceIndex = 0;
        for (PTInstance& instance : _instances.active().resources<PTInstance>())
        {
            // Get the bottom-level acceleration structure (BLAS) from the instance geometry.
            ID3D12ResourcePtr pBLAS = instance.dxGeometry()->blas();

            // Get the transpose of the transform matrix of the instance. GLM has column-major
            // matrices, but DXR expects (4x3) row-major matrices for instance descriptions.
            mat4 matrix = transpose(instance.transform());

            // Describe the instance. Specifically this includes:
            // - A pointer to the BLAS.
            // - The transform for the instance.
            // - An identifier for the hit group data to use when the instance is hit by a ray.
            // NOTE: The instance is not set as opaque here on the instance flags, so that the any
            // hit shader can be called if needed. If the any hit shader is not needed, the shader
            // will use the opaque ray flag when calling TraceRay().
            // NOTE: Using memcpy() for the transform copy writes too much data (all 16 floats) in
            // *release builds*, for an unknown reason. sizeof(instanceDesc.Transform) is only 12
            // floats. Using memcpy_s() does not cause this problem, and it should be used
            // everywhere to be safe!
            D3D12_RAYTRACING_INSTANCE_DESC instanceDesc      = {};
            instanceDesc.AccelerationStructure               = pBLAS->GetGPUVirtualAddress();
            instanceDesc.InstanceID                          = 0;
            instanceDesc.InstanceMask                        = 0xFF;
            instanceDesc.InstanceContributionToHitGroupIndex = instanceIndex++;
            ::memcpy_s(instanceDesc.Transform, sizeof(instanceDesc.Transform), &matrix,
                sizeof(instanceDesc.Transform));
            instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;

            // Copy the instance description to the buffer.
            ::memcpy_s(pInstanceMappedData, sizeof(D3D12_RAYTRACING_INSTANCE_DESC), &instanceDesc,
                sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
            pInstanceMappedData += sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        }

        // Close the instance buffer.
        pInstanceBuffer->Unmap(0, nullptr); // no HRESULT
    }

    // Describe the top-level acceleration structure (TLAS).
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs = {};
    tlasInputs.Type          = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasInputs.DescsLayout   = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasInputs.Flags         = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
    tlasInputs.NumDescs      = instanceCount;
    tlasInputs.InstanceDescs = pInstanceBuffer ? pInstanceBuffer->GetGPUVirtualAddress() : 0;

    // Get the sizes required for the TLAS scratch and result buffers, and create them.
    // NOTE: The scratch buffer is obtained from the renderer and will be retained for the duration
    // of the build task started below, and then it will be released.
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo = {};
    _pRenderer->dxDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasInfo);
    D3D12_GPU_VIRTUAL_ADDRESS tlasScratchAddress =
        _pRenderer->getScratchBuffer(tlasInfo.ScratchDataSizeInBytes);
    ID3D12ResourcePtr pTLAS = _pRenderer->createBuffer(tlasInfo.ResultDataMaxSizeInBytes,
        "TLAS Buffer", D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);

    // Describe the build for the TLAS.
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc = {};
    tlasDesc.Inputs                                             = tlasInputs;
    tlasDesc.ScratchAccelerationStructureData                   = tlasScratchAddress;
    tlasDesc.DestAccelerationStructureData                      = pTLAS->GetGPUVirtualAddress();

    // Build the TLAS using a command list. Insert a UAV barrier so that it can't be used until
    // it is generated.
    ID3D12GraphicsCommandList4Ptr pCommandList = _pRenderer->beginCommandList();
    pCommandList->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);
    _pRenderer->addUAVBarrier(pTLAS.Get());

    // Complete the command list and task, and wait for the work to complete.
    // NOTE: This waits so that the instance buffer doesn't have to be retained beyond this
    // function. Since there is only one TLAS for a scene, this is not a practical bottleneck.
    _pRenderer->submitCommandList();
    _pRenderer->completeTask();
    _pRenderer->waitForTask();

    return pTLAS;
}

END_AURORA
