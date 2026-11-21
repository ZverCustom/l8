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
void test_ap_user_is_not_interrupted_by_background_attempts() {
    Retry r; r.failed(0);
    TEST_ASSERT_EQUAL(int(Action::Wait),int(r.next(120000,true,1)));
    TEST_ASSERT_EQUAL(int(Action::Start),int(r.next(120000,true,0)));
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
RUN_TEST(test_ap_user_is_not_interrupted_by_background_attempts);
RUN_TEST(test_online_and_ip_loss); RUN_TEST(test_clock_wraparound);
RUN_TEST(test_core_first_retry_is_not_interrupted);
return UNITY_END(); }
