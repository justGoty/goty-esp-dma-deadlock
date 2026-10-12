#include "DMA/MemoryPolicy.h"
#include "Deadlock/GlobalPatterns.h"
#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
int checks{};
void Check(bool condition, const char* description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}

// Independent machine-code fixtures from the dumper signatures: LEA returns the
// target's address, whereas the old MOV patterns load a value stored at a slot.
constexpr std::array<uint8_t, 11> ViewMatrixCode{
    0x48, 0x8d, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x48, 0xc1, 0xe0, 0x06
};
constexpr std::array<uint8_t, 22> PredictionCode{
    0x48, 0x8d, 0x05, 0x00, 0x00, 0x00, 0x00, 0xc3,
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0x40, 0x53, 0x56, 0x41, 0x54, 0x00
};
// The last fixture byte belongs to the following instruction and is deliberately
// outside the signature, so exact end-of-buffer and prefix matching are distinct.
constexpr size_t PredictionSignatureSize = 21;

std::optional<size_t> UniqueCandidate(std::span<const uint8_t> bytes,
    const GlobalPatterns::RipSignature& signature) {
    auto first = MemoryPolicy::FindPattern(bytes, signature.bytes, signature.mask);
    if (!first) return {};
    if (MemoryPolicy::FindPattern(bytes.subspan(*first + 1), signature.bytes, signature.mask)) return {};
    return first;
}

std::optional<ptrdiff_t> ResolveFixture(std::span<const uint8_t> bytes,
    size_t sectionRva, const GlobalPatterns::RipSignature& signature) {
    auto found = UniqueCandidate(bytes, signature);
    if (!found || signature.displacementOffset > bytes.size() - *found
        || sizeof(int32_t) > bytes.size() - *found - signature.displacementOffset) return {};
    int32_t displacement{};
    std::memcpy(&displacement, bytes.data() + *found + signature.displacementOffset, sizeof(displacement));
    return MemoryPolicy::RipRelative(sectionRva + *found, signature.instructionLength, displacement);
}

template<size_t N>
void TestPattern(const std::array<uint8_t, N>& code, size_t signatureSize,
    const GlobalPatterns::RipSignature& signature) {
    Check(signature.bytes.size() == signatureSize && signature.mask.size() == signatureSize,
        "production pattern has independently known length");
    Check(signature.displacementOffset == 3 && signature.instructionLength == 7,
        "LEA uses a signed displacement at byte three and instruction length seven");
    Check(UniqueCandidate(std::span(code).first(signatureSize), signature) == 0,
        "signature matches byte zero in an exact-size executable buffer");
    Check(!UniqueCandidate(std::span(code).first(signatureSize - 1), signature),
        "one-byte-short buffer cannot contain a signature");
    Check(!UniqueCandidate({}, signature), "empty section cannot match");

    constexpr size_t sectionRva = 0x1000;
    for (size_t prefix = 0; prefix < 25; ++prefix) {
        std::vector<uint8_t> fixture(prefix, 0x90);
        fixture.insert(fixture.end(), code.begin(), code.begin() + signatureSize);
        Check(UniqueCandidate(fixture, signature) == prefix, "last valid signature location is accepted");
        for (const int32_t displacement : std::array<int32_t, 5>{0, 0x12345, -0x300, 1, -1}) {
            std::memcpy(fixture.data() + prefix + 3, &displacement, sizeof(displacement));
            auto result = ResolveFixture(fixture, sectionRva, signature);
            Check(result == static_cast<ptrdiff_t>(sectionRva + prefix + 7) + displacement,
                "forward backward and zero RIP displacement preserve signed meaning");
        }
        std::memcpy(fixture.data() + prefix + 3, code.data() + 3, sizeof(int32_t));
        fixture.push_back(0x90);
        fixture.insert(fixture.end(), code.begin(), code.begin() + signatureSize);
        Check(!UniqueCandidate(fixture, signature), "two valid signatures are rejected as ambiguous");
    }

    for (size_t byte = 0; byte < signatureSize; ++byte) {
        auto corrupted = code;
        corrupted[byte] ^= 0x01;
        auto result = UniqueCandidate(std::span(corrupted).first(signatureSize), signature);
        Check((byte >= 3 && byte < 7) ? result == 0 : !result,
            "only the independently known RIP displacement bytes are wildcards");
    }
    auto negativeTarget = code;
    const int32_t tooNegative = -0x2000;
    std::memcpy(negativeTarget.data() + 3, &tooNegative, sizeof(tooNegative));
    Check(!ResolveFixture(std::span(negativeTarget).first(signatureSize), sectionRva, signature),
        "RIP target below module-relative zero is rejected");
}

void TestOldPatternsFail() {
    const std::string_view oldMatrixBytes = "\xF3\x0F\x10\x05????\xF3\x0F\x59\x01";
    const std::string_view oldPredictionBytes = "\x48\x8B\x05????\x38\x58";
    Check(!MemoryPolicy::FindPattern(ViewMatrixCode, oldMatrixBytes, "xxxx????xxxx"),
        "new matrix fixture reproduces missing old matrix signature");
    Check(!MemoryPolicy::FindPattern(PredictionCode, oldPredictionBytes, "xxx????xx"),
        "new prediction fixture reproduces missing old prediction signature");
    const std::array<uint8_t, 12> oldMatrixFixture{
        0xf3, 0x0f, 0x10, 0x05, 0x04, 0x03, 0x02, 0x01, 0xf3, 0x0f, 0x59, 0x01
    };
    const std::array<uint8_t, 9> oldPredictionFixture{
        0x48, 0x8b, 0x05, 0x04, 0x03, 0x02, 0x01, 0x38, 0x58
    };
    Check(!UniqueCandidate(oldMatrixFixture, GlobalPatterns::ViewMatrix),
        "new matrix signature cannot silently accept old scalar MOV code");
    Check(!UniqueCandidate(oldPredictionFixture, GlobalPatterns::PredictionInstance),
        "new prediction signature cannot silently accept pointer-slot MOV code");
}

void TestPredictionAddressMeaning() {
    constexpr uintptr_t moduleBase = 0x00007ff600000000ULL;
    constexpr ptrdiff_t instanceRva = 0x120000;
    constexpr uintptr_t instanceAddress = moduleBase + instanceRva;
    constexpr uintptr_t firstWordVtable = 0x00007ff601234000ULL;
    std::array<uint8_t, 0x80> object{};
    std::memcpy(object.data(), &firstWordVtable, sizeof(firstWordVtable));
    constexpr float actualTime = 12.75f;
    std::memcpy(object.data() + 0x68, &actualTime, sizeof(actualTime));
    const auto address = GlobalPatterns::PredictionInstanceAddress(moduleBase, instanceRva);
    Check(address == instanceAddress, "LEA target is the Prediction object instance itself");
    Check(address != firstWordVtable, "Prediction object first word is not a pointer slot to dereference");
    const uintptr_t timeAddress = address + 0x68;
    Check(timeAddress >= instanceAddress && timeAddress + sizeof(float) <= instanceAddress + object.size(),
        "server-time read belongs to the instance fixture");
    float readTime{};
    std::memcpy(&readTime, object.data() + (timeAddress - instanceAddress), sizeof(readTime));
    Check(readTime == actualTime, "server-time address uses direct instance target");
    Check(firstWordVtable + 0x68 >= instanceAddress + object.size(),
        "old pointer dereference would read outside the object fixture");
    Check(GlobalPatterns::PredictionInstanceAddress(0, instanceRva) == 0, "zero module base fails closed");
    Check(GlobalPatterns::PredictionInstanceAddress(moduleBase, 0) == 0, "zero unresolved RVA fails closed");
    Check(GlobalPatterns::PredictionInstanceAddress(moduleBase, -1) == 0, "negative RVA fails closed");
    Check(GlobalPatterns::PredictionInstanceAddress(moduleBase, std::numeric_limits<ptrdiff_t>::min()) == 0,
        "minimum negative RVA fails closed without signed overflow");
    constexpr uintptr_t maximum = std::numeric_limits<uintptr_t>::max();
    Check(GlobalPatterns::PredictionInstanceAddress(maximum - 7, 7) == maximum,
        "largest representable target is accepted");
    Check(GlobalPatterns::PredictionInstanceAddress(maximum - 7, 8) == 0, "module-plus-RVA overflow fails closed");
    Check(GlobalPatterns::PredictionInstanceAddress(1, std::numeric_limits<ptrdiff_t>::max())
        == static_cast<uintptr_t>(std::numeric_limits<ptrdiff_t>::max()) + 1,
        "largest positive RVA does not overflow signed arithmetic");
    Check(!MemoryPolicy::RipRelative(static_cast<size_t>(PTRDIFF_MAX), 7, 0),
        "RIP target above positive ptrdiff range is rejected");
    Check(!MemoryPolicy::RipRelative(1, 7, -8), "RIP target at zero is rejected");
}
}

int main() {
    try {
        TestPattern(ViewMatrixCode, ViewMatrixCode.size(), GlobalPatterns::ViewMatrix);
        TestPattern(PredictionCode, PredictionSignatureSize, GlobalPatterns::PredictionInstance);
        TestOldPatternsFail();
        TestPredictionAddressMeaning();
        std::cout << "PASS: " << checks << " global-pattern checks; no hardware or process access.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "FAIL after " << checks << " checks: " << exception.what() << '\n';
        return 1;
    }
}
