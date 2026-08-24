import os, sys

NEW_SWIFT_CONTENT = '''// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
import Foundation
import NaturalLanguage
#if canImport(Translation)
import Translation
#endif

typealias TranslateProviderMacSwiftCallback = @convention(c) (
\tUnsafeMutableRawPointer?,
\tUnsafePointer<CChar>?,
\tUnsafePointer<CChar>?
) -> Void

private struct CallbackContext: @unchecked Sendable {
\tlet value: UnsafeMutableRawPointer?
}

private struct CallbackFunction: @unchecked Sendable {
\tlet value: TranslateProviderMacSwiftCallback
}

private func duplicatedCString(_ value: String) -> UnsafePointer<CChar>? {
\tguard let duplicated = strdup(value) else {
\t\treturn nil
\t}
\treturn UnsafePointer(duplicated)
}

#if compiler(>=6.2) && canImport(Translation)
@available(macOS 15.0, *)
private func requestTranslation(
\t\t_ text: String,
\t\t_ targetLanguage: String) async throws -> String {
\tguard let sourceLanguage
\t\t= NLLanguageRecognizer.dominantLanguage(for: text) else {
\t\t\tthrow TranslationError.unableToIdentifyLanguage
\t\t}
\tif sourceLanguage.rawValue == targetLanguage {
\t\treturn text
\t}
\tlet source = Locale.Language(identifier: sourceLanguage.rawValue)
\tlet target = Locale.Language(identifier: targetLanguage)
\tlet availability = LanguageAvailability()
\tlet status = await availability.status(from: source, to: target)
\tswitch status {
\tcase .installed:
\t\tbreak
\tcase .supported, .unsupported:
\t\tthrow TranslationError.unsupportedLanguagePairing
\t@unknown default:
\t\tthrow TranslationError.unsupportedLanguagePairing
\t}
\tif #available(macOS 26.0, *) {
\t\tlet session = TranslationSession(installedSource: source, target: target)
\t\tlet response = try await session.translate(text)
\t\treturn response.targetText
\t}
\tthrow TranslationError.unsupportedLanguagePairing
}

@available(macOS 15.0, *)
private func translateErrorCode(_ error: Error) -> String {
\tguard let translationError = error as? TranslationError else {
\t\treturn "unknown"
\t}
\tif #available(macOS 26.0, *), case .notInstalled = translationError {
\t\treturn "local-language-pack-missing"
\t}
\tswitch translationError {
\tcase .unsupportedLanguagePairing:
\t\treturn "local-language-pack-missing"
\tdefault:
\t\treturn "unknown"
\t}
}
#endif

@_cdecl("TranslateProviderMacSwiftIsAvailable")
func TranslateProviderMacSwiftIsAvailable() -> Bool {
#if compiler(>=6.2) && canImport(Translation)
\tif #available(macOS 26.0, *) {
\t\treturn true
\t}
#endif
\treturn false
}

@_cdecl("TranslateProviderMacSwiftTranslate")
func TranslateProviderMacSwiftTranslate(
\t_ sourceTextUtf8: UnsafePointer<CChar>?,
\t_ targetLanguageCodeUtf8: UnsafePointer<CChar>?,
\t_ context: UnsafeMutableRawPointer?,
\t_ callback: TranslateProviderMacSwiftCallback?
) {
\tguard let callback else {
\t\treturn
\t}
\tguard let sourceTextUtf8, let targetLanguageCodeUtf8 else {
\t\tcallback(context, nil, duplicatedCString("invalid-arguments"))
\t\treturn
\t}
\tlet sourceText = String(cString: sourceTextUtf8)
\tlet targetLanguageCode = String(cString: targetLanguageCodeUtf8)
\tlet callbackFunction = CallbackFunction(value: callback)
\tlet callbackContext = CallbackContext(value: context)
\tif #available(macOS 10.15, *) {
\t\tTask.detached(priority: .utility) {
\t\t\tlet callback = callbackFunction.value
\t\t\tlet context = callbackContext.value
#if compiler(>=6.2) && canImport(Translation)
\t\t\tif #available(macOS 26.0, *) {
\t\t\t\tdo {
\t\t\t\t\tlet translated = try await requestTranslation(
\t\t\t\t\t\tsourceText,
\t\t\t\t\t\ttargetLanguageCode)
\t\t\t\t\tcallback(context, duplicatedCString(translated), nil)
\t\t\t\t} catch {
\t\t\t\t\tcallback(
\t\t\t\t\t\tcontext,
\t\t\t\t\t\tnil,
\t\t\t\t\t\tduplicatedCString(translateErrorCode(error)))
\t\t\t\t}
\t\t\t\treturn
\t\t\t}
#endif
\t\t\tcallback(context, nil, duplicatedCString("unsupported-platform"))
\t\t}
\t\treturn
\t}
\tcallback(context, nil, duplicatedCString("unsupported-platform"))
}
'''

def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    swift_file = os.path.join(root, 'Telegram', 'lib_translate', 'translate_provider_mac_swift.swift')
    if os.path.exists(swift_file):
        with open(swift_file, 'w', encoding='utf-8') as f:
            f.write(NEW_SWIFT_CONTENT)
        print(f"Successfully patched {swift_file}")
    else:
        print(f"Warning: {swift_file} not found")

if __name__ == '__main__':
    main()
