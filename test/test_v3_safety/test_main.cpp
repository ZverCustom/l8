#include <unity.h>
#include <initializer_list>
#include <v3_safety.h>
void setUp() {} void tearDown() {}
void test_bts7008_conversion_and_safe_supply() {
    TEST_ASSERT_FLOAT_WITHIN(.001f,5.f,v3::currentAmps(5.f*1200.f/5450.f));
    TEST_ASSERT_FALSE(v3::supplyValid(0)); TEST_ASSERT_FALSE(v3::supplyValid(NAN));
    TEST_ASSERT_FALSE(v3::supplyValid(INFINITY)); TEST_ASSERT_FALSE(v3::supplyValid(8.9f));
    TEST_ASSERT_TRUE(v3::supplyValid(12)); TEST_ASSERT_FALSE(v3::supplyValid(17));
}
void test_shared_sense_never_selects_two_switches() {
    const uint16_t expected[]={0x0100,0x1100,0x0200,0x1200,0x0400,0x1400,0x0800,0x1800};
    for(unsigned c=0;c<8;++c) {
        uint16_t s=v3::senseSelect(c), den=s&0x0f00;
        TEST_ASSERT_EQUAL_HEX16(expected[c],s);
        TEST_ASSERT_TRUE(den && !(den&(den-1)));
        TEST_ASSERT_EQUAL_HEX16(0,s&0xe0ff); // never changes IN, CAN, status or sensor power
    }
    TEST_ASSERT_EQUAL(0,v3::senseSelect(8));
}
void test_all_channel_latches_independent() {
    PowerLatch l[8];
    for(unsigned c=0;c<8;++c) l[c].update(0,c==6?6.f:1.f,.3f,5.f);
    for(unsigned c=0;c<8;++c) TEST_ASSERT_EQUAL(c==6?PowerLatch::OVERCURRENT:PowerLatch::NONE,l[c].update(100,c==6?6.f:1.f,.3f,5.f));
    l[6].reset(); TEST_ASSERT_EQUAL(PowerLatch::NONE,l[6].update(200,1,.2,5));
}
void test_supply_and_ign_interlocks() {
    for(int bits=0;bits<64;++bits) {
        bool ign=bits&1,allowWithoutIgn=bits&2,inhibit=bits&4,ready=bits&8,latched=bits&16;
        float vbat=bits&32?12.f:0.f;
        bool allow=v3::ledsAllowed(vbat,ign,allowWithoutIgn,inhibit,ready,latched);
        if(vbat==0||(!ign&&!allowWithoutIgn)||inhibit||!ready||latched) TEST_ASSERT_FALSE(allow);
        else TEST_ASSERT_TRUE(allow);
        // IGN must never disable CAN/sensor power or the controller itself.
        TEST_ASSERT_EQUAL(vbat!=0&&!inhibit&&ready&&!latched,v3::outputsAllowed(vbat,inhibit,ready,latched));
        TEST_ASSERT_EQUAL(vbat!=0,v3::controllerHold(vbat));
    }
}
void test_controller_stays_alive_outside_strip_voltage_range() {
    for(float vbat : {0.f,.4f,5.f,5.9f,NAN,INFINITY}) TEST_ASSERT_FALSE(v3::controllerHold(vbat));
    for(float vbat : {6.f,8.9f,17.f}) {
        TEST_ASSERT_TRUE(v3::controllerHold(vbat));
        TEST_ASSERT_FALSE(v3::outputsAllowed(vbat,false,true,false));
        TEST_ASSERT_FALSE(v3::ledsAllowed(vbat,false,true,false,true,false));
    }
    for(float vbat : {9.f,12.f,16.5f}) {
        TEST_ASSERT_TRUE(v3::controllerHold(vbat));
        TEST_ASSERT_TRUE(v3::outputsAllowed(vbat,false,true,false));
        TEST_ASSERT_TRUE(v3::ledsAllowed(vbat,false,true,false,true,false));
        TEST_ASSERT_FALSE(v3::ledsAllowed(vbat,false,false,false,true,false));
    }
}
void test_input_topology_and_clamps() {
    TEST_ASSERT_FLOAT_WITHIN(.001f,1.f,v3::inputFraction(5000.f/5.3f,0,0,5,0));
    TEST_ASSERT_FLOAT_WITHIN(.001f,.5f,v3::inputFraction(2500.f/5.3f,0,0,5,0));
    TEST_ASSERT_EQUAL_FLOAT(0,v3::inputFraction(100,0,5,5,0));
    TEST_ASSERT_EQUAL_FLOAT(1,v3::inputFraction(1250,1,0,5,600));
    TEST_ASSERT_EQUAL_FLOAT(0,v3::inputFraction(0,1,0,5,600));
    TEST_ASSERT_EQUAL_FLOAT(0,v3::inputFraction(1650,2,0,5,1565));
    TEST_ASSERT_EQUAL_FLOAT(1,v3::inputFraction(1480,2,0,5,1565));
}
int main(){UNITY_BEGIN();RUN_TEST(test_bts7008_conversion_and_safe_supply);RUN_TEST(test_shared_sense_never_selects_two_switches);RUN_TEST(test_all_channel_latches_independent);RUN_TEST(test_supply_and_ign_interlocks);RUN_TEST(test_controller_stays_alive_outside_strip_voltage_range);RUN_TEST(test_input_topology_and_clamps);return UNITY_END();}
