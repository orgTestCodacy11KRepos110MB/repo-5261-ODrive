
#include "stm32_gpio.hpp"

#define N_EXTI 16

struct subscription_t {
    GPIO_TypeDef* port = nullptr;
    void (*callback)(void*) = nullptr;
    void* ctx = nullptr;
} subscriptions[N_EXTI];

const Stm32Gpio Stm32Gpio::none{nullptr, 0};

/**
 * @brief Returns the IRQ number associated with a certain pin.
 * Note that all GPIOs with the same pin number map to the same IRQn,
 * no matter which port they belong to.
 */
IRQn_Type get_irq_number(uint16_t pin_number) {
    switch (pin_number) {
        case 0: return EXTI0_IRQn;
        case 1: return EXTI1_IRQn;
        case 2: return EXTI2_IRQn;
        case 3: return EXTI3_IRQn;
        case 4: return EXTI4_IRQn;
        case 5:
        case 6:
        case 7:
        case 8:
        case 9: return EXTI9_5_IRQn;
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
        case 15: return EXTI15_10_IRQn;
        default: return (IRQn_Type)0; // impossible
    }
}

#define GPIO_MODE             0x00000003U
#define GPIO_OUTPUT_TYPE      0x00000010U

#ifndef GPIO_MODER_MODER0
// HAL for STM32 H7 has slightly different names for some macros and registers
#define GPIO_MODER_MODER0 GPIO_MODER_MODE0
#define GPIO_PUPDR_PUPDR0 GPIO_PUPDR_PUPD0
#define GPIO_OSPEEDER_OSPEEDR0 GPIO_OSPEEDR_OSPEED0
#define GPIO_OTYPER_OT_0 GPIO_OTYPER_OT0
#define IMR IMR1
#define EMR EMR1
#define RTSR RTSR1
#define FTSR FTSR1
#endif

bool Stm32Gpio::config(uint32_t mode, uint32_t pull, uint32_t speed, uint32_t alternate_function) const {
    if (!enable_clock()) {
        return false;
    }
    
    size_t position = get_pin_number();

    // The following code is mostly taken from HAL_GPIO_Init
    // The reason we copy it is to avoid the extenal interrupt state getting
    // overridden.

    /* Alternate function mode selection */
    if ((mode == GPIO_MODE_AF_PP) || (mode == GPIO_MODE_AF_OD)) {
        if (!IS_GPIO_AF(alternate_function)) {
            return false;
        }

        /* Configure Alternate function mapped with the current IO */
        uint32_t temp = port_->AFR[position >> 3];
        temp &= ~((uint32_t)0xF << ((uint32_t)(position & (uint32_t)0x07) * 4)) ;
        temp |= ((uint32_t)(alternate_function) << (((uint32_t)position & (uint32_t)0x07) * 4));
        port_->AFR[position >> 3] = temp;
    }

    /* Configure IO Direction mode (Input, Output, Alternate or Analog) */
    uint32_t temp = port_->MODER;
    temp &= ~(GPIO_MODER_MODER0 << (position * 2U));
    temp |= ((mode & GPIO_MODE) << (position * 2U));
    port_->MODER = temp;

    /* In case of Output or Alternate function mode selection */
    if((mode == GPIO_MODE_OUTPUT_PP) || (mode == GPIO_MODE_AF_PP) ||
       (mode == GPIO_MODE_OUTPUT_OD) || (mode == GPIO_MODE_AF_OD))
    {
        /* Check the Speed parameter */
        if (!IS_GPIO_SPEED(speed)) {
            return false;
        }

        /* Configure the IO Speed */
        temp = port_->OSPEEDR; 
        temp &= ~(GPIO_OSPEEDER_OSPEEDR0 << (position * 2U));
        temp |= (speed << (position * 2U));
        port_->OSPEEDR = temp;

        /* Configure the IO Output Type */
        temp = port_->OTYPER;
        temp &= ~(GPIO_OTYPER_OT_0 << position) ;
        temp |= (((mode & GPIO_OUTPUT_TYPE) >> 4U) << position);
        port_->OTYPER = temp;
    }

    /* Activate the Pull-up or Pull down resistor for the current IO */
    temp = port_->PUPDR;
    temp &= ~(GPIO_PUPDR_PUPDR0 << (position * 2U));
    temp |= ((pull) << (position * 2U));
    port_->PUPDR = temp;

    return true;
}

bool Stm32Gpio::subscribe(bool rising_edge, bool falling_edge, void (*callback)(void*), void* ctx) {
    uint32_t pin_number = get_pin_number();
    if (pin_number >= N_EXTI) {
        return false; // invalid pin number
    }

    struct subscription_t& subscription = subscriptions[pin_number];

    GPIO_TypeDef* no_port = nullptr;
    if (!__atomic_compare_exchange_n(&subscription.port, &no_port, port_, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        return false; // already in use
    }

    // The following code is mostly taken from HAL_GPIO_Init
    __HAL_RCC_SYSCFG_CLK_ENABLE();

    uint32_t temp = SYSCFG->EXTICR[pin_number >> 2U];
    temp &= ~(0x0FU << (4U * (pin_number & 0x03U)));
    temp |= ((uint32_t)(GPIO_GET_INDEX(port_)) << (4U * (pin_number & 0x03U)));
    SYSCFG->EXTICR[pin_number >> 2U] = temp;

    if (rising_edge) {
        EXTI->RTSR |= (uint32_t)pin_mask_;
    } else {
        EXTI->RTSR &= ~((uint32_t)pin_mask_);
    }

    if (falling_edge) {
        EXTI->FTSR |= (uint32_t)pin_mask_;
    } else {
        EXTI->FTSR &= ~((uint32_t)pin_mask_);
    }

    EXTI->EMR &= ~((uint32_t)pin_mask_);
    EXTI->IMR |= (uint32_t)pin_mask_;

    // Clear any previous triggers
    __HAL_GPIO_EXTI_CLEAR_IT(pin_mask_);
    
    subscription.ctx = ctx;
    subscription.callback = callback;
    return true;
}

void Stm32Gpio::unsubscribe() {
    uint32_t pin_number = get_pin_number();
    if (pin_number >= N_EXTI) {
        return; // invalid pin number
    }

    struct subscription_t& subscription = subscriptions[pin_number];

    if (subscription.port != port_) {
        return; // the subscription was not for this GPIO
    }

    EXTI->IMR |= (uint32_t)pin_mask_;
    __HAL_GPIO_EXTI_CLEAR_IT(pin_mask_);

    // At this point no more interrupts will be triggered for this GPIO

    subscription.callback = nullptr;
    subscription.ctx = nullptr;
    subscription.port = nullptr; // after this line, the subscription can be reused (possibly by another thread)
}

void maybe_handle(uint16_t exti_number) {
    if(__HAL_GPIO_EXTI_GET_IT(1 << exti_number) == RESET) {
        return; // This interrupt source did not trigger the interrupt line
    }

    __HAL_GPIO_EXTI_CLEAR_IT(1 << exti_number);
    
    if (exti_number >= N_EXTI) {
        return;
    }

    subscription_t& subscription = subscriptions[exti_number];
    if (subscription.callback) {
        (*subscription.callback)(subscription.ctx);
    }
}

extern "C" {

/** @brief Entrypoint for the EXTI line 0 interrupt. */
void EXTI0_IRQHandler(void) {
    maybe_handle(0);
}

/** @brief Entrypoint for the EXTI line 1 interrupt. */
void EXTI1_IRQHandler(void) {
    maybe_handle(1);
}

/** @brief Entrypoint for the EXTI line 2 interrupt. */
void EXTI2_IRQHandler(void) {
    maybe_handle(2);
}

/** @brief Entrypoint for the EXTI line 3 interrupt. */
void EXTI3_IRQHandler(void) {
    maybe_handle(3);
}

/** @brief Entrypoint for the EXTI line 4 interrupt. */
void EXTI4_IRQHandler(void) {
    maybe_handle(4);
}

/** @brief Entrypoint for the EXTI lines 5-9 interrupt. */
void EXTI9_5_IRQHandler(void) {
    maybe_handle(5);
    maybe_handle(6);
    maybe_handle(7);
    maybe_handle(8);
    maybe_handle(9);
}

/** @brief This function handles EXTI lines 10-15 interrupt. */
void EXTI15_10_IRQHandler(void) {
    maybe_handle(10);
    maybe_handle(11);
    maybe_handle(12);
    maybe_handle(13);
    maybe_handle(14);
    maybe_handle(15);
}

}