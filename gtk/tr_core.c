/******************************************************************************
 * $Id$
 *
 * Copyright (c) 2007 Transmission authors and contributors
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

#include <string.h>

#include <gtk/gtk.h>
#include <glib/gi18n.h>

#include "bencode.h"
#include "transmission.h"

/* XXX */
#define TR_WANT_TORRENT_PRIVATE

#include "conf.h"
#include "tr_core.h"
#include "tr_torrent.h"
#include "util.h"

static void
tr_core_init( GTypeInstance * instance, gpointer g_class );
static void
tr_core_class_init( gpointer g_class, gpointer g_class_data );
static void
tr_core_marshal_err( GClosure * closure, GValue * ret, guint count,
                     const GValue * vals, gpointer hint, gpointer marshal );
static void
tr_core_dispose( GObject * obj );
static int
tr_core_check_torrents( TrCore * self );
static int
tr_core_check_zombies( TrCore * self );
static void
tr_core_insert( TrCore * self, TrTorrent * tor );
static void
tr_core_errsig( TrCore * self, enum tr_core_err type, const char * msg );

GType
tr_core_get_type( void )
{
    static GType type = 0;

    if( 0 == type )
    {
        static const GTypeInfo info =
        {
            sizeof( TrCoreClass ),
            NULL,                       /* base_init */
            NULL,                       /* base_finalize */
            tr_core_class_init,         /* class_init */
            NULL,                       /* class_finalize */
            NULL,                       /* class_data */
            sizeof( TrCore ),
            0,                          /* n_preallocs */
            tr_core_init,               /* instance_init */
            NULL,
        };
        type = g_type_register_static( G_TYPE_OBJECT, "TrCore", &info, 0 );
    }

    return type;
}

void
tr_core_class_init( gpointer g_class, gpointer g_class_data SHUTUP )
{
    GObjectClass * gobject_class;
    TrCoreClass  * core_class;

    gobject_class = G_OBJECT_CLASS( g_class );
    gobject_class->dispose = tr_core_dispose;

    core_class = TR_CORE_CLASS( g_class );
    core_class->errsig = g_signal_new( "error", G_TYPE_FROM_CLASS( g_class ),
                                       G_SIGNAL_RUN_LAST, 0, NULL, NULL,
                                       tr_core_marshal_err, G_TYPE_NONE,
                                       2, G_TYPE_INT, G_TYPE_STRING );
}

void
tr_core_marshal_err( GClosure * closure, GValue * ret SHUTUP, guint count,
                     const GValue * vals, gpointer hint SHUTUP,
                     gpointer marshal )
{
    typedef void (*TRMarshalErr) ( gpointer, enum tr_core_err, const char *,
                                   gpointer );
    TRMarshalErr     callback;
    GCClosure      * cclosure = (GCClosure*) closure;
    enum tr_core_err errcode;
    const char     * errstr;
    gpointer         inst, gdata;

    g_return_if_fail( 3 == count );

    inst    = g_value_peek_pointer( vals );
    errcode = g_value_get_int( vals + 1 );
    errstr  = g_value_get_string( vals + 2 );
    gdata   = closure->data;

    callback = (TRMarshalErr) ( NULL == marshal ?
                                cclosure->callback : marshal );
    callback( inst, errcode, errstr, gdata );
}

void
tr_core_init( GTypeInstance * instance, gpointer g_class SHUTUP )
{
    TrCore * self = (TrCore *) instance;
    GtkListStore * store;

    /* column types for the model used to store torrent information */
    /* keep this in sync with the enum near the bottom of tr_core.h */
    GType types[] =
    {
        /* info->name, info->totalSize, status,     error,      errorString, */
        G_TYPE_STRING, G_TYPE_UINT64,   G_TYPE_INT, G_TYPE_INT, G_TYPE_STRING,
        /* progress,  rateDownload, rateUpload,   eta,        peersTotal, */
        G_TYPE_FLOAT, G_TYPE_FLOAT, G_TYPE_FLOAT, G_TYPE_INT, G_TYPE_INT,
        /* peersUploading, peersDownloading, seeders,    leechers */
        G_TYPE_INT,        G_TYPE_INT,       G_TYPE_INT, G_TYPE_INT,
        /* completedFromTracker, downloaded,    uploaded       left */
        G_TYPE_INT,              G_TYPE_UINT64, G_TYPE_UINT64, G_TYPE_UINT64,
        /* tracker,            the TrTorrent object */
        TR_TRACKER_BOXED_TYPE, TR_TORRENT_TYPE,
    };

#ifdef REFDBG
    fprintf( stderr, "core    %p init\n", self );
#endif

    /* create the model used to store torrent data */
    g_assert( ALEN( types ) == MC_ROW_COUNT );
    store = gtk_list_store_newv( MC_ROW_COUNT, types );

    self->model    = GTK_TREE_MODEL( store );
    self->handle   = tr_init( "gtk" );
    self->zombies  = NULL;
    self->quitting = FALSE;
    self->disposed = FALSE;
}

void
tr_core_dispose( GObject * obj )
{
    TrCore       * self = (TrCore *) obj;
    GObjectClass * parent;
    GtkTreeIter    iter;
    GList        * ii;
    TrTorrent    * tor;

    if( self->disposed )
    {
        return;
    }
    self->disposed = TRUE;

#ifdef REFDBG
    fprintf( stderr, "core    %p dispose\n", self );
#endif

    /* sever all remaining torrents in the model */
    if( gtk_tree_model_get_iter_first( self->model, &iter ) )
    {
        do
        {
            gtk_tree_model_get( self->model, &iter, MC_TORRENT, &tor, -1 );
            tr_torrent_sever( tor );
            g_object_unref( tor );
        }
        while( gtk_tree_model_iter_next( self->model, &iter ) );
    }
    g_object_unref( self->model );

    /* sever and unref all remaining zombie torrents */
    if( NULL != self->zombies )
    {
        for( ii = g_list_first( self->zombies ); NULL != ii; ii = ii->next )
        {
            tr_torrent_sever( ii->data );
            g_object_unref( ii->data );
        }
        g_list_free( self->zombies );
    }

#ifdef REFDBG
    fprintf( stderr, "core    %p dead\n", self );
#endif

    /* close the libtransmission instance */
    tr_close( self->handle );

    /* Chain up to the parent class */
    parent = g_type_class_peek( g_type_parent( TR_CORE_TYPE ) );
    parent->dispose( obj );
}

TrCore *
tr_core_new( void )
{
    return g_object_new( TR_CORE_TYPE, NULL );
}

GtkTreeModel *
tr_core_model( TrCore * self )
{
    TR_IS_CORE( self );

    if( self->disposed )
    {
        return NULL;
    }

    return self->model;
}

tr_handle_t *
tr_core_handle( TrCore * self )
{
    TR_IS_CORE( self );

    if( self->disposed )
    {
        return NULL;
    }

    return self->handle;
}

void
tr_core_shutdown( TrCore * self )
{
    GtkTreeIter iter;
    TrTorrent * tor;
    tr_stat_t * st;

    TR_IS_CORE( self );

    if( self->disposed )
    {
        return;
    }

    g_assert( !self->quitting );
    self->quitting = TRUE;

    /* try to stop all the torrents nicely */
    if( gtk_tree_model_get_iter_first( self->model, &iter) )
    {
        do
        {
            gtk_tree_model_get( self->model, &iter, MC_TORRENT, &tor, -1 );
            st = tr_torrent_stat( tor );
            if( TR_STATUS_ACTIVE & st->status )
            {
                tr_torrentStop( tr_torrent_handle( tor ) );
            }
            g_object_unref( tor );
        }
        while( gtk_tree_model_iter_next( self->model, &iter ) );
    }

    /* shut down nat traversal */
    tr_natTraversalEnable( self->handle, 0 );
}

gboolean
tr_core_quiescent( TrCore * self )
{
    tr_handle_status_t * hstat;

    TR_IS_CORE( self );
    g_assert( self->quitting );

    if( self->disposed )
    {
        return TRUE;
    }

    if( 0 < tr_core_check_torrents( self ) )
    {
        return FALSE;
    }

    hstat = tr_handleStatus( self->handle );

    return TR_NAT_TRAVERSAL_DISABLED == hstat->natTraversalStatus;
}

int
tr_core_check_torrents( TrCore * self )
{
    GtkTreeIter iter;
    tr_stat_t * st;
    int         count;
    TrTorrent * tor;

    g_assert( !self->disposed && self->quitting );

    count = 0;

    if( gtk_tree_model_get_iter_first( self->model, &iter) )
    {
        do
        {
            gtk_tree_model_get( self->model, &iter, MC_TORRENT, &tor, -1 );
            st = tr_torrent_stat( tor );
            if( !( TR_STATUS_PAUSE & st->status ) )
            {
                count++;
            }
            g_object_unref( tor );
        }
        while( gtk_tree_model_iter_next( self->model, &iter ) );
    }

    count += tr_core_check_zombies( self );

    return count;
}

int
tr_core_check_zombies( TrCore * self )
{
    GList     * ii, * next;
    tr_stat_t * st;

    for( ii = g_list_first( self->zombies ); NULL != ii; ii = next )
    {
        next = ii->next;
        st = tr_torrent_stat( ii->data );
        if( TR_STATUS_PAUSE & st->status )
        {
            tr_torrent_sever( ii->data );
            g_object_unref( ii->data );
            /* XXX is this safe to do? */
            self->zombies = g_list_delete_link( self->zombies, ii );
        }
    }

    return g_list_length( self->zombies );
}

void
tr_core_save( TrCore * self )
{
    benc_val_t  state, * item;
    int         count;
    GtkTreeIter iter;
    TrTorrent * tor;
    char      * errstr;
    GList     * saved, * ii;

    TR_IS_CORE( self );

    if( gtk_tree_model_get_iter_first( self->model, &iter) )
    {
        do
        {
            count++;
        }
        while( gtk_tree_model_iter_next( self->model, &iter ) );
    }

    tr_bencInit( &state, TYPE_LIST );
    if( tr_bencListReserve( &state, count ) )
    {
        tr_core_errsig( self, TR_CORE_ERR_SAVE_STATE, "malloc failure" );
        return;
    }

    saved = NULL;
    if( gtk_tree_model_get_iter_first( self->model, &iter) )
    {
        do
        {
            item = tr_bencListAdd( &state );
            gtk_tree_model_get( self->model, &iter, MC_TORRENT, &tor, -1 );
            if( tr_torrent_get_state( tor, item ) )
            {
                saved = g_list_append( saved, tor );
            }
            else
            {
                tr_bencFree( item );
                tr_bencInitStr( item, NULL, 0, 1 );
            }
            g_object_unref( tor );
        }
        while( gtk_tree_model_iter_next( self->model, &iter ) );
    }

    errstr = NULL;
    cf_savestate( &state, &errstr );
    tr_bencFree( &state );
    if( NULL != errstr )
    {
        tr_core_errsig( self, TR_CORE_ERR_SAVE_STATE, errstr );
        g_free( errstr );
    }
    else
    {
        for( ii = saved; NULL != ii; ii = ii->next )
        {
            tr_torrent_state_saved( ii->data );
        }
    }
    if( NULL != saved )
    {
        g_list_free( saved );
    }
}

int
tr_core_load( TrCore * self, benc_val_t * state )
{
    int         ii, count;
    char      * errstr;
    TrTorrent * tor;

    TR_IS_CORE( self );

    if( TYPE_LIST != state->type )
    {
        return 0;
    }

    count = 0;
    for( ii = 0; ii < state->val.l.count; ii++ )
    {
        errstr = NULL;
        tor = tr_torrent_new_with_state( self->handle, state->val.l.vals + ii,
                                         0, &errstr );
        if( NULL == tor )
        {
            tr_core_errsig( self, TR_CORE_ERR_ADD_TORRENT, errstr );
            g_free( errstr );
        }
        else
        {
            g_assert( NULL == errstr );
            tr_core_insert( self, tor );
            count++;
        }
    }

    return count;
}

gboolean
tr_core_add_torrent( TrCore * self, const char * torrent, const char * dir,
                     guint flags )
{
    TrTorrent * tor;
    char      * errstr;

    TR_IS_CORE( self );

    errstr = NULL;
    tor = tr_torrent_new( self->handle, torrent, dir, flags, &errstr );
    if( NULL == tor )
    {
        tr_core_errsig( self, TR_CORE_ERR_ADD_TORRENT, errstr );
        g_free( errstr );
        return FALSE;
    }
    g_assert( NULL == errstr );

    tr_core_insert( self, tor );

    return TRUE;
}

void
tr_core_torrents_added( TrCore * self )
{
    TR_IS_CORE( self );

    tr_core_errsig( self, TR_CORE_ERR_NO_MORE_TORRENTS, NULL );
}

void
tr_core_delete_torrent( TrCore * self, GtkTreeIter * iter )
{
    TrTorrent * tor;
    tr_stat_t * st;

    TR_IS_CORE( self );

    gtk_tree_model_get( self->model, iter, MC_TORRENT, &tor, -1 );

    gtk_list_store_remove( GTK_LIST_STORE( self->model ), iter );
    if( TR_FLAG_SAVE & tr_torrent_info( tor )->flags )
    {
        tr_torrentRemoveSaved( tr_torrent_handle( tor ) );
    }

    st = tr_torrent_stat( tor );
    if( TR_STATUS_ACTIVE & st->status )
    {
        tr_torrentStop( tr_torrent_handle( tor ) );
        self->zombies = g_list_append( self->zombies, tor );
    }
    else
    {
        tr_torrent_sever( tor );
        g_object_unref( tor );
    }
}

void
tr_core_insert( TrCore * self, TrTorrent * tor )
{
    GtkTreeIter iter;
    tr_info_t * inf;

    gtk_list_store_append( GTK_LIST_STORE( self->model ), &iter );
    inf = tr_torrent_info( tor );
    gtk_list_store_set( GTK_LIST_STORE( self->model ), &iter,
                        MC_NAME,    inf->name,
                        MC_SIZE,    inf->totalSize,
                        MC_TORRENT, tor,
                        -1);
    g_object_unref( tor );
}

void
tr_core_update( TrCore * self )
{
    GtkTreeIter iter;
    TrTorrent * tor;
    tr_stat_t * st;

    TR_IS_CORE( self );

    if( gtk_tree_model_get_iter_first( self->model, &iter ) )
    {
        do
        {
            gtk_tree_model_get( self->model, &iter, MC_TORRENT, &tor, -1 );
            st = tr_torrent_stat( tor );
            g_object_unref( tor );

            /* XXX find out if setting the same data emits changed signal */
            gtk_list_store_set( GTK_LIST_STORE( self->model ), &iter,
                                MC_STAT,        st->status,
                                MC_ERR,         st->error,
                                MC_TERR,        st->errorString,
                                MC_PROG,        st->progress,
                                MC_DRATE,       st->rateDownload,
                                MC_URATE,       st->rateUpload,
                                MC_ETA,         st->eta,
                                MC_PEERS,       st->peersTotal,
                                MC_UPEERS,      st->peersUploading,
                                MC_DPEERS,      st->peersDownloading,
                                MC_SEED,        st->seeders,
                                MC_LEECH,       st->leechers,
                                MC_DONE,        st->completedFromTracker,
                                MC_TRACKER,     st->tracker,
                                MC_DOWN,        st->downloaded,
                                MC_UP,          st->uploaded,
                                MC_LEFT,        st->left,
                                -1 );
        }
        while( gtk_tree_model_iter_next( self->model, &iter ) );
    }

    if( !self->quitting )
    {
        tr_core_check_zombies( self );
    }
}

void
tr_core_errsig( TrCore * self, enum tr_core_err type, const char * msg )
{
    TrCoreClass * class;

    class = g_type_class_peek( TR_CORE_TYPE );
    g_signal_emit( self, class->errsig, 0, type, msg );
}
