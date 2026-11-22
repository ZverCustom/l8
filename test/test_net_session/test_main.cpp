#include <unity.h>
#include <initializer_list>
#include <net_session.h>
void deadlines() {
    l8net::Session s; s.enter(s.Scan,0xfffffff0); TEST_ASSERT_FALSE(s.expired(0xfffffff0+4999u)); TEST_ASSERT_TRUE(s.expired(0xfffffff0+5000u));
    s.enter(s.Associating,100); TEST_ASSERT_FALSE(s.expired(10100)); TEST_ASSERT_TRUE(s.expired(45100));
    s.enter(s.Dhcp,44000); s.enter(s.Dhcp,50000); TEST_ASSERT_TRUE(s.expired(104000));
}
void no_background_attempts() {
    l8net::Session s;
    for(auto phase:{s.Idle,s.Online,s.Stopping}) { s.enter(phase,0); TEST_ASSERT_FALSE(s.expired(999999)); }
}
void finite_boot_attempts() {
    l8net::BootAttempts b;TEST_ASSERT_FALSE(b.retryAllowed());b.discovered();
    for(int i=1;i<=3;++i){b.started();TEST_ASSERT_EQUAL(i,b.attempts);TEST_ASSERT_EQUAL(i<3,b.retryAllowed());}
    b.started();TEST_ASSERT_EQUAL(3,b.attempts);TEST_ASSERT_FALSE(b.retryAllowed());
}
void client_or_connection_ends_boot_budget() {
    l8net::BootAttempts b;b.discovered();b.started();TEST_ASSERT_TRUE(b.retryAllowed());
    b.observeClients(true);b.observeClients(false);TEST_ASSERT_FALSE(b.retryAllowed());
    b.discovered();b.started();b.stop();TEST_ASSERT_FALSE(b.retryAllowed()); // online or manual reconnect
}
int main(){ UNITY_BEGIN(); RUN_TEST(deadlines); RUN_TEST(no_background_attempts); RUN_TEST(finite_boot_attempts); RUN_TEST(client_or_connection_ends_boot_budget); return UNITY_END(); }
