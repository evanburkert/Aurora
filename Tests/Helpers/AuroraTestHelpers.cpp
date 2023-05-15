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

#include "AuroraTestHelpers.h"

#include <Aurora/Foundation/Geometry.h>
#include <Aurora/Foundation/Log.h>
#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

#include "TeapotModel.h"
#include "TestHelpers.h"

#pragma warning(push)
// Disabe type conversion warnings intruduced from stb master.
// refer to the commit in stb
// https://github.com/nothings/stb/commit/b15b04321dfd8a2307c49ad9c5bf3c0c6bcc04cc
#pragma warning(disable : 4244)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#pragma warning(pop)

using namespace Aurora::Foundation;
using namespace Aurora;

namespace TestHelpers
{
// Position data for single planar quad.
static const array<float, 12> g_planePositionArray = {
    -1.0f,
    -1.0f,
    0.0f,
    +1.0f,
    -1.0f,
    0.0f,
    -1.0f,
    +1.0f,
    0.0f,
    +1.0f,
    +1.0f,
    0.0f,
};

// Normal data for single planar quad.
static const array<float, 12> g_planeNormalArray = {
    0.0f,
    0.0f,
    -1.0f,
    0.0f,
    0.0f,
    -1.0f,
    0.0f,
    0.0f,
    -1.0f,
    0.0f,
    0.0f,
    -1.0f,
};

// Tangent data for single planar quad.
static const array<float, 12> g_planeTangentArray = {
    0.0f,
    -1.0f,
    0.0f,

    0.0f,
    -1.0f,
    0.0f,

    0.0f,
    -1.0f,
    0.0f,

    0.0f,
    -1.0f,
    0.0f,
};
// UV data for single planar quad.
static const array<float, 8> g_planeUVArray = {
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    1.0f,
    1.0f,
    1.0f,
};

// Index data for single planar quad.
static const array<unsigned int, 6> g_planeIndexArray = {
    2,
    1,
    0,
    1,
    2,
    3,
};

// Teapot tangents (generated on demand.)
vector<float> gTeapotTangents;

const float* TeapotModel::vertices()
{
    // Get pointer to array generated by from OBJ file.
    return &gTeapotVertices[0];
}

const float* TeapotModel::normals()
{
    // Get pointer to array generated by from OBJ file.
    return &gTeapotNormals[0];
}

const float* TeapotModel::uvs()
{
    // Get pointer to array generated by from OBJ file.
    return &gTeapotUVs[0];
}

const float* TeapotModel::tangents()
{
    // Generate the tangents as needed.
    if (gTeapotTangents.empty())
    {
        gTeapotTangents.resize(gTeapotNormals.size());
        Aurora::Foundation::calculateTangents(gTeapotNormals.size() / 3, gTeapotVertices.data(),
            gTeapotNormals.data(), gTeapotUVs.data(), gTeapotIndices.size() / 3,
            gTeapotIndices.data(), gTeapotTangents.data());
    }

    // Get pointer to array generated by from OBJ file.
    return &gTeapotTangents[0];
}

uint32_t TeapotModel::verticesCount()
{
    // Get size of array generated by from OBJ file (divide by three for 3D vertices.)
    return (uint32_t)(gTeapotVertices.size() / 3);
}

const unsigned int* TeapotModel::indices()
{
    // Get pointer to array generated by from OBJ file.
    return &gTeapotIndices[0];
}

uint32_t TeapotModel::indicesCount()
{
    // Get size of array generated by from OBJ file.
    return (uint32_t)gTeapotIndices.size();
}

// Expect a IValue failure (assert on debug, exception on release.)
#if _DEBUG
#define ASSERT_VALUE_FAIL(_expr) ASSERT_DEATH(_expr, "Assertion failed:*")
#else
#define ASSERT_VALUE_FAIL(_expr) ASSERT_ANY_THROW(_expr)
#endif

// Helper function to load an image from disk.
void FixtureBase::loadImage(const string& filename, ImageData* pImageOut)
{
    // Use STB to load image file.
    int width, height, components;
    unsigned char* pPixels = stbi_load(filename.c_str(), &width, &height, &components, 0);
    AU_ASSERT(pPixels != nullptr, "Failed to load image:%s", filename.c_str());

    // Create Aurora image data struct from the pixels.
    ImageDescriptor& imageDesc = pImageOut->descriptor;
    imageDesc.width            = width;
    imageDesc.height           = height;
    imageDesc.linearize        = true;
    imageDesc.format           = ImageFormat::Integer_RGBA;
    size_t numPixels           = static_cast<size_t>(width * height * 4);
    pImageOut->buffer.resize(numPixels * 4);
    unsigned char* pImageData = &(pImageOut->buffer)[0];

    // STB usually provides RGB, pad to RGBA if needed.
    if (components == 3)
    {
        // Flip vertically as STB loads upside down.
        for (int i = 0; i < height; i++)
        {
            int flippedRow      = (height - 1 - i);
            unsigned char* pIn  = pPixels + (flippedRow * width * 3);
            unsigned char* pOut = ((unsigned char*)pImageData) + (i * width * 4);
            for (int j = 0; j < width; j++)
            {
                *(pOut++) = *(pIn++);
                *(pOut++) = *(pIn++);
                *(pOut++) = *(pIn++);
                *(pOut++) = 255;
            }
        }
    }
    else if (components == 4)
    {
        // Flip vertically as STB loads upside down.
        for (int i = 0; i < height; i++)
        {
            int flippedRow      = (height - 1 - i);
            unsigned char* pIn  = pPixels + (flippedRow * width * 4);
            unsigned char* pOut = ((unsigned char*)pImageData) + (i * width * 4);
            for (int j = 0; j < width; j++)
            {
                *(pOut++) = *(pIn++);
                *(pOut++) = *(pIn++);
                *(pOut++) = *(pIn++);
                *(pOut++) = *(pIn++);
            }
        }
    }
    else
    {
        AU_FAIL("%s invalid number of components %d", filename.c_str(), components);
    }

    free(pPixels);

    // Set up the pixel data callback
    imageDesc.getPixelData = [pImageOut](PixelData& dataOut, glm::ivec2, glm::ivec2) {
        // Get addres and size from buffer (assumes will be called from scope of test, so buffer
        // still valid)
        dataOut.address = pImageOut->buffer.data();
        dataOut.size    = pImageOut->buffer.size();
        return true;
    };
};

// Convenience function to test a float3 value.
void FixtureBase::testFloat3Value(
    IScene& scene, const Path& material, const string& name, bool exists, const string& message)
{
    Properties matProps;

    // Arbitrary test value.
    matProps[name] = vec3(1.0f, 2.0f, 3.0f);

    // If value exists expect it can be set without crashing otherwise expect assert.
    if (exists)
    {
        ASSERT_NO_FATAL_FAILURE(scene.setMaterialProperties(material, matProps))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        matProps.clear();

        // Expect an assert if set with wrong type.
        matProps[name] = 123.0f;
        ASSERT_NO_FATAL_FAILURE(scene.setMaterialProperties(material, matProps))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        ;
        ASSERT_THAT(lastLogMessage(), ::testing::StartsWith("Type mismatch in UniformBlock"))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        ;
    }
    else
    {
        ASSERT_NO_FATAL_FAILURE(scene.setMaterialProperties(material, matProps))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        ;
        ASSERT_THAT(lastLogMessage(), ::testing::StartsWith("Type mismatch in UniformBlock"))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
    }
}

// Convenience function to test a float value.
void FixtureBase::testFloatValue(
    IScene& scene, const Path& material, const string name, bool exists, const string& message)
{
    Properties matProps;

    // Arbitrary test value.
    const float testVal = 42.0f;

    matProps[name] = testVal;

    // If value exists expect it can be set without crashing otherwise expect assert.
    if (exists)
    {
        ASSERT_NO_FATAL_FAILURE(scene.setMaterialProperties(material, matProps))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        matProps.clear();

        // Expect an assert if set with wrong type.
        matProps[name] = vec3(1.0f, 2.0f, 3.0f);
        ASSERT_NO_FATAL_FAILURE(scene.setMaterialProperties(material, matProps))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        ;
        ASSERT_THAT(lastLogMessage(), ::testing::StartsWith("Type mismatch in UniformBlock"))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        ;
    }
    else
    {
        ASSERT_NO_FATAL_FAILURE(scene.setMaterialProperties(material, matProps))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        ;
        ASSERT_THAT(lastLogMessage(), ::testing::StartsWith("Type mismatch in UniformBlock"))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        ;
    }
}

// Convenience function to test a matrix value.
void FixtureBase::testMatrixValue(
    IScene& scene, const Path& material, const string& name, bool exists, const string& message)
{
    Properties matProps;

    // Arbitrary test value.
    mat4 testMat(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f,
        14.0f, 15.0f, 16.0f);

    matProps[name] = testMat;

    // If value exists expect it can be set without crashing otherwise expect assert.
    if (exists)
    {
        ASSERT_NO_FATAL_FAILURE(scene.setMaterialProperties(material, matProps))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        matProps.clear();

        // Expect an assert if set with wrong type.
        matProps[name] = 42;
        ASSERT_NO_FATAL_FAILURE(scene.setMaterialProperties(material, matProps))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        ;
        ASSERT_THAT(lastLogMessage(), ::testing::StartsWith("Type mismatch in UniformBlock"))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
    }
    else
    {
        ASSERT_NO_FATAL_FAILURE(scene.setMaterialProperties(material, matProps))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        ;
        ASSERT_THAT(lastLogMessage(), ::testing::StartsWith("Type mismatch in UniformBlock"))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        ;
    }
}

// Convenience function to test a boolean value.
void FixtureBase::testBooleanValue(
    IScene& scene, const Path& material, const string& name, bool exists, const string& message)
{
    Properties matProps;

    // Arbitrary test value.
    bool testVal = true;

    matProps[name] = testVal;

    // If value exists expect it can be set without crashing otherwise expect assert.
    if (exists)
    {
        ASSERT_NO_FATAL_FAILURE(scene.setMaterialProperties(material, matProps))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        matProps.clear();

        // Expect an assert if set with wrong type.
        matProps[name] = vec3(1.0f, 2.0f, 3.0f);
        ASSERT_NO_FATAL_FAILURE(scene.setMaterialProperties(material, matProps))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
        ;
        ASSERT_THAT(lastLogMessage(), ::testing::StartsWith("Type mismatch in UniformBlock"))
            << " " << __FUNCTION__ << " value:" << name << " " << message;
    }
    else
    {
        ASSERT_THROW(
            scene.setMaterialProperties(material, matProps), TestHelpers::AuroraLoggerException);
        ASSERT_THAT(lastLogMessage(),
            ::testing::StartsWith("AU_ASSERT test failed:\nEXPRESSION: find(name) != end"));
    }
}

// Convenience function to test a float3 option value.
void FixtureBase::testFloat3Option(
    IRenderer& renderer, const string& name, bool exists, const string& message)
{
    Properties options;

    // Arbitrary test value.
    vec3 testValue(1.0f, 2.0f, 3.0f);

    options[name] = testValue;

    // If value exists expect it can be set without crashing otherwise expect assert.
    if (exists)
    {
        ASSERT_NO_FATAL_FAILURE(renderer.setOptions(options))
            << " " << __FUNCTION__ << " value:" << name << " " << message;

        options[name] = 123.0f;

        // Expect an assert if set with wrong type.
        ASSERT_THROW(renderer.setOptions(options), TestHelpers::AuroraLoggerException);
        ASSERT_THAT(lastLogMessage(),
            ::testing::StartsWith("AU_ASSERT test failed:\nEXPRESSION: typesMatch"));
    }
    else
    {
        ASSERT_THROW(renderer.setOptions(options), TestHelpers::AuroraLoggerException);
        ASSERT_THAT(lastLogMessage(),
            ::testing::StartsWith("AU_ASSERT test failed:\nEXPRESSION: find(name) != end"));
    }
}

// Convenience function to test a float option value.
void FixtureBase::testFloatOption(
    IRenderer& renderer, const string& name, bool exists, const string& message)
{
    Properties options;

    // Arbitrary test value.
    float testVal = 42.0f;

    options[name] = testVal;

    // If value exists expect it can be set without crashing otherwise expect assert.
    if (exists)
    {
        ASSERT_NO_FATAL_FAILURE(renderer.setOptions(options))
            << " " << __FUNCTION__ << " value:" << name << " " << message;

        // Expect an assert if set with wrong type.
        vec3 testColor(1.0f, 2.0f, 3.0f);
        options[name] = testColor;

        ASSERT_THROW(renderer.setOptions(options), TestHelpers::AuroraLoggerException);
        ASSERT_THAT(lastLogMessage(),
            ::testing::StartsWith("AU_ASSERT test failed:\nEXPRESSION: typesMatch"));
    }
    else
    {
        ASSERT_THROW(renderer.setOptions(options), TestHelpers::AuroraLoggerException);
        ASSERT_THAT(lastLogMessage(),
            ::testing::StartsWith("AU_ASSERT test failed:\nEXPRESSION: find(name) != end"));
    }
}

// Convenience function to test a boolean option value.
void FixtureBase::testBooleanOption(
    IRenderer& renderer, const string& name, bool exists, const string& message)
{
    Properties options;

    // Arbitrary test value.
    bool testVal = true;

    options[name] = testVal;

    // If value exists expect it can be set without crashing otherwise expect assert.
    if (exists)
    {
        ASSERT_NO_FATAL_FAILURE(renderer.setOptions(options))
            << " " << __FUNCTION__ << " value:" << name << " " << message;

        // Expect an assert if set with wrong type.
        options[name] = vec3(1.0f, 2.0f, 3.0f);

        ASSERT_THROW(renderer.setOptions(options), TestHelpers::AuroraLoggerException);
        ASSERT_THAT(lastLogMessage(),
            ::testing::StartsWith("AU_ASSERT test failed:\nEXPRESSION: typesMatch"));
    }
    else
    {
        ASSERT_THROW(renderer.setOptions(options), TestHelpers::AuroraLoggerException);
        ASSERT_THAT(lastLogMessage(),
            ::testing::StartsWith("AU_ASSERT test failed:\nEXPRESSION: find(name) != end"));
    }
}

// Convenience function to test an int value.
void FixtureBase::testIntOption(
    IRenderer& renderer, const string& name, bool exists, const string& message)
{
    Properties options;

    // Arbitrary test values.
    int testVal = 42;

    options[name] = testVal;

    // If value exists expect it can be set without crashing otherwise expect assert.
    if (exists)
    {
        ASSERT_NO_FATAL_FAILURE(renderer.setOptions(options))
            << " " << __FUNCTION__ << " value:" << name << " " << message;

        // Expect an assert if set with wrong type.
        vec3 testValue(1.0f, 2.0f, 3.0f);
        options[name] = testValue;

        ASSERT_THROW(renderer.setOptions(options), TestHelpers::AuroraLoggerException);
        ASSERT_THAT(lastLogMessage(),
            ::testing::StartsWith("AU_ASSERT test failed:\nEXPRESSION: typesMatch"));
    }
    else
    {
        ASSERT_THROW(renderer.setOptions(options), TestHelpers::AuroraLoggerException);
        ASSERT_THAT(lastLogMessage(),
            ::testing::StartsWith("AU_ASSERT test failed:\nEXPRESSION: find(name) != end"));
    }
}

IRendererPtr FixtureBase::createDefaultRenderer(int width, int height)
{
    IRenderer::Backend type = rendererBackend();
    if (!backendSupported())
    {
        _pDefaultRenderer = nullptr;
        return _pDefaultRenderer;
    }

    // Create a renderer for this type.
    _pDefaultRenderer = createRenderer(type);

    // Set the renderer width/height properties.
    _defaultRendererWidth  = width;
    _defaultRendererHeight = height;

    // Overwrite default renderer width from an environment variable. This also prints a warning,
    // if the variable is set, to avoid baseline images being committed to source control with
    // the wrong resolution.
    if (TestHelpers::getIntegerEnvironmentVariable(
            "BASELINE_IMAGE_WIDTH", (int*)&_defaultRendererWidth))
        cout << "BASELINE_IMAGE_WIDTH env var is set, renderer test baseline images will be "
                "rendered with width "
             << _defaultRendererWidth
             << " (NOTE: This is for debug purposes only, do not commit baseline images with this "
                "set!)"
             << endl;

    // Overwrite default renderer height from an environment variable. This also prints a warning,
    // if the variable is set, to avoid baseline images being committed to source control with
    // the wrong resolution.
    if (TestHelpers::getIntegerEnvironmentVariable(
            "BASELINE_IMAGE_HEIGHT", (int*)&_defaultRendererHeight))
        cout << "BASELINE_IMAGE_HEIGHT env var is set, renderer test baseline images will be "
                "rendered with height "
             << _defaultRendererHeight
             << " (NOTE: This is for debug purposes only, do not commit baseline images with this "
                "set!)"
             << endl;

    // Create a default render buffer.
    _pDefaultRenderBuffer = _pDefaultRenderer->createRenderBuffer(
        _defaultRendererWidth, _defaultRendererHeight, ImageFormat::Integer_RGBA);
    _pDefaultRenderer->setTargets({ { AOV::kFinal, _pDefaultRenderBuffer } });

    // Setup default camera position.
    _projMtx = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 1000.0f);
    _viewMtx = glm::lookAt(
        glm::vec3(0.0f, 0.0f, -5.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    _pDefaultRenderer->setCamera(glm::value_ptr(_viewMtx), glm::value_ptr(_projMtx));

    return _pDefaultRenderer;
}

void FixtureBase::setDefaultRendererPerspective(float fovDeg, float near, float far)
{
    float aspect =
        static_cast<float>(_defaultRendererWidth) / static_cast<float>(_defaultRendererHeight);
    _projMtx = glm::perspective(glm::radians(fovDeg), aspect, near, far);

    _pDefaultRenderer->setCamera(
        reinterpret_cast<float*>(&_viewMtx), reinterpret_cast<float*>(&_projMtx));
}

void FixtureBase::setDefaultRendererCamera(
    const glm::vec3& eye, const glm::vec3& target, const glm::vec3& up)
{
    _viewMtx = glm::lookAt(eye, target, up);
    _pDefaultRenderer->setCamera(
        reinterpret_cast<float*>(&_viewMtx), reinterpret_cast<float*>(&_projMtx));
}

Path FixtureBase::createTeapotGeometry(IScene& scene)
{
    Path geomPath = nextPath("TeapotGeometry");
    GeometryDescriptor geomDesc;
    geomDesc.type                                                       = PrimitiveType::Triangles;
    geomDesc.vertexDesc.attributes[Names::VertexAttributes::kPosition]  = AttributeFormat::Float3;
    geomDesc.vertexDesc.attributes[Names::VertexAttributes::kNormal]    = AttributeFormat::Float3;
    geomDesc.vertexDesc.attributes[Names::VertexAttributes::kTangent]   = AttributeFormat::Float3;
    geomDesc.vertexDesc.attributes[Names::VertexAttributes::kTexCoord0] = AttributeFormat::Float2;
    geomDesc.vertexDesc.count = TeapotModel::verticesCount();
    geomDesc.indexCount       = TeapotModel::indicesCount();
    geomDesc.getAttributeData = [](AttributeDataMap& buffers, size_t firstVertex,
                                    size_t vertexCount, size_t firstIndex, size_t indexCount) {
        AU_ASSERT(firstVertex == 0, "Partial update not supported");
        AU_ASSERT(vertexCount == TeapotModel::verticesCount(), "Partial update not supported");
        buffers[Names::VertexAttributes::kPosition].address = TeapotModel::vertices();
        buffers[Names::VertexAttributes::kPosition].size =
            TeapotModel::verticesCount() * sizeof(vec3);
        buffers[Names::VertexAttributes::kPosition].stride = sizeof(vec3);
        buffers[Names::VertexAttributes::kNormal].address  = TeapotModel::normals();
        buffers[Names::VertexAttributes::kNormal].size =
            TeapotModel::verticesCount() * sizeof(vec3);
        buffers[Names::VertexAttributes::kNormal].stride   = sizeof(vec3);
        
        buffers[Names::VertexAttributes::kTangent].address = TeapotModel::tangents();
        buffers[Names::VertexAttributes::kTangent].size =
            TeapotModel::verticesCount() * sizeof(vec3);
        buffers[Names::VertexAttributes::kTangent].stride    = sizeof(vec3);
        
        buffers[Names::VertexAttributes::kTexCoord0].address = TeapotModel::uvs();
        buffers[Names::VertexAttributes::kTexCoord0].size =
            TeapotModel::verticesCount() * sizeof(vec2);
        buffers[Names::VertexAttributes::kTexCoord0].stride = sizeof(vec2);

        AU_ASSERT(firstIndex == 0, "Partial update not supported");
        AU_ASSERT(indexCount == TeapotModel::indicesCount(), "Partial update not supported");

        buffers[Names::VertexAttributes::kIndices].address = TeapotModel::indices();
        buffers[Names::VertexAttributes::kIndices].size =
            TeapotModel::indicesCount() * sizeof(uint32_t);
        buffers[Names::VertexAttributes::kIndices].stride = sizeof(uint32_t);

        return true;
    };

    scene.setGeometryDescriptor(geomPath, geomDesc);
    return geomPath;
}

Path FixtureBase::createPlaneGeometry(IScene& scene, vec2 uvScale, vec2 uvOffset)
{
    Path geomPath = nextPath("PlaneGeometry");
    GeometryDescriptor geomDesc;
    geomDesc.type                                                       = PrimitiveType::Triangles;
    geomDesc.vertexDesc.attributes[Names::VertexAttributes::kPosition]  = AttributeFormat::Float3;
    geomDesc.vertexDesc.attributes[Names::VertexAttributes::kNormal]    = AttributeFormat::Float3;
    geomDesc.vertexDesc.attributes[Names::VertexAttributes::kTangent]   = AttributeFormat::Float3;
    geomDesc.vertexDesc.attributes[Names::VertexAttributes::kTexCoord0] = AttributeFormat::Float2;
    geomDesc.vertexDesc.count = g_planePositionArray.size() / 3;

    geomDesc.indexCount  = g_planeIndexArray.size();
    vec2* transformedUVs = new vec2[geomDesc.vertexDesc.count];
    for (size_t i = 0; i < geomDesc.vertexDesc.count; i++)
    {
        vec2 uv(g_planeUVArray[i * 2 + 0], g_planeUVArray[i * 2 + 1]);
        uv *= uvScale;
        uv += uvOffset;
        transformedUVs[i] = uv;
    }

    geomDesc.attributeUpdateComplete = [transformedUVs](const Aurora::AttributeDataMap&, size_t,
                                           size_t, size_t, size_t) { delete[] transformedUVs; };

    geomDesc.getAttributeData = [transformedUVs](AttributeDataMap& buffers, size_t firstVertex,
                                    size_t vertexCount, size_t firstIndex, size_t indexCount) {
        AU_ASSERT(firstVertex == 0, "Partial update not supported");
        AU_ASSERT(vertexCount == g_planePositionArray.size() / 3, "Partial update not supported");

        buffers[Names::VertexAttributes::kPosition].address = g_planePositionArray.data();
        buffers[Names::VertexAttributes::kPosition].size =
            g_planePositionArray.size() * sizeof(float);
        buffers[Names::VertexAttributes::kPosition].stride = sizeof(vec3);
        buffers[Names::VertexAttributes::kNormal].address  = g_planeNormalArray.data();
        buffers[Names::VertexAttributes::kNormal].size = g_planeNormalArray.size() * sizeof(float);
        buffers[Names::VertexAttributes::kNormal].stride   = sizeof(vec3);
        buffers[Names::VertexAttributes::kTangent].address = g_planeTangentArray.data();
        buffers[Names::VertexAttributes::kTangent].size =
            g_planeTangentArray.size() * sizeof(float);
        buffers[Names::VertexAttributes::kTangent].stride = sizeof(vec3);

        buffers[Names::VertexAttributes::kTexCoord0].address = &transformedUVs[0];
        buffers[Names::VertexAttributes::kTexCoord0].size    = g_planeUVArray.size() * sizeof(vec2);
        buffers[Names::VertexAttributes::kTexCoord0].stride  = sizeof(vec2);

        AU_ASSERT(firstIndex == 0, "Partial update not supported");
        AU_ASSERT(indexCount == g_planeIndexArray.size(), "Partial update not supported");
        buffers[Names::VertexAttributes::kIndices].address = g_planeIndexArray.data();
        buffers[Names::VertexAttributes::kIndices].size =
            g_planeIndexArray.size() * sizeof(uint32_t);
        buffers[Names::VertexAttributes::kIndices].stride = sizeof(uint32_t);
        return true;
    };

    scene.setGeometryDescriptor(geomPath, geomDesc);
    return geomPath;
}

IScenePtr FixtureBase::createDefaultScene()
{
    // Ensure default renderer created.
    if (!_pDefaultRenderer)
    {
        // Default renderer null if current renderer type not supported.
        if (!createDefaultRenderer())
            return nullptr;
    }

    // Create default scene and set in renderer.
    _pDefaultScene = _pDefaultRenderer->createScene();

    // Create a default environment.
    const Path kDefaultEnvironmentPath = "TestDefaultEnvironment";
    _pDefaultScene->setEnvironmentProperties(kDefaultEnvironmentPath, {});

    // Set the environment (this will activate the texture associated with the environment, and run
    // the callback functions)s
    _pDefaultScene->setEnvironment(kDefaultEnvironmentPath);

    // Set arbritrary -1 to +1 scene bounds.
    vec3 boundsMin(-1, -1, -1);
    vec3 boundsMax(+1, +1, +1);
    _pDefaultScene->setBounds(boundsMin, boundsMax);

    _pDefaultDistantLight =
        _pDefaultScene->addLightPointer(Aurora::Names::LightTypes::kDistantLight);

    // Set the default scene in renderer.
    _pDefaultRenderer->setScene(_pDefaultScene);

    // Return it.
    return _pDefaultScene;
}

FixtureBase::FixtureBase() :
    _dataPath(TestHelpers::kSourceRoot + "/Tests/Assets"),
    // Default baseline image location is in the Aurora Tests folder (should be committed to Git)
    _renderedBaselineImagePath(TestHelpers::kSourceRoot + "/Tests/Aurora/BaselineImages"),
    // Default baseline image location is in local working folder (should not be committed to Git)
    _renderedOutputImagePath("./OutputImages")
{
    // Fill in the look-up values to go from string to IRenderer::Backend.
    _typeLookup["DirectX"] = IRenderer::Backend::DirectX;
    _typeLookup["HGI"]     = IRenderer::Backend::HGI;

    // Set the default baseline image comparison thresholds.
    resetBaselineImageThresholdsToDefaults();

    // Disable message boxes so we they don't break the tests in debug mode.
    logger().enableFailureDialog(false);

    // Add a custom logger callback that throws an exception for critical errors, this allows better
    // testing for error cases. It also stores last message for testing purposes.
    auto cb = [this](const string& file, int line, Log::Level level, const string& msg) {
        _lastLogMessage = msg;
        if (level >= Log::Level::kWarn)
        {
            _errorAndWarningCount++;
        }

        if (level == Log::Level::kFail)
        {
            // Throw exception with error message.
            throw AuroraLoggerException(msg.c_str(), file.c_str(), line);
        }
        return true;
    };
    logger().setLogFunction(cb);
}

TestHelpers::BaselineImageComparison::Result FixtureBase::renderAndCheckBaselineImage(
    const string& name, const string& folder)
{
    // Multiple iterations on PT.
    uint32_t start = 0;
    for (uint32_t i = 0; i < _defaultPathTracingIterations; i++)
    {
        _pDefaultRenderer->render(start, _defaultRendererSampleCount);
        start += _defaultRendererSampleCount;
    }

    // Get the pixels from the default render buffer.
    size_t stride;
    const uint8_t* renderedImage =
        reinterpret_cast<const uint8_t*>(defaultRenderBuffer()->data(stride, true));

    // Ensure baseline and output folder are created.
    TestHelpers::createDirectory(_renderedBaselineImagePath);
    TestHelpers::createDirectory(_renderedOutputImagePath);

    // Concatenate folder, if non-zero length.
    string baselinePath =
        folder.size() ? _renderedBaselineImagePath + "/" + folder : _renderedBaselineImagePath;
    // Concatenate folder, if non-zero length.
    string outputPath =
        folder.size() ? _renderedOutputImagePath + "/" + folder : _renderedOutputImagePath;
    // run a baseline image comparison on the result (using current thresholds from this fixture.)
    return TestHelpers::BaselineImageComparison::compare(renderedImage, defaultRendererWidth(),
        defaultRendererHeight(), baselinePath, outputPath, name, baselineImageThresholds);
}

void FixtureBase::setBaselineImageThresholds(float pixelFailPercent, float maxPercentFailingPixels,
    float pixelWarnPercent, float maxPercentWarningPixels)
{
    // Set the thresholds used in subsequent calls to renderAndCheckBaselineImage
    baselineImageThresholds.pixelFailPercent        = pixelFailPercent;
    baselineImageThresholds.maxPercentFailingPixels = maxPercentFailingPixels;
    baselineImageThresholds.pixelWarnPercent        = pixelWarnPercent;
    baselineImageThresholds.maxPercentWarningPixels = maxPercentWarningPixels;
}

} // namespace TestHelpers
