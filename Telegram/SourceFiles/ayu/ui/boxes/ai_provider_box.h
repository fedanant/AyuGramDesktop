#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class GenericBox;
} // namespace Ui

namespace Ayu {

void AiProviderBox(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session);

} // namespace Ayu
