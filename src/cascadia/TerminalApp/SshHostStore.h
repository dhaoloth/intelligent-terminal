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
    enum class HostAuthentication
    {
        OpenSsh,
        Password,
        IdentityFile,
    };

    enum class ForwardDirection
    {
        Local,
        Remote,
        Dynamic,
    };

    struct PortForward
    {
        ForwardDirection direction{ ForwardDirection::Local };
        std::wstring listenHost;
        uint32_t listenPort{};
        std::wstring destinationHost;
        uint32_t destinationPort{};

        bool operator==(const PortForward&) const = default;
    };

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
        std::wstring jumpHostAlias;
        HostAuthentication authentication{ HostAuthentication::OpenSsh };
        std::vector<PortForward> forwards{};

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
        JumpHostAlias,
        Forwarding,
    };

    InvalidHostField ValidateHost(const SavedHost& host);
    uint32_t ParsePort(std::wstring_view value) noexcept;
    std::wstring BuildCommandline(std::wstring_view executable, const SavedHost& host, std::wstring_view nativeAgent = {});
    std::wstring BuildSftpCommandline(std::wstring_view sftpExecutable, std::wstring_view sshExecutable, const SavedHost& host);
    void AppendWtaSshTarget(std::wstring& arguments, const SavedHost& host);
    std::vector<std::wstring> LoadConfigAliases();
    std::vector<std::wstring> LoadConfigAliases(const std::filesystem::path& configPath);

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
