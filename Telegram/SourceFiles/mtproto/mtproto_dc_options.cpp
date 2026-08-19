/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/mtproto_dc_options.h"

#include "mtproto/details/mtproto_rsa_public_key.h"
#include "mtproto/connection_tcp.h"
#include "mtproto/facade.h"
#include "storage/serialize_common.h"

#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QHostAddress>

namespace MTP {
namespace {

constexpr auto kVersion = 2;

using namespace details;

struct BuiltInDc {
	int id;
	const char *ip;
	int port;
};

const BuiltInDc kBuiltInDcs[] = {
	{ 1, "149.154.175.50" , 443 },
	{ 2, "149.154.167.51" , 443 },
	{ 2, "95.161.76.100"  , 443 },
	{ 3, "149.154.175.100", 443 },
	{ 4, "149.154.167.91" , 443 },
	{ 5, "149.154.171.5"  , 443 },
};

const BuiltInDc kBuiltInDcsIPv6[] = {
	{ 1, "2001:0b28:f23d:f001:0000:0000:0000:000a", 443 },
	{ 2, "2001:067c:04e8:f002:0000:0000:0000:000a", 443 },
	{ 3, "2001:0b28:f23d:f003:0000:0000:0000:000a", 443 },
	{ 4, "2001:067c:04e8:f004:0000:0000:0000:000a", 443 },
	{ 5, "2001:0b28:f23f:f005:0000:0000:0000:000a", 443 },
};

const BuiltInDc kBuiltInDcsTest[] = {
	{ 1, "149.154.175.10" , 443 },
	{ 2, "149.154.167.40" , 443 },
	{ 3, "149.154.175.117", 443 }
};

const BuiltInDc kBuiltInDcsIPv6Test[] = {
	{ 1, "2001:0b28:f23d:f001:0000:0000:0000:000e", 443 },
	{ 2, "2001:067c:04e8:f002:0000:0000:0000:000e", 443 },
	{ 3, "2001:0b28:f23d:f003:0000:0000:0000:000e", 443 }
};

const char *kTestPublicRSAKeys[] = { "\
-----BEGIN RSA PUBLIC KEY-----\n\
MIIBCgKCAQEAyMEdY1aR+sCR3ZSJrtztKTKqigvO/vBfqACJLZtS7QMgCGXJ6XIR\n\
yy7mx66W0/sOFa7/1mAZtEoIokDP3ShoqF4fVNb6XeqgQfaUHd8wJpDWHcR2OFwv\n\
plUUI1PLTktZ9uW2WE23b+ixNwJjJGwBDJPQEQFBE+vfmH0JP503wr5INS1poWg/\n\
j25sIWeYPHYeOrFp/eXaqhISP6G+q2IeTaWTXpwZj4LzXq5YOpk4bYEQ6mvRq7D1\n\
aHWfYmlEGepfaYR8Q0YqvvhYtMte3ITnuSJs171+GDqpdKcSwHnd6FudwGO4pcCO\n\
j4WcDuXc2CTHgH8gFTNhp/Y8/SpDOhvn9QIDAQAB\n\
-----END RSA PUBLIC KEY-----" };

const char *kPublicRSAKeys[] = { "\
-----BEGIN RSA PUBLIC KEY-----\n\
MIIBCgKCAQEA6LszBcC1LGzyr992NzE0ieY+BSaOW622Aa9Bd4ZHLl+TuFQ4lo4g\n\
5nKaMBwK/BIb9xUfg0Q29/2mgIR6Zr9krM7HjuIcCzFvDtr+L0GQjae9H0pRB2OO\n\
62cECs5HKhT5DZ98K33vmWiLowc621dQuwKWSQKjWf50XYFw42h21P2KXUGyp2y/\n\
+aEyZ+uVgLLQbRA1dEjSDZ2iGRy12Mk5gpYc397aYp438fsJoHIgJ2lgMv5h7WY9\n\
t6N/byY9Nw9p21Og3AoXSL2q/2IJ1WRUhebgAdGVMlV1fkuOQoEzR7EdpqtQD9Cs\n\
5+bfo3Nhmcyvk5ftB0WkJ9z6bNZ7yxrP8wIDAQAB\n\
-----END RSA PUBLIC KEY-----" };

} // namespace

class DcOptions::WriteLocker {
public:
	WriteLocker(not_null<DcOptions*> that)
	: _that(that)
	, _lock(&_that->_useThroughLockers) {
	}

	void unlock() {
		_lock.unlock();
	}

	~WriteLocker() {
		_that->computeCdnDcIds();
	}

private:
	not_null<DcOptions*> _that;
	QWriteLocker _lock;

};

class DcOptions::ReadLocker {
public:
	ReadLocker(not_null<const DcOptions*> that)
	: _lock(&that->_useThroughLockers) {
	}

	void unlock() {
		_lock.unlock();
	}

private:
	QReadLocker _lock;

};

DcOptions::DcOptions(Environment environment)
: _environment(environment) {
	constructFromBuiltIn();
}

DcOptions::DcOptions(const DcOptions &other)
: _environment(other._environment) {
	ReadLocker lock(&other);
	_data = other._data;
	_confirmedCustomEndpointProfile = other._confirmedCustomEndpointProfile;
	_confirmedCustomEndpointData = other._confirmedCustomEndpointData;
	_confirmedCustomEndpointKeys = other._confirmedCustomEndpointKeys;
	_cdnDcIds = other._cdnDcIds;
	_publicKeys = other._publicKeys;
	_cdnPublicKeys = other._cdnPublicKeys;
	_immutable = other._immutable;
}

DcOptions::~DcOptions() = default;

bool DcOptions::ValidateSecret(bytes::const_span secret) {
	// See also TcpConnection::Protocol::Create.
	return (secret.size() >= 21 && secret[0] == bytes::type(0xEE))
		|| (secret.size() == 17 && secret[0] == bytes::type(0xDD))
		|| (secret.size() == 16)
		|| secret.empty();
}

void DcOptions::readBuiltInPublicKeys() {
	const auto builtin = (_environment == Environment::Test)
		? gsl::make_span(kTestPublicRSAKeys)
		: gsl::make_span(kPublicRSAKeys);
	for (const auto key : builtin) {
		const auto keyBytes = bytes::make_span(key, strlen(key));
		auto parsed = RSAPublicKey(keyBytes);
		if (parsed.valid()) {
			_publicKeys.emplace(parsed.fingerprint(), std::move(parsed));
		} else {
			LOG(("MTP Error: could not read this public RSA key:"));
			LOG((key));
		}
	}
}

Environment DcOptions::environment() const {
	return _environment;
}

bool DcOptions::isTestMode() const {
	return (_environment != Environment::Production);
}

void DcOptions::constructFromBuiltIn() {
	WriteLocker lock(this);
	_data.clear();

	readBuiltInPublicKeys();

	const auto list = isTestMode()
		? gsl::make_span(kBuiltInDcsTest)
		: gsl::make_span(kBuiltInDcs).subspan(0);
	for (const auto &entry : list) {
		const auto flags = Flag::f_static | 0;
		applyOneGuarded(entry.id, flags, entry.ip, entry.port, {});
		DEBUG_LOG(("MTP Info: adding built in DC %1 connect option: %2:%3"
			).arg(entry.id
			).arg(entry.ip
			).arg(entry.port));
	}

	const auto listv6 = isTestMode()
		? gsl::make_span(kBuiltInDcsIPv6Test)
		: gsl::make_span(kBuiltInDcsIPv6).subspan(0);
	for (const auto &entry : listv6) {
		const auto flags = Flag::f_static | Flag::f_ipv6;
		applyOneGuarded(entry.id, flags, entry.ip, entry.port, {});
		DEBUG_LOG(("MTP Info: adding built in DC %1 IPv6 connect option: "
			"%2:%3"
			).arg(entry.id
			).arg(entry.ip
			).arg(entry.port));
	}
}

void DcOptions::processFromList(
		const QVector<MTPDcOption> &options,
		bool overwrite) {
	if (options.empty() || _immutable) {
		return;
	}

	auto data = [&] {
		if (overwrite) {
			return EndpointMap();
		}
		ReadLocker lock(this);
		return _data;
	}();
	for (auto &mtpOption : options) {
		if (mtpOption.type() != mtpc_dcOption) {
			LOG(("Wrong type in DcOptions: %1").arg(mtpOption.type()));
			continue;
		}

		auto &option = mtpOption.c_dcOption();
		auto dcId = option.vid().v;
		auto flags = option.vflags().v;
		auto ip = std::string(
			option.vip_address().v.constData(),
			option.vip_address().v.size());
		auto port = option.vport().v;
		auto secret = bytes::make_vector(option.vsecret().value_or_empty());
		ApplyOneOption(data, dcId, flags, ip, port, secret);
	}

	const auto difference = [&] {
		WriteLocker lock(this);
		const auto before = effectiveDataGuarded();
		if (!CountOptionsDifference(_data, data).empty()) {
			_data = std::move(data);
		}
		return CountOptionsDifference(before, effectiveDataGuarded());
	}();
	for (const auto dcId : difference) {
		_changed.fire_copy(dcId);
	}
}

void DcOptions::setFromList(const MTPVector<MTPDcOption> &options) {
	processFromList(options.v, true);
}

void DcOptions::addFromList(const MTPVector<MTPDcOption> &options) {
	processFromList(options.v, false);
}

void DcOptions::addFromOther(DcOptions &&options) {
	if (this == &options || _immutable) {
		return;
	}

	auto incomingData = EndpointMap();
	auto incomingCdnPublicKeys = decltype(_cdnPublicKeys)();
	{
		WriteLocker lock(&options);
		if (options._data.empty()) {
			return;
		}
		incomingData = base::take(options._data);
		incomingCdnPublicKeys = base::take(options._cdnPublicKeys);
	}

	const auto idsChanged = [&] {
		WriteLocker lock(this);
		const auto before = effectiveDataGuarded();
		for (const auto &item : incomingData) {
			for (const auto &endpoint : item.second) {
				applyOneGuarded(
					endpoint.id,
					endpoint.flags,
					endpoint.ip,
					endpoint.port,
					endpoint.secret);
			}
		}
		for (auto &item : incomingCdnPublicKeys) {
			for (auto &entry : item.second) {
				_cdnPublicKeys[item.first].insert(std::move(entry));
			}
		}
		return CountOptionsDifference(before, effectiveDataGuarded());
	}();
	for (const auto dcId : idsChanged) {
		_changed.fire_copy(dcId);
	}
}

void DcOptions::constructAddOne(
		int id,
		Flags flags,
		const std::string &ip,
		int port,
		const bytes::vector &secret) {
	WriteLocker lock(this);
	applyOneGuarded(BareDcId(id), flags, ip, port, secret);
}

bool DcOptions::applyOneGuarded(
		DcId dcId,
		Flags flags,
		const std::string &ip,
		int port,
		const bytes::vector &secret) {
	return ApplyOneOption(_data, dcId, flags, ip, port, secret);
}

bool DcOptions::ApplyOneOption(
		EndpointMap &data,
		DcId dcId,
		Flags flags,
		const std::string &ip,
		int port,
		const bytes::vector &secret) {
	auto i = data.find(dcId);
	if (i != data.cend()) {
		for (auto &endpoint : i->second) {
			if (endpoint.ip == ip && endpoint.port == port) {
				return false;
			}
		}
		i->second.emplace_back(dcId, flags, ip, port, secret);
	} else {
		data.emplace(dcId, std::vector<Endpoint>(
			1,
			Endpoint(dcId, flags, ip, port, secret)));
	}
	return true;
}

std::vector<DcId> DcOptions::CountOptionsDifference(
		const EndpointMap &a,
		const EndpointMap &b) {
	const auto canonicalIp = [](const std::string &ip) {
		auto address = QHostAddress();
		return address.setAddress(QString::fromStdString(ip))
			? address.toString().toStdString()
			: ip;
	};
	const auto equalEndpoint = [&](const Endpoint &a, const Endpoint &b) {
		return a.id == b.id
			&& a.flags == b.flags
			&& canonicalIp(a.ip) == canonicalIp(b.ip)
			&& a.port == b.port
			&& a.secret == b.secret;
	};
	auto result = base::flat_set<DcId>();
	const auto find = [&](
			const std::vector<Endpoint> &where,
			const Endpoint &what) {
		for (const auto &endpoint : where) {
			if (equalEndpoint(endpoint, what)) {
				return true;
			}
		}
		return false;
	};
	const auto equal = [&](
			const std::vector<Endpoint> &m,
			const std::vector<Endpoint> &n) {
		if (m.size() != n.size()) {
			return false;
		}
		for (const auto &endpoint : m) {
			if (!find(n, endpoint)) {
				return false;
			}
		}
		return true;
	};

	auto i = begin(a);
	auto j = begin(b);
	const auto max = std::numeric_limits<DcId>::max();
	while (i != end(a) || j != end(b)) {
		const auto aId = (i == end(a)) ? max : i->first;
		const auto bId = (j == end(b)) ? max : j->first;
		if (aId < bId) {
			result.emplace(BareDcId(aId));
			++i;
		} else if (bId < aId) {
			result.emplace(BareDcId(bId));
			++j;
		} else {
			if (!equal(i->second, j->second)) {
				result.emplace(BareDcId(aId));
			}
			++i;
			++j;
		}
	}
	return std::vector<DcId>(result.begin(), result.end());
}

std::vector<DcId> DcOptions::CountCustomEndpointDifference(
		const EndpointMap &before,
		const EndpointMap &after,
		const CustomEndpointStateSnapshot &beforeCustom,
		const CustomEndpointStateSnapshot &afterCustom) {
	auto result = base::flat_set<DcId>();
	for (const auto dcId : CountOptionsDifference(before, after)) {
		result.emplace(BareDcId(dcId));
	}
	if (beforeCustom.publicKeyPem != afterCustom.publicKeyPem) {
		for (const auto &item : beforeCustom.data) {
			result.emplace(BareDcId(item.first));
		}
		for (const auto &item : afterCustom.data) {
			result.emplace(BareDcId(item.first));
		}
	}
	return std::vector<DcId>(result.begin(), result.end());
}

bool DcOptions::PrepareCustomEndpointProfile(
		CustomEndpointProfile &profile,
		Environment environment,
		EndpointMap &data,
		PublicKeyMap &keys) {
	data.clear();
	keys.clear();
	if (profile.environment != environment) {
		return false;
	}

	auto publicKey = CustomEndpointPublicKeyResult();
	if (!profile.publicKeyPem.isEmpty()) {
		publicKey = ValidateCustomEndpointPublicKey(profile.publicKeyPem);
		if (!IsValidCustomEndpoint(publicKey.validation)) {
			return false;
		}
		profile.publicKeyPem = publicKey.publicKeyPem;
	}
	if (!IsValidCustomEndpoint(ValidateCustomEndpointProfile(profile))) {
		return false;
	}

	for (auto &custom : profile.endpoints) {
		auto address = QHostAddress();
		if (!address.setAddress(QString::fromStdString(custom.ip))) {
			return false;
		}
		custom.ip = address.toString().toStdString();
		ApplyOneOption(
			data,
			custom.dcId,
			custom.flags,
			custom.ip,
			custom.port,
			{});
	}
	if (publicKey.key.valid()) {
		keys.emplace(publicKey.fingerprint, std::move(publicKey.key));
	}
	return true;
}

QByteArray DcOptions::serialize() const {
	if (_immutable) {
		// Don't write the overriden options to our settings.
		return DcOptions(_environment).serialize();
	}

	ReadLocker lock(this);

	auto size = sizeof(qint32);

	// Dc options.
	auto optionsCount = 0;
	size += sizeof(qint32);
	for (const auto &item : _data) {
		if (isTemporaryDcId(item.first)) {
			continue;
		}
		for (const auto &endpoint : item.second) {
			++optionsCount;
			// id + flags + port
			size += sizeof(qint32) + sizeof(qint32) + sizeof(qint32);
			size += sizeof(qint32) + endpoint.ip.size();
			size += sizeof(qint32) + endpoint.secret.size();
		}
	}

	// CDN public keys.
	auto count = 0;
	for (auto &keysInDc : _cdnPublicKeys) {
		count += keysInDc.second.size();
	}
	struct SerializedPublicKey {
		DcId dcId;
		bytes::vector n;
		bytes::vector e;
	};
	std::vector<SerializedPublicKey> publicKeys;
	publicKeys.reserve(count);
	size += sizeof(qint32);
	for (const auto &keysInDc : _cdnPublicKeys) {
		for (const auto &entry : keysInDc.second) {
			publicKeys.push_back({
				keysInDc.first,
				entry.second.getN(),
				entry.second.getE()
			});
			size += sizeof(qint32)
				+ Serialize::bytesSize(publicKeys.back().n)
				+ Serialize::bytesSize(publicKeys.back().e);
		}
	}

	auto result = QByteArray();
	result.reserve(size);
	{
		QDataStream stream(&result, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream << qint32(-kVersion);

		// Dc options.
		stream << qint32(optionsCount);
		for (const auto &item : _data) {
			if (isTemporaryDcId(item.first)) {
				continue;
			}
			for (const auto &endpoint : item.second) {
				stream << qint32(endpoint.id)
					<< qint32(endpoint.flags)
					<< qint32(endpoint.port)
					<< qint32(endpoint.ip.size());
				stream.writeRawData(endpoint.ip.data(), endpoint.ip.size());
				stream << qint32(endpoint.secret.size());
				stream.writeRawData(
					reinterpret_cast<const char*>(endpoint.secret.data()),
					endpoint.secret.size());
			}
		}

		// CDN public keys.
		stream << qint32(publicKeys.size());
		for (auto &key : publicKeys) {
			stream << qint32(key.dcId)
				<< Serialize::bytes(key.n)
				<< Serialize::bytes(key.e);
		}
	}
	return result;
}

bool DcOptions::constructFromSerialized(const QByteArray &serialized) {
	QDataStream stream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);

	auto minusVersion = qint32(0);
	stream >> minusVersion;
	const auto version = (minusVersion < 0) ? (-minusVersion) : 0;

	auto count = qint32(0);
	if (version > 0) {
		stream >> count;
	} else {
		count = minusVersion;
	}
	if (stream.status() != QDataStream::Ok) {
		LOG(("MTP Error: Bad data for DcOptions::constructFromSerialized()"));
		return false;
	}

	WriteLocker lock(this);
	_data.clear();
	for (auto i = 0; i != count; ++i) {
		qint32 id = 0, flags = 0, port = 0, ipSize = 0;
		stream >> id >> flags >> port >> ipSize;

		// https://stackoverflow.com/questions/1076714/max-length-for-client-ip-address
		constexpr auto kMaxIpSize = 45;
		if (ipSize <= 0 || ipSize > kMaxIpSize) {
			LOG(("MTP Error: Bad data inside DcOptions::constructFromSerialized()"));
			return false;
		}

		auto ip = std::string(ipSize, ' ');
		stream.readRawData(ip.data(), ipSize);

		constexpr auto kMaxSecretSize = 32;
		auto secret = bytes::vector();
		if (version > 0) {
			auto secretSize = qint32(0);
			stream >> secretSize;
			if (secretSize < 0 || secretSize > kMaxSecretSize) {
				LOG(("MTP Error: Bad data inside DcOptions::constructFromSerialized()"));
				return false;
			} else if (secretSize > 0) {
				secret.resize(secretSize);
				stream.readRawData(
					reinterpret_cast<char*>(secret.data()),
					secretSize);
			}
		}

		if (stream.status() != QDataStream::Ok) {
			LOG(("MTP Error: Bad data inside DcOptions::constructFromSerialized()"));
			return false;
		}

		applyOneGuarded(
			DcId(id),
			Flags::from_raw(flags),
			ip,
			port,
			secret);
	}

	// Read CDN config
	if (!stream.atEnd() && version > 1) {
		auto count = qint32(0);
		stream >> count;
		if (stream.status() != QDataStream::Ok) {
			LOG(("MTP Error: Bad data for CDN config in DcOptions::constructFromSerialized()"));
			return false;
		}

		for (auto i = 0; i != count; ++i) {
			qint32 dcId = 0;
			bytes::vector n, e;
			stream >> dcId >> Serialize::bytes(n) >> Serialize::bytes(e);
			if (stream.status() != QDataStream::Ok) {
				LOG(("MTP Error: Bad data for CDN config inside DcOptions::constructFromSerialized()"));
				return false;
			}

			auto key = RSAPublicKey(n, e);
			if (key.valid()) {
				_cdnPublicKeys[dcId].emplace(key.fingerprint(), std::move(key));
			} else {
				LOG(("MTP Error: Could not read valid CDN public key."));
				return false;
			}
		}
	}
	return true;
}

rpl::producer<DcId> DcOptions::changed() const {
	return _changed.events();
}

rpl::producer<> DcOptions::cdnConfigChanged() const {
	return _cdnConfigChanged.events();
}

auto DcOptions::confirmedCustomEndpointProfile() const
-> std::optional<CustomEndpointProfile> {
	ReadLocker lock(this);
	return _confirmedCustomEndpointProfile;
}

auto DcOptions::confirmedCustomEndpointProfileChanged() const
-> rpl::producer<> {
	return _confirmedCustomEndpointProfileChanged.events();
}

auto DcOptions::customEndpointCandidate() const
-> std::optional<CustomEndpointCandidate> {
	ReadLocker lock(this);
	return _customEndpointCandidate;
}

bool DcOptions::customEndpointCandidateActive() const {
	ReadLocker lock(this);
	return _customEndpointCandidate.has_value();
}

bool DcOptions::hasCustomEndpoint() const {
	ReadLocker lock(this);
	return _confirmedCustomEndpointProfile.has_value()
		|| _customEndpointCandidate.has_value();
}

bool DcOptions::setConfirmedCustomEndpointProfile(
		std::optional<CustomEndpointProfile> profile) {
	auto data = EndpointMap();
	auto keys = PublicKeyMap();
	if (profile
		&& !PrepareCustomEndpointProfile(
			*profile,
			_environment,
			data,
			keys)) {
		return false;
	}

	auto difference = std::vector<DcId>();
	{
		WriteLocker lock(this);
		if (_customEndpointCandidate) {
			return false;
		} else if (_confirmedCustomEndpointProfile == profile) {
			return true;
		}
		const auto before = effectiveDataGuarded();
		const auto beforeCustom = customEndpointStateSnapshotGuarded();
		_confirmedCustomEndpointProfile = std::move(profile);
		_confirmedCustomEndpointData = std::move(data);
		_confirmedCustomEndpointKeys = std::move(keys);
		difference = CountCustomEndpointDifference(
			before,
			effectiveDataGuarded(),
			beforeCustom,
			customEndpointStateSnapshotGuarded());
	}
	for (const auto dcId : difference) {
		_changed.fire_copy(dcId);
	}
	_confirmedCustomEndpointProfileChanged.fire({});
	return true;
}

bool DcOptions::applyCustomEndpointCandidate(
		CustomEndpointCandidate candidate) {
	auto data = EndpointMap();
	auto keys = PublicKeyMap();
	if (candidate.profile
		&& !PrepareCustomEndpointProfile(
			*candidate.profile,
			_environment,
			data,
			keys)) {
		return false;
	}

	auto difference = std::vector<DcId>();
	{
		WriteLocker lock(this);
		if (_customEndpointCandidate) {
			return false;
		}
		const auto before = effectiveDataGuarded();
		const auto beforeCustom = customEndpointStateSnapshotGuarded();
		_customEndpointCandidate = std::move(candidate);
		_customEndpointCandidateData = std::move(data);
		_customEndpointCandidateKeys = std::move(keys);
		difference = CountCustomEndpointDifference(
			before,
			effectiveDataGuarded(),
			beforeCustom,
			customEndpointStateSnapshotGuarded());
	}
	for (const auto dcId : difference) {
		_changed.fire_copy(dcId);
	}
	return true;
}

bool DcOptions::promoteCustomEndpointCandidate() {
	auto difference = std::vector<DcId>();
	{
		WriteLocker lock(this);
		if (!_customEndpointCandidate) {
			return false;
		}
		const auto before = effectiveDataGuarded();
		const auto beforeCustom = customEndpointStateSnapshotGuarded();
		auto candidate = base::take(_customEndpointCandidate);
		_confirmedCustomEndpointProfile = std::move(candidate->profile);
		_confirmedCustomEndpointData = base::take(
			_customEndpointCandidateData);
		_confirmedCustomEndpointKeys = base::take(
			_customEndpointCandidateKeys);
		difference = CountCustomEndpointDifference(
			before,
			effectiveDataGuarded(),
			beforeCustom,
			customEndpointStateSnapshotGuarded());
	}
	for (const auto dcId : difference) {
		_changed.fire_copy(dcId);
	}
	_confirmedCustomEndpointProfileChanged.fire({});
	return true;
}

bool DcOptions::rollbackCustomEndpointCandidate() {
	auto difference = std::vector<DcId>();
	{
		WriteLocker lock(this);
		if (!_customEndpointCandidate) {
			return false;
		}
		const auto before = effectiveDataGuarded();
		const auto beforeCustom = customEndpointStateSnapshotGuarded();
		_customEndpointCandidate.reset();
		_customEndpointCandidateData.clear();
		_customEndpointCandidateKeys.clear();
		difference = CountCustomEndpointDifference(
			before,
			effectiveDataGuarded(),
			beforeCustom,
			customEndpointStateSnapshotGuarded());
	}
	for (const auto dcId : difference) {
		_changed.fire_copy(dcId);
	}
	return true;
}

std::vector<DcId> DcOptions::configEnumDcIds() const {
	auto result = std::vector<DcId>();
	{
		ReadLocker lock(this);
		result.reserve(_data.size());
		for (auto &item : _data) {
			const auto dcId = item.first;
			Assert(!item.second.empty());
			if (!isCdnDc(item.second.front().flags)
				&& !isTemporaryDcId(dcId)) {
				result.push_back(dcId);
			}
		}
	}
	ranges::sort(result);
	return result;
}

DcType DcOptions::dcType(ShiftedDcId shiftedDcId) const {
	if (isTemporaryDcId(shiftedDcId)) {
		return DcType::Temporary;
	}
	ReadLocker lock(this);
	const auto dcId = BareDcId(shiftedDcId);
	if (_cdnDcIds.find(dcId) != _cdnDcIds.cend()) {
		return DcType::Cdn;
	}
	if (isMediaClusterDcId(shiftedDcId)
		&& hasMediaOnlyOptionsForGuarded(dcId)) {
		return DcType::MediaCluster;
	}
	return DcType::Regular;
}

void DcOptions::setCDNConfig(const MTPDcdnConfig &config) {
	WriteLocker lock(this);
	_cdnPublicKeys.clear();
	for (const auto &key : config.vpublic_keys().v) {
		key.match([&](const MTPDcdnPublicKey &data) {
			const auto keyBytes = bytes::make_span(data.vpublic_key().v);
			auto key = RSAPublicKey(keyBytes);
			if (key.valid()) {
				_cdnPublicKeys[data.vdc_id().v].emplace(
					key.fingerprint(),
					std::move(key));
			} else {
				LOG(("MTP Error: could not read this public RSA key:"));
				LOG((qs(data.vpublic_key())));
			}
		});
	}
	lock.unlock();

	_cdnConfigChanged.fire({});
}

bool DcOptions::hasCDNKeysForDc(DcId dcId) const {
	ReadLocker lock(this);
	return _cdnPublicKeys.find(dcId) != _cdnPublicKeys.cend();
}

RSAPublicKey DcOptions::getDcRSAKey(
		DcId dcId,
		const QVector<MTPlong> &fingerprints) const {
	const auto findKey = [&](const PublicKeyMap &keys) {
		for (const auto &fingerprint : fingerprints) {
			const auto it = keys.find(static_cast<uint64>(fingerprint.v));
			if (it != keys.cend()) {
				return it->second;
			}
		}
		return RSAPublicKey();
	};

	ReadLocker lock(this);
	const auto bareDcId = BareDcId(dcId);
	const auto cdnKeys = _cdnPublicKeys.find(bareDcId);
	if (cdnKeys != _cdnPublicKeys.cend()) {
		return findKey(cdnKeys->second);
	}
	if (_cdnDcIds.find(bareDcId) != _cdnDcIds.cend()) {
		return findKey(_publicKeys);
	}

	const auto custom = customEndpointStateGuarded();
	if (custom.data) {
		const auto route = custom.data->find(bareDcId);
		if (route != custom.data->cend()
			&& custom.profile
			&& !custom.profile->publicKeyPem.isEmpty()) {
			return custom.keys
				? findKey(*custom.keys)
				: RSAPublicKey();
		}
	}
	return findKey(_publicKeys);
}

auto DcOptions::lookup(
		DcId dcId,
		DcType type,
		bool throughProxy) const -> Variants {
	using Flag = Flag;
	auto result = Variants();

	ReadLocker lock(this);
	const auto base = _data.find(dcId);
	const auto baseIsCdn = base != end(_data)
		&& !base->second.empty()
		&& isCdnDc(base->second.front().flags);
	const auto custom = (type == DcType::Cdn || baseIsCdn)
		? nullptr
		: customEndpointDataGuarded();
	auto endpoints = static_cast<const std::vector<Endpoint>*>(nullptr);
	if (custom) {
		const auto customEntry = custom->find(dcId);
		if (customEntry != end(*custom)) {
			endpoints = &customEntry->second;
		}
	}
	if (!endpoints && base != end(_data)) {
		endpoints = &base->second;
	}
	if (!endpoints) {
		return result;
	}
	for (const auto &endpoint : *endpoints) {
		const auto flags = endpoint.flags;
		if (type == DcType::Cdn && !(flags & Flag::f_cdn)) {
			continue;
		} else if (type != DcType::MediaCluster
			&& (flags & Flag::f_media_only)) {
			continue;
		} else if (!ValidateSecret(endpoint.secret)) {
			continue;
		}
		const auto address = (flags & Flag::f_ipv6)
			? Variants::IPv6
			: Variants::IPv4;
		result.data[address][Variants::Tcp].push_back(endpoint);
		if (!(flags & (Flag::f_tcpo_only | Flag::f_secret))) {
			result.data[address][Variants::Http].push_back(endpoint);
		}
	}
	if (type == DcType::MediaCluster) {
		FilterIfHasWithFlag(result, Flag::f_media_only);
	}
	if (throughProxy) {
		FilterIfHasWithFlag(result, Flag::f_static);
	}
	return result;
}

bool DcOptions::hasMediaOnlyOptionsFor(DcId dcId) const {
	ReadLocker lock(this);
	return hasMediaOnlyOptionsForGuarded(dcId);
}

bool DcOptions::hasMediaOnlyOptionsForGuarded(DcId dcId) const {
	const auto base = _data.find(dcId);
	const auto baseIsCdn = base != end(_data)
		&& !base->second.empty()
		&& isCdnDc(base->second.front().flags);
	const auto custom = baseIsCdn ? nullptr : customEndpointDataGuarded();
	auto endpoints = static_cast<const std::vector<Endpoint>*>(nullptr);
	if (custom) {
		const auto customEntry = custom->find(dcId);
		if (customEntry != end(*custom)) {
			endpoints = &customEntry->second;
		}
	}
	if (!endpoints && base != end(_data)) {
		endpoints = &base->second;
	}
	if (!endpoints) {
		return false;
	}
	for (const auto &endpoint : *endpoints) {
		const auto flags = endpoint.flags;
		if (flags & Flag::f_media_only) {
			return true;
		}
	}
	return false;
}

DcOptions::CustomEndpointState DcOptions::customEndpointStateGuarded() const {
	if (_customEndpointCandidate) {
		if (!_customEndpointCandidate->profile) {
			return {};
		}
		return {
			&*_customEndpointCandidate->profile,
			&_customEndpointCandidateData,
			&_customEndpointCandidateKeys,
		};
	}
	if (!_confirmedCustomEndpointProfile) {
		return {};
	}
	return {
		&*_confirmedCustomEndpointProfile,
		&_confirmedCustomEndpointData,
		&_confirmedCustomEndpointKeys,
	};
}

auto DcOptions::customEndpointStateSnapshotGuarded() const
-> CustomEndpointStateSnapshot {
	auto result = CustomEndpointStateSnapshot();
	const auto state = customEndpointStateGuarded();
	if (state.profile) {
		Assert(state.data != nullptr);
		Assert(state.keys != nullptr);
		result.data = *state.data;
		result.publicKeyPem = state.profile->publicKeyPem;
	}
	return result;
}

const DcOptions::EndpointMap *DcOptions::customEndpointDataGuarded() const {
	return customEndpointStateGuarded().data;
}

DcOptions::EndpointMap DcOptions::effectiveDataGuarded() const {
	auto result = _data;
	const auto custom = customEndpointDataGuarded();
	if (!custom) {
		return result;
	}
	for (const auto &item : *custom) {
		const auto base = _data.find(item.first);
		const auto baseIsCdn = base != end(_data)
			&& !base->second.empty()
			&& isCdnDc(base->second.front().flags);
		if (!baseIsCdn) {
			result[item.first] = item.second;
		}
	}
	return result;
}

void DcOptions::FilterIfHasWithFlag(Variants &variants, Flag flag) {
	const auto is = [&](const Endpoint &endpoint) {
		return (endpoint.flags & flag) != 0;
	};
	const auto has = [&](const std::vector<Endpoint> &list) {
		return ranges::any_of(list, is);
	};
	for (auto &byAddress : variants.data) {
		for (auto &list : byAddress) {
			if (has(list)) {
				list = ranges::views::all(
					list
				) | ranges::views::filter(
					is
				) | ranges::to_vector;
			}
		}
	}
}

void DcOptions::computeCdnDcIds() {
	_cdnDcIds.clear();
	for (auto &item : _data) {
		Assert(!item.second.empty());
		if (item.second.front().flags & Flag::f_cdn) {
			_cdnDcIds.insert(BareDcId(item.first));
		}
	}
}

bool DcOptions::loadFromFile(const QString &path) {
	QVector<MTPDcOption> options;

	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		LOG(("MTP Error: could not read '%1'").arg(path));
		return false;
	}
	QTextStream stream(&f);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	stream.setCodec("UTF-8");
#endif // Qt < 6.0.0
	while (!stream.atEnd()) {
		static const auto RegExp = QRegularExpression(R"(\s)");
		auto line = stream.readLine();
		auto components = line.split(RegExp, Qt::SkipEmptyParts);
		if (components.isEmpty() || components[0].startsWith('#')) {
			continue;
		}

		auto error = [line] {
			LOG(("MTP Error: in .tdesktop-endpoints expected 'dcId host port [tcpo_only] [media_only]', got '%1'").arg(line));
			return false;
		};
		if (components.size() < 3) {
			return error();
		}
		auto dcId = components[0].toInt();
		auto ip = components[1];
		auto port = components[2].toInt();
		auto host = QHostAddress();
		if (dcId <= 0 || dcId >= kDcShift || !host.setAddress(ip) || port <= 0) {
			return error();
		}
		auto flags = Flags(0);
		if (host.protocol() == QAbstractSocket::IPv6Protocol) {
			flags |= Flag::f_ipv6;
		}
		for (auto &option : components.mid(3)) {
			if (option.startsWith('#')) {
				break;
			} else if (option == u"tcpo_only"_q) {
				flags |= Flag::f_tcpo_only;
			} else if (option == u"media_only"_q) {
				flags |= Flag::f_media_only;
			} else {
				return error();
			}
		}
		options.push_back(MTP_dcOption(
			MTP_flags(flags),
			MTP_int(dcId),
			MTP_string(ip),
			MTP_int(port),
			MTPbytes()));
	}
	if (options.isEmpty()) {
		LOG(("MTP Error: in .tdesktop-endpoints expected at least one endpoint being provided."));
		return false;
	}

	_immutable = false;
	setFromList(MTP_vector<MTPDcOption>(options));
	_immutable = true;

	return true;
}

bool DcOptions::writeToFile(const QString &path) const {
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly)) {
		return false;
	}
	QTextStream stream(&f);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	stream.setCodec("UTF-8");
#endif // Qt < 6.0.0

	ReadLocker lock(this);
	for (const auto &item : _data) {
		for (const auto &option : item.second) {
			stream
				<< option.id
				<< ' '
				<< QString::fromStdString(option.ip)
				<< ' ' << option.port;
			if (option.flags & Flag::f_tcpo_only) {
				stream << " tcpo_only";
			}
			if (option.flags & Flag::f_media_only) {
				stream << " media_only";
			}
			stream << '\n';
		}
	}
	return true;
}

} // namespace MTP
