#include "ayu/ui/boxes/ai_provider_box.h"

#include "ayu/features/ai/ai_client.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "main/main_session_settings.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/masked_input_field.h"
#include "ui/widgets/labels.h"
#include "styles/style_layers.h"
#include "styles/style_widgets.h"

namespace Ayu {

void AiProviderBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	box->setTitle(tr::ayu_AiProviderTitle());
	box->setWidth(st::boxWideWidth);
	const auto &settings = session->settings().aiSettings();
	const auto enabled = box->addRow(object_ptr<Ui::Checkbox>(
		box,
		tr::ayu_AiProviderEnabled(tr::now),
		settings.enabled));
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::ayu_AiProviderAbout(),
		st::defaultFlatLabel));
	const auto addField = [&](
			rpl::producer<QString> label,
			rpl::producer<QString> placeholder,
			const QString &value) {
		box->addSkip(st::defaultVerticalListSkip);
		box->addRow(object_ptr<Ui::FlatLabel>(
			box,
			std::move(label),
			st::defaultFlatLabel));
		const auto wrap = box->addRow(object_ptr<Ui::RpWidget>(box));
		const auto field = Ui::CreateChild<Ui::MaskedInputField>(
			wrap,
			st::defaultInputField,
			std::move(placeholder),
			value);
		wrap->widthValue() | rpl::on_next([=](int width) {
			field->resizeToWidth(width);
		}, field->lifetime());
		field->heightValue() | rpl::on_next([=](int height) {
			wrap->resize(wrap->width(), height);
		}, field->lifetime());
		return field;
	};
	const auto url = addField(
		tr::ayu_AiProviderBaseUrl(),
		rpl::single(u"https://api.openai.com/v1"_q),
		settings.baseUrl);
	url->setMaxLength(2048);
	const auto model = addField(
		tr::ayu_AiProviderModel(),
		tr::ayu_AiProviderModelPlaceholder(),
		settings.model);
	model->setMaxLength(256);
	const auto key = addField(
		tr::ayu_AiProviderKey(),
		tr::ayu_AiProviderKeyPlaceholder(),
		settings.apiKey);
	key->setEchoMode(QLineEdit::Password);
	key->setMaxLength(4096);
	key->setInputMethodHints(Qt::ImhHiddenText | Qt::ImhNoPredictiveText);
	box->addSkip(st::defaultVerticalListSkip);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::ayu_AiProviderScope(),
		st::defaultFlatLabel));

	const auto save = [=] {
		auto settings = Ai::Settings{
			.enabled = enabled->checked(),
			.baseUrl = url->getLastText().trimmed(),
			.model = model->getLastText().trimmed(),
			.apiKey = key->getLastText().trimmed(),
		};
		if (settings.enabled) {
			const auto error = Ai::Validate(settings);
			if (error != Ai::Error::None) {
				const auto field = (error == Ai::Error::InvalidUrl)
					? url
					: (error == Ai::Error::MissingModel) ? model : key;
				field->showError();
				field->setFocus();
				box->showToast(Ai::ErrorText(error));
				return;
			}
		}
		session->settings().setAiSettings(std::move(settings));
		session->saveSettings();
		box->closeBox();
	};
	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	box->setFocusCallback([=] { url->setFocusFast(); });

	for (const auto field : { url, model, key }) {
		QObject::connect(field, &Ui::MaskedInputField::submitted, box, save);
	}
}

} // namespace Ayu
