#import <Foundation/Foundation.h>
#import <Security/Security.h>
#import <Security/CMSEncoder.h>
#import <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// CMS signer for one SHA-256 CodeDirectory. Private key operations stay inside Security.framework/keychain.
// API signatures and CMS attribute layouts: Apple's Security/CMS/CMSPrivate.h and libsecurity_codesigning/lib/signer.cpp.
typedef OSStatus (*SetTimeFn)(CMSEncoderRef, CFAbsoluteTime);
typedef OSStatus (*SetAgilityV1Fn)(CMSEncoderRef, CFDataRef);
typedef OSStatus (*SetAgilityV2Fn)(CMSEncoderRef, CFDictionaryRef);

static void fail(NSString *message) {
    fprintf(stderr, "sign-code-directory: %s\n", message.UTF8String);
    exit(1);
}

static void check(OSStatus status, const char *operation) {
    if (status == errSecSuccess) return;
    CFStringRef detail = SecCopyErrorMessageString(status, NULL);
    NSString *description = detail ? (__bridge NSString *)detail : @"unknown Security error";
    fprintf(stderr, "sign-code-directory: %s: %d (%s)\n", operation, (int)status, description.UTF8String);
    if (detail) CFRelease(detail);
    exit(1);
}

static uint32_t read32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t read64(const uint8_t *p) {
    return ((uint64_t)read32(p) << 32) | read32(p + 4);
}

static NSString *hexString(const uint8_t *bytes, size_t length) {
    NSMutableString *result = [NSMutableString stringWithCapacity:length * 2];
    for (size_t i = 0; i < length; i++) [result appendFormat:@"%02X", bytes[i]];
    return result;
}

static SecIdentityRef findIdentity(NSString *fingerprint) {
    NSDictionary *query = @{(__bridge id)kSecClass: (__bridge id)kSecClassIdentity,
                            (__bridge id)kSecMatchLimit: (__bridge id)kSecMatchLimitAll,
                            (__bridge id)kSecReturnRef: @YES};
    CFTypeRef matches = NULL;
    check(SecItemCopyMatching((__bridge CFDictionaryRef)query, &matches), "SecItemCopyMatching(identity)");
    if (!matches || CFGetTypeID(matches) != CFArrayGetTypeID()) fail(@"identity query did not return an array");
    SecIdentityRef selected = NULL;
    CFArrayRef identities = (CFArrayRef)matches;
    for (CFIndex i = 0; i < CFArrayGetCount(identities); i++) {
        SecIdentityRef candidate = (SecIdentityRef)CFArrayGetValueAtIndex(identities, i);
        if (CFGetTypeID(candidate) != SecIdentityGetTypeID()) fail(@"identity query returned an unexpected type");
        SecCertificateRef certificate = NULL;
        check(SecIdentityCopyCertificate(candidate, &certificate), "SecIdentityCopyCertificate");
        CFDataRef certificateData = SecCertificateCopyData(certificate);
        if (!certificateData) fail(@"cannot obtain public certificate DER");
        uint8_t digest[CC_SHA1_DIGEST_LENGTH];
        CC_SHA1(CFDataGetBytePtr(certificateData), (CC_LONG)CFDataGetLength(certificateData), digest);
        BOOL matchesFingerprint = [hexString(digest, sizeof(digest)) isEqualToString:fingerprint];
        CFRelease(certificateData);
        CFRelease(certificate);
        if (matchesFingerprint) {
            if (selected && !CFEqual(selected, candidate)) fail(@"multiple different identities match the exact certificate fingerprint");
            if (!selected) selected = (SecIdentityRef)CFRetain(candidate);
        }
    }
    CFRelease(matches);
    if (!selected) fail(@"the exact signing certificate SHA-1 was not found in accessible keychain identities");
    return selected;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 4) {
            fprintf(stderr, "usage: %s CERTIFICATE_SHA1 CODEDIRECTORY_FILE OUTPUT_CMS_DER\n", argv[0]);
            return 2;
        }
        NSString *fingerprint = [[NSString stringWithUTF8String:argv[1]] uppercaseString];
        NSCharacterSet *nonHex = [[NSCharacterSet characterSetWithCharactersInString:@"0123456789ABCDEF"] invertedSet];
        if (fingerprint.length != 40 || [fingerprint rangeOfCharacterFromSet:nonHex].location != NSNotFound) fail(@"expected 40 hex characters");
        NSError *error = nil;
        NSData *cd = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:argv[2]] options:0 error:&error];
        if (!cd) fail([NSString stringWithFormat:@"read CodeDirectory: %@", error]);
        const uint8_t *bytes = cd.bytes;
        if (cd.length < 88 || cd.length > UINT32_MAX || read32(bytes) != 0xfade0c02 || read32(bytes + 4) != cd.length)
            fail(@"invalid CodeDirectory magic or length");
        if (read32(bytes + 8) != 0x20400 || bytes[36] != 32 || bytes[37] != 2) fail(@"expected SHA-256 CodeDirectory v0x20400");
        if (bytes[39] != 14 || read32(bytes + 44) != 0) fail(@"expected 16KB pages without scatter");
        if (read64(bytes + 80) != 0) fail(@"unexpected executable-segment flags");
        if (read32(bytes + 12) & 2) fail(@"CodeDirectory must not carry CS_ADHOC");

        SetTimeFn setTime = (SetTimeFn)dlsym(RTLD_DEFAULT, "CMSEncoderSetSigningTime");
        SetAgilityV1Fn setV1 = (SetAgilityV1Fn)dlsym(RTLD_DEFAULT, "CMSEncoderSetAppleCodesigningHashAgility");
        SetAgilityV2Fn setV2 = (SetAgilityV2Fn)dlsym(RTLD_DEFAULT, "CMSEncoderSetAppleCodesigningHashAgilityV2");
        if (!setTime || !setV1 || !setV2) fail(@"required CMS attribute API is unavailable");

        uint8_t digest[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(cd.bytes, (CC_LONG)cd.length, digest);
        NSData *fullHash = [NSData dataWithBytes:digest length:sizeof(digest)];
        NSData *shortHash = [NSData dataWithBytes:digest length:20];
        NSDictionary *v1Object = @{@"cdhashes": @[shortHash]};
        NSData *v1Data = [NSPropertyListSerialization dataWithPropertyList:v1Object format:NSPropertyListXMLFormat_v1_0 options:0 error:&error];
        if (!v1Data) fail([NSString stringWithFormat:@"serialize hash-agility V1: %@", error]);
        // SEC_OID_SHA256 = 192 in Apple's Security/CMS/SecCmsBase.h.
        NSDictionary *v2Object = @{@192: fullHash};

        SecIdentityRef identity = findIdentity(fingerprint);
        CMSEncoderRef encoder = NULL;
        check(CMSEncoderCreate(&encoder), "CMSEncoderCreate");
        check(CMSEncoderSetCertificateChainMode(encoder, kCMSCertificateChainWithRootOrFail), "CMSEncoderSetCertificateChainMode");
        check(CMSEncoderAddSigners(encoder, identity), "CMSEncoderAddSigners");
        check(CMSEncoderSetSignerAlgorithm(encoder, kCMSEncoderDigestAlgorithmSHA256), "CMSEncoderSetSignerAlgorithm");
        check(CMSEncoderSetHasDetachedContent(encoder, true), "CMSEncoderSetHasDetachedContent");
        check(CMSEncoderAddSignedAttributes(encoder, kCMSAttrSigningTime), "add signing time attribute");
        check(setTime(encoder, CFAbsoluteTimeGetCurrent()), "set signing time");
        check(CMSEncoderAddSignedAttributes(encoder, kCMSAttrAppleCodesigningHashAgilityV2), "add hash-agility V2 attribute");
        check(setV2(encoder, (__bridge CFDictionaryRef)v2Object), "set hash-agility V2");
        check(CMSEncoderAddSignedAttributes(encoder, kCMSAttrAppleCodesigningHashAgility), "add hash-agility V1 attribute");
        check(setV1(encoder, (__bridge CFDataRef)v1Data), "set hash-agility V1");
        check(CMSEncoderUpdateContent(encoder, cd.bytes, cd.length), "CMSEncoderUpdateContent");
        CFDataRef output = NULL;
        check(CMSEncoderCopyEncodedContent(encoder, &output), "CMSEncoderCopyEncodedContent");
        NSData *outputData = (__bridge NSData *)output;
        BOOL wrote = [outputData writeToFile:[NSString stringWithUTF8String:argv[3]] options:NSDataWritingAtomic error:&error];
        if (!wrote) fail([NSString stringWithFormat:@"write CMS DER: %@", error]);
        printf("Signed CodeDirectory with certificate SHA-1 %s; cdhash=%s; CMS bytes=%ld\n", fingerprint.UTF8String,
               hexString(digest, 20).UTF8String, (long)CFDataGetLength(output));
        CFRelease(output);
        CFRelease(encoder);
        CFRelease(identity);
        return 0;
    }
}
