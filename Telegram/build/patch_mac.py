import os, sys
from pathlib import Path

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

def patch_tgcalls_crypto_header(path):
    content = path.read_text(encoding='utf-8')
    old = 'extern "C" {\n#include <cstdint>\n'
    new = '#include <cstdint>\n\nextern "C" {\n'
    if content.count(old) == 1:
        path.write_text(content.replace(old, new, 1), encoding='utf-8')
        print(f"Successfully patched {path}")
    elif old not in content and content.count(new) == 1:
        print(f"Already patched {path}")
    else:
        raise RuntimeError(f"Unexpected tgcalls CryptoHelper header: {path}")


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    patch_tgcalls_crypto_header(Path(root) / 'Telegram/ThirdParty/tgcalls/tgcalls/CryptoHelper.h')
    swift_file = os.path.join(root, 'Telegram', 'lib_translate', 'translate_provider_mac_swift.swift')
    if os.path.exists(swift_file):
        with open(swift_file, 'w', encoding='utf-8') as f:
            f.write(NEW_SWIFT_CONTENT)
        print(f"Successfully patched {swift_file}")
    else:
        print(f"Warning: {swift_file} not found")

if __name__ == '__main__':
    main()
