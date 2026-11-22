#include <unity.h>
#include <parallel_led.h>
#include <channel_map.h>
void waveform_mapping_gate() {
    uint8_t pixels[8][6]={}; const uint8_t* strips[8]; int counts[8]; uint8_t masks[8];
    for(int s=0;s<8;++s){ strips[s]=pixels[s];counts[s]=1;masks[s]=v3::channelMask(s);pixels[s][0]=1u<<s; }
    uint8_t buf[l8led::samples(2)]; auto n=l8led::encode(buf,sizeof(buf),strips,counts,masks,0xff,255);
    TEST_ASSERT_EQUAL(l8led::samples(1),n);
    for(int bit=0;bit<8;++bit){auto p=l8led::RESET_SAMPLES+bit*4;TEST_ASSERT_EQUAL_HEX8(255,buf[p]);TEST_ASSERT_EQUAL_HEX8(masks[7-bit],buf[p+1]);TEST_ASSERT_EQUAL(0,buf[p+2]|buf[p+3]);}
    for(size_t i=0;i<l8led::RESET_SAMPLES;++i){TEST_ASSERT_EQUAL(0,buf[i]);TEST_ASSERT_EQUAL(0,buf[n-i-1]);}
    l8led::encode(buf,sizeof(buf),strips,counts,masks,masks[1],255);
    for(size_t i=0;i<n;++i)TEST_ASSERT_EQUAL(0,buf[i]&~masks[1]);
}
void lengths_brightness_and_bounds() {
    uint8_t a[6]={255,255,255,255,255,255};const uint8_t* p[8]={a,a};int count[8]={1,2};uint8_t masks[8]={1,2,4,8,16,32,64,128};uint8_t buf[l8led::samples(2)];
    TEST_ASSERT_EQUAL(sizeof(buf),l8led::encode(buf,sizeof(buf),p,count,masks,3,0));
    TEST_ASSERT_EQUAL(3,buf[1024]);TEST_ASSERT_EQUAL(0,buf[1025]);TEST_ASSERT_EQUAL(2,buf[1024+96]);
    TEST_ASSERT_EQUAL(0,l8led::encode(buf,16,p,count,masks,3,255));
    count[0]=1001;TEST_ASSERT_EQUAL(0,l8led::encode(buf,sizeof(buf),p,count,masks,3,255));
}
void maximum_frame_and_canary() {
    static uint8_t data[3000], buffer[l8led::samples(1000)+4];
    memset(data,255,sizeof(data));memset(buffer,0x5a,sizeof(buffer));
    const uint8_t* p[8];int counts[8];uint8_t masks[8];
    for(int i=0;i<8;++i){p[i]=data;counts[i]=1000;masks[i]=v3::channelMask(i);}
    size_t n=l8led::encode(buffer,sizeof(buffer)-4,p,counts,masks,255,255);
    TEST_ASSERT_EQUAL(98048,n);
    for(size_t i=n;i<sizeof(buffer);++i)TEST_ASSERT_EQUAL_HEX8(0x5a,buffer[i]);
    for(size_t i=1024;i<n-1024;i+=4){TEST_ASSERT_EQUAL(255,buffer[i]);TEST_ASSERT_EQUAL(255,buffer[i+1]);TEST_ASSERT_EQUAL(0,buffer[i+2]|buffer[i+3]);}
}
int main(){UNITY_BEGIN();RUN_TEST(waveform_mapping_gate);RUN_TEST(lengths_brightness_and_bounds);RUN_TEST(maximum_frame_and_canary);return UNITY_END();}
