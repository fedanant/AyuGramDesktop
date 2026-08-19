/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/weak_ptr.h"

class ApiWrap;
class PeerData;
class ChannelData;

namespace Data {
class Thread;
} // namespace Data

namespace Api {

enum class RequestMoreMentionsResult {
	Started,
	Rejected,
};

struct MentionsLoadResult {
	PeerId peerId = 0;
	int beforeLoaded = 0;
	int afterLoaded = 0;
	bool success = false;
	bool allLoaded = false;
	bool stalled = false;
};

class UnreadThings final {
public:
	explicit UnreadThings(not_null<ApiWrap*> api);

	[[nodiscard]] bool trackMentions(Data::Thread *thread) const;
	[[nodiscard]] bool trackReactions(Data::Thread *thread) const;
	[[nodiscard]] bool trackPollVotes(Data::Thread *thread) const;

	void preloadEnough(Data::Thread *thread);

	void mediaAndMentionsRead(
		const base::flat_set<MsgId> &readIds,
		ChannelData *channel = nullptr);

	[[nodiscard]] RequestMoreMentionsResult requestMoreMentions(
		not_null<Data::Thread*> thread);
	[[nodiscard]] rpl::producer<MentionsLoadResult> mentionsLoadResults() const;
	void readAllMentions(
		not_null<Data::Thread*> thread,
		Fn<void(bool)> done);

	void cancelRequests(not_null<Data::Thread*> thread);

private:
	void preloadEnoughMentions(not_null<Data::Thread*> thread);
	void preloadEnoughReactions(not_null<Data::Thread*> thread);
	void preloadEnoughPollVotes(not_null<Data::Thread*> thread);
	void readAllMentions(
		base::weak_ptr<Data::Thread> weakThread,
		Fn<void(bool)> done);

	void requestMentions(not_null<Data::Thread*> thread, int loaded);
	void requestReactions(not_null<Data::Thread*> thread, int loaded);
	void requestPollVotes(not_null<Data::Thread*> thread, int loaded);

	const not_null<ApiWrap*> _api;

	base::flat_map<not_null<Data::Thread*>, mtpRequestId> _topicMentionsRequests;
	base::flat_map<PeerId, mtpRequestId> _globalMentionsRequests;
	base::flat_map<not_null<Data::Thread*>, mtpRequestId> _reactionsRequests;
	base::flat_map<not_null<Data::Thread*>, mtpRequestId> _pollVotesRequests;
	rpl::event_stream<MentionsLoadResult> _mentionsLoadResults;

};

} // namespace Api
