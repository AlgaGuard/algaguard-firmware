#define ALGAGUARD_DEVELOPMENT_BUILD 1
#define ALGAGUARD_DEVELOPMENT_PHYSICAL_TEST_OPT_IN 1
#define ALGAGUARD_PHYSICAL_TEST_MODE 1
#define ALGAGUARD_WIFI_CREDENTIAL_PERSISTENCE_DISABLED 1

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <unity.h>
#include "algaguard/ble_provisioning_gatt_controller.hpp"
#include "algaguard/physical_provisioning_runtime_bridge.hpp"
#include "algaguard/physical_test_harness.hpp"

namespace {
constexpr const char* kSession = "50000000-0000-4000-8000-000000000001";
constexpr const char* kDevice = "AG-000001";
constexpr const char* kToken = "sE2vR8yN5kM1pQ7xT4bW9dF6aC3uH0zL";
class Adapter final : public algaguard::WifiConnectionAdapter { public:
 bool beginConnect(std::string_view, std::string_view) override { ++starts; return true; } void cancelConnect() override {} void disconnect() override {} bool isConnectInProgress() const override{return false;} void clearSensitiveDriverInput() override{} unsigned starts{}; };
class Transport { public:
 bool installDevelopmentSession(std::string_view,std::string_view,std::string_view,std::uint64_t){return true;}
 algaguard::BleWifiCredentialHandoff takeAcceptedWifiCredentials(){return std::move(handoff);} algaguard::BleWifiCredentialHandoff handoff; };
std::uint32_t crc(const std::uint8_t* b,std::size_t n){std::uint32_t v=0xffffffffU;for(std::size_t i=0;i<n;++i){v^=b[i];for(unsigned j=0;j<8;++j)v=(v>>1U)^(0xedb88320U&(0U-(v&1U)));}return~v;}
std::array<std::uint8_t,20> armFrame(std::uint64_t ticks){std::array<std::uint8_t,20>b{};std::memcpy(b.data(),algaguard::kPhysicalSessionMagic.data(),4);b[4]=1;b[5]=4;b[7]=8;for(int i=7;i>=0;--i)b[8+(7-i)]=static_cast<std::uint8_t>(ticks>>(i*8));auto c=crc(b.data(),16);for(int i=0;i<4;++i)b[16+i]=static_cast<std::uint8_t>(c>>((3-i)*8));return b;}
algaguard::BleWifiCredentialHandoff handoff(){algaguard::BleProvisioningGattController c;TEST_ASSERT_TRUE(c.installDevelopmentSession(kSession,kDevice,kToken,200));TEST_ASSERT_TRUE(c.onConnected(1).accepted);std::string p=std::string{"{\"schema\":\"urn:algaguard:schema:onboarding:ble-provisioning-request:v1\",\"schemaVersion\":\"1.0.0\",\"sessionId\":\""}+kSession+"\",\"deviceId\":\""+kDevice+"\",\"sessionToken\":\""+kToken+"\",\"ssid\":\"TestNet\",\"password\":\"synthetic-password\"}";for(std::uint16_t i=0;i<2;++i){auto off=i?180U:0U;auto size=i?p.size()-180U:180U;algaguard::BleProvisioningFrame f;f.messageId=1;f.fragmentIndex=i;f.fragmentCount=2;TEST_ASSERT_TRUE(f.setPayload(reinterpret_cast<const std::uint8_t*>(p.data()+off),size));algaguard::BleProvisioningEncodedFrame e;TEST_ASSERT_TRUE(algaguard::encode_ble_provisioning_frame(f,e));TEST_ASSERT_TRUE(c.onRequestWrite(e.bytes.data(),e.size,10+i).accepted);}return c.takeAcceptedWifiCredentials();}
void resetGate(){algaguard::physical_wifi_connect_gate.reset();}
}
void setUp(){resetGate();} void tearDown(){resetGate();}
void test_217_gate_disabled_by_default_and_physical_only(){TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::PhysicalWifiConnectGateState::kDisabled),static_cast<int>(algaguard::physical_wifi_connect_gate.state()));TEST_ASSERT_FALSE(algaguard::physical_test_profile_allowed(true,false,true,true));}
void test_218_arm_once_and_duplicate_rejected(){Transport t;algaguard::PhysicalTestSessionInstaller i;algaguard::PhysicalSessionControlProtocol p;auto f=armFrame(100);TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::PhysicalSessionControlAck::kWifiTestArmed),static_cast<int>(p.ingest(t,i,f.data(),f.size(),1)));TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::PhysicalSessionControlAck::kWifiTestAlreadyActive),static_cast<int>(p.ingest(t,i,f.data(),f.size(),2)));}
void test_219_expired_cleared_and_reset_gate_cannot_authorize(){Transport t;algaguard::PhysicalTestSessionInstaller i;algaguard::PhysicalSessionControlProtocol p;auto f=armFrame(1);p.ingest(t,i,f.data(),f.size(),1);TEST_ASSERT_TRUE(algaguard::physical_wifi_connect_gate.expire(2));TEST_ASSERT_FALSE(algaguard::physical_wifi_connect_gate.consume(2));algaguard::physical_wifi_connect_gate.clear();TEST_ASSERT_FALSE(algaguard::physical_wifi_connect_gate.consume(3));}
void test_220_accepted_handoff_without_gate_does_not_start(){Adapter a;algaguard::WifiConnectionRuntime r{a};Transport t;t.handoff=handoff();algaguard::PhysicalTestSessionInstaller i;algaguard::PhysicalProvisioningRuntimeBridge b;TEST_ASSERT_TRUE(b.onAccepted(t,r,i,10));TEST_ASSERT_EQUAL(0,a.starts);}
void test_221_armed_gate_authorizes_one_start_and_consumes(){Adapter a;algaguard::WifiConnectionRuntime r{a};Transport t;t.handoff=handoff();algaguard::PhysicalTestSessionInstaller i;algaguard::PhysicalProvisioningRuntimeBridge b;TEST_ASSERT_TRUE(algaguard::physical_wifi_connect_gate.arm(1,100,false,false));TEST_ASSERT_TRUE(b.onAccepted(t,r,i,2));TEST_ASSERT_EQUAL(1,a.starts);TEST_ASSERT_EQUAL_INT(static_cast<int>(algaguard::PhysicalWifiConnectGateState::kConsumed),static_cast<int>(algaguard::physical_wifi_connect_gate.state()));TEST_ASSERT_FALSE(b.onAccepted(t,r,i,3));TEST_ASSERT_EQUAL(1,a.starts);}
void test_222_safe_gate_states_and_ui_have_no_secrets(){for(auto s:{algaguard::PhysicalWifiConnectGateState::kDisabled,algaguard::PhysicalWifiConnectGateState::kArmed,algaguard::PhysicalWifiConnectGateState::kExpired})TEST_ASSERT_TRUE(algaguard::physical_text_is_safe(algaguard::physical_wifi_gate_state_code(s)));TEST_ASSERT_TRUE(algaguard::physical_screen_is_safe(algaguard::physical_test_screen(algaguard::PhysicalTestState::kBleAdvertisingActive)));}
int main(int,char**){UNITY_BEGIN();RUN_TEST(test_217_gate_disabled_by_default_and_physical_only);RUN_TEST(test_218_arm_once_and_duplicate_rejected);RUN_TEST(test_219_expired_cleared_and_reset_gate_cannot_authorize);RUN_TEST(test_220_accepted_handoff_without_gate_does_not_start);RUN_TEST(test_221_armed_gate_authorizes_one_start_and_consumes);RUN_TEST(test_222_safe_gate_states_and_ui_have_no_secrets);return UNITY_END();}
