#include <unity.h>
#include <initializer_list>
#include <current_control.h>
using namespace l8power;
void dim_before_trip() {
    Channel c;
    for(int t=0;t<100;t+=5) c.sample(t,2.9,0.64,3,true);
    TEST_ASSERT_EQUAL(Channel::None,c.fault); TEST_ASSERT_LESS_THAN_FLOAT(.96,c.dimmer.scale);
    auto low=c.dimmer.scale; c.sample(100,0,0,3,true);
    TEST_ASSERT_LESS_THAN_FLOAT(low+.002,c.dimmer.scale);
}
void finite_retries() {
    Channel c; uint32_t now=0;
    for(int i=0;i<4;++i) {
        c.sample(now,3.5,.77,3,true); c.sample(now+100,3.5,.77,3,true); now+=100;
        TEST_ASSERT_EQUAL(Channel::Overcurrent,c.fault);
        if(i==3) { TEST_ASSERT_FALSE(c.waiting); TEST_ASSERT_FALSE(c.ready(now+999999,true,true)); break; }
        uint32_t wait=i==0?1000:i==1?5000:30000;
        TEST_ASSERT_FALSE(c.ready(now+wait-1,true,true)); TEST_ASSERT_TRUE(c.ready(now+wait,true,true)); now+=wait;
        TEST_ASSERT_EQUAL(i+1,c.retries);
    }
}
void hard_faults_do_not_retry() {
    Channel c; c.sample(0,3,1.6,3,true); TEST_ASSERT_EQUAL(Channel::Switch,c.fault); TEST_ASSERT_FALSE(c.ready(999999,true,true));
    c.reset(); c.sample(0,6,1.3,3,true); TEST_ASSERT_EQUAL(Channel::Spike,c.fault); TEST_ASSERT_FALSE(c.waiting);
    c.reset(); c.sample(0,NAN,0,3,true); TEST_ASSERT_EQUAL(Channel::InvalidSample,c.fault);
}
void interlocks_and_wrap() {
    Channel c; uint32_t t=0xffffff00;
    c.trip(Channel::Overcurrent,t,true);
    TEST_ASSERT_FALSE(c.ready(t+1000,true,false)); TEST_ASSERT_TRUE(c.ready(t+1001,true,true));
    c.trip(Channel::Overcurrent,t+1002,true); c.off(); TEST_ASSERT_FALSE(c.ready(t+9000,false,true));
    TEST_ASSERT_FALSE(c.ready(t+19000,true,true));
}
void healthy_budget_reset() {
    Channel c; c.retries=3; c.sample(0,1,.2,3,true); c.sample(59999,1,.2,3,true); TEST_ASSERT_EQUAL(3,c.retries);
    c.off(); c.sample(60000,1,.2,3,true); TEST_ASSERT_EQUAL(3,c.retries);
    c.sample(120000,1,.2,3,true); TEST_ASSERT_EQUAL(0,c.retries);
}
void resistive_load_falls_back_to_trip() {
    Channel c; for(int t=0;t<=100;t+=5)c.sample(t,3.1,.7,3,true);
    TEST_ASSERT_EQUAL(Channel::Overcurrent,c.fault); TEST_ASSERT_TRUE(c.waiting);
}
void measured_feedback_with_render_delay() {
    for(int frameMs:{25,40}) {
        Channel c; float applied=1;
        for(int t=0;t<=2000;t+=5) {
            if(t%frameMs==0) applied=c.dimmer.scale;
            c.sample(t,.2f+3.4f*applied,.8f,3,true);
            TEST_ASSERT_EQUAL(Channel::None,c.fault);
        }
        TEST_ASSERT_LESS_THAN_FLOAT(3,.2f+3.4f*applied);
        TEST_ASSERT_GREATER_THAN_FLOAT(.5,c.dimmer.scale);
    }
}
int main(){ UNITY_BEGIN(); RUN_TEST(dim_before_trip); RUN_TEST(measured_feedback_with_render_delay); RUN_TEST(finite_retries); RUN_TEST(hard_faults_do_not_retry); RUN_TEST(interlocks_and_wrap); RUN_TEST(healthy_budget_reset); RUN_TEST(resistive_load_falls_back_to_trip); return UNITY_END(); }
