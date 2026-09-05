#pragma once

#include "rpl/producer.h"

#include <QtCore/QString>

namespace Ayu {

enum class TgWsProxyState {
	Disabled,
	Starting,
	Running,
	Error,
};

struct TgWsProxyConfig {
	bool enabled = false;
	QString dcRedirects = QStringLiteral(
		"2:149.154.167.220\n4:149.154.167.220");
	bool cloudflareEnabled = true;
	bool customDomainEnabled = false;
	QString customDomains;
	bool workerEnabled = false;
	QString workerDomains;

	friend bool operator==(
		const TgWsProxyConfig &,
		const TgWsProxyConfig &) = default;
};

void StartTgWsProxy();
void StopTgWsProxy();

[[nodiscard]] TgWsProxyConfig GetTgWsProxyConfig();
void SetTgWsProxyConfig(TgWsProxyConfig config);
void SetTgWsProxyEnabled(bool enabled);
[[nodiscard]] bool IsTgWsProxyEnabled();
[[nodiscard]] rpl::producer<bool> TgWsProxyEnabledValue();
[[nodiscard]] rpl::producer<TgWsProxyState> TgWsProxyStateValue();

}
