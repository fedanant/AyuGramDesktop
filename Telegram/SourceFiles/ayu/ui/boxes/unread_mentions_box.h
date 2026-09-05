/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

namespace Window {
class SessionController;
} // namespace Window

namespace Ayu {

class UnreadMentionsModel;

void ShowUnreadMentionsBox(
	not_null<Window::SessionController*> controller,
	not_null<UnreadMentionsModel*> model);

} // namespace Ayu
