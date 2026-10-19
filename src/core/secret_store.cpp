#include "skywave_link/secret_store.hpp"

#include <atomic>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

namespace skylink::secrets {

namespace {
std::atomic<bool> g_system{false};

#ifdef __APPLE__
constexpr const char* kService = "io.github.pingpongshow.skywave-link";

CFStringRef cfString(const std::string& s) {
    return CFStringCreateWithBytes(kCFAllocatorDefault,
                                   reinterpret_cast<const UInt8*>(s.data()),
                                   static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8,
                                   false);
}

// Base query for one generic-password item: service + account.
CFMutableDictionaryRef baseQuery(const std::string& key) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFStringRef service = cfString(kService);
    CFStringRef account = cfString(key);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, service);
    CFDictionarySetValue(q, kSecAttrAccount, account);
    CFRelease(service);
    CFRelease(account);
    return q;
}
#endif
}  // namespace

void useSystemStore(bool on) { g_system = on; }

bool systemStoreActive() {
#ifdef __APPLE__
    return g_system;
#else
    return false;
#endif
}

std::optional<std::string> load(const std::string& key) {
#ifdef __APPLE__
    if (!systemStoreActive()) return std::nullopt;
    CFMutableDictionaryRef q = baseQuery(key);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef out = nullptr;
    const OSStatus st = SecItemCopyMatching(q, &out);
    CFRelease(q);
    if (st != errSecSuccess || !out) return std::nullopt;
    CFDataRef data = static_cast<CFDataRef>(out);
    std::string value(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                      static_cast<size_t>(CFDataGetLength(data)));
    CFRelease(out);
    return value;
#else
    (void)key;
    return std::nullopt;
#endif
}

bool store(const std::string& key, const std::string& value) {
#ifdef __APPLE__
    if (!systemStoreActive()) return false;
    CFMutableDictionaryRef q = baseQuery(key);
    if (value.empty()) {
        const OSStatus st = SecItemDelete(q);
        CFRelease(q);
        return st == errSecSuccess || st == errSecItemNotFound;
    }
    CFDataRef data = CFDataCreate(kCFAllocatorDefault,
                                  reinterpret_cast<const UInt8*>(value.data()),
                                  static_cast<CFIndex>(value.size()));
    // Update in place when the item exists, else add it.
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kSecValueData, data);
    OSStatus st = SecItemUpdate(q, attrs);
    if (st == errSecItemNotFound) {
        CFDictionarySetValue(q, kSecValueData, data);
        CFDictionarySetValue(q, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
        st = SecItemAdd(q, nullptr);
    }
    CFRelease(attrs);
    CFRelease(data);
    CFRelease(q);
    return st == errSecSuccess;
#else
    (void)key;
    (void)value;
    return false;
#endif
}

}  // namespace skylink::secrets
