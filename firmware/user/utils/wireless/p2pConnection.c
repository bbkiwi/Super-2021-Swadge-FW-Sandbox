// Change to test for con broadcast look for msg con rather than length of message
// TODO side and otherSide could be renamed socket and otherSocket
// TODO Why acking all messages, test code that only acks first of sequence of duplicates
//      rather than all
// Added cnc.msgRepeatCnt so when get repeat messages can see if any get missed
// TODO using 1 byte (char) for cnc.mySeqNum; and cnc.msgRepeatCnt. Using str
// Note since now messages may contain a zero byte, do NOT use
//    ets_strlen(p2p->startMsg) but sizeof(p2p->startMsg) - 1 instead
//    op to make message and testing str length will cause problem is the byte
//    is ever zero as thinks this is end of string and gets length wrong.
//    maybe make len part of header of message
// Note ack message contains copy of message it is acking. Prob can remove (just for testing)
// Some storage may be overwritten
// Not working now
/*============================================================================
 * Includes
 *==========================================================================*/
#include <osapi.h>
#include <user_interface.h>
#include <mem.h>

#include "user_main.h"
#include "p2pConnection.h"
#include "espNowUtils.h"
/*============================================================================
 * Defines
 *==========================================================================*/
//#define HANDLE_CON_FROM_RESTARTED_SWADGE
//#define HANDLE_MSG_TO_RESTARTED_SWADGE
//#define SHOW_DISCARD
//#define SHOW_DUMP
// TEST_SEND_ID identifies which p2pSendCb is associated
//    with a send by assigning an id to each espnowsend.
//    This is stored in the p2pInfo of the connection that does the send
// used when ALWAYS_BROADCAST is defined in espNowUtils.c
//    (this is how adam original works)
// If ALWAYS_BROADCAST is not defined, direct sending to otherMac
//     is used, and alternative code is used in p2pSendCb
#ifdef ALWAYS_BROADCAST
    #define TEST_SEND_ID
#endif

#define P2P_DEBUG_PRINT
#ifdef P2P_DEBUG_PRINT
    #define p2p_printf(...) do{os_printf("%s::%d ", __func__, __LINE__); os_printf(__VA_ARGS__);}while(0)
#else
    #define p2p_printf(...)
#endif

// The time we'll spend retrying messages
#define RETRY_TIME_MS 3000

// Time to wait between connection events and game rounds.
// Transmission can be 3s (see above), the round @ 12ms period is 3.636s
// (240 steps of rotation + (252/4) steps of decay) * 12ms
#define FAILURE_RESTART_MS 8000

// TODO CHECK THESE INDICES
// Indices into con message
#define CON_RING_SEQ_IDX 10
#define CON_SIDE_IDX 8

// Indices into messages
#define CMD_IDX 4
#define SEQ_IDX 8
#define INITIAL_PART 8
#define MSG_REPEAT_NUM 9
#define MAC_IDX 11
#define EXT_IDX 29
#define RING_SEQ_IDX 32
#define SIDE_IDX 29
#define OTHER_SIDE_IDX 30
#define MODE_CMD_IDX 35
/*============================================================================
 * Variables
 *==========================================================================*/
//TODO could refactor with messages made of char (bytes), mac is 6 bytes, seq 1 byte etc.
//.    change debug and sniff to parse in readable form.
// TODO change refs to fields to get side, otherSide and ringSeq
//     also must be careful how compute length of the messages as strLen not work if any zero bytes

// Messages to send.
const char p2pConnectionMsgFmt[] = "%s_con_%1X_%02X";

const char p2pNoPayloadMsgFmt[]  = "%s_%s_%c%c_%02X:%02X:%02X:%02X:%02X:%02X_%1X%1X_%02X";

const char p2pPayloadMsgFmt[]    = "%s_%s_%c%c_%02X:%02X:%02X:%02X:%02X:%02X_%1X%1X_%02X_%s";
const char p2pMacFmt[] = "%02X:%02X:%02X:%02X:%02X:%02X";

// Every send if given a unique sequence number
uint16_t p2pEspSendId;

/*============================================================================
 * Function Prototypes
 *==========================================================================*/

void ICACHE_FLASH_ATTR p2pConnectionTimeout(void* arg);
void ICACHE_FLASH_ATTR p2pTxAllRetriesTimeout(void* arg);
void ICACHE_FLASH_ATTR p2pTxRetryTimeout(void* arg);
void ICACHE_FLASH_ATTR p2pStartRestartTimer(void* arg);
void ICACHE_FLASH_ATTR p2pProcConnectionEvt(p2pInfo* p2p, connectionEvt_t event);
void ICACHE_FLASH_ATTR p2pGameStartAckRecv(void* arg);
void ICACHE_FLASH_ATTR p2pSendAckToMac(p2pInfo* p2p, uint8_t* mac_addr, uint8_t* data);
void ICACHE_FLASH_ATTR p2pSendMsgEx(p2pInfo* p2p, char* msg, uint16_t len,
                                    bool shouldAck, void (*success)(void*), void (*failure)(void*));
void ICACHE_FLASH_ATTR p2pModeMsgSuccess(void* arg);
void ICACHE_FLASH_ATTR p2pModeMsgFailure(void* arg);

/*============================================================================
 * Functions
 *==========================================================================*/

/**
 * @brief Initialize the p2p connection protocol
 *
 * @param p2p           The p2pInfo struct with all the state information
 * @param msgId         A three character, null terminated message ID. Must be
 *                      unique among all swadge modes.
 * @param conCbFn A function pointer which will be called when connection
 *                      events occur
 * @param msgRxCbFn A function pointer which will be called when a packet
 *                      is received for the swadge mode
 * @param connectionRssi The strength needed to start a connection with another
 *                      swadge, 0 is first one to see around 55 the swadges need
 *                      to be right next to eachother.
 */
void ICACHE_FLASH_ATTR p2pInitialize(p2pInfo* p2p, char* msgId,
                                     p2pConCbFn conCbFn,
                                     p2pMsgRxCbFn msgRxCbFn, uint8_t connectionRssi)
{
    p2p_printf("%s\r\n", msgId);
    // Make sure everything is zero!
    ets_memset(p2p, 0, sizeof(p2pInfo));
    // Set the callback functions for connection and message events
    p2p->conCbFn = conCbFn;
    p2p->msgRxCbFn = msgRxCbFn;

    // Set the initial sequence number at 255 so that a 0 received is valid.
    p2p->cnc.lastSeqNum = 0x7A;
    p2p->cnc.mySeqNum = 0x30;
    p2p->cnc.msgRepeatCnt = 0x30;


    //TODO remove later when can start at 0
    p2p->ringSeq = 0x7F;

    // Set the connection Rssi, higher value, swadges need to be close
    p2p->connectionRssi = connectionRssi;

    // Set the three character message ID
    ets_strncpy(p2p->msgId, msgId, sizeof(p2p->msgId));

    // Get and save the string form of our MAC address
    uint8_t mymac[6];
    wifi_get_macaddr(SOFTAP_IF, mymac);
    ets_snprintf(p2p->macStr, sizeof(p2p->macStr), p2pMacFmt,
                 mymac[0],
                 mymac[1],
                 mymac[2],
                 mymac[3],
                 mymac[4],
                 mymac[5]);

    // Set up dummy connection message
    ets_snprintf(p2p->conMsg, sizeof(p2p->conMsg), p2pConnectionMsgFmt,
                 p2p->msgId, 0, 0);

    // Set up dummy ACK message
    // TODO is this needed?
    ets_snprintf(p2p->ackMsg, sizeof(p2p->ackMsg), p2pNoPayloadMsgFmt,
                 p2p->msgId,
                 "ack",
                 0x30,
                 0x30,
                 0xFF,
                 0xFF,
                 0xFF,
                 0xFF,
                 0xFF,
                 0xFF,
                 0,
                 0,
                 0);

    // Set up dummy start message
    // TODO could use message to FF:FF ... as broadcast but might interfere with other games of same type
    ets_snprintf(p2p->startMsg, sizeof(p2p->startMsg), p2pNoPayloadMsgFmt,
                 p2p->msgId,
                 "str",
                 0x30,
                 0x30,
                 0xFF,
                 0xFF,
                 0xFF,
                 0xFF,
                 0xFF,
                 0xFF,
                 0,
                 0,
                 0);

    // Set up a timer for acking messages
    syncedTimerDisarm(&p2p->tmr.TxRetry);
    syncedTimerSetFn(&p2p->tmr.TxRetry, p2pTxRetryTimeout, p2p);

    // Set up a timer for when a message never gets ACKed
    syncedTimerDisarm(&p2p->tmr.TxAllRetries);
    syncedTimerSetFn(&p2p->tmr.TxAllRetries, p2pTxAllRetriesTimeout, p2p);

    // Set up a timer to restart after abject failure
    syncedTimerDisarm(&p2p->tmr.Reinit);
    syncedTimerSetFn(&p2p->tmr.Reinit, p2pRestart, p2p);

    // Set up a timer to do an initial connection
    syncedTimerDisarm(&p2p->tmr.Connection);
    syncedTimerSetFn(&p2p->tmr.Connection, p2pConnectionTimeout, p2p);
}

/**
 * Start the connection
 *
 * @param p2p The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pStartConnection(p2pInfo* p2p)
{
    // if (!p2p->otherConnectionMade || p2p->subsequentStartListenFirst)
    // {
    //     p2pStartConnectionListening(p2p);
    // }
    // if (!p2p->otherConnectionMade || !p2p->subsequentStartListenFirst)
    // {
    //     p2pStartConnectionBroadcast(p2p);
    // }


    p2pStartConnectionListening(p2p);
    if (!p2p->otherConnectionMade || p2p->longPushButton)
    {
        p2pStartConnectionBroadcast(p2p);
    }
}

/**
 * Start the connection broadcasts and notify the mode
 *
 * @param p2p The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pStartConnectionBroadcast(p2pInfo* p2p)
{
    p2p_printf("%s %s\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT");
    //TODO what happens if arm a timer that is already armed?
    syncedTimerArm(&p2p->tmr.Connection, 1, false);

    if(NULL != p2p->conCbFn)
    {
        p2p->conCbFn(p2p, CON_BROADCAST_STARTED);
    }
}

/**
 * Start connection listening and notify the mode
 *
 * @param p2p The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pStartConnectionListening(p2pInfo* p2p)
{
    p2p_printf("%s %s\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT");
    p2p->cnc.isConnecting = true;

    if(NULL != p2p->conCbFn)
    {
        p2p->conCbFn(p2p, CON_LISTENING_STARTED);
    }
}

/**
 * Stop a connection in progress. If the connection is already established,
 * this does nothing
 *
 * @param p2p The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pStopConnection(p2pInfo* p2p)
{
    if(true == p2p->cnc.isConnecting)
    {
        p2p_printf("%s %s\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT");
        p2p->cnc.isConnecting = false;
        syncedTimerDisarm(&p2p->tmr.Connection);

        if(NULL != p2p->conCbFn)
        {
            p2p->conCbFn(p2p, CON_STOPPED);
        }

        p2pRestart((void*)p2p);
    }
}

/**
 * Stop up all timers and clear out p2p
 *
 * @param p2p The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pDeinit(p2pInfo* p2p)
{
    p2p_printf("%s %s\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT");

    ets_memset(&(p2p->msgId), 0, sizeof(p2p->msgId));
    ets_memset(&(p2p->conMsg), 0, sizeof(p2p->conMsg));
    ets_memset(&(p2p->ackMsg), 0, sizeof(p2p->ackMsg));
    ets_memset(&(p2p->startMsg), 0, sizeof(p2p->startMsg));

    p2p->conCbFn = NULL;
    p2p->msgRxCbFn = NULL;
    p2p->msgTxCbFn = NULL;
    p2p->connectionRssi = 0;

    ets_memset(&(p2p->cnc), 0, sizeof(p2p->cnc));
    ets_memset(&(p2p->ack), 0, sizeof(p2p->ack));

    syncedTimerDisarm(&p2p->tmr.Connection);
    syncedTimerDisarm(&p2p->tmr.TxRetry);
    syncedTimerDisarm(&p2p->tmr.Reinit);
    syncedTimerDisarm(&p2p->tmr.TxAllRetries);
}

/**
 * Send a broadcast connection message
 *
 * Called periodically, with some randomness mixed in from the tmr.Connection
 * timer. The timer is set when connection starts and is stopped when we
 * receive a response to our connection broadcast
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pConnectionTimeout(void* arg)
{
    p2pInfo* p2p = (p2pInfo*)arg;
    // Send a connection broadcast with side required and ringSeq so next seq can be computed.
    ets_snprintf(p2p->conMsg, sizeof(p2p->conMsg), p2pConnectionMsgFmt,
                 p2p->msgId, p2p->side, p2p->ringSeq);
    p2pSendMsgEx(p2p, p2p->conMsg, sizeof(p2p->conMsg) - 1, false, NULL, NULL);

    // os_random returns a 32 bit number, so this is [500ms,1500ms]
    uint32_t timeoutMs = 100 * (5 + (os_random() % 11));

    // Start the timer again
    p2p_printf("%s %s retry broadcast in %dms\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT", timeoutMs);
    syncedTimerArm(&p2p->tmr.Connection, timeoutMs, false);
}

/**
 * Retries sending a message to be acked
 *
 * Called from the tmr.TxRetry timer. The timer is set when a message to be
 * ACKed is sent and cleared when an ACK is received
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pTxRetryTimeout(void* arg)
{
    p2pInfo* p2p = (p2pInfo*)arg;
    p2p_printf("%s %s\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT");

    if(p2p->ack.msgToAckLen > 0)
    {
        p2p_printf("Retrying message \"%s\"\r\n", p2p->ack.msgToAck);
        p2pSendMsgEx(p2p, p2p->ack.msgToAck, p2p->ack.msgToAckLen, true, p2p->ack.SuccessFn, p2p->ack.FailureFn);
    }
}

/**
 * Stops a message transmission attempt after all retries have been exhausted
 * and calls p2p->ack.FailureFn() if a function was given
 *
 * Called from the tmr.TxAllRetries timer. The timer is set when a message to
 * be ACKed is sent for the first time and cleared when the message is ACKed.
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pTxAllRetriesTimeout(void* arg)
{
    p2p_printf("\n");

    p2pInfo* p2p = (p2pInfo*)arg;
    p2p_printf("%s %s\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT");
    // Disarm all timers
    syncedTimerDisarm(&p2p->tmr.TxRetry);
    syncedTimerDisarm(&p2p->tmr.TxAllRetries);

    // Save the failure function
    void (*FailureFn)(void*) = p2p->ack.FailureFn;
    p2p_printf("Message totally failed \"%s\"\n", p2p->ack.msgToAck);

    // Clear out the ack variables
    ets_memset(&p2p->ack, 0, sizeof(p2p->ack));

    // Call the failure function
    if(NULL != FailureFn)
    {
        FailureFn(p2p);
    }
}

/**
 * Send a message from one Swadge to another.
 * TODO might allow This must not be called before the CON_ESTABLISHED event occurs.
 * TODO maybe if message is sent before CON_ESTABLISHED have receiving mac
 *      first take it as a request to connect, then look at actual message
 * Message addressing, ACKing, and retries
 * all happen automatically
 *
 * @param p2p       The p2pInfo struct with all the state information
 * @param msg       The mandatory three char message type
 * @param payload   An optional message payload string, may be NULL, up to 32 chars
 * //TODO can 32 chars be extended what is total length of espnow message?
*        payload has otherSide, side and ringSeq added
 * @param len       The length of the optional message payload string. May be 0
 * @param msgTxCbFn A callback function when this message is ACKed or dropped
 */
void ICACHE_FLASH_ATTR p2pSendMsg(p2pInfo* p2p, char* msg, char* payload,
                                  uint16_t len, p2pMsgTxCbFn msgTxCbFn)
{
    p2p_printf("%s %s len=%d, payload=%s\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT", len, payload);
    // TODO changed
    char builtMsg[85] = {0}; // so longest message is 85 chars

    if(NULL == payload || len == 0)
    {
        ets_snprintf(builtMsg, sizeof(builtMsg), p2pNoPayloadMsgFmt,
                     p2p->msgId,
                     msg,
                     0x30, // sequence number
                     0x30,
                     p2p->cnc.otherMac[0],
                     p2p->cnc.otherMac[1],
                     p2p->cnc.otherMac[2],
                     p2p->cnc.otherMac[3],
                     p2p->cnc.otherMac[4],
                     p2p->cnc.otherMac[5],
                     p2p->cnc.otherSide,
                     p2p->side,
                     p2p->ringSeq);
    }
    else
    {
        ets_snprintf(builtMsg, sizeof(builtMsg), p2pPayloadMsgFmt,
                     p2p->msgId,
                     msg,
                     0x30, // sequence number, filled in later
                     0x30,
                     p2p->cnc.otherMac[0],
                     p2p->cnc.otherMac[1],
                     p2p->cnc.otherMac[2],
                     p2p->cnc.otherMac[3],
                     p2p->cnc.otherMac[4],
                     p2p->cnc.otherMac[5],
                     p2p->cnc.otherSide,
                     p2p->side,
                     p2p->ringSeq,
                     payload);
    }

    p2p->msgTxCbFn = msgTxCbFn;
    //TODO this could be a problem, maybe if payload (which is for user) is string
    // compute length as 34 + 1 + ets_strlen(payload) instead of ets_strlen(builtMsg)
    p2p_printf("ets_strlen(payload)=%d, builtMsg %s\n", ets_strlen(payload), builtMsg);
    p2pSendMsgEx(p2p, builtMsg, 35 + ets_strlen(payload), true, p2pModeMsgSuccess, p2pModeMsgFailure);
}

/**
 * Callback function for when a message sent by the Swadge mode, not during
 * the connection process, is ACKed
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pModeMsgSuccess(void* arg)
{
    p2p_printf("\n");

    p2pInfo* p2p = (p2pInfo*)arg;
    p2p_printf("%s %s\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT");
    if(NULL != p2p->msgTxCbFn)
    {
        p2p->msgTxCbFn(p2p, MSG_ACKED);
    }
}

/**
 * Callback function for when a message sent by the Swadge mode, not during
 * the connection process, is dropped
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pModeMsgFailure(void* arg)
{
    p2p_printf("\n");

    p2pInfo* p2p = (p2pInfo*)arg;
    p2p_printf("%s %s\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT");
    if(NULL != p2p->msgTxCbFn)
    {
        p2p->msgTxCbFn(p2p, MSG_FAILED);
    }
}

/**
 * Wrapper for sending an ESP-NOW message. Handles ACKing and retries for
 * non-broadcast style messages
 * TODO Improve what a broadcast style message is
 *
 * @param p2p       The p2pInfo struct with all the state information
 * @param msg       The message to send, if NOT con broadcast, will contain destination MAC, its side and otherSide
 * @param len       The length of the message to send
 * @param shouldAck true if this message should be acked, false if we don't care
 * @param success   A callback function if the message is acked. May be NULL
 * @param failure   A callback function if the message isn't acked. May be NULL
 */
void ICACHE_FLASH_ATTR p2pSendMsgEx(p2pInfo* p2p, char* msg, uint16_t len,
                                    bool shouldAck, void (*success)(void*), void (*failure)(void*))
{
    os_printf("\n");
    //p2p_printf("%s,%s\r\n", p2p->msgId, msg);
    p2p_printf("%s %s\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT");
    // If this is a first time message and longer than a connection message
    // TODO note the assumption is that all messages longer than con will be acked
    // If want to be able to broadcast long messages will need to change this
    if (sizeof(p2p->conMsg) - 1 < len)
    {
        if(p2p->ack.msgToAck != msg)
        {
            // TODO is there better place for this code? When send str? and/or when receive con
            // save otherMac the first time we need to send
            if (!p2p->cnc.otherMacReceived)
            {
                p2p->cnc.otherMacReceived = true;
                for (uint8_t i = 0; i < 6; i++)
                {
                    p2p->cnc.otherMac[i] = 16 * p2pHex2Int(msg[MAC_IDX + 3 * i]) + p2pHex2Int(msg[MAC_IDX + 3 * i + 1]);
                }
            }
            // Sequence number 1 byte 0 to 0xFF
            // Insert a sequence number
            msg[SEQ_IDX] = p2p->cnc.mySeqNum;

            // Increment the sequence number
            p2p->cnc.mySeqNum++;

            // Set repeat counter to zero
            p2p->cnc.msgRepeatCnt = 0x30;
        }
        else
        {
            p2p->cnc.msgRepeatCnt++;
        }
        msg[MSG_REPEAT_NUM] = p2p->cnc.msgRepeatCnt;
    }

#ifdef P2P_DEBUG_PRINT
    char* dbgMsg = (char*)os_zalloc(sizeof(char) * (len + 1));
    ets_memcpy(dbgMsg, msg, len);
    p2p_printf("len=%d, %s\r\n", len, dbgMsg);
    os_free(dbgMsg);
#endif

    if(shouldAck)
    {
        // Set the state to wait for an ack
        p2p->ack.isWaitingForAck = true;

        // If this is not a retry
        if(p2p->ack.msgToAck != msg)
        {
            p2p_printf("sending for the first time\r\n");

            // Store the message for potential retries
            ets_memcpy(p2p->ack.msgToAck, msg, len);
            p2p->ack.msgToAckLen = len;
            p2p->ack.SuccessFn = success;
            p2p->ack.FailureFn = failure;

            // Start a timer to retry for 3s total
            syncedTimerDisarm(&p2p->tmr.TxAllRetries);
            syncedTimerArm(&p2p->tmr.TxAllRetries, RETRY_TIME_MS, false);
        }
        else
        {
            p2p_printf("this is a retry\r\n");
        }

        // Mark the time this transmission started, the retry timer gets
        // started in p2pSendCb()
        p2p->ack.timeSentUs = system_get_time();
        p2p_printf("   time started %u\n", p2p->ack.timeSentUs);
    }
    //TODO NOTE see start of code, using mod of espNowSend which can send to specific or broadcast

    // Update send id
    p2pEspSendId++;
    p2p->sendId = p2pEspSendId;
    p2p_printf("%s side sending message with id=%d\n", p2p->side == LEFT ? "LEFT" : "RIGHT", p2p->sendId);
    espNowSend(p2p->cnc.otherMac, (const uint8_t*)msg, len);
}

button_mask ICACHE_FLASH_ATTR  p2pHex2Int(uint8_t in)
{
    if(((in >= '0') && (in <= '9')))
    {
        return in - '0';
    }
    if(((in >= 'A') && (in <= 'F')))
    {
        return in - 'A' + 10;
    }
    if(((in >= 'a') && (in <= 'f')))
    {
        return in - 'a' + 10;
    }
    return 0xF;
}

/**
 * This is must be called whenever an ESP NOW packet is received
 *
 * @param p2p      The p2pInfo struct with all the state information
 * @param senders_mac_addr The MAC of the swadge that sent the data
 * @param data     The data
 * @param len      The length of the data
 * @param rssi     The RSSI of th received message, a proxy for distance
 * TODO fix so actually returns what says on the next line
 * @return false if the message was processed here,
 *         true if the message should be processed by the swadge mode
 */
void ICACHE_FLASH_ATTR p2pRecvCb(p2pInfo* p2p, uint8_t* senders_mac_addr, uint8_t* data, uint8_t len, uint8_t rssi,
                                 uint16_t recvCbCnt)
{
    bool needToAck = false;
#ifdef P2P_DEBUG_PRINT
#ifdef SHOW_DISCARD
    os_printf("\n");
    char* dbgMsg = (char*)os_zalloc(sizeof(char) * (len + 1));
    ets_memcpy(dbgMsg, data, len);
    p2p_printf("%s %s %s\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT", dbgMsg);
    os_free(dbgMsg);
#endif
#endif

    // Check if this message matches our message ID
    if(len < CMD_IDX ||
            (0 != ets_memcmp(data, p2p->conMsg, CMD_IDX)))
    {
        // This message is too short, or does not match our message ID
#ifdef SHOW_DISCARD
        p2p_printf("DISCARD: Not a message for '%s'\r\n", p2p->msgId);
#endif
        return;
    }

    // If this message has a MAC, check it
    if(len >= sizeof(p2p->startMsg) - 1 &&
            0 != ets_memcmp(&data[MAC_IDX], p2p->macStr, sizeof(p2p->macStr) - 1))
    {
        // This MAC isn't for us
#ifdef SHOW_DISCARD
        p2p_printf("DISCARD: Not for our MAC len=%d sizeof(p2p->startMsg)-1=%d\r\n", len, sizeof(p2p->startMsg) - 1 );
#endif
        return;
    }

    // If this is anything besides a con broadcast, check its side and otherSide
    if(     0 != ets_memcmp(data, p2p->conMsg, CON_SIDE_IDX) &&
            (  data[SIDE_IDX]  != '0' + p2p->side  ||
               data[SIDE_IDX + 1]  != '0' + p2p->cnc.otherSide ))
    {
        // This directed message from wrong side, otherSide
#ifdef SHOW_DISCARD
        p2p_printf("DISCARD: From (side, otherSide) = (%d, %d) expect (%d, %d)\r\n", data[SIDE_IDX] - '0',
                   data[SIDE_IDX + 1] - '0', p2p->side, p2p->cnc.otherSide);
#endif
        return;
    }

    // If this is anything besides a con broadcast, check the other MAC
    if(p2p->cnc.otherMacReceived &&
            0 != ets_memcmp(data, p2p->conMsg, CON_SIDE_IDX) &&
            0 != ets_memcmp(senders_mac_addr, p2p->cnc.otherMac, sizeof(p2p->cnc.otherMac)))
    {
        // This directed message not from the other known swadge
#ifdef SHOW_DISCARD
        p2p_printf("DISCARD: From wrong MAC\r\n");
#endif
        return;
    }


    // If this is a con broadcast, check otherSide matches side requesting con
    if(
        0 == ets_memcmp(data, p2p->conMsg, CON_SIDE_IDX) &&
        data[CON_SIDE_IDX]  != '0' + p2p->cnc.otherSide )
    {
        // This con broadcast for wrong side
#ifdef SHOW_DISCARD
        p2p_printf("DISCARD: con broadcast from side %d not match otherSide %d\r\n", data[CON_SIDE_IDX] - '0',
                   p2p->cnc.otherSide);
#endif
        return;
    }

    // If this is a con broadcast for correct side, but we are not connecting (or already connected) there
    if(
        0 == ets_memcmp(data, p2p->conMsg, CON_SIDE_IDX) &&
        data[CON_SIDE_IDX]  == '0' + p2p->cnc.otherSide && !p2p->cnc.isConnecting)
    {
        // This con broadcast for our side but we are already connected on that side
#ifdef SHOW_DISCARD
        p2p_printf("DISCARD: con broadcast from side %d but we are not connecting (or already connected) there\r\n",
                   data[CON_SIDE_IDX] - '0');
#endif
        return;
    }

#ifdef P2P_DEBUG_PRINT
#ifndef SHOW_DISCARD
    os_printf("\n");
    char* dbgMsg = (char*)os_zalloc(sizeof(char) * (len + 1));
    ets_memcpy(dbgMsg, data, len);
    p2p_printf("%d %s %s %s\r\n", recvCbCnt, p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT", dbgMsg);
    os_free(dbgMsg);
#endif
#endif

#ifdef HANDLE_CON_FROM_RESTARTED_SWADGE

    // Let the mode handle RESTART message
    // The mode should reset to state as if just started with no button pushes
    if(len >= sizeof(p2p->ackMsg) - 1 &&
            0 == ets_memcmp(&data[CMD_IDX], "rst", ets_strlen("rst")))
    {
        if(NULL != p2p->msgRxCbFn)
        {
            p2p_printf("letting mode handle RESTART message\r\n");
            char msgType[4] = {0};
            memcpy(msgType, &data[CMD_IDX], 3 * sizeof(char));
            if (len > EXT_IDX)
            {
                p2p->msgRxCbFn(p2p, msgType, &data[EXT_IDX], len - EXT_IDX);
            }
            else
            {
                //TODO should this be NULL, for mode_ring param ignored, but for other applications might use
                p2p->msgRxCbFn(p2p, msgType, NULL, 0);
            }
        }
        //TODO why putting return here stops it working
    }
#endif
    // By here, we know the received message matches our message ID and side and otherSide
    //       either a connection request broadcast or for message for us.
    //If not a con broadcast or for us and not an ack message
    if(0 != ets_memcmp(data, p2p->conMsg, CON_SIDE_IDX) &&
            0 != ets_memcmp(data, p2p->ackMsg, INITIAL_PART))
    {
#ifdef HANDLE_MSG_TO_RESTARTED_SWADGE
        // p2p needs repairing if we are not actually connected
        //Check if we are actually connected and if not
        //Could be receiving a (like str) message which is ok
        //     as maybe we were turned off then on
        // So  reconnect if isConnected is false and isConnecting is false
        //     using the info in the message
        // p2p_printf("%s BEFORE ACK\n", p2p->cnc.isConnected ? "CONNECTED" : "NOT CONNECTED");
        // p2p_printf("     Sender %s [%02X:%02X]\n", p2p->msgId, senders_mac_addr[4], senders_mac_addr[5]);
        // p2p_printf("     on side %X otherSide %X\n", p2p->side, p2p->cnc.otherSide);
        // p2p_printf("     isConnecting %s\n", p2p->cnc.isConnecting ? "TRUE" : "FALSE");
        // p2p_printf("     broadcastReceived %s\n", p2p->cnc.broadcastReceived ? "TRUE" : "FALSE");
        // p2p_printf("     rxGameStartMsg %s\n", p2p->cnc.rxGameStartMsg ? "TRUE" : "FALSE");
        // p2p_printf("     rxGameStartAck %s\n", p2p->cnc.rxGameStartAck ? "TRUE" : "FALSE");
        // p2p_printf("     playOrder %d\n", p2p->cnc.playOrder);
        // p2p_printf("     macStr %s\n", p2p->macStr);
        // p2p_printf("     %s [%02X:%02X]\n",
        //            p2p->cnc.otherMacReceived ? "OTHER MAC RECEIVED " : "NO OTHER MAC RECEIVED ",
        //            p2p->cnc.otherMac[4],
        //            p2p->cnc.otherMac[5]);
        // p2p_printf("     mySeqNum = %d, lastSeqNum = %d\n", p2p->cnc.mySeqNum, p2p->cnc.lastSeqNum);

        if (p2p->cnc.isConnecting == false && p2p->cnc.isConnected == false)
        {
            // Repair connection
            p2p->cnc.isConnected = true;
            p2p->cnc.isConnecting = false;
            p2p->cnc.broadcastReceived = true;
            p2p->cnc.rxGameStartAck = true;
            p2p->cnc.rxGameStartMsg = true;
            p2p->cnc.otherMacReceived = true;
            p2p->cnc.mySeqNum = 0;
            p2p->cnc.lastSeqNum = 255;
            uint8_t i;
            for (i = 0; i < 6; i++)
            {
                p2p->cnc.otherMac[i] = senders_mac_addr[i];
            }
            // Restore side and otherSide
            // TODO EVERYWHWERE use better index names
            p2p->side = p2pHex2Int(data[EXT_IDX + 1]);
            p2p->cnc.otherSide = p2pHex2Int(data[EXT_IDX + 0]);
            //TODO restore p2p->cnc.playOrder?

            p2p_printf("%s REPAIRED \n", p2p->cnc.isConnected ? "CONNECTED" : "NOT CONNECTED");
            p2p_printf("     Sender %s [%02X:%02X]\n", p2p->msgId, senders_mac_addr[4], senders_mac_addr[5]);
            p2p_printf("     on side %X otherSide %X\n", p2p->side, p2p->cnc.otherSide);
            p2p_printf("     isConnecting %s\n", p2p->cnc.isConnecting ? "TRUE" : "FALSE");
            p2p_printf("     broadcastReceived %s\n", p2p->cnc.broadcastReceived ? "TRUE" : "FALSE");
            p2p_printf("     rxGameStartMsg %s\n", p2p->cnc.rxGameStartMsg ? "TRUE" : "FALSE");
            p2p_printf("     rxGameStartAck %s\n", p2p->cnc.rxGameStartAck ? "TRUE" : "FALSE");
            p2p_printf("     playOrder %d\n", p2p->cnc.playOrder);
            p2p_printf("     macStr %s\n", p2p->macStr);
            p2p_printf("     %s [%02X:%02X]\n",
                       p2p->cnc.otherMacReceived ? "OTHER MAC RECEIVED " : "NO OTHER MAC RECEIVED ",
                       p2p->cnc.otherMac[4],
                       p2p->cnc.otherMac[5]);
            p2p_printf("     mySeqNum = %d, lastSeqNum = %d\n", p2p->cnc.mySeqNum, p2p->cnc.lastSeqNum);
        }
#endif
        // Acknowledge message
        // TODO Why acking all messages, why not first one we get see below
        needToAck = true;
        // TODO test! But maybe this is safer need to test
        // p2pSendAckToMac(p2p, senders_mac_addr, data);
    }

    // All messages come here (con, ack, others that needed acking)
    // Those that needed it were ACKed the message
    // Check the sequence number to see if we should process it or ignore it (we already did!)
    if(len >= sizeof(p2p->startMsg) - 1) // these are not broadcasts and have seq numbers
    {
        // TODO fix using hex seq numbers
        // Extract the sequence number
        uint8_t theirSeq = data[SEQ_IDX];

        // Check it against the last known sequence number
        if(theirSeq == p2p->cnc.lastSeqNum)
        {
            p2p_printf("DISCARD: Duplicate sequence number\r\n");
            return;
        }
        else
        {
            //TODO need to test
            if (needToAck)
            {
                needToAck = false;
                p2pSendAckToMac(p2p, senders_mac_addr, data);
            }
            p2p->cnc.lastSeqNum = theirSeq;
            p2p_printf("Letting message thru, storing lastSeqNum %d and %s\n", p2p->cnc.lastSeqNum,
                       p2p->ack.isWaitingForAck ? "WAITING FOR ACK" : "NOT WAITING FOR ACK");
            //TODO Why did we do this? Extract senders side and save
            //     maybe if want correct a broken connections
            //p2p->cnc.otherSide = p2pHex2Int(data[EXT_IDX + 0]);
            //p2p_printf("Set p2p->cnc.otherSide = %d\n", p2p->cnc.otherSide);
        }
    }

    // ACKs can be received in any state
    if(p2p->ack.isWaitingForAck)
    {
        // p2p_printf("Checking if ACK len=%d, sizeof(p2p->ackMsg)-1=%d %s %s\r\n", len, sizeof(p2p->ackMsg)-1, data,
        //            p2p->ackMsg);
        // Check if this is an ACK
        if(INITIAL_PART <= len &&
                0 == ets_memcmp(data, p2p->ackMsg, INITIAL_PART))
        {
            p2p_printf("ACK Received so return\r\n");
            // TODO fix Adam's code as this is prob missing from his
            //Clear flag
            p2p->ack.isWaitingForAck = false;
            // Clear ack timeout variables
            syncedTimerDisarm(&p2p->tmr.TxRetry);
            // Disarm the whole transmission ack timer
            syncedTimerDisarm(&p2p->tmr.TxAllRetries);
            // Save the success function
            void (*successFn)(void*) = p2p->ack.SuccessFn;
            // Clear out ACK variables
            ets_memset(&p2p->ack, 0, sizeof(p2p->ack));

            // Call the function after receiving the ack
            if(NULL != successFn)
            {
                successFn(p2p);
            }
        }
        else
        {
            p2p_printf("ACK NOT Received so continue waiting\r\n");
        }
        // Don't process anything else when waiting or processing an ack
        return;
    }

    if(false == p2p->cnc.isConnected)
        // Handle not connected case
    {
        if(true == p2p->cnc.isConnecting)
        {
            // TODO if broadcast for the other
            // TODO FIX logic here and cases when one side already connected using p2p->otherConnectionMade
            // TODO need to check broadcast from opposite side R side can respond to want connection on L side and vice versa
            // Received another broadcast, Check if this RSSI is strong enough

            // Assemble connection broadcast we are looking for
            char conMsgSought[13];
            ets_snprintf(conMsgSought, sizeof(conMsgSought), p2pConnectionMsgFmt,
                         p2p->msgId, p2p->cnc.otherSide, p2p->ringSeq);
            if(!p2p->cnc.broadcastReceived &&
                    rssi > p2p->connectionRssi &&
                    sizeof(p2p->conMsg) - 1 == len &&
                    // ignore last 2 char which is ringSeq
                    0 == ets_memcmp(data, conMsgSought, len - 2))
            {

                // We received a broadcast, don't allow another
                p2p->cnc.broadcastReceived = true;

                p2pProcConnectionEvt(p2p, RX_BROADCAST);
                p2p_printf("Accept conMsgSought:%s, len=%d\n", conMsgSought, len);
                // Save the other's MAC
                // TODO maybe do in p2pPRocConnectionEvt?
                // TODO maybe not needed at all
                ets_memcpy(p2p->cnc.otherMac, senders_mac_addr, sizeof(p2p->cnc.otherMac));
                p2p->cnc.otherMacReceived = true;


                // TODO next lines could be used if aren't assigning otherSide during initialization
                //p2p->cnc.otherSide = data[CON_SIDE_IDX + 0] - '0'; // taking from con broadcast
                // Save the other's side
                //p2p_printf("p2p->cnc.otherSide = %d from con broadcast\n", p2p->cnc.otherSide);

                // Send a message to other to complete the connection.
                ets_snprintf(p2p->startMsg, sizeof(p2p->startMsg), p2pNoPayloadMsgFmt,
                             p2p->msgId,
                             "str",
                             0x30,
                             0x30,
                             senders_mac_addr[0],
                             senders_mac_addr[1],
                             senders_mac_addr[2],
                             senders_mac_addr[3],
                             senders_mac_addr[4],
                             senders_mac_addr[5],
                             p2p->cnc.otherSide,
                             p2p->side,
                             p2p->ringSeq
                            );

                // If it's acked, call p2pGameStartAckRecv(), if not reinit with p2pRestart()
                p2pSendMsgEx(p2p, p2p->startMsg, sizeof(p2p->startMsg) - 1, true, p2pGameStartAckRecv, p2pRestart);
                return;
            }

            if (!p2p->cnc.rxGameStartMsg &&
                    sizeof(p2p->startMsg) - 1 <= len &&
                    0 == ets_memcmp(data, p2p->startMsg, INITIAL_PART) &&
                    p2p->cnc.otherSide + '0' == data[SIDE_IDX + 1])
            {

                // Received a str message response to our broadcast
                p2p_printf("Game start message received (was acked)\n %s sizeof(p2p->startMsg)-1=%d, len=%d, data=%s, p2p->cnc.otherSide=%d\n",
                           p2p->cnc.rxGameStartMsg ? "RX GAME START" : "HAVENT GOT RX GAME START", sizeof(p2p->startMsg) - 1, len, data,
                           p2p->cnc.otherSide);

                // This is another swadge trying to start a game, which means
                // they received our p2p->conMsg. First disable our p2p->conMsg
                syncedTimerDisarm(&p2p->tmr.Connection);



                // TODO maybe do the following in p2pPRocConnectionEvt?
                // TODO record otherSide and confirm own side consistant with message

                // record ringSeq
                p2p->ringSeq = 16 * p2pHex2Int(data[RING_SEQ_IDX]) + p2pHex2Int(data[RING_SEQ_IDX + 1]);
                if (p2p->side == LEFT)
                {
                    p2p->ringSeq++;
                }
                else
                {
                    p2p->ringSeq--;
                }

                // Save the other's MAC
                ets_memcpy(p2p->cnc.otherMac, senders_mac_addr, sizeof(p2p->cnc.otherMac));
                p2p->cnc.otherMacReceived = true;

                // And process this connection event
                p2pProcConnectionEvt(p2p, RX_GAME_START_MSG);

                return;
            }
        }
        p2p_printf("Not CONNECTED and NOT CONNECTING\r\n");
        return;
    }
    else // (thinks) it's connected
    {

#ifdef HANDLE_CON_FROM_RESTARTED_SWADGE
        // Look for con broadcast coming from the otherMac on otherSide
        if(0 == ets_memcmp(data, p2p->conMsg, CON_SIDE_IDX)
                && 0 == ets_memcmp(senders_mac_addr, p2p->cnc.otherMac, sizeof(p2p->cnc.otherMac))
                && p2p->cnc.otherSide == p2pHex2Int(data[CON_SIDE_IDX + 0]))
        {
            p2p_printf("Re connect from con broadcast %s from otherMac on side %d %d\r\n", p2p->conMsg, p2p->cnc.otherSide,
                       p2pHex2Int(data[EXT_IDX + 0]));
            // This is a con broadcast from p2p->cnc.otherMac (maybe because it was turned off then on and button pushed)
            // reponds automatically to it to reestablish connection
            // Light led with random hue on correct side
            uint8_t randomHue = os_random();
            // Send a repair message with hue
            char testMsg[256] = {0};
            ets_sprintf(testMsg, "%02X REPAIR hue", randomHue);

            // Send a message to other to complete the connection.
            ets_snprintf(p2p->startMsg, sizeof(p2p->startMsg), p2pNoPayloadMsgFmt,
                         p2p->msgId,
                         "rst",
                         0x30,
                         0x30,
                         senders_mac_addr[0],
                         senders_mac_addr[1],
                         senders_mac_addr[2],
                         senders_mac_addr[3],
                         senders_mac_addr[4],
                         senders_mac_addr[5],
                         //TODO check no reverse here ok?
                         //p2p->side,
                         p2p->cnc.otherSide,
                         p2p->side,
                         p2p->ringSeq
                        );
            // TODO gave no callbacks, can get by with no ack request?
            p2pSendMsgEx(p2p, p2p->startMsg, sizeof(p2p->startMsg) - 1, true, NULL, NULL);
            // TODO  check why return can be left out and still ok
            return;
        }
#endif

        p2p_printf("cnc.isconnected is true\r\n");
        // Let the mode handle it
        if(NULL != p2p->msgRxCbFn)
        {
            p2p_printf("letting mode handle message\r\n");
            char msgType[4] = {0};
            memcpy(msgType, &data[CMD_IDX], 3 * sizeof(char));
            if (len > EXT_IDX)
            {
                p2p->msgRxCbFn(p2p, msgType, &data[EXT_IDX], len - EXT_IDX);
            }
            else
            {
                //TODO should this be NULL, for mode_ring param ignored, but for other applications might use
                p2p->msgRxCbFn(p2p, msgType, NULL, 0);
            }
        }
    }
}

/**
 * Helper function to send an ACK message to the given MAC
 *
 * @param p2p      The p2pInfo struct with all the state information
 * @param mac_addr The MAC to address this ACK to
 */
void ICACHE_FLASH_ATTR p2pSendAckToMac(p2pInfo* p2p, uint8_t* mac_addr, uint8_t* data)
{
    p2p_printf("%s %s %s\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT", data);
    ets_snprintf(p2p->ackMsg, sizeof(p2p->ackMsg), p2pPayloadMsgFmt,
                 p2p->msgId,
                 "ack",
                 0x30,
                 0x30,
                 mac_addr[0],
                 mac_addr[1],
                 mac_addr[2],
                 mac_addr[3],
                 mac_addr[4],
                 mac_addr[5],
                 p2p->cnc.otherSide,
                 p2p->side,
                 p2p->ringSeq,
                 data
                );
    //TODO check
    p2p_printf("p2p->ackMsg %s len=%d\n", p2p->ackMsg, 35 + ets_strlen((const char*)data));
    p2pSendMsgEx(p2p, p2p->ackMsg, 35 + ets_strlen((const char*)data), false, NULL, NULL);
}

/**
 * This is called when p2p->startMsg is acked and processes the connection event
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pGameStartAckRecv(void* arg)
{
    p2pInfo* p2p = (p2pInfo*)arg;
    p2p_printf("%s %s\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT");
    //TODO could this routine be called more than once, if so may wish to do this only once
    if (p2p->otherConnectionMade && !p2p->longPushButton)
    {
        // if (p2p->subsequentStartListenFirst)
        // {
        p2pStartConnectionBroadcast(p2p);
        //}
    }
    p2pProcConnectionEvt(p2p, RX_GAME_START_ACK);
}

/**
 * Two steps are necessary to establish a connection in no particular order.
 * 1. This swadge has to receive a start message from another swadge
 * 2. This swadge has to receive an ack to a start message sent to another swadge
 * The order of events determines who is the 'client' and who is the 'server'
 *
 * @param p2p   The p2pInfo struct with all the state information
 * @param event The event that occurred
 */
void ICACHE_FLASH_ATTR p2pProcConnectionEvt(p2pInfo* p2p, connectionEvt_t event)
{
    p2p_printf("%s %s %s evt: %d, p2p->cnc.rxGameStartMsg %d, p2p->cnc.rxGameStartAck %d\r\n", __func__, p2p->msgId,
               p2p->side == LEFT ? "LEFT" : "RIGHT", event,
               p2p->cnc.rxGameStartMsg, p2p->cnc.rxGameStartAck);

    switch(event)
    {
        case RX_GAME_START_MSG:
        {
            // Already received the ack, become the client
            if(!p2p->cnc.rxGameStartMsg && p2p->cnc.rxGameStartAck)
            {
                p2p->cnc.playOrder = GOING_SECOND;
            }
            // Mark this event
            p2p->cnc.rxGameStartMsg = true;
            // if (p2p->otherConnectionMade)
            // {
            //     if (!p2p->subsequentStartListenFirst)
            //     {
            //         p2pStartConnectionListening(p2p);
            //     }
            // }
            break;
        }
        case RX_GAME_START_ACK:
        {
            // Already received the str msg, become the server
            if(!p2p->cnc.rxGameStartAck && p2p->cnc.rxGameStartMsg)
            {
                p2p->cnc.playOrder = GOING_FIRST;
                //p2p->ringSeq++;
            }
            // Mark this event
            p2p->cnc.rxGameStartAck = true;
            break;
        }

        case CON_LOST:
        case CON_STOPPED:
        {
            p2p->cnc.otherMacReceived = false;
            break;
        }
        case CON_BROADCAST_STARTED:
        case CON_LISTENING_STARTED:
        case RX_BROADCAST:
        case CON_ESTABLISHED:
        default:
        {
            break;
        }
    }

    if(NULL != p2p->conCbFn)
    {
        p2p->conCbFn(p2p, event);
    }

    // If both the game start messages are good, start the game
    if(p2p->cnc.rxGameStartMsg && p2p->cnc.rxGameStartAck)
    {
        // Connection was successful, so disarm the failure timer
        syncedTimerDisarm(&p2p->tmr.Reinit);

        p2p->cnc.isConnecting = false;
        p2p->cnc.isConnected = true;

        // Set up ringSeq for swadge
        // something like p2pHex2Int(payload[3]) * 16 + p2pHex2Int(payload[4]);

        // tell the mode it's connected
        if(NULL != p2p->conCbFn)
        {
            p2p->conCbFn(p2p, CON_ESTABLISHED);
        }
    }
    else
    {
        // Start a timer to reinit if we never finish connection
        p2pStartRestartTimer(p2p);
    }
}

/**
 * This starts a timer to call p2pRestart(), used in case of a failure
 * The timer is set when one half of the necessary connection messages is received
 * The timer is disarmed when the connection is established
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pStartRestartTimer(void* arg)
{
    p2pInfo* p2p = (p2pInfo*)arg;
    p2p_printf("%s\r\n", p2p->msgId);
    // If the connection isn't established in FAILURE_RESTART_MS, restart
    syncedTimerArm(&p2p->tmr.Reinit, FAILURE_RESTART_MS, false);
}

/**
 * Restart by deiniting then initing. Persist the msgId and p2p->conCbFn
 * fields
 *
 * @param arg The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pRestart(void* arg)
{
    p2pInfo* p2p = (p2pInfo*)arg;
    p2p_printf("%s %s\r\n", p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT");
    if(NULL != p2p->conCbFn)
    {
        p2p->conCbFn(p2p, CON_LOST);
    }

    // Save what's necessary for init
    char msgId[4] = {0};
    ets_strncpy(msgId, p2p->msgId, sizeof(msgId));
    p2pConCbFn conCbFn = p2p->conCbFn;
    p2pMsgRxCbFn msgRxCbFn = p2p->msgRxCbFn;
    uint8_t connectionRssi = p2p->connectionRssi;
    button_mask sideSave = p2p->side;
    button_mask otherSideSave = p2p->cnc.otherSide;
    // Stop and clear everything
    p2pDeinit(p2p);
    // Start it up again
    p2pInitialize(p2p, msgId, conCbFn, msgRxCbFn, connectionRssi);
    p2p->side = sideSave;
    p2p->cnc.otherSide = otherSideSave;
}

/**
 * This must be called by whatever function is registered to the Swadge mode's
 * fnEspNowSendCb
 *
 * This is called after an attempted transmission. If it was successful, and the
 * message should be acked, start a retry timer. If it wasn't successful, just
 * try again
 *
 * @param p2p      The p2pInfo struct with all the state information
 * @param recipient_mac_addr that transmission was sent to
 * @param status   Whether the transmission succeeded or failed
 */
void ICACHE_FLASH_ATTR p2pSendCb(p2pInfo* p2p, uint8_t* recipient_mac_addr, mt_tx_status status)
{
#ifdef TEST_SEND_ID
    p2p_printf("p2pEspSendId=%d, p2p->sendId = %d\r\n", p2pEspSendId, p2p->sendId);
    if (p2p->sendId != p2pEspSendId)
    {
        p2p_printf("   not from send by %s %s, do nothing\r\n", p2p->msgId,
                   p2p->side == LEFT ? "LEFT" : "RIGHT");
        return;
    }
#else
    bool broadcast = (recipient_mac_addr[0] == 0xFF) && (recipient_mac_addr[1] == 0xFF) && (recipient_mac_addr[2] == 0xFF)
                     && (recipient_mac_addr[3] == 0xFF) && (recipient_mac_addr[4] == 0xFF) && (recipient_mac_addr[5] == 0xFF);
    if (broadcast)
    {
        p2p_printf("p2pEspSendId=%d, p2p->sendId = %d, to %s %s from a broadcast - do nothing\r\n", p2pEspSendId, p2p->sendId,
                   p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT");
        return;
    }
    p2p_printf("p2pEspSendId=%d, p2p->sendId = %d, to %s %s from  %02X:%02X:%02X:%02X:%02X:%02X \r\n", p2pEspSendId,
               p2p->sendId, p2p->msgId,
               p2p->side == LEFT ? "LEFT" : "RIGHT",
               recipient_mac_addr[0], recipient_mac_addr[1], recipient_mac_addr[2], recipient_mac_addr[3], recipient_mac_addr[4],
               recipient_mac_addr[5]);

    // A non-broadcast was sent
    if (p2p->cnc.otherMacReceived)
    {
        for (uint8_t i = 0; i < 6; i++)
        {
            if (p2p->cnc.otherMac[i] != recipient_mac_addr[i])
            {
                p2p_printf("  DISCARD as not from otherMac\r\n");
                return;
            }
        }
    }
    else
    {
        p2p_printf("  DISCARD as no otherMac specified\r\n");
        return;
    }
#endif

    p2p_printf("   from send by %s %s, status MT_TX_STATUS_%s\r\n",
               p2p->msgId, p2p->side == LEFT ? "LEFT" : "RIGHT",
               status == MT_TX_STATUS_OK ? "OK" : (status == MT_TX_STATUS_FAILED ? "FAILED" : "UNKNOWN"));
    p2pDumpInfo(p2p);
    p2p_printf("   (p2p->ack.timeSentUs = %u us)\r\n", p2p->ack.timeSentUs);
    switch(status)
    {
        case MT_TX_STATUS_OK:
        {
            if(0 != p2p->ack.timeSentUs) // from sending a messages that needed an ack
            {
                //handle need to retry if ack not received
                uint32_t transmissionTimeUs = system_get_time() - p2p->ack.timeSentUs;
                // The timers are all millisecond, so make sure that
                // transmissionTimeUs is at least 1ms
                if(transmissionTimeUs < 1000)
                {
                    transmissionTimeUs = 1000;
                }
                // Round it to the nearest ms, add 69ms (the measured worst case)
                // then add some randomness [0ms to 15ms random]
                uint32_t waitTimeMs = ((transmissionTimeUs + 500) / 1000) + 69 + (os_random() & 0b1111);

                // Start the timer
                p2p_printf("   ack timer set for %d ms (transmission time was %d us)\r\n", waitTimeMs, transmissionTimeUs);
                syncedTimerArm(&p2p->tmr.TxRetry, waitTimeMs, false);
            }
            break;
        }
        case MT_TX_STATUS_FAILED:
        {
            // If a message is stored
            if(p2p->ack.msgToAckLen > 0)
            {
                // try again in 1ms
                syncedTimerArm(&p2p->tmr.TxRetry, 1, false);
            }
            break;
        }
        default:
        {
            break;
        }
    }
}

/**
 * Prints to serial info about p2p
 * @param p2p The p2pInfo struct with all the state information
 * @return
 */
#ifdef SHOW_DUMP
void ICACHE_FLASH_ATTR p2pDumpInfo(p2pInfo* p2p)
{

    os_printf("*DUMP*: %s %d%d, %s %s\n", p2p->macStr, p2p->side, p2p->cnc.otherSide,
              p2p->otherConnectionMade ? " has other connection" : "no other connection",
              p2p-> longPushButton ? "long push" : "short push");
    os_printf("  %s %s\n", p2p->ack.isWaitingForAck ? "Waiting for ack to " : "Not waiting for ack",
              p2p->ack.isWaitingForAck ? p2p->ack.msgToAck : "" );
    os_printf("  cnc flags:%1d%1d%1d%1d%1d%1d otherMac [%02X:%02X]\n", p2p->cnc.isConnected, p2p->cnc.isConnecting,
              p2p->cnc.broadcastReceived,
              p2p->cnc.rxGameStartAck, p2p->cnc.rxGameStartMsg, p2p->cnc.otherMacReceived, p2p->cnc.otherMac[4],
              p2p->cnc.otherMac[5] );
}
#else
void ICACHE_FLASH_ATTR p2pDumpInfo(p2pInfo* p2p __attribute__((unused)))
{
}
#endif

/**
 * After the swadge is connected to another, return whether this Swadge is
 * player 1 or player 2. This can be used to determine client/server roles
 *
 * @param p2p The p2pInfo struct with all the state information
 * @return    GOING_SECOND, GOING_FIRST, or NOT_SET
 */
playOrder_t ICACHE_FLASH_ATTR p2pGetPlayOrder(p2pInfo* p2p)
{
    if(p2p->cnc.isConnected)
    {
        return p2p->cnc.playOrder;
    }
    else
    {
        return NOT_SET;
    }
}

/**
 * Override whether the Swadge is player 1 or player 2. You probably shouldn't
 * do this, but you might want to for single player modes
 *
 * @param p2p The p2pInfo struct with all the state information
 */
void ICACHE_FLASH_ATTR p2pSetPlayOrder(p2pInfo* p2p, playOrder_t order)
{
    p2p->cnc.playOrder = order;
}