/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/bytes.h"
#include "mtproto/mtproto_custom_endpoint.h"

#include <QtCore/QReadWriteLock>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace MTP {
namespace details {
class RSAPublicKey;
} // namespace details

enum class DcType {
	Regular,
	Temporary,
	MediaCluster,
	Cdn,
};

enum class Environment : uchar {
	Production,
	Test,
};

struct CustomEndpointCandidate {
	std::optional<CustomEndpointProfile> profile;
};

class DcOptions {
public:
	using Flag = MTPDdcOption::Flag;
	using Flags = MTPDdcOption::Flags;
	struct Endpoint {
		Endpoint(
			DcId id,
			Flags flags,
			const std::string &ip,
			int port,
			const bytes::vector &secret)
			: id(id)
			, flags(flags)
			, ip(ip)
			, port(port)
			, secret(secret) {
		}

		DcId id;
		Flags flags;
		std::string ip;
		int port;
		bytes::vector secret;

	};

	explicit DcOptions(Environment environment);
	DcOptions(const DcOptions &other);
	~DcOptions();

	[[nodiscard]] static bool ValidateSecret(bytes::const_span secret);

	[[nodiscard]] Environment environment() const;
	[[nodiscard]] bool isTestMode() const;

	// construct methods don't notify "changed" subscribers.
	bool constructFromSerialized(const QByteArray &serialized);
	void constructFromBuiltIn();
	void constructAddOne(
		int id,
		Flags flags,
		const std::string &ip,
		int port,
		const bytes::vector &secret);
	QByteArray serialize() const;

	[[nodiscard]] rpl::producer<DcId> changed() const;
	[[nodiscard]] rpl::producer<> cdnConfigChanged() const;
	[[nodiscard]] auto confirmedCustomEndpointProfile() const
	-> std::optional<CustomEndpointProfile>;
	[[nodiscard]] auto confirmedCustomEndpointProfileChanged() const
	-> rpl::producer<>;
	[[nodiscard]] auto customEndpointCandidate() const
	-> std::optional<CustomEndpointCandidate>;
	[[nodiscard]] bool customEndpointCandidateActive() const;
	[[nodiscard]] bool hasCustomEndpoint() const;
	[[nodiscard]] bool setConfirmedCustomEndpointProfile(
		std::optional<CustomEndpointProfile> profile);
	[[nodiscard]] bool applyCustomEndpointCandidate(
		CustomEndpointCandidate candidate);
	[[nodiscard]] bool promoteCustomEndpointCandidate();
	[[nodiscard]] bool rollbackCustomEndpointCandidate();
	void setFromList(const MTPVector<MTPDcOption> &options);
	void addFromList(const MTPVector<MTPDcOption> &options);
	void addFromOther(DcOptions &&options);

	[[nodiscard]] std::vector<DcId> configEnumDcIds() const;

	struct Variants {
		enum Address {
			IPv4 = 0,
			IPv6 = 1,
			AddressTypeCount = 2,
		};
		enum Protocol {
			Tcp = 0,
			Http = 1,
			ProtocolCount = 2,
		};
		std::vector<Endpoint> data[AddressTypeCount][ProtocolCount];
	};
	[[nodiscard]] Variants lookup(
		DcId dcId,
		DcType type,
		bool throughProxy) const;
	[[nodiscard]] DcType dcType(ShiftedDcId shiftedDcId) const;

	void setCDNConfig(const MTPDcdnConfig &config);
	[[nodiscard]] bool hasCDNKeysForDc(DcId dcId) const;
	[[nodiscard]] details::RSAPublicKey getDcRSAKey(
		DcId dcId,
		const QVector<MTPlong> &fingerprints) const;

	// Debug feature for now.
	bool loadFromFile(const QString &path);
	bool writeToFile(const QString &path) const;

private:
	using EndpointMap = base::flat_map<DcId, std::vector<Endpoint>>;
	using PublicKeyMap = base::flat_map<uint64, details::RSAPublicKey>;
	struct CustomEndpointState {
		const CustomEndpointProfile *profile = nullptr;
		const EndpointMap *data = nullptr;
		const PublicKeyMap *keys = nullptr;
	};
	struct CustomEndpointStateSnapshot {
		EndpointMap data;
		std::optional<QByteArray> publicKeyPem;
	};

	bool applyOneGuarded(
		DcId dcId,
		Flags flags,
		const std::string &ip,
		int port,
		const bytes::vector &secret);
	static bool ApplyOneOption(
		EndpointMap &data,
		DcId dcId,
		Flags flags,
		const std::string &ip,
		int port,
		const bytes::vector &secret);
	static std::vector<DcId> CountOptionsDifference(
		const EndpointMap &a,
		const EndpointMap &b);
	static std::vector<DcId> CountCustomEndpointDifference(
		const EndpointMap &before,
		const EndpointMap &after,
		const CustomEndpointStateSnapshot &beforeCustom,
		const CustomEndpointStateSnapshot &afterCustom);
	static bool PrepareCustomEndpointProfile(
		CustomEndpointProfile &profile,
		Environment environment,
		EndpointMap &data,
		PublicKeyMap &keys);
	static void FilterIfHasWithFlag(Variants &variants, Flag flag);

	[[nodiscard]] bool hasMediaOnlyOptionsFor(DcId dcId) const;
	[[nodiscard]] bool hasMediaOnlyOptionsForGuarded(DcId dcId) const;
	[[nodiscard]] CustomEndpointState customEndpointStateGuarded() const;
	[[nodiscard]] auto customEndpointStateSnapshotGuarded() const
	-> CustomEndpointStateSnapshot;
	[[nodiscard]] const EndpointMap *customEndpointDataGuarded() const;
	[[nodiscard]] EndpointMap effectiveDataGuarded() const;

	void processFromList(const QVector<MTPDcOption> &options, bool overwrite);
	void computeCdnDcIds();

	void readBuiltInPublicKeys();

	class WriteLocker;
	friend class WriteLocker;

	class ReadLocker;
	friend class ReadLocker;

	const Environment _environment = Environment();
	EndpointMap _data;
	std::optional<CustomEndpointProfile> _confirmedCustomEndpointProfile;
	EndpointMap _confirmedCustomEndpointData;
	PublicKeyMap _confirmedCustomEndpointKeys;
	std::optional<CustomEndpointCandidate> _customEndpointCandidate;
	EndpointMap _customEndpointCandidateData;
	PublicKeyMap _customEndpointCandidateKeys;
	base::flat_set<DcId> _cdnDcIds;
	PublicKeyMap _publicKeys;
	base::flat_map<DcId, PublicKeyMap> _cdnPublicKeys;
	mutable QReadWriteLock _useThroughLockers;

	rpl::event_stream<DcId> _changed;
	rpl::event_stream<> _cdnConfigChanged;
	rpl::event_stream<> _confirmedCustomEndpointProfileChanged;

	// True when we have overriden options from a .tdesktop-endpoints file.
	bool _immutable = false;

};

} // namespace MTP
