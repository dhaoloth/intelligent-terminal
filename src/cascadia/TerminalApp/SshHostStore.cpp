// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SshHostStore.h"

#include "../../types/inc/utils.hpp"
#include <json/json.h>
#include <til/commandline.h>
#include <til/io.h>
#include <ws2tcpip.h>
#include <cwctype>

#pragma comment(lib, "Ws2_32.lib")

namespace Microsoft::Terminal::OpenSsh
{
    namespace
    {
        constexpr size_t MaxNameLength{ 120 };
        constexpr size_t MaxGroupLength{ 512 };
        constexpr size_t MaxGroupDepth{ 8 };
        constexpr size_t MaxIdentityLength{ 4096 };

        constexpr bool _isAsciiDigit(const wchar_t ch) noexcept
        {
            return ch >= L'0' && ch <= L'9';
        }

        constexpr bool _isAsciiLetter(const wchar_t ch) noexcept
        {
            return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z');
        }

        constexpr bool _isSimpleNameChar(const wchar_t ch) noexcept
        {
            return _isAsciiDigit(ch) || _isAsciiLetter(ch) || ch == L'.' || ch == L'-' || ch == L'_';
        }

        constexpr bool _isControlCharacter(const wchar_t ch) noexcept
        {
            return ch < L' ' || (ch >= 0x7f && ch <= 0x9f);
        }

        bool _isValidLabel(const std::wstring_view label) noexcept
        {
            return !label.empty() && label.size() <= MaxNameLength && label.front() != L' ' && label.back() != L' ' &&
                   std::ranges::none_of(label, _isControlCharacter);
        }

        bool _isValidGroup(std::wstring_view group) noexcept
        {
            if (group.empty())
            {
                return true;
            }
            if (group.size() > MaxGroupLength)
            {
                return false;
            }

            for (size_t depth = 0; depth < MaxGroupDepth; ++depth)
            {
                const auto slash = group.find(L'/');
                if (!_isValidLabel(group.substr(0, slash)))
                {
                    return false;
                }
                if (slash == std::wstring_view::npos)
                {
                    return true;
                }
                group.remove_prefix(slash + 1);
            }
            return false;
        }

        bool _isValidAddress(const std::wstring& address) noexcept
        {
            if (address.empty() || address.size() > 253 || address.front() == L'-')
            {
                return false;
            }
            if (address.find(L':') != std::wstring::npos)
            {
                IN6_ADDR ipv6{};
                return address.find_first_not_of(L"0123456789abcdefABCDEF:.") == std::wstring::npos &&
                       InetPtonW(AF_INET6, address.c_str(), &ipv6) == 1;
            }
            return std::ranges::all_of(address, _isSimpleNameChar) && address.find_first_not_of(L'.') != std::wstring::npos;
        }

        bool _isValidIdentity(const std::wstring_view path) noexcept
        {
            if (path.empty())
            {
                return true;
            }
            // Accept a literal, absolute drive path. ConPTY expands percent
            // variables and OpenSSH also expands percent and ${...} tokens.
            return path.size() > 3 && path.size() <= MaxIdentityLength && _isAsciiLetter(path[0]) && path[1] == L':' &&
                   (path[2] == L'\\' || path[2] == L'/') &&
                   path.find_first_of(L"<>:\"|?*%", 2) == std::wstring_view::npos &&
                   path.find(L"${") == std::wstring_view::npos &&
                   std::ranges::none_of(path, _isControlCharacter);
        }

        std::wstring _readString(const Json::Value& object, const char* field)
        {
            const auto& value = object[field];
            THROW_HR_IF(WEB_E_INVALID_JSON_STRING, !value.isString());
            return std::wstring{ winrt::to_hstring(value.asString()) };
        }

        const char* _authenticationName(const HostAuthentication authentication) noexcept
        {
            switch (authentication)
            {
            case HostAuthentication::OpenSsh:
                return "openssh";
            case HostAuthentication::Password:
                return "password";
            case HostAuthentication::IdentityFile:
                return "identityFile";
            }
            return "";
        }

        std::optional<HostAuthentication> _parseAuthentication(const Json::Value& value)
        {
            if (value.asString() == "openssh")
            {
                return HostAuthentication::OpenSsh;
            }
            if (value.asString() == "password")
            {
                return HostAuthentication::Password;
            }
            if (value.asString() == "identityFile")
            {
                return HostAuthentication::IdentityFile;
            }
            return std::nullopt;
        }

        const char* _forwardDirectionName(const ForwardDirection direction) noexcept
        {
            switch (direction)
            {
            case ForwardDirection::Local:
                return "local";
            case ForwardDirection::Remote:
                return "remote";
            case ForwardDirection::Dynamic:
                return "dynamic";
            }
            return "";
        }

        std::optional<ForwardDirection> _parseForwardDirection(const Json::Value& value)
        {
            if (value.asString() == "local")
            {
                return ForwardDirection::Local;
            }
            if (value.asString() == "remote")
            {
                return ForwardDirection::Remote;
            }
            if (value.asString() == "dynamic")
            {
                return ForwardDirection::Dynamic;
            }
            return std::nullopt;
        }

        std::wstring _bracketIpv6(const std::wstring_view address)
        {
            if (address.find(L':') == std::wstring_view::npos)
            {
                return std::wstring{ address };
            }
            return std::wstring{ L"[" }.append(address).append(L"]");
        }

        std::wstring _forwardSpecification(const PortForward& forward)
        {
            std::wstring result;
            if (!forward.listenHost.empty())
            {
                result.append(_bracketIpv6(forward.listenHost));
                result.push_back(L':');
            }
            result.append(std::to_wstring(forward.listenPort));
            if (forward.direction != ForwardDirection::Dynamic)
            {
                result.push_back(L':');
                result.append(_bracketIpv6(forward.destinationHost));
                result.push_back(L':');
                result.append(std::to_wstring(forward.destinationPort));
            }
            return result;
        }

        bool _isConfigAlias(const std::wstring_view alias) noexcept
        {
            return !alias.empty() && alias.size() <= MaxNameLength && alias.front() != L'-' &&
                   std::ranges::all_of(alias, _isSimpleNameChar) && alias.find_first_not_of(L'.') != std::wstring_view::npos;
        }

        bool _matchesHostPattern(const std::wstring_view alias, const std::wstring_view pattern) noexcept
        {
            std::size_t aliasIndex = 0;
            std::size_t patternIndex = 0;
            std::size_t star = std::wstring_view::npos;
            std::size_t retry = 0;
            while (aliasIndex < alias.size())
            {
                if (patternIndex < pattern.size() && pattern[patternIndex] == L'*')
                {
                    star = patternIndex++;
                    retry = aliasIndex;
                }
                else if (patternIndex < pattern.size() &&
                         (pattern[patternIndex] == L'?' || alias[aliasIndex] == pattern[patternIndex]))
                {
                    ++aliasIndex;
                    ++patternIndex;
                }
                else if (star != std::wstring_view::npos)
                {
                    patternIndex = star + 1;
                    aliasIndex = ++retry;
                }
                else
                {
                    return false;
                }
            }
            return pattern.substr(patternIndex).find_first_not_of(L'*') == std::wstring_view::npos;
        }

        std::vector<std::wstring> _configWords(std::wstring_view line)
        {
            constexpr std::wstring_view whitespace{ L" \t\r" };
            const auto start = line.find_first_not_of(whitespace);
            if (start == std::wstring_view::npos || line[start] == L'#')
            {
                return {};
            }
            line.remove_prefix(start);
            const auto keywordEnd = line.find_first_of(L" \t\r=");
            if (keywordEnd == std::wstring_view::npos)
            {
                return {};
            }

            std::vector<std::wstring> words;
            words.emplace_back(line.substr(0, keywordEnd));
            auto next = line.find_first_not_of(whitespace, keywordEnd);
            if (next != std::wstring_view::npos && line[next] == L'=')
            {
                next = line.find_first_not_of(whitespace, next + 1);
            }
            while (next != std::wstring_view::npos && line[next] != L'#')
            {
                line.remove_prefix(next);
                const auto quote = line.front();
                const auto quoted = quote == L'"' || quote == L'\'';
                auto tokenEnd = line.find_first_of(whitespace);
                if (quoted)
                {
                    const auto closing = line.find(quote, 1);
                    if (closing == std::wstring_view::npos)
                    {
                        return {};
                    }
                    tokenEnd = closing + 1;
                    if (tokenEnd < line.size() && whitespace.find(line[tokenEnd]) == std::wstring_view::npos)
                    {
                        return {};
                    }
                }
                words.emplace_back(quoted ? line.substr(1, tokenEnd - 2) : line.substr(0, tokenEnd));
                next = line.find_first_not_of(whitespace, tokenEnd);
            }
            return words;
        }

        bool _isUncPath(const std::filesystem::path& path)
        {
            const auto value = path.native();
            return value.starts_with(L"\\\\") || value.starts_with(L"//");
        }

        bool _isArchivePath(const std::filesystem::path& path)
        {
            auto extension = path.extension().wstring();
            std::ranges::transform(extension, extension.begin(), [](const wchar_t ch) { return towlower(ch); });
            return extension == L".zip" || extension == L".7z" || extension == L".rar" || extension == L".tar" ||
                   extension == L".gz" || extension == L".bz2" || extension == L".xz" || extension == L".tgz" ||
                   extension == L".tbz" || extension == L".tbz2" || extension == L".txz";
        }

        constexpr size_t MaxConfigIncludeDepth{ 8 };
        constexpr size_t MaxConfigFiles{ 128 };
        constexpr DWORD MaxConfigFileBytes{ 1024 * 1024 };

        winrt::hstring _readAliasConfig(const std::filesystem::path& path)
        {
            const wil::unique_hfile file{ CreateFileW(path.c_str(), GENERIC_READ,
                                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
            THROW_LAST_ERROR_IF(!file);
            LARGE_INTEGER size{};
            THROW_IF_WIN32_BOOL_FALSE(GetFileSizeEx(file.get(), &size));
            THROW_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), size.QuadPart > static_cast<int64_t>(MaxConfigFileBytes));
            std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
            DWORD read{};
            THROW_IF_WIN32_BOOL_FALSE(ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr));
            bytes.resize(read);
            return winrt::to_hstring(bytes);
        }

        void _appendAlias(const std::wstring_view alias, const std::vector<std::wstring>& patterns, std::vector<std::wstring>& aliases)
        {
            if (_isConfigAlias(alias) &&
                std::ranges::none_of(patterns, [&](const auto& pattern) {
                    return pattern.starts_with(L'!') && _matchesHostPattern(alias, std::wstring_view{ pattern }.substr(1));
                }) &&
                std::ranges::none_of(aliases, [&](const auto& value) { return value == alias; }))
            {
                aliases.emplace_back(alias);
            }
        }

        void _loadConfigAliases(const std::filesystem::path& configPath,
                                const std::filesystem::path& initialDirectory,
                                const size_t depth,
                                std::vector<std::filesystem::path>& visited,
                                std::vector<std::wstring>& aliases)
        {
            if (depth > MaxConfigIncludeDepth || visited.size() >= MaxConfigFiles)
            {
                return;
            }

            auto path = configPath.is_absolute() ? configPath : initialDirectory / configPath;
            if (_isUncPath(path) || _isArchivePath(path))
            {
                return;
            }
            std::error_code error;
            auto resolvedPath = std::filesystem::weakly_canonical(path, error);
            if (error)
            {
                error.clear();
                resolvedPath = std::filesystem::absolute(path, error).lexically_normal();
                if (error)
                {
                    return;
                }
            }
            if (_isUncPath(resolvedPath) || _isArchivePath(resolvedPath) ||
                std::ranges::any_of(visited, [&](const auto& previous) {
                    return til::equals_insensitive_ascii(previous.native(), resolvedPath.native());
                }))
            {
                return;
            }
            error.clear();
            if (!std::filesystem::is_regular_file(resolvedPath, error) || error)
            {
                return;
            }
            visited.emplace_back(resolvedPath);

            winrt::hstring content;
            try
            {
                content = _readAliasConfig(resolvedPath);
            }
            catch (...)
            {
                if (depth == 0)
                {
                    throw;
                }
                return;
            }
            auto remaining = std::wstring_view{ content };
            if (remaining.starts_with(L'\ufeff'))
            {
                remaining.remove_prefix(1);
            }
            bool unconditional = true;
            while (!remaining.empty())
            {
                const auto end = remaining.find(L'\n');
                const auto line = remaining.substr(0, end);
                remaining.remove_prefix(end == std::wstring_view::npos ? remaining.size() : end + 1);
                const auto words = _configWords(line);
                if (words.empty())
                {
                    continue;
                }
                if (til::equals_insensitive_ascii(words.front(), L"Match"))
                {
                    unconditional = false;
                    continue;
                }
                if (til::equals_insensitive_ascii(words.front(), L"Host"))
                {
                    unconditional = words.size() == 2 && words[1] == L"*";
                    for (size_t i = 1; i < words.size(); ++i)
                    {
                        _appendAlias(words[i], words, aliases);
                    }
                    continue;
                }
                if (!unconditional || !til::equals_insensitive_ascii(words.front(), L"Include"))
                {
                    continue;
                }
                for (size_t i = 1; i < words.size(); ++i)
                {
                    const std::filesystem::path includePath{ words[i] };
                    if (_isUncPath(includePath) || (includePath.has_root_name() && !includePath.is_absolute()))
                    {
                        continue;
                    }
                    auto resolvedInclude = includePath.is_absolute() ? includePath : initialDirectory / includePath;
                    const auto filename = resolvedInclude.filename().wstring();
                    const bool filenameGlob = filename.find_first_of(L"*?") != std::wstring::npos;
                    const auto parent = resolvedInclude.parent_path();
                    if (resolvedInclude.has_parent_path() &&
                        resolvedInclude.parent_path().wstring().find_first_of(L"*?") != std::wstring::npos)
                    {
                        continue;
                    }
                    std::vector<std::filesystem::path> matches;
                    if (filenameGlob)
                    {
                        if (_isUncPath(parent) || _isArchivePath(resolvedInclude))
                        {
                            continue;
                        }
                        error.clear();
                        for (std::filesystem::directory_iterator it{ parent, error }, end; !error && it != end; it.increment(error))
                        {
                            if (_matchesHostPattern(it->path().filename().wstring(), filename))
                            {
                                matches.emplace_back(it->path());
                            }
                        }
                        std::ranges::sort(matches, [](const auto& left, const auto& right) { return left.native() < right.native(); });
                    }
                    else
                    {
                        matches.emplace_back(std::move(resolvedInclude));
                    }
                    for (const auto& match : matches)
                    {
                        _loadConfigAliases(match, initialDirectory, depth + 1, visited, aliases);
                    }
                }
            }
        }

        wil::unique_handle _lockCatalog(std::filesystem::path path)
        {
            path += L".lock";
            wil::unique_handle lock{ CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
            THROW_LAST_ERROR_IF(!lock);
            return lock;
        }

        void _writeCatalog(const std::filesystem::path& path, const std::vector<SavedHost>& hosts)
        {
            Json::Value root{ Json::objectValue };
            root["version"] = 1;
            Json::Value entries{ Json::arrayValue };
            for (const auto& host : hosts)
            {
                Json::Value entry{ Json::objectValue };
                entry["id"] = winrt::to_string(host.id);
                entry["name"] = winrt::to_string(host.name);
                entry["group"] = winrt::to_string(host.group);
                entry["address"] = winrt::to_string(host.address);
                entry["port"] = host.port;
                entry["username"] = winrt::to_string(host.username);
                entry["identityFile"] = winrt::to_string(host.identityFile);
                if (host.usesOpenSshConfig)
                {
                    entry["usesOpenSshConfig"] = true;
                }
                else if (!host.jumpHostAlias.empty())
                {
                    entry["jumpHostAlias"] = winrt::to_string(host.jumpHostAlias);
                }
                if (!host.usesOpenSshConfig)
                {
                    entry["authentication"] = _authenticationName(host.authentication);
                    if (!host.forwards.empty())
                    {
                        Json::Value forwards{ Json::arrayValue };
                        for (const auto& forward : host.forwards)
                        {
                            Json::Value value{ Json::objectValue };
                            value["direction"] = _forwardDirectionName(forward.direction);
                            value["listenHost"] = winrt::to_string(forward.listenHost);
                            value["listenPort"] = forward.listenPort;
                            if (forward.direction != ForwardDirection::Dynamic)
                            {
                                value["destinationHost"] = winrt::to_string(forward.destinationHost);
                                value["destinationPort"] = forward.destinationPort;
                            }
                            forwards.append(std::move(value));
                        }
                        entry["forwards"] = std::move(forwards);
                    }
                }
                entries.append(std::move(entry));
            }
            root["hosts"] = std::move(entries);
            til::io::write_utf8_string_to_file_atomic(path, Json::writeString(Json::StreamWriterBuilder{}, root));
        }
    }

    uint32_t ParsePort(const std::wstring_view value) noexcept
    {
        if (value.empty() || value.size() > 5 || !std::ranges::all_of(value, _isAsciiDigit))
        {
            return 0;
        }

        uint32_t port = 0;
        for (const auto digit : value)
        {
            port = port * 10 + (digit - L'0');
        }
        return port;
    }

    InvalidHostField ValidateHost(const SavedHost& host)
    {
        if (!_isValidLabel(host.name))
        {
            return InvalidHostField::Name;
        }
        if (!_isValidGroup(host.group))
        {
            return InvalidHostField::Group;
        }
        if (!_isValidAddress(host.address))
        {
            return InvalidHostField::Address;
        }
        if (host.usesOpenSshConfig)
        {
            return !_isConfigAlias(host.address) ? InvalidHostField::Address :
                   (host.forwards.empty() ? InvalidHostField::None : InvalidHostField::Forwarding);
        }
        if (host.port == 0 || host.port > 65535)
        {
            return InvalidHostField::Port;
        }
        if (!host.username.empty() &&
            (host.username.size() > 120 || host.username.front() == L'-' || !std::ranges::all_of(host.username, _isSimpleNameChar)))
        {
            return InvalidHostField::Username;
        }
        if (!_isValidIdentity(host.identityFile))
        {
            return InvalidHostField::IdentityFile;
        }
        if (host.authentication != HostAuthentication::OpenSsh &&
            host.authentication != HostAuthentication::Password &&
            host.authentication != HostAuthentication::IdentityFile)
        {
            return InvalidHostField::IdentityFile;
        }
        if (host.authentication == HostAuthentication::IdentityFile && host.identityFile.empty())
        {
            return InvalidHostField::IdentityFile;
        }
        if (!host.jumpHostAlias.empty() &&
            (!_isConfigAlias(host.jumpHostAlias) || til::equals_insensitive_ascii(host.jumpHostAlias, host.address)))
        {
            return InvalidHostField::JumpHostAlias;
        }
        for (const auto& forward : host.forwards)
        {
            if (forward.direction != ForwardDirection::Local && forward.direction != ForwardDirection::Remote &&
                forward.direction != ForwardDirection::Dynamic)
            {
                return InvalidHostField::Forwarding;
            }
            if (forward.listenPort == 0 || forward.listenPort > 65535 ||
                (!forward.listenHost.empty() && forward.listenHost != L"*" && !_isValidAddress(forward.listenHost)))
            {
                return InvalidHostField::Forwarding;
            }
            if (forward.direction == ForwardDirection::Dynamic)
            {
                if (!forward.destinationHost.empty() || forward.destinationPort != 0)
                {
                    return InvalidHostField::Forwarding;
                }
            }
            else if (!_isValidAddress(forward.destinationHost) || forward.destinationPort == 0 || forward.destinationPort > 65535)
            {
                return InvalidHostField::Forwarding;
            }
        }
        return InvalidHostField::None;
    }

    static void _appendAuthentication(std::wstring& command, const SavedHost& host)
    {
        if (host.authentication == HostAuthentication::Password)
        {
            command.append(L" -o BatchMode=no -o PreferredAuthentications=keyboard-interactive,password"
                           L" -o KbdInteractiveAuthentication=yes -o PasswordAuthentication=yes -o NumberOfPasswordPrompts=3");
        }
        else if (host.authentication == HostAuthentication::IdentityFile)
        {
            command.append(L" -o BatchMode=no -o PreferredAuthentications=publickey -o PubkeyAuthentication=yes -o IdentitiesOnly=yes -i ");
            til::append_quoted_argument(command, host.identityFile);
        }
    }

    std::wstring BuildCommandline(const std::wstring_view executable, const SavedHost& host, const std::wstring_view nativeAgent)
    {
        THROW_HR_IF(E_INVALIDARG, executable.empty() || ValidateHost(host) != InvalidHostField::None);
        THROW_HR_IF(E_INVALIDARG, !nativeAgent.empty() && !std::ranges::all_of(nativeAgent, _isAsciiLetter));

        std::wstring command;
        til::append_quoted_argument(command, executable);
        if (!nativeAgent.empty())
        {
            command.append(L" -t");
        }
        if (!host.usesOpenSshConfig)
        {
            command.append(L" -p ");
            command.append(std::to_wstring(host.port));
            if (!host.username.empty())
            {
                command.append(L" -l ");
                til::append_quoted_argument(command, host.username);
            }
            _appendAuthentication(command, host);
        }
        if (nativeAgent.empty() && !host.usesOpenSshConfig && !host.forwards.empty())
        {
            command.append(L" -o ExitOnForwardFailure=yes");
            for (const auto& forward : host.forwards)
            {
                command.append(forward.direction == ForwardDirection::Local ? L" -L " :
                               forward.direction == ForwardDirection::Remote ? L" -R " : L" -D ");
                til::append_quoted_argument(command, _forwardSpecification(forward));
            }
        }
        if (!host.usesOpenSshConfig && !host.jumpHostAlias.empty())
        {
            command.append(L" -J ");
            til::append_quoted_argument(command, host.jumpHostAlias);
        }
        command.append(L" -- ");
        til::append_quoted_argument(command, host.address);
        if (!nativeAgent.empty())
        {
            command.push_back(L' ');
            til::append_quoted_argument(command, std::wstring{ L"sh -lc 'exec " }.append(nativeAgent).append(L"'"));
        }
        return command;
    }

    std::wstring BuildSftpCommandline(const std::wstring_view sftpExecutable,
                                      const std::wstring_view sshExecutable,
                                      const SavedHost& host)
    {
        THROW_HR_IF(E_INVALIDARG, sftpExecutable.empty() || sshExecutable.empty() || ValidateHost(host) != InvalidHostField::None);

        std::wstring command;
        til::append_quoted_argument(command, sftpExecutable);
        command.append(L" -S ");
        til::append_quoted_argument(command, sshExecutable);
        if (!host.usesOpenSshConfig)
        {
            command.append(L" -P ");
            command.append(std::to_wstring(host.port));
            if (!host.username.empty())
            {
                command.append(L" -o ");
                til::append_quoted_argument(command, std::wstring{ L"User=" }.append(host.username));
            }
            _appendAuthentication(command, host);
            if (!host.jumpHostAlias.empty())
            {
                command.append(L" -J ");
                til::append_quoted_argument(command, host.jumpHostAlias);
            }
        }
        command.append(L" -- ");
        til::append_quoted_argument(command, _bracketIpv6(host.address));
        return command;
    }

    void AppendWtaSshTarget(std::wstring& arguments, const SavedHost& host)
    {
        THROW_HR_IF(E_INVALIDARG, ValidateHost(host) != InvalidHostField::None);
        const auto append = [&](const std::wstring_view option, const std::wstring_view value) {
            arguments.append(option);
            til::append_quoted_argument(arguments, value);
        };
        append(L" --ssh-host ", host.address);
        if (host.usesOpenSshConfig)
        {
            return;
        }
        arguments.append(L" --ssh-port ").append(std::to_wstring(host.port));
        if (!host.username.empty())
        {
            append(L" --ssh-user ", host.username);
        }
        if (!host.jumpHostAlias.empty())
        {
            append(L" --ssh-jump-alias ", host.jumpHostAlias);
        }
        if (host.authentication == HostAuthentication::IdentityFile)
        {
            append(L" --ssh-identity-file ", host.identityFile);
        }
    }

    std::vector<std::wstring> LoadConfigAliases(const std::filesystem::path& configPath)
    {
        std::vector<std::wstring> aliases;
        std::error_code error;
        auto absolutePath = configPath.is_absolute() ? configPath : std::filesystem::absolute(configPath, error);
        if (error || _isUncPath(absolutePath) || _isArchivePath(absolutePath))
        {
            return aliases;
        }
        const auto initialDirectory = absolutePath.parent_path();
        std::vector<std::filesystem::path> visited;
        _loadConfigAliases(absolutePath, initialDirectory, 0, visited, aliases);
        return aliases;
    }

    std::vector<std::wstring> LoadConfigAliases()
    {
        std::vector<std::wstring> aliases;
        for (const auto configPath : { L"%UserProfile%\\.ssh\\config", L"%ProgramData%\\ssh\\ssh_config" })
        {
            const std::filesystem::path path{ wil::ExpandEnvironmentStringsW<std::wstring>(configPath) };
            for (auto& alias : LoadConfigAliases(path))
            {
                if (std::ranges::none_of(aliases, [&](const auto& existing) { return existing == alias; }))
                {
                    aliases.emplace_back(std::move(alias));
                }
            }
        }
        return aliases;
    }

    HostStore::HostStore(const std::filesystem::path& settingsDirectory) :
        _path{ settingsDirectory / L"ssh-hosts.json" }
    {
    }

    std::vector<SavedHost> HostStore::Load() const
    {
        auto content = til::io::read_file_as_utf8_string_if_exists(_path);
        if (content.empty())
        {
            if (!std::filesystem::exists(_path))
            {
                return {};
            }
            // A first save may have appeared after the initial open. Retry
            // before treating an existing empty file as malformed.
            content = til::io::read_file_as_utf8_string_if_exists(_path);
        }

        const std::unique_ptr<Json::CharReader> reader{ Json::CharReaderBuilder{}.newCharReader() };
        Json::Value root;
        std::string errors;
        THROW_HR_IF(WEB_E_INVALID_JSON_STRING,
                    !reader->parse(content.data(), content.data() + content.size(), &root, &errors) ||
                        !root.isObject() || !root["version"].isUInt() || root["version"].asUInt() != 1 ||
                        !root["hosts"].isArray());

        std::vector<SavedHost> hosts;
        std::unordered_set<std::wstring> ids;
        for (const auto& entry : root["hosts"])
        {
            THROW_HR_IF(WEB_E_INVALID_JSON_STRING, !entry.isObject() || !entry["port"].isUInt());
            const auto& configMode = entry["usesOpenSshConfig"];
            const auto& jumpHostAlias = entry["jumpHostAlias"];
            const auto& authentication = entry["authentication"];
            const auto& forwardsValue = entry["forwards"];
            THROW_HR_IF(WEB_E_INVALID_JSON_STRING, !configMode.isNull() && !configMode.isBool());
            THROW_HR_IF(WEB_E_INVALID_JSON_STRING, !jumpHostAlias.isNull() && !jumpHostAlias.isString());
            THROW_HR_IF(WEB_E_INVALID_JSON_STRING, !authentication.isNull() && !authentication.isString());
            THROW_HR_IF(WEB_E_INVALID_JSON_STRING, !forwardsValue.isNull() && !forwardsValue.isArray());
            const bool usesOpenSshConfig = configMode.isBool() && configMode.asBool();
            const auto identityFile = _readString(entry, "identityFile");
            auto authenticationMode = HostAuthentication::OpenSsh;
            if (!authentication.isNull())
            {
                const auto parsed = _parseAuthentication(authentication);
                THROW_HR_IF(WEB_E_INVALID_JSON_STRING, !parsed.has_value());
                authenticationMode = *parsed;
            }
            else if (!usesOpenSshConfig)
            {
                // Preserve the behavior of catalogs written before the explicit
                // authentication mode existed.
                authenticationMode = identityFile.empty() ? HostAuthentication::Password : HostAuthentication::IdentityFile;
            }
            std::vector<PortForward> forwards;
            if (forwardsValue.isArray())
            {
                for (const auto& value : forwardsValue)
                {
                    THROW_HR_IF(WEB_E_INVALID_JSON_STRING,
                                !value.isObject() || !value["direction"].isString() || !value["listenPort"].isUInt());
                    const auto direction = _parseForwardDirection(value["direction"]);
                    THROW_HR_IF(WEB_E_INVALID_JSON_STRING, !direction.has_value());
                    const auto& listenHost = value["listenHost"];
                    THROW_HR_IF(WEB_E_INVALID_JSON_STRING, !listenHost.isNull() && !listenHost.isString());
                    PortForward forward{ *direction,
                                         listenHost.isString() ? std::wstring{ winrt::to_hstring(listenHost.asString()) } : std::wstring{},
                                         value["listenPort"].asUInt(),
                                         {},
                                         0 };
                    if (*direction == ForwardDirection::Dynamic)
                    {
                        THROW_HR_IF(WEB_E_INVALID_JSON_STRING, value.isMember("destinationHost") || value.isMember("destinationPort"));
                    }
                    else
                    {
                        THROW_HR_IF(WEB_E_INVALID_JSON_STRING,
                                    !value["destinationHost"].isString() || !value["destinationPort"].isUInt());
                        forward.destinationHost = std::wstring{ winrt::to_hstring(value["destinationHost"].asString()) };
                        forward.destinationPort = value["destinationPort"].asUInt();
                    }
                    forwards.emplace_back(std::move(forward));
                }
            }
            SavedHost host{
                _readString(entry, "id"),
                _readString(entry, "name"),
                _readString(entry, "group"),
                _readString(entry, "address"),
                entry["port"].asUInt(),
                _readString(entry, "username"),
                identityFile,
                usesOpenSshConfig,
                jumpHostAlias.isString() ? std::wstring{ winrt::to_hstring(jumpHostAlias.asString()) } : std::wstring{},
                authenticationMode,
                std::move(forwards),
            };
            THROW_HR_IF(WEB_E_INVALID_JSON_STRING,
                        host.id.empty() || !ids.insert(host.id).second || ValidateHost(host) != InvalidHostField::None);
            hosts.emplace_back(std::move(host));
        }
        return hosts;
    }

    std::size_t HostStore::ImportAliases(const std::vector<std::wstring>& aliases) const
    {
        const auto lock = _lockCatalog(_path);
        auto hosts = Load();
        const auto originalSize = hosts.size();
        for (const auto& alias : aliases)
        {
            if (!_isConfigAlias(alias) || std::ranges::any_of(hosts, [&](const auto& host) {
                    return host.usesOpenSshConfig && host.address == alias;
                }))
            {
                continue;
            }
            GUID id{};
            THROW_IF_FAILED(CoCreateGuid(&id));
            hosts.emplace_back(SavedHost{ ::Microsoft::Console::Utils::GuidToString(id), alias, L"OpenSSH", alias, 22, {}, {}, true, {} });
        }
        if (hosts.size() != originalSize)
        {
            _writeCatalog(_path, hosts);
        }
        return hosts.size() - originalSize;
    }

    std::optional<std::vector<SavedHost>> HostStore::Replace(const std::optional<SavedHost>& original,
                                                            const std::optional<SavedHost>& replacement) const
    {
        THROW_HR_IF(E_INVALIDARG, !original && !replacement);
        if (replacement)
        {
            THROW_HR_IF(E_INVALIDARG, replacement->id.empty() ||
                                          (original && original->id != replacement->id) ||
                                          ValidateHost(*replacement) != InvalidHostField::None);
        }

        const auto lock = _lockCatalog(_path);
        auto hosts = Load();
        const auto& id = original ? original->id : replacement->id;
        const auto existing = std::ranges::find(hosts, id, &SavedHost::id);
        if (original)
        {
            if (existing == hosts.end() || *existing != *original)
            {
                return std::nullopt;
            }
            if (replacement)
            {
                *existing = *replacement;
            }
            else
            {
                hosts.erase(existing);
            }
        }
        else
        {
            if (existing != hosts.end())
            {
                return std::nullopt;
            }
            hosts.push_back(*replacement);
        }

        _writeCatalog(_path, hosts);
        return hosts;
    }
}
