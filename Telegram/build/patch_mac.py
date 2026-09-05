import os, sys

NEW_SWIFT_CONTENT = '''// This file is part of Desktop App Toolkit,
// a set of libraries for developing nice desktop applications.
//
// For license and copyright information please follow this link:
// https://github.com/desktop-app/legal/blob/master/LEGAL
//
import Foundation

typealias TranslateProviderMacSwiftCallback = @convention(c) (
\tUnsafeMutableRawPointer?,
\tUnsafePointer<CChar>?,
\tUnsafePointer<CChar>?
) -> Void

private func duplicatedCString(_ value: String) -> UnsafePointer<CChar>? {
\tguard let duplicated = strdup(value) else {
\t\treturn nil
\t}
\treturn UnsafePointer(duplicated)
}

@_cdecl("TranslateProviderMacSwiftIsAvailable")
func TranslateProviderMacSwiftIsAvailable() -> Bool {
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
