// USE PA9 and PA10
#include "sha256.h"
#include "uECC.h"
#include <stddef.h>
#include <stdint.h>
#define RCC_BASE 0x40023800
#define GPIOA_BASE 0x40020000
#define USART1_BASE 0x40011000
#define FLASH_BASE 0x40023C00
#define SYSCFG_BASE 0x40013800
// Don't change this
#define SCB_CPACR (*(volatile uint32_t *)0xE000ED88)
// Timers STUFF
#define STK_CTRL (*(volatile uint32_t *)(0xE000E010))
#define STK_LOAD (*(volatile uint32_t *)(0xE000E014))
#define STK_VAL (*(volatile uint32_t *)(0xE000E018))
#define STK_CALIB (*(volatile uint32_t *)(0xE000E01C))
// Set to CPU clock
#define SYSCLK_HZ 84000000UL
#define SYSTICK_LOAD (SYSCLK_HZ / 1000UL - 1UL)

#define USART1_SR (*(volatile uint32_t *)(USART1_BASE + 0x00))
#define USART1_DR (*(volatile uint32_t *)(USART1_BASE + 0x04))
#define USART1_BRR (*(volatile uint32_t *)(USART1_BASE + 0x08))
#define USART1_CR1 (*(volatile uint32_t *)(USART1_BASE + 0x0C))

#define GPIOA_MODER (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_AFRH (*(volatile uint32_t *)(GPIOA_BASE + 0x24))
#define GPIOA_OSPEEDR (*(volatile uint32_t *)(GPIOA_BASE + 0x08))

#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104)

#define FLASH_ACR (*(volatile uint32_t *)(FLASH_BASE + 0x00))

#define RCC_AHB1ENR (*(volatile uint32_t *)(RCC_BASE + 0x30))
#define RCC_APB2ENR (*(volatile uint32_t *)(RCC_BASE + 0x44))
#define RCC_PLLCFGR (*(volatile uint32_t *)(RCC_BASE + 0x04))
#define RCC_CR (*(volatile uint32_t *)(RCC_BASE + 0x00))
#define RCC_CFGR (*(volatile uint32_t *)(RCC_BASE + 0x08))
#define MAX_LORA_BUFFER 64
#define MY_KEY 748545
#define OTHER_KEY 145874
volatile uint32_t ms_ticks;
void start(void);
void Reset_Handler(void) {
  // Enable FPU
  SCB_CPACR |= (0xF << 20);
  __asm volatile("dsb");
  __asm volatile("isb");

  FLASH_ACR &= ~(0xF << 0);  // clear everything
  FLASH_ACR |= (2U << 0);    // set 0010 to latecy
  FLASH_ACR |= (0b111 << 8); // Enable PRFTEN, ICEN, DCEN
  RCC_CR |= (1U << 0);       // HSION
  while (((RCC_CR & 0b10) >> 1) == 0)
    ;
  RCC_CR &= ~(1U << 24);
  RCC_PLLCFGR &= ~(0x3f << 0);
  RCC_PLLCFGR |= (0x8 << 0); // PLLM: 8
  RCC_PLLCFGR &= ~(0x1FF << 6);
  RCC_PLLCFGR |= (168U << 6); // PLLN: 168
  RCC_PLLCFGR &= ~(0x3 << 16);
  RCC_PLLCFGR |= (0b01 << 16); // PLLP: 4(0b01)
  RCC_PLLCFGR &= ~(1U << 22);
  RCC_CFGR &= ~(0x7 << 10);
  RCC_CFGR |= (0x4 << 10);
  RCC_CFGR &= ~(0x7 << 13);
  RCC_CR |= (1U << 24);
  while (!(RCC_CR & (1U << 25)))
    ;
  RCC_CFGR &= ~(3U << 0);
  RCC_CFGR |= (2U << 0);
  while (((RCC_CFGR >> 2) & 3U) != 2)
    ;
  STK_LOAD = SYSTICK_LOAD;
  STK_VAL = 0;
  STK_CTRL = (1U << 2) | (1U << 1) | (1U << 0);
  NVIC_ISER1 |= (1U << 5);
  __asm volatile("cpsie i");
  start();
}
uint32_t millis(void) { return ms_ticks; }
void SysTick_Handler(void) { ms_ticks++; }
static inline int elapsed(uint32_t start_ms, uint32_t duration_ms) {
  return (millis() - start_ms) >= duration_ms;
}
void uart_send_char(char txt) {
  while (!(USART1_SR & (1U << 7)))
    ;
  USART1_DR = txt;
}
void uart_send(char *txt, size_t len) {
  for (size_t i = 0; i < len - 1; i++) {
    uart_send_char(txt[i]);
  }
  uart_send_char('\r');
  uart_send_char('\n');
}
volatile char BUFFER[MAX_LORA_BUFFER];
volatile uint8_t complete;
volatile uint8_t overflow;
volatile uint8_t buffer_index = 0;
volatile char last_char = '\0';
// return 0 if string is equal
int16_t str_cmp(volatile char *s1, char *s2) {
  while (*s1 && !(*s1 - *s2)) {
    s1++;
    s2++;
  }
  return *s1 - *s2;
}
void USART1_IRQHandler() {
  if (USART1_SR & (1U << 5) && overflow == 0) { // RXNE flag
    char c = (char)USART1_DR;
    if (buffer_index >= MAX_LORA_BUFFER - 1) {
      overflow = 1;
      return; // let user handle overflow first.
    }
    if (complete) {
      return;
    }
    BUFFER[buffer_index++] = c;
    if (c == '\n' && last_char == '\r') {
      complete = 1;
      buffer_index -= 2;
      BUFFER[buffer_index] = '\0';
    } else {
      last_char = c;
    }
  }
}
enum SYSTEM_STATE {
  STATE_IDLE,
  STATE_ERROR,
  STATE_CONNECTED,
  STATE_DISCONNECTED,
};
enum HANDSHAKE_EVENT {
  EVENT_NONE = -1,
  EVENT_PUBLIC_KEY,
  EVENT_TEST,
  EVENT_SUCCESS
};
static int dummy_rng(uint8_t *dest, unsigned size) {
  for (unsigned i = 0; i < size; i++) {
    dest[i] = 0xAA;
  }
  return 1;
}
uint8_t private_key[32];
uint8_t public_key[64];
static void setup_crypto() {
  uECC_set_rng(&dummy_rng);
  uECC_make_key(public_key, private_key, uECC_secp256r1());
}
void start(void) {
  RCC_AHB1ENR |= (1 << 0); // Enable GPIOA
  RCC_APB2ENR |= (1 << 4); // Enable USART1
  // We use PA10 and PA9 and we want it in alternate function
  GPIOA_MODER &= ~(3U << 18 | 3U << 20); // Reset to 00
  GPIOA_MODER |= (2U << 18 | 2U << 20);  // set to alternate function (0b10) = 2
  // We want AF7 (USART 1)

  GPIOA_AFRH &= ~(0xFF << 4);
  GPIOA_AFRH |= (7U << 4 | 7U << 8);
  GPIOA_OSPEEDR |= (3U << 18) | (3U << 20);
  USART1_BRR = 0x2D9;
  USART1_CR1 |= (3U << 2); // Enable RE and TE
  USART1_CR1 |= (1U << 5);
  USART1_CR1 |= (1U << 13); // Enable USART
  enum SYSTEM_STATE state = STATE_IDLE;
  enum HANDSHAKE_EVENT state_event = EVENT_NONE;
  setup_crypto();
  uint32_t timer_start = millis();
  uart_send("HELLO", 6);
  char *str = (char *)public_key;
  uint8_t pub_1st = 1;
  int pub_counter = 3;
  while (1) {
    // TODO: Handle overflow properly
    if (overflow) {
      overflow = 0;
      complete = 0;
      buffer_index = 0;
    }
    switch (state) {
    case STATE_IDLE: {

      switch (state_event) {
      case EVENT_PUBLIC_KEY: {
        if ((elapsed(timer_start, 30 * 1000) || pub_1st) && (pub_counter > 0)) {
          timer_start = millis();
          pub_1st = 0;
          uart_send(str, sizeof(public_key) + 1);
          pub_counter--;
        } else {
          state_event = EVENT_NONE;
          pub_1st = 1;
          pub_counter = 3;
        }
        break;
      default:
        break;
      }
      }

      if (elapsed(timer_start, 30 * 1000) && complete == 0) {
        timer_start = millis();
        uart_send("HELLO", 6);
      }
      if (complete) {
        if (!str_cmp(BUFFER, "HI")) {
          uart_send("HELLO_ACK", sizeof("HELLO_ACK"));
          state_event = EVENT_PUBLIC_KEY;
        }
        complete = 0;
        buffer_index = 0;
      }
      break;
    }
    case STATE_CONNECTED: {
      break;
    }
    }
  }
}

extern uint32_t _estack;
void default_handler(void) {
  while (1)
    ;
}
__attribute__((section(".isr_vector"), used)) const void *vector_table[] = {
    &_estack,        Reset_Handler,     default_handler, default_handler,
    default_handler, default_handler,   default_handler, default_handler,
    default_handler, default_handler,   default_handler, default_handler,
    default_handler, default_handler,   default_handler, SysTick_Handler,
    default_handler, default_handler,   default_handler, default_handler,
    default_handler, default_handler,   default_handler, default_handler,
    default_handler, default_handler,   default_handler, default_handler,
    default_handler, default_handler,   default_handler, default_handler,
    default_handler, default_handler,   default_handler, default_handler,
    default_handler, default_handler,   default_handler, default_handler,
    default_handler, default_handler,   default_handler, default_handler,
    default_handler, default_handler,   default_handler, default_handler,
    default_handler, default_handler,   default_handler, default_handler,
    default_handler, USART1_IRQHandler,
};
