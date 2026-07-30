#include <unity.h>

#include <array>
#include <cstdint>
#include <string>

#include "algaguard/qr_onboarding.hpp"
#include "algaguard/qr_credential_bootstrap.hpp"

void setUp() {}
void tearDown() {}

namespace {
class Random final : public algaguard::QrRandomSource {
 public:
  bool succeed{true};
  std::uint8_t calls{};
  bool fill(std::uint8_t* output, std::size_t size) override {
    if (!succeed) return false;
    ++calls;
    for (std::size_t index = 0; index < size; ++index)
      output[index] = static_cast<std::uint8_t>(index + calls);
    return true;
  }
};

class Crypto final : public algaguard::QrBindingCrypto {
 public:
  bool signatureValid{true};
  bool sha256(std::string_view input,
              std::array<std::uint8_t, 32>& output) override {
    output.fill(0);
    for (std::size_t index = 0; index < input.size(); ++index)
      output[index % output.size()] ^= static_cast<std::uint8_t>(input[index]);
    return true;
  }
  bool verifyP256Sha256(const std::uint8_t*, std::size_t,
                        const std::uint8_t* signature,
                        std::size_t signatureSize) override {
    return signatureValid && signatureSize == 64 && signature[0] == 0x5a;
  }
};

class Keys final : public algaguard::LocalPrivateKeyProvider {
 public:
  bool generated{};
  bool destroyed{};
  std::optional<algaguard::PrivateKeyHandle> generate(
      algaguard::KeyAlgorithm algorithm) override {
    generated = algorithm == algaguard::KeyAlgorithm::kRsa3072;
    return generated ? std::optional<algaguard::PrivateKeyHandle>{{1, 1}}
                     : std::nullopt;
  }
  std::optional<algaguard::CsrSubmission> create_csr(
      const algaguard::PrivateKeyHandle&,
      const algaguard::DeviceBinding& binding) override {
    return algaguard::CsrSubmission{"PUBLIC CSR", binding,
                                    algaguard::KeyAlgorithm::kRsa3072};
  }
  bool destroy(const algaguard::PrivateKeyHandle&) override {
    destroyed = true;
    return true;
  }
};

class Storage final : public algaguard::SecureCredentialStorage {
 public:
  bool staged{};
  bool active{};
  bool stage(const algaguard::PrivateKeyHandle&,
             const algaguard::PublicCredentialBundle&) override {
    staged = true;
    return true;
  }
  bool activate_staged() override { active = staged; return active; }
  void discard_staged() override { staged = false; }
  std::optional<algaguard::PublicCredentialBundle> active_public_bundle()
      const override { return std::nullopt; }
};

class BootstrapTransport final : public algaguard::QrCredentialBootstrapTransport {
 public:
  bool exchangeAllowed{true};
  bool issueAllowed{true};
  bool wrongBinding{};
  std::optional<algaguard::QrBootstrapAuthorization> exchange(
      std::string_view, std::string_view) override {
    if (!exchangeAllowed) return std::nullopt;
    algaguard::QrBootstrapAuthorization value;
    value.deviceUuid = "10000000-0000-4000-8000-000000000001";
    value.bootstrapToken = "synthetic-memory-only-authorization";
    return value;
  }
  std::optional<algaguard::PublicCredentialBundle> issue(
      std::string_view, const algaguard::CsrSubmission& csr) override {
    if (!issueAllowed) return std::nullopt;
    const auto uuidValue = wrongBinding
        ? "20000000-0000-4000-8000-000000000002"
        : csr.binding.device_uuid;
    return algaguard::PublicCredentialBundle{
        "30000000-0000-4000-8000-000000000003", "PUBLIC CERT", {"PUBLIC CA"},
        {csr.binding.device_id, {"urn:algaguard:device:" + uuidValue}},
        1, 2, false, false};
  }
};

algaguard::QrCredentialBootstrapContext bootstrapContext() {
  algaguard::QrCredentialBootstrapContext value;
  value.sessionId = "50000000-0000-4000-8000-000000000001";
  value.deviceId = "AG-000001";
  value.sessionToken = "synthetic-memory-only-session-token";
  return value;
}

std::string base64(const std::uint8_t* input, std::size_t size) {
  constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string output;
  std::uint32_t accumulator{};
  unsigned bits{};
  for (std::size_t index = 0; index < size; ++index) {
    accumulator = (accumulator << 8U) | input[index];
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      output.push_back(alphabet[(accumulator >> bits) & 0x3fU]);
    }
  }
  if (bits != 0) output.push_back(alphabet[(accumulator << (6U - bits)) & 0x3fU]);
  return output;
}

std::array<std::uint8_t, 16> uuid(std::string_view text) {
  std::array<std::uint8_t, 16> output{};
  std::size_t written{};
  auto nibble = [](char value) {
    return value >= '0' && value <= '9' ? value - '0'
           : value >= 'a' && value <= 'f' ? value - 'a' + 10
                                          : value - 'A' + 10;
  };
  for (std::size_t index = 0; index < text.size();) {
    if (text[index] == '-') {
      ++index;
      continue;
    }
    output[written++] = static_cast<std::uint8_t>(
        (nibble(text[index]) << 4) | nibble(text[index + 1]));
    index += 2;
  }
  return output;
}

std::array<std::uint8_t, 31> decodeInvitation(std::string_view uri) {
  std::array<std::uint8_t, 31> output{};
  std::uint32_t accumulator{};
  unsigned bits{};
  std::size_t written{};
  for (char value : uri.substr(7)) {
    int decoded = value >= 'A' && value <= 'Z' ? value - 'A'
                  : value >= 'a' && value <= 'z' ? value - 'a' + 26
                  : value >= '0' && value <= '9' ? value - '0' + 52
                  : value == '-' ? 62 : 63;
    accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(decoded);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output[written++] = static_cast<std::uint8_t>(accumulator >> bits);
    }
  }
  return output;
}

std::string grant(algaguard::QrOnboardingManager& manager, Crypto& crypto,
                  std::string_view token = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
                  std::uint32_t expiry = 1300) {
  constexpr std::string_view session = "50000000-0000-4000-8000-000000000001";
  auto invitation = decodeInvitation(manager.uri());
  std::array<std::uint8_t, algaguard::kQrBindingGrantBytes> bytes{};
  bytes[0] = 1;
  bytes[3] = 1;
  std::copy_n(invitation.data() + 4, 16, bytes.data() + 4);
  const auto sessionBytes = uuid(session);
  std::copy(sessionBytes.begin(), sessionBytes.end(), bytes.begin() + 20);
  bytes[36] = static_cast<std::uint8_t>(expiry >> 24U);
  bytes[37] = static_cast<std::uint8_t>(expiry >> 16U);
  bytes[38] = static_cast<std::uint8_t>(expiry >> 8U);
  bytes[39] = static_cast<std::uint8_t>(expiry);
  bytes[47] = 1;
  std::array<std::uint8_t, 32> hash{};
  crypto.sha256(token, hash);
  std::copy(hash.begin(), hash.end(), bytes.begin() + 48);
  bytes[80] = 1;
  bytes[81] = 0x5a;
  return base64(bytes.data(), bytes.size());
}

constexpr std::string_view kSession =
    "50000000-0000-4000-8000-000000000001";
constexpr std::string_view kToken = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";

void test_223_disabled_by_default() {
  Random random;
  algaguard::QrOnboardingManager manager{random};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::QrOnboardingState::kDisabled),
                        static_cast<int>(manager.state()));
}
void test_224_crypto_nonce_and_compact_uri() {
  Random random;
  algaguard::QrOnboardingManager manager{random};
  TEST_ASSERT_TRUE(manager.generate("AG-000001", 1000));
  TEST_ASSERT_EQUAL(algaguard::kQrInvitationUriBytes, manager.uri().size());
  TEST_ASSERT_EQUAL_STRING_LEN("ag://q/", manager.uri().data(), 7);
}
void test_225_invalid_device_rejected() {
  Random random;
  algaguard::QrOnboardingManager manager{random};
  TEST_ASSERT_FALSE(manager.generate("AG-000000", 1000));
}
void test_226_rng_failure_is_error() {
  Random random;
  random.succeed = false;
  algaguard::QrOnboardingManager manager{random};
  TEST_ASSERT_FALSE(manager.generate("AG-000001", 1000));
  TEST_ASSERT_TRUE(manager.nonceCleared());
}
void test_227_lifetime_is_bounded() {
  Random random;
  algaguard::QrOnboardingManager manager{random};
  TEST_ASSERT_FALSE(manager.generate("AG-000001", 1000, 301));
}
void test_228_expiry_zeroizes_nonce() {
  Random random;
  algaguard::QrOnboardingManager manager{random};
  TEST_ASSERT_TRUE(manager.generate("AG-000001", 1000, 180));
  manager.tick(1180);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::QrOnboardingState::kExpired),
                        static_cast<int>(manager.state()));
  TEST_ASSERT_TRUE(manager.nonceCleared());
}
void test_229_valid_binding_consumes_once() {
  Random random;
  Crypto crypto;
  algaguard::QrOnboardingManager manager{random};
  TEST_ASSERT_TRUE(manager.generate("AG-000001", 1000));
  const auto signedGrant = grant(manager, crypto);
  auto authorized = manager.authorize(signedGrant, kSession, "AG-000001",
                                      kToken, 1100, crypto);
  TEST_ASSERT_TRUE(authorized.has_value());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::QrOnboardingState::kConsumed),
                        static_cast<int>(manager.state()));
  TEST_ASSERT_FALSE(manager.authorize(signedGrant, kSession, "AG-000001",
                                      kToken, 1100, crypto).has_value());
}
void test_230_wrong_device_denied() {
  Random random; Crypto crypto; algaguard::QrOnboardingManager manager{random};
  TEST_ASSERT_TRUE(manager.generate("AG-000001", 1000));
  TEST_ASSERT_FALSE(manager.authorize(grant(manager, crypto), kSession, "AG-000002",
                                      kToken, 1100, crypto).has_value());
}
void test_231_wrong_token_denied() {
  Random random; Crypto crypto; algaguard::QrOnboardingManager manager{random};
  TEST_ASSERT_TRUE(manager.generate("AG-000001", 1000));
  TEST_ASSERT_FALSE(manager.authorize(grant(manager, crypto), kSession, "AG-000001",
                                      "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy", 1100, crypto).has_value());
}
void test_232_wrong_signature_denied() {
  Random random; Crypto crypto; algaguard::QrOnboardingManager manager{random};
  TEST_ASSERT_TRUE(manager.generate("AG-000001", 1000));
  crypto.signatureValid = false;
  TEST_ASSERT_FALSE(manager.authorize(grant(manager, crypto), kSession, "AG-000001",
                                      kToken, 1100, crypto).has_value());
}
void test_233_expired_grant_denied() {
  Random random; Crypto crypto; algaguard::QrOnboardingManager manager{random};
  TEST_ASSERT_TRUE(manager.generate("AG-000001", 1000));
  TEST_ASSERT_FALSE(manager.authorize(grant(manager, crypto, kToken, 1100), kSession,
                                      "AG-000001", kToken, 1100, crypto).has_value());
}
void test_234_malformed_grant_denied() {
  Random random; Crypto crypto; algaguard::QrOnboardingManager manager{random};
  TEST_ASSERT_TRUE(manager.generate("AG-000001", 1000));
  TEST_ASSERT_FALSE(manager.authorize("malformed", kSession, "AG-000001",
                                      kToken, 1100, crypto).has_value());
}
void test_235_clear_is_idempotent() {
  Random random; algaguard::QrOnboardingManager manager{random};
  TEST_ASSERT_TRUE(manager.generate("AG-000001", 1000));
  manager.clear(); manager.clear();
  TEST_ASSERT_TRUE(manager.nonceCleared());
}
void test_236_new_qr_rotates_nonce_storage() {
  Random random; algaguard::QrOnboardingManager manager{random};
  TEST_ASSERT_TRUE(manager.generate("AG-000001", 1000));
  const auto first = std::string{manager.uri()};
  manager.tick(1180);
  TEST_ASSERT_TRUE(manager.generate("AG-000001", 1181));
  TEST_ASSERT_EQUAL(algaguard::kQrInvitationUriBytes, manager.uri().size());
  TEST_ASSERT_FALSE(manager.nonceCleared());
  TEST_ASSERT_NOT_EQUAL(0, first.compare(std::string{manager.uri()}));
}
void test_237_session_cleanup_zeroizes_values() {
  algaguard::QrAuthorizedSession session{"id", "AG-000001", "secret", 1};
  session.clear();
  TEST_ASSERT_FALSE(session.available());
}
void test_238_got_ip_bootstrap_issues_and_activates_once() {
  Keys keys; Storage storage; BootstrapTransport transport;
  algaguard::QrCredentialBootstrapCoordinator coordinator{keys, storage, transport};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::QrCredentialBootstrapResult::kSuccess),
                        static_cast<int>(coordinator.run(bootstrapContext())));
  TEST_ASSERT_TRUE(keys.generated);
  TEST_ASSERT_TRUE(storage.active);
  TEST_ASSERT_TRUE(coordinator.attempted());
}
void test_239_exchange_failure_does_not_generate_key() {
  Keys keys; Storage storage; BootstrapTransport transport; transport.exchangeAllowed = false;
  algaguard::QrCredentialBootstrapCoordinator coordinator{keys, storage, transport};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::QrCredentialBootstrapResult::kExchangeRejected),
                        static_cast<int>(coordinator.run(bootstrapContext())));
  TEST_ASSERT_FALSE(keys.generated);
}
void test_240_issue_failure_destroys_local_key() {
  Keys keys; Storage storage; BootstrapTransport transport; transport.issueAllowed = false;
  algaguard::QrCredentialBootstrapCoordinator coordinator{keys, storage, transport};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::QrCredentialBootstrapResult::kIssueRejected),
                        static_cast<int>(coordinator.run(bootstrapContext())));
  TEST_ASSERT_TRUE(keys.destroyed);
  TEST_ASSERT_FALSE(storage.active);
}
void test_241_certificate_binding_mismatch_fails_closed() {
  Keys keys; Storage storage; BootstrapTransport transport; transport.wrongBinding = true;
  algaguard::QrCredentialBootstrapCoordinator coordinator{keys, storage, transport};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::QrCredentialBootstrapResult::kCertificateBindingMismatch),
                        static_cast<int>(coordinator.run(bootstrapContext())));
  TEST_ASSERT_TRUE(keys.destroyed);
}
void test_242_bootstrap_cannot_run_twice() {
  Keys keys; Storage storage; BootstrapTransport transport;
  algaguard::QrCredentialBootstrapCoordinator coordinator{keys, storage, transport};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::QrCredentialBootstrapResult::kSuccess),
                        static_cast<int>(coordinator.run(bootstrapContext())));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::QrCredentialBootstrapResult::kNoContext),
                        static_cast<int>(coordinator.run(bootstrapContext())));
}
void test_243_empty_context_is_rejected_without_network() {
  Keys keys; Storage storage; BootstrapTransport transport;
  algaguard::QrCredentialBootstrapCoordinator coordinator{keys, storage, transport};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::QrCredentialBootstrapResult::kNoContext),
                        static_cast<int>(coordinator.run({})));
  TEST_ASSERT_FALSE(coordinator.attempted());
}
void test_244_context_clear_is_idempotent() {
  auto context = bootstrapContext(); context.clear(); context.clear();
  TEST_ASSERT_FALSE(context.pending());
}
}  // namespace

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_223_disabled_by_default);
  RUN_TEST(test_224_crypto_nonce_and_compact_uri);
  RUN_TEST(test_225_invalid_device_rejected);
  RUN_TEST(test_226_rng_failure_is_error);
  RUN_TEST(test_227_lifetime_is_bounded);
  RUN_TEST(test_228_expiry_zeroizes_nonce);
  RUN_TEST(test_229_valid_binding_consumes_once);
  RUN_TEST(test_230_wrong_device_denied);
  RUN_TEST(test_231_wrong_token_denied);
  RUN_TEST(test_232_wrong_signature_denied);
  RUN_TEST(test_233_expired_grant_denied);
  RUN_TEST(test_234_malformed_grant_denied);
  RUN_TEST(test_235_clear_is_idempotent);
  RUN_TEST(test_236_new_qr_rotates_nonce_storage);
  RUN_TEST(test_237_session_cleanup_zeroizes_values);
  RUN_TEST(test_238_got_ip_bootstrap_issues_and_activates_once);
  RUN_TEST(test_239_exchange_failure_does_not_generate_key);
  RUN_TEST(test_240_issue_failure_destroys_local_key);
  RUN_TEST(test_241_certificate_binding_mismatch_fails_closed);
  RUN_TEST(test_242_bootstrap_cannot_run_twice);
  RUN_TEST(test_243_empty_context_is_rejected_without_network);
  RUN_TEST(test_244_context_clear_is_idempotent);
  return UNITY_END();
}
