/****************************************************************************
 *
 *   Copyright (C) 2013 PX4 Development Team. All rights reserved.
 *   Author: Pavel Kirienko (pavel.kirienko@gmail.com)
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

#include <ch.h>
#include <hal.h>
#include <unistd.h>
#include <assert.h>
#include <math.h>
#include <stm32f10x.h>
#include "pwm.h"
#include "adc.h"
#include "timer.h"

#undef TIM3
#undef TIM4
#undef TIM5
#undef TIM6
#undef TIM7

#define PWM_TIMER_FREQUENCY     STM32_TIMCLK2

#define PWM_DEAD_TIME_NANOSEC   400

/**
 * Local constants, initialized once
 */
static uint16_t _pwm_top;
static uint16_t _pwm_half_top;
static uint16_t _pwm_min;

static bool _prevent_full_duty_cycle_bump = true; /// TODO IMPLEMENT LATER


static int init_constants(unsigned frequency)
{
	assert_always(_pwm_top == 0);   // Make sure it was not initialized already

	/*
	 * PWM top, derived from frequency
	 */
	if (frequency < MOTOR_PWM_MIN_FREQUENCY || frequency > MOTOR_PWM_MAX_FREQUENCY) {
		return -1;
	}

	const int pwm_steps = PWM_TIMER_FREQUENCY / (2 * frequency);
	if (pwm_steps > 65536) {
		assert(0);
		return -1;
	}

	_pwm_top = pwm_steps - 1;
	_pwm_half_top = pwm_steps / 2;

	const int effective_steps = pwm_steps / 2;
	const float true_frequency = PWM_TIMER_FREQUENCY / (float)(pwm_steps * 2);
	const unsigned adc_period_usec = motor_adc_sampling_period_hnsec() / HNSEC_PER_USEC;
	lowsyslog("Motor: PWM freq: %f; Effective steps: %i; ADC period: %u usec\n",
		true_frequency, effective_steps, adc_period_usec);

	/*
	 * PWM min - Limited by MOTOR_ADC_SAMPLE_WINDOW_NANOSEC
	 */
	const float pwm_clock_period = 1.f / PWM_TIMER_FREQUENCY;
	const float pwm_adc_window_len = (MOTOR_ADC_SAMPLE_WINDOW_NANOSEC / 1e9f) * 2;
	const float pwm_adc_window_ticks_float = pwm_adc_window_len / pwm_clock_period;
	assert(pwm_adc_window_ticks_float >= 0);

	// We need to divide the min value by 2 since we're using the center-aligned PWM
	uint16_t pwm_half_adc_window_ticks = ((uint16_t)pwm_adc_window_ticks_float) / 2;
	if (pwm_half_adc_window_ticks > _pwm_half_top)
		pwm_half_adc_window_ticks = _pwm_half_top;

	_pwm_min = pwm_half_adc_window_ticks;
	assert(_pwm_min <= _pwm_half_top);

	lowsyslog("Motor: PWM range [%u; %u]\n", (unsigned)_pwm_min, (unsigned)_pwm_top);
	return 0;
}

static void init_timers(void)
{
	assert_always(_pwm_top > 0);   // Make sure it was initialized

	chSysDisable();

	// TIM1
	RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
	RCC->APB2RSTR |=  RCC_APB2RSTR_TIM1RST;
	RCC->APB2RSTR &= ~RCC_APB2RSTR_TIM1RST;

	// TIM2
	RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
	RCC->APB1RSTR |=  RCC_APB1RSTR_TIM2RST;
	RCC->APB1RSTR &= ~RCC_APB1RSTR_TIM2RST;

	chSysEnable();

	// Reload value
	TIM2->ARR = TIM1->ARR = _pwm_top;

	// Buffered update, center-aligned PWM (will be enabled later)
	TIM2->CR1 = TIM1->CR1 = TIM_CR1_ARPE | TIM_CR1_CMS_0;

	// Output idle state 0, buffered updates
	TIM1->CR2 = TIM_CR2_CCUS | TIM_CR2_CCPC;

	/*
	 * OC channels
	 * TIM1 CC1, CC2, CC3 are used to control the FETs; TIM1 CC4 is not used.
	 * TIM2 CC2 is used to trigger the ADC conversion.
	 */
	// Phase A, phase B
	TIM1->CCMR1 =
		TIM_CCMR1_OC1FE | TIM_CCMR1_OC1PE | TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2 |
		TIM_CCMR1_OC2FE | TIM_CCMR1_OC2PE | TIM_CCMR1_OC2M_1 | TIM_CCMR1_OC2M_2;

	// Phase C
	TIM1->CCMR2 =
		TIM_CCMR2_OC3FE | TIM_CCMR2_OC3PE | TIM_CCMR2_OC3M_1 | TIM_CCMR2_OC3M_2;

	// ADC sync
	TIM2->CCMR1 =
		TIM_CCMR1_OC2FE | TIM_CCMR1_OC2PE | TIM_CCMR1_OC2M_1 | TIM_CCMR1_OC2M_2;

	// OC polarity (no inversion, all disabled except ADC sync)
	TIM1->CCER = 0;
	TIM2->CCER = TIM_CCER_CC2E;

	/*
	 * Dead time generator setup.
	 * DTS clock divider set 0, hence fDTS = input clock.
	 * DTG bit 7 must be 0, otherwise it will change multiplier which is not supported yet.
	 * At 72 MHz one tick ~ 13.9 nsec, max 127 * 13.9 ~ 1.764 usec, which is large enough.
	 */
	const float pwm_dead_time = PWM_DEAD_TIME_NANOSEC / 1e9f;
	const float pwm_dead_time_ticks_float = pwm_dead_time / (1.f / PWM_TIMER_FREQUENCY);
	assert(pwm_dead_time_ticks_float > 0);
	assert(pwm_dead_time_ticks_float < (_pwm_top * 0.2f));

	uint16_t dead_time_ticks = (uint16_t)pwm_dead_time_ticks_float;
	if (dead_time_ticks > 127) {
		assert(0);
		dead_time_ticks = 127;
	}
	lowsyslog("Motor: PWM dead time %u ticks\n", (unsigned)dead_time_ticks);

	TIM1->BDTR = TIM_BDTR_AOE | TIM_BDTR_MOE | dead_time_ticks;

	/*
	 * ADC synchronization
	 *
	 * 100%     /\      /
	 * 75%     /  \    /
	 * 25%    /    \  /
	 * 0%    /      \/
	 *             A  B
	 * Complementary PWM operates in the range (50%, 100%], thus a FET commutation will never happen in
	 * between A and B on the diagram above (save the braking mode, but it's negligible).
	 * ADC shall be triggered either at PWM top or PWM bottom in order to catch the moment when the instant
	 * winding current matches with average winding current - this helps to eliminate the current ripple
	 * caused by the PWM switching. Thus we trigger ADC at the bottom minus advance.
	 * Refer to "Synchronizing the On-Chip Analog-to-Digital Converter on 56F80x Devices" for some explanation.
	 */
	const float adc_trigger_advance = MOTOR_ADC_SYNC_ADVANCE_NANOSEC / 1e9f;
	const float adc_trigger_advance_ticks_float = adc_trigger_advance / (1.f / PWM_TIMER_FREQUENCY);
	assert(adc_trigger_advance_ticks_float >= 0);
	assert(adc_trigger_advance_ticks_float < (_pwm_top * 0.4f));
	TIM2->CCR2 = (uint16_t)adc_trigger_advance_ticks_float;
	if (TIM2->CCR2 == 0) {
		TIM2->CCR2 = 1;
	}

	// Timers are configured now but not started yet. Starting is tricky because of synchronization, see below.
	TIM1->EGR = TIM_EGR_UG | TIM_EGR_COMG;
	TIM2->EGR = TIM_EGR_UG | TIM_EGR_COMG;
}

static void start_timers(void)
{
	const irqstate_t irqstate = irq_primask_save();

	// Make sure the timers are not running
	assert_always(!(TIM1->CR1 & TIM_CR1_CEN));
	assert_always(!(TIM2->CR1 & TIM_CR1_CEN));

	// Start synchronously
	TIM1->CR2 |= TIM_CR2_MMS_0;                   // TIM1 is master
	TIM2->SMCR = TIM_SMCR_SMS_1 | TIM_SMCR_SMS_2; // TIM2 is slave

	TIM1->CR1 |= TIM_CR1_CEN;                     // Start all

	// Remove the synchronization
	TIM2->SMCR = 0;

	// Make sure the timers have started now
	assert_always(TIM1->CR1 & TIM_CR1_CEN);
	assert_always(TIM2->CR1 & TIM_CR1_CEN);

	irq_primask_restore(irqstate);
}

int motor_pwm_init(unsigned frequency, bool prevent_full_duty_cycle_bump)
{
	const int ret = init_constants(frequency);
	if (ret)
		return ret;

	_prevent_full_duty_cycle_bump = prevent_full_duty_cycle_bump;
	lowsyslog("Motor: PWM bump suppression: %i\n", (int)_prevent_full_duty_cycle_bump);

	init_timers();
	start_timers();

	motor_pwm_set_freewheeling();
	return 0;
}

void motor_pwm_prepare_to_start(void)
{
	// High side drivers cap precharge
	const enum motor_pwm_phase_manip cmd[3] = {
		MOTOR_PWM_MANIP_LOW,
		MOTOR_PWM_MANIP_LOW,
		MOTOR_PWM_MANIP_LOW
	};
	motor_pwm_manip(cmd);
	usleep(50);
	motor_pwm_set_freewheeling();
}

uint32_t motor_adc_sampling_period_hnsec(void)
{
	return HNSEC_PER_SEC / (PWM_TIMER_FREQUENCY / (((int)_pwm_top + 1) * 2));
}

/**
 * Safely turns off all phases avoiding shoot-through.
 * Assumes:
 *  - motor IRQs are disabled
 */
static void phase_reset_all_i(void)
{
	TIM1->CCER = 0;
	TIM1->EGR = TIM_EGR_COMG;
}

/**
 * Safely turns off one phase.
 * Assumes:
 *  - motor IRQs are disabled
 */
__attribute__((optimize(3)))
static inline void phase_reset_i(uint_fast8_t phase)
{
	if (phase == 0) {
		TIM1->CCER &= ~(TIM_CCER_CC1E | TIM_CCER_CC1NE);
	} else if (phase == 1) {
		TIM1->CCER &= ~(TIM_CCER_CC2E | TIM_CCER_CC2NE);
	} else {
		TIM1->CCER &= ~(TIM_CCER_CC3E | TIM_CCER_CC3NE);
	}
}

/**
 * Assumes:
 *  - motor IRQs are disabled
 */
__attribute__((optimize(3)))
static inline void phase_set_i(uint_fast8_t phase, uint_fast16_t pwm_val, bool inverted)
{
	if (inverted) {
		pwm_val = _pwm_top - pwm_val;
	}
	assert(pwm_val <= _pwm_top);

	if (phase == 0) {
		TIM1->CCR1 = pwm_val;
		TIM1->CCER |= (TIM_CCER_CC1E | TIM_CCER_CC1NE);
	} else if (phase == 1) {
		TIM1->CCR2 = pwm_val;
		TIM1->CCER |= (TIM_CCER_CC2E | TIM_CCER_CC2NE);
	} else {
		TIM1->CCR3 = pwm_val;
		TIM1->CCER |= (TIM_CCER_CC3E | TIM_CCER_CC3NE);
	}
}

static inline void apply_phase_config(void)
{
	TIM1->EGR = TIM_EGR_COMG;
}

void motor_pwm_manip(const enum motor_pwm_phase_manip command[MOTOR_NUM_PHASES])
{
	irq_primask_disable();
	phase_reset_all_i();
	irq_primask_enable();

	for (int phase = 0; phase < MOTOR_NUM_PHASES; phase++) {
		if (command[phase] == MOTOR_PWM_MANIP_HIGH) {
			// We don't want to engage 100% duty cycle because the high side pump needs switching
			const int pwm_val = motor_pwm_compute_pwm_val(0.95f);
			irq_primask_disable();
			phase_set_i(phase, pwm_val, false);
			irq_primask_enable();
		} else if (command[phase] == MOTOR_PWM_MANIP_HALF) {
			irq_primask_disable();
			phase_set_i(phase, _pwm_half_top, false);
			irq_primask_enable();
		} else if (command[phase] == MOTOR_PWM_MANIP_LOW) {
			irq_primask_disable();
			phase_set_i(phase, 0, false);
			irq_primask_enable();
		} else if (command[phase] == MOTOR_PWM_MANIP_FLOATING) {
			// Nothing to do
		} else {
			assert(0);
		}
	}

	apply_phase_config();
}

void motor_pwm_align(const int polarity[MOTOR_NUM_PHASES], int pwm_val)
{
	irq_primask_disable();
	phase_reset_all_i();
	irq_primask_enable();

	for (int phase = 0; phase < MOTOR_NUM_PHASES; phase++) {
		const int pol = polarity[phase];
		if (pol != 0) {
			continue;
		}

		assert(pol == 1 || pol == -1);
		const bool reverse = pol < 0;

		irq_primask_disable();
		phase_set_i(phase, pwm_val, reverse);
		irq_primask_enable();
	}

	apply_phase_config();
}

void motor_pwm_set_freewheeling(void)
{
	irq_primask_disable();
	phase_reset_all_i();
	irq_primask_enable();
}

void motor_pwm_emergency(void)
{
	const irqstate_t irqstate = irq_primask_save();
	phase_reset_all_i();
	irq_primask_restore(irqstate);
}

int motor_pwm_compute_pwm_val(float duty_cycle)
{
	/*
	 * Normalize into [0; PWM_TOP] regardless of sign
	 */
	const float abs_duty_cycle = fabs(duty_cycle);
	uint_fast16_t int_duty_cycle = 0;
	if (abs_duty_cycle > 0.999)
		int_duty_cycle = _pwm_top;
	else
		int_duty_cycle = (uint_fast16_t)(abs_duty_cycle * _pwm_top);

	/*
	 * Compute the complementary duty cycle
	 * Ref. "Influence of PWM Schemes and Commutation Methods for DC and Brushless Motors and Drives", page 4.
	 */
	int output = 0;

	if (duty_cycle >= 0) {
		// Forward mode
		output = _pwm_top - ((_pwm_top - int_duty_cycle) / 2);

		assert(output >= _pwm_half_top);
		assert(output <= _pwm_top);
	} else {
		// Braking mode
		output = (_pwm_top - int_duty_cycle) / 2;

		if (output < _pwm_min)
			output = _pwm_min;

		assert(output >= 0);
		assert(output <= _pwm_half_top);
	}

	return output;
}

__attribute__((optimize(3)))
void motor_pwm_set_step_from_isr(const struct motor_pwm_commutation_step* step, int pwm_val)
{
	phase_reset_i(step->floating);

	phase_set_i(step->positive, pwm_val, false);
	phase_set_i(step->negative, pwm_val, true);

	apply_phase_config();
}

void motor_pwm_beep(int frequency, int duration_msec)
{
	static const float DUTY_CYCLE = 0.008;
	static const int ACTIVE_USEC_MAX = 20;

	motor_pwm_set_freewheeling();

	frequency = (frequency < 100)  ? 100  : frequency;
	frequency = (frequency > 5000) ? 5000 : frequency;

	duration_msec = (duration_msec < 1)    ? 1    : duration_msec;
	duration_msec = (duration_msec > 5000) ? 5000 : duration_msec;

	/*
	 * Timing constants
	 */
	const int half_period_hnsec = (HNSEC_PER_SEC / frequency) / 2;

	int active_hnsec = half_period_hnsec * DUTY_CYCLE;

	if (active_hnsec > ACTIVE_USEC_MAX * HNSEC_PER_USEC)
		active_hnsec = ACTIVE_USEC_MAX * HNSEC_PER_USEC;

	const int idle_hnsec = half_period_hnsec - active_hnsec;

	const uint64_t end_time = motor_timer_hnsec() + duration_msec * HNSEC_PER_MSEC;

	/*
	 * FET round robin
	 * This way we can beep even if some FETs went bananas
	 */
	static unsigned _phase_sel;
	const int low_phase_first  = _phase_sel++ % MOTOR_NUM_PHASES;
	const int low_phase_second = _phase_sel++ % MOTOR_NUM_PHASES;
	const int high_phase       = _phase_sel++ % MOTOR_NUM_PHASES;
	_phase_sel++;              // We need to increment it not by multiple of 3
	assert(low_phase_first != high_phase && low_phase_second != high_phase);

	/*
	 * Commutations
	 * No high side pumping
	 */
	phase_set_i(low_phase_first, 0, false);
	phase_set_i(low_phase_second, 0, false);
	phase_set_i(high_phase, 0, false);

	while (end_time > motor_timer_hnsec()) {
		chSysSuspend();

		phase_set_i(high_phase, _pwm_top, false);
		motor_timer_hndelay(active_hnsec);
		phase_set_i(high_phase, 0, false);

		chSysEnable();

		motor_timer_hndelay(idle_hnsec);
	}

	motor_pwm_set_freewheeling();
}