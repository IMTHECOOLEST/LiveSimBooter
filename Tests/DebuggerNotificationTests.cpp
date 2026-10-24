#include "../LiveSimBooter/SimSyscallHelpers.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace {

constexpr unsigned kImageCount = 130;
mach_header sHeaders[kImageCount] = {};
const mach_header *sHeaderPointers[kImageCount] = {};
const char *sPaths[kImageCount] = {};
unsigned sCallCount = 0;
unsigned sImageCount = 0;
unsigned sPublishCount = 0;
uint64_t sLastState = 0;
dyld_image_mode sExpectedMode = dyld_image_adding;
dyld_all_image_infos sCanonicalSnapshot = {};
dyld_all_image_infos *sCanonicalInfo = nullptr;

void checkForwarding(const SimSyscallHelpers *helpers, bool unloading) {
    sCallCount = sImageCount = 0;
    sExpectedMode = unloading ? dyld_image_removing : dyld_image_adding;
    unsigned previousPublishCount = sPublishCount;
    helpers->notifyMonitorOfImageListChanges(unloading, kImageCount, sHeaderPointers, sPaths);
    assert(sPublishCount == previousPublishCount + 1);
    assert(sCallCount == 3 && sImageCount == kImageCount);
    assert(std::memcmp(sCanonicalInfo, &sCanonicalSnapshot, sizeof(sCanonicalSnapshot)) == 0);
}

} // namespace

namespace lsb {
int publishCompactImageInfo(dyld_all_image_infos *info, uint64_t state) {
    assert(info == sCanonicalInfo && (state != 0 || sCallCount == 0));
    sLastState = state;
    ++sPublishCount;
    return 0;
}
} // namespace lsb

void LSBTestDebuggerNotification(dyld_image_mode mode, uint32_t count, const dyld_image_info *images) {
    assert(mode == sExpectedMode);
    assert(sPublishCount != 0);
    assert(sCallCount < 3 && count == (sCallCount < 2 ? 64 : 2));
    assert(LSBGetSimSyscallHelpers()->getProcessInfo() == sCanonicalInfo);
    assert(std::memcmp(sCanonicalInfo, &sCanonicalSnapshot, sizeof(sCanonicalSnapshot)) == 0);
    for (uint32_t index = 0; index < count; ++index) {
        assert(images[index].imageLoadAddress == &sHeaders[sImageCount]);
        assert(images[index].imageFilePath == sPaths[sImageCount]);
        assert(images[index].imageFileModDate == 0);
        ++sImageCount;
    }
    ++sCallCount;
}

int main() {
    const SimSyscallHelpers *helpers = LSBGetSimSyscallHelpers();
    mach_header dyldHeader = {};
    assert(LSBInitializeSimProcessInfo(&dyldHeader, "/app/Documents/RuntimeRoot/usr/lib/dyld_sim"));
    dyld_all_image_infos *originalInfo = helpers->getProcessInfo();
    assert(originalInfo->notification == &LSBDebuggerImageNotification);

    dyld_image_info canonicalImages[2] = { { &sHeaders[0], "first", 17 }, { &sHeaders[1], "second", 29 } };
    dyld_all_image_infos relocatedInfo = *originalInfo;
    relocatedInfo.infoArrayCount = 2;
    relocatedInfo.infoArray = canonicalImages;
    relocatedInfo.infoArrayChangeTimestamp = 12345;
    relocatedInfo.dyldAllImageInfosAddress = &relocatedInfo;
    sCanonicalInfo = &relocatedInfo;
    std::memcpy(&sCanonicalSnapshot, sCanonicalInfo, sizeof(sCanonicalSnapshot));
    LSBSetSimProcessInfo(sCanonicalInfo);
    assert(helpers->getProcessInfo() == sCanonicalInfo && sCanonicalInfo != originalInfo);
    for (unsigned index = 0; index < kImageCount; ++index) {
        sHeaderPointers[index] = &sHeaders[index];
        sPaths[index] = index % 2 ? "odd-image" : "even-image";
    }
    checkForwarding(helpers, false);
    checkForwarding(helpers, true);
    helpers->notifyMonitorOfImageListChanges(false, 0, nullptr, nullptr);
    assert(sPublishCount == 2);
    helpers->notifyMonitorOfDyldBeforeInitializers();
    assert(sPublishCount == 3 && sLastState == 0x40);
    helpers->notifyMonitorOfMainCalled();
    assert(sPublishCount == 4 && sLastState == 0x50);
    assert(sCallCount == 3 && sImageCount == kImageCount);
    assert(canonicalImages[0].imageFileModDate == 17 && canonicalImages[1].imageFileModDate == 29);
    assert(originalInfo->infoArrayCount == 0 && originalInfo->infoArray == nullptr);
    LSBSetSimProcessInfo(originalInfo);
    assert(helpers->getProcessInfo() == originalInfo);
    std::puts("Debugger notifications: add/remove chunking, stable ABI hook, relocated metadata and canonical-list preservation passed");
}
