#pragma once

#include "base/basic_types.h"

#include <optional>
#include <vector>

namespace Ayu::Ai {

struct TextRange {
	int offset = 0;
	int length = 0;
	bool editable = true;
};

struct DocumentText {
	QString text;
	std::vector<TextRange> ranges;
};

[[nodiscard]] QString EncodeDocument(const std::vector<DocumentText> &texts);
[[nodiscard]] std::optional<std::vector<DocumentText>> DecodeDocument(
	const std::vector<DocumentText> &original,
	const QString &response);

} // namespace Ayu::Ai
