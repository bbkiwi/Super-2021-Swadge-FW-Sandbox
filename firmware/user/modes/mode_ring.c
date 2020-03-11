#include "mode_ring.h"
#include "p2pConnection.h"
#include "buttons.h"
#include "bresenham.h"
#include "font.h"
#include "hsv_utils.h"

#define RING_DEBUG_PRINT
#ifdef RING_DEBUG_PRINT
    #define ring_printf(...) os_printf(__VA_ARGS__)
#else
    #define ring_printf(...)
#endif

// LEDs relation to screen
#define LED_UPPER_LEFT LED_1
#define LED_UPPER_MID LED_2
#define LED_UPPER_RIGHT LED_3
#define LED_LOWER_RIGHT LED_4
#define LED_LOWER_MID LED_5
#define LED_LOWER_LEFT LED_6


typedef struct
{
    p2pInfo p2p;
    button_mask side;
    char lbl[4];
} ringCon_t;

void ICACHE_FLASH_ATTR ringEnterMode(void);
void ICACHE_FLASH_ATTR ringExitMode(void);
void ICACHE_FLASH_ATTR ringButtonCallback(uint8_t state, int button, int down);
void ICACHE_FLASH_ATTR ringEspNowRecvCb(uint8_t* mac_addr, uint8_t* data, uint8_t len, uint8_t rssi);
void ICACHE_FLASH_ATTR ringEspNowSendCb(uint8_t* mac_addr, mt_tx_status status);
void ICACHE_FLASH_ATTR ringAccelerometerCallback(accel_t* accel);

void ICACHE_FLASH_ATTR ringConCbFn(p2pInfo* p2p, connectionEvt_t);
void ICACHE_FLASH_ATTR ringMsgRxCbFn(p2pInfo* p2p, char* msg, uint8_t* payload, uint8_t len);
void ICACHE_FLASH_ATTR ringMsgTxCbFn(p2pInfo* p2p, messageStatus_t status);

void ICACHE_FLASH_ATTR ringUpdateDisplay(void);
void ICACHE_FLASH_ATTR ringAnimationTimer(void* arg __attribute__((unused)));
void ICACHE_FLASH_ATTR ringSetLeds(led_t* ledData, uint8_t ledDataLen);

uint8_t ledOrderInd[] = {LED_UPPER_LEFT, LED_LOWER_LEFT, LED_LOWER_MID, LED_LOWER_RIGHT, LED_UPPER_RIGHT, LED_UPPER_MID};
uint8_t ledCnOrderInd[] = {LED_UPPER_LEFT, LED_LOWER_LEFT, LED_UPPER_MID, LED_LOWER_MID, LED_UPPER_RIGHT, LED_LOWER_RIGHT};

uint8_t ringBrightnessIdx = 2;
uint8_t indLed;
static led_t leds[NUM_LIN_LEDS] = {{0}};
// When using gamma correcton
static const uint8_t ringBrightnesses[] =
{
    0x01,
    0x02,
    0x04,
    0x08,
};

ringCon_t* ICACHE_FLASH_ATTR getSideConnection(button_mask side);
ringCon_t* ICACHE_FLASH_ATTR getRingConnection(p2pInfo* p2p);

swadgeMode ringMode =
{
    .modeName = "ring",
    .fnEnterMode = ringEnterMode,
    .fnExitMode = ringExitMode,
    .fnButtonCallback = ringButtonCallback,
    .wifiMode = ESP_NOW,
    .fnEspNowRecvCb = ringEspNowRecvCb,
    .fnEspNowSendCb = ringEspNowSendCb,
    .fnAccelerometerCallback = ringAccelerometerCallback
};


ringCon_t connections[3];
#define NUM_CONNECTIONS (sizeof(connections) / sizeof(connections[0]))

button_mask connectionSide;

char lastMsg[256];

os_timer_t animationTimer;
uint8_t radiusLeft = 0;
uint8_t radiusRight = 0;
uint8_t ringHueRight;
uint8_t ringHueLeft;

ringCon_t* ICACHE_FLASH_ATTR getSideConnection(button_mask side)
{
    uint8_t i;
    for(i = 0; i < NUM_CONNECTIONS; i++)
    {
        if(side == connections[i].p2p.side)
        {
            return &connections[i];
        }
    }
    return NULL;
}

ringCon_t* ICACHE_FLASH_ATTR getRingConnection(p2pInfo* p2p)
{
    uint8_t i;
    for(i = 0; i < NUM_CONNECTIONS; i++)
    {
        if(p2p == &connections[i].p2p)
        {
            return &connections[i];
        }
    }
    return NULL;
}

/**
 * @brief TODO
 *
 */
void ICACHE_FLASH_ATTR ringEnterMode(void)
{
    ring_printf("%s\n", __func__);
    ets_memset(&connections, 0, sizeof(connections));

    memcpy(connections[0].lbl, "cn0", 3);
    memcpy(connections[1].lbl, "cn1", 3);
    memcpy(connections[2].lbl, "cn2", 3);

    uint8_t i;
    for(i = 0; i < NUM_CONNECTIONS; i++)
    {
        p2pInitialize(&connections[i].p2p, connections[i].lbl, ringConCbFn, ringMsgRxCbFn, 0);
        // Doesn't work if here! Had to put in p2pInialize
        //connections[i].p2p.side = 0x0F;
    }

    os_timer_setfn(&animationTimer, ringAnimationTimer, NULL);
    os_timer_arm(&animationTimer, 50, true);

    ringUpdateDisplay();
    for(i = 0; i < NUM_CONNECTIONS; i++)
    {
        ring_printf("   i = %d, side = %d", i, connections[i].p2p.side);
    }
    ring_printf("\n");
}

/**
 * @brief TODO
 *
 */
void ICACHE_FLASH_ATTR ringExitMode(void)
{
    ring_printf("%s\n", __func__);
    uint8_t i;
    for(i = 0; i < NUM_CONNECTIONS; i++)
    {
        p2pDeinit(&connections[i].p2p);
    }
}

/**
 * @brief TODO
 *
 * @param state
 * @param button
 * @param down
 */
void ICACHE_FLASH_ATTR ringButtonCallback(uint8_t state __attribute__((unused)), int button, int down)
{
    if(down)
    {
        switch(button)
        {
            default:
            case 0:
            {
                break;
            }
            case 1: // Left
            case 2: // Right
            {
                button_mask side = (button == 1) ? LEFT : RIGHT;

                // If no one's connected on this side
                if(NULL == getSideConnection(side))
                {
                    // Start connections for unconnected p2ps
                    connectionSide = side;
                    uint8_t i;
                    for(i = 0; i < NUM_CONNECTIONS; i++)
                    {
                        //ring_printf("    i = %d, side = %d", i, connections[i].p2p.side);
                        if(0x0F == connections[i].p2p.side)
                        {
                            connections[i].p2p.side = connectionSide;
                            p2pStartConnection(&(connections[i].p2p));
                        }
                    }
                    //ring_printf("\n");
                }
                else
                {
                    // Light led with random hue on correct side
                    uint8_t randomHue = os_random();
                    if (side == LEFT)
                    {
                        radiusLeft = 1;
                        ringHueLeft = randomHue;
                    }
                    else
                    {
                        radiusRight = 1;
                        ringHueRight = randomHue;
                    }
                    // Send a message with hue
                    char testMsg[256] = {0};
                    ets_sprintf(testMsg, "%02X is the hue", randomHue);
                    p2pSendMsg(&(getSideConnection(side)->p2p), "tst", testMsg, sizeof(testMsg),
                               ringMsgTxCbFn);
                }
                break;
            }
        }
    }
    ringUpdateDisplay();
}

/**
 * @brief TODO
 *
 * @param mac_addr
 * @param data
 * @param len
 * @param rssi
 */
void ICACHE_FLASH_ATTR ringEspNowRecvCb(uint8_t* mac_addr, uint8_t* data, uint8_t len, uint8_t rssi)
{
    uint8_t i;
    for(i = 0; i < NUM_CONNECTIONS; i++)
    {
        p2pRecvCb(&(connections[i].p2p), mac_addr, data, len, rssi);
    }
    ringUpdateDisplay();
}

/**
 * @brief TODO
 *
 * @param mac_addr
 * @param status
 */
void ICACHE_FLASH_ATTR ringEspNowSendCb(uint8_t* mac_addr, mt_tx_status status)
{
    uint8_t i;
    for(i = 0; i < NUM_CONNECTIONS; i++)
    {
        p2pSendCb(&(connections[i].p2p), mac_addr, status);
    }
    ringUpdateDisplay();
}

/**
 * @brief TODO
 *
 * @param accel
 */
void ICACHE_FLASH_ATTR ringAccelerometerCallback(accel_t* accel __attribute__((unused)))
{
    ringUpdateDisplay();
    return;
}

/**
 * @brief TODO
 *
 * @param p2p
 * @param evt
 */
void ICACHE_FLASH_ATTR ringConCbFn(p2pInfo* p2p, connectionEvt_t evt)
{
    char* conStr = getRingConnection(p2p)->p2p.msgId;

    switch(evt)
    {
        case CON_STARTED:
        {
            os_snprintf(lastMsg, sizeof(lastMsg), "%s: CON_STARTED\n", conStr);
            break;
        }
        case CON_STOPPED:
        {
            os_snprintf(lastMsg, sizeof(lastMsg), "%s: CON_STOPPED\n", conStr);
            break;
        }
        case RX_BROADCAST:
        case RX_GAME_START_ACK:
        case RX_GAME_START_MSG:
        {
            // As soon as one connection starts, stop the others
            uint8_t i;
            for(i = 0; i < NUM_CONNECTIONS; i++)
            {
                if(p2p != &connections[i].p2p)
                {
                    p2pStopConnection(&connections[i].p2p);
                }
            }
            os_snprintf(lastMsg, sizeof(lastMsg), "%s: %s\n", conStr,
                        (evt == RX_BROADCAST) ? "RX_BROADCAST" : ((evt == RX_GAME_START_ACK) ? "RX_GAME_START_ACK" : "RX_GAME_START_MSG") );
            break;
        }
        case CON_ESTABLISHED:
        {
            for(uint8_t i = 0; i < NUM_CONNECTIONS; i++)
            {
                ring_printf("    i = %d, side = %d", i, connections[i].p2p.side);
            }
            ring_printf("\n");
            getRingConnection(p2p)->p2p.side = connectionSide;
            os_snprintf(lastMsg, sizeof(lastMsg), "%s: CON_ESTABLISHED on side %d\n", conStr, connectionSide);
            for(uint8_t i = 0; i < NUM_CONNECTIONS; i++)
            {
                ring_printf("    i = %d, side = %d", i, connections[i].p2p.side);
            }
            ring_printf("\n");
            break;
        }
        case CON_LOST:
        {
            getRingConnection(p2p)->p2p.side = 0x0F;
            os_snprintf(lastMsg, sizeof(lastMsg), "%s: CON_LOST\n", conStr);
            break;
        }
        default:
        {
            os_snprintf(lastMsg, sizeof(lastMsg), "%s: Unknown event %d\n", conStr, evt);
            break;
        }
    }
    ring_printf("%s %s", __func__, lastMsg);
    ringUpdateDisplay();
    return;
}

/**
 * @brief TODO
 *
 * @param p2p
 * @param msg
 * @param payload
 * @param len
 */
//TODO Here ignore payload, but may for more sophisiticated use may want to include
void ICACHE_FLASH_ATTR ringMsgRxCbFn(p2pInfo* p2p, char* msg, uint8_t* payload, uint8_t len)
{
    if(0 == strcmp(msg, "tst"))
    {
        if(RIGHT == getRingConnection(p2p)->p2p.side)
        {
            radiusRight = 1;
            ringHueRight = p2pHex2Int(payload[3]) * 16 + p2pHex2Int(payload[4]);
            os_printf("ringHueRight = %d\n", ringHueRight);
        }
        else if(LEFT == getRingConnection(p2p)->p2p.side)
        {
            radiusLeft = 1;
            ringHueLeft = p2pHex2Int(payload[3]) * 16 + p2pHex2Int(payload[4]);
            os_printf("ringHueLeft = %d\n", ringHueLeft);
        }
    }
    else if(0 == strcmp(msg, "rst"))
    {
        os_printf("RST message received\n");
        for(uint8_t i = 0; i < NUM_CONNECTIONS; i++)
        {
            {
                p2pStopConnection(&connections[i].p2p);
            }
        }

        if(RIGHT == getRingConnection(p2p)->p2p.side)
        {
            radiusRight = 1;
            ringHueRight = p2pHex2Int(payload[3]) * 16 + p2pHex2Int(payload[4]);
            os_printf("REPAIR ringHueRight = %d\n", ringHueRight);
        }
        else if(LEFT == getRingConnection(p2p)->p2p.side)
        {
            radiusLeft = 1;
            ringHueLeft = p2pHex2Int(payload[3]) * 16 + p2pHex2Int(payload[4]);
            os_printf("REPAIR ringHueLeft = %d\n", ringHueLeft);
        }
    }


    os_snprintf(lastMsg, sizeof(lastMsg), "got msg=%s %s (%d bytes) from %s\n", msg, payload, len,
                getRingConnection(p2p)->p2p.msgId);
    ring_printf("%s %s", __func__, lastMsg);
    ringUpdateDisplay();
    return;
}

/**
 * @brief TODO
 *
 * @param p2p
 * @param status
 */
void ICACHE_FLASH_ATTR ringMsgTxCbFn(p2pInfo* p2p, messageStatus_t status)
{
    char* conStr = getRingConnection(p2p)->p2p.msgId;

    switch(status)
    {
        case MSG_ACKED:
        {
            os_snprintf(lastMsg, sizeof(lastMsg), "%s: MSG_ACKED\n", conStr);
            break;
        }
        case MSG_FAILED:
        {
            os_snprintf(lastMsg, sizeof(lastMsg), "%s: MSG_FAILED\n", conStr);
            getRingConnection(p2p)->p2p.side = 0x0F;
            break;
        }
        default:
        {
            os_snprintf(lastMsg, sizeof(lastMsg), "%s: Unknown status %d\n", conStr, status);
            break;
        }
    }
    ring_printf("%s %s", __func__, lastMsg);
    ringUpdateDisplay();
    return;
}

/**
 * @brief TODO
 *
 */
void ICACHE_FLASH_ATTR ringUpdateDisplay(void)
{
    uint8_t i;
    char macStr[8];
    uint32_t colorToShow;
    clearDisplay();

    plotText(45, 0, &connections[0].p2p.cnc.macStr[12], IBM_VGA_8, WHITE);

    plotText(6, 12, lastMsg, IBM_VGA_8, WHITE);

    for(i = 0; i < NUM_CONNECTIONS; i++)
    {
        if(connections[i].p2p.cnc.otherMacReceived)
        {
            os_snprintf(macStr, sizeof(macStr), "%02X:%02X", connections[i].p2p.cnc.otherMac[4],
                        connections[i].p2p.cnc.otherMac[5]);
            plotText(6 + 40 * i, 24, macStr, IBM_VGA_8, WHITE);
        }
        if (connections[i].p2p.cnc.playOrder > 0)
        {
            os_snprintf(macStr, sizeof(macStr), "p%d", connections[i].p2p.cnc.playOrder);
            plotText(6 + 40 * i, 36, macStr, IBM_VGA_8, WHITE);
        }
        if(connections[i].p2p.ack.isWaitingForAck)
        {
            plotText(6 + 40 * i, 48, "ack ?", IBM_VGA_8, WHITE);
        }
    }

    if(NULL != getSideConnection(RIGHT))
    {
        plotText(104, 0, getSideConnection(RIGHT)->p2p.msgId, IBM_VGA_8, WHITE);
        //plotRect(OLED_WIDTH - 5, 0, OLED_WIDTH - 1, 5, WHITE);
    }

    if(NULL != getSideConnection(LEFT))
    {
        plotText(0, 0, getSideConnection(LEFT)->p2p.msgId, IBM_VGA_8, WHITE);
        //plotRect(0, 0, 4, 5, WHITE);
    }

    //Clear leds
    memset(leds, 0, sizeof(leds));

    if(radiusRight > 0)
    {
        plotCircle(127, 63, radiusRight, WHITE);
        colorToShow = EHSVtoHEXhelper(ringHueRight, 0xFF, 6 * (10 + radiusRight), false);
        leds[LED_LOWER_RIGHT].r =  (colorToShow >>  0) & 0xFF;
        leds[LED_LOWER_RIGHT].g =  (colorToShow >>  8) & 0xFF;
        leds[LED_LOWER_RIGHT].b =  (colorToShow >>  16) & 0xFF;
    }
    if(radiusLeft > 0)
    {
        plotCircle(0, 63, radiusLeft, WHITE);
        colorToShow = EHSVtoHEXhelper(ringHueLeft, 0xFF, 6 * (10 + radiusLeft), false);
        leds[LED_LOWER_LEFT].r =  (colorToShow >>  0) & 0xFF;
        leds[LED_LOWER_LEFT].g =  (colorToShow >>  8) & 0xFF;
        leds[LED_LOWER_LEFT].b =  (colorToShow >>  16) & 0xFF;
    }

    if (radiusLeft == 0 && radiusRight == 0)
    {

        for(i = 0; i < NUM_CONNECTIONS; i++)
        {
            if(connections[i].p2p.cnc.isConnecting)
            {
                leds[ledCnOrderInd[2 * i]].r = 255;
            }
            else if (connections[i].p2p.cnc.isConnected)
            {
                leds[ledCnOrderInd[2 * i]].g = 255;
            }
            else if (connections[i].p2p.cnc.broadcastReceived)
            {
                leds[ledCnOrderInd[2 * i]].r = 255;
                leds[ledCnOrderInd[2 * i]].g = 255;
            }

            if(connections[i].p2p.cnc.otherMacReceived)
            {
                leds[ledCnOrderInd[2 * i + 1]].r = 255;
            }
            if (connections[i].p2p.cnc.rxGameStartAck)
            {
                leds[ledCnOrderInd[2 * i + 1]].g = 255;
            }
            if (connections[i].p2p.cnc.rxGameStartMsg)
            {
                leds[ledCnOrderInd[2 * i + 1]].b = 255;
            }
        }
    }
    ringSetLeds(leds, sizeof(leds));
}

void ICACHE_FLASH_ATTR ringAnimationTimer(void* arg __attribute__((unused)))
{
    bool shouldUpdate = false;
    if(radiusLeft > 0)
    {
        radiusLeft++;
        if(radiusLeft == 30)
        {
            radiusLeft = 0;
        }
        shouldUpdate = true;
    }

    if(radiusRight > 0)
    {
        radiusRight++;
        if(radiusRight == 30)
        {
            radiusRight = 0;
        }
        shouldUpdate = true;
    }

    if(shouldUpdate)
    {
        ringUpdateDisplay();
    }
}

/**
* Intermediate function which adjusts brightness and sets the LEDs
*    and applies a brightness ramp
*
* @param ledData    The LEDs to be scaled, then gamma corrected, then set
* @param ledDataLen The length of the LEDs to set
*/
void ICACHE_FLASH_ATTR ringSetLeds(led_t* ledData, uint8_t ledDataLen)
{
    uint8_t i;
    led_t ledsAdjusted[NUM_LIN_LEDS];
    for(i = 0; i < ledDataLen / sizeof(led_t); i++)
    {
        ledsAdjusted[i].r = GAMMA_CORRECT(ledData[i].r / ringBrightnesses[ringBrightnessIdx]);
        ledsAdjusted[i].g = GAMMA_CORRECT(ledData[i].g / ringBrightnesses[ringBrightnessIdx]);
        ledsAdjusted[i].b = GAMMA_CORRECT(ledData[i].b / ringBrightnesses[ringBrightnessIdx]);
    }
    setLeds(ledsAdjusted, ledDataLen);
}
