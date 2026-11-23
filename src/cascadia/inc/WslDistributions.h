// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/narrow>
#include <wil/registry.h>

#include "til.h"

namespace Microsoft::Terminal::Wsl
{
    // Enumerates registered, user-facing WSL distributions without starting WSL.
    // Utility distributions used by Docker and Rancher are intentionally omitted,
    // matching the dynamic profile generator's behavior.
    inline std::vector<std::wstring> EnumerateDistributions()
    {
        static constexpr wchar_t RegKeyLxss[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss";
        static constexpr wchar_t RegKeyDistroName[] = L"DistributionName";
        static constexpr wchar_t RegKeyModern[] = L"Modern";
        static constexpr std::wstring_view DockerDistributionPrefix{ L"docker-desktop" };
        static constexpr std::wstring_view RancherDistributionPrefix{ L"rancher-desktop" };

        HKEY rawRootKey{ nullptr };
        if (RegOpenKeyEx(HKEY_CURRENT_USER, RegKeyLxss, 0, KEY_READ, &rawRootKey) != ERROR_SUCCESS)
        {
            return {};
        }
        wil::unique_hkey rootKey{ rawRootKey };

        std::vector<std::wstring> names;
        wchar_t guid[39]; // A {GUID} is 38 characters long.
        for (DWORD index = 0;; ++index)
        {
            DWORD length = 39;
            const auto result = RegEnumKeyEx(rootKey.get(), index, guid, &length, nullptr, nullptr, nullptr, nullptr);
            if (result == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            if (result != ERROR_SUCCESS && result != ERROR_MORE_DATA)
            {
                break;
            }
            if (result != ERROR_SUCCESS || length != 38 || guid[0] != L'{' || guid[37] != L'}')
            {
                continue;
            }

            HKEY rawDistroKey{ nullptr };
            if (RegOpenKeyEx(rootKey.get(), guid, 0, KEY_READ, &rawDistroKey) != ERROR_SUCCESS)
            {
                continue;
            }
            wil::unique_hkey distroKey{ rawDistroKey };

            const auto modernValue = wil::reg::try_get_value<uint32_t>(distroKey.get(), RegKeyModern);
            if (modernValue.value_or(0u) == 1u)
            {
                continue;
            }

            std::wstring name;
            const auto status = wil::AdaptFixedSizeToAllocatedResult<std::wstring, 256>(name, [&](PWSTR value, size_t valueLength, size_t* valueLengthNeededWithNull) -> HRESULT {
                auto byteLength = gsl::narrow<DWORD>(valueLength * sizeof(wchar_t));
                const auto queryStatus = RegQueryValueExW(distroKey.get(), RegKeyDistroName, 0, nullptr, reinterpret_cast<BYTE*>(value), &byteLength);
                *valueLengthNeededWithNull = (byteLength + sizeof(wchar_t) - 1) / sizeof(wchar_t);
                return queryStatus == ERROR_MORE_DATA ? S_OK : HRESULT_FROM_WIN32(queryStatus);
            });
            if (status != S_OK)
            {
                continue;
            }

            if (til::starts_with(name, DockerDistributionPrefix) || til::starts_with(name, RancherDistributionPrefix))
            {
                continue;
            }
            names.emplace_back(std::move(name));
        }
        return names;
    }
}
