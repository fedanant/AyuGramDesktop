#include "ayu/ui/boxes/tg_ws_proxy_box.h"

#include "ayu/tg_ws_proxy.h"
#include "lang_auto.h"
#include "lang/lang_text_entity.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"

#include "styles/style_ayu_settings.h"
#include "styles/style_layers.h"

#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtNetwork/QHostAddress>

namespace Ayu {
namespace {

struct FormState {
	QPointer<Ui::InputField> dcRedirects;
	QPointer<Ui::Checkbox> cloudflareEnabled;
	QPointer<Ui::Checkbox> customDomainEnabled;
	QPointer<Ui::InputField> customDomains;
	QPointer<Ui::Checkbox> workerEnabled;
	QPointer<Ui::InputField> workerDomains;
};

[[nodiscard]] QStringList SplitValues(QString value) {
	value.replace(',', ' ');
	value.replace(';', ' ');
	return value.split(
		QRegularExpression(u"\\s+"_q),
		Qt::SkipEmptyParts);
}

[[nodiscard]] bool IsValidDomain(const QString &domain) {
	if (domain.isEmpty()
		|| domain.size() > 253
		|| domain.startsWith('.')
		|| domain.endsWith('.')) {
		return false;
	}
	const auto labels = domain.split('.');
	if (labels.size() < 2) {
		return false;
	}
	static const auto labelExpression = QRegularExpression(
		u"^[A-Za-z0-9-]+$"_q);
	for (const auto &label : labels) {
		if (label.isEmpty()
			|| label.size() > 63
			|| label.startsWith('-')
			|| label.endsWith('-')
			|| !labelExpression.match(label).hasMatch()) {
			return false;
		}
	}
	return true;
}

[[nodiscard]] bool AreValidDomains(const QString &text) {
	const auto values = SplitValues(text);
	return !values.empty() && ranges::all_of(values, &IsValidDomain);
}

[[nodiscard]] bool AreValidDcRedirects(const QString &text) {
	const auto values = SplitValues(text);
	if (values.empty()) {
		return false;
	}
	auto dcIds = QSet<int>();
	for (const auto &value : values) {
		const auto separator = value.indexOf(':');
		if (separator <= 0 || separator != value.lastIndexOf(':')) {
			return false;
		}
		bool dcOk = false;
		const auto dcId = value.left(separator).toInt(&dcOk);
		const auto supportedDc = (dcId >= 1 && dcId <= 5) || dcId == 203;
		auto address = QHostAddress();
		if (!dcOk
			|| !supportedDc
			|| dcIds.contains(dcId)
			|| !address.setAddress(value.mid(separator + 1))
			|| address.protocol() != QAbstractSocket::IPv4Protocol) {
			return false;
		}
		dcIds.insert(dcId);
	}
	return true;
}

[[nodiscard]] QString StateText(TgWsProxyState state) {
	switch (state) {
	case TgWsProxyState::Disabled:
		return tr::ayu_tg_ws_proxy_status_disabled(tr::now);
	case TgWsProxyState::Starting:
		return tr::ayu_tg_ws_proxy_status_starting(tr::now);
	case TgWsProxyState::Running:
		return tr::ayu_tg_ws_proxy_status_running(tr::now);
	case TgWsProxyState::Error:
		return tr::ayu_tg_ws_proxy_status_error(tr::now);
	}
	Unexpected("TgWsProxyState value.");
}

void FillTgWsProxySettingsBox(not_null<Ui::GenericBox*> box) {
	box->setWidth(st::tgWsProxyBoxWidth);
	box->verticalLayout()->resizeToWidth(box->width());
	box->setTitle(tr::ayu_tg_ws_proxy_title());

	const auto config = GetTgWsProxyConfig();
	const auto state = std::make_shared<FormState>();
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_tg_ws_proxy_description(tr::rich),
			st::tgWsProxyDescription),
		st::tgWsProxyDescriptionPadding);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			TgWsProxyStateValue() | rpl::map(&StateText),
			st::tgWsProxyStatus),
		st::tgWsProxyStatusPadding);

	Ui::AddSubsectionTitle(
		box->verticalLayout(),
		tr::ayu_tg_ws_proxy_dc_title());
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_tg_ws_proxy_dc_description(),
			st::tgWsProxyDescription),
		st::tgWsProxySectionDescriptionPadding);
	state->dcRedirects = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::tgWsProxyDcInput,
			Ui::InputField::Mode::MultiLine,
			tr::ayu_tg_ws_proxy_dc_placeholder(),
			config.dcRedirects),
		st::tgWsProxyFieldPadding);
	state->dcRedirects->setMaxLength(2048);

	Ui::AddSubsectionTitle(
		box->verticalLayout(),
		tr::ayu_tg_ws_proxy_cloudflare_title());
	state->cloudflareEnabled = box->addRow(
		object_ptr<Ui::Checkbox>(
			box,
			tr::ayu_tg_ws_proxy_cloudflare_enable(tr::now),
			config.cloudflareEnabled),
		st::tgWsProxyCheckboxPadding);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			tr::ayu_tg_ws_proxy_cloudflare_description(tr::rich),
			st::tgWsProxyDescription),
		st::tgWsProxySectionDescriptionPadding);
	state->customDomainEnabled = box->addRow(
		object_ptr<Ui::Checkbox>(
			box,
			tr::ayu_tg_ws_proxy_custom_domain_enable(tr::now),
			config.customDomainEnabled),
		st::tgWsProxyCheckboxPadding);
	state->customDomains = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::tgWsProxyField,
			Ui::InputField::Mode::NoNewlines,
			tr::ayu_tg_ws_proxy_custom_domain_placeholder(),
			config.customDomains),
		st::tgWsProxyFieldPadding);
	state->customDomains->setMaxLength(1024);

	Ui::AddSubsectionTitle(
		box->verticalLayout(),
		tr::ayu_tg_ws_proxy_worker_title());
	state->workerEnabled = box->addRow(
		object_ptr<Ui::Checkbox>(
			box,
			tr::ayu_tg_ws_proxy_worker_enable(tr::now),
			config.workerEnabled),
		st::tgWsProxyCheckboxPadding);
	state->workerDomains = box->addRow(
		object_ptr<Ui::InputField>(
			box,
			st::tgWsProxyField,
			Ui::InputField::Mode::NoNewlines,
			tr::ayu_tg_ws_proxy_worker_placeholder(),
			config.workerDomains),
		st::tgWsProxyFieldLastPadding);
	state->workerDomains->setMaxLength(1024);

	const auto refreshEnabled = [=] {
		const auto cloudflare = state->cloudflareEnabled->checked();
		state->customDomainEnabled->setDisabled(!cloudflare);
		state->customDomains->setDisabled(
			!cloudflare || !state->customDomainEnabled->checked());
		state->workerDomains->setDisabled(!state->workerEnabled->checked());
	};
	state->cloudflareEnabled->checkedChanges(
	) | rpl::on_next([=](bool) {
		refreshEnabled();
	}, state->cloudflareEnabled->lifetime());
	state->customDomainEnabled->checkedChanges(
	) | rpl::on_next([=](bool) {
		refreshEnabled();
	}, state->customDomainEnabled->lifetime());
	state->workerEnabled->checkedChanges(
	) | rpl::on_next([=](bool) {
		refreshEnabled();
	}, state->workerEnabled->lifetime());
	refreshEnabled();

	box->addButton(tr::ayu_tg_ws_proxy_save(), [=] {
		if (!AreValidDcRedirects(state->dcRedirects->getLastText())) {
			state->dcRedirects->showError();
			box->uiShow()->showToast(
				tr::ayu_tg_ws_proxy_error_dc(tr::now));
			return;
		}
		if (state->cloudflareEnabled->checked()
			&& state->customDomainEnabled->checked()
			&& !AreValidDomains(state->customDomains->getLastText())) {
			state->customDomains->showError();
			box->uiShow()->showToast(
				tr::ayu_tg_ws_proxy_error_domain(tr::now));
			return;
		}
		if (state->workerEnabled->checked()
			&& !AreValidDomains(state->workerDomains->getLastText())) {
			state->workerDomains->showError();
			box->uiShow()->showToast(
				tr::ayu_tg_ws_proxy_error_worker(tr::now));
			return;
		}
		auto updated = GetTgWsProxyConfig();
		updated.dcRedirects = SplitValues(
			state->dcRedirects->getLastText()).join(u'\n');
		updated.cloudflareEnabled = state->cloudflareEnabled->checked();
		updated.customDomainEnabled
			= state->customDomainEnabled->checked();
		updated.customDomains = SplitValues(
			state->customDomains->getLastText()).join(u", "_q);
		updated.workerEnabled = state->workerEnabled->checked();
		updated.workerDomains = SplitValues(
			state->workerDomains->getLastText()).join(u", "_q);
		SetTgWsProxyConfig(std::move(updated));
		box->closeBox();
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

}

object_ptr<Ui::BoxContent> TgWsProxySettingsBox() {
	return Box<Ui::GenericBox>(FillTgWsProxySettingsBox);
}

}
