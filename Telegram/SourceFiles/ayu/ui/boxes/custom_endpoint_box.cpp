/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ayu/ui/boxes/custom_endpoint_box.h"

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_text_entity.h"
#include "main/main_account.h"
#include "mtproto/facade.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_custom_endpoint.h"
#include "mtproto/mtproto_dc_options.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "lang_auto.h"

#include <QtCore/QPointer>

#include "styles/style_ayu_settings.h"

namespace Ayu {
namespace {

constexpr auto kTestTimeout = crl::time(15 * 1000);

enum class EditorStatus {
	Idle,
	Connecting,
	Probing,
	Confirmed,
	Reverted,
	TimeoutReverted,
	RpcReverted,
	ParseReverted,
};

enum class InputMode {
	SingleServer,
	Advanced,
};

enum class FormErrorType {
	None,
	Validation,
	TestInProgress,
	Unavailable,
	MtProxy,
};

enum class TestFailure {
	Cancelled,
	Timeout,
	Rpc,
	Parse,
	MtProxy,
};

struct FormError {
	FormErrorType type = FormErrorType::None;
	MTP::CustomEndpointValidation validation;
};

struct SingleServer {
	QString ip;
	int port = 0;
};

class CustomEndpointEditor final : public base::has_weak_ptr {
public:
	CustomEndpointEditor(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Account*> account);
	~CustomEndpointEditor();

private:
	void setup(not_null<Main::Account*> account);
	void apply();
	void reset();
	void confirmLocalNetwork(MTP::CustomEndpointProfile profile);
	void beginCandidate(
		std::optional<MTP::CustomEndpointProfile> profile);
	void handleConnectionState(
		uint64 generation,
		MTP::ConnectionState state);
	void sendProbe(uint64 generation, MTP::ShiftedDcId shiftedDcId);
	bool probeDone(uint64 generation, const MTP::Response &response);
	bool probeFailed(uint64 generation, const MTP::Response &response);
	void completeSuccess(uint64 generation);
	void failActive(
		TestFailure failure,
		Main::Account *knownAccount = nullptr);
	void cancelAndClose();
	void accountTeardown(not_null<Main::Account*> account);
	void inputChanged();
	void setInputMode(InputMode mode, anim::type animated);
	void setFormFromProfile(
		const std::optional<MTP::CustomEndpointProfile> &profile);
	[[nodiscard]] auto publicKeyDescription()
		-> rpl::producer<TextWithEntities>;
	[[nodiscard]] auto publicKeyFeedback()
		-> rpl::producer<TextWithEntities>;
	[[nodiscard]] MTP::CustomEndpointProfileResult parseForm(
		MTP::Environment environment) const;
	void refreshConfirmedProfile();
	void setControlsEnabled(bool enabled);
	void showFailure(TestFailure failure);
	void showSuccess();
	[[nodiscard]] bool isGeneration(uint64 generation) const;

	const QPointer<Ui::GenericBox> _box;
	const std::shared_ptr<Ui::Show> _show;
	base::weak_ptr<Main::Account> _account;
	base::Timer _timeout;
	rpl::lifetime _testLifetime;
	rpl::variable<std::optional<MTP::CustomEndpointProfile>> _confirmed;
	rpl::variable<FormError> _formError;
	rpl::variable<EditorStatus> _status = EditorStatus::Idle;
	std::shared_ptr<Ui::RadiobuttonGroup> _modeGroup;
	QPointer<Ui::Radiobutton> _singleServerOption;
	QPointer<Ui::Radiobutton> _advancedOption;
	QPointer<Ui::SlideWrap<Ui::VerticalLayout>> _singleServerWrap;
	QPointer<Ui::SlideWrap<Ui::VerticalLayout>> _advancedWrap;
	QPointer<Ui::InputField> _ipField;
	QPointer<Ui::InputField> _portField;
	QPointer<Ui::InputField> _advancedField;
	QPointer<Ui::InputField> _publicKeyField;
	QPointer<Ui::RoundButton> _apply;
	QPointer<Ui::RoundButton> _reset;
	rpl::event_stream<> _modeChanged;
	rpl::event_stream<> _publicKeyChanged;
	std::optional<MTP::CustomEndpointProfile> _candidate;
	mtpRequestId _probeRequestId = 0;
	MTP::ShiftedDcId _probeDcId = 0;
	uint64 _nextGeneration = 0;
	uint64 _activeGeneration = 0;
	InputMode _inputMode = InputMode::SingleServer;
	bool _restartIssued = false;

};

[[nodiscard]] int ExpectedDcCount(MTP::Environment environment) {
	switch (environment) {
	case MTP::Environment::Production:
		return 5;
	case MTP::Environment::Test:
		return 3;
	}
	return 0;
}

[[nodiscard]] std::optional<SingleServer> ExtractSingleServer(
		const MTP::CustomEndpointProfile &profile) {
	using Flag = MTPDdcOption::Flag;
	if (profile.publicKeyPem.isEmpty()) {
		return std::nullopt;
	}
	const auto publicKey = MTP::ValidateCustomEndpointPublicKey(
		profile.publicKeyPem);
	if (!MTP::IsValidCustomEndpoint(publicKey.validation)) {
		return std::nullopt;
	}
	const auto count = ExpectedDcCount(profile.environment);
	if (!count || profile.endpoints.size() != count) {
		return std::nullopt;
	}
	auto covered = std::vector<bool>(count + 1);
	auto result = SingleServer();
	for (const auto &endpoint : profile.endpoints) {
		if (endpoint.dcId < 1
			|| endpoint.dcId > count
			|| covered[endpoint.dcId]
			|| (endpoint.flags & Flag::f_tcpo_only)
			|| (endpoint.flags & Flag::f_media_only)) {
			return std::nullopt;
		}
		covered[endpoint.dcId] = true;
		const auto ip = QString::fromStdString(endpoint.ip);
		if (result.ip.isEmpty()) {
			result = { ip, endpoint.port };
		} else if (result.ip != ip || result.port != endpoint.port) {
			return std::nullopt;
		}
	}
	return result;
}

[[nodiscard]] QString SingleServerProfileText(
		QString ip,
		int port,
		MTP::Environment environment) {
	auto lines = QStringList();
	const auto count = ExpectedDcCount(environment);
	lines.reserve(count);
	for (auto dcId = 1; dcId <= count; ++dcId) {
		lines.push_back(u"%1 %2 %3"_q
			.arg(dcId)
			.arg(ip.trimmed())
			.arg(port));
	}
	return lines.join(u'\n');
}

[[nodiscard]] bool IsMtProxyActive() {
	const auto &settings = Core::App().settings().proxy();
	return settings.isEnabled()
		&& (settings.selected().type == MTP::ProxyData::Type::Mtproto);
}

[[nodiscard]] bool IsPublicKeyError(MTP::CustomEndpointError error) {
	using Error = MTP::CustomEndpointError;
	switch (error) {
	case Error::MissingPublicKey:
	case Error::PublicKeyTooLarge:
	case Error::PrivateKeyMaterial:
	case Error::UnsupportedPublicKeyEnvelope:
	case Error::MalformedPublicKey:
	case Error::InvalidPublicKeySize:
		return true;
	default:
		return false;
	}
}

[[nodiscard]] QString PublicKeyFingerprint(uint64 fingerprint) {
	return u"0x%1"_q.arg(
		QString::number(fingerprint, 16).rightJustified(16, u'0'));
}

[[nodiscard]] auto PublicKeyDescription(InputMode mode)
-> rpl::producer<TextWithEntities> {
	return (mode == InputMode::SingleServer)
		? tr::ayu_custom_endpoints_public_key_teamgram_description(tr::rich)
		: tr::ayu_custom_endpoints_public_key_advanced_description(tr::rich);
}

[[nodiscard]] auto SummaryFor(
		const std::optional<MTP::CustomEndpointProfile> &profile)
-> rpl::producer<QString> {
	if (!profile) {
		return tr::ayu_custom_endpoints_summary_default();
	}
	return tr::ayu_custom_endpoints_summary_custom(
		lt_count,
		rpl::single(float64(profile->endpoints.size())) | tr::to_count());
}

[[nodiscard]] auto SummaryText(
		rpl::producer<std::optional<MTP::CustomEndpointProfile>> profile)
-> rpl::producer<QString> {
	return std::move(profile) | rpl::map([](
			const std::optional<MTP::CustomEndpointProfile> &profile) {
		return SummaryFor(profile);
	}) | rpl::flatten_latest();
}

[[nodiscard]] auto ValidationErrorText(
		const MTP::CustomEndpointValidation &validation)
-> rpl::producer<TextWithEntities> {
	using Error = MTP::CustomEndpointError;
	switch (validation.error) {
	case Error::None:
		return rpl::single(tr::marked());
	case Error::TextTooLong:
		return tr::ayu_custom_endpoints_error_text_too_long(tr::marked);
	case Error::TooManyRows:
		return tr::ayu_custom_endpoints_error_too_many_rows(tr::marked);
	case Error::EmptyProfile:
		return tr::ayu_custom_endpoints_error_empty(tr::marked);
	case Error::InvalidRow:
		return tr::ayu_custom_endpoints_error_invalid_row(
			lt_line,
			rpl::single(tr::marked(QString::number(validation.line))),
			tr::marked);
	case Error::InvalidDcId:
		return tr::ayu_custom_endpoints_error_invalid_dc(
			lt_line,
			rpl::single(tr::marked(QString::number(validation.line))),
			tr::marked);
	case Error::InvalidAddress:
		return tr::ayu_custom_endpoints_error_invalid_address(
			lt_line,
			rpl::single(tr::marked(QString::number(validation.line))),
			tr::marked);
	case Error::InvalidPort:
		return tr::ayu_custom_endpoints_error_invalid_port(
			lt_line,
			rpl::single(tr::marked(QString::number(validation.line))),
			tr::marked);
	case Error::UnknownFlag:
		return tr::ayu_custom_endpoints_error_unknown_flag(
			lt_line,
			rpl::single(tr::marked(QString::number(validation.line))),
			tr::marked);
	case Error::DuplicateFlag:
		return tr::ayu_custom_endpoints_error_duplicate_flag(
			lt_line,
			rpl::single(tr::marked(QString::number(validation.line))),
			tr::marked);
	case Error::UnsupportedEnvironment:
		return tr::ayu_custom_endpoints_error_environment(tr::marked);
	case Error::UnsupportedFlags:
		return tr::ayu_custom_endpoints_error_unsupported_flags(
			lt_line,
			rpl::single(tr::marked(QString::number(validation.line))),
			tr::marked);
	case Error::AddressFamilyMismatch:
		return tr::ayu_custom_endpoints_error_address_family(
			lt_line,
			rpl::single(tr::marked(QString::number(validation.line))),
			tr::marked);
	case Error::UnspecifiedAddress:
		return tr::ayu_custom_endpoints_error_unspecified_address(
			lt_line,
			rpl::single(tr::marked(QString::number(validation.line))),
			tr::marked);
	case Error::MulticastAddress:
		return tr::ayu_custom_endpoints_error_multicast_address(
			lt_line,
			rpl::single(tr::marked(QString::number(validation.line))),
			tr::marked);
	case Error::BroadcastAddress:
		return tr::ayu_custom_endpoints_error_broadcast_address(
			lt_line,
			rpl::single(tr::marked(QString::number(validation.line))),
			tr::marked);
	case Error::DuplicateEndpoint:
		return tr::ayu_custom_endpoints_error_duplicate_endpoint(
			lt_line,
			rpl::single(tr::marked(QString::number(validation.line))),
			tr::marked);
	case Error::ConflictingEndpoint:
		return tr::ayu_custom_endpoints_error_conflicting_endpoint(
			lt_line,
			rpl::single(tr::marked(QString::number(validation.line))),
			tr::marked);
	case Error::MissingDc:
		return tr::ayu_custom_endpoints_error_missing_dc(
			lt_dc,
			rpl::single(tr::marked(QString::number(validation.dcId))),
			tr::marked);
	case Error::MissingPublicKey:
		return tr::ayu_custom_endpoints_error_public_key_missing(tr::marked);
	case Error::PublicKeyTooLarge:
		return tr::ayu_custom_endpoints_error_public_key_too_large(tr::marked);
	case Error::PrivateKeyMaterial:
		return tr::ayu_custom_endpoints_error_public_key_private(tr::marked);
	case Error::UnsupportedPublicKeyEnvelope:
		return tr::ayu_custom_endpoints_error_public_key_envelope(tr::marked);
	case Error::MalformedPublicKey:
		return tr::ayu_custom_endpoints_error_public_key_malformed(tr::marked);
	case Error::InvalidPublicKeySize:
		return tr::ayu_custom_endpoints_error_public_key_size(tr::marked);
	case Error::PayloadTooLarge:
	case Error::UnsupportedPayloadVersion:
	case Error::InvalidPayload:
	case Error::TrailingPayloadData:
		return tr::ayu_custom_endpoints_error_invalid_profile(tr::marked);
	}
	Unexpected("Custom endpoint validation error.");
}

[[nodiscard]] auto PublicKeyFeedbackText(
		QString text,
		InputMode mode)
-> rpl::producer<TextWithEntities> {
	const auto bytes = text.toUtf8();
	if (bytes.trimmed().isEmpty()) {
		return (mode == InputMode::SingleServer)
			? tr::ayu_custom_endpoints_error_public_key_missing(tr::marked)
			: tr::ayu_custom_endpoints_public_key_official(tr::marked);
	}
	const auto result = MTP::ValidateCustomEndpointPublicKey(bytes);
	if (!MTP::IsValidCustomEndpoint(result.validation)) {
		return ValidationErrorText(result.validation);
	}
	return tr::ayu_custom_endpoints_public_key_fingerprint(
		lt_fingerprint,
		rpl::single(tr::marked(PublicKeyFingerprint(result.fingerprint))),
		tr::marked);
}

[[nodiscard]] auto FormErrorFor(const FormError &error)
-> rpl::producer<TextWithEntities> {
	switch (error.type) {
	case FormErrorType::None:
		return rpl::single(tr::marked());
	case FormErrorType::Validation:
		return ValidationErrorText(error.validation);
	case FormErrorType::TestInProgress:
		return tr::ayu_custom_endpoints_error_test_in_progress(tr::marked);
	case FormErrorType::Unavailable:
		return tr::ayu_custom_endpoints_error_unavailable(tr::marked);
	case FormErrorType::MtProxy:
		return tr::ayu_custom_endpoints_error_mtproxy(tr::marked);
	}
	Unexpected("Custom endpoint form error.");
}

[[nodiscard]] auto FormErrorText(rpl::producer<FormError> error)
-> rpl::producer<TextWithEntities> {
	return std::move(error) | rpl::map([](const FormError &error) {
		return FormErrorFor(error);
	}) | rpl::flatten_latest();
}

[[nodiscard]] auto StatusFor(EditorStatus status)
-> rpl::producer<QString> {
	switch (status) {
	case EditorStatus::Idle:
		return tr::ayu_custom_endpoints_status_idle();
	case EditorStatus::Connecting:
		return tr::ayu_custom_endpoints_status_connecting();
	case EditorStatus::Probing:
		return tr::ayu_custom_endpoints_status_probing();
	case EditorStatus::Confirmed:
		return tr::ayu_custom_endpoints_status_confirmed();
	case EditorStatus::Reverted:
		return tr::ayu_custom_endpoints_status_reverted();
	case EditorStatus::TimeoutReverted:
		return tr::ayu_custom_endpoints_status_timeout_reverted();
	case EditorStatus::RpcReverted:
		return tr::ayu_custom_endpoints_status_rpc_reverted();
	case EditorStatus::ParseReverted:
		return tr::ayu_custom_endpoints_status_parse_reverted();
	}
	Unexpected("Custom endpoint editor status.");
}

[[nodiscard]] auto StatusText(rpl::producer<EditorStatus> status)
-> rpl::producer<QString> {
	return std::move(status) | rpl::map([](EditorStatus status) {
		return StatusFor(status);
	}) | rpl::flatten_latest();
}

CustomEndpointEditor::CustomEndpointEditor(
		not_null<Ui::GenericBox*> box,
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Account*> account)
: _box(box.get())
, _show(std::move(show))
, _account(base::make_weak(account.get()))
, _timeout([this] { failActive(TestFailure::Timeout); })
, _confirmed(account->mtp().dcOptions().confirmedCustomEndpointProfile()) {
	setup(account);
}

CustomEndpointEditor::~CustomEndpointEditor() {
	failActive(TestFailure::Cancelled);
}

auto CustomEndpointEditor::publicKeyDescription()
-> rpl::producer<TextWithEntities> {
	const auto weak = base::make_weak(this);
	return _modeChanged.events_starting_with({}) | rpl::map([weak] -> rpl::producer<TextWithEntities> {
		const auto strong = weak.get();
		return strong
			? PublicKeyDescription(strong->_inputMode)
			: rpl::single(tr::marked());
	}) | rpl::flatten_latest();
}

auto CustomEndpointEditor::publicKeyFeedback()
-> rpl::producer<TextWithEntities> {
	const auto weak = base::make_weak(this);
	return rpl::merge(
		_modeChanged.events_starting_with({}),
		_publicKeyChanged.events()
	) | rpl::map([weak] -> rpl::producer<TextWithEntities> {
		const auto strong = weak.get();
		if (!strong || !strong->_publicKeyField) {
			return rpl::single(tr::marked());
		}
		return PublicKeyFeedbackText(
			strong->_publicKeyField->getLastText(),
			strong->_inputMode);
	}) | rpl::flatten_latest();
}

void CustomEndpointEditor::setup(not_null<Main::Account*> account) {
	const auto box = _box.data();
	Expects(box != nullptr);

	box->setNoContentMargin(true);
	box->setWidth(st::customEndpointBoxWidth);
	box->verticalLayout()->resizeToWidth(box->width());
	box->setTitle(tr::ayu_custom_endpoints_title());
	const auto weak = base::make_weak(this);

	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			SummaryText(_confirmed.value()),
			st::customEndpointSummary),
		st::customEndpointSummaryPadding);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			(account->mtp().isTestMode()
				? tr::ayu_custom_endpoints_environment_test()
				: tr::ayu_custom_endpoints_environment_production()),
			st::customEndpointEnvironment),
		st::customEndpointEnvironmentPadding);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_custom_endpoints_mode_title(),
			st::customEndpointDescription),
		st::customEndpointModeTitlePadding);

	const auto current = _confirmed.current();
	const auto singleServer = current
		? ExtractSingleServer(*current)
		: std::optional<SingleServer>();
	_inputMode = (current && !singleServer)
		? InputMode::Advanced
		: InputMode::SingleServer;
	_modeGroup = std::make_shared<Ui::RadiobuttonGroup>(int(_inputMode));
	_singleServerOption = box->addRow(
		object_ptr<Ui::Radiobutton>(
			box,
			_modeGroup,
			int(InputMode::SingleServer),
			tr::ayu_custom_endpoints_mode_teamgram(tr::now)),
		st::customEndpointOptionPadding);
	_advancedOption = box->addRow(
		object_ptr<Ui::Radiobutton>(
			box,
			_modeGroup,
			int(InputMode::Advanced),
			tr::ayu_custom_endpoints_mode_advanced(tr::now)),
		st::customEndpointOptionLastPadding);

	auto singleServerWrap = object_ptr<
		Ui::SlideWrap<Ui::VerticalLayout>>(
			box,
			object_ptr<Ui::VerticalLayout>(box));
	_singleServerWrap = singleServerWrap.data();
	const auto singleServerContent = singleServerWrap->entity();
	singleServerContent->add(
		object_ptr<Ui::FlatLabel>(
			singleServerContent,
			tr::ayu_custom_endpoints_teamgram_description(tr::rich),
			st::customEndpointDescription),
		st::customEndpointDescriptionPadding);
	_ipField = singleServerContent->add(
		object_ptr<Ui::InputField>(
			singleServerContent,
			st::customEndpointSingleInput,
			Ui::InputField::Mode::NoNewlines,
			tr::ayu_custom_endpoints_ip_placeholder(),
			singleServer ? singleServer->ip : QString()),
		st::customEndpointSingleInputPadding);
	_portField = singleServerContent->add(
		object_ptr<Ui::InputField>(
			singleServerContent,
			st::customEndpointSingleInput,
			Ui::InputField::Mode::NoNewlines,
			tr::ayu_custom_endpoints_port_placeholder(),
			QString::number(singleServer ? singleServer->port : 10443)),
		st::customEndpointSingleInputLastPadding);
	_portField->setMaxLength(5);
	box->addRow(
		std::move(singleServerWrap),
		st::customEndpointSectionPadding);

	auto advancedWrap = object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
		box,
		object_ptr<Ui::VerticalLayout>(box));
	_advancedWrap = advancedWrap.data();
	const auto advancedContent = advancedWrap->entity();
	advancedContent->add(
		object_ptr<Ui::FlatLabel>(
			advancedContent,
			tr::ayu_custom_endpoints_grammar(tr::rich),
			st::customEndpointDescription),
		st::customEndpointDescriptionPadding);
	_advancedField = advancedContent->add(
		object_ptr<Ui::InputField>(
			advancedContent,
			st::customEndpointInput,
			Ui::InputField::Mode::MultiLine,
			tr::ayu_custom_endpoints_input_placeholder(),
			TextWithTags{ current
				? MTP::FormatCustomEndpointProfile(*current)
				: QString() }),
		st::customEndpointInputPadding);
	_advancedField->setMaxLength(MTP::kCustomEndpointMaxTextSize);
	box->addRow(
		std::move(advancedWrap),
		st::customEndpointSectionPadding);

	_modeGroup->setChangedCallback([weak](int value) {
		if (const auto strong = weak.get()) {
			strong->setInputMode(
				InputMode(value),
				anim::type::normal);
			strong->inputChanged();
		}
	});
	setInputMode(_inputMode, anim::type::instant);

	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			publicKeyDescription(),
			st::customEndpointDescription),
		st::customEndpointPublicKeyDescriptionPadding);
	_publicKeyField = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::customEndpointPublicKeyInput,
			Ui::InputField::Mode::MultiLine,
			tr::ayu_custom_endpoints_public_key_placeholder(),
			TextWithTags{ current
				? QString::fromUtf8(current->publicKeyPem)
				: QString() }),
		st::customEndpointPublicKeyInputPadding);
	_publicKeyField->setMaxLength(MTP::kCustomEndpointMaxPublicKeySize);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			publicKeyFeedback(),
			st::customEndpointPublicKeyStatus),
		st::customEndpointPublicKeyStatusPadding);

	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_custom_endpoints_warning_privacy(tr::rich),
			st::customEndpointWarning),
		st::customEndpointWarningPadding);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_custom_endpoints_warning_not_https(tr::rich),
			st::customEndpointWarning),
		st::customEndpointWarningPadding);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_custom_endpoints_warning_compatibility(tr::rich),
			st::customEndpointWarning),
		st::customEndpointWarningPadding);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_custom_endpoints_warning_fallback(tr::rich),
			st::customEndpointWarning),
		st::customEndpointWarningLastPadding);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			FormErrorText(_formError.value()),
			st::customEndpointError),
		st::customEndpointErrorPadding);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			StatusText(_status.value()),
			st::customEndpointStatus),
		st::customEndpointStatusPadding);

	const auto apply = box->addButton(
		tr::ayu_custom_endpoints_apply_test(),
		[weak] {
			if (const auto strong = weak.get()) {
				strong->apply();
			}
		});
	_apply = apply;
	const auto reset = box->addLeftButton(
		tr::ayu_custom_endpoints_reset(),
		[weak] {
			if (const auto strong = weak.get()) {
				strong->reset();
			}
		});
	_reset = reset;
	box->addButton(tr::lng_cancel(), [weak] {
		if (const auto strong = weak.get()) {
			strong->cancelAndClose();
		}
	});

	_ipField->changes(
	) | rpl::on_next([weak] {
		if (const auto strong = weak.get()) {
			strong->inputChanged();
		}
	}, _ipField->lifetime());
	_portField->changes(
	) | rpl::on_next([weak] {
		if (const auto strong = weak.get()) {
			strong->inputChanged();
		}
	}, _portField->lifetime());
	_advancedField->changes(
	) | rpl::on_next([weak] {
		if (const auto strong = weak.get()) {
			strong->inputChanged();
		}
	}, _advancedField->lifetime());
	_publicKeyField->changes(
	) | rpl::on_next([weak] {
		if (const auto strong = weak.get()) {
			strong->_publicKeyChanged.fire({});
			strong->inputChanged();
		}
	}, _publicKeyField->lifetime());
	account->mtp().dcOptions().confirmedCustomEndpointProfileChanged(
	) | rpl::on_next([weak] {
		if (const auto strong = weak.get()) {
			strong->refreshConfirmedProfile();
		}
	}, box->lifetime());
	box->boxClosing(
	) | rpl::on_next([weak] {
		if (const auto strong = weak.get()) {
			strong->failActive(TestFailure::Cancelled);
		}
	}, box->lifetime());
	account->lifetime().add([weak, account] {
		if (const auto strong = weak.get()) {
			strong->accountTeardown(account);
		}
	});
	box->setFocusCallback([weak] {
		if (const auto strong = weak.get()) {
			if (strong->_inputMode == InputMode::SingleServer) {
				if (strong->_ipField) {
					strong->_ipField->setFocusFast();
				}
			} else if (strong->_advancedField) {
				strong->_advancedField->setFocusFast();
			}
		}
	});
}

void CustomEndpointEditor::apply() {
	if (_activeGeneration) {
		return;
	}
	const auto account = _account.get();
	if (!account) {
		_formError.force_assign(FormError{
			FormErrorType::Unavailable,
			{},
		});
		return;
	}

	const auto parsed = parseForm(account->mtp().environment());
	if (!MTP::IsValidCustomEndpoint(parsed.validation)) {
		_formError.force_assign(FormError{
			FormErrorType::Validation,
			parsed.validation,
		});
		if (IsPublicKeyError(parsed.validation.error)) {
			if (_publicKeyField) {
				_publicKeyField->showError();
			}
		} else if (_inputMode == InputMode::Advanced) {
			if (_advancedField) {
				_advancedField->showError();
			}
		} else if (parsed.validation.error
				== MTP::CustomEndpointError::InvalidPort) {
			if (_portField) {
				_portField->showError();
			}
		} else if (_ipField) {
			_ipField->showError();
		}
		return;
	}
	_formError.force_assign(FormError());
	if (parsed.validation.requiresLocalNetworkConfirmation) {
		confirmLocalNetwork(parsed.profile);
	} else {
		beginCandidate(parsed.profile);
	}
}

void CustomEndpointEditor::reset() {
	if (!_activeGeneration) {
		_formError.force_assign(FormError());
		beginCandidate(std::nullopt);
	}
}

void CustomEndpointEditor::confirmLocalNetwork(
		MTP::CustomEndpointProfile profile) {
	if (!_show || !_show->valid()) {
		return;
	}
	const auto weak = base::make_weak(this);
	_show->showBox(Ui::MakeConfirmBox({
		.text = tr::ayu_custom_endpoints_local_warning(tr::rich),
		.confirmed = [weak, profile = std::move(profile)](
				Fn<void()> close) mutable {
			close();
			if (const auto strong = weak.get()) {
				strong->beginCandidate(std::move(profile));
			}
		},
		.confirmText = tr::ayu_custom_endpoints_local_proceed(),
		.title = tr::ayu_custom_endpoints_local_warning_title(),
	}));
}

void CustomEndpointEditor::beginCandidate(
		std::optional<MTP::CustomEndpointProfile> profile) {
	if (_activeGeneration) {
		return;
	}
	const auto account = _account.get();
	if (!account) {
		_formError.force_assign(FormError{
			FormErrorType::Unavailable,
			{},
		});
		return;
	}
	if (account->mtp().dcOptions().customEndpointCandidateActive()) {
		_formError.force_assign(FormError{
			FormErrorType::TestInProgress,
			{},
		});
		return;
	}
	if (IsMtProxyActive()) {
		_formError.force_assign(FormError{
			FormErrorType::MtProxy,
			{},
		});
		return;
	}

	const auto generation = ++_nextGeneration;
	_activeGeneration = generation;
	_restartIssued = false;
	_testLifetime.destroy();
	_testLifetime = rpl::lifetime();
	const auto weak = base::make_weak(this);
	account->mtp().connectionStateChanges(
	) | rpl::on_next([weak, generation](MTP::ConnectionState state) {
		if (const auto strong = weak.get()) {
			strong->handleConnectionState(generation, state);
		}
	}, _testLifetime);
	_formError.force_assign(FormError());
	_status = EditorStatus::Connecting;
	setControlsEnabled(false);

	if (!account->mtp().dcOptions().applyCustomEndpointCandidate({ profile })) {
		_activeGeneration = 0;
		_testLifetime.destroy();
		setControlsEnabled(true);
		_status = EditorStatus::Idle;
		_formError.force_assign(FormError{
			FormErrorType::TestInProgress,
			{},
		});
		return;
	}

	_candidate = std::move(profile);
	_timeout.callOnce(kTestTimeout);
	_restartIssued = true;
	account->mtp().restart();
}

void CustomEndpointEditor::handleConnectionState(
		uint64 generation,
		MTP::ConnectionState state) {
	if (!isGeneration(generation)
		|| !_restartIssued
		|| _probeRequestId
		|| state.state != MTP::ConnectedState) {
		return;
	}
	const auto account = _account.get();
	if (!account || state.shiftedDcId != account->mtp().mainDcId()) {
		return;
	}
	sendProbe(generation, state.shiftedDcId);
}

void CustomEndpointEditor::sendProbe(
		uint64 generation,
		MTP::ShiftedDcId shiftedDcId) {
	if (!isGeneration(generation) || _probeRequestId) {
		return;
	}
	const auto account = _account.get();
	if (!account || shiftedDcId != account->mtp().mainDcId()) {
		failActive(TestFailure::Rpc);
		return;
	}
	if (IsMtProxyActive()) {
		failActive(TestFailure::MtProxy);
		return;
	}

	_status = EditorStatus::Probing;
	_probeDcId = shiftedDcId;
	const auto weak = base::make_weak(this);
	_probeRequestId = account->mtp().send(
		MTPhelp_GetConfig(),
		[weak, generation](const MTP::Response &response) {
			if (const auto strong = weak.get()) {
				return strong->probeDone(generation, response);
			}
			return true;
		},
		[weak, generation](
				const MTP::Error &,
				const MTP::Response &response) {
			if (const auto strong = weak.get()) {
				return strong->probeFailed(generation, response);
			}
			return true;
		},
		shiftedDcId);
}

bool CustomEndpointEditor::probeDone(
		uint64 generation,
		const MTP::Response &response) {
	if (!isGeneration(generation)
		|| !_probeRequestId
		|| response.requestId != _probeRequestId) {
		return true;
	}

	_probeRequestId = 0;
	const auto probeDcId = base::take(_probeDcId);
	auto from = response.reply.constData();
	auto result = MTPConfig();
	if (!result.read(from, from + response.reply.size())) {
		failActive(TestFailure::Parse);
		return false;
	}
	const auto account = _account.get();
	if (!account || account->mtp().mainDcId() != probeDcId) {
		failActive(TestFailure::Rpc);
		return true;
	}
	completeSuccess(generation);
	return true;
}

bool CustomEndpointEditor::probeFailed(
		uint64 generation,
		const MTP::Response &response) {
	if (!isGeneration(generation)
		|| !_probeRequestId
		|| response.requestId != _probeRequestId) {
		return true;
	}
	_probeRequestId = 0;
	_probeDcId = 0;
	failActive(TestFailure::Rpc);
	return true;
}

void CustomEndpointEditor::completeSuccess(uint64 generation) {
	if (!isGeneration(generation)) {
		return;
	}
	const auto account = _account.get();
	if (!account) {
		failActive(TestFailure::Rpc);
		return;
	}

	_timeout.cancel();
	_testLifetime.destroy();
	_restartIssued = false;
	if (IsMtProxyActive()) {
		failActive(TestFailure::MtProxy);
		return;
	}
	if (!account->mtp().dcOptions().promoteCustomEndpointCandidate()) {
		failActive(TestFailure::Rpc);
		return;
	}

	_activeGeneration = 0;
	_probeRequestId = 0;
	_probeDcId = 0;
	setControlsEnabled(true);
	refreshConfirmedProfile();
	setFormFromProfile(_candidate);
	_candidate.reset();
	_formError.force_assign(FormError());
	_status = EditorStatus::Confirmed;
	showSuccess();
}

void CustomEndpointEditor::failActive(
		TestFailure failure,
		Main::Account *knownAccount) {
	if (!_activeGeneration) {
		return;
	}

	_activeGeneration = 0;
	_restartIssued = false;
	_timeout.cancel();
	_testLifetime.destroy();
	const auto requestId = base::take(_probeRequestId);
	_probeDcId = 0;
	_candidate.reset();

	const auto account = knownAccount ? knownAccount : _account.get();
	if (account) {
		if (requestId) {
			account->mtp().cancel(requestId);
		}
		if (account->mtp().dcOptions().rollbackCustomEndpointCandidate()) {
			account->mtp().restart();
		}
	}

	setControlsEnabled(true);
	_formError.force_assign((failure == TestFailure::MtProxy)
		? FormError{ FormErrorType::MtProxy, {} }
		: FormError());
	switch (failure) {
	case TestFailure::Cancelled:
		_status = EditorStatus::Reverted;
		break;
	case TestFailure::Timeout:
		_status = EditorStatus::TimeoutReverted;
		break;
	case TestFailure::Rpc:
		_status = EditorStatus::RpcReverted;
		break;
	case TestFailure::Parse:
		_status = EditorStatus::ParseReverted;
		break;
	case TestFailure::MtProxy:
		_status = EditorStatus::Reverted;
		break;
	}
	showFailure(failure);
}

void CustomEndpointEditor::cancelAndClose() {
	failActive(TestFailure::Cancelled);
	if (_box) {
		_box->closeBox();
	}
}

void CustomEndpointEditor::accountTeardown(
		not_null<Main::Account*> account) {
	failActive(TestFailure::Cancelled, account.get());
	if (_box) {
		_box->closeBox();
	}
}

void CustomEndpointEditor::inputChanged() {
	if (_activeGeneration) {
		return;
	}
	_formError.force_assign(FormError());
	_status = EditorStatus::Idle;
}

void CustomEndpointEditor::setInputMode(
		InputMode mode,
		anim::type animated) {
	if (_inputMode != mode) {
		_inputMode = mode;
		_modeChanged.fire({});
	}
	if (_singleServerWrap) {
		_singleServerWrap->toggle(
			mode == InputMode::SingleServer,
			animated);
	}
	if (_advancedWrap) {
		_advancedWrap->toggle(mode == InputMode::Advanced, animated);
	}
}

void CustomEndpointEditor::setFormFromProfile(
		const std::optional<MTP::CustomEndpointProfile> &profile) {
	const auto singleServer = profile
		? ExtractSingleServer(*profile)
		: std::optional<SingleServer>();
	const auto mode = (profile && !singleServer)
		? InputMode::Advanced
		: InputMode::SingleServer;
	if (_ipField) {
		_ipField->setText(singleServer ? singleServer->ip : QString());
	}
	if (_portField) {
		_portField->setText(QString::number(
			singleServer ? singleServer->port : 10443));
	}
	if (_advancedField) {
		_advancedField->setText(profile
			? MTP::FormatCustomEndpointProfile(*profile)
			: QString());
	}
	if (_publicKeyField) {
		_publicKeyField->setText(profile
			? QString::fromUtf8(profile->publicKeyPem)
			: QString());
		_publicKeyChanged.fire({});
	}
	if (_modeGroup && _modeGroup->current() != int(mode)) {
		_modeGroup->setValue(int(mode));
	}
	setInputMode(mode, anim::type::instant);
}

MTP::CustomEndpointProfileResult CustomEndpointEditor::parseForm(
		MTP::Environment environment) const {
	auto result = MTP::CustomEndpointProfileResult();
	if (_inputMode == InputMode::SingleServer) {
		const auto ip = _ipField ? _ipField->getLastText() : QString();
		const auto port = _portField
			? _portField->getLastText().toInt()
			: 0;
		result = MTP::ParseCustomEndpointProfile(
			SingleServerProfileText(ip, port, environment),
			environment);
	} else {
		result = MTP::ParseCustomEndpointProfile(
			_advancedField ? _advancedField->getLastText() : QString(),
			environment);
	}
	if (!MTP::IsValidCustomEndpoint(result.validation)) {
		return result;
	}

	const auto publicKeyPem = (_publicKeyField
		? _publicKeyField->getLastText()
		: QString()).toUtf8();
	if (publicKeyPem.trimmed().isEmpty()) {
		if (_inputMode == InputMode::SingleServer) {
			result.validation.error
				= MTP::CustomEndpointError::MissingPublicKey;
			return result;
		}
		result.profile.publicKeyPem.clear();
	} else {
		const auto publicKey = MTP::ValidateCustomEndpointPublicKey(
			publicKeyPem);
		if (!MTP::IsValidCustomEndpoint(publicKey.validation)) {
			result.validation = publicKey.validation;
			return result;
		}
		result.profile.publicKeyPem = publicKey.publicKeyPem;
	}
	result.validation = MTP::ValidateCustomEndpointProfile(result.profile);
	return result;
}

void CustomEndpointEditor::refreshConfirmedProfile() {
	if (const auto account = _account.get()) {
		_confirmed.force_assign(
			account->mtp().dcOptions().confirmedCustomEndpointProfile());
	}
}

void CustomEndpointEditor::setControlsEnabled(bool enabled) {
	if (_singleServerOption) {
		_singleServerOption->setDisabled(!enabled);
	}
	if (_advancedOption) {
		_advancedOption->setDisabled(!enabled);
	}
	if (_ipField) {
		_ipField->setDisabled(!enabled);
	}
	if (_portField) {
		_portField->setDisabled(!enabled);
	}
	if (_advancedField) {
		_advancedField->setDisabled(!enabled);
	}
	if (_publicKeyField) {
		_publicKeyField->setDisabled(!enabled);
	}
	if (_apply) {
		_apply->setDisabled(!enabled);
	}
	if (_reset) {
		_reset->setDisabled(!enabled);
	}
}

void CustomEndpointEditor::showFailure(TestFailure failure) {
	if (!_show || !_show->valid()) {
		return;
	}
	switch (failure) {
	case TestFailure::Cancelled:
		_show->showToast(
			tr::ayu_custom_endpoints_status_reverted(tr::now));
		break;
	case TestFailure::Timeout:
		_show->showToast(
			tr::ayu_custom_endpoints_status_timeout_reverted(tr::now));
		break;
	case TestFailure::Rpc:
		_show->showToast(
			tr::ayu_custom_endpoints_status_rpc_reverted(tr::now));
		break;
	case TestFailure::Parse:
		_show->showToast(
			tr::ayu_custom_endpoints_status_parse_reverted(tr::now));
		break;
	case TestFailure::MtProxy:
		_show->showToast(
			tr::ayu_custom_endpoints_error_mtproxy(tr::now));
		break;
	}
}

void CustomEndpointEditor::showSuccess() {
	if (_show && _show->valid()) {
		_show->showToast(
			tr::ayu_custom_endpoints_status_confirmed(tr::now));
	}
}

bool CustomEndpointEditor::isGeneration(uint64 generation) const {
	return generation && generation == _activeGeneration;
}

} // namespace

void ShowCustomEndpointBox(
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Account*> account) {
	if (!show || !show->valid()) {
		return;
	}
	show->showBox(Box([show, account](not_null<Ui::GenericBox*> box) {
		box->lifetime().make_state<CustomEndpointEditor>(
			box,
			show,
			account);
	}));
}

} // namespace Ayu
