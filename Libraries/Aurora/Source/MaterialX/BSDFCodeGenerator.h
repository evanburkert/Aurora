//
// Copyright 2023 by Autodesk, Inc.  All rights reserved.
//
// This computer source code and related instructions and comments
// are the unpublished confidential and proprietary information of
// Autodesk, Inc. and are protected under applicable copyright and
// trade secret law.  They may not be disclosed to, copied or used
// by any third party without the prior written consent of Autodesk, Inc.
//
#pragma once

#include "UniformBuffer.h"
#include <MaterialXCore/Generated.h>

// Forward declare MaterialX types.
namespace MaterialX_v1_38_5
{
class Document;
class FileSearchPath;
class ShaderGenerator;
class GenContext;
class ShaderStage;
class ShaderInput;
class Value;
class UnitConverterRegistry;
class UnitSystem;
class ShaderNode;
class TypeDesc;
} // namespace MaterialX_v1_38_5

#include "Properties.h"

BEGIN_AURORA

namespace MaterialXCodeGen
{

// Forward declare shader class (used internally for codegen)
class BSDFCodeGeneratorShader;
class BSDFShaderGenerator;

/// MaterialX code generator for BSDF and material setup.GLSL code for pathtracing.
/// TODO: Currently only generates the material setup function and associated metadata, not the
/// BSDF.
class BSDFCodeGenerator
{
public:
    // The MaterialX units.
    struct Units
    {
        // Unit names.
        std::vector<string> names;
        // Indices for units used in shaders.
        std::map<string, int> indices;
    };

    // The BSDF inputs that are produced by the setup function.
    struct BSDFInput
    {
        /// Input name
        string name;
        /// Input type as PropertyValue type enum
        PropertyValue::Type type = PropertyValue::Type::Undefined;
    };

    /// Code generation result.
    struct Result
    {
        /// Unique hash for the generated code.
        size_t functionHash = 0;
        /// \brief The generated GLSL code for material setup function.
        /// \desc The setup function takes a set of material inputs from CPU and outputs the
        /// parameters of the Standard Surface material, based on a MaterialX network.
        string materialSetupCode;
        /// \desc The GLSL structure for the material object used as an input to this material.
        string materialStructCode;
        /// \desc The name of the setup function .
        string setupFunctionName;
        /// \desc The name of the material structure type.
        string materialStructName;
        /// \brief Vector of material properties in material struct that is the input to the setup
        /// function.
        vector<UniformBufferPropertyDefinition> materialProperties;
        /// \brief Vector of textures used by this material in the setup function.
        vector<string> textures;
        /// \brief Vector of BSDF inputs output by the setup function.
        vector<BSDFInput> bsdfInputs;
        /// \desc True if the setup fuction takes an integer unit parameter (index into unit names
        /// in the Units struct).
        bool hasUnits = false;
        /// Default values for material properties.</summary>
        vector<PropertyValue> materialPropertyDefaults;
        /// \brief Vector of textures used by this material in the setup function.
        vector<TextureDefinition> textureDefaults;
    };

    /// \param surfaceShaderNodeCategory The surface shader category to code generate material
    /// inputs for. \param mtlxPath The search path for the materialX assets, including the library
    /// assets in the 'libraries' folder.
    BSDFCodeGenerator(const string& mtlxPath = "./MaterialX", const string& = "standard_surface");

    ~BSDFCodeGenerator();

    /// \desc Run the code generator on a MaterialX document.
    /// \param.document MaterialX XML document string.
    /// \param inputParameterMapper Function used to map a parameter in the MaterialX network an
    /// input parameter in the setup function. \param outputParameterMapper Function used to map a
    /// parameter in the MaterialX network an ouput parameter in the setup function. \param
    /// pResultOut Code generation result output.
    bool generate(const string& document, Result* pResultOut,
        const set<string> supportedBSDFInputs = {}, const string& overrideDocumentName = "");

    /// \desc Generate the shared GLSL definitions for all the documents generated by this
    /// generator. Will clear the shared definitions.
    /// \param pDefinitionCodeOut GLSL definition string.
    int generateDefinitions(string* pDefinitionCodeOut);

    /// Clear the definition shader code, which is accumulated after each generateDefinitions call.
    void clearDefinitions();

    // Get the units used by materialX.
    const Units& units() { return _units; }

protected:
    enum ParameterType
    {
        MaterialProperty,
        Texture,
        BuiltIn
    };

    struct Parameter
    {
        string variableName;
        string path;
        string type;
        string glslType;
        int index               = 0;
        int parameterIndex      = 0;
        ParameterType paramType = MaterialProperty;
    };

    // Convert GLSL type string to Aurora type (asserts if conversion fails.)
    PropertyValue::Type glslTypeToAuroraType(const string glslType);

    // Convert materialX value to Aurora value (asserts if conversion fails.)
    bool materialXValueToAuroraValue(Value* pValueOut, shared_ptr<MaterialX::Value> pMtlXValue);

    bool materialXValueToAuroraPropertyValue(
        PropertyValue* pValueOut, shared_ptr<MaterialX::Value> pMtlXValue);

    // Process a MaterialX shader input.
    void processInput(MaterialX::ShaderInput* input,
        shared_ptr<BSDFCodeGeneratorShader> pBSDFGenShader, const string& outputVariable,
        string* pSourceOut);

    // Create the static MaterialX standard library.
    void createStdLib();

    // Static MaterialX standard library.
    static shared_ptr<MaterialX::Document> s_pStdLib;

    // The GLSL shader generator used internally.
    shared_ptr<BSDFShaderGenerator> _pGenerator;

    // The shader context used internally.
    unique_ptr<MaterialX::GenContext> _pGeneratorContext;

    // Current active material inputs and outputs.
    map<string, string> _activeBSDFInputs;
    map<string, int> _parameterIndexLookup;
    map<string, int> _builtInIndexLookup;
    vector<Parameter> _parameters;
    vector<UniformBufferPropertyDefinition> _materialProperties;
    vector<PropertyValue> _materialPropertyDefaults;
    vector<TextureDefinition> _textures;
    vector<pair<string, string>> _builtIns;
    vector<string> _activeBSDFInputNames;

    // Definition look-up.
    map<string, size_t> _definitionMap;
    vector<string> _definitions;

    // The surface shader category to code generate inputs for.
    string _surfaceShaderNodeCategory;

    // The units for MaterialX;
    Units _units;

    shared_ptr<MaterialX::UnitSystem> _unitSystem;
    shared_ptr<MaterialX::UnitConverterRegistry> _unitRegistry;
    set<MaterialX::ShaderNode*> _processedNodes;
    string _topLevelShaderNodeName;
    string _mtlxLibPath;
    bool _hasUnits = false;
};

} // namespace MaterialXCodeGen

END_AURORA
