#pragma once

#include "base/basic_types.h"

#include <memory>

namespace Iv {
struct RichPage;
} // namespace Iv

namespace Ayu::Ai {

[[nodiscard]] QString EncodeRichPage(const Iv::RichPage &page);
[[nodiscard]] std::shared_ptr<Iv::RichPage> DecodeRichPage(
	const Iv::RichPage &source,
	const QString &response);
[[nodiscard]] QString RichPageText(const Iv::RichPage &page);

} // namespace Ayu::Ai
