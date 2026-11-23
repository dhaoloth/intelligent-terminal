// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SshHostStore.h"

#include "../../types/inc/utils.hpp"
#include <json/json.h>
#include <til/commandline.h>
#include <til/io.h>
#include <ws2tcpip.h>

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

        bool _isConfigAlias(const std::wstring_view alias) noexcept
        {
            return !alias.empty() && alias.size() <= MaxNameLength && alias.front() != L'-' &&
                   std::ranges::all_of(alias, _isSimpleNameChar) && alias.find_first_not_of(L'.') != std::wstring_view::npos;
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
            return _isConfigAlias(host.address) ? InvalidHostField::None : InvalidHostField::Address;
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
        return InvalidHostField::None;
    }

    std::wstring BuildCommandline(const std::wstring_view executable, const SavedHost& host)
    {
        THROW_HR_IF(E_INVALIDARG, executable.empty() || ValidateHost(host) != InvalidHostField::None);

        std::wstring command;
        til::append_quoted_argument(command, executable);
        if (host.usesOpenSshConfig)
        {
            command.append(L" -- ");
            til::append_quoted_argument(command, host.address);
            return command;
        }
        command.append(L" -o BatchMode=no -p ");
        command.append(std::to_wstring(host.port));
        if (!host.username.empty())
        {
            command.append(L" -l ");
            til::append_quoted_argument(command, host.username);
        }
        if (host.identityFile.empty())
        {
            command.append(L" -o PreferredAuthentications=keyboard-interactive,password"
                           L" -o KbdInteractiveAuthentication=yes -o PasswordAuthentication=yes -o NumberOfPasswordPrompts=3");
        }
        else
        {
            command.append(L" -o PreferredAuthentications=publickey -o PubkeyAuthentication=yes -o IdentitiesOnly=yes -i ");
            til::append_quoted_argument(command, host.identityFile);
        }
        command.append(L" -- ");
        til::append_quoted_argument(command, host.address);
        return command;
    }

    std::vector<std::wstring> LoadConfigAliases()
    {
        std::vector<std::wstring> aliases;
        for (const auto configPath : { L"%UserProfile%\\.ssh\\config", L"%ProgramData%\\ssh\\ssh_config" })
        {
            const std::filesystem::path path{ wil::ExpandEnvironmentStringsW<std::wstring>(configPath) };
            if (!std::filesystem::exists(path))
            {
                continue;
            }

            const auto content = winrt::to_hstring(til::io::read_file_as_utf8_string_if_exists(path));
            auto remaining = std::wstring_view{ content };
            if (remaining.starts_with(L'\ufeff'))
            {
                remaining.remove_prefix(1);
            }
            while (!remaining.empty())
            {
                const auto end = remaining.find(L'\n');
                auto line = remaining.substr(0, end);
                remaining.remove_prefix(end == std::wstring_view::npos ? remaining.size() : end + 1);
                const auto first = line.find_first_not_of(L" \t\r");
                if (first == std::wstring_view::npos)
                {
                    continue;
                }
                line.remove_prefix(first);
                const auto separator = line.find_first_of(L" \t=");
                if (separator == std::wstring_view::npos || !til::equals_insensitive_ascii(line.substr(0, separator), L"Host"))
                {
                    continue;
                }
                line.remove_prefix(separator);
                const auto valueStart = line.find_first_not_of(L" \t=");
                if (valueStart == std::wstring_view::npos)
                {
                    continue;
                }
                line.remove_prefix(valueStart);
                while (!line.empty() && !line.starts_with(L'#'))
                {
                    const auto whitespace = L" \t\r";
                    const auto quoted = line.starts_with(L'"');
                    auto tokenEnd = line.find_first_of(whitespace);
                    if (quoted)
                    {
                        const auto closing = line.find(L'"', 1);
                        if (closing == std::wstring_view::npos)
                        {
                            break;
                        }
                        tokenEnd = closing + 1;
                        if (tokenEnd < line.size() && std::wstring_view{ whitespace }.find(line[tokenEnd]) == std::wstring_view::npos)
                        {
                            break;
                        }
                    }
                    const auto alias = quoted ? line.substr(1, tokenEnd - 2) : line.substr(0, tokenEnd);
                    if (_isConfigAlias(alias) && std::ranges::none_of(aliases, [&](const auto& value) {
                            return til::equals_insensitive_ascii(value, alias);
                        }))
                    {
                        aliases.emplace_back(alias);
                    }
                    const auto next = tokenEnd == std::wstring_view::npos ? tokenEnd : line.find_first_not_of(whitespace, tokenEnd);
                    if (next == std::wstring_view::npos)
                    {
                        break;
                    }
                    line.remove_prefix(next);
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
            THROW_HR_IF(WEB_E_INVALID_JSON_STRING, !configMode.isNull() && !configMode.isBool());
            SavedHost host{
                _readString(entry, "id"),
                _readString(entry, "name"),
                _readString(entry, "group"),
                _readString(entry, "address"),
                entry["port"].asUInt(),
                _readString(entry, "username"),
                _readString(entry, "identityFile"),
                configMode.isBool() && configMode.asBool(),
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
                    return host.usesOpenSshConfig && til::equals_insensitive_ascii(host.address, alias);
                }))
            {
                continue;
            }
            GUID id{};
            THROW_IF_FAILED(CoCreateGuid(&id));
            hosts.emplace_back(SavedHost{ ::Microsoft::Console::Utils::GuidToString(id), alias, L"OpenSSH", alias, 22, {}, {}, true });
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
