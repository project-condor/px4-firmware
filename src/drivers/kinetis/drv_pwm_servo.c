/****************************************************************************
 *
 *   Copyright (C) 2015 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/*
 * @file drv_pwm_servo.c
 *
 * Servo driver supporting PWM servos connected to kinetis flex timer modules.
 *
 */
#include <nuttx/config.h>
#include <stdio.h>
#include <errno.h>
#include <chip.h>
#include <kinetis_ftm.h>
#include <kinetis_internal.h>
#include <up_arch.h>
#include <kinetis_sim.h>

#include "drv_pwm_servo.h"


/**
 * Intialise the PWM servo outputs using the specified configuration.
 *
 * @param channel_mask	Bitmask of channels (LSB = channel 0) to enable.
 *			This allows some of the channels to remain configured
 *			as GPIOs or as another function.
 * @return		OK on success.
 */
int
up_pwm_servo_init(uint32_t channel_mask)
{
   
	for (size_t i = 0; i < PWM_SERVO_MAX_TIMERS; ++i) {
     
		const struct pwm_servo_timer* timer = &pwm_servo_timers[i];
		uint32_t regval;
     
		regval = getreg32(timer->scgc);
		putreg32(regval | timer->scgc_enable, timer->scgc); // enable clock signal
		up_pwm_servo_set_rate_group_update(i, 50); // 50Hz default
     
	}

	for (size_t i = 0; i < PWM_SERVO_MAX_CHANNELS; ++i) {
		const struct pwm_servo_channel* channel = &pwm_servo_channels[i];

		if ((1 << i) & channel_mask) {
			// enable edge aligned pwm
			putreg32(0x28, channel->timer->ftm_base + KINETIS_FTM_CSC_OFFSET(channel->ftm_channel));
			// configure pin
			kinetis_pinconfig(channel->pinconfig); 
		}
      
	}

	return 0;
}

/**
 * De-initialise the PWM servo outputs.
 */
void
up_pwm_servo_deinit(void)
{
  
}

/**
 * Arm or disarm servo outputs.
 *
 * When disarmed, servos output no pulse.
 *
 * @bug This function should, but does not, guarantee that any pulse
 *      currently in progress is cleanly completed.
 *
 * @param armed		If true, outputs are armed; if false they
 *			are disarmed.
 */
void up_pwm_servo_arm(bool armed)
{
	for (size_t i = 0; i < PWM_SERVO_MAX_TIMERS; ++i) {
		const struct pwm_servo_timer* timer = &pwm_servo_timers[i];

		uint32_t regval = getreg32(timer->ftm_base + KINETIS_FTM_SC_OFFSET);
		regval = regval & ~FTM_SC_CLKS_MASK;
    
		if (armed) {
			regval = regval | FTM_SC_CLKS_SYSCLK;
			putreg32(regval, timer->ftm_base + KINETIS_FTM_SC_OFFSET);
		} else {
			regval = regval | FTM_SC_CLKS_NONE;
			putreg32(regval, timer->ftm_base + KINETIS_FTM_SC_OFFSET);
		}
    
	}
}


/**
 * Set the servo update rate for all rate groups.
 *
 * @param rate		The update rate in Hz to set.
 * @return		OK on success, -ERANGE if an unsupported update rate is set.
 */
int
up_pwm_servo_set_rate(unsigned rate)
{
	for (size_t i = 0; i < PWM_SERVO_MAX_TIMERS; ++i) {
		up_pwm_servo_set_rate_group_update(i, rate);
	}
	return 0;
}

/**
 * Get a bitmap of output channels assigned to a given rate group.
 *
 * @param group		The rate group to query. Rate groups are assigned contiguously
 *			starting from zero.
 * @return		A bitmap of channels assigned to the rate group, or zero if
 *			the group number has no channels.
 */
uint32_t
up_pwm_servo_get_rate_group(unsigned group)
{
	return 0;
}

/**
 * Set the update rate for a given rate group.
 *
 * @param group		The rate group whose update rate will be changed.
 * @param rate		The update rate in Hz.
 * @return		OK if the group was adjusted, -ERANGE if an unsupported update rate is set.
 */
int
up_pwm_servo_set_rate_group_update(unsigned group, unsigned rate)
{

	if (group > PWM_SERVO_MAX_TIMERS) {
		return -ERANGE;
	}
  
	/*
	 * ftm rate = bus frequency / prescaler
	 * rate = ftm rate / modulo
	 *
	 * - bus 48MHz
	 * - prescaler 32
	 * - modulo 16-bit
	 * => minimum rate = 48E6/32/0xffff = 22.8 Hz
	 * => maximum rate = 48E6/32/1 => 1500000 Hz
	 * (arbitrarily limit to allow several duty cycle levels)
	 */
	if (rate < 23 || 10000 < rate) {
		return -ERANGE;
	}
    
	const struct pwm_servo_timer* timer = &pwm_servo_timers[group];
  
	uint32_t regval = getreg32(timer->ftm_base + KINETIS_FTM_SC_OFFSET);
	regval = regval & ~FTM_SC_PS_MASK;
	regval = regval | FTM_SC_PS_32;
	putreg32(regval, timer->ftm_base + KINETIS_FTM_SC_OFFSET);
	putreg32(0, timer->ftm_base + KINETIS_FTM_CNT_OFFSET);
	putreg32(0, timer->ftm_base + KINETIS_FTM_CNTIN_OFFSET);
	putreg32(
		BOARD_BUS_FREQ / 32 / rate,
		timer->ftm_base + KINETIS_FTM_MOD_OFFSET);
  
	return 0;
}

/**
 * Set the current output value for a channel.
 *
 * @param channel	The channel to set.
 * @param value		The output pulse width in microseconds.
 */
int
up_pwm_servo_set(unsigned channel, servo_position_t value)
{
	if (channel > PWM_SERVO_MAX_CHANNELS) {
		return -ERANGE;
	}
  
	const struct pwm_servo_channel* ch = &pwm_servo_channels[channel];

	putreg32(
		BOARD_BUS_FREQ / 1000000 * ((uint32_t) value) / 32,
		ch->timer->ftm_base + KINETIS_FTM_CV_OFFSET(ch->ftm_channel));
	return 0;
}

/**
 * Get the current output value for a channel.
 *
 * @param channel	The channel to read.
 * @return		The output pulse width in microseconds, or zero if
 *			outputs are not armed or not configured.
 */
servo_position_t
up_pwm_servo_get(unsigned channel)
{
	if (channel > PWM_SERVO_MAX_CHANNELS) {
		return -ERANGE;
	}
  
	const struct pwm_servo_channel* ch = &pwm_servo_channels[channel];
	uint32_t reg = ch->timer->ftm_base + KINETIS_FTM_CV_OFFSET(ch->ftm_channel);
	uint32_t mod = getreg32(reg);
	return mod * 32 / (BOARD_BUS_FREQ/1000000);
}
