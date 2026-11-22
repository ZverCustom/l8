#include <unity.h>
#include <string.h>
#include <channel_reason.h>
using v3::Reason; using v3::ReasonInputs;
void setUp(){} void tearDown(){}
static ReasonInputs allowedOn() { return ReasonInputs{true,false,false,false,true,true,true,true,false,true}; }
void healthy_permitted_channel_is_on() { TEST_ASSERT_EQUAL(Reason::On,v3::channelReason(allowedOn())); }
void each_single_condition_maps_to_its_reason() {
    auto x=allowedOn(); x.on=false;
    auto io=x; io.ioOk=false; TEST_ASSERT_EQUAL(Reason::Io,v3::channelReason(io));
    auto f=x; f.faultLatched=true; TEST_ASSERT_EQUAL(Reason::Fault,v3::channelReason(f));
    auto inh=x; inh.inhibit=true; TEST_ASSERT_EQUAL(Reason::Inhibit,v3::channelReason(inh));
    auto nv=x; nv.supplyOk=false; TEST_ASSERT_EQUAL(Reason::NoVbat,v3::channelReason(nv));
    auto ig=x; ig.ignOk=false; TEST_ASSERT_EQUAL(Reason::Ign,v3::channelReason(ig));
    auto m=x; m.master=false; TEST_ASSERT_EQUAL(Reason::Master,v3::channelReason(m));
    auto d=x; d.enabled=false; TEST_ASSERT_EQUAL(Reason::Disabled,v3::channelReason(d));
    auto e=x; e.engineCut=true; TEST_ASSERT_EQUAL(Reason::Engine,v3::channelReason(e));
    auto r=x; r.faultLatched=true; r.retryPending=true; TEST_ASSERT_EQUAL(Reason::Retry,v3::channelReason(r));
}
// Strip conditions one by one from "everything wrong" and check the winner each step.
void priority_follows_contract_table() {
    ReasonInputs x{false,true,false,true,false,false,false,false,true,false};
    TEST_ASSERT_EQUAL(Reason::Io,v3::channelReason(x)); x.ioOk=true;
    TEST_ASSERT_EQUAL(Reason::Fault,v3::channelReason(x)); x.faultLatched=false;
    TEST_ASSERT_EQUAL(Reason::Inhibit,v3::channelReason(x)); x.inhibit=false;
    TEST_ASSERT_EQUAL(Reason::NoVbat,v3::channelReason(x)); x.supplyOk=true;
    TEST_ASSERT_EQUAL(Reason::Ign,v3::channelReason(x)); x.ignOk=true;
    TEST_ASSERT_EQUAL(Reason::Master,v3::channelReason(x)); x.master=true;
    TEST_ASSERT_EQUAL(Reason::Disabled,v3::channelReason(x)); x.enabled=true;
    TEST_ASSERT_EQUAL(Reason::Engine,v3::channelReason(x)); x.engineCut=false;
    x.faultLatched=true; x.retryPending=true;
    TEST_ASSERT_EQUAL(Reason::Retry,v3::channelReason(x));
}
void pending_retry_yields_to_missing_power_and_master() {
    ReasonInputs x=allowedOn(); x.on=false; x.faultLatched=true; x.retryPending=true;
    auto nv=x; nv.supplyOk=false; TEST_ASSERT_EQUAL(Reason::NoVbat,v3::channelReason(nv));
    auto m=x; m.master=false; TEST_ASSERT_EQUAL(Reason::Master,v3::channelReason(m));
    // A latched fault without retry outranks everything but I/O.
    auto f=x; f.retryPending=false; f.supplyOk=false; f.inhibit=true; TEST_ASSERT_EQUAL(Reason::Fault,v3::channelReason(f));
}
void io_failure_never_reports_on() { auto x=allowedOn(); x.ioOk=false; TEST_ASSERT_EQUAL(Reason::Io,v3::channelReason(x)); }
void every_reason_has_a_stable_name() {
    const char* names[]={"on","io","fault","inhibit","novbat","ign","master","disabled","engine","retry"};
    for(int i=0;i<10;++i) TEST_ASSERT_EQUAL_STRING(names[i],v3::reasonName(Reason(i)));
    TEST_ASSERT_EQUAL_STRING("unknown",v3::reasonName(Reason(42)));
}
int main(){UNITY_BEGIN();RUN_TEST(healthy_permitted_channel_is_on);RUN_TEST(each_single_condition_maps_to_its_reason);RUN_TEST(priority_follows_contract_table);RUN_TEST(pending_retry_yields_to_missing_power_and_master);RUN_TEST(io_failure_never_reports_on);RUN_TEST(every_reason_has_a_stable_name);return UNITY_END();}
