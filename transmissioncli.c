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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include "libtransmission/transmission.h"

#define USAGE \
"Usage: %s [options] file.torrent [options]\n\n" \
"Options:\n" \
"  -h, --help           Print this help and exit\n" \
"  -i, --info           Print metainfo and exit\n" \
"  -s, --scrape         Print counts of seeders/leechers and exit\n" \
"  -v, --verbose <int>  Verbose level (0 to 2, default = 0)\n" \
"  -p, --port <int>     Port we should listen on (default = 9090)\n" \
"  -u, --upload <int>   Maximum upload rate (-1 = no limit, default = 20)\n"

static int             showHelp     = 0;
static int             showInfo     = 0;
static int             showScrape   = 0;
static int             verboseLevel = 0;
static int             bindPort     = 9090;
static int             uploadLimit  = 20;
static char          * torrentPath  = NULL;
static volatile char   mustDie      = 0;

static int  parseCommandLine ( int argc, char ** argv );
static void sigHandler       ( int signal );

int main( int argc, char ** argv )
{
    int           i;
    tr_handle_t * h;
    tr_stat_t     trStat;

//    printf( "Transmission %s - http://transmission.m0k.org/\n\n",
//            TR_VERSION );

    /* Get options */
    if( parseCommandLine( argc, argv ) )
    {
        printf( USAGE, argv[0] );
        return 1;
    }

    if( showHelp )
    {
        printf( USAGE, argv[0] );
        return 0;
    }

    if( verboseLevel < 0 )
    {
        verboseLevel = 0;
    }
    else if( verboseLevel > 9 )
    {
        verboseLevel = 9;
    }
    if( verboseLevel )
    {
        static char env[11];
        sprintf( env, "TR_DEBUG=%d", verboseLevel );
        putenv( env );
    }

    if( bindPort < 1 || bindPort > 65535 )
    {
        printf( "Invalid port '%d'\n", bindPort );
        return 1;
    }

    /* Initialize libtransmission */
    h = tr_init();

    /* Open and parse torrent file */
    if( tr_torrentInit( h, torrentPath ) )
    {
        printf( "Failed opening torrent file `%s'\n", torrentPath );
        goto failed;
    }

    if( showInfo )
    {
        tr_info_t * info;

        tr_torrentStat( h, 0, &trStat );
        info = trStat.info;

        /* Print torrent info (quite à la btshowmetainfo) */
        printf( "hash:     " );
        for( i = 0; i < SHA_DIGEST_LENGTH; i++ )
        {
            printf( "%02x", info->hash[i] );
        }
        printf( "\n" );
        printf( "tracker:  %s:%d\n",
                info->trackerAddress, info->trackerPort );
        printf( "announce: %s\n", info->trackerAnnounce );
        printf( "size:     %lld (%lld * %d + %lld)\n",
                info->totalSize, info->totalSize / info->pieceSize,
                info->pieceSize, info->totalSize % info->pieceSize );
        printf( "file(s):\n" );
        for( i = 0; i < info->fileCount; i++ )
        {
            printf( " %s (%lld)\n", info->files[i].name,
                    info->files[i].length );
        }

        goto cleanup;
    }

    if( showScrape )
    {
        int seeders, leechers;

        if( tr_torrentScrape( h, 0, &seeders, &leechers ) )
        {
            printf( "Scrape failed.\n" );
        }
        else
        {
            printf( "%d seeder(s), %d leecher(s).\n", seeders, leechers );
        }

        goto cleanup;
    }

    signal( SIGINT, sigHandler );

    tr_setBindPort( h, bindPort );
    tr_setUploadLimit( h, uploadLimit );
    
    tr_torrentSetFolder( h, 0, "." );
    tr_torrentStart( h, 0 );

    while( !mustDie )
    {
        char string[80];
        int  chars = 0;

        sleep( 1 );

        tr_torrentStat( h, 0, &trStat );
        if( trStat.status & TR_STATUS_CHECK )
        {
            chars = snprintf( string, 80,
                "Checking files... %.2f %%", 100.0 * trStat.progress );
        }
        else if( trStat.status & TR_STATUS_DOWNLOAD )
        {
            chars = snprintf( string, 80,
                "Progress: %.2f %%, downloading from %d of %d peer%s, "
                "D: %.2f / U: %.2f kbps", 100.0 * trStat.progress,
                trStat.peersUploading, trStat.peersTotal,
                ( trStat.peersTotal == 1 ) ? "" : "s",
                trStat.rateDownload, trStat.rateUpload );
        }
        else if( trStat.status & TR_STATUS_SEED )
        {
            chars = snprintf( string, 80,
                "Seeding, uploading to %d of %d peer(s), %.2f kbps",
                trStat.peersDownloading, trStat.peersTotal,
                trStat.rateUpload );
        }
        memset( &string[chars], ' ', 79 - chars );
        string[79] = '\0';
        fprintf( stderr, "\r%s", string );

        if( trStat.status & TR_TRACKER_ERROR )
        {
            fprintf( stderr, "\n%s\n", trStat.error );
        }
        else if( verboseLevel > 0 )
        {
            fprintf( stderr, "\n" );
        }
    }
    fprintf( stderr, "\n" );

    tr_torrentStop( h, 0 );
    
cleanup:
    tr_torrentClose( h, 0 );

failed:
    tr_close( h );

    return 0;
}

static int parseCommandLine( int argc, char ** argv )
{
    for( ;; )
    {
        static struct option long_options[] =
          { { "help",    no_argument,       NULL, 'h' },
            { "info",    no_argument,       NULL, 'i' },
            { "scrape",  no_argument,       NULL, 's' },
            { "verbose", required_argument, NULL, 'v' },
            { "port",    required_argument, NULL, 'p' },
            { "upload",  required_argument, NULL, 'u' },
            { 0, 0, 0, 0 } };

        int c, optind = 0;
        c = getopt_long( argc, argv, "hisv:p:u:", long_options, &optind );
        if( c < 0 )
        {
            break;
        }
        switch( c )
        {
            case 'h':
                showHelp = 1;
                break;
            case 'i':
                showInfo = 1;
                break;
            case 's':
                showScrape = 1;
                break;
            case 'v':
                verboseLevel = atoi( optarg );
                break;
            case 'p':
                bindPort = atoi( optarg );
                break;
            case 'u':
                uploadLimit = atoi( optarg );
                break;
            default:
                return 1;
        }
    }

    if( optind > argc - 1  )
    {
        return !showHelp;
    }

    torrentPath = argv[optind];

    return 0;
}

static void sigHandler( int signal )
{
    switch( signal )
    {
        case SIGINT:
            mustDie = 1;
            break;

        default:
            break;
    }
}
