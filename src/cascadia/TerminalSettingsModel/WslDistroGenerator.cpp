// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "WslDistroGenerator.h"
#include "LegacyProfileGeneratorNamespaces.h"
#include "../../inc/DefaultSettings.h"

#include "DynamicProfileUtils.h"
#include "../inc/WslDistributions.h"

static constexpr std::wstring_view WslHomeDirectory{ L"~" };
static constexpr std::wstring_view IconPath{ L"ms-appx:///ProfileIcons/{9acb9455-ca41-5af7-950f-6bca1bc9722f}.png" };
static constexpr std::wstring_view GeneratorIconPath{ L"ms-appx:///ProfileGeneratorIcons/WSL.png" };

using namespace ::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::Settings::Model;

static bool isWslDashDashCdAvailableForLinuxPaths() noexcept
{
    OSVERSIONINFOEXW osver{};
    osver.dwOSVersionInfoSize = sizeof(osver);
    osver.dwBuildNumber = 19041;

    DWORDLONG dwlConditionMask = 0;
    VER_SET_CONDITION(dwlConditionMask, VER_BUILDNUMBER, VER_GREATER_EQUAL);

    return VerifyVersionInfoW(&osver, VER_BUILDNUMBER, dwlConditionMask) != FALSE;
}

// Legacy GUIDs:
//   - Debian       58ad8b0c-3ef8-5f4d-bc6f-13e4c00f2530
//   - Ubuntu       2c4de342-38b7-51cf-b940-2309a097f518
//   - Alpine       1777cdf0-b2c4-5a63-a204-eb60f349ea7c
//   - Ubuntu-18.04 c6eaf9f4-32a7-5fdc-b5cf-066e8a4b1e40

std::wstring_view WslDistroGenerator::GetNamespace() const noexcept
{
    return WslGeneratorNamespace;
}

std::wstring_view WslDistroGenerator::GetDisplayName() const noexcept
{
    return RS_(L"WslDistroGeneratorDisplayName");
}

std::wstring_view WslDistroGenerator::GetIcon() const noexcept
{
    return GeneratorIconPath;
}

static winrt::com_ptr<implementation::Profile> makeProfile(const std::wstring& distName)
{
    const auto WSLDistro{ CreateDynamicProfile(distName) };
    // GH#11096 - make sure the WSL path starts explicitly with
    // C:\Windows\System32. Don't want someone path hijacking wsl.exe.
    std::wstring command{};
    THROW_IF_FAILED(wil::GetSystemDirectoryW<std::wstring>(command));
    WSLDistro->Commandline(winrt::hstring{ command + L"\\wsl.exe -d " + distName });
    WSLDistro->DefaultAppearance().DarkColorSchemeName(L"Campbell");
    WSLDistro->DefaultAppearance().LightColorSchemeName(L"Campbell");
    if (isWslDashDashCdAvailableForLinuxPaths())
    {
        WSLDistro->StartingDirectory(winrt::hstring{ WslHomeDirectory });
    }
    else
    {
        WSLDistro->StartingDirectory(winrt::hstring{ DEFAULT_STARTING_DIRECTORY });
    }
    WSLDistro->Icon(winrt::hstring{ IconPath });
    WSLDistro->PathTranslationStyle(winrt::Microsoft::Terminal::Control::PathTranslationStyle::WSL);
    return WSLDistro;
}

// Method Description:
// - Generate a list of profiles for each on the installed WSL distros. This
//   will first try to read the installed distros from the registry. If that
//   fails, we'll assume there are no WSL distributions installed.
//   Reading the registry is slightly more stable (see GH#7199, GH#9905),
//   but it is certainly BODGY
// Arguments:
// - <none>
// Return Value:
// - A list of WSL profiles.
void WslDistroGenerator::GenerateProfiles(std::vector<winrt::com_ptr<implementation::Profile>>& profiles) const
{
    for (const auto& distName : ::Microsoft::Terminal::Wsl::EnumerateDistributions())
    {
        profiles.emplace_back(makeProfile(distName));
    }
}
