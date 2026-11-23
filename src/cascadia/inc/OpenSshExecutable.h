// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <filesystem>
#include <string_view>

namespace Microsoft::Terminal::OpenSsh
{
    // Keep the configured path expandable in command lines saved by Terminal.
    inline std::wstring_view FindExecutable() noexcept
    {
        try
        {
            for (const auto path : {
                     L"%SystemRoot%\\System32\\OpenSSH\\ssh.exe",
                     L"%ProgramFiles%\\OpenSSH\\ssh.exe",
                     L"%ProgramFiles(x86)%\\OpenSSH\\ssh.exe",
                 })
            {
                std::error_code error;
                if (std::filesystem::is_regular_file(wil::ExpandEnvironmentStringsW<std::wstring>(path), error))
                {
                    return path;
                }
            }
        }
        CATCH_LOG();

        return {};
    }
}
