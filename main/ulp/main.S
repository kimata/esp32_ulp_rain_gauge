/*
 * ESP32 ULP Program
 *
 * Copyright (C) 2018 KIMATA Tetsuya <kimata@green-rabbit.net>
 *
 * This program is free software ; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2.
 */

#include "soc/rtc_cntl_reg.h"
#include "soc/rtc_io_reg.h"
#include "soc/soc_ulp.h"

#include "util_macro.S"

.set LOW_HYST_BIT,7
.set SENSE_BUF_SIZE,256
    
    .bss
    .global gpio_num
gpio_num: .long 0

    // 0: previous state is high and detect down edge
    // 1: previous state is low and check high level continuity
    // 2: previous state low
    .global watch_state
watch_state: .long 0

    .global gpio_level
gpio_level: .long 0
    
    .global edge_count
edge_count: .long 0
    
    .global continue_count
continue_count: .long 0

    .global predecessing_zero
predecessing_zero: .long 0
    
    .global sense_count
sense_count: .long 0

    .balign 4
    .global sense_data
sense_data: .long 0
    .skip SENSE_BUF_SIZE
    
stack:
    .skip 32
    .global stackEnd
stackEnd:
    .long 0

    .text
    .global entry
entry:
    move r3,stackEnd

loop:
    psr
    jump watch
    jump loop

watch:
    READ_RTC_FIELD(RTC_GPIO_IN_REG, RTC_GPIO_IN_NEXT)
    move r1, gpio_num
    ld r1, r1, 0
    rsh r0, r0, r1
    and r0, r0, 1

    move r1, gpio_level
    st r0, r1, 0
    
    push r0
    
    move r1, watch_state
    ld r2, r1, 0

    and r0, r2, 1

    // state == 1
    jumpr watch_state_1, 1, ge

    rsh r2, r2, 1
    and r0, r2, 1

    // state == 2
    jumpr watch_state_2, 1, ge
    jump watch_state_0
watch_end:
    ret
    
    ////////////////////////////////////////////////////////////
    // previous state is high and detect down edge
watch_state_0:
    pop r0 // gpio level
    jumpr _state_0_to_2, 1, lt
    ret
    
_state_0_to_2: // down edge detected
    //  increase edge count
    move r1, edge_count
    ld r2, r1, 0
    add r2, r2, 1
    st r2, r1, 0

    // change state
    move r1, watch_state
    move r2, 2
    st r2, r1, 0
    ret

    ////////////////////////////////////////////////////////////
    // previous state is low and check high state continuity
watch_state_1: 
    pop r0 // gpio level
    jumpr _state_1_to_2, 1, lt
    
    //  increase high continuity count
    move r1, continue_count
    ld r2, r1, 0
    add r2, r2, 1
    st r2, r1, 0

    rsh r0, r2, LOW_HYST_BIT
    jumpr _state_1_to_0, 1, ge
    jump watch_end

_state_1_to_0:
    // change state
    move r1, watch_state
    move r2, 0
    st r2, r1, 0
    jump watch_end
    
_state_1_to_2:
    //  reset high continuity count
    move r1, continue_count
    move r2, 2
    st r2, r1, 0
    jump watch_end

    ////////////////////////////////////////////////////////////
    // previous state low
watch_state_2:
    pop r0 // gpio level
    jumpr _state_2_to_1, 1, ge
    jump watch_end
    
_state_2_to_1:
    //  reset high continuity count
    move r1, continue_count
    move r2, 0
    st r2, r1, 0

    // change state
    move r1, watch_state
    move r2, 1
    st r2, r1, 1
    jump watch_end
    
wake_cpu:
    wake
    // stop wake up timer
    WRITE_RTC_FIELD(RTC_CNTL_STATE0_REG, RTC_CNTL_ULP_CP_SLP_TIMER_EN, 0)

exit:   
    halt
    
// Local Variables:
// mode: asm
// comment-start: "// "
// End:     
