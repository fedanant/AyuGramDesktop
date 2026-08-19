/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ayu/ui/boxes/unread_mentions_box.h"

#include "ayu/mentions/unread_mentions_model.h"
#include "base/unixtime.h"
#include "base/weak_ptr.h"
#include "data/data_forum_topic.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_item_preview.h"
#include "lang_auto.h"
#include "lang/lang_text_entity.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/text/format_values.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/painter.h"
#include "window/window_session_controller.h"

#include "styles/style_dialogs.h"

#include <QtCore/QPointer>
#include <QtCore/QStringList>
#include <QtGui/QAccessible>
#include <QtGui/QKeyEvent>
#include <QtGui/QPaintEvent>
#include <QtGui/QResizeEvent>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace Ayu {
namespace {

struct MentionRowData {
	QString peer;
	QString topic;
	QString sender;
	QString preview;
	QString date;
	bool valid = false;
};

class MentionRow final : public Ui::RippleButton {
public:
	MentionRow(
		QWidget *parent,
		not_null<Main::Session*> session,
		Fn<void(FullMsgId)> activate);

	[[nodiscard]] bool bind(FullMsgId id, const QString &emptyPreview);
	void unbind();
	[[nodiscard]] bool bound() const;

protected:
	void keyPressEvent(QKeyEvent *e) override;
	void paintEvent(QPaintEvent *e) override;
	QAccessible::Role accessibilityRole() override;

private:
	void activate();

	const not_null<Main::Session*> _session;
	const Fn<void(FullMsgId)> _activate;
	FullMsgId _id;
	MentionRowData _data;

};

class MentionsList final : public Ui::RpWidget {
public:
	MentionsList(
		QWidget *parent,
		not_null<Main::Session*> session,
		not_null<UnreadMentionsModel*> model,
		Fn<void(FullMsgId)> activate);

	[[nodiscard]] int replaceIds(
		std::vector<FullMsgId> ids,
		int scrollTop);
	void setEmptyPreview(QString text);
	void setVisibleRange(int top, int bottom);
	void focusFirst();

protected:
	void resizeEvent(QResizeEvent *e) override;
	QAccessible::Role accessibilityRole() override;

private:
	void ensurePool(int size);
	void refreshHeight();
	void rebind();
	void requestRefresh();

	const not_null<Main::Session*> _session;
	const base::weak_ptr<UnreadMentionsModel> _model;
	const Fn<void(FullMsgId)> _activate;
	std::vector<FullMsgId> _ids;
	std::vector<MentionRow*> _pool;
	QString _emptyPreview;
	int _visibleTop = 0;
	int _visibleBottom = 0;
	bool _refreshQueued = false;

};

class UnreadMentionsBox final : public base::has_weak_ptr {
public:
	UnreadMentionsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<UnreadMentionsModel*> model);

private:
	void setup();
	void handleState(const UnreadMentionsState &state);
	void activate(FullMsgId id);
	void retry();
	void confirmMarkAll();
	void handleMarkResult(const MarkAllMentionsResult &result);

	const QPointer<Ui::GenericBox> _box;
	const not_null<Window::SessionController*> _controller;
	const base::weak_ptr<UnreadMentionsModel> _model;
	rpl::variable<int> _markFailedCount = 0;
	QPointer<Ui::ScrollArea> _scroll;
	QPointer<MentionsList> _list;
	QPointer<Ui::RoundButton> _retry;
	QPointer<Ui::RoundButton> _markAll;

};

[[nodiscard]] auto StatusFor(const UnreadMentionsState &state)
-> rpl::producer<QString> {
	if (state.markingAll) {
		return tr::ayu_unread_mentions_status_marking_all();
	}
	if (state.discoveryFailed) {
		return tr::ayu_unread_mentions_status_discovery_failed();
	}
	const auto failed = int(state.failedPeers.size());
	const auto stalled = int(state.stalledPeers.size());
	if (!state.complete) {
		if (failed > 0 && stalled > 0) {
			return tr::ayu_unread_mentions_status_failed_stalled_incomplete(
				lt_count,
				rpl::single(float64(failed)),
				lt_stalled_count,
				rpl::single(QString::number(stalled)));
		} else if (failed > 0) {
			return tr::ayu_unread_mentions_status_failed_incomplete(
				lt_count,
				rpl::single(float64(failed)));
		} else if (stalled > 0) {
			return tr::ayu_unread_mentions_status_stalled_incomplete(
				lt_count,
				rpl::single(float64(stalled)));
		}
		return state.loading
			? tr::ayu_unread_mentions_status_loading()
			: tr::ayu_unread_mentions_status_incomplete();
	} else if (state.count <= 0) {
		return tr::ayu_unread_mentions_status_empty();
	}
	const auto loaded = int(state.loadedIds.size());
	const auto total = state.count;
	if (failed > 0 && stalled > 0) {
		return tr::ayu_unread_mentions_status_failed_stalled(
			lt_count,
			rpl::single(float64(failed)),
			lt_loaded_count,
			rpl::single(QString::number(loaded)),
			lt_total_count,
			rpl::single(QString::number(total)),
			lt_stalled_count,
			rpl::single(QString::number(stalled)));
	} else if (failed > 0) {
		return tr::ayu_unread_mentions_status_failed(
			lt_count,
			rpl::single(float64(failed)),
			lt_loaded_count,
			rpl::single(QString::number(loaded)),
			lt_total_count,
			rpl::single(QString::number(total)));
	} else if (stalled > 0) {
		return tr::ayu_unread_mentions_status_stalled(
			lt_count,
			rpl::single(float64(stalled)),
			lt_loaded_count,
			rpl::single(QString::number(loaded)),
			lt_total_count,
			rpl::single(QString::number(total)));
	} else if (state.loading) {
		return tr::ayu_unread_mentions_status_loading_exact(
			lt_count,
			rpl::single(float64(total)),
			lt_loaded_count,
			rpl::single(QString::number(loaded)));
	}
	return tr::ayu_unread_mentions_status_exact(
		lt_count,
		rpl::single(float64(total)));
}

[[nodiscard]] auto StatusText(
		rpl::producer<UnreadMentionsState> state)
-> rpl::producer<QString> {
	return std::move(state) | rpl::map([](
			const UnreadMentionsState &state) {
		return StatusFor(state);
	}) | rpl::flatten_latest();
}

[[nodiscard]] auto MarkFailureFor(int count)
-> rpl::producer<QString> {
	return (count > 0)
		? tr::ayu_unread_mentions_mark_all_partial(
			lt_count,
			rpl::single(float64(count)))
		: rpl::single(QString());
}

[[nodiscard]] auto MarkFailureText(rpl::producer<int> count)
-> rpl::producer<QString> {
	return std::move(count) | rpl::map([](int count) {
		return MarkFailureFor(count);
	}) | rpl::flatten_latest();
}

[[nodiscard]] auto MarkAllButtonFor(int failedCount)
-> rpl::producer<QString> {
	return (failedCount > 0)
		? tr::ayu_unread_mentions_retry_mark_all()
		: tr::lng_context_mark_read_mentions_all();
}

[[nodiscard]] auto MarkAllButtonText(rpl::producer<int> failedCount)
-> rpl::producer<QString> {
	return std::move(failedCount) | rpl::map([](int failedCount) {
		return MarkAllButtonFor(failedCount);
	}) | rpl::flatten_latest();
}

MentionRow::MentionRow(
	QWidget *parent,
	not_null<Main::Session*> session,
	Fn<void(FullMsgId)> activate)
: RippleButton(parent, st::unreadMentionsRowRipple)
, _session(session)
, _activate(std::move(activate)) {
	setFocusPolicy(Qt::StrongFocus);
	setClickedCallback([this] { this->activate(); });
}

bool MentionRow::bind(FullMsgId id, const QString &emptyPreview) {
	const auto item = _session->data().message(id);
	if (!item || !item->isUnreadMention()) {
		unbind();
		return false;
	}

	auto data = MentionRowData();
	data.peer = item->history()->peer->name();
	if (const auto topic = item->topic()) {
		data.topic = topic->title();
	}
	data.sender = item->from()->shortName();
	const auto preview = item->toPreview({
		.hideSender = true,
		.generateImages = false,
		.ignoreTopic = true,
	});
	data.preview = preview.text.text.simplified();
	if (data.preview.isEmpty()) {
		data.preview = emptyPreview;
	}
	data.date = Ui::FormatDialogsDate(base::unixtime::parse(item->date()));
	data.valid = true;

	_id = id;
	_data = std::move(data);
	auto accessible = QStringList{ _data.peer };
	if (!_data.topic.isEmpty()) {
		accessible.push_back(_data.topic);
	}
	accessible.push_back(_data.sender);
	accessible.push_back(_data.preview);
	accessible.push_back(_data.date);
	setAccessibleName(accessible.join(u", "_q));
	update();
	return true;
}

void MentionRow::unbind() {
	_id = FullMsgId();
	_data = MentionRowData();
	setAccessibleName(QString());
	update();
}

bool MentionRow::bound() const {
	return _data.valid;
}

void MentionRow::keyPressEvent(QKeyEvent *e) {
	if (!e->isAutoRepeat()
		&& (e->key() == Qt::Key_Return
			|| e->key() == Qt::Key_Enter)) {
		activate();
		e->accept();
		return;
	}
	RippleButton::keyPressEvent(e);
}

void MentionRow::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);
	const auto focused = hasFocus();
	const auto over = isOver();
	p.fillRect(
		e->rect(),
		focused
			? st::unreadMentionsRowBgFocused
			: over
			? st::unreadMentionsRowBgOver
			: st::unreadMentionsRowBg);
	paintRipple(p, 0, 0);
	if (focused) {
		auto pen = QPen(st::unreadMentionsRowFocusFg->c);
		pen.setWidth(st::unreadMentionsRowFocusWidth);
		p.setPen(pen);
		p.setBrush(Qt::NoBrush);
		const auto margin = st::unreadMentionsRowFocusMargin;
		const auto radius = st::unreadMentionsRowFocusRadius;
		p.drawRoundedRect(
			rect().marginsRemoved(QMargins(
				margin,
				margin,
				margin,
				margin)),
			radius,
			radius);
	}
	if (!_data.valid) {
		return;
	}

	const auto &padding = st::unreadMentionsRowPadding;
	const auto available = width() - padding.left() - padding.right();
	p.setFont(st::unreadMentionsRowTitleFont);
	p.setPen(st::unreadMentionsRowTitleFg);
	const auto dateWidth = st::unreadMentionsRowDateFont->width(_data.date);
	const auto titleWidth = std::max(
		available - dateWidth - st::unreadMentionsRowDateSkip,
		0);
	p.drawTextLeft(
		padding.left(),
		st::unreadMentionsRowTitleTop,
		width(),
		st::unreadMentionsRowTitleFont->elided(_data.peer, titleWidth));
	p.setFont(st::unreadMentionsRowDateFont);
	p.setPen(st::unreadMentionsRowDateFg);
	p.drawTextRight(
		padding.right(),
		st::unreadMentionsRowTitleTop,
		width(),
		_data.date);

	const auto meta = _data.topic.isEmpty()
		? _data.sender
		: _data.topic + u" · "_q + _data.sender;
	p.setFont(st::unreadMentionsRowMetaFont);
	p.setPen(st::unreadMentionsRowMetaFg);
	p.drawTextLeft(
		padding.left(),
		st::unreadMentionsRowMetaTop,
		width(),
		st::unreadMentionsRowMetaFont->elided(meta, available));
	p.setFont(st::unreadMentionsRowPreviewFont);
	p.setPen(st::unreadMentionsRowPreviewFg);
	p.drawTextLeft(
		padding.left(),
		st::unreadMentionsRowPreviewTop,
		width(),
		st::unreadMentionsRowPreviewFont->elided(
			_data.preview,
			available));
}

QAccessible::Role MentionRow::accessibilityRole() {
	return QAccessible::Button;
}

void MentionRow::activate() {
	if (_data.valid) {
		_activate(_id);
	}
}

MentionsList::MentionsList(
	QWidget *parent,
	not_null<Main::Session*> session,
	not_null<UnreadMentionsModel*> model,
	Fn<void(FullMsgId)> activate)
: RpWidget(parent)
, _session(session)
, _model(base::make_weak(model.get()))
, _activate(std::move(activate)) {
}

int MentionsList::replaceIds(
		std::vector<FullMsgId> ids,
		int scrollTop) {
	auto anchor = std::optional<FullMsgId>();
	auto oldIndex = 0;
	auto offset = 0;
	const auto viewport = std::max(
		_visibleBottom - _visibleTop,
		st::unreadMentionsListHeight);
	if (!_ids.empty()) {
		oldIndex = std::clamp(
			scrollTop / st::unreadMentionsRowHeight,
			0,
			int(_ids.size()) - 1);
		offset = scrollTop % st::unreadMentionsRowHeight;
		anchor = _ids[oldIndex];
	}
	_ids = std::move(ids);
	refreshHeight();
	if (_ids.empty()) {
		rebind();
		return 0;
	}

	auto index = std::min(oldIndex, int(_ids.size()) - 1);
	if (anchor) {
		const auto i = std::find(_ids.begin(), _ids.end(), *anchor);
		if (i != _ids.end()) {
			index = int(i - _ids.begin());
		}
	}
	const auto result = index * st::unreadMentionsRowHeight + offset;
	_visibleTop = result;
	_visibleBottom = result + viewport;
	rebind();
	return result;
}

void MentionsList::setEmptyPreview(QString text) {
	if (_emptyPreview == text) {
		return;
	}
	_emptyPreview = std::move(text);
	rebind();
}

void MentionsList::setVisibleRange(int top, int bottom) {
	_visibleTop = top;
	_visibleBottom = std::max(bottom, top);
	refreshHeight();
	rebind();
}

void MentionsList::focusFirst() {
	for (const auto row : _pool) {
		if (row->bound() && !row->isHidden()) {
			row->setFocus();
			return;
		}
	}
}

void MentionsList::resizeEvent(QResizeEvent *e) {
	RpWidget::resizeEvent(e);
	rebind();
}

QAccessible::Role MentionsList::accessibilityRole() {
	return QAccessible::List;
}

void MentionsList::ensurePool(int size) {
	while (int(_pool.size()) < size) {
		const auto row = Ui::CreateChild<MentionRow>(
			this,
			_session,
			[this](FullMsgId id) { _activate(id); });
		_pool.push_back(row);
	}
}

void MentionsList::refreshHeight() {
	const auto viewport = std::max(_visibleBottom - _visibleTop, 0);
	const auto desired = std::max(
		int(_ids.size()) * st::unreadMentionsRowHeight,
		viewport);
	if (height() != desired) {
		resize(width(), desired);
	}
}

void MentionsList::rebind() {
	const auto count = int(_ids.size());
	const auto buffer = st::unreadMentionsRowBuffer;
	const auto first = std::clamp(
		(_visibleTop - buffer) / st::unreadMentionsRowHeight,
		0,
		count);
	const auto till = std::clamp(
		(_visibleBottom + buffer) / st::unreadMentionsRowHeight + 1,
		first,
		count);
	ensurePool(till - first);
	for (auto i = 0; i != int(_pool.size()); ++i) {
		const auto row = _pool[i];
		const auto index = first + i;
		if (index >= till) {
			row->hide();
			row->unbind();
			continue;
		}
		row->setGeometry(
			0,
			index * st::unreadMentionsRowHeight,
			width(),
			st::unreadMentionsRowHeight);
		if (row->bind(_ids[index], _emptyPreview)) {
			row->show();
		} else {
			row->hide();
			requestRefresh();
		}
	}
}

void MentionsList::requestRefresh() {
	if (_refreshQueued) {
		return;
	}
	_refreshQueued = true;
	InvokeQueued(this, [this] {
		_refreshQueued = false;
		if (const auto model = _model.get()) {
			model->refresh();
		}
	});
}

UnreadMentionsBox::UnreadMentionsBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::SessionController*> controller,
		not_null<UnreadMentionsModel*> model)
: _box(box.get())
, _controller(controller)
, _model(base::make_weak(model.get())) {
	setup();
}

void UnreadMentionsBox::setup() {
	const auto box = _box.data();
	const auto model = _model.get();
	Expects(box != nullptr && model != nullptr);

	box->setNoContentMargin(true);
	box->setWidth(st::unreadMentionsBoxWidth);
	box->verticalLayout()->resizeToWidth(box->width());
	box->setTitle(tr::ayu_unread_mentions_title());
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			StatusText(model->stateValue()),
			st::unreadMentionsStatus),
		st::unreadMentionsStatusPadding);
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box,
			MarkFailureText(_markFailedCount.value()),
			st::unreadMentionsFailure),
		st::unreadMentionsFailurePadding);

	const auto container = box->verticalLayout()->add(
		object_ptr<Ui::RpWidget>(box->verticalLayout()),
		st::unreadMentionsListPadding);
	container->resize(
		box->width()
			- st::unreadMentionsListPadding.left()
			- st::unreadMentionsListPadding.right(),
		st::unreadMentionsListHeight);
	_scroll = Ui::CreateChild<Ui::ScrollArea>(container, st::boxScroll);
	const auto weak = base::make_weak(this);
	_list = _scroll->setOwnedWidget(object_ptr<MentionsList>(
		_scroll,
		&_controller->session(),
		model,
		[weak](FullMsgId id) {
			if (const auto strong = weak.get()) {
				strong->activate(id);
			}
		}));
	container->sizeValue(
	) | rpl::on_next([=](QSize size) {
		_scroll->setGeometry(QRect(QPoint(), size));
	}, container->lifetime());
	_scroll->widthValue(
	) | rpl::on_next([=](int width) {
		_list->resize(width, _list->height());
	}, _list->lifetime());
	rpl::combine(
		_scroll->scrollTopValue(),
		_scroll->heightValue()
	) | rpl::on_next([=](int top, int height) {
		_list->setVisibleRange(top, top + height);
	}, _list->lifetime());
	tr::ayu_unread_mentions_list_accessibility(
	) | rpl::on_next([=](const QString &text) {
		_list->setAccessibleName(text);
	}, _list->lifetime());
	tr::ayu_unread_mentions_preview_empty(
	) | rpl::on_next([=](QString text) {
		_list->setEmptyPreview(std::move(text));
	}, _list->lifetime());

	const auto retry = box->addLeftButton(
		tr::ayu_unread_mentions_retry(),
		[weak] {
			if (const auto strong = weak.get()) {
				strong->retry();
			}
		});
	_retry = retry;
	const auto markAll = box->addButton(
		MarkAllButtonText(_markFailedCount.value()),
		[weak] {
			if (const auto strong = weak.get()) {
				strong->confirmMarkAll();
			}
		});
	_markAll = markAll;
	box->addButton(tr::lng_close(), [=] { box->closeBox(); });

	model->stateValue(
	) | rpl::on_next([weak](const UnreadMentionsState &state) {
		if (const auto strong = weak.get()) {
			strong->handleState(state);
		}
	}, box->lifetime());
	model->markAllResults(
	) | rpl::on_next([weak](const MarkAllMentionsResult &result) {
		if (const auto strong = weak.get()) {
			strong->handleMarkResult(result);
		}
	}, box->lifetime());
	box->setFocusCallback([weak] {
		if (const auto strong = weak.get()) {
			if (strong->_list) {
				strong->_list->focusFirst();
			}
		}
	});
	model->loadAll();
}

void UnreadMentionsBox::handleState(
		const UnreadMentionsState &state) {
	if (!_scroll || !_list) {
		return;
	}
	const auto anchor = _list->replaceIds(
		state.loadedIds,
		_scroll->scrollTop());
	_scroll->scrollToY(anchor);
	_list->setVisibleRange(
		_scroll->scrollTop(),
		_scroll->scrollTop() + _scroll->height());

	const auto retryVisible = !state.failedPeers.empty()
		|| !state.stalledPeers.empty()
		|| state.discoveryFailed;
	if (_retry) {
		_retry->setVisible(retryVisible);
		_retry->setDisabled(
			state.loading
			|| state.loadingPeers > 0
			|| state.markingAll);
	}
	if (_markAll) {
		_markAll->setVisible(state.complete && state.count > 0);
		_markAll->setDisabled(state.loading || state.markingAll);
	}
}

void UnreadMentionsBox::activate(FullMsgId id) {
	const auto controller = _controller;
	const auto item = controller->session().data().message(id);
	if (!item || !item->isUnreadMention()) {
		if (const auto model = _model.get()) {
			model->refresh();
		}
		return;
	}
	if (_box) {
		_box->closeBox();
	}
	controller->showMessage(item);
}

void UnreadMentionsBox::retry() {
	if (const auto model = _model.get()) {
		model->retryFailed();
	}
}

void UnreadMentionsBox::confirmMarkAll() {
	const auto model = _model.get();
	if (!model) {
		return;
	}
	const auto state = model->stateCurrent();
	if (!state.complete
		|| state.loading
		|| state.markingAll
		|| state.count <= 0) {
		return;
	}
	const auto weak = base::make_weak(this);
	_controller->show(
		Ui::MakeConfirmBox({
			.text = tr::ayu_unread_mentions_mark_all_confirmation(tr::rich),
			.confirmed = [weak] {
				if (const auto strong = weak.get()) {
					strong->_markFailedCount = 0;
					if (const auto model = strong->_model.get()) {
						model->markAll();
					}
				}
			},
			.confirmText = tr::lng_context_mark_read_mentions_all(),
			.title = tr::ayu_unread_mentions_mark_all_title(),
		}),
		Ui::LayerOption::KeepOther);
}

void UnreadMentionsBox::handleMarkResult(
		const MarkAllMentionsResult &result) {
	if (!result.failedPeers.empty()) {
		_markFailedCount = int(result.failedPeers.size());
		return;
	}
	const auto controller = _controller;
	const auto text = tr::ayu_unread_mentions_mark_all_success(tr::now);
	if (_box) {
		_box->closeBox();
	}
	controller->showToast(text);
}

} // namespace

void ShowUnreadMentionsBox(
		not_null<Window::SessionController*> controller,
		not_null<UnreadMentionsModel*> model) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->lifetime().make_state<UnreadMentionsBox>(
			box,
			controller,
			model);
	}));
}

} // namespace Ayu
