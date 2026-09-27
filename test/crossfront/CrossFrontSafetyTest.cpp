#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "crossfront/CrossFrontCrypto.h"
#include "crossfront/CrossFrontFileSafety.h"
#include "crossfront/SleepImageRequest.h"

extern "C" int esp_read_mac(uint8_t* mac, int) {
  constexpr std::array<uint8_t, 6> hardwareMac = {0x00, 0x1A, 0x2B, 0xC3, 0xD4, 0xE5};
  std::copy(hardwareMac.begin(), hardwareMac.end(), mac);
  return 0;
}

extern "C" uint32_t esp_random() {
  return 0;
}

namespace {

struct FakeStorage {
  std::unordered_map<std::string, std::string> files;
  std::string failedRenameSource;

  bool exists(const char* path) const { return files.count(path) != 0; }
  bool rename(const char* from, const char* to) {
    if (failedRenameSource == from || !exists(from) || exists(to)) return false;
    files.emplace(to, files.at(from));
    files.erase(from);
    return true;
  }
  bool remove(const char* path) { return files.erase(path) != 0; }
};

}

TEST(CrossFrontCrypto, DeviceIdUsesTwelveUppercaseHexDigitsWithoutPrefix) {
  std::array<char, 14> buffer{};
  buffer.back() = '!';
  crossfront::getDeviceHardwareId(buffer.data(), buffer.size() - 1);
  EXPECT_STREQ(buffer.data(), "001A2BC3D4E5");
  EXPECT_EQ(buffer.back(), '!');
}

TEST(CrossFrontCrypto, DeviceIdRespectsSmallOutputBuffer) {
  std::array<char, 5> buffer{};
  buffer.back() = '!';
  crossfront::getDeviceHardwareId(buffer.data(), buffer.size() - 1);
  EXPECT_STREQ(buffer.data(), "001");
  EXPECT_EQ(buffer.back(), '!');
}

TEST(CrossFrontCrypto, PairingTokenHasExpectedLetterDigitShape) {
  std::array<char, 10> buffer{};
  buffer.back() = '!';
  crossfront::generateRandomToken(buffer.data(), 8);
  EXPECT_STREQ(buffer.data(), "AAA00000");
  EXPECT_EQ(buffer.back(), '!');
}

TEST(CrossFrontCrypto, LongTokenIsTerminatedAndUsesSafeAlphabet) {
  std::array<char, 34> buffer{};
  buffer.back() = '!';
  crossfront::generateRandomToken(buffer.data(), 32);
  EXPECT_EQ(std::string(buffer.data()), std::string(32, '0'));
  EXPECT_EQ(buffer.back(), '!');
}

TEST(CrossFrontSleepImage, UsesCanonicalDevicePathWithoutCacheNonce) {
  const std::string server = "https://cf-api.pocketgo.org";
  const std::string url = crossfront::sleepImageRequestUrl(server, "7CE8B18EF308");
  EXPECT_EQ(url, "https://cf-api.pocketgo.org/api/cf/device/7CE8B18EF308/sleep.bmp");
  EXPECT_EQ(url.find("?v="), std::string::npos);
  EXPECT_EQ(url.find("token"), std::string::npos);
}

TEST(CrossFrontSleepImage, DiscardsStaleBitmapEtagAndBackupBeforeFetch) {
  FakeStorage storage{{{"/.crosspoint/cf_sleep.bmp", "old"}, {"/.crosspoint/cf_sleep.etag", "old-etag"},
                       {"/.crosspoint/cf_sleep.bmp.bak", "older"}}, ""};
  crossfront::discardSleepImageCache(storage, "/.crosspoint/cf_sleep.bmp", "/.crosspoint/cf_sleep.etag",
                                    "/.crosspoint/cf_sleep.bmp.bak");
  EXPECT_TRUE(storage.files.empty());
}

TEST(CrossFrontFileSafety, RejectsUnsafeAssignmentIdsBeforeUrlConstruction) {
  EXPECT_TRUE(crossfront::isSafeAssignmentId("abc123_def-456"));
  EXPECT_FALSE(crossfront::isSafeAssignmentId(""));
  EXPECT_FALSE(crossfront::isSafeAssignmentId("../rotate-token"));
  EXPECT_FALSE(crossfront::isSafeAssignmentId("a?download=1"));
  EXPECT_FALSE(crossfront::isSafeAssignmentId("a#fragment"));
  EXPECT_FALSE(crossfront::isSafeAssignmentId(std::string(65, 'a')));
}

TEST(CrossFrontFileSafety, RejectsFileNamesThatEscapeOrAliasSdPaths) {
  EXPECT_TRUE(crossfront::isSafeFileName("Sách của tôi.epub"));
  EXPECT_TRUE(crossfront::isSafeFileName("Font_400.ttf"));
  for (const auto name : {"", ".", "..", "../settings.json", "books/child.epub", "..\\secrets", "C:evil.epub",
                          "bad\nname", "bad\x7f" "name", "book.", "book "}) {
    EXPECT_FALSE(crossfront::isSafeFileName(name)) << name;
  }
  EXPECT_FALSE(crossfront::isSafeFileName(std::string("bad\0name", 8)));
  EXPECT_TRUE(crossfront::isSafeFileName(std::string(249, 'a')));
  EXPECT_FALSE(crossfront::isSafeFileName(std::string(250, 'a')));
}

TEST(CrossFrontFileSafety, RejectsFolderTraversalAndRootDestination) {
  EXPECT_TRUE(crossfront::isSafeTargetFolder("/Books/Novels"));
  EXPECT_TRUE(crossfront::isSafeTargetFolder("/.fonts/Family"));
  for (const auto folder : {"", "/", "Books", "/Books/../.crosspoint", "/Books/./child", "/Books//child",
                            "/Books/", "/Books\\..\\settings", "/Books:evil", "/Books\nother",
                            "/.crosspoint", "/.CROSSPOINT/crossfront.json"}) {
    EXPECT_FALSE(crossfront::isSafeTargetFolder(folder)) << folder;
  }
  EXPECT_FALSE(crossfront::isSafeTargetFolder(std::string("/Books\0/other", 13)));
  EXPECT_FALSE(crossfront::isSafeTargetFolder("/Books/" + std::string(250, 'a')));
}

TEST(CrossFrontFileSafety, ReplacesExistingFileOnlyAfterDownloadIsReady) {
  FakeStorage storage{{{"/Books/book.epub", "old"}, {"/Books/book.epub.tmp", "new"}}, ""};
  EXPECT_TRUE(crossfront::replaceDownloadedFile(storage, "/Books/book.epub.tmp", "/Books/book.epub"));
  EXPECT_EQ(storage.files.at("/Books/book.epub"), "new");
  EXPECT_FALSE(storage.exists("/Books/book.epub.cfbak"));
}

TEST(CrossFrontFileSafety, FailedRenameRestoresPreviousFile) {
  FakeStorage storage{{{"/Books/book.epub", "old"}, {"/Books/book.epub.tmp", "new"}},
                      "/Books/book.epub.tmp"};
  EXPECT_FALSE(crossfront::replaceDownloadedFile(storage, "/Books/book.epub.tmp", "/Books/book.epub"));
  EXPECT_EQ(storage.files.at("/Books/book.epub"), "old");
  EXPECT_EQ(storage.files.at("/Books/book.epub.tmp"), "new");
  EXPECT_FALSE(storage.exists("/Books/book.epub.cfbak"));
}

TEST(CrossFrontFileSafety, FailedBackupRenameDoesNotTouchEitherFile) {
  FakeStorage storage{{{"/Books/book.epub", "old"}, {"/Books/book.epub.tmp", "new"}},
                      "/Books/book.epub"};
  EXPECT_FALSE(crossfront::replaceDownloadedFile(storage, "/Books/book.epub.tmp", "/Books/book.epub"));
  EXPECT_EQ(storage.files.at("/Books/book.epub"), "old");
  EXPECT_EQ(storage.files.at("/Books/book.epub.tmp"), "new");
}

TEST(CrossFrontFileSafety, InterruptedReplacementRestoresBackupFirst) {
  FakeStorage storage{{{"/Books/book.epub.cfbak", "old"}, {"/Books/book.epub.tmp", "new"}},
                      "/Books/book.epub.tmp"};
  EXPECT_FALSE(crossfront::replaceDownloadedFile(storage, "/Books/book.epub.tmp", "/Books/book.epub"));
  EXPECT_EQ(storage.files.at("/Books/book.epub"), "old");
  EXPECT_FALSE(storage.exists("/Books/book.epub.cfbak"));
}

TEST(CrossFrontFileSafety, ExistingBackupNeverGetsOverwritten) {
  FakeStorage storage{{{"/Books/book.epub", "current"},
                       {"/Books/book.epub.cfbak", "older"},
                       {"/Books/book.epub.tmp", "new"}}, ""};
  EXPECT_FALSE(crossfront::replaceDownloadedFile(storage, "/Books/book.epub.tmp", "/Books/book.epub"));
  EXPECT_EQ(storage.files.at("/Books/book.epub"), "current");
  EXPECT_EQ(storage.files.at("/Books/book.epub.cfbak"), "older");
}
