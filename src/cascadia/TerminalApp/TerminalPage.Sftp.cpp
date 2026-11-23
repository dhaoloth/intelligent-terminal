// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalPage.h"

#include <LibraryResources.h>
#include <json/json.h>
#include <til/commandline.h>

#include "../inc/WtaProcess.h"
#include "../WinRTUtils/inc/Utils.h"
#include "../../types/inc/utils.hpp"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace OpenSsh = ::Microsoft::Terminal::OpenSsh;

namespace
{
    struct RemoteEntry
    {
        hstring name;
        bool directory{};
        std::optional<uint64_t> size;
    };

    struct RemoteDirectory
    {
        hstring path;
        std::vector<RemoteEntry> entries;
        hstring error;
        bool truncated{};
    };

    struct FileTransfer
    {
        std::atomic<bool> cancelled{ false };

        void Cancel() noexcept
        {
            if (!_finalizing.exchange(true))
            {
                cancelled.store(true, std::memory_order_relaxed);
            }
        }

        bool BeginPublication() noexcept
        {
            return !_finalizing.exchange(true);
        }

    private:
        std::atomic<bool> _finalizing{ false };
    };

    hstring _sftpError(const std::string_view status, const hstring& fallback)
    {
        if (status == "auth_error")
        {
            return RS_(L"SftpBrowseAuthenticationRequired");
        }
        if (status == "permission_denied")
        {
            return RS_(L"SftpPermissionDenied");
        }
        if (status == "timeout")
        {
            return RS_(L"SftpTimedOut");
        }
        return fallback;
    }

    RemoteDirectory _readDirectoryResult(const ::Microsoft::Terminal::WtaProcess::CaptureResult& capture)
    {
        RemoteDirectory result;
        if (capture.timedOut)
        {
            result.error = RS_(L"SftpTimedOut");
            return result;
        }

        const std::unique_ptr<Json::CharReader> reader{ Json::CharReaderBuilder{}.newCharReader() };
        Json::Value root;
        std::string errors;
        if (!capture.completed || capture.exitCode != 0 ||
            !reader->parse(capture.output.data(), capture.output.data() + capture.output.size(), &root, &errors) ||
            !root.isObject() || !root["status"].isString())
        {
            result.error = RS_(L"SftpBrowseFailed");
            return result;
        }
        const auto status = root["status"].asString();
        if (status != "ok" || !root["path"].isString() || !root["entries"].isArray())
        {
            result.error = _sftpError(status, RS_(L"SftpBrowseFailed"));
        }
        else
        {
            result.path = winrt::to_hstring(root["path"].asString());
            result.truncated = root["truncated"].isBool() && root["truncated"].asBool();
            for (const auto& item : root["entries"])
            {
                if (!item.isObject() || !item["name"].isString() || !item["directory"].isBool())
                {
                    continue;
                }
                const auto name = winrt::to_hstring(item["name"].asString());
                const auto nameView = std::wstring_view{ name };
                if (name.empty() || name == L"." || name == L".." || nameView.find_first_of(L"/\\") != std::wstring_view::npos ||
                    std::ranges::any_of(nameView, [](const wchar_t ch) { return ch < L' ' || ch == 0x7f; }))
                {
                    continue;
                }
                const auto& size = item["size"];
                result.entries.push_back({ name, item["directory"].asBool(), size.isUInt64() ? std::make_optional(size.asUInt64()) : std::nullopt });
            }
        }
        return result;
    }

    safe_void_coroutine _listRemoteDirectory(winrt::Windows::UI::Core::CoreDispatcher dispatcher,
                                             std::wstring wta,
                                             std::wstring arguments,
                                             std::shared_ptr<std::atomic<bool>> cancelled,
                                             std::function<void(RemoteDirectory)> completed)
    {
        co_await winrt::resume_background();
        RemoteDirectory result;
        try
        {
            result = _readDirectoryResult(::Microsoft::Terminal::WtaProcess::RunWtaCapture(
                wta, arguments, 26000, nullptr, false, cancelled.get(), true));
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            result.error = RS_(L"SftpBrowseFailed");
        }
        co_await wil::resume_foreground(dispatcher);
        if (!cancelled->load(std::memory_order_relaxed))
        {
            completed(std::move(result));
        }
    }

    hstring _saveRemoteFile(const std::wstring& wta, std::wstring arguments,
                            const std::filesystem::path& destination, FileTransfer& transfer)
    {
        const auto fallback = RS_(L"SftpDownloadFailed");
        const auto identifier = ::Microsoft::Console::Utils::GuidToString(::Microsoft::Console::Utils::CreateGuid());
        const auto temporaryFolder = destination.parent_path() / (std::wstring{ L".intelligent-terminal-" } + identifier.c_str());
        const auto temporaryFile = temporaryFolder / L"download";
        if (!std::filesystem::create_directory(temporaryFolder))
        {
            return fallback;
        }
        const auto cleanup = wil::scope_exit([&] {
            std::error_code ignored;
            for (int attempt = 0; attempt < 5; ++attempt)
            {
                std::filesystem::remove(temporaryFile, ignored);
                if (!ignored)
                {
                    break;
                }
                Sleep(100);
            }
            std::filesystem::remove(temporaryFolder, ignored);
        });

        arguments.push_back(L' ');
        til::append_quoted_argument(arguments, std::wstring{ L"--local-path=" }.append(temporaryFile.native()));
        constexpr DWORD transferWatchdogMs{ (6 * 60 * 60 + 45) * 1000 };
        const auto capture = ::Microsoft::Terminal::WtaProcess::RunWtaCapture(
            wta, arguments, transferWatchdogMs, nullptr, false, &transfer.cancelled, true);
        if (transfer.cancelled.load(std::memory_order_relaxed))
        {
            return {};
        }
        if (capture.timedOut)
        {
            return RS_(L"SftpTimedOut");
        }

        const std::unique_ptr<Json::CharReader> reader{ Json::CharReaderBuilder{}.newCharReader() };
        Json::Value result;
        std::string errors;
        if (!capture.completed || capture.exitCode != 0 ||
            !reader->parse(capture.output.data(), capture.output.data() + capture.output.size(), &result, &errors) ||
            !result.isObject() || !result["status"].isString())
        {
            return fallback;
        }
        const auto status = result["status"].asString();
        if (status != "ok" || !result["bytes"].isUInt64())
        {
            return _sftpError(status, fallback);
        }
        std::error_code sizeError;
        if (std::filesystem::file_size(temporaryFile, sizeError) != result["bytes"].asUInt64() || sizeError)
        {
            return fallback;
        }
        if (!transfer.BeginPublication())
        {
            return {};
        }
        if (!MoveFileW(temporaryFile.c_str(), destination.c_str()))
        {
            const auto error = GetLastError();
            return error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ? RS_(L"SftpDownloadFileExists") : fallback;
        }
        return RS_(L"SftpDownloadComplete");
    }

    safe_void_coroutine _downloadRemoteFile(winrt::Windows::UI::Core::CoreDispatcher dispatcher,
                                            std::wstring wta,
                                            std::wstring arguments,
                                            std::filesystem::path destination,
                                            std::shared_ptr<FileTransfer> transfer,
                                            std::function<void(hstring)> completed)
    {
        co_await winrt::resume_background();
        hstring status;
        try
        {
            status = _saveRemoteFile(wta, std::move(arguments), destination, *transfer);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            status = RS_(L"SftpDownloadFailed");
        }
        co_await wil::resume_foreground(dispatcher);
        if (!transfer->cancelled.load(std::memory_order_relaxed))
        {
            completed(status);
        }
    }

    struct SftpBrowser : std::enable_shared_from_this<SftpBrowser>
    {
        SftpBrowser(const OpenSsh::SavedHost& host, std::wstring wta, winrt::Windows::UI::Core::CoreDispatcher dispatcher, HWND owner) :
            _wta{ std::move(wta) },
            _dispatcher{ std::move(dispatcher) },
            _owner{ owner }
        {
            OpenSsh::AppendWtaSshTarget(_targetArguments, host);
            Dialog.Title(box_value(hstring{ RS_fmt(L"SftpBrowseTitle", std::wstring_view{ host.name }) }));
            Dialog.SecondaryButtonText(RS_(L"SshHostsOpenSftp"));
            Dialog.CloseButtonText(RS_(L"SshHostsClose"));
        }

        ~SftpBrowser()
        {
            Close();
        }

        void Close() noexcept
        {
            _closed = true;
            if (_pending)
            {
                _pending->store(true, std::memory_order_relaxed);
            }
            if (_transfer)
            {
                _transfer->Cancel();
            }
        }

        ContentDialog Dialog;

        void Initialize()
        {
            const auto weak = weak_from_this();
            _path.Header(box_value(RS_(L"SftpRemotePath")));
            _path.Text(L".");
            Button open;
            open.Content(box_value(RS_(L"SftpOpenDirectory")));
            open.Click([weak](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    self->_navigate(self->_path.Text());
                }
            });
            _up.Content(box_value(RS_(L"SftpParentDirectory")));
            _up.IsEnabled(false);
            _up.Click([weak](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    self->_navigate(self->_directoryPath(L".."));
                }
            });
            _download.Content(box_value(RS_(L"SftpDownload")));
            _download.IsEnabled(false);
            _download.Click([weak](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    self->_chooseDownloadDestination();
                }
            });
            _entries.IsItemClickEnabled(true);
            _entries.SelectionMode(ListViewSelectionMode::Single);
            _entries.MaxHeight(360);
            _entries.SelectionChanged([weak](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    self->_updateDownloadButton();
                }
            });
            _entries.ItemClick([weak](auto&&, const ItemClickEventArgs& args) {
                if (const auto self = weak.lock())
                {
                    if (const auto entry = self->_entryFor(args.ClickedItem()); entry && entry->directory)
                    {
                        self->_navigate(self->_directoryPath(entry->name));
                    }
                }
            });
            StackPanel actions;
            actions.Orientation(Orientation::Horizontal);
            actions.Spacing(8);
            actions.Children().Append(open);
            actions.Children().Append(_up);
            actions.Children().Append(_download);
            _status.TextWrapping(TextWrapping::Wrap);
            Automation::AutomationProperties::SetLiveSetting(_status, Automation::Peers::AutomationLiveSetting::Polite);
            StackPanel content;
            content.Spacing(8);
            content.Children().Append(_path);
            content.Children().Append(actions);
            content.Children().Append(_status);
            content.Children().Append(_entries);
            Dialog.Content(content);
            _navigate(L".");
        }

    private:
        std::wstring _wta;
        winrt::Windows::UI::Core::CoreDispatcher _dispatcher;
        HWND _owner;
        std::wstring _targetArguments;
        TextBox _path;
        Button _up;
        Button _download;
        ListView _entries;
        TextBlock _status;
        hstring _directory;
        std::vector<RemoteEntry> _displayedEntries;
        std::shared_ptr<std::atomic<bool>> _pending;
        std::shared_ptr<FileTransfer> _transfer;
        bool _closed{};

        const RemoteEntry* _entryFor(const winrt::Windows::Foundation::IInspectable& value) const
        {
            if (const auto item = value.try_as<ListViewItem>())
            {
                const auto index = item.Tag().try_as<uint32_t>();
                if (index && *index < _displayedEntries.size())
                {
                    return &_displayedEntries[*index];
                }
            }
            return nullptr;
        }

        void _updateDownloadButton()
        {
            const auto selected = _entryFor(_entries.SelectedItem());
            _download.IsEnabled(!_closed && _owner && !_pending && !_transfer && selected && !selected->directory);
        }

        hstring _directoryPath(const std::wstring_view name) const
        {
            auto path = std::wstring{ _directory };
            if (!path.empty() && !path.ends_with(L'/'))
            {
                path.push_back(L'/');
            }
            return hstring{ path.append(name) };
        }

        void _navigate(const hstring& path)
        {
            if (_closed || _transfer)
            {
                return;
            }
            if (_pending)
            {
                _pending->store(true, std::memory_order_relaxed);
            }
            const auto cancelled = std::make_shared<std::atomic<bool>>(false);
            _pending = cancelled;
            _entries.IsEnabled(false);
            _download.IsEnabled(false);
            _entries.SelectedItem(nullptr);
            _status.Text(RS_(L"SftpReadingDirectory"));
            auto arguments = std::wstring{ L"sftp-list" }.append(_targetArguments);
            arguments.push_back(L' ');
            til::append_quoted_argument(arguments, std::wstring{ L"--path=" }.append(path.empty() ? L"." : std::wstring_view{ path }));
            _listRemoteDirectory(_dispatcher, _wta, std::move(arguments), cancelled,
                                 [weak = weak_from_this(), cancelled](RemoteDirectory result) {
                                     if (const auto self = weak.lock(); self && self->_pending == cancelled)
                                     {
                                         self->_pending.reset();
                                         self->_showDirectory(result);
                                     }
                                 });
        }

        void _showDirectory(const RemoteDirectory& result)
        {
            _entries.IsEnabled(true);
            if (!result.error.empty())
            {
                if (!_directory.empty())
                {
                    _path.Text(_directory);
                }
                _status.Text(result.error);
                return;
            }
            _directory = result.path;
            _path.Text(result.path);
            _up.IsEnabled(!result.path.empty() && result.path != L"/");
            auto items = _entries.Items();
            items.Clear();
            _displayedEntries = result.entries;
            for (uint32_t index = 0; index < _displayedEntries.size(); ++index)
            {
                const auto& entry = _displayedEntries[index];
                ListViewItem item;
                item.Tag(box_value(index));
                if (entry.directory)
                {
                    item.Content(box_value(hstring{ std::wstring{ entry.name }.append(L"/") }));
                }
                else if (entry.size)
                {
                    item.Content(box_value(hstring{ RS_fmt(L"SftpEntrySize", std::wstring_view{ entry.name }, *entry.size) }));
                }
                else
                {
                    item.Content(box_value(entry.name));
                }
                items.Append(item);
            }
            _status.Text(result.truncated ? RS_(L"SftpDirectoryTruncated") :
                                           result.entries.empty() ? RS_(L"SftpDirectoryEmpty") : hstring{});
        }

        safe_void_coroutine _chooseDownloadDestination()
        {
            const auto lifetime = shared_from_this();
            const auto entry = _entryFor(_entries.SelectedItem());
            if (_closed || !_owner || _pending || _transfer || !entry || entry->directory)
            {
                co_return;
            }
            const auto name = entry->name;
            const auto source = _directoryPath(name);
            try
            {
                const auto destination = co_await SaveFilePicker(_owner, [name](const auto& dialog) {
                    DWORD options{};
                    THROW_IF_FAILED(dialog->GetOptions(&options));
                    THROW_IF_FAILED(dialog->SetOptions(options & ~FOS_OVERWRITEPROMPT));
                    if (std::wstring_view{ name }.find_first_of(L"<>:\"/\\|?*") == std::wstring_view::npos)
                    {
                        THROW_IF_FAILED(dialog->SetFileName(name.c_str()));
                    }
                });
                if (_closed || destination.empty() || _pending || _transfer)
                {
                    co_return;
                }
                auto arguments = std::wstring{ L"sftp-download" }.append(_targetArguments).append(L" ");
                til::append_quoted_argument(arguments, std::wstring{ L"--path=" }.append(source));
                const auto transfer = std::make_shared<FileTransfer>();
                _transfer = transfer;
                _status.Text(RS_(L"SftpDownloading"));
                _entries.IsEnabled(false);
                _download.IsEnabled(false);
                _downloadRemoteFile(_dispatcher, _wta, std::move(arguments), std::filesystem::path{ destination.c_str() }, transfer,
                                    [weak = weak_from_this()](const hstring& status) {
                                        if (const auto self = weak.lock(); self && !self->_closed)
                                        {
                                            self->_transfer.reset();
                                            self->_entries.IsEnabled(true);
                                            self->_updateDownloadButton();
                                            self->_status.Text(status);
                                        }
                                    });
            }
            catch (...)
            {
                LOG_CAUGHT_EXCEPTION();
                if (_transfer)
                {
                    _transfer->Cancel();
                }
                _transfer.reset();
                _entries.IsEnabled(true);
                _updateDownloadButton();
                _status.Text(RS_(L"SftpDownloadFailed"));
            }
        }
    };
}

namespace winrt::TerminalApp::implementation
{
    safe_void_coroutine TerminalPage::_ShowSftpBrowser(OpenSsh::SavedHost host)
    {
        const auto lifetime = get_strong();
        co_await wil::resume_foreground(Dispatcher(), winrt::Windows::UI::Core::CoreDispatcherPriority::Low);
        const auto presenter = _dialogPresenter.get();
        if (!presenter)
        {
            co_return;
        }

        hstring error;
        try
        {
            const auto settingsPath = winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings::SettingsDirectory();
            const OpenSsh::HostStore store{ std::filesystem::path{ settingsPath.c_str() } };
            const auto hosts = store.Load();
            const auto current = std::ranges::find(hosts, host.id, &OpenSsh::SavedHost::id);
            if (current == hosts.end() || *current != host)
            {
                error = RS_(L"SshHostsChanged");
            }
            else if (!host.usesOpenSshConfig && host.authentication == OpenSsh::HostAuthentication::Password)
            {
                error = RS_(L"SftpBrowseAuthenticationRequired");
            }
            else if (const auto wta = _DetectWtaPath(); !wta.empty())
            {
                const auto browser = std::make_shared<SftpBrowser>(host, std::wstring{ wta }, Dispatcher(), _hostingHwnd.value_or(nullptr));
                browser->Initialize();
                const auto action = co_await presenter.ShowDialog(browser->Dialog);
                browser->Close();
                if (action == ContentDialogResult::Secondary)
                {
                    _ConnectSshHost(host, {}, true);
                }
                co_return;
            }
            else
            {
                error = RS_(L"SftpBrowseFailed");
            }
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            error = RS_(L"SftpBrowseFailed");
        }

        ContentDialog dialog;
        dialog.Title(box_value(hstring{ RS_fmt(L"SftpBrowseTitle", std::wstring_view{ host.name }) }));
        dialog.Content(box_value(error));
        dialog.SecondaryButtonText(RS_(L"SshHostsOpenSftp"));
        dialog.CloseButtonText(RS_(L"SshHostsClose"));
        const auto action = co_await presenter.ShowDialog(dialog);
        if (action == ContentDialogResult::Secondary)
        {
            _ConnectSshHost(host, {}, true);
        }
    }
}
