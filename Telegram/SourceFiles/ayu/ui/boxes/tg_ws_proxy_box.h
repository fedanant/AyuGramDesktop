#pragma once

#include "base/object_ptr.h"

namespace Ui {
class BoxContent;
}

namespace Ayu {

[[nodiscard]] object_ptr<Ui::BoxContent> TgWsProxySettingsBox();

}
