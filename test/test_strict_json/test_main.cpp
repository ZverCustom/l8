#include <unity.h>
#include <strict_json.h>
#include <string.h>
void setUp(){} void tearDown(){}
static int c(const char* s,int depth=12){return strictjson::check(s,strlen(s),depth);}
void accepts_rfc8259(){
    const char* ok[]={"{}","[]","{\"a\":1}","{\"a\":[1,-2.5e+3,0,true,false,null,\"x\\u00e9\\n\"]}"," \r\n{\"a\" : {\"b\":[]}}\n","\"s\"","0","-0.1E5"};
    for(auto s:ok) TEST_ASSERT_EQUAL_MESSAGE(0,c(s),s);
}
void rejects_lenient_forms(){
    const char* bad[]={"{v:1}","{'v':1}","{\"a\":1,}","[1,]","{\"a\":01}","{\"a\":1.}","{\"a\":.5}","{\"a\":+1}","{\"a\":\"x\ty\"}","{\"a\":\"\\x\"}",
        "{\"a\":\"\\u12\"}","{\"a\":tru}","{\"a\":NaN}","{\"a\":1}x","{\"a\":1}{}","","  ","{\"a\"","{\"a\":1","[1 2]","{\"a\" 1}","{1:2}","/*c*/{}","{\"a\":Infinity}"};
    for(auto s:bad) TEST_ASSERT_EQUAL_MESSAGE(1,c(s),s);
}
void depth_limit(){
    TEST_ASSERT_EQUAL(0,c("[[[]]]",3)); TEST_ASSERT_EQUAL(2,c("[[[[]]]]",3)); TEST_ASSERT_EQUAL(2,c("{\"a\":{\"b\":{\"c\":{}}}}",3));
}
void embedded_nul_rejected(){ const char s[]="{\"a\":\"x\0y\"}"; TEST_ASSERT_EQUAL(1,strictjson::check(s,sizeof(s)-1,12)); }
int main(){UNITY_BEGIN();RUN_TEST(accepts_rfc8259);RUN_TEST(rejects_lenient_forms);RUN_TEST(depth_limit);RUN_TEST(embedded_nul_rejected);return UNITY_END();}
