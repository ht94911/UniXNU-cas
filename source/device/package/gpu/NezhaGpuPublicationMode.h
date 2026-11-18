/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once

enum class NezhaGpuPublicationMode { Invalid = -1, Off = 0, Rendering = 1, Diagnostic = 2 };

static constexpr NezhaGpuPublicationMode nezhaGpuPublicationMode(int argument)
{
    return argument == 0 ? NezhaGpuPublicationMode::Off :
           argument == 1 ? NezhaGpuPublicationMode::Rendering :
           argument == 2 ? NezhaGpuPublicationMode::Diagnostic :
                           NezhaGpuPublicationMode::Invalid;
}

static constexpr bool nezhaGpuUsesDisplayFences(NezhaGpuPublicationMode mode)
{
    return mode == NezhaGpuPublicationMode::Rendering;
}
