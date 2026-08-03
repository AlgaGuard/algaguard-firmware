#include <unity.h>

#include "algaguard/config.hpp"
#include "algaguard/display.hpp"
#include "algaguard/startup.hpp"
#include "algaguard/storage.hpp"

void setUp() {}
void tearDown() {}

namespace {
class FakeStartupServices final : public algaguard::StartupServices {
 public:
  algaguard::StartupResult platform{algaguard::OperationStatus::kSuccess};
  algaguard::StartupResult storage{algaguard::OperationStatus::kSuccess};
  algaguard::StartupResult display{algaguard::OperationStatus::kSuccess};
  algaguard::StartupResult input{algaguard::OperationStatus::kSuccess};
  algaguard::StartupResult provisioning{algaguard::OperationStatus::kSuccess};
  algaguard::StartupResult ble{algaguard::OperationStatus::kNotImplemented};
  algaguard::StartupResult wifi{algaguard::OperationStatus::kSuccess};
  algaguard::StartupResult time{algaguard::OperationStatus::kSuccess};
  algaguard::StartupResult credentials{algaguard::OperationStatus::kSuccess};
  algaguard::StartupResult bootstrap{algaguard::OperationStatus::kNotImplemented};
  algaguard::StartupResult mqtt{algaguard::OperationStatus::kSuccess};
  algaguard::StartupResult ota{algaguard::OperationStatus::kSuccess};
  algaguard::StartupResult reset{algaguard::OperationStatus::kSuccess};

  algaguard::StartupResult platform_init() override { return platform; }
  algaguard::StartupResult storage_init() override { return storage; }
  algaguard::StartupResult display_init() override { return display; }
  algaguard::StartupResult input_init() override { return input; }
  algaguard::StartupResult load_provisioning_state() override {
    return provisioning;
  }
  algaguard::StartupResult start_ble_provisioning() override { return ble; }
  algaguard::StartupResult poll_wifi() override { return wifi; }
  algaguard::StartupResult poll_time_sync() override { return time; }
  algaguard::StartupResult check_credentials() override { return credentials; }
  algaguard::StartupResult poll_credential_bootstrap() override {
    return bootstrap;
  }
  algaguard::StartupResult poll_mqtt() override { return mqtt; }
  algaguard::StartupResult validate_pending_ota() override { return ota; }
  algaguard::StartupResult controlled_reset() override { return reset; }
};

void initialize(algaguard::StartupStateMachine& machine) {
  for (int count = 0; count < 5; ++count) machine.tick();
}
}  // namespace

void test_development_configuration_is_typed_and_valid() {
  const auto& config = algaguard::active_firmware_config();
  TEST_ASSERT_EQUAL(
      static_cast<int>(algaguard::ConfigError::kNone),
      static_cast<int>(algaguard::validate_firmware_config(config)));
  TEST_ASSERT_EQUAL_STRING("development", config.environment_id.data());
  TEST_ASSERT_EQUAL_UINT16(8883, config.mqtt_tls_port);
}

void test_configuration_rejects_missing_invalid_and_secret_values() {
  auto config = algaguard::kDevelopmentConfig;
  config.bootstrap_api_url = "";
  TEST_ASSERT_EQUAL(
      static_cast<int>(algaguard::ConfigError::kInvalidBootstrapUrl),
      static_cast<int>(algaguard::validate_firmware_config(config)));
  config = algaguard::kDevelopmentConfig;
  config.mqtt_host = "";
  TEST_ASSERT_EQUAL(
      static_cast<int>(algaguard::ConfigError::kInvalidMqttHost),
      static_cast<int>(algaguard::validate_firmware_config(config)));
  config = algaguard::kDevelopmentConfig;
  config.mqtt_tls_port = 0;
  TEST_ASSERT_EQUAL(
      static_cast<int>(algaguard::ConfigError::kInvalidMqttPort),
      static_cast<int>(algaguard::validate_firmware_config(config)));
  config = algaguard::kDevelopmentConfig;
  config.device_id = "invalid";
  TEST_ASSERT_EQUAL(
      static_cast<int>(algaguard::ConfigError::kInvalidDeviceId),
      static_cast<int>(algaguard::validate_firmware_config(config)));
  config = algaguard::kDevelopmentConfig;
  config.environment = static_cast<algaguard::FirmwareEnvironment>(99);
  TEST_ASSERT_EQUAL(
      static_cast<int>(algaguard::ConfigError::kInvalidEnvironment),
      static_cast<int>(algaguard::validate_firmware_config(config)));
  TEST_ASSERT_EQUAL(
      static_cast<int>(algaguard::ConfigError::kForbiddenSecretField),
      static_cast<int>(algaguard::validate_firmware_config(
          algaguard::kDevelopmentConfig, {"wifi_password"})));
  config = algaguard::kDevelopmentConfig;
  config.public_trust_anchor_id = "-----BEGIN PRIVATE KEY-----";
  TEST_ASSERT_EQUAL(
      static_cast<int>(algaguard::ConfigError::kPrivateMaterialPresent),
      static_cast<int>(algaguard::validate_firmware_config(config)));
}

void test_storage_fake_rejects_partial_and_private_material() {
  algaguard::FakeFirmwareStorage storage;
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::StorageStatus::kOk),
                    static_cast<int>(storage.initialize()));
  algaguard::StorageRecord record;
  record.provisioning_state = algaguard::ProvisioningState::kProvisioned;
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::StorageStatus::kPartial),
                    static_cast<int>(storage.save(record)));
  record.device_uuid = "10000000-0000-4000-8000-000000000001";
  record.certificate_pem = "-----BEGIN CERTIFICATE-----PUBLIC";
  record.ca_chain_pem = {"-----BEGIN CERTIFICATE-----CA"};
  record.private_key_handle = algaguard::PrivateKeyHandle{7, 1};
  record.active_firmware_version = "0.2.0";
  record.unacknowledged_telemetry =
      algaguard::TelemetryAckMetadata{1, 2, 2};
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::StorageStatus::kOk),
                    static_cast<int>(storage.save(record)));
  TEST_ASSERT_TRUE(storage.load().record.has_value());
  record.certificate_pem = "-----BEGIN PRIVATE KEY-----";
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::StorageStatus::kCorrupt),
                    static_cast<int>(storage.save(record)));
}

void test_reset_requires_explicit_two_step_confirmation() {
  algaguard::ResetConfirmation reset;
  TEST_ASSERT_FALSE(reset.confirm(true));
  reset.request();
  TEST_ASSERT_FALSE(reset.confirm(false));
  TEST_ASSERT_TRUE(reset.confirm(true));
  TEST_ASSERT_TRUE(reset.confirmed());
}

void test_fresh_and_provisioned_boot_transitions() {
  FakeStartupServices fresh_services;
  algaguard::StartupStateMachine fresh{fresh_services};
  initialize(fresh);
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::StartupState::kUnprovisioned),
                    static_cast<int>(fresh.state()));

  FakeStartupServices provisioned_services;
  provisioned_services.provisioning.provisioned = true;
  algaguard::StartupStateMachine provisioned{provisioned_services};
  initialize(provisioned);
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::StartupState::kWifiConnecting),
                    static_cast<int>(provisioned.state()));
  provisioned.tick();
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::StartupState::kTimeSync),
                    static_cast<int>(provisioned.state()));
  provisioned.tick();
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::StartupState::kCredentialCheck),
                    static_cast<int>(provisioned.state()));
}

void test_credentials_mqtt_and_online_transitions() {
  FakeStartupServices services;
  services.provisioning.provisioned = true;
  services.credentials.credential_present = true;
  algaguard::StartupStateMachine machine{services};
  initialize(machine);
  machine.tick();
  machine.tick();
  machine.tick();
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::StartupState::kMqttConnecting),
                    static_cast<int>(machine.state()));
  machine.tick();
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::StartupState::kOnline),
                    static_cast<int>(machine.state()));

  FakeStartupServices missing_services;
  missing_services.provisioning.provisioned = true;
  missing_services.credentials.status = algaguard::OperationStatus::kMissing;
  algaguard::StartupStateMachine missing{missing_services};
  initialize(missing);
  missing.tick();
  missing.tick();
  missing.tick();
  TEST_ASSERT_EQUAL(
      static_cast<int>(algaguard::StartupState::kCredentialBootstrap),
      static_cast<int>(missing.state()));
}

void test_ota_degraded_fault_and_controlled_reset_transitions() {
  FakeStartupServices ota_services;
  ota_services.provisioning.ota_pending = true;
  algaguard::StartupStateMachine ota{ota_services};
  initialize(ota);
  TEST_ASSERT_EQUAL(
      static_cast<int>(algaguard::StartupState::kOtaPendingValidation),
      static_cast<int>(ota.state()));

  FakeStartupServices degraded_services;
  degraded_services.platform.status =
      algaguard::OperationStatus::kRecoverableFailure;
  algaguard::StartupStateMachine degraded{degraded_services};
  degraded.tick();
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::StartupState::kDegraded),
                    static_cast<int>(degraded.state()));

  FakeStartupServices fault_services;
  fault_services.platform.status = algaguard::OperationStatus::kFatalFailure;
  algaguard::StartupStateMachine fault{fault_services};
  fault.tick();
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::StartupState::kFault),
                    static_cast<int>(fault.state()));
  TEST_ASSERT_FALSE(fault.confirmed_reset(false));
  TEST_ASSERT_TRUE(fault.confirmed_reset(true));
  TEST_ASSERT_EQUAL(static_cast<int>(algaguard::StartupState::kControlledReset),
                    static_cast<int>(fault.state()));
}

void test_display_and_led_hooks_are_safe_and_deterministic() {
  const auto boot = algaguard::boot_screen(algaguard::kDevelopmentConfig);
  const auto hardware = algaguard::hardware_screen(16U * 1024U * 1024U,
                                                   8U * 1024U * 1024U, true);
  const auto error = algaguard::state_screen(
      algaguard::StartupState::kFault,
      algaguard::StartupReason::kBoardProfileMismatch);
  TEST_ASSERT_TRUE(boot.safe());
  TEST_ASSERT_TRUE(hardware.safe());
  TEST_ASSERT_TRUE(error.safe());
  const auto online =
      algaguard::startup_led_state(algaguard::StartupState::kOnline);
  TEST_ASSERT_TRUE(online.green);
  const auto provisioning =
      algaguard::startup_led_state(algaguard::StartupState::kBleProvisioning);
  TEST_ASSERT_TRUE(provisioning.blue);
  const auto fault =
      algaguard::startup_led_state(algaguard::StartupState::kFault, true);
  TEST_ASSERT_TRUE(fault.red);
  TEST_ASSERT_FALSE(fault.blue);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_development_configuration_is_typed_and_valid);
  RUN_TEST(test_configuration_rejects_missing_invalid_and_secret_values);
  RUN_TEST(test_storage_fake_rejects_partial_and_private_material);
  RUN_TEST(test_reset_requires_explicit_two_step_confirmation);
  RUN_TEST(test_fresh_and_provisioned_boot_transitions);
  RUN_TEST(test_credentials_mqtt_and_online_transitions);
  RUN_TEST(test_ota_degraded_fault_and_controlled_reset_transitions);
  RUN_TEST(test_display_and_led_hooks_are_safe_and_deterministic);
  return UNITY_END();
}
