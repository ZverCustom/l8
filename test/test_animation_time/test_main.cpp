#include <unity.h>
#include <animation_time.h>
void cadence_independent() {
    l8anim::Clock a,b; a.advance(0,60); b.advance(0,60);
    for(int t=10;t<=1000;t+=10) a.advance(t,60);
    for(int t=25;t<=1000;t+=25) b.advance(t,60);
    TEST_ASSERT_FLOAT_WITHIN(0.00001,a.ticks,b.ticks);
    auto before=a.ticks; a.advance(1100,0); TEST_ASSERT_FLOAT_WITHIN(.00001,100./150,a.ticks-before);
}
void wrap() {
    l8anim::Clock c; c.advance(0xfffffff0,100); c.advance(0x10,100);
    TEST_ASSERT_FLOAT_WITHIN(.00001,2,c.ticks);
}
void meter_decay_and_peak() {
    l8anim::Meter m; m.update(1,.1,100); auto high=m.value;
    TEST_ASSERT_GREATER_THAN_FLOAT(.85,high);
    m.update(0,.1,200); TEST_ASSERT_GREATER_THAN_FLOAT(.6,m.value);
    TEST_ASSERT_FLOAT_WITHIN(.0001,high,m.peak);
    m.update(0,.4,600); TEST_ASSERT_LESS_THAN_FLOAT(high,m.peak);
    m.update(NAN,10,10600); TEST_ASSERT_FLOAT_WITHIN(.001,0,m.value);
}
int main(){ UNITY_BEGIN(); RUN_TEST(cadence_independent); RUN_TEST(wrap); RUN_TEST(meter_decay_and_peak); return UNITY_END(); }
