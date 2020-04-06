/*==============================================================================
 * Includes
 *============================================================================*/

#include "mode_ring.h"
#include "p2pConnection.h"
#include "buttons.h"
#include "bresenham.h"
#include "font.h"
#include "hsv_utils.h"

/*==============================================================================
 * Defines
 *============================================================================*/
#define RING_DEBUG_PRINT
#ifdef RING_DEBUG_PRINT
#define ring_printf(...) do{os_printf("%s::%d ", __func__, __LINE__); os_printf(__VA_ARGS__);}while(0)
#define ringPrintf(...) do { \
        os_snprintf(lastMsg, sizeof(lastMsg), __VA_ARGS__); \
        os_printf("%s", lastMsg); \
        ringLastMsgTextIdx = SCROLL_GAP; \
        ringUpdateDisplay(); \
    } while(0)
#else
#define ring_printf(...)
#define ringPrintf(...) do { \
        os_snprintf(lastMsg, sizeof(lastMsg), __VA_ARGS__); \
        ringUpdateDisplay(); \
    } while(0)
#endif

#define TST_LABEL "tst"

#define lengthof(a) (sizeof(a) / sizeof(a[0]))

// LEDs relation to screen
#define LED_UPPER_LEFT LED_1
#define LED_UPPER_MID LED_2
#define LED_UPPER_RIGHT LED_3
#define LED_LOWER_RIGHT LED_4
#define LED_LOWER_MID LED_5
#define LED_LOWER_LEFT LED_6

#define LEFT_SIDE 0
#define RIGHT_SIDE 1
#define SCROLL_GAP 30


/*==============================================================================
 * Function Prototypes
 *============================================================================*/

void ringEnterMode(void);
void ringExitMode(void);
void ringButtonCallback(uint8_t state, int button, int down);
void ringEspNowRecvCb(uint8_t* mac_addr, uint8_t* data, uint8_t len, uint8_t rssi);
void ringEspNowSendCb(uint8_t* mac_addr, mt_tx_status status);

void ringConCbFn(p2pInfo* p2p, connectionEvt_t);
void ringMsgRxCbFn(p2pInfo* p2p, char* msg, uint8_t* payload, uint8_t len);
void ringMsgTxCbFn(p2pInfo* p2p, messageStatus_t status);

void ringUpdateDisplay(void);
void ringAnimation(void* arg __attribute__((unused)));
void ringSetLeds(led_t* ledData, uint8_t ledDataLen);
void ringScrollLastMsg(void* arg __attribute__((unused)));
void ringLongPressTimerFunc(void* arg __attribute__((unused)));
void ringAccelerometerCallback(accel_t* accel __attribute__((unused)));
p2pInfo* getSideConnection(button_mask side);
p2pInfo* getRingConnection(p2pInfo* p2p);

/*==============================================================================
 * Variables
 *============================================================================*/

uint8_t ledOrderInd[] = {LED_UPPER_LEFT, LED_LOWER_LEFT, LED_LOWER_MID, LED_LOWER_RIGHT, LED_UPPER_RIGHT, LED_UPPER_MID};
uint8_t ledCnOrderInd[] = {LED_UPPER_LEFT, LED_LOWER_LEFT, LED_UPPER_MID, LED_LOWER_MID, LED_UPPER_RIGHT, LED_LOWER_RIGHT};

uint8_t ringBrightnessIdx = 2;
uint8_t indLed;
int16_t ringLastMsgTextIdx;
bool ringLongPress;

// TODO remove eventually
// For debugging
uint16_t recvCbCnt;
uint16_t sendCbCnt;

static led_t leds[NUM_LIN_LEDS] = {{0}};
// When using gamma correcton
static const uint8_t ringBrightnesses[] =
{
    0x01,
    0x02,
    0x04,
    0x08,
};

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

p2pInfo connections[2];

button_mask connectionSide;

char lastMsg[256];

syncedTimer_t ringAnimationTimer;
syncedTimer_t scrollLastMsgTimer;
syncedTimer_t ringLongPressTimerTimer;
uint8_t radiusLeft = 0;
uint8_t radiusRight = 0;
uint8_t ringHueRight;
uint8_t ringHueLeft;
// TODO this is number assigned to swadge so maybe should be computed in this mode
uint8_t ringSeq;

/*==============================================================================
 * Functions
 *============================================================================*/

/**
 * Initialize the ring mode
 */
void ICACHE_FLASH_ATTR ringEnterMode(void)
{
    ring_printf("\n");
    // Clear everything out
    ets_memset(&connections, 0, sizeof(connections));

    // For each connection, initialize it
    uint8_t i;
    for(i = 0; i < lengthof(connections); i++)
    {
        //TODO could have side and otherSide ( which may become socket) as parameter of p2pInitialize
        p2pInitialize(&connections[i], "rng", ringConCbFn, ringMsgRxCbFn, 0);
        // Extra stuff to initialize
        connections[i].side = (i == 0) ? LEFT : RIGHT;
        connections[i].cnc.otherSide = (i == 0) ? RIGHT : LEFT;
    }

    enableDebounce(false);

    // Set up an animation timer
    syncedTimerDisarm(&ringAnimationTimer);
    syncedTimerSetFn(&ringAnimationTimer, ringAnimation, NULL);
    syncedTimerArm(&ringAnimationTimer, 50, true);

    // Set up a timer to refresh display thus scroll the instructions
    syncedTimerDisarm(&scrollLastMsgTimer);
    syncedTimerSetFn(&scrollLastMsgTimer, ringScrollLastMsg, NULL);
    syncedTimerArm(&scrollLastMsgTimer, 50, true);

    // Set up a timer to determin long press of button
    syncedTimerDisarm(&ringLongPressTimerTimer);
    syncedTimerSetFn(&ringLongPressTimerTimer, ringLongPressTimerFunc, NULL);

    // Draw the initial display
    ringUpdateDisplay();
    for(i = 0; i < lengthof(connections); i++)
    {
        ring_printf("i = %d, side = %d ", i, connections[i].side);
    }
    ring_printf("\n");
}

/**
 * De-initialize the ring mode
 */
void ICACHE_FLASH_ATTR ringExitMode(void)
{
    ring_printf("\n");
    // For each connection, deinitialize it
    uint8_t i;
    for(i = 0; i < lengthof(connections); i++)
    {
        p2pDeinit(&connections[i]);
    }
    syncedTimerDisarm(&ringAnimationTimer);
    syncedTimerDisarm(&scrollLastMsgTimer);
    syncedTimerDisarm(&ringLongPressTimerTimer);
}

/**
 * Ring mode button press handler. Either start connections or send messages
 * depending on the current state
 *
 * @param state  A bitmask of all buttons currently
 * @param button The button that changed state
 * @param down   true if the button was pressed, false if it was released
 */
void ICACHE_FLASH_ATTR ringButtonCallback(uint8_t state __attribute__((unused)), int button, int down)
{
    // If it was pressed
    if(down)
    {
        // Start a timer to specify  long press of button
        syncedTimerArm(&ringLongPressTimerTimer, 1000, false);
        ringLongPress = false;
    }
    else // Released
    {
        // Stop the timer no matter what
        syncedTimerDisarm(&ringLongPressTimerTimer);
        switch(button)
        {
            default:
            case 0:
            {
                // Shouldn't be able to get here
                break;
            }
            case 1: // Left
            case 2: // Right
            {
                // Save which button was pressed
                button_mask side = (button == 1) ? LEFT : RIGHT;
                // get index of connection for the side
                uint8_t idx =  (button == 1) ? LEFT_SIDE : RIGHT_SIDE;
                uint8_t otherIdx =  (button == 2) ? LEFT_SIDE : RIGHT_SIDE;

                // If not connected on this side start connection there
                //if(0x00 == connections[idx].side)
                // TODO do I need to test for both here?
                //if (!connections[idx].cnc.isConnecting && !connections[idx].cnc.isConnected )
                // TODO ok? This will allow multiple (long) push so for last if forget long push, can do again
                // What about long push too early?
                if (!connections[idx].cnc.isConnected )
                {
                    // connections[idx].side = side; // did in ringEnter
                    //TODO Important p2pStartConnection needs to know if other connection has been made

                    connections[idx].otherConnectionMade = connections[otherIdx].cnc.isConnected;
                    // also need to know if long push made
                    connections[idx].longPushButton = ringLongPress;

                    //TODO Alternative idea: if other side connected pushing R starts listening only, L starts broadcasting only so can complete ring
                    // if (button == 1)
                    // {
                    //     connections[idx].subsequentStartListenFirst = false;
                    // }
                    // else
                    // {
                    //     connections[idx].subsequentStartListenFirst = true;
                    // }
                    //connections[idx].cnc.otherSide = connections[otherIdx].side;

                    p2pStartConnection(&(connections[idx]));
                    p2pDumpInfo(&(connections[idx]));
                    ring_printf("Start connection on %s side listening for a %s side\n    %s Connection starting with %s.\n    idx = %d, side = %d cnc.otherSide = %d\n",
                                (idx == 0) ? "LEFT" : "RIGHT", (idx == 1) ? "LEFT" : "RIGHT", connections[idx].otherConnectionMade ? "Second" : "First",
                                (ringLongPress || !connections[idx].otherConnectionMade) ? "broadcast and listening" : "listening only",
                                idx, connections[idx].side,
                                connections[idx].cnc.otherSide);
                }

                else if  (connections[idx].cnc.isConnected)
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
                    // Send a message via the connection with hue
                    char testMsg[256] = {0};
                    ets_sprintf(testMsg, "%02X is the hue", randomHue);
                    p2pSendMsg(&(connections[idx]), "tst", testMsg, ets_strlen(testMsg),
                               ringMsgTxCbFn);
                }
                break;
            }
        }
    }
    ringUpdateDisplay();
}

/**
 * Callback function when ESP-NOW receives a packet. Forward everything to all
 * p2p connections and let them handle it
 *
 * @param mac_addr The MAC of the swadge that sent the data
 * @param data     The data
 * @param len      The length of the data
 * @param rssi     The RSSI of th received message, a proxy for distance
 */
void ICACHE_FLASH_ATTR ringEspNowRecvCb(uint8_t* mac_addr, uint8_t* data, uint8_t len, uint8_t rssi)
{
    uint8_t i;
    recvCbCnt++;
    for(i = 0; i < lengthof(connections); i++)
    {
        p2pRecvCb(&(connections[i]), mac_addr, data, len, rssi, recvCbCnt);
    }
    ringUpdateDisplay();
}

/**
 * Callback function when ESP-NOW sends a packet. Forward everything to all p2p
 * connections and let them handle it
 *
 * @param mac_addr that message was sent to
 * @param status   Whether the transmission succeeded or failed
 */
void ICACHE_FLASH_ATTR ringEspNowSendCb(uint8_t* mac_addr, mt_tx_status status)
{
    uint8_t i;
    sendCbCnt++;
    for(i = 0; i < lengthof(connections); i++)
    {
        p2pSendCb(&(connections[i]), mac_addr, status, sendCbCnt);
    }
    ringUpdateDisplay();
}


/**
 * Callback function when p2p connection events occur.
 * TODO Removed:Whenever a connection starts, halt all the other p2ps from connecting.
 *
 * @param p2p The p2p struct which emitted a connection event
 * @param evt The connection event
 */
void ICACHE_FLASH_ATTR ringConCbFn(p2pInfo* p2p, connectionEvt_t evt)
{
    // Get the msgId (label) for debugging
    char* conStr = getRingConnection(p2p)->msgId;

    switch(evt)
    {
        case CON_BROADCAST_STARTED:
        {
            ringPrintf("%s: CON_BROADCAST_STARTED\n", conStr);
            break;
        }

        case CON_LISTENING_STARTED:
        {
            ringPrintf("%s: CON_LISTENING_STARTED\n", conStr);
            break;
        }

        case CON_STOPPED:
        {
            ringPrintf("%s: CON_STOPPED\n", conStr);
            break;
        }
        case RX_BROADCAST:
            ringPrintf("%s: %s\n", conStr, "RX_BROADCAST");
            break;
        case RX_GAME_START_ACK:
            ringPrintf("%s: %s\n", conStr, "RX_GAME_START_ACK");
            break;
        case RX_GAME_START_MSG:
        {
            ringPrintf("%s: %s\n", conStr, "RX_GAME_START_MSG");
            // Set other connections ringSeq
            uint8_t otherIdx = getRingConnection(p2p)->side == LEFT ? RIGHT_SIDE : LEFT_SIDE;
            ringPrintf(" this other con made=%d, other other con made=%d\n", getRingConnection(p2p)->otherConnectionMade,
                       connections[otherIdx].otherConnectionMade);

            if(!getRingConnection(p2p)->otherConnectionMade)
            {
                connections[otherIdx].ringSeq = getRingConnection(p2p)->ringSeq;
            }
            // As soon as one connection starts, stop the others
            // TODO  does not seem to be needed so comment out. Check why was done
            // uint8_t i;
            // for(i = 0; i < lengthof(connections); i++)
            // {
            //     if(p2p != &connections[i])
            //     {
            //         p2pStopConnection(&connections[i]);
            //     }
            // }
            break;
        }
        case CON_ESTABLISHED:
        {
            // TODO maybe later if allow otherSide to be decided when connection is made
            // When a connection is established, save the current side to that connection
            //getRingConnection(p2p)->cnc.otherSide = connectionSide;
            ringPrintf("%s: CON_ESTABLISHED on side %d to side %d\n", conStr, getRingConnection(p2p)->side,
                       getRingConnection(p2p)->cnc.otherSide);
            // TODO maybe set ringSeq here
            break;
        }
        case CON_LOST:
        {
            // TODO maybe later if allow otherSide to be decided when connection is made
            // When a connection is lost, clear that side
            //getRingConnection(p2p)->cnc.otherSide = 0x00;
            // When a connection is lost, change isConnected flag
            getRingConnection(p2p)->cnc.isConnected = false;
            ringPrintf("%s: CON_LOST\n", conStr);
            break;
        }
        default:
        {
            ringPrintf("%s: Unknown event %d\n", conStr, evt);
            break;
        }
    }
    ringUpdateDisplay();
    return;
}

/**
 * Callback function when p2p receives a message. Draw a little animation if
 * the message is correct
 *
 * @param p2p     The p2p struct which received a message
 * @param msg     The label for the received message
 * @param payload The payload for the received message
 * @param len     The length of the payload
 */
void ICACHE_FLASH_ATTR ringMsgRxCbFn(p2pInfo* p2p, char* msg, uint8_t* payload, uint8_t len)
{
    if(0 == strcmp(msg, TST_LABEL))
    {
        if(RIGHT == getRingConnection(p2p)->side)
        {
            radiusRight = 1;
            ringHueRight = p2pHex2Int(payload[6]) * 16 + p2pHex2Int(payload[7]);
            ring_printf("ringHueRight = %d\n", ringHueRight);
        }
        else if(LEFT == getRingConnection(p2p)->side)
        {
            radiusLeft = 1;
            ringHueLeft = p2pHex2Int(payload[6]) * 16 + p2pHex2Int(payload[7]);
            ring_printf("ringHueLeft = %d\n", ringHueLeft);
        }
    }
    else if(0 == strcmp(msg, "rst"))
    {
        ring_printf("RST message received\n");
        for(uint8_t i = 0; i < lengthof(connections); i++)
        {
            {
                p2pStopConnection(&connections[i]);
            }
        }

        if(RIGHT == getRingConnection(p2p)->side)
        {
            radiusRight = 1;
            ringHueRight = p2pHex2Int(payload[6]) * 16 + p2pHex2Int(payload[7]);
            ring_printf("REPAIR ringHueRight = %d\n", ringHueRight);
        }
        else if(LEFT == getRingConnection(p2p)->side)
        {
            radiusLeft = 1;
            ringHueLeft = p2pHex2Int(payload[6]) * 16 + p2pHex2Int(payload[7]);
            ring_printf("REPAIR ringHueLeft = %d\n", ringHueLeft);
        }
    }


    ringPrintf("got msg=%s %s (%d bytes) from %s\n", msg, payload, len,
               getRingConnection(p2p)->msgId);
    ringUpdateDisplay();
    return;
}

/**
 * Callback function when p2p sends a message. If the message failed, treat
 * that side as disconnected
 *
 * @param p2p    The p2p struct which sent a message
 * @param status The status of the transmission
 */
void ICACHE_FLASH_ATTR ringMsgTxCbFn(p2pInfo* p2p, messageStatus_t status)
{
    // Get the label for debugging
    char* conStr = getRingConnection(p2p)->msgId;

    switch(status)
    {
        case MSG_ACKED:
        {
            // Message acked, do nothing
            ringPrintf("%s: MSG_ACKED\n", conStr);
            break;
        }
        case MSG_FAILED:
        {
            // Message failed, disconnect that side
            ringPrintf("%s: MSG_FAILED\n", conStr);
            //TODO do we want this or is there another way to indicate connection broken???
            //getRingConnection(p2p)->cnc.otherSide = 0x00;
            getRingConnection(p2p)->cnc.isConnected = false;
            //TODO check
            getRingConnection(p2p)->cnc.isConnecting = false; // needed?
            getRingConnection(p2p)->cnc.broadcastReceived = false;
            getRingConnection(p2p)->cnc.rxGameStartAck = false;
            getRingConnection(p2p)->cnc.rxGameStartMsg = false;
            getRingConnection(p2p)->cnc.otherMacReceived = false;
            break;
        }
        default:
        {
            ringPrintf("%s: Unknown status %d\n", conStr, status);
            break;
        }
    }
    ringUpdateDisplay();
    return;
}

/**
 * Given a side, left or right, return the connection for that side. If there is
 * no connection, return NULL
 *
 * @param side The side to return a connection for, LEFT or RIGHT
 * @return A pointer to the connection if it exists, or NULL
 */
p2pInfo* ICACHE_FLASH_ATTR getSideConnection(button_mask side)
{
    // For each connection
    uint8_t i;
    for(i = 0; i < lengthof(connections); i++)
    {
        // If the side matches
        if(side == connections[i].side)
        {
            // Return it
            return &connections[i];
        }
    }
    // No connections found
    return NULL;
}

/**
 * Given a p2p struct pointer, return the connection for that pointer. If there
 * is no connection, return NULL
 *
 * @param p2p The p2p to find a p2pInfo for
 * @return A pointer to the connection if it exists, or NULL
 */
p2pInfo* ICACHE_FLASH_ATTR getRingConnection(p2pInfo* p2p)
{
    // For each connection
    uint8_t i;
    for(i = 0; i < lengthof(connections); i++)
    {
        // If the p2p pointer matches
        if(p2p == &connections[i])
        {
            // Return it
            return &connections[i];
        }
    }
    // No connections found
    return NULL;
}

/**
 * Update the OLED and LEDs
 */
void ICACHE_FLASH_ATTR ringUpdateDisplay(void)
{
    uint8_t i;
    char macStr[6];
    char sidesStr[4];
    uint32_t colorToShow;
    clearDisplay();

    plotText(45, 0, &connections[0].macStr[12], IBM_VGA_8, WHITE);


    // Draw lastMsg ticker ringLastMsgTextIdx counts down by 1 every 50ms
    int16_t plotTextOut = plotText(ringLastMsgTextIdx, 12, lastMsg, IBM_VGA_8, WHITE);
    // repeat text again, so gives continuous scroll of text
    plotText(SCROLL_GAP + plotTextOut, 12, lastMsg, IBM_VGA_8, WHITE);
    if (0 > plotTextOut)
    {
        ringLastMsgTextIdx = SCROLL_GAP;
    }

    for(i = 0; i < lengthof(connections); i++)
    {

        if (connections[i].cnc.isConnected)
        {
            os_snprintf(sidesStr, sizeof(sidesStr), "%01d%01d+", connections[i].side,
                        connections[i].cnc.otherSide);
            plotText(104 * i, 0, sidesStr, IBM_VGA_8, WHITE);
        }
        else if (connections[i].cnc.isConnecting)
        {
            os_snprintf(sidesStr, sizeof(sidesStr), "%01d%01d-", connections[i].side,
                        connections[i].cnc.otherSide);
            plotText(104 * i, 0, sidesStr, IBM_VGA_8, WHITE);
        }
        else
        {
            os_snprintf(sidesStr, sizeof(sidesStr), "%01d%01d ", connections[i].side,
                        connections[i].cnc.otherSide);
            plotText(104 * i, 0, sidesStr, IBM_VGA_8, WHITE);
        }


        if(connections[i].cnc.otherMacReceived)
        {
            os_snprintf(macStr, sizeof(macStr), "%02X:%02X", connections[i].cnc.otherMac[4],
                        connections[i].cnc.otherMac[5]);
            plotText(6 + 80 * i, 24, macStr, IBM_VGA_8, WHITE);
        }
        if (connections[i].cnc.playOrder > 0)
        {
            os_snprintf(macStr, sizeof(macStr), "p%d %02X", connections[i].cnc.playOrder, connections[i].ringSeq);
            plotText(6 + 80 * i, 36, macStr, IBM_VGA_8, WHITE);
        }
        if(connections[i].ack.isWaitingForAck)
        {
            plotText(6 + 80 * i, 48, "ack ?", IBM_VGA_8, WHITE);
        }
    }

    //Clear leds
    ets_memset(leds, 0, sizeof(leds));

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

        for(i = 0; i < lengthof(connections); i++)
        {
            if(connections[i].cnc.isConnecting)
            {
                leds[ledCnOrderInd[4 * i]].r = 255;
            }
            else if (connections[i].cnc.isConnected)
            {
                leds[ledCnOrderInd[4 * i]].g = 255;
            }
            else if (connections[i].cnc.broadcastReceived)
            {
                leds[ledCnOrderInd[4 * i]].r = 255;
                leds[ledCnOrderInd[4 * i]].g = 255;
            }

            if(connections[i].cnc.otherMacReceived)
            {
                leds[ledCnOrderInd[4 * i + 1]].r = 255;
            }
            if (connections[i].cnc.rxGameStartAck)
            {
                leds[ledCnOrderInd[4 * i + 1]].g = 255;
            }
            if (connections[i].cnc.rxGameStartMsg)
            {
                leds[ledCnOrderInd[4 * i + 1]].b = 255;
            }
        }
    }
    ringSetLeds(leds, sizeof(leds));
}


/**
 * @brief Decrement the index to draw the instructions at, then draw the menu
 *
 * @param arg unused
 */
void ICACHE_FLASH_ATTR ringScrollLastMsg(void* arg __attribute__((unused)))
{
    ringLastMsgTextIdx--;
}

/**
 * Timer function for animation
 *
 * @param arg unused
 */
void ICACHE_FLASH_ATTR ringAnimation(void* arg __attribute__((unused)))
{
    // If the radius is nonzero
    if(radiusLeft > 0)
    {
        // Make the circle bigger
        radiusLeft++;
        // If the radius is 30px
        if(radiusLeft == 30)
        {
            // Stop drawing the circle
            radiusLeft = 0;
        }
    }

    // If the radius is nonzero
    if(radiusRight > 0)
    {
        // Make the circle bigger
        radiusRight++;
        // If the radius is 30px
        if(radiusRight == 30)
        {
            // Stop drawing the circle
            radiusRight = 0;
        }
    }
    // Update OLED
    ringUpdateDisplay();
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

/**
 * Timer started when a button is pressed.
 * If it expires before the button is released, set the long press flag
 * If it doesn't expire before the button is released, switch the mode params
 *
 * @param arg unused
 */
void ICACHE_FLASH_ATTR ringLongPressTimerFunc(void* arg __attribute__((unused)))
{
    ringLongPress = true;
}


/**
 * @brief AccelerometerCallback
 *
 * @param accel (not used)
 */
void ICACHE_FLASH_ATTR ringAccelerometerCallback(accel_t* accel __attribute__((unused)))
{
    //Doing nothing here
    return;
}
