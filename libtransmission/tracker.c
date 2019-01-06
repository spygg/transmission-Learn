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

#include "transmission.h"

struct tr_tracker_s
{
    tr_torrent_t * tor;

    char       * id;

    char         started;
    char         completed;
    char         stopped;

    uint64_t     date;

#define TC_STATUS_IDLE    1
#define TC_STATUS_CONNECT 2
#define TC_STATUS_RECV    4
    char         status;
    int          socket;
    char       * buf;
    int          size;
    int          pos;
};

static void sendQuery  ( tr_tracker_t * tc );
static void recvAnswer ( tr_tracker_t * tc );

tr_tracker_t * tr_trackerInit( tr_handle_t * h, tr_torrent_t * tor )
{
    tr_tracker_t * tc;

    tc           = calloc( sizeof( tr_tracker_t ), 1 );
    tc->tor      = tor;
    tc->id       = h->id;
    tc->started  = 1;
    tc->status   = TC_STATUS_IDLE;
    tc->size     = 1024;
    tc->buf      = malloc( tc->size );

    return tc;
}

int tr_trackerPulse( tr_tracker_t * tc )
{
    tr_info_t * inf = &tc->tor->info;

    if( ( tc->status & TC_STATUS_IDLE ) &&
        ( ( ( tc->started || tc->completed || tc->stopped ) &&
             tr_date() > tc->date + 1000 ) ||
          tr_date() > tc->date + 1000 * TR_ANNOUNCE_INTERVAL ) )
    {
        struct in_addr addr;

        /* We have a special query to send or we reached the announce
           interval. Let's connect to the tracker */
        tc->date = tr_date();
        tr_inf( "Tracker: connecting to %s:%d",
                inf->trackerAddress, inf->trackerPort );
        if( tr_netResolve( inf->trackerAddress, &addr ) )
        {
            return 0;
        }
        tc->socket = tr_netOpen( addr, htons( inf->trackerPort ) );
        if( tc->socket < 0 )
        {
            return 0;
        }
        tc->status = TC_STATUS_CONNECT;
    }

    if( tc->status & TC_STATUS_CONNECT )
    {
        /* We are connecting to the tracker. Try to send the query */
        sendQuery( tc );
    }

    if( tc->status & TC_STATUS_RECV )
    {
        /* Try to get something */
        recvAnswer( tc );
    }

    return 0;
}

void tr_trackerCompleted( tr_tracker_t * tc )
{
    tc->completed = 1;
}

void tr_trackerClose( tr_tracker_t * tc )
{
    uint64_t date = tr_date();

    tc->stopped = 1;
    while( tc->stopped && tr_date() < date + 3000 )
    {
        /* Try to tell the tracker for 3 seconds, then give up */
        tr_trackerPulse( tc );
        tr_wait( 20 );
    }
    free( tc->buf );
    free( tc );
}

static void sendQuery( tr_tracker_t * tc )
{
    tr_torrent_t * tor = tc->tor;
    tr_info_t    * inf = &tor->info;

    char     * event;
    uint64_t   left;
    int        peers;
    int        ret;

    if( tc->started )
        event = "&event=started";
    else if( tc->completed )
        event = "&event=completed";
    else if( tc->stopped )
        event = "&event=stopped";
    else
        event = "";

    left  = (uint64_t) ( tor->blockCount - tor->blockHaveCount ) *
            (uint64_t) tor->blockSize;
    left  = MIN( left, inf->totalSize );
    peers = TR_MAX_PEER_COUNT - tor->peerCount;

    snprintf( tc->buf, tc->size,
              "GET %s?info_hash=%s&peer_id=%s&port=%d&uploaded=%lld&"
              "downloaded=%lld&left=%lld&compact=1&numwant=%d%s "
              "HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
              inf->trackerAnnounce, tor->hashString, tc->id,
              tor->bindPort, tor->uploaded[9], tor->downloaded[9],
              left, peers, event, inf->trackerAddress );

    ret = tr_netSend( tc->socket, tc->buf, strlen( tc->buf ) );
    if( ret & TR_NET_CLOSE )
    {
        tr_inf( "Tracker: connection failed" );
        tr_netClose( tc->socket );
        tc->status = TC_STATUS_IDLE;
    }
    else if( ret & TR_NET_BLOCK )
    {
        if( tr_date() > tc->date + TR_ANNOUNCE_INTERVAL * 3000 )
        {
            /* This is taking too long */
            tr_inf( "Tracker: timeout reached (%d s)",
                    TR_ANNOUNCE_INTERVAL * 3 );
            tr_netClose( tc->socket );
            tc->status = TC_STATUS_IDLE;
        }
    }
    else
    {
        // printf( "Tracker: sent %s", tc->buf );
        tc->status = TC_STATUS_RECV;
        tc->pos    = 0;
    }
}

static void recvAnswer( tr_tracker_t * tc )
{
    int ret;
    int i;
    benc_val_t   beAll;
    benc_val_t * bePeers, * beFoo;

    if( tc->pos == tc->size )
    {
        tc->size *= 2;
        tc->buf   = realloc( tc->buf, tc->size );
    }
    
    ret = tr_netRecv( tc->socket, &tc->buf[tc->pos],
                    tc->size - tc->pos );

    if( ret & TR_NET_BLOCK )
    {
        return;
    }
    if( !( ret & TR_NET_CLOSE ) )
    {
        // printf( "got %d bytes\n", ret );
        tc->pos += ret;
        return;
    }

    tr_netClose( tc->socket );
    // printf( "connection closed, got total %d bytes\n", tc->pos );

    tc->started   = 0;
    tc->completed = 0;
    tc->stopped   = 0;
    tc->status    = TC_STATUS_IDLE;

    if( tc->pos < 1 )
    {
        /* We got nothing */
        return;
    }

    /* Find the beginning of the dictionnary */
    for( i = 0; i < tc->pos - 18; i++ )
    {
        /* Hem */
        if( !strncmp( &tc->buf[i], "d8:interval", 11 ) ||
            !strncmp( &tc->buf[i], "d8:complete", 11 ) ||
            !strncmp( &tc->buf[i], "d14:failure reason", 18 ) )
        {
            break;
        }
    }

    if( i >= tc->pos - 18 )
    {
        tr_err( "Tracker error: no dictionnary in answer" );
        // printf( "%s\n", tc->buf );
        return;
    }

    if( tr_bencLoad( &tc->buf[i], &beAll, NULL ) )
    {
        tr_err( "Tracker error: error parsing bencoded data" );
        return;
    }

    // tr_bencPrint( &beAll );

    if( ( bePeers = tr_bencDictFind( &beAll, "failure reason" ) ) )
    {
        tr_err( "Tracker error: %s", bePeers->val.s.s );
        tc->tor->status |= TR_TRACKER_ERROR;
        snprintf( tc->tor->error, sizeof( tc->tor->error ),
                  bePeers->val.s.s );
        goto cleanup;
    }

    tc->tor->status &= ~TR_TRACKER_ERROR;

    if( !( bePeers = tr_bencDictFind( &beAll, "peers" ) ) )
    {
        tr_err( "Tracker error: no \"peers\" field" );
        goto cleanup;
    }

    if( bePeers->type & TYPE_LIST )
    {
        char * ip;
        int    port;

        /* Original protocol */
        tr_inf( "Tracker: got %d peers", bePeers->val.l.count );
        for( i = 0; i < bePeers->val.l.count; i++ )
        {
            beFoo = tr_bencDictFind( &bePeers->val.l.vals[i], "ip" );
            if( !beFoo )
                continue;
            ip = beFoo->val.s.s;
            beFoo = tr_bencDictFind( &bePeers->val.l.vals[i], "port" );
            if( !beFoo )
                continue;
            port = beFoo->val.i;

            tr_peerAddOld( tc->tor, ip, port );
        }
    }
    else if( bePeers->type & TYPE_STR )
    {
        struct in_addr addr;
        in_port_t      port;

        /* "Compact" extension */
        if( bePeers->val.s.i % 6 )
        {
            tr_err( "Tracker error: \"peers\" of size %d",
                    bePeers->val.s.i );
            tr_lockUnlock( tc->tor->lock );
            goto cleanup;
        }

        tr_inf( "Tracker: got %d peers", bePeers->val.s.i / 6 );
        for( i = 0; i < bePeers->val.s.i / 6; i++ )
        {
            memcpy( &addr, &bePeers->val.s.s[6*i],   4 );
            memcpy( &port, &bePeers->val.s.s[6*i+4], 2 );

            tr_peerAddCompact( tc->tor, addr, port, -1 );
        }
    }

cleanup:
    tr_bencFree( &beAll );
}

int tr_trackerScrape( tr_torrent_t * tor, int * seeders, int * leechers )
{
    tr_info_t * inf = &tor->info;

    int s, i, ret;
    char buf[1024];
    benc_val_t scrape, * val1, * val2;
    struct in_addr addr;
    uint64_t date;
    int pos;

    if( !tor->scrape[0] )
    {
        /* scrape not supported */
        return 1;
    }

    if( tr_netResolve( inf->trackerAddress, &addr ) )
    {
        return 0;
    }
    s = tr_netOpen( addr, htons( inf->trackerPort ) );
    if( s < 0 )
    {
        return 1;
    }

    snprintf( buf, sizeof( buf ),
              "GET %s?info_hash=%s HTTP/1.1\r\n"
              "Host: %s\r\n"
              "Connection: close\r\n\r\n",
              tor->scrape, tor->hashString,
              inf->trackerAddress );

    for( date = tr_date();; )
    {
        ret = tr_netSend( s, buf, strlen( buf ) );
        if( ret & TR_NET_CLOSE )
        {
            fprintf( stderr, "Could not connect to tracker\n" );
            tr_netClose( s );
            return 1;
        }
        else if( ret & TR_NET_BLOCK )
        {
            if( tr_date() > date + 10000 )
            {
                fprintf( stderr, "Could not connect to tracker\n" );
                tr_netClose( s );
                return 1;
            }
        }
        else
        {
            break;
        }
        tr_wait( 10 );
    }

    pos = 0;
    for( date = tr_date();; )
    {
        ret = tr_netRecv( s, &buf[pos], sizeof( buf ) - pos );
        if( ret & TR_NET_CLOSE )
        {
            break;
        }
        else if( ret & TR_NET_BLOCK )
        {
            if( tr_date() > date + 10000 )
            {
                fprintf( stderr, "Could not read from tracker\n" );
                tr_netClose( s );
                return 1;
            }
        }
        else
        {
            pos += ret;
        }
        tr_wait( 10 );
    }

    if( pos < 1 )
    {
        fprintf( stderr, "Could not read from tracker\n" );
        tr_netClose( s );
        return 1;
    }

    for( i = 0; i < ret - 8; i++ )
    {
        if( !strncmp( &buf[i], "d5:files", 8 ) )
        {
            break;
        }
    }
    if( i >= ret - 8 )
    {
        return 1;
    }
    if( tr_bencLoad( &buf[i], &scrape, NULL ) )
    {
        return 1;
    }

    val1 = tr_bencDictFind( &scrape, "files" );
    if( !val1 )
    {
        return 1;
    }
    val1 = &val1->val.l.vals[1];
    if( !val1 )
    {
        return 1;
    }
    val2 = tr_bencDictFind( val1, "complete" );
    if( !val2 )
    {
        return 1;
    }
    *seeders = val2->val.i;
    val2 = tr_bencDictFind( val1, "incomplete" );
    if( !val2 )
    {
        return 1;
    }
    *leechers = val2->val.i;
    tr_bencFree( &scrape );

    return 0;
}
