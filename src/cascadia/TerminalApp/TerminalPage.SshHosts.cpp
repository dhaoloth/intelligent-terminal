// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "TerminalPage.h"
#include "SshHostStore.h"
#include "ContentManager.h"

#include <LibraryResources.h>
#include <array>
#include <json/json.h>
#include <til/commandline.h>
#include <winrt/Windows.UI.Xaml.Automation.h>

#include "../inc/AgentPolicy.h"
#include "../inc/OpenSshExecutable.h"
#include "../inc/WslDistributions.h"
#include "../inc/WtaProcess.h"
#include "../WinRTUtils/inc/Utils.h"
#include "../../types/inc/utils.hpp"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

using winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings;

namespace OpenSsh = ::Microsoft::Terminal::OpenSsh;
namespace TreeControls = winrt::Microsoft::UI::Xaml::Controls;
namespace AgentPolicy = ::Microsoft::Terminal::Settings::Model::AgentPolicy;

namespace
{
    struct NativeAgent
    {
        std::wstring_view executable;
        std::wstring_view label;
    };

    constexpr std::array NativeAgents{
        NativeAgent{ L"codex", L"Codex CLI" },
        NativeAgent{ L"claude", L"Claude Code" },
        NativeAgent{ L"hermes", L"Hermes Agent" },
        NativeAgent{ L"qwen", L"Qwen Code" },
    };

    winrt::TerminalApp::implementation::WorkspaceAgentLaunch _workspaceAgentLaunch(
        const std::wstring_view provider, const std::wstring_view targetId, const std::wstring_view targetName, const bool remote)
    {
        const auto agent = std::ranges::find(NativeAgents, provider, &NativeAgent::executable);
        return { hstring{ provider }, hstring{ agent != NativeAgents.end() ? agent->label : provider },
                 hstring{ targetId }, hstring{ targetName }, remote };
    }

    struct ConnectionTarget
    {
        std::optional<OpenSsh::SavedHost> host;
        std::wstring distribution;

        std::wstring key() const
        {
            return host ? L"ssh:" + host->id : L"wsl:" + distribution;
        }
    };

    struct ConnectionRow
    {
        TreeControls::TreeViewNode node;
        ConnectionTarget target;
        const NativeAgent* agent{ nullptr };
    };

    struct AgentProbeResult
    {
        std::vector<const NativeAgent*> agents;
        winrt::hstring message;
        bool retryable{ false };
    };

    struct AgentDiscovery
    {
        std::map<std::wstring, AgentProbeResult, std::less<>> results;
        std::wstring pending;
        std::shared_ptr<std::atomic<bool>> cancelled;

        ~AgentDiscovery()
        {
            if (cancelled)
            {
                cancelled->store(true, std::memory_order_relaxed);
            }
        }
    };

    void _appendArgument(std::wstring& command, const std::wstring_view argument)
    {
        command.push_back(L' ');
        til::append_quoted_argument(command, argument);
    }

    std::wstring _probeArguments(const ConnectionTarget& target)
    {
        std::wstring args{ L"probe-native-agents" };
        if (const auto& host = target.host)
        {
            OpenSsh::AppendWtaSshTarget(args, *host);
        }
        else
        {
            args.append(L" --wsl-distro");
            _appendArgument(args, target.distribution);
        }
        return args;
    }

    AgentProbeResult _readAgentProbe(const ConnectionTarget& target, const ::Microsoft::Terminal::WtaProcess::CaptureResult& capture)
    {
        const auto failed = [] { return AgentProbeResult{ {}, RS_(L"ConnectionAgentDiscoveryUnavailable"), true }; };
        if (!capture.completed || capture.exitCode != 0)
        {
            return failed();
        }

        Json::Value root;
        const std::unique_ptr<Json::CharReader> reader{ Json::CharReaderBuilder{}.newCharReader() };
        const auto& output = capture.output;
        if (!reader->parse(output.data(), output.data() + output.size(), &root, nullptr) || !root.isObject() || root["schemaVersion"] != 1)
        {
            return failed();
        }
        const auto& reported = root["target"];
        const auto identity = winrt::to_string(target.host ? target.host->address : target.distribution);
        if (!reported.isObject() || reported["kind"] != (target.host ? "ssh" : "wsl") || reported[target.host ? "host" : "distribution"] != identity)
        {
            return failed();
        }
        if (root["status"] == "auth_error")
        {
            return { {}, RS_(L"ConnectionAgentAuthenticationRequired"), true };
        }
        if (root["status"] == "unsupported")
        {
            return { {}, RS_(L"ConnectionAgentUnsupported"), true };
        }
        if (root["status"] == "transport_error" || root["status"] == "timeout")
        {
            return { {}, RS_(L"ConnectionAgentTargetUnreachable"), true };
        }
        const auto& available = root["agents"];
        if (root["status"] != "ok" || !available.isObject())
        {
            return failed();
        }

        AgentProbeResult result;
        for (const auto& agent : NativeAgents)
        {
            const auto& found = available[winrt::to_string(agent.executable)];
            if (!found.isBool())
            {
                return failed();
            }
            if (found.asBool() && AgentPolicy::IsAgentAllowed(agent.executable))
            {
                result.agents.push_back(&agent);
            }
        }
        if (result.agents.empty())
        {
            result.message = RS_(L"ConnectionAgentNoneFound");
        }
        return result;
    }

    safe_void_coroutine _runAgentProbe(winrt::Windows::UI::Core::CoreDispatcher dispatcher,
                                       std::wstring wta,
                                       ConnectionTarget target,
                                       std::shared_ptr<std::atomic<bool>> cancelled,
                                       std::function<void(AgentProbeResult)> completed)
    {
        co_await winrt::resume_background();
        AgentProbeResult result;
        try
        {
            const auto captured = ::Microsoft::Terminal::WtaProcess::RunWtaCapture(wta, _probeArguments(target), 16000, nullptr, false, cancelled.get(), true);
            result = _readAgentProbe(target, captured);
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            result.message = RS_(L"ConnectionAgentDiscoveryUnavailable");
            result.retryable = true;
        }
        co_await wil::resume_foreground(dispatcher);
        if (!cancelled->load(std::memory_order_relaxed))
        {
            completed(std::move(result));
        }
    }

    void _refreshAgentNodes(std::vector<ConnectionRow>& rows, const std::wstring_view key, const AgentDiscovery& discovery)
    {
        const auto found = std::ranges::find_if(rows, [&](const ConnectionRow& row) { return !row.agent && row.target.key() == key; });
        if (found == rows.end())
        {
            return;
        }
        const auto parent = found->node;
        const auto target = found->target;
        std::erase_if(rows, [&](const ConnectionRow& row) { return row.agent && row.target.key() == key; });
        parent.Children().Clear();

        const auto result = discovery.results.find(key);
        if (result != discovery.results.end())
        {
            for (const auto* agent : result->second.agents)
            {
                TreeControls::TreeViewNode child;
                child.Content(box_value(hstring{ agent->label }));
                parent.Children().Append(child);
                rows.push_back({ child, target, agent });
            }
        }
        const auto message = discovery.pending == key ? RS_(L"ConnectionAgentDiscovering") :
                             result != discovery.results.end() ? result->second.message : hstring{};
        if (!message.empty())
        {
            TreeControls::TreeViewNode child;
            child.Content(box_value(message));
            parent.Children().Append(child);
        }
        parent.IsExpanded(parent.Children().Size() != 0);
    }

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
        case InvalidHostField::JumpHostAlias:
            return RS_(L"SshHostsInvalidJumpAlias");
        case InvalidHostField::Forwarding:
            return RS_(L"SshHostsInvalidForward");
        default:
            return {};
        }
    }

    struct SshHostsDialog : std::enable_shared_from_this<SshHostsDialog>
    {
        SshHostsDialog(const std::filesystem::path& settingsDirectory, std::optional<OpenSsh::SavedHost> host, const HWND owner) :
            _store{ settingsDirectory },
            _selected{ std::move(host) },
            _owner{ owner }
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
            _authentication.Header(box_value(RS_(L"SshHostsAuthentication")));
            _authentication.Items().Append(box_value(RS_(L"SshHostsAuthenticationOpenSsh")));
            _authentication.Items().Append(box_value(RS_(L"SshHostsAuthenticationPassword")));
            _authentication.Items().Append(box_value(RS_(L"SshHostsAuthenticationKey")));
            _browseIdentity.Content(box_value(RS_(L"SshHostsBrowseIdentity")));
            _browseIdentity.IsEnabled(_owner != nullptr);
            _identity.Header(box_value(RS_(L"SshHostsIdentity")));
            _identity.PlaceholderText(RS_(L"SshHostsIdentityHint"));
            _jumpAlias.Header(box_value(RS_(L"SshHostsJumpAlias")));
            _jumpAlias.PlaceholderText(RS_(L"SshHostsJumpAliasHint"));
            _jumpAlias.MaxLength(120);

            const auto usesConfig = _selected && _selected->usesOpenSshConfig;
            _address.IsEnabled(!usesConfig);
            _port.IsEnabled(!usesConfig);
            _username.IsEnabled(!usesConfig);

            _save.Content(box_value(RS_(L"SshHostsSave")));

            StackPanel editor;
            editor.Spacing(6);
            editor.Margin({ 12, 0, 0, 0 });
            for (const auto& field : { _name, _group, _address, _port, _username })
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
            else
            {
                editor.Children().Append(_authentication);
                editor.Children().Append(_identity);
                editor.Children().Append(_browseIdentity);
                editor.Children().Append(_jumpAlias);
                TextBlock forwardingLabel;
                forwardingLabel.Text(RS_(L"SshHostsForwarding"));
                forwardingLabel.Margin({ 0, 8, 0, 0 });
                editor.Children().Append(forwardingLabel);
                _forwardPanel.Spacing(8);
                editor.Children().Append(_forwardPanel);
                Button addForward;
                addForward.Content(box_value(RS_(L"SshHostsAddForward")));
                addForward.Click([weak = weak_from_this()](auto&&, auto&&) {
                    if (const auto self = weak.lock())
                    {
                        self->_appendForward({});
                        self->_updateButtons();
                    }
                });
                editor.Children().Append(addForward);
            }
            editor.Children().Append(_save);
            _status.TextWrapping(TextWrapping::Wrap);
            Automation::AutomationProperties::SetLiveSetting(_status, Automation::Peers::AutomationLiveSetting::Polite);
            editor.Children().Append(_status);

            ScrollViewer scroll;
            scroll.Content(editor);
            Dialog.Content(scroll);

            const auto weak = weak_from_this();
            for (const auto& field : { _name, _group, _address, _port, _username, _identity, _jumpAlias })
            {
                field.TextChanged([weak](auto&&, auto&&) {
                    if (const auto self = weak.lock())
                    {
                        self->_updateButtons();
                    }
                });
            }
            _authentication.SelectionChanged([weak](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    self->_updateButtons();
                }
            });
            _browseIdentity.Click([weak](auto&&, auto&&) {
                if (const auto self = weak.lock())
                {
                    self->_browseForIdentity();
                }
            });
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
            _authentication.SelectedIndex(static_cast<int32_t>(display.authentication));
            _jumpAlias.Text(display.jumpHostAlias);
            if (!usesConfig)
            {
                for (const auto& forward : display.forwards)
                {
                    _appendForward(forward);
                }
            }
            _updateButtons();
        }

    private:
        struct ForwardRow
        {
            StackPanel panel;
            ComboBox direction;
            TextBox listenHost;
            TextBox listenPort;
            TextBox destinationHost;
            TextBox destinationPort;

            OpenSsh::ForwardDirection Direction() const
            {
                return static_cast<OpenSsh::ForwardDirection>(std::max(0, direction.SelectedIndex()));
            }

            OpenSsh::PortForward Value() const
            {
                const auto kind = Direction();
                const auto dynamic = kind == OpenSsh::ForwardDirection::Dynamic;
                return {
                    kind,
                    std::wstring{ listenHost.Text() },
                    OpenSsh::ParsePort(listenPort.Text()),
                    dynamic ? std::wstring{} : std::wstring{ destinationHost.Text() },
                    dynamic ? 0u : OpenSsh::ParsePort(destinationPort.Text()),
                };
            }

            void UpdateDestinationVisibility() const
            {
                const auto visible = Direction() == OpenSsh::ForwardDirection::Dynamic ? Visibility::Collapsed : Visibility::Visible;
                destinationHost.Visibility(visible);
                destinationPort.Visibility(visible);
            }
        };

        OpenSsh::HostStore _store;
        std::optional<OpenSsh::SavedHost> _selected;
        HWND _owner;
        TextBox _name;
        TextBox _group;
        TextBox _address;
        TextBox _port;
        TextBox _username;
        ComboBox _authentication;
        TextBox _identity;
        Button _browseIdentity;
        TextBox _jumpAlias;
        StackPanel _forwardPanel;
        std::vector<std::shared_ptr<ForwardRow>> _forwardRows;
        Button _save;
        TextBlock _status;

        OpenSsh::SavedHost _draft() const
        {
            const auto usesConfig = _selected && _selected->usesOpenSshConfig;
            const auto authentication = static_cast<OpenSsh::HostAuthentication>(std::max(0, _authentication.SelectedIndex()));
            const auto identityFile = usesConfig || authentication == OpenSsh::HostAuthentication::IdentityFile ?
                                          std::wstring{ _identity.Text() } :
                                          std::wstring{};
            std::vector<OpenSsh::PortForward> forwards;
            forwards.reserve(_forwardRows.size());
            for (const auto& row : _forwardRows)
            {
                forwards.emplace_back(row->Value());
            }
            return {
                _selected ? _selected->id : std::wstring{},
                std::wstring{ _name.Text() },
                std::wstring{ _group.Text() },
                std::wstring{ _address.Text() },
                OpenSsh::ParsePort(_port.Text()),
                std::wstring{ _username.Text() },
                identityFile,
                usesConfig,
                std::wstring{ _jumpAlias.Text() },
                authentication,
                std::move(forwards),
            };
        }

        void _updateButtons()
        {
            const auto draft = _draft();
            const auto showsIdentity = !draft.usesOpenSshConfig && draft.authentication == OpenSsh::HostAuthentication::IdentityFile;
            _identity.Visibility(showsIdentity ? Visibility::Visible : Visibility::Collapsed);
            _browseIdentity.Visibility(showsIdentity ? Visibility::Visible : Visibility::Collapsed);
            const auto unchanged = _selected && *_selected == draft;
            _save.IsEnabled(!unchanged && (_selected || !draft.name.empty() || !draft.address.empty()));
        }

        void _appendForward(const OpenSsh::PortForward& forward)
        {
            const auto row = std::make_shared<ForwardRow>();
            row->panel.Spacing(4);
            row->direction.Header(box_value(RS_(L"SshHostsForwardDirection")));
            row->direction.Items().Append(box_value(RS_(L"SshHostsForwardLocal")));
            row->direction.Items().Append(box_value(RS_(L"SshHostsForwardRemote")));
            row->direction.Items().Append(box_value(RS_(L"SshHostsForwardDynamic")));
            row->direction.SelectedIndex(static_cast<int32_t>(forward.direction));

            row->listenHost.Header(box_value(RS_(L"SshHostsForwardListenHost")));
            row->listenHost.PlaceholderText(RS_(L"SshHostsForwardListenHostHint"));
            row->listenHost.Text(forward.listenHost);
            row->listenPort.Header(box_value(RS_(L"SshHostsForwardListenPort")));
            row->listenPort.MaxLength(5);
            if (forward.listenPort != 0)
            {
                row->listenPort.Text(std::to_wstring(forward.listenPort));
            }
            row->destinationHost.Header(box_value(RS_(L"SshHostsForwardDestinationHost")));
            row->destinationHost.Text(forward.destinationHost);
            row->destinationPort.Header(box_value(RS_(L"SshHostsForwardDestinationPort")));
            row->destinationPort.MaxLength(5);
            if (forward.destinationPort != 0)
            {
                row->destinationPort.Text(std::to_wstring(forward.destinationPort));
            }
            const auto weak = weak_from_this();
            const auto weakRow = std::weak_ptr<ForwardRow>{ row };
            row->direction.SelectionChanged([weak, weakRow](auto&&, auto&&) {
                if (const auto selected = weakRow.lock())
                {
                    selected->UpdateDestinationVisibility();
                }
                if (const auto self = weak.lock())
                {
                    self->_updateButtons();
                }
            });
            row->panel.Children().Append(row->direction);
            for (const auto& field : { row->listenHost, row->listenPort, row->destinationHost, row->destinationPort })
            {
                field.TextChanged([weak](auto&&, auto&&) {
                    if (const auto self = weak.lock())
                    {
                        self->_updateButtons();
                    }
                });
                row->panel.Children().Append(field);
            }

            Button remove;
            remove.Content(box_value(RS_(L"SshHostsRemoveForward")));
            remove.Click([weak, weakRow](auto&&, auto&&) {
                const auto self = weak.lock();
                const auto selected = weakRow.lock();
                if (self && selected)
                {
                    const auto children = self->_forwardPanel.Children();
                    uint32_t index{};
                    if (children.IndexOf(selected->panel, index))
                    {
                        children.RemoveAt(index);
                    }
                    std::erase(self->_forwardRows, selected);
                    self->_updateButtons();
                }
            });
            row->panel.Children().Append(remove);
            row->UpdateDestinationVisibility();
            _forwardRows.emplace_back(row);
            _forwardPanel.Children().Append(row->panel);
        }

        safe_void_coroutine _browseForIdentity()
        {
            const auto lifetime = shared_from_this();
            try
            {
                const auto path = co_await OpenFilePicker(_owner, [](const auto& dialog) {
                    THROW_IF_FAILED(dialog->SetTitle(RS_(L"SshHostsBrowseIdentity").c_str()));
                });
                if (!path.empty())
                {
                    _identity.Text(path);
                }
            }
            CATCH_LOG();
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
        const auto sessionsVisible = visible && _workspaceAgentSessionsOpen;
        WorkspaceAgentSessionsDock().Visibility(sessionsVisible ? Visibility::Visible : Visibility::Collapsed);

        const auto availableWidth = WorkspaceRoot().ActualWidth();
        const auto sideLimit = availableWidth * (sessionsVisible ? 0.23 : 1.0 / 3.0);
        const auto columns = WorkspaceRoot().ColumnDefinitions();
        columns.GetAt(0).Width(GridLengthHelper::FromValueAndType(
            visible ? std::min(SshHostsDockWidth, sideLimit) : 0.0, GridUnitType::Pixel));
        columns.GetAt(2).Width(GridLengthHelper::FromValueAndType(
            sessionsVisible ? std::min(300.0, sideLimit) : 0.0, GridUnitType::Pixel));
    }

    void TerminalPage::_ToggleWorkspaceAgentSessions()
    {
        _workspaceAgentSessionsOpen = !_workspaceAgentSessionsOpen;
        _UpdateSshHostsDockLayout();
        _UpdateWorkspaceAgentSessions();
    }

    void TerminalPage::_UpdateWorkspaceAgentSessions()
    {
        if (!_workspaceAgentSessionsOpen)
        {
            return;
        }

        const auto panel = WorkspaceAgentSessionsRows().Children();
        std::optional<uint64_t> focusedContent;
        winrt::Windows::UI::Xaml::FocusState priorFocus{ FocusState::Unfocused };
        for (const auto& child : panel)
        {
            if (const auto button = child.try_as<Button>(); button && button.FocusState() != FocusState::Unfocused)
            {
                focusedContent = winrt::unbox_value<uint64_t>(button.Tag());
                priorFocus = button.FocusState();
                break;
            }
        }
        panel.Clear();

        TextBlock title;
        title.Text(RS_(L"WorkspaceAgentSessionsTitle"));
        title.FontSize(16);
        title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
        panel.Append(title);

        auto sessions = winrt::get_self<ContentManager>(_manager)->WorkspaceAgentSessions();
        if (sessions.empty())
        {
            TextBlock empty;
            empty.Text(RS_(L"WorkspaceAgentSessionsEmpty"));
            empty.TextWrapping(TextWrapping::Wrap);
            panel.Append(empty);
            return;
        }

        std::ranges::sort(sessions, {}, &ContentManager::WorkspaceAgentSession::contentId);
        for (const auto& session : sessions)
        {
            using Microsoft::Terminal::TerminalConnection::ConnectionState;

            StackPanel details;
            details.Spacing(3);
            TextBlock name;
            name.Text(RS_fmt(L"WorkspaceAgentSessionName", std::wstring_view{ session.launch.providerDisplayName }, std::wstring_view{ session.launch.targetDisplayName }));
            name.TextWrapping(TextWrapping::Wrap);
            name.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
            details.Children().Append(name);

            TextBlock placement;
            placement.Text(session.kept ? RS_(L"WorkspaceAgentSessionBackground") :
                                          hstring{ RS_fmt(L"WorkspaceAgentSessionWindow", session.windowId) });
            placement.TextWrapping(TextWrapping::Wrap);
            details.Children().Append(placement);

            TextBlock state;
            if (session.state == ConnectionState::Connected)
            {
                state.Text(RS_(L"WorkspaceAgentSessionAttached"));
            }
            else if (session.state == ConnectionState::Closed || session.state == ConnectionState::Failed)
            {
                state.Text(session.launch.remote ? RS_(L"WorkspaceAgentSessionRemoteUnknown") :
                                                   RS_(L"WorkspaceAgentSessionDisconnected"));
            }
            else
            {
                state.Text(RS_(L"WorkspaceAgentSessionConnecting"));
            }
            state.TextWrapping(TextWrapping::Wrap);
            details.Children().Append(state);

            Button open;
            open.Content(details);
            open.HorizontalAlignment(HorizontalAlignment::Stretch);
            open.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            open.Tag(box_value(session.contentId));
            Automation::AutomationProperties::SetName(open, name.Text());
            open.Click([weak = get_weak(), id = session.contentId, pane = session.paneSessionId](auto&&, auto&&) {
                if (const auto page = weak.get())
                {
                    page->_FocusWorkspaceAgentSession(id, pane);
                }
            });
            panel.Append(open);
            if (focusedContent == session.contentId)
            {
                open.Focus(priorFocus);
            }
        }

        TextBlock unavailable;
        unavailable.Text(RS_(L"WorkspaceAgentSessionsMetricsUnavailable"));
        unavailable.TextWrapping(TextWrapping::Wrap);
        panel.Append(unavailable);
    }

    safe_void_coroutine TerminalPage::_FocusWorkspaceAgentSession(const uint64_t contentId, const winrt::guid paneId)
    {
        const auto lifetime = get_strong();
        const auto sessions = winrt::get_self<ContentManager>(_manager)->WorkspaceAgentSessions();
        const auto session = std::ranges::find(sessions, contentId, &ContentManager::WorkspaceAgentSession::contentId);
        if (session == sessions.end() || session->paneSessionId != paneId)
        {
            _UpdateWorkspaceAgentSessions();
            co_return;
        }

        try
        {
            const auto owner = session->owner.get();
            auto* page = this;
            if (!session->kept)
            {
                page = owner ? winrt::get_self<TerminalPage>(owner) : nullptr;
            }
            if (page && (co_await page->FocusProtocolPane(paneId)))
            {
                co_return;
            }
        }
        CATCH_LOG();
        _UpdateWorkspaceAgentSessions();
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
        title.Text(RS_(L"ConnectionsTitle"));
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
        Automation::AutomationProperties::SetName(tree, RS_(L"ConnectionsTitle"));

        std::vector<OpenSsh::SavedHost> hosts;
        std::vector<std::wstring> distributions;
        auto rows = std::make_shared<std::vector<ConnectionRow>>();
        auto discovery = std::make_shared<AgentDiscovery>();
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
        try
        {
            distributions = ::Microsoft::Terminal::Wsl::EnumerateDistributions();
            std::ranges::sort(distributions);
        }
        CATCH_LOG();

        const auto populateTree = [tree, hosts, distributions, rows, discovery](const hstring& filter) {
            rows->clear();
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
                rows->push_back({ node, ConnectionTarget{ host, {} } });
            }
            TreeControls::TreeViewNode wslGroup{ nullptr };
            for (const auto& distribution : distributions)
            {
                if (!_containsIgnoreCase(distribution, filter) && !_containsIgnoreCase(L"WSL", filter))
                {
                    continue;
                }
                if (!wslGroup)
                {
                    wslGroup = TreeControls::TreeViewNode{};
                    wslGroup.Content(box_value(hstring{ L"WSL" }));
                    wslGroup.IsExpanded(true);
                    tree.RootNodes().Append(wslGroup);
                }
                TreeControls::TreeViewNode node;
                node.Content(box_value(hstring{ distribution }));
                wslGroup.Children().Append(node);
                rows->push_back({ node, ConnectionTarget{ std::nullopt, distribution } });
            }
            for (const auto& [key, result] : discovery->results)
            {
                _refreshAgentNodes(*rows, key, *discovery);
            }
            if (!discovery->pending.empty())
            {
                _refreshAgentNodes(*rows, discovery->pending, *discovery);
            }
        };
        populateTree(query);
        const winrt::weak_ref<TreeControls::TreeView> weakTree{ tree };
        auto selectedHost = std::make_shared<std::optional<OpenSsh::SavedHost>>();
        const auto cancelOtherProbe = [rows, discovery](const std::wstring_view selectedKey) {
            if (discovery->cancelled && discovery->pending != selectedKey)
            {
                discovery->cancelled->store(true, std::memory_order_relaxed);
                discovery->cancelled.reset();
                const auto previous = std::exchange(discovery->pending, {});
                _refreshAgentNodes(*rows, previous, *discovery);
            }
        };
        const auto probeTarget = [weakThis{ get_weak() }, rows, discovery](const ConnectionTarget& target) {
            const auto page = weakThis.get();
            const auto key = target.key();
            const auto cached = discovery->results.find(key);
            if (!page || discovery->pending == key || (cached != discovery->results.end() && !cached->second.retryable))
            {
                return;
            }
            if (cached != discovery->results.end())
            {
                discovery->results.erase(cached);
            }
            if (target.host && !target.host->usesOpenSshConfig &&
                target.host->authentication == OpenSsh::HostAuthentication::Password)
            {
                discovery->results.emplace(key, AgentProbeResult{ {}, RS_(L"ConnectionAgentAuthenticationRequired") });
                _refreshAgentNodes(*rows, key, *discovery);
                return;
            }
            const auto wta = page->_DetectWtaPath();
            if (wta.empty())
            {
                discovery->results.emplace(key, AgentProbeResult{ {}, RS_(L"ConnectionAgentDiscoveryUnavailable"), true });
                _refreshAgentNodes(*rows, key, *discovery);
                return;
            }
            const auto cancelled = std::make_shared<std::atomic<bool>>(false);
            discovery->cancelled = cancelled;
            discovery->pending = key;
            _refreshAgentNodes(*rows, key, *discovery);
            _runAgentProbe(page->Dispatcher(), std::wstring{ wta }, target, cancelled,
                           [weakRows = std::weak_ptr{ rows }, weakDiscovery = std::weak_ptr{ discovery }, key, cancelled](AgentProbeResult result) {
                               const auto currentRows = weakRows.lock();
                               const auto currentDiscovery = weakDiscovery.lock();
                               if (currentRows && currentDiscovery && currentDiscovery->cancelled == cancelled)
                               {
                                   currentDiscovery->cancelled.reset();
                                   currentDiscovery->pending.clear();
                                   currentDiscovery->results.insert_or_assign(key, std::move(result));
                                   _refreshAgentNodes(*currentRows, key, *currentDiscovery);
                               }
                           });
        };
        const auto openRow = [weakThis{ get_weak() }](const ConnectionRow& row) {
            if (const auto page = weakThis.get())
            {
                const auto agent = row.agent ? row.agent->executable : std::wstring_view{};
                if (!agent.empty() && !AgentPolicy::IsAgentAllowed(agent))
                {
                    return;
                }
                if (row.target.host)
                {
                    page->_ConnectSshHost(*row.target.host, agent);
                }
                else
                {
                    page->_ConnectWslDistribution(row.target.distribution, agent);
                }
            }
        };
        const auto lookup = [rows](const TreeControls::TreeViewNode& node) -> std::optional<ConnectionRow> {
            const auto found = std::ranges::find(*rows, node, &ConnectionRow::node);
            return found == rows->end() ? std::nullopt : std::optional{ *found };
        };
        Button openSftp;
        openSftp.Content(box_value(L"SFTP"));
        openSftp.IsEnabled(false);
        openSftp.HorizontalAlignment(HorizontalAlignment::Stretch);
        MenuFlyout sftpActions;
        MenuFlyoutItem browseSftp;
        browseSftp.Text(RS_(L"SftpBrowseFiles"));
        browseSftp.Click([weak = get_weak(), selectedHost](auto&&, auto&&) {
            if (const auto page = weak.get(); page && *selectedHost)
            {
                page->_ShowSftpBrowser(**selectedHost);
            }
        });
        sftpActions.Items().Append(browseSftp);
        MenuFlyoutItem sftpTerminal;
        sftpTerminal.Text(RS_(L"SshHostsOpenSftp"));
        sftpTerminal.Click([weak = get_weak(), selectedHost](auto&&, auto&&) {
            if (const auto page = weak.get(); page && *selectedHost)
            {
                page->_ConnectSshHost(**selectedHost, {}, true);
            }
        });
        sftpActions.Items().Append(sftpTerminal);
        openSftp.Flyout(sftpActions);
        tree.RegisterPropertyChangedCallback(TreeControls::TreeView::SelectedItemProperty(), [weakTree, lookup, probeTarget, cancelOtherProbe, selectedHost, weakSftp = winrt::make_weak(openSftp)](auto&&, auto&&) {
            selectedHost->reset();
            if (const auto currentTree = weakTree.get())
            {
                const auto row = lookup(currentTree.SelectedItem().try_as<TreeControls::TreeViewNode>());
                cancelOtherProbe(row ? row->target.key() : std::wstring{});
                if (row && !row->agent)
                {
                    *selectedHost = row->target.host;
                    probeTarget(row->target);
                }
            }
            if (const auto button = weakSftp.get())
            {
                button.IsEnabled(selectedHost->has_value());
            }
        });
        const auto connectSelected = [weakTree, lookup, openRow]() {
            if (const auto currentTree = weakTree.get())
            {
                if (const auto row = lookup(currentTree.SelectedItem().try_as<TreeControls::TreeViewNode>()))
                {
                    openRow(*row);
                }
            }
        };
        const auto connectFromRow = [weakTree, lookup, openRow](const Windows::Foundation::IInspectable& originalSource) {
            const auto currentTree = weakTree.get();
            if (!currentTree)
            {
                return false;
            }
            const auto treeElement = currentTree.as<Windows::UI::Xaml::DependencyObject>();
            for (auto source = originalSource.try_as<Windows::UI::Xaml::DependencyObject>(); source && source != treeElement;
                 source = Windows::UI::Xaml::Media::VisualTreeHelper::GetParent(source))
            {
                if (const auto item = source.try_as<TreeControls::TreeViewItem>())
                {
                    if (const auto row = lookup(currentTree.NodeFromContainer(item)))
                    {
                        openRow(*row);
                        return true;
                    }
                    return false;
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
        noMatches.Visibility(!query.empty() && rows->empty() ? Visibility::Visible : Visibility::Collapsed);
        Automation::AutomationProperties::SetLiveSetting(noMatches, Automation::Peers::AutomationLiveSetting::Polite);
        Grid::SetRow(noMatches, 2);
        panel.Children().Append(tree);
        panel.Children().Append(noMatches);
        search.TextChanged([populateTree, selectedHost, rows, noMatches](const Windows::Foundation::IInspectable& sender, auto&&) {
            selectedHost->reset();
            const auto filter = sender.as<TextBox>().Text();
            populateTree(filter);
            noMatches.Visibility(!filter.empty() && rows->empty() ? Visibility::Visible : Visibility::Collapsed);
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
        actions.Children().Append(openSftp);

        Button importAliases;
        importAliases.Content(box_value(RS_(L"SshHostsImportAliases")));
        importAliases.HorizontalAlignment(HorizontalAlignment::Stretch);
        importAliases.IsEnabled(hostsLoaded);
        importAliases.Click([weak = get_weak(), status](const winrt::Windows::Foundation::IInspectable& sender, auto&&) {
            if (const auto page = weak.get())
            {
                page->_PreviewOpenSshAliases(sender.as<Button>(), status);
            }
        });
        actions.Children().Append(importAliases);
        HyperlinkButton sessions;
        sessions.Content(box_value(RS_(L"WorkspaceAgentSessionsTitle")));
        sessions.Click([weak = get_weak()](auto&&, auto&&) {
            if (const auto page = weak.get())
            {
                page->_ToggleWorkspaceAgentSessions();
            }
        });
        actions.Children().Append(sessions);
        actions.Children().Append(status);
        panel.Children().Append(actions);
        SshHostsDock().Children().Append(panel);
    }

    safe_void_coroutine TerminalPage::_PreviewOpenSshAliases(Button button, TextBlock status)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = Dispatcher();
        button.IsEnabled(false);
        status.Text(RS_(L"SshHostsImportReading"));

        co_await winrt::resume_background();
        std::vector<std::wstring> aliases;
        bool readFailed = false;
        try
        {
            aliases = OpenSsh::LoadConfigAliases();
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            readFailed = true;
        }
        co_await wil::resume_foreground(dispatcher);

        if (_windowCloseAccepted || _windowPanesShutdown)
        {
            co_return;
        }
        button.IsEnabled(true);
        status.Text(readFailed ? RS_(L"SshHostsImportReadFailed") : winrt::hstring{});
        if (readFailed)
        {
            co_return;
        }

        const auto weakThis = get_weak();
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

        if (const auto presenter = _dialogPresenter.get())
        {
            presenter.ShowDialog(dialog);
        }
    }

    bool TerminalPage::_OpenConnectionTab(const std::wstring_view commandline, const std::wstring_view title, const WorkspaceAgentLaunch* workspaceAgent)
    {
        using namespace winrt::Microsoft::Terminal::Settings::Model;
        NewTerminalArgs args;
        args.Commandline(hstring{ commandline });
        args.TabTitle(hstring{ title });
        args.StartingDirectory(L"%USERPROFILE%");
        args.Elevate(false);

        const auto azureType = Microsoft::Terminal::TerminalConnection::AzureConnection::ConnectionType();
        const auto profile = _settings.GetProfileForArgs(args);
        if (profile && ((profile.ConnectionType() == azureType && Microsoft::Terminal::TerminalConnection::AzureConnection::IsAzureConnectionAvailable()) ||
                        (workspaceAgent && !profile.AgentProfileId().empty())))
        {
            for (const auto& candidate : _settings.ActiveProfiles())
            {
                if (candidate.ConnectionType() != azureType && (!workspaceAgent || candidate.AgentProfileId().empty()))
                {
                    args.Profile(::Microsoft::Console::Utils::GuidToString(candidate.Guid()));
                    break;
                }
            }
            if (args.Profile().empty())
            {
                return false;
            }
        }
        return _OpenNewTab(args, false, workspaceAgent) == S_OK;
    }

    void TerminalPage::_ConnectWslDistribution(const std::wstring_view distribution, const std::wstring_view agent)
    {
        try
        {
            std::wstring systemDirectory;
            THROW_IF_FAILED(wil::GetSystemDirectoryW<std::wstring>(systemDirectory));
            std::wstring command;
            til::append_quoted_argument(command, systemDirectory.append(L"\\wsl.exe"));
            command.append(L" --distribution");
            _appendArgument(command, distribution);
            command.append(L" --cd ~");
            auto title = std::wstring{ distribution };
            if (!agent.empty())
            {
                command.append(L" --exec sh -lc");
                _appendArgument(command, std::wstring{ L"exec " }.append(agent));
                title.append(L" - ").append(agent);
            }
            const auto launch = _workspaceAgentLaunch(agent, distribution, distribution, false);
            if (_OpenConnectionTab(command, title, agent.empty() ? nullptr : &launch))
            {
                return;
            }
        }
        CATCH_LOG();

        if (const auto presenter = _dialogPresenter.get())
        {
            ContentDialog dialog;
            dialog.Title(box_value(RS_(L"ConnectionsTitle")));
            dialog.Content(box_value(RS_(L"ConnectionTerminalLaunchFailed")));
            dialog.CloseButtonText(RS_(L"SshHostsClose"));
            presenter.ShowDialog(dialog);
        }
    }

    void TerminalPage::_ConnectSshHost(const OpenSsh::SavedHost& host, const std::wstring_view agent, const bool sftp)
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
                if (!current->usesOpenSshConfig && current->authentication == OpenSsh::HostAuthentication::IdentityFile &&
                    !std::filesystem::is_regular_file(std::filesystem::path{ current->identityFile }, identityError))
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
                    auto title = current->name;
                    std::wstring command;
                    if (sftp)
                    {
                        const auto sshPath = wil::ExpandEnvironmentStringsW<std::wstring>(executable.data());
                        auto sftpExecutable = std::filesystem::path{ sshPath };
                        sftpExecutable.replace_filename(L"sftp.exe");
                        std::error_code sftpError;
                        if (std::filesystem::is_regular_file(sftpExecutable, sftpError))
                        {
                            command = OpenSsh::BuildSftpCommandline(sftpExecutable.native(), sshPath, *current);
                            title.append(L" - SFTP");
                        }
                        else
                        {
                            error = RS_(L"SshHostsSftpMissing");
                        }
                    }
                    else
                    {
                        command = OpenSsh::BuildCommandline(executable, *current, agent);
                        if (!agent.empty())
                        {
                            title.append(L" - ").append(agent);
                        }
                    }
                    const auto launch = _workspaceAgentLaunch(agent, current->id, current->name, true);
                    if (error.empty() && !_OpenConnectionTab(command, title, agent.empty() ? nullptr : &launch))
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
        HWND owner{};
        const auto presenter = [&]() -> IDialogPresenter {
            if (const auto page = weak.get())
            {
                owner = page->_hostingHwnd.value_or(nullptr);
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
            const auto editor = std::make_shared<SshHostsDialog>(
                std::filesystem::path{ CascadiaSettings::SettingsDirectory().c_str() }, std::move(host), owner);
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
