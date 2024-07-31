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

#pragma once

class HdAuroraRendererPlugin : public HdRendererPlugin
{
public:
    HdAuroraRendererPlugin()          = default;
    virtual ~HdAuroraRendererPlugin() = default;

    HdRenderDelegate* CreateRenderDelegate() override;
    HdRenderDelegate* CreateRenderDelegate(HdRenderSettingsMap const& settingsMap) override;

    void DeleteRenderDelegate(HdRenderDelegate* renderDelegate) override;

// The extra arguments to IsSupported were added with Hydra version 46. As we need to support
// both USD 24.08 and older versions, make arguments conditional at compile time.
#if HD_API_VERSION >= 46
    virtual bool IsSupported(bool gpuEnabled = true) const override;
#else
    bool IsSupported() const override;
#endif
private:
    HdAuroraRendererPlugin(const HdAuroraRendererPlugin&)            = delete;
    HdAuroraRendererPlugin& operator=(const HdAuroraRendererPlugin&) = delete;
};
