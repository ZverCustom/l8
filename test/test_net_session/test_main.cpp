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
void activity_wrap(){l8net::HttpActivity a;a.touch(0xfffffff0);TEST_ASSERT_TRUE(a.active(0xfffffff0+59999u));TEST_ASSERT_FALSE(a.active(0xfffffff0+60000u));}
int main(){ UNITY_BEGIN(); RUN_TEST(deadlines); RUN_TEST(no_background_attempts); RUN_TEST(activity_wrap); return UNITY_END(); }
