#include "ayu/features/ai/ai_rich_page.h"

#include "ayu/features/ai/ai_document.h"
#include "iv/iv_rich_page.h"

namespace Ayu::Ai {
namespace {

bool Editable(EntityType type) {
	switch (type) {
	case EntityType::CustomUrl:
	case EntityType::Bold:
	case EntityType::Semibold:
	case EntityType::Italic:
	case EntityType::Underline:
	case EntityType::StrikeOut:
	case EntityType::Blockquote:
	case EntityType::Spoiler:
	case EntityType::Subscript:
	case EntityType::Superscript:
	case EntityType::Marked:
	case EntityType::Colorized:
		return true;
	default:
		return false;
	}
}

template <typename Blocks, typename Callback>
void Visit(Blocks &blocks, const Callback &callback, bool summary = false) {
	for (auto &block : blocks) {
		const auto text = [&](auto &value) { callback(value, true); };
		const auto fixed = [&](auto &value) { callback(value, false); };
		if (summary || block.kind != Iv::RichPage::BlockKind::Code) {
			text(block.text.text);
		} else {
			fixed(block.text.text);
		}
		text(block.caption.text);
		for (auto &item : block.listItems) {
			text(item.text.text);
			Visit(item.blocks, callback, summary);
		}
		for (auto &row : block.tableRows) {
			for (auto &cell : row.cells) {
				text(cell.text.text);
			}
		}
		for (auto &button : block.buttons) {
			text(button.text.text);
		}
		Visit(block.blocks, callback, summary);
	}
}

std::vector<DocumentText> Extract(const Iv::RichPage &page) {
	auto result = std::vector<DocumentText>();
	Visit(page.blocks, [&](const TextWithEntities &text, bool editable) {
		if (text.empty()) {
			return;
		}
		auto entry = DocumentText{ .text = text.text };
		for (const auto &entity : text.entities) {
			entry.ranges.push_back({
				.offset = entity.offset(),
				.length = entity.length(),
				.editable = editable && Editable(entity.type()),
			});
		}
		if (!editable) {
			entry.ranges.push_back({ 0, int(text.text.size()), false });
		}
		result.push_back(std::move(entry));
	});
	return result;
}

} // namespace

QString EncodeRichPage(const Iv::RichPage &page) {
	return EncodeDocument(Extract(page));
}

std::shared_ptr<Iv::RichPage> DecodeRichPage(
		const Iv::RichPage &source,
		const QString &response) {
	const auto texts = DecodeDocument(Extract(source), response);
	if (!texts) {
		return nullptr;
	}
	auto page = std::make_shared<Iv::RichPage>(source);
	auto index = 0;
	Visit(page->blocks, [&](TextWithEntities &text, bool) {
		if (text.empty()) {
			return;
		}
		const auto &updated = (*texts)[index++];
		for (auto i = 0; i < text.entities.size(); ++i) {
			auto &entity = text.entities[i];
			const auto &range = updated.ranges[i];
			entity.shiftRight(range.offset - entity.offset());
			entity.shrinkFromRight(entity.length() - range.length);
		}
		text.text = updated.text;
	});
	page->rtl = Iv::DetermineRichPageRtl(*page);
	return page;
}

QString RichPageText(const Iv::RichPage &page) {
	auto result = QString();
	Visit(page.blocks, [&](const TextWithEntities &text, bool) {
		if (!text.empty()) {
			if (!result.isEmpty()) {
				result += '\n';
			}
			result += text.text;
		}
	}, true);
	return result;
}

} // namespace Ayu::Ai
