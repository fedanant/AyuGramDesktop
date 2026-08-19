/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/details/mtproto_rsa_public_key.h"
#include "mtproto/core_types.h"
#include "scheme.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringView>
#include <string>
#include <vector>

namespace MTP {

enum class Environment : uchar;

inline constexpr auto kCustomEndpointMaxTextSize = 16 * 1024;
inline constexpr auto kCustomEndpointMaxPayloadSize = 16 * 1024;
inline constexpr auto kCustomEndpointMaxRows = 64;
inline constexpr auto kCustomEndpointMaxAddressSize = 45;
inline constexpr auto kCustomEndpointMaxPublicKeySize = 4 * 1024;

struct CustomEndpoint {
	DcId dcId = 0;
	MTPDdcOption::Flags flags = MTPDdcOption::Flags();
	std::string ip;
	int port = 0;
};

struct CustomEndpointProfile {
	Environment environment = Environment();
	std::vector<CustomEndpoint> endpoints;
	QByteArray publicKeyPem;
};

enum class CustomEndpointError {
	None,
	TextTooLong,
	TooManyRows,
	EmptyProfile,
	InvalidRow,
	InvalidDcId,
	InvalidAddress,
	InvalidPort,
	UnknownFlag,
	DuplicateFlag,
	UnsupportedEnvironment,
	UnsupportedFlags,
	AddressFamilyMismatch,
	UnspecifiedAddress,
	MulticastAddress,
	BroadcastAddress,
	DuplicateEndpoint,
	ConflictingEndpoint,
	MissingDc,
	MissingPublicKey,
	PublicKeyTooLarge,
	PrivateKeyMaterial,
	UnsupportedPublicKeyEnvelope,
	MalformedPublicKey,
	InvalidPublicKeySize,
	PayloadTooLarge,
	UnsupportedPayloadVersion,
	InvalidPayload,
	TrailingPayloadData,
};

struct CustomEndpointValidation {
	CustomEndpointError error = CustomEndpointError::None;
	int line = 0;
	int row = 0;
	DcId dcId = 0;
	bool requiresLocalNetworkConfirmation = false;
};

struct CustomEndpointProfileResult {
	CustomEndpointProfile profile;
	CustomEndpointValidation validation;
};

struct CustomEndpointPublicKeyResult {
	CustomEndpointValidation validation;
	QByteArray publicKeyPem;
	details::RSAPublicKey key;
	uint64 fingerprint = 0;
};

struct CustomEndpointPayloadResult {
	QByteArray payload;
	CustomEndpointValidation validation;
};

[[nodiscard]] bool operator==(
	const CustomEndpoint &a,
	const CustomEndpoint &b);
[[nodiscard]] bool operator!=(
	const CustomEndpoint &a,
	const CustomEndpoint &b);
[[nodiscard]] bool operator==(
	const CustomEndpointProfile &a,
	const CustomEndpointProfile &b);
[[nodiscard]] bool operator!=(
	const CustomEndpointProfile &a,
	const CustomEndpointProfile &b);

[[nodiscard]] bool IsValidCustomEndpoint(
	const CustomEndpointValidation &validation);
[[nodiscard]] CustomEndpointPublicKeyResult ValidateCustomEndpointPublicKey(
	const QByteArray &publicKeyPem);
[[nodiscard]] CustomEndpointValidation ValidateCustomEndpointProfile(
	const CustomEndpointProfile &profile);
[[nodiscard]] CustomEndpointProfileResult ParseCustomEndpointProfile(
	QStringView text,
	Environment environment);
[[nodiscard]] QString FormatCustomEndpointProfile(
	const CustomEndpointProfile &profile);
[[nodiscard]] CustomEndpointPayloadResult SerializeCustomEndpointProfile(
	const CustomEndpointProfile &profile);
[[nodiscard]] CustomEndpointProfileResult DeserializeCustomEndpointProfile(
	const QByteArray &payload);

} // namespace MTP
