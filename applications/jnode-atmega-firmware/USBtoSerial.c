/*
             LUFA Library
     Copyright (C) Dean Camera, 2011.

  dean [at] fourwalledcubicle [dot] com
           www.lufa-lib.org
*/

/*
  Copyright 2011  Dean Camera (dean [at] fourwalledcubicle [dot] com)

  Permission to use, copy, modify, distribute, and sell this
  software and its documentation for any purpose is hereby granted
  without fee, provided that the above copyright notice appear in
  all copies and that both that the copyright notice and this
  permission notice and warranty disclaimer appear in supporting
  documentation, and that the name of the author not be used in
  advertising or publicity pertaining to distribution of the
  software without specific, written prior permission.

  The author disclaim all warranties with regard to this
  software, including all implied warranties of merchantability
  and fitness.  In no event shall the author be liable for any
  special, indirect or consequential damages or any damages
  whatsoever resulting from loss of use, data or profits, whether
  in an action of contract, negligence or other tortious action,
  arising out of or in connection with the use or performance of
  this software.
*/

/** \file
 *
 *  Main source file for the USBtoSerial project. This file contains the main tasks of
 *  the project and is responsible for the initial application hardware configuration.
 */

#include "USBtoSerial.h"
#include "LUFA/Scheduler/Scheduler.h"
#include <avr/sleep.h>
#include "clock.h"

#define JEN_RESETN  _BV(7) /* PC7 */
#define JEN_SPIMISO _BV(3) /* PB3 */
#define JEN_SPIMOSI _BV(2) /* PB2 */
#define JEN_CLOCK   _BV(1) /* PB1 */
#define JEN_CTS     _BV(0) /* PF0 */
#define JEN_SPISS   _BV(6) /* PE6 */

/** Circular buffer to hold data from the host before it is sent to the device via the serial port. */
static RingBuffer_t USBtoUSART_Buffer;

/** Underlying data buffer for \ref USBtoUSART_Buffer, where the stored bytes are located. */
static uint8_t      USBtoUSART_Buffer_Data[1024];

/** Circular buffer to hold data from the serial port before it is sent to the host. */
static RingBuffer_t USARTtoUSB_Buffer;

/** Underlying data buffer for \ref USARTtoUSB_Buffer, where the stored bytes are located. */
static uint8_t      USARTtoUSB_Buffer_Data[1024];

/** used to toggle the reset lines of the Jennic module, entering programming
 * mode and resetting */
static volatile bool jennic_reset_event = false;

/** LUFA CDC Class driver interface configuration and state information. This structure is
 *  passed to all CDC Class driver functions, so that multiple instances of the same class
 *  within a device can be differentiated from one another.
 */
USB_ClassInfo_CDC_Device_t VirtualSerial_CDC_Interface =
  {
    .Config =
      {
        .ControlInterfaceNumber         = 0,

        .DataINEndpointNumber           = CDC_TX_EPNUM,
        .DataINEndpointSize             = CDC_TXRX_EPSIZE,
        .DataINEndpointDoubleBank       = false,

        .DataOUTEndpointNumber          = CDC_RX_EPNUM,
        .DataOUTEndpointSize            = CDC_TXRX_EPSIZE,
        .DataOUTEndpointDoubleBank      = false,

        .NotificationEndpointNumber     = CDC_NOTIFICATION_EPNUM,
        .NotificationEndpointSize       = CDC_NOTIFICATION_EPSIZE,
        .NotificationEndpointDoubleBank = false,
      },
  };


/** Main program entry point. This routine contains the overall program flow, including initial
 *  setup of all components and the main program loop.
 */
int main(void)
{
  SetupHardware();
  clock_init();

  /* pull jennic into normal mode */
  DDRC |= JEN_RESETN;
  DDRB |= JEN_SPIMISO;
  _delay_ms(5);

  DDRB &= ~JEN_SPIMISO;
  _delay_ms(10);
  DDRC &= ~JEN_RESETN;

  /* get off the spi-bus */
  DDRB &= ~JEN_SPIMISO;
  DDRB &= ~JEN_SPIMOSI;
  DDRB &= ~JEN_CLOCK;
  DDRE &= ~JEN_SPISS;

  RingBuffer_InitBuffer(&USBtoUSART_Buffer, USBtoUSART_Buffer_Data, sizeof(USBtoUSART_Buffer_Data));
  RingBuffer_InitBuffer(&USARTtoUSB_Buffer, USARTtoUSB_Buffer_Data, sizeof(USARTtoUSB_Buffer_Data));

  LEDs_SetAllLEDs(LEDMASK_USB_NOTREADY);
  sei();

  for (;;)
  {
    /* Only try to read in bytes from the CDC interface if the transmit buffer is not full */
    if (!(RingBuffer_IsFull(&USBtoUSART_Buffer)))
    {
      int16_t ReceivedByte = CDC_Device_ReceiveByte(&VirtualSerial_CDC_Interface);

      /* Read bytes from the USB OUT endpoint into the USART transmit buffer */
      if (!(ReceivedByte < 0))
        RingBuffer_Insert(&USBtoUSART_Buffer, ReceivedByte);
    }

    /* Check if the UART receive buffer flush timer has expired or the buffer is nearly full */
    uint16_t BufferCount = RingBuffer_GetCount(&USARTtoUSB_Buffer);
    if ((TIFR0 & (1 << TOV0)) || (BufferCount > (uint8_t)(sizeof(USARTtoUSB_Buffer_Data) * .75)))
    {
      /* Clear flush timer expiry flag */
      TIFR0 |= (1 << TOV0);

      /* Read bytes from the USART receive buffer into the USB IN endpoint */
      while (BufferCount--)
      {
        /* Try to send the next byte of data to the host, abort if there is an error without dequeuing */
        if (CDC_Device_SendByte(&VirtualSerial_CDC_Interface,
                                RingBuffer_Peek(&USARTtoUSB_Buffer)) != ENDPOINT_READYWAIT_NoError)
        {
          break;
        }

        RingBuffer_Remove(&USARTtoUSB_Buffer);
      }
    }

    if (jennic_reset_event)
    {
      static bool jennic_in_programming_mode = false;

      if (!jennic_in_programming_mode)
      {
        /* pull jennic into programming mode */
        DDRC |= JEN_RESETN;
        DDRB |= JEN_SPIMISO;
        _delay_ms(5);

        DDRC &= ~JEN_RESETN;
        _delay_ms(10);
        DDRB &= ~JEN_SPIMISO;
        jennic_in_programming_mode = true;
      }
      else
      {
        /* pull jennic into normal mode */
        DDRC |= JEN_RESETN;
        DDRB |= JEN_SPIMISO;
        _delay_ms(5);

        DDRB &= ~JEN_SPIMISO;
        _delay_ms(10);
        DDRC &= ~JEN_RESETN;
        jennic_in_programming_mode = false;
      }

      jennic_reset_event = false;
    }


    /* Load the next byte from the USART transmit buffer into the USART */
    if (!(RingBuffer_IsEmpty(&USBtoUSART_Buffer)))
      Serial_SendByte(RingBuffer_Remove(&USBtoUSART_Buffer));

    CDC_Device_USBTask(&VirtualSerial_CDC_Interface);
    USB_USBTask();
  }
}

/** Configures the board hardware and chip peripherals for the demo's functionality. */
void SetupHardware(void)
{
  /* Disable watchdog if enabled by bootloader/fuses */
  MCUSR &= ~(1 << WDRF);
  wdt_disable();

  /* enable clock division, run on 16MHz */
  clock_prescale_set(clock_div_1);

  /* Hardware Initialization */
  LEDs_Init();
  USB_Init();

  /* Start the flush timer so that overflows occur rapidly to push received bytes to the USB interface */
  TCCR0B = (1 << CS02);

  PORTB &= ~(JEN_CLOCK|JEN_SPIMOSI);
}

/** Event handler for the library USB Connection event. */
void EVENT_USB_Device_Connect(void)
{
  sleep_disable();
  LEDs_SetAllLEDs(LEDMASK_USB_ENUMERATING);
}

/** Event handler for the library USB Disconnection event. */
void EVENT_USB_Device_Disconnect(void)
{
  LEDs_SetAllLEDs(LEDMASK_USB_NOTREADY);

  /* go to deep-sleep */
  set_sleep_mode(SLEEP_MODE_STANDBY);
  sleep_mode();
}

/** Event handler for the library USB Configuration Changed event. */
void EVENT_USB_Device_ConfigurationChanged(void)
{
  bool ConfigSuccess = true;

  ConfigSuccess &= CDC_Device_ConfigureEndpoints(&VirtualSerial_CDC_Interface);

  LEDs_SetAllLEDs(ConfigSuccess ? LEDMASK_USB_READY : LEDMASK_USB_ERROR);
}

/** Event handler for the library USB Control Request reception event. */
void EVENT_USB_Device_ControlRequest(void)
{
  CDC_Device_ProcessControlRequest(&VirtualSerial_CDC_Interface);
}

/** ISR to manage the reception of data from the serial port, placing received bytes into a circular buffer
 *  for later transmission to the host.
 */
ISR(USART1_RX_vect, ISR_BLOCK)
{
  uint8_t ReceivedByte = UDR1;

  if (USB_DeviceState == DEVICE_STATE_Configured)
    RingBuffer_Insert(&USARTtoUSB_Buffer, ReceivedByte);
}

/** Event handler for the CDC Class driver Line Encoding Changed event.
 *
 *  \param[in] CDCInterfaceInfo  Pointer to the CDC class interface configuration structure being referenced
 */
void EVENT_CDC_Device_LineEncodingChanged(USB_ClassInfo_CDC_Device_t* const CDCInterfaceInfo)
{
  uint8_t ConfigMask = 0;

  switch (CDCInterfaceInfo->State.LineEncoding.ParityType)
  {
    case CDC_PARITY_Odd:
      ConfigMask = ((1 << UPM11) | (1 << UPM10));
      break;
    case CDC_PARITY_Even:
      ConfigMask = (1 << UPM11);
      break;
  }

  if (CDCInterfaceInfo->State.LineEncoding.CharFormat == CDC_LINEENCODING_TwoStopBits)
    ConfigMask |= (1 << USBS1);

  switch (CDCInterfaceInfo->State.LineEncoding.DataBits)
  {
    case 6:
      ConfigMask |= (1 << UCSZ10);
      break;
    case 7:
      ConfigMask |= (1 << UCSZ11);
      break;
    case 8:
      ConfigMask |= ((1 << UCSZ11) | (1 << UCSZ10));
      break;
  }

  /* Must turn off USART before reconfiguring it, otherwise incorrect operation may occur */
  UCSR1B = 0;
  UCSR1A = 0;
  UCSR1C = 0;

  /* Set the new baud rate before configuring the USART */
  UBRR1  = SERIAL_2X_UBBRVAL(CDCInterfaceInfo->State.LineEncoding.BaudRateBPS);
  UCSR1A = (1 << U2X1);

  UCSR1C = ConfigMask;
  UCSR1B = ((1 << RXCIE1) | (1 << TXEN1) | (1 << RXEN1));
}

/** Event handler for the CDC Class driver Host-to-Device Line Encoding Changed event.
 *
 *  \param[in] CDCInterfaceInfo  Pointer to the CDC class interface configuration structure being referenced
 */
#define SWITCH_TIMEOUT CLOCK_SECOND/5 /* in ms */

void EVENT_CDC_Device_ControLineStateChanged(USB_ClassInfo_CDC_Device_t* const CDCInterfaceInfo)
  /* fire a reset jennic event iff there is a high-low-high transition within
   * SWITCH_TIMEOUT ms on the dtr line */
{
  static clock_time_t previous_change = 0;
  static bool currentDTRState = false;

  /* dtr line changed? */
  if ((CDCInterfaceInfo->State.ControlLineStates.HostToDevice & CDC_CONTROL_LINE_OUT_DTR) != currentDTRState)
  {
    clock_time_t delay = clock_time()-previous_change;
    currentDTRState    = (CDCInterfaceInfo->State.ControlLineStates.HostToDevice & CDC_CONTROL_LINE_OUT_DTR);
    jennic_reset_event = currentDTRState && (delay < SWITCH_TIMEOUT);
    previous_change    = clock_time(); /* returns the usb frame clock in ms */
  }
}
