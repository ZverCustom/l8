#include <unity.h>
#include <line_framer.h>
#include <string>
void setUp(){} void tearDown(){}
static std::string feed(LineFramer& f,const std::string& s,int& lines,int& overflows) {
    std::string last; for(char c:s) { auto e=f.feed(c); if(e==LineFramer::Line){++lines;last=f.text();} if(e==LineFramer::Overflow)++overflows; } return last;
}
void exact_limit_is_a_line_and_one_more_overflows() {
    LineFramer f(8); int l=0,o=0;
    TEST_ASSERT_EQUAL_STRING("12345678",feed(f,"12345678\n",l,o).c_str()); TEST_ASSERT_EQUAL(1,l); TEST_ASSERT_EQUAL(0,o);
    l=0; feed(f,"123456789\n",l,o); TEST_ASSERT_EQUAL(0,l); TEST_ASSERT_EQUAL(1,o);
}
void oversize_tail_never_becomes_a_command() {
    LineFramer f(8); int l=0,o=0;
    std::string got=feed(f,std::string(20,'x')+"{\"cmd\":\"x\"}\n{\"ok\":1}\n",l,o);
    TEST_ASSERT_EQUAL(1,o); TEST_ASSERT_EQUAL(1,l); TEST_ASSERT_EQUAL_STRING("{\"ok\":1}",got.c_str());
}
void cr_and_blank_lines_are_ignored() {
    LineFramer f(8); int l=0,o=0;
    TEST_ASSERT_EQUAL_STRING("ab",feed(f,"\r\n\n a\rb\r\n",l,o).substr(1).c_str()); TEST_ASSERT_EQUAL(1,l); TEST_ASSERT_EQUAL(0,o);
}
void overflow_reported_once_per_line_even_if_huge() {
    LineFramer f(4); int l=0,o=0; feed(f,std::string(10000,'y')+"\n",l,o); TEST_ASSERT_EQUAL(1,o); TEST_ASSERT_EQUAL(0,f.buffer.size());
    feed(f,"ok\n",l,o); TEST_ASSERT_EQUAL(1,l);
}
void partial_line_waits_for_newline() {
    LineFramer f(8); int l=0,o=0; feed(f,"abc",l,o); TEST_ASSERT_EQUAL(0,l); feed(f,"\n",l,o); TEST_ASSERT_EQUAL(1,l);
}
int main(){UNITY_BEGIN();RUN_TEST(exact_limit_is_a_line_and_one_more_overflows);RUN_TEST(oversize_tail_never_becomes_a_command);RUN_TEST(cr_and_blank_lines_are_ignored);RUN_TEST(overflow_reported_once_per_line_even_if_huge);RUN_TEST(partial_line_waits_for_newline);return UNITY_END();}
