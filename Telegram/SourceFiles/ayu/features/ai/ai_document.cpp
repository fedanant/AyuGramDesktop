#include "ayu/features/ai/ai_document.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <algorithm>

namespace Ayu::Ai {
namespace {

struct Parts {
	std::vector<int> boundaries;
	std::vector<bool> editable;
};

std::optional<Parts> Split(const DocumentText &text) {
	auto result = Parts{ .boundaries = { 0, int(text.text.size()) } };
	for (const auto &range : text.ranges) {
		if (range.offset < 0 || range.length <= 0
			|| range.offset > text.text.size()
			|| range.length > text.text.size() - range.offset) {
			return std::nullopt;
		}
		result.boundaries.push_back(range.offset);
		result.boundaries.push_back(range.offset + range.length);
	}
	auto &boundaries = result.boundaries;
	std::sort(boundaries.begin(), boundaries.end());
	boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
	for (const auto offset : boundaries) {
		if (offset > 0 && offset < text.text.size()
			&& text.text[offset - 1].isHighSurrogate()
			&& text.text[offset].isLowSurrogate()) {
			return std::nullopt;
		}
	}
	for (auto i = 1; i < int(boundaries.size()); ++i) {
		const auto from = boundaries[i - 1];
		const auto till = boundaries[i];
		auto editable = !text.text.mid(from, till - from).trimmed().isEmpty();
		for (const auto &range : text.ranges) {
			if (!range.editable && range.offset < till
				&& range.offset + range.length > from) {
				editable = false;
			}
		}
		result.editable.push_back(editable);
	}
	return result;
}

} // namespace

QString EncodeDocument(const std::vector<DocumentText> &texts) {
	auto array = QJsonArray();
	for (const auto &text : texts) {
		const auto split = Split(text);
		if (!split) {
			return {};
		}
		auto parts = QJsonArray();
		for (auto i = 1; i < int(split->boundaries.size()); ++i) {
			parts.push_back(QJsonObject{
				{ u"text"_q, text.text.mid(split->boundaries[i - 1],
					split->boundaries[i] - split->boundaries[i - 1]) },
				{ u"editable"_q, bool(split->editable[i - 1]) },
			});
		}
		array.push_back(QJsonObject{ { u"parts"_q, parts } });
	}
	return texts.empty() ? QString() : QString::fromUtf8(QJsonDocument(
		QJsonObject{ { u"texts"_q, array } }).toJson(QJsonDocument::Compact));
}

std::optional<std::vector<DocumentText>> DecodeDocument(
		const std::vector<DocumentText> &original,
		const QString &response) {
	const auto document = QJsonDocument::fromJson(response.toUtf8());
	const auto value = document.object().value(u"texts"_q);
	const auto texts = value.toArray();
	if (!value.isArray() || texts.size() != original.size()) {
		return std::nullopt;
	}
	auto result = original;
	for (auto index = 0; index < int(original.size()); ++index) {
		const auto split = Split(original[index]);
		const auto parts = texts[index].toArray();
		if (!split || !texts[index].isArray()
			|| parts.size() != split->editable.size()) {
			return std::nullopt;
		}
		auto &text = result[index];
		text.text.clear();
		auto offsets = std::vector<int>{ 0 };
		for (auto i = 0; i < parts.size(); ++i) {
			const auto before = original[index].text.mid(split->boundaries[i],
				split->boundaries[i + 1] - split->boundaries[i]);
			const auto after = parts[i].toString();
			if (!parts[i].isString()
				|| (!split->editable[i] && after != before)
				|| (!before.trimmed().isEmpty() && after.trimmed().isEmpty())) {
				return std::nullopt;
			}
			text.text += after;
			offsets.push_back(int(text.text.size()));
		}
		const auto mapped = [&](int offset) {
			const auto i = std::lower_bound(
				split->boundaries.begin(), split->boundaries.end(), offset);
			return offsets[i - split->boundaries.begin()];
		};
		for (auto &range : text.ranges) {
			const auto from = mapped(range.offset);
			const auto till = mapped(range.offset + range.length);
			range.offset = from;
			range.length = till - from;
		}
	}
	return result;
}

} // namespace Ayu::Ai
