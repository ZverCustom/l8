#include <unity.h>
#include <ram_log.h>
void overlap() {
    char memory[8], out[9]={}; RamLog r(memory,8);
    r.append("abcdef",6); auto through=r.end;
    r.append("ghijkl",6); r.acknowledge(through);
    TEST_ASSERT_EQUAL_UINT64(6,r.flushed);
    TEST_ASSERT_EQUAL(6,r.copy(r.flushed,out,8)); TEST_ASSERT_EQUAL_STRING("ghijkl",out);
    TEST_ASSERT_EQUAL_UINT64(4,r.lost);
}
void tail_and_clear() {
    char memory[8], out[9]={}; RamLog r(memory,8);
    r.append("0123456789",10); r.copy(0,out,8);
    TEST_ASSERT_EQUAL_STRING("23456789",out);
    r.clear(); TEST_ASSERT_EQUAL(0,r.copy(r.flushed,out,8));
    r.append("a",1); TEST_ASSERT_EQUAL(1,r.copy(r.flushed,out,8));
}
int main() { UNITY_BEGIN(); RUN_TEST(overlap); RUN_TEST(tail_and_clear); return UNITY_END(); }
