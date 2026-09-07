#pragma once

#include "ayu/features/ai/ai_settings.h"

#include <QtCore/QPointer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace Ayu::Ai {

enum class Error {
	None,
	InvalidUrl,
	MissingModel,
	InvalidKey,
	EmptyText,
	ToneUnavailable,
	Network,
	Timeout,
	Unauthorized,
	RateLimit,
	Http,
	InvalidResponse,
	ResponseTooLarge,
	IncompleteResponse,
};

enum class Operation {
	Edit,
	EditDocument,
	Generate,
	Summarize,
};

struct Request {
	QString text;
	QString language;
	std::optional<QString> tone;
	Operation operation = Operation::Edit;
	bool proofread = false;
	bool emojify = false;
};

[[nodiscard]] QUrl Endpoint(const Settings &settings);
[[nodiscard]] Error Validate(const Settings &settings);
[[nodiscard]] QString ErrorText(Error error);

class Client final {
public:
	using Done = std::function<void(QString)>;
	using Fail = std::function<void(Error)>;

	~Client();
	void request(int id, Settings settings, Request request, Done done, Fail fail);
	bool cancel(int id);

private:
	struct Pending {
		QPointer<QNetworkReply> reply;
		QByteArray body;
		bool timedOut = false;
		bool tooLarge = false;
	};

	QNetworkAccessManager _network;
	std::map<int, std::shared_ptr<Pending>> _pending;

};

} // namespace Ayu::Ai
