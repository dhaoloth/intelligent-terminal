// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalPage.h"
#include "SshHostStore.h"

#include <LibraryResources.h>
#include <winrt/Windows.UI.Xaml.Automation.h>

#include "../inc/OpenSshExecutable.h"
#include "../../types/inc/utils.hpp"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

using winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings;

namespace OpenSsh = ::Microsoft::Terminal::OpenSsh;
namespace TreeControls = winrt::Microsoft::UI::Xaml::Controls;

namespace
{
    bool _containsIgnoreCase(const std::wstring_view text, const std::wstring_view query) noexcept
    {
        if (query.empty())
        {
            return true;
        }
        if (query.size() > text.size())
        {
            return false;
        }
        for (size_t offset = 0; offset + query.size() <= text.size(); ++offset)
        {
            if (CompareStringOrdinal(text.data() + offset, static_cast<int>(query.size()), query.data(), static_cast<int>(query.size()), TRUE) == CSTR_EQUAL)
            {
                return true;
            }
        }
        return false;
    }

    hstring _hostFieldError(const OpenSsh::InvalidHostField field)
    {
        using OpenSsh::InvalidHostField;
        switch (field)
        {
        case InvalidHostField::Name:
            return RS_(L"SshHostsInvalidName");
        case InvalidHostField::Group:
            return RS_(L"SshHostsInvalidGroup");
        case InvalidHostField::Address:
            return RS_(L"SshHostsInvalidAddress");
        case InvalidHostField::Port:
            return RS_(L"SshHostsInvalidPort");
        case InvalidHostField::Username:
            return RS_(L"SshHostsInvalidUsername");
        case InvalidHostField::IdentityFile:
            return RS_(L"SshHostsInvalidIdentity");
        default:
            return {};
        }
    }

    struct SshHostsDialog : std::enable_shared_from_this<SshHostsDialog>
    {
        explicit SshHostsDialog(const std::filesystem::path& settingsDirectory, std::optional<OpenSsh::SavedHost> host) :
            _store{ settingsDirectory },
            _selected{ std::move(host) }
        {
        }

        ContentDialog Dialog;

        void Initialize()
        {
            Dialog.Title(box_value(_selected ? RS_(L"SshHostsEdit") : RS_(L"SshHostsAdd")));
            Dialog.CloseButtonText(RS_(L"SshHostsClose"));

            _name.Header(box_value(RS_(L"SshHostsName")));
            _group.Header(box_value(RS_(L"SshHostsGroup")));
            _group.PlaceholderText(RS_(L"SshHostsGroupHint"));
            _address.Header(box_value(RS_(L"SshHostsAddress")));
            _port.Header(box_value(RS_(L"SshHostsPort")));
            _port.MaxLength(5);
            _username.Header(box_value(RS_(L"SshHostsUsername")));
            _identity.Header(box_value(RS_(L"SshHostsIdentity")));
            _identity.PlaceholderText(RS_(L"SshHostsIdentityHint"));

            const auto usesConfig = _selected && _selected->usesOpenSshConfig;
            _address.IsEnabled(!usesConfig);
            _port.IsEnabled(!usesConfig);
            _username.IsEnabled(!usesConfig);
            _identity.IsEnabled(!usesConfig);

            _save.Content(box_value(RS_(L"SshHostsSave")));

            StackPanel editor;
            editor.Spacing(6);
            editor.Margin({ 12, 0, 0, 0 });
            for (const auto& field : { _name, _group, _address, _port, _username, _identity })
            {
                editor.Children().Append(field);
            }
            if (usesConfig)
            {
                TextBlock configInfo;
                configInfo.Text(RS_(L"SshHostsConfigValuesInfo"));
                configInfo.TextWrapping(TextWrapping::Wrap);
                Automation::AutomationProperties::SetLiveSetting(configInfo, Automation::Peers::AutomationLiveSetting::Polite);
                editor.Children().Append(configInfo);
            }
            editor.Children().Append(_save);
            _status.TextWrapping(TextWrapping::Wrap);
            Automation::AutomationProperties::SetLiveSetting(_status, Automation::Peers::AutomationLiveSetting::Polite);
            editor.Children().Append(_status);

            ScrollViewer scroll;
            scroll.Content(editor);
            Dialog.Content(scroll);

            const auto weak = weak_from_this();
            for (const auto& field : { _name, _group, _address, _port, _username, _identity })
            {
                field.TextChanged([weak](auto&&, auto&&) {
                    if (const auto self = weak.lock())
                    {
                        self->_updateButtons();
                    }
                });
            }
            _save.Click([weak](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    self->_saveHost();
                }
            });
            const auto display = _selected.value_or(OpenSsh::SavedHost{});
            _name.Text(display.name);
            _group.Text(display.group);
            _address.Text(display.address);
            _port.Text(std::to_wstring(display.port));
            _username.Text(display.username);
            _identity.Text(display.identityFile);
            _updateButtons();
        }

    private:
        OpenSsh::HostStore _store;
        std::optional<OpenSsh::SavedHost> _selected;
        TextBox _name;
        TextBox _group;
        TextBox _address;
        TextBox _port;
        TextBox _username;
        TextBox _identity;
        Button _save;
        TextBlock _status;

        OpenSsh::SavedHost _draft() const
        {
            return {
                _selected ? _selected->id : std::wstring{},
                std::wstring{ _name.Text() },
                std::wstring{ _group.Text() },
                std::wstring{ _address.Text() },
                OpenSsh::ParsePort(_port.Text()),
                std::wstring{ _username.Text() },
                std::wstring{ _identity.Text() },
                _selected && _selected->usesOpenSshConfig,
            };
        }

        void _updateButtons()
        {
            const auto draft = _draft();
            const auto unchanged = _selected && *_selected == draft;
            _save.IsEnabled(!unchanged && (_selected || !draft.name.empty() || !draft.address.empty()));
        }

        void _saveHost()
        {
            try
            {
                auto host = _draft();
                if (const auto field = OpenSsh::ValidateHost(host); field != OpenSsh::InvalidHostField::None)
                {
                    _status.Text(_hostFieldError(field));
                    return;
                }
                if (host.id.empty())
                {
                    GUID id{};
                    THROW_IF_FAILED(CoCreateGuid(&id));
                    host.id = ::Microsoft::Console::Utils::GuidToString(id);
                }
                if (_store.Replace(_selected, std::move(host)))
                {
                    Dialog.Hide();
                }
                else
                {
                    _status.Text(RS_(L"SshHostsChanged"));
                }
            }
            catch (...)
            {
                if (wil::ResultFromCaughtException() == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION))
                {
                    _status.Text(RS_(L"SshHostsWriteBusy"));
                }
                else
                {
                    LOG_CAUGHT_EXCEPTION();
                    _status.Text(RS_(L"SshHostsWriteFailed"));
                }
            }
        }

    };
}

namespace winrt::TerminalApp::implementation
{
    void TerminalPage::_UpdateSshHostsDockLayout()
    {
        const auto visible = _freChromeVisible && !_IsFreRequired() && !_isInFocusMode && (!_isFullscreen || _showTabsFullscreen);
        SshHostsDock().Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        const auto width = visible ? std::min(SshHostsDockWidth, WorkspaceRoot().ActualWidth() / 3.0) : 0.0;
        WorkspaceRoot().ColumnDefinitions().GetAt(0).Width(GridLengthHelper::FromValueAndType(width, GridUnitType::Pixel));
    }

    void TerminalPage::_InitializeSshHosts()
    {
        using namespace winrt::Windows::UI::Xaml::Controls;
        using namespace winrt::Windows::UI::Xaml;

        hstring query;
        if (SshHostsDock().Children().Size() != 0)
        {
            const auto previousPanel = SshHostsDock().Children().GetAt(0).try_as<Grid>();
            if (previousPanel && previousPanel.Children().Size() > 1)
            {
                if (const auto previousSearch = previousPanel.Children().GetAt(1).try_as<TextBox>())
                {
                    query = previousSearch.Text();
                }
            }
        }
        SshHostsDock().Children().Clear();
        Grid panel;
        RowDefinition headerRow;
        headerRow.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
        RowDefinition searchRow;
        searchRow.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
        RowDefinition treeRow;
        treeRow.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        RowDefinition actionsRow;
        actionsRow.Height(GridLengthHelper::FromValueAndType(0, GridUnitType::Auto));
        panel.RowDefinitions().Append(headerRow);
        panel.RowDefinitions().Append(searchRow);
        panel.RowDefinitions().Append(treeRow);
        panel.RowDefinitions().Append(actionsRow);

        TextBlock title;
        title.Text(RS_(L"SshHostsMenuItem"));
        title.Margin({ 12, 8, 12, 8 });
        title.FontSize(16);
        title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        panel.Children().Append(title);

        TextBox search;
        search.PlaceholderText(RS_(L"SshHostsSearch"));
        search.Margin({ 8, 0, 8, 8 });
        search.Text(query);
        Automation::AutomationProperties::SetName(search, RS_(L"SshHostsSearch"));
        Grid::SetRow(search, 1);
        panel.Children().Append(search);

        TreeControls::TreeView tree;
        tree.SelectionMode(TreeControls::TreeViewSelectionMode::Single);
        tree.CanDragItems(false);
        tree.CanReorderItems(false);
        Automation::AutomationProperties::SetName(tree, RS_(L"SshHostsMenuItem"));

        std::vector<OpenSsh::SavedHost> hosts;
        auto hostNodes = std::make_shared<std::vector<std::pair<TreeControls::TreeViewNode, OpenSsh::SavedHost>>>();
        bool hostsLoaded = true;
        try
        {
            OpenSsh::HostStore store{ std::filesystem::path{ CascadiaSettings::SettingsDirectory().c_str() } };
            hosts = store.Load();
            std::ranges::sort(hosts, [](const auto& left, const auto& right) {
                return std::tie(left.group, left.name, left.id) < std::tie(right.group, right.name, right.id);
            });

        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            hostsLoaded = false;
        }

        const auto populateTree = [tree, hosts, hostNodes](const hstring& filter) {
            hostNodes->clear();
            tree.SelectedItem(nullptr);
            tree.RootNodes().Clear();
            std::map<std::wstring, TreeControls::TreeViewNode, std::less<>> groups;
            const auto groupNode = [&](const auto& self, const std::wstring_view path) -> TreeControls::TreeViewNode {
                if (const auto existing = groups.find(path); existing != groups.end())
                {
                    return existing->second;
                }
                TreeControls::TreeViewNode node;
                const auto slash = path.rfind(L'/');
                node.Content(box_value(hstring{ path.substr(slash == std::wstring_view::npos ? 0 : slash + 1) }));
                node.IsExpanded(true);
                const auto siblings = slash == std::wstring_view::npos ? tree.RootNodes() : self(self, path.substr(0, slash)).Children();
                siblings.Append(node);
                groups.emplace(path, node);
                return node;
            };
            for (const auto& host : hosts)
            {
                if (!_containsIgnoreCase(host.name, filter) && !_containsIgnoreCase(host.address, filter) && !_containsIgnoreCase(host.group, filter))
                {
                    continue;
                }
                if (!host.group.empty())
                {
                    groupNode(groupNode, host.group);
                }
                TreeControls::TreeViewNode node;
                node.Content(box_value(hstring{ host.name }));
                const auto siblings = host.group.empty() ? tree.RootNodes() : groups.at(host.group).Children();
                siblings.Append(node);
                hostNodes->emplace_back(node, host);
            }
        };
        populateTree(query);
        const winrt::weak_ref<TreeControls::TreeView> weakTree{ tree };
        auto selectedHost = std::make_shared<std::optional<OpenSsh::SavedHost>>();
        tree.RegisterPropertyChangedCallback(TreeControls::TreeView::SelectedItemProperty(), [weakTree, hostNodes, selectedHost](auto&&, auto&&) {
            selectedHost->reset();
            if (const auto currentTree = weakTree.get())
            {
                const auto node = currentTree.SelectedItem().try_as<TreeControls::TreeViewNode>();
                const auto found = std::ranges::find(*hostNodes, node, [](const auto& entry) { return entry.first; });
                if (found != hostNodes->end())
                {
                    *selectedHost = found->second;
                }
            }
        });
        const auto connectSelected = [weakThis{ get_weak() }, weakTree, selectedHost]() {
            const auto currentTree = weakTree.get();
            const auto host = *selectedHost;
            if (!currentTree || !host)
            {
                return;
            }
            if (const auto page = weakThis.get())
            {
                page->_ConnectSshHost(*host);
            }
        };
        const auto connectFromRow = [weakThis{ get_weak() }, weakTree, hostNodes](const Windows::Foundation::IInspectable& originalSource) {
            const auto page = weakThis.get();
            const auto currentTree = weakTree.get();
            if (!page || !currentTree)
            {
                return false;
            }
            const auto treeElement = currentTree.as<Windows::UI::Xaml::DependencyObject>();
            for (auto source = originalSource.try_as<Windows::UI::Xaml::DependencyObject>(); source && source != treeElement;
                 source = Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(source))
            {
                if (const auto item = source.try_as<TreeControls::TreeViewItem>())
                {
                    const auto node = currentTree.NodeFromContainer(item);
                    const auto host = std::ranges::find(*hostNodes, node, [](const auto& entry) { return entry.first; });
                    if (host == hostNodes->end())
                    {
                        return false;
                    }
                    page->_ConnectSshHost(host->second);
                    return true;
                }
            }
            return false;
        };
        tree.DoubleTapped([connectFromRow](auto&&, const Windows::UI::Xaml::Input::DoubleTappedRoutedEventArgs& args) {
            if (connectFromRow(args.OriginalSource()))
            {
                args.Handled(true);
            }
        });
        tree.PreviewKeyDown([connectFromRow](auto&&, const Windows::UI::Xaml::Input::KeyRoutedEventArgs& args) {
            if (args.Key() == Windows::System::VirtualKey::Enter && !args.KeyStatus().WasKeyDown && connectFromRow(args.OriginalSource()))
            {
                args.Handled(true);
            }
        });
        Grid::SetRow(tree, 2);
        TextBlock noMatches;
        noMatches.Text(RS_(L"SshHostsNoMatches"));
        noMatches.Margin({ 12, 8, 12, 8 });
        noMatches.TextWrapping(TextWrapping::Wrap);
        noMatches.Visibility(!query.empty() && hostNodes->empty() ? Visibility::Visible : Visibility::Collapsed);
        Automation::AutomationProperties::SetLiveSetting(noMatches, Automation::Peers::AutomationLiveSetting::Polite);
        Grid::SetRow(noMatches, 2);
        panel.Children().Append(tree);
        panel.Children().Append(noMatches);
        search.TextChanged([populateTree, selectedHost, hostNodes, noMatches](const Windows::Foundation::IInspectable& sender, auto&&) {
            selectedHost->reset();
            const auto filter = sender.as<TextBox>().Text();
            populateTree(filter);
            noMatches.Visibility(!filter.empty() && hostNodes->empty() ? Visibility::Visible : Visibility::Collapsed);
        });
        TextBlock status;
        status.TextWrapping(TextWrapping::Wrap);
        if (!hostsLoaded)
        {
            status.Text(RS_(L"SshHostsReadFailed"));
        }
        Automation::AutomationProperties::SetLiveSetting(status, Automation::Peers::AutomationLiveSetting::Polite);
        StackPanel actions;
        actions.Spacing(6);
        Grid::SetRow(actions, 3);

        for (const auto actionIndex : { 0, 1, 2 })
        {
            Button action;
            action.Content(box_value(actionIndex == 0 ? RS_(L"SshHostsAdd") : actionIndex == 1 ? RS_(L"SshHostsEdit") : RS_(L"SshHostsRemove")));
            action.HorizontalAlignment(HorizontalAlignment::Stretch);
            if (actionIndex == 2)
            {
                MenuFlyout confirmRemove;
                MenuFlyoutItem removeItem;
                removeItem.Text(RS_(L"SshHostsConfirmRemove"));
                removeItem.Click([weakThis{ get_weak() }, selectedHost, status](auto&&, auto&&) {
                    const auto host = *selectedHost;
                    if (const auto page = weakThis.get(); page && host)
                    {
                        try
                        {
                            OpenSsh::HostStore store{ std::filesystem::path{ CascadiaSettings::SettingsDirectory().c_str() } };
                            if (store.Replace(host, std::nullopt))
                            {
                                page->_InitializeSshHosts();
                            }
                            else
                            {
                                status.Text(RS_(L"SshHostsChanged"));
                            }
                        }
                        catch (...)
                        {
                            if (wil::ResultFromCaughtException() == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION))
                            {
                                status.Text(RS_(L"SshHostsWriteBusy"));
                            }
                            else
                            {
                                LOG_CAUGHT_EXCEPTION();
                                status.Text(RS_(L"SshHostsWriteFailed"));
                            }
                        }
                    }
                });
                confirmRemove.Items().Append(removeItem);
                action.Flyout(confirmRemove);
            }
            else
            {
                if (actionIndex == 0)
                {
                    action.IsEnabled(hostsLoaded);
                }
                action.Click([weakThis{ get_weak() }, selectedHost, actionIndex](auto&&, auto&&) {
                    if (const auto page = weakThis.get())
                    {
                        if (actionIndex == 0)
                        {
                            page->_ShowSshHosts();
                        }
                        else if (const auto host = *selectedHost)
                        {
                            page->_ShowSshHosts(*host);
                        }
                    }
                });
            }
            actions.Children().Append(action);
        }

        Button connect;
        connect.Content(box_value(RS_(L"SshHostsConnect")));
        connect.HorizontalAlignment(HorizontalAlignment::Stretch);
        connect.Click([connectSelected](auto&&, auto&&) { connectSelected(); });
        actions.Children().Append(connect);

        Button importAliases;
        importAliases.Content(box_value(RS_(L"SshHostsImportAliases")));
        importAliases.HorizontalAlignment(HorizontalAlignment::Stretch);
        importAliases.IsEnabled(hostsLoaded);
        importAliases.Click([weakThis{ get_weak() }, status](auto&&, auto&&) {
            const auto page = weakThis.get();
            if (!page)
            {
                return;
            }
            try
            {
                auto aliases = OpenSsh::LoadConfigAliases();
                ContentDialog dialog;
                dialog.Title(box_value(RS_(L"SshHostsImportPreview")));
                dialog.PrimaryButtonText(RS_(L"SshHostsImportConfirm"));
                dialog.CloseButtonText(RS_(L"SshHostsClose"));

                StackPanel preview;
                preview.Spacing(6);
                TextBlock description;
                description.Text(aliases.empty() ? RS_(L"SshHostsImportEmpty") : RS_(L"SshHostsImportDescription"));
                description.TextWrapping(TextWrapping::Wrap);
                preview.Children().Append(description);
                if (!aliases.empty())
                {
                    TextBlock aliasList;
                    std::wstring text;
                    for (const auto& alias : aliases)
                    {
                        if (!text.empty())
                        {
                            text.append(L"\n");
                        }
                        text.append(alias);
                    }
                    aliasList.Text(text);
                    aliasList.TextWrapping(TextWrapping::Wrap);
                    preview.Children().Append(aliasList);
                }
                ScrollViewer scroll;
                scroll.MaxHeight(420);
                scroll.Content(preview);
                dialog.Content(scroll);
                dialog.IsPrimaryButtonEnabled(!aliases.empty());

                const auto imported = std::make_shared<bool>(false);
                dialog.PrimaryButtonClick([weakThis, aliases, imported, status](auto&&, auto&&) {
                    if (const auto currentPage = weakThis.get())
                    {
                        try
                        {
                            OpenSsh::HostStore store{ std::filesystem::path{ CascadiaSettings::SettingsDirectory().c_str() } };
                            const auto count = store.ImportAliases(aliases);
                            status.Text(count == 0 ? RS_(L"SshHostsImportNothingNew") : RS_(L"SshHostsImportSucceeded"));
                            *imported = count != 0;
                        }
                        catch (...)
                        {
                            if (wil::ResultFromCaughtException() == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION))
                            {
                                status.Text(RS_(L"SshHostsWriteBusy"));
                            }
                            else
                            {
                                LOG_CAUGHT_EXCEPTION();
                                status.Text(RS_(L"SshHostsImportFailed"));
                            }
                        }
                    }
                });
                dialog.Closed([weakThis, imported](auto&&, auto&&) {
                    if (*imported)
                    {
                        if (const auto currentPage = weakThis.get())
                        {
                            currentPage->_InitializeSshHosts();
                        }
                    }
                });

                if (const auto presenter = page->_dialogPresenter.get())
                {
                    presenter.ShowDialog(dialog);
                }
            }
            catch (...)
            {
                if (wil::ResultFromCaughtException() == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION))
                {
                    status.Text(RS_(L"SshHostsWriteBusy"));
                }
                else
                {
                    LOG_CAUGHT_EXCEPTION();
                    status.Text(RS_(L"SshHostsImportReadFailed"));
                }
            }
        });
        actions.Children().Append(importAliases);
        actions.Children().Append(status);
        panel.Children().Append(actions);
        SshHostsDock().Children().Append(panel);
    }

    void TerminalPage::_ConnectSshHost(const OpenSsh::SavedHost& host)
    {
        hstring error;
        try
        {
            OpenSsh::HostStore store{ std::filesystem::path{ CascadiaSettings::SettingsDirectory().c_str() } };
            const auto hosts = store.Load();
            const auto current = std::ranges::find(hosts, host.id, &OpenSsh::SavedHost::id);
            if (current == hosts.end() || *current != host)
            {
                _InitializeSshHosts();
                error = RS_(L"SshHostsChanged");
            }
            else
            {
                std::error_code identityError;
                if (!current->identityFile.empty() && !std::filesystem::is_regular_file(std::filesystem::path{ current->identityFile }, identityError))
                {
                    error = RS_(L"SshHostsIdentityUnavailable");
                }
                const auto executable = OpenSsh::FindExecutable();
                if (error.empty() && executable.empty())
                {
                    error = RS_(L"SshHostsOpenSshMissing");
                }
                if (error.empty())
                {
                    using namespace winrt::Microsoft::Terminal::Settings::Model;
                    NewTerminalArgs args;
                    args.Commandline(OpenSsh::BuildCommandline(executable, *current));
                    args.TabTitle(current->name);
                    args.StartingDirectory(L"%USERPROFILE%");
                    args.Elevate(false);

                    const auto azureType = Microsoft::Terminal::TerminalConnection::AzureConnection::ConnectionType();
                    if (const auto profile = _settings.GetProfileForArgs(args);
                        profile && profile.ConnectionType() == azureType && Microsoft::Terminal::TerminalConnection::AzureConnection::IsAzureConnectionAvailable())
                    {
                        bool foundFallback = false;
                        for (const auto& candidate : _settings.ActiveProfiles())
                        {
                            if (candidate.ConnectionType() != azureType)
                            {
                                args.Profile(::Microsoft::Console::Utils::GuidToString(candidate.Guid()));
                                foundFallback = true;
                                break;
                            }
                        }
                        if (!foundFallback)
                        {
                            error = RS_(L"SshHostsLaunchFailed");
                        }
                    }
                    if (error.empty() && _OpenNewTab(args) != S_OK)
                    {
                        error = RS_(L"SshHostsLaunchFailed");
                    }
                }
            }
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            error = RS_(L"SshHostsLaunchFailed");
        }

        if (!error.empty())
        {
            ContentDialog dialog;
            dialog.Title(box_value(RS_(L"SshHostsMenuItem")));
            dialog.Content(box_value(error));
            dialog.CloseButtonText(RS_(L"SshHostsClose"));
            if (const auto presenter = _dialogPresenter.get())
            {
                presenter.ShowDialog(dialog);
            }
        }
    }

    safe_void_coroutine TerminalPage::_ShowSshHosts(std::optional<OpenSsh::SavedHost> host)
    {
        const auto weak = get_weak();
        // Run the dialog after the invoking control finishes its input event.
        co_await wil::resume_foreground(Dispatcher(), Windows::UI::Core::CoreDispatcherPriority::Low);
        const auto presenter = [&weak]() -> IDialogPresenter {
            if (const auto page = weak.get())
            {
                return page->_dialogPresenter.get();
            }
            return nullptr;
        }();
        if (!presenter)
        {
            co_return;
        }

        hstring error;
        try
        {
            using namespace winrt::Microsoft::Terminal::Settings::Model;
            const auto editor = std::make_shared<SshHostsDialog>(std::filesystem::path{ CascadiaSettings::SettingsDirectory().c_str() }, std::move(host));
            editor->Initialize();
            co_await presenter.ShowDialog(editor->Dialog);
            if (const auto page = weak.get())
            {
                page->_InitializeSshHosts();
            }
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            error = RS_(L"SshHostsReadFailed");
        }

        if (!error.empty() && weak.get())
        {
            ContentDialog dialog;
            dialog.Title(box_value(RS_(L"SshHostsMenuItem")));
            dialog.Content(box_value(error));
            dialog.CloseButtonText(RS_(L"SshHostsClose"));
            co_await presenter.ShowDialog(dialog);
        }
    }
}
