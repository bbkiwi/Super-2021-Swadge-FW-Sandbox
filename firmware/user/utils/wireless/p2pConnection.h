#ifndef _P2P_CONNECTION_H_
#define _P2P_CONNECTION_H_

#include <osapi.h>
#include "user_main.h"
#include "buttons.h"

typedef enum
{
    NOT_SET,
    GOING_FIRST,
    GOING_SECOND
} playOrder_t;

typedef enum
{
    CON_BROADCAST_STARTED,
    CON_LISTENING_STARTED,
    RX_BROADCAST,
    RX_GAME_START_ACK,
    RX_GAME_START_MSG,
    CON_ESTABLISHED,
    CON_LOST,
    CON_STOPPED
} connectionEvt_t;

typedef enum
{
    MSG_ACKED,
    MSG_FAILED
} messageStatus_t;

typedef struct _p2pInfo p2pInfo;

typedef void (*p2pConCbFn)(p2pInfo* p2p, connectionEvt_t);
typedef void (*p2pMsgRxCbFn)(p2pInfo* p2p, char* msg, uint8_t* payload, uint8_t len);
typedef void (*p2pMsgTxCbFn)(p2pInfo* p2p, messageStatus_t status);

// Variables to track acking messages
typedef struct _p2pInfo
{
    // Messages that every mode uses
    // size needs to be 1 more than longest string (to hold NULL terminator)
    char msgId[4];
    char conMsg[13];
    char ackMsg[90]; //TODO changed
    char startMsg[35];

    // identifiers
    button_mask side;
    char macStr[18];
    uint8_t ringSeq;
    int16_t sendCnt;

    bool otherConnectionMade; // ok for 2 but if have more possible?
    //bool subsequentStartBroadcastFirst;
    //bool subsequentStartListenFirst;
    bool longPushButton;

    // Callback function pointers
    p2pConCbFn conCbFn;
    p2pMsgRxCbFn msgRxCbFn;
    p2pMsgTxCbFn msgTxCbFn;

    uint8_t connectionRssi;

    // Variables used for acking and retrying messages
    struct
    {
        bool isWaitingForAck;
        char msgToAck[64];
        uint16_t msgToAckLen;
        uint32_t timeSentUs;
        void (*SuccessFn)(void*);
        void (*FailureFn)(void*);
    } ack;

    // Connection state variables
    struct
    {
        bool isConnected;
        bool isConnecting;
        bool broadcastReceived;
        bool rxGameStartMsg;
        bool rxGameStartAck;
        bool otherMacReceived;
        playOrder_t playOrder;
        uint8_t otherRingSeq;
        uint8_t otherMac[6];
        button_mask otherSide;
        uint8_t otherRssi; // rssi when it connected
        uint8_t msgRepeatCnt;
        uint8_t mySeqNum;
        uint8_t lastSeqNum;
    } cnc;

    // The timers used for connection and acking
    struct
    {
        syncedTimer_t TxRetry;
        syncedTimer_t TxAllRetries;
        syncedTimer_t Connection;
        syncedTimer_t Reinit;
    } tmr;
} p2pInfo;

void ICACHE_FLASH_ATTR p2pInitialize(p2pInfo* p2p, char* msgId,
                                     p2pConCbFn conCbFn,
                                     p2pMsgRxCbFn msgRxCbFn, uint8_t connectionRssi);
void ICACHE_FLASH_ATTR p2pDeinit(p2pInfo* p2p);
void ICACHE_FLASH_ATTR p2pRestart(void* arg);
void ICACHE_FLASH_ATTR p2pDumpInfo(p2pInfo* p2p);

void ICACHE_FLASH_ATTR p2pStartConnection(p2pInfo* p2p);
void ICACHE_FLASH_ATTR p2pStartConnectionBroadcast(p2pInfo* p2p);
void ICACHE_FLASH_ATTR p2pStartConnectionListening(p2pInfo* p2p);
void ICACHE_FLASH_ATTR p2pStopConnection(p2pInfo* p2p);

void ICACHE_FLASH_ATTR p2pSendMsg(p2pInfo* p2p, char* msg, char* payload, uint16_t len, p2pMsgTxCbFn msgTxCbFn);
void ICACHE_FLASH_ATTR p2pSendCb(p2pInfo* p2p, uint8_t* recipient_mac_addr, mt_tx_status status, uint16_t sendCbCnt);
void ICACHE_FLASH_ATTR p2pRecvCb(p2pInfo* p2p, uint8_t* senders_mac_addr, uint8_t* data, uint8_t len, uint8_t rssi,
                                 uint16_t recvCbCnt);

playOrder_t ICACHE_FLASH_ATTR p2pGetPlayOrder(p2pInfo* p2p);
void ICACHE_FLASH_ATTR p2pSetPlayOrder(p2pInfo* p2p, playOrder_t order);
button_mask ICACHE_FLASH_ATTR p2pHex2Int(uint8_t in);

#endif
