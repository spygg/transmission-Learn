/******************************************************************************
 * Copyright (c) 2005 Eric Petit
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#ifndef TR_PEERUTILS_H
#define TR_PEERUTILS_H 1

#include "transmission.h"

#define MAX_REQUEST_COUNT 16

typedef struct tr_request_s
{
    int index;
    int begin;
    int length;

} tr_request_t;

struct tr_peer_s
{
    struct in_addr addr;
    in_port_t      port;

#define PEER_STATUS_IDLE       1 /* Need to connect */
#define PEER_STATUS_CONNECTING 2 /* Trying to send handshake */
#define PEER_STATUS_HANDSHAKE  4 /* Waiting for peer's handshake */
#define PEER_STATUS_CONNECTED  8 /* Got peer's handshake */
    int            status;
    int            socket;
    uint64_t       date;
    uint64_t       keepAlive;

    char           amChoking;
    char           amInterested;
    char           peerChoking;
    char           peerInterested;

    uint8_t        id[20];
    uint8_t      * bitfield;

    char         * buf;
    int            size;
    int            pos;

    char         * outBuf;
    int            outSize;
    int            outPos;

    int            inRequestCount;
    tr_request_t   inRequests[MAX_REQUEST_COUNT];
    int            inIndex;
    int            inBegin;
    int            inLength;
    uint64_t       inTotal;

    int            outRequestCount;
    tr_request_t   outRequests[MAX_REQUEST_COUNT];
    uint64_t       outTotal;
    uint64_t       outDate;
    int            outSlow;
};

tr_peer_t * tr_peerInit          ( tr_torrent_t * );
int         tr_peerCmp           ( tr_peer_t *, tr_peer_t * );
void        tr_peerAddWithAddr   ( tr_torrent_t *, struct in_addr,
                                   in_port_t );
void        tr_peerSendKeepAlive ( tr_peer_t * );
void        tr_peerSendChoke     ( tr_peer_t *, int );
void        tr_peerSendInterest  ( tr_peer_t *, int );
void        tr_peerSendHave      ( tr_torrent_t *, int );
void        tr_peerSendBitfield  ( tr_torrent_t *, tr_peer_t * );
void        tr_peerSendRequest   ( tr_torrent_t *, tr_peer_t *, int );
void        tr_peerSendPiece     ( tr_torrent_t *, tr_peer_t * );
void        tr_peerSendCancel    ( tr_torrent_t *, tr_peer_t * );

#endif
