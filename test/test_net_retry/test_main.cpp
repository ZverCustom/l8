#include <unity.h>
#include <net_retry.h>
using namespace l8net;
void setUp() {} void tearDown() {}
void test_slow_association_is_not_restarted_at_retry_interval() {
    Retry r; r.started(2000);
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(12000,false,0)));
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(32000,true,0)));
    TEST_ASSERT_EQUAL(int(Action::Cancel),int(r.next(47000,true,0)));
    r.stopping(47000);
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(90000,true,0)));
}
void test_dhcp_has_its_own_deadline() {
    Retry r; r.started(2000); r.associated(6000);
    r.associated(30000); // repeated status observations must not move the deadline
    TEST_ASSERT_EQUAL(6000,r.since);
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(65000,true,0)));
    TEST_ASSERT_EQUAL(int(Action::Cancel),int(r.next(66000,true,0)));
}
void test_failure_backoff_depends_on_ap() {
    Retry r; r.started(2000); r.failed(5000);
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(14999,false,0)));
    TEST_ASSERT_EQUAL(int(Action::Start),int(r.next(15000,false,0)));
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(15000,true,0)));
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(34999,true,0)));
    TEST_ASSERT_EQUAL(int(Action::Start),int(r.next(35000,true,0)));
}
void test_ap_activity_expires_even_if_phone_stays_associated() {
    Retry r; r.failed(0); HttpActivity activity;
    // An associated phone with no HTTP activity never inhibits retries.
    TEST_ASSERT_EQUAL(int(Action::Start),int(r.next(120000,true,activity.active(120000))));
    activity.touch(120000);
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(179999,true,activity.active(179999))));
    activity.touch(179999); // a real AP request refreshes the editing window
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(239998,true,activity.active(239998))));
    TEST_ASSERT_EQUAL(int(Action::Start),int(r.next(239999,true,activity.active(239999))));
    // The same window controls AP shutdown after STA succeeds.
    TEST_ASSERT_FALSE(activity.active(240000));
}
void test_http_activity_wrap_and_zero_timestamp() {
    HttpActivity a; a.touch(0);
    TEST_ASSERT_TRUE(a.active(59999)); TEST_ASSERT_FALSE(a.active(60000));
    a.touch(0xfffffff0u);
    TEST_ASSERT_TRUE(a.active(59983)); TEST_ASSERT_FALSE(a.active(59984));
    TEST_ASSERT_FALSE(a.active(0xfffffff0u)); // expired activity cannot revive after a wrap
}
void test_missing_disconnect_event_recovers_online_phase() {
    Retry r; r.online(1000);
    r.reconcileOffline(5000,false);
    TEST_ASSERT_EQUAL(int(Phase::Failed),int(r.phase));
    r.reconcileOffline(10000,false); // don't postpone the retry on each poll
    TEST_ASSERT_EQUAL(int(Action::Start),int(r.next(15000,false,false)));
}
void test_missing_ip_event_keeps_association_alive() {
    Retry r; r.online(1000); r.reconcileOffline(5000,true);
    TEST_ASSERT_EQUAL(int(Phase::Address),int(r.phase));
    r.reconcileOffline(15000,true);
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(34999,true,false)));
    TEST_ASSERT_EQUAL(5000,r.since);
}
void test_online_and_ip_loss() {
    Retry r; r.started(100); r.associated(500); r.online(1000);
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(100000,true,0)));
    r.associated(100000); // lost IP with association retained
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(159999,false,0)));
    TEST_ASSERT_EQUAL(int(Action::Cancel),int(r.next(160000,false,0)));
}
void test_clock_wraparound() {
    Retry r; r.failed(0xfffffff0u);
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(9983,false,0)));
    TEST_ASSERT_EQUAL(int(Action::Start),int(r.next(9984,false,0)));
}
void test_core_first_retry_is_not_interrupted() {
    Retry r; r.started(2000); r.disconnected(4000,true);
    TEST_ASSERT_TRUE(r.coreRetryAvailable); // explicit disconnect consumes no core retry
    r.started(5000); r.disconnected(8000,false);
    TEST_ASSERT_EQUAL(int(Phase::Connecting),int(r.phase));
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(18000,false,0)));
    r.disconnected(20000,false); // the core retry also failed
    TEST_ASSERT_EQUAL(int(Action::Start),int(r.next(30000,false,0)));
}
int main() { UNITY_BEGIN();
RUN_TEST(test_slow_association_is_not_restarted_at_retry_interval);
RUN_TEST(test_dhcp_has_its_own_deadline);
RUN_TEST(test_failure_backoff_depends_on_ap);
RUN_TEST(test_ap_activity_expires_even_if_phone_stays_associated);
RUN_TEST(test_http_activity_wrap_and_zero_timestamp);
RUN_TEST(test_missing_disconnect_event_recovers_online_phase);
RUN_TEST(test_missing_ip_event_keeps_association_alive);
RUN_TEST(test_online_and_ip_loss); RUN_TEST(test_clock_wraparound);
RUN_TEST(test_core_first_retry_is_not_interrupted);
return UNITY_END(); }
