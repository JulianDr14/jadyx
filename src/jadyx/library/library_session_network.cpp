// SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "jadyx/library/library_session.h"

namespace Jadyx {
using I18n::Tr;

// Every fault is kept in jx_portal with the adapters of the moment. The user reads what
// happened, or that the network itself needs a look once it keeps failing.
void LibrarySession::ReportPortalFault(std::string_view what, PortalError error, const PortalAddress& address,
                                       unsigned short port, int os_error) {
    const auto failures = portal_faults.Record({.what = what, .error = error, .address = address, .port = port,
                                                .spare_ports = portal_spare_ports, .os_error = os_error});
    const std::wstring ip{winrt::to_hstring(address.ipv4)};
    const auto first = std::to_wstring(port), last = std::to_wstring(port + portal_spare_ports);
    std::wstring message;
    if (failures >= PortalFaultLog::RepeatedFailures)
        message = I18n::Format(T::NetworkRepeatedFailures, {std::to_wstring(failures)});
    else if (what == "stopped" && error == PortalError::AddressLost)
        message = I18n::Format(T::NetworkPortalLost, {ip});
    else switch (error) {
        case PortalError::NoAddress: message = Tr(T::NetworkUnavailable); break;
        case PortalError::PortInUse: message = I18n::Format(T::NetworkPortInUse, {first, last}); break;
        case PortalError::PortBlocked: message = I18n::Format(T::NetworkPortBlocked, {first, last}); break;
        case PortalError::AddressLost:
            message = I18n::Format(address.link == PortalLink::WiFi ? T::NetworkWifiLost : T::NetworkAddressLost, {ip});
            break;
        case PortalError::Bind: message = I18n::Format(T::NetworkBindFailed, {first, std::to_wstring(os_error)}); break;
        default:
            message = I18n::Format(T::NetworkStartFailedCode, {winrt::to_hstring(PortalErrorName(error)).c_str()});
    }
    configuration.notice = message + L" " + std::wstring{Tr(T::NetworkReportSaved)};
}

// Reads the console's addresses again, keeping the chosen one when it is still there.
// Returns the address that was chosen before, empty when there was none.
PortalAddress LibrarySession::RefreshAddresses() {
    const auto chosen = configuration.network_addresses.empty() ? PortalAddress{} :
        configuration.network_addresses[configuration.network_selected];
    configuration.network_addresses = PortalAddresses();
    configuration.network_selected = 0;
    for (size_t i = 0; i < configuration.network_addresses.size(); ++i)
        if (configuration.network_addresses[i].ipv4 == chosen.ipv4) configuration.network_selected = i;
    return chosen;
}

// Starts the portal on the chosen address, or says why not. Never logs the pairing code.
bool LibrarySession::StartPortal() {
    // The list was read when Settings opened; a Wi-Fi reconnection may have changed it since.
    // When the address went away while binding, the list is read once more.
    for (unsigned attempt = 0;; ++attempt) {
        const auto chosen = RefreshAddresses();
        if (configuration.network_addresses.empty()) {
            ReportPortalFault("start", PortalError::NoAddress, chosen, portal_port, 0);
            return false;
        }
        const auto address = configuration.network_addresses[configuration.network_selected];
        if (!chosen.ipv4.empty() && chosen.ipv4 != address.ipv4)
            LOG_INFO(Frontend, "Network portal: {} {} is gone, using {} {}", chosen.name, chosen.ipv4,
                     address.name, address.ipv4);
        const bool started = network_portal.Start(address.ipv4, portal_port, portal_spare_ports);
        const auto status = network_portal.Status();
        if (started) {
            portal_address = address;
            portal_faults.Recovered(address, status.port);
            configuration.notice = address.link_local ?
                std::wstring{winrt::to_hstring(address.ipv4)} + L": " + std::wstring{Tr(T::NetworkLinkLocal)} :
                std::wstring{};
            LOG_INFO(Frontend, "Network portal: listening on {} {} port {}", address.name, address.ipv4, status.port);
            return true;
        }
        if (status.error == PortalError::AddressLost && attempt == 0) continue;
        ReportPortalFault("start", status.error, address, status.port ? status.port : portal_port, status.os_error);
        return false;
    }
}

// Without a native deployment (Xbox) the install is guided from a browser: its screen opens
// over the update page with the portal already listening.
void LibrarySession::OpenUpdateGuide() {
    updater.RecordAssistedRequest();
    if (!network_portal.Status().running) StartPortal();
    else configuration.notice.clear();
    configuration.network = network_portal.Status();
    configuration.network_screen = configuration.network_update = true;
    configuration.network_action = 0;
    dirty = true;
}

void LibrarySession::PollNetwork(TimePoint now) {
    if (now >= network_poll_at) {
        auto update = updater.Status();
        if (update.revision != system_page.update.revision) dirty = true;
        system_page.update = std::move(update);
        if (system_page.updates_open && system_page.update.phase == Updates::Phase::Ready) {
            const bool storage_idle = !configuration.busy && !picker && !validation.valid() &&
                !content_session && !loading && !setup_future.valid() &&
                network_portal.BlockForLocalOperation();
            updater.InstallIfReady(storage_idle);
            if (updater.CurrentPhase() == Updates::Phase::Assistance) OpenUpdateGuide();
        }
        network_poll_at = now + std::chrono::milliseconds{500};
        network_portal.SetUpdateOffered(system_page.update.phase == Updates::Phase::Assistance);
        auto state = network_portal.Status();
        // The listener died on its own, or the console left the network it listens on (the
        // Wi-Fi dropped, the cable came out, the router gave another address): say so.
        if (configuration.network.running && !state.running && state.error != PortalError::None) {
            ReportPortalFault("stopped", state.error, portal_address, configuration.network.port, 0);
            dirty = true;
        } else if (state.running && now >= portal_check_at) {
            portal_check_at = now + std::chrono::seconds{3};
            if (!PortalAddressOwned(portal_address.ipv4)) {
                network_portal.Stop();
                state = network_portal.Status();
                ReportPortalFault("stopped", PortalError::AddressLost, portal_address, configuration.network.port, 0);
                RefreshAddresses();
                dirty = true;
            }
        }
        if (state.running) portal_library.Publish(scan.entries, static_cast<std::uint32_t>(title_language));
        if (configuration.network.running != state.running || configuration.network.paired != state.paired ||
            configuration.network.code != state.code || configuration.network.code_expired != state.code_expired ||
            configuration.network.error != state.error ||
            configuration.network.transfer.received != state.transfer.received ||
            configuration.network.transfer.phase != state.transfer.phase ||
            configuration.network.transfer.id != state.transfer.id ||
            configuration.network.transfer.error != state.transfer.error ||
            configuration.network.content.received != state.content.received ||
            configuration.network.content.phase != state.content.phase ||
            configuration.network.content.error != state.content.error ||
            configuration.network.removal != state.removal ||
            configuration.network.system.phase != state.system.phase ||
            configuration.network.system.processed != state.system.processed ||
            configuration.network.system.file.received != state.system.file.received ||
            configuration.network.system.error != state.system.error) {
            configuration.network = state;
            dirty = true;
        }
        const bool system_pending=state.system.phase=="prepare_pending"||state.system.phase=="commit_pending";
        const std::optional<StorageJob> next_job = removal_request ? StorageJob::Removal :
            system_pending ? StorageJob::PortalSystem : state.removal.phase == "pending" ? StorageJob::PortalRemoval :
            state.transfer.phase == "ready" ? StorageJob::PortalGame :
            state.content.phase == "ready" ? std::optional{StorageJob::PortalContent} : std::nullopt;
        if (next_job && !configuration.busy && !loading &&
            !setup_future.valid() && !picker && !validation.valid() && !content_session && !game_options.open) {
            StartStorageJob(*next_job, state);
        }
    }
}

void LibrarySession::StartStorageJob(StorageJob next, const PortalStatus& state) {
    configuration.busy=true;
    storage_job=next;
    import_progress.completed=0; import_progress.total=0;
    portal_library.Progress(0, 0);
    if (storage_job == StorageJob::Removal || storage_job == StorageJob::PortalRemoval) notice = Tr(T::GameRemoving);
    metadata_worker.request_stop();
    // An update/DLC from the portal goes through the content manager's import.
    const auto install=[&](const std::filesystem::path& path, const std::string& tag, std::stop_token stop) {
        return portal_library.Install(path, tag, stop, [&](std::uint64_t done, std::uint64_t total) {
            import_progress.completed=static_cast<unsigned>(done >> 20);
            import_progress.total=static_cast<unsigned>(total >> 20);
        });
    };
    std::packaged_task<std::string(std::stop_token)> task{
        [&, job=storage_job, install, removal=removal_request.value_or(GameFiles{}),
         old_metadata=std::move(metadata_worker)](std::stop_token stop) mutable {
            std::stop_callback cancel{stop,[&] { network_portal.CancelTransfer(); }};
            if (old_metadata.joinable()) old_metadata.join();
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            struct Uninitialize { ~Uninitialize() { winrt::uninit_apartment(); } } guard;
            switch (job) {
            case StorageJob::Removal: return RemoveGame(root, removal);
            case StorageJob::PortalSystem: return network_portal.RunSystemImport(stop);
            case StorageJob::PortalRemoval:
                return network_portal.RunRemoval([&](const PortalRemoval& r) { return portal_library.Remove(r); });
            case StorageJob::PortalContent: return network_portal.FinalizeContent(install);
            default: return network_portal.FinalizeGame(ValidateUploadedGame);
            }
        }};
    // Keys and firmware stay as they are: the rescan keeps every other game's titles.
    keep_titles = storage_job == StorageJob::PortalContent ? std::optional{ContentTagApplication(state.content.tag)} :
        storage_job == StorageJob::PortalRemoval ? std::optional{state.removal.application} :
        storage_job == StorageJob::Removal ? std::optional{removal_request->application} :
        storage_job == StorageJob::PortalGame ? std::optional<std::uint64_t>{0} : std::nullopt;
    removal_request.reset();
    import_future=task.get_future();
    import_worker=std::jthread(std::move(task));
}

void LibrarySession::HandleImportConfirm() {
    if(import_progress.confirmation.load()&&!configuration.confirm_import){
        std::lock_guard lock{import_progress.review_mutex};configuration.review_text=import_progress.review_text;
        configuration.confirm_import=true;configuration.network_action=0;dirty=true;
    }
    if(configuration.confirm_import){
        if(actions&(Left|Right|Up|Down))configuration.network_action^=1;
        if(actions&Quit || actions&Play){
            import_progress.decision=(actions&Quit)||configuration.network_action==0?2:1;
            import_progress.confirmation=false;
            import_progress.decision.notify_all();configuration.confirm_import=false;dirty=true;
        }
        dirty|=actions!=0;actions=0;pressed_tab.reset();
    }
}

void LibrarySession::HandleNetworkScreen() {
    if (configuration.network_screen) {
        if (!configuration.network_update) {
            // The portal's two rows (code, stop); going back is B or the footer.
            if (actions & (Left | Right | Up | Down))
                configuration.network_action = configuration.network_action == 1 ? 2 : 1;
        } else {
            if (actions & (Left | Up)) configuration.network_action = (configuration.network_action + 2) % 3;
            if (actions & (Right | Down)) configuration.network_action = (configuration.network_action + 1) % 3;
        }
        if (actions & Quit || (actions & Play && configuration.network_action == 0))
            configuration.network_screen = false;
        else if (actions & Refresh || (actions & Play && configuration.network_action == 1)) {
            // The update guide stays open when its portal failed to start: this retries it.
            if (configuration.network_update && !network_portal.Status().running) StartPortal();
            else configuration.notice = network_portal.RenewCode() ? std::wstring{} : std::wstring{Tr(T::NetworkStartFailed)};
            configuration.network = network_portal.Status();
        } else if (actions & Play && configuration.network_action == 2) {
            network_portal.Stop();
            configuration.network = network_portal.Status();
            configuration.network_screen = false;
            if (!configuration.network_update) configuration.selected = FilesNetwork;
            configuration.notice.clear();
        }
        if (!configuration.network.running && !configuration.network_update) configuration.network_screen = false;
        if (!configuration.network_screen) configuration.network_update = false;
        dirty |= actions != 0;
        actions = 0;
        pressed_tab.reset();
    }
}

void LibrarySession::PollImport() {
    if (configuration.busy) {
        if (actions & Quit) { import_worker.request_stop(); configuration.notice = Tr(T::CancellingImport); }
        actions = 0;
        const auto done = import_progress.completed.load(std::memory_order_relaxed);
        const auto total = import_progress.total.load(std::memory_order_relaxed);
        if (done != configuration.completed || total != configuration.total) {
            configuration.completed = done; configuration.total = total; dirty = true;
        }
        if (import_future.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
            const bool removal = storage_job == StorageJob::Removal || storage_job == StorageJob::PortalRemoval;
            try {
                const auto error = import_future.get();
                if (removal) {
                    const bool content = storage_job == StorageJob::PortalRemoval &&
                        !network_portal.Status().removal.content.empty();
                    after_scan = Tr(!error.empty() ? (error == "external" ? T::GameRemoveExternal : T::GameRemoveFailed) :
                                    content ? T::ContentRemoved :
                                    storage_job == StorageJob::Removal && removed_copy ? T::GameCopyRemoved : T::GameRemoved);
                } else {
                    configuration.notice = error.empty() ? std::wstring{Tr(storage_job==StorageJob::PortalSystem&&network_portal.Status().system.phase!="complete"?T::SystemReviewReady:T::ImportDone)} :
                        storage_job != StorageJob::Files ? std::wstring{Tr(T::ImportFailedKept)} : Wide(error);
                }
            } catch (...) {
                if (removal) after_scan = Tr(T::GameRemoveFailed);
                else configuration.notice = Tr(T::ImportFailedKept);
            }
            configuration.busy = false;
            portal_library.Progress(0, 0);
            configuration.confirm_import=false;
            // The old metadata worker has joined; discard results before rescanning.
            metadata = {};
            RefreshSetup();
            StartScan();
            dirty = true;
        }
    }
}

void LibrarySession::HandleUpdatesPage() {
    if (system_page.updates_open) {
        const auto& snapshot = system_page.update;
        const auto items = Updates::Actions(snapshot);
        // B, and X like everywhere a page closes, leave; Y does nothing here.
        const bool leave = actions & (Quit | Swap | Refresh | AddFolder);
        if (system_page.update_detail) {
            if (actions & Up && system_page.update_scroll) --system_page.update_scroll;
            if (actions & Down) ++system_page.update_scroll;
            if (leave || actions & Play) { system_page.update_detail=0; system_page.update_scroll=0; }
            if (actions) dirty=true;
            actions=0;
        }
        if (!items.empty()) {
            system_page.update_row = std::min(system_page.update_row, items.size()-1);
            if (actions & Up) system_page.update_row = (system_page.update_row + items.size()-1)%items.size();
            if (actions & Down) system_page.update_row = (system_page.update_row+1)%items.size();
        }
        if (actions && leave) system_page.updates_open = false;
        else if (actions & Play && !items.empty()) {
            using A = Updates::Action;
            const auto action = items[system_page.update_row];
            // Reserve local storage admission atomically against incoming browser uploads.
            const bool mutation = action == A::Download || action == A::Install || action == A::Cancel;
            if (mutation && (configuration.busy || picker || validation.valid() || content_session || !network_portal.BlockForLocalOperation())) {
                system_page.update.error = Updates::Error::Busy;
            } else switch (action) {
            case A::Notes: system_page.update_detail=1; system_page.update_scroll=0; break;
            case A::Check: updater.Check(true); break;
            case A::Channel: updater.SetChannel(snapshot.channel == Updates::Channel::Stable ? Updates::Channel::Preview : Updates::Channel::Stable); break;
            case A::Download: updater.Download(true); break;
            case A::Pause: updater.Pause(); break;
            case A::Cancel: updater.Cancel(); break;
            case A::Install:
                if (snapshot.phase == Updates::Phase::Ready) updater.Install();
                if (updater.CurrentPhase() == Updates::Phase::Assistance) OpenUpdateGuide();
                break;
            case A::Later: updater.Dismiss(false); system_page.updates_open = false; break;
            case A::Skip: updater.Dismiss(true); system_page.updates_open = false; break;
            }
            // Notes and Channel keep their row; the others change which rows there are.
            if (action != A::Notes && action != A::Channel) system_page.update_row = 0;
        }
        if (actions) dirty = true;
        actions = 0; pressed_tab.reset();
    }
}

    // While the new-version notice shows on Home or the Library, Y opens the update page and
    // X hides the notice until tomorrow; neither pins nor rescans then.
void LibrarySession::HandleUpdateNotice() {
    if (system_page.update.notice && system_page.update.release && !system_page.updates_open && !settings &&
        !configuration.open && !configuration.network_screen && !game_options.open && !missing_content &&
        !panel.BindingEditorOpen() && tab != LibraryTab::Settings && actions & (Pin | Refresh)) {
        if (actions & Pin) { system_page.updates_open = true; system_page.update_row = 0; }
        else { updater.Dismiss(false); system_page.update.notice = false; }
        actions &= ~(Pin | Refresh);
        dirty = true;
    }
}

} // namespace Jadyx
