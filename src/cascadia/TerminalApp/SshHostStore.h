// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Microsoft::Terminal::OpenSsh
{
    struct SavedHost
    {
        std::wstring id;
        std::wstring name;
        std::wstring group;
        std::wstring address;
        uint32_t port{ 22 };
        std::wstring username;
        std::wstring identityFile;
        bool usesOpenSshConfig{ false };

        bool operator==(const SavedHost&) const = default;
    };

    enum class InvalidHostField
    {
        None,
        Name,
        Group,
        Address,
        Port,
        Username,
        IdentityFile,
    };

    InvalidHostField ValidateHost(const SavedHost& host);
    uint32_t ParsePort(std::wstring_view value) noexcept;
    std::wstring BuildCommandline(std::wstring_view executable, const SavedHost& host);
    std::vector<std::wstring> LoadConfigAliases();

    class HostStore
    {
    public:
        explicit HostStore(const std::filesystem::path& settingsDirectory);

        std::vector<SavedHost> Load() const;
        std::size_t ImportAliases(const std::vector<std::wstring>& aliases) const;

        // Reload under a writer lock to preserve edits from other windows. An
        // empty result signals that the host changed since the editor loaded it.
        std::optional<std::vector<SavedHost>> Replace(const std::optional<SavedHost>& original,
                                                      const std::optional<SavedHost>& replacement) const;

    private:
        std::filesystem::path _path;
    };
}
