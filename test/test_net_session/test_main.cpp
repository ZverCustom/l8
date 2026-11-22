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
void disconnect_failure_is_fast_after_core_retry() {
    l8net::CoreRetry core;l8net::Session s;s.enter(s.Associating,100);
    s.disconnected(2000,false,core.onDisconnect(false));
    TEST_ASSERT_EQUAL(s.Associating,s.phase);TEST_ASSERT_FALSE(s.expired(2100));
    s.disconnected(2010,true,core.onDisconnect(true)); // internal disconnect()/begin()
    TEST_ASSERT_EQUAL(s.Associating,s.phase);TEST_ASSERT_EQUAL(2000,s.since);
    s.disconnected(3000,false,core.onDisconnect(false));
    TEST_ASSERT_EQUAL(s.Failed,s.phase);TEST_ASSERT_EQUAL(3000,s.since);
    // New explicit begin does not replenish Arduino's process-wide first retry.
    s.stopping(3000);TEST_ASSERT_FALSE(s.restartReady(4999));TEST_ASSERT_TRUE(s.restartReady(5000));s.enter(s.Associating,5000);
    s.disconnected(6000,false,core.onDisconnect(false));TEST_ASSERT_EQUAL(s.Failed,s.phase);
}
void dhcp_failure_and_voluntary_disconnect() {
    l8net::CoreRetry core;l8net::Session s;s.enter(s.Dhcp,100);
    s.disconnected(200,true,core.onDisconnect(true));TEST_ASSERT_EQUAL(s.Dhcp,s.phase);TEST_ASSERT_TRUE(core.available);
    s.disconnected(300,false,core.onDisconnect(false));TEST_ASSERT_EQUAL(s.Associating,s.phase);
    s.enter(s.Dhcp,400);s.disconnected(500,false,core.onDisconnect(false));TEST_ASSERT_EQUAL(s.Failed,s.phase);
    s.enter(s.Stopping,600);s.disconnected(601,true,core.onDisconnect(true));TEST_ASSERT_EQUAL(s.Stopping,s.phase);
}
void dropped_event_cannot_replenish_core_retry() {
    l8net::CoreRetry core;TEST_ASSERT_TRUE(core.onDisconnect(false)); // first event lost AFTER annotation
    l8net::Session s;s.enter(s.Associating,100);
    s.disconnected(200,false,core.onDisconnect(false));TEST_ASSERT_EQUAL(s.Failed,s.phase);
}
void core_retry_timeout_wrap() {
    l8net::CoreRetry core;l8net::Session s;s.enter(s.Dhcp,0xffffff00u);
    uint32_t t=0xfffffff0u;s.disconnected(t,false,core.onDisconnect(false));
    TEST_ASSERT_FALSE(s.expired(t+44999));TEST_ASSERT_TRUE(s.expired(t+45000));
}
void disconnect_settle_restarts_and_wraps() {
    l8net::Session s;uint32_t t=0xfffffff0u;
    s.stopping(t);TEST_ASSERT_FALSE(s.restartReady(t+1999));TEST_ASSERT_TRUE(s.restartReady(t+2000));
    s.stopping(t+2000);TEST_ASSERT_FALSE(s.restartReady(t+3999));TEST_ASSERT_TRUE(s.restartReady(t+4000));
}
int main(){ UNITY_BEGIN(); RUN_TEST(disconnect_failure_is_fast_after_core_retry); RUN_TEST(dhcp_failure_and_voluntary_disconnect); RUN_TEST(dropped_event_cannot_replenish_core_retry); RUN_TEST(core_retry_timeout_wrap); RUN_TEST(disconnect_settle_restarts_and_wraps); RUN_TEST(deadlines); RUN_TEST(no_background_attempts); RUN_TEST(finite_boot_attempts); RUN_TEST(client_or_connection_ends_boot_budget); return UNITY_END(); }
