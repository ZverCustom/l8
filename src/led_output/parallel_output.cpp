#include "parallel_output.h"
#ifdef L8_LED_LCD_DMA
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <driver/periph_ctrl.h>
#include <esp_private/gdma.h>
#include <hal/lcd_hal.h>
#include <hal/lcd_ll.h>
#include <hal/dma_types.h>
#include <soc/lcd_cam_struct.h>
#include <parallel_led.h>
#include <channel_map.h>
#include "board/board.h"
#include "power/power.h"

// Exclusive LCD_CAM ownership. No clock/DC/CS GPIO is assigned: only the eight
// existing DATA pins are used. Full frames/descriptors live in internal SRAM.
// No ISR, refill, PSRAM DMA, or cache-dependent callback is needed mid-frame.
static constexpr size_t CAPACITY=l8led::samples(1000);
static constexpr size_t NODE_BYTES=4092;
static constexpr size_t NODES=(CAPACITY+NODE_BYTES-1)/NODE_BYTES;
static uint8_t* samples=nullptr;
static dma_descriptor_t* descriptors=nullptr;
static gdma_channel_handle_t dma=nullptr;
static bool healthy=false;
static bool fail() {
    healthy=false; boardDataMask(0); powerInhibit(true);
    lcd_ll_stop(&LCD_CAM); if(dma) gdma_stop(dma);
    return false;
}
static bool initFailed() {
    healthy=false; boardDataMask(0);
    if(dma) { gdma_disconnect(dma); gdma_del_channel(dma); dma=nullptr; }
    free(samples); free(descriptors); samples=nullptr; descriptors=nullptr;
    return false;
}
bool parallelHealthy() { return healthy; }
bool parallelBegin() {
    samples=static_cast<uint8_t*>(heap_caps_malloc(CAPACITY,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
    descriptors=static_cast<dma_descriptor_t*>(heap_caps_calloc(NODES,sizeof(dma_descriptor_t),MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
    if(!samples || !descriptors) return initFailed();
    gdma_channel_alloc_config_t allocation={}; allocation.direction=GDMA_CHANNEL_DIRECTION_TX;
    if(gdma_new_channel(&allocation,&dma)!=ESP_OK) return initFailed();
    if(gdma_connect(dma,GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD,0))!=ESP_OK) return initFailed();
    gdma_transfer_ability_t ability={}; ability.sram_trans_align=4;
    if(gdma_set_transfer_ability(dma,&ability)!=ESP_OK) return initFailed();
    gdma_strategy_config_t strategy={}; strategy.owner_check=true; strategy.auto_update_desc=true;
    if(gdma_apply_strategy(dma,&strategy)!=ESP_OK) return initFailed();
    periph_module_enable(PERIPH_LCD_CAM_MODULE);
    lcd_hal_context_t hal={}; lcd_hal_init(&hal,0);
    lcd_ll_reset(&LCD_CAM); lcd_ll_fifo_reset(&LCD_CAM); lcd_ll_enable_clock(&LCD_CAM,true);
    lcd_ll_select_clk_src(&LCD_CAM,LCD_CLK_SRC_PLL160M);
    lcd_ll_set_group_clock_coeff(&LCD_CAM,2,0,0);
    lcd_ll_set_pixel_clock_prescale(&LCD_CAM,25);
    lcd_ll_set_clock_idle_level(&LCD_CAM,false); lcd_ll_set_pixel_clock_edge(&LCD_CAM,false);
    lcd_ll_enable_rgb_mode(&LCD_CAM,false); lcd_ll_enable_rgb_yuv_convert(&LCD_CAM,false);
    lcd_ll_set_data_width(&LCD_CAM,8); lcd_ll_reverse_bit_order(&LCD_CAM,false); lcd_ll_swap_byte_order(&LCD_CAM,8,false);
    lcd_ll_set_phase_cycles(&LCD_CAM,0,0,1); lcd_ll_set_blank_cycles(&LCD_CAM,0,0);
    lcd_ll_enable_output_always_on(&LCD_CAM,true); // DMA EOF terminates transaction
    lcd_ll_set_data_delay_ticks(&LCD_CAM,0);
    lcd_ll_enable_interrupt(&LCD_CAM,UINT32_MAX,false); lcd_ll_clear_interrupt_status(&LCD_CAM,UINT32_MAX);
    healthy=true; return true;
}
bool parallelShow(const uint8_t* const strips[8],const int counts[8],uint8_t brightness) {
    if(!healthy) return false;
    uint8_t masks[8]; for(int i=0;i<8;++i)masks[i]=v3::channelMask(i);
    uint8_t allowed=boardDataMask();
    size_t size=l8led::encode(samples,CAPACITY,strips,counts,masks,allowed,brightness);
    if(!size) return fail();
    size_t offset=0,index=0;
    while(offset<size) {
        auto& d=descriptors[index]; size_t n=min(NODE_BYTES,size-offset);
        d.dw0.size=n;d.dw0.length=n;d.dw0.owner=DMA_DESCRIPTOR_BUFFER_OWNER_DMA;d.dw0.suc_eof=offset+n==size;
        d.buffer=samples+offset;d.next=d.dw0.suc_eof?nullptr:&descriptors[index+1];offset+=n;++index;
    }
    __sync_synchronize(); // publish all sample and descriptor writes before DMA
    lcd_ll_stop(&LCD_CAM); lcd_ll_fifo_reset(&LCD_CAM);
    lcd_ll_clear_interrupt_status(&LCD_CAM,UINT32_MAX);
    if(gdma_reset(dma)!=ESP_OK || gdma_start(dma,reinterpret_cast<intptr_t>(descriptors))!=ESP_OK) return fail();
    boardRouteLcd(allowed); // intersects the latest safety mask under routeMutex
    delayMicroseconds(1); // FIFO prefill before the peripheral starts consuming
    lcd_ll_start(&LCD_CAM);
    uint32_t start=millis();
    // Interrupts are disabled: poll RAW, not the masked interrupt-status register.
    while(!(LCD_CAM.lc_dma_int_raw.val&LCD_LL_EVENT_TRANS_DONE)) {
        if(millis()-start>60) return fail();
        vTaskDelay(1);
    }
    lcd_ll_stop(&LCD_CAM); gdma_stop(dma);
    return true;
}
#endif
