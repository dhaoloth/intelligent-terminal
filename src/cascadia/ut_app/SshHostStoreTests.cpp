// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "../TerminalApp/SshHostStore.h"
#include "../../types/inc/utils.hpp"

#include <fstream>

using namespace WEX::TestExecution;
using namespace Microsoft::Terminal::OpenSsh;

namespace TerminalAppUnitTests
{
    class SshHostStoreTests
    {
        TEST_CLASS(SshHostStoreTests);
        TEST_METHOD(SelectsAuthenticationAndJumpHost);
        TEST_METHOD(AddsForwardsToShellOnly);
        TEST_METHOD(MigratesLegacyAuthentication);
        TEST_METHOD(FindsAliasesInIncludedConfigs);
    };

    static SavedHost makeHost()
    {
        SavedHost host;
        host.id = L"fixture";
        host.name = L"Fixture";
        host.address = L"example.test";
        host.jumpHostAlias = L"bastion";
        return host;
    }

    static std::vector<std::wstring> parseArguments(const std::wstring& command)
    {
        int count{};
        const wil::unique_hlocal_ptr<PWSTR[]> arguments{ CommandLineToArgvW(command.c_str(), &count) };
        THROW_LAST_ERROR_IF_NULL(arguments.get());
        return { arguments.get(), arguments.get() + count };
    }

    void SshHostStoreTests::SelectsAuthenticationAndJumpHost()
    {
        auto host = makeHost();
        const std::vector<std::wstring> openSshExpected{ L"ssh.exe", L"-p", L"22", L"-J", L"bastion", L"--", L"example.test" };
        VERIFY_IS_TRUE(parseArguments(BuildCommandline(L"ssh.exe", host)) == openSshExpected);

        host.authentication = HostAuthentication::Password;
        const auto password = parseArguments(BuildCommandline(L"ssh.exe", host));
        VERIFY_IS_TRUE(std::ranges::find(password, L"PreferredAuthentications=keyboard-interactive,password") != password.end());

        host.authentication = HostAuthentication::IdentityFile;
        VERIFY_ARE_EQUAL(InvalidHostField::IdentityFile, ValidateHost(host));
        host.identityFile = LR"(C:\Key files\fixture)";
        const auto key = parseArguments(BuildCommandline(L"ssh.exe", host));
        const std::vector<std::wstring> identityOption{ L"-i", host.identityFile };
        VERIFY_IS_TRUE(std::ranges::find(key, L"IdentitiesOnly=yes") != key.end());
        VERIFY_IS_FALSE(std::ranges::search(key, identityOption).empty());
    }

    void SshHostStoreTests::AddsForwardsToShellOnly()
    {
        auto host = makeHost();
        host.forwards = {
            { ForwardDirection::Local, L"::1", 10022, L"2001:db8::10", 22 },
            { ForwardDirection::Remote, L"", 18080, L"localhost", 8080 },
            { ForwardDirection::Dynamic, L"127.0.0.1", 1080, L"", 0 },
        };
        const std::vector<std::wstring> shellExpected{
            L"ssh.exe", L"-p", L"22", L"-o", L"ExitOnForwardFailure=yes",
            L"-L", L"[::1]:10022:[2001:db8::10]:22", L"-R", L"18080:localhost:8080",
            L"-D", L"127.0.0.1:1080", L"-J", L"bastion", L"--", L"example.test"
        };
        VERIFY_IS_TRUE(parseArguments(BuildCommandline(L"ssh.exe", host)) == shellExpected);

        const std::vector<std::wstring> agentExpected{
            L"ssh.exe", L"-t", L"-p", L"22", L"-J", L"bastion", L"--", L"example.test", L"sh -lc 'exec codex'"
        };
        VERIFY_IS_TRUE(parseArguments(BuildCommandline(L"ssh.exe", host, L"codex")) == agentExpected);

        constexpr std::wstring_view sshPath{ LR"(C:\OpenSSH Bin\ssh.exe)" };
        const std::vector<std::wstring> sftpExpected{
            L"sftp.exe", L"-S", std::wstring{ sshPath }, L"-P", L"22", L"-J", L"bastion", L"--", L"example.test"
        };
        VERIFY_IS_TRUE(parseArguments(BuildSftpCommandline(L"sftp.exe", sshPath, host)) == sftpExpected);
        host.address = L"2001:db8::a";
        const auto ipv6Arguments = parseArguments(BuildSftpCommandline(L"sftp.exe", L"ssh.exe", host));
        VERIFY_ARE_EQUAL(std::wstring{ L"[2001:db8::a]" }, ipv6Arguments.back());

        host.forwards.front().listenHost = L"localhost -o ProxyCommand=invalid";
        VERIFY_ARE_EQUAL(InvalidHostField::Forwarding, ValidateHost(host));
        host.forwards.front().listenHost = L"::1";
        host.forwards.back().destinationPort = 443;
        VERIFY_ARE_EQUAL(InvalidHostField::Forwarding, ValidateHost(host));
    }

    void SshHostStoreTests::MigratesLegacyAuthentication()
    {
        const auto guid = ::Microsoft::Console::Utils::GuidToString(::Microsoft::Console::Utils::CreateGuid());
        const auto directory = std::filesystem::temp_directory_path() / guid.c_str();
        VERIFY_IS_TRUE(std::filesystem::create_directory(directory));
        const auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        });
        {
            std::ofstream fixture{ directory / L"ssh-hosts.json" };
            fixture << R"({"version":1,"hosts":[
                {"id":"one","name":"Password","group":"","address":"one.test","port":22,"username":"","identityFile":""},
                {"id":"two","name":"Key","group":"","address":"two.test","port":22,"username":"","identityFile":"C:\\ssh\\fixture"}
            ]})";
            VERIFY_IS_TRUE(fixture.good());
        }
        const HostStore store{ directory };
        const auto hosts = store.Load();
        VERIFY_ARE_EQUAL(size_t{ 2 }, hosts.size());
        VERIFY_ARE_EQUAL(HostAuthentication::Password, hosts.at(0).authentication);
        VERIFY_ARE_EQUAL(HostAuthentication::IdentityFile, hosts.at(1).authentication);

        auto updated = hosts.at(0);
        updated.authentication = HostAuthentication::OpenSsh;
        updated.forwards = { { ForwardDirection::Dynamic, L"", 1080, L"", 0 } };
        VERIFY_IS_TRUE(store.Replace(hosts.at(0), updated).has_value());
        const auto reloaded = store.Load();
        VERIFY_IS_TRUE(reloaded.at(0) == updated);
    }

    void SshHostStoreTests::FindsAliasesInIncludedConfigs()
    {
        const auto guid = ::Microsoft::Console::Utils::GuidToString(::Microsoft::Console::Utils::CreateGuid());
        const auto directory = std::filesystem::temp_directory_path() / guid.c_str();
        const auto includes = directory / L"config.d";
        VERIFY_IS_TRUE(std::filesystem::create_directories(includes));
        const auto cleanup = wil::scope_exit([&] {
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        });
        const auto configFile = directory / L"config";
        std::ofstream{ configFile } << "Include config.d/*.conf optional\nHost=first\nInclude conditional\nHost foo#bar next=value\nHost last\n";
        std::ofstream{ includes / L"20.conf" } << "Host 'second' !skip* skip-second\n";
        std::ofstream{ includes / L"10.conf" } << "Include config\nHost included\n";
        std::ofstream{ directory / L"conditional" } << "Host unrelated\n";
        const std::vector<std::wstring> expected{ L"included", L"second", L"first", L"last" };
        VERIFY_IS_TRUE(LoadConfigAliases(configFile) == expected);
    }
}
