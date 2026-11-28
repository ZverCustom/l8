#include "led_control.h"
#include "obd2/obd2.h"
#include "settings/settings.h"
#include "vehicle/vehicle.h"
#include "vehicle/vehicle_rules.h"
#include "imu/imu.h"
#include "power/power.h"
#include "board/board.h"
#include <channel_map.h>
#include <esp_heap_caps.h>
#include <animation_time.h>
#include "led_output/parallel_output.h"

const int ledPins[NUM_STRIPS] = {v3::dataPin(0), v3::dataPin(1), v3::dataPin(2), v3::dataPin(3), v3::dataPin(4), v3::dataPin(5), v3::dataPin(6), v3::dataPin(7)};

CanSettings can_setting = {2000, 7000, 100, 100};
StripSettings stripSettings[NUM_STRIPS] = {};
int numLedsPerStrip[NUM_STRIPS] = {60,60,60,60,60,60,60,60};

extern OBD2 obd2;

// Animations render into leds[], the transformed copy in out[] is what FastLED sends
CRGB* leds[NUM_STRIPS];
static CRGB* out[NUM_STRIPS];
CLEDController* controllers[NUM_STRIPS] = {};
static int outputCounts[NUM_STRIPS]={};
static bool outputHealthy=false;
bool ledsHealthy() { return outputHealthy; }
const char* ledsBackend() {
#ifdef L8_LED_LCD_DMA
    return "lcd_cam_dma_experimental";
#else
    return "rmt";
#endif
}
static bool showFrame(uint8_t brightness) {
#ifdef L8_LED_LCD_DMA
    const uint8_t* buffers[8];for(int i=0;i<8;++i)buffers[i]=reinterpret_cast<const uint8_t*>(out[i]);
    if(!parallelShow(buffers,outputCounts,brightness)) { outputHealthy=false; powerInhibit(true); return false; }
#else
    FastLED.setBrightness(brightness); FastLED.show();
#endif
    return true;
}

// Per-strip animation state so strips don't share counters
struct AnimState {
    l8anim::Clock clock;
    l8anim::Meter meter, force;
    double fireTick=0;
    int effect=-1, count=0, source=-1;
    uint8_t* heat = nullptr;
};
static AnimState anim[NUM_STRIPS];

CRGB uintToCrgb(uint32_t color) {
    return CRGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
}

// 0 RGB, 1 GRB, 2 BGR, 3 RBG, 4 GBR, 5 BRG
static const uint8_t colorOrderMap[6][3] = {
    {0, 1, 2}, {1, 0, 2}, {2, 1, 0}, {0, 2, 1}, {1, 2, 0}, {2, 0, 1},
};

void init(int index) {
    if (numLedsPerStrip[index] < MIN_LEDS) numLedsPerStrip[index] = MIN_LEDS;
    if (numLedsPerStrip[index] > MAX_LEDS) numLedsPerStrip[index] = MAX_LEDS;
    outputCounts[index]=numLedsPerStrip[index];
    if (controllers[index]) controllers[index]->setLeds(out[index], numLedsPerStrip[index]);
    for (int i = numLedsPerStrip[index]; i < MAX_LEDS; i++) leds[index][i] = CRGB::Black;
}

void initializeLEDStrips() {
#ifdef L8_LED_LCD_DMA
    if(!parallelBegin()) { Serial.println("LCD DMA initialization failed; outputs inhibited"); return; }
#endif
    FastLED.setDither(0);  // temporal dither shows as flicker at low frame rates
    for (int i = 0; i < NUM_STRIPS; i++) {
        leds[i] = static_cast<CRGB*>(heap_caps_malloc(sizeof(CRGB)*MAX_LEDS,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        out[i] = static_cast<CRGB*>(heap_caps_malloc(sizeof(CRGB)*MAX_LEDS,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        if(!leds[i] || !out[i]) { Serial.println("LED buffer allocation failed"); return; }
        fill_solid(leds[i], MAX_LEDS, CRGB::Black);
        fill_solid(out[i], MAX_LEDS, CRGB::Black);

        Serial.printf("Initializing strip %d with pin %d and %d LEDs\n", i + 1, ledPins[i], numLedsPerStrip[i]);
#ifndef L8_LED_LCD_DMA
        switch (ledPins[i]) {
            case LED_PIN_1: controllers[i] = &FastLED.addLeds<WS2812B, LED_PIN_1, RGB>(out[i], numLedsPerStrip[i]); break;
            case LED_PIN_2: controllers[i] = &FastLED.addLeds<WS2812B, LED_PIN_2, RGB>(out[i], numLedsPerStrip[i]); break;
            case LED_PIN_3: controllers[i] = &FastLED.addLeds<WS2812B, LED_PIN_3, RGB>(out[i], numLedsPerStrip[i]); break;
            case LED_PIN_4: controllers[i] = &FastLED.addLeds<WS2812B, LED_PIN_4, RGB>(out[i], numLedsPerStrip[i]); break;
            case LED_PIN_5: controllers[i] = &FastLED.addLeds<WS2812B, LED_PIN_5, RGB>(out[i], numLedsPerStrip[i]); break;
            case LED_PIN_6: controllers[i] = &FastLED.addLeds<WS2812B, LED_PIN_6, RGB>(out[i], numLedsPerStrip[i]); break;
            case LED_PIN_7: controllers[i] = &FastLED.addLeds<WS2812B, LED_PIN_7, RGB>(out[i], numLedsPerStrip[i]); break;
            case LED_PIN_8: controllers[i] = &FastLED.addLeds<WS2812B, LED_PIN_8, RGB>(out[i], numLedsPerStrip[i]); break;
        }
#endif
        init(i);
    }
    outputHealthy=true;
}

// Brightness 0-100 -> FastLED 0-255
static uint8_t mapBrightness(int brightness) {
    return map(constrain(brightness, 0, 100), 0, 100, 0, 255);
}

void staticColor(int s) {
    fill_solid(leds[s], numLedsPerStrip[s], uintToCrgb(stripSettings[s].color));
}

static double phase(int s) { return anim[s].clock.ticks; }
static float cycle(int s,float period) { return fmod(phase(s),double(period)); }
static void dot(int s,float position,CRGB color) {
    int n=numLedsPerStrip[s], first=int(floorf(position));
    float fraction=position-first;
    CRGB a=color,b=color; a.nscale8(uint8_t((1-fraction)*255)); b.nscale8(uint8_t(fraction*255));
    leds[s][(first%n+n)%n]+=a; leds[s][((first+1)%n+n)%n]+=b;
}
void theaterChase(int s) {
    fill_solid(leds[s],numLedsPerStrip[s],CRGB::Black);
    for(int i=0;i<numLedsPerStrip[s];i+=3) dot(s,i+cycle(s,3),uintToCrgb(stripSettings[s].color));
}
void snake(int s) {
    fill_solid(leds[s],numLedsPerStrip[s],CRGB::Black);
    for(int i=0;i<min(20,numLedsPerStrip[s]);++i) dot(s,cycle(s,numLedsPerStrip[s])+i,uintToCrgb(stripSettings[s].color));
}
void colorWave(int s) {
    for(int i=0;i<numLedsPerStrip[s];++i) leds[s][i]=CHSV(uint8_t(uint32_t(phase(s)*5)+i*255/20),255,255);
}
void breathingLight(int s) {
    CRGB color=uintToCrgb(stripSettings[s].color);
    color.nscale8(uint8_t((1-cosf(cycle(s,102)*2*PI/102))*127.5f));
    fill_solid(leds[s],numLedsPerStrip[s],color);
}
void rainbowCycle(int s) {
    int n=numLedsPerStrip[s];
    for(int i=0;i<n;++i) leds[s][i]=CHSV(uint8_t(uint32_t(phase(s)*6)+i*256/n),255,255);
}
void meteorRain(int s) {
    int n=numLedsPerStrip[s]; float head=cycle(s,n);
    for(int i=0;i<n;++i) { float distance=fmodf(head-i+n,n); leds[s][i]=uintToCrgb(stripSettings[s].color); leds[s][i].nscale8(uint8_t(255*expf(-distance/3.5f))); }
}
static uint32_t noiseHash(uint32_t x) { x^=x>>16; x*=0x7feb352d; x^=x>>15; x*=0x846ca68b; return x^(x>>16); }
static void sparks(int s,bool multiColor) {
    int n=numLedsPerStrip[s]; fill_solid(leds[s],n,CRGB::Black);
    uint32_t tick=uint32_t(phase(s));
    for(int age=0;age<40;++age) {
        uint32_t seed=noiseHash((tick-age)^uint32_t(s*7919));
        CRGB color=multiColor?CRGB(CHSV(seed>>16,200,255)):uintToCrgb(stripSettings[s].color);
        color.nscale8(uint8_t(255*expf(-(age+float(phase(s)-tick))/(multiColor?15.f:4.f))));
        leds[s][seed%n]+=color;
    }
}
void twinkle(int s) { sparks(s,false); }
void confetti(int s) { sparks(s,true); }
void runningLights(int s) {
    for(int i=0;i<numLedsPerStrip[s];++i) { leds[s][i]=uintToCrgb(stripSettings[s].color); leds[s][i].nscale8(sin8(uint8_t(uint32_t(phase(s))+i*16))); }
}
void larsonScanner(int s) {
    int n=numLedsPerStrip[s]; float t=cycle(s,2*(n-1)); float head=t<n?t:2*(n-1)-t;
    for(int i=0;i<n;++i) { leds[s][i]=uintToCrgb(stripSettings[s].color); leds[s][i].nscale8(uint8_t(255*expf(-fabsf(head-i)/2.5f))); }
}

// Fire2012 by Mark Kriegsman
void fire(int s) {
    int n = numLedsPerStrip[s];
    if (!anim[s].heat) anim[s].heat = static_cast<uint8_t*>(heap_caps_calloc(MAX_LEDS, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint8_t* heat = anim[s].heat;
    if (!heat) { fill_solid(leds[s], n, CRGB::Black); return; }
    const uint8_t cooling = 55;
    const uint8_t sparking = 120;
    int steps=min(8,int(phase(s)-anim[s].fireTick));
    anim[s].fireTick+=steps; if(phase(s)-anim[s].fireTick>8) anim[s].fireTick=phase(s);
    for(int step=0;step<steps;++step) {
    for (int i = 0; i < n; i++) {
        heat[i] = qsub8(heat[i], random8(0, ((cooling * 10) / n) + 2));
    }
    for (int k = n - 1; k >= 2; k--) {
        heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3;
    }
    if (random8() < sparking) {
        int y = random8(min(n, 7));
        heat[y] = qadd8(heat[y], random8(160, 255));
    }
    }
    for (int j = 0; j < n; j++) leds[s][j] = HeatColor(heat[j]);
}

void police(int s) {
    int n=numLedsPerStrip[s], half=n/2;
    bool on=uint32_t(phase(s)/4)&1;
    fill_solid(leds[s],half,on?CRGB::Red:CRGB::Black);
    fill_solid(leds[s]+half,n-half,on?CRGB::Black:CRGB::Blue);
}
void colorWipe(int s) {
    int n=numLedsPerStrip[s]; float head=cycle(s,2*n);
    for(int i=0;i<n;++i) { float amount=constrain(head<n?head-i:i+1-(head-n),0.f,1.f); leds[s][i]=uintToCrgb(stripSettings[s].color); leds[s][i].nscale8(uint8_t(amount*255)); }
}
void sinelon(int s) {
    int n=numLedsPerStrip[s]; float head=(1+sinf(cycle(s,64)*2*PI/64))*(n-1)/2;
    for(int i=0;i<n;++i) { leds[s][i]=uintToCrgb(stripSettings[s].color); leds[s][i].nscale8(uint8_t(255*expf(-fabsf(head-i)/2))); }
}

// Whole strip coloured by lateral G: blue one way, red the other, brightness by magnitude
void gForce(int s) {
    int n = numLedsPerStrip[s];
    float g = anim[s].force.value;
    g = constrain(g, -1.0f, 1.0f);
    uint8_t v = (uint8_t)(fabsf(g) * 255);
    CRGB c = g < 0 ? CRGB(0, 0, 255) : CRGB(255, 0, 0);
    c.nscale8(v);
    fill_solid(leds[s], n, c);
}

CRGB getColorForSection(int currentLED, int numLeds) {
    // 7 colour bands, cold to hot
    static const CRGB bands[7] = {CRGB::Blue, CRGB::Cyan, CRGB::Green, CRGB::Yellow, CRGB::Orange, CRGB::Red, CRGB::Magenta};
    if (numLeds < 1) numLeds = 1;
    int band = currentLED * 7 / numLeds;
    return bands[constrain(band, 0, 6)];
}

// Value of a scale source as fraction of full scale, signed for G / yaw
static float scaleFraction(int source) {
    if((source==3 || source==4) && !imu.ok) return 0;
    switch (source) {
        case 1: case 2:
            return vrules::vehicleScale(source, vehicleFieldFresh(source), vehicle.rpm, vehicle.speed, vehicle.throttle, drive.maxSpeed, can_setting.minRPM, can_setting.maxRPM);
        case 3:  return drive.maxLateralG > 0 ? imuLateralG() / drive.maxLateralG : 0;
        case 4:  return drive.maxYawRate > 0 ? imuYawRate() / drive.maxYawRate : 0;
        case 5: case 6: case 7: return powerSnapshot().inputValue[source-5];
        default:
            return vrules::vehicleScale(0, vehicleFieldFresh(0), vehicle.rpm, vehicle.speed, vehicle.throttle, drive.maxSpeed, can_setting.minRPM, can_setting.maxRPM);
    }
}

static bool shiftLightActive() {
    return vrules::shiftLight(vehicleFieldFresh(0), vehicle.rpm, drive.shiftRPM, can_setting.maxRPM);
}

// Bar graph driven by the selected source
void rpmLevel(int s) {
    int n = numLedsPerStrip[s];
    const StripSettings& st = stripSettings[s];

    if (st.scaleSource == 0 && shiftLightActive()) {
        bool on = (millis() / 100) & 1;
        fill_solid(leds[s], n, on ? uintToCrgb(drive.shiftColor) : CRGB::Black);
        return;
    }

    fill_solid(leds[s], n, CRGB::Black);
    float frac = anim[s].meter.value;
    bool signedSource = st.scaleSource == 3 || st.scaleSource == 4;

    if (st.bottom) {
        // grow from the middle LED outwards
        int mid = constrain(st.center, 0, n - 1);
        int half = max(mid, n - 1 - mid);
        if (signedSource) {
            // negative = left of middle, positive = right
            int len = constrain((int)(fabsf(frac) * half), 0, half);
            for (int i = 0; i < len; i++) {
                int idx = frac < 0 ? mid - i : mid + i;
                if (idx >= 0 && idx < n) leds[s][idx] = getColorForSection(i, half);
            }
        } else {
            int len = constrain((int)(frac * half), 0, half);
            for (int i = 0; i < len; i++) {
                CRGB c = getColorForSection(i, half);
                if (mid + i < n) leds[s][mid + i] = c;
                if (mid - i >= 0) leds[s][mid - i] = c;
            }
        }
    } else {
        int len = constrain((int)(fabsf(frac) * n), 0, n);
        for (int i = 0; i < len; i++) leds[s][i] = getColorForSection(i, n);
    }
    float peak=anim[s].meter.peak;
    if(fabsf(peak)>0.01f) {
        int mid=constrain(st.center,0,n-1), half=max(mid,n-1-mid);
        int distance=constrain(int(fabsf(peak)*(st.bottom?half:n))-1,0,n-1);
        if(!st.bottom) leds[s][distance]=CRGB::White;
        else { if((!signedSource || peak>=0) && mid+distance<n) leds[s][mid+distance]=CRGB::White; if((!signedSource || peak<0) && mid-distance>=0) leds[s][mid-distance]=CRGB::White; }
    }
}

static void renderStrip(int i) {
    const StripSettings& st = stripSettings[i];
    if (st.animationIndex != 10 && anim[i].heat) { free(anim[i].heat); anim[i].heat = nullptr; }
    bool useScale = (st.mode == 1) || (st.mode == 2 && fabsf(anim[i].meter.value) > 0.05f);
    if (useScale) {
        rpmLevel(i);
        return;
    }
    switch (st.animationIndex) {
        case 0:  staticColor(i); break;
        case 1:  theaterChase(i); break;
        case 2:  colorWave(i); break;
        case 3:  breathingLight(i); break;
        case 4:  rainbowCycle(i); break;
        case 5:  snake(i); break;
        case 6:  meteorRain(i); break;
        case 7:  twinkle(i); break;
        case 8:  runningLights(i); break;
        case 9:  larsonScanner(i); break;
        case 10: fire(i); break;
        case 11: police(i); break;
        case 12: confetti(i); break;
        case 13: colorWipe(i); break;
        case 14: sinelon(i); break;
        case 15: gForce(i); break;
        default: staticColor(i); break;
    }
}

// leds[] -> out[]: enable, brightness, reverse, colour order
static void transformStrip(int i) {
    const StripSettings& st = stripSettings[i];
    int n = numLedsPerStrip[i];
    CRGB* dstBuf = out[i];
    if (!st.enabled) {
        fill_solid(dstBuf, n, CRGB::Black);
        return;
    }
    auto power=powerSnapshot();
    uint8_t scale = uint8_t(mapBrightness(st.brightness)*power.channels[i].brightnessScale*power.totalBrightnessScale);
    int order = constrain(st.colorOrder, 0, 5);
    const uint8_t* m = colorOrderMap[order];
    for (int k = 0; k < n; k++) {
        CRGB src = leds[i][st.reverse ? n - 1 - k : k];
        src.nscale8(scale);
        CRGB& dst = dstBuf[k];
        dst[0] = src[m[0]];
        dst[1] = src[m[1]];
        dst[2] = src[m[2]];
    }
}

static volatile bool reinitPending = false;
static volatile bool frameShown = false;

bool ledsFrameShown() {
    return frameShown;
}

void requestLedReinit() {
    reinitPending = true;
}

void runAnimations() {
    if(!outputHealthy) return;
    static uint32_t lastFrame = 0;
    uint32_t now = micros();
    if (uint32_t(now-lastFrame)<16667) return;
    lastFrame=now;
    for(int i=0;i<NUM_STRIPS;++i) {
        auto& a=anim[i]; const auto& st=stripSettings[i];
        if(a.effect!=st.animationIndex || a.count!=numLedsPerStrip[i]) { a.clock={}; a.fireTick=0; if(a.heat) memset(a.heat,0,MAX_LEDS); a.effect=st.animationIndex; a.count=numLedsPerStrip[i]; }
        if(a.source!=st.scaleSource) { a.meter={}; a.source=st.scaleSource; }
        float dt=a.clock.advance(millis(),st.speed);
        a.meter.update(scaleFraction(st.scaleSource),dt,millis());
        a.force.update(imu.ok && drive.maxLateralG>0?imuLateralG()/drive.maxLateralG:0,dt,millis());
    }

    if (reinitPending) {
        reinitPending = false;
        // With live settings the supply stays on. Clear removed pixels using
        // the old frame length before shortening the controller's buffer.
        bool clearTail = false;
        for (int i = 0; i < NUM_STRIPS; i++) {
            int previous = outputCounts[i];
            if (previous > numLedsPerStrip[i]) {
                fill_solid(out[i] + numLedsPerStrip[i], previous - numLedsPerStrip[i], CRGB::Black);
                clearTail = true;
            }
        }
        if (clearTail && !showFrame(mapBrightness(can_setting.brightess))) return;
        for (int i = 0; i < NUM_STRIPS; i++) init(i);
    }

    int brightness = can_setting.brightess;
    // no data into unpowered strips (latched or master off)
    bool blackout = !drive.masterOn || !frameShown || !powerSnapshot().on;
    if (!blackout && !engineRunning()) {
        if (drive.engineOff == 2) blackout = true;
        else if (drive.engineOff == 1) brightness = brightness * constrain(drive.engineOffDim, 0, 100) / 100;
    }

    for (int i = 0; i < NUM_STRIPS; i++) {
        auto channelState = powerSnapshot().channels[i];
        static bool wasOn[NUM_STRIPS] = {};
        bool dataEnabled = channelState.on && (boardDataMask() & v3::channelMask(i));
        bool firstPoweredFrame = dataEnabled && !wasOn[i];
        wasOn[i] = dataEnabled;
        if (blackout || !dataEnabled || firstPoweredFrame) {
            fill_solid(out[i], numLedsPerStrip[i], CRGB::Black);
            continue;
        }
        renderStrip(i);
        transformStrip(i);
    }
    if(!showFrame(mapBrightness(brightness))) return;
    frameShown = true;
    powerSampleAdc();
}

// Flash writes (settings, log, OTA) must not overlap RMT output. The LED task holds
// frameMutex for the whole frame; a writer takes it (blocking until the frame ends) and
// keeps it until resume, so no race window between check and show().
static SemaphoreHandle_t frameMutex = nullptr;
static volatile unsigned pauseDepth = 0;

static void ensureMutex() {
    if (!frameMutex) frameMutex = xSemaphoreCreateRecursiveMutex();
}

void ledsPause() {
    ensureMutex();
    xSemaphoreTakeRecursive(frameMutex, portMAX_DELAY);
    ++pauseDepth;
}

void ledsResume() {
    if (!pauseDepth) return;
    --pauseDepth;
    xSemaphoreGiveRecursive(frameMutex);
}

bool ledsPaused() {
    return pauseDepth != 0;
}

static void ledTask(void*) {
    ensureMutex();
    for (;;) {
        xSemaphoreTakeRecursive(frameMutex, portMAX_DELAY);
        runAnimations();
        xSemaphoreGiveRecursive(frameMutex);
        vTaskDelay(1);
    }
}

void startLedTask() {
    ensureMutex();
    // core 1 alone: loop() runs on core 0 (ARDUINO_RUNNING_CORE), so nothing here delays the
    // RMT refill interrupt, which is allocated on this core at the first show()
    xTaskCreatePinnedToCore(ledTask, "leds", 6144, nullptr, 1, nullptr, 1);
}

// Enable only for cache-off qualification of the optional full-frame DMA backend.
void ledsFlashPause() {
#if !defined(L8_LED_LCD_DMA) || !defined(L8_DMA_FLASH_CONCURRENT)
    ledsPause();
#endif
}
void ledsFlashResume() {
#if !defined(L8_LED_LCD_DMA) || !defined(L8_DMA_FLASH_CONCURRENT)
    ledsResume();
#endif
}
