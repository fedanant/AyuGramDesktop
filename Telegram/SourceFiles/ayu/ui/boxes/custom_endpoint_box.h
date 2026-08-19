/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <memory>

namespace Main {
class Account;
} // namespace Main

namespace Ui {
class Show;
} // namespace Ui

namespace Ayu {

void ShowCustomEndpointBox(
	std::shared_ptr<Ui::Show> show,
	not_null<Main::Account*> account);

} // namespace Ayu

