#pragma once

#include "base/basic_types.h"

namespace Ayu::Ai {

struct Settings {
	bool enabled = false;
	QString baseUrl = u"https://api.openai.com/v1"_q;
	QString model;
	QString apiKey;

	friend bool operator==(const Settings&, const Settings&) = default;
};

} // namespace Ayu::Ai
