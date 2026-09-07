#include "ayu/features/ai/ai_client.h"

#include "lang/lang_keys.h"

namespace Ayu::Ai {

QString ErrorText(Error error) {
	switch (error) {
	case Error::InvalidUrl: return tr::ayu_AiProviderInvalidUrl(tr::now);
	case Error::MissingModel: return tr::ayu_AiProviderMissingModel(tr::now);
	case Error::InvalidKey: return tr::ayu_AiProviderInvalidKey(tr::now);
	case Error::EmptyText: return tr::lng_ai_compose_apply_empty(tr::now);
	case Error::ToneUnavailable: return tr::ayu_AiProviderToneUnavailable(tr::now);
	case Error::Timeout: return tr::ayu_AiProviderTimeout(tr::now);
	case Error::Unauthorized: return tr::ayu_AiProviderUnauthorized(tr::now);
	case Error::RateLimit: return tr::ayu_AiProviderRateLimit(tr::now);
	case Error::Http: return tr::ayu_AiProviderHttpError(tr::now);
	case Error::InvalidResponse: return tr::ayu_AiProviderInvalidResponse(tr::now);
	case Error::ResponseTooLarge: return tr::ayu_AiProviderResponseTooLarge(tr::now);
	case Error::IncompleteResponse: return tr::ayu_AiProviderIncompleteResponse(tr::now);
	case Error::Network: return tr::ayu_AiProviderNetworkError(tr::now);
	case Error::None: return QString();
	}
	Unexpected("Error in Ayu::Ai::ErrorText.");
}

} // namespace Ayu::Ai
