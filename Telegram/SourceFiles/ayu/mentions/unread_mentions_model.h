/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/timer.h"
#include "base/weak_ptr.h"
#include "data/data_types.h"
#include "rpl/event_stream.h"
#include "rpl/lifetime.h"
#include "rpl/producer.h"
#include "rpl/variable.h"

#include <vector>

class History;

namespace Api {
struct MentionsLoadResult;
} // namespace Api

namespace Data {
class Folder;
} // namespace Data

namespace Main {
class Session;
} // namespace Main

namespace Ayu {

struct UnreadMentionsState {
	int count = 0;
	int loadingPeers = 0;
	bool complete = false;
	bool discoveryFailed = false;
	bool loading = false;
	bool retrying = false;
	bool markingAll = false;
	std::vector<FullMsgId> loadedIds;
	base::flat_set<PeerId> failedPeers;
	base::flat_set<PeerId> stalledPeers;

	friend inline bool operator==(
		const UnreadMentionsState &,
		const UnreadMentionsState &) = default;
};

struct MarkAllMentionsResult {
	base::flat_set<PeerId> failedPeers;
};

class UnreadMentionsModel final : public base::has_weak_ptr {
public:
	explicit UnreadMentionsModel(not_null<Main::Session*> session);
	~UnreadMentionsModel();

	[[nodiscard]] UnreadMentionsState stateCurrent() const;
	[[nodiscard]] rpl::producer<UnreadMentionsState> stateValue() const;
	[[nodiscard]] auto markAllResults() const
		-> rpl::producer<MarkAllMentionsResult>;

	void refresh();
	void loadAll();
	void retryFailed();
	void markAll();

private:
	struct Discovery;

	void setupSubscriptions();
	void refresh(bool requestIncompleteDialogs);
	void handleDialogsChange();
	void discover(bool requestIncompleteDialogs);
	void discoverFolder(
		Data::Folder *folder,
		Discovery &result,
		bool requestIncompleteDialogs);
	void scheduleDiscoveryRetry();
	void handleDiscoveryRetry();
	void resetDiscoveryRetry();
	void publish();

	[[nodiscard]] bool needsLoad(PeerId peerId) const;
	void queueNewlyDiscovered();
	void enqueue(PeerId peerId);
	void pumpLoads();
	void finishLoadingIfDone();
	void handleLoadResult(const Api::MentionsLoadResult &result);
	void handleMarkResult(PeerId peerId, bool success);
	[[nodiscard]] bool itemAffectsModel(
		not_null<HistoryItem*> item) const;

	const not_null<Main::Session*> _session;

	base::flat_map<PeerId, not_null<History*>> _histories;
	std::vector<FullMsgId> _loadedIds;
	base::flat_set<FullMsgId> _loadedIdSet;
	base::flat_set<PeerId> _failedPeers;
	base::flat_set<PeerId> _stalledPeers;
	base::flat_set<PeerId> _seenLoadPeers;
	base::flat_set<PeerId> _queuedPeers;
	base::flat_set<PeerId> _inFlightPeers;
	std::vector<PeerId> _pendingPeers;
	base::flat_set<PeerId> _markFailedPeers;

	rpl::variable<UnreadMentionsState> _state;
	rpl::event_stream<MarkAllMentionsResult> _markAllResults;
	rpl::lifetime _lifetime;
	base::Timer _discoveryRetryTimer;

	int _count = 0;
	int _markRemaining = 0;
	int _discoveryRetryAttempt = 0;
	bool _complete = false;
	bool _listsComplete = false;
	bool _discoveryFailed = false;
	bool _resumeLoadingAfterDiscoveryFailure = false;
	bool _loading = false;
	bool _retrying = false;
	bool _markingAll = false;

};

} // namespace Ayu
