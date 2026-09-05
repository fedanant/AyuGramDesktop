/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ayu/mentions/unread_mentions_model.h"

#include "api/api_unread_things.h"
#include "apiwrap.h"
#include "data/data_changes.h"
#include "data/data_folder.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "dialogs/dialogs_main_list.h"
#include "dialogs/dialogs_row.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_unread_things.h"
#include "main/main_session.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Ayu {
namespace {

constexpr auto kMaxConcurrentLoads = 4;
constexpr auto kDiscoveryRetryDelays = std::array{
	crl::time(1000),
	crl::time(2000),
	crl::time(4000),
	crl::time(8000),
};
constexpr auto kDiscoveryRetryLimit = int(kDiscoveryRetryDelays.size()) - 1;

} // namespace

struct UnreadMentionsModel::Discovery {
	struct Loaded {
		FullMsgId id;
		TimeId date = 0;
	};

	base::flat_map<PeerId, not_null<History*>> histories;
	base::flat_set<PeerId> visitedHistories;
	base::flat_set<FolderId> visitedFolders;
	base::flat_set<FullMsgId> visitedMessages;
	std::vector<Loaded> loaded;
	int count = 0;
	bool complete = true;
	bool listsComplete = true;
};

UnreadMentionsModel::UnreadMentionsModel(
		not_null<Main::Session*> session)
: _session(session)
, _discoveryRetryTimer([=] { handleDiscoveryRetry(); }) {
	setupSubscriptions();
	refresh(true);
}

UnreadMentionsModel::~UnreadMentionsModel() = default;

UnreadMentionsState UnreadMentionsModel::stateCurrent() const {
	return _state.current();
}

rpl::producer<UnreadMentionsState> UnreadMentionsModel::stateValue() const {
	return _state.value();
}

auto UnreadMentionsModel::markAllResults() const
		-> rpl::producer<MarkAllMentionsResult> {
	return _markAllResults.events();
}

void UnreadMentionsModel::setupSubscriptions() {
	auto refresh = [=] {
		this->refresh();
	};
	auto dialogsRefresh = [=] {
		handleDialogsChange();
	};
	_session->data().unreadBadgeChanges(
	) | rpl::on_next(refresh, _lifetime);
	rpl::merge(
		_session->data().chatsListChanges(),
		_session->data().chatsListLoadedEvents()
	) | rpl::on_next(dialogsRefresh, _lifetime);
	_session->changes().realtimeHistoryUpdates(
		Data::HistoryUpdate::Flag::UnreadMentions
	) | rpl::on_next(refresh, _lifetime);
	_session->api().unreadThings().mentionsLoadResults(
	) | rpl::on_next([=](const Api::MentionsLoadResult &result) {
		handleLoadResult(result);
	}, _lifetime);
	_session->data().itemRemoved(
	) | rpl::on_next(refresh, _lifetime);
	_session->data().itemDataChanges(
	) | rpl::filter([=](not_null<HistoryItem*> item) {
		return itemAffectsModel(item);
	}) | rpl::on_next(refresh, _lifetime);
}

void UnreadMentionsModel::refresh() {
	refresh(false);
}

void UnreadMentionsModel::refresh(bool requestIncompleteDialogs) {
	discover(requestIncompleteDialogs);
	if (_loading && !_retrying) {
		queueNewlyDiscovered();
	}
	pumpLoads();
	finishLoadingIfDone();
	publish();
}

void UnreadMentionsModel::handleDialogsChange() {
	const auto resumeLoading = _discoveryFailed
		&& _resumeLoadingAfterDiscoveryFailure;
	resetDiscoveryRetry();
	if (resumeLoading) {
		_loading = true;
		_retrying = false;
	}
	refresh(true);
}

void UnreadMentionsModel::discover(bool requestIncompleteDialogs) {
	auto result = Discovery();
	discoverFolder(nullptr, result, requestIncompleteDialogs);
	std::sort(
		result.loaded.begin(),
		result.loaded.end(),
		[](const Discovery::Loaded &a, const Discovery::Loaded &b) {
			return (a.date != b.date) ? (a.date > b.date) : (a.id > b.id);
		});

	_histories = std::move(result.histories);
	_loadedIds.clear();
	_loadedIdSet.clear();
	_loadedIds.reserve(result.loaded.size());
	for (const auto &loaded : result.loaded) {
		_loadedIds.push_back(loaded.id);
		_loadedIdSet.emplace(loaded.id);
	}
	_count = result.complete ? result.count : 0;
	_complete = result.complete;
	_listsComplete = result.listsComplete;

	const auto failed = _failedPeers;
	for (const auto peerId : failed) {
		if (!needsLoad(peerId)) {
			_failedPeers.remove(peerId);
		}
	}
	const auto stalled = _stalledPeers;
	for (const auto peerId : stalled) {
		if (!needsLoad(peerId)) {
			_stalledPeers.remove(peerId);
		}
	}
	if (_listsComplete) {
		resetDiscoveryRetry();
	} else {
		scheduleDiscoveryRetry();
	}
}

void UnreadMentionsModel::discoverFolder(
		Data::Folder *folder,
		Discovery &result,
		bool requestIncompleteDialogs) {
	const auto folderId = folder ? folder->id() : FolderId();
	if (!result.visitedFolders.emplace(folderId).second) {
		return;
	}
	const auto list = _session->data().chatsList(folder);
	if (!list->loaded()) {
		if (requestIncompleteDialogs) {
			_session->api().requestDialogs(folder);
		}
		result.complete = false;
		result.listsComplete = false;
	}
	for (const auto row : list->indexed()->all()) {
		if (const auto history = row->history()) {
			const auto peerId = history->peer->id;
			if (result.visitedHistories.emplace(peerId).second
				&& _session->api().unreadThings().trackMentions(history)) {
				result.histories.emplace(peerId, history);
				const auto count = history->unreadMentions().count();
				if (count < 0) {
					result.complete = false;
				} else {
					result.count += count;
				}
				for (const auto msgId : history->unreadMentionsIds()) {
					const auto fullId = FullMsgId(peerId, msgId);
					if (!result.visitedMessages.emplace(fullId).second) {
						continue;
					}
					const auto item = _session->data().message(fullId);
					if (item && item->isUnreadMention()) {
						result.loaded.push_back({
							.id = fullId,
							.date = item->date(),
						});
					}
				}
			}
		}
		if (const auto child = row->folder()) {
			discoverFolder(child, result, requestIncompleteDialogs);
		}
	}
}

void UnreadMentionsModel::scheduleDiscoveryRetry() {
	if (_listsComplete
		|| _discoveryFailed
		|| _discoveryRetryTimer.isActive()) {
		return;
	}
	const auto index = std::min(
		_discoveryRetryAttempt,
		kDiscoveryRetryLimit);
	_discoveryRetryTimer.callOnce(kDiscoveryRetryDelays[index]);
}

void UnreadMentionsModel::handleDiscoveryRetry() {
	if (_listsComplete || _discoveryFailed) {
		return;
	}
	if (_discoveryRetryAttempt >= kDiscoveryRetryLimit) {
		_discoveryFailed = true;
		_resumeLoadingAfterDiscoveryFailure = _loading;
		_loading = false;
		_retrying = false;
		_seenLoadPeers.clear();
		_queuedPeers.clear();
		_pendingPeers.clear();
		publish();
		return;
	}
	++_discoveryRetryAttempt;
	discover(true);
	if (_loading && !_retrying) {
		queueNewlyDiscovered();
	}
	pumpLoads();
	finishLoadingIfDone();
	publish();
}

void UnreadMentionsModel::resetDiscoveryRetry() {
	_discoveryRetryTimer.cancel();
	_discoveryRetryAttempt = 0;
	_discoveryFailed = false;
	_resumeLoadingAfterDiscoveryFailure = false;
}

void UnreadMentionsModel::publish() {
	_state = UnreadMentionsState{
		.count = _count,
		.loadingPeers = int(_inFlightPeers.size()),
		.complete = _complete,
		.discoveryFailed = _discoveryFailed,
		.loading = _loading,
		.retrying = _retrying,
		.markingAll = _markingAll,
		.loadedIds = _loadedIds,
		.failedPeers = _failedPeers,
		.stalledPeers = _stalledPeers,
	};
}

bool UnreadMentionsModel::needsLoad(PeerId peerId) const {
	const auto i = _histories.find(peerId);
	if (i == _histories.end()) {
		return false;
	}
	const auto mentions = i->second->unreadMentions();
	const auto count = mentions.count();
	return (count < 0) || (mentions.loadedCount() < count);
}

void UnreadMentionsModel::loadAll() {
	if (_loading || !_inFlightPeers.empty() || _markingAll) {
		return;
	}
	resetDiscoveryRetry();
	_failedPeers.clear();
	_stalledPeers.clear();
	_seenLoadPeers.clear();
	_queuedPeers.clear();
	_inFlightPeers.clear();
	_pendingPeers.clear();
	_loading = true;
	_retrying = false;
	discover(true);
	queueNewlyDiscovered();
	pumpLoads();
	finishLoadingIfDone();
	publish();
}

void UnreadMentionsModel::retryFailed() {
	if (_loading || !_inFlightPeers.empty() || _markingAll) {
		return;
	}
	const auto retryDiscovery = _discoveryFailed;
	if (retryDiscovery) {
		resetDiscoveryRetry();
	}
	discover(retryDiscovery);
	auto targets = _failedPeers;
	for (const auto peerId : _stalledPeers) {
		targets.emplace(peerId);
	}
	if (targets.empty() && !retryDiscovery) {
		publish();
		return;
	}
	_seenLoadPeers.clear();
	_queuedPeers.clear();
	_inFlightPeers.clear();
	_pendingPeers.clear();
	_loading = true;
	_retrying = !retryDiscovery;
	for (const auto peerId : targets) {
		_seenLoadPeers.emplace(peerId);
		if (needsLoad(peerId)) {
			enqueue(peerId);
		}
	}
	if (!_retrying) {
		queueNewlyDiscovered();
	}
	pumpLoads();
	finishLoadingIfDone();
	publish();
}

void UnreadMentionsModel::queueNewlyDiscovered() {
	for (const auto &entry : _histories) {
		const auto peerId = entry.first;
		if (!_seenLoadPeers.emplace(peerId).second) {
			continue;
		}
		if (needsLoad(peerId)
			&& !_failedPeers.contains(peerId)
			&& !_stalledPeers.contains(peerId)) {
			enqueue(peerId);
		}
	}
}

void UnreadMentionsModel::enqueue(PeerId peerId) {
	if (_queuedPeers.contains(peerId)
		|| _inFlightPeers.contains(peerId)) {
		return;
	}
	_queuedPeers.emplace(peerId);
	_pendingPeers.push_back(peerId);
}

void UnreadMentionsModel::pumpLoads() {
	if (!_loading) {
		return;
	}
	while (_inFlightPeers.size() < kMaxConcurrentLoads
		&& !_pendingPeers.empty()) {
		const auto peerId = _pendingPeers.back();
		_pendingPeers.pop_back();
		_queuedPeers.remove(peerId);
		const auto i = _histories.find(peerId);
		if (i == _histories.end() || !needsLoad(peerId)) {
			continue;
		}
		const auto result = _session->api().unreadThings(
		).requestMoreMentions(i->second);
		if (result == Api::RequestMoreMentionsResult::Started
			|| needsLoad(peerId)) {
			_inFlightPeers.emplace(peerId);
		}
	}
}

void UnreadMentionsModel::finishLoadingIfDone() {
	if (!_loading
		|| !_pendingPeers.empty()
		|| !_inFlightPeers.empty()) {
		return;
	}
	if (!_retrying && !_listsComplete) {
		return;
	}
	_loading = false;
	_retrying = false;
	_seenLoadPeers.clear();
}

void UnreadMentionsModel::handleLoadResult(
		const Api::MentionsLoadResult &result) {
	const auto ours = _inFlightPeers.remove(result.peerId);
	if (ours) {
		if (!result.success) {
			_failedPeers.emplace(result.peerId);
			_stalledPeers.remove(result.peerId);
		} else if (result.stalled) {
			_failedPeers.remove(result.peerId);
			_stalledPeers.emplace(result.peerId);
		} else {
			_failedPeers.remove(result.peerId);
			_stalledPeers.remove(result.peerId);
		}
	}
	discover(false);
	if (_loading && !_retrying) {
		queueNewlyDiscovered();
	}
	if (ours
		&& result.success
		&& !result.stalled
		&& (result.afterLoaded > result.beforeLoaded)) {
		const auto i = _histories.find(result.peerId);
		if (i != _histories.end()) {
			const auto mentions = i->second->unreadMentions();
			const auto count = mentions.count();
			if (count >= 0 && mentions.loadedCount() < count) {
				enqueue(result.peerId);
			}
		}
	}
	pumpLoads();
	finishLoadingIfDone();
	publish();
}

void UnreadMentionsModel::markAll() {
	if (_markingAll || _loading || !_complete || (_count <= 0)) {
		return;
	}
	discover(false);
	if (!_complete) {
		publish();
		return;
	}
	auto histories = std::vector<not_null<History*>>();
	for (const auto &entry : _histories) {
		const auto history = entry.second;
		if (history->unreadMentions().count() > 0) {
			histories.push_back(history);
		}
	}
	if (histories.empty()) {
		publish();
		return;
	}
	_markFailedPeers.clear();
	_markRemaining = int(histories.size());
	_markingAll = true;
	publish();
	const auto weak = base::make_weak(this);
	for (const auto history : histories) {
		const auto peerId = history->peer->id;
		_session->api().unreadThings().readAllMentions(
			history,
			[weak, peerId](bool success) {
				if (weak) {
					weak->handleMarkResult(peerId, success);
				}
			});
	}
}

void UnreadMentionsModel::handleMarkResult(
		PeerId peerId,
		bool success) {
	if (!_markingAll || (_markRemaining <= 0)) {
		return;
	}
	if (!success) {
		_markFailedPeers.emplace(peerId);
	}
	--_markRemaining;
	discover(false);
	if (_markRemaining > 0) {
		publish();
		return;
	}
	_markingAll = false;
	publish();
	_markAllResults.fire(MarkAllMentionsResult{
		.failedPeers = _markFailedPeers,
	});
}

bool UnreadMentionsModel::itemAffectsModel(
		not_null<HistoryItem*> item) const {
	return _histories.contains(item->history()->peer->id)
		&& (item->isUnreadMention()
			|| _loadedIdSet.contains(item->fullId()));
}

} // namespace Ayu
