# OpenAI-compatible AI editor

Open **Settings → AyuGram → Chats → AI Editor → AI provider**.

Enable **Use OpenAI-compatible API** and enter:

- **API base URL**, including the provider's API prefix, for example `https://api.openai.com/v1`. A full URL ending in `/chat/completions` also works. A local server can use `http://localhost:PORT/v1`.
- **Model**, using the exact model ID supplied by your provider.
- **API key**. Local servers that do not require authentication can leave this empty.

Save the settings, write a message of at least three lines and open the AI editor. Proofreading, translation, styles, emojis and custom prompts use the configured provider. Review the result before applying or sending it. Changed text is returned as plain text; an unchanged response preserves the original formatting.

Disabling the option restores Telegram as the provider. The configuration is local to the current account and device. It is saved with the account's encrypted session settings, outside `ayu_settings.json`. The key field is masked, and the HTTP client does not log request bodies, keys or provider response bodies.

Only the text being edited and the requested transformation are sent to the configured provider. Chat history is not included. Rich text edits and short summaries also go through the configured provider, including the editor's style/prompt flow. Voice transcription still uses Telegram.

The provider must support non-streaming `POST /chat/completions` with `model`, `messages`, `stream: false` and `store: false`, and return `choices[0].message.content` as a string. Responses API endpoints are not supported by this adapter. Storage policies are controlled by the chosen provider; the request's `store: false` flag alone is not a guarantee about its retention policy.

Requests have a 60-second timeout and a 2 MiB response limit. HTTP redirects are rejected to avoid forwarding the key or message to another address. Failures leave the original text intact and never trigger a request to another AI provider.

## Verification

The HTTP client has standalone C++ tests using Qt Core and Network and a local TCP server. No real API credentials are required:

```powershell
cmake -S Telegram/SourceFiles/test/ai_provider -B out/ai-provider-tests -DCMAKE_PREFIX_PATH=<Qt directory>
cmake --build out/ai-provider-tests --config Debug
ctest --test-dir out/ai-provider-tests -C Debug --output-on-failure
```

On Windows, add the Qt `bin` directory to `PATH` before running the tests.

Manual checks after building AyuGram:

1. Save a provider and restart the app. Check that its model and URL persist and the key stays masked.
2. Switch to another Telegram account. Its AI provider should initially remain Telegram.
3. Test proofreading, translation, a default style and a custom prompt in an ordinary message and a media caption.
4. Start a request, then change the selected action or close the editor. An obsolete response must not replace the current result.
5. Try an invalid key or model. Check the error message and that the original draft remains intact.
6. Disable the provider and verify the standard Telegram editor still works.
