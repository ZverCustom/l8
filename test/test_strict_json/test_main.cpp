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
    for(auto s:bad) TEST_ASSERT_EQUAL_MESSAGE(strictjson::Syntax,c(s),s);
}
void depth_limit(){
    TEST_ASSERT_EQUAL(0,c("[[[]]]",3)); TEST_ASSERT_EQUAL(strictjson::TooDeep,c("[[[[]]]]",3)); TEST_ASSERT_EQUAL(strictjson::TooDeep,c("{\"a\":{\"b\":{\"c\":{}}}}",3));
}
void embedded_nul_rejected(){ const char s[]="{\"a\":\"x\0y\"}"; TEST_ASSERT_EQUAL(strictjson::Forbidden,strictjson::check(s,sizeof(s)-1,12)); }
void escaped_nul_forbidden_in_values_and_keys(){
    TEST_ASSERT_EQUAL(strictjson::Forbidden,c("{\"cmd\":\"action\\u0000junk\"}"));
    TEST_ASSERT_EQUAL(strictjson::Forbidden,c("{\"on\\u0000junk\":false}"));
    TEST_ASSERT_EQUAL(strictjson::Forbidden,c("[\"\\u0000\"]"));
    TEST_ASSERT_EQUAL(0,c("{\"a\":\"\\\\u0000\"}")); // literal backslash + u0000, no NUL
}
void duplicate_keys_by_decoded_value(){
    TEST_ASSERT_EQUAL(strictjson::Duplicate,c("{\"on\":true,\"on\":false}"));
    TEST_ASSERT_EQUAL(strictjson::Duplicate,c("{\"a\":1,\"\\u0061\":2}"));
    TEST_ASSERT_EQUAL(strictjson::Duplicate,c("{\"x\":{\"k\":1,\"k\":1}}"));
    TEST_ASSERT_EQUAL(0,c("{\"a\":{\"a\":1},\"b\":[{\"a\":1},{\"a\":2}]}"));
}
static int b(const char* s){return strictjson::check(s,strlen(s),12);}
void utf8_validation(){
    const char* ok[]={"[\"\xd0\x96\xd0\xb8\"]","[\"\xe2\x82\xac\"]","[\"\xf0\x9f\x98\x80\"]","[\"\\u0416\"]","[\"\\ud83d\\ude00\"]","[\"\xef\xbf\xbf\"]","[\"\xf4\x8f\xbf\xbf\"]"};
    for(auto s:ok) TEST_ASSERT_EQUAL_MESSAGE(0,b(s),s);
    const char* bad[]={"[\"\xff\"]","[\"\x80\"]","[\"\xbf\"]","[\"\xc3\"]","[\"\xe2\x82\"]","[\"\xf0\x9f\x98\"]","[\"\xc0\xaf\"]","[\"\xc1\xbf\"]",
        "[\"\xe0\x80\xaf\"]","[\"\xf0\x80\x80\xaf\"]","[\"\xed\xa0\x80\"]","[\"\xf4\x90\x80\x80\"]","[\"\xf5\x80\x80\x80\"]","[\"\\ud800\"]","[\"\\udc00\"]","[\"\\ud83d\\u0041\"]","[\"\\ud83dx\"]","{\"\xff\":1}"};
    for(auto s:bad) TEST_ASSERT_EQUAL_MESSAGE(strictjson::Syntax,b(s),s);
    TEST_ASSERT_TRUE(strictjson::validUtf8("\xd0\x96",2)); TEST_ASSERT_FALSE(strictjson::validUtf8("a\xff",2));
}
int main(){UNITY_BEGIN();RUN_TEST(accepts_rfc8259);RUN_TEST(rejects_lenient_forms);RUN_TEST(depth_limit);RUN_TEST(embedded_nul_rejected);RUN_TEST(escaped_nul_forbidden_in_values_and_keys);RUN_TEST(duplicate_keys_by_decoded_value);RUN_TEST(utf8_validation);return UNITY_END();}
