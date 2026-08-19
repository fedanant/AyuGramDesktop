/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/mtproto_custom_endpoint.h"

#include "mtproto/mtproto_dc_options.h"

#include <QtCore/QDataStream>
#include <QtCore/QIODevice>
#include <QtCore/QRegularExpression>
#include <QtNetwork/QHostAddress>
#include <map>
#include <set>
#include <tuple>

namespace MTP {
namespace {

constexpr auto kLegacyPayloadVersion = 1;
constexpr auto kPayloadVersion = 2;
constexpr auto kRsaModulusSize = 256;

using Flag = MTPDdcOption::Flag;
using Flags = MTPDdcOption::Flags;

[[nodiscard]] CustomEndpointValidation Error(
		CustomEndpointError error,
		int line = 0,
		int row = 0,
		DcId dcId = 0,
		bool requiresLocalNetworkConfirmation = false) {
	return {
		error,
		line,
		row,
		dcId,
		requiresLocalNetworkConfirmation,
	};
}

[[nodiscard]] int ExpectedDcCount(Environment environment) {
	switch (environment) {
	case Environment::Production:
		return 5;
	case Environment::Test:
		return 3;
	}
	return 0;
}

[[nodiscard]] int RawFlags(Flags flags) {
	return qint32(flags);
}

[[nodiscard]] int SupportedFlagsMask() {
	return qint32(Flag::f_ipv6)
		| qint32(Flag::f_tcpo_only)
		| qint32(Flag::f_media_only);
}

[[nodiscard]] bool IsAsciiWhitespace(char value) {
	return value == ' '
		|| value == '\t'
		|| value == '\n'
		|| value == '\v'
		|| value == '\f'
		|| value == '\r';
}

[[nodiscard]] QByteArray TrimAsciiWhitespace(const QByteArray &value) {
	auto begin = 0;
	auto end = value.size();
	while (begin != end && IsAsciiWhitespace(value[begin])) {
		++begin;
	}
	while (end != begin && IsAsciiWhitespace(value[end - 1])) {
		--end;
	}
	return value.mid(begin, end - begin);
}

[[nodiscard]] bool HasExactPublicKeyEnvelope(const QByteArray &pem) {
	static const auto RsaBegin = QByteArray(
		"-----BEGIN RSA PUBLIC KEY-----");
	static const auto RsaEnd = QByteArray(
		"-----END RSA PUBLIC KEY-----");
	static const auto SpkiBegin = QByteArray(
		"-----BEGIN PUBLIC KEY-----");
	static const auto SpkiEnd = QByteArray(
		"-----END PUBLIC KEY-----");

	const auto end = pem.startsWith(RsaBegin)
		? RsaEnd
		: pem.startsWith(SpkiBegin)
		? SpkiEnd
		: QByteArray();
	if (end.isEmpty() || !pem.endsWith(end)) {
		return false;
	}
	const auto endPosition = pem.size() - end.size();
	return pem.indexOf("-----BEGIN ", 1) < 0
		&& pem.indexOf("-----END ") == endPosition;
}

[[nodiscard]] bool IsAsciiDecimal(QStringView value) {
	if (value.isEmpty()) {
		return false;
	}
	for (const auto character : value) {
		if (character < u'0' || character > u'9') {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool IsIpv6Unspecified(const Q_IPV6ADDR &address) {
	for (auto i = 0; i != 16; ++i) {
		if (address.c[i] != 0) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool IsIpv6Loopback(const Q_IPV6ADDR &address) {
	for (auto i = 0; i != 15; ++i) {
		if (address.c[i] != 0) {
			return false;
		}
	}
	return address.c[15] == 1;
}

[[nodiscard]] bool ExtractIpv4(
		const QHostAddress &address,
		quint32 &result) {
	if (address.protocol() == QAbstractSocket::IPv4Protocol) {
		auto ok = false;
		result = address.toIPv4Address(&ok);
		return ok;
	} else if (address.protocol() == QAbstractSocket::IPv6Protocol) {
		const auto value = address.toIPv6Address();
		for (auto i = 0; i != 10; ++i) {
			if (value.c[i] != 0) {
				return false;
			}
		}
		if (value.c[10] != 0xFFU || value.c[11] != 0xFFU) {
			return false;
		}
		result = (quint32(value.c[12]) << 24)
			| (quint32(value.c[13]) << 16)
			| (quint32(value.c[14]) << 8)
			| quint32(value.c[15]);
		return true;
	}
	return false;
}

[[nodiscard]] bool IsUnspecified(const QHostAddress &address) {
	auto ipv4 = quint32(0);
	if (ExtractIpv4(address, ipv4)) {
		return ipv4 == 0;
	} else if (address.protocol() == QAbstractSocket::IPv6Protocol) {
		return IsIpv6Unspecified(address.toIPv6Address());
	}
	return true;
}

[[nodiscard]] bool IsMulticast(const QHostAddress &address) {
	auto ipv4 = quint32(0);
	if (ExtractIpv4(address, ipv4)) {
		return (ipv4 & 0xF0000000U) == 0xE0000000U;
	} else if (address.protocol() == QAbstractSocket::IPv6Protocol) {
		return address.toIPv6Address().c[0] == 0xFFU;
	}
	return false;
}

[[nodiscard]] bool IsBroadcast(const QHostAddress &address) {
	auto ipv4 = quint32(0);
	return ExtractIpv4(address, ipv4) && ipv4 == 0xFFFFFFFFU;
}

[[nodiscard]] bool IsLocalNetwork(const QHostAddress &address) {
	auto ipv4 = quint32(0);
	if (ExtractIpv4(address, ipv4)) {
		return (ipv4 & 0xFF000000U) == 0x0A000000U
			|| (ipv4 & 0xFFF00000U) == 0xAC100000U
			|| (ipv4 & 0xFFFF0000U) == 0xC0A80000U
			|| (ipv4 & 0xFF000000U) == 0x7F000000U
			|| (ipv4 & 0xFFFF0000U) == 0xA9FE0000U;
	} else if (address.protocol() == QAbstractSocket::IPv6Protocol) {
		const auto value = address.toIPv6Address();
		return IsIpv6Loopback(value)
			|| (value.c[0] & 0xFEU) == 0xFCU
			|| (value.c[0] == 0xFEU
				&& (value.c[1] & 0xC0U) == 0x80U);
	}
	return false;
}

[[nodiscard]] QString CanonicalAddress(const std::string &ip) {
	auto address = QHostAddress();
	return address.setAddress(QString::fromStdString(ip))
		? address.toString()
		: QString();
}

[[nodiscard]] int SourceLine(
		const std::vector<int> *sourceLines,
		int row) {
	return sourceLines ? (*sourceLines)[row - 1] : row;
}

[[nodiscard]] CustomEndpointValidation Validate(
		const CustomEndpointProfile &profile,
		const std::vector<int> *sourceLines,
		QByteArray *normalizedPublicKeyPem = nullptr) {
	if (normalizedPublicKeyPem) {
		normalizedPublicKeyPem->clear();
	}
	const auto expectedDcCount = ExpectedDcCount(profile.environment);
	if (!expectedDcCount) {
		return Error(CustomEndpointError::UnsupportedEnvironment);
	} else if (profile.endpoints.empty()) {
		return Error(CustomEndpointError::EmptyProfile);
	} else if (profile.endpoints.size() > kCustomEndpointMaxRows) {
		return Error(CustomEndpointError::TooManyRows);
	}

	auto result = CustomEndpointValidation();
	auto covered = std::set<DcId>();
	auto exact = std::set<std::tuple<DcId, std::string, int, int>>();
	auto addressFlags = std::map<std::tuple<DcId, std::string, int>, int>();
	for (auto index = 0; index != profile.endpoints.size(); ++index) {
		const auto &endpoint = profile.endpoints[index];
		const auto row = int(index + 1);
		const auto line = SourceLine(sourceLines, row);
		if (endpoint.dcId < 1 || endpoint.dcId > expectedDcCount) {
			return Error(
				CustomEndpointError::InvalidDcId,
				line,
				row,
				endpoint.dcId,
				result.requiresLocalNetworkConfirmation);
		} else if (endpoint.port < 1 || endpoint.port > 65535) {
			return Error(
				CustomEndpointError::InvalidPort,
				line,
				row,
				endpoint.dcId,
				result.requiresLocalNetworkConfirmation);
		}

		const auto rawFlags = RawFlags(endpoint.flags);
		if (rawFlags & ~SupportedFlagsMask()) {
			return Error(
				CustomEndpointError::UnsupportedFlags,
				line,
				row,
				endpoint.dcId,
				result.requiresLocalNetworkConfirmation);
		}

		auto address = QHostAddress();
		if (!address.setAddress(QString::fromStdString(endpoint.ip))
			|| !address.scopeId().isEmpty()) {
			return Error(
				CustomEndpointError::InvalidAddress,
				line,
				row,
				endpoint.dcId,
				result.requiresLocalNetworkConfirmation);
		}
		const auto isIpv6 = address.protocol()
			== QAbstractSocket::IPv6Protocol;
		if (isIpv6 != bool(endpoint.flags & Flag::f_ipv6)) {
			return Error(
				CustomEndpointError::AddressFamilyMismatch,
				line,
				row,
				endpoint.dcId,
				result.requiresLocalNetworkConfirmation);
		} else if (IsUnspecified(address)) {
			return Error(
				CustomEndpointError::UnspecifiedAddress,
				line,
				row,
				endpoint.dcId,
				result.requiresLocalNetworkConfirmation);
		} else if (IsMulticast(address)) {
			return Error(
				CustomEndpointError::MulticastAddress,
				line,
				row,
				endpoint.dcId,
				result.requiresLocalNetworkConfirmation);
		} else if (IsBroadcast(address)) {
			return Error(
				CustomEndpointError::BroadcastAddress,
				line,
				row,
				endpoint.dcId,
				result.requiresLocalNetworkConfirmation);
		}

		result.requiresLocalNetworkConfirmation |= IsLocalNetwork(address);
		const auto canonical = address.toString().toStdString();
		const auto addressKey = std::make_tuple(
			endpoint.dcId,
			canonical,
			endpoint.port);
		const auto exactKey = std::make_tuple(
			endpoint.dcId,
			canonical,
			endpoint.port,
			rawFlags);
		if (!exact.emplace(exactKey).second) {
			return Error(
				CustomEndpointError::DuplicateEndpoint,
				line,
				row,
				endpoint.dcId,
				result.requiresLocalNetworkConfirmation);
		}
		const auto existing = addressFlags.find(addressKey);
		if (existing != addressFlags.end() && existing->second != rawFlags) {
			return Error(
				CustomEndpointError::ConflictingEndpoint,
				line,
				row,
				endpoint.dcId,
				result.requiresLocalNetworkConfirmation);
		}
		addressFlags.emplace(addressKey, rawFlags);
		if (!(endpoint.flags & Flag::f_media_only)) {
			covered.emplace(endpoint.dcId);
		}
	}

	for (auto dcId = DcId(1); dcId <= expectedDcCount; ++dcId) {
		if (covered.find(dcId) == covered.end()) {
			return Error(
				CustomEndpointError::MissingDc,
				0,
				0,
				dcId,
				result.requiresLocalNetworkConfirmation);
		}
	}
	if (!profile.publicKeyPem.isEmpty()) {
		auto publicKey = ValidateCustomEndpointPublicKey(
			profile.publicKeyPem);
		if (!IsValidCustomEndpoint(publicKey.validation)) {
			publicKey.validation.requiresLocalNetworkConfirmation
				= result.requiresLocalNetworkConfirmation;
			return publicKey.validation;
		} else if (normalizedPublicKeyPem) {
			*normalizedPublicKeyPem = std::move(publicKey.publicKeyPem);
		}
	}
	return result;
}

[[nodiscard]] bool ReadAddress(
		QDataStream &stream,
		qint32 size,
		std::string &result) {
	if (size <= 0
		|| size > kCustomEndpointMaxAddressSize
		|| !stream.device()
		|| stream.device()->bytesAvailable() < size) {
		return false;
	}
	result.resize(size);
	return stream.readRawData(result.data(), size) == size;
}

[[nodiscard]] bool ReadPublicKey(
		QDataStream &stream,
		qint32 size,
		QByteArray &result) {
	if (size < 0
		|| size > kCustomEndpointMaxPublicKeySize
		|| !stream.device()
		|| stream.device()->bytesAvailable() < size) {
		return false;
	}
	result.resize(size);
	return !size || stream.readRawData(result.data(), size) == size;
}

} // namespace

CustomEndpointPublicKeyResult ValidateCustomEndpointPublicKey(
		const QByteArray &publicKeyPem) {
	auto result = CustomEndpointPublicKeyResult();
	if (publicKeyPem.size() > kCustomEndpointMaxPublicKeySize) {
		result.validation = Error(CustomEndpointError::PublicKeyTooLarge);
		return result;
	}

	result.publicKeyPem = TrimAsciiWhitespace(publicKeyPem);
	if (result.publicKeyPem.isEmpty()) {
		result.validation = Error(CustomEndpointError::MissingPublicKey);
		return result;
	} else if (result.publicKeyPem.contains("PRIVATE KEY")) {
		result.validation = Error(CustomEndpointError::PrivateKeyMaterial);
		return result;
	} else if (!HasExactPublicKeyEnvelope(result.publicKeyPem)) {
		result.validation = Error(
			CustomEndpointError::UnsupportedPublicKeyEnvelope);
		return result;
	}

	result.key = details::RSAPublicKey(bytes::make_span(result.publicKeyPem));
	if (!result.key.valid()) {
		result.validation = Error(CustomEndpointError::MalformedPublicKey);
		return result;
	}
	const auto modulus = result.key.getN();
	if (modulus.size() != kRsaModulusSize
		|| !(gsl::to_integer<unsigned char>(modulus.front()) & 0x80U)) {
		result.key = details::RSAPublicKey();
		result.validation = Error(CustomEndpointError::InvalidPublicKeySize);
		return result;
	}
	result.fingerprint = result.key.fingerprint();
	return result;
}

bool operator==(const CustomEndpoint &a, const CustomEndpoint &b) {
	return a.dcId == b.dcId
		&& a.flags == b.flags
		&& a.ip == b.ip
		&& a.port == b.port;
}

bool operator!=(const CustomEndpoint &a, const CustomEndpoint &b) {
	return !(a == b);
}

bool operator==(
		const CustomEndpointProfile &a,
		const CustomEndpointProfile &b) {
	return a.environment == b.environment
		&& a.endpoints == b.endpoints
		&& a.publicKeyPem == b.publicKeyPem;
}

bool operator!=(
		const CustomEndpointProfile &a,
		const CustomEndpointProfile &b) {
	return !(a == b);
}

bool IsValidCustomEndpoint(const CustomEndpointValidation &validation) {
	return validation.error == CustomEndpointError::None;
}

CustomEndpointValidation ValidateCustomEndpointProfile(
		const CustomEndpointProfile &profile) {
	return Validate(profile, nullptr);
}

CustomEndpointProfileResult ParseCustomEndpointProfile(
		QStringView text,
		Environment environment) {
	auto result = CustomEndpointProfileResult();
	result.profile.environment = environment;
	const auto source = text.toString();
	if (source.toUtf8().size() > kCustomEndpointMaxTextSize) {
		result.validation = Error(CustomEndpointError::TextTooLong);
		return result;
	}

	static const auto Lines = QRegularExpression(uR"(\r\n|\n|\r)"_q);
	static const auto Space = QRegularExpression(uR"(\s+)"_q);
	const auto lines = source.split(Lines, Qt::KeepEmptyParts);
	auto sourceLines = std::vector<int>();
	for (auto lineIndex = 0; lineIndex != lines.size(); ++lineIndex) {
		const auto components = lines[lineIndex].split(
			Space,
			Qt::SkipEmptyParts);
		if (components.empty()) {
			continue;
		}
		const auto line = lineIndex + 1;
		const auto row = int(result.profile.endpoints.size() + 1);
		if (row > kCustomEndpointMaxRows) {
			result.validation = Error(
				CustomEndpointError::TooManyRows,
				line,
				row);
			return result;
		} else if (components.size() < 3 || components.size() > 5) {
			result.validation = Error(
				CustomEndpointError::InvalidRow,
				line,
				row);
			return result;
		}

		auto dcOk = false;
		const auto dcId = IsAsciiDecimal(components[0])
			? components[0].toInt(&dcOk)
			: 0;
		if (!dcOk) {
			result.validation = Error(
				CustomEndpointError::InvalidDcId,
				line,
				row);
			return result;
		}

		auto address = QHostAddress();
		if (!address.setAddress(components[1])
			|| !address.scopeId().isEmpty()) {
			result.validation = Error(
				CustomEndpointError::InvalidAddress,
				line,
				row,
				dcId);
			return result;
		}

		auto portOk = false;
		const auto port = IsAsciiDecimal(components[2])
			? components[2].toInt(&portOk)
			: 0;
		if (!portOk) {
			result.validation = Error(
				CustomEndpointError::InvalidPort,
				line,
				row,
				dcId);
			return result;
		}

		auto flags = Flags();
		if (address.protocol() == QAbstractSocket::IPv6Protocol) {
			flags |= Flag::f_ipv6;
		}
		auto tcpoOnly = false;
		auto mediaOnly = false;
		for (auto i = 3; i != components.size(); ++i) {
			if (components[i] == u"tcpo_only"_q) {
				if (tcpoOnly) {
					result.validation = Error(
						CustomEndpointError::DuplicateFlag,
						line,
						row,
						dcId);
					return result;
				}
				tcpoOnly = true;
				flags |= Flag::f_tcpo_only;
			} else if (components[i] == u"media_only"_q) {
				if (mediaOnly) {
					result.validation = Error(
						CustomEndpointError::DuplicateFlag,
						line,
						row,
						dcId);
					return result;
				}
				mediaOnly = true;
				flags |= Flag::f_media_only;
			} else {
				result.validation = Error(
					CustomEndpointError::UnknownFlag,
					line,
					row,
					dcId);
				return result;
			}
		}

		result.profile.endpoints.push_back({
			dcId,
			flags,
			address.toString().toStdString(),
			port,
		});
		sourceLines.push_back(line);
	}
	result.validation = Validate(result.profile, &sourceLines);
	return result;
}

QString FormatCustomEndpointProfile(const CustomEndpointProfile &profile) {
	if (!IsValidCustomEndpoint(ValidateCustomEndpointProfile(profile))) {
		return QString();
	}
	auto lines = QStringList();
	lines.reserve(profile.endpoints.size());
	for (const auto &endpoint : profile.endpoints) {
		auto line = QString::number(endpoint.dcId)
			+ u' '
			+ CanonicalAddress(endpoint.ip)
			+ u' '
			+ QString::number(endpoint.port);
		if (endpoint.flags & Flag::f_tcpo_only) {
			line += u" tcpo_only"_q;
		}
		if (endpoint.flags & Flag::f_media_only) {
			line += u" media_only"_q;
		}
		lines.push_back(std::move(line));
	}
	return lines.join(u'\n');
}

CustomEndpointPayloadResult SerializeCustomEndpointProfile(
		const CustomEndpointProfile &profile) {
	auto result = CustomEndpointPayloadResult();
	auto publicKeyPem = QByteArray();
	result.validation = Validate(profile, nullptr, &publicKeyPem);
	if (!IsValidCustomEndpoint(result.validation)) {
		return result;
	}

	auto size = 4 * int(sizeof(qint32)) + publicKeyPem.size();
	for (const auto &endpoint : profile.endpoints) {
		size += 4 * int(sizeof(qint32));
		size += CanonicalAddress(endpoint.ip).toLatin1().size();
	}
	if (size > kCustomEndpointMaxPayloadSize) {
		result.validation = Error(CustomEndpointError::PayloadTooLarge);
		return result;
	}
	result.payload.reserve(size);
	QDataStream stream(&result.payload, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_1);
	stream
		<< qint32(kPayloadVersion)
		<< qint32(profile.environment)
		<< qint32(profile.endpoints.size());
	auto rawWritesValid = true;
	for (const auto &endpoint : profile.endpoints) {
		const auto address = CanonicalAddress(endpoint.ip).toLatin1();
		stream
			<< qint32(endpoint.dcId)
			<< qint32(endpoint.flags)
			<< qint32(endpoint.port)
			<< qint32(address.size());
		rawWritesValid = rawWritesValid
			&& (stream.writeRawData(
				address.constData(),
				address.size()) == address.size());
	}
	stream << qint32(publicKeyPem.size());
	if (!publicKeyPem.isEmpty()) {
		rawWritesValid = rawWritesValid
			&& (stream.writeRawData(
				publicKeyPem.constData(),
				publicKeyPem.size()) == publicKeyPem.size());
	}
	if (!rawWritesValid || stream.status() != QDataStream::Ok) {
		result.payload.clear();
		result.validation = Error(CustomEndpointError::InvalidPayload);
	} else if (result.payload.size() > kCustomEndpointMaxPayloadSize) {
		result.payload.clear();
		result.validation = Error(CustomEndpointError::PayloadTooLarge);
	}
	return result;
}

CustomEndpointProfileResult DeserializeCustomEndpointProfile(
		const QByteArray &payload) {
	auto result = CustomEndpointProfileResult();
	if (payload.size() > kCustomEndpointMaxPayloadSize) {
		result.validation = Error(CustomEndpointError::PayloadTooLarge);
		return result;
	}

	QDataStream stream(payload);
	stream.setVersion(QDataStream::Qt_5_1);
	auto version = qint32(0);
	auto environment = qint32(0);
	auto count = qint32(0);
	stream >> version >> environment >> count;
	if (stream.status() != QDataStream::Ok) {
		result.validation = Error(CustomEndpointError::InvalidPayload);
		return result;
	} else if (version != kLegacyPayloadVersion
		&& version != kPayloadVersion) {
		result.validation = Error(
			CustomEndpointError::UnsupportedPayloadVersion);
		return result;
	}

	switch (environment) {
	case qint32(Environment::Production):
		result.profile.environment = Environment::Production;
		break;
	case qint32(Environment::Test):
		result.profile.environment = Environment::Test;
		break;
	default:
		result.validation = Error(
			CustomEndpointError::UnsupportedEnvironment);
		return result;
	}
	if (count < 0 || count > kCustomEndpointMaxRows) {
		result.validation = Error(CustomEndpointError::TooManyRows);
		return result;
	}
	result.profile.endpoints.reserve(count);
	for (auto index = 0; index != count; ++index) {
		auto dcId = qint32(0);
		auto flags = qint32(0);
		auto port = qint32(0);
		auto addressSize = qint32(0);
		stream >> dcId >> flags >> port >> addressSize;
		auto address = std::string();
		if (stream.status() != QDataStream::Ok
			|| !ReadAddress(stream, addressSize, address)) {
			result.validation = Error(
				CustomEndpointError::InvalidPayload,
				index + 1,
				index + 1,
				dcId);
			return result;
		}
		result.profile.endpoints.push_back({
			dcId,
			Flags::from_raw(flags),
			std::move(address),
			port,
		});
	}
	if (stream.status() != QDataStream::Ok) {
		result.validation = Error(CustomEndpointError::InvalidPayload);
		return result;
	}
	if (version == kPayloadVersion) {
		if (!stream.device()
			|| stream.device()->bytesAvailable() < int(sizeof(qint32))) {
			result.validation = Error(CustomEndpointError::InvalidPayload);
			return result;
		}
		auto publicKeySize = qint32(0);
		stream >> publicKeySize;
		if (stream.status() != QDataStream::Ok || publicKeySize < 0) {
			result.validation = Error(CustomEndpointError::InvalidPayload);
			return result;
		} else if (publicKeySize > kCustomEndpointMaxPublicKeySize) {
			result.validation = Error(CustomEndpointError::PublicKeyTooLarge);
			return result;
		} else if (!ReadPublicKey(
				stream,
				publicKeySize,
				result.profile.publicKeyPem)) {
			result.validation = Error(CustomEndpointError::InvalidPayload);
			return result;
		}
	}
	if (!stream.atEnd()) {
		result.validation = Error(CustomEndpointError::TrailingPayloadData);
		return result;
	}

	auto publicKeyPem = QByteArray();
	result.validation = Validate(result.profile, nullptr, &publicKeyPem);
	if (IsValidCustomEndpoint(result.validation)) {
		for (auto &endpoint : result.profile.endpoints) {
			endpoint.ip = CanonicalAddress(endpoint.ip).toStdString();
		}
		result.profile.publicKeyPem = std::move(publicKeyPem);
	}
	return result;
}

} // namespace MTP
