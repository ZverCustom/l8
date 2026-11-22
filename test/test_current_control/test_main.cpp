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
    Channel c; c.enabled(0); c.sample(10,8,1.6,3,true); c.sample(59,8,1.6,3,true); TEST_ASSERT_EQUAL(Channel::None,c.fault); c.sample(60,8,1.6,3,true); TEST_ASSERT_EQUAL(Channel::Switch,c.fault); TEST_ASSERT_FALSE(c.ready(999999,true,true));
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
void startup_and_every_retry_allow_inrush() {
    Channel c; uint32_t on=100;
    for(int attempt=0;attempt<4;++attempt) {
        c.enabled(on);
        for(uint32_t t=0;t<10;++t) { c.sample(on+t,8,t&1?1.4:1.7,3,true); TEST_ASSERT_EQUAL(Channel::None,c.fault); }
        c.sample(on+10,1,.22,3,true); TEST_ASSERT_EQUAL(Channel::None,c.fault);
        // Ordinary overload still times out and consumes one bounded retry.
        c.sample(on+20,3.5,.77,3,true);c.sample(on+120,3.5,.77,3,true);
        TEST_ASSERT_EQUAL(Channel::Overcurrent,c.fault);
        if(attempt<3) { on+=120+c.delayMs();TEST_ASSERT_TRUE(c.ready(on,true,true));TEST_ASSERT_EQUAL(attempt+1,c.retries); }
        else { TEST_ASSERT_FALSE(c.waiting);TEST_ASSERT_FALSE(c.ready(on+999999,true,true)); }
    }
}
void switch_noise_resets_debounce_without_current_trip() {
    Channel c;c.enabled(0);
    c.sample(10,8,1.7,3,true);c.sample(59,8,1.7,3,true);TEST_ASSERT_EQUAL(Channel::None,c.fault);
    c.sample(60,1,.22,3,true);c.sample(70,8,1.7,3,true);c.sample(119,8,1.7,3,true);TEST_ASSERT_EQUAL(Channel::None,c.fault);
    c.sample(120,8,1.7,3,true);TEST_ASSERT_EQUAL(Channel::Switch,c.fault);TEST_ASSERT_FALSE(c.waiting);
}
void startup_boundary_wrap_and_manual_cycles() {
    Channel c;
    for(uint32_t n=0;n<30;++n) { uint32_t t=0xfffffff0u+n*100u;c.off();c.enabled(t);c.sample(t+9,8,1.4,3,true);c.sample(t+10,1,.22,3,true);TEST_ASSERT_EQUAL(Channel::None,c.fault); }
    c.off();c.enabled(0xfffffffbu);c.sample(4,8,1.4,3,true);TEST_ASSERT_EQUAL(Channel::None,c.fault);
    c.sample(5,8,1.4,3,true);TEST_ASSERT_EQUAL(Channel::None,c.fault);
    c.sample(105,8,1.4,3,true);TEST_ASSERT_EQUAL(Channel::Overcurrent,c.fault);TEST_ASSERT_TRUE(c.waiting);
    c.reset();c.enabled(0);c.sample(0,NAN,0,3,true);TEST_ASSERT_EQUAL(Channel::InvalidSample,c.fault);
}
void actual_channel_protections_are_independent() {
    Channel channels[8];
    for(auto& c:channels)c.enabled(0);
    for(int i=0;i<8;++i){channels[i].sample(10,i==6?6:1,.3,5,true);channels[i].sample(110,i==6?6:1,.3,5,true);TEST_ASSERT_EQUAL(i==6?Channel::Overcurrent:Channel::None,channels[i].fault);}
    channels[6].reset();TEST_ASSERT_EQUAL(Channel::None,channels[6].fault);
}
void assert_flashes_regulate(float limit,float demand,uint32_t settledAt=200) {
    for(int frameMs:{16,25,40}) for(int offset=0;offset<frameMs;++offset) {
        Channel c;c.enabled(0);float amps=.1f;
        for(uint32_t t=0;t<=1600;++t) {
            if((t+offset)%frameMs==0) {
                bool white=t>=100 && (t<800 || (t/100)%2==0);
                amps=.1f+(white?(demand-.1f)*c.dimmer.scale:0);
            }
            if(t%5==0) {
                c.sample(t,amps,amps*1200.f/5450.f,limit,true);
                TEST_ASSERT_EQUAL(Channel::None,c.fault);TEST_ASSERT_EQUAL(0,c.retries);
                if(t>=settledAt && t<800) TEST_ASSERT_LESS_THAN_FLOAT(limit,amps);
            }
        }
        TEST_ASSERT_GREATER_THAN_FLOAT(0,c.dimmer.scale);TEST_ASSERT_LESS_THAN_FLOAT(1,c.dimmer.scale);
    }
}
void black_to_white_and_flashes_regulate_without_trip() {
    for(float demand:{6.f,6.2f,7.f}) assert_flashes_regulate(3,demand);
}
void flashes_above_switch_threshold_regulate_without_trip() {
    // One frame first clears Switch, then normal feedback settles below limit.
    // Assert no fault at EVERY sample through both deadlines, including settling.
    for(float demand:{1.8f*5450.f/1200.f,10.f}) assert_flashes_regulate(5,demand,300);
}
void switch_window_flash_recovers() {
    Channel c;c.enabled(0);
    c.sample(10,1.8f*5450.f/1200.f,1.8,5,true);
    TEST_ASSERT_EQUAL(Channel::None,c.fault);TEST_ASSERT_LESS_THAN_FLOAT(.7,c.dimmer.scale);
    float reduced=c.dimmer.scale;
    c.sample(49,1.8f*5450.f/1200.f,1.8,5,true);
    TEST_ASSERT_FLOAT_WITHIN(.00001,reduced,c.dimmer.scale); // one reduction per window
    c.sample(50,1.2f*5450.f/1200.f,1.2,5,true); // next frame clears Switch
    TEST_ASSERT_FALSE(c.switchHigh);TEST_ASSERT_EQUAL(Channel::None,c.fault);
    c.sample(90,4.5,.991,5,true);
    c.sample(200,4.5,.991,5,true);
    TEST_ASSERT_TRUE(c.ready(200,true,true));TEST_ASSERT_EQUAL(0,c.retries);
}
void persistent_switch_fault_still_latches_at_50ms() {
    Channel c;uint32_t t=0xfffffff0u;c.enabled(t);
    c.sample(t+9,14,3.1,5,true);TEST_ASSERT_EQUAL_FLOAT(1,c.dimmer.scale);
    c.sample(t+10,14,3.1,5,true);float reduced=c.dimmer.scale;
    TEST_ASSERT_LESS_THAN_FLOAT(1,reduced);
    for(uint32_t dt=11;dt<60;++dt) {
        c.sample(t+dt,14,3.1,5,true);TEST_ASSERT_EQUAL(Channel::None,c.fault);
        TEST_ASSERT_FLOAT_WITHIN(.00001,reduced,c.dimmer.scale);
    }
    c.sample(t+60,14,3.1,5,true);TEST_ASSERT_EQUAL(Channel::Switch,c.fault);
    TEST_ASSERT_FALSE(c.waiting);TEST_ASSERT_FALSE(c.ready(t+999999,true,true));
}
void switch_reduction_rearms_after_low_sample_and_enable() {
    Channel c;c.enabled(0);c.sample(10,8.175,1.8,5,true);float first=c.dimmer.scale;
    c.sample(20,4.5,.991,5,true);c.sample(30,8.175,1.8,5,true);
    TEST_ASSERT_LESS_THAN_FLOAT(first,c.dimmer.scale);float second=c.dimmer.scale;
    c.off();c.enabled(40);c.sample(49,8.175,1.8,5,true);
    TEST_ASSERT_FLOAT_WITHIN(.00001,second,c.dimmer.scale);
    c.sample(50,8.175,1.8,5,true);TEST_ASSERT_LESS_THAN_FLOAT(second,c.dimmer.scale);
    TEST_ASSERT_EQUAL(Channel::None,c.fault);
}
void large_uncontrollable_load_uses_finite_overload_retries() {
    Channel c;uint32_t now=0;
    for(int attempt=0;attempt<4;++attempt) {
        c.enabled(now);now+=10;
        for(int t=0;t<100;t+=5){c.sample(now+t,6,1.32,3,true);TEST_ASSERT_EQUAL(Channel::None,c.fault);}
        now+=100;c.sample(now,6,1.32,3,true);TEST_ASSERT_EQUAL(Channel::Overcurrent,c.fault);
        if(attempt<3) {now+=c.delayMs();TEST_ASSERT_TRUE(c.ready(now,true,true));}
        else {TEST_ASSERT_FALSE(c.waiting);TEST_ASSERT_FALSE(c.ready(now+100000,true,true));}
    }
}
void large_overload_respects_disabled_auto_retry() {
    Channel c;c.enabled(0);c.sample(10,6,1.32,3,false);c.sample(110,6,1.32,3,false);
    TEST_ASSERT_EQUAL(Channel::Overcurrent,c.fault);TEST_ASSERT_FALSE(c.waiting);TEST_ASSERT_FALSE(c.ready(999999,false,true));
}
void resistive_load_flickering_around_switch_threshold_trips_overcurrent() {
    // Constant ~7.3 A that brightness cannot reduce; IS noise crosses 1.6 V every sample.
    Channel c;c.enabled(0);bool tripped=false;
    for(uint32_t t=10;t<=200 && !tripped;t+=5) {
        c.sample(t,7.3,(t/5)%2?1.7f:1.5f,5,true);
        if(c.fault!=Channel::None) { tripped=true;TEST_ASSERT_UINT32_WITHIN(5,110,t); }
    }
    TEST_ASSERT_EQUAL(Channel::Overcurrent,c.fault);TEST_ASSERT_TRUE(c.waiting);
}
int main(){ UNITY_BEGIN(); RUN_TEST(resistive_load_flickering_around_switch_threshold_trips_overcurrent); RUN_TEST(flashes_above_switch_threshold_regulate_without_trip); RUN_TEST(switch_window_flash_recovers); RUN_TEST(persistent_switch_fault_still_latches_at_50ms); RUN_TEST(switch_reduction_rearms_after_low_sample_and_enable); RUN_TEST(large_overload_respects_disabled_auto_retry); RUN_TEST(black_to_white_and_flashes_regulate_without_trip); RUN_TEST(large_uncontrollable_load_uses_finite_overload_retries); RUN_TEST(startup_and_every_retry_allow_inrush); RUN_TEST(switch_noise_resets_debounce_without_current_trip); RUN_TEST(startup_boundary_wrap_and_manual_cycles); RUN_TEST(actual_channel_protections_are_independent); RUN_TEST(dim_before_trip); RUN_TEST(measured_feedback_with_render_delay); RUN_TEST(finite_retries); RUN_TEST(hard_faults_do_not_retry); RUN_TEST(interlocks_and_wrap); RUN_TEST(healthy_budget_reset); RUN_TEST(resistive_load_falls_back_to_trip); return UNITY_END(); }
