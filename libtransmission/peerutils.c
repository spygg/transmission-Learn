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

#include "peerutils.h"

static void checkOutSize( tr_peer_t *, int );

/***********************************************************************
 * tr_peerInit
 ***********************************************************************
 * Returns NULL if we reached the maximum authorized number of peers.
 * Otherwise, allocates a new tr_peer_t, add it to the peers list and
 * returns a pointer to it.
 **********************************************************************/
tr_peer_t * tr_peerInit( tr_torrent_t * tor )
{
    tr_peer_t * peer;

    if( tor->peerCount >= TR_MAX_PEER_COUNT )
    {
        return NULL;
    }

    peer              = calloc( sizeof( tr_peer_t ), 1 );
    peer->amChoking   = 1;
    peer->peerChoking = 1;
    peer->date        = tr_date();
    peer->keepAlive   = peer->date;

    tor->peers[tor->peerCount++] = peer;
    return peer;
}

int tr_peerCmp( tr_peer_t * peer1, tr_peer_t * peer2 )
{
    /* Wait until we got the peers' ids */
    if( peer1->status < PEER_STATUS_CONNECTED ||
        peer2->status < PEER_STATUS_CONNECTED )
    {
        return 1;
    }

    return memcmp( peer1->id, peer2->id, 20 );
}

/***********************************************************************
 * tr_peerAddWithAddr
 ***********************************************************************
 * Does nothing if we already have a peer matching 'addr' and 'port'.
 * Otherwise adds such a new peer.
 **********************************************************************/
void tr_peerAddWithAddr( tr_torrent_t * tor, struct in_addr addr,
                             in_port_t port )
{
    int i;
    tr_peer_t * peer;

    for( i = 0; i < tor->peerCount; i++ )
    {
        peer = tor->peers[i];
        if( peer->addr.s_addr == addr.s_addr &&
            peer->port        == port )
        {
            /* We are already connected to this peer */
            return;
        }
    }

    if( !( peer = tr_peerInit( tor ) ) )
    {
        return;
    }

    peer->addr   = addr;
    peer->port   = port;
    peer->status = PEER_STATUS_IDLE;
}

/***********************************************************************
 * tr_peerSendKeepAlive
 ***********************************************************************
 * 
 **********************************************************************/
void tr_peerSendKeepAlive( tr_peer_t * peer )
{
    char * p;

    checkOutSize( peer, 4 );
    p = &peer->outBuf[peer->outPos];

    TR_HTONL( 0, p );

    peer->outPos += 4;

    tr_dbg( "%08x:%04x SEND keep-alive",
            peer->addr.s_addr, peer->port );
}


/***********************************************************************
 * tr_peerSendChoke
 ***********************************************************************
 * 
 **********************************************************************/
void tr_peerSendChoke( tr_peer_t * peer, int yes )
{
    char * p;

    checkOutSize( peer, 5 );
    p = &peer->outBuf[peer->outPos];

    TR_HTONL( 1, p );
    p[4] = yes ? 0 : 1;

    peer->outPos += 5;

    peer->amChoking = yes;

    if( yes )
    {
        /* Drop all pending requests */
        peer->outRequestCount = 0;
    }

    tr_dbg( "%08x:%04x SEND %schoke",
            peer->addr.s_addr, peer->port, yes ? "" : "un" );
}

/***********************************************************************
 * tr_peerSendInterest
 ***********************************************************************
 * 
 **********************************************************************/
void tr_peerSendInterest( tr_peer_t * peer, int yes )
{
    char * p;

    checkOutSize( peer, 5 );
    p = &peer->outBuf[peer->outPos];
    
    TR_HTONL( 1, p );
    p[4] = yes ? 2 : 3;

    peer->outPos += 5;

    peer->amInterested = yes;

    tr_dbg( "%08x:%04x SEND %sinterested",
            peer->addr.s_addr, peer->port, yes ? "" : "un" );
}

/***********************************************************************
 * tr_peerSendHave
 ***********************************************************************
 * 
 **********************************************************************/
void tr_peerSendHave( tr_torrent_t * tor, int piece )
{
    int i;
    char * p;
    tr_peer_t * peer;

    for( i = 0; i < tor->peerCount; i++ )
    {
        peer = tor->peers[i];

        if( peer->status < PEER_STATUS_CONNECTED )
        {
            continue;
        }

        checkOutSize( peer, 9 );
        p = &peer->outBuf[peer->outPos];

        TR_HTONL( 5, &p[0] );
        p[4] = 4;
        TR_HTONL( piece, &p[5] );

        peer->outPos += 9;

        tr_dbg( "%08x:%04x SEND have %d", peer->addr.s_addr,
                peer->port, piece );
    }
}

/***********************************************************************
 * tr_peerSendBitfield
 ***********************************************************************
 * Builds a 'bitfield' message:
 *  - size = 5 + X (4 bytes)
 *  - id   = 5     (1 byte)
 *  - bitfield     (X bytes)
 **********************************************************************/
void tr_peerSendBitfield( tr_torrent_t * tor, tr_peer_t * peer )
{
    char * p;
    int    bitfieldSize = ( tor->info.pieceCount + 7 ) / 8;

    checkOutSize( peer, 5 + bitfieldSize );
    p = &peer->outBuf[peer->outPos];

    TR_HTONL( 1 + bitfieldSize, p );
    p[4] = 5;
    memcpy( &p[5], tor->bitfield, bitfieldSize );

    peer->outPos += 5 + bitfieldSize;

    tr_dbg( "%08x:%04x SEND bitfield", peer->addr.s_addr, peer->port );
}

/***********************************************************************
 * tr_peerSendRequest
 ***********************************************************************
 *
 **********************************************************************/
void tr_peerSendRequest( tr_torrent_t * tor, tr_peer_t * peer, int block )
{
    tr_info_t * inf = &tor->info;
    tr_request_t * r;
    char * p;

    /* Get the piece the block is a part of, its position in the piece
       and its size */
    r         = &peer->inRequests[peer->inRequestCount];
    r->index  = block / ( inf->pieceSize / tor->blockSize );
    r->begin  = ( block % ( inf->pieceSize / tor->blockSize ) ) *
                    tor->blockSize;
    r->length = tor->blockSize;
    if( block == tor->blockCount - 1 )
    {
        int lastSize = inf->totalSize % tor->blockSize;
        if( lastSize )
        {
            r->length = lastSize;
        }
    }
    (peer->inRequestCount)++;

    /* Build the "ask" message */
    checkOutSize( peer, 17 );
    p = &peer->outBuf[peer->outPos];

    TR_HTONL( 13, p );
    p[4] = 6;
    TR_HTONL( r->index, p + 5 );
    TR_HTONL( r->begin, p + 9 );
    TR_HTONL( r->length, p + 13 );

    peer->outPos += 17;

    /* Remember that we have one more uploader for this block */
    (tor->blockHave[block])++;

    tr_dbg( "%08x:%04x SEND request %d/%d (%d bytes)",
            peer->addr.s_addr, peer->port,
            r->index, r->begin, r->length );
}

/***********************************************************************
 * tr_peerSendPiece
 ***********************************************************************
 *
 **********************************************************************/
void tr_peerSendPiece( tr_torrent_t * tor, tr_peer_t * peer )
{
    char * p;

    tr_request_t * r = &peer->outRequests[0];

    checkOutSize( peer, 13 + r->length );
    p = &peer->outBuf[peer->outPos];

    TR_HTONL( 9 + r->length, p );
    p[4] = 7;
    TR_HTONL( r->index, p + 5 );
    TR_HTONL( r->begin, p + 9 );
    tr_ioRead( tor->io, r->index, r->begin, r->length, &p[13] );

    peer->outPos += 13 + r->length;

    tr_dbg( "%08x:%04x SEND piece %d/%d (%d bytes)",
            peer->addr.s_addr, peer->port,
            r->index, r->begin, r->length );

    (peer->outRequestCount)--;
    memmove( &peer->outRequests[0], &peer->outRequests[1],
             peer->outRequestCount * sizeof( tr_request_t ) );
}

#if 0
/***********************************************************************
 * tr_peerSendCancel
 ***********************************************************************
 *
 **********************************************************************/
int tr_peerSendCancel( tr_torrent_t * tor, tr_peer_t * peer )
{
    char buf[17];
    int  piece, posInPiece, size;

    /* Get the piece the block is a part of, its position in the piece
       and its size */
    piece      = peer->block / ( inf->pieceSize / tor->blockSize );
    posInPiece = ( peer->block % ( inf->pieceSize / tor->blockSize ) ) *
                 tor->blockSize;
    size       = tor->blockSize;
    if( peer->block == tor->blockCount - 1 )
    {
        int lastSize = inf->totalSize % tor->blockSize;
        if( lastSize )
        {
            size = lastSize;
        }
    }
    
    tr_dbg( "%08x:%04x SEND cancel %d/%d (%d bytes)",
            peer->addr.s_addr, peer->port,
            piece, posInPiece, size );

    /* Build the "cancel" message */
    TR_HTONL( 13, buf );
    buf[4] = 8;
    TR_HTONL( piece, buf + 5 );
    TR_HTONL( posInPiece, buf + 9 );
    TR_HTONL( size, buf + 13 );

    (tor->blockHave[peer->block])--;
    peer->block = -1;

    /* Try to send it to the peer */
    if( tr_netSend( peer->socket, buf, 17 ) != 17 )
    {
        return 1;
    }
   
    return 0;
}
#endif

static void checkOutSize( tr_peer_t * peer, int size )
{
    if( peer->outSize < 1 )
    {
        peer->outSize = size;
        peer->outBuf  = calloc( peer->outSize, 1 );
    }
    if( peer->outPos + size > peer->outSize )
    {
        peer->outSize = peer->outPos + size;
        peer->outBuf  = realloc( peer->outBuf, peer->outSize );
    }
}
