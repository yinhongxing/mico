/*
 * $ Copyright Broadcom Corporation $
 */
/*******************************************************************************
 **  Name:       userial_bby.c
 **
 **  Description:
 **
 **  This file contains the universal driver wrapper for the BTE-QC serial
 **  drivers
 **
 **  Copyright (c) 2001-2004, WIDCOMM Inc., All Rights Reserved.
 **  WIDCOMM Bluetooth Core. Proprietary and confidential.
 *******************************************************************************/
#include <string.h>
#include "bt_target.h"
#include "gki.h"
#include "userial.h"
#include "mico.h"
#include "hcidefs.h"
#include "btm_cfg.h"
#include "bte.h"

#include "platform_config.h"
#include "bt_platform_common.h"

#if( !defined( check_noerr ) )
    #define check_noerr( X )                                                                                  \
        do                                                                                              \
        {                                                                                               \
            if( unlikely( (X) ) )                                                                      \
            {                                                                                           \
                debug_print_assert( 0, #X, NULL, __FILE__, __LINE__, __PRETTY_FUNCTION__ );             \
            }                                                                                           \
                                                                                                        \
        }   while( 1==0 )
#endif


/* Macro for checking of bus is initialised */
#define IS_BUS_INITIALISED( ) \
do \
{ \
    if ( bus_initialised == false ) \
    { \
        printf( "bus uninitialised" ); \
        return -1; \
    } \
}while ( 0 )

/* Macro for checking if bus is ready */
#define BT_BUS_IS_READY( ) \
do \
{ \
    if ( bt_bus_is_ready( ) == false ) \
    { \
        printf( "bus not ready" ) \
        return kGeneralErr; \
    } \
}while ( 0 )

/* Macro for waiting until bus is ready */
#define BT_BUS_WAIT_UNTIL_READY( ) \
do \
{ \
    while ( bt_bus_is_ready( ) == false ) \
    { \
        mico_thread_msleep( 10 ); \
    } \
} while ( 0 )

/* TODO: bring in bt_bus code to remove BTE dependency on wiced bluetooth library */
extern int bt_bus_init( void );
extern int bt_bus_deinit( void );
extern int bt_bus_transmit( const uint8_t* data_out, uint32_t size );
extern int bt_bus_receive( uint8_t* data_in, uint32_t size, uint32_t timeout_ms );
extern int bt_bus_uart_reset( void );
extern int bt_bus_uart_reconifig_baud(uint32_t baud);
extern bool bt_bus_is_ready( void );
static uint32_t userial_baud_tbl[] = {
    300, /* USERIAL_BAUD_300          0 */
    600, /* USERIAL_BAUD_600          1 */
    1200, /* USERIAL_BAUD_1200         2 */
    2400, /* USERIAL_BAUD_2400         3 */
    9600, /* USERIAL_BAUD_9600         4 */
    19200, /* USERIAL_BAUD_19200        5 */
    57600, /* USERIAL_BAUD_57600        6 */
    115200, /* USERIAL_BAUD_115200       7 */
    230400, /* USERIAL_BAUD_230400       8 */
    460800, /* USERIAL_BAUD_460800       9 */
    921600, /* USERIAL_BAUD_921600       10 */
    1000000, /* USERIAL_BAUD_1M           11 */
    1500000, /* USERIAL_BAUD_1_5M         12 */
    2000000, /* USERIAL_BAUD_2M           13 */
    3000000, /* USERIAL_BAUD_3M           14 */
    4000000 /* USERIAL_BAUD_4M           15 */
};

mico_thread_t read_thread_id;
#define READ_THREAD_STACK_SIZE      0x1000
#define READ_THREAD_NAME            ((INT8 *) "READ_THREAD")
//#define READ_THREAD_PRI             5
#define READ_THREAD_PRI             8   /* test: lower than player task */
// #define READ_LIMIT               (GKI_BUF3_SIZE-BT_HDR_SIZE)
// #define HCISU_EVT                   EVENT_MASK(APPL_EVENT_0)

#define HCI_TYPE_COMMAND            1
#define HCI_TYPE_ACL_DATA           2
#define HCI_TYPE_SCO_DATA           3
#define HCI_TYPE_EVENT              4

BUFFER_Q Userial_in_q;
static BT_HDR *pbuf_USERIAL_Read = NULL;

void userial_read_thread( );
UINT8 g_readThreadAlive = 1;

/* USERIAL control block */
typedef struct
{
    tUSERIAL_CBACK *p_cback;
} tUSERIAL_CB;
tUSERIAL_CB userial_cb;



/******************************************************
 *                   Enumerations
 ******************************************************/
#define USERIAL_WICED_HCI_UART  (MICO_UART_2)


/* HCI Transport Layer Packet Type */
typedef enum
{
    HCI_UNINITIALIZED = 0x00, // Uninitialized
    HCI_COMMAND_PACKET = 0x01, // HCI Command packet from Host to Controller
    HCI_ACL_DATA_PACKET = 0x02, // Bidirectional Asynchronous Connection-Less Link (ACL) data packet
    HCI_SCO_DATA_PACKET = 0x03, // Bidirectional Synchronous Connection-Oriented (SCO) link data packet
    HCI_EVENT_PACKET = 0x04, // Events
    HCI_LOOPBACK_MODE = 0xFF,
// Loopback mode
// HCI Event packet from Controller to Host
} hci_packet_type_t;

typedef struct
{
    hci_packet_type_t packet_type; /* This is transport layer packet type. Not transmitted if transport bus is USB */
    uint8_t event_code;
    uint8_t content_length;
} hci_event_header_t;

typedef struct
{
    hci_packet_type_t packet_type; /* This is transport layer packet type. Not transmitted if transport bus is USB */
    uint16_t hci_handle;
    uint16_t content_length;
} hci_acl_packet_header_t;


/*****************************************************************************
 * Platform UART interface, taken from
 * ../Library/bluetooth/internal/bus/UART/bt_bus.c
 * (audio/2.4.x-bluetooth branch)
 *****************************************************************************/
//#include "wiced_bt_platform.h"

#ifndef USERIAL_RX_FIFO_SIZE
#define USERIAL_RX_FIFO_SIZE (3000)
#endif

static volatile bool bus_initialised = false;
static volatile bool device_powered  = false;

/* RX ring buffer. Bluetooth chip UART receive can be asynchronous, therefore a ring buffer is required */
static ring_buffer_t rx_ring_buffer;
static uint8_t             rx_data[USERIAL_RX_FIFO_SIZE];

int bt_bus_init( void )
{
    //USART_OverSampling8Cmd(USART1, ENABLE);

    if ( bus_initialised == false )
    {
#ifdef  MICO_USE_BT_HOST_WAKE_PIN
        check_noerr( platform_gpio_init( &bt_control_pins[BT_PIN_HOST_WAKE], INPUT_HIGH_IMPEDANCE ) );
#endif

#ifdef  MICO_USE_BT_DEVICE_WAKE_PIN
        check_noerr( platform_gpio_init( &bt_control_pins[BT_PIN_DEVICE_WAKE], OUTPUT_OPEN_DRAIN_PULL_UP ) );
        check_noerr( platform_gpio_output_high( &bt_control_pins[BT_PIN_DEVICE_WAKE] ) );
        mico_thread_msleep( 100 );
#endif

        /* Configure Reg Enable pin to output. Set to HIGH */
        check_noerr( platform_gpio_init( &bt_control_pins[BT_PIN_POWER], OUTPUT_OPEN_DRAIN_PULL_UP ) );
        check_noerr( platform_gpio_output_high( &bt_control_pins[BT_PIN_POWER] ) );
        device_powered = true;

#ifdef  MICO_USE_BT_RESET_PIN
        /* Configure Reset pin to output. Set to HIGH */
        check_noerr( platform_gpio_init( &bt_control_pins[BT_PIN_RESET], OUTPUT_OPEN_DRAIN_PULL_UP ) );
        check_noerr( platform_gpio_output_high( &bt_control_pins[BT_PIN_RESET] ) );
#endif

        /* Configure RTS pin to output. Set to HIGH */
        check_noerr( platform_gpio_init( &bt_uart_pins[BT_PIN_UART_RTS], OUTPUT_OPEN_DRAIN_PULL_UP ) );
        check_noerr( platform_gpio_output_high( &bt_uart_pins[BT_PIN_UART_RTS] ) );

        /* Configure CTS pin to input pull-up */
        check_noerr( platform_gpio_init( &bt_uart_pins[BT_PIN_UART_CTS], INPUT_PULL_UP ) );

        /* Initialise RX ring buffer */
        ring_buffer_init( &rx_ring_buffer, (uint8_t*) rx_data, sizeof( rx_data ) );

        /* Configure USART comms */
        check_noerr( MicoUartInitialize( BT_UART, &bt_uart_config, &rx_ring_buffer ) );

#ifdef  MICO_USE_BT_RESET_PIN
        /* Reset bluetooth chip. Delay momentarily. */
        check_noerr( platform_gpio_output_low( &bt_control_pins[BT_PIN_RESET] ) );
        mico_thread_msleep( 100 );
        check_noerr( platform_gpio_output_high( &bt_control_pins[BT_PIN_RESET] ) );
#endif

        /* Wait until the Bluetooth chip stabilises.  */
        mico_thread_msleep( 500 );

        /* Bluetooth chip is ready. Pull host's RTS low */
        check_noerr( platform_gpio_output_low( &bt_uart_pins[BT_PIN_UART_RTS] ) );

        bus_initialised = true;

        /* Wait for bluetooth chip to pull its RTS (host's CTS) low. From observation using CRO, it takes the bluetooth chip > 170ms to pull its RTS low after CTS low */
        BT_BUS_WAIT_UNTIL_READY();
    }

    return kNoErr;
}

int bt_bus_deinit( void )
{
    if ( bus_initialised == true )
    {
#ifdef  MICO_BT_USE_RESET_PIN
        check_noerr( platform_gpio_output_low( &bt_control_pins[BT_PIN_RESET] ) );
#endif
        check_noerr( platform_gpio_output_high( &bt_uart_pins[BT_PIN_UART_RTS] ) ); // RTS deasserted
        check_noerr( platform_gpio_output_low ( &bt_control_pins[BT_PIN_POWER] ) ); // Bluetooth chip regulator off
        device_powered = false;

        /* Deinitialise UART */
        check_noerr( MicoUartFinalize( BT_UART ) );
        bus_initialised = false;

        return kNoErr;
    }
    else
    {
        return kGeneralErr;
    }
}

int bt_bus_transmit( const uint8_t* data_out, uint32_t size )
{
    IS_BUS_INITIALISED();

    BT_BUS_WAIT_UNTIL_READY();

    check_noerr( MicoUartSend( BT_UART, data_out, size ) );

    return kNoErr;
}

int bt_bus_receive( uint8_t* data_in, uint32_t size, uint32_t timeout_ms )
{
    IS_BUS_INITIALISED();

    return MicoUartRecv( BT_UART, (void*)data_in, size, timeout_ms );
}

bool bt_bus_is_ready( void )
{
    return ( bus_initialised == false ) ? false : ( ( platform_gpio_input_get( &bt_uart_pins[BT_PIN_UART_CTS] ) == true ) ? false : true );
}

void platform_USART_Config(uint32_t baud)
{
  USART_InitTypeDef USART_InitStructure;
  USART_Cmd(USART1, DISABLE);
   USART_InitStructure.USART_BaudRate =baud;
  USART_InitStructure.USART_WordLength = USART_WordLength_8b;
  USART_InitStructure.USART_StopBits = USART_StopBits_1;
  USART_InitStructure.USART_Parity = USART_Parity_No;
  USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_RTS_CTS;//USART_HardwareFlowControl_None;
  USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
  USART_Init(USART1, &USART_InitStructure);
  USART_Cmd(USART1, ENABLE);
}

int bt_bus_uart_reconifig_baud(uint32_t baud)
{
    //int result;
    //platform_uart_config_t uart_config;
       
	
       

    if ( bus_initialised == true )
    {
#if 1
        platform_USART_Config(baud);
        DRV_TRACE_ERROR1("uart reconfigure baudrate %d\r\n", baud);
#else        
        uart_config.baud_rate	 = baud;
		uart_config.data_width	 = DATA_WIDTH_8BIT;
		uart_config.parity		 = NO_PARITY;
		uart_config.flow_control = FLOW_CONTROL_CTS_RTS;
		uart_config.stop_bits	 = STOP_BITS_1;

		/* Stop the uart */
		result = MicoUartFinalize( BT_UART );
		if ( result != kNoErr )
		{
			printf("Error deinitialising Bluetooth UART bus\r\n" );
			bt_bus_deinit();
			return kGeneralErr;
		}

		/* Initialise RX ring buffer */
		ring_buffer_init( &rx_ring_buffer, (uint8_t*) rx_data, sizeof( rx_data ) );

		/* Reconfigure UART using requested baud rate */
		check_noerr( MicoUartInitialize( BT_UART, &bt_uart_config, &rx_ring_buffer ));
        DRV_TRACE_ERROR1("uart reconfigure baudrate %d\r\n", baud);
#endif
        return kNoErr;
    }
    else
    {
        return kGeneralErr;
    }
}








/*******************************************************************************
 **
 ** Function           USERIAL_GetLineSpeed
 **
 ** Description        This function convert USERIAL baud to line speed.
 **
 ** Output Parameter   None
 **
 ** Returns            line speed
 **
 *******************************************************************************/
uint32_t USERIAL_GetLineSpeed( UINT8 baud )
{
    if ( baud <= USERIAL_BAUD_4M )
        return ( userial_baud_tbl[baud - USERIAL_BAUD_300] );
    else
        return 0;
}

/*******************************************************************************
 **
 ** Function           USERIAL_GetBaud
 **
 ** Description        This function convert line speed to USERIAL baud.
 **
 ** Output Parameter   None
 **
 ** Returns            line speed
 **
 *******************************************************************************/
UDRV_API
extern UINT8 USERIAL_GetBaud( uint32_t line_speed )
{
    UINT8 i;
    for ( i = USERIAL_BAUD_300; i <= USERIAL_BAUD_4M; i++ )
    {
        if ( userial_baud_tbl[i - USERIAL_BAUD_300] == line_speed )
            return i;
    }

    return USERIAL_BAUD_AUTO;
}

/*******************************************************************************
 **
 ** Function           USERIAL_Init
 **
 ** Description        This function initializes the  serial driver.
 **
 ** Output Parameter   None
 **
 ** Returns            Nothing
 **
 *******************************************************************************/

UDRV_API
void USERIAL_Init( void *p_cfg )
{
    DRV_TRACE_DEBUG0("USERIAL_Init");

    memset(&userial_cb, 0, sizeof(userial_cb));
    GKI_init_q( &Userial_in_q );

    return;
}

void USERIAL_Open_ReadThread( void )
{
    int result;

    DRV_TRACE_DEBUG0("USERIAL_Open_ReadThread++");

    g_readThreadAlive = 1;
    result = mico_rtos_create_thread( &read_thread_id, READ_THREAD_PRI, (const char *) READ_THREAD_NAME, userial_read_thread, READ_THREAD_STACK_SIZE, NULL );
    if ( result != kNoErr )
    {
        DRV_TRACE_DEBUG0("USERIAL_Open failed to create userial_read_thread");
        USERIAL_Close( 0 );
        g_readThreadAlive = 0;
        return;
    }
}

void USERIAL_Close_ReadThread( void )
{
    // Close our read thread
    g_readThreadAlive = 0;
    mico_rtos_delete_thread( &read_thread_id );

    // Flush the queue
    do
    {
        pbuf_USERIAL_Read = (BT_HDR *) GKI_dequeue( &Userial_in_q );
        if ( pbuf_USERIAL_Read )
        {
            GKI_freebuf( pbuf_USERIAL_Read );
        }

    } while ( pbuf_USERIAL_Read );
}

/*******************************************************************************
 **
 ** Function           USERIAL_Open
 **
 ** Description        Open the indicated serial port with the given configuration
 **
 ** Output Parameter   None
 **
 ** Returns            Nothing
 **
 *******************************************************************************/

UDRV_API
void USERIAL_Open( tUSERIAL_PORT port, tUSERIAL_OPEN_CFG *p_cfg, tUSERIAL_CBACK *p_cback )
{
    int result;

    DRV_TRACE_DEBUG0("USERIAL_Open");

    result = bt_bus_init( );
    if ( result != kNoErr )
    {
        DRV_TRACE_DEBUG0("USERIAL_Open failed");
        return;
    }

    USERIAL_Open_ReadThread( );

    userial_cb.p_cback = p_cback;
}

/*******************************************************************************
 **
 ** Function           USERIAL_Read
 **
 ** Description        Read data from a serial port using byte buffers.
 **
 ** Output Parameter   None
 **
 ** Returns            Number of bytes actually read from the serial port and
 **                    copied into p_data.  This may be less than len.
 **
 *******************************************************************************/

UDRV_API
UINT16 USERIAL_Read( tUSERIAL_PORT port, UINT8 *p_data, UINT16 len )
{
    UINT16 total_len = 0;
    UINT16 copy_len = 0;
    UINT8 * current_packet = NULL;

    do
    {
        if ( pbuf_USERIAL_Read != NULL )
        {
            current_packet = ( (UINT8 *) ( pbuf_USERIAL_Read + 1 ) ) + ( pbuf_USERIAL_Read->offset );

            if ( ( pbuf_USERIAL_Read->len ) <= ( len - total_len ) )
                copy_len = pbuf_USERIAL_Read->len;
            else
                copy_len = ( len - total_len );

            memcpy( ( p_data + total_len ), current_packet, copy_len );

            total_len += copy_len;

            pbuf_USERIAL_Read->offset += copy_len;
            pbuf_USERIAL_Read->len -= copy_len;

            if ( pbuf_USERIAL_Read->len == 0 )
            {
                GKI_freebuf( pbuf_USERIAL_Read );
                pbuf_USERIAL_Read = NULL;
            }
        }

        if ( pbuf_USERIAL_Read == NULL )
        {
            pbuf_USERIAL_Read = (BT_HDR *) GKI_dequeue( &Userial_in_q );
//            DRV_TRACE_DEBUG1("USERIAL_Read dequeue %08x", pbuf_USERIAL_Read);
        }
    } while ( ( pbuf_USERIAL_Read != NULL ) && ( total_len < len ) );

//    DRV_TRACE_DEBUG1("USERIAL_Read %i bytes", total_len);

    return total_len;
}

/*******************************************************************************
 **
 ** Function           USERIAL_Readbuf
 **
 ** Description        Read data from a serial port using GKI buffers.
 **
 ** Output Parameter   Pointer to a GKI buffer which contains the data.
 **
 ** Returns            Nothing
 **
 ** Comments           The caller of this function is responsible for freeing the
 **                    GKI buffer when it is finished with the data.  If there is
 **                    no data to be read, the value of the returned pointer is
 **                    NULL.
 **
 *******************************************************************************/
UDRV_API
void USERIAL_ReadBuf( tUSERIAL_PORT port, BT_HDR **p_buf )
{

}

/*******************************************************************************
 **
 ** Function           USERIAL_WriteBuf
 **
 ** Description        Write data to a serial port using a GKI buffer.
 **
 ** Output Parameter   None
 **
 ** Returns            TRUE  if buffer accepted for write.
 **                    FALSE if there is already a buffer being processed.
 **
 ** Comments           The buffer will be freed by the serial driver.  Therefore,
 **                    the application calling this function must not free the
 **                    buffer.
 **
 *******************************************************************************/
UDRV_API
BOOLEAN USERIAL_WriteBuf( tUSERIAL_PORT port, BT_HDR *p_buf )
{
    return FALSE;
}

/*******************************************************************************
 **
 ** Function           USERIAL_Write
 **
 ** Description        Write data to a serial port using a byte buffer.
 **
 ** Output Parameter   None
 **
 ** Returns            Number of bytes actually written to the serial port.  This
 **                    may be less than len.
 **
 *******************************************************************************/
UDRV_API
UINT16 USERIAL_Write( tUSERIAL_PORT port, UINT8 *p_data, UINT16 len )
{
    int result;

//    DRV_TRACE_DEBUG1("USERIAL_Write len=%d", len);
    result = bt_bus_transmit( p_data, len );
    if ( result != kNoErr )
    {
        DRV_TRACE_DEBUG0("USERIAL_Write failed");
        return 0;
    }
//    DRV_TRACE_DEBUG0("USERIAL_Write success");
    return len;
}

/*******************************************************************************
 **
 ** Function           USERIAL_Ioctl
 **
 ** Description        Perform an operation on a serial port.
 **
 ** Output Parameter   The p_data parameter is either an input or output depending
 **                    on the operation.
 **
 ** Returns            Nothing
 **
 *******************************************************************************/

UDRV_API
void USERIAL_Ioctl( tUSERIAL_PORT port, tUSERIAL_OP op, tUSERIAL_IOCTL_DATA *p_data )
{

    switch ( op )
    {
        case USERIAL_OP_FLUSH:
            break;
        case USERIAL_OP_FLUSH_RX:
            break;
        case USERIAL_OP_FLUSH_TX:
            break;
        case USERIAL_OP_BAUD_RD:
            break;
        case USERIAL_OP_BAUD_WR:
            USERIAL_Close_ReadThread( ); // Close read thread before de-initing UART
            bt_bus_uart_reconifig_baud(USERIAL_GetLineSpeed(p_data->baud));
            USERIAL_Open_ReadThread( ); // Start the read thread again
            break;
        default:
            break;
    }

    return;
}

/*******************************************************************************
 **
 ** Function           USERIAL_Close
 **
 ** Description        Close a serial port
 **
 ** Output Parameter   None
 **
 ** Returns            Nothing
 **
 *******************************************************************************/
UDRV_API
void USERIAL_Close( tUSERIAL_PORT port )
{
    int result;
    DRV_TRACE_DEBUG0("USERIAL_Close");

    USERIAL_Close_ReadThread( );

    result = bt_bus_deinit( );
    if ( result != kNoErr )
    {
        DRV_TRACE_DEBUG0("USERIAL_Close failed");
    }

    DRV_TRACE_DEBUG0("USERIAL_Close--");
}

/*******************************************************************************
 **
 ** Function           USERIAL_Feature
 **
 ** Description        Check whether a feature of the serial API is supported.
 **
 ** Output Parameter   None
 **
 ** Returns            TRUE  if the feature is supported
 **                    FALSE if the feature is not supported
 **
 *******************************************************************************/
UDRV_API BOOLEAN USERIAL_Feature( tUSERIAL_FEATURE feature )
{
    return FALSE;
}

int bt_hci_transport_driver_bus_read_handler( BT_HDR* packet )
{
    hci_packet_type_t packet_type = HCI_UNINITIALIZED;
    UINT8 *current_packet;

    if ( packet == NULL )
    {
        return kGeneralErr;
    }

    packet->offset = 0;
    packet->layer_specific = 0;
    current_packet = (UINT8 *) ( packet + 1 );

    // Read 1 byte:
    //    packet_type
    // Use a timeout here so we can shutdown the thread
    if ( bt_bus_receive( (uint8_t*) &packet_type, 1, MICO_NEVER_TIMEOUT ) != kNoErr)
    {
        DRV_TRACE_ERROR0("bt_bus error reading pkt_type");
        return kGeneralErr;
    }

//    DRV_TRACE_DEBUG1("bt_hci_transport_driver_bus_read_handler packet_type=0x%x", packet_type);

    switch ( packet_type )
    {
        case HCI_LOOPBACK_MODE:
            *current_packet++ = packet_type;

            // Read 1 byte:
            //    content_length
            if ( bt_bus_receive( current_packet, 1, MICO_NEVER_TIMEOUT ) != kNoErr )
            {
                return kGeneralErr;
            }

            packet->len = *current_packet++;
            DRV_TRACE_DEBUG1("HCI_LOOPBACK_MODE packet->len=0x%x", packet->len)
            ;

            // Read payload
            if ( bt_bus_receive( current_packet, packet->len, MICO_NEVER_TIMEOUT ) != kNoErr )
            {
                DRV_TRACE_DEBUG0("bt_bus_receive FAIL in bt_hci_transport_driver_bus_read_handler");
                return kGeneralErr;
            }

            packet->len = packet->len + 2; // +2 for packet type + read length

            break;

        case HCI_EVENT_PACKET:
        {
            hci_event_header_t header;

            // Read 2 bytes:
            //    event_code
            //    content_length
            if ( bt_bus_receive( (uint8_t*) &header.event_code, sizeof( header ) - sizeof( packet_type ), MICO_NEVER_TIMEOUT ) != kNoErr )
            {
                return kGeneralErr;
            }

            header.packet_type = packet_type;
            *current_packet++ = packet_type;
            *current_packet++ = header.event_code;
            *current_packet++ = header.content_length;
            packet->len = header.content_length + 3; // +3 to include sizeof: packet_type=1 event_code=1 content_length=1

            // Read payload
            if ( bt_bus_receive( (uint8_t*) current_packet, (uint32_t) header.content_length, MICO_NEVER_TIMEOUT ) != kNoErr )
            {
                return kGeneralErr;
            }

            break;
        }

        case HCI_ACL_DATA_PACKET:
        {
            hci_acl_packet_header_t header;

            // Read 4 bytes:
            //    hci_handle (2 bytes)
            //    content_length (2 bytes)
            if ( bt_bus_receive( (uint8_t*) &header.hci_handle, HCI_DATA_PREAMBLE_SIZE, MICO_NEVER_TIMEOUT ) != kNoErr )
            {
                DRV_TRACE_ERROR0("bt_bus error acl header");
                return kGeneralErr;
            }

            header.packet_type = packet_type;
            UINT8_TO_STREAM(current_packet, packet_type);
            UINT16_TO_STREAM(current_packet, header.hci_handle);
            UINT16_TO_STREAM(current_packet, header.content_length);

            packet->len = header.content_length + 5; // +5 to include sizeof: packet_type=1 hci_handle=2 content_length=2

#ifdef L2CAP_REASSEMBLE

            /* Check for segmented packets. If this is a continuation packet, then   */
            /* current rcv buffer will be freed, and we will continue appending data */
            /* to the original rcv buffer.                                           */
            if ((packet = l2cap_link_chk_pkt_start (packet)) == NULL)
            {
                /* If a NULL is returned, then we have a problem. Ignore remaining data in this packet */
                p_cb->rcv_len = msg_len;
                if (msg_len == 0)
                {
                    p_cb->rcv_state = HCISU_H4_MSGTYPE_ST;  /* Wait for next message */
                }
                else
                {
                    HCI_TRACE_ERROR0("HCIS: Ignoring....");

                    p_cb->rcv_state = HCISU_H4_IGNORE_ST;   /* Ignore rest of the packet */
                }

                break;
            }
#endif

            if (header.content_length > (HCI_ACL_POOL_BUF_SIZE - BT_HDR_SIZE))
            {
                DRV_TRACE_ERROR1("bt_bus error invalid acl len %i", header.content_length);
                return kGeneralErr;
            }
            // Read payload
            if ( bt_bus_receive( (uint8_t*) current_packet, (uint32_t) header.content_length, MICO_NEVER_TIMEOUT ) != kNoErr )
            {
                return kGeneralErr;
            }

            break;
        }

        case HCI_COMMAND_PACKET: /* Fall-through */
        default:
            return kGeneralErr;
    }

    return kNoErr;
}

/*******************************************************************************
 **
 ** Function           userial_read_thread
 **
 ** Description        userial_read_thread
 **
 ** Output Parameter   None
 **
 ** Returns            None
 **
 *******************************************************************************/
void userial_read_thread(void* arg)
{
    BT_HDR *p_buf;
    int ret;

    while ( g_readThreadAlive )
    {
        // FIXME:TODO: Decide on the correct buffer size
//        if ( ( p_buf = (BT_HDR *) GKI_getbuf( GKI_BUF1_SIZE /*200*/) ) != NULL )
        if ( ( p_buf = (BT_HDR *) GKI_getbuf( HCI_ACL_POOL_BUF_SIZE) ) != NULL )
        {
            ret = bt_hci_transport_driver_bus_read_handler( p_buf );
            if ( ret == kGeneralErr )
            {
                GKI_freebuf( p_buf );
                continue;
            }

//            DRV_TRACE_DEBUG1("USERIAL_Read enqueue %08x", p_buf);

#if 0
            p = (p_buf+1);
            if (p[0] == HCI_ACL_DATA_PACKET)
            {
                /* Skip over packet type */
                p_buf->offset = 1;
                p_buf->len -= 1;
                p_buf->layer_specific = 0;
                p_buf->event = BT_EVT_TO_BTU_HCI_ACL;
                GKI_send_msg(BTU_TASK, 0, p_buf);
                continue;
            }
#endif
            GKI_enqueue( &Userial_in_q, p_buf );


            if (userial_cb.p_cback)
            {
                (userial_cb.p_cback)(0, USERIAL_RX_READY_EVT, NULL);
            }
        }
        else
        {
            GKI_delay(2000);
        }
    }

    DRV_TRACE_DEBUG0("Shutdown userial_read_thread");
}
