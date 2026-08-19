#include "ayu/tg_ws_proxy.h"

#ifdef Q_OS_WIN

#include "core/application.h"
#include "mtproto/mtproto_proxy_data.h"
#include "settings.h"

#include "rpl/variable.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcess>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QTimer>
#include <QtCore/qt_windows.h>
#include <QtNetwork/QHostAddress>
#include <QtNetwork/QTcpServer>
#include <QtNetwork/QTcpSocket>

#include <memory>

namespace Ayu {
namespace {

constexpr auto kProbeInterval = crl::time(100);
constexpr auto kStartupTimeout = crl::time(10 * 1000);

[[nodiscard]] QString ConfigPath() {
	return QDir(cWorkingDir() + u"tdata/ayu"_q).filePath(
		u"tg_ws_proxy.json"_q);
}

[[nodiscard]] TgWsProxyConfig LoadConfig() {
	auto result = TgWsProxyConfig();
	auto file = QFile(ConfigPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return result;
	}
	const auto document = QJsonDocument::fromJson(file.readAll());
	if (!document.isObject()) {
		return result;
	}
	const auto object = document.object();
	result.enabled = object.value(u"enabled"_q).toBool(result.enabled);
	result.cloudflareEnabled = object.value(u"cloudflare_enabled"_q).toBool(
		result.cloudflareEnabled);
	result.customDomainEnabled = object.value(
		u"custom_domain_enabled"_q).toBool(result.customDomainEnabled);
	result.workerEnabled = object.value(u"worker_enabled"_q).toBool(
		result.workerEnabled);
	if (const auto value = object.value(u"dc_redirects"_q); value.isString()) {
		result.dcRedirects = value.toString();
	}
	if (const auto value = object.value(u"custom_domains"_q); value.isString()) {
		result.customDomains = value.toString();
	}
	if (const auto value = object.value(u"worker_domains"_q); value.isString()) {
		result.workerDomains = value.toString();
	}
	return result;
}

void SaveConfig(const TgWsProxyConfig &config) {
	const auto path = ConfigPath();
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("TG WS Proxy Error: could not save config: %1").arg(path));
		return;
	}
	const auto object = QJsonObject{
		{ u"version"_q, 1 },
		{ u"enabled"_q, config.enabled },
		{ u"dc_redirects"_q, config.dcRedirects },
		{ u"cloudflare_enabled"_q, config.cloudflareEnabled },
		{ u"custom_domain_enabled"_q, config.customDomainEnabled },
		{ u"custom_domains"_q, config.customDomains },
		{ u"worker_enabled"_q, config.workerEnabled },
		{ u"worker_domains"_q, config.workerDomains },
	};
	file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
	if (!file.commit()) {
		LOG(("TG WS Proxy Error: could not commit config: %1").arg(path));
	}
}

[[nodiscard]] QStringList SplitValues(QString value) {
	value.replace(',', ' ');
	value.replace(';', ' ');
	return value.split(
		QRegularExpression(u"\\s+"_q),
		Qt::SkipEmptyParts);
}

[[nodiscard]] QString GenerateSecret() {
	const auto generator = QRandomGenerator::system();
	const auto first = QString::number(generator->generate64(), 16)
		.rightJustified(16, '0');
	const auto second = QString::number(generator->generate64(), 16)
		.rightJustified(16, '0');
	return first + second;
}

class TgWsProxyManager final : public QObject {
public:
	TgWsProxyManager();
	~TgWsProxyManager();

	[[nodiscard]] TgWsProxyConfig config() const;
	void setConfig(TgWsProxyConfig config);
	[[nodiscard]] rpl::producer<bool> enabledValue() const;
	[[nodiscard]] rpl::producer<TgWsProxyState> stateValue() const;

private:
	void start();
	void probe();
	void activate();
	void handleProcessFinished();
	void stop(bool restoreProxy);
	void setState(TgWsProxyState state);

	QProcess _process;
	QTcpSocket _probe;
	QTimer _probeTimer;
	QElapsedTimer _startedAt;
	rpl::variable<TgWsProxyConfig> _config;
	rpl::variable<TgWsProxyState> _state = TgWsProxyState::Disabled;
	QString _secret;
	uint16 _port = 0;
	bool _active = false;
	bool _stopping = false;

};

std::unique_ptr<TgWsProxyManager> Manager;

TgWsProxyManager::TgWsProxyManager()
: _process(this)
, _probe(this)
, _probeTimer(this)
, _config(LoadConfig()) {
	_probeTimer.setSingleShot(true);
	QObject::connect(
		&_process,
		&QProcess::started,
		this,
		&TgWsProxyManager::probe);
	QObject::connect(
		&_process,
		qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
		this,
		&TgWsProxyManager::handleProcessFinished);
	QObject::connect(
		&_process,
		&QProcess::errorOccurred,
		this,
		[=] { handleProcessFinished(); });
	QObject::connect(
		&_probe,
		&QTcpSocket::connected,
		this,
		&TgWsProxyManager::activate);
	QObject::connect(
		&_probe,
		&QTcpSocket::errorOccurred,
		this,
		[=] {
			if (!_stopping && !_active) {
				_probeTimer.start(kProbeInterval);
			}
		});
	QObject::connect(
		&_probeTimer,
		&QTimer::timeout,
		this,
		&TgWsProxyManager::probe);
	if (_config.current().enabled) {
		start();
	}
}

TgWsProxyManager::~TgWsProxyManager() {
	stop(false);
}

TgWsProxyConfig TgWsProxyManager::config() const {
	return _config.current();
}

void TgWsProxyManager::setConfig(TgWsProxyConfig config) {
	const auto previous = _config.current();
	if (previous == config) {
		if (config.enabled
			&& _process.state() == QProcess::NotRunning
			&& _state.current() != TgWsProxyState::Running) {
			start();
		}
		return;
	}
	if (!config.enabled || previous.enabled) {
		stop(true);
	}
	_config = config;
	SaveConfig(config);
	if (!config.enabled) {
		setState(TgWsProxyState::Disabled);
	} else {
		start();
	}
}

rpl::producer<bool> TgWsProxyManager::enabledValue() const {
	return _config.value(
	) | rpl::map([](const TgWsProxyConfig &config) {
		return config.enabled;
	}) | rpl::distinct_until_changed();
}

rpl::producer<TgWsProxyState> TgWsProxyManager::stateValue() const {
	return _state.value();
}

void TgWsProxyManager::start() {
	if (!_config.current().enabled
		|| _process.state() != QProcess::NotRunning) {
		return;
	}
	_stopping = false;
	_active = false;
	const auto directory = QCoreApplication::applicationDirPath();
	const auto program = QDir(directory).filePath(u"AyuWsProxy.exe"_q);
	if (!QFileInfo::exists(program)) {
		LOG(("TG WS Proxy Error: helper is missing: %1").arg(program));
		setState(TgWsProxyState::Error);
		return;
	}

	auto portReservation = QTcpServer();
	if (!portReservation.listen(QHostAddress::LocalHost, 0)) {
		LOG(("TG WS Proxy Error: could not reserve a local port"));
		setState(TgWsProxyState::Error);
		return;
	}
	_port = portReservation.serverPort();
	portReservation.close();
	_secret = GenerateSecret();

	const auto logDirectory = cWorkingDir() + u"tdata/ayu"_q;
	QDir().mkpath(logDirectory);
	const auto logPath = QDir(logDirectory).filePath(u"tg_ws_proxy.log"_q);
	auto arguments = QStringList{
		u"--host"_q,
		u"127.0.0.1"_q,
		u"--port"_q,
		QString::number(_port),
		u"--secret"_q,
		_secret,
		u"--parent-pid"_q,
		QString::number(QCoreApplication::applicationPid()),
		u"--log-file"_q,
		logPath,
	};
	const auto config = _config.current();
	for (const auto &value : SplitValues(config.dcRedirects)) {
		arguments.push_back(u"--dc-ip"_q);
		arguments.push_back(value);
	}
	if (!config.cloudflareEnabled) {
		arguments.push_back(u"--no-cfproxy"_q);
	}
	if (config.customDomainEnabled) {
		for (const auto &value : SplitValues(config.customDomains)) {
			arguments.push_back(u"--cfproxy-domain"_q);
			arguments.push_back(value);
		}
	}
	if (config.workerEnabled) {
		for (const auto &value : SplitValues(config.workerDomains)) {
			arguments.push_back(u"--cfproxy-worker-domain"_q);
			arguments.push_back(value);
		}
	}
	_process.setProgram(program);
	_process.setArguments(arguments);
	_process.setWorkingDirectory(directory);
	_process.setCreateProcessArgumentsModifier([](
			QProcess::CreateProcessArguments *arguments) {
		arguments->flags |= CREATE_NO_WINDOW;
	});
	_startedAt.start();
	setState(TgWsProxyState::Starting);
	_process.start();
}

void TgWsProxyManager::probe() {
	if (_stopping || _active) {
		return;
	} else if (_process.state() == QProcess::NotRunning) {
		handleProcessFinished();
		return;
	} else if (_startedAt.elapsed() >= kStartupTimeout) {
		LOG(("TG WS Proxy Error: startup timed out"));
		stop(false);
		setState(TgWsProxyState::Error);
		return;
	}
	_probe.abort();
	_probe.connectToHost(QHostAddress::LocalHost, _port);
}

void TgWsProxyManager::activate() {
	if (_stopping
		|| _active
		|| _process.state() == QProcess::NotRunning) {
		return;
	}
	_probeTimer.stop();
	_probe.abort();
	_active = true;
	Core::App().setRuntimeProxy(MTP::ProxyData{
		MTP::ProxyData::Type::Mtproto,
		u"127.0.0.1"_q,
		_port,
		QString(),
		u"dd"_q + _secret,
	});
	setState(TgWsProxyState::Running);
	LOG(("TG WS Proxy Info: connected through 127.0.0.1:%1").arg(_port));
}

void TgWsProxyManager::handleProcessFinished() {
	if (_stopping) {
		return;
	}
	_probeTimer.stop();
	_probe.abort();
	if (_active) {
		_active = false;
		Core::App().setRuntimeProxy(std::nullopt);
	}
	if (_config.current().enabled) {
		setState(TgWsProxyState::Error);
	} else {
		setState(TgWsProxyState::Disabled);
	}
	LOG(("TG WS Proxy Error: helper stopped"));
}

void TgWsProxyManager::stop(bool restoreProxy) {
	if (_stopping) {
		return;
	}
	_stopping = true;
	_probeTimer.stop();
	_probe.abort();
	if (restoreProxy && _active && Core::IsAppLaunched()) {
		Core::App().setRuntimeProxy(std::nullopt);
	}
	_active = false;
	if (_process.state() != QProcess::NotRunning) {
		_process.terminate();
		if (!_process.waitForFinished(1000)) {
			_process.kill();
			_process.waitForFinished(1000);
		}
	}
	_stopping = false;
}

void TgWsProxyManager::setState(TgWsProxyState state) {
	if (_state.current() != state) {
		_state = state;
	}
}

}

void StartTgWsProxy() {
	if (!Manager) {
		Manager = std::make_unique<TgWsProxyManager>();
	}
}

void StopTgWsProxy() {
	Manager = nullptr;
}

TgWsProxyConfig GetTgWsProxyConfig() {
	return Manager ? Manager->config() : LoadConfig();
}

void SetTgWsProxyConfig(TgWsProxyConfig config) {
	if (!Manager) {
		Manager = std::make_unique<TgWsProxyManager>();
	}
	Manager->setConfig(std::move(config));
}

void SetTgWsProxyEnabled(bool enabled) {
	auto config = GetTgWsProxyConfig();
	config.enabled = enabled;
	SetTgWsProxyConfig(std::move(config));
}

bool IsTgWsProxyEnabled() {
	return GetTgWsProxyConfig().enabled;
}

rpl::producer<bool> TgWsProxyEnabledValue() {
	return Manager
		? Manager->enabledValue()
		: rpl::single(LoadConfig().enabled);
}

rpl::producer<TgWsProxyState> TgWsProxyStateValue() {
	return Manager
		? Manager->stateValue()
		: rpl::single(TgWsProxyState::Disabled);
}

}

#else

namespace Ayu {

void StartTgWsProxy() {
}

void StopTgWsProxy() {
}

TgWsProxyConfig GetTgWsProxyConfig() {
	return TgWsProxyConfig();
}

void SetTgWsProxyConfig(TgWsProxyConfig config) {
}

void SetTgWsProxyEnabled(bool enabled) {
}

bool IsTgWsProxyEnabled() {
	return false;
}

rpl::producer<bool> TgWsProxyEnabledValue() {
	return rpl::single(false);
}

rpl::producer<TgWsProxyState> TgWsProxyStateValue() {
	return rpl::single(TgWsProxyState::Disabled);
}

}

#endif
