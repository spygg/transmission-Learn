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

/***********************************************************************
 * Local prototypes
 **********************************************************************/
static int checkPeer       ( tr_torrent_t *, int );
static int parseMessage    ( tr_torrent_t *, tr_peer_t *, int );
static int isInteresting   ( tr_torrent_t *, tr_peer_t * );
static int chooseBlock     ( tr_torrent_t *, tr_peer_t * );

/***********************************************************************
 * tr_peerAddOld
 ***********************************************************************
 * Tries to add a peer given its IP and port (received from a tracker
 * which doesn't support the "compact" extension).
 **********************************************************************/
void tr_peerAddOld( tr_torrent_t * tor, char * ip, int port )
{
    struct in_addr addr;

    if( tr_netResolve( ip, &addr ) )
    {
        return;
    }

    tr_peerAddWithAddr( tor, addr, htons( port ) );
}

/***********************************************************************
 * tr_peerAddCompact
 ***********************************************************************
 * Tries to add a peer. If 's' is a negative value, will use 'addr' and
 * 'port' to connect to the peer. Otherwise, use the already connected
 * socket 's'.
 **********************************************************************/
void tr_peerAddCompact( tr_torrent_t * tor, struct in_addr addr,
                        in_port_t port, int s )
{
    tr_peer_t * peer;

    if( s < 0 )
    {
        tr_peerAddWithAddr( tor, addr, port );
        return;
    }

    if( !( peer = tr_peerInit( tor ) ) )
    {
        tr_netClose( s );
        return;
    }

    peer->socket = s;
    peer->addr   = addr;
    peer->port   = port;
    peer->status = PEER_STATUS_CONNECTING;
}

/***********************************************************************
 * tr_peerRem
 ***********************************************************************
 * Frees and closes everything related to the peer at index 'i', and
 * removes it from the peers list.
 **********************************************************************/
void tr_peerRem( tr_torrent_t * tor, int i )
{
    tr_peer_t * peer = tor->peers[i];
    int j;

    for( j = 0; j < peer->inRequestCount; j++ )
    {
        tr_request_t * r;
        r = &peer->inRequests[j];
        tor->blockHave[tr_block(r->index,r->begin)]--;
    }
    if( !peer->amChoking )
    {
        tr_uploadChoked( tor->upload );
    }
    if( peer->bitfield )
    {
        free( peer->bitfield );
    }
    if( peer->buf )
    {
        free( peer->buf );
    }
    if( peer->outBuf )
    {
        free( peer->outBuf );
    }
    if( peer->status > PEER_STATUS_IDLE )
    {
        tr_netClose( peer->socket );
    }
    free( peer );
    tor->peerCount--;
    memmove( &tor->peers[i], &tor->peers[i+1],
             ( tor->peerCount - i ) * sizeof( tr_peer_t * ) );
}

/***********************************************************************
 * tr_peerPulse
 ***********************************************************************
 *
 **********************************************************************/
void tr_peerPulse( tr_torrent_t * tor )
{
    int i, ret;
    tr_peer_t * peer;

    tor->dates[9] = tr_date();
    if( tor->dates[9] > tor->dates[8] + 1000 )
    {
        memmove( &tor->downloaded[0], &tor->downloaded[1],
                 9 * sizeof( uint64_t ) );
        memmove( &tor->uploaded[0], &tor->uploaded[1],
                 9 * sizeof( uint64_t ) );
        memmove( &tor->dates[0], &tor->dates[1],
                 9 * sizeof( uint64_t ) );

        for( i = 0; i < tor->peerCount; )
        {
            if( checkPeer( tor, i ) )
            {
                tr_peerRem( tor, i );
                continue;
            }
            i++;
        }
    }

    /* Check for incoming connections */
    if( tor->bindSocket > -1 &&
        tor->peerCount < TR_MAX_PEER_COUNT )
    {
        int            s;
        struct in_addr addr;
        in_port_t      port;
        s = tr_netAccept( tor->bindSocket, &addr, &port );
        if( s > -1 )
        {
            tr_peerAddCompact( tor, addr, port, s );
        }
    }
    
    /* Shuffle peers */
    if( tor->peerCount > 1 )
    {
        peer = tor->peers[0];
        memmove( &tor->peers[0], &tor->peers[1],
                 ( tor->peerCount - 1 ) * sizeof( void * ) );
        tor->peers[tor->peerCount - 1] = peer;
    }

    /* Handle peers */
    for( i = 0; i < tor->peerCount; )
    {
        peer = tor->peers[i];

        /* Connect */
        if( peer->status & PEER_STATUS_IDLE )
        {
            peer->socket = tr_netOpen( peer->addr, peer->port );
            if( peer->socket < 0 )
            {
                goto dropPeer;
            }
            peer->status = PEER_STATUS_CONNECTING;
        }

        /* Try to send handshake */
        if( peer->status & PEER_STATUS_CONNECTING )
        {
            char buf[68];
            tr_info_t * inf = &tor->info;

            sprintf( buf, "%cBitTorrent protocol", 19 );
            memset( &buf[20], 0, 8 );
            memcpy( &buf[28], inf->hash, 20 );
            memcpy( &buf[48], tor->id, 20 );

            ret = tr_netSend( peer->socket, buf, 68 );
            if( ret & TR_NET_CLOSE )
            {
                goto dropPeer;
            }
            else if( !( ret & TR_NET_BLOCK ) )
            {
                tr_dbg( "%08x:%04x SEND handshake",
                        peer->addr.s_addr, peer->port );
                peer->status = PEER_STATUS_HANDSHAKE;
            }
        }

        /* Try to read */
        if( peer->status >= PEER_STATUS_HANDSHAKE )
        {
            if( peer->size < 1 )
            {
                peer->size = 1024;
                peer->buf  = malloc( peer->size );
            }
            else if( peer->pos >= peer->size )
            {
                peer->size *= 2;
                peer->buf   = realloc( peer->buf, peer->size );
            }
            ret = tr_netRecv( peer->socket, &peer->buf[peer->pos],
                              peer->size - peer->pos );
            if( ret & TR_NET_CLOSE )
            {
                goto dropPeer;
            }
            else if( !( ret & TR_NET_BLOCK ) )
            {
                peer->date  = tr_date();
                peer->pos  += ret;

                if( parseMessage( tor, peer, ret ) )
                {
                    goto dropPeer;
                }
            }
        }

        /* If we are uploading to this peer, make sure we have something
           ready to be sent */
        if( peer->outPos < tor->blockSize / 2 &&
            peer->outRequestCount > 0 )
        {
            tr_peerSendPiece( tor, peer );
        }

        /* Try to write */
        while( peer->outPos > 0 )
        {
            int willSend;
            
            if( peer->outPos > 100 &&
                !tr_uploadCanUpload( tor->upload ) )
            {
                break;
            }

            willSend = MIN( peer->outPos, 1024 );

            ret = tr_netSend( peer->socket, peer->outBuf, willSend );
            if( ret & TR_NET_CLOSE )
            {
                goto dropPeer;
            }
            else if( ret & TR_NET_BLOCK )
            {
                break;
            }
            tr_uploadUploaded( tor->upload, willSend );

            peer->outPos -= willSend;
            memmove( &peer->outBuf[0], &peer->outBuf[willSend],
                     peer->outPos );

            tor->uploaded[9] += willSend;
            peer->outTotal   += willSend;
            peer->outDate     = tr_date();
        }

        /* Connected peers: update interest if required and ask for
           a block whenever possible */
        if( peer->status & PEER_STATUS_CONNECTED )
        {
            int interested = isInteresting( tor, peer );

            if( !interested && tor->peerCount > TR_MAX_PEER_COUNT - 5 )
            {
                /* This peer is no use to us, and it seems there are
                   others */
                tr_peerRem( tor, i );
                continue;
            }
            
            if( interested && !peer->amInterested )
            {
                tr_peerSendInterest( peer, 1 );
            }
            if( !interested && peer->amInterested )
            {
                tr_peerSendInterest( peer, 0 );
            }
            
            if( peer->amInterested && !peer->peerChoking )
            {
                while( peer->inRequestCount < MAX_REQUEST_COUNT / 2 )
                {
                    tr_peerSendRequest( tor, peer,
                                        chooseBlock( tor, peer ) );
                }
            }
        }
        
        i++;
        continue;

dropPeer:
        tr_peerRem( tor, i );
    }
}

/***********************************************************************
 * tr_peerIsConnected
 ***********************************************************************
 *
 **********************************************************************/
int tr_peerIsConnected( tr_peer_t * peer )
{
    return peer->status & PEER_STATUS_CONNECTED;
}

/***********************************************************************
 * tr_peerIsUploading
 ***********************************************************************
 *
 **********************************************************************/
int tr_peerIsUploading( tr_peer_t * peer )
{
    return peer->amInterested && !peer->peerChoking && peer->inTotal;
}

/***********************************************************************
 * tr_peerIsDownloading
 ***********************************************************************
 *
 **********************************************************************/
int tr_peerIsDownloading( tr_peer_t * peer )
{
    return peer->peerInterested && !peer->amChoking && peer->outTotal;
}

/***********************************************************************
 * tr_peerBitfield
 ***********************************************************************
 *
 **********************************************************************/
uint8_t * tr_peerBitfield( tr_peer_t * peer )
{
    return peer->bitfield;
}

/***********************************************************************
 * Following functions are local
 **********************************************************************/

static int checkPeer( tr_torrent_t * tor, int i )
{
    tr_peer_t * peer = tor->peers[i];

    if( ( peer->status & PEER_STATUS_HANDSHAKE ) &&
        tr_date() > peer->date + 8000 )
    {
        /* If it has been too long, don't wait for the socket
           to timeout - forget about it now */
        return 1;
    }

    /* Drop peers who haven't even sent a keep-alive within the
       last 3 minutes */
    if( tr_date() > peer->date + 180000 )
    {
        return 1;
    }

    /* Drop peers which are supposed to upload but actually
       haven't sent anything within the last minute */
    if( peer->inRequestCount && tr_date() > peer->date + 60000 )
    {
        return 1;
    }

#if 0
    /* Choke unchoked peers we are not sending anything to */
    if( !peer->amChoking && tr_date() > peer->outDate + 10000 )
    {
        tr_dbg( "%08x:%04x not worth the unchoke",
                peer->addr.s_addr, peer->port );
        if( tr_peerSendChoke( peer, 1 ) )
        {
            goto dropPeer;
        }
        peer->outSlow = 1;
        tr_uploadChoked( tor->upload );
    }
#endif

    /* New peers: try to establish a connection */
    /* Choke or unchoke some people */
    /* TODO: prefer people who upload to us */

    if( peer->status & PEER_STATUS_CONNECTED )
    {
        /* Send keep-alive every 2 minutes */
        if( tr_date() > peer->keepAlive + 120000 )
        {
            tr_peerSendKeepAlive( peer );
            peer->keepAlive = tr_date();
        }

        if( !peer->amChoking && !peer->peerInterested )
        {
            /* He doesn't need us */
            tr_peerSendChoke( peer, 1 );
            tr_uploadChoked( tor->upload );
        }
        if( peer->amChoking && peer->peerInterested &&
            !peer->outSlow && tr_uploadCanUnchoke( tor->upload ) )
        {
            tr_peerSendChoke( peer, 0 );
            tr_uploadUnchoked( tor->upload );
        }
    }

    return 0;
}

static int parseMessage( tr_torrent_t * tor, tr_peer_t * peer,
                         int newBytes )
{
    tr_info_t * inf = &tor->info;

    int    i;
    int    len;
    char   id;
    char * p   = peer->buf;
    char * end = &p[peer->pos];
    
    for( ;; )
    {
        if( peer->pos < 4 )
        {
            break;
        }

        if( peer->status & PEER_STATUS_HANDSHAKE )
        {
            if( p[0] != 19 || memcmp( &p[1], "Bit", 3 ) )
            {
                /* Don't wait until we get 68 bytes, this is wrong
                   already */
                tr_dbg( "%08x:%04x GET  handshake, invalid",
                        peer->addr.s_addr, peer->port );
                tr_netSend( peer->socket, "Nice try...\r\n", 13 );
                return 1;
            }

            if( peer->pos < 68 )
            {
                break;
            }

            if( memcmp( &p[4], "Torrent protocol", 16 ) ||
                memcmp( &p[28], inf->hash, 20 ) )
            {
                tr_dbg( "%08x:%04x GET  handshake, invalid",
                        peer->addr.s_addr, peer->port );
                return 1;
            }

            if( !memcmp( &p[48], tor->id, 20 ) )
            {
                /* We are connected to ourselves... */
                tr_dbg( "%08x:%04x GET  handshake, that is us",
                        peer->addr.s_addr, peer->port );
                return 1;
            }

            peer->status  = PEER_STATUS_CONNECTED;
            memcpy( peer->id, &p[48], 20 );
            p            += 68;
            peer->pos    -= 68;

            for( i = 0; i < tor->peerCount; i++ )
            {
                if( tor->peers[i] == peer )
                {
                    continue;
                }
                if( !tr_peerCmp( peer, tor->peers[i] ) )
                {
                    tr_dbg( "%08x:%04x GET  handshake, duplicate",
                            peer->addr.s_addr, peer->port );
                    return 1;
                }
            }

            tr_dbg( "%08x:%04x GET  handshake, ok",
                    peer->addr.s_addr, peer->port );

            tr_peerSendBitfield( tor, peer );
            continue;
        }
        
        /* Get payload size */
        TR_NTOHL( p, len );
        p += 4;

        if( len > 9 + tor->blockSize )
        {
            /* This shouldn't happen. Forget about that peer */
            tr_dbg( "%08x:%04x message too large",
                    peer->addr.s_addr, peer->port );
            return 1;
        }

        if( !len )
        {
            /* keep-alive */
            tr_dbg( "%08x:%04x GET  keep-alive",
                     peer->addr.s_addr, peer->port );
            peer->pos -= 4;
            continue;
        }

        /* That's a piece coming */
        if( p < end && *p == 7 )
        {
            /* XXX */
            tor->downloaded[9] += newBytes;
            peer->inTotal      += newBytes;
            newBytes            = 0;
        }

        if( &p[len] > end )
        {
            /* We do not have the entire message */
            p -= 4;
            break;
        }

        /* Remaining data after this message */
        peer->pos -= 4 + len;

        /* Type of the message */
        id = *(p++);

        switch( id )
        {
            case 0: /* choke */
                if( len != 1 )
                {
                    return 1;
                }
                tr_dbg( "%08x:%04x GET  choke",
                        peer->addr.s_addr, peer->port );
                peer->peerChoking    = 1;
                peer->inRequestCount = 0;
                break;
            case 1: /* unchoke */
                if( len != 1 )
                {
                    return 1;
                }
                tr_dbg( "%08x:%04x GET  unchoke",
                        peer->addr.s_addr, peer->port );
                peer->peerChoking = 0;
                break;
            case 2: /* interested */
                if( len != 1 )
                {
                    return 1;
                }
                tr_dbg( "%08x:%04x GET  interested",
                        peer->addr.s_addr, peer->port );
                peer->peerInterested = 1;
                break;
            case 3: /* uninterested */
                if( len != 1 )
                {
                    return 1;
                }
                tr_dbg( "%08x:%04x GET  uninterested",
                        peer->addr.s_addr, peer->port );
                peer->peerInterested = 0;
                break;
            case 4: /* have */
            {
                uint32_t piece;
                if( len != 5 )
                {
                    return 1;
                }
                TR_NTOHL( p, piece );
                if( !peer->bitfield )
                {
                    peer->bitfield = calloc( ( inf->pieceCount + 7 ) / 8, 1 );
                }
                tr_bitfieldAdd( peer->bitfield, piece );

                tr_dbg( "%08x:%04x GET  have %d",
                        peer->addr.s_addr, peer->port, piece );
                break;
            }
            case 5: /* bitfield */
            {
                int bitfieldSize;

                bitfieldSize = ( inf->pieceCount + 7 ) / 8;
                
                if( len != 1 + bitfieldSize )
                {
                    tr_dbg( "%08x:%04x GET  bitfield, wrong size",
                            peer->addr.s_addr, peer->port );
                    return 1;
                }

                /* Make sure the spare bits are unset */
                if( ( inf->pieceCount & 0x7 ) )
                {
                    uint8_t lastByte;
                    
                    lastByte   = p[bitfieldSize-1];
                    lastByte <<= inf->pieceCount & 0x7;
                    lastByte  &= 0xFF;

                    if( lastByte )
                    {
                        tr_dbg( "%08x:%04x GET  bitfield, spare bits set",
                                peer->addr.s_addr, peer->port );
                        return 1;
                    }
                }

                if( !peer->bitfield )
                {
                    peer->bitfield = malloc( bitfieldSize );
                }
                memcpy( peer->bitfield, p, bitfieldSize );

                tr_dbg( "%08x:%04x GET  bitfield, ok",
                        peer->addr.s_addr, peer->port );
                break;
            }
            case 6: /* request */
            {
                int index, begin, length;

                if( peer->amChoking )
                {
                    /* Didn't he get it? */
                    tr_peerSendChoke( peer, 1 );
                    break;
                }
                
                TR_NTOHL( p,     index );
                TR_NTOHL( &p[4], begin );
                TR_NTOHL( &p[8], length );

                tr_dbg( "%08x:%04x GET  request %d/%d (%d bytes)",
                        peer->addr.s_addr, peer->port,
                        index, begin, length );

                /* TODO sanity checks (do we have the piece, etc) */

                if( peer->outRequestCount < MAX_REQUEST_COUNT )
                {
                    tr_request_t * r;
                    
                    r         = &peer->outRequests[peer->outRequestCount];
                    r->index  = index;
                    r->begin  = begin;
                    r->length = length;

                    (peer->outRequestCount)++;
                }
                else
                {
                    tr_err( "Arggg too many requests" );
                }
                break;
            }
            case 7: /* piece */
            {
                int index, begin;
                int block;
#if 0
                int i;
                tr_peer_t * otherPeer;
#endif
                tr_request_t * r;

                TR_NTOHL( p,     index );
                TR_NTOHL( &p[4], begin );

                tr_dbg( "%08x:%04x GET  piece %d/%d (%d bytes)",
                        peer->addr.s_addr, peer->port,
                        index, begin, len - 9 );
                
                r = &peer->inRequests[0];
                if( index != r->index || begin != r->begin )
                {
                    tr_dbg( "wrong piece (expecting %d/%d)",
                            r->index, r->begin );
                    return 1;
                }

                if( len - 9 != r->length )
                {
                    tr_dbg( "wrong size (expecting %d)", r->length );
                    return 1;
                }

                block = tr_block( r->index, r->begin );
                if( tor->blockHave[block] < 0 )
                {
                    /* We got this block already, too bad */
                    (peer->inRequestCount)--;
                    memmove( &peer->inRequests[0], &peer->inRequests[1],
                             peer->inRequestCount * sizeof( tr_request_t ) );
                    break;
                }

                tor->blockHave[block]  = -1;
                tor->blockHaveCount   +=  1;
                tr_ioWrite( tor->io, index, begin, len - 9, &p[8] );

#if 0
                for( i = 0; i < tor->peerCount; i++ )
                {
                    otherPeer = tor->peers[i];
                    if( otherPeer == peer ||
                        otherPeer->block != peer->block )
                    {
                        continue;
                    }

                    /* That other peer is getting the same block, cancel
                       the request */
                    tr_peerSendCancel( tor, otherPeer );
                }
#endif

                if( tr_bitfieldHas( tor->bitfield, index ) )
                {
                    tr_peerSendHave( tor, index );
                }

                (peer->inRequestCount)--;
                memmove( &peer->inRequests[0], &peer->inRequests[1],
                         peer->inRequestCount * sizeof( tr_request_t ) );
                break;
            }
            case 8: /* cancel */
            {
                int index, begin, length;
                int i;
                tr_request_t * r;

                TR_NTOHL( p,     index );
                TR_NTOHL( &p[4], begin );
                TR_NTOHL( &p[8], length );

                tr_dbg( "%08x:%04x GET  cancel %d/%d (%d bytes)",
                        peer->addr.s_addr, peer->port,
                        index, begin, length );

                for( i = 0; i < peer->outRequestCount; i++ )
                {
                    r = &peer->outRequests[i];
                    if( r->index == index && r->begin == begin &&
                        r->length == length )
                    {
                        (peer->outRequestCount)--;
                        memmove( &r[0], &r[1], sizeof( tr_request_t ) *
                                ( peer->outRequestCount - i ) );
                        break;
                    }
                }

                break;
            }
            default: /* Should not happen */
                break;
        }

        p += len - 1;
    }

    memmove( peer->buf, p, peer->pos );

    return 0;
}

/***********************************************************************
 * isInteresting
 ***********************************************************************
 * Returns 1 if 'peer' has at least one piece that we haven't completed,
 * or 0 otherwise.
 **********************************************************************/
static int isInteresting( tr_torrent_t * tor, tr_peer_t * peer )
{
    tr_info_t * inf = &tor->info;

    int i;
    int bitfieldSize = ( inf->pieceCount + 7 ) / 8;

    if( !peer->bitfield )
    {
        /* We don't know what this peer has */
        return 0;
    }

    for( i = 0; i < bitfieldSize; i++ )
    {
        if( ( peer->bitfield[i] & ~(tor->bitfield[i]) ) & 0xFF )
        {
            return 1;
        }
    }

    return 0;
}

/***********************************************************************
 * chooseBlock
 ***********************************************************************
 * At this point, we know the peer has at least one block we have an
 * interest in. If he has more than one, we choose which one we are
 * going to ask first.
 * Our main goal is to complete pieces, so we look the pieces which are
 * missing less blocks.
 **********************************************************************/
static int chooseBlock( tr_torrent_t * tor, tr_peer_t * peer )
{
    tr_info_t * inf = &tor->info;

    int i, j;
    int startBlock, endBlock, countBlocks;
    int missingBlocks, minMissing;
    int poolSize, * pool;
    int block, minDownloading;

    /* Choose a piece */
    pool       = malloc( inf->pieceCount * sizeof( int ) );
    poolSize   = 0;
    minMissing = tor->blockCount + 1;
    for( i = 0; i < inf->pieceCount; i++ )
    {
        if( !tr_bitfieldHas( peer->bitfield, i ) )
        {
            /* The peer doesn't have this piece */
            continue;
        }
        if( tr_bitfieldHas( tor->bitfield, i ) )
        {
            /* We already have it */
            continue;
        }

        /* Count how many blocks from this piece are missing */
        startBlock    = tr_pieceStartBlock( i );
        countBlocks   = tr_pieceCountBlocks( i );
        endBlock      = startBlock + countBlocks;
        missingBlocks = countBlocks;
        for( j = startBlock; j < endBlock; j++ )
        {
            if( tor->blockHave[j] )
            {
                missingBlocks--;
            }
        }

        if( missingBlocks < 1 )
        {
            /* We are already downloading all blocks */
            continue;
        }

        /* We are interested in this piece, remember it */
        if( missingBlocks < minMissing )
        {
            minMissing = missingBlocks;
            poolSize   = 0;
        }
        if( missingBlocks <= minMissing )
        {
            pool[poolSize++] = i;
        }
    }

    if( poolSize )
    {
        int piece;

        /* All pieces in 'pool' have 'minMissing' missing blocks. Pick
           a random one. TODO: we should choose the rarest one */
        piece = pool[ tr_rand( poolSize ) ];
        free( pool );

        /* Pick a block in this piece */
        startBlock = tr_pieceStartBlock( piece );
        endBlock   = startBlock + tr_pieceCountBlocks( piece );
        for( i = startBlock; i < endBlock; i++ )
        {
            if( !tor->blockHave[i] )
            {
                return i;
            }
        }

        tr_err( "chooseBlock: we should never get here!" );
        return 0;
    }

    free( pool );

    /* "End game" mode */
    block          = -1;
    minDownloading = TR_MAX_PEER_COUNT + 1;
    for( i = 0; i < tor->blockCount; i++ )
    {
        if( tor->blockHave[i] > 0 && tor->blockHave[i] < minDownloading )
        {
            block          = i;
            minDownloading = tor->blockHave[i];
        }
    }

    if( block < 0 )
    {
        tr_err( "chooseBlock: block=%d shoud not be possible", block );
    }
    
    return block;
}
