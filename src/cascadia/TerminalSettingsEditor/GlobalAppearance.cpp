// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "GlobalAppearance.h"
#include "GlobalAppearance.g.cpp"

#include "MainPage.h"
#include "../WinRTUtils/inc/Utils.h"

#include <WtExeUtils.h>
#include <til/io.h>

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Navigation;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Windows::Foundation::Collections;

namespace winrt::Microsoft::Terminal::Settings::Editor::implementation
{
    GlobalAppearance::GlobalAppearance()
    {
        InitializeComponent();
    }

    void GlobalAppearance::OnNavigatedTo(const NavigationEventArgs& e)
    {
        const auto args = e.Parameter().as<Editor::NavigateToPageArgs>();
        _ViewModel = args.ViewModel().as<Editor::GlobalAppearanceViewModel>();
        _windowRoot = args.WindowRoot();
        BringIntoViewWhenLoaded(args.ElementToFocus());

        TraceLoggingWrite(
            g_hTerminalSettingsEditorProvider,
            "NavigatedToPage",
            TraceLoggingDescription("Event emitted when the user navigates to a page in the settings UI"),
            TraceLoggingValue("globalAppearance", "PageId", "The identifier of the page that was navigated to"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));
    }

    safe_void_coroutine GlobalAppearance::_ImportTerminalAppearanceClick(const IInspectable&, const RoutedEventArgs&)
    {
        const auto lifetime = get_strong();
        const auto windowRoot = _windowRoot.get();
        const auto mainPage = windowRoot.try_as<Editor::MainPage>();
        if (!mainPage || !windowRoot.GetHostingWindow())
        {
            co_return;
        }
        const auto mainPageImpl = winrt::get_self<MainPage>(mainPage);
        const auto target = mainPageImpl->AppearanceImportTarget();

        static constexpr COMDLG_FILTERSPEC fileTypes[] = {
            { L"JSON (*.json)", L"*.json" },
        };
        static constexpr winrt::guid pickerId{ 0xcd08a8e8, 0x0174, 0x425e, { 0xb5, 0x16, 0x87, 0x47, 0x76, 0x40, 0x63, 0xd9 } };

        const auto dispatcher = Dispatcher();
        const auto importButton = ImportTerminalAppearanceButton();
        importButton.IsEnabled(false);
        const auto reenable = wil::scope_exit([&] {
            if (dispatcher.HasThreadAccess())
            {
                importButton.IsEnabled(true);
            }
        });
        ImportTerminalAppearanceStatus().Text(L"");

        try
        {
            const auto owner = reinterpret_cast<HWND>(windowRoot.GetHostingWindow());
            const auto path = co_await OpenFilePicker(owner, [](auto&& dialog) {
                THROW_IF_FAILED(dialog->SetClientGuid(pickerId));
                THROW_IF_FAILED(dialog->SetFileTypes(ARRAYSIZE(fileTypes), fileTypes));
                THROW_IF_FAILED(dialog->SetDefaultExtension(L"json"));
            });
            if (path.empty())
            {
                co_return;
            }

            co_await winrt::resume_background();

            winrt::hstring source;
            try
            {
                source = winrt::to_hstring(til::io::read_file_as_utf8_string_if_exists(std::filesystem::path{ path.c_str() }));
            }
            CATCH_LOG();

            co_await wil::resume_foreground(dispatcher);
            const auto imported = !source.empty() && mainPageImpl->ImportTerminalAppearance(target, source);
            ImportTerminalAppearanceStatus().Text(imported ? RS_(L"ImportTerminalAppearanceSuccess") : RS_(L"ImportTerminalAppearanceError"));
        }
        catch (...)
        {
            LOG_CAUGHT_EXCEPTION();
            if (dispatcher.HasThreadAccess())
            {
                ImportTerminalAppearanceStatus().Text(RS_(L"ImportTerminalAppearanceError"));
            }
        }
    }
}
