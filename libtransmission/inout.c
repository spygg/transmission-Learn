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

#ifdef SYS_BEOS
# define fseeko _fseek
#endif

struct tr_io_s
{
    tr_torrent_t * tor;

    /* File descriptors */
    FILE       ** fds;

    /* Position of pieces
       -1 = we haven't started to download this piece yet
        n = we have started or completed the piece in slot n */
    int         * pieceSlot;

    /* Pieces in slot
       -1 = unused slot
        n = piece n */
    int         * slotPiece;

    int           slotsUsed;
};

/***********************************************************************
 * Local prototypes
 **********************************************************************/
static int  createFiles( tr_io_t * );
static int  openAndCheckFiles( tr_io_t * );
static void closeFiles( tr_io_t * );
static int  readOrWriteBytes( tr_io_t *, uint64_t, int, char *, int );
static int  readOrWriteSlot( tr_io_t * io, int slot, uint8_t * buf,
                             int * size, int write );
static void findSlotForPiece( tr_io_t *, int );
static void fastResumeSave( tr_io_t * );
static int  fastResumeLoad( tr_io_t * );

#define readBytes(io,o,s,b)  readOrWriteBytes(io,o,s,b,0)
#define writeBytes(io,o,s,b) readOrWriteBytes(io,o,s,b,1)

#define readSlot(io,sl,b,s)  readOrWriteSlot(io,sl,b,s,0)
#define writeSlot(io,sl,b,s) readOrWriteSlot(io,sl,b,s,1)

/***********************************************************************
 * tr_ioInit
 ***********************************************************************
 * Open all files we are going to write to
 **********************************************************************/
tr_io_t * tr_ioInit( tr_torrent_t * tor )
{
    tr_io_t * io;

    io      = malloc( sizeof( tr_io_t ) );
    io->tor = tor;

    if( createFiles( io ) || openAndCheckFiles( io ) )
    {
        free( io );
        return NULL;
    }

    return io;
}

/***********************************************************************
 * tr_ioRead
 ***********************************************************************
 *
 **********************************************************************/
int tr_ioRead( tr_io_t * io, int index, int begin, int length,
               char * buf )
{
    uint64_t    offset;
    tr_info_t * inf = &io->tor->info;

    offset = (uint64_t) io->pieceSlot[index] *
        (uint64_t) inf->pieceSize + (uint64_t) begin;

    return readBytes( io, offset, length, buf );
}

/***********************************************************************
 * tr_ioWrite
 ***********************************************************************
 *
 **********************************************************************/
int tr_ioWrite( tr_io_t * io, int index, int begin, int length,
                char * buf )
{
    tr_torrent_t * tor = io->tor;
    tr_info_t    * inf = &io->tor->info;
    uint64_t       offset;
    int            i;
    uint8_t        hash[SHA_DIGEST_LENGTH];
    uint8_t      * pieceBuf;
    int            pieceSize;
    int            startBlock, endBlock;

    if( io->pieceSlot[index] < 0 )
    {
        findSlotForPiece( io, index );
        tr_inf( "Piece %d: starting in slot %d", index,
                io->pieceSlot[index] );
    }

    offset = (uint64_t) io->pieceSlot[index] *
        (uint64_t) inf->pieceSize + (uint64_t) begin;

    if( writeBytes( io, offset, length, buf ) )
    {
        return 1;
    }

    startBlock = tr_pieceStartBlock( index );
    endBlock   = startBlock + tr_pieceCountBlocks( index );
    for( i = startBlock; i < endBlock; i++ )
    {
        if( tor->blockHave[i] >= 0 )
        {
            /* The piece is not complete */
            return 0;
        }
    }

    /* The piece is complete, check the hash */
    pieceSize = tr_pieceSize( index );
    pieceBuf  = malloc( pieceSize );
    readBytes( io, (uint64_t) io->pieceSlot[index] *
               (uint64_t) inf->pieceSize, pieceSize, pieceBuf );
    SHA1( pieceBuf, pieceSize, hash );
    free( pieceBuf );

    if( memcmp( hash, &inf->pieces[20*index], SHA_DIGEST_LENGTH ) )
    {
        tr_inf( "Piece %d (slot %d): hash FAILED", index,
                io->pieceSlot[index] );

        /* We will need to reload the whole piece */
        for( i = startBlock; i < endBlock; i++ )
        {
            tor->blockHave[i]    = 0;
            tor->blockHaveCount -= 1;
        }
    }
    else
    {
        tr_inf( "Piece %d (slot %d): hash OK", index,
                io->pieceSlot[index] );
        tr_bitfieldAdd( tor->bitfield, index );
    }

    return 0;
}

void tr_ioClose( tr_io_t * io )
{
    closeFiles( io );

    fastResumeSave( io );

    free( io->pieceSlot );
    free( io->slotPiece );
    free( io );
}

/***********************************************************************
 * createFiles
 ***********************************************************************
 * Make sure the existing folders/files have correct types and
 * permissions, and create missing folders and files
 **********************************************************************/
static int createFiles( tr_io_t * io )
{
    tr_torrent_t * tor = io->tor;
    tr_info_t    * inf = &tor->info;

    int           i;
    char        * path, * p;
    struct stat   sb;
    FILE        * file;

    tr_dbg( "Creating files..." );

    for( i = 0; i < inf->fileCount; i++ )
    {
        asprintf( &path, "%s/%s", tor->destination, inf->files[i].name );

        /* Create folders */
        p = path;
        while( ( p = strchr( p, '/' ) ) )
        {
            *p = '\0';
            if( stat( path, &sb ) )
            {
                /* Folder doesn't exist yet */
                mkdir( path, 0755 );
            }
            else if( ( sb.st_mode & S_IFMT ) != S_IFDIR )
            {
                /* Node exists but isn't a folder */
                printf( "Remove %s, it's in the way.\n", path );
                free( path );
                return 1;
            }
            *p = '/';
            p++;
        }

        if( stat( path, &sb ) )
        {
            /* File doesn't exist yet */
            if( !( file = fopen( path, "w" ) ) )
            {
                tr_err( "Could not create `%s' (%s)", path,
                        strerror( errno ) );
                free( path );
                return 1;
            }
            fclose( file );
        }
        else if( ( sb.st_mode & S_IFMT ) != S_IFREG )
        {
            /* Node exists but isn't a file */
            printf( "Remove %s, it's in the way.\n", path );
            free( path );
            return 1;
        }

        free( path );
    }

    return 0;
}

/***********************************************************************
 * openAndCheckFiles
 ***********************************************************************
 * Open all files in read/write and look for complete pieces
 **********************************************************************/
static int openAndCheckFiles( tr_io_t * io )
{
    tr_torrent_t * tor = io->tor;
    tr_info_t    * inf = &tor->info;

    int i;
    char * path;
    uint8_t * buf;
    uint8_t hash[SHA_DIGEST_LENGTH];
    int startBlock, endBlock;

    io->fds = malloc( inf->fileCount * sizeof( FILE * ) );

    /* Open all files */
    for( i = 0; i < inf->fileCount; i++ )
    {
        asprintf( &path, "%s/%s", tor->destination, inf->files[i].name );

        if( !( io->fds[i] = fopen( path, "r+" ) ) )
        {
            printf( "fopen failed for `%s'\n", path );
            free( path );
            free( io->fds );
            return 1;
        }

        free( path );
    }

    io->pieceSlot = malloc( inf->pieceCount * sizeof( int ) );
    io->slotPiece = malloc( inf->pieceCount * sizeof( int ) );

    if( !fastResumeLoad( io ) )
    {
        return 0;
    }

    tr_dbg( "Checking pieces..." );

    /* Yet we don't have anything */
    memset( io->pieceSlot, 0xFF, inf->pieceCount * sizeof( int ) );
    memset( io->slotPiece, 0xFF, inf->pieceCount * sizeof( int ) );
    memset( tor->bitfield, 0, ( inf->pieceCount + 7 ) / 8 );
    memset( tor->blockHave, 0, tor->blockCount );
    tor->blockHaveCount = 0;

    /* Check pieces */
    io->slotsUsed = 0;
    buf           = malloc( inf->pieceSize );
    for( i = 0; i < inf->pieceCount; i++ )
    {
        int size, j;

        if( readSlot( io, i, buf, &size ) )
        {
            break;
        }

        io->slotsUsed = i + 1;
        SHA1( buf, size, hash );

        for( j = i; j < inf->pieceCount - 1; j++ )
        {
            if( !memcmp( hash, &inf->pieces[20*j], SHA_DIGEST_LENGTH ) )
            {
                int k;
                io->pieceSlot[j] = i;
                io->slotPiece[i] = j;
                tr_bitfieldAdd( tor->bitfield, j );

                startBlock = tr_pieceStartBlock( j );
                endBlock   = startBlock + tr_pieceCountBlocks( j );
                for( k = startBlock; k < endBlock; k++ )
                {
                    tor->blockHave[k] = -1;
                    tor->blockHaveCount++;
                }
                break;
            }
        }

        if( io->slotPiece[i] > -1 )
        {
            continue;
        }

        /* Special case for the last piece */
        SHA1( buf, tr_pieceSize( inf->pieceCount - 1 ), hash );
        if( !memcmp( hash, &inf->pieces[20 * (inf->pieceCount - 1)],
                     SHA_DIGEST_LENGTH ) )
        {
            io->pieceSlot[inf->pieceCount - 1] = i;
            io->slotPiece[i]                   = inf->pieceCount - 1;
            tr_bitfieldAdd( tor->bitfield, inf->pieceCount - 1 );

            startBlock = tr_pieceStartBlock( inf->pieceCount - 1 );
            endBlock   = startBlock +
                tr_pieceCountBlocks( inf->pieceCount - 1 );
            for( j = startBlock; j < endBlock; j++ )
            {
                tor->blockHave[j] = -1;
                tor->blockHaveCount++;
            }
        }
    }
    free( buf );

    return 0;
}

/***********************************************************************
 * closeFiles
 **********************************************************************/
static void closeFiles( tr_io_t * io )
{
    tr_torrent_t * tor = io->tor;
    tr_info_t    * inf = &tor->info;

    int i;

    for( i = 0; i < inf->fileCount; i++ )
    {
        fclose( io->fds[i] );
    }
    free( io->fds );
}

/***********************************************************************
 * readOrWriteBytes
 ***********************************************************************
 *
 **********************************************************************/
static int readOrWriteBytes( tr_io_t * io, uint64_t offset, int size,
                             char * buf, int write )
{
    tr_torrent_t * tor = io->tor;
    tr_info_t    * inf = &tor->info;

    int          piece = offset / inf->pieceSize;
    int          begin = offset % inf->pieceSize;

    int          i;
    uint64_t     foo;
    int          file = 0;
    uint64_t     posInFile = 0;
    int          willRead;

    /* We can't ever read or write more than a piece at a time */
    if( tr_pieceSize( piece ) < begin + size )
    {
        return 1;
    }

    /* Find which file we shall start reading/writing in */
    foo = 0;
    for( i = 0; i < inf->fileCount; i++ )
    {
        if( offset < foo + inf->files[i].length )
        {
            file      = i;
            posInFile = offset - foo;
            break;
        }
        foo += inf->files[i].length;
    }

    while( size > 0 )
    {
        willRead = MIN( inf->files[file].length - posInFile,
                          (uint64_t) size );

        if( fseeko( io->fds[file], posInFile, SEEK_SET ) )
        {
            return 1;
        }
        if( write )
        {
            if( fwrite( buf, willRead, 1, io->fds[file] ) != 1 )
            {
                return 1;
            }
        }
        else
        {
            if( fread( buf, willRead, 1, io->fds[file] ) != 1 )
            {
                return 1;
            }
        }

        /* 'willRead' less bytes to do */
        size -= willRead;
        buf  += willRead;

        /* Go to the beginning of the next file */
        file      += 1;
        posInFile  = 0;
    }

    return 0;
}

/***********************************************************************
 * readSlot
 ***********************************************************************
 * 
 **********************************************************************/
static int readOrWriteSlot( tr_io_t * io, int slot, uint8_t * buf,
                            int * size, int write )
{
    tr_torrent_t * tor = io->tor;
    tr_info_t    * inf = &tor->info;

    uint64_t offset = (uint64_t) slot * (uint64_t) inf->pieceSize;
    
    *size = 0;
    if( slot == inf->pieceCount - 1 )
    {
        *size = inf->totalSize % inf->pieceSize;
    }
    if( !*size )
    {
        *size = inf->pieceSize;
    }

    return readOrWriteBytes( io, offset, *size, buf, write );
}

static void invertSlots( tr_io_t * io, int slot1, int slot2 )
{
    tr_torrent_t * tor = io->tor;
    tr_info_t    * inf = &tor->info;

    uint8_t * buf1, * buf2;
    int piece1, piece2, foo;

    buf1 = calloc( inf->pieceSize, 1 );
    buf2 = calloc( inf->pieceSize, 1 );

    readSlot( io, slot1, buf1, &foo );
    readSlot( io, slot2, buf2, &foo );

    writeSlot( io, slot1, buf2, &foo );
    writeSlot( io, slot2, buf1, &foo );

    free( buf1 );
    free( buf2 );

    piece1                = io->slotPiece[slot1];
    piece2                = io->slotPiece[slot2];
    io->slotPiece[slot1]  = piece2;
    io->slotPiece[slot2]  = piece1;
    io->pieceSlot[piece1] = slot2;
    io->pieceSlot[piece2] = slot1;
}

static void reorderPieces( tr_io_t * io )
{
    tr_torrent_t * tor = io->tor;
    tr_info_t    * inf = &tor->info;

    int i, didInvert;

    /* Try to move pieces to their final places */
    do
    {
        didInvert = 0;

        for( i = 0; i < inf->pieceCount; i++ )
        {
            if( io->pieceSlot[i] < 0 )
            {
                /* We haven't started this piece yet */
                continue;
            }
            if( io->pieceSlot[i] == i )
            {
                /* Already in place */
                continue;
            }
            if( i >= io->slotsUsed )
            {
                /* File is not big enough yet */
                continue;
            }

            /* Move piece i into slot i */
            tr_inf( "invert %d and %d", io->pieceSlot[i], i );
            invertSlots( io, io->pieceSlot[i], i );
            didInvert = 1;
        }
    } while( didInvert );
}

static void findSlotForPiece( tr_io_t * io, int piece )
{
    int i;
#if 0
    tr_torrent_t * tor = io->tor;
    tr_info_t    * inf = &tor->info;

    tr_dbg( "Entering findSlotForPiece" );

    for( i = 0; i < inf->pieceCount; i++ )
        printf( "%02d ", io->slotPiece[i] );
    printf( "\n" );
    for( i = 0; i < inf->pieceCount; i++ )
        printf( "%02d ", io->pieceSlot[i] );
    printf( "\n" );
#endif

    /* Look for an empty slot somewhere */
    for( i = 0; i < io->slotsUsed; i++ )
    {
        if( io->slotPiece[i] < 0 )
        {
            io->pieceSlot[piece] = i;
            io->slotPiece[i]     = piece;
            goto reorder;
        }
    }

    /* No empty slot, extend the file */
    io->pieceSlot[piece]         = io->slotsUsed;
    io->slotPiece[io->slotsUsed] = piece;
    (io->slotsUsed)++;

  reorder:
    reorderPieces( io );

#if 0
    for( i = 0; i < inf->pieceCount; i++ )
        printf( "%02d ", io->slotPiece[i] );
    printf( "\n" );
    for( i = 0; i < inf->pieceCount; i++ )
        printf( "%02d ", io->pieceSlot[i] );
    printf( "\n" );

    printf( "Leaving findSlotForPiece\n" );
#endif
}

/***********************************************************************
 * Fast resume
 ***********************************************************************
 * Format of the resume file:
 *  - 4 bytes: format version (currently 0)
 *  - 4 bytes * number of files: mtimes of files
 *  - 1 byte * number of blocks: whether we have the block or not
 *  - 4 bytes * number of pieces: the pieces that has been completed or
 *    started in each slot
 *
 * The resume file is located in the same folder we are downloading to.
 * Its name is ".libtransmission.<inf->name>".
 *
 * All values are stored in the native endianness. It is not safe to
 * move a libtransmission resume file from an architecture to another.
 **********************************************************************/
static char * fastResumeFolderName()
{
    static char folderName[MAX_PATH_LENGTH];

    snprintf( folderName, MAX_PATH_LENGTH, "%s/.transmission",
              getenv( "HOME" ) );

    return folderName;
}

static char * fastResumeFileName( tr_io_t * io )
{
    static char   fileName[MAX_PATH_LENGTH];
    char        * p;
    int           i;

    snprintf( fileName, MAX_PATH_LENGTH, "%s/resume.",
              fastResumeFolderName() );
    p = &fileName[ strlen( fileName ) ];
    for( i = 0; i < SHA_DIGEST_LENGTH; i++ )
    {
        sprintf( p, "%02x", io->tor->info.hash[i] );
        p += 2;
    }

    return fileName;
}

static int fastResumeMTimes( tr_io_t * io, int * tab )
{
    tr_torrent_t * tor = io->tor;
    tr_info_t    * inf = &tor->info;

    int           i;
    char        * path;
    struct stat   sb;

    for( i = 0; i < inf->fileCount; i++ )
    {
        asprintf( &path, "%s/%s", tor->destination, inf->files[i].name );
        if( stat( path, &sb ) )
        {
            tr_err( "Could not stat '%s'", path );
            free( path );
            return 1;
        }
        if( ( sb.st_mode & S_IFMT ) != S_IFREG )
        {
            tr_err( "Wrong st_mode for '%s'", path );
            free( path );
            return 1;
        }
        free( path );

#ifdef SYS_DARWIN
        tab[i] = ( sb.st_mtimespec.tv_sec & 0x7FFFFFFF );
#else
        tab[i] = ( sb.st_mtime & 0x7FFFFFFF );
#endif
    }

    return 0;
}

static void fastResumeSave( tr_io_t * io )
{
    tr_torrent_t * tor = io->tor;
    tr_info_t    * inf = &tor->info;
    
    FILE    * file;
    int       version = 0;
    char    * path;
    int     * fileMTimes;
    int       i;
    uint8_t * blockBitfield;

    /* Get file sizes */
    fileMTimes = malloc( inf->fileCount * 4 );
    if( fastResumeMTimes( io, fileMTimes ) )
    {
        free( fileMTimes );
        return;
    }

    /* Create folder if missing */
    mkdir( fastResumeFolderName(), 0755 );

    /* Create/overwrite the resume file */
    path = fastResumeFileName( io );
    if( !( file = fopen( path, "w" ) ) )
    {
        tr_err( "Could not open '%s' for writing", path );
        free( fileMTimes );
        return;
    }
    
    /* Write format version */
    fwrite( &version, 4, 1, file );

    /* Write file mtimes */
    fwrite( fileMTimes, 4, inf->fileCount, file );
    free( fileMTimes );

    /* Build and write the bitfield for blocks */
    blockBitfield = calloc( ( tor->blockCount + 7 ) / 8, 1 );
    for( i = 0; i < tor->blockCount; i++ )
    {
        if( tor->blockHave[i] < 0 )
        {
            tr_bitfieldAdd( blockBitfield, i );
        }
    }
    fwrite( blockBitfield, ( tor->blockCount + 7 ) / 8, 1, file );
    free( blockBitfield );

    /* Write the 'slotPiece' table */
    fwrite( io->slotPiece, 4, inf->pieceCount, file );

    fclose( file );
}

static int fastResumeLoad( tr_io_t * io )
{
    tr_torrent_t * tor = io->tor;
    tr_info_t    * inf = &tor->info;
    
    FILE    * file;
    int       version = 0;
    char    * path;
    int     * fileMTimes1, * fileMTimes2;
    int       i, j;
    uint8_t * blockBitfield;

    int size;

    /* Open resume file */
    path = fastResumeFileName( io );
    if( !( file = fopen( path, "r" ) ) )
    {
        tr_inf( "Could not open '%s' for reading", path );
        return 1;
    }

    /* Check the size */
    size = 4 + 4 * inf->fileCount + 4 * inf->pieceCount +
        ( tor->blockCount + 7 ) / 8;
    fseek( file, 0, SEEK_END );
    if( ftell( file ) != size )
    {
        tr_inf( "Wrong size for resume file (%d bytes, %d expected)",
                ftell( file ), size );
        fclose( file );
        return 1;
    }
    fseek( file, 0, SEEK_SET );

    /* Check format version */
    fread( &version, 4, 1, file );
    if( version != 0 )
    {
        tr_inf( "Resume file has version %d, not supported",
                version );
        fclose( file );
        return 1;
    }

    /* Compare file mtimes */
    fileMTimes1 = malloc( inf->fileCount * 4 );
    if( fastResumeMTimes( io, fileMTimes1 ) )
    {
        free( fileMTimes1 );
        fclose( file );
        return 1;
    }
    fileMTimes2 = malloc( inf->fileCount * 4 );
    fread( fileMTimes2, 4, inf->fileCount, file );
    if( memcmp( fileMTimes1, fileMTimes2, inf->fileCount * 4 ) )
    {
        tr_inf( "File mtimes don't match" );
        free( fileMTimes1 );
        free( fileMTimes2 );
        fclose( file );
        return 1;
    }
    free( fileMTimes1 );
    free( fileMTimes2 );

    /* Load the bitfield for blocks and fill blockHave */
    blockBitfield = calloc( ( tor->blockCount + 7 ) / 8, 1 );
    fread( blockBitfield, ( tor->blockCount + 7 ) / 8, 1, file );
    for( i = 0; i < tor->blockCount; i++ )
    {
        if( tr_bitfieldHas( blockBitfield, i ) )
        {
            tor->blockHave[i] = -1;
            (tor->blockHaveCount)++;
        }
    }
    free( blockBitfield );

    /* Load the 'slotPiece' table */
    fread( io->slotPiece, 4, inf->pieceCount, file );

    fclose( file );

    /* Update io->pieceSlot, io->slotsUsed, and tor->bitfield */
    io->slotsUsed = 0;
    for( i = 0; i < inf->pieceCount; i++ )
    {
        io->pieceSlot[i] = -1;
        for( j = 0; j < inf->pieceCount; j++ )
        {
            if( io->slotPiece[j] == i )
            {
                tr_dbg( "Has piece %d in slot %d", i, j );
                io->pieceSlot[i] = j;
                io->slotsUsed = MAX( io->slotsUsed, j + 1 );
                break;
            }
        }

        for( j = tr_pieceStartBlock( i );
             j < tr_pieceStartBlock( i ) + tr_pieceCountBlocks( i );
             j++ )
        {
            if( tor->blockHave[j] > -1 )
            {
                break;
            }
        }
        if( j >= tr_pieceStartBlock( i ) + tr_pieceCountBlocks( i ) )
        {
            tr_dbg( "Piece %d is complete", i );
            tr_bitfieldAdd( tor->bitfield, i );
        }
    }
    tr_dbg( "Slot used: %d", io->slotsUsed );

    tr_inf( "Fast resuming successful" );
    
    return 0;
}
